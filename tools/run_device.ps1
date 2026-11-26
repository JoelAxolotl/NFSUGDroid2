# Build the APK, install it on the ADB device (keeping app data), launch the
# game and print the tail of the native log.
#
#   tools/run_device.ps1 [-NoBuild] [-Seconds 15] [-Tail 80] [-Trace]
#
# Never uninstall the app: that deletes /sdcard/Android/data/com.rea.nfsug2,
# including the pushed game files.
param(
    [switch]$NoBuild,
    [int]$Seconds = 15,
    [int]$Tail = 80,
    [switch]$Trace
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$android = Join-Path $root "android"
$apk = Join-Path $android "app\build\outputs\apk\debug\app-debug.apk"
$pkg = "com.rea.nfsug2"
$userDir = "/sdcard/Android/data/$pkg/files/user"

if (-not $NoBuild) {
    if (-not $env:JAVA_HOME -or -not (Test-Path "$env:JAVA_HOME\bin\java.exe")) {
        $env:JAVA_HOME = "C:\Program Files\Eclipse Adoptium\jdk-21.0.11.10-hotspot"
    }
    Push-Location $android
    try {
        & .\gradlew.bat assembleDebug --console=plain -q
        if ($LASTEXITCODE -ne 0) { throw "gradle build failed" }
    } finally { Pop-Location }
}

adb install -r $apk | Select-Object -Last 1
if ($Trace) { adb shell "touch $userDir/trace_hle" } else { adb shell "rm -f $userDir/trace_hle" }
adb shell am force-stop $pkg
adb logcat -c
adb shell am start -n "$pkg/.LauncherActivity" --ez autostart true | Out-Null
Start-Sleep -Seconds $Seconds
adb shell "tail -n $Tail $userDir/log.txt"
