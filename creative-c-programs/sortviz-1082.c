/*
 * ============================================================================
 *  Terminal Sorting Algorithm Visualizer
 *  ======================================
 *  Animated visualization of 6 classic sorting algorithms in the terminal.
 *  Uses Unicode block characters and ANSI colors for real-time rendering.
 *
 *  Algorithms:
 *    1. Bubble Sort       4. Quick Sort (Lomuto partition)
 *    2. Selection Sort    5. Merge Sort
 *    3. Insertion Sort    6. Heap Sort
 *
 *  Compile:
 *    gcc -std=c99 -O2 -o sortviz sortviz-1082.c -lm
 *
 *  Run:
 *    ./sortviz
 *
 *  Controls (during sort):
 *    q - quit immediately
 *
 *  Requires: POSIX terminal (Linux/macOS), Unicode-capable terminal
 *  License: Public Domain
 * ============================================================================
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* ── ANSI helpers ────────────────────────────────────────────────────────── */

#define CLR         "\033[2J\033[H"
#define RESET       "\033[0m"
#define BOLD        "\033[1m"
#define DIM         "\033[2m"
#define RED         "\033[31m"
#define GREEN       "\033[32m"
#define YELLOW      "\033[33m"
#define BLUE        "\033[34m"
#define MAGENTA     "\033[35m"
#define CYAN        "\033[36m"
#define WHITE       "\033[37m"
#define BG_RED      "\033[41m"
#define BG_GREEN    "\033[42m"
#define BG_BLUE     "\033[44m"
#define BG_MAGENTA  "\033[45m"
#define BG_CYAN     "\033[46m"
#define BG_WHITE    "\033[47m"

/* ── Data types ──────────────────────────────────────────────────────────── */

typedef struct {
    int  *data;          /* array values (0 .. max_val) */
    int   size;          /* array length */
    int   max_val;       /* maximum value in array */
    /* highlight indices for rendering */
    int   hl_a, hl_b;   /* current pair being compared/swapped */
    int   hl_type;       /* 0=none  1=comparing  2=swapping  3=sorted  4=pivot  5=merging */
    int  *sorted;        /* boolean: is element in final position? */
    long  comparisons;
    long  swaps;
    long  array_accesses;
    int   stopped;       /* flag: user pressed q */
    int   speed;         /* delay microseconds */
} SortCtx;

typedef void (*SortFn)(SortCtx *ctx);

/* ── Terminal raw mode ───────────────────────────────────────────────────── */

static struct termios orig_term;

static void term_raw_on(void) {
    tcgetattr(STDIN_FILENO, &orig_term);
    struct termios raw = orig_term;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void term_raw_off(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_term);
}

static int  kbhit(void) {
    char c;
    if (read(STDIN_FILENO, &c, 1) == 1) return c;
    return 0;
}

/* ── Context helpers ─────────────────────────────────────────────────────── */

static void ctx_init(SortCtx *ctx, int size) {
    ctx->size     = size;
    ctx->data     = (int *)malloc(size * sizeof(int));
    ctx->sorted   = (int *)calloc(size, sizeof(int));
    ctx->max_val  = size;
    ctx->hl_a = ctx->hl_b = -1;
    ctx->hl_type  = 0;
    ctx->comparisons = ctx->swaps = ctx->array_accesses = 0;
    ctx->stopped  = 0;
    ctx->speed    = 5000;   /* 5ms default */

    for (int i = 0; i < size; i++)
        ctx->data[i] = i + 1;

    /* Fisher-Yates shuffle */
    srand((unsigned)time(NULL));
    for (int i = size - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = ctx->data[i];
        ctx->data[i] = ctx->data[j];
        ctx->data[j] = tmp;
    }
}

static void ctx_free(SortCtx *ctx) {
    free(ctx->data);
    free(ctx->sorted);
}

