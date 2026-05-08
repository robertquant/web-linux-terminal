/*
 * ============================================================
 *  Terminal N-Body Gravity Simulator
 *  nbody-4199.c
 * ============================================================
 *
 *  Real-time gravitational N-body simulation with orbital
 *  trails, collision merging, and interactive controls.
 *  Uses velocity-Verlet integration for energy conservation.
 *
 *  Compile:  gcc -O2 -o nbody nbody-4199.c -lm
 *  Run:      ./nbody
 *
 *  Controls:
 *    SPACE ....... Pause / Resume
 *    +/- ......... Speed up / Slow down time
 *    1-4 ......... Load preset scenarios
 *    A ........... Add body at cursor (auto-orbital velocity)
 *    D ........... Delete nearest body
 *    Arrows ...... Move cursor
 *    Z ........... Zoom in
 *    X ........... Zoom out
 *    T ........... Toggle trails
 *    C ........... Clear all trails
 *    I ........... Toggle info panel
 *    R ........... Reset camera
 *    Q / ESC ..... Quit
 *
 *  Requires: ANSI terminal with 256-color support
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

/* ---- Constants ---- */
#define MAX_BODIES   64
#define TRAIL_LEN    500
#define G_CONST      1.0
#define SOFTENING    0.5
#define ESC_KEY      27
#define FRAME_US     33000   /* ~30 fps */

#define KEY_UP    0x1000
#define KEY_DOWN  0x1001
#define KEY_LEFT  0x1002
#define KEY_RIGHT 0x1003

/* ---- Types ---- */
typedef struct {
    double x, y, vx, vy, ax, ay;
    double mass;
    int    hue;       /* 0-35 HSV hue index */
    int    alive;
    /* Circular trail buffer */
    double tx[TRAIL_LEN], ty[TRAIL_LEN];
    int    tlen, tidx;
} Body;

typedef struct {
    int ch;
    int fg;
} Cell;

/* ---- Global State ---- */
static struct termios orig_term;
static Body    bodies[MAX_BODIES];
static int     nbodies;
static double  cam_x, cam_y, cam_zoom;
static double  sim_time, sim_dt;
static int     paused, show_trails, show_info, running;
static double  cur_x, cur_y;
static int     term_w, term_h;
static Cell   *fb;
static char   *out_buf;
static int     out_buf_sz;
static int     trail_tick;

/* ---- Terminal ---- */
static void term_raw(void) {
    tcgetattr(0, &orig_term);
    struct termios r = orig_term;
    r.c_iflag &= ~(ICRNL | IXON);
    r.c_lflag &= ~(ECHO | ICANON);
    r.c_cc[VMIN] = 0;
    r.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &r);
}

static void term_restore(void) {
    tcsetattr(0, TCSAFLUSH, &orig_term);
    printf("\033[?25h\033[0m\033[H\033[2J");
    fflush(stdout);
}

static void term_update_size(void) {
    struct winsize ws;
    ioctl(0, TIOCGWINSZ, &ws);
    int nw = ws.ws_col < 20 ? 80 : ws.ws_col;
    int nh = ws.ws_row < 10 ? 24 : ws.ws_row;
    if (nw != term_w || nh != term_h) {
        term_w = nw;
        term_h = nh;
        free(fb);
        fb = malloc(term_w * term_h * sizeof(Cell));
        int need = term_w * term_h * 24 + 512;
        if (need > out_buf_sz) {
            out_buf_sz = need;
            out_buf = realloc(out_buf, out_buf_sz);
        }
    }
}

/* ---- Color: HSV -> ANSI 256 ---- */
static int hsv_to_ansi(int h, int s, int v) {
    double H = h * 10.0, S = s / 100.0, V = v / 100.0;
    double c = V * S, hp = H / 60.0;
    double x = c * (1.0 - fabs(fmod(hp, 2.0) - 1.0));
    double r1, g1, b1;
    if      (hp < 1) { r1=c; g1=x; b1=0; }
    else if (hp < 2) { r1=x; g1=c; b1=0; }
    else if (hp < 3) { r1=0; g1=c; b1=x; }
    else if (hp < 4) { r1=0; g1=x; b1=c; }
    else if (hp < 5) { r1=x; g1=0; b1=c; }
    else              { r1=c; g1=0; b1=x; }
    double m = V - c;
    int ri = (int)((r1 + m) * 5 + 0.5);
    int gi = (int)((g1 + m) * 5 + 0.5);
    int bi = (int)((b1 + m) * 5 + 0.5);
    ri = ri < 0 ? 0 : ri > 5 ? 5 : ri;
    gi = gi < 0 ? 0 : gi > 5 ? 5 : gi;
    bi = bi < 0 ? 0 : bi > 5 ? 5 : bi;
    return 16 + 36 * ri + 6 * gi + bi;
}

