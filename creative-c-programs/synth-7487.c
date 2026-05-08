/*
 * ================================================================
 *  synth.c — Terminal Chiptune Synthesizer
 * ================================================================
 *  An interactive chiptune music synthesizer in your terminal.
 *
 *  FEATURES:
 *   • Real-time keyboard piano (2 octaves)
 *   • 4 waveforms: sine, square, sawtooth, triangle
 *   • ASCII oscilloscope with colour-coded waveform display
 *   • 4 built-in demo songs (Ode to Joy, Tetris, Mary Had a
 *     Little Lamb, Für Elise)
 *   • Song playback with note-by-note visual tracking
 *   • WAV file export (44.1 kHz 16-bit mono PCM)
 *   • Audio playback via aplay / paplay / afplay (auto-detected)
 *
 *  COMPILE:
 *    gcc -std=c11 -O2 -o synth synth-7487.c -lm
 *
 *  RUN:
 *    ./synth                   Interactive piano mode
 *    ./synth play 1            Play demo song #1 (1-4)
 *    ./synth export 1 out.wav  Export demo song #1 to WAV
 *
 *  PIANO KEYS (two octaves):
 *    Lower: Z S X D C V G B H N J M   →  C4 C#4 D4 ... B4
 *    Upper: Q 2 W 3 E R 5 T 6 Y 7 U   →  C5 C#5 D5 ... B5
 *
 *  CONTROLS:
 *    [1]-[4]    Select waveform (sine / square / saw / tri)
 *    [+]/[-]    Volume up / down
 *    [,]/[.]    Octave down / up
 *    P + 1-4    Play demo song
 *    O          Export last played song to synth_output.wav
 *    Q / Esc    Quit
 * ================================================================
 */

#define _DEFAULT_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <signal.h>

/* ── Constants ─────────────────────────────────────────────── */

#define SR      44100           /* sample rate                        */
#define MAXSAMP (SR * 180)      /* max samples (3 minutes)            */
#define PI      3.14159265358979323846

/* ── Types ─────────────────────────────────────────────────── */

typedef enum { W_SINE, W_SQUARE, W_SAW, W_TRI } WaveType;

typedef struct { int midi; double beats; } Note;
typedef struct {
    const char *name;
    int bpm;
    Note *notes;
    int len;
} Song;

/* ── Global state ──────────────────────────────────────────── */

static struct termios g_orig_term;
static int16_t       *g_buf;            /* audio buffer            */
static volatile int   g_running = 1;

/* ── Signal handler ────────────────────────────────────────── */

static void on_sigint(int sig) { (void)sig; g_running = 0; }

/* ── MIDI → frequency ──────────────────────────────────────── */

static double mtof(int midi)
{
    return midi > 0 ? 440.0 * pow(2.0, (midi - 69) / 12.0) : 0.0;
}

/* ── Waveform generator ────────────────────────────────────── */

static float osc(WaveType t, float phase)
{
    float p = phase / (2.0f * (float)PI);
    switch (t) {
    case W_SINE:   return sinf(phase);
    case W_SQUARE: return sinf(phase) >= 0 ? 0.75f : -0.75f;
    case W_SAW:    return 2.0f * (p - floorf(p)) - 1.0f;
    case W_TRI:    return 2.0f * fabsf(2.0f * (p - floorf(p + 0.5f))) - 1.0f;
    }
    return 0.0f;
}

/* ── Generate one note into buffer ─────────────────────────── */

static int gen_note(int16_t *buf, int midi, double dur,
                    WaveType wt, float vol)
{
    int n = (int)(SR * dur);
    if (n <= 0) return 0;
    float f = (float)mtof(midi);
    if (f == 0) { memset(buf, 0, n * sizeof(int16_t)); return n; }

    float dphi = 2.0f * (float)PI * f / (float)SR;
    float phase = 0.0f;
    int atk = SR / 40;          /* 25 ms attack                   */
    int rel = SR / 6;           /* ~166 ms release                */
    float amp = vol * 32000.0f;

    for (int i = 0; i < n; i++) {
        float env;
        if (i < atk)         env = (float)i / (float)atk;
        else if (i > n-rel)  env = (float)(n - i) / (float)rel;
        else                 env = 0.85f;

        buf[i] = (int16_t)(osc(wt, phase) * env * amp);
        phase += dphi;
    }
    return n;
}

