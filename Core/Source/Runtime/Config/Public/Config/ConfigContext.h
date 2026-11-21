#pragma once

#include <Base/Compiler.h>
#include <Config/CVarCommand.h>
#include <Config/CVarDefinition.h>
#include <Config/ConfigTypes.h>
#include <Config/ConsoleSettings.h>
#include <Config/QualityValidation.h>
#include <Core/Containers/Array.h>
#include <Core/Delegate.h>
#include <Core/Lifetime/ModuleLease.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Module/HostServiceKey.h>
#include <Meta/GeneratedType.h>
#include <Meta/SettingsSchema.h>
#include <Object/GeneratedFieldAccess.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Fatal.h>

// 설정 문맥이다(Docs/Design/ConfigSystem.ko.md 3·4·5·7·8절 — 설정 CF1·CF2·CF3). 한 실행(또는 시험
// 하나)의 설정 계층을 소유한다: 등록한 설정 struct(섹션)마다 현재 값과 필드 경로별 출처 스택을
// 두고, 층 입력(branch 파일·메모리 문자열·대입)을 큐에 쌓았다가 Sync에서만 합친다. 전역 싱글턴이
// 아니다(UE `GConfig` 대응이지만 문맥 객체로 넘긴다).
//
// 합치는 규칙:
// - 층 순서는 ConfigLayer(약한 것 → 강한 것)이고 같은 층 안에서는 넣은 순서다. Sync는 섹션마다
//   코드 기본값에서 시작해 모든 입력을 이 순서로 적용한다. 그래서 약한 층의 쓰기는 값을 바꾸지
//   않지만 출처 스택에는 남고(진단 WeakWrite), 강한 출처를 Unset하면 약한 값이 드러난다.
// - struct는 재귀 병합(적힌 필드만), Map은 키별 병합, 배열·Set은 통째 교체다. 배열·Set 필드
//   자리의 연산 객체(`$clear`·`$remove`·`$add`·`$append`)는 이전 층까지의 값에 이 순서로
//   적용한다(원소 비교는 리플렉션 값의 정규 JSON 표기 동등성 — 4.2절).
// - 문자열 필드 값의 경로 이름(`{EngineDir}`·`{ProjectDir}`·`{UserDataDir}`·`{SavedDir}`)은 읽을
//   때 펼친다(4.3절).
// - 사용자 층은 UserConfig 필드만 쓸 수 있다.
//
// 스레드: 소유 스레드(만든 스레드 — 게임 lane)에서 쓴다. 다른 스레드의 호출은 WrongThread다(Get은
// Fatal). 예외는 Acquire·ClearLayer와 CVar 핸들의 `Set`·`Clear`(쓰기 큐 —
// Docs/Design/CVarSystem.ko.md 6.2·10.1절)다. 다른 스레드가 snapshot을 쥐기만 해도(마지막 참조를 그
// 스레드가 놓으면 해제도 그 스레드에서 한다) allocator가 여러 스레드의 동시 호출을 받아야
// 한다(SystemAllocator 등). 값은 Sync에서만 바뀌므로 그 사이 Get이 돌려준 참조의 내용은 바뀌지
// 않는다. 소유 스레드는 문맥이 스스로 최신 snapshot에 묶는다(Initialize·Register·Sync — 그 스레드에
// 명시 묶음이 없을 때). 한 스레드가 여러 문맥의 게임 lane이면 가장 최근에 게시하거나 Sync한 문맥이
// 묶인다(다른 문맥은 SnapshotBinding 으로 읽는다). 문맥은 소유 스레드에서만 없앤다(아니면 Fatal).
//
// 수명: allocator·objects는 문맥보다 오래 살아야 한다. allocator는 마지막 snapshot 참조와 쓰기
// 통로(snapshot들이 공유한다)보다도 오래 살아야 한다. 설정 struct 타입의 모듈(생성 코드)과 그
// TypeRegistry 등록도 문맥보다 오래 살아야 한다(문맥이 TypeLease를 쥔다).
//
// 사용 예(시험 — 봉인 모드):
//   ConfigContextOptions options;                      // mode = Sealed
//   options.projectDir = "C:/Game";
//   ConfigContext context(allocator, objects, options);
//   REQUIRE(context.Initialize());
//   REQUIRE(context.RegisterSettings<RenderGraphSettings>());
//   REQUIRE(context.AddLayerText(ConfigLayer::Engine, "Render", "Render", "<engine>",
//                                R"({ "RenderGraph": { "maxPasses": 32 } })"));
//   REQUIRE(context.Sync());
//   const RenderGraphSettings& graph = context.Get<RenderGraphSettings>();
namespace rocks::config {

namespace detail {

struct ConfigState;

// 설정 struct 타입 하나의 형식 지우기 연산이다(RegisterSettings<T>가 만든다).
struct SettingsTypeOps {
    TypeId id{};
    const SettingsSchema* settings = nullptr;
    // 기본값으로 만든 값 하나를 allocator에 할당해 돌려준다(실패하면 null).
    void* (*create)(Allocator& allocator) noexcept = nullptr;
    // create가 만든 값을 소멸·반환한다.
    void (*destroy)(Allocator& allocator, void* value) noexcept = nullptr;
    // source를 destination으로 옮긴다(실패하지 않는다).
    void (*moveAssign)(void* destination, void* source) noexcept = nullptr;
};

template <class T>
void* CreateSettingsValue(Allocator& allocator) noexcept {
    auto storage = allocator.Allocate(sizeof(T), alignof(T));
    if (!storage) {
        return nullptr;
    }

    return InitializeAt<T>(storage.Value(), rocks::detail::MakeAllocatedValue<T>(allocator));
}

template <class T>
void DestroySettingsValue(Allocator& allocator, void* value) noexcept {
    if constexpr (!IsTriviallyDestructibleV<T>) {
        DestroyAt(static_cast<T*>(value));
    }

    if (!allocator.Deallocate(value)) {
        platform::Fatal("settings value deallocation failed");
    }
}

template <class T>
void MoveAssignSettingsValue(void* destination, void* source) noexcept {
    *static_cast<T*>(destination) = Move(*static_cast<T*>(source));
}

} // namespace detail

// 리플렉션 struct(ROCKS_STRUCT — 생성 헤더를 include한 타입)인지다(콘솔 명령 인자 struct).
template <class T>
concept ReflectedStruct = requires {
    GeneratedType<T>::Id();
    GeneratedType<T>::Ops();
};

// 설정 struct(ROCKS_STRUCT(Settings(...)) — 생성 헤더를 include한 타입)인지다.
template <class T>
concept SettingsStruct = requires {
    GeneratedType<T>::Id();
    GeneratedType<T>::Settings();
};

// 봉인 모드(Sealed)는 헤드리스·시험 기본이다(8절): 파일 로더가 사용자 층 파일을 읽지 않고, 플랫폼·
// 부모 사슬·디렉터리는 옵션으로만 받는다(환경 변수·자동 기기 선택을 쓰지 않는다). Standard는 실행
// 파일용이다(사용자 층 파일을 읽는다).
enum class ConfigMode : uint8 {
    Sealed,
    Standard
};

// 설정 문맥 옵션이다. view는 Initialize가 복사한다(호출 동안만 빌린다).
struct ConfigContextOptions {
    ConfigMode mode = ConfigMode::Sealed;
    // 출시 규칙(DevelopmentOnly 값 고정)이다. 기본은 Release 구성이다. 시험은 개발 빌드에서도 켜서
    // 출시 동작을 확인한다.
    bool releaseRules = ROCKS_BUILD_RELEASE != 0;
    // 플랫폼 이름(`Windows`·`Android` — 플랫폼 층 파일 폴더)이다. 비어 있으면 플랫폼 층을 읽지
    // 않는다.
    StringView platform{};
    // 플랫폼 부모 사슬(약한 것부터 — `Android`면 {"Mobile"})이다(3.3절 — 플랫폼 정의가 선언, 기본
    // 없음). 플랫폼 층은 부모들 → platform 순서로 읽는다.
    Span<const StringView> platformParents{};
    // 경로 이름 값이자 파일 층 위치다(4.3절). engineDir·projectDir의 `Config/` 아래 branch 파일을
    // 읽는다. 사용자 층은 `<userDataDir>/<projectName>/Config/`(Standard 모드만).
    StringView engineDir{};
    StringView projectDir{};
    StringView projectName{};
    StringView userDataDir{};
    StringView savedDir{};
    // 진단 모음 한도(1 이상).
    uint32 maxDiagnostics = DefaultMaxConfigDiagnostics;
};

// 출처 하나다(값 스택의 한 칸). view는 문맥의 입력 기록을 빌린다(다음 Sync까지 유효).
struct ConfigSourceRef {
    ConfigLayer layer = ConfigLayer::CodeDefault;
    // 묶음 이름(파일 층은 Config 폴더 기준 상대 파일 이름 — `Android/Render.json`).
    StringView tag;
    // 사람이 읽는 출처(파일 경로·`<memory>`·`--set` 등).
    StringView sourceName;
};

class ConfigContext;

// 재시작 대기 값 하나다(8절 "실행 중 변경"). 시작(CompleteStartup) 뒤 RestartRequired 값의 새 합친
// 값이 지금 값과 다르면 지금 값을 유지하고 새 값을 대기로 둔다(다음 시작 때 같은 층을 읽으면
// 적용된다 — 사용자 층이면 SaveUserConfig가 저장한다). name은 독립 콘솔 설정이면 콘솔 이름, 설정
// struct 필드면 `<Branch>.<Section>.<필드 도구 이름>`이다. pending·current는 표기다(필드는 compact
// JSON — `4`·`"High"`·`[1,2]`, 콘솔 설정은 콘솔 표기). view는 다음 Sync까지 유효하다.
// CVar 설계(Docs/Design/CVarSystem.ko.md 9절, VD15)의 `BootCVar` + `UserSetting()` 다음 실행용 값과
// Explain `pending restart`가 이 목록을 쓴다(CVR4).
struct ConfigPendingRestart {
    StringView name;
    StringView pending;
    StringView current;
};

// sync 지점 앞 hook이다(ConfigContext::AddSyncHook). Sync가 큐를 보기 전에 소유 스레드에서 등록
// 순서대로 부른다. hook 안에서는 층 입력·Unset·ReplaceLayerText로 큐에 넣을 수 있고(이번 Sync가
// 적용한다) Sync를 다시 부르면 Busy다. 파일 감시(ConfigFileReloader)가 바뀐 파일을 이번 sync
// 지점에 넣는 데 쓴다.
using ConfigSyncHook = void (*)(void* context, ConfigContext& config) noexcept;

// 다른 스레드에서 넣은 쓰기(`CVar::Set`·`Clear`·`ClearLayer`)가 한 번의 Sync 전까지 쓰기 큐에 머물
// 수 있는 수다(넘으면 Busy — 버리지 않는다, Docs/Design/CVarSystem.ko.md 10.1절 표). 소유 스레드의
// 입력은 이 한도와 무관하다.
inline constexpr usize MaxQueuedChanges = 65536;

// ReportUnresolved가 제안할 가까운 이름의 최대 편집 거리다(CVar 12절).
inline constexpr usize MaxSuggestionDistance = 3;

// Sync 한 번의 변경 목록이다(ConfigContext::OnSynced 인자 — CVarSystem.ko.md 7.3절, VD9).
// variables는 유효값이 바뀐 CVar 핸들 번호(이름 순서), settings는 값이 바뀐 설정 struct(등록
// 순서)다. 바뀐 것이 없는 Sync도 빈 목록으로 한 번 부른다. view는 콜백 동안만 유효하다.
struct SyncChanges {
    Span<const cvar::Index> variables;
    Span<const TypeId> settings;
};

// 이름이 실행 중에 정해지는 변수 하나의 정의다(ConfigContext::RegisterDynamic — CVar 5절). Sparks
// 스크립트·플러그인 데이터처럼 핸들을 정적으로 둘 수 없는 변수에 쓴다. 문맥이 복사하므로 view는
// 등록 호출 동안만 빌린다.
//
// 사용 예:
//   DynamicVarDefinition pool;
//   pool.name = "stream.pool_mb";
//   pool.help = "Texture streaming pool size in MiB";
//   pool.type = ConsoleValueType::Int32;
//   pool.defaultValue = "512";
//   pool.hasRange = true;
//   pool.minimum = 64;
//   pool.maximum = 8192;
//   auto index = config.RegisterDynamic(pool);
//   ... int32 megabytes = cvar::ReadDynamic<int32>(*config.Acquire(), index.Value(), 512);
struct DynamicVarDefinition {
    // 소문자 점 경로(IsConsoleSettingName), 첫 마디는 등록된 영역 이름 공간.
    StringView name;
    // 도움말(필수).
    StringView help;
    // cvar.find 분류(비어도 된다).
    StringView category;
    ConsoleValueType type = ConsoleValueType::Bool;
    // Enum 타입의 리플렉션 enum ID(TypeRegistry에 등록돼 있어야 한다 — 닫힌 값 집합은 Enum이다).
    TypeId enumType{};
    // 코드 기본값의 콘솔 표기(`true`·`512`·`1.5`·`High`). 비어 있으면 그 타입의 0·빈 값·enum 첫
    // 열거자다. 범위·enum에 맞아야 한다.
    StringView defaultValue;
    // 성질 비트(cvar::VarFlags의 Cheat·Replicated·UserSetting·ConsoleReadOnly). 부팅 변수는
    // UserSetting만 받는다(BootCVar와 같다). 범위는 아래 칸이다.
    uint32 flags = 0;
    // 이번 실행 동안 고정인 부팅 변수(BootCVar와 같은 규칙 — CVar 9절)인지다.
    bool boot = false;
    // 수 타입(Int32·Int64·Float·Double)의 닫힌 범위다(hasRange일 때). 밖의 값은 거부한다. 끝은
    // 유한해야 하고, 정수 타입이면 정수이고 그 타입의 표현 범위(int32 [-2^31, 2^31-1], int64
    // [-2^63, 2^63)) 안이어야 한다.
    bool hasRange = false;
    double minimum = 0;
    double maximum = 0;
};

// 다른 스레드의 콘솔 입구다(CVar 10.1·15절 — ConfigContext::MakeConsoleInputPort가 만든다). 문맥의
// 쓰기 통로를 참조 계수로 쥐고, 문맥 상태는 건드리지 않는다(다른 스레드가 들어오는 길은 쓰기 큐
// 하나다). 복사할 수 있고 문맥보다 오래 살아도 된다. 단 통로는 문맥의 allocator에서 왔으므로
// 마지막 입구는 그 allocator보다 먼저 없어져야 한다.
//
// 사용 예(입력 스레드):
//   ConsoleInputPort port = config.MakeConsoleInputPort();   // 게임 lane에서 만들어 넘긴다
//   auto result = port.Execute("render.shadow.distance 3000", sink);
class ConsoleInputPort {
public:
    ConsoleInputPort() noexcept = default;

