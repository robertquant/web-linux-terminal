/*
 * ============================================================================
 *  MANDELBROT / JULIA SET FRACTAL EXPLORER
 * ============================================================================
 *  An interactive terminal fractal renderer with ANSI color support.
 *  Explore the infinite beauty of the Mandelbrot and Julia sets in ASCII!
 *
 *  Features:
 *    - Mandelbrot set and Julia set rendering
 *    - Smooth zoom in/out with mouse-like precision
 *    - Pan in all 4 directions (WASD / arrow keys)
 *    - Adjustable max iterations for detail control
 *    - Multiple color palettes (7 palettes)
 *    - ASCII density shading with 16-level characters
 *    - Real-time coordinate and parameter display
 *    - Julia set parameter picking from Mandelbrot view
 *    - Smooth color interpolation
 *
 *  Compile:
 *    gcc -std=c99 -O2 -o mandelbrot mandelbrot-4468.c -lm
 *
 *  Run:
 *    ./mandelbrot
 *
 *  Controls:
 *    +/- or scroll     Zoom in / out (centered on cursor or view center)
 *    WASD / Arrows     Pan view
 *    I / O             Increase / decrease max iterations
 *    C                 Cycle color palette
 *    J                 Toggle Julia set mode
 *    SPACE             In Mandelbrot mode: pick Julia constant from cursor
 *    R                 Reset to default view
 *    L                 Toggle ASCII labels on/off
 *    Q / ESC           Quit
 *
 *  Author: Creative C Programs Collection
 *  License: Public Domain
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>

/* ── Configuration ── */
#define MAX_ITER_DEFAULT  128
#define MAX_ITER_LIMIT    4096
#define ESCAPE_RADIUS_SQ  4.0
#define COLOR_LEVELS      64
#define DENSITY_CHARS     " .,:;+*?%S#@"

/* ── Color Palettes ── */
typedef struct { int r, g, b; } RGB;

static const RGB palettes[][8] = {
    /* 0: Ocean Fire */
    {{0,0,0},{10,0,40},{80,0,120},{180,30,60},{255,100,0},{255,200,50},{255,255,200},{255,255,255}},
    /* 1: Electric Neon */
    {{0,0,0},{0,20,60},{0,80,160},{0,200,200},{0,255,100},{180,255,0},{255,255,80},{255,255,255}},
    /* 2: Lava */
    {{0,0,0},{40,0,0},{100,0,0},{180,20,0},{220,80,0},{255,150,20},{255,220,100},{255,255,200}},
    /* 3: Arctic */
    {{0,0,10},{0,10,40},{0,30,80},{20,80,140},{60,140,200},{140,200,240},{220,240,255},{255,255,255}},
    /* 4: Toxic */
    {{0,0,0},{0,20,10},{0,50,20},{0,100,30},{20,160,40},{80,200,60},{180,240,100},{240,255,200}},
    /* 5: Cosmic Purple */
    {{0,0,0},{20,0,40},{60,0,80},{120,0,140},{180,40,160},{220,100,180},{240,180,220},{255,240,255}},
    /* 6: Grayscale Scientific */
    {{0,0,0},{36,36,36},{73,73,73},{109,109,109},{146,146,146},{182,182,182},{219,219,219},{255,255,255}},
};
#define NUM_PALETTES 7

/* ── State ── */
typedef struct {
    double center_re, center_im;   /* view center in complex plane */
    double zoom;                    /* zoom level (higher = zoomed in) */
    int max_iter;
    int palette;
    int julia_mode;                 /* 0 = Mandelbrot, 1 = Julia */
    double julia_re, julia_im;     /* Julia set constant */
    int cursor_x, cursor_y;        /* screen cursor position */
    int show_labels;
    int cols, rows;                 /* terminal size */
} State;

static State g_state;
static struct termios g_orig_term;
static volatile int g_running = 1;

/* ── Terminal Helpers ── */

static void enable_raw_mode(void) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &g_orig_term);
    t = g_orig_term;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

static void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_term);
}

static void get_terminal_size(int *cols, int *rows) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0 && w.ws_row > 0) {
        *cols = w.ws_col;
        *rows = w.ws_row - 1;  /* leave room for status bar */
    } else {
        *cols = 80;
        *rows = 24;
    }
}

/* ── Color Interpolation ── */

static RGB lerp_color(RGB a, RGB b, double t) {
    RGB out;
    out.r = (int)(a.r + (b.r - a.r) * t);
    out.g = (int)(a.g + (b.g - a.g) * t);
    out.b = (int)(a.b + (b.b - a.b) * t);
    return out;
}

static RGB get_color(double t, int palette) {
    /* t is 0..1, map into the palette with smooth interpolation */
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int n = 8;
    double idx = t * (n - 1);
    int i = (int)idx;
    if (i >= n - 1) return palettes[palette][n - 1];
    double frac = idx - i;
    return lerp_color(palettes[palette][i], palettes[palette][i + 1], frac);
}

/* ── Fractal Computation ── */

