/* aqua (Windows port) — a tiny, fast terminal music player.
 *
 * Native Windows rewrite of the platform layer of aqua.c:
 *   - Console API + VT escape sequences instead of POSIX termios
 *   - CreateProcess + Toolhelp thread-suspend instead of fork/exec + SIGSTOP
 *   - QueryPerformanceCounter instead of clock_gettime(CLOCK_MONOTONIC)
 *   - _kbhit()/_getch() instead of select() on stdin
 *   - %LOCALAPPDATA%\aqua\library.tsv for the metadata cache
 * Audio output still uses ffplay; durations/tags still use ffprobe (ffmpeg).
 *
 * Build:  gcc -O2 -Wall -o aqua.exe aqua_win.c
 * Run:    aqua.exe [directory]   (defaults to current directory)
 *
 * Volume: instead of PulseAudio's pactl, the Windows Core Audio per-app
 * session volume of the running ffplay process is changed in place, so a
 * volume change never restarts playback (no dropout). Windows remembers the
 * level for ffplay.exe, so each new track starts at it with no loud blip.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>       /* MinGW-w64 provides a POSIX dirent */
#include <direct.h>       /* _mkdir */
#include <io.h>
#include <fcntl.h>
#include <conio.h>        /* _kbhit / _getch */
#define COBJMACROS
#include <windows.h>
#include <tlhelp32.h>     /* Toolhelp: suspend/resume a child's threads */
#include <mmdeviceapi.h>  /* Core Audio: per-process volume without a restart */
#include <audiopolicy.h>

/* MinGW exposes the case-insensitive comparisons under different names */
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ---- palette ---- */
#define AQUA   "\033[38;5;44m"
#define AQUA_B "\033[1;38;5;44m"
#define GRAY   "\033[38;5;245m"
#define DGRAY  "\033[38;5;240m"
#define WHITE  "\033[38;5;252m"
#define RESET  "\033[0m"
#define INVAQ  "\033[7;38;5;44m"   /* reverse aqua = selected row */

/* ---- track model ----
 * Fields are chosen for a small footprint in big libraries: `name` and
 * `abspath` are derived on demand (basename of path / canonical path in a
 * stack buffer), and folder/artist/album/group strings are interned, so
 * each distinct name is stored once no matter how many tracks share it. */
typedef struct {
    char *path;     /* full path passed to ffplay (relative to scan dir) */
    char *folder;   /* parent folder name (album fallback), interned */
    long  mtime;    /* file modification time, for cache validation */
    double dur;     /* seconds, -1 if unknown */
    /* metadata from tags (NULL when absent) */
    char *title;
    char *artist;   /* interned */
    char *album;    /* interned */
    char *group;    /* album if tagged, else folder; used for grouping/sort */
    int   trackno;  /* track number, 0 if unknown */
    int   probed;   /* 1 once ffprobe answered (even "no duration"), or cache hit */
} Track;

static Track *tracks = NULL;
static int ntracks = 0, capacity = 0;

static int sel = 0;          /* highlighted row (index into view[]) */
static int cur = -1;         /* currently loaded track, -1 = none */

/* search/filter: view[] holds the track indices matching `query` */
static int *view = NULL;
static int nview = 0;
static char query[128] = "";
static int qlen = 0;
static int searching = 0;    /* 1 while typing a query */

/* tabs: a flat Tracks list, Albums / Folders / Artists browsers, the
 * oscilloscope, Settings */
enum { TAB_TRACKS, TAB_ALBUMS, TAB_FOLDERS, TAB_ARTISTS, TAB_SCOPE, TAB_SETTINGS, TAB_COUNT };
static int tab = TAB_TRACKS;
static int in_album = 0;             /* 1 = drilled into an album's tracks */
static char *cur_album = NULL;      /* the album currently opened (owned string) */
static int view_sort = 0;            /* 0 = by title, 1 = by track number */

/* album list (distinct groups), and its filtered view for search */
static char **albums = NULL;         /* points into tracks[].group (not owned) */
static int  *album_n = NULL;         /* track count per album */
static int   nalbums = 0;
static int  *alview = NULL;          /* album indices matching the query */
static int   nalview = 0;
static int   alsel = 0;              /* selection in the album list */

/* are we browsing a group list (albums/folders/artists) vs a track list? */
static int on_album_list(void) {
    return (tab == TAB_ALBUMS || tab == TAB_FOLDERS || tab == TAB_ARTISTS) && !in_album;
}

/* the string a track is grouped by in the current browse tab */
static const char *browse_key(const Track *t) {
    if (tab == TAB_FOLDERS) return t->folder;   /* parent directory name */
    if (tab == TAB_ARTISTS) return (t->artist && *t->artist) ? t->artist : "(unknown)";
    return t->group;   /* albums (album tag, else folder) */
}

/* label for the active browse tab */
static const char *browse_label(void) {
    return tab == TAB_ARTISTS ? "Artists" : tab == TAB_FOLDERS ? "Folders" : "Albums";
}

/* ---- settings ----
 * Colours: the palette macros above carry the defaults (accent 44, text 252,
 * terminal background). Custom colours are stored as SGR parameters and
 * swapped in by apply_theme() as each line is flushed, so the whole UI
 * follows them. Everything here is saved to the settings file. */
enum { SET_ACCENT, SET_BG, SET_TEXT,          /* colour rows: index col[] */
       SET_SEEK, SET_VOLSTEP, SET_START, SET_MUSIC,
       SET_EXP_SCOPE,                                        /* experimental: oscilloscope on/off */
       SET_SC_MODE, SET_SC_SPAN, SET_SC_GAIN, SET_SC_TRIG,   /* oscilloscope */
       SET_SC_STYLE, SET_SC_FPS, SET_SC_DELAY,
       SET_SC_XYVIEW, SET_SC_SMOOTH,
       SET_SC_COLX, SET_SC_COLY,                             /* colour rows: col[3], col[4] */
       SET_RESET, SET_COUNT };
static const char *const set_names[SET_COUNT] = {
    "Accent color", "Background color", "Text color",
    "Seek step", "Volume step", "Start on tab", "Music folder",
    "Experimental: oscilloscope",
    "Scope mode", "Scope time span", "Scope gain", "Scope trigger",
    "Scope style", "Scope frame rate", "Scope sync delay",
    "Scope X-Y view", "Scope X-Y smoothing",
    "Scope X color (left)", "Scope Y color (right)",
    "Reset all settings"
};
typedef struct {
    char code[12];        /* as shown to the user */
    char sgr[24];         /* what the terminal gets ("" = terminal default) */
    const char *def;      /* default code */
    int  bg;              /* 1 = background (SGR 48), 0 = foreground (38) */
} Color;
static Color col[5] = {
    { "44",      "38;5;44",  "44",      0 },
    { "default", "",         "default", 1 },
    { "252",     "38;5;252", "252",     0 },
    { "default", "",         "default", 0 },   /* scope X: "default" = the accent colour */
    { "default", "",         "default", 0 },   /* scope Y */
};

/* the colour a settings row edits, or NULL for a non-colour row */
static Color *row_color(int r) {
    if (r >= 0 && r <= SET_TEXT) return &col[r];
    if (r == SET_SC_COLX) return &col[3];
    if (r == SET_SC_COLY) return &col[4];
    return NULL;
}
static int  seek_step = 5;               /* seconds per arrow-key seek */
static int  vol_step  = 5;               /* percent per volume keypress */
static int  start_tab = TAB_TRACKS;      /* tab shown at launch */
static int  setsel = 0;                  /* selection in the Settings tab: an index into set_vis[] */

/* Experimental features ship switched off. The oscilloscope is one: until it
 * is turned on in Settings there is no Scope tab and its option rows are not
 * listed, so the app looks and behaves exactly as it does without it. */
static int  scope_on = 0;
static int  set_vis[SET_COUNT];          /* the setting rows currently listed, in order */
static int  nset = 0;

/* ---- music folder ----
 * aqua plays the folder it is started on. Settings > Music folder replaces
 * that with a saved one: it is used whenever aqua starts with no folder
 * argument, or with the same argument it had when the folder was saved (a
 * Start Menu shortcut or alias always passes the same one - the choice has
 * to survive that). A different folder on the command line still wins.
 * Changing it restarts aqua on the new folder (see the end of main()). */
static char music_dir[PATH_MAX]  = "";   /* the saved folder; "" = none */
static char music_from[PATH_MAX] = "";   /* the launch argument when it was saved */
static char launch_arg[PATH_MAX] = "";   /* this run's folder argument; "" = none */
static char lib_dir[PATH_MAX]    = ".";  /* the folder this run is playing */
static char pbuf[1024] = "";             /* the path being typed */
static int  plen = 0;
static int  relaunch = 0;                /* leave the main loop and start over */
static const char *startup_note = "";    /* why the saved folder was not used */

static void settings_rows(void) {
    nset = 0;
    set_vis[nset++] = SET_MUSIC;         /* first: it is what people look for */
    for (int r = 0; r < SET_COUNT; r++) {
        if (r == SET_MUSIC) continue;
        if (!scope_on && r >= SET_SC_MODE && r <= SET_SC_COLY) continue;
        set_vis[nset++] = r;
    }
    if (setsel >= nset) setsel = nset - 1;
}

/* the SET_* row under the cursor */
static int set_row(void) {
    if (!nset) settings_rows();
    return set_vis[setsel < nset ? setsel : nset - 1];
}
static int  editing = 0;                 /* 1 while typing a colour code */
static char ebuf[12] = "";               /* the code being typed */
static int  elen = 0;
static char eprev[12] = "";              /* colour to restore if editing is cancelled */
static const char *set_msg = "";         /* feedback for the last change */
static int  force_full = 0;              /* colours changed: repaint every line */
static char settings_file[PATH_MAX];
static int  shuffle, repeat;             /* playback modes (defined below), saved too */

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* accept "#rrggbb" / "rrggbb" / "#rgb" (true colour), "0".."255" (256-colour
 * palette index) or "default". Returns 1 and updates the colour, or 0 if the
 * code is bad. */
static int set_color(Color *c, const char *code) {
    if (strcasecmp(code, "default") == 0) code = c->def;
    if (strcmp(code, "default") == 0) {          /* terminal's own background */
        c->sgr[0] = '\0';
        snprintf(c->code, sizeof c->code, "default");
        force_full = 1;
        return 1;
    }
    int hash = (*code == '#');
    const char *p = code + hash;
    size_t L = strlen(p);
    int h[6], dec = 1, base = c->bg ? 48 : 38;
    if (L == 0 || L > 6) return 0;
    for (size_t i = 0; i < L; i++) {
        if ((h[i] = hexval((unsigned char)p[i])) < 0) return 0;
        if (h[i] > 9) dec = 0;
    }
    if (L == 6 || (hash && L == 3)) {
        int r, g, b;
        if (L == 6) { r = h[0] * 16 + h[1]; g = h[2] * 16 + h[3]; b = h[4] * 16 + h[5]; }
        else        { r = h[0] * 17;        g = h[1] * 17;        b = h[2] * 17; }
        snprintf(c->sgr, sizeof c->sgr, "%d;2;%d;%d;%d", base, r, g, b);
        snprintf(c->code, sizeof c->code, "#%02x%02x%02x", r, g, b);
        force_full = 1;
        return 1;
    }
    if (!hash && dec && L <= 3) {
        int n = atoi(p);
        if (n > 255) return 0;
        snprintf(c->sgr, sizeof c->sgr, "%d;5;%d", base, n);
        snprintf(c->code, sizeof c->code, "%d", n);
        force_full = 1;
        return 1;
    }
    return 0;
}

static const int seek_opts[] = { 5, 10, 15, 30, 60 };
static const int vol_opts[]  = { 1, 2, 5, 10 };

/* oscilloscope tab (see "oscilloscope" below) */
static const char *const sc_mode_names[] = { "Wave", "Stereo", "X-Y" };
static const int sc_span_opts[]  = { 10, 20, 50, 100, 200 };      /* ms across the screen */
static const int sc_gain_opts[]  = { 0, 1, 2, 4, 8 };             /* 0 = auto */
static const int sc_fps_opts[]   = { 15, 30, 60 };
static const int sc_delay_opts[] = { 0, 100, 200, 300, 400, 500 };/* ms the picture trails the clock */
static int sc_mode   = 0;      /* 0 wave (L+R), 1 stereo (L over R), 2 X-Y */
static int sc_span   = 50;
static int sc_gain   = 0;
static int sc_trig   = 1;      /* hold the wave still on a rising zero crossing */
static int sc_blocks = 0;      /* 0 = braille dots, 1 = half blocks */
static int sc_fps    = 30;
static int sc_delay  = 200;
/* X-Y smoothing: the signal is low-passed before it is drawn, so the beam
 * follows the bass and mids in flowing loops instead of scribbling the
 * treble into a solid blob. 0 off, 1 light (2 kHz), 2 medium (700 Hz),
 * 3 heavy (300 Hz). */
static const char *const sc_smooth_names[] = { "off", "light", "medium", "heavy" };
static int sc_smooth = 3;
/* X-Y view: 0 "L/R" is the textbook plot (left across, right up) - honest,
 * but ordinary music is nearly the same in both speakers, so it collapses
 * onto one diagonal. 1 "wide" plots what the speakers disagree on (L-R)
 * across and what they share (L+R) up, each axis scaled to fill the screen,
 * which opens that diagonal up into a full figure. */
static const char *const sc_xyview_names[] = { "L/R", "wide" };
static int sc_xyview = 1;

/* step to the next/previous allowed value (unknown values snap to the first) */
static int cycle_opt(const int *opts, int n, int v, int dir) {
    int i = 0;
    while (i < n && opts[i] != v) i++;
    if (i == n) return opts[0];
    return opts[(i + dir + n) % n];
}

static const char *tab_name(int t) {
    static const char *const names[] = { "Tracks", "Albums", "Folders", "Artists" };
    return (t >= 0 && t < 4) ? names[t] : names[0];
}

