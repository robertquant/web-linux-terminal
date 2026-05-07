/*
 * ===================================================================
 * towerdef-7537.c — Terminal Tower Defense Game
 * ===================================================================
 *
 *  A full-featured tower defense game rendered entirely in your
 *  terminal with ANSI colours and smooth ASCII animation.
 *
 *  Build:   gcc -o towerdef towerdef-7537.c -lm -O2
 *  Run:     ./towerdef
 *
 *  Controls:
 *    h/j/k/l  or  Arrow keys   Move cursor
 *    1                       Place Cannon   ( $50 – splash damage )
 *    2                       Place Laser    ( $80 – rapid fire    )
 *    3                       Place Frost    ( $40 – slows 50 %    )
 *    4                       Place Sniper   ($100 – high dmg/range)
 *    u                       Upgrade tower under cursor
 *    x                       Sell tower under cursor (50 % refund)
 *    r                       Toggle range circles
 *    Space                   Start next wave
 *    q                       Quit
 *
 *  Requires: POSIX terminal (Linux / macOS), 80+ column terminal.
 * ===================================================================
 */

#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <signal.h>

/* ── grid & limits ─────────────────────────────────────────────── */
#define MW 60          /* map width  */
#define MH 20          /* map height */
#define MAX_WP   32    /* waypoints  */
#define MAX_DENSE 1200 /* dense path points */
#define MAX_E   150    /* enemies    */
#define MAX_T    80    /* towers     */
#define MAX_P   300    /* projectiles */
#define MAX_FX  200    /* effects    */
#define MAX_BEAM 40    /* laser beams */
#define TICK_US 50000  /* 50 ms = 20 FPS */

/* ── ANSI helpers ──────────────────────────────────────────────── */
#define E "\033"
#define RST  E"[0m"
#define BLD  E"[1m"
#define DIM  E"[2m"
#define REV  E"[7m"
#define RED  E"[31m"
#define GRN  E"[32m"
#define YEL  E"[33m"
#define BLU  E"[34m"
#define MAG  E"[35m"
#define CYN  E"[36m"
#define WHT  E"[37m"
#define BRED E"[91m"
#define BGRN E"[92m"
#define BYEL E"[93m"
#define BCYN E"[96m"

/* ── tower definitions ─────────────────────────────────────────── */
typedef struct {
    const char *name, *desc;
    char sym;
    int cost, dmg, rng, cool;
    int splash, slow;
} TDef;

static const TDef TD[] = {
    /*  name      desc          sym cost  dmg  rng cool spl slow */
    {"Cannon","Splash dmg",   'C', 50,  20,  5,  25,  2,  0},
    {"Laser", "Rapid fire",   'L', 80,   3,  7,   2,  0,  0},
    {"Frost", "Slows 50%",    'F', 40,   8,  4,  35,  0, 80},
    {"Sniper","High dmg/rng", 'S',100,  90, 10,  80,  0,  0},
};
#define NTD (int)(sizeof TD / sizeof *TD)

/* ── structures ────────────────────────────────────────────────── */
typedef struct { double x,y; } V2;

typedef struct {
    double x,y;
    int hp, mhp;
    double spd, bspd;
    int type, pi;      /* type 0-3, path index */
    int slow_t, alive, reward;
} Enemy;

typedef struct {
    int x,y,type,lvl,cd,tcost,kills;
} Tower;

typedef struct {
    double x,y,dx,dy,speed;
    int dmg,splash,slow,life,alive;
} Proj;

typedef struct { double x1,y1,x2,y2; int life; } Beam;
typedef struct { double x,y; int life,ml; char k; } FX;

/* ── game state ────────────────────────────────────────────────── */
static struct {
    char map[MH][MW+1];
    V2 wp[MAX_WP]; int nwp;
    V2 dp[MAX_DENSE]; int ndp;

    Enemy en[MAX_E]; int ne;
    Tower tw[MAX_T]; int nt;
    Proj  pj[MAX_P]; int np;
    Beam  bm[MAX_BEAM]; int nb;
    FX    fx[MAX_FX]; int nfx;

    int gold, lives, wave, score;
    int wave_on, sq, stmr;
    int cx, cy, show_rng, over, tick, running;
    struct termios ot;
} G;

