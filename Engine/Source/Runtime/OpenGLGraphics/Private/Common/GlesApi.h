#pragma once

#include <Base/Types.h>

// OpenGL ES 3.2 공통 API의 형식·상수·함수 표다(설계: Docs/Design/OpenGLESGraphics.ko.md 3절).
//
// 공통 코드는 플랫폼 GL 헤더(GL/gl.h, GLES3/gl32.h)를 include하지 않는다. 형식과 상수는 Khronos
// 헤더 값을 이 namespace에 직접 두고, 함수는 플랫폼 층(WGL·EGL)이 준 주소 조회 함수로 실행 시
// 얻는다. 그래서 같은 공통 코드가 Windows 미리보기와 Android에서 그대로 컴파일된다. 이 파일을
// include하는 번역 단위는 플랫폼 GL 헤더를 함께 include하면 안 된다(GL_ 매크로와 이름이 겹친다).
//
// 함수 표에는 OpenGL ES 3.2 core 함수만 둔다. 데스크톱 GL 전용 함수(glClipControl, DSA)는 Windows
// 플랫폼 층만 따로 얻고, 확장 함수(timestamp query)는 확장 표(GlesExtensionFunctions)에 둔다.
namespace rocks::graphics::gles {

// GL 진입점의 호출 규약은 Windows에서 APIENTRY(__stdcall)이고 Android에서 기본 규약이다. Rocks는
// 64-bit 대상만 빌드하며 64-bit에서는 __stdcall이 기본 규약과 같으므로 함수 표는 규약을 적지 않는다
// (공통 코드에 플랫폼 분기를 두지 않는다).
static_assert(sizeof(void*) == 8, "The OpenGL ES function table assumes a 64-bit target where "
                                  "APIENTRY is the default calling convention");

using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLint = int;
using GLuint = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLchar = char;
using GLubyte = unsigned char;
using GLint64 = int64;
using GLuint64 = uint64;
using GLsizeiptr = isize;
using GLintptr = isize;
using GLsync = struct __GLsync*;
using GLDEBUGPROC = void (*)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                             const GLchar* message, const void* user);

inline constexpr GLboolean GL_FALSE = 0;
inline constexpr GLboolean GL_TRUE = 1;

// --- 상태·조회 ---
inline constexpr GLenum GL_NO_ERROR = 0;
inline constexpr GLenum GL_INVALID_ENUM = 0x0500;
inline constexpr GLenum GL_INVALID_VALUE = 0x0501;
inline constexpr GLenum GL_INVALID_OPERATION = 0x0502;
inline constexpr GLenum GL_OUT_OF_MEMORY = 0x0505;
inline constexpr GLenum GL_INVALID_FRAMEBUFFER_OPERATION = 0x0506;
inline constexpr GLenum GL_CONTEXT_LOST = 0x0507;
inline constexpr GLenum GL_RENDERER = 0x1F01;
inline constexpr GLenum GL_VERSION = 0x1F02;
inline constexpr GLenum GL_EXTENSIONS = 0x1F03;
inline constexpr GLenum GL_SHADING_LANGUAGE_VERSION = 0x8B8C;
inline constexpr GLenum GL_MAJOR_VERSION = 0x821B;
inline constexpr GLenum GL_MINOR_VERSION = 0x821C;
inline constexpr GLenum GL_NUM_EXTENSIONS = 0x821D;
inline constexpr GLenum GL_MAX_TEXTURE_SIZE = 0x0D33;
inline constexpr GLenum GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS = 0x8B4D;
inline constexpr GLenum GL_MAX_VERTEX_UNIFORM_VECTORS = 0x8DFB;
inline constexpr GLenum GL_MAX_FRAGMENT_UNIFORM_VECTORS = 0x8DFD;
inline constexpr GLenum GL_MAX_VERTEX_ATTRIBS = 0x8869;

