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
    int   probed;   /* 1 once ffprobe answered (even "no duration"), or cache hit */
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

/* tabs: a flat Tracks list, plus Albums and Artists browsers */
enum { TAB_TRACKS, TAB_ALBUMS, TAB_ARTISTS, TAB_COUNT };
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

/* are we browsing a group list (albums/artists) vs a track list? */
static int on_album_list(void) {
    return (tab == TAB_ALBUMS || tab == TAB_ARTISTS) && !in_album;
}

/* the string a track is grouped by in the current browse tab */
static const char *browse_key(const Track *t) {
    if (tab == TAB_ARTISTS) return (t->artist && *t->artist) ? t->artist : "(unknown)";
    return t->group;   /* albums (album tag, else folder) */
}

/* label for the active browse tab */
static const char *browse_label(void) {
    return tab == TAB_ARTISTS ? "Artists" : "Albums";
}
static pid_t child = -1;     /* ffplay pid, -1 = none */
static int paused = 0;
static int intentional = 0;  /* we killed ffplay, don't auto-advance */
static int volume = 100;     /* the single source of truth for output level */
enum { REP_OFF, REP_ALL, REP_ONE };
static int repeat = REP_OFF; /* off / repeat-all (loop queue) / repeat-one (track) */
static int shuffle = 0;      /* random play order */

/* Volume backends, tried in this order. `volume` is the authority; the chosen
 * backend applies it live. ffplay itself carries the volume only when no
 * backend exists (VOL_NONE), where a debounced restart is the only way to
 * change a running stream. */
enum { VOL_PACTL, VOL_WPCTL, VOL_AMIXER, VOL_NONE };
static int vol_backend = VOL_NONE;
static int vol_dirty = 0;        /* pactl: the running stream lacks `volume` */
static int vol_fails = 0;        /* consecutive backend failures -> degrade */
static int spawned_vol = 100;    /* -volume the running ffplay started with */
static double restart_at = 0;   /* debounced volume restart, 0 = none */

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

/* ---- metadata via ffprobe (duration + title/artist/album/track) ----
 * ffprobe is exec'd directly with the path as a single argv element — there
 * is no shell between us and it, so filenames can't inject commands. Returns
 * 1 when the probe is definitive (ffprobe ran, even if it found no duration),
 * 0 when ffprobe itself couldn't run — the file is then retried next session. */
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
        while (fgets(line, sizeof line, p)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            char *key = line, *val = eq + 1;
            size_t L = strlen(val);
            while (L && (val[L-1] == '\n' || val[L-1] == '\r')) val[--L] = '\0';
            if (strcmp(key, "duration") == 0)             t->dur = atof(val) > 0 ? atof(val) : -1;
            else if (strcasecmp(key, "TAG:title") == 0)  { free(t->title);  t->title  = dup_clean(val); }
            else if (strcasecmp(key, "TAG:artist") == 0) { free(t->artist); t->artist = dup_clean(val); }
            else if (strcasecmp(key, "TAG:album") == 0)  { free(t->album);  t->album  = dup_clean(val); }
            else if (strcasecmp(key, "TAG:track") == 0)  t->trackno = atoi(val);  /* "3/12" -> 3 */
        }
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

/* ---- metadata cache (~/.cache/aqua/library.tsv) ----
 * Each line: <mtime>\t<dur>\t<trackno>\t<title>\t<artist>\t<album>\t<abspath>
 * Only new or modified files are probed; everything else is read from here.
 * Files ffprobe couldn't get a duration from are stored with dur -1 so they
 * aren't re-probed on every launch either. Text fields are escaped with
 * put_escaped(), so newlines/tabs/backslashes can never corrupt a line. */
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

