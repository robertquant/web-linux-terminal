/*
 * ============================================================================
 * fallsand-9948.c — Terminal Falling Sand Particle Simulation
 * ============================================================================
 * An interactive particle physics sandbox rendered entirely in your terminal.
 * Place 10 different materials and watch them interact with realistic physics!
 *
 * Materials:
 *   Sand   — Falls, piles up, sinks through liquids
 *   Water  — Flows and spreads, extinguishes fire, grows plants
 *   Stone  — Indestructible static barrier
 *   Fire   — Burns flammable materials, produces smoke, dies over time
 *   Smoke  — Rises and gradually dissipates
 *   Oil    — Flammable liquid, floats on water
 *   Wood   — Static structural material, burns when touched by fire
 *   Lava   — Hot liquid, turns water to steam/stone, ignites everything
 *   Acid   — Corrosive liquid, dissolves most materials
 *   Plant  — Living material, grows upward when watered, flammable
 *
 * Compile:  gcc -std=c11 -O2 -o fallsand fallsand-9948.c
 *      or:  make fallsand
 *
 * Run:      ./fallsand
 *
 * Controls:
 *   Arrow Keys / hjkl  - Move cursor
 *   Space              - Toggle drawing mode
 *   e                  - Toggle eraser mode
 *   1-9, 0             - Select material (1=Sand 2=Water ... 0=Plant)
 *   Tab                - Cycle material forward
 *   + / -              - Increase / decrease brush size
 *   r                  - Toggle rain
 *   p                  - Pause / resume simulation
 *   c                  - Clear all particles
 *   q / Esc            - Quit
 *
 * Requires: POSIX terminal (Linux/macOS), 256-color ANSI support
 * ============================================================================
 */

#define _DEFAULT_SOURCE  /* for nanosleep */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>

/* ========================= Particle Types ========================= */

enum {
    EMPTY = 0,
    SAND,       /* 1  - granular, falls and piles */
    WATER,      /* 2  - liquid, flows sideways */
    STONE,      /* 3  - indestructible solid */
    FIRE,       /* 4  - burns things, rises, has lifetime */
    SMOKE,      /* 5  - rises and fades */
    OIL,        /* 6  - flammable liquid, lighter than water */
    WOOD,       /* 7  - static, burns */
    LAVA,       /* 8  - hot liquid, solidifies in water */
    ACID,       /* 9  - corrosive liquid */
    PLANT,      /* 10 - grows with water, flammable */
    NUM_TYPES
};

/* Display properties per type */
static const struct {
    const char *name;
    int color;          /* ANSI 256-color foreground */
    char ch;            /* primary display char */
} tinfo[NUM_TYPES] = {
    { "Empty", 0,   ' ' },
    { "Sand",  220, '#' },   /* bright yellow */
    { "Water", 33,  '~' },   /* blue */
    { "Stone", 244, '#' },   /* gray */
    { "Fire",  202, '*' },   /* orange */
    { "Smoke", 245, '.' },   /* light gray */
    { "Oil",   100, '~' },   /* olive */
    { "Wood",  130, '#' },   /* brown */
    { "Lava",  196, '~' },   /* red */
    { "Acid",  118, '~' },   /* bright green */
    { "Plant", 34,  '#' },   /* green */
};

/* ========================= Grid Constants ========================= */

#define MAX_W 300
#define MAX_H 150
#define UI_ROWS 2

/* ========================= Global State ========================= */

static unsigned char grid[MAX_H][MAX_W];
static unsigned char life[MAX_H][MAX_W];      /* lifetime for fire/smoke */
static unsigned char moved[MAX_H][MAX_W];      /* per-frame update guard */

static int gw, gh;         /* grid dimensions (terminal-dependent) */
static int cx, cy;         /* cursor position */
static int brush  = 2;     /* brush radius */
static int sel    = SAND;  /* selected material */
static int draw   = 0;     /* drawing mode active */
static int erase  = 0;     /* eraser mode active */
static int rain   = 0;     /* rain toggle */
static int paused = 0;     /* pause toggle */
static int alive  = 1;     /* main loop flag */
static unsigned long frame = 0;

static struct termios orig_term;
static int orig_fl;

/* ========================= Terminal Setup ========================= */

