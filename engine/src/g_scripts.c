/* Behaviour table: one ScriptVT per script type (types without behaviour stay NULL). */
#include "g_core.h"

#define VT(name) extern const ScriptVT vt_##name;
#include "g_vtlist.h"
#undef VT

const ScriptVT *const g_vt_table[NUM_SCRIPT_TYPES] = {
#define VT(name) [ST_##name] = &vt_##name,
#include "g_vtlist.h"
#undef VT
};
