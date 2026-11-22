#pragma once

#include <Config/DeviceProfileSettings.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <Core/Text/String.h>

// 기기 특성 값이다(Docs/Design/ConfigSystem.ko.md 6절 — 설정 CF4). 기기 프로필 선택 규칙·메모리
// 등급과 품질 조합 검증의 입력이다. Config 모듈은 Graphics를 모른다: 엔진·호스트가 Graphics
// DeviceCaps (RenderBase 4.4절 GX1)와 플랫폼 메모리 정보(platform::QueryPhysicalMemory)를 이름 →
// 값으로 옮겨 넣는다. 이름은 소문자 점 경로(마디 하나 이상)이고 표준 이름은
// DeviceTraitNames다(호스트는 다른 이름을 더해도 된다). UE 대응: Android 기기 프로필 선택기의
// `SrcGpuFamily`·`DeviceMake` 등 비교 입력과 `FPlatformMemory` 메모리 등급 입력.
//
// 사용 예(호스트):
//   DeviceTraits traits(allocator);
//   traits.SetText(DeviceTraitNames::Platform, "Windows");
//   traits.SetNumber(DeviceTraitNames::SystemMemoryMiB, 16384);
//   traits.SetNumber(DeviceTraitNames::GpuTextureSize2D, caps.limits.textureSize2D);
namespace rocks::config {

// 표준 특성 이름이다.
namespace DeviceTraitNames {

// 플랫폼 이름(문자열 — `Windows`·`Mac`·`Android`).
inline constexpr StringView Platform = "platform";
// 시스템(물리) 메모리 MiB(수).
inline constexpr StringView SystemMemoryMiB = "memory.system_mib";
// GPU 이름(문자열 — 어댑터 설명).
inline constexpr StringView GpuName = "gpu.name";
// GPU 벤더 ID(수 — PCI vendor).
inline constexpr StringView GpuVendorId = "gpu.vendor_id";
// GPU 전용 메모리 MiB(수).
inline constexpr StringView GpuMemoryMiB = "gpu.memory_mib";
// 2D 텍스처 한 변 최대(수 — DeviceLimits::textureSize2D).
inline constexpr StringView GpuTextureSize2D = "gpu.texture_size_2d";
// color attachment 수(수 — DeviceLimits::colorAttachments).
inline constexpr StringView GpuColorAttachments = "gpu.color_attachments";
// 픽셀 단계 SRV 텍스처 수(수 — DeviceLimits::pixel.sampledTextures).
inline constexpr StringView GpuPixelSampledTextures = "gpu.pixel_sampled_textures";
// 샘플러 최대 이방성(수 — DeviceLimits::samplerAnisotropy).
inline constexpr StringView GpuSamplerAnisotropy = "gpu.sampler_anisotropy";
// compute 셰이더(수 0·1 — DeviceFeatures::computeShaders).
inline constexpr StringView GpuComputeShaders = "gpu.compute_shaders";
// 셰이더 모델(수 — DeviceFeatures::shaderModel, D3D만).
inline constexpr StringView GpuShaderModel = "gpu.shader_model";

} // namespace DeviceTraitNames

// 특성 하나다. numeric이면 number, 아니면 text가 값이다.
struct DeviceTrait {
    StringView name;
    StringView text;
    double number = 0;
    bool numeric = false;
};

// 기기 특성 목록이다(이름 하나에 값 하나 — 같은 이름을 다시 쓰면 바꾼다). 복사하지 않는다.
// 스레드: 한 스레드에서 채운 뒤 읽기만 한다.
class ROCKS_CONFIG_API DeviceTraits {
public:
    explicit DeviceTraits(Allocator& allocator) noexcept;

    DeviceTraits(const DeviceTraits&) = delete;
    DeviceTraits& operator=(const DeviceTraits&) = delete;

    // 수 특성을 쓴다. 오류: 이름이 소문자 점 경로(마디 하나 이상)가 아니거나 값이 유한하지 않으면
    // InvalidArgument, 할당 실패.
    Result<void> SetNumber(StringView name, double value) noexcept;

    // 문자열 특성을 쓴다. 오류: 이름 문법 InvalidArgument, 할당 실패.
    Result<void> SetText(StringView name, StringView value) noexcept;

    // 이름의 특성을 out에 쓴다(없으면 false — out은 그대로). view는 다음 Set까지 유효하다.
    bool Find(StringView name, DeviceTrait& out) const noexcept;

    usize Count() const noexcept {
        return entries_.Size();
    }

    // index번째(넣은 순서) 특성이다. 전제: index < Count() — 아니면 Fatal.
    DeviceTrait At(usize index) const noexcept;

private:
    struct Entry {
        explicit Entry(Allocator& allocator) noexcept : name(allocator), text(allocator) {
        }

        String name;
        String text;
        double number = 0;
        bool numeric = false;
    };

    Result<Entry*> Slot(StringView name) noexcept;

    Allocator& allocator_;
    Array<UniquePtr<Entry>> entries_;
};

// condition이 traits에서 참인지다. 거짓이면 reason에 사유(영어 문장 — 진단·로그용)를 쓴다.
// 오류: 할당 실패.
ROCKS_CONFIG_API Result<bool> MatchDeviceCondition(const DeviceTraits& traits,
                                                   const DeviceCondition& condition,
                                                   String& reason) noexcept;

// conditions가 모두 참인지다(비면 참). 거짓이면 첫 거짓 조건의 사유를 reason에 쓴다.
Result<bool> MatchDeviceConditions(const DeviceTraits& traits,
                                   Span<const DeviceCondition> conditions, String& reason) noexcept;

} // namespace rocks::config
