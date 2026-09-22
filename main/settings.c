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
    .capture_count      = 4,
    .capture_interval_ms = 1000,
    .cooldown_s         = 120,
    .confidence_pct     = 30,
    .sd_cap_pct         = 80,
    .stream_quality     = 12,
    .ir_led_mode        = 0,
    .rot_deg            = 0,
    .mirror_h           = 0,
    .mirror_v           = 0,
    .region_filter      = 1,
    .resolution         = 3,   /* HD 1280x720 — matches camera.c RES table index.
                                * ROI-crop makes classification aspect-independent,
                                * so we lock a high-detail default and never tune
                                * aspect again (§3.2.3). */
    .contrast           = 0,
    .ae_level           = 0,
    .sharpness          = 0,   /* neutral. OV5640-class only; ignored elsewhere */
    .denoise            = 0,   /* off — smoothing costs the feather detail that
                                * species ID depends on (§5) */
    .focus_mode         = FOCUS_OFF, /* most OV5640 modules are fixed-focus, and
                                      * OFF also skips the AF firmware download */
    .focus_pos          = 0,
    .timezone           = "CET-1CEST,M3.5.0,M10.5.0/3",
    .ntp_server         = "pool.ntp.org",
    .stats_reset_ts     = "",
    .lang               = LANG_NO,
    .detect_zone        = ~0ULL,   /* all 64 cells in the detection zone */
    .detect_zoom        = 0,   /* off: cropping HURTS the v1 iNat model — tight
                                * crops read as "no bird" (whole-frame wins). Keep
                                * 0 until a Nordic-retrained model ships (§3.2.1). */
    .mount              = MOUNT_MEDIUM, /* 20 cells was tuned for a distant
                                * mount and silently discards a close bird as a
                                * wind swath (§3.1); medium is the safer middle */
    .fast_shutter       = 0,
    .detect_quarantine_s = 60,
    .tta                = 0,
    .cloud_provider     = 0,   /* opt-in: off until the user picks a provider and
                                * supplies that provider's key (§3.2.3) */
    .claude_key         = "",
    .gemini_key         = "",
    .gemini_model       = "",   /* "" => gemini.c's GEMINI_MODEL_DEFAULT */
    .inat_cv_enabled    = 0,    /* opt-in primary tier; needs a (24h) iNat JWT */
    .inat_key           = "",
    .inat_loc           = "59.91,10.75",   /* iNat geo hint "lat,lng"; default
                                            * Oslo (matches the cities_data.h
                                            * dropdown entry); "" = no geo */
    .ha_enabled         = 0,    /* opt-in: nothing is published until the
                                * operator enables it and names a broker */
    .ha_host            = "",
    .ha_port            = 1883,
    .ha_user            = "",
    .ha_pass            = "",
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
    /* Rotation migration (v2.83). The old key `s_rot` held a quarter-turn enum
     * 0-3; the new `s_rotd` holds degrees 0-359. They cannot share a key —
     * a stored 3 would be 270 deg under the old meaning and 3 deg under the
     * new. So: prefer s_rotd, and fall back to s_rot x 90 exactly once, on a
     * box that has never saved since upgrading. The first save writes s_rotd
     * and this fallback stops mattering. */
    if (nvs_get_u16(h, "s_rotd", &u16) == ESP_OK) {
        g_settings.rot_deg = u16 % 360;
    } else if (nvs_get_u8(h, "s_rot", &u8) == ESP_OK && u8 < 4) {
        g_settings.rot_deg = (uint16_t) u8 * 90;
        ESP_LOGI(TAG, "migrated stored rotation %u (quarter turns) -> %u deg",
                 (unsigned) u8, (unsigned) g_settings.rot_deg);
    }
    if (nvs_get_u8 (h, "s_mirh", &u8)  == ESP_OK) g_settings.mirror_h = u8 ? 1 : 0;
    if (nvs_get_u8 (h, "s_mirv", &u8)  == ESP_OK) g_settings.mirror_v = u8 ? 1 : 0;
    if (nvs_get_u8 (h, "s_rfilt",&u8)  == ESP_OK) g_settings.region_filter = u8;
    if (nvs_get_u8 (h, "s_res",  &u8)  == ESP_OK) g_settings.resolution = u8;
    int8_t i8;
    if (nvs_get_i8 (h, "s_ctr",  &i8)  == ESP_OK) g_settings.contrast = i8;
    if (nvs_get_i8 (h, "s_ael",  &i8)  == ESP_OK) g_settings.ae_level = i8;
    if (nvs_get_i8 (h, "s_shrp", &i8)  == ESP_OK) g_settings.sharpness = i8;
    if (nvs_get_u8 (h, "s_dnz",  &u8)  == ESP_OK) g_settings.denoise = u8;
    if (nvs_get_u8 (h, "s_fmode",&u8)  == ESP_OK) g_settings.focus_mode = u8;
    if (nvs_get_u16(h, "s_fpos", &u16) == ESP_OK) g_settings.focus_pos = u16;
    l = sizeof(g_settings.timezone);
    nvs_get_str(h, "s_tz", g_settings.timezone, &l);
    l = sizeof(g_settings.ntp_server);
    nvs_get_str(h, "s_ntp", g_settings.ntp_server, &l);
    l = sizeof(g_settings.stats_reset_ts);
    nvs_get_str(h, "s_statrst", g_settings.stats_reset_ts, &l);
    if (nvs_get_u8 (h, "s_lang", &u8)  == ESP_OK) g_settings.lang = u8 ? LANG_NO : LANG_EN;
    uint64_t u64;
    if (nvs_get_u64(h, "s_zone", &u64) == ESP_OK) g_settings.detect_zone = u64;
    if (nvs_get_u8 (h, "s_zoom", &u8)  == ESP_OK) g_settings.detect_zoom = u8;
    if (nvs_get_u8 (h, "s_mount", &u8)  == ESP_OK && u8 <= MOUNT_DISTANT) g_settings.mount = u8;
    if (nvs_get_u8 (h, "s_fshut",&u8)  == ESP_OK) g_settings.fast_shutter = u8;
    if (nvs_get_u8 (h, "s_tta",  &u8)  == ESP_OK) g_settings.tta = u8;
    if (nvs_get_u16(h, "s_qtn",  &u16) == ESP_OK) g_settings.detect_quarantine_s = u16;
    /* Cloud provider selector. New key is s_cprov; migrate the pre-two-provider
     * layout, where a bare s_cld=1 (u8 boolean "Claude on") meant Claude. Read
     * s_cprov if present, else fall back to the old s_cld. */
    if (nvs_get_u8 (h, "s_cprov", &u8) == ESP_OK) g_settings.cloud_provider = u8;
    else if (nvs_get_u8 (h, "s_cld", &u8) == ESP_OK) g_settings.cloud_provider = u8 ? 1 : 0;
    l = sizeof(g_settings.claude_key);
    nvs_get_str(h, "s_ckey", g_settings.claude_key, &l);
    l = sizeof(g_settings.gemini_key);
    nvs_get_str(h, "s_gkey", g_settings.gemini_key, &l);
    l = sizeof(g_settings.gemini_model);
    nvs_get_str(h, "s_gmdl", g_settings.gemini_model, &l);
    if (nvs_get_u8 (h, "s_inatcv", &u8) == ESP_OK) g_settings.inat_cv_enabled = u8;
    l = sizeof(g_settings.inat_key);
    nvs_get_str(h, "s_inatk", g_settings.inat_key, &l);
    l = sizeof(g_settings.inat_session);
    nvs_get_str(h, "s_inatses", g_settings.inat_session, &l);
    l = sizeof(g_settings.inat_user);
    nvs_get_str(h, "s_inatusr", g_settings.inat_user, &l);
    l = sizeof(g_settings.inat_pass);
    nvs_get_str(h, "s_inatpw", g_settings.inat_pass, &l);
    l = sizeof(g_settings.inat_loc);
    nvs_get_str(h, "s_iloc", g_settings.inat_loc, &l);
    if (nvs_get_u8 (h, "s_haen",  &u8)  == ESP_OK) g_settings.ha_enabled = u8 ? 1 : 0;
    l = sizeof(g_settings.ha_host);
    nvs_get_str(h, "s_hahost", g_settings.ha_host, &l);
    if (nvs_get_u16(h, "s_haport", &u16) == ESP_OK && u16 > 0) g_settings.ha_port = u16;
    l = sizeof(g_settings.ha_user);
    nvs_get_str(h, "s_hauser", g_settings.ha_user, &l);
    l = sizeof(g_settings.ha_pass);
    nvs_get_str(h, "s_hapass", g_settings.ha_pass, &l);
    /* "s_inat"/"s_inatv" (the periodic re-scan) are no longer read — v2.91. Any
     * value a device already has in NVS is simply left there, inert. */
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
    nvs_set_u16(h, "s_rotd", g_settings.rot_deg);
    nvs_set_u8 (h, "s_mirh", g_settings.mirror_h);
    nvs_set_u8 (h, "s_mirv", g_settings.mirror_v);
    nvs_set_u8 (h, "s_rfilt", g_settings.region_filter);
    nvs_set_u8 (h, "s_res",  g_settings.resolution);
    nvs_set_i8 (h, "s_ctr",  g_settings.contrast);
    nvs_set_i8 (h, "s_ael",  g_settings.ae_level);
    nvs_set_i8 (h, "s_shrp", g_settings.sharpness);
    nvs_set_u8 (h, "s_dnz",  g_settings.denoise);
    nvs_set_u8 (h, "s_fmode",g_settings.focus_mode);
    nvs_set_u16(h, "s_fpos", g_settings.focus_pos);
    nvs_set_str(h, "s_tz",   g_settings.timezone);
    nvs_set_str(h, "s_ntp", g_settings.ntp_server);
    nvs_set_str(h, "s_statrst", g_settings.stats_reset_ts);
    nvs_set_u8 (h, "s_lang", g_settings.lang == LANG_NO ? 1 : 0);
    nvs_set_u64(h, "s_zone", g_settings.detect_zone);
    nvs_set_u8 (h, "s_zoom", g_settings.detect_zoom);
    nvs_set_u8 (h, "s_mount", g_settings.mount);
    nvs_set_u8 (h, "s_fshut", g_settings.fast_shutter);
    nvs_set_u8 (h, "s_tta",   g_settings.tta);
    nvs_set_u16(h, "s_qtn",   g_settings.detect_quarantine_s);
    nvs_set_u8 (h, "s_cprov", g_settings.cloud_provider);
    nvs_set_str(h, "s_ckey",  g_settings.claude_key);
    nvs_set_str(h, "s_gkey",  g_settings.gemini_key);
    nvs_set_str(h, "s_gmdl",  g_settings.gemini_model);
    nvs_set_u8 (h, "s_inatcv", g_settings.inat_cv_enabled);
    nvs_set_str(h, "s_inatk",  g_settings.inat_key);
    nvs_set_str(h, "s_inatses", g_settings.inat_session);
    nvs_set_str(h, "s_inatusr", g_settings.inat_user);
    nvs_set_str(h, "s_inatpw",  g_settings.inat_pass);
    nvs_set_str(h, "s_iloc",   g_settings.inat_loc);
    nvs_set_u8 (h, "s_haen",   g_settings.ha_enabled);
    nvs_set_str(h, "s_hahost", g_settings.ha_host);
    nvs_set_u16(h, "s_haport", g_settings.ha_port);
    nvs_set_str(h, "s_hauser", g_settings.ha_user);
    nvs_set_str(h, "s_hapass", g_settings.ha_pass);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "settings saved");
    return err;
}

esp_err_t settings_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset: nvs_open failed (%s)", esp_err_to_name(err));
        return err;
    }
    /* Erase the WHOLE namespace, not the s_* settings keys one at a time. WiFi
     * credentials (ssid/pass/ssid2/pass2) and the static-IP block (ipmode/ip/
     * mask/gw/dns) live in this same namespace, and wiping them is the point —
     * this is the full factory reset, so the box comes up in the config portal.
     * Erasing by name would also silently miss any key a later firmware adds. */
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset: erase failed (%s)", esp_err_to_name(err));
        return err;
    }
    /* g_settings is deliberately NOT reloaded here. The caller reboots, and
     * settings_load() then finds an empty namespace and keeps the compiled-in
     * defaults — one code path for "no stored settings", the same one a
     * freshly-flashed board takes. Mutating the live struct first would only
     * create a window where the running box has defaults but the reboot has
     * not happened yet. */
    ESP_LOGW(TAG, "factory reset: NVS namespace '%s' erased — reboot pending",
             NVS_NAMESPACE);
    return ESP_OK;
}
