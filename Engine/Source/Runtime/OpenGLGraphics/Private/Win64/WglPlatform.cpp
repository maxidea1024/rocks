#include "../Common/GlesPlatform.h"

#include <Platform/Memory.h>

#include <Windows.h>

// Windows 미리보기(GLES31_PC)의 WGL 플랫폼 층이다(설계: Docs/Design/OpenGLESGraphics.ko.md 4.1절).
// 데스크톱 GL 전용 호출(glClipControl, GL_FRAMEBUFFER_SRGB 켜기, WGL sRGB 픽셀 형식, WGL context)은
// 이 파일에만 있다. GL/gl.h는 include하지 않는다(GlesApi.h가 GL 이름을 가진다).
namespace rocks::graphics::gles {

namespace {

// WGL 확장 상수다(Khronos wglext.h 값).
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
// WGL_ARB_framebuffer_sRGB·WGL_EXT_framebuffer_sRGB 공통 값이다.
constexpr int WGL_FRAMEBUFFER_SRGB_CAPABLE = 0x20A9;
constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int WGL_CONTEXT_FLAGS_ARB = 0x2094;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x0001;
constexpr int WGL_CONTEXT_ES2_PROFILE_BIT_EXT = 0x0004;
constexpr int WGL_CONTEXT_DEBUG_BIT_ARB = 0x0001;

// ARB_clip_control·EXT_clip_control 공통 값이다.
constexpr GLenum GL_UPPER_LEFT = 0x8CA2;
constexpr GLenum GL_ZERO_TO_ONE = 0x935F;

// 요구 context 버전이다. ES profile은 3.2, 데스크톱 core는 UE GLES31_PC 미리보기와 같은 4.3이다
// (UE WindowsOpenGLPlatform.cpp:200–205, 형식 정의 GLES31_PC deviceRequirements OPENGL_4_3).
constexpr int EsMajorVersion = 3;
constexpr int EsMinorVersion = 2;
constexpr int DesktopMajorVersion = 4;
constexpr int DesktopMinorVersion = 3;

using WglChoosePixelFormat = BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
using WglCreateContextAttribs = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using WglSwapInterval = BOOL(WINAPI*)(int);
using WglGetExtensionsString = const char*(WINAPI*)(HDC);
using GlClipControl = void(APIENTRY*)(GLenum, GLenum);

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

// wglGetProcAddress는 실패를 nullptr뿐 아니라 1·2·3·-1로도 알린다. GL 1.0·1.1 진입점은
// wglGetProcAddress로 얻을 수 없으므로 opengl32.dll export에서 얻는다(UE
// WindowsOpenGLPlatform.cpp:771–800과 같다).
void* LoadWglProc(const char* name, void* context) noexcept {
    (void)context;
    PROC procedure = wglGetProcAddress(name);
    const auto value = reinterpret_cast<intptr_t>(procedure);
    if (value != 0 && value != 1 && value != 2 && value != 3 && value != -1) {
        return reinterpret_cast<void*>(procedure);
    }

    HMODULE system = GetModuleHandleW(L"opengl32.dll");
    return system != nullptr ? reinterpret_cast<void*>(GetProcAddress(system, name)) : nullptr;
}

// 창 표면 하나다.
struct WglSurface {
    HWND window = nullptr;
    HDC dc = nullptr;
    // 이 창에 마지막으로 적용한 swap interval이다(WGL swap interval은 창마다 따로다). -1은 미적용.
    int appliedInterval = -1;
};

class WglPlatform final : public GlesPlatform {
public:
    ~WglPlatform() override {
        if (watcherContext_ != nullptr) {
            wglDeleteContext(watcherContext_);
        }

        if (watcherWindow_ != nullptr) {
            ReleaseDC(watcherWindow_, watcherDc_);
            DestroyWindow(watcherWindow_);
        }

        if (context_ != nullptr) {
            if (wglGetCurrentContext() == context_) {
                wglMakeCurrent(nullptr, nullptr);
            }

            wglDeleteContext(context_);
        }

        if (deviceWindow_ != nullptr) {
            ReleaseDC(deviceWindow_, deviceDc_);
            DestroyWindow(deviceWindow_);
        }
    }

