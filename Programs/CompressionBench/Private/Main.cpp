#include <Compression/CompressedBlocks.h>
#include <Compression/Compression.h>
#include <Compression/CompressionDictionary.h>
#include <Compression/CompressionStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Memory.h>
#include <Task/WorkerScheduler.h>

#include <stdio.h>

// 압축 처리량 측정 도구다. 세 부분을 잰다.
// - 일회성: 형식·수준마다 Compressor·Decompressor를 재사용한 단일 스레드 처리량. LZ4는 raw
//   block(lz4)과 frame(lz4frame: 64KiB linked 블록, 원본 크기·content checksum)을 함께 잰다.
// - 스트림: CompressStream·DecompressStream에 64KiB 조각으로 넣는 처리량(단일 스레드).
// - 블록 컨테이너: 256KiB 블록 RCBK의 순차·병렬 압축과 전체 읽기 처리량.
// - 사전: 작은 항목(1KiB·4KiB)을 하나씩 zstd로 압축할 때 사전 유무의 압축률·처리량. 사전은 학습하지
//   않은 raw 내용(항목 구간 바로 뒤의 64KiB)이라 학습 사전보다 효과가 작다(하한 추정).
// - Brotli: 해제 전용이라 --brotli로 준 외부 stream 파일의 해제 처리량만 잰다(일회성·스트림).
//
// 사용:
//   CompressionBench                       고정 합성 코퍼스(기본 16MiB)
//   CompressionBench --size 64             합성 코퍼스 크기(MiB)
//   CompressionBench --file <path>         실제 파일
//   CompressionBench --rounds 5            반복 횟수(최고값을 보고한다)
//   CompressionBench --workers 8           블록 병렬 측정의 worker 수(기본 4)
//   CompressionBench --brotli a.br         Brotli stream 해제(a.br은 `brotli -q 11 a`처럼 만든다.
//                                          --file a를 함께 주면 결과가 a와 같은지도 적는다)
//
// 합성 코퍼스는 고정 seed로 만든 결정적 데이터다(플랫폼·실행과 무관하게 같은 byte):
// 절반은 어휘 200개로 만든 문장형 텍스트, 절반은 노이즈를 섞은 양자화 정점·색 배열(에셋 binary
// 흉내)이다. 결과는 기기·빌드 구성에 따라 달라지므로 비교할 때는 같은 기기·Release에서 잰다.
namespace {

using namespace rocks;

// 출력용 형식 이름이다(TryGetCompressionFormatInfo, 내장·사용자 등록 형식 공통). printf 인자로 쓰는
// 임시 객체라 문장이 끝날 때까지 유효하다.
struct FormatName {
    char text[MaxCompressionFormatNameBytes + 1] = {};

