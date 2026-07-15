/* NVS-backed runtime settings (FSD §5). One NVS key per field rather than a
 * blob: fields added in later firmware just fall back to their compiled-in
 * default instead of invalidating every stored setting. */
#include "settings.h"
#include "version.h"

#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "settings";

settings_t g_settings = {
    .mode              = MODE_FEEDER,
    .motion_sensitivity = 50,
    .capture_count      = 5,
    .capture_interval_ms = 1500,
    .cooldown_s         = 3,
    .confidence_pct     = 60,
    .sd_cap_pct         = 80,
    .stream_quality     = 12,
    .ir_led_mode        = 0,
    .rotation           = ROTATE_0,
    .region_filter      = 0,
    .resolution         = 3,   /* HD 1280x720 — matches camera.c RES table index.
                                * ROI-crop makes classification aspect-independent,
                                * so we lock a high-detail default and never tune
                                * aspect again (§3.2.3). */
    .contrast           = 0,
    .ae_level           = 0,
    .timezone           = "CET-1CEST,M3.5.0,M10.5.0/3",
    .region             = "",
    .ntp_server         = "pool.ntp.org",
    .lang               = LANG_EN,
    .detect_zone        = ~0ULL,   /* all 64 cells in the detection zone */
    .detect_zoom        = 0,   /* off: cropping HURTS the v1 iNat model — tight
                                * crops read as "no bird" (whole-frame wins). Keep
                                * 0 until a Nordic-retrained model ships (§3.2.1). */
    .fast_shutter       = 0,
    .detect_quarantine_s = 60,
    .tta                = 0,
    .claude_enabled     = 0,   /* opt-in: needs the user's own API key and
                                * bills them per event (§3.2.3) */
    .claude_key         = "",
};

esp_err_t settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored settings — using defaults");
        return ESP_OK;
    }
    uint8_t  u8;
    uint16_t u16;
    size_t   l;
    if (nvs_get_u8 (h, "s_mode", &u8)  == ESP_OK) g_settings.mode = u8 ? MODE_FEEDER : MODE_NESTBOX;
    if (nvs_get_u8 (h, "s_sens", &u8)  == ESP_OK) g_settings.motion_sensitivity = u8;
    if (nvs_get_u8 (h, "s_ccnt", &u8)  == ESP_OK) g_settings.capture_count = u8;
    if (nvs_get_u16(h, "s_civl", &u16) == ESP_OK) g_settings.capture_interval_ms = u16;
    if (nvs_get_u16(h, "s_cool", &u16) == ESP_OK) g_settings.cooldown_s = u16;
    if (nvs_get_u8 (h, "s_conf", &u8)  == ESP_OK) g_settings.confidence_pct = u8;
    if (nvs_get_u8 (h, "s_cap",  &u8)  == ESP_OK) g_settings.sd_cap_pct = u8;
    if (nvs_get_u8 (h, "s_qual", &u8)  == ESP_OK) g_settings.stream_quality = u8;
    if (nvs_get_u8 (h, "s_ir",   &u8)  == ESP_OK) g_settings.ir_led_mode = u8;
    if (nvs_get_u8 (h, "s_rot",  &u8)  == ESP_OK) g_settings.rotation = (rotation_t) u8;
    if (nvs_get_u8 (h, "s_rfilt",&u8)  == ESP_OK) g_settings.region_filter = u8;
    if (nvs_get_u8 (h, "s_res",  &u8)  == ESP_OK) g_settings.resolution = u8;
    int8_t i8;
    if (nvs_get_i8 (h, "s_ctr",  &i8)  == ESP_OK) g_settings.contrast = i8;
    if (nvs_get_i8 (h, "s_ael",  &i8)  == ESP_OK) g_settings.ae_level = i8;
    l = sizeof(g_settings.timezone);
    nvs_get_str(h, "s_tz", g_settings.timezone, &l);
    l = sizeof(g_settings.region);
    nvs_get_str(h, "s_region", g_settings.region, &l);
    l = sizeof(g_settings.ntp_server);
    nvs_get_str(h, "s_ntp", g_settings.ntp_server, &l);
    if (nvs_get_u8 (h, "s_lang", &u8)  == ESP_OK) g_settings.lang = u8 ? LANG_NO : LANG_EN;
    uint64_t u64;
    if (nvs_get_u64(h, "s_zone", &u64) == ESP_OK) g_settings.detect_zone = u64;
    if (nvs_get_u8 (h, "s_zoom", &u8)  == ESP_OK) g_settings.detect_zoom = u8;
    if (nvs_get_u8 (h, "s_fshut",&u8)  == ESP_OK) g_settings.fast_shutter = u8;
    if (nvs_get_u8 (h, "s_tta",  &u8)  == ESP_OK) g_settings.tta = u8;
    if (nvs_get_u16(h, "s_qtn",  &u16) == ESP_OK) g_settings.detect_quarantine_s = u16;
    if (nvs_get_u8 (h, "s_cld",  &u8)  == ESP_OK) g_settings.claude_enabled = u8;
    l = sizeof(g_settings.claude_key);
    nvs_get_str(h, "s_ckey", g_settings.claude_key, &l);
    nvs_close(h);
    ESP_LOGI(TAG, "settings loaded (mode %s, sensitivity %u, quality %u)",
             g_settings.mode == MODE_FEEDER ? "feeder" : "nestbox",
             g_settings.motion_sensitivity, g_settings.stream_quality);
    return ESP_OK;
}

esp_err_t settings_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings_save: nvs_open failed (%s)", esp_err_to_name(err));
        return err;
    }
    nvs_set_u8 (h, "s_mode", g_settings.mode == MODE_FEEDER ? 1 : 0);
    nvs_set_u8 (h, "s_sens", g_settings.motion_sensitivity);
    nvs_set_u8 (h, "s_ccnt", g_settings.capture_count);
    nvs_set_u16(h, "s_civl", g_settings.capture_interval_ms);
    nvs_set_u16(h, "s_cool", g_settings.cooldown_s);
    nvs_set_u8 (h, "s_conf", g_settings.confidence_pct);
    nvs_set_u8 (h, "s_cap",  g_settings.sd_cap_pct);
    nvs_set_u8 (h, "s_qual", g_settings.stream_quality);
    nvs_set_u8 (h, "s_ir",   g_settings.ir_led_mode);
    nvs_set_u8 (h, "s_rot",  (uint8_t) g_settings.rotation);
    nvs_set_u8 (h, "s_rfilt", g_settings.region_filter);
    nvs_set_u8 (h, "s_res",  g_settings.resolution);
    nvs_set_i8 (h, "s_ctr",  g_settings.contrast);
    nvs_set_i8 (h, "s_ael",  g_settings.ae_level);
    nvs_set_str(h, "s_tz",   g_settings.timezone);
    nvs_set_str(h, "s_region", g_settings.region);
    nvs_set_str(h, "s_ntp", g_settings.ntp_server);
    nvs_set_u8 (h, "s_lang", g_settings.lang == LANG_NO ? 1 : 0);
    nvs_set_u64(h, "s_zone", g_settings.detect_zone);
    nvs_set_u8 (h, "s_zoom", g_settings.detect_zoom);
    nvs_set_u8 (h, "s_fshut", g_settings.fast_shutter);
    nvs_set_u8 (h, "s_tta",   g_settings.tta);
    nvs_set_u16(h, "s_qtn",   g_settings.detect_quarantine_s);
    nvs_set_u8 (h, "s_cld",   g_settings.claude_enabled);
    nvs_set_str(h, "s_ckey",  g_settings.claude_key);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "settings saved");
    return err;
}
