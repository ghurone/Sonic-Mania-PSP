"""Reads an RSDKv5 Data.rsdk: the file table, file contents (decrypted the way the engine does) and the names of the
files, which the pack only stores as MD5 hashes - they are recovered from the game's source code and from the data
files that name other files (game/stage configs list stages and sound effects, animations list their sprite sheets).

Used by the PSP data tools; nothing here contains game data.
"""
import hashlib
import os
import re
import struct


def name_hash(path):
    """The pack's 16-byte key for a file path: MD5 of the lower-cased path, each 32-bit word byte-swapped."""
    digest = hashlib.md5(path.lower().encode('latin1')).digest()
    return b''.join(digest[i:i + 4][::-1] for i in range(0, 16, 4)).hex().upper()


def _key_bytes(text):
    digest = hashlib.md5(text.encode('latin1')).digest()
    return b''.join(digest[i:i + 4][::-1] for i in range(0, 16, 4))


def decrypt(data, path):
    """RSDK::DecryptBytes over a whole file, with the keys GenerateELoadKeys derives from its path and size."""
    size = len(data)
    key_a = _key_bytes(path.upper())
    key_b = _key_bytes(str(size))
    key_no, pos_a, pos_b, swap = (size // 4) & 0x7F, 0, 8, 0
    out = bytearray(size)
    for i in range(size):
        value = data[i] ^ key_no ^ key_b[pos_b]
        if swap:
            value = ((value << 4) | (value >> 4)) & 0xFF
        out[i] = value ^ key_a[pos_a]
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
    return bytes(out)


def decrypt_fast(data, path):
    """decrypt(), vectorised: the key schedule depends only on the starting keyNo and, after a short lead-in, repeats
    every ~2000 bytes, so one cycle of (swap, xor) is worked out and applied to the whole file at once."""
    import numpy as np
    size = len(data)
    key_a = _key_bytes(path.upper())
    key_b = _key_bytes(str(size))
    key_no, pos_a, pos_b, swap = (size // 4) & 0x7F, 0, 8, 0
    seen, xors, swaps = {}, [], []
    while (key_no, pos_a, pos_b, swap) not in seen and len(xors) < size:
        seen[(key_no, pos_a, pos_b, swap)] = len(xors)
        k1 = key_no ^ key_b[pos_b]
        swaps.append(swap)
        xors.append(((((k1 << 4) | (k1 >> 4)) & 0xFF) if swap else k1) ^ key_a[pos_a])
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
    pre = seen.get((key_no, pos_a, pos_b, swap), len(xors))
    period = len(xors) - pre
    index = np.arange(size, dtype=np.int64)
    if period > 0:
        tail = index >= pre
        index[tail] = pre + (index[tail] - pre) % period
    d = np.frombuffer(data, dtype=np.uint8)
    xors_a = np.array(xors, dtype=np.uint8)[index]
    swaps_a = np.array(swaps, dtype=bool)[index]
    nib = ((d << 4) | (d >> 4)).astype(np.uint8)
    return np.where(swaps_a, nib, d).astype(np.uint8).__xor__(xors_a).tobytes()


class Pack:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            if f.read(4) != b'RSDK':
                raise ValueError('not an RSDK data pack: ' + path)
            f.read(2)
            (count,) = struct.unpack('<H', f.read(2))
            self.entries = {}
            for _ in range(count):
                key = f.read(16).hex().upper()
                offset, size = struct.unpack('<II', f.read(8))
                self.entries[key] = (offset, size & 0x7FFFFFFF, bool(size & 0x80000000))
        self.names = {}  # key -> path, for the files identified so far

    def has(self, path):
        return name_hash(path) in self.entries

    def raw(self, key):
        offset, size, _ = self.entries[key]
        with open(self.path, 'rb') as f:
            f.seek(offset)
            return f.read(size)

    def read(self, path):
        """The file's contents, decrypted; None when the pack has no such file."""
        key = name_hash(path)
        if key not in self.entries:
            return None
        data = self.raw(key)
        return decrypt_fast(data, path) if self.entries[key][2] else data

    def name(self, path):
        """Records path if the pack has it; returns whether it does."""
        key = name_hash(path)
        if key in self.entries:
            self.names[key] = path
            return True
        return False


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def u8(self):
        self.pos += 1
        return self.data[self.pos - 1]

    def u16(self):
        self.pos += 2
        return struct.unpack_from('<H', self.data, self.pos - 2)[0]

    def u32(self):
        self.pos += 4
        return struct.unpack_from('<I', self.data, self.pos - 4)[0]

    def string(self):
        size = self.u8()
        self.pos += size
        return self.data[self.pos - size:self.pos].decode('latin1').rstrip(chr(0))

    def skip_palettes(self):
        for _ in range(8):
            rows = self.u16()
            self.pos += 3 * 16 * bin(rows).count('1')


def static_path(object_name):
    """Where an object class's static variables live (Object.cpp): its name's MD5 as hex, low nybble of each byte first."""
    digest = hashlib.md5(object_name.encode('latin1')).digest()
    return 'Data/Objects/Static/%s.bin' % ''.join('%X%X' % (b & 0xF, b >> 4) for b in digest)


def parse_game_config(data, objects=None):
    """(scene list as (folder, id), global sound effects); object class names go into objects."""
    r = Reader(data)
    if r.u32() != 0x474643:  # 'CFG'
        return [], []
    r.string(), r.string(), r.string()
    r.u8(), r.u16()
    for _ in range(r.u8()):
        name = r.string()
        if objects is not None:
            objects.add(name)
    r.skip_palettes()
    sfx = []
    for _ in range(r.u8()):
        sfx.append(r.string())
        r.u8()
    r.u16()
    scenes = []
    for _ in range(r.u8()):
        r.string()
        for _ in range(r.u8()):
            r.string()
            folder, scene_id = r.string(), r.string()
            r.u8()  # filter
            scenes.append((folder, scene_id))
    return scenes, sfx


def parse_stage_config(data, objects=None):
    """Stage sound effects; object class names go into objects."""
    r = Reader(data)
    if r.u32() != 0x474643:
        return []
    r.u8()
    for _ in range(r.u8()):
        name = r.string()
        if objects is not None:
            objects.add(name)
    r.skip_palettes()
    sfx = []
    for _ in range(r.u8()):
        sfx.append(r.string())
        r.u8()
    return sfx


def parse_animation(data):
    """Sprite sheets an animation (.bin) uses."""
    r = Reader(data)
    if r.u32() != 0x525053:  # 'SPR'
        return []
    r.u32()
    return [r.string() for _ in range(r.u8())]


SOURCE_PREFIXES = ['Data/Sprites/', 'Data/SoundFX/', 'Data/Music/', 'Data/Stages/', 'Data/Game/', 'Data/Images/', 'Data/Strings/',
                   'Data/Video/', 'Data/Palettes/', 'Data/Meshes/', 'Data/', '']


def discover_names(pack, source_dirs):
    """Identifies as many pack files as possible; returns pack.names."""
    literals, objects = set(), set()
    shaders, shader_paths = set(), set()
    for source in source_dirs:
        for root, _, files in os.walk(source):
            for f in files:
                if f.endswith(('.c', '.h', '.cpp', '.hpp')):
                    text = open(os.path.join(root, f), encoding='latin1').read()
                    literals.update(re.findall(r'"([A-Za-z0-9_ ./-]+\.(?:bin|gif|wav|ogg|ogv|txt|tsv|ini|csv|png|tmd|act))"', text))
                    objects.update(re.findall(r'^Object(\w+) \*;', text, re.M))
                    # the PC renderers' compiled shaders: "Data/Shaders/<api>/%s.<ext>" for each LoadShader("name")
                    shaders.update(re.findall(r'LoadShader\("([^"]+)"', text))
                    shader_paths.update(re.findall(r'"(Data/Shaders/[^"%]+/%s\.\w+)"', text))
    for lit in literals:
        for prefix in SOURCE_PREFIXES:
            pack.name(prefix + lit)
    for path in shader_paths:
        for shader in shaders:
            pack.name(path % shader)

    scenes, sfx = [], []
    config = pack.read('Data/Game/GameConfig.bin')
    if config:
        pack.name('Data/Game/GameConfig.bin')
        scenes, sfx = parse_game_config(config, objects)
    for folder in sorted({f for f, _ in scenes}):
        for name in ('StageConfig.bin', 'TileConfig.bin', '16x16Tiles.gif'):
            pack.name('Data/Stages/%s/%s' % (folder, name))
        stage = pack.read('Data/Stages/%s/StageConfig.bin' % folder)
        if stage:
            sfx += parse_stage_config(stage, objects)
    for folder, scene_id in scenes:
        pack.name('Data/Stages/%s/Scene%s.bin' % (folder, scene_id))
    for s in sfx:
        pack.name('Data/SoundFX/' + s)
    for o in objects:
        pack.name(static_path(o))

    # animations name their sheets; scenes and other data name more files (music tracks, extra sheets...) as plain
    # strings: keep scanning everything identified until nothing new turns up
    pattern = re.compile(rb'[A-Za-z0-9_ /.-]{3,}\.(?:bin|gif|wav|ogg|ogv|txt|png|tga|tmd|act)', re.I)
    scanned = set()
    while True:
        count = len(pack.names)
        for key, path in list(pack.names.items()):
            if key in scanned or path.lower().endswith(('.wav', '.ogg', '.gif', '.png')):
                continue
            scanned.add(key)
            data = pack.read(path)
            if not data:
                continue
            if path.lower().startswith('data/sprites/') and path.lower().endswith('.bin'):
                for sheet in parse_animation(data):
                    pack.name('Data/Sprites/' + sheet)
            # plain strings, and scene attributes (stored as UTF-16)
            texts = {m.decode('latin1').strip() for m in pattern.findall(data)}
            for start in (0, 1):
                wide = data[start:len(data) - ((len(data) - start) & 1)].decode('utf-16-le', 'replace').encode('latin1', 'replace')
                texts.update(m.decode('latin1').strip() for m in pattern.findall(wide))
            for text in texts:
                for prefix in SOURCE_PREFIXES:
                    pack.name(prefix + text)
        if len(pack.names) == count:
            return pack.names


def discover_names_deep(pack, source_dirs):
    """discover_names, then folder x word combinations for the files only built at run time (sprites, sound effects
    and meshes picked by sprintf)."""
    discover_names(pack, source_dirs)
    tokens = set()
    for source in source_dirs:
        for root, _, files in os.walk(source):
            for f in files:
                if f.endswith(('.c', '.h')):
                    tokens.update(re.findall(r'[A-Za-z][A-Za-z0-9_]{1,30}', open(os.path.join(root, f), encoding='latin1').read()))
    for path in list(pack.names.values()):
        if path.lower().endswith('.bin'):
            tokens.update(t.decode() for t in re.findall(rb'[A-Za-z][A-Za-z0-9_]{1,30}', pack.read(path) or b''))
    folders = {n.split('/')[2] for n in pack.names.values()
               if n.startswith(('Data/Sprites/', 'Data/Stages/', 'Data/SoundFX/', 'Data/Meshes/')) and n.count('/') >= 3}
    for folder in folders:
        for t in tokens:
            pack.name('Data/Sprites/%s/%s.bin' % (folder, t))
            pack.name('Data/Sprites/%s/%s.gif' % (folder, t))
            pack.name('Data/SoundFX/%s/%s.wav' % (folder, t))
            pack.name('Data/Meshes/%s/%s.bin' % (folder, t))
    for t in tokens:
        for prefix in ('Data/SoundFX/', 'Data/Meshes/', 'Data/Sprites/'):
            for ext in ('wav', 'bin', 'gif'):
                pack.name(prefix + t + '.' + ext)
    return pack.names