    explicit FormatName(CompressionFormat format) noexcept {
        auto info = TryGetCompressionFormatInfo(format);
        const StringView name = info ? info.Value().Name() : StringView("?");
        for (usize index = 0; index < name.ByteCount(); ++index) {
            text[index] = name.Data()[index];
        }
    }
};

struct Options {
    const char* file = nullptr;
    const char* brotli = nullptr;
    usize sizeMiB = 16;
    uint32 rounds = 3;
    uint32 workers = 4;
};

uint64 Next(uint64& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

bool Generate(Array<uint8>& corpus, usize bytes) {
    if (!corpus.TryResize(bytes)) {
        return false;
    }

    static constexpr const char* Syllables[] = {"ro",  "ck", "en",  "gi",  "ne",  "te", "xt",
                                                "ure", "me", "sh",  "au",  "di",  "o",  "le",
                                                "vel", "ma", "ter", "ial", "sha", "der"};
    uint64 state = 0x2545f4914f6cdd1dull;
    char words[200][12] = {};
    for (auto& word : words) {
        usize length = 0;
        const uint64 parts = 1 + Next(state) % 3;
        for (uint64 part = 0; part < parts; ++part) {
            for (const char* syllable = Syllables[Next(state) % 20];
                 *syllable != '\0' && length < 11; ++syllable) {
                word[length++] = *syllable;
            }
        }
    }

    // 앞 절반: 문장형 텍스트.
    const usize half = bytes / 2;
    usize position = 0;
    while (position < half) {
        const char* word = words[Next(state) % 200];
        for (const char* letter = word; *letter != '\0' && position < half; ++letter) {
            corpus[position++] = static_cast<uint8>(*letter);
        }
        if (position < half) {
            corpus[position++] = static_cast<uint8>(Next(state) % 11 == 0 ? '\n' : ' ');
        }
    }

    // 뒤 절반: 위치(float 3)·색(uint8 4) 정점 배열. 매끈한 곡면에 작은 노이즈를 섞는다.
    usize vertex = 0;
    while (position + 16 <= bytes) {
        const float base = static_cast<float>(vertex) * 0.01f;
        const float noise = static_cast<float>(Next(state) % 1000) * 1e-6f;
        const float values[3] = {base + noise, base * 0.5f - noise, 1.0f - base * 0.25f};
        platform::MemoryCopy(corpus.Data() + position, values, sizeof(values));
        position += sizeof(values);
        const uint8 color[4] = {static_cast<uint8>(vertex & 0xff),
                                static_cast<uint8>((vertex >> 3) & 0xff), 128, 255};
        platform::MemoryCopy(corpus.Data() + position, color, sizeof(color));
        position += sizeof(color);
        ++vertex;
    }
    while (position < bytes) {
        corpus[position++] = 0;
    }
    return true;
}

bool Load(Array<uint8>& corpus, const char* path) {
    platform::File file;
    if (!file.Open(path, platform::FileOpenMode::ReadExisting)) {
        return false;
    }
    uint64 size = 0;
    if (!file.Size(size) || !corpus.TryResize(static_cast<usize>(size))) {
        return false;
    }
    return static_cast<bool>(file.ReadExact(corpus.Data(), corpus.Size()).status);
}

double Seconds(uint64 start) {
    return static_cast<double>(platform::MonotonicNanoseconds() - start) * 1e-9;
}

// body를 rounds번 실행해 가장 짧은 시간을 돌려준다. body가 false면 음수다.
template <typename Body>
double Best(uint32 rounds, Body&& body) {
    double best = 1e30;
    for (uint32 round = 0; round < rounds; ++round) {
        const uint64 start = platform::MonotonicNanoseconds();
        if (!body()) {
            return -1.0;
        }
        const double elapsed = Seconds(start);
        best = elapsed < best ? elapsed : best;
    }
    return best;
}

constexpr usize StreamPieceBytes = usize{64} << 10;

// 작은 항목을 하나씩 압축·해제한다. 사전 문맥과 사전 없는 문맥을 같은 항목에 쓴다.
int RunDictionary(Span<const uint8> input, uint32 rounds, Allocator& allocator) {
    constexpr usize DictionaryBytes = usize{64} << 10;
    constexpr usize ItemsBytes = usize{4} << 20;
    if (input.Size() < ItemsBytes + DictionaryBytes) {
        printf("\ndictionary: corpus smaller than %u MiB, skipped\n", 4u);
        return 0;
    }
    const Span<const uint8> items(input.Data(), ItemsBytes);
    const Span<const uint8> dictionaryBytes(input.Data() + ItemsBytes, DictionaryBytes);

    printf("\ndictionary (zstd, raw 64KiB dictionary, one frame per item)\n%-6s %5s %10s %8s %12s "
           "%12s\n",
           "item", "level", "dictionary", "ratio", "comp MB/s", "decomp MB/s");
    static constexpr usize ItemSizes[] = {usize{1} << 10, usize{4} << 10};
    static constexpr int32 Levels[] = {3, 19};
    for (const usize itemBytes : ItemSizes) {
        for (const int32 level : Levels) {
            auto settings = CompressionSettings::TryMake(CompressionFormat::Zstd, level);
            auto encoding =
                CompressDictionary::TryCreate(settings.Value(), dictionaryBytes, allocator);
            auto decoding = DecompressDictionary::TryCreate(CompressionFormat::Zstd,
                                                            dictionaryBytes, allocator);
            if (!encoding || !decoding) {
                printf("%-6u %5d dictionary setup failed\n", static_cast<unsigned>(itemBytes),
                       level);
                return 1;
            }
            for (int useDictionary = 0; useDictionary < 2; ++useDictionary) {
                auto compressor = useDictionary != 0
                                      ? Compressor::TryCreate(encoding.Value(), allocator)
                                      : Compressor::TryCreate(settings.Value(), allocator);
                auto decompressor =
                    useDictionary != 0
                        ? Decompressor::TryCreate(decoding.Value(), allocator)
                        : Decompressor::TryCreate(CompressionFormat::Zstd, allocator);
                Array<uint8> packed(allocator);
                Array<usize> ends(allocator);
                Array<uint8> restored(allocator);
                if (!compressor || !decompressor || !restored.TryResize(itemBytes)) {
                    printf("%-6u %5d setup failed\n", static_cast<unsigned>(itemBytes), level);
                    return 1;
                }
                const double compressSeconds = Best(rounds, [&]() {
                    (void)packed.TryResize(0);
                    (void)ends.TryResize(0);
                    for (usize offset = 0; offset < items.Size(); offset += itemBytes) {
                        const Span<const uint8> item(items.Data() + offset, itemBytes);
                        if (!compressor.Value().TryCompressAppend(item, packed) ||
                            !ends.TryPushBack(packed.Size())) {
                            return false;
                        }
                    }
                    return true;
                });
                const double decompressSeconds = Best(rounds, [&]() {
                    usize begin = 0;
                    for (usize index = 0; index < ends.Size(); ++index) {
                        const Span<const uint8> frame(packed.Data() + begin, ends[index] - begin);
                        auto written = decompressor.Value().TryDecompress(
                            frame, Span<uint8>(restored.Data(), restored.Size()));
                        if (!written || written.Value() != itemBytes ||
                            !platform::MemoryEquals(restored.Data(),
                                                    items.Data() + index * itemBytes, itemBytes)) {
                            return false;
                        }
                        begin = ends[index];
                    }
                    return true;
                });
                if (compressSeconds < 0 || decompressSeconds < 0) {
                    printf("%-6u %5d dictionary round trip failed\n",
                           static_cast<unsigned>(itemBytes), level);
                    return 1;
                }
                const double megabytes = static_cast<double>(items.Size()) / (1024.0 * 1024.0);
                printf("%-6u %5d %10s %8.3f %12.1f %12.1f\n", static_cast<unsigned>(itemBytes),
                       level, useDictionary != 0 ? "yes" : "no",
                       static_cast<double>(packed.Size()) / static_cast<double>(items.Size()),
                       megabytes / compressSeconds, megabytes / decompressSeconds);
            }
        }
    }
    return 0;
}

int RunStreams(Span<const uint8> input, uint32 rounds, Allocator& allocator) {
    const double megabytes = static_cast<double>(input.Size()) / (1024.0 * 1024.0);
    printf("\nstream (64KiB pieces)\n%-8s %5s %8s %12s %12s\n", "format", "level", "ratio",
           "comp MB/s", "decomp MB/s");
    struct Case {
        CompressionFormat format;
        int32 level;
    };
    static constexpr Case Cases[] = {{CompressionFormat::Lz4Frame, 1},
                                     {CompressionFormat::Zstd, 3},
                                     {CompressionFormat::Gzip, 6}};
    for (const Case& item : Cases) {
        auto stream = CompressStream::TryCreate(
            CompressionSettings::TryMake(item.format, item.level).Value(), allocator, input.Size());
        auto decoder = DecompressStream::TryCreate(
            item.format, DecompressStreamLimits{input.Size(), 27}, allocator);
        if (!stream || !decoder) {
            printf("%-8s %5d cannot create\n", FormatName(item.format).text, item.level);
            return 6;
        }

        Array<uint8> packed(allocator);
        const double compressSeconds = Best(rounds, [&]() {
            packed.Clear();
            stream.Value().Reset();
            for (usize offset = 0; offset < input.Size(); offset += StreamPieceBytes) {
                const usize bytes = input.Size() - offset < StreamPieceBytes ? input.Size() - offset
                                                                             : StreamPieceBytes;
                if (!stream.Value().TryWriteAppend(Span<const uint8>(input.Data() + offset, bytes),
                                                   packed)) {
                    return false;
                }
            }
            return stream.Value().TryFinishAppend(packed).IsOk();
        });
        Array<uint8> restored(allocator);
        const double decompressSeconds = Best(rounds + 2, [&]() {
            restored.Clear();
            decoder.Value().Reset();
            for (usize offset = 0; offset < packed.Size(); offset += StreamPieceBytes) {
                const usize bytes = packed.Size() - offset < StreamPieceBytes
                                        ? packed.Size() - offset
                                        : StreamPieceBytes;
                if (!decoder.Value().TryWriteAppend(
                        Span<const uint8>(packed.Data() + offset, bytes), restored)) {
                    return false;
                }
            }
            return decoder.Value().TryFinish().IsOk() && restored.Size() == input.Size() &&
                   platform::MemoryCompare(restored.Data(), input.Data(), input.Size()) == 0;
        });
        if (compressSeconds < 0 || decompressSeconds < 0) {
            printf("%-8s %5d stream round trip failed\n", FormatName(item.format).text, item.level);
            return 7;
        }
        printf("%-8s %5d %8.3f %12.1f %12.1f\n", FormatName(item.format).text, item.level,
               static_cast<double>(packed.Size()) / static_cast<double>(input.Size()),
               megabytes / compressSeconds, megabytes / decompressSeconds);
    }
    return 0;
}

// Brotli는 해제 전용이라(Rocks에 압축기가 없다) 외부 도구(`brotli -q 11` 등)가 만든 stream 파일을
// 받아 해제 처리량만 잰다. 일회성은 크기를 아는 Span 해제(Decompressor 재사용), 스트림은 64KiB
// 조각이다. 측정 구간에는 해제만 넣고, 결과 비교(첫 Append 해제 결과와 같은지)는 구간 밖에서 한다.
// 해제 결과를 코퍼스와도 비교해 같은 파일을 --file로 주었는지 함께 적는다.
int RunBrotli(const char* path, Span<const uint8> corpus, uint32 rounds, Allocator& allocator) {
    if (path == nullptr) {
        printf("\nbrotli: decode only (no encoder in Rocks), pass --brotli <file.br> to measure\n");
        return 0;
    }

    Array<uint8> packed(allocator);
    if (!Load(packed, path) || packed.Size() == 0) {
        printf("\nbrotli: cannot read %s\n", path);
        return 2;
    }

    const Span<const uint8> source(packed.Data(), packed.Size());
    Array<uint8> reference(allocator);
    auto decoded =
        TryDecompressAppend(CompressionFormat::Brotli, source, usize{4096} << 20, reference);
    if (!decoded) {
        printf("\nbrotli: decode failed (%u)\n", static_cast<unsigned>(decoded.Error()));
        return 5;
    }

    if (reference.Size() == 0) {
        printf("\nbrotli: %s is an empty stream, nothing to measure\n", path);
        return 0;
    }

    const usize outputBytes = reference.Size();
    auto decompressor = Decompressor::TryCreate(CompressionFormat::Brotli, allocator);
    auto decoder = DecompressStream::TryCreate(CompressionFormat::Brotli,
                                               DecompressStreamLimits{outputBytes, 24}, allocator);
    Array<uint8> fixed(allocator);
    Array<uint8> streamed(allocator);
    if (!decompressor || !decoder || !fixed.TryResize(outputBytes) ||
        !streamed.TryReserve(outputBytes)) {
        printf("\nbrotli: out of memory\n");
        return 3;
    }

    const double oneShotSeconds = Best(rounds + 2, [&]() {
        auto written =
            decompressor.Value().TryDecompress(source, Span<uint8>(fixed.Data(), fixed.Size()));
        return written && written.Value() == outputBytes;
    });
    const double streamSeconds = Best(rounds + 2, [&]() {
        streamed.Clear();
        decoder.Value().Reset();
        for (usize offset = 0; offset < source.Size(); offset += StreamPieceBytes) {
            const usize bytes = source.Size() - offset < StreamPieceBytes ? source.Size() - offset
                                                                          : StreamPieceBytes;
            if (!decoder.Value().TryWriteAppend(Span<const uint8>(source.Data() + offset, bytes),
                                                streamed)) {
                return false;
            }
        }

        return decoder.Value().TryFinish().IsOk() && streamed.Size() == outputBytes;
    });
    if (oneShotSeconds < 0 || streamSeconds < 0 ||
        platform::MemoryCompare(fixed.Data(), reference.Data(), outputBytes) != 0 ||
        platform::MemoryCompare(streamed.Data(), reference.Data(), outputBytes) != 0) {
        printf("\nbrotli: decode failed or results differ\n");
        return 5;
    }

    const bool matches = outputBytes == corpus.Size() &&
                         platform::MemoryCompare(reference.Data(), corpus.Data(), outputBytes) == 0;
    const double megabytes = static_cast<double>(outputBytes) / (1024.0 * 1024.0);
    const double ratio = static_cast<double>(source.Size()) / static_cast<double>(outputBytes);
    printf("\nbrotli (decode only, %s, %.1f MiB out, %s)\n%-8s %8s %12s\n", path, megabytes,
           matches ? "equals corpus" : "differs from corpus", "mode", "ratio", "decomp MB/s");
    printf("%-8s %8.3f %12.1f\n", "oneshot", ratio, megabytes / oneShotSeconds);
    printf("%-8s %8.3f %12.1f\n", "stream", ratio, megabytes / streamSeconds);
    return 0;
}

int RunBlocks(Span<const uint8> input, uint32 rounds, uint32 workers, Allocator& allocator) {
    WorkerScheduler scheduler(allocator);
    if (!scheduler.Start({.workerCount = workers, .maxInFlight = 256})) {
        printf("cannot start %u workers\n", workers);
        return 8;
    }

    const double megabytes = static_cast<double>(input.Size()) / (1024.0 * 1024.0);
    printf("\nblocks (RCBK, 256KiB blocks, %u workers)\n%-8s %5s %8s %10s %10s %10s %10s\n",
           workers, "format", "level", "ratio", "comp MB/s", "par comp", "read MB/s", "par read");
    struct Case {
        CompressionFormat format;
        int32 level;
    };
    static constexpr Case Cases[] = {
        {CompressionFormat::Lz4, 1}, {CompressionFormat::Zstd, 3}, {CompressionFormat::Deflate, 6}};
    int status = 0;
    for (const Case& item : Cases) {
        const auto settings = CompressedBlocksSettings::TryMake(
            CompressionSettings::TryMake(item.format, item.level).Value());
        if (!settings) {
            status = 9;
            break;
        }

        Array<uint8> container(allocator);
        const double sequentialSeconds = Best(rounds, [&]() {
            container.Clear();
            return TryCompressBlocksAppend(settings.Value(), input, container, allocator).IsOk();
        });
        Array<uint8> parallel(allocator);
        const double parallelSeconds = Best(rounds, [&]() {
            parallel.Clear();
            return TryCompressBlocksAppend(scheduler, settings.Value(), input, parallel, allocator)
                .IsOk();
        });
        const bool same =
            parallel.Size() == container.Size() &&
            platform::MemoryCompare(parallel.Data(), container.Data(), container.Size()) == 0;

        auto reader = CompressedBlocksReader::TryOpen(
            Span<const uint8>(container.Data(), container.Size()), allocator);
        auto context = reader ? CompressedBlocksReadContext::TryCreate(reader.Value(), allocator)
                              : Result<CompressedBlocksReadContext>::Failure(reader.Error());
        Array<uint8> restored(allocator);
        if (!same || !context || !restored.TryResize(input.Size())) {
            printf("%-8s %5d block setup failed\n", FormatName(item.format).text, item.level);
            status = 10;
            break;
        }
        const Span<const uint8> stored(container.Data(), container.Size());
        const Span<uint8> destination(restored.Data(), restored.Size());
        auto matches = [&]() {
            return platform::MemoryCompare(restored.Data(), input.Data(), input.Size()) == 0;
        };
        const double readSeconds = Best(rounds + 2, [&]() {
            return reader.Value().TryRead(0, destination, stored, context.Value()).IsOk() &&
                   matches();
        });
        const double parallelReadSeconds = Best(rounds + 2, [&]() {
            return reader.Value().TryRead(scheduler, 0, destination, stored, 0, allocator).IsOk() &&
                   matches();
        });
        if (sequentialSeconds < 0 || parallelSeconds < 0 || readSeconds < 0 ||
            parallelReadSeconds < 0) {
            printf("%-8s %5d block round trip failed\n", FormatName(item.format).text, item.level);
            status = 11;
            break;
        }
        printf("%-8s %5d %8.3f %10.1f %10.1f %10.1f %10.1f\n", FormatName(item.format).text,
               item.level,
               static_cast<double>(container.Size()) / static_cast<double>(input.Size()),
               megabytes / sequentialSeconds, megabytes / parallelSeconds, megabytes / readSeconds,
               megabytes / parallelReadSeconds);
    }

    (void)scheduler.Shutdown();
    return status;
}

int Run(const Options& options) {
    SystemAllocator allocator;
    Array<uint8> corpus(allocator);
    const bool loaded = options.file != nullptr ? Load(corpus, options.file)
                                                : Generate(corpus, options.sizeMiB << 20);
    if (!loaded || corpus.Size() == 0) {
        printf("cannot prepare corpus\n");
        return 2;
    }

    const double megabytes = static_cast<double>(corpus.Size()) / (1024.0 * 1024.0);
    printf("corpus: %s, %.1f MiB, best of %u rounds, single thread\n",
           options.file != nullptr ? options.file : "synthetic(text+vertex)", megabytes,
           options.rounds);
    printf("%-8s %5s %8s %12s %12s\n", "format", "level", "ratio", "comp MB/s", "decomp MB/s");

    struct Case {
        CompressionFormat format;
        int32 level;
    };
    static constexpr Case Cases[] = {
        {CompressionFormat::Lz4, 1},      {CompressionFormat::Lz4, 9},
        {CompressionFormat::Lz4, 12},     {CompressionFormat::Lz4Frame, 1},
        {CompressionFormat::Lz4Frame, 9}, {CompressionFormat::Zstd, 1},
        {CompressionFormat::Zstd, 3},     {CompressionFormat::Zstd, 9},
        {CompressionFormat::Zstd, 19},    {CompressionFormat::Deflate, 1},
        {CompressionFormat::Deflate, 6},  {CompressionFormat::Gzip, 9}};
    const Span<const uint8> input(corpus.Data(), corpus.Size());
    for (const Case& item : Cases) {
        auto settings = CompressionSettings::TryMake(item.format, item.level);
        auto bound = TryGetCompressBound(item.format, input.Size());
        if (!settings || !bound) {
            printf("%-8s %5d unsupported\n", FormatName(item.format).text, item.level);
            continue;
        }
        auto compressor = Compressor::TryCreate(settings.Value(), allocator);
        auto decompressor = Decompressor::TryCreate(item.format, allocator);
        Array<uint8> packed(allocator);
        Array<uint8> restored(allocator);
        if (!compressor || !decompressor || !packed.TryResize(bound.Value()) ||
            !restored.TryResize(input.Size())) {
            printf("%-8s %5d out of memory\n", FormatName(item.format).text, item.level);
            return 3;
        }

        // 최고 수준은 느리므로 한 번만 잰다.
        const uint32 rounds = item.level >= 12 ? 1 : options.rounds;
        usize packedBytes = 0;
        double compressSeconds = 1e30;
        for (uint32 round = 0; round < rounds; ++round) {
            const uint64 start = platform::MonotonicNanoseconds();
            auto written =
                compressor.Value().TryCompress(input, Span<uint8>(packed.Data(), packed.Size()));
            const double elapsed = Seconds(start);
            if (!written) {
                printf("%-8s %5d compress failed (%u)\n", FormatName(item.format).text, item.level,
                       static_cast<unsigned>(written.Error()));
                return 4;
            }
            packedBytes = written.Value();
            compressSeconds = elapsed < compressSeconds ? elapsed : compressSeconds;
        }

        double decompressSeconds = 1e30;
        for (uint32 round = 0; round < options.rounds + 2; ++round) {
            const uint64 start = platform::MonotonicNanoseconds();
            auto written =
                decompressor.Value().TryDecompress(Span<const uint8>(packed.Data(), packedBytes),
                                                   Span<uint8>(restored.Data(), restored.Size()));
            const double elapsed = Seconds(start);
            if (!written || written.Value() != input.Size() ||
                platform::MemoryCompare(restored.Data(), input.Data(), input.Size()) != 0) {
                printf("%-8s %5d round trip failed\n", FormatName(item.format).text, item.level);
                return 5;
            }
            decompressSeconds = elapsed < decompressSeconds ? elapsed : decompressSeconds;
        }

        printf("%-8s %5d %8.3f %12.1f %12.1f\n", FormatName(item.format).text, item.level,
               static_cast<double>(packedBytes) / static_cast<double>(input.Size()),
               megabytes / compressSeconds, megabytes / decompressSeconds);
    }

    const int streams = RunStreams(input, options.rounds, allocator);
    if (streams != 0) {
        return streams;
    }
    const int blocks = RunBlocks(input, options.rounds, options.workers, allocator);
    if (blocks != 0) {
        return blocks;
    }
    const int dictionary = RunDictionary(input, options.rounds, allocator);
    if (dictionary != 0) {
        return dictionary;
    }
    return RunBrotli(options.brotli, input, options.rounds, allocator);
}

bool Parse(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const bool hasValue = index + 1 < argc;
        if (platform::CStringEquals(argv[index], "--file") && hasValue) {
            options.file = argv[++index];
        } else if (platform::CStringEquals(argv[index], "--brotli") && hasValue) {
            options.brotli = argv[++index];
        } else if (platform::CStringEquals(argv[index], "--size") && hasValue) {
            options.sizeMiB = 0;
            for (const char* digit = argv[++index]; *digit >= '0' && *digit <= '9'; ++digit) {
                options.sizeMiB = options.sizeMiB * 10 + static_cast<usize>(*digit - '0');
            }
            if (options.sizeMiB == 0 || options.sizeMiB > 4096) {
                return false;
            }
        } else if (platform::CStringEquals(argv[index], "--rounds") && hasValue) {
            options.rounds = 0;
            for (const char* digit = argv[++index]; *digit >= '0' && *digit <= '9'; ++digit) {
                options.rounds = options.rounds * 10 + static_cast<uint32>(*digit - '0');
            }
            if (options.rounds == 0 || options.rounds > 100) {
                return false;
            }
        } else if (platform::CStringEquals(argv[index], "--workers") && hasValue) {
            options.workers = 0;
            for (const char* digit = argv[++index]; *digit >= '0' && *digit <= '9'; ++digit) {
                options.workers = options.workers * 10 + static_cast<uint32>(*digit - '0');
            }
            if (options.workers == 0 || options.workers > 64) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!Parse(argc, argv, options)) {
        printf("usage: CompressionBench [--file <path>] [--size <MiB 1-4096>] [--rounds <1-100>] "
               "[--workers <1-64>] [--brotli <file.br>]\n");
        return 1;
    }
    return Run(options);
}
