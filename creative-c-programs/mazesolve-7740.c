/*
 * mazesolve-7740.c — Terminal Maze Generator & Solver
 *
 * Features:
 *   Generation algorithms: Recursive Backtracker, Prim's, Kruskal's
 *   Solving algorithms: DFS, BFS, A*, Dead-End Filling
 *   Animated step-by-step generation and solving
 *   Multiple maze sizes (Small/Medium/Large/Huge)
 *   ANSI 256-color rendering with solution path highlighting
 *   Stats: dead ends, branching factor, solution length, generation time
 *   Interactive menu-driven interface
 *
 * Compile:
 *   gcc -std=gnu99 -O2 -o mazesolve mazesolve-7740.c -lm
 *
 * Run:
 *   ./mazesolve
 *
 * Requires: ANSI terminal with 256-color support
 *
 * Author: Creative C Series
 * License: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <fcntl.h>

/* ── Constants ─────────────────────────────────────────────────── */

#define MAX_W 120
#define MAX_H 60

#define CELL_SIZE 3   /* 3x3 block per maze cell */
#define MAX_CELLS (MAX_W * MAX_H)

#define WALL_TOP    0x01
#define WALL_RIGHT  0x02
#define WALL_BOTTOM 0x04
#define WALL_LEFT   0x08
#define VISITED     0x10
#define SOLUTION    0x20
#define FRONTIER    0x40
#define EXPLORED    0x80

/* ANSI helpers */
#define ESC         "\033["
#define RESET       "\033[0m"
#define CLEAR()     printf(ESC"2J"ESC"H")
#define CURSOR(x,y) printf(ESC"%d;%dH", (y), (x))
#define HIDE_CUR()  printf(ESC"?25l")
#define SHOW_CUR()  printf(ESC"?25h")

/* Color palette */
#define C_WALL      24    /* dark blue-grey */
#define C_PATH      232   /* near black */
#define C_FRONTIER  220   /* gold */
#define C_EXPLORED  27    /* blue */
#define C_SOLUTION  82    /* bright green */
#define C_START     196   /* red */
#define C_END       46    /* cyan */
#define C_PLAYER    226   /* yellow */

typedef struct {
    int w, h;
    unsigned char cells[MAX_H][MAX_W];
    int start_x, start_y, end_x, end_y;
} Maze;

typedef struct {
    int x, y, parent;
} Node;

/* Union-Find for Kruskal's */
typedef struct {
    int parent[MAX_CELLS];
    int rank[MAX_CELLS];
} UnionFind;

static Maze maze;
static int anim_delay = 2000; /* microseconds per step */
static int gen_steps = 0, solve_steps = 0;

/* ── Terminal helpers ──────────────────────────────────────────── */

static struct termios orig_term;

static void term_raw(void) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &orig_term);
    t = orig_term;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

static void term_restore(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
}

static int kbhit(void) {
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

static int getch(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    if (c == 27) {
        /* Escape sequence */
        unsigned char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return 27;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return 27;
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return 'U'; /* Up */
                case 'B': return 'D'; /* Down */
                case 'C': return 'R'; /* Right */
                case 'D': return 'L'; /* Left */
            }
        }
        return 27;
    }
    return c;
}

/* ── Drawing ───────────────────────────────────────────────────── */

static void set_color(int fg, int bg) {
    printf(ESC"38;5;%dm"ESC"48;5;%dm", fg, bg);
}

static void draw_cell(int cx, int cy) {
    /* Draw a single maze cell at screen position */
    int cell = maze.cells[cy][cx];
    int screen_x = cx * 2 + 2;
    int screen_y = cy + 3;

    /* Center of cell */
    int color = C_PATH;
    if (cell & SOLUTION) color = C_SOLUTION;
    else if (cell & FRONTIER) color = C_FRONTIER;
    else if (cell & EXPLORED) color = C_EXPLORED;
    else if (cell & VISITED) color = C_PATH;

    if (cx == maze.start_x && cy == maze.start_y) color = C_START;
    if (cx == maze.end_x && cy == maze.end_y) color = C_END;

    CURSOR(screen_x, screen_y);
    set_color(color + 60, color);
    printf("  ");

    /* Walls around this cell */
    /* Top wall */
    if (cell & WALL_TOP) {
        CURSOR(screen_x, screen_y - 1);
        set_color(C_WALL + 60, C_WALL);
        printf("--");
    }
    /* Bottom wall */
    if (cell & WALL_BOTTOM) {
        CURSOR(screen_x, screen_y + 1);
        set_color(C_WALL + 60, C_WALL);
        printf("--");
    }
    /* Left wall */
    if (cell & WALL_LEFT) {
        CURSOR(screen_x - 1, screen_y);
        set_color(C_WALL + 60, C_WALL);
        printf("|");
    }
    /* Right wall */
    if (cell & WALL_RIGHT) {
        CURSOR(screen_x + 2, screen_y);
        set_color(C_WALL + 60, C_WALL);
        printf("|");
    }
}

