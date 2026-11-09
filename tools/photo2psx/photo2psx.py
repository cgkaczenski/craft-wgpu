#!/usr/bin/env python3
"""Turns photos into PlayStation-looking block textures, as a texture pack.

Each photo is named after the block texture it replaces: stone.jpg becomes
blocks/stone.png in resources/texturePacks/<pack>. Blocks the pack does not
name keep the game's own textures, so a pack can start with a few photos.

    python3 -m venv .venv
    .venv/bin/pip install -r requirements.txt
    .venv/bin/python photo2psx.py photos/ --pack psxPhotos

Writing a pack also writes preview.png beside this script: the photo, what is
left once its lighting is gone and its edges meet, the final texture, and the
texture tiled, so settings can be judged without starting the game.

Then pick the pack in the game under Settings > Rendering > Textures Packs. The
first pack in the right-hand list wins a block, and "ourcraft" starts there and
has stone, dirt, bricks and others of its own. So remove ourcraft, add this
pack, then add ourcraft back, which leaves this pack first.

A photo goes through, in order:
  1. a centre square crop (--crop takes less of it);
  2. removing its lighting: the divide by a heavily blurred copy, so the
     photo's shadows do not fight the game's sun;
  3. making it tile, by cross-fading its far edges over its near ones;
  4. shrinking to the texture's size (--span: a picture across N x N blocks,
     cut into one texture per block by the game);
  5. a small palette (snapped to the PS1's 15-bit colour) with ordered dither.

Each texture gets a flat normal map and no specular or bump, because without
them the game builds normal and bump maps from the texture's brightness, which
reads a dithered photo as noise. --generated-normals lets it, to compare.

Leaves, glass and animated blocks are not handled: the alpha is always opaque
and a texture is one frame.
"""
import argparse
import difflib
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageOps

TOOL_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOL_DIR.parents[1]
PHOTO_EXTENSIONS = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff"}

# The array layers are 128x128 and a smaller image is enlarged by whole texels,
# so only these sizes stay exact. All are multiples of the dither's 4x4 too,
# which is what lets the dither tile.
TEXTURE_SIZES = [16, 32, 64, 128]

LUMA = np.array([0.2126, 0.7152, 0.0722])

BAYER_4 = (np.array([
    [0, 8, 2, 10],
    [12, 4, 14, 6],
    [3, 11, 1, 9],
    [15, 7, 13, 5],
]) + 0.5) / 16.0 - 0.5


def to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def gaussian_blur(image, sigma):
    """A 2D array, blurred. Mirrored at the edges (doubling it makes it
    periodic, which is what the FFT assumes), so an edge is not darkened."""
    height, width = image.shape
    mirrored = np.block([[image, image[:, ::-1]], [image[::-1], image[::-1, ::-1]]])
    fy = np.fft.fftfreq(2 * height)[:, None]
    fx = np.fft.rfftfreq(2 * width)[None, :]
    falloff = np.exp(-2 * (np.pi * sigma) ** 2 * (fx ** 2 + fy ** 2))
    blurred = np.fft.irfft2(np.fft.rfft2(mirrored) * falloff, s=mirrored.shape)
    return blurred[:height, :width]


def remove_lighting(linear, strength, radius):
    """Divides out the picture's slow change in brightness, keeping its average
    and its fine detail. `radius` is a fraction of the picture's width."""
    luma = (linear * LUMA).sum(axis=-1)
    slow = np.maximum(gaussian_blur(luma, radius * luma.shape[1]), 1e-4)
    gain = (luma.mean() / slow) ** strength
    return linear * gain[..., None]


