#pragma once

#include <Base/NumericLimits.h>
#include <Config/CVar.h>
#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>

// trace 수집 설정이다(Docs/Design/TraceSystem.ko.md 5·8절 — TR5). 채널 묶음은 설정 struct(branch
// `Engine`, `Engine/Config/Engine.json` 섹션 `TracePresets`)이고, 세션 값은 CVar 핸들 `trace.*`다
// (CVarSystem.ko.md CVR7). 모듈 등록: RegisterTraceControlTypes(리플렉션 타입) →
// RegisterTraceSettings(설정 문맥).
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
    ROCKS_GENERATED_BODY()

    explicit TracePresetSettings(Allocator& allocator) noexcept : defaultChannels(allocator) {
    }

    // 기본 묶음 `default`다(명령줄 `--trace=default`).
    ROCKS_PROPERTY(Name = "default") Array<String> defaultChannels;
};

// trace.pool.warn_percent의 상한(백분율)이다.
inline constexpr int32 MaxTracePoolWarningPercent = 100;

// 세션 설정 핸들이다(정의는 TraceController.cpp, 등록은 RegisterTraceSettings). 세션은 시작 때 값을
// 정하므로 스냅샷 제한 시간 외에는 이번 실행 동안 고정인 부팅 변수다(설정 파일·`--set`으로 정하고
// 다시 시작한다). 값은 음수가 아니다(Range).
//   trace.pool.max_bytes          BootCVar<int64>  블록 풀 상한(byte, PageBytes 단위로 내림)
//   trace.pool.warn_percent       BootCVar<int32>  블록 풀 사용률 경고 백분율(0이면 끔, 0–100)
//   trace.recent.bytes            BootCVar<int64>  최근 기록 고리의 byte 수(0이면 고리 없음)
//   trace.writer.period_ms        BootCVar<int32>  writer 스레드의 비우기 주기(ms)
//   trace.stats.interval_pumps    BootCVar<int32>  채널 "stats"의 수집 통계 기록 간격(펌프 수)
//   trace.snapshot.lock_timeout_ms CVar<int32>     스냅샷의 writer 잠금 제한 시간(ms — 실행 중
//   변경)
extern BootCVar<int64> TracePoolMaxBytesVar;
extern BootCVar<int32> TracePoolWarningPercentVar;
extern BootCVar<int64> TraceRecentBytesVar;
extern BootCVar<int32> TraceWriterPeriodVar;
extern BootCVar<int32> TraceStatsIntervalVar;
extern CVar<int32> TraceSnapshotLockTimeoutVar;

// 세션 값 묶음이다(ReadTraceCaptureSettings가 핸들에서 채운다 — TraceSessionSettings의 같은 이름
// 칸에 들어간다, MakeTraceSessionSettings). 부호 있는 칸은 핸들 타입 그대로다(음수는 0으로 본다).
struct TraceCaptureSettings {
    int64 poolMaxBytes = 64 * 1024 * 1024;
    int32 poolWarningPercent = 80;
    int64 recentBytes = 4 * 1024 * 1024;
    int32 writerPeriodMs = 16;
    int32 statsIntervalPumps = 60;
    int32 snapshotLockTimeoutMs = 1000;
};

} // namespace rocks::trace_control
