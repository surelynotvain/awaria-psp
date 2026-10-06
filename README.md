# awaria-psp

A work-in-progress C demake of **Awaria** by **vanripper** for the **PlayStation Portable (PSP)**.

## Current Progress

- Finished the XMB artwork
- Imported the menu logic
- Created the XMB icon
- Extracted the required game assets
- Converted and formatted the music for PSP
- Implemented a working translation package system

The game supports external translation packages using a `LANG.PAK` file.

If `LANG.PAK` is present in the same folder as the game, it will be loaded automatically. If no `LANG.PAK` is found, the game falls back to the built-in English translation stored inside `AW.PAK`.

The translation system covers the game's text, excluding text that is pre-rendered directly into image assets from the original game.

### Planned Languages at Launch

- English -- built into `AW.PAK`
- French
- German
- Polish
- Spanish

More translations can be added through custom `LANG.PAK` files.

## XMB Art

<img width="480" height="272" alt="Awaria PSP XMB artwork" src="https://github.com/user-attachments/assets/3f8387d2-7f38-4e89-94d7-f9a273e3bc16" />

## Files

<img width="593" height="172" alt="Awaria PSP project files" src="https://github.com/user-attachments/assets/3f8c53d2-1d5d-4ea3-b8ad-1550b63a8591" />

## Status

**Work in progress.**

The project is currently under active development. Some systems are already implemented and working, but the game is not yet complete.

## Credits

**Awaria** was originally created by **vanripper**.

This project is an unofficial fan-made C remake/PSP port and is not affiliated with or endorsed by vanripper.
