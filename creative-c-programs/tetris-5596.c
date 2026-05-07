/*
 * ═══════════════════════════════════════════════════════════════
 *  TERMINAL TETRIS — A full-featured Tetris game in pure C
 * ═══════════════════════════════════════════════════════════════
 *
 *  Features:
 *    - All 7 tetrominos with SRS-like rotation & wall kicks
 *    - Ghost piece (landing preview)
 *    - Next piece preview (3 pieces ahead)
 *    - Hold piece system (swap current piece)
 *    - Hard drop (space) & soft drop (down arrow)
 *    - Score, level, lines — speed increases per level
 *    - Lock delay (piece doesn't lock instantly)
 *    - Line clear animation
 *    - Game over animation with final stats
 *    - Pause/resume
 *
 *  Compile:
 *    gcc -std=c11 -O2 -o tetris tetris-5596.c -lm
 *
 *  Run:
 *    ./tetris
 *
 *  Controls:
 *    ← →      Move left/right
 *    ↑         Rotate clockwise
 *    Z         Rotate counter-clockwise
 *    ↓         Soft drop
 *    Space     Hard drop
 *    C         Hold piece
 *    P         Pause / Resume
 *    Q         Quit
 *
 *  Requires: ANSI/VT100 terminal with color support.
 *  Author:   Creative C Programs Collection
 * ═══════════════════════════════════════════════════════════════
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/select.h>
#include <termios.h>
#include <math.h>

/* ─── Board dimensions ─── */
#define ROWS 20
#define COLS 10
#define HIDDEN_ROWS 2          /* buffer rows above visible area */

/* ─── Piece types ─── */
#define PIECE_I 0
#define PIECE_O 1
#define PIECE_T 2
#define PIECE_S 3
#define PIECE_Z 4
#define PIECE_J 5
#define PIECE_L 6
#define NUM_PIECES 7

/* ─── Directions ─── */
#define DIR_CW  1
#define DIR_CCW 0

/* ─── Scoring ─── */
#define SCORE_1LINE  100
#define SCORE_2LINE  300
#define SCORE_3LINE  500
#define SCORE_4LINE  800
#define SCORE_SOFT   1        /* per soft-drop row */
#define SCORE_HARD   2        /* per hard-drop row */

/* ─── Timing (milliseconds) ─── */
#define BASE_FALL_INTERVAL 800
#define MIN_FALL_INTERVAL   50
#define LOCK_DELAY         500
#define LINE_CLEAR_DELAY   400
#define DAS_DELAY          170  /* delayed auto-shift */
#define DAS_REPEAT          50  /* auto-shift repeat rate */

/* ─── ANSI escape helpers ─── */
#define ESC     "\033["
#define RESET   "\033[0m"
#define HIDE    ESC"?25l"
#define SHOW    ESC"?25h"

/* ─── Colors for each piece (index 1-7) ─── */
static const char *PIECE_COLORS[] = {
    "",             /* 0 = empty */
    "\033[96m",    /* 1 = I — cyan */
    "\033[93m",    /* 2 = O — yellow */
    "\033[95m",    /* 3 = T — magenta */
    "\033[92m",    /* 4 = S — green */
    "\033[91m",    /* 5 = Z — red */
    "\033[94m",    /* 6 = J — blue */
    "\033[97m",    /* 7 = L — white */
};

/* Block characters for rendering */
#define BLOCK_FULL   "█"
#define BLOCK_GHOST  "░"
#define BLOCK_EMPTY  " "

/* ─── Tetromino shapes (4 rotations × 4 cells each) ─── */
/* Stored as [piece][rotation][cell_index] = {row, col} offset from pivot */
/* Rotations: 0=spawn, 1=CW, 2=180, 3=CCW */

typedef struct { int r, c; } Cell;

