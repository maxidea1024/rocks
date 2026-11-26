#pragma once

#include <Base/Compiler.h>
#include <Base/Types.h>

namespace rocks::platform {

// ParkingLot 대기 경로의 진단 훅 표다(Docs/Design/Synchronization 10.4·13.3절, D22·D30). 경합
// trace와 통계가 쓴다. 위 모듈(TraceCapture)이 설치하고 PlatformBase는 자리만 둔다. 설치하지 않으면
// 비용은 relaxed load 하나와 분기 하나다. 잠들지 않는 빠른 경로에는 불리지 않는다.
//
// - onWaitBegin: bucket을 잡기 전. onWaitEnd: 깨어 큐에서 나온 뒤.
// - 훅은 잠금 없는 스레드 지역 버퍼에 쓰기만 한다. 잠금·ParkingLot을 쓰면 안 된다(훅 안의 대기는
//   Debug·Development에서 Fatal, 훅 안에서 다시 불리는 훅은 건너뛴다).
struct SyncHooks {
    void (*onWaitBegin)(const void* address) noexcept = nullptr;
    void (*onWaitEnd)(const void* address) noexcept = nullptr;
};

// 훅 표를 바꾼다(nullptr 칸은 끈다). 설치는 세션 시작·종료처럼 대기 경로가 조용한 때 한다: 바꾸는
// 순간 대기 중인 스레드는 옛 훅과 새 훅을 섞어 부를 수 있다.
ROCKS_PLATFORMBASE_API void InstallSyncHooks(const SyncHooks& hooks) noexcept;

} // namespace rocks::platform
