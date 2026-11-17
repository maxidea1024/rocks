#include "GlesFormats.h"

namespace rocks::graphics::gles {

namespace {

// 픽셀 형식 표다. ES 3.2 core는 RGBA8·SRGB8_ALPHA8을 샘플링·렌더 타깃 모두로 받는다(ES 3.2 사양
// 표 8.10·8.13). PixelFormat 확장 때 이 표에 행을 더하고, core 밖 형식은 sampleFeature·
// renderFeature로 확장을 요구한다(예: R16 UNORM → TextureNorm16, RGBA16F 렌더 타깃 →
// ColorBufferFloat).
constexpr GlesTextureFormat TextureFormats[] = {
    {Format::RGBA8Unorm, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, true, GlesFeature::None,
     GlesFeature::None},
    {Format::RGBA8UnormSrgb, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, true, GlesFeature::None,
     GlesFeature::None},
};

} // namespace

bool GlesFeatures::Has(GlesFeature feature) const noexcept {
    switch (feature) {
    case GlesFeature::None:
        return true;
    case GlesFeature::TextureNorm16:
        return textureNorm16;
    case GlesFeature::TextureBgra8:
        return textureBgra8;
    case GlesFeature::FloatLinearFilter:
        return floatLinearFilter;
    case GlesFeature::ColorBufferFloat:
        return colorBufferFloat;
    case GlesFeature::Anisotropic:
        return anisotropic;
    }

    return false;
}

GlesFeatures DetectFeatures(const GlesFunctions& gl, bool desktopSrgbWriteControl) noexcept {
    GlesFeatures features;
    features.textureNorm16 = HasGlExtension(gl, "GL_EXT_texture_norm16");
    features.textureBgra8 = HasGlExtension(gl, "GL_EXT_texture_format_BGRA8888");
    features.floatLinearFilter = HasGlExtension(gl, "GL_OES_texture_float_linear");
    features.colorBufferFloat = HasGlExtension(gl, "GL_EXT_color_buffer_float");
    features.anisotropic = HasGlExtension(gl, "GL_EXT_texture_filter_anisotropic");
    if (features.anisotropic) {
        gl.GetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &features.maxAnisotropy);
        features.anisotropic = features.maxAnisotropy >= 1.0f;
    }

    features.disjointTimerQuery = HasGlExtension(gl, "GL_EXT_disjoint_timer_query");
    features.srgbWriteControl =
        desktopSrgbWriteControl || HasGlExtension(gl, "GL_EXT_sRGB_write_control");
    return features;
}

const GlesTextureFormat* FindTextureFormat(Format format) noexcept {
    for (const GlesTextureFormat& entry : TextureFormats) {
        if (entry.format == format) {
            return &entry;
        }
    }

    return nullptr;
}

bool SupportsTextureFormat(const GlesTextureFormat& format, const GlesFeatures& features,
                           bool renderTarget) noexcept {
    if (!features.Has(format.sampleFeature)) {
        return false;
    }

    if (!renderTarget) {
        return true;
    }

    if (format.renderFeature != GlesFeature::None) {
        return features.Has(format.renderFeature);
    }

    return format.renderable;
}

GlesVertexAttribute VertexAttributeFormat(VertexFormat format) noexcept {
    switch (format) {
    case VertexFormat::Float1:
        return {GL_FLOAT, 1, false, false, true};
    case VertexFormat::Float2:
        return {GL_FLOAT, 2, false, false, true};
    case VertexFormat::Float3:
        return {GL_FLOAT, 3, false, false, true};
    case VertexFormat::Float4:
        return {GL_FLOAT, 4, false, false, true};
    case VertexFormat::Byte4Snorm:
        return {GL_BYTE, 4, true, false, true};
    case VertexFormat::UByte4:
        return {GL_UNSIGNED_BYTE, 4, false, true, true};
    case VertexFormat::UByte4Unorm:
        return {GL_UNSIGNED_BYTE, 4, true, false, true};
    case VertexFormat::Bgra8Unorm:
        return {};
    case VertexFormat::Short2:
        return {GL_SHORT, 2, false, true, true};
    case VertexFormat::Short4:
        return {GL_SHORT, 4, false, true, true};
    case VertexFormat::Short2Snorm:
        return {GL_SHORT, 2, true, false, true};
    case VertexFormat::Short4Snorm:
        return {GL_SHORT, 4, true, false, true};
    case VertexFormat::Half2:
        return {GL_HALF_FLOAT, 2, false, false, true};
    case VertexFormat::Half4:
        return {GL_HALF_FLOAT, 4, false, false, true};
    case VertexFormat::UShort2:
        return {GL_UNSIGNED_SHORT, 2, false, true, true};
    case VertexFormat::UShort4:
        return {GL_UNSIGNED_SHORT, 4, false, true, true};
    case VertexFormat::UShort2Unorm:
        return {GL_UNSIGNED_SHORT, 2, true, false, true};
    case VertexFormat::UShort4Unorm:
        return {GL_UNSIGNED_SHORT, 4, true, false, true};
    case VertexFormat::Rgb10A2Unorm:
        return {GL_UNSIGNED_INT_2_10_10_10_REV, 4, true, false, true};
    case VertexFormat::UInt1:
        return {GL_UNSIGNED_INT, 1, false, true, true};
    }

    return {};
}