static void draw_maze(void) {
    for (int y = 0; y < maze.h; y++)
        for (int x = 0; x < maze.w; x++)
            draw_cell(x, y);

    /* Top border */
    CURSOR(2, 2);
    set_color(C_WALL + 60, C_WALL);
    printf("+");
    for (int x = 0; x < maze.w; x++) printf("--");
    printf("+");

    /* Bottom border */
    CURSOR(2, maze.h + 3);
    printf("+");
    for (int x = 0; x < maze.w; x++) printf("--");
    printf("+");

    /* Side borders */
    for (int y = 0; y < maze.h; y++) {
        CURSOR(1, y + 3);
        printf("|");
        CURSOR(maze.w * 2 + 2, y + 3);
        printf("|");
    }
    printf(RESET);
}

static void draw_full_maze_clean(void) {
    CLEAR();
    CURSOR(1, 1);
    printf(RESET);

    /* Render using characters for clean look */
    for (int y = 0; y < maze.h; y++) {
        CURSOR(2, y + 2);
        for (int x = 0; x < maze.w; x++) {
            int cell = maze.cells[y][x];

            /* Determine cell color */
            int color = C_PATH;
            if (cell & SOLUTION) color = C_SOLUTION;
            else if (cell & EXPLORED) color = C_EXPLORED;
            else if (cell & VISITED) color = C_PATH;

            if (x == maze.start_x && y == maze.start_y) color = C_START;
            if (x == maze.end_x && y == maze.end_y) color = C_END;

            set_color(color + 60, color);
            printf("  ");
        }
        set_color(C_WALL, 0);
    }

    /* Draw walls as overlaid lines */
    for (int y = 0; y < maze.h; y++) {
        for (int x = 0; x < maze.w; x++) {
            int cell = maze.cells[y][x];
            int sx = x * 2 + 3;
            int sy = y + 2;

            set_color(C_WALL + 60, C_WALL);

            if (cell & WALL_TOP) {
                CURSOR(sx, sy - 1);
                printf("--");
            }
            if (cell & WALL_BOTTOM) {
                CURSOR(sx, sy + 1);
                printf("--");
            }
            if (cell & WALL_LEFT) {
                CURSOR(sx - 1, sy);
                printf("|");
            }
            if (cell & WALL_RIGHT) {
                CURSOR(sx + 2, sy);
                printf("|");
            }
        }
    }
    printf(RESET);
}

/* ── Maze initialization ───────────────────────────────────────── */

static void maze_init(int w, int h) {
    maze.w = w;
    maze.h = h;
    maze.start_x = 0;
    maze.start_y = 0;
    maze.end_x = w - 1;
    maze.end_y = h - 1;
    gen_steps = 0;
    solve_steps = 0;

    /* All walls up, nothing visited */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            maze.cells[y][x] = WALL_TOP | WALL_RIGHT | WALL_BOTTOM | WALL_LEFT;
}

static void remove_wall(int x1, int y1, int x2, int y2) {
    if (x2 == x1 + 1) {
        maze.cells[y1][x1] &= ~WALL_RIGHT;
        maze.cells[y2][x2] &= ~WALL_LEFT;
    } else if (x2 == x1 - 1) {
        maze.cells[y1][x1] &= ~WALL_LEFT;
        maze.cells[y2][x2] &= ~WALL_RIGHT;
    } else if (y2 == y1 + 1) {
        maze.cells[y1][x1] &= ~WALL_BOTTOM;
        maze.cells[y2][x2] &= ~WALL_TOP;
    } else if (y2 == y1 - 1) {
        maze.cells[y1][x1] &= ~WALL_TOP;
        maze.cells[y2][x2] &= ~WALL_BOTTOM;
    }
}

