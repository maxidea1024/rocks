#include <Curves/PackedTangentCurve.h>

#include <Base/Checked.h>
#include <Meta/TypeSchema.h>
#include <Platform/Memory.h>
#include "CurveKeyEdit.h"

// PackedTangentCurve 압축·평가·풀기·정합이다(Docs/Design/CurveData/README.ko.md 11절 — UE 5.8
// RichCurve.cpp FindRichCurveCompressionFormat 1279–1330·FindRichCurveKeyFormat
// 1332–1475·CompressCurve 1477–1682·키 시간·데이터 어댑터 1694–1947·InterpEval
// 2016–2135·PopulateCurve 2441–2452를 고친 것).
namespace rocks {

namespace {

// 양자화 비율 1/65535다(UE Quantized16BitKeyTimeAdapter::QuantizationScale).
constexpr float QuantizationScale = 1.0f / 65535.0f;

// 표본 수 상한이다(Bake와 같다 — 넘으면 Float32로 둔다).
constexpr usize MaxQuantizationSamples = usize{1} << 24;

constexpr usize AlignUp(usize value, usize alignment) noexcept {
    return (value + alignment - 1) / alignment * alignment;
}

// 11.3절 배치의 위치들이다. total이 IndexNone이면 계산이 넘쳤다.
struct PackedLayout {
    usize interpBytesPerKey = 0; // Mixed 1, Weighted 2
    usize timesOffset = 0;
    usize rangeOffset = 0; // Quantized16만
    usize dataOffset = 0;
    usize dataStride = 0; // 키당 float 수
    usize total = 0;
};

bool IsKeyedFormat(PackedCurveFormat format) noexcept {
    return format == PackedCurveFormat::Linear || format == PackedCurveFormat::Cubic ||
           format == PackedCurveFormat::Mixed || format == PackedCurveFormat::Weighted;
}

PackedLayout ComputeLayout(PackedCurveFormat format, PackedKeyTimeFormat keyTimeFormat,
                           usize count) noexcept {
    PackedLayout layout;
    layout.interpBytesPerKey = format == PackedCurveFormat::Mixed      ? 1
                               : format == PackedCurveFormat::Weighted ? 2
                                                                       : 0;
    layout.dataStride = format == PackedCurveFormat::Linear     ? 1
                        : format == PackedCurveFormat::Weighted ? 5
                                                                : 3;
    usize interpBytes = 0;
    usize timeBytes = 0;
    usize dataFloats = 0;
    usize dataBytes = 0;
    const usize timeSize = keyTimeFormat == PackedKeyTimeFormat::Quantized16 ? 2 : 4;
    if (!TryMultiply(count, layout.interpBytesPerKey, interpBytes) ||
        !TryMultiply(count, timeSize, timeBytes) ||
        !TryMultiply(count, layout.dataStride, dataFloats) ||
        !TryMultiply(dataFloats, usize{4}, dataBytes)) {
        layout.total = IndexNone;
        return layout;
    }

    if (keyTimeFormat == PackedKeyTimeFormat::Quantized16) {
        layout.timesOffset = AlignUp(interpBytes, 2);
        layout.rangeOffset = AlignUp(layout.timesOffset + timeBytes, 4);
        layout.dataOffset = layout.rangeOffset + 2 * 4;
    } else {
        layout.timesOffset = AlignUp(interpBytes, 4);
        layout.dataOffset = layout.timesOffset + timeBytes;
    }

    if (!TryAdd(layout.dataOffset, dataBytes, layout.total)) {
        layout.total = IndexNone;
    }

    return layout;
}

// ---- 리틀 엔디언 읽기·쓰기 ----

uint16 ReadU16(Span<const uint8> bytes, usize offset) noexcept {
    return static_cast<uint16>(bytes[offset] | (bytes[offset + 1] << 8));
}

float ReadFloat(Span<const uint8> bytes, usize offset) noexcept {
    const uint32 bits = static_cast<uint32>(bytes[offset]) |
                        (static_cast<uint32>(bytes[offset + 1]) << 8) |
                        (static_cast<uint32>(bytes[offset + 2]) << 16) |
                        (static_cast<uint32>(bytes[offset + 3]) << 24);
    return platform::BitCast<float>(bits);
}

void WriteU16(Array<uint8>& bytes, usize offset, uint16 value) noexcept {
    bytes[offset] = static_cast<uint8>(value & 0xFF);
    bytes[offset + 1] = static_cast<uint8>(value >> 8);
}

void WriteFloat(Array<uint8>& bytes, usize offset, float value) noexcept {
    const uint32 bits = platform::BitCast<uint32>(value);
    bytes[offset] = static_cast<uint8>(bits & 0xFF);
    bytes[offset + 1] = static_cast<uint8>((bits >> 8) & 0xFF);
    bytes[offset + 2] = static_cast<uint8>((bits >> 16) & 0xFF);
    bytes[offset + 3] = static_cast<uint8>(bits >> 24);
}

// ---- 보간 byte(형식 값 — 11.3절) ----

uint8 InterpCode(CurveInterp interp) noexcept {
    return interp == CurveInterp::Linear  ? static_cast<uint8>(PackedCurveFormat::Linear)
           : interp == CurveInterp::Cubic ? static_cast<uint8>(PackedCurveFormat::Cubic)
                                          : static_cast<uint8>(PackedCurveFormat::Constant);
}

bool InterpFromCode(uint8 code, CurveInterp& interp) noexcept {
    if (code == static_cast<uint8>(PackedCurveFormat::Linear)) {
        interp = CurveInterp::Linear;
    } else if (code == static_cast<uint8>(PackedCurveFormat::Cubic)) {
        interp = CurveInterp::Cubic;
    } else if (code == static_cast<uint8>(PackedCurveFormat::Constant)) {
        interp = CurveInterp::Constant;
    } else {
        return false;
    }

    return true;
}

// UE FMath::RoundHalfFromZero(float)(UnrealMath.cpp 3033–3046)의 음수 아닌 갈래다(양자화 입력은
// [0, 65535]).
float RoundHalfFromZeroNonNegative(float value) noexcept {
    const float integral = math::Floor(value);
    const float fraction = value - integral;
    return fraction < 0.5f ? integral : integral + 1.0f;
}

// 키 시간의 16비트 양자화(UE CompressCurve 1634–1635)와 복원(Quantized16BitKeyTimeAdapter::GetTime
// 1720–1721)이다.
// Clamp는 UE FMath::Clamp(Max(Min(X, 1), 0) — UnrealMathUtility.h 592–595)와 같은 선택이다(NaN이면
// 1).
uint16 QuantizeTime(float time, float minimum, float inverseDelta) noexcept {
    const float scaled = (time - minimum) * inverseDelta;
    const float lower = scaled < 1.0f ? scaled : 1.0f;
    const float normalized = 0.0f < lower ? lower : 0.0f;
    return static_cast<uint16>(RoundHalfFromZeroNonNegative(normalized * 65535.0f));
}

float RestoreTime(uint16 quantized, float minimum, float delta) noexcept {
    const float normalized = static_cast<float>(quantized) * QuantizationScale;
    return (normalized * delta) + minimum;
}

// 압축 형식이 담는 칸만 남긴 키다(풀어 낸 키와 같다 — 키 시간 판정의 손실 평가가 쓴다).
TangentCurveKey StoredKey(const TangentCurveKey& source, PackedCurveFormat format) noexcept {
    TangentCurveKey key;
    key.time = source.time;
    key.value = source.value;
    key.tangentMode = CurveTangentMode::User;
    if (format == PackedCurveFormat::Linear) {
        key.interp = CurveInterp::Linear;
        return key;
    }

    key.interp = format == PackedCurveFormat::Cubic ? CurveInterp::Cubic : source.interp;
    key.arriveTangent = source.arriveTangent;
    key.leaveTangent = source.leaveTangent;
    if (format == PackedCurveFormat::Weighted) {
        key.tangentWeight = source.tangentWeight;
        key.arriveTangentWeight = source.arriveTangentWeight;
        key.leaveTangentWeight = source.leaveTangentWeight;
    }

    return key;
}

// 형식 결정(11.1절 — UE FindRichCurveCompressionFormat을 고친 것).
PackedCurveFormat ChooseFormat(const TangentCurve& source) noexcept {
    const Span<const TangentCurveKey> keys = source.Keys();
    if (keys.Size() == 0) {
        return PackedCurveFormat::Empty;
    }

    if (keys.Size() == 1) {
        return PackedCurveFormat::Constant;
    }

    // Constant: Cubic 키가 없고 값이 비트까지 같은 유한 값이며, 원본과 결과가 비트까지 같은
    // 경우(Linear 외삽은 0 기울기 × ±Inf 시간 = NaN, ±Inf 값은 Lerp·주기 offset이 NaN이라 값 하나로
    // 줄이면 달라진다 — 17.7절).
    bool constant = source.preExtrapolation != CurveExtrapolation::Linear &&
                    source.postExtrapolation != CurveExtrapolation::Linear &&
                    math::IsFinite(keys[0].value);
    const uint32 firstBits = platform::BitCast<uint32>(keys[0].value);
    for (usize index = 0; constant && index < keys.Size(); ++index) {
        constant = keys[index].interp != CurveInterp::Cubic &&
                   platform::BitCast<uint32>(keys[index].value) == firstBits;
    }

    if (constant) {
        return PackedCurveFormat::Constant;
    }

    for (usize index = 0; index + 1 < keys.Size(); ++index) {
        if (keys[index].interp == CurveInterp::Cubic &&
            !math::IsCurveSegmentUnweighted(keys[index], keys[index + 1])) {
            return PackedCurveFormat::Weighted;
        }
    }

    for (const TangentCurveKey& key : keys) {
        if (key.interp != keys[0].interp) {
            return PackedCurveFormat::Mixed;
        }
    }

    return keys[0].interp == CurveInterp::Linear  ? PackedCurveFormat::Linear
           : keys[0].interp == CurveInterp::Cubic ? PackedCurveFormat::Cubic
                                                  : PackedCurveFormat::Mixed;
}

// 압축 형태의 평가식이다(TangentCurve::Evaluate와 같은 틀·같은 구간 식 — D14).
template <class Keys>
float EvaluatePacked(CurveExtrapolation pre, CurveExtrapolation post, const Keys& keys, float time,
                     float fallback) noexcept {
    return curves_detail::EvaluateCurve(
        Optional<float>{}, pre, post, keys, time, fallback, false,
        [](const TangentCurveKey& first, const TangentCurveKey& second, float at) noexcept {
            return math::EvalCurveSegment(first, second, at);
        });
}

// 키 시간 형식 판정(11.2절 — UE FindRichCurveKeyFormat을 고친 것). 표본 시간은 누적하지 않고
// min + k/rate로 만들고, 원본과 손실 쪽 모두 Evaluate와 같은 평가(같은 구간 탐색·외삽)로 비교한다 —
// 손실 쪽은 압축 형태가 실제로 평가하는 키(StoredKey + 복원 시간)다. 할당 실패는 오류다.
Result<PackedKeyTimeFormat> ChooseKeyTimeFormat(Allocator& allocator, const TangentCurve& source,
                                                PackedCurveFormat format, float errorThreshold,
                                                float sampleRateHz) noexcept {
    const Span<const TangentCurveKey> keys = source.Keys();
    const usize count = keys.Size();
    const auto float32 = PackedKeyTimeFormat::Float32;
    if (!IsKeyedFormat(format) || !math::IsFinite(errorThreshold) || !(errorThreshold > 0.0f) ||
        !math::IsFinite(sampleRateHz) || !(sampleRateHz > 0.0f)) {
        return Result<PackedKeyTimeFormat>::Success(float32);
    }

    const float minimum = keys[0].time;
    const float maximum = keys[count - 1].time;
    const float delta = maximum - minimum;
    const float inverseDelta = 1.0f / delta;
    if (!math::IsFinite(delta) || !(delta > 0.0f) || !math::IsFinite(inverseDelta)) {
        // 시간이 모두 같거나(UE는 1/0과 NaN Clamp로 판정한다) 길이가 denormal이라 역수가 넘치면
        // 양자화 범위가 없다.
        return Result<PackedKeyTimeFormat>::Success(float32);
    }

    const usize size16 = ComputeLayout(format, PackedKeyTimeFormat::Quantized16, count).total;
    const usize size32 = ComputeLayout(format, PackedKeyTimeFormat::Float32, count).total;
    if (!(size16 < size32)) {
        return Result<PackedKeyTimeFormat>::Success(float32);
    }

    const float increment = 1.0f / sampleRateHz;
    if (!math::IsFinite(increment)) {
        // denormal sampleRateHz는 간격이 Inf라 표본이 없다(판정 없이 Quantized16이 되지 않게).
        return Result<PackedKeyTimeFormat>::Success(float32);
    }

    usize samples = 0;
    while (minimum + static_cast<float>(samples) * increment <= maximum) {
        if (samples == MaxQuantizationSamples) {
            return Result<PackedKeyTimeFormat>::Success(float32);
        }

        ++samples;
    }

    Array<TangentCurveKey> stored(allocator);
    Array<TangentCurveKey> lossy(allocator);
    if (auto reserved = stored.TryReserve(count); !reserved) {
        return Result<PackedKeyTimeFormat>::Failure(reserved.Error());
    }

    if (auto reserved = lossy.TryReserve(count); !reserved) {
        return Result<PackedKeyTimeFormat>::Failure(reserved.Error());
    }

    for (const TangentCurveKey& key : keys) {
        TangentCurveKey kept = StoredKey(key, format);
        static_cast<void>(stored.TryPushBack(kept)); // 예약했으므로 실패하지 않는다
        kept.time = RestoreTime(QuantizeTime(key.time, minimum, inverseDelta), minimum, delta);
        if (!math::IsFinite(kept.time)) {
            // 복원 시간이 넘치면(min·길이가 float 끝 근처) Validate가 거부하는 값이 된다.
            return Result<PackedKeyTimeFormat>::Success(float32);
        }

        static_cast<void>(lossy.TryPushBack(kept));
    }

    for (usize sample = 0; sample < samples; ++sample) {
        const float time = minimum + static_cast<float>(sample) * increment;
        const float expected = EvaluatePacked(source.preExtrapolation, source.postExtrapolation,
                                              stored.View(), time, 0.0f);
        const float actual = EvaluatePacked(source.preExtrapolation, source.postExtrapolation,
                                            lossy.View(), time, 0.0f);
        if (!(math::Abs(expected - actual) <= errorThreshold)) {
            return Result<PackedKeyTimeFormat>::Success(float32);
        }
    }

    return Result<PackedKeyTimeFormat>::Success(PackedKeyTimeFormat::Quantized16);
}

// packed byte를 TangentCurve 키로 풀어 내는 보기다(EvaluateCurve의 키 목록). 잘못된 보간·가중
// byte를 만나면 invalid를 세운다(그 키는 Linear로 읽는다 — 호출자가 결과를 버린다).
class PackedKeyView {
public:
    PackedKeyView(Span<const uint8> bytes, PackedCurveFormat format,
                  PackedKeyTimeFormat keyTimeFormat, usize count) noexcept
        : bytes_(bytes), format_(format), keyTimeFormat_(keyTimeFormat), count_(count),
          layout_(ComputeLayout(format, keyTimeFormat, count)) {
        if (keyTimeFormat_ == PackedKeyTimeFormat::Quantized16) {
            minimum_ = ReadFloat(bytes_, layout_.rangeOffset);
            delta_ = ReadFloat(bytes_, layout_.rangeOffset + 4);
        }
    }

