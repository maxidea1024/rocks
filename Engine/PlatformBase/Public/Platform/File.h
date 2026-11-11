#pragma once

#include <Base/Types.h>
#include <Platform/Sync.h>

namespace rocks::platform {

enum class FileCode {
    Success,
    InvalidArgument,
    AlreadyOpen,
    Busy,
    EndOfFile,
    NoProgress,
    NotOpen,
    NotFound,
    AlreadyExists,
    AccessDenied,
    OutOfMemory,
    Overflow,
    Unsupported,
    SystemError,
    // 비동기 요청이 취소되어 데이터가 유효하지 않다(AsyncReadResult). 동기 API는 반환하지 않는다.
    Cancelled,
    // 비어 있지 않은 디렉터리를 DeleteDirectory로 지우려 했다.
    DirectoryNotEmpty,
    // 파일 연산을 디렉터리에, 디렉터리 연산을 파일에 적용했다.
    WrongType
};

struct [[nodiscard]] FileStatus {
    FileCode code = FileCode::Success;
    uint32 nativeError = 0;

    explicit operator bool() const noexcept {
        return code == FileCode::Success;
    }
};

// 실패하더라도 bytes는 해당 호출에서 이미 전송된 바이트 수다. EOF는 성공한 0바이트다.
struct [[nodiscard]] FileTransfer {
    FileStatus status;
    usize bytes = 0;
};

enum class FileOpenMode {
    ReadExisting,
    ReadWriteExisting,
    CreateNew,
    // 기존 파일은 검사 완료 전에 잘릴 수 있으며 이후 Open 실패가 원본 복원을 의미하지 않는다.
    CreateAlways
};

enum class FileSeekOrigin {
    Begin,
    Current,
    End
};

// backend와 wrapper가 구현하는 단일 파일의 동작이다. 각 호출은 File에 의해 직렬화된다.
// Close는 한 번만 호출하며, 성공·실패와 관계없이 native 핸들 소유권을 끝낸다. OS가 실패한
// close의 최종 상태를 보장하지 않을 수 있으므로 자원 회수가 보장되지 않으며 이전 native
// 핸들로 재시도해서는 안 된다. OS close 실패에 대한 실제 강제 주입 검증은 아직 없다.
// Destroy는 Close 이후 인스턴스와 그 메타데이터를 해제하며 예외를 내지 않는다.
class FileHandle {
public:
    virtual FileTransfer Read(void* destination, usize bytes) noexcept = 0;
    virtual FileTransfer Write(const void* source, usize bytes) noexcept = 0;
    virtual FileStatus Seek(int64 offset, FileSeekOrigin origin, uint64& position) noexcept = 0;
    virtual FileStatus Size(uint64& bytes) noexcept = 0;
    virtual FileStatus Close() noexcept = 0;
    virtual void Destroy() noexcept = 0;

protected:
    virtual ~FileHandle() = default;
};

// 파일 시스템 항목의 종류다. Other는 장치·FIFO·socket 등이다.
enum class FileType : uint8 {
    File,
    Directory,
    Other
};

// 파일 시각의 단위다. 시각은 0001-01-01T00:00:00 UTC부터의 100ns tick이며(UE FDateTime tick과
// 같은 기준) 0은 알 수 없음이다. 운영체제·파일 시스템마다 실제 해상도가 다르다(FAT 2초 등).
inline constexpr int64 FileTicksPerSecond = 10000000;

// Stat·EnumerateDirectory가 채우는 항목 정보다.
// - Stat: link를 따라간 대상의 type·size·시각이며, isLink는 경로 자체가 link인지다.
// - EnumerateDirectory: link를 따라가지 않은 항목 자체의 정보다. link면 isLink이고 type은 OS가
//   보고하는 link의 종류(Windows 디렉터리 link·junction은 Directory, POSIX symlink는 Other)다.
// - size는 일반 파일의 byte 수이며 그 밖에는 0이다.
// - readOnly는 Windows 읽기 전용 속성 또는 POSIX에서 소유자 쓰기 권한이 없음을 뜻한다(접근 가능
//   여부를 보장하지 않는다).
struct FileInfo {
    FileType type = FileType::Other;
    bool isLink = false;
    bool readOnly = false;
    uint64 size = 0;
    int64 modifiedTicks = 0;
};

// EnumerateDirectory가 항목마다 넘기는 값이다. name은 "."·".."를 제외한 UTF-8 이름(경로 아님)이며
// NUL로 끝나고 callback 동안만 유효하다.
struct DirectoryEntry {
    const char* name = nullptr;
    usize nameBytes = 0;
    FileInfo info;
};

// false를 반환하면 열거를 멈춘다(결과는 성공). callback 안에서 같은 디렉터리를 바꾸면 이후 항목의
// 포함 여부는 OS 의미를 따른다.
using DirectoryVisitor = bool (*)(const DirectoryEntry& entry, void* context) noexcept;

// 경로는 유효한 UTF-8 byte 범위이며 embedded NUL이나 빈 경로는 허용하지 않는다.
// Open의 out은 처음에 nullptr여야 한다. 성공은 non-null handle 하나를 이전하고 실패는 out을
// 수정하지 않는다. 파일 시스템과 모든 wrapper는 자신이 반환한 handle이 Destroy될 때까지 살아 있어야
// 한다. wrapper는 하위 FileSystem과 그 핸들의 수명을 직접 유지해야 한다.
// Open을 직접 호출하면 반환한 핸들은 caller가 직렬화하고 Close 결과와 무관하게 Destroy를
// 정확히 한 번 호출한다.
class FileSystem {
public:
    virtual ~FileSystem() = default;
    virtual FileStatus Open(const char* path, usize pathBytes, FileOpenMode mode,
                            FileHandle*& out) noexcept = 0;
    virtual FileStatus Exists(const char* path, usize pathBytes, bool& exists) noexcept = 0;
    // 파일 또는 link(대상이 아니라 link 자체, 대상이 없어도)를 지운다. 실제 디렉터리는
    // WrongType이다(Windows에서 link가 아닌 reparse point가 붙은 디렉터리 포함).
    virtual FileStatus Delete(const char* path, usize pathBytes) noexcept = 0;

