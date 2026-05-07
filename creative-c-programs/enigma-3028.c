/*
 * ================================================================
 *  ENIGMA MACHINE SIMULATOR  ─  enigma-3028.c
 * ================================================================
 *  A historically accurate simulation of the WW2 German Enigma
 *  I / M3 cipher machine with authentic rotor wiring, plugboard,
 *  reflectors, and the famous double-stepping mechanism.
 *
 *  Features:
 *    - 8 historical rotors (I-VIII) with authentic wiring
 *    - 3 reflectors (UKW-A, UKW-B, UKW-C)
 *    - Plugboard (Steckerbrett) with configurable pairs
 *    - Ring settings (Ringstellung) & initial positions
 *    - Double-stepping anomaly (historically accurate)
 *    - Signal-path tracing mode (educational)
 *    - 3 preset configurations + custom setup
 *    - Interactive & command-line modes
 *
 *  COMPILE:  gcc -std=c11 -Wall -Wextra -o enigma enigma-3028.c
 *  RUN:      ./enigma                           (interactive)
 *            ./enigma -e "HELLO WORLD"           (quick encrypt)
 *            ./enigma -f input.txt output.txt    (file mode)
 *
 *  REQUIRES: C11 compiler, ANSI terminal. No external libraries.
 * ================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── ANSI escape codes ────────────────────────────────────────── */
#define RST "\033[0m"
#define BLD "\033[1m"
#define DIM "\033[2m"
#define RED "\033[31m"
#define GRN "\033[32m"
#define YEL "\033[33m"
#define BLU "\033[34m"
#define CYN "\033[36m"
#define WHT "\033[37m"

/* ── Constants ────────────────────────────────────────────────── */
#define N_ALPHA  26
#define SLOTS    3
#define MAX_BUF  8192

/* ══════════════════════════════════════════════════════════════════
   Historical Rotor & Reflector Data (Enigma I / Naval M3)
   ══════════════════════════════════════════════════════════════════ */

typedef struct {
    const char wiring[27];  /* Forward substitution A→? */
    const char notch[3];    /* Turnover notch position(s) */
    const char name[8];     /* Display label */
} RotorDef;

typedef struct {
    const char wiring[27];
    const char name[8];
} ReflDef;

static const RotorDef ROTORS[] = {
    {"EKMFLGDQVZNTOWYHXUSPAIBRCJ", "Q",  "I"   },
    {"AJDKSIRUXBLHWTMCQGZNPYFVOE", "E",  "II"  },
    {"BDFHJLCPRTXVZNYEIWGAKMUSQO", "V",  "III" },
    {"ESOVPZJAYQUIRHXLNFTGKDCMWB", "J",  "IV"  },
    {"VZBRGITYUPSDNHLXAWMJQOFECK", "Z",  "V"   },
    {"JPGVOUMFYQBENHZRDKASXLICTW", "ZM", "VI"  },
    {"NZJHGRCXMYSWBOUFAIVLPEKQDT", "ZM", "VII" },
    {"FKQHTLXOCBJSPDZRAMEWNIUYGV", "ZM", "VIII"},
};
#define N_ROTOR (int)(sizeof ROTORS / sizeof *ROTORS)

/* Reflectors are self-inverse involutions (A↔B implies B↔A) */
static const ReflDef REFLS[] = {
    {"EJMZALYXVBWFCRQUONTSPIKHGD", "UKW-A"},
    {"YRUHQSLDPXNGOKMIEBFZCWVJAT", "UKW-B"},
    {"FVPJIAOYEDRZXWGCTKUQSBNMHL", "UKW-C"},
};
#define N_REFL (int)(sizeof REFLS / sizeof *REFLS)

/* ── Machine state ────────────────────────────────────────────── */

typedef struct {
    int  type;         /* Index into ROTORS[] */
    int  pos;          /* Window position 0-25 */
    int  ring;         /* Ring setting 0-25 */
    char inv[27];      /* Precomputed inverse wiring */
} Rotor;

typedef struct {
    Rotor r[SLOTS];    /* [0]=Left  [1]=Middle  [2]=Right */
    int   refl;        /* Reflector index */
    int   pb[N_ALPHA]; /* Plugboard mapping: pb[x] = paired letter or x */
    int   npb;         /* Number of plug pairs */
    int   trace;       /* Signal-path trace flag */
    int   tv[10];      /* Trace values at each stage */
} Enigma;

/* ══════════════════════════════════════════════════════════════════
   Core Cipher Engine
   ══════════════════════════════════════════════════════════════════ */

