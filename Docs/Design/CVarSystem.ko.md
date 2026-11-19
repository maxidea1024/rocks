# CVar·치트 명령 설계

기준일: 2026-11-19, 개정 17(결정 확정, CVR1–CVR7 구현). 실행 중 이름으로 읽고 바꾸는 타입 있는 설정 변수 **CVar**와, 개발 빌드에서 게임 상태를 직접 바꾸는 **치트 명령**을 정한다.

이 문서는 [설정 계층과 콘솔 설정](ConfigSystem.ko.md)(이하 Config)의 5절 "콘솔 설정과 명령"을 대체하는 상세 스펙이다. 값 저장소·층·출처 스택·sync 지점·JSON 파일·`--set`·설정 struct는 Config를 그대로 쓰고, 이 문서는 그 위에 다음을 더한다.
- 한 문장으로 정의하고 `.Get()`으로 읽는 타입 있는 핸들(`CVar<T>`·`BootCVar<T>`·`CVarGroup`·`ConsoleCommand`)
- 잠금·할당·문자열 연산이 없는 읽기 경로(번호로 찾는 snapshot)
- 범위(World·플레이어) 계층, 부팅 고정 변수, 코드로 정의하는 프리셋 묶음, 치트·네트워크 권한
- 객체 시스템과의 연결(리플렉션 enum·설정 struct·치트 집합 객체·객체 콘솔)

UE 근거는 UE 5.8.4(`release`, 로컬 클론)의 현재 기본 경로이고 경로는 `Engine/Source/Runtime/` 기준이다. 레거시·deprecated 경로는 보지 않았다.

상태: **설계 확정**(리뷰어 1명 교차 검증 반영). 결정 VD1–VD18 중 VD1·VD2는 사용자 결정(권장안)이고 나머지는 권장안이다(17절). 다른 문서 개정(22절)은 모두 반영했다. 구현 착수 시점은 다른 세션 작업을 본 뒤 사용자가 정한다.

## 1. 한눈에 보기

정의는 사용하는 곳(.cpp) 근처에 한 문장으로 쓰고, 모듈 등록 함수에 한 줄로 올린다. 읽기는 `.Get()`이다.

```cpp
#include <Config/CVar.h>

namespace rocks::render {

// 정의: 이름·기본값·도움말이 필수이고, 나머지는 필요한 것만 잇는다.
CVar<float> ShadowDistance = cvar::Var("render.shadow.distance", 8000.f)
    .Help("Max shadow distance (cm)").Range(500, 20000).Clamp();

CVar<ShadowQuality> ShadowQualityLevel = cvar::Var("render.shadow.quality", ShadowQuality::High)
    .Help("Shadow quality level");

BootCVar<String> GraphicsBackend = cvar::Boot("render.backend", "vulkan")
    .Help("Graphics backend");

CVar<bool> GodMode = cvar::Var("game.god", false).Help("Invulnerable player").Cheat();

// 모듈 등록 함수(모듈마다 이미 있는 자리)에 한 줄.
Result<void> RegisterRenderVars(ConfigContext& config) noexcept {
    return config.Register(ShadowDistance, ShadowQualityLevel, GraphicsBackend, GodMode);
}

// 사용: 어느 lane에서든 같은 모양이다. 잠금·할당·문자열 비교가 없다.
void SetupShadows(const World& world) noexcept {
    const float distance = ShadowDistance.Get();
    const float worldDistance = ShadowDistance.Get(world.CVarScope());
    ...
}

} // namespace rocks::render
```

치트 명령도 같은 모양이다. 인자는 함수 매개변수 타입에서 읽는다.

```cpp
Result<void> Teleport(cvar::CommandContext& context, Vec3 location) noexcept;

ConsoleCommand TeleportCommand = cvar::Command<&Teleport>("cheat.teleport")
    .Help("Teleport the local player").Cheat();
```

콘솔에서는 `render.shadow.distance 3000`, `render.shadow.distance ?`, `cheat.teleport 0 0 100`처럼 쓴다(14절).

## 2. 결정 요약

| 항목 | 결정 | 근거 |
|---|---|---|
| 정의 | 빌더를 이은 한 문장. 이름 문법·도움말 누락·기본값 범위 위반·타입에 맞지 않는 빌더 호출은 **컴파일 오류**다 | UE `TAutoConsoleVariable`(`Core/Public/HAL/IConsoleManager.h:1969`)은 실행 시 등록하고 범위 검사가 없다. 도움말 검사 함수(`Core/Private/HAL/ConsoleManager.cpp:114`)는 있으나 강제가 주석 처리돼 있다(`:222`). Rocks LogCategory가 같은 consteval 검사를 쓴다(`Core/Public/Core/Diagnostics/LogCategory.h:128–136`) |
| 정적 초기화 | 핸들은 상수 초기화되는 값이다. 전역 생성자가 없고 초기화 순서 문제가 없다 | UE는 전역 생성자로 등록한다. Rocks는 전역 생성자를 금지한다(Config 5.1, Trace T3) |
| 등록 | 모듈 등록 함수에서 `ConfigContext::Register(핸들…)`로 올린다. 빠뜨리면 Policy가 빌드에서 잡는다(VD1, 사용자 확인) | TraceChannel과 같은 방식(`TraceCapture/Public/TraceCapture/TraceChannel.h:19–25`) |
| 읽기 | `Get()`은 이 스레드에 묶인 불변 snapshot의 번호 칸을 읽는다. 잠금·할당·문자열 연산이 없다 | UE는 렌더 스레드 사본을 주소에 쓴다(`IConsoleManager.h:1898–1920`). Config 5.3의 불변 snapshot을 따른다 |
| 쓰기 | 어느 스레드에서나 부를 수 있다. 검증은 즉시, 적용은 다음 sync 지점이다 | Config 5.3을 확장(이전에는 게임 lane만 쓰기 가능) |
| 우선순위 | Config 층 순서에 Hotfix·Code·Server·Scoped 층을 더한 하나의 순서(6절) | UE SetBy 16단계(`IConsoleManager.h:152–187`) |
| 범위 | Global → World → 플레이어·Viewport. 자식 범위 값이 층과 무관하게 이긴다. 범위는 소유 객체와 수명을 같이한다 | UE에는 없다(전역 값 하나) |
| 부팅 고정 | `BootCVar<T>`는 `Get()`만 있다. `ConfigContext::CompleteStartup` 뒤에는 이번 실행 값이 바뀌지 않는다. 직접 쓰기는 거부하고, 파일 층(사용자가 다음 실행용으로 고르는 `UserSetting()` 값 포함) 변경은 재시작 대기로, 파생·휘발 층 변경은 무시로 처리한다(9절) | Config `StartupOnly`·`RestartRequired` 플래그를 타입 하나로 올렸다 |
| 프리셋 | `CVarGroup`을 코드에서 정의하고 Quality 층에 펼친다. 구성원 여부는 항목에서 자동으로 정해진다 | UE scalability group 적용(`Engine/Private/Scalability.cpp:412–446` `SetGroupQualityLevel`) |
| 치트 | `Cheat()` 변수·명령은 출시 빌드에서 등록되지 않고 값은 기본값 상수다. 개발 빌드에서는 치트가 꺼진 동안 콘솔 쓰기를 거부한다 | UE `ECVF_Cheat`(`IConsoleManager.h:72–76`), `UE_WITH_CHEAT_MANAGER`(`Engine/Classes/GameFramework/CheatManagerDefines.h:9`), `AGameModeBase::AllowCheats`(`GameModeBase.h:479`) |
| 치트 명령 | 지금은 함수 포인터 빌더, 함수 리플렉션(CU2) 뒤에는 `CheatSet` 객체의 리플렉션 함수다 | UE `UCheatManager`의 `UFUNCTION(exec)`와 확장(`CheatManager.h:69–103`, `Engine/Private/CheatManager.cpp:93–147`) |
| 매크로 | CVar 시스템은 새 매크로를 정의하지 않는다. 등록·enum 이름·시험 보조 모두 언어 기능이다 | 사용자 요구 |

## 3. 용어 대응

CVar는 Quake 이래 업계 공용어이므로 Rocks 이름으로 쓴다. 나머지 UE 이름은 이 표에만 둔다.

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| CVar `CVar<T>` | `TAutoConsoleVariable<T>` | 실행 중 바꿀 수 있는 타입 있는 설정 변수 |
| 부팅 고정 변수 `BootCVar<T>` | `ECVF_ReadOnly` + 시작 시 값 | 이번 실행 동안 바뀌지 않는 변수 |
| 프리셋 묶음 `CVarGroup` | scalability group(`sg.*`) | 등급 하나가 여러 변수 값을 정하는 묶음(Config 품질 묶음) |
| 콘솔 명령 `ConsoleCommand` | `FAutoConsoleCommand` | 이름으로 부르는 동작 |
| 치트 집합 `CheatSet` | `UCheatManager`·`UCheatManagerExtension` | 치트 함수를 가진 객체 |
| 범위 `CVarScope` | — | 값을 덮어쓰는 단위(World·플레이어) |
| 설정 문맥 `ConfigContext` | `IConsoleManager` | 값·층·snapshot의 소유자(Config 2절) |
| 설정 snapshot `ConfigSnapshot` | 렌더 스레드 사본 | sync 지점에 게시하는 불변 값 블록 |
| 감시자 `cvar::Watcher<T>` | `FConsoleVariableSinkHandle` | 값 세대가 바뀌었는지 묻는 객체 |
| 범위 임시 변경 `cvar::ScopedOverride<T>` | `TGuardConsoleVariable` | 범위가 끝나면 되돌리는 쓰기 |

## 4. 정의 API

공개 헤더는 `Config/CVar.h` 하나다(Config 모듈). 공개 이름은 namespace `rocks::cvar` 안에 둔다. 핸들 타입(`CVar`·`BootCVar`·`CVarGroup`·`ConsoleCommand`)은 `rocks` 바로 아래에도 using으로 둔다(정의 문장을 짧게 하기 위해서다).

### 4.1 지원 타입

| `T` | 기본값 철자 | `Get()` 반환 | 저장 |
|---|---|---|---|
| `bool` | `true` | `bool` | 1 byte |
| `int32`·`int64` | `512`·`int64{1} << 40` | 값 | 4·8 byte |
| `float`·`double` | `8000.f`·`1.0` | 값 | 4·8 byte |
| enum class | `ShadowQuality::High` | enum | 기저 정수 |
| `String` | `"vulkan"` | `StringView`(7.1절 수명) | 불변 문자열 블록 참조 |
| `Name` | `cvar::Var<Name>("x", "Default")` | `Name` | Name 토큰 |

`T`는 기본값에서 추론한다. 문자열 리터럴은 전용 overload `Var(StringView, const char (&)[N])`가 받아 `String` 변수를 만든다(일반 템플릿은 `const char*`로 추론하므로 따로 둔다). `Name`은 pool 토큰이라 상수로 만들 수 없으므로, 핸들은 기본값을 `StringView`로 담고 `Register`가 문맥의 NamePool에 intern한다. 다른 타입은 `static_assert`로 거부한다.

### 4.2 빌더

```cpp
namespace rocks::cvar {

// 진입점이다. T는 기본값에서 추론한다.
template <class T> constexpr VarBuilder<T> Var(StringView name, T defaultValue) noexcept;
template <class T> constexpr BootVarBuilder<T> Boot(StringView name, T defaultValue) noexcept;
constexpr GroupBuilder Group(StringView name, int32 defaultLevel) noexcept;
// 명령 함수는 템플릿 인자다(아래 4.2절 끝 — 함수 포인터 값은 상수 평가에서 타입을 지울 수 없다).
template <auto Function>
constexpr CommandBuilder<Function> Command(StringView name) noexcept;

template <class T>
class VarBuilder {
public:
    VarBuilder(const VarBuilder&) = delete;
    VarBuilder(VarBuilder&&) = delete;

    // 공통
    constexpr VarBuilder&& Help(StringView text) && noexcept;          // 필수
    constexpr VarBuilder&& Category(StringView category) && noexcept;  // cvar.find 분류
    // ValidateArg<T>는 String·Name이면 StringView, 그 밖에는 const T&다.
    constexpr VarBuilder&& Validate(StringView (*check)(ValidateArg<T>) noexcept) && noexcept;
    // 수(int32·int64·float·double)만
    constexpr VarBuilder&& Range(TypeIdentityT<T> minimum, TypeIdentityT<T> maximum) && noexcept;
    constexpr VarBuilder&& Clamp() && noexcept;                        // Range가 있어야 한다
    // 플래그
    constexpr VarBuilder&& Cheat() && noexcept;
    constexpr VarBuilder&& Replicated() && noexcept;
    constexpr VarBuilder&& UserSetting() && noexcept;                  // 사용자 층에 저장 가능
    constexpr VarBuilder&& ConsoleReadOnly() && noexcept;              // 콘솔 층만 거부

    // 끝에서 CVar<T>로 바뀐다. consteval이라 모든 검사가 컴파일 때 끝난다.
    consteval operator CVar<T>() && noexcept;
};

} // namespace rocks::cvar
```

- `Validate`는 갈무리 없는 람다(함수 포인터)다. 통과면 빈 `StringView`, 위반이면 이유(영어 리터럴)를 돌려준다. 검증 함수는 순수해야 하고(입력 값만 본다) 상수 평가가 가능해야 한다. 기본값도 이 함수로 컴파일 때 검사한다.
- `float`·`double` 값은 NaN·무한대를 늘 거부한다(`Range`·`Clamp`와 무관 — 보정하지 않는다). 컴파일 때 기본값(`CVarError_DefaultOutOfRange`)과 실행 때 `Set`·층 입력(`ValueOutOfRange`) 모두 같은 검사 함수다.
- `String`·`Name` 변수의 검증 함수는 `StringView`를 받는다(`StringView (*)(StringView) noexcept`). 핸들이 기본값을 `StringView` 상수로 담고, `String`은 할당이 필요해 상수 평가로 만들 수 없기 때문이다. 실행 중 쓰기도 값 텍스트를 `StringView`로 넘겨 검사한다. 예: 로깅의 `log.levels`는 `.Validate(CheckLogLevelsSyntax)`이고 문법만 본다(Logging L18).
  ```cpp
  CVar<int32> Samples = cvar::Var("render.aa.samples", 4).Help("MSAA sample count")
      .Validate([](const int32& value) noexcept -> StringView {
          return (value & (value - 1)) == 0 ? StringView{} : "must be a power of two";
      });
  ```
- `BootVarBuilder<T>`는 `Help`·`Category`·`Range`·`Validate`·`UserSetting`만 가진다. `Clamp`·`Cheat`·`Replicated`가 없으므로 부르면 컴파일 오류다. "바꾸면 재시작해야 적용"인 값은 `BootCVar` + `UserSetting()`이다(9절 — 따로 `RequiresRestart` 플래그를 두지 않는다).
- **핸들은 정적 저장 기간이어야 한다**(namespace 범위나 static 멤버). 핸들 타입은 `operator new`·`new[]`(placement 포함)를 지워 힙 생성이 컴파일 오류다. 함수 안 지역 핸들도 컴파일은 되므로 `Register`가 Debug·Development 빌드에서 핸들 주소가 호출 스레드 스택 범위 안이면 `InvalidArgument`로 거부한다. Policy `cvar-registration`은 함수 본문 안의 핸들 정의, 비정적 멤버 핸들, `thread_local` 핸들, `const`·`constexpr` 핸들(내부 링크라 헤더에 두면 TU마다 별개 핸들이 된다), 핸들 타입 별칭(`using`·`typedef` — 정의를 lexical하게 따라갈 수 없다)을 거부한다.
- **정의 텍스트 규칙**(컴파일 때 검사): 이름은 소문자 점 경로이고 `MaxVarNameBytes`(128) 이하, 도움말·분류·String 기본값은 NUL 없는 유효 UTF-8이고 각 한도(`MaxVarHelpBytes` 1024·`MaxVarCategoryBytes` 64·`MaxVarDefaultTextBytes` 4096) 이하다. Name 기본값은 출력 가능한 ASCII(0x20–0x7E)이고 1,023 byte 이하다. NFC 판정(consteval에서 할 수 없다)을 ASCII 한정으로 대신한다 — 이름과 Name 기본값은 ASCII라 늘 NFC다. 위반은 `CVarError_TextTooLong`·`CVarError_InvalidUtf8`·`CVarError_NameDefaultNotAscii`다.
- `&&` 체인과 복사·이동 삭제는 `auto b = cvar::Var(...).Help(...)`를 막는다. 진입점 하나만 받는 `auto b = cvar::Var(...)`는 prvalue 생략으로 컴파일되지만, 빌더에는 `Get`·`Set`이 없고 `Register`는 핸들만 받으므로 쓰는 순간 오류다.
- `GroupBuilder`는 11절이다. `CommandBuilder<Function>`은 `Help`(필수)·`Category`·`Cheat`를 가진다. 명령 함수는 `Result<void> (*)(cvar::CommandContext&, Args...) noexcept`이고 `Args`는 bool·int32·int64·float·double·`StringView`·이름 표가 있는 enum이다(`MaxCommandArguments` 8개까지). 함수를 템플릿 인자(`cvar::Command<&Teleport>("cheat.teleport")`)로 받는 까닭: `ConsoleCommand`는 상수 초기화되는 타입 하나여야 하는데, 함수 포인터 값을 실행 시 인자로 받으면 그 서명을 지우는 변환(`reinterpret_cast`)을 상수 평가에서 할 수 없다. 템플릿 인자면 서명대로 만든 호출 함수(`InvokeCommand<&Teleport>`)의 포인터 하나를 담으면 된다. 지원하지 않는 매개변수 타입은 `static_assert`(`Command parameters must be bool, int32, int64, float, double, StringView or a named enum class`)다. 리플렉션 struct·수학 타입 인자는 함수 리플렉션(CU2)과 함께 더한다(20절 CVR6 메모).

### 4.3 컴파일 때 잡는 오류

| 경우 | 방법 | 메시지(영어) |
|---|---|---|
| bool·enum·String·Name에 `Range`·`Clamp` | `static_assert` | `Range() is only available for numeric CVars (int32, int64, float, double)` |
| 지원하지 않는 `T` | `static_assert` | `CVar type must be bool, int32, int64, float, double, String, Name or an enum class` |
| 빌더를 `auto`로 받음 | 복사·이동 삭제(체인은 `VarBuilder&&` 반환) | 삭제된 생성자 진단 |
| 빌더에 `Get`·`Set` | 멤버가 없다 | 멤버 없음 진단 |
| 도움말 없음, 이름 문법 위반(Config D7), `Range` 최소 > 최대, 기본값이 범위·enum 표·`Validate` 위반, `Range` 없는 `Clamp` | consteval 변환 안에서 constexpr이 아닌 표지 함수(`CVarError_MissingHelp` 등)를 부른다 | 컴파일러 진단에 표지 함수 이름이 나온다(LogCategory와 같은 방식) |
| `CVar<int32> X = cvar::Var("x", 1.f)` | 변환 연산자가 `CVar<T>`만 만든다 | `CVar type must match the default value type` |
| enum에 이름 표 없음, 표 크기와 `Count` 불일치 | `static_assert`(4.4절) | 4.4절 |
| `BootCVar`에 `Set`·`Clear`, 프리셋 항목·`ScopedOverride`에 `BootCVar` | 멤버 없음·삭제된 overload + `static_assert` | `BootCVar is fixed at boot; set it in a config file or with --set and restart` |

### 4.4 enum

리플렉션 enum(`ROCKS_ENUM`)은 아무것도 더 쓰지 않는다. 이름은 생성 메타데이터에서 온다(14.1절). 리플렉션 밖 enum(.cpp 안의 enum 등)은 같은 namespace에 `CVarEnumNames`를 둔다. ADL로 찾는다.

```cpp
enum class DebugView : int32 { None, Normals, Depth, Count };

constexpr Span<const StringView> CVarEnumNames(DebugView) noexcept {
    static constexpr StringView names[] = {"None", "Normals", "Depth"};
    return names;
}

// 값을 직접 정한 enum은 이름·값 쌍이다.
enum class Tier : int32 { Low = 10, High = 20 };

constexpr Span<const cvar::EnumEntry> CVarEnumNames(Tier) noexcept {
    static constexpr cvar::EnumEntry entries[] = {{"Low", 10}, {"High", 20}};
    return entries;
}
```

- 찾는 순서: 리플렉션 enum → `CVarEnumNames(E{})`(requires 식으로 탐지). 둘 다 없으면 `static_assert`: `enum CVar needs names: mark the enum ROCKS_ENUM() or define constexpr CVarEnumNames(E) in the enum's namespace`.
- enum에 `Count` 열거자가 있으면 표 크기와 같아야 한다(`static_assert`: `CVarEnumNames(E) size must equal E::Count`). `Count`는 값이 아니다.
- 저장은 기저 정수, `Get()`은 `E`다. 입력은 이름(ASCII 대소문자 무시) 또는 수이고, 표시는 `3 (High)`다. 표에 없는 값은 거부한다.

