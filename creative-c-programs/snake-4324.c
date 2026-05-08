/*
 * ================================================================
 *  SNAKE — Terminal Snake Game with AI  (snake-4324.c)
 * ================================================================
 *
 *  Classic Snake with both human and AI play modes.
 *
 *  AI Strategy (3-tier):
 *    1. BFS to food — if path exists AND next cell has enough
 *       reachable space (flood-fill >= snake length), go for it.
 *    2. Chase own tail (BFS to tail) to survive and buy time.
 *    3. Fallback: pick the direction with the most open space.
 *
 *  Build:   gcc -O2 -o snake snake-4324.c
 *  Run:     ./snake
 *
 *  Controls (in-game):
 *    WASD / Arrow Keys   Move snake (Human mode)
 *    Space               Toggle Human / AI mode
 *    +  /  -             Speed up / slow down
 *    P                   Pause / resume
 *    R                   Restart game
 *    Q / Esc             Quit
 *
 *  Requires: ANSI/VT100 terminal with 256-color support.
 * ================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <stdarg.h>

/* ── Constants ────────────────────────────────────────────────── */

#define W          32        /* play-area width  */
#define H          20        /* play-area height */
#define MAX_SNAKE  (W * H)
#define INIT_LEN   4
#define MAX_SPEED  10
#define MIN_SPEED  1

/* ANSI helpers */
#define ESC_RESET   "\033[0m"
#define ESC_BOLD    "\033[1m"
#define ESC_DIM     "\033[2m"
#define ESC_HIDE    "\033[?25l"
#define ESC_SHOW    "\033[?25h"
#define ALT_ON      "\033[?1049h"
#define ALT_OFF     "\033[?1049l"

/* 256-color palette */
#define C_BG        "\033[48;5;234m"   /* dark gray bg */
#define C_BORDER    "\033[38;5;240m"
#define C_HEAD      "\033[38;5;118m"   /* bright green */
#define C_BODY_A    "\033[38;5;76m"    /* green */
#define C_BODY_B    "\033[38;5;70m"    /* darker green */
#define C_FOOD_A    "\033[38;5;196m"   /* bright red */
#define C_FOOD_B    "\033[38;5;202m"   /* orange-red (blink) */
#define C_DOT       "\033[38;5;238m"   /* faint grid dot */
#define C_TITLE     "\033[38;5;82m"
#define C_SCORE     "\033[38;5;220m"   /* yellow */
#define C_LABEL     "\033[38;5;246m"
#define C_AI_ON     "\033[38;5;81m"    /* cyan */
#define C_WARN      "\033[38;5;214m"
#define C_DEATH     "\033[38;5;160m"

/* ── Types ────────────────────────────────────────────────────── */

typedef struct { int x, y; } Pos;
typedef enum   { DIR_UP=0, DIR_RIGHT, DIR_DOWN, DIR_LEFT } Dir;
typedef enum   { ST_TITLE, ST_PLAY, ST_PAUSE, ST_OVER } State;

typedef struct {
    Pos  body[MAX_SNAKE];
    int  len;
    Dir  dir;
} Snake;

typedef struct {
    Snake snake;
    Pos   food;
    int   score, best, speed, eaten;
    int   ai_mode;          /* 0 = human, 1 = AI */
    State state;
    int   tick;
    int   win;              /* 1 if board completely filled */
} Game;

/* ── Globals ──────────────────────────────────────────────────── */

static Game            g;
static struct termios  orig_term;
static volatile int    running = 1;

static const int DX[] = { 0, 1, 0, -1 };
static const int DY[] = { -1, 0, 1,  0 };

/* Screen buffer — write everything here, flush once per frame */
static char sbuf[8192];
static int  spos;

/* ── Terminal I/O ─────────────────────────────────────────────── */

static void term_raw(void) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &orig_term);
    t = orig_term;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
}

static void term_restore(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_term);
}

static void sig_handler(int s) { (void)s; running = 0; }

static void scr_init(void) {
    spos = 0;
    sbuf[0] = '\0';
}