    ErrorCode Initialize(const DeviceDesc& desc, const OpenGLDeviceOptions& options,
                         GlesCreateFailure& failure) noexcept {
        validation_ = desc.enableValidation;
        const ErrorCode bootstrapped = LoadWglExtensions(failure);
        if (bootstrapped != ErrorCode::None) {
            return bootstrapped;
        }

        // 장치 창에 sRGB 지원 더블 버퍼 픽셀 형식을 정한다. 스왑체인 창도 같은 형식을 쓴다.
        deviceWindow_ = CreateHiddenWindow();
        if (deviceWindow_ == nullptr) {
            return failure.Set(ErrorCode::DependencyFailed, "CreateWindowExW",
                               static_cast<int64>(GetLastError()),
                               "OpenGL device window creation failed");
        }

        deviceDc_ = GetDC(deviceWindow_);
        const int attributes[] = {
            WGL_DRAW_TO_WINDOW_ARB, TRUE, WGL_SUPPORT_OPENGL_ARB, TRUE, WGL_DOUBLE_BUFFER_ARB, TRUE,
            WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB, WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
            // 알파를 뺀 색 비트 수다(WGL_ARB_pixel_format).
            WGL_COLOR_BITS_ARB, 24, WGL_ALPHA_BITS_ARB, 8, WGL_DEPTH_BITS_ARB, 0,
            WGL_STENCIL_BITS_ARB, 0, srgbPixelFormat_ ? WGL_FRAMEBUFFER_SRGB_CAPABLE : 0, TRUE, 0};
        UINT count = 0;
        if (!choosePixelFormat_(deviceDc_, attributes, nullptr, 1, &pixelFormat_, &count) ||
            count == 0) {
            return failure.Set(ErrorCode::Unsupported, "wglChoosePixelFormatARB",
                               static_cast<int64>(GetLastError()),
                               "no accelerated double-buffered RGBA8 pixel format");
        }

        DescribePixelFormat(deviceDc_, pixelFormat_, sizeof(pixelDescriptor_), &pixelDescriptor_);
        if (!SetPixelFormat(deviceDc_, pixelFormat_, &pixelDescriptor_)) {
            return failure.Set(ErrorCode::DependencyFailed, "SetPixelFormat",
                               static_cast<int64>(GetLastError()),
                               "SetPixelFormat failed on the device window");
        }

        // context 종류를 고른다. Automatic은 ES profile을 먼저 시도하고, 만들 수 없거나 요구 확장이
        // 없으면 데스크톱 core로 내려간다. 두 시도의 원인을 모두 남긴다.
        const OpenGLContextProfile profile = options.contextProfile;
        ErrorCode result = ErrorCode::Unsupported;
        if (profile != OpenGLContextProfile::DesktopCore) {
            result = CreateContext(false, failure);
        }

        if (result != ErrorCode::None && profile == OpenGLContextProfile::Automatic) {
            Diagnostic first = failure.detail;
            result = CreateContext(true, failure);
            if (result != ErrorCode::None) {
                failure.detail.Text("; ES profile: ").Text(first.View());
            }
        } else if (profile == OpenGLContextProfile::DesktopCore) {
            result = CreateContext(true, failure);
        }

        if (result != ErrorCode::None) {
            return result;
        }

        return CreateWatcherContext(failure);
    }

    const PlatformInfo& Info() const noexcept override {
        return info_;
    }

    const GlesFunctions& Gl() const noexcept override {
        return gl_;
    }

    void* ProcAddress(const char* name) noexcept override {
        return LoadWglProc(name, nullptr);
    }

    // 캐시 대신 실제 current 상태를 확인한다. 같은 스레드의 다른 장치·GL 라이브러리가 context를
    // 바꿨을 수 있고, wglMakeCurrent가 실패하면 기존 context가 풀린다.
    bool MakeCurrent() noexcept override {
        return MakeCurrentOn(deviceDc_);
    }

    bool IsCurrent() const noexcept override {
        return context_ != nullptr && wglGetCurrentContext() == context_;
    }

    void ReleaseCurrent() noexcept override {
        if (IsCurrent()) {
            wglMakeCurrent(nullptr, nullptr);
        }
    }

