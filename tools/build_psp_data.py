"""Builds Data.psp, the PSP port's data pack, from your own copy of Sonic Mania's Data.rsdk.

Everything the game loads comes from this one file, prepared so the PSP does as little work as possible at load time:
  - files are stored decrypted: the names their keys come from are recovered by rsdk_pack.discover_names_deep, and the
    files no name turns up for are decrypted from their content by rsdk_keyless (only the PC's Direct3D shaders, which
    the PSP never loads, stay encrypted);
  - music is stored as ATRAC3 instead of Ogg Vorbis: the PSP's Media Engine decodes it while the track streams from the
    pack (no Vorbis decoding on the main CPU, no whole track in RAM);
  - images (sprite sheets, stage tiles) are stored already decoded, their pixels packed with LZ4 (smaller than the GIFs
    were, and far quicker to unpack than GIF's LZW) - the engine's ImageGIF::Load reads either;
  - the videos (Theora) become a JPEG per frame at the PSP's resolution, which the PSP decodes in hardware (sceJpeg);
    the 1024x512 images shown full screen (Data/Images) become a one-frame video the same way;
  - the PSP menu's achievements button and page heading are added, put together from the menu's own lettering
    (psp_achievements.py), and so is Portuguese, a language the PSP port adds (psp_portuguese.py: the menu's words in
    Portuguese in the same lettering, and the dialogs' text).

The output contains your game's data: it is for your own use, not for sharing.

usage: python tools/build_psp_data.py <Data.rsdk> <Data.psp> [--atracdenc tools/atracdenc/atracdenc.exe] [--music-cache dir]
                                     [--no-portuguese] [--no-achievements]
needs: pip install numpy pillow lz4; ffmpeg (and ffprobe) on the PATH (music, videos)
"""
import argparse
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

import lz4.block
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import psp_achievements  # noqa: E402
import psp_portuguese  # noqa: E402
import rsdk_keyless  # noqa: E402
import rsdk_pack  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCES = [os.path.join(HERE, '..', 'SonicManiaDecomp')]

# What the PSP port adds to the game (both on unless turned off here, or with --no-portuguese / --no-achievements):
ADD_PORTUGUESE = True    # Portuguese as a language (the language menu gets a PORTUGUÊS button)
ADD_ACHIEVEMENTS = True  # the main menu's ACHIEVEMENTS button and page (the PSP has no system list of them)

MAGIC = b'RPSP'
VERSION = 1
KIND_PLAIN, KIND_ENCRYPTED, KIND_AT3 = 0, 1, 2
ALIGN = 64


def gif_to_sheet(gif):
    """A GIF as the engine's pre-decoded image (see ImageGIF::Load): 'PSHT', version, width, height, packed size, the
    palette, then the palette indices as an LZ4 block. None for a GIF this layout can't stand in for exactly (the engine
    then decodes the GIF as before)."""
    if gif[:6] not in (b'GIF89a', b'GIF87a') or len(gif) < 13:
        return None
    width, height = struct.unpack_from('<HH', gif, 6)
    flags = gif[10]
    if not flags & 0x80:
        return None
    colours = 2 << (flags & 7)
    pos = 13 + 3 * colours
    if len(gif) < pos + 10 or gif[pos] != 0x2C:
        return None  # extensions before the image: not something the game's GIFs have
    x, y, w, h, image_flags = struct.unpack_from('<HHHHB', gif, pos + 1)
    if (x, y, w, h) != (0, 0, width, height) or image_flags & 0x80:
        return None  # the engine decodes the screen size, and handles a local palette its own way
    image = Image.open(io.BytesIO(gif))
    if image.mode != 'P':
        return None
    pixels = image.tobytes()
    if len(pixels) != width * height:
        return None
    packed = lz4.block.compress(pixels, mode='high_compression', compression=12, store_size=False)
    palette = gif[13:13 + 3 * colours].ljust(0x300, b'\0')
    return b'PSHT' + struct.pack('<HHHIH', 1, width, height, len(packed), 0) + palette + packed


VIDEO_W, VIDEO_H, VIDEO_PITCH = 480, 272, 512  # the PSP's screen; frames are encoded 512 wide (the decoder's pitch)


def video_blob(frames, fps_num, fps_den):
    """The engine's video (see Graphics/Video.cpp): 'PSVD', version, size shown, JPEG width, frame rate, frame count,
    largest frame, then an offset/size per frame, then the JPEGs."""
    header = b'PSVD' + struct.pack('<HHHHIIII', 1, VIDEO_W, VIDEO_H, VIDEO_PITCH, fps_num, fps_den, len(frames), max(map(len, frames)))
    offset = len(header) + 8 * len(frames)
    table = b''
    for jpeg in frames:
        table += struct.pack('<II', offset, len(jpeg))
        offset += len(jpeg)
    return header + table + b''.join(frames)


