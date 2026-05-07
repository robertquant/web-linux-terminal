/*
 * ══════════════════════════════════════════════════════════════════════════
 *  DESCENT INTO DARKNESS — Terminal Roguelike Dungeon Crawler
 * ══════════════════════════════════════════════════════════════════════════
 *
 *  Compile:  gcc -O2 -std=c11 -o roguelike roguelike-3152.c -lm
 *  Run:      ./roguelike
 *
 *  Requires: ANSI terminal with color support (Linux/macOS)
 *
 *  Features:
 *    • Procedural dungeon generation (rooms + corridors)
 *    • Fog of war with memory tiles
 *    • Turn-based combat with多种怪物
 *    • Items: potions, weapons, armor, scrolls
 *    • Multiple dungeon floors (descend stairs with '>')
 *    • Experience & leveling system
 *    • Minimap overlay
 *    • Message log
 *
 *  Controls:
 *    h/j/k/l  or  arrows   — Move / attack
 *    g                     — Pick up item
 *    i                     — Use item from inventory (1-9)
 *    >                     — Descend stairs
 *    m                     — Toggle minimap
 *    q                     — Quit
 *
 *  Author: Claude  |  License: MIT
 * ══════════════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include <stdarg.h>

/* ─── Constants ─────────────────────────────────────────────────────────── */

#define MAP_W        80
#define MAP_H        40
#define MAX_ROOMS    12
#define MAX_ITEMS    64
#define MAX_MONSTERS 32
#define MAX_INV      20
#define MAX_MSG      200
#define FLOORS_MAX   10
#define VIEW_RADIUS  6
#define MSG_LINES    5
#define BAR_W        20

/* Tile types */
enum {
    T_WALL, T_FLOOR, T_CORRIDOR, T_DOOR, T_STAIRS_DOWN, T_WATER, T_PILLAR
};

/* Item types */
enum {
    I_POTION_HP, I_POTION_STR, I_SCROLL_FIRE, I_SCROLL_MAP,
    I_WPN_SWORD, I_WPN_AXE, I_WPN_DAGGER, I_ARM_CHAIN, I_ARM_LEATHER
};

/* Monster IDs */
enum {
    M_RAT, M_GOBLIN, M_SKELETON, M_ORC, M_TROLL, M_WRAITH, M_DRAGON
};

/* Colors (ANSI fg) */
static const int tile_color[] = {
    90, 37, 248, 136, 93, 39, 102
};
static const char tile_char[] = {
    '#', '.', '#', '+', '>', '~', 'O'
};

/* ─── Data Structures ───────────────────────────────────────────────────── */

typedef struct { int x, y, w, h; } Room;

typedef struct {
    int type, x, y;
    char name[32];
    int power;          /* healing / damage / stat boost */
    int str_bonus;
    int def_bonus;
} Item;

typedef struct {
    int id, x, y, hp, max_hp, atk, def, exp_val;
    char name[20];
    char glyph;
    int color;
    int awake;
} Monster;

typedef struct {
    int type;
    int visible, seen;
} Tile;

/* Player */
static struct {
    int x, y, hp, max_hp, str, def, exp, level, floor;
    int gold;
    Item inv[MAX_INV];
    int inv_count;
} player;

/* Game state */
static Tile     map[MAP_H][MAP_W];
static Room     rooms[MAX_ROOMS];
static int      room_count;
static Item     items[MAX_ITEMS];
static int      item_count;
static Monster  monsters[MAX_MONSTERS];
static int      mon_count;
static char     msglog[MAX_MSG][128];
static int      msg_count, msg_start;
static int      game_over, show_minimap;
static int      turn;

/* ─── Utilities ──────────────────────────────────────────────────────────── */

static int rand_range(int lo, int hi) {
    return lo + rand() % (hi - lo + 1);
}

static void add_msg(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (msg_count < MAX_MSG) {
        snprintf(msglog[msg_count], 128, "%s", buf);
        msg_count++;
    } else {
        memmove(msglog[0], msglog[1], (MAX_MSG - 1) * 128);
        snprintf(msglog[MAX_MSG - 1], 128, "%s", buf);
    }
    msg_start = msg_count - MSG_LINES;
    if (msg_start < 0) msg_start = 0;
}

/* ─── Dungeon Generation ────────────────────────────────────────────────── */

