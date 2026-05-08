/*
 * solarium-5087.c — Terminal Solar System Simulator
 *
 * An interactive, real-time simulation of our solar system featuring all 8 planets
 * with accurate Keplerian orbital mechanics, orbital trails, speed/zoom controls,
 * planet info panels, and beautiful ANSI 256-color rendering.
 *
 * Compile:  gcc -std=c11 -o solarium solarium-5087.c -lm -lncurses
 * Run:      ./solarium
 *
 * Controls:
 *   +/=              Speed up time
 *   -/_              Slow down time
 *   >/.              Zoom in
 *   </,              Zoom out
 *   Arrow keys       Pan view
 *   1-8              Focus on planet (1=Mercury .. 8=Neptune)
 *   0                Focus on Sun
 *   Tab              Cycle through bodies
 *   i                Toggle info panel
 *   t                Toggle orbital trails
 *   l                Toggle labels
 *   p                Pause / resume
 *   r                Reset view
 *   h                Toggle help
 *   q / Esc          Quit
 *
 * Requires: ncurses, libm (standard on most Unix systems)
 * Author:   Creative C Programs Collection
 */

#define _POSIX_C_SOURCE 199309L
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ncurses.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>

/* ── Constants ────────────────────────────────────────────────────────── */

#define NUM_PLANETS   8
#define TRAIL_MAX     800
#define AU_TO_KM      149597870.7
#define DAY_PER_SEC   86400.0

/* ── Color palette (ANSI 256-color indices) ──────────────────────────── */

enum {
    C_SUN      = 220,
    C_MERCURY  = 244,
    C_VENUS    = 180,
    C_EARTH    = 33,
    C_MARS     = 160,
    C_JUPITER  = 172,
    C_SATURN   = 178,
    C_URANUS   = 67,
    C_NEPTUNE  = 62,
    C_ORBIT    = 236,
    C_TRAIL    = 240,
    C_TEXT     = 252,
    C_DIM      = 242,
    C_HIGHLIGHT= 214,
    C_BG       = 16,
    C_STAR     = 248,
};

/* ── Planet data structure ───────────────────────────────────────────── */

typedef struct {
    const char *name;
    double semi_major;   /* AU */
    double eccentricity;
    double period;       /* days */
    double mass;         /* kg */
    double radius;       /* km (real) */
    double inclination;  /* degrees (simplified) */
    int    color;
    char   symbol;       /* ASCII symbol for rendering */
    int    glyph_size;   /* display size (1-3) */
} Planet;

static const Planet planets[NUM_PLANETS] = {
    /* name      a(AU)   e       T(days)   mass(kg)       R(km)   inc  color  sym size */
    {"Mercury",  0.387,  0.2056,    87.97, 3.301e23,     2439.7,  7.00, C_MERCURY, 'o', 1},
    {"Venus",    0.723,  0.0068,   224.70, 4.867e24,     6051.8,  3.39, C_VENUS,   'O', 1},
    {"Earth",    1.000,  0.0167,   365.25, 5.972e24,     6371.0,  0.00, C_EARTH,   '@', 1},
    {"Mars",     1.524,  0.0934,   687.00, 6.417e23,     3389.5,  1.85, C_MARS,    'o', 1},
    {"Jupiter",  5.203,  0.0489,  4332.59, 1.898e27,    69911.0,  1.31, C_JUPITER, 'J', 2},
    {"Saturn",   9.537,  0.0565, 10759.22, 5.683e26,    58232.0,  2.49, C_SATURN,  'S', 2},
    {"Uranus",  19.191,  0.0457, 30688.50, 8.681e25,    25362.0,  0.77, C_URANUS,  'U', 1},
    {"Neptune", 30.069,  0.0113, 60182.00, 1.024e26,    24622.0,  1.77, C_NEPTUNE, 'N', 1},
};

/* ── Trail ring buffer ───────────────────────────────────────────────── */

typedef struct {
    double x[TRAIL_MAX];
    double y[TRAIL_MAX];
    int    head;
    int    count;
} Trail;

/* ── Star field ──────────────────────────────────────────────────────── */

#define STAR_COUNT 200

typedef struct {
    int sx, sy;
    int brightness;
} Star;

/* ── Application state ───────────────────────────────────────────────── */

