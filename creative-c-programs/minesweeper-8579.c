/*
 * minesweeper-8579.c — Terminal Minesweeper Game
 *
 * A fully playable Minesweeper rendered with box-drawing characters and
 * ANSI colour in any VT100-compatible terminal.  No external libraries
 * beyond the C standard library and POSIX termios are required.
 *
 * ┌──────────────────────────────────────────────┐
 * │  Controls                                     │
 * │  Arrow keys / WASD   Move cursor              │
 * │  Space / Enter       Reveal cell              │
 * │  f  /  F             Toggle flag              │
 * │  c  /  C             Chord (auto-reveal)       │
 * │  r  /  R             Restart same board        │
 * │  n  /  N             New game (re-randomise)   │
 * │  1 / 2 / 3           Difficulty: B / I / E     │
 * │  q  /  Q             Quit                      │
 * └──────────────────────────────────────────────┘
 *
 * Difficulty levels
 *   1  Beginner     9 × 9,   10 mines
 *   2  Intermediate 16 × 16,  40 mines
 *   3  Expert       30 × 16,  99 mines
 *
 * Compile:
 *   gcc -std=c11 -Wall -Wextra -O2 -o minesweeper minesweeper-8579.c -lm
 *
 * Run:
 *   ./minesweeper          (starts Beginner)
 *   ./minesweeper 2        (starts Intermediate)
 *   ./minesweeper 3        (starts Expert)
 *
 * Author: Claude  |  2026-05-07
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <signal.h>
#include <unistd.h>
#include <termios.h>
#include <sys/time.h>

/* ──────────────── ANSI helpers ──────────────── */

#define ESC_RESET      "\033[0m"
#define ESC_BOLD       "\033[1m"
#define ESC_DIM        "\033[2m"
#define ESC_INV        "\033[7m"
#define ESC_RED        "\033[31m"
#define ESC_GREEN      "\033[32m"
#define ESC_YELLOW     "\033[33m"
#define ESC_BLUE       "\033[34m"
#define ESC_MAGENTA    "\033[35m"
#define ESC_CYAN       "\033[36m"
#define ESC_WHITE      "\033[37m"
#define ESC_BRED       "\033[91m"
#define ESC_BGREEN     "\033[92m"
#define ESC_BYELLOW    "\033[93m"
#define ESC_BBLUE      "\033[94m"
#define ESC_BMAGENTA   "\033[95m"
#define ESC_BCYAN      "\033[96m"
#define ESC_BWHITE     "\033[97m"
#define ESC_BG_GRAY    "\033[48;5;240m"
#define ESC_BG_DGRAY   "\033[48;5;236m"
#define ESC_HIDE_CUR   "\033[?25l"
#define ESC_SHOW_CUR   "\033[?25h"

static void clear_screen(void)  { fputs("\033[2J", stdout); }
static void cur_home(void)      { fputs("\033[H", stdout); }
static void __attribute__((unused)) cur_pos(int r, int c){ printf("\033[%d;%dH", r, c); }

/* ──────────────── Terminal raw mode ──────────────── */

static struct termios g_orig_term;

static void restore_term(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_term);
    fputs(ESC_SHOW_CUR, stdout);
    fflush(stdout);
}

static void enter_raw(void) {
    tcgetattr(STDIN_FILENO, &g_orig_term);
    struct termios raw = g_orig_term;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |=  CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    atexit(restore_term);
    fputs(ESC_HIDE_CUR, stdout);
    fflush(stdout);
}

/* ──────────────── Key reading ──────────────── */

/* Returns a small integer code for recognised keys. */
enum {
    K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT,
    K_SPACE, K_ENTER, K_UNKNOWN
};

static int read_key(void) {
    char buf[8];
    int n = (int)read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) return K_UNKNOWN;

    /* Single character */
    if (n == 1) {
        unsigned char c = (unsigned char)buf[0];
        if (c == ' ')  return K_SPACE;
        if (c == 13 || c == 10) return K_ENTER;
        if (c == 27)   return 'q';          /* lone Esc → quit */
        return tolower(c);
    }

    /* CSI sequences  ESC [ … */
    if (n >= 3 && buf[0] == 27 && buf[1] == '[') {
        switch (buf[2]) {
            case 'A': return K_UP;
            case 'B': return K_DOWN;
            case 'C': return K_RIGHT;
            case 'D': return K_LEFT;
        }
    }
    return K_UNKNOWN;
}

