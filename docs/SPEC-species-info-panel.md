# SPEC — click-to-expand species info panel

**Status:** proposed, not implemented. **Author:** drafted 2026-08-06.
**Builds on:** FSD v2.92 / firmware 0.74.58 (the live-view iNaturalist reference card).
**Target:** one release, firmware 0.74.59 / FSD v2.93.

Nothing in here is in the FSD changelog yet — the FSD records what shipped. Move a
condensed version of §13 there when this lands.

**Operator decisions already made** (don't re-litigate them from this document):
- **The photo scroller is IN**, at the operator's request. An earlier draft excluded it on
  bandwidth grounds; the measurements in §5.2 show that objection was overstated — the
  photos ride along in JSON we already fetch, and a dot strip plus lazy `small_url` loads
  makes it 15 KB on open. The design below is what makes it cheap; keep those properties.
- **Wikipedia gets a real button**, not a footnote link (§5.3).

---

## 1. Goal

The v2.92 card answers *"what does this species look like?"*. This answers the obvious
next question — *"what IS it?"* — without growing the card, which is 108 px wide (74 px
under 560 px) and sits on top of the live frame where space is the scarcest thing on the
page.

**Non-goal:** this is reference material for a human watching birds. It must not become a
second classification input, must not touch the visit log, and must not influence
`corrected` or the retrain pipeline in any way.

## 2. Interaction

The card gains a second affordance. It currently has exactly two: the whole body is an
`<a>` to iNat's medium photo, and `.rfx` minimizes it.

- **Trigger:** tap the **name block** (`.rfn` / `.rfl`) → opens the panel.
  The **thumbnail keeps its existing link** to the medium photo. Splitting the two means
  the existing gesture is not stolen from anyone who already uses it.
  This requires restructuring `refRender`'s single `<a>` into an `<a>` around the image
  plus a `<button>` around the text — see §7.
- **Dismiss:** click the backdrop, the ✕, or `Esc`.
- **The panel is transient.** It does not persist across page loads and has no
  `localStorage` state — unlike `.livref.min`, which does (§FSD v2.92: a per-browser view
  preference). An open modal is not a preference.
- **The live `<img src=/stream>` keeps running underneath.** Do not tear the stream down
  to open a modal; the stream-slot accounting (`streamUsed`/`streamMax`) is not involved.

## 3. Reuse the existing modal, do not invent one

`web_server.c` already has exactly this widget: the shared settings-info modal,
`.ipop` / `.ipbox` / `.ipx` / `.iprow` (CSS `web_server.c:535-544`, markup ~`:974`,
filled by `sInfo()` at `:1899`). It is a fixed full-viewport backdrop at `z-index:60`,
`max-width:460px`, dismissed by `onclick='if(event.target===this)ipClose()'` plus an ✕.

**Add a second instance (`#spop`), do not share `#ipop`.** They can be open from
different tabs with different lifetimes, and `sInfo()` writes `#ipT`/`#ipD`/`#ipDef`/`#ipAlt`
by ID. Copy the CSS by extending the existing selector lists (`.ipop,.spop{...}`) rather
than duplicating the rules — the whole stylesheet is C string literals and every
duplicated block is bytes in flash and another thing to keep in sync.

## 4. Data source — verified, not assumed

All of it comes from `api.inaturalist.org`, **fetched by the browser**, exactly as v2.92
does. `Access-Control-Allow-Origin: *`, no device proxy, no firmware fetch, no decode, no
SD cache, no extra TLS on the ESP32. This is the load-bearing constraint from FSD §7: the
scarce pool is **internal DRAM**, which mbedTLS competes for, and reference material must
never contend with the classification path for it.

Two calls, both keyed on the taxon `id` we **already have** from v2.92's `refShow()` —
which today throws it away after reading `default_photo`. Keep it.

### 4.1 `GET /v1/taxa/{id}` — the detail record

The search endpoint v2.92 uses (`/v1/taxa?q=`) returns a **trimmed 22-field** record with
no summary and no conservation data. The detail record has 33 fields. Verified against
*Parus major* (id **203153**) on 2026-08-06:

| Field | Value seen | Use |
|---|---|---|
| `wikipedia_summary` | 471 chars of prose, **contains `<b>` and `<i>` tags** | body text (§6) |
| `ancestors[]` | Animalia → Chordata → Vertebrata → **Aves → Passeriformes → Paridae → Parus** | Order / Family rows |
| `conservation_statuses[]` | `{status:'LC', authority:'Finnish Red List 2019', iucn:10, url:…}` | status row (§5) |
| `observations_count` | 244 678 | "how often is this recorded" |
| `wikipedia_url` | `https://en.wikipedia.org/wiki/Parus major` | "read more" link |
| `taxon_photos[]` | **10** photos | *not used* — see §10 |
| `extinct`, `iconic_taxon_name` | `false`, `Aves` | — |

> **⚠ Cost: this response is 76 506 bytes and cannot be trimmed.**
> `?fields=wikipedia_summary,conservation_status` returns **byte-identical 76 506** — the
> v1 REST API ignores it. Most of the weight is `ancestors` + `children` + 10 full
> `taxon_photos` records we do not want. Budget ~75 KB per *new* species per page load,
> from the phone's connection, not the box's. Mitigation is caching (§8) and the fact
> that a feeder sees a handful of species per session, not hundreds.

### 4.2 `GET /v1/observations/histogram` — seasonality

```
/v1/observations/histogram?taxon_id={id}&place_id=7016&interval=month_of_year&verifiable=true
```
**171 bytes.** `place_id=7016` is Norway (verified via `/v1/places/autocomplete?q=Norway`:
id 7016, `place_type` 12, `admin_level` 0 — the country, not one of the several decoys
like "Norway Island" 202361 or "Norway Street" 174679).

**Read §9 before shipping this. It does not mean what it looks like it means.**

## 5. Panel contents

```
┌─ Kjøttmeis ──────────────────────────── ✕ ─┐
│  Parus major · 95 %                        │
│                                            │
│      ┌──────────────────────┐              │
│   ‹  │   main photo (small) │  ›           │
│      └──────────────────────┘              │
│  ▪ ▫ ▫ ▫ ▫ ▫ ▫ ▫ ▫ ▫ ▫ ▫      3 / 12       │
│  (c) Antti Henttonen (CC BY-NC)            │
│                                            │
│  Klasse    Aves                            │
│  Orden     Passeriformes                   │
│  Familie   Paridae                         │
│                                            │
│  The great tit (Parus major) is a          │
│  passerine bird in the tit family          │
│  Paridae. It is widespread and common      │
│  throughout Europe…                        │
│                                            │
│  Status    LC (Finnish Red List 2019)      │
│  På iNat   244 678 observasjoner           │
│                                            │
│  [ 📖 Wikipedia ]        [ iNaturalist ]   │
└────────────────────────────────────────────┘
```

- **Header** — localized common name from the **device** (`species_i18n`, via
  `spCommon()`), binomial from `spLatin()`, and the confidence already passed to
  `refRender`. Same reasoning as v2.92: iNat's `preferred_common_name` is English
  ("Great Tit"). `?locale=nb` *does* return "Kjøttmeis" (verified) — keep it as the
  fallback for a species outside our 31-name table, not as the primary.
- **Taxonomy** — pick `class`, `order`, `family` out of `ancestors` **by `rank`, never by
  array position**. Rank sets are not uniform across taxa (the Parus chain includes a
  `subphylum`), so index arithmetic will silently mislabel something.
- **Summary** — `wikipedia_summary`, clamped to ~6 lines with the full text scrollable.
  **Sanitize per §6.**
- **Conservation** — see §5.4.
- **Attribution** — see §5.2. It is **per photo** and must change as you scroll.

### 5.2 Photo scroller

The detail call (§4.1) already carries `taxon_photos` — **the photos cost no extra JSON**,
they are part of the 76 KB we are already paying for. Only the image loads are new, and
only for photos actually looked at.

**Counts vary — never hard-code 10 or 12.** Measured 2026-08-06:
*Parus major* **10**, *Perisoreus infaustus* **12**, *Pyrrhula pyrrhula* **12**,
*Pica pica* **12**. Drive everything off `taxon_photos.length`; a taxon with 1 photo must
render without arrows or dots, and 0 photos must fall back to the card's `default_photo`.

**Image size ladder** (measured on a real photo): `square` **3 708 B**, `small`
**15 004 B**, `medium` **53 232 B**.

- **Main image: `small_url`.** The modal is `max-width:460px`, so iNat's ~240 px `small`
  is the right fit and costs 15 KB — `medium` is 3.5× the bytes for resolution the panel
  cannot show.
- **Dot strip, not a thumbnail strip.** 12 × `square` would be ~44 KB of images just to
  *open* the panel. Plain CSS dots cost nothing. If a visual strip is wanted later,
  `square` at 3.7 KB is affordable — but load it on demand, not on open.
- **`medium_url` behind the click-through only**, opening `native_page_url` or the medium
  image in a new tab, exactly as the card already does.
- **Preload only the neighbours** (`i-1`, `i+1`) as the index moves. A 12-photo species
  then costs 15 KB on open and 15 KB per step, instead of 180 KB up front.

**Navigation:** ‹ › buttons, `←`/`→` keys while the panel is open, and swipe on touch.
The dot row doubles as the position indicator with an `n / total` label. Wrap around at
both ends. Index resets to 0 whenever the panel opens on a different species.

### 5.2.1 ⚠ Licences differ *per photo within one species* — the credit line is not static

This is the part that is easy to get wrong. Measured licence spread:

| Species | Photos | Licences |
|---|---|---|
| *Parus major* | 10 | `cc-by-nc`, `cc-by-sa`, **1 all-rights-reserved** |
| *Pyrrhula pyrrhula* | 12 | `cc-by-nc` ×10, `cc-by`, `cc-by-sa` |
| *Perisoreus infaustus* | 12 | `cc-by-nc-sa`, `cc-by-nc-nd` ×6, `cc-by` ×3, `cc-by-nc`, **1 ARR** |
| *Pica pica* | 12 | `cc-by-nc-sa` ×2, `cc-by-nc` ×4, `cc-by-nc-nd` ×2, **4 ARR** |

So: **`license_code` can be `null` (all rights reserved), and up to a third of a
species' photos are.** The photos are hotlinked from iNat's own CDN and displayed the way
iNat's own taxon pages display them, but that makes the credit *more* load-bearing, not
less.

- Render `photo.attribution` **verbatim** — it is the pre-formatted string iNat supplies
  (`"(c) Antti Henttonen, some rights reserved (CC BY-NC), uploaded by Antti Henttonen"`).
  Do not synthesise a credit from `attribution_name` + `license_code`.
- **Re-render it on every index change.** A single credit line left over from photo 1
  while photo 4 is showing mis-credits a named person.
- Never drop or truncate-to-nothing. Clamp to 2 lines as the card does, never `display:none`.
- Photos live on two different hosts (`inaturalist-open-data.s3.amazonaws.com` for
  open-licensed, `static.inaturalist.org` for ARR) — both are just `<img src>`, no special
  handling, but do not assume one hostname.

### 5.3 Wikipedia access

`wikipedia_url` comes back as `https://en.wikipedia.org/wiki/Parus major` — **with a raw
space**, which is legal in practice but should be `encodeURI`'d before use.

Give it a **real button**, not a footnote link: it is the "tell me more" exit and the
reason the summary is deliberately clamped. `target=_blank rel=noopener`, same as every
other outbound link in the UI. Pair it with a second button to the iNat taxon page
(`https://www.inaturalist.org/taxa/{id}` — we have the id from §8), which is where the
full photo set and the range map live.

### 5.4 Conservation status is an array, and it is not IUCN

`conservation_statuses` came back for *Parus major* as a **Finnish Red List 2019** entry.
There is no single global status; the field is a list of *per-authority* assessments, and
the flat `conservation_status` was `null`. So:

- Show **at most one** row, and **name the authority in it** — "LC (Finnish Red List
  2019)" is honest; a bare "LC" implies a global IUCN assessment that this payload does
  not contain.
