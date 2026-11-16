<div align="center">

# Silent Hill VR
### Quest 3 Standalone

*The original Silent Hill (1999), playable in VR natively on the Meta Quest 3 — no PC needed.*

![Status](https://img.shields.io/badge/status-work%20in%20progress-orange)
![Platform](https://img.shields.io/badge/platform-Meta%20Quest%203-1c1e21)
![Version](https://img.shields.io/badge/version-0.1-blue)
![License](https://img.shields.io/badge/license-GPL--3.0-green)

</div>

> [!WARNING]
> **Work in progress.** Expect bugs and missing features.

---

## ✨ Features

| | |
|---|---|
| 🥽 **Standalone** | Runs natively on the Quest 3, no PC or streaming |
| 🎮 **6DoF motion controls** | Head and both hands tracked, roomscale |
| 🔫 **Holsters** | Hips, chest and shoulders: grip to draw or put away a weapon |
| 🧱 **Improved PGXP** | Steadier geometry and textures, less PS1 wobble |

## 📦 What is included

| File | |
|---|---|
| [`SilentHillVR-Quest-0.1.apk`](SilentHillVR-Quest-0.1.apk) | The game (`com.sh1vr.quest`). Everything it needs is inside; only the disc is yours to add. |
| [`Install-SilentHillVR.bat`](Install-SilentHillVR.bat) | Installs it from a Windows PC over USB. |
| [`SilentHillVR-Quest-0.1-source/`](SilentHillVR-Quest-0.1-source) | Full source code. APK build instructions: [`quest/README.md`](SilentHillVR-Quest-0.1-source/quest/README.md) |

## 💿 What you need

- Your own **Silent Hill disc (USA, SLUS-00707)** as `.bin` + `.cue`, named exactly:
  ```
  Silent Hill (USA).bin
  Silent Hill (USA).cue
  ```
- *(Optional)* An HD texture pack in DuckStation format (`.zip`).

## ⚡ Easy install — Windows PC + USB cable

1. *(Optional)* Put `Silent Hill (USA).bin` and `.cue` next to `Install-SilentHillVR.bat` (or in a folder named `disc`), and an HD texture pack `.zip` in a folder named `texturemods`.
2. Connect the Quest (Developer Mode on) and run **`Install-SilentHillVR.bat`**.
   It gets adb if needed (Google's official platform-tools), installs the game, grants its storage permission and copies your disc/textures.
3. Start the game on the Quest: **Library → Unknown Sources**.

<details>
<summary><b>🛠️ Manual installation</b></summary>

1. Install `SilentHillVR-Quest-0.1.apk` with SideQuest (or `adb install`).
2. Start the game on the Quest (**Library → Unknown Sources**).
   The first time, a panel asks for **"All files access"**: turn it on and start the game again.
   The game then creates `/sdcard/SilentHillVR` with everything it needs.
3. With the Quest connected to the PC, put your disc's `.bin` and `.cue` in:
   ```
   /sdcard/SilentHillVR/gamedata/
   ```
   and start the game.
4. *(Optional)* HD textures: put the pack's `.zip`, still zipped, in:
   ```
   /sdcard/SilentHillVR/gamedata/texturemods/
   ```

</details>

## 🎮 Main controls

| Input | Action |
|---|---|
| **X** (hold) | Item screen |
| **X** (tap) | Reload / close the item screen |
| **Y** (hold) | PC options (VR, VR_Tuning, VR_Comfort… pages) |
| **A** (hold 2 s) | Wheel with healing items and the map |
| **Grip** near hip / chest / shoulder | Draw or holster the weapon |
| **Trigger** | Shoot / attack |

> [!NOTE]
> Saves and logs live in `/sdcard/SilentHillVR`; uninstalling the app keeps them.

## 🙏 Credits

- Based on the [Silent Hill PC Port](https://github.com/SlickAmogus/silent-hill-decomp) and the Silent Hill decompilation project.
- VR hands: UltimateXR BigHands (MIT, license in `/sdcard/SilentHillVR/vrhands`).

<sub>Silent Hill is a trademark of Konami; no game files are distributed here.</sub>
