/*
 * sudoku-2765.c - Terminal Sudoku Puzzle Game
 *
 * Features:
 *   - Puzzle generator with guaranteed unique solutions
 *   - 3 difficulty levels (Easy / Medium / Hard)
 *   - Interactive cursor-based play with arrow keys
 *   - Pencil marks (candidate numbers in empty cells)
 *   - Hint system (reveals correct number for one cell)
 *   - Undo support (full move history)
 *   - Real-time error highlighting (conflicts in row/col/box)
 *   - Timer with elapsed time display
 *   - Auto-solve with animated backtracking
 *   - Win celebration animation
 *
 * Compile:  gcc -O2 -o sudoku sudoku-2765.c
 *     or:   cc -o sudoku sudoku-2765.c
 * Run:      ./sudoku
 *
 * Controls:
 *   Arrow keys / hjkl   Move cursor
 *   1-9                 Place number (or pencil mark in pencil mode)
 *   0 / Delete / BS     Clear cell
 *   p                   Toggle pencil marks mode
 *   H                   Hint (reveal one cell)
 *   a                   Auto-solve with animation
 *   u                   Undo last move
 *   n                   New game (same difficulty)
 *   d                   Cycle difficulty and start new game
 *   r / Ctrl-L          Redraw screen
 *   q / Escape          Quit
 *
 * Requires: ANSI/VT100 terminal with color support. No external libraries.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <sys/time.h>

#define N 9
#define BOX 3

/* ── ANSI helpers ──────────────────────────────────────────────── */

#define CSI       "\033["
#define RESET     CSI "0m"
#define BOLD      CSI "1m"
#define DIM       CSI "2m"

#define COL_GIVEN  CSI "38;5;252m"   /* bright white   */
#define COL_PLAY   CSI "38;5;117m"   /* light cyan     */
#define COL_PENCIL CSI "38;5;244m"   /* gray           */
#define COL_ERR    CSI "38;5;196m"   /* bright red     */
#define COL_CUR    CSI "48;5;237m"   /* dark gray bg   */
#define COL_THICK  CSI "38;5;246m"   /* box borders    */
#define COL_THIN   CSI "38;5;239m"   /* cell borders   */
#define COL_TITLE  CSI "38;5;214m"   /* orange title   */
#define COL_HINT   CSI "38;5;150m"   /* light green    */
#define COL_WIN    CSI "38;5;226m"   /* bright yellow  */
#define COL_STATUS CSI "38;5;180m"   /* gold status    */

#define CLR_SCR    CSI "2J" CSI "H"
#define HIDE_CUR   CSI "?25l"
#define SHOW_CUR   CSI "?25h"
#define GO(r,c)    CSI #r ";" #c "H"

#define EASY_CLUES   38
#define MEDIUM_CLUES 30
#define HARD_CLUES   25

/* ── Data structures ───────────────────────────────────────────── */

typedef struct {
    int row, col, prev_val, is_pencil, pencil_digit;
} Move;

typedef struct {
    int puzzle[N][N];
    int board[N][N];
    int pencil[N][N][N]; /* [r][c][d] d=0..8 => digit d+1 */
    int given[N][N];
    int solved, pencil_mode;
    int cur_r, cur_c;
    int difficulty, hints_used;
    Move history[1024];
    int hist_len;
    struct timeval t0;
} Game;

/* ── Terminal ──────────────────────────────────────────────────── */

static struct termios orig_tio;
static int tio_saved = 0;

static void restore_term(void) {
    if (tio_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_tio);
        printf(SHOW_CUR RESET);
        fflush(stdout);
    }
}

static void set_raw(void) {
    if (!tio_saved) {
        tcgetattr(STDIN_FILENO, &orig_tio);
        tio_saved = 1;
        atexit(restore_term);
    }
    struct termios r = orig_tio;
    r.c_lflag &= ~(ECHO | ICANON | ISIG);
    r.c_cc[VMIN] = 1;
    r.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &r);
}

static void gotoxy(int r, int c) { printf("\033[%d;%dH", r, c); }

