"""24×12・24bit BMPの国旗風ドット絵と、GUI用カタログを再生成する。標準ライブラリのみ。"""
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parent
W, H = 24, 12
WHITE, RED, BLUE = 0xFFFFFF, 0xCE1126, 0x003B7A
GREEN, YELLOW, BLACK = 0x008C45, 0xFFCC00, 0x111111
CATALOG = [
    ('jp', '日本', 'Japan'), ('us', 'アメリカ', 'United States'),
    ('gb', 'イギリス', 'United Kingdom'), ('ca', 'カナダ', 'Canada'),
    ('br', 'ブラジル', 'Brazil'), ('ar', 'アルゼンチン', 'Argentina'),
    ('fr', 'フランス', 'France'), ('de', 'ドイツ', 'Germany'),
    ('it', 'イタリア', 'Italy'), ('es', 'スペイン', 'Spain'),
    ('pt', 'ポルトガル', 'Portugal'), ('nl', 'オランダ', 'Netherlands'),
    ('be', 'ベルギー', 'Belgium'), ('ie', 'アイルランド', 'Ireland'),
    ('se', 'スウェーデン', 'Sweden'), ('no', 'ノルウェー', 'Norway'),
    ('dk', 'デンマーク', 'Denmark'), ('fi', 'フィンランド', 'Finland'),
    ('ch', 'スイス', 'Switzerland'), ('pl', 'ポーランド', 'Poland'),
    ('ua', 'ウクライナ', 'Ukraine'), ('at', 'オーストリア', 'Austria'),
    ('cn', '中国', 'China'), ('kr', '韓国', 'South Korea'),
]


def flag(code):
    pixels = [[WHITE] * W for _ in range(H)]

    def paint(test, color):
        for y in range(H):
            for x in range(W):
                if test(x, y):
                    pixels[y][x] = color

    def fill(color):
        paint(lambda x, y: True, color)

    def disk(cx, cy, radius, color):
        paint(lambda x, y: (x - cx)**2 + (y - cy)**2 <= radius**2, color)

    def bands(colors, vertical=False):
        for y in range(H):
            for x in range(W):
                pixels[y][x] = colors[(x * len(colors) // W) if vertical else (y * len(colors) // H)]

    def pattern(rows, x0, y0, color):
        for y, row in enumerate(rows):
            for x, value in enumerate(row):
                if value == '#':
                    pixels[y0+y][x0+x] = color

    if code == 'jp':
        disk(11.5, 5.5, 3.2, 0xBC002D)
    elif code == 'us':
        bands([RED if i % 2 == 0 else WHITE for i in range(13)])
        paint(lambda x, y: x < 10 and y < 7, BLUE)
        paint(lambda x, y: x < 9 and y < 6 and x % 2 == 1 and y % 2 == 1, WHITE)
    elif code == 'gb':
        fill(BLUE)
        paint(lambda x, y: abs(x - 2*y) < 3 or abs(x + 2*y - 23) < 3, WHITE)
        paint(lambda x, y: x == 2*y or x + 2*y == 23, RED)
        paint(lambda x, y: 9 <= x <= 14 or 4 <= y <= 7, WHITE)
        paint(lambda x, y: 11 <= x <= 12 or 5 <= y <= 6, RED)
    elif code == 'ca':
        paint(lambda x, y: x < 6 or x >= 18, RED)
        pattern(['....#....', '...###...', '.#.###.#.', '.#######.', '..#####..', '.#######.', '....#....', '....#....'], 7, 2, RED)
    elif code == 'br':
        fill(GREEN)
        paint(lambda x, y: abs(x - 11.5)/10 + abs(y - 5.5)/5 <= 1, YELLOW)
        disk(11.5, 5.5, 3, BLUE)
        paint(lambda x, y: (x - 11.5)**2 + (y - 5.5)**2 <= 9 and y == 5 + (x-9)//4, WHITE)
    elif code == 'ar':
        bands([0x74ACDF, WHITE, 0x74ACDF]); disk(11.5, 5.5, 1.5, YELLOW)
    elif code in ('fr', 'it', 'be', 'ie'):
        bands({'fr': [BLUE, WHITE, RED], 'it': [GREEN, WHITE, RED],
               'be': [BLACK, YELLOW, RED], 'ie': [GREEN, WHITE, 0xFF883E]}[code], True)
    elif code in ('de', 'nl', 'at', 'pl', 'ua'):
        bands({'de': [BLACK, RED, YELLOW], 'nl': [RED, WHITE, BLUE], 'at': [RED, WHITE, RED],
               'pl': [WHITE, RED], 'ua': [0x0057B7, YELLOW]}[code])
    elif code == 'es':
        bands([RED, YELLOW, YELLOW, RED])
        pattern(['.##.', '####', '#..#', '#..#', '.##.'], 6, 4, RED)
    elif code == 'pt':
        fill(RED); paint(lambda x, y: x < 9, GREEN); disk(8.5, 5.5, 2.7, YELLOW)
        pattern(['###', '#.#', '#.#', '.#.'], 8, 4, WHITE)
    elif code in ('se', 'no', 'dk', 'fi'):
        bg, cross = {'se': (0x006AA7, YELLOW), 'no': (RED, WHITE),
                     'dk': (RED, WHITE), 'fi': (WHITE, BLUE)}[code]
        fill(bg); paint(lambda x, y: 7 <= x <= 9 or 5 <= y <= 6, cross)
        if code == 'no':
            paint(lambda x, y: x == 8 or y == 5, BLUE)
    elif code == 'ch':
        fill(RED); paint(lambda x, y: (10 <= x <= 13 and 2 <= y <= 9) or (8 <= x <= 15 and 4 <= y <= 7), WHITE)
    elif code == 'cn':
        fill(RED)
        pattern(['..#..', '#####', '.###.', '.#.#.'], 2, 2, YELLOW)
        for x, y in [(8, 1), (10, 3), (10, 5), (8, 7)]:
            pixels[y][x] = YELLOW
    elif code == 'kr':
        disk(11.5, 5.5, 3, RED)
        paint(lambda x, y: (x-11.5)**2+(y-5.5)**2 <= 9 and y >= 6, BLUE)
        disk(10, 5.5, 1.2, RED); disk(13, 5.5, 1.2, BLUE)
        for x0, y0, rows in [(3, 1, ['###', '###', '###']), (18, 1, ['#.#', '###', '#.#']),
                             (3, 8, ['###', '#.#', '###']), (18, 8, ['#.#', '#.#', '#.#'])]:
            pattern(rows, x0, y0, BLACK)
    return pixels


def bitmap(pixels):
    data = bytearray(b'BM')
    data += struct.pack('<IHHI', 918, 0, 0, 54)
    data += struct.pack('<IiiHHIIiiII', 40, W, H, 1, 24, 0, 864, 0, 0, 0, 0)
    for row in reversed(pixels):
        for color in row:
            data.extend((color & 255, (color >> 8) & 255, (color >> 16) & 255))
    return data


if __name__ == '__main__':
    for code, _, _ in CATALOG:
        (ROOT / f'{code}.bmp').write_bytes(bitmap(flag(code)))
    entries = '\n'.join(f'    {{"{code}", "{ja}", "{en}"}},' for code, ja, en in CATALOG)
    (ROOT.parent.parent / 'EmblemCatalog.hpp').write_text(
        '#pragma once\n// assets/emblems/generate.py から生成。\n#include <string_view>\n'
        'namespace cccaster::emblem {\n'
        'struct Preset { const char* code; const char* japanese; const char* english; };\n'
        'inline constexpr Preset Presets[]{\n' + entries + '\n};\n'
        'inline const Preset* FindPreset(std::string_view code) {\n'
        '    for (const auto& preset : Presets) if (preset.code == code) return &preset;\n'
        '    return nullptr;\n}\n}\n', encoding='utf-8')