static void fill_map(int tile) {
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++) {
            map[y][x].type    = tile;
            map[y][x].visible = 0;
            map[y][x].seen    = 0;
        }
}

static int room_overlaps(Room *r, int mx, int my, int pad) {
    return mx >= r->x - pad && mx < r->x + r->w + pad &&
           my >= r->y - pad && my < r->y + r->h + pad;
}

static void carve_room(Room *r) {
    for (int y = r->y; y < r->y + r->h; y++)
        for (int x = r->x; x < r->x + r->w; x++)
            if (y > 0 && y < MAP_H - 1 && x > 0 && x < MAP_W - 1)
                map[y][x].type = T_FLOOR;
}

static void carve_corridor(int x1, int y1, int x2, int y2) {
    int x = x1, y = y1;
    while (x != x2) {
        if (y > 0 && y < MAP_H - 1 && x > 0 && x < MAP_W - 1)
            if (map[y][x].type == T_WALL) map[y][x].type = T_CORRIDOR;
        x += (x2 > x) ? 1 : -1;
    }
    while (y != y2) {
        if (y > 0 && y < MAP_H - 1 && x > 0 && x < MAP_W - 1)
            if (map[y][x].type == T_WALL) map[y][x].type = T_CORRIDOR;
        y += (y2 > y) ? 1 : -1;
    }
}

static void generate_dungeon(int floor_num) {
    fill_map(T_WALL);
    room_count = 0;

    /* Place rooms */
    int attempts = 300;
    while (room_count < MAX_ROOMS && attempts-- > 0) {
        Room r;
        r.w = rand_range(5, 12);
        r.h = rand_range(4, 8);
        r.x = rand_range(1, MAP_W - r.w - 2);
        r.y = rand_range(1, MAP_H - r.h - 2);
        int ok = 1;
        for (int i = 0; i < room_count && ok; i++)
            for (int dy = 0; dy < r.h && ok; dy++)
                for (int dx = 0; dx < r.w && ok; dx++)
                    if (room_overlaps(&rooms[i], r.x + dx, r.y + dy, 1))
                        ok = 0;
        if (ok) {
            rooms[room_count] = r;
            carve_room(&r);
            room_count++;
        }
    }

    /* Connect rooms with corridors */
    for (int i = 1; i < room_count; i++) {
        int cx1 = rooms[i - 1].x + rooms[i - 1].w / 2;
        int cy1 = rooms[i - 1].y + rooms[i - 1].h / 2;
        int cx2 = rooms[i].x + rooms[i].w / 2;
        int cy2 = rooms[i].y + rooms[i].h / 2;
        carve_corridor(cx1, cy1, cx2, cy2);
    }

    /* Place doors at room entrances */
    for (int i = 0; i < room_count; i++) {
        Room *r = &rooms[i];
        /* Check perimeter for corridor adjacents */
        for (int x = r->x; x < r->x + r->w; x++) {
            if (map[r->y - 1][x].type == T_CORRIDOR && map[r->y][x].type == T_FLOOR)
                map[r->y][x].type = T_DOOR;
            if (map[r->y + r->h][x].type == T_CORRIDOR && map[r->y + r->h - 1][x].type == T_FLOOR)
                map[r->y + r->h - 1][x].type = T_DOOR;
        }
        for (int y = r->y; y < r->y + r->h; y++) {
            if (map[y][r->x - 1].type == T_CORRIDOR && map[y][r->x].type == T_FLOOR)
                map[y][r->x].type = T_DOOR;
            if (map[y][r->x + r->w].type == T_CORRIDOR && map[y][r->x + r->w - 1].type == T_FLOOR)
                map[y][r->x + r->w - 1].type = T_DOOR;
        }
    }

    /* Scatter pillars in larger rooms */
    for (int i = 0; i < room_count; i++) {
        if (rooms[i].w >= 8 && rooms[i].h >= 6) {
            int px = rooms[i].x + rooms[i].w / 2;
            int py = rooms[i].y + rooms[i].h / 2;
            map[py][px].type = T_PILLAR;
        }
    }

    /* Place stairs down in last room */
    if (floor_num < FLOORS_MAX) {
        Room *lr = &rooms[room_count - 1];
        int sx = lr->x + lr->w / 2;
        int sy = lr->y + lr->h / 2;
        map[sy][sx].type = T_STAIRS_DOWN;
    }
}

