#include "TextureBlockEacAstc.h"
#include "TextureBlockParallel.h"

#include <Base/Compiler.h>
#include <Base/NumericLimits.h>
#include <Core/Memory/Allocator.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Task/ParallelFor.h>
#include <TextureTools/TextureBlockEncode.h>
#include <astcenc.h>

// ASTC 인코더다(Docs/Design/AssetSystem 12.17.8 2·3 — UE 5.8 TextureFormatASTC·astc_thunk와 같은
// 설정). ThirdParty AstcEncoder(astc-encoder 5.1.0 불변성 빌드 + Rocks.patch)를 텍스처 호출 하나에
// 컨텍스트 하나로 부른다. 블록이 2D(z = 1)라 slice는 서로 독립이다. 직렬은 slice마다 압축하고
// reset하고, 병렬(12.17.9 2)은 thread_count = participants 컨텍스트에 slice 묶음을 dim_z로 넘긴다.
namespace rocks {

namespace {

// 유니티 빌드에서 같은 묶음의 이름과 부딪히지 않게 이름 있는 namespace에 둔다.
namespace astc_encode {

constexpr uint32 TexelBytes = 4;
// ASTC 블록 byte 수다.
constexpr uint64 AstcBlockBytes = 16;
// UE DefaultASTCQualityBySpeed 기본 2 → ASTCENC_PRE_MEDIUM이다(12.17.8 3).
// (astcenc.h의 preset은 constexpr가 아닌 static const다.)
const float EncodeQuality = ASTCENC_PRE_MEDIUM;
// 노멀 LA의 tune_db_limit 하한이다(UE bDbLimitGreaterThan60).
constexpr float NormalDbLimit = 60.0f;
// 직렬 컨텍스트 스레드 수다.
constexpr unsigned int SerialThreads = 1;
// 병렬 컨텍스트 스레드 수 상한이다(WorkerScheduler worker 상한 64 + 호출 스레드).
constexpr uint32 MaxThreads = 65;
// astcenc가 한 번에 나눠 주는 블록 수다(astcenc_entry.cpp 858 — participant 하나가 맡을 최소 일).
constexpr uint64 AstcGranuleBlocks = 16;
// astcenc 호출 하나의 블록 수 상한이다. astcenc는 블록 수·출력 offset을 int로
// 센다(astcenc_entry.cpp 819·936 — offset = 블록 번호 × 16 byte가 2^31을 넘으면 넘친다).
constexpr uint64 AstcMaxCallBlocks = (uint64{1} << 31) / 16 - 1;

ErrorCode MapError(astcenc_error error) noexcept {
    // 12.17.8 3 오류 표: 출력 버퍼 부족은 감싸기가 크기를 맞추므로 내부 오류(InvalidArgument), CPU
    // 부동소수 설정 오류는 Unsupported, 나머지 설정 오류는 InvalidArgument다.
    return error == ASTCENC_ERR_BAD_CPU_FLOAT ? ErrorCode::Unsupported : ErrorCode::InvalidArgument;
}

struct Context {
    astcenc_context* handle = nullptr;

    Context() noexcept = default;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    ~Context() noexcept {
        if (handle != nullptr) {
            astcenc_context_free(handle);
        }
    }
};

struct SliceBuffer {
    Allocator* allocator = nullptr;
    void* data = nullptr;

    SliceBuffer() noexcept = default;
    SliceBuffer(const SliceBuffer&) = delete;
    SliceBuffer& operator=(const SliceBuffer&) = delete;