// --- 기능 켜기 ---
inline constexpr GLenum GL_CULL_FACE = 0x0B44;
inline constexpr GLenum GL_DEPTH_TEST = 0x0B71;
inline constexpr GLenum GL_DITHER = 0x0BD0;
inline constexpr GLenum GL_BLEND = 0x0BE2;
inline constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
// 데스크톱 GL core와 GL_EXT_sRGB_write_control의 sRGB 쓰기 변환 스위치다(같은 값).
inline constexpr GLenum GL_FRAMEBUFFER_SRGB = 0x8DB9;
inline constexpr GLenum GL_DEBUG_OUTPUT = 0x92E0;
inline constexpr GLenum GL_DEBUG_OUTPUT_SYNCHRONOUS = 0x8242;

// --- 래스터·블렌드 ---
inline constexpr GLenum GL_FRONT = 0x0404;
inline constexpr GLenum GL_BACK = 0x0405;
inline constexpr GLenum GL_CW = 0x0900;
inline constexpr GLenum GL_CCW = 0x0901;
inline constexpr GLenum GL_ZERO = 0;
inline constexpr GLenum GL_ONE = 1;
inline constexpr GLenum GL_SRC_ALPHA = 0x0302;
inline constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
inline constexpr GLenum GL_TRIANGLES = 0x0004;
inline constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x00004000;

// --- 데이터 형식 ---
inline constexpr GLenum GL_BYTE = 0x1400;
inline constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
inline constexpr GLenum GL_SHORT = 0x1402;
inline constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
inline constexpr GLenum GL_INT = 0x1404;
inline constexpr GLenum GL_UNSIGNED_INT = 0x1405;
inline constexpr GLenum GL_FLOAT = 0x1406;
inline constexpr GLenum GL_HALF_FLOAT = 0x140B;
inline constexpr GLenum GL_UNSIGNED_INT_2_10_10_10_REV = 0x8368;
inline constexpr GLenum GL_RGBA = 0x1908;
inline constexpr GLenum GL_RGBA8 = 0x8058;
inline constexpr GLenum GL_SRGB8_ALPHA8 = 0x8C43;

// --- 버퍼 ---
inline constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
inline constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
inline constexpr GLenum GL_PIXEL_PACK_BUFFER = 0x88EB;
inline constexpr GLenum GL_COPY_READ_BUFFER = 0x8F36;
inline constexpr GLenum GL_COPY_WRITE_BUFFER = 0x8F37;
inline constexpr GLenum GL_STATIC_DRAW = 0x88E4;
inline constexpr GLenum GL_STREAM_READ = 0x88E1;
inline constexpr GLbitfield GL_MAP_READ_BIT = 0x0001;

// --- 텍스처·샘플러 ---
inline constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
inline constexpr GLenum GL_TEXTURE0 = 0x84C0;
inline constexpr GLenum GL_NEAREST = 0x2600;
inline constexpr GLenum GL_LINEAR = 0x2601;
inline constexpr GLenum GL_NEAREST_MIPMAP_NEAREST = 0x2700;
inline constexpr GLenum GL_LINEAR_MIPMAP_NEAREST = 0x2701;
inline constexpr GLenum GL_LINEAR_MIPMAP_LINEAR = 0x2703;
inline constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
inline constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
inline constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
inline constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
inline constexpr GLenum GL_REPEAT = 0x2901;
inline constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
inline constexpr GLenum GL_MIRRORED_REPEAT = 0x8370;
// ES 3.2 core(GL_OES_texture_border_clamp를 흡수)다.
inline constexpr GLenum GL_CLAMP_TO_BORDER = 0x812D;
inline constexpr GLenum GL_TEXTURE_BORDER_COLOR = 0x1004;
// GL_EXT_texture_filter_anisotropic이다.
inline constexpr GLenum GL_TEXTURE_MAX_ANISOTROPY = 0x84FE;
inline constexpr GLenum GL_MAX_TEXTURE_MAX_ANISOTROPY = 0x84FF;
inline constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
inline constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05;

