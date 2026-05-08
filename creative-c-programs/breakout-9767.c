/*
 * BREAKOUT - Terminal Arkanoid / Brick Breaker Game
 *
 * A classic brick-breaking arcade game rendered entirely in the terminal
 * with colorful rainbow bricks, particle explosions, falling power-ups,
 * combo scoring, and 5 unique levels of increasing difficulty.
 *
 * Compile:  gcc -std=c99 -Wall -O2 -o breakout breakout-9767.c -lm
 * Run:      ./breakout
 *
 * Controls:
 *   Left/Right arrows  Move paddle
 *   Space               Launch ball / Start game / Next level
 *   P                   Pause / Resume
 *   Q / Esc             Quit
 *
 * Requires: ANSI-compatible terminal (80x24 minimum), Unix-like OS
 */

/* Enable POSIX extensions (usleep) */
#if !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <signal.h>

/* ─── Constants ──────────────────────────────────────────────────── */
#define SCR_W     80        /* terminal width */
#define SCR_H     24        /* terminal height */
#define BROWS     8         /* brick rows */
#define BCOLS     10        /* brick columns */
#define BW        7         /* brick width in chars */
#define BOX       5         /* brick area X offset */
#define BOY       3         /* brick area Y offset */
#define PAD_W0    12        /* default paddle width */
#define PAD_Y     21        /* paddle Y position */
#define BSPD      0.45f     /* ball base speed */
#define MAX_BALL  5
#define MAX_PART  60
#define MAX_PUP   10
#define NLEVELS   5
#define TICK_US   33333     /* ~30 FPS */

/* ─── ANSI Color Palette ────────────────────────────────────────── */
enum {
    C_DEF, C_RED, C_YEL, C_GRN, C_CYA, C_BLU, C_MAG, C_WHI,
    C_BRED, C_BYEL, C_BGRN, C_BCYA, C_BBLU, C_BMAG, C_BWHI, C_DIM
};

static const char *PAL[] = {
    "\033[0m",            /* C_DEF  */
    "\033[31m",           /* C_RED  */
    "\033[33m",           /* C_YEL  */
    "\033[32m",           /* C_GRN  */
    "\033[36m",           /* C_CYA  */
    "\033[34m",           /* C_BLU  */
    "\033[35m",           /* C_MAG  */
    "\033[37m",           /* C_WHI  */
    "\033[1m\033[91m",    /* C_BRED */
    "\033[1m\033[93m",    /* C_BYEL */
    "\033[1m\033[92m",    /* C_BGRN */
    "\033[1m\033[96m",    /* C_BCYA */
    "\033[1m\033[94m",    /* C_BBLU */
    "\033[1m\033[95m",    /* C_BMAG */
    "\033[1m\033[97m",    /* C_BWHI */
    "\033[2m\033[37m",    /* C_DIM  */
};

/* Brick row colors (rainbow top-to-bottom) */
static const int RCLR[BROWS] = {
    C_BRED, C_BYEL, C_BGRN, C_BCYA, C_BBLU, C_BMAG, C_BWHI, C_RED
};

/* ─── Types ──────────────────────────────────────────────────────── */
typedef struct { float x, y, dx, dy; int active; } Ball;
typedef struct { float x, y, dy; int type, active; } PUp;
typedef struct { float x, y, dx, dy; int life, clr; } Part;
typedef struct { int hp, clr, has_pup, alive; } Brick;

/* ─── Framebuffer ────────────────────────────────────────────────── */
static char fb[SCR_H][SCR_W];
static int  fbc[SCR_H][SCR_W];

static void fb_clear(void)
{
    for (int y = 0; y < SCR_H; y++) {
        memset(fb[y], ' ', SCR_W);
        for (int x = 0; x < SCR_W; x++) fbc[y][x] = C_DEF;
    }
}

static void fb_put(int x, int y, int ch, int clr)
{
    if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H) {
        fb[y][x] = (char)ch;
        fbc[y][x] = clr;
    }
}

static void fb_str(int x, int y, const char *s, int clr)
{
    for (int i = 0; s[i] && x + i < SCR_W; i++)
        fb_put(x + i, y, s[i], clr);
}

static void fb_int(int x, int y, int val, int clr)
{
    char buf[16];
    sprintf(buf, "%d", val);
    fb_str(x, y, buf, clr);
}

