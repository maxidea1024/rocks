#include "SpikeTools.h"

#include <Platform/Memory.h>
#include <stdio.h>
#include <string.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>
#include <GL/gl.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

// 측정 S12(데스크톱, Docs/Design/ShaderSystem.ko.md 5.4절·P11): 이 기계의 GL 드라이버가
// SPIRV-Cross가 만든 `#version 320 es` 원문(GLES31_PC)을 받는지 본다. 숨김 창에 GL core context를
// 만들고 GL_ARB_ES3_2_compatibility·GL_ARB_clip_control 유무와 실제 컴파일·링크 결과를 남긴다.
// ES 3.2 context(WGL_EXT_create_context_es2_profile)가 되면 거기서도 같은 원문을 시험한다. 기존
// OpenGLGraphics(Win64/OpenGLDevice.cpp)의 context 생성 순서를 따르되 그 코드는 쓰지 않는다.
namespace rocks::spike {

namespace {

// GL 1.1 헤더(GL/gl.h)에 없는 상수다(Khronos glcorearb.h·wglext.h 값). gl.h 매크로와 겹치지 않게
// _VALUE를 붙인다.
constexpr GLenum GL_FRAGMENT_SHADER_VALUE = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER_VALUE = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS_VALUE = 0x8B81;
constexpr GLenum GL_LINK_STATUS_VALUE = 0x8B82;
constexpr GLenum GL_SHADING_LANGUAGE_VERSION_VALUE = 0x8B8C;
constexpr GLenum GL_NUM_EXTENSIONS_VALUE = 0x821D;
constexpr GLenum GL_NUM_SHADING_LANGUAGE_VERSIONS_VALUE = 0x82E9;
constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x0001;
constexpr int WGL_CONTEXT_ES2_PROFILE_BIT_EXT = 0x0004;

// 셰이더·프로그램 info log 버퍼 크기다(넘는 부분은 잘린다).
constexpr GLsizei InfoLogBytes = 2048;

constexpr wchar_t HiddenClassName[] = L"RocksShaderToolSpikeGl";

using WglCreateContextAttribs = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using WglGetExtensionsString = const char*(WINAPI*)(HDC);
using GlGetStringi = const GLubyte*(APIENTRY*)(GLenum, GLuint);
using GlCreateShader = GLuint(APIENTRY*)(GLenum);
using GlShaderSource = void(APIENTRY*)(GLuint, GLsizei, const char* const*, const GLint*);
using GlCompileShader = void(APIENTRY*)(GLuint);
using GlGetShaderiv = void(APIENTRY*)(GLuint, GLenum, GLint*);
using GlGetShaderInfoLog = void(APIENTRY*)(GLuint, GLsizei, GLsizei*, char*);
using GlCreateProgram = GLuint(APIENTRY*)();
using GlAttachShader = void(APIENTRY*)(GLuint, GLuint);
using GlLinkProgram = void(APIENTRY*)(GLuint);
using GlGetProgramiv = void(APIENTRY*)(GLuint, GLenum, GLint*);
using GlGetProgramInfoLog = void(APIENTRY*)(GLuint, GLsizei, GLsizei*, char*);
using GlDeleteShader = void(APIENTRY*)(GLuint);
using GlDeleteProgram = void(APIENTRY*)(GLuint);

// 현재 context에서 얻은 GL 2.0+ 함수다(주소는 context·픽셀 형식마다 다를 수 있어 context마다
// 얻는다).
struct GlFunctions {
    GlGetStringi getStringi = nullptr;
    GlCreateShader createShader = nullptr;
    GlShaderSource shaderSource = nullptr;
    GlCompileShader compileShader = nullptr;
    GlGetShaderiv getShaderiv = nullptr;
    GlGetShaderInfoLog getShaderInfoLog = nullptr;
    GlCreateProgram createProgram = nullptr;
    GlAttachShader attachShader = nullptr;
    GlLinkProgram linkProgram = nullptr;
    GlGetProgramiv getProgramiv = nullptr;
    GlGetProgramInfoLog getProgramInfoLog = nullptr;
    GlDeleteShader deleteShader = nullptr;
    GlDeleteProgram deleteProgram = nullptr;
};

template <typename Function>
Function Load(const char* name) noexcept {
    return reinterpret_cast<Function>(reinterpret_cast<void*>(wglGetProcAddress(name)));
}

bool LoadFunctions(GlFunctions& gl) noexcept {
    gl.getStringi = Load<GlGetStringi>("glGetStringi");
    gl.createShader = Load<GlCreateShader>("glCreateShader");
    gl.shaderSource = Load<GlShaderSource>("glShaderSource");
    gl.compileShader = Load<GlCompileShader>("glCompileShader");
    gl.getShaderiv = Load<GlGetShaderiv>("glGetShaderiv");
    gl.getShaderInfoLog = Load<GlGetShaderInfoLog>("glGetShaderInfoLog");
    gl.createProgram = Load<GlCreateProgram>("glCreateProgram");
    gl.attachShader = Load<GlAttachShader>("glAttachShader");
    gl.linkProgram = Load<GlLinkProgram>("glLinkProgram");
    gl.getProgramiv = Load<GlGetProgramiv>("glGetProgramiv");
    gl.getProgramInfoLog = Load<GlGetProgramInfoLog>("glGetProgramInfoLog");
    gl.deleteShader = Load<GlDeleteShader>("glDeleteShader");
    gl.deleteProgram = Load<GlDeleteProgram>("glDeleteProgram");
    return gl.getStringi != nullptr && gl.createShader != nullptr && gl.shaderSource != nullptr &&
           gl.compileShader != nullptr && gl.getShaderiv != nullptr &&
           gl.getShaderInfoLog != nullptr && gl.createProgram != nullptr &&
           gl.attachShader != nullptr && gl.linkProgram != nullptr && gl.getProgramiv != nullptr &&
           gl.getProgramInfoLog != nullptr && gl.deleteShader != nullptr &&
           gl.deleteProgram != nullptr;
}

const char* GlString(GLenum name) noexcept {
    const auto* value = reinterpret_cast<const char*>(glGetString(name));
    return value != nullptr ? value : "";
}

bool HasExtension(const GlFunctions& gl, const char* wanted) noexcept {
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS_VALUE, &count);
    for (GLint index = 0; index < count; ++index) {
        const auto* name =
            reinterpret_cast<const char*>(gl.getStringi(GL_EXTENSIONS, static_cast<GLuint>(index)));
        if (name != nullptr && platform::CStringEquals(name, wanted)) {
            return true;
        }
    }