static const Cell SHAPES[NUM_PIECES][4][4] = {
    /* I */
    {
        {{0,0},{0,1},{0,2},{0,3}},
        {{0,2},{1,2},{2,2},{3,2}},
        {{2,0},{2,1},{2,2},{2,3}},
        {{0,1},{1,1},{2,1},{3,1}},
    },
    /* O */
    {
        {{0,0},{0,1},{1,0},{1,1}},
        {{0,0},{0,1},{1,0},{1,1}},
        {{0,0},{0,1},{1,0},{1,1}},
        {{0,0},{0,1},{1,0},{1,1}},
    },
    /* T */
    {
        {{0,1},{1,0},{1,1},{1,2}},
        {{0,1},{1,1},{1,2},{2,1}},
        {{1,0},{1,1},{1,2},{2,1}},
        {{0,1},{1,0},{1,1},{2,1}},
    },
    /* S */
    {
        {{0,1},{0,2},{1,0},{1,1}},
        {{0,1},{1,1},{1,2},{2,2}},
        {{1,1},{1,2},{2,0},{2,1}},
        {{0,0},{1,0},{1,1},{2,1}},
    },
    /* Z */
    {
        {{0,0},{0,1},{1,1},{1,2}},
        {{0,2},{1,1},{1,2},{2,1}},
        {{1,0},{1,1},{2,1},{2,2}},
        {{0,1},{1,0},{1,1},{2,0}},
    },
    /* J */
    {
        {{0,0},{1,0},{1,1},{1,2}},
        {{0,1},{0,2},{1,1},{2,1}},
        {{1,0},{1,1},{1,2},{2,2}},
        {{0,1},{1,1},{2,0},{2,1}},
    },
    /* L */
    {
        {{0,2},{1,0},{1,1},{1,2}},
        {{0,1},{1,1},{2,1},{2,2}},
        {{1,0},{1,1},{1,2},{2,0}},
        {{0,0},{0,1},{1,1},{2,1}},
    },
};

/* Wall kick data (SRS-style, simplified) */
static const Cell KICKS_NORMAL[4][5] = {
    {{0,0},{-1,0},{-1,1},{0,-2},{-1,-2}},  /* 0→R */
    {{0,0},{1,0},{1,-1},{0,2},{1,2}},       /* R→2 */
    {{0,0},{1,0},{1,1},{0,-2},{1,-2}},      /* 2→L */
    {{0,0},{-1,0},{-1,-1},{0,2},{-1,2}},    /* L→0 */
};

static const Cell KICKS_I[4][5] = {
    {{0,0},{-2,0},{1,0},{-2,-1},{1,2}},
    {{0,0},{-1,0},{2,0},{-1,2},{2,-1}},
    {{0,0},{2,0},{-1,0},{2,1},{-1,-2}},
    {{0,0},{1,0},{-2,0},{1,-2},{-2,1}},
};

/* ─── Game state ─── */
typedef struct {
    int board[ROWS + HIDDEN_ROWS][COLS];
    int piece, rotation, row, col;
    int next_pieces[3];         /* queue of next 3 pieces */
    int hold_piece;
    int can_hold;               /* prevent holding twice in a row */
    int score, level, lines;
    int game_over, paused;
    int fall_timer, lock_timer, lock_active;
    int das_timer, das_dir, das_charging;
    int clearing_lines[4];
    int clear_timer;
    struct timespec last_tick;
} Game;

/* ─── 7-bag randomizer ─── */
static int bag[7], bag_pos;

static void bag_init(void) {
    bag_pos = 7;
}

static int bag_next(void) {
    if (bag_pos >= 7) {
        /* Fisher-Yates shuffle */
        for (int i = 0; i < 7; i++) bag[i] = i;
        for (int i = 6; i > 0; i--) {
            int j = rand() % (i + 1);
            int tmp = bag[i]; bag[i] = bag[j]; bag[j] = tmp;
        }
        bag_pos = 0;
    }
    return bag[bag_pos++];
}

/* ─── Terminal raw mode ─── */
static struct termios orig_termios;

static void term_restore(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf(SHOW RESET);
    fflush(stdout);
}

static void term_raw(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(term_restore);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf(HIDE);
    fflush(stdout);
}

/* ─── Timing ─── */
static long elapsed_ms(struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000 +
           (now.tv_nsec - start->tv_nsec) / 1000000;
}

static void timer_reset(struct timespec *t) {
    clock_gettime(CLOCK_MONOTONIC, t);
}

/* ─── Input (non-blocking) ─── */
static int kbhit(void) {
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

static int read_key(void) {
    if (!kbhit()) return -1;
    unsigned char buf[8] = {0};
    int n = (int)read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) return -1;

    /* Parse escape sequences for arrow keys */
    if (n >= 3 && buf[0] == 27 && buf[1] == '[') {
        switch (buf[2]) {
            case 'A': return 'U';  /* Up */
            case 'B': return 'D';  /* Down */
            case 'C': return 'R';  /* Right */
            case 'D': return 'L';  /* Left */
        }
    }
    return buf[0];
}

