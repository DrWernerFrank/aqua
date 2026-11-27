/* aqua — a tiny, fast terminal music player.
 *
 * One source file for every platform. The whole player below is shared; only
 * the small platform layer at the edges differs, selected by the compiler:
 *
 *                    Linux / POSIX            Windows
 *   terminal         termios + ANSI           Console API + VT sequences
 *   input            select() on stdin        _kbhit()/ReadConsoleInputW()
 *   children         fork/exec + SIGSTOP      CreateProcess + thread suspend
 *   clock            clock_gettime(MONO)      QueryPerformanceCounter
 *   volume           pactl/wpctl/amixer       Core Audio session of ffplay
 *   fades            ffplay afade filter      session-volume ramp
 *   cache            ~/.cache/aqua            %LOCALAPPDATA%\aqua
 *
 * Audio output is always ffplay and durations/tags always ffprobe (ffmpeg).
 * Build:  make           (or: cc -O2 -o aqua aqua.c; gcc -O2 -o aqua.exe aqua.c)
 * Run:    aqua [directory]   (defaults to current directory)
 */

#ifdef _WIN32
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

/* how long the idle loop may sleep: Windows needs a steady tick for fades
 * and audio-session retries (it has no select() to wake on input) */
#define PLAT_MAX_IDLE_MS 25
#define PLAT_SCOPE_MS 4
#else
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <ctype.h>
#ifdef __GLIBC__
#include <malloc.h>   /* malloc_trim: hand freed cache pages back to the OS */
#endif

/* the idle loop can block in select() this long; a key wakes it at once */
#define PLAT_MAX_IDLE_MS 250
#define PLAT_SCOPE_MS 8
#endif

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

/* key codes for keys that don't arrive as a single byte */
enum {
    KEY_UP = 1000, KEY_DOWN, KEY_RIGHT, KEY_LEFT,   /* arrows */
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_BTAB,                                      /* Shift-Tab */
    KEY_IGNORE = -2                                /* consumed, no action */
};

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
       SET_FADEIN, SET_FADEOUT, SET_KEYBINDS,
       SET_RESET, SET_COUNT };
static const char *const set_names[SET_COUNT] = {
    "Accent color", "Background color", "Text color",
    "Seek step", "Volume step", "Start on tab", "Music folder",
    "Experimental: oscilloscope",
    "Scope mode", "Scope time span", "Scope gain", "Scope trigger",
    "Scope style", "Scope frame rate", "Scope sync delay",
    "Scope X-Y view", "Scope X-Y smoothing",
    "Scope X color (left)", "Scope Y color (right)",
    "Fade in", "Fade out", "Custom keybinds",
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

/* ---- custom keybinds ----
 * Every action keeps one key. The defaults are the keys aqua has always
 * used; changing one makes that key the action's shortcut everywhere (the
 * built-in aliases such as j/k and p keep working as well). The keybind
 * editor is opened from Settings > Custom keybinds. */
enum {
    KB_PLAY, KB_NEXT, KB_PREV, KB_SEEKF, KB_SEEKB,
    KB_UP, KB_DOWN, KB_SHUFFLE, KB_REPEAT, KB_SEARCH, KB_QUIT,
    KB_VOLUP, KB_VOLDN, KB_COUNT
};
static const char *const kb_names[KB_COUNT] = {
    "Play / pause", "Next track", "Previous track",
    "Seek forward", "Seek back", "Move up", "Move down",
    "Shuffle", "Repeat", "Search", "Quit",
    "Volume up", "Volume down"
};
static const int kb_def[KB_COUNT] = {
    ' ', 'n', 'b', KEY_RIGHT, KEY_LEFT,
    KEY_UP, KEY_DOWN, 's', 'r', '/', 'q', '+', '-'
};
static int kb_key[KB_COUNT];
static int kb_open = 0;                  /* 1 = the keybind editor is showing */
static int kbsel = 0;                    /* selection in the keybind editor */
static int kb_capture = 0;               /* 1 = waiting for the next key press */

/* ---- fades ----
 * 0 = off; otherwise the length in seconds. The track itself fades, never the
 * system volume. On Linux ffplay runs the fade in its own filter chain (see
 * fade_filter); on Windows aqua ramps the Core Audio session volume (see
 * fade_tick). Neither restarts the stream or touches another application. */
static double fade_in  = 0.0;
static double fade_out = 0.0;
#ifdef _WIN32
static double fade_mult = 1.0;           /* current fade level, 0..1 */
static int    fade_in_active = 0;        /* this track started with a fade-in */
static double fade_start_pos = 0;        /* track position the fade-in began at */
#endif

static void kb_defaults(void) {
    for (int i = 0; i < KB_COUNT; i++) kb_key[i] = kb_def[i];
}

/* the key that performs action `i`, as text for the UI */
static const char *key_name(int k, char *buf, size_t n) {
    switch (k) {
        case ' ':            return "Space";
        case '\t':           return "Tab";
        case '\r': case '\n':return "Enter";
        case 27:             return "Esc";
        case 127: case 8:    return "Backspace";
        case KEY_UP:         return "Up";
        case KEY_DOWN:       return "Down";
        case KEY_LEFT:       return "Left";
        case KEY_RIGHT:      return "Right";
        case KEY_HOME:       return "Home";
        case KEY_END:        return "End";
        case KEY_PGUP:       return "PgUp";
        case KEY_PGDN:       return "PgDn";
        case KEY_BTAB:       return "Shift-Tab";
        default:
            if (k >= 32 && k < 127) { snprintf(buf, n, "%c", k); return buf; }
            snprintf(buf, n, "#%d", k);
            return buf;
    }
}

/* a custom key is translated to the action's built-in key, so every tab's
 * existing key handling performs the action without knowing about binds */
static int kb_translate(int k) {
    for (int i = 0; i < KB_COUNT; i++)
        if (kb_key[i] != kb_def[i] && k == kb_key[i]) return kb_def[i];
    return k;
}

/* assign `k` to the selected action; if another action already uses it,
 * the two keys swap so no action is left without one */
static void kb_assign(int k) {
    for (int i = 0; i < KB_COUNT; i++) {
        if (i != kbsel && kb_key[i] == k) {
            kb_key[i] = kb_key[kbsel];
            break;
        }
    }
    kb_key[kbsel] = k;
}

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
static int  editing = 0;                 /* 1 colour code, 2 music folder, 3 fade seconds */
static char ebuf[12] = "";               /* the code being typed */
static int  elen = 0;
static char eprev[12] = "";              /* colour to restore if editing is cancelled */
static char fbuf[16] = "";               /* the fade length being typed, in seconds */
static int  flen = 0;
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
static const double fade_opts[] = { 0, 1, 2, 3, 5, 8, 10, 15, 20, 30 };

/* step a fade length through the presets; a typed value between two presets
 * moves to the next one in the chosen direction */
static double cycle_fade(double v, int dir) {
    int n = (int)(sizeof fade_opts / sizeof fade_opts[0]);
    int i = 0;
    while (i < n && fade_opts[i] < v - 1e-9) i++;
    if (i < n && fade_opts[i] <= v + 1e-9)
        return fade_opts[(i + dir + n) % n];
    if (dir > 0) return i < n ? fade_opts[i] : fade_opts[n - 1];
    return i > 0 ? fade_opts[i - 1] : fade_opts[0];
}

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
        else if (!strcmp(ln, "fadein"))     { fade_in  = atof(v); if (fade_in  < 0) fade_in  = 0; if (fade_in  > 60) fade_in  = 60; }
        else if (!strcmp(ln, "fadeout"))    { fade_out = atof(v); if (fade_out < 0) fade_out = 0; if (fade_out > 60) fade_out = 60; }
        else if (!strncmp(ln, "bind_", 5))  {
            int i = atoi(ln + 5);
            if (i >= 0 && i < KB_COUNT && n > 0) kb_key[i] = n;
        }
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
    fprintf(f, "fadein=%.2f\nfadeout=%.2f\n", fade_in, fade_out);
    for (int i = 0; i < KB_COUNT; i++) fprintf(f, "bind_%d=%d\n", i, kb_key[i]);
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
        case SET_FADEIN:   fade_in  = cycle_fade(fade_in,  dir); break;
        case SET_FADEOUT:  fade_out = cycle_fade(fade_out, dir); break;
        default: return;
    }
    set_msg = "saved";
    save_settings();
}

