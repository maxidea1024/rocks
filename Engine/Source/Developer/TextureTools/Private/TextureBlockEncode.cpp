#include "TextureBlockTables.h"

#include <Base/NumericLimits.h>
#include <TextureTools/TextureBlockEncode.h>

// BC1·BC3·BC4·BC5 인코더 구현이다(Docs/Design/AssetSystem 12.17절 TD1–TD4·TD7, 12.17.4 1). 색은
// squish WeightedClusterFit의 SSE 경로(nvtt/squish/weightedclusterfit.cpp 437–676, simd_sse.h)를
// lane마다 float 연산 하나씩 같은 순서로 옮겼다. 곱셈과 덧셈은 따로 쓴다(빌드가 FMA 합치기를 끈다 —
// -ffp-contract=off). 알파는 NVTT QuickCompress::compressDXT5A(QuickCompressDXT.cpp 290–578)다.
namespace rocks {

namespace {

// 유니티 빌드에서 같은 묶음의 이름(rocks::Vec3·Min·Max 등)과 부딪히지 않게 이름 있는 namespace에
// 둔다.
namespace bc_encode {

using texture_block_detail::SingleColor5;
using texture_block_detail::SingleColor6;

// 블록 byte 수다.
constexpr uint32 SmallBlockBytes = 8;
constexpr uint32 LargeBlockBytes = 16;
// 565 양자화 격자다.
constexpr float Grid5 = 31.0f;
constexpr float Grid6 = 63.0f;
constexpr int Max5 = 31;
constexpr int Max6 = 63;
// 8비트 값의 최대다.
constexpr float ByteMaxFloat = 255.0f;
// BC3 색 가중치 (a + 1) / 256의 분모다(squish colourset.cpp 63).
constexpr float AlphaWeightScale = 256.0f;
// 주축 power iteration 횟수다(squish maths.cpp POWER_ITERATION_COUNT).
constexpr int PowerIterations = 8;
// 알파 최소제곱 반복 상한이다(QuickCompress::compressDXT5A iterationCount 기본값).
constexpr int AlphaIterations = 8;
// 알파 끝점을 안으로 당기는 몫의 분모다(QuickCompressDXT.cpp 551).
constexpr int AlphaInset = 34;
// 알파 index 하나의 비트 수와 첫 index의 비트 위치다.
constexpr uint32 AlphaIndexBits = 3;
constexpr uint32 AlphaIndexShift = 16;
constexpr uint64 AlphaIndexMask = 0x7;
// 알파 팔레트 크기다.
constexpr uint32 AlphaPaletteSize = 8;
// 단색 블록 index(모두 2, 맞바꾸면 모두 3 — OptimalCompressDXT.cpp 197·202)다.
constexpr uint32 SingleColorIndices = 0xAAAAAAAAu;
constexpr uint32 SingleColorFlip = 0x55555555u;
// int32로 나타낼 수 없는 float(NaN·범위 밖)를 SSE cvttps가 돌려주는 값이다.
constexpr int32 IntegerIndefinite = NumericLimits<int32>::Lowest();
// int32 범위 경계(2^31)다.
constexpr float Int32Limit = 2147483648.0f;

bool IsNan(float value) noexcept {
    return value != value;
}

// _mm_cvttps_epi32 한 lane이다: 0 쪽 절사, NaN·범위 밖은 INT32_MIN.
int32 TruncateToInt(float value) noexcept {
    if (IsNan(value) || value >= Int32Limit || value < -Int32Limit) {
        return IntegerIndefinite;
    }

    return static_cast<int32>(value);
}

// squish Vec4(SSE) 한 값이다. 연산마다 lane 넷을 같은 순서로 계산한다.
struct Lane4 {
    float v[4];
};

constexpr Lane4 Splat(float value) noexcept {
    return Lane4{{value, value, value, value}};
}

Lane4 Add(const Lane4& a, const Lane4& b) noexcept {
    return Lane4{{a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2], a.v[3] + b.v[3]}};
}

Lane4 Sub(const Lane4& a, const Lane4& b) noexcept {
    return Lane4{{a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2], a.v[3] - b.v[3]}};
}

Lane4 Mul(const Lane4& a, const Lane4& b) noexcept {
    return Lane4{{a.v[0] * b.v[0], a.v[1] * b.v[1], a.v[2] * b.v[2], a.v[3] * b.v[3]}};
}

// MultiplyAdd(a, b, c) = a·b + c(FMA 아님 — 곱을 먼저 반올림한다).
Lane4 MultiplyAdd(const Lane4& a, const Lane4& b, const Lane4& c) noexcept {
    return Add(Mul(a, b), c);
}

// NegativeMultiplySubtract(a, b, c) = c − a·b.
Lane4 NegativeMultiplySubtract(const Lane4& a, const Lane4& b, const Lane4& c) noexcept {
    return Sub(c, Mul(a, b));
}

// Reciprocal = 1 / v 나눗셈(SQUISH_USE_DIV).
Lane4 Reciprocal(const Lane4& a) noexcept {
    return Lane4{{1.0f / a.v[0], 1.0f / a.v[1], 1.0f / a.v[2], 1.0f / a.v[3]}};
}

// _mm_min_ps·_mm_max_ps: 비교가 거짓이면(NaN 포함) 둘째 값이다.
float MinLane(float x, float y) noexcept {
    return x < y ? x : y;
}

float MaxLane(float x, float y) noexcept {
    return x > y ? x : y;
}

Lane4 Min(const Lane4& a, const Lane4& b) noexcept {
    return Lane4{{MinLane(a.v[0], b.v[0]), MinLane(a.v[1], b.v[1]), MinLane(a.v[2], b.v[2]),
                  MinLane(a.v[3], b.v[3])}};
}

Lane4 Max(const Lane4& a, const Lane4& b) noexcept {
    return Lane4{{MaxLane(a.v[0], b.v[0]), MaxLane(a.v[1], b.v[1]), MaxLane(a.v[2], b.v[2]),
                  MaxLane(a.v[3], b.v[3])}};
}

// Truncate = int32 절사 뒤 float(SSE2 cvttps_epi32 → cvtepi32_ps).
Lane4 Truncate(const Lane4& a) noexcept {
    return Lane4{
        {static_cast<float>(TruncateToInt(a.v[0])), static_cast<float>(TruncateToInt(a.v[1])),
         static_cast<float>(TruncateToInt(a.v[2])), static_cast<float>(TruncateToInt(a.v[3]))}};
}

Lane4 SplatLane(const Lane4& a, int lane) noexcept {
    return Splat(a.v[lane]);
}

// squish Vec3(스칼라)다. 유니티 묶음에서 rocks::Vec3와 부딪히지 않게 이름을 바꿨다.
struct SquishVec3 {
    float x;
    float y;
    float z;
};

float Dot(const SquishVec3& a, const SquishVec3& b) noexcept {
    const float xx = a.x * b.x;
    const float yy = a.y * b.y;
    const float zz = a.z * b.z;
    const float xy = xx + yy;
    return xy + zz;
}

// 블록 색 집합이다(squish ColourSet). 점은 (R, G, B) / 255, 가중치는 1 또는 (a + 1) / 256이다.
struct ColourSet {
    int count = 0;
    SquishVec3 points[TextureBlockTexels];
    float weights[TextureBlockTexels];
    int remap[TextureBlockTexels];
};

// createMinimalSet 참(BC1 — 같은 RGB를 가중치 1씩 묶는다)·거짓(BC3 — 16점, 알파 가중치)이다.
// flags에 kDxt1이 없으므로 투명 texel 처리는 없다(CompressDXT.cpp 224·353).
void BuildColourSet(const uint8 (&texels)[TextureBlockRgbaBytes], bool minimalSet,
                    bool weightByAlpha, ColourSet& set) noexcept {
    set.count = 0;
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        const uint8* texel = texels + i * TextureBlockTexelBytes;
        const float weight =
            weightByAlpha ? static_cast<float>(texel[3] + 1) / AlphaWeightScale : 1.0f;
        if (minimalSet) {
            bool matched = false;
            for (uint32 j = 0; j < i; ++j) {
                const uint8* other = texels + j * TextureBlockTexelBytes;
                if (texel[0] == other[0] && texel[1] == other[1] && texel[2] == other[2]) {
                    const int index = set.remap[j];
                    set.weights[index] += weight;
                    set.remap[i] = index;
                    matched = true;
                    break;
                }
            }

            if (matched) {
                continue;
            }
        }

        set.points[set.count] = SquishVec3{static_cast<float>(texel[0]) / ByteMaxFloat,
                                           static_cast<float>(texel[1]) / ByteMaxFloat,
                                           static_cast<float>(texel[2]) / ByteMaxFloat};
        set.weights[set.count] = weight;
        set.remap[i] = set.count;
        ++set.count;
    }
}

