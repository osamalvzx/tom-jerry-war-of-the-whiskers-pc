# WOTW Android APK builder — the scripted CLI pipeline (user decision session 28: no Gradle).
# Uses ONLY the local SDK/NDK: cmake (native .so) + aapt2 (resources+manifest) + jar (pack) +
# zipalign + apksigner (debug keystore). Produces a signed, installable debug APK.
#
#   port/tools/build_apk.ps1                 build + package -> port/build-apk/WOTW.apk
#   port/tools/build_apk.ps1 -Install        also `adb install -r` it + push game assets
#   port/tools/build_apk.ps1 -Install -SkipAssets   re-install without re-pushing assets
#   port/tools/build_apk.ps1 -Adb ip:port    connect wireless adb first, then (with -Install)
#
# Output: port/build-apk/WOTW.apk. With -Install, the game data in extracted/ is pushed to
# the app's external files dir (/sdcard/Android/data/com.wotw.port/files/extracted) — the
# stand-in until the SAF ISO picker + on-device extraction land.
param([switch]$Install, [string]$Adb = "", [switch]$SkipNative, [switch]$SkipAssets)

$ErrorActionPreference = "Stop"
$root = Split-Path (Split-Path $PSScriptRoot)          # port/tools -> project root
$sdk  = "$env:LOCALAPPDATA\Android\Sdk"
$bt   = "$sdk\build-tools\34.0.0"
$jbr  = "C:\Program Files\Android\Android Studio\jbr"
$env:JAVA_HOME = $jbr                                   # Android tools want a known-good JDK
$androidJar = "$sdk\platforms\android-34\android.jar"
$cmake = "$sdk\cmake\3.22.1\bin\cmake.exe"
$aapt2 = "$bt\aapt2.exe"
$zipalign = "$bt\zipalign.exe"
$apksigner = "$bt\apksigner.bat"
$jar = "$jbr\bin\jar.exe"
$strip = "$sdk\ndk\26.1.10909125\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe"

$appdir = "$root\port\android\app"
$build  = "$root\port\build-apk"
$stage  = "$build\stage"
Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\lib\arm64-v8a" | Out-Null

# 1) native lib -----------------------------------------------------------------------------
if (-not $SkipNative) {
    Write-Host "== building libtjapp.so (arm64-v8a) =="
    & $cmake --build "$root\port\build-arm64" --target tjapp
    if ($LASTEXITCODE -ne 0) { throw "native build failed" }
}
$so = "$root\port\build-arm64\libtjapp.so"
if (-not (Test-Path $so)) { throw "libtjapp.so not found ($so)" }
Copy-Item $so "$stage\lib\arm64-v8a\libtjapp.so" -Force
if (Test-Path $strip) { & $strip "$stage\lib\arm64-v8a\libtjapp.so" }   # shrink (debug info out)

# 2) resources + manifest -> base apk -------------------------------------------------------
Write-Host "== aapt2 compile + link =="
& $aapt2 compile --dir "$appdir\res" -o "$build\res.zip"
if ($LASTEXITCODE -ne 0) { throw "aapt2 compile failed" }
& $aapt2 link -o "$build\app-unsigned.apk" -I $androidJar `
    --manifest "$appdir\AndroidManifest.xml" `
    --min-sdk-version 24 --target-sdk-version 34 `
    "$build\res.zip"
if ($LASTEXITCODE -ne 0) { throw "aapt2 link failed" }

# 3) add the native lib (extractNativeLibs=true, so compression is fine) ---------------------
Write-Host "== packing native lib =="
& $jar uf "$build\app-unsigned.apk" -C $stage "lib/arm64-v8a/libtjapp.so"
if ($LASTEXITCODE -ne 0) { throw "jar add failed" }

# 4) align + sign (debug keystore) ----------------------------------------------------------
Write-Host "== zipalign + apksigner =="
& $zipalign -f -p 4 "$build\app-unsigned.apk" "$build\app-aligned.apk"
if ($LASTEXITCODE -ne 0) { throw "zipalign failed" }
$ks = "$env:USERPROFILE\.android\debug.keystore"
& $apksigner sign --ks $ks --ks-pass pass:android --ks-key-alias androiddebugkey `
    --key-pass pass:android --out "$build\WOTW.apk" "$build\app-aligned.apk"
if ($LASTEXITCODE -ne 0) { throw "apksigner failed" }

Write-Host "== verifying =="
& $apksigner verify --print-certs "$build\WOTW.apk" | Select-Object -First 3
& $aapt2 dump badging "$build\WOTW.apk" | Select-String "package:|application-label:|native-code|sdkVersion|uses-feature"
Write-Host "APK: $build\WOTW.apk"

# 5) optional install + game assets ---------------------------------------------------------
if ($Install) {
    $adbExe = "$sdk\platform-tools\adb.exe"
    if ($Adb -ne "") { & $adbExe connect $Adb }
    & $adbExe install -r "$build\WOTW.apk"
    if (-not $SkipAssets) {
        # Push the game data to the app's external files dir. Stand-in for the SAF picker +
        # on-device extraction (still no disc data in the APK — this is the user's own copy).
        $extDir = "/sdcard/Android/data/com.wotw.port/files"
        Write-Host "== pushing game assets to $extDir/extracted (~223 MB) =="
        & $adbExe shell mkdir -p "$extDir"
        & $adbExe push "$root\extracted" "$extDir/extracted"
    }
    Write-Host "launching..."
    & $adbExe shell am start -n com.wotw.port/android.app.NativeActivity
}
