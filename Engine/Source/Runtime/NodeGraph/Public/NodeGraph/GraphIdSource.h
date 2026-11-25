#pragma once

#include <Base/Guid.h>
#include <Core/Result.h>

// 그래프 id(GraphId·NodeId·주석 id) 발급기다(Docs/Design/NodeGraph 8.5·8.6절).
//
// 사용 예:
//   RandomGraphIdSource random;                       // 에디터
//   SeededGraphIdSource seeded(42, assetGuid);         // 시험·template·결정적 import
//   auto id = seeded.Next();
namespace rocks::nodegraph {

class ROCKS_NODEGRAPH_API GraphIdSource {
public:
    virtual ~GraphIdSource() noexcept = default;

    // 새 id다. 0이 아니다. 발급 실패(무작위 원천 오류 등)는 그 오류다.
    virtual Result<Guid> Next() noexcept = 0;
};

// 무작위 UUID v4(TryNewGuid)다.
class ROCKS_NODEGRAPH_API RandomGraphIdSource final : public GraphIdSource {
public:
    Result<Guid> Next() noexcept override;
};

// (seed, 대상 자산 id, 순번)에서 유도한 id다: SHA-256(seed u64 LE ‖ asset.high LE ‖ asset.low LE ‖
// 순번 u64 LE)의 앞 16바이트(high = 0–7, low = 8–15, little-endian). 0이면 다음 순번이다. 같은
// seed·asset·호출 순서면 같은 id다. 스레드: 상태(순번)가 있으므로 소유 lane 하나에서 쓴다.
class ROCKS_NODEGRAPH_API SeededGraphIdSource final : public GraphIdSource {
public:
    SeededGraphIdSource(uint64 seed, Guid asset) noexcept : seed_(seed), asset_(asset) {
    }

    Result<Guid> Next() noexcept override;

    // 다음 Next가 쓸 순번이다.
    uint64 Counter() const noexcept {
        return counter_;
    }

private:
    uint64 seed_;
    Guid asset_;
    uint64 counter_ = 0;
};

} // namespace rocks::nodegraph
