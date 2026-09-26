/* Home Assistant integration over MQTT (FSD §13).
 *
 * Design notes worth keeping:
 *
 * ONE state topic, not one per entity. Every sensor's discovery config points
 * at the same "birdbox/<id>/state" topic and picks its own field out of the
 * JSON with a value_template. Twenty-nine entities therefore cost ONE publish a
 * minute instead of twenty-nine, which matters on a box whose link has already
 * proven able to drop to a couple of KB/s.
 *
 * Discovery messages are RETAINED, the state message is not. Retained
 * discovery is what lets HA rebuild the entities after an HA restart without
 * the box being present; a retained state would instead hand a restarting HA
 * a stale temperature and pass it off as current.
 *
 * Availability is a LAST WILL, not something we publish on the way out. A bird
 * box loses power or WiFi without warning far more often than it shuts down
 * cleanly, so "offline" has to come from the broker noticing we stopped
 * answering, not from a goodbye message we would rarely get to send. */
#include "ha.h"
#include "settings.h"
#include "version.h"
#include "camera.h"
#include "capture.h"
#include "classify.h"
#include "motion.h"
#include "storage.h"
#include "species_i18n.h"
#include "web_server.h"
#include "night.h"
#include "stats.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mqtt_client.h"

static const char *TAG = "ha";

extern uint32_t g_wifi_disconnect_count;   /* wifi.c, same source /api/status uses */

static esp_mqtt_client_handle_t s_client;
static bool     s_connected;
static char     s_err[80];
static unsigned s_pubs;
static TaskHandle_t s_task;
static volatile bool s_stop_req;   /* ha_stop() asks; ha_task() exits and self-deletes */

/* "78f1f8" — the MAC's last three bytes. Unique per board, stable across
 * reflashes and DHCP moves, and short enough to read in an entity id. */
static char s_id[16];
static char s_topic_state[48];
static char s_topic_avty[48];

/* ── The published set ───────────────────────────────────────────────────────
 * `key` is both the JSON field in the state message and the entity's object-id
 * suffix. `dev_cla`/`unit`/`stat_cla` are Home Assistant's own device class,
 * unit and state class; NULL leaves the key out of the discovery payload,
 * which is how a plain text sensor (species, version, ip) is expressed.
 * `diag` puts the entity in HA's "Diagnostic" box rather than the main card. */
typedef struct {
    const char *key;
    const char *name;
    const char *dev_cla;
    const char *unit;
    const char *stat_cla;
    bool        diag;
    bool        binary;
} ha_entity_t;