    // 콘솔 입력 한 줄을 넣는다(어느 스레드에서나 — 문법은 ConfigContext::ExecuteConsoleLine과
    // 같다). 이름은 통로가 마지막으로 게시한 snapshot의 콘솔 이름 색인으로 해석하고 권한(콘솔 읽기
    // 전용·치트·복제·부팅 봉인)을 그 snapshot으로 본다. 쓰기·`reset`은 쓰기 큐에 넣고(값 형식은
    // Sync가 꺼낼 때 검사해 거부하면 sink에 알린다), `<이름>`·`<이름> ?`·명령은 다음 Sync 5단계에
    // 게임 lane이 처리한다(출력은 sink — 그때 게임 lane이 쓰므로 sink는 스레드 안전해야 한다).
    // 모르는 이름·권한 거부는 이 스레드에서 sink에 바로 쓴다. 오류: 빈 줄 InvalidArgument, 문맥이
    // 없어졌거나 빈 입구 ShuttingDown, 출시 규칙 Unsupported, 큐가 가득 차면 Busy(버리지 않는다),
    // sink 쓰기 오류, 할당 실패.
    Result<ConsoleLineResult> Execute(StringView line, SharedPtr<ConsoleSink> sink) const noexcept;

private:
    friend class ConfigContext;

