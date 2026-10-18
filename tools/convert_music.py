"""Converts the music in Data.rsdk to ATRAC3 (.at3) for the PSP port.

The PSP decodes ATRAC3 on its Media Engine (sceAtrac), so playing these instead of the OGG files frees the main CPU
from Vorbis decoding, streams from the Memory Stick instead of loading whole tracks into RAM, and shortens stage
loads. The engine looks for Music/<hash>.at3 next to Data.rsdk, where <hash> is the pack's MD5 of the track's path
(the same 16 bytes the pack's file table stores), and falls back to the OGG when there is none.

Needs ffmpeg on the PATH and atracdenc (https://github.com/dcherednik/atracdenc, Windows build 0.2.3); its raw
output is used as is - rewriting its RIFF header made the PSP's decoder reject the files.

usage: python tools/convert_music.py <Data.rsdk> <output Music folder> <atracdenc.exe> [jobs]
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor


FFMPEG = None


def read_pack(path):
    with open(path, 'rb') as f:
        if f.read(4) != b'RSDK':
            raise SystemExit('not an RSDK data pack: ' + path)
        f.read(2)  # 'v', version
        (count,) = struct.unpack('<H', f.read(2))
        entries = []
        for _ in range(count):
            digest = f.read(16)
            offset, size = struct.unpack('<II', f.read(8))
            entries.append((digest.hex().upper(), offset, size & 0x7FFFFFFF, bool(size & 0x80000000)))
    return entries


def convert(pack, entry, out_dir, atracdenc):
    name, offset, size, encrypted = entry
    target = os.path.join(out_dir, name + '.at3')
    if os.path.exists(target):
        return name, 'kept'
    with open(pack, 'rb') as f:
        f.seek(offset)
        data = f.read(size)
    with tempfile.TemporaryDirectory() as tmp:
        ogg = os.path.join(tmp, 'in.ogg')
        wav = os.path.join(tmp, 'in.wav')
        at3 = os.path.join(tmp, 'out.at3')
        with open(ogg, 'wb') as f:
            f.write(data)
        # the PSP's ATRAC3 decoder plays 44.1kHz stereo
        subprocess.run([FFMPEG, '-v', 'error', '-y', '-i', ogg, '-ar', '44100', '-ac', '2', '-c:a', 'pcm_s16le', wav], check=True)
        subprocess.run([atracdenc, '-e', 'atrac3', '-i', wav, '-o', at3], check=True, stdout=subprocess.DEVNULL)
        os.replace(at3, target)
    return name, 'converted'


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    global FFMPEG
    pack, out_dir, atracdenc = sys.argv[1], sys.argv[2], os.path.abspath(sys.argv[3])
    FFMPEG = shutil.which('ffmpeg')
    if not FFMPEG:
        raise SystemExit('ffmpeg not found on the PATH')
    if not os.path.exists(atracdenc):
        raise SystemExit('atracdenc not found: ' + atracdenc)
    jobs = int(sys.argv[4]) if len(sys.argv) > 4 else os.cpu_count() or 4
    os.makedirs(out_dir, exist_ok=True)

    music = []
    with open(pack, 'rb') as f:
        for entry in read_pack(pack):
            _, offset, size, encrypted = entry
            if encrypted or size < 4:
                continue
            f.seek(offset)
            if f.read(4) == b'OggS':
                music.append(entry)
    print(f'{len(music)} OGG tracks in {pack}')

    done = 0
    with ThreadPoolExecutor(jobs) as pool:
        for name, status in pool.map(lambda e: convert(pack, e, out_dir, atracdenc), music):
            done += 1
            print(f'[{done}/{len(music)}] {name}: {status}')


if __name__ == '__main__':
    main()
