/*
 * fluidsim-18879.c — Terminal Fluid Dynamics Simulator
 *
 * Real-time 2D incompressible fluid simulation based on Jos Stam's
 * "Stable Fluids" method (SIGGRAPH 1999). Solves the Navier-Stokes
 * equations using diffusion (Gauss-Seidel), semi-Lagrangian advection,
 * and Helmholtz-Hodge projection on a uniform grid, rendered with
 * ANSI 256-color background fills.
 *
 * Compile:
 *   gcc -O2 -std=c99 -o fluidsim fluidsim-18879.c -lm
 *
 * Run:
 *   ./fluidsim
 *
 * Controls:
 *   W/A/S/D        Move cursor & apply force in that direction
 *   Space (hold)   Inject dye at cursor
 *   Enter          Explode a dye bomb at cursor
 *   1              Fire palette   (black→red→yellow→white)
 *   2              Ocean palette  (black→blue→cyan→white)
 *   3              Neon palette   (black→purple→magenta→pink)
 *   4              Grayscale palette
 *   C              Toggle continuous dye source at cursor
 *   T              Toggle auto-drip (random splashes)
 *   R              Reset simulation
 *   +/=            Increase viscosity
 *   -              Decrease viscosity
 *   [              Decrease diffusion
 *   ]              Increase diffusion
 *   Q              Quit
 *
 * Requires a POSIX terminal with 256-color support (Linux / macOS).
 *
 * SPDX-License-Identifier: MIT
 */

/* Enable POSIX extensions (usleep) */
#if defined(__linux__)
#define _DEFAULT_SOURCE
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* ================================================================
 *  Configuration
 * ================================================================ */

static int W = 64, H = 28;          /* simulation grid (auto-sized) */
#define IX(i, j) ((i) + (W + 2) * (j))
#define SOLVER_ITERS 16              /* Gauss-Seidel iterations       */
#define FRAME_US     33000           /* microseconds between frames   */
#define DENSITY_CAP  5.0f            /* max density per cell          */

/* ================================================================
 *  Global state
 * ================================================================ */

/* Velocity & density fields (current + source) */
static float *u, *v, *u0, *v0;
static float *dens, *dens0;

/* Tuneable parameters */
static float viscosity = 0.00005f;
static float diffusion = 0.00001f;
static float dt        = 0.15f;

/* UI */
static int cursor_x, cursor_y;
static int palette     = 0;          /* 0=fire 1=ocean 2=neon 3=gray */
static int continuous  = 0;          /* constant dye at cursor        */
static int auto_drip   = 1;          /* periodic random splashes      */
static volatile sig_atomic_t running = 1;

/* Terminal */
static struct termios orig_termios;

/* Double-buffered output (avoids per-character write syscalls) */
static char framebuf[262144];
static int  fpos;

/* ================================================================
 *  Frame-buffer helpers
 * ================================================================ */

static void fb_emit(const char *s)
{
    while (*s)
        framebuf[fpos++] = *s++;
}

static void fb_emitf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fpos += vsnprintf(framebuf + fpos, sizeof(framebuf) - fpos, fmt, ap);
    va_end(ap);
}

/* ================================================================
 *  Terminal setup / restore
 * ================================================================ */

static void term_restore(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    /* show cursor, reset attributes, move to bottom */
    (void)write(STDOUT_FILENO, "\033[?25h\033[0m\r\n", 11);
}

static void term_setup(void)
{
    struct termios t;
    tcgetattr(STDIN_FILENO, &orig_termios);
    t = orig_termios;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    (void)write(STDOUT_FILENO, "\033[?25l", 6);   /* hide cursor */
    atexit(term_restore);
}

static void on_signal(int sig) { (void)sig; running = 0; }

/* ================================================================
 *  Memory management
 * ================================================================ */

static void alloc_fields(void)
{
    int sz = (W + 2) * (H + 2);
    u    = calloc(sz, sizeof(float));
    v    = calloc(sz, sizeof(float));
    u0   = calloc(sz, sizeof(float));
    v0   = calloc(sz, sizeof(float));
    dens  = calloc(sz, sizeof(float));
    dens0 = calloc(sz, sizeof(float));
}

static void free_fields(void)
{
    free(u); free(v); free(u0); free(v0);
    free(dens); free(dens0);
}

/* ================================================================
 *  Fluid solver  (Stam's stable-fluids algorithm)
 * ================================================================ */

/* Enforce boundary conditions.
 * b=0 : scalar (density)
 * b=1 : x-velocity (negate at left/right walls)
 * b=2 : y-velocity (negate at top/bottom walls)
 */
