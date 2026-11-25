#include <Core/Memory/UniquePtr.h>
#include <Launch/EntryPoint.h>
#include <ModuleLoader/ModuleLoader.h>
#include <Object/ModuleTypeRegistrations.h>
#include <Object/NativeClass.h>
#include <Object/TypeRegistry.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Memory.h>
#include <World/Actor.generated.h>
#include <World/World.h>
#include <stdio.h>

// World 규모 측정 도구다(Docs/Implementation/GcScaleExperiment.ko.md 15절 — 선형 검색 점검).
// Actor 수 N을 바꿔 연산 하나의 비용이 N과 무관한지 본다.
// - destroy: Actor 하나의 파괴(DestroyActor와 다음 프레임의 정리)
// - bypass destroy: World API 밖 파괴(Registry RequestDestroy) 하나와 다음 프레임의 복구·정리
//   (RepairBypassedObjects — Docs/Design/EngineRuntime 7.4절)
// - frame with GC start: Actor 하나를 파괴한 프레임(그 프레임이 GC cycle을 시작한다)의 Tick
// - level destroy: Actor가 적은 Level 하나의 파괴(다른 Level의 Actor 수 N과 무관해야 한다)
// - frame after one spawn: 틱 있는 Actor 하나를 만든 뒤의 프레임(틱 순서 재구성 포함)
// Actor는 모두 틱이 있다(TickScheduler 규모). 할당자는 해제가 O(1)인 벤치 전용이다(SystemAllocator
// 해제는 살아 있는 할당 수에 비례한다).
//
// 사용: WorldBench [--actors N]... [--ticking]
namespace world_bench {

// 틱이 있는 측정용 Actor다.
class BenchActor : public rocks::Actor {
    ROCKS_NATIVE_TYPE(BenchActor, rocks::Actor, 0x576f726c64426e63ull, 0x8a1f00c3d2b74e01ull)

public:
    explicit BenchActor(rocks::Allocator& allocator) noexcept : Actor(allocator) {
        PrimaryTick().tick = true;
    }

    void OnTick(const rocks::TickContext&) noexcept override {
        ++ticks;
    }

    static inline rocks::uint64 ticks = 0;
};

} // namespace world_bench

namespace {

using namespace rocks;
using world_bench::BenchActor;

class BenchAllocator final : public Allocator {
public:
    Result<void*> Allocate(usize bytes, usize alignment, uint32) noexcept override {
        if (bytes == 0) {
            return Result<void*>::Success(nullptr);
        }

        const usize prefix = alignment > sizeof(usize) ? alignment : sizeof(usize);
        void* base = platform::AlignedAllocate(bytes + prefix, prefix);
        if (base == nullptr) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        uint8* user = static_cast<uint8*>(base) + prefix;
        reinterpret_cast<usize*>(user)[-1] = prefix;
        platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&allocations_, 1);
        return Result<void*>::Success(user);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (address == nullptr) {
            return Result<void>::Success();
        }

        uint8* user = static_cast<uint8*>(address);
        const usize prefix = reinterpret_cast<usize*>(user)[-1];
        platform::AtomicFetchSub<platform::MemoryOrder::Relaxed>(&allocations_, 1);
        platform::AlignedFree(user - prefix);
        return Result<void>::Success();
    }

    uint64 Allocations() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&allocations_);
    }

private:
    uint64 allocations_ = 0;
};

constexpr double FrameSeconds = 1.0 / 60.0;
constexpr uint32 Rounds = 9;
constexpr uint32 DestroyBatch = 200;
// Level 파괴 측정에서 작은 Level 하나의 Actor 수다.
constexpr uint32 SmallLevelActors = 16;

double Microseconds(uint64 nanoseconds) noexcept {
    return static_cast<double>(nanoseconds) * 1e-3;
}

uint64 Min(uint64 left, uint64 right) noexcept {
    return left < right ? left : right;
}

