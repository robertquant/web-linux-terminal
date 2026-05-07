/*
 * ═══════════════════════════════════════════════════════════════════════
 *   REACTDIFF — Terminal Reaction-Diffusion Simulator (Gray-Scott Model)
 * ═══════════════════════════════════════════════════════════════════════
 *
 *  Simulates two virtual chemicals (U and V) that diffuse across a 2D
 *  grid and react via the Gray-Scott equations:
 *
 *      dU/dt = Du * nabla^2(U) - U*V^2 + f*(1-U)
 *      dV/dt = Dv * nabla^2(V) + U*V^2 - (f+k)*V
 *
 *  Small parameter changes (feed rate f, kill rate k) produce wildly
 *  different emergent patterns: coral, mitosis, fingerprints, spots,
 *  stripes, worm-like labyrinths...
 *
 *  Compile:  gcc -O2 -o reactdiff reactdiff-5841.c -lm
 *  Run:      ./reactdiff
 *
 *  Controls:
 *    Space    Pause / Resume           R      Reset simulation
 *    1-6      Switch preset            C      Clear grid
 *    +/-      More/fewer steps/frame   S      Add random seed
 *    P        Cycle color palette      Click  Seed V at cursor
 *    Q / Esc  Quit
 *
 *  Requires: ANSI 256-color terminal, POSIX (termios).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <signal.h>

/* ── Constants ── */
#define MAXW 300
#define MAXH 120
#define NPAL 5

/* ── Preset parameter sets (f = feed rate, k = kill rate) ── */
typedef struct { const char *name; double f, k; } Preset;
static const Preset presets[] = {
    {"Coral Growth",    0.0545, 0.0620},
    {"Mitosis",         0.0367, 0.0649},
    {"Fingerprints",    0.0550, 0.0620},
    {"Soliton Spots",   0.0300, 0.0620},
    {"Stripes",         0.0400, 0.0600},
    {"Worms / Mazes",   0.0780, 0.0610},
};
#define NPRES ((int)(sizeof presets / sizeof *presets))

/* Fixed diffusion coefficients */
static const double Du = 0.16, Dv = 0.08;

/* ── Global state ── */
static double gU[MAXH][MAXW], gV[MAXH][MAXW]; /* current concentrations */
static double bU[MAXH][MAXW], bV[MAXH][MAXW]; /* next-step buffer */
static int sw, sh;                /* active grid dimensions */
static int cur_pre  = 0;         /* current preset index */
static int steps    = 10;        /* simulation steps per rendered frame */
static int paused   = 0;
static int cur_pal  = 0;         /* current palette index */
static int frame    = 0;
static volatile sig_atomic_t got_resize = 0;

/* ── Color palette: maps 0..255 → ANSI 256-color index ── */
static int cmap[256];
static const char *pal_names[NPAL] = {"Heat", "Ocean", "Neon", "Toxic", "Mono"};

static inline int ic(int x) { return x < 0 ? 0 : x > 5 ? 5 : x; }
static inline int ansicolor(int r, int g, int b) {
    return 16 + 36 * ic(r) + 6 * ic(g) + ic(b);
}

/* Fill cmap[lo..hi] with a linear gradient between two RGB colors (0-5) */
static void grad(int lo, int hi,
                 int r0, int g0, int b0,
                 int r1, int g1, int b1) {
    if (hi <= lo) hi = lo + 1;
    for (int i = lo; i <= hi && i < 256; i++) {
        double t = (double)(i - lo) / (hi - lo);
        cmap[i] = ansicolor(
            (int)(r0 + t * (r1 - r0) + 0.5),
            (int)(g0 + t * (g1 - g0) + 0.5),
            (int)(b0 + t * (b1 - b0) + 0.5));
    }
}

