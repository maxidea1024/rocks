#include <Core/Memory/AddressSanitizer.h>

#include <Base/Compiler.h>

#if ROCKS_ADDRESS_SANITIZER
// ASan runtime이 내보내는 함수다. 모든 Rocks 대상의 runtime(clang compiler-rt, MSVC ASan)이 같은
// 이름을 제공한다. 일부 툴체인(Windows clang-cl 배포판)에는 <sanitizer/asan_interface.h>가 없어서
// 직접 선언한다.
extern "C" void __asan_poison_memory_region(void const volatile* address,
                                            decltype(sizeof(0)) bytes);
extern "C" void __asan_unpoison_memory_region(void const volatile* address,
                                              decltype(sizeof(0)) bytes);
#endif

namespace rocks {

void AsanPoisonMemory(const void* address, usize bytes) noexcept {
#if ROCKS_ADDRESS_SANITIZER
    __asan_poison_memory_region(address, bytes);
#else
    (void)address;
    (void)bytes;
#endif
}

void AsanUnpoisonMemory(const void* address, usize bytes) noexcept {
#if ROCKS_ADDRESS_SANITIZER
    __asan_unpoison_memory_region(address, bytes);
#else
    (void)address;
    (void)bytes;
#endif
}

} // namespace rocks
