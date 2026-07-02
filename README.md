# VitaMania

VitaMania is an unofficial PS Vita homebrew rhythm game for four-lane osu!mania beatmaps.

## Current Scope

- Four lanes mapped to `L`, `D-pad Down`, `X`, and `R`.
- Rebindable lane controls from the in-app options menu.
- Reads unzipped beatmap folders from `ux0:data/osuvita/Songs/`.
- Also scans `ux0:data/vitamania/Songs/` as a fallback path.
- Supports native 4K osu!mania, converts other mania key counts to 4K, and converts osu!standard maps to 4K.
- Parses normal notes, hold notes, sliders, and spinners into VitaMania's four-lane playfield.
- Groups multiple difficulties for the same beatmap set into one song entry.
- Shows `.jpg` and `.jpeg` beatmap backgrounds as async thumbnails in song select when background previews are enabled.
- Plays delayed song previews while browsing the library.
- Saves and displays best score, accuracy, max combo, and play count per difficulty.
- Parses storyboard/event samples and explicit per-note custom `.wav` samples.
- Preloads custom `.wav` samples at song start to avoid mid-chart stalls.
- Includes pause, restart, music volume, and hit sound volume controls.
- Plays `.mp3` beatmap audio through Vita's native MP3 decoder.
- Plays `.ogg`/Vorbis beatmap audio through a bundled software decoder.
- Falls back to Vita AvPlayer for non-MP3 audio files when the system codec accepts them.

## Beatmap Folder Layout

Install the VPK, then copy unzipped beatmap folders like this:

```text
ux0:data/osuvita/Songs/
  Artist - Song/
    Artist - Song (Creator) [Difficulty].osu
    audio.mp3 or audio.ogg
```

The app does not extract `.osz` files yet. Unzip them on a computer first and copy the extracted folder.

## Controls

Song select:

```text
Up/Down       Select song
Left/Right    Select difficulty
X             Play
Square        Options
Triangle      Rescan songs
Select        Toggle background previews, off by default
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
Video filenames are detected for beatmap sets, but full video background playback is still future work.