- Prefer an entry whose `place` is Norway/Nordic if one exists, else the first; if the
  array is empty or absent, **omit the row entirely** rather than printing "Unknown".
- Do not colour-code it. A red "LC" badge invites reading it as an alert, and for the
  common feeder species it will be LC every time.

## 6. `esc()` will mangle the summary — this is the trap in this feature

`esc()` (`web_server.c:1301`) escapes `&`, `<` and `"`. `wikipedia_summary` **contains
markup**: `The <b>great tit</b> (<i>Parus major</i>) is a passerine bird…`. Passing it
through `esc()` renders the literal characters `<b>great tit</b>` on screen.

The wrong fix is to skip `esc()` and assign `innerHTML` — that is third-party HTML going
straight into the page.

**Do this instead:** `esc()` the whole string as normal, then re-enable exactly the two
inline tags Wikipedia uses, by replacing the *escaped* forms:

```js
function wsum(s){return esc(String(s||''))
  .replace(/&lt;(\/?)(b|i)&gt;/g,'<$1$2>');}
```

Everything else — links, scripts, attributes — stays inert as escaped text, because
`esc()` already neutralised the `<`. Note `esc()` does **not** escape `>`, which is
harmless here (a bare `>` cannot open a tag) but is why the regex matches `&gt;` on the
close side.

