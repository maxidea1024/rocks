#pragma once

#include <Core/Result.h>
#include <Curves/TangentCurve.h>

// 커브 채널 여럿의 원자적 복사다(Docs/Design/CurveAssets 2.5·3.1.2절). 평탄 값의 TryCopyFrom과
// 원천의 TryFlattenInto가 함께 쓴다.
namespace rocks::curve_assets_detail {

// sources[i]를 targets[i]에 깊이 복사한다. 각 사본은 그 대상 채널의 allocator로 만들고(대상의 수명
// 계약을 바꾸지 않는다), 모두 성공한 뒤에만 옮긴다. 실패(할당)면 대상은 모두 그대로다.
template <usize Count>
Result<void> TryCopyChannels(TangentCurve* const (&targets)[Count],
                             const TangentCurve* const (&sources)[Count]) noexcept {
    // 사본은 이동만 되는 값이라 배열 대신 하나씩 만든다(Count는 3·4).
    static_assert(Count == 3 || Count == 4, "curve channel copy supports 3 or 4 channels");
    TangentCurve first(targets[0]->GetAllocator());
    TangentCurve second(targets[1]->GetAllocator());
    TangentCurve third(targets[2]->GetAllocator());
    TangentCurve fourth(targets[Count - 1]->GetAllocator());
    TangentCurve* staged[4] = {&first, &second, &third, &fourth};
    for (usize index = 0; index < Count; ++index) {
        if (auto copied = staged[index]->TryCopyFrom(*sources[index]); !copied) {
            return copied;
        }
    }

    for (usize index = 0; index < Count; ++index) {
        *targets[index] = Move(*staged[index]);
    }

    return Result<void>::Success();
}

} // namespace rocks::curve_assets_detail
