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
    SystemError
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
    virtual FileStatus Delete(const char* path, usize pathBytes) noexcept = 0;
};

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