static void load_settings(void) {
    FILE *f = fopen(settings_file, "r");
    if (!f) return;
    char ln[PATH_MAX + 32];              /* long enough for a folder path */
    while (fgets(ln, sizeof ln, f)) {
        ln[strcspn(ln, "\r\n")] = '\0';
        char *eq = strchr(ln, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *v = eq + 1;
        int n = atoi(v);
        if      (!strcmp(ln, "accent"))     set_color(&col[SET_ACCENT], v);
        else if (!strcmp(ln, "background")) set_color(&col[SET_BG], v);
        else if (!strcmp(ln, "text"))       set_color(&col[SET_TEXT], v);
        else if (!strcmp(ln, "seek"))       seek_step = cycle_opt(seek_opts, 5, n, 0);
        else if (!strcmp(ln, "volstep"))    vol_step  = cycle_opt(vol_opts, 4, n, 0);
        else if (!strcmp(ln, "start"))      { if (n >= 0 && n < 4) start_tab = n; }
        else if (!strcmp(ln, "music"))      snprintf(music_dir, sizeof music_dir, "%s", v);
        else if (!strcmp(ln, "musicfrom"))  snprintf(music_from, sizeof music_from, "%s", v);
        else if (!strcmp(ln, "scope"))      scope_on = (n != 0);
        else if (!strcmp(ln, "sccolx"))     set_color(&col[3], v);
        else if (!strcmp(ln, "sccoly"))     set_color(&col[4], v);
        else if (!strcmp(ln, "scmode"))     { if (n >= 0 && n < 3) sc_mode = n; }
        else if (!strcmp(ln, "scspan"))     sc_span  = cycle_opt(sc_span_opts, 5, n, 0);
        else if (!strcmp(ln, "scgain"))     sc_gain  = cycle_opt(sc_gain_opts, 5, n, 0);
        else if (!strcmp(ln, "sctrig"))     sc_trig  = (n != 0);
        else if (!strcmp(ln, "scstyle"))    sc_blocks = (n != 0);
        else if (!strcmp(ln, "scfps"))      sc_fps   = cycle_opt(sc_fps_opts, 3, n, 0);
        else if (!strcmp(ln, "scdelay"))    sc_delay = cycle_opt(sc_delay_opts, 6, n, 0);
        else if (!strcmp(ln, "scsmooth"))   { if (n >= 0 && n < 4) sc_smooth = n; }
        else if (!strcmp(ln, "scxyview"))   sc_xyview = (n != 0);
        else if (!strcmp(ln, "shuffle"))    shuffle = (n != 0);
        else if (!strcmp(ln, "repeat"))     { if (n >= 0 && n < 3) repeat = n; }
    }
    fclose(f);
}

static void save_settings(void) {
    FILE *f = fopen(settings_file, "w");
    if (!f) return;
    fprintf(f, "accent=%s\nbackground=%s\ntext=%s\n",
            col[SET_ACCENT].code, col[SET_BG].code, col[SET_TEXT].code);
    fprintf(f, "seek=%d\nvolstep=%d\nstart=%d\nshuffle=%d\nrepeat=%d\n",
            seek_step, vol_step, start_tab, shuffle, repeat);
    fprintf(f, "scope=%d\nscmode=%d\nscspan=%d\nscgain=%d\nsctrig=%d\nscstyle=%d\nscfps=%d\nscdelay=%d\n",
            scope_on, sc_mode, sc_span, sc_gain, sc_trig, sc_blocks, sc_fps, sc_delay);
    fprintf(f, "sccolx=%s\nsccoly=%s\nscsmooth=%d\nscxyview=%d\n", col[3].code, col[4].code, sc_smooth, sc_xyview);
    if (music_dir[0]) fprintf(f, "music=%s\nmusicfrom=%s\n", music_dir, music_from);
    fclose(f);
}

/* change the selected option row by one step (dir = +1 / -1) */
static void setting_step(int dir) {
    switch (set_row()) {
        case SET_SEEK:    seek_step = cycle_opt(seek_opts, 5, seek_step, dir); break;
        case SET_VOLSTEP: vol_step  = cycle_opt(vol_opts, 4, vol_step, dir); break;
        case SET_START:   start_tab = (start_tab + dir + 4) % 4; break;
        case SET_EXP_SCOPE: scope_on = !scope_on; settings_rows(); break;   /* shows / hides the tab and its rows */
        case SET_SC_MODE:  sc_mode  = (sc_mode + dir + 3) % 3; break;
        case SET_SC_SPAN:  sc_span  = cycle_opt(sc_span_opts, 5, sc_span, dir); break;
        case SET_SC_GAIN:  sc_gain  = cycle_opt(sc_gain_opts, 5, sc_gain, dir); break;
        case SET_SC_TRIG:  sc_trig  = !sc_trig; break;
        case SET_SC_STYLE: sc_blocks = !sc_blocks; break;
        case SET_SC_FPS:   sc_fps   = cycle_opt(sc_fps_opts, 3, sc_fps, dir); break;
        case SET_SC_DELAY: sc_delay = cycle_opt(sc_delay_opts, 6, sc_delay, dir); break;
        case SET_SC_SMOOTH: sc_smooth = (sc_smooth + dir + 4) % 4; break;
        case SET_SC_XYVIEW: sc_xyview = !sc_xyview; break;
        default: return;
    }
    set_msg = "saved";
    save_settings();
}

static void settings_reset(void) {
    for (int i = 0; i < 5; i++) set_color(&col[i], col[i].def);
    seek_step = 5; vol_step = 5; start_tab = TAB_TRACKS;
    sc_mode = 0; sc_span = 50; sc_gain = 0; sc_trig = 1; sc_blocks = 0; sc_fps = 30; sc_delay = 200; sc_smooth = 3; sc_xyview = 1;
    scope_on = 0; settings_rows();           /* experimental features go back to off */
    set_msg = "all settings reset";
    save_settings();
}

/* ---- child (ffplay) control, Windows edition ---- */
static HANDLE child_h = NULL;   /* ffplay process handle, NULL = none */
static DWORD  child_pid = 0;
static int paused = 0;
static int intentional = 0;  /* we killed ffplay, don't auto-advance */
static int play_gen = 0;     /* bumped on every (re)start: the scope decoder follows it */
static void scope_stop(void);
static int volume = 100;
enum { REP_OFF, REP_ALL, REP_ONE };
static int repeat = REP_OFF; /* off / repeat-all (loop queue) / repeat-one (track) */
static int shuffle = 0;      /* random play order */

/* play queue: the sequence next/prev walk through. It mirrors the current
 * view[] (so search results become the queue), optionally shuffled. */
static int *queue = NULL;
static int nqueue = 0;

/* playback clock (seconds, monotonic) */
static double t_start_sec = 0;
static double base_elapsed = 0;

/* ---- console state ---- */
static HANDLE hOut = INVALID_HANDLE_VALUE, hIn = INVALID_HANDLE_VALUE;
static DWORD  orig_out_mode = 0, orig_in_mode = 0;
static UINT   orig_out_cp = 0;

/* ---- extensions ---- */
static int is_audio(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    static const char *ext[] = {
        ".mp3",".flac",".ogg",".oga",".wav",".m4a",
        ".aac",".opus",".wma",".webm",".mp4",NULL
    };
    for (int i = 0; ext[i]; i++)
        if (strcasecmp(dot, ext[i]) == 0) return 1;
    return 0;
}

/* last path separator, accepting both / and \ (Windows paths mix them) */
static char *last_sep(char *s) {
    char *a = strrchr(s, '/');
    char *b = strrchr(s, '\\');
    return (a > b) ? a : b;
}

/* filename without extension, derived from path (no stored copy) */
static const char *track_name(const Track *t, char *buf, size_t n) {
    const char *p = last_sep(t->path);
    p = p ? p + 1 : t->path;
    const char *dot = strrchr(p, '.');
    size_t L = dot ? (size_t)(dot - p) : strlen(p);
    if (L >= n) L = n - 1;
    memcpy(buf, p, L);
    buf[L] = '\0';
    return buf;
}

/* display title: prefer the tag title, fall back to the filename */
static const char *disp_title(const Track *t, char *buf, size_t n) {
    return (t->title && *t->title) ? t->title : track_name(t, buf, n);
}

/* ---- collection ---- */
static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) { perror("malloc"); exit(1); }
    return p;
}

static char *xstrdup(const char *s) {
    char *p = malloc(strlen(s) + 1);
    if (!p) { perror("malloc"); exit(1); }
    strcpy(p, s);
    return p;
}

/* ---- string arena ----
 * Per-track strings (path, title) live in big bump-allocated blocks: one
 * allocation per 64 KiB instead of one per string, so a large library costs
 * its text and nothing else — no per-string heap headers, no fragmentation.
 * Nothing in the arena is freed individually; the blocks are released
 * together at exit. */
static char *arena_ptr = NULL;
static size_t arena_left = 0;
static char **arena_blocks = NULL;
static int nblocks = 0, capblocks = 0;

static char *strpool(const char *s) {
    size_t n = strlen(s) + 1;
    if (n > arena_left) {
        size_t bs = (n > (64 << 10)) ? n : (64 << 10);
        char *b = malloc(bs);
        if (!b) { perror("malloc"); exit(1); }
        if (nblocks == capblocks) {
            capblocks = capblocks ? capblocks * 2 : 16;
            arena_blocks = realloc(arena_blocks, capblocks * sizeof(char *));
            if (!arena_blocks) { perror("realloc"); exit(1); }
        }
        arena_blocks[nblocks++] = b;
        arena_ptr = b; arena_left = bs;
    }
    char *p = arena_ptr;
    memcpy(p, s, n);
    arena_ptr += n; arena_left -= n;
    return p;
}

static void free_arena(void) {
    for (int i = 0; i < nblocks; i++) free(arena_blocks[i]);
    free(arena_blocks);
    arena_blocks = NULL; nblocks = capblocks = 0;
    arena_ptr = NULL; arena_left = 0;
}

/* ---- interning for names that repeat across tracks ----
 * Folder, artist and album names are shared by every track in the group;
 * interning stores each distinct name once (in the arena) and hands out
 * that pointer, so a 10,000-track library with 500 albums pays for 500
 * album names, not 10,000 copies of them. */
typedef struct Inode { char *s; struct Inode *next; } Inode;
static Inode **itab = NULL;
static size_t itab_mask = 0;
static size_t icount = 0;

static unsigned long long str_hash(const char *s) {
    unsigned long long h = 1469598103934665603ULL;      /* FNV-1a 64 */
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

static char *intern(const char *s) {
    if (!s || !*s) return (char *)"";
    if (!itab) {
        itab_mask = 2047;
        itab = calloc(itab_mask + 1, sizeof(Inode *));
        if (!itab) { perror("calloc"); exit(1); }
    }
    size_t i = (size_t)(str_hash(s) & itab_mask);
    for (Inode *n = itab[i]; n; n = n->next)
        if (strcmp(n->s, s) == 0) return n->s;
    /* grow when chains get long */
    if (icount > (itab_mask + 1) * 2) {
        size_t nm = itab_mask * 2 + 1;
        Inode **nt = calloc(nm + 1, sizeof(Inode *));
        if (!nt) { perror("calloc"); exit(1); }
        for (size_t b = 0; b <= itab_mask; b++)
            for (Inode *n = itab[b]; n; ) {
                Inode *nx = n->next;
                size_t j = (size_t)(str_hash(n->s) & nm);
                n->next = nt[j]; nt[j] = n;
                n = nx;
            }
        free(itab);
        itab = nt; itab_mask = nm;
        i = (size_t)(str_hash(s) & itab_mask);
    }
    Inode *n = malloc(sizeof *n);
    if (!n) { perror("malloc"); exit(1); }
    n->s = strpool(s);
    n->next = itab[i];
    itab[i] = n;
    icount++;
    return n->s;
}

static void free_intern(void) {
    if (!itab) return;
    for (size_t b = 0; b <= itab_mask; b++)
        for (Inode *n = itab[b]; n; ) {
            Inode *nx = n->next;
            free(n);
            n = nx;
        }
    free(itab);
    itab = NULL; itab_mask = 0; icount = 0;
}

static void add_track(const char *path) {
    if (ntracks == capacity) {
        /* grow via VirtualAlloc: the old block is released to the OS on the
         * spot, where a CRT realloc would leave the freed copy committed */
        int newcap = capacity ? capacity * 2 : 4096;
        Track *nu = VirtualAlloc(NULL, (SIZE_T)newcap * sizeof(Track),
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!nu) { fprintf(stderr, "out of memory\n"); exit(1); }
        if (tracks) {
            memcpy(nu, tracks, (size_t)ntracks * sizeof(Track));
            VirtualFree(tracks, 0, MEM_RELEASE);
        }
        tracks = nu;
        capacity = newcap;
    }
    Track *t = &tracks[ntracks++];
    t->path = strpool(path);

    /* parent folder name (album fallback), shared by every track in it */
    char pbuf[2048];
    snprintf(pbuf, sizeof pbuf, "%s", path);
    char *slash = last_sep(pbuf);                /* strip filename */
    if (slash) {
        *slash = '\0';
        char *par = last_sep(pbuf);              /* parent dir basename */
        t->folder = intern(par ? par + 1 : pbuf);
    } else {
        t->folder = intern("");
    }

    t->mtime = 0;
    t->dur = -1;
    t->title = t->artist = t->album = NULL;
    t->group = t->folder;        /* refined to album once tags are read */
    t->trackno = 0;
    t->probed = 0;
}

static void scan_dir(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[2048];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        DIR *sub = opendir(full);
        if (sub) {                 /* it's a directory */
            closedir(sub);
            if (depth > 0) scan_dir(full, depth - 1);
        } else if (is_audio(e->d_name)) {
            add_track(full);
        }
    }
    closedir(d);
}

/* does this folder hold at least one audio file, as deep as the scan looks? */
static int dir_has_audio(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[2048];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        DIR *sub = opendir(full);
        if (sub) {
            closedir(sub);
            if (depth > 0) found = dir_has_audio(full, depth - 1);
        } else if (is_audio(e->d_name)) found = 1;
    }
    closedir(d);
    return found;
}

/* Settings > Music folder, Enter: check what was typed and, if it is a folder
 * with music in it, save it and ask the main loop to start over on it.
 * Nothing is changed (and aqua keeps playing) when the path is no good. */
static void music_apply(void) {
    char in[sizeof pbuf];
    snprintf(in, sizeof in, "%s", pbuf);
    char *p = in;
    while (*p == ' ' || *p == '"' || *p == '\'') p++;             /* pasted paths come quoted */
    size_t L = strlen(p);
    while (L && (p[L - 1] == ' ' || p[L - 1] == '"' || p[L - 1] == '\'')) p[--L] = '\0';
    if (L == 0 || strcasecmp(p, "default") == 0) {               /* back to the launch folder */
        if (!music_dir[0]) { set_msg = "already using the folder aqua was started on"; return; }
        music_dir[0] = music_from[0] = '\0';
        save_settings();
        relaunch = 1;
        return;
    }
    char abs[PATH_MAX];
    if (!_fullpath(abs, p, PATH_MAX)) { set_msg = "no such folder"; return; }
    DIR *d = opendir(abs);
    if (!d) { set_msg = "that is not a folder"; return; }
    closedir(d);
    if (!dir_has_audio(abs, 6)) { set_msg = "no audio files in that folder"; return; }
    if (strcmp(abs, lib_dir) == 0 && strcmp(abs, music_dir) == 0) { set_msg = "already playing that folder"; return; }
    snprintf(music_dir, sizeof music_dir, "%s", abs);
    snprintf(music_from, sizeof music_from, "%s", launch_arg);
    save_settings();
    relaunch = 1;
}

static int cmp_track(const void *a, const void *b) {
    char na[1024], nb[1024];
    return strcasecmp(track_name((const Track *)a, na, sizeof na),
                      track_name((const Track *)b, nb, sizeof nb));
}

/* group = album when tagged, otherwise the parent folder name */
static void apply_group(Track *t) {
    t->group = (t->album && *t->album) ? t->album : t->folder;
}