/* ── Render entire song to buffer ──────────────────────────── */

static int render_song(const Song *s, WaveType wt, float vol,
                       int16_t *buf, int maxlen)
{
    int pos = 0;
    double beat_s = 60.0 / s->bpm;
    for (int i = 0; i < s->len; i++) {
        double dur = s->notes[i].beats * beat_s;
        int need = (int)(SR * dur) + SR / 4;     /* +250ms margin */
        if (pos + need >= maxlen) break;
        pos += gen_note(buf + pos, s->notes[i].midi, dur, wt, vol);
    }
    return pos;
}

/* ── WAV writer ────────────────────────────────────────────── */

static int write_wav(const char *path, const int16_t *data, int count)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror("write_wav"); return -1; }

    uint32_t data_sz = (uint32_t)(count * 2);
    uint32_t riff_sz = 36 + data_sz;
    uint32_t fmt_sz  = 16;
    uint32_t sr      = SR;
    uint32_t bps     = SR * 2;          /* byte rate             */
    uint16_t pcm     = 1;
    uint16_t ch      = 1;
    uint16_t align   = 2;
    uint16_t bits    = 16;

    fwrite("RIFF", 1, 4, f);
    fwrite(&riff_sz, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    fwrite(&fmt_sz,  4, 1, f);
    fwrite(&pcm,     2, 1, f);
    fwrite(&ch,      2, 1, f);
    fwrite(&sr,      4, 1, f);
    fwrite(&bps,     4, 1, f);
    fwrite(&align,   2, 1, f);
    fwrite(&bits,    2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_sz, 4, 1, f);
    fwrite(data, 2, (size_t)count, f);

    fclose(f);
    return 0;
}

/* ── Try to play a WAV via system audio ────────────────────── */

static void play_wav_bg(const char *path)
{
    char cmd[512];
    if (system("which aplay   >/dev/null 2>&1") == 0)
        snprintf(cmd, sizeof cmd, "aplay -q '%s' &", path);
    else if (system("which paplay  >/dev/null 2>&1") == 0)
        snprintf(cmd, sizeof cmd, "paplay '%s' &", path);
    else if (system("which afplay  >/dev/null 2>&1") == 0)
        snprintf(cmd, sizeof cmd, "afplay '%s' &", path);
    else {
        fprintf(stderr, "No audio player found. WAV saved to %s\n", path);
        return;
    }
    (void)system(cmd);
}

/* ── Terminal helpers ──────────────────────────────────────── */

static void term_raw(void)
{
    tcgetattr(STDIN_FILENO, &g_orig_term);
    struct termios t = g_orig_term;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    (void)write(STDOUT_FILENO, "\033[?25l", 6);   /* hide cursor */
}

static void term_restore(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_term);
    (void)write(STDOUT_FILENO, "\033[?25h", 6);   /* show cursor */
}

static int get_cols(void)
{
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    return w.ws_col > 50 ? w.ws_col : 80;
}

static int get_rows(void)
{
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    return w.ws_row > 20 ? w.ws_row : 24;
}

static int read_key(void)
{
    unsigned char c;
    int n = (int)read(STDIN_FILENO, &c, 1);
    if (n != 1) return -1;
    if (c == 27) {                          /* escape sequence    */
        struct timeval tv = {0, 50000};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
            (void)read(STDIN_FILENO, &c, 1);      /* skip '['           */
            if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
                (void)read(STDIN_FILENO, &c, 1);
                /* translate arrow keys to mnemonics */
                switch (c) {
                case 'A': return 'U';       /* up     */
                case 'B': return 'D';       /* down   */
                case 'C': return 'R';       /* right  */
                case 'D': return 'L';       /* left   */
                }
            }
        }
        return 27;                         /* bare Escape        */
    }
    return (int)c;
}

/* ── Demo songs ────────────────────────────────────────────── */

