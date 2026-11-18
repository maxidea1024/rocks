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
// GC 페이지 하나의 슬롯 수다(chunk 64개). hot 레코드 64 KiB와 비트 묶음 2 KiB를 담는다.
inline constexpr uint32 GcPageSlots = 4096;
inline constexpr uint32 ChunksPerGcPage = GcPageSlots / SlotsPerChunk;
// 슬롯 번호가 없다는 표지다(hot 레코드의 Outer 없음, 루트 목록 밖).
inline constexpr uint32 NoSlot = NumericLimits<uint32>::Max();
// 클러스터 장부 칸의 상태 비트다(Docs/Design/GarbageCollectionScale.ko.md 13.4절). 낮은 29비트가
// 클러스터 번호다(클러스터를 켠 Registry는 슬롯 수를 RegistryOptions::ClusterSlotLimit 이하로
// 둔다).
// - ClusterMember | id: 클러스터 id의 소속
// - ClusterDetached | id: 클러스터 id에서 이탈한 대상(그 소속 객체에서 오는 간선이 남았다)
// - ClusterPending: 공개 전 객체의 가입 지정(Slot::clusterInCount가 루트 슬롯, 루트 지정이면
//   NoSlot)
inline constexpr uint32 ClusterMember = 1u << 31;
inline constexpr uint32 ClusterDetached = 1u << 30;
inline constexpr uint32 ClusterPending = 1u << 29;
inline constexpr uint32 ClusterIdMask = ClusterPending - 1;
// chunk 클러스터 표의 넘침 표지다. GcBits::firstCluster가 이 값이면 chunk에 클러스터가 셋 이상이라
// 넓은 표(ObjectRegistry::ClusterChunk)를 읽고, 넓은 표의 clusters[0]이 이 값이면 그보다 많이
// 섞여 장부 상태를 읽는다.
inline constexpr uint32 ChunkClusterOverflow = NumericLimits<uint32>::Max();
// 넓은 chunk 클러스터 표가 담는 클러스터 수와 소속마다의 고름 비트 수다.
inline constexpr uint32 ChunkClusterEntries = 8;
inline constexpr uint32 ChunkSelectorBits = 3;
// 경계 비율 안전장치(Docs/Implementation/GcScaleExperiment.ko.md 14절)의 문턱이다. 패키지
// 클러스터의 경계 소속(밖으로 가는 간선이 있는 소속)이 소속의 이 백분율을 넘으면 cycle 끝에 해체해
// 일반 trace로 둔다. 경계 소속은 어차피 trace되고 나머지 소속도 처음 만날 때 클러스터 조회를
// 치르므로 경계가 거의 전부인 클러스터는 일반 표식보다 느리다. 값은 GcBench --package-profile로
// 정했다: 80%는 패키지 사이 간선 100%(경계 약 88%)에서 표식을 클러스터 끔 수준으로 되돌리고 70%
// 이하 그래프에는 차이가 없다. 70%로 낮추면 경계 70–80% 패키지 일부만 해체되어 오히려 느려졌다.
inline constexpr uint32 ClusterFlattenBoundaryPercent = 80;
// 작업 큐 항목의 클러스터 표지다(낮은 비트가 클러스터 번호, 표지가 없으면 슬롯 번호).
inline constexpr uint32 ClusterItem = 1u << 31;
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
// 슬롯의 hot 레코드다(캐시 줄 하나 — GcScaleExperiment.ko.md 17절). handle 조회(세대·상태), Borrow,
// 루트, 참조 쓰기(변경 표지·encode 고정·클러스터 계수)가 읽는 칸만 둔다. 이름·고리·회수 목록·생성
// 트랜잭션·Outer 같은 나머지는 같은 chunk의 SlotCold(ObjectRegistry::ColdOf)에 있다.
struct alignas(64) ObjectRegistry::Slot {
    uint32 generation = 1;
    uint32 index = 0;
    // 상태는 SetState로만 바꾼다(Registry의 상태별 통계를 같이 맞춘다).
    registry_detail::SlotState state = registry_detail::SlotState::Free;
    // Retired 슬롯이 든 회수 대상 목록이다(그 밖의 상태는 None).
    RetiredQueue queue = RetiredQueue::None;
    bool indexed = false; // 이름 색인에 있다
    bool linked = false;  // 부모 고리에 있다
    // 클러스터 장부의 inCount다(ClusterCell 주석 — 클러스터를 켠 Registry만 쓴다). 같은 클러스터 안
    // 간선(가장 흔한 쓰기)은 이 계수만 바꾸고, 참조 추가는 대상 슬롯을 검증으로 이미 읽었으므로
    // 장부 칸을 따로 읽지 않는다(GcScaleExperiment.ko.md 14절). 공개 전 가입
    // 지정(ClusterPending)이면 루트 슬롯 번호(루트 지정이면 NoSlot)다.
    uint32 clusterInCount = 0;
    Object* object = nullptr;
    TypeLease type;
    uint32 roots = 0;
    uint32 borrows = 0;
    // encode 고정 계수(PinForEncode). 0이 아니면 컨테이너·참조 변경이 Busy이고 회수하지 않는다.
    uint32 encodePins = 0;
    // 루트 목록(ObjectRegistry::rootList_)에서의 위치다(roots가 0이면 NoSlot).
    uint32 rootPosition = registry_detail::NoSlot;
    // 변경 표지(ObjectRegistry::MutationStamp — 쓰기 Borrow·live 참조·컨테이너 변경·묶음 게시마다
    // 오른다). 슬롯을 다시 써도 되돌리지 않는다.
    uint64 mutations = 0;

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

// 슬롯의 cold 레코드다(같은 chunk에 슬롯과 나란히 — ObjectRegistry::ColdOf). 생성·회수·이름·Outer
// 고리·Discard 경로만 읽는다.
struct ObjectRegistry::SlotCold {
    // Free면 빈 슬롯 목록(단일 연결 — prevInList는 null), Retired면 queue 목록(이중 연결),
    // DestroyRequested면 파괴 요청 목록(이중 연결 — destroyListed, TakeDestroyRequests)의 이웃
    // 슬롯이다. 세 목록은 상태로 배타적이다.
    Slot* nextInList = nullptr;
    Slot* prevInList = nullptr;
    void* storage = nullptr;
    TypeId stableType;
    uint64 transaction = 0;
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
    uint32 backendHolds = 0;
    uint16 pathBytes = 0;
    bool discarded = false; // Discard됐다(DestroyRequested이고 색인·고리에 없다 — 회수 때 내린다)
    bool destroyListed = false; // 파괴 요청 목록에 있다(아직 TakeDestroyRequests가 꺼내지 않았다)
};

struct ObjectRegistry::Chunk {
    static_assert(sizeof(Slot) == 64, "Slot hot record must fill one cache line");

