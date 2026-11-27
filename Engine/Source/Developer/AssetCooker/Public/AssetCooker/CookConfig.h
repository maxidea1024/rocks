#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/ObjectRegistry.h>
#include <Platform/File.h>
#include <ReflectionText/TextDiagnostics.h>
#include <TargetProfiles/TargetProfile.h>

// 쿠킹 설정이다(Docs/Design/AssetSystem 10절, 10.2절 결정 2·3 — A4a). 대상 프로파일(TargetProfile —
// TargetProfiles 모듈, 10.3.1절 A4f1)과 쿠킹 규칙(CookSettings — `Config/CookSettings.json`)을
// ReflectionText ImportJson으로 읽는다(struct 재귀 멤버 병합·배열 통째 교체). 두 설정의 SHA-256은
// 쿠킹 매니페스트 헤더·cookKey 입력이다(NuggetKeyWriter WriteReflected — 필드 이름·타입·값).
//
// 사용 예:
//   CookConfigContext context{&allocator, &files, &registry, access, &types, engineConfig, config};
//   TargetProfile profile(allocator);
//   TextDiagnostics diagnostics(allocator);
//   CookConfigFailure failure;
//   if (auto read = ReadTargetProfile(context, "Windows", profile, diagnostics, &failure); !read) {
//       /* failure.file·field·index, diagnostics의 위치 */
//   }
//   auto settingsHash = HashTargetProfile(context, profile);
namespace rocks {

class TypeRegistry;

// PrimaryAsset 규칙 하나다(UE FPrimaryAssetTypeInfo의 타입·Directories 대응): type(클래스 도구
// 이름 — 그 타입 또는 파생)이고 directories(논리 디렉터리, 끝 '/', 하나 이상) 중 하나 아래인 에셋이
// PrimaryAsset이다.
ROCKS_STRUCT()
struct PrimaryAssetRule {
    ROCKS_GENERATED_BODY()

    explicit PrimaryAssetRule(Allocator& allocator) noexcept
        : type(allocator), directories(allocator) {
    }

    ROCKS_PROPERTY() String type;
    ROCKS_PROPERTY() Array<String> directories;
};

// 쿠킹 규칙이다(10.2절 결정 3). 경로는 논리 경로(`/Game/A/B`) 또는 디렉터리(`/Game/A/` — 끝 '/',
// 재귀)다.
ROCKS_STRUCT()
struct CookSettings {
    ROCKS_GENERATED_BODY()

    explicit CookSettings(Allocator& allocator) noexcept
        : alwaysCook(allocator), neverCook(allocator), primaryAssets(allocator) {
    }

