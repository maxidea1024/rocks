#pragma once

#include <Config/ConfigTypes.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <Object/RuntimeTypeOps.h>

// 설정 snapshot이다(Docs/Design/CVarSystem.ko.md 7.2·10.1절, ConfigSystem.ko.md 5.3절 — CVR2).
// sync 지점(또는 CVar 등록)에서 게임 lane이 만든 불변 값 블록이고, 어느 스레드에서 읽어도 된다(참조
// 계수만 원자적이다). 두 부분으로 된다.
// - CVar 칸: CVar 핸들 번호(cvar::Index) 순서의 조각(ChunkSlotCount 칸) 표다. 칸은 값(8 byte —
//   String·Name은 불변 텍스트 블록 참조)과 세대다. Sync는 바뀐 칸이 있는 조각만 새로 만들고
//   나머지 조각은 이전 snapshot과 참조 계수로 공유한다(copy-on-write). `CVar::Get()`이 이 칸을
//   읽는다(잠금·할당·문자열 연산 없음).
// - 이름 블록: 이름(byte 순서)으로 찾는 값이다. CF2 독립 콘솔 설정과 설정 struct 필드 중
//   RenderRead인 것이다(설정 struct의 렌더 읽기 블록 — Config 5.3절). CVar 핸들은 여기에 없다.
//
// UE 대응: 렌더 스레드 사본(IConsoleManager.h의 shadow 값). Rocks는 주소에 쓰는 명령 대신 불변
// snapshot을 프레임마다 참조로 넘긴다.
namespace rocks::config {

class ConfigSnapshot;

namespace detail {

struct SnapshotAccess;
struct NamedBlock;
struct WriteChannel;
struct QueuedChange;

} // namespace detail

// CVar 조각 하나의 칸 수다(7.2절).
inline constexpr uint32 ChunkSlotCount = 256;

// String·Name CVar 값의 불변 블록이다(만든 뒤 바뀌지 않는다 — 조각들이 참조 계수로 공유한다).
struct CVarText {
    explicit CVarText(Allocator& allocator) noexcept : text(allocator) {
    }

    // String 값이다(Name 값이면 비어 있다 — 값은 토큰이다).
    String text;
    // Name 값의 토큰(문맥 Registry의 NamePool 것 — String 값이면 None).
    Name name{};
};

// CVar 값 칸 하나다.
struct CVarSlot {
    // bool은 0·1, 정수·enum은 int64, 실수는 double의 비트다(String·Name은 쓰지 않는다 — 텍스트
    // 블록이 값이다).
    uint64 bits = 0;
    // 유효값의 세대다(처음 값이 1, 바뀔 때마다 1 늘어난다). 0이면 이 snapshot에 칸이 없다.
    uint32 generation = 0;
};

// CVar 값 조각이다(번호 [k·ChunkSlotCount, (k+1)·ChunkSlotCount)의 칸).
struct CVarChunk {
    CVarSlot slots[ChunkSlotCount];
    // String·Name 칸의 값 블록이다(그 밖의 칸은 비어 있다).
    SharedPtr<const CVarText> texts[ChunkSlotCount];
};

class ConfigSnapshot {
public:
    explicit ConfigSnapshot(Allocator& allocator) noexcept;
    ConfigSnapshot(const ConfigSnapshot&) = delete;
    ConfigSnapshot& operator=(const ConfigSnapshot&) = delete;
    ~ConfigSnapshot();

    // 1부터 늘어나는 세대 번호다(새 snapshot을 게시할 때마다 하나씩 — 바뀐 것이 없는 Sync는 같은
    // snapshot이다).
    uint64 Generation() const noexcept {
        return generation_;
    }

    // ---- 이름 블록 ----

    usize Count() const noexcept;

    // index번째(이름 순서) 이름·값이다. 전제: index < Count() — 아니면 Fatal.
    StringView NameAt(usize index) const noexcept;
    const FieldValue& ValueAt(usize index) const noexcept;

    // 이름의 값이다(없으면 null).
    const FieldValue* Find(StringView name) const noexcept;

    // ---- CVar 칸 ----

    // 번호 index의 칸이다. 이 snapshot에 칸이 없으면(그 뒤에 등록됐거나 다른 문맥의 핸들) null이다.
    const CVarSlot* FindSlot(uint32 index) const noexcept {
        const usize chunk = index / ChunkSlotCount;
        if (chunk >= chunks_.Size()) {
            return nullptr;
        }

        const CVarChunk* block = chunks_.Data()[chunk].Get();
        if (block == nullptr) {
            return nullptr;
        }

        const CVarSlot& slot = block->slots[index % ChunkSlotCount];
        return slot.generation != 0 ? &slot : nullptr;
    }

    // String·Name 칸의 값 블록이다. 전제: FindSlot(index)가 null이 아니다.
    const CVarText& TextAt(uint32 index) const noexcept {
        return *chunks_.Data()[index / ChunkSlotCount]->texts[index % ChunkSlotCount];
    }

private:
    friend struct detail::SnapshotAccess;

    uint64 generation_ = 0;
    SharedPtr<const detail::NamedBlock> named_;
    Array<SharedPtr<const CVarChunk>> chunks_;
    // 이 snapshot을 만든 문맥의 쓰기 통로다(묶음 계수·`CVar::Set`의 대상 — 문맥보다 오래 살 수
    // 있다).
    SharedPtr<detail::WriteChannel> channel_;
};

namespace detail {

// 이 스레드에 묶인 snapshot이다(7.1절). gameLane은 문맥이 자기 소유 스레드(게임 lane)에 스스로
// 묶은 것이면 그 문맥의 통로다(cvar::SnapshotBinding으로 묶은 것이면 null). depth는 지금 살아 있는
// 명시 묶음(cvar::SnapshotBinding)의 중첩 깊이다(0이면 명시 묶음이 없다 — 게임 lane 자동 묶음이거나
// 묶음 없음). 풀 때 깊이로 LIFO를 확인한다.
struct ThreadBinding {
    const ConfigSnapshot* snapshot = nullptr;
    const WriteChannel* gameLane = nullptr;
    uint32 depth = 0;
};

// 스레드 묶음이다. 그 스레드만 읽고 쓴다(10.1절 표).
inline thread_local constinit ThreadBinding CurrentBinding{};

} // namespace detail

} // namespace rocks::config