static void settings_reset(void) {
    for (int i = 0; i < 5; i++) set_color(&col[i], col[i].def);
    seek_step = 5; vol_step = 5; start_tab = TAB_TRACKS;
    sc_mode = 0; sc_span = 50; sc_gain = 0; sc_trig = 1; sc_blocks = 0; sc_fps = 30; sc_delay = 200; sc_smooth = 3; sc_xyview = 1;
    fade_in = fade_out = 0; kb_defaults(); kb_open = 0; kb_capture = 0;
    scope_on = 0; settings_rows();           /* experimental features go back to off */
    set_msg = "all settings reset";
    save_settings();
}

/* ---- process lifetime ----
 * Nothing may outlive aqua: a crash, a closed window or ^C must take the
 * audio with it, and a second launch must not leave the first one playing.
 * Windows does this with a kill-on-close job plus a named mutex/event; POSIX
 * with SIGINT/SIGTERM/SIGHUP handlers that leave through the normal path. */
#ifdef _WIN32
/* Every child (ffplay, the scope decoder, ffprobe) is put in one job with
 * KILL_ON_JOB_CLOSE. If aqua dies for any reason - q, the window's X button,
 * Task Manager, a crash - Windows closes the job and takes the children with
 * it, so no ffplay is left playing in the background. */
static HANDLE g_job = NULL;

static void job_init(void) {
    g_job = CreateJobObjectA(NULL, NULL);
    if (!g_job) return;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    memset(&li, 0, sizeof li);
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
}

static void job_add(HANDLE h) {
    if (g_job && h) AssignProcessToJobObject(g_job, h);
}

/* Opening aqua again asks the running copy to quit (it stops ffplay on the
 * way out) and waits for it, so a second launch doesn't leave the first
 * one's audio playing underneath it. */
static HANDLE g_instance = NULL, g_quit_ev = NULL;

static void instance_init(void) {
    g_instance = CreateMutexA(NULL, TRUE, "Local\\aqua.single");
    int first = !(g_instance && GetLastError() == ERROR_ALREADY_EXISTS);
    g_quit_ev = CreateEventA(NULL, TRUE, FALSE, "Local\\aqua.quit");
    if (first) {
        if (g_quit_ev) ResetEvent(g_quit_ev);
    } else {
        if (g_quit_ev) SetEvent(g_quit_ev);
        if (g_instance) {
            DWORD w = WaitForSingleObject(g_instance, 3000);
            if ((w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) && g_quit_ev)
                ResetEvent(g_quit_ev);
        }
    }
}

static int instance_quit_requested(void) {
    return g_quit_ev && WaitForSingleObject(g_quit_ev, 0) == WAIT_OBJECT_0;
}

static void instance_release(void) {
    if (g_instance) { ReleaseMutex(g_instance); CloseHandle(g_instance); g_instance = NULL; }
    if (g_quit_ev)  { CloseHandle(g_quit_ev); g_quit_ev = NULL; }
}

/* Ctrl-Break / console close: leave via the normal exit path so the cache
 * is written and the console is restored (Ctrl-C itself arrives as byte 3,
 * because ENABLE_PROCESSED_INPUT is off). */
static volatile int g_stop = 0;
static BOOL WINAPI on_console_ctrl(DWORD ev) {
    (void)ev;
    g_stop = 1;
    return TRUE;                        /* we'll exit cleanly on our own */
}

static void platform_init(void) {
    job_init();
    instance_init();
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
}

static void platform_release(void) { instance_release(); }
#else
/* ^C (and TERM/HUP) must leave via the normal exit path: dying by default
 * action would leave the terminal raw, ffplay running, and the metadata
 * cache unsaved. */
static volatile sig_atomic_t g_stop = 0;
static void on_stop(int s) { (void)s; g_stop = 1; }
static int instance_quit_requested(void) { return 0; }

static void platform_init(void) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    signal(SIGHUP, on_stop);
}

static void platform_release(void) { }
static const char *g_argv0 = "aqua";     /* how we were started, to start over the same way */
#endif

/* ---- child (ffplay) control ---- */
#ifdef _WIN32
static HANDLE child_h = NULL;   /* ffplay process handle, NULL = none */
static DWORD  child_pid = 0;
#else
static pid_t child = -1;        /* ffplay pid, -1 = none */
#endif
static int paused = 0;
static int intentional = 0;  /* we killed ffplay, don't auto-advance */
static int play_gen = 0;     /* bumped on every (re)start: the scope decoder follows it */
static void scope_stop(void);
static int volume = 100;     /* the single source of truth for output level */
enum { REP_OFF, REP_ALL, REP_ONE };
static int repeat = REP_OFF; /* off / repeat-all (loop queue) / repeat-one (track) */
static int shuffle = 0;      /* random play order */

#ifndef _WIN32
/* Volume backends, tried in this order. `volume` is the authority; the chosen
 * backend applies it live. ffplay itself carries the volume only when no
 * backend exists (VOL_NONE), where a debounced restart is the only way to
 * change a running stream. */
