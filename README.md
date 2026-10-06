# awaria-psp

A work-in-progress C demake of **Awaria** by **vanripper** for the **PlayStation Portable (PSP)**.

## Current Progress

- All 13 chapters, the final boss, cutscenes and the credits are playable
- Main menu, chapter select, difficulty modes, pause menu and saves
- Finished the XMB artwork, animated XMB icon and XMB music
- Converted and formatted the music for PSP
- Performance pass for real hardware (chapter 5 and the finale)
- Working translation package system (`LANG.PAK`) with public translation tools
- Currently being playtested on a real PSP-3000 before release

## Demo

A playable **demo with Chapter 1 and Chapter 2** is on the [Releases page](https://github.com/surelynotvain/awaria-psp/releases). The rest of the chapters are not finished yet.

**PSP-1000 owners:** please try the demo and [open an issue](https://github.com/surelynotvain/awaria-psp/issues) to tell me if it works (starts, runs smoothly, any crash or freeze). It is tested on a PSP-3000 and in PPSSPP so far.

This demake is heavily based on the code of my PSP Helltaker demake: [helltaker-psp](https://github.com/surelynotvain/helltaker-psp).

## Translations

The game supports external translation packages using a `LANG.PAK` file.

If `LANG.PAK` is present in the same folder as the game, it will be loaded automatically. If no `LANG.PAK` is found, the game falls back to the built-in English translation stored inside `AW.PAK`.

With a `LANG.PAK` installed, the main menu shows **Settings > Language**, which switches between the translation and English at any time (the choice is saved).

The translation system covers the game's text (menus, chapter briefings, dialogue and the texts placed in the game's scenes), excluding text that is pre-rendered directly into image assets from the original game.

### Make your own translation

Anyone can make a `LANG.PAK`, no PSP toolchain needed:

1. Get `Awaria-PSP-LangKit.zip` (the tools plus the fonts and English text files). It will be on the [Releases page](https://github.com/surelynotvain/awaria-psp/releases) together with the game
2. Translate the game's text files (the same files as `Awaria/local` in the PC game, one text per line). Existing PC translations work as they are
3. Build it: `python mklang.py MyLanguage --name MyLanguage` (needs Python 3 with `pip install pillow numpy`)
4. Copy `LANG.PAK` next to `EBOOT.PBP` in `PSP/GAME/Awaria/`

Latin alphabets with accents, Greek and Cyrillic work out of the box. Chinese, Japanese and Korean work with an extra font (`--font some.ttf`).

The full step by step guide is in [translation/TRANSLATING.md](translation/TRANSLATING.md). The tool sources are in [translation/](translation/).

### Languages

Ready (ready-made `LANG.PAK` files are on the [Releases page](https://github.com/surelynotvain/awaria-psp/releases)):

- English -- built into `AW.PAK`
- Polish -- by **EnderSpulka**, in [translations/Polski](translations/Polski)
- Spanish -- by **mod3us**, in [translations/Espanol](translations/Espanol)
- French -- by **Supershadow30**, in [translations/Francais](translations/Francais)

Planned:

- German

Translations live in [translations/](translations/), one folder per language (the translated text files). Build a `LANG.PAK` from any of them with the translation kit. Made one? Open an issue and share it.

## XMB Art

<img width="480" height="272" alt="Awaria PSP XMB artwork" src="https://github.com/user-attachments/assets/3f8387d2-7f38-4e89-94d7-f9a273e3bc16" />

## Files

<img width="593" height="172" alt="Awaria PSP project files" src="https://github.com/user-attachments/assets/3f8c53d2-1d5d-4ea3-b8ad-1550b63a8591" />

## Status

**Work in progress.**

A demo with Chapter 1 and Chapter 2 is out. The other chapters are still being finished and tested on real hardware before the full release. The translation tools and the guide are published so translators can get started before launch.

## Credits

**Awaria** was originally created by **vanripper**.

PSP port by **surelynotvain**.

Translations: Polish by **EnderSpulka**, Spanish by **mod3us**, French by **Supershadow30**. Thank you!

This project is an unofficial fan-made C remake/PSP port and is not affiliated with or endorsed by vanripper.
