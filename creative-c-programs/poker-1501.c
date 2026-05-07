/*
 * ================================================================
 *  poker-1501.c — Terminal Texas Hold'em Poker
 * ================================================================
 *  Full Texas Hold'em with 3 AI opponents, proper hand evaluation,
 *  blind rotation, 4 betting rounds, and colorful ncurses UI.
 *
 *  Features:
 *    - Accurate 7-card hand evaluation (Royal Flush → High Card)
 *    - 3 AI styles: TAG (Hawk), LAG (Blaze), Fish (Guppy)
 *    - Full betting: check, call, raise, fold, all-in
 *    - Blind rotation, player elimination, multi-hand play
 *    - Colored card display with hand ranking at showdown
 *
 *  Compile:  gcc -O2 -o poker poker-1501.c -lncurses
 *  Run:      ./poker
 *  Requires: libncurses  (sudo apt install libncurses5-dev)
 * ================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <ncurses.h>

/* ── Constants ────────────────────────────────────────────────── */
#define NP       4
#define START_CHIPS 1000
#define SB_AMT   10
#define BB_AMT   20

enum { PHASE_PREFLOP, PHASE_FLOP, PHASE_TURN, PHASE_RIVER, PHASE_SHOWDOWN };
enum { ACT_FOLD, ACT_CHECK, ACT_CALL, ACT_RAISE, ACT_ALLIN };
enum { HICARD, PAIR, TWOPAIR, THREEK, STRAIGHT, FLUSH,
       FULLHOUSE, FOURK, STRFLUSH, ROYALFLUSH };

/* ── Types ────────────────────────────────────────────────────── */
typedef unsigned char Card;

typedef struct {
    int cat, kick[5];
    long val;
} HandRank;

typedef struct {
    Card hole[2];
    int  chips, bet;
    int  folded, allin, active;
    char name[16];
    int  style;      /* 0=TAG  1=LAG  2=fish */
    int  is_human;
} Player;

typedef struct {
    Card deck[52], comm[5];
    int  comm_n, deck_pos;
    int  pot, cur_bet, phase;
    Player pl[NP];
    int  dealer;
    char msg[5][80];
    int  msg_n;
} Game;

/* ── Card helpers ─────────────────────────────────────────────── */
static inline int c_rank(Card c) { return c % 13; }
static inline int c_suit(Card c) { return c / 13; }
static char r_ch(int r) { return "23456789TJQKA"[r]; }
static char s_ch(int s) { return "HDCS"[s]; }
static int  s_col(int s) { return s < 2 ? 1 : 2; }

static void deck_init(Card d[]) {
    for (int i = 0; i < 52; i++) d[i] = i;
}
static void deck_shuffle(Card d[]) {
    for (int i = 51; i > 0; i--) {
        int j = rand() % (i + 1);
        Card t = d[i]; d[i] = d[j]; d[j] = t;
    }
}
static Card deal_one(Game *g) { return g->deck[g->deck_pos++]; }

/* ── Message log ──────────────────────────────────────────────── */
static void msg(Game *g, const char *fmt, ...) {
    if (g->msg_n >= 5)
        memmove(g->msg[0], g->msg[1], 4 * 80);
    else
        g->msg_n++;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g->msg[g->msg_n - 1], 80, fmt, ap);
    va_end(ap);
}

