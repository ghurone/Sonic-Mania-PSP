"""Portuguese for the PSP port: the menu's words in Portuguese, in the menu's own lettering.

Sonic Mania's menu words are pictures, a set per language (UI/Text*.bin: buttons, prompts, options, pause menu;
UI/Headings*.bin: the yellow headings). Portuguese gets its own sets, TextPT and HeadingsPT, with the same lists and
frames as the English ones (the game picks them by number), put together from your own Data.rsdk:
  - a Portuguese word another language already has (IDIOMA, CONTROLES, MÚSICA, PAUSA...) is that language's picture;
  - the others are composed letter by letter, in the lettering the English frame uses. The letters come from frames
    whose text is known (every English one, and a few of the other languages' for their accents); one lettering per
    style and size, and the pause menu's apart (its pictures use the stage palette's colours, not the menu's);
  - the accents Portuguese needs and a lettering lacks (Ã, Õ, Ç, Ê...) are put on the plain letter: the mark taken
    from another accented letter of that lettering when it has one (an acute turned into a circumflex), else drawn
    small in its colours (the tilde, the cedilla);
  - frames without words (icons, numbers, names) stay the English ones.
The dialogs' text, Data/Strings/StringsPT.txt, is tools/lang/StringsPT.txt.

build_psp_data.py stores all of it in Data.psp. To add it to a Data.psp built before:
    python tools/psp_portuguese.py <Data.rsdk> <Data.psp>
    python tools/psp_portuguese.py <Data.rsdk> --preview <png>   (the Portuguese frames next to the English ones)
"""
import argparse
import io
import os
import struct
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import psp_achievements  # noqa: E402
import rsdk_pack  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

# what frames say (language, list, frame): the key to the lettering. English: every frame with words; the others: the
# ones whose accents (or pause-menu letters) Portuguese needs
KNOWN_TEXT = {
    ('EN', 0, 0): 'CONFIRM', ('EN', 0, 1): 'BACK', ('EN', 0, 2): 'NEXT', ('EN', 0, 3): 'DELETE', ('EN', 0, 4): 'GO!!',
    ('EN', 0, 5): 'LDR.BOARDS', ('EN', 0, 6): 'SECRETS', ('EN', 0, 7): 'UP', ('EN', 0, 8): 'DOWN', ('EN', 0, 9): 'LEFT',
    ('EN', 0, 10): 'RIGHT', ('EN', 0, 11): 'AUX.', ('EN', 0, 12): 'START', ('EN', 0, 13): 'JUMP', ('EN', 0, 14): 'TOP RANK',
    ('EN', 0, 15): 'MY RANK', ('EN', 0, 16): 'HELP', ('EN', 0, 17): 'APPLY', ('EN', 0, 18): 'GAME OPTS.', ('EN', 0, 19): 'SORT',
    ('EN', 0, 20): 'MANIA', ('EN', 0, 21): 'ENCORE', ('EN', 0, 22): 'RESET',
    ('EN', 1, 0): 'MANIA MODE', ('EN', 1, 1): 'TIME ATTACK', ('EN', 1, 2): 'COMPETITION', ('EN', 1, 3): 'OPTIONS',
    ('EN', 1, 4): 'EXTRAS', ('EN', 1, 5): 'ENCORE MODE', ('EN', 1, 6): 'DLC', ('EN', 1, 7): 'EXIT',
    ('EN', 3, 0): 'VIDEO', ('EN', 3, 1): 'SOUND', ('EN', 3, 2): 'CONTROLS', ('EN', 3, 3): 'LANGUAGE', ('EN', 3, 4): 'DATA',
    ('EN', 4, 0): 'NONE', ('EN', 4, 1): 'CLEAN', ('EN', 4, 2): 'CRT-SHARP', ('EN', 4, 3): 'VIDEO', ('EN', 4, 4): 'SCREEN FILTER',
    ('EN', 4, 5): 'CRT-SOFT',
    ('EN', 5, 0): 'VOLUME ADJUST', ('EN', 5, 1): 'MUSIC', ('EN', 5, 2): 'SFX', ('EN', 5, 3): 'SOUND',
    ('EN', 6, 0): 'BUTTON CONFIG', ('EN', 6, 1): 'MOVE', ('EN', 6, 2): 'JUMP', ('EN', 6, 3): 'LOOK UP', ('EN', 6, 4): 'CROUCH / SPIN',
    ('EN', 6, 5): 'CONTROLS',
    ('EN', 7, 0): '???', ('EN', 7, 1): 'MEAN BEAN', ('EN', 7, 2): '1P VS CPU', ('EN', 7, 3): '1P VS 2P', ('EN', 7, 4): 'D.A. GARDEN',
    ('EN', 7, 5): 'BLUE SPHERES', ('EN', 7, 6): 'ORIGINAL', ('EN', 7, 7): 'MANIA', ('EN', 7, 8): 'STAFF CREDITS',
    ('EN', 8, 0): 'SONIC', ('EN', 8, 1): 'TAILS', ('EN', 8, 2): 'KNUCKLES', ('EN', 8, 3): 'MIGHTY', ('EN', 8, 4): 'RAY',
    ('EN', 9, 0): 'YES', ('EN', 9, 1): 'NO', ('EN', 9, 2): 'OK', ('EN', 9, 3): 'CANCEL', ('EN', 9, 4): 'CONTINUE',
    ('EN', 10, 0): 'CONTINUE', ('EN', 10, 1): 'RESTART', ('EN', 10, 2): 'EXIT', ('EN', 10, 3): 'PAUSED',
    ('EN', 11, 0): 'RANK', ('EN', 11, 1): 'PLAYER', ('EN', 11, 2): 'TIME', ('EN', 11, 3): 'ACT 1', ('EN', 11, 4): 'ACT 2',
    ('EN', 11, 5): 'SONIC', ('EN', 11, 6): 'TAILS', ('EN', 11, 7): 'KNUCKLES',
    ('EN', 12, 0): 'VS', ('EN', 12, 1): 'FIXED', ('EN', 12, 2): 'RANDOM', ('EN', 12, 3): 'TELEPORTERS ONLY', ('EN', 12, 4): 'ROUNDS',
    ('EN', 12, 5): 'ITEMS', ('EN', 12, 6): 'MATCH LENGTH', ('EN', 12, 8): '1P', ('EN', 12, 9): '2P', ('EN', 12, 10): '3P',
    ('EN', 12, 11): '4P', ('EN', 12, 12): 'SCREEN LAYOUT',
    ('EN', 13, 0): 'RINGS', ('EN', 13, 2): 'SCORE', ('EN', 13, 3): 'ITEMS', ('EN', 13, 4): 'ROUND', ('EN', 13, 5): 'TIME',
    ('EN', 13, 6): 'P1', ('EN', 13, 7): 'P2', ('EN', 13, 8): 'WINS', ('EN', 13, 9): 'TOTAL',
    ('EN', 14, 0): 'DEBUG MODE', ('EN', 14, 1): 'SONIC ABILITY', ('EN', 14, 2): '& KNUCKLES', ('EN', 14, 3): 'OFF', ('EN', 14, 4): 'ON',
    ('EN', 14, 5): 'MANIA', ('EN', 14, 6): 'CD', ('EN', 14, 7): '3&K', ('EN', 14, 8): 'TIME LIMIT',
    ('EN', 15, 0): 'ENGLISH', ('EN', 15, 1): 'FRANÇAIS', ('EN', 15, 2): 'ITALIANO', ('EN', 15, 3): 'DEUTSCH', ('EN', 15, 4): 'ESPAÑOL',
    ('EN', 16, 0): 'ACT 1', ('EN', 16, 1): 'ACT 2', ('EN', 16, 5): 'ACT 1', ('EN', 16, 6): 'ACT 2', ('EN', 16, 7): 'WATCH',
    ('EN', 16, 8): 'CHALLENGE', ('EN', 16, 9): 'DELETE', ('EN', 16, 10): 'BY DATE', ('EN', 16, 11): 'BY ZONE', ('EN', 16, 12): 'ACT',
    ('EN', 16, 15): 'MANIA', ('EN', 16, 16): 'ENCORE',
    ('EN', 17, 0): 'EXIT', ('EN', 17, 1): 'DEFAULT', ('EN', 17, 2): 'SCREEN FILTER', ('EN', 17, 3): 'NONE', ('EN', 17, 4): 'CLEAN',
    ('EN', 17, 5): 'CRT-SHARP', ('EN', 17, 6): 'CRT-SOFT', ('EN', 17, 7): 'WINDOW SIZE', ('EN', 17, 12): 'BIG PICTURE',
    ('EN', 17, 13): 'WINDOW BORDER', ('EN', 17, 14): 'FULL SCREEN', ('EN', 17, 15): 'VSYNC', ('EN', 17, 16): 'TRIPLE BUFFERING',
    ('EN', 17, 17): 'OFF', ('EN', 17, 18): 'ON',
    ('EN', 18, 0): 'SAVED GAMES', ('EN', 18, 1): 'MEDALLIONS', ('EN', 18, 2): 'TIME ATTACK DATA', ('EN', 18, 3): 'REPLAYS',
    ('EN', 18, 4): 'ALL GAME DATA',
    # accents and letters English lacks
    ('SP', 3, 0): 'VÍDEO', ('SP', 5, 1): 'MÚSICA', ('SP', 7, 8): 'CRÉDITOS', ('SP', 16, 5): 'ACTO 1', ('SP', 16, 0): 'ACTO 1',
    ('SP', 16, 1): 'ACTO 2', ('SP', 6, 1): 'MOVERSE', ('SP', 12, 1): 'FIJOS', ('SP', 0, 1): 'ATRÁS', ('SP', 0, 9): 'IZQUIERDA',
    ('SP', 17, 5): 'TV NÍTIDA', ('FR', 7, 5): 'SPHÈRES BLEUES', ('FR', 0, 5): 'CLASSEMENT', ('FR', 1, 7): 'QUITTER',
    ('FR', 10, 1): 'RECOMMENCER', ('FR', 10, 2): 'QUITTER', ('FR', 9, 0): 'OUI', ('FR', 9, 1): 'NON', ('SP', 10, 2): 'SALIR',
    ('FR', 17, 1): 'DÉFAUT', ('IT', 13, 8): 'VINCE', ('SP', 13, 2): 'PUNTOS', ('SP', 13, 3): 'OBJETOS', ('SP', 13, 5): 'TIEMPO',
    ('SP', 13, 4): 'RONDAS', ('SP', 7, 5): 'ESFERAS AZULES',
    # the narrow lettering (long words)
    ('FR', 1, 1): 'CONTRE LA MONTRE', ('FR', 14, 8): 'LIMITE DE TEMPS', ('FR', 5, 0): 'RÉGLAGE DU VOLUME', ('FR', 12, 6): 'DURÉE DU MATCH',
    ('FR', 5, 2): 'EFFETS', ('FR', 4, 0): 'PAS DE FILTRE', ('FR', 14, 1): 'COMPÉTENCE SONIC', ('FR', 18, 4): 'DONNÉES DU JEU',
    ('FR', 9, 4): 'CONTINUER', ('FR', 10, 0): 'CONTINUER',
    ('SP', 17, 7): 'TAMAÑO DE PANTALLA', ('SP', 10, 0): 'CONTINUAR',
}
# frames set in the narrow lettering whatever their width: the pause menu's two long items, which stand furthest right
# and would run off screen (SAIR is short: the usual letters, as Spanish's SALIR)
PT_NARROW = {(10, 0), (10, 1)}