enum { VOL_PACTL, VOL_WPCTL, VOL_AMIXER, VOL_NONE };
static int vol_backend = VOL_NONE;
static int vol_dirty = 0;        /* pactl: the running stream lacks `volume` */
static int vol_fails = 0;        /* consecutive backend failures -> degrade */
static int spawned_vol = 100;    /* -volume the running ffplay started with */
static double restart_at = 0;    /* debounced volume restart, 0 = none */
#endif

/* play queue: the sequence next/prev walk through. It mirrors the current
 * view[] (so search results become the queue), optionally shuffled. */
static int *queue = NULL;
static int nqueue = 0;

/* playback clock (seconds, monotonic) */
static double t_start_sec = 0;
static double base_elapsed = 0;

/* ---- console state ---- */
#ifdef _WIN32
static HANDLE hOut = INVALID_HANDLE_VALUE, hIn = INVALID_HANDLE_VALUE;
static DWORD  orig_out_mode = 0, orig_in_mode = 0;
static UINT   orig_out_cp = 0;
#else
static struct termios orig_termios;
#endif

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

/* last path separator: Windows paths mix / and \, POSIX only uses / */
static char *last_sep(char *s) {
    char *a = strrchr(s, '/');
#ifdef _WIN32
    char *b = strrchr(s, '\\');
    return (a > b) ? a : b;
#else
    return a;
#endif
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

/* canonical absolute path, or NULL if it cannot be resolved */
static const char *abs_path(const char *p, char *out) {
#ifdef _WIN32
    return _fullpath(out, p, PATH_MAX) ? out : NULL;
#else
    return realpath(p, out);
#endif
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
        int newcap = capacity ? capacity * 2 : 4096;
#ifdef _WIN32
        /* grow via VirtualAlloc: the old block is released to the OS on the
         * spot, where a CRT realloc would leave the freed copy committed */
        Track *nu = VirtualAlloc(NULL, (SIZE_T)newcap * sizeof(Track),
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!nu) { fprintf(stderr, "out of memory\n"); exit(1); }
        if (tracks) {
            memcpy(nu, tracks, (size_t)ntracks * sizeof(Track));
            VirtualFree(tracks, 0, MEM_RELEASE);
        }
        tracks = nu;
#else
        /* start big: a single large (page-mapped) block instead of a ladder
         * of small reallocs whose freed copies would stay resident */
        tracks = realloc(tracks, (size_t)newcap * sizeof(Track));
        if (!tracks) { perror("realloc"); exit(1); }
#endif
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
#ifndef _WIN32
    char ex[PATH_MAX];
    if (p[0] == '~' && (p[1] == '/' || p[1] == '\0') && getenv("HOME")) {   /* no shell here to expand ~ */
        snprintf(ex, sizeof ex, "%s%s", getenv("HOME"), p + 1);
        p = ex;
    }
#endif
    if (!abs_path(p, abs)) { set_msg = "no such folder"; return; }
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
 * ffprobe is launched directly with the path as one argv element (no shell,
 * and no cmd.exe on Windows either), so filenames can't inject commands — a
 * name like $(touch pwned).mp3 is just a name. Returns 1 when the probe is
 * definitive (ffprobe ran, even if it found no duration), 0 when ffprobe
 * itself couldn't run — the file is then retried next session. */

/* append s to a Windows command line, quoted the way MSVCRT argv parsing
 * expects: inner quotes are escaped and the backslashes before them doubled */
#ifdef _WIN32
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
#endif

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

#ifdef _WIN32
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
    job_add(pi.hProcess);
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
#else
static int probe_meta(Track *t) {
    int fds[2];
    if (pipe(fds) != 0) return 0;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return 0; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        int n = open("/dev/null", O_WRONLY);
        if (n >= 0) dup2(n, STDERR_FILENO);
        execlp("ffprobe", "ffprobe", "-v", "error", "-show_entries",
               "format=duration:format_tags=title,artist,album,track,TITLE,ARTIST,ALBUM,TRACK",
               "-of", "default=noprint_wrappers=1", t->path, (char *)NULL);
        _exit(127);                    /* ffprobe missing */
    }
    close(fds[1]);
    int definitive = 0;
    FILE *p = fdopen(fds[0], "r");
    if (p) {
        char line[1024];
        while (fgets(line, sizeof line, p)) probe_line(t, line);
        fclose(p);
        int st = 0;
        if (waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) != 127)
            definitive = 1;
    } else {
        close(fds[0]);
        waitpid(pid, NULL, 0);
    }
    apply_group(t);
    return definitive;
}
#endif

/* ---- metadata cache ----
 * Linux: ~/.cache/aqua/library.tsv (settings in ~/.config/aqua/settings)
 * Windows: %LOCALAPPDATA%\aqua\library.tsv (settings beside it). The format
 * is shared, so a library scanned on one platform is instant on the other.
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
#ifdef _WIN32
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
#else
    const char *home = getenv("HOME");
    if (!home || !*home) home = ".";
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%.*s/.cache", (int)(sizeof dir - 32), home);
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%.*s/.cache/aqua", (int)(sizeof dir - 32), home);
    mkdir(dir, 0755);
    snprintf(cache_file, sizeof cache_file, "%.*s/library.tsv",
             (int)(sizeof cache_file - 20), dir);
    /* settings live with other per-user config, not in the cache */
    snprintf(dir, sizeof dir, "%.*s/.config", (int)(sizeof dir - 32), home);
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%.*s/.config/aqua", (int)(sizeof dir - 32), home);
    mkdir(dir, 0755);
    snprintf(settings_file, sizeof settings_file, "%.*s/settings",
             (int)(sizeof settings_file - 20), dir);
#endif
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

/* one-shot buffers: on Windows they come straight from the OS, so releasing
 * them returns the pages at once; on POSIX malloc/free is enough. */