/* ── terminal ──────────────────────────────────────────────────── */
static void tinit(void) {
    struct termios t;
    tcgetattr(0, &G.ot);
    t = G.ot;
    t.c_lflag &= ~(ICANON|ECHO|ISIG);
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    setvbuf(stdout, NULL, _IONBF, 0);
    printf(E"[?25l"E"[2J");
}

static void trestore(void) {
    tcsetattr(0, TCSANOW, &G.ot);
    printf(E"[?25h"RST E"[2J"E"[H");
}

static int kbhit(void) {
    struct timeval tv = {0,0};
    fd_set f; FD_ZERO(&f); FD_SET(0,&f);
    return select(1,&f,NULL,NULL,&tv)>0;
}

static int tgetc(void) {
    unsigned char c=0;
    return read(0,&c,1)==1 ? c : -1;
}

/* ── build serpentine path ─────────────────────────────────────── */
static void build_path(void) {
    /* waypoints: serpentine across the map */
    static const int seg[][4] = {
        { 0, 2, 53, 2}, {53, 2, 53, 6}, {53, 6,  5, 6},
        { 5, 6,  5,10}, { 5,10, 53,10}, {53,10, 53,14},
        {53,14,  5,14}, { 5,14,  5,18}, { 5,18, 53,18},
        {53,18, 57,18}
    };
    G.nwp = 0;
    for (int i = 0; i < (int)(sizeof seg/sizeof*seg); i++) {
        if (i==0 || seg[i][0]!=seg[i-1][2] || seg[i][1]!=seg[i-1][3])
            G.wp[G.nwp++] = (V2){seg[i][0], seg[i][1]};
        G.wp[G.nwp++] = (V2){seg[i][2], seg[i][3]};
    }

    /* dense path: 2 points per cell for smooth movement */
    G.ndp = 0;
    for (int i = 0; i < G.nwp-1 && G.ndp < MAX_DENSE-2; i++) {
        double x1=G.wp[i].x, y1=G.wp[i].y;
        double x2=G.wp[i+1].x, y2=G.wp[i+1].y;
        double dx=x2-x1, dy=y2-y1;
        double len=sqrt(dx*dx+dy*dy);
        int steps=(int)(len*2)+1;
        for (int s=0; s<steps && G.ndp<MAX_DENSE-1; s++) {
            double t=(double)s/steps;
            G.dp[G.ndp++] = (V2){x1+dx*t, y1+dy*t};
        }
    }
    if (G.ndp < MAX_DENSE)
        G.dp[G.ndp++] = G.wp[G.nwp-1];
}

/* ── build map grid ────────────────────────────────────────────── */
static void build_map(void) {
    for (int y=0;y<MH;y++)
        for (int x=0;x<MW;x++)
            G.map[y][x] = '.';

    /* mark path cells */
    for (int i=0;i<G.nwp-1;i++) {
        int x1=(int)G.wp[i].x, y1=(int)G.wp[i].y;
        int x2=(int)G.wp[i+1].x, y2=(int)G.wp[i+1].y;
        if (y1==y2) {
            int lo=x1<x2?x1:x2, hi=x1<x2?x2:x1;
            for (int x=lo;x<=hi&&x<MW;x++)
                if(y1>=0&&y1<MH) G.map[y1][x]='#';
        } else {
            int lo=y1<y2?y1:y2, hi=y1<y2?y2:y1;
            for (int y=lo;y<=hi&&y<MH;y++)
                if(x1>=0&&x1<MW) G.map[y][x1]='#';
        }
    }
    /* entrance & exit markers */
    G.map[(int)G.wp[0].y][(int)G.wp[0].x] = '>';
    int lx=(int)G.wp[G.nwp-1].x, ly=(int)G.wp[G.nwp-1].y;
    if(lx<MW&&ly<MH) G.map[ly][lx] = '!';

    for (int y=0;y<MH;y++) G.map[y][MW] = '\0';
}

/* ── helpers ───────────────────────────────────────────────────── */
static double dist(double x1,double y1,double x2,double y2) {
    double a=x2-x1, b=y2-y1;
    return sqrt(a*a+b*b);
}

static Tower *tw_at(int x,int y) {
    for (int i=0;i<G.nt;i++)
        if (G.tw[i].x==x && G.tw[i].y==y) return &G.tw[i];
    return NULL;
}