static const ha_entity_t ENTITIES[] = {
    /* key            name                     device_class      unit   state_class     diag  binary */
    { "temp",         "Temperature",           "temperature",    "°C",  "measurement",  true,  false },
    { "rssi",         "WiFi signal",           "signal_strength","dBm", "measurement",  true,  false },
    { "heap",         "Free heap",             "data_size",      "B",   "measurement",  true,  false },
    { "heap_int",     "Free internal RAM",     "data_size",      "B",   "measurement",  true,  false },
    { "heap_int_big", "Largest internal block","data_size",      "B",   "measurement",  true,  false },
    { "psram",        "Free PSRAM",            "data_size",      "B",   "measurement",  true,  false },
    { "uptime",       "Uptime",                "duration",       "s",   "measurement",  true,  false },
    { "sd_free",      "SD free",               NULL,             "MB",  "measurement",  true,  false },
    { "sd_used",      "SD used",               NULL,             "%",   "measurement",  true,  false },
    { "wifi_rec",     "WiFi reconnects",       NULL,             NULL,  "total_increasing", true, false },
    { "events",       "Capture events",        NULL,             NULL,  "total_increasing", false, false },
    { "triggers",     "Motion triggers",       NULL,             NULL,  "total_increasing", false, false },
    { "species",      "Last species",          NULL,             NULL,  NULL,           false, false },
    { "sp_conf",      "Last confidence",       NULL,             "%",   "measurement",  false, false },
    { "version",      "Firmware",              NULL,             NULL,  NULL,           true,  false },
    { "ip",           "IP address",            NULL,             NULL,  NULL,           true,  false },
    { "motion",       "Motion",                "motion",         NULL,  NULL,           false, true  },
    { "sd_ok",        "SD card",               "connectivity",   NULL,  NULL,           true,  true  },
    { "cam_ok",       "Camera",                "connectivity",   NULL,  NULL,           true,  true  },
    /* Night sleep (FSD §14). Published so the box's own dusk/dawn transitions
     * are visible and alertable in Home Assistant, with history — which is a
     * far better way to watch this feature than polling /api/night. */
    { "night",        "Night state",           NULL,             NULL,  NULL,           true,  false },
    { "night_hold",   "Night hold reason",     NULL,             NULL,  NULL,           true,  false },
    { "cam_on",       "Camera powered",        NULL,             NULL,  NULL,           true,  true  },
    { "asleep_min",   "Asleep for",            "duration",       "min", "measurement",  true,  false },
    /* The value that decides day/night since v3.12. Mean brightness is a
     * display number only — AGC holds it near 140 as the light fails. */
    { "contrast",     "Scene contrast",        NULL,             NULL,  "measurement",  true,  false },
    /* Motion triggers the classifier confidently called "no bird" — a rising
     * share against real visits is the honest measure of detector noise. */
    { "false_pos",    "False positives",       NULL,             NULL,  "total_increasing", true, false },
    /* Camera health. `problem` is the device class HA alerts on, so cam_fault
     * is the one worth a notification: auto-recovery gave up and the sensor
     * needs a real power cycle. cam_recoveries is the EARLY WARNING — a
     * climbing count means the sensor keeps stalling and being re-inited, which
     * shows up long before it gives up altogether, and nightly sleep/wake
     * cycling (§14) is new stress on exactly that path. */
    { "cam_fault",    "Camera fault",          "problem",        NULL,  NULL,           true,  true  },
    { "cam_recoveries","Camera recoveries",    NULL,             NULL,  "total_increasing", true, false },
    /* Motion cluster telemetry (§3.1). The diagnostic is a TREND, which is
     * exactly what HA history is for: `rejected` climbing while `triggers`
     * stays flat is the signature of a cluster cap set too low for the mount —
     * the box is seeing birds and discarding them as wind. Before this existed
     * that rejection left no trace at all, so a box throwing away every bird
     * looked identical to one with nothing in front of it. `cluster_cells`
     * against `cluster_cap` shows how much headroom the current mount has. */
    { "cluster_cells","Last cluster size",     NULL,             NULL,  "measurement",  true,  false },
    { "cluster_cap",  "Cluster cap",           NULL,             NULL,  "measurement",  true,  false },
    { "rejected",     "Oversized rejections",  NULL,             NULL,  "total_increasing", true, false },
    { "rejected_max", "Largest rejected",      NULL,             NULL,  "measurement",  true,  false },
};
#define ENTITY_COUNT (sizeof(ENTITIES) / sizeof(ENTITIES[0]))

/* Safe accumulating append.
 *
 * `n += snprintf(buf + n, cap - n, ...)` is a trap: snprintf returns what it
 * WOULD have written, so the moment n passes cap the expression `cap - n`
 * underflows (it is size_t) into an enormous length, and the next append
 * writes past the end of the buffer. Five of these run back to back building
 * every discovery payload, so the error compounds rather than showing up once.
 *
 * Returns the new length clamped to the buffer, and appends nothing once full.
 * Callers compare the result against cap to detect truncation. */
static int jcat(char *buf, size_t cap, int n, const char *fmt, ...)
{
    if (n < 0) n = 0;
    if ((size_t) n >= cap) return (int) cap;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + n, cap - (size_t) n, fmt, ap);
    va_end(ap);
    if (w < 0) return n;
    n += w;
    return (size_t) n >= cap ? (int) cap : n;
}

