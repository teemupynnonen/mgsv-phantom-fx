# Fails (exit 1) when a binary can compile HLSL at run time, or when source outside the development-only files names
# the compiler. A release build ships bytecode fxc made at build time; D3DCompile on a game draw thread froze the game.
#
#   -Binary  files to scan: a DLL, an EXE or a static library. Any name starting with D3DCompile counts
#            (D3DCompile2, D3DCompileFromFile, ...); D3DReflect, which only reads bytecode, does not.
#   -Source  a directory to scan for the same name in .cpp, .hpp and .h files, apart from the files that
#            are for development builds only.
param(
    [string[]]$Binary = @(),
    [string]$Source = ""
)

# `-File` passes one string per parameter, so several files come comma separated.
$Binary = @($Binary | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$name = "D3DCompile"
$failed = $false

foreach ($file in $Binary) {
    if (-not (Test-Path -LiteralPath $file)) { Write-Error "Not found: $file"; $failed = $true; continue }
    # Latin1 maps each byte to one character, so an import name is found as the ASCII it is.
    $text = [System.Text.Encoding]::GetEncoding(28591).GetString([System.IO.File]::ReadAllBytes($file))
    if ($text.IndexOf($name, [System.StringComparison]::Ordinal) -ge 0) {
        Write-Error "$file refers to $name*: it can compile HLSL at run time. Precompile with phantom_precompile_shader."
        $failed = $true
    }
}

if ($Source) {
    # The only files allowed to name it: the development-only compile, their tests, and this check's docs.
    $allowed = @("d3d11_helpers.hpp", "shader_source.hpp", "shader_source.cpp")
    $hits = Get-ChildItem -LiteralPath $Source -Recurse -File |
        Where-Object { @(".cpp", ".hpp", ".h") -contains $_.Extension } |
        Where-Object { $_.FullName -notmatch "[\\/](build[^\\/]*|_deps)[\\/]" -and $allowed -notcontains $_.Name } |
        Select-String -SimpleMatch -CaseSensitive $name
    foreach ($hit in $hits) {
        Write-Error "$($hit.Path):$($hit.LineNumber) names $name outside the development-only files."
        $failed = $true
    }
}

if ($failed) { exit 1 }
Write-Output "No run-time shader compile."