    explicit ConsoleInputPort(SharedPtr<detail::WriteChannel> channel) noexcept
        : channel_(Move(channel)) {
    }

    SharedPtr<detail::WriteChannel> channel_;
};

namespace detail {

// 프로세스 기본 문맥 경로의 관찰 지점이다(CVar 20절 CVR7 리뷰 반영 — 시험이 묶음 없는 쓰기와 문맥
// 파괴가 겹치는 순서를 gate로 결정적으로 만든다). acquired는 묶음 없는 쓰기가 기본 문맥의 통로를
// 얻은 뒤(잠금 밖, 큐에 넣기 전), released는 문맥 파괴가 기본 문맥을 푼 뒤(그 쓰기를 기다리기 전)
// 부른다. 함수는 null이어도 된다. 소유 스레드에서 쓰기 스레드를 시작하기 전에 정하고, 그 스레드를
// Join한 뒤 null로 되돌린다(그래서 경쟁이 없다).
struct DefaultWriteProbe {
    void (*acquired)(void* context) noexcept = nullptr;
    void (*released)(void* context) noexcept = nullptr;
    void* context = nullptr;
};

// 지금 관찰 지점이다(null이면 부르지 않는다). 저장소는 Config .cpp의 상수 초기화 정적 변수다(공개
// 헤더에 쓰기 가능한 정적 저장소를 두지 않는다 — Docs/Design/BuildTargets.ko.md 3.8절 BD29).
DefaultWriteProbe*& ActiveDefaultWriteProbe() noexcept;

} // namespace detail

class ConfigContext {
public:
    ConfigContext(Allocator& allocator, ObjectRegistry& objects,
                  const ConfigContextOptions& options) noexcept;
    ~ConfigContext();

    ConfigContext(const ConfigContext&) = delete;
    ConfigContext& operator=(const ConfigContext&) = delete;
    ConfigContext(ConfigContext&&) = delete;
    ConfigContext& operator=(ConfigContext&&) = delete;

    // 옵션을 복사·검사하고 기본 branch 표를 등록한다. 다른 API보다 먼저 한 번 부른다.
    // 오류: 두 번째 호출·옵션 범위(mode·maxDiagnostics 0) InvalidArgument, 다른 스레드 WrongThread,
    // 할당 실패. 실패하면 문맥은 쓸 수 없다(다시 Initialize할 수 없다).
    Result<void> Initialize() noexcept;

    // 플러그인 branch를 더한다(ASCII 식별자). 오류: 형식 InvalidArgument, 이미 있으면
    // AlreadyExists.
    Result<void> RegisterBranch(StringView name) noexcept;

    // 설정 struct T를 섹션으로 등록한다(모듈 등록 함수에서 명시적으로 — 전역 생성자 금지). T의
    // branch는 등록돼 있어야 하고 (branch, section)과 TypeId는 유일해야 한다. 값은 코드 기본값으로
    // 시작하고, 이미 들어온 층 입력이 있으면 다음 Sync가 합친다.
    // 오류: 초기화 전 InvalidArgument, 모르는 branch NotFound, 겹치는 섹션·타입 AlreadyExists,
    // TypeRegistry에 T가 없으면 그 오류(NotFound 등), 할당 실패.
    template <SettingsStruct T>
    Result<void> RegisterSettings() noexcept {
        static_assert(IsNothrowMoveAssignableV<T>,
                      "Settings structs must be nothrow move assignable");
        detail::SettingsTypeOps ops;
        ops.id = GeneratedType<T>::Id();
        ops.settings = &GeneratedType<T>::Settings();
        ops.create = &detail::CreateSettingsValue<T>;
        ops.destroy = &detail::DestroySettingsValue<T>;
        ops.moveAssign = &detail::MoveAssignSettingsValue<T>;
        return RegisterSettingsType(ops);
    }

    // 등록한 설정 struct T의 현재 값이다(마지막 Sync 결과). 다음 Sync 전까지 내용이 바뀌지 않고,
    // 참조 자체는 문맥 수명 동안 유효하다. 전제: RegisterSettings<T>를 했다, 소유 스레드 — 아니면
    // Fatal.
    template <SettingsStruct T>
    const T& Get() const noexcept {
        const void* value = FindSettings(GeneratedType<T>::Id());
        if (value == nullptr) {
            platform::Fatal("ConfigContext::Get: settings struct is not registered");
        }

        return *static_cast<const T*>(value);
    }

    // TypeId로 찾은 설정 값이다(없으면 null). 소유 스레드가 아니면 Fatal.
    const void* FindSettings(TypeId type) const noexcept;