## 7. Restructuring `refRender` — the one real regression risk

`refRender` currently emits **one** `<a>` wrapping image + name + latin + confidence +
attribution. Splitting the name block out into a `<button onclick='spInfo()'>` means:

- The `.livref a{display:block;text-decoration:none;color:inherit}` rule and
  `.livref a:hover .rfn{text-decoration:underline}` need companion selectors for the
  button, or the name block will inherit browser button chrome (border, background,
  centred system font) inside a 108 px card.
- Keep the `if(b.dataset.v!==h)` diff guard. `tick()` runs every 2 s; re-assigning
  `innerHTML` on every poll would kill an open `:hover`/focus and re-trigger the image
  load. This guard is already there — do not lose it in the rewrite.
- **Grep the served page after editing** (`curl http://192.168.1.111/`) for `spInfo` and
  the new class names. The whole UI is one inline `<script>` assembled from C string
  literals; a single JS syntax error kills every handler while the live `<img>` keeps
  working — the classic tell, and the C compiler cannot catch it. Run the served JS
  through esprima as v2.92 did (it was 74 110 B then).

## 8. Caching

Extend v2.92's `g_ref` rather than adding a second cache. Today it stores
`{t,b,a,c}` keyed on binomial, with `{}` = "looked up, nothing usable" and `null` =
"in flight". Add:

