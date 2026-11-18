#include <Core/Memory/SystemAllocator.h>
#include <Curves/PackedTangentCurve.h>
#include <Curves/TangentCurve.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/MathTestSupport.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include "../Private/PackedTangentCurveTestAccess.h"
#include "CurvePackUeReference.h"

// PackedTangentCurve 시험이다(Docs/Design/CurveData.ko.md 11절·16절 — CV8):
// - UE 5.8 CompressCurve를 옮긴 참조(CurvePackUeReference.h — 머리 주석에 UE 줄 대조표)와 byte
//   대조: 같은 형식·시간 형식이면 packed byte가 같고, 형식이 다르면 15절 수정(구간 단위 가중·비트
//   같은 Constant)으로 설명되는 사례뿐이다.
// - Float32 시간 형식의 평가가 원본 TangentCurve와 모든 시간에서 비트가 같다(D14).
// - Quantized16 오차가 errorThreshold 안이다. 형식별 왕복, 빈·상수 커브, 손으로 쓴 byte 배치.
// - 정합이 깨진 값(D21)의 평가·풀기·검증 거부, 할당 실패의 원자성(주입 단언).
// 무작위 사례는 고정 seed다.
namespace rocks::test::packed_curve {

using curves_detail::PackedTangentCurveTestAccess;
using ue58richcurve::RichKey;

namespace {

bool SameFloat(float a, float b) noexcept {
    if (platform::PlatformMath::IsNaN(a) && platform::PlatformMath::IsNaN(b)) {
        return true;
    }

    return platform::BitCast<uint32>(a) == platform::BitCast<uint32>(b);
}

bool SameBytes(Span<const uint8> a, Span<const uint8> b) noexcept {
    if (a.Size() != b.Size()) {
        return false;
    }

    for (usize index = 0; index < a.Size(); ++index) {
        if (a[index] != b[index]) {
            return false;
        }
    }

    return true;
}

float RandomFloat(MathRandom& random, double minimum, double maximum) noexcept {
    return static_cast<float>(random.Range(minimum, maximum));
}

TangentCurveKey MakeKey(float time, float value,
                        CurveInterp interp = CurveInterp::Linear) noexcept {
    TangentCurveKey key;
    key.time = time;
    key.value = value;
    key.interp = interp;
    return key;
}

// 무작위 커브다. style로 형식을 몰아 준다: 0 모두 Linear, 1 모두 Cubic(가중 없음), 2 섞음(가중
// 없음), 3 섞음 + 가중, 4 같은 값(상수 후보). 간격은 대개 고르고 가끔 0·아주 작다.
void RandomCurve(Allocator& allocator, MathRandom& random, usize count, uint32 style,
                 TangentCurve& curve) noexcept {
    Array<TangentCurveKey> keys(allocator);
    float time = RandomFloat(random, -10.0, 10.0);
    const float spacing = RandomFloat(random, 0.05, 1.0);
    const float constant = RandomFloat(random, -5.0, 5.0);
    for (usize index = 0; index < count; ++index) {
        if (index > 0) {
            const uint64 gap = random.Next() % 16;
            time +=
                gap == 0 ? 0.0f : (gap == 1 ? 1.0e-3f : spacing * RandomFloat(random, 0.5, 1.5));
        }

        TangentCurveKey key;
        key.time = time;
        key.value = style == 4 ? constant : RandomFloat(random, -20.0, 20.0);
        key.tangentMode = static_cast<CurveTangentMode>(random.Next() % 3);
        key.arriveTangent = RandomFloat(random, -4.0, 4.0);
        key.leaveTangent = RandomFloat(random, -4.0, 4.0);
        key.arriveTangentWeight = RandomFloat(random, 0.0, 2.0);
        key.leaveTangentWeight = RandomFloat(random, 0.0, 2.0);
        switch (style) {
        case 0:
            key.interp = CurveInterp::Linear;
            break;
        case 1:
            key.interp = CurveInterp::Cubic;
            break;
        case 4:
            key.interp = random.Next() % 2 == 0 ? CurveInterp::Linear : CurveInterp::Constant;
            break;
        default:
            key.interp = static_cast<CurveInterp>(random.Next() % 3);
            break;
        }

        if (style == 3) {
            key.tangentWeight = static_cast<CurveTangentWeight>(random.Next() % 4);
        } else if ((style == 1 || style == 2) && random.Next() % 4 == 0) {
            // 구간을 가중으로 만들지 않는 가중 방식(첫 키 Arrive, 마지막 키 Leave)도 섞는다 — UE는
            // Weighted, Rocks는 아니다.
            if (index == 0) {
                key.tangentWeight = CurveTangentWeight::Arrive;
            } else if (index + 1 == count) {
                key.tangentWeight = CurveTangentWeight::Leave;
            }
        }

        CHECK(keys.TryPushBack(key));
    }

    REQUIRE(curve.TrySetKeys(keys.View()));
    curve.preExtrapolation = static_cast<CurveExtrapolation>(random.Next() % 5);
    curve.postExtrapolation = static_cast<CurveExtrapolation>(random.Next() % 5);
    curve.defaultValue = random.Next() % 2 == 0 ? Optional<float>(RandomFloat(random, -3.0, 3.0))
                                                : Optional<float>{};
}

Array<RichKey> ReferenceKeys(Allocator& allocator, const TangentCurve& curve) noexcept {
    Array<RichKey> keys(allocator);
    for (const TangentCurveKey& key : curve.Keys()) {
        CHECK(keys.TryPushBack(ue58richcurve::ToReference(key)));
    }

    return keys;
}

// UE가 Weighted로 정하고 Rocks가 아닌 사례의 근거: 가중 방식 Cubic 키가 있지만 가중 구간은 없다.
bool HasWeightedCubicKey(const TangentCurve& curve) noexcept {
    for (const TangentCurveKey& key : curve.Keys()) {
        if (key.interp == CurveInterp::Cubic && key.tangentWeight != CurveTangentWeight::None) {
            return true;
        }
    }

    return false;
}

bool HasWeightedSegment(const TangentCurve& curve) noexcept {
    const Span<const TangentCurveKey> keys = curve.Keys();
    for (usize index = 0; index + 1 < keys.Size(); ++index) {
        if (keys[index].interp == CurveInterp::Cubic &&
            !math::IsCurveSegmentUnweighted(keys[index], keys[index + 1])) {
            return true;
        }
    }

    return false;
}

// 키 범위 밖·안·키 시간·NaN·±Inf를 모두 담은 평가 시간들이다.
void EvaluationTimes(MathRandom& random, const TangentCurve& curve, Array<float>& times) noexcept {
    times.Clear();
    CHECK(times.TryPushBack(platform::PlatformMath::QuietNaN<float>()));
    CHECK(times.TryPushBack(platform::PlatformMath::Infinity<float>()));
    CHECK(times.TryPushBack(-platform::PlatformMath::Infinity<float>()));
    CHECK(times.TryPushBack(0.0f));
    const Span<const TangentCurveKey> keys = curve.Keys();
    if (keys.Size() == 0) {
        return;
    }

    const float first = keys[0].time;
    const float last = keys[keys.Size() - 1].time;
    for (const TangentCurveKey& key : keys) {
        CHECK(times.TryPushBack(key.time));
    }

    const float span = last - first + 1.0f;
    for (usize sample = 0; sample < 64; ++sample) {
        CHECK(times.TryPushBack(RandomFloat(random, first - 3.0 * span, last + 3.0 * span)));
    }
}

void CheckSameAsSource(Allocator& allocator, const PackedTangentCurve& packed,
                       const TangentCurve& source, MathRandom& random) noexcept {
    Array<float> times(allocator);
    EvaluationTimes(random, source, times);
    for (const float time : times) {
        const float expected = source.Evaluate(time, 1.25f);
        const Result<float> actual = packed.TryEvaluate(time, 1.25f);
        REQUIRE(actual);
        CHECK(SameFloat(actual.Value(), expected));
        CHECK(SameFloat(packed.Evaluate(time, 1.25f), expected));
    }
}

TangentCurve Linear8(Allocator& allocator) noexcept {
    TangentCurve curve(allocator);
    for (int32 index = 0; index < 8; ++index) {
        CHECK(curve.TryAddKey(static_cast<float>(index), static_cast<float>(index)));
    }

    Array<TangentCurveKey> keys(allocator);
    for (const TangentCurveKey& key : curve.Keys()) {
        TangentCurveKey linear = key;
        linear.interp = CurveInterp::Linear;
        CHECK(keys.TryPushBack(linear));
    }

    CHECK(curve.TrySetKeys(keys.View()));
    return curve;
}

TangentCurve Mixed3(Allocator& allocator) noexcept {
    TangentCurve curve(allocator);
    TangentCurveKey keys[3] = {MakeKey(0.5f, 1.0f, CurveInterp::Constant),
                               MakeKey(1.5f, 2.0f, CurveInterp::Linear),
                               MakeKey(2.5f, -1.0f, CurveInterp::Cubic)};
    keys[2].arriveTangent = 0.25f;
    keys[2].leaveTangent = -0.75f;
    CHECK(curve.TrySetKeys(Span<const TangentCurveKey>(keys, 3)));
    curve.postExtrapolation = CurveExtrapolation::Linear;
    return curve;
}

} // namespace

// UE CompressCurve와의 byte 대조. Rocks가 고른 형식·시간 형식으로 UE 참조 배치를 만들면 byte가
// 같고(모든 사례), 고르는 결과가 다른 사례는 15절 수정으로만 설명된다:
// - 형식: UE Constant(허용오차 1e-8) ↔ Rocks 비 Constant는 값의 비트가 다르거나 Linear 외삽,
//   UE Weighted ↔ Rocks 비 Weighted는 가중 구간이 없을 때, UE 비 Weighted ↔ Rocks Weighted는
//   Cubic이 아닌 키의 도착 가중이 앞 Cubic 구간을 가중으로 만들 때뿐이다.
// - 시간 형식: 판정 방식이 다르므로(표본 누적·구간 전진 수정) 다를 수 있다 — 그 수를 세어 대부분
//   같은지 본다(Rocks Quantized16의 오차 보장은 다음 시험이 본다).
TEST(PackedTangentCurve_GoldenBytesMatchUe) {
    SystemAllocator system;
    MathRandom random(0x5043'4B38);
    usize formatAgree = 0;
    usize keyFormatAgree = 0;
    usize quantized = 0;
    usize cases = 0;
    const float thresholds[] = {1.0e-4f, 1.0e-2f, 0.5f};
    for (usize iteration = 0; iteration < 1500; ++iteration) {
        TangentCurve curve(system);
        const uint32 style = static_cast<uint32>(random.Next() % 5);
        const usize count = static_cast<usize>(random.Next() % 24);
        RandomCurve(system, random, count, style, curve);
        const float threshold = thresholds[random.Next() % 3];
        PackedTangentCurve packed(system);
        REQUIRE(packed.TryPack(curve, threshold, 120.0f));
        ++cases;

        const Array<RichKey> reference = ReferenceKeys(system, curve);
        const ue58richcurve::CurveState state = ue58richcurve::StateOf(curve);
        const ue58curve::Options options{
            .exactLerpEndpoints = false, .fixCycle = false, .fixUnfilledRoot = true};
        ue58pack::Compressed ue(system);
        ue58pack::CompressCurve(reference, state, threshold, 120.0f, ue, options);
        const auto rocksFormat = static_cast<ue58pack::Format>(packed.Format());
        const auto rocksKeyFormat = static_cast<ue58pack::KeyFormat>(packed.KeyTimeFormat());
        if (ue.CompressionFormat == rocksFormat) {
            ++formatAgree;
        } else if (ue.CompressionFormat == ue58pack::Format::Constant) {
            // UE는 1e-8 안의 값을 같다고 보고 Linear 외삽도 무시한다.
            bool bitsDiffer = false;
            for (const TangentCurveKey& key : curve.Keys()) {
                bitsDiffer |= !SameFloat(key.value, curve.Keys()[0].value);
            }

            CHECK(bitsDiffer || curve.preExtrapolation == CurveExtrapolation::Linear ||
                  curve.postExtrapolation == CurveExtrapolation::Linear);
        } else if (ue.CompressionFormat == ue58pack::Format::Weighted) {
            // UE는 Cubic 키의 가중 방식만 본다 — 가중 구간이 없어도 Weighted다.
            CHECK(HasWeightedCubicKey(curve) && !HasWeightedSegment(curve));
        } else {
            // Linear·Constant 키의 도착 가중이 앞 Cubic 구간을 가중으로 만든다(UE는 놓친다).
            CHECK(packed.Format() == PackedCurveFormat::Weighted);
            CHECK(!HasWeightedCubicKey(curve) && HasWeightedSegment(curve));
        }

        if (ue.CompressionFormat == rocksFormat && ue.KeyTimeCompressionFormat == rocksKeyFormat) {
            ++keyFormatAgree;
        }

        quantized += packed.KeyTimeFormat() == PackedKeyTimeFormat::Quantized16 ? 1 : 0;

        // 같은 형식으로 만든 UE 배치와 byte가 같다.
        ue58pack::CompressCurve(reference, state, threshold, 120.0f, ue, options, &rocksFormat,
                                &rocksKeyFormat);
        CHECK(SameBytes(packed.Packed(), ue.CompressedKeys.View()));
        CHECK(packed.KeyCount() == static_cast<uint32>(ue.NumKeys));
        CHECK(static_cast<uint8>(packed.PreExtrapolation()) ==
                  static_cast<uint8>(ue.PreInfinityExtrap) &&
              static_cast<uint8>(packed.PostExtrapolation()) ==
                  static_cast<uint8>(ue.PostInfinityExtrap));
        if (packed.Format() == PackedCurveFormat::Constant) {
            CHECK(SameFloat(packed.ConstantValue(), ue.ConstantValue));
        }

        if (packed.Format() == PackedCurveFormat::Empty) {
            CHECK(packed.EmptyDefault().HasValue() == curve.defaultValue.HasValue());
            if (curve.defaultValue.HasValue()) {
                CHECK(SameFloat(packed.EmptyDefault().Value(), ue.ConstantValue));
            }
        }

        CHECK(PackedTangentCurve::Validate(packed));
    }

    CHECK(formatAgree * 10 >= cases * 7);
    CHECK(keyFormatAgree * 10 >= cases * 6);
    CHECK(quantized >= 50);
}

// 형식 결정의 수정 반례(11.1절·15절): UE와 형식이 다르고 Rocks 결과는 원본과 비트가 같다.
TEST(PackedTangentCurve_FormatDecisionFixes) {
    SystemAllocator system;
    auto ueFormat = [&](const TangentCurve& curve) noexcept {
        return ue58pack::FindCompressionFormat(ReferenceKeys(system, curve));
    };

    // 1) 1e-8 안의 다른 값: UE Constant(값 하나로 줄여 원본과 다르다), Rocks Linear.
    {
        TangentCurve curve(system);
        const TangentCurveKey keys[2] = {MakeKey(0.0f, 0.0f), MakeKey(1.0f, 5.0e-9f)};
        REQUIRE(curve.TrySetKeys(Span<const TangentCurveKey>(keys, 2)));
        PackedTangentCurve packed(system);
        REQUIRE(packed.TryPack(curve));
        CHECK(ueFormat(curve) == ue58pack::Format::Constant);
        CHECK(packed.Format() == PackedCurveFormat::Linear);
        CHECK(SameFloat(packed.Evaluate(1.0f), 5.0e-9f));
    }

    // 2) 같은 값 + Linear 외삽: ±Inf 시간에서 0 기울기 × Inf = NaN이라 Constant로 줄일 수 없다.
    {
        TangentCurve curve(system);
        const TangentCurveKey keys[2] = {MakeKey(0.0f, 2.0f), MakeKey(1.0f, 2.0f)};
        REQUIRE(curve.TrySetKeys(Span<const TangentCurveKey>(keys, 2)));
        curve.postExtrapolation = CurveExtrapolation::Linear;
        PackedTangentCurve packed(system);
        REQUIRE(packed.TryPack(curve));
        CHECK(ueFormat(curve) == ue58pack::Format::Constant);
        CHECK(packed.Format() == PackedCurveFormat::Linear);
        const float infinity = platform::PlatformMath::Infinity<float>();
        CHECK(platform::PlatformMath::IsNaN(curve.Evaluate(infinity)));
        CHECK(SameFloat(packed.Evaluate(infinity), curve.Evaluate(infinity)));
        // 외삽이 Constant면 Constant 형식이다.
        curve.postExtrapolation = CurveExtrapolation::Constant;
        REQUIRE(packed.TryPack(curve));
        CHECK(packed.Format() == PackedCurveFormat::Constant && packed.ConstantValue() == 2.0f);
        CHECK(packed.Evaluate(infinity) == 2.0f);
    }

    // 3) 같은 ±Inf 값: Lerp(Inf, Inf, 0.5) = NaN이라 Constant가 아니다(UE도 Inf − Inf = NaN이라
    //    Constant가 아니다).
    {
        TangentCurve curve(system);
        const float infinity = platform::PlatformMath::Infinity<float>();
        const TangentCurveKey keys[2] = {MakeKey(0.0f, infinity), MakeKey(1.0f, infinity)};
        REQUIRE(curve.TrySetKeys(Span<const TangentCurveKey>(keys, 2)));
        PackedTangentCurve packed(system);
        REQUIRE(packed.TryPack(curve));
        CHECK(packed.Format() == PackedCurveFormat::Linear);
        CHECK(SameFloat(packed.Evaluate(0.5f), curve.Evaluate(0.5f)));
    }

    // 4) 가중 방식은 있지만 가중 구간이 없다(첫 키 Arrive, 마지막 키 Leave — 둘 다 어느 구간에도
    //    쓰이지 않는 쪽): UE Weighted(키당 5 float), Rocks Cubic(3 float).
    {
        TangentCurve curve(system);
        TangentCurveKey keys[3] = {MakeKey(0.0f, 0.0f, CurveInterp::Cubic),
                                   MakeKey(1.0f, 1.0f, CurveInterp::Cubic),
                                   MakeKey(2.0f, 0.0f, CurveInterp::Cubic)};
        keys[0].tangentWeight = CurveTangentWeight::Arrive;
        keys[0].leaveTangent = 1.5f;
        keys[2].tangentWeight = CurveTangentWeight::Leave;
        keys[2].leaveTangentWeight = 3.0f;
        REQUIRE(curve.TrySetKeys(Span<const TangentCurveKey>(keys, 3)));
        PackedTangentCurve packed(system);
        REQUIRE(packed.TryPack(curve));
        CHECK(ueFormat(curve) == ue58pack::Format::Weighted);
        CHECK(packed.Format() == PackedCurveFormat::Cubic);
        CHECK(packed.Packed().Size() == 3 * 4 + 3 * 12);
        MathRandom random(17);
        CheckSameAsSource(system, packed, curve, random);
        // 가운데 키가 Both면 구간 0의 도착·구간 1의 출발이 가중이라 Weighted다.
        keys[1].tangentWeight = CurveTangentWeight::Both;
        REQUIRE(curve.TrySetKeys(Span<const TangentCurveKey>(keys, 3)));
        REQUIRE(packed.TryPack(curve));
        CHECK(packed.Format() == PackedCurveFormat::Weighted);
        CheckSameAsSource(system, packed, curve, random);
    }

    // 5) Linear 키의 도착 가중이 앞 Cubic 구간을 가중으로 만든다: UE Mixed(가중을 버린다), Rocks
    //    Weighted(원본과 비트가 같다).
    {
        TangentCurve curve(system);
        TangentCurveKey keys[2] = {MakeKey(0.0f, 0.0f, CurveInterp::Cubic),
                                   MakeKey(1.0f, 1.0f, CurveInterp::Linear)};
        keys[0].leaveTangent = 2.0f;
        keys[1].tangentWeight = CurveTangentWeight::Arrive;
        keys[1].arriveTangent = -1.0f;
        keys[1].arriveTangentWeight = 1.5f;
        REQUIRE(curve.TrySetKeys(Span<const TangentCurveKey>(keys, 2)));
        PackedTangentCurve packed(system);
        REQUIRE(packed.TryPack(curve));
        CHECK(ueFormat(curve) == ue58pack::Format::Mixed);
        CHECK(packed.Format() == PackedCurveFormat::Weighted);
        MathRandom random(29);
        CheckSameAsSource(system, packed, curve, random);
    }
}

// D14: Float32 시간 형식이면 모든 형식에서 원본 TangentCurve와 모든 시간(범위 밖·키 시간·NaN·
// ±Inf)에서 비트가 같다(errorThreshold 0 → Float32).
TEST(PackedTangentCurve_Float32EvaluateIsBitExact) {
    SystemAllocator system;
    MathRandom random(0xD14);
    usize perFormat[6] = {};
    for (usize iteration = 0; iteration < 1200; ++iteration) {
        TangentCurve curve(system);
        RandomCurve(system, random, static_cast<usize>(random.Next() % 12),
                    static_cast<uint32>(random.Next() % 5), curve);
        PackedTangentCurve packed(system);
        REQUIRE(packed.TryPack(curve, 0.0f));
        CHECK(packed.KeyTimeFormat() == PackedKeyTimeFormat::Float32);
        ++perFormat[static_cast<uint8>(packed.Format())];
        CheckSameAsSource(system, packed, curve, random);
    }

    for (const usize count : perFormat) {
        CHECK(count >= 20);
    }
}

// Quantized16: 고른 사례마다 표본 시간(첫 키부터 1/rate 간격)의 평가 오차가 errorThreshold 안이고,
// packed가 Float32보다 작다.
TEST(PackedTangentCurve_Quantized16ErrorWithinThreshold) {
    SystemAllocator system;
    MathRandom random(0x5116);
    usize quantized = 0;
    for (usize iteration = 0; iteration < 600; ++iteration) {
        TangentCurve curve(system);
        RandomCurve(system, random, 7 + static_cast<usize>(random.Next() % 40),
                    static_cast<uint32>(random.Next() % 4), curve);
        const float threshold = random.Next() % 2 == 0 ? 1.0e-2f : 0.2f;
        const float rate = random.Next() % 2 == 0 ? 30.0f : 120.0f;
        PackedTangentCurve packed(system);
        REQUIRE(packed.TryPack(curve, threshold, rate));
        if (packed.KeyTimeFormat() != PackedKeyTimeFormat::Quantized16) {
            continue;
        }

        ++quantized;
        CHECK(packed.Packed().Size() <
              PackedTangentCurve::ExpectedPackedSize(packed.Format(), PackedKeyTimeFormat::Float32,
                                                     packed.KeyCount()));
        const float first = curve.Keys()[0].time;
        const float last = curve.Keys()[curve.KeyCount() - 1].time;
        for (usize sample = 0; first + static_cast<float>(sample) * (1.0f / rate) <= last;
             ++sample) {
            const float time = first + static_cast<float>(sample) * (1.0f / rate);
            const float error = math::Abs(packed.Evaluate(time) - curve.Evaluate(time));
            CHECK(error <= threshold);
        }
    }

    CHECK(quantized >= 50);
}

// 형식별 왕복: 압축 → 풀기 → 원본과 비교. 시간은 Float32면 같고 Quantized16이면 양자화 반 칸
// 안이다. 값·보간·접선은 같고(Linear 형식은 접선 0), 가중은 Weighted만 남는다. 접선 방식은 User,
// 외삽은 같고, 기본값은 Empty만 남는다. 풀어 낸 커브를 다시 압축하면 byte가 같다.
TEST(PackedTangentCurve_RoundTripPerFormat) {
    SystemAllocator system;
    MathRandom random(0x7277);
    usize perFormat[6] = {};
    usize quantized = 0;
    for (usize iteration = 0; iteration < 1000; ++iteration) {
        TangentCurve curve(system);
        RandomCurve(system, random, static_cast<usize>(random.Next() % 24),
                    static_cast<uint32>(random.Next() % 5), curve);
        PackedTangentCurve packed(system);
        REQUIRE(packed.TryPack(curve, random.Next() % 2 == 0 ? 0.0f : 0.05f, 60.0f));
        ++perFormat[static_cast<uint8>(packed.Format())];
        TangentCurve out(system);
        REQUIRE(out.TryAddKey(99.0f, 1.0f));
        REQUIRE(packed.TryUnpack(out));
        CHECK(out.preExtrapolation == curve.preExtrapolation &&
              out.postExtrapolation == curve.postExtrapolation);
        const PackedCurveFormat format = packed.Format();
        if (format == PackedCurveFormat::Empty) {
            CHECK(out.KeyCount() == 0);
            CHECK(out.defaultValue.HasValue() == curve.defaultValue.HasValue());
            continue;
        }

        CHECK(!out.defaultValue.HasValue());
        if (format == PackedCurveFormat::Constant) {
            REQUIRE(out.KeyCount() == 1);
            CHECK(out.Keys()[0].time == 0.0f &&
                  SameFloat(out.Keys()[0].value, curve.Keys()[0].value));
            CHECK(out.Keys()[0].interp == CurveInterp::Linear);
            continue;
        }

        REQUIRE(out.KeyCount() == curve.KeyCount());
        const bool q16 = packed.KeyTimeFormat() == PackedKeyTimeFormat::Quantized16;
        quantized += q16 ? 1 : 0;
        const float first = curve.Keys()[0].time;
        const float range = curve.Keys()[curve.KeyCount() - 1].time - first;
        for (usize index = 0; index < curve.KeyCount(); ++index) {
            const TangentCurveKey& source = curve.Keys()[index];
            const TangentCurveKey& restored = out.Keys()[index];
            if (q16) {
                CHECK(math::Abs(restored.time - source.time) <=
                      range / 65535.0f + 1.0e-5f * (math::Abs(first) + range));
            } else {
                CHECK(SameFloat(restored.time, source.time));
            }

            CHECK(SameFloat(restored.value, source.value));
            CHECK(restored.tangentMode == CurveTangentMode::User);
            if (format == PackedCurveFormat::Linear) {
                CHECK(restored.interp == CurveInterp::Linear && restored.arriveTangent == 0.0f &&
                      restored.leaveTangent == 0.0f);
            } else {
                CHECK(restored.interp == source.interp);
                CHECK(SameFloat(restored.arriveTangent, source.arriveTangent) &&
                      SameFloat(restored.leaveTangent, source.leaveTangent));
            }

            if (format == PackedCurveFormat::Weighted) {
                CHECK(restored.tangentWeight == source.tangentWeight &&
                      SameFloat(restored.arriveTangentWeight, source.arriveTangentWeight) &&
                      SameFloat(restored.leaveTangentWeight, source.leaveTangentWeight));
            } else {
                CHECK(restored.tangentWeight == CurveTangentWeight::None &&
                      restored.arriveTangentWeight == 0.0f && restored.leaveTangentWeight == 0.0f);
            }
        }

        if (!q16) {
            PackedTangentCurve again(system);
            REQUIRE(again.TryPack(out, 0.0f));
            CHECK(again.Format() == format && SameBytes(again.Packed(), packed.Packed()));
        }
    }

    for (usize format = 0; format < 6; ++format) {
        CHECK(perFormat[format] >= 20);
    }

    CHECK(quantized >= 20);
}

// 빈 커브(기본값·fallback·−0)와 상수 커브(키 하나·같은 값 여럿·NaN 시간), 그 풀기.
TEST(PackedTangentCurve_EmptyAndConstant) {
    SystemAllocator system;
    const float nan = platform::PlatformMath::QuietNaN<float>();
    TangentCurve curve(system);
    PackedTangentCurve packed(system);
    CHECK(packed.Format() == PackedCurveFormat::Empty && packed.Evaluate(1.0f, 7.0f) == 7.0f);
    REQUIRE(packed.TryPack(curve));
    CHECK(packed.Format() == PackedCurveFormat::Empty && packed.KeyCount() == 0 &&
          packed.Packed().Size() == 0 && !packed.EmptyDefault().HasValue());
    CHECK(packed.Evaluate(3.0f, 7.0f) == 7.0f && packed.Evaluate(nan, 7.0f) == 7.0f);
    curve.defaultValue = -0.0f;
    REQUIRE(packed.TryPack(curve));
    CHECK(packed.EmptyDefault().HasValue());
    CHECK(SameFloat(packed.Evaluate(3.0f, 7.0f), curve.Evaluate(3.0f, 7.0f)));
    CHECK(SameFloat(packed.Evaluate(3.0f, 7.0f), 0.0f));
    TangentCurve out(system);
    REQUIRE(packed.TryUnpack(out));
    CHECK(out.KeyCount() == 0 && out.defaultValue.HasValue() &&
          SameFloat(out.defaultValue.Value(), -0.0f));

    curve.defaultValue = 4.0f;
    REQUIRE(curve.TryAddKey(2.0f, -3.5f));
    curve.preExtrapolation = CurveExtrapolation::Linear;
    REQUIRE(packed.TryPack(curve));
    CHECK(packed.Format() == PackedCurveFormat::Constant && packed.ConstantValue() == -3.5f);
    CHECK(packed.KeyTimeFormat() == PackedKeyTimeFormat::Float32 && packed.Packed().Size() == 0);
    CHECK(!packed.EmptyDefault().HasValue());
    CHECK(packed.Evaluate(-100.0f) == -3.5f && platform::PlatformMath::IsNaN(packed.Evaluate(nan)));
    CHECK(SameFloat(packed.Evaluate(nan), curve.Evaluate(nan)));
    REQUIRE(packed.TryUnpack(out));
    REQUIRE(out.KeyCount() == 1);
    CHECK(out.Keys()[0].time == 0.0f && out.Keys()[0].value == -3.5f &&
          !out.defaultValue.HasValue() && out.preExtrapolation == CurveExtrapolation::Linear);

    // 같은 값 키 여럿(Constant·Linear 보간, 순환 외삽): Constant이고 모든 시간에서 원본과 같다.
    REQUIRE(curve.TryAddKey(3.0f, -3.5f));
    REQUIRE(curve.TryAddKey(5.0f, -3.5f));
    curve.preExtrapolation = CurveExtrapolation::CycleWithOffset;
    curve.postExtrapolation = CurveExtrapolation::Oscillate;
    REQUIRE(packed.TryPack(curve));
    CHECK(packed.Format() == PackedCurveFormat::Constant);
    MathRandom random(3);
    CheckSameAsSource(system, packed, curve, random);
}

// 손으로 쓴 byte 배치(11.3절, 리틀 엔디언):
// - Mixed 3키 Float32: 보간 [1 2 3] + 틈 1 → 시간 3×f32(4) → 데이터 3×(값·arrive·leave)(16), 52
// byte.
// - Linear 8키 Quantized16(값 = 시간): 시간 8×u16(0) → min·길이(16) → 값 8×f32(24), 56 byte.
TEST(PackedTangentCurve_HandWrittenLayout) {
    SystemAllocator system;
    auto floatAt = [](Span<const uint8> bytes, usize offset) noexcept {
        uint32 bits = 0;
        for (usize index = 0; index < 4; ++index) {
            bits |= static_cast<uint32>(bytes[offset + index]) << (8 * index);
        }

        return platform::BitCast<float>(bits);
    };

    PackedTangentCurve packed(system);
    const TangentCurve mixed = Mixed3(system);
    REQUIRE(packed.TryPack(mixed));
    REQUIRE(packed.Format() == PackedCurveFormat::Mixed);
    CHECK(packed.KeyTimeFormat() == PackedKeyTimeFormat::Float32);
    Span<const uint8> bytes = packed.Packed();
    REQUIRE(bytes.Size() == 52);
    CHECK(PackedTangentCurve::ExpectedPackedSize(PackedCurveFormat::Mixed,
                                                 PackedKeyTimeFormat::Float32, 3) == 52);
    CHECK(bytes[0] == 1 && bytes[1] == 2 && bytes[2] == 3 && bytes[3] == 0);
    CHECK(floatAt(bytes, 4) == 0.5f && floatAt(bytes, 8) == 1.5f && floatAt(bytes, 12) == 2.5f);
    CHECK(floatAt(bytes, 16) == 1.0f && floatAt(bytes, 28) == 2.0f && floatAt(bytes, 40) == -1.0f);
    CHECK(floatAt(bytes, 44) == 0.25f && floatAt(bytes, 48) == -0.75f);
    CHECK(packed.Evaluate(1.0f) == 1.0f && packed.Evaluate(2.0f) == mixed.Evaluate(2.0f));

    const TangentCurve linear = Linear8(system);
    REQUIRE(packed.TryPack(linear));
    REQUIRE(packed.Format() == PackedCurveFormat::Linear);
    REQUIRE(packed.KeyTimeFormat() == PackedKeyTimeFormat::Quantized16);
    bytes = packed.Packed();
    REQUIRE(bytes.Size() == 56);
    const uint16 times[8] = {0, 9362, 18724, 28086, 37449, 46811, 56173, 65535};
    for (usize index = 0; index < 8; ++index) {
        CHECK(static_cast<uint16>(bytes[index * 2] | (bytes[index * 2 + 1] << 8)) == times[index]);
        CHECK(floatAt(bytes, 24 + index * 4) == static_cast<float>(index));
    }

    CHECK(floatAt(bytes, 16) == 0.0f && floatAt(bytes, 20) == 7.0f);
    CHECK(math::Abs(packed.Evaluate(3.5f) - 3.5f) <= 1.0e-4f);
    // 표본 오차 한도를 아주 작게 하면 Float32다.
    REQUIRE(packed.TryPack(linear, 1.0e-7f));
    CHECK(packed.KeyTimeFormat() == PackedKeyTimeFormat::Float32 && packed.Packed().Size() == 64);
    // errorThreshold·sampleRateHz가 비유한·0 이하면 Float32다.
    const float bad[] = {0.0f, -1.0f, platform::PlatformMath::QuietNaN<float>(),
                         platform::PlatformMath::Infinity<float>()};
    for (const float value : bad) {
        REQUIRE(packed.TryPack(linear, value, 120.0f));
        CHECK(packed.KeyTimeFormat() == PackedKeyTimeFormat::Float32);
        REQUIRE(packed.TryPack(linear, 1.0e-4f, value));
        CHECK(packed.KeyTimeFormat() == PackedKeyTimeFormat::Float32);
    }

    // 기대 크기 계산이 넘치면 IndexNone이다(Weighted Float32 키당 2 + 4 + 20 byte).
    CHECK(PackedTangentCurve::ExpectedPackedSize(PackedCurveFormat::Weighted,
                                                 PackedKeyTimeFormat::Float32, 7) == 16 + 28 + 140);
    CHECK(PackedTangentCurve::ExpectedPackedSize(PackedCurveFormat::Empty,
                                                 PackedKeyTimeFormat::Float32, 9) == 0);
}

// 정합이 깨진 값(D21): 평가 진입의 O(1) 대조(형식·시간 형식·외삽 범위, packed 크기 = 기대 크기)와
// 잘못된 보간·가중 byte는 Evaluate NaN·TryEvaluate InvalidArgument이고, Validate는 그 밖의
// 칸 정합·시간 정렬·유한까지 보고 거부하며 TryUnpack은 out을 그대로 둔다.
TEST(PackedTangentCurve_TamperedValuesAreRejected) {
    SystemAllocator system;
    using Access = PackedTangentCurveTestAccess;
    struct Case {
        const char* label;
        bool evaluationRejects; // 평가도 거부하는지(아니면 Validate·풀기만)
        void (*tamper)(PackedTangentCurve&);
    };
    const Case cases[] = {
        {"key count", true, [](PackedTangentCurve& c) noexcept { ++Access::KeyCount(c); }},
        {"short packed", true,
         [](PackedTangentCurve& c) noexcept {
             CHECK(Access::Packed(c).TryResize(Access::Packed(c).Size() - 1));
         }},
        {"format range", true,
         [](PackedTangentCurve& c) noexcept {
             Access::Format(c) = static_cast<PackedCurveFormat>(9);
         }},
        {"format other layout", true,
         [](PackedTangentCurve& c) noexcept { Access::Format(c) = PackedCurveFormat::Weighted; }},
        {"key time format", true,
         [](PackedTangentCurve& c) noexcept {
             Access::KeyTimeFormat(c) = static_cast<PackedKeyTimeFormat>(7);
         }},
        {"extrapolation", true,
         [](PackedTangentCurve& c) noexcept {
             Access::PostExtrapolation(c) = static_cast<CurveExtrapolation>(5);
         }},
        {"interp byte", true, [](PackedTangentCurve& c) noexcept { Access::Packed(c)[1] = 0; }},
        {"interp byte high", true,
         [](PackedTangentCurve& c) noexcept { Access::Packed(c)[1] = 4; }},
        {"time order", false,
         [](PackedTangentCurve& c) noexcept {
             // 시간 3×f32는 offset 4부터 — 둘째 키 시간(offset 8)을 첫 키보다 작게.
             const uint32 bits = platform::BitCast<uint32>(-5.0f);
             for (usize index = 0; index < 4; ++index) {
                 Access::Packed(c)[8 + index] = static_cast<uint8>(bits >> (8 * index));
             }
         }},
        {"non-finite time", false,
         [](PackedTangentCurve& c) noexcept {
             const uint32 bits =
                 platform::BitCast<uint32>(platform::PlatformMath::QuietNaN<float>());
             for (usize index = 0; index < 4; ++index) {
                 Access::Packed(c)[12 + index] = static_cast<uint8>(bits >> (8 * index));
             }
         }},
        {"default on keyed", false,
         [](PackedTangentCurve& c) noexcept { Access::EmptyDefault(c) = 1.0f; }},
        {"constant on keyed", false,
         [](PackedTangentCurve& c) noexcept { Access::ConstantValue(c) = -0.0f; }},
    };
    for (const Case& item : cases) {
        PackedTangentCurve packed(system);
        REQUIRE(packed.TryPack(Mixed3(system)));
        item.tamper(packed);
        CHECK(PackedTangentCurve::Validate(packed).Error() == ErrorCode::InvalidArgument);
        TangentCurve out(system);
        REQUIRE(out.TryAddKey(9.0f, 9.0f));
        CHECK(packed.TryUnpack(out).Error() == ErrorCode::InvalidArgument);
        CHECK(out.KeyCount() == 1 && out.Keys()[0].time == 9.0f);
        if (item.evaluationRejects) {
            // 1.0은 둘째 키(보간 byte를 바꾼 키)에서 시작하는 구간 앞쪽이다.
            CHECK(platform::PlatformMath::IsNaN(packed.Evaluate(1.75f)));
            CHECK(packed.TryEvaluate(1.75f).Error() == ErrorCode::InvalidArgument);
        } else {
            static_cast<void>(packed.Evaluate(1.75f)); // 메모리 밖을 읽지 않는다(정합은 맞다)
        }
    }

    // Empty·Constant의 칸 정합과 Quantized16 범위·시간 정렬.
    PackedTangentCurve packed(system);
    Access::ConstantValue(packed) = 1.0f;
    CHECK(!PackedTangentCurve::Validate(packed));
    Access::ConstantValue(packed) = 0.0f;
    Access::KeyTimeFormat(packed) = PackedKeyTimeFormat::Quantized16;
    CHECK(!PackedTangentCurve::Validate(packed));
    Access::KeyTimeFormat(packed) = PackedKeyTimeFormat::Float32;
    CHECK(Access::Packed(packed).TryPushBack(uint8{0}));
    CHECK(!PackedTangentCurve::Validate(packed));
    CHECK(platform::PlatformMath::IsNaN(packed.Evaluate(0.0f)));
    Access::Packed(packed).Clear();
    Access::KeyCount(packed) = 1;
    CHECK(!PackedTangentCurve::Validate(packed));
    Access::KeyCount(packed) = 0;
    CHECK(PackedTangentCurve::Validate(packed));

    REQUIRE(packed.TryPack(Linear8(system)));
    REQUIRE(packed.KeyTimeFormat() == PackedKeyTimeFormat::Quantized16);
    CHECK(PackedTangentCurve::Validate(packed));
    Array<uint8>& bytes = Access::Packed(packed);
    const uint8 saved = bytes[3];
    bytes[3] = 0xFF; // 둘째 키 시간 > 셋째 키 시간
    CHECK(!PackedTangentCurve::Validate(packed));
    bytes[3] = saved;
    bytes[23] = 0xFF; // 길이 float의 최상위 byte → 음수 NaN
    CHECK(!PackedTangentCurve::Validate(packed));
    bytes[23] = 0xC0; // 길이 −수
    CHECK(!PackedTangentCurve::Validate(packed));
}

// 할당 실패의 원자성(주입 단언): TryPack(Quantized16 판정의 키 사본·packed), TryUnpack(키 배열·
// TrySetKeys), TryCopyFrom이 OutOfMemory면 값이 그대로다.
TEST(PackedTangentCurve_AllocationFailureIsAtomic) {
    SystemAllocator system;
    const TangentCurve linear = Linear8(system);
    const TangentCurve mixed = Mixed3(system);
    PackedTangentCurve expected(system);
    REQUIRE(expected.TryPack(mixed));

    auto same = [](const PackedTangentCurve& a, const PackedTangentCurve& b) noexcept {
        return a.Format() == b.Format() && a.KeyTimeFormat() == b.KeyTimeFormat() &&
               a.KeyCount() == b.KeyCount() && a.PostExtrapolation() == b.PostExtrapolation() &&
               SameBytes(a.Packed(), b.Packed());
    };

    bool injected[3] = {};
    for (uint64 attempt = 1; attempt <= 8; ++attempt) {
        test::NFailAllocator failing(system);
        PackedTangentCurve packed(failing);
        REQUIRE(packed.TryPack(mixed));
        failing.FailOnAttempt(attempt); // 시도 수는 여기서 0부터 센다
        const Result<void> made = packed.TryPack(linear);
        failing.DisableFailure();
        if (!made) {
            injected[0] = true;
            CHECK(made.Error() == ErrorCode::OutOfMemory && same(packed, expected));
        } else {
            CHECK(packed.KeyTimeFormat() == PackedKeyTimeFormat::Quantized16);
        }

        TangentCurve out(failing);
        REQUIRE(out.TryAddKey(9.0f, 9.0f));
        failing.FailOnAttempt(attempt);
        const Result<void> unpacked = expected.TryUnpack(out);
        failing.DisableFailure();
        if (!unpacked) {
            injected[1] = true;
            CHECK(unpacked.Error() == ErrorCode::OutOfMemory && out.KeyCount() == 1 &&
                  out.Keys()[0].time == 9.0f);
        }

        PackedTangentCurve copy(failing);
        REQUIRE(copy.TryPack(linear));
        PackedTangentCurve before(system);
        REQUIRE(before.TryCopyFrom(copy));
        failing.FailOnAttempt(attempt);
        const Result<void> copied = copy.TryCopyFrom(expected);
        failing.DisableFailure();
        if (!copied) {
            injected[2] = true;
            CHECK(copied.Error() == ErrorCode::OutOfMemory && same(copy, before));
        } else {
            CHECK(same(copy, expected));
        }
    }

    CHECK(injected[0] && injected[1] && injected[2]);
}

// 크기 한도: packed가 MaxElements(1 MiB)를 넘는 커브는 CapacityExceeded이고 그대로다.
TEST(PackedTangentCurve_CapacityExceeded) {
    SystemAllocator system;
    Array<TangentCurveKey> keys(system);
    // Linear Float32 키당 8 byte → 131073 키면 1 MiB + 8.
    for (usize index = 0; index < 131073; ++index) {
        REQUIRE(
            keys.TryPushBack(MakeKey(static_cast<float>(index), static_cast<float>(index % 7))));
    }

    TangentCurve curve(system);
    REQUIRE(curve.TrySetKeys(keys.View()));
    PackedTangentCurve packed(system);
    REQUIRE(packed.TryPack(Mixed3(system)));
    CHECK(packed.TryPack(curve, 0.0f).Error() == ErrorCode::CapacityExceeded);
    CHECK(packed.Format() == PackedCurveFormat::Mixed && packed.KeyCount() == 3);
    keys.Clear();
    for (usize index = 0; index < 131072; ++index) {
        REQUIRE(
            keys.TryPushBack(MakeKey(static_cast<float>(index), static_cast<float>(index % 7))));
    }

    REQUIRE(curve.TrySetKeys(keys.View()));
    REQUIRE(packed.TryPack(curve, 0.0f));
    CHECK(packed.Packed().Size() == 1048576 && PackedTangentCurve::Validate(packed));
}

} // namespace rocks::test::packed_curve

namespace rocks::test::packed_curve {

// 범위 밖 읽기 회귀 시험(CV8 리뷰 1): UE FindKeyFormat은 복원한 마지막 키 시간이 max보다 작으면
// (1 ulp) 손실 키를 마지막 키까지 전진한 뒤 Keys[NumKeys]를 읽는다(UE 배열 밖 읽기 — 참조는 고쳐
// 옮겼다, 15절 32). 반례 세 개에서 전제(복원 시간 < max)가 성립하고, 고친 참조와 Rocks가 끝까지
// 돌며 byte가 같다. 고친 갈래가 판정을 바꾸는 반례는 찾지 못했다(15절 32).
TEST(PackedTangentCurve_UeLossyKeyOverrunCounterexamples) {
    SystemAllocator system;
    const float ranges[][2] = {{-3.0f, 6.4f}, {-4.0f, 7.2666669f}, {-5.0f, 6.2666669f}};
    for (const auto& range : ranges) {
        TangentCurve curve(system);
        const TangentCurveKey keys[2] = {MakeKey(range[0], 0.0f), MakeKey(range[1], 1.0f)};
        REQUIRE(curve.TrySetKeys(Span<const TangentCurveKey>(keys, 2)));
        const float delta = range[1] - range[0];
        const float normalized =
            ue58pack::Clamp((range[1] - range[0]) * (1.0f / delta), 0.0f, 1.0f);
        const float restored =
            (ue58pack::RoundHalfFromZero(normalized * 65535.0f) * (1.0f / 65535.0f)) * delta +
            range[0];
        CHECK(restored < range[1]); // UE가 Keys[NumKeys]를 읽는 전제
        const Array<RichKey> reference = ReferenceKeys(system, curve);
        const ue58curve::Options options{.exactLerpEndpoints = false, .fixCycle = false};
        const ue58pack::KeyFormat format =
            ue58pack::FindKeyFormat(reference, 1.0e-4f, 120.0f, ue58pack::Format::Linear, options);
        CHECK(format == ue58pack::KeyFormat::Float32); // 키 2개는 크기가 작아지지 않는다
        PackedTangentCurve packed(system);
        REQUIRE(packed.TryPack(curve));
        const auto rocksFormat = static_cast<ue58pack::Format>(packed.Format());
        const auto rocksKeyFormat = static_cast<ue58pack::KeyFormat>(packed.KeyTimeFormat());
        ue58pack::Compressed ue(system);
        ue58pack::CompressCurve(reference, ue58richcurve::StateOf(curve), 1.0e-4f, 120.0f, ue,
                                options, &rocksFormat, &rocksKeyFormat);
        CHECK(SameBytes(packed.Packed(), ue.CompressedKeys.View()));
    }
}

// CV8 리뷰 2: 복원 시간이 넘치거나(min = 3·2^103, max = FLT_MAX — 마지막 복원 시간 Inf) 표본 간격이
// 비유한(denormal sampleRateHz → 1/rate = Inf, 표본 0개)이면 Validate가 거부하는 Quantized16을
// 만들지 않고 Float32다. 표본이 2^24개를 넘는 rate도 Float32다.
TEST(PackedTangentCurve_ExtremeRangesAndRatesStayFloat32) {
    SystemAllocator system;
    const double minimum = 3.0 * 10141204801825835211973625643008.0; // 3·2^103
    const double maximum = static_cast<double>(NumericLimits<float>::Max());
    Array<TangentCurveKey> keys(system);
    for (int32 index = 0; index < 7; ++index) {
        const float time = static_cast<float>(minimum + (maximum - minimum) * index / 6.0);
        REQUIRE(keys.TryPushBack(MakeKey(time, 1.0f)));
    }

    TangentCurve huge(system);
    REQUIRE(huge.TrySetKeys(keys.View()));
    huge.postExtrapolation = CurveExtrapolation::Linear;
    PackedTangentCurve packed(system);
    REQUIRE(packed.TryPack(huge, 1.0e-4f, 1.0e-38f));
    CHECK(packed.Format() == PackedCurveFormat::Linear);
    CHECK(packed.KeyTimeFormat() == PackedKeyTimeFormat::Float32);
    CHECK(PackedTangentCurve::Validate(packed));

    const TangentCurve linear = Linear8(system);
    REQUIRE(packed.TryPack(linear, 1.0e-4f, 1.0e-40f)); // denormal rate
    CHECK(packed.KeyTimeFormat() == PackedKeyTimeFormat::Float32);
    CHECK(PackedTangentCurve::Validate(packed));
    REQUIRE(packed.TryPack(linear, 1.0e-4f, 1.0e7f)); // 7초 × 1e7 Hz > 2^24 표본
    CHECK(packed.KeyTimeFormat() == PackedKeyTimeFormat::Float32);
    REQUIRE(packed.TryPack(linear, 1.0e-4f, 1.0e6f)); // 7e6 표본 — 상한 안
    CHECK(packed.KeyTimeFormat() == PackedKeyTimeFormat::Quantized16);
}

// CV8 리뷰 7: 이동된 쪽은 Empty 정규형이라 Validate를 통과하고 빈 커브로 평가된다(이동 생성·대입).
TEST(PackedTangentCurve_MovedFromIsEmpty) {
    SystemAllocator system;
    PackedTangentCurve source(system);
    REQUIRE(source.TryPack(Mixed3(system)));
    PackedTangentCurve moved(Move(source));
    CHECK(moved.Format() == PackedCurveFormat::Mixed && moved.KeyCount() == 3);
    CHECK(source.Format() == PackedCurveFormat::Empty && source.KeyCount() == 0 &&
          source.Packed().Size() == 0 && !source.EmptyDefault().HasValue());
    CHECK(source.PostExtrapolation() == CurveExtrapolation::Constant);
    CHECK(PackedTangentCurve::Validate(source));
    CHECK(source.Evaluate(1.0f, 4.0f) == 4.0f);

    PackedTangentCurve assigned(system);
    TangentCurve empty(system);
    empty.defaultValue = 2.0f;
    REQUIRE(assigned.TryPack(empty));
    assigned = Move(moved);
    CHECK(assigned.Format() == PackedCurveFormat::Mixed && !assigned.EmptyDefault().HasValue());
    CHECK(moved.Format() == PackedCurveFormat::Empty && PackedTangentCurve::Validate(moved));
}

// 시험 공백(CV8 리뷰): Weighted의 가중 byte가 3을 넘는 변조는 Validate와 평가가 모두 거부하고,
// Quantized16 범위 변조(NaN·음수 길이)는 Validate가 거부하며 평가는 메모리 안에서 끝난다.
TEST(PackedTangentCurve_TamperedWeightAndRange) {
    SystemAllocator system;
    using Access = PackedTangentCurveTestAccess;
    TangentCurve weightedCurve(system);
    TangentCurveKey keys[2] = {MakeKey(0.0f, 0.0f, CurveInterp::Cubic),
                               MakeKey(1.0f, 1.0f, CurveInterp::Cubic)};
    keys[0].tangentWeight = CurveTangentWeight::Both;
    keys[0].leaveTangentWeight = 0.5f;
    REQUIRE(weightedCurve.TrySetKeys(Span<const TangentCurveKey>(keys, 2)));
    PackedTangentCurve weighted(system);
    REQUIRE(weighted.TryPack(weightedCurve));
    REQUIRE(weighted.Format() == PackedCurveFormat::Weighted);
    Access::Packed(weighted)[1] = 4; // 첫 키의 가중 byte
    CHECK(!PackedTangentCurve::Validate(weighted));
    CHECK(platform::PlatformMath::IsNaN(weighted.Evaluate(0.5f)));
    CHECK(weighted.TryEvaluate(0.5f).Error() == ErrorCode::InvalidArgument);

    const uint32 bits[] = {platform::BitCast<uint32>(platform::PlatformMath::QuietNaN<float>()),
                           platform::BitCast<uint32>(-7.0f)};
    for (const uint32 value : bits) {
        PackedTangentCurve quantized(system);
        REQUIRE(quantized.TryPack(Linear8(system)));
        REQUIRE(quantized.KeyTimeFormat() == PackedKeyTimeFormat::Quantized16);
        for (usize index = 0; index < 4; ++index) {
            Access::Packed(quantized)[20 + index] = static_cast<uint8>(value >> (8 * index));
        }

        CHECK(!PackedTangentCurve::Validate(quantized));
        const float samples[] = {-1.0f, 0.0f, 3.5f,
                                 7.0f,  9.0f, platform::PlatformMath::QuietNaN<float>()};
        for (const float time : samples) {
            static_cast<void>(quantized.Evaluate(time)); // 범위 안 index만 읽는다
        }
    }
}

} // namespace rocks::test::packed_curve