static int body_fg(int hue, int brightness) {
    return hsv_to_ansi(hue, 85, brightness);
}

/* ---- Physics ---- */
static void compute_accel(void) {
    for (int i = 0; i < nbodies; i++) {
        if (!bodies[i].alive) continue;
        bodies[i].ax = 0;
        bodies[i].ay = 0;
        for (int j = 0; j < nbodies; j++) {
            if (i == j || !bodies[j].alive) continue;
            double dx = bodies[j].x - bodies[i].x;
            double dy = bodies[j].y - bodies[i].y;
            double r2 = dx * dx + dy * dy + SOFTENING * SOFTENING;
            double r = sqrt(r2);
            double f = G_CONST * bodies[j].mass / (r2 * r);
            bodies[i].ax += f * dx;
            bodies[i].ay += f * dy;
        }
    }
}

static void check_merges(void) {
    for (int i = 0; i < nbodies; i++) {
        if (!bodies[i].alive) continue;
        for (int j = i + 1; j < nbodies; j++) {
            if (!bodies[j].alive) continue;
            double dx = bodies[j].x - bodies[i].x;
            double dy = bodies[j].y - bodies[i].y;
            double dist = sqrt(dx * dx + dy * dy);
            double threshold = 0.3 * (cbrt(bodies[i].mass) + cbrt(bodies[j].mass));
            if (dist < threshold) {
                int big = (bodies[i].mass >= bodies[j].mass) ? i : j;
                int sml = (big == i) ? j : i;
                double tm = bodies[big].mass + bodies[sml].mass;
                bodies[big].vx = (bodies[big].mass * bodies[big].vx +
                    bodies[sml].mass * bodies[sml].vx) / tm;
                bodies[big].vy = (bodies[big].mass * bodies[big].vy +
                    bodies[sml].mass * bodies[sml].vy) / tm;
                bodies[big].x = (bodies[big].mass * bodies[big].x +
                    bodies[sml].mass * bodies[sml].x) / tm;
                bodies[big].y = (bodies[big].mass * bodies[big].y +
                    bodies[sml].mass * bodies[sml].y) / tm;
                bodies[big].mass = tm;
                bodies[sml].alive = 0;
            }
        }
    }
}

static void step(double dt) {
    /* Velocity Verlet integration */
    for (int i = 0; i < nbodies; i++) {
        if (!bodies[i].alive) continue;
        bodies[i].x += bodies[i].vx * dt + 0.5 * bodies[i].ax * dt * dt;
        bodies[i].y += bodies[i].vy * dt + 0.5 * bodies[i].ay * dt * dt;
    }
    double oax[MAX_BODIES], oay[MAX_BODIES];
    for (int i = 0; i < nbodies; i++) {
        oax[i] = bodies[i].ax;
        oay[i] = bodies[i].ay;
    }
    compute_accel();
    for (int i = 0; i < nbodies; i++) {
        if (!bodies[i].alive) continue;
        bodies[i].vx += 0.5 * (oax[i] + bodies[i].ax) * dt;
        bodies[i].vy += 0.5 * (oay[i] + bodies[i].ay) * dt;
    }
    check_merges();

    /* Record trails (every few steps to extend history) */
    trail_tick++;
    if (trail_tick % 4 == 0) {
        for (int i = 0; i < nbodies; i++) {
            if (!bodies[i].alive) continue;
            Body *b = &bodies[i];
            b->tx[b->tidx] = b->x;
            b->ty[b->tidx] = b->y;
            b->tidx = (b->tidx + 1) % TRAIL_LEN;
            if (b->tlen < TRAIL_LEN) b->tlen++;
        }
    }
    sim_time += dt;
}

/* ---- Presets ---- */
static void add_body(double x, double y, double vx, double vy,
                     double mass, int hue) {
    if (nbodies >= MAX_BODIES) return;
    Body *b = &bodies[nbodies++];
    memset(b, 0, sizeof(*b));
    b->x = x; b->y = y;
    b->vx = vx; b->vy = vy;
    b->mass = mass;
    b->hue = hue;
    b->alive = 1;
}

static void clear_all(void) {
    nbodies = 0;
    sim_time = 0;
    trail_tick = 0;
}