## 5. 등록

- `ConfigContext::Register(handles…)`는 CVar·BootCVar·CVarGroup·ConsoleCommand를 섞어 받는다. 모듈 등록 함수(`RegisterLogSettings`·`RegisterRenderVars` 같은 자리)에서 부른다. 게임 lane 전용이다.
- 핸들 번호는 프로세스 전체에서 한 번 정해진다. 처음 등록될 때 원자적 카운터로 받고 핸들에 적는다. 그래서 시험이 문맥을 여러 개 만들어도 같은 핸들은 같은 번호이고, 문맥마다 그 번호의 값 칸을 가진다. 번호는 실행 중에만 쓰고 출력·wire에는 이름을 쓴다(덤프는 이름 순서다).
- 같은 이름·같은 타입·같은 핸들을 다시 등록하면 성공한다(멱등). 같은 이름을 다른 핸들이 등록하면 같은 타입이어도 `AlreadyExists`, 다른 타입이면 `TypeMismatch`다. 호스트는 이 둘을 부트 실패로 다룬다.
- 등록은 바로 보인다. `Register`는 새 칸만 더한 snapshot을 그 자리에서 게시하고 게임 lane을 다시 묶는다(기존 칸의 값은 바꾸지 않으므로 "값은 sync 지점에서만 바뀐다"를 깨지 않는다). 대기 값(12절)도 이때 적용한다. 그래서 늦게 로드된 플러그인이 등록 직후 같은 프레임에 `Get()`해도 된다. 등록 전에 만든 snapshot(렌더 lane이 처리 중인 이전 프레임 packet 등)에는 그 칸이 없고, 그때 `Get()`은 핸들의 기본값을 돌려준다.
- 번호가 없는 핸들의 `Get()`:
  - 게임 lane에서는 Fatal이다(`CVar 'render.shadow.distance' is not registered; pass it to ConfigContext::Register in its module`). `Register`가 게임 lane에서 일어나므로, 게임 lane에서 번호가 없으면 정말로 등록을 빠뜨린 것이다.
  - 다른 스레드에서는 기본값이다. 등록이 그 스레드의 snapshot보다 나중이면 번호가 이미 보이든 안 보이든 그 snapshot에는 칸이 없으므로, 어느 쪽을 읽어도 같은 결과(기본값)가 나온다. 경쟁의 결과가 값에 드러나지 않는다.
  - 등록 누락은 Policy가 빌드에서 먼저 잡는다.
- **Policy `cvar-registration`**: 모듈 소스의 namespace 범위 `CVar<`·`BootCVar<`·`CVarGroup`·`ConsoleCommand` 정의 이름이 같은 모듈의 `Register(` 인자에 나오는지 lexical하게 검사한다. 빠뜨린 등록은 빌드(policy)에서 실패한다.
- 이름이 런타임에 정해지는 변수(Sparks 스크립트·플러그인 데이터)는 `RegisterDynamic(definition)`으로 등록한다(CVR7). 정의는 `DynamicVarDefinition`(이름·도움말·분류·타입·enum 타입 ID·기본값 표기·`cvar::VarFlags` 성질 비트·부팅 여부·닫힌 범위)이고, 문맥이 정의를 복사해 핸들 정의와 같은 모양(`VarDefinition`)으로 쥔다. 번호는 핸들과 같은 프로세스 카운터에서 받고 값은 같은 조각 칸에 든다(층·대기 값·콘솔·dump·권한이 핸들과 같다). 정의는 문맥이 소유하므로 번호는 등록한 문맥 안에서만 뜻이 있다. 반환은 `cvar::Index`이고, 읽기는 `cvar::ReadDynamic<T>(snapshot, index, fallback)`이다(T는 핸들과 같은 타입, enum은 `int64` — 칸이 없으면 fallback(등록 전 snapshot), 봉인 뒤 snapshot의 부팅 변수는 부팅 블록). 칸은 값 종류(`ValueKind` 1 byte)를 함께 담고, `ReadDynamic`은 모든 빌드에서 T와 칸의 종류가 맞는지 보고 다르면 fallback을 돌려준다(텍스트 칸을 수로 읽거나 float 칸을 int로 읽어도 null 역참조·쓰기 값 해석이 없다). 실행 중 타입으로 등록하는 쪽(Sparks)은 `cvar::DynamicKind(snapshot, index)`로 칸의 종류를 먼저 묻는다(칸이 없으면 `ValueKind::Opaque`). 게임 lane의 도구용 이름 조회는 `FindConsoleSetting`·`ConsoleValue`다. 닫힌 값 집합은 Enum 타입(리플렉션 enum)이고, CF2의 문자열 허용 값 목록(`allowedValues`·`allowedValuesIgnoreCase`)은 없앴다. 규칙 위반(이름 문법·영역·빈 도움말·모르는 성질 비트·`Clamp`·수가 아닌 범위·유한하지 않은 범위·정수 타입의 정수가 아닌 범위 끝 또는 그 타입의 표현 범위를 넘는 범위 끝·enum 타입 불일치·기본값 거부)은 `InvalidArgument`, 이름 중복은 핸들과 같다(같은 타입 `AlreadyExists`·다른 타입 `TypeMismatch`). 출시 빌드의 `Cheat` 동적 변수는 등록하지 않고 `Unsupported`다.

## 6. 층과 쓰기

### 6.1 층 순서

Config 3.1절 순서에 네 층을 더한다. 약한 것부터 강한 것이다.

| 순위 | 층 | 출처 | 변경 |
|---|---|---|---|
| 0 | 코드 기본값 | 정의의 기본값 | |
| 1–4 | 엔진·엔진 플랫폼·프로젝트·프로젝트 플랫폼 | Config 파일 | |
| 5 | 기능 묶음 | 플러그인·게임 기능 | |
| 6 | 품질 묶음 | `CVarGroup` 등급(11절) | 프로젝트 파일보다 강하다(17절 VD4) |
| 7 | 기기 프로필 | Config 6.2절 | |
| 8 | 사용자 | 사용자 파일(`UserSetting()` 변수만) | |
| 9 | **Hotfix** | 라이브 운영 핫픽스 | 신규. UE `ECVF_SetByHotfix`(`IConsoleManager.h:177`) |
| 10 | 세션 | 명령줄 `--set=` | |
| 11 | **Code** | 코드의 `Set()` | 신규. UE `ECVF_SetByCode`(`:183`) |
| 12 | **Scoped** | `ScopedOverride`·시험 | 신규. Config의 "콘솔 층 + 태그" 방식을 대신한다. UE `ECVF_SetByTemp`(`:185`)처럼 콘솔보다 약하다 |
| 13 | 콘솔 | 콘솔 입력 | |
| 14 | **Server** | 서버가 복제한 값(13.2절) | 신규. 가장 강하다. 클라이언트의 콘솔·코드·임시 변경이 복제 값을 이기지 못한다 |

- Config 규칙을 그대로 쓴다: 층마다 `{값, 출처}`를 모두 보관하고, 값이 있는 가장 강한 층이 유효값이다. 약한 층 쓰기도 기록한다. 층을 지우면 아래 값이 드러난다.
- 출처는 `파일:줄:열`(Config 파일), `--set`, `console`, `quality.shadow@2`(프리셋), `hotfix <태그>`, 코드 호출 위치(`ShadowSystem.cpp:120`)다.
- `ClearLayer(ConfigLayer::Hotfix)` 하나로 핫픽스 전체를 되돌린다(태그별로는 Config `Unset(층, 태그)`).

### 6.2 쓰기 API

```cpp
// 코드 층에 쓴다. 출처는 호출 위치다. 검증은 지금, 적용은 다음 Sync다.
Result<void> r = ShadowDistance.Set(4000.f);
// 층·범위를 고른다.
ShadowDistance.Set(4000.f, {.layer = ConfigLayer::Console, .scope = world.CVarScope()});
ShadowDistance.Clear();                         // 코드 층 값을 지운다
config.ClearLayer(ConfigLayer::Hotfix);
```

- `Set(value, SetOptions = {}, SourceLocation = SourceLocation::Current())`, `Clear(SetOptions = {})`. `SetOptions`는 `layer`(기본 Code)·`scope`(기본 Global)·`tag`다.
- 어느 스레드에서나 부를 수 있다. 범위·enum·`Validate`·층 권한(13절)을 바로 검사해 `Result`로 돌려주고, 통과한 쓰기는 문맥의 쓰기 큐(MPSC)에 넣는다. `Clamp()` 변수는 범위 밖 값을 보정해 받고 적용 때 `Clamped` 진단을 남긴다.
- 반환은 `Result<void>`의 `ErrorCode`다: 이 스레드에 묶인 문맥이 없으면 `NotFound`, 검사 위반은 `InvalidArgument`, 다른 스레드의 쓰기가 큐 한도(`MaxQueuedChanges`)를 넘으면 `Busy`(버리지 않는다 — 소유 스레드 입력은 한도와 무관하다), 문맥이 없어졌으면 `ShuttingDown`. 거부 이유는 `SetOptions::reason`(`ConfigDiagnosticCode*`)으로 받는다(`ValueOutOfRange`·`InvalidEnum`·`ValidationFailed`·`ConsoleReadOnly`·`LayerNotAllowed`, 뒤 단계의 `CheatsDisabled`·`ReplicatedOnClient`·`BootSealed`). 통과·보정이면 그대로 둔다. 콘솔 도구(CVR6)가 이것으로 메시지를 만든다.
- `scope`를 주면 그 범위의 값 하나를 바꾼다(8절). 층 스택에는 들어가지 않으므로 `layer`는 우선순위에 쓰이지 않고, 그 범위 값의 **출처 층**(권한 판정·Explain 표시)으로만 남는다. 층 권한(콘솔 읽기 전용·사용자 층 등)은 Global 쓰기와 같이 그 출처 층으로 본다. 범위 값을 받지 않는 변수(`Replicated()` — `cheat.enabled`는 예외, `CVarGroup` 묶음 변수)는 넣을 때 `ScopeNotAllowed`로 거부한다(`BootCVar`는 `Set`이 없다). 범위가 없거나(파괴됐거나 다른 문맥의 범위) 이름이 그 문맥에 등록되지 않았으면 Sync 적용 때 그 쓰기를 버린다(넣을 때는 범위 표를 보지 않는다 — 다른 스레드는 게임 lane의 범위 표를 읽지 않는다, 10.1절). `Clear({.scope = scope})`는 그 범위의 그 변수 값을 지운다. 의미는 Global 층 지우기와 맞춘다: 저장된 범위 값의 출처 층보다 약한 층에서 온 지우기는 무시한다(약한 층이 강한 층의 값을 지우지 못한다). 콘솔에서 온 지우기는 콘솔 쓰기와 같은 권한(콘솔 읽기 전용·치트)을 받을 때와 적용 때 모두 본다.
- 권한(치트·복제·부팅 봉인)은 지금 한 번 검사해 바로 알려 주고, Sync가 적용할 때 다시 검사한다. 그 사이 상태가 바뀌었으면 적용 때의 판정이 이기고 진단을 남긴다.
- 어느 문맥에 쓰는지: 이 스레드에 묶인 문맥(7.1절), 없으면 프로세스 기본 문맥이다. 기본 문맥은 `ConfigContext::MakeProcessDefault()`(게임 lane)로 정하고 문맥이 없어질 때 풀린다. 프로세스에 하나이고, 다른 문맥이 이미 기본이면 `AlreadyExists`다(같은 문맥의 재호출은 성공). `HostSettings::Initialize`가 자기 문맥을 기본으로 정한다(CVR7). 기본 문맥을 거치는 쓰기는 기본 문맥 잠금 안에서 통로 참조를 얻으며 그 통로의 원자 계수(`defaultWriters`)를 하나 올리고, 큐에 넣은 뒤 참조를 놓고 계수를 내린다. 문맥 파괴는 통로를 닫고(`open = false`) 기본 문맥을 푼 다음 이 계수가 0이 될 때까지 통로 잠금의 조건 변수로 기다린다(sleep 없음). 그래서 통로의 마지막 해제는 소유 스레드에서 일어나고(문맥 allocator를 다른 스레드가 해제하지 않는다), 결과는 정해진다: 파괴와 겹친 쓰기(통로를 얻은 뒤 닫힘)는 `ShuttingDown`, 기본 문맥이 없을 때(정한 적이 없거나 풀린 뒤) 들어온 묶음 없는 쓰기는 `NotFound`다. 풀린 뒤를 `ShuttingDown`으로 나누지 않는 까닭: 그러려면 "한 번 정한 적 있음"을 프로세스 전역에 끝까지 남겨야 하는데, 그러면 같은 프로세스의 뒤 문맥(시험·도구가 문맥을 다시 만드는 경우)의 결과가 앞에 무엇이 있었는지에 따라 달라진다(리뷰 결정과 다른 점 — 20절 CVR7 리뷰 반영). `ScopedOverride`는 끝낼 때 지우기를 넣으려고 통로 참조를 범위 동안 쥐므로, 기본 문맥을 거친 범위도 `ConsoleInputPort`와 같이 문맥 allocator보다 먼저 끝나야 한다. 기본 문맥은 쓰기(`Set`·`Clear`·`ScopedOverride`)에만 쓰고 읽기(`Get`)는 늘 묶음이다(7.1절 — 묶음 없는 `Get`은 Fatal). 시험은 문맥을 스레드에 묶는다.
- `ConsoleReadOnly()`는 콘솔 층만 거부한다. `UserSetting()`이 아닌 변수는 사용자 층을 거부한다(Config와 같다).

### 6.3 범위 임시 변경

```cpp
{
    cvar::ScopedOverride shadows{ShadowDistance, 500.f};
    config.Sync();
    CHECK(ShadowDistance.Get() == 500.f);
} // 소멸: Scoped 층의 그 태그를 지운다(다음 Sync에 이전 값이 드러난다)
```

- Scoped 층에 고유 태그로 쓰고 소멸 때 그 태그만 지운다. 중첩하면 나중 것이 이기고, 어떤 순서로 풀어도 남은 것 중 가장 나중 것이 이긴다(태그 스택).
- 값은 만들 때 이 스레드에 묶인 문맥에 쓰고, 지우기는 그 문맥(쓰기 통로)에 간다 — 소멸 때 묶인 문맥이나 스레드와 무관하다. 지우기 쓰기는 만들 때 미리 만들고 큐 자리도 잡아 두므로 소멸에 실패 경로가 없다. 그 문맥이 먼저 없어졌으면 소멸은 할 일이 없다.
- 적용은 다른 쓰기와 같이 Sync에서 한다(값은 sync 지점에서만 바뀐다). `BootCVar`는 받지 않는다(컴파일 오류).
- 프리셋 묶음(`CVarGroup`)도 받는다(CVR3 — `cvar::ScopedOverride levels{ShadowQuality, 1}`): 묶음 변수(int32 등급)의 Scoped 층에 쓰므로 그 동안 묶음의 유효 등급이 바뀌고 다음 Sync의 묶음 펼침이 그 등급을 펼친다. 정의되지 않은 등급은 `InvalidArgument`(`UndefinedLevel`)다.

## 7. 읽기와 snapshot

### 7.1 `Get()`

```cpp
T   Get() const noexcept;                          // 이 스레드에 묶인 snapshot의 Global 값
T   Get(CVarScopeId scope) const noexcept;         // 범위 사슬을 자식 → 부모로
cvar::Index Index() const noexcept;
```

- 다른 snapshot을 읽어야 하면 `cvar::SnapshotBinding`으로 묶은 뒤 `Get()`을 부른다(overload를 늘리지 않는다).
- `Get(scope)`(CVR3)은 묶인 snapshot의 범위 표에서 범위 사슬(자식 → 부모)을 따라 처음 만난 범위 값을 돌려주고, 없으면 `Get()`(Global 유효값)이다. Global 범위(`CVarScopeId{}`)면 `Get()`과 같다. 그 snapshot에 없는 범위(아직 게시되지 않았거나 파괴된 범위, 다른 문맥의 범위)도 Global 값이다(assert 없음 — 렌더 lane이 이전 프레임을 처리하는 것은 정상이다). 어느 스레드에서나 부른다(묶음 규칙은 `Get()`과 같다). `BootCVar`에는 없다.
- 읽기 경로: thread_local 묶음 → snapshot의 조각 표 → 조각 → 칸(의존 load 4개). 번호는 핸들에 있으므로 이름 탐색이 없다. 잠금·할당·문자열 연산이 없다. 분기는 "묶음 없음(Fatal)", "번호 없음 또는 이 snapshot에 칸 없음(5절)" 둘이고 모두 거의 늘 같은 쪽으로 간다.
- **스레드 묶음.** 게임 lane은 Sync가 새 snapshot을 게시할 때 스스로 다시 묶는다. 렌더 lane은 프레임 packet을 처리하기 시작할 때 packet의 snapshot을 묶는다(RenderBase 5.3·12절). worker 작업은 Task 시스템의 범용 작업 문맥 슬롯(Task는 Config에 의존하지 않는다 — 슬롯은 참조 계수 있는 불투명 객체 하나)으로, 만든 lane의 snapshot **참조를 하나 쥐고** 넘겨받는다. 작업이 프레임보다 오래 살아도 snapshot이 먼저 사라지지 않는다(작업 생성마다 원자 증감 한 쌍 — 작업은 그 정도로 굵다). 그 밖의 스레드는 `cvar::SnapshotBinding bind{config.Acquire()};`로 직접 묶는다. 묶음 없이 `Get()`을 부르면 Fatal이다(조용히 최신 값을 읽지 않는다 — 프레임 일관성이 깨지기 때문이다).
- 번호는 프로세스 전체이므로 다른 문맥에만 등록된 핸들은 게임 lane에서도 Fatal이 아니라 기본값이다(이 문맥 snapshot에 칸이 없다). Fatal은 어느 문맥에도 등록된 적 없는 핸들(번호 없음)이다.
- 한 프레임 동안 같은 스레드는 같은 snapshot을 본다. 그래서 같은 Sync에서 바뀐 변수 둘을 일부만 새 값으로 읽는 일이 없다.
- **`String` 값의 수명.** `Get()`이 돌려준 `StringView`는 묶인 snapshot이 바뀔 때까지만 유효하다. 게임 lane은 다음 Sync(또는 `Register`)까지, 렌더 lane은 그 프레임 packet 동안이다. 더 오래 쓰려면 복사한다.
- `Cheat()` 변수는 출시 빌드에서 `if constexpr (IsReleaseBuild)` 분기로 기본값을 바로 돌려준다(13.1절).

### 7.2 snapshot 구조

- 값은 번호 순서의 **조각**(`ChunkSlotCount` = 256칸)에 있다. 칸은 값(최대 8 byte, 문자열은 불변 블록 참조)과 세대(uint32)다. snapshot은 조각 포인터 표, 범위 표, 부팅 값 블록, 세대 번호를 가진다.
- Sync는 바뀐 칸이 있는 조각만 복사하고(copy-on-write), 나머지 조각은 참조 계수로 공유한다. 바뀐 것이 없으면 snapshot을 새로 만들지 않는다. 변수가 1만 개를 넘어도 한 프레임의 복사는 바뀐 조각 수에 비례한다.
- 게시는 문맥의 현재 snapshot 포인터를 원자적으로 바꾸는 것이다. 신규 `Acquire()`는 어느 스레드에서나 부를 수 있고 참조 계수를 올린 `SharedPtr<const ConfigSnapshot>`을 돌려준다(짧은 잠금 허용 — 읽기 경로가 아니다). 지금의 `ConfigContext::Snapshot()`은 소유 스레드 전용이므로 `Acquire()`로 바꾼다.
- 설정 struct 필드의 snapshot(Config 5.3 `RenderRead` struct 블록)은 같은 snapshot 안에 그대로 둔다.

### 7.3 변경 감지

- 칸의 세대는 유효값이 바뀔 때(어느 범위에서 바뀌었든) 1 늘어난다. 처음 값의 세대는 1이다.
- `cvar::Watcher<T>`: 마지막으로 본 세대를 기억한다. `Changed()`(묶인 snapshot)·`Changed(snapshot)`은 세대가 다르면 true이고 본 세대를 갱신한다. 첫 호출은 true다.
  ```cpp
  cvar::Watcher<float> distanceWatcher{ShadowDistance};
  if (distanceWatcher.Changed()) { RebuildCascades(ShadowDistance.Get()); }
  ```
- `ConfigContext::OnSynced`(Core `MulticastDelegate<void(const SyncChanges&)>`)는 Sync마다 한 번, 값이 바뀐 번호 목록(이름 순서)과 바뀐 설정 struct 목록을 넘긴다. 게임 lane에서 부른다. 변수 하나마다의 콜백은 두지 않는다(Config 5.3의 구독을 이것으로 바꾼다). CF2의 `Subscribe`(이름)·`SubscribeSettings`·`Unsubscribe`와 `ConfigChange` 콜백은 CVR7에서 없앴다(하위 호환 경로 없음 — 쓰던 LogHost는 `OnSynced` + `Watcher`로 옮겼다).