// squish ComputeWeightedCovariance(maths.cpp 31–60, metric 1·1·1)다. 공분산 6칸이다.
void WeightedCovariance(const ColourSet& set, float (&covariance)[6]) noexcept {
    float total = 0.0f;
    SquishVec3 centroid{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < set.count; ++i) {
        const float w = set.weights[i];
        total += w;
        centroid.x += set.points[i].x * w;
        centroid.y += set.points[i].y * w;
        centroid.z += set.points[i].z * w;
    }

    // SquishVec3::operator/=(float)는 역수를 곱한다(maths.h 107–113).
    const float inverse = 1.0f / total;
    centroid.x *= inverse;
    centroid.y *= inverse;
    centroid.z *= inverse;
    for (float& value : covariance) {
        value = 0.0f;
    }

    constexpr float Metric = 1.0f;
    for (int i = 0; i < set.count; ++i) {
        const SquishVec3 a{(set.points[i].x - centroid.x) * Metric,
                           (set.points[i].y - centroid.y) * Metric,
                           (set.points[i].z - centroid.z) * Metric};
        const float w = set.weights[i];
        const SquishVec3 b{a.x * w, a.y * w, a.z * w};
        covariance[0] += a.x * b.x;
        covariance[1] += a.x * b.y;
        covariance[2] += a.x * b.z;
        covariance[3] += a.y * b.y;
        covariance[4] += a.y * b.z;
        covariance[5] += a.z * b.z;
    }
}

// squish ComputePrincipleComponent SIMD판(maths.cpp 64–107)이다. 시작은 노름이 가장 큰 행, 정규화는
// 세 성분의 max(0이면 NaN — 투영 정렬이 순서를 그대로 둔다, 12.17절 TD2).
SquishVec3 PrincipalComponent(const float (&m)[6]) noexcept {
    const SquishVec3 row0v{m[0], m[1], m[2]};
    const SquishVec3 row1v{m[1], m[3], m[4]};
    const SquishVec3 row2v{m[2], m[4], m[5]};
    const float r0 = Dot(row0v, row0v);
    const float r1 = Dot(row1v, row1v);
    const float r2 = Dot(row2v, row2v);
    const SquishVec3 start = (r0 > r1 && r0 > r2) ? row0v : (r1 > r2 ? row1v : row2v);
    const Lane4 row0{{m[0], m[1], m[2], 0.0f}};
    const Lane4 row1{{m[1], m[3], m[4], 0.0f}};
    const Lane4 row2{{m[2], m[4], m[5], 0.0f}};
    Lane4 v{{start.x, start.y, start.z, 0.0f}};
    for (int i = 0; i < PowerIterations; ++i) {
        Lane4 w = Mul(row0, SplatLane(v, 0));
        w = MultiplyAdd(row1, SplatLane(v, 1), w);
        w = MultiplyAdd(row2, SplatLane(v, 2), w);
        const Lane4 a = Max(SplatLane(w, 0), Max(SplatLane(w, 1), SplatLane(w, 2)));
        v = Mul(w, Reciprocal(a));
    }

    return SquishVec3{v.v[0], v.v[1], v.v[2]};
}

