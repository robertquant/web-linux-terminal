/*
 *  ═══════════════════════════════════════════════════════════════════
 *  WF3D — 3D ASCII WIREFRAME RENDERER
 *  Real-time rotating 3D objects rendered as colored ASCII art
 *
 *  Compile:  gcc -std=c99 -Wall -O2 -o wf3d wireframe3d-8755.c -lm
 *  Run:      ./wf3d
 *
 *  Controls:
 *    1 - Cube        2 - Pyramid      3 - Octahedron
 *    4 - Torus       5 - Sphere       6 - Icosahedron
 *    +/−  : Adjust rotation speed     Space : Pause / Resume
 *    WASD  : Manual rotation          Q/Esc : Quit
 *  ═══════════════════════════════════════════════════════════════════
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <signal.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Data types ────────────────────────────────────────────────── */

typedef struct { float x, y, z; } Vec3;
typedef struct { int a, b; } Edge;

typedef struct {
    Vec3 *v;
    Edge *e;
    int   nv, ne;
    char  name[24];
} Mesh;

typedef struct {
    char  ch;
    float depth;
} Pixel;

/* ── Globals ───────────────────────────────────────────────────── */

static struct termios g_orig;
static volatile sig_atomic_t g_run = 1;

/* ── Terminal helpers ──────────────────────────────────────────── */

static void raw_on(void)
{
    struct termios t;
    tcgetattr(0, &g_orig);
    t = g_orig;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
}

static void raw_off(void) { tcsetattr(0, TCSANOW, &g_orig); }

static void on_sig(int s) { (void)s; g_run = 0; }

static void get_size(int *w, int *h)
{
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20 && ws.ws_row > 10) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    } else {
        *w = 80; *h = 24;
    }
}

/* Read a keypress; handles ESC sequences for arrow keys */
static int read_key(void)
{
    char c;
    if (read(0, &c, 1) != 1) return -1;
    if (c == 27) {
        char s0 = 0, s1 = 0;
        fd_set fds; struct timeval tv = {0, 5000};
        FD_ZERO(&fds); FD_SET(0, &fds);
        if (select(1, &fds, 0, 0, &tv) > 0 && read(0, &s0, 1) == 1) {
            FD_ZERO(&fds); FD_SET(0, &fds);
            if (select(1, &fds, 0, 0, &tv) > 0 && read(0, &s1, 1) == 1) {
                if (s0 == '[') {
                    switch (s1) {
                        case 'A': return 'U';   /* Up    */
                        case 'B': return 'D';   /* Down  */
                        case 'C': return 'R';   /* Right */
                        case 'D': return 'L';   /* Left  */
                    }
                }
            }
        }
        return 27;   /* bare Escape */
    }
    return c;
}

/* ── 3D math ───────────────────────────────────────────────────── */

static Vec3 rot_x(Vec3 p, float a)
{
    float c = cosf(a), s = sinf(a);
    return (Vec3){p.x, p.y*c - p.z*s, p.y*s + p.z*c};
}

static Vec3 rot_y(Vec3 p, float a)
{
    float c = cosf(a), s = sinf(a);
    return (Vec3){p.x*c + p.z*s, p.y, -p.x*s + p.z*c};
}

static Vec3 rot_z(Vec3 p, float a)
{
    float c = cosf(a), s = sinf(a);
    return (Vec3){p.x*c - p.y*s, p.x*s + p.y*c, p.z};
}

/* ── Mesh constructors ─────────────────────────────────────────── */

static Mesh mk_cube(void)
{
    Mesh m = {.nv = 8, .ne = 12};
    strcpy(m.name, "Cube");
    m.v = malloc(8  * sizeof(Vec3));
    m.e = malloc(12 * sizeof(Edge));
    Vec3 v[] = {
        {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
        {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}
    };
    memcpy(m.v, v, sizeof(v));
    Edge e[] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };
    memcpy(m.e, e, sizeof(e));
    return m;
}

static Mesh mk_pyramid(void)
{
    Mesh m = {.nv = 5, .ne = 8};
    strcpy(m.name, "Pyramid");
    m.v = malloc(5 * sizeof(Vec3));
    m.e = malloc(8 * sizeof(Edge));
    Vec3 v[] = {
        {-1,-1,-1},{ 1,-1,-1},{ 1,-1, 1},{-1,-1, 1},
        { 0, 1.5f, 0}
    };
    memcpy(m.v, v, sizeof(v));
    Edge e[] = {
        {0,1},{1,2},{2,3},{3,0},
        {0,4},{1,4},{2,4},{3,4}
    };
    memcpy(m.e, e, sizeof(e));
    return m;
}

