/*
 * pathfind-1428.c - Terminal Pathfinding Algorithm Visualizer
 *
 * Real-time animated visualization of BFS, DFS, Dijkstra's, and A*
 * pathfinding algorithms on an interactive grid with maze generation.
 *
 * Compile: gcc -O2 -o pathfind pathfind-1428.c -lncurses -lm
 * Run:     ./pathfind
 *
 * Requires: ncurses (sudo apt-get install libncurses5-dev)
 *
 * Controls:
 *   Arrow Keys    Move cursor
 *   Space         Toggle wall (edit) / Pause-Resume (running)
 *   S             Set start point at cursor
 *   E             Set end point at cursor
 *   M             Generate random maze
 *   1             Run Breadth-First Search
 *   2             Run Depth-First Search
 *   3             Run Dijkstra's algorithm
 *   4             Run A* search
 *   + / =         Increase animation speed
 *   - / _         Decrease animation speed
 *   N             Single step (when paused)
 *   C             Clear path only (keep walls)
 *   R             Reset entire grid
 *   Q / Esc       Quit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <ncurses.h>
#include <sys/time.h>
#include <unistd.h>

/* Cell types */
#define EMPTY    0
#define WALL     1
#define START    2
#define END      3
#define VISITED  4
#define FRONTIER 5
#define PATH     6

/* Grid limits */
#define MAXR 80
#define MAXC 200
#define MAXCELLS (MAXR * MAXC)

/* 4-directional offsets */
static const int DR[] = {-1, 0, 1, 0};
static const int DC[] = {0, 1, 0, -1};

/* ---- Data types ---- */
typedef struct { int r, c; } Pos;
typedef struct { int r, c; double f; } PQNode;

/* ---- Global grid state ---- */
static struct {
    int type;
    int pr, pc;          /* parent cell for path reconstruction */
    double g;            /* cost from start (Dijkstra / A*) */
    int closed;          /* already processed */
} cell[MAXR][MAXC];

static int rows, cols;           /* grid dimensions */
static int sr, sc, er, ec;      /* start / end positions */
static int cr, cc;              /* cursor position */

static int algo;                 /* current algorithm: 0=none 1-4 */
static int mode;                 /* 0=edit 1=running 2=paused 3=done */
static int visited_cnt;
static int path_len;
static double elapsed_ms;
static int delay_us = 2000;      /* microseconds between animation steps */

/* Algorithm scratch data */
static Pos bfs_q[MAXCELLS];
static int bfs_hd, bfs_tl;

static Pos dfs_stk[MAXCELLS];
static int dfs_top;

static PQNode pq[MAXCELLS];
static int pq_n;

/* ---- Min-heap priority queue ---- */

static void pq_push(int r, int c, double f)
{
    int i = pq_n++;
    pq[i] = (PQNode){r, c, f};
    while (i > 0) {
        int p = (i - 1) / 2;
        if (pq[p].f <= pq[i].f) break;
        PQNode t = pq[p]; pq[p] = pq[i]; pq[i] = t;
        i = p;
    }
}

static PQNode pq_pop(void)
{
    PQNode top = pq[0];
    pq[0] = pq[--pq_n];
    int i = 0;
    for (;;) {
        int l = 2*i + 1, r = 2*i + 2, m = i;
        if (l < pq_n && pq[l].f < pq[m].f) m = l;
        if (r < pq_n && pq[r].f < pq[m].f) m = r;
        if (m == i) break;
        PQNode t = pq[i]; pq[i] = pq[m]; pq[m] = t;
        i = m;
    }
    return top;
}

/* ---- Grid operations ---- */

static void grid_init(void)
{
    memset(cell, 0, sizeof(cell));
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            cell[r][c].g = 1e18;
            cell[r][c].pr = cell[r][c].pc = -1;
        }
    sr = rows / 2; sc = cols / 4;
    er = rows / 2; ec = 3 * cols / 4;
    cell[sr][sc].type = START;
    cell[er][ec].type = END;
    cr = sr; cc = sc;
}

static void clear_vis(void)
{
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            int t = cell[r][c].type;
            if (t == VISITED || t == FRONTIER || t == PATH)
                cell[r][c].type = EMPTY;
            cell[r][c].pr = cell[r][c].pc = -1;
            cell[r][c].g = 1e18;
            cell[r][c].closed = 0;
        }
    visited_cnt = path_len = 0;
    elapsed_ms = 0;
    algo = 0;
    mode = 0;
}

/* ---- Maze generation (recursive backtracker) ---- */

