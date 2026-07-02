# VitaMania

VitaMania is an unofficial PS Vita homebrew rhythm game for four-lane osu!mania beatmaps.

## How To Install

1. Copy `VitaMania.vpk` to your PS Vita.
2. Install the VPK with VitaShell.
3. Create this data folder if it does not already exist:

```text
ux0:data/vitamania/
```

4. Drop `.osz` beatmap archives directly into `ux0:data/vitamania/`.
5. Launch VitaMania. The first scan may take a while if you added new `.osz` files.

VitaMania automatically extracts `.osz` archives into:

```text
ux0:data/vitamania/Songs/
```

After an archive extracts successfully and contains a playable `.osu`, VitaMania deletes the original `.osz`. You can also copy already-unzipped beatmap folders directly into `ux0:data/vitamania/Songs/`.

## Current Scope

- Four lanes mapped to `L`, `D-pad Down`, `X`, and `R`.
- Rebindable lane controls from the in-app options menu.
- Uses `ux0:data/vitamania/` as the only VitaMania data path.
- Automatically extracts `.osz` archives from `ux0:data/vitamania/` into `ux0:data/vitamania/Songs/`, then deletes the archive after the extracted folder contains a playable `.osu`.
- Reads playable beatmap folders from `ux0:data/vitamania/Songs/`.
- Supports native 4K osu!mania, converts other mania key counts to 4K, and converts osu!standard maps to 4K.
- Parses normal notes, hold notes, sliders, and spinners into VitaMania's four-lane playfield.
- Groups multiple difficulties for the same beatmap set into one song entry.
- Shows beatmap image backgrounds as async thumbnails in song select. Background previews are on by default.
- Plays delayed song previews while browsing the library.
- Saves and displays best score, accuracy, max combo, and play count per difficulty.
- Includes pause, restart, music volume, background toggle, and lane binding controls.
- Plays `.mp3` beatmap audio through Vita's native MP3 decoder.
- Plays `.ogg`/Vorbis beatmap audio through a bundled software decoder.
- Falls back to Vita AvPlayer for non-MP3 audio files when the system codec accepts them.
- Ignores beatmap video events and uses the beatmap's image background instead.

## Beatmap Folder Layout

Install the VPK, then copy `.osz` archives into the VitaMania data folder:

```text
ux0:data/vitamania/
  Artist - Song.osz
  Songs/
```

On the next scan, VitaMania unpacks the archive into `Songs/` and removes the `.osz` after extraction succeeds:

```text
ux0:data/vitamania/
  Songs/
    Artist - Song/
      Artist - Song (Creator) [Difficulty].osu
      audio.mp3 or audio.ogg
```

You can also copy already-unzipped beatmap folders directly into `ux0:data/vitamania/Songs/`. The first scan after adding new songs can take a while.

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
Resume uses a 3-count before the chart continues
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

This is a compatibility player, not an official osu! project. It does not ship music or beatmaps.
The only official data path is `ux0:data/vitamania/`. Older `ux0:data/osuvita/` folders are not scanned.
