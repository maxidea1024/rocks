#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <OpenGLGraphics/OpenGLDevice.h>

#include <Windows.h>
#include <GL/gl.h>

namespace rocks::graphics {

namespace {

// --- GL 4.5 core에서 쓰는 형식·상수(Windows SDK의 gl.h는 1.1까지만 선언한다) ---

using GLchar = char;
using GLsizeiptr = signed long long;
using GLintptr = signed long long;
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
constexpr GLenum GL_ZERO_TO_ONE_VALUE = 0x935F;
constexpr GLenum GL_DEBUG_OUTPUT_VALUE = 0x92E0;
constexpr GLenum GL_DEBUG_OUTPUT_SYNCHRONOUS_VALUE = 0x8242;
constexpr GLenum GL_DEBUG_TYPE_ERROR_VALUE = 0x824C;
constexpr GLenum GL_MAJOR_VERSION_VALUE = 0x821B;
constexpr GLenum GL_MINOR_VERSION_VALUE = 0x821C;

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
    void(APIENTRY* BindTextureUnit)(GLuint, GLuint) = nullptr;
    void(APIENTRY* CreateFramebuffers)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* NamedFramebufferTexture)(GLuint, GLenum, GLuint, GLint) = nullptr;
    GLenum(APIENTRY* CheckNamedFramebufferStatus)(GLuint, GLenum) = nullptr;
    void(APIENTRY* BindFramebuffer)(GLenum, GLuint) = nullptr;
    void(APIENTRY* BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum) = nullptr;
    void(APIENTRY* ClipControl)(GLenum, GLenum) = nullptr;
    void(APIENTRY* DebugMessageCallback)(GLDEBUGPROC, const void*) = nullptr;
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
           Bind(gl.BindTextureUnit, "glBindTextureUnit") &&
           Bind(gl.CreateFramebuffers, "glCreateFramebuffers") &&
           Bind(gl.DeleteFramebuffers, "glDeleteFramebuffers") &&
           Bind(gl.NamedFramebufferTexture, "glNamedFramebufferTexture") &&
           Bind(gl.CheckNamedFramebufferStatus, "glCheckNamedFramebufferStatus") &&
           Bind(gl.BindFramebuffer, "glBindFramebuffer") &&
           Bind(gl.BlendFuncSeparate, "glBlendFuncSeparate") &&
           Bind(gl.ClipControl, "glClipControl") &&
           Bind(gl.DebugMessageCallback, "glDebugMessageCallback");
}

GLint ComponentCount(VertexFormat format) noexcept {
    switch (format) {
    case VertexFormat::Float2:
        return 2;
    case VertexFormat::Float3:
        return 3;
    case VertexFormat::Float4:
        return 4;
    }
    return 0;
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
};

struct TextureRecord {
    GLuint texture = 0;
    GLuint framebuffer = 0;
    uint32 width = 0;
    uint32 height = 0;
    Format format = Format::Unknown;
};

struct PipelineRecord {
    GLuint program = 0;
    GLuint vertexArray = 0;
    GLint transformLocation = -1;
    GLint colorLocation = -1;
    uint32 stride = 0;
    bool alphaBlend = false;
};

struct SwapChainRecord {
    HWND window = nullptr;
    HDC dc = nullptr;
    uint32 width = 0;
    uint32 height = 0;
    bool vsync = true;
};

class OpenGLDevice final : public Device, private CommandContext {
public:
    explicit OpenGLDevice(Allocator& allocator) noexcept
        : adapterName_(allocator), lastError_(allocator), buffers_(allocator), textures_(allocator),
          pipelines_(allocator), swapChains_(allocator) {
    }

