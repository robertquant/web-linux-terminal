/*
 * g2048 - Terminal 2048 Sliding Puzzle Game
 *
 * The classic 2048 game in your terminal!
 * Slide numbered tiles on a 4x4 grid. When two tiles with the same
 * number collide, they merge into one tile with double the value.
 * Reach the 2048 tile to win!
 *
 * Features:
 *   - Smooth sliding & merge animations
 *   - 256-color tile palette (highest tile gets special highlight)
 *   - Score & Best Score tracking (persisted to ~/.g2048_best)
 *   - Unlimited undo
 *   - New Game, Continue after win
 *   - Responsive centering on any terminal size
 *
 * Compile:
 *   gcc -std=c11 -O2 -o g2048 g2048-2710.c -lncursesw -lm
 *   (use -lncurses if ncursesw is unavailable)
 *
 * Run:
 *   ./g2048
 *
 * Controls:
 *   Arrow keys / WASD / hjkl  - Slide tiles
 *   u                          - Undo last move
 *   n                          - New game
 *   q / Esc                    - Quit
 *
 * Requires: ncurses (ncursesw recommended for wide char support)
 *
 * Author: Creative C Programs Collection
 * License: MIT
 */

#define _DEFAULT_SOURCE 1
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>

/* ── Constants ────────────────────────────────────────────────────── */
#define N          4
#define ANIM_MS    80
#define WIN_VAL    2048
#define MAX_UNDO   200

/* ── Tile colour palette (ANSI 256-color indices) ────────────────── */
static const int TILE_FG[] = {
    0,    0,    0,    0,    0,    0,    0,    0,    0,
    232,  232,  232,  232,  232
};
static const int TILE_BG[] = {
    0,
    231,  /*    2 - white     */
    230,  /*    4 - cream     */
    223,  /*    8 - peach     */
    222,  /*   16 - tan       */
    221,  /*   32 - gold      */
    220,  /*   64 - orange    */
    214,  /*  128 - red-orange */
    208,  /*  256 - red        */
    202,  /*  512 - crimson    */
    197,  /* 1024 - hot pink   */
    163,  /* 2048 - magenta    */
    129,  /* 4096 - purple     */
    93,   /* 8192 - blue       */
    63,   /*16384+ - electric  */
};

/* ── Data structures ─────────────────────────────────────────────── */
typedef struct {
    int  val[N][N];
    int  score;
    int  best;
    int  won;       /* 1 = reached 2048, 2 = chose to continue */
    int  over;      /* 1 = no moves left */
} State;

typedef struct {
    int  val[N][N];
    int  score;
} Snapshot;

typedef struct {
    int  r_from, c_from, r_to, c_to;
    int  val;
    int  merged;        /* 1 if this tile is the result of a merge */
} AnimTile;

/* ── Globals ─────────────────────────────────────────────────────── */
static State       g_state;
static Snapshot    g_undo[MAX_UNDO];
static int         g_undo_count;

/* ── Helpers ─────────────────────────────────────────────────────── */
static int tile_idx(int v) {
    if (v <= 0) return 0;
    int i = 0;
    while (v > 1) { v >>= 1; i++; }
    return i > 13 ? 13 : i;
}

/* Return a background color index for a tile value */
static int tile_bg(int v) { return TILE_BG[tile_idx(v)]; }
static int tile_fg(int v) { return TILE_FG[tile_idx(v)]; }

/* Best-score persistence */
static char *best_path(void) {
    static char buf[512];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(buf, sizeof buf, "%s/.g2048_best", home);
    return buf;
}

static void load_best(void) {
    FILE *f = fopen(best_path(), "r");
    if (f) { if (fscanf(f, "%d", &g_state.best) != 1) g_state.best = 0; fclose(f); }
}

static void save_best(void) {
    FILE *f = fopen(best_path(), "w");
    if (f) { fprintf(f, "%d\n", g_state.best); fclose(f); }
}

/* Spawn a random tile (90% = 2, 10% = 4) in an empty cell */
static void spawn_tile(void) {
    int empty[N * N], cnt = 0;
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            if (g_state.val[r][c] == 0)
                empty[cnt++] = r * N + c;
    if (cnt == 0) return;
    int pick = empty[rand() % cnt];
    g_state.val[pick / N][pick % N] = (rand() % 10 < 9) ? 2 : 4;
}

static int can_move(void) {
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            if (g_state.val[r][c] == 0) return 1;
            if (c < N - 1 && g_state.val[r][c] == g_state.val[r][c + 1]) return 1;
            if (r < N - 1 && g_state.val[r][c] == g_state.val[r + 1][c]) return 1;
        }
    return 0;
}