/* arena-copy a string with tabs/newlines flattened to spaces, or NULL if empty */
static char *dup_clean(const char *s) {
    if (!s || !*s) return NULL;
    char *p = strpool(s);
    for (char *q = p; *q; q++) if (*q == '\t' || *q == '\n' || *q == '\r') *q = ' ';
    return p;
}

/* same, but the result is interned (artist/album repeat across tracks) */
static char *intern_clean(const char *s) {
    if (!s || !*s) return NULL;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *q = buf; *q; q++)
        if (*q == '\t' || *q == '\n' || *q == '\r') *q = ' ';
    return intern(buf);
}

/* ---- metadata via ffprobe (duration + title/artist/album/track) ----
 * ffprobe is launched with CreateProcess and its output read over a pipe —
 * there is no cmd.exe between us and it, so filenames can't inject commands
 * (a name like $(touch pwned).mp3 is just a name). Returns 1 when the probe
 * is definitive (ffprobe ran, even if it found no duration), 0 when ffprobe
 * itself couldn't run — the file is then retried next session. */

/* append s to a Windows command line, quoted the way MSVCRT argv parsing
 * expects: inner quotes are escaped and the backslashes before them doubled */
static size_t quote_arg(char *dst, size_t cap, size_t o, const char *s) {
    size_t nbs = 0;
    if (o + 2 >= cap) return o;
    dst[o++] = '"';
    for (const char *p = s; *p; p++) {
        if (*p == '\\') {
            if (o + 4 >= cap) break;
            nbs++;
            dst[o++] = '\\';
        } else if (*p == '"') {
            if (o + nbs + 3 >= cap) break;
            while (nbs--) dst[o++] = '\\';     /* double pending backslashes */
            nbs = 0;
            dst[o++] = '\\'; dst[o++] = '"';
        } else {
            if (o + 2 >= cap) break;
            nbs = 0;
            dst[o++] = *p;
        }
    }
    while (nbs-- && o + 1 < cap) dst[o++] = '\\';   /* double trailing ones */
    if (o + 1 < cap) dst[o++] = '"';
    dst[o] = '\0';
    return o;
}

static void probe_line(Track *t, char *line) {
    char *eq = strchr(line, '=');
    if (!eq) return;
    *eq = '\0';
    char *key = line, *val = eq + 1;
    size_t L = strlen(val);
    while (L && (val[L-1] == '\n' || val[L-1] == '\r')) val[--L] = '\0';
    if (strcmp(key, "duration") == 0)             t->dur = atof(val) > 0 ? atof(val) : -1;
    else if (strcasecmp(key, "TAG:title") == 0)   t->title  = dup_clean(val);
    else if (strcasecmp(key, "TAG:artist") == 0)  t->artist = intern_clean(val);
    else if (strcasecmp(key, "TAG:album") == 0)   t->album  = intern_clean(val);
    else if (strcasecmp(key, "TAG:track") == 0)   t->trackno = atoi(val);  /* "3/12" -> 3 */
}

static int probe_meta(Track *t) {
    char cmd[PATH_MAX + 256];
    size_t o = snprintf(cmd, sizeof cmd,
        "ffprobe -v error -show_entries "
        "format=duration:format_tags=title,artist,album,track,TITLE,ARTIST,ALBUM,TRACK "
        "-of default=noprint_wrappers=1 ");
    o = quote_arg(cmd, sizeof cmd, o, t->path);
    (void)o;

    SECURITY_ATTRIBUTES sa; memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa; sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, NULL);

    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = (nul != INVALID_HANDLE_VALUE) ? nul : wr;
    si.hStdInput  = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        return 0;                        /* ffprobe missing: retried next run */
    }
    CloseHandle(pi.hThread);
    CloseHandle(wr);                     /* our copy: child's stays open */

    /* read the whole answer, then reap */
    int definitive = 0;
    char buf[1024], line[1024];
    size_t lin = 0;
    DWORD n;
    while (ReadFile(rd, buf, sizeof buf, &n, NULL) && n) {
        for (DWORD i = 0; i < n; i++) {
            char c = buf[i];
            if (c != '\n' && lin + 1 < sizeof line) line[lin++] = c;
            else {
                line[lin] = '\0'; lin = 0;
                probe_line(t, line);
            }
        }
    }
    if (lin) { line[lin] = '\0'; probe_line(t, line); }   /* unterminated tail */
    if (WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0) definitive = 1;
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    apply_group(t);
    return definitive;
}

/* ---- metadata cache (%LOCALAPPDATA%\aqua\library.tsv) ----
 * Each line: <mtime>\t<dur>\t<trackno>\t<title>\t<artist>\t<album>\t<abspath>
 * Only new or modified files are probed; everything else is read from here.
 * Files ffprobe couldn't get a duration from are stored with dur -1 so they
 * aren't re-probed on every launch either. Text fields are escaped with
 * put_escaped(), so newlines/tabs/backslashes can never corrupt a line. */
typedef struct {
    char *path; long mtime; double dur; int trackno;
    char *title, *artist, *album;
    int dead;      /* consumed by a track this session; dropped after merge */
} CacheEnt;
static CacheEnt *cache = NULL;
static int ncache = 0, capcache = 0;
static char cache_file[PATH_MAX];

static int cmp_cache(const void *a, const void *b) {
    return strcmp(((const CacheEnt *)a)->path, ((const CacheEnt *)b)->path);
}

static void init_cache_path(void) {
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !*base) base = getenv("USERPROFILE");
    if (!base || !*base) base = ".";
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%.*s\\aqua", (int)(sizeof dir - 32), base);
    _mkdir(dir);
    snprintf(cache_file, sizeof cache_file, "%.*s\\library.tsv",
             (int)(sizeof cache_file - 20), dir);
    snprintf(settings_file, sizeof settings_file, "%.*s\\settings",
             (int)(sizeof settings_file - 20), dir);
}

/* take the next tab-delimited field, advancing *pp past the tab */
static char *next_field(char **pp) {
    char *s = *pp;
    char *tab = strchr(s, '\t');
    if (tab) { *tab = '\0'; *pp = tab + 1; }
    else     { *pp = s + strlen(s); }
    return s;
}

/* write s with \, \n, \t, \r escaped so one cache line is always one file */
static void put_escaped(FILE *f, const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '\n': fputc('\\', f); fputc('n', f); break;
        case '\t': fputc('\\', f); fputc('t', f); break;
        case '\r': fputc('\\', f); fputc('r', f); break;
        case '\\': fputc('\\', f); fputc('\\', f); break;
        default:   fputc(*s, f);
        }
    }
}

/* inverse of put_escaped, in place (the result is never longer); unknown
 * escapes are kept byte-for-byte */
static char *unescape_in(char *s) {
    char *w = s, *r = s;
    while (*r) {
        if (r[0] == '\\' && r[1] == 'n')      { *w++ = '\n'; r += 2; }
        else if (r[0] == '\\' && r[1] == 't')  { *w++ = '\t'; r += 2; }
        else if (r[0] == '\\' && r[1] == 'r')  { *w++ = '\r'; r += 2; }
        else if (r[0] == '\\' && r[1] == '\\') { *w++ = '\\'; r += 2; }
        else *w++ = *r++;
    }
    *w = '\0';
    return s;
}

/* the loaded cache: one whole-file buffer. Entries point into it, so a
 * warm 5,000-track cache costs one large (OS-returnable) allocation
 * instead of four small ones per track — and freeing it after the merge
 * hands the pages straight back instead of leaving heap holes. */
static char *cache_buf = NULL;

static void load_cache(void) {
    FILE *f = fopen(cache_file, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return; }
    if (sz == 0) { fclose(f); return; }
    /* VirtualAlloc, not malloc: this block is freed once, whole, right after
     * the merge — VirtualFree hands the pages straight back to the OS, where
     * a CRT-heap free would leave them committed */
    cache_buf = VirtualAlloc(NULL, (SIZE_T)sz + 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!cache_buf) { fclose(f); return; }
    size_t got = fread(cache_buf, 1, (size_t)sz, f);
    fclose(f);
    if (got == 0) { VirtualFree(cache_buf, 0, MEM_RELEASE); cache_buf = NULL; return; }
    cache_buf[got] = '\n';               /* every line ends with \n */
    cache_buf[got + 1] = '\0';

    /* exact array size: no growth churn on load. VirtualAlloc like the
     * buffer: freed whole after the merge, so the pages return to the OS */
    int lines = 1;
    for (size_t i = 0; i < got; i++)
        if (cache_buf[i] == '\n') lines++;
    capcache = lines;
    cache = VirtualAlloc(NULL, (SIZE_T)capcache * sizeof(CacheEnt),
                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!cache) { VirtualFree(cache_buf, 0, MEM_RELEASE); cache_buf = NULL; return; }

    ncache = 0;
    char *p = cache_buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        else break;
        if (nl > p && nl[-1] == '\r') nl[-1] = '\0';   /* CRLF caches (text-mode writes) */
        char *q = p;
        p = nl + 1;
        long  mt   = strtol(next_field(&q), NULL, 10);
        double dur = strtod(next_field(&q), NULL);
        int   trk  = atoi(next_field(&q));
        char *title  = next_field(&q);
        char *artist = next_field(&q);
        char *album  = next_field(&q);
        char *path   = q;                 /* remainder is the path */
        if (!*path) continue;
        CacheEnt *e = &cache[ncache++];
        e->path   = unescape_in(path);    /* non-empty: checked above */
        e->mtime  = mt; e->dur = dur; e->trackno = trk;
        e->title  = *title  ? unescape_in(title)  : NULL;
        e->artist = *artist ? unescape_in(artist) : NULL;
        e->album  = *album  ? unescape_in(album)  : NULL;
        e->dead = 0;
    }
    if (ncache == 0) { VirtualFree(cache, 0, MEM_RELEASE); cache = NULL; capcache = 0; }
    if (ncache) qsort(cache, ncache, sizeof(CacheEnt), cmp_cache);
}

static CacheEnt *cache_lookup(const char *path, long mtime) {
    if (ncache == 0) return NULL;
    CacheEnt key; key.path = (char *)path;
    CacheEnt *e = bsearch(&key, cache, ncache, sizeof(CacheEnt), cmp_cache);
    return (e && e->mtime == mtime) ? e : NULL;
}

/* insert or update an entry from a track (linear; runs once at save time) */
static void cache_upsert(const Track *t, const char *abspath) {
    CacheEnt *e = NULL;
    for (int i = 0; i < ncache; i++)
        if (strcmp(cache[i].path, abspath) == 0) { e = &cache[i]; break; }
    if (!e) {
        if (ncache == capcache) {
            capcache = capcache ? capcache * 2 : 256;
            cache = realloc(cache, capcache * sizeof(CacheEnt));
            if (!cache) { perror("realloc"); exit(1); }
        }
        e = &cache[ncache++];
        e->path = xstrdup(abspath);
        e->title = e->artist = e->album = NULL;
    } else {
        free(e->title); free(e->artist); free(e->album);   /* cache owns its strings */
    }
    e->mtime = t->mtime; e->dur = t->dur; e->trackno = t->trackno;
    e->title  = t->title  ? xstrdup(t->title)  : NULL;
    e->artist = t->artist ? xstrdup(t->artist) : NULL;
    e->album  = t->album  ? xstrdup(t->album)  : NULL;
    e->dead = 0;
}

static void save_cache(void) {
    if (!cache_file[0]) return;
    for (int i = 0; i < ntracks; i++) {
        if (!tracks[i].probed) continue;
        char rp[PATH_MAX];
        const char *ap = _fullpath(rp, tracks[i].path, PATH_MAX) ? rp : tracks[i].path;
        cache_upsert(&tracks[i], ap);
    }
    FILE *f = fopen(cache_file, "w");
    if (!f) return;
    for (int i = 0; i < ncache; i++) {
        CacheEnt *e = &cache[i];
        fprintf(f, "%ld\t%.3f\t%d\t", e->mtime, e->dur, e->trackno);
        put_escaped(f, e->title  ? e->title  : ""); fputc('\t', f);
        put_escaped(f, e->artist ? e->artist : ""); fputc('\t', f);
        put_escaped(f, e->album  ? e->album  : ""); fputc('\t', f);
        put_escaped(f, e->path); fputc('\n', f);
    }
    fclose(f);
}

/* ---- terminal (Windows console) ---- */
static void term_raw(void) {
    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    hIn  = GetStdHandle(STD_INPUT_HANDLE);

    if (GetConsoleMode(hOut, &orig_out_mode))
        SetConsoleMode(hOut, orig_out_mode | ENABLE_PROCESSED_OUTPUT
                                           | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    if (GetConsoleMode(hIn, &orig_in_mode))
        SetConsoleMode(hIn, orig_in_mode & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT
                                           | ENABLE_PROCESSED_INPUT
                                           | ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT));

    orig_out_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);          /* render the ♬ and box-drawing glyphs */
    _setmode(_fileno(stdout), _O_BINARY); /* write our own \r\n, no translation */

    printf("\033[?25l");      /* hide cursor */
    fflush(stdout);
}

static void term_restore(void) {
    if (col[SET_BG].sgr[0]) printf("\033[0m\033[2J\033[H");   /* don't leave our background behind */
    printf("\033[?25h\033[0m\r\n");  /* show cursor */
    fflush(stdout);
    if (hOut != INVALID_HANDLE_VALUE) SetConsoleMode(hOut, orig_out_mode);
    if (hIn  != INVALID_HANDLE_VALUE) SetConsoleMode(hIn,  orig_in_mode);
    if (orig_out_cp) SetConsoleOutputCP(orig_out_cp);
}

static void get_size(int *rows, int *cols) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
        *rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        *cols = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
        if (*rows < 1) *rows = 24;
        if (*cols < 1) *cols = 80;
    } else { *rows = 24; *cols = 80; }
}