/* ── tower ops ─────────────────────────────────────────────────── */
static int place(int x,int y,int type) {
    if(x<0||x>=MW||y<0||y>=MH) return 0;
    if(G.map[y][x]!='.') return 0;
    if(tw_at(x,y)||G.nt>=MAX_T) return 0;
    if(G.gold<TD[type].cost) return 0;
    Tower *t=&G.tw[G.nt++];
    t->x=x; t->y=y; t->type=type;
    t->lvl=1; t->cd=0; t->tcost=TD[type].cost; t->kills=0;
    G.gold -= TD[type].cost;
    return 1;
}

static int upgrade(Tower *t) {
    if(t->lvl>=3) return 0;
    int cost = TD[t->type].cost * t->lvl;
    if(G.gold<cost) return 0;
    G.gold -= cost; t->lvl++; t->tcost += cost;
    return 1;
}

static void sell(Tower *t) {
    G.gold += t->tcost/2;
    int i = t - G.tw;
    G.tw[i] = G.tw[--G.nt];
}

/* ── enemy spawning ────────────────────────────────────────────── */
static void spawn(int type) {
    if(G.ne>=MAX_E) return;
    static const struct { int hp; double sp; int rw; } ED[]={
        {40,0.6,10},{20,1.2,15},{100,0.3,25},{300,0.5,100}
    };
    double sc = 1.0 + G.wave * 0.35;
    Enemy *e = &G.en[G.ne++];
    e->x=G.dp[0].x; e->y=G.dp[0].y;
    e->hp=(int)(ED[type].hp*sc); e->mhp=e->hp;
    e->bspd=ED[type].sp; e->spd=e->bspd;
    e->type=type; e->pi=0;
    e->slow_t=0; e->alive=1;
    e->reward=ED[type].rw + G.wave*2;
}

static void start_wave(void) {
    if(G.wave_on) return;
    G.wave++; G.wave_on=1;
    G.sq = 5 + G.wave*2; if(G.sq>40) G.sq=40;
    G.stmr=0;
}

static void upd_spawn(void) {
    if(!G.wave_on||G.sq<=0) return;
    if(--G.stmr>0) return;
    G.stmr = 8;
    int type=0;
    if(G.wave>=4 && rand()%3==0) type=1;
    if(G.wave>=7 && rand()%5==0) type=2;
    if(G.wave%5==0 && G.sq<=2) type=3;
    spawn(type);
    G.sq--;
}

/* ── enemy update ──────────────────────────────────────────────── */
static void upd_enemies(void) {
    int any=0;
    for(int i=0;i<G.ne;i++) {
        Enemy *e=&G.en[i];
        if(!e->alive) continue;
        any++;
        e->spd = e->slow_t>0 ? e->bspd*0.5 : e->bspd;
        if(e->slow_t>0) e->slow_t--;

        /* advance along dense path */
        e->pi += (int)(e->spd + 0.5);
        if(e->pi >= G.ndp) {
            e->alive=0; G.lives--;
            if(G.lives<=0){G.lives=0;G.over=1;}
            continue;
        }
        e->x = G.dp[e->pi].x;
        e->y = G.dp[e->pi].y;
    }
    /* wave complete? */
    if(G.wave_on && G.sq<=0) {
        int alive=0;
        for(int i=0;i<G.ne;i++) if(G.en[i].alive) alive++;
        if(!alive) {
            G.wave_on=0;
            G.ne=0;
            G.gold += 20 + G.wave*5;
        }
    }
}

