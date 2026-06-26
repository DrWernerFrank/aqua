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
    char *name;     /* filename without extension (fallback display) */
    char *abspath;  /* canonical path, used as cache key */
    char *folder;   /* parent folder name (album fallback) */
    long  mtime;    /* file modification time, for cache validation */
    double dur;     /* seconds, -1 if unknown */
    /* metadata from tags (NULL when absent) */
    char *title;
    char *artist;
    char *album;
    char *group;    /* album if tagged, else folder; used for grouping/sort */
    int   trackno;  /* track number, 0 if unknown */
} Track;

/* display title: prefer the tag title, fall back to the filename */
static const char *disp_title(const Track *t) {
    return (t->title && *t->title) ? t->title : t->name;
}

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

/* play queue: the sequence next/prev walk through. It mirrors the current
 * view[] (so search results become the queue), optionally shuffled. */
static int *queue = NULL;
static int nqueue = 0;

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

    /* parent folder name (album fallback) */
    char pbuf[2048];
    snprintf(pbuf, sizeof pbuf, "%s", path);
    char *slash = strrchr(pbuf, '/');           /* strip filename */
    if (slash) {
        *slash = '\0';
        char *par = strrchr(pbuf, '/');          /* parent dir basename */
        t->folder = xstrdup(par ? par + 1 : pbuf);
    } else {
        t->folder = xstrdup("");
    }

    t->abspath = NULL;
    t->mtime = 0;
    t->dur = -1;
    t->title = t->artist = t->album = NULL;
    t->group = t->folder;        /* refined to album once tags are read */
    t->trackno = 0;
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

/* group = album when tagged, otherwise the parent folder name */
static void apply_group(Track *t) {
    t->group = (t->album && *t->album) ? t->album : t->folder;
}

/* dup a string with tabs/newlines flattened to spaces, or NULL if empty */
static char *dup_clean(const char *s) {
    if (!s || !*s) return NULL;
    char *p = xstrdup(s);
    for (char *q = p; *q; q++) if (*q == '\t' || *q == '\n' || *q == '\r') *q = ' ';
    return p;
}

/* ---- metadata via ffprobe (duration + title/artist/album/track) ---- */
static void probe_meta(Track *t) {
    char cmd[2600];
    snprintf(cmd, sizeof cmd,
        "ffprobe -v error -show_entries "
        "format=duration:format_tags=title,artist,album,track,TITLE,ARTIST,ALBUM,TRACK "
        "-of default=noprint_wrappers=1 \"%s\" 2>/dev/null", t->path);
    FILE *p = popen(cmd, "r");
    if (!p) { t->dur = -1; return; }
    char line[1024];
    while (fgets(line, sizeof line, p)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;
        size_t L = strlen(val);
        while (L && (val[L-1] == '\n' || val[L-1] == '\r')) val[--L] = '\0';
        if (strcmp(key, "duration") == 0)            t->dur = atof(val) > 0 ? atof(val) : -1;
        else if (strcasecmp(key, "TAG:title") == 0)  { free(t->title);  t->title  = dup_clean(val); }
        else if (strcasecmp(key, "TAG:artist") == 0) { free(t->artist); t->artist = dup_clean(val); }
        else if (strcasecmp(key, "TAG:album") == 0)  { free(t->album);  t->album  = dup_clean(val); }
        else if (strcasecmp(key, "TAG:track") == 0)  t->trackno = atoi(val);  /* "3/12" -> 3 */
    }
    pclose(p);
    apply_group(t);
}

/* ---- metadata cache (~/.cache/aqua/library.tsv) ----
 * Each line: <mtime>\t<dur>\t<trackno>\t<title>\t<artist>\t<album>\t<abspath>
 * Only new or modified files are probed; everything else is read from here. */
typedef struct {
    char *path; long mtime; double dur; int trackno;
    char *title, *artist, *album;
} CacheEnt;
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
    snprintf(cache_file, sizeof cache_file, "%.*s/library.tsv",
             (int)(sizeof cache_file - 20), dir);
}

/* take the next tab-delimited field, advancing *pp past the tab */
static char *next_field(char **pp) {
    char *s = *pp;
    char *tab = strchr(s, '\t');
    if (tab) { *tab = '\0'; *pp = tab + 1; }
    else     { *pp = s + strlen(s); }
    return s;
}

static void load_cache(void) {
    FILE *f = fopen(cache_file, "r");
    if (!f) return;
    char line[PATH_MAX + 256];
    while (fgets(line, sizeof line, f)) {
        size_t L = strlen(line);
        if (L && line[L-1] == '\n') line[--L] = '\0';
        char *p = line;
        long  mt   = strtol(next_field(&p), NULL, 10);
        double dur = strtod(next_field(&p), NULL);
        int   trk  = atoi(next_field(&p));
        char *title  = next_field(&p);
        char *artist = next_field(&p);
        char *album  = next_field(&p);
        char *path   = p;                 /* remainder is the path */
        if (!*path) continue;
        if (ncache == capcache) {
            capcache = capcache ? capcache * 2 : 256;
            cache = realloc(cache, capcache * sizeof(CacheEnt));
            if (!cache) { perror("realloc"); exit(1); }
        }
        CacheEnt *e = &cache[ncache++];
        e->path = xstrdup(path);
        e->mtime = mt; e->dur = dur; e->trackno = trk;
        e->title  = *title  ? xstrdup(title)  : NULL;
        e->artist = *artist ? xstrdup(artist) : NULL;
        e->album  = *album  ? xstrdup(album)  : NULL;
    }
    fclose(f);
    qsort(cache, ncache, sizeof(CacheEnt), cmp_cache);
}

