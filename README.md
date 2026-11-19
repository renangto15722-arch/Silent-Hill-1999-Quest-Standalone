<div align="center">

# Silent Hill VR
### Quest 3 Standalone

*The original Silent Hill (1999), playable in VR natively on the Meta Quest 3 — no PC needed.*

![Status](https://img.shields.io/badge/status-work%20in%20progress-orange)
![Platform](https://img.shields.io/badge/platform-Meta%20Quest%203-1c1e21)
![Version](https://img.shields.io/badge/version-0.2-blue)
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
| 🔫 **Holsters** | Hips, chest and shoulders: grip to draw or put away a weapon. Each holster shows a see-through grab box, sized to your body |
| 🎯 **Weapon wheel** | Optional instead of the holsters: hold the right grip, reach for a weapon, release |
| ✋ **Both hands** | Any weapon in either hand; guns aim and fire from the hand that holds them |
| 🔴 **Laser sight** | The shot goes where the laser points |
| ⚡ **~72 fps** | Optimized for the Quest 3, with sharper eye images |
| 🧱 **Improved PGXP** | Steadier geometry and textures, less PS1 wobble |

## 📦 What is included

| File | |
|---|---|
| [`SilentHillVR-Quest-0.2.apk`](SilentHillVR-Quest-0.2.apk) | The game (`com.sh1vr.quest`). Everything it needs is inside; only the disc is yours to add. |
| [`Install-SilentHillVR.bat`](Install-SilentHillVR.bat) | Installs it from a Windows PC over USB. |
| [`InstallSH-Quest.bat`](InstallSH-Quest.bat) | Same, drag & drop: installs or updates the game, then copies whatever you drag into its window (disc, HD textures, HD maps). |
| [`SilentHillVR-Quest-0.2-source/`](SilentHillVR-Quest-0.2-source) | Full source code. APK build instructions: [`quest/README.md`](SilentHillVR-Quest-0.2-source/quest/README.md) |

## 💿 What you need

- Your own **Silent Hill disc (USA, SLUS-00707)** as `.bin` + `.cue`, named exactly:
  ```
  Silent Hill (USA).bin
  Silent Hill (USA).cue
  ```
- *(Optional)* **HD textures:** [Silent Hill DuckStation HD Textures 2.0](https://gbatemp.net/threads/silent-hill-duckstation-hd-textures-2-0.672174/) (`.zip`).
- *(Optional)* **HD maps:** [HD Maps (Google Drive)](https://drive.google.com/drive/folders/1fYSrGnh8y0qOu1cEa4MbIja8Co-c-fje?usp=sharing).

## 📂 Where each file goes on the Quest

Everything lives in **`/sdcard/SilentHillVR/gamedata/`** on the Quest. From a PC with the cable: *This PC → Quest 3 → Internal shared storage → SilentHillVR → gamedata*.

| What | Put it in | How |
|---|---|---|
| 💿 Disc `Silent Hill (USA).bin` + `.cue` | `/sdcard/SilentHillVR/gamedata/` | as they are |
| 🖼️ HD textures (`.zip`) | `/sdcard/SilentHillVR/gamedata/texturemods/` | **keep it zipped** |
| 🗺️ HD maps | `/sdcard/SilentHillVR/gamedata/load/` | the downloaded folders (`TIM`, ...) go inside `load` |

> [!TIP]
> The `SilentHillVR` folder appears after the game's first start. `Install-SilentHillVR.bat` creates it and copies everything for you.

## 🥽 Install without a PC — Quest only

1. Turn on **Developer Mode**: in the Meta Horizon phone app, *Devices → your headset → Headset settings → Developer Mode* (it asks for a free Meta developer account once).
2. In the **Quest browser**, open this page and download [`SilentHillVR-Quest-0.2.apk`](SilentHillVR-Quest-0.2.apk). Open the Quest's **Files** app → *Downloads*, tap the APK and install it.
3. With the Quest browser, download your own disc (`.bin` + `.cue`, or a `.zip` that has them). *(Optional)* Download the HD textures `.zip` and the HD maps (as a `.zip`) the same way. They all land in the *Download* folder.
4. Start the game: **Library → Unknown Sources**. Allow **"All files access"** when the panel asks, then come back.
   The game moves the disc and the HD packs from *Download* into place by itself, then starts.

> [!NOTE]
> If the Files app will not install the APK on your Quest, install it from an Android phone with a USB cable (the **Bugjaeger** app) or from a PC (below). Steps 3 and 4 stay the same.

## ⚡ Easy install — Windows PC + USB cable

1. *(Optional)* Put `Silent Hill (USA).bin` and `.cue` next to `Install-SilentHillVR.bat` (or in a folder named `disc`), the HD texture pack `.zip` in a folder named `texturemods`, and the HD maps' files in a folder named `load`.
2. Connect the Quest (Developer Mode on) and run **`Install-SilentHillVR.bat`**.
   It gets adb if needed (Google's official platform-tools), installs the game, grants its storage permission and copies your disc, textures and maps.
3. Start the game on the Quest: **Library → Unknown Sources**.

**Or drag & drop:** put `InstallSH-Quest.bat` next to the APK, run it, and drag your disc (`.bin` + `.cue`), the HD texture pack `.zip` and the HD maps (`.zip` or folder) into its window, one by one. It installs or updates the game first and never deletes your saves, settings or packs.

<details>
<summary><b>🛠️ Manual installation</b></summary>

1. Install `SilentHillVR-Quest-0.2.apk` with SideQuest (or `adb install`).
2. Start the game on the Quest (**Library → Unknown Sources**).
   The first time, a panel asks for **"All files access"**: turn it on and come back; the game goes on by itself.
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
5. *(Optional)* HD maps: put the downloaded folders (`TIM`, ...) in:
   ```
   /sdcard/SilentHillVR/gamedata/load/
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
