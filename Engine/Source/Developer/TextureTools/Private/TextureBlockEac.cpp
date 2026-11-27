#include "TextureBlockEacAstc.h"

#include <Base/NumericLimits.h>
#include <Platform/PlatformMath.h>
#include <TextureTools/TextureBlockEncode.h>

// EAC R11 인코더다(Docs/Design/AssetSystem 12.17.8 4). 블록마다 (표, multiplier, base)를 정확히
// 최적으로 고른다: 목적은 오차에 넣는 texel의 (목표 − 디코드)² 합이고, 동률은 (오차, 표,
// multiplier, base) 사전순으로 작은 것이다. 그래서 방문 순서(첫 후보·가지치기)와 무관하게 답이
// 하나다.
namespace rocks {

namespace {

// 유니티 빌드에서 같은 묶음의 이름과 부딪히지 않게 이름 있는 namespace에 둔다.
namespace eac_encode {

constexpr uint32 BlockBytes = 8;
constexpr uint32 ByteBits = 8;
constexpr uint32 MultiplierCount = 16;
constexpr int32 BaseMax = 255;
// multiplier 하나의 base 간격(8)이다(base·8 + 4).
constexpr int32 BaseStep = 8;
constexpr int32 BaseOffset = 4;
// 16 bit unorm → 11 bit unorm 반올림의 분모·반이다.
constexpr uint32 Unorm16Max = 65535;
constexpr uint32 Unorm16Half = 32767;
// 블록 비트 배치다(TextureBlockDecode.cpp와 같다).
constexpr uint32 BaseShift = 56;
constexpr uint32 MultiplierShift = 52;
constexpr uint32 TableShift = 48;
constexpr uint32 IndexTopShift = 45;
constexpr uint32 IndexBits = 3;
constexpr uint16 AllTexels = 0xFFFF;

struct Candidate {
    uint64 error = NumericLimits<uint64>::Max();
    uint32 table = 0;
    uint32 multiplier = 0;
    uint32 base = 0;
};

// (표, multiplier, base) 사전순 키다.
uint32 KeyOf(uint32 table, uint32 multiplier, uint32 base) noexcept {
    return (table << 12) | (multiplier << 8) | base;
}

int32 FloorDiv(int32 value, int32 divisor) noexcept {
    const int32 quotient = value / divisor;
    return (value % divisor != 0 && value < 0) ? quotient - 1 : quotient;
}

int32 CeilDiv(int32 value, int32 divisor) noexcept {
    const int32 quotient = value / divisor;
    return (value % divisor != 0 && value > 0) ? quotient + 1 : quotient;
}

void Palette(uint32 table, uint32 multiplier, uint32 base,
             uint32 (&palette)[TextureEacTableSize]) noexcept {
    for (uint32 k = 0; k < TextureEacTableSize; ++k) {
        palette[k] = TextureEacR11Value(base, multiplier, TextureEacModifiers[table][k]);
    }
}

// 값 하나의 가장 가까운 팔레트 index다(동률은 작은 index).
uint32 Nearest(uint32 value, const uint32 (&palette)[TextureEacTableSize], uint32& error) noexcept {
    uint32 best = 0;
    uint32 bestError = NumericLimits<uint32>::Max();
    for (uint32 k = 0; k < TextureEacTableSize; ++k) {
        const int32 delta = static_cast<int32>(value) - static_cast<int32>(palette[k]);
        const uint32 squared = static_cast<uint32>(delta * delta);
        if (squared < bestError) {
            bestError = squared;
            best = k;
        }
    }

    error = bestError;
    return best;
}

// base 256개의 bit 집합이다.
struct BaseSet {
    static constexpr uint32 Words = 4;
    static constexpr uint32 WordBits = 64;
    uint64 words[Words] = {};

    // [low, high] ∩ [0, 255]를 켠다(word 단위).
    void Add(int32 low, int32 high) noexcept {
        low = low < 0 ? 0 : low;
        high = high > BaseMax ? BaseMax : high;
        if (low > high) {
            return;
        }

        const uint32 first = static_cast<uint32>(low) / WordBits;
        const uint32 last = static_cast<uint32>(high) / WordBits;
        for (uint32 word = first; word <= last; ++word) {
            const uint32 from = word == first ? static_cast<uint32>(low) % WordBits : 0;
            const uint32 to = word == last ? static_cast<uint32>(high) % WordBits : WordBits - 1;
            const uint64 upper =
                to == WordBits - 1 ? NumericLimits<uint64>::Max() : (uint64{1} << (to + 1)) - 1;
            words[word] |= upper & ~((uint64{1} << from) - 1);
        }
    }