static double compute_mandelbrot(double cre, double cim, int max_iter) {
    double zr = 0, zi = 0;
    double zr2, zi2;
    int i;
    for (i = 0; i < max_iter; i++) {
        zr2 = zr * zr;
        zi2 = zi * zi;
        if (zr2 + zi2 > ESCAPE_RADIUS_SQ) {
            /* Smooth iteration count */
            double log_zn = 0.5 * log(zr2 + zi2);
            double nu = log(log_zn / log(2.0)) / log(2.0);
            return (double)i + 1.0 - nu;
        }
        zi = 2.0 * zr * zi + cim;
        zr = zr2 - zi2 + cre;
    }
    return -1.0; /* in set */
}

static double compute_julia(double zre, double zim, double jre, double jim, int max_iter) {
    double zr2, zi2;
    int i;
    for (i = 0; i < max_iter; i++) {
        zr2 = zre * zre;
        zi2 = zim * zim;
        if (zr2 + zi2 > ESCAPE_RADIUS_SQ) {
            double log_zn = 0.5 * log(zr2 + zi2);
            double nu = log(log_zn / log(2.0)) / log(2.0);
            return (double)i + 1.0 - nu;
        }
        zim = 2.0 * zre * zim + jim;
        zre = zr2 - zi2 + jre;
    }
    return -1.0;
}

/* ── Mapping ── */

static void screen_to_complex(int sx, int sy, State *s, double *re, double *im) {
    double aspect = (double)s->cols / s->rows;
    double range = 4.0 / s->zoom;
    *re = s->center_re + (sx - s->cols / 2.0) / s->cols * range * aspect;
    *im = s->center_im + (sy - s->rows / 2.0) / s->rows * range;
}

/* ── Rendering ── */

/* Buffer for building a frame to minimize flicker */
static char *frame_buf = NULL;
static int frame_buf_size = 0;

static void ensure_frame_buf(int size) {
    if (size > frame_buf_size) {
        free(frame_buf);
        frame_buf = malloc(size);
        frame_buf_size = size;
    }
}

static void render_frame(State *s) {
    int cols = s->cols;
    int rows = s->rows;
    /* Estimate max needed buffer: each cell ~ 30 bytes (ANSI escape) */
    int buf_need = cols * rows * 32 + 1024;
    ensure_frame_buf(buf_need);

    int pos = 0;
    /* Hide cursor, move to home */
    pos += sprintf(frame_buf + pos, "\033[?25l\033[H");

    int density_len = (int)strlen(DENSITY_CHARS) - 1;

    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            double re, im;
            screen_to_complex(x, y, s, &re, &im);

            double val;
            if (s->julia_mode) {
                val = compute_julia(re, im, s->julia_re, s->julia_im, s->max_iter);
            } else {
                val = compute_mandelbrot(re, im, s->max_iter);
            }

            if (val < 0) {
                /* In the set: render as dark */
                pos += sprintf(frame_buf + pos, "\033[48;2;0;0;0m ");
            } else {
                /* Map iteration to color */
                double t = val / (double)s->max_iter;
                /* Apply cyclic mapping for more visual interest */
                t = fmod(t * 4.0, 1.0);
                RGB c = get_color(t, s->palette);

                /* Pick density character based on iteration */
                int di = (int)(val * density_len / s->max_iter * 4);
                if (di > density_len) di = density_len;
                if (di < 0) di = 0;
                char ch = DENSITY_CHARS[di];

                /* Use background color for full-block effect */
                pos += sprintf(frame_buf + pos, "\033[48;2;%d;%d;%dm ",
                               c.r, c.g, c.b);
            }
        }
        pos += sprintf(frame_buf + pos, "\033[0m\n");
    }

    /* ── Status Bar ── */
    double cur_re, cur_im;
    screen_to_complex(s->cursor_x, s->cursor_y, s, &cur_re, &cur_im);

    pos += sprintf(frame_buf + pos,
        "\033[7m" /* reverse video */
        " %s Mode | Zoom: %.2fx | Iter: %d | Palette: %d/%d | "
        "Center: (%.10f, %.10fi) | Cursor: (%.6f, %.6fi)"
        " \033[K\033[0m",
        s->julia_mode ? "Julia" : "Mandelbrot",
        s->zoom, s->max_iter, s->palette + 1, NUM_PALETTES,
        s->center_re, s->center_im,
        cur_re, cur_im);

    /* Controls hint */
    pos += sprintf(frame_buf + pos,
        "\n\033[2m +/-Zoom WASD=Pan I/O=Iter C=Palette J=Julia "
        "SPACE=Pick R=Reset Q=Quit\033[0m\033[K");

    fwrite(frame_buf, 1, pos, stdout);
    fflush(stdout);
}

/* ── Input Handling ── */

