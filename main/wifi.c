/* WiFi provisioning & connection (FSD §4), ported from the RemoteStart
 * project's proven flow:
 *  - no stored credentials → SoftAP config portal (BirdBox-Config), scan +
 *    save via web_server.c, then reboot
 *  - stored credentials → STA connect, 15 s / 5 retries at boot; on failure
 *    the portal reopens in APSTA mode but keeps retrying the stored network
 *    every 60 s in the background (FSD §4.4 — a nest box at the edge of
 *    range must not strand itself in portal mode)
 *  - once connected at least once, in-service disconnects retry forever and
 *    never reopen the portal (RemoteStart v1.25 lesson)
 *  - WIFI_PS_NONE + max TX power (v1.32 lesson: modem-sleep latency ruins
 *    HTTP, and a camera stream is even more sensitive than a dashboard)
 *  - boot button (GPIO0) held ≥ 5 s at power-up erases stored credentials */
#include "wifi.h"
#include "version.h"
#include "settings.h"
#include "web_server.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "mdns.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_RETRY           5
#define BOOT_BTN_GPIO       GPIO_NUM_0
#define BOOT_BTN_HOLD_MS    5000
#define PORTAL_RETRY_MS     60000
/* Portal credential verification (§4.4). PORTAL_VERIFY_MS: how long to wait
 * for the submitted network to associate + get a DHCP lease. Then, because a
 * single-radio ESP32 in APSTA must move the SoftAP to the STA's channel when
 * it associates — briefly kicking the phone off BirdBox-Config — we do NOT
 * reboot on a blind timer. We wait until the setup page has actually fetched
 * the acquired IP (the phone auto-rejoins the AP, now co-channel with the STA,
 * within a few seconds), capped by PORTAL_SEEN_MS in case it never comes back,
 * then leave the IP on screen for PORTAL_SHOW_MS before rebooting. */
#define PORTAL_VERIFY_MS    25000
#define PORTAL_SHOW_MS      10000
#define PORTAL_SEEN_MS      60000

static EventGroupHandle_t s_wifi_eg;
static int                s_retry_count = 0;
static bool               s_wifi_ever_connected = false;
static bool               s_connected = false;
static bool               s_portal_mode = false;
static bool               s_have_stored_creds = false;
static bool               s_sntp_started = false;

/* Portal verify state (§4.4): PENDING while the submitted creds are being
 * tested with the SoftAP still up, CONNECTED (with s_portal_ip filled) once a
 * DHCP lease lands, FAILED on timeout. Read by GET /api/portal-status. */
static volatile wifi_portal_state_t s_portal_state = WIFI_PORTAL_IDLE;
static char               s_portal_ip[16] = {0};
/* Set once the setup page has fetched the CONNECTED status (i.e. the phone
 * rejoined the AP and actually saw the IP) — the portal loop waits for this
 * before rebooting so the address is never shown to nobody. */
static volatile bool      s_portal_ip_seen = false;

/* Up to two stored networks (FSD §4.7). s_nets[0] = primary, s_nets[1] =
 * alternative ("alt1"); ssid is "" when a slot is unconfigured. s_net_count is
 * the number of usable (non-empty, contiguous from 0) networks; s_cur_net is
 * the one currently configured on the STA interface. */
#define WIFI_MAX_NETS 2
static struct { char ssid[64]; char pass[64]; } s_nets[WIFI_MAX_NETS];
static int  s_net_count = 0;
static int  s_cur_net   = 0;

volatile bool     g_wifi_save_requested = false;
volatile int      g_new_slot = 0;
char              g_new_ssid[64] = {0};
char              g_new_pass[64] = {0};
volatile uint32_t g_wifi_disconnect_count = 0;
volatile int64_t  g_wifi_last_disc_ts_us  = 0;

bool          g_ipcfg_static = false;
char          g_ipcfg_ip[16]   = {0};
char          g_ipcfg_mask[16] = {0};
char          g_ipcfg_gw[16]   = {0};
char          g_ipcfg_dns[16]  = {0};
volatile bool g_ipcfg_save_requested = false;

