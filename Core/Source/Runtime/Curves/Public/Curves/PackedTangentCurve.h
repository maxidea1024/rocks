#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Optional.h>
#include <Core/Result.h>
#include <Curves/CurveTypes.h>
#include <Curves/TangentCurve.h>
#include <Meta/Annotations.h>

namespace rocks {

namespace curves_detail {
struct PackedTangentCurveTestAccess;
} // namespace curves_detail

// PackedTangentCurve의 압축 형식이다(UE ERichCurveCompressionFormat과 같은 번호 — 설계 11.1절).
// 형식 값은 키 데이터 배치의 보간 byte로도 쓰인다(Constant 1·Linear 2·Cubic 3 — 11.3절).
// - Empty: 키 없음(기본값만). Constant: 값 하나. Linear·Cubic: 모든 키가 그 보간. Mixed: 보간이
// 섞임.
//   Weighted: 가중 접선 구간이 있음.
ROCKS_ENUM()
enum class PackedCurveFormat : uint8 {
    Empty = 0,
    Constant = 1,
    Linear = 2,
    Cubic = 3,
    Mixed = 4,
    Weighted = 5,
};

// 키 시간의 저장 형식이다(UE ERichCurveKeyTimeCompressionFormat과 같은 번호 — 11.2절).
// - Quantized16: [첫 키, 마지막 키]를 0–65535로 양자화한 uint16 + 범위(min·길이 float 둘).
// - Float32: 키 시간 float 그대로.
ROCKS_ENUM()
enum class PackedKeyTimeFormat : uint8 {
    Quantized16 = 0,
    Float32 = 1,
};

// TangentCurve의 압축 저장 형태다(UE 5.8 FCompressedRichCurve — Docs/Design/CurveData.ko.md 11절,
// D13·D14·D18·D21). 쿡된 애니메이션 커브처럼 많이 읽는 런타임 데이터용이다(저작 데이터는
// TangentCurve). 만드는 길은 TryPack과 리플렉션 decode뿐이다(칸은 private PROPERTY).
//
// - 형식 결정(11.1절)은 UE를 고친 것이다: Weighted는 구간 단위 가중 판정, Constant는 키 값이
// 비트까지
//   같고 Cubic 키가 없으며 결과가 원본과 비트가 같을 때만(17.7절)이다.
// - 바이트 배치(11.3절): 보간 배열 → 키 시간 → 키 데이터, 정렬 틈 0, 모든 수 리틀 엔디언.
// - 평가는 풀어 낸 키 쌍으로 TangentCurve와 같은 식(math::EvalCurveSegment·같은 외삽 틀)을 쓴다.
// 그래서
//   Float32 시간 형식이면 원본 TangentCurve와 모든 시간에서 비트가 같다(D14). Quantized16은 복원한
//   키 시간으로 평가하므로 그 시간 오차만큼 다르다(오차는 압축 때 errorThreshold 안인지 본다).
// - 정합(11.4절, D21): 평가 진입은 enum 범위와 packed 크기 = ExpectedPackedSize(형식, 시간 형식, 키
//   수)를 O(1)로 본다. 어긋나면 Evaluate는 quiet NaN, TryEvaluate는 InvalidArgument다(정상
//   경로에서는 검증 hook과 private 칸 때문에 일어나지 않는다 — 메모리 밖 읽기를 막는 방어선). 검증
//   hook Validate는 그 정합과 byte 값·키 시간 정렬까지 보고 decode·대입·경로 쓰기에서 거부한다.
//
// 복사는 TryCopyFrom, 이동은 noexcept다. 바뀌지 않는 동안의 const 함수는 여러 스레드가 함께 불러도
// 된다.
//
// 사용 예:
//   PackedTangentCurve packed(allocator);
//   if (auto made = packed.TryPack(curve); !made) { return made; }
//   const float value = packed.Evaluate(1.5f);
ROCKS_STRUCT()
struct PackedTangentCurve {
    ROCKS_GENERATED_BODY()
public:
    // Empty 형식(키 없음·기본값 없음·외삽 Constant)이다. allocator는 packed byte 배열이 쓴다.
    explicit PackedTangentCurve(Allocator& allocator) noexcept : packed_(allocator) {
    }

    // other의 모든 칸을 복사한다. 실패(OutOfMemory)면 그대로다.
    Result<void> TryCopyFrom(const PackedTangentCurve& other) noexcept;

    // source를 압축해 이 값을 바꾼다(UE FRichCurve::CompressCurve). errorThreshold·sampleRateHz는
    // 키 시간 형식 판정(11.2절)에 쓴다: [첫 키, 마지막 키]를 sampleRateHz 간격으로 표본해
    // Quantized16으로 복원한 시간의 평가가 원본과 errorThreshold 안이고 더 작으면 Quantized16,
    // 아니면 Float32다 (errorThreshold·sampleRateHz가 0 이하·비유한이거나 표본이 2^24개를 넘으면
    // Float32 — UE는 0 이하만 본다). 실패: 할당 실패 OutOfMemory, 키가 uint32·packed
    // 한도(MaxElements)를 넘으면 CapacityExceeded — 실패하면 이 값은 그대로다.
    Result<void> TryPack(const TangentCurve& source, float errorThreshold = 1.0e-4f,
                         float sampleRateHz = 120.0f) noexcept;

