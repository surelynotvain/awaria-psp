#!/usr/bin/env python3
"""Build a LANG.PAK (a translation) for the Awaria PSP port.

    python mklang.py MYLANG_FOLDER [-o LANG.PAK] [--name Deutsch] [--font extra.ttf ...]

MYLANG_FOLDER holds any of the game's text files, translated: m.json ch.json g.json 1.json .. 13.json
(the same files as Awaria/local on PC, one entry per line, UTF-8 or UTF-16), and optionally
psp.json (PSP wording of PC-only prompts) and static.json (texts that are part of scenes).
Anything missing is taken from the English files in langkit/english.

Copy the resulting LANG.PAK next to EBOOT.PBP (PSP/GAME/Awaria/).  Delete it to get English back.
Needs Python 3 with Pillow and numpy (pip install pillow numpy)."""
import argparse, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import langpak


def main():
    ap = argparse.ArgumentParser(description="Build LANG.PAK for the Awaria PSP port")
    ap.add_argument("folder", help="folder with the translated text files")
    ap.add_argument("-o", "--out", default="LANG.PAK")
    ap.add_argument("--name", default=None, help="language name stored in the file (default: folder name)")
    ap.add_argument("--font", action="append", default=[],
                    help="TTF/OTF for characters the game fonts lack (CJK, Arabic, ...); may repeat")
    ap.add_argument("--kit", default=os.path.join(HERE, "langkit"), help="langkit folder (default: next to this script)")
    a = ap.parse_args()

    fontsets, faces, english, psp_en, static_orig = langpak.load_kit(a.kit)
    texts, problems = {}, []
    for nm in langpak.TEXT_FILES:
        p = os.path.join(a.folder, nm + ".json")
        if not os.path.exists(p):
            texts[nm] = english[nm]
            print("  %-8s not found, using English" % (nm + ".json"))
            continue
        lines = langpak.read_lines(p)
        n_en = len(english[nm])
        # a trailing empty line is just the file's last newline
        while len(lines) > n_en and lines[-1] == "":
            lines.pop()
        if len(lines) != n_en:
            problems.append("%s.json has %d lines, English has %d: lines are matched by number, check for "
                            "added/removed lines" % (nm, len(lines), n_en))
            lines = (lines + english[nm][len(lines):])[:n_en]
        texts[nm] = lines
        print("  %-8s %d lines" % (nm + ".json", len(lines)))

    psp_path = os.path.join(a.folder, "psp.json")
    psp = json.load(open(psp_path, encoding="utf-8")) if os.path.exists(psp_path) else psp_en
    texts = langpak.apply_psp(texts, psp)
    static_path = os.path.join(a.folder, "static.json")
    static = {}
    if os.path.exists(static_path):
        static = {k: v for k, v in json.load(open(static_path, encoding="utf-8")).items() if not k.startswith("_")}
        unknown = [k for k in static if k not in static_orig]
        for k in unknown:
            problems.append("static.json: %r is not a text of the game (keys must stay English)" % k)

    name = a.name or os.path.basename(os.path.normpath(a.folder))
    data, missing = langpak.build(fontsets, faces, texts, static, static_orig, name, a.font)
    open(a.out, "wb").write(data)
    print("wrote %s (%s, %d KB)" % (a.out, name, len(data) // 1024))
    used = set("".join(l for lines in texts.values() for l in lines)) | set("".join(static.values()))
    missing = {c: v for c, v in missing.items() if c in used}
    if missing:
        cs = "".join(sorted(missing))
        problems.append("%d characters have no glyph in the game fonts%s and will show as '?': %s"
                        % (len(cs), " or --font" if a.font else " (add one with --font some.ttf)", cs[:200]))
    for p in problems:
        print("WARNING:", p)


if __name__ == "__main__":
    main()