    usize Size() const noexcept {
        return count_;
    }

    float TimeAt(usize index) const noexcept {
        if (keyTimeFormat_ == PackedKeyTimeFormat::Quantized16) {
            return RestoreTime(ReadU16(bytes_, layout_.timesOffset + index * 2), minimum_, delta_);
        }

        return ReadFloat(bytes_, layout_.timesOffset + index * 4);
    }

    TangentCurveKey operator[](usize index) const noexcept {
        TangentCurveKey key;
        key.time = TimeAt(index);
        key.tangentMode = CurveTangentMode::User;
        const usize data = layout_.dataOffset + index * layout_.dataStride * 4;
        key.value = ReadFloat(bytes_, data);
        if (format_ == PackedCurveFormat::Linear) {
            key.interp = CurveInterp::Linear;
            return key;
        }

        key.arriveTangent = ReadFloat(bytes_, data + 4);
        key.leaveTangent = ReadFloat(bytes_, data + 8);
        if (format_ == PackedCurveFormat::Cubic) {
            key.interp = CurveInterp::Cubic;
            return key;
        }

        const usize code = index * layout_.interpBytesPerKey;
        if (!InterpFromCode(bytes_[code], key.interp)) {
            invalid_ = true;
            key.interp = CurveInterp::Linear;
        }

        if (format_ == PackedCurveFormat::Weighted) {
            const uint8 weight = bytes_[code + 1];
            if (weight > static_cast<uint8>(CurveTangentWeight::Both)) {
                invalid_ = true;
            } else {
                key.tangentWeight = static_cast<CurveTangentWeight>(weight);
            }

            key.arriveTangentWeight = ReadFloat(bytes_, data + 12);
            key.leaveTangentWeight = ReadFloat(bytes_, data + 16);
        }

        return key;
    }

