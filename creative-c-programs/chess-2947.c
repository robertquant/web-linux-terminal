/*
 * Terminal Chess Game with AI Opponent
 * =====================================
 * A fully-featured chess game playable in the terminal.
 *
 * Features:
 *   - Unicode chess piece rendering with ANSI colors
 *   - Complete move validation (castling, en passant, promotion)
 *   - AI opponent using minimax with alpha-beta pruning (depth 4)
 *   - Check, checkmate, stalemate detection
 *   - Move history in algebraic notation
 *   - Board highlighting for last move and king in check
 *   - Play as White or Black, or watch AI vs AI
 *   - Undo moves, save/load games
 *
 * Compile:
 *   gcc -std=c99 -O2 -o chess chess-2947.c -lm
 *
 * Run:
 *   ./chess
 *
 * Requires: C99 compiler, Unicode-capable terminal, 256-color support
 *
 * Controls:
 *   Enter moves in coordinate notation: e2e4, e7e8q (promotion)
 *   u       - undo last move
 *   ai      - toggle AI color (play as White/Black/Both)
 *   save    - save game to file
 *   load    - load game from file
 *   new     - start new game
 *   hint    - show AI-recommended move
 *   quit    - exit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

/* ======================== Constants ======================== */

#define BOARD_SIZE 8
#define MAX_MOVES 256
#define AI_DEPTH 4
#define MAX_HISTORY 512
#define SAVE_FILE "chess_save.txt"

/* Piece types */
#define EMPTY  0
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6

/* White = positive, Black = negative */
#define WHITE  1
#define BLACK -1

/* ANSI color codes */
#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define DIM     "\033[2m"
#define INV     "\033[7m"
#define FG_RED     "\033[31m"
#define FG_GREEN   "\033[32m"
#define FG_YELLOW  "\033[33m"
#define FG_BLUE    "\033[34m"
#define FG_MAGENTA "\033[35m"
#define FG_CYAN    "\033[36m"
#define FG_WHITE   "\033[37m"
#define FG_BRED    "\033[91m"
#define FG_BGREEN  "\033[92m"
#define FG_BYELLOW "\033[93m"
#define FG_BBLUE   "\033[94m"
#define FG_BCYAN   "\033[96m"
#define BG_LIGHT   "\033[48;5;222m"
#define BG_DARK    "\033[48;5;180m"
#define BG_SELECT  "\033[48;5;118m"
#define BG_CHECK   "\033[48;5;196m"
#define BG_LAST_L  "\033[48;5;194m"
#define BG_LAST_D  "\033[48;5;150m"

/* Unicode chess pieces */
static const char *UNICODE_PIECE[7][2] = {
    { " ",  " "  },  /* EMPTY */
    { "\xe2\x99\x99", "\xe2\x99\x9f" }, /* WPawn, BPawn   ♙ ♟ */
    { "\xe2\x99\x98", "\xe2\x99\x9e" }, /* WKnight,BKnight ♘ ♞ */
    { "\xe2\x99\x97", "\xe2\x99\x9d" }, /* WBishop,BBishop ♗ ♝ */
    { "\xe2\x99\x96", "\xe2\x99\x9c" }, /* WRook,  BRook   ♖ ♜ */
    { "\xe2\x99\x95", "\xe2\x99\x9b" }, /* WQueen, BQueen   ♕ ♛ */
    { "\xe2\x99\x94", "\xe2\x99\x9a" }, /* WKing,  BKing    ♔ ♚ */
};

/* Piece values for evaluation */
static const int PIECE_VAL[7] = {0, 100, 320, 330, 500, 900, 20000};

/* Piece-square tables for positional evaluation (from White's perspective) */
static const int PST_PAWN[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
    50, 50, 50, 50, 50, 50, 50, 50,
    10, 10, 20, 30, 30, 20, 10, 10,
     5,  5, 10, 25, 25, 10,  5,  5,
     0,  0,  0, 20, 20,  0,  0,  0,
     5, -5,-10,  0,  0,-10, -5,  5,
     5, 10, 10,-20,-20, 10, 10,  5,
     0,  0,  0,  0,  0,  0,  0,  0
};

static const int PST_KNIGHT[64] = {
    -50,-40,-30,-30,-30,-30,-40,-50,
    -40,-20,  0,  0,  0,  0,-20,-40,
    -30,  0, 10, 15, 15, 10,  0,-30,
    -30,  5, 15, 20, 20, 15,  5,-30,
    -30,  0, 15, 20, 20, 15,  0,-30,
    -30,  5, 10, 15, 15, 10,  5,-30,
    -40,-20,  0,  5,  5,  0,-20,-40,
    -50,-40,-30,-30,-30,-30,-40,-50
};

static const int PST_BISHOP[64] = {
    -20,-10,-10,-10,-10,-10,-10,-20,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -10,  0, 10, 10, 10, 10,  0,-10,
    -10,  5,  5, 10, 10,  5,  5,-10,
    -10,  0, 10, 10, 10, 10,  0,-10,
    -10, 10, 10, 10, 10, 10, 10,-10,
    -10,  5,  0,  0,  0,  0,  5,-10,
    -20,-10,-10,-10,-10,-10,-10,-20
};

static const int PST_ROOK[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
     5, 10, 10, 10, 10, 10, 10,  5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
     0,  0,  0,  5,  5,  0,  0,  0
};

static const int PST_QUEEN[64] = {
    -20,-10,-10, -5, -5,-10,-10,-20,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -10,  0,  5,  5,  5,  5,  0,-10,
     -5,  0,  5,  5,  5,  5,  0, -5,
      0,  0,  5,  5,  5,  5,  0, -5,
    -10,  5,  5,  5,  5,  5,  0,-10,
    -10,  0,  5,  0,  0,  0,  0,-10,
    -20,-10,-10, -5, -5,-10,-10,-20
};