/* Pause + check for quit key */
static void frame(SortCtx *ctx) {
    if (ctx->speed > 0) usleep((useconds_t)ctx->speed);
    int ch = kbhit();
    if (ch == 'q' || ch == 'Q') { ctx->stopped = 1; }
}

/* ── Rendering ───────────────────────────────────────────────────────────── */

static int term_cols(void) {
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    return w.ws_col > 20 ? w.ws_col : 80;
}

static int term_rows(void) {
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    return w.ws_row > 10 ? w.ws_row : 24;
}

/* Pick a color for a bar based on value (rainbow mapping) */
static const char *val_color(int val, int max) {
    float t = (float)val / max;
    int segment = (int)(t * 5.999f);
    switch (segment) {
        case 0: return "\033[38;5;45m";   /* cyan */
        case 1: return "\033[38;5;51m";   /* bright cyan */
        case 2: return "\033[38;5;82m";   /* green */
        case 3: return "\033[38;5;226m";  /* yellow */
        case 4: return "\033[38;5;208m";  /* orange */
        case 5: return "\033[38;5;196m";  /* red */
        default: return CYAN;
    }
}

static void render(SortCtx *ctx, const char *title) {
    int cols = term_cols();
    int rows = term_rows();

    int bar_area_height = rows - 8;   /* leave room for header & stats */
    if (bar_area_height < 5)  bar_area_height = 5;
    if (bar_area_height > 40) bar_area_height = 40;

    int bar_width = cols / ctx->size;
    if (bar_width < 1) bar_width = 1;
    int usable_cols = bar_width * ctx->size;

    printf(CLR);
    /* Title bar */
    printf(BOLD CYAN "  ╔══════════════════════════════════════════════════════╗\n" RESET);
    printf(BOLD CYAN "  ║" WHITE "  %-50s" CYAN "║\n" RESET, title);
    printf(BOLD CYAN "  ╚══════════════════════════════════════════════════════╝\n" RESET);

    /* Draw bars (horizontal: each bar is a column of block chars) */
    /* We render top-down: from bar_area_height to 1 */
    float scale = (float)bar_area_height / ctx->max_val;

    for (int row = bar_area_height; row >= 1; row--) {
        for (int i = 0; i < ctx->size; i++) {
            int bar_h = (int)(ctx->data[i] * scale);
            const char *color = val_color(ctx->data[i], ctx->max_val);

            /* Override color for highlights */
            if (i == ctx->hl_a || i == ctx->hl_b) {
                switch (ctx->hl_type) {
                    case 1: color = BOLD YELLOW; break;     /* comparing */
                    case 2: color = BOLD RED;     break;    /* swapping */
                    case 4: color = BOLD MAGENTA; break;    /* pivot */
                    case 5: color = BOLD CYAN;    break;    /* merging */
                }
            }
            if (ctx->sorted[i]) {
                color = BOLD GREEN;
            }

            if (bar_h >= row) {
                printf("%s", color);
                for (int w = 0; w < bar_width; w++) printf("█");
            } else {
                printf(RESET DIM);
                for (int w = 0; w < bar_width; w++) printf(" ");
            }
        }
        printf(RESET "\n");
    }

    /* Stats line */
    double progress = 0;
    for (int i = 0; i < ctx->size; i++) if (ctx->sorted[i]) progress++;
    progress = progress / ctx->size * 100.0;

    printf("\n" DIM "  ");
    /* progress bar */
    int pb_width = 30;
    int filled = (int)(progress / 100.0 * pb_width);
    printf("[");
    printf(GREEN);
    for (int i = 0; i < filled; i++) printf("█");
    printf(DIM);
    for (int i = filled; i < pb_width; i++) printf("░");
    printf(RESET "] %5.1f%%", progress);

    printf("   " YELLOW "Comps:" WHITE " %-8ld "
           YELLOW "Swaps:" WHITE " %-8ld "
           YELLOW "Accesses:" WHITE " %-8ld\n",
           ctx->comparisons, ctx->swaps, ctx->array_accesses);

    printf(DIM "  Speed: %.1fms  |  Press " BOLD "q" DIM " to stop" RESET "\n",
           ctx->speed / 1000.0);

    fflush(stdout);
}

