#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>

// 픽셀 형식 번호표·형식 정보·크기 계산이다(Docs/Design/PixelFormat.ko.md). 그래픽 장치
// 계약(Graphics)과 에셋(AssetRuntime)·도구가 함께 쓰는 값 타입이며 장치 객체를 담지 않는다. 장치
// 형식 대응과 장치별 능력은 backend가 가진다(PixelFormat.ko.md 8절).
namespace rocks::graphics {

// 텍스처·렌더 타깃 픽셀 형식이다(UE 5.8 EPixelFormat 대응, PixelFormat.ko.md 5절).
//
// - 열거자 값이 고정 번호표다. 계열별 구간(결정 PX2): 1–63 비압축 색, 64–79 깊이·스텐실, 80–95
//   BC, 96–111 ETC2·EAC, 112–159 ASTC, 160–175 비디오. 빈 번호는 무효 값이다.
// - 번호는 쿠킹 데이터에 저장되므로 한 번 정하면 바꾸거나 재사용하지 않고 추가만 한다. 직렬화는
//   PixelFormatNumber(바이너리)·PixelFormatName(텍스트·JSON)을 쓴다.
// - 이름 규칙: 채널 대문자 + 비트 + 성분 타입(Unorm·Snorm·Uint·Sint·Float·Ufloat). 바이트 형식은
//   첫 바이트가 첫 채널이고(RGBA8은 R가 첫 바이트), 묶음 형식(R5G6B5·RGB10A2·RG11B10·RGB9E5)은 첫
//   채널이 상위 비트다.
// - sRGB 형식(*UnormSrgb)은 저장값이 sRGB 부호화이며 샘플링은 선형으로 디코딩하고 렌더 타깃 쓰기는
//   부호화한다(결정 PX1). 선형 짝은 PixelFormatInfo::srgbPair다.
// - 장치가 지원하는지는 형식만으로 정해지지 않는다. backend가 지원하지 않는 형식으로 자원을 만들면
//   Unsupported다(다른 형식으로 바꾸지 않는다).
//
// 사용 예:
//   const PixelFormatInfo& info = GetPixelFormatInfo(PixelFormat::BC7UnormSrgb);  // 4×4 블록 16B
//   const uint64 bytes = PixelFormatImageBytes(PixelFormat::BC1Unorm, 5, 3);      // 16
//   auto loaded = PixelFormatFromNumber(archive.ReadU32());                      // 바이너리
//   auto parsed = ParsePixelFormatName("RGBA16Float");                           // 텍스트
enum class PixelFormat : uint8 {
    Unknown = 0,

    // 비압축 색(1–63)
    R8Unorm = 1,
    R8UnormSrgb = 2,
    R8Uint = 3,
    R8Sint = 4,
    // 알파 한 채널이다. 샘플하면 (0, 0, 0, a)다.
    A8Unorm = 5,
    RG8Unorm = 6,
    RG8UnormSrgb = 7,
    RG8Snorm = 8,
    RG8Uint = 9,
    RGB8Unorm = 10,
    RGB8UnormSrgb = 11,
    RGBA8Unorm = 12,
    RGBA8UnormSrgb = 13,
    RGBA8Snorm = 14,
    RGBA8Uint = 15,
    // 메모리 순서가 B·G·R·A다. 셰이더는 (r, g, b, a)로 읽는다.
    BGRA8Unorm = 16,
    BGRA8UnormSrgb = 17,
    R16Unorm = 18,
    R16Uint = 19,
    R16Sint = 20,
    R16Float = 21,
    RG16Unorm = 22,
    RG16Snorm = 23,
    RG16Uint = 24,
    RG16Sint = 25,
    RG16Float = 26,
    RGBA16Unorm = 27,
    RGBA16Snorm = 28,
    RGBA16Uint = 29,
    RGBA16Sint = 30,
    RGBA16Float = 31,
    R32Uint = 32,
    R32Sint = 33,
    R32Float = 34,
    RG32Uint = 35,
    RG32Float = 36,
    RGB32Uint = 37,
    RGB32Sint = 38,
    RGB32Float = 39,
    RGBA32Uint = 40,
    RGBA32Float = 41,
    // 64비트 image atomic 전용이다(UE PF_R64_UINT).
    R64Uint = 42,
    R5G6B5Unorm = 43,
    // 16비트 묶음: A가 최상위 비트, 그 아래 R5·G5·B5(DXGI B5G5R5A1_UNORM 배치).
    B5G5R5A1Unorm = 44,
    // 32비트 묶음: R가 하위 10비트, A가 상위 2비트(DXGI R10G10B10A2_UNORM 배치).
    RGB10A2Unorm = 45,
    RG11B10Ufloat = 46,
    // 공유 지수 RGB(DXGI R9G9B9E5_SHAREDEXP).
    RGB9E5Ufloat = 47,