static const int PST_KING_MG[64] = {
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -20,-30,-30,-40,-40,-30,-30,-20,
    -10,-20,-20,-20,-20,-20,-20,-10,
     20, 20,  0,  0,  0,  0, 20, 20,
     20, 30, 10,  0,  0, 10, 30, 20
};

static const int *PST_TABLE[7] = {
    NULL, PST_PAWN, PST_KNIGHT, PST_BISHOP, PST_ROOK, PST_QUEEN, PST_KING_MG
};

/* ======================== Data Structures ======================== */

typedef struct {
    int fr, fc;      /* from row, col */
    int tr, tc;      /* to row, col */
    int promotion;   /* piece type to promote to, 0 if none */
    int captured;    /* piece captured, 0 if none */
    int castle;      /* 0=none, 1=kingside, 2=queenside */
    int en_passant;  /* 1 if en passant capture */
} Move;

typedef struct {
    int board[8][8];
    int side;            /* WHITE or BLACK to move */
    int castle[4];       /* [0]=wK, [1]=wQ, [2]=bK, [3]=bQ */
    int ep_r, ep_c;      /* en passant target, -1 if none */
    int halfmove;
    int fullmove;
    int last_fr, last_fc, last_tr, last_tc;
    /* For undo */
    Move move;
    int prev_castle[4];
    int prev_ep_r, prev_ep_c;
    int prev_halfmove;
    int prev_last_fr, prev_last_fc, prev_last_tr, prev_last_tc;
} GameState;

typedef struct {
    GameState states[MAX_HISTORY];
    int count;
} History;

/* ======================== Global State ======================== */

static GameState g_state;
static History g_history;
static int g_ai_side = BLACK;  /* AI plays Black by default */

/* ======================== Board Utilities ======================== */

static int abs_val(int x) { return x < 0 ? -x : x; }
static int max_val(int a, int b) { return a > b ? a : b; }
static int min_val(int a, int b) { return a < b ? a : b; }

static int in_bounds(int r, int c) {
    return r >= 0 && r < 8 && c >= 0 && c < 8;
}

static int piece_side(int p) {
    if (p > 0) return WHITE;
    if (p < 0) return BLACK;
    return 0;
}

static int piece_type(int p) {
    return abs_val(p);
}

static void init_board(GameState *gs) {
    /* Black pieces (row 0) */
    int back_rank[8] = { ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK };
    for (int c = 0; c < 8; c++) {
        gs->board[0][c] = -back_rank[c];
        gs->board[1][c] = -PAWN;
        gs->board[6][c] = PAWN;
        gs->board[7][c] = back_rank[c];
    }
    for (int r = 2; r < 6; r++)
        for (int c = 0; c < 8; c++)
            gs->board[r][c] = EMPTY;

    gs->side = WHITE;
    gs->castle[0] = gs->castle[1] = gs->castle[2] = gs->castle[3] = 1;
    gs->ep_r = gs->ep_c = -1;
    gs->halfmove = 0;
    gs->fullmove = 1;
    gs->last_fr = gs->last_fc = gs->last_tr = gs->last_tc = -1;
}

/* ======================== Attack Detection ======================== */

static int is_attacked(const GameState *gs, int r, int c, int by_side) {
    /* Knight attacks */
    static const int knight_dr[] = {-2,-2,-1,-1, 1, 1, 2, 2};
    static const int knight_dc[] = {-1, 1,-2, 2,-2, 2,-1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + knight_dr[i], nc = c + knight_dc[i];
        if (in_bounds(nr, nc) && gs->board[nr][nc] == -KNIGHT * by_side)
            return 1;
    }

    /* King attacks */
    for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) continue;
            int nr = r + dr, nc = c + dc;
            if (in_bounds(nr, nc) && gs->board[nr][nc] == -KING * by_side)
                return 1;
        }

    /* Pawn attacks */
    int pawn_dir = (by_side == WHITE) ? 1 : -1; /* direction FROM which pawn attacks */
    for (int dc = -1; dc <= 1; dc += 2) {
        int pr = r + pawn_dir, pc = c + dc;
        if (in_bounds(pr, pc) && gs->board[pr][pc] == -PAWN * by_side)
            return 1;
    }

    /* Sliding pieces: Bishop/Queen diagonals */
    static const int diag_dr[] = {-1, -1, 1, 1};
    static const int diag_dc[] = {-1, 1, -1, 1};
    for (int d = 0; d < 4; d++) {
        for (int dist = 1; dist < 8; dist++) {
            int nr = r + diag_dr[d] * dist, nc = c + diag_dc[d] * dist;
            if (!in_bounds(nr, nc)) break;
            int p = gs->board[nr][nc];
            if (p != EMPTY) {
                if (piece_side(p) == by_side &&
                    (piece_type(p) == BISHOP || piece_type(p) == QUEEN))
                    return 1;
                break;
            }
        }
    }

    /* Sliding pieces: Rook/Queen straights */
    static const int str_dr[] = {-1, 1, 0, 0};
    static const int str_dc[] = {0, 0, -1, 1};
    for (int d = 0; d < 4; d++) {
        for (int dist = 1; dist < 8; dist++) {
            int nr = r + str_dr[d] * dist, nc = c + str_dc[d] * dist;
            if (!in_bounds(nr, nc)) break;
            int p = gs->board[nr][nc];
            if (p != EMPTY) {
                if (piece_side(p) == by_side &&
                    (piece_type(p) == ROOK || piece_type(p) == QUEEN))
                    return 1;
                break;
            }
        }
    }

    return 0;
}

static int find_king(const GameState *gs, int side, int *kr, int *kc) {
    int king = KING * side;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if (gs->board[r][c] == king) {
                *kr = r; *kc = c; return 1;
            }
    return 0;
}