/* Render framebuffer to terminal, minimizing color-code changes */
static void fb_flush(void)
{
    printf("\033[H");
    int pc = -1;
    for (int y = 0; y < SCR_H; y++) {
        for (int x = 0; x < SCR_W; x++) {
            if (fbc[y][x] != pc) {
                printf("%s", PAL[fbc[y][x]]);
                pc = fbc[y][x];
            }
            putchar(fb[y][x]);
        }
        if (y < SCR_H - 1) putchar('\n');
        pc = -1;
    }
    printf("\033[0m");
    fflush(stdout);
}

/* ─── Game State ─────────────────────────────────────────────────── */
static struct {
    int px, pw;
    Ball balls[MAX_BALL];
    Brick bricks[BROWS][BCOLS];
    Part parts[MAX_PART];
    PUp  pups[MAX_PUP];
    int nball, npart, npup;
    int score, lives, level, combo, bricks_left;
    int state;          /* 0=title 1=play 2=pause 3=over 4=nextlvl 5=win */
    int pw_timer, slow_timer;
    int launched;
    struct termios orig_t;
} G;

static volatile sig_atomic_t g_running = 1;
static void on_sig(int s) { (void)s; g_running = 0; }

/* ─── Terminal I/O ───────────────────────────────────────────────── */
static void term_init(void)
{
    struct termios t;
    tcgetattr(0, &G.orig_t);
    t = G.orig_t;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    printf("\033[?25l");
    setvbuf(stdout, NULL, _IONBF, 0);
}

static void term_done(void)
{
    tcsetattr(0, TCSANOW, &G.orig_t);
    printf("\033[?25h\033[0m\033[2J\033[H");
}

/* Read one key; returns 0 if none, 'L'/'R'/'U'/'D' for arrows */
static int read_key(void)
{
    unsigned char c;
    if (read(0, &c, 1) != 1) return 0;
    if (c == 27) {
        unsigned char s[2] = {0, 0};
        if (read(0, s, 1) != 1) return 27;
        if (read(0, s + 1, 1) != 1) return 27;
        if (s[0] == '[') {
            switch (s[1]) {
            case 'A': return 'U';
            case 'B': return 'D';
            case 'C': return 'R';
            case 'D': return 'L';
            }
        }
        return 27;
    }
    return c;
}

/* ─── Level Data ─────────────────────────────────────────────────── */
/* 0=empty, 1=normal(1hp), 2=strong(2hp), 3=super(3hp) */
typedef struct { int p[BROWS][BCOLS]; } LvData;

static const LvData ldata[NLEVELS] = {
    /* Level 1: Classic */
    {{{1,1,1,1,1,1,1,1,1,1},
      {1,1,1,1,1,1,1,1,1,1},
      {1,1,1,1,1,1,1,1,1,1},
      {1,1,1,1,1,1,1,1,1,1},
      {1,1,1,1,1,1,1,1,1,1},
      {0,0,0,0,0,0,0,0,0,0},
      {0,0,0,0,0,0,0,0,0,0},
      {0,0,0,0,0,0,0,0,0,0}}},
    /* Level 2: Diamond */
    {{{0,0,0,0,1,1,0,0,0,0},
      {0,0,0,1,2,2,1,0,0,0},
      {0,0,1,2,3,3,2,1,0,0},
      {0,1,2,3,2,2,3,2,1,0},
      {0,0,1,2,3,3,2,1,0,0},
      {0,0,0,1,2,2,1,0,0,0},
      {0,0,0,0,1,1,0,0,0,0},
      {0,0,0,0,0,0,0,0,0,0}}},
    /* Level 3: Fortress */
    {{{2,2,2,2,2,2,2,2,2,2},
      {2,0,0,0,0,0,0,0,0,2},
      {2,0,3,3,3,3,3,3,0,2},
      {2,0,3,0,0,0,0,3,0,2},
      {2,0,3,3,3,3,3,3,0,2},
      {2,0,0,0,0,0,0,0,0,2},
      {2,2,2,2,2,2,2,2,2,2},
      {1,1,1,1,1,1,1,1,1,1}}},
    /* Level 4: Checkerboard */
    {{{3,0,3,0,3,0,3,0,3,0},
      {0,2,0,2,0,2,0,2,0,2},
      {3,0,3,0,3,0,3,0,3,0},
      {0,2,0,2,0,2,0,2,0,2},
      {3,0,3,0,3,0,3,0,3,0},
      {0,2,0,2,0,2,0,2,0,2},
      {3,0,3,0,3,0,3,0,3,0},
      {0,2,0,2,0,2,0,2,0,2}}},
    /* Level 5: The Wall */
    {{{3,3,3,3,3,3,3,3,3,3},
      {3,3,3,3,3,3,3,3,3,3},
      {3,3,3,3,3,3,3,3,3,3},
      {3,3,3,3,3,3,3,3,3,3},
      {3,3,3,3,3,3,3,3,3,3},
      {3,3,3,3,3,3,3,3,3,3},
      {3,3,3,3,3,3,3,3,3,3},
      {3,3,3,3,3,3,3,3,3,3}}},
};

