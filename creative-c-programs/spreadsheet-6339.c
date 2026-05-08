/*
 * ============================================================
 *  TSS - Terminal Spreadsheet (VisiCalc-style)
 * ============================================================
 *  An interactive curses spreadsheet with formula evaluation,
 *  cell references (A1-Z99), ranges (A1:B5), functions
 *  (SUM, AVG, MIN, MAX, COUNT), and ASCII box-drawing UI.
 *
 *  Compile:
 *    gcc -O2 -o spreadsheet-6339 spreadsheet-6339.c -lm -lncursesw
 *
 *  Or without wide chars:
 *    gcc -O2 -o spreadsheet-6339 spreadsheet-6339.c -lm -lncurses
 *
 *  Run:
 *    ./spreadsheet-6339
 *
 *  Keys:
 *    Arrow keys / hjkl  - Move cursor
 *    Enter / e           - Edit cell
 *    d                   - Delete cell
 *    g                   - Goto cell (type address like C5)
 *    /                   - Commands: sum/avg/min/max/count/setwidth/quit
 *    q                   - Quit
 *    Tab                 - Move right
 *    Backspace           - Move left (on empty cell) or delete char
 *
 *  Cell format:
 *    Numbers:  42, 3.14, -7
 *    Text:     Hello World (anything non-numeric, non-formula)
 *    Formula:  =A1+B2*3, =SUM(A1:A10), =AVG(B1:B5)+C1
 *
 *  Copyright 2026 - Creative C Programs
 * ============================================================
 */

#define _XOPEN_SOURCE 700
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <float.h>

/* ── Configuration ──────────────────────────────────────── */

#define MAX_COLS      26        /* A – Z */
#define MAX_ROWS      99
#define MAX_FORMULA   256
#define DISPLAY_W     12        /* default column width */
#define MIN_COL_W     4
#define MAX_COL_W     40
#define MAX_UNDO      64
#define FUNC_STACK    64

/* ── Data types ─────────────────────────────────────────── */

typedef enum { CELL_EMPTY, CELL_NUM, CELL_TEXT, CELL_FORMULA } CellType;

typedef struct {
    CellType type;
    double   num;               /* cached numeric value         */
    char    *text;              /* text value / formula source  */
    int      dirty;             /* needs re-evaluation          */
    int      error;             /* 1 = circular / parse error   */
} Cell;

typedef struct {
    int  col_w[MAX_COLS];       /* per-column widths            */
    Cell grid[MAX_COLS][MAX_ROWS];
} Sheet;

/* ── Globals ────────────────────────────────────────────── */

static Sheet  sheet;
static int    cur_col, cur_row;       /* cursor position      */
static int    scroll_col, scroll_row; /* viewport offset      */
static int    screen_w, screen_h;     /* terminal size        */
static int    status_attr;            /* highlight for status */

/* ── Helpers ────────────────────────────────────────────── */

static void col_label(int c, char *buf, int sz) {
    /* A, B, … Z */
    snprintf(buf, sz, "%c", 'A' + c);
}

static int parse_col_char(char ch) {
    ch = toupper((unsigned char)ch);
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    return -1;
}

static void cell_addr(int c, int r, char *buf, int sz) {
    snprintf(buf, sz, "%c%d", 'A' + c, (r + 1) % 100);
}

/* ── Cell management ────────────────────────────────────── */

static void cell_clear(Cell *c) {
    free(c->text);
    c->text  = NULL;
    c->type  = CELL_EMPTY;
    c->num   = 0;
    c->dirty = 0;
    c->error = 0;
}

static void cell_set_num(int c, int r, double v) {
    Cell *cell = &sheet.grid[c][r];
    cell_clear(cell);
    cell->type = CELL_NUM;
    cell->num  = v;
    cell->dirty = 0;
}

static void cell_set_text(int c, int r, const char *s) {
    Cell *cell = &sheet.grid[c][r];
    cell_clear(cell);
    cell->type = CELL_TEXT;
    cell->text = strdup(s);
    cell->dirty = 0;
}

