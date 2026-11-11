#pragma once

#include <Platform/File.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>

namespace rocks::platform {

class AsyncReadFile;
class AsyncWriteFile;
class AsyncIoRequest;

// 비동기 파일 읽기의 완료 방식이다.
// - Native: 플랫폼의 커널 비동기 I/O. Windows는 overlapped I/O + IO completion port다. 이를
// 지원하지
//   않는 플랫폼(현재 macOS)에서는 Start가 Unsupported를 반환한다.
// - WorkerPool: worker 스레드가 위치 지정 동기 읽기(Windows ReadFile+OVERLAPPED offset, POSIX
//   pread)를 수행한다. 모든 플랫폼에서 같은 계약으로 동작하며 macOS·향후 모바일의 기본 경로다.
//   OS 비동기 I/O가 아니라 동기 호출을 worker에 위임한 것이므로 동시 진행 수는 worker 수다.
enum class AsyncIoBackend : uint8 {
    Native,
    WorkerPool
};

// 플랫폼 기본 backend다(Windows: Native, 그 밖: WorkerPool).
AsyncIoBackend DefaultAsyncIoBackend() noexcept;

// 요청 하나의 최종 결과다. 완료 통지는 요청마다 정확히 한 번이다.
// - 읽기 Success: bytes만큼 읽었다. offset이 파일 끝 이상이면 bytes 0의 성공(EOF), 끝에 걸치면
//   짧은 성공이다.
// - 쓰기 Success: 요청한 bytes를 모두 썼다(짧은 성공은 없다). 중간에 실패하면 실패 status와
// 그때까지
//   쓴 bytes를 보고한다(예: 디스크 가득 참). 진행 없이 0 byte만 쓰인 경우는 NoProgress다.
// - Cancelled: 취소가 실제로 적용되었다. bytes는 취소 전 전송량일 수 있으나 데이터는 신뢰하지
// 않는다.
//   취소 요청이 완료와 경합해 이미 끝난 경우는 Success이며 결과가 유효하다.
// - 그 밖의 실패: status와 nativeError로 원인을 보고한다.
struct AsyncIoResult {
    FileStatus status;
    usize bytes = 0;
};

using AsyncReadResult = AsyncIoResult; // 읽기 전용 시절의 이름

// 완료 callback이다. backend의 완료 스레드(Native: completion 스레드, WorkerPool: worker)에서
// 정확히 한 번 호출된다. 즉시 실패한 제출도 같은 스레드에서 통지한다(Submit 호출 스레드에서 직접
// 호출하지 않는다). callback이 호출될 때 요청은 이미 완료 상태로 게시되었으므로 Wait를 반환한
// 다른 스레드가 요청을 파괴했을 수 있다. callback은 요청 객체에 접근하지 않고 context만 사용한다.
// callback 안에서 같은 context의 다른 요청을 Wait하면 완료 스레드가 막히므로 금지한다. 새 Submit은
// 허용한다. noexcept이며 오래 걸리는 작업은 다른 작업 큐로 넘긴다.
using AsyncIoCallback = void (*)(const AsyncIoResult& result, void* context) noexcept;
using AsyncReadCallback = AsyncIoCallback; // 읽기 전용 시절의 이름

// 비동기 I/O 완료 처리기다. 파일과 요청보다 오래 살아야 한다.
// 사용 예(텍스처 mip 읽기):
//   AsyncIoContext io; io.Start(DefaultAsyncIoBackend());
//   AsyncReadFile pack; pack.Open(io, "Textures.pak");
//   AsyncReadRequest request;
//   pack.SubmitRead(request, mipOffset, mipBuffer, mipBytes);
//   ... 다른 작업 ...
//   if (request.Wait() == WaitResult::Signaled && request.Result().status) { Upload(mipBuffer); }
//   pack.Close(); io.Shutdown();
// 계약:
// - Start는 한 번만 성공한다. Shutdown은 열린 AsyncReadFile이 있으면 Busy이며 아무것도 바꾸지
// 않는다.
//   Shutdown 성공 뒤 다시 Start할 수 있다. 소멸 시 열린 파일이 남아 있으면 수명 오류로 Fatal이다.
// - 메서드는 여러 스레드에서 호출할 수 있다. Start·Shutdown·소멸은 다른 호출과 겹치지 않는다.
// - 완료 callback 안에서 Shutdown하거나 context를 파괴하지 않는다.
class AsyncIoContext {
public:
    AsyncIoContext() noexcept;
    ~AsyncIoContext() noexcept;
    AsyncIoContext(const AsyncIoContext&) = delete;
    AsyncIoContext& operator=(const AsyncIoContext&) = delete;

