/* aqua — a tiny, fast terminal music player.
 *
 * Zero library dependencies (just POSIX + ANSI escapes). Uses ffplay for
 * audio output and ffprobe for durations, both from ffmpeg.
 *
 * Palette: aqua (cyan), black, gray. No animations.
 *
 * Build:  make      (or: cc -O2 -o aqua aqua.c)
 * Run:    ./aqua [directory]   (defaults to current directory)
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
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

/* ---- track model ---- */
typedef struct {
    char *path;     /* full path passed to ffplay */
    char *name;     /* display name (basename, no extension) */
    char *abspath;  /* canonical path, used as cache key */
    long  mtime;    /* file modification time, for cache validation */
    double dur;     /* seconds, -1 if unknown */
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
static pid_t child = -1;     /* ffplay pid, -1 = none */
static int paused = 0;
static int intentional = 0;  /* we killed ffplay, don't auto-advance */
static int volume = 100;
static int repeat = 0;       /* loop playlist */
static int have_pactl = 0;   /* set if PulseAudio's pactl is available */
static int shuffle = 0;      /* random play order */

/* play order: order[pos] = track index. Identity when shuffle is off. */
static int *order = NULL;

/* playback clock */
static struct timespec t_start;
static double base_elapsed = 0;

static struct termios orig_termios;

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

/* ---- collection ---- */
static char *xstrdup(const char *s) {
    char *p = malloc(strlen(s) + 1);
    if (!p) { perror("malloc"); exit(1); }
    strcpy(p, s);
    return p;
}

static void add_track(const char *path, const char *fname) {
    if (ntracks == capacity) {
        capacity = capacity ? capacity * 2 : 64;
        tracks = realloc(tracks, capacity * sizeof(Track));
        if (!tracks) { perror("realloc"); exit(1); }
    }
    Track *t = &tracks[ntracks++];
    t->path = xstrdup(path);
    /* display name: basename without extension */
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", fname);
    char *dot = strrchr(buf, '.');
    if (dot) *dot = '\0';
    t->name = xstrdup(buf);
    t->abspath = NULL;
    t->mtime = 0;
    t->dur = -1;
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
            add_track(full, e->d_name);
        }
    }
    closedir(d);
}

static int cmp_track(const void *a, const void *b) {
    return strcasecmp(((const Track *)a)->name, ((const Track *)b)->name);
}

/* ---- duration via ffprobe ---- */
static double probe_duration(const char *path) {
    char cmd[2600];
    snprintf(cmd, sizeof cmd,
        "ffprobe -v error -show_entries format=duration "
        "-of default=noprint_wrappers=1:nokey=1 \"%s\" 2>/dev/null", path);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    char out[64] = {0};
    double dur = -1;
    if (fgets(out, sizeof out, p)) dur = atof(out);
    pclose(p);
    return dur > 0 ? dur : -1;
}

/* ---- duration cache (~/.cache/aqua/durations.tsv) ----
 * Each line:  <duration>\t<mtime>\t<abspath>
 * Only new or modified files are probed; everything else is read from here. */
typedef struct { char *path; long mtime; double dur; } CacheEnt;
static CacheEnt *cache = NULL;
static int ncache = 0, capcache = 0;
static char cache_file[PATH_MAX];

static int cmp_cache(const void *a, const void *b) {
    return strcmp(((const CacheEnt *)a)->path, ((const CacheEnt *)b)->path);
}

static void init_cache_path(void) {
    const char *home = getenv("HOME");
    if (!home || !*home) home = ".";
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%.*s/.cache", (int)(sizeof dir - 32), home);
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%.*s/.cache/aqua", (int)(sizeof dir - 32), home);
    mkdir(dir, 0755);
    snprintf(cache_file, sizeof cache_file, "%.*s/durations.tsv",
             (int)(sizeof cache_file - 20), dir);
}