// squish WeightedClusterFit(SIMD판) 상태다.
struct ClusterFit {
    const ColourSet* colours = nullptr;
    int order[TextureBlockTexels];
    Lane4 weighted[TextureBlockTexels];
    Lane4 xsum;
    Lane4 bestError;
};

// WeightedClusterFit::SetColourSet(weightedclusterfit.cpp 45–110)이다.
void SetColourSet(const ColourSet& set, ClusterFit& fit) noexcept {
    fit.colours = &set;
    fit.bestError = Splat(NumericLimits<float>::Max());
    float covariance[6];
    WeightedCovariance(set, covariance);
    const SquishVec3 principle = PrincipalComponent(covariance);
    float dps[TextureBlockTexels];
    for (int i = 0; i < set.count; ++i) {
        dps[i] = Dot(set.points[i], principle);
        fit.order[i] = i;
    }

    // 안정 삽입 정렬(NaN 비교는 거짓이라 자리를 바꾸지 않는다).
    for (int i = 0; i < set.count; ++i) {
        for (int j = i; j > 0 && dps[j] < dps[j - 1]; --j) {
            const float d = dps[j];
            dps[j] = dps[j - 1];
            dps[j - 1] = d;
            const int o = fit.order[j];
            fit.order[j] = fit.order[j - 1];
            fit.order[j - 1] = o;
        }
    }

    fit.xsum = Splat(0.0f);
    for (int i = 0; i < set.count; ++i) {
        const int p = fit.order[i];
        const Lane4 point{{set.points[p].x, set.points[p].y, set.points[p].z, 1.0f}};
        fit.weighted[i] = Mul(Splat(set.weights[p]), point);
        fit.xsum = Add(fit.xsum, fit.weighted[i]);
    }
}

// squish FloatToInt(colourblock.cpp 29–41): (int)(a + 0.5f)를 0…limit로 자른다. 변환은 cvttss2si와
// 같다(범위 밖은 INT32_MIN → 0).
int FloatToInt(float a, int limit) noexcept {
    int i = TruncateToInt(a + 0.5f);
    if (i < 0) {
        i = 0;
    } else if (i > limit) {
        i = limit;
    }

    return i;
}

int FloatTo565(const Lane4& colour) noexcept {
    const int r = FloatToInt(Grid5 * colour.v[0], Max5);
    const int g = FloatToInt(Grid6 * colour.v[1], Max6);
    const int b = FloatToInt(Grid5 * colour.v[2], Max5);
    return (r << 11) | (g << 5) | b;
}

void WriteColourBlock(int a, int b, const uint8 (&indices)[TextureBlockTexels],
                      uint8* block) noexcept {
    block[0] = static_cast<uint8>(a & 0xFF);
    block[1] = static_cast<uint8>(a >> 8);
    block[2] = static_cast<uint8>(b & 0xFF);
    block[3] = static_cast<uint8>(b >> 8);
    for (uint32 row = 0; row < TextureBlockSize; ++row) {
        const uint8* ind = indices + row * TextureBlockSize;
        block[4 + row] = static_cast<uint8>(ind[0] | (ind[1] << 2) | (ind[2] << 4) | (ind[3] << 6));
    }
}

// WriteColourBlock3(colourblock.cpp 73–103): a > b면 맞바꾸고 index 0·1을 바꾼다.
void WriteColourBlock3(const Lane4& start, const Lane4& end,
                       const uint8 (&indices)[TextureBlockTexels], uint8* block) noexcept {
    int a = FloatTo565(start);
    int b = FloatTo565(end);
    uint8 remapped[TextureBlockTexels];
    if (a <= b) {
        for (uint32 i = 0; i < TextureBlockTexels; ++i) {
            remapped[i] = indices[i];
        }
    } else {
        const int t = a;
        a = b;
        b = t;
        for (uint32 i = 0; i < TextureBlockTexels; ++i) {
            remapped[i] = indices[i] == 0 ? 1 : (indices[i] == 1 ? 0 : indices[i]);
        }
    }

    WriteColourBlock(a, b, remapped, block);
}

// WriteColourBlock4(colourblock.cpp 105–135): a < b면 맞바꾸고 index ^ 1, 같으면 index 모두 0.
void WriteColourBlock4(const Lane4& start, const Lane4& end,
                       const uint8 (&indices)[TextureBlockTexels], uint8* block) noexcept {
    int a = FloatTo565(start);
    int b = FloatTo565(end);
    uint8 remapped[TextureBlockTexels];
    if (a < b) {
        const int t = a;
        a = b;
        b = t;
        for (uint32 i = 0; i < TextureBlockTexels; ++i) {
            remapped[i] = static_cast<uint8>((indices[i] ^ 0x1) & 0x3);
        }
    } else if (a == b) {
        for (uint32 i = 0; i < TextureBlockTexels; ++i) {
            remapped[i] = 0;
        }
    } else {
        for (uint32 i = 0; i < TextureBlockTexels; ++i) {
            remapped[i] = indices[i];
        }
    }

    WriteColourBlock(a, b, remapped, block);
}

