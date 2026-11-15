#include "WeakRefGraphTestSupport.h"

// 리플렉션 B9d1 WeakRef PROPERTY의 쓰기 검증 시험이다(Docs/Design/ReflectionExtensions.ko.md
// 5.1절): 등록된 write(FieldWriteOperation — 경로의 최상위 필드 쓰기도 같은 경로)·경로 쓰기(struct
// 멤버·Array 원소 쓰기·삽입·원소 struct 안 멤버)·staged 쓰기가 대상이 같은 Registry의 생존
// 객체(staged는 이 트랜잭션의 객체도)이고 기대 타입의 파생인지 보고(null 허용), 실패한 쓰기는 값을
// 바꾸지 않으며, 읽기는 대상이 죽은 뒤에도 저장된 handle을 돌려준다(weak는 GC 간선이 아니다).
using namespace rocks;
using namespace rocks::test::weak;

namespace {

const RuntimeFieldOps* FindOps(const TypeLease& lease, Guid id) noexcept {
    for (const RuntimeFieldOps& ops : lease.Ops().fields) {
        if (ops.fieldId == id) {
            return &ops;
        }
    }

    return nullptr;
}

} // namespace

// 최상위 weak 필드(경로 쓰기·등록 write 직접 호출)와 struct 멤버·배열 원소의 경로 쓰기: 생존
// WeakHolder(기대 WeakTarget의 파생)와 null은 받고, WeakOther는 TypeMismatch, 파괴 요청한 대상은
// Destroyed, 다른 Registry의 객체는 WrongContext로 실패하며 값은 그대로다. 기대 타입 Object 필드는
// WeakOther도 받는다. 경로 읽기·옛 값은 weak handle이다.
TEST(WeakRefField_WritesCheckLiveTargetAndExpectedType) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x575246, 1});
        ObjectRegistry foreign(allocator, types, {0x575246, 2});
        const auto access = objects.OwnerAccess();
        {
            auto lease = types.Acquire(TypeIdOf<WeakHolder>());
            REQUIRE(lease);
            RootRef<WeakHolder> holder = New<WeakHolder>(objects);
            RootRef<WeakHolder> peer = New<WeakHolder>(objects);
            RootRef<WeakOther> other = New<WeakOther>(objects);
            RootRef<WeakHolder> doomed = New<WeakHolder>(objects);
            RootRef<WeakHolder> stranger = New<WeakHolder>(foreign);
            REQUIRE(objects.RequestDestroy(access, doomed.Handle()));
            const ObjectHandle handle = holder.Handle();
            auto parse = [&](const char* text, PropertyPathBuffer& out) noexcept {
                REQUIRE(ParsePropertyPath(lease.Value(), StringView(text), out));
            };

            PropertyPathBuffer element;
            parse("targets[0]", element);
            const FieldValue peerValue = WeakValue(allocator, peer.Handle());
            REQUIRE(PathInsertElement(objects, access, handle, element.View(), &peerValue));
            PropertyPathBuffer groupElement;
            parse("group.links[0]", groupElement);
            REQUIRE(PathInsertElement(objects, access, handle, groupElement.View(), nullptr));

            const char* paths[] = {"target", "link.target", "targets[0]", "group.links[0].target"};
            for (const char* text : paths) {
                PropertyPathBuffer path;
                parse(text, path);
                REQUIRE(PathWrite(objects, access, handle, path.View(),
                                  WeakValue(allocator, peer.Handle())));
                PropertyPathFailure failure;
                CHECK(ErrorOf(PathWrite(objects, access, handle, path.View(),
                                        WeakValue(allocator, other.Handle()), nullptr, &failure)) ==
                      ErrorCode::TypeMismatch);
                CHECK(ErrorOf(PathWrite(objects, access, handle, path.View(),
                                        WeakValue(allocator, doomed.Handle()))) ==
                      ErrorCode::Destroyed);
                CHECK(ErrorOf(PathWrite(objects, access, handle, path.View(),
                                        WeakValue(allocator, stranger.Handle()))) ==
                      ErrorCode::WrongContext);
                FieldValue kept(allocator);
                REQUIRE(PathRead(objects, access, handle, path.View(), kept));
                CHECK(kept.kind == ValueKind::WeakObjectReference &&
                      kept.reference == peer.Handle());
                // WeakHolder 자신(WeakTarget 파생)으로 바꾸고 옛 값을 받는다.
                FieldValue previous(allocator);
                REQUIRE(PathWrite(objects, access, handle, path.View(),
                                  WeakValue(allocator, handle), &previous));
                CHECK(previous.kind == ValueKind::WeakObjectReference &&
                      previous.reference == peer.Handle());
                REQUIRE(PathWrite(objects, access, handle, path.View(), WeakValue(allocator, {})));
            }

            // 배열 원소 삽입도 같은 대상 검사를 한다.
            const FieldValue otherValue = WeakValue(allocator, other.Handle());
            CHECK(ErrorOf(PathInsertElement(objects, access, handle, element.View(),
                                            &otherValue)) == ErrorCode::TypeMismatch);
            FieldValue removed(allocator);
            REQUIRE(PathRemoveElement(objects, access, handle, element.View(), &removed));
            CHECK(removed.kind == ValueKind::WeakObjectReference &&
                  removed.reference == ObjectHandle{});

            // 기대 타입 Object 필드는 WeakOther도 받는다. 등록 write를 직접 불러도 같은 검사다.
            PropertyPathBuffer any;
            parse("any", any);
            REQUIRE(PathWrite(objects, access, handle, any.View(),
                              WeakValue(allocator, other.Handle())));
            const RuntimeFieldOps* targetOps = FindOps(lease.Value(), HolderField("target"));
            REQUIRE(targetOps && targetOps->write && targetOps->read);
            CHECK(ErrorOf(targetOps->write(objects, access, handle,
                                           WeakValue(allocator, other.Handle()))) ==
                  ErrorCode::TypeMismatch);
            REQUIRE(targetOps->write(objects, access, handle, WeakValue(allocator, peer.Handle())));
            With(objects, handle, [&](WeakHolder& value) noexcept {
                CHECK(value.target.Handle() == peer.Handle());
                CHECK(value.any.Handle() == other.Handle());
                CHECK(value.link.target.Handle() == ObjectHandle{});
                CHECK(value.targets.IsEmpty());
                REQUIRE(value.group.links.Size() == 1);
                CHECK(value.group.links[0].target.Handle() == ObjectHandle{});
            });

            // 대상이 죽은 뒤에도 읽기는 저장된 handle이다(weak는 대상을 붙잡지 않는다).
            const ObjectHandle peerHandle = peer.Handle();
            REQUIRE(objects.RequestDestroy(access, peerHandle));
            peer.Reset();
            REQUIRE(objects.Collect(access));
            FieldValue stale(allocator);
            REQUIRE(targetOps->read(objects, access, handle, stale));
            CHECK(stale.kind == ValueKind::WeakObjectReference && stale.reference == peerHandle);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        REQUIRE(foreign.Shutdown(foreign.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// staged 쓰기(writeStaged)는 같은 트랜잭션의 공개 전 객체와 생존 객체를 받고, 기대 타입 위반
// (TypeMismatch)·파괴 요청한 객체(StaleHandle — staged 참조 대상 규칙)·다른 Registry의
// 객체(WrongContext)는 거부한다. Commit 뒤 값이 남는다.
TEST(WeakRefField_StagedWritesAcceptTransactionObjects) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x575246, 3});
        ObjectRegistry foreign(allocator, types, {0x575246, 4});
        const auto access = objects.OwnerAccess();
        {
            auto lease = types.Acquire(TypeIdOf<WeakHolder>());
            REQUIRE(lease);
            const RuntimeFieldOps* targetOps = FindOps(lease.Value(), HolderField("target"));
            const RuntimeFieldOps* anyOps = FindOps(lease.Value(), HolderField("any"));
            REQUIRE(targetOps && targetOps->writeStaged && anyOps && anyOps->writeStaged);
            RootRef<WeakHolder> live = New<WeakHolder>(objects);
            RootRef<WeakOther> other = New<WeakOther>(objects);
            RootRef<WeakHolder> doomed = New<WeakHolder>(objects);
            RootRef<WeakHolder> stranger = New<WeakHolder>(foreign);
            REQUIRE(objects.RequestDestroy(access, doomed.Handle()));
            ConstructionContext construction(objects, access);
            auto owner = construction.Create<WeakHolder>(TypeIdOf<WeakHolder>());
            auto sibling = construction.Create<WeakHolder>(TypeIdOf<WeakHolder>());
            REQUIRE(owner && sibling);
            CHECK(ErrorOf(targetOps->writeStaged(construction, owner.Value(),
                                                 WeakValue(allocator, other.Handle()))) ==
                  ErrorCode::TypeMismatch);
            CHECK(ErrorOf(targetOps->writeStaged(construction, owner.Value(),
                                                 WeakValue(allocator, doomed.Handle()))) ==
                  ErrorCode::StaleHandle);
            CHECK(ErrorOf(targetOps->writeStaged(construction, owner.Value(),
                                                 WeakValue(allocator, stranger.Handle()))) ==
                  ErrorCode::WrongContext);
            REQUIRE(targetOps->writeStaged(construction, owner.Value(),
                                           WeakValue(allocator, sibling.Value())));
            REQUIRE(anyOps->writeStaged(construction, owner.Value(),
                                        WeakValue(allocator, live.Handle())));
            const ObjectHandle roots[] = {owner.Value(), sibling.Value()};
            RootRef<Object> held[2];
            REQUIRE(construction.Commit(roots, held));
            With(objects, owner.Value(), [&](WeakHolder& value) noexcept {
                CHECK(value.target.Handle() == sibling.Value());
                CHECK(value.any.Handle() == live.Handle());
            });
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        REQUIRE(foreign.Shutdown(foreign.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}