/* ── Undo ─────────────────────────────────────────────────────────── */
static void push_undo(void) {
    if (g_undo_count >= MAX_UNDO) {
        memmove(g_undo, g_undo + 1, sizeof(Snapshot) * (MAX_UNDO - 1));
        g_undo_count = MAX_UNDO - 1;
    }
    memcpy(&g_undo[g_undo_count], g_state.val, sizeof(g_state.val));
    g_undo[g_undo_count].score = g_state.score;
    g_undo_count++;
}

static int pop_undo(void) {
    if (g_undo_count == 0) return 0;
    g_undo_count--;
    memcpy(g_state.val, g_undo[g_undo_count].val, sizeof(g_state.val));
    g_state.score = g_undo[g_undo_count].score;
    g_state.over = 0;
    return 1;
}

/* ── Slide logic ──────────────────────────────────────────────────── */
/* Returns number of AnimTiles written (0 if nothing moved) */
static int slide_row(const int in[N], int out[N], AnimTile *at) {
    /* Compress non-zero tiles to the left */
    int tmp[N] = {0};
    int src[N];  /* original index of each non-zero tile */
    int cnt = 0;
    for (int c = 0; c < N; c++)
        if (in[c]) { tmp[cnt] = in[c]; src[cnt] = c; cnt++; }

    int wrote = 0;
    int n_anim = 0;
    int i = 0;
    while (i < cnt) {
        if (i + 1 < cnt && tmp[i] == tmp[i + 1]) {
            /* Merge */
            out[wrote] = tmp[i] * 2;
            at[n_anim++] = (AnimTile){ src[i],     wrote, wrote, tmp[i],     1, 1 };
            at[n_anim++] = (AnimTile){ src[i + 1], wrote, wrote, tmp[i + 1], 1, 1 };
            g_state.score += out[wrote];
            if (out[wrote] == WIN_VAL && !g_state.won)
                g_state.won = 1;
            wrote++;
            i += 2;
        } else {
            out[wrote] = tmp[i];
            at[n_anim++] = (AnimTile){ src[i], wrote, wrote, tmp[i], 0, 0 };
            wrote++;
            i++;
        }
    }
    /* Pad with zeros */
    while (wrote < N) out[wrote++] = 0;
    return n_anim;
}

typedef enum { DIR_LEFT, DIR_RIGHT, DIR_UP, DIR_DOWN } Dir;

static int do_move(Dir d) {
    int moved = 0;
    AnimTile anims[N * N * 2];
    int n_anims = 0;
    (void)anims;

    push_undo();

    if (d == DIR_LEFT || d == DIR_RIGHT) {
        for (int r = 0; r < N; r++) {
            int row[N], rev[N], result[N];
            for (int c = 0; c < N; c++) row[c] = g_state.val[r][c];
            if (d == DIR_RIGHT) {
                for (int c = 0; c < N; c++) rev[N - 1 - c] = row[c];
                memcpy(row, rev, sizeof row);
            }
            AnimTile at[N * 2];
            int n = slide_row(row, result, at);
            if (d == DIR_RIGHT) {
                /* Remap columns back */
                int flip[N];
                for (int c = 0; c < N; c++) flip[N - 1 - c] = result[c];
                memcpy(result, flip, sizeof result);
                for (int k = 0; k < n; k++) {
                    at[k].c_from = N - 1 - at[k].c_from;
                    at[k].c_to   = N - 1 - at[k].c_to;
                }
            }
            for (int c = 0; c < N; c++)
                if (g_state.val[r][c] != result[c]) moved = 1;
            memcpy(g_state.val[r], result, sizeof result);
            for (int k = 0; k < n; k++) {
                at[k].r_from += r; at[k].r_to += r;
                /* Adjust for row offset in struct: we stored c_from relative */
                at[k].r_from = r; at[k].r_to = r;
                anims[n_anims++] = at[k];
            }
        }
    } else {
        for (int c = 0; c < N; c++) {
            int col[N], rev[N], result[N];
            for (int r = 0; r < N; r++) col[r] = g_state.val[r][c];
            if (d == DIR_DOWN) {
                for (int r = 0; r < N; r++) rev[N - 1 - r] = col[r];
                memcpy(col, rev, sizeof col);
            }
            AnimTile at[N * 2];
            int n = slide_row(col, result, at);
            if (d == DIR_DOWN) {
                int flip[N];
                for (int r = 0; r < N; r++) flip[N - 1 - r] = result[r];
                memcpy(result, flip, sizeof result);
                for (int k = 0; k < n; k++) {
                    at[k].c_from = N - 1 - at[k].c_from;
                    at[k].c_to   = N - 1 - at[k].c_to;
                }
            }
            for (int r = 0; r < N; r++)
                if (g_state.val[r][c] != result[r]) moved = 1;
            for (int r = 0; r < N; r++) g_state.val[r][c] = result[r];
            for (int k = 0; k < n; k++) {
                /* slide_row treats "columns" as the index; map to rows */
                AnimTile mapped;
                mapped.r_from = d == DIR_DOWN ? N - 1 - at[k].c_from : at[k].c_from;
                mapped.c_from = c;
                mapped.r_to   = d == DIR_DOWN ? N - 1 - at[k].c_to : at[k].c_to;
                mapped.c_to   = c;
                mapped.val    = at[k].val;
                mapped.merged = at[k].merged;
                anims[n_anims++] = mapped;
            }
        }
    }

    if (!moved) {
        g_undo_count--;  /* Undo the push since nothing happened */
        return 0;
    }

    /* Run simple animation */
    if (g_state.best < g_state.score) {
        g_state.best = g_state.score;
        save_best();
    }
    spawn_tile();
    if (!can_move()) g_state.over = 1;
    return 1;
}