/* ─── Item Generation ───────────────────────────────────────────────────── */

static const char *item_names[] = {
    "Health Potion", "Str Potion", "Scroll of Fire", "Scroll of Map",
    "Iron Sword", "Battle Axe", "Shadow Dagger", "Chain Mail", "Leather Armor"
};
static const char item_glyphs[] = { '!', '!', '?', '?', ')', ')', ')', ']', ']' };
static const int  item_colors[] = { 32, 201, 196, 226, 37, 33, 93, 114, 180 };
static const int  item_power[]  = { 15, 2, 20, 0, 3, 5, 2, 0, 0 };

static void spawn_items(int floor_num) {
    item_count = 0;
    int num_items = rand_range(6, 10) + floor_num;
    if (num_items > MAX_ITEMS) num_items = MAX_ITEMS;

    for (int i = 0; i < num_items; i++) {
        Room *r = &rooms[rand_range(0, room_count - 1)];
        Item it;
        it.x = rand_range(r->x + 1, r->x + r->w - 2);
        it.y = rand_range(r->y + 1, r->y + r->h - 2);
        it.str_bonus = 0;
        it.def_bonus = 0;

        /* Weight by floor */
        int roll = rand_range(0, 100) + floor_num * 5;
        if (roll < 25)       it.type = I_POTION_HP;
        else if (roll < 38)  it.type = I_POTION_STR;
        else if (roll < 50)  it.type = I_SCROLL_FIRE;
        else if (roll < 58)  it.type = I_SCROLL_MAP;
        else if (roll < 68)  it.type = I_WPN_DAGGER;
        else if (roll < 78)  it.type = I_WPN_SWORD;
        else if (roll < 86)  it.type = I_WPN_AXE;
        else if (roll < 93)  it.type = I_ARM_LEATHER;
        else                 it.type = I_ARM_CHAIN;

        snprintf(it.name, 32, "%s", item_names[it.type]);
        it.power = item_power[it.type] + floor_num;

        switch (it.type) {
            case I_WPN_SWORD:  it.str_bonus = 3 + floor_num; break;
            case I_WPN_AXE:    it.str_bonus = 5 + floor_num; break;
            case I_WPN_DAGGER: it.str_bonus = 2 + floor_num; break;
            case I_ARM_CHAIN:  it.def_bonus = 4 + floor_num; break;
            case I_ARM_LEATHER:it.def_bonus = 2 + floor_num; break;
            default: break;
        }

        items[item_count++] = it;
    }
}

/* ─── Monster Generation ────────────────────────────────────────────────── */

typedef struct {
    char name[20]; char glyph; int color;
    int hp, atk, def, exp;
} MonInfo;

static const MonInfo mon_db[] = {
    {"Rat",      'r', 130,  4, 2, 0,   3},
    {"Goblin",   'g',  34,  8, 4, 1,   8},
    {"Skeleton", 's',  94, 12, 5, 2,  14},
    {"Orc",      'o',  28, 18, 7, 3,  22},
    {"Troll",    'T',  64, 30, 10,5,  40},
    {"Wraith",   'W',  57, 22, 12,2,  55},
    {"Dragon",   'D', 202, 50, 18,8, 120},
};

static void spawn_monsters(int floor_num) {
    mon_count = 0;
    int num_mon = rand_range(5, 8) + floor_num * 2;
    if (num_mon > MAX_MONSTERS) num_mon = MAX_MONSTERS;

    for (int i = 0; i < num_mon; i++) {
        Room *r = &rooms[rand_range(0, room_count - 1)];
        int mx = rand_range(r->x + 1, r->x + r->w - 2);
        int my = rand_range(r->y + 1, r->y + r->h - 2);
        if (mx == player.x && my == player.y) continue;

        /* Pick monster type based on floor */
        int max_id = floor_num < 3 ? 2 : floor_num < 5 ? 4 : floor_num < 8 ? 5 : 6;
        int id = rand_range(0, max_id);

        Monster m;
        m.id = id;
        m.x = mx; m.y = my;
        snprintf(m.name, 20, "%s", mon_db[id].name);
        m.glyph  = mon_db[id].glyph;
        m.color  = mon_db[id].color;
        m.hp     = mon_db[id].hp + floor_num * 2;
        m.max_hp = m.hp;
        m.atk    = mon_db[id].atk + floor_num;
        m.def    = mon_db[id].def;
        m.exp_val= mon_db[id].exp + floor_num * 3;
        m.awake  = 0;
        monsters[mon_count++] = m;
    }
}

