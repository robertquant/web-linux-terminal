/*
 * ================================================================
 *  voxelspace-8172.c  –  Voxel Space Terrain Flythrough
 * ================================================================
 *
 *  Real-time 3D terrain renderer using the Voxel Space algorithm
 *  (inspired by the 1992 Comanche game engine). A procedurally
 *  generated island landscape is rendered in ASCII characters
 *  with ANSI terminal colors.  Fly over mountains, valleys, and
 *  oceans — all in your terminal.
 *
 *  Compile:
 *    gcc -O2 -o voxelspace voxelspace-8172.c -lm
 *
 *  Run (make sure your terminal is at least 122 x 42):
 *    ./voxelspace
 *
 *  Controls:
 *    W / ↑      Fly forward          S / ↓    Fly backward
 *    A / ←      Turn left            D / →    Turn right
 *    Q          Gain altitude         E        Lose altitude
 *    Space      Toggle auto-fly       M        Toggle minimap
 *    + / -      Adjust speed          Esc      Quit
 *
 *  Requires: C99+, POSIX termios, ANSI terminal with color support.
 *  No external libraries beyond libc and libm.
 * ================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <signal.h>
#include <time.h>

/* ----------------------------------------------------------------
 *  Configuration
 * ---------------------------------------------------------------- */
#define MAP_W        1024
#define MAP_MASK     (MAP_W - 1)
#define SCR_W        120
#define SCR_H        38
#define FOV          1.22f          /* ~70 degrees                      */
#define MAX_DEPTH    420.0f         /* max ray distance                 */
#define MIN_DEPTH    1.0f           /* min ray distance                 */
#define PROJ_K       4.8f           /* projection constant              */
#define TARGET_FPS   30
#define FRAME_US     (1000000 / TARGET_FPS)

/* ----------------------------------------------------------------
 *  Global state
 * ---------------------------------------------------------------- */

/* Terrain data (static -> BSS segment, ~8 MB) */
static float hmap[MAP_W][MAP_W];   /* height field  0..260             */
static int   cmap[MAP_W][MAP_W];   /* ANSI color index 0-7             */

/* Screen buffer */
static char  sbuf[SCR_H][SCR_W + 1];
static int   scol[SCR_H][SCR_W];
static char  obuf[SCR_H * SCR_W * 20 + 4096];

/* Camera */
static float cx = 512.0f, cy = 512.0f, cz = 120.0f;
static float heading  = 0.0f;
static float cam_vz   = 0.0f;      /* vertical velocity                */
static float fly_spd  = 2.8f;      /* auto-fly forward speed           */
static int   auto_fly = 1;
static int   show_map = 1;
static int   frame_no = 0;
static volatile int running = 1;

/* Terminal */
static struct termios tio_save;

/* ----------------------------------------------------------------
 *  Terminal helpers
 * ---------------------------------------------------------------- */

static void term_raw(void)
{
    struct termios t;
    tcgetattr(STDIN_FILENO, &tio_save);
    t = tio_save;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

static void term_restore(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &tio_save);
}

