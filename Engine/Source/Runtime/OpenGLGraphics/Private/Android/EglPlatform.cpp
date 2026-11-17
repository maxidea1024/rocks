#include "../Common/GlesPlatform.h"

#include <Platform/Memory.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>

// Android(GLES31_ANDROID)의 EGL 플랫폼 층이다(설계: Docs/Design/OpenGLESGraphics.ko.md 4.2절).
// context 생성·장치 표면·fence(EGL_KHR_fence_sync)·창 표면 생성과 present를 맡는다. 창 표면의
// 수명(ANativeWindow 생성·소실·재생성)과 앱 껍데기는 P13 Android 실행 기반이 붙이며, 지금은
// 컴파일과 장치(창 없는 offscreen) 경로만 있다.
namespace rocks::graphics::gles {

namespace {

// 요구 context 버전이다. ES 3.2 미만 기기는 받지 않는다(ShaderSystem 2절, UE
// AndroidOpenGL.cpp:453–485).
constexpr EGLint EsMajorVersion = 3;
constexpr EGLint EsMinorVersion = 2;
// 장치 표면용 pbuffer 크기다(EGL_KHR_surfaceless_context가 없을 때만 쓴다, UE
// AndroidEGL.cpp:270–290).
constexpr EGLint DevicePbufferExtent = 1;

// EGL_KHR_fence_sync·EGL_KHR_gl_colorspace·EGL_KHR_create_context 값이다(Khronos eglext.h). NDK
// 헤더 판에 따라 매크로가 없을 수 있어 직접 둔다.
constexpr EGLint EglOpenGlEs3Bit = 0x0040;
constexpr EGLint EglContextMajorVersion = 0x3098;
constexpr EGLint EglContextMinorVersion = 0x30FB;
constexpr EGLint EglContextFlags = 0x30FC;
constexpr EGLint EglContextDebugBit = 0x0001;
constexpr EGLint EglGlColorspace = 0x309D;
constexpr EGLint EglGlColorspaceSrgb = 0x3089;
constexpr EGLenum EglSyncFence = 0x30F9;
constexpr EGLint EglSyncFlushCommandsBit = 0x0001;
constexpr EGLint EglTimeoutExpired = 0x30F5;
constexpr EGLint EglConditionSatisfied = 0x30F6;

using EglSync = void*;
using CreateSync = EglSync (*)(EGLDisplay, EGLenum, const EGLint*);
using DestroySync = EGLBoolean (*)(EGLDisplay, EglSync);
using ClientWaitSync = EGLint (*)(EGLDisplay, EglSync, EGLint, uint64);

// EGL은 ES 3.2 core 함수도 eglGetProcAddress로 준다(EGL 1.5, EGL_KHR_get_all_proc_addresses).
// 주지 않는 구현을 위해 링크한 GLESv3에서 dlsym으로도 찾는다.
void* LoadEglProc(const char* name, void* context) noexcept {
    (void)context;
    void* procedure = reinterpret_cast<void*>(eglGetProcAddress(name));
    if (procedure == nullptr) {
        procedure = dlsym(RTLD_DEFAULT, name);
    }

    return procedure;
}

class EglPlatform final : public GlesPlatform {
public:
    ~EglPlatform() override {
        if (display_ == EGL_NO_DISPLAY) {
            return;
        }

        if (eglGetCurrentContext() == context_) {
            eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }

        if (context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(display_, context_);
        }

        if (deviceSurface_ != EGL_NO_SURFACE) {
            eglDestroySurface(display_, deviceSurface_);
        }

        eglTerminate(display_);
    }

    ErrorCode Initialize(const DeviceDesc& desc, const OpenGLDeviceOptions& options,
                         GlesCreateFailure& failure) noexcept {
        if (options.contextProfile == OpenGLContextProfile::DesktopCore) {
            return failure.Set(ErrorCode::Unsupported, "OpenGLDeviceOptions", 0,
                               "the desktop core context exists only on the Windows preview");
        }

        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint major = 0;
        EGLint minor = 0;
        if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, &major, &minor)) {
            display_ = EGL_NO_DISPLAY;
            return failure.Set(ErrorCode::Unsupported, "eglInitialize", eglGetError(),
                               "EGL display initialization failed");
        }

