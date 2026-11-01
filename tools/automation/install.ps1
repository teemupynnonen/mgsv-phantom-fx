# Installs a dev build into the automation game folder: a copy of the game made of hard links, so test
# builds never replace the add-on in the Steam folder. Run with the game closed.
param(
    [string]$Build = (Join-Path $PSScriptRoot '..\..\build\dev'),
    [string]$Game = (Join-Path $env:USERPROFILE 'Documents\Development\mgsv-automation\game'),
    [string]$Steam = 'C:\Program Files (x86)\Steam\steamapps\common\MGS_TPP',
    # The saves every run starts from; the game reads and writes a copy (src/dev/save_redirect.hpp).
    [string]$SeedSaves = (Join-Path $env:USERPROFILE 'Documents\Development\mgsv-automation\seed-saves'),
    [int]$Width = 1920,
    [int]$Height = 1080,
    # PhantomFX.ini lines in place of the player's. Frame generation and HDR are off by default: the
    # captures compare more easily without them, and changing either takes a restart.
    [string[]]$Settings = @('frame_generation=off', 'hdr=0'),
    # A whole PhantomFX.ini to run with, a debug case's, in place of the two above.
    [string]$Ini
)
$ErrorActionPreference = 'Stop'

if (Get-Process mgsvtpp -ErrorAction SilentlyContinue) { throw 'The game is running; close it first.' }

# The game's own files, linked once.
if (-not (Test-Path (Join-Path $Game 'mgsvtpp.exe'))) {
    foreach ($item in 'master', 'mgo', 'CustomSoundtrack', 'D-Mod', 'mgsvtpp.exe', 'mgsvmgo.exe', 'steam_api64.dll', 'steam_appid.txt', 'version_info.txt') {
        $path = Join-Path $Steam $item
        if (Test-Path $path -PathType Container) {
            Get-ChildItem $path -Recurse -File | ForEach-Object {
                $link = Join-Path $Game $_.FullName.Substring($Steam.Length)
                New-Item -ItemType Directory -Force (Split-Path $link) | Out-Null
                New-Item -ItemType HardLink -Path $link -Target $_.FullName | Out-Null
            }
        } else {
            New-Item -ItemType Directory -Force $Game | Out-Null
            New-Item -ItemType HardLink -Path (Join-Path $Game $item) -Target $path | Out-Null
        }
    }
}

# ReShade and the player's settings with this run's in place.
Copy-Item (Join-Path $Steam 'dxgi.dll'), (Join-Path $Steam 'ReShade.ini') $Game -Force
$replaced = $Settings | ForEach-Object { ($_ -split '=', 2)[0] }
$kept = Get-Content (Join-Path $Steam 'PhantomFX.ini') | Where-Object { ($_ -split '=', 2)[0] -notin $replaced }
$kept + $Settings | Set-Content (Join-Path $Game 'PhantomFX.ini')
if ($Ini) { Copy-Item $Ini (Join-Path $Game 'PhantomFX.ini') -Force }

Get-ChildItem $Build -File | Where-Object { $_.Extension -in '.addon64', '.dll' } | Copy-Item -Destination $Game -Force
# Ray Reconstruction's library, which the player's folder has and a build does not.
Copy-Item (Join-Path $Steam 'nvngx_dlssd.dll') $Game -Force
$saves = Join-Path $Game 'PhantomFX-automation\saves'
if (Test-Path $saves) { Remove-Item $saves -Recurse -Force }
New-Item -ItemType Directory -Force $saves | Out-Null
Copy-Item (Join-Path $SeedSaves '*') $saves

# The game's user data folder for the run: its own display settings, windowed at 1920x1080.
$userData = Join-Path $Game 'PhantomFX-automation\local'
New-Item -ItemType Directory -Force $userData | Out-Null
$config = Get-ChildItem (Join-Path $Steam '..\..\..\userdata\*\287700\local\TPP_GRAPHICS_CONFIG') | Select-Object -First 1
if (-not $config) { throw "No TPP_GRAPHICS_CONFIG in Steam's userdata: run the game once from Steam first." }
$display = Get-Content $config.FullName -Raw | ConvertFrom-Json
$display.graphics.videoout_setting.window_mode = 'Windowed'
$display.graphics.videoout_setting.width = $Width
$display.graphics.videoout_setting.height = $Height
$display | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $userData 'TPP_GRAPHICS_CONFIG')
"Installed $(Join-Path $Build 'PhantomFX.addon64') into $Game"