    ~SliceBuffer() noexcept {
        if (data != nullptr) {
            (void)allocator->Deallocate(data);
        }
    }
};

// 설정과 컨텍스트 할당도 결정적 부동소수 환경 안에서 한다(astcenc_config_init의 preset 보간,
// astcenc_context_alloc의 CPU 부동소수 검사 — 호출자의 반올림 방향이면 BAD_CPU_FLOAT다).
astcenc_error Configure(TextureBlockFormat format, bool srgb, TextureAstcMode mode, bool decodeOnly,
                        astcenc_config& config) noexcept {
    const uint32 edge = TextureBlockWidth(format);
    unsigned int flags = 0;
    if (mode == TextureAstcMode::NormalLa) {
        flags |= ASTCENC_FLG_MAP_NORMAL;
    }

    if (decodeOnly) {
        flags |= ASTCENC_FLG_DECOMPRESS_ONLY;
    }

    const astcenc_profile profile = srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR;
    const astcenc_error status =
        astcenc_config_init(profile, edge, edge, 1, EncodeQuality, flags, &config);
    if (status != ASTCENC_SUCCESS) {
        return status;
    }

    if (mode == TextureAstcMode::NormalLa) {
        if (config.tune_db_limit < NormalDbLimit) {
            config.tune_db_limit = NormalDbLimit;
        }

        config.cw_r_weight = 1.0f;
        config.cw_g_weight = 0.0f;
        config.cw_b_weight = 0.0f;
        config.cw_a_weight = 1.0f;
    }

    return ASTCENC_SUCCESS;
}

// 설정·컨텍스트 할당의 결과를 Rocks 오류로 바꾼다(할당 실패는 OutOfMemory).
ErrorCode Open(TextureBlockFormat format, bool srgb, TextureAstcMode mode, bool decodeOnly,
               unsigned int threads, Context& context) noexcept {
    astcenc_config config{};
    astcenc_error status = Configure(format, srgb, mode, decodeOnly, config);
    if (status != ASTCENC_SUCCESS) {
        return MapError(status);
    }

    status = astcenc_context_alloc(&config, threads, &context.handle);
    if (status != ASTCENC_SUCCESS) {
        return status == ASTCENC_ERR_OUT_OF_MEM ? ErrorCode::OutOfMemory : MapError(status);
    }

    return ErrorCode::None;
}

// 입력 RGBA8 → astcenc swizzle이다(UE의 BGRA 입력 swizzle을 RGBA로 옮겼다).
astcenc_swizzle SwizzleFor(TextureAstcMode mode) noexcept {
    switch (mode) {
    case TextureAstcMode::Rgb:
        return astcenc_swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_1};
    case TextureAstcMode::NormalLa:
        // L+A: R = G = B = X(입력 R), A = Y(입력 G).
        return astcenc_swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_R, ASTCENC_SWZ_R, ASTCENC_SWZ_G};
    case TextureAstcMode::Rgba:
        break;
    }

