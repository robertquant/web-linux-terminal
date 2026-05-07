/*
 * ============================================================================
 * Connect Four — Terminal Connect Four Game with AI Opponent
 * ============================================================================
 *
 * Classic Connect Four: drop coloured discs into a 7-column, 6-row grid.
 * First player to connect 4 in a row (horizontal / vertical / diagonal) wins.
 *
 * Features
 *   - Player vs AI  (Easy / Medium / Hard)
 *   - Player vs Player
 *   - Minimax AI with alpha-beta pruning + centre-preference heuristic
 *   - Animated disc drop
 *   - Winning-line highlight
 *   - Undo, score tracking, restart
 *
 * Compile
 *   gcc -std=c11 -O2 -o connect4 connect4-6475.c -lncurses
 *
 * Run
 *   ./connect4
 *
 * Controls (in-game)
 *   ← →          Select column
 *   Enter / Space Drop piece
 *   u             Undo last move (2 moves in PvAI)
 *   r             Restart
 *   q             Quit to menu
 *
 * Requires: ncurses (curses.h, libncurses)
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <ncurses.h>

/* ── constants ──────────────────────────────────────────────────────────── */

#define ROWS         6
#define COLS         7
#define EMPTY        0
#define P1           1          /* human  – red   */
#define P2           2          /* AI / P2 – yellow */
#define CONNECT      4
#define MAX_MOVES    (ROWS*COLS)
#define AI_EASY      2
#define AI_MEDIUM    5
#define AI_HARD      8

/* colour-pair ids */
enum { C_BOARD=1, C_P1, C_P2, C_CURSOR, C_TITLE, C_MENU,
       C_WIN_P1, C_WIN_P2, C_DIM };

/* ── game state ─────────────────────────────────────────────────────────── */

static int  board[ROWS][COLS];
static int  cur_player;
static int  sel_col;
static int  game_over;
static int  winner;                  /* 0=draw, P1, P2 */
static int  win_cells[CONNECT][2];   /* (row,col) of winning line */
static int  hist[MAX_MOVES];         /* column history */
static int  hcnt;                    /* history count */
static int  scores[3];               /* [draws, p1, p2] */
static int  mode;                    /* 1 = PvAI, 2 = PvP */
static int  ai_depth;
static int  anim_ms = 30;            /* drop-animation delay */

/* ── ncurses helpers ────────────────────────────────────────────────────── */

static void init_colours(void)
{
    start_color();
    init_pair(C_BOARD,  COLOR_WHITE,  COLOR_BLUE);
    init_pair(C_P1,     COLOR_WHITE,  COLOR_RED);
    init_pair(C_P2,     COLOR_BLACK,  COLOR_YELLOW);
    init_pair(C_CURSOR, COLOR_GREEN,  COLOR_BLACK);
    init_pair(C_TITLE,  COLOR_CYAN,   COLOR_BLACK);
    init_pair(C_MENU,   COLOR_WHITE,  COLOR_BLACK);
    init_pair(C_WIN_P1, COLOR_RED,    COLOR_WHITE);
    init_pair(C_WIN_P2, COLOR_YELLOW, COLOR_WHITE);
    init_pair(C_DIM,    COLOR_BLACK,  COLOR_BLACK);
}

/* draw a single cell character with the right colour pair */
static void draw_cell(int r, int c, int highlight)
{
    int piece = board[r][c];
    int is_win = 0;
    for (int i = 0; i < CONNECT; i++)
        if (win_cells[i][0] == r && win_cells[i][1] == c) { is_win = 1; break; }

    if (highlight && piece != EMPTY) {
        attrset(COLOR_PAIR(piece == P1 ? C_WIN_P1 : C_WIN_P2) | A_BOLD);
    } else if (piece == P1) {
        attrset(COLOR_PAIR(C_P1) | A_BOLD);
    } else if (piece == P2) {
        attrset(COLOR_PAIR(C_P2) | A_BOLD);
    } else {
        attrset(COLOR_PAIR(C_BOARD));
    }
    addstr(" O ");
    attrset(A_NORMAL);
}

/* ── board logic ────────────────────────────────────────────────────────── */