static int kbhit(void)
{
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

static void on_signal(int sig) { (void)sig; running = 0; }

/* ----------------------------------------------------------------
 *  Procedural terrain – value noise + fBm
 * ---------------------------------------------------------------- */

static unsigned int ihash(int x, int y)
{
    unsigned int n = (unsigned int)(x * 374761393u + y * 668265263u);
    n = (n ^ (n >> 13)) * 1274126177u;
    return n ^ (n >> 16);
}

static float noise2f(float x, float y)
{
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - ix, fy = y - iy;
    /* smoothstep interpolation */
    float sx = fx * fx * (3.0f - 2.0f * fx);
    float sy = fy * fy * (3.0f - 2.0f * fy);
    float a = (ihash(ix,   iy  ) & 0xffff) / 65535.0f;
    float b = (ihash(ix+1, iy  ) & 0xffff) / 65535.0f;
    float c = (ihash(ix,   iy+1) & 0xffff) / 65535.0f;
    float d = (ihash(ix+1, iy+1) & 0xffff) / 65535.0f;
    return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
}

/* Fractal Brownian Motion – 7 octaves of value noise */
static float fbm(float x, float y)
{
    float v = 0.0f, amp = 0.5f;
    for (int i = 0; i < 7; i++) {
        v += amp * noise2f(x, y);
        x *= 2.03f;
        y *= 2.03f;
        amp *= 0.49f;
    }
    return v;
}

static void gen_terrain(void)
{
    fprintf(stderr, "  Generating island terrain (%dx%d) ...\n", MAP_W, MAP_W);
    for (int y = 0; y < MAP_W; y++) {
        for (int x = 0; x < MAP_W; x++) {
            float nx = x / (float)MAP_W * 6.0f;
            float ny = y / (float)MAP_W * 6.0f;

            float h = fbm(nx + 0.5f, ny + 0.5f);

            /* island mask – lower at edges */
            float dx = (x - 512.0f) / 512.0f;
            float dy = (y - 512.0f) / 512.0f;
            float rim = 1.0f - sqrtf(dx * dx + dy * dy) * 0.65f;
            if (rim < 0.0f) rim = 0.0f;
            h *= rim;

            /* add some ridges */
            float ridge = 1.0f - fabsf(noise2f(nx * 1.5f + 10.0f, ny * 1.5f + 10.0f) * 2.0f - 1.0f);
            h = h * 0.7f + ridge * 0.3f * rim;

            h *= 260.0f;
            if (h < 0.0f) h = 0.0f;
            hmap[y][x] = h;

            /* biome color */
            int ih = (int)h;
            if      (ih < 35)  cmap[y][x] = 4;   /* deep water   blue  */
            else if (ih < 55)  cmap[y][x] = 6;   /* shallows     cyan  */
            else if (ih < 72)  cmap[y][x] = 2;   /* beach        green */
            else if (ih < 115) cmap[y][x] = 2;   /* plains       green */
            else if (ih < 155) cmap[y][x] = 3;   /* hills        yellow*/
            else if (ih < 200) cmap[y][x] = 1;   /* rock         red   */
            else               cmap[y][x] = 7;   /* snow peaks   white */
        }
    }
    fprintf(stderr, "  Terrain ready.\n");
}

/* ----------------------------------------------------------------
 *  Rendering
 * ---------------------------------------------------------------- */

/* Map terrain height to ASCII character density */
static const char density[] = " .,:;=+*#%@@";

static inline char h2ch(float h, float fog)
{
    int i = (int)(h * 0.05f);
    if (i < 0) i = 0;
    if (i > 10) i = 10;
    /* distance fog: fade to sparse characters */
    i = (int)(i * fog);
    if (i < 0) i = 0;
    if (i > 10) i = 10;
    return density[i + 1];
}

static void render(void)
{
    /* clear buffers */
    for (int y = 0; y < SCR_H; y++) {
        memset(sbuf[y], ' ', SCR_W);
        sbuf[y][SCR_W] = '\0';
        for (int x = 0; x < SCR_W; x++) scol[y][x] = -1;
    }

    /* ---- Voxel Space ray-marching ---- */
    for (int col = 0; col < SCR_W; col++) {
        float ray_a = heading - FOV * 0.5f + FOV * col / (SCR_W - 1);
        float rdx = sinf(ray_a);
        float rdy = cosf(ray_a);
        float max_y = (float)SCR_H;          /* bottom of screen */

        for (float d = MIN_DEPTH; d < MAX_DEPTH; d *= 1.018f) {
            float px = cx + rdx * d;
            float py = cy + rdy * d;
            int mx = ((int)px) & MAP_MASK;
            int my = ((int)py) & MAP_MASK;

            float th = hmap[my][mx];
            float proj = (th - cz) * PROJ_K / d;
            int sy = (int)(SCR_H * 0.5f - proj);
            if (sy < 0)         sy = 0;
            if (sy >= SCR_H)    sy = SCR_H - 1;

            if (sy >= (int)max_y) continue;

            float fog = 1.0f - d / MAX_DEPTH;
            int c = cmap[my][mx];
            char ch = h2ch(th, fog);

            if (fog < 0.12f) { ch = ' '; c = 0; }
            else if (fog < 0.28f) { c = 0; }

            for (int y = sy; y < (int)max_y && y < SCR_H; y++) {
                sbuf[y][col] = ch;
                scol[y][col] = c;
            }
            max_y = (float)sy;
            if (max_y <= 0) break;
        }
    }

    /* ---- Sky gradient & twinkling stars ---- */
    for (int col = 0; col < SCR_W; col++) {
        for (int y = 0; y < SCR_H; y++) {
            if (scol[y][col] != -1) continue;
            /* stars near the top */
            if (y < 7 && (ihash(col * 31 + (frame_no >> 3), y * 17) & 0xff) < 6) {
                sbuf[y][col] = '.';
                scol[y][col] = 7;
            } else {
                sbuf[y][col] = ' ';
                scol[y][col] = 4;   /* blue sky */
            }
        }
    }

    /* ---- Mini-map overlay ---- */
    if (show_map) {
        int ms = 16;
        int ox = SCR_W - ms - 2;
        int oy = 1;
        /* border */
        for (int i = 0; i < ms; i++) {
            if (oy - 1 >= 0)        { sbuf[oy-1][ox+i] = '-'; scol[oy-1][ox+i] = 7; }
            if (oy + ms < SCR_H)    { sbuf[oy+ms][ox+i] = '-'; scol[oy+ms][ox+i] = 7; }
            if (oy + i < SCR_H)     {
                sbuf[oy+i][ox-1]  = '|'; scol[oy+i][ox-1]  = 7;
                sbuf[oy+i][ox+ms] = '|'; scol[oy+i][ox+ms] = 7;
            }
        }
        /* terrain pixels */
        for (int dy = 0; dy < ms; dy++) {
            for (int dx = 0; dx < ms; dx++) {
                int wx = ((int)cx - ms/2 + dx) & MAP_MASK;
                int wy = ((int)cy - ms/2 + dy) & MAP_MASK;
                int sy = oy + dy, sx = ox + dx;
                if (sy < SCR_H && sx < SCR_W) {
                    float h = hmap[wy][wx];
                    sbuf[sy][sx] = h < 40 ? '~' : h < 90 ? '.' :
                                   h < 150 ? '=' : h < 200 ? '#' : '@';
                    scol[sy][sx] = cmap[wy][wx];
                }
            }
        }
        /* player direction arrow */
        int pcx = ox + ms/2, pcy = oy + ms/2;
        if (pcy >= 0 && pcy < SCR_H && pcx >= 0 && pcx < SCR_W) {
            static const char arrows[] = ">v<^";
            int ai = (int)(heading / (float)(M_PI * 0.5f) + 0.5f) & 3;
            sbuf[pcy][pcx] = arrows[ai];
            scol[pcy][pcx] = 1;
        }
    }

    /* ---- Build ANSI output string ---- */
    int p = 0;
    p += sprintf(obuf + p, "\033[H");       /* cursor home */

    int prev = -1;
    for (int y = 0; y < SCR_H; y++) {
        for (int x = 0; x < SCR_W; x++) {
            int c = scol[y][x];
            if (c != prev && c >= 0 && c < 8) {
                p += sprintf(obuf + p, "\033[3%dm", c);
                prev = c;
            }
            obuf[p++] = sbuf[y][x];
        }
        obuf[p++] = '\n';
    }

    /* ---- HUD ---- */
    int deg = (int)(heading * 180.0f / M_PI) % 360;
    if (deg < 0) deg += 360;
    static const char *card[] = {"N ","NE","E ","SE","S ","SW","W ","NW"};
    int di = ((deg + 22) / 45) & 7;

    p += sprintf(obuf + p, "\033[0m\033[37m"
        " \x1b[1m%s\x1b[0m\x1b[37m  "
        "Pos:(%4.0f,%4.0f)  Alt:%4.0f  "
        "Hdg:%3d°%s  Spd:%.1f  "
        "%s  \033[2m[WASD:fly Q/E:alt Space:auto M:map Esc:quit]\033[K",
        auto_fly ? "\x1b[32mAUTO" : "\x1b[33mMAN ",
        cx, cy, cz, deg, card[di], fly_spd,
        show_map ? "\x1b[36m[MAP]" : "     ");

    obuf[p] = '\0';
    fwrite(obuf, 1, (size_t)p, stdout);
    fflush(stdout);
}

/* ----------------------------------------------------------------
 *  Input handling
 * ---------------------------------------------------------------- */

static void handle_input(void)
{
    char buf[16];
    while (kbhit()) {
        int n = (int)read(STDIN_FILENO, buf, sizeof(buf));
        for (int i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)buf[i];
            if (ch == 27 && i + 2 < n && buf[i+1] == '[') {
                /* arrow keys */
                switch (buf[i+2]) {
                    case 'A': if (!auto_fly) fly_spd =  3.0f; break;
                    case 'B': if (!auto_fly) fly_spd = -3.0f; break;
                    case 'C': heading += 0.08f; break;
                    case 'D': heading -= 0.08f; break;
                }
                i += 2;
            } else if (ch == 27) {
                running = 0;           /* Escape → quit */
            } else {
                switch (ch) {
                case 3:                 /* Ctrl-C */
                    running = 0; break;
                case 'w': case 'W':
                    if (!auto_fly) fly_spd =  3.5f; break;
                case 's': case 'S':
                    if (!auto_fly) fly_spd = -3.5f; break;
                case 'a': case 'A':
                    heading -= 0.09f; break;
                case 'd': case 'D':
                    heading += 0.09f; break;
                case 'q': case 'Q':
                    cam_vz = 2.0f; break;
                case 'e': case 'E':
                    cam_vz = -2.0f; break;
                case '+': case '=':
                    fly_spd *= 1.3f; break;
                case '-': case '_':
                    fly_spd *= 0.7f; break;
                case ' ':
                    auto_fly = !auto_fly;
                    fly_spd = auto_fly ? 2.8f : 0.0f;
                    break;
                case 'm': case 'M':
                    show_map = !show_map; break;
                }
            }
        }
    }
}

