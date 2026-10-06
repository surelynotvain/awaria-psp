"""LANG.PAK: everything language dependent in the Awaria PSP port, in one file.

  * the 16 text files of the PC game (local/m.json, ch.json, g.json, 1.json .. 13.json; one entry per line)
  * PSP wording for PC-only prompts (psp.json) and translations of strings baked into scenes (static.json)
  * glyph pages for exactly the characters those texts use, in every font size the game draws

The converter (awpack.py) builds the built-in English one with this module and stores it inside AW.PAK;
mklang.py builds translations from a folder of edited text files.  Only Pillow and numpy are needed:
the fonts come from the language kit (langkit/fonts: the game's TextMeshPro SDF atlases), plus optional
TrueType fonts for scripts those atlases do not cover (CJK, ...).

Layout (little endian, offsets from the start of the file, glyph pages 64-byte aligned):
  header  "AWLG" u32 version, u16 nfontsets, u16 npages, u32 nglyphs,
          u32 fontsets, glyphs, pages, clut, u32 ntext, text, u32 nstatic, static, u32 kit, char name[28]
          (kit = kit_id(fontsets): a LANG.PAK only fits the port version whose kit built it)
  FontSet {u16 first, n, line, ascent; u8 face, px}             line/ascent in px*16
  Glyph   {u8 page, w, h, 0; u16 u, v; s16 bx, by; u16 adv; u32 codepoint}   (sorted by codepoint per set)
  FontPage{u16 w, h; u32 off}   8-bit alpha, PSP-swizzled, CLUT = 256 alpha steps of white
  Text    {u32 off, size, nlines}   lines are UTF-8, each ended by '\\0'
  Static  {u32 crc32(original), u32 off}   sorted by crc; translated UTF-8 string, '\\0' ended
"""
import json, math, os, struct, zlib
import numpy as np
from PIL import Image, ImageFont, ImageDraw

MAGIC = b"AWLG"
VERSION = 1
TEXT_FILES = ["m", "ch", "g"] + [str(i) for i in range(1, 14)]
BASE_CHARS = set(chr(c) for c in range(32, 127)) | set("\u2605\u2022\u2026\u2019\u2018\u201c\u201d\u2014\u2013")   # star, bullet, ellipsis, quotes, dashes
HDR = "<4sIHHIIIIIIIIII28s"


def kit_id(fontsets):
    """identifies the port version a kit belongs to (font sets are indexed by the game data)"""
    return zlib.crc32(json.dumps([list(f) for f in fontsets]).encode()) & 0xFFFFFFFF


def pad64(b):
    while len(b) % 64:
        b.append(0)
    return b


def pow2(v):
    p = 8
    while p < v:
        p <<= 1
    return p


def swizzle(data, width_bytes, height):
    a = np.frombuffer(data, np.uint8).reshape(height, width_bytes)
    return a.reshape(height // 8, 8, width_bytes // 16, 16).transpose(0, 2, 1, 3).tobytes()


class Skyline:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.sky = [(0, 0, w)]

    def place(self, rw, rh):
        best = None
        for i, (x, y, w) in enumerate(self.sky):
            if x + rw > self.w:
                break
            top = 0; span = 0; j = i
            while span < rw and j < len(self.sky):
                top = max(top, self.sky[j][1]); span += self.sky[j][2]; j += 1
            if span < rw or top + rh > self.h:
                continue
            if best is None or top + rh < best[1] + rh or (top == best[1] and x < best[0]):
                best = (x, top)
        if best is None:
            return None
        x, y = best
        new = []
        for (sx, sy, sw) in self.sky:
            e = sx + sw
            if e <= x or sx >= x + rw:
                new.append((sx, sy, sw))
            else:
                if sx < x: new.append((sx, sy, x - sx))
                if e > x + rw: new.append((x + rw, sy, e - (x + rw)))
        new.append((x, y + rh, rw))
        new.sort()
        merged = []
        for seg in new:
            if merged and merged[-1][1] == seg[1] and merged[-1][0] + merged[-1][2] == seg[0]:
                merged[-1] = (merged[-1][0], seg[1], merged[-1][2] + seg[2])
            else:
                merged.append(seg)
        self.sky = merged
        return best


# ---------------------------------------------------------------- text files
def read_lines(path):
    """A PC text file: UTF-16 (with BOM) or UTF-8, one entry per line."""
    raw = open(path, "rb").read()
    s = raw.decode("utf-16") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else raw.decode("utf-8-sig")
    return s.replace("\r\n", "\n").split("\n")


def write_lines(path, lines):
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(lines))