static void set_bnd(int b, float *x)
{
    for (int i = 1; i <= W; i++) {
        x[IX(i, 0)]     = b == 2 ? -x[IX(i, 1)]     : x[IX(i, 1)];
        x[IX(i, H + 1)] = b == 2 ? -x[IX(i, H)]     : x[IX(i, H)];
    }
    for (int j = 1; j <= H; j++) {
        x[IX(0, j)]     = b == 1 ? -x[IX(1, j)]     : x[IX(1, j)];
        x[IX(W + 1, j)] = b == 1 ? -x[IX(W, j)]     : x[IX(W, j)];
    }
    /* corners: average of neighbours */
    x[IX(0, 0)]         = 0.5f * (x[IX(1, 0)]     + x[IX(0, 1)]);
    x[IX(0, H + 1)]     = 0.5f * (x[IX(1, H + 1)] + x[IX(0, H)]);
    x[IX(W + 1, 0)]     = 0.5f * (x[IX(W, 0)]     + x[IX(W + 1, 1)]);
    x[IX(W + 1, H + 1)] = 0.5f * (x[IX(W, H + 1)] + x[IX(W + 1, H)]);
}

/* Implicit diffusion via Gauss-Seidel relaxation */
static void diffuse(int b, float *x, float *x0, float diff_coeff)
{
    float a = dt * diff_coeff * W * H;
    if (a < 1e-10f) {
        memcpy(x, x0, (W + 2) * (H + 2) * sizeof(float));
        return;
    }
    float inv_denom = 1.0f / (1.0f + 4.0f * a);
    for (int k = 0; k < SOLVER_ITERS; k++) {
        for (int j = 1; j <= H; j++) {
            for (int i = 1; i <= W; i++) {
                x[IX(i, j)] = (x0[IX(i, j)]
                    + a * (x[IX(i-1, j)] + x[IX(i+1, j)]
                         + x[IX(i, j-1)] + x[IX(i, j+1)])
                ) * inv_denom;
            }
        }
        set_bnd(b, x);
    }
}

/* Semi-Lagrangian advection: trace particles backwards through velocity */
static void advect(int b, float *fld, float *fld0, float *uf, float *vf)
{
    float dtx = dt * W;
    float dty = dt * H;
    for (int j = 1; j <= H; j++) {
        for (int i = 1; i <= W; i++) {
            float x = i - dtx * uf[IX(i, j)];
            float y = j - dty * vf[IX(i, j)];
            /* clamp to grid interior */
            x = fmaxf(0.5f, fminf(W + 0.5f, x));
            y = fmaxf(0.5f, fminf(H + 0.5f, y));
            int i0 = (int)x, i1 = i0 + 1;
            int j0 = (int)y, j1 = j0 + 1;
            float s1 = x - i0, s0 = 1.0f - s1;
            float t1 = y - j0, t0 = 1.0f - t1;
            fld[IX(i, j)] =
                s0 * (t0 * fld0[IX(i0, j0)] + t1 * fld0[IX(i0, j1)]) +
                s1 * (t0 * fld0[IX(i1, j0)] + t1 * fld0[IX(i1, j1)]);
        }
    }
    set_bnd(b, fld);
}

/* Helmholtz-Hodge decomposition: make velocity divergence-free */
static void project(float *uf, float *vf, float *p, float *div)
{
    float hx = 1.0f / W;
    float hy = 1.0f / H;
    for (int j = 1; j <= H; j++) {
        for (int i = 1; i <= W; i++) {
            div[IX(i, j)] = -0.5f * (
                hx * (uf[IX(i+1, j)] - uf[IX(i-1, j)]) +
                hy * (vf[IX(i, j+1)] - vf[IX(i, j-1)])
            );
            p[IX(i, j)] = 0.0f;
        }
    }
    set_bnd(0, div);
    set_bnd(0, p);

    for (int k = 0; k < SOLVER_ITERS; k++) {
        for (int j = 1; j <= H; j++) {
            for (int i = 1; i <= W; i++) {
                p[IX(i, j)] = (div[IX(i, j)]
                    + p[IX(i-1, j)] + p[IX(i+1, j)]
                    + p[IX(i, j-1)] + p[IX(i, j+1)]
                ) * 0.25f;
            }
        }
        set_bnd(0, p);
    }

    for (int j = 1; j <= H; j++) {
        for (int i = 1; i <= W; i++) {
            uf[IX(i, j)] -= 0.5f * W * (p[IX(i+1, j)] - p[IX(i-1, j)]);
            vf[IX(i, j)] -= 0.5f * H * (p[IX(i, j+1)] - p[IX(i, j-1)]);
        }
    }
    set_bnd(1, uf);
    set_bnd(2, vf);
}