/* ─── Helpers ────────────────────────────────────────────────────── */
static void add_particles(int x, int y, int clr, int n)
{
    for (int i = 0; i < n && G.npart < MAX_PART; i++) {
        Part *p = &G.parts[G.npart++];
        p->x = (float)x;  p->y = (float)y;
        p->dx = ((rand() % 100) - 50) / 25.0f;
        p->dy = ((rand() % 100) - 50) / 25.0f;
        p->life = 12 + rand() % 8;
        p->clr = clr;
    }
}

static void add_powerup(int x, int y)
{
    if (G.npup >= MAX_PUP || rand() % 100 > 25) return;
    static const int types[] = {'W', 'M', 'S', 'L'};
    PUp *p = &G.pups[G.npup++];
    p->x = (float)x;  p->y = (float)y;
    p->dy = 0.25f;  p->active = 1;
    p->type = types[rand() % 4];
}

/* ─── Level / Game Init ──────────────────────────────────────────── */
static void init_level(int lvl)
{
    G.pw = PAD_W0;  G.pw_timer = 0;  G.slow_timer = 0;
    G.combo = 0;    G.launched = 0;
    G.npart = 0;    G.npup = 0;      G.nball = 1;
    G.bricks_left = 0;
    G.px = (SCR_W - G.pw) / 2;

    memset(G.balls, 0, sizeof(G.balls));
    G.balls[0].active = 1;
    G.balls[0].x = (float)(G.px + G.pw / 2);
    G.balls[0].y = (float)(PAD_Y - 1);

    const LvData *ld = &ldata[lvl % NLEVELS];
    for (int r = 0; r < BROWS; r++)
        for (int c = 0; c < BCOLS; c++) {
            int v = ld->p[r][c];
            Brick *b = &G.bricks[r][c];
            b->hp = v;  b->clr = r;
            b->alive = v > 0;
            b->has_pup = (v > 0 && rand() % 100 < 18);
            if (b->alive) G.bricks_left++;
        }
}

static void init_game(void)
{
    int lives = G.lives, score = G.score, level = G.level;
    memset(&G.balls, 0, sizeof(G.balls));
    memset(&G.parts, 0, sizeof(G.parts));
    memset(&G.pups, 0, sizeof(G.pups));
    memset(&G.bricks, 0, sizeof(G.bricks));
    G.lives = 3;  G.level = 0;  G.score = 0;  G.state = 1;
    (void)lives; (void)score; (void)level;
    init_level(0);
}