static void scr(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    spos += vsnprintf(sbuf + spos, sizeof(sbuf) - (size_t)spos, fmt, ap);
    va_end(ap);
}

static void scr_flush(void) {
    sbuf[spos] = '\0';
    write(STDOUT_FILENO, sbuf, spos);
    spos = 0;
}

/* ── Utility ──────────────────────────────────────────────────── */

static int in_bounds(int x, int y) {
    return x >= 0 && x < W && y >= 0 && y < H;
}

static int pos_eq(Pos a, Pos b) { return a.x == b.x && a.y == b.y; }

static Dir opposite(Dir d) { return (Dir)((d + 2) % 4); }

/* ── Game Logic ───────────────────────────────────────────────── */

static int is_snake(int x, int y) {
    for (int i = 0; i < g.snake.len; i++)
        if (g.snake.body[i].x == x && g.snake.body[i].y == y) return 1;
    return 0;
}

static void spawn_food(void) {
    int empty[MAX_SNAKE], n = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (!is_snake(x, y)) empty[n++] = y * W + x;
    if (n == 0) { g.win = 1; return; }
    int idx = empty[rand() % n];
    g.food.x = idx % W;
    g.food.y = idx / W;
}

static void game_init(void) {
    memset(&g, 0, sizeof(g));
    g.snake.len = INIT_LEN;
    g.snake.dir = DIR_RIGHT;
    g.speed     = 5;
    g.state     = ST_TITLE;
    int sx = W / 2, sy = H / 2;
    for (int i = 0; i < g.snake.len; i++) {
        g.snake.body[i].x = sx - i;
        g.snake.body[i].y = sy;
    }
    spawn_food();
}

static void game_reset(void) {
    int old_best = g.best;
    int old_ai   = g.ai_mode;
    int old_spd  = g.speed;
    game_init();
    g.best    = old_best;
    g.ai_mode = old_ai;
    g.speed   = old_spd;
    g.state   = ST_PLAY;
}

/* Move the snake one step.  Returns 1 on success, 0 on collision. */
static int move_snake(void) {
    Pos head = g.snake.body[0];
    Pos nh   = { head.x + DX[g.snake.dir], head.y + DY[g.snake.dir] };

    if (!in_bounds(nh.x, nh.y)) return 0;

    int ate = pos_eq(nh, g.food);
    for (int i = 0; i < g.snake.len - (ate ? 0 : 1); i++)
        if (pos_eq(g.snake.body[i], nh)) return 0;

    memmove(&g.snake.body[1], &g.snake.body[0],
            sizeof(Pos) * (g.snake.len - 1));
    g.snake.body[0] = nh;

    if (ate) {
        g.snake.len++;
        g.score += 10 + g.speed * 2;
        g.eaten++;
        if (g.score > g.best) g.best = g.score;
        if (g.snake.len >= W * H) { g.win = 1; return 1; }
        spawn_food();
    }
    return 1;
}

/* ── AI ───────────────────────────────────────────────────────── */

/* BFS from start toward goal, avoiding snake body.
   ignore_tail = count of tail segments to treat as free (0 or 1).
   On success *out gets the first-move direction. */
static int bfs(Pos start, Pos goal, int ignore_tail, Dir *out) {
    typedef struct { int x, y; Dir first; } QN;
    QN q[MAX_SNAKE];
    int vis[H][W];
    int front = 0, back = 0;

    memset(vis, 0, sizeof(vis));
    for (int i = 0; i < g.snake.len - ignore_tail; i++)
        vis[g.snake.body[i].y][g.snake.body[i].x] = 1;
    vis[start.y][start.x] = 0;

    for (Dir d = 0; d < 4; d++) {
        int nx = start.x + DX[d], ny = start.y + DY[d];
        if (!in_bounds(nx, ny) || vis[ny][nx]) continue;
        if (nx == goal.x && ny == goal.y) { *out = d; return 1; }
        vis[ny][nx] = 1;
        q[back++] = (QN){ nx, ny, d };
    }

    while (front < back) {
        QN c = q[front++];
        for (Dir d = 0; d < 4; d++) {
            int nx = c.x + DX[d], ny = c.y + DY[d];
            if (!in_bounds(nx, ny) || vis[ny][nx]) continue;
            if (nx == goal.x && ny == goal.y) { *out = c.first; return 1; }
            vis[ny][nx] = 1;
            q[back++] = (QN){ nx, ny, c.first };
        }
    }
    return 0;
}