/* ── NVS helpers ──────────────────────────────────────────────────────────
 * Slot 0 (primary) uses keys "ssid"/"pass" — unchanged from before, so an
 * existing single-network config still loads. Slot 1 (alt1) uses "ssid2"/
 * "pass2". */
static const char *ssid_key(int slot) { return slot ? "ssid2" : "ssid"; }
static const char *pass_key(int slot) { return slot ? "pass2" : "pass"; }

static esp_err_t nvs_load_wifi(int slot, char *ssid, char *pass, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t sl = len, pl = len;
    err  = nvs_get_str(h, ssid_key(slot), ssid, &sl);
    err |= nvs_get_str(h, pass_key(slot), pass, &pl);
    nvs_close(h);
    return err;
}

static void nvs_save_wifi(int slot, const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (slot == WIFI_SLOT_ALT && (!ssid || ssid[0] == '\0')) {
        nvs_erase_key(h, ssid_key(slot));    /* blank alt SSID removes it */
        nvs_erase_key(h, pass_key(slot));
        ESP_LOGI(TAG, "alternative WiFi network removed");
    } else {
        nvs_set_str(h, ssid_key(slot), ssid);
        nvs_set_str(h, pass_key(slot), pass);
        ESP_LOGI(TAG, "WiFi credentials saved (slot %d)", slot);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_erase_wifi(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "pass");
        nvs_erase_key(h, "ssid2");    /* alt1 too */
        nvs_erase_key(h, "pass2");
        /* IP config too — the boot-button reset is also the documented
         * recovery from an unreachable static IP (FSD §4.5) */
        nvs_erase_key(h, "ipmode");
        nvs_erase_key(h, "ip");
        nvs_erase_key(h, "mask");
        nvs_erase_key(h, "gw");
        nvs_erase_key(h, "dns");
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "WiFi credentials + IP config erased");
    }
}

/* Load both slots into s_nets. s_nets[0]/[1].ssid are always valid strings
 * ("" if unset) so the WiFi tab can show them; s_net_count counts usable
 * networks (the alt is only usable when a primary exists). */
static void load_stored_nets(void)
{
    memset(s_nets, 0, sizeof(s_nets));
    s_net_count = 0;
    for (int i = 0; i < WIFI_MAX_NETS; i++) {
        char ssid[64] = {0}, pass[64] = {0};
        if (nvs_load_wifi(i, ssid, pass, sizeof(ssid)) == ESP_OK && ssid[0]) {
            strlcpy(s_nets[i].ssid, ssid, sizeof(s_nets[i].ssid));
            strlcpy(s_nets[i].pass, pass, sizeof(s_nets[i].pass));
        }
    }
    /* Usable count is contiguous from slot 0: no primary ⇒ nothing usable. */
    if (s_nets[0].ssid[0]) s_net_count = s_nets[1].ssid[0] ? 2 : 1;
}

static void set_sta_config(int idx)
{
    wifi_config_t sta = {0};
    strlcpy((char *) sta.sta.ssid,     s_nets[idx].ssid, sizeof(sta.sta.ssid));
    strlcpy((char *) sta.sta.password, s_nets[idx].pass, sizeof(sta.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &sta);
}

const char *wifi_configured_ssid(int slot)
{
    return (slot >= 0 && slot < WIFI_MAX_NETS) ? s_nets[slot].ssid : "";
}

/* ── IP configuration (FSD §4.5) ────────────────────────────────────────── */
static void nvs_load_ipcfg(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t mode = 0;
    nvs_get_u8(h, "ipmode", &mode);
    g_ipcfg_static = (mode == 1);
    size_t l;
    l = sizeof(g_ipcfg_ip);   if (nvs_get_str(h, "ip",   g_ipcfg_ip,   &l) != ESP_OK) g_ipcfg_ip[0]   = '\0';
    l = sizeof(g_ipcfg_mask); if (nvs_get_str(h, "mask", g_ipcfg_mask, &l) != ESP_OK) g_ipcfg_mask[0] = '\0';
    l = sizeof(g_ipcfg_gw);   if (nvs_get_str(h, "gw",   g_ipcfg_gw,   &l) != ESP_OK) g_ipcfg_gw[0]   = '\0';
    l = sizeof(g_ipcfg_dns);  if (nvs_get_str(h, "dns",  g_ipcfg_dns,  &l) != ESP_OK) g_ipcfg_dns[0]  = '\0';
    nvs_close(h);
}

static void nvs_save_ipcfg(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "ipmode", g_ipcfg_static ? 1 : 0);
        nvs_set_str(h, "ip",   g_ipcfg_ip);
        nvs_set_str(h, "mask", g_ipcfg_mask);
        nvs_set_str(h, "gw",   g_ipcfg_gw);
        nvs_set_str(h, "dns",  g_ipcfg_dns);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "IP config saved (%s)", g_ipcfg_static ? "static" : "DHCP");
    }
}