// --- framebuffer ---
inline constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
inline constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
inline constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
inline constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
inline constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;

// --- 셰이더·프로그램 ---
inline constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
inline constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
inline constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
inline constexpr GLenum GL_LINK_STATUS = 0x8B82;
inline constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
inline constexpr GLenum GL_ACTIVE_ATTRIBUTES = 0x8B89;
inline constexpr GLenum GL_FLOAT_VEC2 = 0x8B50;
inline constexpr GLenum GL_FLOAT_VEC3 = 0x8B51;
inline constexpr GLenum GL_FLOAT_VEC4 = 0x8B52;
inline constexpr GLenum GL_INT_VEC2 = 0x8B53;
inline constexpr GLenum GL_INT_VEC3 = 0x8B54;
inline constexpr GLenum GL_INT_VEC4 = 0x8B55;
inline constexpr GLenum GL_UNSIGNED_INT_VEC2 = 0x8DC6;
inline constexpr GLenum GL_UNSIGNED_INT_VEC3 = 0x8DC7;
inline constexpr GLenum GL_UNSIGNED_INT_VEC4 = 0x8DC8;

// --- 동기화·query ---
inline constexpr GLenum GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117;
inline constexpr GLenum GL_ALREADY_SIGNALED = 0x911A;
inline constexpr GLenum GL_TIMEOUT_EXPIRED = 0x911B;
inline constexpr GLenum GL_CONDITION_SATISFIED = 0x911C;
inline constexpr GLenum GL_WAIT_FAILED = 0x911D;
inline constexpr GLenum GL_QUERY_RESULT = 0x8866;
inline constexpr GLenum GL_QUERY_RESULT_AVAILABLE = 0x8867;
// GL_EXT_disjoint_timer_query(ES)와 데스크톱 GL 3.3 timer query가 같은 값을 쓴다.
inline constexpr GLenum GL_TIMESTAMP = 0x8E28;
inline constexpr GLenum GL_QUERY_COUNTER_BITS = 0x8864;
inline constexpr GLenum GL_GPU_DISJOINT = 0x8FBB;

// --- debug(KHR_debug, ES 3.2 core) ---
inline constexpr GLenum GL_DEBUG_SOURCE_APPLICATION = 0x824A;
inline constexpr GLenum GL_DEBUG_TYPE_ERROR = 0x824C;
inline constexpr GLenum GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR = 0x824E;