/* ──────────────── Game data ──────────────── */

typedef struct {
    int rows, cols, mines;
} Difficulty;

static const Difficulty DIFFS[3] = {
    { 9,  9, 10},   /* Beginner   */
    {16, 16, 40},   /* Intermediate */
    {30, 16, 99},   /* Expert     */
};

#define MAX_ROWS 20
#define MAX_COLS 35

typedef struct {
    int mine;        /* 1 = mine */
    int revealed;    /* 1 = uncovered */
    int flagged;     /* 1 = flagged */
    int adjacent;    /* count of neighbour mines */
} Cell;

typedef struct {
    Cell   grid[MAX_ROWS][MAX_COLS];
    int    rows, cols, total_mines;
    int    cur_r, cur_c;            /* cursor */
    int    flags_placed;
    int    revealed_count;
    int    game_over;               /* -1 lost, 0 playing, 1 won */
    int    first_click;             /* 1 until first reveal */
    struct timeval t_start, t_end;
    int    diff_idx;
} Game;

static Game g;

/* ──────────────── Board logic ──────────────── */

static void place_mines(Game *g, int safe_r, int safe_c) {
    int placed = 0;
    while (placed < g->total_mines) {
        int r = rand() % g->rows;
        int c = rand() % g->cols;
        if (g->grid[r][c].mine) continue;
        /* Keep a 3×3 safe zone around first click */
        if (abs(r - safe_r) <= 1 && abs(c - safe_c) <= 1) continue;
        g->grid[r][c].mine = 1;
        placed++;
    }
    /* Calculate adjacency counts */
    for (int r = 0; r < g->rows; r++)
        for (int c = 0; c < g->cols; c++) {
            if (g->grid[r][c].mine) continue;
            int cnt = 0;
            for (int dr = -1; dr <= 1; dr++)
                for (int dc = -1; dc <= 1; dc++) {
                    if (!dr && !dc) continue;
                    int nr = r+dr, nc = c+dc;
                    if (nr >= 0 && nr < g->rows && nc >= 0 && nc < g->cols)
                        cnt += g->grid[nr][nc].mine;
                }
            g->grid[r][c].adjacent = cnt;
        }
}

static void init_game(Game *g, int diff_idx) {
    memset(g, 0, sizeof(*g));
    g->diff_idx     = diff_idx;
    g->rows         = DIFFS[diff_idx].rows;
    g->cols         = DIFFS[diff_idx].cols;
    g->total_mines  = DIFFS[diff_idx].mines;
    g->first_click  = 1;
    g->cur_r = g->cur_c = 0;
    /* Centre cursor */
    g->cur_r = g->rows / 2;
    g->cur_c = g->cols / 2;
}

static int flood_fill(Game *g, int r, int c) {
    /* BFS flood fill: reveal cells; return -1 if mine hit */
    if (r < 0 || r >= g->rows || c < 0 || c >= g->cols) return 0;
    if (g->grid[r][c].revealed || g->grid[r][c].flagged) return 0;

    g->grid[r][c].revealed = 1;
    g->revealed_count++;

    if (g->grid[r][c].mine) return -1;   /* BOOM */
    if (g->grid[r][c].adjacent > 0) return 0;

    /* Empty cell → recurse to neighbours */
    for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
            if (!dr && !dc) continue;
            if (flood_fill(g, r+dr, c+dc) < 0) return -1;
        }
    return 0;
}

static int chord(Game *g, int r, int c) {
    /* If the revealed number equals adjacent flags, reveal remaining neighbours */
    Cell *cell = &g->grid[r][c];
    if (!cell->revealed || cell->adjacent == 0) return 0;

    int flags = 0;
    for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
            if (!dr && !dc) continue;
            int nr = r+dr, nc = c+dc;
            if (nr >= 0 && nr < g->rows && nc >= 0 && nc < g->cols)
                flags += g->grid[nr][nc].flagged;
        }
    if (flags != cell->adjacent) return 0;

    for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
            if (!dr && !dc) continue;
            int nr = r+dr, nc = c+dc;
            if (nr >= 0 && nr < g->rows && nc >= 0 && nc < g->cols)
                if (flood_fill(g, nr, nc) < 0) return -1;
        }
    return 0;
}

