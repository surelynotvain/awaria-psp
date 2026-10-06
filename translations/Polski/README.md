# Polski (Polish)

Translation by **EnderSpulka** (originally made for the PC version of Awaria, translation version 9).

The `.json` files are the translator's original text files. `psp.json` (the PSP dash prompt) and
`static.json` (texts placed in the game's scenes) were added for the PSP port.

Build the `LANG.PAK` with the translation kit:

```
python mklang.py path/to/translations/Polski --name Polski
```

Then copy `LANG.PAK` next to `EBOOT.PBP` in `PSP/GAME/Awaria/`.
