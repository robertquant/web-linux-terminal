/*
 * clothsim-9722.c — Terminal Verlet Cloth Physics Simulation
 *
 * Simulates cloth using Verlet integration with distance constraints.
 * Features: gravity, wind with turbulence, cloth tearing, strain coloring,
 *           3 scene presets, interactive cursor for tearing.
 *
 * Compile: gcc -O2 -lm clothsim-9722.c -o clothsim
 * Run:     ./clothsim
 *
 * Controls:
 *   Arrow keys   Move cursor
 *   Space/Enter  Tear cloth at cursor
 *   w            Toggle wind
 *   g            Toggle gravity
 *   +/-          Adjust wind strength
 *   1            Scene: Curtain (pinned top)
 *   2            Scene: Flag (pinned left)
 *   3            Scene: Hammock (pinned corners)
 *   r            Reset simulation
 *   q/Esc        Quit
 *
 * Requires: ANSI terminal with 256-color support, POSIX termios
 * License:  Public Domain
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

/* ── Tunables ── */
#define CW       36        /* cloth grid width (particles)  */
#define CH       18        /* cloth grid height (particles) */
#define MAXP     (CW * CH)
#define MAXC     (MAXP * 4)
#define GRAVITY  980.0f    /* px/s^2 */
#define DAMPING  0.990f
#define ITERS    7         /* constraint solver passes */
#define TEAR_MUL 2.3f      /* tear at this × rest length */
#define DT       (1.0f/60.0f)
#define TORN_RAD 3.0f      /* cursor tear radius (px) */

/* ── Data ── */
typedef struct { float x, y, ox, oy; int pinned, alive; } Particle;
typedef struct { int a, b; float rest; int alive; }        Constraint;

static Particle   P[MAXP];
static Constraint C[MAXC];
static int        NC;
static int        TW = 80, TH = 24;     /* terminal size */
static int        cx = 40, cy = 15;     /* cursor pos */
static float      wind_str  = 120.0f;
static int        wind_on, grav_on = 1, scene = 1, running = 1;
static long       frame_cnt;
static struct termios orig_tio;

/* ── Terminal ── */
static void term_raw(void) {
    struct termios t;
    tcgetattr(0, &orig_tio);
    t = orig_tio;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
}
static void term_cooked(void) { tcsetattr(0, TCSANOW, &orig_tio); }

static void term_size(void) {
    struct winsize w;
    ioctl(1, TIOCGWINSZ, &w);
    if (w.ws_col > 20) TW = w.ws_col;
    if (w.ws_row > 10) TH = w.ws_row;
}

/* ── Constraint helpers ── */
static void add_con(int a, int b) {
    if (NC >= MAXC) return;
    float dx = P[b].x - P[a].x, dy = P[b].y - P[a].y;
    C[NC].a = a;  C[NC].b = b;
    C[NC].rest  = sqrtf(dx * dx + dy * dy);
    C[NC].alive = 1;
    NC++;
}

/* ── Init ── */
static void init_cloth(void) {
    NC = 0;
    frame_cnt = 0;

    float sx = (TW - 8) / (float)(CW - 1);
    float sy = (TH - 6) / (float)(CH - 1);
    float sp = sx < sy ? sx : sy;
    if (sp < 0.5f) sp = 0.5f;
    float ox = (TW - sp * (CW - 1)) / 2.0f;
    float oy = 2.0f;

    for (int r = 0; r < CH; r++)
        for (int c = 0; c < CW; c++) {
            int i = r * CW + c;
            P[i].x = P[i].ox = ox + c * sp;
            P[i].y = P[i].oy = oy + r * sp;
            P[i].pinned = 0;
            P[i].alive  = 1;
        }

    /* Pin pattern per scene */
    if (scene == 1) {          /* curtain — top row */
        for (int c = 0; c < CW; c++) P[c].pinned = 1;
    } else if (scene == 2) {   /* flag — left column */
        for (int r = 0; r < CH; r++) P[r * CW].pinned = 1;
    } else {                   /* hammock — top-left & top-right corners + every 4th */
        P[0].pinned = 1;
        P[CW - 1].pinned = 1;
        for (int c = 1; c < CW - 1; c++)
            if (c % 4 == 0) P[c].pinned = 1;
    }

    /* Structural: horizontal + vertical */
    for (int r = 0; r < CH; r++)
        for (int c = 0; c < CW - 1; c++)
            add_con(r * CW + c, r * CW + c + 1);
    for (int r = 0; r < CH - 1; r++)
        for (int c = 0; c < CW; c++)
            add_con(r * CW + c, (r + 1) * CW + c);

    /* Shear: diagonals for stability */
    for (int r = 0; r < CH - 1; r++)
        for (int c = 0; c < CW - 1; c++) {
            add_con(r * CW + c,     (r + 1) * CW + c + 1);
            add_con(r * CW + c + 1, (r + 1) * CW + c);
        }
}

