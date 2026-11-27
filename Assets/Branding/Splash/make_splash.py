"""Build 1x and 2x Rocks splash images from the shared obsidian artwork."""
import argparse
from pathlib import Path

from PIL import Image

LOGICAL_SIZE = (768, 384)


def render(repo, out, kind):
    branding = repo / "Assets/Branding"
    if kind == "template":
        logo_file = "rocks-symbol-white.png"
        logo_height = 24
        center = (724, 316)
    else:
        logo_file = "rocks-logo-stacked-white.png"
        logo_height = 188
        center = (150, 144)

    stem = "StudioSplash" if kind == "studio" else "Splash"
    out.mkdir(parents=True, exist_ok=True)
    with Image.open(branding / "Splash/Obsidian.png") as background, \
            Image.open(branding / "PNG" / logo_file) as master:
        for scale, suffix in ((1, ""), (2, "@2x")):
            size = tuple(length * scale for length in LOGICAL_SIZE)
            image = background.convert("RGB").resize(size, Image.Resampling.LANCZOS)
            height = logo_height * scale
            width = round(master.width * height / master.height)
            logo = master.convert("RGBA").resize((width, height), Image.Resampling.LANCZOS)
            origin = (center[0] * scale - width // 2, center[1] * scale - height // 2)
            image.paste(logo, origin, logo)
            image.save(out / f"{stem}{suffix}.png", optimize=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("kind", choices=("studio", "game", "template"))
    args = parser.parse_args()
    render(args.repo, args.out, args.kind)
