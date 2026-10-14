/*
 * Part of Astonia Server 3.5 (c) Daniel Brockhaus. Please read license.txt.
 */

#include <stdlib.h>
#include "server.h"
#include "balance.h"
#include "los.h"
#include "log.h"
#include "date.h"
#include "btrace.h"
#include "see.h"
#include "skillreg.h"
#include "player.h"

// Perception XP (George 2026-10-13, step 18): a player who sees a character in stealth mode
// (further than 2 tiles, so the stealth roll mattered) gets Perception use_xp scaled by the
// hider's Stealth, +50% while in stealth mode himself. The same hider counts again only after
// SPOT_COOLDOWN, so standing next to one does not farm XP.
#define SPOT_SLOTS 8
#define SPOT_COOLDOWN (TICKS * 60)

static struct {
    int co;
    unsigned int serial;
    int when;
} spotted[MAXPLAYER][SPOT_SLOTS];

static void perception_spot(int cn, int co, int stealth) {
    int nr = ch[cn].player, n, old = 0, id, xp;

    if (nr < 1 || nr >= MAXPLAYER || (ch[co].flags & CF_GOD)) return;
    if ((id = skill_for_value(V_PERCEPT)) < 0) return;

    for (n = 0; n < SPOT_SLOTS; n++) {
        if (spotted[nr][n].co == co && spotted[nr][n].serial == ch[co].serial) {
            if (ticker - spotted[nr][n].when < SPOT_COOLDOWN) return;
            old = n;
            break;
        }
        if (spotted[nr][n].when < spotted[nr][old].when) old = n;
    }
    spotted[nr][old].co = co;
    spotted[nr][old].serial = ch[co].serial;
    spotted[nr][old].when = ticker;

    xp = skilldef[id].use_xp * (stealth + 10) / 20;
    if (ch[cn].speed_mode == SM_STEALTH) xp = xp * 3 / 2;
    add_skill_xp(cn, id, xp);
}

static inline int check_lightm(int m) {

    return min(255, max(map[m].light, (dlight * map[m].dlight) / 256));
}

int char_see_char_nolos(int cn, int co) {
    int m, dist, light, stealth, vcn, vco;

    if (cn < 1 || cn >= MAXCHARS) {
        elog("char_see_char_nolos: illegal cn %d", cn);
        return 0;
    }
    if (co == 0) return 0;
    if (co < 1 || co >= MAXCHARS) {
        elog("char_see_char_nolos: illegal co %d", co);
        return 0;
    }

    if (!(ch[co].flags)) return 0;

    if (cn == co) return 1;

    if (ch[co].flags & CF_INVISIBLE) return 0;

    if ((ch[cn].flags & (CF_GOD | CF_INFRARED)) == (CF_GOD | CF_INFRARED)) return 1;

    dist = max(abs(ch[cn].x - ch[co].x), abs(ch[cn].y - ch[co].y)) + 1;

    m = ch[co].x + ch[co].y * MAXMAP;

    light = check_lightm(m);
    if (ch[cn].flags & (CF_INFRARED | CF_INFRAVISION)) light = max(32, light);

    if (!light && (abs(ch[cn].x - ch[co].x) > 1 || abs(ch[cn].y - ch[co].y) > 1)) return 0;

    if (dist < 3) return 1;
    dist = dist * dist;

    light = max(32 - light, 0) * 2;

    if (ch[co].speed_mode == SM_STEALTH) stealth = ch[co].value[0][V_STEALTH];
    else stealth = 0;

    // 0...64        4..400
    if (stealth) vco = light + stealth + dist;
    else vco = 0;

    vcn = perception_value(cn) + 16 + 49; // higher in stealth mode, lower in fast mode (step 16)

    if (vcn >= vco) {
        if (stealth && (ch[cn].flags & CF_PLAYER)) perception_spot(cn, co, stealth);
        return 1;
    }

    return 0;
}

