#include "ObjectRefMapTestSupport.h"

#include <TestSupport/Test.h>

// ObjectRefMap(리플렉션 B9f4 — Docs/Design/ReflectionExtensions.ko.md 7.4절) Registry API·staged·GC
// 시험이다: 넣기·교체·지우기·비우기와 삽입 순서, 검증(owner·Borrow·위치·대상), null 값, 삽입 장벽과
// cutoff, 순회 중 Busy, staged AssignMapReferences(규칙·Commit 검증·Marking shade). 재진입·실패
// 주입·관찰자는 ObjectRefMapReentryTests.cpp다.
using namespace rocks;
using namespace refmap_fixture;

namespace {

const StringView Alpha("alpha-key");
const StringView Beta("beta-key");
const StringView Gamma("gamma-key");

} // namespace

// 넣기는 끝에, 같은 키 교체는 자리를 지키고, 지운 뒤 다시 넣으면 끝에 붙는다. null 값은 항목으로
// 남지만 간선이 아니다. 조회는 할당하지 않는 형식(StringView)으로 한다. 값은 대상을 살린다.
TEST(ObjectRefMap_SetEraseClearKeepInsertionOrder) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x5246, 1});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        REQUIRE(holder);
        const ObjectHandle owner = holder.Value().Handle();
        const ObjectHandle first = Unrooted(objects);
        const ObjectHandle second = Unrooted(objects);
        const ObjectHandle third = Unrooted(objects);
        const ObjectHandle dropped = Unrooted(objects);
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            Holder* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr);
            ObjectRefMap<String, Item>& named = pointer->named;
            REQUIRE(objects.SetMapReference(access, owner, named, Alpha, first));
            REQUIRE(objects.SetMapReference(access, owner, named, Beta, dropped));
            REQUIRE(objects.SetMapReference(access, owner, named, Gamma, ObjectHandle{}));
            const StringView inserted[] = {Alpha, Beta, Gamma};
            CHECK(NamedOrderIs(named, inserted, 3));
            CHECK(named.Find(Gamma) != nullptr && *named.Find(Gamma) == ObjectHandle{});

            // 교체는 자리를 지킨다(비정규 null도 정규화한다).
            REQUIRE(objects.SetMapReference(access, owner, named, Beta, second));
            REQUIRE(objects.SetMapReference(access, owner, named, Gamma,
                                            ObjectHandle{first.registry, 9, 0}));
            CHECK(NamedOrderIs(named, inserted, 3) && *named.Find(Beta) == second &&
                  *named.Find(Gamma) == ObjectHandle{});

            // 지운 뒤 다시 넣으면 끝에 붙는다. 없는 키 지우기는 NotFound이고 그대로다.
            REQUIRE(objects.EraseMapReference(access, owner, named, Alpha));
            CHECK(ErrorOf(objects.EraseMapReference(access, owner, named, Alpha)) ==
                  ErrorCode::NotFound);
            REQUIRE(objects.SetMapReference(access, owner, named, Alpha, third));
            const StringView reordered[] = {Beta, Gamma, Alpha};
            CHECK(NamedOrderIs(named, reordered, 3));

            // String 키 자체로도 찾는다(같은 해시·같음).
            String key(allocator);
            REQUIRE(key.TryAssign(Alpha));
            CHECK(named.Contains(key) && *named.Find(key) == third);

            // 정수 키 Map은 키 값 그대로다.
            REQUIRE(objects.SetMapReference(access, owner, pointer->numbered, uint32{7}, first));
            REQUIRE(objects.SetMapReference(access, owner, pointer->numbered, uint32{3}, {}));
            CHECK(pointer->numbered.Count() == 2 && *pointer->numbered.Find(uint32{7}) == first);
        }

        // null 값은 간선이 아니고, 교체로 빠진 대상(dropped)은 회수되며 값은 대상을 살린다.
        REQUIRE(objects.Collect(access));
        CHECK(IsAlive(objects, first) && IsAlive(objects, second) && IsAlive(objects, third));
        CHECK(!IsAlive(objects, dropped));
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            Holder* pointer = Get(borrowed.Value());
            REQUIRE(objects.ClearMapReferences(access, owner, pointer->named));
            CHECK(pointer->named.IsEmpty() && pointer->numbered.Count() == 2);
            REQUIRE(objects.ClearMapReferences(access, owner, pointer->numbered));
        }

        REQUIRE(objects.Collect(access));
        CHECK(!IsAlive(objects, first) && !IsAlive(objects, second) && !IsAlive(objects, third));
        holder.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 검증은 ObjectRefArray와 같다: 대상 타입(TypeMismatch)·생존(StaleHandle)·다른 Registry
