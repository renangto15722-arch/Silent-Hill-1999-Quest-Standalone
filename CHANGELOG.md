# Changelog

## 0.4

### New
- **Physical melee combat.** The knife, steel pipe, hammer, axe and katana now hit only with your real swing — no button.
  - A weak swing does little damage, a strong and wide one does the weapon's full damage. Just touching an enemy, pushing it or shaking the weapon does nothing.
  - Each weapon feels different: the knife is quick and light, the pipe needs more force, the hammer and the axe are the heaviest, the katana is fast.
  - One hit per swing. `Melee_Cooldown_ms` sets the shortest time between two hits.
  - The damage always comes from the original game, never more than its own maximum.
- **Chainsaw and rock drill** cut while you hold the trigger and the blade is in the enemy, once every interval (`Chainsaw_Hit_Every_ms`, `Drill_Hit_Every_ms`).
- **Combat difficulty**: *Normal* (the original game), *Hard* and *Silent Hell*. Monsters move faster and recover sooner after being hit. Can be changed at any time. The damage you take still follows the game's own Easy / Normal / Hard.
- **Training dummy** (`Training_Dummy`): the first monster in the room is brought in front of you, stands still, cannot die and shows the damage of every hit on screen. Good for trying the weapons.
- **Show hitboxes** (`Show_Hitboxes`): shows where each weapon hits and the enemies' bodies.
- **New `VR_Combat` page** in the PC options with all the combat settings.
- **Restore defaults** for each weapon on the `VR_Objects` page.
- The `VR_Holsters` page only shows the options of the mode you chose (holsters or weapon wheel).

### Fixed
- The gun flash, smoke and sparks now come out of the weapon in your hand, for every weapon.
- Weapons are no longer shown in Harry's hand during cutscenes.
- Holding the Quest button to recenter no longer leaves the view close to the floor.
- The piano puzzle screen (and other picture screens) no longer go black when they change.
- The flashlight beam glow that covered the view in VR was removed.
- New option `Flashlight_On_Head`: the flashlight points where you look.
- Option names no longer cover their values.

### Performance (Quest 3)
- Smoother in busy places like the café: about 13% less work per frame and almost no stutter frames (from 25–35 to 1–4 every few seconds).

## 0.3

### New
- **Install without a PC.** Download the APK, your disc and the HD packs with the Quest browser, install the APK from the Files app and start the game: it moves everything from the *Download* folder into place by itself.
  - Disc: `.bin` + `.cue`, or a `.zip`, `.7z` or `.rar` that has them, also one archive inside another (any file name; the disc is recognised by its boot file).
  - HD textures: `.zip` → `texturemods` (kept zipped); `.7z` / `.rar` → unpacked into `texturemods`. HD maps / menus (`TIM`, `1ST`, `ITEM`...) in any of the three → unpacked into `load`.
  - RAR5 and password-protected archives cannot be read; the panel says which file and why.
  - Folders inside *Download* are searched too (up to 3 levels), e.g. what the Files app unpacked: the disc, archives, a texture pack folder (`texupload-*` files) → `texturemods`, `TIM` / `1ST` / `ITEM`... folders → `load` (only inside a Silent Hill / "HD Maps" folder, or two or more of them together).
  - Nothing already installed is overwritten; files it does not recognise stay in *Download*.
- **Setup panel** before the game when it cannot start yet: asks for "All files access" and goes on by itself once it is on (no more starting the game twice), shows progress while it installs, lists the files it could not use, and explains how to get the disc when there is none.

### Fixed
- **Gun aim and weapon positions on a fresh install.** The default settings in 0.2 carried old aim values that overrode the per-weapon tuning: every gun shot off target and the handgun sat crooked. New installs now get the tuned aim for each gun and the tuned position of every weapon (kitchen knife included).
- New default settings (tuned on the Quest 3).
- *Updating from 0.2 or an earlier 0.3?* The old default `config.cfg` (the one with the wrong aim) is replaced automatically and kept as `config.cfg.bak`. A config you tuned yourself and your saves are left alone.

## 0.2

### New
- **Weapon wheel** (VR Holsters → `Weapon_Select`): an alternative to the holsters. Hold the right grip, an oval of the weapons you own opens in front of the controller; move the hand to a weapon (it lights up) and release the grip to take it. Back to the centre and release = cancel. Size and distance in the menu (`Wheel_Distance_cm`, `Wheel_Width_cm`, `Wheel_Height_cm`).
- **Visible holster boxes**: each holster shows a see-through box (lights up when your hand is in reach); every weapon is resized to fit inside it. `Box_Size_cm`, `Box_Opacity`.
- **Holsters fitted to your body**: placed from your height measured by the headset (standing or seated), on a virtual torso that follows your head **and** both controllers, so turning only your head barely moves them; no more swaying while walking. `Holster_Forward_cm` moves them forward/back, `Grab_Reach_cm` sets how far around the box still grabs.
- **Any weapon in either hand**: the left hand takes every weapon, not just the knife. A gun in the left hand aims and fires from the left controller (left trigger); placement is the right hand's, mirrored.
- **HD options** (PC Options → Graphics): `HD_Textures`, `HD_Maps`, `HD_Menus` (applied after a restart).
- `Snow_Size` (PC Options → System): snow at its original size in VR.
- `Auto_Load` is back in the in-game Options menu.
- Default position and aim for every weapon (tuned on the Quest), shotgun at 200% size; per-gun aim rows on the VR_Objects page.

### Fixed
- The scripted map scenes (map zooms in with an arrow / Cheryl) now show on the 2D panel instead of filling the whole view.
- Guns no longer jump to another position a few seconds after you first aim them.
- The left-hand weapon now sits exactly mirrored to the right hand.
- Holsters no longer sit too close to the face when playing seated.

### Performance (Quest 3)
- From ~17-32 fps (first build) and ~52 fps to **~72 fps** in most places, with room for sharper eye images (default `Eye_Resolution` 130).
- Each eye image now only shades the part that eye can see (was ~70% wasted).
- No more CPU stalls on the GPU when uploading geometry; the timer thread no longer spins a CPU core.
- Fewer driver calls per draw (shader uniform cache, fast eye passes, both eyes in one instanced draw).
- Default config: post-processing off, `Eye_Resolution` 130.
