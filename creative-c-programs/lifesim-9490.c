/*
 * ╔════════════════════════════════════════════════════════════════════╗
 * ║  lifesim - Interactive Cellular Automata Explorer                 ║
 * ║                                                                    ║
 * ║  Explore Conway's Game of Life and 6 other cellular automata       ║
 * ║  rulesets. Draw cells interactively, stamp classic patterns,       ║
 * ║  and watch emergent complexity unfold in your terminal.            ║
 * ║                                                                    ║
 * ║  Compile:  gcc -std=c11 -O2 -o lifesim lifesim-9490.c             ║
 * ║     (or:   gcc -std=c99 -O2 -o lifesim lifesim-9490.c)            ║
 * ║  Run:      ./lifesim                                              ║
 * ║                                                                    ║
 * ║  Requires: POSIX terminal (Linux/macOS), ANSI color support        ║
 * ╚════════════════════════════════════════════════════════════════════╝
 *
 * Controls:
 *   Arrow Keys / HJKL   Move cursor
 *   Space               Toggle cell under cursor
 *   D                   Draw mode (hold and move to paint cells)
 *   Enter               Step one generation
 *   P                   Play / Pause simulation
 *   + / -               Speed up / Slow down
 *   R                   Random fill (density set by 0-9 keys)
 *   C                   Clear grid
 *   1-7                 Select ruleset
 *   S                   Stamp current pattern at cursor
 *   Tab                 Cycle to next pattern
 *   G                   Toggle grid dots
 *   O                   Toggle population sparkline graph
 *   Q / Esc             Quit
 *
 * Rulesets:
 *   1) Conway's Life  (B3/S23)     - the classic
 *   2) HighLife       (B36/S23)    - creates replicators
 *   3) Seeds          (B2/S)       - explosive growth, everything dies
 *   4) Day & Night    (B3678/S34678) - symmetric beauty
 *   5) Diamoeba       (B35678/S5678) - diamond-shaped growth
 *   6) Replicator     (B1357/S1357) - everything copies itself
 *   7) Morley (Move)  (B368/S245)  - chaotic motion
 */

#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/time.h>

/* ── Constants ──────────────────────────────────────────────────────── */

#define MAX_ROWS     100
#define MAX_COLS     300
#define HIST_LEN     200
#define MAX_AGE      255
#define DEFAULT_SPEED 80      /* ms between generations */
#define MIN_SPEED     10
#define MAX_SPEED     500
#define SPEED_STEP    10
#define RENDER_US     16000   /* ~60 fps render */
#define BUF_SIZE      (MAX_ROWS * MAX_COLS * 20 + 8192)

/* ── Types ──────────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *code;
    int birth[9];
    int survive[9];
} Ruleset;

typedef struct {
    const char *name;
    int h, w;
    const char *rows[20];
} Pattern;

/* ── Rulesets ───────────────────────────────────────────────────────── */

