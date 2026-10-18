"""Checks the PSP achievements page's texts (SonicMania/Objects/Menu/PSPAchievementText.h) against the game's fonts:
every character of a description (and of a Japanese/Korean/Chinese name) must be in UI/SmallFont, every character of a
Latin name, in capitals as MainMenu.c draws it, in UIElements' bold letters (or have a stand-in there).

usage: python tools/check_ach_text.py <Data.rsdk>
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rsdk_pack  # noqa: E402

HEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'SonicManiaDecomp', 'SonicMania', 'Objects', 'Menu',
                      'PSPAchievementText.h')
LANGUAGES = ['EN', 'FR', 'IT', 'GE', 'SP', 'JP', 'TC', 'PT']  # the header's tables (Korean and Simplified Chinese use others)
LATIN = 5
# MainMenu.c's stand-ins for capitals the bold letters lack
STAND_INS = {'Ã': 'A', 'Õ': 'O', 'Ç': 'C', 'Ñ': 'N', 'Ú': 'U', 'Û': 'U', 'Ô': 'O', 'Î': 'I', 'Ï': 'I', 'Â': 'A', 'Ì': 'I', 'Ò': 'O', 'Ë': 'E', 'ß': 'SS'}


def font_chars(pack, path, list_id):
    d = pack.read(path)
    r = rsdk_pack.Reader(d)
    r.u32()
    r.u32()
    [r.string() for _ in range(r.u8())]
    hitboxes = [r.string() for _ in range(r.u8())]
    for a in range(r.u16()):
        r.string()
        count = r.u16()
        r.pos += 4
        chars = set()
        for _ in range(count):
            r.pos += 3
            chars.add(chr(r.u16()))
            r.pos += 12 + 8 * len(hitboxes)
        if a == list_id:
            return chars
    return set()


def capitals(text):
    out = ''
    for ch in text:
        if ch == 'ß':
            out += 'SS'
            continue
        up = ch.upper() if ('a' <= ch <= 'z' or 'à' <= ch <= 'þ') and ch != '÷' else ch
        out += STAND_INS.get(up, up)
    return out


def main():
    pack = rsdk_pack.Pack(sys.argv[1])
    small = font_chars(pack, 'Data/Sprites/UI/SmallFont.bin', 0)
    bold = font_chars(pack, 'Data/Sprites/UI/UIElements.bin', 4)
    src = open(HEADER, encoding='utf-8').read()
    body = src[src.index('pspAchText'):]
    strings = [bytes(s, 'utf-8').decode('unicode_escape').encode('latin1').decode('utf-8')
               for s in re.findall(r'"((?:[^"\\]|\\.)*)"', body)]
    per_lang = 18 * 2
    bad = 0
    for li, lang in enumerate(LANGUAGES):
        texts = strings[li * per_lang:(li + 1) * per_lang]
        missing = set()
        for i, text in enumerate(texts):
            is_name = i % 2 == 0
            if is_name and (li < LATIN or lang == 'PT'):
                missing |= {ch for ch in capitals(text) if ch not in bold}
            else:
                missing |= {ch for ch in text if ch not in small}
        if missing:
            bad += 1
            print('%s: missing %s' % (lang, ' '.join(sorted(missing))))
    print('ok' if not bad else '%d languages need changes' % bad)


if __name__ == '__main__':
    main()
