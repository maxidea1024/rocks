"""Rocks 기본 스플래시 이미지 생성기.

논리 크기 768x384(2:1)를 기준으로 1x·2x 두 장을 만든다. 배경은 절차적 지형 등고선(암석 지층 모티프),
로고는 Assets/Branding의 흰색 PNG 마스터를 고품질 축소해 얹는다(로고 형상은 바꾸지 않는다).

사용: python make_splash.py <repo root> <out dir> studio|game|template
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

LOGICAL_W, LOGICAL_H = 768, 384
# 엔진이 글자를 그리는 아래 띠의 높이와 시작(논리 픽셀 — Docs/Design/SplashScreen 6.2절 SplashTextBandHeight)
TEXT_BAND_HEIGHT = 96
BAND_TOP = LOGICAL_H - TEXT_BAND_HEIGHT

ROCKS_BLACK = np.array([17, 17, 17], dtype=np.float64)


def value_noise(shape, cell, rng):
    """격자 크기 cell(픽셀)의 매끈한 value noise(quintic 보간)."""
    h, w = shape
    gh, gw = h // cell + 3, w // cell + 3
    grid = rng.random((gh, gw))
    ys = np.arange(h) / cell
    xs = np.arange(w) / cell
    y0 = np.floor(ys).astype(int)
    x0 = np.floor(xs).astype(int)
    ty = ys - y0
    tx = xs - x0
    ty = ty * ty * ty * (ty * (ty * 6 - 15) + 10)
    tx = tx * tx * tx * (tx * (tx * 6 - 15) + 10)
    a = grid[y0][:, x0]
    b = grid[y0][:, x0 + 1]
    c = grid[y0 + 1][:, x0]
    d = grid[y0 + 1][:, x0 + 1]
    top = a + (b - a) * tx[None, :]
    bottom = c + (d - c) * tx[None, :]
    return top + (bottom - top) * ty[:, None]


def fbm(shape, base_cell, octaves, rng, gain=0.5):
    total = np.zeros(shape)
    amplitude = 1.0
    norm = 0.0
    cell = base_cell
    for _ in range(octaves):
        total += amplitude * value_noise(shape, max(int(cell), 2), rng)
        norm += amplitude
        amplitude *= gain
        cell /= 2.0
    return total / norm


def warped_field(shape, scale, seed):
    """도메인 왜곡을 넣은 지형 높이장. 등고선이 지층처럼 휘어 흐르게 한다."""
    rng = np.random.default_rng(seed)
    h, w = shape
    warp_x = fbm(shape, 380 * scale, 4, rng)
    warp_y = fbm(shape, 380 * scale, 4, rng)
    base = fbm((h + 400 * scale, w + 400 * scale), 520 * scale, 6, rng)
    yy, xx = np.mgrid[0:h, 0:w]
    sy = np.clip(yy + (warp_y - 0.5) * 260 * scale + 200 * scale, 0, base.shape[0] - 2.001)
    sx = np.clip(xx + (warp_x - 0.5) * 260 * scale + 200 * scale, 0, base.shape[1] - 2.001)
    # 쌍선형 표본(정수로 자르면 등고선이 계단처럼 톱니가 된다)
    y0 = np.floor(sy).astype(int)
    x0 = np.floor(sx).astype(int)
    fy = sy - y0
    fx = sx - x0
    top = base[y0, x0] * (1 - fx) + base[y0, x0 + 1] * fx
    bottom = base[y0 + 1, x0] * (1 - fx) + base[y0 + 1, x0 + 1] * fx
    field = top * (1 - fy) + bottom * fy
    # 큰 기울기를 더해 등고선이 한 방향(오른쪽 위 → 왼쪽 아래)으로 겹쳐 흐르게 한다(지층 느낌).
    field = field + (xx / w) * 0.18 - (yy / h) * 0.10
    return field


def contour_lines(field, levels, line_px, index_every, index_px):
    """반앨리어싱 등고선 강도(0–1)와 굵은 계곡선 강도를 돌려준다."""
    v = field * levels
    gy, gx = np.gradient(v)
    gradient = np.sqrt(gx * gx + gy * gy) + 1e-6
    frac = v - np.floor(v)
    distance = np.minimum(frac, 1.0 - frac) / gradient  # 가장 가까운 등고선까지 픽셀 거리
    thin = np.clip(line_px * 0.5 + 0.5 - distance, 0.0, 1.0)
    # 계곡선(index contour): index_every마다 하나
    vi = v / index_every
    fraci = vi - np.floor(vi)
    distance_index = np.minimum(fraci, 1.0 - fraci) * index_every / gradient
    thick = np.clip(index_px * 0.5 + 0.5 - distance_index, 0.0, 1.0)
    return thin, thick


def smoothstep(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def load_logo(path, target_height):
    logo = Image.open(path).convert("RGBA")
    # 마스터에서 한 번에 최종 크기로 줄인다(지침 9절: 반복 축소 금지).
    w = round(logo.width * target_height / logo.height)
    return logo.resize((w, target_height), Image.LANCZOS)


def to_image(rgb):
    return Image.fromarray(np.clip(np.round(rgb), 0, 255).astype(np.uint8), "RGB")


def dark_contour_background(scale, seed, mask_fn):
    """Rocks Black 위의 흰 등고선. mask_fn(xx, yy) → 0–1(선 보임 정도)."""
    w, h = LOGICAL_W * scale, LOGICAL_H * scale
    field = warped_field((h, w), scale, seed)
    thin, thick = contour_lines(field, 46.0, 0.9 * scale, 5, 1.6 * scale)
    yy, xx = np.mgrid[0:h, 0:w] / scale  # 논리 좌표
    mask = mask_fn(xx, yy)
    # 깊이감: 아주 약한 빛 웅덩이(배경 명암 — 로고에는 그라데이션을 쓰지 않는다)
    glow_center = (560.0, 150.0)
    r = np.sqrt((xx - glow_center[0]) ** 2 + ((yy - glow_center[1]) * 1.25) ** 2)
    glow = np.exp(-(r / 340.0) ** 2) * 9.0
    base = ROCKS_BLACK[None, None, :] + glow[:, :, None]
    lines = (thin * 0.075 + thick * 0.115) * mask
    rgb = base + (255.0 - base) * lines[:, :, None]
    # 아래 띠는 깨끗한 Rocks Black으로(엔진 글자 자리)
    band = smoothstep(BAND_TOP - 22, BAND_TOP - 2, yy)
    rgb = rgb * (1 - band[:, :, None]) + ROCKS_BLACK[None, None, :] * band[:, :, None]
    return rgb


def add_band_rule(image, scale, color=(44, 44, 44)):
    draw = ImageDraw.Draw(image)
    y = BAND_TOP * scale
    draw.rectangle([0, y, image.width, y + scale - 1], fill=color)


def make_studio(repo, out, scale):
    def mask(xx, yy):
        # 로고가 있는 왼쪽은 고요하게, 오른쪽으로 갈수록 선이 드러난다.
        horizontal = smoothstep(250.0, 560.0, xx)
        return 0.18 + 0.82 * horizontal

    rgb = dark_contour_background(scale, seed=20261126, mask_fn=mask)
    image = to_image(rgb)
    add_band_rule(image, scale)
    logo = load_logo(repo / "Assets/Branding/PNG/rocks-logo-stacked-white.png", 188 * scale)
    # 스택형 로고: 위쪽 영역(글자 띠 위)의 세로 가운데, 왼쪽 1/4 축에 둔다.
    cx, cy = 200 * scale, (BAND_TOP * scale) // 2
    image.paste(logo, (cx - logo.width // 2, cy - logo.height // 2), logo)
    image.save(out / f"StudioSplash{'' if scale == 1 else '@2x'}.png", optimize=True)


def make_engine_game(repo, out, scale):
    def mask(xx, yy):
        r = np.sqrt(((xx - 384.0) / 300.0) ** 2 + ((yy - 160.0) / 140.0) ** 2)
        return 0.30 + 0.70 * smoothstep(0.45, 1.20, r)

    rgb = dark_contour_background(scale, seed=7202611, mask_fn=mask)
    image = to_image(rgb)
    add_band_rule(image, scale)
    # 가운데에 브랜드 심볼(작게) — 게임 이름은 엔진이 띠에 그린다.
    symbol = load_logo(repo / "Assets/Branding/PNG/rocks-symbol-white.png", 104 * scale)
    image.paste(symbol, ((LOGICAL_W * scale - symbol.width) // 2, (BAND_TOP * scale) // 2 - symbol.height // 2 - 6 * scale), symbol)
    image.save(out / f"Splash{'' if scale == 1 else '@2x'}.png", optimize=True)


def main():
    repo = Path(sys.argv[1])
    out = Path(sys.argv[2])
    which = sys.argv[3]
    out.mkdir(parents=True, exist_ok=True)
    for scale in (1, 2):
        if which == "studio":
            make_studio(repo, out, scale)
        elif which == "game":
            make_engine_game(repo, out, scale)
        elif which == "template":
            from template_art import make_template
            make_template(repo, out, scale)
        else:
            sys.exit(f"unknown splash kind: {which} (expected studio, game or template)")


if __name__ == "__main__":
    main()
