#include <Core/ScopeExit.h>
#include <PlatformWindow/Window.h>
#include <Platform/Win64/WindowsHeadersBegin.h>
#include <Windows.h>
#include <Platform/Win64/WindowsHeadersEnd.h>

namespace rocks::platform {

namespace {

class Win64DragImage final : public DragImage {
public:
    ~Win64DragImage() override {
        if (window_) {
            DestroyWindow(window_);
        }
    }

    bool Initialize(HWND owner, IntVec2 position, IntVec2 size, Span<const uint8> rgba,
                    uint8 opacity) noexcept {
        // 레이어 창은 뒤쪽 창의 도킹 판정을 가리지 않고 원본 창의 마우스 캡처도 유지한다.
        constexpr DWORD flags =
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        window_ =
            CreateWindowExW(flags, L"STATIC", nullptr, WS_POPUP, position.x, position.y, size.x,
                            size.y, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window_) {
            return false;
        }
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) {
            return false;
        }
        ROCKS_ON_SCOPE_EXIT {
            DeleteDC(dc);
        };
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = size.x;
        info.bmiHeader.biHeight = -size.y;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) {
            return false;
        }
        ROCKS_ON_SCOPE_EXIT {
            DeleteObject(bitmap);
        };
        auto* pixels = static_cast<uint8*>(bits);
        for (usize i = 0; i < rgba.Size(); i += 4) {
            pixels[i] = rgba[i + 2];
            pixels[i + 1] = rgba[i + 1];
            pixels[i + 2] = rgba[i];
            pixels[i + 3] = 255;
        }
        auto previous = SelectObject(dc, bitmap);
        if (!previous || previous == HGDI_ERROR) {
            return false;
        }
        ROCKS_ON_SCOPE_EXIT {
            SelectObject(dc, previous);
        };
        POINT destination{position.x, position.y}, source{};
        SIZE dimensions{size.x, size.y};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, opacity, 0};
        return UpdateLayeredWindow(window_, nullptr, &destination, &dimensions, dc, &source, 0,
                                   &blend, ULW_ALPHA) &&
               SetWindowPos(window_, HWND_TOPMOST, position.x, position.y, 0, 0,
                            SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void MoveTo(IntVec2 position) noexcept override {
        SetWindowPos(window_, nullptr, position.x, position.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

private:
    HWND window_ = nullptr;
};

} // namespace

Result<UniquePtr<DragImage>> DragImage::Create(Allocator& allocator, const Window& owner,
                                               IntVec2 position, IntVec2 size,
                                               Span<const uint8> rgba, uint8 opacity) noexcept {
    if (size.x <= 0 || size.y <= 0 || uint64(size.x) * uint64(size.y) * 4 != rgba.Size()) {
        return Result<UniquePtr<DragImage>>::Failure(ErrorCode::InvalidArgument);
    }
    auto image = UniquePtr<Win64DragImage>::TryMake(allocator);
    if (!image) {
        return Result<UniquePtr<DragImage>>::Failure(image.Error());
    }
    if (!image.Value()->Initialize(static_cast<HWND>(owner.NativeHandle().window), position, size,
                                   rgba, opacity)) {
        return Result<UniquePtr<DragImage>>::Failure(ErrorCode::DependencyFailed);
    }
    return Result<UniquePtr<DragImage>>::Success(UniquePtr<DragImage>(Move(image.Value())));
}

} // namespace rocks::platform