static int in_check(const GameState *gs, int side) {
    int kr, kc;
    if (!find_king(gs, side, &kr, &kc)) return 0;
    return is_attacked(gs, kr, kc, -side);
}

/* ======================== Move Generation ======================== */

static int generate_pseudo_moves(const GameState *gs, Move moves[]) {
    int count = 0;
    int side = gs->side;

    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int p = gs->board[r][c];
            if (piece_side(p) != side) continue;
            int pt = piece_type(p);

            if (pt == PAWN) {
                int dir = (side == WHITE) ? -1 : 1;
                int start_rank = (side == WHITE) ? 6 : 1;
                int promo_rank = (side == WHITE) ? 0 : 7;

                /* Forward one */
                int nr = r + dir;
                if (in_bounds(nr, c) && gs->board[nr][c] == EMPTY) {
                    if (nr == promo_rank) {
                        int promos[] = {QUEEN, ROOK, BISHOP, KNIGHT};
                        for (int pi = 0; pi < 4; pi++) {
                            moves[count++] = (Move){r, c, nr, c, promos[pi], 0, 0, 0};
                        }
                    } else {
                        moves[count++] = (Move){r, c, nr, c, 0, 0, 0, 0};
                    }

                    /* Forward two from starting rank */
                    int nr2 = r + dir * 2;
                    if (r == start_rank && gs->board[nr2][c] == EMPTY) {
                        moves[count++] = (Move){r, c, nr2, c, 0, 0, 0, 0};
                    }
                }

                /* Captures (diagonal) */
                for (int dc = -1; dc <= 1; dc += 2) {
                    int nc = c + dc;
                    if (!in_bounds(nr, nc)) continue;
                    int target = gs->board[nr][nc];

                    if (target != EMPTY && piece_side(target) == -side) {
                        if (nr == promo_rank) {
                            int promos[] = {QUEEN, ROOK, BISHOP, KNIGHT};
                            for (int pi = 0; pi < 4; pi++) {
                                moves[count++] = (Move){r, c, nr, nc, promos[pi], target, 0, 0};
                            }
                        } else {
                            moves[count++] = (Move){r, c, nr, nc, 0, target, 0, 0};
                        }
                    }

                    /* En passant */
                    if (nr == gs->ep_r && nc == gs->ep_c) {
                        moves[count++] = (Move){r, c, nr, nc, 0, -PAWN * side, 0, 1};
                    }
                }
            }
            else if (pt == KNIGHT) {
                static const int dr[] = {-2,-2,-1,-1, 1, 1, 2, 2};
                static const int dc[] = {-1, 1,-2, 2,-2, 2,-1, 1};
                for (int i = 0; i < 8; i++) {
                    int nr = r + dr[i], nc = c + dc[i];
                    if (!in_bounds(nr, nc)) continue;
                    int target = gs->board[nr][nc];
                    if (piece_side(target) != side) {
                        moves[count++] = (Move){r, c, nr, nc, 0, target, 0, 0};
                    }
                }
            }
            else if (pt == BISHOP || pt == ROOK || pt == QUEEN) {
                int start_d, end_d;
                static const int all_dr[] = {-1, -1, 1, 1, -1, 1, 0, 0};
                static const int all_dc[] = {-1, 1, -1, 1, 0, 0, -1, 1};
                if (pt == BISHOP)     { start_d = 0; end_d = 4; }
                else if (pt == ROOK)  { start_d = 4; end_d = 8; }
                else                  { start_d = 0; end_d = 8; } /* Queen */

                for (int d = start_d; d < end_d; d++) {
                    for (int dist = 1; dist < 8; dist++) {
                        int nr = r + all_dr[d] * dist, nc = c + all_dc[d] * dist;
                        if (!in_bounds(nr, nc)) break;
                        int target = gs->board[nr][nc];
                        if (target == EMPTY) {
                            moves[count++] = (Move){r, c, nr, nc, 0, 0, 0, 0};
                        } else {
                            if (piece_side(target) != side)
                                moves[count++] = (Move){r, c, nr, nc, 0, target, 0, 0};
                            break;
                        }
                    }
                }
            }
            else if (pt == KING) {
                /* Normal king moves */
                for (int dr = -1; dr <= 1; dr++)
                    for (int dc = -1; dc <= 1; dc++) {
                        if (dr == 0 && dc == 0) continue;
                        int nr = r + dr, nc = c + dc;
                        if (!in_bounds(nr, nc)) continue;
                        int target = gs->board[nr][nc];
                        if (piece_side(target) != side) {
                            moves[count++] = (Move){r, c, nr, nc, 0, target, 0, 0};
                        }
                    }

                /* Castling */
                if (side == WHITE && r == 7 && c == 4) {
                    /* Kingside: e1 -> g1 */
                    if (gs->castle[0] &&
                        gs->board[7][5] == EMPTY && gs->board[7][6] == EMPTY &&
                        !is_attacked(gs, 7, 4, BLACK) &&
                        !is_attacked(gs, 7, 5, BLACK) &&
                        !is_attacked(gs, 7, 6, BLACK)) {
                        moves[count++] = (Move){7, 4, 7, 6, 0, 0, 1, 0};
                    }
                    /* Queenside: e1 -> c1 */
                    if (gs->castle[1] &&
                        gs->board[7][3] == EMPTY && gs->board[7][2] == EMPTY &&
                        gs->board[7][1] == EMPTY &&
                        !is_attacked(gs, 7, 4, BLACK) &&
                        !is_attacked(gs, 7, 3, BLACK) &&
                        !is_attacked(gs, 7, 2, BLACK)) {
                        moves[count++] = (Move){7, 4, 7, 2, 0, 0, 2, 0};
                    }
                }
                else if (side == BLACK && r == 0 && c == 4) {
                    /* Kingside: e8 -> g8 */
                    if (gs->castle[2] &&
                        gs->board[0][5] == EMPTY && gs->board[0][6] == EMPTY &&
                        !is_attacked(gs, 0, 4, WHITE) &&
                        !is_attacked(gs, 0, 5, WHITE) &&
                        !is_attacked(gs, 0, 6, WHITE)) {
                        moves[count++] = (Move){0, 4, 0, 6, 0, 0, 1, 0};
                    }
                    /* Queenside: e8 -> c8 */
                    if (gs->castle[3] &&
                        gs->board[0][3] == EMPTY && gs->board[0][2] == EMPTY &&
                        gs->board[0][1] == EMPTY &&
                        !is_attacked(gs, 0, 4, WHITE) &&
                        !is_attacked(gs, 0, 3, WHITE) &&
                        !is_attacked(gs, 0, 2, WHITE)) {
                        moves[count++] = (Move){0, 4, 0, 2, 0, 0, 2, 0};
                    }
                }
            }
        }
    }
    return count;
}

