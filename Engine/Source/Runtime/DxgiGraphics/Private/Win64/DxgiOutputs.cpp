#include <DxgiGraphics/DxgiAdapter.h>

#include <Windows.h>
#include <dxgi1_6.h>

namespace rocks::graphics::dxgi {

namespace {

// COM 참조 하나를 범위 끝에서 놓는다(이 파일 안에서만 쓴다).
template <class T>
class ScopedCom {
public:
    ScopedCom() noexcept = default;
    ScopedCom(const ScopedCom&) = delete;
    ScopedCom& operator=(const ScopedCom&) = delete;

    ~ScopedCom() {
        if (pointer_ != nullptr) {
            pointer_->Release();
        }
    }

    T** Out() noexcept {
        return &pointer_;
    }

    T* operator->() const noexcept {
        return pointer_;
    }

private:
    T* pointer_ = nullptr;
};

} // namespace

bool AdapterHasHdrOutput(IDXGIAdapter* adapter) noexcept {
    if (adapter == nullptr) {
        return false;
    }

    for (UINT index = 0;; ++index) {
        ScopedCom<IDXGIOutput> output;
        if (adapter->EnumOutputs(index, output.Out()) != S_OK) {
            return false;
        }

        ScopedCom<IDXGIOutput6> output6;
        DXGI_OUTPUT_DESC1 desc = {};
        if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(output6.Out()))) &&
            SUCCEEDED(output6->GetDesc1(&desc)) &&
            desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
            return true;
        }
    }
}

} // namespace rocks::graphics::dxgi
