#pragma once

#include <Meta/TypeSchema.h>

// 설정 struct 메타데이터다(Docs/Design/ConfigSystem.ko.md 7절). `ROCKS_STRUCT(Settings(Branch="…",
// Section="…"))`로 선언한 struct는 설정 계층의 섹션 하나다: branch 파일(`Render.json`)의 최상위 키
// Section이 이 struct 값으로 읽힌다. MetaCompiler가 그 struct의 GeneratedType에
// `static const SettingsSchema& Settings() noexcept`를 더 낸다(UE `UCLASS(config=…)`·
// `UDeveloperSettings` 대응). 필드 설정 지정자(UserConfig·ConsoleName·RestartRequired·StartupOnly·
// RenderRead·DevelopmentOnly)는 설정 struct의 직접 멤버에만 쓸 수 있고 SettingsFieldSchema로
// 실린다. 설정 계층(Config 모듈)이 이 표를 읽는다. 저장 형식·TypeSchema와는 무관하다(FieldFlags
// 비트를 쓰지 않는다).
//
// 사용 예:
//   ROCKS_STRUCT(Settings(Branch = "Render", Section = "RenderGraph"))
//   struct RenderGraphSettings {
//       ROCKS_PROPERTY(ConsoleName = "render.graph.immediate", RenderRead) bool immediate = false;
//       ROCKS_PROPERTY(UserConfig) int32 maxPasses = 64;
//   };
//
//   const SettingsSchema& settings = GeneratedType<RenderGraphSettings>::Settings();
//   // settings.branch == "Render", settings.section == "RenderGraph", settings.fields.Size() == 2
namespace rocks {

// 설정 필드 지정자 비트다. 한 필드의 효력 시점(RestartRequired·StartupOnly)은 둘 중 하나만 쓴다
// (MetaCompiler RMC2210). 지정자가 없으면 즉시 적용·사용자 층 쓰기 불가다.
enum class SettingsFieldFlags : uint32 {
    None = 0,
    // 사용자 층(ConfigLayer::User)이 쓸 수 있다(UE GlobalUserConfig 계열).
    UserConfig = 1,
    // 바뀐 값이 다음 시작 때 효력을 갖는다(UE ConfigRestartRequired). 표시·저장은 CF3이다.
    RestartRequired = 2,
    // 시작이 끝난 뒤에는 바꿀 수 없다.
    StartupOnly = 4,
    // 설정 snapshot에 넣는다(렌더 lane이 읽는 값 — UE ECVF_RenderThreadSafe).
    RenderRead = 8,
    // 출시 규칙에서 코드 기본값으로 고정한다(UE ECVF_Cheat).
    DevelopmentOnly = 16
};

// SettingsFieldFlags의 알려진 비트 전부다.
inline constexpr uint32 KnownSettingsFieldFlags =
    static_cast<uint32>(SettingsFieldFlags::UserConfig) |
    static_cast<uint32>(SettingsFieldFlags::RestartRequired) |
    static_cast<uint32>(SettingsFieldFlags::StartupOnly) |
    static_cast<uint32>(SettingsFieldFlags::RenderRead) |
    static_cast<uint32>(SettingsFieldFlags::DevelopmentOnly);

constexpr bool HasFlag(uint32 flags, SettingsFieldFlags flag) noexcept {
    return (flags & static_cast<uint32>(flag)) != 0;
}

// 설정 struct 직접 멤버 하나의 설정 지정자다. field는 그 멤버의 FieldSchema::id다. consoleName은
// 비어 있거나 콘솔 설정 이름(소문자 점 경로 `render.graph.immediate` — 영역 이름 공간 표 판정은
// 설정 계층이 등록 때 한다)이다. 이름은 생성 코드의 정적 문자열을 빌린다.
struct SettingsFieldSchema {
    Guid field{};
    uint32 flags = 0;
    Span<const char> consoleName{};
};

// 설정 struct 하나의 설정 메타데이터다. branch는 파일 단위 이름(`Render`), section은 그 파일의
// 최상위 키(`RenderGraph`)다. fields는 설정 지정자가 하나라도 있는 직접 멤버만 스키마 필드 순서로
// 담는다. 모든 view는 생성 코드의 정적 저장소를 빌리므로 그 모듈이 살아 있는 동안 유효하다.
struct SettingsSchema {
    Span<const char> branch{};
    Span<const char> section{};
    Span<const SettingsFieldSchema> fields{};
};

} // namespace rocks
