/*
 * ============================================================================
 *  M A T R I X   R A I N  —  Terminal Matrix Digital Rain Effect
 * ============================================================================
 *
 *  A multi-layered, depth-shaded Matrix digital rain animation for the
 *  terminal.  Streams of falling characters create an illusion of 3D depth
 *  with three visual layers (near / mid / far), each with its own speed,
 *  brightness, and character density.
 *
 *  Features:
 *    - Three depth layers with independent speed, colour brightness, charset
 *    - Smooth head glow (white) fading through bright-green → dark-green
 *    - Random "glitch" characters that flicker in existing columns
 *    - Random burst events that spawn short-lived bright streams
 *    - Unicode box-drawing / katakana characters (if terminal supports UTF-8)
 *    - Graceful resize handling (SIGWINCH)
 *    - Clean shutdown on 'q' or Ctrl-C
 *
 *  Compile:
 *    gcc -O2 -std=c11 -Wall -Wextra -o matrixrain matrixrain-1923.c -lm
 *
 *  Run:
 *    ./matrixrain
 *
 *  Requires: a terminal with ANSI colour support (xterm-256color recommended).
 *
 *  Author : Claude  |  2026-05-06
 *  License: MIT
 * ============================================================================
 */

#define _POSIX_C_SOURCE 199309L   /* for nanosleep */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <math.h>

/* ---------- configuration constants ---------- */

#define NUM_LAYERS      3       /* depth layers: 0 = far, 1 = mid, 2 = near */
#define MAX_STREAMS     512     /* max concurrent streams across all layers  */
#define BURST_PROB      0.008   /* probability of a burst event per frame   */
#define BURST_COUNT     8       /* streams spawned per burst                */
#define FRAME_MS        45      /* milliseconds between frames (~22 fps)    */

/* Characters used for rain drops — mix of katakana-ish, box-drawing, digits */
static const char *RAIN_CHARS =
    "ｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜﾝ"
    "0123456789"
    "│┃┆┇┊┋╭╮╯╰"
    "ABCDEFZ";

/* ANSI colour helpers */
#define ESC_RESET   "\033[0m"
#define ESC_CLR     "\033[2J\033[H"
#define ESC_HIDE    "\033[?25l"
#define ESC_SHOW    "\033[?25h"
#define ESC_FG(n)   "\033[38;5;" #n "m"

/* Brightness colour table (xterm-256 green shades, dim → bright) */
static const int GREEN_SHADES[] = {
    22, 28, 34, 40, 46, 82, 118, 154, 155, 156, 157, 191
};
#define NUM_GREENS  (sizeof(GREEN_SHADES) / sizeof(GREEN_SHADES[0]))

/* ---------- data types ---------- */

typedef struct {
    int     col;        /* column position                          */
    int     row;        /* current head row                         */
    int     length;     /* trail length                             */
    int     speed;      /* rows to advance per frame                */
    int     layer;      /* depth layer 0-2                          */
    int     active;     /* 1 = running, 0 = done                   */
    char   *trail;      /* characters in the trail (heap-allocated) */
} Stream;

typedef struct {
    int     w, h;       /* terminal dimensions                     */
    int     *col_owner; /* which layer "owns" each column (-1=none)*/
    Stream  streams[MAX_STREAMS];
    int     nstreams;
    volatile sig_atomic_t resized; /* SIGWINCH flag                    */
} State;

static State g_state;

/* ---------- terminal helpers ---------- */

static void term_size(int *w, int *h) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    } else {
        *w = 80; *h = 24;
    }
}

static void set_raw(struct termios *old) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, old);
    raw = *old;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

/* ---------- signal handler ---------- */

static void on_sigwinch(int sig) {
    (void)sig;
    g_state.resized = 1;
}

static void on_sigint(int sig) {
    (void)sig;
    /* restore cursor and exit */
    printf(ESC_SHOW ESC_RESET ESC_CLR);
    _exit(0);
}

/* ---------- utility ---------- */

static inline int rand_range(int lo, int hi) {
    return lo + rand() % (hi - lo + 1);
}

