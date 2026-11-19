#pragma once

#include <Config/ConfigTypes.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Memory/UniquePtr.h>
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
// - 이름 블록: 이름(byte 순서)으로 찾는 값이다. 설정 struct 필드 중 RenderRead인 것이다(설정
//   struct의 렌더 읽기 블록 — Config 5.3절). CVar(핸들·동적 변수)는 여기에 없다.
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
    // 값 종류다(Bool·Int32·Int64·Float32·Float64·Utf8String·Name·Enum — 동적 변수 읽기가
    // 확인한다, cvar::ReadDynamic). 칸이 없으면 Opaque다.
    ValueKind kind = ValueKind::Opaque;
};

// CVar 값 조각이다(번호 [k·ChunkSlotCount, (k+1)·ChunkSlotCount)의 칸).
struct CVarChunk {
    CVarSlot slots[ChunkSlotCount];
    // String·Name 칸의 값 블록이다(그 밖의 칸은 비어 있다).
    SharedPtr<const CVarText> texts[ChunkSlotCount];
};

namespace detail {

// 콘솔 이름 색인의 항목 하나다(CVar 10.1·15절 — 다른 스레드의 콘솔 입력이 이름을 해석하고 권한을
// 본다). 만든 뒤 바뀌지 않는다.
struct ConsoleNameEntry {
    explicit ConsoleNameEntry(Allocator& allocator) noexcept : name(allocator) {
    }

    String name;
    // 콘솔 명령이다(아니면 콘솔 설정).
    bool command = false;
    // 콘솔 설정 플래그(ConsoleSettingFlags)다.
    uint32 consoleFlags = 0;
    // CVar 핸들의 성질 비트(cvar::VarFlags — 핸들이 아니면 0)다.
    uint32 varFlags = 0;
    // 설정 struct 필드의 콘솔 이름이다.
    bool field = false;
};

// 콘솔 이름 색인이다(등록 뒤 다음 Sync가 게시하는 snapshot부터 새 색인 — 그 전 snapshot은 앞
// 색인을 공유한다).
struct ConsoleNameIndex {
    explicit ConsoleNameIndex(Allocator& allocator) noexcept : entries(allocator) {
    }

    // 이름이 같은 항목이다(없으면 null).
    const ConsoleNameEntry* Find(StringView name) const noexcept;

    Array<UniquePtr<ConsoleNameEntry>> entries;
};

} // namespace detail

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

    // ---- 콘솔 이름 색인(10.1절) ----

    // 이 snapshot과 함께 게시된 콘솔 이름 색인이다(다른 스레드의 콘솔 입력이 쓴다 — 없으면 null).
    const detail::ConsoleNameIndex* ConsoleNames() const noexcept {
        return names_.Get();
    }

    // ---- 부팅 블록(9절) ----

    // 문맥이 봉인(CompleteStartup)된 뒤에 만든 snapshot인지다. 봉인 뒤 snapshot의 부팅 값은 모두
    // 같다(조각은 공유하고, 봉인 뒤 등록한 부팅 변수의 칸만 그 조각을 복사해 더한다).
    bool IsSealed() const noexcept {
        return sealed_;
    }

    // 부팅 블록에서 번호 index의 칸이다(BootCVar — 봉인 때 값으로 굳었다, 세대는 늘 1). 없으면
    // null.
    const CVarSlot* FindBootSlot(uint32 index) const noexcept {
        const usize chunk = index / ChunkSlotCount;
        if (chunk >= bootChunks_.Size()) {
            return nullptr;
        }

        const CVarChunk* block = bootChunks_.Data()[chunk].Get();
        if (block == nullptr) {
            return nullptr;
        }

        const CVarSlot& slot = block->slots[index % ChunkSlotCount];
        return slot.generation != 0 ? &slot : nullptr;
    }

    // 부팅 블록의 String·Name 값 블록이다. 전제: FindBootSlot(index)가 null이 아니다.
    const CVarText& BootTextAt(uint32 index) const noexcept {
        return *bootChunks_.Data()[index / ChunkSlotCount]->texts[index % ChunkSlotCount];
    }

private:
    friend struct detail::SnapshotAccess;

    uint64 generation_ = 0;
    SharedPtr<const detail::NamedBlock> named_;
    Array<SharedPtr<const CVarChunk>> chunks_;
    // 부팅 블록(봉인 때 굳힌 BootCVar 값 — 봉인 뒤 snapshot이 모두 공유한다).
    Array<SharedPtr<const CVarChunk>> bootChunks_;
    bool sealed_ = false;
    // 콘솔 이름 색인(등록이 바뀌지 않으면 앞 snapshot과 공유한다).
    SharedPtr<const detail::ConsoleNameIndex> names_;
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
