#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>

// trace 수집 설정 struct다(Docs/Design/TraceSystem.ko.md 5·8절 — TR5). 값은 설정 계층(branch
// `Engine`, `Engine/Config/Engine.json`)에서 온다. 모듈 등록: RegisterTraceControlTypes(리플렉션
// 타입) → RegisterTraceSettings(설정 문맥).
namespace rocks::trace_control {

// 채널 묶음이다(섹션 `TracePresets`). 필드 하나가 묶음 하나이고 값은 채널 이름 목록이다. 프로젝트는
// 설정 계층의 배열 연산(`$append`·`$remove` 등)으로 고친다. 묶음 이름은 필드의 도구 이름이다.
//
// 예(Engine.json):
//   "TracePresets": { "default": ["cpu", "task", "gpu", "frame", "log", "mark", "counter"] }
// 예(프로젝트 Engine.json):
//   "TracePresets": { "default": { "$append": ["stats"], "$remove": ["log"] } }
ROCKS_STRUCT(Settings(Branch = "Engine", Section = "TracePresets"))
struct TracePresetSettings {
    explicit TracePresetSettings(Allocator& allocator) noexcept : defaultChannels(allocator) {
    }

    // 기본 묶음 `default`다(명령줄 `--trace=default`).
    ROCKS_PROPERTY(Name = "default") Array<String> defaultChannels;
};

// 세션 설정이다(섹션 `Trace`). TraceSessionSettings의 같은 이름 칸에 들어간다
// (MakeTraceSessionSettings — 콘솔 설정 값 종류가 부호 있는 정수뿐이라 부호 있는 칸으로 두고,
// 음수는 0으로 본다). 세션은 시작 때 값을 정하므로 스냅샷 제한 시간 외에는 재시작 뒤에 효력이 있다.
ROCKS_STRUCT(Settings(Branch = "Engine", Section = "Trace"))
struct TraceCaptureSettings {
    // 블록 풀 상한(byte, PageBytes 단위로 내림).
    ROCKS_PROPERTY(RestartRequired, ConsoleName = "trace.pool.max_bytes")
    int64 poolMaxBytes = 64 * 1024 * 1024;
    // 블록 풀 사용률 경고 백분율(0이면 끔).
    ROCKS_PROPERTY(RestartRequired, ConsoleName = "trace.pool.warn_percent")
    int32 poolWarningPercent = 80;
    // 최근 기록 고리의 byte 수(0이면 고리 없음).
    ROCKS_PROPERTY(RestartRequired, ConsoleName = "trace.recent.bytes")
    int64 recentBytes = 4 * 1024 * 1024;
    // writer 스레드의 비우기 주기(ms).
    ROCKS_PROPERTY(RestartRequired, ConsoleName = "trace.writer.period_ms")
    int32 writerPeriodMs = 16;
    // 채널 "stats"의 수집 통계 기록 간격(펌프 수).
    ROCKS_PROPERTY(RestartRequired, ConsoleName = "trace.stats.interval_pumps")
    int32 statsIntervalPumps = 60;
    // 스냅샷의 writer 잠금 제한 시간(ms).
    ROCKS_PROPERTY(ConsoleName = "trace.snapshot.lock_timeout_ms")
    int32 snapshotLockTimeoutMs = 1000;
};

} // namespace rocks::trace_control