        const char* extensions = eglQueryString(display_, EGL_EXTENSIONS);
        const bool createContext = HasExtensionToken(extensions, "EGL_KHR_create_context");
        surfaceless_ = HasExtensionToken(extensions, "EGL_KHR_surfaceless_context");
        srgbSurface_ = HasExtensionToken(extensions, "EGL_KHR_gl_colorspace");
        if (!HasExtensionToken(extensions, "EGL_KHR_fence_sync")) {
            return failure.Set(ErrorCode::Unsupported, "eglQueryString", 0,
                               "EGL_KHR_fence_sync is required for GPU completion waits");
        }

        createSync_ = reinterpret_cast<CreateSync>(eglGetProcAddress("eglCreateSyncKHR"));
        destroySync_ = reinterpret_cast<DestroySync>(eglGetProcAddress("eglDestroySyncKHR"));
        clientWaitSync_ =
            reinterpret_cast<ClientWaitSync>(eglGetProcAddress("eglClientWaitSyncKHR"));
        if (createSync_ == nullptr || destroySync_ == nullptr || clientWaitSync_ == nullptr) {
            return failure.Set(ErrorCode::Unsupported, "eglGetProcAddress", eglGetError(),
                               "EGL_KHR_fence_sync functions are missing");
        }

        if (!eglBindAPI(EGL_OPENGL_ES_API)) {
            return failure.Set(ErrorCode::Unsupported, "eglBindAPI", eglGetError(),
                               "OpenGL ES API binding failed");
        }

        const EGLint configAttributes[] = {EGL_RENDERABLE_TYPE,
                                           EglOpenGlEs3Bit,
                                           EGL_SURFACE_TYPE,
                                           EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                                           EGL_RED_SIZE,
                                           8,
                                           EGL_GREEN_SIZE,
                                           8,
                                           EGL_BLUE_SIZE,
                                           8,
                                           EGL_ALPHA_SIZE,
                                           8,
                                           EGL_DEPTH_SIZE,
                                           0,
                                           EGL_STENCIL_SIZE,
                                           0,
                                           EGL_CONFIG_CAVEAT,
                                           EGL_NONE,
                                           EGL_NONE};
        EGLint count = 0;
        if (!eglChooseConfig(display_, configAttributes, &config_, 1, &count) || count == 0) {
            return failure.Set(ErrorCode::Unsupported, "eglChooseConfig", eglGetError(),
                               "no RGBA8 OpenGL ES 3 window and pbuffer config");
        }

        // ES 3.2 context다. EGL_KHR_create_context가 있으면 버전과 debug flag를 요청하고, 없으면
        // client version 3으로 만든 뒤 실제 버전을 확인한다(UE는 3.1로 다시 시도하지만 Rocks는 3.2
        // 미만을 받지 않는다).
        const EGLint versioned[] = {EglContextMajorVersion,
                                    EsMajorVersion,
                                    EglContextMinorVersion,
                                    EsMinorVersion,
                                    EglContextFlags,
                                    desc.enableValidation ? EglContextDebugBit : 0,
                                    EGL_NONE};
        const EGLint legacy[] = {EGL_CONTEXT_CLIENT_VERSION, EsMajorVersion, EGL_NONE};
        context_ =
            eglCreateContext(display_, config_, EGL_NO_CONTEXT, createContext ? versioned : legacy);
        if (context_ == EGL_NO_CONTEXT) {
            return failure.Set(ErrorCode::Unsupported, "eglCreateContext", eglGetError(),
                               "OpenGL ES 3.2 context creation failed");
        }

        if (!surfaceless_) {
            const EGLint pbuffer[] = {EGL_WIDTH, DevicePbufferExtent, EGL_HEIGHT,
                                      DevicePbufferExtent, EGL_NONE};
            deviceSurface_ = eglCreatePbufferSurface(display_, config_, pbuffer);
            if (deviceSurface_ == EGL_NO_SURFACE) {
                return failure.Set(ErrorCode::DependencyFailed, "eglCreatePbufferSurface",
                                   eglGetError(), "device pbuffer creation failed");
            }
        }

        if (!MakeCurrent()) {
            return failure.Set(ErrorCode::DependencyFailed, "eglMakeCurrent", eglGetError(),
                               "cannot make the new context current");
        }

        const char* missing = "";
        if (!LoadCoreFunctions(gl_, &LoadEglProc, nullptr, &missing)) {
            failure.Set(ErrorCode::Unsupported, "eglGetProcAddress", 0,
                        "required OpenGL ES 3.2 function is missing: ");
            failure.detail.Text(missing);
            return ErrorCode::Unsupported;
        }

