/* Tests for the platform-independent parts of the Linux port that the
 * Windows port added last: fades and custom keybinds.
 *
 * The player is one C file per platform, so the test translation unit
 * includes aqua.c directly and renames its main(). */
#define main aqua_main
#include "../aqua.c"
#undef main

#include <stdio.h>

static int failures = 0;
static int checks = 0;

static void check(int ok, const char *what, const char *file, int line) {
    checks++;
    if (!ok) {
        failures++;
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what);
    }
}

#define CHECK(cond) check((cond) != 0, #cond, __FILE__, __LINE__)
#define CHECK_STR(got, want) do {                                        \
        const char *g_ = (got), *w_ = (want);                            \
        checks++;                                                        \
        if (strcmp(g_, w_) != 0) {                                       \
            failures++;                                                  \
            fprintf(stderr, "FAIL %s:%d: got \"%s\", want \"%s\"\n",     \
                    __FILE__, __LINE__, g_, w_);                         \
        }                                                                \
    } while (0)

/* ---- fades ---- */

static void test_cycle_fade(void) {
    CHECK(cycle_fade(0, 1) == 1);
    CHECK(cycle_fade(1, 1) == 2);
    CHECK(cycle_fade(2, -1) == 1);
    CHECK(cycle_fade(30, 1) == 0);       /* wraps at the last preset */
    CHECK(cycle_fade(0, -1) == 30);      /* and at the first */
    CHECK(cycle_fade(3.5, 1) == 5);      /* a typed value between presets */
    CHECK(cycle_fade(3.5, -1) == 3);
    CHECK(cycle_fade(20, 1) == 30);
    CHECK(cycle_fade(60, 1) == 30);      /* above the presets: stays on the last one */
    CHECK(cycle_fade(60, -1) == 30);
}

static void test_fade_filter(void) {
    char buf[128];

    fade_in = 0; fade_out = 0;
    fade_filter(buf, sizeof buf, 0, 300);
    CHECK_STR(buf, "");

    fade_in = 3; fade_out = 0;
    fade_filter(buf, sizeof buf, 0, 300);
    CHECK_STR(buf, "afade=t=in:st=0:d=3.00");

    fade_in = 0; fade_out = 5;
    fade_filter(buf, sizeof buf, 0, 200);
    CHECK_STR(buf, "afade=t=out:st=195.00:d=5.00");

    /* after a seek there is no fade-in, but the fade-out still sits at the
     * end of the remaining audio (ffplay resets timestamps at -ss) */
    fade_in = 3; fade_out = 5;
    fade_filter(buf, sizeof buf, 10, 200);
    CHECK_STR(buf, "afade=t=out:st=185.00:d=5.00");

    /* a track shorter than the fade-out: fade across what is left, from
     * the top, instead of emitting a negative start time */
    fade_in = 0; fade_out = 5;
    fade_filter(buf, sizeof buf, 0, 4);
    CHECK_STR(buf, "afade=t=out:st=0.00:d=4.00");

    /* usage includes searches, so never emit a fade past the file */
    fade_filter(buf, sizeof buf, 10, 4);
    CHECK_STR(buf, "");

    /* unknown duration: fade-in still works, fade-out has nowhere to go */
    fade_in = 2; fade_out = 5;
    fade_filter(buf, sizeof buf, 0, -1);
    CHECK_STR(buf, "afade=t=in:st=0:d=2.00");

    /* both applied to one track */
    fade_in = 1; fade_out = 1;
    fade_filter(buf, sizeof buf, 0, 300);
    CHECK_STR(buf, "afade=t=in:st=0:d=1.00,afade=t=out:st=299.00:d=1.00");

    /* a short buffer truncates but stays terminated */
    fade_in = 3; fade_out = 3;
    fade_filter(buf, 10, 0, 300);
    CHECK(strlen(buf) < 10);
    CHECK(strchr(buf, ',') == NULL);

    fade_in = 0; fade_out = 0;
}

/* ---- keybinds ---- */