/* ── Hand evaluation (best 5 from n cards, n≥5) ──────────────── */
static HandRank eval7(Card cards[], int n) {
    int rc[13] = {0}, sc[4] = {0};
    int sf[4][7], sfn[4] = {0};
    for (int i = 0; i < n; i++) {
        int r = c_rank(cards[i]), s = c_suit(cards[i]);
        rc[r]++; sc[s]++;
        sf[s][sfn[s]++] = r;
    }
    /* flush suit */
    int flu = -1;
    for (int s = 0; s < 4; s++)
        if (sc[s] >= 5) { flu = s; break; }
    /* straight */
    int sth = -1;
    for (int h = 12; h >= 4; h--) {
        int ok = 1;
        for (int j = 0; j < 5; j++) if (!rc[h - j]) { ok = 0; break; }
        if (ok) { sth = h; break; }
    }
    if (sth < 0 && rc[12] && rc[0] && rc[1] && rc[2] && rc[3])
        sth = 3;
    /* straight flush */
    int sfh = -1;
    if (flu >= 0) {
        int fc[7]; memcpy(fc, sf[flu], sfn[flu] * sizeof(int));
        for (int i = 0; i < sfn[flu] - 1; i++)
            for (int j = i + 1; j < sfn[flu]; j++)
                if (fc[i] < fc[j]) { int t = fc[i]; fc[i] = fc[j]; fc[j] = t; }
        for (int i = 0; i <= sfn[flu] - 5; i++) {
            int ok = 1;
            for (int j = 0; j < 4; j++)
                if (fc[i+j] - fc[i+j+1] != 1) { ok = 0; break; }
            if (ok) { sfh = fc[i]; break; }
        }
        if (sfh < 0) {
            int h[13] = {0};
            for (int i = 0; i < sfn[flu]; i++) h[fc[i]] = 1;
            if (h[12] && h[0] && h[1] && h[2] && h[3]) sfh = 3;
        }
    }
    /* count pairs/trips/quads */
    int np2 = 0, nt = 0, nq = 0, pr[3], tr[2], qr;
    for (int r = 12; r >= 0; r--) {
        if      (rc[r] == 4) qr = r, nq++;
        else if (rc[r] == 3) tr[nt++] = r;
        else if (rc[r] == 2) pr[np2++] = r;
    }
    HandRank hr; memset(&hr, 0, sizeof(hr));
    if (sfh >= 0) {
        hr.cat = (sfh == 12) ? ROYALFLUSH : STRFLUSH;
        hr.kick[0] = sfh;
    } else if (nq) {
        hr.cat = FOURK; hr.kick[0] = qr;
        for (int r = 12; r >= 0; r--)
            if (r != qr && rc[r]) { hr.kick[1] = r; break; }
    } else if (nt && (np2 || nt >= 2)) {
        hr.cat = FULLHOUSE; hr.kick[0] = tr[0];
        hr.kick[1] = (nt >= 2) ? tr[1] : pr[0];
    } else if (flu >= 0) {
        hr.cat = FLUSH;
        int fc[7]; memcpy(fc, sf[flu], sfn[flu] * sizeof(int));
        for (int i = 0; i < sfn[flu] - 1; i++)
            for (int j = i + 1; j < sfn[flu]; j++)
                if (fc[i] < fc[j]) { int t = fc[i]; fc[i] = fc[j]; fc[j] = t; }
        for (int i = 0; i < 5 && i < sfn[flu]; i++) hr.kick[i] = fc[i];
    } else if (sth >= 0) {
        hr.cat = STRAIGHT; hr.kick[0] = sth;
    } else if (nt) {
        hr.cat = THREEK; hr.kick[0] = tr[0];
        int k = 1;
        for (int r = 12; r >= 0 && k < 3; r--)
            if (r != tr[0] && rc[r]) hr.kick[k++] = r;
    } else if (np2 >= 2) {
        hr.cat = TWOPAIR; hr.kick[0] = pr[0]; hr.kick[1] = pr[1];
        for (int r = 12; r >= 0; r--)
            if (r != pr[0] && r != pr[1] && rc[r]) { hr.kick[2] = r; break; }
    } else if (np2 == 1) {
        hr.cat = PAIR; hr.kick[0] = pr[0];
        int k = 1;
        for (int r = 12; r >= 0 && k < 4; r--)
            if (r != pr[0] && rc[r]) hr.kick[k++] = r;
    } else {
        hr.cat = HICARD;
        int k = 0;
        for (int r = 12; r >= 0 && k < 5; r--)
            if (rc[r]) hr.kick[k++] = r;
    }
    hr.val = hr.cat * 100000000L;
    long m = 10000000;
    for (int i = 0; i < 5; i++) { hr.val += hr.kick[i] * m; m /= 15; }
    return hr;
}