# ---------------------------------------------------------------- fonts
class SdfFace:
    """A TextMeshPro SDF atlas exported to langkit/fonts (alpha PNG + glyph metrics json)."""

    def __init__(self, base):
        meta = json.load(open(base + ".json", encoding="utf-8"))
        self.name = meta["name"]
        self.point = meta["pointSize"]
        self.line = meta["lineHeight"]
        self.ascent = meta["ascent"]
        self.pad = meta["padding"]
        self.glyphs = {int(k): v for k, v in meta["glyphs"].items()}
        self.A = np.asarray(Image.open(base + ".png").convert("L")).astype(np.float32) / 255.0

    def render(self, cp, px):
        g = self.glyphs.get(cp)
        if g is None:
            return None
        x, y, w, h, bxu, byu, advu, gscale = g
        s = px / self.point
        adv = advu * s
        if w == 0 or h == 0:
            return None, 0, 0, adv
        A, pad = self.A, self.pad
        AH = A.shape[0]
        # glyph rect (bottom-left origin) expanded by the SDF padding, in top-down image rows
        x0, y0 = x - pad, AH - (y + h) - pad
        x1, y1 = x + w + pad, AH - y + pad
        tw, th = max(1, int(math.ceil((x1 - x0) * s))), max(1, int(math.ceil((y1 - y0) * s)))
        reg = Image.fromarray((np.clip(A[max(0, y0):y1, max(0, x0):x1], 0, 1) * 255).astype(np.uint8), "L")
        v = np.asarray(reg.resize((tw, th), Image.BILINEAR)).astype(np.float32) / 255.0
        k = (pad + 1) * s
        gim = Image.fromarray((np.clip((v - 0.5) * 2.0 * k + 0.5, 0, 1) * 255).astype(np.uint8), "L")
        bb = gim.getbbox()
        if not bb:
            return None, 0, 0, adv
        gim = gim.crop(bb)
        bx = (bxu - pad / gscale) * s + bb[0]
        by = -(byu + pad / gscale) * s + bb[1]
        return gim, bx, by, adv


class TtfFace:
    """Any TrueType/OpenType font, used for characters the game's atlases lack."""

    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        self.cache = {}
        try:
            from fontTools.ttLib import TTFont      # exact coverage when fontTools is around
            self.cmap = set(TTFont(path, fontNumber=0).getBestCmap())
        except Exception:
            self.cmap = None

    def font(self, px):
        if px not in self.cache:
            self.cache[px] = ImageFont.truetype(self.path, max(1, round(px)))
        return self.cache[px]

    def has(self, cp):
        if self.cmap is not None:
            return cp in self.cmap
        f = self.font(32)
        if not hasattr(self, "notdef"):
            nd = f.getmask(chr(0x10FFFD))
            self.notdef = (nd.size, bytes(nd))
        m = f.getmask(chr(cp))
        return m.getbbox() is not None and (m.size, bytes(m)) != self.notdef

    def render(self, cp, px):
        if not self.has(cp):
            return None
        f = self.font(px)
        ch = chr(cp)
        adv = f.getlength(ch)
        l, t, r, b = f.getbbox(ch, anchor="ls")
        if r <= l or b <= t:
            return None, 0, 0, adv
        im = Image.new("L", (r - l + 2, b - t + 2), 0)
        ImageDraw.Draw(im).text((1 - l, 1 - t), ch, font=f, fill=255, anchor="ls")
        bb = im.getbbox()
        if not bb:
            return None, 0, 0, adv
        return im.crop(bb), l - 1 + bb[0], t - 1 + bb[1], adv


class BitmapFace:
    """Glyphs pre-rendered per pixel size (langkit/fonts/symbols: punctuation and the star the game takes
    from its big CJK fallback font)."""

    def __init__(self, base):
        meta = json.load(open(base + ".json", encoding="utf-8"))
        self.name = "symbols"
        self.glyphs = {(int(px), int(cp)): g for px, d in meta["sizes"].items() for cp, g in d.items()}
        self.img = Image.open(base + ".png").convert("L")

    def render(self, cp, px):
        g = self.glyphs.get((px, cp))
        if g is None:
            return None
        x, y, w, h, bx, by, adv = g
        if w == 0:
            return None, 0, 0, adv
        return self.img.crop((x, y, x + w, y + h)), bx, by, adv