// 정렬된 cluster index를 texel index로 옮긴다(ColourSet::RemapIndices — 투명 texel은 없다).
void RemapIndices(const ClusterFit& fit, const uint8 (&sorted)[TextureBlockTexels],
                  uint8 (&out)[TextureBlockTexels]) noexcept {
    uint8 ordered[TextureBlockTexels] = {};
    for (int i = 0; i < fit.colours->count; ++i) {
        ordered[fit.order[i]] = sorted[i];
    }

    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        out[i] = ordered[fit.colours->remap[i]];
    }
}

// 격자로 자른 끝점 하나다(clamp 0…1 → grid·a + 0.5 절사 → ÷ grid).
Lane4 ClampToGrid(const Lane4& value) noexcept {
    const Lane4 one = Splat(1.0f);
    const Lane4 zero = Splat(0.0f);
    const Lane4 half = Splat(0.5f);
    const Lane4 grid{{Grid5, Grid6, Grid5, 0.0f}};
    const Lane4 gridrcp{{1.0f / Grid5, 1.0f / Grid6, 1.0f / Grid5, 0.0f}};
    Lane4 a = Min(one, Max(zero, value));
    a = Mul(Truncate(MultiplyAdd(grid, a, half)), gridrcp);
    return a;
}

// 대리 오차(xxsum 상수는 뺀다, metric 1·1·1 — W lane은 metric² 0)다. 세 lane 합을 모든 lane에 편
// 값의 첫 lane이다.
float ClusterError(const Lane4& a, const Lane4& b, const Lane4& alphax, const Lane4& alpha2,
                   const Lane4& betax, const Lane4& beta2, const Lane4& alphabeta) noexcept {
    const Lane4 two = Splat(2.0f);
    const Lane4 metricSqr{{1.0f, 1.0f, 1.0f, 0.0f}};
    const Lane4 e1 = MultiplyAdd(Mul(a, a), alpha2, Mul(Mul(b, b), beta2));
    const Lane4 e2 = NegativeMultiplySubtract(a, alphax, Mul(Mul(a, b), alphabeta));
    const Lane4 e3 = NegativeMultiplySubtract(b, betax, e2);
    const Lane4 e4 = MultiplyAdd(two, e3, e1);
    const Lane4 e5 = Mul(e4, metricSqr);
    const float xy = e5.v[0] + e5.v[1];
    return xy + e5.v[2];
}

// WeightedClusterFit::Compress3 SIMD판(weightedclusterfit.cpp 441–546)이다.
void Compress3(ClusterFit& fit, uint8* block) noexcept {
    const int count = fit.colours->count;
    const Lane4 zero = Splat(0.0f);
    const Lane4 half{{0.5f, 0.5f, 0.5f, 0.25f}};
    Lane4 beststart = zero;
    Lane4 bestend = zero;
    float besterror = NumericLimits<float>::Max();
    Lane4 x0 = zero;
    int b0 = 0;
    int b1 = 0;
    for (int c0 = 0; c0 <= count; ++c0) {
        Lane4 x1 = zero;
        for (int c1 = 0; c1 <= count - c0; ++c1) {
            const Lane4 x2 = Sub(Sub(fit.xsum, x1), x0);
            const Lane4 alphax = MultiplyAdd(x1, half, x0);
            const Lane4 alpha2 = SplatLane(alphax, 3);
            const Lane4 betax = MultiplyAdd(x1, half, x2);
            const Lane4 beta2 = SplatLane(betax, 3);
            const Lane4 alphabeta = SplatLane(Mul(x1, half), 3);
            const Lane4 factor =
                Reciprocal(NegativeMultiplySubtract(alphabeta, alphabeta, Mul(alpha2, beta2)));
            Lane4 a = Mul(NegativeMultiplySubtract(betax, alphabeta, Mul(alphax, beta2)), factor);
            Lane4 b = Mul(NegativeMultiplySubtract(alphax, alphabeta, Mul(betax, alpha2)), factor);
            a = ClampToGrid(a);
            b = ClampToGrid(b);
            const float error = ClusterError(a, b, alphax, alpha2, betax, beta2, alphabeta);
            if (error < besterror) {
                besterror = error;
                beststart = a;
                bestend = b;
                b0 = c0;
                b1 = c1;
            }

            if (c0 + c1 < count) {
                x1 = Add(x1, fit.weighted[c0 + c1]);
            }
        }

        if (c0 < count) {
            x0 = Add(x0, fit.weighted[c0]);
        }
    }

    if (besterror < fit.bestError.v[0]) {
        uint8 sorted[TextureBlockTexels] = {};
        int i = 0;
        for (; i < b0; ++i) {
            sorted[i] = 0;
        }

        for (; i < b0 + b1; ++i) {
            sorted[i] = 2;
        }

        for (; i < count; ++i) {
            sorted[i] = 1;
        }

        uint8 indices[TextureBlockTexels];
        RemapIndices(fit, sorted, indices);
        WriteColourBlock3(beststart, bestend, indices, block);
        fit.bestError = Splat(besterror);
    }
}

