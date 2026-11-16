#pragma once

#include <Base/NumericLimits.h>
#include <Base/TypeTraits.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Optional.h>
#include <Core/Text/String.h>
#include <Object/RuntimeTypeOps.h>
#include <Object/SoftObjectRef.h>
#include <Platform/Fatal.h>

namespace rocks {

class ObjectRegistry;

// ObjectRegistry::SetMapReference의 선택 입력·출력이다(리플렉션 B9f4 리뷰 반영). maxCount는 새 키를
// 넣은 뒤의 항목 수 상한이다(넣으면 Count()가 maxCount를 넘을 때 CapacityExceeded — 첫 판정과 잠금
// 밖 준비 뒤 게시 직전 재검증 모두에서 본다, 있는 키의 교체는 길이를 바꾸지 않으므로 걸지 않는다).
// 기본은 상한 없음이다(리플렉션 MaxElements는 경로 쓰기가 넘긴다 — Registry 자체는 필드 상한을
// 모른다). previous· replaced는 성공했을 때만 쓰는 출력으로, 게시하는 잠금 안에서 실제로 덮어쓴 옛
// 값(새 키면 null)과 키가 이미 있었는지다. 호출 전에 읽은 값은 할당·관찰자 예약 콜백이 그 사이 바꿀
// 수 있으므로 되돌리기(Undo)에 쓸 옛 값은 이것으로 얻는다.
//
// 사용 예:
//   MapReferenceWrite write;
//   write.maxCount = field.maxElements;
//   if (auto set = registry.SetMapReference(access, owner, map, key, target, &write); set) {
//       // write.replaced면 write.previous로 되돌리고, 아니면 그 키를 지워 되돌린다.
//   }
struct MapReferenceWrite {
    usize maxCount = NumericLimits<usize>::Max();
    ObjectHandle previous{};
    bool replaced = false;
};

namespace detail {

// ObjectRefMap<K, T>의 저장소다(키 K → 대상 handle, 삽입 순서). Registry만 바꾼다. revision은
// Registry의 구조 변경(넣기·지우기·비우기·통째 지정)마다 증가한다: 잠금 밖에서 키 사본·저장소를
// 준비한 뒤 다시 잠글 때, 그 사이 allocator 콜백이 같은 컨테이너를 바꿨는지 이 값으로 가린다(값
// 교체는 항목 자리를 바꾸지 않으므로 세지 않는다).
template <class K>
struct ObjectRefMapStorage {
    explicit ObjectRefMapStorage(Allocator& allocator) noexcept : map(allocator) {
    }

    OrderedMap<K, ObjectHandle> map;
    uint64 revision = 0;
};

// ObjectRefMap에 넣을 키 사본을 컨테이너의 allocator로 만든다(String·SoftObjectRef는 할당 —
// OutOfMemory, 그 밖의 키는 값 복사). 조회 형식 Q는 K 자신이거나 String 키의 StringView, Soft
// 키의 SoftObjectRefView다.
template <class K>
struct ObjectRefMapKey {
    static_assert(IsNothrowConstructibleV<K, const K&>,
                  "ObjectRefMap keys other than String and SoftObjectRef must be nothrow copyable");

    static Result<K> Make(Allocator&, const K& key) noexcept {
        return Result<K>::Success(K(key));
    }
};

template <>
struct ObjectRefMapKey<String> {
    static Result<String> Make(Allocator& allocator, StringView key) noexcept {
        String copy(allocator);
        if (auto copied = copy.TryAssign(key); !copied) {
            return Result<String>::Failure(copied.Error());
        }

        return Result<String>::Success(Move(copy));
    }

    static Result<String> Make(Allocator& allocator, const String& key) noexcept {
        return Make(allocator, key.View());
    }
};

template <class T>
struct ObjectRefMapKey<SoftObjectRef<T>> {
    static Result<SoftObjectRef<T>> Make(Allocator& allocator, SoftObjectRefView key) noexcept {
        SoftObjectRef<T> copy(allocator);
        if (auto copied = copy.TryAssign(key.asset, key.objectPath); !copied) {
            return Result<SoftObjectRef<T>>::Failure(copied.Error());
        }

        return Result<SoftObjectRef<T>>::Success(Move(copy));
    }