    // 아래 연산의 기본 구현은 Unsupported다. wrapper는 지원하는 것만 재정의한다. 물리 파일 시스템은
    // 모두 구현한다. 이름은 Windows SDK 매크로(CreateDirectory·RemoveDirectory·CopyFile·MoveFile)와
    // 겹치지 않게 정했다.

    // 경로 정보를 채운다. 없으면(대상이 없는 link 포함) NotFound이며 info는 바뀌지 않는다. 끝
    // 구분자가 있으면 OS 규칙대로 해석한다("file/"은 실패, "link/"의 isLink는 POSIX false·Windows
    // true). 파일 시각이 0001-01-01–9999-12-31 범위 밖이면 modifiedTicks는 0이다.
    virtual FileStatus Stat(const char* path, usize pathBytes, FileInfo& info) noexcept;
    // 디렉터리 하나를 만든다(부모는 있어야 한다). 이미 있으면(파일이어도) AlreadyExists다.
    virtual FileStatus MakeDirectory(const char* path, usize pathBytes) noexcept;
    // 빈 디렉터리를 지운다. 비어 있지 않으면 DirectoryNotEmpty, 파일이면 WrongType이다. 디렉터리
    // link는 link 자체를 지운다.
    virtual FileStatus DeleteDirectory(const char* path, usize pathBytes) noexcept;
    // 디렉터리의 직접 자식을 순서 보장 없이 visitor에 넘긴다. 하위 디렉터리로 내려가지 않는다.
    virtual FileStatus EnumerateDirectory(const char* path, usize pathBytes,
                                          DirectoryVisitor visitor, void* context) noexcept;
    // 같은 volume 안에서 이름을 바꾸거나 옮긴다(파일·디렉터리). replaceExisting이 false면 대상이
    // 있을 때 AlreadyExists다. 디렉터리는 replaceExisting이어도 기존 대상을 덮어쓰지 않고
    // (AlreadyExists), 파일과 디렉터리가 섞이면 WrongType이다. link는 가리키는 대상과 무관하게 link
    // 자체(디렉터리 아님)로 본다. 대상이 원본과 같은 파일 객체면 검사 없이 OS에 맡긴다: 대소문자만
    // 바꾸는 이름 변경은 모든 플랫폼에서 되지만, 같은 파일의 hard link 사이 이동은 Windows는 원본
    // 이름을 지우고 POSIX는 아무것도 바꾸지 않고 성공한다. 다른 volume 사이 이동은 Unsupported다
    // (복사 후 삭제는 호출자가 한다). 검사와 이동 사이에 다른 프로세스가 대상을 만들면 결과는 OS
    // 규칙을 따른다.
    virtual FileStatus Move(const char* from, usize fromBytes, const char* to, usize toBytes,
                            bool replaceExisting) noexcept;
    // 일반 파일 하나를 복사한다(디렉터리는 WrongType). replaceExisting이 false면 대상이 있을 때
    // AlreadyExists다. 실패하면 대상 파일을 남기지 않는다: replaceExisting으로 덮어쓰다 실패하면
    // 기존 대상도 없어진다(원자적 교체가 필요하면 임시 파일에 복사한 뒤 Move한다). 대상이 원본과
    // 같은 파일이면 원본을 지키기 위해 실패한다.
    virtual FileStatus Copy(const char* from, usize fromBytes, const char* to, usize toBytes,
                            bool replaceExisting) noexcept;
};

// FileSystem 기본 연산 위의 공용 helper다.
// - MakeDirectoryTree: 없는 부모까지 차례로 만든다. 이미 있는 디렉터리는 성공이며, 경로 중간에
//   파일이 있으면 WrongType이다.
// - DeleteDirectoryTree: 디렉터리와 그 안의 모든 항목을 지운다. link는 따라가지 않고 link 자체만
//   지운다(link 너머의 파일은 남는다). 경로 자체가 link면 끝 구분자가 있어도 link만 지우며, 대상이
//   없는 link도 지운다. 순환 link나 Windows에서 디렉터리를 가리키는 파일 link처럼 OS가 따라갈 수
//   없는 link를 루트로 넘기면 Stat 오류를 그대로 반환한다(아무것도 지우지 않는다). 마지막 구성
//   요소가 "."·".."이면 InvalidArgument다. 중간 실패는 그 상태로 멈추고 실패 원인을 반환한다(이미
//   지운 항목은 복구하지 않는다).
FileStatus MakeDirectoryTree(FileSystem& system, const char* path, usize pathBytes) noexcept;
FileStatus DeleteDirectoryTree(FileSystem& system, const char* path, usize pathBytes) noexcept;

// 실제 OS 파일 시스템. 객체는 상태가 없고 동시 호출이 가능하다. macOS는 일반 파일을 검사한다.
// Windows는 device namespace를 거부하고 FILE_TYPE_DISK를 검사하지만 링크·재분석 지점으로
// 연결된 대상까지 모든 장치를 검출하거나 일반 파일임을 보장하지는 못한다.
// 파일명 유효성/권한, symbolic link 추적 여부와 공유 모드 등은 운영체제의 의미를 따른다.
// 보안 경계가 필요한 호출자는 별도 핸들 기반 경로 제약을 사용해야 한다. Windows 장치
// namespace·확장 경로와 레거시 DOS 장치 이름은 Unsupported다. Exists는 OS가 해석한 링크
// 대상을 검사하며 결과와 이후 접근 사이의 변경을 막지 않는다. ReadAt/Flush/Truncate는 없다.
FileSystem& PhysicalFileSystem() noexcept;

// 사용 예: File file; if (auto s = file.Open("data.bin", FileOpenMode::ReadExisting))
// { auto read = file.Read(buffer, capacity); ...; auto closed = file.Close(); }
// Read/Write는 짧은 성공을 반환할 수 있다. Read의 0바이트 성공은 EOF다.
// 전체 전송은 ReadExact/WriteAll을 사용하고 결과 status와 누적 bytes를 확인한다.
// 소유권은 복사·이동하지 않는다. 같은 File의 호출은 직렬화되지만 외부의 같은 파일 핸들과
// 서로 다른 File 객체 사이 파일 위치나 파일 내용의 일관성은 보장하지 않는다. callback이
// 동일 File에 같은 스레드에서 재진입하면 Busy다. callback이 다른 스레드의 동일 File 호출을
// 기다리는 구성은 교착하므로 금지한다. IsOpen의 동일 스레드 재진입은 Fatal이다.
// 객체 파괴 전 모든 호출을 중지한다. Close 오류는 terminal이며 소멸 중 오류는 Fatal이다.
class File final {
public:
    File() noexcept = default;
    ~File() noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&&) = delete;
    File& operator=(File&&) = delete;

