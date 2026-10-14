#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "server.h"
#include "log.h"
#include "notify.h"
#include "direction.h"
#include "do.h"
#include "drdata.h"
#include "see.h"
#include "death.h"
#include "talk.h"
#include "tool.h"
#include "date.h"
#include "create.h"
#include "spell.h"
#include "effect.h"
#include "light.h"
#include "drvlib.h"
#include "libload.h"
#include "item_id.h"
#include "database.h"
#include "los.h"
#include "map.h"
#include "sector.h"
#include "minoa.h"
#include "player.h"
#include "questlog.h"



// library helper functions needed for init
int ch_driver(int nr, int cn, int ret, int lastact); // character driver (decides next action)
int it_driver(int nr, int in, int cn); // item driver (special cases for use)
int ch_died_driver(int nr, int cn, int co); // called when a character dies
int ch_respawn_driver(int nr, int cn); // called when an NPC is about to respawn

int blake_give(int cn, int co, int in, struct minoa_ppd *ppd);
void minoastack_driver(int in, int cn);

// EXPORTED - character/item driver
int driver(int type, int nr, int obj, intptr_t ret, int lastact) {
    switch (type) {
    case CDT_DRIVER:
        return ch_driver(nr, obj, ret, lastact);
    case CDT_ITEM:
        return it_driver(nr, obj, ret);
    case CDT_DEAD:
        return ch_died_driver(nr, obj, ret);
    case CDT_RESPAWN:
        return ch_respawn_driver(nr, obj);
    default:
        return 0;
    }
}

//-----------------------

struct qa {
    char *word[20];
    char *answer;
    int answer_code;
};

struct qa qa[] = {
    {{"how", "are", "you", NULL}, "I'm fine!", 0},
    {{"hello", NULL}, "Hello, %s!", 0},
    {{"hi", NULL}, "Hi, %s!", 0},
    {{"greetings", NULL}, "Greetings, %s!", 0},
    {{"hail", NULL}, "And hail to you, %s!", 0},
    {{"what's", "up", NULL}, "Everything that isn't nailed down.", 0},
    {{"what", "is", "up", NULL}, "Everything that isn't nailed down.", 0},
    {{"repeat", NULL}, NULL, 2},
    {{"restart", NULL}, NULL, 2},
    {{"please", "repeat", NULL}, NULL, 2},
    {{"please", "restart", NULL}, NULL, 2},
};

void lowerstrcpy(char *dst, char *src) {
    while (*src) *dst++ = tolower(*src++);
    *dst = 0;
}

int analyse_text_driver(int cn, int type, char *text, int co) {
    char word[256];
    char wordlist[20][256];
    int n, w, q;

    // ignore game messages
    if (type == LOG_SYSTEM || type == LOG_INFO) return 0;

    // ignore our own talk
    if (cn == co) return 0;

    if (!(ch[co].flags & CF_PLAYER)) return 0;

    if (char_dist(cn, co) > 12) return 0;

    if (!char_see_char(cn, co)) return 0;

    while (isalpha(*text)) text++;
    while (isspace(*text)) text++;
    while (isalpha(*text)) text++;
    if (*text == ':') text++;
    while (isspace(*text)) text++;
    if (*text == '"') text++;

    n = w = 0;
    while (*text) {
        switch (*text) {
        case ' ':
        case ',':
        case ':':
        case '?':
        case '!':
        case '"':
        case '.':
            if (n) {
                word[n] = 0;
                lowerstrcpy(wordlist[w], word);
                if (strcasecmp(wordlist[w], ch[cn].name)) {
                    if (w < 19) w++; // wordlist has 20 entries: more words reuse the last one
                }
            }
            n = 0;
            text++;
            break;
        default:
            word[n++] = *text++;
            if (n > 250) return 0;
            break;
        }
    }

    if (w) {
        for (q = 0; q < sizeof(qa) / sizeof(struct qa); q++) {
            for (n = 0; n < w && qa[q].word[n]; n++) {
                if (strcmp(wordlist[n], qa[q].word[n])) break;
            }
            if (n == w && !qa[q].word[n]) {
                if (qa[q].answer) say(cn, qa[q].answer, ch[co].name, ch[cn].name);
                else return qa[q].answer_code;

                return 1;
            }
        }
    }

    return 0;
}



