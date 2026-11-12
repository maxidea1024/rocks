#include <Core/Memory/VendorAllocatorBackend.h>
#include <Base/Checked.h>
#include <Core/Move.h>

namespace rocks::detail {

VendorAllocatorBinding::VendorAllocatorBinding(VendorAlignedAllocate* allocate,
                                               VendorDeallocate* deallocate,
                                               bool alignmentFirst) noexcept
    : allocate_(allocate), deallocate_(deallocate), alignmentFirst_(alignmentFirst) {
}

Result<ExternalAllocatorBackend>
VendorAllocatorBinding::Bind(const ModuleLease& module) const& noexcept {
    if (!allocate_ || !deallocate_) {
        return Result<ExternalAllocatorBackend>::Failure(ErrorCode::InvalidArgument);
    }

    ExternalAllocatorBackend backend;
    backend.context = const_cast<VendorAllocatorBinding*>(this);
    backend.allocate = &Allocate;
    backend.deallocate = &Deallocate;
    backend.module = module;
    return Result<ExternalAllocatorBackend>::Success(Move(backend));
}

Result<void*> VendorAllocatorBinding::Allocate(void* context, usize bytes, usize alignment,
                                               uint32) noexcept {
    if (!context) {
        return Result<void*>::Failure(ErrorCode::InvalidArgument);
    }
    auto& binding = *static_cast<const VendorAllocatorBinding*>(context);
    if (!IsPowerOfTwo(alignment)) {
        return Result<void*>::Failure(ErrorCode::InvalidArgument);
    }
    if (bytes == 0) {
        return Result<void*>::Success(nullptr);
    }

    usize size = bytes;
    if (binding.alignmentFirst_) {
        // C11 aligned_alloc은 size가 alignment의 배수여야 한다.
        const usize remainder = bytes & (alignment - 1);
        if (remainder != 0 && !TryAdd(bytes, alignment - remainder, size)) {
            return Result<void*>::Failure(ErrorCode::Overflow);
        }
    }

    void* address = binding.alignmentFirst_ ? binding.allocate_(alignment, size)
                                            : binding.allocate_(size, alignment);
    if (!address) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }
    return Result<void*>::Success(address);
}

Result<void> VendorAllocatorBinding::Deallocate(void* context, void* address) noexcept {
    if (!context) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    auto& binding = *static_cast<const VendorAllocatorBinding*>(context);
    binding.deallocate_(address);
    return Result<void>::Success();
}

} // namespace rocks::detail
