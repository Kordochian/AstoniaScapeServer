#!/usr/bin/env python3
# step 14 (armour requirement Armor Class since step 15): gives the standard weapon and armour templates RuneScape-style equipment bonuses and
# a level requirement. usage: gen14.py in_dir out_dir   (weapons.itm, armor.itm)
import re, sys

def level(t, q):            # requirement level: tier 1-10, material q1 iron / q2 steel / q3 mithril
    return round((t - 1) * 7.5) + 1 + (q - 1) * 5

def weapon(typ, L):
    a, s = L + 4, L + 5     # one-handed sword (RuneScape bronze sword +4 / +5)
    if typ == 'sword':     return 'V_SWORD',   [('V_MELEE_ACC', a), ('V_MELEE_STR', s)]
    if typ == 'dagger':    return 'V_DAGGER',  [('V_MELEE_ACC', L + 8), ('V_MELEE_STR', round(s * 0.8))]
    if typ == 'twohanded': return 'V_TWOHAND', [('V_MELEE_ACC', round(a * 1.8)), ('V_MELEE_STR', round(s * 1.8))]
    if typ == 'staff':     return 'V_STAFF',   [('V_MELEE_ACC', round(a * 0.6)), ('V_MELEE_STR', round(s * 0.8)),
                                                ('V_MAGIC_ACC', round(a * 0.5)), ('V_MAGIC_DMG', 1 + L // 10)]
SHARE = {'armor': 1.0, 'leggings': 0.6, 'helmet': 0.35, 'sleeves': 0.25}
def armour(typ, L):         # body defence about RuneScape's platebody: 1.8 x level + 15
    # melee armour: Magic Defence goes down (step 15, George: plate is for melee, robes for magic)
    return 'V_ARMORSKILL', [('V_MELEE_DEF', max(1, round((1.8 * L + 15) * SHARE[typ]))),
                            ('V_MAGIC_DEF', -max(1, round((0.15 * L + 3) * SHARE[typ])))]

def fix(text, kind):
    out, n = [], 0
    for blk in re.split(r'(?m)^(?=[a-z_0-9]+:$)', text):
        m = re.match(r'([a-z]+)(\d+)q(\d):\n', blk)
        if m and (m.group(1) in SHARE if kind == 'armor' else m.group(1) in ('sword', 'dagger', 'twohanded', 'staff')):
            typ, t, q = m.group(1), int(m.group(2)), int(m.group(3))
            L = level(t, q)
            req, bon = (armour if kind == 'armor' else weapon)(typ, L)
            # drop old weapon value / defense mods on weapons (no longer used), keep armor + speed on armour
            mods = re.findall(r'mod_index=(\w+)\nmod_value=(-?\d+)\n', blk)
            keep = [(i, v) for i, v in mods if kind == 'armor' and i in ('V_ARMOR', 'V_SPEED')]
            blk = re.sub(r'mod_index=\w+\nmod_value=-?\d+\n', '', blk)
            lines = ''.join('mod_index=%s\nmod_value=%s\n' % kv for kv in keep + bon)
            lines += 'req_index=%s\nreq_value=%d\n' % (req, L)
            assert len(keep) + len(bon) + 1 <= 5
            blk = blk.replace('flag=IF_TAKE\n', lines + 'flag=IF_TAKE\n', 1)
            n += 1
        out.append(blk)
    return ''.join(out), n

for f, kind in (('weapons.itm', 'weapon'), ('armor.itm', 'armor')):
    t = open(sys.argv[1] + '/' + f).read().replace('\r', '')
    t, n = fix(t, kind)
    open(sys.argv[2] + '/' + f, 'w').write(t)
    print(f, n, 'templates')