    // 첫 overload는 물리 파일 시스템을 사용한다. 지정한 FileSystem/wrapper를 사용하려면
    // 둘째 overload를 사용한다. path는 NUL로 끝나는 UTF-8이며 호출이 끝날 때까지 유효해야 한다.
    // 첫 NUL 이후는 볼 수 없다. embedded NUL을 검사해야 할 입력은 길이 지정 overload를 쓴다.
    FileStatus Open(const char* path, FileOpenMode mode) noexcept;
    FileStatus Open(FileSystem& system, const char* path, FileOpenMode mode) noexcept;
    FileStatus Open(FileSystem& system, const char* path, usize pathBytes,
                    FileOpenMode mode) noexcept;
    FileTransfer Read(void* destination, usize bytes) noexcept;
    FileTransfer Write(const void* source, usize bytes) noexcept;
    // 전체 요청 동안 파일 위치 잠금을 유지한다. 누적 bytes를 반환하며 ReadExact는 EOF에서
    // EndOfFile, WriteAll은 진행 없는 성공에서 NoProgress를 반환한다. 부분 실패도 누적한다.
    FileTransfer ReadExact(void* destination, usize bytes) noexcept;
    FileTransfer WriteAll(const void* source, usize bytes) noexcept;
    // 성공할 때만 output을 갱신한다. 같은 File 내 seek/read/write는 호출 단위로 직렬화한다.
    FileStatus Seek(int64 offset, FileSeekOrigin origin, uint64& position) noexcept;
    FileStatus Size(uint64& bytes) noexcept;
    FileStatus Close() noexcept;
    bool IsOpen() noexcept;

private:
    Mutex mutex_;
    FileHandle* handle_ = nullptr;
};

} // namespace rocks::platform