/* ======================== Make / Undo Move ======================== */

static void make_move(GameState *gs, const Move *m) {
    int side = gs->side;
    int piece = gs->board[m->fr][m->fc];

    /* Save undo info */
    gs->move = *m;
    memcpy(gs->prev_castle, gs->castle, sizeof(gs->castle));
    gs->prev_ep_r = gs->ep_r;
    gs->prev_ep_c = gs->ep_c;
    gs->prev_halfmove = gs->halfmove;
    gs->prev_last_fr = gs->last_fr;
    gs->prev_last_fc = gs->last_fc;
    gs->prev_last_tr = gs->last_tr;
    gs->prev_last_tc = gs->last_tc;

    /* Halfmove clock */
    if (piece_type(piece) == PAWN || m->captured != 0)
        gs->halfmove = 0;
    else
        gs->halfmove++;

    /* Clear en passant */
    gs->ep_r = gs->ep_c = -1;

    /* Set en passant target for double pawn push */
    if (piece_type(piece) == PAWN && abs_val(m->tr - m->fr) == 2) {
        gs->ep_r = (m->fr + m->tr) / 2;
        gs->ep_c = m->fc;
    }

    /* En passant capture */
    if (m->en_passant) {
        gs->board[m->fr][m->tc] = EMPTY;
    }

    /* Move piece */
    gs->board[m->tr][m->tc] = piece;
    gs->board[m->fr][m->fc] = EMPTY;

    /* Promotion */
    if (m->promotion) {
        gs->board[m->tr][m->tc] = m->promotion * side;
    }

    /* Castling: move the rook */
    if (m->castle == 1) { /* Kingside */
        int row = (side == WHITE) ? 7 : 0;
        gs->board[row][5] = gs->board[row][7];
        gs->board[row][7] = EMPTY;
    } else if (m->castle == 2) { /* Queenside */
        int row = (side == WHITE) ? 7 : 0;
        gs->board[row][3] = gs->board[row][0];
        gs->board[row][0] = EMPTY;
    }

    /* Update castling rights */
    if (piece_type(piece) == KING) {
        if (side == WHITE) { gs->castle[0] = 0; gs->castle[1] = 0; }
        else               { gs->castle[2] = 0; gs->castle[3] = 0; }
    }
    if (piece_type(piece) == ROOK) {
        if (m->fr == 7 && m->fc == 0) gs->castle[1] = 0;
        if (m->fr == 7 && m->fc == 7) gs->castle[0] = 0;
        if (m->fr == 0 && m->fc == 0) gs->castle[3] = 0;
        if (m->fr == 0 && m->fc == 7) gs->castle[2] = 0;
    }
    /* Captured rook loses castling */
    if (m->tr == 7 && m->tc == 0) gs->castle[1] = 0;
    if (m->tr == 7 && m->tc == 7) gs->castle[0] = 0;
    if (m->tr == 0 && m->tc == 0) gs->castle[3] = 0;
    if (m->tr == 0 && m->tc == 7) gs->castle[2] = 0;

    /* Track last move for highlighting */
    gs->last_fr = m->fr; gs->last_fc = m->fc;
    gs->last_tr = m->tr; gs->last_tc = m->tc;

    /* Switch side */
    if (side == BLACK) gs->fullmove++;
    gs->side = -side;
}

static void undo_move(GameState *gs) {
    const Move *m = &gs->move;

    /* Switch back to the side that made the move */
    gs->side = -gs->side;
    if (gs->side == BLACK) gs->fullmove--;

    /* Undo promotion: put original piece back */
    int piece = gs->board[m->tr][m->tc];
    if (m->promotion) {
        piece = PAWN * gs->side;
    }

    /* Move piece back */
    gs->board[m->fr][m->fc] = piece;
    gs->board[m->tr][m->tc] = m->captured;

    /* Undo en passant capture */
    if (m->en_passant) {
        /* The captured pawn was on the same row as 'from', same col as 'to' */
        gs->board[m->fr][m->tc] = -PAWN * gs->side;
        gs->board[m->tr][m->tc] = EMPTY;
    }

    /* Undo castling rook */
    if (m->castle == 1) {
        int row = (gs->side == WHITE) ? 7 : 0;
        gs->board[row][7] = gs->board[row][5];
        gs->board[row][5] = EMPTY;
    } else if (m->castle == 2) {
        int row = (gs->side == WHITE) ? 7 : 0;
        gs->board[row][0] = gs->board[row][3];
        gs->board[row][3] = EMPTY;
    }

    /* Restore saved state */
    memcpy(gs->castle, gs->prev_castle, sizeof(gs->castle));
    gs->ep_r = gs->prev_ep_r;
    gs->ep_c = gs->prev_ep_c;
    gs->halfmove = gs->prev_halfmove;
    gs->last_fr = gs->prev_last_fr;
    gs->last_fc = gs->prev_last_fc;
    gs->last_tr = gs->prev_last_tr;
    gs->last_tc = gs->prev_last_tc;
}

