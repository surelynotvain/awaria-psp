# Awaria PSP engine: Vain's C Portable Engine 2

This folder contains the source code that runs Awaria PSP: plain C for the PlayStation Portable, written from scratch.

**Vain's C Portable Engine** (VCPE) is the PSP engine shared by my two demakes:

- **VCPE 1**, the base: runs [Helltaker PSP](https://github.com/surelynotvain/helltaker-psp).
- **VCPE 2**, this one: the same base plus an advanced layer, a small Unity-style runtime that plays Unity scenes converted for the PSP. Awaria PSP is the first game on it.

It contains **no game assets and no game data**. Graphics, sound, music, text and scenes all come from the player's own copy of Awaria and are not part of this repository.

## Layout

| Folder | What it is |
|---|---|
| `vcpe/src` | VCPE base (the same files as in the Helltaker PSP repository) |
| `vcpe2/src` | VCPE 2 advanced layer: the Unity-style runtime, the script layer and the sprite and text renderer |
| `src` | Awaria itself: the game scripts ported from the original, its sound rules and the main loop |

### VCPE base (`vcpe/src`)

| File | Purpose |
|---|---|
| `vcpe.h` | The one header games include |
| `vcpe_sys.c/.h` | HOME and sleep callbacks, CPU clock, paths next to the EBOOT |
| `vcpe_pak.c/.h` | Data PAK reader that survives sleep/resume and slow memory sticks |
| `vcpe_gfx.c/.h` | GU setup, frames, texture bundles and paletted swizzled pages, cached render state, quads, fills, strips, clipping |
| `vcpe_audio.c/.h` | Software mixer: ADPCM sound effect voices and streamed ADPCM music, volume and fades |
| `vcpe_test.c/.h` | Scripted test input (`AUTOTEST.TXT`) and frame recording for repeatable runs |

### VCPE 2 advanced layer (`vcpe2/src`)

| File | Purpose |
|---|---|
| `w_world.c/.h`, `awfmt.h` | GameObjects, transforms, RectTransforms and components loaded from a converted scene ("world") blob, prefab instancing, SetActive |
| `w_hooks.h` | Callbacks between the world and the runtime modules |
| `w_anim.c/.h` | Mecanim reimplementation: parameters, triggers, exit times, write defaults, curves bound to scene objects, animation events |
| `w_phys.c/.h` | Physics2D subset: Rigidbody2D, circle/box/capsule colliders, wall tiles, layer matrix, trigger and collision events, OverlapCircle, Raycast, CircleCast (fixed 50 Hz step) |
| `w_path.c/.h` | A* grid pathfinding (AstarPath scan and path requests) |
| `w_render.c/.h` | Draw list sorted like Unity: sorting layers, order in layer, custom Y axis; sprites, tilemaps, trails, lines, canvases |
| `w_part.c` | ParticleSystem subset |
| `g_core.c/.h` | Script layer: per-type behaviour tables (Awake, Start, Update, FixedUpdate, events), game time and time scale, coroutine helper |
| `prefs.c/.h` | PlayerPrefs |
| `aw_gfx.c/.h` | Sprites (affine, tiled, sliced), UI images and masks, glyph text with SDF fonts, language packs (`LANG.PAK`) |

The advanced layer was written for Awaria first, so some files still include the game's generated data header and script table. Making it fully game-independent is future work.

### Awaria (`src`)

| File | Purpose |
|---|---|
| `main.c` | Startup, input, main loop, test commands and the test bot |
| `aw_audio.c/.h` | Awaria's sound rules on the VCPE mixer (long, paused and short sound pools, music muffling) |
| `g_mgr.c` | Manager (level flow, saves), camera, transitions, interaction |
| `g_player.c` | Player, the level loop and the full-screen sequences |
| `g_machines.c` | Generators, part machines and hitboxes |
| `g_ai.c` | Ghost AIs (Zmora, Striga, Minion, Dogo, Cutwire, Nikita, Doppel) |
| `g_boss.c` | The final boss |
| `g_weapons.c` | Ghost weapons and hazards: projectiles, bullet spreads, shockwaves, thunder, ice, scrap, traps, arms, boilers |
| `g_menu.c` | Menus, buttons and cutscenes |
| `g_scripts.c` | Behaviour table: one entry per script type |
| `g_game.h`, `g_input.h`, `g_vtlist.h` | Shared state, input and the script type list |

## Building

The engine is built with the [pspdev](https://github.com/pspdev/pspdev) toolchain (PSPSDK) as a standard user-mode PSP application. Compile the files of all three folders together, with all three on the include path.

The code also needs a generated header and source, `awdata.h` and `awdata.c`, with the converted game data (sprite tables, animations, scene and prefab tables, text). They are produced from an Awaria installation by a separate converter, which is not published, so the engine does not build on its own from this repository.

## Notes

Awaria was created by vanripper. All original names, characters, artwork, music and related assets belong to their respective rights holders. This engine is an unofficial fan reimplementation.