static int read_key(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    if (c == 27) {
        unsigned char s[2];
        if (read(STDIN_FILENO, &s[0], 1) != 1) return 27;
        if (s[0] != '[') return 27;
        if (read(STDIN_FILENO, &s[1], 1) != 1) return 27;
        switch (s[1]) {
            case 'A': return 'U';  /* up    */
            case 'B': return 'D';  /* down  */
            case 'C': return 'R';  /* right */
            case 'D': return 'L';  /* left  */
            case '3':              /* Delete */
                if (read(STDIN_FILENO, &s[0], 1) < 0) return 27;
                return 127;
        }
        return 27;
    }
    return c;
}

/* ── Sudoku logic ──────────────────────────────────────────────── */

static void shuffle(int *a, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int t = a[i]; a[i] = a[j]; a[j] = t;
    }
}

static int can_place(int g[N][N], int r, int c, int d) {
    for (int i = 0; i < N; i++) {
        if (g[r][i] == d) return 0;
        if (g[i][c] == d) return 0;
    }
    int br = r / BOX * BOX, bc = c / BOX * BOX;
    for (int i = br; i < br + BOX; i++)
        for (int j = bc; j < bc + BOX; j++)
            if (g[i][j] == d) return 0;
    return 1;
}

static int fill_grid(int g[N][N], int pos) {
    if (pos == N * N) return 1;
    int r = pos / N, c = pos % N;
    int d[N] = {1,2,3,4,5,6,7,8,9};
    shuffle(d, N);
    for (int i = 0; i < N; i++) {
        if (can_place(g, r, c, d[i])) {
            g[r][c] = d[i];
            if (fill_grid(g, pos + 1)) return 1;
            g[r][c] = 0;
        }
    }
    return 0;
}

/* Count solutions up to `limit` (MRV heuristic for speed) */
static int count_sol(int g[N][N], int limit) {
    int br = -1, bc = -1, best = N + 1;
    for (int i = 0; i < N * N; i++) {
        if (g[i/N][i%N] == 0) {
            int opts = 0;
            for (int d = 1; d <= N; d++)
                if (can_place(g, i/N, i%N, d)) opts++;
            if (opts == 0) return 0;
            if (opts < best) { best = opts; br = i/N; bc = i%N; }
        }
    }
    if (br < 0) return 1;
    int cnt = 0;
    for (int d = 1; d <= N; d++) {
        if (can_place(g, br, bc, d)) {
            g[br][bc] = d;
            cnt += count_sol(g, limit - cnt);
            g[br][bc] = 0;
            if (cnt >= limit) return cnt;
        }
    }
    return cnt;
}

static void generate(int p[N][N], int clues) {
    int g[N][N] = {{0}};
    fill_grid(g, 0);
    memcpy(p, g, sizeof(int)*N*N);

    int order[N*N];
    for (int i = 0; i < N*N; i++) order[i] = i;
    shuffle(order, N*N);

    int rem = 0, target = N*N - clues;
    for (int i = 0; i < N*N && rem < target; i++) {
        int r = order[i]/N, c = order[i]%N;
        int bak = p[r][c];
        p[r][c] = 0;
        int cp[N][N];
        memcpy(cp, p, sizeof(cp));
        if (count_sol(cp, 2) == 1) rem++;
        else p[r][c] = bak;
    }
}

/* Simple backtracking solver (returns 1 if solved) */
static int solve(int g[N][N]) {
    for (int i = 0; i < N*N; i++) {
        int r = i/N, c = i%N;
        if (g[r][c] == 0) {
            for (int d = 1; d <= N; d++) {
                if (can_place(g, r, c, d)) {
                    g[r][c] = d;
                    if (solve(g)) return 1;
                    g[r][c] = 0;
                }
            }
            return 0;
        }
    }
    return 1;
}

