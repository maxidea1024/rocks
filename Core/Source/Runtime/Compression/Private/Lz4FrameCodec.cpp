#include "CompressionInternal.h"

#include <Base/Endian.h>
#include <Core/Move.h>
#include <Platform/Memory.h>

// LZ4 1.11.1 frame 형식(lz4frame, LZ4 Frame Format 1.6.1) adapter다.
// LZ4F_STATIC_LINKING_ONLY(사용자 할당기·오류 코드 열거)는 모듈 규칙이 ThirdParty.json의
// consumerDefines에서 정의한다. 모든 문맥은 _advanced 생성 함수에 Allocator 경유 할당·해제 함수를
// 넘겨 만든다. 기본 생성 함수와 LZ4F_compressFrame은 CRT malloc을 쓰므로 부르지 않는다. frame
// 설정과 근거는 Docs/Design/Compression.ko.md 12절이다.
#include <lz4frame.h>

namespace rocks::detail::lz4frame {
namespace {

static_assert(LZ4F_MAGICNUMBER == 0x184D2204u, "LZ4 frame magic");
static_assert(LZ4F_MAGIC_SKIPPABLE_START == 0x184D2A50u, "LZ4 skippable frame magic");

// 블록 최대 크기(LZ4F_max64KB)다. linked 블록은 앞 블록의 64KiB를 참조하므로(LZ4 offset 상한과
// 같다) 큰 블록과 압축률이 거의 같고, 해제 문맥 메모리는 약 256KiB로 작다(4MiB 블록이면 약 8.1MiB).
constexpr usize BlockBytes = usize{64} << 10;

// 일회성 압축은 분할 단위마다 블록을 끝까지 압축한다(autoFlush). 분할 단위가 블록 크기의 배수여야
// 블록 경계가 입력을 한 번에 넣은 것과 같다(동기·비동기와 upstream LZ4F_compressFrame이 같은 byte).
static_assert(CompressionChunkBytes % BlockBytes == 0,
              "compression chunk must be a multiple of the LZ4 frame block size");

// 스트림 압축이 LZ4F_compressUpdate에 한 번에 넘기는 입력 상한이다. 블록보다 작게 넣으면 모든
// 블록이 LZ4F 내부 버퍼에서 완성되어 압축되므로 결과 byte가 입력 조각 나누기·출력 창 크기와
// 무관하게 내용과 flush 위치로만 정해진다. 호출 한 번은 블록을 많아야 하나 끝낸다.
constexpr usize StreamPieceBytes = BlockBytes - 1;

// Append 해제가 원본 크기를 모를 때 처음 확보하는 출력이다.
constexpr usize AppendInitialBytes = usize{64} << 10;

// frame 헤더 크기를 알려면 magic(4byte)과 FLG(1byte)가 필요하다(LZ4F_headerSize).
constexpr usize FrameProbeBytes = LZ4F_MIN_SIZE_TO_KNOW_HEADER_LENGTH;
static_assert(LZ4F_HEADER_SIZE_MAX <= sizeof(StreamDecodeState::header),
              "stream state must hold a whole LZ4 frame header");

// frame 헤더의 원본 크기 필드 위치(magic 4 + FLG 1 + BD 1)와 FLG 표지다.
constexpr usize ContentSizeOffset = 6;
constexpr uint8 ContentSizeFlag = 0x08;
constexpr uint8 DictionaryIdFlag = 0x01;

usize Minimum(usize first, usize second) noexcept {
    return first < second ? first : second;
}

usize Maximum(usize first, usize second) noexcept {
    return first > second ? first : second;
}

void* Allocate(void* opaque, size_t bytes) {
    return CodecAllocate(*static_cast<Allocator*>(opaque), bytes);
}

void Free(void* opaque, void* address) {
    CodecFree(*static_cast<Allocator*>(opaque), address);
}

// calloc은 비워 둔다. LZ4F는 customAlloc으로 받은 뒤 0으로 채운다. CRT 대체 경로는 할당·해제 함수가
// 모두 없을 때만 타므로 이 설정에서는 실행되지 않는다.
LZ4F_CustomMem Memory(Allocator& allocator) noexcept {
    return LZ4F_CustomMem{&Allocate, nullptr, &Free, &allocator};
}

// frame 설정이다(12절). content checksum은 무결성(zstd checksum·gzip CRC와 같은 역할)이고, 블록
// checksum은 같은 손상을 content checksum이 잡으므로 끈다. 일회성은 원본 크기를 기록하고 블록을
// 입력 위치에서 바로 압축한다(autoFlush). 스트림은 원본 크기를 모르고 블록을 채운 뒤 압축한다.
LZ4F_preferences_t Preferences(int32 level, uint64 contentSize, bool autoFlush) noexcept {
    LZ4F_preferences_t preferences{};
    preferences.frameInfo.blockSizeID = LZ4F_max64KB;
    preferences.frameInfo.blockMode = LZ4F_blockLinked;
    preferences.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    preferences.frameInfo.frameType = LZ4F_frame;
    preferences.frameInfo.contentSize = contentSize;
    preferences.frameInfo.dictID = 0;
    preferences.frameInfo.blockChecksumFlag = LZ4F_noBlockChecksum;
    // Rocks 수준과 LZ4F 수준은 같다: 1 = fast(acceleration 1), 2 = lz4mid, 3–9 = HC, 10–12 = HC
    // optimal parser.
    preferences.compressionLevel = level;
    preferences.autoFlush = autoFlush ? 1u : 0u;
    preferences.favorDecSpeed = 0;
    preferences.nbWorkers = 0;
    return preferences;
}

// 압축 경로의 오류다. 입력 데이터가 원인일 수 없으므로 출력 부족·OOM 외에는 계약 밖의 실패다.
ErrorCode CompressionError(size_t code) noexcept {
    switch (LZ4F_getErrorCode(code)) {
    case LZ4F_ERROR_dstCapacity_tooSmall:
        return ErrorCode::CapacityExceeded;
    case LZ4F_ERROR_allocation_failed:
        return ErrorCode::OutOfMemory;
    default:
        return ErrorCode::DependencyFailed;
    }
}

// 해제 경로의 오류다. adapter가 API 계약을 어긴 경우 외에는 입력 손상이다(frame·블록 헤더, 블록
// 해제, 원본 크기, checksum 오류와 GENERIC).
ErrorCode DecompressionError(size_t code) noexcept {
    switch (LZ4F_getErrorCode(code)) {
    case LZ4F_ERROR_allocation_failed:
        return ErrorCode::OutOfMemory;
    case LZ4F_ERROR_parameter_null:
    case LZ4F_ERROR_parameter_invalid:
    case LZ4F_ERROR_compressionLevel_invalid:
    case LZ4F_ERROR_srcPtr_wrong:
    case LZ4F_ERROR_dstCapacity_tooSmall:
    case LZ4F_ERROR_frameDecoding_alreadyStarted:
    case LZ4F_ERROR_compressionState_uninitialized:
    case LZ4F_ERROR_io_write:
    case LZ4F_ERROR_io_read:
        return ErrorCode::DependencyFailed;
    default:
        return ErrorCode::InvalidArgument;
    }
}

// frame 시작 byte(받은 만큼, 최대 4byte)가 LZ4 frame·skippable frame·legacy frame magic의
// 앞부분인지 확인한다. 아니면 손상(InvalidArgument)이고, legacy frame은 아는 형식이지만 지원하지
// 않아 Unsupported다.
Result<void> CheckMagic(const uint8* bytes, usize size) noexcept {
    static constexpr uint8 FrameMagic[4] = {0x04, 0x22, 0x4D, 0x18};
    // 첫 byte의 하위 4bit는 skippable frame 종류(0–15)다.
    static constexpr uint8 SkippableMagic[4] = {0x50, 0x2A, 0x4D, 0x18};
    // legacy frame(`lz4 -l`) magic 0x184C2102다.
    static constexpr uint8 Legacy[4] = {0x02, 0x21, 0x4C, 0x18};
    bool frame = true;
    bool skippable = true;
    bool legacy = true;
    const usize known = Minimum(size, 4);
    for (usize index = 0; index < known; ++index) {
        frame = frame && bytes[index] == FrameMagic[index];
        const uint8 mask = index == 0 ? 0xF0 : 0xFF;
        skippable = skippable && (bytes[index] & mask) == SkippableMagic[index];
        legacy = legacy && bytes[index] == Legacy[index];
    }
    if (legacy && known == 4) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }
    if (!frame && !skippable && !legacy) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

// frame 헤더 전체(LZ4F_headerSize byte)만 LZ4F에 먼저 넣는다. LZ4F가 version·예약 bit·블록
// 크기·헤더 checksum을 검증하고 블록 버퍼를 준비한다(블록은 아직 풀지 않는다). 검증을 통과한 LZ4
// frame이 사전 ID를 기록했으면 Unsupported다(사전은 zstd만 지원하므로 zstd 사전 ID frame·zlib
// FDICT와 같은 분류다). 그래서 FLG가 손상된 frame은 사전 frame으로 잘못 분류되지 않는다. 헤더만으로
// 끝나는 frame(빈 skippable frame)이면 true다.
Result<bool> OpenFrame(LZ4F_dctx* context, const uint8* header, usize headerBytes) noexcept {
    uint8 none = 0;
    size_t read = headerBytes;
    size_t written = 0;
    const size_t hint = LZ4F_decompress(context, &none, &written, header, &read, nullptr);
    if (LZ4F_isError(hint)) {
        return Result<bool>::Failure(DecompressionError(hint));
    }
    if (read != headerBytes || written != 0) {
        return Result<bool>::Failure(ErrorCode::DependencyFailed);
    }
    if (LoadLittleEndian<uint32>(header) == LZ4F_MAGICNUMBER &&
        (header[4] & DictionaryIdFlag) != 0) {
        return Result<bool>::Failure(ErrorCode::Unsupported);
    }

    return Result<bool>::Success(hint == 0);
}

// 첫 frame 헤더에 기록된 원본 크기다(없으면 0). Append 해제가 처음 확보할 출력의 힌트로만 쓴다.
// 거짓 크기는 LZ4F가 frame 끝에서 frameSize_wrong으로 거부한다.
uint64 DeclaredContentSize(Span<const uint8> source) noexcept {
    if (source.Size() < ContentSizeOffset + 8 ||
        LoadLittleEndian<uint32>(source.Data()) != LZ4F_MAGICNUMBER ||
        (source[4] & ContentSizeFlag) == 0) {
        return 0;
    }

    return LoadLittleEndian<uint64>(source.Data() + ContentSizeOffset);
}

// 압축 입력 byte당 해제 출력의 넉넉한 상한이다(LZ4 sequence는 압축 byte당 255byte를 넘지 못한다).
// Append 해제의 첫 확보량을 묶는 데만 쓴다(악성 헤더가 원본 크기를 크게 적어도 입력에 비례한 만큼만
// 먼저 잡는다).
usize MaximumExpansion(usize sourceBytes) noexcept {
    const usize slack = usize{64} << 10;
    if (sourceBytes > (static_cast<usize>(-1) - slack) / 256) {
        return static_cast<usize>(-1);
    }
    return sourceBytes * 256 + slack;
}

// 압축 문맥이다. 스트림 문맥만 단계 버퍼를 가진다(일회성은 nullptr).
struct FrameCompressor {
    LZ4F_cctx* context = nullptr;
    Allocator* allocator = nullptr;
    int32 level = 0;
    // 스트림 frame 설정이다.
    LZ4F_preferences_t preferences{};
    // 스트림 전용: 호출자 창이 LZ4F 호출 한 번의 최악 출력보다 작으면 이 버퍼에 쓴 뒤 나눠 넘긴다.
    // [stageBegin, stageEnd)가 아직 넘기지 않은 출력이다.
    uint8* stage = nullptr;
    usize stageCapacity = 0;
    usize stageBegin = 0;
    usize stageEnd = 0;
    // frame 헤더를 썼는지, frame을 끝냈는지(LZ4F_compressEnd 완료)다.
    bool begun = false;
    bool ended = false;
};

Result<FrameCompressor*> CreateState(int32 level, Allocator& allocator) noexcept {
    void* memory = CodecAllocate(allocator, sizeof(FrameCompressor));
    if (memory == nullptr) {
        return Result<FrameCompressor*>::Failure(ErrorCode::OutOfMemory);
    }

    FrameCompressor* state = ConstructAt<FrameCompressor>(memory);
    state->allocator = &allocator;
    state->level = level;
    // 생성 함수의 실패는 문맥 할당 실패뿐이다. 블록 버퍼·LZ4 상태는 첫 frame을 시작할 때 할당한다.
    state->context = LZ4F_createCompressionContext_advanced(Memory(allocator), LZ4F_VERSION);
    if (state->context == nullptr) {
        DestroyAt(state);
        CodecFree(allocator, state);
        return Result<FrameCompressor*>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<FrameCompressor*>::Success(state);
}

// 해제 출력 대상이다. fixed면 Span, 아니면 Array 뒤를 상한까지 늘린다.
struct FrameOutput {
    uint8* fixed = nullptr;
    Array<uint8>* array = nullptr;
    usize base = 0;
    usize limit = 0;
    usize produced = 0;
    usize writable = 0;

    uint8* Cursor() noexcept {
        if (array != nullptr) {
            return array->Data() + base + produced;
        }
        return fixed + produced;
    }

    // 쓸 공간을 bytes만큼 늘린다(Array만, 상한 안에서). 늘린 구간은 해제가 채우므로 0으로 채우지
    // 않는다.
    Result<void> Grow(usize bytes) noexcept {
        auto grown = array->TryAppendUninitialized(bytes);
        if (!grown) {
            return Result<void>::Failure(grown.Error());
        }
        writable += bytes;
        return Result<void>::Success();
    }

    // 예비 창에 1byte가 나왔다. 상한 안이면(Append) 출력을 두 배씩(최소 64KiB) 늘리고 그 byte를
    // 옮긴다.
    Result<void> AcceptSpare(uint8 spare) noexcept {
        if (array == nullptr || produced == limit) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        auto grown = Grow(Minimum(Maximum(produced, AppendInitialBytes), limit - produced));
        if (!grown) {
            return grown;
        }
        *Cursor() = spare;
        ++produced;
        --writable;
        return Result<void>::Success();
    }
};

// 일회성 해제 공통 루프다.
// - 입력·출력을 분할 단위로 넣어 취소를 확인한다.
// - 연속 frame과 skippable frame을 모두 처리한다. frame 시작마다 magic을 확인하고 헤더를 먼저
//   검증한다(OpenFrame: 손상 InvalidArgument, 사전 ID frame Unsupported).
// - 쓸 공간이 없으면 1byte 예비 창으로 계속 진행한다. 1byte라도 나오면 출력을 늘리거나(Append) 상한
//   초과이고, frame 끝(endmark·checksum)만 남았으면 출력 없이 끝난다. 그래서 정확히 상한 크기인
//   결과는 성공한다.
// - 입력을 모두 넣었는데 frame이 끝나지 않았고 출력 공간도 남았으면 절단(InvalidArgument)이다.
//   frame 뒤의 쓰레기는 magic 검사나 절단으로 InvalidArgument다.
// stableOutput: 출력이 호출 사이에 옮겨지지 않아(Span) 이전 출력이 다음 창 바로 앞에 그대로 있다.
// LZ4F가 history를 내부 버퍼로 복사하지 않아도 된다(stableDst).
Result<void> Decode(LZ4F_dctx* context, Span<const uint8> source, FrameOutput& output,
                    bool stableOutput, const CompressionProbe& probe) noexcept {
    LZ4F_resetDecompressionContext(context);
    const uint8* input = NonNullSource(source);
    usize consumed = 0;
    bool frameStart = true;
    uint8 spare = 0;
    while (true) {
        if (probe.IsCancelled()) {
            return Result<void>::Failure(ErrorCode::Cancelled);
        }

        if (frameStart) {
            if (consumed == source.Size()) {
                return Result<void>::Success();
            }
            const usize remaining = source.Size() - consumed;
            auto checked = CheckMagic(input + consumed, remaining);
            if (!checked) {
                return checked;
            }
            frameStart = false;
            // 헤더 전체가 있으면 먼저 검증한다. 모자라면 LZ4F가 받은 만큼 보관하고 절단으로 끝난다.
            if (remaining >= FrameProbeBytes) {
                const size_t headerBytes = LZ4F_headerSize(input + consumed, remaining);
                if (LZ4F_isError(headerBytes)) {
                    return Result<void>::Failure(DecompressionError(headerBytes));
                }
                if (headerBytes <= remaining) {
                    auto opened = OpenFrame(context, input + consumed, headerBytes);
                    if (!opened) {
                        return Result<void>::Failure(opened.Error());
                    }
                    consumed += headerBytes;
                    frameStart = opened.Value();
                    continue;
                }
            }
        }

        const bool probing = output.writable == 0;
        const usize inputChunk = Minimum(source.Size() - consumed, CompressionChunkBytes);
        const usize outputChunk = probing ? 1 : Minimum(output.writable, CompressionChunkBytes);
        size_t read = inputChunk;
        size_t written = outputChunk;
        LZ4F_decompressOptions_t options{};
        options.stableDst = stableOutput && !probing ? 1u : 0u;
        const size_t hint = LZ4F_decompress(context, probing ? &spare : output.Cursor(), &written,
                                            input + consumed, &read, &options);
        consumed += read;
        if (LZ4F_isError(hint)) {
            return Result<void>::Failure(DecompressionError(hint));
        }

        if (!probing) {
            output.produced += written;
            output.writable -= written;
        } else if (written != 0) {
            auto accepted = output.AcceptSpare(spare);
            if (!accepted) {
                return accepted;
            }
        }

        if (hint == 0) {
            // frame(또는 skippable frame)이 끝났다. 다음 byte는 새 frame의 시작이다.
            frameStart = true;
            continue;
        }
        if (consumed == source.Size() && written != outputChunk) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        if (read == 0 && written == 0) {
            // 입력·출력 공간이 있는데 진행하지 않았다: 계약 밖의 상태다(무한 반복 방지).
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }
    }
}

// --- 스트림 압축의 출력 대상 ---

// LZ4F 호출 한 번의 출력 대상이다. 남은 창이 그 호출의 최악 출력 이상이면 호출자 출력에 바로 쓰고,
// 아니면 단계 버퍼에 쓴다. 최악 크기 이상을 주므로 LZ4F 호출은 출력 부족으로 실패하지 않고
// flush·end는 한 번에 끝난다(lz4frame.h의 LZ4F_compressBound 계약). 결과 byte는 대상과
// 무관하다(블록 압축 용량이 같다).
struct StepTarget {
    uint8* data = nullptr;
    usize capacity = 0;
    bool staged = false;
};

StepTarget ChooseTarget(FrameCompressor& state, Span<uint8> output, usize produced,
                        usize worst) noexcept {
    const usize room = output.Size() - produced;
    if (room >= worst) {
        return StepTarget{output.Data() + produced, room, false};
    }
    return StepTarget{state.stage, state.stageCapacity, true};
}

// LZ4F가 쓴 byte를 반영하고 호출자 출력으로 넘긴 byte 수를 돌려준다. 단계 버퍼면 창에 들어가는 만큼
// 옮기고 나머지는 다음 호출이 먼저 넘긴다.
usize Commit(FrameCompressor& state, const StepTarget& target, usize written, Span<uint8> output,
             usize produced) noexcept {
    if (!target.staged) {
        return written;
    }

    const usize copy = Minimum(written, output.Size() - produced);
    platform::MemoryCopy(output.Data() + produced, state.stage, copy);
    state.stageBegin = copy;
    state.stageEnd = written;
    return copy;
}

} // namespace

Result<usize> Bound(usize sourceBytes) noexcept {
    // 최악 크기 = 헤더(최대 19) + 블록마다 블록 헤더 4byte + 원본(비압축 저장) + endmark 4 +
    // content checksum 4다. LZ4F_compressFrameBound는 size_t로 계산하므로 overflow를 먼저 거른다.
    const usize blocks = sourceBytes / BlockBytes + (sourceBytes % BlockBytes != 0 ? 1 : 0);
    const usize overhead = LZ4F_HEADER_SIZE_MAX + blocks * LZ4F_BLOCK_HEADER_SIZE +
                           LZ4F_ENDMARK_SIZE + LZ4F_CONTENT_CHECKSUM_SIZE;
    if (sourceBytes > static_cast<usize>(-1) - overhead) {
        return Result<usize>::Failure(ErrorCode::Overflow);
    }

    const LZ4F_preferences_t preferences = Preferences(1, sourceBytes, true);
    const size_t bound = LZ4F_compressFrameBound(sourceBytes, &preferences);
    if (bound < sourceBytes) {
        return Result<usize>::Failure(ErrorCode::Overflow);
    }

    return Result<usize>::Success(bound);
}

Result<void*> CreateCompressor(int32 level, Allocator& allocator) noexcept {
    auto created = CreateState(level, allocator);
    if (!created) {
        return Result<void*>::Failure(created.Error());
    }

    return Result<void*>::Success(created.Value());
}

Result<void*> CreateStreamCompressor(int32 level, Allocator& allocator) noexcept {
    auto created = CreateState(level, allocator);
    if (!created) {
        return Result<void*>::Failure(created.Error());
    }

    // 단계 버퍼는 LZ4F 호출 한 번의 최악 출력(헤더, 블록 하나를 끝내는 update, flush·end) 중 가장
    // 큰 값이다.
    FrameCompressor* state = created.Value();
    state->preferences = Preferences(level, 0, false);
    state->stageCapacity = Maximum(LZ4F_compressBound(StreamPieceBytes, &state->preferences),
                                   LZ4F_compressBound(0, &state->preferences));
    state->stageCapacity = Maximum(state->stageCapacity, LZ4F_HEADER_SIZE_MAX);
    state->stage = static_cast<uint8*>(CodecAllocate(allocator, state->stageCapacity));
    if (state->stage == nullptr) {
        DestroyCompressor(state);
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<void*>::Success(state);
}

void DestroyCompressor(void* codec) noexcept {
    if (codec == nullptr) {
        return;
    }

    auto* state = static_cast<FrameCompressor*>(codec);
    Allocator& allocator = *state->allocator;
    (void)LZ4F_freeCompressionContext(state->context);
    CodecFree(allocator, state->stage);
    DestroyAt(state);
    CodecFree(allocator, state);
}

Result<void*> CreateDecompressor(Allocator& allocator) noexcept {
    // 생성 함수의 실패는 문맥 할당 실패뿐이다. 블록 버퍼는 frame 헤더를 읽을 때 블록 크기에 맞춰
    // 할당한다.
    LZ4F_dctx* context = LZ4F_createDecompressionContext_advanced(Memory(allocator), LZ4F_VERSION);
    if (context == nullptr) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<void*>::Success(context);
}

void DestroyDecompressor(void* codec) noexcept {
    // 끝나지 않은 frame의 단계 값을 돌려주지만 해제는 항상 성공한다.
    (void)LZ4F_freeDecompressionContext(static_cast<LZ4F_dctx*>(codec));
}

Result<usize> Compress(void* codec, Span<const uint8> source, Span<uint8> destination,
                       const CompressionProbe& probe) noexcept {
    FrameCompressor& state = *static_cast<FrameCompressor*>(codec);
    const uint8* input = NonNullSource(source);
    uint8* output = NonNullDestination(destination);
    const usize capacity = destination.Size();

    // 원본 크기를 기록하고 블록을 입력 위치에서 바로 압축한다(autoFlush, stable source). 이
    // 설정·호출 순서는 64KiB를 넘는 입력에서 upstream LZ4F_compressFrame과 같은 byte를 만든다. 분할
    // 단위로 나눠 넣어도 앞 조각이 바로 앞에 이어져 있으므로 결과가 같다. 앞선 실패·취소로 남은
    // frame은 LZ4F_compressBegin이 버린다.
    const LZ4F_preferences_t preferences = Preferences(state.level, source.Size(), true);
    const size_t header = LZ4F_compressBegin(state.context, output, capacity, &preferences);
    if (LZ4F_isError(header)) {
        return Result<usize>::Failure(CompressionError(header));
    }

    LZ4F_compressOptions_t options{};
    options.stableSrc = LZ4F_srcStable;
    usize produced = header;
    usize consumed = 0;
    while (true) {
        if (probe.IsCancelled()) {
            return Result<usize>::Failure(ErrorCode::Cancelled);
        }
        if (consumed == source.Size()) {
            break;
        }

        // 남은 출력이 TryGetCompressBound 이상이면 LZ4F 계약상 조각마다 성공한다. 작으면 시도하고
        // 넘치면 dstCapacity_tooSmall이다.
        const usize piece = Minimum(source.Size() - consumed, CompressionChunkBytes);
        const size_t written =
            LZ4F_compressUpdate(state.context, output + produced, capacity - produced,
                                input + consumed, piece, &options);
        if (LZ4F_isError(written)) {
            return Result<usize>::Failure(CompressionError(written));
        }
        consumed += piece;
        produced += written;
    }

    // footer(endmark·content checksum)다. 남은 공간을 정확히 채우면 끝났는지 1byte 예비 창으로
    // 확인한다(LZ4F는 창을 채운 호출 뒤에 출력이 남았을 수 있다고 본다).
    const usize room = capacity - produced;
    if (room == 0) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }
    const size_t footer = LZ4F_compressEnd(state.context, output + produced, room, nullptr);
    if (LZ4F_isError(footer)) {
        return Result<usize>::Failure(CompressionError(footer));
    }
    produced += footer;
    if (footer == room) {
        uint8 spare = 0;
        const size_t rest = LZ4F_compressEnd(state.context, &spare, 1, nullptr);
        if (LZ4F_isError(rest)) {
            return Result<usize>::Failure(CompressionError(rest));
        }
        if (rest != 0) {
            return Result<usize>::Failure(ErrorCode::CapacityExceeded);
        }
    }

    return Result<usize>::Success(produced);
}

Result<usize> Decompress(void* codec, Span<const uint8> source, Span<uint8> destination,
                         const CompressionProbe& probe) noexcept {
    FrameOutput output;
    output.fixed = NonNullDestination(destination);
    output.limit = destination.Size();
    output.writable = destination.Size();

    auto decoded = Decode(static_cast<LZ4F_dctx*>(codec), source, output, true, probe);
    if (!decoded) {
        return Result<usize>::Failure(decoded.Error());
    }
    return Result<usize>::Success(output.produced);
}

Result<usize> DecompressAppend(void* codec, Span<const uint8> source, usize maxOutputBytes,
                               Array<uint8>& output, const CompressionProbe& probe) noexcept {
    FrameOutput target;
    target.array = &output;
    target.base = output.Size();
    target.limit = maxOutputBytes;

    // 첫 frame이 원본 크기를 기록했으면 그만큼, 아니면 64KiB를 먼저 확보한다. 상한과 입력 비례
    // 상한을 넘지 않는다. 이후에는 예비 창에 출력이 더 나올 때만 늘리므로 output이 base + 상한보다
    // 길어지지 않는다.
    const uint64 declared = DeclaredContentSize(source);
    usize initial = AppendInitialBytes;
    if (declared != 0) {
        initial = declared < static_cast<uint64>(maxOutputBytes) ? static_cast<usize>(declared)
                                                                 : maxOutputBytes;
    }
    initial = Minimum(Minimum(initial, maxOutputBytes), MaximumExpansion(source.Size()));
    if (initial != 0) {
        auto grown = target.Grow(initial);
        if (!grown) {
            RestoreLength(output, target.base);
            return Result<usize>::Failure(grown.Error());
        }
    }

    // 출력 저장소는 늘 때 옮겨질 수 있으므로 stableDst를 쓰지 않는다(LZ4F가 history를 내부 버퍼로
    // 복사한다).
    auto decoded = Decode(static_cast<LZ4F_dctx*>(codec), source, target, false, probe);
    if (!decoded) {
        RestoreLength(output, target.base);
        return Result<usize>::Failure(decoded.Error());
    }

    RestoreLength(output, target.base + target.produced);
    return Result<usize>::Success(target.produced);
}

// --- 스트리밍(10·12절) ---

void ResetStreamCompressor(void* codec) noexcept {
    // 진행 중인 frame은 다음 LZ4F_compressBegin이 버린다.
    FrameCompressor& state = *static_cast<FrameCompressor*>(codec);
    state.begun = false;
    state.ended = false;
    state.stageBegin = 0;
    state.stageEnd = 0;
}

void ResetStreamDecompressor(void* codec) noexcept {
    LZ4F_resetDecompressionContext(static_cast<LZ4F_dctx*>(codec));
}

Result<StreamProgress> CompressStep(void* codec, Span<const uint8> input, Span<uint8> output,
                                    StreamFlush flush) noexcept {
    FrameCompressor& state = *static_cast<FrameCompressor*>(codec);
    const uint8* source = NonNullSource(input);
    usize consumed = 0;
    usize produced = 0;
    bool flushed = false;
    bool complete = false;
    while (true) {
        // 단계 버퍼에 남은 출력을 먼저 넘긴다(출력 순서 유지). 창이 차면 None은 입력을 모두 넣었을
        // 때 완료다(남은 출력은 다음 호출이 먼저 넘긴다).
        if (state.stageBegin != state.stageEnd) {
            if (produced == output.Size()) {
                complete = flush == StreamFlush::None && consumed == input.Size();
                break;
            }
            const usize copy = Minimum(state.stageEnd - state.stageBegin, output.Size() - produced);
            platform::MemoryCopy(output.Data() + produced, state.stage + state.stageBegin, copy);
            state.stageBegin += copy;
            produced += copy;
            continue;
        }

        // 요청 완료: None은 입력을 모두 넣은 때, Sync는 이 호출의 flush 출력을 모두 넘긴 때, End는
        // frame을 끝내고 모두 넘긴 때다. 지난 호출이 flush 출력을 다 넘기지 못했으면 다시 부른
        // LZ4F_flush가 0byte로 끝난다.
        if (state.ended || flushed || (flush == StreamFlush::None && consumed == input.Size())) {
            complete = true;
            break;
        }
        if (produced == output.Size()) {
            break;
        }

        StepTarget target;
        size_t written = 0;
        if (!state.begun) {
            target = ChooseTarget(state, output, produced, LZ4F_HEADER_SIZE_MAX);
            written =
                LZ4F_compressBegin(state.context, target.data, target.capacity, &state.preferences);
            state.begun = !LZ4F_isError(written);
        } else if (consumed < input.Size()) {
            const usize piece = Minimum(input.Size() - consumed, StreamPieceBytes);
            target = ChooseTarget(state, output, produced,
                                  LZ4F_compressBound(piece, &state.preferences));
            written = LZ4F_compressUpdate(state.context, target.data, target.capacity,
                                          source + consumed, piece, nullptr);
            consumed += LZ4F_isError(written) ? 0 : piece;
        } else if (flush == StreamFlush::Sync) {
            target =
                ChooseTarget(state, output, produced, LZ4F_compressBound(0, &state.preferences));
            written = LZ4F_flush(state.context, target.data, target.capacity, nullptr);
            flushed = true;
        } else {
            target =
                ChooseTarget(state, output, produced, LZ4F_compressBound(0, &state.preferences));
            written = LZ4F_compressEnd(state.context, target.data, target.capacity, nullptr);
            state.ended = !LZ4F_isError(written);
        }
        if (LZ4F_isError(written)) {
            return Result<StreamProgress>::Failure(CompressionError(written));
        }
        produced += Commit(state, target, written, output, produced);
    }

    StreamProgress progress;
    progress.consumed = consumed;
    progress.produced = produced;
    progress.complete = complete;
    return Result<StreamProgress>::Success(progress);
}

Result<StreamProgress> DecompressStep(void* codec, Span<const uint8> input, Span<uint8> output,
                                      StreamDecodeState& state) noexcept {
    auto* context = static_cast<LZ4F_dctx*>(codec);
    const uint8* source = NonNullSource(input);
    usize consumed = 0;
    usize produced = 0;
    // LZ4F가 내보내지 못한 출력을 갖고 있을 수 있는지다. 출력을 가득 채우고 frame이 끝나지 않았을
    // 때만 참이다.
    bool pending = false;
    while (produced < output.Size()) {
        if (state.atFrameStart) {
            if (consumed == input.Size()) {
                break;
            }
            // frame 시작 byte를 모은다. magic·FLG(5byte)로 헤더 크기를 알면 헤더 전체(최대
            // 19byte)를 모아 LZ4F에 먼저 넣는다(OpenFrame). 쓰레기는 받은 byte에서 바로 거부한다.
            // 헤더 byte도 새 frame의 입력이므로 경계를 지운다.
            usize wanted = FrameProbeBytes;
            if (state.headerBytes >= FrameProbeBytes) {
                const size_t headerBytes = LZ4F_headerSize(state.header, state.headerBytes);
                if (LZ4F_isError(headerBytes)) {
                    return Result<StreamProgress>::Failure(DecompressionError(headerBytes));
                }
                wanted = headerBytes;
            }
            const usize take = Minimum(wanted - state.headerBytes, input.Size() - consumed);
            platform::MemoryCopy(state.header + state.headerBytes, source + consumed, take);
            state.headerBytes += static_cast<uint32>(take);
            consumed += take;
            state.atBoundary = false;
            auto checked = CheckMagic(state.header, state.headerBytes);
            if (!checked) {
                return Result<StreamProgress>::Failure(checked.Error());
            }
            if (state.headerBytes < wanted || wanted == FrameProbeBytes) {
                // 입력을 더 기다리거나, 이제 안 헤더 크기만큼 다음 반복이 더 모은다.
                continue;
            }

            auto opened = OpenFrame(context, state.header, state.headerBytes);
            if (!opened) {
                return Result<StreamProgress>::Failure(opened.Error());
            }
            state.headerBytes = 0;
            pending = false;
            if (opened.Value()) {
                // 헤더만으로 끝나는 frame(빈 skippable frame)이다.
                state.atBoundary = true;
                state.sawStream = true;
                continue;
            }
            state.atFrameStart = false;
            continue;
        }

        const usize room = output.Size() - produced;
        size_t read = input.Size() - consumed;
        size_t written = room;
        const size_t hint = LZ4F_decompress(context, output.Data() + produced, &written,
                                            source + consumed, &read, nullptr);
        consumed += read;
        produced += written;
        if (LZ4F_isError(hint)) {
            return Result<StreamProgress>::Failure(DecompressionError(hint));
        }
        if (hint == 0) {
            pending = false;
            state.atFrameStart = true;
            state.atBoundary = true;
            state.sawStream = true;
            continue;
        }
        pending = written == room;
        // 입력을 다 넣었고 출력 공간이 남았다면 LZ4F가 가진 출력도 다 나왔다. 빈 입력으로 다시
        // 부르지 않는다.
        if (consumed == input.Size() && !pending) {
            break;
        }
        if (read == 0 && written == 0) {
            break;
        }
    }

    state.outputPending = pending;
    StreamProgress progress;
    progress.consumed = consumed;
    progress.produced = produced;
    progress.complete = consumed == input.Size() && !pending;
    return Result<StreamProgress>::Success(progress);
}

} // namespace rocks::detail::lz4frame