    Result<PlatformSurface> CreateSurface(const NativeSurface& surface,
                                          Diagnostic& error) noexcept override {
        auto* window = static_cast<HWND>(surface.window);
        // DC를 스왑체인 수명 동안 쥐므로 공용 DC가 아니라 창 전용 DC(CS_OWNDC)여야 한다.
        if ((GetClassLongPtrW(window, GCL_STYLE) & CS_OWNDC) == 0) {
            error.Text("swap chain window class must use CS_OWNDC");
            return Result<PlatformSurface>::Failure(ErrorCode::InvalidArgument);
        }

        HDC dc = GetDC(window);
        if (dc == nullptr) {
            error.Text("GetDC failed on the swap chain window (Win32 error ")
                .Number(GetLastError())
                .Text(")");
            return Result<PlatformSurface>::Failure(ErrorCode::DependencyFailed);
        }

        // 창의 픽셀 형식은 한 번만 정할 수 있다. 이미 다른 형식이면 이 context로 그릴 수 없다.
        const int existing = GetPixelFormat(dc);
        if (existing != 0 && existing != pixelFormat_) {
            ReleaseDC(window, dc);
            error.Text("window pixel format is incompatible with the context");
            return Result<PlatformSurface>::Failure(ErrorCode::InvalidArgument);
        }

        if (existing == 0 && !SetPixelFormat(dc, pixelFormat_, &pixelDescriptor_)) {
            const DWORD code = GetLastError();
            ReleaseDC(window, dc);
            error.Text("SetPixelFormat failed on the swap chain window (Win32 error ")
                .Number(code)
                .Text(")");
            return Result<PlatformSurface>::Failure(ErrorCode::DependencyFailed);
        }

        auto memory = allocator_->Allocate(sizeof(WglSurface), alignof(WglSurface));
        if (!memory) {
            ReleaseDC(window, dc);
            error.Text("swap chain surface allocation failed");
            return Result<PlatformSurface>::Failure(memory.Error());
        }

        auto* record = static_cast<WglSurface*>(memory.Value());
        *record = WglSurface{window, dc, -1};
        return Result<PlatformSurface>::Success(record);
    }

    bool MakeCurrentOnSurface(PlatformSurface surface) noexcept override {
        return MakeCurrentOn(static_cast<WglSurface*>(surface)->dc);
    }

    bool SurfaceExtent(PlatformSurface surface, uint32& width, uint32& height) noexcept override {
        RECT client = {};
        if (!GetClientRect(static_cast<WglSurface*>(surface)->window, &client) ||
            client.right <= client.left || client.bottom <= client.top) {
            return false;
        }

        width = static_cast<uint32>(client.right - client.left);
        height = static_cast<uint32>(client.bottom - client.top);
        return true;
    }

    bool Present(PlatformSurface surface, bool vsync, Diagnostic& error) noexcept override {
        auto* record = static_cast<WglSurface*>(surface);
        const int interval = vsync ? 1 : 0;
        // swap interval은 current 창에 걸린다(값이 바뀔 때만 부른다, UE와 같다).
        if (swapInterval_ != nullptr && record->appliedInterval != interval &&
            swapInterval_(interval)) {
            record->appliedInterval = interval;
        }

        if (!SwapBuffers(record->dc)) {
            error.Text("SwapBuffers failed (Win32 error ").Number(GetLastError()).Text(")");
            return false;
        }

        return true;
    }

    void DetachSurface(PlatformSurface surface) noexcept override {
        const auto* record = static_cast<WglSurface*>(surface);
        if (wglGetCurrentContext() == context_ && wglGetCurrentDC() == record->dc &&
            !MakeCurrentOn(deviceDc_)) {
            wglMakeCurrent(nullptr, nullptr);
        }
    }

    void DestroySurface(PlatformSurface surface) noexcept override {
        auto* record = static_cast<WglSurface*>(surface);
        ReleaseDC(record->window, record->dc);
        (void)allocator_->Deallocate(record);
    }

    void* InsertFence() noexcept override {
        GLsync sync = gl_.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        // 다른 context(관찰 스레드)가 기다리므로 fence 명령을 GPU로 보낸다.
        gl_.Flush();
        return sync;
    }

    // --- FenceWaitSource: 관찰 스레드가 공유 context로 sync를 기다린다. sync 객체는 공유 그룹
    // 전체에서 보이고 소유 스레드가 glFenceSync 뒤 glFlush하므로 다른 context에서 기다려도 끝난다.

    void OnWatcherStart() noexcept override {
        watcherCurrent_ = wglMakeCurrent(watcherDc_, watcherContext_) != FALSE;
    }