/* ── Sorting algorithms ──────────────────────────────────────────────────── */

static void bubble_sort(SortCtx *ctx) {
    int n = ctx->size;
    for (int i = 0; i < n - 1 && !ctx->stopped; i++) {
        int swapped = 0;
        for (int j = 0; j < n - i - 1 && !ctx->stopped; j++) {
            ctx->hl_a = j; ctx->hl_b = j + 1;
            ctx->hl_type = 1;  /* comparing */
            ctx->comparisons++;
            ctx->array_accesses += 2;
            render(ctx, "Bubble Sort");
            frame(ctx);

            if (ctx->data[j] > ctx->data[j + 1]) {
                ctx->hl_type = 2;  /* swapping */
                int tmp = ctx->data[j];
                ctx->data[j] = ctx->data[j + 1];
                ctx->data[j + 1] = tmp;
                ctx->swaps++;
                ctx->array_accesses += 4;
                swapped = 1;
                render(ctx, "Bubble Sort");
                frame(ctx);
            }
        }
        ctx->sorted[n - 1 - i] = 1;
        if (!swapped) break;
    }
    if (!ctx->stopped) {
        for (int i = 0; i < n; i++) ctx->sorted[i] = 1;
    }
    ctx->hl_a = ctx->hl_b = -1; ctx->hl_type = 0;
    render(ctx, "Bubble Sort");
}

static void selection_sort(SortCtx *ctx) {
    int n = ctx->size;
    for (int i = 0; i < n - 1 && !ctx->stopped; i++) {
        int min_idx = i;
        for (int j = i + 1; j < n && !ctx->stopped; j++) {
            ctx->hl_a = min_idx; ctx->hl_b = j;
            ctx->hl_type = 1;
            ctx->comparisons++;
            ctx->array_accesses += 2;
            render(ctx, "Selection Sort");
            frame(ctx);
            if (ctx->data[j] < ctx->data[min_idx]) {
                min_idx = j;
            }
        }
        if (min_idx != i) {
            ctx->hl_a = i; ctx->hl_b = min_idx;
            ctx->hl_type = 2;
            int tmp = ctx->data[i];
            ctx->data[i] = ctx->data[min_idx];
            ctx->data[min_idx] = tmp;
            ctx->swaps++;
            ctx->array_accesses += 4;
            render(ctx, "Selection Sort");
            frame(ctx);
        }
        ctx->sorted[i] = 1;
    }
    if (!ctx->stopped) ctx->sorted[n - 1] = 1;
    ctx->hl_a = ctx->hl_b = -1; ctx->hl_type = 0;
    render(ctx, "Selection Sort");
}

static void insertion_sort(SortCtx *ctx) {
    int n = ctx->size;
    ctx->sorted[0] = 1;
    for (int i = 1; i < n && !ctx->stopped; i++) {
        int key = ctx->data[i];
        ctx->array_accesses++;
        int j = i - 1;
        ctx->hl_a = i; ctx->hl_type = 1;
        render(ctx, "Insertion Sort");
        frame(ctx);

        while (j >= 0 && ctx->data[j] > key && !ctx->stopped) {
            ctx->hl_a = j; ctx->hl_b = j + 1;
            ctx->hl_type = 2;
            ctx->comparisons++;
            ctx->array_accesses += 3;
            ctx->data[j + 1] = ctx->data[j];
            ctx->swaps++;
            render(ctx, "Insertion Sort");
            frame(ctx);
            j--;
        }
        if (j >= 0) { ctx->comparisons++; ctx->array_accesses++; }
        ctx->data[j + 1] = key;
        ctx->array_accesses++;
        ctx->sorted[i] = 1;
        render(ctx, "Insertion Sort");
        frame(ctx);
    }
    if (!ctx->stopped) for (int i = 0; i < n; i++) ctx->sorted[i] = 1;
    ctx->hl_a = ctx->hl_b = -1; ctx->hl_type = 0;
    render(ctx, "Insertion Sort");
}