// WeightedClusterFit::Compress4 SIMD판(weightedclusterfit.cpp 548–676)이다.
void Compress4(ClusterFit& fit, uint8* block) noexcept {
    const int count = fit.colours->count;
    const Lane4 zero = Splat(0.0f);
    const Lane4 onethird{{1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 9.0f}};
    const Lane4 twothirds{{2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f, 4.0f / 9.0f}};
    const Lane4 twonineths = Splat(2.0f / 9.0f);
    Lane4 beststart = zero;
    Lane4 bestend = zero;
    float besterror = NumericLimits<float>::Max();
    Lane4 x0 = zero;
    int b0 = 0;
    int b1 = 0;
    int b2 = 0;
    for (int c0 = 0; c0 <= count; ++c0) {
        Lane4 x1 = zero;
        for (int c1 = 0; c1 <= count - c0; ++c1) {
            Lane4 x2 = zero;
            for (int c2 = 0; c2 <= count - c0 - c1; ++c2) {
                const Lane4 x3 = Sub(Sub(Sub(fit.xsum, x2), x1), x0);
                const Lane4 alphax = MultiplyAdd(x2, onethird, MultiplyAdd(x1, twothirds, x0));
                const Lane4 alpha2 = SplatLane(alphax, 3);
                const Lane4 betax = MultiplyAdd(x2, twothirds, MultiplyAdd(x1, onethird, x3));
                const Lane4 beta2 = SplatLane(betax, 3);
                const Lane4 alphabeta = Mul(twonineths, SplatLane(Add(x1, x2), 3));
                const Lane4 factor =
                    Reciprocal(NegativeMultiplySubtract(alphabeta, alphabeta, Mul(alpha2, beta2)));
                Lane4 a =
                    Mul(NegativeMultiplySubtract(betax, alphabeta, Mul(alphax, beta2)), factor);
                Lane4 b =
                    Mul(NegativeMultiplySubtract(alphax, alphabeta, Mul(betax, alpha2)), factor);
                a = ClampToGrid(a);
                b = ClampToGrid(b);
                const float error = ClusterError(a, b, alphax, alpha2, betax, beta2, alphabeta);
                if (error < besterror) {
                    besterror = error;
                    beststart = a;
                    bestend = b;
                    b0 = c0;
                    b1 = c1;
                    b2 = c2;
                }

                if (c0 + c1 + c2 < count) {
                    x2 = Add(x2, fit.weighted[c0 + c1 + c2]);
                }
            }

            if (c0 + c1 < count) {
                x1 = Add(x1, fit.weighted[c0 + c1]);
            }
        }

        if (c0 < count) {
            x0 = Add(x0, fit.weighted[c0]);
        }
    }

    if (besterror < fit.bestError.v[0]) {
        uint8 sorted[TextureBlockTexels] = {};
        int i = 0;
        for (; i < b0; ++i) {
            sorted[i] = 0;
        }

        for (; i < b0 + b1; ++i) {
            sorted[i] = 2;
        }

        for (; i < b0 + b1 + b2; ++i) {
            sorted[i] = 3;
        }

        for (; i < count; ++i) {
            sorted[i] = 1;
        }

        uint8 indices[TextureBlockTexels];
        RemapIndices(fit, sorted, indices);
        WriteColourBlock4(beststart, bestend, indices, block);
        fit.bestError = Splat(besterror);
    }
}

// ColorBlock::isSingleColor(ColorBlock.cpp 114–128 — 알파는 보지 않는다)다.
bool IsSingleColor(const uint8 (&texels)[TextureBlockRgbaBytes]) noexcept {
    for (uint32 i = 1; i < TextureBlockTexels; ++i) {
        const uint8* texel = texels + i * TextureBlockTexelBytes;
        if (texel[0] != texels[0] || texel[1] != texels[1] || texel[2] != texels[2]) {
            return false;
        }
    }

    return true;
}

// OptimalCompress::compressDXT1(Color32)(OptimalCompressDXT.cpp 189–204)다.
void CompressSingleColor(const uint8* texel, uint8* block) noexcept {
    int col0 = (SingleColor5[texel[0]][0] << 11) | (SingleColor6[texel[1]][0] << 5) |
               SingleColor5[texel[2]][0];
    int col1 = (SingleColor5[texel[0]][1] << 11) | (SingleColor6[texel[1]][1] << 5) |
               SingleColor5[texel[2]][1];
    uint32 indices = SingleColorIndices;
    if (col0 < col1) {
        const int t = col0;
        col0 = col1;
        col1 = t;
        indices ^= SingleColorFlip;
    }

    block[0] = static_cast<uint8>(col0 & 0xFF);
    block[1] = static_cast<uint8>(col0 >> 8);
    block[2] = static_cast<uint8>(col1 & 0xFF);
    block[3] = static_cast<uint8>(col1 >> 8);
    for (uint32 i = 0; i < 4; ++i) {
        block[4 + i] = static_cast<uint8>((indices >> (8 * i)) & 0xFF);
    }
}

// BC1·BC3의 색 블록 8 byte다.
void CompressColour(const uint8 (&texels)[TextureBlockRgbaBytes], bool bc3, uint8* block) noexcept {
    if (IsSingleColor(texels)) {
        CompressSingleColor(texels, block);
        return;
    }

    ColourSet set;
    BuildColourSet(texels, !bc3, bc3, set);
    ClusterFit fit;
    SetColourSet(set, fit);
    if (!bc3) {
        // kDxt1: 3색 뒤 4색(투명 texel이 없으므로 4색도 본다 — colourfit.cpp 48–63).
        Compress3(fit, block);
    }

    Compress4(fit, block);
}

// NVTT AlphaBlockDXT5(64비트: alpha0 8, alpha1 8, index 3비트 × 16)다.
struct AlphaBlock {
    uint64 u = 0;

    uint32 Alpha0() const noexcept {
        return static_cast<uint32>(u & 0xFF);
    }

    uint32 Alpha1() const noexcept {
        return static_cast<uint32>((u >> 8) & 0xFF);
    }

    void SetEndpoints(uint32 alpha0, uint32 alpha1) noexcept {
        u = (u & ~uint64{0xFFFF}) | uint64{alpha0 & 0xFF} | (uint64{alpha1 & 0xFF} << 8);
    }

    uint32 Index(uint32 i) const noexcept {
        return static_cast<uint32>((u >> (AlphaIndexShift + AlphaIndexBits * i)) & AlphaIndexMask);
    }

