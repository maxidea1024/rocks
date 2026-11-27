#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <AssetRuntime/CookedManifest.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/ObjectRegistry.h>
#include <Platform/File.h>
#include <ReflectionText/TextAssignments.h>
#include <ReflectionText/TextDiagnostics.h>
#include <Texture/TextureEnums.h>

// 대상 프로파일이다(Docs/Design/AssetSystem 10.2절 결정 2·10.3절 1항·10.3.1절 A4f1). 엔진
// `Engine/Config/TargetProfiles/<id>.json` 위에 프로젝트 `Config/TargetProfiles/<id>.json`을
// ReflectionText ImportJson으로 덮는다(struct 재귀 멤버 병합·배열과 Map 통째 교체 — 텍스처 LOD 그룹
// 표만 키별 병합 `TextMapMergeByKey`). 쿠커(AssetCooker)·셰이더 도구(`shader-compile`)·Development
// 실행 파일의 셰이더 공급자가 같은 함수로 읽는다(출처는 하나 — AssetCooker·에셋 DB를 링크하지
// 않는다).
//
// 사용 예:
//   TargetProfileContext context{&allocator, &files, &registry, access, &types, engineConfig,
//   config}; TargetProfile profile(allocator); TextDiagnostics diagnostics(allocator);
//   TargetProfileFailure failure;
//   if (auto read = ReadTargetProfile(context, "Windows", profile, diagnostics, &failure); !read) {
//       /* failure.file·field·index, diagnostics의 위치 */
//   }
namespace rocks {

class TypeRegistry;

// 쿠킹 bulk 압축이다(10절 — entry 단위, 기본 None).
ROCKS_ENUM()
enum class CookBulkCodec : uint8 {
    None = 0,
    Zstd = 1
};

// LOD 그룹의 min·mag 필터다(UE FTextureLODGroup MinMagFilter — FName point·linear·aniso,
// Docs/Design/AssetSystem 12.14절).
ROCKS_ENUM()
enum class TextureLodMinMagFilter : uint8 {
    Point = 0,
    Linear = 1,
    Aniso = 2
};

// LOD 그룹의 mip 필터다(UE MipFilter — point·linear).
ROCKS_ENUM()
enum class TextureLodMipFilter : uint8 {
    Point = 0,
    Linear = 1
};

// 텍스처 LOD 그룹 하나다(UE FTextureLODGroup — 12.4.5·12.14절, 기본값은 UE 멤버 기본값
// TextureLODSettings.h). 표에 없는 그룹은 이 기본값이다.
ROCKS_STRUCT()
struct TextureLodGroup {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 minLodSize = 1;
    ROCKS_PROPERTY() int32 maxLodSize = 4096;
    ROCKS_PROPERTY() int32 lodBias = 0;
    ROCKS_PROPERTY()
    TextureMipGenSettings mipGenSettings = TextureMipGenSettings::SimpleAverage;
    // 0이면 전역 값이다.
    ROCKS_PROPERTY() int32 maxAniso = 0;
    ROCKS_PROPERTY() bool highPriorityLoad = false;
    ROCKS_PROPERTY()
    TextureMipLoadOptions mipLoadOptions = TextureMipLoadOptions::AllMips;
    ROCKS_PROPERTY() bool cookPlatformTilingDisabled = false;
    ROCKS_PROPERTY()
    TextureLodMinMagFilter minMagFilter = TextureLodMinMagFilter::Aniso;
    ROCKS_PROPERTY() TextureLodMipFilter mipFilter = TextureLodMipFilter::Point;
    // 계약 칸(저장·전달만 — 런타임 스트리밍 단계가 쓴다): 스트리밍 mip 수(-1 = 전부), UE 5.8 품질
    // 등급별 상한(-1 = 없음).
    ROCKS_PROPERTY() int32 numStreamedMips = -1;
    ROCKS_PROPERTY() int32 maxLodSizeLow = -1;
    ROCKS_PROPERTY() int32 maxLodSizeMedium = -1;
    ROCKS_PROPERTY() int32 maxLodSizeHigh = -1;
    ROCKS_PROPERTY() int32 maxLodSizeEpic = -1;
};

// backend 하나의 셰이더 형식 목록이다(선호 순서 — 10.3절 1항). 리플렉션 Map 값은 컨테이너를 담지
// 못해 struct 하나로 감싼다(JSON `{"D3D12": {"formats": ["D3D12_SM6"]}}` — 10.3.1절).
ROCKS_STRUCT()
struct ShaderFormatList {
    ROCKS_GENERATED_BODY()

