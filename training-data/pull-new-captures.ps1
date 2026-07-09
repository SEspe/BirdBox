# Pulls BirdBox capture files not yet in manifest.txt into review/ for
# later visual sorting into lavskrike/candidate or not-lavskrike.
# Mechanical only - no species judgment here, that needs a look (by eye or
# by asking Claude to review training-data/lavskrike/review/ in a session).
$ErrorActionPreference = "Stop"
$Device   = "http://192.168.1.111"
$Root     = "D:\SteinsRootMappe\Claude\BirdBox\training-data"
$Manifest = "$Root\manifest.txt"
$ReviewDir = "$Root\lavskrike\review"

if (-not (Test-Path $Manifest)) { New-Item -ItemType File -Path $Manifest | Out-Null }
$seen = Get-Content $Manifest | Where-Object { $_.Trim() -ne "" }
$seenSet = @{}
foreach ($f in $seen) { $seenSet[$f] = $true }

$days = Invoke-RestMethod -Uri "$Device/api/days" -TimeoutSec 15
$newCount = 0

foreach ($day in $days) {
    $date = $day.d
    $events = Invoke-RestMethod -Uri "$Device/api/events?date=$date" -TimeoutSec 15
    foreach ($ev in $events) {
        $fname = $ev.f
        if ($seenSet.ContainsKey($fname)) { continue }

        $url  = "$Device/captures/$date/$fname"
        $dest = "$ReviewDir\$fname"
        try {
            Invoke-WebRequest -Uri $url -OutFile $dest -TimeoutSec 20
            Add-Content -Path $Manifest -Value $fname
            $seenSet[$fname] = $true
            $newCount++
            Write-Output "Pulled: $date/$fname"
        } catch {
            Write-Warning "Failed to pull $date/$fname : $_"
        }
    }
}

Write-Output "Done. $newCount new file(s) in $ReviewDir"