    // 원래 TangentCurve 형태로 푼다(UE PopulateCurve를 고친 것 — 11.5절): 키 시간·값·보간·접선(가중
    // 형식은 가중 방식·가중치도), 접선 방식은 User, 외삽, Empty는 기본값. Constant는 그 값의 키
    // 하나(시간 0, Linear)다. 원래 접선 방식(Auto·Break)과 Empty가 아닌 형식의 기본값은 압축에서
    // 사라진다. 실패: 정합이 깨졌으면(Validate) InvalidArgument, 할당 실패 OutOfMemory — 실패하면
    // out은 그대로다.
    Result<void> TryUnpack(TangentCurve& out) const noexcept;

    // time의 값이다(11.5절). Empty는 기본값(없으면 fallback), Constant는 그 값, 그 밖은 풀어 낸
    // 키로 TangentCurve::Evaluate와 같은 규칙이다(NaN 시간은 키가 있으면 NaN). 정합이 깨졌으면
    // quiet NaN이다. 할당하지 않는다.
    float Evaluate(float time, float fallback = 0.0f) const noexcept;

    // Evaluate와 같고, 정합이 깨졌으면(O(1) 대조·잘못된 보간·가중 byte) InvalidArgument다.
    Result<float> TryEvaluate(float time, float fallback = 0.0f) const noexcept;

    PackedCurveFormat Format() const noexcept {
        return format_;
    }

    PackedKeyTimeFormat KeyTimeFormat() const noexcept {
        return keyTimeFormat_;
    }

    CurveExtrapolation PreExtrapolation() const noexcept {
        return preExtrapolation_;
    }

    CurveExtrapolation PostExtrapolation() const noexcept {
        return postExtrapolation_;
    }

    // Linear 이상 형식의 키 수다(Empty·Constant는 0).
    uint32 KeyCount() const noexcept {
        return keyCount_;
    }

    // Constant 형식의 값이다(그 밖은 0).
    float ConstantValue() const noexcept {
        return constantValue_;
    }

    // Empty 형식의 기본값이다(그 밖은 없음).
    const Optional<float>& EmptyDefault() const noexcept {
        return emptyDefault_;
    }

    // 압축 byte(11.3절 배치)다. 다음 TryPack까지 유효하다.
    Span<const uint8> Packed() const noexcept {
        return packed_.View();
    }

    // 형식·시간 형식·키 수의 packed 기대 크기다(11.3절 배치). Empty·Constant는 0이다. 계산이 넘치면
    // IndexNone이다.
    static usize ExpectedPackedSize(PackedCurveFormat format, PackedKeyTimeFormat keyTimeFormat,
                                    uint32 keyCount) noexcept;

    // 리플렉션 경로의 검증 hook이다(11.4절): enum 범위, 형식별 칸 정합(Empty·Constant는 키 0·packed
    // 비어 있음·시간 형식 Float32, Linear 이상은 키 ≥ 2·packed 크기 = 기대 크기, Empty가 아니면
    // 기본값 없음, Constant가 아니면 constantValue 비트 0), 보간 byte ∈ {1, 2, 3}, 가중 byte ∈
    // {0..3}, 키 시간 유한·비내림차순(Quantized16은 uint16 비내림차순과 min·길이 유한·길이 ≥ 0)을
    // 본다. 위반은 InvalidArgument다. 할당하지 않는다.
    static Result<void> Validate(const PackedTangentCurve& curve) noexcept;

private:
    friend struct curves_detail::PackedTangentCurveTestAccess;

    ROCKS_PROPERTY(Name = "format") PackedCurveFormat format_ = PackedCurveFormat::Empty;
    ROCKS_PROPERTY(Name = "keyTimeFormat")
    PackedKeyTimeFormat keyTimeFormat_ = PackedKeyTimeFormat::Float32;
    ROCKS_PROPERTY(Name = "preExtrapolation")
    CurveExtrapolation preExtrapolation_ = CurveExtrapolation::Constant;
    ROCKS_PROPERTY(Name = "postExtrapolation")
    CurveExtrapolation postExtrapolation_ = CurveExtrapolation::Constant;
    ROCKS_PROPERTY(Name = "keyCount") uint32 keyCount_ = 0;
    ROCKS_PROPERTY(Name = "constantValue") float constantValue_ = 0.0f;
    ROCKS_PROPERTY(Name = "emptyDefault") Optional<float> emptyDefault_;
    // 압축 byte다(Bytes wire — D18). 한도는 MaxArrayElements(1 MiB)다.
    ROCKS_PROPERTY(Name = "packed", MaxElements = 1048576) Array<uint8> packed_;
};

} // namespace rocks
