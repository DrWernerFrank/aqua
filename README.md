# ♬ aqua

A tiny, fast terminal music player. One C file per platform, no libraries, no
GUI — it shells out to **ffplay** for audio and uses **ffprobe** for track
lengths. Minimal aqua/black/gray interface, no animations.

```
  ♬ AQUA   1024 tracks
  ▶ Bonobo - Kerala                          3:54
    Bonobo - Kong                            4:41
    Boards of Canada - Roygbiv               2:31
  ...
  ─────────────────────────────────────────────────
  PLAYING  Bonobo - Kerala
   1:12 ████████████░░░░░░░░░░░░░░░░  3:54
  ↑↓ move  ↵/space play/pause  n/b next/prev  ←→ seek  -/+ vol  s shuffle  r repeat  q quit
```

## Features

- Recursively scans a folder for audio (`mp3 flac ogg wav m4a aac opus wma ...`)
- **Reads tags** (title, artist, album, track number)
- **Tracks / Albums / Folders / Artists tabs** (`Tab` to cycle): a flat track
  list, an Albums browser (open an album to see its tracks in track-number
  order), a Folders browser (your library as it sits on disk, tracks in
  filename order), and an Artists browser (open an artist to see their tracks
  by album then track). Untagged files fall back to their containing folder
  name as the album
- **Oscilloscope tab** *(experimental — off until you switch it on in
  Settings)* — the playing track drawn live in braille dots: waveform, stereo
  (left over right) or X-Y, with trigger and auto gain
- **Settings tab** — the music folder, accent, background and text colors by color code
  (`#rrggbb` hex or a `0`-`255` palette index, previewed live as you type),
  seek and volume step sizes, the tab to start on, and the oscilloscope's
  options; all remembered between
  runs, along with shuffle and repeat
- Play / pause (a true pause: `SIGSTOP`/`SIGCONT` on Linux, thread-suspend on
  Windows), next / previous, seek
- **Volume without dropouts** — applied live, in place: through `pactl`,
  `wpctl` or `amixer` on Linux (the first that answers), and through the
  Windows Core Audio per-app session on Windows. `+`/`-` never restart
  playback when a live backend exists; without one, the track is restarted
  *once* after you stop pressing the key (not once per key repeat), and a
  change made while paused is applied on resume
- **Shuffle** and **repeat** (off / all / one)
- **Search** across title, artist, album and filename
- **Flicker-free UI** — the screen is repainted as a diff (only the lines
  that changed, wrapped in synchronized updates), so an idle display emits
  nothing at all: no full-screen redraws, no flashing while tracks play
- **Metadata cache** — only new or modified files are re-probed, so large
  libraries open instantly; files ffprobe can't read are cached with an
  unknown duration so they aren't re-probed every launch either
- Tiny: one C file per platform, no build dependencies beyond libc/Win32
- **Small memory footprint** — a 5,000-track library with a warm cache sits
  around 3 MB RSS on Linux and ~5 MB working set on Windows (about 180 bytes
  per track): repeated names (artist, album, folder) are stored once and
  shared, and the cache is parsed in place and released right after startup

## Requirements

