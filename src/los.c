/*
 * Part of Astonia Server 3.5 (c) Daniel Brockhaus. Please read license.txt.
 */

#include <stdlib.h>
#include <string.h>

#include "server.h"
#include "log.h"
#include "light.h"
#include "mem.h"
#include "los.h"

struct los *los;
struct vlos *vlos;

static int dodoors = 0, doorx, doory;

int init_los(void) {
    los = xcalloc(sizeof(struct los) * MAXCHARS, IM_BASE);
    if (!los) return 0;
    xlog("Allocated los: %.2fM (%d*%d)", sizeof(struct los) * MAXCHARS / 1024.0 / 1024.0, (int)(sizeof(struct los)), MAXCHARS);
    mem_usage += sizeof(struct los) * MAXCHARS;

    vlos = xcalloc(sizeof(struct vlos) * MAXVLOS, IM_BASE);
    if (!vlos) return 0;
    xlog("Allocated vlos: %.2fM (%d*%d)", sizeof(struct vlos) * MAXVLOS / 1024.0 / 1024.0, (int)(sizeof(struct vlos)), MAXVLOS);
    mem_usage += sizeof(struct vlos) * MAXVLOS;

    return 1;
}

// The LOS builder works on a square table of size*size entries centered on (xc,yc).
// los[] uses SIZE (DISTMAX), vlos[] uses VSIZE (DISTVIEW); both are compile-time
// constants, so the inlined copies stay as fast as the old fixed-size code.
#define TAB(x, y) tab[(x) * size + (y)]

static inline void add_los(unsigned char *tab, int size, int xoff, int yoff, int x, int y, int v) {
    if (!TAB(x + xoff, y + yoff)) TAB(x + xoff, y + yoff) = v;
}

static inline int check_map(int x, int y) {
    int m;

    if (x < 1 || x >= MAXMAP || y < 1 || y >= MAXMAP) return 0;

    m = x + y * MAXMAP;

    if (dodoors && (map[m].flags & MF_DOOR) && (x != doorx || y != doory)) return 1;

    if (map[m].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK)) return 0;

    return 1;
}

static inline int is_close_los_down(unsigned char *tab, int size, int xoff, int yoff, int xc, int yc, int v) {
    int x, y;

    x = xc + xoff;
    y = yc + yoff;

    if (TAB(x, y + 1) == v && check_map(xc, yc + 1)) return 1;
    if (TAB(x + 1, y + 1) == v && check_map(xc + 1, yc + 1)) return 1;
    if (TAB(x - 1, y + 1) == v && check_map(xc - 1, yc + 1)) return 1;

    return 0;
}

static inline int is_close_los_up(unsigned char *tab, int size, int xoff, int yoff, int xc, int yc, int v) {
    int x, y;

    x = xc + xoff;
    y = yc + yoff;

    if (TAB(x, y - 1) == v && check_map(xc, yc - 1)) return 1;
    if (TAB(x + 1, y - 1) == v && check_map(xc + 1, yc - 1)) return 1;
    if (TAB(x - 1, y - 1) == v && check_map(xc - 1, yc - 1)) return 1;

    return 0;
}

static inline int is_close_los_left(unsigned char *tab, int size, int xoff, int yoff, int xc, int yc, int v) {
    int x, y;

    x = xc + xoff;
    y = yc + yoff;

    if (TAB(x + 1, y) == v && check_map(xc + 1, yc)) return 1;
    if (TAB(x + 1, y + 1) == v && check_map(xc + 1, yc + 1)) return 1;
    if (TAB(x + 1, y - 1) == v && check_map(xc + 1, yc - 1)) return 1;

    return 0;
}

static inline int is_close_los_right(unsigned char *tab, int size, int xoff, int yoff, int xc, int yc, int v) {
    int x, y;

    x = xc + xoff;
    y = yc + yoff;

    if (TAB(x - 1, y) == v && check_map(xc - 1, yc)) return 1;
    if (TAB(x - 1, y + 1) == v && check_map(xc - 1, yc + 1)) return 1;
    if (TAB(x - 1, y - 1) == v && check_map(xc - 1, yc - 1)) return 1;

    return 0;
}

static inline int check_los(int cn, int x, int y) {
    x = x + los[cn].xoff;
    y = y + los[cn].yoff;

    return los[cn].tab[x][y];
}

// fill tab (size*size, already zeroed) with the distance+1 of every tile visible from (xc,yc)
static inline void build_tab(unsigned char *tab, int size, int xoff, int yoff, int xc, int yc, int maxdist) {
    int x, y, dist, found;

    add_los(tab, size, xoff, yoff, xc, yc, 1);

    add_los(tab, size, xoff, yoff, xc + 1, yc, 2);
    add_los(tab, size, xoff, yoff, xc - 1, yc, 2);
    add_los(tab, size, xoff, yoff, xc, yc + 1, 2);
    add_los(tab, size, xoff, yoff, xc, yc - 1, 2);

    add_los(tab, size, xoff, yoff, xc + 1, yc + 1, 2);
    add_los(tab, size, xoff, yoff, xc + 1, yc - 1, 2);
    add_los(tab, size, xoff, yoff, xc - 1, yc + 1, 2);
    add_los(tab, size, xoff, yoff, xc - 1, yc - 1, 2);

    for (dist = 2; dist < maxdist; dist++) {
        found = 0;
        for (x = xc - dist; x <= xc + dist; x++) {
            if (is_close_los_down(tab, size, xoff, yoff, x, yc - dist, dist)) {
                add_los(tab, size, xoff, yoff, x, yc - dist, dist + 1);
                found = 1;
            }
            if (is_close_los_up(tab, size, xoff, yoff, x, yc + dist, dist)) {
                add_los(tab, size, xoff, yoff, x, yc + dist, dist + 1);
                found = 1;
            }
        }
        for (y = yc - dist; y <= yc + dist; y++) {
            if (is_close_los_left(tab, size, xoff, yoff, xc - dist, y, dist)) {
                add_los(tab, size, xoff, yoff, xc - dist, y, dist + 1);
                found = 1;
            }
            if (is_close_los_right(tab, size, xoff, yoff, xc + dist, y, dist)) {
                add_los(tab, size, xoff, yoff, xc + dist, y, dist + 1);
                found = 1;
            }
        }
        if (!found) break;
    }
}

