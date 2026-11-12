#include <CompressionTraining/CompressionDictionaryTraining.h>

#include <Core/Assert.h>
#include <Core/Move.h>
#include <Platform/Sync.h>

// zstd dictBuilder adapter다. zdict.h를 include하는 유일한 번역 단위다(학습 직렬화의 전제).
// ZDICT_STATIC_LINKING_ONLY는 모듈 규칙이 ThirdParty.json consumerDefines에서 정의한다.
#include <zdict.h>
#include <zstd.h>
#include <zstd_errors.h>

namespace rocks {
namespace {

// ZDICT는 표본 길이를 size_t 배열로 받는다. usize가 같은 타입이라 복사 없이 넘긴다.
static_assert(__is_same(usize, size_t), "usize must be size_t");

// fastCover 고정 매개변수다(공개하지 않는다): 빈도 표 2^20, 가속 1(가장 정확).
constexpr unsigned FrequencyLog = 20;
constexpr unsigned Acceleration = 1;
// k를 탐색할 때 ZDICT_trainFromBuffer와 같은 후보 수다([50, 2000]에서 5개). 40단계는 8–16배 느린데
// 이득이 0–0.5%다.
constexpr unsigned SearchSteps = 4;
// 학습 결과에 붙는 헤더의 최대 크기다(magic·ID 8byte + 엔트로피 표 ≤ 248, zdict.c).
constexpr usize MaxHeaderBytes = 256;
// 표본 길이 합의 상한(미만)이다(COVER_MAX_SAMPLES_SIZE와 32bit 계산).
constexpr uint64 MaxSampleBytes = 0xFFFFFFFFull;
// fastCover가 학습할 수 있는 최소 표본 수다(fastcover.c FASTCOVER_ctx_init).
constexpr usize MinTrainingSamples = 5;

bool Overlaps(const void* data, usize bytes, const Array<uint8>& output) noexcept {
    if (bytes == 0 || output.Capacity() == 0) {
        return false;
    }

    const uptr begin = reinterpret_cast<uptr>(data);
    const uptr storage = reinterpret_cast<uptr>(output.Data());
    return begin < storage + output.Capacity() && storage < begin + bytes;
}

bool ValidLevel(int32 level) noexcept {
    auto range = TryGetCompressionLevelRange(CompressionFormat::Zstd);
    return range && level >= range.Value().minimum && level <= range.Value().maximum;
}

bool ValidId(uint32 id) noexcept {
    return id == 0 || (id >= MinExplicitDictionaryId && id <= MaxExplicitDictionaryId);
}

// 표본 길이 합을 overflow 없이 구한다. samples 크기와 같고 상한 미만이어야 한다.
bool ValidSamples(Span<const uint8> samples, Span<const usize> sampleSizes) noexcept {
    uint64 total = 0;
    for (const usize size : sampleSizes) {
        if (size > MaxSampleBytes - total) {
            return false;
        }
        total += size;
    }
    return total == samples.Size() && total < MaxSampleBytes &&
           sampleSizes.Size() < static_cast<usize>(0xFFFFFFFFu);
}

// finalizing: finalize는 래퍼가 헤더 최대 크기 + 내용을 확보하므로 dstSize_tooSmall은 인자 탓이
// 아니라 upstream 헤더 버퍼(256byte) 초과다(이론상 최대 297byte, 실측 최대 191byte).
ErrorCode DictionaryBuilderError(size_t code, bool finalizing) noexcept {
    switch (ZSTD_getErrorCode(code)) {
    case ZSTD_error_dstSize_tooSmall:
        return finalizing ? ErrorCode::DependencyFailed : ErrorCode::InvalidArgument;
    case ZSTD_error_parameter_outOfBound:
    case ZSTD_error_srcSize_wrong:
    case ZSTD_error_dictionaryCreation_failed:
        return ErrorCode::InvalidArgument;
    case ZSTD_error_memory_allocation:
        return ErrorCode::OutOfMemory;
    default:
        // GENERIC: 모든 후보가 실패했다(인자는 미리 검사했으므로 후보 안의 할당 실패일 수 있다).
        return ErrorCode::DependencyFailed;
    }
}

ZDICT_params_t BuilderParameters(int32 level, uint32 id) noexcept {
    ZDICT_params_t parameters{};
    parameters.compressionLevel = level;
    parameters.notificationLevel = 0;
    parameters.dictID = id;
    return parameters;
}

// 학습 직렬화 mutex다. 해제하지 않는다: 프로세스 종료 중 다른 스레드가 학습해도 소멸된 mutex를 쓰지
// 않는다(POSIX 소멸자는 잠긴 mutex에서 Fatal이다). 첫 호출에서 thread-safe하게 만든다.
platform::Mutex& TrainingMutex() noexcept {
    alignas(platform::Mutex) static unsigned char storage[sizeof(platform::Mutex)];
    static platform::Mutex* mutex = ConstructAt<platform::Mutex>(storage);
    return *mutex;
}

// 빈 Span의 nullptr 대신 넘길 주소다(zdict는 0byte 표본에도 포인터 산술을 한다).
const uint8 EmptyBytes = 0;

const uint8* NonNull(Span<const uint8> bytes) noexcept {
    return bytes.Size() != 0 ? bytes.Data() : &EmptyBytes;
}

void RestoreLength(Array<uint8>& output, usize length) noexcept {
    auto restored = output.TryResize(length);
    ROCKS_ASSERT(restored.IsOk());
}

} // namespace

Result<DictionaryTrainingResult>
TryTrainCompressionDictionary(Span<const uint8> samples, Span<const usize> sampleSizes,
                              const DictionaryTrainingSettings& settings,
                              Array<uint8>& output) noexcept {
    const uint32 k = settings.segmentSize;
    const uint32 d = settings.dmerSize;
    const bool argumentsValid =
        sampleSizes.Size() >= MinTrainingSamples && ValidSamples(samples, sampleSizes) &&
        samples.Size() >= 8 && settings.capacity >= MinTrainingCapacity &&
        settings.capacity <= MaxDictionaryBytes && ValidLevel(settings.compressionLevel) &&
        ValidId(settings.dictionaryId) && (d == 6 || d == 8) &&
        (k == 0 || (k >= d && k <= settings.capacity && k - d + 1 <= 65535)) &&
        !Overlaps(samples.Data(), samples.Size(), output) &&
        !Overlaps(sampleSizes.Data(), sampleSizes.Size() * sizeof(usize), output);
    if (!argumentsValid) {
        return Result<DictionaryTrainingResult>::Failure(ErrorCode::InvalidArgument);
    }

    const usize base = output.Size();
    auto reserved = output.TryAppendUninitialized(settings.capacity);
    if (!reserved) {
        return Result<DictionaryTrainingResult>::Failure(reserved.Error());
    }

    // k를 지정하면 탐색 범위가 그 값 하나라 정확히 1회 학습한다(ZDICT_trainFromBuffer_fastCover는
    // 할당 실패를 검사하지 않아 쓰지 않는다). splitPoint 1.0은 모든 표본으로 학습·평가한다(0.75는
    // 학습 부분이 작으면 upstream이 죽는다).
    ZDICT_fastCover_params_t parameters{};
    parameters.k = k;
    parameters.d = d;
    parameters.f = FrequencyLog;
    parameters.steps = SearchSteps;
    parameters.nbThreads = 1;
    parameters.splitPoint = 1.0;
    parameters.accel = Acceleration;
    parameters.shrinkDict = 0;
    parameters.zParams = BuilderParameters(settings.compressionLevel, settings.dictionaryId);

    size_t written = 0;
    {
        // fastCover는 프로세스 전역 표시 수준을 동기화 없이 쓴다. 학습 호출을 직렬화한다.
        platform::LockGuard lock(TrainingMutex());
        written = ZDICT_optimizeTrainFromBuffer_fastCover(
            reserved.Value().Data(), settings.capacity, NonNull(samples), sampleSizes.Data(),
            static_cast<unsigned>(sampleSizes.Size()), &parameters);
    }
    if (ZDICT_isError(written)) {
        RestoreLength(output, base);
        return Result<DictionaryTrainingResult>::Failure(DictionaryBuilderError(written, false));
    }

    RestoreLength(output, base + written);
    DictionaryTrainingResult result;
    result.bytes = written;
    result.segmentSize = parameters.k;
    result.dmerSize = parameters.d;
    return Result<DictionaryTrainingResult>::Success(result);
}

Result<usize> TryFinalizeCompressionDictionary(Span<const uint8> content, Span<const uint8> samples,
                                               Span<const usize> sampleSizes,
                                               const DictionaryFinalizeSettings& settings,
                                               Array<uint8>& output) noexcept {
    const bool argumentsValid =
        content.Size() <= MaxDictionaryBytes - MaxHeaderBytes &&
        ValidSamples(samples, sampleSizes) && ValidLevel(settings.compressionLevel) &&
        ValidId(settings.dictionaryId) && !Overlaps(content.Data(), content.Size(), output) &&
        !Overlaps(samples.Data(), samples.Size(), output) &&
        !Overlaps(sampleSizes.Data(), sampleSizes.Size() * sizeof(usize), output);
    if (!argumentsValid) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    // 결과는 헤더(≤ 256) + max(내용, 8)이다. 이만큼 확보하면 내용이 잘리지 않는다(zstd는 용량이
    // 모자라면 내용의 끝을 자른다).
    const usize capacity = (content.Size() > 8 ? content.Size() : usize{8}) + MaxHeaderBytes;
    const usize base = output.Size();
    auto reserved = output.TryAppendUninitialized(capacity);
    if (!reserved) {
        return Result<usize>::Failure(reserved.Error());
    }

    // finalize는 전역 상태를 쓰지 않아 직렬화하지 않는다. 출력 버퍼를 읽지 않으므로 미초기화 공간을
    // 넘긴다. 빈 내용은 0byte 복사에도 nullptr을 넘기지 않는다.
    const size_t written = ZDICT_finalizeDictionary(
        reserved.Value().Data(), capacity, NonNull(content), content.Size(), NonNull(samples),
        sampleSizes.Data(), static_cast<unsigned>(sampleSizes.Size()),
        BuilderParameters(settings.compressionLevel, settings.dictionaryId));
    if (ZDICT_isError(written)) {
        RestoreLength(output, base);
        return Result<usize>::Failure(DictionaryBuilderError(written, true));
    }

    RestoreLength(output, base + written);
    return Result<usize>::Success(written);
}

Result<usize> TryRefinalizeCompressionDictionary(Span<const uint8> dictionary,
                                                 Span<const uint8> samples,
                                                 Span<const usize> sampleSizes,
                                                 const DictionaryFinalizeSettings& settings,
                                                 Array<uint8>& output) noexcept {
    if (dictionary.Size() > MaxDictionaryBytes) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    // 엔트로피 표를 읽어 헤더 크기를 구한다(magic이 없거나 표가 손상이면 오류). 내용은 헤더 뒤
    // 전부다.
    const size_t header = ZDICT_getDictHeaderSize(NonNull(dictionary), dictionary.Size());
    if (ZDICT_isError(header)) {
        return Result<usize>::Failure(ZSTD_getErrorCode(header) == ZSTD_error_memory_allocation
                                          ? ErrorCode::OutOfMemory
                                          : ErrorCode::InvalidArgument);
    }

    return TryFinalizeCompressionDictionary(
        Span<const uint8>(dictionary.Data() + header, dictionary.Size() - header), samples,
        sampleSizes, settings, output);
}

} // namespace rocks