/* Full simulation tick: sources → diffuse → project → advect → project */
static void sim_step(void)
{
    int sz = (W + 2) * (H + 2);
    float *tmp;

    /* --- velocity step --- */
    for (int i = 0; i < sz; i++) {
        u[i] += dt * u0[i];
        v[i] += dt * v0[i];
    }
    tmp = u0; u0 = u; u = tmp;
    diffuse(1, u, u0, viscosity);
    tmp = v0; v0 = v; v = tmp;
    diffuse(2, v, v0, viscosity);
    project(u, v, u0, v0);           /* u0,v0 used as scratch */

    tmp = u0; u0 = u; u = tmp;
    tmp = v0; v0 = v; v = tmp;
    advect(1, u, u0, u0, v0);        /* advect u using (u0,v0) velocity */
    advect(2, v, v0, u0, v0);        /* advect v using (u0,v0) velocity */
    project(u, v, u0, v0);

    /* --- density step --- */
    for (int i = 0; i < sz; i++)
        dens[i] += dt * dens0[i];
    tmp = dens0; dens0 = dens; dens = tmp;
    diffuse(0, dens, dens0, diffusion);
    tmp = dens0; dens0 = dens; dens = tmp;
    advect(0, dens, dens0, u, v);

    /* clear source arrays, clamp density */
    memset(u0,    0, sz * sizeof(float));
    memset(v0,    0, sz * sizeof(float));
    memset(dens0, 0, sz * sizeof(float));
    for (int i = 0; i < sz; i++) {
        if (dens[i] < 0.0f) dens[i] = 0.0f;
        if (dens[i] > DENSITY_CAP) dens[i] = DENSITY_CAP;
    }
}

/* ================================================================
 *  Colour palettes  (ANSI 256-color codes)
 * ================================================================ */

static const int pal_fire[] = {
    16,  52,  88, 124, 160, 196, 202, 208,
   214, 220, 226, 228, 229, 230, 231
};
#define PAL_FIRE_N 15

static const int pal_ocean[] = {
    16,  17,  18,  19,  20,  21,  26,  27,
    32,  33,  39,  44,  45,  51,  87, 123,
   159, 195, 231
};
#define PAL_OCEAN_N 19

static const int pal_neon[] = {
    16,  54,  55,  91,  92, 127, 128, 163,
   164, 200, 201, 198, 199, 205, 211, 225, 231
};
#define PAL_NEON_N 17

static int lookup_palette(float val, const int *colors, int n)
{
    val = fmaxf(0.0f, fminf(1.0f, val));
    int idx = (int)(val * (n - 1) + 0.5f);
    return colors[idx < n ? idx : n - 1];
}

static int map_color(float val)
{
    switch (palette) {
    case 0: return lookup_palette(val, pal_fire,   PAL_FIRE_N);
    case 1: return lookup_palette(val, pal_ocean,  PAL_OCEAN_N);
    case 2: return lookup_palette(val, pal_neon,   PAL_NEON_N);
    default: /* grayscale 232-255 */
        return 232 + (int)(fmaxf(0.0f, fminf(1.0f, val)) * 23);
    }
}

/* ================================================================
 *  Splash / bomb helper
 * ================================================================ */

static void add_splash(int x, int y, float amount, int radius, float force)
{
    for (int dj = -radius; dj <= radius; dj++) {
        for (int di = -radius; di <= radius; di++) {
            int ci = x + di, cj = y + dj;
            if (ci < 1 || ci > W || cj < 1 || cj > H) continue;
            float dist = sqrtf((float)(di * di + dj * dj));
            if (dist > radius) continue;
            float falloff = 1.0f - dist / radius;
            dens0[IX(ci, cj)] += amount * falloff;
            if (force > 0.0f) {
                float angle = atan2f((float)dj, (float)di);
                u0[IX(ci, cj)] += force * cosf(angle) * falloff;
                v0[IX(ci, cj)] += force * sinf(angle) * falloff;
            }
        }
    }
}

/* ================================================================
 *  Input
 * ================================================================ */