struct blake_driver_data {
    int last_talk;
    int current_victim;
};

void minoastack_driver(int in, int cn) {
    return;
}


void blake_driver(int cn, int ret, int lastact) {
    struct blake_driver_data* dat;
    struct minoa_ppd* ppd;
    int co, in, talkdir = 0, didsay = 0;
    struct msg* msg, * next;

    dat = set_data(cn, DRD_BLAKEDRIVER, sizeof(struct blake_driver_data));
    if (!dat) return; // oops...

    // loop through our messages
    for (msg = ch[cn].msg; msg; msg = next) {
        next = msg->next;

        // did we see someone?
        if (msg->type == NT_CHAR) {

            co = msg->dat1;

            // dont talk to other NPCs
            if (!(ch[co].flags & CF_PLAYER)) {
                remove_message(cn, msg);
                continue;
            }

            // dont talk to players without connection
            if (ch[co].driver == CDR_LOSTCON) {
                remove_message(cn, msg);
                continue;
            }

            // only talk every ten seconds
            if (ticker < dat->last_talk + TICKS * 5) {
                remove_message(cn, msg);
                continue;
            }

            if (ticker < dat->last_talk + TICKS * 10 && dat->current_victim && dat->current_victim != co) {
                remove_message(cn, msg);
                continue;
            }

            // dont talk to someone we cant see, and dont talk to ourself
            if (!char_see_char(cn, co) || cn == co) {
                remove_message(cn, msg);
                continue;
            }

            // dont talk to someone far away
            if (char_dist(cn, co) > 20) {
                remove_message(cn, msg);
                continue;
            }

            // get current status with player
            ppd = set_data(co, DRD_MINOA_PPD, sizeof(struct minoa_ppd));
            if (ppd) {
                switch (ppd->blake_state) {
                case 0:
                    say(cn, "Hey there! You look like a strong adventurer. Maybe you can help me.");
                    questlog_open(co, 80);
                    ppd->blake_state = 1;
                    ppd->blake_bear_pelts = 0;
                    ppd->blake_wolf_pelts = 0;
                    didsay = 1;
                    break;
                case 1:
                    say(cn, "There have been a lot of mean animals around lately. I think it has something to do with that weird tower.");
                    ppd->blake_state = 2;
                    didsay = 1;
                    break;
                case 2:
                    say(cn, "Can you help us out? The guards are exhausted from keeping us safe, and they need some rest.");
                    ppd->blake_state = 3;
                    didsay = 1;
                    break;
                case 3:
                    say(cn, "Please bring me 10 wolf pelts and 10 bear pelts. I can use them to sew warm clothes for everyone.");
                    ppd->blake_state = 4;
                    didsay = 1;
                    break;
                case 4:
                    break; // pause for item hand in
                case 5: {
                    int wolf_left = 10 - ppd->blake_wolf_pelts;
                    int bear_left = 10 - ppd->blake_bear_pelts;

                    if (wolf_left <= 0 && bear_left <= 0) {
                        ppd->blake_state = 6;
                        break;
                    }

                    if (wolf_left <= 0) {
                        say(cn, "I have enough wolf pelts now. I still need %d bear pelts.", bear_left);
                    } else if (bear_left <= 0) {
                        say(cn, "I have enough bear pelts now. I still need %d wolf pelts.", wolf_left);
                    } else {
                        say(cn, "I still need %d wolf pelts and %d bear pelts.", wolf_left, bear_left);
                    }

                    ppd->blake_state = 4;
                    didsay = 1;
                    break;
                }
                case 6:
                    say(cn, "Thank you! Now I can make warm clothes for everyone.");
                    ppd->blake_state = 7;
                    didsay = 1;
                    break;
                case 7:
                    say(cn, "I hope to be of more help to the village now. Maybe somebody inside the church needs help.");
                    ppd->blake_state = 8;
                    didsay = 1;
                    break;
                case 8:
                    say(cn, "I'm sure there is much more to do.");
                    ppd->blake_state = 9;
                    didsay = 1;
                    break;
                case 9:
                    break;
                }


                if (didsay) {
                    dat->last_talk = ticker;
                    dat->current_victim = co;
                    talkdir = offset2dx(ch[cn].x, ch[cn].y, ch[co].x, ch[co].y);
                }
            }
        }

        if (msg->type == NT_TEXT) {
            co = msg->dat3;

            if (ticker > dat->last_talk + TICKS * 10 && dat->current_victim) dat->current_victim = 0;

            if (dat->current_victim && dat->current_victim != co) {
                remove_message(cn, msg);
                continue;
            }

            didsay = analyse_text_driver(cn, msg->dat1, (char *)msg->dat2, co);

            if (didsay == 2) {
                ppd = set_data(co, DRD_MINOA_PPD, sizeof(struct minoa_ppd));
                if (ppd && ppd->blake_state <= 3) {
                    dat->last_talk = 0;
                    ppd->blake_state = 0;
                } else if (ppd && ppd->blake_state == 4) {
                    dat->last_talk = 0;
                    ppd->blake_state = 3;
                }
            }
            ppd = set_data(co, DRD_MINOA_PPD, sizeof(struct minoa_ppd));

            if (ppd && strstr((char *)msg->dat2, "pelts")) {
                if (ppd->blake_state == 4) {
                    say(cn, "I still need %d wolf pelts and %d bear pelts.",
                        10 - ppd->blake_wolf_pelts,
                        10 - ppd->blake_bear_pelts);
                }
            }
        }

        if (msg->type == NT_GIVE) {
            co = msg->dat1;

            if ((in = ch[cn].citem)) {
                ppd = set_data(co, DRD_MINOA_PPD, sizeof(struct minoa_ppd));
                if (ppd) {
                    if (!blake_give(cn, co, in, ppd) && !give_char_item(co, in)) {
                        destroy_item(in);
                    }
                    ch[cn].citem = 0;
                } else {
                    destroy_item(in);
                    ch[cn].citem = 0;
                }
            }
        }

        standard_message_driver(cn, msg, 0, 0);
        remove_message(cn, msg);
    }

    if (talkdir) turn(cn, talkdir);

    if (spell_self_driver(cn)) return;
    if (regenerate_driver(cn)) return;

    do_idle(cn, TICKS);
}