/* ─── Physics Update ─────────────────────────────────────────────── */
static void update(void)
{
    if (G.state != 1) return;

    float spd = BSPD;
    if (G.slow_timer > 0) { spd *= 0.55f; G.slow_timer--; }
    if (G.pw_timer > 0) {
        G.pw_timer--;
        if (!G.pw_timer) G.pw = PAD_W0;
    }

    for (int i = 0; i < MAX_BALL; i++) {
        Ball *b = &G.balls[i];
        if (!b->active) continue;

        /* Before launch, stick ball to paddle center */
        if (!G.launched) {
            b->x = (float)(G.px + G.pw / 2);
            b->y = (float)(PAD_Y - 1);
            continue;
        }

        float nx = b->x + b->dx;
        float ny = b->y + b->dy;

        /* Wall collisions */
        if (nx < 1)        { nx = 1;          b->dx =  fabsf(b->dx); }
        if (nx > SCR_W-2)  { nx = (float)(SCR_W-2); b->dx = -fabsf(b->dx); }
        if (ny < 1)        { ny = 1;          b->dy =  fabsf(b->dy); }

        /* Paddle collision: angle depends on hit position */
        if (b->dy > 0 && ny >= PAD_Y && ny <= PAD_Y + 1 &&
            nx >= G.px && nx < G.px + G.pw) {
            float hit = (nx - G.px) / (float)G.pw;    /* 0..1 */
            float ang = (hit - 0.5f) * 2.2f;           /* ±1.1 rad */
            float s = sqrtf(b->dx * b->dx + b->dy * b->dy);
            if (s < 0.2f) s = spd;
            b->dx = sinf(ang) * s;
            b->dy = -cosf(ang) * s;
            /* Prevent near-horizontal bounce */
            if (fabsf(b->dy) < 0.18f)
                b->dy = (b->dy < 0) ? -0.18f : 0.18f;
            ny = (float)(PAD_Y - 1);
            G.combo = 0;
        }

        /* Brick collision */
        int br = (int)ny - BOY;
        int bc_raw = (int)nx - BOX;
        if (br >= 0 && br < BROWS && bc_raw >= 0) {
            int bc = bc_raw / BW;
            if (bc < BCOLS) {
                Brick *bk = &G.bricks[br][bc];
                int bleft = BOX + bc * BW;
                if ((int)nx >= bleft && (int)nx < bleft + BW && bk->alive) {
                    /* Determine reflection axis by penetration depth */
                    float mx = (nx < bleft + BW / 2.0f)
                               ? nx - bleft : bleft + BW - nx;
                    float my = (ny < BOY + br + 0.5f)
                               ? ny - (BOY + br) : BOY + br + 1.0f - ny;
                    if (mx < my) b->dx = -b->dx;
                    else         b->dy = -b->dy;

                    bk->hp--;
                    if (bk->hp <= 0) {
                        bk->alive = 0;
                        G.bricks_left--;
                        G.combo++;
                        G.score += 10 * G.combo;
                        add_particles(bleft + BW / 2, BOY + br, bk->clr, 8);
                        if (bk->has_pup)
                            add_powerup(bleft + BW / 2, BOY + br);
                    }
                }
            }
        }

        b->x = nx;  b->y = ny;

        /* Ball lost below screen */
        if (ny > SCR_H + 1) {
            b->active = 0;
            G.nball--;
        }
    }

    /* All balls lost */
    if (G.launched && G.nball <= 0) {
        G.lives--;
        if (G.lives <= 0) { G.state = 3; return; }
        G.launched = 0;  G.nball = 1;
        G.balls[0].active = 1;
        G.balls[0].x = (float)(G.px + G.pw / 2);
        G.balls[0].y = (float)(PAD_Y - 1);
        G.balls[0].dx = G.balls[0].dy = 0;
    }

    /* Level clear */
    if (G.bricks_left <= 0) {
        G.level++;
        G.state = (G.level >= NLEVELS) ? 5 : 4;
    }

    /* Particle physics */
    for (int i = 0; i < G.npart; ) {
        Part *p = &G.parts[i];
        p->x += p->dx;  p->y += p->dy;  p->dy += 0.04f;  p->life--;
        if (p->life <= 0) G.parts[i] = G.parts[--G.npart];
        else i++;
    }

    /* Power-up descent and collection */
    for (int i = 0; i < G.npup; ) {
        PUp *p = &G.pups[i];
        if (!p->active) { G.pups[i] = G.pups[--G.npup]; continue; }
        p->y += p->dy;

        /* Caught by paddle */
        if ((int)p->y >= PAD_Y && (int)p->y <= PAD_Y + 1 &&
            (int)p->x >= G.px && (int)p->x < G.px + G.pw) {
            switch (p->type) {
            case 'W':
                G.pw = PAD_W0 + 8;
                G.pw_timer = 300;
                break;
            case 'M': {
                int added = 0;
                for (int j = 0; j < MAX_BALL && added < 2; j++) {
                    if (!G.balls[j].active) {
                        float a = (float)(added + 1) * 0.9f;
                        G.balls[j].x = G.balls[0].x;
                        G.balls[j].y = G.balls[0].y;
                        G.balls[j].dx = sinf(a) * BSPD;
                        G.balls[j].dy = -cosf(a) * BSPD;
                        G.balls[j].active = 1;
                        G.nball++;  added++;
                    }
                }
                break;
            }
            case 'S': G.slow_timer = 300; break;
            case 'L': if (G.lives < 5) G.lives++; break;
            }
            G.pups[i] = G.pups[--G.npup];
            continue;
        }
        if (p->y > SCR_H) { G.pups[i] = G.pups[--G.npup]; continue; }
        i++;
    }
}

