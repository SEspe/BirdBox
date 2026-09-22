/* Night sleep (FSD §14). See night.h for why waking is a timed probe rather
 * than the same continuous reading that puts the box to sleep.
 *
 * Deep sleep note: on the ESP32 a deep-sleep wake is a RESET, not a resume —
 * the app starts again from app_main. That is exactly why the "WiFi window"
 * the operator asked for costs nothing extra to implement: every probe wake is
 * an ordinary boot, which brings WiFi, the web server and Home Assistant up on
 * its own. The box is therefore reachable for NIGHT_MIN_AWAKE_S out of every
 * probe interval, and only goes back down if it is still dark. The price is
 * that a wake is a full bringup (NTP resync, SD remount, camera init), so deep
 * sleep is worth it for battery/solar and not for a mains box — which is why
 * it is one of three levels rather than the only behaviour. */
#include "night.h"
#include "settings.h"
#include "camera.h"
#include "motion.h"
#include "web_server.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"

static const char *TAG = "night";

#define NIGHT_CHECK_S       30   /* how often the awake box re-reads ambient   */
#define NIGHT_MIN_AWAKE_S   45   /* reachable window before deep-sleeping again */
#define NIGHT_SNOOZE_MIN    15   /* no re-sleep for this long after a manual wake */
/* Backstop. A lens under snow, a leaf, or a bird roosting on the camera reads
 * dark forever, and the box would sleep through the day with nothing to say so.
 * After this long asleep it wakes and STAYS awake for a snooze period whatever
 * the light says, so a stuck reading self-heals daily instead of silently
 * costing a season. Chosen over an almanac sanity-check because a latitude
 * this project actually runs at has real polar nights, where "the sun must be
 * up by now" is simply false for weeks. */
#define NIGHT_MAX_SLEEP_H   14
/* How long a live viewer may hold off sleep once it has gone dark. A viewer is
 * a reason to WAIT, not a veto: watching the Live tab is exactly what someone
 * does to see whether the box sleeps, so an unbounded hold makes the feature
 * defeat itself the moment it is observed. */
#define NIGHT_VIEWER_GRACE_S 300

static volatile bool s_asleep;
static volatile bool s_probing;
static int64_t s_sleep_since_us;
static int64_t s_snooze_until_us;
static volatile int s_last_luma = -1;
static int64_t s_dark_since_us;
/* Why the box is still awake though night sleep is enabled. Reported by
 * /api/night: "online, mode 1" with no explanation is the same invisible-state
 * trap the motion cluster-cap rejection telemetry exists to close (v3.01). */
static const char *s_hold = "";
static int64_t s_boot_us;

static void enter_sleep(void)
{
    if (s_asleep) return;
    /* Detection first, camera second: stopping the loop means nothing is
     * mid-grab when the sensor goes down, which is what camera_sleep()'s drain
     * is there to guarantee anyway — this just makes the common case clean. */
    motion_set_detection_enabled(false);
    vTaskDelay(pdMS_TO_TICKS(500));
    if (camera_sleep() != ESP_OK) {
        /* A wedged grab: stay fully awake rather than deinit under it. */
        motion_set_detection_enabled(true);
        ESP_LOGW(TAG, "could not sleep the camera — staying online");
        return;
    }
    s_asleep = true;
    s_sleep_since_us = esp_timer_get_time();
    ESP_LOGI(TAG, "dark — detection paused, camera down");
}

static void resume_online(void)
{
    if (!s_asleep) return;
    camera_wake();
    motion_set_detection_enabled(true);
    s_asleep = false;
    s_sleep_since_us = 0;
    ESP_LOGI(TAG, "light — back online");
}

/* One probe cycle: wake the sensor, measure, decide. Leaves the camera down
 * again if it is still dark. */
static bool probe_is_bright(void)
{
    s_probing = true;
    if (camera_wake() != ESP_OK) {
        s_probing = false;
        ESP_LOGW(TAG, "probe: camera wake failed");
        return false;
    }
    int luma = motion_ambient_probe();
    s_last_luma = luma;
    /* A failed decode is not darkness. Treating it as dark would let a
     * transient decode error extend the night indefinitely; treating it as
     * bright would wake the box on a glitch. Stay asleep and retry next
     * interval, which is what "inconclusive" should cost. */
    bool bright = (luma >= 0) && !motion_ambient_dark();
    if (!bright) camera_sleep();
    s_probing = false;
    ESP_LOGI(TAG, "probe: luma %d -> %s", luma, bright ? "bright" : "still dark");
    return bright;
}

