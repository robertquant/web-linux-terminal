/*
 * lsystem-9296.c — Terminal L-System Fractal Generator
 *
 * COMPILATION:
 *   gcc -O2 -std=c99 -Wall -lm -o lsystem lsystem-9296.c
 *   (or just: cc -O2 -lm -o lsystem lsystem-9296.c)
 *
 * RUN:
 *   ./lsystem
 *
 * An interactive terminal application that generates and renders 10 classic
 * Lindenmayer System (L-System) fractals using ASCII art with ANSI 256-color
 * gradients. Features direction-aware line rendering, 6 color palettes,
 * zoom/pan, animated step-through generation, and density visualization.
 *
 * FRACTALS:
 *   1. Koch Snowflake       6. Gosper Flowsnake
 *   2. Sierpinski Arrowhead 7. Levy C Curve
 *   3. Dragon Curve         8. Fractal Tree
 *   4. Fractal Plant        9. Koch Island
 *   5. Hilbert Curve       10. Pentigree
 *
 * CONTROLS:
 *   1-9,0   Select fractal        +/-  Adjust iterations
 *   C       Cycle color palette    R    Regenerate
 *   Z / X   Zoom in / out          ←↑↓→ Pan view
 *   Space   Auto-fit view          A    Animate (step iterations)
 *   Q / Esc Quit
 *
 * REQUIRES: C99, POSIX terminal (Linux/macOS), ANSI color support.
 */

#define _DEFAULT_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_STR   4000000
#define MAX_SEGS  600000
#define NFRACS    10
#define NPALS     6
#define STACKSZ   512

/* =======================================================================
 * Terminal Handling
 * ======================================================================= */

static struct termios orig_tio;

static void term_restore(void) {
    tcsetattr(0, TCSANOW, &orig_tio);
    fprintf(stdout, "\033[?25h\033[0m");
    fflush(stdout);
}

static void term_raw(void) {
    struct termios t;
    tcgetattr(0, &orig_tio);
    t = orig_tio;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    setvbuf(stdout, NULL, _IONBF, 0);
}

static void term_size(int *w, int *h) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) < 0 || ws.ws_col < 1) {
        *w = 80; *h = 24; return;
    }
    *w = ws.ws_col;
    *h = ws.ws_row;
}

/* Read a single key; returns ASCII, or 'U'/'D'/'L'/'R' for arrows */
static int keypress(void) {
    unsigned char c;
    if (read(0, &c, 1) != 1) return -1;
    if (c == 27) {
        unsigned char c2, c3;
        if (read(0, &c2, 1) != 1) return 27;
        if (read(0, &c3, 1) != 1) return 27;
        if (c2 == '[') {
            switch (c3) {
                case 'A': return 0x81; /* up    */
                case 'B': return 0x82; /* down  */
                case 'C': return 0x83; /* right */
                case 'D': return 0x84; /* left  */
            }
        }
        return 27;
    }
    return c;
}

/* =======================================================================
 * L-System Definitions
 * ======================================================================= */

typedef struct {
    const char *name;
    const char *axiom;
    const char *rules[8];   /* NULL-terminated, format: "X=replacement" */
    double angle;           /* turn angle in degrees */
    int def_iter;           /* default iteration count */
    int max_iter;           /* maximum safe iteration count */
    const char *draw;       /* characters that cause turtle forward movement */
} LDef;

static const LDef fractals[NFRACS] = {
    {"Koch Snowflake", "F--F--F",
     {"F=F+F--F+F", NULL},
     60, 4, 6, "F"},

    {"Sierpinski Arrowhead", "A",
     {"A=B-A-B", "B=A+B+A", NULL},
     60, 7, 10, "AB"},

    {"Dragon Curve", "FX",
     {"X=X+YF+", "Y=-FX-Y", NULL},
     90, 12, 17, "F"},

    {"Fractal Plant", "X",
     {"X=F+[[X]-X]-F[-FX]+X", "F=FF", NULL},
     25, 5, 7, "F"},

    {"Hilbert Curve", "A",
     {"A=-BF+AFA+FB-", "B=+AF-BFB-FA+", NULL},
     90, 5, 7, "F"},

    {"Gosper Flowsnake", "A",
     {"A=A-B--B+A++AA+B-", "B=+A-BB--B-A++A+B", NULL},
     60, 4, 5, "AB"},

    {"Levy C Curve", "F",
     {"F=+F--F+", NULL},
     45, 12, 17, "F"},

    {"Fractal Tree", "F",
     {"F=FF+[+F-F-F]-[-F+F+F]", NULL},
     22.5, 4, 5, "F"},

    {"Koch Island", "F+F+F+F",
     {"F=F+F-F-FF+F+F-F", NULL},
     90, 3, 4, "F"},

    {"Pentigree", "F-F-F-F-F",
     {"F=F-F++F+F-F", NULL},
     72, 4, 5, "F"},
};

