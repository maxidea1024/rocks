#include "../SplashPresenter.h"

#include <Core/Move.h>
#include <Platform/Atomics.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>
#include <Platform/Win64/WideString.h>
#include <SplashScreen/SplashLayout.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>
#include <ShellScalingApi.h>
#include <dwmapi.h>
#include <wincodec.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

// Windows 스플래시 창이다(Docs/Design/SplashScreen 9.2절). 스플래시 스레드가 창을 만들고 메시지를
// 펌프한다. 이미지는 WIC로 풀어(PNG·JPEG·BMP) 창 크기에 맞춘 뒤 바탕색 위에 합성하고, 글자는
// GDI(Segoe UI)로 back buffer DIB에 그려 WM_PAINT에서 옮긴다. UE WindowsPlatformSplash와 같은
// 구조이고, 스레드 단위 DPI 문맥과 @2x 원본으로 고해상도 모니터에서 선명하다.
namespace rocks::splash {

namespace {

constexpr wchar_t SplashWindowClassName[] = L"RocksSplashWindow";
constexpr wchar_t SplashFontFace[] = L"Segoe UI";

// 대화상자 hook이 보내는 숨김 메시지다.
constexpr UINT SplashDialogHideMessage = WM_APP + 1;

// 96 DPI가 배율 1.0이다.
constexpr double BaseDpi = 96.0;

// Windows 11 창 모서리 설정(DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2)이다. 오래된
// SDK에도 맞게 값으로 둔다. 지원하지 않는 OS는 호출이 실패하고 각진 모서리로 남는다.
constexpr DWORD DwmCornerPreferenceAttribute = 33;
constexpr DWORD DwmCornerRound = 2;

// WIC 이미징 factory의 CLSID다(CLSID_WICImagingFactory — uuid 라이브러리 없이 쓰려고 값으로 둔다).
constexpr CLSID WicImagingFactoryClsid = {
    0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};

// 미리 곱한 alpha BGRA 32bpp 형식(GUID_WICPixelFormat32bppPBGRA)이다.
constexpr GUID WicPixelFormat32bppPbgra = {
    0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10}};

// 픽셀당 byte 수다(BGRA 8-bit).
constexpr uint32 BytesPerPixel = 4;

// alpha 최대값이다.
constexpr uint32 OpaqueAlpha = 255;

// 대기 한 번의 상한(ms)이다. 깨움이 없어도 이 간격으로 표시를 다시 본다.
constexpr DWORD MaxWaitMilliseconds = 1000;

constexpr uint64 NanosecondsPerMillisecond = 1'000'000;

// 이미지 경로의 UTF-16 버퍼 크기(단위, NUL 포함)다. 긴 경로 접두 없이 MAX_PATH를 넘는 경로는 OS가
// 거부할 수 있다.
constexpr usize MaxSplashPathUnits = 4096;

// 1x·@2x 두 원본이다.
constexpr usize SplashVariantCount = 2;

// 떠 있는 스플래시 창이다(대화상자 hook이 메시지를 보낸다). 없으면 null.
platform::Atomic<HWND> ActiveSplashWindow{nullptr};

// COM 인터페이스 하나를 소유한다.
template <class T>
class ComRef {
public:
    ComRef() noexcept = default;
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;

    ~ComRef() {
        Reset();
    }

    T* Get() const noexcept {
        return pointer_;
    }

    T** Out() noexcept {
        Reset();
        return &pointer_;
    }

    T* operator->() const noexcept {
        return pointer_;
    }