/* Animated solver — returns 1 if solved */
static int solve_anim(int g[N][N], int delay_us) {
    for (int i = 0; i < N*N; i++) {
        int r = i/N, c = i%N;
        if (g[r][c] == 0) {
            for (int d = 1; d <= N; d++) {
                if (can_place(g, r, c, d)) {
                    g[r][c] = d;
                    /* show attempt */
                    gotoxy(6 + r + (r/3), 4 + c*4 + (c/3));
                    printf(COL_HINT "%d" RESET, d);
                    fflush(stdout);
                    usleep(delay_us);

                    if (solve_anim(g, delay_us)) return 1;
                    g[r][c] = 0;

                    gotoxy(6 + r + (r/3), 4 + c*4 + (c/3));
                    printf("·");
                    fflush(stdout);
                    usleep(delay_us / 3);
                }
            }
            return 0;
        }
    }
    return 1;
}

static int has_conflict(int b[N][N], int r, int c) {
    int d = b[r][c];
    if (!d) return 0;
    for (int i = 0; i < N; i++) {
        if (i != c && b[r][i] == d) return 1;
        if (i != r && b[i][c] == d) return 1;
    }
    int br = r/BOX*BOX, bc = c/BOX*BOX;
    for (int i = br; i < br+BOX; i++)
        for (int j = bc; j < bc+BOX; j++)
            if ((i != r || j != c) && b[i][j] == d) return 1;
    return 0;
}

static int is_complete(Game *g) {
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            if (!g->board[r][c] || has_conflict(g->board, r, c)) return 0;
    return 1;
}

/* ── Drawing ───────────────────────────────────────────────────── */

/* Grid geometry:
 *   Each cell: 3 chars wide, 1 char tall
 *   Number at offset (col*4 + 2) from left, (row + offset) from top
 *   Extra gap at box boundaries (cols 3,6 and rows 3,6)
 *
 *   Grid left margin = 4, top row = 6
 *   Cell (r,c) value at screen: row = 6+r+(r/3), col = 4+c*4+(c/3)
 */

#define GLEFT 4
#define GTOP  6

static int cell_row(int r) { return GTOP + r + r/3; }
static int cell_col(int c) { return GLEFT + c*4 + c/3; }

static void draw_grid_lines(void) {
    /* Top border */
    gotoxy(GTOP - 1, GLEFT - 1);
    printf(COL_THICK "┌───┬───┬───┬───┬───┬───┬───┬───┬───┐");

    for (int r = 0; r < N; r++) {
        /* Thin row separators */
        if (r > 0) {
            gotoxy(GTOP + r + (r-1)/3, GLEFT - 1);
            if (r % 3 == 0)
                printf(COL_THICK "├━━━╂───┼───┼───╂───┼───┼───╂───┼───┼───┤");
            else
                printf(COL_THIN  "├───┼───┼───┼───┼───┼───┼───┼───┼───┤");
        }
    }
    /* Top border */
    gotoxy(GTOP - 1, GLEFT - 1);
    printf(COL_THICK "┏━━━┯━━━┯━━━┳━━━┯━━━┯━━━┳━━━┯━━━┯━━━┓" RESET);

    for (int r = 0; r < N; r++) {
        /* Data row placeholder (vertical lines only) */
        gotoxy(GTOP + r + r/3, GLEFT - 1);
        printf(COL_THICK "┃");
        for (int c = 0; c < N; c++) {
            printf("   ");   /* 3 spaces for cell content */
            if (c < N-1) {
                if ((c+1) % 3 == 0) printf(COL_THICK "┃");
                else                printf(COL_THIN "│");
            }
        }
        printf(COL_THICK "┃" RESET);

        /* Horizontal line below row */
        if (r < N-1) {
            gotoxy(GTOP + r + r/3 + 1, GLEFT - 1);
            if ((r+1) % 3 == 0) {
                printf(COL_THICK "┣━━━╋━━━╋━━━╋━━━╋━━━╋━━━╋━━━╋━━━╋━━━┫" RESET);
            } else {
                printf(COL_THICK "┃" COL_THIN "───┼───┼───" COL_THICK "╋"
                       COL_THIN "───┼───┼───" COL_THICK "╋"
                       COL_THIN "───┼───┼───" COL_THICK "┃" RESET);
            }
        }
    }

    /* Bottom border */
    gotoxy(GTOP + N + N/3, GLEFT - 1);
    printf(COL_THICK "┗━━━┷━━━┷━━━┻━━━┷━━━┷━━━┻━━━┷━━━┷━━━┛" RESET);
}