    ~OpenGLDevice() override {
        if (context_ != nullptr) {
            MakeCurrent(deviceDc_);
            glFinish();
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
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(context_);
        }
        for (const SwapChainRecord& record : swapChains_) {
            ReleaseDC(record.window, record.dc);
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
                swapInterval_ = reinterpret_cast<WglSwapInterval>(LoadGl("wglSwapIntervalEXT"));
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
        const int attributes[] = {WGL_DRAW_TO_WINDOW_ARB,
                                  TRUE,
                                  WGL_SUPPORT_OPENGL_ARB,
                                  TRUE,
                                  WGL_DOUBLE_BUFFER_ARB,
                                  TRUE,
                                  WGL_ACCELERATION_ARB,
                                  WGL_FULL_ACCELERATION_ARB,
                                  WGL_PIXEL_TYPE_ARB,
                                  WGL_TYPE_RGBA_ARB,
                                  WGL_COLOR_BITS_ARB,
                                  32,
                                  WGL_ALPHA_BITS_ARB,
                                  8,
                                  WGL_DEPTH_BITS_ARB,
                                  0,
                                  WGL_STENCIL_BITS_ARB,
                                  0,
                                  WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB,
                                  TRUE,
                                  0};
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

        GLint major = 0;
        GLint minor = 0;
        glGetIntegerv(GL_MAJOR_VERSION_VALUE, &major);
        glGetIntegerv(GL_MINOR_VERSION_VALUE, &minor);
        if (major < 4 || (major == 4 && minor < 5)) {
            return Fail("OpenGL 4.5 is required");
        }
        const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        if (renderer != nullptr) {
            auto assigned =
                adapterName_.TryAssign(StringView(renderer, __builtin_strlen(renderer)));
            (void)assigned;
        }

        if (desc.enableValidation) {
            glEnable(GL_DEBUG_OUTPUT_VALUE);
            glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_VALUE);
            gl_.DebugMessageCallback(&OpenGLDevice::DebugMessage, this);
            validation_ = true;
        }
        gl_.ClipControl(GL_LOWER_LEFT_VALUE, GL_ZERO_TO_ONE_VALUE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);

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
        info.validationEnabled = validation_;
        return info;
    }

