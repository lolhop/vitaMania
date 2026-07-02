# vitaMania!

vitaMania! is an unofficial PS Vita homebrew rhythm game for 4k osu!mania beatmaps.

<p align="center">
  <img src="https://github.com/user-attachments/assets/498ac4a6-83d5-4a4d-845c-28ba3f584fe0" alt="vitaMania song select" width="45%">
  <img src="https://github.com/user-attachments/assets/2c1498a3-0c28-41e0-b835-2662fd559a32" alt="vitaMania gameplay" width="45%">
</p>


## How To Install

1. Copy `VitaMania.vpk` to your PS Vita.
2. Install the VPK with VitaShell.
3. Create this data folder if it does not already exist:

```text
ux0:data/vitamania/
```

4. Drop `.osz` beatmap archives directly into `ux0:data/vitamania/`.
5. Launch vitaMania!. The first scan may take a while if you added new `.osz` files.

vitaMania! automatically extracts `.osz` archives into:

```text
ux0:data/vitamania/Songs/
```

After an archive extracts successfully and contains a playable `.osu`, vitaMania! deletes the original `.osz`. You can also copy already-unzipped beatmap folders directly into `ux0:data/vitamania/Songs/`.

## Controls

Song select:

```text
Up/Down       Select song
Left/Right    Select difficulty
X             Play
Square        Options
Triangle      Rescan songs
Select        Toggle background previews, on by default
Circle        Quit
```

Gameplay:

```text
Start         Pause
Default lanes L, D-pad Down, X, R
```

Pause menu:

```text
Up/Down       Select item
Left/Right    Adjust volume
X             Confirm
Start/Circle  Resume
```

## Build

```sh
cmake -S . -B build
cmake --build build
```

The build outputs `VitaMania.vpk` in the build directory.

## Notes

AI tools were used while writing and debugging the code. No generative AI was used for the game assets.

This is a compatibility player, not an official osu! project. It does not ship music or beatmaps.

You can download your own beatmaps from the official osu! beatmap listing:
https://osu.ppy.sh/beatmapsets?m=3

The only official data path is `ux0:data/vitamania/`.