- A C compiler (`cc`/`gcc`) and `make`
- [`ffmpeg`](https://ffmpeg.org/) — provides `ffplay` (playback),
  `ffprobe` (durations + tags) and `ffmpeg` itself (samples for the
  experimental oscilloscope)
- Linux only, optional: `pactl` (`pulseaudio-utils`), `wpctl` or `amixer`
  for live volume. Without any of them, a volume key restarts the track at
  its current position once the keys settle — a brief gap, but never a
  stutter chain

On Debian/Ubuntu/Linux Lite:

```sh
sudo apt install build-essential ffmpeg
```

## Build & install (Linux)

```sh
make                 # builds ./aqua
make install         # installs to ~/.local/bin/aqua  (override PREFIX=...)
```

Make sure `~/.local/bin` is on your `PATH`.

## Windows

A native Windows port lives in **`aqua_win.c`** (no WSL, no Cygwin). It swaps
the POSIX platform layer for the Win32 Console API + VT sequences,
`CreateProcess` + Toolhelp thread-suspend (in place of `fork`/`SIGSTOP`),
`QueryPerformanceCounter`, and `_kbhit`/`_getch`. The metadata cache lives in
`%LOCALAPPDATA%\aqua\library.tsv`. Volume is changed live through the Windows
Core Audio session of the running `ffplay` process, so `+`/`-` never restart
playback. That level is the one you see for `ffplay.exe` in the Windows Volume
Mixer; Windows remembers it between tracks and runs, and aqua picks it up at
startup instead of resetting it to 100%.

```powershell
# Requirements: a MinGW-w64 gcc, plus ffmpeg on PATH (winget install Gyan.FFmpeg)
gcc -O2 -Wall -o aqua.exe aqua_win.c
aqua.exe "%USERPROFILE%\Music"
```

Or run **`install-windows.ps1`** (in this repo) to build, install to
`%LOCALAPPDATA%\Programs\aqua`, and add an **aqua** entry to the Start Menu:

```powershell
powershell -ExecutionPolicy Bypass -File install-windows.ps1
```

Both ports share one design and one metadata format; the Linux and Windows
caches are interchangeable, so a library scanned on one is instant on the
other (over a shared drive).

## Usage

```sh
aqua                 # play the current directory
aqua ~/Music         # play a specific folder (recurses into subfolders)
```

You can also pick the folder from inside aqua: **Settings → Music folder**
(see below). A saved folder is used whenever aqua starts without a folder
argument, or with the same argument it had when you saved it — so a Start Menu
shortcut or shell alias that always passes one folder keeps your choice. A
*different* folder on the command line still wins for that run.

### Controls

| Key            | Action                  |
|----------------|-------------------------|
| `↑` / `↓` `k`/`j` | move selection        |
| `Enter`        | play selected track     |
| `Space` / `p`  | play / pause            |
| `n` / `b`      | next / previous         |
| `←` / `→`      | seek back / forward (5s; see Settings) |
| `-` / `+`      | volume down / up        |
| `/`            | search / filter         |
| `Tab` / `Shift-Tab` | next / previous tab  |
| `1`-`5`        | jump straight to a tab (`1`-`6` with the Scope tab on) |
| `s`            | toggle shuffle          |
| `r`            | repeat: off / all / one |
| `g` / `G`, `Home` / `End` | jump to top / bottom |
| `PgUp` / `PgDn`| move a page            |
| `q` / `Ctrl-C` | quit                    |

### Tabs

`Tab` cycles between **Tracks** (a flat list of every track), **Albums**,
**Folders**, **Artists**, and **Settings** (`Shift-Tab` goes back, `1`-`5` jump
directly). Turning on the experimental oscilloscope adds a **Scope** tab just
before Settings (the number keys then run `1`-`6`). In the Albums/Folders/Artists tabs, `↑↓` move between entries and
`Enter` (or `→`) opens one — an album shows its tracks in track-number order, a
folder shows its files in filename order, an artist shows their tracks grouped
by album then track. `Esc` goes back to the list. Opening an entry makes that
selection the queue, so next/prev stay within it. `/` filters the
album/folder/artist list while browsing, tracks otherwise.

### Scope (experimental)

**Off by default.** To turn it on, open Settings, move to **Experimental:
oscilloscope** and press `Enter`: the Scope tab appears just before Settings,
and its options are added to the Settings list. Switching it off (or **Reset
all settings**) removes both again. Experimental features may still change.

An oscilloscope for the playing track, drawn in braille dots (two by four
per character cell). `m` cycles the mode, `↑`/`↓` change the time span, and
the playback keys (`Space`, `n`/`b`, `←`/`→`, `-`/`+`, `s`, `r`) work as in a
track list.

| Mode | What it shows |
|------|---------------|
| **Wave** | left + right mixed into one trace |
| **Stereo** | left channel on top, right channel below |
| **X-Y** | left across, right up — a Lissajous / stereo-field picture |

X-Y redraws the last stretch of signal every frame as one smooth curve (a
spline through the samples), the newest part in the X color and the older tail
in the Y color. Two things keep it a line instead of a filled blob:

- **Smoothing** low-passes the signal first, so the beam follows the bass and
  mids in open loops instead of scribbling the treble over everything.
- The **wide** view plots what the speakers disagree on (L−R) across and what
  they share (L+R) up, each axis scaled to fill the screen. Ordinary music is
  nearly the same in both speakers, so the textbook **L/R** plot collapses onto
  one diagonal; wide opens it up. Switch to L/R (and smoothing off) for a true
  Lissajous figure, e.g. with test tones or oscilloscope music.

ffplay keeps the audio to itself, so while this tab is open a second, silent
`ffmpeg` decodes the same track to raw samples on a pipe, and aqua reads
exactly as much as the playback clock has advanced — the picture follows the
music through pause, seek and track changes. The decoder is stopped as soon as
you leave the tab, so the scope costs nothing when you aren't looking at it.

### Settings

The last tab. `↑↓` pick a row, `Enter` changes it, `←`/`→` step option rows.

| Setting | What it does |
|---------|--------------|
| **Music folder** | the folder aqua plays. `Enter`, then type or paste a path and press `Enter`: aqua checks it is a folder with audio in it, saves it and restarts on it (a bad path changes nothing). Empty or `default` goes back to the folder aqua was started on |
| **Accent color** | highlight color (selection, progress bar, logo); default `44` |
| **Background color** | fills the whole UI; `default` keeps your terminal's own background |
| **Text color** | titles and other primary text; default `252` |
| **Seek step** | how far `←`/`→` jump: 5 / 10 / 15 / 30 / 60 seconds |
| **Volume step** | how much `-`/`+` change: 1 / 2 / 5 / 10 % |
| **Start on tab** | the tab aqua opens on: Tracks, Albums, Folders or Artists |
| **Experimental: oscilloscope** | `off` by default; `on` adds the Scope tab and the Scope rows below |
| **Scope mode** | Wave / Stereo / X-Y (also `m` in the Scope tab) |
| **Scope time span** | how much time fits across the screen (in X-Y: how long a stretch is drawn, never less than two frames): 10 / 20 / 50 / 100 / 200 ms |
| **Scope gain** | `auto` scales the trace to fill the screen; or a fixed 1x / 2x / 4x / 8x |
| **Scope trigger** | `on` starts each sweep at a rising zero crossing so the wave holds still |
| **Scope style** | `braille` dots, or half `blocks` for fonts without braille glyphs |
| **Scope frame rate** | 15 / 30 / 60 fps |
| **Scope sync delay** | how far the picture trails the clock (0-500 ms); raise it if the picture runs ahead of the sound |
| **Scope X-Y view** | `wide` (L−R across, L+R up, each axis scaled to fit) or `L/R` (left across, right up, true shape) |
| **Scope X-Y smoothing** | low-pass before drawing: off / light / medium / heavy |
| **Scope X color (left)** | color code for the wave, the left channel in Stereo, and the newest part of the X-Y curve; `default` follows the accent |
| **Scope Y color (right)** | color code for the right channel in Stereo and the X-Y tail; `default` follows the accent |
| **Reset all settings** | back to the defaults above (the music folder is kept) |

Colors are typed as a code:

- `#rrggbb` (or `rrggbb`, or short `#rgb`) — a true-color hex code, e.g. `#ff8800`
- `0`-`255` — an index into the terminal's 256-color palette
- `default` — that color's default

The whole UI previews the color as soon as the code is valid; `Enter` keeps it,
`Esc` cancels. Shuffle and repeat are remembered between runs as well.
Everything is saved to `~/.config/aqua/settings` on Linux and
`%LOCALAPPDATA%\aqua\settings` on Windows. Playback keys (`Space`, `n`/`b`,
`-`/`+`) keep working in this tab.

### Search

Press `/` and type to filter the list live (case-insensitive, matches anywhere
in the track name). While searching: `↑`/`↓` move through results, `Enter`
plays the highlighted match and keeps the filter, `Esc` clears it. With a
filter active, `Esc` in normal mode clears it too.

**The filtered list becomes the play queue:** while a filter is active,
next/prev (and auto-advance and shuffle) move only through the matching
tracks. Clear the filter and the queue returns to the full library.

## How it works

- Playback is an `ffplay -nodisp -autoexit` child process. Pause suspends it
  (`SIGSTOP`/`SIGCONT` on Linux, thread suspend/resume on Windows);
  next/seek replace it. `Ctrl-C` and terminal close quit through the normal
  exit path: ffplay is stopped, the metadata cache is written, and the
  terminal is restored — no raw-terminal leftovers, no orphaned audio.
- Metadata (duration + title/artist/album/track) is read with `ffprobe`
  lazily inside the main loop (the UI is responsive immediately) and cached
  (`~/.cache/aqua/library.tsv` on Linux, `%LOCALAPPDATA%\aqua\library.tsv`
  on Windows), keyed by absolute path + modification time. ffprobe is
  executed directly with the path as a single argument — no shell sits in
  between, so filenames like `$(touch pwned).mp3` are just filenames — and
  text fields are escaped in the cache, so newlines or tabs in names can't
  corrupt it. Files ffprobe can't read are cached with duration `-1` and
  skipped on later runs too; if ffprobe itself is missing, nothing bogus is
  cached and the library is probed once ffmpeg is installed. Delete the
  cache file to force a rescan.
- The list is grouped by album (or folder when untagged) and sorted by track
  number within each group; on the very first run the grouping settles as
  tags load, then stays instant.
- Rendering is a diff against a shadow of the screen: each `draw()` emits
  only the lines whose content changed, each positioned absolutely, bracketed
  in synchronized updates (`\033[?2026h/l`) so supporting terminals never show
  a partial frame. The shadow is kept as one 64-bit hash per line (a few KB),
  and output accumulates in a small chunk buffer — so even a full repaint
  never needs a whole-frame buffer. An unchanged screen writes zero bytes —
  while a track plays, only the elapsed-time and bar lines update, about
  twice a second total. Resizes repaint in place; the screen is blanked
  (`\033[2J`) exactly once, at startup, never mid-session.
- Memory is budgeted for large libraries. Track strings live in a bump
  allocator (one allocation per 64 KiB, no per-string heap headers), and
  folder/artist/album names are interned: a 10,000-track library with 500
  albums stores 500 album names, not 10,000 copies. The display name is
  derived from the path on demand, and the canonical cache key is computed
  in a stack buffer only when needed, so neither is stored per track. On
  startup the cache file is read into one buffer, parsed in place, and
  released as soon as its entries have been merged into the track list —
  a warm 5,000-track cache leaves no heap residue (the pages go straight
  back to the OS: `malloc`+`munmap` on Linux, `VirtualAlloc` on Windows).
- Volume is owned by whichever live backend answers at startup: ffplay's
  PulseAudio sink-input located by PID (`pactl`), the default sink
  (`wpctl`/`amixer`), or on Windows the Core Audio session of the ffplay
  process. On Linux a live backend keeps the ffplay stream itself at 100%
  software volume, so the level is never applied twice. If the stream hasn't
  registered yet (right after a track starts), the change is retried in the
  main loop rather than restarting.

## License

MIT — see [LICENSE](LICENSE).