/* ─── Field of View (simple raycasting) ─────────────────────────────────── */

static void compute_fov(void) {
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++)
            map[y][x].visible = 0;

    for (double a = 0; a < 6.2832; a += 0.03) {
        double dx = cos(a), dy = sin(a);
        double fx = player.x + 0.5, fy = player.y + 0.5;
        for (int s = 0; s < VIEW_RADIUS * 2; s++) {
            int ix = (int)fx, iy = (int)fy;
            if (ix < 0 || ix >= MAP_W || iy < 0 || iy >= MAP_H) break;
            int dist = abs(ix - player.x) + abs(iy - player.y);
            if (dist > VIEW_RADIUS) break;
            map[iy][ix].visible = 1;
            map[iy][ix].seen = 1;
            if (map[iy][ix].type == T_WALL) break;
            fx += dx * 0.5;
            fy += dy * 0.5;
        }
    }
}

/* ─── Player / Monsters ─────────────────────────────────────────────────── */

static Monster *monster_at(int x, int y) {
    for (int i = 0; i < mon_count; i++)
        if (monsters[i].hp > 0 && monsters[i].x == x && monsters[i].y == y)
            return &monsters[i];
    return NULL;
}

static Item *item_at(int x, int y) {
    for (int i = 0; i < item_count; i++)
        if (items[i].x == x && items[i].y == y)
            return &items[i];
    return NULL;
}

static void gain_exp(int amt) {
    player.exp += amt;
    int need = player.level * 25 + 10;
    while (player.exp >= need) {
        player.exp -= need;
        player.level++;
        player.max_hp += 5;
        player.hp = player.max_hp;
        player.str += 1;
        player.def += 1;
        add_msg("Level up! You are now level %d!", player.level);
        need = player.level * 25 + 10;
    }
}

static void player_attack(Monster *m) {
    int dmg = player.str + rand_range(1, 4) - m->def;
    if (dmg < 1) dmg = 1;
    m->hp -= dmg;
    add_msg("You hit the %s for %d damage!", m->name, dmg);
    if (m->hp <= 0) {
        add_msg("The %s is slain! (+%d exp)", m->name, m->exp_val);
        gain_exp(m->exp_val);
        player.gold += rand_range(1, 5) + player.floor * 2;
    }
}

static void monster_attack(Monster *m) {
    int dmg = m->atk + rand_range(0, 3) - player.def;
    if (dmg < 1) dmg = 1;
    player.hp -= dmg;
    add_msg("The %s hits you for %d damage!", m->name, dmg);
    if (player.hp <= 0) {
        game_over = 1;
        add_msg("You have been slain by %s on floor %d!", m->name, player.floor);
    }
}

static int try_move(int dx, int dy) {
    int nx = player.x + dx, ny = player.y + dy;
    if (nx < 0 || nx >= MAP_W || ny < 0 || ny >= MAP_H) return 0;

    Monster *m = monster_at(nx, ny);
    if (m) {
        player_attack(m);
        return 1;
    }

    if (map[ny][nx].type == T_WALL || map[ny][nx].type == T_PILLAR) return 0;

    player.x = nx;
    player.y = ny;

    /* Auto-pickup gold from killing is handled in combat, but check for items to hint */
    Item *it = item_at(nx, ny);
    if (it) add_msg("You see a %s here. Press 'g' to pick up.", it->name);

    return 1;
}

/* ─── Monster AI ─────────────────────────────────────────────────────────── */