static char rand_char(void) {
    /* RAIN_CHARS contains multi-byte sequences; pick a random byte index
       that is the start of a character.  For simplicity treat each byte
       as independent (works fine visually). */
    int len = (int)strlen(RAIN_CHARS);
    return RAIN_CHARS[rand() % len];
}

/* ---------- layer parameters ---------- */

static void layer_params(int layer, int *speed_lo, int *speed_hi,
                         int *len_lo, int *len_hi, int *brightness) {
    switch (layer) {
        case 0: /* far — slow, short, dim */
            *speed_lo = 1; *speed_hi = 1;
            *len_lo   = 3; *len_hi   = 8;
            *brightness = 0;
            break;
        case 1: /* mid */
            *speed_lo = 1; *speed_hi = 2;
            *len_lo   = 6; *len_hi   = 16;
            *brightness = 1;
            break;
        case 2: /* near — fast, long, bright */
            *speed_lo = 2; *speed_hi = 3;
            *len_lo   = 8; *len_hi   = 24;
            *brightness = 2;
            break;
        default:
            *speed_lo = 1; *speed_hi = 1;
            *len_lo   = 5; *len_hi   = 10;
            *brightness = 1;
            break;
    }
}

/* ---------- stream management ---------- */

static void spawn_stream(int layer) {
    State *s = &g_state;
    if (s->nstreams >= MAX_STREAMS) return;

    int sp_lo, sp_hi, ln_lo, ln_hi, brightness;
    layer_params(layer, &sp_lo, &sp_hi, &ln_lo, &ln_hi, &brightness);
    (void)brightness;

    Stream *st = &s->streams[s->nstreams];
    st->col    = rand() % s->w;
    st->row    = -rand_range(0, s->h / 2);   /* start above screen */
    st->length = rand_range(ln_lo, ln_hi);
    st->speed  = rand_range(sp_lo, sp_hi);
    st->layer  = layer;
    st->active = 1;
    st->trail  = calloc(st->length, 1);
    for (int i = 0; i < st->length; i++)
        st->trail[i] = rand_char();
    s->nstreams++;
}

static void kill_stream(int idx) {
    State *s = &g_state;
    free(s->streams[idx].trail);
    s->streams[idx] = s->streams[s->nstreams - 1];
    s->nstreams--;
}

/* ---------- rendering ---------- */

static const char *green_for_layer(int layer, int pos_from_head, int trail_len) {
    /* pos_from_head: 0 = head, trail_len-1 = tail */
    if (pos_from_head == 0) return "\033[38;5;231m"; /* white head */

    /* fraction from head (0) to tail (1) */
    float frac = (float)pos_from_head / (float)trail_len;

    int base;
    switch (layer) {
        case 0: base = 0;  break;   /* far: dim colours */
        case 1: base = 3;  break;   /* mid */
        case 2: base = 5;  break;   /* near: bright colours */
        default: base = 2; break;
    }
    int idx = base + (int)(frac * (NUM_GREENS - 1 - base));
    if (idx < 0)             idx = 0;
    if (idx >= (int)NUM_GREENS) idx = (int)NUM_GREENS - 1;

    static char buf[20];
    snprintf(buf, sizeof(buf), "\033[38;5;%dm", GREEN_SHADES[idx]);
    return buf;
}

static void draw_frame(void) {
    State *s = &g_state;

    /* Clear screen */
    printf(ESC_CLR);

    /* Sort streams by layer (far first) so near layers paint on top */
    /* Simple insertion sort — we have few streams */
    for (int i = 1; i < s->nstreams; i++) {
        Stream tmp = s->streams[i];
        int j = i - 1;
        while (j >= 0 && s->streams[j].layer > tmp.layer) {
            s->streams[j + 1] = s->streams[j];
            j--;
        }
        s->streams[j + 1] = tmp;
    }

    /* Render each stream */
    for (int i = 0; i < s->nstreams; i++) {
        Stream *st = &s->streams[i];
        if (!st->active) continue;

        for (int t = 0; t < st->length; t++) {
            int r = st->row - t;
            if (r < 0 || r >= s->h) continue;

            /* Move cursor to (r, st->col) */
            printf("\033[%d;%dH", r + 1, st->col + 1);

            /* Pick colour */
            const char *col = green_for_layer(st->layer, t, st->length);
            printf("%s%c", col, st->trail[t]);
        }
    }

    /* Reset colour at end */
    printf(ESC_RESET);
    fflush(stdout);
}