    // 층 입력 하나(branch 파일 내용)를 검사하고 큐에 넣는다(값은 다음 Sync에 바뀐다). text는 JSON
    // 객체이고 줄 주석을 받는다. 최상위 키는 branch의 설정 struct 섹션이거나 `Settings`(콘솔 설정
    // 이름 → 값)다. 섹션은 코드 기본값 위에 적용해 보아 모르는 키·형식·`$` 위치·경로 이름·층 허용을
    // 검사한다(이전 층에 따라 달라지는 배열 연산 결과의 한도는 Sync가 본다).
    // tag는 묶음 이름(같은 층·태그를 Unset으로 한꺼번에 뺀다), sourceName은 진단·출처 표시용이다.
    // 반환: 진단에 Error가 없으면 성공(Note는 남을 수 있다). 소유 스레드의 입력은 쓰기 큐 한도와
    // 무관하다(Busy가 없다 — 소유자가 스스로 조절한다). 오류: 진단을 남겼으면
    // InvalidArgument(Diagnostics 참조 — 아무것도 큐에 넣지 않는다), layer가 CodeDefault이거나
    // 정의되지 않음 InvalidArgument, 모르는 branch NotFound(진단 UnknownBranch), 다른 스레드
    // WrongThread, 할당 실패. 봉인 뒤 바로 쓰는 층(콘솔·코드·범위 임시 변경)의 입력에 부팅 독립
    // 콘솔 설정 값이 하나라도 있으면 문서 전체를 거부한다(진단 BootSealed — 같은 입력의 나머지만
    // 적용하지 않는다).
    Result<void> AddLayerText(ConfigLayer layer, StringView tag, StringView branch,
                              StringView sourceName, StringView text) noexcept;

    // 대입 하나를 큐에 넣는다(세션 `--set=<target>=<value>`·시험 픽스처·코드의 임시 변경). target은
    // `<Branch>.<Section>.<필드 경로>`(필드 경로는 리플렉션 명령행 경로 —
    // `Inner.x`·`scores[a]`)이고 value는 명령행 값 표기다(ReflectionText 10.4절 — 목록은 쉼표,
    // struct 기본값은 `{}`). target이 콘솔 설정 이름(소문자 점 경로)이면 그 설정(필드 콘솔 이름이면
    // 그 필드)에 쓰고, 등록되지 않은 이름이면 대기 값으로 받는다. 코드 기본값 위에 적용해 보아
    // 검사한다(AddLayerText와 같다). 오류: target이 등록 섹션도 콘솔 이름도 아니면
    // InvalidArgument(진단 UnknownTarget), 그 밖은 AddLayerText와 같다.
    Result<void> AddAssignment(ConfigLayer layer, StringView tag, StringView target,
                               StringView value) noexcept;

    // (layer, tag)의 입력을 text 하나로 바꾼다(hot reload — 8절). AddLayerText처럼 검사하고, 받으면
    // Unset(layer, tag)과 새 입력을 한꺼번에 큐에 넣는다(다음 Sync가 옛 입력을 빼고 새 입력을
    // 적용한다 — 다른 스레드의 쓰기가 그 사이에 끼지 않는다). 거부하면 아무것도 넣지 않는다(옛
    // 입력이 그대로 남는다 — 잘못 고친 파일이 값을 지우지 않는다). 오류: AddLayerText와 같다(Busy
    // 없음).
    Result<void> ReplaceLayerText(ConfigLayer layer, StringView tag, StringView branch,
                                  StringView sourceName, StringView text) noexcept;

    // (layer, tag)의 모든 입력을 다음 Sync에서 뺀다(3.1절 `Unset` — 기능 묶음 해제, 품질 등급 변경,
    // 시험 정리). 큐 순서대로 처리하므로 그보다 먼저 넣은 입력(같은 Sync에 들어갈 것 포함)만 뺀다.
    // 없는 태그는 무동작이다. 쓰기 큐 한도와 무관하다(Busy 없음). 오류: 초기화 전·layer 범위
    // InvalidArgument, 다른 스레드 WrongThread, 할당 실패.
    Result<void> Unset(ConfigLayer layer, StringView tag) noexcept;

    // sync 지점이다(Docs/Design/CVarSystem.ko.md 10절 순서): 0) sync hook을 등록 순서대로 부른다
    // 1) 쓰기 큐를 넣은 순서대로 적용한다(다른 스레드에서 넣은 쓰기는 여기서 다시 검사하고,
    // 거부되면 진단을 남기고 버린다) 2) 모든 섹션 값·콘솔 설정 값과 출처 스택을 다시 합친다(묶음
    // 펼침 반복은 CVR5 — 지금은 한 번) 3) 유효값이 바뀐 CVar 칸의 세대를 올린다 4) 바뀐 조각만 새로
    // 만든 snapshot을 게시하고 게임 lane을 다시 묶는다(바뀐 것이 없으면 같은 snapshot) 5) 큐에 든
    // 콘솔 명령을 실행하고 값이 바뀐 대상의 구독 콜백(CF2 — CVR7까지)을 부른다 6) OnSynced를
    // 부른다. 콜백 안에서 Sync를 부르면 Busy다. 새 입력이 더 강한 출처에 가려졌으면 진단 Note
    // WeakWrite를 남긴다. 적용에 실패한 입력(이전 층에 따라 달라진 배열 연산 결과가 원소 수 한도를
    // 넘는 등)은 그 섹션에서 건너뛰고 Error 진단을 남긴다(값은 나머지 입력으로 합친다). 오류: 다른
    // 스레드 WrongThread, 할당 실패(OutOfMemory — 그때 값은 Sync 전 그대로이고 꺼낸 쓰기는 다음
    // Sync가 이어서 적용한다).
    Result<void> Sync() noexcept;

    // 다음 Sync가 적용할 입력이 큐에 있는지다.
    bool HasPendingChanges() const noexcept;

    // sync 지점 앞 hook을 더한다(ConfigSyncHook). context는 RemoveSyncHook 또는 문맥 파괴까지 살아
    // 있어야 한다. 오류: hook 없음 InvalidArgument, 같은 (hook, context)가 이미 있으면
    // AlreadyExists, 다른 스레드 WrongThread, 할당 실패.
    Result<void> AddSyncHook(ConfigSyncHook hook, void* context) noexcept;

    // hook을 뺀다. 오류: 없으면 NotFound, hook 안에서 부르면 Busy, 다른 스레드 WrongThread.
    Result<void> RemoveSyncHook(ConfigSyncHook hook, void* context) noexcept;

    // 재시작 대기 값을 이름 순서로 out에 쓴다(마지막 Sync 기준 — 없으면 빈 목록). 오류: 다른 스레드
    // WrongThread, 할당 실패.
    Result<void> GetPendingRestarts(Array<ConfigPendingRestart>& out) const noexcept;

