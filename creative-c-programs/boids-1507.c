/*
 * boids - Terminal Boids Flocking Simulation
 * ===========================================
 * Craig Reynolds' Boids algorithm — emergent flocking behavior rendered
 * as rainbow-colored directional glyphs in your terminal.
 *
 * Compile:
 *   gcc -O2 -o boids boids-1507.c -lm
 *
 * Run:
 *   ./boids          (default 150 boids)
 *   ./boids 300      (custom count)
 *
 * Controls:
 *   +/=   Add 20 boids        -     Remove 20 boids
 *   S     Toggle Separation   A     Toggle Alignment
 *   C     Toggle Cohesion     P     Cycle predators (0→1→2→3→0)
 *   T     Toggle trails       W     Toggle wind
 *   H     Toggle HUD          [/]   Speed down / up
 *   Space  Pause / Resume     R     Reset
 *   Q/Esc  Quit
 *
 * Requires: ANSI/VT100 terminal with 256-colour support.
 * License:  Public Domain.
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdarg.h>

/* ======================= tunables ======================== */

#define MAX_BOIDS     600
#define MAX_PRED       3
#define TRAIL_LEN      5
#define TGT_FPS       30

#define INIT_BOIDS    150

#define SEP_R   2.5f       /* separation perception radius */
#define ALI_R   5.0f       /* alignment   perception radius */
#define COH_R   5.0f       /* cohesion    perception radius */
#define SEP_W   1.8f       /* separation weight */
#define ALI_W   1.0f       /* alignment   weight */
#define COH_W   1.0f       /* cohesion    weight */
#define MXSPD   1.5f       /* max boid speed */
#define MNSPD   0.4f       /* min boid speed */
#define MXSTR   0.09f      /* max steering force */
#define PRD_R   8.0f       /* predator perception radius */
#define PRD_W   3.5f       /* predator flee weight */
#define WAL_M   3.0f       /* soft-wall margin */
#define WAL_W   0.6f       /* soft-wall steer weight */

/* ======================= types =========================== */

typedef struct {
    float x, y, vx, vy;
    int tx[TRAIL_LEN], ty[TRAIL_LEN];
    int tlen;
} Boid;

typedef struct { float x, y, vx, vy; } Pred;

typedef struct {
    int  w, h;
    struct termios tio_orig;
    Boid  boids[MAX_BOIDS];
    Pred  preds[MAX_PRED];
    int   nb, np;
    int   paused, trails, hud, sep, ali, coh, wind;
    float spd;
    int   frame;
    double fps;
    struct timespec tprev;
} Sim;

/* ======================= globals ========================= */

static Sim        G;
static volatile int g_run = 1;

/* large stdout buffer so one write() per frame → no flicker */
#define OBUFSZ (300 * 1024)
static char *obuf;

/* ======================= output helpers ================== */

static size_t opos;

static inline void oinit(void) { opos = 0; }

static inline void oputs(const char *s)
{
    size_t n = strlen(s);
    memcpy(obuf + opos, s, n);
    opos += n;
}

/* printf into output buffer (safe, grows if needed via initial large alloc) */
static void oprintf(const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (opos + (size_t)n + 1 >= OBUFSZ) { opos = 0; n = 0; } /* overflow guard */
    opos += (size_t)vsnprintf(obuf + opos, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
}

static inline void oflush(void)
{
    ssize_t _wr __attribute__((unused)) = write(STDOUT_FILENO, obuf, opos);
    (void)_wr;
    opos = 0;
}

/* ======================= terminal ======================== */

static void term_restore(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &G.tio_orig);
    fputs("\033[?25h\033[?1049l", stdout);   /* cursor on, main screen */
    fflush(stdout);
}

static void term_init(void)
{
    tcgetattr(STDIN_FILENO, &G.tio_orig);
    atexit(term_restore);

    struct termios t = G.tio_orig;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);

    fputs("\033[?1049h\033[?25l", stdout);   /* alt screen, cursor off */
    fflush(stdout);
}

static void term_size(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        G.w = ws.ws_col;
        G.h = ws.ws_row;
    }
}

/* read one key (non-blocking); returns -1 if nothing */
static int kbget(void)
{
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    return n > 0 ? (int)c : -1;
}

/* ======================= math ============================ */

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static inline float wrapf(float v, float mx)
{
    while (v < 0)    v += mx;
    while (v >= mx)  v -= mx;
    return v;
}

static inline float mag(float x, float y) { return sqrtf(x * x + y * y); }

static inline void limit(float *x, float *y, float m)
{
    float d = mag(*x, *y);
    if (d > m && d > 1e-9f) { *x = *x / d * m; *y = *y / d * m; }
}

/* ======================= colour & glyph ================== */