/* ─── Rendering ──────────────────────────────────────────────────── */
static void draw(void)
{
    fb_clear();

    /* ── HUD bar ── */
    fb_str( 0, 0, " BREAKOUT ", C_BWHI);
    fb_str(13, 0, "Score:", C_WHI);
    fb_int(19, 0, G.score, C_BYEL);
    fb_str(27, 0, "Lvl:", C_WHI);
    fb_int(31, 0, G.level + 1, C_BGRN);
    fb_str(37, 0, "Lives:", C_WHI);
    for (int i = 0; i < 5; i++)
        fb_put(43 + i, 0, i < G.lives ? '*' : '.', i < G.lives ? C_BRED : C_DIM);
    fb_str(50, 0, "Combo:", C_WHI);
    fb_int(56, 0, G.combo, G.combo > 3 ? C_BYEL : C_WHI);
    if (G.pw_timer > 0)   fb_str(64, 0, "WIDE!", C_BGRN);
    if (G.slow_timer > 0) fb_str(71, 0, "SLOW!", C_BCYA);

    /* ── Borders ── */
    fb_put(0, 1, '+', C_DIM);
    for (int x = 1; x < SCR_W - 1; x++) fb_put(x, 1, '-', C_DIM);
    fb_put(SCR_W - 1, 1, '+', C_DIM);

    for (int y = 2; y < SCR_H - 2; y++) {
        fb_put(0, y, '|', C_DIM);
        fb_put(SCR_W - 1, y, '|', C_DIM);
    }

    fb_put(0, SCR_H - 2, '+', C_DIM);
    for (int x = 1; x < SCR_W - 1; x++) fb_put(x, SCR_H - 2, '-', C_DIM);
    fb_put(SCR_W - 1, SCR_H - 2, '+', C_DIM);

    /* ── Bricks ── */
    for (int r = 0; r < BROWS; r++)
        for (int c = 0; c < BCOLS; c++) {
            Brick *b = &G.bricks[r][c];
            if (!b->alive) continue;
            int x0 = BOX + c * BW;
            int y0 = BOY + r;
            /* Brick appearance: stronger = denser fill */
            char ch = b->hp >= 3 ? '#' : b->hp == 2 ? '=' : '-';
            int clr = RCLR[r];
            for (int j = 0; j < BW; j++)
                fb_put(x0 + j, y0, ch, clr);
        }

    /* ── Particles ── */
    static const char pglyph[] = ".+*o#@";
    for (int i = 0; i < G.npart; i++) {
        Part *p = &G.parts[i];
        int px = (int)p->x, py = (int)p->y;
        if (px > 0 && px < SCR_W - 1 && py > 1 && py < SCR_H - 2)
            fb_put(px, py, pglyph[p->life % 6], RCLR[p->clr % BROWS]);
    }

    /* ── Power-ups ── */
    for (int i = 0; i < G.npup; i++) {
        PUp *p = &G.pups[i];
        if (!p->active) continue;
        int clr = p->type == 'W' ? C_BGRN : p->type == 'M' ? C_BCYA
                : p->type == 'S' ? C_BBLU : C_BRED;
        int px = (int)p->x, py = (int)p->y;
        if (px > 0 && px + 2 < SCR_W && py > 0 && py < SCR_H - 1) {
            fb_put(px,     py, '[', clr);
            fb_put(px + 1, py, p->type, clr);
            fb_put(px + 2, py, ']', clr);
        }
    }

    /* ── Paddle ── */
    for (int i = 0; i < G.pw; i++)
        fb_put(G.px + i, PAD_Y, '=', C_BWHI);

    /* ── Balls ── */
    for (int i = 0; i < MAX_BALL; i++) {
        Ball *b = &G.balls[i];
        if (!b->active) continue;
        int bx = (int)b->x, by = (int)b->y;
        if (bx > 0 && bx < SCR_W - 1 && by > 1 && by < SCR_H - 2)
            fb_put(bx, by, 'O', C_BWHI);
    }

    /* ── Footer ── */
    fb_str(1, SCR_H - 1,
           " Arrows:Move  Space:Launch  P:Pause  Q:Quit", C_DIM);

    /* ── Overlay screens ── */
    if (G.state == 0) {
        /* Title screen */
        for (int c = 0; c < BCOLS; c++) {
            int x0 = BOX + c * BW;
            for (int j = 0; j < BW; j++)
                fb_put(x0 + j, 4, '#', RCLR[c % BROWS]);
        }
        fb_str(25, 7,  "B R E A K O U T", C_BWHI);
        fb_str(25, 8,  "================", C_BRED);
        fb_str(17, 10, "A Terminal Brick Breaker Game", C_WHI);
        fb_str(12, 12, "Destroy all bricks with the ball!", C_YEL);
        fb_str(12, 13, "Catch power-ups: [W]ide [M]ulti [S]low [L]ife", C_CYA);
        fb_str(20, 15, "Press SPACE to start", C_BYEL);
        fb_str(24, 17, "Q / Esc to quit", C_DIM);
    } else if (G.state == 2) {
        /* Paused */
        fb_str(30, 10, "PAUSED", C_BWHI);
        fb_str(23, 12, "Press P to resume", C_WHI);
    } else if (G.state == 3) {
        /* Game Over */
        fb_str(25, 9,  "G A M E   O V E R", C_BRED);
        fb_str(27, 11, "Final Score:", C_WHI);
        fb_int(40, 11, G.score, C_BYEL);
        fb_str(22, 13, "Press SPACE to continue", C_DIM);
    } else if (G.state == 4) {
        /* Level complete */
        fb_str(23, 9,  "LEVEL COMPLETE!", C_BGRN);
        fb_str(27, 11, "Score:", C_WHI);
        fb_int(33, 11, G.score, C_BYEL);
        fb_str(22, 13, "Press SPACE for next level", C_WHI);
    } else if (G.state == 5) {
        /* Victory */
        fb_str(25, 8,  "Y O U   W I N !", C_BYEL);
        fb_str(23, 10, "All levels cleared!", C_BGRN);
        fb_str(27, 12, "Score:", C_WHI);
        fb_int(33, 12, G.score, C_BYEL);
        fb_str(22, 14, "Press SPACE to continue", C_DIM);
        /* Victory particle burst */
        for (int i = 0; i < SCR_W - 2; i += 4)
            fb_put(1 + i, 6, '*', RCLR[i % BROWS]);
    }

    fb_flush();
}

