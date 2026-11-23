#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Text/StringView.h>
#include <ShaderToolTable/ShaderToolTable.h>

// 셰이더 형식 정의와 엔진 컴파일 프로필이다(Docs/Design/ShaderSystem 12.1절, P6a).
//
// 형식 정의(Engine/Config/ShaderFormats/<id>.json)는 한 셰이더 형식(D3D12_SM6 등)을 만드는 데
// 필요한 모든 값의 단일 출처다: 쓰는 도구 목록(job 키 3항의 정적 목록), HLSL 2021 기본값(결정 2),
// 컴파일 경로와 셰이더 모델·target env·ESSL·MSL 버전(결정 3), 바인딩 규약 버전, ShaderFormat 처리기
// 버전, 상한(7.2절 4항)·지원 기능·장치 요구, 생산 호스트 규칙(결정 5). 파일 byte의 SHA-256이 job 키
// 3항에 들어가므로(10절) 파일의 어떤 byte가 바뀌어도 그 형식의 키가 바뀐다(.gitattributes가
// *.json을 LF로 고정한다).
//
// 엔진 컴파일 프로필(Engine-<호스트>.json)은 프로젝트 프로필 없이 하는 컴파일(엔진 내장 라이브러리,
// Development 필요 시 컴파일)이 쓰는 호스트별 형식 목록이다. 런타임 backend 선택이 없으므로
// TargetProfile과 스키마가 다르다.
//
// 읽기 규칙: 모르는 속성·모르는 어휘·범위 밖 값은 오류다(조용히 무시하지 않는다). 도구 이름은 도구
// 표(ShaderToolTable)에 있어야 한다. 자리만 둔 형식(status "placeholder",
// VULKAN_ANDROID·METAL_IOS)은 schema·id·status·backend만 가지며 프로필에 넣을 수 없다.
//
// 사용 예(메인 프로세스, 엔진 내장 셰이더 컴파일):
//   EngineCompileProfile profile;
//   if (const ShaderFormatStatus loaded =
//           LoadEngineCompileProfile(ShaderFormatDirectory, "Engine-Windows", profile);
//       !loaded) {
//       printf("%s: %s (line %u)\n", ShaderFormatErrorName(loaded.code), loaded.path, loaded.line);
//   }
//   for (const ShaderFormatDefinition& format : profile.Formats()) {
//       // 상한은 job 형식마다 따로 검사한다(7.2절 4항): format.limits.
//   }
//
// 스레드: 모든 함수는 재진입 가능하며 전역 상태를 쓰지 않는다. 결과 객체는 값 형식이고 도구 표
// 항목(정적 수명)만 가리킨다.
namespace rocks::shader {

// 형식 정의·엔진 컴파일 프로필 스키마 버전이다("schema"). 다른 값은 UnsupportedSchema다.
inline constexpr uint32 ShaderFormatSchemaVersion = 1;
// 저장소 루트 기준 형식 정의 디렉터리다.
inline constexpr char ShaderFormatDirectory[] = "Engine/Config/ShaderFormats";
// 형식 id·프로필 id의 최대 byte 수(NUL 포함)다.
inline constexpr usize MaxShaderFormatIdBytes = 32;
inline constexpr usize MaxShaderFormatTools = 8;
inline constexpr usize MaxShaderFormatProducers = 4;
inline constexpr usize MaxEngineProfileFormats = 8;
// 오류 위치(JSON 경로 또는 파일 이름)의 최대 byte 수(NUL 포함)다. 넘는 부분은 잘린다.
inline constexpr usize MaxShaderFormatErrorPathBytes = 96;
// 루트 상수 크기 상한(7.2절 2항, Vulkan push constant 최소 보장 128 byte)이다.
inline constexpr uint32 MaxRootConstantBytes = 128;
// 단계 사이 vec4 상한(7.2절 7항, TEXCOORD0–15)과 render target 상한(SV_Target0–7)이다.
inline constexpr uint32 MaxInterStageVectors = 16;
inline constexpr uint32 MaxRenderTargets = 8;
// 상한이 없다는 표지다(uavTotal이 없는 형식, uniform 에뮬레이션이 없는 형식의 uniformVectors).
inline constexpr uint32 NoShaderLimit = NumericLimits<uint32>::Max();

enum class ShaderBackend : uint8 {
    D3D12,
    D3D11,
    Vulkan,
    Metal,
    // OpenGL ES backend다(UE EWindowsRHI::OpenGL과 같은 이름, 12.3절).
    OpenGL
};

enum class ShaderFormatState : uint8 {
    Supported,
    // 자리만 둔 형식(2절 Unsupported). 컴파일 필드가 없고 프로필에 넣을 수 없다.
    Placeholder
};

// ShaderFormat 처리기(워커 안 형식 모듈) 이름이다. 처리기 버전과 함께 job 키에 들어간다.
enum class ShaderFormatModule : uint8 {
    D3D,
    Vulkan,
    Metal,
    OpenGL
};

// 컴파일 경로(2절 형식 표)다.
enum class ShaderCompilerPath : uint8 {
    None,
    // DXC → DXIL(내장 validator 서명).
    DxcDxil,
    // FXC → DXBC SM 5.0.
    Fxc,
    // DXC → SPIR-V → SPIRV-Cross HLSL 5.0 → FXC(D3D11_SM5의 HLSL 2021·FXC 실패 셰이더).
    DxcSpirvHlsl50Fxc,
    // DXC → SPIR-V(재배치·spirv-val).
    DxcSpirv,
    // DXC → SPIR-V → SPIRV-Cross MSL → metal·metallib.
    DxcSpirvMsl,
    // DXC → SPIR-V → SPIRV-Cross ESSL → 상수 버퍼 에뮬레이션 재작성.
    DxcSpirvEssl
};

// ROCKS_IMAGE_FORMAT(<형식>) 전개(7.2절 1항)다. SH0에서 DXC가 [[vk::image_format]]을 지원하고
// DXIL에서는 무시하며 경고를 내는 것을 확인했으므로 D3D 경로는 빈 전개만 받는다.
enum class ImageFormatExpansion : uint8 {
    Empty,
    VkImageFormat
};

enum class SpirvTargetEnv : uint8 {
    None,
    Vulkan1_1
};

enum class MslPlatform : uint8 {
    None,
    MacOS
};

// 도구를 돌리는 개발 호스트다(binary manifest 호스트 키와 같은 철자: windows-x64·macos-arm64).
enum class ShaderHost : uint8 {
    WindowsX64,
    MacosArm64
};

// 단계 비트("stages"·"uavStages").
enum ShaderStageBits : uint32 {
    ShaderStageVertex = 1u << 0,
    ShaderStagePixel = 1u << 1,
    ShaderStageCompute = 1u << 2
};

// 지원 기능 비트("features"). "이 형식이 그 기능을 쓰는 셰이더를 컴파일한다"는 뜻이다. 셰이더
// 타입이 요구한 기능이 없으면 그 형식에서 쿠킹하지 않는다(2절, 12.2절 거르기).
//
// 기능은 두 범주다(15절 결정 12).
// - 장치 보장 기능: 형식의 deviceRequirements가 보장하므로 그 형식을 고른 장치는 늘 쓸 수 있다.
// - 런타임 확인 기능(RuntimeCheckedShaderFeatures): 같은 형식 안에서도 장치마다 다르다. 런타임
//   형식 선택은 이 기능으로 장치를 거르지 않고, 셰이더 집합이 장치 능력(Graphics DeviceCaps)과
//   대조해 없는 장치에서 그 셰이더 생성을 Unsupported로 막는다. 호출자(렌더러)가 대체 경로를
//   고른다.
enum ShaderFeatureBits : uint32 {
    ShaderFeatureCompute = 1u << 0,
    // SM 6 웨이브 연산(2절, UE EShaderOptionalDataKey::ShaderModel6에 해당).
    ShaderFeatureWaveOperations = 1u << 1,
    // VS 출력 SV_RenderTargetArrayIndex·SV_ViewportArrayIndex(7.2절 7항). 런타임 확인 기능이다.
    // 장치 쪽 값은 DeviceCaps의 vertexViewportLayerOutput이다.
    ShaderFeatureVertexViewportLayerOutput = 1u << 2
};

// 런타임 확인 기능 비트 묶음이다. 나머지 기능은 장치 보장 기능이다. 셰이더 라이브러리 항목은
// 요구 기능 가운데 이 비트만 담는다(12.2절).
inline constexpr uint32 RuntimeCheckedShaderFeatures = ShaderFeatureVertexViewportLayerOutput;

// feature 비트 하나가 런타임 확인 기능인가.
constexpr bool IsRuntimeCheckedShaderFeature(uint32 feature) noexcept {
    return (feature & RuntimeCheckedShaderFeatures) != 0;
}

// 장치 요구 비트("deviceRequirements"). 런타임 backend가 장치를 고를 때 대조한다.
enum ShaderDeviceRequirementBits : uint32 {
    DeviceRequiresD3DFeatureLevel11_0 = 1u << 0,
    DeviceRequiresD3D12ShaderModel6_6 = 1u << 1,
    DeviceRequiresVulkan1_1 = 1u << 2,
    DeviceRequiresOpenGL4_3 = 1u << 3,
    DeviceRequiresOpenGLES3_2 = 1u << 4,
    DeviceRequiresGlArbEs32Compatibility = 1u << 5,
    DeviceRequiresGlArbClipControl = 1u << 6
};

// 셰이더 모델·언어 버전(major.minor)이다. 셰이더 모델 6.6은 {6, 6}, MSL 2.4는 {2, 4}다.
struct ShaderVersion {
    uint8 major = 0;
    uint8 minor = 0;