static void *big_alloc(size_t n) {
#ifdef _WIN32
    return VirtualAlloc(NULL, n ? n : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    return malloc(n ? n : 1);
#endif
}

static void big_free(void *p) {
    if (!p) return;
#ifdef _WIN32
    VirtualFree(p, 0, MEM_RELEASE);
#else
    free(p);
#endif
}

static void load_cache(void) {
    FILE *f = fopen(cache_file, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return; }
    if (sz == 0) { fclose(f); return; }
    /* one whole-file buffer: it is freed once, right after the merge */
    cache_buf = big_alloc((size_t)sz + 2);
    if (!cache_buf) { fclose(f); return; }
    size_t got = fread(cache_buf, 1, (size_t)sz, f);
    fclose(f);
    if (got == 0) { big_free(cache_buf); cache_buf = NULL; return; }
    cache_buf[got] = '\n';               /* every line ends with \n */
    cache_buf[got + 1] = '\0';

    /* exact array size: no growth churn on load. Freed whole after the
     * merge, so the pages return to the OS */
    int lines = 1;
    for (size_t i = 0; i < got; i++)
        if (cache_buf[i] == '\n') lines++;
    capcache = lines;
    cache = big_alloc((size_t)capcache * sizeof(CacheEnt));
    if (!cache) { big_free(cache_buf); cache_buf = NULL; return; }

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
    if (ncache == 0) { big_free(cache); cache = NULL; capcache = 0; }
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
        const char *ap = abs_path(tracks[i].path, rp) ? rp : tracks[i].path;
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

/* ---- terminal ---- */
static void term_raw(void) {
#ifdef _WIN32
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
#else
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);   /* ISIG stays on: ^C arrives as a signal */
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
#endif
    printf("\033[?25l");      /* hide cursor */
    fflush(stdout);
}

static void term_restore(void) {
#ifdef _WIN32
    if (col[SET_BG].sgr[0]) printf("\033[0m\033[2J\033[H");   /* don't leave our background behind */
    printf("\033[?25h\033[0m\r\n");  /* show cursor */
    fflush(stdout);
    if (hOut != INVALID_HANDLE_VALUE) SetConsoleMode(hOut, orig_out_mode);
    if (hIn  != INVALID_HANDLE_VALUE) SetConsoleMode(hIn,  orig_in_mode);
    if (orig_out_cp) SetConsoleOutputCP(orig_out_cp);
#else
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    if (col[SET_BG].sgr[0]) printf("\033[0m\033[2J\033[H");   /* don't leave our background behind */
    printf("\033[?25h\033[0m\n");  /* show cursor */
    fflush(stdout);
#endif
}

static void get_size(int *rows, int *cols) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
        *rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        *cols = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
        if (*rows < 1) *rows = 24;
        if (*cols < 1) *cols = 80;
    } else { *rows = 24; *cols = 80; }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row) {
        *rows = ws.ws_row; *cols = ws.ws_col;
    } else { *rows = 24; *cols = 80; }
#endif
}

/* ---- playback clock ---- */
static double now_sec(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

static double elapsed(void) {
    if (cur < 0) return 0;
    if (paused) return base_elapsed;
    return base_elapsed + (now_sec() - t_start_sec);
}

/* ---- pause / resume ----
 * Windows has no SIGSTOP; walk the thread table and Suspend/Resume each
 * thread of the child. This is the faithful analogue of pause/continue. */
#ifdef _WIN32
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
 * plain `gcc -O2 -Wall -o aqua.exe aqua.c`. */
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
            if (fade_mult < 1.0)
                session_volume_op(child_pid, 1, (float)(volume / 100.0 * fade_mult), NULL);
            vol_sync = 0;
            return;
        }
    } else if (session_volume_op(child_pid, 1, (float)(volume / 100.0 * fade_mult), NULL)) {
        vol_known = volume;
        vol_sync = 0;
        return;
    }
    if (now_sec() - vol_sync_t0 > 5.0) vol_sync = 0;   /* no audio session: give up */
}

/* Ramp the live session volume for a fade-in / fade-out. Runs every loop
 * tick; it only touches the audio session when the level actually changes. */
static void fade_tick(void) {
    if (cur < 0 || !child_h) { fade_mult = 1.0; fade_in_active = 0; return; }
    double el = elapsed();
    double m = 1.0;
    if (fade_in_active && fade_in > 0) {
        double t = el - fade_start_pos;
        if (t < 0) t = 0;
        if (t < fade_in) m = t / fade_in;
        else fade_in_active = 0;
    }
    double dur = tracks[cur].dur;
    if (fade_out > 0 && dur > 0) {
        double rem = dur - el;
        if (rem < fade_out) {
            double f = rem / fade_out;
            if (f < 0) f = 0;
            if (f < m) m = f;
        }
    }
    if (m < 0) m = 0;
    if (m > 1) m = 1;
    if (m != fade_mult) {
        fade_mult = m;
        if (session_volume_op(child_pid, 1, (float)(volume / 100.0 * fade_mult), NULL))
            vol_known = volume;
    }
}

#endif  /* _WIN32: thread suspend + Core Audio */

/* ---- fades on Linux: ffplay's own filter chain ----
 * ffplay runs the track through an afade filter, so no second process and no
 * volume change outside aqua's stream is involved. A fade-out is placed at the
 * end of the audio that is left after `startpos`; ffplay resets filter
 * timestamps at -ss, so a seek keeps the fade at track end. `dur` < 0
 * (unknown length) can only take a fade-in. */
#ifndef _WIN32
static void fade_filter(char *buf, size_t n, double startpos, double dur) {
    size_t o = 0;
    buf[0] = '\0';
    if (fade_in > 0 && startpos <= 0.0) {
        int w = snprintf(buf, n, "afade=t=in:st=0:d=%.2f", fade_in);
        if (w < 0) return;
        if ((size_t)w >= n) return;          /* truncated: nothing to append to */
        o = (size_t)w;
    }
    if (fade_out > 0 && dur > 0) {
        double rem = dur - (startpos > 0 ? startpos : 0);
        if (rem > 0.05) {
            double st = rem - fade_out;
            double d  = fade_out;
            if (st < 0) st = 0;
            if (d > rem) d = rem;
            int w = snprintf(buf + o, n - o, "%safade=t=out:st=%.2f:d=%.2f",
                             o ? "," : "", st, d);
            if (w < 0 || (size_t)w >= n - o) buf[n - 1] = '\0';
        }
    }
}
#endif

/* ---- child control ---- */
static void stop_child(void) {
#ifdef _WIN32
    if (child_h) {
        intentional = 1;
        if (paused) resume_process(child_pid);
        TerminateProcess(child_h, 0);
        WaitForSingleObject(child_h, 2000);
        CloseHandle(child_h);
        child_h = NULL;
        child_pid = 0;
    }
    fade_mult = 1.0;
    fade_in_active = 0;
#else
    if (child > 0) {
        intentional = 1;
        if (paused) kill(child, SIGCONT);
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        child = -1;
    }
#endif
    paused = 0;
}

/* reap a naturally-finished track; returns 1 if it just ended */
static int child_finished(void) {
#ifdef _WIN32
    if (!child_h) return 0;
    if (WaitForSingleObject(child_h, 0) != WAIT_OBJECT_0) return 0;
    CloseHandle(child_h);
    child_h = NULL;
    child_pid = 0;
    return 1;
#else
    if (child <= 0) return 0;
    int st;
    pid_t r = waitpid(child, &st, WNOHANG);
    if (r != child) return 0;
    child = -1;
    return 1;
#endif
}

/* start ffplay for one track (the platform's process + volume plumbing) */
static void start_ffplay(int idx, double startpos) {
#ifdef _WIN32
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
        job_add(pi.hProcess);
        CloseHandle(pi.hThread);
        child_h = pi.hProcess;
        child_pid = pi.dwProcessId;
        request_vol_sync();      /* bring the new stream to `volume` once it exists */
    } else {
        child_h = NULL;
        child_pid = 0;
    }
