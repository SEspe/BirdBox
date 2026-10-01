# BirdBox docs

## What is here

| | |
|---|---|
| [`TELEMETRY.md`](TELEMETRY.md) | **Every field** in `/api/motion` and `/api/sysinfo` and every Home Assistant sensor — what it measures, a healthy value, and what a bad one means. Explicit about which fields are **live** and which are **snapshots written only on success**, because reading one as the other has repeatedly produced wrong conclusions. Read this before drawing any conclusion from a number. |
| [`HA-CARDS.md`](HA-CARDS.md) | Ready-to-paste ApexCharts dashboard cards for those sensors, verified against a live dashboard. Two rules they follow deliberately: one scale per card, and colours in fixed order from a CVD-validated palette. |
| [`MODEL.md`](MODEL.md) | **Historical.** How the on-device TFLite-Micro model was installed, and why it was abandoned for online iNaturalist in firmware 0.74.0. No model file is installed any more. |
| [`SPEC-species-info-panel.md`](SPEC-species-info-panel.md) | A **proposed, unimplemented** click-to-expand species info panel. Its target version numbers are long overtaken; the design is unaffected. |

## Not written

`wiring.md`, `enclosure.md` and `model-swap.md` were planned under FSD §9 and do
not exist. Pin maps live in `main/board_config.h` instead, and `model-swap.md` is
moot — the on-device model was removed (see `MODEL.md`).

## Elsewhere in the repo

- [`../FSD_BirdBox.md`](../FSD_BirdBox.md) — the clean current specification. What to build.
- [`../FSD_BirdBox_CHANGELOG.md`](../FSD_BirdBox_CHANGELOG.md) — the development record. Read this for any "why", and before reopening a settled question.
- [`../CLAUDE.md`](../CLAUDE.md) — how to build, flash, verify and safely change the code.
- [`../TODO.md`](../TODO.md) — backlog, and the documented dead ends not to re-attempt.
- [`../SESSION_NOTES.md`](../SESSION_NOTES.md) — where the last session stopped and what is still unproven.
