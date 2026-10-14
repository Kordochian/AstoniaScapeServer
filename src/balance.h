/*
 * Part of Astonia Server 3.5 (c) Daniel Brockhaus. Please read license.txt.
 */

#define ATTACK_DIV 10 // turns an average of 50 weapon value into an average of 5 damage (before armor)

#define FIREBALL_DAMAGE (85 / ATTACK_DIV) // makes it the same as weapon damage
#define STRIKE_DAMAGE (56 / ATTACK_DIV) // unused: lightning now deals FIREBALL_DAMAGE spread over time (strike_damage in balance.c)

#define RAGEMOD 4 // rage skill is divided by this before being used

int skilldiff2percent(int diff);

// combat settings (skilling.cfg config:, step 14)
extern int combat_tactics_boost; // percent of Tactics added to the effective melee levels
extern int combat_min_hit; // a landed melee hit rolls between this percent of the max hit and the max hit
extern int combat_armor_soak; // armor soaks at most this percent of a melee hit
extern int combat_magic_damage_cap; // most magic damage percent gear can add
extern int combat_body_control; // tenths of a percent per Body Control level on the melee levels (step 15)
extern int combat_clarity; // tenths of a percent per Clarity level on magic strength, immunity and spell damage
extern int combat_armor_class_percent; // tenths of a percent per Armor Class level on worn armour's stats
extern int perception_stealth, perception_fast, stealth_endurance; // step 16, see balance.c
int perception_value(int cn); // Perception after the speed mode
extern int attr_agility, attr_agility_speed, attr_intuition, attr_wisdom; // percent of the attribute, see balance.c
extern int athletics_speed, immunity_defence;
int attribute_bonus(int cn, int v); // what the attributes add to value v (update_char): Speed only
int wisdom_boost(int cn);
int intuition_boost(int cn);
int immunity_value(int cn); // Immunity + Wisdom

// effects breakdown (George 2026-10-13): what levels, gear and attributes add up to, for the client's
// Effects tab (SV_EFFECTS, skillreg.c) and /effects. Skill levels themselves stay unmodified.
#define MAXEFFECTLINES 48
#define EG_MELEE 0
#define EG_MAGIC 1
#define EG_ATTRIBUTE 2
#define EG_GATHERING 3
#define EG_STEALTH 4
struct effect_list {
    int n;
    unsigned char group[MAXEFFECTLINES];
    char label[MAXEFFECTLINES][24]; // short, fits the panel next to the value
    char value[MAXEFFECTLINES][16];
    char desc[MAXEFFECTLINES][160]; // hover text
};
// desc may be NULL; the value is printf-formatted
void effect_add(struct effect_list *e, int group, const char *label, const char *desc, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
void effects_build(int cn, struct effect_list *e);
extern int combat_weapon_skill_percent; // tenths of a percent per weapon skill level on the wielded weapon's stats
int item_boost_skill(int in); // V_ARMORSKILL or the weapon skill an item requires, -1 = none
int item_skill_permille(int cn, int v); // 1000 = no bonus
int body_control_permille(int cn); // 1000 = no bonus
int clarity_permille(int cn);
void combat_settings_reset(void);
int melee_attack_level(int cn);
int melee_strength_level(int cn);
int melee_defence_level(int cn);
int melee_surround_level(int cn);
int melee_hit_chance(int cn, int co, int vcn, int vco); // in 1/10000
int melee_max_hit(int cn); // in POWERSCALE units
int tactics2skill(int val);
