#pragma once

/* One field of a visit-log CSV row, split IN PLACE: writes a NUL over the
 * comma, advances *p past it, and returns the field. On the last field it
 * stops at the line end (CR or LF) instead, and *p is left on the terminator,
 * so further calls return "".
 *
 * Hand-rolled on purpose, never strtok/strtok_r: those collapse a run of
 * delimiters into one, so an EMPTY field (the "corrected" column usually is)
 * vanishes and every later column shifts left by one. Fixed-column parsing
 * must use this. Pure C, no ESP-IDF headers, so test/host can test it. */
char *csv_next_field(char **p);