def convert_video(ogv, ffmpeg):
    """A Theora video as JPEG frames at the PSP's resolution (padded to the decoder's 512-pixel pitch).

    One frame per Theora packet, played back to back at the stream's frame rate - which is how the game plays them:
    libtheora numbers frames by counting packets and ignores the Ogg pages' granule positions. Mania.ogv's granules
    skip 180 frames (172 of them right after the first frame); honouring them, as ffmpeg does by default, holds the
    first frame for 7 seconds and leaves the picture that far behind the music."""
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'in.ogv')
        with open(src, 'wb') as f:
            f.write(ogv)
        probe = subprocess.run([shutil.which('ffprobe') or 'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                                'stream=r_frame_rate', '-of', 'default=nw=1:nk=1', src], check=True, capture_output=True, text=True)
        fps_num, fps_den = (int(x) for x in probe.stdout.strip().split('/'))
        subprocess.run([ffmpeg, '-v', 'error', '-y', '-i', src, '-fps_mode', 'passthrough', '-vf',
                        'scale=%d:%d:flags=lanczos,pad=%d:%d:0:0' % (VIDEO_W, VIDEO_H, VIDEO_PITCH, VIDEO_H),
                        '-c:v', 'mjpeg', '-pix_fmt', 'yuvj420p', '-q:v', '3', os.path.join(tmp, '%05d.jpg')], check=True)
        frames = []
        for name in sorted(n for n in os.listdir(tmp) if n.endswith('.jpg')):
            with open(os.path.join(tmp, name), 'rb') as f:
                frames.append(f.read())
    return video_blob(frames, fps_num, fps_den)


def convert_image(png):
    """A full-screen image (1024x512 PNG) as a one-frame video."""
    image = Image.open(io.BytesIO(png)).convert('RGBA')
    flat = Image.new('RGB', image.size)
    flat.paste(image, mask=image.getchannel('A'))
    frame = Image.new('RGB', (VIDEO_PITCH, VIDEO_H))
    frame.paste(flat.resize((VIDEO_W, VIDEO_H), Image.LANCZOS))
    out = io.BytesIO()
    frame.save(out, 'JPEG', quality=92, subsampling=2, progressive=False)  # baseline 4:2:0, what the decoder takes
    return video_blob([out.getvalue()], 1, 1)


def convert_at3(ogg, atracdenc, ffmpeg):
    with tempfile.TemporaryDirectory() as tmp:
        src, wav, at3 = (os.path.join(tmp, n) for n in ('in.ogg', 'in.wav', 'out.at3'))
        with open(src, 'wb') as f:
            f.write(ogg)
        subprocess.run([ffmpeg, '-v', 'error', '-y', '-i', src, '-ar', '44100', '-ac', '2', '-c:a', 'pcm_s16le', wav], check=True)
        # atracdenc's raw output as is: rewriting its RIFF header made the PSP's decoder reject the files
        subprocess.run([atracdenc, '-e', 'atrac3', '-i', wav, '-o', at3], check=True, stdout=subprocess.DEVNULL)
        with open(at3, 'rb') as f:
            return f.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rsdk')
    ap.add_argument('out')
    ap.add_argument('--atracdenc', default=os.path.join(HERE, 'atracdenc', 'atracdenc.exe'))
    ap.add_argument('--music-cache', default=None, help='folder of already converted <hash>.at3 files (also filled in)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--no-portuguese', dest='portuguese', action='store_false', default=ADD_PORTUGUESE,
                    help='leave Portuguese out (the language menu stays as the game has it)')
    ap.add_argument('--no-achievements', dest='achievements', action='store_false', default=ADD_ACHIEVEMENTS,
                    help='leave the achievements menu out (no ACHIEVEMENTS button on the main menu)')
    args = ap.parse_args()

    pack = rsdk_pack.Pack(args.rsdk)
    print('naming files...')
    names = rsdk_pack.discover_names_deep(pack, SOURCES)
    print('  %d of %d files named' % (len(names), len(pack.entries)))
    print('decrypting the rest from their contents...')
    recovered = rsdk_keyless.recover_unnamed(pack, names, SOURCES)
    print('  %d more files decrypted' % len(recovered))

    music = [k for k, (off, size, enc) in pack.entries.items() if not enc and pack.raw(k)[:4] == b'OggS']
    cache = args.music_cache or os.path.join(os.path.dirname(os.path.abspath(args.out)), 'Music')
    os.makedirs(cache, exist_ok=True)
    todo = [k for k in music if not os.path.exists(os.path.join(cache, k + '.at3'))]
    if todo:
        ffmpeg = shutil.which('ffmpeg')
        if not ffmpeg or not os.path.exists(args.atracdenc):
            raise SystemExit('converting music needs ffmpeg on the PATH and atracdenc (%s)' % args.atracdenc)
        print('converting %d music tracks to ATRAC3...' % len(todo))

        def one(k):
            data = convert_at3(pack.raw(k), os.path.abspath(args.atracdenc), ffmpeg)
            with open(os.path.join(cache, k + '.at3'), 'wb') as f:
                f.write(data)
        with ThreadPoolExecutor(args.jobs) as pool:
            list(pool.map(one, todo))

    videos = sorted(k for k, path in names.items() if path.lower().endswith('.ogv'))
    print('converting %d videos to JPEG frames...' % len(videos))
    ffmpeg = shutil.which('ffmpeg')
    if videos and not ffmpeg:
        raise SystemExit('converting the videos needs ffmpeg on the PATH')
    with ThreadPoolExecutor(min(args.jobs, 4)) as pool:
        converted = dict(zip(videos, pool.map(lambda k: convert_video(pack.read(names[k]), ffmpeg), videos)))
    for k, path in names.items():
        if path.lower().startswith('data/images/') and path.lower().endswith('.png'):
            converted[k] = convert_image(pack.read(path))

    # stored in load order when tools/psp_load_order.txt is there (make_load_order.py: a stage load then reads mostly
    # forward instead of jumping around the Memory Stick), the rest in path order, unnamed files last
    rank = {}
    order_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'psp_load_order.txt')
    if os.path.exists(order_path):
        for line in open(order_path, encoding='utf-8'):
            line = line.strip()
            if line and not line.startswith('#'):
                rank.setdefault(rsdk_pack.name_hash(line), len(rank))
        print('  load order: %d of the files from %s' % (sum(1 for k in pack.entries if k in rank), os.path.basename(order_path)))
    keys = sorted(pack.entries, key=lambda k: (rank.get(k, len(rank)), k not in names, names.get(k, '').lower(), k))

    extra = {}
    if args.achievements:
        print('putting together the achievements menu lettering...')
        extra.update({rsdk_pack.name_hash(path): data for path, data in psp_achievements.build(pack)[0].items()})
    if args.portuguese:
        print('putting together Portuguese...')
        extra.update({rsdk_pack.name_hash(path): data for path, data in psp_portuguese.build(pack)[0].items()})
    keys += [k for k in extra if k not in pack.entries]
    table = []
    sheets = 0
    offset = 0
    header_size = 12 + len(keys) * 28
    data_start = (header_size + ALIGN - 1) & ~(ALIGN - 1)
    print('writing %s...' % args.out)
    with open(args.out + '.tmp', 'wb') as out:
        out.write(b'\0' * data_start)
        for k in keys:
            _, size, enc = pack.entries.get(k, (0, 0, False))
            if k in extra:
                kind, data = KIND_PLAIN, extra[k]
            elif k in music:
                kind = KIND_AT3
                with open(os.path.join(cache, k + '.at3'), 'rb') as f:
                    data = f.read()
            elif enc and k in names:
                kind, data = KIND_PLAIN, rsdk_pack.decrypt_fast(pack.raw(k), names[k])
            elif enc and k in recovered:
                kind, data = KIND_PLAIN, recovered[k][1]
            elif enc:
                kind, data = KIND_ENCRYPTED, pack.raw(k)
            else:
                kind, data = KIND_PLAIN, pack.raw(k)
            if k in converted:
                data = converted[k]
            if kind == KIND_PLAIN and data[:3] == b'GIF':
                sheet = gif_to_sheet(data)
                if sheet:
                    data = sheet
                    sheets += 1
            pos = out.tell()
            out.write(data)
            out.write(b'\0' * (-out.tell() % ALIGN))
            table.append((k, pos, len(data), kind))
        out.seek(0)
        out.write(MAGIC + struct.pack('<II', VERSION, len(table)))
        for k, pos, size, kind in sorted(table):
            out.write(bytes.fromhex(k) + struct.pack('<IIBxxx', pos, size, kind))
    os.replace(args.out + '.tmp', args.out)
    kinds = [t[3] for t in table]
    print('done: %d files (%d decrypted, %d of them images pre-decoded, %d videos/full-screen images as JPEG frames; '
          '%d still encrypted; %d ATRAC3 music), %.1f MB' % (
              len(table), kinds.count(KIND_PLAIN), sheets, len(converted), kinds.count(KIND_ENCRYPTED), kinds.count(KIND_AT3),
              os.path.getsize(args.out) / 1e6))


if __name__ == '__main__':
    main()