def make_symbols(base, ttf_path, sizes, chars):
    """awpack side: pre-render chars from a TTF at every pixel size into a BitmapFace."""
    t = TtfFace(ttf_path)
    sk = Skyline(1024, 1024)
    sheet = Image.new("L", (1024, 1024), 0)
    meta = {"sizes": {}}
    for px in sorted(set(sizes)):
        d = meta["sizes"].setdefault(str(px), {})
        for c in chars:
            r = t.render(ord(c), px)
            if r is None:
                continue
            gim, bx, by, adv = r
            if gim is None:
                d[str(ord(c))] = [0, 0, 0, 0, 0, 0, adv]
                continue
            x, y = sk.place(gim.width + 1, gim.height + 1)
            sheet.paste(gim, (x, y))
            d[str(ord(c))] = [x, y, gim.width, gim.height, bx, by, adv]
    used_h = max(y for x, y, w in sk.sky)
    sheet.crop((0, 0, 1024, max(8, used_h))).save(base + ".png", optimize=True)
    json.dump(meta, open(base + ".json", "w"))


def load_kit(kit):
    """-> (fontsets [(face, px)], faces [SdfFace], english texts {name: lines}, psp {file: {line: text}},
           static originals [str])"""
    fontsets = [tuple(x) for x in json.load(open(os.path.join(kit, "fontsets.json")))]
    faces = []
    i = 0
    while os.path.exists(os.path.join(kit, "fonts", "face%d.json" % i)):
        faces.append(SdfFace(os.path.join(kit, "fonts", "face%d" % i)))
        i += 1
    sym = os.path.join(kit, "fonts", "symbols")
    if os.path.exists(sym + ".json"):
        faces.append(BitmapFace(sym))
    en = os.path.join(kit, "english")
    texts = {nm: read_lines(os.path.join(en, nm + ".json")) for nm in TEXT_FILES}
    psp = json.load(open(os.path.join(en, "psp.json"), encoding="utf-8"))
    static = list(json.load(open(os.path.join(kit, "static_strings.json"), encoding="utf-8")))
    return fontsets, faces, texts, psp, static


def apply_psp(texts, psp):
    out = {nm: list(lines) for nm, lines in texts.items()}
    for nm, entries in psp.items():
        if nm.startswith("_"):
            continue
        for k, v in entries.items():
            i = int(k)
            if nm in out and 0 <= i < len(out[nm]):
                out[nm][i] = v
    return out