    void Intersect(const BaseSet& other) noexcept {
        for (uint32 i = 0; i < Words; ++i) {
            words[i] &= other.words[i];
        }
    }

    bool IsEmpty() const noexcept {
        return (words[0] | words[1] | words[2] | words[3]) == 0;
    }
};

// floor(sqrt(value))다(정수 이분 탐색 — 부동소수를 쓰지 않는다). 11 bit 거리 상한 2047에서
// 자른다(그보다 크면 모든 base가 후보다).
uint32 FloorSqrt(uint64 value) noexcept {
    uint32 low = 0;
    uint32 high = TextureEacR11Max;
    while (low < high) {
        const uint32 middle = (low + high + 1) / 2;
        if (uint64{middle} * middle <= value) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }

    return low;
}

// texel 값 v의 오차가 radius² 이하인 base 전부다(정확 — 팔레트 값 x = base·8 + 4 + modifier·scale은
// [0, 2047]로 잘리므로, 잘리지 않은 값은 |v − x| ≤ radius 구간, 0으로 잘린 값은 v ≤ radius일 때,
// 2047로 잘린 값은 2047 − v ≤ radius일 때 넣는다).
void Reachable(uint32 value, uint32 table, int32 scale, uint32 radius, BaseSet& out) noexcept {
    const int32 v = static_cast<int32>(value);
    const int32 r = static_cast<int32>(radius);
    for (uint32 k = 0; k < TextureEacTableSize; ++k) {
        const int32 offset = BaseOffset + TextureEacModifiers[table][k] * scale;
        out.Add(CeilDiv(v - r - offset, BaseStep), FloorDiv(v + r - offset, BaseStep));
        if (v <= r) {
            // x ≤ −1 ⇔ base ≤ floor((−1 − offset) / 8).
            out.Add(0, FloorDiv(-1 - offset, BaseStep));
        }

        if (static_cast<int32>(TextureEacR11Max) - v <= r) {
            // x ≥ 2048 ⇔ base ≥ ceil((2048 − offset) / 8).
            out.Add(CeilDiv(static_cast<int32>(TextureEacR11Max) + 1 - offset, BaseStep), BaseMax);
        }
    }
}

struct Search {
    // 오차에 넣는 texel의 서로 다른 11 bit 목표값과 그 texel 수다(가지치기가 일찍 끝나게
    // 최솟값·최댓값이 앞이다).
    uint32 values[TextureBlockTexels] = {};
    uint32 weights[TextureBlockTexels] = {};
    uint32 count = 0;
    uint32 minimum = 0;
    uint32 maximum = 0;
    Candidate best{};
    uint32 bestKey = 0;

    // 후보 하나를 재고 사전순으로 더 나으면 바꾼다. 부분합이 최선보다 크거나, 같고 키가 크면
    // 그만둔다.
    void Try(uint32 table, uint32 multiplier, uint32 base) noexcept {
        uint32 palette[TextureEacTableSize];
        Palette(table, multiplier, base, palette);
        const uint32 key = KeyOf(table, multiplier, base);
        const bool laterKey = key > bestKey;
        uint64 error = 0;
        for (uint32 i = 0; i < count; ++i) {
            uint32 texelError = 0;
            Nearest(values[i], palette, texelError);
            error += uint64{texelError} * weights[i];
            if (error > best.error || (error == best.error && laterKey)) {
                return;
            }
        }

        if (error < best.error || key < bestKey) {
            best = Candidate{error, table, multiplier, base};
            bestKey = key;
        }
    }

