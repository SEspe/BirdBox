# Nordic species retrain — local training-data archive

Not part of the firmware (gitignored). Source material for the retrain
scoped in `FSD_BirdBox.md` §3.2.1.

## Layout

- `lavskrike/candidate/` — real BirdBox captures visually confirmed as
  Siberian Jay (*Perisoreus infaustus* / Lavskrike), the species that
  prompted this whole effort (see FSD v1.40/v1.41). These are genuine
  domain examples (real motion blur, real edge-clipping, real color
  response) rather than synthetic approximations of them.
- `lavskrike/review/` — newly pulled captures not yet confirmed one way or
  the other; needs a look before moving to `candidate/` or discarding.
- `not-lavskrike/` — pulled captures that turned out to be something else
  (kept in case they're useful negative/other-species examples later, not
  actively curated yet).
- `manifest.txt` — device capture filenames already pulled, so the periodic
  pull doesn't re-download or re-review the same file twice.

## How it grows

A scheduled routine periodically checks the device (`192.168.1.111`) for
capture files not yet in `manifest.txt`, downloads new ones into
`lavskrike/review/`, and does a first-pass visual sort. Files that are
clearly the same species as the existing `candidate/` set move there
automatically; anything uncertain stays in `review/` for a human look.

This alone won't be enough data for a retrain — variety (different
visits/days/light/individuals) matters more than raw count, and stock
photos from iNaturalist/GBIF (§3.2.1) still supply pose/angle diversity a
single camera position can't. This archive's job is specifically to close
the domain gap (real blur/crop/color) that stock photography can't.