    static Result<SoftObjectRef<T>> Make(Allocator& allocator,
                                         const SoftObjectRef<T>& key) noexcept {
        return Make(allocator, SoftObjectRefView{key.asset, key.objectPath.View()});
    }
};

// Registry(Private/ObjectRegistryMapEdges.cpp)가 키 형식을 모른 채 ObjectRefMap을 편집하는
// 지점이다. 구현은 아래 ObjectRefMapEditor<K, Q> 하나다. Registry는 잠금 안에서 조회·게시(Find·
// CommitInsert·Erase·Detach·CommitAssign — 할당·해제·실패 없음)를, 잠금 밖에서 할당하는 준비
// (PrepareInsert·PrepareAssign)를 부른다. 준비한 키 사본·예약·떼어 낸 옛 항목·지운 키는 편집 객체가
// 쥐고 있다가 편집 객체가 소멸할 때(Registry 호출이 돌아온 뒤 — 잠금 밖) 반환하므로 그 해제 콜백은
// 게시를 마친 컨테이너를 본다. 항목 포인터는 잠금 하나 안에서만 쓴다(Erase·압축이 모든 포인터를
// 무효화하므로 다시 잠그면 키로 다시 찾는다 — Docs/Design/ReflectionExtensions.ko.md 7.4절).
class ObjectRefMapEdit {
public:
    ObjectRefMapEdit(const ObjectRefMapEdit&) = delete;
    ObjectRefMapEdit& operator=(const ObjectRefMapEdit&) = delete;

    // owner 저장소 안 위치 검사에 쓰는 컨테이너 저장소의 주소와 크기다.
    const void* Storage() const noexcept {
        return storage_;
    }

    usize StorageBytes() const noexcept {
        return storageBytes_;
    }

    // 컨테이너를 순회(ForEach 등) 중인지다 — 구조 변경은 Busy로 거절한다.
    virtual bool IsIterating() const noexcept = 0;

    // Registry 구조 변경 계수다(ObjectRefMapStorage::revision).
    virtual uint64 Revision() const noexcept = 0;

    virtual usize Count() const noexcept = 0;

    // 편집 키 항목의 값 칸이다(없으면 nullptr). 잠금 안에서 부르고 그 잠금 안에서만 쓴다.
    virtual ObjectHandle* Find() noexcept = 0;

    // 잠금 밖: 편집 키의 사본(컨테이너 allocator)과 한 항목 더 담을 저장소 예약을 준비한다.
    // 컨테이너는 바꾸지 않는다. 오류: 할당 오류, Busy(순회 중), Overflow.
    virtual Result<void> PrepareInsert() noexcept = 0;

    // 잠금 안: 준비한 예약을 적용하고 키 사본을 target 값으로 끝에 넣는다(할당·해제·실패 없음 —
    // 준비 뒤 구조 변경이 없음은 호출자가 Revision으로 확인했다).
    virtual void CommitInsert(ObjectHandle target) noexcept = 0;

    // 잠금 안: 편집 키 항목을 지운다(키는 편집 객체가 넘겨받아 소멸 때 반환한다). 항목이 있어야
    // 한다.
    virtual void Erase() noexcept = 0;

    // 잠금 안: 모든 항목과 저장소를 편집 객체로 떼어 내고 빈 컨테이너를 게시한다(할당·해제 없음).
    virtual void Detach() noexcept = 0;

    // 모든 값을 삽입 순서로 방문한다(null 포함 — 방문자가 무시한다). 구조를 바꾸지 않는다.
    virtual void VisitValues(TraceVisitor& visitor) const noexcept = 0;

    // 잠금 밖(처리 표시 아래): 편집 키 목록과 targets로 새 컨테이너를 만든다(키 사본·저장소는
    // 컨테이너 allocator, null은 ObjectHandle{}로 정규화). 같은 키가 둘이면 InvalidArgument, 할당
    // 오류는 그대로이고 컨테이너는 바꾸지 않는다.
    virtual Result<void> PrepareAssign(Span<const ObjectHandle> targets) noexcept = 0;

    // 잠금 안: 준비한 새 컨테이너를 게시하고 옛 항목은 편집 객체로 떼어 낸다(할당·해제 없음).
    virtual void CommitAssign() noexcept = 0;

protected:
    ObjectRefMapEdit(const void* storage, usize storageBytes) noexcept
        : storage_(storage), storageBytes_(storageBytes) {
    }

    ~ObjectRefMapEdit() = default;

private:
    const void* storage_;
    usize storageBytes_;
};

// ObjectRefMapEdit의 키 형식 K 구현이다. key는 편집 키(넣기·교체·지우기 — Q는 K 또는 그 조회
// 형식), keys는 통째 지정의 키 목록이다. 스택에서만 쓰고 Registry 호출 하나가 끝나면 소멸한다.
template <class K, class Q>
class ObjectRefMapEditor final : public ObjectRefMapEdit {
public:
    using Map = OrderedMap<K, ObjectHandle>;

    ObjectRefMapEditor(ObjectRefMapStorage<K>& storage, const Q* key,
                       Span<const K> keys = {}) noexcept
        : ObjectRefMapEdit(&storage, sizeof(storage)), storage_(storage), key_(key), keys_(keys),
          detached_(storage.map.GetAllocator()) {
    }

