SILENT HILL VR - QUEST 3 (standalone)
=====================================

*** WORK IN PROGRESS *** Expect bugs and missing features.

Features
--------
- Runs natively on the Quest 3 (no PC needed)
- 6DoF motion controls: head and both hands tracked, roomscale
- Holsters on the hips, chest and shoulders: grip to draw or put away
  a weapon
- Improved PGXP: steadier geometry and textures, less PS1 wobble

What is included
----------------
SilentHillVR-Quest-0.1.apk the game (app com.sh1vr.quest). Everything it needs
                         is inside; only the disc is yours to add.
Install-SilentHillVR.bat installs it from a Windows PC over USB.

Source code: SilentHillVR-Quest-0.1-source.zip (separate download) and
https://github.com/renangto15722-arch/Silent-Hill-1999-Quest-Standalone
Build instructions for the APK are in SilentHillVR-Quest-0.1-source/quest/README.md.

What YOU need
-------------
- Your own Silent Hill disc (USA, SLUS-00707) as .bin + .cue.
  The files must be named exactly:
    Silent Hill (USA).bin
    Silent Hill (USA).cue
- (Optional) An HD texture pack in DuckStation format (.zip).

Easy install (Windows PC + USB cable)
-------------------------------------
1. (Optional) Put "Silent Hill (USA).bin" and ".cue" next to
   Install-SilentHillVR.bat (or in a folder named "disc"), and an HD texture
   pack .zip in a folder named "texturemods".
2. Connect the Quest (Developer Mode on) and run Install-SilentHillVR.bat.
   It gets adb if needed (Google's official platform-tools), installs the
   game, grants its storage permission and copies your disc/textures.
3. Start the game on the Quest (Library > Unknown Sources).

Manual installation
-------------------
1. Install SilentHillVR-Quest-0.1.apk with SideQuest (or "adb install").
2. Start the game on the Quest (Library > Unknown Sources).
   The first time, a panel asks for "All files access": turn it on and
   start the game again. The game then creates /sdcard/SilentHillVR
   with everything it needs.
3. With the Quest connected to the PC, put your disc's .bin and .cue in:
     /sdcard/SilentHillVR/gamedata/
   and start the game.
4. (Optional) HD textures: put the pack's .zip, still zipped, in:
     /sdcard/SilentHillVR/gamedata/texturemods/

Main controls
-------------
- Hold X: item screen. Tap X: reload / close the item screen.
- Hold Y: PC options (VR, VR_Tuning, VR_Comfort... pages).
- Hold A for 2 s: wheel with healing items and the map.
- Grip near the hip/chest/shoulder: draw or holster the weapon.
- Trigger: shoot / attack.

Saves and logs live in /sdcard/SilentHillVR (uninstalling the app keeps them).

Credits
-------
Based on the Silent Hill PC Port (github.com/SlickAmogus/silent-hill-decomp)
and the Silent Hill decompilation project. VR hands: UltimateXR BigHands
(MIT, license in /sdcard/SilentHillVR/vrhands). Silent Hill is a trademark of
Konami; no game files are distributed here.
