#pragma once

#include <Core/Memory/ExternalAllocator.h>

namespace rocks {

// 외부 C ABI의 호출 규약은 실제 제공자 헤더와 바이너리의 선언에 일치해야 한다.
// 공급자 함수는 예외를 발생시키지 않고 동일한 할당기 빌드의 allocate/free 쌍이어야 한다.
// 공급자 함수는 예외 없이 반환해야 한다. 이 연결은 Rocks tag를 공급자에 전달하지 않는다.
extern "C" {
using VendorAlignedAllocate = void*(usize, usize);
using VendorDeallocate = void(void*);
}

namespace detail {

// 함수 주소와 순서는 생성 후 변경되지 않는다. Bind가 반환하는 context가 이 객체를
// 빌리므로 이동·복사를 금지하며, 객체는 ExternalAllocator보다 오래 살아야 한다.
class VendorAllocatorBinding {
public:
    VendorAllocatorBinding(const VendorAllocatorBinding&) = delete;
    VendorAllocatorBinding& operator=(const VendorAllocatorBinding&) = delete;
    VendorAllocatorBinding(VendorAllocatorBinding&&) = delete;
    VendorAllocatorBinding& operator=(VendorAllocatorBinding&&) = delete;

    // 주소가 없으면 InvalidArgument다. 동적 라이브러리의 함수는 해당 모듈 lease를 전달한다.
    // 이 연산은 공급자의 이름이나 실제 함수 출처를 검증하지 않으며 전역 malloc을 바꾸지 않는다.
    Result<ExternalAllocatorBackend> Bind(const ModuleLease& module = {}) const& noexcept;
    Result<ExternalAllocatorBackend> Bind(const ModuleLease& module = {}) const&& = delete;

protected:
    VendorAllocatorBinding(VendorAlignedAllocate* allocate, VendorDeallocate* deallocate,
                           bool alignmentFirst) noexcept;
    ~VendorAllocatorBinding() = default;

private:
    static Result<void*> Allocate(void* context, usize bytes, usize alignment, uint32 tag) noexcept;
    static Result<void> Deallocate(void* context, void* address) noexcept;

    VendorAlignedAllocate* allocate_;
    VendorDeallocate* deallocate_;
    bool alignmentFirst_;
};

} // namespace detail

// 사용 예: JemallocBackend functions(je_aligned_alloc, je_free);
// auto backend = functions.Bind(moduleLease); ExternalAllocator allocator(Move(backend.Value()));
// je_aligned_alloc과 je_free는 같은 jemalloc 빌드에서 얻는다. 지원되는 심볼 이름과 접두사,
// calling convention, 실제 라이브러리의 thread 안전성과 모듈 수명은 호출자가 확인한다.
class JemallocBackend final : public detail::VendorAllocatorBinding {
public:
    JemallocBackend(VendorAlignedAllocate* alignedAllocate, VendorDeallocate* deallocate) noexcept
        : VendorAllocatorBinding(alignedAllocate, deallocate, true) {
    }
};

// TCMalloc의 aligned_alloc/free는 프로세스 전역 심볼일 수 있다. 호출자가 TCMalloc에 실제로
// 바인딩된 한 쌍인지 확인해야 하며 주소 존재만으로 공급자를 식별하지 않는다. Google의 공식
// 플랫폼 지원을 벗어난 시스템은 별도 검증 전 지원한다고 가정하지 않는다.
class TcmallocBackend final : public detail::VendorAllocatorBinding {
public:
    TcmallocBackend(VendorAlignedAllocate* alignedAllocate, VendorDeallocate* deallocate) noexcept
        : VendorAllocatorBinding(alignedAllocate, deallocate, true) {
    }
};

// mi_malloc_aligned(size, alignment)와 mi_free의 인자 순서를 사용한다. 이 바인딩은
// 라이브러리를 포함·로드하지 않는다. 함수쌍은 같은 mimalloc 빌드에서 얻어야 한다.
class MimallocBackend final : public detail::VendorAllocatorBinding {
public:
    MimallocBackend(VendorAlignedAllocate* alignedAllocate, VendorDeallocate* deallocate) noexcept
        : VendorAllocatorBinding(alignedAllocate, deallocate, false) {
    }
};

} // namespace rocks