/* ── Per-species visit counters (FSD §13/§3.4) ───────────────────────────────
 * "How many Kjøttmeis this week" is the question Home Assistant is actually
 * good at, and a text sensor holding the LAST species cannot answer it: there
 * is nothing to aggregate. One monotonic counter per species can, because HA's
 * long-term statistics engine derives per-hour/day/month deltas from exactly
 * that shape — so visits-per-day needs no templating, and a utility_meter gives
 * daily/weekly cycles for free.
 *
 * COUNTS COME FROM THE VISIT LOG, NOT FROM COUNTING EVENTS AS THEY HAPPEN.
 * That is the important choice. An in-memory counter would be cheaper and would
 * be WRONG: relabelling a bird in the Gallery rewrites the log's `corrected`
 * column, and those corrections are exactly what the operator spends effort on.
 * A live counter would never see them and would drift away from the Stats tab
 * for good. Re-reading the log also makes the counters survive reboots without
 * persisting anything, which is what lets them be honest `total_increasing`
 * sensors rather than ones that silently reset.
 *
 * The log read is SD-bound (~2 s), so it runs on its own slow cadence rather
 * than with every 60 s state message — 2 s per 15 min is a 0.2 % duty cycle,
 * and species totals do not move faster than that anyway. */
#define HA_SPECIES_MAX      16    /* entities; the feeder shows ~6 in practice */
#define HA_SPECIES_REFRESH_MIN 15 /* minutes between visit-log re-reads        */

static struct {
    char     slug[28];            /* MQTT/uniq_id-safe key, from the binomial  */
    char     name[64];            /* localized display name                    */
    uint32_t n;
    bool     announced;           /* discovery config published for it yet     */
} s_sp[HA_SPECIES_MAX];
static int      s_sp_count;
static uint32_t s_false_pos;
static int64_t  s_sp_next_us;     /* 0 = due now */

/* Latin binomial -> "parus_major". Stable across UI language changes, which
 * matters because the display name is localized and switching language would
 * otherwise orphan every entity. */
static void slugify(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < n; i++) {
        unsigned char c = (unsigned char) in[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char) (c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[o++] = (char) c;
        else if (o && out[o - 1] != '_') out[o++] = '_';
    }
    while (o && out[o - 1] == '_') o--;      /* no trailing separator */
    out[o] = '\0';
    if (!o) strlcpy(out, "unknown", n);
}

static void publish_species_discovery(int i);

/* Re-read the visit log and refresh the species table. Announces any species
 * seen for the first time. */
static void species_refresh(void)
{
    stats_t *st = calloc(1, sizeof(stats_t));   /* ~2.6 kB: never on the stack */
    if (!st) { ESP_LOGW(TAG, "species refresh: no memory"); return; }
    if (stats_collect(st) == ESP_OK) {
        s_false_pos = st->false_pos;
        for (int r = 0; r < st->sp_count && r < HA_SPECIES_MAX; r++) {
            /* Key on the binomial where the row has one — a merged row can
             * span several raw common names (v2.70), and the binomial is the
             * species' real identity. */
            const char *key = st->sp_latin[r][0] ? st->sp_latin[r] : st->sp[r];
            char slug[28];
            slugify(key, slug, sizeof(slug));

            int i = 0;
            while (i < s_sp_count && strcmp(s_sp[i].slug, slug) != 0) i++;
            if (i == s_sp_count) {
                if (s_sp_count >= HA_SPECIES_MAX) continue;   /* table full */
                strlcpy(s_sp[i].slug, slug, sizeof(s_sp[i].slug));
                s_sp[i].announced = false;
                s_sp_count++;
            }
            species_localize(st->sp[r], st->sp_latin[r], g_settings.lang,
                             s_sp[i].name, sizeof(s_sp[i].name));
            s_sp[i].n = st->sp_n[r];
            if (!s_sp[i].announced && s_connected) publish_species_discovery(i);
        }
    }
    free(st);
    s_sp_next_us = esp_timer_get_time() +
                   (int64_t) HA_SPECIES_REFRESH_MIN * 60 * 1000000LL;
}

