#pragma once

#include "VulkanApi.h"

#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <Graphics/GpuPageAllocator.h>

// 장치 메모리 할당기다(Docs/Design/VulkanGraphics.ko.md 5절). 메모리 종류·자원 모양(선형·최적)마다
// 페이지를 두고 GpuPageAllocator로 구간을 나눈다. 큰 요청과 전용 할당을 요구·선호하는 자원은 전용
// VkDeviceMemory다. 장치 내부 잠금 안에서만 쓴다(스레드 안전하지 않다).
namespace rocks::graphics::vulkan {

// 메모리 용도다.
enum class MemoryUsage : uint8 {
    // 정점·index 버퍼, 텍스처(DEVICE_LOCAL)
    Device,
    // 업로드 ring·staging(HOST_VISIBLE | HOST_COHERENT, 계속 map)
    Upload,
    // readback(HOST_VISIBLE | HOST_CACHED 우선, 없으면 COHERENT, 계속 map)
    Readback
};

// 자원 모양이다. 선형(버퍼)과 최적(이미지)을 같은 페이지에 섞지 않아 bufferImageGranularity를
// 따로 맞추지 않는다.
enum class ResourceShape : uint8 {
    Linear,
    Optimal
};

// 할당 요청이다(vkGet*MemoryRequirements2 결과). dedicated면 buffer·image 가운데 하나를 준다.
struct MemoryRequest {
    uint64 size = 0;
    uint64 alignment = 1;
    uint32 typeBits = 0;
    bool dedicated = false;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
};

// 잡은 메모리 구간이다. mapped는 호스트 가시 메모리일 때 offset 위치의 주소다.
struct MemoryAllocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64 offset = 0;
    uint64 size = 0;
    uint8* mapped = nullptr;
    // HOST_COHERENT가 아니면 읽기 전에 Invalidate가 필요하다.
    bool coherent = true;
    // 페이지 번호(전용 할당이면 DedicatedPage)와 페이지 안 구간이다.
    uint32 page = 0;
    GpuPageAllocation block;

    bool IsNull() const noexcept {
        return memory == VK_NULL_HANDLE;
    }
};

// 전용 할당 표지다.
inline constexpr uint32 DedicatedPage = NumericLimits<uint32>::Max();

class MemoryManager {
public:
    explicit MemoryManager(Allocator& allocator) noexcept;
    ~MemoryManager() noexcept;

    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    void Initialize(const Functions& functions, VkDevice device,
                    const VkPhysicalDeviceMemoryProperties& properties,
                    uint64 nonCoherentAtomSize) noexcept;

    // 요청을 만족하는 구간을 잡는다. 실패: 맞는 메모리 종류가 없으면 Unsupported, 메모리 부족
    // OutOfMemory, 그 밖의 Vulkan 실패 DependencyFailed. failure에 원본 VkResult를 둔다.
    Result<MemoryAllocation> Allocate(const MemoryRequest& request, MemoryUsage usage,
                                      ResourceShape shape, VkResult& failure) noexcept;

    // 구간을 돌려준다(GPU가 다 쓴 뒤). 빈 페이지는 같은 묶음에 하나만 남기고 놓는다.
    void Free(const MemoryAllocation& allocation) noexcept;

    // coherent가 아닌 호스트 메모리의 CPU 읽기 전에 부른다.
    void Invalidate(const MemoryAllocation& allocation) noexcept;

    // 모든 페이지를 놓는다(장치 파괴 직전, GPU idle 뒤).
    void Clear() noexcept;

    // 할당한 VkDeviceMemory 수다(진단).
    usize DeviceMemoryCount() const noexcept;

private:
    struct Page {
        explicit Page(Allocator& allocator) noexcept : ledger(allocator) {
        }

        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint32 type = 0;
        ResourceShape shape = ResourceShape::Linear;
        uint8* mapped = nullptr;
        GpuPageAllocator ledger;
    };

    // usage에 맞는 메모리 종류다. 없으면 NoMemoryType이다.
    uint32 PickType(uint32 typeBits, MemoryUsage usage) const noexcept;

    uint64 PageBytes(uint32 type, MemoryUsage usage) const noexcept;

    Result<MemoryAllocation> AllocateDedicated(const MemoryRequest& request, uint32 type,
                                               VkResult& failure) noexcept;

    static constexpr uint32 NoMemoryType = NumericLimits<uint32>::Max();

    Allocator& allocator_;
    const Functions* functions_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties properties_ = {};
    uint64 atom_ = 1;
    // 빈 칸은 null이다(페이지 번호를 바꾸지 않는다).
    Array<UniquePtr<Page>> pages_;
    usize dedicatedCount_ = 0;
};

} // namespace rocks::graphics::vulkan
