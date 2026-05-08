/*
 * ============================================================
 *  RAYCASTER - Terminal Pseudo-3D Raycasting Engine
 * ============================================================
 *  A Wolfenstein 3D-style first-person renderer using DDA
 *  raycasting with ANSI 256-color gradients and ASCII shading.
 *
 *  Compile:
 *    gcc -O2 -o raycaster raycaster-5013.c -lm
 *    (or: cc -O2 raycaster-5013.c -o raycaster -lm)
 *
 *  Run:
 *    ./raycaster
 *
 *  Controls:
 *    W / Up     Move forward       S / Down   Move backward
 *    A / Left   Turn left          D / Right  Turn right
 *    M          Toggle minimap     Q / Esc    Quit
 *
 *  Requires:
 *    - ANSI/VT100 terminal with 256-color support
 *    - Terminal size >= 100 x 38 characters
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <termios.h>
#include <time.h>
#include <sys/select.h>
#include <signal.h>

/* ── Configuration ─────────────────────────────────────── */
#define MAPW     24
#define MAPH     24
#define SCR_W    100
#define SCR_H    36
#define MOVE_SPD 0.08
#define ROT_SPD  0.05
#define MM_RAD   5          /* minimap half-size */

/* ── World Map ─────────────────────────────────────────── */
/* 0 = empty, 1-5 = coloured walls                         */
static int wmap[MAPH][MAPW] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,2,2,2,2,0,0,0,0,0,0,3,3,3,3,3,0,0,0,0,1},
    {1,0,0,0,2,0,0,2,0,0,0,0,0,0,3,0,0,0,3,0,0,0,0,1},
    {1,0,0,0,2,0,0,2,0,0,0,0,0,0,3,0,0,0,3,0,0,0,0,1},
    {1,0,0,0,2,0,0,0,0,0,0,0,0,0,3,0,0,0,3,0,0,0,0,1},
    {1,0,0,0,2,2,2,2,0,0,0,0,0,0,3,3,3,3,3,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,4,4,4,4,4,4,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,4,0,0,0,0,4,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,4,0,0,0,0,4,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,4,0,0,0,0,4,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,4,4,4,0,4,4,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,5,5,5,5,0,0,0,0,0,0,0,0,0,0,2,0,0,2,0,0,1},
    {1,0,0,5,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,0,2,0,0,1},
    {1,0,0,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,5,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,0,2,0,0,1},
    {1,0,0,5,5,5,5,0,0,0,0,0,0,0,0,0,0,2,2,2,2,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

/* Wall-shading characters (near -> far) */
static const char shade_ch[] = "@%#*+-.";
#define N_SHADE 7

/* ── Player State ──────────────────────────────────────── */
static double px = 2.5,  py = 2.5;       /* position        */
static double dx = 1.0,  dy = 0.0;       /* direction       */
static double plx = 0.0, ply = 0.66;     /* camera plane    */

/* ── Terminal / Runtime State ──────────────────────────── */
static struct termios orig_term;
static volatile sig_atomic_t running = 1;
static int show_map = 1;
static int fps_val  = 0;

/* Large output buffer so printf calls don't trigger per-char writes */
static char outbuf[131072];

static void handle_sig(int s) { (void)s; running = 0; }

static void raw_on(void)
{
    struct termios t;
    tcgetattr(STDIN_FILENO, &orig_term);
    t = orig_term;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    setvbuf(stdout, outbuf, _IOFBF, sizeof(outbuf));
}

static void raw_off(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
    printf("\033[?25h\033[0m\r\n");
    fflush(stdout);
}

static int kbhit(void)
{
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

/* Read one key; arrow keys are mapped to WASD */
static int read_key(void)
{
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return 0;
    if (c == 27) {                       /* ESC sequence */
        unsigned char s1, s2;
        if (read(STDIN_FILENO, &s1, 1) != 1) return 27;
        if (read(STDIN_FILENO, &s2, 1) != 1) return 27;
        if (s1 == '[') {
            switch (s2) {
            case 'A': return 'w';
            case 'B': return 's';
            case 'C': return 'd';
            case 'D': return 'a';
            }
        }
        return 27;
    }
    return tolower(c);
}

/* ── ANSI Colour Helpers ───────────────────────────────── */

/* Base foreground colour per wall type (ANSI 30-37) */
static const int wall_base[] = { 0, 31, 32, 34, 33, 36 };

static void set_wall_fg(int type, int side, double dist)
{
    int fg = (type >= 1 && type <= 5) ? wall_base[type] : 37;
    /* E/W walls (side 0) and distant walls rendered dimmer */
    if (side == 0 || dist > 8.0)
        printf("\033[2;%dm", fg);
    else
        printf("\033[1;%dm", fg);
}

static char wall_char(double dist)
{
    int i = (int)(dist * 0.9);
    if (i >= N_SHADE) i = N_SHADE - 1;
    return shade_ch[i];
}

/* ── Raycasting Render ─────────────────────────────────── */

/* Per-column raycast result */
typedef struct {
    int    top, bot;     /* wall strip boundaries */
    int    type, side;   /* wall type & hit side   */
    double dist;         /* perpendicular distance */
} Col;

static void render(void)
{
    static Col cols[SCR_W];
    int half = SCR_H / 2;

    /* ── Cast one ray per screen column ────────────── */
    for (int x = 0; x < SCR_W; x++) {
        double cx  = 2.0 * x / SCR_W - 1.0;
        double rdx = dx + plx * cx;
        double rdy = dy + ply * cx;

        int mx = (int)px, my = (int)py;
        double ddx = (rdx == 0) ? 1e30 : fabs(1.0 / rdx);
        double ddy = (rdy == 0) ? 1e30 : fabs(1.0 / rdy);
        double sdx, sdy;
        int    sx, sy, side = 0;

        if (rdx < 0) { sx = -1; sdx = (px - mx) * ddx; }
        else          { sx =  1; sdx = (mx + 1.0 - px) * ddx; }
        if (rdy < 0) { sy = -1; sdy = (py - my) * ddy; }
        else          { sy =  1; sdy = (my + 1.0 - py) * ddy; }

        int hit = 0;
        while (!hit) {
            if (sdx < sdy) { sdx += ddx; mx += sx; side = 0; }
            else            { sdy += ddy; my += sy; side = 1; }
            if (mx < 0 || mx >= MAPW || my < 0 || my >= MAPH) break;
            hit = wmap[my][mx];
        }

        double pwd;
        if (side == 0) pwd = (mx - px + (1 - sx) / 2.0) / rdx;
        else           pwd = (my - py + (1 - sy) / 2.0) / rdy;
        if (pwd < 0.01) pwd = 0.01;

        int lh = (int)(SCR_H / pwd);
        int ds = -lh / 2 + half;
        int de =  lh / 2 + half;
        if (ds < 0)      ds = 0;
        if (de >= SCR_H)  de = SCR_H - 1;

        cols[x].top  = ds;
        cols[x].bot  = de;
        cols[x].type = (mx >= 0 && mx < MAPW && my >= 0 && my < MAPH)
                       ? wmap[my][mx] : 1;
        cols[x].side = side;
        cols[x].dist = pwd;
    }

    /* ── Draw frame ────────────────────────────────── */
    printf("\033[H");

    for (int y = 0; y < SCR_H; y++) {
        for (int x = 0; x < SCR_W; x++) {
            if (y < cols[x].top) {
                /* Ceiling: dark blue gradient (256-color bg) */
                int shade = (int)((double)y / half * 4.0);
                if (shade > 3) shade = 3;
                printf("\033[48;5;%dm ", 16 + shade);
            } else if (y <= cols[x].bot) {
                /* Wall */
                set_wall_fg(cols[x].type, cols[x].side, cols[x].dist);
                putchar(wall_char(cols[x].dist));
            } else {
                /* Floor: grey-brown gradient (256-color bg) */
                int shade = (int)((double)(y - half) / half * 6.0);
                if (shade < 0) shade = 0;
                if (shade > 6) shade = 6;
                printf("\033[48;5;%dm ", 232 + shade);
            }
        }
        printf("\033[0m\n");
    }

    /* ── Minimap overlay (top-right) ───────────────── */
    if (show_map) {
        int mm_size = MM_RAD * 2 + 1;
        int mm_x    = SCR_W - mm_size - 2;

        /* Border */
        printf("\033[1;%dH\033[1;37m+", mm_x);
        for (int i = 0; i < mm_size; i++) putchar('-');
        putchar('+');

        for (int r = -MM_RAD; r <= MM_RAD; r++) {
            int wy = (int)py + r;
            printf("\033[%d;%dH\033[1;37m|", r + MM_RAD + 2, mm_x);
            for (int c = -MM_RAD; c <= MM_RAD; c++) {
                int wx = (int)px + c;
                if (wx == (int)px && wy == (int)py) {
                    /* Player dot with direction indicator */
                    double a = atan2(dy, dx);
                    int ci = (int)round(cos(a));
                    int ri = (int)round(sin(a));
                    if (c == ci && r == ri)
                        printf("\033[1;32m>");
                    else
                        printf("\033[1;32m@");
                } else if (wx >= 0 && wx < MAPW && wy >= 0 && wy < MAPH) {
                    if (wmap[wy][wx]) {
                        /* Coloured wall on minimap */
                        int fg = (wmap[wy][wx] >= 1 && wmap[wy][wx] <= 5)
                                 ? wall_base[wmap[wy][wx]] : 37;
                        printf("\033[1;%dm#", fg);
                    } else {
                        printf("\033[40;37m.");
                    }
                } else {
                    printf("\033[40;30m ");
                }
            }
            printf("\033[1;37m|");
        }

        printf("\033[%d;%dH+", MM_RAD * 2 + 3, mm_x);
        for (int i = 0; i < mm_size; i++) putchar('-');
        putchar('+');
    }

    /* ── HUD bar ───────────────────────────────────── */
    printf("\033[%d;1H", SCR_H + 1);
    double angle = atan2(dy, dx) * 180.0 / M_PI;
    if (angle < 0) angle += 360.0;

    /* Compass direction */
    const char *dirs[] = {"E","SE","S","SW","W","NW","N","NE"};
    int di = (int)round(angle / 45.0) % 8;

    printf("\033[1;37m Pos(%.1f,%.1f) %s %.0f°  FPS:%d",
            px, py, dirs[di], angle, fps_val);
    printf("\033[%dG\033[2;37m[WASD]Move [M]Map [Q]Quit",
            SCR_W - 28);

    fflush(stdout);
}

/* ── Title Screen ──────────────────────────────────────── */
static void title_screen(void)
{
    printf("\033[2J\033[H");
    printf("\033[1;36m");
    printf("  +=====================================================+\n");
    printf("  |                                                     |\n");
    printf("  |      \033[1;33mR A Y C A S T E R   E N G I N E\033[1;36m            |\n");
    printf("  |                                                     |\n");
    printf("  |   \033[0;37mA Wolfenstein 3D-style pseudo-3D renderer\033[1;36m     |\n");
    printf("  |                                                     |\n");
    printf("  |                                                     |\n");
    printf("  |   \033[0;37mExplore a dungeon with coloured rooms and    \033[1;36m |\n");
    printf("  |   \033[0;37mcorridors, rendered entirely in ASCII text.   \033[1;36m |\n");
    printf("  |                                                     |\n");
    printf("  |   \033[1;32mW/\xe2\x86\x91\033[0;37m Forward   \033[1;32mS/\xe2\x86\x93\033[0;37m Backward"
           "   \033[1;32mA/\xe2\x86\x90\033[0;37m Left   \033[1;32mD/\xe2\x86\x92\033[0;37m Right\033[1;36m  |\n");
    printf("  |   \033[1;32mM\033[0;37m Toggle Map   \033[1;32mQ/Esc\033[0;37m Quit"
           "                      \033[1;36m |\n");
    printf("  |                                                     |\n");
    printf("  +=====================================================+\n");
    printf("\n          \033[1;33mPress any key to start...\033[0m");
    fflush(stdout);

    while (!kbhit()) usleep(50000);
    read_key();
    printf("\033[2J\033[H");
}

/* ── FPS Counter ───────────────────────────────────────── */
static void update_fps(void)
{
    static struct timespec prev = {0, 0};
    static int count = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    count++;
    double elapsed = (now.tv_sec - prev.tv_sec)
                   + (now.tv_nsec - prev.tv_nsec) / 1e9;
    if (elapsed >= 1.0) {
        fps_val = (int)(count / elapsed);
        count = 0;
        prev  = now;
    }
}

/* ── Main Loop ─────────────────────────────────────────── */
int main(void)
{
    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);
    atexit(raw_off);
    raw_on();

    title_screen();

    while (running) {
        update_fps();
        render();

        /* Process all queued key events */
        while (kbhit()) {
            int k = read_key();
            double npx = px, npy = py;
            int moved = 0;

            switch (k) {
            case 'w':
                npx += dx * MOVE_SPD;
                npy += dy * MOVE_SPD;
                moved = 1;
                break;
            case 's':
                npx -= dx * MOVE_SPD;
                npy -= dy * MOVE_SPD;
                moved = 1;
                break;
            case 'a': {
                double od = dx, op = plx;
                double c = cos(ROT_SPD), s = sin(ROT_SPD);
                dx  = od * c - dy * s;
                dy  = od * s + dy * c;
                plx = op * c - ply * s;
                ply = op * s + ply * c;
                continue;   /* no collision check needed */
            }
            case 'd': {
                double od = dx, op = plx;
                double c = cos(-ROT_SPD), s = sin(-ROT_SPD);
                dx  = od * c - dy * s;
                dy  = od * s + dy * c;
                plx = op * c - ply * s;
                ply = op * s + ply * c;
                continue;
            }
            case 'm':
                show_map = !show_map;
                continue;
            case 'q': case 27:
                running = 0;
                continue;
            default:
                continue;
            }

            /* Collision detection: slide along walls */
            if (moved) {
                int ix = (int)npx, iy = (int)npy;
                int opx = (int)px,  opy = (int)py;
                if (ix >= 0 && ix < MAPW && opy >= 0 && opy < MAPH
                    && wmap[opy][ix] == 0)
                    px = npx;
                if (opx >= 0 && opx < MAPW && iy >= 0 && iy < MAPH
                    && wmap[iy][opx] == 0)
                    py = npy;
            }
        }

        usleep(1000000 / 30);    /* ~30 fps cap */
    }

    printf("\033[2J\033[H\033[1;36mThanks for playing!\033[0m\n");
    return 0;
}