/* Compute inverse wiring from forward wiring */
static void calc_inv(const char fwd[27], char inv[27]) {
    for (int i = 0; i < N_ALPHA; i++)
        inv[fwd[i] - 'A'] = (char)('A' + i);
}

/* Is this rotor currently sitting at its turnover notch? */
static int at_notch(const Rotor *r) {
    char c = (char)('A' + r->pos);
    const char *n = ROTORS[r->type].notch;
    return c == n[0] || (n[1] && c == n[1]);
}

/*
 * Step the rotors.  Called BEFORE encrypting each character.
 * Implements the famous "double-stepping" anomaly:
 *   1) If middle rotor is at its notch → step both left & middle
 *   2) If right rotor is at its notch  → step middle
 *   3) Always step right rotor
 * Rule 1 + rule 2 can both fire, causing the middle rotor to
 * advance twice on a single keystroke — the historical bug/feature
 * that Alan Turing exploited.
 */
static void step_rotors(Enigma *e) {
    int mid = at_notch(&e->r[1]);
    int rgt = at_notch(&e->r[2]);
    if (mid) {
        e->r[0].pos = (e->r[0].pos + 1) % N_ALPHA;
        e->r[1].pos = (e->r[1].pos + 1) % N_ALPHA;
    }
    if (rgt) {
        e->r[1].pos = (e->r[1].pos + 1) % N_ALPHA;
    }
    e->r[2].pos = (e->r[2].pos + 1) % N_ALPHA;
}

/* Forward through a rotor (signal goes right → left) */
static int rotor_fwd(const Rotor *r, int c) {
    int in  = (c + r->pos - r->ring + 2 * N_ALPHA) % N_ALPHA;
    int out = ROTORS[r->type].wiring[in] - 'A';
    return (out - r->pos + r->ring + 2 * N_ALPHA) % N_ALPHA;
}

/* Backward through a rotor (signal goes left → right, uses inverse) */
static int rotor_bwd(const Rotor *r, int c) {
    int in  = (c + r->pos - r->ring + 2 * N_ALPHA) % N_ALPHA;
    int out = r->inv[in] - 'A';
    return (out - r->pos + r->ring + 2 * N_ALPHA) % N_ALPHA;
}

/*
 * Encrypt one character (0-25).
 * Full signal path:
 *   Input → Plugboard → R→M→L (fwd) → Reflector → L→M→R (bwd) → Plugboard → Output
 */
static int enc1(Enigma *e, int c) {
    step_rotors(e);
    int t = 0;
    if (e->trace) e->tv[t++] = c;
    c = e->pb[c];
    if (e->trace) e->tv[t++] = c;
    c = rotor_fwd(&e->r[2], c);
    if (e->trace) e->tv[t++] = c;
    c = rotor_fwd(&e->r[1], c);
    if (e->trace) e->tv[t++] = c;
    c = rotor_fwd(&e->r[0], c);
    if (e->trace) e->tv[t++] = c;
    c = REFLS[e->refl].wiring[c] - 'A';
    if (e->trace) e->tv[t++] = c;
    c = rotor_bwd(&e->r[0], c);
    if (e->trace) e->tv[t++] = c;
    c = rotor_bwd(&e->r[1], c);
    if (e->trace) e->tv[t++] = c;
    c = rotor_bwd(&e->r[2], c);
    if (e->trace) e->tv[t++] = c;
    c = e->pb[c];
    if (e->trace) e->tv[t++] = c;
    return c;
}

/* Encrypt a string, outputting letters in traditional 5-letter groups */
static void enc_str(Enigma *e, const char *in, char *out) {
    int j = 0, grp = 0;
    for (int i = 0; in[i]; i++) {
        if (isalpha((unsigned char)in[i])) {
            out[j++] = (char)('A' + enc1(e, toupper((unsigned char)in[i]) - 'A'));
            if (++grp % 5 == 0) out[j++] = ' ';
        }
    }
    if (j > 0 && out[j - 1] == ' ') j--;
    out[j] = '\0';
}

/* Initialize machine with default settings: III/II/I, UKW-B, no plugs */
static void machine_init(Enigma *e) {
    int defs[] = {2, 1, 0};
    for (int i = 0; i < SLOTS; i++) {
        e->r[i].type = defs[i];
        e->r[i].pos  = 0;
        e->r[i].ring = 0;
        calc_inv(ROTORS[defs[i]].wiring, e->r[i].inv);
    }
    e->refl = 1;
    for (int i = 0; i < N_ALPHA; i++) e->pb[i] = i;
    e->npb   = 0;
    e->trace = 0;
}