/* map heading angle → ANSI 256-colour (rainbow) */
static int ang_color(float a)
{
    float h = fmodf(a / (2.0f * (float)M_PI) * 6.0f, 6.0f);
    if (h < 0) h += 6.0f;
    int hi = (int)h;
    float f = h - hi;
    int r, g, b;
    switch (hi) {
    case 0: r=5;           g=(int)(f*5);   b=0;           break;
    case 1: r=(int)((1-f)*5); g=5;         b=0;           break;
    case 2: r=0;           g=5;            b=(int)(f*5);   break;
    case 3: r=0;           g=(int)((1-f)*5); b=5;          break;
    case 4: r=(int)(f*5);  g=0;            b=5;            break;
    default:r=5;           g=0;            b=(int)((1-f)*5); break;
    }
    return 16 + 36*clampf(r,0,5) + 6*clampf(g,0,5) + clampf(b,0,5);
}

/* directional character based on velocity */
static char dir_ch(float vx, float vy)
{
    float a = atan2f(vy, vx);
    if      (a >= -M_PI/8    && a <  M_PI/8)    return '>';
    else if (a >=  M_PI/8    && a <  3*M_PI/8)   return '\\';
    else if (a >=  3*M_PI/8  && a <  5*M_PI/8)   return 'v';
    else if (a >=  5*M_PI/8  && a <  7*M_PI/8)   return '\\';
    else if (a >=  7*M_PI/8  || a < -7*M_PI/8)   return '<';
    else if (a >= -7*M_PI/8  && a < -5*M_PI/8)   return '/';
    else if (a >= -5*M_PI/8  && a < -3*M_PI/8)   return '^';
    else                                            return '/';
}

/* ======================= boid init ======================= */

static void boid_spawn(Boid *b, float mx, float my)
{
    b->x  = (float)rand() / RAND_MAX * mx;
    b->y  = (float)rand() / RAND_MAX * my;
    float a = (float)rand() / RAND_MAX * 2.0f * (float)M_PI;
    float s = MNSPD + (float)rand() / RAND_MAX * (MXSPD - MNSPD);
    b->vx = cosf(a) * s;
    b->vy = sinf(a) * s;
    b->tlen = 0;
}

/* ======================= flocking rules ================== */

static void flock_force(Boid *b, int idx,
                        float *sx, float *sy,
                        float *ax, float *ay,
                        float *cx, float *cy)
{
    *sx = *sy = *ax = *ay = *cx = *cy = 0;
    int sc = 0, ac = 0, cc = 0;

    for (int i = 0; i < G.nb; i++) {
        if (i == idx) continue;
        Boid *o = &G.boids[i];
        float dx = b->x - o->x, dy = b->y - o->y;
        float d2 = dx*dx + dy*dy;

        /* separation */
        if (d2 < SEP_R * SEP_R && d2 > 1e-6f) {
            float d = sqrtf(d2);
            *sx += dx / d;
            *sy += dy / d;
            sc++;
        }
        /* alignment */
        if (d2 < ALI_R * ALI_R) {
            *ax += o->vx;
            *ay += o->vy;
            ac++;
        }
        /* cohesion */
        if (d2 < COH_R * COH_R) {
            *cx += o->x;
            *cy += o->y;
            cc++;
        }
    }

    if (sc) { *sx /= sc; *sy /= sc; }
    if (ac) { *ax /= ac; *ay /= ac; }
    if (cc) { *cx = *cx / cc - b->x; *cy = *cy / cc - b->y; }
}

/* ======================= simulation step ================= */