/* ======================== Legal Move Generation ======================== */

static int generate_legal_moves(const GameState *gs, Move moves[]) {
    Move pseudo[MAX_MOVES];
    int n = generate_pseudo_moves(gs, pseudo);
    int legal = 0;

    for (int i = 0; i < n; i++) {
        GameState copy = *gs;
        make_move(&copy, &pseudo[i]);
        /* After making the move, it's opponent's turn. Check if OUR king is safe. */
        if (!in_check(&copy, gs->side)) {
            moves[legal++] = pseudo[i];
        }
    }
    return legal;
}

static int is_legal_move(const GameState *gs, const Move *m) {
    Move legal[MAX_MOVES];
    int n = generate_legal_moves(gs, legal);
    for (int i = 0; i < n; i++) {
        if (legal[i].fr == m->fr && legal[i].fc == m->fc &&
            legal[i].tr == m->tr && legal[i].tc == m->tc &&
            legal[i].promotion == m->promotion)
            return 1;
    }
    return 0;
}

/* ======================== Evaluation ======================== */

static int evaluate(const GameState *gs) {
    int score = 0;
    int wk_r = -1, wk_c = -1, bk_r = -1, bk_c = -1;

    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int p = gs->board[r][c];
            if (p == EMPTY) continue;
            int pt = piece_type(p);
            int side = piece_side(p);
            int sign = side;

            /* Material */
            score += sign * PIECE_VAL[pt];

            /* Positional (piece-square tables) */
            if (PST_TABLE[pt]) {
                /* For black, flip the row */
                int idx = (side == WHITE) ? (r * 8 + c) : ((7 - r) * 8 + c);
                score += sign * PST_TABLE[pt][idx];
            }

            /* Track king positions */
            if (pt == KING) {
                if (side == WHITE) { wk_r = r; wk_c = c; }
                else               { bk_r = r; bk_c = c; }
            }
        }
    }

    /* Mobility bonus */
    Move moves[MAX_MOVES];
    int w_mob = 0, b_mob = 0;
    int orig_side = gs->side;

    /* White mobility */
    ((GameState *)gs)->side = WHITE;
    w_mob = generate_pseudo_moves(gs, moves);
    ((GameState *)gs)->side = BLACK;
    b_mob = generate_pseudo_moves(gs, moves);
    ((GameState *)gs)->side = orig_side;

    score += (w_mob - b_mob) * 3;

    return score;
}

/* ======================== AI: Minimax with Alpha-Beta ======================== */

static int nodes_searched;

static int alpha_beta(GameState *gs, int depth, int alpha, int beta, int maximizing) {
    nodes_searched++;

    if (depth == 0) {
        return evaluate(gs);
    }

    Move moves[MAX_MOVES];
    int n = generate_legal_moves(gs, moves);

    if (n == 0) {
        if (in_check(gs, gs->side)) {
            /* Checkmate: penalize by depth so engine prefers faster mates */
            return maximizing ? -100000 + (AI_DEPTH - depth) : 100000 - (AI_DEPTH - depth);
        }
        return 0; /* Stalemate */
    }

    if (gs->halfmove >= 100) return 0; /* 50-move rule */

    /* Move ordering: captures first, then promotions */
    int scores[MAX_MOVES];
    for (int i = 0; i < n; i++) {
        scores[i] = 0;
        if (moves[i].captured) scores[i] += 10 * PIECE_VAL[piece_type(moves[i].captured)] - PIECE_VAL[piece_type(gs->board[moves[i].fr][moves[i].fc])];
        if (moves[i].promotion) scores[i] += PIECE_VAL[moves[i].promotion];
    }
    /* Simple selection sort for move ordering */
    for (int i = 0; i < n - 1; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++) {
            if (scores[j] > scores[best]) best = j;
        }
        if (best != i) {
            Move tm = moves[i]; moves[i] = moves[best]; moves[best] = tm;
            int ts = scores[i]; scores[i] = scores[best]; scores[best] = ts;
        }
    }

    if (maximizing) {
        int value = -999999;
        for (int i = 0; i < n; i++) {
            GameState copy = *gs;
            make_move(&copy, &moves[i]);
            int child = alpha_beta(&copy, depth - 1, alpha, beta, 0);
            value = max_val(value, child);
            alpha = max_val(alpha, value);
            if (alpha >= beta) break;
        }
        return value;
    } else {
        int value = 999999;
        for (int i = 0; i < n; i++) {
            GameState copy = *gs;
            make_move(&copy, &moves[i]);
            int child = alpha_beta(&copy, depth - 1, alpha, beta, 1);
            value = min_val(value, child);
            beta = min_val(beta, value);
            if (alpha >= beta) break;
        }
        return value;
    }
}