static void cell_set_formula(int c, int r, const char *expr) {
    Cell *cell = &sheet.grid[c][r];
    cell_clear(cell);
    cell->type = CELL_FORMULA;
    cell->text = strdup(expr);
    cell->dirty = 1;
    cell->error = 0;
}

/* ── Formula evaluator (recursive descent) ──────────────── */

/* Forward declaration for mutual recursion */
static double eval_expr(const char **s, int depth);
static double eval_cell_ref(int c, int r, int depth);

static void skip_ws(const char **s) {
    while (**s == ' ') (*s)++;
}

/* Parse a cell reference like A1 or B23, advance *s */
static int parse_ref(const char **s, int *c, int *r) {
    skip_ws(s);
    if (!isalpha((unsigned char)**s)) return 0;
    *c = parse_col_char(**s);
    if (*c < 0) return 0;
    (*s)++;
    if (!isdigit((unsigned char)**s)) return 0;
    int n = 0;
    while (isdigit((unsigned char)**s)) {
        n = n * 10 + (**s - '0');
        (*s)++;
    }
    if (n < 1 || n > MAX_ROWS) return 0;
    *r = n - 1;
    return 1;
}

/* Evaluate function: SUM, AVG, MIN, MAX, COUNT */
static double eval_func(const char **s, int depth) {
    skip_ws(s);

    /* Read function name */
    char fname[16] = {0};
    int fi = 0;
    while (isalpha((unsigned char)**s) && fi < 15) {
        fname[fi++] = toupper((unsigned char)**s);
        (*s)++;
    }
    fname[fi] = '\0';

    skip_ws(s);
    if (**s != '(') return NAN;
    (*s)++; /* skip ( */

    /* Parse range: C1:C10 */
    skip_ws(s);
    int c1, r1, c2, r2;
    if (!parse_ref(s, &c1, &r1)) return NAN;
    skip_ws(s);
    if (**s == ':') {
        (*s)++;
        if (!parse_ref(s, &c2, &r2)) return NAN;
    } else {
        c2 = c1; r2 = r1;
    }
    skip_ws(s);
    if (**s == ')') (*s)++;

    /* Compute */
    double result = 0, val;
    int count = 0;
    double mn = DBL_MAX, mx = -DBL_MAX;
    int do_min = 0, do_max = 0, do_count = 0, do_avg = 0;

    if      (strcmp(fname, "SUM")   == 0) { /* default accumulate */ }
    else if (strcmp(fname, "AVG")   == 0) { do_avg = 1; }
    else if (strcmp(fname, "MIN")   == 0) { do_min = 1; mn = DBL_MAX; }
    else if (strcmp(fname, "MAX")   == 0) { do_max = 1; mx = -DBL_MAX; }
    else if (strcmp(fname, "COUNT") == 0) { do_count = 1; }
    else return NAN;

    int sc = c1 < c2 ? c1 : c2, ec = c1 < c2 ? c2 : c1;
    int sr = r1 < r2 ? r1 : r2, er = r1 < r2 ? r2 : r1;

    for (int cc = sc; cc <= ec; cc++) {
        for (int rr = sr; rr <= er; rr++) {
            val = eval_cell_ref(cc, rr, depth + 1);
            if (isnan(val)) continue;
            count++;
            result += val;
            if (val < mn) mn = val;
            if (val > mx) mx = val;
        }
    }

    if (do_avg)   return count ? result / count : 0;
    if (do_min)   return count ? mn : 0;
    if (do_max)   return count ? mx : 0;
    if (do_count) return count;
    return result; /* SUM */
}

/* Primary: number | cell ref | function | ( expr ) | unary +/- */
static double eval_primary(const char **s, int depth) {
    skip_ws(s);

    /* Parenthesized expression */
    if (**s == '(') {
        (*s)++;
        double v = eval_expr(s, depth);
        skip_ws(s);
        if (**s == ')') (*s)++;
        return v;
    }

    /* Function call (5+ alpha followed by '(') */
    if (isalpha((unsigned char)**s)) {
        const char *peek = *s;
        int alen = 0;
        while (isalpha((unsigned char)*peek)) { alen++; peek++; }
        while (*peek == ' ') peek++;
        if (alen >= 3 && *peek == '(') {
            return eval_func(s, depth);
        }
    }

    /* Cell reference: single letter then digits */
    if (isalpha((unsigned char)**s)) {
        const char *save = *s;
        int c, r;
        if (parse_ref(s, &c, &r)) {
            return eval_cell_ref(c, r, depth);
        }
        *s = save; /* not a valid ref, backtrack */
    }

    /* Number */
    char *end;
    double v = strtod(*s, &end);
    if (end != *s) {
        *s = end;
        return v;
    }

    return 0;
}