    Slot slots[registry_detail::SlotsPerChunk];
    SlotCold colds[registry_detail::SlotsPerChunk];
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
    Array<uint32>* work;
    uint64 transaction = 0;
    ErrorCode error = ErrorCode::None;
    // 이번 표식이 새로 표식한 객체 수다(잎 객체 포함 — 큐 길이와 다르다).
    uint32 marked = 0;
    // Commit 검증: 지금 trace하는 소유자가 잎 타입(강한 참조 없음 선언)이면 간선을 방문하면 안
    // 된다.
    bool leafOwner = false;
    // 클러스터 장부(Commit 검증·이탈): ledger가 있고 ledgerOwner가 슬롯이면 지금 trace하는 소유자의
    // 간선을 (소유자, 대상) 슬롯 쌍으로 모은다(Commit). 이탈 trace는 ledgerOwner의 몫을 뺀다.
    Array<uint32>* ledger = nullptr;
    uint32 ledgerOwner = registry_detail::NoSlot;
    // 영구 창의 Commit 검증이다(staged 간선 대상은 같은 트랜잭션 객체나 영구 객체만).
    bool permanent = false;
    // 작업 계수(RegistryOptions::countGcWork — 직렬 Collect만)와 지금 trace하는 소유자의 장부
    // 상태다.
    GcWorkCounters* counters = nullptr;
    uint32 ownerClusterState = 0;
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

// GC가 슬롯마다 읽는 값의 밀집 레코드다(GC-S3, Docs/Design/GarbageCollectionScale.ko.md). 슬롯
// 번호로 GcPage에서 찾는다. 생성 게시(잠금 안)에서 채우고 회수 때 비운다. Reserved·Alive·
// DestroyRequested·Retired 동안 object·type·outer가 유효하다.
struct ObjectRegistry::GcHot {
    Object* object = nullptr;
    // Outer의 슬롯 번호다(없으면 NoSlot). Outer는 생성 때 고정이다.
    uint32 outer = registry_detail::NoSlot;
    // GC 타입 표 번호다(0은 없음).
    uint16 type = 0;
    uint16 reserved = 0;
};

// 슬롯 64개(chunk 하나)의 비트 묶음이다. bit i가 chunk의 i번 슬롯이다.
// - live: Alive·DestroyRequested(GC 후보). Commit이 켜고 Retire가 끈다(잠금 안, 소유 스레드).
// - mark: 이번 cycle에 표식됨. Begin·Collect가 지우고 marker·장벽·Sweeping 출생이 켠다.
// - leaf: 잎 타입(강한 참조 없음)이라 표식만 하고 trace하지 않는다(M2 — 패키지 정책 Registry는
//   자식 고리를 따라야 하므로 켜지 않는다). 생성 게시가 켜고 회수가 끈다.
// - clustered: 클러스터 소속이다(GC-S7). sweep 후보에서 빠지고(live & ~mark & ~clustered) 표식은
//   그 클러스터를 표식한다. 가입이 켜고 이탈·해체가 끈다(잠금 안).
// 나머지는 chunk 클러스터 표다. marker와 참조 쓰기가 소속의 클러스터 번호를 비트 묶음과 같은 캐시
// 줄에서 얻는다(슬롯마다 둔 장부 칸을 무작위로 읽으면 캐시 미스다 — GcScaleExperiment.ko.md
// 14·15절).
// - clusterSeen: 이번 cycle에 클러스터 표식을 본 소속이다. mark처럼 Begin·Collect가 지우고 marker가
//   켠다(병렬 marker는 atomic OR). 같은 소속으로 가는 다음 간선은 이 비트 하나로 끝난다.
// - firstCluster·secondCluster: 소속의 (클러스터 번호 + 1), 0이면 비었다. chunk 하나에 클러스터
//   둘까지 담는다(이웃 슬롯에 만든 클러스터의 경계가 chunk를 가르는 흔한 경우). clusterSecond
//   비트가 켜진 소속은 secondCluster, 꺼진 소속은 firstCluster다. 셋 이상이면 firstCluster가
//   registry_detail::ChunkClusterOverflow이고 넓은 표(ClusterChunk)를 읽는다.
// 가입이 맞추고(chunk의 소속이 모두 떠난 뒤 가입하면 다시 비운다), 이탈·해체는 clustered 비트만
// 끄므로 남은 소속에 대해 표는 여전히 맞다.
struct alignas(64) ObjectRegistry::GcBits {
    uint64 live = 0;
    uint64 mark = 0;
    uint64 leaf = 0;
    uint64 clustered = 0;
    uint64 clusterSeen = 0;
    uint64 clusterSecond = 0;
    uint32 firstCluster = 0;
    uint32 secondCluster = 0;
};

// chunk(소속 64개)의 넓은 클러스터 표다(클러스터를 켠 Registry만, 캐시 줄 하나 — GcBits 표가 넘칠
// 때만 읽는다). 작은 패키지(64칸 chunk에 패키지 10개 안팎)의 소속도 장부 칸 대신 이 표에서 번호를
// 얻는다(표 배열은 1M 슬롯에 1MB라 캐시에 남는다).
// - clusters: chunk에 든 클러스터의 (번호 + 1), 0이면 빈 칸. 소속마다 selectors의 3비트가 칸을
//   고른다. 8개까지 담고, 더 많으면 clusters[0]이 registry_detail::ChunkClusterOverflow다(장부
//   상태를 읽는다).
struct alignas(64) ObjectRegistry::ClusterChunk {
    uint8 selectors[registry_detail::SlotsPerChunk * registry_detail::ChunkSelectorBits / 8] = {};
    uint32 clusters[registry_detail::ChunkClusterEntries] = {};