HEADING_TEXT = {
    'EN': ['MAIN MENU', 'MANIA MODE', 'TIME ATTACK', 'COMPETITION', 'RESULTS', 'OPTIONS', 'EXTRAS', 'LEADERBOARDS', 'GAME OPTIONS',
           'PLAYER SELECT', 'ZONE SELECT', 'REPLAYS', 'ENCORE MODE'],
    'FR': ['MENU PRINCIPAL', 'MODE MANIA', 'CONTRE LA MONTRE', 'COMPÉTITION', 'RÉSULTATS', 'OPTIONS', 'BONUS', 'CLASSEMENT',
           'OPTIONS DU JEU', 'CHOISIR UN JOUEUR', 'CHOISIR UNE ZONE', 'REPLAYS', 'MODE BIS'],
    'IT': ['MENU PRINCIPALE', 'MODALITÀ MANIA', 'CRONO', 'COMPETIZIONE', 'RISULTATI', 'OPZIONI', 'EXTRA', 'CLASSIFICA',
           'OPZIONI DI GIOCO', 'SELEZIONE GIOCATORE', 'SELEZIONE ZONA', 'REPLAY', 'MODALITÀ BIS'],
    'SP': ['MENÚ PRINCIPAL', 'MODO MANÍA', 'CRONO', 'COMPETICIÓN', 'RESULTADOS', 'OPCIONES', 'EXTRAS', 'CLASIFICACIONES', 'OPCIONES',
           'SELECCIONAR JUGADOR', 'SELECCIONAR ZONA', 'REPETICIONES', 'MODO BIS'],
    'GE': ['HAUPTMENÜ', 'MANIA-MODUS', 'GEGEN DIE ZEIT', 'WETTKAMPF', 'ERGEBNISSE', 'OPTIONEN', 'EXTRAS', 'BESTENLISTEN',
           'SPIELOPTIONEN', 'SPIELER WÄHLEN', 'ZONE WÄHLEN', 'REPLAYS', 'ZUGABEMODUS'],
}
HEADING_WIDTH = 214  # the heading bar's