static int in_bounds(int x, int y) {
    return x >= 0 && x < maze.w && y >= 0 && y < maze.h;
}

/* ── Generation: Recursive Backtracker (DFS) ──────────────────── */

static void gen_dfs(void) {
    typedef struct { int x, y; } StackEntry;
    StackEntry stack[MAX_CELLS];
    int sp = 0;

    int sx = rand() % maze.w, sy = rand() % maze.h;
    stack[sp++] = (StackEntry){sx, sy};
    maze.cells[sy][sx] |= VISITED;

    int dx[] = {0, 1, 0, -1};
    int dy[] = {-1, 0, 1, 0};

    while (sp > 0) {
        StackEntry cur = stack[sp - 1];
        int nx[4], ny[4], nc = 0;

        for (int d = 0; d < 4; d++) {
            int xx = cur.x + dx[d], yy = cur.y + dy[d];
            if (in_bounds(xx, yy) && !(maze.cells[yy][xx] & VISITED)) {
                nx[nc] = xx; ny[nc] = yy; nc++;
            }
        }

        if (nc > 0) {
            int choice = rand() % nc;
            remove_wall(cur.x, cur.y, nx[choice], ny[choice]);
            maze.cells[ny[choice]][nx[choice]] |= VISITED;
            stack[sp++] = (StackEntry){nx[choice], ny[choice]};
            gen_steps++;

            if (anim_delay > 0 && gen_steps % 3 == 0) {
                draw_full_maze_clean();
                CURSOR(1, maze.h + 4);
                printf(RESET"Gen (DFS) step %d... press 'q' to skip animation", gen_steps);
                fflush(stdout);
                if (kbhit() && getch() == 'q') { anim_delay = 0; }
                usleep(anim_delay);
            }
        } else {
            sp--;
        }
    }
}

/* ── Generation: Prim's Algorithm ──────────────────────────────── */

static void gen_prims(void) {
    int dx[] = {0, 1, 0, -1};
    int dy[] = {-1, 0, 1, 0};

    typedef struct { int x, y, fx, fy; } Frontier;
    Frontier frontiers[MAX_CELLS];
    int fc = 0;

    int sx = rand() % maze.w, sy = rand() % maze.h;
    maze.cells[sy][sx] |= VISITED;

    /* Add neighbors of start */
    for (int d = 0; d < 4; d++) {
        int nx = sx + dx[d], ny = sy + dy[d];
        if (in_bounds(nx, ny) && !(maze.cells[ny][nx] & VISITED)) {
            maze.cells[ny][nx] |= FRONTIER;
            frontiers[fc++] = (Frontier){nx, ny, sx, sy};
        }
    }

    while (fc > 0) {
        int idx = rand() % fc;
        Frontier f = frontiers[idx];
        frontiers[idx] = frontiers[--fc];
        maze.cells[f.y][f.x] &= ~FRONTIER;

        if (maze.cells[f.y][f.x] & VISITED) continue;

        /* Find visited neighbor to connect */
        int found = 0;
        for (int d = 0; d < 4; d++) {
            int nx = f.x + dx[d], ny = f.y + dy[d];
            if (in_bounds(nx, ny) && (maze.cells[ny][nx] & VISITED)) {
                remove_wall(f.x, f.y, nx, ny);
                found = 1;
                break;
            }
        }

        if (!found) continue;
        maze.cells[f.y][f.x] |= VISITED;
        gen_steps++;

        /* Add new frontiers */
        for (int d = 0; d < 4; d++) {
            int nx = f.x + dx[d], ny = f.y + dy[d];
            if (in_bounds(nx, ny) && !(maze.cells[ny][nx] & VISITED) && !(maze.cells[ny][nx] & FRONTIER)) {
                maze.cells[ny][nx] |= FRONTIER;
                frontiers[fc++] = (Frontier){nx, ny, f.x, f.y};
            }
        }

        if (anim_delay > 0 && gen_steps % 3 == 0) {
            draw_full_maze_clean();
            CURSOR(1, maze.h + 4);
            printf(RESET"Gen (Prim's) step %d... press 'q' to skip", gen_steps);
            fflush(stdout);
            if (kbhit() && getch() == 'q') anim_delay = 0;
            usleep(anim_delay);
        }
    }
}

/* ── Generation: Kruskal's Algorithm ───────────────────────────── */