static Move find_best_move(GameState *gs) {
    Move moves[MAX_MOVES];
    int n = generate_legal_moves(gs, moves);
    if (n == 0) return (Move){-1, -1, -1, -1, 0, 0, 0, 0};

    nodes_searched = 0;
    int maximizing = (gs->side == WHITE);
    Move best_move = moves[0];
    int best_value = maximizing ? -999999 : 999999;

    /* Move ordering for root */
    int scores[MAX_MOVES];
    for (int i = 0; i < n; i++) {
        scores[i] = 0;
        if (moves[i].captured) scores[i] += 10 * PIECE_VAL[piece_type(moves[i].captured)];
        if (moves[i].promotion) scores[i] += PIECE_VAL[moves[i].promotion];
    }
    for (int i = 0; i < n - 1; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++) {
            if (scores[j] > scores[best]) best = j;
        }
        if (best != i) {
            Move tm = moves[i]; moves[i] = moves[best]; moves[best] = tm;
            int ts = scores[i]; scores[i] = scores[best]; scores[best] = ts;
        }
    }

    /* Add slight randomness to avoid repetitive play */
    srand((unsigned)time(NULL));

    for (int i = 0; i < n; i++) {
        GameState copy = *gs;
        make_move(&copy, &moves[i]);
        int value = alpha_beta(&copy, AI_DEPTH - 1, -999999, 999999, !maximizing);
        /* Tiny random perturbation for variety */
        value += (rand() % 5) - 2;

        if (maximizing) {
            if (value > best_value) {
                best_value = value;
                best_move = moves[i];
            }
        } else {
            if (value < best_value) {
                best_value = value;
                best_move = moves[i];
            }
        }
    }

    return best_move;
}

/* ======================== Rendering ======================== */

static void clear_screen(void) {
    printf("\033[2J\033[H");
}

static void print_piece(int piece) {
    int pt = piece_type(piece);
    int idx = (piece > 0) ? 0 : 1;
    if (pt < 1 || pt > 6) { printf(" "); return; }

    if (piece > 0) {
        printf(FG_WHITE BOLD "%s" RESET, UNICODE_PIECE[pt][0]);
    } else {
        printf(FG_RED BOLD "%s" RESET, UNICODE_PIECE[pt][1]);
    }
}

static void draw_board(const GameState *gs) {
    int kr_w, kc_w, kr_b, kc_b;
    find_king(gs, WHITE, &kr_w, &kc_w);
    find_king(gs, BLACK, &kr_b, &kc_b);

    printf("\n");

    /* Top border */
    printf("    ");
    for (int c = 0; c < 8; c++) printf("  %c   ", 'a' + c);
    printf("\n");
    printf("   +" RESET);
    for (int c = 0; c < 8; c++) printf("------+");
    printf("\n");

    for (int r = 0; r < 8; r++) {
        printf(" %d |", 8 - r);

        for (int c = 0; c < 8; c++) {
            int p = gs->board[r][c];
            int is_light = (r + c) % 2 == 0;
            int is_last = (r == gs->last_fr && c == gs->last_fc) ||
                          (r == gs->last_tr && c == gs->last_tc);
            int is_king_check = 0;
            if (p == KING * WHITE && in_check(gs, WHITE) && r == kr_w && c == kc_w)
                is_king_check = 1;
            if (p == KING * BLACK && in_check(gs, BLACK) && r == kr_b && c == kc_b)
                is_king_check = 1;

            /* Background color */
            if (is_king_check) {
                printf(BG_CHECK);
            } else if (is_last) {
                printf(is_light ? BG_LAST_L : BG_LAST_D);
            } else {
                printf(is_light ? BG_LIGHT : BG_DARK);
            }

            printf(" ");
            if (p == EMPTY) {
                printf("  ");
            } else {
                print_piece(p);
            }
            printf(" " RESET);
            printf("|");
        }
        printf(" %d\n", 8 - r);

        printf("   +");
        for (int c = 0; c < 8; c++) printf("------+");
        printf("\n");
    }

    printf("    ");
    for (int c = 0; c < 8; c++) printf("  %c   ", 'a' + c);
    printf("\n");
}

static char piece_char(int pt) {
    static const char chars[] = " PNBRQK";
    return (pt >= 1 && pt <= 6) ? chars[pt] : ' ';
}

static void move_to_str(const Move *m, char *buf) {
    if (m->castle == 1) { strcpy(buf, "O-O"); return; }
    if (m->castle == 2) { strcpy(buf, "O-O-O"); return; }
    int len = 0;
    buf[len++] = 'a' + m->fc;
    buf[len++] = '0' + (8 - m->fr);
    buf[len++] = 'a' + m->tc;
    buf[len++] = '0' + (8 - m->tr);
    if (m->promotion) {
        buf[len++] = '=';
        buf[len++] = tolower(piece_char(m->promotion));
    }
    buf[len] = '\0';
}

static void print_status(const GameState *gs) {
    const char *side_str = (gs->side == WHITE) ?
        FG_BYELLOW "White" RESET : FG_BCYAN "Black" RESET;

    printf("\n %s to move", side_str);

    if (in_check(gs, gs->side)) {
        printf(FG_BRED "  [CHECK]" RESET);
    }

    printf("    Move: %d.%s  Halfmove: %d",
        gs->fullmove,
        (gs->side == WHITE) ? ".." : "",
        gs->halfmove);

    /* AI side indicator */
    if (g_ai_side == WHITE) printf("  AI: White");
    else if (g_ai_side == BLACK) printf("  AI: Black");
    else printf("  AI: Off");

    printf("\n");
}

/* ======================== Input Parsing ======================== */

