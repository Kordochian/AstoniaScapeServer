/*
 * Gathering and production skills (Mining -> Smithing, Woodcutting -> Crafting).
 * Everything is defined in zones/generic/skilling.cfg, see the header of that file.
 */

#define DEV_ID_SKILLING 0x52 // item IDs of skilling items: MAKE_ITEMID(DEV_ID_SKILLING, def id)

int skilling_load(void); // after the item templates are loaded
int skilling_item_driver(int nr, int in, int cn);
int skilling_command(int cn, char *cmd, char *args); // /smelt, /smith, /craft
void skilling_fight(int cn); // cn made or took a melee attack (pauses Stealth walking XP)
void skilling_walk_xp(int cn); // XP for one step by speed mode (walk_xp ... in config:)
int skilling_spell_xp(int v); // Magic XP per cast of the spell in value slot v (spell_xp_* in config:), -1 = not a spell

// things a player carries (resource stacks, tools, items a recipe makes), by skilling.cfg key; used by skillquest.c
int skilling_thing(char *key); // -1 = unknown / not carryable
int skilling_thing_is_stack(int d);
char *skilling_thing_name(int d);
int skilling_thing_count(int cn, int d);
void skilling_thing_take(int cn, int d, int cnt);
int skilling_thing_room(int cn, int d, int cnt);
int skilling_thing_give(int cn, int d, int cnt);

// furnace / anvil window in the client (protocol 7). Uses a free command number of client.h (was SV_RESERVED3).
// u16 total length, station (1 furnace, 2 anvil), flags (CRAFT_OPEN, CRAFT_NOHAMMER), Smithing level, count,
// then per recipe: nr, level, can make (0-100), xp u16, sprite u32, material (first need, 255 none),
// material count, key, name, needs (strings: u8 length + text);
// then material count, per material: nr, carried u16, name. The client groups anvil recipes by material.
// Protocol 9 (CRAFT_BENCH_VERSION) adds station 3 (workbench), flag CRAFT_GROUP and five strings after the
// materials: window title, command, skill name, tool text ("a hammer"), station name. Older clients get no
// workbench window (text list instead).
// The client makes things by sending "/<command> <key> <count>" (smelt, smith, craft).
#define SV_CRAFTLIST 56
#define CRAFT_CLIENT_VERSION 7
#define CRAFT_OPEN 1 // open the window (else only refresh it if it shows this station)
#define CRAFT_NOHAMMER 2 // the character has no tool for this station (hammer, knife)
#define CRAFT_GROUP 4 // protocol 9: group the recipes by their material (anvil, workbench)
#define CRAFT_BENCH_VERSION 9