// (StaleHandle), 컨테이너가 owner 밖이거나 Borrow가 없으면 InvalidArgument, DestroyRequested
// owner의 넣기·교체는 StaleHandle이고 지우기·비우기는 된다. 실패하면 컨테이너는 그대로다. 잘못된
// 호출은 할당하지 않는다.
TEST(ObjectRefMap_EditsValidateOwnerAndTargetLikeArrays) {
    ScriptedAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x5246, 2});
        ObjectRegistry foreign(allocator, types, {0x5246, 3});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        auto other = objects.NewObject<OtherItem>(access, NativeTypeIdOf<OtherItem>());
        auto item = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        auto stranger = foreign.NewObject<Item>(foreign.OwnerAccess(), NativeTypeIdOf<Item>());
        REQUIRE(holder && other && item && stranger);
        const ObjectHandle owner = holder.Value().Handle();
        const ObjectHandle target = item.Value().Handle();
        Holder* pointer = nullptr;
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            pointer = Get(borrowed.Value());
            REQUIRE(objects.SetMapReference(access, owner, pointer->named, Alpha, target));
            const uint64 before = allocator.allocations;
            CHECK(ErrorOf(objects.SetMapReference(access, owner, pointer->named, Beta,
                                                  other.Value().Handle())) ==
                  ErrorCode::TypeMismatch);
            CHECK(ErrorOf(objects.SetMapReference(access, owner, pointer->named, Alpha,
                                                  other.Value().Handle())) ==
                  ErrorCode::TypeMismatch);
            CHECK(ErrorOf(objects.SetMapReference(access, owner, pointer->named, Beta,
                                                  stranger.Value().Handle())) ==
                  ErrorCode::StaleHandle);
            auto victim = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
            REQUIRE(victim);
            REQUIRE(objects.RequestDestroy(access, victim.Value().Handle()));
            const uint64 afterVictim = allocator.allocations;
            CHECK(ErrorOf(objects.SetMapReference(access, owner, pointer->named, Beta,
                                                  victim.Value().Handle())) ==
                  ErrorCode::StaleHandle);
            // owner 밖 컨테이너.
            ObjectRefMap<String, Item> local(allocator);
            CHECK(ErrorOf(objects.SetMapReference(access, owner, local, Beta, target)) ==
                  ErrorCode::InvalidArgument);
            CHECK(ErrorOf(objects.EraseMapReference(access, owner, local, Beta)) ==
                  ErrorCode::InvalidArgument);
            CHECK(ErrorOf(objects.ClearMapReferences(access, owner, local)) ==
                  ErrorCode::InvalidArgument);
            CHECK(allocator.allocations == afterVictim && afterVictim > before);
            CHECK(pointer->named.Count() == 1 && *pointer->named.Find(Alpha) == target);
        }

        // Borrow 없는 owner.
        CHECK(ErrorOf(objects.SetMapReference(access, owner, pointer->named, Beta, target)) ==
              ErrorCode::InvalidArgument);
        CHECK(ErrorOf(objects.EraseMapReference(access, owner, pointer->named, Alpha)) ==
              ErrorCode::InvalidArgument);
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            REQUIRE(objects.RequestDestroy(access, owner));
            CHECK(ErrorOf(objects.SetMapReference(access, owner, pointer->named, Beta, target)) ==
                  ErrorCode::StaleHandle);
            CHECK(ErrorOf(objects.SetMapReference(access, owner, pointer->named, Alpha, {})) ==
                  ErrorCode::StaleHandle);
            REQUIRE(objects.EraseMapReference(access, owner, pointer->named, Alpha));
            REQUIRE(objects.ClearMapReferences(access, owner, pointer->numbered));
            CHECK(pointer->named.IsEmpty());
        }

        holder.Value().Reset();
        other.Value().Reset();
        item.Value().Reset();
        stranger.Value().Reset();
        REQUIRE(objects.Collect(access));
        REQUIRE(foreign.Collect(foreign.OwnerAccess()));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.backing.Stats().liveAllocations == 0);
}