    return false;
}

// 컴파일 결과와 info log를 report에 쓴다. 성공하면 셰이더 이름, 실패하면 0이다.
GLuint CompileShader(const GlFunctions& gl, GLenum stage, StringView text, StringView key,
                     Report& report) noexcept {
    const GLuint shader = gl.createShader(stage);
    const char* sources[] = {text.Data()};
    const GLint lengths[] = {static_cast<GLint>(text.ByteCount())};
    gl.shaderSource(shader, 1, sources, lengths);
    gl.compileShader(shader);
    GLint compiled = GL_FALSE;
    gl.getShaderiv(shader, GL_COMPILE_STATUS_VALUE, &compiled);
    char log[InfoLogBytes] = {};
    GLsizei logBytes = 0;
    gl.getShaderInfoLog(shader, InfoLogBytes, &logBytes, log);
    report.BeginObject(key);
    report.Bool("compiled", compiled == GL_TRUE);
    report.String("log", StringView(log, static_cast<usize>(logBytes)));
    report.EndObject();
    if (compiled != GL_TRUE) {
        gl.deleteShader(shader);
        return 0;
    }

    return shader;
}

// 현재 context에서 GLSL 버전 목록·확장·ESSL 컴파일·링크를 시험한다.
void ProbeCurrentContext(StringView vertexEssl, StringView pixelEssl, Report& report) noexcept {
    GlFunctions gl;
    if (!LoadFunctions(gl)) {
        report.String("error", StringView("GL 2.0+ functions are missing"));
        return;
    }

    report.String("version", StringView(GlString(GL_VERSION)));
    report.String("renderer", StringView(GlString(GL_RENDERER)));
    report.String("vendor", StringView(GlString(GL_VENDOR)));
    report.String("shadingLanguage", StringView(GlString(GL_SHADING_LANGUAGE_VERSION_VALUE)));
    report.Bool("GL_ARB_ES3_2_compatibility", HasExtension(gl, "GL_ARB_ES3_2_compatibility"));
    report.Bool("GL_ARB_ES3_1_compatibility", HasExtension(gl, "GL_ARB_ES3_1_compatibility"));
    report.Bool("GL_ARB_clip_control", HasExtension(gl, "GL_ARB_clip_control"));

    // GL 4.3+는 받는 #version 줄 목록을 알려 준다(빈 문자열은 #version 없는 110이다).
    GLint versions = 0;
    glGetIntegerv(GL_NUM_SHADING_LANGUAGE_VERSIONS_VALUE, &versions);
    report.BeginArray("glslVersions");
    for (GLint index = 0; index < versions; ++index) {
        const auto* name = reinterpret_cast<const char*>(
            gl.getStringi(GL_SHADING_LANGUAGE_VERSION_VALUE, static_cast<GLuint>(index)));
        report.Element(StringView(name != nullptr ? name : ""));
    }

    report.EndArray();
    (void)glGetError();

    const GLuint vertex = CompileShader(gl, GL_VERTEX_SHADER_VALUE, vertexEssl, "vertex", report);
    const GLuint pixel = CompileShader(gl, GL_FRAGMENT_SHADER_VALUE, pixelEssl, "pixel", report);
    GLint linked = GL_FALSE;
    char log[InfoLogBytes] = {};
    GLsizei logBytes = 0;
    if (vertex != 0 && pixel != 0) {
        const GLuint program = gl.createProgram();
        gl.attachShader(program, vertex);
        gl.attachShader(program, pixel);
        gl.linkProgram(program);
        gl.getProgramiv(program, GL_LINK_STATUS_VALUE, &linked);
        gl.getProgramInfoLog(program, InfoLogBytes, &logBytes, log);
        gl.deleteProgram(program);
    }

    report.Bool("linked", linked == GL_TRUE);
    report.String("linkLog", StringView(log, static_cast<usize>(logBytes)));
    if (vertex != 0) {
        gl.deleteShader(vertex);
    }

    if (pixel != 0) {
        gl.deleteShader(pixel);
    }
}

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

