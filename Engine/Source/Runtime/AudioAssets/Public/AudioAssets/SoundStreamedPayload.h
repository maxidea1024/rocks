#pragma once

#include <Core/Containers/Span.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>

// sidecar 압축 payload의 범위 읽기 경계다(Docs/Design/AudioSystem 3.46.3절). 엔진 구현은
// AssetManager::OpenStreamedBulk의 StreamedBulk를 감싸고(EngineAudio StreamedBulkPayload), 시험은
// 가짜를 쓴다. AssetManager의 스레드 규칙(Game 스레드 열기·AudioRT 금지·callback 스레드)을 오디오
// 계약 하나로 감싼다.
//
// 계약(StreamedBulk와 같다 — AssetSystem 20.2 3·20.6):
// - Read·Cancel·SetPriority는 아무 스레드(AudioRT 제외 — 할당한다).
// - 완료 callback은 아무 스레드에서 정확히 한 번이다. Read가 손잡이를 돌려주기 전일 수도, Read·
//   Cancel을 부른 스레드일 수도 있다. 그래서 호출자는 자기 잠금을 쥐지 않고 부르고, Read 전에
//   결과를 받을 자리를 준비한다. SetPriority는 callback을 부르지 않는다.
// - callback 안에서 Read·Cancel·SetPriority를 다시 불러도 되고 자기 손잡이를 파괴해도 된다.
// - 즉시 실패(범위·할당·종료)는 Read 반환값이고 callback이 없다.
// - destination은 callback이 돌아올 때까지 산다. Cancelled면 내용을 정하지 않는다.
// - Pending인 손잡이의 소멸은 금지다(구현이 Fatal — Cancel한 뒤 callback을 받고 놓는다).
//
// 사용 예:
//   auto read = payload->Read(offset, buffer, SoundReadPriority::High, &OnLoaded, &slot);
//   // OnLoaded(&slot, ErrorCode::None)가 정확히 한 번 불린다.
namespace rocks {

// 읽기 우선순위다(UE EAsyncIOPriorityAndFlags 축약 — 미리 읽기 Normal, 재생 필요 High, 굶음
// Critical).
enum class SoundReadPriority : uint8 {
    Low,
    Normal,
    High,
    Critical
};

enum class SoundReadState : uint8 {
    Pending,
    Done,
    Failed,
    Cancelled
};

// 완료 callback이다. error가 None이면 destination에 요청 범위가 다 있다(검증된 byte).
using SoundReadCallback = void (*)(void* context, ErrorCode error) noexcept;

// 읽기 요청 하나의 손잡이다.
class ROCKS_AUDIOASSETS_API SoundPayloadRequest {
public:
    virtual ~SoundPayloadRequest() = default;

    virtual SoundReadState State() const noexcept = 0;

    // 취소를 요청한다. callback은 Cancelled 또는 원래 결과로 정확히 한 번이다(그 자리일 수 있다).
    virtual void Cancel() noexcept = 0;

    // 아직 제출하지 않은 요청의 순서를 바꾼다(callback을 부르지 않는다).
    virtual void SetPriority(SoundReadPriority priority) noexcept = 0;
};

using SoundPayloadRead = UniquePtr<SoundPayloadRequest>;

// sidecar payload 영역 하나다(플랫폼 데이터의 payload 영역 — 오프셋 0이 payload 시작).
class ROCKS_AUDIOASSETS_API SoundStreamedPayload {
public:
    virtual ~SoundStreamedPayload() = default;

    // payload byte 수다.
    virtual uint64 Bytes() const noexcept = 0;

    // destination에 [offset, offset + destination.Size())를 읽기 시작한다. 범위가 Bytes()를 넘거나
    // callback이 null이면 InvalidArgument, 종료 뒤 ShuttingDown, 할당 실패 OutOfMemory(모두
    // callback 없음)다.
    virtual Result<SoundPayloadRead> Read(uint64 offset, Span<uint8> destination,
                                          SoundReadPriority priority, SoundReadCallback callback,
                                          void* context) noexcept = 0;
};

} // namespace rocks
