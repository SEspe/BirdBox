#pragma once
#include <stdbool.h>
#include <stddef.h>

/* The pure part of the daily update check (FSD §8, update_check.c): reading a
 * GitHub "latest release" reply and comparing versions. No ESP-IDF headers,
 * so test/host can test it on a PC. */

/* "1.2.3" -> true and the three parts. Anything else — a leading "v", a
 * pre-release suffix ("1.2.3-rc1"), missing parts, trailing text — is false
 * and is therefore never offered as an update. */
bool semver_parse(const char *s, int v[3]);

/* True only when `a` parses and is strictly newer than `b` (which must parse
 * too). Compares numerically, so 1.10.0 is newer than 1.9.0. */
bool semver_newer(const char *a, const char *b);

/* Pull "tag_name":"v1.2.0" out of a GitHub release JSON reply, tolerating
 * whitespace around the colon, and write it with any leading v/V removed.
 * Only digits and dots are accepted, which also makes the result JSON-safe
 * for /api/status. False if absent, empty, too long for `n`, or malformed. */
bool release_extract_tag(const char *json, char *out, size_t n);

/* True if the reply's "assets" array names a .bin file — the OTA tab lists
 * releases by their .bin, so a release without one is nothing it can flash. */
bool release_has_bin(const char *json);