/* ── Physics step ── */
static void physics(void) {
    /* Verlet integration */
    for (int i = 0; i < MAXP; i++) {
        if (!P[i].alive || P[i].pinned) continue;
        float vx = (P[i].x - P[i].ox) * DAMPING;
        float vy = (P[i].y - P[i].oy) * DAMPING;
        P[i].ox = P[i].x;
        P[i].oy = P[i].y;

        float ax = 0.0f;
        float ay = grav_on ? GRAVITY * DT * DT : 0.0f;

        /* Wind with per-particle turbulence */
        if (wind_on) {
            float phase = P[i].y * 0.25f + frame_cnt * 0.04f;
            ax += (wind_str + sinf(phase) * wind_str * 0.5f
                   + sinf(phase * 2.7f) * wind_str * 0.2f) * DT * DT;
        }

        P[i].x += vx + ax;
        P[i].y += vy + ay;

        /* Keep inside terminal */
        if (P[i].x < 1)        { P[i].x = 1;        P[i].ox = 1;        }
        if (P[i].x > TW - 2)   { P[i].x = TW - 2;   P[i].ox = TW - 2;   }
        if (P[i].y < 1)        { P[i].y = 1;        P[i].oy = 1;        }
        if (P[i].y > TH - 2)   { P[i].y = TH - 2;   P[i].oy = TH - 2;   }
    }

    /* Constraint relaxation */
    for (int it = 0; it < ITERS; it++) {
        for (int i = 0; i < NC; i++) {
            if (!C[i].alive) continue;
            Particle *a = &P[C[i].a], *b = &P[C[i].b];
            if (!a->alive || !b->alive) { C[i].alive = 0; continue; }

            float dx = b->x - a->x, dy = b->y - a->y;
            float d  = sqrtf(dx * dx + dy * dy);
            if (d < 0.0001f) d = 0.0001f;

            /* Tear */
            if (d > C[i].rest * TEAR_MUL) { C[i].alive = 0; continue; }

            float corr = (d - C[i].rest) / d * 0.5f;
            if (!a->pinned) { a->x += dx * corr; a->y += dy * corr; }
            if (!b->pinned) { b->x -= dx * corr; b->y -= dy * corr; }
        }
    }
}

/* ── Tear at cursor ── */
static void tear_at(int tx, int ty) {
    for (int i = 0; i < NC; i++) {
        if (!C[i].alive) continue;
        Particle *a = &P[C[i].a], *b = &P[C[i].b];
        float mx = (a->x + b->x) * 0.5f, my = (a->y + b->y) * 0.5f;
        float dx = mx - tx, dy = my - ty;
        if (dx * dx + dy * dy < TORN_RAD * TORN_RAD)
            C[i].alive = 0;
    }
}

/* ── Framebuffer ── */
static char fb_c[300][600];
static int  fb_k[300][600];  /* ANSI-256 color, 0 = default */

static void fb_clear(void) {
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++) {
            fb_c[y][x] = ' ';
            fb_k[y][x] = 0;
        }
}

static void fb_put(int x, int y, char ch, int col) {
    if (x >= 0 && x < TW && y >= 0 && y < TH) {
        fb_c[y][x] = ch;
        fb_k[y][x] = col;
    }
}