/* ══════════════════════════════════════════════════════════════════
   Display Functions
   ══════════════════════════════════════════════════════════════════ */

static void banner(void) {
    printf(BLD CYN
        "\n"
        "     ███████╗███╗   ██╗ █████╗ ██╗███╗   ██╗ ██████╗ \n"
        "     ██╔════╝████╗  ██║██╔══██╗██║████╗  ██║██╔════╝ \n"
        "     █████╗  ██╔██╗ ██║███████║██║██╔██╗ ██║██║  ███╗\n"
        "     ██╔══╝  ██║╚██╗██║██╔══██║██║██║╚██╗██║██║   ██║\n"
        "     ███████╗██║ ╚████║██║  ██║██║██║ ╚████║╚██████╔╝\n"
        "     ╚══════╝╚═╝  ╚═══╝╚═╝  ╚═╝╚═╝╚═╝  ╚═══╝ ╚═════╝ \n"
        RST "\n"
        BLD "     ── Enigma I / M3 Cipher Machine Simulator ──\n" RST
        DIM "     Historically accurate | 8 rotors | 3 reflectors | plugboard\n"
        "     Alan Turing & Bletchley Park broke this cipher in WWII\n\n" RST);
}

static void show_cfg(const Enigma *e) {
    const char *sn[] = {"Left", "Middle", "Right"};
    printf(BLD "\n  ╔══ Machine Configuration ═════════════════════════╗\n\n" RST);
    printf("  " BLD "Reflector:" RST " %s\n\n", REFLS[e->refl].name);
    printf("  " BLD "Rotors:\n" RST);
    for (int i = 0; i < SLOTS; i++)
        printf("    %-6s: " CYN BLD "%-5s" RST
               "  Window: " YEL BLD "%c" RST
               "  Ring: " GRN BLD "%c" RST "\n",
               sn[i], ROTORS[e->r[i].type].name,
               'A' + e->r[i].pos, 'A' + e->r[i].ring);

    printf("\n  " BLD "Plugboard:" RST " ");
    if (e->npb == 0) {
        printf(DIM "(none)\n" RST);
    } else {
        for (int i = 0; i < N_ALPHA; i++)
            if (e->pb[i] != i && i < e->pb[i])
                printf(YEL BLD "%c%c " RST, 'A' + i, 'A' + e->pb[i]);
        printf("\n");
    }

    /* Visual rotor window */
    printf("\n        ┌──┐  ┌──┐  ┌──┐\n");
    printf("        │" BLD WHT "%c" RST "│  │" BLD WHT "%c" RST "│  │" BLD WHT "%c" RST "│\n",
           'A' + e->r[0].pos, 'A' + e->r[1].pos, 'A' + e->r[2].pos);
    printf("        └──┘  └──┘  └──┘\n");
    printf("       " DIM " %-4s  %-4s  %-4s\n\n" RST,
           ROTORS[e->r[0].type].name,
           ROTORS[e->r[1].type].name,
           ROTORS[e->r[2].type].name);
    printf(BLD "  ╚══════════════════════════════════════════════════╝\n\n" RST);
}

static void show_help(void) {
    printf(BLD "\n  ── Commands ──────────────────────────────────────\n\n" RST
        "    " CYN "/help"   RST "   Show this help\n"
        "    " CYN "/config" RST " Show machine configuration\n"
        "    " CYN "/reset"  RST " Reset rotor positions to initial\n"
        "    " CYN "/path"   RST " Toggle signal-path tracing (educational)\n"
        "    " CYN "/quit"   RST " Exit\n\n"
        DIM "  Enigma is self-reciprocal: encrypting ciphertext with\n"
        "  identical settings recovers the plaintext.\n"
        "  Reset positions before decrypting!\n\n" RST);
}