    // slot(chunk 안 번호)의 칸 번호다.
    uint32 Selector(uint32 slot) const noexcept {
        const uint32 at = slot * registry_detail::ChunkSelectorBits;
        const uint32 byte = at / 8;
        uint32 word = selectors[byte];
        if (byte + 1 < sizeof(selectors)) {
            word |= uint32(selectors[byte + 1]) << 8;
        }

        return (word >> (at % 8)) & ((1u << registry_detail::ChunkSelectorBits) - 1);
    }

    void SetSelector(uint32 slot, uint32 entry) noexcept {
        const uint32 at = slot * registry_detail::ChunkSelectorBits;
        const uint32 byte = at / 8;
        const uint32 shift = at % 8;
        const uint32 mask = ((1u << registry_detail::ChunkSelectorBits) - 1) << shift;
        uint32 word = selectors[byte];
        if (byte + 1 < sizeof(selectors)) {
            word |= uint32(selectors[byte + 1]) << 8;
        }

        word = (word & ~mask) | (entry << shift);
        selectors[byte] = static_cast<uint8>(word);
        if (byte + 1 < sizeof(selectors)) {
            selectors[byte + 1] = static_cast<uint8>(word >> 8);
        }
    }
};

// 클러스터 장부 칸이다(슬롯마다, 클러스터를 켠 Registry만 —
// Docs/Design/GarbageCollectionScale.ko.md 13.4절). 잠금 안에서 바꾸고 marker는 처리 표시 아래에서
// 읽기만 한다. 비운 칸은 기본값이다.
// - state: 위 ClusterMember·ClusterDetached·ClusterPending 비트와 클러스터 번호(0이면 없음). 같은
//   칸의 계수와 함께 읽히도록 칸 안에 둔다(marker는 섞인 chunk에서만 읽는다).
// - inCount(Slot::clusterInCount에 둔다): 소속이면 같은 클러스터 소속 객체에서 오는 간선 수,
//   detached면 그 클러스터 소속 객체에서 오는 간선 수
// - outCount: 소속 객체의 간선 가운데 대상이 같은 클러스터의 소속도 detached도 아닌 간선 수
// - 목록 고리(소속 목록 또는 detached 목록)와 경계 고리(outCount > 0인 소속 목록), 슬롯 번호다
struct ObjectRegistry::ClusterCell {
    uint32 state = 0;
    uint32 outCount = 0;
    uint32 listNext = registry_detail::NoSlot;
    uint32 listPrev = registry_detail::NoSlot;
    uint32 boundaryNext = registry_detail::NoSlot;
    uint32 boundaryPrev = registry_detail::NoSlot;
};

// 클러스터 레코드다. 번호로 ObjectRegistry::clusters_에서 찾는다. 표식 epoch는 따로 둔
// ObjectRegistry::clusterEpochs_다(병렬 marker가 atomic 비교 교환으로 쓴다). processedEpoch는
// 처리한 참여자 하나만 쓴다.
struct ObjectRegistry::ClusterRecord {
    // 루트 슬롯이다(영구 영역은 NoSlot).
    uint32 root = registry_detail::NoSlot;
    uint32 members = 0;
    uint32 memberHead = registry_detail::NoSlot;
    uint32 boundaryHead = registry_detail::NoSlot;
    // 경계 고리의 소속 수다(경계 비율 안전장치가 읽는다).
    uint32 boundaryMembers = 0;
    uint32 detachedHead = registry_detail::NoSlot;
    uint32 processedEpoch = 0;
    // 빈 레코드 목록·해체 대기 목록의 다음 번호다.
    uint32 nextFree = registry_detail::NoSlot;
    // 쓰는 중이다(빈 레코드는 거짓).
    bool live = false;
    // 해체됐다. 큐에 남은 이 클러스터 항목은 꺼낼 때 버린다(레코드는 그 cycle의 큐가 빈 뒤 재사용).
    bool dissolved = false;
    // 영구 영역이다(언제나 표식됨, 처리하지 않는다).
    bool permanent = false;
    // 엔진 계층이 지정한 클러스터다(World Level — 소속 불변식 검증 대상).
    bool designated = false;
};

struct ObjectRegistry::GcPage {
    static_assert(sizeof(GcHot) == 16, "GC hot record must stay 16 bytes");
    static_assert(sizeof(GcBits) == 64, "GC bit group must fill one cache line");
    static_assert(sizeof(ClusterChunk) == 64, "Wide chunk cluster table must fill one cache line");