void fireball_machine(int in, int cn) {
    int dx, dy, power, fn, frequency;

    power = it[in].drdata[2];
    frequency = it[in].drdata[3];

    if (frequency && !cn) call_item(IDR_FIREBALL, in, 0, ticker + frequency);

    if (it[in].sprite == 50041) {
        dx = 0;
        dy = -1;
    } else if (it[in].sprite == 50000) {
        dx = -1;
        dy = 0;
    } else {
        dx = 0;
        dy = 1;
    }

    fn = create_fireball(0,
                         it[in].x + dx, it[in].y + dy,
                         it[in].x + dx * 16, it[in].y + dy * 16,
                         power);

    notify_area(it[in].x, it[in].y, NT_SPELL, 0, V_FIRE, fn);
}


int blake_give(int cn, int co, int in, struct minoa_ppd *ppd) {
    int wolf_left, bear_left;

    if (ppd->blake_state < 4) {
        say(cn, "I don't need this.");
        return 0;
    }

    if (it[in].ID == IID_MINOA_WOLFPELT) {
        if (ppd->blake_wolf_pelts >= 10) {
            destroy_item(in);

            if (ppd->blake_bear_pelts < 10) {
                say(cn, "I already have enough wolf pelts. I still need %d bear pelts.",
                    10 - ppd->blake_bear_pelts);
            } else {
                say(cn, "I already have enough wolf pelts.");
            }
            return 1;
        }

        ppd->blake_wolf_pelts++;
        destroy_item(in);

    } else if (it[in].ID == IID_MINOA_BEARPELT) {
        if (ppd->blake_bear_pelts >= 10) {
            destroy_item(in);

            if (ppd->blake_wolf_pelts < 10) {
                say(cn, "I already have enough bear pelts. I still need %d wolf pelts.",
                    10 - ppd->blake_wolf_pelts);
            } else {
                say(cn, "I already have enough bear pelts.");
            }
            return 1;
        }

        ppd->blake_bear_pelts++;
        destroy_item(in);

    } else {
        say(cn, "That isn't one of the pelts I need.");
        return 0;
    }

    wolf_left = 10 - ppd->blake_wolf_pelts;
    bear_left = 10 - ppd->blake_bear_pelts;

 // final turn in
if (wolf_left <= 0 && bear_left <= 0) {
        questlog_done(co, 80);
        ppd->blake_state = 6;
        return 1;
    }

// progress update
    ppd->blake_state = 5;
    return 1;
}