static Mesh mk_octa(void)
{
    Mesh m = {.nv = 6, .ne = 12};
    strcpy(m.name, "Octahedron");
    m.v = malloc(6  * sizeof(Vec3));
    m.e = malloc(12 * sizeof(Edge));
    float s = 1.3f;
    Vec3 v[] = {
        { s, 0, 0},{-s, 0, 0},
        { 0, s, 0},{ 0,-s, 0},
        { 0, 0, s},{ 0, 0,-s}
    };
    memcpy(m.v, v, sizeof(v));
    Edge e[] = {
        {0,2},{0,3},{0,4},{0,5},
        {1,2},{1,3},{1,4},{1,5},
        {2,4},{2,5},{3,4},{3,5}
    };
    memcpy(m.e, e, sizeof(e));
    return m;
}

static Mesh mk_torus(void)
{
    int N = 24, n = 12;
    float R = 1.0f, r = 0.4f;
    Mesh m;
    m.nv = N * n;
    m.ne = N * n * 2;
    strcpy(m.name, "Torus");
    m.v = malloc(m.nv * sizeof(Vec3));
    m.e = malloc(m.ne * sizeof(Edge));

    for (int i = 0; i < N; i++) {
        float th = 2.0f * (float)M_PI * i / N;
        for (int j = 0; j < n; j++) {
            float ph = 2.0f * (float)M_PI * j / n;
            m.v[i*n + j] = (Vec3){
                (R + r*cosf(ph)) * cosf(th),
                (R + r*cosf(ph)) * sinf(th),
                 r * sinf(ph)
            };
        }
    }
    int ei = 0;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < n; j++) {
            m.e[ei++] = (Edge){i*n + j, ((i+1)%N)*n + j};
            m.e[ei++] = (Edge){i*n + j, i*n + (j+1)%n};
        }
    return m;
}

static Mesh mk_sphere(void)
{
    int slices = 20, stacks = 14;
    float rad = 1.3f;
    Mesh m;
    m.nv = slices * (stacks + 1);
    strcpy(m.name, "Sphere");
    m.v = malloc(m.nv * sizeof(Vec3));

    int vi = 0;
    for (int i = 0; i <= stacks; i++) {
        float th = (float)M_PI * i / stacks;
        for (int j = 0; j < slices; j++) {
            float ph = 2.0f * (float)M_PI * j / slices;
            m.v[vi++] = (Vec3){
                rad * sinf(th) * cosf(ph),
                rad * cosf(th),
                rad * sinf(th) * sinf(ph)
            };
        }
    }
    m.ne = stacks * slices * 2;
    m.e  = malloc(m.ne * sizeof(Edge));
    int ei = 0;
    for (int i = 0; i < stacks; i++)
        for (int j = 0; j < slices; j++) {
            int idx = i * slices + j;
            m.e[ei++] = (Edge){idx, i * slices + (j+1) % slices};
            m.e[ei++] = (Edge){idx, (i+1) * slices + j};
        }
    return m;
}

static Mesh mk_icosa(void)
{
    Mesh m = {.nv = 12, .ne = 30};
    strcpy(m.name, "Icosahedron");
    m.v = malloc(12 * sizeof(Vec3));
    m.e = malloc(30 * sizeof(Edge));

    float phi = (1.0f + sqrtf(5.0f)) / 2.0f;
    Vec3 v[] = {
        {-1, phi, 0}, { 1, phi, 0}, {-1,-phi, 0}, { 1,-phi, 0},
        { 0,-1, phi}, { 0, 1, phi}, { 0,-1,-phi}, { 0, 1,-phi},
        { phi, 0,-1}, { phi, 0, 1}, {-phi, 0,-1}, {-phi, 0, 1}
    };
    memcpy(m.v, v, sizeof(v));
    /* 30 edges — each vertex has degree 5 */
    Edge e[] = {
        {0,1},{0,5},{0,7},{0,10},{0,11},
        {1,5},{1,7},{1,8},{1,9},
        {2,3},{2,4},{2,6},{2,10},{2,11},
        {3,4},{3,6},{3,8},{3,9},
        {4,5},{4,9},{4,11},
        {5,9},{5,11},
        {6,7},{6,8},{6,10},
        {7,8},{7,10},
        {8,9},
        {10,11}
    };
    memcpy(m.e, e, sizeof(e));
    return m;
}

/* ── Rendering ─────────────────────────────────────────────────── */

static const char shades[] = " .:-=+*#%@";
#define NSHADE 10

typedef struct { int idx; float z; } SortEdge;

static int cmp_z(const void *a, const void *b)
{
    float za = ((const SortEdge *)a)->z;
    float zb = ((const SortEdge *)b)->z;
    return (za > zb) - (za < zb);
}