typedef struct {
    double  sim_time;        /* simulation days elapsed */
    double  time_speed;      /* days per real second */
    double  zoom;            /* pixels per AU */
    double  pan_x, pan_y;   /* pan offset in AU */
    int     focus;           /* focused body index (-1=free, 0=sun, 1..8=planet) */
    int     show_trails;
    int     show_labels;
    int     show_info;
    int     show_help;
    int     paused;
    Trail   trails[NUM_PLANETS];
    Star    stars[STAR_COUNT];
    int     screen_w, screen_h;
    double  initial_angle[NUM_PLANETS]; /* random starting orbital angle */
} State;

/* ── Kepler equation solver (Newton-Raphson) ─────────────────────────── */

static double solve_kepler(double M, double e, double tol) {
    /* Solve M = E - e*sin(E) for E */
    double E = M;
    for (int i = 0; i < 50; i++) {
        double dE = (E - e * sin(E) - M) / (1.0 - e * cos(E));
        E -= dE;
        if (fabs(dE) < tol) break;
    }
    return E;
}

/* Get planet position in AU (heliocentric, 2D projection) */
static void planet_pos(const Planet *p, double sim_time,
                       double init_angle, double *x, double *y) {
    double M = 2.0 * M_PI * sim_time / p->period + init_angle;
    double E = solve_kepler(M, p->eccentricity, 1e-8);
    double cosE = cos(E);
    double sinE = sin(E);
    *x = p->semi_major * (cosE - p->eccentricity);
    *y = p->semi_major * sqrt(1.0 - p->eccentricity * p->eccentricity) * sinE;
}

/* ── Coordinate transform ────────────────────────────────────────────── */

/* AU -> screen coordinates */
static void au_to_screen(State *st, double ax, double ay, int *sx, int *sy) {
    int cx = st->screen_w / 2;
    int cy = st->screen_h / 2;
    *sx = (int)(cx + (ax - st->pan_x) * st->zoom);
    *sy = (int)(cy - (ay - st->pan_y) * st->zoom);
}

/* ── Drawing helpers ─────────────────────────────────────────────────── */

static void draw_char(WINDOW *win, int y, int x, char ch, int color) {
    if (y < 0 || y >= getmaxy(win) || x < 0 || x >= getmaxx(win)) return;
    wattron(win, COLOR_PAIR(color % 256 + 1));
    mvwaddch(win, y, x, (unsigned char)ch);
    wattroff(win, COLOR_PAIR(color % 256 + 1));
}

static void draw_string(WINDOW *win, int y, int x, const char *s, int color) {
    wattron(win, COLOR_PAIR(color % 256 + 1));
    mvwaddstr(win, y, x, s);
    wattroff(win, COLOR_PAIR(color % 256 + 1));
}

/* ── Initialize stars ────────────────────────────────────────────────── */

static void init_stars(State *st) {
    srand((unsigned)time(NULL));
    for (int i = 0; i < STAR_COUNT; i++) {
        st->stars[i].sx = rand() % 300;
        st->stars[i].sy = rand() % 200;
        st->stars[i].brightness = 232 + rand() % 24;
    }
}

/* ── Draw orbital path (ellipse) ─────────────────────────────────────── */

static void draw_orbit(WINDOW *win, State *st, const Planet *p, double init_angle) {
    double b = p->semi_major * sqrt(1.0 - p->eccentricity * p->eccentricity);
    double cx_off = -p->semi_major * p->eccentricity;
    int steps = (int)(p->semi_major * st->zoom * 0.8);
    if (steps < 40) steps = 40;
    if (steps > 360) steps = 360;

    int prev_sx = -999, prev_sy = -999;
    for (int i = 0; i <= steps; i++) {
        double E = 2.0 * M_PI * i / steps + init_angle;
        double ax = cx_off + p->semi_major * cos(E);
        double ay = b * sin(E);
        int sx, sy;
        au_to_screen(st, ax, ay, &sx, &sy);
        if (sx != prev_sx || sy != prev_sy) {
            draw_char(win, sy, sx, '.', C_ORBIT);
        }
        prev_sx = sx;
        prev_sy = sy;
    }
}

/* ── Draw trail ──────────────────────────────────────────────────────── */

static void draw_trail(WINDOW *win, State *st, int idx) {
    Trail *t = &st->trails[idx];
    int color = planets[idx].color;
    for (int i = 0; i < t->count; i++) {
        int pos = (t->head - t->count + i + TRAIL_MAX) % TRAIL_MAX;
        int sx, sy;
        au_to_screen(st, t->x[pos], t->y[pos], &sx, &sy);
        /* Fade: older points are dimmer */
        int fade = 232 + (i * 16 / (t->count + 1));
        if (fade > color) fade = color;
        draw_char(win, sy, sx, '.', fade);
    }
}