/* ---- playback clock ---- */
static double now_sec(void) {
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

static double elapsed(void) {
    if (cur < 0) return 0;
    if (paused) return base_elapsed;
    return base_elapsed + (now_sec() - t_start_sec);
}

/* ---- suspend / resume a process by suspending all its threads ----
 * Windows has no SIGSTOP; walk the thread table and Suspend/Resume each
 * thread of the child. This is the faithful analogue of pause/continue. */
static void suspend_process(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) {
                HANDLE th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (th) { SuspendThread(th); CloseHandle(th); }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}
static void resume_process(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) {
                HANDLE th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (th) { ResumeThread(th); CloseHandle(th); }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

/* ---- live volume via the Windows Core Audio session of ffplay ----
 * Every audio process has a "session" on the default output device with its
 * own volume knob (the one in the Volume Mixer). We find ffplay's session by
 * pid and turn that knob, so the stream is never interrupted. The GUIDs are
 * spelled out and ole32 is loaded at runtime so the build command stays
 * plain `gcc -O2 -Wall -o aqua.exe aqua_win.c`. */
static const GUID G_CLSID_MMDevEnum = {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const GUID G_IID_MMDevEnum   = {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const GUID G_IID_SessMgr2    = {0x77AA99A0,0x1BD6,0x484F,{0x8B,0xC7,0x2C,0x65,0x4C,0x9A,0x9B,0x6F}};
static const GUID G_IID_SessCtl2    = {0xBFB7FF88,0x7239,0x4FC9,{0x8F,0xA2,0x07,0xC9,0x50,0xBE,0x9C,0x6D}};
static const GUID G_IID_SimpleVol   = {0x87CE5498,0x68D6,0x44E5,{0x92,0x15,0x6D,0xA4,0x7E,0xF8,0x83,0xD8}};

typedef HRESULT (WINAPI *PFN_CoInit)(LPVOID, DWORD);
typedef HRESULT (WINAPI *PFN_CoCreate)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
static PFN_CoCreate p_CoCreate = NULL;
static int com_state = 0;        /* 0 = untried, 1 = ready, -1 = unavailable */

static int com_init(void) {
    if (com_state) return com_state > 0;
    HMODULE ole = LoadLibraryA("ole32.dll");
    PFN_CoInit ci = ole ? (PFN_CoInit)(void *)GetProcAddress(ole, "CoInitializeEx") : NULL;
    p_CoCreate = ole ? (PFN_CoCreate)(void *)GetProcAddress(ole, "CoCreateInstance") : NULL;
    HRESULT hr = (ci && p_CoCreate) ? ci(NULL, COINIT_MULTITHREADED) : E_FAIL;
    com_state = (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) ? 1 : -1;
    return com_state > 0;
}

/* Read (do_set = 0, into *out) or write (do_set = 1, `level` 0..1) the session
 * volume of process `pid`. Returns 1 on success, 0 if the process has no audio
 * session yet (ffplay opens it a moment after it starts). */
static int session_volume_op(DWORD pid, int do_set, float level, float *out) {
    int ok = 0, n = 0;
    IMMDeviceEnumerator *en = NULL; IMMDevice *dev = NULL;
    IAudioSessionManager2 *mgr = NULL; IAudioSessionEnumerator *se = NULL;
    if (!pid || !com_init()) return 0;
    if (FAILED(p_CoCreate(&G_CLSID_MMDevEnum, NULL, CLSCTX_ALL, &G_IID_MMDevEnum, (void **)&en))) goto done;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eMultimedia, &dev))) goto done;
    if (FAILED(IMMDevice_Activate(dev, &G_IID_SessMgr2, CLSCTX_ALL, NULL, (void **)&mgr))) goto done;
    if (FAILED(IAudioSessionManager2_GetSessionEnumerator(mgr, &se))) goto done;
    IAudioSessionEnumerator_GetCount(se, &n);
    for (int i = 0; i < n && !ok; i++) {
        IAudioSessionControl *ctl = NULL; IAudioSessionControl2 *ctl2 = NULL;
        ISimpleAudioVolume *vol = NULL;
        DWORD spid = 0;
        if (FAILED(IAudioSessionEnumerator_GetSession(se, i, &ctl))) continue;
        if (SUCCEEDED(IAudioSessionControl_QueryInterface(ctl, &G_IID_SessCtl2, (void **)&ctl2))
            && SUCCEEDED(IAudioSessionControl2_GetProcessId(ctl2, &spid)) && spid == pid
            && SUCCEEDED(IAudioSessionControl_QueryInterface(ctl, &G_IID_SimpleVol, (void **)&vol))) {
            if (do_set) {
                ok = SUCCEEDED(ISimpleAudioVolume_SetMasterVolume(vol, level, NULL));
            } else {
                float v = 0;
                ok = SUCCEEDED(ISimpleAudioVolume_GetMasterVolume(vol, &v));
                if (out) *out = v;
            }
        }
        if (vol)  ISimpleAudioVolume_Release(vol);
        if (ctl2) IAudioSessionControl2_Release(ctl2);
        IAudioSessionControl_Release(ctl);
    }
done:
    if (se)  IAudioSessionEnumerator_Release(se);
    if (mgr) IAudioSessionManager2_Release(mgr);
    if (dev) IMMDevice_Release(dev);
    if (en)  IMMDeviceEnumerator_Release(en);
    return ok;
}

/* volume bookkeeping: `volume` (declared above) is what the UI shows. */
static int    vol_known = -1;    /* level Windows will give a new ffplay session, -1 = unknown */
static int    vol_touched = 0;   /* the user changed the volume during this run */
static int    vol_sync = 0;      /* the live stream still needs to be brought to `volume` */
static double vol_sync_t0 = 0;

static void request_vol_sync(void) { vol_sync = 1; vol_sync_t0 = now_sec(); }

/* Called every loop tick while vol_sync is set (and right after a key press).
 * Until ffplay's session exists this quietly retries — it never restarts the
 * track, which is what used to cut the sound out. */
static void sync_volume(void) {
    if (!vol_sync) return;
    if (!child_h) { vol_sync = 0; return; }
    if (vol_known < 0 && !vol_touched) {
        /* first stream of this run: show what Windows actually remembered
         * rather than stomping it with our default of 100 */
        float v;
        if (session_volume_op(child_pid, 0, 0, &v)) {
            volume = (int)(v * 100.0f + 0.5f);
            vol_known = volume;
            vol_sync = 0;
            return;
        }
    } else if (session_volume_op(child_pid, 1, volume / 100.0f, NULL)) {
        vol_known = volume;
        vol_sync = 0;
        return;
    }
    if (now_sec() - vol_sync_t0 > 5.0) vol_sync = 0;   /* no audio session: give up */
}

/* ---- child control ---- */
static void stop_child(void) {
    if (child_h) {
        intentional = 1;
        if (paused) resume_process(child_pid);
        TerminateProcess(child_h, 0);
        WaitForSingleObject(child_h, 2000);
        CloseHandle(child_h);
        child_h = NULL;
        child_pid = 0;
    }
    paused = 0;
}

static void play_track(int idx, double startpos) {
    if (idx < 0 || idx >= ntracks) return;
    stop_child();
    cur = idx;
    play_gen++;
    base_elapsed = startpos > 0 ? startpos : 0;
    t_start_sec = now_sec();
    paused = 0;

    /* Build a single command line for CreateProcess. ffplay is found on PATH.
     * No -volume: the level is applied through the Windows audio session
     * (see sync_volume), so every track and every seek shares one scale. */
    char cmd[PATH_MAX + 256];
    if (startpos > 0)
        snprintf(cmd, sizeof cmd,
            "ffplay -nodisp -autoexit -loglevel quiet -ss %.2f \"%s\"",
            startpos, tracks[idx].path);
    else
        snprintf(cmd, sizeof cmd,
            "ffplay -nodisp -autoexit -loglevel quiet \"%s\"",
            tracks[idx].path);

    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        child_h = pi.hProcess;
        child_pid = pi.dwProcessId;
        request_vol_sync();      /* bring the new stream to `volume` once it exists */
    } else {
        child_h = NULL;
        child_pid = 0;
    }
    intentional = 0;
}

static void toggle_pause(void) {
    if (!child_h) return;
    if (paused) {
        t_start_sec = now_sec();
        resume_process(child_pid);
        paused = 0;
    } else {
        base_elapsed = elapsed();
        suspend_process(child_pid);
        paused = 1;
    }
}

/* position of a track index within the play queue, or -1 if not queued */
static int queue_pos(int track) {
    for (int i = 0; i < nqueue; i++)
        if (queue[i] == track) return i;
    return -1;
}

/* (re)build the play queue from the current view; keep `cur` first so the
 * next pick is fresh. Call this whenever the view or shuffle state changes. */
static void build_queue(void) {
    nqueue = nview;
    for (int i = 0; i < nview; i++) queue[i] = view[i];
    if (shuffle) {
        for (int i = nqueue - 1; i > 0; i--) {     /* Fisher-Yates */
            int j = rand() % (i + 1);
            int tmp = queue[i]; queue[i] = queue[j]; queue[j] = tmp;
        }
    }
    if (cur >= 0) {                                 /* move current track to front */
        int p = queue_pos(cur);
        if (p > 0) { int tmp = queue[0]; queue[0] = queue[p]; queue[p] = tmp; }
    }
}

/* case-insensitive substring match */
static int ci_contains(const char *hay, const char *needle) {
    if (!*needle) return 1;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return 1;
    }
    return 0;
}

/* a track matches the query if any of its fields contain it */
static int track_matches(const Track *t, const char *q) {
    if (!*q) return 1;
    char nb[1024];
    return ci_contains(track_name(t, nb, sizeof nb), q)
        || (t->title  && ci_contains(t->title, q))
        || (t->artist && ci_contains(t->artist, q))
        || (t->album  && ci_contains(t->album, q));
}

/* compare two tracks by (track number, then title) */
static int cmp_by_trackno(const Track *ta, const Track *tb) {
    if (ta->trackno != tb->trackno) {
        if (ta->trackno == 0) return 1;
        if (tb->trackno == 0) return -1;
        return ta->trackno - tb->trackno;
    }
    char ba[1024], bb[1024];
    return strcasecmp(disp_title(ta, ba, sizeof ba), disp_title(tb, bb, sizeof bb));
}

/* order the view depending on context:
 *   0 = by title (Tracks tab)
 *   1 = by track number (opened album)
 *   2 = by album, then track number (opened artist)
 *   3 = by filename (opened folder) */
static int cmp_view(const void *a, const void *b) {
    const Track *ta = &tracks[*(const int *)a];
    const Track *tb = &tracks[*(const int *)b];
    if (view_sort == 1) return cmp_by_trackno(ta, tb);
    if (view_sort == 2) {
        int c = strcasecmp(ta->group, tb->group);
        if (c) return c;
        return cmp_by_trackno(ta, tb);
    }
    if (view_sort == 3) {
        char fa[1024], fb[1024];
        return strcasecmp(track_name(ta, fa, sizeof fa), track_name(tb, fb, sizeof fb));
    }
    char ba[1024], bb[1024];
    int c = strcasecmp(disp_title(ta, ba, sizeof ba), disp_title(tb, bb, sizeof bb));   /* Tracks tab: by title */
    if (c) return c;
    char na[1024], nb[1024];
    return strcasecmp(track_name(ta, na, sizeof na), track_name(tb, nb, sizeof nb));
}

/* rebuild the track view: all tracks (Tracks tab) or one album's tracks
 * (when an album is opened), filtered by the query. */
static void rebuild_view(void) {
    if (!view) view = xmalloc((ntracks ? ntracks : 1) * sizeof(int));
    nview = 0;
    for (int i = 0; i < ntracks; i++) {
        if (in_album && cur_album &&
            strcasecmp(browse_key(&tracks[i]), cur_album) != 0) continue;
        if (!track_matches(&tracks[i], query)) continue;
        view[nview++] = i;
    }
    /* opened album -> by track no; opened artist -> by album then track no */
    view_sort = !in_album ? 0 : (tab == TAB_ARTISTS ? 2 : tab == TAB_FOLDERS ? 3 : 1);
    qsort(view, nview, sizeof(int), cmp_view);
    if (sel >= nview) sel = nview ? nview - 1 : 0;
    if (sel < 0) sel = 0;
    build_queue();   /* the visible list is the play queue */
}

/* build the distinct browse list (albums or artists), sorted, with counts */
static int cmp_group_idx(const void *a, const void *b) {
    return strcasecmp(browse_key(&tracks[*(const int *)a]),
                      browse_key(&tracks[*(const int *)b]));
}
static void build_albums(void) {
    static int *tmp = NULL;
    if (!albums) {
        albums  = xmalloc((ntracks ? ntracks : 1) * sizeof(char *));
        album_n = xmalloc((ntracks ? ntracks : 1) * sizeof(int));
        alview  = xmalloc((ntracks ? ntracks : 1) * sizeof(int));
        tmp     = xmalloc((ntracks ? ntracks : 1) * sizeof(int));
    }
    for (int i = 0; i < ntracks; i++) tmp[i] = i;
    qsort(tmp, ntracks, sizeof(int), cmp_group_idx);
    nalbums = 0;
    for (int k = 0; k < ntracks; k++) {
        const char *g = browse_key(&tracks[tmp[k]]);
        if (nalbums == 0 || strcasecmp(g, albums[nalbums - 1]) != 0) {
            albums[nalbums] = (char *)g; album_n[nalbums] = 1; nalbums++;
        } else album_n[nalbums - 1]++;
    }
}

/* filter the album list by the query into alview */
static void rebuild_albums_view(void) {
    nalview = 0;
    for (int i = 0; i < nalbums; i++)
        if (ci_contains(albums[i], query)) alview[nalview++] = i;
    if (alsel >= nalview) alsel = nalview ? nalview - 1 : 0;
    if (alsel < 0) alsel = 0;
}

/* apply the current query to whichever list is active */
static void apply_query(void) {
    if (on_album_list()) rebuild_albums_view();
    else                 rebuild_view();
}

static void clear_query(void) { qlen = 0; query[0] = '\0'; }

/* switch to a tab, resetting drill-down and any active search */
static void switch_tab(int t) {
    if (t != TAB_SCOPE) scope_stop();        /* the decoder only runs while it's watched */
    tab = t; in_album = 0;
    free(cur_album); cur_album = NULL;
    searching = 0; clear_query();
    if (tab == TAB_TRACKS) rebuild_view();
    else if (tab == TAB_SETTINGS) { setsel = 0; editing = 0; set_msg = ""; settings_rows(); }
    else if (tab == TAB_SCOPE) { /* nothing to build: scope_pump() starts the decoder */ }
    else { build_albums(); rebuild_albums_view(); alsel = 0; }
}

/* Tab / Shift-Tab and the number keys walk only the tabs that are showing:
 * the Scope tab exists only while the experimental oscilloscope is on */
static int tab_step(int dir) {
    int t = tab;
    do t = (t + dir + TAB_COUNT) % TAB_COUNT; while (t == TAB_SCOPE && !scope_on);
    return t;
}

static void tab_jump(int n) {            /* n = 0 for the first tab shown */
    for (int t = 0; t < TAB_COUNT; t++) {
        if (t == TAB_SCOPE && !scope_on) continue;
        if (n-- == 0) { switch_tab(t); return; }
    }
}

/* open an album from the album list (ai indexes albums[]) */
static void open_album(int ai) {
    in_album = 1;
    free(cur_album);
    cur_album = xstrdup(albums[ai]);   /* any length: no truncation */
    searching = 0; clear_query(); sel = 0;
    rebuild_view();
}

/* return from an opened album back to the album list */
static void back_to_albums(void) {
    in_album = 0;
    free(cur_album); cur_album = NULL;
    searching = 0; clear_query();
    build_albums(); rebuild_albums_view();
}

/* move the highlight to a given track index, if it's in the current view */
static void select_in_view(int track) {
    for (int i = 0; i < nview; i++)
        if (view[i] == track) { sel = i; return; }
}

static void next_track(void) {
    if (nqueue == 0) return;
    int p = queue_pos(cur);                 /* -1 if current isn't in the queue */
    int pos = p + 1;                        /* p<0 -> start at 0 */
    if (pos >= nqueue) { if (repeat == REP_OFF) { stop_child(); cur = -1; return; } pos = 0; }
    play_track(queue[pos], 0);
    select_in_view(queue[pos]);
}

