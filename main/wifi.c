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

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_RETRY           5
#define BOOT_BTN_GPIO       GPIO_NUM_0
#define BOOT_BTN_HOLD_MS    5000
#define PORTAL_RETRY_MS     60000

static EventGroupHandle_t s_wifi_eg;
static int                s_retry_count = 0;
static bool               s_wifi_ever_connected = false;
static bool               s_connected = false;
static bool               s_portal_mode = false;
static bool               s_have_stored_creds = false;

volatile bool     g_wifi_save_requested = false;
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

/* ── NVS helpers ────────────────────────────────────────────────────────── */
static esp_err_t nvs_load_wifi(char *ssid, char *pass, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t sl = len, pl = len;
    err  = nvs_get_str(h, "ssid", ssid, &sl);
    err |= nvs_get_str(h, "pass", pass, &pl);
    nvs_close(h);
    return err;
}

static void nvs_save_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "WiFi credentials saved");
    }
}

static void nvs_erase_wifi(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "pass");
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
                esp_wifi_connect();
                s_retry_count++;
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
            nvs_save_wifi(g_new_ssid, g_new_pass);
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

    /* Block until the user submits credentials; meanwhile, if stored
     * credentials exist (boot-time connect failed, §4.4), keep retrying them
     * in the background — the router may just have been down. */
    int64_t last_retry_us = esp_timer_get_time();
    while (!g_wifi_save_requested) {
        if (s_have_stored_creds &&
            esp_timer_get_time() - last_retry_us > (int64_t) PORTAL_RETRY_MS * 1000) {
            last_retry_us = esp_timer_get_time();
            ESP_LOGI(TAG, "Portal open — retrying stored network in background");
            esp_wifi_connect();   /* GOT_IP handler reboots on success */
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    nvs_save_wifi(g_new_ssid, g_new_pass);
    ESP_LOGI(TAG, "Credentials saved, restarting…");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* ── Public API ─────────────────────────────────────────────────────────── */
esp_err_t wifi_start(void)
{
    check_credential_reset();

    s_wifi_eg = xEventGroupCreate();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    nvs_load_ipcfg();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    char ssid[64] = {0}, pass[64] = {0};
    if (nvs_load_wifi(ssid, pass, sizeof(ssid)) == ESP_OK && ssid[0] != '\0') {
        s_have_stored_creds = true;
        wifi_config_t sta_cfg = {0};
        strlcpy((char *) sta_cfg.sta.ssid,     ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *) sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        apply_static_ip(sta_netif);
        ESP_ERROR_CHECK(esp_wifi_start());

        /* Modem-sleep power save adds DTIM-interval latency to every HTTP
         * exchange — deadly for an MJPEG stream. Device is mains-powered. */
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
        /* Best-effort: the real ceiling is module-dependent, a rejected value
         * just leaves the IDF default. */
        esp_err_t txp = esp_wifi_set_max_tx_power(84);
        if (txp != ESP_OK) ESP_LOGW(TAG, "set_max_tx_power: %s", esp_err_to_name(txp));

        EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(15000));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "WiFi connected to %s", ssid);
            /* Clock: SNTP + timezone (FSD §3.4). Best-effort — events before
             * first sync get placeholder timestamps. */
            esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&sntp_cfg);
            setenv("TZ", g_settings.timezone, 1);
            tzset();
            xTaskCreate(config_apply_task, "cfg_apply", 4096, NULL, 3, NULL);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "WiFi connect failed, opening portal (will keep retrying stored network)");
        esp_wifi_stop();
    }

    s_portal_mode = true;
    start_config_portal();
    return ESP_OK;   /* unreachable — portal reboots after save */
}

bool wifi_is_connected(void)   { return s_connected; }
bool wifi_in_portal_mode(void) { return s_portal_mode; }
