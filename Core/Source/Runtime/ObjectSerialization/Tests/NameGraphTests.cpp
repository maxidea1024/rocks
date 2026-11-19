#include "NameGraphTestSupport.h"

// 리플렉션 B9c2 Name 필드의 그래프·쓰기 시험이다(Docs/Design/ReflectionExtensions.ko.md 4절, 17절
// B9c 항목): MetaCompiler 생성 fixture(NameFieldFixture.h)로 Name 클래스 필드·struct
// 멤버·Array<Name> 원소(클래스 필드·struct 멤버·struct 원소 배열 안)를 표시 문자열 wire로
// 왕복하고(번호 접미·대소문자 표시 보존, None은 빈 문자열 — 다른 pool로 복원, 다시 저장한 byte
// 같음), pool 없는 Registry는 None만 받으며, 쓰기(등록 write 직접 호출 포함)·경로 쓰기·staged
// 쓰기·저장이 토큰의 pool·내용을 검사함을 본다. 복원 단계(1단계·2a)의 시험은
// NameGraphLoadTests.cpp다.
using namespace rocks;
namespace name_graph_tests_file {

using namespace rocks::test::names;

// 표시 문자열 wire로 왕복한다: 번호 접미(Socket_7·Weapon_0·None_3)와 대소문자 표시(SOCKET·weapon)가
// 남고, None(기본 None·"none" 표시 변형·빈 base의 번호 이름)은 빈 문자열로 저장되어 기본 None으로
// 돌아온다. 복원은 대상 Registry의 다른 pool로 intern하고(같은 비교 identity는 ==), 다시 저장한
// byte가 같다. None은 pool에 entry를 만들지 않는다.
TEST(NameGraph_RoundTripKeepsDisplayNumberAndNone) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    NamePool sourceNames(allocator);
    NamePool targetNames(allocator);
    REQUIRE(sourceNames.Initialize());
    REQUIRE(targetNames.Initialize());
    {
        ObjectRegistry source(allocator, types, {0x4e4d47, 1}, WithNames(&sourceNames));
        ObjectRegistry target(allocator, types, {0x4e4d47, 2}, WithNames(&targetNames));
        {
            auto created = NewFields(source, [&](NameFields& value) noexcept {
                value.label = Intern(sourceNames, "Socket_7");
                value.other = Intern(sourceNames, "SOCKET");
                value.socket.socket = Intern(sourceNames, "None_3");
                value.socket.slot = 5;
                if (!value.socket.aliases.TryPushBack(Intern(sourceNames, "\xC3\x9Cnit")) ||
                    !value.socket.aliases.TryPushBack(Name{}) ||
                    !value.socket.aliases.TryPushBack(Intern(sourceNames, "none"))) {
                    return false;
                }

                const char* names[] = {"Weapon", "weapon", "Weapon_0", "_7"};
                for (const char* text : names) {
                    if (!value.names.TryPushBack(Intern(sourceNames, text))) {
                        return false;
                    }
                }

                auto element =
                    value.group.sockets.TryEmplaceBack(value.group.sockets.GetAllocator());
                if (!element) {
                    return false;
                }

                element.Value()->socket = Intern(sourceNames, "Grip");
                return true;
            });
            REQUIRE(created);
            auto saved =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), created.Value().Handle());
            REQUIRE(saved);
            REQUIRE(targetNames.Stats().entryCount == 0);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          saved.Value().View());
            REQUIRE(loaded);
            Look(target, loaded.Value().Handle(), [&](const NameFields& value) noexcept {
                CHECK(DisplayIs(targetNames, value.label, "Socket_7"));
                CHECK(value.label.GetNumber() == 7);
                CHECK(DisplayIs(targetNames, value.other, "SOCKET"));
                CHECK(DisplayIs(targetNames, value.socket.socket, "None_3"));
                CHECK(value.socket.socket.GetNumber() == 3 && !value.socket.socket.IsNone());
                CHECK(value.socket.slot == 5);
                REQUIRE(value.socket.aliases.Size() == 3);
                CHECK(DisplayIs(targetNames, value.socket.aliases[0], "\xC3\x9Cnit"));
                CHECK(value.socket.aliases[1].kind == NameKind::None);
                // "none" 표시 변형은 빈 문자열로 저장되어 기본 None으로 돌아온다.
                CHECK(value.socket.aliases[2].kind == NameKind::None);
                REQUIRE(value.names.Size() == 4);
                CHECK(DisplayIs(targetNames, value.names[0], "Weapon"));
                CHECK(DisplayIs(targetNames, value.names[1], "weapon"));
                CHECK(value.names[0] == value.names[1]);
                CHECK(DisplayIs(targetNames, value.names[2], "Weapon_0"));
                CHECK(value.names[2].GetNumber() == 0 && !(value.names[2] == value.names[0]));
                CHECK(value.names[3].kind == NameKind::None);
                REQUIRE(value.group.sockets.Size() == 1);
                CHECK(DisplayIs(targetNames, value.group.sockets[0].socket, "Grip"));
            });
            // 표시 entry: Socket_7·SOCKET·None_3·Ünit·Weapon·weapon·Weapon_0·Grip(None은 entry
            // 없음).
            CHECK(targetNames.Stats().entryCount == 8);

            auto resaved =
                SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(resaved);
            CHECK(test::graph::SameBytes(saved.Value().View(), resaved.Value().View()));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(sourceNames.Shutdown());
    CHECK(targetNames.Shutdown());
    CHECK(types.Shutdown());
}