static const char *hname(int cat) {
    static const char *n[] = {
        "High Card","Pair","Two Pair","Three of a Kind","Straight",
        "Flush","Full House","Four of a Kind","Straight Flush","Royal Flush"
    };
    return n[cat];
}

/* ── AI logic ─────────────────────────────────────────────────── */
static int ai_str_preflop(Card h[2]) {
    int r0 = c_rank(h[0]), r1 = c_rank(h[1]);
    int suited = (c_suit(h[0]) == c_suit(h[1]));
    int hi = r0 > r1 ? r0 : r1, lo = r0 > r1 ? r1 : r0;
    int s = hi + lo;
    if (r0 == r1) s += 15;
    if (suited) s += 4;
    if (hi - lo <= 2 && hi != lo) s += 3;
    return s;
}

static int ai_decide(Game *g, int pos) {
    Player *p = &g->pl[pos];
    int str;
    if (g->comm_n == 0) {
        str = ai_str_preflop(p->hole);
    } else {
        Card all[7];
        memcpy(all, p->hole, 2);
        memcpy(all + 2, g->comm, g->comm_n);
        HandRank hr = eval7(all, 2 + g->comm_n);
        str = hr.cat * 6 + 12;
    }
    str += (rand() % 10) - 5;

    int pt, rt;
    switch (p->style) {
        case 0: pt = 15; rt = 26; break;   /* TAG: tight */
        case 1: pt = 10; rt = 20; break;   /* LAG: loose-aggressive */
        case 2: pt = 7;  rt = 33; break;   /* Fish: calls a lot */
        default: pt = 12; rt = 24;
    }
    int to_call = g->cur_bet - p->bet;
    /* occasional bluff */
    if (str < pt && to_call == 0 && (rand() % 12) == 0) return ACT_RAISE;

    if (to_call == 0)
        return str >= rt ? ACT_RAISE : ACT_CHECK;
    if (str < pt) {
        /* fish sometimes calls anyway */
        if (p->style == 2 && (rand() % 3) == 0) return ACT_CALL;
        return ACT_FOLD;
    }
    if (str >= rt && p->chips > to_call * 3) return ACT_RAISE;
    return ACT_CALL;
}

/* ── Drawing ──────────────────────────────────────────────────── */
static void draw_card(int y, int x, Card c, int up) {
    if (up) {
        int pair = s_col(c_suit(c));
        attron(A_BOLD | COLOR_PAIR(pair));
        mvprintw(y, x, "+---+");
        mvprintw(y+1, x, "|%c %c|", r_ch(c_rank(c)), s_ch(c_suit(c)));
        mvprintw(y+2, x, "+---+");
        attroff(A_BOLD | COLOR_PAIR(pair));
    } else {
        attron(COLOR_PAIR(4));
        mvprintw(y, x, "+---+");
        mvprintw(y+1, x, "|///|");
        mvprintw(y+2, x, "+---+");
        attroff(COLOR_PAIR(4));
    }
}

static void draw_pair(int y, int x, Card h[2], int up) {
    draw_card(y, x, h[0], up);
    draw_card(y, x + 6, h[1], up);
}

