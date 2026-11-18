#pragma once

#include <Base/NumericLimits.h>
#include <Config/ConfigTypes.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Memory/UniquePtr.h>
#include <Object/RuntimeTypeOps.h>

// 콘솔 설정·콘솔 명령의 정의와 설정 snapshot이다(Docs/Design/ConfigSystem.ko.md 5절 — 설정 CF2).
// 등록과 조회는 ConfigContext가 한다(ConfigContext.h). UE 대응: 콘솔 설정은 console
// variable(IConsoleVariable), 콘솔 명령은 console command, 설정 snapshot은 렌더 스레드 사본(이
// 문서는 주소 전파 대신 불변 블록을 쓴다).
namespace rocks::config {

namespace detail {

struct SnapshotAccess;

} // namespace detail

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

// 콘솔 설정 플래그다(5.1절 표).
enum class ConsoleSettingFlags : uint32 {
    None = 0,
    // 콘솔 층에서 바꿀 수 없다(다른 층은 가능 — UE ECVF_ReadOnly).
    ConsoleReadOnly = 1,
    // 출시 규칙에서는 콘솔에 보이지 않고 값이 코드 기본값으로 고정된다(UE ECVF_Cheat).
    DevelopmentOnly = 2,
    // 설정 snapshot에 넣는다(UE ECVF_RenderThreadSafe).
    RenderRead = 4,
    // 바뀐 값이 다음 시작 때 효력을 갖는다(시작 뒤에는 지금 값을 유지 — CompleteStartup 참고).
    RestartRequired = 8,
    // 시작(ConfigContext::CompleteStartup) 뒤에는 이번 실행 값이 바뀌지 않는다(RestartRequired와
    // 같은 규칙 — 파일 층 변경은 재시작 대기, 그 밖은 BootValueIgnored).
    StartupOnly = 16,
    // 사용자 층이 쓸 수 있다.
    UserConfig = 32,
    // 품질 묶음 층이 바꿀 수 있다(UE ECVF_Scalability). 아니면 품질 묶음 층 쓰기를 거부한다.
    QualityMember = 64
};

// ConsoleSettingFlags의 알려진 비트 전부다.
inline constexpr uint32 KnownConsoleSettingFlags =
    static_cast<uint32>(ConsoleSettingFlags::ConsoleReadOnly) |
    static_cast<uint32>(ConsoleSettingFlags::DevelopmentOnly) |
    static_cast<uint32>(ConsoleSettingFlags::RenderRead) |
    static_cast<uint32>(ConsoleSettingFlags::RestartRequired) |
    static_cast<uint32>(ConsoleSettingFlags::StartupOnly) |
    static_cast<uint32>(ConsoleSettingFlags::UserConfig) |
    static_cast<uint32>(ConsoleSettingFlags::QualityMember);

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

// 독립 콘솔 설정 하나의 정의다(설정 struct 필드가 아닌 것 — 필드는 ConsoleName 지정자로 등록된다).
// view는 등록 호출 동안만 빌린다(문맥이 복사한다).
//
// 사용 예:
//   ConsoleSettingDefinition pool;
//   pool.name = "stream.pool_mb";
//   pool.help = "Texture streaming pool size in MiB";
//   pool.type = ConsoleValueType::Int32;
//   pool.defaultValue = "512";
//   pool.flags = static_cast<uint32>(ConsoleSettingFlags::RenderRead);
//   pool.hasRange = true;
//   pool.minimum = 64;
//   pool.maximum = 8192;
//   auto id = context.RegisterConsoleSetting(pool);
struct ConsoleSettingDefinition {
    // 소문자 점 경로(IsConsoleSettingName), 첫 마디는 등록된 영역 이름 공간.
    StringView name;
    // 도움말(필수 — 비어 있으면 InvalidArgument).
    StringView help;
    ConsoleValueType type = ConsoleValueType::Bool;
    // Enum 종류의 리플렉션 enum ID(TypeRegistry에 등록돼 있어야 한다).
    TypeId enumType{};
    // 코드 기본값의 콘솔 표기(리플렉션 명령행 단일 값 — `true`·`512`·`1.5`·`High`). 비어 있으면 그
    // 종류의 0·빈 값·enum 첫 열거자다.
    StringView defaultValue;
    // ConsoleSettingFlags 비트.
    uint32 flags = 0;
    // 수 종류(Int32·Int64·Float·Double)의 닫힌 범위다(hasRange일 때). 밖의 값은 거부한다.
    bool hasRange = false;
    double minimum = 0;
    double maximum = 0;
    // String·Name·Enum의 허용 값(콘솔 표기 — Enum은 대소문자 무시)이다. 비어 있으면 모두 허용한다.
    Span<const StringView> allowedValues{};
    // String·Name의 허용 값도 ASCII 대소문자를 무시하고 맞춘다(값은 입력 표기 그대로 남는다 —
    // 읽는 쪽도 대소문자를 무시해야 한다). 예: log.level의 "Warning".
    bool allowedValuesIgnoreCase = false;
};

// 콘솔 설정 핸들이다(등록 순번). 문맥 수명 동안 유효하다.
struct ConsoleSettingId {
    // 무효 핸들 표지다.
    static constexpr uint32 Invalid = NumericLimits<uint32>::Max();

