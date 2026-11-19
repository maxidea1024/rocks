#pragma once

#include <Base/NumericLimits.h>
#include <Config/ConfigSnapshot.h>
#include <Config/ConfigTypes.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Memory/UniquePtr.h>
#include <Object/RuntimeTypeOps.h>

// 콘솔 설정의 값 종류·성질·설명과 콘솔 입출력 타입이다(Docs/Design/ConfigSystem.ko.md 5절,
// CVarSystem.ko.md — 설정 snapshot은 Config/ConfigSnapshot.h). 콘솔 설정은 CVar(핸들·동적 변수 —
// Config/CVar.h, ConfigContext::RegisterDynamic)와 설정 struct 필드의 ConsoleName이다. 등록과
// 조회는 ConfigContext가 한다(ConfigContext.h). UE 대응: 콘솔 설정은 console
// variable(IConsoleVariable), 콘솔 명령은 console command다.
namespace rocks::config {

// 콘솔 설정 값 종류다(5.1절). 값은 리플렉션 FieldValue로 담는다(ValueKind는 ConsoleValueKind).
enum class ConsoleValueType : uint8 {
    Bool,
    Int32,
    Int64,
    Float,
    Double,
    String,
    Name,
    Enum
};

// 콘솔 설정의 성질 비트다(ConsoleSettingInfo::flags). CVar는 빌더
// 성질(cvar::VarFlags)·BootCVar에서, 설정 struct 필드는 필드 지정자에서 온다(CVar 21절 — 정의
// 입력으로는 쓰지 않는다).
enum class ConsoleSettingFlags : uint32 {
    None = 0,
    // 콘솔 층에서 바꿀 수 없다(다른 층은 가능 — UE ECVF_ReadOnly). CVar ConsoleReadOnly().
    ConsoleReadOnly = 1,
    // 출시 규칙에서는 콘솔에 보이지 않고 값이 코드 기본값으로 고정된다(UE ECVF_Cheat). CVar
    // Cheat().
    DevelopmentOnly = 2,
    // 설정 struct 필드의 값을 설정 snapshot의 이름 블록에 넣는다(필드만 — CVar는 모두 칸에 있다).
    RenderRead = 4,
    // 설정 struct 필드 RestartRequired 지정자다(StartupOnly와 같은 부팅 값 규칙).
    RestartRequired = 8,
    // 시작(ConfigContext::CompleteStartup) 뒤에는 이번 실행 값이 바뀌지 않는다(BootCVar, 설정
    // struct 필드 StartupOnly 지정자): 바로 쓰는 층(콘솔·코드·범위 임시 변경)의 쓰기는 넣을 때
    // Error BootSealed로 거부하고, 파일 층 변경은 재시작 대기, 파생·휘발 층 변경은 Warning
    // BootValueIgnored다(CVar 9절).
    StartupOnly = 16,
    // 사용자 층이 쓸 수 있다. CVar UserSetting().
    UserConfig = 32
    // 품질 묶음 층(UE ECVF_Scalability)은 플래그가 아니다: 그 층은 프리셋 묶음(CVarGroup — CVar
    // 11절)의 펼침만 쓰고, 펼침은 묶음 항목인 변수에만 닿는다(구성원 자동 — VD14).
};

constexpr bool HasFlag(uint32 flags, ConsoleSettingFlags flag) noexcept {
    return (flags & static_cast<uint32>(flag)) != 0;
}

// 콘솔 설정 종류의 리플렉션 값 종류다(Bool·Int32·Int64·Float32·Float64·Utf8String·Name·Enum).
ValueKind ConsoleValueKind(ConsoleValueType type) noexcept;

// 기본 영역 이름 공간 표다(5.1절 — 이름의 첫 마디). `cvar`·`cheat`·`obj`는 CVar 도구·치트·객체
// 콘솔 자리다(Docs/Design/CVarSystem.ko.md 15절). ConfigContext::RegisterConsoleRegion으로 더한다.
inline constexpr StringView DefaultConsoleRegions[] = {
    "render", "graph", "stream", "shader", "quality", "net",   "physics", "log",
    "game",   "input", "audio",  "ui",     "cvar",    "cheat", "obj"};

// 콘솔 설정 핸들이다(등록 순번). 문맥 수명 동안 유효하다.
struct ConsoleSettingId {
    // 무효 핸들 표지다.
    static constexpr uint32 Invalid = NumericLimits<uint32>::Max();