static void build_los(int cn, int xc, int yc, int maxdist) {
    unsigned long long prof;

    prof = prof_start(15);

    bzero(los[cn].tab, sizeof(los[cn].tab));

    los[cn].xoff = DISTMAX - xc;
    los[cn].yoff = DISTMAX - yc;
    los[cn].maxdist = maxdist;

    build_tab(&los[cn].tab[0][0], SIZE, los[cn].xoff, los[cn].yoff, xc, yc, maxdist);

    prof_stop(15, prof);
}

static void build_vlos(int nr, int xc, int yc) {
    unsigned long long prof;

    prof = prof_start(15);

    bzero(vlos[nr].tab, sizeof(vlos[nr].tab));

    vlos[nr].xoff = DISTVIEW - xc;
    vlos[nr].yoff = DISTVIEW - yc;
    vlos[nr].maxdist = DISTVIEW;

    build_tab(&vlos[nr].tab[0][0], VSIZE, vlos[nr].xoff, vlos[nr].yoff, xc, yc, DISTVIEW);

    prof_stop(15, prof);
}

// updates the view LOS of player slot nr; returns true if it had to be rebuilt
int update_vlos(int nr, int xc, int yc) {
    if (nr < 1 || nr >= MAXVLOS) return 0;

    if (xc != vlos[nr].x || yc != vlos[nr].y) {
        build_vlos(nr, xc, yc);
        vlos[nr].x = xc;
        vlos[nr].y = yc;
        return 1;
    }
    return 0;
}

// can player slot nr (whose vlos is current) see tile tx,ty?
int vlos_can_see(int nr, int tx, int ty) {
    int x, y;

    if (nr < 1 || nr >= MAXVLOS) return 0;

    x = tx + vlos[nr].xoff;
    y = ty + vlos[nr].yoff;
    if (x < 0 || y < 0 || x >= VSIZE || y >= VSIZE) return 0;

    return vlos[nr].tab[x][y];
}

void reset_los(int xc, int yc) {
    int x, y, cn;
    unsigned long long prof;

    prof = prof_start(14);

    for (y = max(0, yc - DISTMAX); y <= min(MAXMAP - 1, yc + DISTMAX); y++) {
        for (x = max(0, xc - DISTMAX); x <= min(MAXMAP - 1, xc + DISTMAX); x++) {
            if ((cn = map[x + y * MAXMAP].ch) != 0) {
                los[cn].x = los[cn].y = 0;
            }
        }
    }

    if (abs(los[0].x - xc) <= LIGHTDIST && abs(los[0].y - yc) <= LIGHTDIST)
        los[0].x = los[0].y = 0;

    // player view tables reach further than DISTMAX
    for (int nr = 1; nr < MAXVLOS; nr++) {
        if (vlos[nr].x && abs(vlos[nr].x - xc) <= DISTVIEW && abs(vlos[nr].y - yc) <= DISTVIEW)
            vlos[nr].x = vlos[nr].y = 0;
    }

    prof_stop(14, prof);
}

// updates the LOS cache for character cn
// returns true if los had to be rebuild, false otherwise
int update_los(int cn, int xc, int yc, int maxdist) {
    if (maxdist > DISTMAX) maxdist = DISTMAX;

    if (xc != los[cn].x || yc != los[cn].y || maxdist > los[cn].maxdist) {
        build_los(cn, xc, yc, maxdist);
        los[cn].x = xc;
        los[cn].y = yc;
        return 1;
    }
    return 0;
}

int los_can_see(int cn, int xc, int yc, int tx, int ty, int maxdist) {
    int tmp;
    unsigned long long prof;

    prof = prof_start(13);

    if (abs(xc - tx) >= maxdist || abs(yc - ty) >= maxdist) {
        prof_stop(13, prof);
        return 0;
    }

    if (maxdist > DISTMAX) maxdist = DISTMAX;

    if (xc != los[cn].x || yc != los[cn].y || maxdist > los[cn].maxdist) {
        build_los(cn, xc, yc, maxdist);
        los[cn].x = xc;
        los[cn].y = yc;
    }

    tmp = check_los(cn, tx, ty);
    if (tmp >= maxdist) tmp = 0;

    prof_stop(13, prof);

    return tmp;
}

int door_los(int cn, int xc, int yc, int tx, int ty, int maxdist, int dx, int dy) {
    int tmp;

    dodoors = 1;
    doorx = dx;
    doory = dy;

    tmp = los_can_see(cn, xc, yc, tx, ty, maxdist);

    dodoors = 0;

    return tmp;
}

void reset_los_for_char(int cn) {
    if (cn < 1 || cn >= MAXCHARS) return;
    bzero(los + cn, sizeof(struct los));
}