static CacheEnt *cache_lookup(const char *path, long mtime) {
    if (ncache == 0) return NULL;
    CacheEnt key; key.path = (char *)path;
    CacheEnt *e = bsearch(&key, cache, ncache, sizeof(CacheEnt), cmp_cache);
    return (e && e->mtime == mtime) ? e : NULL;
}

/* insert or update an entry from a track (linear; runs once at save time) */
static void cache_upsert(const Track *t) {
    CacheEnt *e = NULL;
    for (int i = 0; i < ncache; i++)
        if (strcmp(cache[i].path, t->abspath) == 0) { e = &cache[i]; break; }
    if (!e) {
        if (ncache == capcache) {
            capcache = capcache ? capcache * 2 : 256;
            cache = realloc(cache, capcache * sizeof(CacheEnt));
            if (!cache) { perror("realloc"); exit(1); }
        }
        e = &cache[ncache++];
        e->path = xstrdup(t->abspath);
    }
    e->mtime = t->mtime; e->dur = t->dur; e->trackno = t->trackno;
    e->title  = t->title;   /* shares the track's strings; only used until save */
    e->artist = t->artist;
    e->album  = t->album;
}

static void save_cache(void) {
    if (!cache_file[0]) return;
    for (int i = 0; i < ntracks; i++)
        if (tracks[i].dur > 0 && tracks[i].abspath)
            cache_upsert(&tracks[i]);
    FILE *f = fopen(cache_file, "w");
    if (!f) return;
    for (int i = 0; i < ncache; i++) {
        CacheEnt *e = &cache[i];
        fprintf(f, "%ld\t%.3f\t%d\t%s\t%s\t%s\t%s\n",
                e->mtime, e->dur, e->trackno,
                e->title ? e->title : "", e->artist ? e->artist : "",
                e->album ? e->album : "", e->path);
    }
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
    return ci_contains(t->name, q)
        || (t->title  && ci_contains(t->title, q))
        || (t->artist && ci_contains(t->artist, q))
        || (t->album  && ci_contains(t->album, q));
}

/* order the view by album group, then track number, then title */
static int cmp_view(const void *a, const void *b) {
    const Track *ta = &tracks[*(const int *)a];
    const Track *tb = &tracks[*(const int *)b];
    int c = strcasecmp(ta->group, tb->group);
    if (c) return c;
    if (ta->trackno != tb->trackno) {        /* numbered tracks first, in order */
        if (ta->trackno == 0) return 1;
        if (tb->trackno == 0) return -1;
        return ta->trackno - tb->trackno;
    }
    return strcasecmp(disp_title(ta), disp_title(tb));
}

/* rebuild the filtered, album-grouped view from the current query */
static void rebuild_view(void) {
    if (!view) view = malloc((ntracks ? ntracks : 1) * sizeof(int));
    nview = 0;
    for (int i = 0; i < ntracks; i++)
        if (track_matches(&tracks[i], query)) view[nview++] = i;
    qsort(view, nview, sizeof(int), cmp_view);
    if (sel >= nview) sel = nview ? nview - 1 : 0;
    if (sel < 0) sel = 0;
    build_queue();   /* the filtered list is the new play queue */
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
    if (pos >= nqueue) { if (!repeat) { stop_child(); cur = -1; return; } pos = 0; }
    play_track(queue[pos], 0);
    select_in_view(queue[pos]);
}