#else
    /* ffplay carries the volume itself only when no live backend can adjust
     * a running stream; otherwise keep software volume neutral and let the
     * backend own the level (so it is never applied twice). */
    int vol = (vol_backend == VOL_NONE) ? volume : 100;
    spawned_vol = vol;
    char ss[32] = "";
    if (startpos > 0) snprintf(ss, sizeof ss, "%.2f", startpos);
    char vols[16];
    snprintf(vols, sizeof vols, "%d", vol);
    char af[96];
    fade_filter(af, sizeof af, startpos, tracks[idx].dur);

    pid_t pid = fork();
    if (pid == 0) {
        int n = open("/dev/null", O_WRONLY);
        if (n >= 0) { dup2(n, STDOUT_FILENO); dup2(n, STDERR_FILENO); }
        char *av[16];
        int ac = 0;
        av[ac++] = "ffplay"; av[ac++] = "-nodisp"; av[ac++] = "-autoexit";
        av[ac++] = "-loglevel"; av[ac++] = "quiet";
        av[ac++] = "-volume"; av[ac++] = vols;
        if (startpos > 0) { av[ac++] = "-ss"; av[ac++] = ss; }
        if (af[0]) { av[ac++] = "-af"; av[ac++] = af; }
        av[ac++] = tracks[idx].path;
        av[ac] = NULL;
        execvp("ffplay", av);
        _exit(127);
    }
    child = pid;
#endif
}

static void play_track(int idx, double startpos) {
    if (idx < 0 || idx >= ntracks) return;
    stop_child();
    cur = idx;
    play_gen++;
    base_elapsed = startpos > 0 ? startpos : 0;
    t_start_sec = now_sec();
    paused = 0;
#ifdef _WIN32
    fade_in_active = (fade_in > 0 && startpos <= 0.0);
    fade_start_pos = startpos > 0 ? startpos : 0;
    fade_mult = fade_in_active ? 0.0 : 1.0;
#else
    restart_at = 0;                    /* the new stream already carries it */
    vol_dirty = (vol_backend == VOL_PACTL && volume != 100);
#endif
    start_ffplay(idx, startpos);
    intentional = 0;
}