/* Count reachable empty cells from (sx,sy) via flood fill. */
static int flood(int sx, int sy) {
    if (!in_bounds(sx, sy) || is_snake(sx, sy)) return 0;
    Pos stk[MAX_SNAKE];
    int vis[H][W];
    int top = 0, cnt = 0;
    memset(vis, 0, sizeof(vis));
    stk[top++] = (Pos){ sx, sy };
    vis[sy][sx] = 1;
    while (top > 0) {
        Pos c = stk[--top]; cnt++;
        for (int d = 0; d < 4; d++) {
            int nx = c.x + DX[d], ny = c.y + DY[d];
            if (in_bounds(nx, ny) && !vis[ny][nx] && !is_snake(nx, ny)) {
                vis[ny][nx] = 1;
                stk[top++] = (Pos){ nx, ny };
            }
        }
    }
    return cnt;
}

/* Pick best direction for the AI this tick. */
static Dir ai_decide(void) {
    Pos head = g.snake.body[0];
    Dir chosen = g.snake.dir;

    /* --- Tier 1: BFS to food, if safe --- */
    Dir to_food;
    if (bfs(head, g.food, 0, &to_food)) {
        Pos nh = { head.x + DX[to_food], head.y + DY[to_food] };
        int space = flood(nh.x, nh.y);
        if (space >= g.snake.len || pos_eq(nh, g.food))
            chosen = to_food;
        else
            goto tier2;
    } else {
tier2:
        /* --- Tier 2: Chase tail --- */
        Dir to_tail;
        Pos tail = g.snake.body[g.snake.len - 1];
        if (bfs(head, tail, 1, &to_tail))
            chosen = to_tail;
    }

    /* --- Validate chosen direction --- */
    Pos nh = { head.x + DX[chosen], head.y + DY[chosen] };
    if (!in_bounds(nh.x, nh.y) || is_snake(nh.x, nh.y) ||
        chosen == opposite(g.snake.dir)) {
        /* --- Tier 3: Pick safest neighbor --- */
        int best_space = -1;
        for (Dir d = 0; d < 4; d++) {
            if (d == opposite(g.snake.dir)) continue;
            int nx = head.x + DX[d], ny = head.y + DY[d];
            if (!in_bounds(nx, ny) || is_snake(nx, ny)) continue;
            int sp = flood(nx, ny);
            if (sp > best_space) { best_space = sp; chosen = d; }
        }
    }
    return chosen;
}

/* ── Rendering ────────────────────────────────────────────────── */

static void draw_hline(const char *left, const char *mid, const char *right) {
    scr(C_BORDER);
    scr(left);
    for (int i = 0; i < W; i++) scr(mid);
    scr(right);
    scr(ESC_RESET "\n");
}