/* Show the electrical signal path through the machine */
static void show_trace(const Enigma *e) {
    const char *lbl[] = {"IN", "PB", " R", " M", " L", "RF", " L", " M", " R", "PB"};
    printf("    " DIM "Signal: " RST);
    for (int i = 0; i < 10; i++) {
        if (i > 0) printf(DIM " → " RST);
        printf("%s" BLD "%c" RST, lbl[i], 'A' + e->tv[i]);
    }
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   Configuration & Presets
   ══════════════════════════════════════════════════════════════════ */

static char *readln(char *buf, int sz) {
    if (!fgets(buf, sz, stdin)) { buf[0] = 0; return buf; }
    buf[strcspn(buf, "\r\n")] = 0;
    return buf;
}

static void set_rotor(Enigma *e, int slot, int type, int pos, int ring) {
    e->r[slot].type = type;
    e->r[slot].pos  = pos;
    e->r[slot].ring = ring;
    calc_inv(ROTORS[type].wiring, e->r[slot].inv);
}

static void parse_plugs(Enigma *e, const char *s) {
    for (int i = 0; i < N_ALPHA; i++) e->pb[i] = i;
    e->npb = 0;
    if (!s) return;
    while (*s) {
        while (*s && !isalpha((unsigned char)*s)) s++;
        if (!*s) break;
        int a = toupper((unsigned char)*s++) - 'A';
        while (*s && !isalpha((unsigned char)*s)) s++;
        if (!*s) break;
        int b = toupper((unsigned char)*s++) - 'A';
        if (a != b && a < N_ALPHA && b < N_ALPHA) {
            e->pb[a] = b;
            e->pb[b] = a;
            e->npb++;
        }
    }
}

typedef struct {
    const char *name;
    int rotors[SLOTS];
    int pos[SLOTS];
    int ring[SLOTS];
    int refl;
    const char *plugs;
} Preset;

static const Preset PRESETS[] = {
    {"Enigma I Default (1938)",
     {2, 1, 0}, {0, 0, 0}, {0, 0, 0}, 1, ""},
    {"Wehrmacht 1940 Battle",
     {1, 3, 4}, {0, 0, 0}, {2, 5, 3}, 1, "AM FI NV PS TU WZ"},
    {"Kriegsmarine M3 Atlantic",
     {6, 3, 0}, {5, 11, 22}, {1, 8, 4}, 2, "AE BJ KO MQ WZ"},
    {"Custom Configuration",
     {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, 0, ""},
};
#define N_PRESET (int)(sizeof PRESETS / sizeof *PRESETS)

static void configure(Enigma *e) {
    printf(BLD "  Select a configuration:\n\n" RST);
    for (int i = 0; i < N_PRESET; i++)
        printf("    " CYN "%d)" RST " %s\n", i + 1, PRESETS[i].name);
    printf("\n");

    char buf[64];
    int choice = 0;
    for (;;) {
        printf("  Choice [1-%d]: ", N_PRESET);
        fflush(stdout);
        readln(buf, sizeof buf);
        if (sscanf(buf, "%d", &choice) == 1 && choice >= 1 && choice <= N_PRESET) break;
    }
    choice--;

    if (choice < N_PRESET - 1) {
        const Preset *p = &PRESETS[choice];
        for (int i = 0; i < SLOTS; i++)
            set_rotor(e, i, p->rotors[i], p->pos[i], p->ring[i]);
        e->refl = p->refl;
        parse_plugs(e, p->plugs);
        printf(GRN "\n  Loaded: %s\n" RST, p->name);
    } else {
        /* ── Custom configuration ── */
        printf("\n  " BLD "Select Reflector:\n" RST);
        for (int i = 0; i < N_REFL; i++)
            printf("    %d) %s\n", i + 1, REFLS[i].name);
        for (;;) {
            printf("  Choice [1-%d]: ", N_REFL);
            fflush(stdout);
            readln(buf, sizeof buf);
            int v;
            if (sscanf(buf, "%d", &v) == 1 && v >= 1 && v <= N_REFL) {
                e->refl = v - 1;
                break;
            }
        }

        const char *slot_name[] = {"Left", "Middle", "Right"};
        int used[SLOTS];
        for (int s = 0; s < SLOTS; s++) {
            printf("\n  " BLD "%s Rotor:\n" RST, slot_name[s]);
            for (int i = 0; i < N_ROTOR; i++) {
                int dup = 0;
                for (int j = 0; j < s; j++) if (used[j] == i) dup = 1;
                if (!dup) printf("    %d) %s\n", i + 1, ROTORS[i].name);
            }
            for (;;) {
                printf("  Rotor [1-%d]: ", N_ROTOR);
                fflush(stdout);
                readln(buf, sizeof buf);
                int v;
                if (sscanf(buf, "%d", &v) == 1 && v >= 1 && v <= N_ROTOR) {
                    v--;
                    int dup = 0;
                    for (int j = 0; j < s; j++) if (used[j] == v) dup = 1;
                    if (!dup) { used[s] = v; break; }
                    printf("    " RED "Already in use.\n" RST);
                }
            }

            printf("  Position [A-Z]: ");
            fflush(stdout);
            readln(buf, sizeof buf);
            int pos = isalpha((unsigned char)buf[0]) ? toupper((unsigned char)buf[0]) - 'A' : 0;

            printf("  Ring setting [A-Z]: ");
            fflush(stdout);
            readln(buf, sizeof buf);
            int ring = isalpha((unsigned char)buf[0]) ? toupper((unsigned char)buf[0]) - 'A' : 0;

            set_rotor(e, s, used[s], pos, ring);
        }

        printf("\n  " BLD "Plugboard pairs" RST " (e.g. " YEL "AB CD EF" RST "), Enter to skip: ");
        fflush(stdout);
        char pbuf[256];
        readln(pbuf, sizeof pbuf);
        parse_plugs(e, pbuf);
    }
    show_cfg(e);
}

/* ══════════════════════════════════════════════════════════════════
   Interactive Mode
   ══════════════════════════════════════════════════════════════════ */

static void interactive(Enigma *e) {
    int init_pos[SLOTS];
    for (int i = 0; i < SLOTS; i++) init_pos[i] = e->r[i].pos;

    printf(GRN BLD "  Ready. Type to encrypt. /help for commands.\n\n" RST);

    char buf[MAX_BUF];
    for (;;) {
        /* Prompt with rotor window */
        printf("  " BLD BLU "[%c %c %c] > " RST,
               'A' + e->r[0].pos, 'A' + e->r[1].pos, 'A' + e->r[2].pos);
        fflush(stdout);
        if (!readln(buf, sizeof buf)) break;

        char *p = buf;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;

        /* Commands */
        if (*p == '/') {
            if (strstr(p, "quit") || strstr(p, "exit")) break;
            else if (strstr(p, "help"))   show_help();
            else if (strstr(p, "config")) show_cfg(e);
            else if (strstr(p, "reset")) {
                for (int i = 0; i < SLOTS; i++) e->r[i].pos = init_pos[i];
                printf(YEL "  Positions reset.\n\n" RST);
            }
            else if (strstr(p, "path")) {
                e->trace = !e->trace;
                printf("  Signal tracing: %s\n\n",
                       e->trace ? GRN BLD "ON" RST : RED BLD "OFF" RST);
            }
            else printf("  " RED "Unknown command. Type /help\n\n" RST);
            continue;
        }

        /* Encrypt the line */
        char out[MAX_BUF];
        enc_str(e, p, out);
        printf("  " BLD YEL "= %s\n" RST, out);

        if (e->trace) {
            show_trace(e);
            printf("\n");
        }
    }
    printf("\n  " DIM "Auf Wiedersehen.\n\n" RST);
}

/* ══════════════════════════════════════════════════════════════════
   Command-Line Mode (for scripting / file processing)
   ══════════════════════════════════════════════════════════════════ */

static void usage(const char *prog) {
    printf("Usage:\n"
           "  %s                            Interactive mode\n"
           "  %s -e \"HELLO WORLD\"           Quick encrypt\n"
           "  %s -f infile.txt outfile.txt   File encrypt/decrypt\n",
           prog, prog, prog);
}

/* ══════════════════════════════════════════════════════════════════
   Main
   ══════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    Enigma eng;
    machine_init(&eng);

    /* Quick encrypt: -e "text" */
    if (argc == 3 && strcmp(argv[1], "-e") == 0) {
        char out[MAX_BUF];
        enc_str(&eng, argv[2], out);
        printf("%s\n", out);
        return 0;
    }

    /* File mode: -f infile outfile */
    if (argc == 4 && strcmp(argv[1], "-f") == 0) {
        FILE *fin = fopen(argv[2], "r");
        if (!fin) {
            fprintf(stderr, RED "Error: cannot open '%s'\n" RST, argv[2]);
            return 1;
        }
        FILE *fout = fopen(argv[3], "w");
        if (!fout) {
            fclose(fin);
            fprintf(stderr, RED "Error: cannot create '%s'\n" RST, argv[3]);
            return 1;
        }
        char in[MAX_BUF], out[MAX_BUF];
        while (fgets(in, sizeof in, fin)) {
            enc_str(&eng, in, out);
            fprintf(fout, "%s\n", out);
        }
        fclose(fin);
        fclose(fout);
        printf(GRN "Done. Output: %s\n" RST, argv[3]);
        return 0;
    }

    if (argc > 1) {
        usage(argv[0]);
        return 1;
    }

    /* Interactive mode */
    banner();
    configure(&eng);
    interactive(&eng);
    return 0;
}