static void sim_update(void)
{
    if (G.paused) return;

    float W = (float)G.w, H = (float)G.h;
    float spd = G.spd;

    for (int i = 0; i < G.nb; i++) {
        Boid *b = &G.boids[i];

        float sx, sy, ax, ay, cx, cy;
        flock_force(b, i, &sx, &sy, &ax, &ay, &cx, &cy);

        /* scale by weights & toggles */
        sx *= G.sep ? SEP_W : 0;  sy *= G.sep ? SEP_W : 0;
        ax *= G.ali ? ALI_W : 0;  ay *= G.ali ? ALI_W : 0;
        cx *= G.coh ? COH_W : 0;  cy *= G.coh ? COH_W : 0;

        /* predator avoidance */
        float px = 0, py = 0;
        for (int j = 0; j < G.np; j++) {
            float dx = b->x - G.preds[j].x, dy = b->y - G.preds[j].y;
            float d = mag(dx, dy);
            if (d < PRD_R && d > 1e-6f) {
                px += dx / d * PRD_W * (1.0f - d / PRD_R);
                py += dy / d * PRD_W * (1.0f - d / PRD_R);
            }
        }

        /* soft walls */
        float wx = 0, wy = 0;
        if (b->x < WAL_M)          wx += WAL_W * (WAL_M - b->x);
        if (b->x > W - WAL_M)      wx -= WAL_W * (b->x - (W - WAL_M));
        if (b->y < WAL_M)          wy += WAL_W * (WAL_M - b->y);
        if (b->y > H - WAL_M)      wy -= WAL_W * (b->y - (H - WAL_M));

        /* wind */
        if (G.wind) {
            float t = (float)G.frame * 0.008f;
            wx += 0.03f * sinf(t);
            wy += 0.02f * cosf(t * 0.7f);
        }

        /* limit steering forces */
        limit(&sx, &sy, MXSTR);
        limit(&ax, &ay, MXSTR);
        limit(&cx, &cy, MXSTR);
        limit(&px, &py, MXSTR * 2);

        /* apply */
        b->vx += (sx + ax + cx + px + wx) * spd;
        b->vy += (sy + ay + cy + py + wy) * spd;

        /* clamp speed */
        float s = mag(b->vx, b->vy);
        if (s > MXSPD * spd) { b->vx = b->vx/s * MXSPD * spd; b->vy = b->vy/s * MXSPD * spd; }
        if (s < MNSPD && s > 1e-9f) { b->vx = b->vx/s * MNSPD; b->vy = b->vy/s * MNSPD; }

        /* trail */
        if (G.trails) {
            if (b->tlen < TRAIL_LEN) b->tlen++;
            memmove(b->tx + 1, b->tx, (TRAIL_LEN - 1) * sizeof(int));
            memmove(b->ty + 1, b->ty, (TRAIL_LEN - 1) * sizeof(int));
            b->tx[0] = (int)b->x;
            b->ty[0] = (int)b->y;
        } else {
            b->tlen = 0;
        }

        /* integrate */
        b->x += b->vx * spd;
        b->y += b->vy * spd;
        b->x = wrapf(b->x, W);
        b->y = wrapf(b->y, H);
    }

    /* update predators — figure-8 patrol paths */
    for (int j = 0; j < G.np; j++) {
        Pred *p = &G.preds[j];
        float t = (float)G.frame * 0.006f + j * 2.094f;   /* 120° offset */
        float tx = W/2 + cosf(t) * W * 0.3f;
        float ty = H/2 + sinf(t * 1.4f) * H * 0.25f;
        p->vx = (tx - p->x) * 0.05f;
        p->vy = (ty - p->y) * 0.05f;
        limit(&p->vx, &p->vy, 2.0f);
        p->x = clampf(p->x + p->vx, 1, W - 2);
        p->y = clampf(p->y + p->vy, 1, H - 2);
    }
}

/* ======================= render ========================== */

static void sim_render(void)
{
    int W = G.w, H = G.h;
    oinit();

    /* clear screen, cursor home */
    oputs("\033[2J\033[H");

    /* ---------- trails ---------- */
    if (G.trails) {
        for (int i = 0; i < G.nb; i++) {
            Boid *b = &G.boids[i];
            for (int t = b->tlen - 1; t >= 0; t--) {
                int tx = b->tx[t], ty = b->ty[t];
                if (tx >= 0 && tx < W && ty >= 1 && ty < H) {
                    /* fade: dim grayscale for older points */
                    int c = 233 + (TRAIL_LEN - 1 - t);
                    oprintf("\033[38;5;%dm\033[%d;%dH\xb7", c, ty + 1, tx + 1);
                }
            }
        }
    }

    /* ---------- boids ---------- */
    for (int i = 0; i < G.nb; i++) {
        Boid *b = &G.boids[i];
        int bx = (int)b->x, by = (int)b->y;
        if (bx >= 0 && bx < W && by >= 0 && by < H) {
            float a = atan2f(b->vy, b->vx);
            int c = ang_color(a);
            char ch = dir_ch(b->vx, b->vy);
            oprintf("\033[38;5;%dm\033[%d;%dH%c", c, by + 1, bx + 1, ch);
        }
    }

    /* ---------- predators ---------- */
    for (int j = 0; j < G.np; j++) {
        Pred *p = &G.preds[j];
        int px = (int)p->x, py = (int)p->y;
        /* danger aura */
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                int nx = px + dx, ny = py + dy;
                if (nx >= 0 && nx < W && ny >= 1 && ny < H && (dx || dy)) {
                    float d = mag((float)dx, (float)dy);
                    if (d < 2.5f) {
                        int bright = (int)(200 - d * 30);
                        oprintf("\033[38;5;%dm\033[%d;%dH%c",
                                bright, ny + 1, nx + 1, d < 1.5f ? '+' : '.');
                    }
                }
            }
        }
        /* body */
        if (px >= 0 && px < W && py >= 0 && py < H)
            oprintf("\033[1;38;5;196m\033[%d;%dH@", py + 1, px + 1);
    }

    /* ---------- HUD ---------- */
    if (G.hud) {
        oputs("\033[0m\033[48;5;236;38;5;255m");
        oprintf("\033[1;1H BOIDS  N:%-4d FPS:%-5.1f Spd:%-4.1f Frame:%-6d",
                G.nb, G.fps, G.spd, G.frame);
        oprintf("\033[2;1H [%c]Sep [%c]Ali [%c]Coh [%c]Trail [%c]Wind Pred:%d [H]UD [Q]uit",
                G.sep?'X':' ', G.ali?'X':' ', G.coh?'X':' ',
                G.trails?'X':' ', G.wind?'X':' ', G.np);

        if (G.paused) {
            int cx = W / 2 - 4, cy = H / 2;
            oprintf("\033[48;5;52;38;5;196m\033[%d;%dH || PAUSED ", cy, cx);
        }
    }

    /* reset attributes */
    oputs("\033[0m");
    oflush();
}

