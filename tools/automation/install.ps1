# Installs a dev build into the automation game folder: a copy of the game made of hard links, so test
# builds never replace the add-on in the Steam folder. Run with the game closed.
param(
    [string]$Build = (Join-Path $PSScriptRoot '..\..\build\dev'),
    [string]$Game = (Join-Path $env:USERPROFILE 'Documents\Development\mgsv-automation\game'),
    [string]$Steam = $(if ($env:PHANTOM_FX_GAME) { $env:PHANTOM_FX_GAME } else { 'C:\Program Files (x86)\Steam\steamapps\common\MGS_TPP' }),
    # The saves every run starts from; the game reads and writes a copy (src/dev/save_redirect.hpp).
    [string]$SeedSaves = (Join-Path $env:USERPROFILE 'Documents\Development\mgsv-automation\seed-saves'),
    # 0 keeps the player's own output size, so a case recorded in their game has the render size the copy
    # reproduces (a smaller window renders smaller still under an upscaler).
    [int]$Width = 0,
    [int]$Height = 0,
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
$Runtime = Join-Path $Build 'PhantomFX/D3D12'
if (Test-Path $Runtime) {
    $RuntimeTarget = Join-Path $Game 'PhantomFX/D3D12'
    New-Item -ItemType Directory -Force $RuntimeTarget | Out-Null
    Copy-Item (Join-Path $Runtime '*') $RuntimeTarget -Recurse -Force
}
# Ray Reconstruction's library, which the player's folder has and a build does not.
Copy-Item (Join-Path $Steam 'nvngx_dlssd.dll') $Game -Force
$saves = Join-Path $Game 'PhantomFX-automation\saves'
# The game keeps its graphics, audio and language settings in TPP_CONFIG_DATA among the saves, and the copy's own game
# writes the one set in its menu there. Seeding the saves replaced it with the seed's, so a copy set to the maximum came back
# below Extra High, where the path tracing fails (2026-11-07). It is kept over the seed when the copy has one.
$ownConfig = Join-Path $env:TEMP 'phantom-fx-copy-TPP_CONFIG_DATA'
$keepConfig = Test-Path (Join-Path $saves 'TPP_CONFIG_DATA')
if ($keepConfig) { Copy-Item (Join-Path $saves 'TPP_CONFIG_DATA') $ownConfig -Force }
if (Test-Path $saves) { Remove-Item $saves -Recurse -Force }
New-Item -ItemType Directory -Force $saves | Out-Null
Copy-Item (Join-Path $SeedSaves '*') $saves
if ($keepConfig) { Copy-Item $ownConfig (Join-Path $saves 'TPP_CONFIG_DATA') -Force }

# The game's user data folder for the run, with the player's display and quality settings.
$userData = Join-Path $Game 'PhantomFX-automation\local'
New-Item -ItemType Directory -Force $userData | Out-Null
$config = Get-ChildItem (Join-Path $Steam '..\..\..\userdata\*\287700\local\TPP_GRAPHICS_CONFIG') | Select-Object -First 1
if (-not $config) { throw "No TPP_GRAPHICS_CONFIG in Steam's userdata: run the game once from Steam first." }
# Byte for byte: the file carries a checksum ("as") of its settings, and an edited one (windowed, another size) the game
# throws away for its defaults, Effects below Extra High among them, where the ray tracing fails (2026-11-07). The copy
# runs in the player's own display mode.
# The game reads the player's file in the copy and still starts at its defaults (2026-11-07), so settings the copy's own
# game saved, set once in its menu, are kept over the player's.
$own = Join-Path $userData 'TPP_GRAPHICS_CONFIG'
$isGameSaved = (Test-Path $own) -and (Get-FileHash $own).Hash -ne (Get-FileHash $config.FullName).Hash
if (-not $isGameSaved) { Copy-Item $config.FullName $own -Force }
if ($Width -gt 0 -or $Height -gt 0) { Write-Warning 'Width and Height are no longer applied: an edited TPP_GRAPHICS_CONFIG resets the game to its defaults.' }
"Installed $(Join-Path $Build 'PhantomFX.addon64') into $Game"