    return astcenc_swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
}

struct Job {
    TextureBlockFormat format;
    TextureBlockOptions options;
    const uint8* base;
    uint32 sizeX;
    uint32 sizeY;
    uint32 numSlices;
    uint64 rowPitch;
    uint64 slicePitch;
    uint8* packed;
    uint8* out;
    uint64 sliceOutBytes;
    ErrorCode error;
};

// 결정적 부동소수 환경 안에서 돈다(SSE2 round의 가장 가까운 짝수 전제 — 12.17.8 2).
void Run(void* opaque) noexcept {
    Job& job = *static_cast<Job*>(opaque);
    Context context;
    job.error =
        Open(job.format, job.options.srgb, job.options.astcMode, false, SerialThreads, context);
    if (job.error != ErrorCode::None) {
        return;
    }

    const astcenc_swizzle swizzle = SwizzleFor(job.options.astcMode);
    const uint64 rowBytes = uint64{job.sizeX} * TexelBytes;
    for (uint32 slice = 0; slice < job.numSlices; ++slice) {
        const uint8* source = job.base + job.slicePitch * slice;
        void* data = const_cast<uint8*>(source);
        if (job.packed != nullptr) {
            for (uint32 y = 0; y < job.sizeY; ++y) {
                const uint8* line = source + job.rowPitch * y;
                uint8* target = job.packed + rowBytes * y;
                for (uint64 k = 0; k < rowBytes; ++k) {
                    target[k] = line[k];
                }
            }

            data = job.packed;
        }

        astcenc_image image{job.sizeX, job.sizeY, 1, ASTCENC_TYPE_U8, &data};
        astcenc_error status = astcenc_compress_image(context.handle, &image, &swizzle,
                                                      job.out + job.sliceOutBytes * slice,
                                                      static_cast<size_t>(job.sliceOutBytes), 0);
        if (status == ASTCENC_SUCCESS) {
            status = astcenc_compress_reset(context.handle);
        }

        if (status != ASTCENC_SUCCESS) {
            job.error = MapError(status);
            return;
        }
    }
}

struct DecodeJob {
    TextureBlockFormat format;
    bool srgb;
    const uint8* blocks;
    uint32 sizeX;
    uint32 sizeY;
    uint32 numSlices;
    uint64 sliceBlockBytes;
    uint8* rgba;
    ErrorCode error;
};

void RunDecode(void* opaque) noexcept {
    DecodeJob& job = *static_cast<DecodeJob*>(opaque);
    Context context;
    job.error = Open(job.format, job.srgb, TextureAstcMode::Rgba, true, SerialThreads, context);
    if (job.error != ErrorCode::None) {
        return;
    }

    const astcenc_swizzle identity{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    const uint64 sliceBytes = uint64{job.sizeX} * job.sizeY * TexelBytes;
    for (uint32 slice = 0; slice < job.numSlices; ++slice) {
        void* data = job.rgba + sliceBytes * slice;
        astcenc_image image{job.sizeX, job.sizeY, 1, ASTCENC_TYPE_U8, &data};
        astcenc_error status = astcenc_decompress_image(
            context.handle, job.blocks + job.sliceBlockBytes * slice,
            static_cast<size_t>(job.sliceBlockBytes), &image, &identity, 0);
        if (status == ASTCENC_SUCCESS) {
            status = astcenc_decompress_reset(context.handle);
        }

        if (status != ASTCENC_SUCCESS) {
            job.error = MapError(status);
            return;
        }
    }
}

// 병렬 압축의 컨텍스트 열기다(결정적 FP 환경 안 — 호출 스레드).
struct OpenJob {
    TextureBlockFormat format;
    const TextureBlockOptions* options;
    unsigned int threads;
    Context* context;
    ErrorCode error;
};

void RunOpen(void* opaque) noexcept {
    OpenJob& job = *static_cast<OpenJob*>(opaque);
    job.error = Open(job.format, job.options->srgb, job.options->astcMode, false, job.threads,
                     *job.context);
}

// 병렬 압축에서 participant 하나의 몫이다(thread_index — astcenc가 16블록 단위로 동적으로 나눈다).
struct ThreadJob {
    astcenc_context* context;
    astcenc_image* image;
    const astcenc_swizzle* swizzle;
    uint8* out;
    usize outBytes;
    unsigned int threadIndex;
    astcenc_error status;
};

void RunThread(void* opaque) noexcept {
    ThreadJob& job = *static_cast<ThreadJob*>(opaque);
    job.status = astcenc_compress_image(job.context, job.image, job.swizzle, job.out, job.outBytes,
                                        job.threadIndex);
}

// 병렬 압축이다(12.17.9 2 — UE astc_thunk처럼 컨텍스트 하나에 thread_count = participants). 그림은
// dim_z = slice 수와 slice 포인터 배열로 한 번에 넘기고, 끝나면 reset을 명시적으로
// 부른다(thread_count > 1이면 자동 reset이 없다).
Result<void> EncodeParallel(TextureBlockFormat format, const uint8* base, uint32 sizeX,
                            uint32 sizeY, uint32 numSlices, uint64 rowPitch, uint64 slicePitch,
                            const TextureBlockOptions& options, uint32 participants,
                            Span<uint8> out) noexcept {
    Allocator& scratch = *options.scratch;
    const uint64 rowBytes = uint64{sizeX} * TexelBytes;
    const uint64 planeBytes = rowBytes * sizeY;
    // 행 여백 입력은 호출 스레드에서 한 번 빽빽한 사본을 만든다.
    SliceBuffer packed;
    if (rowPitch != rowBytes) {
        if (planeBytes > NumericLimits<usize>::Max() / numSlices) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        auto allocated =
            scratch.Allocate(static_cast<usize>(planeBytes * numSlices), alignof(uint32));
        if (!allocated) {
            return Result<void>::Failure(allocated.Error());
        }

        packed.allocator = &scratch;
        packed.data = allocated.Value();
        uint8* target = static_cast<uint8*>(packed.data);
        for (uint32 slice = 0; slice < numSlices; ++slice) {
            for (uint32 y = 0; y < sizeY; ++y) {
                const uint8* line = base + slicePitch * slice + rowPitch * y;
                uint8* into = target + planeBytes * slice + rowBytes * y;
                for (uint64 k = 0; k < rowBytes; ++k) {
                    into[k] = line[k];
                }
            }
        }
    }

    SliceBuffer pointers;
    auto table = scratch.Allocate(sizeof(void*) * usize{numSlices}, alignof(void*));
    if (!table) {
        return Result<void>::Failure(table.Error());
    }

    pointers.allocator = &scratch;
    pointers.data = table.Value();
    void** slices = static_cast<void**>(pointers.data);
    for (uint32 slice = 0; slice < numSlices; ++slice) {
        slices[slice] =
            packed.data != nullptr
                ? static_cast<void*>(static_cast<uint8*>(packed.data) + planeBytes * slice)
                : const_cast<uint8*>(base + slicePitch * slice);
    }

    Context context;
    OpenJob open{format, &options, participants, &context, ErrorCode::None};
    platform::RunWithDeterministicFloatingPoint(&RunOpen, &open);
    if (open.error != ErrorCode::None) {
        return Result<void>::Failure(open.error);
    }

    // slice 묶음마다 astcenc 호출 하나다(묶음의 블록 수가 AstcMaxCallBlocks 이하 — 호출자가 slice
    // 하나는 넘지 않음을 확인했다). 묶음이 끝날 때마다 단일 스레드 지점에서 reset한다(reset은
    // 카운터 대입이라 결정적 FP 환경 밖이어도 된다).
    const uint64 sliceBlocks = out.Size() / AstcBlockBytes / numSlices;
    const uint32 chunkSlices = static_cast<uint32>(
        AstcMaxCallBlocks / sliceBlocks < numSlices ? AstcMaxCallBlocks / sliceBlocks : numSlices);
    const astcenc_swizzle swizzle = SwizzleFor(options.astcMode);
    ParallelForOptions parallel;
    parallel.minBatchSize = 1;
    parallel.maxHelpers = participants - 1;
    for (uint32 first = 0; first < numSlices; first += chunkSlices) {
        const uint32 count = numSlices - first < chunkSlices ? numSlices - first : chunkSlices;
        astcenc_image image{sizeX, sizeY, count, ASTCENC_TYPE_U8, slices + first};
        uint8* chunkOut = out.Data() + sliceBlocks * AstcBlockBytes * first;
        const usize chunkBytes = static_cast<usize>(sliceBlocks * AstcBlockBytes * count);
        auto done = ParallelFor(
            *options.scheduler, scratch, participants,
            [&](usize begin, usize end) noexcept -> ErrorCode {
                for (usize index = begin; index < end; ++index) {
                    ThreadJob job{context.handle, &image,     &swizzle,
                                  chunkOut,       chunkBytes, static_cast<unsigned int>(index),
                                  ASTCENC_SUCCESS};
                    platform::RunWithDeterministicFloatingPoint(&RunThread, &job);
                    if (job.status != ASTCENC_SUCCESS) {
                        return MapError(job.status);
                    }
                }

                return ErrorCode::None;
            },
            parallel);
        (void)astcenc_compress_reset(context.handle);
        if (!done) {
            return done;
        }
    }

    return Result<void>::Success();
}

} // namespace astc_encode

} // namespace

