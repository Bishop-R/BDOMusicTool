# BDO Music Compositor

Compose music for Black Desert Online outside of the game. Import MIDIs, edit in a full piano roll, tweak instruments and effects, then export directly to BDO's in-game format.

Supports all 26+ BDO instruments, per-instrument aux sends, MIDI tempo maps, and automatic sample extraction from your game installation so no copyrighted assets are distributed.

![Screenshot](Screenshot.png)

**Early access** — there will be bugs. Report them via [Issues](https://github.com/Bishop-R/BDOMusicTool/issues) or Discord DM: `bishof.`

Most inputs are handled via shortcuts. Check the shortcut window in the bottom right if you're lost.

## Download

Grab the latest build from [Releases](https://github.com/Bishop-R/BDOMusicTool/releases). Extract and run `composer.exe`. The first launch walks you through sample extraction and account setup.

## Build from Source

Needs CMake 3.20+ and a C compiler. SDL3 is fetched automatically.

```bash
cd src && mkdir build && cd build
cmake .. && make
./composer
```

Windows cross-compile from Linux:
```bash
cmake .. -DCMAKE_TOOLCHAIN_FILE=../mingw-toolchain.cmake && make
```

Run the platform-neutral regression suite:
```bash
cmake -S src -B src/build -DBUILD_TESTING=ON
cmake --build src/build
ctest --test-dir src/build --output-on-failure
```

Linux and 64-bit Windows are both supported build targets. SDL is pinned to
a tested release, and CI builds both platforms.

Linux desktop integration (icon + .desktop file):
```bash
./install-linux.sh
```

## What it Does

- Piano roll editor
- MIDI import with automatic tempo change handling and instrument mapping
- BDO export
- WAV export
- All BDO instruments: Beginner, Florchestra, Marnian synths, electric guitars
- Per-instrument reverb, delay, and chorus sends (Still WIP)
- Undo/redo, copy/paste, transpose, etc.
- Tempo mapping
- Quantize (`Ctrl+Alt+Q`), humanize (`Ctrl+Alt+H`), arpeggiate
  (`Ctrl+Alt+A`), and strum (`Ctrl+Alt+S`) selected notes
- MIDI export (`Ctrl+Shift+M`) using BDO's single project tempo
- Editor-only arrangement markers (`Ctrl+K` at the playhead)
- MIDI-keyboard preview and recording (`Ctrl+R`) on Linux/ALSA and Windows/WinMM
- BDO compatibility validation before export
- Atomic saves and rotating crash-recovery history
- Extracts instrument samples from your BDO installation on first launch

## Disclaimer

This tool reads data from your local BDO installation to extract instrument samples. It does not modify any game files or connect to game servers. Not affiliated with Pearl Abyss.

## License

GPL-3.0