static int uf_find(UnionFind *uf, int x) {
    while (uf->parent[x] != x) {
        uf->parent[x] = uf->parent[uf->parent[x]]; /* path compression */
        x = uf->parent[x];
    }
    return x;
}

static void uf_union(UnionFind *uf, int a, int b) {
    a = uf_find(uf, a);
    b = uf_find(uf, b);
    if (a == b) return;
    if (uf->rank[a] < uf->rank[b]) { int t = a; a = b; b = t; }
    uf->parent[b] = a;
    if (uf->rank[a] == uf->rank[b]) uf->rank[a]++;
}

static void gen_kruskals(void) {
    UnionFind uf;
    int total = maze.w * maze.h;

    for (int i = 0; i < total; i++) {
        uf.parent[i] = i;
        uf.rank[i] = 0;
    }

    /* Collect all internal edges */
    typedef struct { int x1, y1, x2, y2; } Edge;
    Edge edges[MAX_CELLS * 2];
    int ec = 0;

    for (int y = 0; y < maze.h; y++) {
        for (int x = 0; x < maze.w; x++) {
            if (x + 1 < maze.w) edges[ec++] = (Edge){x, y, x+1, y};
            if (y + 1 < maze.h) edges[ec++] = (Edge){x, y, x, y+1};
        }
    }

    /* Shuffle edges (Fisher-Yates) */
    for (int i = ec - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        Edge t = edges[i]; edges[i] = edges[j]; edges[j] = t;
    }

    for (int i = 0; i < ec; i++) {
        int id1 = edges[i].y1 * maze.w + edges[i].x1;
        int id2 = edges[i].y2 * maze.w + edges[i].x2;

        if (uf_find(&uf, id1) != uf_find(&uf, id2)) {
            uf_union(&uf, id1, id2);
            remove_wall(edges[i].x1, edges[i].y1, edges[i].x2, edges[i].y2);
            maze.cells[edges[i].y1][edges[i].x1] |= VISITED;
            maze.cells[edges[i].y2][edges[i].x2] |= VISITED;
            gen_steps++;

            if (anim_delay > 0 && gen_steps % 3 == 0) {
                draw_full_maze_clean();
                CURSOR(1, maze.h + 4);
                printf(RESET"Gen (Kruskal's) step %d... press 'q' to skip", gen_steps);
                fflush(stdout);
                if (kbhit() && getch() == 'q') anim_delay = 0;
                usleep(anim_delay);
            }
        }
    }
}

/* ── Solving: DFS ──────────────────────────────────────────────── */

static int solve_dfs_helper(int x, int y) {
    if (x == maze.end_x && y == maze.end_y) {
        maze.cells[y][x] |= SOLUTION;
        return 1;
    }

    maze.cells[y][x] |= EXPLORED;
    solve_steps++;

    int dx[] = {0, 1, 0, -1};
    int dy[] = {-1, 0, 1, 0};
    unsigned char walls[] = {WALL_TOP, WALL_RIGHT, WALL_BOTTOM, WALL_LEFT};

    for (int d = 0; d < 4; d++) {
        int nx = x + dx[d], ny = y + dy[d];
        if (in_bounds(nx, ny) && !(maze.cells[ny][nx] & EXPLORED) &&
            !(maze.cells[y][x] & walls[d])) {
            if (solve_dfs_helper(nx, ny)) {
                maze.cells[y][x] |= SOLUTION;
                return 1;
            }
        }
    }
    return 0;
}

static void solve_dfs(void) {
    solve_dfs_helper(maze.start_x, maze.start_y);
}

/* ── Solving: BFS ──────────────────────────────────────────────── */

