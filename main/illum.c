#include "illum.h"
#include "board_config.h"
#include "led_strip.h"
#include "esp_log.h"

static const char *TAG = "illum";
static led_strip_handle_t s_strip = NULL;

void illum_init(void)
{
#if PIN_IR_LED >= 0
    led_strip_config_t strip_cfg = { .strip_gpio_num = PIN_IR_LED, .max_leds = 1 };
    led_strip_rmt_config_t rmt_cfg = { .resolution_hz = 10 * 1000 * 1000 };
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip) != ESP_OK) {
        ESP_LOGW(TAG, "illuminator init failed (GPIO%d)", PIN_IR_LED);
        s_strip = NULL;
    }
#endif
}

bool illum_available(void) { return s_strip != NULL; }

void illum_set(bool on)
{
    if (!s_strip) return;
    if (on) led_strip_set_pixel(s_strip, 0, 24, 24, 24);
    else    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);
}
