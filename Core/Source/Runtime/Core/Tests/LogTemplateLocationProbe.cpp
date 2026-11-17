#include <Core/Diagnostics/LogCapture.h>

// 호출 위치 시험의 두 번째 번역 단위다. LogTemplateTests.cpp가 이 함수의 캡처를 렌더해
// 파일·줄·함수가 이 파일의 값인지 확인한다.
namespace rocks::test {

// 캡처는 카테고리 포인터만 담으므로 카테고리는 정적 저장소여야 한다(함수 지역 constexpr이면 렌더
// 때 사라진 객체를 가리킨다).
inline constexpr LogCategory LogProbe{"Tests.LocationProbe"};

uint32 CaptureLogFromProbeUnit(Span<uint8> buffer) noexcept {
    const uint32 line = __LINE__ + 1;
    const auto captured = CaptureLog({buffer}, LogLevel::Info, LogProbe, "probe unit {Value}", 7);
    return captured.IsOk() ? line : 0;
}

} // namespace rocks::test