// ES 3.2 core 함수 목록이다. X(반환, 이름(gl 접두사 없이), 인자)로 펼친다. 모두 필수이며 하나라도
// 없으면 장치 생성이 실패한다.
#define ROCKS_GLES_CORE_FUNCTIONS(X)                                                               \
    X(GLenum, GetError, ())                                                                        \
    X(const GLubyte*, GetString, (GLenum))                                                         \
    X(const GLubyte*, GetStringi, (GLenum, GLuint))                                                \
    X(void, GetIntegerv, (GLenum, GLint*))                                                         \
    X(void, GetInteger64v, (GLenum, GLint64*))                                                     \
    X(void, GetFloatv, (GLenum, GLfloat*))                                                         \
    X(void, Enable, (GLenum))                                                                      \
    X(void, Disable, (GLenum))                                                                     \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                                            \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                      \
    X(void, Clear, (GLbitfield))                                                                   \
    X(void, Finish, ())                                                                            \
    X(void, Flush, ())                                                                             \
    X(void, PixelStorei, (GLenum, GLint))                                                          \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))                   \
    X(void, FrontFace, (GLenum))                                                                   \
    X(void, CullFace, (GLenum))                                                                    \
    X(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                                   \
    X(void, GenBuffers, (GLsizei, GLuint*))                                                        \
    X(void, DeleteBuffers, (GLsizei, const GLuint*))                                               \
    X(void, BindBuffer, (GLenum, GLuint))                                                          \
    X(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum))                                 \
    X(void, BufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*))                            \
    X(void*, MapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield))                           \
    X(GLboolean, UnmapBuffer, (GLenum))                                                            \
    X(void, GenVertexArrays, (GLsizei, GLuint*))                                                   \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint*))                                          \
    X(void, BindVertexArray, (GLuint))                                                             \
    X(void, EnableVertexAttribArray, (GLuint))                                                     \
    X(void, VertexAttribFormat, (GLuint, GLint, GLenum, GLboolean, GLuint))                        \
    X(void, VertexAttribIFormat, (GLuint, GLint, GLenum, GLuint))                                  \
    X(void, VertexAttribBinding, (GLuint, GLuint))                                                 \
    X(void, BindVertexBuffer, (GLuint, GLuint, GLintptr, GLsizei))                                 \
    X(void, VertexBindingDivisor, (GLuint, GLuint))                                                \
    X(void, DrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei))                                \
    X(void, DrawElementsInstanced, (GLenum, GLsizei, GLenum, const void*, GLsizei))                \
    X(void, GetActiveAttrib, (GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*))        \
    X(GLint, GetAttribLocation, (GLuint, const GLchar*))                                           \
    X(GLuint, CreateShader, (GLenum))                                                              \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                   \
    X(void, CompileShader, (GLuint))                                                               \
    X(void, GetShaderiv, (GLuint, GLenum, GLint*))                                                 \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                \
    X(void, DeleteShader, (GLuint))                                                                \
    X(GLuint, CreateProgram, ())                                                                   \
    X(void, AttachShader, (GLuint, GLuint))                                                        \
    X(void, LinkProgram, (GLuint))                                                                 \
    X(void, GetProgramiv, (GLuint, GLenum, GLint*))                                                \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                               \
    X(void, DeleteProgram, (GLuint))                                                               \
    X(void, UseProgram, (GLuint))                                                                  \
    X(GLint, GetUniformLocation, (GLuint, const GLchar*))                                          \
    X(void, Uniform4fv, (GLint, GLsizei, const GLfloat*))                                          \
    X(void, GenTextures, (GLsizei, GLuint*))                                                       \
    X(void, DeleteTextures, (GLsizei, const GLuint*))                                              \
    X(void, ActiveTexture, (GLenum))                                                               \
    X(void, BindTexture, (GLenum, GLuint))                                                         \
    X(void, TexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))                             \
    X(void, TexSubImage2D,                                                                         \
      (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*))                \
    X(void, GenSamplers, (GLsizei, GLuint*))                                                       \
    X(void, DeleteSamplers, (GLsizei, const GLuint*))                                              \
    X(void, BindSampler, (GLuint, GLuint))                                                         \
    X(void, SamplerParameteri, (GLuint, GLenum, GLint))                                            \
    X(void, SamplerParameterf, (GLuint, GLenum, GLfloat))                                          \
    X(void, SamplerParameterfv, (GLuint, GLenum, const GLfloat*))                                  \
    X(void, GenFramebuffers, (GLsizei, GLuint*))                                                   \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint*))                                          \
    X(void, BindFramebuffer, (GLenum, GLuint))                                                     \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                         \
    X(GLenum, CheckFramebufferStatus, (GLenum))                                                    \
    X(void, BlitFramebuffer,                                                                       \
      (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))                \
    X(void, ReadBuffer, (GLenum))                                                                  \
    X(GLsync, FenceSync, (GLenum, GLbitfield))                                                     \
    X(GLenum, ClientWaitSync, (GLsync, GLbitfield, GLuint64))                                      \
    X(void, DeleteSync, (GLsync))                                                                  \
    X(void, GenQueries, (GLsizei, GLuint*))                                                        \
    X(void, DeleteQueries, (GLsizei, const GLuint*))                                               \
    X(void, GetQueryiv, (GLenum, GLenum, GLint*))                                                  \
    X(void, GetQueryObjectuiv, (GLuint, GLenum, GLuint*))                                          \
    X(void, DebugMessageCallback, (GLDEBUGPROC, const void*))                                      \
    X(void, PushDebugGroup, (GLenum, GLuint, GLsizei, const GLchar*))                              \
    X(void, PopDebugGroup, ())

