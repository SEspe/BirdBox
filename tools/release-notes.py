#!/usr/bin/env python3
"""Print the GitHub release notes for one firmware version, built from
FSD_BirdBox_CHANGELOG.md. Used by .github/workflows/release.yml so a release
says what changed instead of a fixed template.

    python3 tools/release-notes.py 1.4.1 > notes.md

Picks every changelog entry whose title says "(firmware <version>)". Exits
non-zero if there is none, so a tag pushed without its changelog entry fails
the release instead of publishing empty notes.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CHANGELOG = ROOT / "FSD_BirdBox_CHANGELOG.md"
REPO = "https://github.com/SEspe/BirdBox"

FOOTER = """
### Update via OTA
1. Open the device web UI → **OTA Update** tab (or click the *update available* note in the header).
2. Either **Flash from GitHub release** → pick this version → *Download & flash*,
   or download `BirdBox_esp32s3_v{ver}.bin` below and use **Upload & Flash**.
3. Reopen the UI as `http://<device>/?v={nodots}` so the browser does not keep running the old cached page.

### First flash via USB
Use the browser flasher at https://SEspe.github.io/BirdBox/ or
`idf.py set-target esp32s3 && idf.py -p COMx flash`.

Full change record: [{changelog}]({repo}/blob/master/FSD_BirdBox_CHANGELOG.md)
"""


def entries_for(version: str, text: str):
    # Entries are top-level bullets: "- vX.Y — **Title (firmware A.B.C), §..** body"
    for m in re.finditer(r"^- (v\d+\.\d+) — \*\*(.+?)\*\*(.*)$", text, re.M):
        fsd, title, body = m.group(1), m.group(2), m.group(3).strip()
        fw = re.search(r"\(firmware ([0-9.]+)\)", title)
        if fw and fw.group(1) == version:
            clean = re.sub(r"\s*\(firmware [0-9.]+\).*$", "", title).rstrip(" ,.")   # drop "(firmware X), §n."
            yield fsd, clean, body


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: release-notes.py <version>")
    ver = sys.argv[1].lstrip("vV")
    found = list(entries_for(ver, CHANGELOG.read_text(encoding="utf-8")))
    if not found:
        sys.exit(f"no changelog entry says (firmware {ver}) - add one before tagging")
    out = [f"## BirdBox Firmware v{ver}", "",
           "Built by CI with ESP-IDF v6.0.1 from this tag. "
           "`BirdBox_esp32s3_v" + ver + ".bin` is for ESP32-S3 boards.", ""]
    for fsd, title, body in found:
        out += [f"### {title} (FSD {fsd})", "", body, ""]
    out.append(FOOTER.format(ver=ver, nodots=ver.replace(".", ""),
                             changelog="FSD_BirdBox_CHANGELOG.md", repo=REPO))
    sys.stdout.buffer.write("\n".join(out).encode("utf-8"))   # UTF-8 on any console


if __name__ == "__main__":
    main()