# ---------------------------------------------------------------- build
def build(fontsets, faces, texts, static_map, static_originals, name, extra_fonts=()):
    """texts: {file: [lines]} for all TEXT_FILES (PSP wording already applied).
    static_map: {original scene string: translation}.  -> (bytes, {char: [font sets missing it]})"""
    chars = set(BASE_CHARS)
    for lines in texts.values():
        for l in lines:
            chars |= set(l)
    for s in static_originals:
        chars |= set(s)
    for s in static_map.values():
        chars |= set(s)
    cps = sorted(ord(c) for c in chars if ord(c) >= 32)
    ttfs = [TtfFace(p) for p in extra_fonts]
    sdf_fallback = 0     # Xolonium: the game's main font and Sturkopf's stand-in for missing letters

    fset_rows, glyph_rows, missing = [], [], {}
    pack, imgs = [Skyline(512, 512)], [Image.new("L", (512, 512), 0)]
    for si, (fi, px) in enumerate(fontsets):
        face = faces[fi]
        s = px / face.point
        first = len(glyph_rows)
        chain = [face] + ([faces[sdf_fallback]] if fi != sdf_fallback else []) + \
                [f for f in faces if isinstance(f, BitmapFace)] + ttfs
        for cp in cps:
            r = None
            for f in chain:
                r = f.render(cp, px)
                if r is not None:
                    break
            if r is None:
                missing.setdefault(chr(cp), []).append(si)
                continue
            gim, bx, by, adv = r
            if gim is None:
                glyph_rows.append((0, 0, 0, 0, 0, 0, 0, round(adv * 16), cp))
                continue
            pos = pack[-1].place(gim.width + 1, gim.height + 1)
            if pos is None:
                pack.append(Skyline(512, 512)); imgs.append(Image.new("L", (512, 512), 0))
                pos = pack[-1].place(gim.width + 1, gim.height + 1)
            imgs[-1].paste(gim, pos)
            glyph_rows.append((len(imgs) - 1, gim.width, gim.height, pos[0], pos[1],
                               round(bx * 16), round(by * 16), round(adv * 16), cp))
        fset_rows.append((first, len(glyph_rows) - first, round(face.line * s * 16), round(face.ascent * s * 16), fi, px))
    if len(imgs) > 255 or len(glyph_rows) > 65535:
        raise SystemExit("too many glyphs for one LANG.PAK (%d glyphs, %d pages)" % (len(glyph_rows), len(imgs)))

    out = bytearray(struct.calcsize(HDR))
    pad64(out); o_fsets = len(out)
    for r in fset_rows:
        out += struct.pack("<HHHHBB", *r)
    pad64(out); o_glyphs = len(out)
    for (page, w, h, u, v, bx, by, adv, cp) in glyph_rows:
        out += struct.pack("<BBBBHHhhH2xI", page, w, h, 0, u, v, bx, by, adv, cp)
    page_data = []
    for i, im in enumerate(imgs):
        used_h = pow2(max(y for x, y, w in pack[i].sky))
        page_data.append((512, used_h, swizzle(im.crop((0, 0, 512, used_h)).tobytes(), 512, used_h)))
    pad64(out); o_pages = len(out)
    out += bytes(8 * len(page_data))
    for i, (w, h, data) in enumerate(page_data):
        pad64(out)
        struct.pack_into("<HHI", out, o_pages + 8 * i, w, h, len(out))
        out += data
    pad64(out); o_clut = len(out)
    for i in range(256):
        out += struct.pack("<I", 0x00FFFFFF | (i << 24))
    pad64(out); o_text = len(out)
    out += bytes(12 * len(TEXT_FILES))
    for i, nm in enumerate(TEXT_FILES):
        blob = b"".join(l.encode("utf-8") + b"\0" for l in texts[nm])
        struct.pack_into("<III", out, o_text + 12 * i, len(out), len(blob), len(texts[nm]))
        out += blob
    pad64(out); o_static = len(out)
    st = sorted((zlib.crc32(k.encode("utf-8")) & 0xFFFFFFFF, v) for k, v in static_map.items() if v != k)
    out += bytes(8 * len(st))
    for i, (crc, v) in enumerate(st):
        struct.pack_into("<II", out, o_static + 8 * i, crc, len(out))
        out += v.encode("utf-8") + b"\0"
    pad64(out)
    struct.pack_into(HDR, out, 0, MAGIC, VERSION, len(fset_rows), len(page_data), len(glyph_rows),
                     o_fsets, o_glyphs, o_pages, o_clut, len(TEXT_FILES), o_text, len(st), o_static,
                     kit_id(fontsets), name.encode("utf-8")[:27])
    return bytes(out), missing


def export_face(base, name, d, atlas):
    """awpack side: write a TextMeshPro font asset (typetree dict + atlas image) to langkit/fonts."""
    face = d["m_FaceInfo"]
    gtab = {gl["m_Index"]: gl for gl in d["m_GlyphTable"]}
    glyphs = {}
    for ch in d["m_CharacterTable"]:
        gl = gtab.get(ch["m_GlyphIndex"])
        if gl is None:
            continue
        m, r = gl["m_Metrics"], gl["m_GlyphRect"]
        glyphs[str(ch["m_Unicode"])] = [r["m_X"], r["m_Y"], r["m_Width"], r["m_Height"], m["m_HorizontalBearingX"],
                                        m["m_HorizontalBearingY"], m["m_HorizontalAdvance"], gl.get("m_Scale", 1.0)]
    json.dump({"name": name, "pointSize": face["m_PointSize"], "lineHeight": face["m_LineHeight"],
               "ascent": face["m_AscentLine"], "padding": d.get("m_AtlasPadding", 9), "glyphs": glyphs},
              open(base + ".json", "w", encoding="utf-8"))
    atlas.getchannel("A").save(base + ".png", optimize=True)