## 8. 범위 계층

- 범위는 Global(문맥이 가진 뿌리 — `CVarScopeId{}`, 값 0) 아래의 나무다. `ConfigContext::CreateScope(parent = Global)`·`DestroyScope(scope)`는 게임 lane에서 부르고(다른 스레드는 `WrongThread`) 다음 Sync가 게시하는 snapshot에 반영된다(그래서 그 Sync 전에는 다른 스레드의 `Get(scope)`가 새 범위를 모른다 — Global 값이다).
- 범위 id(`cvar::CVarScopeId` — `uint64`)는 프로세스 원자 카운터에서 받고 다시 쓰지 않는다. 문맥이 여럿이어도 겹치지 않으므로 다른 문맥의 범위 id는 어느 표에도 없는 id다(Global 값). 오류: `CreateScope`의 부모가 없으면(파괴됐거나 다른 문맥) `NotFound`, `DestroyScope(Global)`은 `InvalidArgument`, 없는 범위는 `NotFound`.
- 범위는 변수마다 **값 하나**(값·출처 층·출처)만 가진다. 층 스택은 Global에만 있다. 자식 범위에 값이 있으면 부모보다 이긴다. 범위 값은 층과 무관하게 Global 유효값보다 강하다(Global은 사슬의 끝이다). 범위 값은 `Clear({.scope = scope})`로 지운다.
- 범위를 파괴하면 그 범위의 값은 버리고, 그 자식은 조부모(파괴한 범위의 부모)에 다시 붙는다. 파괴한 범위에 큐로 들어온 쓰기는 적용 때 버린다(6.2절).
- 값의 검사(범위·`Clamp`·`Validate`·enum)는 Global 쓰기와 같은 함수다. `Clamp()` 변수는 보정해 받고 `Clamped` 진단을 남긴다.
- **snapshot의 범위 표**(불변): 범위마다 노드(id, 부모 id, (번호, 칸, 텍스트 블록)을 번호 순서로 정렬한 배열)이고 노드는 id 순서다. 표에는 "범위 값이 하나라도 있는 번호" 비트 집합이 함께 있다. 범위 상태(생성·파괴·값)가 바뀐 Sync만 새 표를 만들고, 아니면 앞 snapshot의 표를 공유한다. 새 표도 노드는 참조 계수로 공유한다: 그 Sync에서 값·부모가 바뀐 범위 노드만 새로 만들고 나머지는 앞 표의 노드를 그대로 쓴다(게임 lane의 범위 상태도 범위마다 copy-on-write — 그 Sync에서 처음 바뀌는 범위만 사본을 만든다).
- **범위만 바뀐 Sync**(`CreateScope`·`DestroyScope`, 큐에 범위 쓰기만 있다)는 Global 층의 다시 합치기(섹션·콘솔 설정·묶음 해결)를 건너뛰고 범위 상태만 적용·게시한다(Global 칸·`OnSynced`의 번호 목록은 그대로 — 빈 목록).
- **진단**: 범위 쓰기의 적용 진단(`CheatsDisabled`·`ConsoleReadOnly`·값 거부 등 — 넣을 때 다시 검사한 `Clamped`는 큐에서 꺼낼 때 한 번)은 적용 단계에서 모으고 게시가 성공해야 남는다. 게시가 실패해 다음 Sync가 다시 적용할 때 같은 진단이 두 번 남지 않는다. 파괴된 범위나 그 문맥에 등록되지 않은 이름의 쓰기는 값 검사 없이 버리고 진단도 남기지 않는다(6.2절 — 범위는 실행 중 객체라 그 객체가 먼저 없어지는 것은 정상 순서다).
- 빠른 길: 비트가 없으면 `Get(scope)`는 `Get()`과 같은 비용이다(비트 하나 읽기 더). 비트가 있으면 사슬 깊이만큼 범위를 id로 이진 탐색하고 그 범위의 값 배열을 번호로 이진 탐색한다.
- 범위 값의 변경은 Global 칸의 세대를 바꾸지 않는다(`Watcher`·`OnSynced`의 번호 목록은 Global 유효값의 변경이다). 범위 값을 따르는 쪽은 프레임마다 `Get(scope)`를 읽는다.
- `BootCVar`·`Replicated()` 변수와 `CVarGroup` 묶음 변수는 범위 값을 받지 않는다(Global만, 쓰기 거부 `ScopeNotAllowed`). 예외는 치트 허용 상태 `cheat.enabled`다(플레이어별 치트 허용 — 13.1절).
- 범위 값은 dump(`DumpConsoleValues`)에 넣지 않는다(범위는 실행 중 객체다). 콘솔 `<이름>`·`<이름> ?`는 그 변수의 범위 값을 `scope <id> <출처 층> <값> <출처>` 줄로 보인다(15절).
- 범위는 객체가 소유한다(14.2절): World가 Global의 자식 범위를, 플레이어·Viewport가 World 범위의 자식 범위를 만든다.

## 9. 부팅 고정 변수

- `cvar::Boot(name, default)` → `BootCVar<T>`. `Get()`만 있다.
- `ConfigContext::CompleteStartup`(Config 5.4 부트 5단계)이 봉인이다. 봉인 전에는 엔진·엔진 플랫폼·프로젝트·프로젝트 플랫폼·기기 프로필·사용자(`UserSetting()`일 때)·세션 층이 값을 정한다.
- 봉인 뒤 이번 실행 값은 바뀌지 않는다. 들어온 변경은 경로에 따라 셋으로 나뉜다. 설정 struct 필드의 `RestartRequired`·`StartupOnly`와 같은 "부팅 값" 규칙이다(Config 5.1·8절 — CF3 구현, Config 개정 7·8).

| 들어온 길 | 처리 | 진단 |
|---|---|---|
| 이번 실행에 바로 쓰는 쓰기(콘솔·코드·범위 임시 변경 층 — 콘솔 입력·이름으로 하는 대입·`Set`·`ScopedOverride`, 설정 struct 필드의 콘솔 이름·필드 경로 포함 — 개정 12) | 쓰기 때 거부한다. 부른 쪽이 바로 효력을 기대하므로 결과로 알린다. 이 층의 문서 입력(`AddLayerText`)에 부팅 값이 하나라도 있으면 문서 전체를 거부한다. 봉인 뒤 아직 등록되지 않은 이름이라 받은 쓰기는 그 이름을 등록할 때 받지 않는다 | Error `BootSealed` |
| 파일 층(엔진·엔진 플랫폼·프로젝트·프로젝트 플랫폼 파일의 hot reload, `UserSetting()` 변수의 사용자 층) | 층에는 받아 두고 Sync가 지금 값과 출처를 유지한다. (이름, 대기 값, 지금 값)을 재시작 대기 목록(`ConfigContext::GetPendingRestarts`)에 남긴다. 다음 실행에 같은 파일이 다시 읽히기 때문이다. 되돌리면 대기가 없어진다 | Note `RestartRequired` |
| 그 밖의 층(기기 프로필·품질·기능 묶음 같은 파생 층, 핫픽스·세션·서버(`Server`) 같은 휘발 층) | 지금 값을 유지하고 대기를 만들지 않는다 | Warning `BootValueIgnored`(같은 이름·층·출처에 한 번) |

- 파생 층을 대기로 두지 않는 까닭: 시작 뒤 GPU 특성으로 다시 고른 기기 프로필이 부팅 변수를 정하면, 다음 실행의 부팅 때는 GPU 특성이 없어 같은 값이 다시 대기가 된다("영원한 대기"). 부팅 때 GPU 특성을 아는 방법은 Config 11절의 결정 필요 항목이다.
- **다음 실행용 값.** `UserSetting()` 부팅 변수는 봉인 뒤에도 사용자 층 쓰기(옵션 메뉴에서 그래픽 backend 고르기 등)를 받는다. 그 값은 사용자 파일에 저장되고 이번 실행의 `Get()`은 바뀌지 않는다. 위 표의 파일 층 행 그대로 대기 목록에 오르고, Explain은 `pending restart: <값>`을 보인다. 옵션 메뉴는 대기 목록으로 "재시작 필요" 표시를 한다. 이것이 프롬프트의 `RequiresRestart`를 대신한다(17절 VD15).
- 대기 목록은 Config CF3 구현(`ConfigPendingRestart`·`GetPendingRestarts`)을 그대로 쓴다. CVR4는 그 위에 `BootCVar`를 올리고, 쓰기 때 거부(`BootSealed`)를 더했다. 직접 쓰기 거부는 `BootCVar`(부팅 동적 변수 포함 — CVR7에서 CF2 독립 설정의 `StartupOnly`·`RestartRequired`가 이것이 됐다)와 설정 struct 필드의 `RestartRequired`·`StartupOnly` 지정자에 건다(필드는 CVR6 결정 — Config 8절 개정 13). 필드 지정자는 설정 struct에만 남는 성질이고(필드는 CVar 칸이 아니라 설정 struct 저장소에 있다 — 7.2절), 규칙은 `BootCVar`와 같다: 바로 쓰는 층 `BootSealed`, 파일 층 재시작 대기, 파생·휘발 층 `BootValueIgnored`.
- 봉인 때 부팅 값을 불변 블록으로 굳힌다. 봉인 뒤 모든 snapshot의 부팅 값은 같다(조각을 공유하고, 봉인 뒤 등록한 부팅 변수는 그 조각을 복사해 칸을 더한다). `Get()`은 묶인 snapshot의 부팅 블록 칸을 읽는다(범위·세대 없음).
- 봉인 전 snapshot을 읽는 `Get()`은 Debug·Development 빌드에서 assert다(초기화 순서 버그를 잡는다). 봉인 전 호출뿐 아니라 봉인을 넘겨 살아 있는 명시 `SnapshotBinding`, 다른 스레드가 쥔 봉인 전 snapshot도 같다. 렌더 lane과 worker는 봉인 뒤에 시작하므로(Config 5.4 순서) 실제로는 게임 lane의 부트 코드만 해당한다.
- 큐에 남은 쓰기가 있으면 `CompleteStartup`은 `Busy`다(그대로 봉인하면 그 쓰기가 부팅 값에서 빠진다). 호스트는 Sync한 뒤 봉인한다.
- 콘솔에서 바꾸려 하면 `render.backend is a boot variable; set it in a config file or with --set and restart`를 출력한다. Explain·dump는 `[boot]`를 붙인다.
- 실행 중 바꿀 수 있는 값은 `CVar`, 이번 실행 동안 고정인 값은 `BootCVar`다. "실행 중 바꿀 수 있지만 효력은 다음 실행"이라는 세 번째 종류는 두지 않는다. `Get()`이 새 값을 바로 돌려주면서 효력은 다음 실행이라는 것은 모순이기 때문이다.

## 10. Sync(프레임 sync 지점)

Config 5.3·5.4의 sync 지점(엔진 `RunFrame` 2단계 뒤, 3단계 전)에서 게임 lane이 `ConfigContext::Sync`를 부른다. 순서는 다음과 같다.

0. sync hook을 등록 순서대로 부른다(`ConfigContext::AddSyncHook`·`RemoveSyncHook` — Config CF3 구현). hook은 게임 lane에서 큐를 보기 전에 불리고, 층 입력·`Unset`·`ReplaceLayerText`로 쓰기 큐에 넣을 수 있다. 넣은 것은 이번 Sync의 1단계가 적용한다. hook 안에서 `Sync`를 부르거나 hook을 빼면 `Busy`다. hot reload가 이 단계를 쓴다(아래).
1. 쓰기 큐를 넣은 순서대로 층 스택·범위 값에 적용한다(권한 재검사 — 6.2절).
2. 묶음 해결: 앞 Sync의 펼침을 모두 빼고, 묶음을 위상 순서(부모 묶음 → 자식 묶음 — 묶음의 묶음은 DAG다, 11절)로 펼친다. 묶음마다 그때까지의 펼침을 합친 입력으로 묶음 변수의 유효 등급(요청 등급)을 계산하고, 그 등급의 항목을 Quality 층에 쓴다. 다 펼친 뒤 조합 검증(Config 6.1)이 요청 등급을 낮추면, 낮춘 등급으로 처음부터 다시 펼친다(앞 차례의 낮춘 결과는 요청 등급이 같은 묶음에만 쓴다). 매 Sync는 같은 시작점(묶음 변수의 유효 등급)에서 출발하므로 같은 입력이면 같은 결과다(앞 Sync의 펼친 등급에 기대지 않는다). 조합 검증이 없거나 낮추지 않으면 한 번에 끝난다. 다시 펼치기는 `MaxGroupPasses`(8)번까지이고, 넘으면 마지막 결과를 쓰고 Warning `GroupNoConverge`를 같은 상태에 한 번 남긴다(검증 규칙끼리의 상호작용에 대한 안전장치). 봉인 뒤 부팅 변수는 이 계산에서 지금 값을 유지하고 9절 표대로 대기 목록·진단을 남긴다.
3. 이번 Sync 전체에서 유효값이 바뀐 칸의 세대를 올린다(반복 중 값이 갔다가 되돌아온 칸은 올리지 않는다).
4. 바뀐 조각만 복사한 새 snapshot을 게시하고 게임 lane을 다시 묶는다.
5. 큐에 든 콘솔 명령을 실행한다(Config 5.2와 같다).
6. `OnSynced`를 부른다.

대기 값은 `Register`가 바로 적용하므로(5절) Sync 순서에 들어가지 않는다.

**hot reload**(에디터·개발 빌드, Config 8절 — CF3 구현 `ConfigFileReloader`, `Config/ConfigReload.h`)
- 엔진·프로젝트·사용자 `Config/` 폴더를 감시한다. 감시 이벤트는 "다시 읽을 파일" 표시만 하고 값을 바꾸지 않는다. 그래서 값은 sync 지점에서만 바뀐다는 규칙이 hot reload에도 그대로다.
- 0단계 hook이 표시된 파일만 다시 읽는다. 있는 파일은 `ReplaceLayerText`로 그 (층, 태그) 입력을 바꾼다(사용자 파일은 관대한 읽기로 바꾼다). 지운 파일은 `Unset(층, 태그)`로 뺀다. 감시 이벤트를 잃었으면(Overflow) 그 루트의 모든 branch 파일을 다시 읽는다.
- `ReplaceLayerText`는 새 내용을 `AddLayerText`처럼 검사하고, 통과해야만 그 앞에 `Unset`을 넣는다. 잘못 고친 파일은 옛 입력을 지우지 않고 진단만 남긴다. 같은 Sync의 1단계가 `Unset`과 새 입력을 함께 적용하므로 다른 스레드는 옛 값 전체나 새 값 전체만 본다.
- 바뀐 값이 부팅 변수면 9절 표의 파일 층 행(재시작 대기)이다. 출시 규칙에서는 감시하지 않는다(`Start`가 `Unsupported`).
- CVR2에서 쓰기 큐(6.2절)가 생기면 hook은 그 큐에 넣는다. 감시 이벤트를 받는 스레드가 직접 큐에 넣지 않고 hook을 거치는 까닭은, 파일 읽기와 검사를 게임 lane의 sync 지점에 모아 한 Sync에 한 파일의 입력이 통째로 들어가게 하기 위해서다.

- 게임 lane 전용 API(`Sync`·`CompleteStartup`·`Register`·`CreateScope`·`DestroyScope`·`Explain`·`DumpOverrides`·`ReportUnresolved`)는 모든 구성에서 소유 스레드를 검사해 다른 스레드의 호출을 `WrongThread`로 돌려준다(Config 관례 — Debug assert가 아니다).
- 같은 Sync에서 바뀐 값은 같은 snapshot에 함께 들어간다. 다른 스레드는 그 snapshot 전체를 보거나 이전 snapshot 전체를 본다.

### 10.1 스레드 안전성

원칙은 셋이다. 이 원칙을 벗어나는 공유 상태는 두지 않는다.
- **가변 상태는 게임 lane 하나가 소유한다.** 층 스택·범위 표·이름 표·대기 값·묶음 상태는 게임 lane만 읽고 쓴다. 다른 스레드가 들어오는 길은 쓰기 큐 하나뿐이다.
- **다른 스레드가 읽는 것은 게시된 뒤 바뀌지 않는다.** snapshot·조각·문자열 블록·범위 표 사본·부팅 블록은 만든 뒤 고치지 않는다. 수명은 참조 계수로 지킨다.
- **핸들의 상수 부분은 컴파일 때 정해진다.** 이름·타입·범위·enum 표·`Validate`·플래그는 상수 초기화된 핸들 안에 있어 어느 스레드가 읽어도 경쟁이 없다.

| 공유 상태 | 쓰는 쪽 | 읽는 쪽 | 동기화 | 수명 |
|---|---|---|---|---|
| 핸들 상수 부분 | 없음(상수 초기화) | 모두 | 필요 없음 | 프로그램 |
| 핸들 번호 `index_` | 첫 `Register`(CAS 한 번, 이후 불변) | 모두 | 원자 값(relaxed). 그 칸이 든 snapshot은 등록 뒤 release로 게시되므로, 그 snapshot을 acquire로 얻은 스레드는 번호도 본다. 번호가 안 보이면 칸도 없으므로 결과가 같다(5절) | 프로그램 |
| sync hook 목록 | 게임 lane(`AddSyncHook`·`RemoveSyncHook`) | 게임 lane(Sync 0단계) | 소유 스레드 assert. hook 안에서 빼면 `Busy` | 문맥(hook의 context는 뺄 때까지 산다) |
| 쓰기 큐 | 모든 스레드(`Set`·`Clear`·콘솔 입력·`ScopedOverride` 소멸), 게임 lane의 sync hook(hot reload) | 게임 lane(Sync) | 다중 생산자·단일 소비자 큐(Core `BoundedMpscQueue` 또는 잠금 큐 — 쓰기는 읽기 경로가 아니다). 꽉 차면 `Busy`를 돌려주고 버리지 않는다 | 문맥 |
| 현재 snapshot 포인터 | 게임 lane(Sync·`Register`) | `Acquire()` | 짧은 잠금 안에서 바꾸고 참조 계수를 올려 돌려준다. 잠금은 게시·`Acquire`만 잡고 `Get()`은 잡지 않는다 | 참조 계수 |
| snapshot·조각·문자열 블록 | 만들 때 한 번(게임 lane) | 묶인 모든 스레드 | 불변 + 원자 참조 계수. 바뀐 조각은 새로 만들고 옛 조각은 마지막 참조가 놓일 때 해제한다 | 마지막 참조까지 |
| 스레드 묶음 | 그 스레드만 | 그 스레드만 | thread_local. 묶음이 참조 하나를 쥔다(게임 lane은 현재 snapshot, 렌더 lane은 packet, worker는 작업 문맥) | 묶음 범위 |
| 범위 표(생성·파괴·부모 — 게임 lane 상태) | 게임 lane(`CreateScope`·`DestroyScope`·Sync 적용) | 게임 lane만 | 소유 스레드 검사(`WrongThread`). 다른 스레드는 snapshot 안의 범위 표만 본다 | 문맥 |
| 범위 값(게임 lane 상태) | 쓰기 큐(아무 스레드의 `Set`·`Clear`의 `scope`) → Sync 1단계 | 게임 lane(Sync·콘솔 표시) | 쓰기 큐 하나로 들어온다. 적용은 큐 순서이고 Sync 실패 때는 사본을 버린다(값이 반쯤 바뀌지 않는다) | 문맥 |
| snapshot의 범위 표(id·부모·값) | 범위 상태가 바뀐 Sync의 게시(게임 lane) | 묶인 모든 스레드의 `Get(scope)` | 불변 + 참조 계수(바뀌지 않은 Sync는 앞 표를 공유한다). 범위 id는 프로세스 원자 카운터로 다시 쓰지 않는다. 파괴된 범위 id로 `Get(scope)`하면 그 범위가 든 snapshot에서는 그 사슬로 답하고, 그 범위가 없는 snapshot에서는 Global 값이다(Debug assert 없음 — 렌더 lane이 이전 프레임을 처리하는 것은 정상이다) | snapshot |
| 권한 상태(`cheat.enabled`·네트워크 역할) | 게임 lane(Sync) | `Set` 즉시 검사(호출 스레드의 묶음 또는 `Acquire`) | snapshot 값이므로 불변. 최종 판정은 Sync 적용 때 게임 lane이 다시 한다 | snapshot |
| 부팅 블록 | `CompleteStartup` 한 번, 봉인 뒤 부팅 변수 등록(조각 copy-on-write로 칸을 더한다 — 게임 lane) | 모두 | 게시한 조각은 불변, 봉인 뒤 snapshot의 부팅 값은 모두 같다. 렌더 lane·worker는 봉인 뒤에 시작한다(Config 5.4) | 문맥 |
| 진단 목록·`OnSynced`·명령 실행 | 게임 lane | 게임 lane | 소유 스레드 assert | 문맥 |
| 프로세스 기본 문맥(`DefaultChannel` — 기본 문맥 상태의 통로 참조를 가리키는 포인터, 6.2절) | 게임 lane(`MakeProcessDefault`·문맥 파괴의 `ReleaseProcessDefault`) | 묶음 없는 스레드의 `Set`·`Clear`·`ScopedOverride` | 원자 깃발 잠금(상수 초기화 — 전역 생성자 없음). 잠금 안에서는 포인터 비교·통로 참조 복사·통로의 `defaultWriters` 증가만 한다. 감소와 파괴 쪽 대기는 통로 잠금 + 조건 변수 | 기본 문맥이 없어지기 전에 풀린다. 통로는 `defaultWriters`가 0이 된 뒤 소유 스레드가 놓는다 |