/* ─── Piece helpers ─── */
static Cell piece_cell(int piece, int rot, int idx) {
    return SHAPES[piece][rot][idx];
}

static int piece_color(int piece) {
    return piece + 1;  /* colors 1-7 */
}

static int try_rotation(int rot, int dir) {
    return dir == DIR_CW ? (rot + 1) % 4 : (rot + 3) % 4;
}

/* ─── Collision detection ─── */
static int collides(Game *g, int piece, int rot, int row, int col) {
    for (int i = 0; i < 4; i++) {
        Cell c = piece_cell(piece, rot, i);
        int r = row + c.r, cc = col + c.c;
        if (cc < 0 || cc >= COLS || r >= ROWS + HIDDEN_ROWS) return 1;
        if (r >= 0 && g->board[r][cc]) return 1;
    }
    return 0;
}

/* ─── Place piece on board ─── */
static void place_piece(Game *g) {
    int color = piece_color(g->piece);
    for (int i = 0; i < 4; i++) {
        Cell c = piece_cell(g->piece, g->rotation, i);
        int r = g->row + c.r, cc = g->col + c.c;
        if (r >= 0 && r < ROWS + HIDDEN_ROWS && cc >= 0 && cc < COLS)
            g->board[r][cc] = color;
    }
}

/* ─── Check and clear full lines ─── */
static int check_lines(Game *g) {
    int count = 0, idx = 0;
    for (int r = 0; r < ROWS + HIDDEN_ROWS; r++) {
        int full = 1;
        for (int c = 0; c < COLS; c++) {
            if (!g->board[r][c]) { full = 0; break; }
        }
        if (full) {
            g->clearing_lines[idx++] = r;
            count++;
        }
    }
    g->clearing_lines[idx] = -1;
    return count;
}

static void remove_lines(Game *g) {
    for (int i = 0; g->clearing_lines[i] != -1; i++) {
        int r = g->clearing_lines[i];
        memmove(&g->board[1][0], &g->board[0][0], (size_t)(r) * COLS * sizeof(int));
        memset(&g->board[0][0], 0, COLS * sizeof(int));
        /* Adjust remaining line indices */
        for (int j = i + 1; g->clearing_lines[j] != -1; j++)
            g->clearing_lines[j]++;
    }
}

/* ─── Ghost piece row (where piece would land) ─── */
static int ghost_row(Game *g) {
    int r = g->row;
    while (!collides(g, g->piece, g->rotation, r + 1, g->col))
        r++;
    return r;
}

/* ─── Spawn a new piece ─── */
static void spawn_piece(Game *g) {
    g->piece = g->next_pieces[0];
    g->next_pieces[0] = g->next_pieces[1];
    g->next_pieces[1] = g->next_pieces[2];
    g->next_pieces[2] = bag_next();

    g->rotation = 0;
    g->row = 0;
    g->col = (COLS / 2) - 1;
    g->lock_active = 0;
    g->can_hold = 1;

    if (collides(g, g->piece, g->rotation, g->row, g->col)) {
        g->game_over = 1;
    }
}

/* ─── Wall kick rotation ─── */
static int rotate(Game *g, int dir) {
    int new_rot = try_rotation(g->rotation, dir);
    for (int k = 0; k < 5; k++) {
        const Cell *kicks;
        if (g->piece == PIECE_I) kicks = KICKS_I[g->rotation];
        else if (g->piece == PIECE_O) kicks = NULL;
        else kicks = KICKS_NORMAL[g->rotation];

        if (g->piece == PIECE_O) {
            if (!collides(g, g->piece, new_rot, g->row, g->col)) {
                g->rotation = new_rot;
                return 1;
            }
        } else {
            int dr = kicks[k].r, dc = kicks[k].c;
            if (!collides(g, g->piece, new_rot, g->row + dr, g->col + dc)) {
                g->row += dr;
                g->col += dc;
                g->rotation = new_rot;
                return 1;
            }
        }
    }
    return 0;
}

/* ─── Hold piece ─── */
static void hold(Game *g) {
    if (!g->can_hold) return;
    g->can_hold = 0;
    int tmp = g->hold_piece;
    g->hold_piece = g->piece;
    if (tmp < 0) {
        spawn_piece(g);
    } else {
        g->piece = tmp;
        g->rotation = 0;
        g->row = 0;
        g->col = (COLS / 2) - 1;
        g->lock_active = 0;
    }
}

