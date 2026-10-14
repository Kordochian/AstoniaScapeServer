/*
 * Gathering and production skills: Mining -> Smithing, Woodcutting -> Crafting.
 * Everything is defined in zones/generic/skilling.cfg, see the header of that file.
 *
 * Item drivers (core, not in a DLL, so they work in every area):
 *   IDR_SKROCK     gathering spot on the map: rock (pickaxe), tree (axe) or plant
 *   IDR_SKSTACK    stackable resource (ore, bar, logs, arrows), drdata[0..3] = count
 *   IDR_SKSTATION  furnace, anvil, workbench or supply crate
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "server.h"
#include "log.h"
#include "talk.h"
#include "create.h"
#include "balance.h"
#include "tool.h"
#include "drvlib.h"
#include "map.h"
#include "player_driver.h"
#include "sector.h"
#include "mem.h"
#include "database.h"
#include "item_id.h"
#include "skillreg.h"
#include "skilling.h"
#include "player.h"
#include "io.h"

#define SKILLING_CFG "zones/generic/skilling.cfg"

#define SKT_RESOURCE 1
#define SKT_ROCK 2
#define SKT_PICKAXE 3
#define SKT_HAMMER 4
#define SKT_FURNACE 5
#define SKT_ANVIL 6
#define SKT_SMELT 7
#define SKT_SMITH 8
#define SKT_SUPPLY 9
#define SKT_TREE 10
#define SKT_AXE 11
#define SKT_KNIFE 12
#define SKT_BENCH 13
#define SKT_CRAFT 14
#define SKT_PLANT 15
#define SKT_MAX 15

#define MAXSKDEF 256
#define MAXNEED 4
#define STATION_RANGE 3 // /smelt, /smith and /craft work this many tiles from a furnace / anvil / workbench
#define MAXMAKE 100 // most makes one /smelt, /smith or /craft command does

static char *type_name[] = {"", "resource", "rock", "pickaxe", "hammer", "furnace", "anvil", "smelt", "smith", "supply",
                            "tree", "axe", "knife", "bench", "craft", "plant"};

struct skdef {
    char key[32];
    int type; // SKT_*
    int id; // item ID number (item-bearing types), saved with the items: never change
    char item[80]; // item template
    int tmp; // template number

    int level; // skill level needed
    int xp; // XP per ore / per item made
    int power; // tool power
    int ores, respawn, empty_sprite; // rock, tree, plant
    int stack_sprite[4]; // resource: sprite for a stack of 1, 2-9, 10-99, 100+ (0 = the template's)

    char skill_key[32]; // rock, tree, plant, recipe: skill it uses (empty = the type's default)
    int skill; // skill registry id, -1 = none
    char tool_key[16]; // rock, tree, plant, recipe: tool type it needs (empty = the type's default)
    int tool; // SKT_* tool type, 0 = none
    int amount; // recipe: how many of the resource one make gives
    int chance; // rock, tree, plant: base chance per try in percent
    char group_key[32];
    int group; // recipe: resource it is listed under in the client window (default: its first need)

    char ore_key[32];
    int ore; // rock, tree, plant: resource def it gives

    char make_key[80];
    int make_res; // recipe: resource def it makes, or -1
    int make_tmp; // recipe: item template it makes
    char name[40], desc[80]; // recipe: name/description of the made item (empty = template's)
    int fail; // recipe: fail chance in percent at its level, 2 less per level above

    int nneed;
    char need_key[MAXNEED][32];
    int need[MAXNEED], needcnt[MAXNEED];
};

static struct skdef skdef[MAXSKDEF];
static int nskdef = 0;
// config: while blessed, bless_gather percent is added to the gather chance and bless_craft percent taken off fail
// chances, plus *_strength percent of the spell's strength on top (0 = flat bonus only)
static int bless_gather = 10, bless_craft = 10, bless_gather_strength = 0, bless_craft_strength = 0;
// config (step 15): body_control_gather tenths of a percent per Body Control level are added to the
// gather chance, clarity_luck tenths of a percent per Clarity level is the chance of one extra resource
static int body_control_gather = 2, clarity_luck = 2;
// config: Magic XP per successful cast. Fire and Lightning default to 0: they train Magic only through the
// damage they deal (dmg_xp of MAGIC in skills.cfg)
static int spell_xp_heal = 8, spell_xp_bless = 8, spell_xp_freeze = 8, spell_xp_magicshield = 8;
static int spell_xp_fire = 0, spell_xp_lightning = 0;
// config: XP per step by speed mode. walk_xp is the base per step, the others are percent of it per skill
static int walk_xp = 2, fast_athletics = 50, fast_perception = 50, stealth_stealth = 100, stealth_athletics = 25;
// config: no Stealth XP for steps within stealth_combat_pause seconds of a melee attack (made or taken), so
// fighting in stealth mode (which trains Defence) does not train Stealth
static int stealth_combat_pause = 5;
static int *last_fight = NULL, last_fight_size = 0; // tick of each character's last melee attack, by cn
static int type_skill[SKT_MAX + 1]; // skill of the recipes of a type (smelt, smith, craft), for headers and the window

// Magic XP for one successful cast of the spell in value slot v, -1 if v is not a spell (use the skill's use_xp)
int skilling_spell_xp(int v) {
    switch (v) {
    case V_HEAL: return spell_xp_heal;
    case V_BLESS: return spell_xp_bless;
    case V_FREEZE: return spell_xp_freeze;
    case V_MAGICSHIELD: return spell_xp_magicshield;
    case V_FIRE: return spell_xp_fire;
    case V_FLASH: return spell_xp_lightning;
    default: return -1;
    }
}

// pct percent of walk_xp to the skill in value slot v; a fraction of a point is given by chance (half a
// point = one point every second step on average, also after the XP rates)
static void walk_xp_to(int cn, int v, int pct) {
    add_skill_xp_milli(cn, skill_for_value(v), (long long)walk_xp * pct * 10);
}

// remember that cn just made or took a melee attack (players only)
void skilling_fight(int cn) {
    int n;

    if (cn < 1 || cn >= MAXCHARS || !(ch[cn].flags & CF_PLAYER)) return;
    if (!last_fight) { // MAXCHARS is fixed once the server runs
        n = MAXCHARS;
        if (!(last_fight = xcalloc(n * sizeof(int), IM_BASE))) return;
        last_fight_size = n;
    }
    if (cn < last_fight_size) last_fight[cn] = ticker;
}

static int in_fight(int cn) {
    return cn < last_fight_size && last_fight[cn] && ticker - last_fight[cn] < stealth_combat_pause * TICKS;
}

// XP for one step of a player: fast mode trains Athletics and Perception, stealth mode Stealth and Athletics
// (no Stealth XP while fighting)
void skilling_walk_xp(int cn) {
    if (!(ch[cn].flags & CF_PLAYER)) return;
    if (ch[cn].speed_mode == SM_FAST) {
        walk_xp_to(cn, V_SPEEDSKILL, fast_athletics);
        walk_xp_to(cn, V_PERCEPT, fast_perception);
    } else if (ch[cn].speed_mode == SM_STEALTH) {
        if (!in_fight(cn)) walk_xp_to(cn, V_STEALTH, stealth_stealth);
        walk_xp_to(cn, V_SPEEDSKILL, stealth_athletics);
    }
}

static int is_node(int type) {
    return type == SKT_ROCK || type == SKT_TREE || type == SKT_PLANT;
}

static int is_tool(int type) {
    return type == SKT_PICKAXE || type == SKT_AXE || type == SKT_HAMMER || type == SKT_KNIFE;
}

static int is_station(int type) {
    return type == SKT_FURNACE || type == SKT_ANVIL || type == SKT_BENCH || type == SKT_SUPPLY;
}

static int is_recipe(int type) {
    return type == SKT_SMELT || type == SKT_SMITH || type == SKT_CRAFT;
}

// defaults: which skill and tool a gathering spot or recipe type uses
static char *default_skill(int type) {
    switch (type) {
    case SKT_ROCK: return "MINING";
    case SKT_TREE: return "WOODCUTTING";
    case SKT_SMELT:
    case SKT_SMITH: return "SMITHING";
    case SKT_CRAFT: return "CRAFTING";
    default: return "none";
    }
}

static int default_tool(int type) {
    switch (type) {
    case SKT_ROCK: return SKT_PICKAXE;
    case SKT_TREE: return SKT_AXE;
    case SKT_SMITH: return SKT_HAMMER;
    case SKT_CRAFT: return SKT_KNIFE;
    default: return 0;
    }
}

static char *tool_text(int tool) { // "You need ..."
    switch (tool) {
    case SKT_PICKAXE: return "a pickaxe";
    case SKT_AXE: return "an axe";
    case SKT_HAMMER: return "a hammer";
    case SKT_KNIFE: return "a knife";
    default: return "a tool";
    }
}

static char *node_verb(int type) {
    switch (type) {
    case SKT_ROCK: return "mine";
    case SKT_TREE: return "cut";
    default: return "pick";
    }
}

static char *node_empty_text(int type) {
    switch (type) {
    case SKT_ROCK: return "There is no ore left in this rock.";
    case SKT_TREE: return "This tree has been cut down. It will grow back.";
    default: return "There is nothing left to pick here.";
    }
}

static char *station_text(int type) { // "You need to stand next to ..."
    switch (type) {
    case SKT_FURNACE: return "a furnace";
    case SKT_ANVIL: return "an anvil";
    default: return "a workbench";
    }
}

static char *station_name(int type) {
    switch (type) {
    case SKT_FURNACE: return "furnace";
    case SKT_ANVIL: return "anvil";
    default: return "workbench";
    }
}

static char *recipe_cmd(int type) {
    switch (type) {
    case SKT_SMELT: return "smelt";
    case SKT_SMITH: return "smith";
    default: return "craft";
    }
}

static char *skill_name(int sk) {
    if (sk < 0 || sk >= MAXSKILLS || !skilldef[sk].used) return "no";
    return skilldef[sk].name;
}

// ---------- config ----------

static int lookup_def(char *key) {
    int n;

    for (n = 0; n < nskdef; n++)
        if (!strcasecmp(skdef[n].key, key)) return n;
    return -1;
}

static int def_by_item(int in) {
    int n, id;

    if ((it[in].ID >> 24) != DEV_ID_SKILLING) return -1;
    id = it[in].ID & 0xFFFFFF;
    for (n = 0; n < nskdef; n++)
        if (skdef[n].tmp && skdef[n].id == id) return n;
    return -1;
}

static int is_item_type(int type) {
    return type == SKT_RESOURCE || is_node(type) || is_tool(type) || is_station(type);
}

static void cfgcopy(char *dst, char *src, int size) {
    int n;

    for (n = 0; n < size - 1 && src[n]; n++) dst[n] = src[n];
    dst[n] = 0;
}

static int parse_cfg(char *text) {
    char *ptr = text, name[64], value[256], *comma;
    struct skdef *d = NULL;
    int line = 1, n;

    while ((ptr = skillcfg_line(ptr, name, value, &line))) {
        if (name[0] == '#') continue;

        if (value[0] == '!') {
            elog("%s line %d: syntax error near \"%s\".", SKILLING_CFG, line, name);
            return 0;
        }
        if (value[0] == ':' && !strcasecmp(name, "config")) { // config: settings ; (not an item block)
            while ((ptr = skillcfg_line(ptr, name, value, &line))) {
                if (name[0] == '#') continue;
                if (value[0] == ';') break;
                if (!strcasecmp(name, "bless_gather")) bless_gather = max(0, atoi(value));
                else if (!strcasecmp(name, "bless_craft")) bless_craft = max(0, atoi(value));
                else if (!strcasecmp(name, "bless_gather_strength")) bless_gather_strength = max(0, atoi(value));
                else if (!strcasecmp(name, "bless_craft_strength")) bless_craft_strength = max(0, atoi(value));
                else if (!strcasecmp(name, "tactics_boost")) combat_tactics_boost = max(0, atoi(value));
                else if (!strcasecmp(name, "min_hit")) combat_min_hit = max(0, min(100, atoi(value)));
                else if (!strcasecmp(name, "armor_soak")) combat_armor_soak = max(0, min(100, atoi(value)));
                else if (!strcasecmp(name, "magic_damage_cap")) combat_magic_damage_cap = max(0, atoi(value));
                else if (!strcasecmp(name, "body_control")) combat_body_control = max(0, atoi(value));
                else if (!strcasecmp(name, "clarity")) combat_clarity = max(0, atoi(value));
                else if (!strcasecmp(name, "armor_class_percent")) combat_armor_class_percent = max(0, atoi(value));
                else if (!strcasecmp(name, "weapon_skill_percent")) combat_weapon_skill_percent = max(0, atoi(value));
                else if (!strcasecmp(name, "perception_stealth")) perception_stealth = max(0, atoi(value));
                else if (!strcasecmp(name, "perception_fast")) perception_fast = max(0, min(100, atoi(value)));
                else if (!strcasecmp(name, "stealth_endurance")) stealth_endurance = max(0, atoi(value));
                else if (!strcasecmp(name, "agility_bonus")) attr_agility = max(0, atoi(value));
                else if (!strcasecmp(name, "agility_speed")) attr_agility_speed = max(0, atoi(value));
                else if (!strcasecmp(name, "intuition_bonus")) attr_intuition = max(0, atoi(value));
                else if (!strcasecmp(name, "wisdom_bonus")) attr_wisdom = max(0, atoi(value));
                else if (!strcasecmp(name, "athletics_speed")) athletics_speed = max(0, atoi(value));
                else if (!strcasecmp(name, "immunity_defence")) immunity_defence = max(0, atoi(value));
                else if (!strcasecmp(name, "body_control_gather")) body_control_gather = max(0, atoi(value));
                else if (!strcasecmp(name, "clarity_luck")) clarity_luck = max(0, atoi(value));
                else if (!strcasecmp(name, "spell_xp_heal")) spell_xp_heal = max(0, atoi(value));
                else if (!strcasecmp(name, "spell_xp_bless")) spell_xp_bless = max(0, atoi(value));
                else if (!strcasecmp(name, "spell_xp_freeze")) spell_xp_freeze = max(0, atoi(value));
                else if (!strcasecmp(name, "spell_xp_magicshield")) spell_xp_magicshield = max(0, atoi(value));
                else if (!strcasecmp(name, "spell_xp_fire")) spell_xp_fire = max(0, atoi(value));
                else if (!strcasecmp(name, "spell_xp_lightning")) spell_xp_lightning = max(0, atoi(value));
                else if (!strcasecmp(name, "walk_xp")) walk_xp = max(0, atoi(value));
                else if (!strcasecmp(name, "fast_athletics")) fast_athletics = max(0, atoi(value));
                else if (!strcasecmp(name, "fast_perception")) fast_perception = max(0, atoi(value));
                else if (!strcasecmp(name, "stealth_stealth")) stealth_stealth = max(0, atoi(value));
                else if (!strcasecmp(name, "stealth_athletics")) stealth_athletics = max(0, atoi(value));
                else if (!strcasecmp(name, "stealth_combat_pause")) stealth_combat_pause = max(0, atoi(value));
                else {
                    elog("%s line %d: unknown config setting %s.", SKILLING_CFG, line, name);
                    return 0;
                }
            }
            if (!ptr) {
                elog("%s: missing ; after config:", SKILLING_CFG);
                return 0;
            }
            continue;
        }
        if (value[0] == ':') {
            if (d) {
                elog("%s line %d: missing ; before %s:", SKILLING_CFG, line, name);
                return 0;
            }
            if (nskdef >= MAXSKDEF) {
                elog("%s line %d: more than %d blocks.", SKILLING_CFG, line, MAXSKDEF);
                return 0;
            }
            if (lookup_def(name) != -1) {
                elog("%s line %d: %s is defined twice.", SKILLING_CFG, line, name);
                return 0;
            }
            d = skdef + nskdef;
            bzero(d, sizeof(*d));
            cfgcopy(d->key, name, sizeof(d->key));
            d->ore = d->make_res = -1;
            d->power = 1;
            d->ores = 1;
            d->respawn = 30;
            d->amount = 1;
            d->chance = 35;
            d->group = -1;
            continue;
        }
        if (value[0] == ';') {
            if (!d) {
                elog("%s line %d: ; without block.", SKILLING_CFG, line);
                return 0;
            }
            if (!d->type) {
                elog("%s line %d: %s has no type=.", SKILLING_CFG, line, d->key);
                return 0;
            }
            if (is_item_type(d->type) && (d->id < 1 || d->id > 0xFFFFFF || !d->item[0])) {
                elog("%s line %d: %s needs id= (1 to 16777215) and item=.", SKILLING_CFG, line, d->key);
                return 0;
            }
            nskdef++;
            d = NULL;
            continue;
        }
        if (!d) {
            elog("%s line %d: %s outside of a block.", SKILLING_CFG, line, name);
            return 0;
        }

        if (!strcasecmp(name, "type")) {
            for (n = 1; n <= SKT_MAX; n++)
                if (!strcasecmp(value, type_name[n])) break;
            if (n > SKT_MAX) {
                elog("%s line %d: unknown type %s.", SKILLING_CFG, line, value);
                return 0;
            }
            d->type = n;
        } else if (!strcasecmp(name, "id")) d->id = atoi(value);
        else if (!strcasecmp(name, "item")) cfgcopy(d->item, value, sizeof(d->item));
        else if (!strcasecmp(name, "level")) d->level = atoi(value);
        else if (!strcasecmp(name, "xp")) d->xp = max(0, atoi(value));
        else if (!strcasecmp(name, "power")) d->power = max(1, atoi(value));
        else if (!strcasecmp(name, "ores") || !strcasecmp(name, "count")) d->ores = max(1, atoi(value));
        else if (!strcasecmp(name, "respawn")) d->respawn = max(1, atoi(value));
        else if (!strcasecmp(name, "empty_sprite")) d->empty_sprite = atoi(value);
        else if (!strcasecmp(name, "stack_sprites")) { // stack_sprites=1,2-9,10-99,100+ (later ones optional)
            char *v = value;
            for (n = 0; n < 4 && v && *v; n++) {
                d->stack_sprite[n] = atoi(v);
                if ((v = strchr(v, ','))) v++;
            }
        }
        else if (!strcasecmp(name, "ore") || !strcasecmp(name, "gives")) cfgcopy(d->ore_key, value, sizeof(d->ore_key));
        else if (!strcasecmp(name, "skill")) cfgcopy(d->skill_key, value, sizeof(d->skill_key));
        else if (!strcasecmp(name, "tool")) cfgcopy(d->tool_key, value, sizeof(d->tool_key));
        else if (!strcasecmp(name, "amount")) d->amount = max(1, min(10000, atoi(value)));
        else if (!strcasecmp(name, "chance")) d->chance = max(1, min(100, atoi(value)));
        else if (!strcasecmp(name, "group")) cfgcopy(d->group_key, value, sizeof(d->group_key));
        else if (!strcasecmp(name, "make")) cfgcopy(d->make_key, value, sizeof(d->make_key));
        else if (!strcasecmp(name, "name")) cfgcopy(d->name, value, sizeof(d->name));
        else if (!strcasecmp(name, "description")) cfgcopy(d->desc, value, sizeof(d->desc));
        else if (!strcasecmp(name, "fail")) d->fail = max(0, min(100, atoi(value)));
        else if (!strcasecmp(name, "need") || !strcasecmp(name, "give")) { // need=KEY,count  give=KEY (supply)
            if (d->nneed >= MAXNEED) {
                elog("%s line %d: at most %d need= per recipe.", SKILLING_CFG, line, MAXNEED);
                return 0;
            }
            if ((comma = strchr(value, ','))) *comma = 0;
            cfgcopy(d->need_key[d->nneed], value, sizeof(d->need_key[0]));
            d->needcnt[d->nneed] = comma ? max(1, atoi(comma + 1)) : 1;
            d->nneed++;
        } else {
            elog("%s line %d: unknown setting %s.", SKILLING_CFG, line, name);
            return 0;
        }
    }
    if (d) {
        elog("%s: missing ; at end of file.", SKILLING_CFG);
        return 0;
    }
    return 1;
}

// links keys and templates, stamps driver and ID into the item templates
static int resolve_defs(void) {
    struct skdef *d;
    struct item *ti;
    int n, m, k;

    for (n = 0; n < nskdef; n++) {
        d = skdef + n;

        if (is_item_type(d->type)) {
            for (m = 0; m < n; m++) {
                if (is_item_type(skdef[m].type) && skdef[m].id == d->id) {
                    elog("%s: %s and %s have the same id %d.", SKILLING_CFG, skdef[m].key, d->key, d->id);
                    return 0;
                }
                if (is_item_type(skdef[m].type) && !strcmp(skdef[m].item, d->item)) {
                    elog("%s: %s and %s use the same item %s.", SKILLING_CFG, skdef[m].key, d->key, d->item);
                    return 0;
                }
            }
            if (!(d->tmp = lookup_item(d->item))) {
                elog("%s: %s: unknown item template %s.", SKILLING_CFG, d->key, d->item);
                return 0;
            }
            ti = item_template(d->tmp);
            ti->ID = MAKE_ITEMID(DEV_ID_SKILLING, d->id);
            switch (d->type) {
            case SKT_RESOURCE:
                ti->driver = IDR_SKSTACK;
                ti->flags |= IF_TAKE | IF_USE;
                *(unsigned int *)(ti->drdata) = 1;
                sprintf(ti->description, "1 %.70s.", ti->name);
                break;
            case SKT_ROCK:
            case SKT_TREE:
            case SKT_PLANT:
                ti->driver = IDR_SKROCK;
                ti->flags |= IF_USE;
                ti->flags &= ~IF_TAKE;
                if (!d->empty_sprite) d->empty_sprite = ti->sprite;
                break;
            case SKT_FURNACE:
            case SKT_ANVIL:
            case SKT_BENCH:
            case SKT_SUPPLY:
                ti->driver = IDR_SKSTATION;
                ti->flags |= IF_USE;
                ti->flags &= ~IF_TAKE;
                break;
            }
        }

        if (is_node(d->type) || is_recipe(d->type)) { // skill= and tool=
            char *key = d->skill_key[0] ? d->skill_key : default_skill(d->type);

            if (!strcasecmp(key, "none")) d->skill = -1;
            else if ((d->skill = skill_lookup_key(key)) == -1) {
                if (d->skill_key[0]) {
                    elog("%s: %s: skill=%s is not in skills.cfg.", SKILLING_CFG, d->key, d->skill_key);
                    return 0;
                }
                elog("%s: %s: skills.cfg has no %s skill, it gives no XP and has no level limit.", SKILLING_CFG, d->key, key);
            }
            if (!d->tool_key[0]) d->tool = default_tool(d->type);
            else if (!strcasecmp(d->tool_key, "none")) d->tool = 0;
            else {
                for (k = 1; k <= SKT_MAX; k++)
                    if (is_tool(k) && !strcasecmp(d->tool_key, type_name[k])) break;
                if (k > SKT_MAX) {
                    elog("%s: %s: tool=%s is not pickaxe, axe, hammer, knife or none.", SKILLING_CFG, d->key, d->tool_key);
                    return 0;
                }
                d->tool = k;
            }
        }

        if (is_node(d->type)) {
            if ((d->ore = lookup_def(d->ore_key)) == -1 || skdef[d->ore].type != SKT_RESOURCE) {
                elog("%s: %s %s: ore=%s is not a resource.", SKILLING_CFG, type_name[d->type], d->key, d->ore_key);
                return 0;
            }
        }

        if (d->type == SKT_SUPPLY) {
            for (m = 0; m < d->nneed; m++) {
                if ((k = lookup_def(d->need_key[m])) == -1 || (!is_tool(skdef[k].type) && skdef[k].type != SKT_RESOURCE)) {
                    elog("%s: supply %s: give=%s is not a tool or resource.", SKILLING_CFG, d->key, d->need_key[m]);
                    return 0;
                }
                d->need[m] = k;
            }
        }

        if (is_recipe(d->type)) {
            if ((k = lookup_def(d->make_key)) != -1) {
                if (skdef[k].type != SKT_RESOURCE) {
                    elog("%s: recipe %s: make=%s is not a resource.", SKILLING_CFG, d->key, d->make_key);
                    return 0;
                }
                d->make_res = k;
            } else if (!(d->make_tmp = lookup_item(d->make_key))) {
                elog("%s: recipe %s: make=%s is neither a resource nor an item template.", SKILLING_CFG, d->key, d->make_key);
                return 0;
            }
            if (!d->nneed) {
                elog("%s: recipe %s needs at least one need=.", SKILLING_CFG, d->key);
                return 0;
            }
            for (m = 0; m < d->nneed; m++) {
                if ((k = lookup_def(d->need_key[m])) == -1 || skdef[k].type != SKT_RESOURCE) {
                    elog("%s: recipe %s: need=%s is not a resource.", SKILLING_CFG, d->key, d->need_key[m]);
                    return 0;
                }
                d->need[m] = k;
            }
            if (d->group_key[0] && ((d->group = lookup_def(d->group_key)) == -1 || skdef[d->group].type != SKT_RESOURCE)) {
                elog("%s: recipe %s: group=%s is not a resource.", SKILLING_CFG, d->key, d->group_key);
                return 0;
            }
            if (!d->name[0]) {
                if (d->make_res != -1) cfgcopy(d->name, item_template(skdef[d->make_res].tmp)->name, sizeof(d->name));
                else cfgcopy(d->name, item_template(d->make_tmp)->name, sizeof(d->name));
            }
        }
    }
    return 1;
}

int skilling_load(void) {
    char *text;
    int ok, n, cnt[SKT_MAX + 1];

    nskdef = 0;
    bless_gather = bless_craft = 10;
    bless_gather_strength = bless_craft_strength = 0;
    body_control_gather = clarity_luck = 2;
    spell_xp_heal = spell_xp_bless = spell_xp_freeze = spell_xp_magicshield = 8;
    spell_xp_fire = spell_xp_lightning = 0;
    walk_xp = 2;
    fast_athletics = fast_perception = 50;
    stealth_stealth = 100;
    stealth_athletics = 25;
    stealth_combat_pause = 5;
    combat_settings_reset();
    for (n = 0; n <= SKT_MAX; n++) type_skill[n] = is_recipe(n) ? skill_lookup_key(default_skill(n)) : -1;

    if (!(text = skillcfg_read(SKILLING_CFG))) {
        elog("Could not read %s. Gathering and production skills are off.", SKILLING_CFG);
        return 1;
    }
    ok = parse_cfg(text);
    xfree(text);
    if (!ok || !resolve_defs()) {
        nskdef = 0;
        return 0;
    }

    bzero(cnt, sizeof(cnt));
    for (n = 0; n < nskdef; n++) cnt[skdef[n].type]++;
    xlog("Loaded %s: %d resources, %d rocks, %d trees, %d plants, %d tools, %d stations, %d recipes.", SKILLING_CFG,
         cnt[SKT_RESOURCE], cnt[SKT_ROCK], cnt[SKT_TREE], cnt[SKT_PLANT], cnt[SKT_PICKAXE] + cnt[SKT_AXE] + cnt[SKT_HAMMER] + cnt[SKT_KNIFE],
         cnt[SKT_FURNACE] + cnt[SKT_ANVIL] + cnt[SKT_BENCH] + cnt[SKT_SUPPLY], cnt[SKT_SMELT] + cnt[SKT_SMITH] + cnt[SKT_CRAFT]);
    return 1;
}

static int skill_level(int cn, int sk) {
    if (sk == -1) return 1;
    return skill_get_level(cn, sk);
}

// strength of the Bless spell on the character, 0 = none
static int bless_strength(int cn) {
    int n, in;

    for (n = 12; n < 30; n++)
        if ((in = ch[cn].item[n]) && it[in].driver == IDR_BLESS) return *(int *)(it[in].drdata + 8);
    return 0;
}

// gather chance bonus / fail chance reduction from Bless, in percent
static int bless_bonus(int cn, int flat, int per_strength) {
    int str = bless_strength(cn);

    return str ? flat + str * per_strength / 100 : 0;
}

static void give_xp(int cn, int sk, int xp) {
    if (sk != -1 && xp > 0) add_skill_xp(cn, sk, xp);
}

// ---------- resource stacks ----------

static unsigned int stack_count(int in) {
    return *(unsigned int *)(it[in].drdata);
}

// sprite tier of a stack: 1, 2-9, 10-99, 100+
static int stack_tier(unsigned int cnt) {
    if (cnt >= 100) return 3;
    if (cnt >= 10) return 2;
    if (cnt >= 2) return 1;
    return 0;
}

static void stack_set(int in, unsigned int cnt) {
    int tmp, d, t;

    *(unsigned int *)(it[in].drdata) = cnt;
    if ((d = def_by_item(in)) != -1 && (tmp = skdef[d].tmp)) {
        it[in].value = item_template(tmp)->value * cnt;
        // bigger stacks look bigger: the highest tier with a sprite at or below this count
        it[in].sprite = item_template(tmp)->sprite;
        for (t = stack_tier(cnt); t >= 0; t--) {
            if (skdef[d].stack_sprite[t]) {
                it[in].sprite = skdef[d].stack_sprite[t];
                break;
            }
        }
    }
    sprintf(it[in].description, "%s %.60s.", bignumber(cnt), it[in].name);
    it[in].flags |= IF_FORCEUPDATE;
}

static int is_res(int in, int d) {
    return in && it[in].ID == MAKE_ITEMID(DEV_ID_SKILLING, skdef[d].id);
}

static int res_count(int cn, int d) {
    int n, cnt = 0;

    for (n = 30; n < INVENTORYSIZE; n++)
        if (is_res(ch[cn].item[n], d)) cnt += stack_count(ch[cn].item[n]);
    if (is_res(ch[cn].citem, d)) cnt += stack_count(ch[cn].citem);
    return cnt;
}

static void res_take(int cn, int d, int cnt) {
    int n, in, have;

    for (n = 29; n < INVENTORYSIZE && cnt > 0; n++) { // 29 = the item in hand, then the inventory
        in = (n == 29) ? ch[cn].citem : ch[cn].item[n];
        if (!is_res(in, d)) continue;
        have = stack_count(in);
        if (have > cnt) {
            stack_set(in, have - cnt);
            cnt = 0;
        } else {
            cnt -= have;
            remove_item_char(in);
            destroy_item(in);
        }
    }
    ch[cn].flags |= CF_ITEMS;
}

// is there room for the result of one resource?
static int res_room(int cn, int d) {
    int n;

    for (n = 30; n < INVENTORYSIZE; n++)
        if (!ch[cn].item[n] || is_res(ch[cn].item[n], d)) return 1;
    return 0;
}

static int free_slot(int cn) {
    int n;

    for (n = 30; n < INVENTORYSIZE; n++)
        if (!ch[cn].item[n]) return 1;
    return 0;
}

static int res_give(int cn, int d, int cnt) {
    int n, in;

    for (n = 30; n < INVENTORYSIZE; n++) {
        if (is_res(ch[cn].item[n], d)) {
            stack_set(ch[cn].item[n], stack_count(ch[cn].item[n]) + cnt);
            ch[cn].flags |= CF_ITEMS;
            return 1;
        }
    }
    if (!(in = create_item_nr(skdef[d].tmp))) return 0;
    stack_set(in, cnt);
    if (!store_item(cn, in)) {
        destroy_item(in);
        return 0;
    }
    ch[cn].flags |= CF_ITEMS;
    return 1;
}

static void stack_driver(int in, int cn) {
    int in2, d;
    unsigned int a, b;

    if (!cn) { // created: make sure the count and description are set
        stack_set(in, stack_count(in) ? stack_count(in) : 1); // also picks the stack sprite
        return;
    }
    if (!it[in].carried) return;

    if (!(in2 = ch[cn].citem)) { // split off half
        a = stack_count(in);
        if (a < 2) {
            log_char(cn, LOG_SYSTEM, 0, "You cannot split 1 %s.", it[in].name);
            return;
        }
        b = a / 2;
        if ((d = def_by_item(in)) == -1 || !(in2 = create_item_nr(skdef[d].tmp))) {
            log_char(cn, LOG_SYSTEM, 0, "Bug: cannot split %s.", it[in].name);
            return;
        }
        stack_set(in, a - b);
        stack_set(in2, b);
        ch[cn].citem = in2;
        it[in2].carried = cn;
        ch[cn].flags |= CF_ITEMS;
        log_char(cn, LOG_SYSTEM, 0, "Split into %u and %u %s.", a - b, b, it[in].name);
        return;
    }
    if (in2 == in) return;
    if (it[in2].ID != it[in].ID) {
        log_char(cn, LOG_SYSTEM, 0, "You cannot mix those.");
        return;
    }
    stack_set(in, stack_count(in) + stack_count(in2));
    remove_item_char(in2);
    destroy_item(in2);
    ch[cn].flags |= CF_ITEMS;
    log_char(cn, LOG_SYSTEM, 0, "%s", it[in].description);
}

// ---------- gathering: rocks, trees, plants ----------

#define ROCK_COUNT(in) (*(unsigned int *)(it[in].drdata))
#define ROCK_STATE(in) (it[in].drdata[4]) // 0 = new, 1 = has ore, 2 = empty

// best tool of this type the character carries and may use, -1 = none; *need = lowest level of the ones carried
static int best_tool(int cn, int type, int lvl, int *need) {
    int n, in, d, best = -1;

    *need = 0;
    for (n = 0; n <= INVENTORYSIZE; n++) {
        if (n >= 12 && n < 30) continue; // spells
        in = (n == INVENTORYSIZE) ? ch[cn].citem : ch[cn].item[n];
        if (!in || (d = def_by_item(in)) == -1 || skdef[d].type != type) continue;
        if (skdef[d].level > lvl) {
            if (!*need || skdef[d].level < *need) *need = skdef[d].level;
            continue;
        }
        if (best == -1 || skdef[d].power > skdef[best].power) best = d;
    }
    return best;
}

static void rock_refill(int in, int d) {
    ROCK_COUNT(in) = skdef[d].ores;
    ROCK_STATE(in) = 1;
    it[in].sprite = item_template(skdef[d].tmp)->sprite;
    if (it[in].x) set_sector(it[in].x, it[in].y);
}

static void rock_driver(int in, int cn) {
    int d, ore, lvl, tool, need, chance, power = 1;

    if ((d = def_by_item(in)) == -1) return;
    ore = skdef[d].ore;

    if (!cn) { // timer: first call after creation, or respawn
        if (ROCK_STATE(in) != 1) rock_refill(in, d);
        return;
    }

    if (ROCK_STATE(in) != 1) {
        log_char(cn, LOG_SYSTEM, 0, "%s", node_empty_text(skdef[d].type));
        player_driver_dig_off(cn);
        return;
    }
    if (ch[cn].citem) {
        log_char(cn, LOG_SYSTEM, 0, "Please empty your hand (mouse cursor) first.");
        player_driver_dig_off(cn);
        return;
    }
    lvl = skill_level(cn, skdef[d].skill);
    if (lvl < skdef[d].level) {
        log_char(cn, LOG_SYSTEM, 0, "You need %s level %d to %s %s.", skill_name(skdef[d].skill), skdef[d].level, node_verb(skdef[d].type), it[in].name);
        player_driver_dig_off(cn);
        return;
    }
    if (skdef[d].tool) {
        if ((tool = best_tool(cn, skdef[d].tool, lvl, &need)) == -1) {
            if (need) log_char(cn, LOG_SYSTEM, 0, "You need %s level %d to use your %s.", skill_name(skdef[d].skill), need, type_name[skdef[d].tool]);
            else log_char(cn, LOG_SYSTEM, 0, "You need %s to %s this.", tool_text(skdef[d].tool), node_verb(skdef[d].type));
            player_driver_dig_off(cn);
            return;
        }
        power = skdef[tool].power;
    }
    if (!res_room(cn, ore)) {
        log_char(cn, LOG_SYSTEM, 0, "Your inventory is full.");
        player_driver_dig_off(cn);
        return;
    }

    // in tenths of a percent so Body Control can add 0.2% per level
    chance = (skdef[d].chance + 2 * (lvl - skdef[d].level) + 10 * (power - 1) + bless_bonus(cn, bless_gather, bless_gather_strength)) * 10;
    chance += ch[cn].value[0][V_BODYCONTROL] * body_control_gather;
    chance = max(50, min(950, chance));
    if (RANDOM(1000) >= chance) { // no luck this swing, keep going
        player_driver_dig_on(cn);
        return;
    }

    if (!res_give(cn, ore, 1)) {
        log_char(cn, LOG_SYSTEM, 0, "Your inventory is full.");
        player_driver_dig_off(cn);
        return;
    }
    log_char(cn, LOG_SYSTEM, 0, "You %s some %s.", node_verb(skdef[d].type), item_template(skdef[ore].tmp)->name);
    give_xp(cn, skdef[d].skill, skdef[d].xp);

    // Clarity: a chance of one extra resource, without XP and without using up the node
    if (RANDOM(1000) < ch[cn].value[0][V_CLARITY] * clarity_luck && res_room(cn, ore) && res_give(cn, ore, 1))
        log_char(cn, LOG_SYSTEM, 0, "Your clarity lets you %s an extra %s.", node_verb(skdef[d].type), item_template(skdef[ore].tmp)->name);

    if (ROCK_COUNT(in) > 1) {
        ROCK_COUNT(in)--;
        player_driver_dig_on(cn);
        return;
    }

    ROCK_COUNT(in) = 0;
    ROCK_STATE(in) = 2;
    it[in].sprite = skdef[d].empty_sprite;
    set_sector(it[in].x, it[in].y);
    call_item(it[in].driver, in, 0, ticker + skdef[d].respawn * TICKS);
    player_driver_dig_off(cn);
}

// ---------- smelting, smithing and crafting ----------

static int station_type(int rtype) {
    switch (rtype) {
    case SKT_SMELT: return SKT_FURNACE;
    case SKT_SMITH: return SKT_ANVIL;
    default: return SKT_BENCH;
    }
}

static int recipe_type(int station) {
    switch (station) {
    case SKT_FURNACE: return SKT_SMELT;
    case SKT_ANVIL: return SKT_SMITH;
    default: return SKT_CRAFT;
    }
}

static char *recipe_title(int rtype) {
    switch (rtype) {
    case SKT_SMELT: return "Smelting";
    case SKT_SMITH: return "Smithing";
    default: return "Crafting";
    }
}

static int has_tool(int cn, int tool) {
    int need;

    return !tool || best_tool(cn, tool, 999, &need) != -1;
}

// how many the character could make right now from what they carry
static int can_make(int cn, int r) {
    int m, best = MAXMAKE;

    for (m = 0; m < skdef[r].nneed; m++) best = min(best, res_count(cn, skdef[r].need[m]) / skdef[r].needcnt[m]);
    return best;
}

static void need_text(int r, char *buf) {
    int m;

    buf[0] = 0;
    for (m = 0; m < skdef[r].nneed; m++)
        sprintf(buf + strlen(buf), "%s%d %s", m ? " + " : "", skdef[r].needcnt[m], item_template(skdef[skdef[r].need[m]].tmp)->name);
}

static void list_recipes(int cn, int rtype) {
    int r;
    char buf[256], amount[16];

    log_char(cn, LOG_SYSTEM, 0, "\260c3%s (your %s level is %d):", recipe_title(rtype), skill_name(type_skill[rtype]), skill_level(cn, type_skill[rtype]));
    for (r = 0; r < nskdef; r++) {
        if (skdef[r].type != rtype) continue;
        need_text(r, buf);
        if (skdef[r].amount > 1) sprintf(amount, " x%d", skdef[r].amount);
        else amount[0] = 0;
        log_char(cn, LOG_SYSTEM, 0, "%s%s%s (level %d, %d XP): %s", skill_level(cn, skdef[r].skill) < skdef[r].level ? "\260c1" : "",
                 skdef[r].name, amount, skdef[r].level, skdef[r].xp, buf);
    }
    log_char(cn, LOG_SYSTEM, 0, "Use /%s <name> [count|all], or click the %s holding the material.", recipe_cmd(rtype), station_name(station_type(rtype)));
}

// makes up to cnt of recipe r; returns how many were made
static int make_recipe(int cn, int r, int cnt) {
    struct skdef *d = skdef + r;
    int lvl, done = 0, failed = 0, m, in, fail, xp = 0;
    char buf[256];

    lvl = skill_level(cn, d->skill);
    if (lvl < d->level) {
        log_char(cn, LOG_SYSTEM, 0, "You need %s level %d to make %s.", skill_name(d->skill), d->level, d->name);
        return 0;
    }
    if (!has_tool(cn, d->tool)) {
        log_char(cn, LOG_SYSTEM, 0, "You need %s to make %s.", tool_text(d->tool), d->name);
        return 0;
    }
    if (can_make(cn, r) < 1) {
        need_text(r, buf);
        log_char(cn, LOG_SYSTEM, 0, "You need %s to make %s.", buf, d->name);
        return 0;
    }
    fail = d->fail ? max(0, d->fail - 2 * (lvl - d->level) - bless_bonus(cn, bless_craft, bless_craft_strength)) : 0;

    while (done + failed < cnt && can_make(cn, r) > 0) {
        if (d->make_res != -1 ? !res_room(cn, d->make_res) : !free_slot(cn)) {
            log_char(cn, LOG_SYSTEM, 0, "Your inventory is full.");
            break;
        }
        for (m = 0; m < d->nneed; m++) res_take(cn, d->need[m], d->needcnt[m]);

        if (fail && RANDOM(100) < fail) {
            failed++;
            continue;
        }
        if (d->make_res != -1) {
            if (!res_give(cn, d->make_res, d->amount)) break; // checked above, cannot happen
        } else {
            if (!(in = create_item_nr(d->make_tmp))) break;
            if (d->name[0]) cfgcopy(it[in].name, d->name, sizeof(it[in].name));
            if (d->desc[0]) cfgcopy(it[in].description, d->desc, sizeof(it[in].description));
            if (!store_item(cn, in)) {
                destroy_item(in);
                break;
            }
            ch[cn].flags |= CF_ITEMS;
            if (ch[cn].flags & CF_PLAYER) dlog(cn, in, "made by %s", recipe_cmd(d->type));
        }
        done++;
        xp += d->xp;
    }

    if (done) log_char(cn, LOG_SYSTEM, 0, "You make %d %s.", done * d->amount, d->name);
    if (failed) log_char(cn, LOG_SYSTEM, 0, "%d %s failed and the materials were lost.", failed, failed == 1 ? "attempt" : "attempts");
    give_xp(cn, d->skill, xp);
    return done;
}

// recipe list for the client's furnace / anvil window (protocol 6)
static int craft_str(unsigned char *buf, int p, const char *s, int maxlen) {
    int len = min((int)strlen(s), maxlen);

    buf[p++] = len;
    memcpy(buf + p, s, len);
    return p + len;
}

// can this character's client show the window for this station?
static int craft_window(int cn, int rtype) {
    int ver = get_char_version(cn);

    if (rtype == SKT_CRAFT) return ver >= CRAFT_BENCH_VERSION;
    return ver >= CRAFT_CLIENT_VERSION;
}

static void send_craft_list(int cn, int rtype, int open) {
    static unsigned char buf[16384];
    int nr, r, p, n = 0, lvl, sprite, m, nmat = 0, mat[256], tool, g, k;
    char text[256];

    if (!(nr = ch[cn].player) || !craft_window(cn, rtype)) return;

    lvl = skill_level(cn, type_skill[rtype]);
    tool = default_tool(rtype);
    buf[0] = SV_CRAFTLIST;
    buf[3] = rtype == SKT_SMELT ? 1 : rtype == SKT_SMITH ? 2 : 3;
    buf[4] = (open ? CRAFT_OPEN : 0) | (!has_tool(cn, tool) ? CRAFT_NOHAMMER : 0) | (rtype != SKT_SMELT ? CRAFT_GROUP : 0);
    buf[5] = min(lvl, 255);
    p = 7;
    for (r = 0; r < nskdef && n < 255; r++) {
        if (skdef[r].type != rtype) continue;
        if (p > (int)sizeof(buf) - 4000) break; // room for this recipe and the material list
        if (skdef[r].make_res != -1) sprite = item_template(skdef[skdef[r].make_res].tmp)->sprite;
        else sprite = item_template(skdef[r].make_tmp)->sprite;
        need_text(r, text);

        buf[p++] = r;
        buf[p++] = min(skdef[r].level, 255);
        buf[p++] = min(skill_level(cn, skdef[r].skill) < skdef[r].level && skdef[r].skill != type_skill[rtype] ? 0 : can_make(cn, r), 255);
        *(unsigned short *)(buf + p) = min(skdef[r].xp, 65535);
        p += 2;
        *(unsigned int *)(buf + p) = sprite;
        p += 4;
        if ((g = skdef[r].group) == -1 && skdef[r].nneed > 0) g = skdef[r].need[0];
        if (g != -1) {
            for (k = 0, m = 0; m < skdef[r].nneed; m++)
                if (skdef[r].need[m] == g) k = skdef[r].needcnt[m];
            buf[p++] = g;
            buf[p++] = min(k, 255);
            for (m = 0; m < nmat && mat[m] != g; m++);
            if (m == nmat) mat[nmat++] = g;
        } else {
            buf[p++] = 255;
            buf[p++] = 0;
        }
        p = craft_str(buf, p, skdef[r].key, 31);
        p = craft_str(buf, p, skdef[r].name, 39);
        p = craft_str(buf, p, text, 160);
        n++;
    }
    buf[6] = n;

    // the materials: what the client groups by, and how many the character carries
    buf[p++] = nmat;
    for (m = 0; m < nmat; m++) {
        buf[p++] = mat[m];
        *(unsigned short *)(buf + p) = min(res_count(cn, mat[m]), 65535);
        p += 2;
        p = craft_str(buf, p, item_template(skdef[mat[m]].tmp)->name, 39);
    }

    // protocol 9: the texts of the station, so the client needs no fixed list
    if (get_player_version(nr) >= CRAFT_BENCH_VERSION) {
        sprintf(text, "What would you like to %s?", rtype == SKT_SMELT ? "smelt" : "make");
        p = craft_str(buf, p, text, 80);
        p = craft_str(buf, p, recipe_cmd(rtype), 15);
        p = craft_str(buf, p, skill_name(type_skill[rtype]), 39);
        p = craft_str(buf, p, tool_text(tool), 39);
        p = craft_str(buf, p, station_name(station_type(rtype)), 39);
    }
    *(unsigned short *)(buf + 1) = p;
    psend(nr, (char *)buf, p);
}

// station clicked with a resource in hand: best recipe that uses it
static void station_use_with(int cn, int rtype, int in) {
    int r, best = -1, m, lvl, res;

    if ((res = def_by_item(in)) == -1 || skdef[res].type != SKT_RESOURCE) {
        log_char(cn, LOG_SYSTEM, 0, "You cannot use that here.");
        return;
    }
    for (r = 0; r < nskdef; r++) {
        if (skdef[r].type != rtype) continue;
        lvl = skill_level(cn, skdef[r].skill);
        if (skdef[r].level > lvl || !has_tool(cn, skdef[r].tool)) continue;
        for (m = 0; m < skdef[r].nneed; m++)
            if (skdef[r].need[m] == res) break;
        if (m == skdef[r].nneed || can_make(cn, r) < 1) continue;
        if (best == -1 || skdef[r].level > skdef[best].level) best = r;
    }
    if (best == -1) {
        log_char(cn, LOG_SYSTEM, 0, "You cannot make anything here from what you carry.");
        if (craft_window(cn, rtype)) send_craft_list(cn, rtype, 1);
        else list_recipes(cn, rtype);
        return;
    }
    make_recipe(cn, best, 1);
    send_craft_list(cn, rtype, 0);
}

// gives each listed tool the character does not carry yet, and tops resources up to their count
static void supply_driver(int cn, int d) {
    int m, n, in, t, have, given = 0;

    for (m = 0; m < skdef[d].nneed; m++) {
        t = skdef[d].need[m];
        if (skdef[t].type == SKT_RESOURCE) {
            if ((have = res_count(cn, t)) >= skdef[d].needcnt[m]) continue;
            if (!res_room(cn, t)) {
                log_char(cn, LOG_SYSTEM, 0, "Your inventory is full.");
                return;
            }
            if (!res_give(cn, t, skdef[d].needcnt[m] - have)) continue;
            log_char(cn, LOG_SYSTEM, 0, "You take %d %s.", skdef[d].needcnt[m] - have, item_template(skdef[t].tmp)->name);
            given++;
            continue;
        }
        for (have = 0, n = 0; n <= INVENTORYSIZE && !have; n++) {
            if (n >= 12 && n < 30) continue;
            in = (n == INVENTORYSIZE) ? ch[cn].citem : ch[cn].item[n];
            if (in && it[in].ID == MAKE_ITEMID(DEV_ID_SKILLING, skdef[t].id)) have = 1;
        }
        if (have) continue;
        if (!free_slot(cn)) {
            log_char(cn, LOG_SYSTEM, 0, "Your inventory is full.");
            return;
        }
        if (!(in = create_item_nr(skdef[t].tmp))) continue;
        store_item(cn, in);
        ch[cn].flags |= CF_ITEMS;
        if (ch[cn].flags & CF_PLAYER) dlog(cn, in, "took from %s", skdef[d].key);
        log_char(cn, LOG_SYSTEM, 0, "You take a %s.", it[in].name);
        given++;
    }
    if (!given) log_char(cn, LOG_SYSTEM, 0, "You already have everything from here.");
}

static void station_driver(int in, int cn) {
    int d, rtype;

    if (!cn) return;
    if ((d = def_by_item(in)) == -1) return;
    if (skdef[d].type == SKT_SUPPLY) {
        supply_driver(cn, d);
        return;
    }
    rtype = recipe_type(skdef[d].type);

    if (ch[cn].citem) station_use_with(cn, rtype, ch[cn].citem);
    else if (craft_window(cn, rtype)) send_craft_list(cn, rtype, 1);
    else list_recipes(cn, rtype);
}

int skilling_item_driver(int nr, int in, int cn) {
    switch (nr) {
    case IDR_SKROCK:
        rock_driver(in, cn);
        break;
    case IDR_SKSTACK:
        stack_driver(in, cn);
        break;
    case IDR_SKSTATION:
        station_driver(in, cn);
        break;
    default:
        return 0;
    }
    return 1;
}

// ---------- /smelt, /smith and /craft ----------

static int near_station(int cn, int type) {
    int x, y, in, d;

    for (y = max(1, ch[cn].y - STATION_RANGE); y <= min(MAXMAP - 2, ch[cn].y + STATION_RANGE); y++) {
        for (x = max(1, ch[cn].x - STATION_RANGE); x <= min(MAXMAP - 2, ch[cn].x + STATION_RANGE); x++) {
            if (!(in = map[x + y * MAXMAP].it) || it[in].driver != IDR_SKSTATION) continue;
            if ((d = def_by_item(in)) != -1 && skdef[d].type == type) return 1;
        }
    }
    return 0;
}

// lowercase, letters and digits only
static void squash(char *dst, char *src, int size) {
    int n = 0;

    for (; *src && n < size - 1; src++)
        if (isalnum(*src)) dst[n++] = tolower(*src);
    dst[n] = 0;
}

static int find_recipe(int cn, int rtype, char *what) {
    char want[80], have[80];
    int r, found = -1, many = 0;

    squash(want, what, sizeof(want));
    if (!want[0]) return -1;
    for (r = 0; r < nskdef; r++) {
        if (skdef[r].type != rtype) continue;
        squash(have, skdef[r].name, sizeof(have));
        if (!strcmp(have, want)) return r;
        squash(have, skdef[r].key, sizeof(have));
        if (!strcmp(have, want)) return r;
        squash(have, skdef[r].name, sizeof(have));
        if (strstr(have, want)) {
            if (found == -1) found = r;
            else many = 1;
        }
    }
    if (many) {
        log_char(cn, LOG_SYSTEM, 0, "\"%s\" matches more than one recipe, please be more exact.", what);
        return -2;
    }
    return found;
}

// /smelt, /smith, /craft [what] [count|all]; returns 1 if cmd was one of ours
int skilling_command(int cn, char *cmd, char *args) {
    int rtype, r, cnt = 1, len;
    char what[80], *last;

    if (!strcasecmp(cmd, "smelt")) rtype = SKT_SMELT;
    else if (!strcasecmp(cmd, "smith")) rtype = SKT_SMITH;
    else if (!strcasecmp(cmd, "craft")) rtype = SKT_CRAFT;
    else return 0;

    if (!nskdef) {
        log_char(cn, LOG_SYSTEM, 0, "%s is not set up on this server.", recipe_title(rtype));
        return 1;
    }
    if (!near_station(cn, station_type(rtype))) {
        log_char(cn, LOG_SYSTEM, 0, "You need to stand next to %s.", station_text(station_type(rtype)));
        return 1;
    }

    while (isspace(*args)) args++;
    cfgcopy(what, args, sizeof(what));
    len = strlen(what);
    while (len && isspace(what[len - 1])) what[--len] = 0;

    if ((last = strrchr(what, ' '))) { // trailing count or "all"
        if (!strcasecmp(last + 1, "all")) {
            cnt = MAXMAKE;
            *last = 0;
        } else if (isdigit(last[1])) {
            cnt = max(1, min(MAXMAKE, atoi(last + 1)));
            *last = 0;
        }
    }

    if (!what[0]) {
        list_recipes(cn, rtype);
        return 1;
    }
    if ((r = find_recipe(cn, rtype, what)) == -2) return 1;
    if (r == -1) {
        log_char(cn, LOG_SYSTEM, 0, "There is no recipe called \"%s\".", what);
        list_recipes(cn, rtype);
        return 1;
    }
    make_recipe(cn, r, cnt);
    send_craft_list(cn, rtype, 0);
    return 1;
}

// gathering lines of the effects breakdown (balance.c effects_build)
void skilling_effects(int cn, struct effect_list *e) {
    int b;
    char desc[160];

    snprintf(desc, sizeof(desc), "Extra chance to gather from rocks and trees: %.1f%% per Body Control level.", body_control_gather / 10.0);
    if (ch[cn].value[0][V_BODYCONTROL]) effect_add(e, EG_GATHERING, "Gather chance", desc, "%+.1f%%", ch[cn].value[0][V_BODYCONTROL] * body_control_gather / 10.0);
    if ((b = bless_bonus(cn, bless_gather, bless_gather_strength))) {
        snprintf(desc, sizeof(desc), "Bless adds %d%% gather chance and lowers craft failure by %d%%.", b, bless_bonus(cn, bless_craft, bless_craft_strength));
        effect_add(e, EG_GATHERING, "Bless", desc, "%+d%%", b);
    }
    snprintf(desc, sizeof(desc), "Chance to get one extra resource when gathering: %.1f%% per Clarity level.", clarity_luck / 10.0);
    if (ch[cn].value[0][V_CLARITY]) effect_add(e, EG_GATHERING, "Extra resource", desc, "%.1f%%", min(1000, ch[cn].value[0][V_CLARITY] * clarity_luck) / 10.0);
}

// ---------- things a player carries, for other modules (skillquest.c) ----------

// a skilling.cfg key naming something a player can carry: a resource (stack), a tool, or a recipe that makes
// an item (a recipe that makes a resource stands for that resource). -1 if unknown or not carryable.
int skilling_thing(char *key) {
    int d;

    if ((d = lookup_def(key)) == -1) return -1;
    if (skdef[d].type == SKT_RESOURCE || is_tool(skdef[d].type)) return d;
    if (is_recipe(skdef[d].type)) return skdef[d].make_res != -1 ? skdef[d].make_res : d;
    return -1;
}

int skilling_thing_is_stack(int d) {
    return d >= 0 && d < nskdef && skdef[d].type == SKT_RESOURCE;
}

char *skilling_thing_name(int d) {
    if (d < 0 || d >= nskdef) return "?";
    if (is_recipe(skdef[d].type)) return skdef[d].name;
    return item_template(skdef[d].tmp)->name;
}

// does item in count as thing d? Recipe items are matched by template ID and the recipe's name, so a bought
// dagger is not a smithed Iron Dagger.
static int thing_item(int in, int d) {
    if (!in) return 0;
    if (!is_recipe(skdef[d].type)) return it[in].ID == MAKE_ITEMID(DEV_ID_SKILLING, skdef[d].id);
    return it[in].ID == item_template(skdef[d].make_tmp)->ID && !strcmp(it[in].name, skdef[d].name);
}

// how many of thing d the character carries (inventory and hand, not worn)
int skilling_thing_count(int cn, int d) {
    int n, in, cnt = 0;

    if (d < 0 || d >= nskdef) return 0;
    if (skdef[d].type == SKT_RESOURCE) return res_count(cn, d);
    for (n = 30; n <= INVENTORYSIZE; n++) {
        in = (n == INVENTORYSIZE) ? ch[cn].citem : ch[cn].item[n];
        if (thing_item(in, d)) cnt++;
    }
    return cnt;
}

// takes cnt of thing d (check skilling_thing_count first)
void skilling_thing_take(int cn, int d, int cnt) {
    int n, in;

    if (d < 0 || d >= nskdef || cnt < 1) return;
    if (skdef[d].type == SKT_RESOURCE) {
        res_take(cn, d, cnt);
        return;
    }
    for (n = 30; n <= INVENTORYSIZE && cnt > 0; n++) {
        in = (n == INVENTORYSIZE) ? ch[cn].citem : ch[cn].item[n];
        if (!thing_item(in, d)) continue;
        if (ch[cn].flags & CF_PLAYER) dlog(cn, in, "handed in for a quest");
        remove_item_char(in);
        destroy_item(in);
        cnt--;
    }
    ch[cn].flags |= CF_ITEMS;
}

// is there room to give cnt of thing d?
int skilling_thing_room(int cn, int d, int cnt) {
    int n, nfree = 0;

    if (d < 0 || d >= nskdef) return 0;
    if (skdef[d].type == SKT_RESOURCE) return res_room(cn, d);
    for (n = 30; n < INVENTORYSIZE; n++)
        if (!ch[cn].item[n]) nfree++;
    return nfree >= cnt;
}

// gives cnt of thing d, returns how many were given
int skilling_thing_give(int cn, int d, int cnt) {
    int n, in, tmp;

    if (d < 0 || d >= nskdef || cnt < 1) return 0;
    if (skdef[d].type == SKT_RESOURCE) return res_give(cn, d, cnt) ? cnt : 0;
    tmp = is_recipe(skdef[d].type) ? skdef[d].make_tmp : skdef[d].tmp;
    for (n = 0; n < cnt; n++) {
        if (!(in = create_item_nr(tmp))) break;
        if (is_recipe(skdef[d].type)) {
            if (skdef[d].name[0]) cfgcopy(it[in].name, skdef[d].name, sizeof(it[in].name));
            if (skdef[d].desc[0]) cfgcopy(it[in].description, skdef[d].desc, sizeof(it[in].description));
        }
        if (!store_item(cn, in)) {
            destroy_item(in);
            break;
        }
        if (ch[cn].flags & CF_PLAYER) dlog(cn, in, "quest reward");
    }
    ch[cn].flags |= CF_ITEMS;
    return n;
}