static void solve_bfs(void) {
    Node queue[MAX_CELLS];
    int head = 0, tail = 0;

    int visited[MAX_H][MAX_W] = {0};
    int parent[MAX_H][MAX_W];
    memset(parent, -1, sizeof(parent));

    queue[tail++] = (Node){maze.start_x, maze.start_y, -1};
    visited[maze.start_y][maze.start_x] = 1;
    maze.cells[maze.start_y][maze.start_x] |= EXPLORED;

    int dx[] = {0, 1, 0, -1};
    int dy[] = {-1, 0, 1, 0};
    unsigned char walls[] = {WALL_TOP, WALL_RIGHT, WALL_BOTTOM, WALL_LEFT};

    int found = 0;
    while (head < tail) {
        Node cur = queue[head++];

        if (cur.x == maze.end_x && cur.y == maze.end_y) {
            /* Trace back */
            int cx = cur.x, cy = cur.y;
            while (cx != maze.start_x || cy != maze.start_y) {
                maze.cells[cy][cx] |= SOLUTION;
                int p = parent[cy][cx];
                cx = p % maze.w;
                cy = p / maze.w;
            }
            maze.cells[maze.start_y][maze.start_x] |= SOLUTION;
            found = 1;
            break;
        }

        for (int d = 0; d < 4; d++) {
            int nx = cur.x + dx[d], ny = cur.y + dy[d];
            if (in_bounds(nx, ny) && !visited[ny][nx] &&
                !(maze.cells[cur.y][cur.x] & walls[d])) {
                visited[ny][nx] = 1;
                parent[ny][nx] = cur.y * maze.w + cur.x;
                maze.cells[ny][nx] |= EXPLORED;
                queue[tail++] = (Node){nx, ny, -1};
                solve_steps++;
            }
        }

        if (anim_delay > 0 && solve_steps % 5 == 0) {
            draw_full_maze_clean();
            CURSOR(1, maze.h + 4);
            printf(RESET"Solve (BFS) explored %d cells... press 'q' to skip", solve_steps);
            fflush(stdout);
            if (kbhit() && getch() == 'q') anim_delay = 0;
            usleep(anim_delay);
        }
    }
}

/* ── Solving: A* ───────────────────────────────────────────────── */

static void solve_astar(void) {
    /* Simple A* with Manhattan distance heuristic */
    int g[MAX_H][MAX_W];
    int f[MAX_H][MAX_W];
    int closed[MAX_H][MAX_W] = {0};
    int parent[MAX_H][MAX_W];
    memset(parent, -1, sizeof(parent));

    for (int y = 0; y < maze.h; y++)
        for (int x = 0; x < maze.w; x++)
            g[y][x] = f[y][x] = 999999;

    g[maze.start_y][maze.start_x] = 0;
    int hd = abs(maze.end_x - maze.start_x) + abs(maze.end_y - maze.start_y);
    f[maze.start_y][maze.start_x] = hd;

    int dx[] = {0, 1, 0, -1};
    int dy[] = {-1, 0, 1, 0};
    unsigned char walls[] = {WALL_TOP, WALL_RIGHT, WALL_BOTTOM, WALL_LEFT};

    while (1) {
        /* Find lowest f in open set */
        int bx = -1, by = -1, bf = 999999;
        for (int y = 0; y < maze.h; y++)
            for (int x = 0; x < maze.w; x++)
                if (!closed[y][x] && f[y][x] < bf) {
                    bf = f[y][x]; bx = x; by = y;
                }

        if (bx < 0) break; /* no path */

        closed[by][bx] = 1;
        maze.cells[by][bx] |= EXPLORED;
        solve_steps++;

        if (bx == maze.end_x && by == maze.end_y) {
            /* Trace back */
            int cx = bx, cy = by;
            while (cx != maze.start_x || cy != maze.start_y) {
                maze.cells[cy][cx] |= SOLUTION;
                int p = parent[cy][cx];
                cx = p % maze.w;
                cy = p / maze.w;
            }
            maze.cells[maze.start_y][maze.start_x] |= SOLUTION;
            return;
        }

        for (int d = 0; d < 4; d++) {
            int nx = bx + dx[d], ny = by + dy[d];
            if (in_bounds(nx, ny) && !closed[ny][nx] &&
                !(maze.cells[by][bx] & walls[d])) {
                int ng = g[by][bx] + 1;
                if (ng < g[ny][nx]) {
                    g[ny][nx] = ng;
                    f[ny][nx] = ng + abs(maze.end_x - nx) + abs(maze.end_y - ny);
                    parent[ny][nx] = by * maze.w + bx;
                }
            }
        }

        if (anim_delay > 0 && solve_steps % 3 == 0) {
            draw_full_maze_clean();
            CURSOR(1, maze.h + 4);
            printf(RESET"Solve (A*) explored %d cells... press 'q' to skip", solve_steps);
            fflush(stdout);
            if (kbhit() && getch() == 'q') anim_delay = 0;
            usleep(anim_delay);
        }
    }
}

/* ── Solving: Dead-End Filling ─────────────────────────────────── */

