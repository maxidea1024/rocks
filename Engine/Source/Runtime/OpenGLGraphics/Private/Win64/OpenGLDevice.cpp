#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Graphics/DeviceOwnership.h>
#include <Graphics/FenceTimeline.h>
#include <Graphics/StagingPool.h>
#include <Graphics/TimestampRing.h>
#include <OpenGLGraphics/OpenGLDevice.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>

#include <Windows.h>
#include <GL/gl.h>
#include <Platform/Memory.h>

namespace rocks::graphics {

namespace {

// --- GL 4.5 core에서 쓰는 형식·상수(Windows SDK의 gl.h는 1.1까지만 선언한다) ---

using GLchar = char;
using GLsizeiptr = signed long long;
using GLintptr = signed long long;
using GLuint64 = unsigned long long;
using GLint64 = long long;
using GLsync = struct __GLsync*;
using GLDEBUGPROC = void(APIENTRY*)(GLenum source, GLenum type, GLuint id, GLenum severity,
                                    GLsizei length, const GLchar* message, const void* user);

constexpr GLenum GL_FRAGMENT_SHADER_VALUE = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER_VALUE = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS_VALUE = 0x8B81;
constexpr GLenum GL_LINK_STATUS_VALUE = 0x8B82;
constexpr GLenum GL_INFO_LOG_LENGTH_VALUE = 0x8B84;
constexpr GLenum GL_SRGB8_ALPHA8_VALUE = 0x8C43;
constexpr GLenum GL_CLAMP_TO_EDGE_VALUE = 0x812F;
constexpr GLenum GL_FRAMEBUFFER_VALUE = 0x8D40;
constexpr GLenum GL_COLOR_ATTACHMENT0_VALUE = 0x8CE0;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE_VALUE = 0x8CD5;
constexpr GLenum GL_FRAMEBUFFER_SRGB_VALUE = 0x8DB9;
constexpr GLenum GL_LOWER_LEFT_VALUE = 0x8CA1;
constexpr GLenum GL_UPPER_LEFT_VALUE = 0x8CA2;
constexpr GLenum GL_ZERO_TO_ONE_VALUE = 0x935F;
constexpr GLenum GL_DEBUG_OUTPUT_VALUE = 0x92E0;
constexpr GLenum GL_DEBUG_OUTPUT_SYNCHRONOUS_VALUE = 0x8242;
constexpr GLenum GL_DEBUG_TYPE_ERROR_VALUE = 0x824C;
constexpr GLenum GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_VALUE = 0x824E;
constexpr GLenum GL_MAJOR_VERSION_VALUE = 0x821B;
constexpr GLenum GL_MINOR_VERSION_VALUE = 0x821C;
constexpr GLenum GL_NUM_EXTENSIONS_VALUE = 0x821D;
constexpr GLenum GL_MIRRORED_REPEAT_VALUE = 0x8370;
constexpr GLenum GL_SYNC_GPU_COMMANDS_COMPLETE_VALUE = 0x9117;
constexpr GLenum GL_HALF_FLOAT_VALUE = 0x140B;
constexpr GLenum GL_PIXEL_PACK_BUFFER_VALUE = 0x88EB;
// timestamp query(GL 3.3 ARB_timer_query)와 debug group(GL 4.3)이다.
constexpr GLenum GL_TIMESTAMP_VALUE = 0x8E28;
constexpr GLenum GL_QUERY_COUNTER_BITS_VALUE = 0x8864;
constexpr GLenum GL_QUERY_RESULT_VALUE = 0x8866;
constexpr GLenum GL_QUERY_RESULT_AVAILABLE_VALUE = 0x8867;
constexpr GLenum GL_GPU_DISJOINT_EXT_VALUE = 0x8FBB;
constexpr GLenum GL_DEBUG_SOURCE_APPLICATION_VALUE = 0x824A;
// glNamedBufferStorage: glNamedBufferSubData로 내용을 바꿀 수 있다.
constexpr GLbitfield GL_DYNAMIC_STORAGE_BIT_VALUE = 0x0100;
constexpr GLenum GL_UNSIGNED_INT_2_10_10_10_REV_VALUE = 0x8368;
constexpr GLenum GL_ACTIVE_ATTRIBUTES_VALUE = 0x8B89;
constexpr GLenum GL_FLOAT_VEC2_VALUE = 0x8B50;
constexpr GLenum GL_FLOAT_VEC3_VALUE = 0x8B51;
constexpr GLenum GL_FLOAT_VEC4_VALUE = 0x8B52;
constexpr GLenum GL_INT_VEC2_VALUE = 0x8B53;
constexpr GLenum GL_INT_VEC3_VALUE = 0x8B54;
constexpr GLenum GL_INT_VEC4_VALUE = 0x8B55;
constexpr GLenum GL_UNSIGNED_INT_VEC2_VALUE = 0x8DC6;
constexpr GLenum GL_UNSIGNED_INT_VEC3_VALUE = 0x8DC7;
constexpr GLenum GL_UNSIGNED_INT_VEC4_VALUE = 0x8DC8;
constexpr GLenum GL_ALREADY_SIGNALED_VALUE = 0x911A;
constexpr GLenum GL_TIMEOUT_EXPIRED_VALUE = 0x911B;
constexpr GLenum GL_CONDITION_SATISFIED_VALUE = 0x911C;
constexpr GLenum GL_CLAMP_TO_BORDER_VALUE = 0x812D;
// GL 4.6 core·ARB_texture_filter_anisotropic·EXT_texture_filter_anisotropic 공통 값이다.
constexpr GLenum GL_TEXTURE_MAX_ANISOTROPY_VALUE = 0x84FE;
constexpr GLenum GL_MAX_TEXTURE_MAX_ANISOTROPY_VALUE = 0x84FF;

constexpr int WGL_DRAW_TO_WINDOW_ARB = 0x2001;
constexpr int WGL_ACCELERATION_ARB = 0x2003;
constexpr int WGL_SUPPORT_OPENGL_ARB = 0x2010;
constexpr int WGL_DOUBLE_BUFFER_ARB = 0x2011;
constexpr int WGL_PIXEL_TYPE_ARB = 0x2013;
constexpr int WGL_COLOR_BITS_ARB = 0x2014;
constexpr int WGL_ALPHA_BITS_ARB = 0x201B;
constexpr int WGL_DEPTH_BITS_ARB = 0x2022;
constexpr int WGL_STENCIL_BITS_ARB = 0x2023;
constexpr int WGL_FULL_ACCELERATION_ARB = 0x2027;
constexpr int WGL_TYPE_RGBA_ARB = 0x202B;
constexpr int WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB = 0x20A9;
constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int WGL_CONTEXT_FLAGS_ARB = 0x2094;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x0001;
constexpr int WGL_CONTEXT_DEBUG_BIT_ARB = 0x0001;

using WglChoosePixelFormat = BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
using WglCreateContextAttribs = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using WglSwapInterval = BOOL(WINAPI*)(int);
using WglGetExtensionsString = const char*(WINAPI*)(HDC);

// 공백으로 나뉜 확장 목록에 name이 온전한 토큰으로 있는지다.
bool HasExtension(const char* list, const char* name) noexcept {
    if (list == nullptr) {
        return false;
    }
    const usize nameBytes = platform::CStringLength(name);
    for (const char* cursor = list; *cursor != '\0';) {
        while (*cursor == ' ') {
            ++cursor;
        }
        const char* end = cursor;
        while (*end != '\0' && *end != ' ') {
            ++end;
        }
        if (static_cast<usize>(end - cursor) == nameBytes &&
            platform::MemoryCompare(cursor, name, nameBytes) == 0) {
            return true;
        }
        cursor = end;
    }
    return false;
}

// 실행 시 얻는 GL 1.2+ 함수 표다.
struct GlFunctions {
    void(APIENTRY* CreateBuffers)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* NamedBufferStorage)(GLuint, GLsizeiptr, const void*, GLbitfield) = nullptr;
    void(APIENTRY* DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* CreateVertexArrays)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* DeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* BindVertexArray)(GLuint) = nullptr;
    void(APIENTRY* EnableVertexArrayAttrib)(GLuint, GLuint) = nullptr;
    void(APIENTRY* VertexArrayAttribFormat)(GLuint, GLuint, GLint, GLenum, GLboolean,
                                            GLuint) = nullptr;
    void(APIENTRY* VertexArrayAttribBinding)(GLuint, GLuint, GLuint) = nullptr;
    void(APIENTRY* VertexArrayVertexBuffer)(GLuint, GLuint, GLuint, GLintptr, GLsizei) = nullptr;
    void(APIENTRY* VertexArrayAttribIFormat)(GLuint, GLuint, GLint, GLenum, GLuint) = nullptr;
    void(APIENTRY* VertexArrayBindingDivisor)(GLuint, GLuint, GLuint) = nullptr;
    void(APIENTRY* VertexArrayElementBuffer)(GLuint, GLuint) = nullptr;
    void(APIENTRY* DrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei) = nullptr;
    void(APIENTRY* DrawElementsInstanced)(GLenum, GLsizei, GLenum, const void*, GLsizei) = nullptr;
    void(APIENTRY* GetActiveAttrib)(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*,
                                    GLchar*) = nullptr;
    GLint(APIENTRY* GetAttribLocation)(GLuint, const GLchar*) = nullptr;
    GLuint(APIENTRY* CreateShader)(GLenum) = nullptr;
    void(APIENTRY* ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
    void(APIENTRY* CompileShader)(GLuint) = nullptr;
    void(APIENTRY* GetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
    void(APIENTRY* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void(APIENTRY* DeleteShader)(GLuint) = nullptr;
    GLuint(APIENTRY* CreateProgram)() = nullptr;
    void(APIENTRY* AttachShader)(GLuint, GLuint) = nullptr;
    void(APIENTRY* LinkProgram)(GLuint) = nullptr;
    void(APIENTRY* GetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
    void(APIENTRY* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void(APIENTRY* DeleteProgram)(GLuint) = nullptr;
    void(APIENTRY* UseProgram)(GLuint) = nullptr;
    GLint(APIENTRY* GetUniformLocation)(GLuint, const GLchar*) = nullptr;
    void(APIENTRY* ProgramUniformMatrix4fv)(GLuint, GLint, GLsizei, GLboolean,
                                            const GLfloat*) = nullptr;
    void(APIENTRY* ProgramUniform4fv)(GLuint, GLint, GLsizei, const GLfloat*) = nullptr;
    void(APIENTRY* CreateTextures)(GLenum, GLsizei, GLuint*) = nullptr;
    void(APIENTRY* TextureStorage2D)(GLuint, GLsizei, GLenum, GLsizei, GLsizei) = nullptr;
    void(APIENTRY* TextureSubImage2D)(GLuint, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                      const void*) = nullptr;
    void(APIENTRY* TextureParameteri)(GLuint, GLenum, GLint) = nullptr;
    void(APIENTRY* TextureParameterf)(GLuint, GLenum, GLfloat) = nullptr;
    void(APIENTRY* TextureParameterfv)(GLuint, GLenum, const GLfloat*) = nullptr;
    const GLubyte*(APIENTRY* GetStringi)(GLenum, GLuint) = nullptr;
    void(APIENTRY* BindTextureUnit)(GLuint, GLuint) = nullptr;
    void(APIENTRY* CreateFramebuffers)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* NamedFramebufferTexture)(GLuint, GLenum, GLuint, GLint) = nullptr;
    GLenum(APIENTRY* CheckNamedFramebufferStatus)(GLuint, GLenum) = nullptr;
    void(APIENTRY* BindFramebuffer)(GLenum, GLuint) = nullptr;
    void(APIENTRY* BlitNamedFramebuffer)(GLuint, GLuint, GLint, GLint, GLint, GLint, GLint, GLint,
                                         GLint, GLint, GLbitfield, GLenum) = nullptr;
    void(APIENTRY* BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum) = nullptr;
    void(APIENTRY* ClipControl)(GLenum, GLenum) = nullptr;
    void(APIENTRY* DebugMessageCallback)(GLDEBUGPROC, const void*) = nullptr;
    GLsync(APIENTRY* FenceSync)(GLenum, GLbitfield) = nullptr;
    void(APIENTRY* NamedBufferSubData)(GLuint, GLintptr, GLsizeiptr, const void*) = nullptr;
    void(APIENTRY* GetNamedBufferSubData)(GLuint, GLintptr, GLsizeiptr, void*) = nullptr;
    void(APIENTRY* BindBuffer)(GLenum, GLuint) = nullptr;
    GLenum(APIENTRY* ClientWaitSync)(GLsync, GLbitfield, GLuint64) = nullptr;
    void(APIENTRY* DeleteSync)(GLsync) = nullptr;
    void(APIENTRY* CreateQueries)(GLenum, GLsizei, GLuint*) = nullptr;
    void(APIENTRY* DeleteQueries)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* QueryCounter)(GLuint, GLenum) = nullptr;
    void(APIENTRY* GetQueryiv)(GLenum, GLenum, GLint*) = nullptr;
    void(APIENTRY* GetQueryObjectiv)(GLuint, GLenum, GLint*) = nullptr;
    void(APIENTRY* GetQueryObjectui64v)(GLuint, GLenum, GLuint64*) = nullptr;
    void(APIENTRY* GetInteger64v)(GLenum, GLint64*) = nullptr;
    void(APIENTRY* PushDebugGroup)(GLenum, GLuint, GLsizei, const GLchar*) = nullptr;
    void(APIENTRY* PopDebugGroup)() = nullptr;
};

// wglGetProcAddress는 실패를 nullptr뿐 아니라 1·2·3·-1로도 알린다.
void* LoadGl(const char* name) noexcept {
    PROC procedure = wglGetProcAddress(name);
    const auto value = reinterpret_cast<intptr_t>(procedure);
    if (value == 0 || value == 1 || value == 2 || value == 3 || value == -1) {
        return nullptr;
    }
    return reinterpret_cast<void*>(procedure);
}

template <class Function>
bool Bind(Function& function, const char* name) noexcept {
    function = reinterpret_cast<Function>(LoadGl(name));
    return function != nullptr;
}

bool LoadFunctions(GlFunctions& gl) noexcept {
    return Bind(gl.CreateBuffers, "glCreateBuffers") &&
           Bind(gl.NamedBufferStorage, "glNamedBufferStorage") &&
           Bind(gl.DeleteBuffers, "glDeleteBuffers") &&
           Bind(gl.CreateVertexArrays, "glCreateVertexArrays") &&
           Bind(gl.DeleteVertexArrays, "glDeleteVertexArrays") &&
           Bind(gl.BindVertexArray, "glBindVertexArray") &&
           Bind(gl.EnableVertexArrayAttrib, "glEnableVertexArrayAttrib") &&
           Bind(gl.VertexArrayAttribFormat, "glVertexArrayAttribFormat") &&
           Bind(gl.VertexArrayAttribBinding, "glVertexArrayAttribBinding") &&
           Bind(gl.VertexArrayVertexBuffer, "glVertexArrayVertexBuffer") &&
           Bind(gl.VertexArrayAttribIFormat, "glVertexArrayAttribIFormat") &&
           Bind(gl.VertexArrayBindingDivisor, "glVertexArrayBindingDivisor") &&
           Bind(gl.VertexArrayElementBuffer, "glVertexArrayElementBuffer") &&
           Bind(gl.DrawArraysInstanced, "glDrawArraysInstanced") &&
           Bind(gl.DrawElementsInstanced, "glDrawElementsInstanced") &&
           Bind(gl.GetActiveAttrib, "glGetActiveAttrib") &&
           Bind(gl.GetAttribLocation, "glGetAttribLocation") &&
           Bind(gl.CreateShader, "glCreateShader") && Bind(gl.ShaderSource, "glShaderSource") &&
           Bind(gl.CompileShader, "glCompileShader") && Bind(gl.GetShaderiv, "glGetShaderiv") &&
           Bind(gl.GetShaderInfoLog, "glGetShaderInfoLog") &&
           Bind(gl.DeleteShader, "glDeleteShader") && Bind(gl.CreateProgram, "glCreateProgram") &&
           Bind(gl.AttachShader, "glAttachShader") && Bind(gl.LinkProgram, "glLinkProgram") &&
           Bind(gl.GetProgramiv, "glGetProgramiv") &&
           Bind(gl.GetProgramInfoLog, "glGetProgramInfoLog") &&
           Bind(gl.DeleteProgram, "glDeleteProgram") && Bind(gl.UseProgram, "glUseProgram") &&
           Bind(gl.GetUniformLocation, "glGetUniformLocation") &&
           Bind(gl.ProgramUniformMatrix4fv, "glProgramUniformMatrix4fv") &&
           Bind(gl.ProgramUniform4fv, "glProgramUniform4fv") &&
           Bind(gl.CreateTextures, "glCreateTextures") &&
           Bind(gl.TextureStorage2D, "glTextureStorage2D") &&
           Bind(gl.TextureSubImage2D, "glTextureSubImage2D") &&
           Bind(gl.TextureParameteri, "glTextureParameteri") &&
           Bind(gl.TextureParameterf, "glTextureParameterf") &&
           Bind(gl.TextureParameterfv, "glTextureParameterfv") &&
           Bind(gl.GetStringi, "glGetStringi") && Bind(gl.BindTextureUnit, "glBindTextureUnit") &&
           Bind(gl.CreateFramebuffers, "glCreateFramebuffers") &&
           Bind(gl.DeleteFramebuffers, "glDeleteFramebuffers") &&
           Bind(gl.NamedFramebufferTexture, "glNamedFramebufferTexture") &&
           Bind(gl.CheckNamedFramebufferStatus, "glCheckNamedFramebufferStatus") &&
           Bind(gl.BindFramebuffer, "glBindFramebuffer") &&
           Bind(gl.BlitNamedFramebuffer, "glBlitNamedFramebuffer") &&
           Bind(gl.BlendFuncSeparate, "glBlendFuncSeparate") &&
           Bind(gl.ClipControl, "glClipControl") &&
           Bind(gl.DebugMessageCallback, "glDebugMessageCallback") &&
           Bind(gl.FenceSync, "glFenceSync") &&
           Bind(gl.NamedBufferSubData, "glNamedBufferSubData") &&
           Bind(gl.GetNamedBufferSubData, "glGetNamedBufferSubData") &&
           Bind(gl.BindBuffer, "glBindBuffer") && Bind(gl.ClientWaitSync, "glClientWaitSync") &&
           Bind(gl.DeleteSync, "glDeleteSync") && Bind(gl.CreateQueries, "glCreateQueries") &&
           Bind(gl.DeleteQueries, "glDeleteQueries") && Bind(gl.QueryCounter, "glQueryCounter") &&
           Bind(gl.GetQueryiv, "glGetQueryiv") && Bind(gl.GetQueryObjectiv, "glGetQueryObjectiv") &&
           Bind(gl.GetQueryObjectui64v, "glGetQueryObjectui64v") &&
           Bind(gl.GetInteger64v, "glGetInteger64v") &&
           Bind(gl.PushDebugGroup, "glPushDebugGroup") && Bind(gl.PopDebugGroup, "glPopDebugGroup");
}

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

GlAttribute AttributeFormat(VertexFormat format) noexcept {
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
bool ShaderTypeOf(GLenum type, VertexShaderType& out) noexcept {
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

GLint MinFilter(Filter filter) noexcept {
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

GLint WrapMode(AddressMode mode) noexcept {
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
GLenum FrontFaceFor(GLenum origin) noexcept {
    (void)origin;
    return GL_CCW;
}

GLenum InternalFormat(Format format) noexcept {
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

constexpr wchar_t HiddenClassName[] = L"RocksOpenGLHidden";

HWND CreateHiddenWindow() noexcept {
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = HiddenClassName;
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }
    return CreateWindowExW(0, HiddenClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr,
                           nullptr, GetModuleHandleW(nullptr), nullptr);
}

struct BufferRecord {
    GLuint buffer = 0;
    uint64 size = 0;
};

struct TextureRecord {
    GLuint texture = 0;
    GLuint framebuffer = 0;
    uint32 width = 0;
    uint32 height = 0;
    uint32 mipLevels = 1;
    Format format = Format::Unknown;
};

struct PipelineRecord {
    GLuint program = 0;
    GLuint vertexArray = 0;
    GLint transformLocation = -1;
    GLint colorLocation = -1;
    // stream별 stride와 배치가 선언한 stream mask다.
    uint32 strides[MaxVertexStreams] = {};
    uint32 streamMask = 0;
    bool alphaBlend = false;
    CullMode cullMode = CullMode::None;
};

struct SwapChainRecord {
    HWND window = nullptr;
    HDC dc = nullptr;
    uint32 width = 0;
    uint32 height = 0;
    bool vsync = true;
    // 이 창에 마지막으로 적용한 swap interval이다(WGL swap interval은 창마다 따로다). -1은 미적용.
    int appliedInterval = -1;
};

// fence 완료 뒤에 지울 GL 객체다(장치 계약의 "OpenGL도 fence 뒤 회수").
enum class RetiredKind : uint8 {
    Buffer,
    Texture,
    Pipeline,
    SwapChain
};

struct RetiredObject {
    RetiredKind kind = RetiredKind::Buffer;
    // Buffer: 버퍼, Texture: 텍스처·framebuffer, Pipeline: program·정점 배열.
    GLuint first = 0;
    GLuint second = 0;
    // SwapChain: 창과 DC.
    HWND window = nullptr;
    HDC dc = nullptr;
    uint64 fenceValue = 0;
    // 기록 중인(아직 제출하지 않은) 프레임이 쓸 수 있다. EndFrame이 그 프레임의 fence 값을 정한다.
    bool pending = false;
};

// OpenGL staging은 CPU 메모리다(TextureStreaming 5.4절). client 메모리 업로드는 호출이 반환되면
// 다시 써도 되므로 fence를 기다리지 않는다.
class OpenGLStagingSource final : public StagingSource {
public:
    Result<StagingMemory> CreateStaging(uint64 bytes) noexcept override {
        auto memory = allocator->Allocate(static_cast<usize>(bytes), 64);
        if (!memory) {
            return Result<StagingMemory>::Failure(memory.Error());
        }
        StagingMemory staging;
        staging.resource = memory.Value();
        staging.data = static_cast<uint8*>(memory.Value());
        staging.size = bytes;
        return Result<StagingMemory>::Success(staging);
    }

    void DestroyStaging(const StagingMemory& memory) noexcept override {
        (void)allocator->Deallocate(memory.resource);
    }

    Allocator* allocator = nullptr;
};

// timestamp query 결과를 읽는다(GX9). Harvest가 fence 완료 뒤에만 부르지만 query 결과가 아직
// 보이지 않을 수 있으므로 가용 여부를 먼저 본다. GL_EXT_disjoint_timer_query가 있으면 끊김 표시를
// 읽어 그 묶음을 무효로 한다.
class OpenGLTimestampSource final : public TimestampSource {
public:
    TimestampReadState ReadTimestamps(uint32 first, uint32 count, uint64* ticks) noexcept override {
        for (uint32 index = 0; index < count; ++index) {
            GLint available = GL_FALSE;
            gl->GetQueryObjectiv(queries[first + index], GL_QUERY_RESULT_AVAILABLE_VALUE,
                                 &available);
            if (available == GL_FALSE) {
                return TimestampReadState::NotReady;
            }
        }

        for (uint32 index = 0; index < count; ++index) {
            GLuint64 value = 0;
            gl->GetQueryObjectui64v(queries[first + index], GL_QUERY_RESULT_VALUE, &value);
            ticks[index] = value;
        }

        if (disjointDetection) {
            GLint disjoint = GL_FALSE;
            glGetIntegerv(GL_GPU_DISJOINT_EXT_VALUE, &disjoint);
            if (disjoint != GL_FALSE) {
                return TimestampReadState::Disjoint;
            }
        }

        return TimestampReadState::Ready;
    }

    const GlFunctions* gl = nullptr;
    const GLuint* queries = nullptr;
    bool disjointDetection = false;
};

// 비동기 readback 하나다(pixel pack buffer). fenceValue 제출이 끝나면 읽을 수 있다.
struct ReadbackRecord {
    GLuint buffer = 0;
    uint64 fenceValue = 0;
    uint32 width = 0;
    uint32 height = 0;
};

// 관찰 스레드가 공유 context로 fence sync를 기다린다. sync 객체는 공유 그룹 전체에서 보이고 소유
// 스레드가 glFenceSync 뒤 glFlush하므로 다른 context에서 기다려도 끝난다.
class OpenGLFenceSource final : public FenceWaitSource {
public:
    void OnWatcherStart() noexcept override {
        current = wglMakeCurrent(dc, context) != FALSE;
    }

    void OnWatcherStop() noexcept override {
        if (current) {
            wglMakeCurrent(nullptr, nullptr);
            current = false;
        }
    }

    bool WaitSubmission(uint64 value, void* token) noexcept override {
        (void)value;
        // sync를 만들지 못한 제출은 소유 스레드가 glFinish로 끝냈다.
        if (token == nullptr) {
            return true;
        }
        if (!current) {
            return false;
        }
        for (;;) {
            const GLenum waited =
                clientWaitSync(static_cast<GLsync>(token), 0, WaitSliceNanoseconds);
            if (waited == GL_ALREADY_SIGNALED_VALUE || waited == GL_CONDITION_SATISFIED_VALUE) {
                return true;
            }
            if (waited != GL_TIMEOUT_EXPIRED_VALUE) {
                return false;
            }
        }
    }

    void ReleaseToken(void* token) noexcept override {
        if (token != nullptr) {
            deleteSync(static_cast<GLsync>(token));
        }
    }

    // glClientWaitSync 한 번의 최대 대기다. 끝나지 않았으면 다시 기다린다(완료 판정에 시간을 쓰지
    // 않는다).
    static constexpr GLuint64 WaitSliceNanoseconds = 1000000000ull;

    HDC dc = nullptr;
    HGLRC context = nullptr;
    GLenum(APIENTRY* clientWaitSync)(GLsync, GLbitfield, GLuint64) = nullptr;
    void(APIENTRY* deleteSync)(GLsync) = nullptr;
    bool current = false;
};

class OpenGLDevice final : public Device, private CommandContext {
public:
    explicit OpenGLDevice(Allocator& allocator) noexcept
        : adapterName_(allocator), lastError_(allocator), buffers_(allocator), textures_(allocator),
          pipelines_(allocator), swapChains_(allocator), retired_(allocator), readbacks_(allocator),
          timestamps_(allocator), timeline_(allocator, NewDeviceEpoch(), QueueKind::Graphics) {
        stagingSource_.allocator = &allocator;
    }

    ~OpenGLDevice() override {
        // 장치는 소유 스레드(또는 소유자가 없을 때 아무 스레드)에서 파괴한다.
        if (!ownership_.IsOwner() && !ownership_.Acquire()) {
            platform::Fatal("graphics device destroyed while another thread owns it");
        }
        const bool current = context_ != nullptr && MakeCurrent(deviceDc_);
        if (current) {
            glFinish();
            timeline_.Publish(fenceValue_);
        }
        // 모든 sync가 신호된 뒤(glFinish)이므로 관찰 스레드가 곧 빠져나온다.
        timeline_.StopWatcher();
        timeline_.Close();
        if (watcherContext_ != nullptr) {
            wglDeleteContext(watcherContext_);
        }
        if (watcherWindow_ != nullptr) {
            ReleaseDC(watcherWindow_, watcherDc_);
            DestroyWindow(watcherWindow_);
        }
        if (staging_) {
            staging_->Clear();
        }
        if (current) {
            if (timestampsReady_) {
                gl_.DeleteQueries(static_cast<GLsizei>(TimestampQueryCount), timestampQueries_);
            }
            for (const ReadbackRecord& record : readbacks_) {
                gl_.DeleteBuffers(1, &record.buffer);
            }
            for (RetiredObject& entry : retired_) {
                entry.pending = false;
            }
            DeleteRetired(NumericLimits<uint64>::Max());
            for (const BufferRecord& record : buffers_) {
                gl_.DeleteBuffers(1, &record.buffer);
            }
            for (const TextureRecord& record : textures_) {
                ReleaseTexture(record);
            }
            for (const PipelineRecord& record : pipelines_) {
                gl_.DeleteProgram(record.program);
                gl_.DeleteVertexArrays(1, &record.vertexArray);
            }
        }
        if (context_ != nullptr) {
            if (wglGetCurrentContext() == context_) {
                wglMakeCurrent(nullptr, nullptr);
            }
            wglDeleteContext(context_);
        }
        for (const SwapChainRecord& record : swapChains_) {
            ReleaseDC(record.window, record.dc);
        }
        for (const RetiredObject& entry : retired_) {
            if (entry.kind == RetiredKind::SwapChain) {
                ReleaseDC(entry.window, entry.dc);
            }
        }
        if (deviceWindow_ != nullptr) {
            ReleaseDC(deviceWindow_, deviceDc_);
            DestroyWindow(deviceWindow_);
        }
    }

    Result<void> Initialize(const DeviceDesc& desc) noexcept {
        // 1) 임시 창·legacy context로 WGL 확장 함수를 얻는다(픽셀 형식은 창마다 한 번만 정할 수
        // 있다).
        WglChoosePixelFormat choosePixelFormat = nullptr;
        WglCreateContextAttribs createContext = nullptr;
        {
            HWND dummy = CreateHiddenWindow();
            if (dummy == nullptr) {
                return Fail("OpenGL helper window creation failed");
            }
            HDC dc = GetDC(dummy);
            PIXELFORMATDESCRIPTOR pfd = {};
            pfd.nSize = sizeof(pfd);
            pfd.nVersion = 1;
            pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
            pfd.iPixelType = PFD_TYPE_RGBA;
            pfd.cColorBits = 32;
            const int format = ChoosePixelFormat(dc, &pfd);
            HGLRC legacy = nullptr;
            if (format != 0 && SetPixelFormat(dc, format, &pfd)) {
                legacy = wglCreateContext(dc);
            }
            if (legacy != nullptr && wglMakeCurrent(dc, legacy)) {
                choosePixelFormat =
                    reinterpret_cast<WglChoosePixelFormat>(LoadGl("wglChoosePixelFormatARB"));
                createContext =
                    reinterpret_cast<WglCreateContextAttribs>(LoadGl("wglCreateContextAttribsARB"));
                wglMakeCurrent(nullptr, nullptr);
            }
            if (legacy != nullptr) {
                wglDeleteContext(legacy);
            }
            ReleaseDC(dummy, dc);
            DestroyWindow(dummy);
        }
        if (choosePixelFormat == nullptr || createContext == nullptr) {
            return Fail("WGL_ARB_pixel_format / WGL_ARB_create_context are not available");
        }

        // 2) 장치 창에 sRGB 지원 픽셀 형식과 core 4.5 context를 만든다. 스왑체인 창도 같은 형식을
        // 쓴다.
        deviceWindow_ = CreateHiddenWindow();
        if (deviceWindow_ == nullptr) {
            return Fail("OpenGL device window creation failed");
        }
        deviceDc_ = GetDC(deviceWindow_);
        const int attributes[] = {
            WGL_DRAW_TO_WINDOW_ARB, TRUE, WGL_SUPPORT_OPENGL_ARB, TRUE, WGL_DOUBLE_BUFFER_ARB, TRUE,
            WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB, WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
            // 알파를 뺀 색 비트 수다(WGL_ARB_pixel_format).
            WGL_COLOR_BITS_ARB, 24, WGL_ALPHA_BITS_ARB, 8, WGL_DEPTH_BITS_ARB, 0,
            WGL_STENCIL_BITS_ARB, 0, WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB, TRUE, 0};
        UINT count = 0;
        if (!choosePixelFormat(deviceDc_, attributes, nullptr, 1, &pixelFormat_, &count) ||
            count == 0) {
            return Fail("no sRGB-capable double-buffered pixel format");
        }
        DescribePixelFormat(deviceDc_, pixelFormat_, sizeof(pixelDescriptor_), &pixelDescriptor_);
        if (!SetPixelFormat(deviceDc_, pixelFormat_, &pixelDescriptor_)) {
            return Fail("SetPixelFormat failed on the device window");
        }
        const int contextAttributes[] = {WGL_CONTEXT_MAJOR_VERSION_ARB,
                                         4,
                                         WGL_CONTEXT_MINOR_VERSION_ARB,
                                         5,
                                         WGL_CONTEXT_PROFILE_MASK_ARB,
                                         WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                                         WGL_CONTEXT_FLAGS_ARB,
                                         desc.enableValidation ? WGL_CONTEXT_DEBUG_BIT_ARB : 0,
                                         0};
        context_ = createContext(deviceDc_, nullptr, contextAttributes);
        if (context_ == nullptr || !MakeCurrent(deviceDc_)) {
            return Fail("OpenGL 4.5 core context creation failed");
        }
        if (!LoadFunctions(gl_)) {
            return Fail("required OpenGL 4.5 functions are missing");
        }
        // 확장 함수 주소는 픽셀 형식·context마다 다를 수 있으므로 실제 context에서 얻는다.
        const auto extensions =
            reinterpret_cast<WglGetExtensionsString>(LoadGl("wglGetExtensionsStringARB"));
        if (extensions != nullptr && HasExtension(extensions(deviceDc_), "WGL_EXT_swap_control")) {
            swapInterval_ = reinterpret_cast<WglSwapInterval>(LoadGl("wglSwapIntervalEXT"));
        }

        GLint major = 0;
        GLint minor = 0;
        glGetIntegerv(GL_MAJOR_VERSION_VALUE, &major);
        glGetIntegerv(GL_MINOR_VERSION_VALUE, &minor);
        if (major < 4 || (major == 4 && minor < 5)) {
            return Fail("OpenGL 4.5 is required");
        }
        // 이방성 필터: GL 4.6 core이거나 ARB/EXT 확장이 있으면 쓴다(없으면 Trilinear로 대신한다).
        bool anisotropic = major > 4 || (major == 4 && minor >= 6);
        GLint extensionCount = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS_VALUE, &extensionCount);
        bool disjointTimer = false;
        for (GLint index = 0; index < extensionCount; ++index) {
            const auto* name = reinterpret_cast<const char*>(
                gl_.GetStringi(GL_EXTENSIONS, static_cast<GLuint>(index)));
            if (name == nullptr) {
                continue;
            }
            anisotropic =
                anisotropic ||
                platform::CStringCompare(name, "GL_ARB_texture_filter_anisotropic") == 0 ||
                platform::CStringCompare(name, "GL_EXT_texture_filter_anisotropic") == 0;
            disjointTimer =
                disjointTimer || platform::CStringCompare(name, "GL_EXT_disjoint_timer_query") == 0;
        }
        if (anisotropic) {
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_VALUE, &maxAnisotropy_);
        }
        const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        if (renderer != nullptr) {
            auto assigned =
                adapterName_.TryAssign(StringView(renderer, platform::CStringLength(renderer)));
            (void)assigned;
        }

        // 3) fence 관찰 스레드용 공유 context를 숨김 창에 만든다(sync 객체를 공유한다).
        watcherWindow_ = CreateHiddenWindow();
        if (watcherWindow_ == nullptr) {
            return Fail("OpenGL fence watcher window creation failed");
        }
        watcherDc_ = GetDC(watcherWindow_);
        if (!SetPixelFormat(watcherDc_, pixelFormat_, &pixelDescriptor_)) {
            return Fail("SetPixelFormat failed on the fence watcher window");
        }
        watcherContext_ = createContext(watcherDc_, context_, contextAttributes);
        if (watcherContext_ == nullptr) {
            return Fail("OpenGL shared context for the fence watcher failed");
        }
        const uint64 stagingBudget =
            desc.stagingBudgetBytes != 0 ? desc.stagingBudgetBytes : DefaultStagingBudgetBytes;
        auto pool = UniquePtr<StagingPool>::TryMake(
            *stagingSource_.allocator, *stagingSource_.allocator, stagingSource_, stagingBudget);
        if (!pool) {
            return Result<void>::Failure(pool.Error());
        }
        staging_ = Move(pool.Value());
        fenceSource_.dc = watcherDc_;
        fenceSource_.context = watcherContext_;
        fenceSource_.clientWaitSync = gl_.ClientWaitSync;
        fenceSource_.deleteSync = gl_.DeleteSync;
        auto watching = timeline_.StartWatcher(fenceSource_, "OpenGLFence");
        if (!watching) {
            return Fail("fence watcher thread creation failed");
        }

        if (desc.enableValidation) {
            glEnable(GL_DEBUG_OUTPUT_VALUE);
            glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_VALUE);
            gl_.DebugMessageCallback(&OpenGLDevice::DebugMessage, this);
            validation_ = true;
        }
        InitializeTimestamps(disjointTimer);
        SetClipOrigin(GL_LOWER_LEFT_VALUE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        // RGBA8에서도 dither가 허용되므로 backend 간 결과가 ±1 달라지지 않게 끈다.
        glDisable(GL_DITHER);

        const uint8 white[4] = {255, 255, 255, 255};
        TextureDesc whiteDesc;
        whiteDesc.width = 1;
        whiteDesc.height = 1;
        whiteDesc.format = Format::RGBA8Unorm;
        auto whiteTexture = CreateTexture(whiteDesc, Span<const uint8>(white, 4));
        if (!whiteTexture) {
            return Result<void>::Failure(whiteTexture.Error());
        }
        white_ = whiteTexture.Value();
        return Result<void>::Success();
    }

    DeviceInfo Info() const noexcept override {
        DeviceInfo info;
        info.backend = Backend::OpenGL;
        info.adapterName = adapterName_.View();
        info.deviceEpoch = timeline_.DeviceEpoch();
        info.concurrentResourceCreation = false;
        info.validationEnabled = validation_;
        // GL_TIMESTAMP 값은 ns다. counter bit가 0이면 timestamp가 없다(추정값을 만들지 않는다).
        info.timestamps = timestampsReady_;
        info.timestampFrequency = timestampsReady_ ? TimestampNanosecondFrequency : 0;
        info.timestampDisjointDetection = timestampsReady_ && timestampSource_.disjointDetection;
        info.clockCalibration = timestampsReady_;
        info.debugMarkers = true;
        return info;
    }

    Result<SwapChainHandle> CreateSwapChain(const SwapChainDesc& desc) noexcept override {
        auto owner = ownership_.Check();
        if (!owner) {
            return Result<SwapChainHandle>::Failure(owner.Error());
        }

        if (desc.surface.window == nullptr || desc.width == 0 || desc.height == 0) {
            return Result<SwapChainHandle>::Failure(ErrorCode::InvalidArgument);
        }

        SwapChainRecord record;
        record.window = static_cast<HWND>(desc.surface.window);
        // DC를 스왑체인 수명 동안 쥐므로 공용 DC가 아니라 창 전용 DC(CS_OWNDC)여야 한다.
        if ((GetClassLongPtrW(record.window, GCL_STYLE) & CS_OWNDC) == 0) {
            auto failed = Fail("swap chain window class must use CS_OWNDC");
            (void)failed;
            return Result<SwapChainHandle>::Failure(ErrorCode::InvalidArgument);
        }
        record.dc = GetDC(record.window);
        if (record.dc == nullptr) {
            return FailWith<SwapChainHandle>("GetDC failed on the swap chain window");
        }
        record.width = desc.width;
        record.height = desc.height;
        record.vsync = desc.vsync;
        // 창의 픽셀 형식은 한 번만 정할 수 있다. 이미 다른 형식이면 이 context로 그릴 수 없다.
        const int existing = GetPixelFormat(record.dc);
        if ((existing != 0 && existing != pixelFormat_) ||
            (existing == 0 && !SetPixelFormat(record.dc, pixelFormat_, &pixelDescriptor_))) {
            ReleaseDC(record.window, record.dc);
            return FailWith<SwapChainHandle>(
                "window pixel format is incompatible with the context");
        }

        auto added = swapChains_.TryAdd(record);
        if (!added) {
            ReleaseDC(record.window, record.dc);
            return Result<SwapChainHandle>::Failure(added.Error());
        }
        return Result<SwapChainHandle>::Success(SwapChainHandle{added.Value()});
    }

    Result<void> ResizeSwapChain(SwapChainHandle handle, uint32 width,
                                 uint32 height) noexcept override {
        auto owner = ownership_.Check();
        if (!owner) {
            return Result<void>::Failure(owner.Error());
        }

        SwapChainRecord* record = swapChains_.Find(handle.slot);
        if (record == nullptr) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }
        if (width == 0 || height == 0 || recording_) {
            return Result<void>::Failure(recording_ ? ErrorCode::Busy : ErrorCode::InvalidArgument);
        }
        // 기본 framebuffer는 창 크기를 따른다. 크기만 기록한다(BeginFrame은 실제 창 크기를 쓴다).
        record->width = width;
        record->height = height;
        return Result<void>::Success();
    }

    Result<BufferHandle> CreateBuffer(const BufferDesc& desc,
                                      Span<const uint8> initial) noexcept override {
        auto owner = ownership_.Check();
        if (!owner) {
            return Result<BufferHandle>::Failure(owner.Error());
        }

        if (desc.size == 0 || initial.Size() != desc.size ||
            (desc.usage != BufferUsage::Vertex && desc.usage != BufferUsage::Index)) {
            return Result<BufferHandle>::Failure(ErrorCode::InvalidArgument);
        }
        if (!MakeCurrentForResources()) {
            return FailWith<BufferHandle>("wglMakeCurrent failed");
        }
        BufferRecord record;
        record.size = desc.size;
        gl_.CreateBuffers(1, &record.buffer);
        gl_.NamedBufferStorage(record.buffer, static_cast<GLsizeiptr>(desc.size), initial.Data(),
                               GL_DYNAMIC_STORAGE_BIT_VALUE);
        auto added = buffers_.TryAdd(record);
        if (!added) {
            gl_.DeleteBuffers(1, &record.buffer);
            return Result<BufferHandle>::Failure(added.Error());
        }
        return Result<BufferHandle>::Success(BufferHandle{added.Value()});
    }

    Result<TextureHandle> CreateTexture(const TextureDesc& desc,
                                        Span<const uint8> pixels) noexcept override {
        auto owner = ownership_.Check();
        if (!owner) {
            return Result<TextureHandle>::Failure(owner.Error());
        }

        const uint32 bytesPerPixel = BytesPerPixel(desc.format);
        const bool renderTarget = desc.usage == TextureUsage::RenderTarget;
        const uint64 size = MipChainBytes(desc.width, desc.height, desc.mipLevels, desc.format);
        if (desc.width > 16384 || desc.height > 16384 || size == 0 ||
            !IsValidSamplerDesc(desc.sampler) ||
            (renderTarget ? (pixels.Size() != 0 || desc.mipLevels != 1) : pixels.Size() != size)) {
            return Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
        }

        if (!MakeCurrentForResources()) {
            return FailWith<TextureHandle>("wglMakeCurrent failed");
        }
        TextureRecord record;
        record.width = desc.width;
        record.height = desc.height;
        record.mipLevels = desc.mipLevels;
        record.format = desc.format;
        gl_.CreateTextures(GL_TEXTURE_2D, 1, &record.texture);
        gl_.TextureStorage2D(record.texture, static_cast<GLsizei>(desc.mipLevels),
                             InternalFormat(desc.format), static_cast<GLsizei>(desc.width),
                             static_cast<GLsizei>(desc.height));
        // 샘플러 상태는 텍스처 객체에 둔다(텍스처마다 하나의 SamplerDesc).
        gl_.TextureParameteri(record.texture, GL_TEXTURE_MIN_FILTER,
                              MinFilter(desc.sampler.filter));
        gl_.TextureParameteri(record.texture, GL_TEXTURE_MAG_FILTER,
                              desc.sampler.filter == Filter::Nearest ? GL_NEAREST : GL_LINEAR);
        gl_.TextureParameteri(record.texture, GL_TEXTURE_WRAP_S, WrapMode(desc.sampler.addressU));
        gl_.TextureParameteri(record.texture, GL_TEXTURE_WRAP_T, WrapMode(desc.sampler.addressV));
        float border[4];
        BorderColorValue(desc.sampler.borderColor, border);
        gl_.TextureParameterfv(record.texture, GL_TEXTURE_BORDER_COLOR, border);
        if (desc.sampler.filter == Filter::Anisotropic && maxAnisotropy_ >= 1.0f) {
            const float requested = static_cast<float>(desc.sampler.maxAnisotropy);
            gl_.TextureParameterf(record.texture, GL_TEXTURE_MAX_ANISOTROPY_VALUE,
                                  requested < maxAnisotropy_ ? requested : maxAnisotropy_);
        }
        if (!renderTarget) {
            // 첫 행을 v = 0에 둔다. UV (0,0)이 이미지 좌상단이 되어 D3D12와 결과가 같다.
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            usize offset = 0;
            for (uint32 level = 0; level < desc.mipLevels; ++level) {
                const uint32 width = MipExtent(desc.width, level);
                const uint32 height = MipExtent(desc.height, level);
                gl_.TextureSubImage2D(record.texture, static_cast<GLint>(level), 0, 0,
                                      static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                                      GL_RGBA, GL_UNSIGNED_BYTE, pixels.Data() + offset);
                offset += static_cast<usize>(width) * height * bytesPerPixel;
            }
        } else {
            gl_.CreateFramebuffers(1, &record.framebuffer);
            gl_.NamedFramebufferTexture(record.framebuffer, GL_COLOR_ATTACHMENT0_VALUE,
                                        record.texture, 0);
            if (gl_.CheckNamedFramebufferStatus(record.framebuffer, GL_FRAMEBUFFER_VALUE) !=
                GL_FRAMEBUFFER_COMPLETE_VALUE) {
                ReleaseTexture(record);
                return FailWith<TextureHandle>("render target framebuffer is incomplete");
            }
        }

        auto added = textures_.TryAdd(record);
        if (!added) {
            ReleaseTexture(record);
            return Result<TextureHandle>::Failure(added.Error());
        }
        return Result<TextureHandle>::Success(TextureHandle{added.Value()});
    }

    Result<PipelineHandle> CreatePipeline(const PipelineDesc& desc) noexcept override {
        auto owner = ownership_.Check();
        if (!owner) {
            return Result<PipelineHandle>::Failure(owner.Error());
        }

        if (desc.vertex.glsl.ByteCount() == 0 || desc.pixel.glsl.ByteCount() == 0 ||
            InternalFormat(desc.colorFormat) == 0) {
            return Result<PipelineHandle>::Failure(ErrorCode::InvalidArgument);
        }
        if (!ValidateVertexLayout(desc.layout)) {
            auto failed = Fail("vertex layout is invalid (RenderBase 10.4 limits)");
            (void)failed;
            return Result<PipelineHandle>::Failure(ErrorCode::InvalidArgument);
        }
        for (const VertexAttribute& attribute : desc.layout.attributes) {
            if (!AttributeFormat(attribute.format).supported) {
                Diagnostic message;
                message.Text("vertex format ")
                    .Text(VertexFormatName(attribute.format))
                    .Text(" is not supported by the OpenGL backend");
                auto rejected = Reject(message, ErrorCode::Unsupported);
                return Result<PipelineHandle>::Failure(rejected.Error());
            }
        }

        if (!MakeCurrentForResources()) {
            return FailWith<PipelineHandle>("wglMakeCurrent failed");
        }
        auto vertex = CompileShader(GL_VERTEX_SHADER_VALUE, desc.vertex.glsl);
        if (!vertex) {
            return Result<PipelineHandle>::Failure(vertex.Error());
        }
        auto pixel = CompileShader(GL_FRAGMENT_SHADER_VALUE, desc.pixel.glsl);
        if (!pixel) {
            gl_.DeleteShader(vertex.Value());
            return Result<PipelineHandle>::Failure(pixel.Error());
        }

        PipelineRecord record;
        record.program = gl_.CreateProgram();
        gl_.AttachShader(record.program, vertex.Value());
        gl_.AttachShader(record.program, pixel.Value());
        gl_.LinkProgram(record.program);
        gl_.DeleteShader(vertex.Value());
        gl_.DeleteShader(pixel.Value());
        GLint linked = GL_FALSE;
        gl_.GetProgramiv(record.program, GL_LINK_STATUS_VALUE, &linked);
        if (linked != GL_TRUE) {
            StoreLog(record.program, false);
            gl_.DeleteProgram(record.program);
            return Result<PipelineHandle>::Failure(ErrorCode::DependencyFailed);
        }
        auto matched = MatchVertexInputs(record.program, desc.layout);
        if (!matched) {
            gl_.DeleteProgram(record.program);
            return Result<PipelineHandle>::Failure(matched.Error());
        }
        record.transformLocation = gl_.GetUniformLocation(record.program, "uTransform");
        record.colorLocation = gl_.GetUniformLocation(record.program, "uColor");
        record.alphaBlend = desc.alphaBlend;
        record.cullMode = desc.cullMode;
        for (usize index = 0; index < desc.layout.streams.Size(); ++index) {
            record.strides[index] = desc.layout.streams[index].stride;
            record.streamMask |= 1u << index;
        }

        // 속성이 없는 배치도 core profile 그리기에는 정점 배열 객체가 있어야 한다.
        gl_.CreateVertexArrays(1, &record.vertexArray);
        for (const VertexAttribute& attribute : desc.layout.attributes) {
            const GlAttribute format = AttributeFormat(attribute.format);
            gl_.EnableVertexArrayAttrib(record.vertexArray, attribute.location);
            if (format.integer) {
                gl_.VertexArrayAttribIFormat(record.vertexArray, attribute.location, format.size,
                                             format.type, attribute.offset);
            } else {
                gl_.VertexArrayAttribFormat(record.vertexArray, attribute.location, format.size,
                                            format.type, format.normalized ? GL_TRUE : GL_FALSE,
                                            attribute.offset);
            }
            gl_.VertexArrayAttribBinding(record.vertexArray, attribute.location, attribute.stream);
        }
        for (usize index = 0; index < desc.layout.streams.Size(); ++index) {
            gl_.VertexArrayBindingDivisor(
                record.vertexArray, static_cast<GLuint>(index),
                desc.layout.streams[index].stepRate == VertexStepRate::Instance ? 1u : 0u);
        }

        auto added = pipelines_.TryAdd(record);
        if (!added) {
            gl_.DeleteProgram(record.program);
            gl_.DeleteVertexArrays(1, &record.vertexArray);
            return Result<PipelineHandle>::Failure(added.Error());
        }
        return Result<PipelineHandle>::Success(PipelineHandle{added.Value()});
    }

    Result<CommandContext*> BeginFrame(SwapChainHandle handle,
                                       const ClearColor& clear) noexcept override {
        auto owner = ownership_.Check();
        if (!owner) {
            return Result<CommandContext*>::Failure(owner.Error());
        }

        SwapChainRecord* record = swapChains_.Find(handle.slot);
        if (record == nullptr) {
            return Result<CommandContext*>::Failure(ErrorCode::StaleHandle);
        }
        if (recording_) {
            return Result<CommandContext*>::Failure(ErrorCode::Busy);
        }
        if (timeline_.IsLost()) {
            return Result<CommandContext*>::Failure(ErrorCode::Destroyed);
        }
        if (!MakeCurrent(record->dc)) {
            return FailWith<CommandContext*>("wglMakeCurrent failed on the swap chain window");
        }
        const int interval = record->vsync ? 1 : 0;
        if (swapInterval_ != nullptr && record->appliedInterval != interval &&
            swapInterval_(interval)) {
            record->appliedInterval = interval;
        }
        gl_.BindFramebuffer(GL_FRAMEBUFFER_VALUE, 0);
        // 기본 framebuffer는 창 아래쪽이 원점이다.
        SetClipOrigin(GL_LOWER_LEFT_VALUE);
        presentTarget_ = handle;
        // 기본 framebuffer는 창 크기를 즉시 따르므로 ResizeSwapChain 전 프레임도 실제 크기로
        // 그린다.
        uint32 width = record->width;
        uint32 height = record->height;
        RECT client = {};
        if (GetClientRect(record->window, &client) && client.right > client.left &&
            client.bottom > client.top) {
            width = static_cast<uint32>(client.right - client.left);
            height = static_cast<uint32>(client.bottom - client.top);
        }
        pass_ = {0, width, height, Format::RGBA8UnormSrgb, {}};
        StartPass(width, height, clear);
        return Result<CommandContext*>::Success(this);
    }

    Result<CommandContext*> BeginOffscreen(TextureHandle handle,
                                           const ClearColor& clear) noexcept override {
        auto owner = ownership_.Check();
        if (!owner) {
            return Result<CommandContext*>::Failure(owner.Error());
        }

        TextureRecord* record = textures_.Find(handle.slot);
        if (record == nullptr || record->framebuffer == 0) {
            return Result<CommandContext*>::Failure(record == nullptr ? ErrorCode::StaleHandle
                                                                      : ErrorCode::InvalidArgument);
        }
        if (recording_) {
            return Result<CommandContext*>::Failure(ErrorCode::Busy);
        }
        if (timeline_.IsLost()) {
            return Result<CommandContext*>::Failure(ErrorCode::Destroyed);
        }
        if (!MakeCurrentForResources()) {
            return FailWith<CommandContext*>("wglMakeCurrent failed");
        }
        gl_.BindFramebuffer(GL_FRAMEBUFFER_VALUE, record->framebuffer);
        // FBO 텍스처의 첫 행(t = 0)을 화면 위(clip y = +1)에 두어 D3D12와 같은 배치로 만든다. 그
        // 결과 이 텍스처를 UV (0,0) = 좌상단으로 샘플링해도, 첫 행부터 읽어도 D3D12와 같다.
        SetClipOrigin(GL_UPPER_LEFT_VALUE);
        presentTarget_ = {};
        pass_ = {record->framebuffer, record->width, record->height, record->format, handle};
        StartPass(record->width, record->height, clear);
        return Result<CommandContext*>::Success(this);
    }

    Result<GpuFencePoint> EndFrame() noexcept override {
        auto owner = ownership_.Check();
        if (!owner) {
            return Result<GpuFencePoint>::Failure(owner.Error());
        }

        if (!recording_) {
            return Result<GpuFencePoint>::Failure(ErrorCode::InvalidArgument);
        }
        recording_ = false;
        gl_.BindVertexArray(0);
        gl_.UseProgram(0);
        Result<void> outcome = Result<void>::Success();
        if (!presentTarget_.IsNull()) {
            const SwapChainRecord* record = swapChains_.Find(presentTarget_.slot);
            if (record != nullptr && !SwapBuffers(record->dc)) {
                outcome = Fail("SwapBuffers failed");
            }
        }
        gl_.BindFramebuffer(GL_FRAMEBUFFER_VALUE, 0);
        for (; markerDepth_ != 0; --markerDepth_) {
            gl_.PopDebugGroup();
        }
        const uint64 value = Submit();
        timestamps_.Close(value, true);
        AssignPendingRetires(value);
        DeleteRetired(timeline_.Completed());
        if (!outcome) {
            return Result<GpuFencePoint>::Failure(outcome.Error());
        }
        return Result<GpuFencePoint>::Success(timeline_.Point(value));
    }

    Result<void> ReadPixels(TextureHandle handle, Span<uint8> out) noexcept override {
        auto owner = ownership_.Check();
        if (!owner) {
            return Result<void>::Failure(owner.Error());
        }

        TextureRecord* record = textures_.Find(handle.slot);
        if (record == nullptr) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }
        const usize rowBytes = static_cast<usize>(record->width) * BytesPerPixel(record->format);
        if (record->framebuffer == 0 || out.Size() != rowBytes * record->height || recording_) {
            return Result<void>::Failure(recording_ ? ErrorCode::Busy : ErrorCode::InvalidArgument);
        }
        if (!MakeCurrentForResources()) {
            return Fail("wglMakeCurrent failed");
        }
        gl_.BindFramebuffer(GL_FRAMEBUFFER_VALUE, record->framebuffer);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        // offscreen 패스는 위쪽 원점(GL_UPPER_LEFT)으로 그렸으므로 저장소 첫 행이 이미지 위쪽이다.
        glReadPixels(0, 0, static_cast<GLsizei>(record->width),
                     static_cast<GLsizei>(record->height), GL_RGBA, GL_UNSIGNED_BYTE, out.Data());
        gl_.BindFramebuffer(GL_FRAMEBUFFER_VALUE, 0);
        return Result<void>::Success();
    }

    void Retire(BufferHandle handle) noexcept override {
        if (!ownership_.Check()) {
            return;
        }

        const BufferRecord* record = buffers_.Find(handle.slot);
        if (record == nullptr) {
            return;
        }
        RetiredObject entry;
        entry.kind = RetiredKind::Buffer;
        entry.first = record->buffer;
        Defer(entry);
        auto removed = buffers_.TryRemove(handle.slot);
        (void)removed;
    }

    void Retire(TextureHandle handle) noexcept override {
        if (!ownership_.Check()) {
            return;
        }

        const TextureRecord* record = textures_.Find(handle.slot);
        if (record == nullptr || handle == white_) {
            return;
        }
        RetiredObject entry;
        entry.kind = RetiredKind::Texture;
        entry.first = record->texture;
        entry.second = record->framebuffer;
        Defer(entry);
        auto removed = textures_.TryRemove(handle.slot);
        (void)removed;
    }

    void Retire(PipelineHandle handle) noexcept override {
        if (!ownership_.Check()) {
            return;
        }

        const PipelineRecord* record = pipelines_.Find(handle.slot);
        if (record == nullptr) {
            return;
        }
        RetiredObject entry;
        entry.kind = RetiredKind::Pipeline;
        entry.first = record->program;
        entry.second = record->vertexArray;
        Defer(entry);
        auto removed = pipelines_.TryRemove(handle.slot);
        (void)removed;
    }

    void Retire(SwapChainHandle handle) noexcept override {
        if (!ownership_.Check()) {
            return;
        }

        SwapChainRecord* record = swapChains_.Find(handle.slot);
        if (record == nullptr) {
            return;
        }
        // 이 창 DC에 context가 붙어 있으면 장치 창으로 옮긴다. DC는 fence 뒤에 놓는다.
        if (wglGetCurrentContext() == context_ && wglGetCurrentDC() == record->dc &&
            !MakeCurrent(deviceDc_)) {
            wglMakeCurrent(nullptr, nullptr);
        }
        RetiredObject entry;
        entry.kind = RetiredKind::SwapChain;
        entry.window = record->window;
        entry.dc = record->dc;
        Defer(entry);
        auto removed = swapChains_.TryRemove(handle.slot);
        (void)removed;
    }

    void ReclaimCompleted() noexcept override {
        if (!ownership_.Check()) {
            return;
        }

        DeleteRetired(timeline_.Completed());
    }

    Result<void> WaitIdle() noexcept override {
        auto owner = ownership_.Check();
        if (!owner) {
            return Result<void>::Failure(owner.Error());
        }

        if (!MakeCurrentForResources()) {
            return Fail("wglMakeCurrent failed");
        }
        glFinish();
        // glFinish 뒤에는 앞선 모든 sync가 신호되었다. 관찰 스레드를 기다리지 않고 바로 게시한다.
        timeline_.Publish(fenceValue_);
        DeleteRetired(fenceValue_);
        if (timeline_.IsLost()) {
            return Result<void>::Failure(ErrorCode::Destroyed);
        }
        return Result<void>::Success();
    }

    // --- 업로드·readback(GX8) ---
    // OpenGL은 드라이버가 업로드 순서를 지키므로 쌓아 둘 기록이 없다. 갱신은 바로
    // glNamedBufferSubData· glTextureSubImage2D로 보내고 staging은 CPU 메모리다.

    Result<void> UpdateBuffer(BufferHandle handle, uint64 offset,
                              Span<const uint8> data) noexcept override {
        auto owner = ownership_.Check();
        if (!owner) {
            return owner;
        }
        const BufferRecord* record = buffers_.Find(handle.slot);
        if (record == nullptr) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }
        if (data.Size() == 0 || offset > record->size || data.Size() > record->size - offset) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        if (!MakeCurrentForResources()) {
            return Fail("wglMakeCurrent failed");
        }
        gl_.NamedBufferSubData(record->buffer, static_cast<GLintptr>(offset),
                               static_cast<GLsizeiptr>(data.Size()), data.Data());
        return Result<void>::Success();
    }

    Result<void> UpdateTexture(TextureHandle handle, uint32 mipLevel,
                               Span<const uint8> pixels) noexcept override {
        auto owner = ownership_.Check();
        if (!owner) {
            return owner;
        }
        const TextureRecord* record = textures_.Find(handle.slot);
        if (record == nullptr) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }
        const uint32 width = MipExtent(record->width, mipLevel);
        const uint32 height = MipExtent(record->height, mipLevel);
        if (mipLevel >= record->mipLevels ||
            pixels.Size() != static_cast<usize>(width) * height * BytesPerPixel(record->format)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        if (!MakeCurrentForResources()) {
            return Fail("wglMakeCurrent failed");
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl_.TextureSubImage2D(record->texture, static_cast<GLint>(mipLevel), 0, 0,
                              static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA,
                              GL_UNSIGNED_BYTE, pixels.Data());
        return Result<void>::Success();
    }

    Result<GpuFencePoint> SubmitUploads() noexcept override {
        auto owner = ownership_.Check();
        if (!owner) {
            return Result<GpuFencePoint>::Failure(owner.Error());
        }
        if (recording_) {
            return Result<GpuFencePoint>::Failure(ErrorCode::Busy);
        }
        return Result<GpuFencePoint>::Success(GpuFencePoint{});
    }

    UploadStats UploadStatistics() const noexcept override {
        return UploadStats{};
    }

    Result<StagingBuffer> AcquireStaging(uint64 bytes) noexcept override {
        return staging_->Acquire(bytes);
    }

    Result<void> UploadTextureFromStaging(TextureHandle handle, uint32 mipLevel,
                                          StagingHandle staging, uint64 offset) noexcept override {
        auto memory = staging_->Find(staging);
        const TextureRecord* record = textures_.Find(handle.slot);
        if (!memory || record == nullptr) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }
        const uint64 bytes = static_cast<uint64>(MipExtent(record->width, mipLevel)) *
                             MipExtent(record->height, mipLevel) * BytesPerPixel(record->format);
        if (offset > memory.Value().size || bytes > memory.Value().size - offset) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        return UpdateTexture(
            handle, mipLevel,
            Span<const uint8>(memory.Value().data + offset, static_cast<usize>(bytes)));
    }

    Result<void> UploadBufferFromStaging(BufferHandle handle, uint64 destinationOffset,
                                         StagingHandle staging, uint64 sourceOffset,
                                         uint64 size) noexcept override {
        auto memory = staging_->Find(staging);
        if (!memory) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }
        if (sourceOffset > memory.Value().size || size > memory.Value().size - sourceOffset) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        return UpdateBuffer(
            handle, destinationOffset,
            Span<const uint8>(memory.Value().data + sourceOffset, static_cast<usize>(size)));
    }

    void ReleaseStaging(StagingHandle staging) noexcept override {
        // client 메모리 업로드는 반환하면 끝났으므로 바로 재사용할 수 있다.
        staging_->Release(staging);
        staging_->Reclaim(NumericLimits<uint64>::Max());
    }

    StagingStats StagingStatistics() const noexcept override {
        return staging_->Stats();
    }

    Result<ReadbackRequest> RequestReadback(TextureHandle target) noexcept override {
        auto owner = ownership_.Check();
        if (!owner) {
            return Result<ReadbackRequest>::Failure(owner.Error());
        }
        const TextureRecord* record = textures_.Find(target.slot);
        if (record == nullptr) {
            return Result<ReadbackRequest>::Failure(ErrorCode::StaleHandle);
        }
        if (record->framebuffer == 0 || recording_) {
            return Result<ReadbackRequest>::Failure(recording_ ? ErrorCode::Busy
                                                               : ErrorCode::InvalidArgument);
        }
        if (!MakeCurrentForResources()) {
            return FailWith<ReadbackRequest>("wglMakeCurrent failed");
        }
        ReadbackRecord readback;
        readback.width = record->width;
        readback.height = record->height;
        const uint64 bytes =
            static_cast<uint64>(record->width) * record->height * BytesPerPixel(record->format);
        gl_.CreateBuffers(1, &readback.buffer);
        gl_.NamedBufferStorage(readback.buffer, static_cast<GLsizeiptr>(bytes), nullptr, 0);
        gl_.BindFramebuffer(GL_FRAMEBUFFER_VALUE, record->framebuffer);
        gl_.BindBuffer(GL_PIXEL_PACK_BUFFER_VALUE, readback.buffer);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        // offscreen 패스는 위쪽 원점으로 그렸으므로 저장소 첫 행이 이미지 위쪽이다(ReadPixels와
        // 같다).
        glReadPixels(0, 0, static_cast<GLsizei>(record->width),
                     static_cast<GLsizei>(record->height), GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl_.BindBuffer(GL_PIXEL_PACK_BUFFER_VALUE, 0);
        gl_.BindFramebuffer(GL_FRAMEBUFFER_VALUE, 0);
        readback.fenceValue = Submit();
        auto added = readbacks_.TryAdd(readback);
        if (!added) {
            gl_.DeleteBuffers(1, &readback.buffer);
            return Result<ReadbackRequest>::Failure(added.Error());
        }
        ReadbackRequest request;
        request.handle = ReadbackHandle{added.Value()};
        request.completion = timeline_.Point(readback.fenceValue);
        request.bytes = bytes;
        return Result<ReadbackRequest>::Success(request);
    }

    Result<bool> TryReadback(ReadbackHandle handle, Span<uint8> out) noexcept override {
        auto owner = ownership_.Check();
        if (!owner) {
            return Result<bool>::Failure(owner.Error());
        }
        const ReadbackRecord* record = readbacks_.Find(handle.slot);
        if (record == nullptr) {
            return Result<bool>::Failure(ErrorCode::StaleHandle);
        }
        if (out.Size() != static_cast<usize>(record->width) * record->height * 4u) {
            return Result<bool>::Failure(ErrorCode::InvalidArgument);
        }
        if (timeline_.Completed() < record->fenceValue) {
            if (timeline_.IsLost()) {
                return Result<bool>::Failure(ErrorCode::Destroyed);
            }
            return Result<bool>::Success(false);
        }
        if (!MakeCurrentForResources()) {
            return FailWith<bool>("wglMakeCurrent failed");
        }
        gl_.GetNamedBufferSubData(record->buffer, 0, static_cast<GLsizeiptr>(out.Size()),
                                  out.Data());
        return Result<bool>::Success(true);
    }

    void ReleaseReadback(ReadbackHandle handle) noexcept override {
        if (!ownership_.Check()) {
            return;
        }
        const ReadbackRecord* record = readbacks_.Find(handle.slot);
        if (record == nullptr) {
            return;
        }
        RetiredObject entry;
        entry.kind = RetiredKind::Buffer;
        entry.first = record->buffer;
        Defer(entry);
        auto removed = readbacks_.TryRemove(handle.slot);
        (void)removed;
    }

    GpuFencePoint LastSubmittedFence() const noexcept override {
        return timeline_.LastSubmitted();
    }

    Result<bool> IsFenceComplete(GpuFencePoint point) const noexcept override {
        return timeline_.IsComplete(point);
    }

    Result<platform::WaitResult> WaitFence(GpuFencePoint point,
                                           uint32 timeoutMs) noexcept override {
        return timeline_.Wait(point, timeoutMs);
    }

    Result<void> NotifyFence(GpuFencePoint point, FenceCallback callback,
                             void* context) noexcept override {
        return timeline_.Notify(point, callback, context);
    }

    bool IsLost() const noexcept override {
        return timeline_.IsLost();
    }

    bool IsOwnerThread() const noexcept override {
        return ownership_.IsOwner();
    }

    Result<void> ReleaseOwnership() noexcept override {
        if (!ownership_.IsOwner()) {
            return Result<void>::Failure(ErrorCode::WrongThread);
        }
        if (recording_) {
            return Result<void>::Failure(ErrorCode::Busy);
        }
        // context는 한 번에 한 스레드에만 붙는다. 새 소유 스레드가 붙일 수 있게 이 스레드에서 뗀다.
        if (wglGetCurrentContext() == context_) {
            wglMakeCurrent(nullptr, nullptr);
        }
        return ownership_.Release();
    }

    Result<void> AcquireOwnership() noexcept override {
        const bool alreadyOwner = ownership_.IsOwner();
        auto acquired = ownership_.Acquire();
        if (!acquired) {
            return acquired;
        }
        if (!alreadyOwner && !MakeCurrent(deviceDc_)) {
            auto released = ownership_.Release();
            (void)released;
            return Fail("wglMakeCurrent failed on the new owner thread");
        }
        return Result<void>::Success();
    }

    uint64 RejectedCalls() const noexcept override {
        return ownership_.Rejected();
    }

    usize PendingRetireCount() const noexcept override {
        return retired_.Size();
    }

    StringView LastError() const noexcept override {
        return lastError_.View();
    }

    uint64 IgnoredDraws() const noexcept override {
        return ignoredDraws_;
    }

    usize CollectTimestamps(Span<TimestampSample> out) noexcept override {
        if (!ownership_.IsOwner() || !timestampsReady_ || !MakeCurrentForResources()) {
            return 0;
        }
        timestamps_.Harvest(timeline_.Completed(), timestampSource_);
        return timestamps_.Collect(out);
    }

    // GL_TIMESTAMP 조회(앞 명령이 서버에 닿은 시각)와 CPU 시각을 잇달아 읽는다. CPU 시각은 조회
    // 앞뒤 값의 가운데로 잡아 조회 지연의 절반만 오차로 남긴다.
    Result<ClockCalibration> CalibrateClocks() noexcept override {
        auto owner = ownership_.Check();
        if (!owner) {
            return Result<ClockCalibration>::Failure(owner.Error());
        }
        if (!timestampsReady_) {
            return Result<ClockCalibration>::Failure(ErrorCode::Unsupported);
        }
        if (timeline_.IsLost()) {
            return Result<ClockCalibration>::Failure(ErrorCode::Destroyed);
        }
        if (!MakeCurrentForResources()) {
            return FailWith<ClockCalibration>("OpenGL context is not current");
        }
        const uint64 before = platform::MonotonicNanoseconds();
        GLint64 gpu = 0;
        gl_.GetInteger64v(GL_TIMESTAMP_VALUE, &gpu);
        const uint64 after = platform::MonotonicNanoseconds();
        ClockCalibration calibration;
        calibration.gpuTicks = static_cast<uint64>(gpu);
        calibration.cpuNanoseconds = before + (after - before) / 2;
        calibration.gpuFrequency = TimestampNanosecondFrequency;
        return Result<ClockCalibration>::Success(calibration);
    }

    uint64 ValidationErrorCount() const noexcept override {
        return validationErrors_;
    }

private:
    static void APIENTRY DebugMessage(GLenum, GLenum type, GLuint, GLenum, GLsizei length,
                                      const GLchar* message, const void* user) {
        auto* self = static_cast<OpenGLDevice*>(const_cast<void*>(user));
        if (type == GL_DEBUG_TYPE_ERROR_VALUE || type == GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_VALUE) {
            ++self->validationErrors_;
            if (message != nullptr && length > 0) {
                auto assigned =
                    self->lastError_.TryAssign(StringView(message, static_cast<usize>(length)));
                (void)assigned;
            }
        }
    }

    Result<void> Fail(const char* message) noexcept {
        auto assigned = lastError_.TryAssign(StringView(message, platform::CStringLength(message)));
        (void)assigned;
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    template <class T>
    Result<T> FailWith(const char* message) noexcept {
        auto failed = Fail(message);
        (void)failed;
        return Result<T>::Failure(ErrorCode::DependencyFailed);
    }

    // 캐시 대신 실제 current 상태를 확인한다. 같은 스레드의 다른 장치·GL 라이브러리가 context를
    // 바꿨을 수 있고, wglMakeCurrent가 실패하면 기존 context가 풀린다.
    bool MakeCurrent(HDC dc) noexcept {
        if (wglGetCurrentContext() == context_ && wglGetCurrentDC() == dc) {
            return true;
        }
        return wglMakeCurrent(dc, context_) != FALSE;
    }

    // 자원 생성·파괴는 어느 DC에 붙어 있든 이 context면 된다. 다른 context가 current이거나 아무것도
    // 없을 때만 장치 창에 붙인다.
    bool MakeCurrentForResources() noexcept {
        if (wglGetCurrentContext() == context_) {
            return true;
        }
        return MakeCurrent(deviceDc_);
    }

    void SetClipOrigin(GLenum origin) noexcept {
        if (clipOrigin_ != origin) {
            gl_.ClipControl(origin, GL_ZERO_TO_ONE_VALUE);
            glFrontFace(FrontFaceFor(origin));
            clipOrigin_ = origin;
        }
    }

    // graphics queue에 fence sync를 넣고 다음 값을 돌려준다. sync를 만들지 못하면 glFinish로 끝내고
    // token 없이 알린다(관찰 스레드가 곧바로 완료로 본다).
    uint64 Submit() noexcept {
        GLsync sync = gl_.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE_VALUE, 0);
        if (sync == nullptr) {
            glFinish();
        } else {
            // 다른 context(관찰 스레드)가 기다리므로 fence 명령을 GPU로 보낸다.
            glFlush();
        }
        const uint64 value = ++fenceValue_;
        timeline_.Submitted(value, sync);
        return value;
    }

    // value의 제출이 끝나면 지울 수 있게 retire 목록에 넣는다. 목록을 늘릴 수 없으면 GPU가 끝날
    // 때까지 기다린 뒤(glFinish) 바로 지운다.
    void Defer(RetiredObject entry) noexcept {
        entry.fenceValue = fenceValue_;
        entry.pending = recording_;
        auto pushed = retired_.TryPushBack(entry);
        if (pushed) {
            return;
        }
        if (recording_ || !MakeCurrentForResources()) {
            // 기록 중인 프레임이 쓸 수 있거나 context가 없다. 지우지 않고 버린다(드라이버가 context
            // 삭제 때 회수한다).
            return;
        }
        glFinish();
        DeleteObject(entry);
    }

    void AssignPendingRetires(uint64 value) noexcept {
        for (RetiredObject& entry : retired_) {
            if (entry.pending) {
                entry.pending = false;
                entry.fenceValue = value;
            }
        }
    }

    // completed까지 끝난 제출만 쓰던 retire 항목을 지운다.
    void DeleteRetired(uint64 completed) noexcept {
        if (retired_.IsEmpty() || !MakeCurrentForResources()) {
            return;
        }
        for (usize index = retired_.Size(); index > 0; --index) {
            const RetiredObject& entry = retired_[index - 1];
            if (entry.pending || entry.fenceValue > completed) {
                continue;
            }
            DeleteObject(entry);
            auto removed = retired_.TryRemoveAtSwap(index - 1);
            (void)removed;
        }
    }

    void DeleteObject(const RetiredObject& entry) noexcept {
        switch (entry.kind) {
        case RetiredKind::Buffer:
            gl_.DeleteBuffers(1, &entry.first);
            break;
        case RetiredKind::Texture:
            if (entry.second != 0) {
                gl_.DeleteFramebuffers(1, &entry.second);
            }
            glDeleteTextures(1, &entry.first);
            break;
        case RetiredKind::Pipeline:
            gl_.DeleteProgram(entry.first);
            gl_.DeleteVertexArrays(1, &entry.second);
            break;
        case RetiredKind::SwapChain:
            ReleaseDC(entry.window, entry.dc);
            break;
        }
    }

    void ReleaseTexture(const TextureRecord& record) noexcept {
        if (record.framebuffer != 0) {
            gl_.DeleteFramebuffers(1, &record.framebuffer);
        }
        glDeleteTextures(1, &record.texture);
    }

    Result<void> Reject(const Diagnostic& message, ErrorCode error) noexcept {
        auto assigned = lastError_.TryAssign(
            StringView(message.CStr(), platform::CStringLength(message.CStr())));
        (void)assigned;
        return Result<void>::Failure(error);
    }

    // 링크한 프로그램의 active attribute를 배치와 대조한다(RenderBase 10.4절 셰이더 입력 대조).
    // gl_로 시작하는 내장 입력은 건너뛴다. 셰이더가 읽지 않는 속성은 허용한다.
    Result<void> MatchVertexInputs(GLuint program, const VertexLayout& layout) noexcept {
        GLint count = 0;
        gl_.GetProgramiv(program, GL_ACTIVE_ATTRIBUTES_VALUE, &count);
        for (GLint index = 0; index < count; ++index) {
            char name[128] = {};
            GLsizei length = 0;
            GLint size = 0;
            GLenum type = 0;
            gl_.GetActiveAttrib(program, static_cast<GLuint>(index), sizeof(name), &length, &size,
                                &type, name);
            if (length >= 3 && name[0] == 'g' && name[1] == 'l' && name[2] == '_') {
                continue;
            }
            const GLint location = gl_.GetAttribLocation(program, name);
            Diagnostic message;
            VertexShaderType shaderType = VertexShaderType::Float;
            if (location < 0 || location >= static_cast<GLint>(MaxVertexAttributes) ||
                !ShaderTypeOf(type, shaderType)) {
                message.Text("vertex shader input ")
                    .Text(name)
                    .Text(" needs layout(location = 0-15) and a float, int or uint type");
                return Reject(message, ErrorCode::InvalidArgument);
            }
            const VertexAttribute* attribute = nullptr;
            for (const VertexAttribute& candidate : layout.attributes) {
                if (candidate.location == static_cast<uint32>(location)) {
                    attribute = &candidate;
                }
            }
            if (attribute == nullptr) {
                message.Text("vertex shader reads location ")
                    .Number(static_cast<uint32>(location))
                    .Text(" which the vertex layout does not provide");
                return Reject(message, ErrorCode::InvalidArgument);
            }
            if (VertexFormatShaderType(attribute->format) != shaderType) {
                message.Text("vertex input location ")
                    .Number(static_cast<uint32>(location))
                    .Text(": format ")
                    .Text(VertexFormatName(attribute->format))
                    .Text(" feeds ")
                    .Text(VertexShaderTypeName(VertexFormatShaderType(attribute->format)))
                    .Text(" but the shader reads ")
                    .Text(VertexShaderTypeName(shaderType));
                return Reject(message, ErrorCode::InvalidArgument);
            }
        }
        return Result<void>::Success();
    }

    // 묶인 정점 버퍼 하나를 지금 파이프라인의 정점 배열에 건다.
    void ApplyVertexBuffer(uint32 stream) noexcept {
        const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_.slot);
        const BufferRecord* buffer = buffers_.Find(boundVertex_[stream].buffer.slot);
        if (pipeline == nullptr || buffer == nullptr) {
            return;
        }
        gl_.VertexArrayVertexBuffer(pipeline->vertexArray, stream, buffer->buffer,
                                    static_cast<GLintptr>(boundVertex_[stream].offset),
                                    static_cast<GLsizei>(pipeline->strides[stream]));
    }

    void ApplyIndexBuffer() noexcept {
        const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_.slot);
        const BufferRecord* buffer = buffers_.Find(boundIndex_.buffer.slot);
        if (pipeline != nullptr) {
            gl_.VertexArrayElementBuffer(pipeline->vertexArray,
                                         buffer != nullptr ? buffer->buffer : 0u);
        }
    }