/* Unary */
static double eval_unary(const char **s, int depth) {
    skip_ws(s);
    if (**s == '-') { (*s)++; return -eval_unary(s, depth); }
    if (**s == '+') { (*s)++; return  eval_unary(s, depth); }
    return eval_primary(s, depth);
}

/* Multiplicative: * / % */
static double eval_mul(const char **s, int depth) {
    double v = eval_unary(s, depth);
    skip_ws(s);
    while (**s == '*' || **s == '/' || **s == '%') {
        char op = **s; (*s)++;
        double r = eval_unary(s, depth);
        if      (op == '*') v *= r;
        else if (op == '/' && r != 0) v /= r;
        else if (op == '%' && r != 0) v = fmod(v, r);
        else if (op == '/') v = NAN;
        skip_ws(s);
    }
    return v;
}

/* Additive: + - */
static double eval_add(const char **s, int depth) {
    double v = eval_mul(s, depth);
    skip_ws(s);
    while (**s == '+' || **s == '-') {
        char op = **s; (*s)++;
        double r = eval_mul(s, depth);
        if (op == '+') v += r; else v -= r;
        skip_ws(s);
    }
    return v;
}

/* Top-level expression */
static double eval_expr(const char **s, int depth) {
    if (depth > FUNC_STACK) return NAN;
    return eval_add(s, depth);
}

/* Resolve a cell's numeric value (with circular-ref guard) */
static double eval_cell_ref(int c, int r, int depth) {
    if (c < 0 || c >= MAX_COLS || r < 0 || r >= MAX_ROWS) return 0;
    Cell *cell = &sheet.grid[c][r];

    if (cell->type == CELL_EMPTY) return 0;
    if (cell->type == CELL_NUM)   return cell->num;
    if (cell->type == CELL_TEXT)  return 0;

    /* Formula */
    if (cell->error) return NAN;

    /* Mark dirty to detect circular refs */
    if (cell->dirty && depth > 1) {
        cell->error = 1;
        return NAN;
    }
    if (!cell->dirty) return cell->num;

    cell->dirty = 0; /* temporarily clear for recursion detect */
    const char *expr = cell->text;
    if (expr && *expr == '=') expr++;
    double v = eval_expr(&expr, depth);
    cell->num = v;
    if (isnan(v)) cell->error = 1;
    return v;
}

/* Recalculate all formula cells (multi-pass for dependencies) */
static void recalc_all(void) {
    for (int pass = 0; pass < 5; pass++) {
        int any_dirty = 0;
        for (int c = 0; c < MAX_COLS; c++)
            for (int r = 0; r < MAX_ROWS; r++) {
                Cell *cell = &sheet.grid[c][r];
                if (cell->type == CELL_FORMULA) {
                    cell->dirty = 1;
                    cell->error = 0;
                    eval_cell_ref(c, r, 0);
                    if (cell->dirty) any_dirty = 1;
                }
            }
        if (!any_dirty) break;
    }
}

/* ── Display ────────────────────────────────────────────── */

static int col_start(int c) {
    /* returns screen-x for left edge of column c */
    int x = 4; /* row-label area */
    for (int i = scroll_col; i < c; i++)
        x += sheet.col_w[i] + 1;
    return x;
}

static int row_start(int r) {
    return 2 + (r - scroll_row); /* row 0 is header, row 1 is separator */
}