**규칙**
- 다른 스레드에서 온 쓰기 사이의 순서는 큐에 들어간 순서다. 같은 변수·같은 층을 두 스레드가 같은 프레임에 쓰면 나중에 큐에 든 것이 이긴다. 서로 다른 스레드의 쓰기 순서를 바라는 코드는 호출 쪽이 순서를 정한다(시스템이 순서를 지어내지 않는다).
- `Set`이 즉시 돌려주는 결과는 그 시점 판정이다. 적용 때 판정이 달라지면(그 사이 치트가 꺼짐 등) 적용 때 판정을 따르고 진단을 남긴다. 값이 반쯤 적용되는 일은 없다.
- 콘솔 입력(`Execute`)을 다른 스레드에서 받으면 이름 해석은 그 스레드가 `Acquire()`한 snapshot의 이름 색인(불변)으로 하고 쓰기는 큐에 넣는다. 게임 lane의 이름 표를 직접 읽지 않는다.
- 문맥 파괴 전에는 모든 묶음이 놓여 있어야 한다. 문맥은 살아 있는 묶음 수를 원자 계수로 세고, 파괴 때 0이 아니면 Fatal이다(종료 순서 버그를 조용히 넘기지 않는다). 문맥은 소유 스레드에서만 파괴한다(다른 스레드면 Fatal — 그 스레드의 자동 묶음을 풀어야 한다).
- 한 스레드가 여러 문맥의 게임 lane이면 가장 최근에 게시(Initialize·Register·Sync — 바뀐 것이 없는 Sync 포함)한 문맥이 묶인다. 그 스레드에 명시 `SnapshotBinding`이 살아 있으면 자동으로 다시 묶지 않는다. 다른 문맥을 읽으려면 `SnapshotBinding`을 쓴다. 문맥을 파괴할 때 그 스레드 묶음이 자기면 푼다.
- `SnapshotBinding`은 LIFO로 푼다. 묶음은 중첩 깊이를 기록해 같은 snapshot을 두 번 묶고 바깥 것부터 풀어도 Fatal로 잡는다.
- 문맥 allocator는 다른 스레드가 snapshot을 쥐기만 해도(마지막 참조를 놓는 스레드가 해제한다) 여러 스레드의 동시 호출을 받아야 하고, 마지막 snapshot 참조와 쓰기 통로보다 오래 살아야 한다.
- 콜백(`OnSynced`·명령 함수)은 게임 lane에서 문맥 잠금 없이 부른다. 콜백 안의 `Set`은 큐로 가고 다음 Sync에 적용된다. 콜백 안에서 `Sync`를 부르면 `Busy`다(Config와 같다). 그래서 재진입 교착이 없다.
- 시험·도구에서 문맥이 여럿이어도 핸들 번호만 프로세스 공유이고 값은 문맥마다 따로다. 서로 다른 문맥을 다른 스레드에서 동시에 써도 공유하는 가변 상태는 번호 카운터(원자 값)뿐이다.
- 검증: 19절 스트레스 시험(짝 어긋남 0건), 묶음 수명 시험(작업이 프레임보다 오래 살 때), 다른 스레드 `Set`·게임 lane `Register`·`DestroyScope` 동시 실행 시험을 모든 플랫폼에서 돌리고, macOS CI에서 TSan 경고 0건을 확인한다.

## 11. 프리셋 묶음

```cpp
CVarGroup ShadowGroup = cvar::Group("quality.shadow", 2)
    .Help("Shadow scalability")
    .Level(0, {{ShadowQualityLevel, ShadowQuality::Off},    {ShadowDistance, 1000}})
    .Level(1, {{ShadowQualityLevel, ShadowQuality::Low},    {ShadowDistance, 3000}})
    .Level(2, {{ShadowQualityLevel, ShadowQuality::Medium}, {ShadowDistance, 6000}})
    .Level(3, {{ShadowQualityLevel, ShadowQuality::Epic},   {ShadowDistance, 15000}});
```

- 묶음 자체가 int32 변수다(`ShadowGroup.Get()`은 등급). 허용 값은 정의된 등급뿐이다. 정의되지 않은 등급은 사이에 있어도 거부한다(`UndefinedLevel`).
- 항목 `cvar::PresetEntry`는 `template <class T> constexpr PresetEntry(const CVar<T>&, TypeIdentityT<SetArg<T>>)` 생성자다(String·Name 변수의 값은 `StringView` — 상수로 만들 수 있는 타입이어야 하므로 `Set`의 인자 타입을 쓴다). `1000` → float 같은 암시 변환은 되고(목록 초기화라 좁히는 변환은 컴파일 오류), 타입이 다르면 컴파일 오류다(`Preset entry value type must match the CVar type`). `BootCVar`는 `static_assert` overload로 거부한다(4.3절 문구). 묶음이 다른 묶음을 항목으로 가지려면(전체 품질 `quality.overall`) `{ShadowGroup, 2}` 형태의 묶음 항목 overload를 쓴다.
- 묶음의 묶음은 DAG만 허용한다. 자기 자신을 항목으로 넣거나 순환(A의 항목이 B, B의 항목이 A)이 생기면 `Register`가 거부한다(`InvalidArgument`, Error `GroupConflict` — 메시지에 순환 경로 `a -> b -> a`). 항목이 다른 .cpp의 묶음을 주소로 가리키므로 컴파일 때는 막을 수 없다.
- 한 변수(묶음 변수 포함)는 한 묶음의 항목만 될 수 있다. 같은 변수를 항목으로 가진 두 묶음은 묶음의 묶음 관계가 아니어도 `Register`가 거부한다(`InvalidArgument`, `GroupConflict` — 두 묶음 이름). 두 묶음이 같은 칸을 쓰면 등록 순서가 결과를 정하기 때문이다. 묶음의 묶음은 부모가 자식 묶음의 등급만 정하므로 겹침이 필요 없다.
- 항목은 핸들 **주소**만 담고, 번호는 Sync에서 읽는다. 다른 .cpp에 정의된 변수를 가리켜도 정적 초기화 순서와 무관하다(주소는 상수다). 가리키는 변수가 아직 등록되지 않았으면 그 항목은 등록될 때까지 보류된다.
- 항목 값의 범위·`Validate`·enum 표·출시 규칙(개발 전용 변수), 묶음 항목의 등급은 항목 변수가 다른 .cpp에 있을 수 있어 컴파일 때 볼 수 없다. 묶음과 항목 변수가 둘 다 등록되는 때(묶음 등록 때 이미 등록된 항목, 항목이 나중에 등록될 때)에 검사해 맞지 않으면 (묶음, 등급, 항목)마다 Error 한 번을 남기고 그 항목을 펼치지 않는다. 펼칠 때도 같은 검사를 하고 같은 키로 다시 내지 않는다.
- 등급을 바꾸면 Quality 층의 그 묶음 태그를 `Unset`하고 새 등급 항목을 쓴다. 출처는 `quality.shadow@2`다.
- 구성원 표시는 자동이다: 어떤 묶음의 항목인 변수만 Quality 층 쓰기를 받는다(Config의 `QualityMember` 플래그를 없앤다). 구현은 Quality 층을 묶음 펼침 전용으로 둔다: 그 층에 직접 쓰는 입력(`AddLayerText`·`AddAssignment`·`Set`·`Unset`·`ClearLayer`)은 모두 거부하고(`LayerNotAllowed`), 펼침은 항목에만 쓰므로 항목이 아닌 변수는 Quality 층 값을 받을 길이 없다.
- 같은 등급에 같은 변수 두 번, 같은 등급 두 번 정의, 기본 등급 미정의는 컴파일 오류다. 크기 한도는 이름 있는 상수 `MaxGroupLevels`(8)·`MaxEntriesPerLevel`(32)이다. consteval 안에서는 할당할 수 없으므로 `CVarGroup`은 이 고정 용량을 품는다(항목은 정의 주소 + `ScalarValue` 48 byte라 묶음 하나에 약 12 KiB의 정적 데이터). 묶음은 수십 개 이하라 받아들인다.
- 프로젝트는 `Quality.json`(Config 6.1)에서 코드가 정의한 항목의 **값만** 바꿀 수 있다. 항목 추가는 코드에서 한다.
- Config 6.1의 나머지 규칙은 그대로다: 사용자가 고른 등급은 사용자 층에 저장한다(묶음 변수는 늘 `UserSetting()`). 등급 조합이 장치 능력(`DeviceCaps`)으로 불가능하면 정해진 순서로 낮추고 사유를 진단에 남긴다.
- 낮춘 등급은 묶음 변수에 쓰지 않는다. 묶음 변수(`ShadowGroup.Get()`)는 사용자가 고른 등급 그대로이고, 낮춤은 펼칠 때 쓰는 등급에만 적용된다. 그래서 Quality 층 항목 값이 낮춘 등급의 것이 된다. 사용자 층보다 약한 Quality 층에 등급을 써서는 효력이 없고, 고른 값을 덮어쓰면 장치가 바뀐 뒤 원래 선택을 잃기 때문이다. 펼친 등급은 Explain·진단에 `quality.shadow@1 (requested 3, lowered: <사유>)`로 보인다.
- `GroupBuilder`도 복사·이동 삭제와 `&&` 체인을 따른다.

## 12. 지연 바인딩과 진단

- Config 파일·`--set`에 아직 등록되지 않은 이름이 있으면 대기 값으로 둔다(Config 5.1). 나중에 등록되면 `Register`가 바로 검사·적용하고 `LateBound` 진단(정보)을 남긴다. 늦게 로드된 플러그인 변수도 같다.
- `ConfigContext::ReportUnresolved()`는 호스트가 `CompleteStartup` 바로 뒤에 부른다. 남은 대기 이름마다 `UnresolvedName` 진단을 남기고, 등록된 이름 중 편집 거리가 가장 작은 것(거리 ≤ `MaxSuggestionDistance` = 3)을 `did you mean 'render.shadow.distance'?`로 붙인다. 편집 거리 함수는 Core 텍스트 유틸리티의 `EditDistance`(OSA — 제한된 Damerau-Levenshtein, 인접 두 글자 맞바꾸기 하나가 1)다. 명령줄 이름 제안([AppInfo·CommandLine](AppInfoCommandLine.ko.md))과 같은 함수이고, CVar는 대소문자를 구분해 비교한다(콘솔 이름은 소문자 규칙).
- 모든 거부·보정은 Config 진단(`ConfigDiagnostic` — 출처·코드·이름·메시지)으로 모으고 Log API로 기록한다(영어). `TakeDiagnostics()`는 모은 진단을 꺼내고 비운다.

| 코드 | 경우 |
|---|---|
| `ValueOutOfRange`·`Clamped` | 범위 밖 거부 / `Clamp()` 보정 |
| `InvalidEnum`·`ValidationFailed`·`InvalidSetting` | enum 표 밖 / `Validate` 위반 / 형식 오류 |
| `CheatsDisabled`·`ReplicatedOnClient`·`ConsoleReadOnly` | 권한(13절) |
| `BootSealed`·`ScopeNotAllowed` | 9절·8절 |
| `UndefinedLevel`·`GroupNoConverge` | 11절·10절 |
| `LateBound`·`UnresolvedName` | 이 절 |
| `GroupConflict` | 묶음의 묶음 순환·자기 참조, 두 묶음이 같은 항목(11절 — `Register` 거부) |
| `QualityLowered` | 정보: 조합 검증이 펼칠 등급을 낮췄다(11절 — 같은 결과는 다시 내지 않는다) |
| `RestartRequired` | 봉인 뒤 파일 층(사용자 층 포함)이 부팅 변수를 바꿨다 — 재시작 대기(9절) |
| `BootValueIgnored` | 봉인 뒤 파생·휘발 층이 부팅 변수를 바꾸려 했다 — 지금 값 유지(9절) |

## 13. 치트와 네트워크

### 13.1 치트

- `Cheat()` 변수와 명령은 출시 빌드(`IsReleaseBuild`)에서:
  - 등록되지 않는다(`Register`가 `if constexpr`로 건너뛴다). 콘솔·dump·파일에 보이지 않는다.
  - `Get()`은 핸들에 든 기본값을 돌려준다. 이름·도움말 문자열은 핸들에 담지 않아(consteval 변환이 출시 빌드에서 버린다) 실행 파일에 남지 않는다.
  - consteval 변환이 출시 빌드의 `Cheat()` 항목에서 명령 함수·`Validate` 포인터도 null로 버린다. 그래서 그 함수를 참조하는 곳이 없어 링커가 뺀다.
  - 출시용 쿠킹(Config CF5)은 허용 목록으로 치트 변수 값을 빼므로 출시 config 파일에 치트 이름이 남지 않는다(`UnresolvedName`이 생기지 않는다).
- 개발 빌드에서 시험하는 출시 규칙: `ConfigContextOptions::releaseRules`(지금 있는 시험 옵션)를 켜면 `Register`가 `Cheat()` 변수를 숨김·쓰기 불가 칸으로 등록한다. 칸에는 기본값만 있으므로 `Get()`에 따로 분기가 없다. Debug 시험으로 출시 동작을 확인한다(지금의 `DevelopmentOnly` 동작과 같다).
- 개발 빌드에서는 **치트 허용 상태**가 꺼져 있으면 콘솔 층 쓰기·명령 실행을 `CheatsDisabled`로 거부한다. 파일·`--set`은 막지 않는다(개발자 입력이다).
- 치트 허용 상태는 Config 모듈이 정의하는 CVar 하나 `cheat.enabled`(`cvar::CheatsEnabled` — bool, `Replicated()`, `ConsoleReadOnly()`, 범위 값 허용)뿐이다. 따로 둔 원자 값은 없다. `ConfigContext::Initialize`가 등록한다.
  - 기본값 true(단독 실행). 게임 규칙 객체(UE `AGameModeBase::AllowCheats`)가 Code 층에 쓰고, `--set=cheat.enabled=false`·서버(Server 층)로도 바꾼다. 콘솔 층으로는 바꿀 수 없다(개정 13 — 콘솔 층은 Code 층보다 강하므로 콘솔로 켜면 게임 규칙의 false를 이겨 치트 규칙이 뚫린다).
  - 복제 변수이므로 클라이언트는 서버가 준 값(Server 층)을 따르고 콘솔에서 켤 수 없다.
  - 플레이어마다 다르게 하려면 그 플레이어 범위에 값을 둔다(14.3절). 판정은 쓰기를 낸 범위의 값이다(`CheatsEnabled.Get(scope)` — 핸들 이름 `CheatsEnabled`).
  - 판정은 쓰기를 받을 때 한 번, Sync 적용 때 한 번 한다(6.2절). 받을 때는 그 스레드가 보는 snapshot의 값이다. 적용 때는 **큐 순서**로 판정한다: Sync가 큐를 꺼내며 `cheat.enabled`를 바꾸는 입력(Code·`--set`·Server import 등)을 따라 그 시점의 실효값을 갱신하므로, 큐에서 치트를 끄는 변경 뒤에 놓인 치트 쓰기는 거부되고 앞에 놓인 것은 받는다(개정 13). 콘솔 층의 `Settings` 문서 입력(`AddLayerText(Console, …)`)도 같은 판정을 받는다. 네트워크 역할은 큐에 들지 않는 즉시 상태라 적용 때의 역할로 판정한다.
  - 범위 값(CVR3): 출처 층이 콘솔인 범위 쓰기·지우기의 치트 판정은 **Global 실효값 AND 범위 사슬 값**이다(개정 17): Global 실효값(층 스택 — Server 층의 서버 값 포함)이 false면 범위 값과 무관하게 거부하고, true일 때 범위 사슬에서 처음 만난 `cheat.enabled` 범위 값이 false면 거부한다. 범위는 치트를 좁히기만 하고 넓히지 못한다. 범위 값은 층 스택에 들지 않아 Server 층보다 약한지 강한지를 따질 수 없으므로, 범위 값이 Global을 이기면 클라이언트 코드가 `CheatsEnabled.Set(true, {.scope = player})`로 서버의 false를 뒤집을 수 있기 때문이다. `CheatsEnabled.Get(scope)` 자체는 다른 변수와 같은 읽기 규칙(범위 값이 이긴다)이고, 판정만 AND다. 받을 때는 그 스레드가 보는 snapshot(Global 칸과 범위 표)으로, 적용 때는 큐 순서로 본다: 같은 큐에서 앞에 놓인 `cheat.enabled` 범위 쓰기·지우기와 Global 실효값(위 큐 순서 규칙)을 따라간다.

### 13.2 복제

- 문맥은 네트워크 역할(`NetworkRole` — `Standalone`·`Server`·`Client`, 기본 `Standalone`)을 가진다. `ConfigContext::SetNetworkRole`로 정한다(넷 모듈이 접속 상태에 따라 부른다 — 지금은 호스트·시험이 부른다, 게임 lane). `GetNetworkRole`은 원자 사본을 읽어 어느 스레드에서나 부른다. 역할은 권한 판정에만 쓰이고, 역할을 정한다고 값이 전송되지는 않는다.
- `Replicated()` 변수는 `Client` 역할에서 콘솔 쓰기를 `ReplicatedOnClient`로 거부한다.
- `ExportReplicated()`(서버)는 복제 변수 **전부**(기본값이어도)를 이름 순서의 (이름, 값 표기) 목록으로 낸다(개정 13 — 기본값을 빼면 클라이언트의 파일·`--set`·Code 층 값이 남아 서버 값과 갈린다). `ImportReplicated(목록)`(클라이언트)은 Server 층을 통째로 바꾸고, 목록에 없는 복제 변수는 Server 층에서 지운다. 모르는 이름은 진단 후 건너뛴다.
- 전송은 네트워크 모듈이 맡는다(아직 없다). 이 문서는 목록 형식과 층만 정한다: 목록은 `ReplicatedValue`(이름, 값 표기 — 콘솔 표기) 배열이다. `ImportReplicated`는 서버 층(태그 `server`)을 통째로 바꾸고 다음 Sync에 적용된다. 넷 모듈이 생기기 전까지 Export/Import는 시험과 도구에서만 부른다(자리표시 전송 동작은 두지 않는다).

## 14. 객체 시스템과의 연결

CVar를 리플렉션·객체 수명과 묶어, 따로 만들 것을 줄이고 도구가 저절로 붙게 한다.

### 14.1 리플렉션을 값 표현으로

- **enum 이름**: 리플렉션 enum은 생성 메타데이터의 열거자 이름·도구 이름을 그대로 쓴다. 손으로 쓴 이름 표가 필요 없다.
- **값 표기**: 콘솔·`--set`·JSON의 값 해석은 ReflectionText 명령행 단일 값 표기 하나를 쓴다(Config 5.1과 같다). CVar 전용 파서를 만들지 않는다.
- **설정 struct 필드**: `ROCKS_STRUCT(Settings(...))` struct의 `ConsoleName` 필드는 같은 번호 체계에 들어가 CVar와 똑같이 콘솔·Explain·dump·프리셋 대상이 된다(저장소는 struct와 공유 — Config 7절). 묶어서 읽을 값은 설정 struct(`ConfigContext::Get<S>()`), 하나씩 자주 읽을 값은 CVar로 나눈다.
- **명령 인자**: `cvar::Command`의 매개변수 타입이 리플렉션 struct·enum·수학 타입(`Vec3` 등 CoreTypeBindings)이면 ReflectionText 표기로 읽는다(`cheat.teleport (X=0,Y=0,Z=100)` 또는 공백으로 나눈 성분).

### 14.2 범위는 객체가 소유한다