static void box_ip(char *out, size_t n)
{
    out[0] = '\0';
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK)
        snprintf(out, n, IPSTR, IP2STR(&info.ip));
}

/* The species name is the only published value that comes from outside the
 * firmware's own control (it is a localized label that has been through a CSV
 * and an online API), so it is the only one that could carry a quote or a
 * backslash and break the JSON document every other entity is reading. */
static void json_safe(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < n; i++) {
        unsigned char c = (unsigned char) in[i];
        if (c == '"' || c == '\\' || c < 0x20) continue;
        out[o++] = (char) c;
    }
    out[o] = '\0';
}

/* Home Assistant MQTT Discovery. Abbreviated keys (stat_t, val_tpl, dev_cla…)
 * are HA's own documented short forms; they keep each payload comfortably
 * inside the client's 1.5 KB buffer once the device block is included. */

/* Discovery for one species counter. total_increasing is the whole point: it
 * is what lets HA derive visits-per-day from a cumulative total. Not marked
 * diagnostic — these are the data the box exists to produce. */
static void publish_species_discovery(int i)
{
    char ip[16];
    box_ip(ip, sizeof(ip));
    char topic[128], payload[640], name_e[96];
    json_safe(s_sp[i].name, name_e, sizeof(name_e));

    snprintf(topic, sizeof(topic), "homeassistant/sensor/birdbox_%s/sp_%s/config",
             s_id, s_sp[i].slug);
    int n = snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\",\"uniq_id\":\"birdbox_%s_sp_%s\","
        "\"stat_t\":\"%s\",\"avty_t\":\"%s\","
        "\"val_tpl\":\"{{value_json.sp_%s}}\","
        "\"stat_cla\":\"total_increasing\",\"unit_of_meas\":\"visits\","
        "\"ic\":\"mdi:bird\"",
        name_e, s_id, s_sp[i].slug, s_topic_state, s_topic_avty, s_sp[i].slug);
    n = jcat(payload, sizeof(payload), n,
        ",\"dev\":{\"ids\":[\"birdbox_%s\"],\"name\":\"%s\",\"mf\":\"BirdBox\","
        "\"mdl\":\"%s\",\"sw\":\"%s\",\"cu\":\"http://%s/\"}}",
        s_id, FIRMWARE_NAME, camera_caps()->name[0] ? camera_caps()->name : "ESP32",
        FIRMWARE_VERSION, ip);
    if ((size_t) n >= sizeof(payload)) {
        ESP_LOGE(TAG, "species discovery for '%s' TRUNCATED — entity will not appear",
                 s_sp[i].slug);
        return;
    }
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);
    s_sp[i].announced = true;
    ESP_LOGI(TAG, "announced species entity sp_%s (%s, n=%lu)",
             s_sp[i].slug, s_sp[i].name, (unsigned long) s_sp[i].n);
}
static void publish_discovery(void)
{
    char ip[16];
    box_ip(ip, sizeof(ip));

    char topic[128], payload[768];
    for (size_t i = 0; i < ENTITY_COUNT; i++) {
        const ha_entity_t *e = &ENTITIES[i];
        snprintf(topic, sizeof(topic), "homeassistant/%s/birdbox_%s/%s/config",
                 e->binary ? "binary_sensor" : "sensor", s_id, e->key);

        int n = snprintf(payload, sizeof(payload),
            "{\"name\":\"%s\",\"uniq_id\":\"birdbox_%s_%s\","
            "\"stat_t\":\"%s\",\"avty_t\":\"%s\","
            "\"val_tpl\":\"{{value_json.%s}}\"",
            e->name, s_id, e->key, s_topic_state, s_topic_avty, e->key);
        if (e->dev_cla)  n = jcat(payload, sizeof(payload), n,
                                       ",\"dev_cla\":\"%s\"", e->dev_cla);
        if (e->unit)     n = jcat(payload, sizeof(payload), n,
                                       ",\"unit_of_meas\":\"%s\"", e->unit);
        if (e->stat_cla) n = jcat(payload, sizeof(payload), n,
                                       ",\"stat_cla\":\"%s\"", e->stat_cla);
        if (e->diag)     n = jcat(payload, sizeof(payload), n,
                                       ",\"ent_cat\":\"diagnostic\"");
        /* The device block is what makes all twenty-nine entities collapse into a
         * single "BirdBox" device in HA instead of twenty-nine loose ones. */
        n = jcat(payload, sizeof(payload), n,
            ",\"dev\":{\"ids\":[\"birdbox_%s\"],\"name\":\"%s\",\"mf\":\"BirdBox\","
            "\"mdl\":\"%s\",\"sw\":\"%s\",\"cu\":\"http://%s/\"}}",
            s_id, FIRMWARE_NAME, camera_caps()->name[0] ? camera_caps()->name : "ESP32",
            FIRMWARE_VERSION, ip);

        /* A clipped discovery config is invalid JSON, and HA answers by simply
         * not creating that entity — silently, with nothing on the box to say
         * why one sensor is missing while the rest appeared. */
        if ((size_t) n >= sizeof(payload))
            ESP_LOGE(TAG, "discovery payload for '%s' TRUNCATED (%u bytes) — "
                          "entity will not appear in Home Assistant",
                     e->key, (unsigned) sizeof(payload));
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);
    }
    for (int i = 0; i < s_sp_count; i++) { s_sp[i].announced = false; publish_species_discovery(i); }
    ESP_LOGI(TAG, "published %u discovery configs + %d species",
             (unsigned) ENTITY_COUNT, s_sp_count);
}