    void OnWatcherStop() noexcept override {
        if (watcherCurrent_) {
            wglMakeCurrent(nullptr, nullptr);
            watcherCurrent_ = false;
        }
    }

    bool WaitSubmission(uint64 value, void* token) noexcept override {
        (void)value;
        // sync를 만들지 못한 제출은 소유 스레드가 glFinish로 끝냈다.
        if (token == nullptr) {
            return true;
        }

        if (!watcherCurrent_) {
            return false;
        }

        for (;;) {
            const GLenum waited =
                gl_.ClientWaitSync(static_cast<GLsync>(token), 0, WaitSliceNanoseconds);
            if (waited == GL_ALREADY_SIGNALED || waited == GL_CONDITION_SATISFIED) {
                return true;
            }

            if (waited != GL_TIMEOUT_EXPIRED) {
                return false;
            }
        }
    }

    void ReleaseToken(void* token) noexcept override {
        if (token != nullptr) {
            gl_.DeleteSync(static_cast<GLsync>(token));
        }
    }

    Allocator* allocator_ = nullptr;

private:
    // glClientWaitSync 한 번의 최대 대기다. 끝나지 않았으면 다시 기다린다(완료 판정에 시간을 쓰지
    // 않는다).
    static constexpr GLuint64 WaitSliceNanoseconds = 1000000000ull;

    bool MakeCurrentOn(HDC dc) noexcept {
        if (wglGetCurrentContext() == context_ && wglGetCurrentDC() == dc) {
            return true;
        }

        return wglMakeCurrent(dc, context_) != FALSE;
    }