    // 품질 조합 검증 입력을 정한다(Config 6.1 — Config/QualityValidation.h). Sync가 프리셋
    // 묶음(CVarGroup)을 펼치기 전에 묶음 변수의 등급(사용자가 고른 등급)을 이 규칙으로 낮춘 등급을
    // 펼친다. 묶음 변수 자체는 바꾸지 않는다(장치가 바뀌면 원래 선택이 살아난다 — CVar 11절).
    // 정의된 등급은 등록된 묶음 정의에서 온다. 등록되지 않은 묶음의 규칙은 건너뛰고
    // ReportUnresolved가 알린다(정의되지 않은 등급의 규칙도). traits가 null이면 검증하지 않는다.
    // groups·budgets·traits는 빌린다: 다음 호출이나 문맥 파괴까지 살아 있어야 한다. 다음 Sync가
    // 묶음을 다시 해결한다. 오류: 초기화 전 InvalidArgument, 다른 스레드 WrongThread.
    //
    // 사용 예(호스트 — 규칙은 문맥보다 오래 사는 저장소에 둔다):
    //   QualityLevelRule shadowRules[] = {{3, epicNeeds, {}}};   // 등급 3은 epicNeeds 기기 조건
    //   QualityGroupRule rules[] = {{"quality.shadow", 0, shadowRules}};
    //   REQUIRE(config.SetQualityValidation(rules, {}, &traits));
    Result<void> SetQualityValidation(Span<const QualityGroupRule> groups,
                                      Span<const QualityBudgetRule> budgets,
                                      const DeviceTraits* traits) noexcept;

    // 마지막 Sync가 펼친 묶음별 결과다(등록 순서 — 한 번도 펼치지 않은 묶음은 없다): 요청 등급(묶음
    // 변수 값), 펼친 등급, 낮춘 사유, 가장 낮은 등급도 맞지 않음, 넘친 예산 사유. 콘솔 Explain의
    // `quality.shadow@1 (requested 3, lowered: <사유>)`(FormatQualityResolution)가 쓴다(CVR6).
    // out은 먼저 비운다. 오류: 초기화 전 InvalidArgument, 다른 스레드 WrongThread, 할당 실패(out은
    // 빈다).
    Result<void> GetQualityLevels(QualityResolutionSet& out) const noexcept;

    // 섹션(branch, section)의 필드 경로(도구 이름 — `Inner.x`, 컨테이너는 그 필드)의 출처 스택을
    // out에 쓴다(약한 것부터 — 마지막이 현재 값의 출처). 경로는 대입이 닿은 override
    // 단위다(컨테이너 필드는 그 필드 경로). 기본값뿐이면 빈 목록이다. 오류: 모르는 섹션 NotFound,
    // 다른 스레드 WrongThread, 할당 실패.
    Result<void> GetSources(StringView branch, StringView section, StringView path,
                            Array<ConfigSourceRef>& out) const noexcept;

    // 진단 모음이다(AddLayerText·AddAssignment·Sync가 더한다 — 호출자가 Clear로 비운다).
    ConfigDiagnostics& Diagnostics() noexcept;

    // 모은 진단을 out 뒤에 옮겨 담고 문맥의 모음을 비운다(CVar 12절 — 넘친 수도 out에 더한다). 전부
    // 옮기거나 하나도 옮기지 않는다. 오류: 초기화 전·out이 문맥의 모음 자신이면 InvalidArgument,
    // 다른 스레드 WrongThread, out의 할당 실패(그때 out과 문맥의 모음 모두 그대로다).
    Result<void> TakeDiagnostics(ConfigDiagnostics& out) noexcept;

    // 시작 뒤에도 등록되지 않은 콘솔 이름의 값(파일 `Settings`·콘솔 이름 대입의 대기 값)을 이름마다
    // 한 번 Warning UnresolvedName으로 남긴다(CVar 12절). 편집 거리(Core EditDistance — 대소문자
    // 구분)가 MaxSuggestionDistance 이하인 등록 이름(콘솔 설정·명령 — 출시 규칙에서 숨긴 것
    // 제외)이 있으면 가장 가까운 것을 메시지에 `did you mean '<이름>'?`로 붙인다. 콘솔 명령 이름에
    // 넣은 값도 같은 코드로 "명령은 값을 받지 않는다"고 알린다. 한 문맥에서 이미 보고한 이름은 다시
    // 보고하지 않는다(여러 번 불러도 된다). 호스트가 CompleteStartup 바로 뒤에 부른다. 반환: 이번에
    // 새로 보고한 이름 수. 오류: 초기화 전 InvalidArgument, 다른 스레드 WrongThread, 할당 실패(그때
    // 보고하지 못한 이름은 다음 호출이 다시 본다).
    //
    // 사용 예:
    //   REQUIRE(config.CompleteStartup());
    //   auto unresolved = config.ReportUnresolved();
    Result<usize> ReportUnresolved() noexcept;

    // 콘솔 설정 하나의 설명이다(이름·도움말·종류·지금 값·부팅 변수 여부·재시작 대기 값 — 콘솔
    // Explain·dump가 `[boot]`·`pending restart: <값>`을 보이는 데 쓴다). 오류: 무효 핸들
    // InvalidArgument, 다른 스레드 WrongThread.
    Result<ConsoleSettingInfo> DescribeConsoleSetting(ConsoleSettingId id) const noexcept;

    // ---- 콘솔 설정·명령(Docs/Design/ConfigSystem.ko.md 5절 — 설정 CF2) ----
    //
    // 콘솔 설정은 이름 하나에 값 하나다. 설정 struct 필드의 ConsoleName(RegisterSettings가 함께
    // 등록한다)과 CVar(핸들 Register·동적 변수 RegisterDynamic)가 한 이름 공간이고 콘솔 명령과도
    // 겹칠 수 없다.
    // 값은 층 입력(branch 파일의 `Settings` 섹션, 콘솔 이름 대입)으로 들어오고 Sync에서만 바뀐다.
    // 등록 전에 들어온 이름의 값은 대기 값으로 남았다가 등록 때 넘어간다.

    // 영역 이름 공간(콘솔 이름의 첫 마디 — `[a-z][a-z0-9_]*`)을 더한다. 기본 표는
    // DefaultConsoleRegions다. 오류: 형식 InvalidArgument, 이미 있으면 AlreadyExists, 다른 스레드
    // WrongThread.
    Result<void> RegisterConsoleRegion(StringView region) noexcept;

    // 이름이 실행 중에 정해지는 변수를 등록한다(CVar 5절). 문맥이 정의를 복사해 핸들 정의와 같은
    // 모양으로 쥐고, 번호는 핸들과 같은 프로세스 카운터에서 받으며 값은 같은 CVar 칸에 든다(층·대기
    // 값·콘솔·dump·권한이 핸들과 같다). 이미 들어온 층 입력(대기 값)을 바로 검사·적용하고(거부한
    // 값은 진단을 남기고 건너뛴다) 새 칸을 더한 snapshot을 바로 게시한다(Register와 같다). 번호는
    // 이 문맥에서만 뜻이 있다. 읽기는 cvar::ReadDynamic(snapshot, 번호, fallback)이고, 게임 lane
    // 도구는 FindConsoleSetting·ConsoleValue로 이름으로 본다.
    // 오류: 초기화 전, 이름 문법·영역 표에 없음, 빈 help, 모르는 성질 비트(부팅 변수의 Cheat·
    // Replicated 포함), 수가 아닌 타입의 범위·minimum > maximum·유한하지 않은 범위·정수 타입의
    // 정수가 아니거나 표현 범위를 넘는 범위 끝, Enum과 enumType
    // 불일치, 기본값 거부 InvalidArgument, enumType이 TypeRegistry에 없으면 그 조회 오류, 같은
    // 이름이 이미 있으면 같은 타입 AlreadyExists·다른 타입 TypeMismatch(콘솔 명령이면
    // AlreadyExists), 출시 빌드의 Cheat 변수 Unsupported, 다른 스레드 WrongThread, 할당 실패.
    Result<cvar::Index> RegisterDynamic(const DynamicVarDefinition& definition) noexcept;

