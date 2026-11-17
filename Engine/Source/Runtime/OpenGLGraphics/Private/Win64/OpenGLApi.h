#pragma once

#include <Base/Types.h>
#include <Platform/Memory.h>

#include <Windows.h>
#include <GL/gl.h>

// OpenGL 장치가 쓰는 GL 형식·상수와 실행 시 얻는 함수 표다(OpenGLDevice.cpp에서 나눴다).
namespace rocks::graphics::opengl {

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
inline bool HasExtension(const char* list, const char* name) noexcept {
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
inline void* LoadGl(const char* name) noexcept {
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

inline bool LoadFunctions(GlFunctions& gl) noexcept {
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

} // namespace rocks::graphics::opengl