static void draw_cells(Game *g) {
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            int sr = cell_row(r), sc = cell_col(c);
            gotoxy(sr, sc);

            int cur = (r == g->cur_r && c == g->cur_c);
            int err = has_conflict(g->board, r, c);

            if (cur) printf(COL_CUR);

            if (g->board[r][c]) {
                if (g->given[r][c])
                    printf(BOLD COL_GIVEN "%d" RESET, g->board[r][c]);
                else if (err)
                    printf(BOLD COL_ERR "%d" RESET, g->board[r][c]);
                else
                    printf(COL_PLAY "%d" RESET, g->board[r][c]);
            } else {
                /* Pencil marks */
                int any = 0;
                for (int d = 0; d < N; d++)
                    if (g->pencil[r][c][d]) any = 1;
                if (any) {
                    printf(DIM COL_PENCIL);
                    for (int d = 0; d < N; d++)
                        printf("%d", g->pencil[r][c][d] ? d+1 : 0);
                    printf(RESET);
                    /* overwrite 0s with dots */
                    gotoxy(sr, sc);
                    if (cur) printf(COL_CUR);
                    printf(DIM COL_PENCIL);
                    for (int d = 0; d < N; d++) {
                        if (g->pencil[r][c][d])
                            printf("%d", d+1);
                        else
                            printf("·");
                    }
                    printf(RESET);
                } else {
                    if (cur) printf(DIM "·" RESET);
                    else     printf(" ");
                }
            }
            if (cur) printf(RESET);
        }
    }
}

static void draw_title(void) {
    gotoxy(1, 4);
    printf(BOLD COL_TITLE "╔════════════════════════════════╗");
    gotoxy(2, 4);
    printf(           "║   ◆  T E R M I N A L  S U D O K U  ◆   ║");
    gotoxy(3, 4);
    printf(           "╚════════════════════════════════╝" RESET);
}

static void draw_status(Game *g) {
    struct timeval now;
    gettimeofday(&now, NULL);
    int sec = (int)(now.tv_sec - g->t0.tv_sec);
    int m = sec/60, s = sec%60;

    static const char *dname[] = {"Easy", "Medium", "Hard"};
    static const char *dcol[]  = {CSI "38;5;120m", CSI "38;5;214m", CSI "38;5;196m"};

    int row = GTOP + N + N/3 + 2;
    gotoxy(row, 3);
    printf(COL_STATUS "  Time: " BOLD "%02d:%02d" RESET, m, s);
    printf(COL_STATUS "  │  %s%s" RESET, dcol[g->difficulty], dname[g->difficulty]);
    printf(COL_STATUS "  │  %s" RESET, g->pencil_mode ? "Pencil ✎" : "Normal");

    int filled = 0;
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            filled += !!g->board[r][c];
    printf(COL_STATUS "  │  %d/%d" RESET, filled, N*N);
    if (g->hints_used)
        printf(COL_STATUS "  │  Hints: %d" RESET, g->hints_used);

    gotoxy(row+2, 3);
    printf(DIM " [hjkl/↑↓←→] Move   [1-9] Place   [0/Del] Clear" RESET);
    gotoxy(row+3, 3);
    printf(DIM " [p] Pencil   [H] Hint   [a] Auto-solve   [u] Undo" RESET);
    gotoxy(row+4, 3);
    printf(DIM " [n] New game   [d] Difficulty   [q] Quit" RESET);
}

static void draw_all(Game *g) {
    printf(CLR_SCR HIDE_CUR);
    draw_title();
    draw_grid_lines();
    draw_cells(g);
    draw_status(g);
    fflush(stdout);
}

/* ── Win celebration ───────────────────────────────────────────── */