    void SetIndex(uint32 i, uint32 value) noexcept {
        const uint32 shift = AlphaIndexShift + AlphaIndexBits * i;
        u = (u & ~(AlphaIndexMask << shift)) | (uint64{value & AlphaIndexMask} << shift);
    }
};

// AlphaBlockDXT5::evaluatePalette(BlockDXT.cpp 343–380 — 바닥 나눗셈)다.
void AlphaPalette(const AlphaBlock& block, uint8 (&alpha)[AlphaPaletteSize]) noexcept {
    const uint32 a0 = block.Alpha0();
    const uint32 a1 = block.Alpha1();
    alpha[0] = static_cast<uint8>(a0);
    alpha[1] = static_cast<uint8>(a1);
    if (a0 > a1) {
        for (uint32 k = 1; k <= 6; ++k) {
            alpha[1 + k] = static_cast<uint8>(((7 - k) * a0 + k * a1) / 7);
        }
    } else {
        for (uint32 k = 1; k <= 4; ++k) {
            alpha[1 + k] = static_cast<uint8>(((5 - k) * a0 + k * a1) / 5);
        }

        alpha[6] = 0x00;
        alpha[7] = 0xFF;
    }
}

// computeAlphaIndices(QuickCompressDXT.cpp 294–325)다.
uint32 ComputeAlphaIndices(const uint8 (&values)[TextureBlockTexels], AlphaBlock& block) noexcept {
    uint8 alphas[AlphaPaletteSize];
    AlphaPalette(block, alphas);
    uint32 total = 0;
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        uint32 besterror = 256 * 256;
        uint32 best = AlphaPaletteSize;
        for (uint32 p = 0; p < AlphaPaletteSize; ++p) {
            const int d = static_cast<int>(alphas[p]) - static_cast<int>(values[i]);
            const uint32 error = static_cast<uint32>(d * d);
            if (error < besterror) {
                besterror = error;
                best = p;
            }
        }

        total += besterror;
        block.SetIndex(i, best);
    }

    return total;
}

// nv::max·nv::min(Containers.h 75–92 — `<` 비교, NaN은 첫 값을 그대로 넘긴다)다.
float NvMax(float a, float b) noexcept {
    return a < b ? b : a;
}

float NvMin(float a, float b) noexcept {
    return b < a ? b : a;
}

// uint(float) 변환이다. x64 NVTT 빌드는 64비트 절사의 하위 32비트를 쓰므로 NaN은 0이다. 값은 이미
// 0…255로 잘려 있다(12.17.4 1).
uint32 AlphaEndpoint(float value) noexcept {
    if (IsNan(value)) {
        return 0;
    }

    return static_cast<uint32>(value);
}

// optimizeAlpha8(QuickCompressDXT.cpp 327–378)이다.
void OptimizeAlpha8(const uint8 (&values)[TextureBlockTexels], AlphaBlock& block) noexcept {
    float alpha2Sum = 0.0f;
    float beta2Sum = 0.0f;
    float alphabetaSum = 0.0f;
    float alphaxSum = 0.0f;
    float betaxSum = 0.0f;
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        const uint32 idx = block.Index(i);
        const float alpha =
            idx < 2 ? 1.0f - static_cast<float>(idx) : (8.0f - static_cast<float>(idx)) / 7.0f;
        const float beta = 1.0f - alpha;
        alpha2Sum += alpha * alpha;
        beta2Sum += beta * beta;
        alphabetaSum += alpha * beta;
        alphaxSum += alpha * static_cast<float>(values[i]);
        betaxSum += beta * static_cast<float>(values[i]);
    }

    const float denominator = alpha2Sum * beta2Sum - alphabetaSum * alphabetaSum;
    const float factor = 1.0f / denominator;
    const float a = (alphaxSum * beta2Sum - betaxSum * alphabetaSum) * factor;
    const float b = (betaxSum * alpha2Sum - alphaxSum * alphabetaSum) * factor;
    uint32 alpha0 = AlphaEndpoint(NvMin(NvMax(a, 0.0f), ByteMaxFloat));
    uint32 alpha1 = AlphaEndpoint(NvMin(NvMax(b, 0.0f), ByteMaxFloat));
    if (alpha0 < alpha1) {
        const uint32 t = alpha0;
        alpha0 = alpha1;
        alpha1 = t;
        for (uint32 i = 0; i < TextureBlockTexels; ++i) {
            const uint32 idx = block.Index(i);
            block.SetIndex(i, idx < 2 ? 1 - idx : 9 - idx);
        }
    } else if (alpha0 == alpha1) {
        for (uint32 i = 0; i < TextureBlockTexels; ++i) {
            block.SetIndex(i, 0);
        }
    }

    block.SetEndpoints(alpha0, alpha1);
}

bool SameIndices(const AlphaBlock& a, const AlphaBlock& b) noexcept {
    constexpr uint64 Mask = ~uint64{0xFFFF};
    return (a.u | Mask) == (b.u | Mask);
}

// QuickCompress::compressDXT5A(QuickCompressDXT.cpp 537–578)다. values는 압축할 채널 16개다.
void CompressAlpha(const uint8 (&values)[TextureBlockTexels], uint8* out) noexcept {
    uint32 alpha0 = 0;
    uint32 alpha1 = 255;
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        alpha0 = values[i] > alpha0 ? values[i] : alpha0;
        alpha1 = values[i] < alpha1 ? values[i] : alpha1;
    }

    AlphaBlock block;
    const int inset = (static_cast<int>(alpha0) - static_cast<int>(alpha1)) / AlphaInset;
    block.SetEndpoints(static_cast<uint32>(static_cast<int>(alpha0) - inset),
                       static_cast<uint32>(static_cast<int>(alpha1) + inset));
    uint32 besterror = ComputeAlphaIndices(values, block);
    AlphaBlock bestblock = block;
    for (int i = 0; i < AlphaIterations; ++i) {
        OptimizeAlpha8(values, block);
        const uint32 error = ComputeAlphaIndices(values, block);
        if (error >= besterror) {
            break;
        }

        if (SameIndices(block, bestblock)) {
            bestblock = block;
            break;
        }

        besterror = error;
        bestblock = block;
    }

    for (uint32 i = 0; i < SmallBlockBytes; ++i) {
        out[i] = static_cast<uint8>((bestblock.u >> (8 * i)) & 0xFF);
    }
}

