#pragma once

#include "GlesApi.h"

#include <Graphics/DeviceTypes.h>

// Graphics 값(픽셀 형식·정점 형식·샘플러)을 OpenGL ES 3.2 값으로 옮기는 표다(설계:
// Docs/Design/OpenGLESGraphics.ko.md 5절).
namespace rocks::graphics::gles {

// 확장에 기대는 기능이다. 장치가 만들어질 때 한 번 조사해 GlesFeatures에 둔다. 형식 표의 항목이
// 이 값 하나를 요구할 수 있다(None이면 ES 3.2 core만으로 된다). 판정은 ES 확장 이름으로 한다
// (Windows 미리보기의 데스크톱 core context도 같은 이름을 공개할 때만 켠다,
// PixelFormat.ko.md 8.6절).
enum class GlesFeature : uint8 {
    None,
    // R16·RG16·RGBA16 UNORM·SNORM 텍스처와 UNORM 렌더 타깃(GL_EXT_texture_norm16)
    TextureNorm16,
    // BGRA8 텍스처(GL_EXT_texture_format_BGRA8888)
    TextureBgra8,
    // 32-bit float 텍스처의 선형 필터(GL_OES_texture_float_linear)
    FloatLinearFilter,
    // 32-bit float 렌더 타깃의 블렌드(GL_EXT_float_blend)
    FloatBlend,
    // SNORM 렌더 타깃(GL_EXT_render_snorm)
    RenderSnorm,
    // sRGB R8·RG8 텍스처(GL_EXT_texture_sRGB_R8·GL_EXT_texture_sRGB_RG8)
    SrgbR8,
    SrgbRG8,
    // BC1–3(GL_EXT_texture_compression_s3tc), 그 sRGB(GL_EXT_texture_compression_s3tc_srgb),
    // BC4·BC5(GL_EXT_texture_compression_rgtc), BC6H·BC7(GL_EXT_texture_compression_bptc)
    S3tc,
    S3tcSrgb,
    Rgtc,
    Bptc,
    // ASTC LDR(ES 3.2 core, 데스크톱 core context는 GL_KHR_texture_compression_astc_ldr)
    AstcLdr,
    // ASTC HDR(GL_KHR_texture_compression_astc_hdr)
    AstcHdr,
    // 이방성 필터(GL_EXT_texture_filter_anisotropic)
    Anisotropic
};

// 이 장치가 가진 확장 기능이다(GlesFeature 하나마다 bool 하나).
struct GlesFeatures {
    bool textureNorm16 = false;
    bool textureBgra8 = false;
    bool floatLinearFilter = false;
    bool floatBlend = false;
    bool renderSnorm = false;
    bool srgbR8 = false;
    bool srgbRG8 = false;
    bool s3tc = false;
    bool s3tcSrgb = false;
    bool rgtc = false;
    bool bptc = false;
    bool astcLdr = false;
    bool astcHdr = false;
    bool anisotropic = false;
    // 이방성 필터 최대 배율이다(anisotropic이 false면 0).
    float maxAnisotropy = 0.0f;
    // timestamp query가 끊김(GL_GPU_DISJOINT_EXT)을 알리는지다(GL_EXT_disjoint_timer_query).
    bool disjointTimerQuery = false;
    // 렌더 타깃의 sRGB 쓰기 변환을 끄고 켤 수 있는지다(GL_FRAMEBUFFER_SRGB: 데스크톱 core 또는
    // GL_EXT_sRGB_write_control).
    bool srgbWriteControl = false;
    // 깊이 clamp(GL_EXT_depth_clamp, UE OpenGLES.cpp:185와 같은 판정)다.
    bool depthClamp = false;

    bool Has(GlesFeature feature) const noexcept;
};

// 지금 context의 확장을 조사한다. desktopContext는 플랫폼 층이 데스크톱 core context면 true로
// 넘긴다(sRGB 쓰기 스위치가 core 기능이고, ASTC LDR은 ES core가 아니므로 확장 이름을 요구한다).
GlesFeatures DetectFeatures(const GlesFunctions& gl, bool desktopContext) noexcept;

// ReadTexture가 framebuffer에서 읽는 방법이다(ES 3.2 16.1.2절의 읽기 형식·타입 조합). 깊이·스텐실
// plane은 셰이더로 색 대상에 옮겨 읽는다.
enum class GlesReadClass : uint8 {
    // 읽을 수 없다(색 렌더 타깃이 아닌 형식, 블록 압축).
    None,
    // RGBA/UNSIGNED_BYTE(정규화 고정소수점 8-bit 이하)
    Unorm8,
    // RGBA/BYTE(GL_EXT_render_snorm 8-bit)
    Snorm8,
    // RGBA/UNSIGNED_SHORT(GL_EXT_texture_norm16)
    Unorm16,
    // RGBA/SHORT(GL_EXT_render_snorm 16-bit)
    Snorm16,
    // RGBA_INTEGER/UNSIGNED_INT
    Uint,
    // RGBA_INTEGER/INT
    Sint,
    // RGBA/FLOAT
    Float,
    // RGBA/UNSIGNED_INT_2_10_10_10_REV
    Rgb10A2,
    // 깊이 plane은 R32F 대상, 스텐실 plane은 R32UI 대상으로 셰이더가 옮긴다.
    DepthStencil
};

// 형식 표 항목의 성질 비트다.
enum GlesFormatFlag : uint16 {
    // ES 3.2 core에서 색 렌더 타깃이다(Table 8.10 CR).
    GlesColorRenderable = 1u << 0,
    // core에서 선형 필터를 받는다(Table 8.10 TF).
    GlesFilterable = 1u << 1,
    // image load·store 형식이다(Table 8.26).
    GlesStorage = 1u << 2,
    // image atomic 대상이다(R32UI·R32I).
    GlesAtomic = 1u << 3,
    // 블록 압축 형식이다(glCompressedTexSubImage2D로 올린다).
    GlesCompressed = 1u << 4,
    // R8 저장소를 (0, 0, 0, R)로 읽는 A8 에뮬레이션이다(샘플 전용).
    GlesAlphaSwizzle = 1u << 5,
    // 정수 형식이다(블렌드·선형 필터 없음).
    GlesInteger = 1u << 6
};

// 텍스처 형식 대응 하나다. 새 픽셀 형식(PixelFormat 확장)은 GlesFormats.cpp의 표에 행을 더한다.
struct GlesTextureFormat {
    PixelFormat format = PixelFormat::Unknown;
    // glTexStorage2D 내부 형식이다.
    GLenum internalFormat = 0;
    // glTexSubImage2D의 외부 형식·타입이다(Rocks 배치와 같은 byte 배치). 블록 압축은 0이다.
    // 깊이·스텐실은 장치가 plane을 이 배치로 엮어 올린다.
    GLenum externalFormat = 0;
    GLenum externalType = 0;
    uint16 flags = 0;
    // 샘플링에 필요한 확장이다(None = core).
    GlesFeature sampleFeature = GlesFeature::None;
    // 렌더 타깃에 필요한 확장이다(GlesColorRenderable이 없을 때만 본다).
    GlesFeature renderFeature = GlesFeature::None;
    // 선형 필터에 필요한 확장이다(GlesFilterable이 없을 때만 본다).
    GlesFeature filterFeature = GlesFeature::None;
    // 블렌드에 필요한 확장이다(None = 렌더 타깃이면 블렌드 가능).
    GlesFeature blendFeature = GlesFeature::None;
    GlesReadClass read = GlesReadClass::None;

