/*
 * Skill registry: use-based skills (RuneScape-style overhaul).
 *
 * Every skill is defined in zones/generic/skills.cfg. A skill either mirrors its
 * level into ch.value[1][v] (combat skills, attributes, powers, spells) or lives only
 * in the skill PPD (gathering/production skills like Mining). XP for all skills is
 * kept in the persistent player data block DRD_SKILL_PPD.
 */

#define MAXSKILLS 128 // registry ids 0..127, ids are save indexes: never renumber
#define SKILL_MAXLEVEL 120 // hard upper bound of the XP table

#define SG_POWER 0
#define SG_ATTRIBUTE 1
#define SG_COMBAT 2
#define SG_MAGIC 3
#define SG_GATHERING 4
#define SG_PRODUCTION 5
#define SG_SUPPORT 6
#define SG_MAX 7

#define MAXSKILLFEED 8
#define MAXSKILLMIRROR 8
#define MAXRETIRED 16

struct skilldef {
    int used;
    char key[32]; // used as SK_<key> in .chr files
    char name[40];
    char desc[160];
    int group; // SG_*
    int value; // V_* slot this skill mirrors into, -1 = none
    int nmirror; // more V_* slots that get the same level (Attack -> the weapon skills, Magic -> every spell)
    int mirror[MAXSKILLMIRROR];
    int start; // level every player starts with (0 = player does not have the skill)
    int max; // highest level
    int rate; // XP multiplier in percent
    int use_xp; // XP per use (spell cast, warcry, mining swing ...)
    int dmg_xp; // XP per point of damage dealt with this skill
    int nfeed;
    int feed_id[MAXSKILLFEED]; // also gives feed_pct percent of gained XP to these skills
    int feed_pct[MAXSKILLFEED];
    char feed_key[MAXSKILLFEED][32];
};

#define MAXQUESTREWARD 8

struct quest_reward { // skill XP a quest gives, from zones/generic/quests.cfg
    int n;
    int id[MAXQUESTREWARD];
    int xp[MAXQUESTREWARD];
};

struct skill_ppd {
    unsigned int xp[MAXSKILLS];
};

extern struct skilldef skilldef[MAXSKILLS];
extern int skill_xp_context; // skill that gets damage XP for the next hurt() call, -1 = none, SKILL_XP_MELEE = by style

// Combat style (protocol 11): which skill melee damage XP goes to. Saved in the skill PPD in xp[SKILL_STYLE_SLOT],
// so that registry id must never be used by a skill. 0 (the default) = shared: a third each to Attack,
// Strength and Defence.
#define SKILL_XP_MELEE -2
#define SKILL_STYLE_SLOT 127
#define STYLE_SHARED 0
#define STYLE_ATTACK 1
#define STYLE_STRENGTH 2
#define STYLE_DEFENCE 3
#define STYLE_MAX 3
// server -> client: [SV_COMBATSTYLE][style], at login and on every change. The client sets the style with
// the chat command "/style <shared|attack|strength|defence>".
#define SV_COMBATSTYLE 58
#define STYLE_CLIENT_VERSION 11
int skill_get_style(int cn);
int skill_set_style(int cn, int style);
void skill_melee_damage_xp(int cn, int dam);
void style_command(int cn, char *arg);

int skills_load(void);
int skill_lookup_key(char *key);
int skill_for_value(int v);
void skill_store_level(int cn, int id, int level); // value slot and mirrors
int skill_is_mirror(int v); // v only copies another skill's level (mirror=)
unsigned int skill_xp_for_level(int level);
int skill_level_for_xp(unsigned int xp);
int skill_get_level(int cn, int id);
unsigned int skill_get_xp(int cn, int id);
void skill_set_level(int cn, int id, int level);
int add_skill_xp(int cn, int id, int xp);
int add_skill_xp_milli(int cn, int id, long long milli); // XP in thousandths, fraction given by chance
int skill_add_level(int cn, int id);
void skill_used(int cn, int id);
void skill_damage_xp(int cn, int id, int dam);
int weapon_value(int cn); // V_HAND ... V_TWOHAND of the weapon cn fights with
int skill_weapon_id(int cn);
void skills_init_player(int cn);
int skills_show(int cn, char *filter);
int quests_load(void);

// shared config reader (block files: KEY: ... ;, name=value, # comments)
char *skillcfg_line(char *ptr, char *name, char *value, int *line);
char *skillcfg_read(char *filename); // xfree() the result
void skill_quest_reward(int cn, int qnr, int cnt);

// client skills panel (protocol 5): skill list and per-skill XP for the client.
// Both use free command numbers of client.h (were SV_RESERVED1/2).
#define SV_SKILLDEF 54 // id, group, value slot (255 none), max, name len, name, desc len, desc
#define SV_SKILLXP 55 // id, level, xp, xp at this level, xp at next level (0 = max), 15 bytes
#define SV_EFFECTS 57 // effects tab, see effects_client_update()
#define EFFECTS_CLIENT_VERSION 10
#define SKILL_CLIENT_VERSION 5 // clients from this protocol version on get the messages

void skill_client_reset(int nr);
int skill_client_update(int cn, int nr);
