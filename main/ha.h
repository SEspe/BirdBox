#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Home Assistant integration over MQTT (FSD §13).
 *
 * BirdBox publishes its own diagnostics to an MQTT broker using Home
 * Assistant's MQTT Discovery convention, so HA creates every entity by itself
 * and no YAML is ever written by hand. One retained discovery message per
 * sensor at connect, then ONE state message carrying all values every
 * HA_PUBLISH_INTERVAL_S seconds — a single publish rather than one per entity,
 * because the entities all read their value out of the same JSON document.
 *
 * Entirely opt-in: with g_settings.ha_enabled clear no client is created, no
 * task is spawned and no socket is opened, so a box with no HA pays nothing
 * for this existing. */

#define HA_PUBLISH_INTERVAL_S 60

/* Starts the MQTT client + publish task when HA is enabled and a broker host
 * is set; a no-op otherwise. Safe to call when WiFi is not up yet — esp-mqtt
 * retries the connection on its own. Called LAST in app_main, after the image
 * has already marked itself valid, so a broker that misbehaves can never cost
 * an otherwise-good image its OTA rollback vote (FSD §8). */
esp_err_t ha_start(void);

/* Re-reads g_settings and restarts (or stops) the client. Called after a
 * settings save so a changed broker/credential takes effect immediately
 * instead of at the next reboot. */
void ha_apply(void);

/* Live state for the Settings tab's System Monitoring status line. */
bool        ha_enabled(void);
bool        ha_connected(void);
const char *ha_last_error(void);   /* "" until something fails */
unsigned    ha_publish_count(void);