// 삽입 장벽: 이미 검게 스캔된 owner에 흰 대상을 넣거나(새 키 — 새 저장소 게시 포함) 교체로 넣으면
// shade되어 이번 cycle을 넘긴다. 지운 값은 장벽 없이 회수된다. cutoff 뒤의 흰 객체는 넣기·교체·
// staged 지정으로 되살릴 수 없다(StaleHandle — 컨테이너 불변).
TEST(ObjectRefMap_EditsShadeDuringMarkingAndRejectCutoff) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x5246, 4});
        const auto access = objects.OwnerAccess();
        // holder가 slot 0이라 root snapshot 큐의 맨 앞이다(첫 slice에서 검게 된다). 패딩 루트가
        // Marking을 이어 가게 한다.
        auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        auto padFirst = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        auto padSecond = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        REQUIRE(holder && padFirst && padSecond);
        const ObjectHandle owner = holder.Value().Handle();
        const ObjectHandle inserted = Unrooted(objects);
        const ObjectHandle replaced = Unrooted(objects);
        const ObjectHandle grown = Unrooted(objects);
        const ObjectHandle erased = Unrooted(objects);
        const ObjectHandle garbage = Unrooted(objects);
        {
            // 용량 4를 채운다(다음 새 키가 새 저장소를 게시하는 경로를 탄다).
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            Holder* pointer = Get(borrowed.Value());
            for (uint32 key = 0; key < 3; ++key) {
                REQUIRE(objects.SetMapReference(access, owner, pointer->numbered, key, {}));
            }

            REQUIRE(objects.SetMapReference(access, owner, pointer->numbered, uint32{3}, erased));
            REQUIRE(pointer->numbered.View().Capacity() == 4);
        }

        REQUIRE(objects.BeginCollection(access));
        REQUIRE(StepOnce(objects));
        REQUIRE(objects.CollectionPhase() == GcPhase::Marking);
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            Holder* pointer = Get(borrowed.Value());
            REQUIRE(objects.SetMapReference(access, owner, pointer->numbered, uint32{1}, replaced));
            REQUIRE(objects.SetMapReference(access, owner, pointer->numbered, uint32{10}, grown));
            CHECK(pointer->numbered.View().Capacity() == 8);
            REQUIRE(objects.SetMapReference(access, owner, pointer->named, Alpha, inserted));
            REQUIRE(objects.EraseMapReference(access, owner, pointer->numbered, uint32{3}));
        }

        REQUIRE(FinishCollection(objects));
        CHECK(IsAlive(objects, inserted) && IsAlive(objects, replaced) && IsAlive(objects, grown));
        CHECK(!IsAlive(objects, garbage));
        // erased는 owner가 스캔될 때 값이었으므로 이번 cycle을 넘기고 다음 수집에서 회수된다.
        REQUIRE(objects.Collect(access));
        CHECK(!IsAlive(objects, erased));

        const ObjectHandle white = Unrooted(objects);
        REQUIRE(objects.BeginCollection(access));
        for (uint32 guard = 0; guard < 64 && objects.CollectionPhase() == GcPhase::Marking;
             ++guard) {
            REQUIRE(StepOnce(objects));
        }

        REQUIRE(objects.CollectionPhase() == GcPhase::Sweeping);
        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            Holder* pointer = Get(borrowed.Value());
            CHECK(ErrorOf(objects.SetMapReference(access, owner, pointer->named, Beta, white)) ==
                  ErrorCode::StaleHandle);
            CHECK(ErrorOf(objects.SetMapReference(access, owner, pointer->named, Alpha, white)) ==
                  ErrorCode::StaleHandle);
            CHECK(pointer->named.Count() == 1 && *pointer->named.Find(Alpha) == inserted);
        }

        {
            ConstructionContext transaction(objects, access);
            auto staged = transaction.Create<Holder>(NativeTypeIdOf<Holder>());
            REQUIRE(staged);
            auto stagedHolder = transaction.Get<Holder>(staged.Value());
            REQUIRE(stagedHolder);
            const uint32 keys[] = {1};
            const ObjectHandle targets[] = {white};
            CHECK(ErrorOf(transaction.AssignMapReferences(
                      staged.Value(), stagedHolder.Value()->numbered, Span<const uint32>(keys),
                      Span<const ObjectHandle>(targets))) == ErrorCode::StaleHandle);
            CHECK(stagedHolder.Value()->numbered.IsEmpty());
        }

        REQUIRE(FinishCollection(objects));
        holder.Value().Reset();
        padFirst.Value().Reset();
        padSecond.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// View().ForEach 순회 중의 넣기·교체·지우기·비우기는 Core의 Fatal에 닿기 전에 Busy다(조회는 된다).
TEST(ObjectRefMap_IterationMakesEditsBusy) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x5246, 5});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
        auto item = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        REQUIRE(holder && item);
        const ObjectHandle owner = holder.Value().Handle();
        const ObjectHandle target = item.Value().Handle();
        auto borrowed = objects.Resolve<Holder>(access, owner);
        REQUIRE(borrowed);
        Holder* pointer = Get(borrowed.Value());
        REQUIRE(objects.SetMapReference(access, owner, pointer->numbered, uint32{1}, target));
        ErrorCode results[4] = {};
        bool found = false;
        pointer->numbered.View().ForEach([&](const uint32&, const ObjectHandle&) noexcept {
            results[0] = ErrorOf(objects.SetMapReference(access, owner, pointer->numbered,
                                                         uint32{1}, ObjectHandle{}));
            results[1] = ErrorOf(
                objects.SetMapReference(access, owner, pointer->numbered, uint32{2}, target));
            results[2] =
                ErrorOf(objects.EraseMapReference(access, owner, pointer->numbered, uint32{1}));
            results[3] = ErrorOf(objects.ClearMapReferences(access, owner, pointer->numbered));
            found = pointer->numbered.Contains(uint32{1});
        });
        for (ErrorCode result : results) {
            CHECK(result == ErrorCode::Busy);
        }

        CHECK(found && pointer->numbered.Count() == 1 &&
              *pointer->numbered.Find(uint32{1}) == target);
        REQUIRE(objects.ClearMapReferences(access, owner, pointer->numbered));
        borrowed.Value().Reset();
        holder.Value().Reset();
        item.Value().Reset();
        REQUIRE(objects.Collect(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// staged AssignMapReferences: owner는 이 트랜잭션의 Reserved, 값은 null·생존·이 트랜잭션의 객체,
// 다른 트랜잭션·타입 불일치·키와 값 길이 불일치·같은 키 둘은 실패하고 컨테이너는 그대로다. 통째
// 지정은 옛 내용을 바꾸고 순서는 키 순서다. Commit은 owner Trace로 값을 다시 검증해 지정 뒤 파괴
// 요청된 대상이면 StaleHandle로 아무것도 게시하지 않는다.
TEST(ConstructionContext_AssignMapReferencesStagedRules) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x5246, 6});
        const auto access = objects.OwnerAccess();
        auto live = objects.NewObject<Item>(access, NativeTypeIdOf<Item>());
        auto other = objects.NewObject<OtherItem>(access, NativeTypeIdOf<OtherItem>());
        REQUIRE(live && other);
        RootRef<Object> committedRoot;
        ObjectHandle committedOwner;
        ObjectHandle stagedItem;
        {
            ConstructionContext transaction(objects, access);
            auto owner = transaction.Create<Holder>(NativeTypeIdOf<Holder>());
            auto inner = transaction.Create<Item>(NativeTypeIdOf<Item>());
            REQUIRE(owner && inner);
            stagedItem = inner.Value();
            auto pointer = transaction.Get<Holder>(owner.Value());
            REQUIRE(pointer);
            ObjectRefMap<String, Item>& named = pointer.Value()->named;
            String keys[3] = {String(allocator), String(allocator), String(allocator)};
            REQUIRE(keys[0].TryAssign(Gamma) && keys[1].TryAssign(Alpha) &&
                    keys[2].TryAssign(Beta));
            const ObjectHandle targets[] = {inner.Value(), {}, live.Value().Handle()};
            REQUIRE(transaction.AssignMapReferences(owner.Value(), named,
                                                    Span<const String>(keys, 3),
                                                    Span<const ObjectHandle>(targets)));
            const StringView order[] = {Gamma, Alpha, Beta};
            CHECK(NamedOrderIs(named, order, 3) && *named.Find(Gamma) == inner.Value());

            // 실패는 내용을 바꾸지 않는다: 길이 불일치·같은 키 둘·타입 불일치.
            const ObjectHandle two[] = {live.Value().Handle(), live.Value().Handle()};
            CHECK(ErrorOf(transaction.AssignMapReferences(
                      owner.Value(), named, Span<const String>(keys, 3),
                      Span<const ObjectHandle>(two))) == ErrorCode::InvalidArgument);
            String duplicate[2] = {String(allocator), String(allocator)};
            REQUIRE(duplicate[0].TryAssign(Alpha) && duplicate[1].TryAssign(Alpha));
            CHECK(ErrorOf(transaction.AssignMapReferences(
                      owner.Value(), named, Span<const String>(duplicate, 2),
                      Span<const ObjectHandle>(two))) == ErrorCode::InvalidArgument);
            const ObjectHandle wrong[] = {other.Value().Handle()};
            CHECK(ErrorOf(transaction.AssignMapReferences(
                      owner.Value(), named, Span<const String>(keys, 1),
                      Span<const ObjectHandle>(wrong))) == ErrorCode::TypeMismatch);
            CHECK(NamedOrderIs(named, order, 3));

            // 다른 트랜잭션의 공개 전 객체는 대상도 owner도 될 수 없다.
            {
                ConstructionContext second(objects, access);
                auto foreignOwner = second.Create<Holder>(NativeTypeIdOf<Holder>());
                REQUIRE(foreignOwner);
                const uint32 one[] = {1};
                const ObjectHandle foreignTarget[] = {inner.Value()};
                auto foreignPointer = second.Get<Holder>(foreignOwner.Value());
                REQUIRE(foreignPointer);
                CHECK(ErrorOf(second.AssignMapReferences(
                          foreignOwner.Value(), foreignPointer.Value()->numbered,
                          Span<const uint32>(one), Span<const ObjectHandle>(foreignTarget))) ==
                      ErrorCode::StaleHandle);
                CHECK(ErrorOf(transaction.AssignMapReferences(
                          foreignOwner.Value(), foreignPointer.Value()->numbered,
                          Span<const uint32>(one), Span<const ObjectHandle>(foreignTarget))) ==
                      ErrorCode::StaleHandle);
            }

            auto committed = transaction.Commit();
            REQUIRE(committed);
            committedRoot = Move(committed.Value());
            committedOwner = owner.Value();
        }

        // 값이 staged 객체와 생존 객체를 살린다.
        live.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(IsAlive(objects, stagedItem));
        {
            ConstructionContext transaction(objects, access);
            auto owner = transaction.Create<Holder>(NativeTypeIdOf<Holder>());
            REQUIRE(owner);
            auto pointer = transaction.Get<Holder>(owner.Value());
            REQUIRE(pointer);
            const uint32 keys[] = {4, 5};
            const ObjectHandle targets[] = {ObjectHandle{}, stagedItem};
            REQUIRE(transaction.AssignMapReferences(owner.Value(), pointer.Value()->numbered,
                                                    Span<const uint32>(keys),
                                                    Span<const ObjectHandle>(targets)));
            REQUIRE(objects.RequestDestroy(access, stagedItem));
            CHECK(transaction.Commit().Error() == ErrorCode::StaleHandle);
            CHECK(objects.Stats().reservedObjects == 1);
        }

        CHECK(objects.Stats().reservedObjects == 0);
        committedRoot.Reset();
        other.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// slice 사이 Marking에서 staged Map에 넣은 생존 대상은 바로 shade된다: Commit 뒤 owner가 스캔되기