static void publish_state(void)
{
    uint64_t sd_total = 0, sd_free = 0;
    storage_get_info(&sd_total, &sd_free);
    unsigned sd_used_pct = sd_total ? (unsigned) (100 - (sd_free * 100 / sd_total)) : 0;

    wifi_ap_record_t ap = {0};
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

    char species[80], safe[80];
    species_localize(classify_last_species(), classify_last_latin(),
                     g_settings.lang, species, sizeof(species));
    json_safe(classify_last_species()[0] ? species : "none", safe, sizeof(safe));

    char ip[16];
    box_ip(ip, sizeof(ip));

    float t = web_soc_temp_c();

    /* Grew with the four night-sleep fields (v3.07) — a truncated state message
     * is silently invalid JSON and every entity reading it goes unknown. */
    char buf[1600];   /* + per-species counters (v3.14) */
    int n = snprintf(buf, sizeof(buf),
        "{\"rssi\":%d,\"heap\":%lu,\"heap_int\":%lu,\"heap_int_big\":%lu,"
        "\"psram\":%lu,\"uptime\":%lld,\"sd_free\":%llu,\"sd_used\":%u,"
        "\"wifi_rec\":%lu,\"events\":%lu,\"triggers\":%lu,"
        "\"species\":\"%s\",\"sp_conf\":%u,\"version\":\"%s\",\"ip\":\"%s\","
        "\"motion\":\"%s\",\"sd_ok\":\"%s\",\"cam_ok\":\"%s\","
        "\"night\":\"%s\",\"night_hold\":\"%s\",\"cam_on\":\"%s\",\"asleep_min\":%d,"
        "\"cam_fault\":\"%s\",\"cam_recoveries\":%lu,"
        "\"contrast\":%d,\"cluster_cells\":%d,\"cluster_cap\":%d,"
        "\"rejected\":%lu,\"rejected_max\":%d",
        rssi,
        (unsigned long) esp_get_free_heap_size(),
        (unsigned long) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned long) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned long) heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (long long) (esp_timer_get_time() / 1000000),
        sd_free / (1024 * 1024), sd_used_pct,
        (unsigned long) g_wifi_disconnect_count,
        (unsigned long) capture_event_count(),
        (unsigned long) motion_trigger_count(),
        safe, (unsigned) classify_last_confidence(),
        FIRMWARE_VERSION, ip,
        motion_active() ? "ON" : "OFF",
        storage_sd_present() ? "ON" : "OFF",
        /* A camera that is deliberately asleep is NOT a fault, and reporting it
         * as one would light up "Camera disconnected" in HA every single night
         * (FSD §14). cam_ok now means "no fault"; cam_on is the separate,
         * honest answer to "is the sensor powered right now". */
        (!camera_fault() && (camera_available() || camera_asleep())) ? "ON" : "OFF",
        night_state_str(),
        night_hold_reason()[0] ? night_hold_reason() : "none",
        camera_asleep() ? "OFF" : "ON",
        night_asleep_s() / 60,
        camera_fault() ? "ON" : "OFF",          /* ON = problem, HA alerts on it */
        (unsigned long) camera_recovery_count(),
        motion_ambient_contrast(),
        motion_cluster_cells(), motion_cluster_cap(),
        (unsigned long) motion_reject_count(), motion_reject_cells());
    /* An unavailable on-die sensor reports -1000; publishing that would draw a
     * cliff through the HA history graph. Omit the field instead — HA renders a
     * missing value as "unknown", which is what it is. */
    if (t > -100.0f) n = jcat(buf, sizeof(buf), n, ",\"temp\":%.1f", t);
    n = jcat(buf, sizeof(buf), n, "}");

    /* Per-species visit counts and the confirmed false-positive total. Appended
     * with jcat so a long species list can never run past the buffer; if it
     * does, the truncation check below catches it and the message is dropped
     * rather than published malformed. */
    n = jcat(buf, sizeof(buf), n, ",\"false_pos\":%lu", (unsigned long) s_false_pos);
    for (int i = 0; i < s_sp_count; i++)
        n = jcat(buf, sizeof(buf), n, ",\"sp_%s\":%lu",
                 s_sp[i].slug, (unsigned long) s_sp[i].n);

    /* One malformed state message takes EVERY entity to "unknown" at once,
     * because they all read their value out of this single document. */
    if ((size_t) n >= sizeof(buf)) {
        ESP_LOGE(TAG, "state message TRUNCATED (%u bytes) — all %u entities "
                      "will read unknown; grow the buffer",
                 (unsigned) sizeof(buf), (unsigned) ENTITY_COUNT);
        return;
    }
    if (esp_mqtt_client_publish(s_client, s_topic_state, buf, 0, 0, 0) >= 0) s_pubs++;
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg; (void) base;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t) data;
    switch ((esp_mqtt_event_id_t) id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        s_err[0] = '\0';
        ESP_LOGI(TAG, "connected to broker %s:%u",
                 g_settings.ha_host, (unsigned) g_settings.ha_port);
        /* Order matters: availability first, then discovery. HA drops a state
         * message for an entity it has not been told about yet, and marks an
         * entity unavailable until the availability topic says otherwise. */
        esp_mqtt_client_publish(s_client, s_topic_avty, "online", 0, 1, 1);
        publish_discovery();
        publish_state();
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) ESP_LOGW(TAG, "disconnected from broker");
        s_connected = false;
        break;
    case MQTT_EVENT_ERROR:
        s_connected = false;
        if (ev && ev->error_handle) {
            if (ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
                snprintf(s_err, sizeof(s_err), "cannot reach broker (errno %d)",
                         ev->error_handle->esp_transport_sock_errno);
            else if (ev->error_handle->connect_return_code != MQTT_CONNECTION_ACCEPTED)
                snprintf(s_err, sizeof(s_err), "broker refused connection (code %d)",
                         (int) ev->error_handle->connect_return_code);
            else
                snprintf(s_err, sizeof(s_err), "MQTT error");
        }
        ESP_LOGW(TAG, "%s", s_err);
        break;
    default:
        break;
    }
}

