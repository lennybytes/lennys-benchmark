#!/usr/bin/env python3
"""Generate the lennys-benchmark application icon (PNG) used by the .deb and AppImage.

Reproducible: just run `python3 packaging/make-icon.py`. Requires Pillow.
"""
import os

from PIL import Image, ImageDraw, ImageFont

SIZE = 256
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icon", "lennys-benchmark.png")

BG = (15, 22, 32, 255)
BORDER = (0, 190, 214, 255)
BAR_COLORS = [
    (0, 190, 214, 255),
    (0, 190, 214, 255),
    (47, 208, 122, 255),
    (255, 205, 86, 255),
]
BAR_HEIGHTS = [0.40, 0.60, 0.80, 1.00]


def load_font(size: int):
    for path in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    ):
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def main() -> None:
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # Rounded background with a cyan border.
    d.rounded_rectangle([8, 8, SIZE - 8, SIZE - 8], radius=52, fill=BG,
                        outline=BORDER, width=5)

    # Baseline.
    base_y = 196
    d.line([52, base_y, SIZE - 52, base_y], fill=(60, 78, 98, 255), width=3)

    # Ascending performance bars.
    x0, bar_w, gap = 58, 26, 20
    for i, (frac, color) in enumerate(zip(BAR_HEIGHTS, BAR_COLORS)):
        x = x0 + i * (bar_w + gap)
        top = base_y - int(frac * 118)
        d.rounded_rectangle([x, top, x + bar_w, base_y - 4], radius=6, fill=color)

    # "PK" wordmark.
    font = load_font(40)
    text = "PK"
    bbox = d.textbbox((0, 0), text, font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    d.text(((SIZE - tw) / 2 - bbox[0], 34 - bbox[1]), text, font=font,
           fill=(235, 245, 250, 255))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    img.save(OUT)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
