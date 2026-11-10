# ngSid

*NextGenSid, a modern SID player for Linux.*

**A SID player for Linux with an OpenGL GUI, built to sound like a real SID.**

ngSid started as a Linux port of **VacSid** by **Oliver Blasnik ("R0M" / Vacuum)**, the legendary DOS SID player
that was ahead of its time. Today the C64 and the SID chip come from **libsidplayfp** (cycle-exact 6510, VIC-II and
CIA, **reSIDfp** for the sound, MOS 6581 and 8580), so tunes with raster tricks, CIA-timed samples and RSID code play as on
the real machine. What remains of VacSid is the idea, the credit, and a small synth that drives the displays
(voice cards, envelopes, digi channel). Oliver's work is the reason this project exists.

An independent project: it contains no code and no binaries from VacSid, and the original author is not affiliated with it.

## Features

- **Cycle-exact C64** through libsidplayfp: PSID and RSID, raster tricks, multispeed players, sample playback.
- **reSIDfp sound** with the SID model from the tune header (or `--6581` / `--8580`).
- **OpenGL GUI**: playlist, online HVSC browser, drag and drop, per-voice cards with note, frequency, scope and
  envelope, digi channel display (volume-register and oscillator samples), filter strip, spectrum and VU meters, fullscreen.
- Version display and an optional update check against GitHub releases.
- Unit tests for the display synth, PSID header parsing and the engine.

## Build

Requirements: a C99 and a C++ compiler, `make`, `curl` or `wget`, `tar`, SDL2 (development files) and `pkg-config`.
The first `make` downloads libsidplayfp 2.16.1 (checksum verified) and builds it statically; this needs network access once.

    make                 # builds ./ngSid
    make check           # runs the self-tests (no SDL2 needed)
    sudo make install    # optional, installs to /usr/local/bin (PREFIX=... to change)

## Use

    ./ngSid tune.sid          # or a folder, or drag and drop files onto the window
    ./ngSid                   # no argument: loads SID files from ~/Musik or ~/Music if present,
                              # otherwise opens the online HVSC browser
    ./ngSid -s 2 tune.sid     # start with song 2

Keys: `Space` pause, `Left/Right` song, `Up/Down` file, `1`-`3` mute voices, `4` mute digi, `+`/`-` volume,
`R` restart, `F` / `F11` fullscreen, `Esc` quit. Mouse: click a voice card to mute it,
click the playlist, drag the volume bar.

Options:

| Option | Meaning |
|---|---|
| `-s N` | start song |
| `--pal`, `--ntsc` | override the clock |
| `--rate N` | sample rate |
| `--6581`, `--8580` | force the SID model (default: from the tune header, else 6581) |
| `--no-update-check` | do not ask GitHub for a newer release at startup |
| `--version` | print the version and quit |

The version is shown at the bottom of the playlist panel. At startup ngSid asks the GitHub API once (through `curl`, in the background) whether a newer release exists and then shows a clickable hint; nothing else is sent or stored. Switch it off with `--no-update-check`.

SID files are not included. The best source is the High Voltage SID Collection (<https://hvsc.de>).

## Status and limits

Multi-SID tunes (2SID and 3SID) are played completely; the GUI shows one row of voice cards per chip. The filter strip and the digi channel show SID 1 only.
Not implemented yet: SID model/filter settings beyond `--6581` / `--8580`. Please report tunes that sound different from a real SID.

## Credits

- **Oliver Blasnik ("R0M" / Vacuum)**: VacSid, the player this project is based on.
- DejaVu Sans (Bitstream Vera licence) is baked into `gui/font_data.h` for the GUI.
- **libsidplayfp** and **reSIDfp** (GPL-2.0-or-later, <https://github.com/libsidplayfp/libsidplayfp>): cycle-exact C64 and the SID emulation (Dag Lem, Antti Lankila, Leandro Nini, Simon White and contributors).
- The High Voltage SID Collection crew, for keeping the music alive.

## License

GPL-3.0-or-later. Everyone may use, study, change and share it. See [`LICENSE`](LICENSE).
