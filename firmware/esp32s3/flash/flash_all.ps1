# Flash bootloader + partition + app + assets in one go (N16R8 / 16MB).
# Usage: .\flash_all.ps1 COM5
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
  0x10000 esp32s3-music-keyboard.bin `
  0x310000 assets.bin
Write-Host "Full flash done."
