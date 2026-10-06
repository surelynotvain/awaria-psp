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

## Translations

The game supports external translation packages using a `LANG.PAK` file.

If `LANG.PAK` is present in the same folder as the game, it will be loaded automatically. If no `LANG.PAK` is found, the game falls back to the built-in English translation stored inside `AW.PAK`.

The translation system covers the game's text (menus, chapter briefings, dialogue and the texts placed in the game's scenes), excluding text that is pre-rendered directly into image assets from the original game.

### Make your own translation

Anyone can make a `LANG.PAK`, no PSP toolchain needed:

1. Download `Awaria-PSP-LangKit.zip` from the [Releases page](https://github.com/surelynotvain/awaria-psp/releases)
2. Translate the game's text files (the same files as `Awaria/local` in the PC game, one text per line). Existing PC translations work as they are
3. Build it: `python mklang.py MyLanguage --name MyLanguage` (needs Python 3 with `pip install pillow numpy`)
4. Copy `LANG.PAK` next to `EBOOT.PBP` in `PSP/GAME/Awaria/`

Latin alphabets with accents, Greek and Cyrillic work out of the box. Chinese, Japanese and Korean work with an extra font (`--font some.ttf`).

The full step by step guide is in [translation/TRANSLATING.md](translation/TRANSLATING.md). The tool sources are in [translation/](translation/).

### Planned Languages at Launch

- English -- built into `AW.PAK`
- French
- German
- Polish
- Spanish

More translations can be added through custom `LANG.PAK` files. Made one? Open an issue and share it.

## XMB Art

<img width="480" height="272" alt="Awaria PSP XMB artwork" src="https://github.com/user-attachments/assets/3f8387d2-7f38-4e89-94d7-f9a273e3bc16" />

## Files

<img width="593" height="172" alt="Awaria PSP project files" src="https://github.com/user-attachments/assets/3f8c53d2-1d5d-4ea3-b8ad-1550b63a8591" />

## Status

**Work in progress.**

The whole game is playable from start to finish and is now in real hardware testing. The game itself is not released yet; the translation tools are available now so translations can be ready at launch.

## Credits

**Awaria** was originally created by **vanripper**.

This project is an unofficial fan-made C remake/PSP port and is not affiliated with or endorsed by vanripper.