static void draw_table(Game *g) {
    clear();
    int W = COLS, cy;

    /* title */
    attron(A_BOLD | COLOR_PAIR(3));
    mvprintw(0, (W - 28) / 2, "=== Texas Hold'em Poker ===");
    attroff(A_BOLD | COLOR_PAIR(3));

    const char *phases[] = {"Pre-Flop","Flop","Turn","River","Showdown"};
    attron(COLOR_PAIR(5));
    mvprintw(1, (W - 20) / 2, "-- %s --", phases[g->phase]);
    attroff(COLOR_PAIR(5));

    /* AI players: 3 across the top */
    int ax[3] = { 4, W / 2 - 14, W - 28 };
    int ai = 0;
    for (int i = 0; i < NP; i++) {
        if (g->pl[i].is_human) continue;
        int x = ax[ai], y = 3;
        Player *p = &g->pl[i];

        attron(A_BOLD);
        mvprintw(y, x, "%s", p->name);
        attroff(A_BOLD);
        if (i == g->dealer) {
            attron(COLOR_PAIR(4)); mvprintw(y, x + 8, "(D)"); attroff(COLOR_PAIR(4));
        }
        if (!p->active) {
            attron(COLOR_PAIR(4)); mvprintw(y+1, x, "OUT"); attroff(COLOR_PAIR(4));
        } else if (p->folded) {
            attron(COLOR_PAIR(4)); mvprintw(y+1, x, "Folded"); attroff(COLOR_PAIR(4));
            draw_pair(y + 2, x, p->hole, 0);
        } else if (p->allin) {
            attron(COLOR_PAIR(1)); mvprintw(y+1, x, "ALL-IN"); attroff(COLOR_PAIR(1));
            draw_pair(y + 2, x, p->hole,
                      g->phase == PHASE_SHOWDOWN);
            attron(COLOR_PAIR(5));
            mvprintw(y + 2, x + 14, "$%d", p->chips);
            attroff(COLOR_PAIR(5));
        } else {
            attron(COLOR_PAIR(5));
            mvprintw(y+1, x, "$%d", p->chips);
            attroff(COLOR_PAIR(5));
            draw_pair(y + 2, x, p->hole,
                      g->phase == PHASE_SHOWDOWN);
            if (p->bet > 0) {
                attron(COLOR_PAIR(3));
                mvprintw(y + 2, x + 14, "Bet:%d", p->bet);
                attroff(COLOR_PAIR(3));
            }
        }
        ai++;
    }

    /* community cards */
    cy = 10;
    int cx = (W - 36) / 2;
    attron(A_BOLD);
    mvprintw(cy, cx, "Board:");
    attroff(A_BOLD);
    for (int i = 0; i < g->comm_n; i++)
        draw_card(cy + 1, cx + 8 + i * 6, g->comm[i], 1);

    /* pot */
    attron(A_BOLD | COLOR_PAIR(3));
    mvprintw(cy + 1, cx + 42, "Pot: $%d", g->pot);
    attroff(A_BOLD | COLOR_PAIR(3));

    /* human player */
    cy = 16;
    Player *me = &g->pl[0];
    attron(A_BOLD);
    mvprintw(cy, 4, "Your Hand");
    attroff(A_BOLD);
    if (g->dealer == 0) {
        attron(COLOR_PAIR(4)); mvprintw(cy, 14, "(D)"); attroff(COLOR_PAIR(4));
    }
    if (me->active && !me->folded) {
        draw_pair(cy + 1, 4, me->hole, 1);
        attron(COLOR_PAIR(5));
        mvprintw(cy + 1, 20, "Chips: $%d", me->chips);
        if (me->bet > 0) mvprintw(cy + 2, 20, "Bet:   $%d", me->bet);
        attroff(COLOR_PAIR(5));
        if (me->allin) {
            attron(A_BOLD | COLOR_PAIR(1));
            mvprintw(cy + 3, 4, "ALL-IN!");
            attroff(A_BOLD | COLOR_PAIR(1));
        }
    } else if (me->folded) {
        draw_pair(cy + 1, 4, me->hole, 0);
        attron(COLOR_PAIR(4)); mvprintw(cy + 2, 4, "Folded"); attroff(COLOR_PAIR(4));
    } else {
        attron(COLOR_PAIR(4)); mvprintw(cy + 1, 4, "Eliminated"); attroff(COLOR_PAIR(4));
    }

    /* message log */
    for (int i = 0; i < g->msg_n; i++) {
        attron(COLOR_PAIR(5));
        mvprintw(21 + i, 4, "%s", g->msg[i]);
        attroff(COLOR_PAIR(5));
    }

    refresh();
}