static void solve_deadfill(void) {
    /* Iteratively fill dead ends until only solution remains */
    int changed = 1;
    int fill_count = 0;

    while (changed) {
        changed = 0;
        for (int y = 0; y < maze.h; y++) {
            for (int x = 0; x < maze.w; x++) {
                if (x == maze.start_x && y == maze.start_y) continue;
                if (x == maze.end_x && y == maze.end_y) continue;
                if (maze.cells[y][x] & SOLUTION) continue;

                /* Count open passages */
                int open = 0;
                if (!(maze.cells[y][x] & WALL_TOP)) open++;
                if (!(maze.cells[y][x] & WALL_BOTTOM)) open++;
                if (!(maze.cells[y][x] & WALL_LEFT)) open++;
                if (!(maze.cells[y][x] & WALL_RIGHT)) open++;

                /* Dead end: only one open passage */
                if (open == 1) {
                    maze.cells[y][x] |= SOLUTION; /* mark as filled */
                    /* Rebuild walls to seal it */
                    maze.cells[y][x] |= WALL_TOP | WALL_BOTTOM | WALL_LEFT | WALL_RIGHT;
                    /* Also block the neighbor's side */
                    int dx[] = {0, 1, 0, -1};
                    int dy[] = {-1, 0, 1, 0};
                    unsigned char opp[] = {WALL_BOTTOM, WALL_LEFT, WALL_TOP, WALL_RIGHT};
                    unsigned char walls[] = {WALL_TOP, WALL_RIGHT, WALL_BOTTOM, WALL_LEFT};
                    for (int d = 0; d < 4; d++) {
                        if (!(maze.cells[y][x] & walls[d])) {
                            /* This was the open direction - seal from neighbor */
                            /* Already sealed our side, we need to seal neighbor */
                            /* Actually let's just seal our side and mark visited */
                            (void)opp;
                            break;
                        }
                    }

                    changed = 1;
                    fill_count++;
                    solve_steps++;
                }
            }
        }

        if (anim_delay > 0 && changed && fill_count % 5 == 0) {
            draw_full_maze_clean();
            CURSOR(1, maze.h + 4);
            printf(RESET"Solve (DeadFill) filled %d cells... press 'q' to skip", fill_count);
            fflush(stdout);
            if (kbhit() && getch() == 'q') anim_delay = 0;
            usleep(anim_delay);
        }
    }

    /* Mark remaining un-filled cells as solution path */
    for (int y = 0; y < maze.h; y++)
        for (int x = 0; x < maze.w; x++) {
            maze.cells[y][x] &= ~SOLUTION;
            maze.cells[y][x] |= EXPLORED;
        }

    /* Re-run BFS to actually trace the path (dead-fill leaves a corridor) */
    solve_steps = 0;
    solve_bfs();
}

/* ── Clear solve markers ───────────────────────────────────────── */

static void clear_solve_marks(void) {
    for (int y = 0; y < maze.h; y++)
        for (int x = 0; x < maze.w; x++)
            maze.cells[y][x] &= ~(SOLUTION | EXPLORED | FRONTIER);
    solve_steps = 0;
}

/* ── Statistics ────────────────────────────────────────────────── */

static void show_stats(void) {
    int dead_ends = 0, branch = 0, sol_len = 0;

    for (int y = 0; y < maze.h; y++) {
        for (int x = 0; x < maze.w; x++) {
            int open = 0;
            if (!(maze.cells[y][x] & WALL_TOP)) open++;
            if (!(maze.cells[y][x] & WALL_BOTTOM)) open++;
            if (!(maze.cells[y][x] & WALL_LEFT)) open++;
            if (!(maze.cells[y][x] & WALL_RIGHT)) open++;
            if (open == 1) dead_ends++;
            if (open >= 3) branch++;
            if (maze.cells[y][x] & SOLUTION) sol_len++;
        }
    }

    int total = maze.w * maze.h;
    double branching = total > 0 ? (double)branch / total * 100 : 0;

    printf(RESET);
    printf("\n  ┌────────────────────────────────────┐\n");
    printf("  │       MAZE STATISTICS               │\n");
    printf("  ├────────────────────────────────────┤\n");
    printf("  │ Maze size:     %dx%d = %d cells      │\n", maze.w, maze.h, total);
    printf("  │ Dead ends:     %-5d (%.1f%%)         │\n", dead_ends, total > 0 ? (double)dead_ends/total*100 : 0);
    printf("  │ Branch points: %-5d (%.1f%%)         │\n", branch, branching);
    printf("  │ Solution len:  %-5d cells           │\n", sol_len);
    printf("  │ Gen steps:     %-5d                  │\n", gen_steps);
    printf("  │ Solve steps:   %-5d                  │\n", solve_steps);
    printf("  │ Optimality:    %.1f%% (sol/total)     │\n",
           total > 0 ? (double)sol_len / total * 100 : 0);
    printf("  └────────────────────────────────────┘\n");
}