/* inverse of put_escaped; unknown escapes are kept byte-for-byte */
static char *dup_unescaped(const char *s) {
    char *out = xmalloc(strlen(s) + 1);
    char *w = out;
    for (const char *p = s; *p; ) {
        if (p[0] == '\\' && p[1] == 'n')  { *w++ = '\n'; p += 2; }
        else if (p[0] == '\\' && p[1] == 't')  { *w++ = '\t'; p += 2; }
        else if (p[0] == '\\' && p[1] == 'r')  { *w++ = '\r'; p += 2; }
        else if (p[0] == '\\' && p[1] == '\\') { *w++ = '\\'; p += 2; }
        else *w++ = *p++;
    }
    *w = '\0';
    return out;
}

static void load_cache(void) {
    FILE *f = fopen(cache_file, "r");
    if (!f) return;
    char line[PATH_MAX * 2 + 512];   /* escaped fields can grow past the path */
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
        e->path = dup_unescaped(path);
        e->mtime = mt; e->dur = dur; e->trackno = trk;
        e->title  = *title  ? dup_unescaped(title)  : NULL;
        e->artist = *artist ? dup_unescaped(artist) : NULL;
        e->album  = *album  ? dup_unescaped(album)  : NULL;
    }
    fclose(f);
    if (ncache) qsort(cache, ncache, sizeof(CacheEnt), cmp_cache);
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
        e->title = e->artist = e->album = NULL;
    } else {
        free(e->title); free(e->artist); free(e->album);   /* cache owns its strings */
    }
    e->mtime = t->mtime; e->dur = t->dur; e->trackno = t->trackno;
    e->title  = t->title  ? xstrdup(t->title)  : NULL;
    e->artist = t->artist ? xstrdup(t->artist) : NULL;
    e->album  = t->album  ? xstrdup(t->album)  : NULL;
}

static void save_cache(void) {
    if (!cache_file[0]) return;
    for (int i = 0; i < ntracks; i++)
        if (tracks[i].probed && tracks[i].abspath)
            cache_upsert(&tracks[i]);
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
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);   /* ISIG stays on: ^C arrives as a signal */
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

    /* ffplay carries the volume itself only when no live backend can adjust
     * a running stream; otherwise keep software volume neutral and let the
     * backend own the level (so it is never applied twice). */
    int vol = (vol_backend == VOL_NONE) ? volume : 100;
    spawned_vol = vol;
    char ss[32] = "";
    if (startpos > 0) snprintf(ss, sizeof ss, "%.2f", startpos);
    char vols[16];
    snprintf(vols, sizeof vols, "%d", vol);

    pid_t pid = fork();
    if (pid == 0) {
        int n = open("/dev/null", O_WRONLY);
        if (n >= 0) { dup2(n, STDOUT_FILENO); dup2(n, STDERR_FILENO); }
        if (startpos > 0)
            execlp("ffplay", "ffplay", "-nodisp", "-autoexit",
                   "-loglevel", "quiet", "-volume", vols, "-ss", ss,
                   tracks[idx].path, (char *)NULL);
        else
            execlp("ffplay", "ffplay", "-nodisp", "-autoexit",
                   "-loglevel", "quiet", "-volume", vols,
                   tracks[idx].path, (char *)NULL);
        _exit(127);
    }
    child = pid;
    intentional = 0;
    restart_at = 0;                    /* the new stream already carries it */
    vol_dirty = (vol_backend == VOL_PACTL && volume != 100);
}