    uint32 index = Invalid;

    bool IsValid() const noexcept {
        return index != Invalid;
    }
};

// 콘솔 출력 sink다(콘솔 입력의 응답·명령 출력). write는 호출 동안만 text를 빌린다.
struct ConsoleOutput {
    using WriteFn = Result<void> (*)(void* context, StringView text) noexcept;

    WriteFn write = nullptr;
    void* context = nullptr;
};

// 콘솔 명령 실행 함수다. arguments는 인자 struct 값(인자 스키마가 없으면 null)이고 호출 동안만
// 빌린다. sync 지점(ConfigContext::Sync)에서 소유 스레드가 부른다. 실패는 출력에 알리고 Sync는
// 계속한다(명령 실패가 설정 적용을 되돌리지 않는다).
using ConsoleCommandFunction = Result<void> (*)(void* context, const void* arguments,
                                                ConsoleOutput& output) noexcept;

// 콘솔 명령 정의다(5.2절 — Commandlet registry와 따로, 이름 문법만 공유한다). view는 등록 동안만
// 빌린다. context는 문맥보다 오래 살아야 한다.
struct ConsoleCommandDefinition {
    StringView name;
    StringView help;
    ConsoleCommandFunction execute = nullptr;
    void* context = nullptr;
};

// 콘솔 입력 한 줄의 처리 결과다(ConfigContext::ExecuteConsoleLine).
enum class ConsoleLineResult : uint8 {
    // `<이름>`: 값과 출처 스택을 출력했다.
    Shown,
    // `<이름> ?`: 도움말을 출력했다.
    Help,
    // `<이름> <값>`: 콘솔 층 쓰기를 큐에 넣었다(다음 Sync에 적용).
    Queued,
    // `<명령> <인자…>`: 명령을 큐에 넣었다(다음 Sync에 실행).
    CommandQueued,
    // 쓰기·인자가 거부됐다(진단을 출력했다 — 아무것도 큐에 넣지 않았다).
    Rejected,
    // 모르는 이름이다.
    Unknown
};

// 값이 바뀐 설정 하나다(변경 콜백 인자). name은 콘솔 설정 이름이거나 설정 struct의
// `<Branch>.<Section>`이다. settings는 설정 struct면 그 TypeId, 콘솔 설정이면 0이다. view는 콜백
// 동안만 유효하다.
struct ConfigChange {
    StringView name;
    TypeId settings{};
};

// 변경 콜백이다. sync 지점에서 값이 바뀐 대상마다, 구독 순서대로 소유 스레드가 부른다. 콜백 안에서
// 층 입력·Unset·콘솔 입력은 큐에 들어가 다음 Sync에 적용된다. Sync를 다시 부르면 Busy다.
using ConfigChangeCallback = void (*)(void* context, const ConfigChange& change) noexcept;

// 구독 핸들이다(Unsubscribe에 쓴다).
struct ConfigSubscriptionId {
    uint64 value = 0;
};

// 설정 snapshot이다(5.3절): sync 지점에서 만든 "렌더 읽기" 값의 불변 블록과 세대 번호. 프레임
// packet이 SharedPtr로 참조하고, 그 프레임 slot이 끝나 마지막 참조가 놓이면 사라진다. 만든 뒤
// 바뀌지 않으므로 어느 스레드에서 읽어도 된다(참조 계수만 원자적이다 — 렌더 lane은 registry를 읽지
// 않는다). 값은 이름(byte 순서)으로 정렬돼 있다. 이름은 콘솔 설정 이름이거나, 콘솔 이름이 없는
// RenderRead 설정 struct 필드면 `<Branch>.<Section>.<필드>`다. Name 값의 토큰은 문맥 Registry의
// NamePool 것이다.
//
// 사용 예(렌더 lane):
//   const FieldValue* immediate = snapshot->Find("render.graph.immediate");
//   const bool value = immediate != nullptr && immediate->boolean;
class ConfigSnapshot {
public:
    explicit ConfigSnapshot(Allocator& allocator) noexcept;
    ConfigSnapshot(const ConfigSnapshot&) = delete;
    ConfigSnapshot& operator=(const ConfigSnapshot&) = delete;

    // 1부터 늘어나는 세대 번호다(값이 바뀐 sync 지점마다 하나씩).
    uint64 Generation() const noexcept {
        return generation_;
    }

    usize Count() const noexcept {
        return entries_.Size();
    }

    // index번째(이름 순서) 이름·값이다. 전제: index < Count() — 아니면 Fatal.
    StringView NameAt(usize index) const noexcept;
    const FieldValue& ValueAt(usize index) const noexcept;

    // 이름의 값이다(없으면 null).
    const FieldValue* Find(StringView name) const noexcept;

private:
    friend struct detail::SnapshotAccess;

    struct Entry {
        explicit Entry(Allocator& allocator) noexcept : name(allocator), value(allocator) {
        }

        String name;
        FieldValue value;
    };

    uint64 generation_ = 0;
    Array<UniquePtr<Entry>> entries_;
};

} // namespace rocks::config