static void board_reset(void)
{
    memset(board, 0, sizeof board);
    cur_player = P1;
    sel_col = COLS / 2;
    game_over = 0;
    winner = 0;
    memset(win_cells, 0, sizeof win_cells);
    hcnt = 0;
}

/* return the lowest empty row in col, or -1 */
static int find_row(int col)
{
    for (int r = ROWS - 1; r >= 0; r--)
        if (board[r][col] == EMPTY) return r;
    return -1;
}

/* place piece; return the row it landed on, or -1 */
static int drop(int col, int player)
{
    int r = find_row(col);
    if (r < 0) return -1;
    board[r][col] = player;
    hist[hcnt++] = col;
    return r;
}

/* undo last move; return column undone, or -1 */
static int undo_drop(void)
{
    if (hcnt == 0) return -1;
    int col = hist[--hcnt];
    int r = find_row(col);           /* first empty = one above last piece */
    /* actually the piece is at r+1 if r>=0, or at row 0 if r==-1 */
    int row = (r >= 0) ? r + 1 : 0;
    board[row][col] = EMPTY;
    return col;
}

/* check whether `player` has won; if so fill win_cells */
static int check_winner(int player)
{
    static const int dr[] = { 0, 1, 1,  1 };
    static const int dc[] = { 1, 0, 1, -1 };
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if (board[r][c] != player) continue;
            for (int d = 0; d < 4; d++) {
                int ok = 1;
                for (int k = 0; k < CONNECT; k++) {
                    int nr = r + dr[d]*k, nc = c + dc[d]*k;
                    if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS ||
                        board[nr][nc] != player) { ok = 0; break; }
                }
                if (ok) {
                    for (int k = 0; k < CONNECT; k++) {
                        win_cells[k][0] = r + dr[d]*k;
                        win_cells[k][1] = c + dc[d]*k;
                    }
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int is_full(void)
{
    for (int c = 0; c < COLS; c++)
        if (board[0][c] == EMPTY) return 0;
    return 1;
}

/* ── AI ─────────────────────────────────────────────────────────────────── */

/* Evaluate a window of 4 cells for `player` (positive = good for player) */
static int eval_window(int w[4], int player)
{
    int opp = 3 - player;
    int pc = 0, oc = 0, ec = 0;
    for (int i = 0; i < 4; i++) {
        if      (w[i] == player) pc++;
        else if (w[i] == opp)    oc++;
        else                     ec++;
    }
    if (pc == 4) return 10000;
    if (pc == 3 && ec == 1) return 50;
    if (pc == 2 && ec == 2) return 10;
    if (oc == 3 && ec == 1) return -80;   /* block opponent */
    if (oc == 4) return -10000;
    return 0;
}

/* Static evaluation: sum of all 4-cell windows + centre preference */
static int evaluate(int player)
{
    int score = 0;
    int opp = 3 - player;
    /* centre column bonus */
    for (int r = 0; r < ROWS; r++)
        if (board[r][COLS/2] == player) score += 6;

    int w[4];
    /* horizontal */
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c <= COLS-4; c++) {
            for (int k = 0; k < 4; k++) w[k] = board[r][c+k];
            score += eval_window(w, player);
        }
    /* vertical */
    for (int c = 0; c < COLS; c++)
        for (int r = 0; r <= ROWS-4; r++) {
            for (int k = 0; k < 4; k++) w[k] = board[r+k][c];
            score += eval_window(w, player);
        }
    /* diagonal ↘ */
    for (int r = 0; r <= ROWS-4; r++)
        for (int c = 0; c <= COLS-4; c++) {
            for (int k = 0; k < 4; k++) w[k] = board[r+k][c+k];
            score += eval_window(w, player);
        }
    /* diagonal ↗ */
    for (int r = 3; r < ROWS; r++)
        for (int c = 0; c <= COLS-4; c++) {
            for (int k = 0; k < 4; k++) w[k] = board[r-k][c+k];
            score += eval_window(w, player);
        }
    return score;
}

/* Minimax with alpha-beta.  Returns score from `player`'s perspective. */
static int minimax(int depth, int alpha, int beta, int maximising, int ai_player)
{
    int opp = 3 - ai_player;

    /* terminal checks */
    if (check_winner(ai_player))  return  100000 + depth;
    if (check_winner(opp))        return -100000 - depth;
    if (is_full())                return 0;
    if (depth == 0)               return evaluate(ai_player);

    /* order columns: centre first for better pruning */
    int order[COLS];
    int idx = 0;
    for (int d = 0; d <= COLS/2; d++) {
        int c = COLS/2 + d;
        if (c < COLS) order[idx++] = c;
        c = COLS/2 - d - 1;
        if (c >= 0)   order[idx++] = c;
    }

    if (maximising) {
        int best = INT_MIN;
        for (int i = 0; i < COLS; i++) {
            int c = order[i];
            if (find_row(c) < 0) continue;
            drop(c, ai_player);
            int val = minimax(depth-1, alpha, beta, 0, ai_player);
            undo_drop();
            if (val > best) best = val;
            if (best > alpha) alpha = best;
            if (alpha >= beta) break;
        }
        return best;
    } else {
        int best = INT_MAX;
        for (int i = 0; i < COLS; i++) {
            int c = order[i];
            if (find_row(c) < 0) continue;
            drop(c, opp);
            int val = minimax(depth-1, alpha, beta, 1, ai_player);
            undo_drop();
            if (val < best) best = val;
            if (best < beta) beta = best;
            if (alpha >= beta) break;
        }
        return best;
    }
}

static int ai_choose(void)
{
    int best_col = -1, best_score = INT_MIN;
    /* centre-first ordering */
    int order[COLS]; int idx = 0;
    for (int d = 0; d <= COLS/2; d++) {
        int c = COLS/2 + d; if (c < COLS) order[idx++] = c;
        c = COLS/2 - d - 1; if (c >= 0)   order[idx++] = c;
    }
    for (int i = 0; i < COLS; i++) {
        int c = order[i];
        if (find_row(c) < 0) continue;
        drop(c, P2);
        int s = minimax(ai_depth - 1, INT_MIN, INT_MAX, 0, P2);
        undo_drop();
        /* tiny random tie-break so AI isn't completely deterministic */
        if (s > best_score || (s == best_score && rand() % 2)) {
            best_score = s;
            best_col = c;
        }
    }
    return best_col;
}

/* ── drawing ────────────────────────────────────────────────────────────── */

static void draw_board(int anim_r, int anim_c, int anim_p)
{
    int maxy, maxx;
    getmaxyx(stdscr, maxy, maxx);

    int bx = (maxx - COLS*3 - 2) / 2;      /* board left edge */
    int by = (maxy - ROWS*2 - 6) / 2;       /* board top edge */
    if (bx < 0) bx = 0;
    if (by < 2) by = 2;

    /* title */
    attrset(COLOR_PAIR(C_TITLE) | A_BOLD);
    mvprintw(by - 2, bx, "CONNECT FOUR");

    /* cursor indicator above board */
    if (!game_over) {
        int cx = bx + 1 + sel_col * 3;
        attrset(COLOR_PAIR(cur_player == P1 ? C_P1 : C_P2) | A_BOLD);
        mvaddch(by, cx, 'V');
    }

    /* board frame */
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int y = by + 1 + r * 2;
            int x = bx + c * 3;
            if (r == anim_r && c == anim_c) {
                /* animated piece in flight */
                attrset(COLOR_PAIR(anim_p == P1 ? C_P1 : C_P2) | A_BOLD);
                mvaddstr(y, x, " O ");
            } else {
                int hl = (game_over && winner != 0);
                move(y, x);
                draw_cell(r, c, hl);
            }
        }
    }

    /* column numbers */
    attrset(COLOR_PAIR(C_MENU));
    for (int c = 0; c < COLS; c++)
        mvprintw(by + 1 + ROWS*2, bx + 1 + c*3, "%d", c+1);

    /* status line */
    int sy = by + ROWS*2 + 4;
    attrset(COLOR_PAIR(C_MENU));
    if (game_over) {
        if (winner == 0) {
            attrset(COLOR_PAIR(C_TITLE) | A_BOLD);
            mvprintw(sy, bx, "It's a draw!");
        } else {
            attrset(COLOR_PAIR(winner == P1 ? C_P1 : C_P2) | A_BOLD);
            mvprintw(sy, bx, "%s wins!",
                     (mode == 1 && winner == P2) ? "AI" :
                     (winner == P1) ? "Red" : "Yellow");
        }
        attrset(COLOR_PAIR(C_MENU));
        mvprintw(sy+1, bx, "Press 'r' to play again, 'q' for menu");
    } else {
        attrset(COLOR_PAIR(cur_player == P1 ? C_P1 : C_P2) | A_BOLD);
        if (mode == 1 && cur_player == P2)
            mvprintw(sy, bx, "AI is thinking...");
        else
            mvprintw(sy, bx, "%s's turn",
                     cur_player == P1 ? "Red" : "Yellow");
    }

    /* scores */
    attrset(COLOR_PAIR(C_MENU));
    mvprintw(sy+2, bx, "Score  Red: %d  Yellow: %d  Draw: %d",
             scores[P1], scores[P2], scores[0]);

    /* controls */
    attrset(A_DIM);
    mvprintw(sy+3, bx, "[←→] move  [Space] drop  [u]ndo  [r]estart  [q]uit");

    attrset(A_NORMAL);
}