static void test_keybinds(void) {
    char buf[16];

    kb_defaults();
    for (int i = 0; i < KB_COUNT; i++) CHECK(kb_key[i] == kb_def[i]);
    CHECK(kb_translate('n') == 'n');          /* defaults pass through */
    CHECK(kb_translate(KEY_UP) == KEY_UP);

    CHECK_STR(key_name(' ', buf, sizeof buf), "Space");
    CHECK_STR(key_name(27, buf, sizeof buf), "Esc");
    CHECK_STR(key_name(KEY_UP, buf, sizeof buf), "Up");
    CHECK_STR(key_name(KEY_BTAB, buf, sizeof buf), "Shift-Tab");
    CHECK_STR(key_name('x', buf, sizeof buf), "x");
    CHECK_STR(key_name(8, buf, sizeof buf), "Backspace");

    /* a custom key stands in for the action's built-in key */
    kbsel = KB_NEXT;
    kb_assign('x');
    CHECK(kb_key[KB_NEXT] == 'x');
    CHECK(kb_translate('x') == 'n');
    CHECK(kb_translate('n') == 'n');          /* the default alias still works */
    CHECK(kb_translate('b') == 'b');

    /* assigning a key another action owns swaps the two */
    kbsel = KB_PREV;                          /* 'b' -> 's' (shuffle's key) */
    kb_assign('s');
    CHECK(kb_key[KB_PREV] == 's');
    CHECK(kb_key[KB_SHUFFLE] == 'b');
    CHECK(kb_translate('s') == 'b');
    CHECK(kb_translate('b') == 's');

    /* arrow keys can be bound too */
    kbsel = KB_VOLUP;
    kb_assign('[');
    CHECK(kb_translate('[') == '+');

    kb_defaults();
}

/* ---- settings persistence ---- */

static void test_settings_roundtrip(void) {
    char path[] = "/tmp/aqua-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0) return;
    close(fd);

    snprintf(settings_file, sizeof settings_file, "%s", path);
    kb_defaults();

    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fprintf(f, "fadein=2.50\nfadeout=7.25\nbind_1=120\nbind_12=91\n");
        fclose(f);
    }
    fade_in = fade_out = 0;
    load_settings();
    CHECK(fade_in == 2.5);
    CHECK(fade_out == 7.25);
    CHECK(kb_key[KB_NEXT] == 'x');
    CHECK(kb_key[KB_VOLDN] == '[');
    CHECK(kb_key[KB_PLAY] == ' ');            /* unset rows keep the default */

    /* out-of-range and hostile values are ignored or clamped */
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "fadein=-5\nfadeout=999\nbind_1=-3\nbind_99=65\n");
        fclose(f);
    }
    load_settings();
    CHECK(fade_in == 0);
    CHECK(fade_out == 60);
    CHECK(kb_key[KB_NEXT] == 'x');            /* n <= 0 ignored */

    /* saves everything it loaded */
    save_settings();
    fade_in = fade_out = 0;
    kb_defaults();
    load_settings();
    CHECK(fade_in == 0);
    CHECK(fade_out == 60);
    CHECK(kb_key[KB_NEXT] == 'x');

    /* reset restores defaults without touching the music folder */
    fade_in = 4; fade_out = 4; kbsel = KB_NEXT; kb_assign('z'); kb_open = 1;
    settings_reset();
    CHECK(fade_in == 0);
    CHECK(fade_out == 0);
    CHECK(kb_open == 0);
    CHECK(kb_key[KB_NEXT] == 'n');

    remove(path);
}

static void test_settings_rows(void) {
    scope_on = 0;
    settings_rows();
    CHECK(nset > 0);
    CHECK(set_vis[0] == SET_MUSIC);

    int have_fadein = 0, have_fadeout = 0, have_binds = 0, have_scope_row = 0;
    for (int i = 0; i < nset; i++) {
        if (set_vis[i] == SET_FADEIN)  have_fadein = i;
        if (set_vis[i] == SET_FADEOUT) have_fadeout = i;
        if (set_vis[i] == SET_KEYBINDS) have_binds = i;
        if (set_vis[i] >= SET_SC_MODE && set_vis[i] <= SET_SC_COLY) have_scope_row = 1;
        if (set_vis[i] == SET_RESET) CHECK(i == nset - 1);
    }
    CHECK(have_fadein > 0);
    CHECK(have_fadeout == have_fadein + 1);
    CHECK(have_binds == have_fadeout + 1);
    CHECK(!have_scope_row);

    scope_on = 1;
    settings_rows();
    have_scope_row = 0;
    for (int i = 0; i < nset; i++)
        if (set_vis[i] >= SET_SC_MODE && set_vis[i] <= SET_SC_COLY) have_scope_row = 1;
    CHECK(have_scope_row);
    scope_on = 0;
    settings_rows();
}

int main(void) {
    test_cycle_fade();
    test_fade_filter();
    test_keybinds();
    test_settings_roundtrip();
    test_settings_rows();

    if (failures) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("ok: %d checks\n", checks);
    return 0;
}