static void toggle_pause(void) {
#ifdef _WIN32
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
#else
    if (child <= 0) return;
    if (paused) {
        /* if the volume changed while paused and only a respawn can apply it,
         * resume from the paused position with the new volume */
        if (vol_backend == VOL_NONE && volume != spawned_vol) {
            play_track(cur, base_elapsed);
            return;
        }
        t_start_sec = now_sec();
        kill(child, SIGCONT);
        paused = 0;
    } else {
        base_elapsed = elapsed();
        kill(child, SIGSTOP);
        paused = 1;
    }
#endif
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
    else if (tab == TAB_SETTINGS) { setsel = 0; editing = 0; kb_open = 0; kb_capture = 0; set_msg = ""; settings_rows(); }
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

/* ---- volume ----
 * `volume` itself is shared; how a change reaches the running stream is not.
 * Both platforms apply it live, without restarting the track. */
static void volume_init(void) {
#ifndef _WIN32
    /* pick the first backend that actually answers — presence alone isn't
     * enough. Windows needs no detection: Core Audio is always there. */
    if (system("pactl info >/dev/null 2>&1") == 0)              vol_backend = VOL_PACTL;
    else if (system("wpctl status >/dev/null 2>&1") == 0)       vol_backend = VOL_WPCTL;
    else if (system("amixer sget Master >/dev/null 2>&1") == 0) vol_backend = VOL_AMIXER;
    else                                                        vol_backend = VOL_NONE;
#endif
}

#ifdef _WIN32
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

static void volume_tick(void) {
    sync_volume();   /* no-op unless a volume change is still waiting on the stream */
    fade_tick();     /* ramp the session volume during a fade-in / fade-out */
}

/* does the loop need to wake early for a pending volume change? */
static int volume_waiting(void) { return 0; }   /* capped by PLAT_MAX_IDLE_MS */
#else
/* Change the volume of the live ffplay stream via PulseAudio (no restart, no
 * gap). Matches the sink-input by ffplay's PID. Returns 1 on success. */
static int pulse_set_volume(pid_t pid, int pct) {
    if (vol_backend != VOL_PACTL || pid <= 0) return 0;
    char cmd[640];
    snprintf(cmd, sizeof cmd,
        "idx=$(pactl list sink-inputs 2>/dev/null | "
        "awk '/Sink Input #/{i=substr($3,2)} "
        "/application.process.id/{gsub(/\"/,\"\");if($3==%d){print i;exit}}'); "
        "[ -n \"$idx\" ] && pactl set-sink-input-volume \"$idx\" %d%% >/dev/null 2>&1",
        (int)pid, pct);
    return system(cmd) == 0;
}

/* System-level fallbacks (PipeWire's wpctl, then plain amixer). They move the
 * default sink rather than aqua's own stream, but they never interrupt audio. */
static int wire_set_volume(int pct) {
    char cmd[160];
    if (vol_backend == VOL_WPCTL)
        snprintf(cmd, sizeof cmd,
                 "wpctl set-volume @DEFAULT_AUDIO_SINK@ %d%% >/dev/null 2>&1", pct);
    else
        snprintf(cmd, sizeof cmd,
                 "amixer -q sset Master %d%% >/dev/null 2>&1", pct);
    return system(cmd) == 0;
}

static void set_volume(int delta) {
    volume += delta;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (vol_backend == VOL_WPCTL || vol_backend == VOL_AMIXER) {
        if (wire_set_volume(volume)) vol_fails = 0;
        else if (++vol_fails >= 3) vol_backend = VOL_NONE;  /* broken: restart instead */
        return;
    }
    if (vol_backend == VOL_PACTL) {
        /* Live, gapless change — try right now so the key press is heard
         * instantly. If ffplay hasn't registered its stream yet (right after
         * a track starts), retry from the main loop instead of restarting
         * the track — a restart is what makes the sound drop out. */
        if (pulse_set_volume(child, volume)) { vol_dirty = 0; return; }
        vol_dirty = 1;
        return;                     /* idle: the next spawn re-applies it */
    }
    /* VOL_NONE: only a respawn can change the running stream. Debounce so held
     * keys coalesce into one restart; a change made while paused is applied
     * on resume (see toggle_pause). */
    if (cur >= 0 && child > 0 && !paused)
        restart_at = now_sec() + 0.25;
}

/* keep the running stream's volume in sync (called each main-loop pass) */
static void volume_tick(void) {
    if (restart_at > 0 && now_sec() >= restart_at) {
        restart_at = 0;
        if (cur >= 0 && child > 0 && !paused) play_track(cur, elapsed());
    }
    if (vol_dirty && child <= 0)
        vol_dirty = 0;                 /* nothing to sync; next spawn carries it */
    else if (vol_dirty && child > 0) {
        if (pulse_set_volume(child, volume)) { vol_dirty = 0; vol_fails = 0; }
        else if (++vol_fails >= 20) { vol_backend = VOL_NONE; vol_dirty = 0; vol_fails = 0; }
    }
}

/* does the loop need to wake early for a pending volume change? */
static int volume_waiting(void) { return vol_dirty || restart_at > 0; }
#endif

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

/* the decoder process: ffmpeg writes PCM into a pipe we read without
 * blocking, so the picture follows the playback clock. */
#ifdef _WIN32
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
        job_add(pi.hProcess);
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
#else
static pid_t sc_pid = -1;
static int sc_fd = -1;

static void scope_stop(void) {
    if (sc_pid > 0) {
        kill(sc_pid, SIGKILL);
        waitpid(sc_pid, NULL, 0);
        sc_pid = -1;
    }
    if (sc_fd >= 0) { close(sc_fd); sc_fd = -1; }
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

    int p[2];
    if (pipe(p) != 0) { sc_failed = 1; return; }
    char ss[32], rate[16];
    snprintf(ss, sizeof ss, "%.3f", pos);
    snprintf(rate, sizeof rate, "%d", SC_RATE);
    pid_t pid = fork();
    if (pid == 0) {
        int n = open("/dev/null", O_RDWR);
        if (n >= 0) { dup2(n, STDIN_FILENO); dup2(n, STDERR_FILENO); }
        dup2(p[1], STDOUT_FILENO);
        close(p[0]); close(p[1]);
        execlp("ffmpeg", "ffmpeg", "-v", "quiet", "-nostdin", "-ss", ss,
               "-i", tracks[cur].path, "-vn", "-f", "s16le", "-ac", "2",
               "-ar", rate, "-", (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) { close(p[0]); sc_failed = 1; return; }
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
    sc_pid = pid;
    sc_fd = p[0];
    sc_failed = 0;
}

/* called every main-loop tick while the Scope tab is showing */
static void scope_pump(void) {
    if (cur < 0 || child <= 0) { if (sc_pid > 0) scope_stop(); return; }
    if (sc_gen != play_gen) scope_start();
    if (sc_fd < 0) return;
    long long target = (long long)((elapsed() - sc_delay / 1000.0 - sc_base) * SC_RATE);
    static unsigned char buf[16384];
    static int carry = 0;                /* bytes of a split frame kept in buf[] */
    if (sc_read == 0 && sc_w == 0) carry = 0;
    for (int budget = 64; sc_read < target && budget > 0; budget--) {
        long long want = (target - sc_read) * 4 - carry;
        if (want > (long long)sizeof buf - carry) want = (long long)sizeof buf - carry;
        if (want <= 0) break;
        ssize_t got = read(sc_fd, buf + carry, (size_t)want);
        if (got < 0) break;              /* nothing ready (or interrupted): next tick */
        if (got == 0) {                  /* decoder finished - or never started */
            int st;
            if (waitpid(sc_pid, &st, WNOHANG) == sc_pid) {
                if (WIFEXITED(st) && WEXITSTATUS(st) == 127) sc_failed = 1;   /* no ffmpeg */
                sc_pid = -1;
            }
            close(sc_fd); sc_fd = -1;
            break;
        }
        int have = carry + (int)got, frames = have / 4;
        sc_push((const short *)(void *)buf, frames);
        sc_read += frames;
        carry = have - frames * 4;
        if (carry) memmove(buf, buf + frames * 4, (size_t)carry);
    }
}
#endif

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

/* write a whole chunk. POSIX retries past partial writes and EINTR; the
 * Windows console write is a buffered fwrite of the whole chunk. */
static void write_all(const char *buf, int n) {
#ifdef _WIN32
    if (n > 0) { fwrite(buf, 1, (size_t)n, stdout); fflush(stdout); }
#else
    size_t left = (size_t)n;
    while (left > 0) {
        ssize_t w = write(STDOUT_FILENO, buf, left);
        if (w > 0) { buf += w; left -= (size_t)w; }
        else if (w < 0 && errno == EINTR) continue;
        else break;                          /* terminal gone: drop the frame */
    }
#endif
}

static void delta_flush(Delta *del) {
    if (del->n) { write_all(del->buf, del->n); del->n = 0; }
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
    int kb = settings && kb_open;
    int scope = (tab == TAB_SCOPE);
    int asel   = kb ? kbsel : settings ? setsel : scope ? 0 : album_list ? alsel : sel;   /* active selection */
    int acount = kb ? KB_COUNT : settings ? nset : album_list ? nalview : nview;   /* active list length */

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
    if (settings && kb_open)
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "Settings \xE2\x96\xB8 " AQUA "Keybinds" DGRAY "   %s" RESET,
            kb_capture ? "press a key (esc cancels)" : "Enter: change   Backspace: reset   Esc: back");
    else if (settings && editing == 3)
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "%s seconds: " WHITE "%s" AQUA "\xE2\x96\x88" RESET
            DGRAY "   0 = off" RESET, set_row() == SET_FADEIN ? "Fade in" : "Fade out", fbuf);
    else if (settings && editing == 2) {              /* a long path: keep the end you are typing in view */
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
            if (kb) {                            /* ---- keybind editor row ---- */
                if (r < KB_COUNT) {
                    char kn[16];
                    const char *ks = key_name(kb_key[r], kn, sizeof kn);
                    int nw = cols - 13;
                    if (nw < 8) nw = 8;
                    if (r == kbsel)
                        lo = emit(line, sizeof line, 0,
                            INVAQ " %-*.*s %7s " RESET, nw, nw, kb_names[r], ks);
                    else
                        lo = emit(line, sizeof line, 0,
                            " " WHITE "%-*.*s" RESET " %s%7s" RESET,
                            nw, nw, kb_names[r], GRAY, ks);
                } else if (r == KB_COUNT + 1)
                    lo = emit(line, sizeof line, 0,
                        "  " DGRAY "Enter: press the new key.  Backspace: back to the default.  Esc: Settings" RESET);
                else if (r == KB_COUNT + 2)
                    lo = emit(line, sizeof line, 0,
                        "  " DGRAY "Built-in aliases (j/k, p, =, _, g/G, Home/End, PgUp/PgDn) keep working" RESET);
                else {
                    lo = 0;
                    line[0] = '\0';
                }
                flush_line(&del, row++, line);
                continue;
            }
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
                else if (id == SET_FADEIN)    { if (fade_in  > 0) snprintf(vbuf, sizeof vbuf, "%.1fs", fade_in);  else val = "off"; }
                else if (id == SET_FADEOUT)   { if (fade_out > 0) snprintf(vbuf, sizeof vbuf, "%.1fs", fade_out); else val = "off"; }
                else if (id == SET_KEYBINDS)  val = "edit";
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
    if (settings && kb_open)
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "\xE2\x86\x91\xE2\x86\x93" GRAY " move  " DGRAY "\xE2\x86\xB5" GRAY " change key  "
            DGRAY "Backspace" GRAY " reset  " DGRAY "Esc" GRAY " back" RESET);
    else if (settings && editing == 3)
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "type seconds (0 = off)   " DGRAY "\xE2\x86\xB5" GRAY " apply  "
            DGRAY "esc" GRAY " cancel" RESET);
    else if (settings && editing == 2)
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
        write_all("\033[?2026l", 8);
    }
}

