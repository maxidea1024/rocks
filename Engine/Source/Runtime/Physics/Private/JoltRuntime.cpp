// Jolt 프로세스 공유 자원(Docs/Design/PhysicsModule 7.1.1절 PMD21, 어댑터 경계 10.4절 순서).
#include "JoltRuntime.h"

#include "PhysicsLog.h"

#include <Base/PlacementNew.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Platform/Sync.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Jolt.h가 Jolt의 모든 헤더보다 먼저 와야 한다(Core.h의 설정).
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/Memory.h>
// clang-format on

namespace rocks::physics {

namespace {

// Jolt 기본 Allocate는 malloc이다. 64-bit 대상의 malloc 정렬과 같게 둔다.
constexpr usize JoltDefaultAlignment = 16;

// Trace hook이 메시지를 펼치는 버퍼 크기(byte)다. 넘으면 잘린다.
constexpr usize TraceMessageBytes = 1024;

// assert 실패 Fatal 문구 버퍼 크기(byte)다.
constexpr usize AssertMessageBytes = 1024;

// 프로세스 수명 할당기다(소멸하지 않는 정적 저장소 — Jolt가 프로세스 종료 때 남긴 블록이 있어도
// 할당기가 먼저 사라지지 않게).
SystemAllocator& JoltAllocator() noexcept {
    alignas(SystemAllocator) static unsigned char storage[sizeof(SystemAllocator)];
    static SystemAllocator* allocator = ::new (static_cast<void*>(storage)) SystemAllocator();
    return *allocator;
}

uint64 g_liveAllocations = 0;

platform::Mutex& RuntimeMutex() noexcept {
    alignas(platform::Mutex) static unsigned char storage[sizeof(platform::Mutex)];
    static platform::Mutex* mutex = ::new (static_cast<void*>(storage)) platform::Mutex();
    return *mutex;
}

// RuntimeMutex가 보호한다.
uint32 g_references = 0;

void* AllocateAligned(size_t size, size_t alignment) {
    Result<void*> block = JoltAllocator().Allocate(size == 0 ? 1 : size, alignment);
    if (!block) {
        // Jolt는 할당 실패를 다루지 않는다(16.4절 — 진단 후 fatal).
        platform::Fatal("Jolt allocation failed (out of memory)");
    }

    platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&g_liveAllocations, uint64{1});
    return block.Value();
}

void Free(void* block) {
    if (block == nullptr) {
        return;
    }

    (void)JoltAllocator().Deallocate(block);
    platform::AtomicFetchSub<platform::MemoryOrder::Relaxed>(&g_liveAllocations, uint64{1});
}

void* Allocate(size_t size) {
    return AllocateAligned(size, JoltDefaultAlignment);
}

void* Reallocate(void* block, size_t oldSize, size_t newSize) {
    void* resized = Allocate(newSize);
    if (block != nullptr) {
        memcpy(resized, block, oldSize < newSize ? oldSize : newSize);
        Free(block);
    }

    return resized;
}

void TraceHook(const char* format, ...) {
    char message[TraceMessageBytes];
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (written < 0) {
        return;
    }

    Log::Info(LogPhysicsJolt, "{Message}", StringView(message, strlen(message)));
}

bool AssertFailedHook(const char* expression, const char* message, const char* file,
                      JPH::uint line) {
    // Rocks assert와 같이 모든 구성에서 프로세스를 멈춘다(어댑터 경계 10.4절).
    char text[AssertMessageBytes];
    snprintf(text, sizeof(text), "Jolt assertion failed: %s (%s) at %s:%u",
             expression != nullptr ? expression : "", message != nullptr ? message : "",
             file != nullptr ? file : "", static_cast<unsigned>(line));
    platform::Fatal(text);
}

void InstallHooks() noexcept {
    JPH::Allocate = Allocate;
    JPH::Reallocate = Reallocate;
    JPH::Free = Free;
    JPH::AlignedAllocate = AllocateAligned;
    JPH::AlignedFree = Free;
    JPH::Trace = TraceHook;
    JPH::AssertFailed = AssertFailedHook;
}

} // namespace

JoltRuntimeLease::JoltRuntimeLease(JoltRuntimeLease&& other) noexcept : held_(other.held_) {
    other.held_ = false;
}

JoltRuntimeLease& JoltRuntimeLease::operator=(JoltRuntimeLease&& other) noexcept {
    if (this != &other) {
        Release();
        held_ = other.held_;
        other.held_ = false;
    }

    return *this;
}

JoltRuntimeLease::~JoltRuntimeLease() {
    Release();
}

Result<JoltRuntimeLease> JoltRuntimeLease::Acquire() noexcept {
    platform::LockGuard lock(RuntimeMutex());
    if (g_references == 0) {
        // hook은 매번 다시 건다 — 사이에 다른 코드(Jolt 연결 시험)가 바꿨을 수 있다.
        InstallHooks();
        if (!JPH::VerifyJoltVersionID()) {
            Log::Error(LogPhysics,
                       "Jolt version ID mismatch: the library and the Physics module were built "
                       "with different Jolt defines");
            return Result<JoltRuntimeLease>::Failure(ErrorCode::InvalidArgument);
        }

        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }

    ++g_references;
    JoltRuntimeLease lease;
    lease.held_ = true;
    return Result<JoltRuntimeLease>::Success(Move(lease));
}

void JoltRuntimeLease::Release() noexcept {
    if (!held_) {
        return;
    }

    held_ = false;
    platform::LockGuard lock(RuntimeMutex());
    if (--g_references == 0) {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

uint32 JoltRuntimeReferenceCount() noexcept {
    platform::LockGuard lock(RuntimeMutex());
    return g_references;
}

uint64 JoltLiveAllocations() noexcept {
    return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&g_liveAllocations);
}

} // namespace rocks::physics