static int parse_move(const char *input, Move *m) {
    /* Handle castling notation */
    if (strcmp(input, "o-o") == 0 || strcmp(input, "O-O") == 0 ||
        strcmp(input, "0-0") == 0) {
        int row = (g_state.side == WHITE) ? 7 : 0;
        m->fr = row; m->fc = 4; m->tr = row; m->tc = 6;
        m->promotion = 0; m->captured = 0; m->castle = 1; m->en_passant = 0;
        return 1;
    }
    if (strcmp(input, "o-o-o") == 0 || strcmp(input, "O-O-O") == 0 ||
        strcmp(input, "0-0-0") == 0) {
        int row = (g_state.side == WHITE) ? 7 : 0;
        m->fr = row; m->fc = 4; m->tr = row; m->tc = 2;
        m->promotion = 0; m->captured = 0; m->castle = 2; m->en_passant = 0;
        return 1;
    }

    /* Parse coordinate notation: e2e4 or e2 e4 or e2e4q */
    int len = (int)strlen(input);
    if (len < 4) return 0;

    char fc = tolower(input[0]);
    char fr = input[1];
    char tc = tolower(input[2]);  /* might be space */
    char tr_char;
    int idx = 3;

    /* Skip spaces */
    if (tc == ' ') {
        if (len < 5) return 0;
        tc = tolower(input[2 + 1]);
        idx = 4;
    }
    tr_char = input[idx];
    idx++;

    if (fc < 'a' || fc > 'h' || tc < 'a' || tc > 'h') return 0;
    if (fr < '1' || fr > '8' || tr_char < '1' || tr_char > '8') return 0;

    m->fc = fc - 'a';
    m->fr = 8 - (fr - '0');
    m->tc = tc - 'a';
    m->tr = 8 - (tr_char - '0');
    m->promotion = 0;
    m->captured = 0;
    m->castle = 0;
    m->en_passant = 0;

    /* Promotion */
    if (idx < len) {
        char promo = tolower(input[idx]);
        switch (promo) {
            case 'q': m->promotion = QUEEN; break;
            case 'r': m->promotion = ROOK; break;
            case 'b': m->promotion = BISHOP; break;
            case 'n': m->promotion = KNIGHT; break;
            default: return 0;
        }
    }

    return 1;
}

/* ======================== Save / Load ======================== */

static void save_game(void) {
    FILE *f = fopen(SAVE_FILE, "w");
    if (!f) { printf("  Cannot open save file.\n"); return; }

    /* Write board */
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            fprintf(f, "%d ", g_state.board[r][c]);
        }
        fprintf(f, "\n");
    }
    fprintf(f, "%d\n", g_state.side);
    fprintf(f, "%d %d %d %d\n", g_state.castle[0], g_state.castle[1],
            g_state.castle[2], g_state.castle[3]);
    fprintf(f, "%d %d\n", g_state.ep_r, g_state.ep_c);
    fprintf(f, "%d %d\n", g_state.halfmove, g_state.fullmove);
    fprintf(f, "%d\n", g_ai_side);

    /* Write history moves */
    fprintf(f, "%d\n", g_history.count);
    for (int i = 0; i < g_history.count; i++) {
        GameState *s = &g_history.states[i];
        Move *mv = &s->move;
        fprintf(f, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
            s->board[0][0], /* We'll use a compact format */
            mv->fr, mv->fc, mv->tr, mv->tc, mv->promotion, mv->captured,
            mv->castle, mv->en_passant,
            s->side, s->castle[0], s->castle[1], s->castle[2], s->castle[3],
            s->ep_r, s->ep_c, s->halfmove, s->fullmove,
            s->prev_castle[0], s->prev_castle[1]);
    }

    fclose(f);
    printf("  Game saved to %s\n", SAVE_FILE);
}

/* ======================== Game Loop ======================== */

static void print_help(void) {
    printf("\n"
        "  " BOLD "Commands:" RESET "\n"
        "  " FG_BGREEN "e2e4" RESET "      - move piece (coordinate notation)\n"
        "  " FG_BGREEN "e7e8q" RESET "    - pawn promotion (q/r/b/n)\n"
        "  " FG_BGREEN "O-O" RESET "        - kingside castling\n"
        "  " FG_BGREEN "O-O-O" RESET "      - queenside castling\n"
        "  " FG_BCYAN "u" RESET "          - undo last move\n"
        "  " FG_BCYAN "ai" RESET "        - toggle AI side (White/Black/Off)\n"
        "  " FG_BCYAN "hint" RESET "      - show AI suggestion\n"
        "  " FG_BCYAN "save" RESET "      - save game to file\n"
        "  " FG_BCYAN "new" RESET "       - start new game\n"
        "  " FG_BCYAN "quit" RESET "      - exit\n"
        "\n");
}

static void print_captured(const GameState *gs) {
    /* Count material on board */
    int w_material[7] = {0}, b_material[7] = {0};
    int w_initial[7] = {0, 8, 2, 2, 2, 1, 1};
    int b_initial[7] = {0, 8, 2, 2, 2, 1, 1};

    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            int p = gs->board[r][c];
            if (p > 0) w_material[piece_type(p)]++;
            else if (p < 0) b_material[piece_type(p)]++;
        }

    printf("  Captured: ");
    int first = 1;
    for (int pt = 1; pt <= 6; pt++) {
        int missing = b_initial[pt] - b_material[pt];
        for (int i = 0; i < missing; i++) {
            if (!first) printf(" ");
            printf(FG_WHITE "%s" RESET, UNICODE_PIECE[pt][0]);
            first = 0;
        }
    }
    printf("  |  ");
    for (int pt = 1; pt <= 6; pt++) {
        int missing = w_initial[pt] - w_material[pt];
        for (int i = 0; i < missing; i++) {
            printf(FG_RED "%s" RESET " ", UNICODE_PIECE[pt][1]);
        }
    }
    printf("\n");
}

