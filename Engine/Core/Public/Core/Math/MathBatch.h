#pragma once

#include <Core/Containers/Span.h>
#include <Core/Math/Matrix4.h>

namespace rocks::math {

// 연속 배열의 일괄 변환이다. 원소마다 선택된 platform::simd(SSE2/NEON) 경로로 계산하며 결과는
// Mat4::TransformPoint/TransformVector를 원소별로 호출한 것과 같다.
// - input과 output의 원소 수가 다르면 Fatal이다. 0개는 아무 작업도 하지 않는다.
// - output이 input과 정확히 같은 범위(in-place)이면 허용한다. 부분적으로 겹치는 범위는 금지하며
//   Fatal로 거부한다(겹친 원소를 읽기 전에 덮어쓸 수 있기 때문이다).
// - 정렬 요구는 없다. Vec3는 12-byte 원소이며 padding을 가정하지 않는다.
namespace detail {

inline void CheckBatchSpans(Span<const Vec3> input, Span<Vec3> output) noexcept {
    if (input.Size() != output.Size()) {
        platform::Fatal("Math batch input/output sizes differ");
    }
    if (input.Size() == 0 || input.Data() == output.Data()) {
        return;
    }
    const auto inputBegin = reinterpret_cast<uptr>(input.Data());
    const auto outputBegin = reinterpret_cast<uptr>(output.Data());
    const uptr bytes = input.Size() * sizeof(Vec3);
    const bool disjoint = inputBegin + bytes <= outputBegin || outputBegin + bytes <= inputBegin;
    if (!disjoint) {
        platform::Fatal("Math batch input/output ranges partially overlap");
    }
}

inline void TransformBatch(const Mat4& matrix, Span<const Vec3> input, Span<Vec3> output,
                           float w) noexcept {
    CheckBatchSpans(input, output);
    for (usize index = 0; index < input.Size(); ++index) {
        const Vec3 source = input[index];
        const auto transformed = platform::simd::TransformVector4(
            matrix.m, platform::simd::Set(source.x, source.y, source.z, w));
        output[index] =
            Vec3(platform::simd::Get<0>(transformed), platform::simd::Get<1>(transformed),
                 platform::simd::Get<2>(transformed));
    }
}

} // namespace detail

// affine 행렬 전제의 점 일괄 변환(w=1)이다.
inline void TransformPoints(const Mat4& matrix, Span<const Vec3> input,
                            Span<Vec3> output) noexcept {
    detail::TransformBatch(matrix, input, output, 1.0f);
}

// 방향 일괄 변환(w=0, 이동 무시)이다.
inline void TransformVectors(const Mat4& matrix, Span<const Vec3> input,
                             Span<Vec3> output) noexcept {
    detail::TransformBatch(matrix, input, output, 0.0f);
}

} // namespace rocks::math