static Note n_ode[] = {
    {64,1},{64,1},{65,1},{67,1},{67,1},{65,1},{64,1},{62,1},
    {60,1},{60,1},{62,1},{64,1},{64,1.5},{62,.5},{62,2},
    {64,1},{64,1},{65,1},{67,1},{67,1},{65,1},{64,1},{62,1},
    {60,1},{60,1},{62,1},{64,1},{62,1.5},{60,.5},{60,2},
    {62,1},{62,1},{64,1},{60,1},{62,1},{64,.5},{65,.5},{64,1},{60,1},
    {62,1},{64,.5},{65,.5},{64,1},{62,1},{60,1},{62,1},{67,2},
    {64,1},{64,1},{65,1},{67,1},{67,1},{65,1},{64,1},{62,1},
    {60,1},{60,1},{62,1},{64,1},{62,1.5},{60,.5},{60,2},
};

static Note n_tetris[] = {
    {71,1},{71,1},{72,1},{74,1},{74,1},{72,1},{71,1},{69,1},
    {67,1},{67,1},{69,1},{71,1},{71,1.5},{69,.5},{69,2},
    {71,1},{71,1},{72,1},{74,1},{74,1},{72,1},{71,1},{69,1},
    {67,1},{67,1},{69,1},{71,1},{69,1.5},{67,.5},{67,2},
    {69,1},{69,1},{71,1},{67,1},{69,1},{71,.5},{72,.5},{71,1},{67,1},
    {69,1},{71,.5},{72,.5},{71,1},{69,1},{67,1},{69,1},{72,2},
    {71,1},{71,1},{72,1},{74,1},{74,1},{72,1},{71,1},{69,1},
    {67,1},{67,1},{69,1},{71,1},{69,1.5},{67,.5},{67,2},
};

static Note n_mary[] = {
    {67,1},{65,1},{64,1},{65,1},{67,1},{67,1},{67,2},
    {65,1},{65,1},{65,2},{67,1},{71,1},{71,2},
    {67,1},{65,1},{64,1},{65,1},{67,1},{67,1},{67,1},{67,1},
    {65,1},{65,1},{67,1},{65,1},{64,4},
};

static Note n_elise[] = {
    {76,.5},{75,.5},{76,.5},{75,.5},{76,.5},{71,.5},{74,.5},{72,.5},{69,1},
    {60,.5},{64,.5},{69,.5},{71,1},{64,.5},{68,.5},{71,.5},{72,1},
    {64,.5},{76,.5},{75,.5},{76,.5},{75,.5},{76,.5},{71,.5},{74,.5},{72,.5},{69,1},
    {60,.5},{64,.5},{69,.5},{71,1},{64,.5},{72,.5},{71,.5},{69,2},
};

static Song g_songs[] = {
    {"Ode to Joy  (Beethoven)",   120, n_ode,    (int)(sizeof n_ode    / sizeof(Note))},
    {"Tetris Theme (Korobeiniki)",144, n_tetris, (int)(sizeof n_tetris / sizeof(Note))},
    {"Mary Had a Little Lamb",    100, n_mary,   (int)(sizeof n_mary   / sizeof(Note))},
    {"Fur Elise   (Beethoven)",   160, n_elise,  (int)(sizeof n_elise  / sizeof(Note))},
};
#define NUM_SONGS 4

/* ── Note names & colours ──────────────────────────────────── */

static const char *g_nnames[] = {
    "C ","C#","D ","D#","E ","F ","F#","G ","G#","A ","A#","B "
};

static const char *g_wave_name[] = { "SINE", "SQUARE", "SAW", "TRIANGLE" };
static const int   g_wave_color[] = { 46, 226, 214, 51 };   /* ANSI 256 */

/* ── Key → MIDI mapping ────────────────────────────────────── */

static int key_to_midi(char c, int base_oct)
{
    /* Lower octave: Z S X D C V G B H N J M  →  C..B */
    static const struct { char k; int off; } lo[] = {
        {'z',0},{'s',1},{'x',2},{'d',3},{'c',4},{'v',5},
        {'g',6},{'b',7},{'h',8},{'n',9},{'j',10},{'m',11},
    };
    /* Upper octave: Q 2 W 3 E R 5 T 6 Y 7 U  →  C..B */
    static const struct { char k; int off; } hi[] = {
        {'q',0},{'2',1},{'w',2},{'3',3},{'e',4},{'r',5},
        {'5',6},{'t',7},{'6',8},{'y',9},{'7',10},{'u',11},
    };
    int base = (base_oct + 1) * 12;        /* MIDI note of C in base_oct */
    for (int i = 0; i < 12; i++) {
        if (c == lo[i].k) return base + lo[i].off;
        if (c == hi[i].k) return base + 12 + hi[i].off;
    }
    return -1;
}