static void draw_play_area(void) {
    /* Build segment-index map */
    int si[H][W];
    memset(si, -1, sizeof(si));
    for (int i = 0; i < g.snake.len; i++)
        si[g.snake.body[i].y][g.snake.body[i].x] = i;

    int food_blink = (g.tick / 3) % 2;

    for (int y = 0; y < H; y++) {
        scr(C_BORDER " │" ESC_RESET);
        scr(C_BG);
        for (int x = 0; x < W; x++) {
            if (si[y][x] == 0) {
                /* Head */
                scr(ESC_BOLD C_HEAD "◉");   /* ◉ */
            } else if (si[y][x] > 0) {
                /* Body — alternating shade */
                int t = si[y][x] * 100 / g.snake.len;
                if (t < 33)      scr(C_BODY_A "█");
                else if (t < 66) scr(C_BODY_B "█");
                else             scr("\033[38;5;64m█");
            } else if (x == g.food.x && y == g.food.y) {
                scr(ESC_BOLD);
                scr(food_blink ? C_FOOD_A : C_FOOD_B);
                scr("♥");   /* ♥ */
            } else {
                scr(C_DOT " ·");  /* · */
            }
        }
        scr(ESC_RESET);
        scr(C_BORDER "│" ESC_RESET);

        /* Info panel alongside */
        switch (y) {
        case 1:  scr("  " ESC_BOLD C_TITLE "S N A K E" ESC_RESET); break;
        case 2:  scr("  " C_LABEL "Terminal Edition + AI"); break;
        case 4:  scr("  " C_LABEL "Score  " ESC_BOLD C_SCORE "%-6d" ESC_RESET, g.score); break;
        case 5:  scr("  " C_LABEL "Best   " ESC_BOLD C_SCORE "%-6d" ESC_RESET, g.best); break;
        case 6:  scr("  " C_LABEL "Speed  " C_SCORE "%-6d" ESC_RESET, g.speed); break;
        case 7:  scr("  " C_LABEL "Length " C_SCORE "%-6d" ESC_RESET, g.snake.len); break;
        case 8:  scr("  " C_LABEL "Food   " C_SCORE "%-6d" ESC_RESET, g.eaten); break;
        case 10: {
            if (g.ai_mode)
                scr("  " C_LABEL "Mode   " ESC_BOLD C_AI_ON "AI ●" ESC_RESET);
            else
                scr("  " C_LABEL "Mode   " ESC_BOLD C_HEAD "HUMAN" ESC_RESET);
            break;
        }
        case 12: scr("  " ESC_DIM "WASD/Arrows Move"); break;
        case 13: scr("  " ESC_DIM "Space  AI on/off"); break;
        case 14: scr("  " ESC_DIM "+/-    Speed");     break;
        case 15: scr("  " ESC_DIM "P      Pause");     break;
        case 16: scr("  " ESC_DIM "R      Restart");   break;
        case 17: scr("  " ESC_DIM "Q/Esc  Quit");      break;
        default: break;
        }
        scr("\n");
    }
}

static void render_title(void) {
    scr(ESC_HIDE "\033[2J\033[H");
    scr("\n\n\n");
    scr("        " ESC_BOLD C_TITLE);
    scr("  ____   _   _    _    _   _  ____  ____\n");
    scr("        / ___| | \\ | |  / \\  | \\ | |/ ___||  _ \\\n");
    scr("        \\___ \\ |  \\| | / _ \\ |  \\| | |  _ | |_) |\n");
    scr("         ___) || |\\  ||/ ___ \\| |\\  | |_| ||  _ <\n");
    scr("        |____/ |_| \\_|/_/   \\_\\_| \\_|\\____||_| \\_\\\n");
    scr(ESC_RESET "\n\n");
    scr("              " C_LABEL "Terminal Edition with AI\n\n");
    scr("        " ESC_BOLD "  [SPACE]" C_LABEL "  Start Game (Human)\n");
    scr("        " ESC_BOLD "  [A]" C_LABEL "       Start Game (AI)\n");
    scr("        " ESC_BOLD "  [Q]" C_LABEL "       Quit\n\n");
    scr("        " ESC_DIM "Best Score: " ESC_BOLD C_SCORE "%d\n" ESC_RESET, g.best);
}

static void render_play(void) {
    scr("\033[H" ESC_HIDE);
    scr("\n ");
    draw_hline("┌", "─", "┐");   /* ╔═╗ */
    draw_play_area();
    scr(" ");
    draw_hline("└", "─", "┘");   /* ╚═╝ */
}