- `World`는 `Initialize`에서 Global의 자식 범위를 만들고 `Shutdown`에서 파괴한다(`World::CVarScope()`). World 안 객체(Actor·Component)는 `GetWorld().CVarScope()`로 읽는다.
  - 구현(CVR3): `WorldDesc::settings`(설정 문맥 — 엔진의 `EngineConfig::settings`를 `Engine::CreateWorld`가 넘긴다)가 있으면 `Initialize`가 범위를 만들고(실패하면 `Initialize` 실패), Subsystem 초기화가 실패해 되돌릴 때와 `Shutdown`(Actor·Subsystem·Level 정리 뒤)이 파괴한다. 설정 문맥이 없으면 `CVarScope()`는 Global이다. World는 설정 문맥과 같은 게임 lane에서 만들고 끝낸다(다른 스레드면 범위 API가 `WrongThread`). 범위 파괴가 실패하면(다른 스레드 등) World는 Warning 로그를 남기고 Debug 빌드에서 assert한다(조용히 버리지 않는다).
  - **수명**: 설정 문맥은 World보다 오래 살아야 한다(World가 문맥 포인터를 쥐고 `Shutdown`에서 범위를 파괴한다). 범위는 소유 객체가 문맥 파괴 전에 파괴해야 하므로, 범위가 남은 채 문맥을 파괴하면 Fatal이다(`ConfigContext destroyed while CVar scopes remain` — 명령 대상 묶음과 같은 규칙, 20절 CVR6 리뷰 반영). 그래서 World가 문맥보다 오래 살면 문맥 파괴가 그 자리에서 잡는다.
- 로컬 플레이어·Viewport(입력 계획의 플레이어 단계)는 World 범위의 자식 범위를 만든다. 분할 화면에서 플레이어마다 다른 값(시야 거리·디버그 표시)을 두는 데 쓴다.
- 범위 수명이 객체 수명과 같으므로 범위 id를 따로 관리할 일이 없다. 파괴 순서가 바뀌어도 8절의 재연결 규칙이 남은 자식을 살린다.

### 14.3 치트 집합 객체

함수 리플렉션(UiToolkit CU2 `ROCKS_FUNCTION`) 뒤의 기본형이다.

```cpp
ROCKS_CLASS()
class GameCheats : public CheatSet {
    ROCKS_GENERATED_BODY()

public:
    // 무적을 켜고 끈다.
    ROCKS_FUNCTION(Cheat, Help = "Toggle invulnerability")
    void God(bool enable);

    // 서버 권한이 필요한 치트다. 클라이언트에서 부르면 서버로 보낸다(네트워크 단계).
    ROCKS_FUNCTION(Cheat, Authority, Help = "Spawn an actor in front of the player")
    void Summon(ClassRef<Actor> type);
};
```

- `CheatSet`은 Object 파생이고, 개발 빌드에서 플레이어(플레이어 단계 전에는 World)마다 등록된 치트 집합 클래스 인스턴스를 하나씩 만든다. 모듈은 자기 치트 집합 클래스를 등록만 하면 된다(UE `UCheatManagerExtension`처럼 모듈별로 나뉜다).
- 콘솔 이름은 `cheat.` + 함수 이름의 소문자 밑줄 형태다(`God` → `cheat.god`, `GiveItem` → `cheat.give_item`). `Name = "..."` 지정자로 바꾼다.
- 실행은 입력한 플레이어 범위의 치트 집합 인스턴스에서 한다. 인자는 리플렉션 함수 스키마로 읽는다. 치트 허용 상태(13.1)는 그 플레이어 범위의 값을 본다.
- 출시 빌드에서는 생성 코드가 `Cheat` 함수의 등록 표를 만들지 않고 `CheatSet` 인스턴스를 만들지 않는다.
- CU2 전에는 1절의 `cvar::Command` 빌더로 같은 일을 한다. `CommandContext`가 입력한 범위·World·출력 sink를 준다. CU2 뒤에도 빌더는 객체가 필요 없는 명령(엔진 도구 명령)에 남는다.

### 14.4 객체 콘솔(개발 빌드)

리플렉션 경로 접근(`PathRead`·`PathWrite`)이 이미 있으므로 객체 필드를 콘솔에서 보고 바꾸는 명령을 저절로 얻는다.

| 명령 | 뜻 | UE 대응 |
|---|---|---|
| `obj.list <클래스>` | 살아 있는 인스턴스 목록 | `obj list` |
| `obj.get <객체> <필드 경로>` | 필드 값 | `get` |
| `obj.set <객체> <필드 경로> <값>` | 필드 쓰기(Cheat, 다음 Sync에 적용, struct 검증 hook 통과 필수) | `set` |

- 객체 지정은 World 안 이름 경로 또는 객체 id다. 쓰기는 리플렉션 경로 쓰기의 검증(CV3의 struct 검증 hook)을 그대로 거친다.

### 14.5 Sparks·도구

- Sparks 스크립트는 이름으로 찾은 `cvar::Index`로 읽고, 스크립트 변수는 `RegisterDynamic`으로 등록한다(언어 단계에서 문법을 정한다).
- 값 변경은 trace 이벤트로 낼 수 있다(Trace 채널 `config.cvar` — 이름·이전 값·새 값·출처). 프레임 기록에서 "이 프레임에 무엇이 바뀌었나"를 볼 수 있다.

## 15. 입력 소스와 콘솔 도구

- **파일**: Config 파일의 `Settings` 섹션(`"Settings": { "render.shadow.distance": 6000 }`)이다. 줄 주석 `//`을 쓸 수 있다(Config D1). 출처는 `파일:줄:열`이다.
- **명령줄**: `--set=render.shadow.distance=3000`(세션 층, Config 5.4).
- **콘솔 입력**:

| 입력 | 동작 |
|---|---|
| `<이름>` | 유효값과 출처를 보인다. enum은 `3 (High)` |
| `<이름> ?` | Explain(아래) |
| `<이름> <값>` | 콘솔 층에 쓴다 |
| `<이름> reset` | 콘솔 층 값을 지운다(아래 층 값이 드러난다) |
| `cvar.find <텍스트>` | 이름·도움말·분류에 텍스트가 든 변수·명령 |
| `cvar.explain <이름>` | Explain |
| `cvar.dump` · `cvar.dump overrides` | 전체 값 / 기본값과 다른 값만(JSON) |
| `<명령> <인자…>` | 명령 실행 |
| 이름 접두 | 자동완성(`CompleteConsoleName`) |

- **구현 규칙**(CVR6):
  - 값 표기는 ReflectionText 명령행 단일 값 표기 하나다. enum은 이름(대소문자 무시) 또는 수를 받는다 — 수는 Config가 그 enum 스키마의 열거자로 바꿔 넘긴다(ReflectionText enum 표기는 이름만이다. 수 입력은 CVar 값 규칙(4.4절)이라 Config 값 검사 한 곳에 둔다 — 콘솔·`--set`·파일이 같다). 표시는 `3 (High)`다.
  - `cvar.find`·`cvar.explain`·`cvar.dump`는 문맥이 등록하는 `ConsoleCommand`다(영역 `cvar`). 명령이므로 Sync 5단계에 실행되고 출력은 입력 때 받은 sink로 간다. `<이름>`·`<이름> ?`는 게임 lane이면 바로 출력한다.
  - 다른 스레드의 입력은 `ConsoleInputPort`(`ConfigContext::MakeConsoleInputPort` — 쓰기 통로를 참조 계수로 쥔다)로 넣는다. 10.1절 "다른 스레드가 들어오는 길은 쓰기 큐 하나" 원칙 그대로, 입구는 문맥 상태를 건드리지 않고 통로와 그 통로의 최신 snapshot만 본다. 문맥이 없어진 뒤의 입력은 `ShuttingDown`이다(정의된 동작). 통로는 문맥의 allocator에서 오므로 마지막 입구는 그 allocator보다 먼저 없어져야 한다. `ConfigContext::ExecuteConsoleLine`은 게임 lane 전용이다(다른 스레드면 `WrongThread`). 입구는 snapshot의 이름 색인(snapshot과 함께 게시되는 불변 표 — 이름·종류·플래그)으로 이름을 해석하고 권한(콘솔 읽기 전용·치트·복제·부팅 봉인)을 본다. 쓰기·`reset`은 쓰기 큐에 넣고(값 형식은 Sync가 꺼낼 때 검사해 거부하면 그 입력의 sink에 알린다), `<이름>`·`<이름> ?`·명령은 큐에 넣어 Sync 5단계에 게임 lane이 실행한다. 명령 도움말(`<명령> ?`)은 치트가 꺼져 있어도 받는다(게임 lane과 같다).
  - 출력 sink는 참조 계수 `ConsoleSink`(`SharedPtr<ConsoleSink>`)다. 큐에 든 일은 sink를 쥐고 있어 입력한 쪽이 먼저 놓아도 산다. 큐에 든 일의 출력은 게임 lane(Sync)이 쓰므로, 입력한 스레드와 동시에 sink를 쓰는 경우 sink가 스레드 안전해야 한다.
  - 인자와 `<이름> <값>`의 구분은 공백·탭이다. 따옴표 묶음은 없다(ReflectionText 단일 값 표기와 같다 — 공백이 든 값은 마지막 `StringView` 매개변수로 받거나 파일·`--set`으로 쓴다).
  - `cvar.explain`은 이름 토큰 하나만 받는다(그 밖은 `cvar.explain takes one name`).
  - 오류 문구는 `<이름> <사유>` 한 줄이다(예: `render.backend is a boot variable; set it in a config file or with --set and restart`). 그 밖의 진단은 `error <코드> <이름>: <사유>`다.
  - 출력 줄: `<이름> = <값> [boot]`, 재시작 대기면 `  pending restart: <값>`, 품질 묶음이면 `  quality.shadow@1 (requested 3, lowered: <사유>)`, 이어서 출처 스택(강한 것부터 `<층> <값> <출처>`, 마지막 `Default <값> code`).
- **Explain(name, scope)** 예:
  ```
  render.shadow.distance = 1200 (float, range 500..20000, clamp)
    scope player:1  Console   1200   console              <- applied
    scope world:3   (none)
    global          Console   2000   console
                    Session   3000   --set
                    Quality   6000   quality.shadow@2
                    Project   5000   Game/Config/Render.json:12:5
                    Default   8000   code
  ```
- **DumpOverrides()**: 기본값과 다른 유효값만 Config 파일 형식(`{"Settings": {...}}`)으로 내고, 값마다 출처를 `//` 주석으로 단다. `BootCVar`는 `// [boot]`를 붙인다. 구현은 `ConfigContext::DumpConsoleValues(out, overridesOnly)`다(CVar·독립 콘솔 설정·설정 struct 필드의 콘솔 이름 — 이름 순서, JSON 값은 `Settings` 값 모양: bool·수·문자열, enum은 이름 문자열). 형식은 Config 층 문서(줄 주석 JSON — Config D1, JSON5 2단계 전)와 같다. 개발 빌드의 `Cheat()` 값도 넣는다(개발 도구다 — 출시 규칙에서는 숨긴 설정이라 나오지 않고, 출시 쿠킹은 허용 목록이 뺀다). 이 출력을 빈 문맥의 **세션 층**(`--config`·`AddLayerText(ConfigLayer::Session, ...)`)으로 다시 읽으면 같은 유효값이 나온다. 세션 층은 품질·기기 프로필 층보다 강하므로 그 층에서 온 값도 그대로 재현된다. 범위 값은 dump에 넣지 않는다(범위는 실행 중 객체다).
- 출시 빌드의 `--set`은 Config D5대로 서버 대상만 받는다.
- 콘솔 이름 영역 표(Config 5.1)에 `cvar`·`cheat`·`obj`를 더한다.

## 16. 엔진 연결 지점

| 지점 | 위치 | 하는 일 |
|---|---|---|
| 등록 | `HostSettings` 부트 1단계(Config 5.4)와 각 모듈 등록 함수 | `config.Register(...)` |
| 봉인·미해결 보고 | 부트 5단계 `CompleteStartup` 바로 뒤 | `ReportUnresolved()` |
| Sync | 엔진 `RunFrame` 2단계 뒤, 3단계 전(Config 5.4) | 10절, 게임 lane 다시 묶기 |
| 렌더 lane | 프레임 packet 처리 시작(RenderBase RB1) | packet이 든 snapshot을 `SnapshotBinding`으로 묶는다 |
| worker | Task 생성 | 만든 lane의 묶음을 작업에 넘긴다 |
| World | `World::Initialize`·`Shutdown` | 범위 만들기·파괴 |
| 종료 | 문맥 파괴 전 | 모든 lane이 묶음을 놓았는지 Debug assert |

## 17. 결정 사항

| ID | 결정 | 상태 |
|---|---|---|
| VD1 | 등록은 모듈 등록 함수의 `Register(핸들…)` + Policy `cvar-registration`. 정적 초기화 때 스스로 목록에 붙는 방식(프롬프트의 "변환 순간 등록")은 쓰지 않는다. 이유: 엔진 전역 생성자 금지 원칙(Config 5.1·Trace T3), 정적 라이브러리에서 참조 없는 .cpp의 변수가 링크에서 빠지는 문제, 모듈 해제 때 목록 정리 문제 | 결정됨(2026-11-18, 사용자 — 권장안) |
| VD2 | 이름: 타입은 `CVar<T>`(업계 공용어), namespace `rocks::cvar`(2026-11-18 사용자 확인). 콘솔 이름은 Config D7의 소문자 점 경로(`render.shadow.distance`)이고 UE식 `r.ShadowDistance`는 쓰지 않는다. 프롬프트의 `cvar::Registry`·`cvar::Snapshot`은 기존 `ConfigContext`·`ConfigSnapshot`이 맡는다(같은 것에 이름 둘을 두지 않는다) | 결정됨(2026-11-18, 사용자 — 권장안) |
| VD3 | 입력 형식은 JSON `Settings` 섹션·`--set`(Config D1). 프롬프트의 ini `LoadIni`·`-cvar=`는 두지 않는다 | 권장안 |
| VD4 | 층 순서는 6.1절. 품질 묶음은 프로젝트 파일보다 강하다(구현과 같다. UE는 반대 — 프로젝트 ini가 이긴다). Config 3.1 표 비고의 "UE처럼 프로젝트보다 약하다"는 구현·표와 어긋나므로 고친다 | 권장안 |
| VD5 | Config의 `DevelopmentOnly`를 `Cheat`로 합친다(출시 빌드 제거 + 개발 빌드 치트 허용 검사). 치트 허용 상태는 복제 CVar `cheat.enabled` 하나 | 권장안 |
| VD6 | 모든 CVar가 snapshot에 든다. `RenderRead` 플래그는 CVar에서 없앤다(설정 struct 필드에는 남는다) | 권장안 |
| VD7 | `Get()`은 스레드에 묶인 snapshot을 읽는다. 묶음 없는 `Get()`은 Fatal | 권장안 |
| VD8 | 쓰기는 어느 스레드에서나(검증 즉시, 적용은 Sync). Config 5.3 "게임 lane 밖 쓰기 API 없음"을 바꾼다 | 권장안 |
| VD9 | 변경 알림은 `Watcher`와 Sync당 한 번의 `OnSynced`. 변수별 구독 콜백은 없앤다 | 권장안 |
| VD10 | 도움말·이름·범위·기본값·`Validate`를 컴파일 때 검사한다(프롬프트는 등록 때) | 권장안 |
| VD11 | enum 이름은 리플렉션 enum → ADL `CVarEnumNames` 순서 | 권장안 |
| VD12 | 치트 명령: 지금은 `cvar::Command` 빌더, CU2 뒤 `CheatSet` 리플렉션 함수가 기본형 | 권장안 |
| VD13 | ThreadSanitizer는 macOS CI 작업에서 돌린다(Windows Clang은 TSan이 없다). 짝 어긋남 0건 단언은 모든 플랫폼 시험이다 | 권장안 |
| VD14 | 프리셋 구성원은 항목에서 자동. `Quality.json`은 값만 바꾼다 | 권장안 |
| VD15 | `RequiresRestart`를 두지 않고 `BootCVar` + `UserSetting()`(봉인 뒤 다음 실행용 사용자 값 저장)으로 대신한다 | 권장안(리뷰 반영) |
| VD16 | 범위는 변수마다 값 하나. 층 스택은 Global에만 | 권장안(리뷰 반영) |
| VD17 | `Register`는 새 칸만 더한 snapshot을 즉시 게시한다. 칸이 없는 snapshot의 `Get()`은 기본값 | 권장안(리뷰 반영) |
| VD18 | 스레드 안전성은 10.1절의 세 원칙(가변 상태 단일 소유·게시물 불변·핸들 상수)으로 보장하고, worker는 snapshot 참조를 쥔다 | 권장안(사용자 요구 2026-11-18) |

## 18. 프롬프트 대비 바꾼 점

| 프롬프트 | 이 문서 | 이유 |
|---|---|---|
| 변환 순간 정적 초기화에서 등록, 함수 지역 static 레지스트리 | 상수 초기화 핸들 + 모듈 등록 함수, 값은 `ConfigContext` | 전역 생성자 금지·시험마다 독립 문맥(VD1) |
| Help 누락은 등록 시점 assert·진단 | 컴파일 오류 | 더 일찍 잡는다(VD10) |
| `std::string`·`std::array`·`std::pair` | `String`·`StringView`·`Span`·`cvar::EnumEntry` | 엔진 타입 사용 |
| `Validate(bool(const T&, std::string& err))` | `StringView(*)(const T&)` | 할당 없음, 상수 평가 가능 |
| SFINAE 탐지 | C++23 requires 식 | 같은 효과, 진단이 짧다 |
| 층 10개(Default…Test) | Config 11층 + Hotfix·Code·Server·Scoped | 기존 층과 하나의 순서(Config D2) |
| `Flush`·`SealBoot`·`Acquire` | `Sync`·`CompleteStartup`·`Acquire` | `Sync`·`CompleteStartup`은 기존 이름. `Acquire`는 신규(기존 `Snapshot()`은 소유 스레드 전용) |
| `LoadIni`, `-cvar=` | JSON `Settings`, `--set=` | Config D1 |
| 컨텍스트 id `CreateContext` | `CVarScope`, 객체가 소유 | `ConfigContext`와 이름 충돌 회피, 객체 수명 연동 |
| `Clamp`·`Cheat`·`Replicated` 플래그 | 같다. `UserSetting`·`ConsoleReadOnly` 추가 | Config 플래그 이관 |
| `RequiresRestart` 플래그 | `BootCVar` + `UserSetting()` | 실행 중 바뀌는 `Get()`과 "다음 실행 적용"이 모순(VD15) |
| 층 Test가 가장 강함 | Scoped는 Code와 콘솔 사이, Server가 가장 강함 | 클라이언트 임시 변경이 복제 값을 이기지 않게(UE Temp < Console) |
| 컨텍스트마다 층별 값 | 범위마다 값 하나 | 단순화(VD16) |
| `Get(snapshot, ctx)` 등 overload 넷 | `Get()`·`Get(scope)` 둘 + `SnapshotBinding` | 단순화 |
| 시험 헬퍼 매크로 금지 | CVar 보조(`ScopedOverride`·fixture)는 매크로 없음. 시험 본문은 기존 `TEST`·`CHECK` 틀 | 기존 시험 틀을 바꾸지 않는다. "새 매크로 0개"는 CVar 코드에 적용 |

## 19. 시험과 수용 기준

**컴파일 성공**: 1절·4절·11절의 예시가 그대로 컴파일된다.

**컴파일 실패 시험**(`Rocks.ToolTests native`의 실제 컴파일 실패 하네스 — CompilerDiagnostics "실제 강제와 수용 기준" 3항. 기대 메시지 문자열이 진단에 있어야 통과, 문법 오류 실패는 불인정): bool에 `Range`, String에 `Clamp`, `auto`로 빌더 받기, 도움말 없음, 이름 문법 위반, 기본값 범위 밖, `Range` 없는 `Clamp`, 이름 표 없는 enum, enum `Count`와 표 크기 불일치, 프리셋 항목 타입 불일치, `BootCVar::Set`, 프리셋 항목·`ScopedOverride`에 `BootCVar`, `BootVarBuilder::Cheat`, 같은 등급 두 번.

**Policy**: CVar 소스의 새 `#define` 0개(`#pragma once` 제외), `cvar-registration` 양성·음성 사례.