/* =======================================================================
 * L-System String Expansion
 *   Iteratively applies production rules to expand the axiom.
 * ======================================================================= */

static char *lsys_expand(const LDef *d, int iters) {
    /* Build a lookup table: rule_map[char] = replacement string */
    const char *rule_map[256] = {NULL};
    for (int i = 0; d->rules[i]; i++) {
        unsigned char ch = (unsigned char)d->rules[i][0];
        rule_map[ch] = d->rules[i] + 2; /* skip "C=" */
    }

    char *cur = malloc(MAX_STR);
    char *nxt = malloc(MAX_STR);
    if (!cur || !nxt) { free(cur); free(nxt); return NULL; }

    strncpy(cur, d->axiom, MAX_STR - 1);
    cur[MAX_STR - 1] = '\0';

    for (int it = 0; it < iters; it++) {
        int pos = 0;
        for (int i = 0; cur[i] && pos < MAX_STR - 100; i++) {
            unsigned char ch = (unsigned char)cur[i];
            const char *rep = rule_map[ch];
            if (rep) {
                int len = (int)strlen(rep);
                if (pos + len >= MAX_STR - 1) break;
                memcpy(nxt + pos, rep, len);
                pos += len;
            } else {
                nxt[pos++] = cur[i];
            }
        }
        nxt[pos] = '\0';
        char *tmp = cur; cur = nxt; nxt = tmp;
    }

    free(nxt);
    return cur;
}

/* =======================================================================
 * Turtle Interpretation
 *   Walks the expanded L-system string and produces line segments.
 *   F/draw chars move forward, +/- turn, [/] push/pop state.
 * ======================================================================= */

typedef struct {
    double x1, y1, x2, y2;
    int depth;
} Seg;

static int turtle_interpret(const char *str, double angle_deg,
                            const char *draw_chars,
                            Seg *segs, int max_segs) {
    double x = 0, y = 0, a = 0;
    double rad = angle_deg * M_PI / 180.0;
    int nsegs = 0, depth = 0;
    int sp = 0;

    /* Push-down stack for [ ] */
    double sx[STACKSZ], sy[STACKSZ], sa[STACKSZ];
    int sd[STACKSZ];

    /* Lookup table for drawing characters */
    char is_draw[256] = {0};
    for (const char *p = draw_chars; *p; p++)
        is_draw[(unsigned char)*p] = 1;

    for (int i = 0; str[i]; i++) {
        unsigned char c = (unsigned char)str[i];
        if (is_draw[c]) {
            double nx = x + cos(a);
            double ny = y + sin(a);
            if (nsegs < max_segs) {
                segs[nsegs++] = (Seg){x, y, nx, ny, depth};
            }
            x = nx; y = ny;
        } else if (c == '+') {
            a += rad;       /* turn left (counter-clockwise) */
        } else if (c == '-') {
            a -= rad;       /* turn right (clockwise) */
        } else if (c == '[') {
            if (sp < STACKSZ) {
                sx[sp] = x; sy[sp] = y; sa[sp] = a; sd[sp] = depth;
                sp++;
            }
            depth++;
        } else if (c == ']') {
            if (sp > 0) {
                sp--;
                x = sx[sp]; y = sy[sp]; a = sa[sp]; depth = sd[sp];
            }
        }
    }

    return nsegs;
}

/* =======================================================================
 * Color: HSV → ANSI 256-color
 *   Maps hue/saturation/value to the nearest ANSI 256-color cube entry.
 * ======================================================================= */

static int hsv_to_ansi(double h, double s, double v) {
    double r, g, b;
    h = fmod(h, 360.0);
    if (h < 0) h += 360.0;
    int hi = (int)(h / 60.0) % 6;
    double f = h / 60.0 - (int)(h / 60.0);
    double p = v * (1.0 - s);
    double q = v * (1.0 - s * f);
    double t = v * (1.0 - s * (1.0 - f));
    switch (hi) {
        case 0: r=v; g=t; b=p; break;
        case 1: r=q; g=v; b=p; break;
        case 2: r=p; g=v; b=t; break;
        case 3: r=p; g=q; b=v; break;
        case 4: r=t; g=p; b=v; break;
        default: r=v; g=p; b=q; break;
    }
    return 16 + 36 * (int)(r * 5 + 0.5)
              + 6  * (int)(g * 5 + 0.5)
                   + (int)(b * 5 + 0.5);
}

