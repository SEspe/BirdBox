# Host unit tests

Tests for the parts of the firmware that are pure logic: no SD card, no
camera, no FreeRTOS. They compile with any C compiler on a PC and run in CI on
every push (`host-tests` job in `.github/workflows/release.yml`); a release
cannot be published unless they pass.

```sh
make -C test/host test          # needs gcc or clang
```

Built with `-Wall -Wextra -Werror` and AddressSanitizer + UBSan, so a buffer
overrun or undefined behaviour fails the run instead of passing silently.

| Module (`main/`) | What it is | Tested for |
|---|---|---|
| `csv_field.c` | the one CSV field scanner (visit log, gallery, stats) | empty fields survive (the `strtok` bug), CR/LF never leak into the last field |
| `stats_core.c` | folding a visit-log row into the Stats aggregate | corrections win, "no bird" kept out of visits, reset point, unsynced rows, Latin-name merging, newest-62-days eviction in either read order, species cap |
| `release_core.c` | the daily update check's parsing | strict `X.Y.Z`, numeric compare (1.10 > 1.9), `tag_name` extraction from real GitHub JSON, truncation and junk refused, `.bin` asset detection |
| `capture_name.c` | hour buckets and logical paths | hour from the capture name, pre-SNTP names stay flat, physical paths rejected, over-long parts refused rather than truncated |
| `count_table.c` | the per-day capture-count table | put/find/add/drop, clamping, capacity, and the rule that decides whether an unlocked directory walk may be stored |

## Adding a test

Keep testable logic in a `main/*.c` file that includes **no ESP-IDF headers**
and add it to `SRC` in the `Makefile` and to `main/CMakeLists.txt`. The
firmware module that needs the hardware (`stats.c`, `storage.c`,
`update_check.c`) calls into it. Then add a `test_<module>.c` here, declare it
in `test.h`, and list it in `test_main.c`.

## What these do NOT cover

Anything touching the SD card, FreeRTOS, the camera, HTTP or the web page.
Those are still verified on hardware (see CLAUDE.md "Testing / verification"),
and the UI JavaScript is syntax-checked in CI by `tools/check-ui-js.py`.
