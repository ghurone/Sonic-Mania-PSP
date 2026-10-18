"""Decrypts Data.rsdk files whose names are unknown.

The key of an encrypted file has two halves: one from its size (known) and 16 bytes (keyA) from its name. Those 16
bytes are reused all through the file, so they can be recovered from the content instead:
  - a first guess per key byte from frequency (data files are mostly zero bytes);
  - bytes a format fixes (signatures, GIF block lengths, a WAV/PNG header) set key bytes outright;
  - then a strict parser for the format reads the file until the first value that can't be right, and the key byte
    behind that spot is changed to whichever value lets the parser get furthest; repeated until the whole file parses.
A result is only kept when the parser accepts the whole file to its last byte and the format's own checks pass (LZW
stream of the exact pixel count for GIFs, zlib checksums for scenes and tile configs, PNG CRCs...).
"""
import io
import os
import re
import struct
import zlib

import numpy as np

import rsdk_pack


def schedule(size):
    """Per byte: the size-derived key byte (keyNo ^ keyB[posB]), which keyA byte applies, and whether nybbles swap."""
    key_b = rsdk_pack._key_bytes(str(size))
    key_no, pos_a, pos_b, swap = (size // 4) & 0x7F, 0, 8, 0
    k1 = np.empty(size, np.uint8)
    pa = np.empty(size, np.uint8)
    sw = np.empty(size, bool)
    for i in range(size):
        k1[i], pa[i], sw[i] = key_no ^ key_b[pos_b], pos_a, swap
        pos_a += 1
        pos_b += 1
        if pos_a <= 15:
            if pos_b > 12:
                pos_b = 0
                swap ^= 1
        elif pos_b <= 8:
            pos_a = 0
            swap ^= 1
        else:
            key_no = (key_no + 2) & 0x7F
            if swap:
                swap = 0
                pos_a = key_no % 7
                pos_b = (key_no % 12) + 2
            else:
                swap = 1
                pos_a = (key_no % 12) + 3
                pos_b = key_no % 7
    return k1, pa, sw


# ---------------------------------------------------------------------------------------------------------------------
# strict parsers: parse(plain) -> (ok, progress), progress being where the first impossible value starts


class Bad(Exception):
    def __init__(self, pos, over=0):
        super().__init__(pos)
        self.pos = pos
        self.over = over  # for reads past the end: how far past


class R:
    def __init__(self, data):
        self.d = data
        self.i = 0

    def take(self, n):
        if self.i + n > len(self.d):
            raise Bad(len(self.d), self.i + n - len(self.d))
        self.i += n
        return self.i - n

    def u8(self, lo=0, hi=0xFF):
        p = self.take(1)
        v = self.d[p]
        if not lo <= v <= hi:
            raise Bad(p)
        return v

    def u16(self, lo=0, hi=0xFFFF):
        p = self.take(2)
        v = self.d[p] | self.d[p + 1] << 8
        if not lo <= v <= hi:
            raise Bad(p + 1 if v > hi and self.d[p + 1] else p)
        return v

    def i16(self, lim):
        p = self.take(2)
        v = struct.unpack_from('<h', self.d, p)[0]
        if not -lim <= v <= lim:
            raise Bad(p + 1)
        return v

    def u32(self, hi=0xFFFFFFFF, lo=0):
        p = self.take(4)
        v = struct.unpack_from('<I', self.d, p)[0]
        if v < lo:
            raise Bad(p)
        if v > hi:
            for b in (3, 2, 1, 0):
                if (v >> (8 * b)) & 0xFF and (hi >> (8 * b)) == 0:
                    raise Bad(p + b)
            raise Bad(p)
        return v

    def f32(self, lim):
        p = self.take(4)
        v = struct.unpack_from('<f', self.d, p)[0]
        if not -lim <= v <= lim:  # also false for NaN
            raise Bad(p + 3)
        return v

    def expect(self, data):
        p = self.take(len(data))
        for j, b in enumerate(data):
            if self.d[p + j] != b:
                raise Bad(p + j)

    def string(self, suffix=b''):
        """RSDK string: length byte, then that many printable bytes (some tools count a closing NUL in)."""
        n = self.u8(len(suffix) + 1, 0x7F)
        p = self.take(n)
        if not self.d[p + n - 1]:
            n -= 1
        for j in range(n):
            if not 0x20 <= self.d[p + j] < 0x7F:
                raise Bad(p + j)
        if suffix and self.d[p + n - len(suffix):p + n].lower() != suffix:
            raise Bad(p + n - len(suffix))
        return self.d[p:p + n].decode()

    def compressed(self):
        """RSDK compressed block (u32 size + 4, u32 big-endian unpacked size, zlib); the unpacked data."""
        start = self.i
        csize = self.u32(len(self.d) - start) - 4
        size = struct.unpack('>I', bytes(self.d[self.take(4):self.i]))[0]
        p = self.take(csize)
        try:
            out = zlib.decompress(bytes(self.d[p:p + csize]))
        except zlib.error:
            raise Bad(p) from None
        if len(out) != size:
            raise Bad(start + 4)
        return out

    def end(self):
        if self.i != len(self.d):
            raise Bad(self.i)


def _run(parse, plain):
    """(ok, score): score orders failures by how far parsing got, then by how little it overran the end."""
    r = R(plain)
    try:
        parse(r)
        r.end()
        return True, (len(plain), 0)
    except Bad as e:
        return False, (e.pos, -e.over)
    except (IndexError, struct.error):
        return False, (r.i, 0)


def parse_spr(r):
    r.expect(b'SPR\0')
    frames = r.u32(0x8000)
    sheets = r.u8(1, 24)
    for _ in range(sheets):
        r.string(b'.gif')
    hitboxes = r.u8(0, 8)
    for _ in range(hitboxes):
        r.string()
    total = 0
    for _ in range(r.u16(1, 0x400)):
        r.string()
        count = r.u16(0, frames - total)
        total += count
        r.u16(0, 0x400)  # speed
        r.u8()  # loop index
        r.u8(0, 7)  # rotation style
        for _ in range(count):
            r.u8(0, sheets - 1)
            r.i16(0x4000)  # duration
            r.u16()  # unicode char
            r.u16(0, 0x1000)
            r.u16(0, 0x1000)
            r.u16(0, 0x400)
            r.u16(0, 0x400)
            r.i16(0x400)
            r.i16(0x400)
            for _ in range(hitboxes * 4):
                r.i16(0x400)
    if total != frames:
        raise Bad(4)


def parse_gif(r):
    r.expect(b'GIF89a')
    width = r.u16(1, 0x4000)
    height = r.u16(1, 0x4000)
    r.expect(b'\xf7\0\0')
    r.take(768)
    while r.d[r.i] == 0x21:  # extensions
        r.take(2)
        while r.u8():
            r.take(r.d[r.i - 1])
    r.expect(b'\x2c')
    x, y = r.u16(), r.u16()
    w, h = r.u16(1, width - x), r.u16(1, height - y)
    r.u8(0, 0x7F)  # no local palette
    r.u8(2, 8)
    _lzw(r, r.d[r.i - 1], w * h)
    r.expect(b'\x3b')


def _lzw(r, min_size, pixels):
    """Walks a GIF's LZW stream (sub-blocks) and insists on valid codes, exactly `pixels` pixels and an end code."""
    clear, eoi = 1 << min_size, (1 << min_size) + 1
    lengths = [1] * (1 << min_size) + [0, 0]
    size, nxt, prev, done = min_size + 1, eoi + 1, -1, 0
    bits = nbits = 0
    ended = False
    while True:
        n = r.u8()
        if not n:
            break
        start = r.take(n)
        if ended:
            raise Bad(start - 1)
        for j in range(start, start + n):
            bits |= r.d[j] << nbits
            nbits += 8
            while nbits >= size and not ended:
                code = bits & ((1 << size) - 1)
                bits >>= size
                nbits -= size
                if code == clear:
                    size, nxt, prev = min_size + 1, eoi + 1, -1
                    del lengths[eoi + 1:]
                    continue
                if code == eoi:
                    if done != pixels:
                        raise Bad(j)
                    ended = True
                    break
                if code > nxt or (code == nxt and prev < 0):
                    raise Bad(j)
                length = lengths[code] if code < nxt else lengths[prev] + 1
                done += length
                if done > pixels:
                    raise Bad(j)
                if prev >= 0 and nxt < 4096:
                    lengths.append(lengths[prev] + 1)
                    nxt += 1
                    if nxt == 1 << size and size < 12:
                        size += 1
                prev = code
    if not ended:
        raise Bad(r.i - 1)


def parse_mdl(r):
    r.expect(b'MDL\0')
    flags = r.u8(0, 7)
    face = r.u8(3, 4)
    verts = r.u16(1, 0x2000)
    frames = r.u16(1, 0x400)
    if flags & 2:
        for _ in range(verts * 2):
            r.f32(64)
    if flags & 4:
        for _ in range(verts):
            r.u32()
    count = r.u16(face, 0xFFFF)
    if count % face:
        raise Bad(r.i - 2)
    for _ in range(count):
        r.u16(0, verts - 1)
    for _ in range(frames * verts):
        for _ in range(3):
            r.f32(1e5)
        if flags & 1:  # normals: unit length (or none at all)
            start = r.i
            length = sum(r.f32(1.01) ** 2 for _ in range(3))
            if abs(length - 1) > 0.02 and length > 1e-6:
                raise Bad(start)


VAR_TYPES = ('int', 'bool', 'uint8', 'uint16', 'uint32', 'int8', 'int16', 'int32', 'float')


def parse_vars(r):
    """Global variable list: a count, then name/type/default for each; a count, then name/value constants."""
    for _ in range(r.u8(1)):
        r.string()
        start = r.i
        if r.string() not in VAR_TYPES:
            raise Bad(start)
        start = r.i
        if not all(ch in '-0123456789' for ch in r.string()):
            raise Bad(start)
    for _ in range(r.u8()):
        start = r.i
        if not all(ch in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789' for ch in r.string()):
            raise Bad(start)
        start = r.i
        if not all(ch in '-0123456789' for ch in r.string()):
            raise Bad(start)


def parse_obj(r):
    r.expect(b'OBJ\0')
    sizes = [1, 2, 4, 1, 2, 4, 4]
    while r.i < len(r.d):
        kind = r.u8()
        if (kind & 0x7F) > 12:
            raise Bad(r.i - 1)
        array = r.u32(0x10000)
        if kind & 0x80:
            if kind & 0x7F > 6:
                raise Bad(r.i - 5)
            count = r.u32(array)
            r.take(count * sizes[kind & 0x7F])


def parse_til(r):
    r.expect(b'TIL\0')
    r.compressed()


def parse_scn(r):
    r.expect(b'SCN\0')
    r.take(0x10)
    r.take(r.u8() + 1)
    for _ in range(r.u8(0, 8)):
        r.u8(0, 1)
        r.string()
        r.u8(0, 3)  # type
        r.u8(0, 16)  # draw group (16: not drawn)
        r.u16(0, 0x2000)
        r.u16(0, 0x2000)
        r.take(4)
        for _ in range(r.u16(0, 0x400)):
            r.take(4)
            r.u8(0, 1)
            r.u8()
        r.compressed()
        r.compressed()
    sizes = [1, 2, 4, 1, 2, 4, 4, 4, None, 8, 4, 4]
    for _ in range(r.u8()):
        r.take(16)
        count = r.u8(1)
        types = []
        for _ in range(count - 1):
            r.take(16)
            types.append(r.u8(0, 11))
        for _ in range(r.u16()):
            r.u16()  # slot
            r.take(8)
            for t in types:
                if sizes[t] is None:
                    r.take(r.u16(0, 0x400) * 2)
                else:
                    r.take(sizes[t])


def parse_cfg(r):
    r.expect(b'CFG\0')
    r.u8(0, 1)
    for _ in range(r.u8()):
        r.string()
    for _ in range(8):
        rows = r.u16()
        for b in range(16):
            if rows >> b & 1:
                r.take(16 * 3)
    for _ in range(r.u8()):
        r.string(b'.wav')
        r.u8(1, 0x20)


def parse_wav(r):
    r.expect(b'RIFF')
    if struct.unpack_from('<I', r.d, 4)[0] != len(r.d) - 8:
        raise Bad(4)
    r.take(4)
    r.expect(b'WAVE')
    chunks = []
    while r.i < len(r.d):
        start = r.i
        r.take(4)
        for j in range(4):
            if not 0x20 <= r.d[start + j] < 0x7F:
                raise Bad(start + j)
        n = r.u32(len(r.d) - r.i - 4)
        chunks.append(bytes(r.d[start:start + 4]))
        if chunks[0] != b'fmt ':
            raise Bad(start)
        if chunks[-1] == b'fmt ':
            fmt = r.u16(1, 3)
            channels = r.u16(1, 2)
            rate = r.u32(96000, 8000)
            byte_rate = r.u32()
            align = r.u16(1, 8)
            bits = r.u16(8, 32)
            if bits % 8 or align != channels * bits // 8 or byte_rate != rate * align:
                raise Bad(start + 8)
            r.take(n - 16)
        else:
            r.take(n + (n & 1 if r.i + n < len(r.d) else 0))
    if b'data' not in chunks:
        raise Bad(len(r.d) - 1)


def parse_png(r):
    from PIL import Image
    try:
        Image.open(io.BytesIO(bytes(r.d))).load()
    except Exception:
        raise Bad(0) from None
    r.take(len(r.d))


def parse_act(r):
    """Palette: 256 RGB colours (sometimes 4 more bytes). The colours are near enough all multiples of 8."""
    if len(r.d) not in (768, 772):
        raise Bad(0)
    odd = [j for j in range(768) if r.d[j] & 7 and r.d[j] not in (1, 0xFF)]
    if len(odd) > 768 // 20:
        raise Bad(odd[0])
    r.take(len(r.d))


FORMATS = {
    'gif': (b'GIF89a', parse_gif),
    'png': (b'\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR', parse_png),
    'wav': (b'RIFF', parse_wav),
    'anim': (b'SPR\0', parse_spr),
    'scn': (b'SCN\0', parse_scn),
    'mdl': (b'MDL\0', parse_mdl),
    'obj': (b'OBJ\0', parse_obj),
    'til': (b'TIL\0', parse_til),
    'cfg': (b'CFG\0', parse_cfg),
    'act': (((b'\xff\0\xff' * 6)[:16], bytes(16)), parse_act),  # palettes mostly open with magenta (unused) or black
    'vars': (b'', parse_vars),  # no signature: found from words it is sure to contain
}


def check(plain, kind=None):
    """What a decrypted file is, if it parses completely as that; None if it doesn't."""
    for name, (_, parse) in FORMATS.items():
        if (kind is None or kind == name) and _run(parse, plain)[0]:
            return name
    return None


# ---------------------------------------------------------------------------------------------------------------------


def _fixed_bytes(kind, sig, n):
    """(position, plaintext byte) pairs a format implies for a file of n bytes."""
    pairs = list(enumerate(sig))
    if kind == 'wav':
        pairs += list(zip(range(4, 16), struct.pack('<I', n - 8) + b'WAVEfmt '))
    if kind == 'gif' and n > 800:
        pairs += [(10, 0xF7), (11, 0), (12, 0), (781, 0x2C), (782, 0), (783, 0), (784, 0), (785, 0), (790, 0),
                  (791, 8), (n - 1, 0x3B), (n - 2, 0)]
        pairs += [(p, 0xFF) for p in range(792, n - 260, 256)]
    return pairs


def recover(data, kinds=None, sheets=()):
    """(plain bytes, kind) for an encrypted file of unknown name, or (None, None).

    sheets: known sprite sheet paths (as animations name them), tried as the first sheet of an animation - small
    animations have too few bytes per key byte for statistics, but a whole known string fixes most of the key."""
    c = np.frombuffer(data, np.uint8)
    n = len(c)
    k1, pa, sw = schedule(n)
    v = c ^ k1
    v = np.where(sw, ((v << 4) | (v >> 4)).astype(np.uint8), v)

    base = np.zeros(16, np.uint8)
    for a in set(pa.tolist()):
        base[a] = np.bincount(v[pa == a], minlength=256).argmax()

    # compiled Direct3D shaders: PC only, the PSP never loads them - recognised, not worth recovering
    guess = (v[:12] ^ base[pa[:12]]).tobytes()
    if not kinds and (guess[:4] == b'DXBC' or guess[4:6] == b'\xfe\xff' and guess[8:12] == b'CTAB'):
        return None, 'shader'

    for kind in kinds or FORMATS:
        sigs, parse = FORMATS[kind]
        for sig in sigs if isinstance(sigs, tuple) else (sigs,):
            if n < len(sig):
                continue
            if _consistent(v, pa, list(enumerate(sig))) is None:
                continue  # the signature contradicts itself: not this format
            for fixed, guesses in _starts(kind, sig, v, pa, sheets):
                key = base.copy()
                for a, value in list(guesses.items()) + list(fixed.items()):
                    key[a] = value
                result = _climb(v, pa, key, parse, set(fixed))
                if result is not None:
                    return result, kind
    return None, None


def _starts(kind, sig, v, pa, sheets):
    """Starting points for _climb: (key bytes held fixed, key bytes only guessed), most likely first."""
    n = len(pa)
    fixed = _consistent(v, pa, list(enumerate(sig)))
    # bytes the format implies, by majority where they disagree
    votes = {}
    for p, b in _fixed_bytes(kind, sig, n):
        votes.setdefault(int(pa[p]), []).append(int(v[p]) ^ b)
    yield fixed, {a: int(np.bincount(vals).argmax()) for a, vals in votes.items()}

    if kind == 'gif' and n > 781:
        # sheets use 8-bit-step colours (multiples of 8, or 0xFF/0x01): the palette alone points at each key byte
        yield fixed, _palette_key(v[13:781], pa[13:781])
    if kind == 'anim':
        for crib in _sheet_cribs(sheets):
            more = _consistent(v, pa, list(enumerate(sig)) + [(8 + j, b) for j, b in enumerate(crib)])
            if more is not None:
                yield more, {}
    if kind == 'mdl':
        for header in _model_headers(v, pa):
            yield header, {}
    if kind == 'vars':
        yield {}, _crib_key(v, pa, VARS_CRIBS)


def _palette_key(v, pa):
    plausible = np.zeros(256, np.int32)
    plausible[::8] = 1
    plausible[[1, 0xFF]] = 1
    key = {}
    for a in set(pa.tolist()):
        sel = v[pa == a]
        key[a] = int(np.argmax([plausible[sel ^ k].sum() for k in range(256)]))
    return key


def _model_headers(v, pa):
    """Key bytes for each model layout that fits the file size exactly, given that face indices (< vertex count) have
    a zero high byte when there are fewer than 256 vertices."""
    n = len(pa)
    for flags in range(8):
        for face in (3, 4):
            for verts in range(1, n // 12 + 1):
                extra = (verts * 8 if flags & 2 else 0) + (verts * 4 if flags & 4 else 0)
                for frames in range(1, 0x400):
                    rest = n - 12 - extra - frames * verts * (24 if flags & 1 else 12)
                    if rest < 0:
                        break
                    count = rest // 2
                    if rest % 2 or count < face or count % face:
                        continue
                    at = 10 + extra
                    pairs = list(enumerate(b'MDL\0')) + list(zip(range(4, 10), struct.pack('<BBHH', flags, face, verts, frames)))
                    pairs += list(zip((at, at + 1), struct.pack('<H', count)))
                    if verts < 256:
                        pairs += [(at + 3 + 2 * i, 0) for i in range(count)]
                    fixed = _consistent(v, pa, pairs)
                    if fixed is not None:
                        yield fixed


VARS_CRIBS = [b'\x03int\x01', b'restart', b'Secret', b'replay', b'Buffer[262144]', b'MEDAL_', b'ID_', b'Mode\x03int',
              b'[16384]', b'Seconds', b'Loaded\x03int', b'Enable', b'Slot', b'Rings', b'FILTER_', b'Score', b'Flags']


def _crib_key(v, pa, cribs):
    """Key bytes that the most placements of known words agree on."""
    votes = np.zeros((16, 256), np.int32)
    n = len(pa)
    for crib in cribs:
        for p in range(n - len(crib)):
            implied = _consistent(v[p:p + len(crib)], pa[p:p + len(crib)], list(enumerate(crib)))
            if implied:
                for a, value in implied.items():
                    votes[a, value] += 1
    return {a: int(votes[a].argmax()) for a in range(16) if votes[a].any()}


def _sheet_cribs(sheets):
    """Bytes an animation would hold from offset 8 (sheet count, then the first sheet's name) for each known sheet."""
    for name in sheets:
        for text in (name.encode() + b'\0', name.encode()):
            for count in (1, 2, 3, 4):
                yield bytes([count, len(text)]) + text


def _consistent(v, pa, pairs):
    """The key bytes that (position, plaintext byte) pairs imply, or None if they contradict each other."""
    fixed = {}
    for p, b in pairs:
        if p >= len(pa):
            return None
        a, value = int(pa[p]), int(v[p]) ^ b
        if fixed.setdefault(a, value) != value:
            return None
    return fixed


def _climb(v, pa, key, parse, pinned):
    """Changes one key byte at a time, each time the one that lets the parser get furthest, until the file parses."""
    n = len(pa)
    free = [a for a in sorted(set(pa.tolist())) if a not in pinned]
    tried = set()
    for _ in range(96):
        plain = (v ^ key[pa]).tobytes()
        ok, score = _run(parse, plain)
        if ok:
            return plain
        if bytes(key) in tried:
            return None
        tried.add(bytes(key))
        # the byte the parser stopped at is wrong, or one shortly before it (a length or count): the key bytes behind
        # the last few bytes are tried first, closest first; the rest only if none of those helps
        near = []
        for p in range(min(score[0], n - 1), max(score[0] - 24, -1), -1):
            if pa[p] in free and pa[p] not in near:
                near.append(int(pa[p]))
        order = near + [a for a in free if a not in near]
        best = (score, None, None)
        for count, a in enumerate(order):
            original = key[a]
            for value in range(256):
                if value == original:
                    continue
                key[a] = value
                ok2, score2 = _run(parse, (v ^ key[pa]).tobytes())
                if ok2:
                    key[a] = original
                    best = ((n + 1, 0), a, value)
                    break
                if score2 > best[0]:
                    best = (score2, a, value)
            key[a] = original
            if best[0][0] > n or (best[1] is not None and count + 1 >= min(2, len(near))):
                break
        if best[1] is None:
            return None
        key[best[1]] = best[2]
    return None


def sheet_candidates(pack, names, source_dirs):
    """Sheet paths animations might name: the known sheets, and common sheet names in every folder-like word."""
    sheets = {p[len('Data/Sprites/'):] for p in names.values() if p.lower().startswith('data/sprites/') and p.lower().endswith('.gif')}
    folders = {s.split('/')[0] for s in sheets if '/' in s}
    for key, path in names.items():
        if path.lower().startswith('data/sprites/') and path.lower().endswith('.bin'):
            sheets.update(rsdk_pack.parse_animation(pack.read(path) or b''))
    for source in source_dirs:
        for root, _, files in os.walk(source):
            folders.add(os.path.basename(root))
            for f in files:
                if f.endswith('.c'):
                    folders.update(re.findall(r'\b[A-Z][A-Za-z0-9]{1,15}\b', open(os.path.join(root, f), encoding='latin1').read()))
    for folder in folders:
        for base in ('Objects.gif', 'Objects2.gif', 'Objects3.gif', 'Objects-Classic.gif'):
            sheets.add('%s/%s' % (folder, base))
    # known sheets first: they are the likely ones
    known = sorted(s for s in sheets if s.split('/')[0] in folders and pack.has('Data/Sprites/' + s))
    return known + sorted(sheets - set(known))


def recover_unnamed(pack, names, source_dirs, log=print):
    """{pack key: (kind, plain bytes)} for the encrypted files no name was found for."""
    todo = [k for k, (_, _, enc) in pack.entries.items() if enc and k not in names]
    sheets = sheet_candidates(pack, names, source_dirs)
    found = {}
    for k in todo:
        plain, kind = recover(pack.raw(k), sheets=sheets)
        if plain is not None:
            found[k] = (kind, plain)
        elif kind != 'shader':
            log('  could not recover %s (%d bytes)' % (k, pack.entries[k][1]))
    return found
