#pragma once

#include <Core/Diagnostics/RuntimeLogSink.h>

namespace rocks::detail {

// Android logcat sink다(Docs/Design/AndroidPlatform.ko.md 6절). APK에는 stderr를 보는 사람이
// 없으므로 LogHost가 Android에서 stderr sink 옆에 단다. 프로세스에 하나이며 파괴하지
// 않는다(LogHost가 출력 스레드를 포기해도 남은 기록이 이 sink에 닿을 수 있다). Android가 아니면
// nullptr다.
RuntimeLogSink* AndroidLogcatSink() noexcept;

} // namespace rocks::detail