static void monsters_turn(void) {
    for (int i = 0; i < mon_count; i++) {
        Monster *m = &monsters[i];
        if (m->hp <= 0) continue;

        int dist = abs(m->x - player.x) + abs(m->y - player.y);
        if (dist <= VIEW_RADIUS) m->awake = 1;
        if (!m->awake) continue;

        /* Simple chase: move toward player */
        int dx = 0, dy = 0;
        if (player.x > m->x) dx = 1;
        else if (player.x < m->x) dx = -1;
        if (player.y > m->y) dy = 1;
        else if (player.y < m->y) dy = -1;

        /* Try primary direction, then secondary */
        int moved = 0;
        if (abs(player.x - m->x) >= abs(player.y - m->y)) {
            int nx = m->x + dx, ny = m->y;
            if (nx >= 0 && nx < MAP_W && map[ny][nx].type != T_WALL && map[ny][nx].type != T_PILLAR) {
                if (nx == player.x && ny == player.y) { monster_attack(m); moved = 1; }
                else if (!monster_at(nx, ny)) { m->x = nx; moved = 1; }
            }
            if (!moved) {
                int nx2 = m->x, ny2 = m->y + dy;
                if (ny2 >= 0 && ny2 < MAP_H && map[ny2][nx2].type != T_WALL && map[ny2][nx2].type != T_PILLAR) {
                    if (nx2 == player.x && ny2 == player.y) monster_attack(m);
                    else if (!monster_at(nx2, ny2)) m->y = ny2;
                }
            }
        } else {
            int nx = m->x, ny = m->y + dy;
            if (ny >= 0 && ny < MAP_H && map[ny][nx].type != T_WALL && map[ny][nx].type != T_PILLAR) {
                if (nx == player.x && ny == player.y) { monster_attack(m); moved = 1; }
                else if (!monster_at(nx, ny)) { m->y = ny; moved = 1; }
            }
            if (!moved) {
                int nx2 = m->x + dx, ny2 = m->y;
                if (nx2 >= 0 && nx2 < MAP_W && map[ny2][nx2].type != T_WALL && map[ny2][nx2].type != T_PILLAR) {
                    if (nx2 == player.x && ny2 == player.y) monster_attack(m);
                    else if (!monster_at(nx2, ny2)) m->x = nx2;
                }
            }
        }
    }
}

/* ─── Items / Inventory ─────────────────────────────────────────────────── */

static void pick_up(void) {
    Item *it = item_at(player.x, player.y);
    if (!it) { add_msg("Nothing to pick up here."); return; }
    if (player.inv_count >= MAX_INV) { add_msg("Inventory full!"); return; }

    /* Weapons/armor: auto-equip */
    if (it->type >= I_WPN_DAGGER && it->type <= I_WPN_AXE) {
        player.str += it->str_bonus;
        add_msg("Equipped %s! (STR +%d)", it->name, it->str_bonus);
    } else if (it->type >= I_ARM_LEATHER) {
        player.def += it->def_bonus;
        add_msg("Equipped %s! (DEF +%d)", it->name, it->def_bonus);
    } else {
        add_msg("Picked up %s.", it->name);
    }

    player.inv[player.inv_count++] = *it;
    /* Remove from map */
    for (int i = 0; i < item_count; i++) {
        if (&items[i] == it) {
            items[i] = items[--item_count];
            break;
        }
    }
}

static void use_item(int slot) {
    if (slot < 0 || slot >= player.inv_count) { add_msg("Invalid slot."); return; }
    Item *it = &player.inv[slot];
    switch (it->type) {
        case I_POTION_HP:
            player.hp += it->power;
            if (player.hp > player.max_hp) player.hp = player.max_hp;
            add_msg("Drank %s! Restored %d HP.", it->name, it->power);
            break;
        case I_POTION_STR:
            player.str += it->power;
            add_msg("Drank %s! STR +%d!", it->name, it->power);
            break;
        case I_SCROLL_FIRE: {
            add_msg("You read %s! Flames burst forth!", it->name);
            for (int i = 0; i < mon_count; i++) {
                if (monsters[i].hp > 0) {
                    int d = abs(monsters[i].x - player.x) + abs(monsters[i].y - player.y);
                    if (d <= 4) {
                        int dmg = it->power + rand_range(5, 15);
                        monsters[i].hp -= dmg;
                        add_msg("The %s takes %d fire damage!", monsters[i].name, dmg);
                        if (monsters[i].hp <= 0) {
                            add_msg("The %s burns to ashes! (+%d exp)",
                                    monsters[i].name, monsters[i].exp_val);
                            gain_exp(monsters[i].exp_val);
                        }
                    }
                }
            }
            break;
        }
        case I_SCROLL_MAP:
            add_msg("Scroll of Map! All nearby area revealed!");
            for (int y = 0; y < MAP_H; y++)
                for (int x = 0; x < MAP_W; x++) {
                    int d = abs(x - player.x) + abs(y - player.y);
                    if (d < 20) map[y][x].seen = 1;
                }
            break;
        default:
            add_msg("You can't use that directly — it's passive equipment.");
            return;
    }
    /* Remove from inventory */
    player.inv[slot] = player.inv[--player.inv_count];
}