/* ----------------------------------------------------------------
 *  Camera / world update
 * ---------------------------------------------------------------- */

static void update(void)
{
    if (auto_fly) {
        /* cinematic flight: follow terrain contours */
        cx += sinf(heading) * fly_spd;
        cy += cosf(heading) * fly_spd;

        /* gentle sinusoidal turning for scenic route */
        heading += 0.005f * sinf(cx * 0.004f) * cosf(cy * 0.003f);

        /* altitude follows terrain */
        int mx = ((int)cx) & MAP_MASK;
        int my = ((int)cy) & MAP_MASK;
        float target_z = hmap[my][mx] + 45.0f;
        cz += (target_z - cz) * 0.04f;
    } else {
        cx += sinf(heading) * fly_spd;
        cy += cosf(heading) * fly_spd;
        cz += cam_vz;
        cam_vz *= 0.85f;          /* vertical damping */
        /* clamp altitude */
        int mx = ((int)cx) & MAP_MASK;
        int my = ((int)cy) & MAP_MASK;
        float ground = hmap[my][mx] + 5.0f;
        if (cz < ground) cz = ground;
    }

    /* wrap around map edges */
    cx = fmodf(cx, (float)MAP_W); if (cx < 0) cx += MAP_W;
    cy = fmodf(cy, (float)MAP_W); if (cy < 0) cy += MAP_W;

    /* decelerate in manual mode */
    if (!auto_fly) fly_spd *= 0.92f;

    frame_no++;
}

