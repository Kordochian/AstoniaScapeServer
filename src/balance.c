/*
 * Part of Astonia Server 3.5 (c) Daniel Brockhaus. Please read license.txt.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "server.h"
#include "log.h"
#include "talk.h"
#include "btrace.h"
#include "balance.h"
#include "skillreg.h"

int get_fight_skill(int cn) {
    int in, m = 0;

    if (cn < 1 || cn >= MAXCHARS) {
        elog("get_fight_skill(): called with illegal character %d", cn);
        return 0;
    }

    in = ch[cn].item[WN_RHAND];

    if (in < 0 || in >= MAXITEM) {
        elog("get_fight_skill(): character %s (%d) is wielding illegal item %d. Removing.", ch[cn].name, cn, in);
        ch[cn].item[WN_RHAND] = 0;
        in = 0;
    }

    if (!in || !(it[in].flags & IF_WEAPON)) { // not wielding anything, or not wielding a weapon
        return ch[cn].value[0][V_HAND];
    }

    if (it[in].flags & IF_HAND) m = max(m, ch[cn].value[0][V_HAND]);
    if (it[in].flags & IF_DAGGER) m = max(m, ch[cn].value[0][V_DAGGER]);
    if (it[in].flags & IF_STAFF) m = max(m, ch[cn].value[0][V_STAFF]);
    if (it[in].flags & IF_SWORD) m = max(m, ch[cn].value[0][V_SWORD]);
    if (it[in].flags & IF_TWOHAND) m = max(m, ch[cn].value[0][V_TWOHAND]);

    return m;
}

double get_spell_average(int cn) {
    return (ch[cn].value[0][V_BLESS] +
            ch[cn].value[0][V_HEAL] +
            ch[cn].value[0][V_FREEZE] +
            ch[cn].value[0][V_MAGICSHIELD] +
            ch[cn].value[0][V_FLASH] +
            ch[cn].value[0][V_FIRE]) /
        6.0;
}

// ---------- RuneScape-style melee (step 14, George 2026-10-13) ----------
// Effective levels (step 18, George: one skill, one job): accuracy = Attack, strength = Strength,
// defence = Defence (V_PARRY), surround = (weapon skill + Surround) / 2. For players the weapon skills
// mirror Attack (skills.cfg). Enemies use the same values from their .chr files (V_ATTACK, V_STR,
// V_PARRY). Each plus combat_tactics_boost percent of Tactics (enemies only now, players have no
// Tactics; default 0), then raised by Body Control (players: retired, 0). Gear adds
// Melee Accuracy / Strength / Defence bonuses.

int combat_tactics_boost = 0, combat_min_hit = 20, combat_armor_soak = 75, combat_magic_damage_cap = 50;
int combat_body_control = 3, combat_clarity = 3; // step 15: tenths of a percent per skill level
int combat_armor_class_percent = 5; // tenths of a percent per Armor Class level on worn armour's stats
int combat_weapon_skill_percent = 5; // tenths of a percent per weapon skill level on the wielded weapon's stats
// stealth and perception (step 16): Perception is raised this percent while the looker is in stealth
// mode and lowered this percent in fast mode; stealth walking costs this percent of a fast step's endurance
int perception_stealth = 25, perception_fast = 25, stealth_endurance = 100;

// Old attributes (step 17). Step 18 removed them from players (skills.cfg retire=); these percents
// still work for enemies whose .chr files give them Agility / Intuition / Wisdom, default 0 (off):
// Agility -> melee accuracy and defence levels (agility_bonus) and Speed (agility_speed)
// Intuition -> spell power (spellpower()), Wisdom -> mana regeneration and Immunity (wisdom_bonus)
int attr_agility = 0, attr_agility_speed = 0, attr_intuition = 0, attr_wisdom = 0;
// Athletics (V_SPEEDSKILL, step 18): adds this percent of its level to Speed (movement and attack speed)
int athletics_speed = 50;
// Immunity against spells (step 18): at least this percent of Defence
int immunity_defence = 50;

// what the attributes add to a value in update_char(): only Speed now
int attribute_bonus(int cn, int v) {
    if (v == V_SPEED) return ch[cn].value[0][V_AGI] * attr_agility_speed / 100;
    return 0;
}

static int agility_boost(int cn) {
    return ch[cn].value[0][V_AGI] * attr_agility / 100;
}

int wisdom_boost(int cn) {
    return ch[cn].value[0][V_WIS] * attr_wisdom / 100;
}

int intuition_boost(int cn) {
    return ch[cn].value[0][V_INT] * attr_intuition / 100;
}

// Immunity against spells: the Immunity value (enemies) or immunity_defence percent of Defence,
// whichever is higher, plus Wisdom
int immunity_value(int cn) {
    return max(ch[cn].value[0][V_IMMUNITY], ch[cn].value[0][V_PARRY] * immunity_defence / 100) + wisdom_boost(cn);
}

// Perception after the looker's speed mode (cautious in stealth, careless when running)
int perception_value(int cn) {
    int val = ch[cn].value[0][V_PERCEPT];

    if (ch[cn].speed_mode == SM_STEALTH) return val * (100 + perception_stealth) / 100;
    if (ch[cn].speed_mode == SM_FAST) return val * max(0, 100 - perception_fast) / 100;
    return val;
}

void combat_settings_reset(void) {
    combat_tactics_boost = 0;
    combat_min_hit = 20;
    combat_armor_soak = 75;
    combat_magic_damage_cap = 50;
    combat_body_control = 3;
    combat_clarity = 3;
    combat_armor_class_percent = 5;
    combat_weapon_skill_percent = 5;
    perception_stealth = 25;
    perception_fast = 25;
    stealth_endurance = 100;
    attr_agility = 0;
    attr_agility_speed = 0;
    attr_intuition = 0;
    attr_wisdom = 0;
    athletics_speed = 50;
    immunity_defence = 50;
}

// the skill an item's level requirement is on, when that skill boosts the item: Armor Class for
// armour, the weapon skill for weapons. -1 = none.
int item_boost_skill(int in) {
    int n, v;

    if (in < 1 || in >= MAXITEM) return -1;
    for (n = 0; n < MAXMOD; n++) {
        if (it[in].mod_value[n] <= 0) continue;
        v = -it[in].mod_index[n];
        if (v == V_ARMORSKILL || v == V_HAND || v == V_DAGGER || v == V_STAFF || v == V_SWORD || v == V_TWOHAND) return v;
    }
    return -1;
}

// Armor Class and weapon skills (step 15): the wearer's level in the item's requirement skill raises
// the item's positive Armor and equipment bonuses by combat_armor_class_percent (armour) or
// combat_weapon_skill_percent (weapons) tenths of a percent per level. It is worked out on the
// character in update_char() and never written to the item, so traded gear keeps its own values.
int item_skill_permille(int cn, int v) {
    if (cn < 1 || cn >= MAXCHARS || v < 0 || v >= V_MAX) return 1000;
    return 1000 + max(0, ch[cn].value[1][v]) * (v == V_ARMORSKILL ? combat_armor_class_percent : combat_weapon_skill_percent);
}

// Body Control and Clarity (step 15): a flat bonus per trained level, in tenths of a percent
// (1000 = no bonus). Gear never raises these skills (IS_PURE_SKILL in update_char()).
int body_control_permille(int cn) {
    if (cn < 1 || cn >= MAXCHARS) return 1000;
    return 1000 + max(0, ch[cn].value[0][V_BODYCONTROL]) * combat_body_control;
}

int clarity_permille(int cn) {
    if (cn < 1 || cn >= MAXCHARS) return 1000;
    return 1000 + max(0, ch[cn].value[0][V_CLARITY]) * combat_clarity;
}

static int body_control(int cn, int lvl) {
    return (long long)lvl * body_control_permille(cn) / 1000;
}

static int clarity(int cn, int val) {
    return (long long)val * clarity_permille(cn) / 1000;
}

static int weapon_skill(int cn) {
    return get_fight_skill(cn);
}

static int tactics_boost(int cn) {
    return ch[cn].value[0][V_TACTICS] * combat_tactics_boost / 100;
}

int melee_attack_level(int cn) {
    return body_control(cn, ch[cn].value[0][V_ATTACK] + tactics_boost(cn) + agility_boost(cn));
}

int melee_strength_level(int cn) {
    return body_control(cn, ch[cn].value[0][V_STR] + tactics_boost(cn));
}

int melee_defence_level(int cn) {
    return body_control(cn, ch[cn].value[0][V_PARRY] + tactics_boost(cn) + agility_boost(cn));
}

int melee_surround_level(int cn) {
    if (!ch[cn].value[1][V_SURROUND]) return 0;
    return body_control(cn, (weapon_skill(cn) + ch[cn].value[0][V_SURROUND]) / 2 + tactics_boost(cn));
}

// RuneScape: attack roll = (level + 8) x (accuracy bonus + 64), defence roll likewise; vcn and vco
// are the effective levels after situation modifiers (side, back, dark, swamp, rage)
int melee_hit_chance(int cn, int co, int vcn, int vco) {
    double att, def, p;

    att = (double)(max(0, vcn) + 8) * max(1, ch[cn].value[0][V_MELEE_ACC] + 64);
    def = (double)(max(0, vco) + 8) * max(1, ch[co].value[0][V_MELEE_DEF] + 64);

    if (att > def) p = 1.0 - (def + 2.0) / (2.0 * (att + 1.0));
    else p = att / (2.0 * (def + 1.0));

    return (int)(p * 10000);
}

// RuneScape: max hit = 0.5 + (strength level + 8) x (strength bonus + 64) / 640, in hitpoints
int melee_max_hit(int cn) {
    return POWERSCALE / 2 + (long long)(melee_strength_level(cn) + 8) * max(0, ch[cn].value[0][V_MELEE_STR] + 64) * POWERSCALE / 640;
}

int tactics2skill(int val) {
    return val * 15 / 100;
}

// spell strength: the spell's level plus Intuition (spells only, not Warcry)
int spellpower(int cn, int v) {
    if (v == V_BLESS || v == V_HEAL || v == V_FREEZE || v == V_MAGICSHIELD || v == V_FLASH || v == V_FIRE) {
        if (!ch[cn].value[0][v]) return 0;
        return ch[cn].value[0][v] + intuition_boost(cn);
    }
    return ch[cn].value[0][v];
}

int get_parry_skill(int cn) {
    int val;

    if (ch[cn].value[1][V_PARRY]) val = get_fight_skill(cn) + ch[cn].value[0][V_PARRY] * 2;
    else if (ch[cn].flags & CF_EDEMON) val = get_fight_skill(cn) * 3.5;
    else if (ch[cn].value[1][V_MAGICSHIELD]) val = get_fight_skill(cn) + ch[cn].value[0][V_MAGICSHIELD] * 2 - min(10, ch[cn].level / 2);
    else val = get_fight_skill(cn) + get_spell_average(cn) * 2;

    return val;
}

int get_attack_skill(int cn) {
    int val;

    if (ch[cn].value[1][V_ATTACK]) val = get_fight_skill(cn) + ch[cn].value[0][V_ATTACK] * 2;
    else if (ch[cn].flags & CF_EDEMON) val = get_fight_skill(cn) * 3.5;
    else {
        val = get_fight_skill(cn) + get_spell_average(cn) * 2 - ch[cn].level * 3 - 20;
        if (val < 0) val = 0;
    }

    return val;
}

int get_surround_attack_skill(int cn) {
    int val;

    if (ch[cn].value[1][V_SURROUND]) val = get_fight_skill(cn) +
        ch[cn].value[0][V_SURROUND] * 2 - 20;
    else return 0;

    return val;
}

int skilldiff2percent(int diff) {
    if (diff < -62) return 10;
    if (diff < -60) return 11;
    if (diff < -58) return 12;
    if (diff < -56) return 13;
    if (diff < -54) return 14;
    if (diff < -52) return 15;
    if (diff < -50) return 16;
    if (diff < -48) return 17;
    if (diff < -46) return 18;
    if (diff < -44) return 19;
    if (diff < -42) return 20;
    if (diff < -40) return 21;
    if (diff < -38) return 22;
    if (diff < -36) return 23;
    if (diff < -34) return 24;
    if (diff < -32) return 25;
    if (diff < -30) return 26;
    if (diff < -28) return 27;
    if (diff < -26) return 28;
    if (diff < -24) return 29;
    if (diff < -22) return 30;

    if (diff > 62) return 90;
    if (diff > 60) return 89;
    if (diff > 58) return 88;
    if (diff > 56) return 87;
    if (diff > 54) return 86;
    if (diff > 52) return 85;
    if (diff > 50) return 84;
    if (diff > 48) return 83;
    if (diff > 46) return 82;
    if (diff > 44) return 81;
    if (diff > 42) return 80;
    if (diff > 40) return 79;
    if (diff > 38) return 78;
    if (diff > 36) return 77;
    if (diff > 34) return 76;
    if (diff > 32) return 75;
    if (diff > 30) return 74;
    if (diff > 28) return 73;
    if (diff > 26) return 72;
    if (diff > 24) return 71;
    if (diff > 22) return 70;

    switch (diff) {
    case -22:
        return 31;
    case -21:
        return 31;
    case -20:
        return 32;
    case -19:
        return 32;
    case -18:
        return 33;
    case -17:
        return 33;
    case -16:
        return 34;
    case -15:
        return 34;
    case -14:
        return 35;
    case -13:
        return 35;
    case -12:
        return 36;
    case -11:
        return 36;
    case -10:
        return 37;
    case -9:
        return 37;
    case -8:
        return 38;
    case -7:
        return 38;
    case -6:
        return 39;
    case -5:
        return 39;
    case -4:
        return 40;
    case -3:
        return 42;
    case -2:
        return 44;
    case -1:
        return 47;
    case 0:
        return 50;
    case 1:
        return 53;
    case 2:
        return 56;
    case 3:
        return 59;
    case 4:
        return 60;
    case 5:
        return 61;
    case 6:
        return 61;
    case 7:
        return 62;
    case 8:
        return 62;
    case 9:
        return 63;
    case 10:
        return 63;
    case 11:
        return 64;
    case 12:
        return 64;
    case 13:
        return 65;
    case 14:
        return 65;
    case 15:
        return 66;
    case 16:
        return 66;
    case 17:
        return 67;
    case 18:
        return 67;
    case 19:
        return 68;
    case 20:
        return 68;
    case 21:
        return 69;
    case 22:
        return 69;
    default:
        btrace("impossible in skilldiff2percent");
        return 50;
    }
}

// reduce skill_strength according to subjects immunity
// skill_strength is in the usual bounds (about 7...200)
// subject's immunity will be about the same
int immunity_reduction(int caster, int subject, int skill_strength) {
    int immun, stren, diff, dam;
    extern int showspell;

    immun = immunity_value(subject);
    if (ch[subject].value[1][V_RAGE]) immun += ch[subject].rage / POWERSCALE / RAGEMOD;

    // equipment: magic accuracy raises the caster's side, magic defence the target's, the way
    // accuracy and defence bonuses scale the melee rolls (x (bonus + 64) / 64)
    if (caster > 0 && caster < MAXCHARS) skill_strength = skill_strength * max(1, ch[caster].value[0][V_MAGIC_ACC] + 64) / 64;
    immun = immun * max(1, ch[subject].value[0][V_MAGIC_DEF] + 64) / 64;
    // Clarity (step 15): raises the caster's spell strength and the subject's immunity
    if (caster > 0 && caster < MAXCHARS) skill_strength = clarity(caster, skill_strength);
    immun = clarity(subject, immun);

    diff = skill_strength - immun;

    dam = skilldiff2percent(diff);

    stren = (dam / 100.0) * POWERSCALE;

    if (showspell) say(caster, "skill=%d, immun=%d, diff=%d, stren=%.2f", skill_strength, immun, diff, stren / 1000.0);

    return stren;
}

// returns the resulting speed reduction for cn casting freeze on co
// str>=0 means freeze would fail.
int freeze_value(int cn, int co) {
    int str, dam, diff, immun;

    immun = immunity_value(co);
    if (ch[co].value[1][V_RAGE]) immun += ch[co].rage / POWERSCALE / RAGEMOD;

    str = clarity(cn, spellpower(cn, V_FREEZE));
    immun = clarity(co, immun);

    diff = str - immun;
    if (diff < -50) return 0;

    dam = skilldiff2percent(diff);

    str = dam;

    if ((ch[cn].flags & CF_IDEMON) && ch[co].value[0][V_COLD] > ch[cn].value[1][V_DEMON]) {
        str -= (ch[co].value[0][V_COLD] - ch[cn].value[1][V_DEMON]) * 10;
    }

    return -str;
}

// returns the resulting speed reduction for cn casting warcry on co
// str>=0 means warcry would fail.
int warcry_value(int cn, int co, int pwr) {
    int str, immun, diff, dam;

    immun = immunity_value(co);
    if (ch[co].value[1][V_RAGE]) immun += ch[co].rage / POWERSCALE / RAGEMOD;

    diff = pwr - immun;
    if (diff < -50) return 0;

    dam = skilldiff2percent(diff);
    str = dam;

    return -str * (1.0 + str / 400.0);
    ;
}

int warcry_damage(int cn, int co, int pwr) {
    return immunity_reduction(cn, co, pwr);
}

// Fire and Lightning damage, raised by the caster's Magic Damage % from gear (at most
// combat_magic_damage_cap) and by Clarity
static int spell_damage(int cn, int co, int str) {
    int pct = 100;

    if (cn > 0 && cn < MAXCHARS) pct += max(-100, min(combat_magic_damage_cap, ch[cn].value[0][V_MAGIC_DMG]));
    return immunity_reduction(cn, co, str) * FIREBALL_DAMAGE * (1.0 + str / 400.0) * pct / 100.0 * clarity_permille(cn) / 1000.0;
}

int fireball_damage(int cn, int co, int str) {
    return spell_damage(cn, co, str);
}

// total damage of a lightning ball or flash over its whole life: the same as
// one fireball (add_strike() in effect.c spreads it over the strikes)
int strike_damage(int cn, int co, int str) {
    return spell_damage(cn, co, str);
}

// ---------- effects breakdown (client Effects tab, /effects) ----------

void effect_add(struct effect_list *e, int group, const char *label, const char *desc, const char *fmt, ...) {
    va_list args;

    if (e->n >= MAXEFFECTLINES) return;
    e->group[e->n] = group;
    snprintf(e->label[e->n], sizeof(e->label[0]), "%s", label);
    snprintf(e->desc[e->n], sizeof(e->desc[0]), "%s", desc ? desc : "");
    va_start(args, fmt);
    vsnprintf(e->value[e->n], sizeof(e->value[0]), fmt, args);
    va_end(args);
    e->n++;
}

static double permille_pct(int permille) {
    return (permille - 1000) / 10.0;
}

static void spell_line(int cn, struct effect_list *e, int v, const char *label, const char *what) {
    char desc[160];

    if (!ch[cn].value[1][v]) return;
    snprintf(desc, sizeof(desc), "%s power: your Magic level (%d). %s", label, ch[cn].value[1][v], what);
    effect_add(e, EG_MAGIC, label, desc, "%d", spellpower(cn, v));
}

void skilling_effects(int cn, struct effect_list *e); // skilling.c: gathering lines

void effects_build(int cn, struct effect_list *e) {
    int in, sk, id, dmg;
    char desc[160];

    e->n = 0;

    // melee
    effect_add(e, EG_MELEE, "Accuracy level", "Melee hit chance: your Attack level.", "%d", melee_attack_level(cn));
    effect_add(e, EG_MELEE, "Strength level", "Melee max hit: your Strength level.", "%d", melee_strength_level(cn));
    effect_add(e, EG_MELEE, "Defence level", "Chance to avoid melee hits: your Defence level.", "%d", melee_defence_level(cn));
    effect_add(e, EG_MELEE, "Max hit", "Highest damage one melee hit can do, in HP, before the enemy's armour.", "%.1f", melee_max_hit(cn) / (double)POWERSCALE);
    effect_add(e, EG_MELEE, "Gear accuracy", "Melee accuracy bonus from your equipment (raises hit chance).", "%+d", ch[cn].value[0][V_MELEE_ACC]);
    effect_add(e, EG_MELEE, "Gear strength", "Melee strength bonus from your equipment (raises max hit).", "%+d", ch[cn].value[0][V_MELEE_STR]);
    effect_add(e, EG_MELEE, "Gear defence", "Melee defence bonus from your equipment (lowers the enemy's hit chance).", "%+d", ch[cn].value[0][V_MELEE_DEF]);
    in = ch[cn].item[WN_RHAND];
    if (in && (sk = item_boost_skill(in)) >= 0 && (id = skill_for_value(sk)) >= 0) {
        snprintf(desc, sizeof(desc), "Your %s level raises the stats of the weapon you hold by %.1f%% per level, on you only.", skilldef[id].name, combat_weapon_skill_percent / 10.0);
        effect_add(e, EG_MELEE, "Weapon bonus", desc, "%+.1f%%", permille_pct(item_skill_permille(cn, sk)));
    }
    if (ch[cn].value[1][V_ARMORSKILL]) {
        snprintf(desc, sizeof(desc), "Your Defence level raises the stats of the armour you wear by %.1f%% per level, on you only.", combat_armor_class_percent / 10.0);
        effect_add(e, EG_MELEE, "Armor bonus", desc, "%+.1f%%", permille_pct(item_skill_permille(cn, V_ARMORSKILL)));
    }
    if (ch[cn].value[1][V_BODYCONTROL]) {
        snprintf(desc, sizeof(desc), "Your Body Control level raises your melee accuracy, strength and defence levels by %.1f%% per level.", combat_body_control / 10.0);
        effect_add(e, EG_MELEE, "Body Control", desc, "%+.1f%%", permille_pct(body_control_permille(cn)));
    }
    if (ch[cn].value[1][V_TACTICS]) effect_add(e, EG_MELEE, "Tactics", "Your Tactics level adds to your melee accuracy, strength and defence levels.", "%+d", tactics_boost(cn));
    snprintf(desc, sizeof(desc), "Armour from your gear. Each melee hit on you is reduced by up to %.1f HP, at most %d%% of the hit.", ch[cn].value[0][V_ARMOR] / 20.0 / ATTACK_DIV, combat_armor_soak);
    effect_add(e, EG_MELEE, "Armor", desc, "%d", ch[cn].value[0][V_ARMOR]);
    snprintf(desc, sizeof(desc), "Movement and attack speed. Athletics adds %d%% of its level (+%d now); heavy gear can lower it.", athletics_speed, ch[cn].value[0][V_SPEEDSKILL] * athletics_speed / 100);
    effect_add(e, EG_MELEE, "Speed", desc, "%d", ch[cn].value[0][V_SPEED]);

    // magic
    spell_line(cn, e, V_FIRE, "Fire", "Raises fire damage and fireball hit chance.");
    spell_line(cn, e, V_FLASH, "Lightning", "Raises lightning damage and hit chance.");
    spell_line(cn, e, V_FREEZE, "Freeze", "Raises how much Freeze slows enemies.");
    spell_line(cn, e, V_HEAL, "Heal", "Raises how many HP Heal restores.");
    spell_line(cn, e, V_BLESS, "Bless", "Raises Bless strength.");
    spell_line(cn, e, V_MAGICSHIELD, "Magic Shield", "Raises the Magic Shield's strength.");
    effect_add(e, EG_MAGIC, "Magic accuracy", "Spell accuracy bonus from your equipment (raises spell hit chance).", "%+d", ch[cn].value[0][V_MAGIC_ACC]);
    dmg = max(-100, min(combat_magic_damage_cap, ch[cn].value[0][V_MAGIC_DMG]));
    snprintf(desc, sizeof(desc), "Spell damage bonus from your equipment, at most %d%%.", combat_magic_damage_cap);
    effect_add(e, EG_MAGIC, "Spell damage", desc, "%+d%%", dmg);
    effect_add(e, EG_MAGIC, "Magic defence", "Magic defence from your equipment (lowers enemy spell hit chance). Melee armour lowers it.", "%+d", ch[cn].value[0][V_MAGIC_DEF]);
    if (ch[cn].value[1][V_CLARITY]) {
        snprintf(desc, sizeof(desc), "Your Clarity level raises spell power, spell damage and immunity by %.1f%% per level.", combat_clarity / 10.0);
        effect_add(e, EG_MAGIC, "Clarity", desc, "%+.1f%%", permille_pct(clarity_permille(cn)));
    }
    snprintf(desc, sizeof(desc), "Resistance to enemy spells: %d%% of your Defence level, raised by magic gear.", immunity_defence);
    effect_add(e, EG_MAGIC, "Spell resist", desc, "%d", immunity_value(cn));
    effect_add(e, EG_MAGIC, "Mana regen", "Mana regained while resting: 7 plus your Regenerate level (HP regen works the same).", "%d", 7 + ch[cn].value[0][V_MEDITATE] + wisdom_boost(cn));

    // gathering
    skilling_effects(cn, e);

    // stealth
    snprintf(desc, sizeof(desc), "How well you see hidden characters. Now in %s mode: stealth mode raises it by %d%%, fast mode lowers it by %d%%.",
             ch[cn].speed_mode == SM_STEALTH ? "stealth" : ch[cn].speed_mode == SM_FAST ? "fast" : "normal", perception_stealth, perception_fast);
    effect_add(e, EG_STEALTH, "Perception", desc, "%d", perception_value(cn));
    effect_add(e, EG_STEALTH, "Stealth", "How well you stay hidden in stealth mode. Moving in stealth mode drains endurance.", "%d", ch[cn].value[0][V_STEALTH]);
}