    bool Invalid() const noexcept {
        return invalid_;
    }

    const PackedLayout& Layout() const noexcept {
        return layout_;
    }

private:
    Span<const uint8> bytes_;
    PackedCurveFormat format_;
    PackedKeyTimeFormat keyTimeFormat_;
    usize count_;
    PackedLayout layout_;
    float minimum_ = 0.0f;
    float delta_ = 0.0f;
    mutable bool invalid_ = false;
};

bool IsDeclared(PackedCurveFormat format) noexcept {
    return static_cast<uint8>(format) <= static_cast<uint8>(PackedCurveFormat::Weighted);
}

bool IsDeclared(PackedKeyTimeFormat format) noexcept {
    return static_cast<uint8>(format) <= static_cast<uint8>(PackedKeyTimeFormat::Float32);
}

bool IsDeclared(CurveExtrapolation extrapolation) noexcept {
    return static_cast<uint8>(extrapolation) <= static_cast<uint8>(CurveExtrapolation::Constant);
}

} // namespace

usize PackedTangentCurve::ExpectedPackedSize(PackedCurveFormat format,
                                             PackedKeyTimeFormat keyTimeFormat,
                                             uint32 keyCount) noexcept {
    if (!IsKeyedFormat(format)) {
        return 0;
    }

    return ComputeLayout(format, keyTimeFormat, keyCount).total;
}

// 평가 진입의 O(1) 정합 대조다(11.4절, D21).
static bool IsPackedConsistent(PackedCurveFormat format, PackedKeyTimeFormat keyTimeFormat,
                               CurveExtrapolation pre, CurveExtrapolation post, uint32 keyCount,
                               usize packedSize) noexcept {
    if (!IsDeclared(format) || !IsDeclared(keyTimeFormat) || !IsDeclared(pre) ||
        !IsDeclared(post)) {
        return false;
    }

    if (!IsKeyedFormat(format)) {
        return keyCount == 0 && packedSize == 0;
    }

    return keyCount >= 2 &&
           packedSize == PackedTangentCurve::ExpectedPackedSize(format, keyTimeFormat, keyCount);
}

PackedTangentCurve::PackedTangentCurve(PackedTangentCurve&& other) noexcept
    : format_(other.format_), keyTimeFormat_(other.keyTimeFormat_),
      preExtrapolation_(other.preExtrapolation_), postExtrapolation_(other.postExtrapolation_),
      keyCount_(other.keyCount_), constantValue_(other.constantValue_),
      emptyDefault_(other.emptyDefault_), packed_(Move(other.packed_)) {
    other.ResetFieldsToEmpty();
}

PackedTangentCurve& PackedTangentCurve::operator=(PackedTangentCurve&& other) noexcept {
    if (this != &other) {
        format_ = other.format_;
        keyTimeFormat_ = other.keyTimeFormat_;
        preExtrapolation_ = other.preExtrapolation_;
        postExtrapolation_ = other.postExtrapolation_;
        keyCount_ = other.keyCount_;
        constantValue_ = other.constantValue_;
        emptyDefault_ = other.emptyDefault_;
        packed_ = Move(other.packed_);
        other.ResetFieldsToEmpty();
    }

    return *this;
}

void PackedTangentCurve::ResetFieldsToEmpty() noexcept {
    format_ = PackedCurveFormat::Empty;
    keyTimeFormat_ = PackedKeyTimeFormat::Float32;
    preExtrapolation_ = CurveExtrapolation::Constant;
    postExtrapolation_ = CurveExtrapolation::Constant;
    keyCount_ = 0;
    constantValue_ = 0.0f;
    emptyDefault_ = Optional<float>{};
    packed_.Clear(); // 이동된 배열은 이미 비었다(자기 이동이 아닌 한 할 일 없음)
}

Result<void> PackedTangentCurve::TryCopyFrom(const PackedTangentCurve& other) noexcept {
    if (this == &other) {
        return Result<void>::Success();
    }

    Array<uint8> packed(packed_.GetAllocator());
    if (auto reserved = packed.TryReserve(other.packed_.Size()); !reserved) {
        return reserved;
    }

    static_cast<void>(packed.TryAppend(other.packed_.View())); // 예약했으므로 실패하지 않는다
    format_ = other.format_;
    keyTimeFormat_ = other.keyTimeFormat_;
    preExtrapolation_ = other.preExtrapolation_;
    postExtrapolation_ = other.postExtrapolation_;
    keyCount_ = other.keyCount_;
    constantValue_ = other.constantValue_;
    emptyDefault_ = other.emptyDefault_;
    packed_ = Move(packed);
    return Result<void>::Success();
}

Result<void> PackedTangentCurve::TryPack(const TangentCurve& source, float errorThreshold,
                                         float sampleRateHz) noexcept {
    const Span<const TangentCurveKey> keys = source.Keys();
    if (keys.Size() > static_cast<usize>(NumericLimits<uint32>::Max())) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    const PackedCurveFormat format = ChooseFormat(source);
    auto keyTimeFormat =
        ChooseKeyTimeFormat(packed_.GetAllocator(), source, format, errorThreshold, sampleRateHz);
    if (!keyTimeFormat) {
        return Result<void>::Failure(keyTimeFormat.Error());
    }

    const uint32 count = IsKeyedFormat(format) ? static_cast<uint32>(keys.Size()) : 0;
    const PackedLayout layout = ComputeLayout(format, keyTimeFormat.Value(), count);
    const usize total = IsKeyedFormat(format) ? layout.total : 0;
    if (total == IndexNone || total > MaxArrayElements) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    Array<uint8> packed(packed_.GetAllocator());
    if (auto sized = packed.TryResize(total); !sized) {
        return sized;
    }

    for (usize index = 0; index < total; ++index) {
        packed[index] = 0; // 정렬 틈은 0이다(UE AddZeroed)
    }

    if (IsKeyedFormat(format)) {
        // 보간 배열(11.3절 — 형식 값, Weighted는 가중 방식이 뒤따른다).
        for (usize index = 0; index < keys.Size(); ++index) {
            if (layout.interpBytesPerKey >= 1) {
                packed[index * layout.interpBytesPerKey] = InterpCode(keys[index].interp);
            }

            if (layout.interpBytesPerKey == 2) {
                packed[index * 2 + 1] = static_cast<uint8>(keys[index].tangentWeight);
            }
        }

        // 키 시간.
        if (keyTimeFormat.Value() == PackedKeyTimeFormat::Quantized16) {
            const float minimum = keys[0].time;
            const float delta = keys[keys.Size() - 1].time - minimum;
            const float inverseDelta = 1.0f / delta;
            for (usize index = 0; index < keys.Size(); ++index) {
                WriteU16(packed, layout.timesOffset + index * 2,
                         QuantizeTime(keys[index].time, minimum, inverseDelta));
            }

            WriteFloat(packed, layout.rangeOffset, minimum);
            WriteFloat(packed, layout.rangeOffset + 4, delta);
        } else {
            for (usize index = 0; index < keys.Size(); ++index) {
                WriteFloat(packed, layout.timesOffset + index * 4, keys[index].time);
            }
        }

        // 키 데이터(키별 교차 배치).
        for (usize index = 0; index < keys.Size(); ++index) {
            const TangentCurveKey& key = keys[index];
            const usize data = layout.dataOffset + index * layout.dataStride * 4;
            WriteFloat(packed, data, key.value);
            if (layout.dataStride >= 3) {
                WriteFloat(packed, data + 4, key.arriveTangent);
                WriteFloat(packed, data + 8, key.leaveTangent);
            }

            if (layout.dataStride == 5) {
                WriteFloat(packed, data + 12, key.arriveTangentWeight);
                WriteFloat(packed, data + 16, key.leaveTangentWeight);
            }
        }
    }

    format_ = format;
    keyTimeFormat_ = keyTimeFormat.Value();
    preExtrapolation_ = source.preExtrapolation;
    postExtrapolation_ = source.postExtrapolation;
    keyCount_ = count;
    constantValue_ = format == PackedCurveFormat::Constant ? keys[0].value : 0.0f;
    emptyDefault_ = format == PackedCurveFormat::Empty ? source.defaultValue : Optional<float>{};
    packed_ = Move(packed);
    return Result<void>::Success();
}

Result<float> PackedTangentCurve::TryEvaluate(float time, float fallback) const noexcept {
    if (!IsPackedConsistent(format_, keyTimeFormat_, preExtrapolation_, postExtrapolation_,
                            keyCount_, packed_.Size())) {
        return Result<float>::Failure(ErrorCode::InvalidArgument);
    }

    if (format_ == PackedCurveFormat::Empty) {
        // TangentCurve의 빈 커브와 같이 0을 더한다(−0은 +0이 된다).
        const float value = emptyDefault_.HasValue() ? emptyDefault_.Value() : fallback;
        return Result<float>::Success(value + 0.0f);
    }

    if (format_ == PackedCurveFormat::Constant) {
        if (math::IsNaN(time)) {
            return Result<float>::Success(platform::PlatformMath::QuietNaN<float>());
        }

        return Result<float>::Success(constantValue_ + 0.0f);
    }

    const PackedKeyView view(packed_.View(), format_, keyTimeFormat_, keyCount_);
    const float value = EvaluatePacked(preExtrapolation_, postExtrapolation_, view, time, fallback);
    if (view.Invalid()) {
        return Result<float>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<float>::Success(value);
}

float PackedTangentCurve::Evaluate(float time, float fallback) const noexcept {
    const Result<float> value = TryEvaluate(time, fallback);
    return value ? value.Value() : platform::PlatformMath::QuietNaN<float>();
}

Result<void> PackedTangentCurve::TryUnpack(TangentCurve& out) const noexcept {
    if (auto valid = Validate(*this); !valid) {
        return valid;
    }

    Array<TangentCurveKey> keys(packed_.GetAllocator());
    if (format_ == PackedCurveFormat::Constant) {
        TangentCurveKey key;
        key.value = constantValue_;
        key.interp = CurveInterp::Linear;
        key.tangentMode = CurveTangentMode::User;
        if (auto pushed = keys.TryPushBack(key); !pushed) {
            return pushed;
        }
    } else if (IsKeyedFormat(format_)) {
        if (auto reserved = keys.TryReserve(keyCount_); !reserved) {
            return reserved;
        }

        const PackedKeyView view(packed_.View(), format_, keyTimeFormat_, keyCount_);
        for (usize index = 0; index < keyCount_; ++index) {
            static_cast<void>(keys.TryPushBack(view[index])); // 예약했으므로 실패하지 않는다
        }
    }

    if (auto set = out.TrySetKeys(keys.View()); !set) {
        return set;
    }

    out.preExtrapolation = preExtrapolation_;
    out.postExtrapolation = postExtrapolation_;
    out.defaultValue = format_ == PackedCurveFormat::Empty ? emptyDefault_ : Optional<float>{};
    return Result<void>::Success();
}

Result<void> PackedTangentCurve::Validate(const PackedTangentCurve& curve) noexcept {
    const auto invalid = [] { return Result<void>::Failure(ErrorCode::InvalidArgument); };
    if (!IsPackedConsistent(curve.format_, curve.keyTimeFormat_, curve.preExtrapolation_,
                            curve.postExtrapolation_, curve.keyCount_, curve.packed_.Size())) {
        return invalid();
    }

    const bool empty = curve.format_ == PackedCurveFormat::Empty;
    const bool constant = curve.format_ == PackedCurveFormat::Constant;
    if ((!empty && curve.emptyDefault_.HasValue()) ||
        (!constant && platform::BitCast<uint32>(curve.constantValue_) != 0)) {
        return invalid();
    }

    if (empty || constant) {
        return curve.keyTimeFormat_ == PackedKeyTimeFormat::Float32 ? Result<void>::Success()
                                                                    : invalid();
    }

    const Span<const uint8> bytes = curve.packed_.View();
    const PackedKeyView view(bytes, curve.format_, curve.keyTimeFormat_, curve.keyCount_);
    const PackedLayout& layout = view.Layout();
    for (usize index = 0; index < curve.keyCount_; ++index) {
        if (layout.interpBytesPerKey >= 1) {
            CurveInterp interp;
            if (!InterpFromCode(bytes[index * layout.interpBytesPerKey], interp)) {
                return invalid();
            }
        }

        if (layout.interpBytesPerKey == 2 &&
            bytes[index * 2 + 1] > static_cast<uint8>(CurveTangentWeight::Both)) {
            return invalid();
        }
    }

    if (curve.keyTimeFormat_ == PackedKeyTimeFormat::Quantized16) {
        const float minimum = ReadFloat(bytes, layout.rangeOffset);
        const float delta = ReadFloat(bytes, layout.rangeOffset + 4);
        if (!math::IsFinite(minimum) || !math::IsFinite(delta) || !(delta >= 0.0f)) {
            return invalid();
        }

        for (usize index = 1; index < curve.keyCount_; ++index) {
            if (ReadU16(bytes, layout.timesOffset + (index - 1) * 2) >
                ReadU16(bytes, layout.timesOffset + index * 2)) {
                return invalid();
            }
        }

        // 복원 시간이 유한한지(min·길이가 커 합이 넘치는 경우).
        if (!math::IsFinite(view.TimeAt(curve.keyCount_ - 1))) {
            return invalid();
        }
    } else {
        for (usize index = 0; index < curve.keyCount_; ++index) {
            const float time = view.TimeAt(index);
            if (!math::IsFinite(time) || (index > 0 && view.TimeAt(index - 1) > time)) {
                return invalid();
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks
