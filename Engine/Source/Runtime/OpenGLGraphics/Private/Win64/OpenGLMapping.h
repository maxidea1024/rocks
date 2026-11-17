#pragma once

#include "OpenGLApi.h"

#include <Graphics/DeviceTypes.h>

// Graphics 값(정점 형식·샘플러·픽셀 형식)을 GL 값으로 옮기는 표와 진단 문자열 조립 칸이다
// (OpenGLDevice.cpp에서 나눴다).
namespace rocks::graphics::opengl {

// 정점 원소 형식의 GL 속성 설정이다(RenderBase 10.4절 표, UE OpenGLVertexDeclaration.cpp:49–77).
// 정규화 없는 정수 형식은 glVertexArrayAttribIFormat(integer = true)로 설정한다(float 변환을
// 막는다). Bgra8Unorm은 ES 공통 API에 BGRA 정점 배열이 없어 supported = false다.
struct GlAttribute {
    GLenum type = 0;
    GLint size = 0;
    bool normalized = false;
    bool integer = false;
    bool supported = false;
};

inline GlAttribute AttributeFormat(VertexFormat format) noexcept {
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
        return {GL_HALF_FLOAT_VALUE, 2, false, false, true};
    case VertexFormat::Half4:
        return {GL_HALF_FLOAT_VALUE, 4, false, false, true};
    case VertexFormat::UShort2:
        return {GL_UNSIGNED_SHORT, 2, false, true, true};
    case VertexFormat::UShort4:
        return {GL_UNSIGNED_SHORT, 4, false, true, true};
    case VertexFormat::UShort2Unorm:
        return {GL_UNSIGNED_SHORT, 2, true, false, true};
    case VertexFormat::UShort4Unorm:
        return {GL_UNSIGNED_SHORT, 4, true, false, true};
    case VertexFormat::Rgb10A2Unorm:
        return {GL_UNSIGNED_INT_2_10_10_10_REV_VALUE, 4, true, false, true};
    case VertexFormat::UInt1:
        return {GL_UNSIGNED_INT, 1, false, true, true};
    }
    return {};
}

// active attribute 타입의 셰이더 타입 계열이다. 받을 수 없는 타입(double 등)은 false다.
inline bool ShaderTypeOf(GLenum type, VertexShaderType& out) noexcept {
    switch (type) {
    case GL_FLOAT:
    case GL_FLOAT_VEC2_VALUE:
    case GL_FLOAT_VEC3_VALUE:
    case GL_FLOAT_VEC4_VALUE:
        out = VertexShaderType::Float;
        return true;
    case GL_INT:
    case GL_INT_VEC2_VALUE:
    case GL_INT_VEC3_VALUE:
    case GL_INT_VEC4_VALUE:
        out = VertexShaderType::Int;
        return true;
    case GL_UNSIGNED_INT:
    case GL_UNSIGNED_INT_VEC2_VALUE:
    case GL_UNSIGNED_INT_VEC3_VALUE:
    case GL_UNSIGNED_INT_VEC4_VALUE:
        out = VertexShaderType::Uint;
        return true;
    default:
        return false;
    }
}

// 진단 문자열 조립 칸이다(셰이더 입력 대조 실패의 location·형식·셰이더 타입).
class Diagnostic {
public:
    Diagnostic& Text(const char* text) noexcept {
        while (*text != 0 && length_ + 1 < sizeof(buffer_)) {
            buffer_[length_++] = *text++;
        }
        buffer_[length_] = 0;
        return *this;
    }

    Diagnostic& Number(uint32 value) noexcept {
        char digits[12] = {};
        usize count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count > 0 && length_ + 1 < sizeof(buffer_)) {
            buffer_[length_++] = digits[--count];
        }
        buffer_[length_] = 0;
        return *this;
    }

    const char* CStr() const noexcept {
        return buffer_;
    }

private:
    char buffer_[256] = {};
    usize length_ = 0;
};

inline GLint MinFilter(Filter filter) noexcept {
    switch (filter) {
    case Filter::Nearest:
        return GL_NEAREST_MIPMAP_NEAREST;
    case Filter::Bilinear:
        return GL_LINEAR_MIPMAP_NEAREST;
    case Filter::Trilinear:
    case Filter::Anisotropic:
        return GL_LINEAR_MIPMAP_LINEAR;
    }
    return GL_LINEAR_MIPMAP_LINEAR;
}

inline GLint WrapMode(AddressMode mode) noexcept {
    switch (mode) {
    case AddressMode::Wrap:
        return GL_REPEAT;
    case AddressMode::Clamp:
        return static_cast<GLint>(GL_CLAMP_TO_EDGE_VALUE);
    case AddressMode::Mirror:
        return static_cast<GLint>(GL_MIRRORED_REPEAT_VALUE);
    case AddressMode::Border:
        return static_cast<GLint>(GL_CLAMP_TO_BORDER_VALUE);
    }
    return GL_REPEAT;
}

// clip 원점별 앞면 감김이다(Graphics 규약: 표시된 화면에서 반시계가 앞면). 두 원점 모두 GL_CCW다:
// GL_UPPER_LEFT는 window y를 뒤집지만 ARB_clip_control이 면 판정의 넓이 부호도 함께 뒤집으므로
// 감김은 NDC 기준 그대로이고, 창(GL_LOWER_LEFT)과 RenderTarget(GL_UPPER_LEFT) 모두 화면 반시계가
// 앞면이 된다(UE도 clip control에서 기본 GL_CCW에 CM_CW → GL_BACK, OpenGLState.cpp:50). Sandbox
// 컬링 시험이 두 경로를 픽셀로 대조한다(창을 GL_CW로 두면 창 경로가 offscreen과 달라진다).
inline GLenum FrontFaceFor(GLenum origin) noexcept {
    (void)origin;
    return GL_CCW;
}

inline GLenum InternalFormat(Format format) noexcept {
    switch (format) {
    case Format::RGBA8Unorm:
        return GL_RGBA8;
    case Format::RGBA8UnormSrgb:
        return GL_SRGB8_ALPHA8_VALUE;
    case Format::Unknown:
        break;
    }
    return 0;
}

} // namespace rocks::graphics::opengl