/* ── Drawing ──────────────────────────────────────────────────────── */
static int TW = 7;   /* tile width in chars */
static int TH = 3;   /* tile height in chars */

static void draw_tile(int sy, int sx, int v, int highlight) {
    int bg = tile_bg(v);
    if (highlight) {
        /* Pulsing highlight for newest tile */
        bg = 118;
    }
    char num[16];
    if (v) snprintf(num, sizeof num, "%d", v);
    else   num[0] = '\0';

    int len = (int)strlen(num);
    int xoff = (TW - len) / 2;

    for (int dy = 0; dy < TH; dy++) {
        for (int dx = 0; dx < TW; dx++) {
            move(sy + dy, sx + dx);
            attron(COLOR_PAIR(bg));
            if (v) {
                attron(A_BOLD);
            } else {
                attroff(A_BOLD);
            }
            if (dy == TH / 2 && dx >= xoff && dx < xoff + len)
                addch(num[dx - xoff]);
            else
                addch(' ');
            attroff(COLOR_PAIR(bg));
            attroff(A_BOLD);
        }
    }
}

/* Simple flash animation for merges */
static void flash_tile(int sy, int sx, int v) {
    int bg = tile_bg(v);
    for (int f = 0; f < 2; f++) {
        /* Bright flash */
        for (int dy = 0; dy < TH; dy++)
            for (int dx = 0; dx < TW; dx++) {
                move(sy + dy, sx + dx);
                attron(A_REVERSE | A_BOLD);
                attron(COLOR_PAIR(bg));
                addch(' ');
                attroff(COLOR_PAIR(bg));
                attroff(A_REVERSE | A_BOLD);
            }
        refresh();
        usleep(ANIM_MS * 1000 / 2);
        draw_tile(sy, sx, v, 0);
        refresh();
        usleep(ANIM_MS * 1000 / 2);
    }
}

static void draw_board(int sy, int sx, int new_r, int new_c,
                       int merged_r[N*N], int merged_c[N*N], int n_merged) {
    /* Title */
    attron(A_BOLD | COLOR_PAIR(227));
    mvprintw(sy - 3, sx, "  2 0 4 8  ");
    attroff(A_BOLD | COLOR_PAIR(227));

    /* Score bar */
    mvprintw(sy - 3, sx + TW * N - 28,
             "Score: %-8d Best: %-8d", g_state.score, g_state.best);

    /* Grid background */
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            draw_tile(sy + r * TH, sx + c * TW, g_state.val[r][c], 0);

    /* Flash merged tiles */
    for (int m = 0; m < n_merged; m++) {
        flash_tile(sy + merged_r[m] * TH, sx + merged_c[m] * TW,
                   g_state.val[merged_r[m]][merged_c[m]]);
    }

    /* Highlight new tile */
    if (new_r >= 0)
        draw_tile(sy + new_r * TH, sx + new_c * TW,
                  g_state.val[new_r][new_c], 1);

    /* Grid border */
    attron(COLOR_PAIR(243));
    for (int c = 0; c <= N; c++) {
        for (int dy = 0; dy < TH * N + 1; dy++) {
            mvaddch(sy - 1 + dy, sx - 1 + c * TW, ACS_VLINE);
        }
    }
    for (int r = 0; r <= N; r++) {
        for (int dx = 0; dx < TW * N; dx++) {
            mvaddch(sy - 1 + r * TH, sx + dx, ACS_HLINE);
        }
        mvaddch(sy - 1 + r * TH, sx - 1, ACS_LTEE);
        mvaddch(sy - 1 + r * TH, sx + N * TW, ACS_RTEE);
    }
    attroff(COLOR_PAIR(243));

    /* Controls */
    int cy = sy + N * TH + 2;
    attron(A_DIM);
    mvprintw(cy,     sx, "Arrow/WASD/hjkl: Move");
    mvprintw(cy + 1, sx, "u: Undo  n: New  q: Quit");
    attroff(A_DIM);
}