static void celebrate(Game *g) {
    struct timeval now;
    gettimeofday(&now, NULL);
    int sec = (int)(now.tv_sec - g->t0.tv_sec);
    int m = sec/60, s = sec%60;
    int row = GTOP + 3;

    for (int flash = 0; flash < 4; flash++) {
        gotoxy(row, 6);
        printf(BOLD COL_WIN "  ╔════════════════════════════╗");
        gotoxy(row+1, 6);
        printf(           "  ║                            ║");
        gotoxy(row+2, 6);
        printf(           "  ║   ★  SOLVED!  %02d:%02d  ★    ║", m, s);
        gotoxy(row+3, 6);
        printf(           "  ║    Hints: %-3d             ║", g->hints_used);
        gotoxy(row+4, 6);
        printf(           "  ║                            ║");
        gotoxy(row+5, 6);
        printf(           "  ╚════════════════════════════╝" RESET);
        fflush(stdout);
        usleep(250000);

        for (int i = 0; i < 6; i++) {
            gotoxy(row+i, 6);
            printf("%*s", 32, "");
        }
        fflush(stdout);
        usleep(150000);
    }

    gotoxy(row, 6);
    printf(BOLD COL_WIN "  ╔════════════════════════════╗");
    gotoxy(row+1, 6);
    printf(           "  ║                            ║");
    gotoxy(row+2, 6);
    printf(           "  ║   ★  SOLVED!  %02d:%02d  ★    ║", m, s);
    gotoxy(row+3, 6);
    printf(           "  ║    Hints: %-3d             ║", g->hints_used);
    gotoxy(row+4, 6);
    printf(           "  ║                            ║");
    gotoxy(row+5, 6);
    printf(           "  ╚════════════════════════════╝" RESET);

    gotoxy(row+7, 8);
    printf(DIM " [n] New game   [q] Quit" RESET);
    fflush(stdout);
}

/* ── Game init ─────────────────────────────────────────────────── */

static void init_game(Game *g, int diff) {
    memset(g, 0, sizeof(*g));
    g->difficulty = diff;
    int clues[] = {EASY_CLUES, MEDIUM_CLUES, HARD_CLUES};

    /* Show generating message */
    printf(CLR_SCR "  Generating puzzle (difficulty: %s)...\n" RESET,
           (const char*[]){"Easy","Medium","Hard"}[diff]);
    fflush(stdout);

    generate(g->puzzle, clues[diff]);
    memcpy(g->board, g->puzzle, sizeof(g->board));
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            g->given[r][c] = g->puzzle[r][c] != 0;
    gettimeofday(&g->t0, NULL);
}

/* ── Main loop ─────────────────────────────────────────────────── */