static void shuffle4(int *a)
{
    for (int i = 3; i > 0; i--) {
        int j = rand() % (i + 1);
        int t = a[i]; a[i] = a[j]; a[j] = t;
    }
}

static void carve(int r, int c)
{
    cell[r][c].type = EMPTY;
    int dirs[] = {0, 1, 2, 3};
    shuffle4(dirs);
    for (int d = 0; d < 4; d++) {
        int nr = r + 2 * DR[dirs[d]];
        int nc = c + 2 * DC[dirs[d]];
        if (nr > 0 && nr < rows - 1 && nc > 0 && nc < cols - 1
            && cell[nr][nc].type == WALL) {
            cell[r + DR[dirs[d]]][c + DC[dirs[d]]].type = EMPTY;
            carve(nr, nc);
        }
    }
}

static void make_maze(void)
{
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            cell[r][c].type = WALL;
            cell[r][c].g = 1e18;
            cell[r][c].pr = cell[r][c].pc = -1;
            cell[r][c].closed = 0;
        }
    carve(1, 1);

    /* Place start at top-left passage */
    sr = 1; sc = 1;
    cell[sr][sc].type = START;

    /* Find farthest open cell for end */
    int br = 1, bc = 1, bd = 0;
    for (int r = 1; r < rows - 1; r += 2)
        for (int c = 1; c < cols - 1; c += 2)
            if (cell[r][c].type == EMPTY && (r + c) > bd) {
                bd = r + c;
                br = r; bc = c;
            }
    er = br; ec = bc;
    cell[er][ec].type = END;
    cr = sr; cc = sc;
    visited_cnt = path_len = 0;
    elapsed_ms = 0;
    algo = 0; mode = 0;
}

/* ---- Heuristic: Manhattan distance ---- */

static double h_manhattan(int r1, int c1, int r2, int c2)
{
    return (double)(abs(r1 - r2) + abs(c1 - c2));
}

/* ---- Algorithm init & step ---- */

static struct timeval t0; /* algorithm start timestamp */

static void algo_start(int a)
{
    clear_vis();
    algo = a;
    mode = 1;
    visited_cnt = 0;

    cell[sr][sc].g = 0;
    cell[sr][sc].pr = cell[sr][sc].pc = -1;

    switch (a) {
    case 1: /* BFS */
        bfs_hd = bfs_tl = 0;
        bfs_q[bfs_tl++] = (Pos){sr, sc};
        cell[sr][sc].closed = 1;
        break;
    case 2: /* DFS */
        dfs_top = 0;
        dfs_stk[dfs_top++] = (Pos){sr, sc};
        cell[sr][sc].closed = 1;
        break;
    case 3: /* Dijkstra */
    case 4: /* A* */
        pq_n = 0;
        cell[sr][sc].g = 0;
        pq_push(sr, sc, (a == 4) ? h_manhattan(sr, sc, er, ec) : 0.0);
        break;
    }
    gettimeofday(&t0, NULL);
}

