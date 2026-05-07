/*
 * ================================================================
 *  fireworks-9040.c — Terminal Fireworks Particle Simulator
 * ================================================================
 *  Real-time particle-based fireworks display rendered entirely
 *  in the terminal using ANSI escape codes. Features multiple
 *  burst shapes (peony, ring, willow, crossette), gravity,
 *  air resistance, color palettes, and auto/manual launch.
 *
 *  Compile:
 *    gcc -O2 -Wall -std=c99 -o fireworks fireworks-9040.c -lm
 *
 *  Run:
 *    ./fireworks
 *
 *  Controls:
 *    SPACE  — launch a firework at a random position
 *    a      — toggle auto-launch mode (default: ON)
 *    +/=    — increase launch frequency
 *    -      — decrease launch frequency
 *    q/Esc  — quit
 *
 *  Requires: POSIX terminal (Linux/macOS), ANSI 256-color support.
 * ================================================================
 */

#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200809L
#undef  _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <termios.h>
#include <signal.h>

/* ---- Configuration ---- */
#define MAX_PARTICLES  8000
#define GRAVITY        0.032f
#define DRAG           0.991f
#define FRAME_US       33000   /* ~30 fps (microseconds) */
#define MAX_STARS      120
#define PALETTE_COUNT  6

/* Particle types */
#define SHELL   0
#define SPARK   1
#define TRAIL   2

/* ---- Color Palettes (ANSI 256-color ranges) ---- */
typedef struct { int lo, hi; } Palette;
static const Palette palettes[PALETTE_COUNT] = {
    {196, 226},   /* fire:     red → yellow       */
    { 21,  51},   /* ice:      blue → cyan        */
    {129, 231},   /* violet:   purple → white      */
    { 22, 118},   /* emerald:  dark green → lime   */
    {178, 226},   /* gold:     amber → yellow      */
    { 52,  81},   /* crimson:  deep red → orange   */
};

/* Burst shape types */
#define BURST_PEONY      0
#define BURST_RING       1
#define BURST_WILLOW     2
#define BURST_CROSSETTE  3
#define BURST_DOUBLE     4
#define BURST_PALM       5
#define BURST_COUNT      6

/* ---- Particle ---- */
typedef struct {
    float x, y;        /* position (terminal coords) */
    float vx, vy;      /* velocity */
    int   life;        /* remaining frames */
    int   max_life;
    int   color;       /* ANSI 256-color code */
    int   active;
    int   type;        /* SHELL, SPARK, TRAIL */
    int   palette;     /* which color palette */
    int   can_explode; /* sparks that produce secondary bursts */
} Particle;

/* ---- Global State ---- */
static Particle pool[MAX_PARTICLES];
static int    W = 80, H = 24;
static int    auto_on    = 1;
static int    auto_cd    = 0;
static int    freq       = 1;       /* launch frequency multiplier */
static int    running    = 1;
static struct termios saved_term;

/* Background stars */
typedef struct { int x, y; char ch; } Star;
static Star stars[MAX_STARS];
static int  star_count = 0;

/* ---- Terminal Control ---- */
static void raw_mode(void) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &saved_term);
    t = saved_term;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

static void restore_term(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &saved_term);
    printf("\033[?25h");      /* show cursor */
    printf("\033[?1049l");    /* exit alt screen */
    fflush(stdout);
}

static void get_size(void) {
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    if (w.ws_col > 10) W = w.ws_col;
    if (w.ws_row > 5)  H = w.ws_row;
}

static void handle_sigwinch(int sig) {
    (void)sig;
    get_size();
}

/* ---- Helpers ---- */
static int palette_color(int pal) {
    if (pal < 0 || pal >= PALETTE_COUNT) pal = rand() % PALETTE_COUNT;
    return palettes[pal].lo + rand() % (palettes[pal].hi - palettes[pal].lo + 1);
}

static int kbhit(void) {
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(0, &fds);
    return select(1, &fds, NULL, NULL, &tv) > 0;
}

/* ---- Star Background ---- */
static void init_stars(void) {
    star_count = MAX_STARS;
    if (star_count > (W * H) / 20) star_count = (W * H) / 20;
    for (int i = 0; i < star_count; i++) {
        stars[i].x  = rand() % W;
        stars[i].y  = rand() % (H - 2);  /* keep bottom rows free */
        stars[i].ch = ".:*"[rand() % 3];
    }
}

/* ---- Particle Management ---- */
static void add_particle(float x, float y, float vx, float vy,
                         int life, int color, int type,
                         int palette, int can_explode)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!pool[i].active) {
            pool[i] = (Particle){
                x, y, vx, vy,
                life, life,
                color, 1, type,
                palette, can_explode
            };
            return;
        }
    }
}

