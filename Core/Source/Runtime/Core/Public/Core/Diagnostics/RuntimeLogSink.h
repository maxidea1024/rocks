#pragma once

#include <Core/Diagnostics/LogCategory.h>
#include <Core/Diagnostics/LogEvent.h>
#include <Core/Diagnostics/LogRecord.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Platform/Sync.h>

namespace rocks {

// 로그 출력 대상이다(UE FOutputDevice 대응). LogSystem::AddSink로 등록한다.
//
// 호출 계약:
// - Async·Manual 모드에서는 출력 스레드(Manual은 Pump를 부른 스레드)만, Sync 모드에서는 로그를 남긴
//   스레드가 LogSystem의 sink lock 안에서 부른다. 한 LogSystem 안에서 콜백은 동시에 실행되지
//   않는다.
// - 여러 LogSystem에 등록한 sink는 동시 호출을 직접 보호한다.
// - event의 view는 콜백 동안만 유효하다. 보관하려면 복사한다.
// - 콜백 안에서 로그를 남기면 큐에 들어가지 않고 비상 경로(stderr 직접 쓰기)로만 간다.
// - 실패를 돌려주면 LogSystem이 센다. 연속 SinkFailureLimit번 실패하면 잠시 막고 다시 시도한다.
//
// 수명: 등록부터 RemoveSink(또는 LogSystem::Stop) 반환까지 살아 있어야 한다.
class RuntimeLogSink {
public:
    virtual ~RuntimeLogSink() noexcept = default;

    // 기록 한 건을 출력한다.
    virtual Result<void> Write(const LogEvent& event) noexcept = 0;

    // 묶음 끝에 부른다. 버퍼가 있는 sink는 여기서 내보낸다. 기본은 아무것도 하지 않는다.
    virtual Result<void> Flush() noexcept {
        return Result<void>::Success();
    }
};

// sink 하나의 기록 필터다. level이 minimum 이상이고 카테고리가 category 접두사 아래면 통과한다.
// Fatal은 카테고리 조건과 무관하게 늘 통과한다(어떤 필터도 Fatal을 막지 않는다). category 기본값(빈
// 접두사)은 모든 카테고리를 통과시킨다.
//
// 수명: category를 LogCategory에서 만들면 이름이 정적 저장소라 제약이 없다. 직접 채운 접두사는
// 등록된 동안 이름 저장소가 살아 있어야 한다.
//
// 사용 예:
//   inline constexpr LogCategory LogGraphics{"Graphics"};
//   logs.AddSink(sink, {LogLevel::Warning, LogGraphics}); // Graphics·Graphics.* 의 Warning 이상
struct LogFilter {
    LogLevel minimum = LogLevel::Trace;
    LogCategoryPrefix category{};

    // event가 이 필터를 통과하는지다.
    bool Accepts(const LogEvent& event) const noexcept {
        if (event.level == LogLevel::Fatal) {
            return true;
        }

        return static_cast<uint8>(event.level) >= static_cast<uint8>(minimum) &&
               category.MatchesName(event.category);
    }
};

// callback과 context는 sink가 소멸하고 모든 호출이 끝날 때까지 살아야 한다. write는 전달받은
// byte 전체를 소비하거나 실패해야 하며 원본을 보관하려면 복사한다.
struct LogTextOutput {
    void* context = nullptr;
    Result<void> (*write)(void*, StringView) noexcept = nullptr;
    Result<void> (*flush)(void*) noexcept = nullptr;
};

// 출력 정책을 주입하는 텍스트 sink다. 기록마다 `[레벨][카테고리 이름] `, 메시지 byte, 줄바꿈을
// 순서대로 출력하고 flush한다(예: `[Warning][Graphics.D3D12] Device lost`). 부분 출력 뒤 실패하면
// 되돌리지 않는다. 같은 인스턴스의 호출은 직렬화한다. 잘못된 레벨, UTF-8이 아닌 message, 카테고리
// 이름 문법(ValidateLogCategoryName)을 어긴 category는 InvalidArgument다. 레이아웃 패턴과 묶음
// flush는 후속 단계(LG4)에서 바뀐다.
class TextLogSink final : public RuntimeLogSink {
public:
    explicit TextLogSink(LogTextOutput output) noexcept : output_(output) {
    }

    Result<void> Write(const LogEvent& event) noexcept override;

private:
    platform::Mutex mutex_;
    LogTextOutput output_;
};

// stderr로 UTF-8 byte를 출력하고 즉시 flush한다. Windows console의 실제 글자 표시는
// console code page/환경에 의존한다. CRT의 text mode는 줄바꿈을 변환할 수 있다.
// 다른 stderr 사용자와의 원자성은 보장하지 않는다.
// stderr의 기존 error flag도 실패로 보고하며 clearerr는 호출하지 않는다. 복구와 오류 상태
// 초기화는 다른 stderr 사용자와의 동기화를 포함하여 호스트가 책임진다.
// Unix에서 읽는 쪽이 닫힌 pipe는 기본 SIGPIPE 설정이면 프로세스를 종료할 수 있다.
// 엔진은 전역 signal 처리를 바꾸지 않으며, 오류 반환을 원하면 호스트가 정책을 설정해야 한다.
class StderrLogSink final : public RuntimeLogSink {
public:
    StderrLogSink() noexcept;
    Result<void> Write(const LogEvent& event) noexcept override;

private:
    TextLogSink sink_;
};

} // namespace rocks