/* ── Quick Sort (Lomuto partition) ───────────────────────────────────── */

static void quicksort_rec(SortCtx *ctx, int lo, int hi) {
    if (lo >= hi || ctx->stopped) return;

    /* Partition */
    int pivot = ctx->data[hi];
    ctx->array_accesses++;
    ctx->hl_a = hi; ctx->hl_type = 4; /* pivot highlight */
    render(ctx, "Quick Sort");
    frame(ctx);

    int i = lo;
    for (int j = lo; j < hi && !ctx->stopped; j++) {
        ctx->hl_a = j; ctx->hl_b = hi;
        ctx->hl_type = 1;
        ctx->comparisons++;
        ctx->array_accesses += 2;
        render(ctx, "Quick Sort");
        frame(ctx);

        if (ctx->data[j] < pivot) {
            ctx->hl_a = i; ctx->hl_b = j;
            ctx->hl_type = 2;
            int tmp = ctx->data[i];
            ctx->data[i] = ctx->data[j];
            ctx->data[j] = tmp;
            ctx->swaps++;
            ctx->array_accesses += 4;
            render(ctx, "Quick Sort");
            frame(ctx);
            i++;
        }
    }
    /* Place pivot */
    ctx->hl_a = i; ctx->hl_b = hi;
    ctx->hl_type = 2;
    int tmp = ctx->data[i];
    ctx->data[i] = ctx->data[hi];
    ctx->data[hi] = tmp;
    ctx->swaps++;
    ctx->array_accesses += 4;
    ctx->sorted[i] = 1;
    render(ctx, "Quick Sort");
    frame(ctx);

    quicksort_rec(ctx, lo, i - 1);
    quicksort_rec(ctx, i + 1, hi);
}

static void quick_sort(SortCtx *ctx) {
    quicksort_rec(ctx, 0, ctx->size - 1);
    if (!ctx->stopped) for (int i = 0; i < ctx->size; i++) ctx->sorted[i] = 1;
    ctx->hl_a = ctx->hl_b = -1; ctx->hl_type = 0;
    render(ctx, "Quick Sort");
}

/* ── Merge Sort ──────────────────────────────────────────────────────── */

static void merge(SortCtx *ctx, int l, int m, int r) {
    int n1 = m - l + 1, n2 = r - m;
    int *L = (int *)malloc(n1 * sizeof(int));
    int *R = (int *)malloc(n2 * sizeof(int));

    for (int i = 0; i < n1; i++) { L[i] = ctx->data[l + i]; ctx->array_accesses++; }
    for (int i = 0; i < n2; i++) { R[i] = ctx->data[m + 1 + i]; ctx->array_accesses++; }

    int i = 0, j = 0, k = l;
    while (i < n1 && j < n2 && !ctx->stopped) {
        ctx->comparisons++;
        ctx->array_accesses += 2;
        ctx->hl_a = l + i; ctx->hl_b = m + 1 + j;
        ctx->hl_type = 5; /* merging */
        if (L[i] <= R[j]) {
            ctx->data[k] = L[i]; i++;
        } else {
            ctx->data[k] = R[j]; j++;
            ctx->swaps++;
        }
        ctx->array_accesses++;
        render(ctx, "Merge Sort");
        frame(ctx);
        k++;
    }
    while (i < n1 && !ctx->stopped) {
        ctx->data[k] = L[i];
        ctx->array_accesses++;
        ctx->hl_a = k; ctx->hl_type = 5;
        render(ctx, "Merge Sort");
        frame(ctx);
        i++; k++;
    }
    while (j < n2 && !ctx->stopped) {
        ctx->data[k] = R[j];
        ctx->array_accesses++;
        ctx->hl_a = k; ctx->hl_type = 5;
        render(ctx, "Merge Sort");
        frame(ctx);
        j++; k++;
    }
    free(L); free(R);
}

