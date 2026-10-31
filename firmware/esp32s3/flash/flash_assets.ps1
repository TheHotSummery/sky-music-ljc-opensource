# Flash factory assets FAT image (piano / UI / demo score) to partition @ 0x310000 (N16R8 partitions.csv).
# Usage: .\flash_assets.ps1 COM5
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [string]$Baud = "460800"
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here
python -m esptool --chip esp32s3 -p $Port -b $Baud --before default_reset --after hard_reset write_flash `
  --flash_mode dio --flash_freq 80m --flash_size 16MB `
  0x310000 assets.bin
Write-Host "Assets flash done."
