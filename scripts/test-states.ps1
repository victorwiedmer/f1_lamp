<#
.SYNOPSIS
    F1 Lamp - test all states by cycling through them via the WLED HTTP API.

.DESCRIPTION
    Locks the lamp in each F1 state for a configurable number of seconds,
    then restores live mode. No reflashing required.

.PARAMETER WledIp
    IP address of your WLED device (e.g. 192.168.1.42).

.PARAMETER DelaySeconds
    How many seconds to hold each state. Default: 4.

.EXAMPLE
    .\scripts\test-states.ps1 -WledIp 192.168.1.42
    .\scripts\test-states.ps1 -WledIp 192.168.1.42 -DelaySeconds 8
#>

param(
    [Parameter(Mandatory)]
    [string]$WledIp,

    [int]$DelaySeconds = 4
)

$baseUrl = "http://$WledIp/json/um"

$states = [ordered]@{
    1 = "IDLE            (pl.12 — dim red)"
    2 = "SESSION START   (pl.20 — formation lap)"
    3 = "GREEN FLAG      (pl.15 — solid green)"
    4 = "YELLOW FLAG     (pl.16 — amber blink)"
    5 = "SAFETY CAR      (pl.13 — amber chase)"
    6 = "VIRTUAL SC      (pl.22 — VSC pulse)"
    7 = "RED FLAG        (pl.14 — fast red blink)"
    8 = "CHEQUERED       (pl.23 — white/black 30s)"
}

function Set-F1State([int]$state) {
    Invoke-RestMethod -Method Post -Uri $baseUrl `
        -ContentType "application/json" `
        -Body "{`"F1Lamp`":{`"forceState`":$state}}" `
        -ErrorAction Stop | Out-Null
}

Write-Host ""
Write-Host "F1 Lamp — state test" -ForegroundColor Cyan
Write-Host "Target: http://$WledIp" -ForegroundColor Cyan
Write-Host "Hold time: ${DelaySeconds}s per state" -ForegroundColor Cyan
Write-Host "───────────────────────────────────" -ForegroundColor DarkGray
Write-Host ""

# Verify the device is reachable
try {
    Invoke-RestMethod -Uri "http://$WledIp/json/info" -ErrorAction Stop | Out-Null
} catch {
    Write-Host "ERROR: Could not reach WLED at http://$WledIp" -ForegroundColor Red
    Write-Host "       Check the IP address and WiFi connection." -ForegroundColor Red
    exit 1
}

try {
    foreach ($num in $states.Keys) {
        $label = $states[$num]
        Write-Host "  State $num : $label" -ForegroundColor Yellow
        Set-F1State $num
        Start-Sleep -Seconds $DelaySeconds
    }
} catch {
    Write-Host "ERROR: $_" -ForegroundColor Red
} finally {
    # Always restore live mode, even if the script is interrupted
    Write-Host ""
    Write-Host "  Restoring live mode (forceState = 0)..." -ForegroundColor Green
    try { Set-F1State 0 } catch {}
    Write-Host "  Done." -ForegroundColor Green
    Write-Host ""
}