static void draw_actions(Game *g) {
    Player *me = &g->pl[0];
    int to_call = g->cur_bet - me->bet;
    int y = 20;
    attron(A_BOLD | COLOR_PAIR(4));
    if (to_call <= 0)
        mvprintw(y, 4, "(C)heck  (R)aise  (F)old  (A)ll-in");
    else
        mvprintw(y, 4, "(C)all $%d  (R)aise  (F)old  (A)ll-in  ", to_call);
    attroff(A_BOLD | COLOR_PAIR(4));
    refresh();
}

static void draw_title(void) {
    clear();
    int W = COLS, H = LINES;
    attron(A_BOLD | COLOR_PAIR(3));
    mvprintw(H/2 - 4, (W-36)/2, "====================================");
    mvprintw(H/2 - 3, (W-36)/2, "     TEXAS HOLD'EM POKER           ");
    mvprintw(H/2 - 2, (W-36)/2, "         Terminal Edition           ");
    mvprintw(H/2 - 1, (W-36)/2, "                                    ");
    mvprintw(H/2,     (W-36)/2, "  3 AI opponents with unique styles ");
    mvprintw(H/2 + 1, (W-36)/2, "    Full rules  /  Accurate eval    ");
    mvprintw(H/2 + 2, (W-36)/2, "====================================");
    attroff(A_BOLD | COLOR_PAIR(3));
    attron(COLOR_PAIR(5));
    mvprintw(H/2 + 4, (W-26)/2, "Press any key to start...");
    attroff(COLOR_PAIR(5));
    refresh();
    getch();
}

/* ── Game logic ───────────────────────────────────────────────── */
static void init_game(Game *g) {
    memset(g, 0, sizeof(Game));
    srand(time(NULL));

    g->pl[0] = (Player){ .chips=START_CHIPS, .active=1,
                          .is_human=1, .style=0 };
    strcpy(g->pl[0].name, "You");

    g->pl[1] = (Player){ .chips=START_CHIPS, .active=1, .style=0 };
    strcpy(g->pl[1].name, "Hawk");

    g->pl[2] = (Player){ .chips=START_CHIPS, .active=1, .style=1 };
    strcpy(g->pl[2].name, "Blaze");

    g->pl[3] = (Player){ .chips=START_CHIPS, .active=1, .style=2 };
    strcpy(g->pl[3].name, "Guppy");

    g->dealer = 0;
}

/* Find next active (non-eliminated) player clockwise from pos */
static int next_active(Game *g, int pos) {
    for (int i = 1; i <= NP; i++) {
        int p = (pos + i) % NP;
        if (g->pl[p].active) return p;
    }
    return -1;
}

static void new_hand(Game *g) {
    deck_init(g->deck);
    deck_shuffle(g->deck);
    g->deck_pos = 0;
    g->comm_n = 0;
    g->pot = 0;
    g->cur_bet = 0;
    g->phase = PHASE_PREFLOP;
    g->msg_n = 0;

    for (int i = 0; i < NP; i++) {
        g->pl[i].folded = 0;
        g->pl[i].allin = 0;
        g->pl[i].bet = 0;
        if (!g->pl[i].active) g->pl[i].folded = 1;
    }

    /* deal hole cards */
    for (int r = 0; r < 2; r++)
        for (int i = 0; i < NP; i++)
            if (g->pl[i].active)
                g->pl[i].hole[r] = deal_one(g);

    msg(g, "--- New Hand ---");
}

