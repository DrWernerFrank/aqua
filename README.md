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
- **Tracks / Albums / Artists tabs** (`Tab` to cycle): a flat track list, an
  Albums browser (open an album to see its tracks in track-number order), and an
  Artists browser (open an artist to see their tracks by album then track).
  Untagged files fall back to their containing folder name as the album
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
- [`ffmpeg`](https://ffmpeg.org/) — provides `ffplay` (playback) and
  `ffprobe` (durations + tags)
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

### Controls

| Key            | Action                  |
|----------------|-------------------------|
| `↑` / `↓` `k`/`j` | move selection        |
| `Enter`        | play selected track     |
| `Space` / `p`  | play / pause            |
| `n` / `b`      | next / previous         |
| `←` / `→`      | seek −5s / +5s          |
| `-` / `+`      | volume down / up        |
| `/`            | search / filter         |
| `Tab`          | cycle Tracks/Albums/Artists |
| `s`            | toggle shuffle          |
| `r`            | repeat: off / all / one |
| `g` / `G`, `Home` / `End` | jump to top / bottom |
| `PgUp` / `PgDn`| move a page            |
| `q` / `Ctrl-C` | quit                    |

### Tabs

`Tab` cycles between **Tracks** (a flat list of every track), **Albums**, and
**Artists**. In the Albums/Artists tabs, `↑↓` move between entries and `Enter`
(or `→`) opens one — an album shows its tracks in track-number order, an artist
shows their tracks grouped by album then track. `Esc` goes back to the list.
Opening an album or artist makes that selection the queue, so next/prev stay
within it. `/` filters the album/artist list while browsing, tracks otherwise.

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