    // 표·multiplier 하나의 base 구간 전부다(12.17.8 4 — 구간 밖 base는 최적이 될 수 없다).
    void TryBases(uint32 table, uint32 multiplier) noexcept {
        const int32 scale = multiplier == 0 ? 1 : static_cast<int32>(multiplier) * BaseStep;
        int32 smallest = TextureEacModifiers[table][0];
        int32 largest = TextureEacModifiers[table][0];
        for (uint32 k = 1; k < TextureEacTableSize; ++k) {
            const int32 modifier = TextureEacModifiers[table][k];
            smallest = modifier < smallest ? modifier : smallest;
            largest = modifier > largest ? modifier : largest;
        }

        // 팔레트 위끝이 0으로 잘리는 low(8·low + 4 + 가장 큰 수정값·scale ≤ 0)는 늘 음수라(그
        // 수정값·scale > 0) BaseSet::Add가 0부터로 자른다 — 그 아래 base는 모두 같은 오차이고 가장
        // 작은 base(0)가 이긴다.
        const int32 low =
            FloorDiv(static_cast<int32>(minimum) - BaseOffset - largest * scale, BaseStep);

        int32 high = CeilDiv(static_cast<int32>(maximum) - BaseOffset - smallest * scale, BaseStep);
        // texel마다 현재 최선 오차 안에 드는 base만 남긴다(각 texel 오차 ≤ 블록 오차이므로 최선이
        // 될 수 있는 base는 모두 남는다 — 정확). 최솟값·최댓값 texel이 앞이라 집합이 일찍 줄어든다.
        BaseSet candidates;
        candidates.Add(low, high);
        const uint32 radius = FloorSqrt(best.error);
        for (uint32 i = 0; i < count && radius < TextureEacR11Max && !candidates.IsEmpty(); ++i) {
            BaseSet reach;
            Reachable(values[i], table, scale, radius, reach);
            candidates.Intersect(reach);
        }

        for (uint32 word = 0; word < BaseSet::Words; ++word) {
            uint64 bits = candidates.words[word];
            while (bits != 0) {
                const uint32 bit = platform::PlatformMath::CountTrailingZeros64(bits);
                bits &= bits - 1;
                Try(table, multiplier, word * BaseSet::WordBits + bit);
            }
        }
    }
};

void WriteBlock(const Candidate& choice, const uint32 (&targets)[TextureBlockTexels],
                uint8* out) noexcept {
    uint32 palette[TextureEacTableSize];
    Palette(choice.table, choice.multiplier, choice.base, palette);
    uint64 bits = (uint64{choice.base} << BaseShift) |
                  (uint64{choice.multiplier} << MultiplierShift) |
                  (uint64{choice.table} << TableShift);
    // index는 texel a(0,0), b(0,1) … 열 우선이다(명세 Table C.2).
    for (uint32 column = 0; column < TextureBlockSize; ++column) {
        for (uint32 row = 0; row < TextureBlockSize; ++row) {
            uint32 error = 0;
            const uint32 index = Nearest(targets[row * TextureBlockSize + column], palette, error);
            const uint32 order = column * TextureBlockSize + row;
            bits |= uint64{index} << (IndexTopShift - IndexBits * order);
        }
    }

    for (uint32 i = 0; i < BlockBytes; ++i) {
        out[i] = static_cast<uint8>(bits >> (ByteBits * (BlockBytes - 1 - i)));
    }
}

} // namespace eac_encode

} // namespace

void EncodeEacR11Block(const uint16 (&values)[TextureBlockTexels], uint16 validMask,
                       uint8* out) noexcept {
    using namespace eac_encode;
    const uint16 mask = validMask == 0 ? AllTexels : validMask;
    uint32 targets[TextureBlockTexels];
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        targets[i] = (uint32{values[i]} * TextureEacR11Max + Unorm16Half) / Unorm16Max;
    }

    Search search;
    uint32 minimumIndex = TextureBlockTexels;
    uint32 maximumIndex = TextureBlockTexels;
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        if ((mask & (1u << i)) == 0) {
            continue;
        }

        if (minimumIndex == TextureBlockTexels || targets[i] < targets[minimumIndex]) {
            minimumIndex = i;
        }