static void mergesort_rec(SortCtx *ctx, int l, int r) {
    if (l >= r || ctx->stopped) return;
    int m = l + (r - l) / 2;
    mergesort_rec(ctx, l, m);
    mergesort_rec(ctx, m + 1, r);
    merge(ctx, l, m, r);
}

static void merge_sort(SortCtx *ctx) {
    mergesort_rec(ctx, 0, ctx->size - 1);
    if (!ctx->stopped) for (int i = 0; i < ctx->size; i++) ctx->sorted[i] = 1;
    ctx->hl_a = ctx->hl_b = -1; ctx->hl_type = 0;
    render(ctx, "Merge Sort");
}

/* ── Heap Sort ───────────────────────────────────────────────────────── */

static void heapify(SortCtx *ctx, int n, int i) {
    int largest = i;
    int left  = 2 * i + 1;
    int right = 2 * i + 2;

    if (left < n && !ctx->stopped) {
        ctx->hl_a = largest; ctx->hl_b = left;
        ctx->hl_type = 1;
        ctx->comparisons++;
        ctx->array_accesses += 2;
        render(ctx, "Heap Sort");
        frame(ctx);
        if (ctx->data[left] > ctx->data[largest])
            largest = left;
    }
    if (right < n && !ctx->stopped) {
        ctx->hl_a = largest; ctx->hl_b = right;
        ctx->hl_type = 1;
        ctx->comparisons++;
        ctx->array_accesses += 2;
        render(ctx, "Heap Sort");
        frame(ctx);
        if (ctx->data[right] > ctx->data[largest])
            largest = right;
    }
    if (largest != i && !ctx->stopped) {
        ctx->hl_a = i; ctx->hl_b = largest;
        ctx->hl_type = 2;
        int tmp = ctx->data[i];
        ctx->data[i] = ctx->data[largest];
        ctx->data[largest] = tmp;
        ctx->swaps++;
        ctx->array_accesses += 4;
        render(ctx, "Heap Sort");
        frame(ctx);
        heapify(ctx, n, largest);
    }
}

static void heap_sort(SortCtx *ctx) {
    int n = ctx->size;
    /* Build max heap */
    for (int i = n / 2 - 1; i >= 0 && !ctx->stopped; i--)
        heapify(ctx, n, i);

    /* Extract elements */
    for (int i = n - 1; i > 0 && !ctx->stopped; i--) {
        ctx->hl_a = 0; ctx->hl_b = i;
        ctx->hl_type = 2;
        int tmp = ctx->data[0];
        ctx->data[0] = ctx->data[i];
        ctx->data[i] = tmp;
        ctx->swaps++;
        ctx->array_accesses += 4;
        ctx->sorted[i] = 1;
        render(ctx, "Heap Sort");
        frame(ctx);
        heapify(ctx, i, 0);
    }
    if (!ctx->stopped) ctx->sorted[0] = 1;
    ctx->hl_a = ctx->hl_b = -1; ctx->hl_type = 0;
    render(ctx, "Heap Sort");
}

/* ── All algorithms ──────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *desc;
    SortFn      fn;
    const char *complexity;
    const char *color;
} Algorithm;

static const Algorithm algorithms[] = {
    { "Bubble Sort",    "Repeatedly swap adjacent elements",    bubble_sort,    "O(n²)",     RED    },
    { "Selection Sort", "Find minimum, place at front",         selection_sort, "O(n²)",     YELLOW },
    { "Insertion Sort", "Build sorted array one element at a time", insertion_sort, "O(n²)", GREEN },
    { "Quick Sort",     "Divide & conquer with pivot",          quick_sort,     "O(n log n)",MAGENTA},
    { "Merge Sort",     "Divide & conquer with merge step",     merge_sort,     "O(n log n)",CYAN   },
    { "Heap Sort",      "Binary heap selection",                heap_sort,      "O(n log n)",BLUE   },
};
#define NUM_ALGOS 6

/* ── Interactive menu ─────────────────────────────────────────────────── */