static void build_palette(int pid) {
    memset(cmap, 0, sizeof cmap);
    switch (pid) {
    case 0: /* Heat: black → red → orange → yellow → white */
        grad(0,   51,  0,0,0,  3,0,0);
        grad(51, 115,  3,0,0,  5,2,0);
        grad(115,179,  5,2,0,  5,5,0);
        grad(179,255,  5,5,0,  5,5,4);
        break;
    case 1: /* Ocean: dark blue → cyan → white */
        grad(0,   77,  0,0,2,  0,1,4);
        grad(77, 153,  0,1,4,  0,4,5);
        grad(153,255,  0,4,5,  5,5,5);
        break;
    case 2: /* Neon: black → magenta → blue → cyan → green */
        grad(0,   64,  0,0,1,  4,0,4);
        grad(64, 128,  4,0,4,  1,0,5);
        grad(128,192,  1,0,5,  0,4,4);
        grad(192,255,  0,4,4,  2,5,0);
        break;
    case 3: /* Toxic: black → green → yellow → orange */
        grad(0,   64,  0,1,0,  0,3,0);
        grad(64,  128, 0,3,0,  2,5,0);
        grad(128,192,  2,5,0,  5,5,0);
        grad(192,255,  5,5,0,  5,2,0);
        break;
    case 4: /* Mono: black → white */
        grad(0, 255, 0,0,0, 5,5,5);
        break;
    }
}

/* ── Terminal handling ── */
static struct termios orig_term;

static void term_restore(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
    fprintf(stderr, "\033[?25h\033[?1000l\033[0m\r\n");
}

static void term_init(void) {
    tcgetattr(STDIN_FILENO, &orig_term);
    struct termios t = orig_term;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    fprintf(stderr, "\033[?25l\033[?1000h");  /* hide cursor, enable mouse */
    atexit(term_restore);
}

static void get_size(int *w, int *h) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    } else {
        *w = 80; *h = 24;
    }
}

static void on_resize(int sig) { (void)sig; got_resize = 1; }

/* ── Grid sizing ── */
static void resize_grid(void) {
    int tw, th;
    get_size(&tw, &th);
    sw = tw / 2;          /* each cell is 2 chars wide for square aspect */
    sh = th - 2;          /* reserve 2 rows for HUD */
    if (sw > MAXW) sw = MAXW;
    if (sh > MAXH) sh = MAXH;
    if (sw < 20) sw = 20;
    if (sh < 10) sh = 10;
}

/* ── Simulation ── */

/* Seed a circular patch of chemical V */
static void seed_circle(int cx, int cy, int r) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            if (dx*dx + dy*dy > r*r) continue;
            int y = (cy + dy + sh) % sh;
            int x = (cx + dx + sw) % sw;
            gU[y][x] = 0.50 + (rand() / (double)RAND_MAX) * 0.08;
            gV[y][x] = 0.25 + (rand() / (double)RAND_MAX) * 0.08;
        }
}

static void init_grid(void) {
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            gU[y][x] = 1.0;
            gV[y][x] = 0.0;
        }
    /* Central seed */
    seed_circle(sw / 2, sh / 2, 8);
    /* Scattered random seeds */
    for (int i = 0; i < 8; i++)
        seed_circle(10 + rand() % (sw - 20),
                    5  + rand() % (sh - 10),
                    3  + rand() % 5);
}

/* 5-point Laplacian with toroidal wrapping */
static inline double lap5(double g[MAXH][MAXW], int y, int x) {
    return g[(y - 1 + sh) % sh][x]
         + g[(y + 1) % sh][x]
         + g[y][(x - 1 + sw) % sw]
         + g[y][(x + 1) % sw]
         - 4.0 * g[y][x];
}

/* Advance one timestep (forward Euler, dt = 1) */
static void sim_step(void) {
    double f = presets[cur_pre].f;
    double k = presets[cur_pre].k;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            double u = gU[y][x], v = gV[y][x];
            double uvv = u * v * v;
            double nu = u + Du * lap5(gU, y, x) - uvv + f * (1.0 - u);
            double nv = v + Dv * lap5(gV, y, x) + uvv - (f + k) * v;
            bU[y][x] = nu < 0 ? 0 : nu > 1 ? 1 : nu;
            bV[y][x] = nv < 0 ? 0 : nv > 1 ? 1 : nv;
        }
    /* Copy buffer → current (row by row for non-contiguous layout) */
    for (int y = 0; y < sh; y++) {
        memcpy(gU[y], bU[y], sizeof(double) * sw);
        memcpy(gV[y], bV[y], sizeof(double) * sw);
    }
}

/* ── Rendering ── */
static char outbuf[MAXW * 2 * MAXH * 20 + 1024];