int main(void) {
    srand((unsigned)time(NULL));

    Game game;
    init_game(&game, 0);  /* start Easy */
    set_raw();

    int running = 1;
    while (running) {
        draw_all(&game);

        int k = read_key();

        /* Movement */
        if (k=='h' || k=='L') { game.cur_c = (game.cur_c-1+N)%N; continue; }
        if (k=='l' || k=='R') { game.cur_c = (game.cur_c+1)%N;   continue; }
        if (k=='k' || k=='U') { game.cur_r = (game.cur_r-1+N)%N; continue; }
        if (k=='j' || k=='D') { game.cur_r = (game.cur_r+1)%N;   continue; }

        int r = game.cur_r, c = game.cur_c;

        /* Place number */
        if (k >= '1' && k <= '9' && !game.given[r][c]) {
            int d = k - '0';
            if (game.pencil_mode) {
                int di = d - 1;
                game.pencil[r][c][di] = !game.pencil[r][c][di];
                if (game.hist_len < 1024) {
                    Move *m = &game.history[game.hist_len++];
                    *m = (Move){r, c, game.pencil[r][c][di], 1, di};
                }
            } else {
                int old = game.board[r][c];
                game.board[r][c] = d;
                memset(game.pencil[r][c], 0, sizeof(game.pencil[r][c]));
                /* Clear pencil mark d from peers */
                for (int i = 0; i < N; i++) {
                    game.pencil[r][i][d-1] = 0;
                    game.pencil[i][c][d-1] = 0;
                }
                int br = r/BOX*BOX, bc = c/BOX*BOX;
                for (int i = br; i < br+BOX; i++)
                    for (int j = bc; j < bc+BOX; j++)
                        game.pencil[i][j][d-1] = 0;
                if (game.hist_len < 1024) {
                    Move *m = &game.history[game.hist_len++];
                    *m = (Move){r, c, old, 0, 0};
                }
            }
            if (!game.pencil_mode && is_complete(&game)) {
                game.solved = 1;
                draw_all(&game);
                celebrate(&game);
            }
            continue;
        }

        /* Clear cell */
        if ((k=='0' || k==127 || k==8) && !game.given[r][c]) {
            int old = game.board[r][c];
            game.board[r][c] = 0;
            memset(game.pencil[r][c], 0, sizeof(game.pencil[r][c]));
            if (game.hist_len < 1024) {
                Move *m = &game.history[game.hist_len++];
                *m = (Move){r, c, old, 0, 0};
            }
            continue;
        }

        /* Toggle pencil mode */
        if (k == 'p') { game.pencil_mode = !game.pencil_mode; continue; }

        /* Undo */
        if (k == 'u' && game.hist_len > 0) {
            Move *m = &game.history[--game.hist_len];
            if (m->is_pencil)
                game.pencil[m->row][m->col][m->pencil_digit] = !m->prev_val;
            else
                game.board[m->row][m->col] = m->prev_val;
            game.cur_r = m->row; game.cur_c = m->col;
            continue;
        }

        /* Hint */
        if (k == 'H') {
            int empty[N*N][2], cnt = 0;
            for (int i = 0; i < N*N; i++)
                if (!game.board[i/N][i%N]) {
                    empty[cnt][0] = i/N;
                    empty[cnt][1] = i%N;
                    cnt++;
                }
            if (cnt > 0) {
                int cp[N][N];
                memcpy(cp, game.board, sizeof(cp));
                solve(cp);
                int idx = rand() % cnt;
                int hr = empty[idx][0], hc = empty[idx][1];
                game.board[hr][hc] = cp[hr][hc];
                game.given[hr][hc] = 1;
                memset(game.pencil[hr][hc], 0, sizeof(game.pencil[hr][hc]));
                game.hints_used++;
                game.cur_r = hr; game.cur_c = hc;
                if (is_complete(&game)) {
                    game.solved = 1;
                    draw_all(&game);
                    celebrate(&game);
                }
            }
            continue;
        }

        /* Auto-solve animated */
        if (k == 'a' && !game.solved) {
            /* First show the empty grid */
            draw_grid_lines();
            for (int r2 = 0; r2 < N; r2++)
                for (int c2 = 0; c2 < N; c2++) {
                    if (!game.given[r2][c2]) {
                        gotoxy(cell_row(r2), cell_col(c2));
                        printf(DIM "·" RESET);
                    }
                }
            fflush(stdout);

            int cp[N][N];
            memcpy(cp, game.board, sizeof(cp));
            if (solve_anim(cp, 3000)) {
                memcpy(game.board, cp, sizeof(cp));
                for (int i = 0; i < N; i++)
                    for (int j = 0; j < N; j++)
                        memset(game.pencil[i][j], 0, sizeof(game.pencil[i][j]));
                game.solved = 1;
                draw_all(&game);
                celebrate(&game);
            }
            continue;
        }

        /* New game */
        if (k == 'n') {
            restore_term();
            init_game(&game, game.difficulty);
            set_raw();
            continue;
        }

        /* Cycle difficulty */
        if (k == 'd') {
            game.difficulty = (game.difficulty + 1) % 3;
            restore_term();
            init_game(&game, game.difficulty);
            set_raw();
            continue;
        }

        /* Redraw */
        if (k == 'r' || k == 12) continue;

        /* Quit */
        if (k == 'q' || k == 27) running = 0;
    }

    restore_term();
    printf(CLR_SCR);
    return 0;
}