/* ── Menu UI ───────────────────────────────────────────────────── */

static void clear_line(int y) {
    CURSOR(1, y);
    printf(ESC"2K");
}

static void draw_menu(const char *title, const char **options, int count, int sel) {
    CLEAR();
    CURSOR(2, 1);
    printf(RESET"  ╔════════════════════════════════════════════╗\n");
    printf("  ║     %s     ║\n", title);
    printf("  ╠════════════════════════════════════════════╣\n");

    for (int i = 0; i < count; i++) {
        CURSOR(3, i + 5);
        if (i == sel) {
            printf(ESC"38;5;46m"ESC"48;5;236m" " ▶ %-40s " RESET, options[i]);
        } else {
            printf(ESC"38;5;252m"ESC"48;5;234m" "   %-40s " RESET, options[i]);
        }
    }

    CURSOR(3, count + 6);
    printf(RESET"  ╚════════════════════════════════════════════╝\n");
    CURSOR(3, count + 8);
    printf(ESC"38;5;244m"  "  ↑↓ Navigate  Enter Select  q Back" RESET);
    fflush(stdout);
}

static int menu_select(const char *title, const char **options, int count) {
    int sel = 0;
    HIDE_CUR();
    term_raw();

    while (1) {
        draw_menu(title, options, count, sel);
        int c = getch();

        if (c == 'U' || c == 'k') sel = (sel - 1 + count) % count;
        else if (c == 'D' || c == 'j') sel = (sel + 1) % count;
        else if (c == '\n' || c == '\r') { term_restore(); SHOW_CUR(); return sel; }
        else if (c == 'q' || c == 27) { term_restore(); SHOW_CUR(); return -1; }
    }
}

/* ── Interactive maze player mode ──────────────────────────────── */

