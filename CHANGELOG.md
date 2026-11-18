# Changelog

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
