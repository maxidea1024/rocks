#include "LogInternal.h"

#include <Core/Algorithms.h>
#include <Core/Containers/Array.h>
#include <Core/Diagnostics/FileLogSink.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Time/DateTime.h>
#include <Platform/Clock.h>
#include <Platform/Memory.h>

namespace rocks {

namespace {

// 같은 시각의 다른 실행과 이름이 겹칠 때 시도하는 번호 상한이다.
constexpr uint32 MaxLogFileParts = 100000;

// 이름 뒤 번호 조각("-" + MaxLogFileParts 미만의 10진수)의 최대 byte 수다.
constexpr usize MaxLogFilePartBytes = 6;

constexpr StringView LogFileExtension = ".log";

bool IsDigit(char value) noexcept {
    return value >= '0' && value <= '9';
}

// "YYYYMMDDTHHMMSSmmmZ"(LogFileStampBytes)인지다.
bool IsLogFileStamp(StringView text) noexcept {
    if (text.ByteCount() != LogFileStampBytes || text.Data()[8] != 'T' ||
        text.Data()[LogFileStampBytes - 1] != 'Z') {
        return false;
    }

    for (usize index = 0; index + 1 < LogFileStampBytes; ++index) {
        if (index != 8 && !IsDigit(text.Data()[index])) {
            return false;
        }
    }

    return true;
}

// name이 정확히 `<prefix><시각>[-<숫자>].log`인지다. 호스트 이름에 '-'가 있을 수 있으므로
// 접두만으로 고르지 않는다(Sandbox와 Sandbox-Server의 파일을 섞지 않는다).
bool IsHostLogFileName(StringView name, StringView prefix) noexcept {
    if (!name.StartsWith(prefix, StringComparison::CaseSensitive) ||
        !name.EndsWith(LogFileExtension, StringComparison::CaseSensitive) ||
        name.ByteCount() < prefix.ByteCount() + LogFileStampBytes + LogFileExtension.ByteCount()) {
        return false;
    }

    const StringView body(name.Data() + prefix.ByteCount(),
                          name.ByteCount() - prefix.ByteCount() - LogFileExtension.ByteCount());
    if (!IsLogFileStamp(StringView(body.Data(), LogFileStampBytes))) {
        return false;
    }

    const usize rest = body.ByteCount() - LogFileStampBytes;
    if (rest == 0) {
        return true;
    }

    const char* part = body.Data() + LogFileStampBytes;
    if (rest < 2 || rest > MaxLogFilePartBytes || part[0] != '-') {
        return false;
    }

    for (usize index = 1; index < rest; ++index) {
        if (!IsDigit(part[index])) {
            return false;
        }
    }

    return true;
}

bool IsHostByte(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '-';
}

bool IsSeparator(char value) noexcept {
    return value == '/' || value == '\\';
}

// Open이 지울 후보로 모으는 기존 파일 하나다.
struct ExistingLogFile {
    char name[MaxLogFilePathBytes];
    usize nameBytes = 0;
    int64 modifiedTicks = 0;
};

struct ExistingLogFiles {
    Array<ExistingLogFile>* files = nullptr;
    StringView prefix;
    bool failed = false;
};

// 이 호스트가 만든 이름(`<호스트>-<시각>[-<숫자>].log`)의 일반 파일을 모은다.
bool CollectLogFile(const platform::DirectoryEntry& entry, void* raw) noexcept {
    auto& context = *static_cast<ExistingLogFiles*>(raw);
    const StringView name(entry.name, entry.nameBytes);
    if (entry.info.type != platform::FileType::File || name.ByteCount() >= MaxLogFilePathBytes ||
        !IsHostLogFileName(name, context.prefix)) {
        return true;
    }

    auto added = context.files->TryEmplaceBack();
    if (!added) {
        context.failed = true;
        return false;
    }

    ExistingLogFile& file = *added.Value();
    platform::MemoryCopy(file.name, name.Data(), name.ByteCount());
    file.nameBytes = name.ByteCount();
    file.modifiedTicks = entry.info.modifiedTicks;
    return true;
}

} // namespace

FileLogSink::FileLogSink(Allocator& allocator, const FileLogSinkConfig& config) noexcept
    : allocator_(allocator), config_(config),
      fileSystem_(config.fileSystem != nullptr ? config.fileSystem
                                               : &platform::PhysicalFileSystem()),
      text_(LogTextOutput{this, &FileLogSink::AppendOutput, &FileLogSink::FlushOutput},
            config.layout),
      json_(LogTextOutput{this, &FileLogSink::AppendOutput, &FileLogSink::FlushOutput}) {
    // 문자열은 Open이 검사한다. 여기서는 잘라서라도 복사만 해 둔다(빌린 view를 보관하지 않는다).
    directoryBytes_ = config.directory.ByteCount() < sizeof(directory_)
                          ? config.directory.ByteCount()
                          : sizeof(directory_);
    platform::MemoryCopy(directory_, config.directory.Data(), directoryBytes_);
    hostBytes_ =
        config.hostName.ByteCount() < sizeof(host_) ? config.hostName.ByteCount() : sizeof(host_);
    platform::MemoryCopy(host_, config.hostName.Data(), hostBytes_);
    config_.directory = {};
    config_.hostName = {};
}

FileLogSink::~FileLogSink() noexcept {
    Close();
}

Result<void> FileLogSink::Open() noexcept {
    if (opened_) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    bool hostValid = hostBytes_ != 0 && hostBytes_ <= MaxLogFileHostBytes;
    for (usize index = 0; hostValid && index < hostBytes_; ++index) {
        hostValid = IsHostByte(host_[index]);
    }

    // 파일 이름: 호스트 + "-" + 시각(19) + "-N"(최대 7) + ".log". 폴더와 구분자를 더해도 남아야
    // 한다.
    const usize nameLimit =
        hostBytes_ + 1 + LogFileStampBytes + MaxLogFilePartBytes + LogFileExtension.ByteCount();
    if (!hostValid || directoryBytes_ == 0 || directoryBytes_ >= sizeof(directory_) ||
        directoryBytes_ + 1 + nameLimit >= MaxLogFilePathBytes || config_.maxBytes == 0 ||
        config_.batchBytes == 0 || config_.retention == 0 ||
        config_.retention > MaxLogFileRetention) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 끝 구분자를 뗀다(경로를 이어 붙일 때 하나만 넣는다).
    while (directoryBytes_ > 1 && IsSeparator(directory_[directoryBytes_ - 1])) {
        --directoryBytes_;
    }

    const usize overhead = config_.cipher != nullptr ? config_.cipher->SealOverhead() : 0;
    const usize header = config_.cipher != nullptr ? config_.cipher->HeaderBytes() : 0;
    auto batch = allocator_.Allocate(config_.batchBytes, alignof(uint64));
    if (!batch) {
        return Result<void>::Failure(batch.Error());
    }

    batch_ = static_cast<uint8*>(batch.Value());
    if (config_.cipher != nullptr) {
        sealedCapacity_ =
            (config_.batchBytes + overhead > header ? config_.batchBytes + overhead : header);
        auto sealed = allocator_.Allocate(sealedCapacity_, alignof(uint64));
        if (!sealed) {
            ReleaseBuffers();
            return Result<void>::Failure(sealed.Error());
        }

        sealed_ = static_cast<uint8*>(sealed.Value());
    }

    auto kept = allocator_.Allocate(static_cast<usize>(config_.retention) * MaxLogFilePathBytes,
                                    alignof(uint64));
    if (!kept) {
        ReleaseBuffers();
        return Result<void>::Failure(kept.Error());
    }

    kept_ = static_cast<char*>(kept.Value());
    keptFirst_ = 0;
    keptCount_ = 0;
    directory_[directoryBytes_] = '\0';
    if (!platform::MakeDirectoryTree(*fileSystem_, directory_, directoryBytes_)) {
        ReleaseBuffers();
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    // 이전 실행의 파일을 수정 시각 순서로 모아 새 파일 하나가 들어갈 자리만 남기고 지운다.
    {
        Array<ExistingLogFile> files(allocator_);
        char prefix[MaxLogFileHostBytes + 2];
        platform::MemoryCopy(prefix, host_, hostBytes_);
        prefix[hostBytes_] = '-';
        ExistingLogFiles existing{&files, StringView(prefix, hostBytes_ + 1), false};
        const platform::FileStatus listed = fileSystem_->EnumerateDirectory(
            directory_, directoryBytes_, &CollectLogFile, &existing);
        if (!listed || existing.failed) {
            ReleaseBuffers();
            return Result<void>::Failure(existing.failed ? ErrorCode::OutOfMemory
                                                         : ErrorCode::DependencyFailed);
        }

        algo::Sort(files.View(),
                   [](const ExistingLogFile& left, const ExistingLogFile& right) noexcept {
                       if (left.modifiedTicks != right.modifiedTicks) {
                           return left.modifiedTicks < right.modifiedTicks;
                       }

                       const usize shorter =
                           left.nameBytes < right.nameBytes ? left.nameBytes : right.nameBytes;
                       const int compared = platform::MemoryCompare(left.name, right.name, shorter);
                       return compared != 0 ? compared < 0 : left.nameBytes < right.nameBytes;
                   });
        const usize keep = config_.retention - 1;
        for (usize index = 0; index < files.Size(); ++index) {
            char path[MaxLogFilePathBytes];
            FixedStringBuilder built{Span<char>(path)};
            (void)built.TryAppend(StringView(directory_, directoryBytes_));
            (void)built.TryAppendAscii('/');
            (void)built.TryAppend(StringView(files[index].name, files[index].nameBytes));
            if (index + keep < files.Size()) {
                // 지우지 못해도(다른 프로세스가 열어 둠) 새 로그 쓰기는 계속한다.
                (void)fileSystem_->Delete(path, built.ByteCount());
            } else {
                Remember(built.View());
            }
        }
    }

    // 시작 시각이다. UTC를 읽지 못하면 단조 시각으로 대신한다(이름이 겹치면 번호가 붙는다).
    int64 ticks = 0;
    if (!platform::TryUtcTicks(ticks)) {
        ticks = static_cast<int64>(platform::MonotonicNanoseconds() / 100);
    }

    const auto now = DateTime::TryFromTicks(ticks);
    const DateTime time = now ? now.Value() : DateTime::MinValue();
    FixedStringBuilder stamp{Span<char>(stamp_)};
    (void)stamp.TryAppendFormat("{:04}{:02}{:02}T{:02}{:02}{:02}{:03}Z", time.Year(), time.Month(),
                                time.Day(), time.Hour(), time.Minute(), time.Second(),
                                time.Millisecond());
    opened_ = true;
    nextPart_ = 1;
    rotations_ = 0;
    fileBytes_ = 0;
    batchUsed_ = 0;
    auto first = OpenNextFile();
    if (!first) {
        opened_ = false;
        ReleaseBuffers();
        return first;
    }

    return Result<void>::Success();
}

void FileLogSink::Close() noexcept {
    if (!opened_) {
        return;
    }

    if (!disabled_) {
        // 렌더 sink가 모아 둔 것까지 묶음으로 내보낸 뒤 끝 표시를 쓴다.
        (void)(config_.format == LogFileFormat::Text ? text_.Flush() : json_.Flush());
        (void)FinishFile();
    }

    opened_ = false;
    ReleaseBuffers();
}

Result<void> FileLogSink::Write(const LogEvent& event) noexcept {
    if (!opened_ || disabled_) {
        return Result<void>::Failure(disabled_ ? ErrorCode::Unsupported : ErrorCode::NotFound);
    }

    outputFailed_ = false;
    auto written = config_.format == LogFileFormat::Text ? text_.Write(event) : json_.Write(event);
    if (!written) {
        return written;
    }

    return outputFailed_ ? Result<void>::Failure(ErrorCode::DependencyFailed)
                         : Result<void>::Success();
}

Result<void> FileLogSink::Flush() noexcept {
    if (!opened_ || disabled_) {
        return Result<void>::Failure(disabled_ ? ErrorCode::Unsupported : ErrorCode::NotFound);
    }

    return config_.format == LogFileFormat::Text ? text_.Flush() : json_.Flush();
}

Result<void> FileLogSink::AppendOutput(void* context, StringView text) noexcept {
    return static_cast<FileLogSink*>(context)->Append(text);
}

Result<void> FileLogSink::FlushOutput(void* context) noexcept {
    return static_cast<FileLogSink*>(context)->WriteBatch();
}

Result<void> FileLogSink::Append(StringView text) noexcept {
    usize offset = 0;
    while (offset < text.ByteCount()) {
        if (batchUsed_ == config_.batchBytes) {
            // 묶음이 찼다. flush를 기다리지 않고 지금까지를 먼저 쓴다.
            if (auto written = WriteBatch(); !written) {
                outputFailed_ = true;
                return written;
            }
        }

        const usize room = config_.batchBytes - batchUsed_;
        const usize remaining = text.ByteCount() - offset;
        const usize count = remaining < room ? remaining : room;
        platform::MemoryCopy(batch_ + batchUsed_, text.Data() + offset, count);
        batchUsed_ += count;
        offset += count;
    }

    return Result<void>::Success();
}

Result<void> FileLogSink::WriteBatch() noexcept {
    if (disabled_) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (batchUsed_ == 0) {
        return Result<void>::Success();
    }

    // 회전은 봉인 전에 판정한다(봉인 상태는 파일마다 새로 만든다).
    const usize overhead = config_.cipher != nullptr ? config_.cipher->SealOverhead() : 0;
    if (fileOpen_ && fileBytes_ > headerBytes_ &&
        fileBytes_ + batchUsed_ + overhead > config_.maxBytes) {
        (void)FinishFile();
        ++rotations_;
    }

    if (!fileOpen_) {
        if (auto opened = OpenNextFile(); !opened) {
            return opened;
        }
    }

    const uint8* bytes = batch_;
    usize count = batchUsed_;
    if (config_.cipher != nullptr) {
        auto sealed = config_.cipher->TrySeal(Span<const uint8>(batch_, batchUsed_), false,
                                              Span<uint8>(sealed_, sealedCapacity_));
        if (!sealed) {
            return Result<void>::Failure(sealed.Error());
        }

        bytes = sealed_;
        count = sealed.Value();
    }

    auto written = WriteRaw(bytes, count);
    if (written) {
        batchUsed_ = 0;
    }

    return written;
}

Result<void> FileLogSink::WriteRaw(const uint8* bytes, usize count) noexcept {
    const platform::FileTransfer written = file_.WriteAll(bytes, count);
    fileBytes_ += written.bytes;
    if (!written.status) {
        // 이 파일은 더 쓰지 않는다. 다음 묶음이 새 파일을 연다(회전과 같은 경로).
        (void)file_.Close();
        fileOpen_ = false;
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    return Result<void>::Success();
}

// 지금 파일에 끝 표시(cipher)를 쓰고 닫는다. 남은 묶음은 쓰지 않는다(Close는 먼저 flush하고,
// 회전은 그 묶음을 다음 파일에 쓴다).
Result<void> FileLogSink::FinishFile() noexcept {
    if (!fileOpen_) {
        return Result<void>::Success();
    }

    Result<void> outcome = Result<void>::Success();
    if (config_.cipher != nullptr) {
        auto sealed = config_.cipher->TrySeal({}, true, Span<uint8>(sealed_, sealedCapacity_));
        outcome =
            sealed ? WriteRaw(sealed_, sealed.Value()) : Result<void>::Failure(sealed.Error());
    }

    if (fileOpen_) {
        (void)file_.Flush();
        if (!file_.Close()) {
            outcome = Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        fileOpen_ = false;
    }

    return outcome;
}

Result<void> FileLogSink::OpenNextFile() noexcept {
    // 머리를 먼저 만든다. 키 준비가 실패하면 파일을 만들지 않는다(평문으로 내려가지 않는다).
    usize header = 0;
    if (config_.cipher != nullptr) {
        auto written = config_.cipher->TryWriteHeader(Span<uint8>(sealed_, sealedCapacity_));
        if (!written) {
            Disable(written.Error());
            return Result<void>::Failure(written.Error());
        }

        header = written.Value();
    }

    for (; nextPart_ < MaxLogFileParts; ++nextPart_) {
        FixedStringBuilder built{Span<char>(path_)};
        (void)built.TryAppend(StringView(directory_, directoryBytes_));
        (void)built.TryAppendAscii('/');
        (void)built.TryAppend(StringView(host_, hostBytes_));
        (void)built.TryAppendAscii('-');
        (void)built.TryAppend(StringView(stamp_));
        if (nextPart_ > 1) {
            (void)built.TryAppendFormat("-{}", nextPart_);
        }

        (void)built.TryAppend(LogFileExtension);
        pathBytes_ = built.ByteCount();
        const platform::FileStatus opened =
            file_.Open(*fileSystem_, path_, pathBytes_, platform::FileOpenMode::CreateNew);
        if (opened.code == platform::FileCode::AlreadyExists) {
            continue;
        }

        if (!opened) {
            pathBytes_ = 0;
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        ++nextPart_;
        fileOpen_ = true;
        fileBytes_ = 0;
        headerBytes_ = header;
        Remember(StringView(path_, pathBytes_));
        if (header != 0) {
            return WriteRaw(sealed_, header);
        }

        return Result<void>::Success();
    }

    pathBytes_ = 0;
    return Result<void>::Failure(ErrorCode::CapacityExceeded);
}

void FileLogSink::Remember(StringView path) noexcept {
    if (kept_ == nullptr) {
        return;
    }

    if (keptCount_ == config_.retention) {
        // 가장 오래된 파일을 지우고 그 칸을 쓴다. 지우지 못해도 쓰기는 계속한다.
        const char* oldest = kept_ + static_cast<usize>(keptFirst_) * MaxLogFilePathBytes;
        (void)fileSystem_->Delete(oldest, platform::CStringLength(oldest));
        keptFirst_ = (keptFirst_ + 1) % config_.retention;
        --keptCount_;
    }

    const uint32 slot = (keptFirst_ + keptCount_) % config_.retention;
    char* target = kept_ + static_cast<usize>(slot) * MaxLogFilePathBytes;
    const usize bytes =
        path.ByteCount() < MaxLogFilePathBytes ? path.ByteCount() : MaxLogFilePathBytes - 1;
    platform::MemoryCopy(target, path.Data(), bytes);
    target[bytes] = '\0';
    ++keptCount_;
}

void FileLogSink::Disable(ErrorCode error) noexcept {
    if (disabled_) {
        return;
    }

    disabled_ = true;
    char storage[LogNoticeBytes];
    FixedStringBuilder text{Span<char>(storage)};
    (void)text.TryAppendFormat("Log file encryption setup failed ({}); the file log sink is "
                               "disabled and writes nothing (no plaintext fallback)",
                               ErrorCodeName(error));
    detail::WriteLogEmergencyText(LogLevel::Error, LogLogging.Name(), text.View());
}

void FileLogSink::ReleaseBuffers() noexcept {
    if (fileOpen_) {
        (void)file_.Close();
        fileOpen_ = false;
    }

    void* buffers[] = {batch_, sealed_, kept_};
    for (void* memory : buffers) {
        if (memory != nullptr) {
            (void)allocator_.Deallocate(memory);
        }
    }

    batch_ = nullptr;
    sealed_ = nullptr;
    kept_ = nullptr;
    sealedCapacity_ = 0;
    batchUsed_ = 0;
    keptCount_ = 0;
    keptFirst_ = 0;
}

} // namespace rocks
