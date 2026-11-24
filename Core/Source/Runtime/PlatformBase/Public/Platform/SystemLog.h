#pragma once

#include <Base/Types.h>

namespace rocks::platform {

// OS 시스템 로그의 수준이다(Android logcat 우선순위에 대응한다).
enum class SystemLogLevel : uint8 {
    Verbose,
    Debug,
    Info,
    Warning,
    Error,
    Fatal
};

// OS 시스템 로그 한 줄의 byte 상한이다. Android logger 한 항목(LOGGER_ENTRY_MAX_PAYLOAD 4068) 안에
// 든다. 더 긴 줄은 호출자가 글자 경계에서 나눠 쓴다.
inline constexpr usize MaxSystemLogLineBytes = 4000;

// 이 플랫폼에 OS 시스템 로그(Android logcat)가 있는지다. Windows·macOS는 false다(stderr·디버거
// 출력을 쓴다). LogHost가 Android APK 로그 sink를 달지 정한다(Docs/Design/AndroidPlatform.ko.md
// 6절).
ROCKS_PLATFORMBASE_API bool HasSystemLog() noexcept;

// NUL 종료 UTF-8 한 줄을 OS 시스템 로그에 tag "Rocks"로 쓴다(`adb logcat -s Rocks`). 시스템 로그가
// 없는 플랫폼은 아무것도 하지 않는다. 할당하지 않고 어느 스레드에서나 부를 수 있다. 줄이
// MaxSystemLogLineBytes를 넘으면 OS가 자를 수 있다.
//
// 사용 예:
//   if (platform::HasSystemLog()) {
//       platform::WriteSystemLogLine(platform::SystemLogLevel::Info, "[Engine] started");
//   }
ROCKS_PLATFORMBASE_API void WriteSystemLogLine(SystemLogLevel level, const char* text) noexcept;

} // namespace rocks::platform