/* Bresenham line drawing into a pixel buffer */
static void draw_line(Pixel *buf, int w, int h,
                      int x0, int y0, int x1, int y1,
                      char ch, float depth)
{
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h) {
            Pixel *p  = &buf[y0 * w + x0];
            p->ch    = ch;
            p->depth = depth;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Map normalised depth t∈[0,1] to RGB — dark blue ➜ cyan ➜ white */
static void depth_color(float t, int *r, int *g, int *b)
{
    if (t < 0.5f) {
        float s = t * 2.0f;
        *r = (int)(30  * s);
        *g = (int)(60  + 195 * s);
        *b = (int)(160 + 95  * s);
    } else {
        float s = (t - 0.5f) * 2.0f;
        *r = (int)(30  + 225 * s);
        *g = 255;
        *b = 255;
    }
}

/* ── Title screen ──────────────────────────────────────────────── */

static void show_title(void)
{
    const char *art =
        "\033[1;36m"
        "  ╦ ╦┌─┐┌┐ ╔═╗╔═╗  ╦┌┐┌┌┬┐┌─┐┬  ┌─┐\n"
        "  ║║║├┤ ├┴┐║ ║╚═╗  ║│││ ││├┤ │  └─┐\n"
        "  ╚╩╝└─┘└─┘╚═╝╚═╝  ╩┘└┘─┴┘└─┘┴─┘└─┘\n"
        "\033[0;36m"
        "      3D ASCII WIREFRAME RENDERER\n"
        "\033[2m"
        "      Press any key to start...\033[0m\n";

    printf("\033[2J\033[H%s", art);
    fflush(stdout);

    /* Wait for any key */
    fd_set fds;
    struct timeval tv;
    do {
        FD_ZERO(&fds); FD_SET(0, &fds);
        tv = (struct timeval){0, 100000};
    } while (select(1, &fds, 0, 0, &tv) <= 0 && g_run);
    char discard;
    ssize_t _r = read(0, &discard, 1); (void)_r;
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(void)
{
    /* Setup */
    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);
    raw_on();
    atexit(raw_off);

    printf("\033[?25l");      /* hide cursor */
    fflush(stdout);

    show_title();

    /* Build all meshes */
    Mesh meshes[] = {
        mk_cube(), mk_pyramid(), mk_octa(),
        mk_torus(), mk_sphere(), mk_icosa()
    };
    int nmesh = 6, cur = 0;

    float ax = 0.3f, ay = 0.5f, az = 0.0f;
    float speed  = 0.03f;
    int   paused = 0;

    /* FPS tracking */
    int frame_count = 0, fps = 0;
    struct timespec t_prev;
    clock_gettime(CLOCK_MONOTONIC, &t_prev);

    printf("\033[2J");
    fflush(stdout);

    /* ── Main loop ──────────────────────────────────────────────── */
    while (g_run) {
        int w, h;
        get_size(&w, &h);
        int rh = h - 2;
        if (rh < 5) rh = 5;
        int cx = w / 2, cy = rh / 2;
        float fov  = (w < 100 ? 12.0f : 20.0f);
        float dist = 5.0f;

        /* ── Input ──────────────────────────────────────────────── */
        fd_set fds; struct timeval tv_in = {0, 0};
        while (1) {
            FD_ZERO(&fds); FD_SET(0, &fds);
            if (select(1, &fds, 0, 0, &tv_in) <= 0) break;
            int k = read_key();
            if (k < 0) break;
            switch (k) {
                case '1': cur = 0; break;
                case '2': cur = 1; break;
                case '3': cur = 2; break;
                case '4': cur = 3; break;
                case '5': cur = 4; break;
                case '6': cur = 5; break;
                case '+': case '=':
                    speed = fminf(speed * 1.3f, 0.3f); break;
                case '-': case '_':
                    speed = fmaxf(speed / 1.3f, 0.002f); break;
                case ' ': paused = !paused; break;
                case 'q': case 'Q': case 27:
                    g_run = 0; break;
                case 'w': case 'U': ax -= 0.15f; break;
                case 's': case 'D': ax += 0.15f; break;
                case 'a': case 'L': ay -= 0.15f; break;
                case 'd': case 'R': ay += 0.15f; break;
            }
        }

        /* ── Update ─────────────────────────────────────────────── */
        if (!paused) {
            ax += speed;
            ay += speed * 0.7f;
            az += speed * 0.3f;
        }

        /* ── Transform vertices ─────────────────────────────────── */
        Mesh *m = &meshes[cur];
        Vec3 *xv = malloc(m->nv * sizeof(Vec3));
        float minz = 1e9f, maxz = -1e9f;

        for (int i = 0; i < m->nv; i++) {
            Vec3 p = m->v[i];
            p = rot_x(p, ax);
            p = rot_y(p, ay);
            p = rot_z(p, az);
            xv[i] = p;
            if (p.z < minz) minz = p.z;
            if (p.z > maxz) maxz = p.z;
        }
        float zrange = maxz - minz;
        if (zrange < 0.01f) zrange = 1.0f;

        /* ── Sort edges back-to-front (painter's algorithm) ─────── */
        SortEdge *se = malloc(m->ne * sizeof(SortEdge));
        for (int i = 0; i < m->ne; i++) {
            se[i].idx = i;
            se[i].z   = (xv[m->e[i].a].z + xv[m->e[i].b].z) * 0.5f;
        }
        qsort(se, m->ne, sizeof(SortEdge), cmp_z);

        /* ── Rasterise into frame buffer ────────────────────────── */
        Pixel *frame = malloc(w * rh * sizeof(Pixel));
        for (int i = 0; i < w * rh; i++) {
            frame[i].ch    = ' ';
            frame[i].depth = 0;
        }

        for (int i = 0; i < m->ne; i++) {
            Edge edge = m->e[se[i].idx];
            Vec3 va = xv[edge.a], vb = xv[edge.b];

            float t  = (se[i].z - minz) / zrange;   /* 0=far 1=near */
            int   si = (int)(t * (NSHADE - 1));
            if (si < 0)     si = 0;
            if (si >= NSHADE) si = NSHADE - 1;
            char ch = shades[si];
            if (ch == ' ') ch = '.';   /* even far edges should be visible */

            /* Perspective projection (Y halved for terminal aspect ratio) */
            float da = va.z + dist; if (da < 0.1f) da = 0.1f;
            float db = vb.z + dist; if (db < 0.1f) db = 0.1f;
            int sx0 = (int)( fov    * va.x / da + cx);
            int sy0 = (int)( fov*0.5f * va.y / da + cy);
            int sx1 = (int)( fov    * vb.x / db + cx);
            int sy1 = (int)( fov*0.5f * vb.y / db + cy);

            draw_line(frame, w, rh, sx0, sy0, sx1, sy1, ch, se[i].z);
        }

        /* ── Blit frame to terminal ─────────────────────────────── */
        printf("\033[H");
        int last_band = -1;
        for (int y = 0; y < rh; y++) {
            for (int x = 0; x < w; x++) {
                Pixel *p = &frame[y * w + x];
                if (p->ch != ' ') {
                    float t = (p->depth - minz) / zrange;
                    /* 5 color bands to reduce escape-sequence overhead */
                    int band = (int)(t * 4.99f);
                    if (band < 0) band = 0;
                    if (band > 4) band = 4;
                    if (band != last_band) {
                        int r, g, b;
                        depth_color((float)band / 4.0f, &r, &g, &b);
                        printf("\033[38;2;%d;%d;%dm", r, g, b);
                        last_band = band;
                    }
                    putchar(p->ch);
                } else {
                    if (last_band != -1) {
                        printf("\033[0m");
                        last_band = -1;
                    }
                    putchar(' ');
                }
            }
            printf("\033[0m\n");
            last_band = -1;
        }

        /* HUD bar */
        printf("\033[0m\033[K "
               "\033[1;36m[%s]\033[0m"
               "  Speed:%.2f  %s  FPS:%d"
               "  \033[2m[1-6]Shape [+/-]Speed"
               " [Space]Pause [WASD]Rotate [Q]Quit\033[0m\n",
               m->name, speed, paused ? "\033[1;33mPAUSED\033[0m" : "Running",
               fps);
        fflush(stdout);

        /* ── FPS counter ────────────────────────────────────────── */
        frame_count++;
        struct timespec t_now;
        clock_gettime(CLOCK_MONOTONIC, &t_now);
        float elapsed = (t_now.tv_sec  - t_prev.tv_sec)
                      + (t_now.tv_nsec - t_prev.tv_nsec) * 1e-9f;
        if (elapsed >= 1.0f) {
            fps = (int)(frame_count / elapsed + 0.5f);
            frame_count = 0;
            t_prev = t_now;
        }

        free(xv); free(se); free(frame);
        { struct timespec _ts = {0, 30000000}; nanosleep(&_ts, NULL); }
    }

    /* ── Cleanup ────────────────────────────────────────────────── */
    printf("\033[?25h\033[2J\033[H"
           "\033[1;36mThanks for using WF3D!\033[0m\n\n");
    fflush(stdout);

    for (int i = 0; i < nmesh; i++) {
        free(meshes[i].v);
        free(meshes[i].e);
    }
    return 0;
}
