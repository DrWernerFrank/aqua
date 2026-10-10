<#
    install-windows.ps1 - build, install, and add a Start Menu shortcut for aqua.

    Run from the repo:   powershell -ExecutionPolicy Bypass -File install-windows.ps1

    - Builds aqua.exe from aqua_win.c with gcc (MinGW-w64) if needed
    - Installs into %LOCALAPPDATA%\Programs\aqua  (per-user, no admin)
    - Creates an "aqua" Start Menu entry that opens your Music folder
    - aqua needs ffmpeg (ffplay/ffprobe) on PATH:  winget install Gyan.FFmpeg
#>

$ErrorActionPreference = "Stop"
$src      = $PSScriptRoot
$programs = Join-Path $env:LOCALAPPDATA "Programs"
$startDir = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
$aquaExe  = Join-Path $src "aqua.exe"

if (-not (Test-Path $aquaExe) -or $args -contains "-Rebuild") {
    if (-not (Get-Command gcc -ErrorAction SilentlyContinue)) {
        throw "gcc not found - install MinGW-w64 (e.g. winget install BrechtSanders.WinLibs.POSIX.UCRT)"
    }
    Write-Host "Building aqua.exe..."
    & gcc -O2 -Wall -o $aquaExe (Join-Path $src "aqua_win.c")
}

$aquaDir = Join-Path $programs "aqua"
New-Item -ItemType Directory -Force -Path $aquaDir | Out-Null
Copy-Item -Force $aquaExe (Join-Path $aquaDir "aqua.exe")
Copy-Item -Force (Join-Path $src "aqua.ico") (Join-Path $aquaDir "aqua.ico")

$musicDir = Join-Path $env:USERPROFILE "Music"
$sh = New-Object -ComObject WScript.Shell
$lnk = $sh.CreateShortcut((Join-Path $startDir "aqua.lnk"))
$lnk.TargetPath       = Join-Path $aquaDir "aqua.exe"
$lnk.Arguments        = "`"$musicDir`""
$lnk.WorkingDirectory = $musicDir
$lnk.IconLocation     = Join-Path $aquaDir "aqua.ico"
$lnk.Description       = "aqua - tiny terminal music player"
$lnk.Save()

Write-Host "Done. aqua is in the Start Menu (search 'aqua'); installed under $aquaDir."