static void draw_header(void) {
    attron(A_REVERSE);
    mvprintw(0, 0, "TSS - Terminal Spreadsheet");
    clrtoeol();
    attroff(A_REVERSE);

    /* Column labels */
    attron(A_REVERSE);
    mvaddch(1, 0, ' ');
    mvaddch(1, 1, ' ');
    mvaddch(1, 2, ' ');
    mvaddch(1, 3, ' ');
    int x = 4;
    for (int c = scroll_col; c < MAX_COLS && x < screen_w; c++) {
        char lbl[4];
        col_label(c, lbl, sizeof(lbl));
        int w = sheet.col_w[c];
        mvprintw(1, x, "%-*s", w, lbl);
        x += w + 1;
    }
    clrtoeol();
    attroff(A_REVERSE);
}

static void draw_row_label(int r) {
    attron(A_REVERSE);
    mvprintw(row_start(r), 0, "%3d", r + 1);
    attroff(A_REVERSE);
}

static void draw_cell(int c, int r) {
    int x = col_start(c);
    int y = row_start(r);
    if (x >= screen_w || y >= screen_h - 2) return;

    Cell *cell = &sheet.grid[c][r];
    int w = sheet.col_w[c];
    if (x + w > screen_w) w = screen_w - x;

    char buf[64];
    int is_cur = (c == cur_col && r == cur_row);

    if (is_cur) attron(A_BOLD | A_UNDERLINE);

    if (cell->type == CELL_EMPTY) {
        snprintf(buf, sizeof(buf), "%*s", w, "");
    } else if (cell->type == CELL_FORMULA && cell->error) {
        snprintf(buf, sizeof(buf), "%*s", w, "#ERR");
    } else if (cell->type == CELL_NUM || cell->type == CELL_FORMULA) {
        double v = cell->num;
        if (v == floor(v) && fabs(v) < 1e12)
            snprintf(buf, sizeof(buf), "%*.0lf", w, v);
        else
            snprintf(buf, sizeof(buf), "%*.4g", w, v);
    } else {
        /* Text – left-align */
        int tlen = strlen(cell->text ? cell->text : "");
        if (tlen > w) tlen = w;
        char tmp[64];
        strncpy(tmp, cell->text ? cell->text : "", tlen);
        tmp[tlen] = '\0';
        snprintf(buf, sizeof(buf), "%-*s", w, tmp);
    }

    buf[w] = '\0';
    mvprintw(y, x, "%s", buf);

    if (is_cur) attroff(A_BOLD | A_UNDERLINE);

    /* Column separator */
    if (x + w < screen_w)
        mvaddch(y, x + w, ACS_VLINE);
}

static void draw_grid(void) {
    for (int r = scroll_row; r < MAX_ROWS; r++) {
        int y = row_start(r);
        if (y >= screen_h - 2) break;
        draw_row_label(r);
        for (int c = scroll_col; c < MAX_COLS; c++) {
            if (col_start(c) >= screen_w) break;
            draw_cell(c, r);
        }
    }
}

static void draw_status(void) {
    char addr[8];
    cell_addr(cur_col, cur_row, addr, sizeof(addr));

    Cell *cell = &sheet.grid[cur_col][cur_row];
    char info[128] = "";

    if (cell->type == CELL_FORMULA) {
        snprintf(info, sizeof(info), "  [%s] = %s", addr, cell->text ? cell->text : "");
    } else if (cell->type == CELL_NUM) {
        snprintf(info, sizeof(info), "  [%s] = %g", addr, cell->num);
    } else if (cell->type == CELL_TEXT) {
        snprintf(info, sizeof(info), "  [%s] = \"%s\"", addr, cell->text ? cell->text : "");
    } else {
        snprintf(info, sizeof(info), "  [%s]  (empty)", addr);
    }

    attron(A_REVERSE);
    mvprintw(screen_h - 1, 0, "%s", info);
    clrtoeol();
    attroff(A_REVERSE);

    /* Help bar */
    attron(A_DIM);
    mvprintw(screen_h - 2, 0, "Enter:edit  d:del  g:goto  /:func  q:quit  hjkl/arrows:move  Tab:next col");
    clrtoeol();
    attroff(A_DIM);
}

static void draw_all(void) {
    clear();
    draw_header();
    draw_grid();
    draw_status();
    refresh();
}

/* ── Input ──────────────────────────────────────────────── */