/* ── tower update ──────────────────────────────────────────────── */
static void upd_towers(void) {
    for(int i=0;i<G.nt;i++) {
        Tower *t=&G.tw[i];
        if(t->cd>0){t->cd--;continue;}
        const TDef *d=&TD[t->type];
        double rng = d->rng*(1.0+(t->lvl-1)*0.15);
        int dmg = (int)(d->dmg*(1.0+(t->lvl-1)*0.5));
        int cool = (int)(d->cool*(1.0-(t->lvl-1)*0.12));
        if(cool<1) cool=1;

        /* target: furthest-along-path enemy in range */
        Enemy *tgt=NULL; int best=-1;
        for(int j=0;j<G.ne;j++){
            Enemy *e=&G.en[j];
            if(!e->alive) continue;
            if(dist(t->x,t->y,e->x,e->y)<=rng && e->pi>best){
                best=e->pi; tgt=e;
            }
        }
        if(!tgt) continue;
        t->cd = cool;

        if(d->splash>0 || d->slow>0) {
            /* spawn projectile */
            if(G.np<MAX_P){
                Proj *p=&G.pj[G.np++];
                p->x=t->x; p->y=t->y;
                double dd=dist(t->x,t->y,tgt->x,tgt->y);
                if(dd>0.01){p->dx=(tgt->x-t->x)/dd;p->dy=(tgt->y-t->y)/dd;}
                else {p->dx=0;p->dy=0;}
                p->speed=1.8; p->dmg=dmg;
                p->splash=d->splash; p->slow=d->slow;
                p->life=30; p->alive=1;
            }
        } else {
            /* instant hit */
            tgt->hp -= dmg;
            if(G.nb<MAX_BEAM)
                G.bm[G.nb++]=(Beam){t->x,t->y,tgt->x,tgt->y,3};
            if(G.nfx<MAX_FX)
                G.fx[G.nfx++]=(FX){tgt->x,tgt->y,4,4,'H'};
            if(tgt->hp<=0){
                tgt->alive=0; G.gold+=tgt->reward; G.score+=tgt->reward;
                t->kills++;
                if(G.nfx<MAX_FX)
                    G.fx[G.nfx++]=(FX){tgt->x,tgt->y,8,8,'E'};
            }
        }
    }
}

/* ── projectile update ─────────────────────────────────────────── */
static void upd_proj(void) {
    for(int i=G.np-1;i>=0;i--){
        Proj *p=&G.pj[i];
        if(!p->alive) continue;
        p->x+=p->dx*p->speed; p->y+=p->dy*p->speed;
        if(--p->life<=0){p->alive=0;continue;}

        for(int j=0;j<G.ne;j++){
            Enemy *e=&G.en[j];
            if(!e->alive) continue;
            if(dist(p->x,p->y,e->x,e->y)<0.9){
                p->alive=0;
                if(p->splash>0){
                    for(int k=0;k<G.ne;k++){
                        Enemy *e2=&G.en[k];
                        if(!e2->alive) continue;
                        double sd=dist(p->x,p->y,e2->x,e2->y);
                        if(sd<=p->splash){
                            double f=1.0-(sd/p->splash)*0.5;
                            e2->hp-=(int)(p->dmg*f);
                            if(p->slow) e2->slow_t=p->slow;
                            if(e2->hp<=0){
                                e2->alive=0;G.gold+=e2->reward;G.score+=e2->reward;
                            }
                        }
                    }
                    if(G.nfx<MAX_FX)
                        G.fx[G.nfx++]=(FX){p->x,p->y,6,6,'E'};
                } else {
                    e->hp-=p->dmg;
                    if(p->slow) e->slow_t=p->slow;
                    if(e->hp<=0){
                        e->alive=0;G.gold+=e->reward;G.score+=e->reward;
                    }
                    if(G.nfx<MAX_FX)
                        G.fx[G.nfx++]=(FX){e->x,e->y,3,3,'H'};
                }
                break;
            }
        }
    }
    /* compact */
    int w=0;
    for(int i=0;i<G.np;i++) if(G.pj[i].alive) G.pj[w++]=G.pj[i];
    G.np=w;
}

/* ── effects / beams update ────────────────────────────────────── */
static void upd_fx(void) {
    int w=0;
    for(int i=0;i<G.nfx;i++) if(--G.fx[i].life>0) G.fx[w++]=G.fx[i];
    G.nfx=w;
    w=0;
    for(int i=0;i<G.nb;i++) if(--G.bm[i].life>0) G.bm[w++]=G.bm[i];
    G.nb=w;
}

/* ── input ─────────────────────────────────────────────────────── */
static void input(void) {
    if(!kbhit()) return;
    int c=tgetc();
    if(c==27){ /* arrow keys */
        if(tgetc()=='['){
            switch(tgetc()){
                case 'A': if(G.cy>0) G.cy--; break;
                case 'B': if(G.cy<MH-1) G.cy++; break;
                case 'C': if(G.cx<MW-1) G.cx++; break;
                case 'D': if(G.cx>0) G.cx--; break;
            }
        }
        return;
    }
    switch(c){
        case 'h': if(G.cx>0) G.cx--; break;
        case 'j': if(G.cy<MH-1) G.cy++; break;
        case 'k': if(G.cy>0) G.cy--; break;
        case 'l': if(G.cx<MW-1) G.cx++; break;
        case '1': place(G.cx,G.cy,0); break;
        case '2': place(G.cx,G.cy,1); break;
        case '3': place(G.cx,G.cy,2); break;
        case '4': place(G.cx,G.cy,3); break;
        case 'u': { Tower *t=tw_at(G.cx,G.cy); if(t) upgrade(t); } break;
        case 'x': { Tower *t=tw_at(G.cx,G.cy); if(t) sell(t); } break;
        case 'r': G.show_rng=!G.show_rng; break;
        case ' ': if(!G.wave_on&&!G.over) start_wave(); break;
        case 'q': G.running=0; break;
    }
}