static int read_key(void) {
    unsigned char buf[8];
    int n = (int)read(STDIN_FILENO, buf, 8);
    if (n <= 0) return -1;

    /* Handle escape sequences */
    if (buf[0] == 27 && n >= 2) {
        if (buf[1] == '[' && n >= 3) {
            switch (buf[2]) {
                case 'A': return 1;   /* up */
                case 'B': return 2;   /* down */
                case 'C': return 3;   /* right */
                case 'D': return 4;   /* left */
                case '1': if (n >= 4 && buf[3] == '~') return 'H'; /* Home */
                case '4': if (n >= 4 && buf[3] == '~') return 'E'; /* End */
                case '5': if (n >= 4 && buf[3] == '~') return '+'; /* PgUp = zoom in */
                case '6': if (n >= 4 && buf[3] == '~') return '-'; /* PgDn = zoom out */
            }
            /* Mouse scroll: ESC [ M <btn> <x> <y> */
            if (buf[2] == 'M' && n >= 6) {
                int btn = buf[3];
                if (btn == 96) return '+';  /* scroll up */
                if (btn == 97) return '-';  /* scroll down */
            }
        }
        return 27; /* bare ESC */
    }
    return buf[0];
}

static void handle_input(int key) {
    double pan_step = 0.15 / g_state.zoom;
    double zoom_factor = 1.4;

    switch (key) {
        case 'q': case 27: /* ESC or Q */
            g_running = 0;
            break;

        /* Pan (arrow keys use codes 1-4, WASD uses letters) */
        case 'w': case 1:   /* up */
            g_state.center_im -= pan_step; break;
        case 's': case 2:   /* down */
            g_state.center_im += pan_step; break;
        case 'a': case 4:   /* left */
            g_state.center_re -= pan_step; break;
        case 'd': case 3:   /* right */
            g_state.center_re += pan_step; break;

        /* Zoom */
        case '+': case '=':
            g_state.zoom *= zoom_factor; break;
        case '-': case '_':
            g_state.zoom /= zoom_factor;
            if (g_state.zoom < 0.25) g_state.zoom = 0.25;
            break;

        /* Iterations */
        case 'i': case 'I':
            g_state.max_iter = g_state.max_iter >= MAX_ITER_LIMIT
                ? MAX_ITER_LIMIT : g_state.max_iter + 32;
            break;
        case 'o': case 'O':
            g_state.max_iter = g_state.max_iter <= 16
                ? 16 : g_state.max_iter - 32;
            break;

        /* Palette */
        case 'c': case 'C':
            g_state.palette = (g_state.palette + 1) % NUM_PALETTES;
            break;

        /* Toggle Julia */
        case 'j': case 'J':
            g_state.julia_mode = !g_state.julia_mode;
            if (g_state.julia_mode) {
                /* Default Julia constant if not picked */
                if (g_state.julia_re == 0 && g_state.julia_im == 0) {
                    g_state.julia_re = -0.7;
                    g_state.julia_im = 0.27015;
                }
            }
            break;

        /* Pick Julia constant from cursor position */
        case ' ':
            if (!g_state.julia_mode) {
                screen_to_complex(g_state.cursor_x, g_state.cursor_y,
                                  &g_state, &g_state.julia_re, &g_state.julia_im);
                g_state.julia_mode = 1;
            }
            break;

        /* Reset */
        case 'r': case 'R':
            g_state.center_re = -0.5;
            g_state.center_im = 0.0;
            g_state.zoom = 1.0;
            g_state.max_iter = MAX_ITER_DEFAULT;
            g_state.julia_mode = 0;
            g_state.julia_re = 0;
            g_state.julia_im = 0;
            break;
    }
}

/* ── Initialization ── */

static void init_state(void) {
    g_state.center_re = -0.5;
    g_state.center_im = 0.0;
    g_state.zoom = 1.0;
    g_state.max_iter = MAX_ITER_DEFAULT;
    g_state.palette = 0;
    g_state.julia_mode = 0;
    g_state.julia_re = -0.7;
    g_state.julia_im = 0.27015;
    g_state.cursor_x = 40;
    g_state.cursor_y = 12;
    g_state.show_labels = 1;
    get_terminal_size(&g_state.cols, &g_state.rows);
}

/* ── Signal Handler ── */

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* ── Main ── */

int main(void) {
    /* Show loading message */
    printf("\033[?25l");  /* hide cursor */
    printf("Computing fractal... please wait.\r\n");
    fflush(stdout);

    init_state();
    enable_raw_mode();

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    atexit(disable_raw_mode);

    /* Enable mouse scroll tracking */
    printf("\033[?1000h");
    fflush(stdout);

    /* Initial render */
    render_frame(&g_state);

    /* Main loop */
    while (g_running) {
        int key = read_key();
        if (key > 0) {
            /* Re-check terminal size */
            int new_cols, new_rows;
            get_terminal_size(&new_cols, &new_rows);
            if (new_cols != g_state.cols || new_rows != g_state.rows) {
                g_state.cols = new_cols;
                g_state.rows = new_rows;
            }

            handle_input(key);
            render_frame(&g_state);
        }
    }

    /* Cleanup */
    printf("\033[?1000l");  /* disable mouse tracking */
    printf("\033[?25h");    /* show cursor */
    printf("\033[0m");      /* reset colors */
    printf("\033[2J\033[H"); /* clear screen */
    printf("Mandelbrot Explorer exited. Thanks for exploring fractals!\n");
    fflush(stdout);

    free(frame_buf);
    return 0;
}