/* ─── Input Handling ─────────────────────────────────────────────── */
static void handle_input(void)
{
    fd_set fds;
    struct timeval tv = {0, 0};

    while (1) {
        FD_ZERO(&fds);
        FD_SET(0, &fds);
        if (select(1, &fds, NULL, NULL, &tv) <= 0) break;
        int k = read_key();
        if (!k) break;

        switch (G.state) {
        case 0: /* title */
            if (k == ' ' || k == '\n') init_game();
            if (k == 'q' || k == 'Q' || k == 27) g_running = 0;
            break;
        case 1: /* playing */
            if (k == 'L') { if (G.px > 1) G.px -= 2; }
            if (k == 'R') { if (G.px + G.pw < SCR_W - 1) G.px += 2; }
            if (k == ' ' && !G.launched) {
                G.launched = 1;
                float a = ((float)(rand() % 100) / 100.0f - 0.5f) * 0.8f;
                G.balls[0].dx = sinf(a) * BSPD;
                G.balls[0].dy = -cosf(a) * BSPD;
            }
            if (k == 'p' || k == 'P') G.state = 2;
            if (k == 'q' || k == 'Q' || k == 27) g_running = 0;
            break;
        case 2: /* paused */
            if (k == 'p' || k == 'P') G.state = 1;
            if (k == 'q' || k == 'Q' || k == 27) g_running = 0;
            break;
        case 3: /* game over */
        case 5: /* victory */
            if (k == ' ') G.state = 0;
            if (k == 'q' || k == 'Q' || k == 27) g_running = 0;
            break;
        case 4: /* next level */
            if (k == ' ') { init_level(G.level); G.state = 1; }
            break;
        }
    }
}

/* ─── Main ───────────────────────────────────────────────────────── */
int main(void)
{
    srand((unsigned)time(NULL));
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    term_init();
    atexit(term_done);

    /* Default state: title screen */
    G.state = 0;
    G.pw = PAD_W0;
    G.px = (SCR_W - G.pw) / 2;

    while (g_running) {
        handle_input();
        update();
        draw();
        usleep(TICK_US);
    }

    return 0;
}
