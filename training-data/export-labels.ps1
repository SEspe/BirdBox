# Exports the BirdBox's human-confirmed species labels (set via the Gallery
# relabel UI, FSD v1.51) into a training-ready dataset for the Nordic retrain
# (FSD v1.57 / §3.2.1).
#
# Pulls GET /api/labels/confirmed, downloads each confirmed capture into
#   training-data/dataset/<class>/<file.jpg>
# (one folder per species, keyed on the Latin binomial when known, else the
# common name), and writes training-data/dataset/labels.csv as the manifest a
# fine-tune script consumes directly.
#
# Idempotent: already-downloaded files are skipped, so re-run it any time more
# labels have accumulated. Mechanical only - the species judgment was already
# made by the human in the relabel UI, so nothing here needs a visual review.

param(
    # Device base URL. Precedence: -Device arg > $env:BIRDBOX_DEVICE > default.
    # The reference unit's LAN IP changes when it's relocated, so override
    # without editing this file, e.g.:
    #   .\export-labels.ps1 -Device http://192.168.10.236
    #   $env:BIRDBOX_DEVICE = 'http://192.168.10.236'   # persists for the shell
    [string]$Device = $(if ($env:BIRDBOX_DEVICE) { $env:BIRDBOX_DEVICE } else { 'http://192.168.1.111' })
)

$ErrorActionPreference = "Stop"
$Root    = "D:\SteinsRootMappe\Claude\BirdBox\training-data"
$OutDir  = "$Root\dataset"
$Labels  = "$OutDir\labels.csv"

# Turn a species name into a safe folder name: spaces/punctuation -> "_".
function Sanitize([string]$s) {
    if ([string]::IsNullOrWhiteSpace($s)) { return "_unknown" }
    ($s -replace '[^A-Za-z0-9]+', '_').Trim('_')
}

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

Write-Output "Fetching confirmed labels from $Device ..."
$rows = Invoke-RestMethod -Uri "$Device/api/labels/confirmed" -TimeoutSec 30
if (-not $rows) { Write-Output "No confirmed labels on the device yet - relabel some captures first."; return }

# (Re)write the manifest header; rows are appended as we go.
"file,common,latin,timestamp,relpath" | Out-File -FilePath $Labels -Encoding utf8

$pulled = 0; $skipped = 0; $missing = 0; $excluded = 0
$counts = @{}

foreach ($r in $rows) {
    $capPath = [string]$r.f          # e.g. /captures/2026-07-10/2026-07-10_21-30-08-366.jpg
    $common  = [string]$r.c
    $latin   = [string]$r.l
    $ts      = [string]$r.ts
    if ([string]::IsNullOrWhiteSpace($capPath)) { continue }

    # "unknown" = a bird IS present but is unidentifiable or too blurry to use
    # (gallery state 6). It's neither a usable species positive nor a valid hard
    # negative (a bird must never train the detector to suppress birds), so it is
    # excluded from the dataset entirely. "other" (cat/sheep, state 5) is kept:
    # it sanitizes to an `other/` folder, used as a hard-negative distractor class.
    if ($common -eq 'unknown') { $excluded++; continue }

    $fname = Split-Path $capPath -Leaf
    $class = if (-not [string]::IsNullOrWhiteSpace($latin)) { Sanitize $latin } else { Sanitize $common }
    $classDir = Join-Path $OutDir $class
    if (-not (Test-Path $classDir)) { New-Item -ItemType Directory -Path $classDir | Out-Null }

    $dest    = Join-Path $classDir $fname
    $relpath = "$class/$fname"

    if (-not (Test-Path $dest)) {
        try {
            Invoke-WebRequest -Uri "$Device$capPath" -OutFile $dest -TimeoutSec 30
            $pulled++
        } catch {
            Write-Warning "Missing on device (row kept in manifest): $capPath"
            $missing++
        }
    } else {
        $skipped++
    }

    # CSV-quote each field (species names are comma-sanitized on-device, but be safe).
    $line = @($fname, $common, $latin, $ts, $relpath) |
            ForEach-Object { '"' + ($_ -replace '"', '""') + '"' }
    ($line -join ",") | Add-Content -Path $Labels -Encoding utf8

    if ($counts.ContainsKey($class)) { $counts[$class]++ } else { $counts[$class] = 1 }
}

Write-Output ""
Write-Output "Done. $pulled new, $skipped already had, $missing missing on device, $excluded excluded (unknown/bad-bird)."
Write-Output "Dataset: $OutDir"
Write-Output "Manifest: $Labels"
Write-Output ""
Write-Output "Per-class image counts:"
$counts.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object {
    "  {0,-32} {1}" -f $_.Key, $_.Value | Write-Output
}
