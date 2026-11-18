#pragma once

#include "../../RenderBase/Tests/RecordingDevice.h"

#include <Core/Memory/FrameArena.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringView.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <RenderGraph/RenderGraph.h>

// 렌더 그래프 시험 공용 준비물이다. 기록 장치(RenderBase/Tests/RecordingDevice) 위에 handle 표·풀·
// 프레임 arena를 두고, pass 실행 순서를 기록한다.
namespace rocks::render::testing {

// pass 실행 순서 기록이다(시험 하나가 쓰고 Reset으로 비운다).
struct PassLog {
    static constexpr uint32 Capacity = 64;
    uint32 values[Capacity] = {};
    uint32 count = 0;

    void Add(uint32 value) noexcept {
        if (count == Capacity) {
            platform::Fatal("render graph test pass log overflow");
        }

        values[count++] = value;
    }

    void Reset() noexcept {
        count = 0;
    }

    bool Equals(Span<const uint32> expected) const noexcept {
        if (expected.Size() != count) {
            return false;
        }

        for (uint32 index = 0; index < count; ++index) {
            if (values[index] != expected[index]) {
                return false;
            }
        }

        return true;
    }
};

// 시험 하나의 장치·표·풀·arena 묶음이다.
struct GraphBench {
    SystemAllocator allocator;
    RecordingDevice device{allocator};
    RenderHandleTable handles{allocator};
    GraphResourcePool pool;
    FrameArena arena{allocator};
    uint64 frame = 0;

    explicit GraphBench(const GraphResourcePoolConfig& config = {}) noexcept
        : pool(allocator, device, config) {
    }

    // 다음 프레임 그래프 설정이다. 시험 안의 그래프들이 arena를 함께 쓰므로 비우지 않는다(bench가
    // 끝날 때 함께 사라진다).
    RenderGraphDesc Next(GraphMode mode = GraphMode::Deferred, GpuTrace* trace = nullptr) noexcept {
        RenderGraphDesc desc;
        desc.device = &device;
        desc.handles = &handles;
        desc.pool = &pool;
        desc.arena = &arena;
        desc.trace = trace;
        desc.frame = ++frame;
        desc.mode = mode;
        return desc;
    }

    // 그래프 밖 렌더 타깃 텍스처를 만들어 렌더 handle에 잇는다(import 원본).
    RenderHandle ExternalTarget(uint32 size = 4) noexcept {
        graphics::TextureDesc desc;
        desc.width = size;
        desc.height = size;
        desc.usage = graphics::TextureUsage::RenderTarget;
        auto created = device.CreateTexture(desc, {});
        auto reserved = handles.Reserve(RenderResourceKind::Texture);
        if (!created || !reserved || !handles.BindTexture(reserved.Value(), created.Value())) {
            platform::Fatal("render graph test external target creation failed");
        }

        return reserved.Value();
    }

    graphics::TextureHandle DeviceTexture(RenderHandle handle) const noexcept {
        auto texture = handles.Texture(handle);
        return texture ? texture.Value() : graphics::TextureHandle{};
    }

    // 기록한 장치 호출 가운데 op의 수다.
    usize Count(RecordingDevice::Op op) const noexcept {
        usize count = 0;
        for (const RecordingDevice::Call& call : device.Calls()) {
            count += call.op == op ? 1u : 0u;
        }

        return count;
    }

    // 기록한 장치 호출 가운데 nth번째(0부터) op의 위치다. 없으면 NumericLimits<usize>::Max()다.
    usize IndexOf(RecordingDevice::Op op, usize nth = 0) const noexcept {
        Span<const RecordingDevice::Call> calls = device.Calls();
        for (usize index = 0; index < calls.Size(); ++index) {
            if (calls[index].op != op) {
                continue;
            }

            if (nth == 0) {
                return index;
            }

            --nth;
        }

        return NumericLimits<usize>::Max();
    }

    // handle을 Retire하고 ReleaseRetiredResource로 넘긴다(렌더 lane 없는 시험의 Retire 경로).
    // lease가 남았으면 미룬다(deferred를 돌려준다).
    bool RetireHandle(RenderHandle handle) noexcept {
        auto retired = handles.BeginRetire(handle);
        if (!retired) {
            platform::Fatal("render graph test retire failed");
        }

        if (!retired.Value().deferred) {
            ReleaseRetiredResource(device, handle, retired.Value());
        }

        return retired.Value().deferred;
    }
};

// text가 part를 포함하는지다(오류 문구 확인).
inline bool ContainsText(StringView text, const char* part) noexcept {
    const StringView needle(part);
    if (needle.ByteCount() > text.ByteCount()) {
        return false;
    }

    for (usize offset = 0; offset + needle.ByteCount() <= text.ByteCount(); ++offset) {
        if (platform::MemoryEquals(text.Data() + offset, needle.Data(), needle.ByteCount())) {
            return true;
        }
    }

    return false;
}

inline graphics::BufferDesc BufferOf(uint64 size) noexcept {
    graphics::BufferDesc desc;
    desc.size = size;
    return desc;
}

inline graphics::TextureDesc TargetDesc(uint32 size = 4, uint32 mips = 1) noexcept {
    graphics::TextureDesc desc;
    desc.width = size;
    desc.height = size;
    desc.mipLevels = mips;
    desc.usage = graphics::TextureUsage::RenderTarget;
    return desc;
}

} // namespace rocks::render::testing