/* ----------------------------------------------------------------
 *  Main
 * ---------------------------------------------------------------- */

int main(void)
{
    srand((unsigned)time(NULL));

    printf("\033[2J\033[H");
    printf("\033[36m  ╔═══════════════════════════════════════════╗\n");
    printf("  ║     \033[37mVoxel Space Terrain Flythrough\033[36m       ║\n");
    printf("  ║                                           ║\n");
    printf("  ║  \033[33mProcedural island · ASCII rendering\033[36m      ║\n");
    printf("  ╚═══════════════════════════════════════════╝\033[0m\n\n");
    printf("  Generating terrain, please wait...\n");
    fflush(stdout);

    gen_terrain();

    /* find a nice starting position on land */
    for (int attempt = 0; attempt < 200; attempt++) {
        int sx = rand() & MAP_MASK;
        int sy = rand() & MAP_MASK;
        if (hmap[sy][sx] > 80.0f && hmap[sy][sx] < 180.0f) {
            cx = (float)sx;
            cy = (float)sy;
            cz = hmap[sy][sx] + 45.0f;
            break;
        }
    }

    /* terminal setup */
    term_raw();
    atexit(term_restore);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    printf("\033[?25l");           /* hide cursor */

    /* main loop */
    while (running) {
        handle_input();
        update();
        render();
        usleep(FRAME_US);
    }

    /* clean exit */
    printf("\033[?25h");           /* show cursor */
    printf("\033[2J\033[H\033[37m");
    printf("  Safe landing!  Explored (%.0f, %.0f) at altitude %.0f\n\n", cx, cy, cz);
    printf("\033[0m");
    return 0;
}
