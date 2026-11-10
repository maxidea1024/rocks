#include <Tests/Support/Test.h>
#include <Platform/Clock.h>
#include <Platform/Memory.h>
#include <Platform/Sync.h>
#include <Core/Memory/SystemAllocator.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#endif

using namespace rocks;

namespace {

constexpr usize WorkerCount = 4;
constexpr usize BlocksPerWorker = 64;

struct WorkerContext {
    platform::Mutex* mutex;
    uint64* counter;
    SystemAllocator* allocator;
    void** blocks;
    uint64 threadId = 0;
    bool success = true;
    bool release = false;
};

void RunWorker(WorkerContext& context) noexcept {
    context.threadId = platform::CurrentThreadId();
    uint64 previous = platform::MonotonicNanoseconds();
    for (usize index = 0; index < 10000; ++index) {
        {
            platform::LockGuard lock(*context.mutex);
            ++*context.counter;
        }

        const uint64 now = platform::MonotonicNanoseconds();
        if (now < previous) {
            context.success = false;
        }

        previous = now;
    }

    for (usize index = 0; index < BlocksPerWorker; ++index) {
        if (context.release) {
            if (!context.allocator->Deallocate(context.blocks[index])) {
                context.success = false;
            }
        } else {
            auto allocation = context.allocator->Allocate(32, 32);
            if (!allocation) {
                context.success = false;
                context.blocks[index] = nullptr;
            } else {
                context.blocks[index] = allocation.Value();
            }
        }
    }
}

#if defined(_WIN32)
DWORD WINAPI WorkerEntry(void* raw) {
    RunWorker(*static_cast<WorkerContext*>(raw));
    return 0;
}

bool RunAllWorkers(WorkerContext* contexts) {
    HANDLE threads[WorkerCount]{};
    bool success = true;
    contexts[0].mutex->Lock();
    for (usize index = 0; index < WorkerCount; ++index) {
        threads[index] = CreateThread(nullptr, 0, WorkerEntry, &contexts[index], 0, nullptr);
        if (threads[index] == nullptr) {
            success = false;
        }
    }

    contexts[0].mutex->Unlock();

    for (usize index = 0; index < WorkerCount; ++index) {
        if (threads[index] != nullptr) {
            if (WaitForSingleObject(threads[index], INFINITE) != WAIT_OBJECT_0) {
                platform::Fatal("Failed to join a platform test thread");
            }

            CloseHandle(threads[index]);
        }
    }

    return success;
}
#elif defined(__APPLE__)
void* WorkerEntry(void* raw) {
    RunWorker(*static_cast<WorkerContext*>(raw));
    return nullptr;
}

bool RunAllWorkers(WorkerContext* contexts) {
    pthread_t threads[WorkerCount]{};
    bool started[WorkerCount]{};
    bool success = true;
    contexts[0].mutex->Lock();
    for (usize index = 0; index < WorkerCount; ++index) {
        started[index] =
            pthread_create(&threads[index], nullptr, WorkerEntry, &contexts[index]) == 0;
        success = success && started[index];
    }

    contexts[0].mutex->Unlock();

    for (usize index = 0; index < WorkerCount; ++index) {
        if (started[index] && pthread_join(threads[index], nullptr) != 0) {
            platform::Fatal("Failed to join a platform test thread");
        }
    }

    return success;
}
#endif

} // namespace

TEST(PlatformAlignedAllocationAndClock) {
    CHECK(platform::AlignedAllocate(0, 16) == nullptr);
    CHECK(platform::AlignedAllocate(17, 0) == nullptr);
    CHECK(platform::AlignedAllocate(17, 3) == nullptr);
    CHECK(platform::AlignedAllocate(MaxSize, 64) == nullptr);
    platform::AlignedFree(nullptr);

    for (usize alignment = 1; alignment <= 4096; alignment *= 2) {
        void* address = platform::AlignedAllocate(57, alignment);
        REQUIRE(address != nullptr);
        CHECK(reinterpret_cast<uptr>(address) % alignment == 0);
        platform::AlignedFree(address);
    }

    const auto identifier = platform::CurrentThreadId();
    CHECK(identifier != 0);
    CHECK(identifier == platform::CurrentThreadId());
    const auto before = platform::MonotonicNanoseconds();
    const auto after = platform::MonotonicNanoseconds();
    CHECK(after >= before);
}

TEST(PlatformMutexPublicationAndCrossThreadFree) {
    platform::Mutex mutex;
    SystemAllocator allocator;
    uint64 counter = 0;
    void* blocks[WorkerCount][BlocksPerWorker]{};
    WorkerContext contexts[WorkerCount]{};
    for (usize index = 0; index < WorkerCount; ++index) {
        contexts[index].mutex = &mutex;
        contexts[index].counter = &counter;
        contexts[index].allocator = &allocator;
        contexts[index].blocks = blocks[index];
    }

    const bool allStarted = RunAllWorkers(contexts);
    // Join all successful starts before any assertion can return from this stack.
    CHECK(allStarted);
    if (allStarted) {
        CHECK(counter == WorkerCount * 10000);
        CHECK(allocator.Stats().liveAllocations == WorkerCount * BlocksPerWorker);
        CHECK(allocator.Stats().liveBytes == WorkerCount * BlocksPerWorker * 32);
    }

    for (usize index = 0; index < WorkerCount; ++index) {
        CHECK(contexts[index].success);
        if (allStarted) {
            CHECK(contexts[index].threadId != platform::CurrentThreadId());
            for (usize other = 0; other < index; ++other) {
                CHECK(contexts[index].threadId != contexts[other].threadId);
            }
        }
    }

    // Different workers free each producer's blocks. All allocation workers are joined first.
    counter = 0;
    for (usize index = 0; index < WorkerCount; ++index) {
        contexts[index].blocks = blocks[(index + 1) % WorkerCount];
        contexts[index].release = true;
    }

    const bool releasesStarted = RunAllWorkers(contexts);
    CHECK(releasesStarted);
    if (!releasesStarted) {
        // Failed starts leave blocks live; membership checks make cleanup safe without
        // dereferencing already-freed addresses.
        for (auto& workerBlocks : blocks) {
            for (void* address : workerBlocks) {
                auto ignored = allocator.Deallocate(address);
                (void)ignored;
            }
        }
    }

    for (const auto& context : contexts) {
        CHECK(context.success);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
}