/* ─── Descend Stairs ────────────────────────────────────────────────────── */

static void descend(void) {
    if (map[player.y][player.x].type != T_STAIRS_DOWN) {
        add_msg("There are no stairs here.");
        return;
    }
    if (player.floor >= FLOORS_MAX) {
        add_msg("You've reached the bottom! You win!");
        game_over = 1;
        return;
    }
    player.floor++;
    add_msg("You descend to floor %d...", player.floor);
    generate_dungeon(player.floor);
    player.x = rooms[0].x + rooms[0].w / 2;
    player.y = rooms[0].y + rooms[0].h / 2;
    spawn_items(player.floor);
    spawn_monsters(player.floor);
    compute_fov();
}

/* ─── Rendering ──────────────────────────────────────────────────────────── */

static void ansi_fg(int c) { printf("\033[38;5;%dm", c); }
static void ansi_bg(int c) { printf("\033[48;5;%dm", c); }
static void ansi_reset(void) { printf("\033[0m"); }
static void clear_screen(void) { printf("\033[2J\033[H"); }
static void move_cursor(int r, int c) { printf("\033[%d;%dH", r, c); }

static void draw_bar(int y, int x, const char *label, int val, int max, int color) {
    move_cursor(y, x);
    printf("%s:", label);
    int filled = max > 0 ? (val * BAR_W) / max : 0;
    if (filled > BAR_W) filled = BAR_W;
    ansi_fg(color);
    printf("[");
    for (int i = 0; i < BAR_W; i++)
        printf("%s", i < filled ? "█" : "░");
    printf("]");
    ansi_reset();
    printf(" %d/%d", val, max);
}

static void draw_map(void) {
    for (int y = 0; y < MAP_H; y++) {
        move_cursor(y + 1, 1);
        for (int x = 0; x < MAP_W; x++) {
            Tile *t = &map[y][x];

            /* Player */
            if (x == player.x && y == player.y) {
                ansi_fg(255); printf("@"); ansi_reset();
                continue;
            }

            /* Visible monster */
            if (t->visible) {
                Monster *m = monster_at(x, y);
                if (m) {
                    ansi_fg(m->color); printf("%c", m->glyph); ansi_reset();
                    continue;
                }
                Item *it = item_at(x, y);
                if (it) {
                    ansi_fg(item_colors[it->type]);
                    printf("%c", item_glyphs[it->type]);
                    ansi_reset();
                    continue;
                }
            }

            if (!t->seen) {
                printf(" ");
                continue;
            }

            int dim = t->visible ? 1 : 0;
            if (dim) ansi_fg(tile_color[t->type]);
            else     ansi_fg(238); /* dim seen tiles */
            printf("%c", tile_char[t->type]);
            ansi_reset();
        }
    }
}

static void draw_hud(void) {
    int hud_x = MAP_W + 2;

    move_cursor(1, hud_x);
    ansi_fg(220); printf("╔══════════════╗"); ansi_reset();

    move_cursor(2, hud_x);
    ansi_fg(220); printf("║ DESCENT INTO ║"); ansi_reset();

    move_cursor(3, hud_x);
    ansi_fg(220); printf("║   DARKNESS   ║"); ansi_reset();

    move_cursor(4, hud_x);
    ansi_fg(220); printf("╚══════════════╝"); ansi_reset();

    move_cursor(5, hud_x);
    ansi_fg(214); printf("Floor: %d  Turn: %d", player.floor, turn); ansi_reset();

    move_cursor(6, hud_x);
    ansi_fg(226); printf("Level: %d", player.level); ansi_reset();

    draw_bar(7, hud_x, "HP ", player.hp, player.max_hp, 196);
    draw_bar(8, hud_x, "EXP", player.exp, player.level * 25 + 10, 34);

    move_cursor(9, hud_x);
    ansi_fg(82); printf("STR: %d  DEF: %d", player.str, player.def); ansi_reset();

    move_cursor(10, hud_x);
    ansi_fg(214); printf("Gold: %d", player.gold); ansi_reset();

    /* Controls */
    int cy = 12;
    move_cursor(cy++, hud_x);
    ansi_fg(248); printf("─── Controls ───"); ansi_reset();
    const char *help[] = {
        "hjkl/Arrows: Move",
        "g: Pick up item",
        "i: Use inventory",
        ">: Descend stairs",
        "m: Toggle minimap",
        "q: Quit",
        NULL
    };
    for (int i = 0; help[i]; i++) {
        move_cursor(cy++, hud_x);
        ansi_fg(246); printf("%s", help[i]); ansi_reset();
    }

    /* Message log */
    cy = MAP_H - MSG_LINES;
    move_cursor(cy++, hud_x);
    ansi_fg(248); printf("─── Messages ───"); ansi_reset();
    for (int i = 0; i < MSG_LINES; i++) {
        move_cursor(cy++, hud_x);
        int idx = msg_start + i;
        if (idx >= 0 && idx < msg_count) {
            ansi_fg(252); printf("%.16s", msglog[idx]); ansi_reset();
        }
    }
}

