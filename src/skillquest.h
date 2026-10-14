/*
 * Skilling quests and the daily task board, see skillquest.c and zones/generic/skillquests.cfg.
 */

int skillquest_load(void); // after skilling_load (uses its keys) and skills_load
int skillquest_driver(int type, int cn, intptr_t ret, int lastact); // character driver CDR_SKILLQUEST
void skillquest_command(int cn, char *args); // /tasks
