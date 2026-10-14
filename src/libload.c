/*
 * Part of Astonia Server 3.5 (c) Daniel Brockhaus. Please read license.txt.
 */

#define __USE_BSD_SIGNAL

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#ifdef _WIN32
#include <windows.h>

// Minimal dlopen() family on top of LoadLibrary(). Zone DLLs resolve the
// server's functions and globals through the import library of server35.exe.
#define RTLD_NOW 0
static void *dlopen(const char *name, int flags) {
    (void)flags;
    return (void *)LoadLibraryA(name);
}
static void *dlsym(void *handle, const char *sym) {
    return (void *)GetProcAddress((HMODULE)handle, sym);
}
static int dlclose(void *handle) {
    return FreeLibrary((HMODULE)handle) ? 0 : -1;
}
static const char *dlerror(void) {
    static char buf[256];
    DWORD err = GetLastError();

    if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, buf, sizeof(buf), NULL))
        sprintf(buf, "error %lu", err);
    return buf;
}
#else
#include <dlfcn.h>
#endif

#include "server.h"
#include "tool.h"
#include "log.h"
#include "player.h"
#include "mem.h"
#include "libload.h"
#include "drvlib.h"
#include "talk.h"
#include "skilling.h"
#include "skillquest.h"

#define MAXLIB 100
#define MAXDRV 200

struct lib {
    int used;
    char name[80];
    time_t time;

    void *handle;
    int (*driver)(int type, int nr, int obj, intptr_t ret, int last_action);
};

static struct lib *libs = NULL;

static int *fast_chdrv;
static int *fast_itdrv;

static int load_lib(struct lib *lib) {
    struct stat st;

    if (stat(lib->name, &st)) {
        elog("Error loading library %s, not present.", lib->name);
        if (lib->handle) {
            dlclose(lib->handle);
            lib->handle = NULL;
            lib->time = 0;
            lib->driver = NULL;
        }
        return 0;
    }
    if (st.st_mtime > lib->time) {
        if (lib->handle) {
            dlclose(lib->handle);
        }

        lib->handle = dlopen(lib->name, RTLD_NOW);
        if (!lib->handle) {
            elog("Error loading library %s (%s).", lib->name, dlerror());
            lib->handle = NULL;
            lib->time = 0;
            lib->driver = NULL;
            return 0;
        }

        lib->driver = dlsym(lib->handle, "driver");
        if (!lib->driver) {
            dlclose(lib->handle);
            elog("Error resolving symbol driver in library %s.", lib->name);
            lib->handle = NULL;
            lib->time = 0;
            lib->driver = NULL;
            return 0;
        }

        lib->time = st.st_mtime;
    }
    if (lib->handle) return 1;
    else return 0;
}

static void add_lib(char *name) {
    int n;

    for (n = 1; n < MAXLIB; n++) {
        if (!libs[n].used) {
            libs[n].used = 1;
            strcpy(libs[n].name, name);
            load_lib(libs + n);
            return;
        }
    }
}

int init_lib(void) {
    DIR *dir;
    struct dirent *de;
    char dirname[NAME_MAX], name[NAME_MAX + 512];

    libs = xcalloc(sizeof(struct lib) * MAXLIB, IM_BASE);
    if (!libs) return 0;
    xlog("Allocated libs: %.2fM (%d*%d)", sizeof(struct lib) * MAXLIB / 1024.0 / 1024.0, (int)(sizeof(struct lib)), MAXLIB);
    mem_usage += sizeof(struct lib) * MAXLIB;

    fast_chdrv = xcalloc(sizeof(int) * MAXDRV, IM_BASE);
    if (!fast_chdrv) return 0;
    xlog("Allocated fast_chdrvs: %.2fM (%d*%d)", sizeof(int) * MAXDRV / 1024.0 / 1024.0, (int)(sizeof(int)), MAXDRV);
    mem_usage += sizeof(int) * MAXDRV;

    fast_itdrv = xcalloc(sizeof(int) * MAXDRV, IM_BASE);
    if (!fast_itdrv) return 0;
    xlog("Allocated fast_itdrvs: %.2fM (%d*%d)", sizeof(int) * MAXDRV / 1024.0 / 1024.0, (int)(sizeof(int)), MAXDRV);
    mem_usage += sizeof(int) * MAXDRV;

    dir = opendir("runtime/generic");
    if (dir) {
        while ((de = readdir(dir))) {
            if (!endcmp(de->d_name, ".dll")) continue;

            sprintf(name, "./runtime/generic/%s", de->d_name);

            add_lib(name);
        }
        closedir(dir);
    }

    sprintf(dirname, "runtime/%d", areaID);
    dir = opendir(dirname);
    if (dir) {
        while ((de = readdir(dir))) {
            if (!endcmp(de->d_name, ".dll")) continue;

            sprintf(name, "./%s/%s", dirname, de->d_name);

            add_lib(name);
        }
        closedir(dir);
    }

    return 1;
}

