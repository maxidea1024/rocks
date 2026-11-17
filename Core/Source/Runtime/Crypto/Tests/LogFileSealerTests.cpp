#include <TestSupport/Test.h>
#include <Core/Containers/Array.h>
#include <Core/Diagnostics/FileLogSink.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Move.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Crypto/LogFileSealer.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/File.h>

using namespace rocks;

namespace rocks::test {
uint64 OperatorNewCalls() noexcept;
} // namespace rocks::test

namespace {

constexpr LogCategory LogSealTests{"Tests.Seal"};

// 작업 디렉터리 아래 시험 전용 폴더다(소멸 때 지운다).
struct ScratchDirectory {
    ScratchDirectory() noexcept {
        static uint32 counter = 0;
        FixedStringBuilder text{Span<char>(root)};
        (void)text.TryAppendFormat(
            "rocks-logseal-{}-{}", platform::MonotonicNanoseconds(),
            platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&counter, 1u));
        length = text.ByteCount();
    }

    ~ScratchDirectory() noexcept {
        (void)platform::DeleteDirectoryTree(platform::PhysicalFileSystem(), root, length);
    }

    StringView View() const noexcept {
        return StringView(root, length);
    }

    char root[160]{};
    usize length = 0;
};

bool CountFile(const platform::DirectoryEntry& entry, void* raw) noexcept {
    if (entry.info.type == platform::FileType::File) {
        ++*static_cast<usize*>(raw);
    }

    return true;
}

usize CountFiles(const ScratchDirectory& directory) noexcept {
    usize count = 0;
    (void)platform::PhysicalFileSystem().EnumerateDirectory(directory.root, directory.length,
                                                            &CountFile, &count);
    return count;
}

// 폴더의 파일 이름을 모은다(최대 16개).
struct FileNames {
    char names[16][128]{};
    uint32 count = 0;
};

bool CollectName(const platform::DirectoryEntry& entry, void* raw) noexcept {
    auto& names = *static_cast<FileNames*>(raw);
    if (entry.info.type != platform::FileType::File || names.count == 16 ||
        entry.nameBytes >= sizeof(names.names[0])) {
        return true;
    }

    for (usize index = 0; index < entry.nameBytes; ++index) {
        names.names[names.count][index] = entry.name[index];
    }

    names.names[names.count][entry.nameBytes] = '\0';
    ++names.count;
    return true;
}

bool ReadAll(StringView path, Array<uint8>& bytes) noexcept {
    char native[MaxLogFilePathBytes];
    FixedStringBuilder text{Span<char>(native)};
    (void)text.TryAppend(path);
    platform::File file;
    uint64 size = 0;
    if (!file.Open(native, platform::FileOpenMode::ReadExisting) || !file.Size(size) ||
        !bytes.TryResize(static_cast<usize>(size))) {
        return false;
    }

    return static_cast<bool>(file.ReadExact(bytes.Data(), bytes.Size()).status);
}

// 복호한 평문을 모은다.
struct Collected {
    explicit Collected(Allocator& allocator) noexcept : bytes(allocator) {
    }

    StringView Text() const noexcept {
        return StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size());
    }

    Array<uint8> bytes;
};

Result<void> Collect(void* raw, Span<const uint8> plaintext) noexcept {
    return static_cast<Collected*>(raw)->bytes.TryAppend(plaintext);
}

FileLogSinkConfig Config(const ScratchDirectory& directory, LogFileCipher& cipher) noexcept {
    FileLogSinkConfig config;
    config.directory = directory.View();
    config.hostName = "Sealed";
    config.layout = LogLayout::Parse("[{Level}][{Category}] {Message}{NewLine}").Value();
    config.cipher = &cipher;
    return config;
}

void ResetUninstalledDrops() noexcept {
    SystemAllocator allocator;
    LogSystem reset(allocator, LogSystemConfig::ForTool());
    (void)reset.Start();
    LogInstallScope installed(reset);
}