static char *input_line(const char *prompt, const char *initial) {
    /* Simple line editor at the bottom of the screen */
    char buf[MAX_FORMULA];
    int pos = 0;
    if (initial) {
        strncpy(buf, initial, MAX_FORMULA - 1);
        pos = strlen(buf);
    }
    buf[pos] = '\0';

    int base_y = screen_h - 1;

    curs_set(1);
    for (;;) {
        attron(A_REVERSE);
        mvprintw(base_y, 0, "%s%s", prompt, buf);
        clrtoeol();
        attroff(A_REVERSE);
        move(base_y, strlen(prompt) + pos);
        refresh();

        int ch = getch();
        if (ch == '\n' || ch == KEY_ENTER) {
            return strdup(buf);
        }
        if (ch == 27 || ch == 7) { /* ESC or Ctrl-G */
            return NULL;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (pos > 0) { buf[--pos] = '\0'; }
            continue;
        }
        if (ch == KEY_DC) { /* delete */
            if (buf[pos]) memmove(buf + pos, buf + pos + 1, strlen(buf + pos));
            continue;
        }
        if (ch == KEY_LEFT)  { if (pos > 0) pos--; continue; }
        if (ch == KEY_RIGHT) { if (pos < (int)strlen(buf)) pos++; continue; }
        if (ch == 1) { pos = 0; continue; }             /* Ctrl-A: home */
        if (ch == 5) { pos = strlen(buf); continue; }    /* Ctrl-E: end */
        if (ch == 21) { pos = 0; buf[0] = '\0'; continue; } /* Ctrl-U: clear */

        if (ch >= 32 && ch < 127 && pos < MAX_FORMULA - 2) {
            memmove(buf + pos + 1, buf + pos, strlen(buf + pos) + 1);
            buf[pos++] = ch;
        }
    }
}

static void edit_cell(void) {
    char addr[8];
    cell_addr(cur_col, cur_row, addr, sizeof(addr));
    char prompt[16];
    snprintf(prompt, sizeof(prompt), "%s> ", addr);

    Cell *cell = &sheet.grid[cur_col][cur_row];
    const char *initial = NULL;
    if (cell->type == CELL_FORMULA && cell->text) initial = cell->text;
    else if (cell->type == CELL_TEXT && cell->text) initial = cell->text;
    else if (cell->type == CELL_NUM) {
        char tmp[32]; snprintf(tmp, sizeof(tmp), "%g", cell->num);
        /* We need a persistent copy for initial */
        static char numbuf[32];
        strncpy(numbuf, tmp, sizeof(numbuf));
        initial = numbuf;
    }

    char *val = input_line(prompt, initial);
    if (!val) { draw_all(); return; }

    /* Parse input */
    char *p = val;
    while (*p == ' ') p++;

    if (*p == '\0') {
        /* Empty → clear cell */
        cell_clear(&sheet.grid[cur_col][cur_row]);
    } else if (*p == '=') {
        cell_set_formula(cur_col, cur_row, p);
        recalc_all();
    } else {
        /* Try as number */
        char *end;
        double num = strtod(p, &end);
        if (*end == '\0') {
            cell_set_num(cur_col, cur_row, num);
            recalc_all();
        } else {
            cell_set_text(cur_col, cur_row, p);
        }
    }

    free(val);
    draw_all();
}

static void goto_cell(void) {
    char *val = input_line("Goto> ", "");
    if (!val) { draw_all(); return; }

    int c, r;
    const char *s = val;
    if (parse_ref(&s, &c, &r)) {
        cur_col = c;
        cur_row = r;
        /* Adjust scroll to keep cursor visible */
        if (cur_col < scroll_col) scroll_col = cur_col;
        if (cur_row < scroll_row) scroll_row = cur_row;
    }
    free(val);
    draw_all();
}