static void launch(void) {
    float x  = 5.0f + (float)(rand() % (W - 10));
    float vy = -(1.3f + (float)(rand() % 80) / 80.0f * 1.0f);
    float vx = ((float)(rand() % 60 - 30)) / 50.0f;
    int life = 22 + rand() % 22;
    int pal  = rand() % PALETTE_COUNT;
    add_particle(x, (float)H, vx, vy, life, palette_color(pal), SHELL, pal, 0);
}

static void explode(float x, float y, int pal, int shape) {
    int n;

    switch (shape) {
    case BURST_PEONY:     /* classic spherical burst */
        n = 80 + rand() % 50;
        for (int i = 0; i < n; i++) {
            float angle = (float)(rand() % 6283) / 1000.0f;
            float speed = 0.3f + (float)(rand() % 60) / 100.0f;
            float vx = cosf(angle) * speed;
            float vy = sinf(angle) * speed - 0.15f;
            int life = 35 + rand() % 35;
            add_particle(x, y, vx, vy, life, palette_color(pal), SPARK, pal, 0);
        }
        break;

    case BURST_RING:     /* circular ring */
        n = 60 + rand() % 30;
        for (int i = 0; i < n; i++) {
            float angle = (float)i / (float)n * 6.2832f;
            float speed = 0.55f + (float)(rand() % 15) / 100.0f;
            float vx = cosf(angle) * speed;
            float vy = sinf(angle) * speed - 0.1f;
            int life = 40 + rand() % 20;
            add_particle(x, y, vx, vy, life, palette_color(pal), SPARK, pal, 0);
        }
        break;

    case BURST_WILLOW:   /* droopy, long-lived sparks */
        n = 70 + rand() % 40;
        for (int i = 0; i < n; i++) {
            float angle = (float)(rand() % 6283) / 1000.0f;
            float speed = 0.2f + (float)(rand() % 50) / 100.0f;
            float vx = cosf(angle) * speed * 0.8f;
            float vy = sinf(angle) * speed * 0.6f - 0.1f;
            int life = 55 + rand() % 45;
            add_particle(x, y, vx, vy, life, palette_color(pal), SPARK, pal, 0);
        }
        break;

    case BURST_CROSSETTE: /* sparks that explode again */
        n = 20 + rand() % 15;
        for (int i = 0; i < n; i++) {
            float angle = (float)(rand() % 6283) / 1000.0f;
            float speed = 0.5f + (float)(rand() % 30) / 100.0f;
            float vx = cosf(angle) * speed;
            float vy = sinf(angle) * speed - 0.1f;
            int life = 20 + rand() % 15;
            /* can_explode=1 → these sparks will burst into sub-sparks */
            add_particle(x, y, vx, vy, life, palette_color(pal), SPARK, pal, 1);
        }
        break;

    case BURST_DOUBLE:   /* two concentric bursts */
        for (int ring = 0; ring < 2; ring++) {
            n = 40 + rand() % 20;
            float base_speed = (ring == 0) ? 0.7f : 0.35f;
            for (int i = 0; i < n; i++) {
                float angle = (float)(rand() % 6283) / 1000.0f;
                float speed = base_speed + (float)(rand() % 15) / 100.0f;
                float vx = cosf(angle) * speed;
                float vy = sinf(angle) * speed - 0.12f;
                int life = 30 + rand() % 30;
                add_particle(x, y, vx, vy, life, palette_color(pal), SPARK, pal, 0);
            }
        }
        break;

    case BURST_PALM:     /* upward burst then drooping */
        n = 60 + rand() % 30;
        for (int i = 0; i < n; i++) {
            float angle = -1.57f + (float)(rand() % 3142 - 1571) / 1000.0f;
            float speed = 0.5f + (float)(rand() % 40) / 100.0f;
            float vx = cosf(angle) * speed * 0.6f;
            float vy = sinf(angle) * speed;
            int life = 50 + rand() % 40;
            add_particle(x, y, vx, vy, life, palette_color(pal), SPARK, pal, 0);
        }
        break;
    }
}