/* ─── Hard drop ─── */
static void hard_drop(Game *g) {
    int dist = 0;
    while (!collides(g, g->piece, g->rotation, g->row + 1, g->col)) {
        g->row++;
        dist++;
    }
    g->score += dist * SCORE_HARD;
    place_piece(g);
    int lines = check_lines(g);
    if (lines > 0) {
        g->clear_timer = LINE_CLEAR_DELAY;
    } else {
        spawn_piece(g);
    }
}

/* ─── Rendering ─── */

/* Draw a 4×4 mini grid of a piece at screen position (y, x) */
static void draw_mini_piece(int y, int x, int piece, int show) {
    if (piece < 0 || !show) {
        for (int r = 0; r < 4; r++) {
            printf(ESC"%d;%dH      ", y + r, x);
        }
        return;
    }
    const char *color = PIECE_COLORS[piece_color(piece)];
    for (int r = 0; r < 4; r++) {
        printf(ESC"%d;%dH", y + r, x);
        for (int c = 0; c < 4; c++) {
            int found = 0;
            for (int i = 0; i < 4; i++) {
                Cell cell = piece_cell(piece, 0, i);
                if (cell.r == r && cell.c == c) { found = 1; break; }
            }
            if (found)
                printf("%s" BLOCK_FULL BLOCK_FULL RESET, color);
            else
                printf("  ");
        }
    }
}

static void draw_board(Game *g) {
    int top = 2;
    int left = 4;

    /* Title */
    printf(ESC"1;%dH\033[1;97m╔══ TETRIS ══╗" RESET, left + 4);

    /* Board border top */
    printf(ESC"%d;%dH\033[90m╔", top, left);
    for (int c = 0; c < COLS * 2; c++) printf("═");
    printf("╗");

    /* Board rows */
    for (int r = HIDDEN_ROWS; r < ROWS + HIDDEN_ROWS; r++) {
        int screen_r = top + 1 + (r - HIDDEN_ROWS);
        printf(ESC"%d;%dH\033[90m║", screen_r, left);

        for (int c = 0; c < COLS; c++) {
            int cell = g->board[r][c];
            /* Check if current piece occupies this cell */
            int is_piece = 0, is_ghost = 0;
            for (int i = 0; i < 4; i++) {
                Cell pc = piece_cell(g->piece, g->rotation, i);
                if (g->row + pc.r == r && g->col + pc.c == c) is_piece = 1;
            }
            if (!is_piece) {
                int gr = ghost_row(g);
                for (int i = 0; i < 4; i++) {
                    Cell pc = piece_cell(g->piece, g->rotation, i);
                    if (gr + pc.r == r && g->col + pc.c == c) is_ghost = 1;
                }
            }

            /* Check if this row is being cleared */
            int clearing = 0;
            for (int ci = 0; g->clearing_lines[ci] != -1; ci++) {
                if (g->clearing_lines[ci] == r) { clearing = 1; break; }
            }

            if (clearing && g->clear_timer > 0) {
                printf("\033[1;97m▓▓" RESET);
            } else if (is_piece) {
                printf("%s" BLOCK_FULL BLOCK_FULL RESET, PIECE_COLORS[piece_color(g->piece)]);
            } else if (is_ghost && !cell) {
                printf("\033[90m" BLOCK_GHOST BLOCK_GHOST RESET);
            } else if (cell) {
                printf("%s" BLOCK_FULL BLOCK_FULL RESET, PIECE_COLORS[cell]);
            } else {
                printf("\033[23m  " RESET);
            }
        }
        printf("\033[90m║" RESET);
    }

    /* Board border bottom */
    printf(ESC"%d;%dH\033[90m╚", top + ROWS + 1, left);
    for (int c = 0; c < COLS * 2; c++) printf("═");
    printf("╝" RESET);

    /* ─── Side panel ─── */
    int px = left + COLS * 2 + 4;

    /* Score section */
    printf(ESC"%d;%dH\033[1;93m┌─ SCORE ─┐", top, px);
    printf(ESC"%d;%dH│ \033[97mScore: \033[92m%-" "7d\033[90m│", top+1, px, g->score);
    printf(ESC"%d;%dH│ \033[97mLevel: \033[93m%-7d\033[90m│", top+2, px, g->level);
    printf(ESC"%d;%dH│ \033[97mLines: \033[96m%-7d\033[90m│", top+3, px, g->lines);
    printf(ESC"%d;%dH└──────────┘", top+4, px);

    /* Next pieces */
    printf(ESC"%d;%dH\033[1;93m┌─ NEXT ──┐", top+6, px);
    draw_mini_piece(top + 8, px + 2, g->next_pieces[0], 1);
    printf(ESC"%d;%dH\033[90m──────────", top + 12, px);
    draw_mini_piece(top + 13, px + 2, g->next_pieces[1], 1);
    printf(ESC"%d;%dH\033[90m──────────", top + 17, px);
    draw_mini_piece(top + 18, px + 2, g->next_pieces[2], 1);

    /* Hold piece */
    int hx = left - 14;
    if (hx < 2) hx = 2;
    printf(ESC"%d;%dH\033[1;93m┌─ HOLD ──┐", top, hx);
    draw_mini_piece(top + 2, hx + 2, g->hold_piece, g->hold_piece >= 0);
    printf(ESC"%d;%dH└──────────┘" RESET, top + 6, hx);

    /* Controls */
    int cy = top + ROWS - 6;
    printf(ESC"%d;%dH\033[90m── Controls ──", cy, hx - 2);
    printf(ESC"%d;%dH\033[37m← →  Move", cy+1, hx);
    printf(ESC"%d;%dH ↑   Rotate", cy+2, hx);
    printf(ESC"%d;%dH ↓   Soft drop", cy+3, hx);
    printf(ESC"%d;%dH SPC Hard drop", cy+4, hx);
    printf(ESC"%d;%dH C   Hold", cy+5, hx);
    printf(ESC"%d;%dH P   Pause", cy+6, hx);
    printf(ESC"%d;%dH Q   Quit" RESET, cy+7, hx);
}