static void run_command(void) {
    char *val = input_line("/> ", "");
    if (!val) { draw_all(); return; }

    /* Parse command */
    char cmd[32] = {0};
    sscanf(val, "%31s", cmd);
    for (int i = 0; cmd[i]; i++) cmd[i] = toupper((unsigned char)cmd[i]);

    if (strcmp(cmd, "QUIT") == 0 || strcmp(cmd, "Q") == 0) {
        free(val);
        endwin();
        exit(0);
    }

    if (strcmp(cmd, "SETWIDTH") == 0 || strcmp(cmd, "W") == 0) {
        int w = 0;
        char arg[8];
        if (sscanf(val, "%*s %7s %d", arg, &w) >= 2) {
            int c = parse_col_char(arg[0]);
            if (c >= 0 && w >= MIN_COL_W && w <= MAX_COL_W)
                sheet.col_w[c] = w;
        }
        free(val);
        draw_all();
        return;
    }

    /* Range functions: /sum A1:A10 etc */
    if (strcmp(cmd, "SUM") == 0 || strcmp(cmd, "AVG") == 0 ||
        strcmp(cmd, "MIN") == 0 || strcmp(cmd, "MAX") == 0 ||
        strcmp(cmd, "COUNT") == 0) {

        char arg1[8], arg2[8];
        int c1, r1, c2, r2;
        char *rest = val;
        /* skip command word */
        while (*rest && !isspace((unsigned char)*rest)) rest++;
        while (*rest == ' ') rest++;

        const char *s = rest;
        if (parse_ref(&s, &c1, &r1)) {
            while (*s == ' ' || *s == ':') s++;
            if (parse_ref(&s, &c2, &r2)) {
                /* Build formula and place at cursor */
                char formula[MAX_FORMULA];
                char a1[8], a2[8];
                cell_addr(c1, r1, a1, sizeof(a1));
                cell_addr(c2, r2, a2, sizeof(a2));
                snprintf(formula, sizeof(formula), "=%s(%s:%s)", cmd, a1, a2);
                cell_set_formula(cur_col, cur_row, formula);
                recalc_all();
            }
        }
        free(val);
        draw_all();
        return;
    }

    free(val);
    draw_all();
}

/* ── Scroll adjustment ──────────────────────────────────── */

static void adjust_scroll(void) {
    /* Ensure cursor visible */
    int cx = col_start(cur_col);
    int cy = row_start(cur_row);

    if (cx < 4) {
        while (col_start(cur_col) < 4 && scroll_col > 0) scroll_col--;
    }
    if (cx + sheet.col_w[cur_col] >= screen_w) {
        while (col_start(cur_col) + sheet.col_w[cur_col] >= screen_w) scroll_col++;
    }
    if (cy < 2) scroll_row--;
    if (cy >= screen_h - 2) scroll_row++;
}

/* ── Main loop ──────────────────────────────────────────── */

static void init_sheet(void) {
    memset(&sheet, 0, sizeof(sheet));
    for (int c = 0; c < MAX_COLS; c++)
        sheet.col_w[c] = DISPLAY_W;
}

int main(void) {
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    getmaxyx(stdscr, screen_h, screen_w);

    init_sheet();
    cur_col = 0;
    cur_row = 0;

    draw_all();

    for (;;) {
        int ch = getch();

        switch (ch) {
        case 'q':
            endwin();
            return 0;

        case 'h': case KEY_LEFT:
            if (cur_col > 0) cur_col--;
            adjust_scroll();
            draw_all();
            break;
        case 'l': case KEY_RIGHT: case '\t':
            if (cur_col < MAX_COLS - 1) cur_col++;
            adjust_scroll();
            draw_all();
            break;
        case 'k': case KEY_UP:
            if (cur_row > 0) cur_row--;
            adjust_scroll();
            draw_all();
            break;
        case 'j': case KEY_DOWN: case KEY_ENTER:
            if (ch != KEY_ENTER || 1) {
                if (ch == KEY_DOWN && cur_row < MAX_ROWS - 1) cur_row++;
            }
            adjust_scroll();
            draw_all();
            break;

        case '\n':
            edit_cell();
            break;
        case 'e':
            edit_cell();
            break;
        case 'i': /* vi-style insert */
            edit_cell();
            break;

        case 'd':
            cell_clear(&sheet.grid[cur_col][cur_row]);
            recalc_all();
            draw_all();
            break;

        case 'g':
            goto_cell();
            break;

        case '/':
            run_command();
            break;

        case KEY_RESIZE:
            getmaxyx(stdscr, screen_h, screen_w);
            draw_all();
            break;
        }
    }
}