/* ---- Physics Update ---- */
static void step(void) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!pool[i].active) continue;
        Particle *p = &pool[i];

        p->vx *= DRAG;
        p->vy += GRAVITY;
        p->x  += p->vx;
        p->y  += p->vy;
        p->life--;

        /* Kill off-screen or dead particles */
        if (p->life <= 0 || p->y > H + 3 || p->x < -3 || p->x > W + 3) {
            /* Shell explodes when it dies */
            if (p->type == SHELL) {
                int shape = rand() % BURST_COUNT;
                explode(p->x, p->y, p->palette, shape);
            }
            /* Crossette spark: secondary explosion */
            if (p->type == SPARK && p->can_explode && p->life <= 0) {
                for (int j = 0; j < 8 + rand() % 8; j++) {
                    float angle = (float)(rand() % 6283) / 1000.0f;
                    float speed = 0.15f + (float)(rand() % 30) / 100.0f;
                    float vx = cosf(angle) * speed;
                    float vy = sinf(angle) * speed - 0.05f;
                    int life = 15 + rand() % 20;
                    add_particle(p->x, p->y, vx, vy, life,
                                 palette_color(p->palette), SPARK, p->palette, 0);
                }
            }
            p->active = 0;
            continue;
        }

        /* Shell leaves a trail of sparks */
        if (p->type == SHELL && (rand() % 2 == 0)) {
            add_particle(p->x, p->y,
                         p->vx * 0.05f, p->vy * 0.05f + 0.03f,
                         6 + rand() % 5,
                         253, TRAIL, p->palette, 0);
        }
    }
}

/* ---- Rendering ---- */
static void render(void) {
    /* Clear screen and hide cursor */
    printf("\033[2J\033[H\033[?25l");

    /* Draw background stars */
    for (int i = 0; i < star_count; i++) {
        printf("\033[%d;%dH\033[38;5;240m%c",
               stars[i].y + 1, stars[i].x + 1, stars[i].ch);
    }

    /* Draw ground */
    printf("\033[%d;1H\033[38;5;22m", H);
    for (int i = 0; i < W; i++) putchar('^');
    printf("\033[%d;1H\033[38;5;28m", H + 1);
    for (int i = 0; i < W; i++) putchar('~');

    /* Draw all particles */
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!pool[i].active) continue;
        Particle *p = &pool[i];
        int ix = (int)p->x;
        int iy = (int)p->y;
        if (ix < 0 || ix >= W || iy < 0 || iy >= H) continue;

        float fade = (float)p->life / (float)p->max_life;
        char ch;

        if (p->type == SHELL) {
            ch = (fade > 0.3f) ? '@' : 'o';
        } else if (p->type == TRAIL) {
            ch = '.';
        } else {
            /* Spark: character changes as it fades */
            if      (fade > 0.7f) ch = '*';
            else if (fade > 0.45f) ch = 'o';
            else if (fade > 0.2f)  ch = '.';
            else                   ch = '`';
        }

        /* Color: dim as particle ages */
        int color = p->color;
        if (p->type == SPARK && fade < 0.4f) {
            color = (color > 235) ? 235 : color - 12;
            if (color < 0) color = 232;
        }

        printf("\033[%d;%dH\033[38;5;%dm%c",
               iy + 1, ix + 1, color, ch);
    }

    /* HUD — top bar */
    printf("\033[1;1H\033[38;5;250m");
    printf(" FIREWORKS │ SPACE:Launch  A:Auto:%-3s  +/-:Freq:%dx  Q:Quit",
           auto_on ? "ON" : "OFF", freq);
    printf("\033[0m");

    /* Count active particles (simple) */
    int cnt = 0;
    for (int i = 0; i < MAX_PARTICLES; i++) cnt += pool[i].active;
    printf("\033[1;%dH\033[38;5;243m[%d particles]", W - 16, cnt);

    fflush(stdout);
}

/* ---- Input Handling ---- */
static void process_input(void) {
    if (!kbhit()) return;
    char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1) return;

    switch (c) {
    case 'q':
    case 27:   /* ESC */
        running = 0;
        break;
    case ' ':
        launch();
        break;
    case 'a': case 'A':
        auto_on = !auto_on;
        break;
    case '+': case '=':
        if (freq < 5) freq++;
        break;
    case '-':
        if (freq > 1) freq--;
        break;
    }
}

/* ---- Main ---- */
int main(void) {
    srand((unsigned)time(NULL));

    /* Terminal setup */
    atexit(restore_term);
    printf("\033[?1049h");   /* alt screen buffer */
    fflush(stdout);
    raw_mode();
    get_size();
    signal(SIGWINCH, handle_sigwinch);
    init_stars();

    /* Clear particle pool */
    memset(pool, 0, sizeof(pool));

    /* Splash — auto-launch a few fireworks to start */
    for (int i = 0; i < 3; i++) {
        launch();
    }
    auto_cd = 10;

    /* Main loop */
    while (running) {
        process_input();

        /* Auto-launch */
        if (auto_on) {
            auto_cd--;
            if (auto_cd <= 0) {
                for (int f = 0; f < freq; f++) launch();
                auto_cd = 18 + rand() % 35;
            }
        }

        step();
        render();
        usleep(FRAME_US);
    }

    return 0;
}