static void term_restore(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_term);
    fcntl(STDIN_FILENO, F_SETFL, orig_fl);
    /* Show cursor, reset attributes, go home */
    printf("\033[?25h\033[0m\033[H\033[2J");
    fflush(stdout);
}

static void term_init(void)
{
    tcgetattr(STDIN_FILENO, &orig_term);
    orig_fl = fcntl(STDIN_FILENO, F_GETFL);
    atexit(term_restore);

    struct termios t = orig_term;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);

    printf("\033[?25l");  /* hide cursor */
    fflush(stdout);
}

static void term_resize(void)
{
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    gw = ws.ws_col;
    gh = ws.ws_row - UI_ROWS;
    if (gw > MAX_W) gw = MAX_W;
    if (gh > MAX_H) gh = MAX_H;
    if (gw < 10) gw = 10;
    if (gh < 5)  gh = 5;
    if (cx >= gw) cx = gw / 2;
    if (cy >= gh) cy = gh / 2;
}

/* ========================= Grid Helpers ========================= */

static inline int inb(int y, int x)
{
    return x >= 0 && x < gw && y >= 0 && y < gh;
}

static inline int get(int y, int x)
{
    return inb(y, x) ? grid[y][x] : STONE;
}

static void gswap(int y1, int x1, int y2, int x2)
{
    unsigned char g = grid[y1][x1];
    grid[y1][x1] = grid[y2][x2];
    grid[y2][x2] = g;

    unsigned char l = life[y1][x1];
    life[y1][x1] = life[y2][x2];
    life[y2][x2] = l;

    moved[y2][x2] = 1;
}

/* Density-based displacement: can 'mover' push 'target' out of the way? */
static int can_push(int mover, int target)
{
    if (target == EMPTY) return 1;
    if (target == STONE || target == WOOD || target == PLANT) return 0;

    /* Heavy particles push aside light/gaseous ones */
    if ((target == SMOKE || target == FIRE)
        && mover != SMOKE && mover != FIRE)
        return 1;

    /* Sand sinks through all liquids */
    if (mover == SAND
        && (target == WATER || target == OIL || target == LAVA || target == ACID))
        return 1;

    /* Denser liquids displace lighter ones */
    if (mover == WATER && target == OIL)  return 1;
    if (mover == LAVA  && (target == WATER || target == OIL)) return 1;
    if (mover == ACID  && (target == WATER || target == OIL)) return 1;

    return 0;
}

static void try_move(int y, int x, int ny, int nx)
{
    if (!inb(ny, nx) || moved[ny][nx]) return;
    int t = grid[ny][nx];
    if (t == EMPTY || can_push(grid[y][x], t))
        gswap(y, x, ny, nx);
}

/* ========================= Brush / Painting ========================= */

static void paint(int py, int px, int type, int r)
{
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) continue;
            int ny = py + dy, nx = px + dx;
            if (!inb(ny, nx)) continue;
            /* Only place into empty cells (or erase any cell) */
            if (type == EMPTY || grid[ny][nx] == EMPTY) {
                grid[ny][nx] = (unsigned char)type;
                life[ny][nx] = 0;
                if (type == FIRE)  life[ny][nx] = 18 + rand() % 25;
                if (type == SMOKE) life[ny][nx] = 25 + rand() % 35;
            }
        }
    }
}

/* ========================= Physics: Chemical Interactions ========================= */