/* ── Drawing: oscilloscope ─────────────────────────────────── */

static void draw_scope(const int16_t *buf, int len, int w, int h,
                       WaveType wt)
{
    int mid = h / 2;

    /* character grid */
    char grid[h][w + 1];
    for (int y = 0; y < h; y++) {
        memset(grid[y], ' ', w);
        grid[y][w] = '\0';
    }

    /* centre line */
    for (int x = 0; x < w; x++) grid[mid][x] = 249;   /* · */

    /* plot waveform samples */
    for (int x = 0; x < w; x++) {
        int idx = (int)((double)x / w * len);
        if (idx >= len) idx = len - 1;
        double v = (double)buf[idx] / 32768.0;
        int y = mid - (int)(v * (mid - 1));
        if (y < 0) y = 0;
        if (y >= h) y = h - 1;

        /* mark the wave crest */
        grid[y][x] = (y < mid) ? '/' : (y > mid) ? '\\' : '~';

        /* fill the area between wave and centre */
        int lo = y < mid ? y + 1 : mid + 1;
        int hi = y > mid ? y - 1 : mid - 1;
        for (int fy = lo; fy <= hi; fy++) grid[fy][x] = '|';
    }

    /* render with ANSI colours */
    printf("\033[38;5;238m  \xE2\x95\x94");         /* ╔ */
    for (int x = 0; x < w; x++) printf("\xE2\x95\x90");  /* ═ */
    printf("\xE2\x95\x97\033[0m\r\n");              /* ╗ */

    for (int y = 0; y < h; y++) {
        printf("\033[38;5;238m  \xE2\x95\x91\033[0m");    /* ║ */
        for (int x = 0; x < w; x++) {
            unsigned char c = (unsigned char)grid[y][x];
            if (c == '/' || c == '\\' || c == '~')
                printf("\033[38;5;%dm%c\033[0m",
                       g_wave_color[wt], c);
            else if (c == '|')
                printf("\033[38;5;%dm%c\033[0m",
                       g_wave_color[wt] - 18, c);
            else if (c == 249)
                printf("\033[38;5;236m%c\033[0m", c);
            else
                putchar(c);
        }
        printf("\033[38;5;238m\xE2\x95\x91\033[0m\r\n");  /* ║ */
    }

    printf("\033[38;5;238m  \xE2\x95\x9A");         /* ╚ */
    for (int x = 0; x < w; x++) printf("\xE2\x95\x90");
    printf("\xE2\x95\x9D\033[0m\r\n");              /* ╝ */
}

/* ── Drawing: piano keyboard ───────────────────────────────── */