    // 깊이·스텐실(64–79). 스텐실 읽기는 형식이 아니라 view의 plane 선택이다(PixelFormat.ko.md
    // 10절).
    D16Unorm = 64,
    D24Unorm = 65,
    D32Float = 66,
    D24UnormS8Uint = 67,
    D32FloatS8Uint = 68,

    // BC(80–95). BC1은 1비트 punch-through 알파를 포함한다(D3D 의미).
    BC1Unorm = 80,
    BC1UnormSrgb = 81,
    BC2Unorm = 82,
    BC2UnormSrgb = 83,
    BC3Unorm = 84,
    BC3UnormSrgb = 85,
    BC4Unorm = 86,
    BC5Unorm = 87,
    BC6HUfloat = 88,
    BC7Unorm = 89,
    BC7UnormSrgb = 90,

    // ETC2·EAC(96–111)
    ETC2RGB8Unorm = 96,
    ETC2RGB8UnormSrgb = 97,
    ETC2RGBA8Unorm = 98,
    ETC2RGBA8UnormSrgb = 99,
    EACR11Unorm = 100,
    EACRG11Unorm = 101,

    // ASTC(112–159). UE와 같은 정사각 블록 5종이다.
    ASTC4x4Unorm = 112,
    ASTC4x4UnormSrgb = 113,
    ASTC4x4Hdr = 114,
    ASTC6x6Unorm = 115,
    ASTC6x6UnormSrgb = 116,
    ASTC6x6Hdr = 117,
    ASTC8x8Unorm = 118,
    ASTC8x8UnormSrgb = 119,
    ASTC8x8Hdr = 120,
    ASTC10x10Unorm = 121,
    ASTC10x10UnormSrgb = 122,
    ASTC10x10Hdr = 123,
    ASTC12x12Unorm = 124,
    ASTC12x12UnormSrgb = 125,
    ASTC12x12Hdr = 126,

