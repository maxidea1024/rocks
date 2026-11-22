#pragma once

#include <Config/ConfigContext.h>
#include <Core/Text/Json.h>
#include <Platform/File.h>

// 사용자 층(Docs/Design/ConfigSystem.ko.md 3.1·8절 — 설정 CF3): 사용자 설정 쓰기, 사용자 파일의
// 관대한 읽기(오래된 파일의 모르는 키는 경고 후 버린다), 아래 층과의 차이만 쓰는 저장(원자적 교체).
// UE 대응: `GConfig`의 저장 층과 `SaveConfig`(아래 층을 합친 값과의 차이만 쓴다 — UE
// `Core/Private/Misc/ConfigCacheIni.cpp`의 BuildDiffOutputString·SaveBranch).
//
// 사용자 파일 형식(`<userDataDir>/<projectName>/Config/<Branch>.json`, Core canonical JSON — 주석
// 없음, 들여쓰기 2칸, LF):
//   {
//     "$version": 1,                              // UserConfigSchemaVersion
//     "RenderGraph": { "maxPasses": 32 },         // 섹션(이름 byte 순서) — 바뀐 UserConfig 필드만
//     "Settings": { "game.input.sensitivity": 2 } // 독립 콘솔 설정(이름 byte 순서)
//   }
//
// 사용 예(설정 화면 — 게임 lane):
//   SetUserValue(context, "Render.RenderGraph.maxPasses", "32");
//   context.Sync();                                       // 값이 바뀐다
//   SaveUserConfig(context, platform::PhysicalFileSystem());
namespace rocks::config {

// 사용자 파일의 스키마 버전이다. 읽을 때 다르거나 없으면 경고(UserFileVersion)하고, 키마다 검사해
// 모르는 것만 버린다.
inline constexpr int64 UserConfigSchemaVersion = 1;

// 사용자 파일의 스키마 버전 키다(`$`로 시작하므로 섹션 이름과 겹치지 않는다).
inline constexpr StringView UserConfigVersionKey = "$version";

// 실행 중 사용자 설정 변경(SetUserValue)의 층 태그다. 파일에서 읽은 입력의 태그는 Config 폴더 기준
// 파일 이름(`Render.json`)이다.
inline constexpr StringView UserChangeTag = "user";

// 사용자 파일 내용 하나(branch 파일)를 관대하게 읽어 사용자 층(ConfigLayer::User)에 넣는다. 오래된
// 사용자 파일을 위해 조각(최상위 키, 섹션의 멤버, `Settings`의 이름)마다 검사하고, 받을 수 없는
// 조각은 버리며 그 원문 위치로 Warning 진단을 남긴다: JSON 문법·모양 오류(파일 전체), 모르는 섹션
// UnknownSection, 모르는 필드 FieldValue(UnknownProperty), UserConfig가 아닌 필드·콘솔 설정
// LayerNotAllowed, 값 형식 FieldValue·InvalidSetting, 등록되지 않은 콘솔 설정 UnknownSetting, 버전
// UserFileVersion. 남은 조각은 하나의 입력으로 넣는다(tag·sourceName은 AddLayerText와 같다).
// replace면 그 앞에 Unset(User, tag)를 넣는다(hot reload — 남은 조각이 없으면 Unset만 넣는다).
// 값은 다음 Sync에 바뀐다. 설정 struct·콘솔 설정은 먼저 등록해 둔다(등록 전 이름은 모르는 키다).
// 오류: 초기화 전 InvalidArgument, 모르는 branch NotFound, 다른 스레드 WrongThread, 할당 실패.
// 버린 조각은 오류가 아니다.
ROCKS_CONFIG_API Result<void> AddUserFileText(ConfigContext& context, StringView tag,
                                              StringView branch, StringView sourceName,
                                              StringView text, bool replace = false) noexcept;

// 사용자 설정 값 하나를 사용자 층에 쓴다(설정 화면의 변경 — AddAssignment(User, UserChangeTag,
// target, value)). target은 `<Branch>.<Section>.<필드 경로>` 또는 콘솔 설정 이름이고 UserConfig
// 필드·설정이어야 한다. 값은 다음 Sync에 바뀐다(RestartRequired 값은 시작 뒤면 재시작 대기가
// 된다). 오류: AddAssignment와 같다(UserConfig가 아니면 InvalidArgument — 진단 LayerNotAllowed).
ROCKS_CONFIG_API Result<void> SetUserValue(ConfigContext& context, StringView target,
                                           StringView value) noexcept;

// branch 하나의 사용자 파일 내용을 writer에 객체 하나로 쓴다(Finish는 호출자가 한다): 사용자 층까지
// 합친 값과 그 아래 층(Engine … DeviceProfile)만 합친 값이 다른 UserConfig 필드·독립 콘솔 설정만
// 쓴다. 세션·콘솔 층 값은 쓰지 않는다. 마지막 Sync의 입력을 쓴다(큐는 보지 않는다). 반환: 쓴 값
// 수(0이면 `$version`만 있는 객체다). 오류: 초기화 전 InvalidArgument, 모르는 branch NotFound, 다른
// 스레드 WrongThread, ExportJson·writer의 오류.
ROCKS_CONFIG_API Result<uint32> WriteUserConfig(ConfigContext& context, StringView branch,
                                                JsonWriter& writer) noexcept;

// 사용자 층 폴더(`<userDataDir>/<projectName>/Config`)를 out에 쓴다. 오류: 봉인 모드(사용자 층이
// 없다)·userDataDir·projectName이 비었으면 Unsupported, 초기화 전 InvalidArgument, 할당 실패.
Result<void> UserConfigDirectory(ConfigContext& context, String& out) noexcept;

// SaveUserConfig의 결과다.
struct UserConfigSaveResult {
    // 쓴 파일 수(값이 있는 branch).
    uint32 written = 0;
    // 지운 파일 수(값이 없어진 branch의 옛 파일 — UE SaveBranch처럼 빈 파일을 남기지 않는다).
    uint32 removed = 0;
    // 쓴 값 수(모든 branch 합).
    uint32 values = 0;
};

// 모든 branch의 사용자 파일을 쓴다(WriteUserConfig). 값이 있는 branch는 임시 파일
// `<Branch>.json.tmp`에 쓰고 Flush한 뒤 FileSystem::Replace로 원자적으로 바꾼다(전원이 끊겨도 옛
// 파일이나 새 파일 둘 중 하나다). 값이 없는 branch의 옛 파일은 지운다. 폴더가 없으면 만든다.
// 출시·개발 빌드 모두 저장한다(사용자 설정은 출시 기능이다). 오류: UserConfigDirectory의 오류,
// WriteUserConfig의 오류, 파일 연산의 오류(FileErrorCode — 그 branch에서 멈춘다. 앞 branch는 이미
// 바뀌었고 그 branch의 옛 파일은 그대로다).
ROCKS_CONFIG_API Result<UserConfigSaveResult> SaveUserConfig(ConfigContext& context,
                                                             platform::FileSystem& files) noexcept;

} // namespace rocks::config