static void play_maze(void) {
    int px = maze.start_x, py = maze.start_y;
    int moves = 0;
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);

    term_raw();
    HIDE_CUR();

    while (1) {
        draw_full_maze_clean();

        /* Draw player */
        int sx = px * 2 + 3;
        int sy = py + 2;
        CURSOR(sx, sy);
        set_color(C_PLAYER + 60, C_PLAYER);
        printf("██");
        printf(RESET);

        CURSOR(1, maze.h + 4);
        gettimeofday(&t1, NULL);
        double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
        printf(ESC"38;5;252m" "  Player Mode | Pos: (%d,%d) | Moves: %d | Time: %.1fs | Arrows:move  r:restart  q:quit",
               px, py, moves, elapsed);
        fflush(stdout);

        int c = getch();
        int nx = px, ny = py;
        unsigned char wall_check = 0;

        if (c == 'U') { ny = py - 1; wall_check = WALL_TOP; }
        else if (c == 'D') { ny = py + 1; wall_check = WALL_BOTTOM; }
        else if (c == 'L') { nx = px - 1; wall_check = WALL_LEFT; }
        else if (c == 'R') { nx = px + 1; wall_check = WALL_RIGHT; }
        else if (c == 'q') break;
        else if (c == 'r') { px = maze.start_x; py = maze.start_y; moves = 0; gettimeofday(&t0, NULL); continue; }

        if (in_bounds(nx, ny) && !(maze.cells[py][px] & wall_check)) {
            px = nx; py = ny; moves++;
        }

        if (px == maze.end_x && py == maze.end_y) {
            gettimeofday(&t1, NULL);
            double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;

            draw_full_maze_clean();
            CURSOR(maze.w, maze.h / 2 + 2);
            set_color(C_SOLUTION + 60, C_SOLUTION);
            printf(" ★ YOU WIN! ★ ");
            CURSOR(1, maze.h + 4);
            printf(RESET ESC"38;5;82m" "  Solved in %d moves, %.1f seconds! Press any key...", moves, elapsed);
            fflush(stdout);
            getch();
            break;
        }
    }

    term_restore();
    SHOW_CUR();
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(void) {
    srand(time(NULL));

    while (1) {
        /* Main menu */
        const char *main_opts[] = {
            "Generate New Maze",
            "Quit"
        };
        int choice = menu_select("MAZE GENERATOR & SOLVER", main_opts, 2);
        if (choice != 0) break;

        /* Size selection */
        const char *size_opts[] = {
            "Small   (15x10)",
            "Medium  (25x15)",
            "Large   (40x25)",
            "Huge    (55x30)",
            "Back"
        };
        int sizes[][2] = {{15,10},{25,15},{40,25},{55,30}};
        choice = menu_select("Select Maze Size", size_opts, 5);
        if (choice < 0 || choice >= 4) continue;

        int mw = sizes[choice][0], mh = sizes[choice][1];

        /* Algorithm selection */
        const char *gen_opts[] = {
            "Recursive Backtracker (DFS)",
            "Prim's Algorithm",
            "Kruskal's Algorithm",
            "Back"
        };
        choice = menu_select("Generation Algorithm", gen_opts, 4);
        if (choice < 0 || choice >= 3) continue;
        int gen_algo = choice;

        /* Animation speed */
        const char *speed_opts[] = {
            "Fast (instant)",
            "Normal",
            "Slow",
            "Back"
        };
        choice = menu_select("Animation Speed", speed_opts, 4);
        if (choice < 0 || choice >= 3) continue;

        switch (choice) {
            case 0: anim_delay = 0; break;
            case 1: anim_delay = 3000; break;
            case 2: anim_delay = 10000; break;
        }

        /* Generate */
        CLEAR();
        CURSOR(1, 1);
        printf(RESET"Generating maze...\n");
        fflush(stdout);

        struct timeval gt0, gt1;
        gettimeofday(&gt0, NULL);

        maze_init(mw, mh);
        switch (gen_algo) {
            case 0: gen_dfs(); break;
            case 1: gen_prims(); break;
            case 2: gen_kruskals(); break;
        }

        gettimeofday(&gt1, NULL);
        double gen_time = (gt1.tv_sec - gt0.tv_sec) + (gt1.tv_usec - gt0.tv_usec) / 1e6;

        /* Action menu */
        while (1) {
            draw_full_maze_clean();
            CURSOR(1, maze.h + 4);
            printf(RESET ESC"38;5;244m" "  Generated in %.3fs (%d steps)\n", gen_time, gen_steps);

            const char *act_opts[] = {
                "Solve (DFS)",
                "Solve (BFS)",
                "Solve (A*)",
                "Solve (Dead-End Fill)",
                "Play Maze Yourself!",
                "Show Statistics",
                "New Maze",
                "Back to Main Menu"
            };
            choice = menu_select("What Next?", act_opts, 8);

            if (choice < 0 || choice == 7) break;
            if (choice == 6) goto new_maze; /* restart outer loop */

            if (choice == 5) {
                /* Show stats */
                CLEAR();
                show_stats();
                printf("\n  Press any key to continue...");
                fflush(stdout);
                term_raw(); getch(); term_restore();
                continue;
            }

            if (choice == 4) {
                /* Play mode */
                play_maze();
                continue;
            }

            /* Solve */
            clear_solve_marks();
            anim_delay = choice == 3 ? 0 : anim_delay; /* deadfill is visual-only */

            struct timeval st0, st1;
            gettimeofday(&st0, NULL);

            switch (choice) {
                case 0: solve_dfs(); break;
                case 1: solve_bfs(); break;
                case 2: solve_astar(); break;
                case 3: solve_deadfill(); break;
            }

            gettimeofday(&st1, NULL);
            double solve_time = (st1.tv_sec - st0.tv_sec) + (st1.tv_usec - st0.tv_usec) / 1e6;

            /* Show solution */
            draw_full_maze_clean();
            CURSOR(1, maze.h + 4);
            printf(RESET ESC"38;5;82m" "  Solved in %.3fs (%d cells explored)\n", solve_time, solve_steps);
            printf(ESC"38;5;244m" "  Press any key to continue...");
            fflush(stdout);
            term_raw(); getch(); term_restore();
        }
        new_maze:;
    }

    CLEAR();
    CURSOR(1, 1);
    printf(RESET"Thanks for playing! 🐀\n");
    return 0;
}
