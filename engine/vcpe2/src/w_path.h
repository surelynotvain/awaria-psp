#ifndef W_PATH_H
#define W_PATH_H
void path_build(void);      /* AstarPath.Scan on scene load */
void path_free(void);
/* path from (sx,sy) to (tx,ty): node centres into out[2*i], returns the count (0 = none) */
int path_find(float sx, float sy, float tx, float ty, float *out, int max);
#endif