# Portuguese: the English frame -> its words, or ('XX', list, frame) for another language's picture of them
PT_TEXT = {
    (0, 0): ('SP', 0, 0), (0, 1): 'VOLTAR', (0, 2): 'PRÓXIMO', (0, 3): 'APAGAR', (0, 4): 'VAI!!', (0, 5): 'RANKINGS',
    (0, 6): 'SEGREDOS', (0, 7): 'CIMA', (0, 8): 'BAIXO', (0, 9): 'ESQUERDA', (0, 10): 'DIREITA', (0, 13): 'PULAR',
    (0, 14): 'MELHORES', (0, 15): 'MINHA POS.', (0, 16): 'AJUDA', (0, 17): ('SP', 0, 17), (0, 18): 'AJUSTES', (0, 19): ('SP', 0, 19),
    (0, 22): 'ZERAR',
    (1, 0): 'MODO MANIA', (1, 1): 'CONTRA O TEMPO', (1, 2): 'COMPETIÇÃO', (1, 3): 'OPÇÕES', (1, 5): 'MODO ENCORE', (1, 7): 'SAIR',
    (2, 1): ('SP', 2, 1),
    (3, 0): ('SP', 3, 0), (3, 1): 'SOM', (3, 2): ('SP', 3, 2), (3, 3): ('SP', 3, 3), (3, 4): 'DADOS',
    (4, 0): 'NENHUM', (4, 1): 'LIMPO', (4, 2): 'CRT-NÍTIDO', (4, 3): ('SP', 4, 3), (4, 4): 'FILTRO DE TELA', (4, 5): 'CRT-SUAVE',
    (5, 0): 'AJUSTAR VOLUME', (5, 1): ('SP', 5, 1), (5, 2): 'EFEITOS', (5, 3): 'SOM',
    (6, 0): 'BOTÕES', (6, 1): 'MOVER', (6, 2): 'PULAR', (6, 3): 'OLHAR P/ CIMA', (6, 4): 'ABAIXAR / GIRAR',
    (6, 5): ('SP', 6, 5),
    (7, 5): 'ESFERAS AZUIS', (7, 8): ('SP', 7, 8),
    (9, 0): 'SIM', (9, 1): 'NÃO', (9, 3): ('SP', 9, 3), (9, 4): 'CONTINUAR',
    # (the pause menu's CONTINUAR narrow, as in Spanish: its top item stands furthest right, the usual letters would run
    # off the screen)
    (10, 0): 'CONTINUAR', (10, 1): 'REINICIAR', (10, 2): 'SAIR', (10, 3): ('SP', 10, 3),
    (11, 0): 'POSIÇÃO', (11, 1): 'JOGADOR', (11, 2): 'TEMPO', (11, 3): 'ATO 1', (11, 4): 'ATO 2',
    (12, 1): 'FIXO', (12, 2): 'ALEATÓRIO', (12, 3): 'SÓ TELEPORTES', (12, 4): 'RODADAS', (12, 5): 'ITENS', (12, 6): 'DURAÇÃO',
    (12, 7): ('SP', 12, 7), (12, 12): 'LAYOUT DA TELA',
    (13, 0): 'ANÉIS', (13, 2): 'PONTOS', (13, 3): 'ITENS', (13, 4): 'RODADA', (13, 5): 'TEMPO', (13, 8): 'VENCE',
    (14, 0): ('SP', 14, 0), (14, 1): 'HABILIDADE', (14, 3): 'NÃO', (14, 4): 'SIM', (14, 8): 'LIMITE DE TEMPO',
    (16, 0): 'ATO 1', (16, 1): 'ATO 2', (16, 5): 'ATO 1', (16, 6): 'ATO 2', (16, 7): 'ASSISTIR', (16, 8): ('SP', 16, 8),
    (16, 9): 'APAGAR', (16, 10): 'POR DATA', (16, 11): ('SP', 16, 11), (16, 12): 'ATO',
    (18, 0): 'JOGOS SALVOS', (18, 1): 'MEDALHÕES', (18, 2): 'RECORDES', (18, 4): 'TODOS OS DADOS',
    # players are J1, J2... as in Spanish and French; places 1º 2º 3º
    (7, 2): ('SP', 7, 2), (7, 3): ('SP', 7, 3), (12, 8): ('SP', 12, 8), (12, 9): ('SP', 12, 9), (12, 10): ('SP', 12, 10),
    (12, 11): ('SP', 12, 11), (13, 6): ('SP', 13, 6), (13, 7): ('SP', 13, 7), (16, 2): ('SP', 16, 2), (16, 3): ('SP', 16, 3),
    (16, 4): ('SP', 16, 4),
    (13, 1): 'TOTAL DE\nANÉIS',
    # the PC's video menu
    (17, 0): 'SAIR', (17, 1): 'PADRÃO', (17, 2): 'FILTRO DE TELA', (17, 3): 'NENHUM', (17, 4): 'LIMPO', (17, 5): 'CRT-NÍTIDO',
    (17, 6): 'CRT-SUAVE', (17, 7): 'TAMANHO DA JANELA', (17, 13): 'BORDA DA JANELA', (17, 14): 'TELA CHEIA', (17, 17): 'NÃO',
    (17, 18): 'SIM',
}
# frames of two lines of letters (each line read on its own)
KNOWN_LINES = {('EN', 13, 1): ['TOTAL', 'RINGS'], ('SP', 13, 1): ['TOTAL DE', 'RINGS']}


def frame_lines(frame):
    """a frame of several lines of letters, as a frame per line (split where a clear row parts them)"""
    rows = [any(row) for row in frame.pixels]
    lines, r = [], 0
    while r < frame.h:
        if rows[r]:
            st = r
            while r < frame.h and rows[r]:
                r += 1
            lines.append((st, r))
        else:
            r += 1
    out = []
    for a, b in lines:
        image = Image.new('P', (frame.w, b - a))
        for y in range(a, b):
            for x in range(frame.w):
                image.putpixel((x, y - a), frame.pixels[y][x])
        out.append(Frame(frame.lang, frame.lst, frame.frm, image, (0, 0, 0, frame.w, b - a, 0, 0), frame.family))
    return out


# the save slots' badges, each in its own lettering (box_word, badge_word)
PT_BADGES = {
    (2, 0): ('box', ('IT', 2, 0), 'NUOVO', 'NOVO'),
    (2, 1): ('box', ('IT', 2, 1), 'COMPLETATO', 'COMPLETO'),
    (2, 2): ('lines', ('SP', 2, 2), None, ['SEM', 'SALVAR']),  # two lines, as Spanish's SIN / GUARDAR
}
# the title screen's "press any button" (Title/Logo.bin list 8), and what its frames say
PT_PRESS = 'APERTE QUALQUER BOTÃO'
PRESS_TEXT = ['PRESS ANY BUTTON', 'APPUYEZ SUR UNE TOUCHE', 'APPUYEZ SUR UN BOUTON', 'PREMI UN TASTO', 'PREMI UN PULSANTE',
              'BELIEBIGE TASTE DRÜCKEN', 'BELIEBIGEN KNOPF DRÜCKEN', 'PULSA CUALQUIER BOTÓN']
# the language menu's own button: its name, in every language's set
PT_NAME = 'PORTUGUÊS'
PT_HEADINGS = ['MENU PRINCIPAL', 'MODO MANIA', 'CONTRA O TEMPO', 'COMPETIÇÃO', ('SP', 4), 'OPÇÕES', ('EN', 6), 'RANKINGS',
               'OPÇÕES DE JOGO', 'ESCOLHA O JOGADOR', 'ESCOLHA A ZONA', ('EN', 11), 'MODO ENCORE']

MARKS = {'Á': ('A', 'acute'), 'É': ('E', 'acute'), 'Í': ('I', 'acute'), 'Ó': ('O', 'acute'), 'Ú': ('U', 'acute'),
         'À': ('A', 'grave'), 'È': ('E', 'grave'), 'Â': ('A', 'circumflex'), 'Ê': ('E', 'circumflex'), 'Ô': ('O', 'circumflex'),
         'Ã': ('A', 'tilde'), 'Õ': ('O', 'tilde'), 'Ñ': ('N', 'tilde'), 'Ç': ('C', 'cedilla')}
