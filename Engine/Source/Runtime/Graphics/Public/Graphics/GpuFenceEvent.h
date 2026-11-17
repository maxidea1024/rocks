#pragma once

#include <Graphics/Device.h>
#include <Task/WorkerScheduler.h>

// GPU fence 완료를 Core 작업 이벤트(WorkerTaskEvent)로 잇는다(RenderBase 4.4절 GX7). 비동기 파일
// IO의 Task/AsyncIoEvent.h와 같은 방식이다: GPU가 제출을 끝내면 그 결과를 쓰는 작업(readback
// 해석·회수 통지)이 scheduler에서 실행된다.
//
// 사용 예:
//   auto event = scheduler.TryCreateEvent();
//   auto after = Span<const WorkerTaskHandle>(&event.Value().Handle(), 1);
//   auto parse = scheduler.Submit(Move(body), after);
//   TrySubmitFenceEvent(device, device.LastSubmittedFence(), Move(event.Value()), allocator);
//
// 계약:
// - 이벤트의 완료 권한은 이 요청이 가져간다. 함수는 통지 문맥(이벤트 포함)을 allocator에 만들고,
//   장치의 fence 통지가 이벤트를 완료한 뒤 그 문맥을 해제한다. 호출자는 통지와 수명을 맞출 필요가
//   없다. 이벤트 상태를 나중에 보려면 넘기기 전에 Handle().Clone()을 받아 둔다.
// - allocator는 다른 스레드(장치의 fence 관찰 스레드)의 해제를 허용해야 한다(System·Binned·
//   Budget·Pool·External·Debug, Arena·Stack 불가). 장치보다 오래 살아야 한다.
// - 완료 규칙: GPU 완료면 Trigger, 장치 손실이면 Fail(Destroyed), 장치가 먼저 닫히면
//   Fail(ShuttingDown)이다. 이 이벤트에 의존한 작업은 실패면 DependencyFailed가 된다. 이미
//   완료·취소된 이벤트(scheduler Shutdown 등)는 그대로 둔다.
// - 실패: 문맥 할당 실패는 그 오류이며 이벤트는 호출자에게 그대로 남는다. 장치가 지점을 거부하면
//   (WrongContext·InvalidArgument·ShuttingDown) 이벤트를 그 오류로 Fail하고 같은 오류를 돌려준다.
//   빈 이벤트는 StaleHandle이다.
// - 스레드: 어느 스레드에서나 부를 수 있다(Device::NotifyFence가 스레드 안전).
namespace rocks::graphics {

Result<void> TrySubmitFenceEvent(Device& device, GpuFencePoint point, WorkerTaskEvent&& event,
                                 Allocator& allocator) noexcept;

} // namespace rocks::graphics