        GLint glMajor = 0;
        GLint glMinor = 0;
        gl_.GetIntegerv(GL_MAJOR_VERSION, &glMajor);
        gl_.GetIntegerv(GL_MINOR_VERSION, &glMinor);
        if (glMajor < EsMajorVersion || (glMajor == EsMajorVersion && glMinor < EsMinorVersion)) {
            failure.Set(ErrorCode::Unsupported, "GL_MAJOR_VERSION", glMajor * 10 + glMinor,
                        "OpenGL ES 3.2 is required, the context reports ");
            failure.detail.Number(static_cast<uint64>(glMajor))
                .Text(".")
                .Number(static_cast<uint64>(glMinor));
            return ErrorCode::Unsupported;
        }

        // 클립 공간은 셰이더가 보정하고(fixup_clipspace·flip_vert_y) 앞면은 GL_CW다. UE도
        // Android에서 clip control을 쓰지 않는다(OpenGLDrv.cpp:469–475는 Windows·Linux만).
        info_.clip = ClipConvention::ShaderFlip;
        info_.desktopContext = false;
        info_.contextName = "OpenGL ES 3.2, EGL, GLES31_ANDROID";
        return ErrorCode::None;
    }

    const PlatformInfo& Info() const noexcept override {
        return info_;
    }

    const GlesFunctions& Gl() const noexcept override {
        return gl_;
    }

    void* ProcAddress(const char* name) noexcept override {
        return LoadEglProc(name, nullptr);
    }

    bool MakeCurrent() noexcept override {
        if (eglGetCurrentContext() == context_ &&
            eglGetCurrentSurface(EGL_DRAW) == deviceSurface_) {
            return true;
        }

        return eglMakeCurrent(display_, deviceSurface_, deviceSurface_, context_) == EGL_TRUE;
    }

    bool IsCurrent() const noexcept override {
        return context_ != EGL_NO_CONTEXT && eglGetCurrentContext() == context_;
    }