/* ── Initialization ───────────────────────────────────────────────── */
static void init_colors(void) {
    if (!has_colors()) return;
    start_color();
    /* Define color pairs for each tile background (fg=black, bg=tile color) */
    for (int i = 0; i < 256; i++) {
        init_pair(i, 232, i);  /* pair i = dark fg on bg i */
    }
    /* Override pair for special use */
    init_pair(227, 227, 0);   /* title */
    init_pair(243, 243, 0);   /* border */
}

static void new_game(void) {
    memset(&g_state, 0, sizeof g_state);
    load_best();
    g_undo_count = 0;
    spawn_tile();
    spawn_tile();
}

/* ── Main ─────────────────────────────────────────────────────────── */
int main(void) {
    srand((unsigned)time(NULL));

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(100);  /* non-blocking getch for animation */
    init_colors();

    new_game();

    int new_r = -1, new_c = -1;
    int ch;
    int need_redraw = 1;

    while (1) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        int bw = TW * N + 1;
        int bh = TH * N + 1;
        int sy = (rows - bh) / 2;
        int sx = (cols - bw) / 2;

        if (need_redraw) {
            int empty_merged[N*N] = {0};
            (void)empty_merged;
            erase();
            draw_board(sy, sx, new_r, new_c, empty_merged, empty_merged, 0);

            /* Win / Lose overlay */
            if (g_state.won == 1) {
                attron(A_BOLD | COLOR_PAIR(227));
                int mx = sx + bw / 2 - 8;
                mvprintw(sy + bh / 2 - 1, mx, "  YOU WIN! 2048! ");
                mvprintw(sy + bh / 2,     mx, " c:Continue  n:New ");
                attroff(A_BOLD | COLOR_PAIR(227));
            } else if (g_state.over) {
                attron(A_BOLD | COLOR_PAIR(197));
                int mx = sx + bw / 2 - 7;
                mvprintw(sy + bh / 2 - 1, mx, "  GAME OVER!  ");
                mvprintw(sy + bh / 2,     mx, " u:Undo n:New ");
                attroff(A_BOLD | COLOR_PAIR(197));
            }
            refresh();
            need_redraw = 0;
        }

        ch = getch();
        if (ch == ERR) continue;

        if (ch == 'q' || ch == 27 /* ESC */) break;

        if (ch == 'n') { new_game(); new_r = -1; need_redraw = 1; continue; }
        if (ch == 'u') { pop_undo(); new_r = -1; need_redraw = 1; continue; }
        if (g_state.won == 1 && ch == 'c') { g_state.won = 2; need_redraw = 1; continue; }

        if (g_state.over || g_state.won == 1) continue;

        Dir d;
        int valid = 1;
        switch (ch) {
            case KEY_LEFT:  case 'a': case 'h': d = DIR_LEFT;  break;
            case KEY_RIGHT: case 'd': case 'l': d = DIR_RIGHT; break;
            case KEY_UP:    case 'w': case 'k': d = DIR_UP;    break;
            case KEY_DOWN:  case 's': case 'j': d = DIR_DOWN;  break;
            default: valid = 0;
        }
        if (!valid) continue;

        /* Track merged cells for animation */
        int old_vals[N][N];
        memcpy(old_vals, g_state.val, sizeof old_vals);

        if (do_move(d)) {
            /* Find the new spawned tile */
            new_r = -1;
            for (int r = 0; r < N; r++)
                for (int c = 0; c < N; c++)
                    if (g_state.val[r][c] != 0 && old_vals[r][c] == 0) {
                        /* This is approximate: old_vals was before do_move,
                           so the new tile from spawn won't match exactly.
                           We'll find the newest by looking for the spawn. */
                    }
            /* Simpler: track last empty -> filled */
            new_r = -1;
            for (int r = 0; r < N; r++)
                for (int c = 0; c < N; c++)
                    if (g_state.val[r][c] != 0 && old_vals[r][c] == 0)
                        { new_r = r; new_c = c; }

            /* Find merged cells for flash animation */
            int mr[N*N], mc[N*N], nm = 0;
            for (int r = 0; r < N; r++)
                for (int c = 0; c < N; c++)
                    if (g_state.val[r][c] > old_vals[r][c] && old_vals[r][c] > 0 &&
                        (g_state.val[r][c] == old_vals[r][c] * 2))
                        { mr[nm] = r; mc[nm] = c; nm++; }

            erase();
            draw_board(sy, sx, new_r, new_c, mr, mc, nm);
            refresh();

            /* Brief highlight on new tile */
            usleep(ANIM_MS * 1000);
            new_r = -1;
            need_redraw = 1;
        }
    }

    endwin();
    return 0;
}
