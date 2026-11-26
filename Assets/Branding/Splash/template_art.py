"""템플릿(게임 프로젝트) 스플래시: 해 질 녘 사암 바위산(뷰트·첨탑) key art. make_splash.py가 import한다."""
import numpy as np

from make_splash import (BAND_TOP, LOGICAL_H, LOGICAL_W, load_logo, smoothstep, to_image)


def midpoint_profile(n_samples, rng, roughness, octaves=10):
    """중점 변위 1차원 잡음(-1..1)."""
    n = 2 ** octaves + 1
    heights = np.zeros(n)
    heights[0] = rng.uniform(-1, 1)
    heights[-1] = rng.uniform(-1, 1)
    step = n - 1
    amplitude = 1.0
    while step > 1:
        half = step // 2
        for i in range(half, n - 1, step):
            heights[i] = (heights[i - half] + heights[i + half]) * 0.5 + rng.uniform(-1, 1) * amplitude
        amplitude *= roughness
        step = half
    profile = np.interp(np.linspace(0, n - 1, n_samples), np.arange(n), heights)
    return profile / (np.abs(profile).max() + 1e-9)


def ridged_profile(n_samples, rng, roughness):
    """뾰족한 봉우리가 생기는 ridged 잡음(0..1, 1이 봉우리)."""
    p = midpoint_profile(n_samples, rng, roughness)
    r = 1.0 - np.abs(p)
    return r ** 2.2


def butte(xs, center, top_width, height, flare, cap_noise):
    """뷰트(평평한 꼭대기 + 가파른 절벽 + 아래로 퍼지는 비탈) 높이(논리 px)."""
    d = np.abs(xs - center)
    cliff = np.clip(1.0 - (d - top_width * 0.5) / flare, 0.0, 1.0)
    cliff = np.where(d <= top_width * 0.5, 1.0, cliff ** 0.35)
    talus = np.clip(1.0 - (d - top_width * 0.5) / (flare * 5.0), 0.0, 1.0) ** 2 * 0.38
    shape = np.maximum(cliff, talus)
    return shape * height + cap_noise * (shape > 0.98) * 2.5


def piecewise_gradient(y, stops):
    """세로 그라데이션: stops = [(y, rgb)], 구간마다 smoothstep 보간(꺾임 띠가 안 보이게)."""
    out = np.zeros(y.shape + (3,))
    ys = [s[0] for s in stops]
    cs = [np.array(s[1], dtype=np.float64) for s in stops]
    out[...] = cs[0]
    for i in range(len(stops) - 1):
        t = np.clip((y - ys[i]) / (ys[i + 1] - ys[i]), 0, 1)
        t = t * t * (3 - 2 * t)
        seg = (y >= ys[i])
        blend = cs[i][None, None, :] + (cs[i + 1] - cs[i])[None, None, :] * t[:, :, None]
        out = np.where(seg[:, :, None], blend, out)
    return out