# drawn marks, for letterings with no letter carrying one ('#': the mark, outlined in the lettering's colours)
TEMPLATES_BIG = {  # the headings' (32 pixels)
    'tilde': ['.###...##', '##.##.##.', '#...###..'],
    'cedilla': ['.##.', '..##', '###.'],
    'acute': ['..##', '.##.', '##..'],
    'grave': ['##..', '.##.', '..##'],
    'circumflex': ['..##..', '.####.', '##..##'],
}
TEMPLATES = {
    'tilde': ['.##..#', '#..##.'],
    'cedilla': ['.#.', '..#', '##.'],
    'acute': ['..#', '.#.'],
    'grave': ['#..', '.#.'],
    'circumflex': ['.#.', '#.#'],
}


class Glyph:
    """a letter: its pixels (palette indices, 0 = clear), with top rows above the line"""

    def __init__(self, pixels, top):
        self.pixels = [list(r) for r in pixels]
        self.top = top

    @property
    def width(self):
        return len(self.pixels[0]) if self.pixels else 0

    def rows(self):
        filled = [r for r, row in enumerate(self.pixels) if any(row)]
        return (filled[0], filled[-1]) if filled else (0, -1)

    def key(self):
        a, b = self.rows()
        return tuple(tuple(r) for r in self.pixels[a:b + 1]), a - self.top


