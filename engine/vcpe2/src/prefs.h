#ifndef PREFS_H
#define PREFS_H
#include <stdint.h>
void prefs_load(const char *file);
void prefs_save(void);
int prefs_int(const char *key, int def);
float prefs_float(const char *key, float def);
void prefs_set_int(const char *key, int v);
void prefs_set_float(const char *key, float v);
int prefs_has(const char *key);
#endif
