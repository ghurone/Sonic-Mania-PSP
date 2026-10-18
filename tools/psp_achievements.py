"""The PSP main menu's achievements button and page heading, in each language, in the menu's own lettering.

The PSP has no system overlay listing achievements, so the port's main menu has an "ACHIEVEMENTS" button and page
(SonicMania/Objects/Menu/MainMenu.c). Their words aren't among the menu's pictures, so they are put together here from
your own Data.rsdk: each letter is cut out of a label or heading that has it (white button lettering from UI/Text*,
yellow heading lettering from UI/Headings*), and the few letters no heading has are derived from their nearest ones
(V from U, as the game's own white V is its U drawn to a point; È from É, its accent mirrored). Japanese, Korean and
Chinese use the English words: their menu lettering has none of the characters.

The result is a sprite sheet and its animation, UI/PSPAchievements.gif/.bin: list 2*L is language L's button label (as
the main menu's labels: pivot at the left), list 2*L+1 its heading (as the headings: centred).

build_psp_data.py stores them in Data.psp. To add them to a Data.psp built before, without building it again:
    python tools/psp_achievements.py <Data.rsdk> <Data.psp>
(--preview <png> also writes a picture of the words.)
"""
import argparse
import io
import os
import struct
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rsdk_pack  # noqa: E402

SHEET = 'UI/PSPAchievements.gif'
ANIMATION = 'UI/PSPAchievements.bin'
LANGUAGES = ['EN', 'FR', 'IT', 'GE', 'SP', 'JP', 'KO', 'SC', 'TC', 'PT']  # the engine's language IDs (PT: the PSP port's)
WORDS = {'EN': 'ACHIEVEMENTS', 'FR': 'SUCCÈS', 'IT': 'OBIETTIVI', 'GE': 'ERFOLGE', 'SP': 'LOGROS', 'PT': 'TROFÉUS'}

# labels and headings whose letters are taken: (animation, list, frame, text)
LABEL_SOURCES = [
    ('UI/TextEN.bin', 1, 0, 'MANIA MODE'),
    ('UI/TextEN.bin', 1, 4, 'EXTRAS'),
    ('UI/TextEN.bin', 1, 2, 'COMPETITION'),
    ('UI/TextEN.bin', 1, 3, 'OPTIONS'),
    ('UI/TextEN.bin', 1, 1, 'TIME ATTACK'),
    ('UI/TextEN.bin', 3, 0, 'VIDEO'),
    ('UI/TextEN.bin', 3, 1, 'SOUND'),
    ('UI/TextEN.bin', 3, 3, 'LANGUAGE'),
    ('UI/TextEN.bin', 5, 2, 'SFX'),
    ('UI/TextEN.bin', 7, 5, 'BLUE SPHERES'),
    ('UI/TextEN.bin', 15, 3, 'DEUTSCH'),
    ('UI/TextFR.bin', 7, 5, 'SPHÈRES BLEUES'),
    ('UI/TextSP.bin', 7, 8, 'CRÉDITOS'),
]
HEADING_SOURCES = [
    ('UI/HeadingsEN.bin', 0, 0, 'MAIN MENU'),
    ('UI/HeadingsEN.bin', 2, 0, 'TIME ATTACK'),
    ('UI/HeadingsEN.bin', 3, 0, 'COMPETITION'),
    ('UI/HeadingsEN.bin', 5, 0, 'OPTIONS'),
    ('UI/HeadingsEN.bin', 6, 0, 'EXTRAS'),
    ('UI/HeadingsEN.bin', 7, 0, 'LEADERBOARDS'),
    ('UI/HeadingsEN.bin', 8, 0, 'GAME OPTIONS'),
    ('UI/HeadingsGE.bin', 10, 0, 'ZONE WÄHLEN'),
    ('UI/HeadingsGE.bin', 3, 0, 'WETTKAMPF'),
    ('UI/HeadingsFR.bin', 3, 0, 'COMPÉTITION'),
]