def cross_fade_axis(image, margin, axis):
    """Removes `margin` from the far end of `axis`, fading it over the near
    end: the first kept line is then the one just past the last kept line.

    The weights are normalised by their length, because a plain mix of two
    unrelated textures averages their detail away and leaves a soft band."""
    image = np.moveaxis(image, axis, 0)
    kept = image[:-margin].copy()
    near, far = image[:margin], image[-margin:]
    mean = image.mean(axis=(0, 1))
    t = (np.arange(margin) / margin).reshape((margin, 1, 1))
    mixed = t * near + (1 - t) * far - mean
    kept[:margin] = np.maximum(mean + mixed / np.sqrt(t ** 2 + (1 - t) ** 2), 0.0)
    return np.moveaxis(kept, 0, axis)


def make_tileable(linear, seam):
    margin = int(round(linear.shape[0] * seam))
    if margin < 1:
        return linear
    return cross_fade_axis(cross_fade_axis(linear, margin, 0), margin, 1)


def shrink(linear, size):
    """Area averaging, channel by channel. BOX reads nothing past the picture's
    edges, so a tileable picture stays tileable, where a wider kernel would
    clamp at them. In linear light, so bright and dark average correctly."""
    channels = [Image.fromarray(linear[..., i].astype(np.float32)).resize((size, size), Image.BOX)
                for i in range(3)]
    return np.stack([np.asarray(c) for c in channels], axis=-1)


def grade(srgb, contrast, saturation):
    luma = (srgb * LUMA).sum(axis=-1)
    srgb = srgb.mean(axis=(0, 1)) + (srgb - srgb.mean(axis=(0, 1))) * contrast
    return np.clip(luma[..., None] + (srgb - luma[..., None]) * saturation, 0.0, 1.0)


def snap_to_15_bit(colours):
    fives = colours >> 3
    return (fives << 3) | (fives >> 2)


def make_palette(srgb8, colours):
    picture = Image.fromarray(srgb8)
    quantized = picture.quantize(colors=colours, method=Image.Quantize.MEDIANCUT, kmeans=4,
                                 dither=Image.Dither.NONE)
    table = np.array(quantized.getpalette()[:3 * colours], dtype=np.uint8).reshape(-1, 3)
    used = table[np.unique(np.asarray(quantized))]
    return np.unique(snap_to_15_bit(used), axis=0)