    void Reset() noexcept {
        if (pointer_ != nullptr) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

private:
    T* pointer_ = nullptr;
};

void CopyCallName(char (&out)[MaxSplashCallNameBytes], const char* name) noexcept {
    usize index = 0;
    for (; name[index] != '\0' && index + 1 < sizeof(out); ++index) {
        out[index] = name[index];
    }

    out[index] = '\0';
}

COLORREF ToColorRef(uint32 rgb) noexcept {
    return RGB((rgb >> 16) & 0xFFu, (rgb >> 8) & 0xFFu, rgb & 0xFFu);
}

// 0xRRGGBB를 DIB 픽셀(0xAARRGGBB — 메모리 순서 BGRA)로 바꾼다.
uint32 ToPixel(uint32 rgb) noexcept {
    return (OpaqueAlpha << 24) | (rgb & 0xFFFFFFu);
}

// a 위에 색 c를 amount(0..1)만큼 섞는다.
uint32 BlendPixel(uint32 pixel, uint32 rgb, double amount) noexcept {
    auto channel = [amount](uint32 base, uint32 over) noexcept {
        const double mixed = base + (static_cast<double>(over) - base) * amount;
        return static_cast<uint32>(platform::PlatformMath::Round(mixed)) & 0xFFu;
    };
    const uint32 red = channel((pixel >> 16) & 0xFFu, (rgb >> 16) & 0xFFu);
    const uint32 green = channel((pixel >> 8) & 0xFFu, (rgb >> 8) & 0xFFu);
    const uint32 blue = channel(pixel & 0xFFu, rgb & 0xFFu);
    return (OpaqueAlpha << 24) | (red << 16) | (green << 8) | blue;
}

class Win64SplashPresenter final : public SplashPresenter {
public:
    explicit Win64SplashPresenter(Allocator& allocator) noexcept : allocator_(allocator) {
        wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    ~Win64SplashPresenter() override {
        ReleaseResources();
        if (wakeEvent_ != nullptr) {
            CloseHandle(wakeEvent_);
        }
    }

    bool IsReady() const noexcept {
        return wakeEvent_ != nullptr;
    }

    SplashCreateReport Create(const SplashPresenterSetup& setup) noexcept override {
        SplashCreateReport report;
        auto fail = [&report](SplashSkipReason reason, const char* call, int64 osError) noexcept {
            report.reason = reason;
            report.osError = osError;
            CopyCallName(report.call, call);
            return report;
        };

        // 이 스레드가 만드는 창만 per-monitor v2로 다룬다(프로세스 DPI 모드는 바꾸지 않는다 —
        // 설계 9.2절). 오래된 OS에서 실패하면 시스템 DPI로 그린다.
        (void)SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(com)) {
            return fail(SplashSkipReason::WindowFailed, "CoInitializeEx", com);
        }

        comInitialized_ = true;
        kind_ = setup.kind;
        if (const SplashSkipReason decoded = DecodeImage(setup, report);
            decoded != SplashSkipReason::None) {
            report.reason = decoded;
            return report;
        }

        CreateFonts();
        memoryDc_ = CreateCompatibleDC(nullptr);
        if (memoryDc_ == nullptr) {
            return fail(SplashSkipReason::WindowFailed, "CreateCompatibleDC", GetLastError());
        }

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(layout_.window.width);
        info.bmiHeader.biHeight = -static_cast<LONG>(layout_.window.height); // 위에서 아래로
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        backBitmap_ = CreateDIBSection(memoryDc_, &info, DIB_RGB_COLORS, &backBits_, nullptr, 0);
        if (backBitmap_ == nullptr || backBits_ == nullptr) {
            return fail(SplashSkipReason::WindowFailed, "CreateDIBSection", GetLastError());
        }

        previousBitmap_ = SelectObject(memoryDc_, backBitmap_);
        platform::MemoryCopy(backBits_, base_, PixelBytes());

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &WindowProcedure;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        windowClass.lpszClassName = SplashWindowClassName;
        if (RegisterClassExW(&windowClass) == 0) {
            return fail(SplashSkipReason::WindowFailed, "RegisterClassExW", GetLastError());
        }

        classRegistered_ = true;
        wchar_t title[MaxSplashLineBytes + 1];
        (void)platform::windows::ConvertUtf8ToWide(
            setup.windowTitle.Data(), setup.windowTitle.ByteCount(), title, MaxSplashLineBytes + 1,
            platform::windows::TextFit::Truncate);
        // 주 모니터 작업 영역 가운데에 둔다(UE와 같다). Studio는 작업 표시줄에 보이고(Alt+Tab
        // 대상), 게임은 보이지 않는다.
        const RECT& work = workArea_;
        const LONG width = static_cast<LONG>(layout_.window.width);
        const LONG height = static_cast<LONG>(layout_.window.height);
        const LONG x = work.left + ((work.right - work.left) - width) / 2;
        const LONG y = work.top + ((work.bottom - work.top) - height) / 2;
        const DWORD extendedStyle =
            setup.kind == SplashKind::Studio ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW;
        window_ = CreateWindowExW(extendedStyle, SplashWindowClassName, title, WS_POPUP, x, y,
                                  width, height, nullptr, nullptr, instance, this);
        if (window_ == nullptr) {
            return fail(SplashSkipReason::WindowFailed, "CreateWindowExW", GetLastError());
        }

        const DWORD corner = DwmCornerRound;
        (void)DwmSetWindowAttribute(window_, DwmCornerPreferenceAttribute, &corner, sizeof(corner));
        ActiveSplashWindow.Store<platform::MemoryOrder::Release>(window_);
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        return report;
    }