static void load_cache(void) {
    FILE *f = fopen(cache_file, "r");
    if (!f) return;
    char line[PATH_MAX + 64];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        double dur = strtod(p, &p);   if (*p != '\t') continue; p++;
        long  mt  = strtol(p, &p, 10); if (*p != '\t') continue; p++;
        size_t L = strlen(p);
        if (L && p[L - 1] == '\n') p[L - 1] = '\0';
        if (!*p) continue;
        if (ncache == capcache) {
            capcache = capcache ? capcache * 2 : 256;
            cache = realloc(cache, capcache * sizeof(CacheEnt));
            if (!cache) { perror("realloc"); exit(1); }
        }
        cache[ncache].path = xstrdup(p);
        cache[ncache].mtime = mt;
        cache[ncache].dur = dur;
        ncache++;
    }
    fclose(f);
    qsort(cache, ncache, sizeof(CacheEnt), cmp_cache);
}

static double cache_lookup(const char *path, long mtime) {
    if (ncache == 0) return -1;
    CacheEnt key; key.path = (char *)path;
    CacheEnt *e = bsearch(&key, cache, ncache, sizeof(CacheEnt), cmp_cache);
    return (e && e->mtime == mtime) ? e->dur : -1;
}

/* insert or update an entry (linear; runs once at save time) */
static void cache_upsert(const char *path, long mtime, double dur) {
    for (int i = 0; i < ncache; i++)
        if (strcmp(cache[i].path, path) == 0) {
            cache[i].mtime = mtime; cache[i].dur = dur; return;
        }
    if (ncache == capcache) {
        capcache = capcache ? capcache * 2 : 256;
        cache = realloc(cache, capcache * sizeof(CacheEnt));
        if (!cache) { perror("realloc"); exit(1); }
    }
    cache[ncache].path = xstrdup(path);
    cache[ncache].mtime = mtime;
    cache[ncache].dur = dur;
    ncache++;
}

/* merge current durations into the cache and write it out (preserves entries
 * for other folders the user may have opened before). */
static void save_cache(void) {
    if (!cache_file[0]) return;
    for (int i = 0; i < ntracks; i++)
        if (tracks[i].dur > 0 && tracks[i].abspath)
            cache_upsert(tracks[i].abspath, tracks[i].mtime, tracks[i].dur);
    FILE *f = fopen(cache_file, "w");
    if (!f) return;
    for (int i = 0; i < ncache; i++)
        fprintf(f, "%.3f\t%ld\t%s\n", cache[i].dur, cache[i].mtime, cache[i].path);
    fclose(f);
}

/* ---- terminal ---- */
static void term_raw(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l");      /* hide cursor */
    fflush(stdout);
}

static void term_restore(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h\033[0m\n");  /* show cursor */
    fflush(stdout);
}

static void get_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row) {
        *rows = ws.ws_row; *cols = ws.ws_col;
    } else { *rows = 24; *cols = 80; }
}

/* ---- playback clock ---- */
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double elapsed(void) {
    if (cur < 0) return 0;
    if (paused) return base_elapsed;
    return base_elapsed + (now_sec() - (t_start.tv_sec + t_start.tv_nsec / 1e9));
}

/* ---- child control ---- */
static void stop_child(void) {
    if (child > 0) {
        intentional = 1;
        if (paused) kill(child, SIGCONT);
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        child = -1;
    }
    paused = 0;
}

static void play_track(int idx, double startpos) {
    if (idx < 0 || idx >= ntracks) return;
    stop_child();
    cur = idx;
    base_elapsed = startpos > 0 ? startpos : 0;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    paused = 0;

    char ss[32] = "";
    if (startpos > 0) snprintf(ss, sizeof ss, "%.2f", startpos);
    char vol[16];
    snprintf(vol, sizeof vol, "%d", volume);

    pid_t pid = fork();
    if (pid == 0) {
        int n = open("/dev/null", O_WRONLY);
        if (n >= 0) { dup2(n, STDOUT_FILENO); dup2(n, STDERR_FILENO); }
        if (startpos > 0)
            execlp("ffplay", "ffplay", "-nodisp", "-autoexit",
                   "-loglevel", "quiet", "-volume", vol, "-ss", ss,
                   tracks[idx].path, (char *)NULL);
        else
            execlp("ffplay", "ffplay", "-nodisp", "-autoexit",
                   "-loglevel", "quiet", "-volume", vol,
                   tracks[idx].path, (char *)NULL);
        _exit(127);
    }
    child = pid;
    intentional = 0;
}

