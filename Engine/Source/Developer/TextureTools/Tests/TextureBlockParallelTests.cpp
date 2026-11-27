#include "BlockGoldenImages.h"

#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Function.h>
#include <Core/Math/Float16.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Sync.h>
#include <Task/WorkerScheduler.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <TextureTools/TextureBlockEncode.h>

#include <stdio.h>
#include <string.h>

// 병렬 블록 압축 시험이다(Docs/Design/AssetSystem 12.17.9 5 — TC5a). 결과 byte가 scheduler
// 유무·worker 수·batch 크기와 무관한지(모든 형식·블록 배수가 아닌 크기·slice 여럿·batch가 slice를
// 넘음·행 여백 입력), 시작 전·종료 뒤·같은 scheduler worker 안에서 부르면 직렬로 대체되는지,
// scratch 없는 scheduler는 InvalidArgument인지, BC1–BC5가 호출자 FP 설정과 무관한지를 본다.
using namespace rocks;
namespace texture_block_parallel_tests_file {

namespace {

namespace golden = test::block_golden;

using Body = Function<void(WorkerTaskContext&)>;

// 블록 배수가 아닌 크기와 slice 셋이다.
constexpr uint32 SizeX = 37;
constexpr uint32 SizeY = 29;
constexpr uint32 Slices = 3;
// 행·slice 여백(byte)이다.
constexpr usize RowPadding = 12;
constexpr usize SlicePadding = 64;

struct Workers {
    explicit Workers(uint32 count) noexcept : scheduler(allocator) {
        WorkerSchedulerConfig config;
        config.workerCount = count;
        started = static_cast<bool>(scheduler.Start(config));
    }

    ~Workers() noexcept {
        if (started) {
            (void)scheduler.Shutdown();
        }
    }

    SystemAllocator allocator;
    WorkerScheduler scheduler;
    bool started = false;
};

bool HasIspcEncoder() noexcept {
    return !IspcBcEncoderIdentity().IsEmpty();
}

// 형식의 입력 그림이다(texel byte는 TextureBlockSourceTexelBytes). BC6H는 [0, 4] half, 그 밖은
// 무작위 byte다.
bool MakeInput(TextureBlockFormat format, uint32 seed, Array<uint8>& out, usize rowPitch = 0,
               usize slicePitch = 0) noexcept {
    const usize texelBytes = TextureBlockSourceTexelBytes(format);
    const usize pitch = rowPitch == 0 ? usize{SizeX} * texelBytes : rowPitch;
    const usize plane = slicePitch == 0 ? pitch * SizeY : slicePitch;
    if (!out.TryResize(plane * Slices)) {
        return false;
    }

    golden::Random random(seed);
    for (usize i = 0; i < out.Size(); ++i) {
        out[i] = random.Byte();
    }

    if (format == TextureBlockFormat::Bc6h) {
        for (uint32 slice = 0; slice < Slices; ++slice) {
            for (uint32 y = 0; y < SizeY; ++y) {
                uint8* line = out.Data() + plane * slice + pitch * y;
                for (uint32 c = 0; c < SizeX * 4; ++c) {
                    const float value = static_cast<float>(random.Next() % 4096) / 1024.0f;
                    const uint16 half = Float16::FromFloat(value).bits;
                    line[c * 2] = static_cast<uint8>(half & 0xFF);
                    line[c * 2 + 1] = static_cast<uint8>(half >> 8);
                }
            }
        }
    }

    return true;
}

Result<void> Encode(TextureBlockFormat format, const Array<uint8>& input,
                    const TextureBlockOptions& options, Array<uint8>& out, usize rowPitch = 0,
                    usize slicePitch = 0) noexcept {
    auto bytes = TextureBlockOutputBytes(format, SizeX, SizeY, Slices);
    if (!bytes) {
        return Result<void>::Failure(bytes.Error());
    }

    if (!out.TryResize(static_cast<usize>(bytes.Value()))) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    TextureBlockImage image;
    image.rgba = input.View();
    image.sizeX = SizeX;
    image.sizeY = SizeY;
    image.numSlices = Slices;
    image.rowPitch = rowPitch;
    image.slicePitch = slicePitch;
    return EncodeTextureBlocks(format, image, options, out.View());
}

bool Same(const Array<uint8>& a, const Array<uint8>& b) noexcept {
    return a.Size() == b.Size() && memcmp(a.Data(), b.Data(), a.Size()) == 0;
}

struct Variant {
    TextureBlockFormat format;
    TextureAstcMode mode;
    bool srgb;
};

// 시험할 형식·ASTC 배치 조합이다(ISPC 형식은 인코더가 있는 대상만).
usize Variants(Variant (&out)[16]) noexcept {
    usize count = 0;
    for (const TextureBlockFormat format :
         {TextureBlockFormat::Bc1, TextureBlockFormat::Bc3, TextureBlockFormat::Bc4,
          TextureBlockFormat::Bc5, TextureBlockFormat::EacR11, TextureBlockFormat::Astc4x4,
          TextureBlockFormat::Astc8x8, TextureBlockFormat::Astc10x10,
          TextureBlockFormat::Astc12x12}) {
        out[count++] = Variant{format, TextureAstcMode::Rgba, false};
    }

    out[count++] = Variant{TextureBlockFormat::Astc6x6, TextureAstcMode::Rgb, false};
    out[count++] = Variant{TextureBlockFormat::Astc6x6, TextureAstcMode::Rgba, true};
    out[count++] = Variant{TextureBlockFormat::Astc6x6, TextureAstcMode::NormalLa, false};
    if (HasIspcEncoder()) {
        out[count++] = Variant{TextureBlockFormat::Bc6h, TextureAstcMode::Rgba, false};
        out[count++] = Variant{TextureBlockFormat::Bc7, TextureAstcMode::Rgba, false};
    }

    return count;
}

TextureBlockOptions OptionsOf(const Variant& variant) noexcept {
    TextureBlockOptions options;
    options.astcMode = variant.mode;
    options.srgb = variant.srgb;
    options.alphaProfile = variant.format == TextureBlockFormat::Bc7;
    return options;
}

// worker count개를 gate가 열릴 때까지 붙잡는다. 각 작업이 시작했다고 알린 뒤에 돌려준다(그 뒤
// 제출한 helper는 worker가 비지 않아 실행되지 못하고 호출 스레드가 회수한다 — 시간에 기대지
// 않는다).
struct Blockers {
    explicit Blockers(Allocator& allocator) noexcept : tasks(allocator) {
    }