/* Reset IP config to DHCP. Provisioning through the SoftAP portal almost
 * always means the box has moved to a new location on a new subnet, where a
 * static IP saved for the old network would strand it — it associates but is
 * unreachable at an out-of-subnet address, and (having "GOT_IP" immediately
 * with a static address) never falls back to the portal. So a portal
 * reprovision clears the stored static and reverts to DHCP (FSD §4.5). The
 * in-service WiFi tab is left alone — it has its own explicit IP-config
 * controls and its network changes stay on the reachable subnet. */
static void nvs_reset_ipcfg_dhcp(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "ipmode");
        nvs_erase_key(h, "ip");
        nvs_erase_key(h, "mask");
        nvs_erase_key(h, "gw");
        nvs_erase_key(h, "dns");
        nvs_commit(h);
        nvs_close(h);
    }
    g_ipcfg_static = false;
    g_ipcfg_ip[0] = g_ipcfg_mask[0] = g_ipcfg_gw[0] = g_ipcfg_dns[0] = '\0';
    ESP_LOGI(TAG, "portal reprovision — IP config reset to DHCP");
}

/* Applies a saved static IP to the STA netif before esp_wifi_start(); a
 * no-op (DHCP keeps running) in DHCP mode or when the saved ip/mask are
 * missing/invalid — an invalid config must degrade to DHCP, not brick the
 * device (RemoteStart v1.37). */
static void apply_static_ip(esp_netif_t *sta_netif)
{
    if (!g_ipcfg_static || !g_ipcfg_ip[0] || !g_ipcfg_mask[0]) return;

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_str_to_ip4(g_ipcfg_ip,   &ip_info.ip)      != ESP_OK ||
        esp_netif_str_to_ip4(g_ipcfg_mask, &ip_info.netmask) != ESP_OK) {
        ESP_LOGW(TAG, "Saved static IP config invalid — falling back to DHCP");
        return;
    }
    if (g_ipcfg_gw[0]) esp_netif_str_to_ip4(g_ipcfg_gw, &ip_info.gw);

    esp_netif_dhcpc_stop(sta_netif);
    esp_netif_set_ip_info(sta_netif, &ip_info);
    if (g_ipcfg_dns[0]) {
        esp_netif_dns_info_t dns_info = {0};
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        if (esp_netif_str_to_ip4(g_ipcfg_dns, &dns_info.ip.u_addr.ip4) == ESP_OK)
            esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    }
    ESP_LOGI(TAG, "Static IP applied: %s / %s gw %s", g_ipcfg_ip, g_ipcfg_mask,
             g_ipcfg_gw[0] ? g_ipcfg_gw : "-");
}

/* ── SNTP (FSD §3.4/§5) ──────────────────────────────────────────────────
 * Started once on first successful connect; the Settings tab can switch
 * servers afterward via wifi_restart_sntp() without a reboot. */

/* Fires on every completed sync, not just the first — harmless (the
 * web_server flag it clears is idempotent) and keeps clockSrc honest if the
 * browser-time fallback (POST /api/time) ever re-latches "manual" after an
 * earlier successful sync, e.g. a page load that raced a transient SNTP
 * lookup failure. */
static void on_sntp_sync(struct timeval *tv)
{
    (void) tv;
    web_server_note_ntp_sync();
}