    GcBits bits[registry_detail::ChunksPerGcPage];
    GcHot hot[registry_detail::GcPageSlots];
    // 클러스터 장부 칸(GcPageSlots개)과 chunk 클러스터 표(ChunksPerGcPage개)다(클러스터를 켠
    // Registry만 — 아니면 null). 페이지와 함께 잠금 밖에서 할당한다.
    ClusterCell* cells = nullptr;
    ClusterChunk* clusterChunks = nullptr;
};

// GC 타입 표의 항목이다. key는 타입 레코드의 RuntimeTypeOps 주소다(레코드가 살아 있는 동안
// 안정하다 — 레코드가 해제되어 같은 주소를 다른 타입이 쓰면 찾을 때 값을 다시 채운다. 그때는
// 옛 타입의 객체가 남아 있지 않다).
struct ObjectRegistry::GcType {
    // 참조 위치 표 상태다(M3).
    enum class Layout : uint8 {
        Unknown, // 아직 객체를 보지 못했다(첫 생성에서 정한다)
        Table,   // slots로 표식한다
        Trace    // 표가 없거나 나타낼 수 없다
    };

    const void* key = nullptr;
    void (*trace)(const Object&, TraceVisitor&) noexcept = nullptr;
    void (*referenceLayout)(const Object&, detail::ReferenceLayoutBuilder&) noexcept = nullptr;
    // Table이면 Registry allocator로 받은 칸 표다(slotCount개).
    detail::ReferenceSlot* slots = nullptr;
    uint16 slotCount = 0;
    Layout layout = Layout::Unknown;
    // 표에 배열 칸이 있다(큐 앞쪽에서 배열 버퍼를 미리 읽는다).
    bool hasArray = false;
    bool parallelTraceSafe = false;
    bool leaf = false;
};

// 병렬 표식의 큐 블록이다(GC-S4). 참여자의 지역 큐는 블록 더미이고 공유 풀·소유 스레드 큐는 블록을
// 주고받는다. 재사용 목록(ObjectRegistry::gcFreeBlocks_)에 남겨 다음 slice가 다시 쓴다.
struct ObjectRegistry::GcBlock {
    static constexpr uint32 Capacity = 512;