static void prev_track(void) {
    if (nqueue == 0) return;
    if (elapsed() > 3) { play_track(cur, 0); return; }
    int p = queue_pos(cur);
    int pos = (p > 0) ? p - 1 : (repeat != REP_OFF ? nqueue - 1 : 0);
    play_track(queue[pos], 0);
    select_in_view(queue[pos]);
}

static void seek(double delta) {
    if (cur < 0) return;
    double pos = elapsed() + delta;
    if (pos < 0) pos = 0;
    double dur = tracks[cur].dur;
    if (dur > 0 && pos > dur - 1) pos = dur - 1;
    play_track(cur, pos);
}

static void set_volume(int delta) {
    volume += delta;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    vol_touched = 1;
    if (cur < 0 || !child_h) return;   /* applied when the next track starts */
    /* Turn the live stream's knob in place — no restart, so no dropout. If
     * ffplay hasn't opened its audio session yet the main loop retries. */
    request_vol_sync();
    sync_volume();
}

/* ---- rendering ---- */

/* draw() builds each frame in a fixed buffer; emit() appends to it and clamps
 * the offset, so a frame that would not fit is truncated, never overflowed. */
static int emit(char *buf, size_t cap, int o, const char *fmt, ...) {
    if (o < 0 || (size_t)o + 1 >= cap) return o;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + o, cap - (size_t)o, fmt, ap);
    va_end(ap);
    if (n < 0) return o;                    /* encoding error: drop this chunk */
    o += n;
    return o < (int)cap - 1 ? o : (int)cap - 1;
}

static void fmt_time(double s, char *buf, size_t n) {
    if (s < 0) { snprintf(buf, n, "--:--"); return; }
    int t = (int)(s + 0.5);
    snprintf(buf, n, "%d:%02d", t / 60, t % 60);
}

static int scroll_top = 0;
static int page_size = 10;      /* visible rows; draw() keeps it current */
#define MAX_LIST_ROWS 400       /* cap so one frame always fits in out[] */

/* ---- oscilloscope ----
 * ffplay keeps the audio to itself, so while the Scope tab is open a second,
 * silent ffmpeg decodes the same track to raw PCM on a pipe. It decodes far
 * faster than real time and simply blocks on the full pipe; scope_pump()
 * reads exactly as many samples as the playback clock has advanced, so the
 * picture follows the music through pause, seek and track changes. Leaving
 * the tab stops the decoder: the scope costs nothing when it isn't watched. */
#define SC_RATE 44100                   /* full rate: X-Y curves need every sample */
#define SC_RING 32768                   /* frames kept (~0.74 s), power of two */
#define SC_MAXC 200                     /* draw() never goes wider */
#define SC_XY_MAX 9000                  /* longest X-Y stretch, frames (200 ms) */
static short sc_l[SC_RING], sc_r[SC_RING];
static unsigned sc_w = 0;               /* frames written so far */
static long long sc_read = 0;           /* frames consumed from the decoder */
static double sc_base = 0;              /* track position of the decoder's first frame */
static int sc_gen = -1;                 /* play_gen the decoder was started for */
static int sc_failed = 0;               /* ffmpeg could not be started */
static unsigned char sc_cells[SC_MAXC * MAX_LIST_ROWS];   /* dot bits per character cell */
static unsigned char sc_ccol[SC_MAXC * MAX_LIST_ROWS];    /* which colour the cell is drawn in: 0 = X, 1 = Y */

/* SGR sequence for scope colour 0 (X) or 1 (Y); unset follows the accent.
 * The trailing ";22" keeps apply_theme() from mistaking an explicit 44 or
 * 252 for the default accent/text colour and re-theming it. */
static const char *sc_color_seq(int k) {
    static char seq[2][40];
    k = k ? 1 : 0;
    const Color *c = &col[3 + k];
    if (!c->sgr[0]) return AQUA;
    snprintf(seq[k], sizeof seq[k], "\033[%s;22m", c->sgr);
    return seq[k];
}
static int sc_pw, sc_ph, sc_cw;         /* canvas in dots, cells per row */

static void sc_push(const short *s, int frames) {
    for (int i = 0; i < frames; i++) {
        sc_l[sc_w & (SC_RING - 1)] = s[2 * i];
        sc_r[sc_w & (SC_RING - 1)] = s[2 * i + 1];
        sc_w++;
    }
}

/* ch: 0 = mono mix, 1 = left, 2 = right */
static int sc_at(unsigned i, int ch) {
    i &= SC_RING - 1;
    return ch == 1 ? sc_l[i] : ch == 2 ? sc_r[i] : (sc_l[i] + sc_r[i]) / 2;
}

/* light one dot; returns the index of the cell it lives in, or -1 if off-canvas */
static int sc_plot(int x, int y) {
    if (x < 0 || y < 0 || x >= sc_pw || y >= sc_ph) return -1;
    int cell;
    if (sc_blocks) {
        cell = (y >> 1) * sc_cw + x;
        sc_cells[cell] |= (y & 1) ? 2 : 1;
    } else {
        static const unsigned char bit[4][2] = { {0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80} };
        cell = (y >> 2) * sc_cw + (x >> 1);
        sc_cells[cell] |= bit[y & 3][x & 1];
    }
    return cell;
}

/* one trace across the full width, inside the band [ytop, ytop + yh) */
static void sc_trace(unsigned start, int n, int ch, int ytop, int yh, float gain, int color) {
    float half = (yh - 1) / 2.0f, mid = ytop + half;
    int pt = 0, pb = 0;
    for (int x = 0; x < sc_pw; x++) {
        int a = (int)((long long)x * n / sc_pw), b = (int)((long long)(x + 1) * n / sc_pw);
        if (b <= a) b = a + 1;
        int mn = 32767, mx = -32768;
        for (int i = a; i < b; i++) {
            int v = sc_at(start + (unsigned)i, ch);
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        int t  = (int)(mid - mx / 32768.0f * gain * half + 0.5f);
        int bt = (int)(mid - mn / 32768.0f * gain * half + 0.5f);
        if (t < ytop) t = ytop;
        if (t > ytop + yh - 1) t = ytop + yh - 1;
        if (bt < ytop) bt = ytop;
        if (bt > ytop + yh - 1) bt = ytop + yh - 1;
        if (x) {                         /* join up with the previous column */
            if (t > pb + 1) t = pb + 1;
            if (bt < pt - 1) bt = pt - 1;
        }
        for (int y = t; y <= bt; y++) {
            int cell = sc_plot(x, y);
            if (cell >= 0) sc_ccol[cell] = (unsigned char)color;
        }
        pt = t; pb = bt;
    }
}

/* rasterise the newest samples into sc_cells[] for a cols x h character area */
static void scope_render(int cols, int h) {
    if (cols > SC_MAXC) cols = SC_MAXC;
    if (h > MAX_LIST_ROWS) h = MAX_LIST_ROWS;
    sc_cw = cols;
    sc_pw = sc_blocks ? cols : cols * 2;
    sc_ph = sc_blocks ? h * 2 : h * 4;
    memset(sc_cells, 0, (size_t)cols * h);
    memset(sc_ccol, 0, (size_t)cols * h);

    int n = sc_span * SC_RATE / 1000;
    int have = sc_w > SC_RING ? SC_RING : (int)sc_w;
    if (have < n + 2) return;            /* decoder still catching up */
    unsigned start = sc_w - (unsigned)n;

    if (sc_trig && sc_mode != 2) {       /* newest rising zero crossing */
        int lim = have - n - 2;
        if (lim > 2048) lim = 2048;
        for (int k = 0; k < lim; k++) {
            unsigned i = start - (unsigned)k;
            if (sc_at(i - 1, 0) < 0 && sc_at(i, 0) >= 0) { start = i; break; }
        }
    }

    static float agc = 1.0f;
    static unsigned gain_w = 0;              /* sc_w the followers last saw */
    int moved = (sc_w != gain_w);            /* paused: no new samples, hold every scale */
    gain_w = sc_w;
    float gain = (float)sc_gain;
    if (!sc_gain && moved) {
        /* auto: scale to the loudest sample of everything still in the ring
         * (~0.7 s), not just the slice on screen, and ease upward slowly - the
         * picture keeps its proportions instead of breathing with every beat */
        int peak = 0;
        for (int i = 0; i < have; i++) {
            int a = abs(sc_l[i]), b = abs(sc_r[i]);
            if (a > peak) peak = a;
            if (b > peak) peak = b;
        }
        if (peak > 150) {
            float want = 0.9f * 32768.0f / peak;
            if (want > 16.0f) want = 16.0f;
            if (want < agc) agc = want; else agc += (want - agc) * 0.02f;
        }
    }
    if (!sc_gain) gain = agc;

    if (sc_mode == 0) sc_trace(start, n, 0, 0, sc_ph, gain, 0);
    else if (sc_mode == 1) {
        int h2 = sc_ph / 2;
        sc_trace(start, n, 1, 0, h2, gain, 0);
        sc_trace(start, n, 2, h2, sc_ph - h2, gain, 1);
    } else {
        /* X-Y. Stateless: every frame redraws the last stretch of signal as
         * one smooth curve - a Catmull-Rom spline through the samples, stepped
         * a dot at a time - with the newest part in the X colour and the older
         * tail in the Y colour. The stretch is never shorter than two frames,
         * so successive pictures overlap and the figure glides instead of
         * jumping; nothing is accumulated, and a paused track holds still.
         *
         * Raw music scribbles so densely that on/off dots turn it into a
         * filled blob, so (unless smoothing is off) both channels first go
         * through a two-pole low-pass: what is left is the slow, large motion
         * - open loops that turn with the bass. Each axis follows its own
         * slowly-falling peak, so the figure doesn't pump with the beat. */
        static float fx[SC_XY_MAX + 4], fy[SC_XY_MAX + 4];
        static float envx = 0, envy = 0;         /* peak followers of what is drawn */
        static const float coef[4] = { 1.0f, 0.2480f, 0.0949f, 0.0418f };  /* 1 - exp(-2*pi*fc/SC_RATE) */
        int m = n, two = 2 * SC_RATE / sc_fps;
        if (m < two) m = two;
        if (m > SC_XY_MAX) m = SC_XY_MAX;
        if (m > have - 3) m = have - 3;
        int warm = have - m - 3;                 /* let the filter settle before the part we draw */
        if (warm > 4096) warm = 4096;
        unsigned s0 = sc_w - (unsigned)m - 1;    /* fx[0] is the sample before the stretch */
        float a = coef[sc_smooth & 3], l1 = 0, l2 = 0, r1 = 0, r2 = 0, pkx = 0, pky = 0;
        for (int i = -warm; i <= m; i++) {
            unsigned j = (s0 + (unsigned)i) & (SC_RING - 1);
            if (i == -warm) { l1 = l2 = sc_l[j]; r1 = r2 = sc_r[j]; }
            l1 += (sc_l[j] - l1) * a; l2 += (l1 - l2) * a;
            r1 += (sc_r[j] - r1) * a; r2 += (r1 - r2) * a;
            if (i >= 0) {
                float vx = sc_xyview ? (l2 - r2) * 0.5f : l2;    /* wide: side across */
                float vy = sc_xyview ? (l2 + r2) * 0.5f : r2;    /*       mid up      */
                fx[i] = vx; fy[i] = vy;
                if (vx < 0) vx = -vx;
                if (vy < 0) vy = -vy;
                if (vx > pkx) pkx = vx;
                if (vy > pky) pky = vy;
            }
        }
        float gx = gain, gy = gain;
        if (!sc_gain) {
            if (moved) {
                if (!sc_xyview) { if (pky > pkx) pkx = pky; else pky = pkx; }   /* L/R: one scale, true shape */
                if (pkx > envx) envx = pkx; else envx -= (envx - pkx) * 0.01f;
                if (pky > envy) envy = pky; else envy -= (envy - pky) * 0.01f;
            }
            /* wide may overshoot the edge on the loudest peaks: a bigger
             * figure the rest of the time is worth a clipped tip now and then */
            float fill = sc_xyview ? 1.25f : 0.92f;
            gy = envy > 100.0f ? fill * 32768.0f / envy : 1.0f;
            gx = envx > 100.0f ? fill * 32768.0f / envx : gy;
            if (gy > 40.0f) gy = 40.0f;
            if (gx > gy * 12.0f) gx = gy * 12.0f;    /* a mono track stays a line, not amplified noise */
        }
        /* L/R keeps a square plot (true shape); wide has independent axes
         * anyway, so it takes the whole width */
        int side = sc_pw < sc_ph ? sc_pw : sc_ph;
        float rx = ((sc_xyview ? sc_pw : side) - 1) / 2.0f / 32768.0f * gx;
        float ry = ((sc_xyview ? sc_ph : side) - 1) / 2.0f / 32768.0f * gy;
        float cx = (sc_pw - 1) / 2.0f, cy = (sc_ph - 1) / 2.0f;
        int head = m - m * 2 / 5;                /* the newest 40% is the beam */
        memset(sc_ccol, 1, (size_t)cols * h);
        for (int i = 0; i + 3 <= m; i++) {
            float x0 = fx[i], x1 = fx[i + 1], x2 = fx[i + 2], x3 = fx[i + 3];
            float y0 = fy[i], y1 = fy[i + 1], y2 = fy[i + 2], y3 = fy[i + 3];
            float dx = (x2 - x1) * rx, dy = (y2 - y1) * ry;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            int k = (int)(dx > dy ? dx : dy) + 1;    /* one step per dot travelled */
            if (k > 24) k = 24;                      /* a fast jump is only dotted in */
            for (int q = 0; q < k; q++) {
                float t = (float)q / k, t2 = t * t, t3 = t2 * t;
                float px = 0.5f * (2 * x1 + (x2 - x0) * t + (2 * x0 - 5 * x1 + 4 * x2 - x3) * t2
                                   + (3 * x1 - x0 - 3 * x2 + x3) * t3);
                float py = 0.5f * (2 * y1 + (y2 - y0) * t + (2 * y0 - 5 * y1 + 4 * y2 - y3) * t2
                                   + (3 * y1 - y0 - 3 * y2 + y3) * t3);
                int cell = sc_plot((int)(cx + px * rx + 0.5f), (int)(cy - py * ry + 0.5f));
                if (cell >= 0 && i >= head) sc_ccol[cell] = 0;
            }
        }
    }
}

/* the decoder process, Windows edition: ffmpeg writing PCM into an anonymous pipe */
static HANDLE sc_proc = NULL, sc_pipe = NULL;

static void scope_stop(void) {
    if (sc_proc) {
        TerminateProcess(sc_proc, 0);
        WaitForSingleObject(sc_proc, 1000);
        CloseHandle(sc_proc);
        sc_proc = NULL;
    }
    if (sc_pipe) { CloseHandle(sc_pipe); sc_pipe = NULL; }
    sc_gen = -1;
}

static void scope_start(void) {
    scope_stop();
    sc_gen = play_gen;
    sc_read = 0; sc_w = 0;
    memset(sc_l, 0, sizeof sc_l); memset(sc_r, 0, sizeof sc_r);
    double pos = elapsed() - sc_delay / 1000.0;
    if (pos < 0) pos = 0;
    sc_base = pos;

    SECURITY_ATTRIBUTES sa; sa.nLength = sizeof sa; sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) { sc_failed = 1; return; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);      /* our end stays ours */
    HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    if (nul == INVALID_HANDLE_VALUE) nul = NULL;

    char cmd[PATH_MAX + 256];
    snprintf(cmd, sizeof cmd,
        "ffmpeg -v quiet -nostdin -ss %.3f -i \"%s\" -vn -f s16le -ac 2 -ar %d -",
        pos, tracks[cur].path, SC_RATE);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul; si.hStdOutput = wr; si.hStdError = nul;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
    if (CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        sc_proc = pi.hProcess;
        sc_pipe = rd;
        sc_failed = 0;
    } else {
        CloseHandle(rd);
        sc_failed = 1;
    }
    CloseHandle(wr);
    if (nul) CloseHandle(nul);
}