static void render_over(void) {
    render_play();
    int cy = H / 2 + 3;
    scr("\033[%d;%dH", cy, 4);
    if (g.win) {
        scr(ESC_BOLD C_AI_ON "  *** YOU WIN! ***  " ESC_RESET);
    } else {
        scr(ESC_BOLD C_DEATH "   GAME  OVER   " ESC_RESET);
    }
    scr("\033[%d;%dH", cy + 1, 4);
    scr(C_SCORE "  Score: %d   Length: %d  " ESC_RESET, g.score, g.snake.len);
    scr("\033[%d;%dH", cy + 2, 4);
    scr(C_LABEL "  [R] Restart   [Q] Quit" ESC_RESET);
}

static void render(void) {
    scr_init();
    switch (g.state) {
    case ST_TITLE: render_title(); break;
    case ST_PLAY:
    case ST_PAUSE: render_play();  break;
    case ST_OVER:  render_over();  break;
    }
    scr_flush();
}

/* ── Input ────────────────────────────────────────────────────── */

static int read_key(void) {
    unsigned char c;
    int n = (int)read(STDIN_FILENO, &c, 1);
    if (n <= 0) return 0;
    if (c == 27) {                    /* ESC — might be arrow key */
        unsigned char seq[3] = {0};
        usleep(8000);
        int got = (int)read(STDIN_FILENO, seq, 3);
        if (got >= 2 && seq[0] == '[') {
            switch (seq[1]) {
            case 'A': return 'w';
            case 'B': return 's';
            case 'C': return 'd';
            case 'D': return 'a';
            }
        }
        return 27;                    /* bare ESC */
    }
    return c;
}

static void handle_key(int key) {
    if (!key) return;

    if (g.state == ST_TITLE) {
        switch (key) {
        case ' ':  g.state = ST_PLAY; g.ai_mode = 0; break;
        case 'a': case 'A':
                   g.state = ST_PLAY; g.ai_mode = 1; break;
        case 'q': case 'Q': case 27: running = 0;    break;
        }
        return;
    }

    if (g.state == ST_OVER) {
        switch (key) {
        case 'r': case 'R': game_reset(); break;
        case 'q': case 'Q': case 27: running = 0; break;
        }
        return;
    }

    /* ST_PLAY or ST_PAUSE */
    switch (key) {
    case 'q': case 'Q': case 27: running = 0; return;
    case 'p': case 'P':
        g.state = (g.state == ST_PAUSE) ? ST_PLAY : ST_PAUSE;
        return;
    case 'r': case 'R': game_reset(); return;
    case ' ':
        g.ai_mode = !g.ai_mode;
        return;
    case '+': case '=':
        if (g.speed < MAX_SPEED) g.speed++;
        return;
    case '-': case '_':
        if (g.speed > MIN_SPEED) g.speed--;
        return;
    }

    /* Direction keys (human mode only) */
    if (!g.ai_mode) {
        Dir want = g.snake.dir;
        switch (key) {
        case 'w': case 'W': want = DIR_UP;    break;
        case 's': case 'S': want = DIR_DOWN;  break;
        case 'a': case 'A': want = DIR_LEFT;  break;
        case 'd': case 'D': want = DIR_RIGHT; break;
        default: return;
        }
        if (want != opposite(g.snake.dir))
            g.snake.dir = want;
    }
}

/* ── Main ─────────────────────────────────────────────────────── */

int main(void) {
    srand((unsigned)time(NULL));
    term_raw();
    printf(ALT_ON);
    fflush(stdout);

    struct sigaction sa;
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    game_init();

    while (running) {
        int key = read_key();
        handle_key(key);

        if (g.state == ST_PLAY) {
            if (g.ai_mode)
                g.snake.dir = ai_decide();
            if (!move_snake()) {
                g.state = ST_OVER;
            }
            g.tick++;
        }

        render();

        /* Delay based on speed: speed 1→230 ms … speed 10→50 ms */
        int delay_ms = 250 - g.speed * 20;
        usleep((unsigned)delay_ms * 1000);
    }

    /* Cleanup */
    printf(ESC_SHOW ALT_OFF);
    fflush(stdout);
    term_restore();

    printf("Snake — Score: %d  Best: %d  Length: %d\n",
           g.score, g.best, g.snake.len);
    return 0;
}