static void night_task(void *arg)
{
    (void) arg;
    s_boot_us = esp_timer_get_time();

    for (;;) {
        night_mode_t mode = (night_mode_t) g_settings.sleep_mode;
        int64_t now = esp_timer_get_time();

        if (mode == NIGHT_OFF) {
            resume_online();
            vTaskDelay(pdMS_TO_TICKS(NIGHT_CHECK_S * 1000));
            continue;
        }

        if (!s_asleep) {
            bool dark = motion_ambient_dark();
            if (!dark) { s_dark_since_us = 0; s_hold = "bright"; }
            else if (!s_dark_since_us) s_dark_since_us = now;

            if (dark) {
                /* An OTA is an absolute veto — interrupting a firmware write
                 * can brick the box. A viewer is only a DELAY: it gets a grace
                 * period from the moment it went dark, then the box sleeps
                 * anyway and the live view handles the stream ending. Letting a
                 * viewer veto outright meant one forgotten browser tab disabled
                 * night sleep permanently and silently. */
                bool grace = (now - s_dark_since_us) <
                             (int64_t) NIGHT_VIEWER_GRACE_S * 1000000LL;
                if (now < s_snooze_until_us)                 s_hold = "snoozed";
                else if (web_server_ota_active())            s_hold = "ota in progress";
                else if (web_server_streaming() && grace)    s_hold = "live viewer";
                else { s_hold = ""; enter_sleep(); }
            }
            vTaskDelay(pdMS_TO_TICKS(NIGHT_CHECK_S * 1000));
            continue;
        }

        /* ── Asleep ─────────────────────────────────────────────────────── */
        if (mode == NIGHT_DEEP_SLEEP &&
            (now - s_boot_us) > (int64_t) NIGHT_MIN_AWAKE_S * 1000000 &&
            !web_server_ota_active()) {   /* never deep-sleep mid-OTA */
            uint32_t mins = g_settings.sleep_probe_min ? g_settings.sleep_probe_min : 10;
            ESP_LOGW(TAG, "deep sleep for %u min (wake = reboot; WiFi window on wake)",
                     (unsigned) mins);
            /* The camera is already down; deep sleep takes care of the rest.
             * This call does not return — the box reboots on the timer. */
            esp_deep_sleep((uint64_t) mins * 60ULL * 1000000ULL);
        }

        uint32_t probe_min = g_settings.sleep_probe_min ? g_settings.sleep_probe_min : 10;
        vTaskDelay(pdMS_TO_TICKS(probe_min * 60 * 1000));

        if (!s_asleep) continue;                    /* woken manually meanwhile */
        if ((night_mode_t) g_settings.sleep_mode == NIGHT_OFF) continue;

        if (s_sleep_since_us &&
            (esp_timer_get_time() - s_sleep_since_us) >
                (int64_t) NIGHT_MAX_SLEEP_H * 3600 * 1000000LL) {
            ESP_LOGW(TAG, "asleep %d h — forcing a wake (lens blocked?)", NIGHT_MAX_SLEEP_H);
            resume_online();
            s_snooze_until_us = esp_timer_get_time() +
                                (int64_t) NIGHT_SNOOZE_MIN * 60 * 1000000LL;
            continue;
        }

        if (probe_is_bright()) resume_online();
    }
}

esp_err_t night_start(void)
{
    if (xTaskCreate(night_task, "night", 3072, NULL, 3, NULL) != pdPASS)
        return ESP_FAIL;
    /* Bitmap API: the singular esp_sleep_get_wakeup_cause() is deprecated in
     * IDF v6. A timer wake means this boot IS a probe wake, not a cold start. */
    if (esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_TIMER))
        ESP_LOGI(TAG, "woke from deep sleep — probing before deciding");
    return ESP_OK;
}

void night_wake_now(void)
{
    s_snooze_until_us = esp_timer_get_time() +
                        (int64_t) NIGHT_SNOOZE_MIN * 60 * 1000000LL;
    resume_online();
    ESP_LOGI(TAG, "manual wake — holding off sleep for %d min", NIGHT_SNOOZE_MIN);
}

bool night_asleep(void) { return s_asleep; }
const char *night_hold_reason(void) { return s_asleep ? "" : s_hold; }
int  night_last_luma(void) { return s_last_luma; }

int night_asleep_s(void)
{
    if (!s_asleep || !s_sleep_since_us) return 0;
    return (int) ((esp_timer_get_time() - s_sleep_since_us) / 1000000);
}

const char *night_state_str(void)
{
    if (g_settings.sleep_mode == NIGHT_OFF) return "disabled";
    if (s_probing) return "probing";
    return s_asleep ? "sleeping" : "online";
}
