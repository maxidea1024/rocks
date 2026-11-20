#include "SpikeTools.h"

#include <Platform/Win64/ComRef.h>
#include <Platform/Win64/WideString.h>

#include <stdio.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

// 측정 S2(Docs/Design/ShaderSystem.ko.md 5.4절): 이 기계의 하드웨어 어댑터와 WARP가 OS D3D12 런타임
// (Agility SDK 없음)에서 지원하는 최대 셰이더 모델이다. 결정 3(D3D12 셰이더 모델)의 측정 근거다.
namespace rocks::spike {

using platform::windows::ComRef;

namespace {

// D3D12_FEATURE_SHADER_MODEL로 물어볼 가장 높은 값이다. Windows SDK 10.0.22621 헤더는 6.7까지
// 이름이 있고, 6.8(0x68)·6.9(0x69)는 Agility SDK 헤더(D3D_SHADER_MODEL_6_8·6_9)가 정한 값이다.
// 런타임이 모르는 값은 E_INVALIDARG이므로 위에서부터 내려가며 묻는다.
constexpr uint32 HighestQueriedShaderModel = 0x69;
constexpr uint32 LowestShaderModel = 0x60;
constexpr uint32 ShaderModelMinorMask = 0x0f;
constexpr uint32 ShaderModelMajorShift = 4;
// D3D_FEATURE_LEVEL 값은 0xMm00(예: 12_1 = 0xc100)이다.
constexpr unsigned FeatureLevelMajorShift = 12;
constexpr unsigned FeatureLevelMinorShift = 8;
constexpr unsigned FeatureLevelMinorMask = 0xf;
// 어댑터 이름 UTF-8 버퍼 크기다(DXGI_ADAPTER_DESC1::Description은 128 UTF-16 code unit, UTF-8로
// 최대 384 byte).
constexpr usize AdapterNameBytes = 512;

void WriteShaderModel(StringView key, uint32 model, Report& report) noexcept {
    char text[16];
    snprintf(text, sizeof(text), "%u.%u", model >> ShaderModelMajorShift,
             model & ShaderModelMinorMask);
    report.String(key, StringView(text));
}

void ProbeAdapter(IDXGIAdapter1* adapter, const char* kind, Report& report) noexcept {
    DXGI_ADAPTER_DESC1 desc = {};
    if (FAILED(adapter->GetDesc1(&desc))) {
        return;
    }

    char name[AdapterNameBytes] = {};
    (void)platform::windows::ConvertWideToUtf8(
        desc.Description, platform::windows::WideLength(desc.Description), name, AdapterNameBytes,
        platform::windows::TextFit::Truncate);
    report.BeginObject();
    report.String("kind", StringView(kind));
    report.String("name", StringView(name));
    report.UInt("vendorId", desc.VendorId);
    report.UInt("deviceId", desc.DeviceId);

    // UMD 버전은 IDXGIDevice 지원 조회의 LARGE_INTEGER로 온다(a.b.c.d).
    LARGE_INTEGER umd = {};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
        char version[48];
        snprintf(version, sizeof(version), "%u.%u.%u.%u",
                 static_cast<unsigned>(HIWORD(static_cast<DWORD>(umd.HighPart))),
                 static_cast<unsigned>(LOWORD(static_cast<DWORD>(umd.HighPart))),
                 static_cast<unsigned>(HIWORD(umd.LowPart)),
                 static_cast<unsigned>(LOWORD(umd.LowPart)));
        report.String("driverVersion", StringView(version));
    }

    ComRef<ID3D12Device> device;
    const HRESULT created =
        D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.Out()));
    report.Bool("deviceCreated", SUCCEEDED(created));
    if (FAILED(created)) {
        report.UInt("deviceError", static_cast<uint32>(created));
        report.EndObject();
        return;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1,
                                        D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1,
                                        D3D_FEATURE_LEVEL_12_2};
    D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels = {};
    featureLevels.NumFeatureLevels = static_cast<UINT>(sizeof(levels) / sizeof(levels[0]));
    featureLevels.pFeatureLevelsRequested = levels;
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureLevels,
                                              sizeof(featureLevels)))) {
        char level[16];
        const auto value = static_cast<unsigned>(featureLevels.MaxSupportedFeatureLevel);
        snprintf(level, sizeof(level), "%u.%u", value >> FeatureLevelMajorShift,
                 (value >> FeatureLevelMinorShift) & FeatureLevelMinorMask);
        report.String("maxFeatureLevel", StringView(level));
    }

    // 런타임이 아는 가장 높은 요청 값(첫 성공)과 장치가 돌려준 최대 셰이더 모델을 함께 남긴다.
    for (uint32 requested = HighestQueriedShaderModel; requested >= LowestShaderModel;
         --requested) {
        D3D12_FEATURE_DATA_SHADER_MODEL model = {};
        model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(requested);
        if (SUCCEEDED(
                device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)))) {
            WriteShaderModel("highestKnownRequest", requested, report);
            WriteShaderModel("highestShaderModel", static_cast<uint32>(model.HighestShaderModel),
                             report);
            break;
        }
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    if (SUCCEEDED(
            device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))) {
        report.UInt("resourceBindingTier", static_cast<uint64>(options.ResourceBindingTier));
    }

    report.EndObject();
}

} // namespace

void ProbeD3D12ShaderModel(Report& report) noexcept {
    report.BeginObject("d3d12");
    // 실행 파일이 D3D12SDKVersion을 내보내지 않으므로 OS에 들어 있는 D3D12 런타임을 쓴다.
    report.Bool("agilitySdk", false);
    ComRef<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.Out())))) {
        report.String("error", StringView("CreateDXGIFactory2 failed"));
        report.EndObject();
        return;
    }

    report.BeginArray("adapters");
    ComRef<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, adapter.Out()) != DXGI_ERROR_NOT_FOUND;
         ++index) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            ProbeAdapter(adapter.Get(), "hardware", report);
        }
    }

    if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.Out())))) {
        ProbeAdapter(adapter.Get(), "warp", report);
    }

    report.EndArray();
    report.EndObject();
}

} // namespace rocks::spike