static const Ruleset rulesets[] = {
    /*                   name              code          B0 B1 B2 B3 B4 B5 B6 B7 B8  S0 S1 S2 S3 S4 S5 S6 S7 S8 */
    {"Conway's Life",  "B3/S23",     {0, 0, 0, 1, 0, 0, 0, 0, 0}, {0, 0, 1, 1, 0, 0, 0, 0, 0}},
    {"HighLife",       "B36/S23",    {0, 0, 0, 1, 0, 0, 1, 0, 0}, {0, 0, 1, 1, 0, 0, 0, 0, 0}},
    {"Seeds",          "B2/S",       {0, 0, 1, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"Day & Night",    "B3678/S34678",{0, 0, 0, 1, 0, 0, 1, 1, 1}, {0, 0, 0, 1, 1, 0, 1, 1, 1}},
    {"Diamoeba",       "B35678/S5678",{0, 0, 0, 1, 0, 1, 1, 1, 1}, {0, 0, 0, 0, 0, 1, 1, 1, 1}},
    {"Replicator",     "B1357/S1357",{0, 1, 0, 1, 0, 1, 0, 1, 0}, {0, 1, 0, 1, 0, 1, 0, 1, 0}},
    {"Morley (Move)",  "B368/S245",  {0, 0, 0, 1, 0, 0, 1, 0, 1}, {0, 0, 1, 0, 1, 0, 0, 0, 0}},
};
#define NUM_RULESETS (sizeof(rulesets) / sizeof(rulesets[0]))

/* ── Patterns ───────────────────────────────────────────────────────── */

static const Pattern patterns[] = {
    {"Glider", 3, 3,
     {"#.#", "..#", "###"}},
    {"LWSS", 4, 5,
     {"#..#.", "#....", "#...#", "####."}},
    {"R-pentomino", 3, 3,
     {".##", "##.", ".#."}},
    {"Acorn", 3, 7,
     {".#.....", "...#...", "##..###"}},
    {"Diehard", 3, 8,
     {"......#.", "##......", ".#...###"}},
    {"Beacon", 4, 4,
     {"##..", "##..", "..##", "..##"}},
    {"Pulsar", 13, 13,
     {"..###...###..",
      ".............",
      "#....#.#....#",
      "#....#.#....#",
      "#....#.#....#",
      "..###...###..",
      ".............",
      "..###...###..",
      "#....#.#....#",
      "#....#.#....#",
      "#....#.#....#",
      ".............",
      "..###...###.."}},
    {"Gosper Glider Gun", 9, 36,
     {"........................#...........",
      "......................#.#...........",
      "............##......##............##",
      "...........#...#....##............##",
      "##........#.....#...##..............",
      "##........#...#.##....#.#...........",
      "..........#.....#.......#...........",
      "...........#...#....................",
      "............##......................"}},
};
#define NUM_PATTERNS (sizeof(patterns) / sizeof(patterns[0]))

/* ── Global State ───────────────────────────────────────────────────── */

static struct {
    int grid[MAX_ROWS][MAX_COLS];
    int next_buf[MAX_ROWS][MAX_COLS];
    int age[MAX_ROWS][MAX_COLS];
    int rows, cols;
    int term_w, term_h;
    int cur_r, cur_c;
    int running;
    int speed;
    int ruleset;
    int pat_idx;
    int gen;
    int pop;
    int pop_hist[HIST_LEN];
    int hist_count;
    int show_grid;
    int show_graph;
    int draw_mode;
    int density;
    int dirty;
    struct termios orig_term;
    char buf[BUF_SIZE];
} S;

/* ── Terminal Handling ──────────────────────────────────────────────── */

static void term_raw(void) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &S.orig_term);
    t = S.orig_term;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

static void raw_write(const char *s, int n) {
    ssize_t r;
    do { r = write(STDOUT_FILENO, s, (size_t)n); } while (r < 0);
}

static void term_restore(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &S.orig_term);
    raw_write("\033[?25h\033[?1049l", 15);
}

static void get_term_size(void) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0 && w.ws_row > 0) {
        S.term_w = w.ws_col < 40 ? 40 : w.ws_col;
        S.term_h = w.ws_row < 12 ? 12 : w.ws_row;
    }
}

static void on_sigwinch(int sig) {
    (void)sig;
    get_term_size();
    S.dirty = 1;
}

/* ── Helpers ────────────────────────────────────────────────────────── */

static long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ── Grid Operations ────────────────────────────────────────────────── */

static void grid_resize(void) {
    int nr = S.term_h - 6;
    int nc = S.term_w - 2;
    if (nr < 5) nr = 5;
    if (nc < 10) nc = 10;
    if (nr > MAX_ROWS) nr = MAX_ROWS;
    if (nc > MAX_COLS) nc = MAX_COLS;

    if (nr != S.rows || nc != S.cols) {
        for (int r = 0; r < MAX_ROWS; r++)
            for (int c = 0; c < MAX_COLS; c++)
                if (r >= nr || c >= nc) {
                    S.grid[r][c] = 0;
                    S.age[r][c] = 0;
                }
        S.rows = nr;
        S.cols = nc;
    }
}

static void grid_clear(void) {
    memset(S.grid, 0, sizeof(S.grid));
    memset(S.age, 0, sizeof(S.age));
    S.gen = 0;
    S.pop = 0;
    S.hist_count = 0;
    memset(S.pop_hist, 0, sizeof(S.pop_hist));
}

static void count_pop(void) {
    int n = 0;
    for (int r = 0; r < S.rows; r++)
        for (int c = 0; c < S.cols; c++)
            n += S.grid[r][c];
    S.pop = n;
}