/* ── Draw the Sun ────────────────────────────────────────────────────── */

static void draw_sun(WINDOW *win, State *st) {
    int sx, sy;
    au_to_screen(st, 0.0, 0.0, &sx, &sy);
    /* Sun glow */
    draw_char(win, sy - 1, sx,   '~', C_SUN);
    draw_char(win, sy + 1, sx,   '~', C_SUN);
    draw_char(win, sy,     sx-1, '~', C_SUN);
    draw_char(win, sy,     sx+1, '~', C_SUN);
    draw_char(win, sy,     sx,   '*', COLOR_PAIR(C_SUN % 256 + 1) | A_BOLD);
    if (st->show_labels) {
        draw_string(win, sy + 2, sx - 1, "Sun", C_SUN);
    }
}

/* ── Draw a planet ───────────────────────────────────────────────────── */

static void draw_planet(WINDOW *win, State *st, int idx) {
    const Planet *p = &planets[idx];
    double x, y;
    planet_pos(p, st->sim_time, st->initial_angle[idx], &x, &y);

    int sx, sy;
    au_to_screen(st, x, y, &sx, &sy);

    /* Planet symbol */
    int attr = COLOR_PAIR(p->color % 256 + 1);
    if (st->focus == idx + 1) attr |= A_BOLD | A_UNDERLINE;
    wattron(win, attr);
    mvwaddch(win, sy, sx, (unsigned char)p->symbol);
    wattroff(win, attr);

    /* Saturn's ring */
    if (idx == 5) { /* Saturn */
        draw_char(win, sy, sx - 1, '-', C_SATURN);
        draw_char(win, sy, sx + 1, '-', C_SATURN);
    }

    /* Label */
    if (st->show_labels) {
        draw_string(win, sy - 1, sx - (int)(strlen(p->name) / 2), p->name, p->color);
    }

    /* Focus crosshair */
    if (st->focus == idx + 1) {
        draw_char(win, sy - 1, sx, '+', C_HIGHLIGHT);
        draw_char(win, sy + 1, sx, '+', C_HIGHLIGHT);
        draw_char(win, sy, sx - 2, '+', C_HIGHLIGHT);
        draw_char(win, sy, sx + 2, '+', C_HIGHLIGHT);
    }
}

/* ── Info panel ──────────────────────────────────────────────────────── */

