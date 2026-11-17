#pragma once

#include <Core/Diagnostics/LogFileCipher.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/Allocator.h>
#include <Platform/File.h>

namespace rocks {

// 파일 sink 경로의 최대 byte 수(NUL 포함)다. 디렉터리·파일 이름을 합친 길이가 넘으면 Open이
// InvalidArgument다.
inline constexpr usize MaxLogFilePathBytes = 1024;

// 파일 이름 앞부분(호스트 이름)의 최대 byte 수다.
inline constexpr usize MaxLogFileHostBytes = 32;

// 보관 개수 상한의 최대값이다.
inline constexpr uint32 MaxLogFileRetention = 256;

// 파일 내용 형식이다. Text는 LogLayout 줄(TextLogSink), JsonLines는 JsonLinesLogSink 줄이다.
enum class LogFileFormat : uint8 {
    Text,
    JsonLines
};

// 파일 sink 설정이다(설정 시스템 연결 log.file.*은 LG9). 문자열은 Open까지만 빌린다(복사한다).
struct FileLogSinkConfig {
    static constexpr uint64 DefaultMaxBytes = 16ull * 1024 * 1024;
    static constexpr uint32 DefaultRetention = 8;
    static constexpr usize DefaultBatchBytes = 64 * 1024;

    // 로그 폴더(UTF-8)다. 없으면 Open이 만든다.
    StringView directory;
    // 파일 이름 앞부분이다(ASCII 영문·숫자·'_'·'-', 1–MaxLogFileHostBytes). 보통 실행 파일
    // 이름이다.
    StringView hostName = "Rocks";
    LogFileFormat format = LogFileFormat::Text;
    // Text 형식의 줄 모양이다.
    LogLayout layout = LogLayout::DefaultConsole();
    // 파일 하나의 크기 상한이다. 다음 묶음을 쓰면 넘을 때 새 파일로 회전한다(묶음 하나가 상한보다
    // 크면 그 묶음만 담은 파일이 상한을 넘을 수 있다).
    uint64 maxBytes = DefaultMaxBytes;
    // 폴더에 남길 이 호스트의 로그 파일 수(현재 파일 포함, 1–MaxLogFileRetention)다. 넘으면 수정
    // 시각이 오래된 것부터 지운다.
    uint32 retention = DefaultRetention;
    // flush 묶음 버퍼 크기다(Open에서 확보). 묶음이 차면 flush 전이라도 그만큼을 먼저 쓴다.
    usize batchBytes = DefaultBatchBytes;
    // 묶음 봉인(암호화)이다. null이면 평문이다. sink보다 오래 살아야 한다.
    LogFileCipher* cipher = nullptr;
    // null이면 platform::PhysicalFileSystem이다. sink보다 오래 살아야 한다.
    platform::FileSystem* fileSystem = nullptr;
};

// 로그를 파일에 쓰는 sink다(Docs/Design/LoggingSystem.ko.md 9.1절).
//
// - 실행마다 새 파일 `<폴더>/<호스트>-<UTC 시작 시각>.log`를 만든다(시각은
//   `YYYYMMDDTHHMMSSmmmZ`). 회전한 파일과 같은 시각의 다른 실행은 `-2`, `-3` … 을 붙인다.
// - 렌더는 기존 sink(TextLogSink·JsonLinesLogSink)를 그대로 쓰고, 그 출력을 flush 묶음 버퍼에 모아
//   LogSystem의 묶음 flush마다 한 번 파일에 쓴다(cipher가 있으면 묶음 하나가 봉인 메시지 하나).
// - 메모리: 묶음·봉인 버퍼와 보관 목록은 Open에서 확보한다. 그 뒤 Write·Flush·회전은 엔진
//   allocator와 operator new를 쓰지 않는다(파일 열기의 OS 자원은 PlatformBase가 맡는다).
// - 실패(설계 10절): 파일 쓰기·열기 실패는 Write·Flush가 DependencyFailed로 돌려주고 LogSystem의
//   sink 막기·재시도가 다룬다. 회전에 실패해도 멈추지 않고 다음 묶음에서 다시 연다. cipher의 머리
//   (키 준비)가 실패하면 평문으로 내려가지 않는다: 그 파일을 만들지 않고 sink를 끄고(이후 Write·
//   Flush는 Unsupported) 비상 경로로 한 번 알린다.
// - 호출은 직렬화한다(LogSystem이 sink lock 안에서 부른다). Open·Close는 LogSystem에 등록하기 전과
//   뺀 뒤(또는 Stop 뒤)에 부른다.
//
// 사용 예:
//   FileLogSinkConfig config;
//   config.directory = "Saved/Logs";
//   config.hostName = "Sandbox";
//   FileLogSink file(allocator, config);
//   if (file.Open()) { logs.AddSink(file); }
//   ... logs.Stop();
//   file.Close();   // 남은 묶음과 끝 표시를 쓰고 닫는다(소멸자도 부른다)
class FileLogSink final : public RuntimeLogSink {
public:
    FileLogSink(Allocator& allocator, const FileLogSinkConfig& config) noexcept;
    ~FileLogSink() noexcept override;
    FileLogSink(const FileLogSink&) = delete;
    FileLogSink& operator=(const FileLogSink&) = delete;

