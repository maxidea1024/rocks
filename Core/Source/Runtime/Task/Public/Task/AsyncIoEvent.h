#pragma once

#include <Platform/AsyncFile.h>
#include <Task/WorkerScheduler.h>

// 비동기 파일 IO(Platform/AsyncFile.h)의 완료를 WorkerTaskEvent로 잇는다. 읽기가 끝나면 그 버퍼를
// 쓰는 작업(해제·파싱)이 scheduler에서 실행되게 한다(Docs/Implementation/WorkerScheduler.ko.md,
// Docs/Design/Compression.ko.md 5절).
//
// 사용 예(압축된 entry를 비동기로 읽고 이어서 해제):
//   auto event = scheduler.TryCreateEvent();
//   auto after = Span<const WorkerTaskHandle>(&event.Value().Handle(), 1);
//   TrySubmitDecompress(scheduler, task, CompressionFormat::Zstd,
//                       Span<const uint8>(packed, packedBytes), rawBytes, output, after);
//   TrySubmitReadEvent(file, request, offset, packed, packedBytes, Move(event.Value()), allocator);
//   ...
//   auto bytes = task.TryWaitResult();   // 읽기가 끝나면 해제가 실행된다
//
// 계약:
// - 이벤트의 완료 권한은 요청이 가져간다. 함수는 완료 콜백 문맥(이벤트 포함)을 allocator에 만들고,
//   IO 스레드의 콜백이 이벤트를 완료한 뒤 그 문맥을 마지막으로 해제한다. 그래서 호출자는 콜백과
//   수명을 맞출 필요가 없다(AsyncFile은 완료를 게시한 뒤 콜백을 부르므로 request.Wait() 반환이 콜백
//   종료를 뜻하지 않는다). 이벤트 상태를 나중에 보려면 넘기기 전에 Handle().Clone()을 받아 둔다.
// - allocator는 IO 스레드의 해제를 허용해야 하고(System·Binned·Budget·Pool·External·Debug,
//   Arena·Stack 불가) 요청이 끝날 때까지 살아 있어야 한다. file·request·버퍼는 AsyncFile 계약대로
//   요청 완료까지 유효하다.
// - 완료 규칙(AsyncIoCompletionError): 성공이고 전송 byte가 요청 byte와 같으면 Trigger, 아니면
//   Fail이다. Fail 오류는 짧은 전송(파일 끝 등) InvalidArgument, 그 밖은
//   FileErrorCode(Core/FileError.h)다. 이 이벤트에 의존한 작업은 실행되지 않고 DependencyFailed가
//   된다. 이미 완료·취소된 이벤트(Shutdown 등)는 그대로 둔다.
// - 실패: 문맥 할당 실패는 그 오류이며 이벤트는 호출자에게 그대로 남는다(이동하지 않는다). 제출이
//   즉시 실패하면(열리지 않은 파일·잘못된 인자 등, 콜백 없음) 이벤트를 그 오류로 Fail하고 같은
//   오류를 돌려준다. 빈 이벤트는 StaleHandle이다.
namespace rocks {

// IO 결과를 이벤트 완료 오류로 바꾼다(None이면 Trigger).
ErrorCode AsyncIoCompletionError(const platform::AsyncIoResult& result,
                                 usize expectedBytes) noexcept;

Result<void> TrySubmitReadEvent(platform::AsyncReadFile& file, platform::AsyncIoRequest& request,
                                uint64 offset, void* destination, usize bytes,
                                WorkerTaskEvent&& event, Allocator& allocator) noexcept;

Result<void> TrySubmitWriteEvent(platform::AsyncWriteFile& file, platform::AsyncIoRequest& request,
                                 uint64 offset, const void* source, usize bytes,
                                 WorkerTaskEvent&& event, Allocator& allocator) noexcept;

} // namespace rocks
