#pragma once

#include <Config/ConfigContext.h>
#include <Core/Containers/Span.h>
#include <Core/Text/String.h>
#include <Object/ModuleRegistration.h>
#include <TraceCapture/TraceSession.h>
#include <TraceControl/TraceSettings.h>

// trace 수집 제어다(Docs/Design/TraceSystem.ko.md 5·8절 — TR5): 채널 묶음(설정 데이터)의 펼침,
// 명령줄 `--trace=<채널|묶음>,…`, 콘솔 명령 trace.enable·trace.disable·trace.file·trace.stop·
// trace.snapshot. TraceCapture는 Core만 알므로, 설정 계층(Config)과 잇는 일은 이 모듈이 맡는다.
namespace rocks::trace_control {

// 이 모듈의 리플렉션 타입(설정 struct·명령 인자 struct)을 등록한다. 설정 문맥을 만들기 전, 엔진의
// 모듈 등록 단계에서 부른다. 실패: ModuleRegistration의 오류.
Result<void> RegisterTraceControlTypes(ModuleRegistration& registration) noexcept;

// 콘솔 영역 `trace`와 설정 struct(TracePresetSettings·TraceCaptureSettings)를 설정 문맥에 등록한다.
// 파일 층을 읽기 전에 부른다. 실패: ConfigContext::RegisterConsoleRegion·RegisterSettings의 오류.
Result<void> RegisterTraceSettings(config::ConfigContext& config) noexcept;

// 설정 섹션 `Trace`의 값을 세션 설정으로 옮긴다(음수는 0, 너무 큰 값은 그 칸의 최대값). allocator와
// writer 방식은 호출자가 정한다.
TraceSessionSettings MakeTraceSessionSettings(const TraceCaptureSettings& settings,
                                              Allocator& allocator) noexcept;

// 채널 켜기·끄기의 결과다.
struct TraceChannelChange {
    // 상태를 바꾼 채널 수(이미 그 상태였던 것도 센다)다.
    uint32 applied = 0;
    // 등록되지 않은 채널 이름 수다. 묶음에서 온 이름(아직 등록되지 않은 모듈의 채널)은 세지만
    // 경고하지 않고, 직접 적은 이름은 경고 로그를 한 번 남긴다.
    uint32 unknown = 0;
    // 읽기 전용이라 바꾸지 못한 채널 수다.
    uint32 readOnly = 0;
};

// 한 세션의 trace 제어기다. 콘솔 명령을 등록하면 명령이 이 객체를 쓴다.
//
// 사용 예(엔진 부트):
//   RegisterTraceControlTypes(modules);
//   RegisterTraceSettings(config);
//   LoadConfigFiles(config, files); config.Sync();
//   session.Start(MakeTraceSessionSettings(config.Get<TraceCaptureSettings>(), allocator));
//   TraceController trace(allocator, session, config);
//   trace.RegisterConsoleCommands();
//   ... 모듈이 채널 등록 ...
//   trace.ApplyCommandLine(arguments);              // --trace=default,-log 등
//
// 이름 목록: 쉼표로 나눈 채널 이름 또는 묶음 이름이다. 앞에 `-`를 붙이면 그 이름(묶음이면 묶음의
// 채널 전부)을 끈다(`--trace=default,-log`). 앞뒤 공백은 무시한다.
//
// 스레드: 설정 문맥의 소유 스레드(게임 lane)에서만 쓴다. 수명: session·config보다 먼저 파괴한다.
// 파괴할 때 이 객체가 붙인 파일 출력을 뗀다.
class TraceController {
public:
    TraceController(Allocator& allocator, TraceSession& session,
                    config::ConfigContext& config) noexcept;
    ~TraceController() noexcept;

    TraceController(const TraceController&) = delete;
    TraceController& operator=(const TraceController&) = delete;

    // 콘솔 명령 다섯을 등록한다.
    //   trace.enable channels=<목록>   채널·묶음 켜기
    //   trace.disable channels=<목록>  채널·묶음 끄기
    //   trace.file path=<경로>         파일 출력 시작(이미 있으면 바꾼다)
    //   trace.stop                     trace.file이 붙인 파일 출력 끝내기
    //   trace.snapshot path=<경로>     스냅샷 쓰기(최근 기록 고리, 8절)
    // 실패: RegisterConsoleCommand의 오류(이름 중복 AlreadyExists 등). 한 번만 부른다.
    Result<void> RegisterConsoleCommands() noexcept;

    // 이름 목록의 채널을 켜거나 끈다(위 "이름 목록"). 실패: 세션이 실행 중이 아니면 WrongContext,
    // 메모리 부족 OutOfMemory. 모르는 이름은 실패가 아니다(결과의 unknown).
    Result<TraceChannelChange> SetChannels(StringView list, bool enabled) noexcept;

    // 명령줄 인자 가운데 `--trace=<목록>`을 모두 적용한다(나온 순서대로). 다른 인자는 무시한다.
    // 실패: SetChannels와 같다.
    Result<TraceChannelChange> ApplyCommandLine(Span<const StringView> arguments) noexcept;

    // 파일 출력을 붙인다. 이 객체가 붙인 파일 출력이 있으면 먼저 뗀다. 실패: 빈 경로
    // InvalidArgument, TraceSession::AttachFileOutput의 오류(실패를 확정하므로 경고 로그를 남긴다).
    Result<void> OpenFile(StringView path) noexcept;

    // 이 객체가 붙인 파일 출력을 뗀다. 없으면 NotFound다.
    Result<void> CloseFile() noexcept;

    // 스냅샷을 쓴다(설정 snapshotLockTimeoutMs). 불완전 스냅샷은 성공이며 경고 로그를 남긴다. 실패:
    // 빈 경로 InvalidArgument, TraceSession::WriteSnapshot의 오류.
    Result<TraceSnapshotInfo> WriteSnapshot(StringView path) noexcept;

private:
    friend struct TraceControllerAccess;

    // 이름 하나(채널 또는 묶음)를 적용한다.
    Result<void> ApplyName(StringView name, bool enabled, TraceChannelChange& change) noexcept;
    Result<void> SetChannel(StringView name, bool enabled, bool fromPreset,
                            TraceChannelChange& change) noexcept;

    Allocator& allocator_;
    TraceSession& session_;
    config::ConfigContext& config_;
    TraceOutputId file_{};
};

} // namespace rocks::trace_control
