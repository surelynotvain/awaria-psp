/* PlayerPrefs: int / float / string values by key, saved as SETTINGS.BIN next to the EBOOT. */
#include <pspiofilemgr.h>
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
    int fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return;
    uint32_t hdr[2];
    if (sceIoRead(fd, hdr, 8) == 8 && hdr[0] == MAGIC && hdr[1] <= MAXP) {
        int n = sceIoRead(fd, prefs, sizeof(Pref) * hdr[1]);
        if (n == (int)(sizeof(Pref) * hdr[1]))
            nprefs = hdr[1];
    }
    sceIoClose(fd);
    for (int i = 0; i < nprefs; i++)
        prefs[i].key[sizeof prefs[i].key - 1] = 0;
}

void prefs_save(void)
{
    if (!dirty || !path[0])
        return;
    int fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return;
    uint32_t hdr[2] = {MAGIC, (uint32_t)nprefs};
    sceIoWrite(fd, hdr, 8);
    sceIoWrite(fd, prefs, sizeof(Pref) * nprefs);
    sceIoClose(fd);
    dirty = 0;
}

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