bool ShaderTypeOf(GLenum type, VertexShaderType& out) noexcept {
    switch (type) {
    case GL_FLOAT:
    case GL_FLOAT_VEC2:
    case GL_FLOAT_VEC3:
    case GL_FLOAT_VEC4:
        out = VertexShaderType::Float;
        return true;
    case GL_INT:
    case GL_INT_VEC2:
    case GL_INT_VEC3:
    case GL_INT_VEC4:
        out = VertexShaderType::Int;
        return true;
    case GL_UNSIGNED_INT:
    case GL_UNSIGNED_INT_VEC2:
    case GL_UNSIGNED_INT_VEC3:
    case GL_UNSIGNED_INT_VEC4:
        out = VertexShaderType::Uint;
        return true;
    default:
        return false;
    }
}

GLint MinFilter(Filter filter) noexcept {
    switch (filter) {
    case Filter::Nearest:
        return static_cast<GLint>(GL_NEAREST_MIPMAP_NEAREST);
    case Filter::Bilinear:
        return static_cast<GLint>(GL_LINEAR_MIPMAP_NEAREST);
    case Filter::Trilinear:
    case Filter::Anisotropic:
        return static_cast<GLint>(GL_LINEAR_MIPMAP_LINEAR);
    }

    return static_cast<GLint>(GL_LINEAR_MIPMAP_LINEAR);
}

GLint MagFilter(Filter filter) noexcept {
    return static_cast<GLint>(filter == Filter::Nearest ? GL_NEAREST : GL_LINEAR);
}

GLint WrapMode(AddressMode mode) noexcept {
    switch (mode) {
    case AddressMode::Wrap:
        return static_cast<GLint>(GL_REPEAT);
    case AddressMode::Clamp:
        return static_cast<GLint>(GL_CLAMP_TO_EDGE);
    case AddressMode::Mirror:
        return static_cast<GLint>(GL_MIRRORED_REPEAT);
    case AddressMode::Border:
        return static_cast<GLint>(GL_CLAMP_TO_BORDER);
    }

    return static_cast<GLint>(GL_REPEAT);
}

void Diagnostic::Put(char character) noexcept {
    if (length_ + 1 < sizeof(buffer_)) {
        buffer_[length_++] = character;
        buffer_[length_] = 0;
    } else {
        truncated_ = true;
    }
}

Diagnostic& Diagnostic::Text(const char* text) noexcept {
    while (text != nullptr && *text != 0) {
        Put(*text++);
    }

    return *this;
}

Diagnostic& Diagnostic::Text(StringView text) noexcept {
    for (usize index = 0; index < text.ByteCount(); ++index) {
        Put(text.Data()[index]);
    }

    return *this;
}

Diagnostic& Diagnostic::Number(uint64 value) noexcept {
    char digits[24] = {};
    usize count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);

    while (count > 0) {
        Put(digits[--count]);
    }

    return *this;
}

Diagnostic& Diagnostic::Hex(uint64 value) noexcept {
    constexpr char HexDigits[] = "0123456789ABCDEF";
    char digits[20] = {};
    usize count = 0;
    do {
        digits[count++] = HexDigits[value & 0xF];
        value >>= 4;
    } while (value != 0);

    Text("0x");
    while (count > 0) {
        Put(digits[--count]);
    }

    return *this;
}

} // namespace rocks::graphics::gles
