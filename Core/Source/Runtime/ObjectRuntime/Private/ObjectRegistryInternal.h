#pragma once

#include <Core/Containers/HashMap.h>
#include <Core/Hash.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Fatal.h>

// ObjectRegistry 구현 파일(ObjectRegistry*.cpp)이 함께 쓰는 내부 정의다. 슬롯 상태와 Registry
// 내부 구조체(슬롯·chunk·chunk 표·회수 작업·표식 상태·관찰자 집계·기록 계획·이름 기록·이름 색인
// 항목), 여러 파일이 쓰는 상수·도우미를 둔다. 상수·도우미는 registry_detail에 있고 각 구현 파일이
// using namespace로 한정 없이 쓴다. ObjectRegistry*.cpp만 include한다.
namespace rocks::registry_detail {

inline constexpr uint32 SlotsPerChunk = 64;
enum class SlotState : uint8 {
    Free,
    Reserved,
    Alive,
    DestroyRequested,
    Retired,
    Reclaiming,
    Exhausted
};

// state 슬롯을 세는 통계 칸이다(Free·Exhausted는 세지 않아 null). Retired와 Reclaiming은 같이 센다.
inline uint32* StateTally(ObjectStats& tally, SlotState state) noexcept {
    switch (state) {
    case SlotState::Reserved:
        return &tally.reservedObjects;
    case SlotState::Alive:
        return &tally.liveObjects;
    case SlotState::DestroyRequested:
        return &tally.destroyRequested;
    case SlotState::Retired:
    case SlotState::Reclaiming:
        return &tally.retiredObjects;
    default:
        return nullptr;
    }
}

} // namespace rocks::registry_detail

namespace rocks {

// 슬롯과 객체 저장소를 이동하지 않는다. 슬롯 재사용 시 세대를 올리고 wrap 대신 영구 격리한다.
// TypeLease를 소멸 호출용 지역 변수로 옮긴 뒤에도 진단에는 stableType을 사용한다.
struct ObjectRegistry::Slot {
    uint32 generation = 1;
    uint32 index = 0;
    // 상태는 SetState로만 바꾼다(Registry의 상태별 통계를 같이 맞춘다).
    registry_detail::SlotState state = registry_detail::SlotState::Free;
    // Retired 슬롯이 든 회수 대상 목록이다(그 밖의 상태는 None).
    RetiredQueue queue = RetiredQueue::None;
    // Free면 빈 슬롯 목록(단일 연결 — prevInList는 null), Retired면 queue 목록(이중 연결)의 이웃
    // 슬롯이다. 두 목록은 상태로 배타적이다.
    Slot* nextInList = nullptr;
    Slot* prevInList = nullptr;
    Object* object = nullptr;
    void* storage = nullptr;
    TypeLease type;
    TypeId stableType;
    uint32 roots = 0;
    uint32 borrows = 0;
    uint32 backendHolds = 0;
    // encode 고정 계수(PinForEncode). 0이 아니면 컨테이너·참조 변경이 Busy이고 회수하지 않는다.
    uint32 encodePins = 0;
    uint64 transaction = 0;
    uint64 mark = 0;
    // Outer·이름(생성 때 고정 — Docs/Design/ObjectFoundations.ko.md 1.4절). 고리는 슬롯 번호가
    // 아니라 Slot*다: chunk는 Registry 소멸 전까지 이동·반환되지 않으므로 포인터가 안정하고, 순회가
    // 슬롯 번호 → chunk 표 조회를 거치지 않고 바로 따라간다. Reclaiming·Free 슬롯의 이 칸은 읽지
    // 않는다. Retired 슬롯은 자기를 부모 고리에서 이미 뗐지만, Retired 부모의 firstChild는 그
    // 자식들이 스스로 뗄 때까지 유효하다(그동안 회수하지 않는다 — IsReclaimable).
    ObjectHandle outer;
    Slot* outermost = nullptr;
    Slot* parent = nullptr;
    Slot* firstChild = nullptr;
    Slot* lastChild = nullptr;
    Slot* nextSibling = nullptr;
    Slot* prevSibling = nullptr;
    // 이름이 있거나 대표 자식이면 있다(Outer 없는 이름 없는 객체는 null). 회수 때 잠금 밖에서
    // 반환한다.
    NameRecord* name = nullptr;
    uint16 pathBytes = 0;
    bool indexed = false;   // 이름 색인에 있다
    bool linked = false;    // 부모 고리에 있다
    bool discarded = false; // Discard됐다(DestroyRequested이고 색인·고리에 없다 — 회수 때 내린다)

