#pragma once

#include "GlesApi.h"

#include <Graphics/DeviceTypes.h>

// Graphics 값(픽셀 형식·정점 형식·샘플러)을 OpenGL ES 3.2 값으로 옮기는 표다(설계:
// Docs/Design/OpenGLESGraphics.ko.md 5절).
namespace rocks::graphics::gles {

// 확장에 기대는 기능이다. 장치가 만들어질 때 한 번 조사해 GlesFeatures에 둔다. 형식·필터 표의
// 항목이 이 값 하나를 요구할 수 있다(None이면 ES 3.2 core만으로 된다).
enum class GlesFeature : uint8 {
    None,
    // R16·RG16·RGBA16 UNORM 텍스처(GL_EXT_texture_norm16)
    TextureNorm16,
    // BGRA8 텍스처(GL_EXT_texture_format_BGRA8888)
    TextureBgra8,
    // 32-bit float 텍스처의 선형 필터(GL_OES_texture_float_linear)
    FloatLinearFilter,
    // float·half float 색 렌더 타깃(GL_EXT_color_buffer_float)
    ColorBufferFloat,
    // 이방성 필터(GL_EXT_texture_filter_anisotropic)
    Anisotropic
};

// 이 장치가 가진 확장 기능이다(GlesFeature 하나마다 bool 하나).
struct GlesFeatures {
    bool textureNorm16 = false;
    bool textureBgra8 = false;
    bool floatLinearFilter = false;
    bool colorBufferFloat = false;
    bool anisotropic = false;
    // 이방성 필터 최대 배율이다(anisotropic이 false면 0).
    float maxAnisotropy = 0.0f;
    // timestamp query가 끊김(GL_GPU_DISJOINT_EXT)을 알리는지다(GL_EXT_disjoint_timer_query).
    bool disjointTimerQuery = false;
    // 렌더 타깃의 sRGB 쓰기 변환을 끄고 켤 수 있는지다(GL_FRAMEBUFFER_SRGB: 데스크톱 core 또는
    // GL_EXT_sRGB_write_control).
    bool srgbWriteControl = false;

    bool Has(GlesFeature feature) const noexcept;
};

// 지금 context의 확장을 조사한다. srgbWriteControl은 플랫폼 층이 데스크톱 core context면 true로
// 넘긴다(core 기능이라 확장 이름이 없다).
GlesFeatures DetectFeatures(const GlesFunctions& gl, bool desktopSrgbWriteControl) noexcept;

// 텍스처 형식 대응 하나다. 새 픽셀 형식(PixelFormat 확장)은 GlesFormats.cpp의 표에 행을 더한다.
struct GlesTextureFormat {
    PixelFormat format = PixelFormat::Unknown;
    // glTexStorage2D 내부 형식이다.
    GLenum internalFormat = 0;
    // glTexSubImage2D·glReadPixels의 외부 형식·타입이다.
    GLenum externalFormat = 0;
    GLenum externalType = 0;
    // 렌더 타깃(색 attachment)으로 쓸 수 있는지다(ES 3.2 core 기준, 확장이 넓히는 것은
    // renderFeature).
    bool renderable = false;
    // 샘플링에 필요한 확장이다(None = core).
    GlesFeature sampleFeature = GlesFeature::None;
    // 렌더 타깃에 필요한 확장이다(None = renderable이 core 판정 그대로).
    GlesFeature renderFeature = GlesFeature::None;
};

// format의 대응이다. 표에 없는 형식(Unknown 포함)은 nullptr이다.
const GlesTextureFormat* FindTextureFormat(PixelFormat format) noexcept;

// format을 이 장치에서 샘플링(renderTarget이면 렌더 타깃으로도)할 수 있는지다.
bool SupportsTextureFormat(const GlesTextureFormat& format, const GlesFeatures& features,
                           bool renderTarget) noexcept;

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