static int show_menu(void) {
    printf(CLR);
    printf(BOLD CYAN "\n");
    printf("  ╔═══════════════════════════════════════════════════════════════╗\n");
    printf("  ║                                                               ║\n");
    printf("  ║   " WHITE "█▀▀█ █▀▀ █  █ █▀▀█ █▀▀█   " CYAN "█▀▀█ █  █ █▀▀▄ █▀▀█   " CYAN "║\n");
    printf("  ║   " WHITE "█▄▄█ █▀▀ █▄▄█ █▄▄█ █▄▄▀   " CYAN "█ ▄▄ █▄▄█ █  █ █▄▄█   " CYAN "║\n");
    printf("  ║   " WHITE "█  █ █▄▄ █  █ █  █ █  █   " CYAN "█▄▄█ █  █ █  █ █  █   " CYAN "║\n");
    printf("  ║                                                               ║\n");
    printf("  ║   " DIM "Terminal Sorting Algorithm Visualizer" CYAN "                    ║\n");
    printf("  ║═══════════════════════════════════════════════════════════════║\n");
    printf("  ║                                                               ║\n");

    for (int i = 0; i < NUM_ALGOS; i++) {
        printf("  ║   %s[%d] %-16s" CYAN "  " WHITE "%-28s " DIM "%-10s " CYAN "║\n",
               algorithms[i].color, i + 1,
               algorithms[i].name,
               algorithms[i].desc,
               algorithms[i].complexity);
    }

    printf("  ║                                                               ║\n");
    printf("  ║   " WHITE "[" BOLD "R" WHITE "]" DIM " Run all algorithms sequentially" CYAN "                       ║\n");
    printf("  ║   " WHITE "[" BOLD "Q" WHITE "]" DIM " Quit" CYAN "                                                 ║\n");
    printf("  ║                                                               ║\n");
    printf("  ╚═══════════════════════════════════════════════════════════════╝\n");
    printf(RESET "\n  Choose [1-6, R, Q]: ");
    fflush(stdout);

    term_raw_on();
    char ch = 0;
    while (1) {
        if (read(STDIN_FILENO, &ch, 1) == 1) {
            if (ch >= '1' && ch <= '6') { term_raw_off(); return ch - '1'; }
            if (ch == 'r' || ch == 'R') { term_raw_off(); return NUM_ALGOS; }
            if (ch == 'q' || ch == 'Q') { term_raw_off(); return -1; }
        }
        usleep(50000);
    }
}

static int ask_size(void) {
    printf(CLR);
    printf(BOLD CYAN "\n  Select array size:\n\n" RESET);
    printf("    " WHITE "[1]" DIM " 30 elements  (fast)\n" RESET);
    printf("    " WHITE "[2]" DIM " 60 elements  (medium)\n" RESET);
    printf("    " WHITE "[3]" DIM " 100 elements (slow)\n" RESET);
    printf("    " WHITE "[4]" DIM " 150 elements (very slow)\n\n  Choose [1-4]: ");
    fflush(stdout);

    term_raw_on();
    char ch = 0;
    while (1) {
        if (read(STDIN_FILENO, &ch, 1) == 1) {
            term_raw_off();
            switch (ch) {
                case '1': return 30;
                case '2': return 60;
                case '3': return 100;
                case '4': return 150;
                default:  return 60;
            }
        }
        usleep(50000);
    }
}