    explicit ShaderFormatList(Allocator& allocator) noexcept : formats(allocator) {
    }

    ROCKS_PROPERTY() Array<String> formats;
};

// 오디오 쿠킹 rate 칸이 받는 범위다(0은 재정의 없음 — Docs/Design/AudioSystem 3.39.1절). Audio
// 모듈의 MinPlatformSampleRate·MaxPlatformSampleRate와 같다(AudioTools가 static_assert로 맞춘다 —
// TargetProfiles는 Audio를 링크하지 않는다).
inline constexpr uint32 AudioCookMinSampleRate = 8000;
inline constexpr uint32 AudioCookMaxSampleRate = 192000;

// 플랫폼 오디오 쿠킹 재정의다(Docs/Design/AudioSystem 3.39.1절, UE FPlatformAudioCookOverrides —
// 기본값은 UE 멤버 기본값·BaseEngine.ini 플랫폼 절). rate는 0(재정의 없음)이거나
// [AudioCookMinSampleRate, AudioCookMaxSampleRate]이고, 배수는 유한하고 0 이상이다.
ROCKS_STRUCT()
struct AudioCookOverrides {
    ROCKS_GENERATED_BODY()

    // 켜면 PCM16·ADPCM을 SoundClip sampleRateQuality의 rate로 낮춰 쿠킹한다(UE bResampleForDevice —
    // Opus는 늘 48 kHz).
    ROCKS_PROPERTY() bool resampleForDevice = false;
    // sampleRateQuality(Max·High·Medium·Low·Min)별 rate 상한이다(UE PlatformSampleRates).
    ROCKS_PROPERTY() uint32 maxSampleRate = 48000;
    ROCKS_PROPERTY() uint32 highSampleRate = 32000;
    ROCKS_PROPERTY() uint32 mediumSampleRate = 24000;
    ROCKS_PROPERTY() uint32 lowSampleRate = 12000;
    ROCKS_PROPERTY() uint32 minSampleRate = 8000;
    // 모든 압축 품질에 곱한다(UE CompressionQualityModifier — floor 뒤 [1, 100]으로 자른다).
    ROCKS_PROPERTY() float compressionQualityModifier = 1.0f;
};

// AudioCookOverrides 칸 번호다(TargetProfileField::AudioCook의 index — 검증·명령 메시지가 같이
// 쓴다).
enum class AudioCookFieldIndex : uint32 {
    ResampleForDevice = 0,
    MaxSampleRate = 1,
    HighSampleRate = 2,
    MediumSampleRate = 3,
    LowSampleRate = 4,
    MinSampleRate = 5,
    CompressionQualityModifier = 6,
    Count = 7
};

// 대상 프로파일이다(10.2절 결정 2). 텍스처 칸은 12.14절(A5j), 셰이더 칸은 10.3절 1항(A4f1), 오디오
// 칸은 Docs/Design/AudioSystem 3.39절(AU5d)이다.
ROCKS_STRUCT()
struct TargetProfile {
    ROCKS_GENERATED_BODY()

    explicit TargetProfile(Allocator& allocator) noexcept
        : id(allocator), graphicsBackends(allocator), shaderFormats(allocator),
          textureLodGroups(allocator) {
    }