    ~ObjectRefMapEditor() = default;

    bool IsIterating() const noexcept override {
        return storage_.map.IsIterating();
    }

    uint64 Revision() const noexcept override {
        return storage_.revision;
    }

    usize Count() const noexcept override {
        return storage_.map.Count();
    }

    ObjectHandle* Find() noexcept override {
        return storage_.map.Find(*key_);
    }

    Result<void> PrepareInsert() noexcept override {
        if (!inserted_) {
            auto made = ObjectRefMapKey<K>::Make(storage_.map.GetAllocator(), *key_);
            if (!made) {
                return Result<void>::Failure(made.Error());
            }

            inserted_.Emplace(Move(made.Value()));
        }

        reservation_.Reset();
        auto prepared = storage_.map.PrepareReserve(storage_.map.Count() + 1);
        if (!prepared) {
            return Result<void>::Failure(prepared.Error());
        }

        reservation_.Emplace(Move(prepared.Value()));
        return Result<void>::Success();
    }

    void CommitInsert(ObjectHandle target) noexcept override {
        if (!inserted_ || !reservation_) {
            platform::Fatal("ObjectRefMap insertion was not prepared");
        }

        storage_.map.CommitReserve(*reservation_);
        auto inserted = storage_.map.Insert(Move(*inserted_), target);
        if (!inserted || inserted.Value() != InsertOutcome::Inserted) {
            platform::Fatal("ObjectRefMap insertion into a reserved map failed");
        }

        ++storage_.revision;
    }

    void Erase() noexcept override {
        const bool erased = storage_.map.EraseAndTake(
            *key_, [this](K&& key, ObjectHandle&&) noexcept { erased_.Emplace(Move(key)); });
        if (!erased) {
            platform::Fatal("ObjectRefMap erase of a missing key");
        }

        ++storage_.revision;
    }

    void Detach() noexcept override {
        detached_ = Move(storage_.map);
        ++storage_.revision;
    }

    void VisitValues(TraceVisitor& visitor) const noexcept override {
        for (auto entry : static_cast<const Map&>(storage_.map)) {
            visitor.Visit(entry.value);
        }
    }