static void start_sntp(void)
{
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(g_settings.ntp_server);
    sntp_cfg.sync_cb = on_sntp_sync;
    esp_netif_sntp_init(&sntp_cfg);
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started (%s)", g_settings.ntp_server);
}

/* mDNS responder (FSD §4.8): the box answers to http://birdbox.local/ so the
 * UI and OTA don't depend on knowing the current DHCP address (the unit has
 * changed subnets before). Best-effort — a failure here must never block
 * bringup; the IP path keeps working regardless. Note mDNS is same-subnet by
 * design; clients behind another subnet still use the IP unless the router
 * reflects multicast. */
static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set(FIRMWARE_NAME " camera");
    /* Advertise the web UI so service browsers (Bonjour etc.) list it too. */
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://%s.local/", MDNS_HOSTNAME);
}

void wifi_restart_sntp(void)
{
    if (!s_connected) return;   /* applies from g_settings on the next connect anyway */
    if (s_sntp_started) esp_netif_sntp_deinit();
    start_sntp();
}

/* ── Boot-button credential reset (FSD §4.6) ────────────────────────────── */
static void check_credential_reset(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    if (gpio_get_level(BOOT_BTN_GPIO) != 0) return;

    ESP_LOGW(TAG, "Boot button held — keep holding %d s to erase WiFi credentials",
             BOOT_BTN_HOLD_MS / 1000);
    for (int held = 0; held < BOOT_BTN_HOLD_MS; held += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(BOOT_BTN_GPIO) != 0) return;   /* released early */
    }
    nvs_erase_wifi();
}