/* Returns: 0 = still running, 1 = path found, -1 = no path */
static int algo_step(void)
{
    /* BFS */
    if (algo == 1) {
        if (bfs_hd >= bfs_tl) return -1;
        Pos p = bfs_q[bfs_hd++];
        if (cell[p.r][p.c].type == END) return 1;
        if (cell[p.r][p.c].type != START)
            cell[p.r][p.c].type = VISITED;
        visited_cnt++;
        for (int d = 0; d < 4; d++) {
            int nr = p.r + DR[d], nc = p.c + DC[d];
            if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) continue;
            if (cell[nr][nc].type == WALL || cell[nr][nc].closed) continue;
            cell[nr][nc].closed = 1;
            cell[nr][nc].pr = p.r;
            cell[nr][nc].pc = p.c;
            if (cell[nr][nc].type != END)
                cell[nr][nc].type = FRONTIER;
            bfs_q[bfs_tl++] = (Pos){nr, nc};
        }
        return 0;
    }

    /* DFS */
    if (algo == 2) {
        if (dfs_top <= 0) return -1;
        Pos p = dfs_stk[--dfs_top];
        if (cell[p.r][p.c].type == END) return 1;
        if (cell[p.r][p.c].type != START)
            cell[p.r][p.c].type = VISITED;
        visited_cnt++;
        for (int d = 0; d < 4; d++) {
            int nr = p.r + DR[d], nc = p.c + DC[d];
            if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) continue;
            if (cell[nr][nc].type == WALL || cell[nr][nc].closed) continue;
            cell[nr][nc].closed = 1;
            cell[nr][nc].pr = p.r;
            cell[nr][nc].pc = p.c;
            if (cell[nr][nc].type != END)
                cell[nr][nc].type = FRONTIER;
            dfs_stk[dfs_top++] = (Pos){nr, nc};
        }
        return 0;
    }

    /* Dijkstra / A* */
    if (algo == 3 || algo == 4) {
        while (pq_n > 0) {
            PQNode top = pq_pop();
            if (cell[top.r][top.c].closed) continue; /* skip stale entry */
            cell[top.r][top.c].closed = 1;
            if (cell[top.r][top.c].type == END) return 1;
            if (cell[top.r][top.c].type != START)
                cell[top.r][top.c].type = VISITED;
            visited_cnt++;
            for (int d = 0; d < 4; d++) {
                int nr = top.r + DR[d], nc = top.c + DC[d];
                if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) continue;
                if (cell[nr][nc].type == WALL || cell[nr][nc].closed) continue;
                double ng = cell[top.r][top.c].g + 1.0;
                if (ng < cell[nr][nc].g) {
                    cell[nr][nc].g = ng;
                    cell[nr][nc].pr = top.r;
                    cell[nr][nc].pc = top.c;
                    double h = (algo == 4) ? h_manhattan(nr, nc, er, ec) : 0.0;
                    pq_push(nr, nc, ng + h);
                    if (cell[nr][nc].type != END)
                        cell[nr][nc].type = FRONTIER;
                }
            }
            return 0; /* one visual step per call */
        }
        return -1;
    }

    return -1;
}

/* Trace path from end back to start, marking cells */
static void trace_path(void)
{
    int r = er, c = ec;
    int len = 0;
    while (r >= 0 && c >= 0) {
        if (cell[r][c].type != START && cell[r][c].type != END)
            cell[r][c].type = PATH;
        int pr = cell[r][c].pr, pc = cell[r][c].pc;
        r = pr; c = pc;
        len++;
    }
    path_len = len;
}

/* ---- Screen drawing ---- */

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec - t0.tv_sec) * 1000.0 + (tv.tv_usec - t0.tv_usec) / 1000.0;
}

static void draw(void)
{
    static const char *aname[] = {"", "BFS", "DFS", "Dijkstra", "A*"};
    static const char *mname[] = {"EDIT", "RUNNING", "PAUSED", "DONE"};

    /* Top info bar */
    attron(A_BOLD);
    mvprintw(0, 0, " PATHFINDING VISUALIZER");
    clrtoeol();
    attroff(A_BOLD);

    double show_ms = (mode == 1) ? now_ms() : elapsed_ms;
    mvprintw(0, 24, "| Algo: %-9s | %s | Visited: %-5d | Path: %-4d | %.1f ms",
             aname[algo], mname[mode], visited_cnt, path_len, show_ms);

    /* Grid */
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int t = cell[r][c].type;
            int is_cur = (r == cr && c == cc && mode == 0);
            int ch, pair;

            switch (t) {
            case EMPTY:    ch = ' '; pair = 1; break;
            case WALL:     ch = ' '; pair = 2; break;
            case START:    ch = 'S'; pair = 3; break;
            case END:      ch = 'E'; pair = 4; break;
            case VISITED:  ch = '.'; pair = 5; break;
            case FRONTIER: ch = 'o'; pair = 6; break;
            case PATH:     ch = '*'; pair = 7; break;
            default:       ch = ' '; pair = 1;
            }

            if (is_cur) {
                attron(A_REVERSE | COLOR_PAIR(8));
                mvaddch(r + 1, c, '+');
                attroff(A_REVERSE | COLOR_PAIR(8));
            } else {
                attron(COLOR_PAIR(pair));
                mvaddch(r + 1, c, ch);
                attroff(COLOR_PAIR(pair));
            }
        }
    }

    /* Bottom controls bar */
    attron(A_BOLD);
    mvprintw(rows + 1, 0, " Keys:");
    attroff(A_BOLD);
    clrtoeol();
    mvprintw(rows + 1, 7,
        " Arrows=Move  Space=Wall/Pause  S/E=Start/End  M=Maze"
        "  1-4=Algo  +/-=Speed  R=Reset  C=Clear  Q=Quit");

    mvprintw(rows + 2, 0, " Speed: %d us/step  Grid: %dx%d  ",
             delay_us, rows, cols);

    refresh();
}

/* ---- Main ---- */

