# ♬ aqua

A tiny, fast terminal music player. One C file, no libraries, no GUI — it
shells out to **ffplay** for audio and uses **ffprobe** for track lengths.
Minimal aqua/black/gray interface, no animations.

```
  ♬ AQUA   1024 tracks
  ▶ Bonobo - Kerala                          3:54
    Bonobo - Kong                            4:41
    Boards of Canada - Roygbiv               2:31
  ...
  ─────────────────────────────────────────────────
  PLAYING  Bonobo - Kerala
   1:12 ████████████░░░░░░░░░░░░░░░░░░░░  3:54
  ↑↓ move  ↵/space play/pause  n/b next/prev  ←→ seek  -/+ vol  s shuffle  r repeat  q quit
```

## Features

- Recursively scans a folder for audio (`mp3 flac ogg wav m4a aac opus wma ...`)
- Play / pause (true pause via `SIGSTOP`/`SIGCONT`), next / previous, seek
- **Gapless volume** — adjusts the live PulseAudio stream, no restart
- **Shuffle** and **repeat**
- **Duration cache** in `~/.cache/aqua/` — only new or modified files are
  re-probed, so large libraries open instantly
- Tiny: a single ~700-line C file, no build dependencies beyond libc

## Requirements

- A C compiler (`cc`/`gcc`) and `make`
- [`ffmpeg`](https://ffmpeg.org/) — provides `ffplay` (playback) and `ffprobe` (durations)
- PulseAudio's `pactl` *(optional)* — enables gapless volume; without it,
  volume changes fall back to a quick restart

On Debian/Ubuntu/Linux Lite:

```sh
sudo apt install build-essential ffmpeg
```

## Build & install

```sh
make                 # builds ./aqua
make install         # installs to ~/.local/bin/aqua  (override PREFIX=...)
```

Make sure `~/.local/bin` is on your `PATH`.

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
| `/`            | search / filter tracks  |
| `s`            | toggle shuffle          |
| `r`            | toggle repeat (playlist)|
| `g` / `G`      | jump to top / bottom    |
| `q`            | quit                    |

### Search

Press `/` and type to filter the list live (case-insensitive, matches anywhere
in the track name). While searching: `↑`/`↓` move through results, `Enter`
plays the highlighted match and keeps the filter, `Esc` clears it. With a
filter active, `Esc` in normal mode clears it too. Playback (next/prev) still
walks the full library, not just the filtered results.

## How it works

- Playback is an `ffplay -nodisp -autoexit` child process. Pause sends it
  `SIGSTOP`/`SIGCONT`; next/seek replace it.
- Durations are read with `ffprobe` lazily inside the main loop (the UI is
  responsive immediately) and cached to `~/.cache/aqua/durations.tsv`, keyed by
  absolute path + modification time. Delete that file to force a full rescan.
- Volume changes locate ffplay's PulseAudio sink-input by PID and set its
  volume with `pactl`, so the audio never cuts out.

## License

MIT — see [LICENSE](LICENSE).