/* Return 1 if the calling particle was consumed and should skip movement. */
static int interact(int y, int x, int me)
{
    static const int dx[] = {-1, 1, 0, 0};
    static const int dy[] = { 0, 0,-1, 1};

    for (int d = 0; d < 4; d++) {
        int ny = y + dy[d], nx = x + dx[d];
        if (!inb(ny, nx)) continue;
        int n = grid[ny][nx];

        /* ---- Fire interactions ---- */
        if (me == FIRE) {
            if (n == WATER) {
                grid[y][x]   = SMOKE; life[y][x]   = 12 + rand() % 15;
                grid[ny][nx] = EMPTY;
                moved[ny][nx] = 1;
                return 1;  /* fire consumed */
            }
            if ((n == WOOD || n == OIL || n == PLANT) && rand() % 10 == 0) {
                grid[ny][nx] = FIRE;
                life[ny][nx] = 15 + rand() % 20;
                moved[ny][nx] = 1;
            }
        }

        /* ---- Lava interactions ---- */
        if (me == LAVA) {
            if (n == WATER) {
                grid[y][x]   = STONE;
                grid[ny][nx] = SMOKE;
                life[ny][nx] = 10 + rand() % 15;
                moved[y][x] = moved[ny][nx] = 1;
                return 1;
            }
            if ((n == WOOD || n == OIL || n == PLANT) && rand() % 12 == 0) {
                grid[ny][nx] = FIRE;
                life[ny][nx] = 15 + rand() % 20;
                moved[ny][nx] = 1;
            }
        }

        /* ---- Acid interactions ---- */
        if (me == ACID) {
            if (n != EMPTY && n != STONE && n != ACID && n != WATER
                && rand() % 18 == 0) {
                grid[ny][nx] = SMOKE;
                life[ny][nx] = 6 + rand() % 10;
                moved[ny][nx] = 1;
                /* Acid is partially consumed */
                if (rand() % 3 == 0) {
                    grid[y][x] = EMPTY;
                    return 1;
                }
            }
        }

        /* ---- Plant interactions ---- */
        if (me == PLANT && n == WATER && rand() % 70 == 0) {
            grid[ny][nx] = EMPTY;   /* consume water */
            /* Grow upward (or sideways) */
            int gy = y - 1;
            int gx = x + (rand() % 3 - 1);
            if (inb(gy, gx) && grid[gy][gx] == EMPTY) {
                grid[gy][gx] = PLANT;
                moved[gy][gx] = 1;
            }
        }
    }
    return 0;
}

/* ========================= Physics: Movement ========================= */

static void upd_sand(int y, int x)
{
    int d = rand() % 2 ? -1 : 1;

    /* Straight down */
    if (can_push(SAND, get(y + 1, x)))
        { try_move(y, x, y + 1, x); return; }
    /* Diagonal down */
    if (can_push(SAND, get(y + 1, x + d)))
        { try_move(y, x, y + 1, x + d); return; }
    if (can_push(SAND, get(y + 1, x - d)))
        try_move(y, x, y + 1, x - d);
}

static void upd_liquid(int y, int x, int me)
{
    int d = rand() % 2 ? -1 : 1;

    /* Fall straight down */
    if (can_push(me, get(y + 1, x)))
        { try_move(y, x, y + 1, x); return; }
    /* Diagonal down */
    if (can_push(me, get(y + 1, x + d)))
        { try_move(y, x, y + 1, x + d); return; }
    if (can_push(me, get(y + 1, x - d)))
        { try_move(y, x, y + 1, x - d); return; }

    /* Flow sideways — move one cell in a random horizontal direction */
    if (inb(y, x + d) && grid[y][x + d] == EMPTY && !moved[y][x + d])
        { gswap(y, x, y, x + d); return; }
    if (inb(y, x - d) && grid[y][x - d] == EMPTY && !moved[y][x - d])
        gswap(y, x, y, x - d);
}

static void upd_fire(int y, int x)
{
    if (life[y][x] > 0) life[y][x]--;
    if (life[y][x] == 0) {
        /* Fire dies — may leave smoke */
        if (rand() % 3 == 0) {
            grid[y][x] = SMOKE;
            life[y][x] = 18 + rand() % 25;
        } else {
            grid[y][x] = EMPTY;
        }
        return;
    }

    if (interact(y, x, FIRE)) return;

    /* Rise with random drift */
    int d = rand() % 2 ? -1 : 1;
    if (rand() % 3 == 0 && inb(y - 1, x + d) && grid[y - 1][x + d] == EMPTY)
        gswap(y, x, y - 1, x + d);
    else if (inb(y - 1, x) && grid[y - 1][x] == EMPTY)
        gswap(y, x, y - 1, x);
    else if (rand() % 2 == 0 && inb(y, x + d) && grid[y][x + d] == EMPTY)
        gswap(y, x, y, x + d);
}

static void upd_smoke(int y, int x)
{
    if (life[y][x] > 0) life[y][x]--;
    if (life[y][x] == 0) { grid[y][x] = EMPTY; return; }

    int d = rand() % 2 ? -1 : 1;
    if (rand() % 4 == 0 && inb(y - 1, x + d) && grid[y - 1][x + d] == EMPTY)
        gswap(y, x, y - 1, x + d);
    else if (inb(y - 1, x) && grid[y - 1][x] == EMPTY)
        gswap(y, x, y - 1, x);
    else if (rand() % 3 == 0 && inb(y, x + d) && grid[y][x + d] == EMPTY)
        gswap(y, x, y, x + d);
}