static int neighbors(int r, int c) {
    int n = 0;
    for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) continue;
            int nr = (r + dr + S.rows) % S.rows;
            int nc = (c + dc + S.cols) % S.cols;
            n += S.grid[nr][nc];
        }
    return n;
}

static void step(void) {
    const Ruleset *rs = &rulesets[S.ruleset];
    for (int r = 0; r < S.rows; r++)
        for (int c = 0; c < S.cols; c++) {
            int n = neighbors(r, c);
            S.next_buf[r][c] = S.grid[r][c]
                ? (n < 9 && rs->survive[n])
                : (n < 9 && rs->birth[n]);
        }
    for (int r = 0; r < S.rows; r++)
        for (int c = 0; c < S.cols; c++) {
            if (S.next_buf[r][c]) {
                S.age[r][c] = S.grid[r][c]
                    ? (S.age[r][c] < MAX_AGE ? S.age[r][c] + 1 : MAX_AGE)
                    : 1;
            } else {
                S.age[r][c] = 0;
            }
            S.grid[r][c] = S.next_buf[r][c];
        }
    S.gen++;
    count_pop();
    if (S.hist_count < HIST_LEN)
        S.pop_hist[S.hist_count++] = S.pop;
    else {
        memmove(S.pop_hist, S.pop_hist + 1, (HIST_LEN - 1) * sizeof(int));
        S.pop_hist[HIST_LEN - 1] = S.pop;
    }
}

static void random_fill(void) {
    int pct = (S.density + 1) * 5;
    for (int r = 0; r < S.rows; r++)
        for (int c = 0; c < S.cols; c++) {
            S.grid[r][c] = (rand() % 100 < pct) ? 1 : 0;
            S.age[r][c] = S.grid[r][c] ? 1 : 0;
        }
    S.gen = 0;
    count_pop();
    S.hist_count = 0;
    memset(S.pop_hist, 0, sizeof(S.pop_hist));
}

static void stamp_pattern(int idx, int cr, int cc) {
    const Pattern *p = &patterns[idx];
    for (int r = 0; r < p->h && r < 20; r++) {
        if (!p->rows[r]) continue;
        int len = (int)strlen(p->rows[r]);
        for (int c = 0; c < p->w && c < len; c++) {
            if (p->rows[r][c] == '#') {
                int gr = (cr + r) % S.rows;
                int gc = (cc + c) % S.cols;
                S.grid[gr][gc] = 1;
                S.age[gr][gc] = 1;
            }
        }
    }
    count_pop();
}

/* ── Rendering ──────────────────────────────────────────────────────── */

/* Map cell age to ANSI 256-color index */
static int age_color(int a) {
    if (a <= 1) return 15;   /* bright white - newborn */
    if (a <= 2) return 230;  /* light yellow */
    if (a <= 4) return 227;  /* yellow */
    if (a <= 8) return 190;  /* yellow-green */
    if (a <= 16) return 82;  /* bright green */
    if (a <= 32) return 46;  /* green */
    if (a <= 64) return 51;  /* cyan */
    if (a <= 128) return 39; /* blue */
    return 201;               /* magenta - ancient */
}

static void flush_all(int len) {
    int written = 0;
    while (written < len) {
        int n = (int)write(STDOUT_FILENO, S.buf + written, len - written);
        if (n <= 0) break;
        written += n;
    }
}

