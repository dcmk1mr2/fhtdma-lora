# Build the firmware once and flash it to every connected board.
#
#   .\upload_all.ps1                      # all boards found (Silicon Labs CP210x USB ports)
#   .\upload_all.ps1 -Ports COM3,COM5     # only these ports
#   .\upload_all.ps1 -Env lora32_v21      # a different board type
#
# If Windows blocks the script, run it as:
#   powershell -ExecutionPolicy Bypass -File .\upload_all.ps1
# or double-click upload_all.bat.
#
# Close every serial monitor first, or the upload to that port fails.

param(
    [string[]]$Ports = @(),
    [string]$Env = "heltec_v3"
)

$ErrorActionPreference = "Stop"
Set-Location -Path $PSScriptRoot

# Find PlatformIO: on PATH, or where the VS Code extension installs it.
$pio = (Get-Command pio -ErrorAction SilentlyContinue).Source
if (-not $pio) {
    $pio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
    if (-not (Test-Path $pio)) {
        Write-Host "PlatformIO not found. Install the PlatformIO extension in VS Code first." -ForegroundColor Red
        exit 1
    }
}

# Find the boards: CP210x USB-serial chips (USB ID 10C4:EA60) used by Heltec and LilyGO.
if ($Ports.Count -eq 0) {
    $devices = & $pio device list --json-output | ConvertFrom-Json
    $Ports = @($devices | Where-Object { $_.hwid -match "10C4:EA60" } | ForEach-Object { $_.port })
    if ($Ports.Count -eq 0) {
        Write-Host "No CP210x boards found. Connected serial ports:" -ForegroundColor Yellow
        $devices | ForEach-Object { Write-Host ("  {0}  {1}" -f $_.port, $_.description) }
        Write-Host "Run again with -Ports COMx,COMy to pick them yourself."
        exit 1
    }
}
Write-Host ("Boards: " + ($Ports -join ", ")) -ForegroundColor Cyan

Write-Host "Building $Env ..." -ForegroundColor Cyan
& $pio run -e $Env
if ($LASTEXITCODE -ne 0) { Write-Host "Build failed." -ForegroundColor Red; exit 1 }

$ok = @(); $failed = @()
foreach ($port in $Ports) {
    Write-Host "`n=== Uploading to $port ===" -ForegroundColor Cyan
    & $pio run -e $Env -t upload --upload-port $port
    if ($LASTEXITCODE -eq 0) { $ok += $port } else { $failed += $port }
}

Write-Host ""
if ($ok.Count)     { Write-Host ("Uploaded:  " + ($ok -join ", ")) -ForegroundColor Green }
if ($failed.Count) {
    Write-Host ("Failed:    " + ($failed -join ", ")) -ForegroundColor Red
    Write-Host "For a failed port: close its serial monitor, or hold PRG while the upload starts."
    exit 1
}
Write-Host "Open a monitor with:  & `"$pio`" device monitor -p COMx -b 115200"