**동작 시험**(Debug·각 플랫폼):
- 등록: 같은 핸들 재등록 멱등, 같은 이름 다른 타입 `TypeMismatch`, 등록 안 된 핸들 `Get()` Fatal(사망 시험).
- enum: 파일의 `"High"`가 적용되고 `Get()`이 `ShadowQuality::High`, 표시 `3 (High)`, 표 밖 값 거부.
- 검증: 파일의 범위 밖 값 거부, `Clamp()` 변수는 보정 + `Clamped`, `Validate` 위반 거부.
- 미해결: 오타 이름이 `ReportUnresolved`에 did-you-mean과 함께 나온다. 늦게 등록한 변수에 대기 값이 적용되고 `LateBound`가 남는다.
- 층: 세션 > 기기 프로필 > 프로젝트. 콘솔 `reset` 뒤 세션 값이 드러난다. Explain에 `파일:줄:열`. `ClearLayer(Hotfix)`로 핫픽스 롤백.
- 권한: 치트 꺼짐에서 콘솔 쓰기 거부, 클라이언트에서 `Replicated()` 콘솔 쓰기 거부, Server 층 값이 클라이언트 콘솔 값을 이긴다.
- 부팅: 봉인 전 파일·`--set` 값 적용, 봉인 뒤 콘솔·`Set` 거부 + `BootSealed`, 봉인 뒤 파일 층(hot reload·사용자 층) 변경은 대기 목록에 오르고 `Get()` 불변·되돌리면 대기 없어짐, 기기 프로필 재적용은 `BootValueIgnored` 한 번·대기 없음, 봉인 전 `Get()` assert(사망 시험).
- 범위: global=90, world=100, player1=110, player2(상속)=100. world 파괴 뒤 player2=90.
- 프리셋: 묶음을 0으로 바꾸면 구성원이 Quality 층 값이 되고 프로젝트 값을 이긴다. 정의되지 않은 등급 거부. 순환·자기 참조 묶음과 항목이 겹치는 두 묶음은 `Register` 거부. 같은 입력의 새 문맥은 같은 결과. 조합 검증 상호작용이 멈추지 않으면 8회 뒤 `GroupNoConverge` 한 번.
- Watcher: 첫 호출 true, 변경 없으면 false, 변경 뒤 true.
- ScopedOverride: 중첩 후 어떤 순서로 풀어도 원래 값.
- Dump 왕복: `DumpOverrides` 출력을 빈 문맥에 다시 읽으면 유효값이 같다.
- 스트레스: 게임 스레드가 프레임마다 변수 두 개를 짝으로 바꾸고(`a = n`, `b = -n`) Sync, 렌더 스레드가 프레임 gate마다 묶음을 바꿔 계속 읽는다. 짝 어긋남 0건을 단언한다. 순서는 명시 gate로 만들고 sleep·경쟁 운에 기대지 않는다. macOS CI에서 TSan 경고 0건(VD13).
- 스레드: 다른 스레드의 `Set`과 게임 lane의 `Register`·`DestroyScope`·Sync를 gate로 엇갈려 돌린다. 등록 직후 같은 프레임 `Get()`, 이전 snapshot에서 새 변수 `Get()`(기본값), 파괴된 범위 id로 읽기, 프레임보다 오래 사는 작업의 묶음 수명(Asan 없이도 참조 계수 단언), 묶음이 남은 채 문맥 파괴 Fatal(사망 시험).
- 지역 핸들: 함수 안 핸들을 `Register`하면 `InvalidArgument`.
- 출시 규칙: Debug에서 `releaseRules`를 켜면 `Cheat()` 변수가 콘솔·dump에 없고 쓰기가 거부되며 `Get()`이 기본값이다. Release 구성의 컴파일 시점 제거(문자열·함수가 바이너리에 없음)는 CVR6 완료 때 한 번 확인한다.

## 20. 단계와 완료 기준

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| CVR1 | 핸들·빌더(4절)·enum 이름(4.4)·등록과 번호(5절)·Policy·컴파일 실패 fixture | Config CF2(구현) | 19절 컴파일 성공·실패·Policy, 등록 시험 |
| CVR2 | 조각 snapshot·스레드 묶음·`Get`·`Watcher`·`OnSynced`·아무 스레드 쓰기 큐·새 층 넷·`Clear`·`ClearLayer`·`ScopedOverride` | CVR1 | 층·Watcher·ScopedOverride·스트레스 시험 |
| CVR3 | 범위 계층·World 연결(14.2) | CVR2 | 범위 시험 |
| CVR4 | `BootCVar`·봉인·대기 값 늦은 적용·`ReportUnresolved`(Core 편집 거리)·진단 코드 | CVR2 | 부팅·미해결 시험 |
| CVR5 | `CVarGroup`·Quality 층 펼침·되풀이 한도 | CVR2 | 프리셋 시험. Config CF4는 이 묶음을 쓴다 |
| CVR6 | 콘솔 도구(Explain·find·dump·reset·enum 표시)·`cvar::Command`·치트 허용 상태·네트워크 역할·Export/Import | CVR4 | 권한·dump 왕복 시험 |
| CVR7 | 이관: `log.*`(LogHost)·`trace.*`(TraceControl)·호스트 설정을 핸들로, `RegisterConsoleSetting` 공개 경로를 `RegisterDynamic`으로. `log.*`는 로깅 문서 L18·9.2절 표와 맞춘다: 실행 중 값은 `log.level`(enum 핸들, 대소문자 무시 입력)·`log.levels`(`.Validate(CheckLogLevelsSyntax)`)·`log.rate_limits`(`.Validate(CheckLogRateLimitsSyntax)`)이고, 나머지 `log.mode`·`log.console.layout`·`log.file.*`(`layout`·`max_age_minutes`·`compress` 포함)는 `BootCVar`다. 레이아웃 둘은 `.Validate(CheckLogLayoutSyntax)`, 닫힌 값 집합인 `log.mode`·`log.file.format`은 enum 핸들이다. 검사 함수(c5fe9289 — `LogHost.h`·`LogLayout.h`)는 `constexpr StringView (StringView) noexcept`이고 문법만 본다(순수 함수라 런타임 카테고리 목록은 못 본다. 카테고리 목록은 실행 중에도 닫혀 있지 않으므로 모르는 접두사는 판정하지 않고, LogHost는 적용 실패(표 용량·해시 충돌)만 적용 때 진단한다 — 아래 CVR7 메모) | CVR6 | 기존 Config·LogHost·Trace 시험 통과 |
| CVR8 | `CheatSet`·`ROCKS_FUNCTION(Cheat)`·객체 콘솔(14.3·14.4) | CVR6, CU2 | 치트 집합 등록·실행·출시 규칙 시험 |

- CVR1–CVR7은 Core 단독(Config 모듈)이고 World 연결만 Engine이다. CVR8은 함수 리플렉션 뒤다.
- 검증은 단계마다 Debug·format·policy·바뀐 부분 시험이다. TSan은 macOS CI다.

**CVR1 구현(2026-11-18, `feature/cvar`)**
- 헤더: `Config/CVar.h`(공개 진입점) → `CVarBuilder.h`(빌더·진입점) · `CVarHandle.h`(`CVar<T>`·`BootCVar<T>`) · `CVarEnum.h`(4.4절) · `CVarDefinition.h`(타입을 지운 상수 정의·번호·검사 함수). 등록은 `ConfigContext::Register`(템플릿) → `RegisterHandles`(`Private/CVarRegistration.cpp`).
- 저장: CVR2 조각 snapshot 전까지 등록한 핸들은 CF2 독립 콘솔 설정과 같은 저장소(`ConsoleSettingState`)에 들고 핸들 정의를 가리킨다. 그래서 대기 값·층·출처·콘솔 입력·dump가 그대로 붙고, 값 검사(범위·`Clamp` 보정·`Validate`)는 컴파일 때 기본값 검사와 같은 함수(`CheckValue<T>`)를 쓴다. 성질은 CF2 플래그로 옮긴다: `BootCVar` → `StartupOnly`, `Cheat()` → `DevelopmentOnly`, `UserSetting()` → `UserConfig`, `ConsoleReadOnly()` → `ConsoleReadOnly`, 모든 핸들 → `RenderRead`(VD6). `Replicated()`는 정의에만 남는다(검사는 CVR6).
- `Get()`은 CVR1에 두지 않았다. 7.1절의 읽기는 스레드에 묶인 snapshot의 번호 칸이고(VD7 — 묶음 없는 `Get()`은 Fatal), 묶음·조각 snapshot이 CVR2이므로 그 전의 임시 경로(이름 조회·전역 문맥)는 VD7과 다른 동작을 만들고 버려질 코드다. 등록된 값은 `FindConsoleSetting`·`ConsoleValue`로 본다. 19절 "등록 안 된 핸들 `Get()` Fatal" 사망 시험은 CVR2로 넘긴다.
- 지역 핸들 거부(4.2절)를 위해 PlatformBase에 `platform::CurrentThreadStackRange()`를 더했다(Windows `GetCurrentThreadStackLimits`, macOS `pthread_get_stackaddr_np`, Android `pthread_getattr_np`).
- 진단 코드 `Clamped`(Note)·`ValidationFailed`를 더했다(12절). 기본 영역 표에 `cvar`·`cheat`·`obj`를 더했다(15절 — Config 5.1 표와 같다).
- 스펙과 다른 점: `Clamp()`를 수가 아닌 타입에 부르면 메시지가 `Clamp() is only available for numeric CVars (...)`다(4.3절 표는 `Range()` 문구 하나). `BootVarBuilder`에 `ConsoleReadOnly`도 없다(4.2절 목록 그대로). enum `Count` 검사는 ADL 표에만 건다(리플렉션 enum의 이름은 생성 메타데이터가 정한다). 수 입력과 `3 (High)` 표시는 ReflectionText 단일 값 표기가 enum 이름만 받으므로 CVR6 콘솔 도구에서 정한다.
- 19절 컴파일 실패 목록 중 프리셋 항목(타입 불일치·`BootCVar` 항목·같은 등급 두 번)은 CVR5, `ScopedOverride`에 `BootCVar`는 CVR2에서 그 타입과 함께 더한다.
- (CVR1 당시) `Register`는 즉시 게시 대신 dirty 표시 뒤 다음 `Sync`가 반영했다. CVR2에서 VD17대로 즉시 게시로 바꿨다(아래).
- `Register`의 소유 스레드 위반은 Debug assert가 아니라 모든 구성에서 `WrongThread`다(Config 관례 — 10절 문장을 이에 맞췄다).
- 리뷰 반영(2026-11-18): 핸들 힙 생성 금지(`operator new` 삭제), Policy가 직접·중괄호 초기화·쉼표 뒤 선언자·비정적 멤버·`thread_local`·`const`·`constexpr`·핸들 타입 별칭을 잡고 `Templates`도 본다. `Register`는 모든 검사·기본값 해석(Name intern 포함)을 끝낸 뒤 대기 값을 넘기고, 넘기다 실패하면 이번 호출의 진단을 되돌린다(`ConfigDiagnostics::Mark`·`Rewind` — 실패한 등록은 진단을 남기지 않는다). Release `Cheat()` 정의는 enum 스키마도 버린다.

**CVR2 구현(2026-11-19, `feature/cvar`)**
- snapshot(7.2절): `Config/ConfigSnapshot.h`. CVar 칸은 `ChunkSlotCount`(256) 칸 조각의 표이고, 칸은 8 byte 값(실수는 double 비트)과 세대다. String·Name 값은 불변 텍스트 블록(`CVarText`)을 조각이 참조 계수로 쥔다. 이름 블록(CF2 독립 설정과 설정 struct 필드 중 RenderRead — 설정 struct의 렌더 읽기 블록)은 따로 공유되는 불변 블록이다. CVar 핸들은 이름 블록에 들지 않는다(VD6 — 모두 CVar 칸에 든다. CVR1의 `RenderRead` 플래그는 뺐다). Sync는 바뀐 칸이 든 조각만 복사하고, 이름 블록도 바뀐 경우에만 새로 만든다. 둘 다 그대로면 snapshot을 새로 만들지 않는다.
- `Initialize`가 빈 snapshot(세대 1)을 게시하고 소유 스레드를 묶는다. 그래서 초기화 뒤 `Acquire()`는 null이 아니다(CF2의 "첫 Sync 전 null"을 바꿨다).
- 스레드 묶음(7.1절): `thread_local` 묶음은 {snapshot, 게임 lane 통로}다. 게임 lane은 게시(Initialize·Register·Sync) 때 스스로 다시 묶는다(그 스레드가 명시 묶음·다른 문맥에 묶여 있으면 그대로 둔다). `cvar::SnapshotBinding`은 LIFO로 되돌리고(중첩 깊이로 확인), 앞 묶음이 게임 lane이면 그 문맥의 최신 snapshot으로 되돌린다. 한 스레드에 문맥이 여럿이면 가장 최근에 게시·Sync한 문맥이 묶인다(10.1절). 묶음 수는 문맥의 통로(`WriteChannel` — 문맥과 snapshot들이 공유)가 원자 계수로 세고, 문맥 파괴 때 0이 아니면 Fatal이다.
- `Get()`(7.1절): 묶음 → 조각 표 → 조각 → 칸. 묶음 없음 Fatal, 번호 없는 핸들은 게임 lane에서 Fatal·다른 스레드에서 기본값, 칸 없는 snapshot은 기본값이다. Name 변수의 기본값은 None이다(Name은 문맥 pool의 토큰이라 핸들이 상수로 담을 수 없다 — 칸이 있으면 등록 때 intern한 기본값이다). `BootCVar::Get()`도 같은 칸을 읽는다(봉인 블록은 CVR4).
- 쓰기 큐(6.2절, VD8): 통로의 잠금 큐 하나에 다른 스레드의 `Set`·`Clear`·`ClearLayer`와 소유 스레드의 층 입력(`AddLayerText`·`AddAssignment`·`Unset`·`ReplaceLayerText`)이 모두 넣은 순서대로 든다(한도 `MaxQueuedChanges`는 다른 스레드의 쓰기에만 건다 — 넘으면 `Busy`. 기기 프로필은 빼기와 새 값을 한꺼번에 넣는다). `Set`은 바로 범위·`Validate`·층 권한을 검사하고(값 표기를 만들어 콘솔 이름 입력으로 넣는다), Sync가 꺼낼 때 소유 스레드에서 다시 검사해 진단(`Clamped` 등)을 남기고 거부되면 버린다. 같은 (층, 태그)의 `Set`은 앞 `Set`을 바꾼다. 다른 스레드에서 쓰려면 문맥 allocator가 동시 호출을 받아야 한다.
- 층(6.1절): `Hotfix`·`Code`·`Scoped`·`Server`를 더했다. `ScopedOverride`는 `scoped.<번호>` 고유 태그로 Scoped 층에 쓰고 소멸 때 그 태그를 지운다.
- Sync 순서(10절): hook → 큐 꺼내기·재검사 → 적용·유효값 계산(묶음 펼침 반복은 CVR5) → 바뀐 칸 세대 → 게시·다시 묶기 → 명령·CF2 구독 콜백 → `OnSynced`. `OnSynced`는 바뀐 것이 없는 Sync에도 빈 목록으로 부른다. hot reload hook은 `ReplaceLayerText`로 새 큐에 (빼기, 새 입력)을 한꺼번에 넣는다.
- `Set`의 오류는 `Result`의 `ErrorCode`이고 거부 이유는 `SetOptions::reason`이다(6.2절 — 개정 7).
- CF2 이관(21절): `Snapshot()`은 `Acquire()`로 바꿨다(snapshot 구조가 바뀌어 같은 API를 둘 수 없다 — RenderBase와 시험을 함께 바꿨다). 변수별 `Subscribe`는 남겼다: 21절대로 쓰는 곳(LogHost)을 CVR7에서 핸들·`OnSynced`·`Watcher`로 한꺼번에 옮긴다(LogHost의 `log.*`는 아직 핸들이 없는 CF2 독립 설정이라 지금 옮기면 CVR7에서 한 번 더 바꾼다).
- 넘긴 것: `Get(scope)`·`SetOptions::scope`(CVR3), 봉인 뒤 `BootSealed`·불변 부팅 블록(CVR4), 묶음 펼침 반복(CVR5), 권한(치트 허용·복제) 적용 때 재판정(CVR6 — 지금 적용 때 다시 보는 권한은 층 권한뿐이다), 렌더 lane이 프레임 packet snapshot을 묶는 연결(RenderBase RB1 — RenderBase는 Config를 모른다, 16절 엔진 연결)·Task 작업 문맥 슬롯으로 worker에 묶음 넘기기(16절), 묶음 없는 `Set`의 프로세스 기본 문맥(6.2절 — CVR7에서 `MakeProcessDefault`로 넣었다, `HostSettings`가 정한다), macOS TSan(CI).

**CVR4 구현(2026-11-19, `feature/cvar`)**
- 봉인(`ConfigContext::CompleteStartup`)은 `BootCVar`의 지금 값을 부팅 블록(조각 표 — `ConfigSnapshot::FindBootSlot`)으로 굳혀 게시한다. 봉인 뒤 snapshot은 모두 같은 부팅 블록 조각을 공유하고(`IsSealed()`), `BootCVar::Get()`은 부팅 블록 칸을 읽는다. 봉인 전 `Get()`은 Debug·Development에서 Fatal이다(출시 빌드는 지금 칸을 읽는다). 봉인 뒤에 등록한 부팅 변수는 등록 때 값(대기 값 포함)으로 부팅 블록에 굳는다. 두 번째 `CompleteStartup`은 무동작이다.
- 봉인 뒤 직접 쓰기(콘솔·코드·범위 임시 변경 층 — 콘솔 입력·이름으로 하는 대입)는 넣을 때 Error `BootSealed`로 거부한다. 메시지는 콘솔 안내 문구(`is a boot variable; set it in a config file or with --set and restart` — 이름 뒤에 붙인다)다. 다른 스레드의 쓰기는 꺼낼 때 다시 검사한다. Sync의 다시 합치기는 이 검사를 하지 않는다(봉인 전에 들어간 콘솔 값이 봉인 뒤 Sync에서 빠지지 않게). `CVar<T>::Set`·`ScopedOverride`는 `BootCVar`에 컴파일 단계에서 막혀 있다.
- 파일 층 변경은 CF3 재시작 대기(`GetPendingRestarts`·Note `RestartRequired`), 파생·휘발 층은 `BootValueIgnored` 그대로다. `UserSetting()` 부팅 변수의 사용자 층 쓰기는 대기로 오른다.
- Explain·dump 자료: `ConfigContext::DescribeConsoleSetting`(`ConsoleSettingInfo` — 이름·도움말·종류·지금 값·`boot`(`[boot]`)·재시작 대기 값(`pending restart: <값>`)). 표시는 CVR6 콘솔 도구다.
- 대기 값 늦은 적용: 등록 때 대기 값이 적용되면 Note `LateBound`(독립 콘솔 설정·CVar 모두 — 출처는 가장 강한 입력).
- `ReportUnresolved()`: 적용 중인 입력의 콘솔 이름(파일 `Settings` 멤버·콘솔 이름 대입) 중 등록되지 않은 이름마다 한 번 Warning `UnresolvedName`, 편집 거리 `MaxSuggestionDistance`(3) 이하의 가장 가까운 등록 이름(같으면 byte 순서로 앞, 출시 규칙에서 숨긴 것 제외)을 `did you mean '...'?`로 붙인다. 반환은 보고한 이름 수다. `HostSettings::CompleteStartup`이 봉인 바로 뒤에 부르고 경고를 기록한다.
- 편집 거리: Core·다른 모듈에 없어 Core 공용 `Core/Text/EditDistance.h`의 `EditDistance(left, right, limit, EditDistanceCase)`를 만들었다(AppInfoCommandLine 확정 스펙과 같은 함수 — byte 단위 OSA, 대소문자 구분 또는 ASCII 대소문자 무시, 한도 폭 띠만 계산 — 할당 없음). 한도를 넘으면 한도 + 1을 돌려주고, 한도가 `MaxEditDistanceLimit`(32)보다 크면 그 값으로 줄여 계산한다(더 먼 거리는 "멀다"로만 답한다). 전체 행렬 OSA 기준과의 전수 대조 시험을 붙였다(UE `Algo::LevenshteinDistance` 대응 — UE는 맞바꾸기가 없다).
- 진단 코드: `BootSealed`·`LateBound`·`UnresolvedName`을 더했다(CVR2의 `InvalidEnum`·CVR1의 `Clamped`·`ValidationFailed`와 함께). `CheatsDisabled`·`ReplicatedOnClient`(CVR6), `ScopeNotAllowed`(CVR3), `UndefinedLevel`·`GroupNoConverge`(CVR5)는 그 단계에서 쓰기 시작할 때 더한다(쓰지 않는 코드를 미리 두지 않는다). `ConsoleReadOnly`·`ValueOutOfRange`·`InvalidSetting`·`RestartRequired`·`BootValueIgnored`는 이미 있다.
- `TakeDiagnostics(out)`: 모은 진단을 out 뒤에 옮기고 문맥의 모음을 비운다(`ConfigDiagnostics`는 복사·이동이 없으므로 출력 인자다).