/* ── Event handler ──────────────────────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            /* In portal (APSTA) mode the STA config still holds the failed
             * credentials — the portal loop owns retry timing there. */
            if (!s_portal_mode) esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_connected = false;
            g_wifi_disconnect_count++;
            g_wifi_last_disc_ts_us = esp_timer_get_time();
            if (s_portal_mode) return;   /* portal loop retries on its own schedule */
            /* Once we've connected at least once, retry forever — MAX_RETRY only
             * governs the initial boot decision to fall back to the portal. */
            if (s_wifi_ever_connected || s_retry_count < MAX_RETRY) {
                s_retry_count++;
                /* In-service failover (FSD §4.7): after MAX_RETRY consecutive
                 * failures on the current AP, switch to the other stored network
                 * so a box that roams between two coverage areas finds whichever
                 * is present. (Boot-time network switching is driven by the loop
                 * in wifi_start, not here.) */
                if (s_wifi_ever_connected && s_net_count > 1 &&
                    s_retry_count % MAX_RETRY == 0) {
                    s_cur_net = (s_cur_net + 1) % s_net_count;
                    set_sta_config(s_cur_net);
                    ESP_LOGI(TAG, "failover: switching to network %d (%s)",
                             s_cur_net, s_nets[s_cur_net].ssid);
                }
                esp_wifi_connect();
                ESP_LOGI(TAG, "WiFi retry %d/%d", s_retry_count, MAX_RETRY);
            } else {
                xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *) data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_count = 0;
        s_connected = true;
        s_wifi_ever_connected = true;
        if (s_portal_mode) {
            if (s_portal_state == WIFI_PORTAL_PENDING) {
                /* User submitted creds through the portal — capture the
                 * DHCP-acquired IP and hand it to the setup page instead of
                 * rebooting immediately (§4.4). start_config_portal persists
                 * the creds and reboots after a short display window. */
                snprintf(s_portal_ip, sizeof(s_portal_ip), IPSTR,
                         IP2STR(&ev->ip_info.ip));
                s_portal_state = WIFI_PORTAL_CONNECTED;
                ESP_LOGI(TAG, "Portal verify connected — IP %s", s_portal_ip);
                return;
            }
            /* Stored network came back into reach while the portal was open —
             * reboot into a clean normal-mode connect (FSD §4.4). */
            ESP_LOGI(TAG, "Connected during portal mode — rebooting to apply");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

/* ── Save watcher (normal operation) ────────────────────────────────────────
 * The first-boot portal has its own blocking wait loop below; this task is
 * what makes the always-available WiFi tab (FSD §4.5) genuinely functional
 * during normal operation (RemoteStart v1.37 lesson: a save nobody consumes
 * silently does nothing). */
static void config_apply_task(void *arg)
{
    for (;;) {
        if (g_wifi_save_requested) {
            nvs_save_wifi(g_new_slot, g_new_ssid, g_new_pass);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
        if (g_ipcfg_save_requested) {
            nvs_save_ipcfg();
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ── SoftAP config portal ───────────────────────────────────────────────── */
static void start_config_portal(void)
{
    ESP_LOGI(TAG, "Starting config portal AP: %s (pw: %s)", WIFI_AP_SSID, WIFI_AP_PASSWORD);

    esp_netif_create_default_wifi_ap();
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = WIFI_AP_SSID,
            .password       = WIFI_AP_PASSWORD,
            .ssid_len       = strlen(WIFI_AP_SSID),
            .channel        = 1,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .max_connection = 4,
        }
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    web_server_start();   /* serves the setup page, /api/scan and /wifi-save */

    /* Outer loop so a failed verify (e.g. mistyped password) drops back to
     * waiting for another submission rather than bricking the portal. */
    for (;;) {
        /* Block until the user submits credentials; meanwhile, if stored
         * credentials exist (boot-time connect failed, §4.4), keep retrying
         * them in the background — the router may just have been down. */
        int64_t last_retry_us = esp_timer_get_time();
        while (!g_wifi_save_requested) {
            if (s_have_stored_creds &&
                esp_timer_get_time() - last_retry_us > (int64_t) PORTAL_RETRY_MS * 1000) {
                last_retry_us = esp_timer_get_time();
                /* Alternate stored networks each cycle so the portal keeps
                 * trying both the primary and alt1 (FSD §4.7). */
                if (s_net_count > 1) {
                    s_cur_net = (s_cur_net + 1) % s_net_count;
                    set_sta_config(s_cur_net);
                }
                ESP_LOGI(TAG, "Portal open — retrying %s in background", s_nets[s_cur_net].ssid);
                esp_wifi_connect();   /* GOT_IP handler reboots on success */
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        g_wifi_save_requested = false;   /* consume the submission */

        /* Verify the submitted creds with the SoftAP still up so we can report
         * the DHCP IP to the setup page (§4.4). Force DHCP first: a stale
         * static from a previous location would otherwise associate and report
         * a wrong, out-of-subnet address. */
        esp_wifi_disconnect();
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta) esp_netif_dhcpc_start(sta);   /* ALREADY_STARTED is harmless */
        wifi_config_t sta_cfg = {0};
        strlcpy((char *) sta_cfg.sta.ssid,     g_new_ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *) sta_cfg.sta.password, g_new_pass, sizeof(sta_cfg.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        s_portal_ip[0] = '\0';
        s_portal_ip_seen = false;
        s_portal_state = WIFI_PORTAL_PENDING;
        ESP_LOGI(TAG, "Portal verify — connecting to %s", g_new_ssid);
        esp_wifi_connect();

        int64_t t0 = esp_timer_get_time();
        while (s_portal_state == WIFI_PORTAL_PENDING &&
               esp_timer_get_time() - t0 < (int64_t) PORTAL_VERIFY_MS * 1000) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        if (s_portal_state == WIFI_PORTAL_CONNECTED) {
            nvs_save_wifi(g_new_slot, g_new_ssid, g_new_pass);
            nvs_reset_ipcfg_dhcp();   /* new location ⇒ drop stale static IP (§4.5) */
            /* Associating with the STA's AP hopped our SoftAP to its channel,
             * dropping the phone; wait for it to rejoin and actually fetch the
             * IP (s_portal_ip_seen) before starting the reboot countdown, so
             * the address is never shown to a disconnected phone. Capped in
             * case the phone never returns. */
            ESP_LOGI(TAG, "Credentials verified (IP %s) — awaiting page fetch", s_portal_ip);
            int64_t seen0 = esp_timer_get_time();
            while (!s_portal_ip_seen &&
                   esp_timer_get_time() - seen0 < (int64_t) PORTAL_SEEN_MS * 1000) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            ESP_LOGI(TAG, "%s — rebooting onto the LAN in %d s",
                     s_portal_ip_seen ? "Page showed the IP" : "Page never returned",
                     PORTAL_SHOW_MS / 1000);
            vTaskDelay(pdMS_TO_TICKS(PORTAL_SHOW_MS));   /* countdown window */
            esp_restart();
        }

        /* Timed out — surface the failure to the page and loop back so the
         * user can re-enter the password. */
        s_portal_state = WIFI_PORTAL_FAILED;
        esp_wifi_disconnect();
        ESP_LOGW(TAG, "Portal verify failed for %s — awaiting retry", g_new_ssid);
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */
esp_err_t wifi_start(void)
{
    check_credential_reset();

    s_wifi_eg = xEventGroupCreate();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    /* DHCP hostname (shows in router client lists as "birdbox") — must be set
     * before the DHCP client starts, i.e. before esp_wifi_start below. */
    esp_netif_set_hostname(sta_netif, MDNS_HOSTNAME);
    nvs_load_ipcfg();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    load_stored_nets();
    if (s_net_count > 0) {
        s_have_stored_creds = true;
        s_cur_net = WIFI_SLOT_PRIMARY;
        set_sta_config(WIFI_SLOT_PRIMARY);
        apply_static_ip(sta_netif);         /* shared IP setting for both nets */
        ESP_ERROR_CHECK(esp_wifi_start());  /* STA_START connects to slot 0    */

        /* Modem-sleep power save adds DTIM-interval latency to every HTTP
         * exchange — deadly for an MJPEG stream. Device is mains-powered. */
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
        /* Best-effort: the real ceiling is module-dependent, a rejected value
         * just leaves the IDF default. */
        esp_err_t txp = esp_wifi_set_max_tx_power(84);
        if (txp != ESP_OK) ESP_LOGW(TAG, "set_max_tx_power: %s", esp_err_to_name(txp));

        /* Try each stored network in turn — primary, then alt1 (FSD §4.7).
         * The shared static-IP/DHCP setting applies to whichever connects; a
         * single static address is only valid if both APs share a subnet,
         * otherwise use DHCP. */
        bool connected = false;
        for (int i = 0; i < s_net_count; i++) {
            if (i > 0) {                    /* slot 0 already connecting above  */
                s_cur_net = i;
                set_sta_config(i);
                s_retry_count = 0;
                xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
                ESP_LOGI(TAG, "primary unreachable — trying alternative %s", s_nets[i].ssid);
                esp_wifi_connect();
            }
            EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
                pdMS_TO_TICKS(15000));
            if (bits & WIFI_CONNECTED_BIT) { connected = true; break; }
        }

        if (connected) {
            ESP_LOGI(TAG, "WiFi connected to %s", s_nets[s_cur_net].ssid);
            /* Clock: SNTP + timezone (FSD §3.4). Best-effort — events before
             * first sync get placeholder timestamps. */
            start_sntp();
            setenv("TZ", g_settings.timezone, 1);
            tzset();
            start_mdns();   /* http://birdbox.local/ (FSD §4.8) */
            xTaskCreate(config_apply_task, "cfg_apply", 4096, NULL, 3, NULL);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "all networks failed, opening portal (will keep retrying)");
        esp_wifi_stop();
    }

    s_portal_mode = true;
    start_config_portal();
    return ESP_OK;   /* unreachable — portal reboots after save */
}

bool wifi_is_connected(void)   { return s_connected; }
bool wifi_in_portal_mode(void) { return s_portal_mode; }

wifi_portal_state_t wifi_portal_status(char *ip_out, size_t len)
{
    if (ip_out && len) strlcpy(ip_out, s_portal_ip, len);
    /* The page reached us and is reading a CONNECTED result — it can now show
     * the IP, so the portal loop is free to run its reboot countdown. */
    if (s_portal_state == WIFI_PORTAL_CONNECTED) s_portal_ip_seen = true;
    return s_portal_state;
}
