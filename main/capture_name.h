#pragma once
#include <stdbool.h>
#include <stddef.h>

/* The pure part of the capture layout (FSD §3.1, v3.41): which hour bucket a
 * capture name belongs to, and how a logical path splits. No ESP-IDF headers,
 * so test/host can test it on a PC; storage.c does the filesystem work. */

/* "YYYY-MM-DD_HH-MM-SS-mmm.jpg" -> "HH" in out (NUL-terminated). Any other
 * shape (a pre-SNTP "upNNNN.jpg") has no hour and returns false: such files
 * stay flat in the day folder. */
bool capture_name_hour(const char *name, char out[3]);

/* The same as an hour number 0-23, or -1 when there is none (or it is >23). */
int capture_name_hour_num(const char *name);

/* Split "/captures/<day>/<name>" (with or without a mount prefix in front)
 * into day and name. False when either part is empty or does not fit, or when
 * the name itself contains a slash — that is a PHYSICAL bucketed path from the
 * filesystem, not a logical one from the visit log. */
bool capture_path_split(const char *logical, char *day, size_t dsz,
                        char *name, size_t nsz);