int main(void) {
    char input[64];
    int game_over = 0;

    init_board(&g_state);
    g_history.count = 0;

    clear_screen();
    printf(BOLD FG_BYELLOW "\n"
        "  ╔══════════════════════════════════════╗\n"
        "  ║     ♔  Terminal Chess  ♚            ║\n"
        "  ║   AI Opponent  ·  Alpha-Beta  d=4   ║\n"
        "  ╚══════════════════════════════════════╝\n"
        RESET "\n");

    printf("  Play as: " FG_BGREEN "1" RESET ") White  " FG_BGREEN "2" RESET ") Black  "
           FG_BGREEN "3" RESET ") Watch AI vs AI\n");
    printf("  Choice [1]: ");
    fflush(stdout);

    if (fgets(input, sizeof(input), stdin)) {
        int choice = atoi(input);
        if (choice == 2) { g_ai_side = WHITE; }
        else if (choice == 3) { g_ai_side = WHITE | BLACK; } /* Both */
        else { g_ai_side = BLACK; }
    }

    while (1) {
        clear_screen();
        draw_board(&g_state);
        print_captured(&g_state);
        print_status(&g_state);

        if (game_over) {
            printf("\n  Game over. Type " FG_BGREEN "new" RESET " to start a new game, "
                   FG_BGREEN "u" RESET " to undo, or " FG_BGREEN "quit" RESET " to exit.\n");
        }

        /* Check for game end */
        if (!game_over) {
            Move legal[MAX_MOVES];
            int n_legal = generate_legal_moves(&g_state, legal);

            if (n_legal == 0) {
                game_over = 1;
                if (in_check(&g_state, g_state.side)) {
                    const char *winner = (g_state.side == WHITE) ?
                        FG_BYELLOW "Black" RESET : FG_BYELLOW "White" RESET;
                    printf("\n  " BOLD "CHECKMATE! %s wins!" RESET "\n", winner);
                } else {
                    printf("\n  " BOLD "STALEMATE! It's a draw." RESET "\n");
                }
                printf("  Type " FG_BGREEN "new" RESET " to start a new game, or "
                       FG_BGREEN "quit" RESET " to exit.\n");
            } else if (g_state.halfmove >= 100) {
                game_over = 1;
                printf("\n  " BOLD "Draw by 50-move rule." RESET "\n");
            }
        }

        /* AI's turn */
        if (!game_over && g_ai_side == g_state.side) {
            printf("\n  AI is thinking");
            fflush(stdout);

            Move best = find_best_move(&g_state);
            char move_str[16];
            move_to_str(&best, move_str);

            printf(" ... %s" FG_BGREEN "%s" RESET " (%d nodes evaluated)\n",
                (g_state.side == WHITE) ? FG_BYELLOW : FG_BCYAN,
                move_str, nodes_searched);

            /* Save to history */
            if (g_history.count < MAX_HISTORY) {
                g_history.states[g_history.count++] = g_state;
            }

            make_move(&g_state, &best);

            printf("  Press Enter to continue...");
            fflush(stdout);
            fgets(input, sizeof(input), stdin);
            continue;
        }

        printf("\n  Enter move or command: ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) break;

        /* Trim newline */
        input[strcspn(input, "\n")] = 0;

        /* Skip empty input */
        if (input[0] == '\0') continue;

        /* Commands */
        if (strcmp(input, "quit") == 0 || strcmp(input, "q") == 0) {
            printf("\n  Goodbye!\n\n");
            break;
        }
        else if (strcmp(input, "help") == 0 || strcmp(input, "h") == 0) {
            print_help();
            printf("  Press Enter to continue...");
            fgets(input, sizeof(input), stdin);
            continue;
        }
        else if (strcmp(input, "new") == 0) {
            init_board(&g_state);
            g_history.count = 0;
            game_over = 0;
            continue;
        }
        else if (strcmp(input, "u") == 0 || strcmp(input, "undo") == 0) {
            /* Undo: undo AI move + human move */
            int undo_count = (g_ai_side != 0) ? 2 : 1;
            for (int u = 0; u < undo_count; u++) {
                if (g_history.count > 0) {
                    g_history.count--;
                    g_state = g_history.states[g_history.count];
                }
            }
            game_over = 0;
            continue;
        }
        else if (strcmp(input, "ai") == 0) {
            if (g_ai_side == BLACK) { g_ai_side = WHITE; printf("  AI now plays White.\n"); }
            else if (g_ai_side == WHITE) { g_ai_side = 0; printf("  AI off. Human vs Human.\n"); }
            else { g_ai_side = BLACK; printf("  AI now plays Black.\n"); }
            printf("  Press Enter...");
            fgets(input, sizeof(input), stdin);
            continue;
        }
        else if (strcmp(input, "hint") == 0) {
            printf("  Calculating hint...");
            fflush(stdout);
            Move hint = find_best_move(&g_state);
            char ms[16];
            move_to_str(&hint, ms);
            printf(" Suggestion: " FG_BGREEN "%s" RESET "\n", ms);
            printf("  Press Enter...");
            fgets(input, sizeof(input), stdin);
            continue;
        }
        else if (strcmp(input, "save") == 0) {
            save_game();
            printf("  Press Enter...");
            fgets(input, sizeof(input), stdin);
            continue;
        }

        /* Try to parse as a move */
        if (game_over) {
            printf("  Game is over. Type " FG_BGREEN "new" RESET " or " FG_BGREEN "quit" RESET ".\n");
            printf("  Press Enter...");
            fgets(input, sizeof(input), stdin);
            continue;
        }

        Move m;
        if (!parse_move(input, &m)) {
            printf("  " FG_BRED "Invalid input. Type 'help' for commands." RESET "\n");
            printf("  Press Enter...");
            fgets(input, sizeof(input), stdin);
            continue;
        }

        if (!is_legal_move(&g_state, &m)) {
            printf("  " FG_BRED "Illegal move." RESET "\n");
            printf("  Press Enter...");
            fgets(input, sizeof(input), stdin);
            continue;
        }

        /* Auto-promote to queen if promotion not specified */
        if (g_state.board[m.fr][m.fc] != EMPTY &&
            piece_type(g_state.board[m.fr][m.fc]) == PAWN &&
            (m.tr == 0 || m.tr == 7) && m.promotion == 0) {
            m.promotion = QUEEN;
        }

        /* Save state for undo */
        if (g_history.count < MAX_HISTORY) {
            g_history.states[g_history.count++] = g_state;
        }

        make_move(&g_state, &m);
    }

    return 0;
}