    Result<SwapChainHandle> CreateSwapChain(const SwapChainDesc& desc) noexcept override {
        if (desc.surface.window == nullptr || desc.width == 0 || desc.height == 0) {
            return Result<SwapChainHandle>::Failure(ErrorCode::InvalidArgument);
        }

        SwapChainRecord record;
        record.window = static_cast<HWND>(desc.surface.window);
        record.dc = GetDC(record.window);
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
        SwapChainRecord* record = swapChains_.Find(handle.slot);
        if (record == nullptr) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }
        if (width == 0 || height == 0 || recording_) {
            return Result<void>::Failure(recording_ ? ErrorCode::Busy : ErrorCode::InvalidArgument);
        }
        // 기본 framebuffer는 창 크기를 따른다. 뷰포트 크기만 갱신한다.
        record->width = width;
        record->height = height;
        return Result<void>::Success();
    }

    Result<BufferHandle> CreateBuffer(const BufferDesc& desc,
                                      Span<const uint8> initial) noexcept override {
        if (desc.size == 0 || initial.Size() != desc.size) {
            return Result<BufferHandle>::Failure(ErrorCode::InvalidArgument);
        }
        MakeCurrentForResources();
        BufferRecord record;
        gl_.CreateBuffers(1, &record.buffer);
        gl_.NamedBufferStorage(record.buffer, static_cast<GLsizeiptr>(desc.size), initial.Data(),
                               0);
        auto added = buffers_.TryAdd(record);
        if (!added) {
            gl_.DeleteBuffers(1, &record.buffer);
            return Result<BufferHandle>::Failure(added.Error());
        }
        return Result<BufferHandle>::Success(BufferHandle{added.Value()});
    }

    Result<TextureHandle> CreateTexture(const TextureDesc& desc,
                                        Span<const uint8> pixels) noexcept override {
        const uint32 bytesPerPixel = BytesPerPixel(desc.format);
        const bool renderTarget = desc.usage == TextureUsage::RenderTarget;
        const usize size = static_cast<usize>(desc.width) * desc.height * bytesPerPixel;
        if (desc.width == 0 || desc.height == 0 || desc.width > 16384 || desc.height > 16384 ||
            bytesPerPixel == 0 || (renderTarget ? pixels.Size() != 0 : pixels.Size() != size)) {
            return Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
        }

        MakeCurrentForResources();
        TextureRecord record;
        record.width = desc.width;
        record.height = desc.height;
        record.format = desc.format;
        gl_.CreateTextures(GL_TEXTURE_2D, 1, &record.texture);
        gl_.TextureStorage2D(record.texture, 1, InternalFormat(desc.format),
                             static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height));
        gl_.TextureParameteri(record.texture, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl_.TextureParameteri(record.texture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl_.TextureParameteri(record.texture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_VALUE);
        gl_.TextureParameteri(record.texture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_VALUE);
        if (!renderTarget) {
            // 첫 행을 v = 0에 둔다. UV (0,0)이 이미지 좌상단이 되어 D3D12와 결과가 같다.
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            gl_.TextureSubImage2D(record.texture, 0, 0, 0, static_cast<GLsizei>(desc.width),
                                  static_cast<GLsizei>(desc.height), GL_RGBA, GL_UNSIGNED_BYTE,
                                  pixels.Data());
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
        if (desc.vertex.glsl.ByteCount() == 0 || desc.pixel.glsl.ByteCount() == 0 ||
            desc.vertexStride == 0 || desc.attributes.Size() == 0 || desc.attributes.Size() > 8 ||
            InternalFormat(desc.colorFormat) == 0) {
            return Result<PipelineHandle>::Failure(ErrorCode::InvalidArgument);
        }

        MakeCurrentForResources();
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
        record.transformLocation = gl_.GetUniformLocation(record.program, "uTransform");
        record.colorLocation = gl_.GetUniformLocation(record.program, "uColor");
        record.stride = desc.vertexStride;
        record.alphaBlend = desc.alphaBlend;

        gl_.CreateVertexArrays(1, &record.vertexArray);
        for (const VertexAttribute& attribute : desc.attributes) {
            gl_.EnableVertexArrayAttrib(record.vertexArray, attribute.location);
            gl_.VertexArrayAttribFormat(record.vertexArray, attribute.location,
                                        ComponentCount(attribute.format), GL_FLOAT, GL_FALSE,
                                        attribute.offset);
            gl_.VertexArrayAttribBinding(record.vertexArray, attribute.location, 0);
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
        SwapChainRecord* record = swapChains_.Find(handle.slot);
        if (record == nullptr) {
            return Result<CommandContext*>::Failure(ErrorCode::StaleHandle);
        }
        if (recording_) {
            return Result<CommandContext*>::Failure(ErrorCode::Busy);
        }
        if (!MakeCurrent(record->dc)) {
            return FailWith<CommandContext*>("wglMakeCurrent failed on the swap chain window");
        }
        if (swapInterval_ != nullptr && appliedInterval_ != (record->vsync ? 1 : 0)) {
            appliedInterval_ = record->vsync ? 1 : 0;
            swapInterval_(appliedInterval_);
        }
        gl_.BindFramebuffer(GL_FRAMEBUFFER_VALUE, 0);
        presentTarget_ = handle;
        StartPass(record->width, record->height, clear);
        return Result<CommandContext*>::Success(this);
    }

    Result<CommandContext*> BeginOffscreen(TextureHandle handle,
                                           const ClearColor& clear) noexcept override {
        TextureRecord* record = textures_.Find(handle.slot);
        if (record == nullptr || record->framebuffer == 0) {
            return Result<CommandContext*>::Failure(record == nullptr ? ErrorCode::StaleHandle
                                                                      : ErrorCode::InvalidArgument);
        }
        if (recording_) {
            return Result<CommandContext*>::Failure(ErrorCode::Busy);
        }
        MakeCurrentForResources();
        gl_.BindFramebuffer(GL_FRAMEBUFFER_VALUE, record->framebuffer);
        presentTarget_ = {};
        StartPass(record->width, record->height, clear);
        return Result<CommandContext*>::Success(this);
    }

    Result<void> EndFrame() noexcept override {
        if (!recording_) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        recording_ = false;
        gl_.BindVertexArray(0);
        gl_.UseProgram(0);
        if (!presentTarget_.IsNull()) {
            const SwapChainRecord* record = swapChains_.Find(presentTarget_.slot);
            if (record != nullptr && !SwapBuffers(record->dc)) {
                return Fail("SwapBuffers failed");
            }
        }
        gl_.BindFramebuffer(GL_FRAMEBUFFER_VALUE, 0);
        return Result<void>::Success();
    }

    Result<void> ReadPixels(TextureHandle handle, Span<uint8> out) noexcept override {
        TextureRecord* record = textures_.Find(handle.slot);
        if (record == nullptr) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }
        const usize rowBytes = static_cast<usize>(record->width) * BytesPerPixel(record->format);
        if (record->framebuffer == 0 || out.Size() != rowBytes * record->height || recording_) {
            return Result<void>::Failure(recording_ ? ErrorCode::Busy : ErrorCode::InvalidArgument);
        }
        MakeCurrentForResources();
        gl_.BindFramebuffer(GL_FRAMEBUFFER_VALUE, record->framebuffer);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, static_cast<GLsizei>(record->width),
                     static_cast<GLsizei>(record->height), GL_RGBA, GL_UNSIGNED_BYTE, out.Data());
        gl_.BindFramebuffer(GL_FRAMEBUFFER_VALUE, 0);
        // GL은 아래 행부터 준다. D3D12와 같은 위→아래 순서로 제자리에서 뒤집는다.
        for (uint32 row = 0; row < record->height / 2; ++row) {
            uint8* top = out.Data() + static_cast<usize>(row) * rowBytes;
            uint8* bottom = out.Data() + static_cast<usize>(record->height - 1 - row) * rowBytes;
            for (usize index = 0; index < rowBytes; ++index) {
                const uint8 swap = top[index];
                top[index] = bottom[index];
                bottom[index] = swap;
            }
        }
        return Result<void>::Success();
    }

    void Destroy(BufferHandle handle) noexcept override {
        const BufferRecord* record = buffers_.Find(handle.slot);
        if (record == nullptr) {
            return;
        }
        MakeCurrentForResources();
        gl_.DeleteBuffers(1, &record->buffer);
        auto removed = buffers_.TryRemove(handle.slot);
        (void)removed;
    }

    void Destroy(TextureHandle handle) noexcept override {
        const TextureRecord* record = textures_.Find(handle.slot);
        if (record == nullptr || handle == white_) {
            return;
        }
        MakeCurrentForResources();
        ReleaseTexture(*record);
        auto removed = textures_.TryRemove(handle.slot);
        (void)removed;
    }

    void Destroy(PipelineHandle handle) noexcept override {
        const PipelineRecord* record = pipelines_.Find(handle.slot);
        if (record == nullptr) {
            return;
        }
        MakeCurrentForResources();
        gl_.DeleteProgram(record->program);
        gl_.DeleteVertexArrays(1, &record->vertexArray);
        auto removed = pipelines_.TryRemove(handle.slot);
        (void)removed;
    }

    void Destroy(SwapChainHandle handle) noexcept override {
        SwapChainRecord* record = swapChains_.Find(handle.slot);
        if (record == nullptr) {
            return;
        }
        // 이 창 DC에 context가 붙어 있으면 장치 창으로 옮긴 뒤 DC를 놓는다.
        if (currentDc_ == record->dc) {
            MakeCurrent(deviceDc_);
        }
        ReleaseDC(record->window, record->dc);
        auto removed = swapChains_.TryRemove(handle.slot);
        (void)removed;
    }

    Result<void> WaitIdle() noexcept override {
        glFinish();
        return Result<void>::Success();
    }

    StringView LastError() const noexcept override {
        return lastError_.View();
    }

    uint64 IgnoredDraws() const noexcept override {
        return ignoredDraws_;
    }

    uint64 ValidationErrorCount() const noexcept override {
        return validationErrors_;
    }

private:
    static void APIENTRY DebugMessage(GLenum, GLenum type, GLuint, GLenum, GLsizei length,
                                      const GLchar* message, const void* user) {
        auto* self = static_cast<OpenGLDevice*>(const_cast<void*>(user));
        if (type == GL_DEBUG_TYPE_ERROR_VALUE) {
            ++self->validationErrors_;
            if (message != nullptr && length > 0) {
                auto assigned =
                    self->lastError_.TryAssign(StringView(message, static_cast<usize>(length)));
                (void)assigned;
            }
        }
    }

    Result<void> Fail(const char* message) noexcept {
        auto assigned = lastError_.TryAssign(StringView(message, __builtin_strlen(message)));
        (void)assigned;
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    template <class T>
    Result<T> FailWith(const char* message) noexcept {
        auto failed = Fail(message);
        (void)failed;
        return Result<T>::Failure(ErrorCode::DependencyFailed);
    }

    bool MakeCurrent(HDC dc) noexcept {
        if (currentDc_ == dc) {
            return true;
        }
        if (!wglMakeCurrent(dc, context_)) {
            return false;
        }
        currentDc_ = dc;
        return true;
    }

    // 자원 생성·파괴는 어느 DC에 붙어 있든 같은 context면 된다. 붙어 있지 않을 때만 장치 창에
    // 붙인다.
    void MakeCurrentForResources() noexcept {
        if (currentDc_ == nullptr) {
            MakeCurrent(deviceDc_);
        }
    }

    void ReleaseTexture(const TextureRecord& record) noexcept {
        if (record.framebuffer != 0) {
            gl_.DeleteFramebuffers(1, &record.framebuffer);
        }
        glDeleteTextures(1, &record.texture);
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

    void StartPass(uint32 width, uint32 height, const ClearColor& clear) noexcept {
        recording_ = true;
        boundPipeline_ = {};
        bufferBound_ = false;
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
        constantsDirty_ = true;
        // 앞서 묶은 정점 버퍼를 새 파이프라인의 정점 배열·간격으로 다시 묶는다.
        if (bufferBound_) {
            SetVertexBuffer(boundBuffer_);
        }
    }

    void SetVertexBuffer(BufferHandle handle) noexcept override {
        const BufferRecord* record = buffers_.Find(handle.slot);
        bufferBound_ = record != nullptr;
        boundBuffer_ = handle;
        const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_.slot);
        if (record != nullptr && pipeline != nullptr) {
            gl_.VertexArrayVertexBuffer(pipeline->vertexArray, 0, record->buffer, 0,
                                        static_cast<GLsizei>(pipeline->stride));
        }
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

    void Draw(uint32 vertexCount, uint32 firstVertex) noexcept override {
        const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_.slot);
        if (pipeline == nullptr || !bufferBound_ || !buffers_.IsValid(boundBuffer_.slot)) {
            ++ignoredDraws_;
            return;
        }
        if (constantsDirty_) {
            // Rocks 행렬은 열 우선 저장이며 GLSL mat4도 열 우선이므로 전치하지 않는다.
            gl_.ProgramUniformMatrix4fv(pipeline->program, pipeline->transformLocation, 1, GL_FALSE,
                                        constants_.transform);
            gl_.ProgramUniform4fv(pipeline->program, pipeline->colorLocation, 1, constants_.color);
            constantsDirty_ = false;
        }
        glDrawArrays(GL_TRIANGLES, static_cast<GLint>(firstVertex),
                     static_cast<GLsizei>(vertexCount));
    }

    String adapterName_;
    String lastError_;
    SlotArray<BufferRecord> buffers_;
    SlotArray<TextureRecord> textures_;
    SlotArray<PipelineRecord> pipelines_;
    SlotArray<SwapChainRecord> swapChains_;
    GlFunctions gl_;
    WglSwapInterval swapInterval_ = nullptr;
    HWND deviceWindow_ = nullptr;
    HDC deviceDc_ = nullptr;
    HDC currentDc_ = nullptr;
    HGLRC context_ = nullptr;
    int pixelFormat_ = 0;
    PIXELFORMATDESCRIPTOR pixelDescriptor_ = {};
    int appliedInterval_ = -1;
    TextureHandle white_;
    SwapChainHandle presentTarget_;
    PipelineHandle boundPipeline_;
    BufferHandle boundBuffer_;
    DrawConstants constants_;
    uint64 ignoredDraws_ = 0;
    uint64 validationErrors_ = 0;
    bool recording_ = false;
    bool bufferBound_ = false;
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