def read_animation(data):
    """(sheet names, [[(sheet, x, y, w, h, pivotX, pivotY), ...] per list])"""
    r = rsdk_pack.Reader(data)
    if r.u32() != 0x525053:
        raise ValueError('not an animation')
    r.u32()
    sheets = [r.string() for _ in range(r.u8())]
    hitboxes = [r.string() for _ in range(r.u8())]
    lists = []
    for _ in range(r.u16()):
        r.string()
        count = r.u16()
        r.pos += 4
        frames = []
        for _ in range(count):
            sheet = r.u8()
            r.pos += 4
            x, y, w, h, px, py = struct.unpack_from('<4H2h', data, r.pos)
            r.pos += 12 + 8 * len(hitboxes)
            frames.append((sheet, x, y, w, h, px, py))
        lists.append(frames)
    return sheets, lists


def write_animation(sheet, lists):
    """lists: [(name, [(x, y, w, h, pivotX, pivotY), ...]), ...] all on one sheet"""
    def string(text):
        raw = text.encode('latin1') + b'\0'
        return bytes([len(raw)]) + raw
    out = bytearray(b'SPR\0')
    out += struct.pack('<I', sum(len(frames) for _, frames in lists))
    out += bytes([1]) + string(sheet) + bytes([0])
    out += struct.pack('<H', len(lists))
    for name, frames in lists:
        out += string(name) + struct.pack('<HhBB', len(frames), 1, 0, 0)
        for x, y, w, h, px, py in frames:
            out += struct.pack('<BhH4H2h', 0, 1, 0, x, y, w, h, px, py)
    return bytes(out)


class Glyph:
    """A letter's pixels (palette indices, 0 = clear) and how far its top is above the line"""

    def __init__(self, pixels, top):
        self.pixels = pixels  # list of rows
        self.top = top

    @property
    def width(self):
        return len(self.pixels[0]) if self.pixels else 0


def cut_letters(image, frame, text, gap):
    """The letters of a label/heading frame, keyed by character (spaces skipped); None when they can't be told apart.
    gap: the letters are apart by clear columns (headings), else they touch outline to outline (labels)."""
    _, x, y, w, h, px, py = frame
    pix = [[image.getpixel((x + c, y + r)) for c in range(w)] for r in range(h)]
    letters = [ch for ch in text if ch != ' ']
    if gap:
        used = [any(pix[r][c] for r in range(h)) for c in range(w)]
    else:
        # a letter is its run of inner columns (anything but clear and outline), plus the outline column each side
        used = [any(pix[r][c] not in (0, 1) for r in range(h)) for c in range(w)]
    runs, c = [], 0
    while c < w:
        if used[c]:
            s = c
            while c < w and used[c]:
                c += 1
            runs.append((s, c) if gap else (max(0, s - 1), min(w, c + 1)))
        else:
            c += 1
    if gap and len(runs) < len(letters):
        # some heading letters touch: such a run is several letters of their usual widths side by side
        split, i = [], 0
        for a, b in runs:
            if i < len(letters) and abs((b - a) - HEADING_WIDTHS.get(letters[i], 16)) <= 2:
                split.append((a, b))
                i += 1
                continue
            widths, total = [], 0
            while i + len(widths) < len(letters) and total < b - a:
                widths.append(HEADING_WIDTHS.get(letters[i + len(widths)], 16))
                total += widths[-1]
            if total != b - a:
                return None
            for wd in widths:
                split.append((a, a + wd))
                a += wd
            i += len(widths)
        runs = split
    if len(runs) != len(letters):
        return None
    return {ch: Glyph([row[a:b] for row in pix], -py) for ch, (a, b) in zip(letters, runs)}


# the heading letters' widths, where they aren't the usual 16 (for telling touching ones apart)
HEADING_WIDTHS = {'I': 6, 'M': 24, 'W': 24, 'O': 24}


def gather(pack, sources, gap):
    glyphs, images, animations = {}, {}, {}
    for path, lst, frm, text in sources:
        if path not in animations:
            data = pack.read('Data/Sprites/' + path)
            animations[path] = read_animation(data) if data else None
        if not animations[path]:
            continue
        sheets, lists = animations[path]
        frame = lists[lst][frm]
        name = sheets[frame[0]]
        if name not in images:
            data = pack.read('Data/Sprites/' + name)
            images[name] = Image.open(io.BytesIO(data)) if data else None
        if not images[name]:
            continue
        cut = cut_letters(images[name], frame, text, gap)
        if cut is None:
            print('  (letters of %s %d.%d "%s" not told apart)' % (path, lst, frm, text))
            continue
        for ch, g in cut.items():
            glyphs.setdefault(ch, g)
    return glyphs