static void render(void) {
    int pos = 0;

    /* Cursor home */
    memcpy(outbuf + pos, "\033[H", 3); pos += 3;

    /* Render grid: each cell = 2 spaces with background color */
    for (int y = 0; y < sh; y++) {
        int prev_c = -1;
        for (int x = 0; x < sw; x++) {
            /* Map V concentration to palette index (V typically 0..0.5) */
            int idx = (int)(gV[y][x] * 550.0);
            if (idx < 0)   idx = 0;
            if (idx > 255) idx = 255;
            int c = cmap[idx];
            if (c != prev_c) {
                /* Only emit color code when it changes (run-length optimization) */
                pos += sprintf(outbuf + pos, "\033[48;5;%dm", c);
                prev_c = c;
            }
            outbuf[pos++] = ' ';
            outbuf[pos++] = ' ';
        }
        /* End row: reset + newline */
        memcpy(outbuf + pos, "\033[0m\n", 5); pos += 5;
    }

    /* HUD bar */
    pos += sprintf(outbuf + pos,
        "\033[0m\033[7m Palette:%-6s| Preset %d/%d: %-16s| "
        "f=%.4f k=%.4f | Steps:%2d | %s | "
        "[Space]Pause [1-6]Preset [P]Palette [+/-]Speed [R]Reset [S]eed [Q]uit"
        "\033[K\033[0m",
        pal_names[cur_pal],
        cur_pre + 1, NPRES, presets[cur_pre].name,
        presets[cur_pre].f, presets[cur_pre].k,
        steps,
        paused ? "PAUSED " : "RUNNING");

    ssize_t wr __attribute__((unused)) = write(STDOUT_FILENO, outbuf, pos);
}

/* ── Input handling ── */
static void read_mouse(void) {
    unsigned char mb, mx, my;
    if (read(STDIN_FILENO, &mb, 1) != 1) return;
    if (read(STDIN_FILENO, &mx, 1) != 1) return;
    if (read(STDIN_FILENO, &my, 1) != 1) return;
    int button = mb - 32;
    if (button != 0) return; /* left click only */
    int col = (int)mx - 33;  /* 0-based terminal column */
    int row = (int)my - 33;  /* 0-based terminal row */
    int sx = col / 2;        /* convert to grid coords */
    int sy = row;
    if (sx >= 0 && sx < sw && sy >= 0 && sy < sh)
        seed_circle(sx, sy, 5);
}

static void handle_input(void) {
    unsigned char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == '\033') {
            unsigned char c2;
            if (read(STDIN_FILENO, &c2, 1) != 1) {
                /* Standalone Escape */
                exit(0);
            }
            if (c2 == '[') {
                unsigned char c3;
                if (read(STDIN_FILENO, &c3, 1) != 1) return;
                if (c3 == 'M') {
                    read_mouse();  /* xterm mouse: \033[Mbxy */
                }
                /* Arrow keys and other sequences: consume and ignore */
            }
            continue;
        }
        switch (c) {
        case ' ':  paused = !paused;                                break;
        case 'r': case 'R': init_grid(); frame = 0;                 break;
        case 'c': case 'C':
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < sw; x++)
                    { gU[y][x] = 1.0; gV[y][x] = 0.0; }
            break;
        case 's': case 'S':
            seed_circle(10 + rand() % (sw - 20),
                        5  + rand() % (sh - 10), 5 + rand() % 5);
            break;
        case 'p': case 'P':
            cur_pal = (cur_pal + 1) % NPAL;
            build_palette(cur_pal);
            break;
        case '+': case '=':
            if (steps < 40) steps += 2;
            break;
        case '-': case '_':
            if (steps > 2) steps -= 2;
            break;
        case 'q': case 'Q':
            exit(0);
        default:
            if (c >= '1' && c <= '0' + NPRES) {
                cur_pre = c - '1';
                init_grid();
                frame = 0;
            }
            break;
        }
    }
}

/* ── Main ── */
int main(void) {
    srand((unsigned)time(NULL));
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Palette */
    build_palette(cur_pal);

    /* Terminal */
    term_init();
    signal(SIGWINCH, on_resize);

    /* Grid sizing */
    resize_grid();

    /* Initial state */
    init_grid();

    /* Clear screen */
    fprintf(stderr, "\033[2J");

    /* Main loop */
    while (1) {
        if (got_resize) {
            got_resize = 0;
            resize_grid();
            init_grid();
            frame = 0;
            fprintf(stderr, "\033[2J");
        }

        handle_input();

        if (!paused) {
            for (int i = 0; i < steps; i++)
                sim_step();
            frame++;
        }

        render();
        usleep(30000);  /* ~30 fps target */
    }
}