/* ========================= Main Physics Step ========================= */

static void physics(void)
{
    memset(moved, 0, sizeof(moved));

    /* Randomize horizontal scan direction each frame to reduce bias */
    int ltr = rand() % 2;

    /* Bottom-to-top for falling particles; rising particles also handled */
    for (int y = gh - 1; y >= 0; y--) {
        int x0 = ltr ? 0 : gw - 1;
        int x1 = ltr ? gw : -1;
        int dx = ltr ? 1 : -1;

        for (int x = x0; x != x1; x += dx) {
            if (moved[y][x] || grid[y][x] == EMPTY) continue;

            switch (grid[y][x]) {
            case SAND:  upd_sand(y, x);  break;
            case WATER: upd_liquid(y, x, WATER); break;
            case OIL:   upd_liquid(y, x, OIL);   break;
            case STONE: break;
            case WOOD:  break;
            case FIRE:  upd_fire(y, x);  break;
            case SMOKE: upd_smoke(y, x); break;
            case LAVA:
                /* Lava is slower than water — moves every other frame */
                if (!interact(y, x, LAVA) && rand() % 2 == 0)
                    upd_liquid(y, x, LAVA);
                break;
            case ACID:
                if (!interact(y, x, ACID))
                    upd_liquid(y, x, ACID);
                break;
            case PLANT:
                interact(y, x, PLANT);
                break;
            }
        }
    }

    /* Rain: drop water from the sky */
    if (rain && frame % 2 == 0) {
        int rx = rand() % gw;
        if (grid[0][rx] == EMPTY)
            grid[0][rx] = WATER;
    }
}

/* ========================= Rendering ========================= */

static void render(void)
{
    /* Allocate a generous output buffer */
    size_t bsz = (size_t)(gw * gh) * 16 + 4096;
    char *buf = malloc(bsz);
    if (!buf) return;
    int p = 0;

    p += sprintf(buf + p, "\033[H");  /* cursor home */

    for (int y = 0; y < gh; y++) {
        int last_color = -1;

        for (int x = 0; x < gw; x++) {
            int t = grid[y][x];
            int dx2 = x - cx, dy2 = y - cy;
            int in_brush = (dx2 * dx2 + dy2 * dy2 <= brush * brush + 1);

            if (t == EMPTY) {
                /* Show brush outline with dim character */
                if (in_brush && (frame % 20 < 14)) {
                    if (last_color != 240) {
                        p += sprintf(buf + p, "\033[38;5;240m");
                        last_color = 240;
                    }
                    buf[p++] = '+';
                } else {
                    if (last_color != 0) {
                        p += sprintf(buf + p, "\033[0m");
                        last_color = 0;
                    }
                    buf[p++] = ' ';
                }
            } else {
                int c = tinfo[t].color;
                char ch = tinfo[t].ch;

                /* Fire flicker — randomize color and char each frame */
                if (t == FIRE) {
                    c = 196 + rand() % 24;   /* red → yellow range */
                    ch = "*^#~"[rand() % 4];
                }
                /* Lava shimmer */
                if (t == LAVA) {
                    c = rand() % 2 ? 196 : 208;
                    ch = "~#"[rand() % 2];
                }
                /* Water shimmer (subtle) */
                if (t == WATER && rand() % 30 == 0) {
                    ch = '=';
                }

                if (c != last_color) {
                    p += sprintf(buf + p, "\033[38;5;%dm", c);
                    last_color = c;
                }
                buf[p++] = ch;
            }
        }

        /* Reset at end of line */
        if (last_color != 0)
            p += sprintf(buf + p, "\033[0m");
        buf[p++] = '\n';
    }

    /* ---- Material selection bar ---- */
    p += sprintf(buf + p, "\033[48;5;235m\033[38;5;252m ");
    for (int i = 1; i < NUM_TYPES; i++) {
        int key = i % 10;  /* display key: 1-9, 0 */
        if (i == sel) {
            p += sprintf(buf + p,
                "\033[48;5;%dm\033[38;5;16m[%d:%s]\033[48;5;235m\033[38;5;252m ",
                tinfo[i].color, key, tinfo[i].name);
        } else {
            p += sprintf(buf + p, "%d:%s ", key, tinfo[i].name);
        }
    }
    p += sprintf(buf + p, "\033[K\033[0m\n");

    /* ---- Status bar ---- */
    p += sprintf(buf + p,
        "\033[48;5;235m\033[38;5;252m"
        " Brush:%d | %s%s | Rain:%s | Pause:%s | (%d,%d)"
        " | Arrows/hjkl:Move Space:Draw Tab:Cycle +/-:Size r:Rain c:Clear q:Quit"
        "\033[K\033[0m",
        brush,
        draw ? "DRAW" : "    ",
        erase ? " ERASE" : "      ",
        rain ? "ON " : "OFF",
        paused ? "YES" : "NO ",
        cx, cy);

    fwrite(buf, 1, (size_t)p, stdout);
    fflush(stdout);
    free(buf);
}

