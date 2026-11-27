# 작업 진행 보고와 취소 — 4. 취소 표지(`CancelToken`)를 Core로

[목차](README.ko.md)

## 4. 취소 표지(`CancelToken`)를 Core로

**구현(PG1, 2026-11-27).** `Core/Public/Core/CancelToken.h`·`Core/Private/Misc/CancelToken.cpp`(git 이동으로 이력 유지). 연결 취소는 상태에 부모 포인터와 사슬 깊이를 두고, 조회는 자기·부모 사슬의 원자 표시를 차례로 읽는다. 상태 해제는 마지막 참조를 놓은 상태에서 부모로 이어 가며 재귀 없이 한다. 시험은 `Core/Tests/CancelTokenTests.cpp`(14장 1·2).

`CancelToken`·`CancelSource`(`Engine/Source/Runtime/AssetRuntime/Public/AssetRuntime/CancelToken.h`)를 `Core/Public/Core/CancelToken.h`로 옮기고 export 매크로를 `ROCKS_CORE_API`로 바꾼다. 기존 계약(아무 스레드에서 Cancel·조회, 공유 상태는 원자 계수, `Create(allocator)`)은 그대로다. 하위호환 별칭 없이 호출처를 모두 바꾸고, 참조하는 문서(AssetSystem 11.1절, CoreObjectInputDetailedPlan.ko.md, EngineImplementationPlan.ko.md의 `CancelToken` 언급)도 갱신한다. 범용 기능을 한 곳에만 두는 규칙을 따른다.

더할 것:
- `Result<void> CancelToken::CheckCancelled() const noexcept` — 취소면 `ErrorCode::Cancelled`.
- **연결 취소.** `static Result<CancelSource> CancelSource::CreateLinked(Allocator&, const CancelToken& parent) noexcept`.
  - 자식 공유 상태가 부모 공유 상태를 참조 계수로 붙잡는다. 조회(`IsCancellationRequested` — Token·Source 둘 다)는 자기 표시를 보고, 아니면 부모 사슬을 따라 읽는다. 콜백 등록은 두지 않는다(임의 스레드 재진입을 부른다).
  - 사슬 깊이 상한 `MaxCancelChainDepth` = 8. 넘으면 `CapacityExceeded`.
  - 부모가 기본(취소될 수 없는) token이면 일반 source를 만든다.
- Task 모듈의 스케줄러 작업 취소(`WorkerScheduler.h`의 `RequestCancel`·`IsCancellationRequested`)는 스케줄러 내부 작업 단위의 취소이고 이 표지와 합치지 않는다. 진행 작업의 취소를 스케줄러 작업으로 옮기고 싶으면 호출자가 token을 보고 스케줄러 취소를 부른다.