/* Palette definitions: {hue_start, hue_end, saturation, value} */
static const double palettes[NPALS][4] = {
    {  0, 300, 1.0, 1.0 },   /* Rainbow: full spectrum */
    {180, 260, 0.7, 0.9 },   /* Ocean:  cyan → blue */
    {  0,  60, 1.0, 1.0 },   /* Fire:   red → yellow */
    { 80, 160, 0.7, 0.85},   /* Forest: green range */
    {260, 340, 1.0, 1.0 },   /* Neon:   purple → pink */
    {190, 250, 0.5, 1.0 },   /* Ice:    cool blue-purple */
};

static const char *pal_names[NPALS] = {
    "Rainbow", "Ocean", "Fire", "Forest", "Neon", "Ice"
};

static int depth_color(int depth, int max_depth, int pal) {
    double t = max_depth > 0 ? (double)depth / max_depth : 0.0;
    if (t > 1.0) t = 1.0;
    double h = palettes[pal][0] + t * (palettes[pal][1] - palettes[pal][0]);
    return hsv_to_ansi(h, palettes[pal][2], palettes[pal][3]);
}

/* =======================================================================
 * Grid Cell
 *   Each cell tracks segment density and direction for smart rendering.
 * ======================================================================= */

typedef struct {
    unsigned short cnt;        /* number of segments passing through */
    unsigned short depth_sum;  /* sum of depths (for average color) */
    unsigned char  dir;        /* quantized direction 0-7 */
} Cell;

/* Map a screen-space angle to one of 8 directions */
static unsigned char angle_to_dir(double a) {
    a = fmod(a * (180.0 / M_PI), 360.0);
    if (a < 0) a += 360.0;
    return (unsigned char)((a + 22.5) / 45.0) % 8;
}

/* Pick a direction-aware ASCII character */
static char dir_char(unsigned char d) {
    static const char chars[8] = {'-', '\\', '|', '/', '-', '\\', '|', '/'};
    return chars[d];
}

/* =======================================================================
 * Rendering: Segments → Grid
 *   Uses Bresenham's line algorithm with y-flip for screen coordinates.
 * ======================================================================= */

static void render_grid(Seg *segs, int nsegs, Cell *grid,
                        int gw, int gh,
                        double ox, double oy, double sc,
                        int *max_depth_out) {
    memset(grid, 0, (size_t)gw * gh * sizeof(Cell));
    int md = 0;
    for (int i = 0; i < nsegs; i++)
        if (segs[i].depth > md) md = segs[i].depth;
    *max_depth_out = md;

    for (int i = 0; i < nsegs; i++) {
        /* World → screen; y is flipped so "up" in world = "up" on screen */
        int x1 = (int)((segs[i].x1 - ox) * sc + 0.5);
        int y1 = (int)((oy - segs[i].y1) * sc + 0.5);
        int x2 = (int)((segs[i].x2 - ox) * sc + 0.5);
        int y2 = (int)((oy - segs[i].y2) * sc + 0.5);

        /* Screen-space direction for character selection */
        double sdx = (segs[i].x2 - segs[i].x1) * sc;
        double sdy = -(segs[i].y2 - segs[i].y1) * sc; /* y-flip */
        unsigned char dir = angle_to_dir(atan2(sdy, sdx));

        /* Bresenham's line algorithm */
        int dx = abs(x2 - x1), dy = abs(y2 - y1);
        int sx = x1 < x2 ? 1 : -1;
        int sy = y1 < y2 ? 1 : -1;
        int err = dx - dy;
        int steps = dx + dy + 1;

        while (steps-- > 0) {
            if (x1 >= 0 && x1 < gw && y1 >= 0 && y1 < gh) {
                int idx = y1 * gw + x1;
                grid[idx].cnt++;
                grid[idx].depth_sum += (unsigned short)segs[i].depth;
                grid[idx].dir = dir;
            }
            if (x1 == x2 && y1 == y2) break;
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x1 += sx; }
            if (e2 <  dx) { err += dx; y1 += sy; }
        }
    }
}

/* =======================================================================
 * Display: Grid → Terminal
 *   Renders the grid with ANSI 256-colors. Uses direction-aware characters
 *   for sparse regions and density characters for dense regions.
 * ======================================================================= */