// mode로 LogSystem을 돌려 기록 몇 줄을 봉인한 파일에 남기고 그 경로를 path에 둔다.
void WriteSealedLog(const ScratchDirectory& directory, LogFileCipher& cipher, LogMode mode,
                    Array<char>& path) noexcept {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    FileLogSink file(allocator, Config(directory, cipher));
    REQUIRE(file.Open().IsOk());
    REQUIRE(path.TryAppend(
        Span<const char>(file.CurrentPath().Data(), file.CurrentPath().ByteCount())));
    LogSystemConfig logConfig;
    logConfig.mode = mode;
    logConfig.globalMinimum = LogLevel::Trace;
    LogSystem logs(allocator, logConfig);
    REQUIRE(logs.AddSink(file).IsOk());
    REQUIRE(logs.Start().IsOk());
    {
        LogInstallScope installed(logs);
        for (uint32 index = 0; index < 5; ++index) {
            Log::Info(LogSealTests, "sealed record {Index} of {Count}", index, 5);
            if (mode == LogMode::Manual) {
                (void)logs.Pump();
            }
        }

        Log::Warning(LogSealTests, "secret token {Value}", "do-not-leak");
        (void)logs.Flush();
    }

    logs.Stop();
    file.Close();
}

constexpr StringView ExpectedText = "[Info][Tests.Seal] sealed record 0 of 5\n"
                                    "[Info][Tests.Seal] sealed record 1 of 5\n"
                                    "[Info][Tests.Seal] sealed record 2 of 5\n"
                                    "[Info][Tests.Seal] sealed record 3 of 5\n"
                                    "[Info][Tests.Seal] sealed record 4 of 5\n"
                                    "[Warning][Tests.Seal] secret token do-not-leak\n";

struct Recipient {
    Recipient() noexcept {
        auto generated = crypto::KemKeyPair::TryGenerate(crypto::KemAlgorithm::XWing);
        if (!generated) {
            platform::Fatal("X-Wing key generation failed");
        }

        pair = Move(generated.Value());
    }

    crypto::KemKeyPair pair;
};

bool ContainsBytes(const Array<uint8>& bytes, StringView needle) noexcept {
    return StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size())
        .Contains(needle, StringComparison::CaseSensitive);
}

} // namespace

