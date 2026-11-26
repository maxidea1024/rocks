"""스플래시 이미지 위에 엔진이 그릴 글자 칸(기본 배치, Docs/Design/SplashScreen.ko.md 6절)을 얹은 모의 화면을 만든다.

실제 엔진 출력이 아니라 배치 확인·문서용이다. 글꼴은 Windows 기본(Segoe UI)으로 엔진 Windows 구현과 같다.

사용: python mockup.py <image> <out> <scale> <title> <version> <progress> <fraction> <copyright>
"""
import sys

from PIL import Image, ImageDraw, ImageFont

FONTS = "C:/Windows/Fonts/"

# 기본 배치(논리 px, 이미지 아래·좌우 가장자리 기준) — 스펙 6.2절 표와 같은 값
MARGIN_X = 32
TITLE_FROM_BOTTOM = 84
VERSION_FROM_BOTTOM = 55
PROGRESS_FROM_BOTTOM = 34
COPYRIGHT_FROM_BOTTOM = 33
BAR_FROM_BOTTOM = 12
BAR_HEIGHT = 2


def main():
    src, dst, scale = sys.argv[1], sys.argv[2], int(sys.argv[3])
    title, version, progress, fraction, copyright_text = sys.argv[4], sys.argv[5], sys.argv[6], float(sys.argv[7]), sys.argv[8]
    base = Image.open(src).convert("RGBA")
    overlay = Image.new("RGBA", base.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    w, h = base.width, base.height
    s = scale

    def font(name, px):
        return ImageFont.truetype(FONTS + name, px * s)

    if title:
        draw.text((MARGIN_X * s, h - TITLE_FROM_BOTTOM * s), title, font=font("seguisb.ttf", 20), fill=(255, 255, 255, 255))
    if version:
        draw.text((MARGIN_X * s, h - VERSION_FROM_BOTTOM * s), version, font=font("segoeui.ttf", 12), fill=(168, 168, 168, 255))
    if progress:
        draw.text((MARGIN_X * s, h - PROGRESS_FROM_BOTTOM * s), progress, font=font("segoeui.ttf", 12), fill=(214, 214, 214, 255))
    if copyright_text:
        f = font("segoeui.ttf", 11)
        width = draw.textlength(copyright_text, font=f)
        draw.text((w - MARGIN_X * s - width, h - COPYRIGHT_FROM_BOTTOM * s), copyright_text, font=f, fill=(128, 128, 128, 255))
    x0, x1 = MARGIN_X * s, w - MARGIN_X * s
    y0 = h - BAR_FROM_BOTTOM * s
    draw.rectangle([x0, y0, x1 - 1, y0 + BAR_HEIGHT * s - 1], fill=(255, 255, 255, 31))
    composed = Image.alpha_composite(base, overlay)
    overlay = Image.new("RGBA", base.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    filled = int((x1 - x0) * fraction)
    if filled > 0:
        draw.rectangle([x0, y0, x0 + filled - 1, y0 + BAR_HEIGHT * s - 1], fill=(245, 242, 235, 255))
    composed = Image.alpha_composite(composed, overlay)
    composed.convert("RGB").save(dst, optimize=True)


if __name__ == "__main__":
    main()