static void display_grid(Cell *grid, int gw, int gh,
                         int max_depth, int pal,
                         const char *name, int iter, int nsegs) {
    /* Clear screen and move cursor to (0,0) */
    printf("\033[H\033[2J");

    /* Header bar */
    printf("\033[1;48;5;236;38;5;255m"
           "  %s \033[38;5;75m| \033[38;5;220mIter: %d "
           "\033[38;5;75m| \033[38;5;82mSegments: %d "
           "\033[38;5;75m| \033[38;5;213mPalette: %s "
           "\033[K\033[0m\n", name, iter, nsegs, pal_names[pal]);

    /* Separator */
    printf("\033[90m");
    for (int i = 0; i < gw; i++) putchar(i % 4 == 0 ? '+' : '-');
    printf("\033[0m\n");

    /* Density ramp for cells with multiple overlapping segments */
    static const char density[] = " .:;=+*#%@";

    int prev_col = -1;
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            Cell *c = &grid[y * gw + x];
            if (c->cnt > 0) {
                /* Color based on average depth */
                int avg_depth = c->cnt > 0 ? c->depth_sum / c->cnt : 0;
                int col = depth_color(avg_depth, max_depth, pal);
                if (col != prev_col) {
                    printf("\033[38;5;%dm", col);
                    prev_col = col;
                }
                /* Character: direction for sparse, density for dense */
                if (c->cnt == 1) {
                    putchar(dir_char(c->dir));
                } else {
                    int ci = c->cnt > 10 ? 10 : c->cnt;
                    putchar(density[ci]);
                }
            } else {
                if (prev_col != 0) {
                    printf("\033[0m");
                    prev_col = 0;
                }
                putchar(' ');
            }
        }
        putchar('\n');
        prev_col = -1; /* reset color tracking per line */
    }

    /* Footer help bar */
    printf("\033[48;5;236;38;5;248m"
           " \033[1m1-0\033[0;48;5;236;38;5;248m:select "
           "\033[1m+/-\033[0;48;5;236;38;5;248m:iter "
           "\033[1mR\033[0;48;5;236;38;5;248m:rebuild "
           "\033[1mC\033[0;48;5;236;38;5;248m:palette "
           "\033[1mZ/X\033[0;48;5;236;38;5;248m:zoom "
           "\033[1mArrows\033[0;48;5;236;38;5;248m:pan "
           "\033[1mA\033[0;48;5;236;38;5;248m:anim "
           "\033[1mQ\033[0;48;5;236;38;5;248m:quit"
           "\033[K\033[0m");
    fflush(stdout);
}

/* =======================================================================
 * Auto-fit: compute view parameters to center segments in the grid
 * ======================================================================= */

static void autofit(Seg *segs, int nsegs, int gw, int gh,
                    double *ox, double *oy, double *sc) {
    if (nsegs == 0) { *ox = 0; *oy = 0; *sc = 1; return; }

    double mnx = 1e18, mxx = -1e18, mny = 1e18, mxy = -1e18;
    for (int i = 0; i < nsegs; i++) {
        double xlo = segs[i].x1 < segs[i].x2 ? segs[i].x1 : segs[i].x2;
        double xhi = segs[i].x1 < segs[i].x2 ? segs[i].x2 : segs[i].x1;
        double ylo = segs[i].y1 < segs[i].y2 ? segs[i].y1 : segs[i].y2;
        double yhi = segs[i].y1 < segs[i].y2 ? segs[i].y2 : segs[i].y1;
        if (xlo < mnx) mnx = xlo;
        if (xhi > mxx) mxx = xhi;
        if (ylo < mny) mny = ylo;
        if (yhi > mxy) mxy = yhi;
    }

    double bw = mxx - mnx;
    double bh = mxy - mny;
    if (bw < 1e-6) bw = 1;
    if (bh < 1e-6) bh = 1;

    /* Scale to fit with a small margin */
    double margin = 4.0;
    double sx = (gw - margin) / bw;
    double sy = (gh - margin) / bh;
    *sc = sx < sy ? sx : sy;

    /* Center in the grid; oy = max-y (because y is flipped for display) */
    *ox = mnx - (gw / *sc - bw) / 2.0;
    *oy = mxy + (gh / *sc - bh) / 2.0;
}

/* =======================================================================
 * Animation: step through iterations 1..N with visual feedback
 * ======================================================================= */

static void animate(const LDef *d, int target_iter, int pal,
                    Seg *segs, Cell *grid, int gw, int gh,
                    double *ox, double *oy, double *sc) {
    for (int it = 1; it <= target_iter; it++) {
        char *str = lsys_expand(d, it);
        int ns = str ? turtle_interpret(str, d->angle, d->draw,
                                        segs, MAX_SEGS) : 0;
        free(str);

        autofit(segs, ns, gw, gh, ox, oy, sc);
        int md;
        render_grid(segs, ns, grid, gw, gh, *ox, *oy, *sc, &md);
        display_grid(grid, gw, gh, md, pal, d->name, it, ns);

        /* Animate label */
        printf("\n\033[38;5;226m  >> Iteration %d / %d <<\033[K\033[A",
               it, target_iter);
        fflush(stdout);

        /* Wait with interrupt check */
        for (int w = 0; w < 8; w++) {
            usleep(60000);
            int k = keypress();
            if (k == 'q' || k == 27 || k == 3) return;
            if (k >= 0) break;
        }
    }
}