def dither_to_palette(srgb8, palette, dither):
    """Each texel to its nearest palette colour, after the 4x4 ordered offset,
    which is scaled to how far apart the palette's colours are."""
    pixels = srgb8.astype(np.float32)
    if len(palette) > 1 and dither > 0:
        gaps = np.linalg.norm(palette[:, None].astype(np.float32) - palette[None, :], axis=-1)
        np.fill_diagonal(gaps, np.inf)
        spread = float(np.median(gaps.min(axis=1))) / np.sqrt(3)
        height, width = srgb8.shape[:2]
        offset = np.tile(BAYER_4, (height // 4, width // 4))
        pixels = pixels + (offset * spread * dither)[..., None]
    distances = np.linalg.norm(pixels[:, :, None] - palette[None, None].astype(np.float32), axis=-1)
    return palette[distances.argmin(axis=-1)]


def load_square(path, working_size, crop):
    """The centre square, or `crop` of its width from the middle of it: a
    photo of a wide area makes small stones or bricks on one block."""
    with Image.open(path) as opened:
        photo = ImageOps.exif_transpose(opened).convert("RGB")
    side = max(1, int(round(min(photo.size) * crop)))
    left = (photo.width - side) // 2
    top = (photo.height - side) // 2
    photo = photo.crop((left, top, left + side, top + side))
    size = min(side, working_size)
    return photo.resize((size, size), Image.LANCZOS)


def process(path, options):
    """The picture at each stage, as 8-bit sRGB arrays."""
    square = load_square(path, max(512, options.size * options.span * 8), options.crop)
    original = np.asarray(square)
    linear = to_linear(original.astype(np.float64) / 255.0)
    linear = remove_lighting(linear, options.delight, options.delight_radius)
    linear = make_tileable(linear, options.seam)
    cleaned = (to_srgb(linear) * 255.0 + 0.5).astype(np.uint8)

    small = grade(to_srgb(shrink(linear, options.size * options.span)), options.contrast, options.saturation)
    small8 = (small * 255.0 + 0.5).astype(np.uint8)
    palette = make_palette(small8, options.colors)
    final = dither_to_palette(small8, palette, options.dither).astype(np.uint8)
    return {"original": original, "cleaned": cleaned, "final": final, "colours": len(palette)}


def one_colour_png(path, colour):
    Image.new("RGBA", (1, 1), colour).save(path)


def write_texture(blocks, name, final, generated_normals, span):
    Image.fromarray(final).convert("RGBA").save(blocks / f"{name}.png")
    # The game reads <name>_span.txt as how many blocks the picture covers
    # each way (BlocksLoader::spannedTextures).
    if span > 1:
        (blocks / f"{name}_span.txt").write_text(f"{span}\n")
    else:
        (blocks / f"{name}_span.txt").unlink(missing_ok=True)
    if generated_normals:
        for suffix in ("_n", "_s", "_b"):
            (blocks / f"{name}{suffix}.png").unlink(missing_ok=True)
        return
    # The loader's own defaults (blocksLoader.cpp).
    one_colour_png(blocks / f"{name}_n.png", (127, 127, 255, 255))
    one_colour_png(blocks / f"{name}_s.png", (0, 0, 0, 255))
    one_colour_png(blocks / f"{name}_b.png", (0, 0, 0, 255))


def nearest(array, size):
    return Image.fromarray(array).resize((size, size), Image.NEAREST)


def write_preview(results, path, cell=200, gap=8):
    headers = ["photo", "lighting removed, edges meet", "texture", "texture tiled 3x3"]
    columns = len(headers)
    header_height = 18
    label_height = 16
    row_height = cell + label_height + gap
    sheet = Image.new("RGB", (columns * (cell + gap) + gap, header_height + row_height * len(results) + gap),
                      (32, 32, 36))
    draw = ImageDraw.Draw(sheet)
    for column, header in enumerate(headers):
        draw.text((gap + column * (cell + gap), 4), header, fill=(220, 220, 220))
    for row, (name, stages) in enumerate(results):
        top = header_height + row * row_height
        draw.text((gap, top), f"{name}  ({stages['colours']} colours)", fill=(170, 200, 170))
        tiled = np.tile(stages["final"], (3, 3, 1))
        # Whole texels where the picture fits the cell; shrunk to it otherwise.
        fit = lambda texels: cell - cell % texels if texels <= cell else cell
        pictures = [Image.fromarray(stages["original"]).resize((cell, cell), Image.BILINEAR),
                    Image.fromarray(stages["cleaned"]).resize((cell, cell), Image.BILINEAR),
                    nearest(stages["final"], fit(stages["final"].shape[0])),
                    nearest(tiled, fit(tiled.shape[0]))]
        for column, picture in enumerate(pictures):
            sheet.paste(picture, (gap + column * (cell + gap), top + label_height))
    sheet.save(path)


def write_pack_icon(results, path, size=128):
    """The menu shows pack.png: up to four of the textures."""
    icon = Image.new("RGB", (size, size))
    half = size // 2
    for i in range(4):
        final = results[i % len(results)][1]["final"]
        icon.paste(nearest(final, half), ((i % 2) * half, (i // 2) * half))
    icon.convert("RGBA").save(path)


def known_block_names():
    source = REPO_ROOT / "src" / "gameLayer" / "blocksLoader.cpp"
    try:
        text = source.read_text(errors="replace")
    except OSError:
        return []
    table = re.search(r"texturesNames\[\]\s*=\s*\{(.*?)\};", text, re.S)
    return re.findall(r'"([^"]+)"', table.group(1)) if table else []


def find_photos(source):
    if source.is_file():
        return [source]
    if not source.is_dir():
        sys.exit(f"no such file or folder: {source}")
    photos = []
    for path in sorted(source.iterdir()):
        if path.suffix.lower() in PHOTO_EXTENSIONS:
            photos.append(path)
        elif path.suffix.lower() in {".heic", ".heif"}:
            print(f"skipped {path.name}: HEIC is not readable here, convert it with "
                  f"sips -s format jpeg {path.name} --out {path.stem}.jpg")
    if not photos:
        sys.exit(f"no photos in {source}")
    return photos


def fraction(text):
    value = float(text)
    if not 0 <= value <= 1:
        raise argparse.ArgumentTypeError("must be between 0 and 1")
    return value


def main():
    parser = argparse.ArgumentParser(description="Photos to PlayStation-looking block textures.")
    parser.add_argument("source", type=Path, help="a photo, or a folder of them, named after the block texture")
    parser.add_argument("--name", help="the block texture a single photo replaces, instead of its file name")
    parser.add_argument("--pack", default="psxPhotos", help="the texture pack's folder name (default: %(default)s)")
    parser.add_argument("--span", type=int, default=1,
                        help="how many blocks the picture covers each way, 1-16, so it repeats every N blocks instead of every block (default: %(default)s)")
    parser.add_argument("--size", type=int, choices=TEXTURE_SIZES, default=64, help="texels across one block (default: %(default)s)")
    parser.add_argument("--colors", type=int, default=16, help="palette size, 2-256 (default: %(default)s)")
    parser.add_argument("--dither", type=fraction, default=0.5, help="ordered dither strength, 0-1 (default: %(default)s)")
    parser.add_argument("--delight", type=fraction, default=1.0, help="how much of the photo's lighting to remove, 0-1 (default: %(default)s)")
    parser.add_argument("--delight-radius", type=fraction, default=0.08,
                        help="what counts as lighting, as a fraction of the width: smaller flattens more (default: %(default)s)")
    parser.add_argument("--crop", type=fraction, default=1.0,
                        help="how much of the photo's width to use, from its centre; under 1 needs --seam to tile (default: %(default)s)")
    parser.add_argument("--seam", type=fraction, default=0.2, help="how much of the edges to cross-fade, 0-0.5 (default: %(default)s)")
    parser.add_argument("--contrast", type=float, default=1.15, help="default: %(default)s")
    parser.add_argument("--saturation", type=float, default=1.1, help="default: %(default)s")
    parser.add_argument("--generated-normals", action="store_true", help="let the game build normal and bump maps from brightness")
    options = parser.parse_args()

    if not 2 <= options.colors <= 256:
        parser.error("--colors must be between 2 and 256")
    if not 1 <= options.span <= 16:
        parser.error("--span must be between 1 and 16")
    if options.crop <= 0:
        parser.error("--crop must be above 0")
    if options.seam > 0.5:
        parser.error("--seam must be at most 0.5")

    known = known_block_names()
    pack = REPO_ROOT / "resources" / "texturePacks" / options.pack
    blocks = pack / "blocks"
    blocks.mkdir(parents=True, exist_ok=True)

    results = []
    photos = find_photos(options.source)
    if options.name and len(photos) != 1:
        parser.error("--name is for a single photo")
    for path in photos:
        name = options.name or path.stem
        if known and name not in known:
            close = difflib.get_close_matches(name, known, n=1)
            hint = f" (did you mean {close[0]}?)" if close else ""
            print(f"warning: no block texture is named {name}{hint}; it will have no effect")
        try:
            stages = process(path, options)
        except (OSError, ValueError) as error:
            print(f"skipped {path.name}: {error}")
            continue
        write_texture(blocks, name, stages["final"], options.generated_normals, options.span)
        results.append((name, stages))
        print(f"{name}: {options.size}x{options.size} a block, across {options.span}x{options.span} blocks, {stages['colours']} colours")

    if not results:
        sys.exit("nothing was written")
    if not (pack / "pack.png").exists():  # an existing pack keeps its own icon
        write_pack_icon(results, pack / "pack.png")
    write_preview(results, TOOL_DIR / "preview.png")
    print(f"pack: {pack}")
    print(f"preview: {TOOL_DIR / 'preview.png'}")


if __name__ == "__main__":
    main()