    Result<void> PrepareAssign(Span<const ObjectHandle> targets) noexcept override {
        if (targets.Size() != keys_.Size()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        Allocator& allocator = storage_.map.GetAllocator();
        Map& fresh = fresh_.Emplace(allocator);
        if (auto reserved = fresh.Reserve(keys_.Size()); !reserved) {
            return reserved;
        }

        for (usize index = 0; index < keys_.Size(); ++index) {
            auto made = ObjectRefMapKey<K>::Make(allocator, keys_[index]);
            if (!made) {
                return Result<void>::Failure(made.Error());
            }

            const ObjectHandle target = targets[index].IsNull() ? ObjectHandle{} : targets[index];
            auto inserted = fresh.Insert(Move(made.Value()), target);
            if (!inserted) {
                return Result<void>::Failure(inserted.Error());
            }

            if (inserted.Value() == InsertOutcome::AlreadyPresent) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        return Result<void>::Success();
    }

    void CommitAssign() noexcept override {
        if (!fresh_) {
            platform::Fatal("ObjectRefMap assignment was not prepared");
        }

        detached_ = Move(storage_.map);
        storage_.map = Move(*fresh_);
        ++storage_.revision;
    }

private:
    ObjectRefMapStorage<K>& storage_;
    const Q* key_;
    Span<const K> keys_;
    // 아래는 편집 객체가 소멸할 때(잠금 밖) 반환된다: 떼어 낸 옛 항목, 통째 지정의 새
    // 컨테이너(실패한 준비), 쓰지 않은 키 사본, 예약(적용했으면 옛 저장소, 아니면 잡은 새 저장소),
    // 지운 키.
    Map detached_;
    Optional<Map> fresh_;
    Optional<K> inserted_;
    Optional<typename Map::Reservation> reservation_;
    Optional<K> erased_;
};

} // namespace detail

// 객체 안에 직접 포함하는 키 → 강한 GC 간선 Map이다(ObjectRefArray의 Map판 — 리플렉션 B9f4,
// Docs/Design/ReflectionExtensions.ko.md 7.4절, UE TMap<K, TObjectPtr<T>>). 값은 T 또는 T의 파생
// 객체 handle이거나 null이다(null은 정규 값 ObjectHandle{}로 저장하며 간선이 아니다). 순서는 삽입
// 순서다(Core OrderedMap — 같은 키 교체는 자리를 지키고, 지운 뒤 다시 넣으면 끝에 붙는다). 키는
// 리플렉션 Map 키 종류(정수·bool·enum·Guid·AssetId·Sha256Digest·String·Name·SoftObjectRef·
// ClassRef)다.
//
// 사용 예:
//   class Inventory : public Object {
//   public:
//       explicit Inventory(Allocator& allocator) noexcept : slots_(allocator) {
//       }
//       void Trace(TraceVisitor& visitor) const noexcept {
//           slots_.Trace(visitor);                         // 모든 값을 GC에 알린다
//       }
//       ObjectRefMap<Name, Item> slots_;
//   };
//
//   registry.SetMapReference(access, inventory, pointer->slots_, Name(...), item);  // 넣기·교체
//   registry.EraseMapReference(access, inventory, pointer->slots_, Name(...));      // 지우기
//   registry.ClearMapReferences(access, inventory, pointer->slots_);                // 비우기
//   if (const ObjectHandle* found = pointer->slots_.Find(key)) { ... }
//   for (auto [key, handle] : pointer->slots_.View()) { ... }                      // 삽입 순서
//
// 계약:
// - 소유 객체의 저장소 안에 직접 멤버로 둔다(클래스 필드·인라인 struct 멤버 — StructRefArray 원소
//   struct 안에는 둘 수 없다, RF 결정 15와 같은 이유). 항목 저장소와 String·Soft 키는 생성자에 준
//   allocator(보통 Registry allocator)에서 할당하며 그 allocator가 소유 객체보다 오래 살아야 한다.
// - live 변경은 ObjectRegistry의 SetMapReference·EraseMapReference·ClearMapReferences로만 하며,
//   호출 동안 소유 객체의 Borrow를 쥐고 있어야 한다. 생성 트랜잭션 안의 객체는
//   ConstructionContext::AssignMapReferences로 통째로 채운다.
// - non-null 값은 타입·생존·같은 Registry를 검증하고, 증분 GC Marking 중이면 대상을 shade한 뒤
//   게시한다(삽입 장벽). 소유자의 Trace는 Trace(visitor)로 모든 값을 조건 없이 방문해야 한다.
// - 읽기(Count·Find·Contains·View)는 소유자 Borrow 범위에서 소유 스레드가 한다. Find의 포인터와
//   View의 순회는 다음 변경까지만 유효하다(넣기의 성장·지우기의 압축이 모든 항목을 옮길 수 있다).
//   View().ForEach 순회 중의 Registry 변경은 Busy다.
// - 복사·이동은 소유자 검증을 우회하므로 금지한다. 항목은 소유 객체 소멸(저장소 회수) 때 반환한다.
template <class K, class T = Object>
class ObjectRefMap {
public:
    using Key = K;
    using Target = T;
    using MapType = OrderedMap<K, ObjectHandle>;

    explicit ObjectRefMap(Allocator& allocator) noexcept : storage_(allocator) {
    }

    ObjectRefMap(const ObjectRefMap&) = delete;
    ObjectRefMap& operator=(const ObjectRefMap&) = delete;
    ObjectRefMap(ObjectRefMap&&) = delete;
    ObjectRefMap& operator=(ObjectRefMap&&) = delete;
    ~ObjectRefMap() = default;

    usize Count() const noexcept {
        return storage_.map.Count();
    }

    bool IsEmpty() const noexcept {
        return storage_.map.IsEmpty();
    }

    // 키의 값(null 포함)을 가리키는 포인터이며 없으면 nullptr다. Q는 K이거나 String 키의
    // StringView, Soft 키의 SoftObjectRefView다. 포인터는 다음 변경까지만 유효하다.
    template <class Q>
    const ObjectHandle* Find(const Q& key) const noexcept {
        return storage_.map.Find(key);
    }

    template <class Q>
    bool Contains(const Q& key) const noexcept {
        return storage_.map.Contains(key);
    }

    // 삽입 순서 읽기 view다(ForEach·반복자·Find). 다음 변경까지만 유효하다.
    const MapType& View() const noexcept {
        return storage_.map;
    }

    // 항목 저장소와 키 사본을 할당하는 allocator다(생성자에 준 것).
    Allocator& GetAllocator() const noexcept {
        return storage_.map.GetAllocator();
    }

    // 소유자의 Trace에서 호출해 모든 값을 강한 간선으로 방문한다(순회 표시를 세우지 않는다 — 읽기만
    // 한다).
    void Trace(TraceVisitor& visitor) const noexcept {
        for (auto entry : storage_.map) {
            visitor.Visit(entry.value);
        }
    }

private:
    friend class ObjectRegistry;

    detail::ObjectRefMapStorage<K> storage_;
};

} // namespace rocks