/* =======================================================================
 * Main Interactive Loop
 * ======================================================================= */

int main(void) {
    term_raw();
    atexit(term_restore);

    /* Allocate segment buffer */
    Seg *segs = malloc(MAX_SEGS * sizeof(Seg));
    if (!segs) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    int fi = 0, iter = fractals[0].def_iter, pal = 0;
    int tw, th;
    term_size(&tw, &th);
    int gw = tw, gh = th - 3;
    if (gw < 20) gw = 20;
    if (gh < 8)  gh = 8;

    Cell *grid = malloc((size_t)gw * gh * sizeof(Cell));
    if (!grid) { free(segs); fprintf(stderr, "Out of memory\n"); return 1; }

    char *str = NULL;
    int ns = 0;
    double ox = 0, oy = 0, sc = 1;
    int need_gen = 1, need_render = 1;

    /* Hide cursor during rendering */
    printf("\033[?25l");
    fflush(stdout);

    for (;;) {
        /* Check terminal resize */
        int new_tw, new_th;
        term_size(&new_tw, &new_th);
        int new_gw = new_tw, new_gh = new_th - 3;
        if (new_gw < 20) new_gw = 20;
        if (new_gh < 8)  new_gh = 8;
        if (new_gw != gw || new_gh != gh) {
            gw = new_gw; gh = new_gh;
            free(grid);
            grid = malloc((size_t)gw * gh * sizeof(Cell));
            if (!grid) { free(segs); free(str); return 1; }
            autofit(segs, ns, gw, gh, &ox, &oy, &sc);
            need_render = 1;
        }

        /* Generate L-system and interpret turtle */
        if (need_gen) {
            free(str);
            str = lsys_expand(&fractals[fi], iter);
            ns = str ? turtle_interpret(str, fractals[fi].angle,
                                        fractals[fi].draw, segs, MAX_SEGS)
                     : 0;
            autofit(segs, ns, gw, gh, &ox, &oy, &sc);
            need_gen = 0;
            need_render = 1;
        }

        /* Render to grid and display */
        if (need_render) {
            int md;
            render_grid(segs, ns, grid, gw, gh, ox, oy, sc, &md);
            display_grid(grid, gw, gh, md, pal,
                        fractals[fi].name, iter, ns);
            need_render = 0;
        }

        /* Wait for input */
        int k = keypress();
        if (k < 0) { usleep(50000); continue; }

        switch (k) {
        case 'q': case 'Q': case 3: /* Ctrl-C */
            printf("\033[?25h\033[0m\033[H\033[2J");
            fflush(stdout);
            free(str); free(segs); free(grid);
            return 0;

        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9':
            fi = k - '1';
            iter = fractals[fi].def_iter;
            need_gen = 1;
            break;
        case '0':
            fi = 9;
            iter = fractals[fi].def_iter;
            need_gen = 1;
            break;

        case '+': case '=':
            if (iter < fractals[fi].max_iter) { iter++; need_gen = 1; }
            break;
        case '-': case '_':
            if (iter > 1) { iter--; need_gen = 1; }
            break;

        case 'r': case 'R':
            need_gen = 1;
            break;

        case 'c': case 'C':
            pal = (pal + 1) % NPALS;
            need_render = 1;
            break;

        case 'z': case 'Z':
            sc *= 1.5;
            need_render = 1;
            break;
        case 'x': case 'X':
            sc /= 1.5;
            if (sc < 1e-6) sc = 1e-6;
            need_render = 1;
            break;

        case ' ':
            autofit(segs, ns, gw, gh, &ox, &oy, &sc);
            need_render = 1;
            break;

        case 0x81: oy += 5.0 / sc; need_render = 1; break; /* ↑ */
        case 0x82: oy -= 5.0 / sc; need_render = 1; break; /* ↓ */
        case 0x84: ox -= 5.0 / sc; need_render = 1; break; /* ← */
        case 0x83: ox += 5.0 / sc; need_render = 1; break; /* → */

        case 'a': case 'A':
            animate(&fractals[fi], iter, pal, segs, grid, gw, gh,
                    &ox, &oy, &sc);
            need_render = 1;
            break;
        }
    }
}