// pool 없는 Registry의 Name 필드에는 None만 둘 수 있다: None이 아닌 토큰의 필드·경로·staged 쓰기와
// 저장, None이 아닌 표시 문자열의 복원은 Unsupported이고(객체를 만들지 않는다) 값은 그대로다.
// None의 쓰기·저장·복원은 된다.
TEST(NameGraph_RegistryWithoutPoolKeepsOnlyNone) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    {
        ObjectRegistry plain(allocator, types, {0x4e4d47, 3});
        ObjectRegistry named(allocator, types, {0x4e4d47, 4}, WithNames(&names));
        const auto access = plain.OwnerAccess();
        {
            const Name token = Intern(names, "Grip");
            auto lease = types.Acquire(TypeIdOf<NameFields>());
            auto created = NewFields(plain, [](NameFields&) noexcept { return true; });
            REQUIRE(lease && created);
            const ObjectHandle handle = created.Value().Handle();
            auto path = [&](const char* text, PropertyPathBuffer& out) noexcept {
                REQUIRE(ParsePropertyPath(lease.Value(), StringView(text), out));
            };

            PropertyPathBuffer label;
            path("label", label);
            CHECK(ErrorOf(PathWrite(plain, access, handle, label.View(),
                                    NameValue(allocator, token))) == ErrorCode::Unsupported);
            CHECK(PathWrite(plain, access, handle, label.View(), NameValue(allocator, Name{})));
            PropertyPathBuffer member;
            path("socket.socket", member);
            CHECK(ErrorOf(PathWrite(plain, access, handle, member.View(),
                                    NameValue(allocator, token))) == ErrorCode::Unsupported);
            PropertyPathBuffer element;
            path("names[0]", element);
            const FieldValue named0 = NameValue(allocator, token);
            CHECK(ErrorOf(PathInsertElement(plain, access, handle, element.View(), &named0)) ==
                  ErrorCode::Unsupported);
            const FieldValue none = NameValue(allocator, Name{});
            CHECK(PathInsertElement(plain, access, handle, element.View(), &none));
            Look(plain, handle, [](const NameFields& value) noexcept {
                CHECK(value.label.kind == NameKind::None);
                CHECK(value.socket.socket.kind == NameKind::None);
                CHECK(value.names.Size() == 1 && value.names[0].kind == NameKind::None);
            });

            // staged 쓰기도 같다.
            const Guid labelId = SchemaField(TypeOf<NameFields>(), "label").id;
            const RuntimeFieldOps* labelOps = nullptr;
            for (const RuntimeFieldOps& ops : lease.Value().Ops().fields) {
                labelOps = ops.fieldId == labelId ? &ops : labelOps;
            }

            REQUIRE(labelOps && labelOps->writeStaged);
            {
                ConstructionContext construction(plain, access);
                auto staged = construction.Create<NameFields>(TypeIdOf<NameFields>());
                REQUIRE(staged);
                CHECK(ErrorOf(labelOps->writeStaged(construction, staged.Value(),
                                                    NameValue(allocator, token))) ==
                      ErrorCode::Unsupported);
                CHECK(labelOps->writeStaged(construction, staged.Value(),
                                            NameValue(allocator, Name{})));
            }

            // None만 담은 객체는 저장·복원된다. C++에서 직접 넣은 None이 아닌 토큰은 저장하지
            // 않는다.
            auto saved = SaveObjectGraph(allocator, plain, access, handle);
            REQUIRE(saved);
            CHECK(LoadGraph(allocator, types, plain, saved.Value().View()) == ErrorCode::None);
            Look(plain, handle, [&](NameFields& value) noexcept { value.other = token; });
            auto rejected = SaveObjectGraph(allocator, plain, access, handle);
            CHECK(!rejected && rejected.Error() == ErrorCode::Unsupported);
            Look(plain, handle, [](NameFields& value) noexcept { value.other = Name{}; });

            // pool 있는 Registry가 저장한 None이 아닌 값은 pool 없는 Registry가 복원하지 않는다.
            auto source = NewFields(named, [&](NameFields& value) noexcept {
                value.label = token;
                return true;
            });
            REQUIRE(source);
            auto namedGraph =
                SaveObjectGraph(allocator, named, named.OwnerAccess(), source.Value().Handle());
            REQUIRE(namedGraph);
            CHECK(LoadGraph(allocator, types, plain, namedGraph.Value().View()) ==
                  ErrorCode::Unsupported);
        }

        REQUIRE(plain.Shutdown(plain.OwnerAccess()));
        REQUIRE(named.Shutdown(named.OwnerAccess()));
    }

    CHECK(names.Shutdown());
    CHECK(types.Shutdown());
}