StringView AstcEncoderIdentity() noexcept {
    // SIMD 경로(x64 SSE2·Arm64 NEON)를 신원에 넣어 아키텍처 사이 너겟 공유를 막는다 — 두 경로의
    // byte가 같음을 Mac에서 확인하면 빼고 AstcEncoderVersion을 올린다(12.17.8 2).
#if ROCKS_ARCH_X64
    return "astcenc 5.1.0 medium invariant rocks-patch 1 x64-sse2";
#else
    return "astcenc 5.1.0 medium invariant rocks-patch 1 arm64-neon";
#endif
}

Result<void> DecodeAstcBlocks(TextureBlockFormat format, Span<const uint8> blocks, uint32 sizeX,
                              uint32 sizeY, uint32 numSlices, bool srgb,
                              Span<uint8> rgba) noexcept {
    using namespace astc_encode;
    if (!TextureBlockIsAstc(format)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto blockBytes = TextureBlockOutputBytes(format, sizeX, sizeY, numSlices);
    if (!blockBytes) {
        return Result<void>::Failure(blockBytes.Error());
    }

    // 출력 texel byte는 블록 byte보다 크므로 따로 넘침을 본다.
    const uint64 sliceTexels = uint64{sizeX} * sizeY;
    if (sliceTexels > NumericLimits<uint64>::Max() / TexelBytes / numSlices) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    if (blocks.Size() != blockBytes.Value() ||
        rgba.Size() != sliceTexels * TexelBytes * numSlices) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    DecodeJob job{format,
                  srgb,
                  blocks.Data(),
                  sizeX,
                  sizeY,
                  numSlices,
                  blockBytes.Value() / numSlices,
                  rgba.Data(),
                  ErrorCode::None};
    platform::RunWithDeterministicFloatingPoint(&RunDecode, &job);
    if (job.error != ErrorCode::None) {
        return Result<void>::Failure(job.error);
    }

    return Result<void>::Success();
}

namespace texture_block_detail {

Result<void> EncodeAstcBlocks(TextureBlockFormat format, const uint8* base, uint32 sizeX,
                              uint32 sizeY, uint32 numSlices, uint64 rowPitch, uint64 slicePitch,
                              const TextureBlockOptions& options, Span<uint8> out) noexcept {
    using namespace astc_encode;
    // slice 하나가 astcenc 호출 상한을 넘으면 압축할 수 없다(직렬·병렬 같은 판정).
    const uint64 sliceBlocks = out.Size() / AstcBlockBytes / numSlices;
    if (sliceBlocks > AstcMaxCallBlocks) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    // participant 하나가 astcenc 몫(16블록)을 하나 이상 맡을 때만 병렬로 한다(작은 mip은 직렬).
    const uint64 totalBlocks = sliceBlocks * numSlices;
    uint32 participants = BlockParticipants(options, MaxThreads);
    const uint64 useful = (totalBlocks + AstcGranuleBlocks - 1) / AstcGranuleBlocks;
    participants = useful < participants ? static_cast<uint32>(useful) : participants;
    if (participants > 1) {
        return EncodeParallel(format, base, sizeX, sizeY, numSlices, rowPitch, slicePitch, options,
                              participants, out);
    }

    // astcenc 입력은 행 사이 여백이 없어야 한다(astcenc_image.cpp 224). 여백이 있으면 slice 하나
    // 크기의 빽빽한 사본을 만든다.
    const uint64 rowBytes = uint64{sizeX} * TexelBytes;
    SliceBuffer packed;
    if (rowPitch != rowBytes) {
        if (options.scratch == nullptr) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const uint64 bytes = rowBytes * sizeY;
        if (bytes > NumericLimits<usize>::Max()) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        auto allocated = options.scratch->Allocate(static_cast<usize>(bytes), alignof(uint32));
        if (!allocated) {
            return Result<void>::Failure(allocated.Error());
        }

        packed.allocator = options.scratch;
        packed.data = allocated.Value();
    }

    Job job{format,
            options,
            base,
            sizeX,
            sizeY,
            numSlices,
            rowPitch,
            slicePitch,
            static_cast<uint8*>(packed.data),
            out.Data(),
            out.Size() / numSlices,
            ErrorCode::None};
    platform::RunWithDeterministicFloatingPoint(&Run, &job);
    if (job.error != ErrorCode::None) {
        return Result<void>::Failure(job.error);
    }

    return Result<void>::Success();
}

} // namespace texture_block_detail

} // namespace rocks