/* ── render ────────────────────────────────────────────────────── */
static void render(void) {
    /* build character & colour buffers */
    char buf[MH][MW+1];
    int  col[MH][MW]; /* colour index: 0=grass 1=path 2=entrance 3=exit
                          10-13=towers 20-22=fx 30=frozen 40-43=enemies */

    for(int y=0;y<MH;y++){
        memcpy(buf[y],G.map[y],MW+1);
        for(int x=0;x<MW;x++) col[y][x]=0;
    }
    for(int y=0;y<MH;y++)
        for(int x=0;x<MW;x++){
            if(G.map[y][x]=='#') col[y][x]=1;
            else if(G.map[y][x]=='>') col[y][x]=2;
            else if(G.map[y][x]=='!') col[y][x]=3;
        }

    /* towers */
    for(int i=0;i<G.nt;i++){
        Tower *t=&G.tw[i];
        int x=t->x, y=t->y;
        char syms[]="CLFS";
        buf[y][x] = t->lvl>=3 ? syms[t->type] : (syms[t->type]|0x20);
        col[y][x] = 10+t->type;
    }

    /* effects (under enemies) */
    for(int i=0;i<G.nfx;i++){
        FX *f=&G.fx[i];
        int x=(int)(f->x+.5), y=(int)(f->y+.5);
        if(x>=0&&x<MW&&y>=0&&y<MH){
            float t=(float)f->life/f->ml;
            buf[y][x] = f->k=='E' ? (t>0.5?'*':'.') : '+';
            col[y][x] = f->k=='E' ? 20 : 21;
        }
    }

    /* enemies */
    static const char esym[]="o>#X";
    for(int i=0;i<G.ne;i++){
        Enemy *e=&G.en[i];
        if(!e->alive) continue;
        int x=(int)(e->x+.5), y=(int)(e->y+.5);
        if(x<0||x>=MW||y<0||y>=MH) continue;
        buf[y][x] = esym[e->type];
        col[y][x] = e->slow_t>0 ? 30 : 40+e->type;
    }

    /* projectiles */
    for(int i=0;i<G.np;i++){
        Proj *p=&G.pj[i];
        if(!p->alive) continue;
        int x=(int)(p->x+.5), y=(int)(p->y+.5);
        if(x>=0&&x<MW&&y>=0&&y<MH){
            buf[y][x]= p->splash>0 ? 'o' : '-';
            col[y][x]=22;
        }
    }

    /* ── draw ──────────────────────────────────────────────── */
    printf(E"[H");  /* home cursor */

    /* title bar */
    printf(BLD YEL" TOWER DEFENSE"RST);
    printf("  Wave:"BLD"%d"RST, G.wave);
    printf("  Gold:"BYEL"%d"RST, G.gold);
    printf("  Lives:"BRED"%d"RST, G.lives);
    printf("  Score:"BCYN"%d"RST, G.score);
    if(G.wave_on) printf("  "DIM"[Wave in progress]"RST);
    else if(!G.over) printf("  "BGRN"[SPACE: Start wave]"RST);
    printf(E"[K\n");

    /* map rows */
    for(int y=0;y<MH;y++){
        printf(" ");
        for(int x=0;x<MW;x++){
            /* cursor highlight */
            int cur = (x==G.cx && y==G.cy);

            /* range highlight */
            int rng_hl = 0;
            if(G.show_rng){
                for(int i=0;i<G.nt;i++){
                    double r=TD[G.tw[i].type].rng*(1+(G.tw[i].lvl-1)*0.15);
                    if(dist(x,y,G.tw[i].x,G.tw[i].y)<=r+0.5){
                        rng_hl=1; break;
                    }
                }
            }

            if(cur) printf(REV);

            switch(col[y][x]){
            case  0: printf(rng_hl?DIM BLU:GRN); break;
            case  1: printf(YEL DIM); break;
            case  2: printf(BGRN BLD); break;
            case  3: printf(BRED BLD); break;
            case 10: printf(BYEL BLD); break;
            case 11: printf(RED BLD);  break;
            case 12: printf(CYN BLD);  break;
            case 13: printf(MAG BLD);  break;
            case 20: printf(BYEL BLD); break;
            case 21: printf(WHT BLD);  break;
            case 22: printf(WHT);      break;
            case 30: printf(BLU BLD);  break;
            case 40: printf(BGRN BLD); break;
            case 41: printf(BYEL BLD); break;
            case 42: printf(BRED BLD); break;
            case 43: printf(MAG BLD);  break;
            }

            char ch = buf[y][x];
            if(ch=='.') ch=' ';
            if(ch=='#') ch=':';
            putchar(ch);
            printf(RST);
        }
        printf(E"[K\n");
    }

    /* ── laser beams (drawn directly on screen) ────────────── */
    for(int i=0;i<G.nb;i++){
        Beam *b=&G.bm[i];
        /* draw dotted line with Bresenham-ish approach */
        int x1=(int)b->x1, y1=(int)b->y1;
        int x2=(int)b->x2, y2=(int)b->y2;
        int dx=abs(x2-x1), dy=abs(y2-y1);
        int steps = dx>dy?dx:dy;
        if(steps==0) steps=1;
        for(int s=0;s<=steps;s++){
            int px=x1+(x2-x1)*s/steps;
            int py=y1+(y2-y1)*s/steps;
            if(px>=0&&px<MW&&py>=0&&py<MH){
                printf(E"[%d;%dH",py+2,px+2);
                printf(RED BLD"|"RST);
            }
        }
    }

    /* ── info bar ──────────────────────────────────────────── */
    printf(E"[%dH",MH+2);
    Tower *ct = tw_at(G.cx, G.cy);
    if(ct){
        const TDef *d=&TD[ct->type];
        double rng=d->rng*(1+(ct->lvl-1)*0.15);
        printf(" "BLD"%s"RST" Lv%d  Kills:%d  Range:%.1f",
               d->name,ct->lvl,ct->kills,rng);
        if(ct->lvl<3) printf(DIM"  [u]Upgrade(%dg)"RST,
                              d->cost*ct->lvl);
        printf(DIM"  [x]Sell(%dg)"RST, ct->tcost/2);
    } else if(G.cx>=0&&G.cx<MW&&G.cy>=0&&G.cy<MH&&G.map[G.cy][G.cx]=='.'){
        printf(" "DIM"[1]Cannon(%dg) [2]Laser(%dg) "
               "[3]Frost(%dg) [4]Sniper(%dg)"RST,
               TD[0].cost,TD[1].cost,TD[2].cost,TD[3].cost);
    } else {
        printf(" Move: hjkl/Arrows | [r]Ranges | [q]Quit");
    }
    printf(E"[K\n");

    /* tower legend */
    printf(" ");
    for(int i=0;i<NTD;i++){
        const char *c = i==0?BYEL:i==1?RED:i==2?CYN:MAG;
        printf("%s[%d]%c"RST" %s  ",c,i+1,TD[i].sym,TD[i].desc);
    }
    printf(E"[K\n");

    /* enemy legend */
    printf(" Enemies: "BGRN"o"RST"Normal "BYEL">"RST"Fast "
           BRED"#"RST"Tank "MAG"X"RST"Boss  "
           BLU"*"RST"=Frozen"E"[K\n");

    if(G.over){
        printf(E"[K\n");
        printf(BLD RED"   GAME OVER!  Final Score: %d  Wave: %d  "
               "Press q to quit"RST E"[K\n", G.score, G.wave);
    }
}

/* ── init & main ───────────────────────────────────────────────── */
static void ginit(void) {
    srand((unsigned)time(NULL));
    memset(&G,0,sizeof G);
    build_path();
    build_map();
    G.gold=200; G.lives=20; G.running=1;
    G.cx=10; G.cy=0;
}

int main(void) {
    ginit();
    tinit();
    while(G.running){
        input();
        if(!G.over){
            upd_spawn(); upd_enemies();
            upd_towers(); upd_proj(); upd_fx();
            G.tick++;
        }
        render();
        usleep(TICK_US);
    }
    trestore();
    printf("Tower Defense — Score: %d  Wave: %d\n", G.score, G.wave);
    return 0;
}