class Frame:
    def __init__(self, lang, lst, frm, image, rect, family):
        self.lang, self.lst, self.frm, self.family = lang, lst, frm, family
        self.sheet, x, y, self.w, self.h, self.px, self.py = rect
        self.pixels = [[image.getpixel((x + c, y + r)) for c in range(self.w)] for r in range(self.h)]
        values = {v for row in self.pixels for v in row}
        if family == 'title':
            self.style = 'shadow'  # the title's letters: a dark shadow on their right, no outline
            self.body_colours = values - {0, 1}
        elif 0 not in values:
            self.style = None  # a picture on a box
        elif self.h <= 16 and 185 in values:
            self.style = 'shadow'
        else:
            self.style = 'outline'
        self.outline = self._outline() if self.style == 'outline' else None
        # the line the letters stand on (rows above it in the frame): where most of them end
        ends = []
        for a, b in (self.runs() if self.style else []):
            filled = [r for r in range(self.h) if any(self.pixels[r][a:b])]
            if filled:
                ends.append(filled[-1] + 1)
        self.baseline = max(set(ends), key=ends.count) if ends else -self.py
        # the narrow lettering the menus use for long words: its letters are ~10 pixels wide, the usual ones ~14
        widths = sorted(b - a for a, b in self.runs()) if self.style == 'outline' else []
        self.narrow = len(widths) >= 3 and widths[len(widths) // 2] <= 11

    def _outline(self):
        # the colour that borders the clear pixels most
        counts = {}
        for r in range(self.h):
            for c in range(self.w):
                v = self.pixels[r][c]
                if v and ((c and not self.pixels[r][c - 1]) or (c + 1 < self.w and not self.pixels[r][c + 1])):
                    counts[v] = counts.get(v, 0) + 1
        return max(counts, key=counts.get) if counts else 1

    def font(self):
        if self.style == 'outline':
            size = 19 if self.h <= 20 else 32 if self.h >= 30 or self.lst == 13 else 22
        elif self.style == 'shadow' and self.family == 'title':
            size = 8
        elif self.style == 'shadow':
            size = 7 if self.h <= 9 or (self.lst == 13 and self.h == 16) else 11  # (13.1: two lines of the small one)
        else:
            return None
        return (self.style, size, self.family, getattr(self, 'narrow', False))

    def runs(self):
        """[(first column, end column)] of each letter"""
        w = self.w
        if self.style == 'outline':
            body = [any(self.pixels[r][c] not in (0, self.outline) for r in range(self.h)) for c in range(w)]
        else:
            colours = getattr(self, 'body_colours', {185})
            body = [any(self.pixels[r][c] in colours for r in range(self.h)) for c in range(w)]
        runs, c = [], 0
        while c < w:
            if body[c]:
                s = c
                while c < w and body[c]:
                    c += 1
                runs.append((max(0, s - 1), min(w, c + 1)) if self.style == 'outline' else (s, min(w, c + 1)))
            else:
                c += 1
        if self.family == 'title':
            # the title's letters are 8 columns (7 and the shadow): two that touch make one run of 16
            split = []
            for a, b in runs:
                while b - a >= 14:
                    split.append((a, a + 8))
                    a += 8
                split.append((a, b))
            runs = split
        return runs

    def glyph(self, run):
        a, b = run
        return Glyph([row[a:b] for row in self.pixels], self.baseline)


class Lettering:
    """the letters of every lettering, learnt from frames of known text"""

    def __init__(self):
        self.glyphs = {}  # (font, character) -> Glyph
        self.spaces = {}  # font -> [gap widths]

    def learn(self, frame, text):
        font = frame.font()
        runs = frame.runs()
        chars = [ch for ch in text if ch != ' ']
        # letters that touch: the widest run split where it is thinnest, until there are as many runs as letters
        while font is not None and 0 < len(runs) < len(chars):
            i = max(range(len(runs)), key=lambda k: runs[k][1] - runs[k][0])
            a, b = runs[i]
            if b - a < 8:
                break
            singles = sorted(y - x for x, y in runs if y - x < 14)
            usual = singles[len(singles) // 2] if singles else 8
            near = range(max(a + 3, a + usual - 2), min(b - 3, a + usual + 3))
            cut = min(near or range(a + 3, b - 3), key=lambda k: sum(1 for r in range(frame.h) if frame.pixels[r][k]))
            runs[i:i + 1] = [(a, cut), (cut, b)]
        if font is None or len(runs) != len(chars):
            return False
        i = 0
        for n, ch in enumerate(text):
            if ch == ' ':
                if 0 < i < len(runs):
                    self.spaces.setdefault(font, []).append(runs[i][0] - runs[i - 1][1])
                continue
            self.glyphs.setdefault((font, ch), frame.glyph(runs[i]))
            i += 1
        return True

    def space(self, font):
        gaps = sorted(self.spaces.get(font, []))
        return gaps[len(gaps) // 2] if gaps else max(3, font[1] // 3)

    def get(self, font, ch, make_marks=True):
        g = self.glyphs.get((font, ch))
        if g or not make_marks or ch not in MARKS:
            return g
        base_ch, kind = MARKS[ch]
        base = self.glyphs.get((font, base_ch))
        if not base:
            return None
        return with_mark(base, self.mark(font, kind), kind == 'cedilla')

    def mark(self, font, kind):
        """(pixels rows, gap to the letter) of a mark in this lettering"""
        for ch, (base_ch, k) in MARKS.items():
            if k != kind:
                continue
            acc, base = self.glyphs.get((font, ch)), self.glyphs.get((font, base_ch))
            if acc and base:
                return mark_of(acc, base, kind == 'cedilla')
        if kind == 'circumflex':
            acute = self.mark(font, 'acute')
            if acute and acute[2] != 'drawn':
                return circumflex_from_acute(acute)
        return drawn_mark(kind, self.colours(font, kind == 'cedilla'), font[1], font[0] == 'shadow')

    def colours(self, font, below=False):
        """(fill, edge) for a drawn mark: the colour that edges the letters (outline, or shadow), and the one they
        have most on the side the mark goes (their top rows, or their bottom ones)"""
        edges, fills = {}, {}
        for (f, ch), g in self.glyphs.items():
            if f != font or ch in MARKS:
                continue
            a, b = g.rows()
            band = range(b - 3, b - 1) if below else range(a + 1, a + 4)
            for r, row in enumerate(g.pixels):
                for c, v in enumerate(row):
                    if not v:
                        continue
                    if (c and not row[c - 1]) or (c + 1 < len(row) and not row[c + 1]):
                        edges[v] = edges.get(v, 0) + 1
                    if r in band:
                        fills[v] = fills.get(v, 0) + 1
        edge = (1 if font[2] == 'title' else 178) if font[0] == 'shadow' else max(edges, key=edges.get, default=1)
        fill = max((v for v in fills if v != edge), key=fills.get, default=185)
        return fill, edge


def mark_of(accented, base, below):
    """the mark of an accented letter: its rows beyond the plain letter's, centred on the letter's middle"""
    ba, bb = base.rows()
    aa, ab = accented.rows()
    # baseline coordinates: row r of a glyph is r - top
    if below:
        start = bb - base.top + 1
        rows = [r for r in range(accented.top + start, ab + 1)]
    else:
        end = ba - base.top  # the plain letter's first row
        rows = [r for r in range(aa, accented.top + end)]
    if not rows:
        return None
    pix = [accented.pixels[r] for r in rows]
    cols = [c for c in range(accented.width) if any(row[c] for row in pix)]
    pix = [row[cols[0]:cols[-1] + 1] for row in pix]
    gap = (ba - base.top) - (rows[-1] - accented.top) - 1 if not below else (rows[0] - accented.top) - (bb - base.top) - 1
    return pix, max(0, gap), 'taken'


def circumflex_from_acute(acute):
    pix, gap, _ = acute
    h, w = len(pix), len(pix[0])
    out = [[0] * (2 * w - 1) for _ in range(h)]
    for r in range(h):
        for c in range(w):
            if pix[r][c]:
                out[r][w - 1 + c] = pix[r][c]          # the acute: the right half
            if pix[r][w - 1 - c]:
                out[r][c] = out[r][c] or pix[r][w - 1 - c]  # its mirror: the left half
    return out, gap, 'derived'


def drawn_mark(kind, colours, size, shadowed=False):
    fill, edge = colours
    mask = (TEMPLATES_BIG if size >= 30 else TEMPLATES)[kind]
    if shadowed:
        # as the letters of a shadow lettering: the mark, its shadow one to the right and one below
        h, w = len(mask) + 1, len(mask[0]) + 1
        out = [[0] * w for _ in range(h)]
        for r, line in enumerate(mask):
            for c, ch in enumerate(line):
                if ch == '#':
                    for dr, dc in ((0, 1), (1, 0), (1, 1)):
                        if not out[r + dr][c + dc]:
                            out[r + dr][c + dc] = edge
        for r, line in enumerate(mask):
            for c, ch in enumerate(line):
                if ch == '#':
                    out[r][c] = fill
        return out, 1, 'drawn'
    h, w = len(mask) + 2, len(mask[0]) + 2
    out = [[0] * w for _ in range(h)]
    for r, line in enumerate(mask):
        for c, ch in enumerate(line):
            if ch == '#':
                for dr in (-1, 0, 1):
                    for dc in (-1, 0, 1):
                        if not out[r + 1 + dr][c + 1 + dc]:
                            out[r + 1 + dr][c + 1 + dc] = edge
    for r, line in enumerate(mask):
        for c, ch in enumerate(line):
            if ch == '#':
                out[r + 1][c + 1] = fill
    return out, 0, 'drawn'


def with_mark(base, mark, below):
    if not mark:
        return None
    pix, gap, _ = mark
    mh, mw = len(pix), len(pix[0])
    a, b = base.rows()
    w = max(base.width, mw)
    left = (w - base.width) // 2
    body = [c for c in range(base.width) if any(row[c] for row in base.pixels)]
    centre = left + (body[0] + body[-1] + 1) // 2 if body else w // 2
    mx = max(0, min(w - mw, centre - mw // 2))
    if below:
        need = b + 1 + gap + mh
        rows = [[0] * w for _ in range(max(len(base.pixels), need))]
        for r, row in enumerate(base.pixels):
            rows[r][left:left + base.width] = row
        for r in range(mh):
            for c in range(mw):
                if pix[r][c]:
                    rows[b + 1 + gap + r][mx + c] = pix[r][c]
        return Glyph(rows, base.top)
    extra = max(0, mh + gap - a)
    rows = [[0] * w for _ in range(len(base.pixels) + extra)]
    for r, row in enumerate(base.pixels):
        rows[r + extra][left:left + base.width] = row
    y0 = a + extra - gap - mh
    for r in range(mh):
        for c in range(mw):
            if pix[r][c]:
                rows[y0 + r][mx + c] = pix[r][c]
    return Glyph(rows, base.top + extra)


def compose(lettering, font, text):
    """a word's Glyph in a lettering (lines split by '\\n', centred), or the characters it lacks"""
    lines, missing = [], set()
    for line in text.split('\n'):
        parts = []
        for ch in line:
            if ch == ' ':
                parts.append(None)
                continue
            g = lettering.get(font, ch)
            if not g and not font[3]:
                g = lettering.get(font[:3] + (True,), ch)  # a letter only the narrow lettering has
            if not g:
                missing.add(ch)
            parts.append(g)
        lines.append(parts)
    if missing:
        return None, missing
    space = lettering.space(font)
    pictures = []
    for parts in lines:
        glyphs = [g for g in parts if g]
        top = max(g.top for g in glyphs)
        bottom = max(len(g.pixels) - g.top for g in glyphs)
        width = sum(g.width if g else space for g in parts)
        rows = [[0] * width for _ in range(top + bottom)]
        x = 0
        for g in parts:
            if not g:
                x += space
                continue
            for r, row in enumerate(g.pixels):
                for c, v in enumerate(row):
                    if v:
                        rows[top - g.top + r][x + c] = v
            x += g.width
        pictures.append((rows, top))
    if len(pictures) == 1:
        return Glyph(*pictures[0]), set()
    width = max(len(rows[0]) for rows, _ in pictures)
    out = []
    for rows, _ in pictures:
        pad = (width - len(rows[0])) // 2
        out += [[0] * pad + row + [0] * (width - pad - len(row)) for row in rows]
    return Glyph(out, pictures[0][1]), set()


def compose_fit(lettering, font, text, budget, compose_fn=None):
    """the word in the lettering, or in its narrow form when that keeps it within budget pixels and the usual one
    doesn't (and the narrow one has the letters)"""
    compose_fn = compose_fn or compose
    glyph, missing = compose_fn(lettering, font, text)
    if glyph is not None and glyph.width <= budget + 12:  # (a little over: still the usual lettering, it looks better)
        return glyph, missing
    narrow = font[:3] + (True,)
    other, other_missing = compose_fn(lettering, narrow, text)
    if other is not None and (glyph is None or other.width < glyph.width):
        return other, other_missing
    return glyph, missing


def read_animation_full(data):
    """(sheet names, [(name, speed, loop, rotation, [(sheet, x, y, w, h, px, py), ...]), ...])"""
    r = rsdk_pack.Reader(data)
    r.u32()
    r.u32()
    sheets = [r.string() for _ in range(r.u8())]
    hitboxes = [r.string() for _ in range(r.u8())]
    lists = []
    for _ in range(r.u16()):
        name = r.string()
        count = r.u16()
        speed, loop, rot = struct.unpack_from('<hBB', data, r.pos)
        r.pos += 4
        frames = []
        for _ in range(count):
            sheet = r.u8()
            r.pos += 4
            frames.append((sheet,) + struct.unpack_from('<4H2h', data, r.pos))
            r.pos += 12 + 8 * len(hitboxes)
        lists.append((name, speed, loop, rot, frames))
    return sheets, lists


def write_animation_full(sheets, lists):
    def string(text):
        raw = text.encode('latin1') + b'\0'
        return bytes([len(raw)]) + raw
    out = bytearray(b'SPR\0')
    out += struct.pack('<I', sum(len(l[4]) for l in lists))
    out += bytes([len(sheets)]) + b''.join(string(s) for s in sheets) + bytes([0])
    out += struct.pack('<H', len(lists))
    for name, speed, loop, rot, frames in lists:
        out += string(name) + struct.pack('<HhBB', len(frames), speed, loop, rot)
        for sheet, x, y, w, h, px, py in frames:
            out += struct.pack('<BhH4H2h', sheet, 1, 0, x, y, w, h, px, py)
    return bytes(out)


class Source:
    """a language's Text set: its frames"""

    def __init__(self, pack, lang):
        self.sheets, self.lists = read_animation_full(pack.read('Data/Sprites/UI/Text%s.bin' % lang))
        images = {n: Image.open(io.BytesIO(pack.read('Data/Sprites/' + n))) for n in self.sheets}
        self.frames = {}
        for l, (_, _, _, _, fl) in enumerate(self.lists):
            for f, rect in enumerate(fl):
                name = self.sheets[rect[0]]
                self.frames[(l, f)] = Frame(lang, l, f, images[name], rect, 'pause' if 'Pause' in name else 'menu')


class Packer:
    """pictures put on one sheet in shelves: side by side, each shelf as tall as its pictures (one shelf per height in
    use), so the sheet stays small (the game holds it in the stage's memory)"""

    def __init__(self, width=512):
        self.width, self.rows, self.y = width, [], 0
        self.shelves = {}  # height -> [x, y]

    def add(self, pixels):
        h, w = len(pixels), len(pixels[0])
        shelf = self.shelves.get(h)
        if shelf is None or shelf[0] + w > self.width:
            shelf = self.shelves[h] = [0, self.y]
            self.y += h + 1
        x, y = shelf
        shelf[0] += w + 1
        self.rows.append((x, y, pixels))
        return (x, y, w, h)

    def image(self, palette):
        sheet = Image.new('P', (self.width, max(16, (self.y + 15) & ~15)), 0)
        sheet.putpalette(palette)
        for x, y, pixels in self.rows:
            for r, row in enumerate(pixels):
                for c, v in enumerate(row):
                    if v:
                        sheet.putpixel((x + c, y + r), v)
        return sheet


def gif(image):
    out = io.BytesIO()
    image.save(out, 'GIF', optimize=False, transparency=0)
    return out.getvalue()


def first_column(pixels):
    cols = [c for c in range(len(pixels[0])) if any(row[c] for row in pixels)]
    return cols[0] if cols else 0


def place_x(src, pixels):
    """the new picture's pivot, placed as the English frame's: a gap after the point (the prompts, after their icon),
    centred, or ending at it - else at a fixed offset (the pause menu's items), where its letters start just where the
    English ones did"""
    w = len(pixels[0])
    if src.px >= 0:
        return src.px
    if abs(src.px + src.w / 2) <= 2:
        return -(w // 2)
    if abs(src.px + src.w) <= 2:
        return -w
    return src.px + first_column(src.pixels) - first_column(pixels)


def pivot_x(old_px, old_w, w):
    """where the new picture's left edge goes, as the old one's did: from the point (0 or a gap after it: kept), its
    right edge at the point, or centred"""
    if old_px >= 0:
        return old_px
    if old_px <= -old_w + 1:
        return -w
    return -(w // 2)


def build(pack, report=None):
    """{path in the pack: contents}: TextPT, HeadingsPT, StringsPT (and the language name for the other sets)"""
    sources = {lang: Source(pack, lang) for lang in ('EN', 'FR', 'IT', 'GE', 'SP')}
    lettering = Lettering()
    for (lang, l, f), text in KNOWN_TEXT.items():
        if not lettering.learn(sources[lang].frames[(l, f)], text) and report is not None:
            report.append('not learnt: %s %d.%d %s' % (lang, l, f, text))

    for (lang, l, f), texts in KNOWN_LINES.items():
        lines = frame_lines(sources[lang].frames[(l, f)])
        if len(lines) == len(texts):
            for frame, text in zip(lines, texts):
                if not lettering.learn(frame, text) and report is not None:
                    report.append('not learnt: %s %d.%d line "%s"' % (lang, l, f, text))

    en = sources['EN']
    packer = Packer()
    lists = []
    pictures = {}
    for l, (name, speed, loop, rot, frames) in enumerate(en.lists):
        out = []
        for f, rect in enumerate(frames):
            what = PT_TEXT.get((l, f))
            src = en.frames[(l, f)]
            if (l, f) in PT_BADGES:
                kind, source, source_text, word = PT_BADGES[(l, f)]
                if kind == 'box':
                    pix = box_word(sources[source[0]].frames[(source[1], source[2])], source_text, word)
                elif kind == 'lines':
                    pix = badge_lines(sources[source[0]].frames[(source[1], source[2])], lettering, word)
                else:
                    pix = badge_word(src, lettering, word)
                if pix is None:
                    if report is not None:
                        report.append('%d.%d badge "%s" not made, English kept' % (l, f, word))
                    out.append(rect)
                    continue
                x, y, w, h = packer.add(pix)
                if kind == 'lines':
                    # set where the badge it was made on goes (taller than the English one: it stands higher)
                    model = sources[source[0]].frames[(source[1], source[2])]
                    out.append((3, x, y, w, h, model.px, model.py))
                else:
                    out.append((3, x, y, w, h, place_x(src, pix), src.py))
                pictures[(l, f)] = pix
                continue
            if what is None:
                out.append(rect)
                continue
            if isinstance(what, tuple):
                other = sources[what[0]].frames[(what[1], what[2])]
                x, y, w, h = packer.add(other.pixels)
                out.append((3, x, y, w, h, place_x(src, other.pixels), other.py))
                pictures[(l, f)] = other.pixels
                continue
            font = src.font()
            # as wide as any language's picture there may go (a little more for the short ones)
            budget = max(max(s.frames[(l, f)].w for s in sources.values() if (l, f) in s.frames) + 4, 40)
            if font and (l, f) in PT_NARROW:
                glyph, missing = compose(lettering, font[:3] + (True,), what)
            else:
                glyph, missing = compose_fit(lettering, font, what, budget) if font else (None, {'(lettering)'})
            if glyph is not None and glyph.width > budget and report is not None:
                report.append('%d.%d "%s": %d pixels wide, %d at most there' % (l, f, what, glyph.width, budget))
            if glyph is None:
                if report is not None:
                    report.append('%d.%d "%s": no %s in %s, English kept' % (l, f, what, ''.join(sorted(missing)), font))
                out.append(rect)
                continue
            x, y, w, h = packer.add(glyph.pixels)
            out.append((3, x, y, w, h, place_x(src, glyph.pixels), src.py + src.baseline - glyph.top))
            pictures[(l, f)] = glyph.pixels
        if l == 15:
            # the language menu's own name
            glyph, missing = compose(lettering, en.frames[(15, 0)].font(), PT_NAME)
            if glyph:
                x, y, w, h = packer.add(glyph.pixels)
                first = en.frames[(15, 0)]
                out.append((3, x, y, w, h, place_x(first, glyph.pixels), first.py + first.baseline - glyph.top))
                pictures[(15, len(frames))] = glyph.pixels
        lists.append((name, speed, loop, rot, out))

    palette = Image.open(io.BytesIO(pack.read('Data/Sprites/' + en.sheets[0]))).getpalette()
    files = {
        'Data/Sprites/UI/TextPT.bin': write_animation_full(en.sheets + ['UI/TextPT.gif'], lists),
        'Data/Sprites/UI/TextPT.gif': gif(packer.image(palette)),
    }

    # the headings, in the yellow lettering
    yellow = {}
    heading_frames = {}
    for lang, texts in HEADING_TEXT.items():
        sheets, hl = psp_achievements.read_animation(pack.read('Data/Sprites/UI/Headings%s.bin' % lang))
        image = Image.open(io.BytesIO(pack.read('Data/Sprites/' + sheets[0])))
        for i, text in enumerate(texts):
            frame = hl[i][0]
            heading_frames[(lang, i)] = (image, frame)
            if not text:
                continue
            # the narrow lettering's letters (~10 pixels) are always apart; the usual ones may touch
            _, x, y, w, h, px, py = frame
            used = [any(image.getpixel((x + c, y + r)) for r in range(h)) for c in range(w)]
            widths, c = [], 0
            while c < w:
                if used[c]:
                    st = c
                    while c < w and used[c]:
                        c += 1
                    widths.append(c - st)
                else:
                    c += 1
            narrow = sorted(widths)[len(widths) // 2] <= 12
            if narrow:
                cut = psp_achievements.cut_letters(image, frame, text, gap=True) if len(widths) == len(text.replace(' ', '')) else None
            else:
                cut = psp_achievements.cut_letters(image, frame, text, gap=True)
            if cut is None:
                if report is not None:
                    report.append('heading not learnt: %s %d %s' % (lang, i, text))
                continue
            for ch, g in cut.items():
                yellow.setdefault((('heading', 32, 'menu', narrow), ch), Glyph(g.pixels, g.top))
    heads = Lettering()
    heads.glyphs = yellow
    for narrow in (False, True):
        u = yellow.get((('heading', 32, 'menu', narrow), 'U'))
        if u and (('heading', 32, 'menu', narrow), 'V') not in yellow:
            v = psp_achievements.v_from_u(psp_achievements.Glyph(u.pixels, u.top))
            yellow[(('heading', 32, 'menu', narrow), 'V')] = Glyph(v.pixels, v.top)
    hfont = ('heading', 32, 'menu', False)
    hpacker = Packer()
    hlists = []
    hpalette = None
    for i, what in enumerate(PT_HEADINGS):
        if isinstance(what, tuple):
            image, (s, x, y, w, h, px, py) = heading_frames.get(what, (None, None)) if what in heading_frames else (None, None)
            if image is None:
                sheets, hl = psp_achievements.read_animation(pack.read('Data/Sprites/UI/Headings%s.bin' % what[0]))
                image = Image.open(io.BytesIO(pack.read('Data/Sprites/' + sheets[0])))
                s, x, y, w, h, px, py = hl[what[1]][0]
            pix = [[image.getpixel((x + c, y + r)) for c in range(w)] for r in range(h)]
            rx, ry, rw, rh = hpacker.add(pix)
            hlists.append(('Heading %d' % i, 0, 0, 0, [(0, rx, ry, rw, rh, px, py)]))
            hpalette = hpalette or image.getpalette()
            continue
        glyph, missing = compose_fit(heads, hfont, what, HEADING_WIDTH, compose_heading)
        if glyph is None:
            if report is not None:
                report.append('heading %d "%s": no %s' % (i, what, ''.join(sorted(missing))))
            image, (s, x, y, w, h, px, py) = heading_frames[('EN', i)]
            pix = [[image.getpixel((x + c, y + r)) for c in range(w)] for r in range(h)]
            rx, ry, rw, rh = hpacker.add(pix)
            hlists.append(('Heading %d' % i, 0, 0, 0, [(0, rx, ry, rw, rh, px, py)]))
            continue
        rx, ry, rw, rh = hpacker.add(glyph.pixels)
        hlists.append(('Heading %d' % i, 0, 0, 0, [(0, rx, ry, rw, rh, -(rw // 2), -glyph.top)]))
    hpalette = Image.open(io.BytesIO(pack.read('Data/Sprites/UI/HeadingsEN.gif'))).getpalette()
    files['Data/Sprites/UI/HeadingsPT.bin'] = write_animation_full(['UI/HeadingsPT.gif'], hlists)
    files['Data/Sprites/UI/HeadingsPT.gif'] = gif(hpacker.image(hpalette))

    files.update(build_press(pack, report))

    text = open(os.path.join(HERE, 'lang', 'StringsPT.txt'), encoding='utf-8').read().splitlines()
    files['Data/Strings/StringsPT.txt'] = '﻿'.encode('utf-16-le') + '\r\n'.join(text).encode('utf-16-le') + b'\r\0\n\0'
    return files, pictures, packer, hpacker, palette, hpalette


def box_word(frame, text, word):
    """a word on one of the save slots' light boxes, in the box's own lettering: the letters are cut from another
    language's box (they stand apart), and the word is set on that box, centred (the box grown when it needs to)"""
    pix, h, w = frame.pixels, frame.h, frame.w
    counts = {}
    for row in pix:
        for v in row:
            counts[v] = counts.get(v, 0) + 1
    bg = max(counts, key=counts.get)
    body = [any(pix[r][c] != bg for r in range(h)) for c in range(w)]
    runs, c = [], 0
    while c < w:
        if body[c]:
            st = c
            while c < w and body[c]:
                c += 1
            runs.append((st, c))
        else:
            c += 1
    letters = [ch for ch in text if ch != ' ']
    # letters that touch: the widest run is split where it is thinnest, until there are as many as letters
    while len(runs) < len(letters):
        i = max(range(len(runs)), key=lambda k: runs[k][1] - runs[k][0])
        a, b = runs[i]
        if b - a < 8:
            return None
        singles = sorted(y - x for x, y in runs if y - x < 14)
        usual = singles[len(singles) // 2] if singles else 8
        near = range(max(a + 3, a + usual - 2), min(b - 3, a + usual + 3))
        cut = min(near or range(a + 3, b - 3), key=lambda k: sum(pix[r][k] != bg for r in range(h)))
        runs[i:i + 1] = [(a, cut), (cut + 1, b)]  # (the column where they touched: neither's)
    if len(runs) != len(letters):
        return None
    glyphs = {}
    for ch, (a, b) in zip(letters, runs):
        glyphs.setdefault(ch, [row[a:b] for row in pix])
    if any(ch not in glyphs for ch in word):
        return None
    gaps = sorted(runs[i + 1][0] - runs[i][1] for i in range(len(runs) - 1))
    gap = gaps[len(gaps) // 2] if gaps else 1
    width = sum(len(glyphs[ch][0]) for ch in word) + gap * (len(word) - 1)
    new_w = max(w, width + runs[0][0] + (w - runs[-1][1]))
    out = [[bg] * new_w for _ in range(h)]
    x = (new_w - width) // 2
    for ch in word:
        g = glyphs[ch]
        for r in range(h):
            for c2, v in enumerate(g[r]):
                if v != bg:
                    out[r][x + c2] = v
        x += len(g[0]) + gap
    return out


def badge_word(frame, lettering, word):
    """the no-save badge: its slanted end kept, a word in the small white letters on the dark rest (grown to fit)"""
    pix, h, w = frame.pixels, frame.h, frame.w
    start = min(c for c in range(w) if any(pix[r][c] == 185 for r in range(h)))  # where the English letters begin
    ends = [r for r in range(h) if any(v in (185, 178) for v in pix[r][start:])]
    baseline = ends[-1] + 1
    glyph, missing = compose(lettering, ('shadow', 11, 'menu', False), word)
    if glyph is None:
        return None
    fill = [pix[r][w - 2] for r in range(h)]  # the badge's own colours down a column without letters
    new_w = start + glyph.width + 5
    out = [list(pix[r][:start]) + [fill[r]] * (new_w - start) for r in range(h)]
    y0 = baseline - glyph.top
    for r, row in enumerate(glyph.pixels):
        for c, v in enumerate(row):
            if v and 0 <= y0 + r < h:
                out[y0 + r][start + c] = v
    return out


def badge_lines(frame, lettering, words):
    """the no-save badge in two lines, on another language's two-line badge: its letters cleared to the badge's dark,
    each word set where that line was (right-aligned, on its baseline)"""
    pix, h, w = [list(r) for r in frame.pixels], frame.h, frame.w
    letter_rows = [r for r in range(h) if 185 in pix[r]]  # (the badge's slanted edge is the shadows' colour too)
    bands, prev = [], None
    for r in letter_rows:
        if prev is None or r > prev + 1:
            bands.append([r, r])
        bands[-1][1] = r
        prev = r
    if len(bands) != len(words):
        return None
    fill = [pix[r][w - 2] for r in range(h)]
    lines = []
    for (a, b), word in zip(bands, words):
        cols = [c for c in range(w) if any(pix[r][c] == 185 for r in range(a, b + 1))]
        lines.append((cols[0], cols[-1] + 2, b + 2))  # (with their shadow: a column right, a row below)
        for r in range(a, min(h, b + 2)):
            for c in range(cols[0], min(w, cols[-1] + 2)):
                if pix[r][c] in (185, 178):
                    pix[r][c] = fill[r]
    font = ('shadow', 11, 'menu', False)
    for (left, right, baseline), word in zip(lines, words):
        glyph, missing = compose(lettering, font, word)
        if glyph is None:
            return None
        x0, y0 = right - glyph.width, baseline - glyph.top
        if x0 < 0:
            return None
        for r, row in enumerate(glyph.pixels):
            for c, v in enumerate(row):
                if v and 0 <= y0 + r < h:
                    pix[y0 + r][x0 + c] = v
    return pix


def build_press(pack, report=None):
    """Title/PSPPressPT: the title screen's "press any button" in Portuguese, in its lettering"""
    sheets, lists = read_animation_full(pack.read('Data/Sprites/Title/Logo.bin'))
    image = Image.open(io.BytesIO(pack.read('Data/Sprites/' + sheets[0])))
    frames = lists[8][4]
    title = Lettering()
    for i, text in enumerate(PRESS_TEXT):
        fr = Frame('TITLE', 8, i, image, frames[i], 'title')
        if not title.learn(fr, text) and report is not None:
            report.append('title not learnt: %d %s' % (i, text))
    src = Frame('TITLE', 8, 0, image, frames[0], 'title')
    glyph, missing = compose(title, src.font(), PT_PRESS)
    if glyph is None:
        if report is not None:
            report.append('title "%s": no %s' % (PT_PRESS, ''.join(sorted(missing))))
        return {}
    packer = Packer(256)
    x, y, w, h = packer.add(glyph.pixels)
    anim = write_animation_full(['Title/PSPPressPT.gif'],
                                [('Press Button', 0, 0, 0, [(0, x, y, w, h, place_x(src, glyph.pixels), src.py + src.baseline - glyph.top)])])
    return {'Data/Sprites/Title/PSPPressPT.bin': anim, 'Data/Sprites/Title/PSPPressPT.gif': gif(packer.image(image.getpalette()))}


def compose_heading(heads, font, text):
    """as compose, with the headings' 1-column gap between letters"""
    parts, missing = [], set()
    for ch in text:
        if ch == ' ':
            parts.append(None)
            continue
        g = heads.get(font, ch)
        if not g:
            missing.add(ch)
        parts.append(g)
    if missing:
        return None, missing
    glyphs = [g for g in parts if g]
    top = max(g.top for g in glyphs)
    bottom = max(len(g.pixels) - g.top for g in glyphs)
    space = 5 if font[3] else 8
    width = sum((g.width + 1) if g else space for g in parts) - 1
    rows = [[0] * width for _ in range(top + bottom)]
    x = 0
    for g in parts:
        if not g:
            x += space
            continue
        for r, row in enumerate(g.pixels):
            for c, v in enumerate(row):
                if v:
                    rows[top - g.top + r][x + c] = v
        x += g.width + 1
    return Glyph(rows, top), set()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rsdk')
    ap.add_argument('psp', nargs='?')
    ap.add_argument('--preview')
    args = ap.parse_args()
    pack = rsdk_pack.Pack(args.rsdk)
    report = []
    files, pictures, packer, hpacker, palette, hpalette = build(pack, report)
    print('\n'.join(report))
    if args.preview:
        a = packer.image(palette).convert('RGBA')
        b = hpacker.image(hpalette).convert('RGBA')
        out = Image.new('RGBA', (a.width + b.width + 8, max(a.height, b.height)), (40, 40, 120, 255))
        out.paste(a, (0, 0), a)
        out.paste(b, (a.width + 8, 0), b)
        out.save(args.preview)
    if args.psp:
        import build_psp_data
        psp_achievements.update_pack(args.psp, files,
                                     lambda data: (build_psp_data.gif_to_sheet(data) or data) if data[:3] == b'GIF' else data)
        print('added the Portuguese sets to %s' % args.psp)


if __name__ == '__main__':
    main()
