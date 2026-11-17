#pragma once

#include <Base/Compiler.h>
#include <Config/ConfigTypes.h>
#include <Config/ConsoleSettings.h>
#include <Core/Memory/UniquePtr.h>
#include <Meta/GeneratedType.h>
#include <Meta/SettingsSchema.h>
#include <Object/GeneratedFieldAccess.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Fatal.h>

// 설정 문맥이다(Docs/Design/ConfigSystem.ko.md 3·4·5·7·8절 — 설정 CF1·CF2). 한 실행(또는 시험
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
// 스레드: 소유 스레드(만든 스레드 — 게임 lane)에서만 쓴다. 다른 스레드의 호출은 WrongThread다(Get은
// Fatal). 값은 Sync에서만 바뀌므로 그 사이 Get이 돌려준 참조의 내용은 바뀌지 않는다.
//
// 수명: allocator·objects는 문맥보다 오래 살아야 한다. 설정 struct 타입의 모듈(생성 코드)과 그
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

    return ConstructAt<T>(storage.Value(), rocks::detail::MakeAllocatedValue<T>(allocator));
}

template <class T>
void DestroySettingsValue(Allocator& allocator, void* value) noexcept {
    DestroyAt(static_cast<T*>(value));
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
    // 반환: 진단에 Error가 없으면 성공(Note는 남을 수 있다). 오류: 진단을 남겼으면
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

    // (layer, tag)의 모든 입력을 다음 Sync에서 뺀다(3.1절 `Unset` — 기능 묶음 해제, 품질 등급 변경,
    // 시험 정리). 큐 순서대로 처리하므로 그보다 먼저 넣은 입력(같은 Sync에 들어갈 것 포함)만 뺀다.
    // 없는 태그는 무동작이다. 오류: layer 범위 InvalidArgument, 다른 스레드 WrongThread, 할당 실패.
    Result<void> Unset(ConfigLayer layer, StringView tag) noexcept;

    // sync 지점이다(5.3절): 큐를 순서대로 적용하고 모든 섹션 값·콘솔 설정 값과 출처 스택을 다시
    // 합친다. 그 뒤 큐에 든 콘솔 명령을 실행하고, 값이 바뀐 대상의 구독 콜백을 구독 순서대로
    // 부르고, RenderRead 값이 바뀌었으면 새 설정 snapshot(세대 +1)을 만든다. 콜백 안에서 Sync를
    // 부르면 Busy다. 새 입력이 더 강한 출처에 가려졌으면 진단 Note WeakWrite를 남긴다. 적용에
    // 실패한 입력(이전 층에 따라 달라진 배열 연산 결과가 원소 수 한도를 넘는 등)은 그 섹션에서
    // 건너뛰고 Error 진단을 남긴다(값은 나머지 입력으로 합친다). 큐가 비었으면 아무것도 하지
    // 않는다. 오류: 다른 스레드 WrongThread, 할당 실패(OutOfMemory — 그때 값은 Sync 전 그대로이고
    // 큐도 그대로다).
    Result<void> Sync() noexcept;

    // 다음 Sync가 적용할 입력이 큐에 있는지다.
    bool HasPendingChanges() const noexcept;

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

    // 시작이 끝났음을 알린다. 그 뒤로 StartupOnly 값(콘솔 설정·설정 struct 필드)에 새로 들어오는
    // 쓰기는 거부한다(이미 들어온 값은 그대로). 오류: 다른 스레드 WrongThread.
    Result<void> CompleteStartup() noexcept;

    // 마지막 Sync의 설정 snapshot이다(5.3절 — RenderRead 값의 불변 블록과 세대 번호). 첫 Sync
    // 전에는 null이다. RenderRead 값이 바뀌지 않은 Sync는 같은 snapshot(같은 세대)을 돌려준다. 렌더
    // lane은 이 snapshot만 읽는다(문맥은 읽지 않는다). 소유 스레드가 아니면 Fatal.
    SharedPtr<ConfigSnapshot> Snapshot() const noexcept;

    // 내부 상태다(config-dump·파일 로더 등 모듈 안 구현이 쓴다). 초기화 전이면 null이다.
    detail::ConfigState* State() const noexcept {
        return state_.Get();
    }

private:
    Result<void> RegisterSettingsType(const detail::SettingsTypeOps& ops) noexcept;
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