static void render(void) {
    int p = 0;

    /* Home cursor, clear if dirty */
    if (S.dirty) {
        p += sprintf(S.buf + p, "\033[2J\033[H");
        S.dirty = 0;
    } else {
        p += sprintf(S.buf + p, "\033[H");
    }

    /* ── Header line 1: title bar ── */
    const Ruleset *rs = &rulesets[S.ruleset];
    p += sprintf(S.buf + p,
        "\033[1;37;44m lifesim "
        "\033[1;36m%-14s\033[1;37m %s "
        "\033[1;33mGen:%-7d "
        "\033[1;32mPop:%-6d "
        "\033[1;35m%3dms "
        "\033[1;34m%s\033[K\033[0m\r\n",
        rs->name, rs->code, S.gen, S.pop, S.speed,
        S.running ? "\xe2\x96\xb6 PLAY " : "\xe2\x96\xa0 PAUSE");

    /* ── Header line 2: info ── */
    p += sprintf(S.buf + p,
        "\033[0;37m Grid:%dx%d  Stamp:\033[1;33m%-18s\033[0;37m "
        "Density:%d%%  Cursor:(%d,%d)%s\033[K\033[0m\r\n",
        S.rows, S.cols, patterns[S.pat_idx].name,
        (S.density + 1) * 5, S.cur_r, S.cur_c,
        S.draw_mode ? "  \033[1;31m[DRAW]\033[0m" : "");

    /* ── Grid ── */
    for (int r = 0; r < S.rows; r++) {
        for (int c = 0; c < S.cols; c++) {
            if (r == S.cur_r && c == S.cur_c) {
                /* Cursor cell - magenta highlight */
                if (S.grid[r][c])
                    p += sprintf(S.buf + p, "\033[1;45;97m\xe2\x96\x88");
                else
                    p += sprintf(S.buf + p, "\033[1;45;37m\xe2\x96\x92");
            } else if (S.grid[r][c]) {
                /* Alive cell colored by age */
                p += sprintf(S.buf + p, "\033[38;5;%dm\xe2\x96\x88", age_color(S.age[r][c]));
            } else if (S.show_grid) {
                p += sprintf(S.buf + p, "\033[0;90m\xc2\xb7");
            } else {
                S.buf[p++] = ' ';
            }
        }
        p += sprintf(S.buf + p, "\033[K\r\n");
    }

    /* ── Population sparkline ── */
    if (S.show_graph && S.hist_count > 2) {
        int max_p = 1;
        for (int i = 0; i < S.hist_count; i++)
            if (S.pop_hist[i] > max_p) max_p = S.pop_hist[i];

        /* Unicode sparkline chars: ▁▂▃▄▅▆▇█ */
        static const char *sparks[] = {
            "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
            "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"
        };
        p += sprintf(S.buf + p, "\033[0;36m Pop:");
        int avail = S.cols - 5;
        int start = S.hist_count > avail ? S.hist_count - avail : 0;
        for (int i = start; i < S.hist_count; i++) {
            int v = S.pop_hist[i];
            int lv = v * 7 / max_p;
            if (lv < 0) lv = 0;
            if (lv > 7) lv = 7;
            const char *s = sparks[lv];
            memcpy(S.buf + p, s, 3);
            p += 3;
        }
        p += sprintf(S.buf + p, "\033[K\r\n");
    }

    /* ── Footer ── */
    p += sprintf(S.buf + p,
        "\033[0;37m"
        " Space:draw  \xe2\x86\x90\xe2\x86\x91\xe2\x86\x93\xe2\x86\x92/HJKL:move  "
        "Enter:step  P:play  +/-:speed  R:rand  C:clr\033[K\r\n"
        " 1-7:rules  S:stamp  Tab:pattern  G:grid  O:graph  [/]:density  D:draw  Q:quit"
        "\033[K\033[0m");

    flush_all(p);
}

/* ── Input Handling ─────────────────────────────────────────────────── */

static int read_key(void) {
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0)
        return -1;

    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) <= 0)
        return -1;

    if (c == 27) {
        /* Escape sequence: read up to 2 more bytes */
        unsigned char s1 = 0, s2 = 0;
        fd_set f;
        struct timeval t = {0, 30000};
        FD_ZERO(&f);
        FD_SET(STDIN_FILENO, &f);
        if (select(STDIN_FILENO + 1, &f, NULL, NULL, &t) <= 0)
            return 27; /* bare Escape */
        if (read(STDIN_FILENO, &s1, 1) <= 0) return 27;
        if (s1 == '[') {
            FD_ZERO(&f);
            FD_SET(STDIN_FILENO, &f);
            t.tv_usec = 30000;
            if (select(STDIN_FILENO + 1, &f, NULL, NULL, &t) <= 0) return -1;
            if (read(STDIN_FILENO, &s2, 1) <= 0) return -1;
            switch (s2) {
                case 'A': return 'K'; /* Up    */
                case 'B': return 'J'; /* Down  */
                case 'C': return 'L'; /* Right */
                case 'D': return 'H'; /* Left  */
                default:  return -1;  /* unknown sequence */
            }
        }
        return 27;
    }
    return (int)c;
}