static void ha_task(void *arg)
{
    (void) arg;
    for (;;) {
        /* Sleep in 1 s slices rather than one long delay, so a stop request is
         * honoured within a second instead of up to a full publish interval. */
        for (int i = 0; i < HA_PUBLISH_INTERVAL_S && !s_stop_req; i++)
            vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_stop_req) break;
        /* Slow, SD-bound: re-read the visit log on its own cadence, never with
         * every state message. Also picks up Gallery relabels, which a live
         * counter would miss entirely. */
        if (s_connected && esp_timer_get_time() >= s_sp_next_us) species_refresh();
        if (s_client && s_connected) publish_state();
    }
    /* Exit at a loop boundary and delete OURSELVES. ha_stop() used to call
     * vTaskDelete() on this task from outside, which could kill it anywhere —
     * including inside esp_mqtt_client_publish() holding an esp-mqtt internal
     * mutex, after which destroying the client can deadlock or corrupt. That
     * ran on EVERY settings save (ha_apply), so it was a narrow window on a
     * frequently-travelled path. */
    s_task = NULL;
    vTaskDelete(NULL);
}

static void ha_stop(void)
{
    /* Ask, then wait — never delete the publish task from outside. It clears
     * s_task and deletes itself once it reaches a loop boundary, so the client
     * below is only ever destroyed with no publish in flight. */
    if (s_task) {
        s_stop_req = true;
        for (int i = 0; i < 60 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(50));
        if (s_task) ESP_LOGW(TAG, "publish task did not exit in 3 s — "
                                  "destroying the client anyway");
        s_stop_req = false;
    }
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
}