static void toggle_pause(void) {
    if (child <= 0) return;
    if (paused) {
        /* if the volume changed while paused and only a respawn can apply it,
         * resume from the paused position with the new volume */
        if (vol_backend == VOL_NONE && volume != spawned_vol) {
            play_track(cur, base_elapsed);
            return;
        }
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

/* compare two tracks by (track number, then title) */
static int cmp_by_trackno(const Track *ta, const Track *tb) {
    if (ta->trackno != tb->trackno) {
        if (ta->trackno == 0) return 1;
        if (tb->trackno == 0) return -1;
        return ta->trackno - tb->trackno;
    }
    return strcasecmp(disp_title(ta), disp_title(tb));
}

/* order the view depending on context:
 *   0 = by title (Tracks tab)
 *   1 = by track number (opened album)
 *   2 = by album, then track number (opened artist) */
static int cmp_view(const void *a, const void *b) {
    const Track *ta = &tracks[*(const int *)a];
    const Track *tb = &tracks[*(const int *)b];
    if (view_sort == 1) return cmp_by_trackno(ta, tb);
    if (view_sort == 2) {
        int c = strcasecmp(ta->group, tb->group);
        if (c) return c;
        return cmp_by_trackno(ta, tb);
    }
    int c = strcasecmp(disp_title(ta), disp_title(tb));   /* Tracks tab: by title */
    if (c) return c;
    return strcasecmp(ta->name, tb->name);
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
    view_sort = !in_album ? 0 : (tab == TAB_ARTISTS ? 2 : 1);
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
    tab = t; in_album = 0;
    free(cur_album); cur_album = NULL;
    searching = 0; clear_query();
    if (tab == TAB_TRACKS) rebuild_view();
    else { build_albums(); rebuild_albums_view(); alsel = 0; }
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

/* pick the first backend that actually answers — presence alone isn't enough */
static void detect_vol_backend(void) {
    if (system("pactl info >/dev/null 2>&1") == 0)              vol_backend = VOL_PACTL;
    else if (system("wpctl status >/dev/null 2>&1") == 0)       vol_backend = VOL_WPCTL;
    else if (system("amixer sget Master >/dev/null 2>&1") == 0) vol_backend = VOL_AMIXER;
    else                                                        vol_backend = VOL_NONE;
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

/* write a whole frame, retrying past partial writes and EINTR */
static void write_all(const char *buf, int n) {
    size_t left = (size_t)n;
    while (left > 0) {
        ssize_t w = write(STDOUT_FILENO, buf, left);
        if (w > 0) { buf += w; left -= (size_t)w; }
        else if (w < 0 && errno == EINTR) continue;
        else break;                          /* terminal gone: drop the frame */
    }
}

static void fmt_time(double s, char *buf, size_t n) {
    if (s < 0) { snprintf(buf, n, "--:--"); return; }
    int t = (int)(s + 0.5);
    snprintf(buf, n, "%d:%02d", t / 60, t % 60);
}

static int scroll_top = 0;
static int page_size = 10;      /* visible rows; draw() keeps it current */
#define MAX_LIST_ROWS 400       /* cap so one frame always fits in out[] */

/* ---- flicker-free rendering ----
 * A shadow copy of the last painted frame sits under the screen: draw()
 * emits only the lines that actually changed, each positioned absolutely,
 * inside synchronized-update brackets, so the terminal never shows a
 * partial frame. An unchanged screen emits nothing at all — the display
 * sits still instead of repainting (and flashing) at 10 Hz. Resizes
 * repaint in place; \033[2J (blank, then redraw = a hard flash) happens
 * exactly once, at startup. */
#define MAX_LINE 1024                       /* one rendered screen line */
#define MAX_FRAME_LINES (MAX_LIST_ROWS + 8)
static char shadow[MAX_FRAME_LINES][MAX_LINE];
static int  shadow_count = 0;               /* rows valid in the shadow */

/* overwrite one screen line unless it already shows exactly this */
static void flush_line(char *out, size_t cap, int *o, int row, const char *line) {
    if (row >= MAX_FRAME_LINES) return;
    if (row < shadow_count && strcmp(shadow[row], line) == 0) return;
    *o = emit(out, cap, *o, "\033[%d;1H%s\033[K", row + 1, line);
    strcpy(shadow[row], line);
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
    int asel   = album_list ? alsel   : sel;     /* active selection */
    int acount = album_list ? nalview : nview;   /* active list length */

    /* keep the active selection on screen */
    if (asel < scroll_top) scroll_top = asel;
    if (asel >= scroll_top + list_h) scroll_top = asel - list_h + 1;
    if (scroll_top < 0) scroll_top = 0;

    /* a resize changes geometry: repaint everything in place, no blanking */
    static int last_rows = -1, last_cols = -1;
    int full = (rows != last_rows || cols != last_cols);
    if (full) { shadow_count = 0; last_rows = rows; last_cols = cols; }

    static char out[1 << 18];               /* 256 KiB frame buffer */
    char line[MAX_LINE];
    int o = 0, lo = 0, row = 0;

    /* ---- tab bar ---- */
    const char *t_tracks  = (tab == TAB_TRACKS)  ? INVAQ " Tracks "  RESET : DGRAY " Tracks "  RESET;
    const char *t_albums  = (tab == TAB_ALBUMS)  ? INVAQ " Albums "  RESET : DGRAY " Albums "  RESET;
    const char *t_artists = (tab == TAB_ARTISTS) ? INVAQ " Artists " RESET : DGRAY " Artists " RESET;
    lo = emit(line, sizeof line, 0,
        AQUA_B "  \xE2\x99\xAC AQUA  " RESET "%s%s%s", t_tracks, t_albums, t_artists);
    flush_line(out, sizeof out, &o, row++, line);

    /* ---- breadcrumb / search line ---- */
    if (searching)
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
            nalbums, tab == TAB_ARTISTS ? "artists" : "albums");
    else
        lo = emit(line, sizeof line, 0,
            "  " DGRAY "%d tracks" RESET, ntracks);
    flush_line(out, sizeof out, &o, row++, line);

    /* ---- list body ---- */
    for (int i = 0; i < list_h; i++) {
        int r = scroll_top + i;
        if (r >= acount) {
            if (acount == 0 && i == 0)
                lo = emit(line, sizeof line, 0,
                    "  " DGRAY "%s" RESET,
                    query[0] ? "No matches." :
                    (album_list ? (tab == TAB_ARTISTS ? "No artists." : "No albums.")
                                : "No tracks."));
            else {
                lo = 0;
                line[0] = '\0';
            }
            flush_line(out, sizeof out, &o, row++, line);
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
            flush_line(out, sizeof out, &o, row++, line);
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
        const char *title = disp_title(t);

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
        flush_line(out, sizeof out, &o, row++, line);
    }

    /* footer */
    lo = emit(line, sizeof line, 0, DGRAY);
    for (int i = 0; i < cols && i < 200 && lo < (int)sizeof line - 2; i++) line[lo++] = '-';
    emit(line, sizeof line, lo, RESET);
    flush_line(out, sizeof out, &o, row++, line);

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
        lo = emit(line, sizeof line, 0,
            "  %s" RESET " " WHITE "%-*.*s" RESET,
            state, cols - 14, cols - 14, now);
        flush_line(out, sizeof out, &o, row++, line);

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
        flush_line(out, sizeof out, &o, row++, line);
    } else {
        emit(line, sizeof line, 0, "  " GRAY "Nothing playing" RESET);
        flush_line(out, sizeof out, &o, row++, line);
        line[0] = '\0';
        flush_line(out, sizeof out, &o, row++, line);
    }

    /* help line */
    if (searching)
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
    flush_line(out, sizeof out, &o, row++, line);

    /* wipe leftovers below a shortened frame, then publish the delta */
    if (full && row < rows)
        o = emit(out, sizeof out, o, "\033[%d;1H\033[J", row + 1);
    shadow_count = row;

    if (o == 0) return;                    /* nothing changed: emit nothing */

    write_all("\033[?2026h", 8);            /* synchronized update: no tearing */
    write_all(out, o);
    write_all("\033[?2026l", 8);
}

/* ---- input ---- */
enum {
    KEY_UP = 1000, KEY_DOWN, KEY_RIGHT, KEY_LEFT,   /* arrows (CSI and SS3) */
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_IGNORE = -2                                /* consumed, no action */
};

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

static volatile sig_atomic_t resized = 0;
static volatile sig_atomic_t g_stop = 0;   /* set by SIGINT/SIGTERM/SIGHUP */
static void on_winch(int s) { (void)s; resized = 1; }
static void on_stop(int s) { (void)s; g_stop = 1; }

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
        if (e) {
            tracks[i].dur = e->dur;          /* may be -1: probed, no duration */
            tracks[i].trackno = e->trackno;
            tracks[i].title  = e->title  ? xstrdup(e->title)  : NULL;
            tracks[i].artist = e->artist ? xstrdup(e->artist) : NULL;
            tracks[i].album  = e->album  ? xstrdup(e->album)  : NULL;
            tracks[i].probed = 1;            /* cached: don't probe again */
            apply_group(&tracks[i]);
        }
    }

    srand((unsigned)time(NULL));
    queue = xmalloc(ntracks * sizeof(int));
    rebuild_view();   /* initial view = all tracks; also builds the queue */
    build_albums();   /* album list for the Albums tab */

    detect_vol_backend();

    signal(SIGPIPE, SIG_IGN);
    signal(SIGWINCH, on_winch);
    /* ^C (and TERM/HUP) must leave via the normal exit path: dying by
     * default action would leave the terminal raw, ffplay running, and
     * the metadata cache unsaved. */
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    signal(SIGHUP, on_stop);

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
        if (child > 0 && !g_stop) {
            int st;
            pid_t r = waitpid(child, &st, WNOHANG);
            if (r == child) {
                child = -1;
                if (!intentional) {
                    if (repeat == REP_ONE && cur >= 0) play_track(cur, 0);
                    else next_track();
                }
            }
        }

        volume_tick();   /* debounced volume restarts, live-volume retries */

        if (resized) resized = 0;   /* draw() sees the new geometry and repaints in place */

        int probing = probe_idx < ntracks;

        /* While probing, don't block in select — keep input snappy but make
         * steady progress on durations. Once done, idle at a 250ms refresh
         * (50ms while a volume change is still waiting on the stream). */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        int vwait = (vol_dirty || restart_at > 0);
        struct timeval tv = { 0, probing ? 0 : vwait ? 50000 : 250000 };
        int rv = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);

        if (rv > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
            int k = read_key();
            int alist = on_album_list();
            if (searching) {
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
                    case '\033':                                     /* esc: clear */
                        searching = 0; clear_query(); apply_query();
                        break;
                    case 127: case 8:                                /* backspace */
                        if (qlen > 0) { query[--qlen] = '\0'; apply_query(); }
                        break;
                    default:
                        /* printable ASCII and UTF-8 bytes; decoded KEY_* codes
                         * (1000+) never reach this branch */
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
                    case '\t': switch_tab((tab + 1) % TAB_COUNT); break;
                    case '/': searching = 1; break;
                    case '\033': if (query[0]) { clear_query(); rebuild_albums_view(); } break;
                    case 'q': running = 0; break;
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
                    case KEY_RIGHT: seek(5); break;
                    case KEY_LEFT: seek(-5); break;
                    case '-': case '_': set_volume(-5); break;
                    case '+': case '=': set_volume(5); break;
                    case 'r': repeat = (repeat + 1) % 3; break;
                    case 's': shuffle = !shuffle; build_queue(); break;
                    case '\t': switch_tab((tab + 1) % TAB_COUNT); break;
                    case '/': searching = 1; break;
                    case '\033':                  /* esc: clear filter, else leave album */
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
                    case 'q': running = 0; break;
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

        /* throttle redraws to ~10/sec so probing/progress stay smooth */
        double t = now_sec();
        if (t - last_draw >= 0.1) { draw(); last_draw = t; }
    }

    stop_child();
    save_cache();          /* persist anything probed this session */
    term_restore();
    return 0;
}