static void post_blinds(Game *g) {
    /* SB: first active after dealer */
    int sb = next_active(g, g->dealer);
    /* BB: first active after SB */
    int bb = next_active(g, sb);

    int sb_amt = g->pl[sb].chips < SB_AMT ? g->pl[sb].chips : SB_AMT;
    g->pl[sb].chips -= sb_amt; g->pl[sb].bet = sb_amt; g->pot += sb_amt;
    if (g->pl[sb].chips == 0) g->pl[sb].allin = 1;

    int bb_amt = g->pl[bb].chips < BB_AMT ? g->pl[bb].chips : BB_AMT;
    g->pl[bb].chips -= bb_amt; g->pl[bb].bet = bb_amt; g->pot += bb_amt;
    if (g->pl[bb].chips == 0) g->pl[bb].allin = 1;

    g->cur_bet = bb_amt;
    msg(g, "%s posts SB $%d, %s posts BB $%d",
        g->pl[sb].name, sb_amt, g->pl[bb].name, bb_amt);
}

/* Execute a single player action */
static void do_action(Game *g, int pos, int act, int acted[]) {
    Player *p = &g->pl[pos];
    switch (act) {
    case ACT_FOLD:
        p->folded = 1;
        acted[pos] = 1;
        msg(g, "%s folds", p->name);
        break;
    case ACT_CHECK:
        acted[pos] = 1;
        msg(g, "%s checks", p->name);
        break;
    case ACT_CALL: {
        int need = g->cur_bet - p->bet;
        if (need > p->chips) need = p->chips;
        p->chips -= need; p->bet += need; g->pot += need;
        if (p->chips == 0) p->allin = 1;
        acted[pos] = 1;
        msg(g, "%s calls $%d%s", p->name, need,
            p->allin ? " (all-in)" : "");
        break;
    }
    case ACT_RAISE: {
        int raise_to = g->cur_bet + BB_AMT;
        int need = raise_to - p->bet;
        if (need > p->chips) need = p->chips;
        p->chips -= need; p->bet += need; g->pot += need;
        g->cur_bet = p->bet;
        if (p->chips == 0) p->allin = 1;
        for (int i = 0; i < NP; i++) if (i != pos) acted[i] = 0;
        acted[pos] = 1;
        msg(g, "%s raises to $%d%s", p->name, g->cur_bet,
            p->allin ? " (all-in)" : "");
        break;
    }
    case ACT_ALLIN: {
        int amt = p->chips;
        p->chips = 0; p->bet += amt; g->pot += amt;
        if (p->bet > g->cur_bet) {
            g->cur_bet = p->bet;
            for (int i = 0; i < NP; i++) if (i != pos) acted[i] = 0;
        }
        p->allin = 1;
        acted[pos] = 1;
        msg(g, "%s goes ALL-IN ($%d)!", p->name, amt);
        break;
    }
    }
}

/* Count players still in the hand (active + not folded) */
static int count_live(Game *g) {
    int n = 0;
    for (int i = 0; i < NP; i++)
        if (g->pl[i].active && !g->pl[i].folded) n++;
    return n;
}

/* Count players who can still act (not folded, not all-in) */
static int count_can_act(Game *g) {
    int n = 0;
    for (int i = 0; i < NP; i++)
        if (g->pl[i].active && !g->pl[i].folded && !g->pl[i].allin
            && g->pl[i].chips > 0)
            n++;
    return n;
}

