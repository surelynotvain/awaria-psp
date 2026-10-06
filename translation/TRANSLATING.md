# Translating the Awaria PSP port

The PSP port reads all of its text from one file, `LANG.PAK`. If `LANG.PAK` sits next to
`EBOOT.PBP`, the game uses it. If it is missing (or made for a different version), the game
uses its built-in English.

You make a `LANG.PAK` from the game's own text files with the `mklang.py` script of the
**translation kit**. Download `Awaria-PSP-LangKit.zip` from the
[Releases page](https://github.com/surelynotvain/awaria-psp/releases) and unzip it. It contains:

```
Awaria-PSP-LangKit/
  mklang.py         builds LANG.PAK
  langpak.py        used by mklang.py
  langkit/          game fonts and the English text files
  TRANSLATING.md    this guide
```

You do not need the PSP toolchain or the PC game installed. You need **Python 3** with
**Pillow** and **numpy**:

```
pip install pillow numpy
```

Use the kit from the same release as the game version you translate for (see section 6).

## 1. Get the text files

The game's text is in 16 plain text files. You can take them from either place:

* this kit: `langkit/english/`
* your PC copy of Awaria: in Steam, right click Awaria > Manage > Browse local files,
  then open the `local` folder

| File | What is in it |
|---|---|
| `m.json` | menus, buttons, pause menu, HUD, game over and victory texts |
| `ch.json` | chapter names and the "Word from your boss" briefings |
| `g.json` | gallery page texts |
| `1.json` ... `13.json` | the dialogue of chapter 1 ... 13 |

Despite the name, these are **not JSON**. Each line is one text, and the game finds a text by its
line number. So:

* keep every line where it is: do not add, remove, merge or split lines
* keep empty lines empty
* texts like `[ Zmora ]` are speaker names, translate them or keep them as you like

Already have a translation of the PC game? Use its `local` files as they are.

## 2. Translate

Make a folder for your language, for example `Deutsch`, and put the files you translated in it.
You do not need all 16: any file that is missing stays English. Save as UTF-8 (UTF-16, as the
PC game uses, works too).

```
Deutsch/
  m.json
  ch.json
  1.json
  ...
```

Two optional files cover text that is not in the 16 files:

* **`psp.json`**: lines the port changes for the PSP, for example the dash prompt
  `[button: SQUARE / R]` instead of the PC key. Copy `langkit/english/psp.json` and translate the
  texts. The numbers are line numbers counted from 0 (line 1 in your editor is `"0"`).
* **`static.json`**: texts that are part of the game's scenes instead of the text files, like
  `READY TO KISS`, `GET READY!`, `LOW TIER Specter` and the gallery notes. Copy
  `langkit/english/static.json`, keep the left side (English) as it is and translate the right side.
  Entries you leave unchanged stay English.

## 3. Build LANG.PAK

From the kit folder:

```
python mklang.py Deutsch --name Deutsch
```

This writes `LANG.PAK`. Read what it prints:

* `xx.json has N lines, English has M`: a line was added or removed somewhere; texts after that
  point will show up in the wrong places. Fix the file and build again.
* `characters have no glyph`: see section 5.
* `static.json: '...' is not a text of the game`: the left side of that entry was changed.

## 4. Install

Copy `LANG.PAK` to the memory stick, next to the game:

```
PSP/GAME/Awaria/
  EBOOT.PBP
  AW.PAK
  MUSIC.PAK
  LANG.PAK      <- your translation
```

To go back to English, delete `LANG.PAK`. You can also test in PPSSPP the same way.

## 5. Letters and alphabets

Built in, no extra steps: all Latin letters with accents (German, French, Spanish, Italian,
Portuguese, Polish, Czech, Hungarian, Turkish, Nordic, ...), Greek and Cyrillic (Russian,
Ukrainian, Bulgarian, Serbian, ...).

Other alphabets (Chinese, Japanese, Korean, ...) need a font that has them. Pass any TTF or OTF
font file and the letters the game fonts lack are taken from it:

```
python mklang.py Japanese --name Japanese --font NotoSansJP-Regular.otf
```

Not supported: right-to-left scripts (Arabic, Hebrew) and scripts that need letter shaping
(Thai, Devanagari, ...). Letters a font does not have show as `?`.

## 6. Good to know

* Text that is part of the artwork stays English, as in the PC game: the AWARIA logo,
  "REPAIR COMPLETE", the "KISS SECURED" and "WARNING / PARANORMAL" splashes, the ghost name
  cards and the gallery pictures.
* Longer texts wrap inside their boxes, but one-line labels (menu buttons, chapter names)
  have limited room. Check your translation on a PSP or in PPSSPP and shorten what does not fit.
* Upper case styled titles (like the pause menu title) are converted for all the alphabets above.
* A `LANG.PAK` belongs to one version of the port. When a new version comes out, build again with
  the kit from that version (your translated files stay the same).

## 7. Share it

Share the `LANG.PAK`, and ideally your folder of translated files too, so others can rebuild it for
future versions.