def make_template(repo, out, scale):
    w, h = LOGICAL_W * scale, LOGICAL_H * scale
    yy, xx = np.mgrid[0:h, 0:w] / scale  # 논리 좌표
    xs = np.arange(w) / scale
    rng = np.random.default_rng(20261126)

    sky = piecewise_gradient(yy, [
        (0.0, (10, 14, 32)),
        (90.0, (26, 32, 64)),
        (160.0, (78, 64, 104)),
        (205.0, (176, 104, 110)),
        (240.0, (238, 150, 96)),
        (268.0, (252, 196, 132)),
    ])
    sun = (548.0, 226.0)
    r = np.sqrt((xx - sun[0]) ** 2 + ((yy - sun[1]) * 1.4) ** 2)
    sun_color = np.array([255, 210, 150], dtype=np.float64)
    glow = np.exp(-(r / 210.0) ** 2) * 0.42 + np.exp(-(r / 60.0) ** 2) * 0.45
    sky = sky + (sun_color - sky) * np.clip(glow, 0, 1)[:, :, None]
    rd = np.sqrt((xx - sun[0]) ** 2 + (yy - sun[1]) ** 2)
    disc = smoothstep(17.0, 15.6, rd)
    sky = sky + (np.array([255, 244, 222]) - sky) * disc[:, :, None]
    # 얇은 구름 띠(해 앞을 가로지르는 가는 층운)
    cloud_noise = midpoint_profile(w, rng, 0.55)
    for cy, thickness, strength in ((150.0, 3.0, 0.20), (172.0, 2.2, 0.26), (196.0, 1.6, 0.22)):
        offset = cloud_noise * 6.0
        band = np.exp(-((yy - cy - offset[None, :]) / thickness) ** 2)
        horizontal = smoothstep(120.0, 360.0, xx) * smoothstep(760.0, 600.0, xx)
        lit = np.exp(-((xx - sun[0]) / 200.0) ** 2)
        cloud_color = np.array([120, 80, 110]) * (1 - lit[:, :, None]) + np.array([255, 190, 140]) * lit[:, :, None]
        a = (band * horizontal * strength)[:, :, None]
        sky = sky * (1 - a) + cloud_color * a
    # 별
    stars = (rng.random((h, w)) > 0.99962) * rng.uniform(0.2, 0.85, (h, w))
    sky = sky + (255 - sky) * (stars * np.clip(1 - yy / 140.0, 0, 1))[:, :, None]

    image = sky
    haze_color = np.array([236, 158, 104], dtype=np.float64)

    def apply_layer(top_y, color, haze, rim_strength, texture_strength):
        nonlocal image
        top_y = top_y * scale
        depth = yy * scale - top_y[None, :]
        inside = np.clip(depth + 0.5, 0.0, 1.0)  # 1 px 반앨리어싱
        base = color.astype(np.float64) * (1 - haze) + haze_color * haze
        # 수평 지층 결: 약한 줄무늬(사암 층리)
        strata = np.sin((yy + midpoint_profile(w, rng, 0.5)[None, :] * 4.0) * 0.42) * 0.5 + 0.5
        ground = np.median(top_y)  # 층 지면 높이(꼭대기 기준 깊이로 음영하면 뷰트 아래에 세로 기둥이 생긴다)
        shade = 1.0 - texture_strength * strata - np.clip((yy * scale - ground) / (140.0 * scale), 0, 1) * 0.25
        fill = base[None, None, :] * shade[:, :, None]
        # 해 쪽 가장자리 역광(1–2 px)
        sun_side = np.exp(-((xx - sun[0]) / 230.0) ** 2)
        rim = np.exp(-np.clip(depth, 0, None) / (1.1 * scale)) * (0.25 + 0.75 * sun_side) * rim_strength
        fill = fill + (sun_color - fill) * np.clip(rim, 0, 1)[:, :, None]
        image = image * (1 - inside[:, :, None]) + fill * inside[:, :, None]

    # 1) 아주 먼 산맥(뾰족)
    far = 238.0 - ridged_profile(w, rng, 0.58) * 34.0 - midpoint_profile(w, rng, 0.5) * 6.0
    apply_layer(far, np.array([170, 110, 120]), 0.62, 0.35, 0.0)
    # 2) 먼 뷰트들
    mid_noise = midpoint_profile(w, rng, 0.62)
    mid = 262.0 - mid_noise * 5.0 - 8.0
    for c, tw, hgt, fl in ((150.0, 70.0, 58.0, 9.0), (300.0, 44.0, 40.0, 7.0), (690.0, 90.0, 66.0, 10.0)):
        mid = mid - butte(xs, c, tw, hgt, fl, mid_noise)
    apply_layer(np.minimum(mid, 268.0), np.array([120, 78, 92]), 0.40, 0.6, 0.04)
    # 먼 층 사이 안개(가까운 층보다 뒤)
    mist = np.exp(-((yy - 268.0) / 12.0) ** 2) * 0.22
    image = image + (haze_color - image) * mist[:, :, None]
    # 3) 가까운 능선 + 큰 뷰트(왼쪽)와 바위기둥(hoodoo)
    near_noise = midpoint_profile(w, rng, 0.6)
    near = 300.0 - ridged_profile(w, rng, 0.55) * 16.0 - near_noise * 4.0
    near = near - butte(xs, 92.0, 120.0, 132.0, 12.0, near_noise)
    near = near - butte(xs, 200.0, 9.0, 92.0, 3.5, near_noise) - butte(xs, 222.0, 6.0, 54.0, 3.0, near_noise)
    near = near - butte(xs, 470.0, 36.0, 30.0, 6.0, near_noise)
    apply_layer(near, np.array([54, 36, 46]), 0.10, 1.0, 0.05)
    # 4) 전경(거의 실루엣)
    front = 330.0 - midpoint_profile(w, rng, 0.58) * 10.0 - ridged_profile(w, rng, 0.5) * 8.0
    apply_layer(front, np.array([26, 20, 28]), 0.0, 0.8, 0.03)


    # 아래 띠: 엔진 글자 자리라 거의 검은색으로
    band = smoothstep(BAND_TOP - 36, BAND_TOP + 6, yy)
    band_color = np.array([13, 11, 15], dtype=np.float64)
    image = image * (1 - band[:, :, None] * 0.94) + band_color * band[:, :, None] * 0.94
    # 비네트와 미세 grain(그라데이션 밴딩 방지)
    vignette = np.sqrt(((xx - 400) / 560.0) ** 2 + ((yy - 210) / 360.0) ** 2)
    image = image * (1 - np.clip(vignette - 0.55, 0, 1)[:, :, None] * 0.6)
    image = image + rng.normal(0, 1.0, image.shape)

    result = to_image(image)
    # "Powered by" 표시: 띠 오른쪽 아래에 심볼만(가로형 로고 최소 폭 240 px보다 작은 자리 — 지침 6절)
    symbol = load_logo(repo / "Assets/Branding/PNG/rocks-symbol-white.png", 24 * scale)
    symbol.putalpha(symbol.getchannel("A").point(lambda a: int(a * 0.85)))
    result.paste(symbol, (w - symbol.width - 32 * scale, 352 * scale), symbol)  # 제목 줄 오른쪽(저작권 자리와 겹치지 않게)
    result.save(out / f"Splash{'' if scale == 1 else '@2x'}.png", optimize=True)