    // 임시 창·legacy context로 WGL 확장 함수를 얻는다(픽셀 형식은 창마다 한 번만 정할 수 있다).
    ErrorCode LoadWglExtensions(GlesCreateFailure& failure) noexcept {
        HWND dummy = CreateHiddenWindow();
        if (dummy == nullptr) {
            return failure.Set(ErrorCode::DependencyFailed, "CreateWindowExW",
                               static_cast<int64>(GetLastError()),
                               "OpenGL helper window creation failed");
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

        const DWORD legacyError = legacy == nullptr ? GetLastError() : 0;
        if (legacy != nullptr && wglMakeCurrent(dc, legacy)) {
            choosePixelFormat_ = reinterpret_cast<WglChoosePixelFormat>(
                LoadWglProc("wglChoosePixelFormatARB", nullptr));
            createContext_ = reinterpret_cast<WglCreateContextAttribs>(
                LoadWglProc("wglCreateContextAttribsARB", nullptr));
            const auto extensions = reinterpret_cast<WglGetExtensionsString>(
                LoadWglProc("wglGetExtensionsStringARB", nullptr));
            const char* list = extensions != nullptr ? extensions(dc) : nullptr;
            esProfile_ = HasExtensionToken(list, "WGL_EXT_create_context_es2_profile");
            srgbPixelFormat_ = HasExtensionToken(list, "WGL_ARB_framebuffer_sRGB") ||
                               HasExtensionToken(list, "WGL_EXT_framebuffer_sRGB");
            swapControl_ = HasExtensionToken(list, "WGL_EXT_swap_control");
            wglMakeCurrent(nullptr, nullptr);
        }

        if (legacy != nullptr) {
            wglDeleteContext(legacy);
        }

        ReleaseDC(dummy, dc);
        DestroyWindow(dummy);
        if (choosePixelFormat_ == nullptr || createContext_ == nullptr) {
            return failure.Set(ErrorCode::Unsupported, "wglCreateContext",
                               static_cast<int64>(legacyError),
                               "WGL_ARB_pixel_format / WGL_ARB_create_context are not available");
        }

        return ErrorCode::None;
    }

    // ES profile(desktop = false) 또는 데스크톱 core(true) context를 만들고 요구 확장을 확인한다.
    // 실패하면 만든 context를 지우고 failure에 원인을 적는다.
    ErrorCode CreateContext(bool desktop, GlesCreateFailure& failure) noexcept {
        if (!desktop && !esProfile_) {
            return failure.Set(ErrorCode::Unsupported, "wglCreateContextAttribsARB", 0,
                               "WGL_EXT_create_context_es2_profile is not available");
        }

        const int attributes[] = {WGL_CONTEXT_MAJOR_VERSION_ARB,
                                  desktop ? DesktopMajorVersion : EsMajorVersion,
                                  WGL_CONTEXT_MINOR_VERSION_ARB,
                                  desktop ? DesktopMinorVersion : EsMinorVersion,
                                  WGL_CONTEXT_PROFILE_MASK_ARB,
                                  desktop ? WGL_CONTEXT_CORE_PROFILE_BIT_ARB
                                          : WGL_CONTEXT_ES2_PROFILE_BIT_EXT,
                                  WGL_CONTEXT_FLAGS_ARB,
                                  validation_ ? WGL_CONTEXT_DEBUG_BIT_ARB : 0,
                                  0};
        HGLRC context = createContext_(deviceDc_, nullptr, attributes);
        if (context == nullptr) {
            return failure.Set(ErrorCode::Unsupported, "wglCreateContextAttribsARB",
                               static_cast<int64>(GetLastError()),
                               desktop ? "OpenGL 4.3 core context creation failed"
                                       : "OpenGL ES 3.2 context creation failed");
        }

        if (!wglMakeCurrent(deviceDc_, context)) {
            const DWORD code = GetLastError();
            wglDeleteContext(context);
            return failure.Set(ErrorCode::DependencyFailed, "wglMakeCurrent",
                               static_cast<int64>(code), "cannot make the new context current");
        }

        const ErrorCode checked = CheckContext(desktop, failure);
        if (checked != ErrorCode::None) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(context);
            return checked;
        }

        context_ = context;
        for (usize index = 0; index < sizeof(attributes) / sizeof(attributes[0]); ++index) {
            attributes_[index] = attributes[index];
        }

        info_.desktopContext = desktop;
        info_.clip = ClipConvention::ClipControlUpperLeft;
        info_.contextName = desktop ? "OpenGL 4.3 core, GLES31_PC preview"
                                    : "OpenGL ES 3.2, WGL ES profile, GLES31_PC preview";
        if (swapControl_) {
            swapInterval_ =
                reinterpret_cast<WglSwapInterval>(LoadWglProc("wglSwapIntervalEXT", nullptr));
        }

        // 클립 공간: D3D와 같은 [0, 1] 깊이와 위쪽 원점(ShaderSystem 7.2절 8항). RenderTarget·
        // 스왑체인 텍스처 모두 저장소 첫 행이 이미지 위쪽이 된다.
        clipControl_(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
        if (desktop) {
            // 데스크톱 core는 GL_FRAMEBUFFER_SRGB를 켜야 sRGB 렌더 타깃에 쓸 때 인코딩한다(ES는
            // 늘 인코딩한다).
            gl_.Enable(GL_FRAMEBUFFER_SRGB);
        }

        return ErrorCode::None;
    }

    // 지금 context가 ES 3.2 공통 API를 갖추었는지 본다(버전·함수·확장).
    ErrorCode CheckContext(bool desktop, GlesCreateFailure& failure) noexcept {
        GlesFunctions loaded;
        const char* missing = "";
        if (!LoadCoreFunctions(loaded, &LoadWglProc, nullptr, &missing)) {
            failure.Set(ErrorCode::Unsupported, "wglGetProcAddress", 0,
                        "required OpenGL ES 3.2 function is missing: ");
            failure.detail.Text(missing);
            return ErrorCode::Unsupported;
        }

        GLint major = 0;
        GLint minor = 0;
        loaded.GetIntegerv(GL_MAJOR_VERSION, &major);
        loaded.GetIntegerv(GL_MINOR_VERSION, &minor);
        const int wantedMajor = desktop ? DesktopMajorVersion : EsMajorVersion;
        const int wantedMinor = desktop ? DesktopMinorVersion : EsMinorVersion;
        if (major < wantedMajor || (major == wantedMajor && minor < wantedMinor)) {
            failure.Set(ErrorCode::Unsupported, "GL_MAJOR_VERSION", major * 10 + minor,
                        desktop ? "OpenGL 4.3 is required, the context reports "
                                : "OpenGL ES 3.2 is required, the context reports ");
            failure.detail.Number(static_cast<uint64>(major))
                .Text(".")
                .Number(static_cast<uint64>(minor));
            return ErrorCode::Unsupported;
        }

        // 데스크톱 core가 `#version 320 es` 원문을 받으려면 GL_ARB_ES3_2_compatibility가 있어야
        // 한다(측정 S12). 클립 공간은 ES는 GL_EXT_clip_control, 데스크톱은 GL_ARB_clip_control이다.
        const char* clipExtension = desktop ? "GL_ARB_clip_control" : "GL_EXT_clip_control";
        const char* clipFunction = desktop ? "glClipControl" : "glClipControlEXT";
        if (desktop && !HasGlExtension(loaded, "GL_ARB_ES3_2_compatibility")) {
            return failure.Set(ErrorCode::Unsupported, "GL_EXTENSIONS", 0,
                               "GL_ARB_ES3_2_compatibility is required for the GLES31_PC preview");
        }

        auto clip = reinterpret_cast<GlClipControl>(LoadWglProc(clipFunction, nullptr));
        if (!HasGlExtension(loaded, clipExtension) || clip == nullptr) {
            failure.Set(ErrorCode::Unsupported, "GL_EXTENSIONS", 0, "");
            failure.detail.Text(clipExtension).Text(" is required for the GLES31_PC preview");
            return ErrorCode::Unsupported;
        }

        gl_ = loaded;
        clipControl_ = clip;
        return ErrorCode::None;
    }

    // fence 관찰 스레드용 공유 context를 숨김 창에 만든다(sync 객체를 공유한다).
    ErrorCode CreateWatcherContext(GlesCreateFailure& failure) noexcept {
        watcherWindow_ = CreateHiddenWindow();
        if (watcherWindow_ == nullptr) {
            return failure.Set(ErrorCode::DependencyFailed, "CreateWindowExW",
                               static_cast<int64>(GetLastError()),
                               "OpenGL fence watcher window creation failed");
        }

        watcherDc_ = GetDC(watcherWindow_);
        if (!SetPixelFormat(watcherDc_, pixelFormat_, &pixelDescriptor_)) {
            return failure.Set(ErrorCode::DependencyFailed, "SetPixelFormat",
                               static_cast<int64>(GetLastError()),
                               "SetPixelFormat failed on the fence watcher window");
        }

        watcherContext_ = createContext_(watcherDc_, context_, attributes_);
        if (watcherContext_ == nullptr) {
            return failure.Set(ErrorCode::DependencyFailed, "wglCreateContextAttribsARB",
                               static_cast<int64>(GetLastError()),
                               "shared context for the fence watcher failed");
        }

        return ErrorCode::None;
    }

    PlatformInfo info_;
    GlesFunctions gl_;
    GlClipControl clipControl_ = nullptr;
    WglChoosePixelFormat choosePixelFormat_ = nullptr;
    WglCreateContextAttribs createContext_ = nullptr;
    WglSwapInterval swapInterval_ = nullptr;
    bool esProfile_ = false;
    bool srgbPixelFormat_ = false;
    bool swapControl_ = false;
    bool validation_ = false;
    HWND deviceWindow_ = nullptr;
    HDC deviceDc_ = nullptr;
    HGLRC context_ = nullptr;
    // 장치 context를 만든 속성이다(관찰 스레드의 공유 context도 같은 속성으로 만든다).
    int attributes_[9] = {};
    // fence 관찰 스레드의 숨김 창·DC·공유 context다.
    HWND watcherWindow_ = nullptr;
    HDC watcherDc_ = nullptr;
    HGLRC watcherContext_ = nullptr;
    bool watcherCurrent_ = false;
    int pixelFormat_ = 0;
    PIXELFORMATDESCRIPTOR pixelDescriptor_ = {};
};

} // namespace

Result<UniquePtr<GlesPlatform>> CreateGlesPlatform(Allocator& allocator, const DeviceDesc& desc,
                                                   const OpenGLDeviceOptions& options,
                                                   GlesCreateFailure& failure) noexcept {
    auto created = UniquePtr<WglPlatform>::TryMake(allocator);
    if (!created) {
        failure.Set(created.Error(), "TryMake", 0, "OpenGL platform allocation failed");
        return Result<UniquePtr<GlesPlatform>>::Failure(created.Error());
    }

    created.Value()->allocator_ = &allocator;
    const ErrorCode initialized = created.Value()->Initialize(desc, options, failure);
    if (initialized != ErrorCode::None) {
        return Result<UniquePtr<GlesPlatform>>::Failure(initialized);
    }

    return Result<UniquePtr<GlesPlatform>>::Success(UniquePtr<GlesPlatform>(Move(created.Value())));
}

} // namespace rocks::graphics::gles