int main(void)
{
    srand((unsigned)time(NULL));

    /* ncurses setup */
    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0);
    nodelay(stdscr, TRUE);
    start_color();
    use_default_colors();

    /* Color palette */
    init_pair(1, COLOR_BLACK,  234);              /* EMPTY - dark gray */
    init_pair(2, COLOR_WHITE,  COLOR_WHITE);      /* WALL */
    init_pair(3, COLOR_BLACK,  COLOR_GREEN);      /* START */
    init_pair(4, COLOR_BLACK,  COLOR_RED);        /* END */
    init_pair(5, COLOR_WHITE,  COLOR_BLUE);       /* VISITED */
    init_pair(6, COLOR_BLACK,  COLOR_CYAN);       /* FRONTIER */
    init_pair(7, COLOR_BLACK,  COLOR_YELLOW);     /* PATH */
    init_pair(8, COLOR_BLACK,  COLOR_MAGENTA);    /* CURSOR */

    /* Size grid to terminal */
    int my, mx;
    getmaxyx(stdscr, my, mx);
    rows = my - 3;
    cols = mx;
    if (rows > MAXR) rows = MAXR;
    if (cols > MAXC) cols = MAXC;
    if (rows < 5) rows = 5;
    if (cols < 10) cols = 10;

    grid_init();

    int ch;
    int running = 1;

    while (running) {
        /* Animation step */
        if (mode == 1) {
            if (delay_us > 0) usleep((useconds_t)delay_us);

            /* Process a batch of steps for fast speeds */
            int batch = (delay_us == 0) ? 50 : 1;
            int res = 0;
            for (int i = 0; i < batch && res == 0; i++)
                res = algo_step();

            if (res == 1) {
                trace_path();
                mode = 3;
                elapsed_ms = now_ms();
            } else if (res == -1) {
                mode = 3;
                elapsed_ms = now_ms();
            }
        }

        draw();

        /* Input handling */
        ch = getch();
        if (ch == ERR) continue;

        switch (ch) {
        case 'q': case 27: /* Esc */
            running = 0;
            break;

        /* Cursor movement */
        case KEY_UP:
            if (cr > 0) cr--;
            break;
        case KEY_DOWN:
            if (cr < rows - 1) cr++;
            break;
        case KEY_LEFT:
            if (cc > 0) cc--;
            break;
        case KEY_RIGHT:
            if (cc < cols - 1) cc++;
            break;

        /* Toggle wall / Pause-Resume */
        case ' ':
            if (mode == 1) {
                mode = 2; /* pause */
            } else if (mode == 2) {
                mode = 1; /* resume */
            } else if (mode == 0) {
                if (cell[cr][cc].type == EMPTY)
                    cell[cr][cc].type = WALL;
                else if (cell[cr][cc].type == WALL)
                    cell[cr][cc].type = EMPTY;
            }
            break;

        /* Place start / end */
        case 's': case 'S':
            if (mode == 0) {
                cell[sr][sc].type = EMPTY;
                sr = cr; sc = cc;
                cell[sr][sc].type = START;
            }
            break;
        case 'e': case 'E':
            if (mode == 0) {
                cell[er][ec].type = EMPTY;
                er = cr; ec = cc;
                cell[er][ec].type = END;
            }
            break;

        /* Generate maze */
        case 'm': case 'M':
            if (mode == 0 || mode == 3)
                make_maze();
            break;

        /* Run algorithms */
        case '1': if (mode == 0 || mode == 3) algo_start(1); break;
        case '2': if (mode == 0 || mode == 3) algo_start(2); break;
        case '3': if (mode == 0 || mode == 3) algo_start(3); break;
        case '4': if (mode == 0 || mode == 3) algo_start(4); break;

        /* Speed control */
        case '+': case '=':
            delay_us -= 500;
            if (delay_us < 0) delay_us = 0;
            break;
        case '-': case '_':
            delay_us += 500;
            if (delay_us > 50000) delay_us = 50000;
            break;

        /* Single step when paused */
        case 'n': case 'N':
            if (mode == 2) {
                int res = algo_step();
                if (res == 1) {
                    trace_path();
                    mode = 3;
                    elapsed_ms = now_ms();
                } else if (res == -1) {
                    mode = 3;
                    elapsed_ms = now_ms();
                }
            }
            break;

        /* Reset / clear */
        case 'r': case 'R':
            grid_init();
            break;
        case 'c': case 'C':
            clear_vis();
            break;
        }
    }

    endwin();
    return 0;
}
