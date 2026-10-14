/*
 * Part of Astonia Server 3.5 (c) Daniel Brockhaus. Please read license.txt.
 */

#define SIZE (DISTMAX * 2 + 1)

struct los {
    int x, y;
    int xoff, yoff;
    int maxdist;
    unsigned char tab[SIZE][SIZE];
};

extern struct los *los;

// Line of sight over the bigger view range sent to protocol 4 clients (DISTVIEW).
// One per player slot; only used to decide which map tiles the client is shown.
#define VSIZE (DISTVIEW * 2 + 1)
#define MAXVLOS 256 // MAXPLAYER

struct vlos {
    int x, y;
    int xoff, yoff;
    int maxdist;
    unsigned char tab[VSIZE][VSIZE];
};

extern struct vlos *vlos;

#ifdef NEED_FAST_LOS
static inline int fast_los(int cn, int x, int y) {
    x = x + los[cn].xoff;
    y = y + los[cn].yoff;

    if (los[cn].tab[x][y]) return 1;
    else return 0;
}
#endif

#ifdef NEED_FAST_LOS
static inline int fast_vlos(int nr, int x, int y) {
    x = x + vlos[nr].xoff;
    y = y + vlos[nr].yoff;

    if (vlos[nr].tab[x][y]) return 1;
    else return 0;
}
#endif

#ifdef NEED_FAST_LOS_LIGHT
static inline int fast_los_light(int cn, int x, int y, int maxdist) {
    x = x + los[cn].xoff;
    y = y + los[cn].yoff;

    return los[cn].tab[x][y] >= maxdist ? 0 : los[cn].tab[x][y];
}
#endif

int init_los(void);
void reset_los(int xc, int yc);
int los_can_see(int cn, int xc, int yc, int tx, int ty, int maxdist);
int update_los(int cn, int xc, int yc, int maxdist);
void reset_los_for_char(int cn);
int update_vlos(int nr, int xc, int yc);
int vlos_can_see(int nr, int tx, int ty);
