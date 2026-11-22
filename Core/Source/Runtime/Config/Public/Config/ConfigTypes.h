#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <Core/Text/Json.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <ReflectionText/TextDiagnostics.h>

// 설정 계층의 공용 형식이다(Docs/Design/ConfigSystem.ko.md 2·3절): 층 순서(ConfigLayer), branch
// 이름 표, 경로 치환 표, 진단 모음(ConfigDiagnostics). UE 대응: 층은 config layer·콘솔 변수 SetBy
// (`Core/Public/Misc/ConfigHierarchy.h`, `Core/Public/HAL/IConsoleManager.h`)를 하나로 합친 것이다.
namespace rocks::config {

// 설정 층이다. 약한 것부터 강한 것 순서이고 값마다 이 순서로 출처를 쌓는다(3.1절 표). 더 강한 층의
// 값이 현재 값이고, 약한 층의 쓰기는 값을 바꾸지 않지만 출처 스택에는 남는다.
enum class ConfigLayer : uint8 {
    // 설정 struct의 기본값·콘솔 설정 등록값(파일이 없다 — 출처 스택에 넣지 않는다).
    CodeDefault,
    // Engine/Config/<Branch>.json
    Engine,
    // Engine/Config/<Platform>/<Branch>.json(플랫폼 부모 사슬을 약한 것부터 — 3.3절)
    EnginePlatform,
    // <Project>/Config/<Branch>.json
    Project,
    // <Project>/Config/<Platform>/<Branch>.json
    ProjectPlatform,
    // 플러그인·게임 기능이 더하는 층(태그 = 묶음 이름)
    Feature,
    // 품질 등급이 펼친 값(CF4)
    Quality,
    // 선택된 기기 프로필과 부모 사슬(CF4)
    DeviceProfile,
    // <UserData>/<Project>/Config/<Branch>.json — 사용자 설정 가능(UserConfig) 필드만
    User,
    // 라이브 운영 핫픽스(태그 = 핫픽스 묶음 — Docs/Design/CVarSystem.ko.md 6.1절, UE
    // ECVF_SetByHotfix)
    Hotfix,
    // 명령줄 `--set=`·시험 픽스처
    Session,
    // 코드의 `CVar::Set`(출처 = 호출 위치 — UE ECVF_SetByCode)
    Code,
    // 범위 임시 변경(`cvar::ScopedOverride`·시험 — 태그마다 하나, 콘솔보다 약하다 — UE
    // ECVF_SetByTemp)
    Scoped,
    // 실행 중 콘솔 입력
    Console,
    // 서버가 복제한 값(가장 강하다 — 클라이언트의 콘솔·코드·임시 변경이 이기지 못한다, CVar 13.2절)
    Server
};

// 층 수다(ConfigLayer::Server + 1).
inline constexpr uint32 ConfigLayerCount = static_cast<uint32>(ConfigLayer::Server) + 1;

// 정의된 층 값인지다.
constexpr bool IsConfigLayer(ConfigLayer layer) noexcept {
    return static_cast<uint32>(layer) < ConfigLayerCount;
}

// 층 이름(열거자 철자 — "Engine" 등)이다. 정의되지 않은 값이면 빈 view다. config-dump·진단이 쓴다.
StringView ConfigLayerName(ConfigLayer layer) noexcept;

// 기본 branch 이름 표다(3.2절 — 파일 단위). ConfigContext::Initialize가 이 순서로 등록하고,
// 플러그인은 RegisterBranch로 자기 branch를 더한다. 파일 로더는 등록 순서로 branch 파일을 읽는다.
inline constexpr StringView DefaultConfigBranches[] = {
    "Engine", "Game", "Input", "Render", "Quality", "DeviceProfiles", "Editor"};

// 프리셋 묶음 값 파일(`Quality.json` — 묶음 → 등급 → 항목 → 값, 6.1절)의 branch 이름이다. 이 branch
// 문서는 설정 struct 섹션이 아니라 코드가 정의한 묶음 항목의 값만 담는다(CVar 11절).
inline constexpr StringView QualityBranch = "Quality";

// branch 파일 안에서 콘솔 설정 이름 → 값 섹션의 예약 키다(3.2절 — `"Settings": {"a.b": 1}`).
// 설정 struct의 Section으로 쓸 수 없다(MetaCompiler RMC2210).
inline constexpr StringView ConsoleSettingsSection = "Settings";

// 콘솔 설정 이름 문법인지다(5.1절 — 결정 D7): 소문자 점 경로 `[a-z][a-z0-9_]*('.'
// [a-z][a-z0-9_]*)+` (마디 둘 이상). 첫 마디는 영역 이름 공간이다(등록 표 판정은 콘솔 설정 등록이
// 한다). MetaCompiler의 ConsoleName 검사와 같은 규칙이다.
//
// 사용 예:
//   IsConsoleSettingName("render.graph.immediate");  // true
//   IsConsoleSettingName("Render.Graph");            // false(대문자)
//
// constexpr이라 CVar 빌더가 컴파일 때 같은 함수로 이름을 검사한다(Docs/Design/CVarSystem.ko.md
// 4.3절).
constexpr bool IsConsoleSettingName(StringView name) noexcept {
    usize segments = 0;
    bool start = true;
    for (const char c : name) {
        if (c == '.') {
            if (start) {
                return false;
            }

            start = true;
            continue;
        }

        const bool lower = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        if (start ? !lower : !(lower || digit || c == '_')) {
            return false;
        }

        if (start) {
            ++segments;
        }

        start = false;
    }

    return !start && segments >= 2;
}

// 문자열 필드 값에서 읽을 때 펼치는 경로 이름 표다(4.3절 — UE `%GAMEDIR%` 등 대응). 값은
// ConfigContextOptions의 같은 이름 디렉터리다. 표에 없는 `{이름}`과 값이 비어 있는 이름은 오류다.
enum class ConfigPathToken : uint8 {
    EngineDir,
    ProjectDir,
    UserDataDir,
    SavedDir
};

inline constexpr StringView ConfigPathTokenNames[] = {"EngineDir", "ProjectDir", "UserDataDir",
                                                      "SavedDir"};

// 설정 진단 종류다. ConfigDiagnosticCodeName이 고정 이름을 준다.
enum class ConfigDiagnosticCode : uint8 {
    // JSON 문법 오류(줄 주석은 허용 — 4.1절). textCode는 쓰지 않는다.
    JsonSyntax,
    // 등록되지 않은 branch 이름이다.
    UnknownBranch,
    // branch 파일의 최상위 키가 그 branch의 설정 struct 섹션도 `Settings`도 아니다.
    UnknownSection,
    // 섹션 값이 JSON 객체가 아니다.
    InvalidSection,
    // 설정 struct 값의 리플렉션 진단이다(모르는 키·형식·범위 등 — textCode가 종류).
    FieldValue,
    // 배열 연산 객체가 잘못됐다(모르는 연산·값 모양·Set의 `$append`·빈 연산 객체 — 4.2절).
    InvalidArrayOperation,
    // 배열·Set 필드 자리가 아닌 곳의 `$` 키다.
    MisplacedOperation,
    // 컨테이너(배열 원소·Map 값·Optional) 안의 배열 연산이다(이전 값이 층마다 정해지지 않는다).
    UnsupportedOperation,
    // 표에 없거나 값이 비어 있는 경로 이름 `{...}`이다(4.3절).
    InvalidPathToken,
    // 이 층이 쓸 수 없는 필드다(사용자 층의 UserConfig 아닌 필드, 출시 규칙의 DevelopmentOnly
    // 필드, 프리셋 묶음 펼침만 쓰는 품질 묶음 층에 직접 쓰기 — CVar 11절).
    LayerNotAllowed,
    // `Settings` 섹션의 콘솔 설정 이름·값 모양이 잘못됐다.
    InvalidSetting,
    // 대입 대상 텍스트(`<Branch>.<Section>.<경로>`)가 등록된 섹션을 가리키지 않는다.
    UnknownTarget,
    // 콘솔 층이 ConsoleReadOnly 콘솔 설정을 쓰려 했다(5.1절).
    ConsoleReadOnly,
    // 콘솔 설정 값이 범위·허용 값 밖이다.
    ValueOutOfRange,
    // 정보: 더 강한 출처가 이미 있어 값이 바뀌지 않은 쓰기다(출처 스택에는 남았다 — 3.1절).
    WeakWrite,
    // 경고: 사용자 파일의 스키마 버전(`$version`)이 없거나 지금 버전과 다르다(8절 — 파일은 관대하게
    // 읽는다).
    UserFileVersion,
    // 경고: 사용자 파일의 `Settings`에 등록되지 않은 콘솔 설정 이름이 있다(버렸다).
    UnknownSetting,
    // 정보: 시작 뒤 RestartRequired 값이 바뀌어 다음 실행 때 적용된다(지금 값은 그대로 — 8절, CVar
    // VD15의 `RestartRequired` 진단).
    RestartRequired,
    // 경고: 시작 뒤 파일 층이 아닌 층(기기 프로필·품질 같은 파생 층, 세션·콘솔 같은 휘발 층)이
    // 부팅 값(RestartRequired·StartupOnly)을 바꾸려 했다. 다음 실행에 같은 입력이 다시 오지
    // 않으므로 대기로 두지 않고 지금 값을 유지한다(같은 이름·출처에 한 번만 — 8절).
    BootValueIgnored,
    // 기기 프로필 정의·선택 오류(모르는 프로필·부모, 부모 순환, 모르는 메모리 등급 — 6.2절).
    InvalidDeviceProfile,
    // 정보: `Clamp()` CVar의 범위 밖 값을 범위 끝으로 보정해 받았다(CVar 6.2·12절).
    Clamped,
    // enum CVar 값이 이름 표에 없다(CVar 4.4·12절).
    InvalidEnum,
    // 봉인(CompleteStartup) 뒤 부팅 변수(BootCVar·StartupOnly·RestartRequired 독립 콘솔 설정)에
    // 바로 쓰려 했다(콘솔·코드·범위 임시 변경 층 — CVar 9절). 메시지는 콘솔 안내 문구다("is a boot
    // variable; set it in a config file or with --set and restart" — 이름 뒤에 붙인다).
    BootSealed,
    // 정보: 등록 전에 들어온 값(대기 값)이 등록 때 적용됐다(CVar 12절).
    LateBound,
    // 경고: 시작이 끝났는데 등록되지 않은 콘솔 이름의 값이 남았다(ReportUnresolved — 가까운 이름이
    // 있으면 메시지에 `did you mean '...'?`를 붙인다, CVar 12절).
    UnresolvedName,
    // CVar의 `Validate` 검사 함수가 값을 거부했다(메시지는 검사 함수가 돌려준 이유 —
    // CVar 4.2·12절).
    ValidationFailed,
    // 프리셋 묶음이 정의하지 않은 등급이다(묶음 변수 쓰기, `Quality.json`의 등급 키 — CVar
    // 11·12절).
    UndefinedLevel,
    // 경고: 묶음 펼침이 MaxGroupPasses 번 안에 멈추지 않았다(묶음끼리 서로 바꾸는 순환 — 마지막
    // 결과를 쓴다, CVar 10절).
    GroupNoConverge,
    // 치트 허용 상태(`cheat.enabled`)가 꺼져 있어 `Cheat()` 변수의 콘솔 쓰기·치트 명령을 거부했다
    // (CVar 13.1절 — 받을 때와 Sync 적용 때 판정).
    CheatsDisabled,
    // 클라이언트 역할에서 `Replicated()` 변수의 콘솔 쓰기를 거부했다(서버 값을 따른다 —
    // CVar 13.2절).
    ReplicatedOnClient,
    // 프리셋 묶음 정의끼리 맞지 않는다: 묶음의 묶음 순환·자기 참조(메시지에 순환 경로), 두 묶음이
    // 같은 변수를 항목으로 가짐(두 묶음 이름) — ConfigContext::Register가 거부한다(CVar 11절).
    GroupConflict,
    // 정보: 품질 조합 검증이 묶음의 펼칠 등급을 낮췄거나 가장 낮은 등급도 기기 조건에 맞지 않는다
    // (메시지는 `quality.shadow@1 (requested 3, lowered: <사유>)` — Config 6.1, CVar 11절).
    QualityLowered,
    // 범위 값을 받지 않는 변수(`Replicated()` — `cheat.enabled` 제외, 프리셋 묶음 변수)에 범위
    // 쓰기를 했다(CVar 8절).
    ScopeNotAllowed
};

// 진단 이름(열거자 철자)이다. 정의되지 않은 값이면 빈 view다.
ROCKS_CONFIG_API StringView ConfigDiagnosticCodeName(ConfigDiagnosticCode code) noexcept;

// 진단 심각도다. Error가 하나라도 있으면 그 입력(층 추가·대입)은 받지 않는다.
enum class ConfigSeverity : uint8 {
    Error,
    // 경고 — 입력의 그 조각만 버리거나(오래된 사용자 파일의 모르는 키 — 8절) 값을 바꿔(품질 등급
    // 낮춤) 계속했다. ErrorCount에 들지 않는다.
    Warning,
    // 정보(WeakWrite 등) — 입력은 받았다.
    Note
};

// 진단 모음의 한 시점이다(ConfigDiagnostics::Mark — Rewind로 그 뒤 진단을 버린다).
struct ConfigDiagnosticsMark {
    usize count = 0;
    usize errors = 0;
    uint64 omitted = 0;
    usize textBytes = 0;
};

// 진단 하나의 읽기 view다. source(층 출처 이름 — 파일 경로·`<memory>` 등)·path(섹션 안 필드 경로
// 또는 콘솔 이름)·message는 그것을 돌려준 ConfigDiagnostics의 버퍼를 빌린다(다음 Add·Clear까지).
// position.line이 0이면 위치가 없다(대입·콘솔 입력).
struct ConfigDiagnostic {
    ConfigDiagnosticCode code = ConfigDiagnosticCode::JsonSyntax;
    ConfigSeverity severity = ConfigSeverity::Error;
    // code가 FieldValue일 때 리플렉션 진단 종류다.
    reflection_text::TextDiagnosticCode textCode =
        reflection_text::TextDiagnosticCode::InvalidValue;
    ConfigLayer layer = ConfigLayer::CodeDefault;
    StringView source;
    JsonPosition position{0, 0, 0};
    StringView path;
    StringView message;
};

// 진단 모음 기본 한도다.
inline constexpr uint32 DefaultMaxConfigDiagnostics = 256;

// 설정 진단을 순서대로 모은다. 한도까지 저장하고 넘친 것은 개수만 센다(Omitted). Error와 Note를
// 함께 담는다. 복사·이동하지 않는다. 스레드: 설정 문맥의 소유 스레드에서만 쓴다.
//
// 사용 예:
//   ConfigDiagnostics& diagnostics = context.Diagnostics();
//   for (usize i = 0; i < diagnostics.Count(); ++i) {
//       const ConfigDiagnostic item = diagnostics.Get(i);
//       // item.source:item.position.line:item.position.column ConfigDiagnosticCodeName(item.code)
//   }
class ROCKS_CONFIG_API ConfigDiagnostics {
public:
    // allocator는 텍스트·레코드 버퍼를 할당한다(모음보다 오래 살아야 한다).
    explicit ConfigDiagnostics(Allocator& allocator) noexcept;