void exit_lib(void) {
    xfree(fast_chdrv);
    xfree(fast_itdrv);
    xfree(libs);
}

int char_driver(int nr, int type, int cn, intptr_t ret, int last_action) {
    int n, val = 0;

    if (nr == 0) {
        if (type == 0) player_driver(cn, ret, last_action);
        return 1;
    }

    if (nr < 0 || nr >= MAXDRV) {
        elog("ERROR: Character driver %d out of bounds", nr);
        return 0;
    }

    if (nr == CDR_SKILLQUEST) return skillquest_driver(type, cn, ret, last_action); // core, all areas

    if ((n = fast_chdrv[nr]) && libs[n].used && load_lib(libs + n) && (val = libs[n].driver(type, nr, cn, ret, last_action))) return val;

    for (n = 1; n < MAXLIB; n++)
        if (libs[n].used && load_lib(libs + n) && (val = libs[n].driver(type, nr, cn, ret, last_action))) break;

    if (n == MAXLIB) {
        elog("ERROR: Could not find character driver %d in any library", nr);
        return 0;
    }

    fast_chdrv[nr] = n;

    return val;
}

int item_driver(int nr, int in, int cn) {
    int n, tmp = 0;

    if (nr == 0) {
        if (cn) look_item(cn, it + in, -1);
        return 1;
    }

    if (nr >= 1000) return 1; // identity tag

    if (nr < 0 || nr >= MAXDRV) {
        elog("ERROR: Item driver %d out of bounds", nr);
        return 0;
    }

    if (nr == IDR_SKROCK || nr == IDR_SKSTACK || nr == IDR_SKSTATION) return skilling_item_driver(nr, in, cn); // core, all areas

    switch (nr) {
    case IDR_BONEBRIDGE:
        if (areaID != 18) {
            if (cn) log_char(cn, LOG_SYSTEM, 0, "This does not work outside its area.");
            return 1;
        }
        break;
    case IDR_BONEHINT:
        if (areaID != 18) {
            if (cn) log_char(cn, LOG_SYSTEM, 0, "This does not work outside its area.");
            return 1;
        }
        break;
    case IDR_NOMADDICE:
        if (areaID != 19) {
            if (cn) log_char(cn, LOG_SYSTEM, 0, "This does not work outside its area.");
            return 1;
        }
        break;
    case IDR_STAFFER2:
        if (areaID != 29) {
            if (cn) log_char(cn, LOG_SYSTEM, 0, "This does not work outside its area.");
            return 1;
        }
        break;
    case IDR_OXYPOTION:
        if (areaID != 31) {
            if (cn) log_char(cn, LOG_SYSTEM, 0, "This does not work outside its area.");
            return 1;
        }
        break;
    case IDR_LIZARDFLOWER:
        if (areaID != 31) {
            if (cn) log_char(cn, LOG_SYSTEM, 0, "This does not work outside its area.");
            return 1;
        }
        break;
    case IDR_CALIGAR:
        if (areaID != 36) {
            if (cn) log_char(cn, LOG_SYSTEM, 0, "This does not work outside its area.");
            return 1;
        }
        break;
    case IDR_RWWKEY:
        if (areaID != 25) {
            if (cn) log_char(cn, LOG_SYSTEM, 0, "This does not work outside its area.");
            return 1;
        }
        break;
    case IDR_ARKHATA:
        if (areaID != 37) {
            if (cn) log_char(cn, LOG_SYSTEM, 0, "This does not work outside its area.");
            return 1;
        }
        break;
    }

    if ((n = fast_itdrv[nr]) && libs[n].used && load_lib(libs + n) && (tmp = libs[n].driver(CDT_ITEM, nr, in, cn, 0))) return tmp;

    for (n = 1; n < MAXLIB; n++)
        if (libs[n].used && load_lib(libs + n) && (tmp = libs[n].driver(CDT_ITEM, nr, in, cn, 0))) break;

    if (n == MAXLIB) {
        elog("ERROR: Could not find item driver %d in any library", nr);
        return 0;
    }

    fast_itdrv[nr] = n;

    return tmp;
}
