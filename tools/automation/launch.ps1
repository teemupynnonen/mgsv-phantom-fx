# Starts the automation copy of the game and waits for it to close. Its display settings are its own
# (install.ps1 writes them, src/dev/save_redirect.hpp points the game at them).
param(
    [string]$Game = (Join-Path $env:USERPROFILE 'Documents\Development\mgsv-automation\game'),
    [string]$Steam = 'C:\Program Files (x86)\Steam\steamapps\common\MGS_TPP',
    # Off-screen by default: the game keeps rendering there (not when minimised), out of the way.
    [switch]$Visible
)
$ErrorActionPreference = 'Stop'

Add-Type -Namespace Automation -Name Window -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetWindowPos(System.IntPtr window, System.IntPtr after, int x, int y, int width, int height, uint flags);
[StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr window, out Rect rect);
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr window);
'@
$NoSizeNoOrderNoActivate = 0x15

if (Get-Process mgsvtpp -ErrorAction SilentlyContinue) { throw 'The game is already running.' }

# The player's saves, their backups and display settings. The add-on keeps the run away from them; if
# anything changes anyway, it is put back from this copy when the game exits.
$userData = Join-Path $Steam '..\..\..\userdata'
$playerFolders = Get-ChildItem (Join-Path $userData '*\287700\local'), (Join-Path $userData '*\311340\remote') -Directory
$snapshot = Join-Path (Split-Path $Game) 'player-files-snapshot'
if (Test-Path $snapshot) { Remove-Item $snapshot -Recurse -Force }
$playerFiles = foreach ($folder in $playerFolders) {
    $copy = Join-Path $snapshot ($folder.Parent.Name + '-' + $folder.Name)
    New-Item -ItemType Directory -Force $copy | Out-Null
    Get-ChildItem $folder.FullName -File | ForEach-Object {
        Copy-Item $_.FullName $copy
        [pscustomobject]@{ Path = $_.FullName; Copy = Join-Path $copy $_.Name; Hash = (Get-FileHash $_.FullName).Hash }
    }
}

$player = [Automation.Window]::GetForegroundWindow()
$process = Start-Process (Join-Path $Game 'mgsvtpp.exe') -WorkingDirectory $Game -PassThru
"Started the game, process $($process.Id)"
# The game places its window as it starts, so it is moved again until it stays put. Off-screen it
# should not take the keyboard either, so the window that had the focus gets it back.
for ($second = 0; -not $Visible -and $second -lt 60 -and -not $process.HasExited; $second++) {
    Start-Sleep -Seconds 1
    $process.Refresh()
    $rect = New-Object Automation.Window+Rect
    if ($process.MainWindowHandle -ne 0 -and [Automation.Window]::GetWindowRect($process.MainWindowHandle, [ref]$rect) -and $rect.Left -gt -1000) {
        [Automation.Window]::SetWindowPos($process.MainWindowHandle, [System.IntPtr]::Zero, -6000, 100, 0, 0, $NoSizeNoOrderNoActivate) | Out-Null
    }
    if ($process.MainWindowHandle -ne 0 -and [Automation.Window]::GetForegroundWindow() -eq $process.MainWindowHandle) {
        [Automation.Window]::SetForegroundWindow($player) | Out-Null
    }
}
$process.WaitForExit()
"The game exited with $($process.ExitCode)"

$changed = $playerFiles | Where-Object { -not (Test-Path $_.Path) -or (Get-FileHash $_.Path).Hash -ne $_.Hash }
foreach ($file in $changed) { Copy-Item $file.Copy $file.Path -Force }
$added = foreach ($folder in $playerFolders) { Get-ChildItem $folder.FullName -File | Where-Object { $_.FullName -notin $playerFiles.Path } }
if ($changed -or $added) {
    "WARNING: the run changed the player's files. Put back: $($changed.Path -join ', '). New, left in place: $($added.FullName -join ', ')"
} else {
    "The player's $($playerFiles.Count) files are unchanged"
}