def v_from_u(u):
    """The game's V is its U drawn to a point (compare their white button letters). Towards the bottom the U's two
    straight arms (as they are above that) close in on the middle a column per row, as the white V's sides do, meeting
    in a point on the U's own bottom row; each row keeps the U's colour band at that height, and its outline."""
    w, h = u.width, len(u.pixels)
    mid = w // 2
    filled = [r for r in range(h) if any(u.pixels[r])]
    bottom = filled[-1]
    start = bottom - (mid - 2)
    outline = max(set(u.pixels[bottom]) - {0}, key=u.pixels[bottom].count)
    # the arms: the lowest row still like the U's middle one (open between them), before its bottom curves
    middle = u.pixels[h // 2]
    straight = max(r for r in range(h // 2, bottom) if [v != 0 for v in u.pixels[r]] == [v != 0 for v in middle])
    arms = u.pixels[straight]

    def band(r):
        inner = [v for v in u.pixels[r] if v not in (0, outline)]
        return max(set(inner), key=inner.count) if inner else None

    rows = [list(row) for row in u.pixels[:straight + 1]]
    last_band = band(straight)
    for r in range(straight + 1, start):  # the arms go on straight down to where they start closing in
        colour = band(r) or last_band
        last_band = colour
        rows.append([colour if v and v != outline else v for v in arms])
    for r in range(start, h):
        if r > bottom:
            rows.append([0] * w)
            continue
        k = r - start + 1  # (the bottom row: the arms one column either side of the middle)
        left = [arms[c - k] if k <= c < mid + k and c - k < mid else 0 for c in range(w)]
        right = [arms[c + k] if mid - k <= c < w - k and c + k >= mid else 0 for c in range(w)]
        colour = band(r) or last_band
        last_band = colour
        new = []
        for a, b in zip(left, right):
            if (a and a != outline) or (b and b != outline):
                new.append(colour)  # inside either arm: inside the V
            elif a or b:
                new.append(outline)
            else:
                new.append(0)
        if band(r) is None:  # the U's own bottom edge: only outline here
            new = [outline if v else 0 for v in new]
        cols = [c for c, v in enumerate(new) if v]
        if cols:
            new[cols[0]] = new[cols[-1]] = outline
        rows.append(new)
    return Glyph(rows, u.top)


def grave_from_acute(acute, plain):
    """È from É: the rows above the plain E's top (its accent) mirrored"""
    first = min(r for r, row in enumerate(plain.pixels) if any(row))
    accent_rows = acute.top - (plain.top - first)  # rows of the accented letter above the E's own top
    rows = [list(reversed(row)) if r < accent_rows else list(row) for r, row in enumerate(acute.pixels)]
    return Glyph(rows, acute.top)


def compose(glyphs, word, spacing):
    """A word's pixels and how far its top is above the line"""
    parts = [glyphs[ch] for ch in word if ch != ' ']
    top = max(g.top for g in parts)
    bottom = max(len(g.pixels) - g.top for g in parts)
    width = sum(g.width for g in parts) + spacing * (len(parts) - 1)
    rows = [[0] * width for _ in range(top + bottom)]
    x = 0
    for g in parts:
        y0 = top - g.top
        for r, row in enumerate(g.pixels):
            for c, v in enumerate(row):
                if v:
                    rows[y0 + r][x + c] = v
        x += g.width + spacing
    return rows, top


def build(pack):
    """{path in the pack: contents} for the sheet (a GIF) and its animation; None when the data lacks a source"""
    white = gather(pack, LABEL_SOURCES, gap=False)
    yellow = gather(pack, HEADING_SOURCES, gap=True)
    if 'U' in yellow:
        yellow.setdefault('V', v_from_u(yellow['U']))
    if 'É' in yellow and 'E' in yellow:
        yellow.setdefault('È', grave_from_acute(yellow['É'], yellow['E']))

    words = {}
    for lang in LANGUAGES:
        word = WORDS.get(lang, WORDS['EN'])
        missing = sorted({ch for ch in word if ch != ' ' and (ch not in white or ch not in yellow)})
        if missing:
            print('  %s: no lettering for %s, English instead' % (lang, ''.join(missing)))
            word = WORDS['EN']
        words[lang] = word
    unique = sorted(set(words.values()))

    # one picture per word and lettering, stacked on a 256-wide sheet
    pictures = {}
    for word in unique:
        pictures[(word, 'label')] = compose(white, word, 0)
        pictures[(word, 'heading')] = compose(yellow, word, 1)
    width = 256
    if max(len(rows[0]) for rows, _ in pictures.values()) > width:
        width = 512
    height = sum(len(rows) + 1 for rows, _ in pictures.values())
    sheet = Image.new('P', (width, (height + 15) & ~15), 0)
    palette_src = Image.open(io.BytesIO(pack.read('Data/Sprites/UI/TextEN.gif')))
    sheet.putpalette(palette_src.getpalette())
    rects, y = {}, 0
    for key, (rows, top) in pictures.items():
        for r, row in enumerate(rows):
            for c, v in enumerate(row):
                if v:
                    sheet.putpixel((c, y + r), v)
        w, h = len(rows[0]), len(rows)
        rects[key] = (0, y, w, h, 0 if key[1] == 'label' else -(w // 2), -top)
        y += h + 1

    lists = []
    for lang in LANGUAGES:
        lists.append(('Label ' + lang, [rects[(words[lang], 'label')]]))
        lists.append(('Heading ' + lang, [rects[(words[lang], 'heading')]]))
    gif = io.BytesIO()
    sheet.save(gif, 'GIF', optimize=False, transparency=0)
    return {'Data/Sprites/' + SHEET: gif.getvalue(), 'Data/Sprites/' + ANIMATION: write_animation(SHEET, lists)}, sheet


def update_pack(psp_path, files, convert):
    """Rewrites a Data.psp with files added (or replaced): the table grows, the rest moves down"""
    with open(psp_path, 'rb') as f:
        magic, version, count = struct.unpack('<4sII', f.read(12))
        if magic != b'RPSP':
            raise SystemExit('%s is not a Data.psp' % psp_path)
        table = [struct.unpack('<16sIIB3x', f.read(28)) for _ in range(count)]
    new_keys = {bytes.fromhex(rsdk_pack.name_hash(path)): convert(data) for path, data in files.items()}
    table = [t for t in table if t[0] not in new_keys]
    align = 64
    count = len(table) + len(new_keys)
    start = (12 + count * 28 + align - 1) & ~(align - 1)
    tmp = psp_path + '.tmp'
    entries = []
    with open(psp_path, 'rb') as src, open(tmp, 'wb') as out:
        out.write(b'\0' * start)
        for key, pos, size, kind in sorted(table, key=lambda t: t[1]):
            src.seek(pos)
            at = out.tell()
            remaining = size
            while remaining:
                chunk = src.read(min(remaining, 1 << 20))
                out.write(chunk)
                remaining -= len(chunk)
            out.write(b'\0' * (-out.tell() % align))
            entries.append((key, at, size, kind))
        for key, data in new_keys.items():
            at = out.tell()
            out.write(data)
            out.write(b'\0' * (-out.tell() % align))
            entries.append((key, at, len(data), 0))
        out.seek(0)
        out.write(b'RPSP' + struct.pack('<II', version, len(entries)))
        for key, at, size, kind in sorted(entries):
            out.write(key + struct.pack('<IIBxxx', at, size, kind))
    os.replace(tmp, psp_path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rsdk')
    ap.add_argument('psp', nargs='?')
    ap.add_argument('--preview')
    args = ap.parse_args()
    pack = rsdk_pack.Pack(args.rsdk)
    files, sheet = build(pack)
    if args.preview:
        sheet.convert('RGBA').resize((sheet.width * 3, sheet.height * 3), Image.NEAREST).save(args.preview)
    if args.psp:
        import build_psp_data
        update_pack(args.psp, files, lambda data: build_psp_data.gif_to_sheet(data) or data if data[:3] == b'GIF' else data)
        print('added %s and %s to %s' % (SHEET, ANIMATION, args.psp))


if __name__ == '__main__':
    main()