    // workerCount는 WorkerPool의 worker 수(1 이상, 0은 InvalidArgument)이며 Native는 완료 스레드
    // 1개를 쓴다. 이미 시작했으면 AlreadyOpen, 스레드·OS 객체 생성 실패는 원인 코드다.
    FileStatus Start(AsyncIoBackend backend, uint32 workerCount = 2) noexcept;
    FileStatus Shutdown() noexcept;
    bool IsRunning() const noexcept;
    AsyncIoBackend Backend() const noexcept;

private:
    friend class AsyncReadFile;
    friend class AsyncWriteFile;
    friend class AsyncIoRequest;
    alignas(16) unsigned char storage_[1024]{};
};

// 비동기 읽기 전용 파일이다. 같은 파일에 여러 요청을 동시에 제출할 수 있다(offset 지정 읽기이므로
// 파일 위치 개념이 없다). 파일 내용이 진행 중 바뀌는 경우의 결과는 OS 의미를 따른다.
// - Open의 경로 규약은 FileSystem::Open과 같다(유효한 UTF-8, embedded NUL 금지, 장치 경로 거부).
// - Close는 진행 중 요청이 있으면 Busy다. 소멸 시 진행 중 요청이 있으면 모두 취소하고 OS가 완료를
//   알릴 때까지 기다린 뒤 닫는다(버퍼 조기 해제 방지). 장치가 응답하지 않으면 이 대기는 끝나지 않을
//   수 있다.
class AsyncReadFile {
public:
    AsyncReadFile() noexcept;
    ~AsyncReadFile() noexcept;
    AsyncReadFile(const AsyncReadFile&) = delete;
    AsyncReadFile& operator=(const AsyncReadFile&) = delete;

    FileStatus Open(AsyncIoContext& context, const char* path) noexcept;
    FileStatus Open(AsyncIoContext& context, const char* path, usize pathBytes) noexcept;
    FileStatus Close() noexcept;
    bool IsOpen() const noexcept;
    FileStatus Size(uint64& bytes) const noexcept;

    // destination의 [0, bytes) 영역에 offset부터 읽는다. request와 destination은 완료(Wait 반환
    // 또는 callback 시작)까지 살아 있고 다른 용도로 쓰지 않아야 한다.
    // 즉시 반환 오류(요청 상태 변경 없음): 파일이 열려 있지 않음 NotOpen, request가 진행 중 Busy,
    // destination null(bytes>0)·bytes > 0xFFFFFFFF·offset+bytes overflow InvalidArgument,
    // context 종료 중 Busy. 그 밖의 OS 실패는 요청 결과로 통지한다.
    FileStatus SubmitRead(AsyncIoRequest& request, uint64 offset, void* destination, usize bytes,
                          AsyncIoCallback callback = nullptr,
                          void* callbackContext = nullptr) noexcept;

private:
    friend class AsyncIoRequest;
    alignas(16) unsigned char storage_[128]{};
};

// 비동기 쓰기 파일이다(캐시·저장 파일·로그 등). 여러 쓰기 요청을 서로 다른 offset에 동시에 제출할
// 수 있다. 겹치는 범위에 동시에 쓰면 결과 순서는 정해지지 않는다(OS 의미를 따른다). 사용 예(파생
// 데이터 캐시 저장):
//   AsyncWriteFile cache; cache.Open(io, "Derived.bin", FileOpenMode::CreateAlways);
//   AsyncIoRequest header, body;
//   cache.SubmitWrite(header, 0, headerBytes, headerSize);
//   cache.SubmitWrite(body, headerSize, payload, payloadSize);
//   header.Wait(); body.Wait();   // 두 결과의 status·bytes를 확인한다
//   cache.Flush(); cache.Close();
// 계약:
// - Open의 mode는 FileOpenMode의 ReadWriteExisting(기존 파일, 없으면 NotFound), CreateNew(이미
// 있으면
//   AlreadyExists), CreateAlways(있으면 비우고 만든다)만 받는다. ReadExisting은 InvalidArgument다.
//   경로 규약은 FileSystem::Open과 같다. 새 파일의 권한은 동기 File과 같다.
// - 파일 끝을 넘는 offset에 쓰면 파일이 늘어나며 그 사이는 0으로 채워진다(OS 의미).
// - Flush는 이미 완료된 쓰기를 저장 장치까지 내리는 동기 호출이다(Windows FlushFileBuffers, macOS
//   F_FULLFSYNC, 지원하지 않으면 fsync). 진행 중 요청이 있으면 Busy다. 오래 걸릴 수 있다.
// - Close·소멸·요청 수명 규칙은 AsyncReadFile과 같다. 소멸 시 진행 중 쓰기는 취소를 요청하고 완료를
//   기다리므로 일부만 쓰였을 수 있다.
class AsyncWriteFile {
public:
    AsyncWriteFile() noexcept;
    ~AsyncWriteFile() noexcept;
    AsyncWriteFile(const AsyncWriteFile&) = delete;
    AsyncWriteFile& operator=(const AsyncWriteFile&) = delete;