static void draw_piano(int active, int base_oct)
{
    /* White keys: C D E F G A B → indices 0 2 4 5 7 9 11 */
    static const int white_idx[] = {0, 2, 4, 5, 7, 9, 11};
    /* Black keys: C# D# F# G# A# → indices 1 3 6 8 10 */
    static const int black_idx[] = {1, 3, 6, 8, 10};

    /* Keyboard labels (key cap on top, note name below) */
    static const char *lo_key[] = {
        "Z","S","X","D","C","V","G","B","H","N","J","M"
    };
    static const char *hi_key[] = {
        "Q","2","W","3","E","R","5","T","6","Y","7","U"
    };

    int base = (base_oct + 1) * 12;

    /* ── Draw two octaves of keys ── */
    for (int oct = 0; oct < 2; oct++) {
        const char **keys = (oct == 0) ? lo_key : hi_key;
        int o_base = base + oct * 12;

        printf("  \033[38;5;248mOct %d\033[0m  ", base_oct + oct);

        /* Row 1: black keys (positioned above white keys) */
        for (int i = 0; i < 7; i++) {
            int w_midi __attribute__((unused)) = o_base + white_idx[i];
            /* check if a black key exists before this white key */
            if (i == 1 || i == 2 || i == 4 || i == 5 || i == 6) {
                /* there's a black key before white key i */
                /* skip for first white key (C has no preceding black) */
            }
            printf("   ");
        }
        printf("\r\n         ");

        /* Black keys row */
        int bi = 0;
        for (int i = 0; i < 7; i++) {
            /* Black key between white keys? */
            int has_black = (i == 0 || i == 1 || i == 3 || i == 4 || i == 5);
            if (has_black && bi < 5) {
                int midi = o_base + black_idx[bi];
                int act = (midi == active);
                if (act)
                    printf("\033[48;5;46;30m");
                else
                    printf("\033[48;5;236;37m");
                printf(" %s", keys[black_idx[bi]]);
                bi++;
                printf(" \033[0m");
            } else {
                printf("   ");
            }
        }
        printf("\r\n  ");

        /* White keys row */
        for (int i = 0; i < 7; i++) {
            int midi = o_base + white_idx[i];
            int act = (midi == active);
            if (act)
                printf("\033[48;5;46;30m");
            else
                printf("\033[48;5;254;30m");
            printf(" %s ", keys[white_idx[i]]);
            printf("\033[0m");
        }
        printf("\r\n  ");

        /* Note names under white keys */
        for (int i = 0; i < 7; i++) {
            int midi = o_base + white_idx[i];
            int act = (midi == active);
            if (act)
                printf("\033[38;5;46m");
            else
                printf("\033[38;5;244m");
            printf(" %s ", g_nnames[white_idx[i]]);
            printf("\033[0m");
        }
        printf("\r\n");
    }
}

/* ── Drawing: status bar ───────────────────────────────────── */

static void draw_status(WaveType wt, int oct, float vol, int active,
                        int playing_song)
{
    int w = get_cols() - 4;
    if (w > 74) w = 74;

    printf("\033[38;5;238m  \xE2\x94\x8C");         /* ┌ */
    for (int i = 0; i < w; i++) printf("\xE2\x94\x80");  /* ─ */
    printf("\xE2\x94\x90\033[0m\r\n");              /* ┐ */

    printf("  \033[38;5;238m\xE2\x94\x82\033[0m");          /* │ */

    /* Title */
    printf(" \033[1;38;5;220mSYNTH\033[0m  ");

    /* Waveform indicator */
    printf("Wave:");
    for (int i = 0; i < 4; i++) {
        if (i == (int)wt)
            printf("\033[38;5;%dm\033[1m[%s]\033[0m ",
                   g_wave_color[i], g_wave_name[i]);
    }

    /* Octave */
    printf(" Oct:\033[1m%d\033[0m ", oct);

    /* Volume bar */
    printf("Vol:");
    int bars = (int)(vol * 12);
    printf("\033[38;5;28m");
    for (int i = 0; i < 12; i++)
        fputs(i < bars ? "\xE2\x96\x88" : "\xE2\x96\x91", stdout);
    printf("\033[0m");

    /* Active note */
    if (active > 0) {
        const char *n = g_nnames[(active - 60) % 12];
        int o = (active - 12) / 12;
        printf(" \033[38;5;46m\xE2\x99\xAA %s%d\033[0m", n, o);
    }

    /* Song indicator */
    if (playing_song >= 0)
        printf(" \033[38;5;226m\xE2\x99\xAB Playing...\033[0m");

    /* Pad to width */
    printf("\033[38;5;238m\xE2\x94\x82\033[0m\r\n");        /* │ */

    printf("\033[38;5;238m  \xE2\x94\x94");         /* └ */
    for (int i = 0; i < w; i++) printf("\xE2\x94\x80");
    printf("\xE2\x94\x98\033[0m\r\n");              /* ┘ */
}

/* ── Drawing: help bar ─────────────────────────────────────── */

static void draw_help(void)
{
    printf("\r\n"
           "  \033[38;5;244m"
           "[1-4] Wave   [+/-] Volume   [</>] Octave   "
           "[P 1-4] Song   [O] Export   [Q] Quit"
           "\033[0m\r\n");
}