- `id` — **capture it now**, in the existing `refShow` hit loop. Without it the panel
  needs its own `q=` search and re-runs the fuzzy-match problem from scratch.
- `det` — the detail payload, fetched **lazily on first open**, never on the 2 s poll.
  Same three-state convention (`undefined` / `null` / `{}`). **Keep the trimmed
  `taxon_photos` array on it** — `[{s:small_url, m:medium_url, a:attribution,
  p:native_page_url}]` — not the raw 76 KB record. There is no reason to hold ten full
  photo objects, `children` and `ancestors` in memory once the fields we want are read out.
- `hist` — the 12 monthly counts, same convention.
- `pi` — **the scroll index, per species.** Reopening the panel on the same bird should
  land where you left it; opening it on a *different* bird starts at 0.

**The 75 KB detail call must never fire from `tick()`.** Only from a user opening the
panel. A species identified and never clicked costs zero extra bytes, and one never
scrolled costs a single 15 KB image.

Browser image cache does the rest: stepping back to a photo already seen re-reads it from
cache, so the wrap-around navigation in §5.2 does not re-download.

## 9. ⚠ Seasonality: what the histogram actually measures

I recommended this before looking at the numbers. Having looked, it is the weakest item
here and needs either careful labelling or dropping. Both curves for *Parus major*:

| | J | F | M | A | M | J | J | A | S | O | N | D |
|---|--|--|--|--|--|--|--|--|--|--|--|--|
| **Global** | 33 964 | 32 861 | 32 311 | 29 658 | 20 107 | 12 348 | 9 704 | **7 930** | 9 782 | 14 973 | 18 230 | 22 154 |
| **Norway** (7016) | 148 | 145 | **238** | 237 | 161 | 141 | 105 | **73** | 115 | 158 | 147 | 223 |