/* ---- input ---- */
#ifdef _WIN32
/* Letter keys on the QWERTY row/rows, by hardware scan code, so that the
 * same physical key still works when a non-Latin keyboard layout is active. */
static int latin_for_scan(const KEY_EVENT_RECORD *ke) {
    static const struct { WORD sc; char ch; } keys[] = {
        {0x10,'q'}, {0x11,'w'}, {0x12,'e'}, {0x13,'r'}, {0x14,'t'}, {0x15,'y'},
        {0x16,'u'}, {0x17,'i'}, {0x18,'o'}, {0x19,'p'}, {0x1E,'a'}, {0x1F,'s'},
        {0x20,'d'}, {0x21,'f'}, {0x22,'g'}, {0x23,'h'}, {0x24,'j'}, {0x25,'k'},
        {0x26,'l'}, {0x2C,'z'}, {0x2D,'x'}, {0x2E,'c'}, {0x2F,'v'}, {0x30,'b'},
        {0x31,'n'}, {0x32,'m'}, {0x35,'/'},
    };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        if (keys[i].sc != ke->wVirtualScanCode) continue;
        char ch = keys[i].ch;
        if (ch >= 'a' && ch <= 'z') {
            int shift = (ke->dwControlKeyState & SHIFT_PRESSED) != 0;
            int caps  = (ke->dwControlKeyState & CAPSLOCK_ON) != 0;
            if (shift != caps) ch = (char)(ch - 'a' + 'A');
        }
        return ch;
    }
    return -1;
}

/* Returns an ASCII byte, a KEY_* code, or -1 (no key / ignored key).
 * Reads key-down events from the console input queue, so the characters are
 * those of the active keyboard layout. Non-ASCII letters (Persian, Russian,
 * Greek...) fall back to the Latin letter on the same physical key. Windows
 * console "extended" keys (arrows, Home/End/PgUp/PgDn...) are mapped by their
 * virtual key, and the rest are dropped (-1) so they can't leak into the query. */
static int key_from_event(const KEY_EVENT_RECORD *ke) {
    switch (ke->wVirtualKeyCode) {
        case VK_UP:     return KEY_UP;
        case VK_DOWN:   return KEY_DOWN;
        case VK_RIGHT:  return KEY_RIGHT;
        case VK_LEFT:   return KEY_LEFT;
        case VK_HOME:   return KEY_HOME;
        case VK_END:    return KEY_END;
        case VK_PRIOR:  return KEY_PGUP;
        case VK_NEXT:   return KEY_PGDN;
        case VK_TAB:
            if (ke->dwControlKeyState & SHIFT_PRESSED) return KEY_BTAB;
            break;
    }
    WCHAR u = ke->uChar.UnicodeChar;
    if (u > 0 && u < 0x80) return u;
    if (u >= 0x80) return latin_for_scan(ke);
    return -1;
}

static int read_key(void) {
    INPUT_RECORD ir;
    DWORD n = 0;
    while (PeekConsoleInputW(hIn, &ir, 1, &n) && n == 1) {
        ReadConsoleInputW(hIn, &ir, 1, &n);
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;
        return key_from_event(&ir.Event.KeyEvent);
    }
    return -1;
}

/* wait up to ms for a key; 1 = one is ready. The console input handle is
 * waitable, so a key wakes the loop at once instead of after a sleep. */
static int wait_key(int ms) {
    if (_kbhit()) return 1;
    if (ms <= 0) return 0;
    if (WaitForSingleObject(hIn, (DWORD)ms) == WAIT_FAILED) Sleep((DWORD)ms);
    return _kbhit();
}
#else
static int read_byte(void) {
    unsigned char c;
    return (read(STDIN_FILENO, &c, 1) == 1) ? c : -1;
}

/* Read one key: a single byte, a lone Esc, or a decoded CSI/SS3 sequence.
 * Unknown sequences (Home variants, F-keys, mouse reports, modifiers) are
 * consumed whole and returned as KEY_IGNORE instead of masquerading as Esc
 * and leaking their tail bytes as garbage keystrokes. */
static int read_key(void) {
    static int pending = -1;            /* one-byte pushback */
    if (pending >= 0) { int p = pending; pending = -1; return p; }

    int c = read_byte();
    if (c < 0) return -1;
    if (c != '\033') return c;

    int s0 = read_byte();
    if (s0 < 0) return '\033';                                  /* lone Esc */
    if (s0 != '[' && s0 != 'O') { pending = s0; return '\033'; } /* Alt+key */

    /* CSI '[' / SS3 'O': parameter bytes, then one final byte */
    int num = 0, b;
    while ((b = read_byte()) >= 0) {
        if (b >= '0' && b <= '9') { num = num * 10 + (b - '0'); continue; }
        if (b == ';' || b == '<' || b == '>' || b == '?' || b == ':' || b == ' ') {
            num = 0;                     /* sub-parameter: start a new number */
            continue;
        }
        if (s0 == 'O')
            switch (b) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
                default:  return KEY_IGNORE;
            }
        switch (b) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
            case 'Z': return KEY_BTAB;
            case '~':
                switch (num) {
                    case 1: case 7: return KEY_HOME;
                    case 4: case 8: return KEY_END;
                    case 5: return KEY_PGUP;
                    case 6: return KEY_PGDN;
                    default:  return KEY_IGNORE;   /* Ins, Del, F-keys */
                }
            default: return KEY_IGNORE;            /* mouse, modifiers, ... */
        }
    }
    return KEY_IGNORE;   /* sequence cut short */
}

/* wait up to ms for input (0 = poll); 1 = a key is waiting */
static int wait_key(int ms) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    int rv = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    return rv > 0 && FD_ISSET(STDIN_FILENO, &fds);
}
#endif