    void Present(const SplashModel& model) noexcept override {
        if (window_ == nullptr) {
            return;
        }

        GdiFlush();
        platform::MemoryCopy(backBits_, base_, PixelBytes());
        DrawBar(model);
        DrawTexts(model);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void SetSuspended(bool suspended) noexcept override {
        if (window_ != nullptr && !dialogHidden_) {
            ShowWindow(window_, suspended ? SW_HIDE : SW_SHOWNA);
        }
    }

    void Wait(uint64 timeoutNanoseconds) noexcept override {
        uint64 milliseconds =
            (timeoutNanoseconds + NanosecondsPerMillisecond - 1) / NanosecondsPerMillisecond;
        if (milliseconds > MaxWaitMilliseconds) {
            milliseconds = MaxWaitMilliseconds;
        }

        (void)MsgWaitForMultipleObjectsEx(1, &wakeEvent_, static_cast<DWORD>(milliseconds),
                                          QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    void Wake() noexcept override {
        (void)SetEvent(wakeEvent_);
    }

    void Destroy() noexcept override {
        ReleaseResources();
    }

private:
    usize PixelBytes() const noexcept {
        return static_cast<usize>(layout_.window.width) * layout_.window.height * BytesPerPixel;
    }

    // 1x·@2x 크기를 읽어 논리 크기를 정하고, 고른 원본을 창 크기로 맞춰 바탕색 위에 합성한다.
    SplashSkipReason DecodeImage(const SplashPresenterSetup& setup,
                                 SplashCreateReport& report) noexcept {
        auto fail = [&report](SplashSkipReason reason, const char* call, int64 osError) noexcept {
            report.osError = osError;
            CopyCallName(report.call, call);
            return reason;
        };
        ComRef<IWICImagingFactory> factory;
        HRESULT result = CoCreateInstance(WicImagingFactoryClsid, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(factory.Out()));
        if (FAILED(result)) {
            return fail(SplashSkipReason::ImageInvalid, "CoCreateInstance", result);
        }

        ComRef<IWICBitmapFrameDecode> frames[SplashVariantCount];
        SplashSize sizes[SplashVariantCount]{};
        const StringView paths[SplashVariantCount] = {setup.standardPath, setup.doublePath};
        for (usize index = 0; index < SplashVariantCount; ++index) {
            if (paths[index].IsEmpty()) {
                continue;
            }

            if (const SplashSkipReason opened =
                    OpenFrame(*factory.Get(), paths[index], frames[index], sizes[index], report);
                opened != SplashSkipReason::None) {
                return opened;
            }
        }

        auto logical = SplashLogicalSize(sizes[0], sizes[1]);
        if (!logical) {
            return fail(SplashSkipReason::ImageInvalid, "SplashLogicalSize", 0);
        }

        const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!GetMonitorInfoW(monitor, &monitorInfo)) {
            return fail(SplashSkipReason::WindowFailed, "GetMonitorInfoW", GetLastError());
        }

        workArea_ = monitorInfo.rcWork;
        UINT dpiX = static_cast<UINT>(BaseDpi);
        UINT dpiY = static_cast<UINT>(BaseDpi);
        if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
            dpiX = static_cast<UINT>(BaseDpi);
        }

        const double scale = static_cast<double>(dpiX) / BaseDpi;
        const SplashImageVariant variant =
            ChooseSplashVariant(scale, !setup.standardPath.IsEmpty(), !setup.doublePath.IsEmpty());
        const usize chosen = variant == SplashImageVariant::Double ? 1 : 0;
        layout_ = ComputeSplashLayout(logical.Value(), scale);

        ComRef<IWICFormatConverter> converter;
        result = factory->CreateFormatConverter(converter.Out());
        if (SUCCEEDED(result)) {
            result = converter->Initialize(frames[chosen].Get(), WicPixelFormat32bppPbgra,
                                           WICBitmapDitherTypeNone, nullptr, 0.0,
                                           WICBitmapPaletteTypeCustom);
        }

        if (FAILED(result)) {
            return fail(SplashSkipReason::ImageInvalid, "IWICFormatConverter::Initialize", result);
        }

        IWICBitmapSource* source = converter.Get();
        ComRef<IWICBitmapScaler> scaler;
        if (sizes[chosen] != layout_.window) {
            result = factory->CreateBitmapScaler(scaler.Out());
            if (SUCCEEDED(result)) {
                result = scaler->Initialize(converter.Get(), layout_.window.width,
                                            layout_.window.height, WICBitmapInterpolationModeFant);
            }

            if (FAILED(result)) {
                return fail(SplashSkipReason::ImageInvalid, "IWICBitmapScaler::Initialize", result);
            }

            source = scaler.Get();
        }

        auto memory = allocator_.Allocate(PixelBytes(), alignof(uint32));
        if (!memory) {
            return fail(SplashSkipReason::ImageInvalid, "Allocate", 0);
        }

        base_ = static_cast<uint32*>(memory.Value());
        const UINT stride = layout_.window.width * BytesPerPixel;
        result = source->CopyPixels(nullptr, stride, static_cast<UINT>(PixelBytes()),
                                    reinterpret_cast<BYTE*>(base_));
        if (FAILED(result)) {
            return fail(SplashSkipReason::ImageInvalid, "IWICBitmapSource::CopyPixels", result);
        }

        // 미리 곱한 alpha를 바탕색 위에 합성한다: 결과 = 원본 + 바탕 × (1 − alpha).
        const usize count = static_cast<usize>(layout_.window.width) * layout_.window.height;
        for (usize index = 0; index < count; ++index) {
            const uint32 pixel = base_[index];
            const uint32 alpha = pixel >> 24;
            if (alpha == OpaqueAlpha) {
                continue;
            }

            const uint32 rest = OpaqueAlpha - alpha;
            auto channel = [rest](uint32 value, uint32 background) noexcept {
                const uint32 mixed = value + (background * rest + OpaqueAlpha / 2) / OpaqueAlpha;
                return mixed > OpaqueAlpha ? OpaqueAlpha : mixed;
            };
            const uint32 red =
                channel((pixel >> 16) & 0xFFu, (SplashBackgroundColor >> 16) & 0xFFu);
            const uint32 green =
                channel((pixel >> 8) & 0xFFu, (SplashBackgroundColor >> 8) & 0xFFu);
            const uint32 blue = channel(pixel & 0xFFu, SplashBackgroundColor & 0xFFu);
            base_[index] = (OpaqueAlpha << 24) | (red << 16) | (green << 8) | blue;
        }

        return SplashSkipReason::None;
    }

    // 파일 크기 상한을 확인하고 첫 프레임과 크기를 연다.
    static SplashSkipReason OpenFrame(IWICImagingFactory& factory, StringView path,
                                      ComRef<IWICBitmapFrameDecode>& frame, SplashSize& size,
                                      SplashCreateReport& report) noexcept {
        auto fail = [&report](const char* call, int64 osError) noexcept {
            report.osError = osError;
            CopyCallName(report.call, call);
            return SplashSkipReason::ImageInvalid;
        };
        wchar_t widePath[MaxSplashPathUnits];
        if (!platform::windows::ConvertUtf8ToWide(path.Data(), path.ByteCount(), widePath,
                                                  MaxSplashPathUnits,
                                                  platform::windows::TextFit::Exact)) {
            return fail("ConvertUtf8ToWide", 0);
        }

        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(widePath, GetFileExInfoStandard, &attributes)) {
            return fail("GetFileAttributesExW", GetLastError());
        }

        const uint64 bytes =
            (static_cast<uint64>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
        if (bytes > MaxSplashImageBytes) {
            return fail("MaxSplashImageBytes", 0);
        }

        ComRef<IWICBitmapDecoder> decoder;
        HRESULT result = factory.CreateDecoderFromFilename(
            widePath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.Out());
        if (FAILED(result)) {
            return fail("CreateDecoderFromFilename", result);
        }

        result = decoder->GetFrame(0, frame.Out());
        if (FAILED(result)) {
            return fail("IWICBitmapDecoder::GetFrame", result);
        }

        UINT width = 0;
        UINT height = 0;
        result = frame->GetSize(&width, &height);
        if (FAILED(result)) {
            return fail("IWICBitmapFrameDecode::GetSize", result);
        }

        size = {width, height};
        return SplashSkipReason::None;
    }

    void CreateFonts() noexcept {
        for (usize index = 0; index < SplashTextSlotCount; ++index) {
            const SplashTextStyle& style = layout_.style[index];
            fonts_[index] = CreateFontW(
                -style.fontPixels, 0, 0, 0, style.semibold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE,
                FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, SplashFontFace);
        }
    }

    void DrawBar(const SplashModel& model) noexcept {
        const SplashBarMode mode = model.BarMode();
        const SplashRect& bar = layout_.bar;
        if (mode == SplashBarMode::Hidden || bar.right <= bar.left || bar.bottom <= bar.top) {
            return;
        }

        const int32 width = bar.right - bar.left;
        int32 fillStart = 0;
        int32 fillEnd = 0;
        if (mode == SplashBarMode::Determinate) {
            fillEnd = static_cast<int32>(platform::PlatformMath::Round(model.BarValue() * width));
        } else {
            const int32 segment =
                static_cast<int32>(platform::PlatformMath::Round(width * SplashMarqueeWidthRatio));
            const int32 travel = width + segment;
            fillEnd =
                static_cast<int32>(platform::PlatformMath::Round(model.MarqueePhase() * travel));
            fillStart = fillEnd - segment;
        }

        auto* pixels = static_cast<uint32*>(backBits_);
        const uint32 fill = ToPixel(SplashBarFillColor);
        for (int32 y = bar.top; y < bar.bottom; ++y) {
            uint32* row = pixels + static_cast<usize>(y) * layout_.window.width;
            for (int32 x = 0; x < width; ++x) {
                uint32& pixel = row[bar.left + x];
                pixel = (x >= fillStart && x < fillEnd)
                            ? fill
                            : BlendPixel(pixel, SplashBarTrackColor, SplashBarTrackAlpha);
            }
        }
    }

    void DrawTexts(const SplashModel& model) noexcept {
        SetBkMode(memoryDc_, TRANSPARENT);
        wchar_t text[MaxSplashLineBytes + 1];
        for (usize index = 0; index < SplashTextSlotCount; ++index) {
            const auto slot = static_cast<SplashTextSlot>(index);
            const StringView utf8 = model.Text(slot);
            if (!model.IsVisible(slot) || utf8.IsEmpty()) {
                continue;
            }

            const platform::windows::TextConversion converted =
                platform::windows::ConvertUtf8ToWide(utf8.Data(), utf8.ByteCount(), text,
                                                     MaxSplashLineBytes + 1,
                                                     platform::windows::TextFit::Truncate);
            if (!converted) {
                continue;
            }

            const SplashTextStyle& style = layout_.style[index];
            const SplashRect& place = layout_.text[index];
            RECT rect{place.left, place.top, place.right, place.bottom};
            const HGDIOBJ font = fonts_[index] != nullptr ? static_cast<HGDIOBJ>(fonts_[index])
                                                          : GetStockObject(DEFAULT_GUI_FONT);
            const HGDIOBJ previous = SelectObject(memoryDc_, font);
            SetTextColor(memoryDc_, ToColorRef(style.color));
            const UINT format = DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_TOP |
                                (style.align == SplashTextAlign::Right ? DT_RIGHT : DT_LEFT);
            DrawTextW(memoryDc_, text, static_cast<int>(converted.length), &rect, format);
            SelectObject(memoryDc_, previous);
        }

        GdiFlush();
    }

    void ReleaseResources() noexcept {
        if (window_ != nullptr) {
            HWND expected = window_;
            (void)ActiveSplashWindow.CompareExchangeStrong<platform::MemoryOrder::AcquireRelease,
                                                           platform::MemoryOrder::Acquire>(expected,
                                                                                           nullptr);
            DestroyWindow(window_);
            window_ = nullptr;
        }

        if (classRegistered_) {
            UnregisterClassW(SplashWindowClassName, GetModuleHandleW(nullptr));
            classRegistered_ = false;
        }

        if (memoryDc_ != nullptr) {
            if (previousBitmap_ != nullptr) {
                SelectObject(memoryDc_, previousBitmap_);
                previousBitmap_ = nullptr;
            }

            DeleteDC(memoryDc_);
            memoryDc_ = nullptr;
        }

        if (backBitmap_ != nullptr) {
            DeleteObject(backBitmap_);
            backBitmap_ = nullptr;
            backBits_ = nullptr;
        }

        for (HFONT& font : fonts_) {
            if (font != nullptr) {
                DeleteObject(font);
                font = nullptr;
            }
        }

        if (base_ != nullptr) {
            (void)allocator_.Deallocate(base_);
            base_ = nullptr;
        }

        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
    }

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam,
                                            LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return DefWindowProcW(window, message, wParam, lParam);
        }