// 전에 값을 지워도 이번 cycle에서 회수되지 않는다(다음 수집에서 회수된다). Reserved 대상은 Commit이
// shade한다.
TEST(ConstructionContext_AssignMapReferencesShadesAliveTargetsDuringMarking) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        ObjectRegistry objects(allocator, types, {0x5246, 7});
        const auto access = objects.OwnerAccess();
        // padding은 trace되는(잎이 아닌) 타입이라 slice마다 하나씩 처리되어 Marking이 남는다(잎
        // 타입 Item은 표식만 하고 큐에 넣지 않는다 — GC M2).
        RootRef<Holder> padding[3];
        for (RootRef<Holder>& root : padding) {
            auto created = objects.NewObject<Holder>(access, NativeTypeIdOf<Holder>());
            REQUIRE(created);
            root = Move(created.Value());
        }

        const ObjectHandle white = Unrooted(objects);
        REQUIRE(objects.BeginCollection(access));
        REQUIRE(StepOnce(objects));
        REQUIRE(objects.CollectionPhase() == GcPhase::Marking);
        RootRef<Object> root;
        ObjectHandle owner;
        ObjectHandle reserved;
        {
            ConstructionContext transaction(objects, access);
            auto created = transaction.Create<Holder>(NativeTypeIdOf<Holder>());
            auto inner = transaction.Create<Item>(NativeTypeIdOf<Item>());
            REQUIRE(created && inner);
            owner = created.Value();
            reserved = inner.Value();
            auto pointer = transaction.Get<Holder>(owner);
            REQUIRE(pointer);
            const uint32 keys[] = {1, 2, 3};
            const ObjectHandle targets[] = {white, ObjectHandle{}, reserved};
            REQUIRE(transaction.AssignMapReferences(owner, pointer.Value()->numbered,
                                                    Span<const uint32>(keys),
                                                    Span<const ObjectHandle>(targets)));
            auto committed = transaction.Commit();
            REQUIRE(committed);
            root = Move(committed.Value());
        }

        {
            auto borrowed = objects.Resolve<Holder>(access, owner);
            REQUIRE(borrowed);
            REQUIRE(objects.EraseMapReference(access, owner, Get(borrowed.Value())->numbered,
                                              uint32{1}));
        }

        REQUIRE(FinishCollection(objects));
        CHECK(IsAlive(objects, white) && IsAlive(objects, reserved));
        REQUIRE(objects.Collect(access));
        CHECK(!IsAlive(objects, white) && IsAlive(objects, reserved));
        root.Reset();
        for (RootRef<Holder>& padded : padding) {
            padded.Reset();
        }

        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