/* ─── Game Over overlay ─── */
static void draw_game_over(Game *g) {
    printf("\033[1;97m");
    int cy = 8, cx = 12;
    printf(ESC"%d;%dH╔══════════════════════════════╗", cy, cx);
    printf(ESC"%d;%dH║       \033[91mG A M E   O V E R\033[97m       ║", cy+1, cx);
    printf(ESC"%d;%dH╠══════════════════════════════╣", cy+2, cx);
    printf(ESC"%d;%dH║                              ║", cy+3, cx);
    printf(ESC"%d;%dH║  \033[93mFinal Score: \033[92m%-12d\033[97m  ║", cy+4, cx, g->score);
    printf(ESC"%d;%dH║  \033[93mLevel:       \033[93m%-12d\033[97m  ║", cy+5, cx, g->level);
    printf(ESC"%d;%dH║  \033[93mLines:       \033[96m%-12d\033[97m  ║", cy+6, cx, g->lines);
    printf(ESC"%d;%dH║                              ║", cy+7, cx);
    printf(ESC"%d;%dH║    \033[97mPress R to restart       \033[97m║", cy+8, cx);
    printf(ESC"%d;%dH║    \033[97mPress Q to quit           \033[97m║", cy+9, cx);
    printf(ESC"%d;%dH╚══════════════════════════════╝" RESET, cy+10, cx);
}

static void draw_pause(void) {
    int cy = 10, cx = 14;
    printf(ESC"%d;%dH\033[1;97m╔════════════════════╗", cy, cx);
    printf(ESC"%d;%dH║   \033[93mP A U S E D\033[97m      ║", cy+1, cx);
    printf(ESC"%d;%dH║                    ║", cy+2, cx);
    printf(ESC"%d;%dH║  Press P to resume ║", cy+3, cx);
    printf(ESC"%d;%dH╚════════════════════╝" RESET, cy+4, cx);
}

/* ─── Line clear score calculation ─── */
static int line_score(int lines, int level) {
    int base;
    switch (lines) {
        case 1: base = SCORE_1LINE; break;
        case 2: base = SCORE_2LINE; break;
        case 3: base = SCORE_3LINE; break;
        case 4: base = SCORE_4LINE; break;
        default: base = 0;
    }
    return base * (level + 1);
}

/* ─── Fall speed for current level ─── */
static int fall_interval(int level) {
    int ms = BASE_FALL_INTERVAL - level * 70;
    return ms < MIN_FALL_INTERVAL ? MIN_FALL_INTERVAL : ms;
}

/* ─── Initialize game ─── */
static void game_init(Game *g) {
    memset(g, 0, sizeof(Game));
    g->hold_piece = -1;
    bag_init();
    for (int i = 0; i < 3; i++) g->next_pieces[i] = bag_next();
    spawn_piece(g);
    timer_reset(&g->last_tick);
}