        auto* self =
            reinterpret_cast<Win64SplashPresenter*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self == nullptr) {
            return DefWindowProcW(window, message, wParam, lParam);
        }

        switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint;
            const HDC target = BeginPaint(window, &paint);
            BitBlt(target, 0, 0, static_cast<int>(self->layout_.window.width),
                   static_cast<int>(self->layout_.window.height), self->memoryDc_, 0, 0, SRCCOPY);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            // Studio 스플래시는 끌어서 옮길 수 있다(UE WITH_EDITOR와 같다).
            if (self->kind_ == SplashKind::Studio) {
                return HTCAPTION;
            }

            return HTCLIENT;
        case WM_CLOSE:
            // 사용자가 닫지 않는다(Alt+F4). 닫기는 Hide만 한다.
            return 0;
        case SplashDialogHideMessage:
            self->dialogHidden_ = true;
            ShowWindow(window, SW_HIDE);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }

    Allocator& allocator_;
    HANDLE wakeEvent_ = nullptr;
    HWND window_ = nullptr;
    HDC memoryDc_ = nullptr;
    HBITMAP backBitmap_ = nullptr;
    HGDIOBJ previousBitmap_ = nullptr;
    void* backBits_ = nullptr;
    uint32* base_ = nullptr;
    HFONT fonts_[SplashTextSlotCount]{};
    SplashLayout layout_{};
    RECT workArea_{};
    SplashKind kind_ = SplashKind::Game;
    bool comInitialized_ = false;
    bool classRegistered_ = false;
    bool dialogHidden_ = false;
};

} // namespace

Result<UniquePtr<SplashPresenter>> CreatePlatformSplashPresenter(Allocator& allocator) noexcept {
    auto created = UniquePtr<Win64SplashPresenter>::TryMake(allocator, allocator);
    if (!created) {
        return Result<UniquePtr<SplashPresenter>>::Failure(created.Error());
    }

    if (!created.Value()->IsReady()) {
        return Result<UniquePtr<SplashPresenter>>::Failure(ErrorCode::DependencyFailed);
    }

    return Result<UniquePtr<SplashPresenter>>::Success(
        UniquePtr<SplashPresenter>(Move(created.Value())));
}

void PostSplashDialogHide() noexcept {
    if (const HWND window = ActiveSplashWindow.Load<platform::MemoryOrder::Acquire>()) {
        (void)PostMessageW(window, SplashDialogHideMessage, 0, 0);
    }
}

} // namespace rocks::splash