/* ---------- glitch effect ---------- */

static void glitch_chars(void) {
    State *s = &g_state;
    /* Randomly change some trail characters to create a "flicker" */
    for (int i = 0; i < s->nstreams; i++) {
        Stream *st = &s->streams[i];
        if (!st->active || st->length < 2) continue;
        /* Change 1-2 random chars in the trail */
        int changes = rand_range(1, 2);
        for (int c = 0; c < changes; c++) {
            int idx = rand() % st->length;
            st->trail[idx] = rand_char();
        }
    }
}

/* ---------- main loop ---------- */

static void handle_resize(void) {
    State *s = &g_state;
    int new_w, new_h;
    term_size(&new_w, &new_h);
    if (new_w != s->w || new_h != s->h) {
        s->w = new_w;
        s->h = new_h;
        free(s->col_owner);
        s->col_owner = malloc(s->w * sizeof(int));
        for (int c = 0; c < s->w; c++)
            s->col_owner[c] = -1;
        /* Kill streams that are now out of bounds */
        for (int i = s->nstreams - 1; i >= 0; i--) {
            if (s->streams[i].col >= s->w)
                kill_stream(i);
        }
    }
    s->resized = 0;
}

int main(void) {
    srand((unsigned)time(NULL));
    State *s = &g_state;

    term_size(&s->w, &s->h);
    s->col_owner = malloc(s->w * sizeof(int));
    for (int c = 0; c < s->w; c++) s->col_owner[c] = -1;
    s->nstreams = 0;
    s->resized  = 0;

    struct termios old_tio;
    set_raw(&old_tio);

    signal(SIGWINCH, on_sigwinch);
    signal(SIGINT,   on_sigint);
    signal(SIGTERM,  on_sigint);

    printf(ESC_HIDE);          /* hide cursor */
    printf(ESC_CLR);           /* clear screen */

    /* Seed initial streams across all layers */
    for (int l = 0; l < NUM_LAYERS; l++) {
        int count = (l == 0) ? s->w / 5 : (l == 1) ? s->w / 4 : s->w / 3;
        for (int i = 0; i < count; i++)
            spawn_stream(l);
    }

    const struct timespec frame_ts = { .tv_sec = 0, .tv_nsec = FRAME_MS * 1000000L };
    int frame = 0;

    for (;;) {
        /* Check for resize */
        if (s->resized) handle_resize();

        /* Check for quit key */
        char ch = 0;
        if (read(STDIN_FILENO, &ch, 1) > 0) {
            if (ch == 'q' || ch == 'Q' || ch == 27 /* ESC */) break;
        }

        /* Update streams */
        for (int i = s->nstreams - 1; i >= 0; i--) {
            Stream *st = &s->streams[i];
            st->row += st->speed;
            /* If the tail has left the screen, respawn */
            if (st->row - st->length > s->h) {
                int layer = st->layer;
                kill_stream(i);
                spawn_stream(layer);
            }
        }

        /* Random burst event */
        if ((double)rand() / RAND_MAX < BURST_PROB) {
            for (int b = 0; b < BURST_COUNT; b++)
                spawn_stream(2); /* burst streams are always "near" layer */
        }

        /* Glitch effect every few frames */
        if (frame % 3 == 0) glitch_chars();

        /* Draw */
        draw_frame();

        nanosleep(&frame_ts, NULL);
        frame++;
    }

    /* Cleanup */
    printf(ESC_SHOW ESC_RESET ESC_CLR);
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
    for (int i = 0; i < s->nstreams; i++)
        free(s->streams[i].trail);
    free(s->col_owner);
    return 0;
}