static void draw_info_panel(WINDOW *win, State *st) {
    int px = st->screen_w - 36;
    int py = 1;
    if (px < 10) px = 10;

    /* Panel background */
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 35; x++)
            draw_char(win, py + y, px + x, ' ', C_BG);

    /* Border */
    for (int y = 0; y < 16; y++) {
        draw_char(win, py + y, px,       '|', C_DIM);
        draw_char(win, py + y, px + 34,  '|', C_DIM);
    }
    for (int x = 0; x < 35; x++) {
        draw_char(win, py,      px + x, '-', C_DIM);
        draw_char(win, py + 15, px + x, '-', C_DIM);
    }

    int idx = st->focus - 1;
    if (idx < 0 || idx >= NUM_PLANETS) {
        draw_string(win, py + 1, px + 2, "   SOLAR SYSTEM INFO   ", C_HIGHLIGHT);
        char buf[40];
        snprintf(buf, sizeof(buf), "Sim time: %.1f days", st->sim_time);
        draw_string(win, py + 3, px + 2, buf, C_TEXT);
        snprintf(buf, sizeof(buf), "  Speed:  %.2f days/s", st->time_speed);
        draw_string(win, py + 4, px + 2, buf, C_TEXT);
        snprintf(buf, sizeof(buf), "  Zoom:   %.1f px/AU", st->zoom);
        draw_string(win, py + 5, px + 2, buf, C_TEXT);
        double years = st->sim_time / 365.25;
        snprintf(buf, sizeof(buf), "  Elapsed:%.2f years", years);
        draw_string(win, py + 6, px + 2, buf, C_TEXT);
        draw_string(win, py + 8, px + 2, "Press 1-8 for planets", C_DIM);
        draw_string(win, py + 9, px + 2, "Press 0 for Sun", C_DIM);
        draw_string(win, py + 10, px + 2, "Press h for help", C_DIM);
    } else {
        const Planet *p = &planets[idx];
        double x, y;
        planet_pos(p, st->sim_time, st->initial_angle[idx], &x, &y);
        double dist = sqrt(x * x + y * y);

        char buf[40];
        draw_string(win, py + 1, px + 2, p->name, p->color);
        snprintf(buf, sizeof(buf), "Distance: %.4f AU", dist);
        draw_string(win, py + 3, px + 2, buf, C_TEXT);
        snprintf(buf, sizeof(buf), "         (%.0f M km)", dist * AU_TO_KM / 1e6);
        draw_string(win, py + 4, px + 2, buf, C_DIM);
        snprintf(buf, sizeof(buf), "Orbit:    %.2f days", p->period);
        draw_string(win, py + 5, px + 2, buf, C_TEXT);
        snprintf(buf, sizeof(buf), "          %.2f years", p->period / 365.25);
        draw_string(win, py + 6, px + 2, buf, C_DIM);
        snprintf(buf, sizeof(buf), "Semi-maj: %.3f AU", p->semi_major);
        draw_string(win, py + 7, px + 2, buf, C_TEXT);
        snprintf(buf, sizeof(buf), "Ecc:      %.4f", p->eccentricity);
        draw_string(win, py + 8, px + 2, buf, C_TEXT);
        snprintf(buf, sizeof(buf), "Radius:   %.0f km", p->radius);
        draw_string(win, py + 9, px + 2, buf, C_TEXT);
        snprintf(buf, sizeof(buf), "Mass:     %.2e kg", p->mass);
        draw_string(win, py + 10, px + 2, buf, C_TEXT);

        /* Orbital velocity */
        double v = 2.0 * M_PI * p->semi_major * AU_TO_KM / (p->period * DAY_PER_SEC);
        snprintf(buf, sizeof(buf), "Velocity: %.1f km/s", v);
        draw_string(win, py + 11, px + 2, buf, C_TEXT);

        /* Current position */
        snprintf(buf, sizeof(buf), "Pos: (%.3f, %.3f) AU", x, y);
        draw_string(win, py + 12, px + 2, buf, C_DIM);

        /* Angle from Sun */
        double angle = atan2(y, x) * 180.0 / M_PI;
        if (angle < 0) angle += 360.0;
        snprintf(buf, sizeof(buf), "Angle:    %.1f deg", angle);
        draw_string(win, py + 13, px + 2, buf, C_DIM);
    }
}

/* ── Help overlay ────────────────────────────────────────────────────── */

static void draw_help(WINDOW *win, State *st) {
    int cx = st->screen_w / 2 - 20;
    int cy = st->screen_h / 2 - 8;

    for (int y = 0; y < 17; y++)
        for (int x = 0; x < 40; x++)
            draw_char(win, cy + y, cx + x, ' ', C_BG);
    for (int y = 0; y < 17; y++) {
        draw_char(win, cy + y, cx,      '|', C_DIM);
        draw_char(win, cy + y, cx + 39, '|', C_DIM);
    }
    for (int x = 0; x < 40; x++) {
        draw_char(win, cy,      cx + x, '-', C_DIM);
        draw_char(win, cy + 16, cx + x, '-', C_DIM);
    }

    draw_string(win, cy + 1,  cx + 12, "SOLARIUM CONTROLS", C_HIGHLIGHT);
    draw_string(win, cy + 3,  cx + 2,  "+/-    Speed up / slow down", C_TEXT);
    draw_string(win, cy + 4,  cx + 2,  "<> ,.  Zoom out / zoom in",   C_TEXT);
    draw_string(win, cy + 5,  cx + 2,  "Arrows  Pan view",            C_TEXT);
    draw_string(win, cy + 6,  cx + 2,  "1-8     Focus planet",        C_TEXT);
    draw_string(win, cy + 7,  cx + 2,  "0       Focus Sun",           C_TEXT);
    draw_string(win, cy + 8,  cx + 2,  "Tab     Cycle bodies",        C_TEXT);
    draw_string(win, cy + 9,  cx + 2,  "i       Info panel",          C_TEXT);
    draw_string(win, cy + 10, cx + 2,  "t       Orbital trails",      C_TEXT);
    draw_string(win, cy + 11, cx + 2,  "l       Labels",              C_TEXT);
    draw_string(win, cy + 12, cx + 2,  "p       Pause / resume",      C_TEXT);
    draw_string(win, cy + 13, cx + 2,  "r       Reset view",          C_TEXT);
    draw_string(win, cy + 14, cx + 2,  "q/Esc   Quit",                C_TEXT);
}