int char_see_char(int cn, int co) {
    if (cn < 1 || cn >= MAXCHARS) {
        elog("char_see_char: illegal cn %d", cn);
        btrace("char_see_char");
        return 0;
    }
    if (co == 0) return 0;
    if (co < 1 || co >= MAXCHARS) {
        elog("char_see_char: illegal co %d, cn=%s (%d)", co, ch[cn].name, cn);
        btrace("char_see_char");
        return 0;
    }

    if (!(ch[co].flags)) return 0;

    if (cn == co) return 1;

    if (ch[co].flags & CF_INVISIBLE) return 0;

    if (!los_can_see(cn, ch[cn].x, ch[cn].y, ch[co].x, ch[co].y, DISTMAX)) return 0;

    return char_see_char_nolos(cn, co);
}

int char_see_item(int cn, int in) {
    int m, light;

    if (!(it[in].flags)) return 0;

    if (it[in].carried) return 0;

    if (!los_can_see(cn, ch[cn].x, ch[cn].y, it[in].x, it[in].y, DISTMAX)) return 0;

    if ((it[in].flags & IF_FRONTWALL) &&
        ((map[it[in].x + 1 + it[in].y * MAXMAP].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK)) || !los_can_see(cn, ch[cn].x, ch[cn].y, it[in].x + 1, it[in].y, DISTMAX)) &&
        ((map[it[in].x + it[in].y * MAXMAP + MAXMAP].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK)) || !los_can_see(cn, ch[cn].x, ch[cn].y, it[in].x, it[in].y + 1, DISTMAX)))
        return 0;

    if (!(it[in].flags & IF_TAKE)) return 1;

    m = it[in].x + it[in].y * MAXMAP;

    light = check_lightm(m);
    if (ch[cn].flags & (CF_INFRARED | CF_INFRAVISION)) light = max(32, light);
    if (light < 1) return 0;

    return 1;
}

// What the client of player slot nr is shown on its map. Within DISTMAX this is exactly
// what the character sees. Protocol 4 clients get a bigger map (DISTVIEW); further out
// the player's view LOS (vlos, kept current by player_map) replaces the game LOS.
static int far_from(int cn, int x, int y) {
    return abs(ch[cn].x - x) >= DISTMAX || abs(ch[cn].y - y) >= DISTMAX;
}

// only trust vlos if player_map built it for the character's current position
static int vlos_current(int nr, int cn) {
    return nr > 0 && nr < MAXVLOS && vlos[nr].x == ch[cn].x && vlos[nr].y == ch[cn].y;
}

int player_see_char(int nr, int cn, int co) {
    if (!far_from(cn, ch[co].x, ch[co].y)) return char_see_char(cn, co);

    if (!vlos_current(nr, cn)) return 0;
    if (co < 1 || co >= MAXCHARS || !(ch[co].flags)) return 0;
    if (ch[co].flags & CF_INVISIBLE) return 0;
    if (!vlos_can_see(nr, ch[co].x, ch[co].y)) return 0;

    return char_see_char_nolos(cn, co);
}

int player_see_item(int nr, int cn, int in) {
    int m, light;

    if (!far_from(cn, it[in].x, it[in].y)) return char_see_item(cn, in);

    if (!vlos_current(nr, cn)) return 0;
    if (!(it[in].flags)) return 0;
    if (it[in].carried) return 0;
    if (!vlos_can_see(nr, it[in].x, it[in].y)) return 0;

    if ((it[in].flags & IF_FRONTWALL) &&
        ((map[it[in].x + 1 + it[in].y * MAXMAP].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK)) || !vlos_can_see(nr, it[in].x + 1, it[in].y)) &&
        ((map[it[in].x + it[in].y * MAXMAP + MAXMAP].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK)) || !vlos_can_see(nr, it[in].x, it[in].y + 1)))
        return 0;

    if (!(it[in].flags & IF_TAKE)) return 1;

    m = it[in].x + it[in].y * MAXMAP;

    light = check_lightm(m);
    if (ch[cn].flags & (CF_INFRARED | CF_INFRAVISION)) light = max(32, light);
    if (light < 1) return 0;

    return 1;
}