// 봉인한 파일은 평문을 담지 않고, 개인키로 열면 같은 줄이 나온다. 정상 종료라 끝 표시가 있다.
TEST(LogFileSealerRoundTripsThroughDecode) {
    ScratchDirectory directory;
    Recipient recipient;
    crypto::LogFileSealer sealer(recipient.pair.PublicKey().View());
    SystemAllocator allocator;
    Array<char> path(allocator);
    WriteSealedLog(directory, sealer, LogMode::Sync, path);
    Array<uint8> file(allocator);
    REQUIRE(ReadAll(StringView(path.Data(), path.Size()), file));
    CHECK(!ContainsBytes(file, "do-not-leak") && !ContainsBytes(file, "sealed record"));
    CHECK(ContainsBytes(file, "RKLOGENC"));
    Collected plain(allocator);
    auto summary =
        crypto::TryDecodeLogFile(recipient.pair, file.View(), allocator, &Collect, &plain);
    REQUIRE(summary);
    CHECK(summary.Value().endMarker && !summary.Value().partialTail);
    CHECK(plain.Text() == ExpectedText);

    // 다른 개인키로는 열리지 않는다.
    Recipient other;
    Collected wrong(allocator);
    CHECK(crypto::TryDecodeLogFile(other.pair, file.View(), allocator, &Collect, &wrong).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(wrong.bytes.Size() == 0);
}

// Sync와 Async·Manual로 남긴 파일은 복호하면 같다.
TEST(LogFileSealerSyncAsyncAndManualDecodeIdentically) {
    Recipient recipient;
    crypto::LogFileSealer sealer(recipient.pair.PublicKey().View());
    SystemAllocator allocator;
    const LogMode modes[] = {LogMode::Sync, LogMode::Async, LogMode::Manual};
    for (LogMode mode : modes) {
        ScratchDirectory directory;
        Array<char> path(allocator);
        WriteSealedLog(directory, sealer, mode, path);
        Array<uint8> file(allocator);
        REQUIRE(ReadAll(StringView(path.Data(), path.Size()), file));
        Collected plain(allocator);
        auto summary =
            crypto::TryDecodeLogFile(recipient.pair, file.View(), allocator, &Collect, &plain);
        REQUIRE(summary);
        CHECK(summary.Value().endMarker);
        CHECK(plain.Text() == ExpectedText);
    }
}

// 끝 표시가 없는 파일(비정상 종료)은 마지막 완전한 묶음까지 열리고 endMarker가 거짓이다. 쓰다 만
// 꼬리는 버린다.
TEST(LogFileSealerDecodesUpToTheLastCompleteBatchWithoutFinal) {
    ScratchDirectory directory;
    Recipient recipient;
    crypto::LogFileSealer sealer(recipient.pair.PublicKey().View());
    SystemAllocator allocator;
    Array<char> path(allocator);
    WriteSealedLog(directory, sealer, LogMode::Sync, path);
    Array<uint8> file(allocator);
    REQUIRE(ReadAll(StringView(path.Data(), path.Size()), file));
    // 마지막 기록은 빈 평문 + Final(길이 4 + 17)이다. 그것을 떼면 비정상 종료한 파일이다.
    const usize finalRecord =
        crypto::LogFileRecordLengthBytes + crypto::SecretStreamWriter::OverheadBytes;
    REQUIRE(file.Size() > crypto::LogFileHeaderBytes + finalRecord);
    const Span<const uint8> missingFinal = file.View().Left(file.Size() - finalRecord);
    Collected plain(allocator);
    auto summary =
        crypto::TryDecodeLogFile(recipient.pair, missingFinal, allocator, &Collect, &plain);
    REQUIRE(summary);
    CHECK(!summary.Value().endMarker && !summary.Value().partialTail);
    CHECK(plain.Text() == ExpectedText);

    // 마지막 묶음을 쓰다 만 파일: 그 앞 묶음까지(Sync라 기록마다 묶음 하나) 나온다.
    const Span<const uint8> torn = missingFinal.Left(missingFinal.Size() - 5);
    Collected partial(allocator);
    auto tornSummary =
        crypto::TryDecodeLogFile(recipient.pair, torn, allocator, &Collect, &partial);
    REQUIRE(tornSummary);
    CHECK(!tornSummary.Value().endMarker && tornSummary.Value().partialTail);
    constexpr usize LastLine = sizeof("[Warning][Tests.Seal] secret token do-not-leak") - 1 + 1;
    CHECK(partial.Text() == StringView(ExpectedText.Data(), ExpectedText.ByteCount() - LastLine));
}

// 변조(기록 byte, 머리의 KEM 암호문, 끝 표시 뒤에 붙인 byte)는 인증 실패다.
TEST(LogFileSealerDetectsTampering) {
    ScratchDirectory directory;
    Recipient recipient;
    crypto::LogFileSealer sealer(recipient.pair.PublicKey().View());
    SystemAllocator allocator;
    Array<char> path(allocator);
    WriteSealedLog(directory, sealer, LogMode::Sync, path);
    Array<uint8> file(allocator);
    REQUIRE(ReadAll(StringView(path.Data(), path.Size()), file));
    const usize positions[] = {crypto::LogFileHeaderBytes + crypto::LogFileRecordLengthBytes + 3,
                               crypto::LogFileHeaderBytes - 40, 100};
    for (usize position : positions) {
        Array<uint8> copy(allocator);
        REQUIRE(copy.TryAppend(file.View()));
        copy[position] ^= 0x20u;
        Collected plain(allocator);
        CHECK(crypto::TryDecodeLogFile(recipient.pair, copy.View(), allocator, &Collect, &plain)
                  .Error() == ErrorCode::AuthenticationFailed);
    }

    Array<uint8> appended(allocator);
    REQUIRE(appended.TryAppend(file.View()));
    REQUIRE(appended.TryPushBack(uint8{0}));
    Collected plain(allocator);
    CHECK(crypto::TryDecodeLogFile(recipient.pair, appended.View(), allocator, &Collect, &plain)
              .Error() == ErrorCode::AuthenticationFailed);
    CHECK(plain.Text() == ExpectedText);

    // 머리의 예약 칸(version·kem 뒤 2 byte)이 0이 아니면 모르는 형식이다.
    Array<uint8> reserved(allocator);
    REQUIRE(reserved.TryAppend(file.View()));
    reserved[sizeof(crypto::LogFileMagic) + 2] = 1;
    CHECK(crypto::TryDecodeLogFile(recipient.pair, reserved.View(), allocator, &Collect, &plain)
              .Error() == ErrorCode::Unsupported);

    Array<uint8> notALog(allocator);
    REQUIRE(notALog.TryResize(crypto::LogFileHeaderBytes));
    CHECK(crypto::TryDecodeLogFile(recipient.pair, notALog.View(), allocator, &Collect, &plain)
              .Error() == ErrorCode::InvalidArgument);
}

// 키 준비가 실패하면(잘못된 공개키) 파일을 만들지 않는다. 평문이 남지 않는다.
TEST(LogFileSealerKeySetupFailureLeavesNoPlaintext) {
    ScratchDirectory directory;
    const uint8 shortKey[16] = {};
    crypto::LogFileSealer sealer{Span<const uint8>(shortKey)};
    SystemAllocator allocator;
    FileLogSink file(allocator, Config(directory, sealer));
    CHECK(file.Open().Error() == ErrorCode::InvalidArgument);
    CHECK(file.IsDisabled());
    LogEvent event;
    event.category = "Tests.Seal";
    event.message = "do-not-leak";
    CHECK(file.Write(event).Error() == ErrorCode::Unsupported);
    CHECK(CountFiles(directory) == 0);
}

// 회전한 파일은 파일마다 따로 열리고(새 캡슐화) 끝 표시가 있다. Open 뒤의 봉인·쓰기는 엔진
// allocator와 operator new를 쓰지 않는다(회전의 키 준비는 Crypto 키 저장소를 쓴다).
TEST(LogFileSealerRotatedFilesDecodeIndependently) {
    ScratchDirectory directory;
    Recipient recipient;
    crypto::LogFileSealer sealer(recipient.pair.PublicKey().View());
    SystemAllocator system;
    {
        // 무할당: 회전 없이 여러 묶음을 봉인한다.
        struct CountingAllocator final : Allocator {
            Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
                ++allocations;
                return inner.Allocate(bytes, alignment, tag);
            }

            Result<void> Deallocate(void* address) noexcept override {
                return inner.Deallocate(address);
            }

            SystemAllocator inner;
            uint64 allocations = 0;
        } counting;
        FileLogSink file(counting, Config(directory, sealer));
        REQUIRE(file.Open().IsOk());
        const uint64 allocations = counting.allocations;
        const uint64 news = test::OperatorNewCalls();
        LogEvent event;
        event.category = "Tests.Seal";
        event.message = "batch without allocation";
        for (uint32 index = 0; index < 50; ++index) {
            REQUIRE(file.Write(event).IsOk());
            REQUIRE(file.Flush().IsOk());
        }

        CHECK(counting.allocations == allocations);
        CHECK(test::OperatorNewCalls() == news);
    }

    ScratchDirectory rotated;
    FileLogSinkConfig config = Config(rotated, sealer);
    config.maxBytes = crypto::LogFileHeaderBytes + 200;
    config.retention = 8;
    uint32 rotations = 0;
    {
        FileLogSink file(system, config);
        REQUIRE(file.Open().IsOk());
        LogEvent event;
        event.category = "Tests.Seal";
        event.message = "rotated record with enough text to fill the file";
        for (uint32 index = 0; index < 12; ++index) {
            REQUIRE(file.Write(event).IsOk());
            REQUIRE(file.Flush().IsOk());
        }

        rotations = file.Rotations();
    }

    CHECK(rotations >= 2);
    CHECK(CountFiles(rotated) == rotations + 1);
    FileNames names;
    REQUIRE(platform::PhysicalFileSystem().EnumerateDirectory(rotated.root, rotated.length,
                                                              &CollectName, &names));
    REQUIRE(names.count == rotations + 1);
    uint64 records = 0;
    for (uint32 index = 0; index < names.count; ++index) {
        char path[MaxLogFilePathBytes];
        FixedStringBuilder built{Span<char>(path)};
        (void)built.TryAppendFormat("{}/{}", rotated.View(), StringView(names.names[index]));
        Array<uint8> bytes(system);
        REQUIRE(ReadAll(built.View(), bytes));
        Collected plain(system);
        auto summary =
            crypto::TryDecodeLogFile(recipient.pair, bytes.View(), system, &Collect, &plain);
        REQUIRE(summary);
        CHECK(summary.Value().endMarker);
        records += summary.Value().batches - 1;
    }

    CHECK(records == 12);
}
