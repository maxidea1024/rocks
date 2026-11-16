#include <Graphics/DeviceTypes.h>
#include <Platform/Atomics.h>

namespace rocks::graphics {

namespace {

// 마지막으로 나눠 준 장치 incarnation 번호다.
uint64 lastDeviceEpoch = 0;

} // namespace

uint64 NewDeviceEpoch() noexcept {
    return platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&lastDeviceEpoch, 1) + 1;
}

const char* BackendName(Backend backend) noexcept {
    switch (backend) {
    case Backend::D3D12:
        return "D3D12";
    case Backend::OpenGL:
        return "OpenGL";
    case Backend::Vulkan:
        return "Vulkan";
    case Backend::Metal:
        return "Metal";
    }

    return "Unknown";
}

Result<void> ValidateVertexLayout(const VertexLayout& layout) noexcept {
    if (layout.attributes.Size() > MaxVertexAttributes ||
        layout.streams.Size() > MaxVertexStreams) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (const VertexStream& stream : layout.streams) {
        if (stream.stride == 0 || stream.stride > MaxVertexStride ||
            stream.stride % VertexLayoutAlignment != 0 ||
            (stream.stepRate != VertexStepRate::Vertex &&
             stream.stepRate != VertexStepRate::Instance)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    uint32 locations = 0;
    uint32 usedStreams = 0;
    for (const VertexAttribute& attribute : layout.attributes) {
        const uint32 size = VertexFormatSize(attribute.format);
        if (attribute.location >= MaxVertexAttributes || size == 0 ||
            attribute.stream >= layout.streams.Size() ||
            attribute.offset > MaxVertexAttributeOffset ||
            attribute.offset % VertexLayoutAlignment != 0 ||
            (locations & (1u << attribute.location)) != 0 ||
            attribute.offset + size > layout.streams[attribute.stream].stride) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        locations |= 1u << attribute.location;
        usedStreams |= 1u << attribute.stream;
    }

    // 선언한 stream마다 속성이 하나 이상 있어야 한다.
    const uint32 declared = layout.streams.Size() == 0 ? 0u : (1u << layout.streams.Size()) - 1u;
    if (usedStreams != declared) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

} // namespace rocks::graphics