static void preset_solar(void) {
    clear_all();
    double M = 500;
    add_body(0, 0, 0, 0, M, 12);  /* Sun - yellow */

    /* { radius, mass, hue_angle } */
    double pdata[][3] = {
        { 5,  0.5,  6 },   /* Mercury */
        { 8,  2.0,  9 },   /* Venus */
        {12,  2.5, 21 },   /* Earth */
        {17,  1.5,  3 },   /* Mars */
        {28, 30,   28 },   /* Jupiter */
        {40, 15,   18 },   /* Saturn */
    };
    for (int i = 0; i < 6; i++) {
        double r = pdata[i][0], m = pdata[i][1], h = pdata[i][2];
        double v = sqrt(G_CONST * M / r);
        double a = i * 1.1;
        add_body(r * cos(a), r * sin(a), -v * sin(a), v * cos(a), m, (int)h);
    }
    cam_x = 0; cam_y = 0; cam_zoom = 1.2;
    sim_dt = 0.015;
    compute_accel();
}

static void preset_binary(void) {
    clear_all();
    double m = 200, sep = 8;
    double v = sqrt(G_CONST * m / (4 * sep));
    add_body(-sep, 0, 0,  v, m, 12);  /* Star A - yellow */
    add_body( sep, 0, 0, -v, m, 21);  /* Star B - blue */
    /* Circumbinary planet */
    double rp = 25, vp = sqrt(G_CONST * 2 * m / rp);
    add_body(0, rp, vp, 0, 2, 3);
    cam_x = 0; cam_y = 0; cam_zoom = 1.0;
    sim_dt = 0.015;
    compute_accel();
}

static void preset_figure8(void) {
    /* Chenciner-Montgomery figure-8 three-body solution */
    clear_all();
    add_body(-0.97000436,  0.24308753,
              0.46620368,  0.43236573, 1,  0);
    add_body( 0.97000436, -0.24308753,
              0.46620368,  0.43236573, 1, 12);
    add_body( 0.0,         0.0,
             -0.93240737, -0.86473146, 1, 24);
    cam_x = 0; cam_y = 0; cam_zoom = 25.0;
    sim_dt = 0.0008;
    compute_accel();
}

static void preset_random(void) {
    clear_all();
    int n = 12 + rand() % 12;
    for (int i = 0; i < n; i++) {
        double x  = (rand() % 200 - 100) * 0.1;
        double y  = (rand() % 200 - 100) * 0.1;
        double vx = (rand() % 60 - 30) * 0.008;
        double vy = (rand() % 60 - 30) * 0.008;
        double m  = 3 + rand() % 40;
        add_body(x, y, vx, vy, m, rand() % 36);
    }
    cam_x = 0; cam_y = 0; cam_zoom = 1.5;
    sim_dt = 0.015;
    compute_accel();
}

/* ---- Coordinate Transform ---- */
static void w2s(double wx, double wy, int *sx, int *sy) {
    *sx = (int)((wx - cam_x) * cam_zoom + term_w * 0.5);
    *sy = (int)((wy - cam_y) * cam_zoom + term_h * 0.5);
}

/* ---- Framebuffer Helpers ---- */
static void fb_clear(void) {
    for (int i = 0; i < term_w * term_h; i++) {
        fb[i].ch = ' ';
        fb[i].fg = 16;
    }
}

static void fb_set(int x, int y, int ch, int fg) {
    if (x >= 0 && x < term_w && y >= 0 && y < term_h) {
        int idx = y * term_w + x;
        fb[idx].ch = ch;
        fb[idx].fg = fg;
    }
}

static void fb_str(int x, int y, const char *s, int fg) {
    for (int i = 0; s[i] && x + i < term_w; i++)
        fb_set(x + i, y, s[i], fg);
}

static char mass_char(double m) {
    if (m < 2)   return '.';
    if (m < 10)  return 'o';
    if (m < 50)  return 'O';
    if (m < 200) return '@';
    return '*';
}

static int glow_r(double m) {
    if (m < 5)   return 0;
    if (m < 30)  return 1;
    if (m < 150) return 2;
    return 3;
}

