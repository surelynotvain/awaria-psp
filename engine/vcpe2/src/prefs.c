/* PlayerPrefs: int / float values by key, saved as SETTINGS.BIN next to the EBOOT.
 *
 * Unity's PlayerPrefs: a small key/value table loaded once and saved whole after a change. File layout: magic, count,
 * then count fixed-size records. A file that does not check out (wrong magic, too many records, cut short) loads as
 * "no settings", so the game starts with its defaults. 5.6: files through vcpe_plat. */
#include "vcpe_plat.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "prefs.h"

#define MAXP 96
typedef struct { char key[24]; uint8_t kind; int32_t i; float f; } Pref;   /* kind 1 int, 2 float */
static Pref prefs[MAXP];
static int nprefs;
static char path[300];
static int dirty;

#define MAGIC 0x31505741   /* "AWP1" */

void prefs_load(const char *file)
{
    snprintf(path, sizeof path, "%s", file);
    nprefs = 0;
    int fd = vcpe_file_open(path);
    if (fd < 0)
        return;
    uint32_t hdr[2];
    if (vcpe_file_read(fd, hdr, 8) == 8 && hdr[0] == MAGIC && hdr[1] <= MAXP) {
        int n = vcpe_file_read(fd, prefs, sizeof(Pref) * hdr[1]);
        if (n == (int)(sizeof(Pref) * hdr[1]))
            nprefs = hdr[1];
    }
    vcpe_file_close(fd);
    for (int i = 0; i < nprefs; i++)
        prefs[i].key[sizeof prefs[i].key - 1] = 0;
}

void prefs_save(void)
{
    if (!dirty || !path[0])
        return;
    int fd = vcpe_file_create(path, 0);
    if (fd < 0)
        return;
    uint32_t hdr[2] = {MAGIC, (uint32_t)nprefs};
    uint32_t body = sizeof(Pref) * (uint32_t)nprefs;
    int ok = vcpe_file_write(fd, hdr, 8) == 8 && vcpe_file_write(fd, prefs, body) == (int)body;
    vcpe_file_close(fd);
    dirty = !ok;   /* a failed write is tried again at the next save */
}

/* Find a key (linear search: there are only a few), creating it if asked. */
static Pref *find(const char *key, int create)
{
    for (int i = 0; i < nprefs; i++)
        if (!strcmp(prefs[i].key, key))
            return &prefs[i];
    if (!create || nprefs >= MAXP)
        return 0;
    Pref *p = &prefs[nprefs++];
    memset(p, 0, sizeof *p);
    strncpy(p->key, key, sizeof p->key - 1);
    return p;
}

int prefs_int(const char *key, int def)
{
    Pref *p = find(key, 0);
    return p ? (p->kind == 2 ? (int)p->f : p->i) : def;
}

float prefs_float(const char *key, float def)
{
    Pref *p = find(key, 0);
    return p ? (p->kind == 1 ? (float)p->i : p->f) : def;
}

void prefs_set_int(const char *key, int v)
{
    Pref *p = find(key, 1);
    if (p && (p->kind != 1 || p->i != v)) {
        p->kind = 1;
        p->i = v;
        dirty = 1;
    }
}

void prefs_set_float(const char *key, float v)
{
    Pref *p = find(key, 1);
    if (p && (p->kind != 2 || p->f != v)) {
        p->kind = 2;
        p->f = v;
        dirty = 1;
    }
}

int prefs_has(const char *key) { return find(key, 0) != 0; }