/* ── HUD (top bar) ───────────────────────────────────────────────────── */

static void draw_hud(WINDOW *win, State *st) {
    char buf[256];
    double years = st->sim_time / 365.25;

    /* Top bar background */
    for (int x = 0; x < st->screen_w; x++)
        draw_char(win, 0, x, ' ', C_BG);

    snprintf(buf, sizeof(buf),
             " SOLARIUM | Day %.0f (%.2f yr) | Speed: %.2f d/s | Zoom: %.0f px/AU%s",
             st->sim_time, years, st->time_speed, st->zoom,
             st->paused ? " | PAUSED" : "");
    draw_string(win, 0, 0, buf, C_HIGHLIGHT);

    /* Focused body name */
    if (st->focus >= 1 && st->focus <= NUM_PLANETS) {
        snprintf(buf, sizeof(buf), " Focus: %s ",
                 planets[st->focus - 1].name);
        draw_string(win, 0, st->screen_w - (int)strlen(buf) - 1, buf,
                    planets[st->focus - 1].color);
    } else if (st->focus == 0) {
        draw_string(win, 0, st->screen_w - 12, " Focus: Sun ", C_SUN);
    }
}

/* ── Bottom bar ──────────────────────────────────────────────────────── */

static void draw_bottom_bar(WINDOW *win, State *st) {
    int y = st->screen_h - 1;
    for (int x = 0; x < st->screen_w; x++)
        draw_char(win, y, x, ' ', C_BG);

    /* Planet quick-select bar */
    int pos = 2;
    draw_string(win, y, pos, "0:Sun", (st->focus == 0) ? C_SUN : C_DIM);
    pos += 7;
    for (int i = 0; i < NUM_PLANETS; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d:%s", i + 1, planets[i].name);
        int col = (st->focus == i + 1) ? planets[i].color : C_DIM;
        draw_string(win, y, pos, buf, col);
        pos += (int)strlen(buf) + 2;
        if (pos > st->screen_w - 15) break;
    }

    /* Toggle states */
    char toggles[64];
    snprintf(toggles, sizeof(toggles), "[%cTrails|%cInfo|%cLabels]",
             st->show_trails ? '#' : ' ',
             st->show_info   ? '#' : ' ',
             st->show_labels ? '#' : ' ');
    draw_string(win, y, st->screen_w - (int)strlen(toggles) - 2, toggles, C_DIM);
}

/* ── Mini-map (overview of full solar system) ────────────────────────── */

static void draw_minimap(WINDOW *win, State *st) {
    int mm_w = 28, mm_h = 14;
    int mm_x = 1, mm_y = st->screen_h - mm_h - 2;
    if (mm_y < 3) return;

    /* Background */
    for (int y = 0; y < mm_h; y++)
        for (int x = 0; x < mm_w; x++)
            draw_char(win, mm_y + y, mm_x + x, ' ', C_BG);

    /* Border */
    for (int y = 0; y < mm_h; y++) {
        draw_char(win, mm_y + y, mm_x,        '|', C_DIM);
        draw_char(win, mm_y + y, mm_x + mm_w - 1, '|', C_DIM);
    }
    for (int x = 0; x < mm_w; x++) {
        draw_char(win, mm_y,           mm_x + x, '-', C_DIM);
        draw_char(win, mm_y + mm_h - 1, mm_x + x, '-', C_DIM);
    }

    double mm_scale = (mm_w - 4) / 2.0 / 32.0; /* 32 AU fits Neptune */
    int mcx = mm_x + mm_w / 2;
    int mcy = mm_y + mm_h / 2;

    /* Sun */
    draw_char(win, mcy, mcx, '*', C_SUN);

    /* Planets */
    for (int i = 0; i < NUM_PLANETS; i++) {
        double px, py;
        planet_pos(&planets[i], st->sim_time, st->initial_angle[i], &px, &py);
        int sx = mcx + (int)(px * mm_scale);
        int sy = mcy - (int)(py * mm_scale);
        draw_char(win, sy, sx, planets[i].symbol, planets[i].color);
    }

    /* Viewport rectangle */
    double vp_w = st->screen_w / st->zoom;
    double vp_h = st->screen_h / st->zoom;
    int vx1 = mcx + (int)((st->pan_x - vp_w / 2) * mm_scale);
    int vx2 = mcx + (int)((st->pan_x + vp_w / 2) * mm_scale);
    int vy1 = mcy - (int)((st->pan_y + vp_h / 2) * mm_scale);
    int vy2 = mcy - (int)((st->pan_y - vp_h / 2) * mm_scale);

    /* Clamp to minimap area */
    if (vx1 < mm_x + 1) vx1 = mm_x + 1;
    if (vx2 > mm_x + mm_w - 2) vx2 = mm_x + mm_w - 2;
    if (vy1 < mm_y + 1) vy1 = mm_y + 1;
    if (vy2 > mm_y + mm_h - 2) vy2 = mm_y + mm_h - 2;

    for (int x = vx1; x <= vx2; x++) {
        if (x >= mm_x + 1 && x < mm_x + mm_w - 1) {
            draw_char(win, vy1, x, '-', C_HIGHLIGHT);
            draw_char(win, vy2, x, '-', C_HIGHLIGHT);
        }
    }
    for (int y = vy1; y <= vy2; y++) {
        if (y >= mm_y + 1 && y < mm_y + mm_h - 1) {
            draw_char(win, y, vx1, '|', C_HIGHLIGHT);
            draw_char(win, y, vx2, '|', C_HIGHLIGHT);
        }
    }
}

