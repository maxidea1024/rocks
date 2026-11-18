#pragma once

#include <Base/Types.h>
#include <Curves/PackedTangentCurve.h>

// 시험이 압축 커브의 private 칸을 직접 바꾸는 모듈 내부 접근점이다(검증 hook을 거치지 않은 정합
// 위반 — 평가 진입의 O(1) 대조와 잘못된 byte 거부를 시험한다). 공개 API에는 칸을 따로 쓰는 길을
// 두지 않는다. Tests/ 소스만 include한다(정책 검사 test-access-include).
namespace rocks::curves_detail {

struct PackedTangentCurveTestAccess {
    static PackedCurveFormat& Format(PackedTangentCurve& curve) noexcept {
        return curve.format_;
    }

    static PackedKeyTimeFormat& KeyTimeFormat(PackedTangentCurve& curve) noexcept {
        return curve.keyTimeFormat_;
    }

    static CurveExtrapolation& PostExtrapolation(PackedTangentCurve& curve) noexcept {
        return curve.postExtrapolation_;
    }

    static uint32& KeyCount(PackedTangentCurve& curve) noexcept {
        return curve.keyCount_;
    }

    static float& ConstantValue(PackedTangentCurve& curve) noexcept {
        return curve.constantValue_;
    }

    static Optional<float>& EmptyDefault(PackedTangentCurve& curve) noexcept {
        return curve.emptyDefault_;
    }

    static Array<uint8>& Packed(PackedTangentCurve& curve) noexcept {
        return curve.packed_;
    }
};

} // namespace rocks::curves_detail