/* animate a piece falling into column `col` for `player` */
static void animate_drop(int col, int player)
{
    int target = find_row(col);
    if (target < 0) return;
    /* temporarily remove the piece we already placed */
    board[target][col] = EMPTY;
    for (int r = 0; r <= target; r++) {
        erase();
        draw_board(r, col, player);
        refresh();
        napms(anim_ms);
    }
    board[target][col] = player;
}

/* ── screens ────────────────────────────────────────────────────────────── */

static void show_splash(void)
{
    erase();
    int my, mx; getmaxyx(stdscr, my, mx);
    const char *lines[] = {
        "   ██████╗ ███╗   ██╗███████╗████████╗███████╗███████╗",
        "  ██╔════╝ ████╗  ██║██╔════╝╚══██╔══╝██╔════╝██╔════╝",
        "  ██║  ███╗██╔██╗ ██║█████╗     ██║   █████╗  ███████╗",
        "  ██║   ██║██║╚██╗██║██╔══╝     ██║   ██╔══╝  ╚════██║",
        "  ╚██████╔╝██║ ╚████║███████╗   ██║   ███████╗███████║",
        "   ╚═════╝ ╚═╝  ╚═══╝╚══════╝   ╚═╝   ╚══════╝╚══════╝",
        "",
        "     ███╗   ██╗███████╗███████╗████████╗   ██████╗ ████████╗██╗  ██╗███████╗",
        "     ████╗  ██║██╔════╝██╔════╝╚══██╔══╝   ██╔═══██╗╚══██╔══╝██║  ██║██╔════╝",
        "     ██╔██╗ ██║█████╗  ███████╗   ██║█████╗██║   ██║   ██║   ███████║█████╗  ",
        "     ██║╚██╗██║██╔══╝  ╚════██║   ██║╚════╝██║   ██║   ██║   ██╔══██║██╔══╝  ",
        "     ██║ ╚████║███████╗███████║   ██║      ╚██████╔╝   ██║   ██║  ██║███████╗",
        "     ╚═╝  ╚═══╝╚══════╝╚══════╝   ╚═╝       ╚═════╝    ╚═╝   ╚═╝  ╚═╝╚══════╝",
    };
    int n = sizeof(lines)/sizeof(lines[0]);
    int top = (my - n - 6) / 2;
    if (top < 0) top = 0;
    int left = (mx - 60) / 2;
    if (left < 0) left = 0;

    attrset(COLOR_PAIR(C_TITLE) | A_BOLD);
    for (int i = 0; i < n; i++)
        mvprintw(top + i, left, "%s", lines[i]);

    attrset(COLOR_PAIR(C_MENU));
    mvprintw(top + n + 2, left + 10, "Press any key to continue...");
    refresh();
    nodelay(stdscr, FALSE);
    getch();
}

static int menu_choose(void)
{
    static const char *opts[] = {
        "1.  Player vs AI  — Easy",
        "2.  Player vs AI  — Medium",
        "3.  Player vs AI  — Hard",
        "4.  Player vs Player",
        "5.  Quit",
    };
    int n = sizeof(opts)/sizeof(opts[0]);
    int sel = 0;

    while (1) {
        erase();
        int my, mx; getmaxyx(stdscr, my, mx);
        int top = (my - n - 4) / 2;
        int left = (mx - 40) / 2;

        attrset(COLOR_PAIR(C_TITLE) | A_BOLD);
        mvprintw(top, left, "=== CONNECT FOUR ===");
        attrset(COLOR_PAIR(C_MENU));
        mvprintw(top+1, left, "Select game mode:");

        for (int i = 0; i < n; i++) {
            if (i == sel) {
                attrset(COLOR_PAIR(C_CURSOR) | A_BOLD);
                mvprintw(top+3+i, left, "  > %s", opts[i]);
            } else {
                attrset(COLOR_PAIR(C_MENU));
                mvprintw(top+3+i, left, "    %s", opts[i]);
            }
        }
        attrset(A_DIM);
        mvprintw(top+n+4, left, "↑↓  select    Enter  confirm");
        attrset(A_NORMAL);
        refresh();

        int ch = getch();
        switch (ch) {
            case KEY_UP:   sel = (sel - 1 + n) % n; break;
            case KEY_DOWN: sel = (sel + 1) % n;      break;
            case '\n': case ' ':
                if (sel == 0) { mode=1; ai_depth=AI_EASY;   return 1; }
                if (sel == 1) { mode=1; ai_depth=AI_MEDIUM; return 1; }
                if (sel == 2) { mode=1; ai_depth=AI_HARD;   return 1; }
                if (sel == 3) { mode=2;                     return 1; }
                return 0; /* quit */
        }
    }
}

/* ── game loop ──────────────────────────────────────────────────────────── */

static void game_loop(void)
{
    board_reset();

    while (!game_over) {
        erase();
        draw_board(-1, -1, 0);
        refresh();

        /* AI turn */
        if (mode == 1 && cur_player == P2) {
            napms(200);              /* small pause so it feels "natural" */
            int col = ai_choose();
            if (col < 0) { game_over = 1; break; }
            drop(col, P2);
            animate_drop(col, P2);
            if (check_winner(P2)) { winner = P2; game_over = 1; }
            else if (is_full())    { winner = 0;  game_over = 1; }
            else cur_player = P1;
            continue;
        }

        /* human turn */
        int ch = getch();
        switch (ch) {
        case KEY_LEFT:
            sel_col = (sel_col - 1 + COLS) % COLS;
            break;
        case KEY_RIGHT:
            sel_col = (sel_col + 1) % COLS;
            break;
        case ' ': case '\n': {
            if (find_row(sel_col) < 0) break;
            int col = sel_col;
            drop(col, cur_player);
            animate_drop(col, cur_player);
            if (check_winner(cur_player)) { winner = cur_player; game_over = 1; }
            else if (is_full())           { winner = 0;          game_over = 1; }
            else cur_player = 3 - cur_player;
            break;
        }
        case 'u': case 'U': {
            /* undo: in PvAI undo both AI + player move */
            int count = (mode == 1 && hcnt >= 2) ? 2 : 1;
            for (int i = 0; i < count && hcnt > 0; i++) {
                int uc = undo_drop();
                if (uc >= 0) sel_col = uc;
            }
            if (mode == 1) cur_player = P1;
            else           cur_player = 3 - cur_player;
            break;
        }
        case 'r':
            board_reset();
            break;
        case 'q':
            return;
        }
    }

    /* final screen */
    scores[winner]++;
    erase();
    draw_board(-1, -1, 0);
    refresh();

    /* wait for restart or quit */
    while (1) {
        int ch = getch();
        if (ch == 'r') { game_loop(); return; }
        if (ch == 'q') return;
    }
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    srand((unsigned)time(NULL));

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    nodelay(stdscr, FALSE);
    init_colours();

    show_splash();

    while (menu_choose())
        game_loop();

    endwin();
    return 0;
}