    friend constexpr bool operator==(ShaderVersion, ShaderVersion) noexcept = default;
};

// 컴파일러 설정("compiler")이다. 경로에 쓰이지 않는 필드는 기본값이다.
struct ShaderFormatCompiler {
    ShaderCompilerPath path = ShaderCompilerPath::None;
    // D3D11_SM5의 대체 경로(DxcSpirvHlsl50Fxc)다. 그 밖은 None.
    ShaderCompilerPath fallbackPath = ShaderCompilerPath::None;
    // DXC·FXC 프로필의 셰이더 모델(D3D12 6.6, D3D11 5.0, SPIR-V 경로 6.0).
    ShaderVersion shaderModel;
    ImageFormatExpansion imageFormat = ImageFormatExpansion::Empty;
    // 이 형식이 받지 않는 ROCKS_IMAGE_FORMAT 형식(ImageFormatName 색인의 비트). ESSL 320은
    // r11g11b10f를 받지 않는다(SH0).
    uint64 unsupportedImageFormats = 0;
    SpirvTargetEnv spirvTargetEnv = SpirvTargetEnv::None;
    // -fvk-use-dx-position-w(7.2절 8항).
    bool dxPositionW = false;
    // ESSL #version 숫자(320).
    uint32 esslVersion = 0;
    // SPIRV-Cross fixup_clipspace·flip_vert_y(7.2절 8항, GLES31_ANDROID만 켠다).
    bool fixupClipSpace = false;
    bool flipVertexY = false;
    ShaderVersion mslVersion;
    MslPlatform mslPlatform = MslPlatform::None;
};

// 단계별 상한(7.2절 4항)이다. 모든 상한은 job 형식의 이 값으로 검사한다(프로필 공통 상한은 없다).
// 핵심 최소 보장을 넘는 값은 장치 요구이며, 런타임 형식 선택이 장치 능력과 대조한다.
struct ShaderFormatLimits {
    uint32 constantBuffers = 0;
    uint32 sampledTextures = 0;
    uint32 storageBuffers = 0;
    uint32 storageImages = 0;
    uint32 samplers = 0;
    // 파이프라인 전체 UAV 수(D3D Tier 1 FL 11.0의 8). 없으면 NoShaderLimit.
    uint32 uavTotal = NoShaderLimit;
    // UAV(구조화 버퍼 쓰기·storage image)를 쓸 수 있는 단계 비트. 데스크톱 공통은 CS만이다.
    uint32 uavStages = 0;
    uint32 renderTargets = 0;
    uint32 interStageVectors = 0;
    uint32 rootConstantBytes = 0;
    // GLES 상수 버퍼 에뮬레이션의 단계별 vec4 합계 상한(VS·PS·CS 순). 에뮬레이션이 없으면
    // NoShaderLimit.
    uint32 uniformVectors[3] = {NoShaderLimit, NoShaderLimit, NoShaderLimit};
};

// 생산 호스트 규칙 한 줄이다. systemTools는 그 호스트에서 이 형식을 만들 때 있어야 하는 system 도구
// (Apple metal)이며, 없으면 job이 UnsupportedCapability로 실패한다(결정 5).
struct ShaderFormatProducer {
    ShaderHost host = ShaderHost::WindowsX64;
    const ShaderTool* systemTools[MaxShaderFormatTools] = {};
    usize systemToolCount = 0;
};

// 형식 정의 하나다. 값 형식이며 도구 표 항목(정적 수명)만 가리킨다.
struct ROCKS_SHADERCOMPILERCOMMON_API ShaderFormatDefinition {
    char id[MaxShaderFormatIdBytes] = {};
    ShaderFormatState state = ShaderFormatState::Placeholder;
    ShaderBackend backend = ShaderBackend::D3D12;
    ShaderFormatModule module = ShaderFormatModule::D3D;
    uint32 moduleVersion = 0;
    uint32 bindingConvention = 0;
    // 이 형식이 쓸 수 있는 모든 도구(job 키 3항 정적 목록, 파일 순서).
    const ShaderTool* tools[MaxShaderFormatTools] = {};
    usize toolCount = 0;
    // HLSL 2021 기본값(결정 2). 셰이더 타입이 바꿀 수 있다.
    bool hlsl2021 = false;
    ShaderFormatCompiler compiler;
    uint32 stages = 0;
    ShaderFormatLimits limits;
    uint32 features = 0;
    uint32 deviceRequirements = 0;
    ShaderFormatProducer producers[MaxShaderFormatProducers] = {};
    usize producerCount = 0;
    // 파일 byte의 SHA-256(job 키 3항).
    Sha256Digest sha256;