    ROCKS_PROPERTY() Array<String> alwaysCook;
    ROCKS_PROPERTY() Array<String> neverCook;
    ROCKS_PROPERTY() Array<PrimaryAssetRule> primaryAssets;
    // Soft 참조도 쿠킹 대상 폐포에 넣는다(기본 false — bundle 구성원은 이것과 무관하다).
    ROCKS_PROPERTY() bool cookSoftReferences = false;
    // 참이면 셰이더 라이브러리 단계가 개발 전용 퍼뮤테이션을 넣는다(기본 false — AssetSystem 10.3절
    // 2항). CookSettings SHA-256에 들어가고, 단계의 cookKey도 이 값을 담는다(10.3.1절 A4f3 1번).
    ROCKS_PROPERTY() bool cookDevelopmentShaders = false;
};

// 설정 파일 크기 상한이다(TargetProfiles 설정 JSON 층과 같다).
inline constexpr usize CookConfigMaxFileBytes = ConfigJsonMaxFileBytes;
// 프로젝트 설정 디렉터리 안의 쿠킹 규칙 파일이다.
inline constexpr StringView CookSettingsFile = "CookSettings.json";

// 설정을 읽는 문맥이다. 모두 빌림이고 호출 동안 살아 있어야 한다. registry·access·types는 설정
// struct의 리플렉션 타입이 등록된 곳이다(쿠커는 에디터 Registry를 쓴다).
struct CookConfigContext {
    Allocator* allocator = nullptr;
    platform::FileSystem* files = nullptr;
    ObjectRegistry* registry = nullptr;
    ObjectAccessContext access{};
    TypeRegistry* types = nullptr;
    // 엔진 설정 디렉터리(`Engine/Config`)와 프로젝트 설정 디렉터리(`<Project>/Config`)다.
    StringView engineConfigDirectory{};
    StringView projectConfigDirectory{};
};

// 실패한 파일이다.
enum class CookConfigFile : uint8 {
    None,
    EngineProfile,
    ProjectProfile,
    CookSettings
};

// 실패한 칸이다.
enum class CookConfigField : uint8 {
    None,
    // 파일 크기 상한 초과(CapacityExceeded).
    File,
    // JSON 문법(line·column).
    Json,
    // 값 대입 진단(diagnostics — 모르는 키·타입·값).
    Text,
    Id,
    BulkAlignment,
    AlwaysCook,
    NeverCook,
    PrimaryAssetType,
    PrimaryAssetDirectory,
    // 텍스처 LOD 그룹(index = 그룹 값 — Min > Max·음수 크기·음수 maxAniso·계약
    // mipGen·OnlyFirstMip).
    TextureLodGroup,
    // 셰이더 칸(10.3절 1항 — TargetProfileField와 같은 뜻).
    GraphicsBackends,
    ShaderFormats,
    // 오디오 쿠킹 칸(Docs/Design/AudioSystem 3.39.1절 — TargetProfileField와 같은 뜻, index = 칸
    // 번호).
    AudioCook
};

// TargetProfiles 실패 칸·파일의 대응이다(같은 이름 값 1:1 — 10.3.1절).
ROCKS_ASSETCOOKER_API CookConfigField ToCookConfigField(TargetProfileField field) noexcept;
ROCKS_ASSETCOOKER_API CookConfigFile ToCookConfigFile(TargetProfileFile file) noexcept;

struct CookConfigFailure {
    // InvalidSetting(거부)이거나 None(읽기·할당 오류 — error).
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    ErrorCode error = ErrorCode::None;
    CookConfigFile file = CookConfigFile::None;
    CookConfigField field = CookConfigField::None;
    // 배열 칸의 원소 순번이다(그 밖은 0).
    uint32 index = 0;
    // PrimaryAssetDirectory의 디렉터리 순번이다(그 밖은 0).
    uint32 item = 0;
    // Json일 때 문법 오류 위치(1부터)다.
    uint32 line = 0;
    uint32 column = 0;
};

// id 프로파일을 읽는다(TargetProfiles ReadTargetProfile — 실패를 CookConfigFailure로 옮겨 담는다).
// 규칙·오류는 그 함수와 같다(텍스처 LOD 그룹 표만 키별 병합, 셰이더 칸 구조 검증).
ROCKS_ASSETCOOKER_API Result<void> ReadTargetProfile(const CookConfigContext& context,
                                                     StringView id, TargetProfile& out,
                                                     reflection_text::TextDiagnostics& diagnostics,
                                                     CookConfigFailure* failure) noexcept;

// 쿠킹 규칙을 읽는다(파일이 없으면 기본값). 거부(InvalidSetting): JSON 문법, 대입 진단, 경로(논리
// 경로·디렉터리 규칙), PrimaryAsset 타입(등록된 Asset 파생 클래스가 아님)·디렉터리. 그 밖 오류는
// ReadTargetProfile과 같다.
ROCKS_ASSETCOOKER_API Result<void> ReadCookSettings(const CookConfigContext& context,
                                                    CookSettings& out,
                                                    reflection_text::TextDiagnostics& diagnostics,
                                                    CookConfigFailure* failure) noexcept;

// 프로파일 출력 설정 SHA-256·쿠킹 규칙 SHA-256이다(WriteReflected — 칸 이름·타입·값, 칸을 더하면
// 바뀐다). 오류: 타입 미등록 NotFound, 할당 실패.
ROCKS_ASSETCOOKER_API Result<Sha256Digest> HashTargetProfile(const CookConfigContext& context,
                                                             const TargetProfile& profile) noexcept;
ROCKS_ASSETCOOKER_API Result<Sha256Digest> HashCookSettings(const CookConfigContext& context,
                                                            const CookSettings& settings) noexcept;

// 경로 항목 하나의 형식이다(alwaysCook·neverCook·PrimaryAsset 디렉터리).
enum class CookPathKind : uint8 {
    Invalid,
    // 논리 경로 하나(`/Game/A/B`).
    Asset,
    // 디렉터리(끝 '/' — `/Game/`은 마운트 전체).
    Directory
};

// text의 형식을 판정한다(디렉터리는 끝 '/'를 뗀 경로가 마운트이거나 논리 경로 규칙을 지킨다 — 규칙
// 위반은 Invalid). 오류: 할당 실패 OutOfMemory(규칙 위반과 구분한다).
ROCKS_ASSETCOOKER_API Result<CookPathKind> ClassifyCookPath(Allocator& allocator,
                                                            StringView text) noexcept;

} // namespace rocks