/* Bresenham line on framebuffer */
static void fb_line(int x0, int y0, int x1, int y1, char ch, int col) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        fb_put(x0, y0, ch, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Map strain to ANSI-256 color (blue→cyan→green→yellow→red) */
static int strain_col(float s) {
    if (s < 0.6f) return 17;
    if (s < 0.8f) return 25;
    if (s < 0.95f) return 37;
    if (s < 1.05f) return 40;
    if (s < 1.15f) return 113;
    if (s < 1.30f) return 190;
    if (s < 1.50f) return 220;
    if (s < 1.80f) return 208;
    return 196;
}

/* ── Render ── */
static void render(void) {
    fb_clear();

    /* Draw all alive structural constraints (first CW*(CH-1)+(CW-1)*CH of them
       are h+v, rest are diagonal — only draw h+v for clarity) */
    int nv = (CW - 1) * CH;               /* horizontal count */
    int nh = CW * (CH - 1);               /* vertical count */
    int struc = nv + nh;

    for (int i = 0; i < struc; i++) {
        if (!C[i].alive) continue;
        Particle *a = &P[C[i].a], *b = &P[C[i].b];
        if (!a->alive || !b->alive) continue;

        float dx = b->x - a->x, dy = b->y - a->y;
        float d  = sqrtf(dx * dx + dy * dy);
        float strain = d / C[i].rest;

        int col = strain_col(strain);
        char ch;
        if (strain < 1.2f) {
            ch = (i < nv) ? '-' : '|';
        } else {
            ch = (i < nv) ? '~' : '/';
        }

        fb_line((int)(a->x + 0.5f), (int)(a->y + 0.5f),
                (int)(b->x + 0.5f), (int)(b->y + 0.5f), ch, col);
    }

    /* Pinned particles */
    for (int i = 0; i < MAXP; i++)
        if (P[i].pinned && P[i].alive)
            fb_put((int)(P[i].x + 0.5f), (int)(P[i].y + 0.5f), '#', 231);

    /* Cursor */
    if (cx >= 0 && cx < TW && cy >= 0 && cy < TH) {
        fb_c[cy][cx] = '+';
        fb_k[cy][cx] = 15;
    }

    /* ── Output ── */
    printf("\033[H");

    /* Header bar */
    const char *sname = scene == 1 ? "Curtain" : scene == 2 ? "Flag" : "Hammock";
    int alive = 0;
    for (int i = 0; i < NC; i++) if (C[i].alive) alive++;
    printf("\033[1;48;5;236;38;5;252m"
           " ClothSim [%s]  Wind:%s Grav:%s Str:%.0f"
           "  Constraints:%d/%d  Frame:%ld"
           "  [Space]Tear [w]Wind [g]Grav [1-3]Scene [r]Reset [q]Quit"
           "\033[K\033[0m\n",
           sname, wind_on ? "ON " : "OFF", grav_on ? "ON " : "OFF",
           wind_str, alive, NC, frame_cnt);

    int last = -1;
    for (int y = 1; y < TH - 1; y++) {
        for (int x = 0; x < TW; x++) {
            int k = fb_k[y][x];
            if (k != last) { printf("\033[38;5;%dm", k ? k : 236); last = k; }
            putchar(fb_c[y][x]);
        }
        printf("\033[K\n");
    }

    /* Footer */
    printf("\033[38;5;244m Arrow=cursor  Space=tear  w=wind  g=gravity  +/-=wind str"
           "  1/2/3=scene  r=reset  q=quit\033[K\033[0m");
    fflush(stdout);
}

/* ── Input ── */
static void read_keys(void) {
    char buf[16];
    int n = read(0, buf, sizeof(buf));
    for (int i = 0; i < n; i++) {
        if (buf[i] == 'q' || buf[i] == 27) { running = 0; return; }
        if (buf[i] == 'w') wind_on = !wind_on;
        if (buf[i] == 'g') grav_on = !grav_on;
        if (buf[i] == 'r') init_cloth();
        if (buf[i] == ' ') tear_at(cx, cy);
        if (buf[i] == '+' || buf[i] == '=') wind_str += 20;
        if (buf[i] == '-' || buf[i] == '_') wind_str -= 20;
        if (wind_str < 0) wind_str = 0;
        if (buf[i] >= '1' && buf[i] <= '3') { scene = buf[i] - '0'; init_cloth(); }

        /* Arrow keys: ESC [ A/B/C/D */
        if (buf[i] == '\033' && i + 2 < n && buf[i + 1] == '[') {
            char d = buf[i + 2];
            if (d == 'A' && cy > 1)       cy--;
            if (d == 'B' && cy < TH - 2)  cy++;
            if (d == 'C' && cx < TW - 2)  cx++;
            if (d == 'D' && cx > 0)       cx--;
            i += 2;
        }
    }
}

/* ── Main ── */
int main(void) {
    term_size();
    term_raw();
    atexit(term_cooked);
    printf("\033[?25l");          /* hide cursor */
    printf("\033[2J\033[H");     /* clear screen */

    init_cloth();

    struct timespec ts = {0, 16666666};  /* ~60 fps target */
    while (running) {
        read_keys();
        physics();
        render();
        frame_cnt++;
        nanosleep(&ts, NULL);
    }

    printf("\033[?25h\033[2J\033[H");   /* restore cursor, clear */
    term_cooked();
    return 0;
}