    // 비디오(160–175). 4:2:0 2-plane이며 폭·높이는 2의 배수여야 한다.
    // plane 0은 휘도(원래 크기), plane 1은 색차 CbCr(폭·높이 ½)다.
    NV12 = 160,
    // NV12와 같은 배치의 16비트 저장(상위 10비트 사용).
    P010 = 161
};

// 유효 형식 수(Unknown 제외)와 번호표에서 가장 큰 번호다.
inline constexpr uint32 PixelFormatCount = 86;
inline constexpr uint32 MaxPixelFormatNumber = 161;

// 형식의 종류다.
enum class PixelFormatKind : uint8 {
    Unknown,
    // 비압축 색
    Color,
    // 깊이만
    Depth,
    // 깊이 + 스텐실(plane 2개: 깊이, 스텐실)
    DepthStencil,
    // 블록 압축 색
    Compressed,
    // 다중 plane 색(비디오)
    Planar
};

// 블록 압축 계열이다.
enum class PixelCompression : uint8 {
    None,
    BC,
    ETC2,
    ASTC
};

// 저장 성분의 타입이다. Ufloat는 부호 없는 float(BC6H·RG11B10·RGB9E5)다.
enum class PixelComponentType : uint8 {
    None,
    Unorm,
    Snorm,
    Uint,
    Sint,
    Float,
    Ufloat
};

// 셰이더가 이 형식을 읽을 때의 값 계열이다(바인딩 검사용).
enum class PixelShaderType : uint8 {
    None,
    Float,
    Int,
    Uint,
    Uint64
};

// 형식이 가진 채널 비트다. PixelFormatInfo::channels가 이 값들의 OR다.
enum class PixelChannel : uint8 {
    R = 1u << 0,
    G = 1u << 1,
    B = 1u << 2,
    A = 1u << 3,
    Depth = 1u << 4,
    Stencil = 1u << 5
};

// 장치가 형식에 대해 가진 능력이다(PixelFormat.ko.md 7절). 값은 장치가 채우고(GX1) 이 타입은 표현만
// 정한다. 여러 비트를 OR로 묶는다.
enum class PixelFormatCaps : uint32 {
    None = 0,
    // 2D 텍스처로 만들고 셰이더에서 샘플·load
    Sampled = 1u << 0,
    Texture3D = 1u << 1,
    // cube와 cube array
    TextureCube = 1u << 2,
    // 선형 필터
    Filterable = 1u << 3,
    Gather = 1u << 4,
    Mipmaps = 1u << 5,
    RenderTarget = 1u << 6,
    Blendable = 1u << 7,
    // MSAA 렌더 타깃(표본 수는 장치에 따로 묻는다)
    Multisample = 1u << 8,
    DepthStencil = 1u << 9,
    // typed UAV load·store·atomic
    StorageRead = 1u << 10,
    StorageWrite = 1u << 11,
    StorageAtomic = 1u << 12,
    // typed buffer view
    TexelBufferRead = 1u << 13,
    TexelBufferWrite = 1u << 14,
    TexelBufferAtomic = 1u << 15
};

constexpr PixelFormatCaps operator|(PixelFormatCaps left, PixelFormatCaps right) noexcept {
    return static_cast<PixelFormatCaps>(static_cast<uint32>(left) | static_cast<uint32>(right));
}

constexpr PixelFormatCaps operator&(PixelFormatCaps left, PixelFormatCaps right) noexcept {
    return static_cast<PixelFormatCaps>(static_cast<uint32>(left) & static_cast<uint32>(right));
}

// caps가 required의 모든 비트를 가졌는지다. required가 None이면 true다.
constexpr bool HasPixelFormatCaps(PixelFormatCaps caps, PixelFormatCaps required) noexcept {
    return (caps & required) == required;
}

// 다중 plane 형식의 plane 하나다. 크기는 (width >> widthShift, height >> heightShift)이고 texel은
// format 형식이다(깊이·스텐실의 스텐실 plane은 R8Uint).
struct PixelFormatPlane {
    PixelFormat format = PixelFormat::Unknown;
    uint8 widthShift = 0;
    uint8 heightShift = 0;
};

// 형식 하나의 장치 없는 정보다(PixelFormat.ko.md 6절, UE FPixelFormatInfo 대응).
struct PixelFormatInfo {
    PixelFormat format = PixelFormat::Unknown;
    // 안정 이름이다(열거자 이름과 같다). 텍스트·JSON 직렬화에 쓴다.
    const char* name = "Unknown";
    PixelFormatKind kind = PixelFormatKind::Unknown;
    PixelCompression compression = PixelCompression::None;
    // 블록 크기다. 비압축·깊이·다중 plane은 1×1이다. 깊이 방향 블록은 없다.
    uint8 blockWidth = 0;
    uint8 blockHeight = 0;
    // 블록 하나의 바이트다. 깊이·스텐실과 다중 plane 형식은 0이고 plane별 크기를 쓴다.
    uint8 blockBytes = 0;
    // PixelChannel 비트의 OR다.
    uint8 channels = 0;
    PixelComponentType componentType = PixelComponentType::None;
    PixelShaderType shaderType = PixelShaderType::None;
    // sRGB 부호화 형식인지와 그 선형·sRGB 짝이다(없으면 Unknown).
    bool srgb = false;
    PixelFormat srgbPair = PixelFormat::Unknown;
    uint8 depthBits = 0;
    uint8 stencilBits = 0;
    // 1(일반) 또는 2(깊이 + 스텐실, 비디오)다. 2일 때만 planes가 의미를 가진다.
    uint8 planeCount = 0;
    PixelFormatPlane planes[2] = {};
    // 최상위 mip의 폭·높이가 블록 크기의 배수여야 하는지다(BC만 true, UE RequiresBlock4Alignment).
    bool topLevelBlockAligned = false;
};

// 형식 정보다. 번호표 밖이면 Unknown 행(크기 0)을 준다.
const PixelFormatInfo& GetPixelFormatInfo(PixelFormat format) noexcept;

// 번호표 안의 유효 형식인지다(Unknown은 false).
bool IsValidPixelFormat(PixelFormat format) noexcept;

// 고정 번호다(바이너리 직렬화).
constexpr uint32 PixelFormatNumber(PixelFormat format) noexcept {
    return static_cast<uint32>(format);
}

// 번호의 형식이다. 실패: 번호표 밖 또는 0(Unknown) InvalidArgument.
Result<PixelFormat> PixelFormatFromNumber(uint32 number) noexcept;

// "RGBA8UnormSrgb" 같은 안정 이름이다. 번호표 밖이면 "Unknown"이다.
const char* PixelFormatName(PixelFormat format) noexcept;

// 안정 이름의 형식이다(대소문자 구분). 실패: 모르는 이름("Unknown" 포함) NotFound.
Result<PixelFormat> ParsePixelFormatName(StringView name) noexcept;

// 폭 width를 덮는 블록 열 수(ceil(width / blockWidth))와 높이 height를 덮는 블록 행 수다. 무효
// 형식이면 0이다.
uint32 PixelFormatBlockColumns(PixelFormat format, uint32 width) noexcept;
uint32 PixelFormatBlockRows(PixelFormat format, uint32 height) noexcept;

// 블록 행 하나의 바이트다(색·압축 형식만, 그 밖 0).
uint64 PixelFormatRowBytes(PixelFormat format, uint32 width) noexcept;

// 2D 이미지 하나의 바이트다(행 사이 padding 없음, RenderBase 6.5절 배치).
// - 색·압축: 블록 열 수 × 블록 행 수 × blockBytes
// - 다중 plane(비디오): plane 크기의 합. 폭·높이가 plane 축소 배수가 아니면 0
// - 깊이·스텐실: 0(CPU 업로드 크기를 정의하지 않는다. plane별 크기는 PixelFormatPlaneImageBytes)
// 크기가 0이거나 형식이 무효면 0이다.
uint64 PixelFormatImageBytes(PixelFormat format, uint32 width, uint32 height) noexcept;

// plane 하나의 2D 이미지 바이트다(readback·plane 업로드). 단일 plane 형식은 plane 0만 있다.
// 깊이·스텐실은 plane 0이 깊이(D16 2·D24 4·D32F 4바이트), plane 1이 스텐실(1바이트)이다. 범위 밖
// plane이나 무효 크기는 0이다.
uint64 PixelFormatPlaneImageBytes(PixelFormat format, uint32 plane, uint32 width,
                                  uint32 height) noexcept;

// mip level의 크기다(max(1, size >> level)).
constexpr uint32 MipExtent(uint32 size, uint32 level) noexcept {
    const uint32 extent = level < 32 ? size >> level : 0u;
    return extent == 0 ? 1u : extent;
}

// width × height의 전체 mip 수(1×1까지)다.
constexpr uint32 FullMipCount(uint32 width, uint32 height) noexcept {
    uint32 largest = width > height ? width : height;
    uint32 count = 1;
    while (largest > 1) {
        largest >>= 1;
        ++count;
    }

    return count;
}

// 앞 mipLevels개 mip을 빈틈 없이 이은 바이트다(텍스처 초기 데이터 배치). 각 mip은 depth개의 2D
// 이미지(PixelFormatImageBytes)이고 depth도 mip마다 max(1, depth >> level)로 줄어든다(2D는 1).
// 크기·mip 수(1–전체 mip 수)·형식이 잘못됐거나 이미지 크기가 0이면 0이다.
//
// 사용 예:
//   MipChainBytes(PixelFormat::RGBA8Unorm, 4, 4, 1, 3);  // 64 + 16 + 4 = 84
uint64 MipChainBytes(PixelFormat format, uint32 width, uint32 height, uint32 depth,
                     uint32 mipLevels) noexcept;

} // namespace rocks::graphics