/* called every main-loop tick while the Scope tab is showing */
static void scope_pump(void) {
    if (cur < 0 || !child_h) { if (sc_proc) scope_stop(); return; }
    if (sc_gen != play_gen) scope_start();
    if (!sc_pipe) return;
    long long target = (long long)((elapsed() - sc_delay / 1000.0 - sc_base) * SC_RATE);
    static unsigned char buf[16384];
    for (int budget = 64; sc_read < target && budget > 0; budget--) {
        DWORD avail = 0, got = 0;
        if (!PeekNamedPipe(sc_pipe, NULL, 0, NULL, &avail, NULL) || avail < 4) break;
        long long want = (target - sc_read) * 4;
        if (want > (long long)sizeof buf) want = sizeof buf;
        if (want > (long long)avail) want = avail & ~(DWORD)3;
        if (!ReadFile(sc_pipe, buf, (DWORD)want, &got, NULL) || got < 4) break;
        sc_push((const short *)(void *)buf, (int)(got / 4));
        sc_read += got / 4;
    }
}

/* ---- flicker-free rendering ----
 * The last painted frame is remembered as one 64-bit hash per screen line
 * (a few KB, not a text copy of the screen): draw() emits only the lines
 * whose hash changed, each positioned absolutely, inside synchronized-
 * update brackets, so the terminal never shows a partial frame. An
 * unchanged screen emits nothing at all — the display sits still instead
 * of repainting (and flashing) at 10 Hz. Resizes repaint in place;
 * \033[2J (blank, then redraw = a hard flash) happens exactly once, at
 * startup. Output accumulates in a small chunk buffer that is flushed as
 * it fills, so even a full repaint never needs a whole-frame buffer. */
#define MAX_LINE 4096                       /* one rendered screen line (scope rows switch colour per cell) */
#define MAX_FRAME_LINES (MAX_LIST_ROWS + 8)
static unsigned long long shadow_hash[MAX_FRAME_LINES];
static int  shadow_count = 0;               /* rows valid in the shadow */

/* the current delta: bounded chunk buffer + whether anything was written */
typedef struct { char buf[16384]; int n; int wrote; } Delta;

static void delta_flush(Delta *del) {
    if (del->n) { fwrite(del->buf, 1, (size_t)del->n, stdout); fflush(stdout); del->n = 0; }
}

/* swap the default colours for the configured ones (see settings above);
 * every screen line passes through here on its way out. A custom text or
 * background colour rides on every reset, so plain text and erased cells
 * (\033[K) pick it up too. */
#define THEME_SLACK 1024                    /* room for longer colour codes */
static const char *apply_theme(const char *line, char *out, size_t cap) {
    const char *acc = col[SET_ACCENT].sgr, *bg = col[SET_BG].sgr, *tx = col[SET_TEXT].sgr;
    int ca = strcmp(acc, "38;5;44") != 0, ct = strcmp(tx, "38;5;252") != 0, cb = (*bg != '\0');
    if (!ca && !ct && !cb) return line;
    char rst[64] = "";                       /* what a reset becomes */
    if (ct || cb)
        snprintf(rst, sizeof rst, "\033[0;%s%s%sm", ct ? tx : "", (ct && cb) ? ";" : "", cb ? bg : "");
    size_t rl = strlen(rst), al = strlen(acc), tl = strlen(tx), o = 0;
    if (rl) { memcpy(out, rst, rl); o = rl; }
    while (*line && o + 64 < cap) {
        if (ca && line[0] == '3' && strncmp(line, "38;5;44m", 8) == 0) {
            memcpy(out + o, acc, al); o += al; out[o++] = 'm'; line += 8;
        } else if (ct && line[0] == '3' && strncmp(line, "38;5;252m", 9) == 0) {
            memcpy(out + o, tx, tl); o += tl; out[o++] = 'm'; line += 9;
        } else if (rl && line[0] == '\033' && strncmp(line, "\033[0m", 4) == 0) {
            memcpy(out + o, rst, rl); o += rl; line += 4;
        } else out[o++] = *line++;
    }
    out[o] = '\0';
    return out;
}

/* overwrite one screen line unless its hash says it's already shown */
static void flush_line(Delta *del, int row, const char *line) {
    if (row >= MAX_FRAME_LINES) return;
    char themed[MAX_LINE + THEME_SLACK];
    line = apply_theme(line, themed, sizeof themed);
    unsigned long long h = str_hash(line);
    if (row < shadow_count && shadow_hash[row] == h) return;
    if (!del->wrote) {                      /* first change opens the bracket */
        del->n = emit(del->buf, sizeof del->buf, del->n, "\033[?2026h");
        del->wrote = 1;
    }
    del->n = emit(del->buf, sizeof del->buf, del->n, "\033[%d;1H%s\033[K", row + 1, line);
    if (del->n > (int)sizeof del->buf - MAX_LINE - THEME_SLACK - 32) delta_flush(del);
    shadow_hash[row] = h;
}

