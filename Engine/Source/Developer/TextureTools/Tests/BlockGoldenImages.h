#pragma once

// 블록 압축 golden의 시험 그림 생성기다(Docs/Design/AssetSystem 12.17.4 3). golden probe
// (Tools/NvttGolden/NvttGoldenProbe.cpp)와 시험(TextureBlockEncodeTests.cpp)이 같은 이 헤더를
// include한다. probe는 Rocks 빌드 밖에서 NVTT 원본과 함께 컴파일되므로 이 헤더는 Rocks 헤더·
// 표준 헤더 없이 기본 타입만 쓴다. 모든 식은 정수다(컴파일러·호스트와 무관하게 같은 그림).
//
// 사용 예:
//   for (const block_golden::Case& one : block_golden::Cases) {
//       unsigned char texels[block_golden::MaxTexels * 4];
//       block_golden::Generate(one, texels);  // sizeX × sizeY RGBA8, 행 우선
//   }
namespace rocks::test::block_golden {

// 그림 종류다(값은 golden 파일에 쓰이지 않는다 — 이름과 입력 해시로 맞춘다).
enum Pattern : unsigned {
    Gradient,
    Radial,
    ValueNoise,
    WhiteNoise,
    Checker,
    AlphaEdge,
    Normal,
    TwoColor,
    OneDifferent,
    Gray,
    SolidBlocks,
    NegativeCorrelation,
    FuzzNarrow,
    FuzzClusters,
    // 드문 경로(12.17.4 4): 값 차이 0–2(565 같은 끝점), 블록마다 2–4색 + 섞인 알파, 0·255 근처 알파
    // (6값 모드 붕괴·index 6/7), 한 texel의 한 채널만 1 다름(거의 퇴화한 공분산).
    NearSame,
    FewColorsAlpha,
    AlphaExtremes,
    OneChannelOne,
};

struct Case {
    const char* name;
    Pattern pattern;
    unsigned sizeX;
    unsigned sizeY;
    unsigned seed;
    // 참이면 golden이 출력 byte 없이 해시만 담는다(큰 그림 — fixture 크기를 늘리지 않는다).
    bool hashOnly = false;
};

// 가장 큰 그림의 texel 수다(드문 경로 256 × 256).
inline constexpr unsigned MaxTexels = 256 * 256;

// 그림 목록이다. 32 × 32 그림 열둘, 가장자리 크기 열하나, 무작위 128 × 128 셋, 드문 경로 256 ×
// 256 넷(해시만)이다.
inline constexpr Case Cases[] = {
    {"gradient", Gradient, 32, 32, 1},
    {"radial", Radial, 32, 32, 2},
    {"value-noise", ValueNoise, 32, 32, 3},
    {"white-noise", WhiteNoise, 32, 32, 4},
    {"checker", Checker, 32, 32, 5},
    {"alpha-edge", AlphaEdge, 32, 32, 6},
    {"normal", Normal, 32, 32, 7},
    {"two-color", TwoColor, 32, 32, 8},
    {"one-different", OneDifferent, 32, 32, 9},
    {"gray", Gray, 32, 32, 10},
    {"solid-blocks", SolidBlocks, 32, 32, 11},
    {"negative-correlation", NegativeCorrelation, 32, 32, 12},
    {"edge-1x1", WhiteNoise, 1, 1, 21},
    {"edge-2x2", WhiteNoise, 2, 2, 22},
    {"edge-3x3", WhiteNoise, 3, 3, 23},
    {"edge-5x5", Gradient, 5, 5, 24},
    {"edge-6x7", WhiteNoise, 6, 7, 25},
    {"edge-7x6", ValueNoise, 7, 6, 26},
    {"edge-9x9", WhiteNoise, 9, 9, 27},
    {"edge-1x9", Gradient, 1, 9, 28},
    {"edge-9x1", WhiteNoise, 9, 1, 29},
    {"edge-3x5", AlphaEdge, 3, 5, 30},
    {"edge-2x6", WhiteNoise, 2, 6, 31},
    {"fuzz-uniform", WhiteNoise, 128, 128, 41},
    {"fuzz-narrow", FuzzNarrow, 128, 128, 42},
    {"fuzz-clusters", FuzzClusters, 128, 128, 43},
    {"rare-near-same", NearSame, 256, 256, 51, true},
    {"rare-few-colors-alpha", FewColorsAlpha, 256, 256, 52, true},
    {"rare-alpha-extremes", AlphaExtremes, 256, 256, 53, true},
    {"rare-one-channel-one", OneChannelOne, 256, 256, 54, true},
};

// xorshift32다(seed 0은 1로 바꾼다).
struct Random {
    unsigned state;

    explicit constexpr Random(unsigned seed) noexcept : state(seed == 0 ? 1u : seed) {
    }

    constexpr unsigned Next() noexcept {
        unsigned x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }

    constexpr unsigned char Byte() noexcept {
        return static_cast<unsigned char>(Next() >> 24);
    }
};

// value * 255 / (extent - 1)이다(extent 1이면 0).
constexpr unsigned Ramp(unsigned value, unsigned extent) noexcept {
    return extent <= 1 ? 0u : value * 255u / (extent - 1u);
}

constexpr unsigned char Clamp255(int value) noexcept {
    return static_cast<unsigned char>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

// 8 texel 간격 격자 값의 쌍선형 보간이다(정수, 격자는 그림마다 seed로 만든다).
inline unsigned char ValueNoiseAt(unsigned x, unsigned y, unsigned channel,
                                  unsigned seed) noexcept {
    constexpr unsigned Cell = 8;
    const auto lattice = [&](unsigned gx, unsigned gy) noexcept {
        Random random(seed * 2654435761u + gx * 73856093u + gy * 19349663u + channel * 83492791u);
        (void)random.Next();
        return static_cast<int>(random.Byte());
    };
    const unsigned gx = x / Cell;
    const unsigned gy = y / Cell;
    const int fx = static_cast<int>(x % Cell);
    const int fy = static_cast<int>(y % Cell);
    const int top = lattice(gx, gy) * (static_cast<int>(Cell) - fx) + lattice(gx + 1, gy) * fx;
    const int bottom =
        lattice(gx, gy + 1) * (static_cast<int>(Cell) - fx) + lattice(gx + 1, gy + 1) * fx;
    const int value =
        (top * (static_cast<int>(Cell) - fy) + bottom * fy) / static_cast<int>(Cell * Cell);
    return Clamp255(value);
}

// one을 sizeX × sizeY RGBA8(행 우선)로 rgba에 쓴다. rgba는 sizeX × sizeY × 4 byte 이상이다.
inline void Generate(const Case& one, unsigned char* rgba) noexcept {
    Random random(one.seed);
    const unsigned w = one.sizeX;
    const unsigned h = one.sizeY;
    // 블록마다 고정한 값(단색·두 색·무작위 분포)이다. 블록 수 상한은 (256 / 4)²다.
    constexpr unsigned MaxBlocks = (256 / 4) * (256 / 4);
    unsigned char blockColor[MaxBlocks][2][4] = {};
    for (unsigned block = 0; block < MaxBlocks; ++block) {
        for (unsigned pick = 0; pick < 2; ++pick) {
            for (unsigned channel = 0; channel < 4; ++channel) {
                blockColor[block][pick][channel] = random.Byte();
            }
        }
    }

    const unsigned blocksX = (w + 3) / 4;
    for (unsigned y = 0; y < h; ++y) {
        for (unsigned x = 0; x < w; ++x) {
            unsigned char* out = rgba + (y * w + x) * 4;
            const unsigned block = (y / 4) * blocksX + (x / 4);
            const unsigned char* first = blockColor[block % MaxBlocks][0];
            const unsigned char* second = blockColor[block % MaxBlocks][1];
            unsigned r = 0;
            unsigned g = 0;
            unsigned b = 0;
            unsigned a = 255;
            switch (one.pattern) {
            case Gradient:
                r = Ramp(x, w);
                g = Ramp(y, h);
                b = Ramp(x + y, w + h - 1);
                break;
            case Radial: {
                const int dx = 2 * static_cast<int>(x) - static_cast<int>(w - 1);
                const int dy = 2 * static_cast<int>(y) - static_cast<int>(h - 1);
                const int limit = static_cast<int>(w * w + h * h);
                const int d2 = dx * dx + dy * dy;
                r = 255u - static_cast<unsigned>(d2 * 255 / (limit == 0 ? 1 : limit));
                g = r / 2;
                b = 255u - r;
                break;
            }
            case ValueNoise:
                r = ValueNoiseAt(x, y, 0, one.seed);
                g = ValueNoiseAt(x, y, 1, one.seed);
                b = ValueNoiseAt(x, y, 2, one.seed);
                break;
            case WhiteNoise:
                r = random.Byte();
                g = random.Byte();
                b = random.Byte();
                a = random.Byte();
                break;
            case Checker: {
                constexpr unsigned char Colors[4][3] = {
                    {230, 30, 40}, {20, 200, 60}, {30, 40, 220}, {240, 240, 30}};
                const unsigned pick = ((x / 3) + 2 * (y / 3)) % 4;
                r = Colors[pick][0];
                g = Colors[pick][1];
                b = Colors[pick][2];
                break;
            }
            case AlphaEdge:
                r = Ramp(x, w);
                g = 255u - Ramp(y, h);
                b = 96;
                a = y < h / 2 ? (x < w / 2 ? 0u : 255u) : Ramp(x, w);
                break;
            case Normal: {
                const int u = w <= 1 ? 0
                                     : (2 * static_cast<int>(x) - static_cast<int>(w - 1)) * 127 /
                                           static_cast<int>(w - 1);
                const int v = h <= 1 ? 0
                                     : (2 * static_cast<int>(y) - static_cast<int>(h - 1)) * 127 /
                                           static_cast<int>(h - 1);
                r = Clamp255(128 + u);
                g = Clamp255(128 + v);
                b = 255;
                break;
            }
            case TwoColor: {
                const unsigned char* pick = (random.Next() & 0x100u) != 0 ? first : second;
                r = pick[0];
                g = pick[1];
                b = pick[2];
                break;
            }
            case OneDifferent:
                r = first[0];
                g = first[1];
                b = first[2];
                if ((x % 4) + 4 * (y % 4) == block % 16) {
                    r = second[0];
                    g = second[1];
                    b = second[2];
                }

                break;
            case Gray:
                r = ValueNoiseAt(x, y, 0, one.seed);
                g = r;
                b = r;
                break;
            case SolidBlocks:
                r = first[0];
                g = first[1];
                b = first[2];
                a = first[3];
                break;
            case NegativeCorrelation: {
                const unsigned t = (x % 4) + 4 * (y % 4);
                r = Clamp255(static_cast<int>(first[0] / 2 + t * 8));
                g = Clamp255(static_cast<int>(255 - first[0] / 2) - static_cast<int>(t * 8));
                b = first[2];
                a = Clamp255(static_cast<int>(first[3]) - static_cast<int>(t * 4));
                break;
            }
            case FuzzNarrow:
                r = Clamp255(static_cast<int>(first[0]) + static_cast<int>(random.Byte() % 17) - 8);
                g = Clamp255(static_cast<int>(first[1]) + static_cast<int>(random.Byte() % 17) - 8);
                b = Clamp255(static_cast<int>(first[2]) + static_cast<int>(random.Byte() % 17) - 8);
                a = Clamp255(static_cast<int>(first[3]) + static_cast<int>(random.Byte() % 17) - 8);
                break;
            case FuzzClusters: {
                const unsigned char* pick = (random.Next() & 0x100u) != 0 ? first : second;
                r = Clamp255(static_cast<int>(pick[0]) + static_cast<int>(random.Byte() % 9) - 4);
                g = Clamp255(static_cast<int>(pick[1]) + static_cast<int>(random.Byte() % 9) - 4);
                b = Clamp255(static_cast<int>(pick[2]) + static_cast<int>(random.Byte() % 9) - 4);
                a = pick[3];
                break;
            }
            case NearSame:
                r = Clamp255(static_cast<int>(first[0]) + static_cast<int>(random.Byte() % 3));
                g = Clamp255(static_cast<int>(first[1]) + static_cast<int>(random.Byte() % 3));
                b = Clamp255(static_cast<int>(first[2]) + static_cast<int>(random.Byte() % 3));
                a = first[3];
                break;
            case FewColorsAlpha: {
                // 블록마다 2–4색(첫 색, 둘째 색, 둘의 평균, 첫 색의 반전) 중 하나와 그 색의 알파다.
                const unsigned colors = 2 + block % 3;
                const unsigned pick = random.Next() % colors;
                const unsigned char* base = pick == 1 ? second : first;
                unsigned char mixed[4];
                for (unsigned channel = 0; channel < 4; ++channel) {
                    mixed[channel] =
                        pick == 2
                            ? static_cast<unsigned char>((first[channel] + second[channel]) / 2)
                        : pick == 3 ? static_cast<unsigned char>(255 - first[channel])
                                    : base[channel];
                }

                r = mixed[0];
                g = mixed[1];
                b = mixed[2];
                a = mixed[3];
                break;
            }
            case AlphaExtremes: {
                r = random.Byte();
                g = random.Byte();
                b = random.Byte();
                const unsigned value = random.Byte();
                a = (value & 1u) != 0 ? (value >> 1) % 4 : 252 + (value >> 1) % 4;
                break;
            }
            case OneChannelOne: {
                r = first[0];
                g = first[1];
                b = first[2];
                a = first[3];
                if ((x % 4) + 4 * (y % 4) == block % 16) {
                    unsigned* channel = block % 3 == 0 ? &r : (block % 3 == 1 ? &g : &b);
                    *channel = *channel == 255 ? 254u : *channel + 1u;
                }

                break;
            }
            }

            out[0] = static_cast<unsigned char>(r);
            out[1] = static_cast<unsigned char>(g);
            out[2] = static_cast<unsigned char>(b);
            out[3] = static_cast<unsigned char>(a);
        }
    }
}

// FNV-1a 64다(golden 입력·디코드 대조 — 보안 용도가 아니다).
inline constexpr unsigned long long FnvOffset = 14695981039346656037ull;

inline unsigned long long Fnv1a64(const unsigned char* bytes, unsigned long long count,
                                  unsigned long long hash = FnvOffset) noexcept {
    constexpr unsigned long long Prime = 1099511628211ull;
    for (unsigned long long index = 0; index < count; ++index) {
        hash ^= bytes[index];
        hash *= Prime;
    }

    return hash;
}

} // namespace rocks::test::block_golden