    uint32 index = Invalid;

    bool IsValid() const noexcept {
        return index != Invalid;
    }
};

// 콘솔 설정 하나의 설명이다(ConfigContext::DescribeConsoleSetting — 콘솔 Explain·dump가 쓴다, CVar
// 9·15절). view는 다음 Sync·등록까지 유효하다.
struct ConsoleSettingInfo {
    StringView name;
    StringView help;
    // 값 종류(ConsoleValueKind — Enum은 Enum).
    ValueKind kind = ValueKind::Opaque;
    // ConsoleSettingFlags 비트.
    uint32 flags = 0;
    // 지금 값의 콘솔 표기.
    StringView value;
    // 이번 실행 동안 고정인 부팅 변수인지다(BootCVar·StartupOnly·RestartRequired — Explain·dump의
    // `[boot]`).
    bool boot = false;
    // 재시작 대기 값이 있는지와 그 표기다(Explain의 `pending restart: <값>` — 9절).
    bool hasPendingRestart = false;
    StringView pendingRestart;
};

// 콘솔 출력 sink다(콘솔 입력의 응답·명령 출력 — CVar 15절). 참조 계수(SharedPtr<ConsoleSink>)로
// 넘기고, 큐에 든 일(다음 Sync의 명령·표시, 다른 스레드 입력의 거부 알림)은 sink를 쥐고 있어 입력한
// 쪽이 먼저 놓아도 산다. 큐에 든 일의 출력은 게임 lane(Sync)이 쓰므로, 입력한 스레드와 동시에
// sink를 쓰는 경우 Write는 스레드 안전해야 한다.
//
// 사용 예:
//   struct Transcript final : ConsoleSink {
//       Result<void> Write(StringView text) noexcept override { return text_.TryAppend(text); }
//       String text_;
//   };
class ConsoleSink {
public:
    virtual ~ConsoleSink() = default;

    // text를 쓴다(호출 동안만 빌린다). 오류: 그 sink의 오류(콘솔 처리는 결과로 돌려준다).
    virtual Result<void> Write(StringView text) noexcept = 0;
};

// 콘솔 입력 한 줄의 처리 결과다(ConfigContext::ExecuteConsoleLine — CVar 15절).
enum class ConsoleLineResult : uint8 {
    // `<이름>`: 값과 출처 스택을 출력했다(게임 lane).
    Shown,
    // `<이름> ?`: Explain을 출력했다(게임 lane).
    Explained,
    // `<이름> <값>`·`<이름> reset`: 콘솔 층 쓰기·지우기를 큐에 넣었다(다음 Sync에 적용).
    Queued,
    // `<명령> <인자…>`(`cvar.*` 포함), 다른 스레드의 `<이름>`·`<이름> ?`: 큐에 넣었다(다음 Sync의
    // 5단계에 게임 lane이 실행하고 sink에 쓴다).
    CommandQueued,
    // 쓰기·인자·권한이 거부됐다(사유를 출력했다 — 아무것도 큐에 넣지 않았다).
    Rejected,
    // 모르는 이름이다.
    Unknown
};

// 문맥의 네트워크 역할이다(CVar 13.2절 — 권한 판정에만 쓴다. 전송은 넷 모듈의 일이다).
enum class NetworkRole : uint8 {
    // 단독 실행(기본).
    Standalone,
    // 서버: 복제 변수의 값을 정하고 ExportReplicated로 낸다.
    Server,
    // 클라이언트: 복제 변수의 콘솔 쓰기를 거부하고(ReplicatedOnClient) 서버 값(Server 층)을 따른다.
    Client
};

// 복제 값 하나다(이름과 콘솔 표기 — ExportReplicated·ImportReplicated 목록 원소). view의 수명은 그
// 목록을 만든 쪽이 정한다.
struct ReplicatedValue {
    StringView name;
    StringView value;
};

} // namespace rocks::config