    // WS_VISIBLE이 없으므로 화면에 나타나지 않는다.
    return CreateWindowExW(0, HiddenClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr,
                           nullptr, GetModuleHandleW(nullptr), nullptr);
}

// attributes로 context를 만들어 현재로 두고 시험한 뒤 지운다. 만들지 못하면 false다.
bool ProbeContext(HDC dc, WglCreateContextAttribs create, const int* attributes,
                  StringView vertexEssl, StringView pixelEssl, Report& report) noexcept {
    HGLRC context = create(dc, nullptr, attributes);
    if (context == nullptr || !wglMakeCurrent(dc, context)) {
        if (context != nullptr) {
            wglDeleteContext(context);
        }

        return false;
    }

    ProbeCurrentContext(vertexEssl, pixelEssl, report);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(context);
    return true;
}

} // namespace

void ProbeOpenGl(StringView vertexEssl, StringView pixelEssl, Report& report) noexcept {
    report.BeginObject("opengl");
    HWND window = CreateHiddenWindow();
    if (window == nullptr) {
        report.String("error", StringView("hidden window creation failed"));
        report.EndObject();
        return;
    }

    HDC dc = GetDC(window);
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

    WglCreateContextAttribs create = nullptr;
    const char* wglExtensions = "";
    if (legacy != nullptr && wglMakeCurrent(dc, legacy)) {
        create = Load<WglCreateContextAttribs>("wglCreateContextAttribsARB");
        const auto extensions = Load<WglGetExtensionsString>("wglGetExtensionsStringARB");
        if (extensions != nullptr && extensions(dc) != nullptr) {
            wglExtensions = extensions(dc);
        }
    }

    const bool esProfile = strstr(wglExtensions, "WGL_EXT_create_context_es2_profile") != nullptr;
    report.Bool("WGL_EXT_create_context_es2_profile", esProfile);
    wglMakeCurrent(nullptr, nullptr);
    if (legacy != nullptr) {
        wglDeleteContext(legacy);
    }

    if (create == nullptr) {
        report.String("error", StringView("WGL_ARB_create_context is not available"));
    } else {
        // 가장 높은 core 버전부터 만든다(4.6 → 4.5 → 4.3, P11 Windows 미리보기 요구는 4.3 core
        // 이상).
        const int coreVersions[][2] = {{4, 6}, {4, 5}, {4, 3}};
        report.BeginObject("core");
        bool created = false;
        for (const auto& version : coreVersions) {
            const int attributes[] = {WGL_CONTEXT_MAJOR_VERSION_ARB,
                                      version[0],
                                      WGL_CONTEXT_MINOR_VERSION_ARB,
                                      version[1],
                                      WGL_CONTEXT_PROFILE_MASK_ARB,
                                      WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                                      0};
            if (ProbeContext(dc, create, attributes, vertexEssl, pixelEssl, report)) {
                created = true;
                break;
            }
        }

        report.Bool("created", created);
        report.EndObject();
        if (esProfile) {
            const int attributes[] = {WGL_CONTEXT_MAJOR_VERSION_ARB,
                                      3,
                                      WGL_CONTEXT_MINOR_VERSION_ARB,
                                      2,
                                      WGL_CONTEXT_PROFILE_MASK_ARB,
                                      WGL_CONTEXT_ES2_PROFILE_BIT_EXT,
                                      0};
            report.BeginObject("es32");
            report.Bool("created",
                        ProbeContext(dc, create, attributes, vertexEssl, pixelEssl, report));
            report.EndObject();
        }
    }

    ReleaseDC(window, dc);
    DestroyWindow(window);
    report.EndObject();
}

} // namespace rocks::spike