    // 잠금 보유 상태에서 부른다. tally는 그 Registry의 상태별 통계다.
    void SetState(ObjectStats& tally, registry_detail::SlotState next) noexcept {
        if (uint32* from = registry_detail::StateTally(tally, state)) {
            --*from;
        }

        if (uint32* to = registry_detail::StateTally(tally, next)) {
            ++*to;
        }

        state = next;
    }
};

struct ObjectRegistry::Chunk {
    Slot slots[registry_detail::SlotsPerChunk];
};

// chunk 포인터 표 하나의 머리다. Chunk* 칸이 바로 뒤에 이어진다(할당 하나). 표를 키우면 옛 표를
// previous로 이어 Registry 소멸 때까지 보관한다. 옛 표의 칸은 그 표에 있던 chunk를 그대로
// 가리키므로, 잠금 없이 옛 표를 들고 있던 조회도 해제된 메모리를 읽지 않는다.
struct ObjectRegistry::ChunkTable {
    ChunkTable* previous = nullptr;

    Chunk** Entries() noexcept {
        return reinterpret_cast<Chunk**>(this + 1);
    }
};

// 회수할 슬롯에서 잠금 안에 떼어 낸 것이다. 사용자 소멸자·allocator 콜백은 잠금 밖에서 부른다.
struct ObjectRegistry::ReclaimWork {
    Slot* slot = nullptr;
    TypeLease type;
    Object* object = nullptr;
    void* storage = nullptr;
    NameRecord* record = nullptr;
};

struct ObjectRegistry::MarkState {
    ObjectRegistry* registry;
    Array<ObjectHandle>* work;
    uint64 transaction = 0;
    ErrorCode error = ErrorCode::None;
    // 관찰자 쌍 집계(Commit 검증·Discard·다중 간선 지점): tally가 있고 observe면 지금 trace하는
    // 소유자의 패키지 ownerPackage에서 다른 패키지로 가는 간선을 delta로 합친다.
    EdgeTally* tally = nullptr;
    ObjectHandle ownerPackage{};
    int64 delta = 0;
    bool observe = false;
};

// 관찰자 기록 전에 같은 (소유 패키지, 대상 패키지) 쌍을 합치는 작업별 집계 표다. 순서는 발견
// 순이고(handle 해시 순서를 쓰지 않는다 — 결정성) 쌍 → 위치 색인은 Core HashMap이다. 잠금 밖에서
// Registry allocator로 자라고(처리 표시 아래 — allocator 콜백의 Registry 변경 재진입은 Busy라 같은
// 표에 재진입하지 않는다) 소멸도 잠금 밖이다.
struct ObjectRegistry::EdgeTally {
    struct Pair {
        ObjectHandle owner;
        ObjectHandle target;
        int64 delta = 0;
    };

    struct PairKey {
        ObjectHandle owner;
        ObjectHandle target;

        bool operator==(const PairKey&) const noexcept = default;
    };

    struct PairKeyHash {
        uint64 operator()(const PairKey& key) const noexcept {
            const Hash<ObjectHandle> hash;
            return HashCombine(hash(key.owner), hash(key.target));
        }
    };