/* ---- Rendering ---- */
static void render(void) {
    fb_clear();

    /* Trails */
    if (show_trails) {
        for (int i = 0; i < nbodies; i++) {
            Body *b = &bodies[i];
            if (!b->alive || b->tlen < 2) continue;
            for (int t = 0; t < b->tlen; t++) {
                int age = b->tlen - 1 - t;
                int bright = (int)(70.0 * (1.0 - (double)age / b->tlen));
                if (bright < 8) continue;
                int idx = ((b->tidx - 1 - age) % TRAIL_LEN + TRAIL_LEN)
                          % TRAIL_LEN;
                int sx, sy;
                w2s(b->tx[idx], b->ty[idx], &sx, &sy);
                int ch = (bright > 40) ? '.' : ',';
                fb_set(sx, sy, ch, body_fg(b->hue, bright));
            }
        }
    }

    /* Bodies */
    for (int i = 0; i < nbodies; i++) {
        Body *b = &bodies[i];
        if (!b->alive) continue;
        int sx, sy;
        w2s(b->x, b->y, &sx, &sy);

        /* Glow halo */
        int gr = glow_r(b->mass);
        for (int dy = -gr; dy <= gr; dy++) {
            for (int dx = -gr; dx <= gr; dx++) {
                if (dx == 0 && dy == 0) continue;
                if (dx * dx + dy * dy > gr * gr + 1) continue;
                fb_set(sx + dx, sy + dy, '.', body_fg(b->hue, 20));
            }
        }
        /* Body */
        fb_set(sx, sy, mass_char(b->mass), body_fg(b->hue, 95));
    }

    /* Cursor crosshair */
    {
        int sx, sy;
        w2s(cur_x, cur_y, &sx, &sy);
        fb_set(sx, sy, '+', 255);
        if (sx > 0)      fb_set(sx - 1, sy, '-', 244);
        if (sx < term_w-1) fb_set(sx + 1, sy, '-', 244);
        if (sy > 0)      fb_set(sx, sy - 1, '|', 244);
        if (sy < term_h-1) fb_set(sx, sy + 1, '|', 244);
    }

    /* Info panel */
    if (show_info) {
        char buf[72];
        int px = term_w - 38;
        int py = 1;
        if (px < 2) px = 2;
        int pf = 252;  /* light gray */

        fb_str(px, py, "+=== N-Body Gravity Sim ===+", 227); py++;
        fb_str(px, py, "|                           |", 240); py++;

        int alive = 0;
        for (int i = 0; i < nbodies; i++)
            if (bodies[i].alive) alive++;
        snprintf(buf, sizeof(buf), "| Bodies : %-3d", alive);
        fb_str(px, py, buf, pf); py++;

        snprintf(buf, sizeof(buf), "| Time   : %.1f", sim_time);
        fb_str(px, py, buf, pf); py++;

        snprintf(buf, sizeof(buf), "| Speed  : %.4f", sim_dt);
        fb_str(px, py, buf, pf); py++;

        snprintf(buf, sizeof(buf), "| Zoom   : %.1fx", cam_zoom);
        fb_str(px, py, buf, pf); py++;

        /* Total energy */
        double ke = 0, pe = 0;
        for (int i = 0; i < nbodies; i++) {
            if (!bodies[i].alive) continue;
            ke += 0.5 * bodies[i].mass *
                (bodies[i].vx * bodies[i].vx + bodies[i].vy * bodies[i].vy);
            for (int j = i + 1; j < nbodies; j++) {
                if (!bodies[j].alive) continue;
                double dx = bodies[j].x - bodies[i].x;
                double dy = bodies[j].y - bodies[i].y;
                double r = sqrt(dx*dx + dy*dy + SOFTENING*SOFTENING);
                pe -= G_CONST * bodies[i].mass * bodies[j].mass / r;
            }
        }
        snprintf(buf, sizeof(buf), "| Energy : %.1f", ke + pe);
        fb_str(px, py, buf, pf); py++;

        fb_str(px, py, "+---------------------------+", 240); py++;
        fb_str(px, py, "| SPC Pause   +/- Speed     |", 244); py++;
        fb_str(px, py, "| 1   Solar   2   Binary    |", 244); py++;
        fb_str(px, py, "| 3   Fig-8   4   Random    |", 244); py++;
        fb_str(px, py, "| A   Add     D   Delete    |", 244); py++;
        fb_str(px, py, "| Z/X Zoom    T   Trails    |", 244); py++;
        fb_str(px, py, "| C   Clear   R   Reset     |", 244); py++;
        fb_str(px, py, "| I   Info    Q   Quit      |", 244); py++;
        fb_str(px, py, "+===========================+", 240);
    }

    if (paused) {
        int mx = term_w / 2 - 4;
        int my = term_h / 2;
        fb_str(mx - 1, my, "[ PAUSED ]", 227);
    }

    /* Output framebuffer to terminal */
    int pos = 0;
    pos += sprintf(out_buf + pos, "\033[H\033[?25l");
    int prev_fg = -1;
    for (int y = 0; y < term_h; y++) {
        for (int x = 0; x < term_w; x++) {
            Cell *c = &fb[y * term_w + x];
            if (c->fg != prev_fg) {
                pos += sprintf(out_buf + pos, "\033[38;5;%dm", c->fg);
                prev_fg = c->fg;
            }
            out_buf[pos++] = c->ch;
        }
        if (y < term_h - 1) out_buf[pos++] = '\n';
    }
    pos += sprintf(out_buf + pos, "\033[0m");
    (void)!write(1, out_buf, pos);
}