    StringView Id() const noexcept {
        return StringView(id);
    }

    // host가 생산 호스트 규칙에 있는가(system 도구 유무는 보지 않는다).
    bool CanProduceOn(ShaderHost host) const noexcept;

    // host의 생산 규칙이다. 없으면 nullptr.
    const ShaderFormatProducer* Producer(ShaderHost host) const noexcept;
};

// 엔진 컴파일 프로필이다. formats는 파일 순서이고 모두 Supported이며 host에서 만들 수 있다.
struct EngineCompileProfile {
    char id[MaxShaderFormatIdBytes] = {};
    ShaderHost host = ShaderHost::WindowsX64;
    ShaderFormatDefinition formats[MaxEngineProfileFormats] = {};
    usize formatCount = 0;
    // 프로필 파일 byte의 SHA-256.
    Sha256Digest sha256;

    Span<const ShaderFormatDefinition> Formats() const noexcept {
        return Span<const ShaderFormatDefinition>(formats, formatCount);
    }
};

// 프로필 파일만 읽은 결과(형식 정의를 아직 찾지 않음)다.
struct EngineCompileProfileDesc {
    char id[MaxShaderFormatIdBytes] = {};
    ShaderHost host = ShaderHost::WindowsX64;
    char formats[MaxEngineProfileFormats][MaxShaderFormatIdBytes] = {};
    usize formatCount = 0;
    Sha256Digest sha256;
};

enum class ShaderFormatErrorCode : uint8 {
    None,
    // 파일을 열거나 읽지 못했다(path에 파일 이름).
    ReadFailed,
    // JSON 문법 오류(line에 위치).
    InvalidJson,
    // "schema"가 ShaderFormatSchemaVersion이 아니다.
    UnsupportedSchema,
    // 스키마에 없는 속성이다(그 형식·경로에 쓰이지 않는 속성 포함).
    UnknownProperty,
    MissingProperty,
    // 값의 JSON 타입이 다르다.
    WrongType,
    // 어휘 밖 이름, 범위 밖 숫자, 서로 맞지 않는 값(backend와 경로, 경로와 이미지 형식 전개 등).
    InvalidValue,
    // 목록에 같은 값이 두 번 있다.
    DuplicateValue,
    // 도구 이름이 도구 표에 없다.
    UnknownTool,
    // 파일 이름(기대 id)과 "id"가 다르다.
    IdMismatch,
    // 프로필의 형식에 형식 정의가 없다.
    UnknownFormat,
    // 프로필에 자리만 둔 형식이 있다.
    PlaceholderFormat,
    // 프로필 호스트가 그 형식의 생산 호스트 규칙에 없다.
    HostCannotProduce,
    // 목록이 고정 용량(MaxShaderFormatTools 등)을 넘는다.
    TooMany
};

// 진단용 열거자 이름이다(정적 수명).
ROCKS_SHADERCOMPILERCOMMON_API const char*
ShaderFormatErrorName(ShaderFormatErrorCode code) noexcept;

// 읽기·검증 결과다. path는 오류 위치(예: "compiler.esslVersion", "formats[2]", 읽기 실패면 파일
// 경로)이고, line은 JSON 위치(1부터, 모르면 0)다.
struct [[nodiscard]] ShaderFormatStatus {
    ShaderFormatErrorCode code = ShaderFormatErrorCode::None;
    char path[MaxShaderFormatErrorPathBytes] = {};
    uint32 line = 0;