static void prev_track(void) {
    if (nqueue == 0) return;
    if (elapsed() > 3) { play_track(cur, 0); return; }
    int p = queue_pos(cur);
    int pos = (p > 0) ? p - 1 : (repeat ? nqueue - 1 : 0);
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

/* render rows: a track row or an album header row. Built fresh each draw. */
static int *rr_head = NULL;   /* 1 = header row */
static int *rr_val  = NULL;   /* header: track idx (for group); track: view pos */
static int  n_rr = 0;

static void build_rows(void) {
    if (!rr_head) {
        rr_head = malloc(2 * (ntracks ? ntracks : 1) * sizeof(int));
        rr_val  = malloc(2 * (ntracks ? ntracks : 1) * sizeof(int));
    }
    n_rr = 0;
    const char *prev = NULL;
    for (int pos = 0; pos < nview; pos++) {
        int ti = view[pos];
        const char *g = tracks[ti].group;
        if (!prev || strcasecmp(g, prev) != 0) {
            rr_head[n_rr] = 1; rr_val[n_rr] = ti; n_rr++;
            prev = g;
        }
        rr_head[n_rr] = 0; rr_val[n_rr] = pos; n_rr++;
    }
}

static void draw(void) {
    int rows, cols;
    get_size(&rows, &cols);
    if (cols > 200) cols = 200;

    int header = 2;        /* title + blank */
    int footer = 5;        /* divider, nowtitle, bar, help, pad */
    int list_h = rows - header - footer;
    if (list_h < 1) list_h = 1;

    build_rows();

    /* find the render row holding the current selection */
    int selrow = 0;
    for (int r = 0; r < n_rr; r++)
        if (!rr_head[r] && rr_val[r] == sel) { selrow = r; break; }

    /* keep selection (and its header just above) on screen */
    if (selrow - 1 < scroll_top) scroll_top = selrow - 1;
    if (selrow >= scroll_top + list_h) scroll_top = selrow - list_h + 1;
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

    /* list (render rows: album headers + tracks) */
    for (int i = 0; i < list_h; i++) {
        int r = scroll_top + i;
        o += snprintf(out + o, sizeof out - o, "\033[K");
        if (r >= n_rr) {
            if (n_rr == 0 && i == 0)
                o += snprintf(out + o, sizeof out - o,
                    "  " DGRAY "%s" RESET "\r\n",
                    ntracks ? "No matches." : "No tracks.");
            else
                o += snprintf(out + o, sizeof out - o, "\r\n");
            continue;
        }

        if (rr_head[r]) {                       /* album / folder header */
            const char *g = tracks[rr_val[r]].group;
            int gw = cols - 4;
            o += snprintf(out + o, sizeof out - o,
                " " AQUA_B "\xE2\x96\x8C %-*.*s" RESET "\r\n", gw, gw, *g ? g : "(unknown)");
            continue;
        }

        int vidx = rr_val[r];
        int idx = view[vidx];
        Track *t = &tracks[idx];
        char dbuf[16];
        fmt_time(t->dur, dbuf, sizeof dbuf);

        char num[16];
        if (t->trackno > 0) snprintf(num, sizeof num, "%2d", t->trackno);
        else                snprintf(num, sizeof num, "  ");

        const char *marker = (idx == cur) ? (paused ? "\xE2\x9D\x9A " : "\xE2\x96\xB6 ") : "  ";
        int name_w = cols - 16;
        if (name_w < 8) name_w = 8;
        const char *title = disp_title(t);

        if (vidx == sel) {
            o += snprintf(out + o, sizeof out - o,
                INVAQ " %s%s %-*.*s %5s " RESET "\r\n",
                marker, num, name_w, name_w, title, dbuf);
        } else {
            const char *col = (idx == cur) ? AQUA : WHITE;
            o += snprintf(out + o, sizeof out - o,
                " %s%s" DGRAY "%s " RESET "%s%-*.*s" RESET " " DGRAY "%5s" RESET "\r\n",
                col, marker, num, col, name_w, name_w, title, dbuf);
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
        /* "Title — Artist" when an artist tag exists */
        char now[1024];
        if (tracks[cur].artist && *tracks[cur].artist)
            snprintf(now, sizeof now, "%s \xE2\x80\x94 %s",
                     disp_title(&tracks[cur]), tracks[cur].artist);
        else
            snprintf(now, sizeof now, "%s", disp_title(&tracks[cur]));
        o += snprintf(out + o, sizeof out - o,
            "  %s" RESET " " WHITE "%-*.*s" RESET "\033[K\r\n",
            state, cols - 14, cols - 14, now);

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
        CacheEnt *e = cache_lookup(tracks[i].abspath, tracks[i].mtime);
        if (e && e->dur > 0) {
            tracks[i].dur = e->dur;
            tracks[i].trackno = e->trackno;
            tracks[i].title  = e->title  ? xstrdup(e->title)  : NULL;
            tracks[i].artist = e->artist ? xstrdup(e->artist) : NULL;
            tracks[i].album  = e->album  ? xstrdup(e->album)  : NULL;
            apply_group(&tracks[i]);
        }
    }

    srand((unsigned)time(NULL));
    queue = malloc(ntracks * sizeof(int));
    if (!queue) { perror("malloc"); return 1; }
    rebuild_view();   /* initial view = all tracks; also builds the queue */

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
                    case 's': shuffle = !shuffle; build_queue(); break;
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

        /* probe a small batch of *uncached* tracks this tick. Cached tracks
         * already have metadata, so they're skipped instantly. */
        if (probing) {
            int probed = 0;
            while (probed < 4 && probe_idx < ntracks) {
                if (tracks[probe_idx].dur < 0) {
                    probe_meta(&tracks[probe_idx]);
                    probed_new += (tracks[probe_idx].dur > 0);
                    probed++;
                }
                probe_idx++;
            }
            /* tags just loaded may change grouping/sort: refresh the view,
             * keeping the highlight on the same track. */
            if (probed > 0) {
                int seltrack = (sel < nview) ? view[sel] : -1;
                rebuild_view();
                if (seltrack >= 0) select_in_view(seltrack);
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
