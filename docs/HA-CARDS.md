# Home Assistant dashboard cards

Ready-to-paste [ApexCharts card](https://github.com/RomRider/apexcharts-card)
configurations for the diagnostics the box publishes over MQTT. Verified against
a live dashboard on 2026-09-30 (firmware 0.99.2).

Field meanings, healthy ranges and what a bad value indicates are in
[`TELEMETRY.md`](TELEMETRY.md); this file is only the presentation.

## Entity IDs

Home Assistant builds these from the MQTT discovery the box publishes: the
device name (`BirdBox`, from `FIRMWARE_NAME`) plus each entity's name,
slugified. The device id is the last three bytes of the MAC —
`birdbox_47807c` on the reference unit.

| MQTT key | Entity id |
|---|---|
| `inat_ms` | `sensor.birdbox_inaturalist_response` |
| `classify_ms` | `sensor.birdbox_last_classification` |
| `cls_queue` | `sensor.birdbox_classification_queue` |
| `cls_queue_peak` | `sensor.birdbox_classification_queue_peak` |
| `cls_drops` | `sensor.birdbox_classification_drops` |
| `visits` | `sensor.birdbox_bird_visits` |
| `detect_ms` | `sensor.birdbox_detect_cadence` |
| `detect_grab_ms` | `sensor.birdbox_detect_frame_grab` |
| `detect_decode_ms` | `sensor.birdbox_detect_frame_decode` |
| `fast_gap_ms` | `sensor.birdbox_fast_burst_gap` |
| `contrast` | `sensor.birdbox_scene_contrast` |

**On a second box these differ.** Home Assistant appends `_2` when a name is
already taken, and every unit ships the same device name. Confirm in
**Developer Tools → States**, filter `birdbox`, before assuming.

## Two rules these configs follow

**One scale per card.** Milliseconds and queue depth never share a chart. A
dual-axis chart invites the reader to compare two lines whose relative heights
mean nothing, and it is the single most common charting mistake. Different unit
→ different card.

**Colours are assigned in fixed order, never cycled** — slot 1 blue `#3987e5`,
slot 2 orange `#d95926`, from a palette validated for colour-vision deficiency.
Keep the order when adding a series; do not invent a hue for a third line
without checking it separates from these two.

Values below are the dark-mode steps. For a light theme use `#2a78d6` and
`#eb6834`.

---

## 1. Identification time

The cause-and-effect pair, and the reason both are published. `classify_ms` is
the **whole event** — card reads, one call *per frame uploaded*, any crop step —
so it rises both when the service slows and when more frames are sent, and
cannot distinguish them alone. `inat_ms` is **one round trip**, the remote
service by itself.

- lines apart → we sent several frames; the early exit did not settle it
- lines rising **together** → iNaturalist is slow, and nothing on the box helps

```yaml
type: custom:apexcharts-card
header:
  show: true
  title: Identification time
  show_states: true
  colorize_states: true
graph_span: 12h
span:
  end: minute
yaxis:
  - min: 0
    apex_config:
      title: { text: ms }
series:
  - entity: sensor.birdbox_inaturalist_response
    name: iNaturalist round trip
    color: '#3987e5'
    stroke_width: 2
    type: line
    extend_to: now
    group_by: { func: avg, duration: 5min }
  - entity: sensor.birdbox_last_classification
    name: Whole event
    color: '#d95926'
    stroke_width: 2
    type: line
    extend_to: now
    group_by: { func: avg, duration: 5min }
apex_config:
  chart: { height: 240 }
  legend: { show: true, position: bottom }
  stroke: { curve: smooth }
  grid: { borderColor: 'rgba(255,255,255,0.08)' }
  tooltip: { shared: true, x: { format: 'HH:mm' } }
  markers: { size: 0, hover: { size: 5 } }
```

Both read *unknown* until the first identification after a reboot. That is
deliberate — the accessors return −1 beforehand, and publishing that would draw
a false point on a duration graph.

## 2. Identification backlog

Watch the **peak**, not the depth. The reporting interval is 120 s by default
while a burst can fill and drain in 40 s, so the live depth provably cannot show
you the worst moment — the high-water mark can. Peak approaching 16 means the
next busy spell will start discarding events.

```yaml
type: custom:apexcharts-card
header:
  show: true
  title: Identification backlog
  show_states: true
graph_span: 12h
yaxis:
  - min: 0
    max: 16
    apex_config:
      title: { text: events waiting }
series:
  - entity: sensor.birdbox_classification_queue
    name: Queue depth
    color: '#3987e5'
    stroke_width: 2
    type: line
    extend_to: now
  - entity: sensor.birdbox_classification_queue_peak
    name: Peak since boot
    color: '#d95926'
    stroke_width: 2
    curve: stepline
    extend_to: now
apex_config:
  chart: { height: 220 }
  legend: { show: true, position: bottom }
  grid: { borderColor: 'rgba(255,255,255,0.08)' }
  tooltip: { shared: true }
  markers: { size: 0, hover: { size: 5 } }
```

A step line is the honest shape for a high-water mark: it only ever rises, and
smoothing would imply intermediate values that never existed.

## 3. Visits per hour

`visits` is cumulative, so its **difference** is the traffic shape. The raw
total only ever climbs and says nothing about when the birds came.

```yaml
type: custom:apexcharts-card
header:
  show: true
  title: Visits per hour
graph_span: 24h
yaxis:
  - min: 0
    apex_config:
      title: { text: visits }
series:
  - entity: sensor.birdbox_bird_visits
    name: Visits
    color: '#3987e5'
    type: column
    group_by: { func: diff, duration: 1h }
apex_config:
  chart: { height: 200 }
  legend: { show: false }
  grid: { borderColor: 'rgba(255,255,255,0.08)' }
  plotOptions: { bar: { borderRadius: 4, columnWidth: '60%' } }
  tooltip: { x: { format: 'HH:mm' } }
```

Single series, so no legend — the title names it.

The sensor carries `species`, `confidence` and `false_positives` as
**attributes**, and Home Assistant's recorder keeps attributes beside each state
change. Clicking through to the more-info dialog therefore shows which bird was
identified at a given point: the trend and the species in one entity, which is
why the per-species counters were removed.

## 4. Dropped events — deliberately not a chart

This should read 0 forever, and a flat line of zeros is wasted dashboard. A
conditional card shows nothing until something is wrong:

```yaml
type: conditional
conditions:
  - entity: sensor.birdbox_classification_drops
    state_not: '0'
card:
  type: markdown
  content: >
    ⚠️ **{{ states('sensor.birdbox_classification_drops') }} visits were
    discarded** — identification could not keep up. The photographs are still on
    the card; the identification never happened.
```

## 5. Detector health (optional)

Same unit throughout, so one card is legitimate. Useful while diagnosing a
duty-cycle problem; not worth permanent dashboard space otherwise.

```yaml
type: custom:apexcharts-card
header:
  show: true
  title: Detector
  show_states: true
graph_span: 6h
yaxis:
  - min: 0
    apex_config:
      title: { text: ms }
series:
  - entity: sensor.birdbox_detect_cadence
    name: Cadence
    color: '#3987e5'
    stroke_width: 2
    extend_to: now
  - entity: sensor.birdbox_detect_frame_decode
    name: Decode
    color: '#d95926'
    stroke_width: 2
    extend_to: now
  - entity: sensor.birdbox_detect_frame_grab
    name: Camera grab
    color: '#199e70'
    stroke_width: 2
    extend_to: now
apex_config:
  chart: { height: 220 }
  legend: { show: true, position: bottom }
  grid: { borderColor: 'rgba(255,255,255,0.08)' }
  tooltip: { shared: true }
  markers: { size: 0, hover: { size: 5 } }
```

Cadence climbing well above the decode time means the detector is being starved
by something else, not by its own work — which is how the lwIP task landing on
the detection core was found.

## Reporting interval

These trends are only as fine as the publish rate, set in **Settings → System →
System Monitoring** (`ha_interval_s`, 30–600 s, default 120). There is no single
right value: nothing published moves meaningfully inside a minute, and the peak
and cumulative readings carry their own extremes, so they lose nothing to a
slower rate. Use 30–60 s while actively diagnosing so short spikes are not
missed between samples, and 300–600 s for unattended monitoring, where the trend
matters and the database does not need a point every minute.