/* ── Victory animation ───────────────────────────────────────────────── */

static void victory_scroll(SortCtx *ctx, const char *title) {
    /* Mark all sorted, then do a celebratory scan */
    for (int i = 0; i < ctx->size; i++) ctx->sorted[i] = 1;
    ctx->hl_type = 0;

    /* Sweep green highlight across all bars */
    for (int sweep = 0; sweep < ctx->size && !ctx->stopped; sweep++) {
        ctx->hl_a = sweep; ctx->hl_b = sweep;
        ctx->hl_type = 3;
        render(ctx, title);
        usleep(15000);
        int ch = kbhit();
        if (ch == 'q' || ch == 'Q') ctx->stopped = 1;
    }
    ctx->hl_a = ctx->hl_b = -1;
    ctx->hl_type = 0;
    render(ctx, title);
}

/* ── Speed adjustment menu ──────────────────────────────────────────── */

static int ask_speed(void) {
    printf(CLR);
    printf(BOLD CYAN "\n  Animation speed:\n\n" RESET);
    printf("    " WHITE "[1]" DIM " Fast    (1ms delay)\n" RESET);
    printf("    " WHITE "[2]" DIM " Normal  (5ms delay)\n" RESET);
    printf("    " WHITE "[3]" DIM " Slow    (20ms delay)\n" RESET);
    printf("    " WHITE "[4]" DIM " Visual  (50ms delay)\n\n  Choose [1-4]: ");
    fflush(stdout);

    term_raw_on();
    char ch = 0;
    while (1) {
        if (read(STDIN_FILENO, &ch, 1) == 1) {
            term_raw_off();
            switch (ch) {
                case '1': return 1000;
                case '2': return 5000;
                case '3': return 20000;
                case '4': return 50000;
                default:  return 5000;
            }
        }
        usleep(50000);
    }
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf(CLR);
    printf(BOLD WHITE "\n  Sorting Algorithm Visualizer\n" RESET);
    printf("  Initializing...\n");
    fflush(stdout);

    while (1) {
        int algo = show_menu();
        if (algo < 0) break;

        int size = ask_size();
        int speed = ask_speed();

        if (algo < NUM_ALGOS) {
            /* Single algorithm */
            SortCtx ctx;
            ctx_init(&ctx, size);
            ctx.speed = speed;
            render(&ctx, algorithms[algo].name);
            usleep(200000);

            term_raw_on();
            algorithms[algo].fn(&ctx);
            term_raw_off();

            if (!ctx.stopped) victory_scroll(&ctx, algorithms[algo].name);

            printf("\n  " GREEN BOLD "Done!" RESET " Press any key to continue...");
            fflush(stdout);
            term_raw_on();
            char dummy = 0;
            while (read(STDIN_FILENO, &dummy, 1) != 1) usleep(50000);
            term_raw_off();

            ctx_free(&ctx);
        } else {
            /* Run all */
            int saved_size = size;
            for (int a = 0; a < NUM_ALGOS; a++) {
                SortCtx ctx;
                ctx_init(&ctx, saved_size);
                ctx.speed = speed;
                render(&ctx, algorithms[a].name);
                usleep(300000);

                term_raw_on();
                algorithms[a].fn(&ctx);
                term_raw_off();

                if (!ctx.stopped) victory_scroll(&ctx, algorithms[a].name);

                printf("\n  " GREEN BOLD "%s complete!" RESET " Press any key for next algorithm (q=menu)...", algorithms[a].name);
                fflush(stdout);
                term_raw_on();
                char ch = 0;
                while (read(STDIN_FILENO, &ch, 1) != 1) usleep(50000);
                term_raw_off();
                if (ch == 'q' || ch == 'Q') { ctx_free(&ctx); break; }

                ctx_free(&ctx);
            }
        }
    }

    printf(CLR BOLD GREEN "  Goodbye!\n\n" RESET);
    return 0;
}
