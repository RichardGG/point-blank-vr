# Starts MAME (with sound) and the PC VR host together.   UNTESTED on Windows: if a step fails, run the two commands by hand (see vr/README.md).
#   .\Play-PC.ps1 -Mame C:\mame\mame.exe -RomPath C:\roms -Roms C:\roms\ptblank -Research C:\Users\Richard\repos\point-blank-research
param(
  [Parameter(Mandatory = $true)][string]$Mame,          # mame.exe (0.264)
  [Parameter(Mandatory = $true)][string]$RomPath,       # folder that contains ptblank.zip or the ptblank folder (MAME's -rompath)
  [Parameter(Mandatory = $true)][string]$Roms,          # the same ROMs for pbvr: the ptblank folder or ptblank.zip
  [Parameter(Mandatory = $true)][string]$Research,      # the point-blank-research checkout
  [string]$Ipc = "$env:TEMP\pb_ipc",                    # folder shared by MAME and pbvr
  [string]$Pbvr = "",                                   # default: build-win\pbvr.exe next to this script
  [switch]$NoSound,
  [string[]]$PbvrArgs = @()
)
$ErrorActionPreference = 'Stop'
$here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }   # $PSScriptRoot is empty in a param default on some PowerShell versions
if (-not $Pbvr) { $Pbvr = Join-Path $here 'build-win\pbvr.exe' }
if (-not (Test-Path $Pbvr)) { throw "pbvr.exe not found at $Pbvr - build it (build_windows.sh) or pass -Pbvr" }
if (-not (Test-Path $Mame)) { throw "mame.exe not found at $Mame" }
New-Item -ItemType Directory -Force -Path $Ipc | Out-Null
Remove-Item "$Ipc\*" -Force -ErrorAction SilentlyContinue
$env:PB_IPC = $Ipc; $env:PB_EVERY = '2'
$lua = Join-Path $Research 'research\tools\play\pbplay.lua'
$tables = Join-Path $Research 'research\data\tables\scene_tables.json'
$mameArgs = @('ptblank', '-rompath', $RomPath, '-video', 'none', '-skip_gameinfo', '-autoboot_script', $lua)
if ($NoSound) { $mameArgs += @('-sound', 'none') }
$p = Start-Process -FilePath $Mame -ArgumentList $mameArgs -PassThru -WindowStyle Minimized
try {
  & $Pbvr --roms $Roms --ipc $Ipc --tables $tables @PbvrArgs
} finally {
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
