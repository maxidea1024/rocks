#include <Core/Math/IntVector.h>
#include <Core/Math/MathBatch.h>
#include <Core/Math/Quaternion.h>
#include <TestSupport/HostFailure.h>
#include <Platform/PlatformMath.h>

// 수학 계약 위반이 잘못된 값으로 계속 진행되지 않고 Fatal로 끝나는지 자식 프로세스에서 확인한다.
// 각 operation은 정확히 하나의 위반을 일으킨다. 반환에 도달하면 하네스가 실패로 판정한다.
namespace rocks::test {

int RunMathNegativeControl(uint32 operation) noexcept {
    PrepareFatalTestProcess();
    volatile double nan = platform::PlatformMath::QuietNaN<double>();
    volatile double justAboveInt32 = 2147483647.5;
    Vec3 values[4] = {};
    switch (operation) {
    case 0:
        // NaN을 0 또는 CPU의 integer indefinite 값으로 바꾸면 안 된다.
        return math::FloorToInt32(static_cast<float>(nan));
    case 1:
        // 반올림 결과 2^31은 int32 밖이다. SSE 보정 단계의 overflow 경로를 거친다.
        return math::RoundToInt32(static_cast<double>(justAboveInt32));
    case 2:
        return (IntVec2(2147483647, 0) + IntVec2(1, 0)).x;
    case 3:
        math::TransformPoints(Mat4(), Span<const Vec3>(values, 2), Span<Vec3>(values, 3));
        return 0;
    case 4:
        // 한 원소 어긋난 부분 겹침은 읽기 전 덮어쓰기가 생기므로 거부한다.
        math::TransformPoints(Mat4(), Span<const Vec3>(values, 3), Span<Vec3>(values + 1, 3));
        return 0;
    case 5:
        return static_cast<int>(Mat4().At(4, 0));
    case 6:
        return static_cast<int>(Quat::FromUnitAxisAngle(Vec3(0.0f, 0.0f, 2.0f), 1.0f).w);
    case 7:
        return static_cast<int>(math::Clamp(0.5, 1.0, 0.0));
    default:
        return 2;
    }
}

} // namespace rocks::test
