"""Makes the program icon and the installer wizard images from icon.png.

    python tools/make_artwork.py

Needs Python 3 with Pillow (pip install pillow). Run it after changing icon.png, then rebuild
with build.bat. It writes:

    src/res/ps3eye.ico        PS3EyeSetup.exe, PS3EyeServer.exe and the installer (16-256 px)
    installer/wizard/*.bmp    installer wizard: large = Welcome/Finish pages, small = page
                              headers, each at 100/150/200% screen scaling

icon.png is a photo of the camera on a plain white background; the white is cut away so the
artwork works on light and dark backgrounds.
"""
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "icon.png"
ICON = ROOT / "src" / "res" / "ps3eye.ico"
WIZARD = ROOT / "installer" / "wizard"

ICON_SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
# Inno Setup's sizes for 100%, 150% and 200% scaling: (percent, large image, small image)
WIZARD_SIZES = [(100, (164, 314), (55, 55)), (150, (246, 459), (83, 80)), (200, (328, 604), (110, 106))]


def cutout(path):
    """The camera on a transparent background, cropped to its outline."""
    img = Image.open(path).convert("RGB")
    w, h = img.size

    # background = near-white pixels connected to the border
    marker = (255, 0, 255)
    filled = img.copy()
    for x in range(0, w, 7):
        for y in (0, h - 1):
            if min(filled.getpixel((x, y))) > 225:
                ImageDraw.floodfill(filled, (x, y), marker, thresh=40)
    for y in range(0, h, 7):
        for x in (0, w - 1):
            if min(filled.getpixel((x, y))) > 225:
                ImageDraw.floodfill(filled, (x, y), marker, thresh=40)
    bg = Image.eval(ImageChops.difference(filled, Image.new("RGB", (w, h), marker)).convert("L"),
                    lambda v: 255 if v == 0 else 0)

    # in a thin band along the outline, estimate how much of each pixel is camera (dark) and
    # how much is white, then take the white out of its colour: clean edges, no white fringe
    band = ImageChops.subtract(bg.filter(ImageFilter.MaxFilter(5)), bg)
    grey = img.convert("L")
    rgba = Image.new("RGBA", (w, h))
    src_px, bg_px, band_px, grey_px, out = img.load(), bg.load(), band.load(), grey.load(), rgba.load()
    for y in range(h):
        for x in range(w):
            r, g, b = src_px[x, y]
            if bg_px[x, y]:
                out[x, y] = (0, 0, 0, 0)
            elif band_px[x, y]:
                a = max(0.0, min(1.0, (255 - grey_px[x, y]) / 225.0))
                if a < 0.02:
                    out[x, y] = (0, 0, 0, 0)
                else:
                    unmix = lambda c: max(0, min(255, int(round((c - (1 - a) * 255) / a))))
                    out[x, y] = (unmix(r), unmix(g), unmix(b), int(round(a * 255)))
            else:
                out[x, y] = (r, g, b, 255)

    box = rgba.getchannel("A").point(lambda v: 255 if v > 8 else 0).getbbox()
    return rgba.crop(box)


def fit(img, width, height):
    scale = min(width / img.width, height / img.height)
    return img.resize((max(1, round(img.width * scale)), max(1, round(img.height * scale))), Image.LANCZOS)


def make_icon(cam):
    side = int(max(cam.size) * 1.04)
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    square.paste(cam, ((side - cam.width) // 2, (side - cam.height) // 2), cam)
    frames = [square.resize((s, s), Image.LANCZOS) for s in ICON_SIZES]
    frames[-1].save(ICON, format="ICO", sizes=[(s, s) for s in ICON_SIZES], append_images=frames[:-1])


def wizard_large(cam, width, height):
    """Light grey gradient panel (the camera is black), camera low-centre on a faint shadow."""
    panel = Image.new("RGBA", (width, height))
    top, bottom = (246, 247, 250), (214, 219, 227)
    px = panel.load()
    for y in range(height):
        t = y / (height - 1)
        colour = tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
        for x in range(width):
            px[x, y] = colour
    c = fit(cam, width * 0.86, height * 0.5)
    cx, cy = (width - c.width) // 2, int(height * 0.56) - c.height // 2
    shadow = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    sw, sh = int(c.width * 0.8), max(4, int(c.height * 0.08))
    ImageDraw.Draw(shadow).ellipse(
        ((width - sw) // 2, cy + c.height - sh // 2, (width + sw) // 2, cy + c.height + sh // 2), fill=(20, 24, 32, 90))
    panel.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(max(2, width / 40))))
    panel.alpha_composite(c, (cx, cy))
    return panel


def wizard_small(cam, width, height):
    """The camera on transparency, so it sits on the page header in any colour scheme."""
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    c = fit(cam, width * 0.96, height * 0.96)
    canvas.alpha_composite(c, ((width - c.width) // 2, (height - c.height) // 2))
    return canvas


def main():
    cam = cutout(SOURCE)
    ICON.parent.mkdir(parents=True, exist_ok=True)
    WIZARD.mkdir(parents=True, exist_ok=True)
    make_icon(cam)
    print(f"wrote {ICON.relative_to(ROOT)}")
    # 32-bit BMPs with alpha (WizardImageAlphaFormat=defined): BMP works with Inno Setup 6 and 7
    for pct, (lw, lh), (sw, sh) in WIZARD_SIZES:
        for name, image in ((f"large-{pct}.bmp", wizard_large(cam, lw, lh)), (f"small-{pct}.bmp", wizard_small(cam, sw, sh))):
            image.save(WIZARD / name)
            print(f"wrote {(WIZARD / name).relative_to(ROOT)}")


if __name__ == "__main__":
    main()