/* ── Init colors ─────────────────────────────────────────────────────── */

static void init_colors(void) {
    if (has_colors() && can_change_color()) {
        start_color();
        /* Simple mapping: pair N+1 for ANSI color N */
        for (int i = 0; i < 256; i++) {
            init_pair(i + 1, i, COLOR_BLACK);
        }
    } else if (has_colors()) {
        start_color();
        /* Fallback: use basic colors */
        for (int i = 0; i < 8; i++)
            init_pair(i + 1, i, COLOR_BLACK);
    }
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0);
    nodelay(stdscr, TRUE);
    mousemask(0, NULL);

    init_colors();

    State st = {
        .sim_time    = 0.0,
        .time_speed  = 5.0,   /* 5 days per real second */
        .zoom        = 40.0,  /* pixels per AU (inner system view) */
        .pan_x       = 0.0,
        .pan_y       = 0.0,
        .focus       = 0,     /* start focused on Sun */
        .show_trails = 1,
        .show_labels = 1,
        .show_info   = 1,
        .show_help   = 0,
        .paused      = 0,
    };
    memset(st.trails, 0, sizeof(st.trails));

    getmaxyx(stdscr, st.screen_h, st.screen_w);

    /* Randomize initial orbital angles for variety */
    srand((unsigned)time(NULL));
    for (int i = 0; i < NUM_PLANETS; i++) {
        st.initial_angle[i] = (double)(rand() % 628) / 100.0;
    }

    init_stars(&st);

    struct timespec last_time;
    clock_gettime(CLOCK_MONOTONIC, &last_time);

    int running = 1;
    int trail_counter = 0;

    while (running) {
        /* ── Input handling ─────────────────────────────────────────── */
        int ch;
        while ((ch = getch()) != ERR) {
            switch (ch) {
            case 'q': case 27: /* ESC */
                running = 0;
                break;
            case '+': case '=':
                st.time_speed *= 1.5;
                if (st.time_speed > 1e6) st.time_speed = 1e6;
                break;
            case '-': case '_':
                st.time_speed /= 1.5;
                if (st.time_speed < 0.01) st.time_speed = 0.01;
                break;
            case '>': case '.':
                st.zoom *= 1.3;
                if (st.zoom > 2000) st.zoom = 2000;
                break;
            case '<': case ',':
                st.zoom /= 1.3;
                if (st.zoom < 0.5) st.zoom = 0.5;
                break;
            case KEY_UP:
                st.pan_y += 2.0 / st.zoom;
                break;
            case KEY_DOWN:
                st.pan_y -= 2.0 / st.zoom;
                break;
            case KEY_LEFT:
                st.pan_x -= 2.0 / st.zoom;
                break;
            case KEY_RIGHT:
                st.pan_x += 2.0 / st.zoom;
                break;
            case '0':
                st.focus = 0;
                st.pan_x = 0; st.pan_y = 0;
                break;
            case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8':
                st.focus = ch - '0';
                break;
            case '\t': /* Tab: cycle */
                st.focus++;
                if (st.focus > NUM_PLANETS) st.focus = 0;
                break;
            case 'i':
                st.show_info = !st.show_info;
                break;
            case 't':
                st.show_trails = !st.show_trails;
                if (!st.show_trails) {
                    for (int i = 0; i < NUM_PLANETS; i++)
                        st.trails[i].count = 0;
                }
                break;
            case 'l':
                st.show_labels = !st.show_labels;
                break;
            case 'p':
                st.paused = !st.paused;
                break;
            case 'r':
                st.pan_x = 0; st.pan_y = 0;
                st.zoom = 40.0;
                st.time_speed = 5.0;
                st.focus = 0;
                for (int i = 0; i < NUM_PLANETS; i++)
                    st.trails[i].count = 0;
                break;
            case 'h':
                st.show_help = !st.show_help;
                break;
            }
        }

        /* ── Time step ─────────────────────────────────────────────── */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double dt = (now.tv_sec - last_time.tv_sec) +
                    (now.tv_nsec - last_time.tv_nsec) / 1e9;
        last_time = now;

        if (dt > 0.1) dt = 0.1; /* cap for alt-tab etc. */
        if (!st.paused) {
            st.sim_time += dt * st.time_speed;
        }

        /* Update screen size */
        int new_h, new_w;
        getmaxyx(stdscr, new_h, new_w);
        if (new_h != st.screen_h || new_w != st.screen_w) {
            st.screen_h = new_h;
            st.screen_w = new_w;
        }

        /* Focus tracking: center view on focused body */
        if (st.focus >= 1 && st.focus <= NUM_PLANETS) {
            double fx, fy;
            planet_pos(&planets[st.focus - 1], st.sim_time,
                       st.initial_angle[st.focus - 1], &fx, &fy);
            /* Smooth follow */
            st.pan_x += (fx - st.pan_x) * 0.15;
            st.pan_y += (fy - st.pan_y) * 0.15;
        } else if (st.focus == 0) {
            st.pan_x += (0 - st.pan_x) * 0.15;
            st.pan_y += (0 - st.pan_y) * 0.15;
        }

        /* Update trails */
        if (st.show_trails && !st.paused) {
            trail_counter++;
            if (trail_counter >= 2) {
                trail_counter = 0;
                for (int i = 0; i < NUM_PLANETS; i++) {
                    double px, py;
                    planet_pos(&planets[i], st.sim_time,
                               st.initial_angle[i], &px, &py);
                    Trail *t = &st.trails[i];
                    t->x[t->head] = px;
                    t->y[t->head] = py;
                    t->head = (t->head + 1) % TRAIL_MAX;
                    if (t->count < TRAIL_MAX) t->count++;
                }
            }
        }

        /* ── Render ─────────────────────────────────────────────────── */
        werase(stdscr);

        /* Background stars */
        for (int i = 0; i < STAR_COUNT; i++) {
            int sy = st.stars[i].sy % st.screen_h;
            int sx = st.stars[i].sx % st.screen_w;
            if (sy > 0 && sy < st.screen_h - 1) {
                char star_ch = (i % 5 == 0) ? '+' : '.';
                draw_char(stdscr, sy, sx, star_ch, st.stars[i].brightness);
            }
        }

        /* Orbital paths */
        for (int i = 0; i < NUM_PLANETS; i++) {
            draw_orbit(stdscr, &st, &planets[i], st.initial_angle[i]);
        }

        /* Trails */
        if (st.show_trails) {
            for (int i = 0; i < NUM_PLANETS; i++) {
                draw_trail(stdscr, &st, i);
            }
        }

        /* Sun */
        draw_sun(stdscr, &st);

        /* Planets */
        for (int i = 0; i < NUM_PLANETS; i++) {
            draw_planet(stdscr, &st, i);
        }

        /* UI overlays */
        draw_hud(stdscr, &st);
        draw_bottom_bar(stdscr, &st);
        draw_minimap(stdscr, &st);

        if (st.show_info) {
            draw_info_panel(stdscr, &st);
        }

        if (st.show_help) {
            draw_help(stdscr, &st);
        }

        wrefresh(stdscr);

        /* ── Frame delay (~30 FPS) ──────────────────────────────────── */
        struct timespec frame_delay = {0, 33000000}; /* 33ms */
        nanosleep(&frame_delay, NULL);
    }

    endwin();
    return 0;
}