esp_err_t ha_start(void)
{
    if (!g_settings.ha_enabled || !g_settings.ha_host[0]) {
        ESP_LOGI(TAG, "Home Assistant reporting disabled");
        return ESP_OK;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_id, sizeof(s_id), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_topic_state, sizeof(s_topic_state), "birdbox/%s/state", s_id);
    snprintf(s_topic_avty,  sizeof(s_topic_avty),  "birdbox/%s/status", s_id);

    static char uri[96];
    snprintf(uri, sizeof(uri), "mqtt://%s:%u",
             g_settings.ha_host, (unsigned) g_settings.ha_port);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri            = uri,
        .credentials.client_id         = s_id,
        /* An empty username means an anonymous broker, and esp-mqtt wants NULL
         * for that — passing "" makes it send a zero-length username, which a
         * broker configured for anonymous access rejects. */
        .credentials.username          = g_settings.ha_user[0] ? g_settings.ha_user : NULL,
        .credentials.authentication.password =
                                         g_settings.ha_pass[0] ? g_settings.ha_pass : NULL,
        .session.last_will.topic       = s_topic_avty,
        .session.last_will.msg         = "offline",
        .session.last_will.msg_len     = 7,
        .session.last_will.qos         = 1,
        .session.last_will.retain      = 1,
        .session.keepalive             = 60,
        .network.reconnect_timeout_ms  = 10000,
        .network.disable_auto_reconnect = false,
        /* Sized for the largest discovery payload plus its device block. The
         * default 1024 would silently truncate one. */
        .buffer.size                   = 1536,
        .task.stack_size               = 4096,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        snprintf(s_err, sizeof(s_err), "client init failed");
        ESP_LOGE(TAG, "%s", s_err);
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        snprintf(s_err, sizeof(s_err), "start failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_err);
        ha_stop();
        return err;
    }
    xTaskCreate(ha_task, "ha", 6144, NULL, 2, &s_task);   /* stats_collect below it */
    ESP_LOGI(TAG, "Home Assistant reporting to %s every %d s", uri, HA_PUBLISH_INTERVAL_S);
    return ESP_OK;
}

void ha_apply(void)
{
    ha_stop();
    s_pubs = 0;
    s_err[0] = '\0';
    ha_start();
}

bool        ha_enabled(void)       { return g_settings.ha_enabled && g_settings.ha_host[0]; }
bool        ha_connected(void)     { return s_connected; }
const char *ha_last_error(void)    { return s_err; }
unsigned    ha_publish_count(void) { return s_pubs; }
