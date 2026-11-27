# Builds a personal NFSUG2 APK from YOUR OWN installed copy of the game.
#
#   tools/build_apk.ps1 -GameDir "D:\Games\NFS Underground 2" [-Install] [-PushData]
#
# Nothing from the game is distributed with this project. This script:
#   1. checks the install (SPEED2.EXE v1.2 layout with its relocation section),
#   2. statically recompiles YOUR SPEED2.EXE to C++ (tools/recomp) - once, on this PC,
#   3. builds the APK (translation layer + your recompiled game code),
#   4. optionally installs it and copies your game data to the phone over ADB.
# The generated code (android/app/src/main/cpp/generated) is derived from your
# copy of the game: keep it to yourself and never publish it or the APK.
param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    [switch]$Install,
    [switch]$PushData,
    [string]$Profile = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$py = Join-Path $root ".venv\Scripts\python.exe"
$exe = Join-Path $GameDir "SPEED2.EXE"
$gen = Join-Path $root "android\app\src\main\cpp\generated"
$pkgData = "/sdcard/Android/data/com.rea.nfsug2/files/game"

if (-not (Test-Path $exe)) { throw "SPEED2.EXE not found in $GameDir" }
if (-not (Test-Path $py)) {
    python -m venv (Join-Path $root ".venv")
    & $py -m pip install -q pefile capstone
}

Write-Host "== 1/3 Recompiling $exe"
$args = @("-I", (Join-Path $root "tools\recomp\recomp.py"), $exe, "--reloc-rva", "0x4f5000", "--out", $gen)
if ($Profile -and (Test-Path $Profile)) { $args += @("--profile", $Profile) }
& $py @args
if ($LASTEXITCODE -ne 0) { throw "recompilation failed" }

Write-Host "== 2/3 Building APK"
if (-not $env:JAVA_HOME -or -not (Test-Path "$env:JAVA_HOME\bin\java.exe")) {
    $env:JAVA_HOME = "C:\Program Files\Eclipse Adoptium\jdk-21.0.11.10-hotspot"
}
Push-Location (Join-Path $root "android")
try {
    & .\gradlew.bat assembleDebug --console=plain -q
    if ($LASTEXITCODE -ne 0) { throw "gradle build failed" }
} finally { Pop-Location }
$apk = Join-Path $root "android\app\build\outputs\apk\debug\app-debug.apk"
Write-Host ("   APK: {0} ({1:N0} MB)" -f $apk, ((Get-Item $apk).Length / 1MB))

if ($Install) {
    Write-Host "== 3/3 Installing"
    adb install -r $apk | Select-Object -Last 1
    if ($PushData) {
        foreach ($item in "SPEED2.EXE", "foobar", "GLOBAL", "FRONTEND", "LANGUAGES", "CARS", "SOUND", "TRACKS",
                          "CREDITS", "NIS", "SDATA", "SUBTITLES", "MOVIES", "memcard") {
            $src = Join-Path $GameDir $item
            if (Test-Path $src) { adb push $src "$pkgData/" | Select-Object -Last 1 }
        }
    }
}