    explicit operator bool() const noexcept {
        return code == ShaderFormatErrorCode::None;
    }
};

// 형식 정의 파일 byte를 읽고 검증한다. expectedId는 파일 이름에서 얻은 id(빈 값이면 검사하지
// 않는다), tools는 도구 이름을 찾는 표다(기본은 생성 도구 표). 성공하면 out.sha256 =
// SHA-256(bytes)다. 실패하면 out은 정해지지 않은 상태다.
ROCKS_SHADERCOMPILERCOMMON_API ShaderFormatStatus ParseShaderFormatDefinition(
    Span<const uint8> bytes, StringView expectedId, ShaderFormatDefinition& out,
    Span<const ShaderTool> tools = ShaderTools()) noexcept;

// directory(UTF-8, NUL 종료)/<id>.json을 읽어 ParseShaderFormatDefinition한다.
ROCKS_SHADERCOMPILERCOMMON_API ShaderFormatStatus LoadShaderFormatDefinition(
    const char* directory, StringView id, ShaderFormatDefinition& out) noexcept;

// 엔진 컴파일 프로필 파일 byte를 읽는다({schema, id, host, formats}). 형식 이름의 중복은 오류다.
ROCKS_SHADERCOMPILERCOMMON_API ShaderFormatStatus ParseEngineCompileProfile(
    Span<const uint8> bytes, StringView expectedId, EngineCompileProfileDesc& out) noexcept;

// desc의 형식을 definitions에서 찾아 out을 채운다. 없으면 UnknownFormat, 자리만 둔 형식이면
// PlaceholderFormat, 프로필 호스트가 생산 호스트가 아니면 HostCannotProduce다.
ROCKS_SHADERCOMPILERCOMMON_API ShaderFormatStatus ResolveEngineCompileProfile(
    const EngineCompileProfileDesc& desc, Span<const ShaderFormatDefinition> definitions,
    EngineCompileProfile& out) noexcept;

// directory/<profileId>.json과 그 프로필이 가리키는 형식 정의 파일들을 읽고 검증한다. 형식 정의
// 파일이 없으면 UnknownFormat(path에 형식 id)이다.
ROCKS_SHADERCOMPILERCOMMON_API ShaderFormatStatus LoadEngineCompileProfile(
    const char* directory, StringView profileId, EngineCompileProfile& out) noexcept;

// 어휘 이름 조회다(진단·시험용, 정적 수명). 범위 밖이면 "unknown".
const char* ShaderBackendName(ShaderBackend backend) noexcept;
const char* ShaderHostName(ShaderHost host) noexcept;

// ROCKS_IMAGE_FORMAT 형식 이름(DXC [[vk::image_format]] 철자)의 색인이다. 없으면 IndexNone.
// unsupportedImageFormats의 비트 번호다.
ROCKS_SHADERCOMPILERCOMMON_API usize ImageFormatIndex(StringView name) noexcept;

} // namespace rocks::shader
