/*
 * Skill registry: use-based skills (RuneScape-style overhaul).
 * See skillreg.h and zones/generic/skills.cfg.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "server.h"
#include "log.h"
#include "talk.h"
#include "drdata.h"
#include "create.h"
#include "tool.h"
#include "skill.h"
#include "database.h"
#include "mem.h"
#include "questlog.h"
#include "skillreg.h"
#include "player.h"
#include "io.h"
#include "balance.h"

#define SKILLS_CFG "zones/generic/skills.cfg"
#define QUESTS_CFG "zones/generic/quests.cfg"

struct skilldef skilldef[MAXSKILLS];
int skill_xp_context = -1;

static int xp_base = 80; // XP from level 1 to 2
static int xp_growth = 110; // each level needs this percent of the previous step
static int xp_rate = 100; // global XP rate in percent
static unsigned int xp_table[SKILL_MAXLEVEL + 2]; // xp_table[l] = total XP needed for level l
static int value_skill[V_MAX]; // V_* slot -> registry id, -1 = none
static int nretired, retired[MAXRETIRED]; // config retire=V_X: value slots of removed skills, zeroed for players at login

static char *group_name[SG_MAX] = {"power", "attribute", "combat", "magic", "gathering", "production", "support"};

static char *V_name[V_MAX] = {
    "V_HP", "V_ENDURANCE", "V_MANA", "V_WIS", "V_INT", "V_AGI", "V_STR", "V_ARMOR", "V_WEAPON", "V_OFFENSE",
    "V_DEFENSE", "V_LIGHT", "V_SPEED", "V_DAGGER", "V_HAND", "V_STAFF", "V_SWORD", "V_TWOHAND", "V_ATTACK", "V_PARRY",
    "V_WARCRY", "V_TACTICS", "V_SURROUND", "V_SPEEDSKILL", "V_BARTER", "V_PERCEPT", "V_STEALTH", "V_BLESS", "V_HEAL", "V_FREEZE",
    "V_MAGICSHIELD", "V_FLASH", "V_FIRE", "V_REGENERATE", "V_MEDITATE", "V_IMMUNITY", "V_DEMON", "V_DURATION", "V_RAGE", "V_COLD",
    "V_PROFESSION", "V_MELEE_ACC", "V_MELEE_STR", "V_MELEE_DEF", "V_MAGIC_ACC", "V_MAGIC_DMG", "V_MAGIC_DEF", "V_ARMORSKILL",
    "V_BODYCONTROL", "V_CLARITY"};

// bounded string copy, always terminated
static void scopy(char *dst, const char *src, int size) {
    int n;

    for (n = 0; n < size - 1 && src[n]; n++) dst[n] = src[n];
    dst[n] = 0;
}

static void build_xp_table(void) {
    double step = xp_base, total = 0;
    int l;

    xp_table[0] = xp_table[1] = 0;
    for (l = 2; l <= SKILL_MAXLEVEL + 1; l++) {
        total += step;
        if (total > 4000000000.0) total = 4000000000.0;
        xp_table[l] = (unsigned int)total;
        step = step * xp_growth / 100.0;
    }
}

unsigned int skill_xp_for_level(int level) {
    if (level < 1) return 0;
    if (level > SKILL_MAXLEVEL + 1) level = SKILL_MAXLEVEL + 1;
    return xp_table[level];
}

int skill_level_for_xp(unsigned int xp) {
    int lo = 1, hi = SKILL_MAXLEVEL, mid;

    while (lo < hi) { // highest level whose XP requirement is <= xp
        mid = (lo + hi + 1) / 2;
        if (xp_table[mid] <= xp) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

int skill_lookup_key(char *key) {
    int n;

    if (!strncasecmp(key, "SK_", 3)) key += 3;

    for (n = 0; n < MAXSKILLS; n++)
        if (skilldef[n].used && !strcasecmp(skilldef[n].key, key)) return n;
    return -1;
}

int skill_for_value(int v) {
    if (v < 0 || v >= V_MAX) return -1;
    return value_skill[v];
}

static int lookup_value_name(char *name) {
    int n;

    for (n = 0; n < V_MAX; n++)
        if (V_name[n] && !strcasecmp(V_name[n], name)) return n;
    return -1;
}

static int lookup_group(char *name) {
    int n;

    for (n = 0; n < SG_MAX; n++)
        if (!strcasecmp(group_name[n], name)) return n;
    return -1;
}

// reads "name=value" (value may be quoted), "name:" (block start) or ";" (block end).
// returns pointer after the line, or NULL at end of text
char *skillcfg_line(char *ptr, char *name, char *value, int *line) {
    int len;

    *name = *value = 0;

    while (*ptr && isspace(*ptr)) {
        if (*ptr == '\n') (*line)++;
        ptr++;
    }
    if (!*ptr) return NULL;

    if (*ptr == '#') { // comment
        while (*ptr && *ptr != '\n') ptr++;
        strcpy(name, "#");
        return ptr;
    }
    if (*ptr == ';') {
        strcpy(value, ";");
        return ptr + 1;
    }

    for (len = 0; *ptr && (isalnum(*ptr) || *ptr == '_') && len < 63; len++) *name++ = *ptr++;
    *name = 0;

    while (*ptr == ' ' || *ptr == '\t') ptr++;

    if (*ptr == ':') {
        strcpy(value, ":");
        return ptr + 1;
    }
    if (*ptr != '=') {
        strcpy(value, "!");
        return ptr;
    }
    ptr++;
    while (*ptr == ' ' || *ptr == '\t') ptr++;

    if (*ptr == '"') {
        ptr++;
        for (len = 0; *ptr && *ptr != '"' && *ptr != '\n' && len < 255; len++) *value++ = *ptr++;
        if (*ptr == '"') ptr++;
    } else {
        for (len = 0; *ptr && !isspace(*ptr) && *ptr != '#' && len < 255; len++) *value++ = *ptr++;
    }
    *value = 0;

    while (*ptr && *ptr != '\n') { // allow trailing comments
        if (*ptr == '#') {
            while (*ptr && *ptr != '\n') ptr++;
            break;
        }
        ptr++;
    }

    return ptr;
}

char *skillcfg_read(char *filename) {
    FILE *fp;
    long len;
    char *text;

    if (!(fp = fopen(filename, "rb"))) return NULL;
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len < 0 || !(text = xmalloc(len + 1, IM_TEMP))) {
        fclose(fp);
        return NULL;
    }
    if (fread(text, 1, len, fp) != (size_t)len) {
        xfree(text);
        fclose(fp);
        return NULL;
    }
    text[len] = 0;
    fclose(fp);
    return text;
}

static int store_block(struct skilldef *sd, int id, int is_config, int line) {
    if (is_config) return 1;

    if (id < 0 || id >= MAXSKILLS || id == SKILL_STYLE_SLOT) {
        elog("%s line %d: skill %s needs an id from 0 to %d (not %d, it holds the combat style).", SKILLS_CFG, line, sd->key, MAXSKILLS - 1, SKILL_STYLE_SLOT);
        return 0;
    }
    if (skilldef[id].used) {
        elog("%s line %d: skill id %d used twice (%s and %s).", SKILLS_CFG, line, id, skilldef[id].key, sd->key);
        return 0;
    }
    if (skill_lookup_key(sd->key) != -1) {
        elog("%s line %d: skill key %s used twice.", SKILLS_CFG, line, sd->key);
        return 0;
    }
    if (!sd->name[0]) strcpy(sd->name, sd->key);
    if (sd->max < 1) sd->max = 99;
    if (sd->max > SKILL_MAXLEVEL) sd->max = SKILL_MAXLEVEL;
    if (sd->value >= 0) {
        if (value_skill[sd->value] != -1) {
            elog("%s line %d: value %s mirrored by two skills.", SKILLS_CFG, line, V_name[sd->value]);
            return 0;
        }
        value_skill[sd->value] = id;
    }
    for (int n = 0; n < sd->nmirror; n++) {
        if (sd->value < 0) {
            elog("%s line %d: skill %s has mirror= but no value=.", SKILLS_CFG, line, sd->key);
            return 0;
        }
        if (value_skill[sd->mirror[n]] != -1) {
            elog("%s line %d: value %s mirrored by two skills.", SKILLS_CFG, line, V_name[sd->mirror[n]]);
            return 0;
        }
        value_skill[sd->mirror[n]] = id;
    }
    sd->used = 1;
    skilldef[id] = *sd;
    return 1;
}

int skills_load(void) {
    char *text, *ptr, name[64], value[256];
    struct skilldef sd;
    int line = 1, inblock = 0, is_config = 0, id = -1, n, m, tmp;

    bzero(skilldef, sizeof(skilldef));
    for (n = 0; n < V_MAX; n++) value_skill[n] = -1;
    nretired = 0;

    if (!(text = skillcfg_read(SKILLS_CFG))) {
        elog("Could not read %s. The skill system needs it.", SKILLS_CFG);
        return 0;
    }

    ptr = text;
    while ((ptr = skillcfg_line(ptr, name, value, &line))) {
        if (name[0] == '#') continue;

        if (value[0] == '!') {
            elog("%s line %d: syntax error near \"%s\".", SKILLS_CFG, line, name);
            xfree(text);
            return 0;
        }
        if (value[0] == ':') {
            if (inblock) {
                elog("%s line %d: missing ; before %s:", SKILLS_CFG, line, name);
                xfree(text);
                return 0;
            }
            inblock = 1;
            bzero(&sd, sizeof(sd));
            sd.value = -1;
            sd.max = 99;
            sd.rate = 100;
            id = -1;
            is_config = !strcasecmp(name, "config");
            scopy(sd.key, name, sizeof(sd.key));
            for (n = 0; sd.key[n]; n++) sd.key[n] = toupper(sd.key[n]);
            continue;
        }
        if (value[0] == ';') {
            if (!inblock) {
                elog("%s line %d: ; without block.", SKILLS_CFG, line);
                xfree(text);
                return 0;
            }
            if (!store_block(&sd, id, is_config, line)) {
                xfree(text);
                return 0;
            }
            inblock = 0;
            continue;
        }
        if (!inblock) {
            elog("%s line %d: %s outside of a block.", SKILLS_CFG, line, name);
            xfree(text);
            return 0;
        }

        if (is_config) {
            if (!strcasecmp(name, "xp_base")) xp_base = max(1, atoi(value));
            else if (!strcasecmp(name, "xp_growth")) xp_growth = max(100, atoi(value));
            else if (!strcasecmp(name, "rate")) xp_rate = max(0, atoi(value));
            else if (!strcasecmp(name, "retire")) { // retire=V_X: a removed skill's value, set to 0 for players
                if ((tmp = lookup_value_name(value)) == -1 || nretired >= MAXRETIRED) {
                    elog("%s line %d: retire needs a V_ name (at most %d).", SKILLS_CFG, line, MAXRETIRED);
                    xfree(text);
                    return 0;
                }
                retired[nretired++] = tmp;
            } else {
                elog("%s line %d: unknown config setting %s.", SKILLS_CFG, line, name);
                xfree(text);
                return 0;
            }
            continue;
        }

        if (!strcasecmp(name, "id")) id = atoi(value);
        else if (!strcasecmp(name, "name")) scopy(sd.name, value, sizeof(sd.name));
        else if (!strcasecmp(name, "description")) scopy(sd.desc, value, sizeof(sd.desc));
        else if (!strcasecmp(name, "start")) sd.start = atoi(value);
        else if (!strcasecmp(name, "max")) sd.max = atoi(value);
        else if (!strcasecmp(name, "rate")) sd.rate = atoi(value);
        else if (!strcasecmp(name, "use_xp")) sd.use_xp = atoi(value);
        else if (!strcasecmp(name, "dmg_xp")) sd.dmg_xp = atoi(value);
        else if (!strcasecmp(name, "group")) {
            if ((sd.group = lookup_group(value)) == -1) {
                elog("%s line %d: unknown group %s.", SKILLS_CFG, line, value);
                xfree(text);
                return 0;
            }
        } else if (!strcasecmp(name, "value")) {
            if ((sd.value = lookup_value_name(value)) == -1) {
                elog("%s line %d: unknown value %s.", SKILLS_CFG, line, value);
                xfree(text);
                return 0;
            }
        } else if (!strcasecmp(name, "mirror")) { // mirror=V_X: this slot gets the same level
            if ((tmp = lookup_value_name(value)) == -1 || sd.nmirror >= MAXSKILLMIRROR) {
                elog("%s line %d: unknown mirror value %s (at most %d per skill).", SKILLS_CFG, line, value, MAXSKILLMIRROR);
                xfree(text);
                return 0;
            }
            sd.mirror[sd.nmirror++] = tmp;
        } else if (!strcasecmp(name, "feed")) { // feed=KEY,percent
            char *comma = strchr(value, ',');
            if (!comma || sd.nfeed >= MAXSKILLFEED) {
                elog("%s line %d: feed must be KEY,percent (at most %d per skill).", SKILLS_CFG, line, MAXSKILLFEED);
                xfree(text);
                return 0;
            }
            *comma = 0;
            scopy(sd.feed_key[sd.nfeed], value, sizeof(sd.feed_key[0]));
            sd.feed_pct[sd.nfeed] = atoi(comma + 1);
            sd.nfeed++;
        } else {
            elog("%s line %d: unknown setting %s.", SKILLS_CFG, line, name);
            xfree(text);
            return 0;
        }
    }
    xfree(text);

    if (inblock) {
        elog("%s: missing ; at end of file.", SKILLS_CFG);
        return 0;
    }

    // resolve feeds now that every key is known
    for (n = 0; n < MAXSKILLS; n++) {
        if (!skilldef[n].used) continue;
        for (m = 0; m < skilldef[n].nfeed; m++) {
            if ((tmp = skill_lookup_key(skilldef[n].feed_key[m])) == -1) {
                elog("%s: skill %s feeds unknown skill %s.", SKILLS_CFG, skilldef[n].key, skilldef[n].feed_key[m]);
                return 0;
            }
            skilldef[n].feed_id[m] = tmp;
        }
    }

    for (n = 0; n < nretired; n++) {
        if (value_skill[retired[n]] != -1) {
            elog("%s: retire=%s is still used by skill %s.", SKILLS_CFG, V_name[retired[n]], skilldef[value_skill[retired[n]]].key);
            return 0;
        }
    }

    build_xp_table();

    for (n = tmp = 0; n < MAXSKILLS; n++)
        if (skilldef[n].used) tmp++;
    xlog("Loaded %d skills from %s (level 99 needs %u XP).", tmp, SKILLS_CFG, xp_table[99]);

    return quests_load();
}

// ---------- quest rewards (zones/generic/quests.cfg) ----------

static struct quest_reward quest_reward[MAXQUEST];

// reads the skill XP every quest gives; needs the skill keys, so it runs after skills_load
int quests_load(void) {
    char *text, *ptr, name[64], value[256];
    struct quest_reward qr;
    int line = 1, inblock = 0, qnr = -1, cnt = 0, id;

    bzero(quest_reward, sizeof(quest_reward));

    if (!(text = skillcfg_read(QUESTS_CFG))) {
        elog("Could not read %s. Quests give no skill XP.", QUESTS_CFG);
        return 1;
    }

    ptr = text;
    while ((ptr = skillcfg_line(ptr, name, value, &line))) {
        if (name[0] == '#') continue;

        if (value[0] == '!') {
            elog("%s line %d: syntax error near \"%s\".", QUESTS_CFG, line, name);
            goto fail;
        }
        if (value[0] == ':') {
            if (inblock) {
                elog("%s line %d: missing ; before %s:", QUESTS_CFG, line, name);
                goto fail;
            }
            inblock = 1;
            bzero(&qr, sizeof(qr));
            qnr = -1;
            continue;
        }
        if (value[0] == ';') {
            if (!inblock) {
                elog("%s line %d: ; without block.", QUESTS_CFG, line);
                goto fail;
            }
            if (qnr < 0 || qnr >= MAXQUEST) {
                elog("%s line %d: quest= must be a quest number from 0 to %d.", QUESTS_CFG, line, MAXQUEST - 1);
                goto fail;
            }
            if (quest_reward[qnr].n) {
                elog("%s line %d: quest %d has two blocks.", QUESTS_CFG, line, qnr);
                goto fail;
            }
            quest_reward[qnr] = qr;
            cnt++;
            inblock = 0;
            continue;
        }
        if (!inblock) {
            elog("%s line %d: %s outside of a block.", QUESTS_CFG, line, name);
            goto fail;
        }

        if (!strcasecmp(name, "quest")) qnr = atoi(value);
        else if (!strcasecmp(name, "xp")) { // xp=KEY,amount
            char *comma = strchr(value, ',');
            if (!comma || qr.n >= MAXQUESTREWARD) {
                elog("%s line %d: xp must be KEY,amount (at most %d per quest).", QUESTS_CFG, line, MAXQUESTREWARD);
                goto fail;
            }
            *comma = 0;
            if ((id = skill_lookup_key(value)) == -1) {
                elog("%s line %d: unknown skill %s.", QUESTS_CFG, line, value);
                goto fail;
            }
            qr.id[qr.n] = id;
            qr.xp[qr.n] = max(0, atoi(comma + 1));
            qr.n++;
        } else {
            elog("%s line %d: unknown setting %s.", QUESTS_CFG, line, name);
            goto fail;
        }
    }
    xfree(text);

    if (inblock) {
        elog("%s: missing ; at end of file.", QUESTS_CFG);
        return 0;
    }
    xlog("Loaded skill rewards for %d quests from %s.", cnt, QUESTS_CFG);
    return 1;

fail:
    xfree(text);
    return 0;
}

// gives the skill XP of quest qnr; cnt = how often it was done before (repeats give less)
void skill_quest_reward(int cn, int qnr, int cnt) {
    struct quest_reward *qr;
    int n, xp;

    if (qnr < 0 || qnr >= MAXQUEST) return;
    qr = &quest_reward[qnr];

    for (n = 0; n < qr->n; n++) {
        if (!(xp = questlog_scale(cnt, qr->xp[n]))) continue;
        log_char(cn, LOG_SYSTEM, 0, "\260c3Quest reward: %d %s XP.", xp, skilldef[qr->id[n]].name);
        add_skill_xp(cn, qr->id[n], xp);
    }
}

// writes a skill's level into its value slot and every mirror= slot
void skill_store_level(int cn, int id, int level) {
    struct skilldef *sd;
    int n;

    if (id < 0 || id >= MAXSKILLS || !skilldef[id].used || skilldef[id].value < 0) return;
    sd = skilldef + id;
    ch[cn].value[1][sd->value] = level;
    for (n = 0; n < sd->nmirror; n++) ch[cn].value[1][sd->mirror[n]] = level;
}

int skill_is_mirror(int v) {
    int id = skill_for_value(v);

    return id >= 0 && skilldef[id].value != v;
}

static struct skill_ppd *get_ppd(int cn) {
    return set_data(cn, DRD_SKILL_PPD, sizeof(struct skill_ppd));
}

int skill_get_level(int cn, int id) {
    struct skill_ppd *ppd;

    if (id < 0 || id >= MAXSKILLS || !skilldef[id].used) return 0;
    if (skilldef[id].value >= 0) return ch[cn].value[1][skilldef[id].value];

    if (!(ppd = get_ppd(cn))) return 0;
    if (!ppd->xp[id] && !skilldef[id].start) return 0; // never had it
    return skill_level_for_xp(ppd->xp[id]);
}

unsigned int skill_get_xp(int cn, int id) {
    struct skill_ppd *ppd;

    if (id < 0 || id >= MAXSKILLS || !skilldef[id].used) return 0;
    if (!(ppd = get_ppd(cn))) return 0;
    return ppd->xp[id];
}

// sets level and XP directly (templates, GM commands). no messages.
void skill_set_level(int cn, int id, int level) {
    struct skill_ppd *ppd;

    if (id < 0 || id >= MAXSKILLS || !skilldef[id].used) return;
    level = max(0, min(level, skilldef[id].max));

    skill_store_level(cn, id, level);
    if ((ppd = get_ppd(cn))) ppd->xp[id] = skill_xp_for_level(level);
}

static int add_xp_raw(int cn, int id, int xp, int depth) {
    struct skill_ppd *ppd;
    struct skilldef *sd;
    int oldlevel, newlevel, n;
    unsigned long long tmp;

    if (id < 0 || id >= MAXSKILLS || !skilldef[id].used) return 0;
    if (xp <= 0) return 0;

    sd = skilldef + id;
    if (sd->value >= 0 && !ch[cn].value[1][sd->value]) return 0; // character does not have the skill
    if (!(ppd = get_ppd(cn))) return 0;

    oldlevel = skill_level_for_xp(ppd->xp[id]);
    if (sd->value >= 0) oldlevel = max(oldlevel, ch[cn].value[1][sd->value]);

    tmp = (unsigned long long)ppd->xp[id] + xp;
    if (tmp > 4000000000ull) tmp = 4000000000ull;
    ppd->xp[id] = (unsigned int)tmp;

    newlevel = min(sd->max, skill_level_for_xp(ppd->xp[id]));

    if (newlevel > oldlevel) {
        skill_store_level(cn, id, newlevel);
        log_char(cn, LOG_SYSTEM, 0, "\260c3Congratulations! Your %s level is now %d.", sd->name, newlevel);
        dlog(cn, 0, "skill %s reached level %d", sd->name, newlevel);
        if (sd->value >= 0) {
            update_char(cn);
            check_levelup(cn);
        }
    }

    if (depth == 0) {
        for (n = 0; n < sd->nfeed; n++) {
            int f = xp * sd->feed_pct[n]; // in hundredths, a fraction is given by chance
            add_xp_raw(cn, sd->feed_id[n], f / 100 + (RANDOM(100) < f % 100), depth + 1);
        }
    }

    return 1;
}

// the one entry point for skill XP. xp is the base amount before rates.
int add_skill_xp(int cn, int id, int xp) {
    if (cn < 1 || cn >= MAXCHARS || !(ch[cn].flags & CF_PLAYER)) return 0;
    if (id < 0 || id >= MAXSKILLS || !skilldef[id].used) return 0;

    return add_skill_xp_milli(cn, id, (long long)xp * 1000);
}

// the same with XP in thousandths of a point (damage XP). After the rates, a fraction left over is
// given by chance, so small amounts (a lightning strike, a step) are not lost to rounding.
int add_skill_xp_milli(int cn, int id, long long milli) {
    long long t;
    int xp;

    if (cn < 1 || cn >= MAXCHARS || !(ch[cn].flags & CF_PLAYER)) return 0;
    if (id < 0 || id >= MAXSKILLS || !skilldef[id].used) return 0;
    if (milli <= 0) return 0;

    t = milli * xp_rate * skilldef[id].rate / 10000; // still in thousandths
    xp = (int)(t / 1000);
    if (RANDOM(1000) < t % 1000) xp++;

    return add_xp_raw(cn, id, xp, 0);
}

// raises a skill by exactly one level (quest/item rewards that used to "raise" a skill)
int skill_add_level(int cn, int id) {
    struct skill_ppd *ppd;
    int level;
    unsigned int need;

    if (cn < 1 || cn >= MAXCHARS || !(ch[cn].flags & CF_PLAYER)) return 0;
    if (id < 0 || id >= MAXSKILLS || !skilldef[id].used) return 0;
    if (!(ppd = get_ppd(cn))) return 0;

    level = skill_get_level(cn, id);
    if (level < 1 || level >= skilldef[id].max) return 0;

    need = skill_xp_for_level(level + 1);
    if (ppd->xp[id] >= need) return 0;

    return add_xp_raw(cn, id, need - ppd->xp[id], 1);
}

void skill_used(int cn, int id) {
    if (id < 0 || id >= MAXSKILLS || !skilldef[id].used) return;
    add_skill_xp(cn, id, skilldef[id].use_xp);
}

// dam is in POWERSCALE units (1000 = one hitpoint)
void skill_damage_xp(int cn, int id, int dam) {
    if (id < 0 || id >= MAXSKILLS || !skilldef[id].used) return;
    if (dam <= 0 || !skilldef[id].dmg_xp) return;
    // dam is in thousandths of a hitpoint, so dam x dmg_xp is in thousandths of an XP point
    add_skill_xp_milli(cn, id, (long long)dam * skilldef[id].dmg_xp * 1000 / POWERSCALE);
}

// value index (V_HAND ... V_TWOHAND) of the weapon skill cn is fighting with
int weapon_value(int cn) {
    int in, v = V_HAND, best = -1;

    in = ch[cn].item[WN_RHAND];
    if (in > 0 && in < MAXITEM && (it[in].flags & IF_WEAPON)) {
        // same order as get_fight_skill(): the highest matching skill is used
        if ((it[in].flags & IF_HAND) && ch[cn].value[0][V_HAND] > best) best = ch[cn].value[0][v = V_HAND];
        if ((it[in].flags & IF_DAGGER) && ch[cn].value[0][V_DAGGER] > best) best = ch[cn].value[0][v = V_DAGGER];
        if ((it[in].flags & IF_STAFF) && ch[cn].value[0][V_STAFF] > best) best = ch[cn].value[0][v = V_STAFF];
        if ((it[in].flags & IF_SWORD) && ch[cn].value[0][V_SWORD] > best) best = ch[cn].value[0][v = V_SWORD];
        if ((it[in].flags & IF_TWOHAND) && ch[cn].value[0][V_TWOHAND] > best) best = ch[cn].value[0][v = V_TWOHAND];
    }
    return v;
}

// registry id of the weapon skill cn is fighting with
int skill_weapon_id(int cn) {
    return skill_for_value(weapon_value(cn));
}

// ---------- combat style ----------

static const char *style_name[STYLE_MAX + 1] = {"Shared", "Attack", "Strength", "Defence"};

int skill_get_style(int cn) {
    struct skill_ppd *ppd;

    if (cn < 1 || cn >= MAXCHARS || !(ch[cn].flags & CF_PLAYER) || !(ppd = get_ppd(cn))) return STYLE_SHARED;
    return ppd->xp[SKILL_STYLE_SLOT] <= STYLE_MAX ? (int)ppd->xp[SKILL_STYLE_SLOT] : STYLE_SHARED;
}

int skill_set_style(int cn, int style) {
    struct skill_ppd *ppd;

    if (style < 0 || style > STYLE_MAX) return 0;
    if (cn < 1 || cn >= MAXCHARS || !(ch[cn].flags & CF_PLAYER) || !(ppd = get_ppd(cn))) return 0;
    ppd->xp[SKILL_STYLE_SLOT] = style;
    return 1;
}

// melee damage XP by the attacker's style; shared gives a third of the damage to each skill
void skill_melee_damage_xp(int cn, int dam) {
    switch (skill_get_style(cn)) {
    case STYLE_ATTACK: skill_damage_xp(cn, skill_weapon_id(cn), dam); break;
    case STYLE_STRENGTH: skill_damage_xp(cn, skill_for_value(V_STR), dam); break;
    case STYLE_DEFENCE: skill_damage_xp(cn, skill_for_value(V_PARRY), dam); break;
    default:
        skill_damage_xp(cn, skill_weapon_id(cn), dam / 3);
        skill_damage_xp(cn, skill_for_value(V_STR), dam / 3);
        skill_damage_xp(cn, skill_for_value(V_PARRY), dam / 3);
        break;
    }
}

// /style [shared|attack|strength|defence]
void style_command(int cn, char *arg) {
    int n;

    while (isspace(*arg)) arg++;
    if (*arg) {
        for (n = 0; n <= STYLE_MAX; n++) {
            if (!strncasecmp(arg, style_name[n], 3)) break;
        }
        if (n > STYLE_MAX) {
            log_char(cn, LOG_SYSTEM, 0, "Usage: /style shared, attack, strength or defence.");
            return;
        }
        skill_set_style(cn, n);
    }
    n = skill_get_style(cn);
    if (n == STYLE_SHARED) log_char(cn, LOG_SYSTEM, 0, "Combat style: Shared (melee XP goes to Attack, Strength and Defence equally).");
    else log_char(cn, LOG_SYSTEM, 0, "Combat style: %s (melee XP goes to %s).", style_name[n], style_name[n]);
}

// called at every login: gives every player all starting skills and keeps XP in line
// with levels that were changed by other means (templates, rewards, GM commands).
void skills_init_player(int cn) {
    struct skill_ppd *ppd;
    struct skilldef *sd;
    int id, level;

    if (!(ch[cn].flags & CF_PLAYER)) return;
    if (!(ppd = get_ppd(cn))) return;

    for (id = 0; id < nretired; id++) ch[cn].value[1][retired[id]] = 0; // removed skills

    for (id = 0; id < MAXSKILLS; id++) {
        sd = skilldef + id;
        if (!sd->used) continue;

        if (sd->value >= 0) {
            level = max(ch[cn].value[1][sd->value], sd->start);
            skill_store_level(cn, id, level);
        } else {
            level = skill_level_for_xp(ppd->xp[id]);
            if (!ppd->xp[id] && !sd->start) continue;
            level = max(level, sd->start);
        }
        if (level > 0 && ppd->xp[id] < skill_xp_for_level(level)) ppd->xp[id] = skill_xp_for_level(level);
    }
}

// /skills [filter]: list skills with level and XP
int skills_show(int cn, char *filter) {
    struct skill_ppd *ppd;
    struct skilldef *sd;
    int g, id, level, cnt = 0;
    unsigned int xp, next;

    if (!(ppd = get_ppd(cn))) return 0;

    for (g = 0; g < SG_MAX; g++) {
        int head = 0;

        if (filter && *filter && strncasecmp(group_name[g], filter, strlen(filter))) continue;
        for (id = 0; id < MAXSKILLS; id++) {
            sd = skilldef + id;
            if (!sd->used || sd->group != g) continue;
            if (!(level = skill_get_level(cn, id))) continue;
            if (!head++) log_char(cn, LOG_SYSTEM, 0, "\260c3%c%s:", toupper(group_name[g][0]), group_name[g] + 1);
            xp = ppd->xp[id];
            if (level >= sd->max) log_char(cn, LOG_SYSTEM, 0, "  %s %d (%u XP, max)", sd->name, level, xp);
            else {
                next = skill_xp_for_level(level + 1);
                log_char(cn, LOG_SYSTEM, 0, "  %s %d (%u XP, %u to next)", sd->name, level, xp, next > xp ? next - xp : 0);
            }
            cnt++;
        }
    }
    log_char(cn, LOG_SYSTEM, 0, "Combat level %d.", ch[cn].level);
    return cnt;
}

// ---- client skills panel (protocol 5) ----
// The skill list is sent once per login, XP only when it changed, at most four times a second.

static unsigned int sent_xp[MAXPLAYER][MAXSKILLS];
static unsigned char sent_level[MAXPLAYER][MAXSKILLS];
static char sent_defs[MAXPLAYER];
static unsigned char sent_style[MAXPLAYER]; // style + 1, 0 = not sent yet
static unsigned int sent_effects[MAXPLAYER]; // hash of the last effects packet, 0 = none sent

void skill_client_reset(int nr) {
    if (nr < 1 || nr >= MAXPLAYER) return;
    sent_effects[nr] = 0;
    bzero(sent_xp[nr], sizeof(sent_xp[nr]));
    bzero(sent_level[nr], sizeof(sent_level[nr]));
    sent_defs[nr] = 0;
    sent_style[nr] = 0;
}

static int skill_client_def(int nr, int id) {
    struct skilldef *sd = skilldef + id;
    unsigned char buf[256];
    int nlen, dlen;

    nlen = min((int)strlen(sd->name), 39);
    dlen = min((int)strlen(sd->desc), 159);

    buf[0] = SV_SKILLDEF;
    buf[1] = id;
    buf[2] = sd->group;
    buf[3] = sd->value >= 0 ? sd->value : 255;
    buf[4] = min(sd->max, 255);
    buf[5] = nlen;
    memcpy(buf + 6, sd->name, nlen);
    buf[6 + nlen] = dlen;
    memcpy(buf + 7 + nlen, sd->desc, dlen);
    psend(nr, (char *)buf, 7 + nlen + dlen);

    return get_player_version(nr) != 0; // 0: kicked while sending
}

// Effects tab (protocol 10): [SV_EFFECTS][length, 2 bytes][count] then per line
// [group][label length][label][value length][value][desc length][desc]. Sent only when a line changed.
static int effects_client_update(int cn, int nr) {
    static unsigned char buf[8192];
    static struct effect_list e;
    unsigned int hash = 2166136261u;
    int n, p = 4, l;

    effects_build(cn, &e);
    for (n = 0; n < e.n; n++) {
        buf[p++] = e.group[n];
        l = strlen(e.label[n]);
        buf[p++] = l;
        memcpy(buf + p, e.label[n], l);
        p += l;
        l = strlen(e.value[n]);
        buf[p++] = l;
        memcpy(buf + p, e.value[n], l);
        p += l;
        l = strlen(e.desc[n]);
        buf[p++] = l;
        memcpy(buf + p, e.desc[n], l);
        p += l;
    }
    buf[0] = SV_EFFECTS;
    *(unsigned short *)(buf + 1) = p;
    buf[3] = e.n;
    for (n = 3; n < p; n++) hash = (hash ^ buf[n]) * 16777619u;
    if (!hash) hash = 1;
    if (hash == sent_effects[nr]) return 1;
    sent_effects[nr] = hash;
    psend(nr, (char *)buf, p);
    return get_player_version(nr) != 0;
}

// returns 0 if the player was kicked while sending
int skill_client_update(int cn, int nr) {
    unsigned char buf[16];
    unsigned int xp;
    int id, level;

    if (nr < 1 || nr >= MAXPLAYER) return 1;
    if (get_player_version(nr) < SKILL_CLIENT_VERSION) return 1; // also 0 if the player is gone

    if (!sent_defs[nr]) {
        for (id = 0; id < MAXSKILLS; id++) {
            if (skilldef[id].used && !skill_client_def(nr, id)) return 0;
        }
        sent_defs[nr] = 1;
    } else if (ticker % 6) return 1;

    for (id = 0; id < MAXSKILLS; id++) {
        if (!skilldef[id].used) continue;

        level = min(skill_get_level(cn, id), 255);
        xp = skill_get_xp(cn, id);
        if (level == sent_level[nr][id] && xp == sent_xp[nr][id]) continue;
        sent_level[nr][id] = level;
        sent_xp[nr][id] = xp;

        buf[0] = SV_SKILLXP;
        buf[1] = id;
        buf[2] = level;
        *(unsigned int *)(buf + 3) = xp;
        *(unsigned int *)(buf + 7) = level > 0 ? skill_xp_for_level(level) : 0;
        *(unsigned int *)(buf + 11) = (level > 0 && level < skilldef[id].max) ? skill_xp_for_level(level + 1) : 0;
        psend(nr, (char *)buf, 15);
        if (!get_player_version(nr)) return 0;
    }
    if (get_player_version(nr) >= STYLE_CLIENT_VERSION && sent_style[nr] != skill_get_style(cn) + 1) {
        sent_style[nr] = skill_get_style(cn) + 1;
        buf[0] = SV_COMBATSTYLE;
        buf[1] = sent_style[nr] - 1;
        psend(nr, (char *)buf, 2);
        if (!get_player_version(nr)) return 0;
    }
    if (get_player_version(nr) >= EFFECTS_CLIENT_VERSION) return effects_client_update(cn, nr);
    return 1;
}