static void handle_keys(void) {
    int key = read_key();
    if (key < 0) return;

    switch (key) {
    /* Movement */
    case 'h': case 'H': S.cur_c = (S.cur_c - 1 + S.cols) % S.cols; break;
    case 'j': case 'J': S.cur_r = (S.cur_r + 1) % S.rows; break;
    case 'k': case 'K': S.cur_r = (S.cur_r - 1 + S.rows) % S.rows; break;
    case 'l': case 'L': S.cur_c = (S.cur_c + 1) % S.cols; break;

    /* Toggle cell / draw */
    case ' ':
        S.grid[S.cur_r][S.cur_c] = !S.grid[S.cur_r][S.cur_c];
        S.age[S.cur_r][S.cur_c] = S.grid[S.cur_r][S.cur_c] ? 1 : 0;
        count_pop();
        break;

    /* Draw mode toggle */
    case 'd': case 'D':
        S.draw_mode = !S.draw_mode;
        break;

    /* Step */
    case '\n': case '\r':
        step();
        break;

    /* Play/Pause */
    case 'p': case 'P':
        S.running = !S.running;
        break;

    /* Speed */
    case '+': case '=':
        if (S.speed > MIN_SPEED) S.speed -= SPEED_STEP;
        break;
    case '-': case '_':
        if (S.speed < MAX_SPEED) S.speed += SPEED_STEP;
        break;

    /* Random */
    case 'r': case 'R':
        random_fill();
        break;

    /* Clear */
    case 'c': case 'C':
        grid_clear();
        break;

    /* Rulesets 1-7 */
    case '1': case '2': case '3': case '4': case '5': case '6': case '7':
        S.ruleset = key - '1';
        if (S.ruleset >= (int)NUM_RULESETS) S.ruleset = 0;
        break;

    /* Stamp pattern */
    case 's': case 'S':
        stamp_pattern(S.pat_idx, S.cur_r, S.cur_c);
        break;

    /* Next pattern */
    case '\t':
        S.pat_idx = (S.pat_idx + 1) % (int)NUM_PATTERNS;
        break;

    /* Toggle grid dots */
    case 'g': case 'G':
        S.show_grid = !S.show_grid;
        S.dirty = 1;
        break;

    /* Toggle population graph */
    case 'o': case 'O':
        S.show_graph = !S.show_graph;
        S.dirty = 1;
        break;

    /* Density: [ decrease, ] increase */
    case '[':
        if (S.density > 0) S.density--;
        break;
    case ']':
        if (S.density < 9) S.density++;
        break;

    /* Quit */
    case 'q': case 'Q': case 27:
        exit(0);
    }
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(void) {
    srand((unsigned)time(NULL));
    memset(&S, 0, sizeof(S));

    S.speed     = DEFAULT_SPEED;
    S.show_grid = 1;
    S.show_graph = 1;
    S.density   = 3;   /* 20% */
    S.dirty     = 1;

    /* Terminal init */
    get_term_size();
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigwinch;
    sigaction(SIGWINCH, &sa, NULL);

    term_raw();
    atexit(term_restore);

    /* Alternate screen buffer, hide cursor */
    raw_write("\033[?1049h\033[?25l", 15);

    grid_resize();
    grid_clear();

    /* Seed some patterns for immediate visual interest */
    int mr = S.rows / 2;
    int mc = S.cols / 2;
    stamp_pattern(0, mr - 8, mc - 10);  /* Glider */
    stamp_pattern(2, mr - 1, mc - 1);   /* R-pentomino */
    stamp_pattern(3, mr + 5, mc + 10);  /* Acorn */
    stamp_pattern(4, mr + 8, mc - 15);  /* Diehard */

    /* Main loop */
    long last_step_ms = now_ms();

    for (;;) {
        /* Handle terminal resize */
        int ow = S.term_w, oh = S.term_h;
        get_term_size();
        if (ow != S.term_w || oh != S.term_h) {
            S.dirty = 1;
            grid_resize();
        }

        /* Drain all pending key events */
        for (int i = 0; i < 30; i++)
            handle_keys();

        /* Auto-step if playing */
        if (S.running) {
            long now = now_ms();
            if (now - last_step_ms >= S.speed) {
                step();
                last_step_ms = now;
            }
        }

        /* Draw mode: paint cells as cursor moves (handled via movement) */
        if (S.draw_mode) {
            S.grid[S.cur_r][S.cur_c] = 1;
            S.age[S.cur_r][S.cur_c] = 1;
        }

        render();
        usleep(RENDER_US);
    }

    return 0;
}