int Measure(ModuleLoader& modules, uint32 actors, bool ticking) noexcept {
    BenchAllocator memory;
    int status = 0;
    {
        // World 리플렉션 타입은 모듈 로더가 등록한다(Docs/Design/ModuleLoader 3.11절):
        // Launch의 프로세스 로더에서 World 모듈(의존 폐포)을 시작하고(처음 한 번, 프로세스 묶음) 이
        // 레지스트리의 타입 등록기를 붙인다. 등록기는 레지스트리보다 먼저 뗀다(detach).
        if (!modules.Load("World").IsOk()) {
            return 2;
        }

        TypeRegistry types(memory);
        ModuleTypeRegistrations registrations(types, memory);
        if (!modules.AttachTypeRegistrar(registrations).IsOk()) {
            return 2;
        }

        struct DetachTypes {
            ModuleLoader& loader;
            ModuleTypeRegistrations& registrations;

            ~DetachTypes() noexcept {
                loader.DetachTypeRegistrar(registrations);
            }
        } detach{modules, registrations};

        auto token = RegisterNativeClass<BenchActor>(types);
        if (!token) {
            return 2;
        }

        {
            WorldDesc desc;
            desc.name = "Bench";
            desc.context = {0x57424e43ull, actors};
            desc.timeStep = {FrameSeconds, 0.25, 8};
            auto created = UniquePtr<World>::TryMake(memory, memory, types, desc);
            if (!created) {
                return 3;
            }

            World& world = *created.Value();
            // 측정 대상 Actor다(ticking이면 틱이 있다 — 틱 순서 재구성 비용을 볼 때).
            auto spawn = [&](const SpawnParams& params) noexcept {
                return ticking ? static_cast<bool>(world.SpawnActor<BenchActor>(params))
                               : static_cast<bool>(world.SpawnActor<Actor>(params));
            };

            if (!world.Initialize() || !world.BeginPlay()) {
                return 4;
            }

            const uint64 spawnStarted = platform::MonotonicNanoseconds();
            for (uint32 index = 0; index < actors; ++index) {
                if (!spawn({})) {
                    return 5;
                }
            }

            const uint64 spawned = platform::MonotonicNanoseconds();
            for (uint32 frame = 0; frame < 4; ++frame) {
                if (!world.Tick(FrameSeconds)) {
                    return 6;
                }
            }

            // Actor 파괴: DestroyBatch개를 파괴하고 한 프레임으로 정리한다.
            uint64 destroyBest = NumericLimits<uint64>::Max();
            for (uint32 round = 0; round < Rounds; ++round) {
                ObjectHandle victims[DestroyBatch];
                uint32 count = 0;
                world.ForEachActor([&](ObjectHandle actor) noexcept {
                    if (count < DestroyBatch) {
                        victims[count++] = actor;
                    }
                });
                const uint64 started = platform::MonotonicNanoseconds();
                for (uint32 index = 0; index < count; ++index) {
                    if (!world.DestroyActor(victims[index])) {
                        return 7;
                    }
                }

                if (!world.Tick(FrameSeconds)) {
                    return 8;
                }

                destroyBest = Min(destroyBest, platform::MonotonicNanoseconds() - started);
                for (uint32 index = 0; index < count; ++index) {
                    if (!spawn({})) {
                        return 9;
                    }
                }
            }

            // World API 밖 파괴: DestroyBatch개를 Registry에서 바로 파괴 요청하고 한 프레임으로
            // 복구·정리한다.
            uint64 bypassBest = NumericLimits<uint64>::Max();
            for (uint32 round = 0; round < Rounds; ++round) {
                ObjectHandle victims[DestroyBatch];
                uint32 count = 0;
                world.ForEachActor([&](ObjectHandle actor) noexcept {
                    if (count < DestroyBatch) {
                        victims[count++] = actor;
                    }
                });
                const uint64 started = platform::MonotonicNanoseconds();
                for (uint32 index = 0; index < count; ++index) {
                    if (!world.Objects().RequestDestroy(world.Access(), victims[index])) {
                        return 24;
                    }
                }

                if (!world.Tick(FrameSeconds)) {
                    return 25;
                }

                bypassBest = Min(bypassBest, platform::MonotonicNanoseconds() - started);
                for (uint32 index = 0; index < count; ++index) {
                    if (!spawn({})) {
                        return 26;
                    }
                }
            }

            // 변화 없는 프레임(비교 기준 — 틱 있는 Actor면 모두 실행한다).
            uint64 idleBest = NumericLimits<uint64>::Max();
            for (uint32 round = 0; round < Rounds; ++round) {
                const uint64 started = platform::MonotonicNanoseconds();
                if (!world.Tick(FrameSeconds)) {
                    return 23;
                }

                idleBest = Min(idleBest, platform::MonotonicNanoseconds() - started);
            }

            // GC cycle을 시작하는 프레임: Actor 하나를 파괴해 다음 Tick이 cycle을 시작하게 한다.
            uint64 frameBest = NumericLimits<uint64>::Max();
            for (uint32 round = 0; round < Rounds; ++round) {
                // 진행 중인 cycle을 끝낸다.
                for (uint32 frame = 0; frame < 64; ++frame) {
                    if (!world.Tick(FrameSeconds)) {
                        return 10;
                    }
                }

                ObjectHandle victim;
                world.ForEachActor([&](ObjectHandle actor) noexcept { victim = actor; });
                if (!world.DestroyActor(victim) || !spawn({})) {
                    return 11;
                }

                const uint64 started = platform::MonotonicNanoseconds();
                if (!world.Tick(FrameSeconds)) {
                    return 12;
                }

                frameBest = Min(frameBest, platform::MonotonicNanoseconds() - started);
            }

            // 틱 있는 Actor 하나를 만든 뒤의 프레임(틱 순서 재구성).
            uint64 spawnFrameBest = NumericLimits<uint64>::Max();
            for (uint32 round = 0; round < Rounds; ++round) {
                for (uint32 frame = 0; frame < 64; ++frame) {
                    if (!world.Tick(FrameSeconds)) {
                        return 20;
                    }
                }

                if (!spawn({})) {
                    return 21;
                }

                const uint64 started = platform::MonotonicNanoseconds();
                if (!world.Tick(FrameSeconds)) {
                    return 22;
                }

                spawnFrameBest = Min(spawnFrameBest, platform::MonotonicNanoseconds() - started);
            }

            // 작은 Level 파괴: 다른 Level(영구 Level)의 Actor 수와 무관해야 한다.
            uint64 levelBest = NumericLimits<uint64>::Max();
            for (uint32 round = 0; round < Rounds; ++round) {
                auto level = world.CreateLevel("Small");
                if (!level) {
                    return 13;
                }

                SpawnParams params;
                params.level = level.Value();
                for (uint32 index = 0; index < SmallLevelActors; ++index) {
                    if (!spawn(params)) {
                        return 14;
                    }
                }

                const uint64 started = platform::MonotonicNanoseconds();
                if (!world.DestroyLevel(level.Value())) {
                    return 15;
                }

                levelBest = Min(levelBest, platform::MonotonicNanoseconds() - started);
                if (!world.Tick(FrameSeconds)) {
                    return 16;
                }
            }

            printf("actors %u%s: spawn %.2f us/actor, destroy %.2f us/actor, bypass destroy %.2f "
                   "us/actor, idle frame %.1f us, "
                   "frame with GC start %.1f us, frame after one spawn %.1f us, small level "
                   "destroy %.1f us\n",
                   actors, ticking ? " (ticking)" : "",
                   Microseconds(spawned - spawnStarted) / actors,
                   Microseconds(destroyBest) / DestroyBatch,
                   Microseconds(bypassBest) / DestroyBatch, Microseconds(idleBest),
                   Microseconds(frameBest), Microseconds(spawnFrameBest), Microseconds(levelBest));
            fflush(stdout);
            if (!world.Shutdown()) {
                status = 17;
            }
        }

        // 모듈 타입은 DetachTypes가 로더에서 떼며 되돌린다(origin ML2에서 남은 옛
        // ModuleRegistration 호출 modules.Unregister()는 컴파일되지 않아 뺐다).
        if (!types.Unregister(token.Value())) {
            status = 18;
        }
    }

    return memory.Allocations() == 0 ? status : 19;
}