static void handle_input(void)
{
    char buf[64];
    int n;
    while ((n = (int)read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char ch = buf[i];
            float f = 10.0f;            /* force magnitude */
            switch (ch) {
            case 'q': case 'Q':
                running = 0; return;
            /* cursor movement + force */
            case 'w': if (cursor_y > 1)  cursor_y--; v0[IX(cursor_x, cursor_y)] -= f; break;
            case 's': if (cursor_y < H)  cursor_y++; v0[IX(cursor_x, cursor_y)] += f; break;
            case 'a': if (cursor_x > 1)  cursor_x--; u0[IX(cursor_x, cursor_y)] -= f; break;
            case 'd': if (cursor_x < W)  cursor_x++; u0[IX(cursor_x, cursor_y)] += f; break;
            /* dye injection */
            case ' ':
                add_splash(cursor_x, cursor_y, 6.0f, 3, 0.0f);
                break;
            /* dye bomb */
            case '\n': case '\r':
                add_splash(cursor_x, cursor_y, 20.0f, 7, 15.0f);
                break;
            /* palette */
            case '1': palette = 0; break;
            case '2': palette = 1; break;
            case '3': palette = 2; break;
            case '4': palette = 3; break;
            /* toggles */
            case 'c': case 'C': continuous  = !continuous;  break;
            case 't': case 'T': auto_drip   = !auto_drip;   break;
            /* reset */
            case 'r': case 'R': {
                int sz = (W + 2) * (H + 2) * sizeof(float);
                memset(u,    0, sz); memset(v,    0, sz);
                memset(dens, 0, sz);
                break;
            }
            /* parameter adjust */
            case '=': case '+': viscosity  *= 2.0f; break;
            case '-':           viscosity   = fmaxf(1e-7f, viscosity  * 0.5f); break;
            case ']':           diffusion  *= 2.0f; break;
            case '[':           diffusion   = fmaxf(1e-7f, diffusion  * 0.5f); break;
            default: break;
            }
        }
    }

    /* continuous dye source */
    if (continuous) {
        dens0[IX(cursor_x, cursor_y)] += 12.0f;
        u0[IX(cursor_x, cursor_y)] += ((float)rand() / RAND_MAX - 0.5f) * 6.0f;
        v0[IX(cursor_x, cursor_y)] += ((float)rand() / RAND_MAX - 0.5f) * 6.0f;
    }
}

/* ================================================================
 *  Rendering
 * ================================================================ */

static void render(void)
{
    fpos = 0;
    fb_emit("\033[H");               /* cursor home */

    /* top border */
    fb_emit("+");
    for (int i = 0; i < W; i++) fb_emit("-");
    fb_emit("+\n");

    for (int j = 1; j <= H; j++) {
        fb_emit("|");
        for (int i = 1; i <= W; i++) {
            /* cursor marker */
            if (i == cursor_x && j == cursor_y) {
                fb_emit("\033[47;30m+\033[0m");
                continue;
            }
            float val = dens[IX(i, j)] / 3.0f;   /* normalise to ~[0,1] */
            if (val < 0.015f) {
                fb_emit(" ");
                continue;
            }
            int col = map_color(val);
            fb_emitf("\033[48;5;%dm ", col);
        }
        fb_emit("\033[0m|\n");
    }

    /* bottom border */
    fb_emit("+");
    for (int i = 0; i < W; i++) fb_emit("-");
    fb_emit("+\n");

    /* HUD line 1 */
    const char *pal_name[] = {"Fire", "Ocean", "Neon", "Gray"};
    fb_emitf(
        " \033[1mPalette:\033[0m%-6s \033[1mVisc:\033[0m%.6f"
        " \033[1mDiff:\033[0m%.6f \033[1mCursor:\033[0m(%d,%d)"
        "%s%s\n",
        pal_name[palette], viscosity, diffusion,
        cursor_x, cursor_y,
        continuous ? " \033[32m[DRIP]\033[0m" : "",
        auto_drip  ? " \033[33m[AUTO]\033[0m" : ""
    );

    /* HUD line 2 */
    fb_emit(
        " WASD:move  Space:dye  Enter:bomb  1-4:palette"
        "  C:drip  T:auto  R:reset  +/-:visc  Q:quit\n"
    );

    (void)write(STDOUT_FILENO, framebuf, fpos);
}

/* ================================================================
 *  Main
 * ================================================================ */

int main(void)
{
    srand((unsigned)time(NULL));

    /* auto-size grid to terminal dimensions */
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0
        && ws.ws_col > 24 && ws.ws_row > 10) {
        W = ws.ws_col - 4;
        H = ws.ws_row - 5;
        if (W < 20)  W = 20;
        if (W > 200) W = 200;
        if (H < 10)  H = 10;
        if (H > 60)  H = 60;
    }
    cursor_x = W / 2;
    cursor_y = H / 2;

    alloc_fields();
    term_setup();

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* initial splash to demonstrate the fluid */
    add_splash(W / 2,     H / 2,     4.5f, 7, 12.0f);
    add_splash(W / 4,     H / 3,     3.5f, 4,  9.0f);
    add_splash(3 * W / 4, 2 * H / 3, 3.5f, 4,  9.0f);

    (void)write(STDOUT_FILENO, "\033[2J", 4);     /* clear screen */

    int frame = 0;
    while (running) {
        handle_input();

        /* periodic random splashes */
        if (auto_drip && (frame % 18 == 0)) {
            int rx = 4 + rand() % (W - 8);
            int ry = 4 + rand() % (H - 8);
            add_splash(rx, ry, 2.5f, 2, 5.0f);
        }

        sim_step();
        render();
        frame++;
        usleep(FRAME_US);
    }

    free_fields();
    return 0;
}