void ChannelValues(const uint8 (&texels)[TextureBlockRgbaBytes], uint32 channel,
                   uint8 (&values)[TextureBlockTexels]) noexcept {
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        values[i] = texels[i * TextureBlockTexelBytes + channel];
    }
}

uint64 ReadLittle64(const uint8* bytes) noexcept {
    uint64 value = 0;
    for (uint32 i = 0; i < SmallBlockBytes; ++i) {
        value |= uint64{bytes[i]} << (8 * i);
    }

    return value;
}

// 알파 블록 하나를 16 값으로 푼다(AlphaBlockDXT5::decodeBlock).
void DecodeAlpha(const uint8* bytes, uint8 (&values)[TextureBlockTexels]) noexcept {
    AlphaBlock block;
    block.u = ReadLittle64(bytes);
    uint8 palette[AlphaPaletteSize];
    AlphaPalette(block, palette);
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        values[i] = palette[block.Index(i)];
    }
}

// 색 블록 하나를 RGBA로 푼다(BlockDXT1::decodeBlock — 끝점 비교로 4색·3색).
void DecodeColour(const uint8* bytes, uint8 (&texels)[TextureBlockRgbaBytes]) noexcept {
    const uint32 col0 = uint32{bytes[0]} | (uint32{bytes[1]} << 8);
    const uint32 col1 = uint32{bytes[2]} | (uint32{bytes[3]} << 8);
    const auto expand = [](uint32 c, uint8(&rgb)[3]) noexcept {
        const uint32 r = (c >> 11) & 0x1F;
        const uint32 g = (c >> 5) & 0x3F;
        const uint32 b = c & 0x1F;
        rgb[0] = static_cast<uint8>((r << 3) | (r >> 2));
        rgb[1] = static_cast<uint8>((g << 2) | (g >> 4));
        rgb[2] = static_cast<uint8>((b << 3) | (b >> 2));
    };
    uint8 palette[4][4] = {};
    uint8 c0[3];
    uint8 c1[3];
    expand(col0, c0);
    expand(col1, c1);
    for (uint32 k = 0; k < 3; ++k) {
        palette[0][k] = c0[k];
        palette[1][k] = c1[k];
        if (col0 > col1) {
            palette[2][k] = static_cast<uint8>((2 * c0[k] + c1[k]) / 3);
            palette[3][k] = static_cast<uint8>((2 * c1[k] + c0[k]) / 3);
        } else {
            palette[2][k] = static_cast<uint8>((c0[k] + c1[k]) / 2);
            palette[3][k] = 0;
        }
    }

    palette[0][3] = 0xFF;
    palette[1][3] = 0xFF;
    palette[2][3] = 0xFF;
    palette[3][3] = col0 > col1 ? 0xFF : 0x00;
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        const uint32 index = (bytes[4 + i / 4] >> (2 * (i % 4))) & 0x3;
        for (uint32 k = 0; k < 4; ++k) {
            texels[i * TextureBlockTexelBytes + k] = palette[index][k];
        }
    }
}

} // namespace bc_encode

using bc_encode::ChannelValues;
using bc_encode::CompressAlpha;
using bc_encode::CompressColour;
using bc_encode::DecodeAlpha;
using bc_encode::DecodeColour;
using bc_encode::LargeBlockBytes;
using bc_encode::SmallBlockBytes;

} // namespace

uint32 TextureBlockBytes(TextureBlockFormat format) noexcept {
    switch (format) {
    case TextureBlockFormat::Bc1:
    case TextureBlockFormat::Bc4:
        return SmallBlockBytes;
    case TextureBlockFormat::Bc3:
    case TextureBlockFormat::Bc5:
        return LargeBlockBytes;
    }

    return 0;
}

Result<uint64> TextureBlockOutputBytes(TextureBlockFormat format, uint32 sizeX, uint32 sizeY,
                                       uint32 numSlices) noexcept {
    if (sizeX == 0 || sizeY == 0 || numSlices == 0 || TextureBlockBytes(format) == 0) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 blocksX = (uint64{sizeX} + TextureBlockSize - 1) / TextureBlockSize;
    const uint64 blocksY = (uint64{sizeY} + TextureBlockSize - 1) / TextureBlockSize;
    // blocksX·blocksY는 2^30 이하라 둘의 곱은 넘치지 않는다. 블록 byte·slice를 곱할 때 넘칠 수
    // 있다.
    const uint64 blocks = blocksX * blocksY;
    const uint64 blockBytes = TextureBlockBytes(format);
    if (blocks > NumericLimits<uint64>::Max() / blockBytes) {
        return Result<uint64>::Failure(ErrorCode::CapacityExceeded);
    }

    const uint64 perSlice = blocks * blockBytes;
    if (perSlice > NumericLimits<uint64>::Max() / numSlices) {
        return Result<uint64>::Failure(ErrorCode::CapacityExceeded);
    }

    return Result<uint64>::Success(perSlice * numSlices);
}