    ConfigDiagnostics(const ConfigDiagnostics&) = delete;
    ConfigDiagnostics& operator=(const ConfigDiagnostics&) = delete;

    // 저장 한도를 정한다(1 이상 — 0이면 Fatal). 이미 저장한 진단은 그대로 둔다.
    void SetLimit(uint32 maxDiagnostics) noexcept;

    // 진단 하나를 더한다(텍스트는 복사한다). 한도에 찼으면 저장하지 않고 Omitted를 올린 뒤
    // 성공한다. 오류: code·severity·layer가 정의되지 않은 값이면 InvalidArgument, 할당 실패는
    // OutOfMemory 등.
    Result<void> Add(const ConfigDiagnostic& diagnostic) noexcept;

    usize Count() const noexcept {
        return entries_.Size();
    }

    // index번째 진단이다. 전제: index < Count() — 아니면 Fatal.
    ConfigDiagnostic Get(usize index) const noexcept;

    uint64 Omitted() const noexcept {
        return omitted_;
    }

    // Error 심각도 진단 수다(넘친 것 제외).
    usize ErrorCount() const noexcept {
        return errors_;
    }

    // 저장하지 못하고 넘친 진단 수를 count만큼 더한다(다른 모음의 진단을 옮길 때 그 모음의 넘친
    // 수도 함께 넘긴다 — ConfigContext::TakeDiagnostics).
    void AddOmitted(uint64 count) noexcept {
        omitted_ += count;
    }

    // 모든 진단을 지운다(한도는 그대로).
    void Clear() noexcept;

    // 지금 시점이다. 시험 적용처럼 진단을 남기지 않을 검사를 하기 전에 잡고, 끝나면 Rewind로 그 뒤
    // 진단을 버린다(관대한 사용자 파일 읽기가 조각마다 쓴다).
    ConfigDiagnosticsMark Mark() const noexcept;

    // mark 뒤에 더한 진단(넘친 개수 포함)을 버린다. 전제: mark는 이 모음의 Mark가 돌려준 것이고 그
    // 뒤 Clear하지 않았다 — 아니면 Fatal.
    void Rewind(const ConfigDiagnosticsMark& mark) noexcept;

private:
    struct Entry {
        ConfigDiagnostic head;
        usize textOffset;
        usize sourceBytes;
        usize pathBytes;
        usize messageBytes;
    };

    Array<char> text_;
    Array<Entry> entries_;
    uint64 omitted_ = 0;
    usize errors_ = 0;
    uint32 limit_ = DefaultMaxConfigDiagnostics;
};

} // namespace rocks::config
