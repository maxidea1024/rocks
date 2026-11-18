#pragma once

#include <Base/Compiler.h>
#include <Config/CVarDefinition.h>
#include <Config/ConfigTypes.h>
#include <Config/ConsoleSettings.h>
#include <Core/Delegate.h>
#include <Core/Memory/UniquePtr.h>
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

// Sync 한 번의 변경 목록이다(ConfigContext::OnSynced 인자 — CVarSystem.ko.md 7.3절, VD9).
// variables는 유효값이 바뀐 CVar 핸들 번호(이름 순서), settings는 값이 바뀐 설정 struct(등록
// 순서)다. 바뀐 것이 없는 Sync도 빈 목록으로 한 번 부른다. view는 콜백 동안만 유효하다.
struct SyncChanges {
    Span<const cvar::Index> variables;
    Span<const TypeId> settings;
};

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
    // WrongThread, 할당 실패.
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

    // 섹션(branch, section)의 필드 경로(도구 이름 — `Inner.x`, 컨테이너는 그 필드)의 출처 스택을
    // out에 쓴다(약한 것부터 — 마지막이 현재 값의 출처). 경로는 대입이 닿은 override
    // 단위다(컨테이너 필드는 그 필드 경로). 기본값뿐이면 빈 목록이다. 오류: 모르는 섹션 NotFound,
    // 다른 스레드 WrongThread, 할당 실패.
    Result<void> GetSources(StringView branch, StringView section, StringView path,
                            Array<ConfigSourceRef>& out) const noexcept;

    // 진단 모음이다(AddLayerText·AddAssignment·Sync가 더한다 — 호출자가 Clear로 비운다).
    ConfigDiagnostics& Diagnostics() noexcept;

    // ---- 콘솔 설정·명령(Docs/Design/ConfigSystem.ko.md 5절 — 설정 CF2) ----
    //
    // 콘솔 설정은 이름 하나에 값 하나다. 설정 struct 필드의 ConsoleName(RegisterSettings가 함께
    // 등록한다)과 독립 설정(RegisterConsoleSetting)이 한 이름 공간이고 콘솔 명령과도 겹칠 수 없다.
    // 값은 층 입력(branch 파일의 `Settings` 섹션, 콘솔 이름 대입)으로 들어오고 Sync에서만 바뀐다.
    // 등록 전에 들어온 이름의 값은 대기 값으로 남았다가 등록 때 넘어간다.

    // 영역 이름 공간(콘솔 이름의 첫 마디 — `[a-z][a-z0-9_]*`)을 더한다. 기본 표는
    // DefaultConsoleRegions다. 오류: 형식 InvalidArgument, 이미 있으면 AlreadyExists, 다른 스레드
    // WrongThread.
    Result<void> RegisterConsoleRegion(StringView region) noexcept;

    // 독립 콘솔 설정을 등록한다. 이미 들어온 층 입력의 대기 값을 바로 검사·적용한다(거부한 값은
    // 진단을 남기고 건너뛴다 — 등록은 성공한다). 출시 규칙의 DevelopmentOnly 설정은 등록되지만
    // 콘솔에 보이지 않고 값은 기본값으로 고정된다.
    // 오류: 이름 문법·영역 표에 없음·help 비어 있음·모르는 플래그·범위(minimum > maximum, 수가 아닌
    // 종류의 범위)·enum 아닌 종류의 enumType·기본값 표기 오류 InvalidArgument, Enum 종류의
    // enumType이 없으면 NotFound, 이름이 이미 있으면 AlreadyExists, 다른 스레드 WrongThread, 할당
    // 실패.
    Result<ConsoleSettingId>
    RegisterConsoleSetting(const ConsoleSettingDefinition& definition) noexcept;

    // CVar 핸들(`CVar<T>`·`BootCVar<T>` — Config/CVar.h)을 등록한다(Docs/Design/CVarSystem.ko.md
    // 5절, VD1). 모듈 등록 함수에서 그 모듈의 핸들을 모두 넘긴다(Policy `cvar-registration`이
    // 누락을 잡는다). 핸들 번호(Index)는 처음 등록될 때 프로세스 전체에서 한 번 정해진다(문맥이
    // 여럿이어도 같은 핸들은 같은 번호). 이미 들어온 층 입력(대기 값)을 바로 검사·적용한다(거부한
    // 값은 진단을 남기고 건너뛴다 — 등록은 성공한다). 등록한 변수는 독립 콘솔 설정과 같은 이름
    // 공간·값 저장소에 들고(FindConsoleSetting·ConsoleValue로 보인다), 등록은 바로 보인다: 기존
    // 칸은 그대로 두고 새 칸만 더한 snapshot을 그 자리에서 게시하고 게임 lane을 다시 묶는다(VD17 —
    // 등록 직후 같은 프레임에 `Get()`해도 된다. 등록 전에 만든 snapshot에는 칸이 없다). 출시 빌드의
    // `Cheat()` 변수는 건너뛴다(13.1절 — 번호도 받지 않는다).
    //
    // 넘긴 핸들을 모두 검사한 뒤에 등록한다(하나라도 거부하면 아무것도 등록하지 않고 진단도 남기지
    // 않는다). 같은 핸들을 다시 넘기면(같은 호출 안 포함) 성공이다(멱등).
    //
    // 사용 예:
    //   Result<void> RegisterRenderVars(ConfigContext& config) noexcept {
    //       return config.Register(ShadowDistance, ShadowQualityLevel, GraphicsBackend);
    //   }
    //
    // 오류: 초기화 전 InvalidArgument, 영역 표에 없는 이름의 첫 마디 InvalidArgument, 호출 스레드
    // 스택 위의 지역 핸들 InvalidArgument(Debug·Development — 핸들은 정적 저장 기간이어야 한다),
    // 같은 이름을 다른 핸들·독립 설정·설정 struct 필드가 이미 쓰면 같은 타입 AlreadyExists·다른
    // 타입 TypeMismatch, 콘솔 명령이 쓰면 AlreadyExists, Name 변수인데 문맥 Registry에 NamePool이
    // 없으면 Unsupported(pool의 오류 — ShuttingDown 등 — 는 그대로), 소유 스레드가 아니면 모든
    // 구성에서 WrongThread, 할당 실패. 호스트는 AlreadyExists·TypeMismatch를 부트 실패로 다룬다.
    // 스레드: 게임 lane(소유 스레드) 전용이다.
    template <class... Handles>
    Result<void> Register(const Handles&... handles) noexcept {
        static_assert(sizeof...(Handles) != 0, "ConfigContext::Register needs at least one handle");
        constexpr bool handlesOnly = (cvar::detail::IsHandle<Handles> && ...);
        static_assert(handlesOnly,
                      "ConfigContext::Register takes CVar or BootCVar handles (Config/CVar.h)");
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

    // 인자 없는 콘솔 명령을 등록한다. 오류: 이름 문법·영역·help·execute 없음 InvalidArgument, 이름
    // 겹침 AlreadyExists, 다른 스레드 WrongThread, 할당 실패.
    Result<void> RegisterConsoleCommand(const ConsoleCommandDefinition& definition) noexcept;

    // 인자 struct Args(리플렉션 struct — 강한 참조 없음)를 받는 콘솔 명령을 등록한다. 콘솔 입력
    // `<명령> a=1 b=x`의 각 `<경로>=<값>`을 Args 기본값 위에 대입해(리플렉션 명령행 표기) execute에
    // `const Args*`로 넘긴다. 오류: RegisterConsoleCommand와 같고, Args가 TypeRegistry에 없으면 그
    // 오류.
    template <ReflectedStruct Args>
    Result<void> RegisterConsoleCommand(const ConsoleCommandDefinition& definition) noexcept {
        detail::SettingsTypeOps ops;
        ops.id = GeneratedType<Args>::Id();
        ops.create = &detail::CreateSettingsValue<Args>;
        ops.destroy = &detail::DestroySettingsValue<Args>;
        return RegisterConsoleCommandType(definition, &ops);
    }

    // 콘솔 입력 한 줄을 처리한다(5.2절 문법): `<이름>`은 값과 출처 스택을, `<이름> ?`는 도움말을
    // output에 쓴다. `<이름> <값>`은 콘솔 층(태그 `console`) 쓰기를, `<명령> <인자…>`는 명령 실행을
    // 큐에 넣는다(다음 Sync에 적용·실행). 거부한 입력은 진단을 output에 쓰고 Rejected다. output은
    // 큐에 넣은 명령이 실행되는 다음 Sync까지 살아 있어야 한다.
    // 오류: 출시 규칙(출시 빌드에는 콘솔 입력이 없다) Unsupported, 빈 줄 InvalidArgument, 다른
    // 스레드 WrongThread, output 쓰기 오류, 할당 실패.
    Result<ConsoleLineResult> ExecuteConsoleLine(StringView line, ConsoleOutput output) noexcept;

    // prefix로 시작하는 콘솔 설정·명령 이름을 이름 순서로 out에 쓴다(자동완성 — 출시 규칙에서 숨긴
    // 설정 제외). view는 다음 등록까지 유효하다. 오류: 다른 스레드 WrongThread, 할당 실패.
    Result<void> CompleteConsoleName(StringView prefix, Array<StringView>& out) const noexcept;

    // 콘솔 설정 값이 바뀔 때 부를 콜백을 구독한다(Sync 끝, 구독 순서대로). 오류: 모르는 이름
    // NotFound, callback 없음 InvalidArgument, 다른 스레드 WrongThread, 할당 실패.
    Result<ConfigSubscriptionId> Subscribe(StringView name, ConfigChangeCallback callback,
                                           void* context) noexcept;

    // 설정 struct T의 값(어느 필드든)이 바뀔 때 부를 콜백을 구독한다. 오류: Subscribe와 같다(T를
    // 등록 하지 않았으면 NotFound).
    template <SettingsStruct T>
    Result<ConfigSubscriptionId> SubscribeSettings(ConfigChangeCallback callback,
                                                   void* context) noexcept {
        return SubscribeSection(GeneratedType<T>::Id(), callback, context);
    }

    // 구독을 끝낸다(이번 Sync의 콜백 도중이면 그 콜백 다음부터 부르지 않는다). 오류: 없는 구독
    // NotFound, 다른 스레드 WrongThread.
    Result<void> Unsubscribe(ConfigSubscriptionId id) noexcept;

    // 시작이 끝났음을 알린다. 그 뒤로 부팅 값(StartupOnly·RestartRequired — 콘솔 설정·설정 struct
    // 필드)은 이번 실행 동안 지금 값을 유지한다: 파일 층(엔진·프로젝트·플랫폼·사용자)에서 온 변경은
    // 재시작 대기(GetPendingRestarts)로, 그 밖의 층(기기 프로필·품질 같은 파생 층, 세션·콘솔 같은
    // 휘발 층)에서 온 변경은 Warning BootValueIgnored(같은 이름·층·출처에 한 번)로 남긴다. 쓰기
    // 자체는 거부하지 않는다. 오류: 다른 스레드 WrongThread.
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
    Result<void> RegisterConsoleCommandType(const ConsoleCommandDefinition& definition,
                                            const detail::SettingsTypeOps* arguments) noexcept;
    Result<ConfigSubscriptionId> SubscribeSection(TypeId settings, ConfigChangeCallback callback,
                                                  void* context) noexcept;

    Allocator* allocator_;
    ObjectRegistry* objects_;
    ConfigContextOptions options_;
    UniquePtr<detail::ConfigState> state_;
};

} // namespace rocks::config