    // CVar 핸들(`CVar<T>`·`BootCVar<T>` — Config/CVar.h)을 등록한다(Docs/Design/CVarSystem.ko.md
    // 5절, VD1). 모듈 등록 함수에서 그 모듈의 핸들을 모두 넘긴다(Policy `cvar-registration`이
    // 누락을 잡는다). 핸들 번호(Index)는 처음 등록될 때 프로세스 전체에서 한 번 정해진다(문맥이
    // 여럿이어도 같은 핸들은 같은 번호). 이미 들어온 층 입력(대기 값)을 바로 검사·적용한다(거부한
    // 값은 진단을 남기고 건너뛴다 — 등록은 성공한다). 등록한 변수는 독립 콘솔 설정과 같은 이름
    // 공간·값 저장소에 들고(게임 lane 도구는 FindConsoleSetting·ConsoleValue로 본다), 등록은 바로
    // 보인다: 기존 칸은 그대로 두고 새 칸만 더한 snapshot을 그 자리에서 게시하고 게임 lane을 다시
    // 묶는다(VD17 — 등록 직후 같은 프레임에 `Get()`해도 된다. 등록 전에 만든 snapshot에는 칸이
    // 없다). 출시 빌드의 `Cheat()` 변수는 건너뛴다(13.1절 — 번호도 받지 않는다).
    //
    // 넘긴 핸들을 모두 검사한 뒤에 등록한다(하나라도 거부하면 아무것도 등록하지 않고 진단도 남기지
    // 않는다). 같은 핸들을 다시 넘기면(같은 호출 안 포함) 성공이다(멱등). 프리셋 묶음(CVarGroup)은
    // int32 등급 변수로 등록되고, 등급 항목은 다음 Sync가 품질 묶음 층에 펼친다(CVar 11절) — 그래서
    // 묶음 등록 직후 다음 Sync 전까지는 묶음 등급 칸과 항목 값이 어긋나 보인다. 묶음의 묶음
    // 순환·자기 참조와 같은 변수를 항목으로 가진 두 묶음은 거부한다(InvalidArgument — 이 거부만
    // Error GroupConflict 진단을 남긴다). 묶음과 항목 변수가 둘 다 등록되면 코드 프리셋 값을 항목
    // 변수의 규칙으로 검사해 맞지 않는 것마다 Error를 한 번 남긴다(등록은 성공하고 그 항목은 펼치지
    // 않는다).
    //
    // 사용 예:
    //   Result<void> RegisterRenderVars(ConfigContext& config) noexcept {
    //       return config.Register(ShadowDistance, ShadowQualityLevel, GraphicsBackend);
    //   }
    //
    // 오류: 초기화 전 InvalidArgument, 영역 표에 없는 이름의 첫 마디 InvalidArgument, 호출 스레드
    // 스택 위의 지역 핸들 InvalidArgument(Debug·Development — 핸들은 정적 저장 기간이어야 한다),
    // 같은 이름을 다른 핸들·동적 변수·설정 struct 필드가 이미 쓰면 같은 타입 AlreadyExists·다른
    // 타입 TypeMismatch, 콘솔 명령이 쓰면 AlreadyExists, Name 변수인데 문맥 Registry에 NamePool이
    // 없으면 Unsupported(pool의 오류 — ShuttingDown 등 — 는 그대로), 소유 스레드가 아니면 모든
    // 구성에서 WrongThread, 할당 실패. 호스트는 AlreadyExists·TypeMismatch를 부트 실패로 다룬다.
    // 스레드: 게임 lane(소유 스레드) 전용이다.
    template <class... Handles>
    Result<void> Register(const Handles&... handles) noexcept {
        static_assert(sizeof...(Handles) != 0, "ConfigContext::Register needs at least one handle");
        constexpr bool handlesOnly = (cvar::detail::IsHandle<Handles> && ...);
        static_assert(
            handlesOnly,
            "ConfigContext::Register takes CVar, BootCVar, CVarGroup or ConsoleCommand handles "
            "(Config/CVar.h)");
        if constexpr (handlesOnly) {
            const cvar::detail::VarDefinition* const definitions[] = {
                &cvar::detail::HandleAccess::Definition(handles)...};
            return RegisterHandles(Span<const cvar::detail::VarDefinition* const>(definitions));
        } else {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    // 이름의 콘솔 설정 핸들이다(필드 설정 포함, 출시 규칙에서 숨긴 것 제외). 오류: 없으면 NotFound,
    // 다른 스레드 WrongThread.
    Result<ConsoleSettingId> FindConsoleSetting(StringView name) const noexcept;

    // 콘솔 설정의 현재 값이다(마지막 Sync 결과 — 다음 Sync까지 바뀌지 않는다). kind는 그 종류의
    // ConsoleValueKind이고 Enum은 signedInteger·type이다. 전제: 유효한 핸들, 소유 스레드 — 아니면
    // Fatal.
    const FieldValue& ConsoleValue(ConsoleSettingId id) const noexcept;

    // 등록한 콘솔 명령(Register)에 실행 대상을 묶는다(명령 함수가 CommandContext::Target<T>()로
    // 받는다 — 다른 타입으로 꺼내면 Fatal, CVar 13·15절). 앞 대상이 있으면 바꾼다. 수명: 문맥은
    // 묶인 대상보다 오래 살아야 하고, 대상은 없어지기 전에 UnbindCommand로 풀어야 한다(묶인 대상이
    // 남은 채 문맥을 없애면 Fatal). 묶기·풀기는 게임 lane 전용이다(다른 스레드면 Fatal — 대상의
    // 파괴도 게임 lane에서 한다). 출시 빌드의 `Cheat()` 명령은 등록되지 않으므로 성공하는
    // 무동작이다. 오류: 등록하지 않은 명령 NotFound.
    //
    // 사용 예:
    //   REQUIRE(config.Register(TraceEnableCommand));
    //   REQUIRE(config.BindCommand(TraceEnableCommand, controller));
    //   ... 소멸 때: config.UnbindCommand(TraceEnableCommand, controller);
    template <class T>
    Result<void> BindCommand(const cvar::ConsoleCommand& command, T& target) noexcept {
        return BindCommandTarget(cvar::detail::HandleAccess::Definition(command), &target,
                                 &cvar::detail::TargetTypeTag<T>, nullptr);
    }

    // BindCommand로 묶은 대상이 expected면 푼다(그 뒤 Target은 null). 다른 대상이 묶여 있으면
    // 그대로 둔다(같은 문맥의 두 제어기 — 뒤에 묶은 것이 앞의 것 소멸에 풀리지 않는다). 규칙·오류는
    // BindCommand와 같다.
    template <class T>
    Result<void> UnbindCommand(const cvar::ConsoleCommand& command, T& expected) noexcept {
        return BindCommandTarget(cvar::detail::HandleAccess::Definition(command), nullptr, nullptr,
                                 &expected);
    }

    // 다른 스레드의 콘솔 입구를 만든다(CVar 10.1·15절 — 쓰기 통로를 참조 계수로 쥔다). 입구는
    // 문맥보다 오래 살아도 되고, 문맥이 없어진 뒤의 입력은 ShuttingDown이다. 초기화 전이면 빈
    // 입구(모든 입력이 ShuttingDown)다.
    ConsoleInputPort MakeConsoleInputPort() const noexcept;

    // 문맥의 네트워크 역할을 정한다(CVar 13.2절 — 권한 판정에만 쓴다: 클라이언트는 `Replicated()`
    // 변수의 콘솔 쓰기를 ReplicatedOnClient로 거부한다). 넷 모듈이 접속 상태에 따라 부른다. 오류:
    // 초기화 전·정의되지 않은 역할 InvalidArgument, 다른 스레드 WrongThread.
    Result<void> SetNetworkRole(NetworkRole role) noexcept;

    // 지금 네트워크 역할이다(원자 사본 — 어느 스레드에서나 부른다, 초기화 전이면 Standalone).
    NetworkRole GetNetworkRole() const noexcept;

    // 서버가 복제할 값 목록이다(CVar 13.2절): `Replicated()` 변수 전부(기본값인 것 포함 —
    // 클라이언트의 다른 층 값을 서버 값이 이기게 한다)를 이름 순서로 out 뒤에 붙인다(값은 콘솔 표기
    // — view는 다음 Sync·등록까지 유효). 오류: 초기화 전 InvalidArgument, 다른 스레드 WrongThread,
    // 할당 실패.
    Result<void> ExportReplicated(Array<ReplicatedValue>& out) const noexcept;

    // 서버가 준 복제 목록을 서버 층(태그 `server`)에 넣는다(클라이언트 — 다음 Sync에 적용). 앞
    // 목록을 통째로 바꾸므로 목록에 없는 복제 변수는 서버 층에서 빠진다. 복제 변수가 아니거나
    // 등록되지 않은 이름, 맞지 않는 값은 진단(UnresolvedName·LayerNotAllowed·값 진단)을 남기고
    // 건너뛴다. 반환: 넣은 값 수. 오류: 초기화 전 InvalidArgument, 다른 스레드 WrongThread, 할당
    // 실패(그때 큐에 넣은 것은 없다).
    Result<usize> ImportReplicated(Span<const ReplicatedValue> values) noexcept;

    // 콘솔 이름이 있는 값(CVar·독립 콘솔 설정·설정 struct 필드의 콘솔 이름)을 Config 파일 형식
    // `{"Settings": {...}}`(줄 주석 JSON — Config D1)으로 out에 쓴다(CVar 15절 DumpOverrides).
    // overridesOnly면 기본값과 다른 것만이다. 이름 순서이고 값마다 앞 줄에 `// <층> <출처>`
    // 주석(부팅 변수는 `[boot]`)을 단다. 이 출력을 빈 문맥의 세션 층으로 읽으면 같은 유효값이다.
    // 출시 규칙에서 숨긴 설정은 넣지 않는다. 오류: 초기화 전 InvalidArgument, 다른 스레드
    // WrongThread, 할당 실패.
    Result<void> DumpConsoleValues(String& out, bool overridesOnly) const noexcept;

    // 콘솔 입력 한 줄을 처리한다(CVar 15절 문법). 게임 lane 전용이다(다른 스레드는
    // ConsoleInputPort).
    // - `<이름>`: 값(enum은 `3 (High)`, 부팅 변수 `[boot]`, 재시작 대기·품질 묶음 펼침)과 출처
    //   스택을 쓴다. `<이름> ?`: Explain(종류·범위·도움말·출처 스택)을 쓴다.
    // - `<이름> <값>`: 콘솔 층(태그 `console`) 쓰기, `<이름> reset`: 콘솔 층 값 지우기를 큐에
    //   넣는다(다음 Sync에 적용). 권한(콘솔 읽기 전용·치트·복제·부팅 봉인)과 값을 지금 검사한다.
    // - `<명령> <인자…>`(`cvar.find <텍스트>`·`cvar.explain <이름>`·`cvar.dump [overrides]` 포함):
    //   인자를 검사해 큐에 넣고 다음 Sync 5단계에 실행한다.
    // 거부한 입력은 사유를 sink에 쓰고 Rejected다(`<이름> <사유>` 한 줄). 큐에 넣은 일은 sink를
    // 쥐고 있다(ConsoleSink). 인자와 값의 구분은 공백·탭이고 따옴표 묶음은 없다. 오류: 초기화 전·빈
    // 줄 InvalidArgument, 출시 규칙(출시 빌드에는 콘솔 입력이 없다) Unsupported, 다른 스레드
    // WrongThread, sink 쓰기 오류, 할당 실패.
    Result<ConsoleLineResult> ExecuteConsoleLine(StringView line,
                                                 SharedPtr<ConsoleSink> sink) noexcept;

    // prefix로 시작하는 콘솔 설정·명령 이름을 이름 순서로 out에 쓴다(자동완성 — 출시 규칙에서 숨긴
    // 설정 제외). view는 다음 등록까지 유효하다. 오류: 다른 스레드 WrongThread, 할당 실패.
    Result<void> CompleteConsoleName(StringView prefix, Array<StringView>& out) const noexcept;

    // CVar 범위를 만든다(CVar 8절 — parent의 자식, 기본은 Global). id는 프로세스에서 다시 쓰지
    // 않는다. 새 범위는 다음 Sync가 게시하는 snapshot부터 보인다(그 전의 `Get(scope)`는 Global
    // 값이다). 범위는 객체가 소유한다(World·플레이어·Viewport — 14.2절). 게임 lane 전용이다. 오류:
    // 초기화 전 InvalidArgument, parent가 없음(파괴됐거나 다른 문맥) NotFound, 다른 스레드
    // WrongThread, 할당 실패.
    //
    // 사용 예:
    //   auto scope = config.CreateScope();             // World 범위(Global의 자식)
    //   ShadowDistance.Set(1200.f, {.scope = scope.Value()});
    //   config.Sync();
    //   const float distance = ShadowDistance.Get(scope.Value());
    //   config.DestroyScope(scope.Value());
    Result<cvar::CVarScopeId> CreateScope(cvar::CVarScopeId parent = {}) noexcept;

    // CVar 범위를 파괴한다(그 범위의 값은 버리고 자식은 조부모에 다시 붙는다 — 다음 Sync가
    // 게시한다). 그 범위에 큐로 들어온 쓰기는 적용 때 버린다. 게임 lane 전용이다. 오류: 초기화 전·
    // Global InvalidArgument, 없는 범위 NotFound, 다른 스레드 WrongThread.
    Result<void> DestroyScope(cvar::CVarScopeId scope) noexcept;

    // 이 문맥을 프로세스 기본 문맥으로 정한다(CVar 6.2절 — 묶음 없는 스레드의 `Set`·`Clear`·
    // `ScopedOverride`가 이 문맥의 쓰기 큐에 간다. 읽기 `Get`은 늘 묶음이다). 프로세스에 하나이고,
    // 문맥이 없어질 때 풀린다. 호스트(HostSettings::Initialize)가 부른다. 같은 문맥이 다시 부르면
    // 성공이다. 문맥 파괴는 기본 문맥을 거쳐 통로를 얻은 쓰기가 끝날 때까지 기다린다(그 쓰기는
    // ShuttingDown — 통로의 마지막 해제가 소유 스레드에서 일어난다). 기본 문맥이 없을 때의 묶음
    // 없는 쓰기는 NotFound다. 기본 문맥을 거친 ScopedOverride는 문맥 allocator보다 먼저 끝나야
    // 한다(ConsoleInputPort와 같다). 오류: 초기화 전 InvalidArgument, 다른 문맥이 이미 기본
    // AlreadyExists, 다른 스레드 WrongThread.
    Result<void> MakeProcessDefault() noexcept;

    // 모듈 코드 수명 토큰을 문맥이 맡는다(Docs/Design/ModuleLoader.ko.md 3.10절 MD16). 모듈의
    // RegisterSettings가 등록한 CVar 핸들·설정 struct·콘솔 명령 정의는 그 모듈의 코드·데이터를
    // 가리키므로, 설정 등록을 부르는 쪽(HostSettings)이 그 모듈의 lease를 여기에 맡겨 문맥이 사는
    // 동안 모듈 코드를 붙잡는다. 맡긴 lease는 문맥이 없어질 때 등록을 모두 정리한 뒤 놓는다. 빈
    // lease(정적 링크 코드)는 맡기지 않아도 된다. 오류: 할당 실패(lease는 놓인다). 소유
    // 스레드에서만 부른다.
    Result<void> RetainModule(ModuleLease lease) noexcept;

    // 맡은 모듈 lease 수다(진단·시험용).
    usize RetainedModuleCount() const noexcept {
        return moduleLeases_.Size();
    }

    // 시작이 끝났음을 알리고 부팅 값을 봉인한다(CVar 9절). 부팅 변수(BootCVar)의 지금 값을 불변
    // 부팅 블록으로 굳혀 새 snapshot으로 게시한다(봉인 뒤 snapshot의 부팅 값은 모두 같다 —
    // BootCVar::Get이 읽는다). 그 뒤로 부팅 값(StartupOnly·RestartRequired — 콘솔 설정·설정 struct
    // 필드)은 이번 실행 동안 지금 값을 유지한다:
    // - 독립 콘솔 설정(BootCVar 포함)에 바로 쓰는 층(콘솔·코드·범위 임시 변경)의 쓰기는 넣을 때
    //   Error BootSealed로 거부한다(이름을 몰라 받은 쓰기는 그 이름을 늦게 등록할 때 받지 않고
    //   BootSealed를 남긴다).
    // - 파일 층(엔진·프로젝트·플랫폼·사용자)에서 온 변경은 재시작 대기(GetPendingRestarts)다.
    // - 그 밖의 층(기기 프로필·품질 같은 파생 층, 세션·서버 같은 휘발 층)과 설정 struct 필드의 콘솔
    //   쓰기는 Warning BootValueIgnored(같은 이름·층·출처에 한 번)다.
    // 큐에 남은 쓰기가 있으면(HasPendingChanges) 봉인하지 않고 Busy다 — 먼저 Sync한다. 두 번째
    // 호출은 아무것도 하지 않고 성공한다. 오류: 초기화 전 InvalidArgument, 남은 쓰기 Busy, 다른
    // 스레드 WrongThread, 할당 실패(그때는 봉인하지 않는다 — 다시 부를 수 있다).
    Result<void> CompleteStartup() noexcept;

    // 마지막으로 게시한 설정 snapshot이다(Docs/Design/CVarSystem.ko.md 7.2절 — CVar 칸과 RenderRead
    // 이름 블록, Config 5.3절). 어느 스레드에서나 부를 수 있다(게시와 함께 짧은 잠금을 잡는다 —
    // 읽기 경로가 아니다). Initialize가 빈 snapshot을 게시하므로 초기화 뒤에는 null이 아니다(초기화
    // 전이면 null). 바뀐 것이 없는 Sync는 같은 snapshot(같은 세대)을 남긴다. 렌더 lane·worker는 이
    // snapshot을 cvar::SnapshotBinding으로 묶어 `CVar::Get()`으로 읽는다.
    //
    // 사용 예(다른 스레드):
    //   cvar::SnapshotBinding bind{config.Acquire()};
    //   const float distance = ShadowDistance.Get();
    SharedPtr<const ConfigSnapshot> Acquire() const noexcept;

    // 층 하나의 모든 입력(모든 태그)을 다음 Sync에서 뺀다(6.1절 —
    // `ClearLayer(ConfigLayer::Hotfix)`로 핫픽스 전체를 되돌린다). 어느 스레드에서나 부를 수
    // 있다(쓰기 큐에 넣는다). 오류: 초기화 전·CodeDefault·정의되지 않은 층 InvalidArgument, 다른
    // 스레드에서 불렀는데 큐 한도를 넘으면 Busy, 할당 실패.
    Result<void> ClearLayer(ConfigLayer layer) noexcept;

    // Sync마다 한 번(값 적용·snapshot 게시·명령 실행 뒤) 부르는 변경 알림이다(7.3절, VD9 — 변수
    // 하나마다의 콜백은 없다). 게임 lane에서 문맥 잠금 없이 부른다. 콜백 안의 `Set`·층 입력은 다음
    // Sync에 적용되고, 콜백 안에서 Sync를 부르면 Busy다. 소유 스레드에서만 더하고 뺀다.
    //
    // 사용 예:
    //   auto handle = config.OnSynced().TryAddLambda([](const SyncChanges& changes) noexcept { ...
    //   });
    MulticastDelegate<void(const SyncChanges&)>& OnSynced() noexcept;

    // 내부 상태다(config-dump·파일 로더 등 모듈 안 구현이 쓴다). 초기화 전이면 null이다.
    detail::ConfigState* State() const noexcept {
        return state_.Get();
    }

private:
    Result<void> RegisterSettingsType(const detail::SettingsTypeOps& ops) noexcept;
    Result<void>
    RegisterHandles(Span<const cvar::detail::VarDefinition* const> definitions) noexcept;
    Result<void> BindCommandTarget(const cvar::detail::VarDefinition& command, void* target,
                                   const void* targetType, const void* expected) noexcept;

    Allocator* allocator_;
    ObjectRegistry* objects_;
    ConfigContextOptions options_;
    // RetainModule로 맡은 모듈 lease다. state_보다 먼저 선언해 나중에 소멸시킨다(등록을 모두 정리한
    // 뒤 모듈 코드를 놓는다).
    Array<ModuleLease> moduleLeases_;
    UniquePtr<detail::ConfigState> state_;
};

} // namespace rocks::config

// 호스트 서비스다(Docs/Design/ModuleLoader.ko.md 3.12절). 엔진 호스트가 모듈 단계 실행 전에
// 등록하고 호스트 묶음을 내린 뒤 뗀다.
ROCKS_DECLARE_HOST_SERVICE(rocks::config::ConfigContext)