// ES 3.2 core 함수 표다. 실행 시 플랫폼 층의 주소 조회 함수로 채운다(LoadCoreFunctions).
struct GlesFunctions {
#define ROCKS_GLES_DECLARE_FUNCTION(ReturnType, Name, Parameters)                                  \
    ReturnType(*Name) Parameters = nullptr;
    ROCKS_GLES_CORE_FUNCTIONS(ROCKS_GLES_DECLARE_FUNCTION)
#undef ROCKS_GLES_DECLARE_FUNCTION
};

// timestamp query 함수다. ES는 GL_EXT_disjoint_timer_query(EXT 접미사), 데스크톱 GL core context는
// GL 3.3 core 이름으로 얻는다(GlesTimerQueryNames). 둘 다 없으면 null이고 timestamp는 "없음"이다.
struct GlesTimerQueryFunctions {
    void (*QueryCounter)(GLuint, GLenum) = nullptr;
    void (*GetQueryObjectui64v)(GLuint, GLenum, GLuint64*) = nullptr;
    // GL_TIMESTAMP의 counter bit 조회다. ES 3.2 core glGetQueryiv는 GL_TIMESTAMP 대상을 정의하지
    // 않으므로 ES는 확장의 glGetQueryivEXT를 쓴다.
    void (*GetQueryiv)(GLenum, GLenum, GLint*) = nullptr;
};

// 이름으로 GL 함수 주소를 얻는 플랫폼 함수다(wglGetProcAddress·eglGetProcAddress를 감싼다). 없으면
// nullptr이다.
using GlesProcLoader = void* (*)(const char* name, void* context) noexcept;

// core 함수 표를 채운다. 모두 얻으면 true다. 하나라도 없으면 false이고 *missing에 그 함수 이름
// ("glTexStorage2D" 등, 정적 문자열)을 둔다.
bool LoadCoreFunctions(GlesFunctions& gl, GlesProcLoader loader, void* context,
                       const char** missing) noexcept;

// timestamp query 함수 이름 쌍이다.
struct GlesTimerQueryNames {
    const char* queryCounter = nullptr;
    const char* getQueryObjectui64v = nullptr;
    const char* getQueryiv = nullptr;
};

// GL_EXT_disjoint_timer_query(ES)의 이름이다.
inline constexpr GlesTimerQueryNames EsTimerQueryNames = {
    "glQueryCounterEXT", "glGetQueryObjectui64vEXT", "glGetQueryivEXT"};

// 데스크톱 GL 3.3 core(ARB_timer_query)의 이름이다. Windows 미리보기의 데스크톱 core context만
// 쓴다.
inline constexpr GlesTimerQueryNames DesktopTimerQueryNames = {
    "glQueryCounter", "glGetQueryObjectui64v", "glGetQueryiv"};

// timestamp query 함수를 names로 얻는다. 모두 얻으면 true다.
bool LoadTimerQueryFunctions(GlesTimerQueryFunctions& out, const GlesTimerQueryNames& names,
                             GlesProcLoader loader, void* context) noexcept;

// 지금 context가 name 확장(GL_EXT_... 온전한 이름)을 공개하는지다(glGetStringi로 찾는다).
bool HasGlExtension(const GlesFunctions& gl, const char* name) noexcept;

// 공백으로 나뉜 확장 목록 문자열(WGL·EGL 확장 문자열)에 name이 온전한 토큰으로 있는지다. list가
// nullptr이면 false다.
bool HasExtensionToken(const char* list, const char* name) noexcept;

// GL 오류 코드의 이름이다("GL_INVALID_ENUM" 등). 모르는 값은 "GL error"다.
const char* GlErrorName(GLenum error) noexcept;

} // namespace rocks::graphics::gles
