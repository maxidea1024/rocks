#include <Graphics/DeviceTypes.h>

// 버퍼 설정·view의 장치 무관 규칙이다(RenderBase 4.4절 GX2).
namespace rocks::graphics {

namespace {

constexpr BufferUsage KnownBufferUsage =
    BufferUsage::Vertex | BufferUsage::Index | BufferUsage::Constant | BufferUsage::Indirect |
    BufferUsage::ShaderRead | BufferUsage::ShaderWrite | BufferUsage::Structured | BufferUsage::Raw;

constexpr BufferUsage ShaderAccessUsage = BufferUsage::ShaderRead | BufferUsage::ShaderWrite;

Result<void> Invalid() noexcept {
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

} // namespace

Result<void> ValidateBufferDesc(const BufferDesc& desc) noexcept {
    const BufferUsage usage = desc.usage;
    if (desc.size == 0 || usage == BufferUsage::None || (usage & KnownBufferUsage) != usage) {
        return Invalid();
    }

    if (HasAnyBufferUsage(usage, BufferUsage::Constant) &&
        (usage != BufferUsage::Constant || desc.size % 16 != 0)) {
        return Invalid();
    }

    const bool structured = HasAnyBufferUsage(usage, BufferUsage::Structured);
    const bool raw = HasAnyBufferUsage(usage, BufferUsage::Raw);
    if (structured && raw) {
        return Invalid();
    }

    if (structured) {
        if (desc.stride == 0 || desc.stride % 4 != 0 || desc.stride > MaxStructuredStride ||
            desc.size % desc.stride != 0 || !HasAnyBufferUsage(usage, ShaderAccessUsage) ||
            HasAnyBufferUsage(usage,
                              BufferUsage::Vertex | BufferUsage::Index | BufferUsage::Indirect)) {
            return Invalid();
        }
    } else if (desc.stride != 0) {
        return Invalid();
    }

    if (raw && (!HasAnyBufferUsage(usage, ShaderAccessUsage) || desc.size % 4 != 0)) {
        return Invalid();
    }

    if (HasAnyBufferUsage(usage, BufferUsage::Indirect) && desc.size % 4 != 0) {
        return Invalid();
    }

    return Result<void>::Success();
}

Result<ResolvedBufferView> ResolveBufferView(const BufferDesc& buffer,
                                             const BufferViewDesc& view) noexcept {
    using Resolved = Result<ResolvedBufferView>;
    const BufferUsage required =
        view.access == ViewAccess::Read ? BufferUsage::ShaderRead : BufferUsage::ShaderWrite;
    if ((view.access != ViewAccess::Read && view.access != ViewAccess::ReadWrite) ||
        !HasAnyBufferUsage(buffer.usage, required)) {
        return Resolved::Failure(ErrorCode::InvalidArgument);
    }

    const bool structured = HasAnyBufferUsage(buffer.usage, BufferUsage::Structured);
    uint32 elementBytes = 0;
    switch (view.kind) {
    case BufferViewKind::Typed: {
        const PixelFormatInfo& info = GetPixelFormatInfo(view.format);
        if (structured || !IsValidPixelFormat(view.format) || info.kind != PixelFormatKind::Color ||
            info.planeCount != 1 || info.blockBytes == 0) {
            return Resolved::Failure(ErrorCode::InvalidArgument);
        }

        elementBytes = info.blockBytes;
        break;
    }

    case BufferViewKind::Structured:
        if (!structured || view.format != PixelFormat::Unknown) {
            return Resolved::Failure(ErrorCode::InvalidArgument);
        }

        elementBytes = buffer.stride;
        break;

    case BufferViewKind::Raw:
        if (!HasAnyBufferUsage(buffer.usage, BufferUsage::Raw) ||
            view.format != PixelFormat::Unknown || view.offset % RawViewOffsetAlignment != 0) {
            return Resolved::Failure(ErrorCode::InvalidArgument);
        }

        elementBytes = 4;
        break;
    default:
        return Resolved::Failure(ErrorCode::InvalidArgument);
    }

    if (view.offset % elementBytes != 0 || view.offset >= buffer.size ||
        (view.elementCount == 0 && view.offset != 0)) {
        return Resolved::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 available = (buffer.size - view.offset) / elementBytes;
    const uint64 count = view.elementCount == 0 ? available : view.elementCount;
    if (count == 0 || count > available) {
        return Resolved::Failure(ErrorCode::InvalidArgument);
    }

    ResolvedBufferView resolved;
    resolved.offset = view.offset;
    resolved.elementCount = count;
    resolved.elementBytes = elementBytes;
    return Resolved::Success(resolved);
}

Result<void> CheckBufferViewLimits(const DeviceLimits& limits, PixelFormatCaps formatCaps,
                                   const BufferViewDesc& view,
                                   const ResolvedBufferView& resolved) noexcept {
    if (view.kind == BufferViewKind::Typed) {
        const uint32 alignment =
            limits.texelBufferOffsetAlignment == 0 ? 1 : limits.texelBufferOffsetAlignment;
        if (resolved.offset % alignment != 0) {
            return Invalid();
        }

        const PixelFormatCaps required = view.access == ViewAccess::Read
                                             ? PixelFormatCaps::TexelBufferRead
                                             : PixelFormatCaps::TexelBufferWrite;
        if (!HasPixelFormatCaps(formatCaps, required) ||
            resolved.elementCount > limits.texelBufferElements) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        return Result<void>::Success();
    }

    const uint32 alignment =
        limits.storageBufferOffsetAlignment == 0 ? 1 : limits.storageBufferOffsetAlignment;
    if (resolved.offset % alignment != 0) {
        return Invalid();
    }

    if (resolved.Bytes() > limits.storageBufferBytes) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    return Result<void>::Success();
}

bool AddDrawBufferSlot(DrawBufferSlots& slots, uint32 slot, BufferViewKind kind,
                       PixelShaderType type, uint32 stride) noexcept {
    if (slot >= MaxDrawBufferViews) {
        return false;
    }

    const uint32 bit = 1u << slot;
    if ((slots.usedMask & bit) != 0) {
        const uint32 known = slots.structureStrides[slot];
        if (known == 0) {
            slots.structureStrides[slot] = stride;
        }

        return slots.kinds[slot] == kind && slots.typedShaderTypes[slot] == type &&
               (known == 0 || stride == 0 || known == stride);
    }

    slots.usedMask |= bit;
    slots.kinds[slot] = kind;
    slots.typedShaderTypes[slot] = type;
    slots.structureStrides[slot] = kind == BufferViewKind::Structured ? stride : 0u;
    return true;
}

bool DrawBufferSlotsSatisfied(const DrawBufferSlots& slots,
                              const BoundBufferViewInfo (&bound)[MaxDrawBufferViews]) noexcept {
    for (uint32 slot = 0; slot < MaxDrawBufferViews; ++slot) {
        if ((slots.usedMask & (1u << slot)) == 0) {
            continue;
        }

        const BoundBufferViewInfo& view = bound[slot];
        const uint32 declared = slots.structureStrides[slot];
        if (!view.bound || view.kind != slots.kinds[slot] ||
            (view.kind == BufferViewKind::Typed &&
             view.shaderType != slots.typedShaderTypes[slot]) ||
            (view.kind == BufferViewKind::Structured && declared != 0 && view.stride != declared)) {
            return false;
        }
    }

    return true;
}

} // namespace rocks::graphics
