"""A contact sheet of the HUD button pictures that hud-icons-preview wrote (gow3_pc/tests/
hud_icons_preview.cpp): a row per picture, the game's own (PlayStation), keyboard and mouse, and
Xbox, enlarged and at their size on a 1080p screen, over a dark scene colour.

    py tools/hud_icons_sheet.py <preview folder> <sheet.png> [u1 u2 u3]
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ORDER = ['triangle-button-color', 'circle-button-color', 'square-button-color', 'x-button-color',
         'triangle-button-up', 'triangle-button-down', 'circle-button-up', 'circle-button-down',
         'square-button-up', 'square-button-down', 'l1-button', 'r1-button', 'l2-button', 'r2-button',
         'up-button', 'up-button-look-color', 'bp-up-button']
STYLES = [('ps', 'PlayStation (game)'), ('keyboard', 'Keyboard and mouse'), ('xbox', 'Xbox')]
SCENE = (52, 46, 40)
ZOOM = 0.75          # texels of a 256 picture per sheet pixel; a 512 picture at half
SCREEN = 0.53        # a 256 picture on a 1080p screen (the face button's dark disc is ~72 px)


def font(size):
    for name in ('segoeui.ttf', 'arial.ttf', 'DejaVuSans.ttf'):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def over_scene(picture, scale):
    """The picture (its mask as alpha) over the scene colour, scaled."""
    base = Image.new('RGBA', picture.size, SCENE + (255,))
    shown = Image.alpha_composite(base, picture).convert('RGB')
    w, h = max(1, round(picture.width * scale)), max(1, round(picture.height * scale))
    return shown.resize((w, h), Image.LANCZOS)


def main():
    folder, out = Path(sys.argv[1]), Path(sys.argv[2])
    games = sys.argv[3:] or ['u1', 'u2', 'u3']
    rows = []
    for game in games:
        for name in ORDER:
            pictures = {s: folder / f'{game}-{name}-{s}.png' for s, _ in STYLES}
            if all(p.exists() for p in pictures.values()):
                rows.append((game, name, {s: Image.open(p).convert('RGBA') for s, p in pictures.items()}))
    if not rows:
        sys.exit(f'no pictures in {folder}')
    label_font, head_font = font(15), font(17)
    cells = []
    for game, name, pictures in rows:
        ps = pictures['ps']
        box = ps.split()[3].point(lambda a: 255 if a > 10 else 0).getbbox() or (0, 0, ps.width, ps.height)
        margin = ps.width // 32
        box = (max(0, box[0] - margin), max(0, box[1] - margin), min(ps.width, box[2] + margin), min(ps.height, box[3] + margin))
        factor = 256 / ps.width  # 512 pictures are drawn at the size of the 256 ones
        big = [over_scene(pictures[s].crop(box), ZOOM * factor) for s, _ in STYLES]
        small = [over_scene(pictures[s].crop(box), SCREEN * factor) for s, _ in STYLES]
        cells.append((game, name, big, small))
    label_w = 230
    big_w = max(c[2][0].width for c in cells)
    small_w = max(c[3][0].width for c in cells)
    gap = 14
    width = label_w + 3 * (big_w + gap) + 3 * (small_w + gap) + gap
    heights = [max(c[2][0].height, 40) + gap for c in cells]
    header = 56
    sheet = Image.new('RGB', (width, header + sum(heights)), (30, 28, 26))
    draw = ImageDraw.Draw(sheet)
    x = label_w
    for _, title in STYLES:
        draw.text((x + 4, 8), title, fill=(235, 228, 214), font=head_font)
        x += big_w + gap
    draw.text((x + 4, 8), 'At 1080p size', fill=(235, 228, 214), font=head_font)
    draw.text((x + 4, 30), 'PlayStation, keyboard, Xbox', fill=(170, 164, 152), font=label_font)
    y = header
    previous = None
    for (game, name, big, small), height in zip(cells, heights):
        if game != previous:
            draw.line((0, y - 4, width, y - 4), fill=(90, 84, 76))
            previous = game
        draw.text((12, y + 6), {'u1': 'Uncharted 1', 'u2': 'Uncharted 2', 'u3': 'Uncharted 3'}[game],
                  fill=(170, 164, 152), font=label_font)
        draw.text((12, y + 26), name, fill=(235, 228, 214), font=label_font)
        x = label_w
        for image in big:
            sheet.paste(image, (x, y))
            x += big_w + gap
        for image in small:
            sheet.paste(image, (x, y))
            x += small_w + gap
        y += height
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
    print(f'{len(cells)} pictures, {sheet.width}x{sheet.height}: {out}')


if __name__ == '__main__':
    main()
