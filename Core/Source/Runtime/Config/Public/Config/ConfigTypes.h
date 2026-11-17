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
    // 명령줄 `--set=`·시험 픽스처
    Session,
    // 실행 중 콘솔 입력·코드의 범위 임시 변경
    Console
};

// 층 수다(ConfigLayer::Console + 1).
inline constexpr uint32 ConfigLayerCount = static_cast<uint32>(ConfigLayer::Console) + 1;

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
bool IsConsoleSettingName(StringView name) noexcept;

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
    // 필드).
    LayerNotAllowed,
    // `Settings` 섹션의 콘솔 설정 이름·값 모양이 잘못됐다.
    InvalidSetting,
    // 대입 대상 텍스트(`<Branch>.<Section>.<경로>`)가 등록된 섹션을 가리키지 않는다.
    UnknownTarget,
    // 정보: 더 강한 출처가 이미 있어 값이 바뀌지 않은 쓰기다(출처 스택에는 남았다 — 3.1절).
    WeakWrite
};

// 진단 이름(열거자 철자)이다. 정의되지 않은 값이면 빈 view다.
StringView ConfigDiagnosticCodeName(ConfigDiagnosticCode code) noexcept;

// 진단 심각도다. Error가 하나라도 있으면 그 입력(층 추가·대입)은 받지 않는다.
enum class ConfigSeverity : uint8 {
    Error,
    // 정보(WeakWrite 등) — 입력은 받았다.
    Note
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
class ConfigDiagnostics {
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

    // 모든 진단을 지운다(한도는 그대로).
    void Clear() noexcept;

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