    GcBlock* next = nullptr;
    uint32 count = 0;
    uint32 items[Capacity];
};

// 생성이 잠금 밖에서 준비해 잠금 안에서 게시하는 GC 자료 성장분이다. 쓰지 않은 것은
// ReleaseGcGrowth가 반환한다.
struct ObjectRegistry::GcGrowth {
    GcPage* page = nullptr;
    GcPage** pageTable = nullptr;
    uint32 pageTableCapacity = 0;
    uint32* rootList = nullptr;
    uint32 rootCapacity = 0;
};

inline ObjectRegistry::GcPage& ObjectRegistry::PageOf(uint32 index) const noexcept {
    return *gcPages_[index / registry_detail::GcPageSlots];
}

inline ObjectRegistry::GcHot& ObjectRegistry::HotOf(uint32 index) const noexcept {
    return PageOf(index).hot[index % registry_detail::GcPageSlots];
}

inline ObjectRegistry::ClusterCell& ObjectRegistry::CellOf(uint32 index) const noexcept {
    return PageOf(index).cells[index % registry_detail::GcPageSlots];
}

inline uint32& ObjectRegistry::ClusterStateOf(uint32 index) const noexcept {
    return CellOf(index).state;
}

// 미룬 장부 감소 기록의 칸 수와 적용 때 미리 읽는 거리다(GC 설계 13.4절).
inline constexpr uint32 LedgerRemovalCapacity = 4096;
inline constexpr uint32 LedgerApplyPrefetchDistance = 8;

inline uint32& ObjectRegistry::ClusterInCountOf(uint32 index) const noexcept {
    return SlotAt(index)->clusterInCount;
}

inline ObjectRegistry::ClusterChunk& ObjectRegistry::ClusterChunkOf(uint32 index) const noexcept {
    return PageOf(index)
        .clusterChunks[(index % registry_detail::GcPageSlots) / registry_detail::SlotsPerChunk];
}

inline uint32 ObjectRegistry::MemberClusterOf(uint32 index) const noexcept {
    const GcBits& bits = BitsOf(index);
    const uint32 local = index % registry_detail::SlotsPerChunk;
    const uint32 key =
        (bits.clusterSecond >> local & 1) != 0 ? bits.secondCluster : bits.firstCluster;
    if (key != registry_detail::ChunkClusterOverflow) {
        return key - 1;
    }

    const ClusterChunk& chunk = ClusterChunkOf(index);
    if (chunk.clusters[0] == registry_detail::ChunkClusterOverflow) {
        return ClusterStateOf(index) & registry_detail::ClusterIdMask;
    }

    return chunk.clusters[chunk.Selector(local)] - 1;
}

inline ObjectRegistry::GcBits& ObjectRegistry::BitsOf(uint32 index) const noexcept {
    return PageOf(index)
        .bits[(index % registry_detail::GcPageSlots) / registry_detail::SlotsPerChunk];
}

// chunk 표 색인이라 O(1)이다. 다른 스레드의 계수 반환도 잠금 안에서 부르므로 표 교체(잠금 안)와
// 겹치지 않고, 처리 표시 아래 잠금 없이 부를 때(소유 스레드와 병렬 trace worker)는 표가 바뀌지
// 않는다(교체는 생성의 처리 표시 아래에서만 한다). 옛 표는 소멸 때까지 보관하므로 이 규칙 밖의 잠금
// 없는 조회가 생겨도 해제된 표를 읽지는 않는다. GC marker의 간선마다 부르므로 inline이다.
inline ObjectRegistry::Slot* ObjectRegistry::SlotAt(uint32 index) const noexcept {
    const uint32 chunk = index / registry_detail::SlotsPerChunk;
    return chunk < chunkCount_ ? &chunks_[chunk]->slots[index % registry_detail::SlotsPerChunk]
                               : nullptr;
}

inline ObjectRegistry::SlotCold& ObjectRegistry::ColdOf(const Slot& slot) const noexcept {
    return chunks_[slot.index / registry_detail::SlotsPerChunk]
        ->colds[slot.index % registry_detail::SlotsPerChunk];
}

inline uptr ObjectRegistry::StorageStartOf(const Slot& slot) noexcept {
    return reinterpret_cast<uptr>(slot.object) - slot.type.ObjectOffset();
}

inline ObjectRegistry::Slot* ObjectRegistry::FindSlot(ObjectHandle handle) const noexcept {
    if (handle.registry != domain_ || handle.generation == 0 || handle.slot >= options_.maxSlots) {
        return nullptr;
    }

    Slot* slot = SlotAt(handle.slot);
    return slot && slot->generation == handle.generation ? slot : nullptr;
}

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

// 새 객체의 객체 경로 길이다(공개 detail::ChildPathBytes — 그래프 1단계와 같이 쓴다).
using detail::ChildPathBytes;

// 컨테이너 버퍼의 다음 용량이다(4부터 두 배, uint32 상한에서 포화).
inline uint32 GrownCapacity(uint32 capacity) noexcept {
    constexpr uint32 Maximum = NumericLimits<uint32>::Max();
    return capacity < 4u ? 4u : capacity > Maximum / 2 ? Maximum : capacity * 2u;
}

inline void ReleaseContainerMemory(Allocator& allocator, void* memory) noexcept {
    if (memory != nullptr && !allocator.Deallocate(memory)) {
        platform::Fatal("Registry container deallocation failed");
    }
}

} // namespace rocks::registry_detail
