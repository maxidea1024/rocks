#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Hash/Sha256.h>
#include <Core/Text/StringView.h>
#include <ShaderCore/ShaderFormatFacts.h>
#include <ShaderCore/ShaderFormatTypes.h>
#include <ShaderToolTable/ShaderToolTable.h>

// 셰이더 형식 정의다(Docs/Design/ShaderSystem 12.1절, P6a).
//
// 형식 정의(Engine/Config/ShaderFormats/<id>.json)는 한 셰이더 형식(D3D12_SM6 등)을 만드는 데
// 필요한 모든 값의 단일 출처다: 쓰는 도구 목록(job 키 3항의 정적 목록), HLSL 2021 기본값(결정 2),
// 컴파일 경로와 셰이더 모델·target env·ESSL·MSL 버전(결정 3), 바인딩 규약 버전, ShaderFormat 처리기
// 버전, 상한(7.2절 4항)·지원 기능·장치 요구, 플랫폼 분류(16.5절 SSD89), 생산 호스트 규칙(결정 5).
// 파일 byte의 SHA-256이 job 키 3항에 들어가므로(10절) 파일의 어떤 byte가 바뀌어도 그 형식의 키가
// 바뀐다(.gitattributes가
// *.json을 LF로 고정한다).
//
// 형식 목록과 순서는 TargetProfile이 정한다(엔진 컴파일 프로필은 개정 32에서 없앴다 — 12.3.1절
// SSD24). 공용 어휘(단계·기능·상한·backend)는 ShaderCore(ShaderCore/ShaderFormatTypes.h)에 있다.
//
// 읽기 규칙: 모르는 속성·모르는 어휘·범위 밖 값은 오류다(조용히 무시하지 않는다). 도구 이름은 도구
// 표(ShaderToolTable)에 있어야 한다. 자리만 둔 형식(status "placeholder",
// VULKAN_ANDROID·METAL_IOS)은 schema·id·status·backend만 가진다.
//
// 사용 예(메인 프로세스):
//   ShaderFormatDefinition format;
//   if (const ShaderFormatStatus loaded =
//           LoadShaderFormatDefinition(ShaderFormatDirectory, "D3D12_SM6", format);
//       !loaded) {
//       printf("%s: %s (line %u)\n", ShaderFormatErrorName(loaded.code), loaded.path, loaded.line);
//   }
//   // 상한은 job 형식마다 따로 검사한다(7.2절 4항): format.limits.
//
// 스레드: 모든 함수는 재진입 가능하며 전역 상태를 쓰지 않는다. 결과 객체는 값 형식이고 도구 표
// 항목(정적 수명)만 가리킨다.
namespace rocks::shader {

// 형식 정의 스키마 버전이다("schema"). 다른 값은 UnsupportedSchema다.
inline constexpr uint32 ShaderFormatSchemaVersion = 1;
// 저장소 루트 기준 형식 정의 디렉터리다.
inline constexpr char ShaderFormatDirectory[] = "Engine/Config/ShaderFormats";
// 형식 id의 최대 byte 수(MaxShaderFormatIdBytes)는 ShaderCore에 있다(형식 사실이 쓴다).
inline constexpr usize MaxShaderFormatTools = 8;
inline constexpr usize MaxShaderFormatProducers = 4;
// 오류 위치(JSON 경로 또는 파일 이름)의 최대 byte 수(NUL 포함)다. 넘는 부분은 잘린다.
inline constexpr usize MaxShaderFormatErrorPathBytes = 96;
// 루트 상수 크기 상한(7.2절 2항, Vulkan push constant 최소 보장 128 byte)이다.
inline constexpr uint32 MaxRootConstantBytes = 128;
// 단계 사이 vec4 상한(7.2절 7항, TEXCOORD0–15)과 render target 상한(SV_Target0–7)이다.
inline constexpr uint32 MaxInterStageVectors = 16;
inline constexpr uint32 MaxRenderTargets = 8;
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
    // 플랫폼 분류 비트(ShaderPlatformClassBits — "platformClass", 16.5절 SSD89)다. OpenGL backend는
    // Mobile을 담아야 한다.
    uint32 platformClasses = 0;
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

// 형식 정의의 형식 사실이다(12.2.1절 SSD25 — 셰이더 타입 거르기와 라이브러리 머리가 쓴다).
// id·backend· compiler.shaderModel·limits·features·deviceRequirements·platformClasses를 옮긴다.
// 정의는 읽기를 통과한 값이어야 한다(id 철자가 형식 사실 규칙과 같다).
ROCKS_SHADERCOMPILERCOMMON_API ShaderFormatFacts
MakeShaderFormatFacts(const ShaderFormatDefinition& definition) noexcept;

// 어휘 이름 조회다(진단·시험용, 정적 수명). 범위 밖이면 "unknown". ShaderBackendName은
// ShaderCore(ShaderCore/ShaderFormatTypes.h)에 있다.
const char* ShaderHostName(ShaderHost host) noexcept;

// ROCKS_IMAGE_FORMAT 형식 이름(DXC [[vk::image_format]] 철자)의 색인이다. 없으면 IndexNone.
// unsupportedImageFormats의 비트 번호다.
ROCKS_SHADERCOMPILERCOMMON_API usize ImageFormatIndex(StringView name) noexcept;

// 색인의 ROCKS_IMAGE_FORMAT 형식 이름이다(정적 수명). 범위 밖이면 "unknown". SPIR-V image format
// 값은 이 색인 + 1이다(DXC 지원 목록과 SPIR-V ImageFormat 열거 순서가 같다).
ROCKS_SHADERCOMPILERCOMMON_API const char* ImageFormatName(usize index) noexcept;

} // namespace rocks::shader
