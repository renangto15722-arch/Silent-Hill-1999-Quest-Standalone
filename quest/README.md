# Silent Hill VR - Meta Quest standalone (Android project)

Gradle project that builds the native game (`../pc_port`, with the VR code in
`pc_port/src/vr_*.c`) into a Quest APK (`com.sh1vr.quest`). It runs fully on
the headset through OpenXR (no PC streaming).

## Requirements

- Android SDK with NDK **27.1.12297006** and CMake **3.31.6**
- JDK 17 or newer (built with Temurin 21)
- Windows host (the NDK tool paths in `app/build.gradle` use `windows-x86_64`)

## Dependencies (not in the repo)

Put them in `quest/deps/`:

| Folder | What |
|---|---|
| `deps/SDL2` | SDL2 2.32 source (its `android-project/app/src/main/java` is used too) |
| `deps/openal-soft` | openal-soft 1.24.3 source |
| `deps/libjpeg-turbo` | libjpeg-turbo 3.0.4 source (headers) |
| `deps/jpeg-build` | CMake build dir of libjpeg-turbo 3.0.4 for arm64-v8a (NDK toolchain): `libjpeg.a`, `jconfig.h`, `jconfigint.h`, `jversion.h` |
| `deps/openxr` | OpenXR headers 1.1.62 in `include/`, and `lib/arm64-v8a/libopenxr_loader.so` (the Khronos Android loader, same file as `android/app/src/main/jniLibs`) |

## Build

```
cd quest/android
gradlew.bat assembleDebug
```

Output: `app/build/outputs/apk/debug/app-debug.apk`. The debug type is built
as RelWithDebInfo (-O0 cannot hold the headset's frame rate).

Signing: without `android/keystore/keystore.properties` the standard Android
debug key is used. To sign with your own key, create `android/keystore/` with
your `.keystore` and a `keystore.properties`:

```
storeFile=your.keystore
storePassword=...
keyAlias=...
keyPassword=...
```

`keystore/` is ignored by git. An APK signed with another key cannot update
one already installed (uninstall first; game data in `/sdcard/SilentHillVR`
survives).

## Game data on the headset

Everything lives in `/sdcard/SilentHillVR` (the app asks for "All files
access" on first start):

```
/sdcard/SilentHillVR/config.cfg          (start from quest/data/config.cfg)
/sdcard/SilentHillVR/vrhands/            (VR hand models, see the release zip)
/sdcard/SilentHillVR/gamedata/           (pc_port/assets/gamedata + your disc)
    Silent Hill (USA).bin / .cue         (your own USA disc, SLUS-00707)
    texturemods/                         (optional DuckStation HD pack .zip)
```

No game data is included in this repository or its releases.