**CVR4 리뷰 반영(2026-11-19)**
- `CompleteStartup`은 큐에 남은 쓰기가 있으면 `Busy`다(같은 프레임의 쓰기가 봉인에서 빠지고 다음 Sync에서 `BootValueIgnored`·재시작 대기로 바뀌던 것). 두 번째 호출은 무동작이다.
- 봉인 뒤 바로 쓰는 층(콘솔·코드·범위 임시 변경)으로 들어온 아직 등록되지 않은 이름의 값은 그 이름을 부팅 독립 콘솔 설정(`BootCVar`·CF2 `StartupOnly`·`RestartRequired`)으로 등록할 때 받지 않고 Error `BootSealed`를 남긴다(늦은 등록으로 `BootSealed`를 우회하던 것). 뒤 Sync에서도 그 값은 닿지 않는다.
- 바로 쓰는 층의 문서 입력에 부팅 값이 하나라도 있으면 문서 전체를 거부한다(의도 — 지금 호출자 없음, 9절 표·`AddLayerText` 주석).
- `CheckConsoleLayer`는 출시 규칙의 `DevelopmentOnly` 판정을 `BootSealed`보다 먼저 한다.
- 편집 거리는 확정 스펙(AppInfoCommandLine 942행)대로 `EditDistance`(OSA·비교 방식 인자)로 바꿨다(위 항목).
- `ReportUnresolved`: 콘솔 명령 이름에 넣은 값은 같은 코드 `UnresolvedName`에 "is a console command, not a setting; it does not take a value"로 알린다(진단 코드를 늘리지 않았다 — 고칠 곳이 "값의 대상 이름"으로 같다). 문맥마다 이미 보고한 이름은 다시 보고하지 않는다(반환은 새로 보고한 수). `HostSettings`는 보고 실패도 Error 로그만 남기고 시작을 이어 간다.
- `TakeDiagnostics`: 전부 옮기거나 하나도 옮기지 않고(실패하면 out을 되돌린다), 넘친 수(`Omitted`)도 out에 더한다(`ConfigDiagnostics::AddOmitted`). out이 문맥의 모음 자신이면 `InvalidArgument`다.
- CVR6 후속: 콘솔 안내 문구의 형식 — 지금 진단은 `error BootSealed render.backend: is a boot variable…`이고 스펙(9절)은 `render.backend is a boot variable…`이다. CVR6 콘솔이 스펙 형식으로 출력한다.
- CVR6 결정 항목: 설정 struct 필드의 콘솔 이름에 대한 콘솔 입력은 지금 `BootValueIgnored`(경고)이고 독립 설정은 `BootSealed`(거부)라 콘솔 경험이 갈린다. 필드 콘솔 이름에도 콘솔 직접 쓰기 거부를 걸지(`ConfigApply.cpp`의 필드 층 검사) CVR6에서 정한다.

**CVR5 구현(2026-11-19, `feature/cvar`)**
- `Config/CVarGroup.h`: `cvar::Group(name, 기본 등급)` → `GroupBuilder`(`Help`·`Category`·`Level(등급, {항목…})`·항목 없는 `Level(등급)`) → `CVarGroup`(`Get`·`Set`·`Clear`·`LevelCount`·`LevelAt`). 묶음 변수는 int32이고 늘 `UserSetting`, 허용 값은 정의된 등급뿐이다(검사 함수 `CheckGroupLevel` → `UndefinedLevel`). 정의는 `GroupDefinition`(공통 머리 `VarDefinition`의 `HandleKind::Group` 위에 등급 표)이라 `Register`가 `const VarDefinition*`를 받는 길 그대로 묶음을 받는다(CVR1 리뷰 L5 — 공통 머리는 종류 칸이다).
- 컴파일 오류: 항목 타입 불일치(`static_assert`), `BootCVar` 항목(4.3절 문구), 같은 등급 두 번(`CVarError_DuplicateLevel`), 한 등급에 같은 변수 두 번(`CVarError_DuplicateEntry`), 기본 등급 미정의(`CVarError_DefaultLevelUndefined`), 등급 9개 이상(`CVarError_TooManyLevels`), 항목 33개 이상(`static_assert`). 항목 변수 값의 범위·`Validate`는 항목이 다른 .cpp 변수를 가리킬 수 있어(상수로 읽을 수 없다) 컴파일 때 보지 않는다(아래 등록 검사). 자기 참조·순환·항목 겹침도 컴파일 때 보지 않고 `Register`가 거부한다(리뷰 반영).
- 등록: 묶음 변수는 다른 변수처럼 칸·대기 값을 받고, 등급 표(항목 코드 값의 콘솔 표기)를 묶음 상태로 둔다. 펼침은 다음 Sync다(값은 sync 지점에서만 바뀐다 — 등록은 `dirty`만 켠다. 그래서 묶음 등록 직후 다음 Sync 전까지는 묶음 등급 칸과 항목 값이 어긋나 보인다). 묶음의 묶음 순환·자기 참조·항목 겹침은 거부하고 Error `GroupConflict`를 남긴다(이 거부만 진단을 남긴다). 묶음과 항목 변수가 둘 다 등록되면 코드 프리셋 값을 항목 변수의 규칙으로 검사한다(맞지 않으면 (묶음, 등급, 항목)마다 Error 한 번 — 펼치지 않는다).
- Sync 2단계(`ResolveGroups` — 리뷰 반영으로 10절 2단계 그대로): 앞 Sync의 펼침을 빼고, 묶음을 위상 순서(부모 → 자식)로 펼친다. 묶음마다 그때까지의 펼침을 합친 입력으로 묶음 변수의 유효 등급(요청 등급)을 계산해 Quality 층 입력(Console 종류, 태그 = 묶음 이름, 출처 `quality.shadow@2`)을 만든다. 다 펼친 뒤 조합 검증(`ResolveQualityLevels`)이 낮추면 낮춘 등급으로 처음부터 다시 펼친다(앞 차례의 결과는 요청 등급이 같은 묶음에만 쓴다). 시작점은 늘 묶음 변수의 유효 등급이라 이력에 기대지 않는다. 규칙이 없으면 펼친 등급 = 요청 등급이고 한 번에 끝난다. `MaxGroupPasses`(8)번을 넘으면 마지막 결과와 Warning `GroupNoConverge`(같은 상태에 한 번 — 요청·펼친 등급의 서명으로 판정)다. 세대는 Sync 전 값과 비교하므로 반복 중 갔다 돌아온 칸은 오르지 않는다. 항목 변수가 아직 등록되지 않았으면 펼침이 그 이름의 대기 값이 된다(등록 때 받는다 — 보류). 펼침 입력은 등록 때 검사하지 않고(대기 값 이관의 일반 진단은 내지 않는다) 묶음 검사가 같은 키로 한 번 알린다.
- `Quality.json`(branch `Quality`, `{"<묶음>": {"<등급>": {"<항목>": 값}}}`): 엔진·엔진 플랫폼·프로젝트·프로젝트 플랫폼 파일 층만 받고(그 밖은 `LayerNotAllowed`), 넣을 때는 모양만 본다(묶음은 늦게 등록될 수 있다). 등급 키는 정규 10진 표기만 받는다(`"+0"`·`"00"`은 넣을 때 `InvalidSetting`). 펼칠 때 가장 강한 파일의 값이 코드 값을 바꾼다. 항목 변수에 맞지 않는 값은 같은 (입력, 묶음, 등급, 항목)에 Error 한 번을 남기고 그 값만 건너뛴다(다음으로 강한 파일의 유효한 값, 없으면 코드 값 — 일반 층 규칙과 같다). 이 기록은 Sync가 살아 있는 입력의 것만 남긴다. 모르는 묶음(`UnresolvedName`)·정의되지 않은 등급(`UndefinedLevel`)·그 등급의 코드 항목이 아닌 이름(`InvalidSetting`)은 `ReportUnresolved`가 Warning으로 한 번씩 알린다.
- 조합 검증 연결: `ConfigContext::SetQualityValidation(규칙, 예산, 기기 특성)`(빌린다), `HostSettingsOptions::qualityRules`·`qualityBudgets`(호스트가 코드로 정한다 — `HostSettings`가 기기 특성과 함께 넘기고 `ApplyDeviceTraits` 때 프로필 적용과 무관하게 먼저 다시 넘긴다). 정의된 등급은 묶음 정의에서 오므로 `QualityGroupRule::levels`를 없애고 `QualityRequest::levels`(묶음 정의의 등급)로 옮겼다. 등록되지 않은 묶음의 규칙(`UnresolvedName`)·정의되지 않은 등급의 규칙(`UndefinedLevel`)은 `ReportUnresolved`가 Warning 한 번으로 알린다. 낮춘 등급은 묶음 변수에 쓰지 않고 펼칠 등급에만 쓴다. 결과는 `GetQualityLevels`(`QualityResolutionSet` — 요청·펼친 등급·사유·unmet·넘친 예산)와 낮춘 결과가 바뀔 때 Note `QualityLowered`(`quality.shadow@1 (requested 3, lowered: <사유>)`)다. Explain 표시는 CVR6이다.
- 대체: Config `ConsoleSettingFlags::QualityMember`·`NotQualityMember`를 없앴다(하위 호환 경로 없음). CF2 독립 콘솔 설정은 묶음 항목이 될 수 없다(항목은 CVar 핸들) — CVR7에서 CVar로 옮기면 항목이 될 수 있다. 기기 프로필은 묶음 변수(`quality.shadow`)를 그대로 콘솔 이름으로 쓴다(층 7 > 6 — VD4 순서 그대로).
- 등급 요구·비용을 어디서 정하는지(Config 6.1 CVR5 항목): 코드(호스트가 `QualityGroupRule`을 넘긴다)로 정했다. 기기 조건·예산은 기기 특성에 기대는 플랫폼별 조정이라 묶음 정의(consteval — 할당 없는 상수)에 넣기 어렵고, 데이터 파일 형식은 기기 프로필 조건과 함께 정할 일이라 미룬다(아래 후속).
- 후속: 규칙의 데이터 파일 형식(`Quality.json`이나 기기 프로필에 둘지), `ScopedOverride`의 묶음 판(CVR3 범위와 함께), 묶음 Explain·`cvar.find` 표시(CVR6).

**CVR6 구현(2026-11-19, `feature/cvar`)**
- 명령: `Config/CVarCommand.h` — `cvar::Command<&Function>(name)` → `CommandBuilder`(`Help`·`Category`·`Cheat`) → `ConsoleCommand`(정의 `CommandDefinition` = 공통 머리 `VarDefinition`의 `HandleKind::Command` + 매개변수 표 + 호출 함수). `Register`가 받고 Policy `cvar-registration` 대상이다. 출시 빌드의 `Cheat()` 명령은 이름·호출 함수를 버리고 등록되지 않는다. 명령이 객체(제어기 등)를 다뤄야 하면 `ConfigContext::BindCommand(command, target)`로 대상을 묶고 함수는 `CommandContext::Target<T>()`로 받는다(핸들은 정적이고 대상은 실행 중 객체라 묶음이 따로다). 인자는 공백으로 나눈 토큰을 매개변수마다 ReflectionText 단일 값 표기로 읽고, 마지막 `StringView` 매개변수는 줄의 나머지(비어도 된다)를 받는다. 게임 lane 입력은 넣을 때 인자를 검사한다. CF2 `ConsoleCommandDefinition`·인자 struct `<경로>=<값>` 방식은 없앴다(TraceControl `trace.*` 명령을 옮겼다 — `trace.enable cpu,gpu`·`trace.file <경로>`).
- 콘솔 도구: 15절 표 전부(`<이름>`·`?`·`<값>`·`reset`·`cvar.find`·`cvar.explain`·`cvar.dump [overrides]`·명령·자동완성). 다른 스레드 입력은 snapshot 이름 색인(`ConfigSnapshot` — 등록 뒤 다음 Sync가 게시하는 snapshot부터)으로 해석한다.
- 치트·복제: `cvar::CheatsEnabled`(`cheat.enabled`), `NetworkRole`·`SetNetworkRole`, `CheatsDisabled`·`ReplicatedOnClient`를 넣을 때(콘솔 입력·`Set({.layer = Console})`)와 Sync가 큐를 꺼낼 때 판정한다. `ExportReplicated`·`ImportReplicated`(서버 층).
- 덤프: `DumpConsoleValues(out, overridesOnly)` — 빈 문맥의 세션 층으로 다시 읽으면 같은 유효값이다(시험).
- 결정(CVR4 결정 항목): 설정 struct 필드의 콘솔 이름·필드 경로에 봉인 뒤 바로 쓰는 층(콘솔·코드·범위 임시 변경)으로 부팅 값(`RestartRequired`·`StartupOnly`)을 쓰면 독립 설정과 같이 넣을 때 `BootSealed`로 거부한다(Config 8절 개정 13). 콘솔 경험을 하나로 맞추고, Config 8절이 피하려던 것(기기 프로필 재적용의 거부 오류)은 파생 층이라 그대로 `BootValueIgnored`다. 설정 struct는 시작 전에 등록하므로 봉인 뒤 늦은 섹션 등록에 봉인 뒤 쓰기를 거르는 처리는 두지 않았다(독립 설정의 늦은 등록 처리와 다른 점 — 늦은 섹션 등록이 생기면 같은 규칙을 더한다).
- 넘긴 것: 리플렉션 struct·수학 타입 명령 인자(CU2 함수 리플렉션과 함께), 범위 값의 치트 판정(CVR3), 실제 복제 전송(넷 모듈), Release 구성의 컴파일 시점 제거 확인(19절 — 출시 구성 빌드에서, 출시 빌드 Cheat 명령의 Bind/Unbind 무동작 포함), 객체 콘솔·`CheatSet`(CVR8).

**CVR6 리뷰 반영(개정 13)**
- `cvar.explain`은 이름 하나만 받고 설명만 쓴다(값 쓰기·명령 실행으로 새지 않는다).
- 다른 스레드 입구 `ConsoleInputPort`와 참조 계수 sink `ConsoleSink`(위 15절 구현 규칙).
- 적용 때 치트 판정은 큐 순서(13.1절). 콘솔 층 문서 입력도 입력 때·적용 때 치트·복제 판정을 받는다.
- `ExportReplicated`는 복제 변수 전부(13.2절).
- 명령 대상 묶음: `BindCommand`가 대상 타입 표지를 기록하고 `CommandContext::Target<T>()`가 확인한다(다르면 Fatal). `UnbindCommand(command, expectedTarget)`은 자기 대상일 때만 지운다. 묶기·풀기는 게임 lane 전용이고 다른 스레드면 Fatal이다(대상의 파괴는 게임 lane에서 해야 한다). 문맥은 묶인 대상보다 오래 살아야 하고, 묶인 대상이 남은 채 문맥을 파괴하면 Fatal이다(대상이 소멸 때 풀어야 한다 — TraceController가 그렇게 한다). 출시 빌드의 `Cheat()` 명령은 등록되지 않으므로 Bind·Unbind는 성공하는 무동작이다.
- float·double 값의 NaN·무한대는 모든 입력 경로(CVar·독립 콘솔 설정·설정 struct 필드의 콘솔 이름)가 같은 판정으로 거부한다(4.2절). 덤프의 출처 주석은 제어 문자를 escape한다.
- `cheat.enabled`는 `ConsoleReadOnly()`(13.1절). `GetNetworkRole`은 어느 스레드에서나 부른다.

**CVR7 구현(2026-11-19, `feature/cvar`)**
- `log.*`(LogHost): 15개 이름을 핸들로 옮겼다(`LogHost.h`의 `extern` 핸들 `LogModeVar` 등, 정의는 `LogHost.cpp`, `RegisterLogSettings`가 `Register`). 실행 중 셋은 `log.level`(`CVar<LogLevel>` — ADL `CVarEnumNames(LogLevel)` 표 `trace`…`fatal`, 입력은 enum 규칙대로 대소문자 무시), `log.levels`·`log.rate_limits`(`CVar<String>` + `.Validate(CheckLogLevelsSyntax)`·`.Validate(CheckLogRateLimitsSyntax)`)다. 나머지는 `BootCVar`다: `log.mode`(`LogHostMode` enum `default`·`async`·`sync`), `log.file.format`(`LogFileFormat` enum `text`·`jsonlines`), 두 레이아웃(`.Validate(CheckLogLayoutSyntax)`), 수 셋(`Range`), bool 셋, 문자열 둘. 이름 상수(`LogLevelSetting` 등)는 핸들의 `Name()`으로 바꿔 없앴다.
- `LogHost::Start(preset, settings, hostName)`은 `settings->Acquire()`를 묶어(`cvar::SnapshotBinding`) 핸들로 읽는다. 부팅 값을 읽으므로 settings는 봉인(`CompleteStartup`) 뒤여야 하고, 아니면 `InvalidArgument`다(9절 — 봉인 전 `BootCVar::Get`은 Fatal이므로 그 전에 거부한다). `Watch(settings)`는 `OnSynced`에 하나를 더하고, 부를 때마다 settings의 snapshot을 묶어 `Watcher` 셋(level·levels·rate_limits)으로 바뀐 것만 다시 적용한다. `Stop`이 뺀다.
- 문법 오류는 이제 설정 단계에서 거부된다(Logging L17·L18). LogHost의 적용 때 검사는 문법이 아니라 적용 실패(재정의 표 용량·해시 충돌·속도 제한 표 용량)를 Warning으로 알린다. 레이아웃 해석 실패의 Error·기본 레이아웃 대체는 방어로 남긴다(같은 문법 함수라 설정 단계를 지난 값에서는 생기지 않는다). **카테고리 목록은 닫혀 있지 않다**: 카테고리는 상태 없는 상수 객체이고 실행 중 등록 표가 없어서(Logging 6절), 아직 기록하지 않은 모듈의 접두사도 정당하다. 그래서 "모르는 카테고리"는 판정할 수 없고 진단하지 않는다(CVR7 행의 "모르는 카테고리는 적용 때 진단"을 이렇게 고쳤다 — 적용 때 진단하는 것은 적용 실패다).
- `trace.*`(TraceControl): 설정 struct `TraceCaptureSettings`의 콘솔 이름 필드 여섯을 핸들로 옮겼다(`trace.pool.max_bytes`·`trace.pool.warn_percent`·`trace.recent.bytes`·`trace.writer.period_ms`·`trace.stats.interval_pumps`는 `BootCVar`, `trace.snapshot.lock_timeout_ms`는 `CVar`). 필드의 `RestartRequired`는 `BootCVar`가 됐다(사용자 층은 받지 않던 것 그대로 — `UserSetting()` 없음). 음수를 0으로 보던 처리는 `Range(0, …)`로 거부로 바꿨다(백분율은 `0..100`). `TraceCaptureSettings`는 리플렉션이 없는 값 묶음으로 남고 `ReadTraceCaptureSettings(config)`가 봉인 뒤 snapshot에서 채운다(`MakeTraceSessionSettings`는 그대로). 채널 묶음 `TracePresets`(배열 값)는 설정 struct로 남는다(CVar는 배열 타입이 없다).
- 호스트(HostConfig): `HostSettings::Initialize`가 문맥을 프로세스 기본 문맥으로 정한다(6.2절 — 묶음 없는 스레드의 `Set`이 이 문맥에 간다). 등록 순서는 그대로다(`RegisterLogSettings` → `RegisterTraceSettings` → 기기 프로필 → 호스트 등록). Sandbox `EngineHost`는 `ReadTraceCaptureSettings`로 바꿨다.
- `RegisterConsoleSetting`·`ConsoleSettingDefinition`을 없애고 `RegisterDynamic`·`DynamicVarDefinition`으로 바꿨다(5절). 문맥이 정의를 소유하는 `VarDefinition`을 만들어 핸들 등록과 같은 단계(대기 값 넘기기 → 번호 → 즉시 게시)를 지난다. 범위 검사는 정의의 범위로 한다(검사 함수는 없다).
- `Subscribe`·`SubscribeSettings`·`Unsubscribe`·`ConfigChange`·`ConfigChangeCallback`·`ConfigSubscriptionId`를 없앴다(7.3절).
- 21절 끝의 규칙 변화를 함께 적었다.

**CVR7 리뷰 반영(개정 15)**
- 동적 변수 읽기의 타입 확인: snapshot 칸에 값 종류(`ValueKind`)를 두고 `ReadDynamic`이 모든 빌드에서 확인한다(다르면 fallback). `DynamicKind`로 종류를 묻는다(5절). 핸들의 `Get()`은 타입이 컴파일 때 정해지므로 확인하지 않는다(읽기 경로 비용 그대로).
- 동적 정의의 정수 범위: 정수가 아닌 범위 끝, 그 타입의 표현 범위(int32 `[-2^31, 2^31-1]`, int64 `[-2^63, 2^63)`)를 넘는 범위 끝은 `InvalidArgument`다(double → 정수 변환의 미정의 동작과 검사 값·설명 값 불일치를 없앤다).
- 프로세스 기본 문맥: 기본 문맥 경유 쓰기 계수와 파괴 쪽 대기(6.2절), 10.1절 표 행. **리뷰 결정과 다른 점**: 결정은 "해제 뒤 들어온 묶음 없는 쓰기는 `ShuttingDown`"이었지만, 파괴와 겹친 쓰기만 `ShuttingDown`이고 기본 문맥이 풀린 뒤 들어온 쓰기는 `NotFound`로 정했다(6.2절의 까닭 — 프로세스 전역 이력이 뒤 문맥의 결과를 바꾸지 않게). 파괴와 겹친 쓰기를 시험으로 결정적으로 만들려고 `config::detail::DefaultWriteProbe`(기본 문맥 경로의 관찰 지점 — 통로를 얻은 뒤·파괴가 기본 문맥을 푼 뒤, null이면 부르지 않는다)를 두었다.
- LogHost `Watch` 기준점: `Watcher` 기준은 `Start`가 읽은 snapshot이다. `Watch`는 그 뒤 바뀐 값을 바로 한 번 적용하고(Start와 Watch 사이 Sync의 변경을 잃지 않는다) `OnSynced`를 단다. 적용이 일시적으로 실패하면(LogSystem의 `Busy` — 출력 스레드 안 호출) 그 값의 `Watcher`를 잊어(`Watcher::Reset`) 다음 Sync에 다시 시도한다. 용량 같은 지속 실패는 Warning 한 번이고 다시 시도하지 않는다.