static void draw_minimap(void) {
    if (!show_minimap) return;
    int mm_w = 30, mm_h = 15;
    int sx = MAP_W / 2 - mm_w / 2;
    int sy = MAP_H / 2 - mm_h / 2;
    int ox = MAP_W / 2 - mm_w / 2;
    int oy = MAP_H / 2 - mm_h / 2;

    /* Border */
    move_cursor(oy, ox - 1);
    ansi_fg(240); printf("+");
    for (int i = 0; i < mm_w; i++) printf("-");
    printf("+"); ansi_reset();

    for (int y = 0; y < mm_h; y++) {
        move_cursor(oy + 1 + y, ox - 1);
        ansi_fg(240); printf("|"); ansi_reset();
        for (int x = 0; x < mm_w; x++) {
            int mx = sx + x * (MAP_W / mm_w);
            int my = sy + y * (MAP_H / mm_h);
            if (mx == player.x && my == player.y) {
                ansi_fg(255); printf("@"); ansi_reset();
            } else if (map[my][mx].seen) {
                if (map[my][mx].type == T_WALL) { ansi_fg(238); printf("#"); }
                else if (map[my][mx].type == T_STAIRS_DOWN) { ansi_fg(93); printf(">"); }
                else { ansi_fg(244); printf("."); }
                ansi_reset();
            } else {
                printf(" ");
            }
        }
        ansi_fg(240); printf("|"); ansi_reset();
    }
    move_cursor(oy + mm_h + 1, ox - 1);
    ansi_fg(240); printf("+");
    for (int i = 0; i < mm_w; i++) printf("-");
    printf("+"); ansi_reset();
}

static void draw_inventory_screen(void) {
    move_cursor(MAP_H / 2 - 6, 20);
    ansi_fg(226); printf("╔═════════ INVENTORY ═════════╗"); ansi_reset();
    for (int i = 0; i < MAX_INV && i < 12; i++) {
        move_cursor(MAP_H / 2 - 5 + i, 20);
        ansi_fg(244); printf("║"); ansi_reset();
        if (i < player.inv_count) {
            ansi_fg(item_colors[player.inv[i].type]);
            printf(" %d) %-12s %c", i + 1, player.inv[i].name, item_glyphs[player.inv[i].type]);
            if (player.inv[i].str_bonus) printf(" STR+%d", player.inv[i].str_bonus);
            if (player.inv[i].def_bonus) printf(" DEF+%d", player.inv[i].def_bonus);
        } else {
            printf(" %d) (empty)", i + 1);
        }
        ansi_fg(244); printf("          ║"); ansi_reset();
    }
    move_cursor(MAP_H / 2 + 7, 20);
    ansi_fg(226); printf("╚════ Press number to use ════╝"); ansi_reset();
}