// pool 있는 Registry의 쓰기(최상위 필드·struct 멤버·배열 원소 삽입·교체·삭제·원소 struct 안
// 멤버·staged)와 저장은 토큰을 검사한다: 다른 pool의 토큰 WrongContext, 내용 규칙을 어긴 표시(같은
// pool에 intern한 NUL·분해형) InvalidArgument, 구조가 틀린 토큰 InvalidArgument. 실패한 쓰기는 값을
// 바꾸지 않고, 경로 읽기·옛 값은 토큰 그대로다.
TEST(NameGraph_WritesCheckPoolDomainAndContent) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    NamePool names(allocator);
    NamePool foreign(allocator);
    REQUIRE(names.Initialize());
    REQUIRE(foreign.Initialize());
    {
        ObjectRegistry objects(allocator, types, {0x4e4d47, 5}, WithNames(&names));
        const auto access = objects.OwnerAccess();
        {
            const Name grip = Intern(names, "Grip");
            const Name hand = Intern(names, "Hand_2");
            const Name stranger = Intern(foreign, "Grip");
            const Name withNul = Intern(names, StringView("a\0b", 3));
            const Name decomposed = Intern(names, "e\xCC\x81");
            auto lease = types.Acquire(TypeIdOf<NameFields>());
            auto created = NewFields(objects, [](NameFields&) noexcept { return true; });
            REQUIRE(lease && created);
            const ObjectHandle handle = created.Value().Handle();
            auto parse = [&](const char* text, PropertyPathBuffer& out) noexcept {
                REQUIRE(ParsePropertyPath(lease.Value(), StringView(text), out));
            };

            const char* writable[] = {"label", "socket.socket"};
            for (const char* text : writable) {
                PropertyPathBuffer path;
                parse(text, path);
                PropertyPathFailure failure;
                CHECK(ErrorOf(PathWrite(objects, access, handle, path.View(),
                                        NameValue(allocator, stranger), nullptr, &failure)) ==
                      ErrorCode::WrongContext);
                CHECK(failure.reason == PropertyPathError::None);
                CHECK(ErrorOf(PathWrite(objects, access, handle, path.View(),
                                        NameValue(allocator, withNul), nullptr, &failure)) ==
                      ErrorCode::InvalidArgument);
                CHECK(failure.reason == PropertyPathError::InvalidValue);
                CHECK(ErrorOf(PathWrite(objects, access, handle, path.View(),
                                        NameValue(allocator, decomposed))) ==
                      ErrorCode::InvalidArgument);
                FieldValue untouched(allocator);
                REQUIRE(PathRead(objects, access, handle, path.View(), untouched));
                CHECK(untouched.kind == ValueKind::Name && untouched.name.kind == NameKind::None);
                REQUIRE(
                    PathWrite(objects, access, handle, path.View(), NameValue(allocator, grip)));
                FieldValue previous(allocator);
                REQUIRE(PathWrite(objects, access, handle, path.View(), NameValue(allocator, hand),
                                  &previous));
                CHECK(previous.kind == ValueKind::Name && previous.name == grip);
                FieldValue current(allocator);
                REQUIRE(PathRead(objects, access, handle, path.View(), current));
                CHECK(current.kind == ValueKind::Name && current.name == hand &&
                      current.name.displayId == hand.displayId);
            }

            // 배열 원소: 삽입·교체·삭제(옛 값).
            PropertyPathBuffer element;
            parse("names[0]", element);
            const FieldValue strangerValue = NameValue(allocator, stranger);
            CHECK(ErrorOf(PathInsertElement(objects, access, handle, element.View(),
                                            &strangerValue)) == ErrorCode::WrongContext);
            const FieldValue gripValue = NameValue(allocator, grip);
            REQUIRE(PathInsertElement(objects, access, handle, element.View(), &gripValue));
            CHECK(ErrorOf(PathWrite(objects, access, handle, element.View(),
                                    NameValue(allocator, withNul))) == ErrorCode::InvalidArgument);
            FieldValue kept(allocator);
            REQUIRE(PathRead(objects, access, handle, element.View(), kept));
            CHECK(kept.name == grip);
            REQUIRE(PathWrite(objects, access, handle, element.View(), NameValue(allocator, hand)));
            FieldValue removed(allocator);
            REQUIRE(PathRemoveElement(objects, access, handle, element.View(), &removed));
            CHECK(removed.kind == ValueKind::Name && removed.name == hand);

            // 원소 struct 안의 Name 멤버·Name 배열 멤버.
            PropertyPathBuffer socketElement;
            parse("group.sockets[0]", socketElement);
            REQUIRE(PathInsertElement(objects, access, handle, socketElement.View(), nullptr));
            PropertyPathBuffer nested;
            parse("group.sockets[0].aliases[0]", nested);
            CHECK(ErrorOf(PathInsertElement(objects, access, handle, nested.View(),
                                            &strangerValue)) == ErrorCode::WrongContext);
            REQUIRE(PathInsertElement(objects, access, handle, nested.View(), &gripValue));
            PropertyPathBuffer nestedSocket;
            parse("group.sockets[0].socket", nestedSocket);
            REQUIRE(PathWrite(objects, access, handle, nestedSocket.View(),
                              NameValue(allocator, hand)));
            Look(objects, handle, [&](const NameFields& value) noexcept {
                CHECK(value.label == hand && value.socket.socket == hand);
                CHECK(value.names.IsEmpty());
                REQUIRE(value.group.sockets.Size() == 1);
                CHECK(value.group.sockets[0].socket == hand);
                CHECK(value.group.sockets[0].aliases.Size() == 1 &&
                      value.group.sockets[0].aliases[0] == grip);
            });

            // staged 쓰기.
            const Guid otherId = SchemaField(TypeOf<NameFields>(), "other").id;
            const RuntimeFieldOps* otherOps = nullptr;
            for (const RuntimeFieldOps& ops : lease.Value().Ops().fields) {
                otherOps = ops.fieldId == otherId ? &ops : otherOps;
            }

            REQUIRE(otherOps && otherOps->writeStaged && otherOps->read);
            {
                ConstructionContext construction(objects, access);
                auto staged = construction.Create<NameFields>(TypeIdOf<NameFields>());
                REQUIRE(staged);
                CHECK(ErrorOf(otherOps->writeStaged(construction, staged.Value(),
                                                    NameValue(allocator, stranger))) ==
                      ErrorCode::WrongContext);
                CHECK(ErrorOf(otherOps->writeStaged(construction, staged.Value(),
                                                    NameValue(allocator, decomposed))) ==
                      ErrorCode::InvalidArgument);
                REQUIRE(otherOps->writeStaged(construction, staged.Value(),
                                              NameValue(allocator, grip)));
                auto committed = construction.Commit();
                REQUIRE(committed);
                FieldValue read(allocator);
                REQUIRE(otherOps->read(objects, access, committed.Value().Handle(), read));
                CHECK(read.kind == ValueKind::Name && read.name == grip);
            }

            // 저장: C++에서 직접 넣은 토큰을 다시 검사한다.
            Name malformed = grip;
            malformed.id = 0;
            struct SaveCase {
                Name token;
                ErrorCode expected;
            };

            const SaveCase saves[] = {{stranger, ErrorCode::WrongContext},
                                      {withNul, ErrorCode::InvalidArgument},
                                      {malformed, ErrorCode::InvalidArgument},
                                      {grip, ErrorCode::None}};
            for (const SaveCase& example : saves) {
                Look(objects, handle,
                     [&](NameFields& value) noexcept { value.other = example.token; });
                auto saved = SaveObjectGraph(allocator, objects, access, handle);
                CHECK((saved ? ErrorCode::None : saved.Error()) == example.expected);
            }
        }

        REQUIRE(objects.Shutdown(access));
    }

    CHECK(names.Shutdown());
    CHECK(foreign.Shutdown());
    CHECK(types.Shutdown());
}