    FileStatus Open(AsyncIoContext& context, const char* path, FileOpenMode mode) noexcept;
    FileStatus Open(AsyncIoContext& context, const char* path, usize pathBytes,
                    FileOpenMode mode) noexcept;
    FileStatus Close() noexcept;
    bool IsOpen() const noexcept;
    FileStatus Size(uint64& bytes) const noexcept;
    FileStatus Flush() noexcept;

    // source의 [0, bytes) 영역을 offset부터 쓴다. request와 source는 완료까지 살아 있고 바뀌지
    // 않아야 한다. 즉시 반환 오류는 SubmitRead와 같다(source null·bytes 범위·overflow는
    // InvalidArgument).
    FileStatus SubmitWrite(AsyncIoRequest& request, uint64 offset, const void* source, usize bytes,
                           AsyncIoCallback callback = nullptr,
                           void* callbackContext = nullptr) noexcept;

private:
    friend class AsyncIoRequest;
    alignas(16) unsigned char storage_[128]{};
};

// 호출자가 소유하는 요청 레코드다(읽기·쓰기 공용). 진행 중에는 이동·복사·파괴하지 않는다.
// 완료 뒤에는 같은 객체로 다시 제출할 수 있다. 소멸 시 진행 중이면 취소 후 완료까지 기다린다.
class AsyncIoRequest {
public:
    AsyncIoRequest() noexcept;
    ~AsyncIoRequest() noexcept;
    AsyncIoRequest(const AsyncIoRequest&) = delete;
    AsyncIoRequest& operator=(const AsyncIoRequest&) = delete;

    // 완료 결과가 게시되었으면 true다. 제출 전에는 false다.
    bool IsComplete() const noexcept;
    // 완료까지 기다린다. 제출한 적이 없으면 Failed, timeout은 상태를 바꾸지 않는다.
    WaitResult Wait(uint32 timeoutMs = InfiniteTimeout) noexcept;
    // 취소를 요청한다. 즉시 완료나 버퍼 회수를 뜻하지 않으며 결과는 완료 통지로 확인한다.
    // 진행 중이 아니면 아무것도 하지 않는다. 여러 번 호출해도 된다.
    void Cancel() noexcept;
    // 완료된 요청의 결과다. 완료 전 호출은 Fatal이다.
    AsyncIoResult Result() const noexcept;

private:
    friend class AsyncReadFile;
    friend class AsyncWriteFile;
    friend class AsyncIoContext;
    alignas(16) unsigned char storage_[192]{};
};

using AsyncReadRequest = AsyncIoRequest; // 읽기 전용 시절의 이름

} // namespace rocks::platform
