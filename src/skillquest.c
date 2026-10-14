/*
 * Skilling quests and the daily task board (Astonia rebuild).
 * Everything is defined in zones/generic/skillquests.cfg, see the header of that file.
 *
 * Character driver CDR_SKILLQUEST (core, not in a DLL, so it works in every area). The NPC's .chr template
 * has driver=160 and arg="npc=KEY;dir=N;". An NPC with quests (npc=KEY in a quest block) gives them in file
 * order; an NPC whose key is the board= of config: hands out daily tasks.
 *
 * Players talk to the NPC: "quest" (what is there to do), "accept", "done" (hand in), and at the board "task",
 * "done", "cancel". /tasks lists the player's progress.
 *
 * Quest state lives in the questlog (open / done), the daily task in DRD_SKILLQUEST_PPD.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "server.h"
#include "log.h"
#include "notify.h"
#include "do.h"
#include "drdata.h"
#include "see.h"
#include "talk.h"
#include "tool.h"
#include "drvlib.h"
#include "mem.h"
#include "questlog.h"
#include "database.h"
#include "direction.h"
#include "skillreg.h"
#include "skilling.h"
#include "skillquest.h"

#define SKILLQUESTS_CFG "zones/generic/skillquests.cfg"

#define MAXSQ 64
#define MAXTASK 64
#define MAXSQNEED 4
#define MAXSQXP 4
#define TALKDIST 6 // NPCs answer players this close

struct sqdef { // one quest
    char key[32];
    int quest; // questlog number
    char npc[32]; // NPC key that gives it
    char skill_key[32];
    int skill, level; // skill level needed to accept (-1 = none)
    char after_key[32];
    int after; // quest that must be done first, -1 = none

    int nneed;
    char need_key[MAXSQNEED][32];
    int need[MAXSQNEED], needcnt[MAXSQNEED];

    int nxp;
    char xp_key[MAXSQXP][32];
    int xpskill[MAXSQXP], xp[MAXSQXP];

    int gold; // in gold coins
    char reward_key[32];
    int reward, reward_cnt; // skilling thing, -1 = none

    char offer[256], remind[256], thanks[256];
};

struct taskdef { // one daily task
    char key[32];
    int id; // saved in the player's data: never change or reuse
    char skill_key[32];
    int skill, level, maxlevel;
    char need_key[32];
    int need, min, max;
    int xp_each; // XP per item for the task's skill
    int silver_each; // money per item, in silver (100 = 1 gold)
    char text[256]; // what the board NPC says, {count} and {item} are filled in
};

struct skillquest_ppd {
    int day; // the day done_today counts for
    int done_today; // tasks finished or cancelled today
    int task_id; // current task (taskdef id), 0 = none
    int task_count; // how many items it asks for
    int total; // tasks finished ever
    int last_id; // the task before, not given twice in a row
    int reserved[10];
};

struct sqnpc_data {
    int init;
    int dir;
    char npc[32];
    int last_talk;
};

static struct sqdef sq[MAXSQ];
static int nsq = 0;
static struct taskdef task[MAXTASK];
static int ntask = 0;

static char board_npc[32] = "BOARD";
static int board_quest = -1; // questlog number shown while a daily task is open
static int tasks_per_day = 3, reset_hour = 0, board_bonus_gold = 0;

static void scopy(char *dst, char *src, int size) {
    int n;

    for (n = 0; n < size - 1 && src[n]; n++) dst[n] = src[n];
    dst[n] = 0;
}

static void upcase(char *s) {
    for (; *s; s++) *s = toupper(*s);
}

// ---------- config ----------

static int lookup_sq(char *key) {
    int n;

    for (n = 0; n < nsq; n++)
        if (!strcasecmp(sq[n].key, key)) return n;
    return -1;
}

static int parse_cfg(char *text) {
    char *ptr = text, name[64], value[256], blockkey[64], *comma;
    struct sqdef *q = NULL;
    struct taskdef *t = NULL;
    int line = 1, inblock = 0, is_config = 0;

    while ((ptr = skillcfg_line(ptr, name, value, &line))) {
        if (name[0] == '#') continue;
        if (value[0] == '!') {
            elog("%s line %d: syntax error near \"%s\".", SKILLQUESTS_CFG, line, name);
            return 0;
        }
        if (value[0] == ':') {
            if (inblock) {
                elog("%s line %d: missing ; before %s:", SKILLQUESTS_CFG, line, name);
                return 0;
            }
            inblock = 1;
            q = NULL;
            t = NULL;
            scopy(blockkey, name, sizeof(blockkey));
            is_config = !strcasecmp(name, "config");
            continue;
        }
        if (value[0] == ';') {
            if (!inblock) {
                elog("%s line %d: ; without block.", SKILLQUESTS_CFG, line);
                return 0;
            }
            if (!is_config && !q && !t) {
                elog("%s line %d: %s needs type=quest or type=task first.", SKILLQUESTS_CFG, line, blockkey);
                return 0;
            }
            if (q && (q->quest < 0 || q->quest >= MAXQUEST || !q->npc[0] || !q->nneed)) {
                elog("%s: quest %s needs quest= (0-%d), npc= and at least one need=.", SKILLQUESTS_CFG, q->key, MAXQUEST - 1);
                return 0;
            }
            if (t && (t->id < 1 || !t->need_key[0] || !t->skill_key[0])) {
                elog("%s: task %s needs id= (1 or more), skill= and need=.", SKILLQUESTS_CFG, t->key);
                return 0;
            }
            inblock = 0;
            continue;
        }
        if (!inblock) {
            elog("%s line %d: %s outside a block.", SKILLQUESTS_CFG, line, name);
            return 0;
        }

        if (is_config) {
            if (!strcasecmp(name, "tasks_per_day")) tasks_per_day = max(1, atoi(value));
            else if (!strcasecmp(name, "reset_hour")) reset_hour = max(0, min(23, atoi(value)));
            else if (!strcasecmp(name, "board")) {
                scopy(board_npc, value, sizeof(board_npc));
                upcase(board_npc);
            } else if (!strcasecmp(name, "board_quest")) board_quest = atoi(value);
            else if (!strcasecmp(name, "bonus_gold")) board_bonus_gold = max(0, atoi(value));
            else {
                elog("%s line %d: unknown config setting %s.", SKILLQUESTS_CFG, line, name);
                return 0;
            }
            continue;
        }

        if (!strcasecmp(name, "type")) {
            if (q || t) {
                elog("%s line %d: type= must come first and only once.", SKILLQUESTS_CFG, line);
                return 0;
            }
            if (!strcasecmp(value, "quest")) {
                if (nsq >= MAXSQ) {
                    elog("%s: more than %d quests.", SKILLQUESTS_CFG, MAXSQ);
                    return 0;
                }
                q = sq + nsq++;
                bzero(q, sizeof(*q));
                scopy(q->key, blockkey, sizeof(q->key));
                q->quest = -1;
                q->skill = q->after = q->reward = -1;
            } else if (!strcasecmp(value, "task")) {
                if (ntask >= MAXTASK) {
                    elog("%s: more than %d tasks.", SKILLQUESTS_CFG, MAXTASK);
                    return 0;
                }
                t = task + ntask++;
                bzero(t, sizeof(*t));
                scopy(t->key, blockkey, sizeof(t->key));
                t->skill = t->need = -1;
                t->min = t->max = 1;
                t->maxlevel = 999;
            } else {
                elog("%s line %d: type=%s, expected quest or task.", SKILLQUESTS_CFG, line, value);
                return 0;
            }
            continue;
        }
        if (!q && !t) {
            elog("%s line %d: %s needs type=quest or type=task first.", SKILLQUESTS_CFG, line, blockkey);
            return 0;
        }

        comma = strchr(value, ',');
        if (q) {
            if (!strcasecmp(name, "quest")) q->quest = atoi(value);
            else if (!strcasecmp(name, "npc")) {
                scopy(q->npc, value, sizeof(q->npc));
                upcase(q->npc);
            } else if (!strcasecmp(name, "skill")) scopy(q->skill_key, value, sizeof(q->skill_key));
            else if (!strcasecmp(name, "level")) q->level = atoi(value);
            else if (!strcasecmp(name, "after")) scopy(q->after_key, value, sizeof(q->after_key));
            else if (!strcasecmp(name, "need")) { // need=KEY,count
                if (q->nneed >= MAXSQNEED) {
                    elog("%s line %d: at most %d need= per quest.", SKILLQUESTS_CFG, line, MAXSQNEED);
                    return 0;
                }
                if (comma) *comma = 0;
                scopy(q->need_key[q->nneed], value, sizeof(q->need_key[0]));
                q->needcnt[q->nneed] = comma ? max(1, atoi(comma + 1)) : 1;
                q->nneed++;
            } else if (!strcasecmp(name, "xp")) { // xp=SKILL,amount
                if (q->nxp >= MAXSQXP) {
                    elog("%s line %d: at most %d xp= per quest.", SKILLQUESTS_CFG, line, MAXSQXP);
                    return 0;
                }
                if (!comma) {
                    elog("%s line %d: xp=SKILL,amount.", SKILLQUESTS_CFG, line);
                    return 0;
                }
                *comma = 0;
                scopy(q->xp_key[q->nxp], value, sizeof(q->xp_key[0]));
                q->xp[q->nxp] = max(0, atoi(comma + 1));
                q->nxp++;
            } else if (!strcasecmp(name, "gold")) q->gold = max(0, atoi(value));
            else if (!strcasecmp(name, "reward")) { // reward=KEY[,count]
                if (comma) *comma = 0;
                scopy(q->reward_key, value, sizeof(q->reward_key));
                q->reward_cnt = comma ? max(1, atoi(comma + 1)) : 1;
            } else if (!strcasecmp(name, "offer")) scopy(q->offer, value, sizeof(q->offer));
            else if (!strcasecmp(name, "remind")) scopy(q->remind, value, sizeof(q->remind));
            else if (!strcasecmp(name, "thanks")) scopy(q->thanks, value, sizeof(q->thanks));
            else {
                elog("%s line %d: unknown quest setting %s.", SKILLQUESTS_CFG, line, name);
                return 0;
            }
        } else {
            if (!strcasecmp(name, "id")) t->id = atoi(value);
            else if (!strcasecmp(name, "skill")) scopy(t->skill_key, value, sizeof(t->skill_key));
            else if (!strcasecmp(name, "level")) t->level = atoi(value);
            else if (!strcasecmp(name, "maxlevel")) t->maxlevel = atoi(value);
            else if (!strcasecmp(name, "need")) { // need=KEY,min[,max]
                char *c2;

                if (comma) *comma = 0;
                scopy(t->need_key, value, sizeof(t->need_key));
                t->min = t->max = comma ? max(1, atoi(comma + 1)) : 1;
                if (comma && (c2 = strchr(comma + 1, ','))) t->max = max(t->min, atoi(c2 + 1));
            } else if (!strcasecmp(name, "xp_each")) t->xp_each = max(0, atoi(value));
            else if (!strcasecmp(name, "silver_each")) t->silver_each = max(0, atoi(value));
            else if (!strcasecmp(name, "text")) scopy(t->text, value, sizeof(t->text));
            else {
                elog("%s line %d: unknown task setting %s.", SKILLQUESTS_CFG, line, name);
                return 0;
            }
        }
    }
    if (inblock) {
        elog("%s: missing ; at the end.", SKILLQUESTS_CFG);
        return 0;
    }
    return 1;
}

static int resolve(void) {
    int n, m;
    struct sqdef *q;
    struct taskdef *t;

    for (n = 0; n < nsq; n++) {
        q = sq + n;
        for (m = 0; m < n; m++) {
            if (sq[m].quest == q->quest) {
                elog("%s: %s and %s use the same quest number %d.", SKILLQUESTS_CFG, sq[m].key, q->key, q->quest);
                return 0;
            }
        }
        if (q->quest == board_quest) {
            elog("%s: %s uses the task board's quest number %d.", SKILLQUESTS_CFG, q->key, q->quest);
            return 0;
        }
        if (q->skill_key[0] && (q->skill = skill_lookup_key(q->skill_key)) == -1) {
            elog("%s: %s: skill=%s is not in skills.cfg.", SKILLQUESTS_CFG, q->key, q->skill_key);
            return 0;
        }
        if (q->after_key[0] && (q->after = lookup_sq(q->after_key)) == -1) {
            elog("%s: %s: after=%s is not a quest in this file.", SKILLQUESTS_CFG, q->key, q->after_key);
            return 0;
        }
        for (m = 0; m < q->nneed; m++) {
            if ((q->need[m] = skilling_thing(q->need_key[m])) == -1) {
                elog("%s: %s: need=%s is not a resource, tool or recipe in skilling.cfg.", SKILLQUESTS_CFG, q->key, q->need_key[m]);
                return 0;
            }
        }
        for (m = 0; m < q->nxp; m++) {
            if ((q->xpskill[m] = skill_lookup_key(q->xp_key[m])) == -1) {
                elog("%s: %s: xp=%s is not in skills.cfg.", SKILLQUESTS_CFG, q->key, q->xp_key[m]);
                return 0;
            }
        }
        if (q->reward_key[0] && (q->reward = skilling_thing(q->reward_key)) == -1) {
            elog("%s: %s: reward=%s is not a resource, tool or recipe in skilling.cfg.", SKILLQUESTS_CFG, q->key, q->reward_key);
            return 0;
        }
    }
    for (n = 0; n < ntask; n++) {
        t = task + n;
        for (m = 0; m < n; m++) {
            if (task[m].id == t->id) {
                elog("%s: tasks %s and %s have the same id %d.", SKILLQUESTS_CFG, task[m].key, t->key, t->id);
                return 0;
            }
        }
        if ((t->skill = skill_lookup_key(t->skill_key)) == -1) {
            elog("%s: task %s: skill=%s is not in skills.cfg.", SKILLQUESTS_CFG, t->key, t->skill_key);
            return 0;
        }
        if ((t->need = skilling_thing(t->need_key)) == -1) {
            elog("%s: task %s: need=%s is not a resource, tool or recipe in skilling.cfg.", SKILLQUESTS_CFG, t->key, t->need_key);
            return 0;
        }
    }
    if (ntask && (board_quest < 0 || board_quest >= MAXQUEST)) {
        elog("%s: config: needs board_quest= (the questlog number of the task board).", SKILLQUESTS_CFG);
        return 0;
    }
    return 1;
}

int skillquest_load(void) {
    char *text;
    int ok;

    nsq = ntask = 0;
    tasks_per_day = 3;
    reset_hour = 0;
    board_bonus_gold = 0;
    board_quest = -1;
    strcpy(board_npc, "BOARD");

    if (!(text = skillcfg_read(SKILLQUESTS_CFG))) {
        xlog("No %s, skilling quests and the task board are off.", SKILLQUESTS_CFG);
        return 1;
    }
    ok = parse_cfg(text);
    xfree(text);
    if (!ok || !resolve()) {
        nsq = ntask = 0;
        return 0;
    }
    xlog("Loaded %s: %d quests, %d daily tasks.", SKILLQUESTS_CFG, nsq, ntask);
    return 1;
}

// ---------- helpers ----------

static int skill_lvl(int cn, int sk) {
    return sk == -1 ? 999 : skill_get_level(cn, sk);
}

static char *skname(int sk) {
    return (sk >= 0 && sk < MAXSKILLS && skilldef[sk].used) ? skilldef[sk].name : "?";
}

static int quest_open(int cn, int nr) { // questlog says open
    struct quest *quest;

    if (!(quest = set_data(cn, DRD_QUESTLOG_PPD, sizeof(struct quest) * MAXQUEST))) return 0;
    return quest[nr].flags == QF_OPEN;
}

// "10 Copper Ore and 5 Tin Ore"; with have=1 the counts read "4/10 Copper Ore"
static void need_text(int cn, struct sqdef *q, char *buf, int size, int have) {
    int m, len = 0;

    buf[0] = 0;
    for (m = 0; m < q->nneed && len < size - 1; m++) {
        if (m) len += snprintf(buf + len, size - len, m == q->nneed - 1 ? " and " : ", ");
        if (len >= size - 1) break;
        if (have) len += snprintf(buf + len, size - len, "%d/%d %s", min(skilling_thing_count(cn, q->need[m]), q->needcnt[m]), q->needcnt[m], skilling_thing_name(q->need[m]));
        else len += snprintf(buf + len, size - len, "%d %s", q->needcnt[m], skilling_thing_name(q->need[m]));
    }
}

// the quest this NPC has for cn: the open one, else the first not done. *state: 1 open, 2 can accept,
// 3 level too low, 4 needs an earlier quest, 0 = nothing left
static int npc_quest(int cn, char *npc, int *state) {
    int n;

    for (n = 0; n < nsq; n++) {
        if (strcmp(sq[n].npc, npc)) continue;
        if (quest_open(cn, sq[n].quest)) {
            *state = 1;
            return n;
        }
    }
    for (n = 0; n < nsq; n++) {
        if (strcmp(sq[n].npc, npc)) continue;
        if (questlog_isdone(cn, sq[n].quest)) continue;
        if (sq[n].after != -1 && !questlog_isdone(cn, sq[sq[n].after].quest)) *state = 4;
        else if (sq[n].skill != -1 && skill_lvl(cn, sq[n].skill) < sq[n].level) *state = 3;
        else *state = 2;
        return n;
    }
    *state = 0;
    return -1;
}

// ---------- quests ----------

static void quest_talk(int cn, int co, char *npc) {
    int state, n;
    char buf[256];

    n = npc_quest(co, npc, &state);
    switch (state) {
    case 0:
        quiet_say(cn, "Thou hast done all I could ask of thee, %s. The task board in the village square always has work.", ch[co].name);
        return;
    case 1:
        need_text(co, sq + n, buf, sizeof(buf), 1);
        if (sq[n].remind[0]) quiet_say(cn, "%s", sq[n].remind);
        quiet_say(cn, "Thou hast %s. Say '\260c4done\260c0' when thou hast all of it.", buf);
        return;
    case 3:
        quiet_say(cn, "Come back when thy %s is level %d, %s. I may have work for thee then.", skname(sq[n].skill), sq[n].level, ch[co].name);
        return;
    case 4:
        quiet_say(cn, "Finish \"%s\" first, %s.", questlog_name(sq[sq[n].after].quest), ch[co].name);
        return;
    }
    need_text(co, sq + n, buf, sizeof(buf), 0);
    if (sq[n].offer[0]) quiet_say(cn, "%s", sq[n].offer);
    quiet_say(cn, "Bring me %s. Say '\260c4accept\260c0' if thou wilt do it.", buf);
}

static void quest_accept(int cn, int co, char *npc) {
    int state, n;
    char buf[256];

    n = npc_quest(co, npc, &state);
    if (state != 2) {
        quest_talk(cn, co, npc);
        return;
    }
    questlog_open(co, sq[n].quest);
    need_text(co, sq + n, buf, sizeof(buf), 0);
    quiet_say(cn, "Good! Bring me %s, and say '\260c4done\260c0' when thou hast it all.", buf);
    log_char(co, LOG_SYSTEM, 0, "\260c3New quest: %s.", questlog_name(sq[n].quest));
}

static void quest_done(int cn, int co, char *npc) {
    int state, n, m;
    char buf[256];
    struct sqdef *q;

    n = npc_quest(co, npc, &state);
    if (state != 1) {
        quest_talk(cn, co, npc);
        return;
    }
    q = sq + n;
    for (m = 0; m < q->nneed; m++) {
        if (skilling_thing_count(co, q->need[m]) < q->needcnt[m]) {
            need_text(co, q, buf, sizeof(buf), 1);
            quiet_say(cn, "That is not all of it yet, %s. Thou hast %s.", ch[co].name, buf);
            return;
        }
    }
    if (q->reward != -1 && !skilling_thing_room(co, q->reward, q->reward_cnt)) {
        quiet_say(cn, "Make some room in thy pack first, %s, I have something for thee.", ch[co].name);
        return;
    }

    for (m = 0; m < q->nneed; m++) skilling_thing_take(co, q->need[m], q->needcnt[m]);
    dlog(co, 0, "finished skilling quest %s (%d)", q->key, q->quest);
    questlog_done(co, q->quest);

    quiet_say(cn, "%s", q->thanks[0] ? q->thanks : "Well done, and thank thee!");
    for (m = 0; m < q->nxp; m++) {
        if (!q->xp[m]) continue;
        log_char(co, LOG_SYSTEM, 0, "\260c3Quest reward: %d %s XP.", q->xp[m], skname(q->xpskill[m]));
        add_skill_xp(co, q->xpskill[m], q->xp[m]);
    }
    if (q->gold) {
        give_money(co, q->gold * 100, "skilling quest");
        log_char(co, LOG_SYSTEM, 0, "\260c3Quest reward: %d gold.", q->gold);
    }
    if (q->reward != -1 && skilling_thing_give(co, q->reward, q->reward_cnt)) {
        if (q->reward_cnt > 1) log_char(co, LOG_SYSTEM, 0, "\260c3Quest reward: %d %s.", q->reward_cnt, skilling_thing_name(q->reward));
        else log_char(co, LOG_SYSTEM, 0, "\260c3Quest reward: %s.", skilling_thing_name(q->reward));
    }

    // what next?
    n = npc_quest(co, npc, &state);
    if (state == 2) quiet_say(cn, "I have more work, if thou wantst it. Say '\260c4quest\260c0'.");
    else if (state == 3) quiet_say(cn, "Come back when thy %s is level %d, I will have more work then.", skname(sq[n].skill), sq[n].level);
}

// ---------- daily task board ----------

static struct taskdef *task_by_id(int id) {
    int n;

    for (n = 0; n < ntask; n++)
        if (task[n].id == id) return task + n;
    return NULL;
}

static int today(void) {
    return (time_now - reset_hour * 3600) / (24 * 3600);
}

static struct skillquest_ppd *board_ppd(int cn) {
    struct skillquest_ppd *ppd;

    if (!(ppd = set_data(cn, DRD_SKILLQUEST_PPD, sizeof(struct skillquest_ppd)))) return NULL;
    if (ppd->day != today()) {
        ppd->day = today();
        ppd->done_today = 0;
    }
    return ppd;
}

// the task's text= with {count} and {item} filled in
static void task_text(struct taskdef *t, int cnt, char *buf, int size) {
    char *src = t->text[0] ? t->text : "Bring me {count} {item}.";
    int len = 0;

    while (*src && len < size - 1) {
        if (!strncmp(src, "{count}", 7)) {
            len += snprintf(buf + len, size - len, "%d", cnt);
            src += 7;
        } else if (!strncmp(src, "{item}", 6)) {
            len += snprintf(buf + len, size - len, "%s", skilling_thing_name(t->need));
            src += 6;
        } else buf[len++] = *src++;
    }
    buf[min(len, size - 1)] = 0;
}

static void hours_left(char *buf, int size) {
    int left = (today() + 1) * 24 * 3600 + reset_hour * 3600 - time_now;

    if (left >= 3600) snprintf(buf, size, "%d hour%s", left / 3600, left / 3600 == 1 ? "" : "s");
    else snprintf(buf, size, "%d minute%s", max(1, left / 60), left / 60 == 1 ? "" : "s");
}

static void board_talk(int cn, int co) {
    struct skillquest_ppd *ppd;
    struct taskdef *t;
    int n, cnt, pick, list[MAXTASK];
    char buf[300], tmp[40];

    if (!ntask) {
        quiet_say(cn, "I have no tasks today, %s.", ch[co].name);
        return;
    }
    if (!(ppd = board_ppd(co))) return;

    if (ppd->task_id && (t = task_by_id(ppd->task_id))) {
        task_text(t, ppd->task_count, buf, sizeof(buf));
        quiet_say(cn, "%s", buf);
        quiet_say(cn, "Thou hast %d of %d. Say '\260c4done\260c0' when thou hast them, or '\260c4cancel\260c0' to drop the task.",
                  min(skilling_thing_count(co, t->need), ppd->task_count), ppd->task_count);
        return;
    }
    ppd->task_id = 0;
    if (ppd->done_today >= tasks_per_day) {
        hours_left(tmp, sizeof(tmp));
        quiet_say(cn, "That is all the work for today, %s. Come back in %s.", ch[co].name, tmp);
        return;
    }

    for (n = cnt = 0; n < ntask; n++) {
        t = task + n;
        if (skill_lvl(co, t->skill) < t->level || skill_lvl(co, t->skill) > t->maxlevel) continue;
        list[cnt++] = n;
    }
    if (!cnt) {
        quiet_say(cn, "I have nothing for thee yet, %s. Learn to mine, chop or pick flax first.", ch[co].name);
        return;
    }
    if (cnt > 1) { // not the same task twice in a row
        for (n = 0; n < cnt; n++) {
            if (task[list[n]].id == ppd->last_id) {
                list[n] = list[--cnt];
                break;
            }
        }
    }
    pick = list[RANDOM(cnt)];
    t = task + pick;
    ppd->task_id = t->id;
    ppd->task_count = t->min + RANDOM(t->max - t->min + 1);
    questlog_open(co, board_quest);

    task_text(t, ppd->task_count, buf, sizeof(buf));
    quiet_say(cn, "%s", buf);
    quiet_say(cn, "Say '\260c4done\260c0' when thou hast them. That is task %d of %d for today.", ppd->done_today + 1, tasks_per_day);
}

static void board_finish(int cn, int co, struct skillquest_ppd *ppd, int finished) {
    if (finished) {
        ppd->total++;
        if (questlog_count(co, board_quest) < 63) questlog_done(co, board_quest); // the count has 6 bits
        else questlog_close(co, board_quest);
    } else questlog_close(co, board_quest);
    ppd->last_id = ppd->task_id;
    ppd->task_id = 0;
    ppd->task_count = 0;
    ppd->done_today++;
}

static void board_done(int cn, int co) {
    struct skillquest_ppd *ppd;
    struct taskdef *t;
    int have, xp, silver;

    if (!(ppd = board_ppd(co))) return;
    if (!ppd->task_id || !(t = task_by_id(ppd->task_id))) {
        ppd->task_id = 0;
        board_talk(cn, co);
        return;
    }
    if ((have = skilling_thing_count(co, t->need)) < ppd->task_count) {
        quiet_say(cn, "Thou hast only %d of %d %s, %s.", have, ppd->task_count, skilling_thing_name(t->need), ch[co].name);
        return;
    }
    skilling_thing_take(co, t->need, ppd->task_count);
    xp = t->xp_each * ppd->task_count;
    silver = t->silver_each * ppd->task_count;
    dlog(co, 0, "finished daily task %s (%d x %s)", t->key, ppd->task_count, t->need_key);
    board_finish(cn, co, ppd, 1);

    quiet_say(cn, "Splendid work, %s! Here is thy pay.", ch[co].name);
    if (xp) {
        log_char(co, LOG_SYSTEM, 0, "\260c3Task reward: %d %s XP.", xp, skname(t->skill));
        add_skill_xp(co, t->skill, xp);
    }
    if (ppd->done_today == tasks_per_day && board_bonus_gold) { // all of today's tasks done
        silver += board_bonus_gold * 100;
        log_char(co, LOG_SYSTEM, 0, "\260c3All of today's tasks done: %d gold bonus.", board_bonus_gold);
    }
    if (silver) {
        give_money(co, silver, "daily task");
        if (silver % 100) log_char(co, LOG_SYSTEM, 0, "\260c3Task reward: %d gold %d silver.", silver / 100, silver % 100);
        else log_char(co, LOG_SYSTEM, 0, "\260c3Task reward: %d gold.", silver / 100);
    }
    if (ppd->done_today < tasks_per_day) quiet_say(cn, "I have more work today. Say '\260c4task\260c0'.");
    else quiet_say(cn, "That was the last task for today. Come back tomorrow!");
}

static void board_cancel(int cn, int co) {
    struct skillquest_ppd *ppd;

    if (!(ppd = board_ppd(co))) return;
    if (!ppd->task_id) {
        quiet_say(cn, "Thou hast no task from me, %s.", ch[co].name);
        return;
    }
    board_finish(cn, co, ppd, 0);
    quiet_say(cn, "As thou wilt. That counts as one of today's tasks, %s.", ch[co].name);
}

// ---------- the NPC ----------

static int has_word(char *text, char *word) {
    int len = strlen(word);
    char *p;

    for (p = text; (p = strcasestr(p, word)); p += len) {
        if ((p == text || !isalpha(p[-1])) && !isalpha(p[len])) return 1;
    }
    return 0;
}

static void npc_text(int cn, struct sqnpc_data *dat, int co, char *text) {
    char *said;
    int board;

    if (co == cn || co < 1 || co >= MAXCHARS || !(ch[co].flags & CF_PLAYER)) return;
    if (char_dist(cn, co) > TALKDIST || !char_see_char(cn, co)) return;
    if (!(said = strchr(text, '"'))) return; // "Name says: "text""
    said++;

    board = !strcmp(dat->npc, board_npc);
    if (board) {
        if (has_word(said, "task") || has_word(said, "tasks") || has_word(said, "daily") || has_word(said, "work") || has_word(said, "quest")) board_talk(cn, co);
        else if (has_word(said, "done") || has_word(said, "finished") || has_word(said, "deliver")) board_done(cn, co);
        else if (has_word(said, "cancel") || has_word(said, "abandon")) board_cancel(cn, co);
        else if (has_word(said, "hi") || has_word(said, "hello") || has_word(said, "help")) quiet_say(cn, "Hello %s! Say '\260c4task\260c0' and I will find thee some honest work.", ch[co].name);
        else return;
    } else {
        if (has_word(said, "quest") || has_word(said, "work") || has_word(said, "job") || has_word(said, "task")) quest_talk(cn, co, dat->npc);
        else if (has_word(said, "accept") || has_word(said, "yes")) quest_accept(cn, co, dat->npc);
        else if (has_word(said, "done") || has_word(said, "finished") || has_word(said, "deliver")) quest_done(cn, co, dat->npc);
        else if (has_word(said, "hi") || has_word(said, "hello") || has_word(said, "help")) quest_talk(cn, co, dat->npc);
        else return;
    }
    dat->last_talk = ticker;
}

static void npc_greet(int cn, struct sqnpc_data *dat, int co) {
    int state;

    if (co == cn || !(ch[co].flags & CF_PLAYER) || !char_see_char(cn, co) || char_dist(cn, co) > TALKDIST) return;
    if (mem_check_driver(cn, co, 7)) return;
    mem_add_driver(cn, co, 7);

    if (!strcmp(dat->npc, board_npc)) {
        quiet_say(cn, "Hello %s! Looking for work? Say '\260c4task\260c0'.", ch[co].name);
        return;
    }
    npc_quest(co, dat->npc, &state);
    if (state == 1) quiet_say(cn, "Back again, %s? Say '\260c4done\260c0' if thou hast everything.", ch[co].name);
    else if (state == 2) quiet_say(cn, "Hello %s! I could use a hand. Say '\260c4quest\260c0'.", ch[co].name);
}

static void npc_parse(int cn, struct sqnpc_data *dat) {
    char *ptr, name[64], value[64];

    for (ptr = nextnv(ch[cn].arg, name, value); ptr; ptr = nextnv(ptr, name, value)) {
        if (!strcmp(name, "dir")) dat->dir = atoi(value);
        else if (!strcmp(name, "npc")) {
            scopy(dat->npc, value, sizeof(dat->npc));
            upcase(dat->npc);
        } else elog("unknown arg for %s (%d): %s", ch[cn].name, cn, name);
    }
}

static void npc_driver(int cn, int ret, int lastact) {
    struct sqnpc_data *dat;
    struct msg *msg, *next;
    int co, in;

    if (!(dat = set_data(cn, DRD_SKILLQUESTDRIVER, sizeof(struct sqnpc_data)))) return;
    if (!dat->init) {
        dat->init = 1;
        dat->dir = DX_DOWN;
        if (ch[cn].arg) npc_parse(cn, dat);
        ch[cn].arg = NULL;
        if (!dat->npc[0]) elog("%s (%d) has driver %d but no npc= in its arg.", ch[cn].name, cn, CDR_SKILLQUEST);
    }

    for (msg = ch[cn].msg; msg; msg = next) {
        next = msg->next;

        switch (msg->type) {
        case NT_CHAR:
            npc_greet(cn, dat, (int)msg->dat1);
            break;
        case NT_TEXT:
            if (msg->dat1 == LOG_TALK || msg->dat1 == LOG_SHOUT) npc_text(cn, dat, (int)msg->dat3, (char *)msg->dat2);
            break;
        case NT_GIVE: // give it back, items are handed in by saying "done"
            co = (int)msg->dat1;
            if ((in = ch[cn].citem)) {
                ch[cn].citem = 0;
                it[in].carried = 0;
                if (co > 0 && co < MAXCHARS && (ch[co].flags & CF_PLAYER) && give_char_item(co, in)) {
                    quiet_say(cn, "Keep that, %s. Just say '\260c4done\260c0' when thou hast everything.", ch[co].name);
                } else {
                    destroy_item(in);
                }
            }
            break;
        }
        remove_message(cn, msg);
    }

    if (secure_move_driver(cn, ch[cn].tmpx, ch[cn].tmpy, dat->dir, ret, lastact)) return;
    do_idle(cn, TICKS);
}

int skillquest_driver(int type, int cn, intptr_t ret, int lastact) {
    if (type == 0) npc_driver(cn, (int)ret, lastact);
    return 1; // item (1) and death (2) calls: nothing to do
}

// ---------- /tasks ----------

void skillquest_command(int cn, char *args) {
    struct skillquest_ppd *ppd;
    struct taskdef *t;
    int n, state, any = 0;
    char buf[256], tmp[40];

    if (!nsq && !ntask) {
        log_char(cn, LOG_SYSTEM, 0, "There are no skilling quests on this server.");
        return;
    }
    while (isspace(*args)) args++;
    if (!strcasecmp(args, "reset") && (ch[cn].flags & CF_GOD)) {
        if ((ppd = set_data(cn, DRD_SKILLQUEST_PPD, sizeof(struct skillquest_ppd)))) ppd->day = -1;
        log_char(cn, LOG_SYSTEM, 0, "Today's task count reset.");
        return;
    }
    if (!strncasecmp(args, "give ", 5) && (ch[cn].flags & CF_GOD)) { // /tasks give <KEY> [count], for testing
        char key[64];
        int d, cnt;

        args += 5;
        while (isspace(*args)) args++;
        for (n = 0; n < 63 && *args && !isspace(*args); n++) key[n] = *args++;
        key[n] = 0;
        cnt = max(1, atoi(args));
        upcase(key);
        if ((d = skilling_thing(key)) == -1) log_char(cn, LOG_SYSTEM, 0, "Unknown skilling key \"%s\".", key);
        else if (!skilling_thing_room(cn, d, cnt) || !skilling_thing_give(cn, d, cnt)) log_char(cn, LOG_SYSTEM, 0, "No room for %d %s.", cnt, skilling_thing_name(d));
        else log_char(cn, LOG_SYSTEM, 0, "Gave %d %s.", cnt, skilling_thing_name(d));
        return;
    }

    log_char(cn, LOG_SYSTEM, 0, "\260c3Skilling quests:");
    for (n = 0; n < nsq; n++) {
        int q, k, done = 0, total = 0;

        for (k = 0; k < n; k++)
            if (!strcmp(sq[k].npc, sq[n].npc)) break;
        if (k < n) continue; // one line per quest giver

        for (k = 0; k < nsq; k++) {
            if (strcmp(sq[k].npc, sq[n].npc)) continue;
            total++;
            if (questlog_isdone(cn, sq[k].quest)) done++;
        }
        q = npc_quest(cn, sq[n].npc, &state);
        any = 1;
        switch (state) {
        case 0:
            log_char(cn, LOG_SYSTEM, 0, "  %s: all %d quests done.", questlog_giver(sq[n].quest), total);
            break;
        case 1:
            need_text(cn, sq + q, buf, sizeof(buf), 1);
            log_char(cn, LOG_SYSTEM, 0, "  %s: \"%s\", %s.", questlog_giver(sq[q].quest), questlog_name(sq[q].quest), buf);
            break;
        case 2:
            log_char(cn, LOG_SYSTEM, 0, "  %s: \"%s\" is waiting for thee (%d of %d done).", questlog_giver(sq[q].quest), questlog_name(sq[q].quest), done, total);
            break;
        case 3:
            log_char(cn, LOG_SYSTEM, 0, "  %s: next quest at %s level %d (%d of %d done).", questlog_giver(sq[q].quest), skname(sq[q].skill), sq[q].level, done, total);
            break;
        default:
            log_char(cn, LOG_SYSTEM, 0, "  %s: next quest after \"%s\" (%d of %d done).", questlog_giver(sq[q].quest), questlog_name(sq[sq[q].after].quest), done, total);
            break;
        }
    }
    if (!any) log_char(cn, LOG_SYSTEM, 0, "  none");

    if (ntask && (ppd = board_ppd(cn))) {
        log_char(cn, LOG_SYSTEM, 0, "\260c3Task board (%s):", board_quest >= 0 ? questlog_giver(board_quest) : "?");
        if (ppd->task_id && (t = task_by_id(ppd->task_id))) {
            log_char(cn, LOG_SYSTEM, 0, "  Bring %d/%d %s.", min(skilling_thing_count(cn, t->need), ppd->task_count), ppd->task_count, skilling_thing_name(t->need));
        }
        hours_left(tmp, sizeof(tmp));
        log_char(cn, LOG_SYSTEM, 0, "  %d of %d tasks done today (new tasks in %s), %d done in all.", min(ppd->done_today, tasks_per_day), tasks_per_day, tmp, ppd->total);
    }
}