/* ── Full screen redraw ────────────────────────────────────── */

static void redraw(const int16_t *scope_buf, int scope_len,
                   WaveType wt, int oct, float vol, int active,
                   int playing_song)
{
    int cols = get_cols();
    int rows = get_rows();
    int scope_w = cols - 8;
    if (scope_w > 68) scope_w = 68;
    if (scope_w < 30) scope_w = 30;
    int scope_h = rows - 16;
    if (scope_h > 10) scope_h = 10;
    if (scope_h < 4)  scope_h = 4;

    printf("\033[H");                         /* cursor home   */

    draw_status(wt, oct, vol, active, playing_song);
    printf("\r\n");
    draw_scope(scope_buf, scope_len, scope_w, scope_h, wt);
    printf("\r\n");
    draw_piano(active, oct);
    draw_help();
}

/* ── Song playback with visual feedback ────────────────────── */

static void play_song_interactive(const Song *s, WaveType wt, float vol)
{
    int scope_w = get_cols() - 8;
    if (scope_w > 68) scope_w = 68;
    if (scope_w < 30) scope_w = 30;
    int scope_h = get_rows() - 16;
    if (scope_h > 10) scope_h = 10;
    if (scope_h < 4)  scope_h = 4;

    double beat_s = 60.0 / s->bpm;

    /* First render entire song to buffer for audio playback */
    int total = render_song(s, wt, vol, g_buf, MAXSAMP);
    write_wav("/tmp/synth_playback.wav", g_buf, total);
    play_wav_bg("/tmp/synth_playback.wav");

    /* Now animate note-by-note */
    for (int i = 0; i < s->len && g_running; i++) {
        int midi = s->notes[i].midi;
        double dur = s->notes[i].beats * beat_s;

        /* Generate scope samples for this note */
        int n = gen_note(g_buf, midi, dur, wt, vol);

        /* Redraw with this note highlighted */
        printf("\033[H");
        draw_status(wt, 4, vol, midi, 1);
        printf("\r\n");
        draw_scope(g_buf, n, scope_w, scope_h, wt);
        printf("\r\n");
        draw_piano(midi, 4);
        draw_help();

        /* Wait for the duration of this note */
        int ms = (int)(dur * 1000);
        int steps = ms / 50;
        for (int si = 0; si < steps && g_running; si++) {
            usleep(50000);
            /* check for quit */
            int ch = read_key();
            if (ch == 'q' || ch == 27) { g_running = 0; }
        }
    }
}

/* ── Main: interactive mode ────────────────────────────────── */