/* ─── Main loop ─── */
int main(void) {
    srand((unsigned)time(NULL));
    term_raw();
    printf(ESC"2J");  /* clear screen */

    Game game;
    game_init(&game);

    int soft_dropping = 0;

    while (1) {
        draw_board(&game);

        if (game.game_over) {
            draw_game_over(&game);
            fflush(stdout);
            while (1) {
                int ch = read_key();
                if (ch == 'q' || ch == 'Q') goto quit;
                if (ch == 'r' || ch == 'R') {
                    game_init(&game);
                    soft_dropping = 0;
                    printf(ESC"2J");
                    break;
                }
                { struct timespec _ts = {0, 50000000}; nanosleep(&_ts, NULL); }
            }
            continue;
        }

        if (game.paused) {
            draw_pause();
            fflush(stdout);
            while (1) {
                int ch = read_key();
                if (ch == 'p' || ch == 'P') { game.paused = 0; timer_reset(&game.last_tick); break; }
                if (ch == 'q' || ch == 'Q') goto quit;
                { struct timespec _ts = {0, 50000000}; nanosleep(&_ts, NULL); }
            }
            continue;
        }

        fflush(stdout);

        /* Process input */
        int ch = read_key();
        if (ch >= 0) {
            switch (ch) {
                case 'L': /* left */
                    if (!collides(&game, game.piece, game.rotation, game.row, game.col - 1))
                        game.col--;
                    game.lock_active = 0;
                    break;
                case 'R': /* right */
                    if (!collides(&game, game.piece, game.rotation, game.row, game.col + 1))
                        game.col++;
                    game.lock_active = 0;
                    break;
                case 'U': /* up = rotate CW */
                    rotate(&game, DIR_CW);
                    break;
                case 'D': /* down = soft drop */
                    soft_dropping = 1;
                    break;
                case 'z': case 'Z': /* rotate CCW */
                    rotate(&game, DIR_CCW);
                    break;
                case ' ': /* hard drop */
                    hard_drop(&game);
                    break;
                case 'c': case 'C':
                    hold(&game);
                    break;
                case 'p': case 'P':
                    game.paused = 1;
                    break;
                case 'q': case 'Q':
                    goto quit;
            }
        }

        /* Timers */
        long dt = elapsed_ms(&game.last_tick);
        timer_reset(&game.last_tick);

        /* Line clear animation */
        if (game.clear_timer > 0) {
            game.clear_timer -= (int)dt;
            if (game.clear_timer <= 0) {
                int lines = 0;
                for (int i = 0; game.clearing_lines[i] != -1; i++) lines++;
                game.score += line_score(lines, game.level);
                game.lines += lines;
                game.level = game.lines / 10;
                remove_lines(&game);
                for (int i = 0; i < 4; i++) game.clearing_lines[i] = -1;
                spawn_piece(&game);
            }
            { struct timespec _ts = {0, 16000000}; nanosleep(&_ts, NULL); }
            continue;
        }

        /* Soft drop */
        if (soft_dropping) {
            if (!collides(&game, game.piece, game.rotation, game.row + 1, game.col)) {
                game.row++;
                game.score += SCORE_SOFT;
            }
            soft_dropping = 0;
        }

        /* Gravity / fall */
        game.fall_timer += (int)dt;
        int interval = fall_interval(game.level);
        if (game.fall_timer >= interval) {
            game.fall_timer = 0;
            if (!collides(&game, game.piece, game.rotation, game.row + 1, game.col)) {
                game.row++;
                game.lock_active = 0;
            }
        }

        /* Lock delay */
        if (collides(&game, game.piece, game.rotation, game.row + 1, game.col)) {
            if (!game.lock_active) {
                game.lock_active = 1;
                game.lock_timer = LOCK_DELAY;
            }
            game.lock_timer -= (int)dt;
            if (game.lock_timer <= 0) {
                place_piece(&game);
                int lines = check_lines(&game);
                if (lines > 0) {
                    game.clear_timer = LINE_CLEAR_DELAY;
                } else {
                    spawn_piece(&game);
                }
            }
        } else {
            game.lock_active = 0;
        }

        { struct timespec _ts = {0, 16000000}; nanosleep(&_ts, NULL); }  /* ~60fps */
    }

quit:
    printf(ESC"2J" ESC"1;1H" SHOW);
    printf("\033[93mThanks for playing Terminal Tetris!\033[0m\n");
    term_restore();
    return 0;
}