/* ========================= Input Handling ========================= */

/* Read a single keypress. Returns negative on no input,
   'U'/'D'/'L'/'R' for arrow keys, or the literal character. */
static int readch(void)
{
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) <= 0) return -1;

    if (c == 27) {   /* ESC — start of escape sequence */
        unsigned char s[2];
        if (read(STDIN_FILENO, &s[0], 1) <= 0) return 27;  /* bare ESC */
        if (read(STDIN_FILENO, &s[1], 1) <= 0) return 27;
        if (s[0] == '[') {
            switch (s[1]) {
            case 'A': return 'U';   /* Up    */
            case 'B': return 'D';   /* Down  */
            case 'C': return 'R';   /* Right */
            case 'D': return 'L';   /* Left  */
            }
        }
        return -1;   /* unknown sequence, discard */
    }
    return c;
}

static void handle_input(void)
{
    int k;
    while ((k = readch()) >= 0) {
        switch (k) {
        /* Quit */
        case 'q': alive = 0; return;
        case 27:  alive = 0; return;

        /* Cursor movement */
        case 'U': case 'k': cy = cy > 0      ? cy - 1 : 0;      break;
        case 'D': case 'j': cy = cy < gh - 1  ? cy + 1 : gh - 1; break;
        case 'L': case 'h': cx = cx > 0      ? cx - 1 : 0;      break;
        case 'R': case 'l': cx = cx < gw - 1  ? cx + 1 : gw - 1; break;

        /* Drawing / erasing toggle */
        case ' ': draw  = !draw;  break;
        case 'e': erase = !erase; break;

        /* Material selection */
        case '\t': sel = sel % (NUM_TYPES - 1) + 1; break;
        case '1': sel = SAND;  break;
        case '2': sel = WATER; break;
        case '3': sel = STONE; break;
        case '4': sel = FIRE;  break;
        case '5': sel = SMOKE; break;
        case '6': sel = OIL;   break;
        case '7': sel = WOOD;  break;
        case '8': sel = LAVA;  break;
        case '9': sel = ACID;  break;
        case '0': sel = PLANT; break;

        /* Brush size */
        case '=': case '+': brush = brush < 15 ? brush + 1 : 15; break;
        case '-': case '_': brush = brush > 0  ? brush - 1 : 0;  break;

        /* Toggles */
        case 'r': rain   = !rain;   break;
        case 'p': paused = !paused; break;

        /* Clear grid */
        case 'c':
            memset(grid, 0, sizeof(grid));
            memset(life, 0, sizeof(life));
            break;
        }
    }
}

/* ========================= Main ========================= */

int main(void)
{
    srand((unsigned)time(NULL));
    term_init();
    term_resize();
    cx = gw / 2;
    cy = gh / 2;

    printf("\033[2J");  /* clear screen */

    /* ~30 fps frame timing */
    struct timespec ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = 33000000L;

    while (alive) {
        term_resize();
        handle_input();

        /* Draw particles at cursor when in drawing mode */
        if (draw)
            paint(cy, cx, erase ? EMPTY : sel, brush);

        if (!paused) {
            physics();
            frame++;
        }

        render();
        nanosleep(&ts, NULL);
    }

    return 0;
}