void burn_char(int cn) {
    int fn, n;

    // dont add effect twice
    for (n = 0; n < 4; n++) {
        if (!(fn = ch[cn].ef[n])) continue;
        if (ef[fn].type != EF_BURN) continue;
        return;
    }

    create_show_effect(EF_BURN, cn, ticker, ticker + TICKS * 60, 250, 1);
    hurt(cn, 20 * POWERSCALE, 0, 1, 50, 75);
}

void extinguish_driver(int in, int cn) {
    int fn, n;

    if (!cn) return;

    // is he on fire?
    for (n = 0; n < 4; n++) {
        if (!(fn = ch[cn].ef[n])) continue;
        if (ef[fn].type != EF_BURN) continue;
        break;
    }
    if (n == 4) {
        log_char(cn, LOG_SYSTEM, 0, "Ahh. Sweet and refreshing.");
        return;
    }

    remove_effect(fn);
    free_effect(fn);

    log_char(cn, LOG_SYSTEM, 0, "You extinguish the flames.");
}

void flamethrow_driver(int in, int cn) {
    int fire, dir, co, x, y, m;
        
    if (cn) return;

    fire = it[in].drdata[0];

    if (fire) {
        it[in].drdata[0]--;

        if (!it[in].drdata[2]) {
            it[in].sprite++;
            it[in].drdata[2] = 1;

            remove_item_light(in);
            it[in].mod_index[4] = V_LIGHT;
            it[in].mod_value[4] = 250;
            add_item_light(in);
        }

        dir = it[in].drdata[1];

        if (dir < DX_RIGHT || dir > DX_RIGHTUP) {
            elog("flamethrow_driver: bad dir=%d item=%d pos=%d,%d",
                 dir, in, it[in].x, it[in].y);
            return;
        }

        dx2offset(dir, &x, &y, NULL);

        x = it[in].x + x;
        y = it[in].y + y;
        if (x > 0 && x < MAXMAP && y > 0 && y < MAXMAP) {
            m = x + y * MAXMAP;
            if ((co = map[m].ch)) burn_char(co);
        }

        dx2offset(dir, &x, &y, NULL);
        x = it[in].x + x * 2;
        y = it[in].y + y * 2;
        if (x > 0 && x < MAXMAP && y > 0 && y < MAXMAP) {
            m = x + y * MAXMAP;
            if ((co = map[m].ch)) burn_char(co);
        }

        call_item(it[in].driver, in, 0, ticker + 1);
    } else {
        it[in].sprite--;
        it[in].drdata[0] = TICKS;
        it[in].drdata[2] = 0;

        remove_item_light(in);
        it[in].mod_index[4] = 0;
        it[in].mod_value[4] = 0;
        add_item_light(in);

        if (it[in].drdata[3]) {
            call_item(it[in].driver, in, 0, ticker + TICKS * it[in].drdata[3]);
        }
    }
}


int it_driver(int nr, int in, int cn) {
    switch (nr) {
        
    case IDR_FIREBALL:
        fireball_machine(in, cn);
        return 1;
    case IDR_FLAMETHROW:
        flamethrow_driver(in, cn);
        return 1;
    case IDR_EXTINGUISH:
        extinguish_driver(in, cn);
        return 1;
    case IDR_MINOASTACK:
        minoastack_driver(in, cn);
        return 1;
        
    default:
        return 0;
    }
}

int ch_driver(int nr, int cn, int ret, int lastact) {
    switch (nr) {
    case CDR_BLAKE:
        blake_driver(cn, ret, lastact);
        return 1;

    default:
        return 0;
    }
}


int ch_died_driver(int nr, int cn, int co) {
    switch (nr) {
    
    default:
        return 0;
    }
}


int ch_respawn_driver(int nr, int cn) {
    switch (nr) {

    default:
        return 0;
    }
}