static void draw(void) {
    int rows, cols;
    get_size(&rows, &cols);
    if (cols > 200) cols = 200;

    int header = 2;        /* tab bar + breadcrumb/search */
    int footer = 5;        /* divider, nowtitle, bar, help, pad */
    int list_h = rows - header - footer;
    if (list_h < 1) list_h = 1;
    if (list_h > MAX_LIST_ROWS) list_h = MAX_LIST_ROWS;
    page_size = list_h;

    int album_list = on_album_list();
    int settings = (tab == TAB_SETTINGS);
    int scope = (tab == TAB_SCOPE);
    int asel   = settings ? setsel    : scope ? 0 : album_list ? alsel   : sel;   /* active selection */
    int acount = settings ? nset      : album_list ? nalview : nview;   /* active list length */

    /* keep the active selection on screen */
    if (asel < scroll_top) scroll_top = asel;
    if (asel >= scroll_top + list_h) scroll_top = asel - list_h + 1;
    if (scroll_top < 0) scroll_top = 0;

    /* a resize changes geometry: repaint everything in place, no blanking */
    static int last_rows = -1, last_cols = -1;
    int full = (rows != last_rows || cols != last_cols || force_full);
    force_full = 0;
    if (full) { shadow_count = 0; last_rows = rows; last_cols = cols; }

    Delta del = {{0}, 0, 0};                 /* bounded output chunk */
    char line[MAX_LINE];
    int lo = 0, row = 0;

    /* ---- tab bar ---- */
    const char *t_tracks  = (tab == TAB_TRACKS)  ? INVAQ " Tracks "  RESET : DGRAY " Tracks "  RESET;
    const char *t_albums  = (tab == TAB_ALBUMS)  ? INVAQ " Albums "  RESET : DGRAY " Albums "  RESET;
    const char *t_folders = (tab == TAB_FOLDERS) ? INVAQ " Folders " RESET : DGRAY " Folders " RESET;
    const char *t_artists = (tab == TAB_ARTISTS) ? INVAQ " Artists " RESET : DGRAY " Artists " RESET;
    const char *t_scope   = !scope_on ? ""                              /* experimental, off: no tab */
                          : scope    ? INVAQ " Scope "   RESET : DGRAY " Scope "   RESET;
    const char *t_settings = settings            ? INVAQ " Settings " RESET : DGRAY " Settings " RESET;
    lo = emit(line, sizeof line, 0,
        AQUA_B "  \xE2\x99\xAC AQUA  " RESET "%s%s%s%s%s%s",
        t_tracks, t_albums, t_folders, t_artists, t_scope, t_settings);
    flush_line(&del, row++, line);

    /* ---- breadcrumb / search line ---- */
    if (settings && editing == 2) {              /* a long path: keep the end you are typing in view */
        int room = cols - 20;
        if (room < 10) room = 10;
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "Music folder: " WHITE "%s" AQUA "\xE2\x96\x88" RESET,
            plen > room ? pbuf + plen - room : pbuf);
    }
    else if (settings && editing)
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "%s code: " WHITE "%s" AQUA "\xE2\x96\x88" RESET
            DGRAY "   #rrggbb, 0-255 or default" RESET, set_names[set_row()], ebuf);
    else if (settings)
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "Settings%s" AQUA "%s" RESET, *set_msg ? "   " : "", set_msg);
    else if (scope) {
        char gb[16];
        if (sc_gain) snprintf(gb, sizeof gb, "%dx", sc_gain); else snprintf(gb, sizeof gb, "auto");
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "Oscilloscope   " AQUA "%s" DGRAY "   %d ms   gain %s%s" RESET,
            sc_mode_names[sc_mode], sc_span, gb,
            sc_mode == 2 ? (sc_xyview ? "   wide" : "   L/R") : sc_trig ? "   triggered" : "");
    }
    else if (searching)
        lo = emit(line, sizeof line, 0,
            "  " AQUA "/" WHITE "%s" AQUA "\xE2\x96\x88" RESET
            DGRAY "   %d matches" RESET, query, acount);
    else if (in_album)
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "%s \xE2\x96\xB8 " AQUA "%s" DGRAY
            "   %d tracks%s%s  (esc: back)" RESET,
            browse_label(), cur_album ? cur_album : "", nview,
            query[0] ? "  filter: " : "", query[0] ? query : "");
    else if (query[0])
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "filter: " AQUA "%s" DGRAY "  (esc to clear)" RESET, query);
    else if (album_list)
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "%d %s" RESET,
            nalbums, tab == TAB_ARTISTS ? "artists" : tab == TAB_FOLDERS ? "folders" : "albums");
    else
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "%d tracks" RESET, ntracks);
    flush_line(&del, row++, line);

    /* ---- list body ---- */
    int scope_live = scope && cur >= 0 && !sc_failed;
    if (scope_live) scope_render(cols, list_h);
    for (int i = 0; i < list_h; i++) {
        int r = scroll_top + i;
        if (scope) {                             /* ---- oscilloscope row ---- */
            lo = 0;
            line[0] = '\0';
            if (!scope_live) {
                if (i == list_h / 2)
                    lo = emit(line, sizeof line, 0, "  " DGRAY "%s" RESET,
                        sc_failed ? "The oscilloscope needs ffmpeg on PATH (it decodes the playing track)."
                                  : "Nothing playing - press space to start a track.");
            } else {
                const unsigned char *cr = sc_cells + (size_t)i * cols;
                int last = cols - 1;
                while (last >= 0 && !cr[last]) last--;
                if (last >= 0) {
                    const unsigned char *cc = sc_ccol + (size_t)i * cols;
                    int curc = -1;               /* colour currently switched on */
                    for (int x = 0; x <= last && lo < (int)sizeof line - 64; x++) {
                        unsigned char c = cr[x];
                        if (!c) { line[lo++] = ' '; continue; }
                        if (cc[x] != curc) {
                            curc = cc[x];
                            line[lo] = '\0';
                            lo = emit(line, sizeof line, lo, "%s", sc_color_seq(curc));
                        }
                        if (sc_blocks) {         /* upper half, lower half, full block */
                            line[lo++] = '\xE2'; line[lo++] = '\x96';
                            line[lo++] = (char)(c == 1 ? 0x80 : c == 2 ? 0x84 : 0x88);
                        } else {                 /* U+2800 + dot bits */
                            line[lo++] = '\xE2';
                            line[lo++] = (char)(0xA0 | (c >> 6));
                            line[lo++] = (char)(0x80 | (c & 0x3F));
                        }
                    }
                    line[lo] = '\0';
                    lo = emit(line, sizeof line, lo, RESET);
                }
            }
            flush_line(&del, row++, line);
            continue;
        }
        if (settings) {                          /* ---- settings row ---- */
            if (r < nset) {
                int id = set_vis[r];             /* which setting this line shows */
                char vbuf[16] = "";
                const char *val = vbuf;
                if (row_color(id))            val = row_color(id)->code;
                else if (id == SET_EXP_SCOPE) val = scope_on ? "on" : "off";
                else if (id == SET_SEEK)      snprintf(vbuf, sizeof vbuf, "%ds", seek_step);
                else if (id == SET_VOLSTEP)   snprintf(vbuf, sizeof vbuf, "%d%%", vol_step);
                else if (id == SET_START)     val = tab_name(start_tab);
                else if (id == SET_SC_MODE)   val = sc_mode_names[sc_mode];
                else if (id == SET_SC_SPAN)   snprintf(vbuf, sizeof vbuf, "%dms", sc_span);
                else if (id == SET_SC_GAIN)   { if (sc_gain) snprintf(vbuf, sizeof vbuf, "%dx", sc_gain); else val = "auto"; }
                else if (id == SET_SC_TRIG)   val = sc_trig ? "on" : "off";
                else if (id == SET_SC_STYLE)  val = sc_blocks ? "blocks" : "braille";
                else if (id == SET_SC_FPS)    snprintf(vbuf, sizeof vbuf, "%dfps", sc_fps);
                else if (id == SET_SC_DELAY)  snprintf(vbuf, sizeof vbuf, "%dms", sc_delay);
                else if (id == SET_SC_SMOOTH) val = sc_smooth_names[sc_smooth];
                else if (id == SET_SC_XYVIEW) val = sc_xyview_names[sc_xyview];
                if (id == SET_MUSIC) {               /* a path gets the rest of the line */
                    int room = cols - 19, pl = (int)strlen(lib_dir);
                    if (room < 8) room = 8;
                    const char *shown = pl > room ? lib_dir + pl - room : lib_dir;
                    if (r == setsel)
                        lo = emit(line, sizeof line, 0,
                            INVAQ " %-13s %*s " RESET, set_names[id], room, shown);
                    else
                        lo = emit(line, sizeof line, 0,
                            " " WHITE "%-13s" RESET " " GRAY "%*s" RESET, set_names[id], room, shown);
                    flush_line(&del, row++, line);
                    continue;
                }
                int nw = cols - 13;
                if (nw < 8) nw = 8;
                if (r == setsel)
                    lo = emit(line, sizeof line, 0,
                        INVAQ " %-*.*s %7s " RESET, nw, nw, set_names[id], val);
                else
                    lo = emit(line, sizeof line, 0,
                        " " WHITE "%-*.*s" RESET " %s%7s" RESET,
                        nw, nw, set_names[id], (id == SET_ACCENT) ? AQUA : GRAY, val);
            } else if (r == nset + 1)
                lo = emit(line, sizeof line, 0,
                    "  " DGRAY "Color codes: hex #rrggbb, 0-255 (256-color palette), or \"default\"" RESET);
            else if (r == nset + 2)
                lo = emit(line, sizeof line, 0,
                    "  " DGRAY "Shuffle and repeat are remembered between runs" RESET);
            else if (r == nset + 3 && !scope_on)
                lo = emit(line, sizeof line, 0,
                    "  " DGRAY "Experimental features are off by default and may still change; turning one on adds its tab and options" RESET);
            else if (r == nset + 3)
                lo = emit(line, sizeof line, 0,
                    "  " DGRAY "Scope: raise the sync delay if the picture runs ahead of the sound; style \"blocks\" if dots show as boxes" RESET);
            else if (r == nset + 4 && scope_on)
                lo = emit(line, sizeof line, 0,
                    "  " DGRAY "Scope X color: the wave, the left channel, the X-Y beam.  Y color: the right channel, the X-Y tail.  default = accent" RESET);
            else if (r == nset + 5 && *startup_note)
                lo = emit(line, sizeof line, 0, "  " GRAY "%s" RESET, startup_note);
            else {
                lo = 0;
                line[0] = '\0';
            }
            flush_line(&del, row++, line);
            continue;
        }
        if (r >= acount) {
            if (acount == 0 && i == 0)
                lo = emit(line, sizeof line, 0,
                    "  " DGRAY "%s" RESET,
                    query[0] ? "No matches." :
                    (album_list ? (tab == TAB_ARTISTS ? "No artists." :
                                   tab == TAB_FOLDERS ? "No folders." : "No albums.")
                                : "No tracks."));
            else {
                lo = 0;
                line[0] = '\0';
            }
            flush_line(&del, row++, line);
            continue;
        }

        if (album_list) {                        /* ---- album row ---- */
            int ai = alview[r];
            const char *an = albums[ai];
            char cbuf[24];
            snprintf(cbuf, sizeof cbuf, "%d \xE2\x99\xAA", album_n[ai]);
            int nw = cols - 12;
            if (nw < 8) nw = 8;
            if (r == alsel)
                lo = emit(line, sizeof line, 0,
                    INVAQ " %-*.*s %6s " RESET,
                    nw, nw, *an ? an : "(unknown)", cbuf);
            else
                lo = emit(line, sizeof line, 0,
                    " " WHITE "%-*.*s" RESET " " DGRAY "%6s" RESET,
                    nw, nw, *an ? an : "(unknown)", cbuf);
            flush_line(&del, row++, line);
            continue;
        }

        /* ---- track row ---- */
        int idx = view[r];
        Track *t = &tracks[idx];
        char dbuf[16];
        fmt_time(t->dur, dbuf, sizeof dbuf);

        char num[16];
        if (t->trackno > 0) snprintf(num, sizeof num, "%2d", t->trackno);
        else                snprintf(num, sizeof num, "  ");

        const char *marker = (idx == cur) ? (paused ? "\xE2\x9D\x9A " : "\xE2\x96\xB6 ") : "  ";
        int name_w = cols - 16;
        if (name_w < 8) name_w = 8;
        char nbuf[1024];
        const char *title = disp_title(t, nbuf, sizeof nbuf);

        if (r == sel)
            lo = emit(line, sizeof line, 0,
                INVAQ " %s%s %-*.*s %5s " RESET,
                marker, num, name_w, name_w, title, dbuf);
        else {
            const char *col = (idx == cur) ? AQUA : WHITE;
            lo = emit(line, sizeof line, 0,
                " %s%s" DGRAY "%s " RESET "%s%-*.*s" RESET " " DGRAY "%5s" RESET,
                col, marker, num, col, name_w, name_w, title, dbuf);
        }
        flush_line(&del, row++, line);
    }

    /* footer */
    lo = emit(line, sizeof line, 0, DGRAY);
    for (int i = 0; i < cols && i < 200 && lo < (int)sizeof line - 2; i++) line[lo++] = '-';
    emit(line, sizeof line, lo, RESET);
    flush_line(&del, row++, line);

    if (cur >= 0) {
        double el = elapsed(), dur = tracks[cur].dur;
        char e[16], d[16];
        fmt_time(el, e, sizeof e);
        fmt_time(dur, d, sizeof d);

        const char *state = paused ? GRAY "PAUSED" : AQUA "PLAYING";
        /* "Title — Artist" when an artist tag exists */
        char now[1024], tbuf[1024];
        const char *dtitle = disp_title(&tracks[cur], tbuf, sizeof tbuf);
        if (tracks[cur].artist && *tracks[cur].artist)
            snprintf(now, sizeof now, "%s \xE2\x80\x94 %s",
                     dtitle, tracks[cur].artist);
        else
            snprintf(now, sizeof now, "%s", dtitle);
        lo = emit(line, sizeof line, 0,
            "  %s" RESET " " WHITE "%-*.*s" RESET,
            state, cols - 14, cols - 14, now);
        flush_line(&del, row++, line);

        /* progress bar */
        int bar_w = cols - 20;
        if (bar_w < 4) bar_w = 4;
        int fill = (dur > 0) ? (int)(bar_w * (el / dur)) : 0;
        if (fill > bar_w) fill = bar_w;
        if (fill < 0) fill = 0;
        lo = emit(line, sizeof line, 0, "  " GRAY "%5s " AQUA, e);
        for (int i = 0; i < fill; i++)      lo = emit(line, sizeof line, lo, "\xE2\x96\x88");
        lo = emit(line, sizeof line, lo, DGRAY);
        for (int i = fill; i < bar_w; i++)  lo = emit(line, sizeof line, lo, "\xE2\x94\x80");
        emit(line, sizeof line, lo, " " GRAY "%5s" RESET, d);
        flush_line(&del, row++, line);
    } else {
        emit(line, sizeof line, 0, "  " GRAY "Nothing playing" RESET);
        flush_line(&del, row++, line);
        line[0] = '\0';
        flush_line(&del, row++, line);
    }

    /* help line */
    if (settings && editing == 2)
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "type or paste a folder path   " DGRAY "\xE2\x86\xB5" GRAY " use it (aqua restarts on it)  "
            DGRAY "esc" GRAY " cancel  " DGRAY "empty" GRAY " = the folder aqua was started on" RESET);
    else if (settings && editing)
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "type a color code   " DGRAY "\xE2\x86\xB5" GRAY " apply  "
            DGRAY "esc" GRAY " cancel" RESET);
    else if (settings)
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "\xE2\x86\x91\xE2\x86\x93" GRAY " move  " DGRAY "\xE2\x86\xB5" GRAY " change  "
            DGRAY "\xE2\x86\x90\xE2\x86\x92" GRAY " adjust  "
            DGRAY "\xE2\x87\xA5" GRAY " tabs  " DGRAY "q" GRAY " quit" RESET);
    else if (scope)
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "m" GRAY " mode  " DGRAY "\xE2\x86\x91\xE2\x86\x93" GRAY " time span  "
            DGRAY "space" GRAY " play  " DGRAY "n/b" GRAY " next/prev  "
            DGRAY "\xE2\x86\x90\xE2\x86\x92" GRAY " seek  " DGRAY "-/+" GRAY " vol %d%%  "
            DGRAY "\xE2\x87\xA5" GRAY " tabs  " DGRAY "q" GRAY " quit" RESET, volume);
    else if (searching)
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "type to filter   " DGRAY "\xE2\x86\xB5" GRAY " keep  "
            DGRAY "esc" GRAY " clear  " DGRAY "\xE2\x86\x91\xE2\x86\x93" GRAY " move" RESET);
    else if (on_album_list())
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "\xE2\x86\x91\xE2\x86\x93" GRAY " move  " DGRAY "\xE2\x86\xB5" GRAY " open  "
            DGRAY "\xE2\x87\xA5" GRAY " tabs  " DGRAY "/" GRAY " search  " DGRAY "q" GRAY " quit" RESET);
    else
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "\xE2\x86\x91\xE2\x86\x93" GRAY " move  " DGRAY "\xE2\x86\xB5/space" GRAY " play  "
            DGRAY "n/b" GRAY " next/prev  " DGRAY "\xE2\x86\x90\xE2\x86\x92" GRAY " seek  "
            DGRAY "-/+" GRAY " vol %d%%  " DGRAY "\xE2\x87\xA5" GRAY " tabs  " DGRAY "/" GRAY " search  "
            DGRAY "s" GRAY " shuf%s  " DGRAY "r" GRAY " rep%s  " "%s" DGRAY "q" GRAY " quit" RESET,
            volume,
            shuffle ? AQUA " on" GRAY : "",
            repeat == REP_ALL ? AQUA " all" GRAY : repeat == REP_ONE ? AQUA " one" GRAY : "",
            in_album ? DGRAY "esc" GRAY " back  " : "");
    flush_line(&del, row++, line);

    /* wipe leftovers below a shortened frame, then publish the delta */
    if (full && row < rows) {
        if (!del.wrote) { del.n = emit(del.buf, sizeof del.buf, del.n, "\033[?2026h"); del.wrote = 1; }
        del.n = emit(del.buf, sizeof del.buf, del.n, "\033[%d;1H\033[J", row + 1);
    }
    shadow_count = row;

    if (del.wrote) {                        /* nothing changed: emit nothing */
        delta_flush(&del);
        fwrite("\033[?2026l", 1, 8, stdout);
        fflush(stdout);
    }
}

/* ---- input ---- */
enum {
    KEY_UP = 1000, KEY_DOWN, KEY_RIGHT, KEY_LEFT,   /* arrows */
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_BTAB                                        /* Shift-Tab */
};

/* Returns an ASCII byte, a KEY_* code, or -1 (no key / ignored key).
 * Windows console "extended" keys (arrows, Home/End/PgUp/PgDn, Delete...)
 * arrive as 0xE0- or 0-prefixed scan codes; the useful ones are mapped and
 * the rest are dropped (-1) so they can't leak garbage into the query. */
static int read_key(void) {
    int c = _getch();
    if (c == 0 || c == 0xE0) {          /* extended key: a second byte follows */
        int c2 = _getch();
        switch (c2) {
            case 72: return KEY_UP;     /* up */
            case 80: return KEY_DOWN;   /* down */
            case 77: return KEY_RIGHT;  /* right */
            case 75: return KEY_LEFT;   /* left */
            case 71: return KEY_HOME;   /* Home */
            case 79: return KEY_END;    /* End */
            case 73: return KEY_PGUP;   /* PgUp */
            case 81: return KEY_PGDN;   /* PgDn */
            case 15: return KEY_BTAB;   /* Shift-Tab */
        }
        return -1;
    }
    return c;
}

/* Ctrl-Break / console close: leave via the normal exit path so the cache
 * is written and the console is restored (Ctrl-C itself arrives as byte 3
 * below, because ENABLE_PROCESSED_INPUT is off). */
static volatile int g_stop = 0;
static BOOL WINAPI on_console_ctrl(DWORD ev) {
    (void)ev;
    g_stop = 1;
    return TRUE;                        /* we'll exit cleanly on our own */
}