    bool Hold(WorkerScheduler& scheduler, Allocator& allocator, uint32 count) noexcept {
        for (uint32 index = 0; index < count; ++index) {
            auto body = Body::TryCreate(allocator, [this, index](WorkerTaskContext&) noexcept {
                started[index].Signal();
                gate.Wait();
            });
            if (!body) {
                return false;
            }

            auto task = scheduler.Submit(Move(body.Value()));
            if (!task || !tasks.TryPushBack(Move(task.Value()))) {
                return false;
            }
        }

        for (uint32 index = 0; index < count; ++index) {
            started[index].Wait();
        }

        return true;
    }

    void Release() noexcept {
        gate.Signal();
        for (const WorkerTaskHandle& task : tasks) {
            (void)task.Wait();
        }
    }

    static constexpr uint32 Max = 8;
    platform::ManualResetEvent gate;
    platform::ManualResetEvent started[Max];
    Array<WorkerTaskHandle> tasks;
};

struct NestedEncode {
    WorkerScheduler* scheduler;
    Allocator* allocator;
    const Array<uint8>* input;
    Array<uint8>* out;
    ErrorCode result;
};

} // namespace

// 직렬과 scheduler worker 1·2·4·8의 byte가 같다. batch는 형식 기본, 블록 행 하나(minBatchBlocks 1),
// slice를 넘는 묶음(블록 행 다섯 — 블록 행 8인 slice 경계를 넘는다) 셋이다.
TEST(TextureBlockParallel_MatchesSerial) {
    SystemAllocator allocator;
    Variant variants[16];
    const usize count = Variants(variants);
    Array<uint8> serial[16] = {
        Array<uint8>(allocator), Array<uint8>(allocator), Array<uint8>(allocator),
        Array<uint8>(allocator), Array<uint8>(allocator), Array<uint8>(allocator),
        Array<uint8>(allocator), Array<uint8>(allocator), Array<uint8>(allocator),
        Array<uint8>(allocator), Array<uint8>(allocator), Array<uint8>(allocator),
        Array<uint8>(allocator), Array<uint8>(allocator), Array<uint8>(allocator),
        Array<uint8>(allocator)};
    Array<uint8> inputs[16] = {
        Array<uint8>(allocator), Array<uint8>(allocator), Array<uint8>(allocator),
        Array<uint8>(allocator), Array<uint8>(allocator), Array<uint8>(allocator),
        Array<uint8>(allocator), Array<uint8>(allocator), Array<uint8>(allocator),
        Array<uint8>(allocator), Array<uint8>(allocator), Array<uint8>(allocator),
        Array<uint8>(allocator), Array<uint8>(allocator), Array<uint8>(allocator),
        Array<uint8>(allocator)};
    for (usize i = 0; i < count; ++i) {
        REQUIRE(MakeInput(variants[i].format, static_cast<uint32>(i + 1), inputs[i]));
        REQUIRE(Encode(variants[i].format, inputs[i], OptionsOf(variants[i]), serial[i]));
    }

    for (const uint32 workers : {1u, 2u, 4u, 8u}) {
        Workers pool(workers);
        REQUIRE(pool.started);
        for (usize i = 0; i < count; ++i) {
            const uint32 blocksX = (SizeX + TextureBlockWidth(variants[i].format) - 1) /
                                   TextureBlockWidth(variants[i].format);
            // 마지막 batch가 1행: 블록 행 24(4 × 4 — 8행 × 3 slice) ÷ 23행 batch.
            for (const uint32 batch : {0u, 1u, blocksX * 5, blocksX * 23}) {
                TextureBlockOptions options = OptionsOf(variants[i]);
                options.scheduler = &pool.scheduler;
                options.scratch = &pool.allocator;
                options.minBatchBlocks = batch;
                Array<uint8> parallel(allocator);
                REQUIRE(Encode(variants[i].format, inputs[i], options, parallel));
                const bool same = Same(parallel, serial[i]);
                if (!same) {
                    printf("  mismatch: format %u mode %u srgb %u workers %u batch %u\n",
                           static_cast<unsigned>(variants[i].format),
                           static_cast<unsigned>(variants[i].mode),
                           static_cast<unsigned>(variants[i].srgb), workers, batch);
                }

                CHECK(same);
            }
        }
    }
}

// 행·slice 여백 입력 + scheduler(ASTC는 호출 스레드에서 빽빽한 사본을 한 번 만든다)도 빽빽한 입력의
// 직렬 결과와 같다.
TEST(TextureBlockParallel_PitchedInput) {
    SystemAllocator allocator;
    Workers pool(4);
    REQUIRE(pool.started);
    for (const TextureBlockFormat format : {TextureBlockFormat::Bc1, TextureBlockFormat::EacR11,
                                            TextureBlockFormat::Astc6x6, TextureBlockFormat::Bc7}) {
        if (format == TextureBlockFormat::Bc7 && !HasIspcEncoder()) {
            continue;
        }

        const usize texelBytes = TextureBlockSourceTexelBytes(format);
        const usize pitch = usize{SizeX} * texelBytes + RowPadding;
        const usize plane = pitch * SizeY + SlicePadding;
        Array<uint8> padded(allocator);
        REQUIRE(MakeInput(format, 7, padded, pitch, plane));
        Array<uint8> packed(allocator);
        REQUIRE(packed.TryResize(usize{SizeX} * texelBytes * SizeY * Slices));
        for (uint32 slice = 0; slice < Slices; ++slice) {
            for (uint32 y = 0; y < SizeY; ++y) {
                memcpy(packed.Data() + (usize{slice} * SizeY + y) * SizeX * texelBytes,
                       padded.Data() + plane * slice + pitch * y, usize{SizeX} * texelBytes);
            }
        }

        TextureBlockOptions serialOptions;
        serialOptions.scratch = &allocator;
        Array<uint8> want(allocator);
        REQUIRE(Encode(format, packed, serialOptions, want));
        TextureBlockOptions options;
        options.scheduler = &pool.scheduler;
        options.scratch = &pool.allocator;
        options.minBatchBlocks = 1;
        Array<uint8> got(allocator);
        REQUIRE(Encode(format, padded, options, got, pitch, plane));
        if (!Same(got, want)) {
            printf("  pitched mismatch: format %u\n", static_cast<unsigned>(format));
        }

        CHECK(Same(got, want));
    }
}

// 직렬 대체: 시작 전·종료 뒤 scheduler, 같은 scheduler의 worker 안에서 부름. 결과는 직렬과 같다.
TEST(TextureBlockParallel_SerialFallbacks) {
    SystemAllocator allocator;
    Array<uint8> input(allocator);
    REQUIRE(MakeInput(TextureBlockFormat::Astc6x6, 3, input));
    Array<uint8> want(allocator);
    REQUIRE(Encode(TextureBlockFormat::Astc6x6, input, TextureBlockOptions{}, want));
    Array<uint8> bcInput(allocator);
    REQUIRE(MakeInput(TextureBlockFormat::Bc3, 4, bcInput));
    Array<uint8> bcWant(allocator);
    REQUIRE(Encode(TextureBlockFormat::Bc3, bcInput, TextureBlockOptions{}, bcWant));

    WorkerScheduler idle(allocator);
    TextureBlockOptions options;
    options.scheduler = &idle;
    options.scratch = &allocator;
    options.minBatchBlocks = 1;
    Array<uint8> got(allocator);
    REQUIRE(Encode(TextureBlockFormat::Astc6x6, input, options, got));
    CHECK(Same(got, want));
    REQUIRE(Encode(TextureBlockFormat::Bc3, bcInput, options, got));
    CHECK(Same(got, bcWant));

    {
        Workers pool(2);
        REQUIRE(pool.started);
        REQUIRE(pool.scheduler.Shutdown());
        pool.started = false;
        options.scheduler = &pool.scheduler;
        REQUIRE(Encode(TextureBlockFormat::Astc6x6, input, options, got));
        CHECK(Same(got, want));
        REQUIRE(Encode(TextureBlockFormat::Bc3, bcInput, options, got));
        CHECK(Same(got, bcWant));
    }

    Workers pool(2);
    REQUIRE(pool.started);
    Array<uint8> nestedOut(allocator);
    NestedEncode nested{&pool.scheduler, &pool.allocator, &input, &nestedOut,
                        ErrorCode::OutOfMemory};
    auto body = Body::TryCreate(allocator, [&nested](WorkerTaskContext&) noexcept {
        TextureBlockOptions inner;
        inner.scheduler = nested.scheduler;
        inner.scratch = nested.allocator;
        inner.minBatchBlocks = 1;
        auto done = Encode(TextureBlockFormat::Astc6x6, *nested.input, inner, *nested.out);
        nested.result = done ? ErrorCode::None : done.Error();
    });
    REQUIRE(body);
    auto task = pool.scheduler.Submit(Move(body.Value()));
    REQUIRE(task);
    auto waited = task.Value().Wait();
    REQUIRE(waited && waited.Value() == platform::WaitResult::Signaled);
    CHECK(nested.result == ErrorCode::None);
    CHECK(Same(nestedOut, want));
}

// scheduler가 있는데 scratch가 없으면 그림 크기와 무관하게 InvalidArgument다(블록 하나 그림도).
TEST(TextureBlockParallel_SchedulerNeedsScratch) {
    Workers pool(2);
    REQUIRE(pool.started);
    uint8 texels[TextureBlockRgbaBytes] = {};
    TextureBlockImage image;
    image.rgba = Span<const uint8>(texels, sizeof(texels));
    image.sizeX = TextureBlockSize;
    image.sizeY = TextureBlockSize;
    TextureBlockOptions options;
    options.scheduler = &pool.scheduler;
    uint8 out[16];
    for (const TextureBlockFormat format : {TextureBlockFormat::Bc1, TextureBlockFormat::Astc4x4}) {
        CHECK(
            EncodeTextureBlocks(format, image, options, Span<uint8>(out, TextureBlockBytes(format)))
                .Error() == ErrorCode::InvalidArgument);
    }
}

// BC1·BC3·BC4·BC5는 호출자의 FTZ·DAZ·0 쪽 반올림 설정과 무관하다(직렬·병렬 — 결정적 FP 환경 안).
TEST(TextureBlockParallel_BcFloatingPointEnvironment) {
    SystemAllocator allocator;
    Workers pool(2);
    REQUIRE(pool.started);
    for (const TextureBlockFormat format : {TextureBlockFormat::Bc1, TextureBlockFormat::Bc3,
                                            TextureBlockFormat::Bc4, TextureBlockFormat::Bc5}) {
        Array<uint8> input(allocator);
        REQUIRE(MakeInput(format, 11, input));
        Array<uint8> plain(allocator);
        REQUIRE(Encode(format, input, TextureBlockOptions{}, plain));
        TextureBlockOptions parallel;
        parallel.scheduler = &pool.scheduler;
        parallel.scratch = &pool.allocator;
        parallel.minBatchBlocks = 1;
        const uint64 original = platform::detail::ReadFloatingPointControl();
        platform::detail::WriteFloatingPointControl(original | platform::detail::FlushToZeroBit() |
                                                    platform::detail::DenormalsAreZeroBit() |
                                                    platform::detail::RoundTowardZeroBits());
        Array<uint8> serialDisturbed(allocator);
        Array<uint8> parallelDisturbed(allocator);
        const bool serialDone =
            static_cast<bool>(Encode(format, input, TextureBlockOptions{}, serialDisturbed));
        const bool parallelDone =
            static_cast<bool>(Encode(format, input, parallel, parallelDisturbed));
        platform::detail::WriteFloatingPointControl(original);
        REQUIRE(serialDone && parallelDone);
        CHECK(Same(serialDisturbed, plain));
        CHECK(Same(parallelDisturbed, plain));
    }
}

// helper 회수 경로: worker를 모두(또는 일부) gate 작업으로 막은 뒤 압축한다. 막힌 worker 몫의
// helper는 실행되지 못하고 호출 스레드가 회수해 나머지를 차례로 실행한다(ASTC는 thread index 여럿을
// 한 스레드가). byte는 직렬과 같다.
TEST(TextureBlockParallel_RevokedHelpers) {
    SystemAllocator allocator;
    Variant variants[16];
    const usize count = Variants(variants);
    for (const uint32 blocked : {4u, 2u}) {
        Workers pool(4);
        REQUIRE(pool.started);
        Blockers blockers(allocator);
        REQUIRE(blockers.Hold(pool.scheduler, pool.allocator, blocked));
        bool same = true;
        for (usize i = 0; i < count; ++i) {
            Array<uint8> input(allocator);
            REQUIRE(MakeInput(variants[i].format, static_cast<uint32>(i + 31), input));
            Array<uint8> want(allocator);
            REQUIRE(Encode(variants[i].format, input, OptionsOf(variants[i]), want));
            TextureBlockOptions options = OptionsOf(variants[i]);
            options.scheduler = &pool.scheduler;
            options.scratch = &pool.allocator;
            options.minBatchBlocks = 1;
            Array<uint8> got(allocator);
            REQUIRE(Encode(variants[i].format, input, options, got));
            same = same && Same(got, want);
        }

        blockers.Release();
        CHECK(same);
    }
}

// 병렬의 scratch 할당 실패: 시도마다 n번째 할당을 실패시킨다. ASTC 사본·slice 포인터 표 실패는
// OutOfMemory이고, helper 작업 할당 실패는 성공(호출 스레드가 실행 — 직렬과 같은 byte)이다.
TEST(TextureBlockParallel_ScratchAllocationFailures) {
    SystemAllocator allocator;
    Workers pool(2);
    REQUIRE(pool.started);
    for (const TextureBlockFormat format :
         {TextureBlockFormat::Bc1, TextureBlockFormat::EacR11, TextureBlockFormat::Astc6x6}) {
        const usize texelBytes = TextureBlockSourceTexelBytes(format);
        const usize pitch = usize{SizeX} * texelBytes + RowPadding;
        const usize plane = pitch * SizeY;
        Array<uint8> padded(allocator);
        REQUIRE(MakeInput(format, 41, padded, pitch, plane));
        TextureBlockOptions serialOptions;
        serialOptions.scratch = &allocator;
        Array<uint8> want(allocator);
        REQUIRE(Encode(format, padded, serialOptions, want, pitch, plane));
        bool finished = false;
        for (uint64 attempt = 1; attempt < 64 && !finished; ++attempt) {
            test::NFailAllocator failing(pool.allocator);
            failing.FailOnAttempt(attempt);
            TextureBlockOptions options;
            options.scheduler = &pool.scheduler;
            options.scratch = &failing;
            options.minBatchBlocks = 1;
            Array<uint8> got(allocator);
            auto done = Encode(format, padded, options, got, pitch, plane);
            if (done) {
                CHECK(Same(got, want));
            } else {
                CHECK(done.Error() == ErrorCode::OutOfMemory);
                CHECK(TextureBlockIsAstc(format));
            }

            finished = failing.Attempts() < attempt;
        }

        CHECK(finished);
    }
}

// 병렬 ASTC·EAC·BC6H·BC7도 호출자의 FTZ·DAZ·0 쪽 반올림과 무관하다(컨텍스트 열기·batch가 결정적 FP
// 환경 안 — 호출자 반올림이면 astcenc가 BAD_CPU_FLOAT를 낸다).
TEST(TextureBlockParallel_FloatingPointEnvironment) {
    SystemAllocator allocator;
    Workers pool(2);
    REQUIRE(pool.started);
    for (const TextureBlockFormat format : {TextureBlockFormat::EacR11, TextureBlockFormat::Astc6x6,
                                            TextureBlockFormat::Bc6h, TextureBlockFormat::Bc7}) {
        if ((format == TextureBlockFormat::Bc6h || format == TextureBlockFormat::Bc7) &&
            !HasIspcEncoder()) {
            continue;
        }

        Array<uint8> input(allocator);
        REQUIRE(MakeInput(format, 51, input));
        Array<uint8> plain(allocator);
        REQUIRE(Encode(format, input, TextureBlockOptions{}, plain));
        TextureBlockOptions parallel;
        parallel.scheduler = &pool.scheduler;
        parallel.scratch = &pool.allocator;
        parallel.minBatchBlocks = 1;
        const uint64 original = platform::detail::ReadFloatingPointControl();
        platform::detail::WriteFloatingPointControl(original | platform::detail::FlushToZeroBit() |
                                                    platform::detail::DenormalsAreZeroBit() |
                                                    platform::detail::RoundTowardZeroBits());
        Array<uint8> disturbed(allocator);
        const bool done = static_cast<bool>(Encode(format, input, parallel, disturbed));
        platform::detail::WriteFloatingPointControl(original);
        REQUIRE(done);
        CHECK(Same(disturbed, plain));
    }
}

// 같은 scheduler의 worker 안에서 BC·EAC(행 병렬 경로)를 불러도 직렬로 대체되어 byte가 같다.
TEST(TextureBlockParallel_RowsInsideWorker) {
    SystemAllocator allocator;
    Workers pool(2);
    REQUIRE(pool.started);
    for (const TextureBlockFormat format : {TextureBlockFormat::Bc5, TextureBlockFormat::EacR11}) {
        Array<uint8> input(allocator);
        REQUIRE(MakeInput(format, 61, input));
        Array<uint8> want(allocator);
        REQUIRE(Encode(format, input, TextureBlockOptions{}, want));
        Array<uint8> got(allocator);
        struct Nested {
            WorkerScheduler* scheduler;
            Allocator* allocator;
            TextureBlockFormat format;
            const Array<uint8>* input;
            Array<uint8>* out;
            ErrorCode result;
        } nested{&pool.scheduler, &pool.allocator, format, &input, &got, ErrorCode::OutOfMemory};
        auto body = Body::TryCreate(allocator, [&nested](WorkerTaskContext&) noexcept {
            TextureBlockOptions inner;
            inner.scheduler = nested.scheduler;
            inner.scratch = nested.allocator;
            inner.minBatchBlocks = 1;
            auto done = Encode(nested.format, *nested.input, inner, *nested.out);
            nested.result = done ? ErrorCode::None : done.Error();
        });
        REQUIRE(body);
        auto task = pool.scheduler.Submit(Move(body.Value()));
        REQUIRE(task);
        auto waited = task.Value().Wait();
        REQUIRE(waited && waited.Value() == platform::WaitResult::Signaled);
        CHECK(nested.result == ErrorCode::None);
        CHECK(Same(got, want));
    }
}

} // namespace texture_block_parallel_tests_file