/* Run one betting round. Returns number of live players. */
static int betting_round(Game *g) {
    /* if 0-1 can act, skip */
    if (count_can_act(g) <= 1 && count_live(g) <= 1) return count_live(g);

    /* find starting position */
    int start;
    int active_ct = 0;
    for (int i = 0; i < NP; i++) if (g->pl[i].active) active_ct++;

    if (g->phase == PHASE_PREFLOP) {
        if (active_ct == 2)
            start = g->dealer;  /* heads-up: SB/dealer acts first */
        else {
            /* UTG = 3rd active seat after dealer */
            int s = g->dealer;
            for (int i = 0; i < 3; i++) s = next_active(g, s);
            start = s;
        }
    } else {
        /* post-flop: first active after dealer */
        start = next_active(g, g->dealer);
    }

    int acted[NP] = {0};
    int pos = start;
    int safety = 200;  /* prevent infinite loop */

    while (safety-- > 0) {
        /* skip inactive / folded / all-in */
        if (!g->pl[pos].active || g->pl[pos].folded || g->pl[pos].allin) {
            pos = (pos + 1) % NP;
            if (count_live(g) <= 1) break;

            /* check if round is done */
            int all_ok = 1;
            for (int i = 0; i < NP; i++) {
                if (g->pl[i].active && !g->pl[i].folded
                    && !g->pl[i].allin && g->pl[i].chips > 0) {
                    if (!acted[i] || g->pl[i].bet != g->cur_bet)
                        all_ok = 0;
                }
            }
            if (all_ok && count_can_act(g) <= count_live(g)) break;
            continue;
        }

        /* check termination */
        if (count_live(g) <= 1) break;
        int all_ok = 1;
        for (int i = 0; i < NP; i++) {
            if (g->pl[i].active && !g->pl[i].folded
                && !g->pl[i].allin && g->pl[i].chips > 0) {
                if (!acted[i]) all_ok = 0;
                if (g->pl[i].bet != g->cur_bet) all_ok = 0;
            }
        }
        if (all_ok) break;

        /* get action */
        int act;
        if (g->pl[pos].is_human) {
            draw_table(g);
            draw_actions(g);
            while (1) {
                int ch = getch();
                int to_call = g->cur_bet - g->pl[pos].bet;
                if (ch == 'f' || ch == 'F') { act = ACT_FOLD; break; }
                if (ch == 'c' || ch == 'C') {
                    act = (to_call <= 0) ? ACT_CHECK : ACT_CALL;
                    break;
                }
                if (ch == 'r' || ch == 'R') { act = ACT_RAISE; break; }
                if (ch == 'a' || ch == 'A') { act = ACT_ALLIN; break; }
            }
        } else {
            act = ai_decide(g, pos);
        }

        do_action(g, pos, act, acted);

        if (!g->pl[pos].is_human) {
            draw_table(g);
            napms(400);
        }

        pos = (pos + 1) % NP;
    }

    /* reset bets for next round */
    for (int i = 0; i < NP; i++) g->pl[i].bet = 0;
    g->cur_bet = 0;

    return count_live(g);
}

static void showdown(Game *g) {
    g->phase = PHASE_SHOWDOWN;
    msg(g, "=== SHOWDOWN ===");

    /* evaluate each live player's hand */
    HandRank best_hr; memset(&best_hr, 0, sizeof(best_hr));
    int winner = -1;
    for (int i = 0; i < NP; i++) {
        if (!g->pl[i].active || g->pl[i].folded) continue;
        Card all[7];
        memcpy(all, g->pl[i].hole, 2);
        memcpy(all + 2, g->comm, g->comm_n);
        HandRank hr = eval7(all, 2 + g->comm_n);
        msg(g, "%s: %s", g->pl[i].name, hname(hr.cat));
        if (hr.val > best_hr.val) { best_hr = hr; winner = i; }
    }

    draw_table(g);
    napms(600);

    if (winner >= 0) {
        g->pl[winner].chips += g->pot;
        attron(A_BOLD | COLOR_PAIR(3));
        mvprintw(LINES / 2, (COLS - 30) / 2,
                 "%s wins $%d with %s!",
                 g->pl[winner].name, g->pot, hname(best_hr.cat));
        attroff(A_BOLD | COLOR_PAIR(3));
        refresh();
        napms(2000);
    }

    g->pot = 0;
}

static void award_pot_by_fold(Game *g) {
    /* find the one remaining player */
    for (int i = 0; i < NP; i++) {
        if (g->pl[i].active && !g->pl[i].folded) {
            g->pl[i].chips += g->pot;
            msg(g, "%s wins $%d (others folded)",
                g->pl[i].name, g->pot);
            g->pot = 0;

            draw_table(g);
            attron(A_BOLD | COLOR_PAIR(3));
            mvprintw(LINES / 2, (COLS - 28) / 2,
                     "%s wins $%d!", g->pl[i].name, g->pot);
            attroff(A_BOLD | COLOR_PAIR(3));
            refresh();
            napms(1500);
            return;
        }
    }
}