int main(int argc, char **argv) {
    init_cache_path();
    load_settings();                     /* first: it may name the music folder */
    settings_rows();

    /* The folder to play: the one aqua was started on, unless Settings >
     * Music folder has replaced it (see "music folder" above). If the saved
     * folder has gone missing or empty, fall back rather than refuse to start. */
    if (argc > 1) snprintf(launch_arg, sizeof launch_arg, "%s", argv[1]);
    const char *dir = launch_arg[0] ? launch_arg : ".";
    if (music_dir[0] && (!launch_arg[0] || strcmp(launch_arg, music_from) == 0)) {
        scan_dir(music_dir, 6);
        if (ntracks > 0) dir = music_dir;
        else startup_note = "Note: the saved music folder is missing or empty, so aqua is playing the folder it was started on";
    }
    if (ntracks == 0) scan_dir(dir, 6);
    if (ntracks == 0) {
        fprintf(stderr, "No audio files found in '%s'.\n", dir);
        return 1;
    }
    if (!_fullpath(lib_dir, dir, PATH_MAX)) snprintf(lib_dir, sizeof lib_dir, "%s", dir);
    qsort(tracks, ntracks, sizeof(Track), cmp_track);

    load_cache();

    /* canonical paths + mtimes, then prefill durations from cache.
     * The canonical path lives in a stack buffer: it's needed only here (as
     * the cache key) and again at save time, so a 5,000-track library
     * doesn't carry 5,000 ~80-byte path strings all session. */
    for (int i = 0; i < ntracks; i++) {
        char rp[PATH_MAX];
        const char *ap = _fullpath(rp, tracks[i].path, PATH_MAX) ? rp : tracks[i].path;
        struct stat st;
        if (stat(tracks[i].path, &st) == 0) tracks[i].mtime = (long)st.st_mtime;
        CacheEnt *e = cache_lookup(ap, tracks[i].mtime);
        if (e) {
            tracks[i].dur = e->dur;          /* may be -1: probed, no duration */
            tracks[i].trackno = e->trackno;
            tracks[i].title  = e->title  ? strpool(e->title)  : NULL;
            tracks[i].artist = e->artist ? intern(e->artist) : NULL;
            tracks[i].album  = e->album  ? intern(e->album)  : NULL;
            tracks[i].probed = 1;            /* cached: don't probe again */
            apply_group(&tracks[i]);
            /* consumed: the tags now live in tracks[] (shared/interned); the
             * entry is marked dead and goes when the buffer is released.
             * save_cache() re-creates it at exit, so nothing is lost. */
            e->dead = 1;
        }
    }
    /* drop consumed entries: their data lives in tracks[] only. Survivors
     * are files probed under a different folder or with a newer mtime —
     * their strings point into cache_buf and must be copied out before
     * the buffer is freed. */
    {
        int w = 0;
        for (int i = 0; i < ncache; i++) {
            if (cache[i].dead) continue;
            CacheEnt *e = &cache[w];
            if (w != i) *e = cache[i];
            e->path = xstrdup(e->path);
            if (e->title)  e->title  = xstrdup(e->title);
            if (e->artist) e->artist = xstrdup(e->artist);
            if (e->album)  e->album  = xstrdup(e->album);
            w++;
        }
        ncache = w;
    }
    VirtualFree(cache_buf, 0, MEM_RELEASE);   /* pages straight back to the OS */
    cache_buf = NULL;
    /* the cache array (VirtualAlloc'd): fully consumed -> release it; a few
     * survivors -> move them to a compact heap array and release the big
     * one. save_cache() grows the heap array again as needed. */
    if (ncache == 0) { VirtualFree(cache, 0, MEM_RELEASE); cache = NULL; capcache = 0; }
    else {
        CacheEnt *heapsrv = xmalloc((size_t)ncache * sizeof(CacheEnt));
        memcpy(heapsrv, cache, (size_t)ncache * sizeof(CacheEnt));
        if (cache) VirtualFree(cache, 0, MEM_RELEASE);
        cache = heapsrv; capcache = ncache;
    }

    srand((unsigned)time(NULL));
    queue = xmalloc(ntracks * sizeof(int));
    rebuild_view();   /* initial view = all tracks; also builds the queue */
    build_albums();   /* album list for the Albums tab */
    if (start_tab != TAB_TRACKS) switch_tab(start_tab);   /* Settings: Start on tab */

    SetConsoleCtrlHandler(on_console_ctrl, TRUE);

    term_raw();
    printf("\033[2J");
    fflush(stdout);

    draw();

    /* Durations are probed lazily in the loop below (one ffprobe per file),
     * so the UI is responsive immediately even with thousands of tracks. */
    int probe_idx = 0;
    int probed_new = 0;
    double last_draw = 0;

    int running = 1;
    while (running && !g_stop) {
        /* reap a naturally-finished track -> advance */
        if (child_h) {
            if (WaitForSingleObject(child_h, 0) == WAIT_OBJECT_0) {
                CloseHandle(child_h);
                child_h = NULL;
                child_pid = 0;
                if (!intentional) {
                    if (repeat == REP_ONE && cur >= 0) play_track(cur, 0);
                    else next_track();
                }
            }
        }

        sync_volume();   /* no-op unless a volume change is still waiting on the stream */
        int scoping = (tab == TAB_SCOPE);
        if (scoping) scope_pump();

        int probing = probe_idx < ntracks;

        if (_kbhit()) {
            int k = read_key();
            int alist = on_album_list();
            if (tab == TAB_SETTINGS && editing == 2) {
                /* ---- typing a music folder ---- */
                switch (k) {
                    case '\r': case '\n':
                        editing = 0;
                        music_apply();
                        if (relaunch) running = 0;       /* saved: start over on it */
                        break;
                    case 27: editing = 0; break;                     /* esc: cancel */
                    case 127: case 8:                                /* backspace */
                        if (plen > 0) pbuf[--plen] = '\0';
                        break;
                    case 3: running = 0; break;
                    default:
                        if (k >= 32 && k < 256 && k != 127 && plen < (int)sizeof(pbuf) - 1) {
                            pbuf[plen++] = (char)k; pbuf[plen] = '\0';
                        }
                        break;
                }
            } else if (tab == TAB_SETTINGS && editing) {
                /* ---- typing a colour code (valid codes preview live) ---- */
                Color *ec = row_color(set_row());
                switch (k) {
                    case '\r': case '\n':
                        editing = 0;
                        if (elen == 0) { set_color(ec, eprev); break; }
                        if (set_color(ec, ebuf)) { save_settings(); set_msg = "color updated"; }
                        else {
                            set_color(ec, eprev);
                            set_msg = "invalid code - use #rrggbb, 0-255 or default";
                        }
                        break;
                    case 27: editing = 0; set_color(ec, eprev); break;   /* esc: cancel */
                    case 127: case 8:                                /* backspace */
                        if (elen > 0) ebuf[--elen] = '\0';
                        if ((ebuf[0] == '#' && elen == 4) || !set_color(ec, ebuf))
                            set_color(ec, eprev);   /* #rgb shorthand applies on enter only */
                        break;
                    case 3: set_color(ec, eprev); running = 0; break;
                    default:
                        if (k > 32 && k < 127 && elen < (int)sizeof(ebuf) - 1) {
                            ebuf[elen++] = (char)k; ebuf[elen] = '\0';
                            if ((ebuf[0] == '#' && elen == 4) || !set_color(ec, ebuf))
                            set_color(ec, eprev);   /* #rgb shorthand applies on enter only */
                        }
                        break;
                }
            } else if (tab == TAB_SETTINGS) {
                /* ---- settings list ---- */
                switch (k) {
                    case KEY_UP: case 'k': if (setsel > 0) setsel--; break;
                    case KEY_DOWN: case 'j': if (setsel < nset - 1) setsel++; break;
                    case KEY_HOME: case 'g': setsel = 0; break;
                    case KEY_END: case 'G': setsel = nset - 1; break;
                    case '\r': case '\n': case KEY_RIGHT:
                        if (set_row() == SET_MUSIC) {    /* edit the path, starting from the current one */
                            editing = 2; set_msg = "";
                            snprintf(pbuf, sizeof pbuf, "%.*s", (int)sizeof pbuf - 1, lib_dir);
                            plen = (int)strlen(pbuf);
                        } else if (row_color(set_row())) {
                            editing = 1; elen = 0; ebuf[0] = '\0'; set_msg = "";
                            snprintf(eprev, sizeof eprev, "%s", row_color(set_row())->code);
                        } else if (set_row() == SET_RESET) {
                            if (k != KEY_RIGHT) settings_reset();
                        } else setting_step(1);
                        break;
                    case KEY_LEFT: setting_step(-1); break;
                    case ' ': case 'p': if (cur >= 0) toggle_pause(); break;
                    case 'n': next_track(); break;
                    case 'b': prev_track(); break;
                    case '-': case '_': set_volume(-vol_step); break;
                    case '+': case '=': set_volume(vol_step); break;
                    case '\t': switch_tab(tab_step(1)); break;
                    case KEY_BTAB: switch_tab(tab_step(-1)); break;
                    case '1': case '2': case '3': case '4': case '5': case '6':
                        tab_jump(k - '1'); break;         /* jump straight to a tab */
                    case 'q': case 3: running = 0; break;
                }
            } else if (tab == TAB_SCOPE) {
                /* ---- oscilloscope ---- */
                switch (k) {
                    case 'm': sc_mode = (sc_mode + 1) % 3; save_settings(); break;
                    case KEY_UP: case 'k':   sc_span = cycle_opt(sc_span_opts, 5, sc_span, 1);  save_settings(); break;
                    case KEY_DOWN: case 'j': sc_span = cycle_opt(sc_span_opts, 5, sc_span, -1); save_settings(); break;
                    case ' ': case 'p':
                        if (cur < 0) { if (nview > 0) play_track(view[sel], 0); }
                        else toggle_pause();
                        break;
                    case 'n': next_track(); break;
                    case 'b': prev_track(); break;
                    case KEY_RIGHT: seek(seek_step); break;
                    case KEY_LEFT: seek(-seek_step); break;
                    case '-': case '_': set_volume(-vol_step); break;
                    case '+': case '=': set_volume(vol_step); break;
                    case 'r': repeat = (repeat + 1) % 3; save_settings(); break;
                    case 's': shuffle = !shuffle; build_queue(); save_settings(); break;
                    case '\t': switch_tab(tab_step(1)); break;
                    case KEY_BTAB: switch_tab(tab_step(-1)); break;
                    case '1': case '2': case '3': case '4': case '5': case '6':
                        tab_jump(k - '1'); break;         /* jump straight to a tab */
                    case 'q': case 3: running = 0; break;
                }
            } else if (searching) {
                /* ---- search input mode (filters the active list) ---- */
                switch (k) {
                    case KEY_UP:
                        if (alist) { if (alsel > 0) alsel--; }
                        else      { if (sel > 0) sel--; } break;
                    case KEY_DOWN:
                        if (alist) { if (alsel < nalview - 1) alsel++; }
                        else      { if (sel < nview - 1) sel++; } break;
                    case KEY_HOME:
                        if (alist) alsel = 0; else sel = 0; break;
                    case KEY_END:
                        if (alist) alsel = nalview ? nalview - 1 : 0;
                        else sel = nview ? nview - 1 : 0;
                        break;
                    case KEY_PGUP:
                        if (alist) alsel = alsel > page_size ? alsel - page_size : 0;
                        else sel = sel > page_size ? sel - page_size : 0;
                        break;
                    case KEY_PGDN:
                        if (alist)
                            alsel = alsel + page_size < nalview
                                  ? alsel + page_size : (nalview ? nalview - 1 : 0);
                        else
                            sel = sel + page_size < nview
                                ? sel + page_size : (nview ? nview - 1 : 0);
                        break;
                    case '\r': case '\n':
                        searching = 0;                               /* keep filter */
                        if (alist) { if (nalview > 0) open_album(alview[alsel]); }
                        else if (nview > 0) play_track(view[sel], 0);
                        break;
                    case 27:                                         /* esc: clear */
                        searching = 0; clear_query(); apply_query();
                        break;
                    case 127: case 8:                                /* backspace */
                        if (qlen > 0) { query[--qlen] = '\0'; apply_query(); }
                        break;
                    default:
                        /* printable ASCII/ANSI bytes; decoded KEY_* codes and
                         * ignored keys (-1) never reach this branch */
                        if (k >= 32 && k < 256 && k != 127 &&
                            qlen < (int)sizeof(query) - 1) {
                            query[qlen++] = (char)k; query[qlen] = '\0';
                            apply_query();
                        }
                        break;
                }
            } else if (alist) {
                /* ---- album list ---- */
                switch (k) {
                    case KEY_UP: case 'k': if (alsel > 0) alsel--; break;
                    case KEY_DOWN: case 'j': if (alsel < nalview - 1) alsel++; break;
                    case '\r': case '\n': case KEY_RIGHT: /* enter / right -> open */
                        if (nalview > 0) open_album(alview[alsel]);
                        break;
                    case KEY_HOME: case 'g': alsel = 0; break;
                    case KEY_END: case 'G': alsel = nalview ? nalview - 1 : 0; break;
                    case KEY_PGUP:
                        alsel = alsel > page_size ? alsel - page_size : 0; break;
                    case KEY_PGDN:
                        alsel = alsel + page_size < nalview
                              ? alsel + page_size : (nalview ? nalview - 1 : 0); break;
                    case '\t': switch_tab(tab_step(1)); break;
                    case KEY_BTAB: switch_tab(tab_step(-1)); break;
                    case '1': case '2': case '3': case '4': case '5': case '6':
                        tab_jump(k - '1'); break;         /* jump straight to a tab */
                    case '/': searching = 1; break;
                    case 27: if (query[0]) { clear_query(); rebuild_albums_view(); } break;
                    case 'q': case 3: running = 0; break;   /* Ctrl-C = byte 3 here */
                }
            } else {
                /* ---- track list (Tracks tab or an opened album) ---- */
                switch (k) {
                    case KEY_UP: case 'k': if (sel > 0) sel--; break;
                    case KEY_DOWN: case 'j': if (sel < nview - 1) sel++; break;
                    case '\r': case '\n': if (nview > 0) play_track(view[sel], 0); break;
                    case ' ': case 'p':
                        if (cur < 0) { if (nview > 0) play_track(view[sel], 0); }
                        else toggle_pause();
                        break;
                    case 'n': next_track(); break;
                    case 'b': prev_track(); break;
                    case KEY_RIGHT: seek(seek_step); break;    /* right */
                    case KEY_LEFT: seek(-seek_step); break;    /* left */
                    case '-': case '_': set_volume(-vol_step); break;
                    case '+': case '=': set_volume(vol_step); break;
                    case 'r': repeat = (repeat + 1) % 3; save_settings(); break;
                    case 's': shuffle = !shuffle; build_queue(); save_settings(); break;
                    case '\t': switch_tab(tab_step(1)); break;
                    case KEY_BTAB: switch_tab(tab_step(-1)); break;
                    case '1': case '2': case '3': case '4': case '5': case '6':
                        tab_jump(k - '1'); break;         /* jump straight to a tab */
                    case '/': searching = 1; break;
                    case 27:                      /* esc: clear filter, else leave album */
                        if (query[0]) { clear_query(); rebuild_view(); }
                        else if (in_album) back_to_albums();
                        break;
                    case KEY_HOME: case 'g': sel = 0; break;
                    case KEY_END: case 'G': sel = nview ? nview - 1 : 0; break;
                    case KEY_PGUP:
                        sel = sel > page_size ? sel - page_size : 0; break;
                    case KEY_PGDN:
                        sel = sel + page_size < nview
                            ? sel + page_size : (nview ? nview - 1 : 0); break;
                    case 'q': case 3: running = 0; break;   /* Ctrl-C = byte 3 here */
                }
            }
            draw();
            last_draw = now_sec();
            continue;   /* respond to the key immediately */
        }

        /* probe a small batch of unprobed tracks this tick. Cached tracks
         * already have metadata, so they're skipped instantly. */
        if (probing) {
            int probed = 0;
            while (probed < 4 && probe_idx < ntracks) {
                if (!tracks[probe_idx].probed) {
                    tracks[probe_idx].probed = probe_meta(&tracks[probe_idx]);
                    probed_new += tracks[probe_idx].probed;
                    probed++;
                }
                probe_idx++;
            }
            /* tags just loaded may change albums/grouping: refresh lists,
             * keeping the track-list highlight on the same track. */
            if (probed > 0) {
                build_albums();
                if (on_album_list()) rebuild_albums_view();
                else {
                    int seltrack = (sel < nview) ? view[sel] : -1;
                    rebuild_view();
                    if (seltrack >= 0) select_in_view(seltrack);
                }
            }
            if (probe_idx >= ntracks && probed_new) { save_cache(); probed_new = 0; }
        } else {
            /* idle: don't spin; keys are still polled each tick */
            Sleep(scoping && cur >= 0 && !paused ? 4 : 25);
        }

        /* throttle redraws to ~10/sec so probing/progress stay smooth; the
         * oscilloscope runs at its own frame rate while a track is moving */
        double t = now_sec();
        double every = (scoping && cur >= 0 && !paused) ? 1.0 / sc_fps : 0.1;
        if (t - last_draw >= every) { draw(); last_draw = t; }
    }

    stop_child();
    scope_stop();
    save_cache();          /* persist anything probed this session */
    free_arena();          /* track strings: one bulk release, no per-string frees */
    free_intern();
    if (tracks) { VirtualFree(tracks, 0, MEM_RELEASE); tracks = NULL; }
    term_restore();
    if (relaunch) {
        /* Settings > Music folder: start over on the new folder. Everything
         * above has been saved and released, so this is a clean second run.
         * Windows has no exec: run a fresh copy in this same console, wait
         * for it, and pass its exit code on. It is started exactly as this
         * run was; the saved folder takes over from there. */
        char exe[PATH_MAX], cmd[PATH_MAX * 2 + 16];
        if (GetModuleFileNameA(NULL, exe, sizeof exe)) {
            size_t al = strlen(launch_arg);
            if (al)     /* a trailing backslash would otherwise escape the closing quote */
                snprintf(cmd, sizeof cmd, "\"%s\" \"%s%s\"", exe, launch_arg,
                         launch_arg[al - 1] == '\\' ? "\\" : "");
            else
                snprintf(cmd, sizeof cmd, "\"%s\"", exe);
            STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
            PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
            if (CreateProcessA(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
                CloseHandle(pi.hThread);
                WaitForSingleObject(pi.hProcess, INFINITE);
                DWORD code = 0;
                GetExitCodeProcess(pi.hProcess, &code);
                CloseHandle(pi.hProcess);
                return (int)code;
            }
        }
        fprintf(stderr, "aqua: the music folder is saved, but aqua could not restart itself - start it again.\n");
        return 1;
    }
    return 0;
}