void EncodeTextureBlock(TextureBlockFormat format, const uint8 (&texels)[TextureBlockRgbaBytes],
                        uint8* out) noexcept {
    switch (format) {
    case TextureBlockFormat::Bc1:
        CompressColour(texels, false, out);
        break;
    case TextureBlockFormat::Bc3: {
        uint8 alpha[TextureBlockTexels];
        ChannelValues(texels, 3, alpha);
        CompressAlpha(alpha, out);
        CompressColour(texels, true, out + SmallBlockBytes);
        break;
    }
    case TextureBlockFormat::Bc4: {
        uint8 red[TextureBlockTexels];
        ChannelValues(texels, 0, red);
        CompressAlpha(red, out);
        break;
    }
    case TextureBlockFormat::Bc5: {
        uint8 red[TextureBlockTexels];
        uint8 green[TextureBlockTexels];
        ChannelValues(texels, 0, red);
        ChannelValues(texels, 1, green);
        CompressAlpha(red, out);
        CompressAlpha(green, out + SmallBlockBytes);
        break;
    }
    }
}

void DecodeTextureBlock(TextureBlockFormat format, const uint8* block,
                        uint8 (&texels)[TextureBlockRgbaBytes]) noexcept {
    uint8 first[TextureBlockTexels];
    uint8 second[TextureBlockTexels];
    switch (format) {
    case TextureBlockFormat::Bc1:
        DecodeColour(block, texels);
        break;
    case TextureBlockFormat::Bc3:
        DecodeColour(block + SmallBlockBytes, texels);
        DecodeAlpha(block, first);
        for (uint32 i = 0; i < TextureBlockTexels; ++i) {
            texels[i * TextureBlockTexelBytes + 3] = first[i];
        }

        break;
    case TextureBlockFormat::Bc4:
        DecodeAlpha(block, first);
        for (uint32 i = 0; i < TextureBlockTexels; ++i) {
            uint8* texel = texels + i * TextureBlockTexelBytes;
            texel[0] = first[i];
            texel[1] = 0;
            texel[2] = 0;
            texel[3] = 0xFF;
        }

        break;
    case TextureBlockFormat::Bc5:
        DecodeAlpha(block, first);
        DecodeAlpha(block + SmallBlockBytes, second);
        for (uint32 i = 0; i < TextureBlockTexels; ++i) {
            uint8* texel = texels + i * TextureBlockTexelBytes;
            texel[0] = first[i];
            texel[1] = second[i];
            texel[2] = 0;
            texel[3] = 0xFF;
        }

        break;
    }
}

Result<void> EncodeTextureBlocks(TextureBlockFormat format, const TextureBlockImage& image,
                                 Span<uint8> out) noexcept {
    auto outputBytes = TextureBlockOutputBytes(format, image.sizeX, image.sizeY, image.numSlices);
    if (!outputBytes) {
        return Result<void>::Failure(outputBytes.Error());
    }

    constexpr uint64 Max = NumericLimits<uint64>::Max();
    const uint64 rowBytes = uint64{image.sizeX} * TextureBlockTexelBytes;
    const uint64 rowPitch = image.rowPitch == 0 ? rowBytes : uint64{image.rowPitch};
    if (rowPitch < rowBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // slice 하나가 실제로 닿는 byte 수다(마지막 행은 pitch 여백이 없어도 된다).
    const uint64 lastRow = image.sizeY - 1;
    if (lastRow != 0 && rowPitch > (Max - rowBytes) / lastRow) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    const uint64 sliceExtent = rowPitch * lastRow + rowBytes;
    uint64 slicePitch = image.slicePitch;
    if (slicePitch == 0) {
        if (rowPitch > Max / image.sizeY) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        slicePitch = rowPitch * image.sizeY;
    }

    if (slicePitch < sliceExtent) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 lastSlice = image.numSlices - 1;
    if (lastSlice != 0 && slicePitch > (Max - sliceExtent) / lastSlice) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    const uint64 needed = slicePitch * lastSlice + sliceExtent;
    if (image.rgba.Size() < needed || out.Size() != outputBytes.Value()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // NVTT ColorBlock::init의 가장자리 remainder 표(ColorBlock.cpp 63–68)다.
    constexpr uint32 Remainder[TextureBlockSize][TextureBlockSize] = {
        {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 0}, {0, 1, 2, 3}};
    const uint32 blockBytes = TextureBlockBytes(format);
    uint8* cursor = out.Data();
    for (uint32 slice = 0; slice < image.numSlices; ++slice) {
        const uint8* base = image.rgba.Data() + slicePitch * slice;
        for (uint32 y = 0; y < image.sizeY; y += TextureBlockSize) {
            const uint32 bh =
                image.sizeY - y < TextureBlockSize ? image.sizeY - y : TextureBlockSize;
            for (uint32 x = 0; x < image.sizeX; x += TextureBlockSize) {
                const uint32 bw =
                    image.sizeX - x < TextureBlockSize ? image.sizeX - x : TextureBlockSize;
                uint8 texels[TextureBlockRgbaBytes];
                for (uint32 row = 0; row < TextureBlockSize; ++row) {
                    const uint32 by = Remainder[bh - 1][row];
                    const uint8* line = base + rowPitch * (y + by);
                    for (uint32 column = 0; column < TextureBlockSize; ++column) {
                        const uint32 bx = Remainder[bw - 1][column];
                        const uint8* texel = line + uint64{x + bx} * TextureBlockTexelBytes;
                        uint8* target =
                            texels + (row * TextureBlockSize + column) * TextureBlockTexelBytes;
                        for (uint32 k = 0; k < TextureBlockTexelBytes; ++k) {
                            target[k] = texel[k];
                        }
                    }
                }

                EncodeTextureBlock(format, texels, cursor);
                cursor += blockBytes;
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks
