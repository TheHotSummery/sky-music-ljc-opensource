# Flash ESP32-S3 app (+ bootloader + partition table). Does NOT flash assets.
# Usage: .\flash_app.ps1 COM5
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [string]$Baud = "460800"
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here
python -m esptool --chip esp32s3 -p $Port -b $Baud --before default_reset --after hard_reset write_flash `
  --flash_mode dio --flash_freq 80m --flash_size 16MB `
  0x0 bootloader.bin `
  0x8000 partition-table.bin `
  0x10000 esp32s3-music-keyboard.bin
Write-Host "App flash done. If first time or assets changed, run flash_assets.ps1 too."