static void eliminate_broke(Game *g) {
    for (int i = 0; i < NP; i++) {
        if (g->pl[i].active && g->pl[i].chips <= 0) {
            g->pl[i].active = 0;
            msg(g, "%s is eliminated!", g->pl[i].name);
        }
    }
}

static int count_active(Game *g) {
    int n = 0;
    for (int i = 0; i < NP; i++) if (g->pl[i].active) n++;
    return n;
}

/* ── Main ─────────────────────────────────────────────────────── */
int main(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    start_color();
    init_pair(1, COLOR_RED,     COLOR_BLACK);
    init_pair(2, COLOR_CYAN,    COLOR_BLACK);
    init_pair(3, COLOR_GREEN,   COLOR_BLACK);
    init_pair(4, COLOR_YELLOW,  COLOR_BLACK);
    init_pair(5, COLOR_WHITE,   COLOR_BLACK);

    draw_title();

    Game g;
    init_game(&g);

    while (count_active(&g) > 1 && g.pl[0].active) {
        new_hand(&g);
        draw_table(&g); napms(400);

        post_blinds(&g);
        draw_table(&g); napms(400);

        /* pre-flop */
        g.phase = PHASE_PREFLOP;
        if (betting_round(&g) <= 1) { award_pot_by_fold(&g); goto hand_done; }

        /* flop */
        deal_one(&g);
        for (int i = 0; i < 3; i++) g.comm[g.comm_n++] = deal_one(&g);
        g.phase = PHASE_FLOP;
        msg(&g, "--- Flop ---");
        draw_table(&g); napms(500);
        if (betting_round(&g) <= 1) { award_pot_by_fold(&g); goto hand_done; }

        /* turn */
        deal_one(&g);
        g.comm[g.comm_n++] = deal_one(&g);
        g.phase = PHASE_TURN;
        msg(&g, "--- Turn ---");
        draw_table(&g); napms(500);
        if (betting_round(&g) <= 1) { award_pot_by_fold(&g); goto hand_done; }

        /* river */
        deal_one(&g);
        g.comm[g.comm_n++] = deal_one(&g);
        g.phase = PHASE_RIVER;
        msg(&g, "--- River ---");
        draw_table(&g); napms(500);
        if (betting_round(&g) <= 1) { award_pot_by_fold(&g); goto hand_done; }

        /* showdown */
        showdown(&g);

hand_done:
        eliminate_broke(&g);

        /* advance dealer */
        int nd = next_active(&g, g.dealer);
        if (nd >= 0) g.dealer = nd;

        draw_table(&g);

        attron(COLOR_PAIR(5));
        mvprintw(LINES - 1, 4, "Press SPACE for next hand, Q to quit");
        attroff(COLOR_PAIR(5));
        refresh();

        int ch;
        while ((ch = getch())) {
            if (ch == ' ') break;
            if (ch == 'q' || ch == 'Q') goto done;
        }
    }

    /* game over */
    clear();
    if (!g.pl[0].active) {
        attron(A_BOLD | COLOR_PAIR(1));
        mvprintw(LINES/2, (COLS-18)/2, "GAME OVER - You lost!");
        attroff(A_BOLD | COLOR_PAIR(1));
    } else {
        attron(A_BOLD | COLOR_PAIR(3));
        mvprintw(LINES/2, (COLS-20)/2, "YOU WIN THE GAME!");
        attroff(A_BOLD | COLOR_PAIR(3));
    }
    mvprintw(LINES/2 + 2, (COLS-26)/2, "Press any key to exit...");
    refresh();
    getch();

done:
    endwin();
    return 0;
}