// 최대 측정 크기 수다(--actors를 이보다 많이 주면 앞의 것만 쓴다).
constexpr rocks::uint32 MaxSizes = 8;

// 실행 본문이다(Launch 공통 실행 계층이 크래시 수집·명령줄 캡처 뒤 부른다).
int RunMain(rocks::launch::ApplicationContext& context) noexcept {
    const rocks::Span<const rocks::StringView> arguments = context.Arguments();
    rocks::uint32 sizes[MaxSizes];
    rocks::uint32 count = 0;
    bool ticking = false;
    // Launch 인자의 Data()는 NUL로 끝난다.
    for (const rocks::StringView argument : arguments) {
        const char* text = argument.Data();
        ticking = ticking || (text[0] == '-' && text[1] == '-' && text[2] == 't');
    }

    for (rocks::usize index = 0; index + 1 < arguments.Size() && count < MaxSizes; ++index) {
        const char* text = arguments[index].Data();
        if (text[0] == '-' && text[1] == '-' && text[2] == 'a') {
            rocks::uint32 value = 0;
            for (const char* digit = arguments[index + 1].Data(); *digit >= '0' && *digit <= '9';
                 ++digit) {
                value = value * 10 + static_cast<rocks::uint32>(*digit - '0');
            }

            sizes[count++] = value;
        }
    }

    if (count == 0) {
        sizes[count++] = 10000;
        sizes[count++] = 100000;
    }

    for (rocks::uint32 index = 0; index < count; ++index) {
        if (const int status = Measure(context.Modules(), sizes[index], ticking); status != 0) {
            printf("WorldBench failed with status %d\n", status);
            return status;
        }
    }

    return 0;
}

// 무인 측정 도구다. 실패가 대화상자에서 멈추지 않게 OS·CRT 오류 대화상자를 끈다(전에는
// TestSupport PrepareAutomatedTestProcess가 했다).
const rocks::launch::ApplicationDescription& Describe() noexcept {
    static constexpr rocks::launch::ApplicationDescription Description{
        .name = "WorldBench",
        .kind = rocks::launch::ApplicationKind::Tool,
        .settings = {.suppressSystemErrorDialogs = true},
        .run = &RunMain};
    return Description;
}

} // namespace

ROCKS_IMPLEMENT_APPLICATION(Describe)