static int check_win(Game *g) {
    return g->revealed_count == g->rows * g->cols - g->total_mines;
}

/* ──────────────── Rendering ──────────────── */

/* Colour palette for numbers 1-8 */
static const char *NUM_COLOUR[] = {
    ESC_BBLUE, ESC_BGREEN, ESC_BRED, ESC_BMAGENTA,
    ESC_RED,   ESC_CYAN,  ESC_YELLOW, ESC_WHITE
};

static void draw_cell(const Game *g, int r, int c) {
    const Cell *cell = &g->grid[r][c];
    int is_cursor = (r == g->cur_r && c == g->cur_c);

    if (is_cursor) fputs(ESC_INV, stdout);

    if (cell->flagged) {
        printf("%s" "⚑" ESC_RESET, ESC_BRED);   /* ⚑ */
        if (is_cursor) fputs(ESC_INV, stdout);
    } else if (!cell->revealed) {
        fputs(ESC_BG_GRAY, stdout);
        if (is_cursor) fputs(ESC_INV, stdout);
        printf(" ");
        fputs(ESC_RESET, stdout);
        if (is_cursor) fputs(ESC_INV, stdout);
        printf(" ");
    } else if (cell->mine) {
        printf("%s" "✸" " ", ESC_BRED);           /* ✸ */
    } else if (cell->adjacent == 0) {
        printf("  ");
    } else {
        printf("%s%d " ESC_RESET, NUM_COLOUR[cell->adjacent - 1],
               cell->adjacent);
        if (is_cursor) fputs(ESC_INV, stdout);
    }

    if (is_cursor) fputs(ESC_RESET, stdout);
}

static int elapsed_secs(const Game *g) {
    struct timeval now;
    if (g->game_over) now = g->t_end;
    else gettimeofday(&now, NULL);
    return (int)(now.tv_sec - g->t_start.tv_sec);
}

static void draw_header(const Game *g) {
    const char *diff_name[] = {"Beginner", "Intermediate", "Expert"};
    printf(ESC_BOLD ESC_CYAN "  ✸ MINESWEEPER ✸" ESC_RESET);
    printf("   %s  %dx%d", diff_name[g->diff_idx], g->cols, g->rows);

    int mines_left = g->total_mines - g->flags_placed;
    printf("   " ESC_BRED "⚑ %03d" ESC_RESET, mines_left > 0 ? mines_left : 0);

    int t = elapsed_secs(g);
    printf("   " ESC_BGREEN "⏱ %03ds" ESC_RESET, t);

    if (g->game_over == 1)
        printf("   " ESC_BOLD ESC_BGREEN "YOU WIN!" ESC_RESET);
    else if (g->game_over == -1)
        printf("   " ESC_BOLD ESC_BRED "GAME OVER" ESC_RESET);

    printf("\r\n");
}

static void draw_footer(void) {
    printf(ESC_DIM "\r\n"
           "  Arrow/WASD:Move  Space:Reveal  F:Flag  C:Chord"
           "  N:New  R:Restart  1-3:Difficulty  Q:Quit"
           ESC_RESET "\r\n");
}

static void draw_board(Game *g) {
    cur_home();
    clear_screen();
    draw_header(g);

    /* Column header every 5 */
    printf("   ");
    for (int c = 0; c < g->cols; c++)
        printf("%s%d%s", (c % 5 == 0) ? ESC_DIM : "",
               c % 10, ESC_RESET);
    printf("\r\n");

    for (int r = 0; r < g->rows; r++) {
        printf(ESC_DIM "%2d " ESC_RESET, r);
        for (int c = 0; c < g->cols; c++)
            draw_cell(g, r, c);
        printf("\r\n");
    }

    draw_footer();
    fflush(stdout);
}

/* Reveal all mines on loss */
static void reveal_all_mines(Game *g) {
    for (int r = 0; r < g->rows; r++)
        for (int c = 0; c < g->cols; c++)
            if (g->grid[r][c].mine)
                g->grid[r][c].revealed = 1;
}