        if (maximumIndex == TextureBlockTexels || targets[i] > targets[maximumIndex]) {
            maximumIndex = i;
        }
    }

    search.minimum = targets[minimumIndex];
    search.maximum = targets[maximumIndex];
    // 같은 값은 하나로 모으고 texel 수를 가중치로 둔다(최솟값·최댓값이 앞).
    const auto add = [&search](uint32 value) noexcept {
        for (uint32 k = 0; k < search.count; ++k) {
            if (search.values[k] == value) {
                ++search.weights[k];
                return;
            }
        }

        search.values[search.count] = value;
        search.weights[search.count] = 1;
        ++search.count;
    };
    add(search.minimum);
    add(search.maximum);
    search.weights[0] = 0;
    if (search.count > 1) {
        search.weights[1] = 0;
    }

    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        if ((mask & (1u << i)) != 0) {
            add(targets[i]);
        }
    }

    // 첫 후보: 표마다 범위에 맞는 multiplier와 가운데 base다(최선을 일찍 줄이는 용도 — 답은 사전순
    // 규칙이 정한다).
    const int32 range = static_cast<int32>(search.maximum - search.minimum);
    const int32 middle = static_cast<int32>(search.minimum + search.maximum) / 2;
    // 표마다 index 3이 가장 작은 수정값, index 7이 가장 큰 수정값이다(Table C.10).
    constexpr uint32 SmallestIndex = 3;
    constexpr uint32 LargestIndex = 7;
    for (uint32 table = 0; table < TextureEacTableCount; ++table) {
        const int32 smallest = TextureEacModifiers[table][SmallestIndex];
        const int32 largest = TextureEacModifiers[table][LargestIndex];
        const int32 spread = largest - smallest;
        int32 multiplier = (range + spread * BaseStep / 2) / (spread * BaseStep);
        multiplier = multiplier > static_cast<int32>(MultiplierCount - 1)
                         ? static_cast<int32>(MultiplierCount - 1)
                         : multiplier;
        const int32 scale = multiplier == 0 ? 1 : multiplier * BaseStep;
        const int32 centre = (smallest + largest) * scale / 2;
        int32 base = (middle - BaseOffset - centre + BaseStep / 2) / BaseStep;
        base = base < 0 ? 0 : base > BaseMax ? BaseMax : base;
        search.Try(table, static_cast<uint32>(multiplier), static_cast<uint32>(base));
    }

    for (uint32 table = 0; table < TextureEacTableCount; ++table) {
        for (uint32 multiplier = 0; multiplier < MultiplierCount; ++multiplier) {
            search.TryBases(table, multiplier);
        }
    }

    WriteBlock(search.best, targets, out);
}

namespace texture_block_detail {

void EncodeEacBlocks(const uint8* base, uint32 sizeX, uint32 sizeY, uint32 numSlices,
                     uint64 rowPitch, uint64 slicePitch, Span<uint8> out) noexcept {
    // NVTT ColorBlock::init의 가장자리 remainder 표다(BC4와 같은 채움 — 채운 texel은 오차에 넣지
    // 않는다, 12.17.8 4).
    constexpr uint32 Remainder[TextureBlockSize][TextureBlockSize] = {
        {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 0}, {0, 1, 2, 3}};
    constexpr uint32 TexelBytes = 2;
    uint8* cursor = out.Data();
    for (uint32 slice = 0; slice < numSlices; ++slice) {
        const uint8* plane = base + slicePitch * slice;
        for (uint32 y = 0; y < sizeY; y += TextureBlockSize) {
            const uint32 bh = sizeY - y < TextureBlockSize ? sizeY - y : TextureBlockSize;
            for (uint32 x = 0; x < sizeX; x += TextureBlockSize) {
                const uint32 bw = sizeX - x < TextureBlockSize ? sizeX - x : TextureBlockSize;
                uint16 values[TextureBlockTexels];
                uint16 mask = 0;
                for (uint32 row = 0; row < TextureBlockSize; ++row) {
                    const uint8* line = plane + rowPitch * (y + Remainder[bh - 1][row]);
                    for (uint32 column = 0; column < TextureBlockSize; ++column) {
                        const uint8* texel =
                            line + uint64{x + Remainder[bw - 1][column]} * TexelBytes;
                        const uint32 i = row * TextureBlockSize + column;
                        values[i] = static_cast<uint16>(texel[0] | (texel[1] << 8));
                        if (row < bh && column < bw) {
                            mask = static_cast<uint16>(mask | (1u << i));
                        }
                    }
                }

                EncodeEacR11Block(values, mask, cursor);
                cursor += eac_encode::BlockBytes;
            }
        }
    }
}

} // namespace texture_block_detail

} // namespace rocks
