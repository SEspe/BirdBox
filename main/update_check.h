#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Daily "is there a newer release?" check against this project's GitHub
 * releases (FSD §8). Notification only — it never downloads or flashes; the
 * operator updates from the OTA tab as before.
 *
 * Once a day the device asks api.github.com for the latest published release
 * and compares its tag with FIRMWARE_VERSION. The result rides on /api/status
 * (latest, updAvail, updAgeS, updErr) and the header shows a clickable
 * "(update available vX.Y.Z)" note that opens the OTA tab. */

void update_check_start(void);

void update_check_latest(char *out, size_t n); /* "1.2.0", "" until a check has succeeded */
void update_check_error(char *out, size_t n);  /* last failure tag ("net", "h403", ...), "" if none */
bool update_check_available(void);             /* latest is strictly newer than running */
long update_check_age_s(void);                 /* seconds since the last successful check, -1 never */
bool update_check_busy(void);                  /* a check (TLS handshake) is in flight */