    explicit EdgeTally(Allocator& allocator) noexcept : pairs(allocator), positions(allocator) {
    }

    EdgeTally(const EdgeTally&) = delete;
    EdgeTally& operator=(const EdgeTally&) = delete;

    // 같은 쌍이 있으면 더하고 없으면 끝에 붙인다. 할당 실패는 그 오류이고 표는 그대로다.
    Result<void> Add(ObjectHandle owner, ObjectHandle target, int64 delta) noexcept {
        const PairKey key{owner, target};
        if (usize* position = positions.Find(key)) {
            pairs[*position].delta += delta;
            return Result<void>::Success();
        }

        Pair pair;
        pair.owner = owner;
        pair.target = target;
        pair.delta = delta;
        if (auto pushed = pairs.TryPushBack(pair); !pushed) {
            return pushed;
        }

        // 색인에 넣지 못하면 붙인 원소를 되돌린다.
        if (auto inserted = positions.Insert(key, pairs.Size() - 1); !inserted) {
            (void)pairs.TryPopBack();
            return Result<void>::Failure(inserted.Error());
        }

        return Result<void>::Success();
    }

    Array<Pair> pairs;
    HashMap<PairKey, usize, PairKeyHash> positions;
};

// 단일 간선 지점의 관찰자 기록 계획이다(최대 두 키 — 옛 대상 키가 먼저다).
struct ObjectRegistry::EdgePlan {
    struct Change {
        ObjectHandle owner;
        ObjectHandle target;
        int64 delta = 0;
    };

    Change changes[2];
    uint32 count = 0;
};

// 할당 하나에 NFC 이름과 그 뒤 비교 키를 담는다. keyHash는 (outer handle, 비교 키)의 색인 해시다.
struct ObjectRegistry::NameRecord {
    uint64 keyHash = 0;
    uint16 nameBytes = 0;
    uint16 keyBytes = 0;

    const char* Data() const noexcept {
        return reinterpret_cast<const char*>(this + 1);
    }

    StringView Name() const noexcept {
        return StringView(Data(), nameBytes);
    }

    StringView Key() const noexcept {
        return StringView(Data() + nameBytes, keyBytes);
    }
};

// 이름 색인 항목이다. slot이 null이면 빈 칸(keyHash 0) 또는 tombstone(keyHash 1)이다. 해시를 항목에
// 두어 rehash가 슬롯을 읽지 않는다.
struct ObjectRegistry::NameEntry {
    Slot* slot = nullptr;
    uint64 keyHash = 0;
};

} // namespace rocks

namespace rocks::registry_detail {

inline void ReleaseRegistryMemory(Allocator& allocator, void* memory) noexcept {
    if (memory != nullptr && !allocator.Deallocate(memory)) {
        platform::Fatal("Registry allocation ownership mismatch");
    }
}

inline Result<void> OpenState(bool closed, bool processing) noexcept {
    if (closed || processing) {
        return Result<void>::Failure(closed ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    return Result<void>::Success();
}

// (outer handle 전체, 비교 키)의 색인 해시다. outer에 generation이 있으므로 회수된 Outer의 옛
// 항목이 새 객체와 섞이지 않는다.
inline uint64 NameKeyHash(ObjectHandle outer, StringView key) noexcept {
    return HashCombine(Hash<ObjectHandle>{}(outer), HashStringBytes(key));
}

// outer 아래 이름이 nameBytes byte인 새 객체의 객체 경로 길이다. 경로는 Outer 경로 + (둘 다 비어
// 있지 않으면 '.') + 이름이다(outermost·대표 자식은 빈 경로).
inline usize ChildPathBytes(const OuterSummary* outer, usize nameBytes) noexcept {
    if (outer == nullptr) {
        return 0;
    }

    return outer->pathBytes + (outer->pathBytes != 0 && nameBytes != 0 ? 1u : 0u) + nameBytes;
}

} // namespace rocks::registry_detail