    // 프로파일 id다(파일 이름과 같다 — 파일에서 생략하면 읽기가 채운다).
    ROCKS_PROPERTY() String id;
    // 런타임이 시도하는 backend 순서다(비면 셰이더 없는 프로파일 — 장치·렌더러를 만들지 않는다).
    ROCKS_PROPERTY() Array<String> graphicsBackends;
    // backend → 형식 목록이다(JSON은 통째 교체 — backend 목록과 반쪽만 덮여 엇갈리지 않게).
    ROCKS_PROPERTY() OrderedMap<String, ShaderFormatList> shaderFormats;
    ROCKS_PROPERTY() CookBulkCodec bulkCodec = CookBulkCodec::None;
    // 쿠킹 패키지 bulk 정렬이다(0이면 쿠킹 기본 4096, 아니면 2의 거듭제곱 8–65536).
    ROCKS_PROPERTY() uint32 bulkAlignment = 0;
    // 텍스처 LOD 그룹 표다(JSON은 키별 병합 — 프로젝트 파일이 그룹 하나의 칸 하나만 덮는다).
    ROCKS_PROPERTY(TextMapMergeByKey)
    OrderedMap<TextureGroup, TextureLodGroup> textureLodGroups;
    // R16·RG16·RGBA16 Unorm 텍스처를 표본할 수 있는지다(false면 같은 채널 수의 16F — Android).
    ROCKS_PROPERTY() bool textureNorm16Supported = true;
    // 오디오 쿠킹 재정의다(JSON은 칸별 병합 — 프로젝트 파일이 칸 하나만 덮는다).
    ROCKS_PROPERTY() AudioCookOverrides audioCook;
};

// group의 그룹 값이다(표에 없으면 기본값).
ROCKS_TARGETPROFILES_API TextureLodGroup FindTextureLodGroup(const TargetProfile& profile,
                                                             TextureGroup group) noexcept;

// 설정 파일 크기 상한·프로파일 id 길이 상한이다(형식 id 길이 상한 ShaderFormatIdMaxBytes와
// 어휘·문법 함수는 매니페스트와 같은 출처 — AssetRuntime CookedManifest.h, 10.3.1절 A4f2 2번).
inline constexpr usize ConfigJsonMaxFileBytes = 1024 * 1024;
inline constexpr usize TargetProfileIdMaxBytes = 64;
// 엔진·프로젝트 설정 디렉터리 안의 상대 경로다.
inline constexpr StringView TargetProfileDirectory = "TargetProfiles";

// 설정 JSON 한 층을 읽은 결과다(실패한 단계와 JSON 문법 위치).
enum class ConfigJsonStage : uint8 {
    None,
    // 파일 크기 상한 초과(CapacityExceeded)·읽기 오류.
    File,
    // JSON 문법(line·column).
    Json,
    // 값 대입 진단(diagnostics).
    Text
};

struct ConfigJsonOutcome {
    // 파일이 있었는지다(없으면 아무것도 하지 않고 성공).
    bool existed = false;
    ConfigJsonStage stage = ConfigJsonStage::None;
    uint32 line = 0;
    uint32 column = 0;
};

// path의 JSON을 target에 대입한다(설정 층 하나 — 기본 TextCodecOptions, Map 병합은 필드 플래그만).
// 파일이 없으면 outcome.existed = false로 성공한다. 오류: 크기 상한 CapacityExceeded·읽기
// 오류(stage File), JSON 문법 InvalidArgument(stage Json·line·column), 대입 진단
// InvalidArgument(stage Text — diagnostics), 게시·할당 실패(stage None). 실패하면 target은 바뀌지
// 않는다(대입 묶음은 게시 전에 버려진다).
ROCKS_TARGETPROFILES_API Result<void>
ApplyConfigJsonFile(Allocator& allocator, platform::FileSystem& files, StringView path,
                    const reflection_text::AssignmentTarget& target,
                    reflection_text::TextDiagnostics& diagnostics,
                    ConfigJsonOutcome& outcome) noexcept;

// 프로파일을 읽는 문맥이다. 모두 빌림이고 호출 동안 살아 있어야 한다. registry·access·types는
// TargetProfile 리플렉션 타입이 등록된 곳이다(ReflectionText import가 Registry를 요구한다 — 셰이더
// 도구도 ObjectRegistry 하나를 만든다).
struct TargetProfileContext {
    Allocator* allocator = nullptr;
    platform::FileSystem* files = nullptr;
    ObjectRegistry* registry = nullptr;
    ObjectAccessContext access{};
    TypeRegistry* types = nullptr;
    // 엔진 설정 디렉터리(`Engine/Config`)와 프로젝트 설정 디렉터리(`<Project>/Config` — 비면
    // 없음)다.
    StringView engineConfigDirectory{};
    StringView projectConfigDirectory{};
};

// 실패한 파일이다.
enum class TargetProfileFile : uint8 {
    None,
    EngineProfile,
    ProjectProfile
};

// 실패한 칸이다(AssetCooker CookConfigField의 같은 이름 값과 1:1).
enum class TargetProfileField : uint8 {
    None,
    File,
    Json,
    Text,
    Id,
    BulkAlignment,
    // 텍스처 LOD 그룹(index = 그룹 값 — Min > Max·음수 크기·음수 maxAniso·계약
    // mipGen·OnlyFirstMip).
    TextureLodGroup,
    // graphicsBackends(index = 번호 — 어휘 밖·중복·상한 초과는 상한 번호).
    GraphicsBackends,
    // shaderFormats(index = backend 번호 — 형식 목록 없음·빔·형식 수 상한·형식 id 문법·전역 중복,
    // 단 목록에 없는 키는 Map 순서 번호, 셰이더 없는 프로파일의 빈 Map 아님은 0 — 판정
    // 순서는 10.3.1절).
    ShaderFormats,
    // audioCook(index = AudioCookFieldIndex — ResampleForDevice는 검사하지 않는다, Docs/Design/
    // AudioSystem 3.39.1절).
    AudioCook
};

struct TargetProfileFailure {
    // InvalidSetting(거부)이거나 None(읽기·할당 오류 — error).
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    ErrorCode error = ErrorCode::None;
    TargetProfileFile file = TargetProfileFile::None;
    TargetProfileField field = TargetProfileField::None;
    uint32 index = 0;
    // Json일 때 문법 오류 위치(1부터)다.
    uint32 line = 0;
    uint32 column = 0;
};

// id 프로파일을 읽는다: 엔진 파일(필수) → 프로젝트 파일(있으면) 순으로 대입하고 검증한다. out은
// 기본값에서 시작한다(id는 미리 채운다). 오류(InvalidArgument — failure의 code InvalidSetting): id
// 형식(`[A-Za-z0-9_]`, 1–64 byte), 엔진 파일 없음(error NotFound도 InvalidSetting), JSON 문법, 대입
// 진단(diagnostics), 파일의 id가 다름, bulkAlignment, 텍스처 LOD 그룹(12.14절), 셰이더 칸(10.3절
// 1항 구조 검증), 오디오 쿠킹 칸(rate 범위·배수 — AudioSystem 3.39.1절). 파일 크기 상한 초과
// CapacityExceeded, 읽기 오류는 그 오류, 할당 실패. 실패하면 out은 정해지지 않는다.
ROCKS_TARGETPROFILES_API Result<void>
ReadTargetProfile(const TargetProfileContext& context, StringView id, TargetProfile& out,
                  reflection_text::TextDiagnostics& diagnostics,
                  TargetProfileFailure* failure) noexcept;

// 호스트 플랫폼 프로파일을 읽는다(10.3.1절 4번): platform이 비지 않았으면 그 이름(HostSettings
// 플랫폼 이름 재정의), 비면 HostConfig HostPlatformName()(Windows·Mac·Android)이다. 오류는
// ReadTargetProfile과 같다(엔진 프로파일 파일이 없는 플랫폼은 엔진 파일 없음).
ROCKS_TARGETPROFILES_API Result<void>
ReadHostTargetProfile(const TargetProfileContext& context, StringView platform, TargetProfile& out,
                      reflection_text::TextDiagnostics& diagnostics,
                      TargetProfileFailure* failure) noexcept;

// 검증한 프로필의 셰이더 칸을 매니페스트 backend 표로 바꿔 out을 채운다(10.3절 4항 —
// graphicsBackends 순서, backend마다 shaderFormats 순서). ReadTargetProfile을 통과한 프로필은
// 언제나 검증을 통과하는 표를 만든다(같은 상한·어휘·문법).
//
// 오류: graphicsBackends의 backend가 shaderFormats에 없으면 InvalidArgument(검증하지 않은 프로필),
// 할당 실패는 그 오류다. 실패하면 out은 정해지지 않는다.
ROCKS_TARGETPROFILES_API Result<void>
MakeCookedManifestBackends(Allocator& allocator, const TargetProfile& profile,
                           Array<CookedManifestBackend>& out) noexcept;

} // namespace rocks