// 등록된 write(FieldWriteOperation — 수동 callback도 지나는 검사 지점)를 직접 불러도 토큰을
// 검사한다: 다른 pool의 토큰 WrongContext, pool 없는 Registry의 None이 아닌 토큰 Unsupported,
// 실패하면 값이 그대로다.
TEST(NameGraph_FieldWriteOperationChecksTokens) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    NamePool names(allocator);
    NamePool foreign(allocator);
    REQUIRE(names.Initialize());
    REQUIRE(foreign.Initialize());
    {
        ObjectRegistry objects(allocator, types, {0x4e4d47, 13}, WithNames(&names));
        ObjectRegistry plain(allocator, types, {0x4e4d47, 14});
        {
            auto lease = types.Acquire(TypeIdOf<NameFields>());
            REQUIRE(lease);
            const Guid labelId = SchemaField(TypeOf<NameFields>(), "label").id;
            const RuntimeFieldOps* labelOps = nullptr;
            for (const RuntimeFieldOps& ops : lease.Value().Ops().fields) {
                labelOps = ops.fieldId == labelId ? &ops : labelOps;
            }

            REQUIRE(labelOps && labelOps->write && labelOps->read);
            const Name grip = Intern(names, "Grip");
            const Name stranger = Intern(foreign, "Grip");
            auto created = NewFields(objects, [](NameFields&) noexcept { return true; });
            auto loose = NewFields(plain, [](NameFields&) noexcept { return true; });
            REQUIRE(created && loose);
            const ObjectHandle handle = created.Value().Handle();
            CHECK(ErrorOf(labelOps->write(objects, objects.OwnerAccess(), handle,
                                          NameValue(allocator, stranger))) ==
                  ErrorCode::WrongContext);
            Look(objects, handle, [](const NameFields& value) noexcept {
                CHECK(value.label.kind == NameKind::None);
            });
            REQUIRE(labelOps->write(objects, objects.OwnerAccess(), handle,
                                    NameValue(allocator, grip)));
            FieldValue read(allocator);
            REQUIRE(labelOps->read(objects, objects.OwnerAccess(), handle, read));
            CHECK(read.kind == ValueKind::Name && read.name == grip);

            const ObjectHandle looseHandle = loose.Value().Handle();
            CHECK(ErrorOf(labelOps->write(plain, plain.OwnerAccess(), looseHandle,
                                          NameValue(allocator, grip))) == ErrorCode::Unsupported);
            Look(plain, looseHandle, [](const NameFields& value) noexcept {
                CHECK(value.label.kind == NameKind::None);
            });
            CHECK(labelOps->write(plain, plain.OwnerAccess(), looseHandle,
                                  NameValue(allocator, Name{})));
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        REQUIRE(plain.Shutdown(plain.OwnerAccess()));
    }

    CHECK(names.Shutdown());
    CHECK(foreign.Shutdown());
    CHECK(types.Shutdown());
}
} // namespace name_graph_tests_file
