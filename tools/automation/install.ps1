# Installs a dev build into the automation game folder: a copy of the game made of hard links, so test
# builds never replace the add-on in the Steam folder. Run with the game closed.
param(
    [string]$Build = (Join-Path $PSScriptRoot '..\..\build\dev'),
    [string]$Game = $(if ($env:PHANTOM_FX_AUTOMATION_GAME) { $env:PHANTOM_FX_AUTOMATION_GAME } else { Join-Path $env:USERPROFILE 'Documents\Development\mgsv-automation\game' }),
    # The player's game the copy is made from: PHANTOM_FX_GAME, else Steam's usual folder.
    [string]$Steam = $(if ($env:PHANTOM_FX_GAME) { $env:PHANTOM_FX_GAME } else { 'C:\Program Files (x86)\Steam\steamapps\common\MGS_TPP' }),
    # The saves every run starts from; the game reads and writes a copy (src/devkit/save_redirect.hpp).
    [string]$SeedSaves = (Join-Path (Split-Path $Game) 'seed-saves'),
    # No longer applied, only warned about. The copy has started at the game's defaults even with the player's
    # graphics config, so set its graphics once in its own menu; what it saves is kept (see below).
    [int]$Width = 0,
    [int]$Height = 0,
    # PhantomFX.ini lines in place of the player's. Frame generation and HDR are off by default: the
    # captures compare more easily without them, and changing either takes a restart.
    [string[]]$Settings = @('frame_generation=off', 'hdr=0'),
    # A whole PhantomFX.ini to run with, a debug case's, in place of the two above.
    [string]$Ini
)
$ErrorActionPreference = 'Stop'
if (-not $PSBoundParameters.ContainsKey('Build') -and $env:PHANTOM_FX_DEV_BUILD) { $Build = $env:PHANTOM_FX_DEV_BUILD }

if (Get-Process mgsvtpp -ErrorAction SilentlyContinue) { throw 'The game is running; close it first.' }

# The game's own files, linked once. A hard link cannot cross drives, so a copy on another drive gets real copies.
if (-not (Test-Path (Join-Path $Game 'mgsvtpp.exe'))) {
    $sameDrive = (Split-Path -Qualifier $Game) -eq (Split-Path -Qualifier $Steam)
    $place = if ($sameDrive) { { param($l, $t) New-Item -ItemType HardLink -Path $l -Target $t | Out-Null } } else { { param($l, $t) Copy-Item $t $l } }
    foreach ($item in 'master', 'mgo', 'CustomSoundtrack', 'mgsvtpp.exe', 'mgsvmgo.exe', 'steam_api64.dll', 'steam_appid.txt', 'version_info.txt') {
        $path = Join-Path $Steam $item
        if (Test-Path $path -PathType Container) {
            # A lock file belongs to whatever holds it open, and an interrupted copy resumes where it stopped.
            Get-ChildItem $path -Recurse -File | Where-Object Extension -ne '.lock' | ForEach-Object {
                $link = Join-Path $Game $_.FullName.Substring($Steam.Length)
                if ((Test-Path $link) -and -not $sameDrive -and (Get-Item $link).Length -eq $_.Length) { return }
                New-Item -ItemType Directory -Force (Split-Path $link) | Out-Null
                & $place $link $_.FullName
            }
        } else {
            New-Item -ItemType Directory -Force $Game | Out-Null
            & $place (Join-Path $Game $item) $path
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
$saves = Join-Path $Game 'DevKit\saves'
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
# The copy drops the game's startup logos, which cost eighteen seconds of every run (src/devkit/automation.cpp).
New-Item -ItemType File -Force (Join-Path $Game 'DevKit\skip-logos') | Out-Null

# The game's user data folder for the run, seeded with the player's graphics config.
$userData = Join-Path $Game 'DevKit\local'
New-Item -ItemType Directory -Force $userData | Out-Null
$config = Get-ChildItem (Join-Path $Steam '..\..\..\userdata\*\287700\local\TPP_GRAPHICS_CONFIG') | Select-Object -First 1
if (-not $config) { throw "No TPP_GRAPHICS_CONFIG in Steam's userdata: run the game once from Steam first." }
# Byte for byte: the file carries a checksum ("as") of its settings, and an edited one (windowed, another size) the game
# throws away for its defaults, Effects below Extra High among them, where the ray tracing fails (2026-11-07).
# The game reads the player's file in the copy and still starts at its defaults (2026-11-07), so settings the copy's own
# game saved, set once in its menu, are kept over the player's.
$own = Join-Path $userData 'TPP_GRAPHICS_CONFIG'
$isGameSaved = (Test-Path $own) -and (Get-FileHash $own).Hash -ne (Get-FileHash $config.FullName).Hash
if (-not $isGameSaved) { Copy-Item $config.FullName $own -Force }
if ($Width -gt 0 -or $Height -gt 0) { Write-Warning 'Width and Height are no longer applied: an edited TPP_GRAPHICS_CONFIG resets the game to its defaults.' }
"Installed $(Join-Path $Build 'PhantomFX.addon64') into $Game"