**CVR3 구현(2026-11-19, `feature/cvar`)**
- API: `cvar::CVarScopeId`(`rocks`에도 using), `SetOptions::scope`, `CVar<T>::Get(CVarScopeId)`, `ConfigContext::CreateScope`·`DestroyScope`, 진단 코드 `ScopeNotAllowed`, `ScopedOverride`의 `CVarGroup` 판. 범위 상태·snapshot 범위 표·사슬 탐색·재연결·스레드 규칙은 8절·10.1절 그대로다.
- Sync 1단계는 범위 쓰기를 층 입력 목록에 넣지 않고, 범위 값의 사본에 큐 순서로 적용해 게시가 성공하면 맞바꾼다(값은 sync 지점에서만, 실패 때는 그대로). 범위 생성·파괴만 있어도 다음 Sync가 새 표를 게시한다.
- 치트 판정(13.1절 — CVR6가 넘긴 것): 출처 층이 콘솔인 범위 쓰기는 그 범위의 치트 허용 상태로 받을 때·적용 때(큐 순서) 판정한다.
- World 연결(14.2절): `WorldDesc::settings`·`World::CVarScope()`, `Engine::CreateWorld`가 엔진 설정 문맥을 넘긴다. World 모듈이 Config 모듈에 의존한다.
- 넘긴 것: 로컬 플레이어·Viewport 범위의 엔진 연결(그 객체가 아직 없다 — API와 코드에서 만든 자식 범위 시험까지만 했다, 자리표시 객체는 만들지 않았다), 범위 값을 실행 중 콘솔 입력으로 쓰는 문법(지금은 코드의 `Set({.scope})`만 — 콘솔은 범위 값을 보이기만 한다), 범위 값의 복제(범위 값은 지역 값이다).

**CVR3 리뷰 반영(개정 17)**
- 범위 치트 판정은 Global 실효값 AND 범위 사슬 값이다(13.1절 — 범위는 좁히기만 한다, 서버 값을 범위로 뒤집지 못한다).
- 범위 지우기: 저장된 값의 출처 층보다 약한 층의 지우기는 무시하고, 콘솔 지우기는 콘솔 읽기 전용·치트 판정을 받을 때·적용 때 받는다(6.2절).
- 범위 적용 진단은 게시가 성공해야 남고 다시 시도 때 중복되지 않는다. 파괴된 범위·미등록 이름의 쓰기는 진단 없이 버린다(8절).
- 비용: 범위 상태는 범위마다 copy-on-write, snapshot 범위 표의 노드는 참조 계수로 공유하고 바뀐 노드만 새로 만든다. 범위만 바뀐 Sync는 Global 다시 합치기를 건너뛴다(8절).
- World: 범위 파괴 실패는 Warning + Debug assert, 수명 규칙과 범위가 남은 문맥 파괴 Fatal(14.2절). `World.h`는 `Config/CVarDefinition.h`와 `ConfigContext` 전방 선언만 쓴다.

**CVR1 이후 결정·후속 항목**
- `Register`는 타입을 지운 `const cvar::detail::VarDefinition*`만 받는다. CVR5 `CVarGroup`·CVR6 `ConsoleCommand`가 같은 경로로 오려면 종류(`HandleKind`)를 가진 공통 정의 머리가 필요하다(지금 `VarDefinition`을 머리로 쓰고 종류별 꼬리를 덧붙이는 모양).
- 플러그인(모듈)을 내릴 때의 처리가 스펙에 없다: 문맥이 핸들 정의 포인터를 쥐므로, 모듈 코드가 내려가면 포인터가 dangling이 된다. `Unregister` 또는 모듈 언로드 때 그 모듈 핸들을 문맥에서 빼는 규칙을 정해야 한다(결정 필요).
  - 같은 결정에 묶인 한계: 동적 변수(`RegisterDynamic`)의 번호는 문맥이 정의를 소유해도 프로세스 카운터에서 받고 다시 쓰지 않는다. 문맥을 되풀이해 만드는 경우(에디터의 Tryout 세션처럼 실행 중 문맥을 여러 번 만들고 없앤다)에는 번호가 단조로 늘고, snapshot의 조각 표가 그만큼 길어진다(빈 조각은 공유되지 않고 포인터 칸만 는다). 번호 재사용은 `Unregister`와 함께 정한다(지금 코드는 바꾸지 않는다).
- 핸들의 `Name()` 멤버가 클래스 범위에서 타입 `Name`을 가린다. CVR2 `Get()`의 Name 반환 타입은 `rocks::Name`으로 쓴다.

## 21. 기존 콘솔 설정에서 이전

| 지금(Config CF2) | 이전 뒤 |
|---|---|
| `ConsoleSettingDefinition` + `RegisterConsoleSetting` | 정적 이름은 `CVar<T>` 핸들, 동적 이름은 `RegisterDynamic` |
| `ConsoleValue(id)`·`FindConsoleSetting(name)` | `Handle.Get()`, 도구용 이름 조회는 남긴다 |
| 플래그 `ConsoleReadOnly`·`UserConfig` | 빌더 `ConsoleReadOnly()`·`UserSetting()` |
| 플래그 `RestartRequired` | `BootCVar` + `UserSetting()`(VD15) |
| `DevelopmentOnly` | `Cheat()`(VD5). 출시 규칙의 "등록은 하되 숨김·고정"은 `releaseRules` 시험 동작으로 남고, 실제 출시 빌드는 등록하지 않는다 |
| `StartupOnly` | `BootCVar<T>` |
| `RenderRead` | 없음(모든 CVar가 snapshot에 — VD6) |
| `QualityMember` | 프리셋 항목에서 자동(VD14) |
| 이름 → `FieldValue` snapshot | 번호 조각 snapshot(설정 struct 블록은 유지) |
| 변수별 `Subscribe` 콜백 | `Watcher`·`OnSynced`(VD9) |
| 범위 임시 변경 = 콘솔 층 + 태그 | Scoped 층 |

하위 호환 경로는 두지 않는다. 쓰는 곳(LogHost·TraceControl·HostConfig·시험)을 CVR7에서 한꺼번에 바꿨다(20절 CVR7 메모).

이전 뒤의 규칙 변화(CVR7):
- 독립 콘솔 설정은 모두 CVar 칸에 든다(정적 이름은 핸들, 동적 이름은 `RegisterDynamic`). 이름 블록(snapshot의 이름으로 찾는 값)은 설정 struct 필드의 `RenderRead` 값만 담는다. 그래서 콘솔 설정 플래그 `RenderRead`는 필드에만 뜻이 있다.
- `StartupOnly`·`RestartRequired`·`DevelopmentOnly`·`UserConfig`·`ConsoleReadOnly`는 독립 설정의 정의 플래그가 아니다(동적 정의는 `cvar::VarFlags`와 `boot`). `ConsoleSettingFlags`는 설정 struct 필드의 성질과 설명(`ConsoleSettingInfo::flags`)에만 쓴다.
- 문자열 허용 값 목록이 없다. 닫힌 값 집합은 enum 변수이고 입력은 ASCII 대소문자를 무시한다(4.4절). 로깅의 `log.level`이 쓰던 `allowedValuesIgnoreCase`가 이것으로 바뀌었다.
- 검증은 `Validate`(정적 핸들 — 컴파일 때 기본값도 검사)와 범위다. 동적 변수는 범위와 타입·enum만 본다(검증 함수는 상수 평가가 필요하므로 정적 핸들에만 있다).
- 변경 알림은 `Watcher`·`OnSynced`뿐이다(7.3절).

## 22. 다른 문서 개정

| 문서 | 위치 | 개정 | 상태 |
|---|---|---|---|
| ConfigSystem | 3.1절 표·규칙 | Hotfix·Code·Scoped·Server 층, 품질 묶음 비고(VD4), "범위 임시 변경은 콘솔 층 + 태그" 규칙을 Scoped 층으로 | 반영(Config 개정 4) |
| ConfigSystem | 5.3절 | `Snapshot()` → `Acquire()`, 아무 스레드 쓰기 큐, 스레드 안전성은 이 문서 10.1절 | 반영(Config 개정 4) |
| ConfigSystem | 5절 | 이 문서로 대체한다는 안내, 플래그 표·snapshot·구독·쓰기 스레드 문단 갱신 | 반영(Config 개정 4) |
| ConfigSystem | 6.1절 | 품질 묶음 정의는 코드 `CVarGroup`, `Quality.json`은 값만 | 반영(Config 개정 4) |
| ConfigSystem | 7절 지정자 표 | `DevelopmentOnly` → `Cheat`, `StartupOnly`는 설정 struct 필드에만 | 반영(Config 개정 4) |
| UiToolkit | CU2 행 | 사용처에 치트 함수(`Cheat`·`Authority` 지정자) | 반영 |
| RenderBase | RB1 | 프레임 packet이 든 snapshot을 렌더 lane에 묶는 지점 | 반영(RenderBase 개정 13, 12절) |
| UnrealCoreFeatureAudit | 콘솔 변수·치트 행 | 이 문서 단계(CVR1–CVR8) | 반영(UE-C21·UE-C29) |
| ConfigSystem | 5절·5.1·5.3·5.4절 | CF2 구현 서술을 CVR7 뒤 동작으로(독립 설정 = CVar, `RegisterDynamic`, 구독 없앰, 호스트 등록 순서) | 반영(Config 개정 15) |
| TraceSystem | 5절 TraceControl 세션 설정 | 설정 struct `TraceCaptureSettings`(섹션 `Trace`) → `trace.*` 핸들(`BootCVar` 다섯·`CVar` 하나), 엔진 층 파일의 `"Trace"` 섹션은 이제 모르는 섹션으로 거부된다, 콘솔 명령 인자 표기(CVR6) | 반영(Trace 개정 8) |
| LoggingSystem | 9.2절 표·실행 중 변경, L17·L18, 15.1절 CVR7 행 | `log.*` 핸들 종류(enum·`BootCVar`·`Validate`), `Watch`는 `OnSynced` + `Watcher`, 설정 단계 거부 | 반영(Logging 개정 12) |

## 23. 개정 이력

- 개정 17(2026-11-19): CVR3 리뷰 반영. 13.1절 범위 치트 판정 AND, 6.2절 범위 지우기의 층·권한, 8절 copy-on-write 노드 공유·범위만 바뀐 Sync·적용 진단, 14.2절 World 범위 파괴 실패 처리·수명 규칙(범위가 남은 문맥 파괴 Fatal), 20절 리뷰 반영 메모.
- 개정 16(2026-11-19): CVR3. 6.2절 범위 쓰기(출처 층·`ScopeNotAllowed`·없는 범위의 쓰기), 6.3절 `ScopedOverride`의 묶음 판, 7.1절 `Get(scope)`, 8절 범위 계층(id·API·오류·snapshot 범위 표·빠른 길·재연결·세대), 10.1절 범위 표·범위 값 행, 13.1절 범위 값의 치트 판정, 14.2절 World 연결 구현, 20절 CVR3 메모.
- 개정 15(2026-11-19): CVR7 리뷰 반영. 5절 동적 변수 읽기의 종류 확인(`ValueKind` 칸·`DynamicKind`)·정수 범위 규칙, 6.2절 기본 문맥 경유 쓰기 계수·파괴 대기·오류 코드(`ShuttingDown`·`NotFound`), 10.1절 표 행, 20절 CVR7 행 정정·리뷰 반영 메모·번호 재사용 한계, 22절 TraceSystem 행.
- 개정 14(2026-11-19): CVR7. 5절 `RegisterDynamic`(정의·번호·읽기·오류), 6.2절 프로세스 기본 문맥(`MakeProcessDefault`, `HostSettings`), 7.3절 구독 없앰, 9절 필드 지정자 문장, 21절 이전 뒤 규칙 변화, 22절 Config·Logging 행, 20절 CVR7 메모(`log.*`·`trace.*` 핸들 종류, LogHost 읽기·`Watch`, 카테고리 목록이 닫혀 있지 않다는 정정).
- 개정 13(2026-11-19): CVR6 리뷰 반영. 13.1절(`cheat.enabled` 콘솔 읽기 전용, 적용 때 판정의 큐 순서·콘솔 층 문서), 13.2절(Export는 복제 변수 전부, `GetNetworkRole` 어느 스레드나), 15절 구현 규칙(다른 스레드 입구 `ConsoleInputPort`·참조 계수 sink·스레드 안전 요건, 공백·탭 구분·따옴표 없음, `cvar.explain` 이름 하나), 20절 CVR6 리뷰 반영 메모(명령 대상 묶음 규칙·수명, NaN·무한대, 덤프 주석 escape).
- 개정 12(2026-11-19): CVR6. 명령 정의를 `cvar::Command<&Function>(name)`으로(4.2절 — 까닭), `BindCommand`·`CommandContext::Target`, 콘솔 구현 규칙(15절 — enum 수 입력은 Config 값 검사, 내장 `cvar.*` 명령, 다른 스레드 입력의 이름 색인, 오류 문구 형식), 치트 허용 상태의 정의 위치·판정 시점(13.1절), 네트워크 역할·복제 목록 경계(13.2절), 덤프 구현(15절), 20절 CVR6 메모(설정 struct 필드의 봉인 뒤 직접 쓰기 `BootSealed` 결정 포함).
- 개정 11(2026-11-19): CVR5 리뷰 반영. 묶음의 묶음은 DAG(순환·자기 참조 `Register` 거부), 항목 겹침 금지(11절), 10절 2단계를 위상 순서 펼침·검증 낮춤 때만 다시 펼치기·같은 상태에 한 번 `GroupNoConverge`로, 코드 프리셋 값의 등록 검사, `Quality.json` 정규 등급 키·잘못된 값만 건너뛰기, `QualityRequest::levels`(`QualityGroupRule::levels` 없앰), 19절 프리셋 시험, 20절 CVR5 메모를 고쳤다.
- 개정 10(2026-11-19): CVR5 구현 결과를 20절에 적었다(`CVarGroup`·묶음 해결 반복·`Quality.json` 값 덮어쓰기·조합 검증 연결·`QualityMember` 대체). 11절의 항목 생성자(`SetArg<T>`)·정적 데이터 크기·Quality 층 직접 쓰기 거부, 12절 `QualityLowered`를 고쳤다.
- 개정 9(2026-11-19): CVR4 리뷰 반영. 9절(봉인 Busy, 늦은 등록의 `BootSealed`, 문서 입력 전체 거부, 서버 층 분류, 봉인 전 snapshot assert 범위, 부팅 값 "같다"), 10.1절 부팅 블록 행(봉인 뒤 등록의 copy-on-write), 12절(`Register`가 바로 적용, 편집 거리 OSA), 20절 리뷰 반영 메모·CVR6 후속(콘솔 문구 형식, 설정 struct 필드 콘솔 쓰기 결정 항목).
- 개정 8(2026-11-19): CVR4 구현 결과를 20절에 적었다(봉인과 부팅 블록, 직접 쓰기 `BootSealed`의 범위 — CF2 독립 콘솔 설정 포함·설정 struct 필드는 Config 규칙, `DescribeConsoleSetting`, `LateBound`, `ReportUnresolved`·Core 편집 거리, 진단 코드, `TakeDiagnostics`). 9절 CF3 비고를 고쳤다.
- 개정 7(2026-11-19): CVR2 리뷰 반영. 4.2절에 float·double의 NaN·무한대 거부를, 6.2절에 `Set`의 반환(`ErrorCode`)과 거부 이유 출력(`SetOptions::reason`)·큐 한도는 다른 스레드 쓰기에만을, 6.3절에 `ScopedOverride`의 지우기가 값을 쓴 문맥으로 가고 소멸에 실패 경로가 없음을, 7.1절에 다른 문맥에만 등록된 핸들은 기본값임을, 10.1절에 한 스레드 여러 문맥의 묶음 규칙·묶음 깊이 확인·소유 스레드만 파괴·allocator 수명을 적었다.
- 개정 6(2026-11-19): CVR2 구현 결과를 20절에 적었다(조각 snapshot·이름 블록, Initialize 빈 snapshot, 스레드 묶음·묶음 계수, `Get()`·Name 기본값, 하나의 쓰기 큐와 적용 때 재검사, 새 층 넷, Sync 순서, `Set` 오류 코드, CF2 이관 — `Snapshot()`→`Acquire()`, `Subscribe`는 CVR7, 넘긴 것). CVR1 메모의 dirty 반영을 즉시 게시로 고쳤다.
- 개정 5(2026-11-18): CVR1 리뷰 반영. 4.2절에 힙 생성 금지·Policy 거부 형태·정의 텍스트 규칙(Name 기본값 ASCII 한정으로 NFC 판정 대신)을, 10절에 `Register` 등 게임 lane API의 소유 스레드 위반은 모든 구성에서 `WrongThread`임을, 20절에 `Register`의 dirty 반영(즉시 게시는 CVR2)·리뷰 반영 내용·후속 항목(공통 정의 머리, 모듈 언로드, `rocks::Name`)을 적었다.
- 개정 4(2026-11-18): CVR1 구현 결과를 20절에 적었다(헤더 구성, CVR2 전 저장소, `Get()`을 CVR2로 둔 까닭, 스택 범위 platform API, 진단 코드 둘, 스펙과 다른 점, 다음 단계로 넘긴 컴파일 실패 사례).
- 개정 3(2026-11-18): Config CF3·CF4 병합(78d69a5d, Config 개정 5–8)에 맞췄다. 10절 Sync 순서에 sync hook(`AddSyncHook`, 큐 적용 전 0단계)을 넣고 hot reload 경로(`ConfigFileReloader` → hook → `ReplaceLayerText`·`Unset`)를 적었다. 10.1절 표에 sync hook 목록을 더했다. 9절 봉인 뒤 처리를 Config의 부팅 값 규칙과 같게 바꿨다: 직접 쓰기는 `BootSealed` 거부, 파일 층 변경은 재시작 대기(`GetPendingRestarts`, `RestartRequired`), 파생·휘발 층 변경은 지금 값 유지와 `BootValueIgnored`. 앞의 "봉인 뒤 모든 쓰기 거부"는 시작 뒤 기기 프로필 재적용마다 오류를 내고 hot reload한 파일의 다음 실행 효력을 보이지 못했다. 1·12·19절을 함께 고쳤다. 로깅 세션의 검사 함수 병합(c5fe9289)에 맞춰 CVR7 행에 `log.*` 대응(실행 중 셋, 나머지 `BootCVar`, 검사 함수 셋, enum 둘)을 적었다.
- 개정 2(2026-11-18): VD2(소문자 점 경로 이름, `r.X` 안 씀)를 사용자가 권장안으로 확정했다. 로깅 세션 조율로 CVR7 `log.*` 이관 규칙을 적고, `String`·`Name` 변수의 `Validate` 인자를 `StringView`로 정했다(`ValidateArg<T>` — 4.2절). 22절의 다른 문서 개정을 모두 반영했다(Config 개정 4, RenderBase 개정 13, UiToolkit CU2 행, UnrealCoreFeatureAudit UE-C21·UE-C29 행). 구현 착수는 다른 세션 작업 뒤 사용자가 정한다.
- 개정 1(2026-11-18): 초안. 사용자 제공 요구(타입 있는 CVar·부팅 고정·enum·레이어 스택·컨텍스트·프레임 snapshot·프리셋·지연 바인딩·치트·복제·도구)를 Config 개정 3과 엔진 원칙(전역 생성자 금지·JSON·소문자 점 이름·결정적 시험)에 맞춰 옮기고, 객체 시스템 연결(14절)을 더했다. UE 5.8.4 `IConsoleManager.h`(SetBy 순서·`ECVF_Cheat`·`TAutoConsoleVariable`)·`CheatManager`(exec 함수·확장·권한 경로)·`CheatManagerDefines.h`를 조사했다. 같은 날 리뷰어 1명의 교차 검증(clang으로 consteval 빌더·상수 초기화·Release 문자열 제거·다른 TU 주소 항목 시험)을 반영했다: Sync 순서(묶음 해결을 유효값 계산과 한 반복으로), 등록 즉시 게시, 치트 허용 상태를 CVar 하나로, Scoped < 콘솔 < Server, `RequiresRestart`를 BootCVar로 흡수, 범위 값 하나, 문자열·Name 기본값 overload, 지역 핸들 거부, `Get` overload 축소, UE 인용 정정. 사용자 요구(스레드 문제 없음)로 10.1절 스레드 안전성을 더하고 worker가 snapshot 참조를 쥐게 했다.