The global curve peaks in **January**; the Norway curve peaks in **March/April**. Kjøttmeis
is **resident year-round in Norway** and does not migrate. Neither curve is bird
abundance — both are dominated by **when people go out and log observations** (global:
feeder-watching in winter; Norway: spring birding). The two disagree precisely because
the observers differ, not because the birds do.

**Therefore:**
- If shown, label it **"observations per month (iNaturalist, Norway)"** — never "when you
  will see this bird", never "season".
- It is genuinely informative for a **true migrant** (a summer visitor's curve really does
  collapse in winter). It is noise for a resident.
- **Recommendation: ship the panel without it**, add it later only if the operator wants
  it and accepts the caveat. It is 171 bytes and one line of code, so deferring costs
  nothing. Do not build a bar chart component for it.

## 10. Explicitly out of scope

- **Any device-side fetch, proxy, or cache of this data.** Non-negotiable (§4).
- **Any new HTTP route.** This adds **zero** routes — worth stating because the route
  table is capped at 48 and registrations past the cap are silently dropped.
- **Any NVS/settings key.** The panel has no persisted state (§2).
- **Rotation.** The panel, like the card, must **not** get the `--vrot` transform: that
  corrects *this* camera's mounting and would just tilt someone else's photo.

## 11. i18n

`applyLang()` (`web_server.c:1252`) walks `document.body`'s text nodes **once** and does
not re-walk dynamically inserted content — the Phase 3 limitation v2.92 already worked
around by making the card heading static markup.

- Put every **static label** — "Status", "Order", "Family", "On iNaturalist",
  "Read on Wikipedia", the panel heading — in the `#spop` **skeleton markup**, not in the
  JS that fills it, so `applyLang` catches them.
- Add one `main/i18n.txt` row per new static string (TAB-separated, English master,
  real characters not HTML entities).
- The **summary text stays English.** It is Wikipedia's, we do not translate it, and
  there is no Norwegian `wikipedia_summary` in this payload. Label the section so that is
  not read as a bug.

## 12. Failure modes — all must degrade to "less panel", never to a broken page

| Condition | Behaviour |
|---|---|
| Viewing phone has no internet | Card already hidden (v2.92); panel unreachable. No change needed. |
| Detail fetch fails / times out | Panel opens with header + photo + confidence from cache; body shows a short "details unavailable". Never an empty modal. |
| `wikipedia_summary` null | Omit the section. Several taxa have no summary. |
| `conservation_statuses` empty | Omit the row (§5.4). |
| `taxon_photos` empty or absent | Fall back to the card's `default_photo`; no arrows, no dots. |
| `taxon_photos.length === 1` | Render the photo, hide arrows and dot row — but **keep the credit**. |
| A single image 404s / fails to load | Leave the frame blank with the credit intact and let ‹ › move on. Never skip the index silently — the credit and the image must stay in step (§5.2.1). |
| `wikipedia_url` null | Hide the Wikipedia button; keep the iNat one. |
| Species has no parenthesised binomial | Cannot happen — the name block only renders when `spLatin()` returned non-empty, which is what gates the card at all. |
| Pre-SNTP boot clock | Not applicable: no date comparison anywhere in this feature. |

## 13. Release contract

1. **Code** — `main/web_server.c` (CSS + skeleton + JS), `main/i18n.txt` (new rows).
2. **`main/version.h`** → `0.74.59`. Use the **Write tool**, not an inline Python
   read-modify-write — `open(p,"w").write(open(p).read())` truncates the file.
3. **`FSD_BirdBox.md`** — header `**Version:** 2.93`, and prepend
   `- v2.93 — **Species info panel (firmware 0.74.59), §5.** …`.
4. **Verify:** build (PowerShell + `idf_tools.py`, not the Bash tool); OTA via
   `curl --data-binary`; confirm `version` flips in `/api/status`; **grep the served page**
   for the new tokens; esprima-parse the served JS; open the panel on a real ID and
   confirm the ✕/backdrop/`Esc` all dismiss it.