    // 버퍼를 확보하고 폴더를 만들고 보관 상한을 적용한 뒤 첫 파일을 연다.
    // 실패: 잘못된 설정(빈 폴더·호스트 이름 문법·경로 길이·0 크기·보관 범위) InvalidArgument, 이미
    // 열림 AlreadyExists, allocator 오류, 파일 시스템 오류 DependencyFailed, cipher 머리 실패는 그
    // 오류(sink는 꺼진다).
    Result<void> Open() noexcept;

    // 남은 묶음을 쓰고 끝 표시(cipher)를 쓴 뒤 파일을 닫고 버퍼를 돌려준다. 두 번 불러도 된다.
    void Close() noexcept;

    Result<void> Write(const LogEvent& event) noexcept override;
    Result<void> Flush() noexcept override;

    // 지금 쓰는 파일의 경로다(열린 파일이 없으면 빈 값). 다음 회전 전까지 유효하다.
    StringView CurrentPath() const noexcept {
        return StringView(path_, pathBytes_);
    }

    // cipher 머리 실패로 꺼졌는지다.
    bool IsDisabled() const noexcept {
        return disabled_;
    }

    // 회전으로 새로 연 파일 수다(첫 파일 제외).
    uint32 Rotations() const noexcept {
        return rotations_;
    }

private:
    static Result<void> AppendOutput(void* context, StringView text) noexcept;
    static Result<void> FlushOutput(void* context) noexcept;

    Result<void> Append(StringView text) noexcept;
    Result<void> WriteBatch() noexcept;
    Result<void> OpenNextFile() noexcept;
    Result<void> FinishFile() noexcept;
    Result<void> WriteRaw(const uint8* bytes, usize count) noexcept;
    void Remember(StringView path) noexcept;
    void Disable(ErrorCode error) noexcept;
    void ReleaseBuffers() noexcept;

    Allocator& allocator_;
    FileLogSinkConfig config_;
    platform::FileSystem* fileSystem_;
    char directory_[MaxLogFilePathBytes]{};
    usize directoryBytes_ = 0;
    char host_[MaxLogFileHostBytes + 1]{};
    usize hostBytes_ = 0;
    // UTC 시작 시각 "YYYYMMDDTHHMMSSmmmZ"(19 byte)다.
    char stamp_[24]{};
    char path_[MaxLogFilePathBytes]{};
    usize pathBytes_ = 0;
    platform::File file_;
    bool fileOpen_ = false;
    uint64 fileBytes_ = 0;
    uint64 headerBytes_ = 0;
    uint32 nextPart_ = 1;
    uint32 rotations_ = 0;
    // flush 묶음과 봉인 출력이다(Open에서 확보).
    uint8* batch_ = nullptr;
    usize batchUsed_ = 0;
    uint8* sealed_ = nullptr;
    usize sealedCapacity_ = 0;
    // 보관 목록: 오래된 것부터 retention칸의 경로 고리다(Open에서 확보).
    char* kept_ = nullptr;
    uint32 keptFirst_ = 0;
    uint32 keptCount_ = 0;
    bool opened_ = false;
    bool disabled_ = false;
    bool outputFailed_ = false;
    TextLogSink text_;
    JsonLinesLogSink json_;
};

} // namespace rocks
