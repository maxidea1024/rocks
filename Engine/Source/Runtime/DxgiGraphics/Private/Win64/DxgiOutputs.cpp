#include <DxgiGraphics/DxgiAdapter.h>

#include <Platform/Win64/ComRef.h>
#include <Platform/Win64/WideString.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>
#include <dxgi1_6.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

namespace rocks::graphics::dxgi {

using platform::windows::ComRef;

namespace {

// DXGI_ADAPTER_DESC1::Description의 UTF-16 단위 수다.
constexpr usize AdapterDescriptionUnits =
    sizeof(DXGI_ADAPTER_DESC1::Description) / sizeof(DXGI_ADAPTER_DESC1::Description[0]);
// 그 이름의 UTF-8 최대 byte 수 + NUL이다. UTF-16 한 단위는 UTF-8로 최대 3 byte다(surrogate 쌍은 두
// 단위가 4 byte). 따라서 이 칸에서는 잘리지 않는다.
constexpr usize AdapterNameBytes = AdapterDescriptionUnits * 3 + 1;

} // namespace

bool AdapterHasHdrOutput(IDXGIAdapter* adapter) noexcept {
    if (adapter == nullptr) {
        return false;
    }

    for (UINT index = 0;; ++index) {
        ComRef<IDXGIOutput> output;
        if (adapter->EnumOutputs(index, output.Out()) != S_OK) {
            return false;
        }

        ComRef<IDXGIOutput6> output6;
        DXGI_OUTPUT_DESC1 desc = {};
        if (SUCCEEDED(output.TryQueryAs(output6)) && SUCCEEDED(output6->GetDesc1(&desc)) &&
            desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
            return true;
        }
    }
}

Result<void> AssignAdapterName(IDXGIAdapter1* adapter, String& name) noexcept {
    DXGI_ADAPTER_DESC1 desc = {};
    if (adapter == nullptr || FAILED(adapter->GetDesc1(&desc))) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    char text[AdapterNameBytes] = {};
    const platform::windows::TextConversion converted = platform::windows::ConvertWideToUtf8(
        desc.Description, platform::windows::WideLength(desc.Description), text, AdapterNameBytes,
        platform::windows::TextFit::Exact);
    if (!converted) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return name.TryAssign(StringView(text, converted.length));
}

} // namespace rocks::graphics::dxgi