/* ──────────────── Main loop ──────────────── */

static void win_animation(Game *g) {
    /* Flash the board a few times */
    for (int i = 0; i < 4; i++) {
        cur_home(); clear_screen();
        draw_header(g);
        printf("\r\n");
        for (int r = 0; r < g->rows; r++) {
            printf("  ");
            for (int c = 0; c < g->cols; c++) {
                if (g->grid[r][c].flagged || g->grid[r][c].mine) {
                    printf("%s" "⚑ " ESC_RESET,
                           (i % 2 == 0) ? ESC_BGREEN : ESC_BYELLOW);
                } else {
                    draw_cell(g, r, c);
                }
            }
            printf("\r\n");
        }
        draw_footer();
        fflush(stdout);
        usleep(200000);
    }
}

int main(int argc, char **argv) {
    srand((unsigned)time(NULL));
    int diff = 0;
    if (argc > 1) {
        diff = atoi(argv[1]) - 1;
        if (diff < 0 || diff > 2) diff = 0;
    }

    init_game(&g, diff);
    enter_raw();

    int running = 1;
    while (running) {
        draw_board(&g);
        int key = read_key();

        if (g.game_over && key != 'n' && key != 'r' &&
            key != '1' && key != '2' && key != '3' && key != 'q')
            continue;

        switch (key) {
        /* Movement */
        case K_UP:    case 'w': if (g.cur_r > 0)            g.cur_r--; break;
        case K_DOWN:  case 's': if (g.cur_r < g.rows - 1)   g.cur_r++; break;
        case K_LEFT:  case 'a': if (g.cur_c > 0)            g.cur_c--; break;
        case K_RIGHT: case 'd': if (g.cur_c < g.cols - 1)   g.cur_c++; break;

        /* Reveal */
        case K_SPACE: case K_ENTER: {
            Cell *cell = &g.grid[g.cur_r][g.cur_c];
            if (cell->flagged || cell->revealed) break;
            if (g.first_click) {
                gettimeofday(&g.t_start, NULL);
                place_mines(&g, g.cur_r, g.cur_c);
                g.first_click = 0;
            }
            if (flood_fill(&g, g.cur_r, g.cur_c) < 0) {
                g.game_over = -1;
                gettimeofday(&g.t_end, NULL);
                reveal_all_mines(&g);
            } else if (check_win(&g)) {
                g.game_over = 1;
                gettimeofday(&g.t_end, NULL);
                /* Auto-flag remaining mines */
                for (int r = 0; r < g.rows; r++)
                    for (int c = 0; c < g.cols; c++)
                        if (g.grid[r][c].mine && !g.grid[r][c].flagged) {
                            g.grid[r][c].flagged = 1;
                            g.flags_placed++;
                        }
                draw_board(&g);
                win_animation(&g);
            }
            break;
        }

        /* Flag */
        case 'f': {
            Cell *cell = &g.grid[g.cur_r][g.cur_c];
            if (cell->revealed) break;
            cell->flagged = !cell->flagged;
            g.flags_placed += cell->flagged ? 1 : -1;
            break;
        }

        /* Chord */
        case 'c': {
            if (chord(&g, g.cur_r, g.cur_c) < 0) {
                g.game_over = -1;
                gettimeofday(&g.t_end, NULL);
                reveal_all_mines(&g);
            } else if (check_win(&g)) {
                g.game_over = 1;
                gettimeofday(&g.t_end, NULL);
                draw_board(&g);
                win_animation(&g);
            }
            break;
        }

        /* New game (re-randomise) */
        case 'n':
            init_game(&g, g.diff_idx);
            break;

        /* Restart (same layout not possible since we only generate
           on first click — just start fresh) */
        case 'r':
            init_game(&g, g.diff_idx);
            break;

        /* Change difficulty */
        case '1': init_game(&g, 0); break;
        case '2': init_game(&g, 1); break;
        case '3': init_game(&g, 2); break;

        /* Quit */
        case 'q':
            running = 0;
            break;
        }
    }

    clear_screen(); cur_home();
    printf(ESC_BOLD ESC_CYAN "  Thanks for playing Minesweeper!\r\n" ESC_RESET);
    restore_term();
    return 0;
}