/* ======================= input =========================== */

static void handle_input(void)
{
    int ch;
    while ((ch = kbget()) != -1) {
        switch (ch) {
        case 'q': case 27:  g_run = 0;                        break;
        case ' ':           G.paused = !G.paused;             break;
        case '+': case '=':
            for (int i = 0; i < 20 && G.nb < MAX_BOIDS; i++)
                boid_spawn(&G.boids[G.nb++], (float)G.w, (float)G.h);
            break;
        case '-': case '_':
            G.nb = G.nb > 20 ? G.nb - 20 : 0;
            break;
        case 's': G.sep  = !G.sep;  break;
        case 'a': G.ali  = !G.ali;  break;
        case 'c': G.coh  = !G.coh;  break;
        case 't': G.trails = !G.trails; break;
        case 'w': G.wind = !G.wind; break;
        case 'p':
            G.np = (G.np + 1) % (MAX_PRED + 1);   /* cycle 0→1→2→3→0 */
            for (int j = 0; j < G.np; j++) {
                if (G.preds[j].x == 0 && G.preds[j].y == 0) {
                    G.preds[j].x = (float)G.w / 2.0f;
                    G.preds[j].y = (float)G.h / 2.0f;
                }
            }
            break;
        case '[': G.spd = G.spd > 0.2f ? G.spd - 0.2f : 0.2f; break;
        case ']': G.spd = G.spd < 3.0f ? G.spd + 0.2f : 3.0f; break;
        case 'r':
            G.nb = INIT_BOIDS;
            G.np = 0;
            for (int i = 0; i < G.nb; i++)
                boid_spawn(&G.boids[i], (float)G.w, (float)G.h);
            break;
        case 'h': G.hud = !G.hud; break;
        }
    }
}

/* ======================= fps counter ===================== */

static void calc_fps(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = (now.tv_sec - G.tprev.tv_sec)
              + (now.tv_nsec - G.tprev.tv_nsec) / 1e9;
    if (dt > 0.0)
        G.fps = G.fps * 0.9 + (1.0 / dt) * 0.1;   /* EMA */
    G.tprev = now;
}

/* ======================= signal ========================== */

static void on_sig(int sig) { (void)sig; g_run = 0; }

/* ======================= main ============================ */

int main(int argc, char *argv[])
{
    int n0 = argc > 1 ? atoi(argv[1]) : INIT_BOIDS;
    if (n0 < 1)   n0 = INIT_BOIDS;
    if (n0 > MAX_BOIDS) n0 = MAX_BOIDS;

    srand((unsigned)time(NULL));

    /* allocate render buffer */
    obuf = malloc(OBUFSZ);
    if (!obuf) { fputs("out of memory\n", stderr); return 1; }

    /* init sim state */
    memset(&G, 0, sizeof(G));
    G.spd  = 1.0f;
    G.sep  = G.ali = G.coh = 1;
    G.hud  = 1;
    G.fps  = TGT_FPS;
    G.nb   = n0;
    clock_gettime(CLOCK_MONOTONIC, &G.tprev);

    /* terminal */
    term_init();
    term_size();

    /* spawn boids */
    for (int i = 0; i < G.nb; i++)
        boid_spawn(&G.boids[i], (float)G.w, (float)G.h);

    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGWINCH, SIG_IGN);     /* we poll size each frame */

    /* main loop */
    while (g_run) {
        term_size();
        handle_input();
        sim_update();
        sim_render();
        calc_fps();
        G.frame++;
        usleep(1000000 / TGT_FPS);
    }

    term_restore();
    printf("Boids simulation ended — %d frames rendered.\n", G.frame);
    free(obuf);
    return 0;
}