static void toggle_pause(void) {
    if (child <= 0) return;
    if (paused) {
        clock_gettime(CLOCK_MONOTONIC, &t_start);
        kill(child, SIGCONT);
        paused = 0;
    } else {
        base_elapsed = elapsed();
        kill(child, SIGSTOP);
        paused = 1;
    }
}

/* position of a track index within the play order (linear, fine for libraries) */
static int order_pos(int track) {
    for (int i = 0; i < ntracks; i++)
        if (order[i] == track) return i;
    return 0;
}

/* (re)build the play order; keep `cur` first so the next pick is fresh */
static void build_order(void) {
    for (int i = 0; i < ntracks; i++) order[i] = i;
    if (!shuffle) return;
    for (int i = ntracks - 1; i > 0; i--) {     /* Fisher-Yates */
        int j = rand() % (i + 1);
        int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
    }
    if (cur >= 0) {                              /* move current track to front */
        int p = order_pos(cur);
        int tmp = order[0]; order[0] = order[p]; order[p] = tmp;
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

/* rebuild the filtered view from the current query */
static void rebuild_view(void) {
    if (!view) view = malloc((ntracks ? ntracks : 1) * sizeof(int));
    nview = 0;
    for (int i = 0; i < ntracks; i++)
        if (ci_contains(tracks[i].name, query)) view[nview++] = i;
    if (sel >= nview) sel = nview ? nview - 1 : 0;
    if (sel < 0) sel = 0;
}

/* move the highlight to a given track index, if it's in the current view */
static void select_in_view(int track) {
    for (int i = 0; i < nview; i++)
        if (view[i] == track) { sel = i; return; }
}

static void next_track(void) {
    if (ntracks == 0) return;
    int pos = (cur >= 0) ? order_pos(cur) + 1 : 0;
    if (pos >= ntracks) { if (!repeat) { stop_child(); cur = -1; return; } pos = 0; }
    play_track(order[pos], 0);
    select_in_view(order[pos]);
}

static void prev_track(void) {
    if (ntracks == 0) return;
    if (elapsed() > 3) { play_track(cur, 0); return; }
    int pos = (cur >= 0) ? order_pos(cur) - 1 : 0;
    if (pos < 0) pos = repeat ? ntracks - 1 : 0;
    play_track(order[pos], 0);
    select_in_view(order[pos]);
}

static void seek(double delta) {
    if (cur < 0) return;
    double pos = elapsed() + delta;
    if (pos < 0) pos = 0;
    double dur = tracks[cur].dur;
    if (dur > 0 && pos > dur - 1) pos = dur - 1;
    play_track(cur, pos);
}

/* Change the volume of the live ffplay stream via PulseAudio (no restart, no
 * gap). Matches the sink-input by ffplay's PID. Returns 1 on success. */
static int pulse_set_volume(pid_t pid, int pct) {
    if (!have_pactl || pid <= 0) return 0;
    char cmd[640];
    snprintf(cmd, sizeof cmd,
        "idx=$(pactl list sink-inputs 2>/dev/null | "
        "awk '/Sink Input #/{i=substr($3,2)} "
        "/application.process.id/{gsub(/\"/,\"\");if($3==%d){print i;exit}}'); "
        "[ -n \"$idx\" ] && pactl set-sink-input-volume \"$idx\" %d%% >/dev/null 2>&1",
        (int)pid, pct);
    return system(cmd) == 0;
}

static void set_volume(int delta) {
    volume += delta;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (cur < 0 || child <= 0) return;
    /* Prefer a gapless live change; fall back to restarting only if needed. */
    if (!pulse_set_volume(child, volume) && !paused)
        play_track(cur, elapsed());
}

/* ---- rendering ---- */
static void fmt_time(double s, char *buf, size_t n) {
    if (s < 0) { snprintf(buf, n, "--:--"); return; }
    int t = (int)(s + 0.5);
    snprintf(buf, n, "%d:%02d", t / 60, t % 60);
}

static int scroll_top = 0;

static void draw(void) {
    int rows, cols;
    get_size(&rows, &cols);
    if (cols > 200) cols = 200;

    int header = 2;        /* title + blank */
    int footer = 5;        /* divider, nowtitle, bar, help, pad */
    int list_h = rows - header - footer;
    if (list_h < 1) list_h = 1;

    /* keep selection in view */
    if (sel < scroll_top) scroll_top = sel;
    if (sel >= scroll_top + list_h) scroll_top = sel - list_h + 1;
    if (scroll_top < 0) scroll_top = 0;

    char out[1 << 16];
    int o = 0;
    o += snprintf(out + o, sizeof out - o, "\033[H");   /* home */

    /* header: show filtered count when a query is active */
    if (query[0])
        o += snprintf(out + o, sizeof out - o,
            AQUA_B "  \xE2\x99\xAC AQUA" RESET GRAY "   %d/%d tracks" RESET "\033[K\r\n",
            nview, ntracks);
    else
        o += snprintf(out + o, sizeof out - o,
            AQUA_B "  \xE2\x99\xAC AQUA" RESET GRAY "   %d tracks" RESET "\033[K\r\n",
            ntracks);

    /* search bar (when typing) or filter indicator */
    if (searching)
        o += snprintf(out + o, sizeof out - o,
            "  " AQUA "/" WHITE "%s" AQUA "\xE2\x96\x88" RESET "\033[K\r\n", query);
    else if (query[0])
        o += snprintf(out + o, sizeof out - o,
            "  " DGRAY "filter: " AQUA "%s" DGRAY "  (esc to clear)" RESET "\033[K\r\n", query);
    else
        o += snprintf(out + o, sizeof out - o, "\033[K\r\n");

    /* list (over the filtered view) */
    for (int i = 0; i < list_h; i++) {
        int vidx = scroll_top + i;
        o += snprintf(out + o, sizeof out - o, "\033[K");
        if (vidx >= nview) {
            if (nview == 0 && i == 0)
                o += snprintf(out + o, sizeof out - o,
                    "  " DGRAY "%s" RESET "\r\n",
                    ntracks ? "No matches." : "No tracks.");
            else
                o += snprintf(out + o, sizeof out - o, "\r\n");
            continue;
        }
        int idx = view[vidx];
        Track *t = &tracks[idx];
        char dbuf[16];
        fmt_time(t->dur, dbuf, sizeof dbuf);

        const char *marker = (idx == cur) ? (paused ? "\xE2\x9D\x9A " : "\xE2\x96\xB6 ") : "  ";
        int name_w = cols - 12;
        if (name_w < 10) name_w = 10;

        if (vidx == sel) {
            o += snprintf(out + o, sizeof out - o,
                INVAQ " %s%-*.*s %5s " RESET "\r\n",
                marker, name_w, name_w, t->name, dbuf);
        } else {
            const char *col = (idx == cur) ? AQUA : WHITE;
            o += snprintf(out + o, sizeof out - o,
                " %s%s%-*.*s" RESET " " DGRAY "%5s" RESET "\r\n",
                col, marker, name_w, name_w, t->name, dbuf);
        }
    }

    /* footer */
    o += snprintf(out + o, sizeof out - o, DGRAY);
    for (int i = 0; i < cols && i < 200; i++) out[o++] = '-';
    o += snprintf(out + o, sizeof out - o, RESET "\033[K\r\n");

    if (cur >= 0) {
        double el = elapsed(), dur = tracks[cur].dur;
        char e[16], d[16];
        fmt_time(el, e, sizeof e);
        fmt_time(dur, d, sizeof d);

        const char *state = paused ? GRAY "PAUSED" : AQUA "PLAYING";
        o += snprintf(out + o, sizeof out - o,
            "  %s" RESET " " WHITE "%-*.*s" RESET "\033[K\r\n",
            state, cols - 14, cols - 14, tracks[cur].name);

        /* progress bar */
        int bar_w = cols - 20;
        if (bar_w < 4) bar_w = 4;
        int fill = (dur > 0) ? (int)(bar_w * (el / dur)) : 0;
        if (fill > bar_w) fill = bar_w;
        if (fill < 0) fill = 0;
        o += snprintf(out + o, sizeof out - o, "  " GRAY "%5s " AQUA, e);
        for (int i = 0; i < fill; i++)      o += snprintf(out + o, sizeof out - o, "\xE2\x96\x88");
        o += snprintf(out + o, sizeof out - o, DGRAY);
        for (int i = fill; i < bar_w; i++)  o += snprintf(out + o, sizeof out - o, "\xE2\x94\x80");
        o += snprintf(out + o, sizeof out - o, " " GRAY "%5s" RESET "\033[K\r\n", d);
    } else {
        o += snprintf(out + o, sizeof out - o, "  " GRAY "Nothing playing" RESET "\033[K\r\n");
        o += snprintf(out + o, sizeof out - o, "\033[K\r\n");
    }

    /* help line */
    if (searching)
        o += snprintf(out + o, sizeof out - o,
            "  " DGRAY "type to filter   " DGRAY "\xE2\x86\xB5" GRAY " keep  "
            DGRAY "esc" GRAY " clear  " DGRAY "\xE2\x86\x91\xE2\x86\x93" GRAY " move" RESET "\033[K");
    else
        o += snprintf(out + o, sizeof out - o,
            "  " DGRAY "\xE2\x86\x91\xE2\x86\x93" GRAY " move  " DGRAY "\xE2\x86\xB5/space" GRAY " play/pause  "
            DGRAY "n/b" GRAY " next/prev  " DGRAY "\xE2\x86\x90\xE2\x86\x92" GRAY " seek  "
            DGRAY "-/+" GRAY " vol %d%%  " DGRAY "/" GRAY " search  " DGRAY "s" GRAY " shuffle%s  "
            DGRAY "r" GRAY " repeat%s  " DGRAY "q" GRAY " quit" RESET "\033[K",
            volume,
            shuffle ? AQUA " on" GRAY : "",
            repeat  ? AQUA " on" GRAY : "");

    o += snprintf(out + o, sizeof out - o, "\033[J");   /* clear below */

    if (write(STDOUT_FILENO, out, o) < 0) { /* terminal closed */ }
}

/* ---- input ---- */
static int read_key(void) {
    static int pending = -1;            /* one-byte pushback */
    if (pending >= 0) { int p = pending; pending = -1; return p; }

    unsigned char c;
    int n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return -1;
    if (c == '\033') {
        unsigned char s0;
        if (read(STDIN_FILENO, &s0, 1) != 1) return '\033';   /* lone Esc */
        if (s0 != '[') { pending = s0; return '\033'; }       /* Esc + other key */
        unsigned char s1;
        if (read(STDIN_FILENO, &s1, 1) != 1) return '\033';
        switch (s1) {
            case 'A': return 1000; /* up */
            case 'B': return 1001; /* down */
            case 'C': return 1002; /* right */
            case 'D': return 1003; /* left */
        }
        return '\033';
    }
    return c;
}

static volatile sig_atomic_t resized = 0;
static void on_winch(int s) { (void)s; resized = 1; }

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";

    scan_dir(dir, 6);
    if (ntracks == 0) {
        fprintf(stderr, "No audio files found in '%s'.\n", dir);
        return 1;
    }
    qsort(tracks, ntracks, sizeof(Track), cmp_track);

    /* resolve canonical paths + mtimes, then prefill durations from cache */
    for (int i = 0; i < ntracks; i++) {
        char rp[PATH_MAX];
        tracks[i].abspath = xstrdup(realpath(tracks[i].path, rp) ? rp : tracks[i].path);
        struct stat st;
        if (stat(tracks[i].path, &st) == 0) tracks[i].mtime = (long)st.st_mtime;
    }
    init_cache_path();
    load_cache();
    for (int i = 0; i < ntracks; i++) {
        double d = cache_lookup(tracks[i].abspath, tracks[i].mtime);
        if (d > 0) tracks[i].dur = d;
    }

    srand((unsigned)time(NULL));
    order = malloc(ntracks * sizeof(int));
    if (!order) { perror("malloc"); return 1; }
    build_order();
    rebuild_view();   /* initial view = all tracks */

    have_pactl = (system("command -v pactl >/dev/null 2>&1") == 0);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGWINCH, on_winch);

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
    while (running) {
        /* reap a naturally-finished track -> advance */
        if (child > 0) {
            int st;
            pid_t r = waitpid(child, &st, WNOHANG);
            if (r == child) {
                child = -1;
                if (!intentional) next_track();
            }
        }

        if (resized) { resized = 0; printf("\033[2J"); fflush(stdout); }

        int probing = probe_idx < ntracks;

        /* While probing, don't block in select — keep input snappy but make
         * steady progress on durations. Once done, idle at a 250ms refresh. */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = { 0, probing ? 0 : 250000 };
        int rv = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);

        if (rv > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
            int k = read_key();
            if (searching) {
                /* ---- search input mode ---- */
                switch (k) {
                    case 1000: if (sel > 0) sel--; break;            /* up */
                    case 1001: if (sel < nview - 1) sel++; break;    /* down */
                    case '\r': case '\n':
                        searching = 0;                               /* keep filter */
                        if (nview > 0) play_track(view[sel], 0);
                        break;
                    case '\033':                                     /* esc: clear */
                        searching = 0; qlen = 0; query[0] = '\0'; rebuild_view();
                        break;
                    case 127: case 8:                                /* backspace */
                        if (qlen > 0) { query[--qlen] = '\0'; rebuild_view(); }
                        break;
                    default:
                        if (k >= 32 && k < 127 && qlen < (int)sizeof(query) - 1) {
                            query[qlen++] = (char)k; query[qlen] = '\0';
                            rebuild_view();
                        }
                        break;
                }
            } else {
                /* ---- normal mode ---- */
                switch (k) {
                    case 1000: case 'k': if (sel > 0) sel--; break;
                    case 1001: case 'j': if (sel < nview - 1) sel++; break;
                    case '\r': case '\n': if (nview > 0) play_track(view[sel], 0); break;
                    case ' ': case 'p':
                        if (cur < 0) { if (nview > 0) play_track(view[sel], 0); }
                        else toggle_pause();
                        break;
                    case 'n': next_track(); break;
                    case 'b': prev_track(); break;
                    case 1002: seek(5); break;    /* right */
                    case 1003: seek(-5); break;   /* left */
                    case '-': case '_': set_volume(-5); break;
                    case '+': case '=': set_volume(5); break;
                    case 'r': repeat = !repeat; break;
                    case 's': shuffle = !shuffle; build_order(); break;
                    case '/': searching = 1; break;
                    case '\033':                  /* esc clears an active filter */
                        if (query[0]) { qlen = 0; query[0] = '\0'; rebuild_view(); }
                        break;
                    case 'g': sel = 0; break;
                    case 'G': sel = nview ? nview - 1 : 0; break;
                    case 'q': case 3: running = 0; break;
                }
            }
            draw();
            last_draw = now_sec();
            continue;   /* respond to the key immediately */
        }

        /* probe a small batch of *uncached* durations this tick. Cached tracks
         * already have dur set, so they're skipped instantly. */
        if (probing) {
            int probed = 0;
            while (probed < 4 && probe_idx < ntracks) {
                if (tracks[probe_idx].dur < 0) {
                    tracks[probe_idx].dur = probe_duration(tracks[probe_idx].path);
                    probed_new += (tracks[probe_idx].dur > 0);
                    probed++;
                }
                probe_idx++;
            }
            if (probe_idx >= ntracks && probed_new) { save_cache(); probed_new = 0; }
        }

        /* throttle redraws to ~10/sec so probing/progress stay smooth */
        double t = now_sec();
        if (t - last_draw >= 0.1) { draw(); last_draw = t; }
    }

    stop_child();
    save_cache();          /* persist anything probed this session */
    term_restore();
    return 0;
}
