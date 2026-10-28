# First Person

A native mod for the Super Mario Sunshine PC port: the game seen through Mario's eyes.

- **V** (changeable as `key.toggle`) or **D-pad Down** (changeable in the Mods menu) switches it on and off.
- The **C-stick** turns the view left and right and tilts it up and down; Mario runs where you look. Walls never stop the view turning (the game's own camera, which they stop, no longer turns it).
- **L** looks the way Mario faces.
- While **R** is held, Mario's body and FLUDD aim where you look, up and down too, so the water goes straight where you look, and you can still walk any way, sideways too (Yoshi spits there as well). R all the way down no longer plants him in place: he keeps moving, spraying at full strength (the Mods menu can switch that off). Needs a game with mod API 1.5 beta 8's `player->set_aim`; on an older game FLUDD sprays the way he faces.
- The mouse does not turn the view in first person (mods cannot read it yet).
- **Mario in first person** (Mods menu): Body only (the default: his head and FLUDD are left out, so looking down you see his body; FLUDD still sprays; his body always faces where you look, so it walks and runs ahead of you whichever way you move, on Yoshi too, except swimming), or Hidden. Body only needs mod API 1.5 beta 8's `player->set_hidden_parts`; on an older game he is hidden.
- In cutscenes and talks the game's own camera takes over, and comes back after.

The Mods menu (F1) has: the controller button, field of view (60), eye height (130, higher on Yoshi
by itself), turn and tilt speed, invert left and right, invert up and down, and what of Mario shows.

## Which game it needs

Any version of the game with mod support. Two features need a newer game (mod API 1.5 beta 8, not
released yet): aiming FLUDD where you look (`player->set_aim`, `set_yaw`), and the Body only model
(`player->set_hidden_parts`). On an older game FLUDD sprays the way Mario faces, and Mario is hidden.

They come with these pull requests, once merged and a new game is released:

- Game: [chasem-dev/sms-pc-port#318](https://github.com/chasem-dev/sms-pc-port/pull/318)
- Mod SDK: [chasem-dev/sms-mod-sdk#12](https://github.com/chasem-dev/sms-mod-sdk/pull/12)

## Download

The latest build, for every platform, is on the [Releases](https://github.com/Allebale/sms-first-person-mod/releases) page.

## Install

Copy the `first-person` folder (with `mod.ini` and `bin/`) into the game's `mods/` folder.

## Build

    cmake -S . -B build -A x64 -DSMS_MODS_OUT=C:/path/to/the/game/mods
    cmake --build build --config Release