int main(int argc, char **argv) {
#ifndef _WIN32
    g_argv0 = argv[0];
#endif
    platform_init();
    init_cache_path();
    kb_defaults();
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
    if (!abs_path(dir, lib_dir)) snprintf(lib_dir, sizeof lib_dir, "%s", dir);
    qsort(tracks, ntracks, sizeof(Track), cmp_track);

    load_cache();

    /* canonical paths + mtimes, then prefill durations from cache.
     * The canonical path lives in a stack buffer: it's needed only here (as
     * the cache key) and again at save time, so a 5,000-track library
     * doesn't carry 5,000 ~80-byte path strings all session. */
    for (int i = 0; i < ntracks; i++) {
        char rp[PATH_MAX];
        const char *ap = abs_path(tracks[i].path, rp) ? rp : tracks[i].path;
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
    big_free(cache_buf);                  /* pages straight back to the OS */
    cache_buf = NULL;
    /* the cache array: fully consumed -> release it; a few survivors (probed
     * under another folder, or with a newer mtime) -> move them to a compact
     * heap array and release the big one. save_cache() grows it again. */
    if (ncache == 0) { big_free(cache); cache = NULL; capcache = 0; }
    else {
        CacheEnt *heapsrv = xmalloc((size_t)ncache * sizeof(CacheEnt));
        memcpy(heapsrv, cache, (size_t)ncache * sizeof(CacheEnt));
        big_free(cache);
        cache = heapsrv; capcache = ncache;
    }
#ifdef __GLIBC__
    /* the freed cache strings leave holes glibc keeps resident; give the
     * pages back so the steady-state footprint is the library, not the
     * load transient */
    malloc_trim(0);
#endif

    srand((unsigned)time(NULL));
    queue = xmalloc(ntracks * sizeof(int));
    rebuild_view();   /* initial view = all tracks; also builds the queue */
    build_albums();   /* album list for the Albums tab */
    if (start_tab != TAB_TRACKS) switch_tab(start_tab);   /* Settings: Start on tab */

    volume_init();

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
    while (running && !g_stop && !instance_quit_requested()) {
        /* reap a naturally-finished track -> advance */
        if (child_finished()) {
            if (!intentional) {
                if (repeat == REP_ONE && cur >= 0) play_track(cur, 0);
                else next_track();
            }
        }

        volume_tick();   /* live-volume retries, debounced restarts, fades */

        int scoping = (tab == TAB_SCOPE);
        if (scoping) scope_pump();
        int moving = scoping && cur >= 0 && !paused;   /* the scope is animating */

        int probing = probe_idx < ntracks;

        /* While probing, poll key input without blocking so durations make
         * steady progress; otherwise sleep until a key arrives (POSIX) or a
         * short tick (Windows: fades and audio-session retries need it). */
        int wait_ms = probing ? 0 : moving ? PLAT_SCOPE_MS
                    : volume_waiting() ? 50 : PLAT_MAX_IDLE_MS;
        if (wait_key(wait_ms)) {
            int k = read_key();
            int alist = on_album_list();

            if (kb_open) {
                /* ---- keybind editor ---- */
                if (kb_capture) {
                    if (k == 27) { kb_capture = 0; set_msg = "cancelled"; }
                    else if (k == 3) { kb_capture = 0; }
                    else if (k > 0) {
                        kb_assign(k);
                        kb_capture = 0;
                        save_settings();
                        set_msg = "key updated";
                    }
                } else {
                    switch (k) {
                        case KEY_UP: case 'k': if (kbsel > 0) kbsel--; break;
                        case KEY_DOWN: case 'j': if (kbsel < KB_COUNT - 1) kbsel++; break;
                        case KEY_HOME: case 'g': kbsel = 0; break;
                        case KEY_END: case 'G': kbsel = KB_COUNT - 1; break;
                        case '\r': case '\n': kb_capture = 1; set_msg = ""; break;
                        case 127: case 8:
                            kb_key[kbsel] = kb_def[kbsel];
                            save_settings();
                            set_msg = "default restored";
                            break;
                        case 27: case 'q': kb_open = 0; set_msg = ""; break;
                        case 3: running = 0; break;
                    }
                }
                draw();
                last_draw = now_sec();
                continue;
            }

            /* a custom key stands in for the action's built-in key; never
             * while the user is typing text */
            if (!searching && !(tab == TAB_SETTINGS && editing) && k > 0)
                k = kb_translate(k);

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
            } else if (tab == TAB_SETTINGS && editing == 3) {
                /* ---- typing a fade length in seconds ---- */
                switch (k) {
                    case '\r': case '\n': {
                        editing = 0;
                        if (flen == 0) break;            /* nothing typed: keep the fade */
                        double v = atof(fbuf);
                        if (v < 0) v = 0;
                        if (v > 60) v = 60;
                        if (set_row() == SET_FADEIN) fade_in = v;
                        else                         fade_out = v;
                        save_settings();
                        set_msg = v > 0 ? "fade saved" : "fade off";
                        break;
                    }
                    case 27: editing = 0; break;                     /* esc: cancel */
                    case 127: case 8:
                        if (flen > 0) fbuf[--flen] = '\0';
                        break;
                    case 3: running = 0; break;
                    default:
                        if (((k >= '0' && k <= '9') || k == '.') &&
                            flen < (int)sizeof(fbuf) - 1) {
                            fbuf[flen++] = (char)k;
                            fbuf[flen] = '\0';
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
                        } else if (set_row() == SET_KEYBINDS) {
                            kb_open = 1; kbsel = 0; kb_capture = 0; set_msg = "";
                        } else if (set_row() == SET_FADEIN || set_row() == SET_FADEOUT) {
                            if (k == KEY_RIGHT) setting_step(1);
                            else {
                                editing = 3; set_msg = "";
                                fbuf[0] = '\0'; flen = 0;   /* type the new length */
                            }
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
        }

        /* throttle redraws to ~10/sec so probing/progress stay smooth; the
         * oscilloscope runs at its own frame rate while a track is moving */
        double t = now_sec();
        if (t - last_draw >= (moving ? 1.0 / sc_fps : 0.1)) { draw(); last_draw = t; }
    }

    stop_child();
    scope_stop();
    save_cache();          /* persist anything probed this session */
    free_arena();          /* track strings: one bulk release, no per-string frees */
    free_intern();
#ifdef _WIN32
    if (tracks) { VirtualFree(tracks, 0, MEM_RELEASE); tracks = NULL; }
#else
    free(tracks);
    tracks = NULL;
#endif
    term_restore();
    platform_release();    /* the relaunch below is a new instance, not a second copy */
    if (relaunch) {
        /* Settings > Music folder: start over on the new folder. Everything
         * above has been saved and released, so this is a clean second run
         * with the same command line as before; the saved folder takes over. */
#ifdef _WIN32
        /* Windows has no exec: run a fresh copy in this same console, wait
         * for it, and pass its exit code on. */
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
#else
        if (launch_arg[0]) execlp(g_argv0, g_argv0, launch_arg, (char *)NULL);
        else               execlp(g_argv0, g_argv0, (char *)NULL);
#endif
        fprintf(stderr, "aqua: the music folder is saved, but aqua could not restart itself - start it again.\n");
        return 1;
    }
    return 0;
}