static void interactive(void)
{
    WaveType wt = W_SINE;
    int oct = 4;
    float vol = 0.6f;
    int active = 0;
    int scope_len = SR / 4;                /* 250ms default scope */

    /* Generate initial scope (middle C) */
    scope_len = gen_note(g_buf, 60, 0.25, wt, vol);

    redraw(g_buf, scope_len, wt, oct, vol, active, -1);
    fflush(stdout);

    while (g_running) {
        int ch = read_key();
        if (ch < 0) { usleep(20000); continue; }

        switch (ch) {
        case 'q': case 27:
            g_running = 0;
            break;

        /* Waveform select */
        case '1': wt = W_SINE;   break;
        case '2': wt = W_SQUARE; break;
        case '3': wt = W_SAW;    break;
        case '4': wt = W_TRI;    break;

        /* Volume */
        case '+': case '=':
            vol = vol < 0.95f ? vol + 0.05f : 1.0f;
            break;
        case '-':
            vol = vol > 0.05f ? vol - 0.05f : 0.0f;
            break;

        /* Octave */
        case ',': case '<':
            oct = oct > 1 ? oct - 1 : 1;
            break;
        case '.': case '>':
            oct = oct < 7 ? oct + 1 : 7;
            break;

        /* Play song */
        case 'p': case 'P': {
            int sn = -1;
            usleep(100000);
            int c2 = read_key();
            if (c2 >= '1' && c2 <= '4') sn = c2 - '1';
            if (sn >= 0 && sn < NUM_SONGS) {
                play_song_interactive(&g_songs[sn], wt, vol);
            }
            break;
        }

        /* Export last rendered song */
        case 'o': case 'O': {
            /* Re-render all songs concatenated */
            int total = 0;
            for (int i = 0; i < NUM_SONGS; i++) {
                total += render_song(&g_songs[i], wt, vol,
                                     g_buf + total, MAXSAMP - total);
            }
            write_wav("synth_output.wav", g_buf, total);
            printf("\033[%d;1H  \033[38;5;46mExported %d samples "
                   "to synth_output.wav\033[0m",
                   get_rows(), total);
            fflush(stdout);
            break;
        }

        /* Arrow key volume/octave shortcuts */
        case 'U': vol = vol < 0.95f ? vol + 0.05f : 1.0f; break;
        case 'D': vol = vol > 0.05f ? vol - 0.05f : 0.0f; break;
        case 'L': oct = oct > 1 ? oct - 1 : 1; break;
        case 'R': oct = oct < 7 ? oct + 1 : 7; break;

        default: {
            /* Try to map to a piano key */
            int midi = key_to_midi((char)ch, oct);
            if (midi > 0) {
                active = midi;
                scope_len = gen_note(g_buf, midi, 0.3, wt, vol);

                /* Write mini WAV and play */
                char path[64];
                snprintf(path, sizeof path,
                         "/tmp/synth_note_%d.wav", midi);
                write_wav(path, g_buf, scope_len);
                play_wav_bg(path);
            }
            break;
        }
        }

        /* Reset active note display after a moment */
        if (active > 0 && ch < 0) {
            static int fade = 0;
            if (++fade > 6) { active = 0; fade = 0; }
        }

        redraw(g_buf, scope_len, wt, oct, vol, active, -1);
        fflush(stdout);
    }
}

/* ── Main: CLI mode ────────────────────────────────────────── */

static void usage(const char *prog)
{
    printf("Usage:\n"
           "  %s                   Interactive piano mode\n"
           "  %s play <1-4>        Play demo song\n"
           "  %s export <1-4> [file.wav]\n"
           "                       Export demo song to WAV\n"
           "\nSongs:\n", prog, prog, prog);
    for (int i = 0; i < NUM_SONGS; i++)
        printf("  %d. %s (%d BPM, %d notes)\n",
               i + 1, g_songs[i].name, g_songs[i].bpm, g_songs[i].len);
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);

    g_buf = calloc(MAXSAMP, sizeof(int16_t));
    if (!g_buf) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    /* ── CLI subcommands ── */
    if (argc >= 3 && strcmp(argv[1], "play") == 0) {
        int idx = atoi(argv[2]) - 1;
        if (idx < 0 || idx >= NUM_SONGS) { usage(argv[0]); free(g_buf); return 1; }
        int n = render_song(&g_songs[idx], W_SINE, 0.7f, g_buf, MAXSAMP);
        printf("Playing: %s\n", g_songs[idx].name);
        write_wav("/tmp/synth_cli.wav", g_buf, n);
        play_wav_bg("/tmp/synth_cli.wav");
        printf("Press Ctrl-C to stop.\n");
        while (g_running) usleep(200000);
        free(g_buf);
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "export") == 0) {
        int idx = atoi(argv[2]) - 1;
        if (idx < 0 || idx >= NUM_SONGS) { usage(argv[0]); free(g_buf); return 1; }
        const char *out = (argc >= 4) ? argv[3] : "synth_output.wav";
        int n = render_song(&g_songs[idx], W_SINE, 0.8f, g_buf, MAXSAMP);
        write_wav(out, g_buf, n);
        printf("Exported %s → %s (%d samples, %.1fs)\n",
               g_songs[idx].name, out, n, (double)n / SR);
        free(g_buf);
        return 0;
    }

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage(argv[0]);
        free(g_buf);
        return 0;
    }

    /* ── Interactive mode ── */
    printf("\033[2J\033[H");                /* clear screen        */
    term_raw();
    atexit(term_restore);

    interactive();

    printf("\033[2J\033[H");
    printf("\033[38;5;46mThanks for playing! \xE2\x99\xAB\033[0m\n");
    free(g_buf);
    return 0;
}