    // 묶인 파이프라인이 선언한 stream에 모두 유효한 버퍼가 있는지다.
    const PipelineRecord* DrawablePipeline() const noexcept {
        const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_.slot);
        if (pipeline == nullptr) {
            return nullptr;
        }
        for (uint32 stream = 0; stream < MaxVertexStreams; ++stream) {
            if ((pipeline->streamMask & (1u << stream)) == 0) {
                continue;
            }
            const BufferRecord* buffer = buffers_.Find(boundVertex_[stream].buffer.slot);
            if (buffer == nullptr || boundVertex_[stream].offset >= buffer->size) {
                return nullptr;
            }
        }
        return pipeline;
    }

    void StoreLog(GLuint object, bool shader) noexcept {
        GLint length = 0;
        if (shader) {
            gl_.GetShaderiv(object, GL_INFO_LOG_LENGTH_VALUE, &length);
        } else {
            gl_.GetProgramiv(object, GL_INFO_LOG_LENGTH_VALUE, &length);
        }
        char log[2048] = {};
        GLsizei written = 0;
        if (shader) {
            gl_.GetShaderInfoLog(object, static_cast<GLsizei>(sizeof(log)), &written, log);
        } else {
            gl_.GetProgramInfoLog(object, static_cast<GLsizei>(sizeof(log)), &written, log);
        }
        (void)length;
        auto assigned = lastError_.TryAssign(StringView(log, static_cast<usize>(written)));
        (void)assigned;
    }

    Result<GLuint> CompileShader(GLenum stage, StringView source) noexcept {
        const GLuint shader = gl_.CreateShader(stage);
        const GLchar* text = source.Data();
        const GLint length = static_cast<GLint>(source.ByteCount());
        gl_.ShaderSource(shader, 1, &text, &length);
        gl_.CompileShader(shader);
        GLint compiled = GL_FALSE;
        gl_.GetShaderiv(shader, GL_COMPILE_STATUS_VALUE, &compiled);
        if (compiled != GL_TRUE) {
            StoreLog(shader, true);
            gl_.DeleteShader(shader);
            return Result<GLuint>::Failure(ErrorCode::DependencyFailed);
        }
        return Result<GLuint>::Success(shader);
    }

    // timestamp query 묶음을 만든다(GX9). GL_TIMESTAMP counter bit가 0이면 timestamp를 "없음"으로
    // 공개한다(장치 생성은 실패하지 않는다).
    void InitializeTimestamps(bool disjointExtension) noexcept {
        GLint bits = 0;
        gl_.GetQueryiv(GL_TIMESTAMP_VALUE, GL_QUERY_COUNTER_BITS_VALUE, &bits);
        if (bits <= 0 || !timestamps_.Reset(TimestampFrameSlots, MaxTimestampsPerFrame)) {
            return;
        }
        gl_.CreateQueries(GL_TIMESTAMP_VALUE, static_cast<GLsizei>(TimestampQueryCount),
                          timestampQueries_);
        timestampSource_.gl = &gl_;
        timestampSource_.queries = timestampQueries_;
        timestampSource_.disjointDetection = disjointExtension;
        timestampsReady_ = true;
    }

    void StartPass(uint32 width, uint32 height, const ClearColor& clear) noexcept {
        recording_ = true;
        markerDepth_ = 0;
        boundPipeline_ = {};
        for (BoundVertexBuffer& bound : boundVertex_) {
            bound = {};
        }
        boundIndex_ = {};
        constantsDirty_ = true;
        constants_ = DrawConstants{};
        glEnable(GL_FRAMEBUFFER_SRGB_VALUE);
        glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        glDisable(GL_BLEND);
        glClearColor(clear.r, clear.g, clear.b, clear.a);
        glClear(GL_COLOR_BUFFER_BIT);
        const TextureRecord* white = textures_.Find(white_.slot);
        if (white != nullptr) {
            gl_.BindTextureUnit(0, white->texture);
        }
    }

    // --- CommandContext ---

    void SetPipeline(PipelineHandle handle) noexcept override {
        // 레코드 포인터는 프레임 중 파이프라인 생성으로 재배치될 수 있으므로 handle만 기억한다.
        boundPipeline_ = handle;
        const PipelineRecord* pipeline = pipelines_.Find(handle.slot);
        if (pipeline == nullptr) {
            return;
        }
        gl_.UseProgram(pipeline->program);
        gl_.BindVertexArray(pipeline->vertexArray);
        if (pipeline->alphaBlend) {
            glEnable(GL_BLEND);
            gl_.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                                  GL_ONE_MINUS_SRC_ALPHA);
        } else {
            glDisable(GL_BLEND);
        }
        // 앞면 감김(glFrontFace)은 패스 대상의 clip 원점에 맞춰 SetClipOrigin이 정한다.
        if (pipeline->cullMode == CullMode::None) {
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
            glCullFace(pipeline->cullMode == CullMode::Back ? GL_BACK : GL_FRONT);
        }
        constantsDirty_ = true;
        // 앞서 묶은 정점·index 버퍼를 새 파이프라인의 정점 배열·간격으로 다시 묶는다.
        for (uint32 stream = 0; stream < MaxVertexStreams; ++stream) {
            if (!boundVertex_[stream].buffer.IsNull()) {
                ApplyVertexBuffer(stream);
            }
        }
        ApplyIndexBuffer();
    }

    void SetVertexBuffers(uint32 firstStream,
                          Span<const VertexBufferBinding> buffers) noexcept override {
        for (usize index = 0; index < buffers.Size(); ++index) {
            const usize stream = firstStream + index;
            if (stream >= MaxVertexStreams) {
                return;
            }
            boundVertex_[stream] = {buffers[index].buffer, buffers[index].offset};
            ApplyVertexBuffer(static_cast<uint32>(stream));
        }
    }

    void SetIndexBuffer(BufferHandle buffer, IndexFormat format, uint64 offset) noexcept override {
        boundIndex_ = {buffer, format, offset};
        ApplyIndexBuffer();
    }

    void SetTexture(TextureHandle handle) noexcept override {
        const TextureRecord* record = textures_.Find(handle.slot);
        if (record == nullptr) {
            record = textures_.Find(white_.slot);
        }
        if (record != nullptr) {
            gl_.BindTextureUnit(0, record->texture);
        }
    }

    void SetConstants(const DrawConstants& constants) noexcept override {
        constants_ = constants;
        constantsDirty_ = true;
    }

    void ApplyConstants(const PipelineRecord& pipeline) noexcept {
        if (constantsDirty_) {
            // transform은 행 우선 저장이다(DrawConstants). 기본 블록 uniform에는
            // layout(row_major)를 쓸 수 없으므로(GLSL 4.50 4.4.5절: 행렬 배치 한정자는
            // uniform·buffer block 전용) transpose = GL_TRUE로 행 우선으로 담는다. 셰이더는
            // vec4(position, 1) * uTransform이다.
            gl_.ProgramUniformMatrix4fv(pipeline.program, pipeline.transformLocation, 1, GL_TRUE,
                                        constants_.transform);
            gl_.ProgramUniform4fv(pipeline.program, pipeline.colorLocation, 1, constants_.color);
            constantsDirty_ = false;
        }
    }

    Result<void> CopyPassTarget(TextureHandle destination) noexcept override {
        if (!recording_) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        const TextureRecord* record = textures_.Find(destination.slot);
        if (record == nullptr) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }
        if (record->framebuffer == 0 || destination == pass_.texture ||
            record->width != pass_.width || record->height != pass_.height ||
            record->format != pass_.format) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 창(기본 framebuffer)은 저장소 첫 행이 화면 아래쪽이고 RenderTarget은
        // 위쪽이므로(GL_UPPER_LEFT 패스) 창에서 복사할 때만 위아래를 바꾼다. sRGB 변환 없이 byte를
        // 그대로 옮긴다.
        const GLint width = static_cast<GLint>(pass_.width);
        const GLint height = static_cast<GLint>(pass_.height);
        const bool window = pass_.framebuffer == 0;
        glDisable(GL_FRAMEBUFFER_SRGB_VALUE);
        gl_.BlitNamedFramebuffer(pass_.framebuffer, record->framebuffer, 0, 0, width, height, 0,
                                 window ? height : 0, width, window ? 0 : height,
                                 GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glEnable(GL_FRAMEBUFFER_SRGB_VALUE);
        return Result<void>::Success();
    }

    Result<uint64> WriteTimestamp() noexcept override {
        if (!timestampsReady_) {
            return Result<uint64>::Failure(ErrorCode::Unsupported);
        }
        if (!recording_) {
            return Result<uint64>::Failure(ErrorCode::InvalidArgument);
        }
        timestamps_.Harvest(timeline_.Completed(), timestampSource_);
        auto write = timestamps_.Allocate();
        if (!write) {
            return Result<uint64>::Failure(write.Error());
        }
        gl_.QueryCounter(timestampQueries_[write.Value().index], GL_TIMESTAMP_VALUE);
        return Result<uint64>::Success(write.Value().query);
    }

    void PushMarker(StringView name) noexcept override {
        if (!recording_) {
            return;
        }
        const StringView clipped = ClipDebugMarkerName(name);
        gl_.PushDebugGroup(GL_DEBUG_SOURCE_APPLICATION_VALUE, 0,
                           static_cast<GLsizei>(clipped.ByteCount()), clipped.Data());
        ++markerDepth_;
    }

    void PopMarker() noexcept override {
        if (!recording_ || markerDepth_ == 0) {
            ++ignoredDraws_;
            return;
        }
        gl_.PopDebugGroup();
        --markerDepth_;
    }

    void Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept override {
        const PipelineRecord* pipeline = DrawablePipeline();
        if (pipeline == nullptr) {
            ++ignoredDraws_;
            return;
        }
        ApplyConstants(*pipeline);
        // base instance는 0으로 고정한다(RenderBase 10.4절).
        gl_.DrawArraysInstanced(GL_TRIANGLES, static_cast<GLint>(firstVertex),
                                static_cast<GLsizei>(vertexCount),
                                static_cast<GLsizei>(instanceCount));
    }

    void DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex) noexcept override {
        const PipelineRecord* pipeline = DrawablePipeline();
        const BufferRecord* index = buffers_.Find(boundIndex_.buffer.slot);
        if (pipeline == nullptr || index == nullptr || boundIndex_.offset >= index->size) {
            ++ignoredDraws_;
            return;
        }
        ApplyConstants(*pipeline);
        const uint64 start = boundIndex_.offset +
                             static_cast<uint64>(firstIndex) * IndexFormatSize(boundIndex_.format);
        // base vertex·base instance는 0으로 고정한다.
        gl_.DrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(indexCount),
                                  boundIndex_.format == IndexFormat::UInt16 ? GL_UNSIGNED_SHORT
                                                                            : GL_UNSIGNED_INT,
                                  reinterpret_cast<const void*>(static_cast<uptr>(start)),
                                  static_cast<GLsizei>(instanceCount));
    }

    String adapterName_;
    String lastError_;
    SlotArray<BufferRecord> buffers_;
    SlotArray<TextureRecord> textures_;
    SlotArray<PipelineRecord> pipelines_;
    SlotArray<SwapChainRecord> swapChains_;
    Array<RetiredObject> retired_;
    SlotArray<ReadbackRecord> readbacks_;
    OpenGLStagingSource stagingSource_;
    UniquePtr<StagingPool> staging_;
    // GPU 계측(GX9): timestamp query 묶음(slot × 프레임당 한도)과 프레임 slot 장부다. GL_TIMESTAMP
    // 값의 단위는 ns다.
    static constexpr uint32 TimestampQueryCount = TimestampFrameSlots * MaxTimestampsPerFrame;
    static constexpr uint64 TimestampNanosecondFrequency = 1000000000;
    GLuint timestampQueries_[TimestampQueryCount] = {};
    OpenGLTimestampSource timestampSource_;
    TimestampRing timestamps_;
    bool timestampsReady_ = false;
    // 지금 프레임에서 열린 debug group 수다.
    uint32 markerDepth_ = 0;
    GlFunctions gl_;
    WglSwapInterval swapInterval_ = nullptr;
    HWND deviceWindow_ = nullptr;
    HDC deviceDc_ = nullptr;
    HGLRC context_ = nullptr;
    // fence 관찰 스레드의 숨김 창·DC·공유 context다.
    HWND watcherWindow_ = nullptr;
    HDC watcherDc_ = nullptr;
    HGLRC watcherContext_ = nullptr;
    int pixelFormat_ = 0;
    PIXELFORMATDESCRIPTOR pixelDescriptor_ = {};
    GLenum clipOrigin_ = 0;
    // 기록 중인 패스의 대상이다(CopyPassTarget). 창이면 framebuffer 0·texture null이다.
    struct PassTarget {
        GLuint framebuffer = 0;
        uint32 width = 0;
        uint32 height = 0;
        Format format = Format::Unknown;
        TextureHandle texture;
    };
    PassTarget pass_;
    // 이방성 필터 최대 배율이다. 지원하지 않으면 0이다.
    GLfloat maxAnisotropy_ = 0.0f;
    TextureHandle white_;
    SwapChainHandle presentTarget_;
    PipelineHandle boundPipeline_;
    // 기록 중 묶인 정점·index 버퍼다(파이프라인을 바꾸면 그 정점 배열에 다시 묶는다).
    struct BoundVertexBuffer {
        BufferHandle buffer;
        uint64 offset = 0;
    };
    struct BoundIndexBuffer {
        BufferHandle buffer;
        IndexFormat format = IndexFormat::UInt16;
        uint64 offset = 0;
    };
    BoundVertexBuffer boundVertex_[MaxVertexStreams];
    BoundIndexBuffer boundIndex_;
    DrawConstants constants_;
    uint64 ignoredDraws_ = 0;
    uint64 validationErrors_ = 0;
    // 마지막으로 제출한 fence 값이다.
    uint64 fenceValue_ = 0;
    OpenGLFenceSource fenceSource_;
    FenceTimeline timeline_;
    DeviceOwnership ownership_;
    bool recording_ = false;
    bool constantsDirty_ = false;
    bool validation_ = false;
};

} // namespace

Result<UniquePtr<Device>> CreateOpenGLDevice(Allocator& allocator,
                                             const DeviceDesc& desc) noexcept {
    auto created = UniquePtr<OpenGLDevice>::TryMake(allocator, allocator);
    if (!created) {
        return Result<UniquePtr<Device>>::Failure(created.Error());
    }

    auto initialized = created.Value()->Initialize(desc);
    if (!initialized) {
        return Result<UniquePtr<Device>>::Failure(initialized.Error());
    }

    return Result<UniquePtr<Device>>::Success(UniquePtr<Device>(Move(created.Value())));
}

} // namespace rocks::graphics