/* ---- Input ---- */
static int read_key(void) {
    unsigned char c;
    if (read(0, &c, 1) != 1) return 0;
    if (c == ESC_KEY) {
        unsigned char s0, s1;
        if (read(0, &s0, 1) != 1) return ESC_KEY;
        if (read(0, &s1, 1) != 1) return ESC_KEY;
        if (s0 == '[') {
            switch (s1) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
            }
        }
        return ESC_KEY;
    }
    return c;
}

static void handle_input(void) {
    int k = read_key();
    if (!k) return;
    double cstep = 5.0 / cam_zoom;

    switch (k) {
    case 'q': case ESC_KEY:
        running = 0; break;
    case ' ':
        paused = !paused; break;
    case '+': case '=':
        sim_dt *= 1.5; break;
    case '-':
        sim_dt = fmax(sim_dt * 0.667, 1e-5); break;
    case '1': preset_solar(); break;
    case '2': preset_binary(); break;
    case '3': preset_figure8(); break;
    case '4': preset_random(); break;
    case 'a': case 'A': {
        double m = 5 + rand() % 45;
        double vx = 0, vy = 0;
        double tm = 0, cx = 0, cy = 0;
        for (int i = 0; i < nbodies; i++) {
            if (!bodies[i].alive) continue;
            tm += bodies[i].mass;
            cx += bodies[i].mass * bodies[i].x;
            cy += bodies[i].mass * bodies[i].y;
        }
        if (tm > 0) {
            cx /= tm; cy /= tm;
            double dx = cur_x - cx, dy = cur_y - cy;
            double r = sqrt(dx*dx + dy*dy);
            if (r > 0.5) {
                double v = sqrt(G_CONST * tm / r);
                vx = -v * dy / r;
                vy =  v * dx / r;
            }
        }
        add_body(cur_x, cur_y, vx, vy, m, rand() % 36);
        compute_accel();
        break;
    }
    case 'd': case 'D': {
        int best = -1; double bd = 1e18;
        for (int i = 0; i < nbodies; i++) {
            if (!bodies[i].alive) continue;
            double dx = bodies[i].x - cur_x;
            double dy = bodies[i].y - cur_y;
            double d = dx*dx + dy*dy;
            if (d < bd) { bd = d; best = i; }
        }
        if (best >= 0) bodies[best].alive = 0;
        break;
    }
    case KEY_UP:    cur_y -= cstep; break;
    case KEY_DOWN:  cur_y += cstep; break;
    case KEY_LEFT:  cur_x -= cstep; break;
    case KEY_RIGHT: cur_x += cstep; break;
    case 'z': case 'Z': cam_zoom *= 1.3; break;
    case 'x': case 'X': cam_zoom = fmax(cam_zoom / 1.3, 0.05); break;
    case 't': case 'T': show_trails = !show_trails; break;
    case 'c': case 'C':
        for (int i = 0; i < nbodies; i++) {
            bodies[i].tlen = 0; bodies[i].tidx = 0;
        }
        break;
    case 'i': case 'I': show_info = !show_info; break;
    case 'r': case 'R':
        cam_x = 0; cam_y = 0; cam_zoom = 1.5;
        cur_x = 0; cur_y = 0;
        break;
    }
}

/* ---- Main ---- */
int main(void) {
    srand((unsigned)time(NULL));
    term_raw();
    atexit(term_restore);

    term_w = 0; term_h = 0;
    term_update_size();

    show_trails = 1;
    show_info   = 1;
    running     = 1;
    paused      = 0;
    cur_x = 0; cur_y = 0;

    preset_solar();

    while (running) {
        handle_input();

        if (!paused) {
            int subs = (sim_dt > 0.01) ? 4 : (sim_dt > 0.002) ? 8 : 15;
            double sdt = sim_dt / subs;
            for (int s = 0; s < subs; s++)
                step(sdt);
        }

        term_update_size();
        render();
        usleep(FRAME_US);
    }

    free(fb);
    free(out_buf);
    return 0;
}