    void ReleaseCurrent() noexcept override {
        if (IsCurrent()) {
            eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
    }

    Result<PlatformSurface> CreateSurface(const NativeSurface& surface,
                                          Diagnostic& error) noexcept override {
        // 스왑체인 텍스처(sRGB)를 창에 byte 그대로 옮기려면 창 표면도 sRGB여야 한다(ES의 blit은
        // sRGB 원본을 선형으로 읽는다). EGL_KHR_gl_colorspace가 없는 기기의 셰이더 변환 present는
        // P13에서 더한다. 그때까지는 거짓 색을 내지 않고 Unsupported다.
        if (!srgbSurface_) {
            error.Text("EGL_KHR_gl_colorspace is required for an sRGB window surface");
            return Result<PlatformSurface>::Failure(ErrorCode::Unsupported);
        }

        const EGLint attributes[] = {EglGlColorspace, EglGlColorspaceSrgb, EGL_NONE};
        EGLSurface created = eglCreateWindowSurface(
            display_, config_, static_cast<EGLNativeWindowType>(surface.window), attributes);
        if (created == EGL_NO_SURFACE) {
            error.Text("eglCreateWindowSurface failed (EGL error ").Hex(eglGetError()).Text(")");
            return Result<PlatformSurface>::Failure(ErrorCode::DependencyFailed);
        }

        return Result<PlatformSurface>::Success(created);
    }

    bool MakeCurrentOnSurface(PlatformSurface surface) noexcept override {
        auto* target = static_cast<EGLSurface>(surface);
        if (eglGetCurrentContext() == context_ && eglGetCurrentSurface(EGL_DRAW) == target) {
            return true;
        }

        return eglMakeCurrent(display_, target, target, context_) == EGL_TRUE;
    }

    bool SurfaceExtent(PlatformSurface surface, uint32& width, uint32& height) noexcept override {
        EGLint queriedWidth = 0;
        EGLint queriedHeight = 0;
        auto* target = static_cast<EGLSurface>(surface);
        if (!eglQuerySurface(display_, target, EGL_WIDTH, &queriedWidth) ||
            !eglQuerySurface(display_, target, EGL_HEIGHT, &queriedHeight) || queriedWidth <= 0 ||
            queriedHeight <= 0) {
            return false;
        }

        width = static_cast<uint32>(queriedWidth);
        height = static_cast<uint32>(queriedHeight);
        return true;
    }

    bool Present(PlatformSurface surface, bool vsync, Diagnostic& error) noexcept override {
        auto* target = static_cast<EGLSurface>(surface);
        // swap interval은 current 표면에 걸린다.
        const EGLint interval = vsync ? 1 : 0;
        if (target != intervalSurface_ || interval != appliedInterval_) {
            if (eglSwapInterval(display_, interval)) {
                intervalSurface_ = target;
                appliedInterval_ = interval;
            }
        }

        if (!eglSwapBuffers(display_, target)) {
            error.Text("eglSwapBuffers failed (EGL error ").Hex(eglGetError()).Text(")");
            return false;
        }

        return true;
    }

    void DetachSurface(PlatformSurface surface) noexcept override {
        auto* target = static_cast<EGLSurface>(surface);
        if (eglGetCurrentContext() == context_ && eglGetCurrentSurface(EGL_DRAW) == target &&
            !MakeCurrent()) {
            eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
    }

    void DestroySurface(PlatformSurface surface) noexcept override {
        auto* target = static_cast<EGLSurface>(surface);
        if (target == intervalSurface_) {
            intervalSurface_ = EGL_NO_SURFACE;
        }

        eglDestroySurface(display_, target);
    }

    // EGL fence는 지금 context의 명령 흐름에 들어가고, context 없이 어느 스레드에서나 기다릴 수
    // 있다(eglClientWaitSyncKHR). 그래서 관찰 스레드에 공유 context가 필요 없다(UE도 Android에서
    // context를 하나만 쓴다, AndroidEGL.cpp:77–78).
    void* InsertFence() noexcept override {
        EglSync sync = createSync_(display_, EglSyncFence, nullptr);
        if (sync == nullptr) {
            return nullptr;
        }

        gl_.Flush();
        return sync;
    }

    bool WaitSubmission(uint64 value, void* token) noexcept override {
        (void)value;
        if (token == nullptr) {
            return true;
        }

        for (;;) {
            const EGLint waited =
                clientWaitSync_(display_, token, EglSyncFlushCommandsBit, WaitSliceNanoseconds);
            if (waited == EglConditionSatisfied) {
                return true;
            }

            if (waited != EglTimeoutExpired) {
                return false;
            }
        }
    }

    void ReleaseToken(void* token) noexcept override {
        if (token != nullptr) {
            destroySync_(display_, token);
        }
    }

private:
    // eglClientWaitSyncKHR 한 번의 최대 대기다. 끝나지 않았으면 다시 기다린다(완료 판정에 시간을
    // 쓰지 않는다).
    static constexpr uint64 WaitSliceNanoseconds = 1000000000ull;

    PlatformInfo info_;
    GlesFunctions gl_;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    // 장치 표면이다. EGL_KHR_surfaceless_context가 있으면 표면 없이 붙는다(EGL_NO_SURFACE).
    EGLSurface deviceSurface_ = EGL_NO_SURFACE;
    EGLSurface intervalSurface_ = EGL_NO_SURFACE;
    EGLint appliedInterval_ = -1;
    CreateSync createSync_ = nullptr;
    DestroySync destroySync_ = nullptr;
    ClientWaitSync clientWaitSync_ = nullptr;
    bool surfaceless_ = false;
    bool srgbSurface_ = false;
};

} // namespace

Result<UniquePtr<GlesPlatform>> CreateGlesPlatform(Allocator& allocator, const DeviceDesc& desc,
                                                   const OpenGLDeviceOptions& options,
                                                   GlesCreateFailure& failure) noexcept {
    auto created = UniquePtr<EglPlatform>::TryMake(allocator);
    if (!created) {
        failure.Set(created.Error(), "TryMake", 0, "OpenGL platform allocation failed");
        return Result<UniquePtr<GlesPlatform>>::Failure(created.Error());
    }

    const ErrorCode initialized = created.Value()->Initialize(desc, options, failure);
    if (initialized != ErrorCode::None) {
        return Result<UniquePtr<GlesPlatform>>::Failure(initialized);
    }

    return Result<UniquePtr<GlesPlatform>>::Success(UniquePtr<GlesPlatform>(Move(created.Value())));
}

} // namespace rocks::graphics::gles