static void draw_game_over(void) {
    move_cursor(MAP_H / 2 - 2, MAP_W / 2 - 15);
    ansi_fg(196); printf("╔══════════════════════════════╗"); ansi_reset();
    move_cursor(MAP_H / 2 - 1, MAP_W / 2 - 15);
    ansi_fg(196); printf("║     GAME  OVER              ║"); ansi_reset();
    move_cursor(MAP_H / 2, MAP_W / 2 - 15);
    ansi_fg(196); printf("║ Floor: %-3d  Level: %-3d        ║", player.floor, player.level); ansi_reset();
    move_cursor(MAP_H / 2 + 1, MAP_W / 2 - 15);
    ansi_fg(214); printf("║ Gold: %-5d  Turns: %-5d      ║", player.gold, turn); ansi_reset();
    move_cursor(MAP_H / 2 + 2, MAP_W / 2 - 15);
    ansi_fg(196); printf("╚══════════════════════════════╝"); ansi_reset();
    move_cursor(MAP_H / 2 + 4, MAP_W / 2 - 10);
    ansi_fg(248); printf("Press 'q' to quit."); ansi_reset();
}

static void render(int inv_mode) {
    draw_map();
    draw_hud();
    if (inv_mode) draw_inventory_screen();
    if (show_minimap && !inv_mode) draw_minimap();
    if (game_over) draw_game_over();
    fflush(stdout);
}

/* ─── Input ──────────────────────────────────────────────────────────────── */

#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>

static struct termios orig_term;

static void raw_mode_on(void) {
    tcgetattr(STDIN_FILENO, &orig_term);
    struct termios raw = orig_term;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void raw_mode_off(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_term);
}

static int read_key(void) {
    struct timeval tv = {0, 50000};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) return 0;
    unsigned char buf[4] = {0};
    int n = (int)read(STDIN_FILENO, buf, 3);
    if (n <= 0) return 0;
    if (buf[0] == 27 && buf[1] == '[') {
        switch (buf[2]) {
            case 'A': return 'k';  /* Up */
            case 'B': return 'j';  /* Down */
            case 'C': return 'l';  /* Right */
            case 'D': return 'h';  /* Left */
        }
        return 0;
    }
    return tolower(buf[0]);
}
#else
/* Stub for Windows (would need _kbhit / _getch) */
static void raw_mode_on(void) {}
static void raw_mode_off(void) {}
static int read_key(void) {
    int c = getchar();
    return c == EOF ? 0 : tolower(c);
}
#endif

/* ─── Main Loop ──────────────────────────────────────────────────────────── */

int main(void) {
    srand((unsigned)time(NULL));
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Init player */
    memset(&player, 0, sizeof player);
    player.hp = 30; player.max_hp = 30;
    player.str = 5; player.def = 2;
    player.level = 1; player.floor = 1;

    /* Generate first floor */
    generate_dungeon(1);
    player.x = rooms[0].x + rooms[0].w / 2;
    player.y = rooms[0].y + rooms[0].h / 2;
    spawn_items(1);
    spawn_monsters(1);
    compute_fov();

    add_msg("Welcome to DESCENT INTO DARKNESS!");
    add_msg("Find the stairs (>) to go deeper.");
    add_msg("hjkl or arrow keys to move. g=pickup, i=inventory.");

    raw_mode_on();
    atexit(raw_mode_off);
    clear_screen();

    int inv_mode = 0;

    while (!game_over) {
        compute_fov();
        render(inv_mode);

        int key = read_key();
        if (!key) continue;

        if (inv_mode) {
            if (key >= '1' && key <= '9') {
                use_item(key - '1');
                inv_mode = 0;
                turn++;
                monsters_turn();
            } else {
                inv_mode = 0;
            }
            continue;
        }

        int moved = 0;
        switch (key) {
            case 'h': moved = try_move(-1,  0); break;
            case 'j': moved = try_move( 0,  1); break;
            case 'k': moved = try_move( 0, -1); break;
            case 'l': moved = try_move( 1,  0); break;
            case 'g': pick_up(); break;
            case 'i': inv_mode = 1; break;
            case '>': descend(); moved = 1; break;
            case 'm': show_minimap = !show_minimap; break;
            case 'q': game_over = 1; break;
        }

        if (moved) {
            turn++;
            monsters_turn();
        }
    }

    /* Show final screen */
    while (1) {
        render(0);
        int key = read_key();
        if (key == 'q') break;
    }

    clear_screen();
    ansi_reset();
    printf("Thanks for playing DESCENT INTO DARKNESS!\n");
    printf("Floor: %d  Level: %d  Gold: %d  Turns: %d\n",
           player.floor, player.level, player.gold, turn);
    return 0;
}
