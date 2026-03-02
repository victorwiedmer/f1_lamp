Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot    = Split-Path -Parent $PSScriptRoot
$WledDir     = Join-Path $RepoRoot "wled"
$UsermodSrcDir = Join-Path $RepoRoot "usermod"
$PioOverride = Join-Path $RepoRoot "platformio_override.ini"

function Write-Ok($msg)   { Write-Host "  [OK]   $msg" -ForegroundColor Green }
function Write-Info($msg) { Write-Host "  [-->]  $msg" -ForegroundColor Cyan }
function Write-Warn($msg) { Write-Host "  [WARN] $msg" -ForegroundColor Yellow }
function Write-Fail($msg) { Write-Host "  [FAIL] $msg" -ForegroundColor Red; exit 1 }

Write-Host ""
Write-Host "=======================================" -ForegroundColor Magenta
Write-Host "  F1 Lamp - WLED setup script          " -ForegroundColor Magenta
Write-Host "=======================================" -ForegroundColor Magenta
Write-Host ""

Write-Info "Checking prerequisites..."

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Fail "git not found. Install from https://git-scm.com and re-run."
}
Write-Ok "git found"

if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    Write-Fail "Python not found. Install Python 3 and re-run."
}
Write-Ok "Python found"

if (-not (Get-Command pio -ErrorAction SilentlyContinue)) {
    Write-Warn "PlatformIO (pio) not found. Installing via pip..."
    python -m pip install platformio
    if ($LASTEXITCODE -ne 0) { Write-Fail "pip install platformio failed." }
    Write-Ok "PlatformIO installed"
} else {
    Write-Ok "PlatformIO found"
}

Write-Host ""
Write-Info "Setting up WLED source..."

if (Test-Path (Join-Path $WledDir ".git")) {
    Write-Ok "WLED already cloned - skipping"
} else {
    Write-Info "Cloning WLED (this may take a minute)..."
    git clone https://github.com/Aircoookie/WLED.git $WledDir
    if ($LASTEXITCODE -ne 0) { Write-Fail "git clone failed." }
    Write-Ok "WLED cloned"
}

Write-Host ""
Write-Info "Copying F1LampUsermod library into wled/usermods/..."

# Modern WLED keeps usermods as libraries under wled/usermods/<Name>/
$TargetDir = Join-Path $WledDir "usermods\F1LampUsermod"
New-Item -ItemType Directory -Force $TargetDir | Out-Null

@("F1LampUsermod.h", "F1LampUsermod.cpp", "library.json") | ForEach-Object {
    $src = Join-Path $UsermodSrcDir $_
    if (Test-Path $src) {
        Copy-Item -Force $src $TargetDir
        Write-Ok "Copied $_ -> wled/usermods/F1LampUsermod/"
    } else {
        Write-Warn "$_ not found in usermod/ - skipping"
    }
}

Write-Host ""
Write-Info "Copying platformio_override.ini..."
Copy-Item -Force $PioOverride $WledDir
Write-Ok "Copied platformio_override.ini -> wled/"

Write-Host ""
Write-Host "=======================================" -ForegroundColor Green
Write-Host "  Setup complete!" -ForegroundColor Green
Write-Host "=======================================" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:" -ForegroundColor White
Write-Host "  1. Connect your ESP32-C3 via USB"
Write-Host "  2. Run:" -ForegroundColor Yellow
Write-Host "       cd wled" -ForegroundColor Yellow
Write-Host "       pio run -e esp32c3_f1lamp --target upload" -ForegroundColor Yellow
Write-Host ""
Write-Host "     (First build downloads the ESP32 toolchain - takes ~10 min)"
Write-Host "  3. On first boot: connect to the WLED-AP hotspot and set your WiFi"
Write-Host "  4. In WLED -> Settings -> Usermods, assign preset IDs to each F1 state"
Write-Host ""