    bool Is(GlesFormatFlag flag) const noexcept {
        return (flags & flag) != 0;
    }
};

// format의 대응이다. 표에 없는 형식(Unknown 포함)은 nullptr이다.
const GlesTextureFormat* FindTextureFormat(PixelFormat format) noexcept;

// internalFormat이 ES 3.2 버퍼 텍스처(GL_TEXTURE_BUFFER) 내부 형식인지다(Table 8.18 가운데 Rocks
// 형식이 쓰는 것, norm16 제외).
bool IsBufferTextureFormat(GLenum internalFormat) noexcept;

// 이 장치에서 format의 능력이다(PixelFormat.ko.md 7절 OpenGL ES 열). 표에 없으면 None이다.
PixelFormatCaps TextureFormatCaps(const GlesTextureFormat& format,
                                  const GlesFeatures& features) noexcept;

// read 방법의 framebuffer 읽기 형식·타입과 texel 하나의 byte 수다. 깊이·스텐실은 깊이 plane의
// 값(R32F 대상)이고 plane 1은 RGBA_INTEGER/UNSIGNED_INT다(호출자가 고른다).
struct GlesReadPair {
    GLenum format = 0;
    GLenum type = 0;
    uint32 texelBytes = 0;
};

GlesReadPair CanonicalReadPair(GlesReadClass read) noexcept;

// 표준 읽기 조합으로 읽은 texel들을 Rocks 배치로 옮긴다(채널 고르기·좁히기, half·11/10-bit float
// 묶기, BGRA 순서, 5_6_5 묶기). 정확히 되돌릴 수 있는 변환만 한다.
void ConvertReadTexels(const GlesTextureFormat& format, const uint8* source, uint8* destination,
                       uint64 texels) noexcept;

// 정점 원소 형식의 GL 속성 설정이다(RenderBase 10.4절 표, UE OpenGLVertexDeclaration.cpp:49–77).
// 정규화 없는 정수 형식은 glVertexAttribIFormat(integer = true)로 설정한다(float 변환을 막는다).
// Bgra8Unorm은 ES에 BGRA 정점 배열(size GL_BGRA)이 없어 supported = false다.
struct GlesVertexAttribute {
    GLenum type = 0;
    GLint size = 0;
    bool normalized = false;
    bool integer = false;
    bool supported = false;
};

GlesVertexAttribute VertexAttributeFormat(VertexFormat format) noexcept;

// active attribute 타입의 셰이더 타입 계열이다. 받을 수 없는 타입(행렬 등)은 false다.
bool ShaderTypeOf(GLenum type, VertexShaderType& out) noexcept;

// 샘플러 객체의 축소·확대 필터와 주소 모드다.
GLint MinFilter(Filter filter) noexcept;
GLint MagFilter(Filter filter) noexcept;
GLint WrapMode(AddressMode mode) noexcept;

// 진단 문자열 조립 칸이다(고정 256 byte, 넘치면 자른다). 할당하지 않는다.
class Diagnostic {
public:
    Diagnostic& Text(const char* text) noexcept;
    Diagnostic& Text(StringView text) noexcept;
    Diagnostic& Number(uint64 value) noexcept;
    // 0x를 붙인 16진수다.
    Diagnostic& Hex(uint64 value) noexcept;

    const char* CStr() const noexcept {
        return buffer_;
    }

    StringView View() const noexcept {
        return StringView(buffer_, length_);
    }

    // 칸이 모자라 자른 적이 있는지다(DeviceCreateReport::truncated).
    bool Truncated() const noexcept {
        return truncated_;
    }

private:
    // 한 글자를 붙인다. 칸이 모자라면 잘림을 기록한다.
    void Put(char character) noexcept;

    char buffer_[256] = {};
    usize length_ = 0;
    bool truncated_ = false;
};

} // namespace rocks::graphics::gles
