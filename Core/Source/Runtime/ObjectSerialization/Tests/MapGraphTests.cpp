#include "MapTestSupport.h"

// 리플렉션 B9f2 Map·Set PROPERTY의 그래프 저장·복원 시험이다(Docs/Design/ReflectionExtensions.ko.md
// 7.2절, 17절 B9f 항목): 키 종류별 왕복과 삽입 순서 보존(다시 저장한 byte 같음), 파일 안 중복 키
// 거부(String·Name 대소문자·None), 키 자리의 값 대체 정책 거부, MaxElements 저장 거부·로드 허용,
// wire 형식 오류.
using namespace rocks;
namespace map_graph_tests_file {

using namespace rocks::test::map;

namespace {

Sha256Digest DigestOf(uint8 seed) noexcept {
    Sha256Digest digest{};
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        digest.bytes[index] = static_cast<uint8>(seed + index);
    }

    return digest;
}

constexpr uint32 MapWireTag = static_cast<uint32>(WireType::Map);
constexpr uint32 SetWireTag = static_cast<uint32>(WireType::Set);

} // namespace

// 키 종류마다(정수·String·Name·enum·Guid·AssetId·Digest·int8·Soft·ClassRef) 값과 삽입 순서가
// 그대로 왕복한다: 해시 순서가 아니라 넣은 순서(같은 키 대입은 자리 유지, 지우고 다시 넣으면
// 끝)이고, 값 종류(int32·float·String·enum·digest·bool·int16·uint64·Name·값 struct·weak·Required
// Soft)와 Set 원소(String·Name·enum·ClassRef·Soft), struct 멤버 Map·Set과 그 원소 배열도 같다. Name
// 키는 다른 pool로 intern되고 표시 문자열(대소문자)이 보존된다. 복원한 그래프를 다시 저장하면
// byte가 같다.
TEST(MapGraph_RoundTripKeyKindsAndInsertionOrder) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    NamePool sourceNames(allocator);
    NamePool targetNames(allocator);
    REQUIRE(sourceNames.Initialize());
    REQUIRE(targetNames.Initialize());
    {
        ObjectRegistry source(allocator, types, {0x4d4150, 1}, WithNames(&sourceNames));
        ObjectRegistry target(allocator, types, {0x4d4150, 2}, WithNames(&targetNames));
        {
            RootRef<MapHolder> holder = New(source);
            auto android = sourceNames.TryIntern("Android");
            auto ios = sourceNames.TryIntern("IOS_2");
            REQUIRE(android && ios);
            Allocator& objects = source.ObjectAllocator();
            With(source, holder.Handle(), [&](MapHolder& value) noexcept {
                // 큰 키부터 넣고, 가운데 키를 다시 대입하고(자리 유지), 첫 키를 지운 뒤 다시
                // 넣는다(끝).
                Put(value.counts, uint32{30}, int32{-3});
                Put(value.counts, uint32{10}, int32{-1});
                Put(value.counts, uint32{20}, int32{-2});
                REQUIRE(value.counts.InsertOrAssign(uint32{10}, int32{-10}));
                REQUIRE(value.counts.Erase(uint32{30}));
                Put(value.counts, uint32{30}, int32{-30});
                Put(value.ratios, MakeString(objects, "zeta"), 0.5f);
                Put(value.ratios, MakeString(objects, "alpha"), 1.5f);
                Put(value.perPlatform, android.Value(), 0.25f);
                Put(value.perPlatform, ios.Value(), 0.75f);
                Put(value.colorNames, MapColor::Blue, MakeString(objects, "blue"));
                Put(value.colorNames, MapColor::Red, MakeString(objects, "red"));
                Put(value.guidColors, Guid{0x4d4150, 9}, MapColor::Green);
                Put(value.digests, AssetId{Guid{0x4d4150, 7}}, DigestOf(5));
                Put(value.digestFlags, DigestOf(9), true);
                Put(value.small, static_cast<int8>(-128), static_cast<int16>(-32768));
                Put(value.small, static_cast<int8>(127), static_cast<int16>(32767));
                SoftObjectRef<> key(objects);
                REQUIRE(key.TryAssign(AssetId{Guid{0x4d4150, 3}}, "Body.Socket"));
                Put(value.softKeys, Move(key), uint64{NumericLimits<uint64>::Max()});
                Put(value.classNames, ClassRef<MapKeyBase>::Of<MapKeyDerived>(), ios.Value());
                Put(value.classNames, ClassRef<MapKeyBase>(), android.Value());
                MapPoint point(objects);
                point.x = 4;
                point.y = -5;
                REQUIRE(point.label.TryAssign("spot"));
                Put(value.points, MakeString(objects, "p"), Move(point));
                MapVersioned versioned;
                versioned.value = 6;
                versioned.extra = 7;
                Put(value.versioned, uint32{1}, versioned);
                Put(value.weak, uint32{1}, WeakObjectRef<MapHolder>(holder.Handle()));
                Put(value.weak, uint32{2}, WeakObjectRef<MapHolder>());
                SoftObjectRef<> required(objects);
                REQUIRE(required.TryAssign(AssetId{Guid{0x4d4150, 4}}, ""));
                Put(value.required, uint32{8}, Move(required));
                REQUIRE(value.words.Add(MakeString(objects, "b")));
                REQUIRE(value.words.Add(MakeString(objects, "a")));
                REQUIRE(value.tags.Add(ios.Value()));
                REQUIRE(value.tags.Add(android.Value()));
                REQUIRE(value.colors.Add(MapColor::Green));
                REQUIRE(value.colors.Add(MapColor::Red));
                REQUIRE(value.classes.Add(ClassRef<MapKeyBase>::Of<MapKeyBase>()));
                MapPoint inner(objects);
                inner.x = 11;
                Put(value.bag.points, uint32{2}, Move(inner));
                REQUIRE(value.bag.words.Add(MakeString(objects, "bag")));
                REQUIRE(value.bags.TryEmplaceBack(objects));
                REQUIRE(value.bags.TryEmplaceBack(objects));
                REQUIRE(value.bags[1].words.Add(MakeString(objects, "second")));
            });

            auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
            REQUIRE(saved);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          saved.Value().View());
            REQUIRE(loaded);
            With(target, loaded.Value().Handle(), [&](MapHolder& value) noexcept {
                const uint32 countKeys[] = {10, 20, 30};
                const int32 countValues[] = {-10, -2, -30};
                usize index = 0;
                for (auto entry : value.counts) {
                    REQUIRE(index < 3);
                    CHECK(entry.key == countKeys[index] && entry.value == countValues[index]);
                    ++index;
                }

                CHECK(index == 3);
                auto ratio = value.ratios.begin();
                CHECK((*ratio).key.View() == "zeta" && (*ratio).value == 0.5f);
                ++ratio;
                CHECK((*ratio).key.View() == "alpha" && (*ratio).value == 1.5f);
                REQUIRE(value.perPlatform.Count() == 2);
                auto platform = value.perPlatform.begin();
                auto first = targetNames.Acquire((*platform).key);
                CHECK(first && first.Value().View() == "Android" && (*platform).value == 0.25f);
                ++platform;
                auto second = targetNames.Acquire((*platform).key);
                CHECK(second && second.Value().View() == "IOS_2");
                CHECK(value.colorNames.begin()->key == MapColor::Blue);
                CHECK(value.colorNames.Find(MapColor::Red)->View() == "red");
                CHECK(*value.guidColors.Find(Guid{0x4d4150, 9}) == MapColor::Green);
                CHECK(value.digests.Find(AssetId{Guid{0x4d4150, 7}})->bytes[31] == 36);
                CHECK(*value.digestFlags.Find(DigestOf(9)));
                CHECK(*value.small.Find(static_cast<int8>(-128)) == -32768);
                CHECK(*value.small.Find(static_cast<int8>(127)) == 32767);
                REQUIRE(value.softKeys.Count() == 1);
                CHECK(value.softKeys.begin()->key.objectPath.View() == "Body.Socket");
                CHECK(value.classNames.begin()->key == ClassRef<MapKeyBase>::Of<MapKeyDerived>());
                CHECK(value.classNames.Find(ClassRef<MapKeyBase>()) != nullptr);
                const MapPoint* point = value.points.Find(StringView("p"));
                REQUIRE(point);
                CHECK(point->x == 4 && point->y == -5 && point->label.View() == "spot");
                CHECK(value.versioned.Find(uint32{1})->extra == 7);
                CHECK(value.weak.Find(uint32{1})->Handle() == loaded.Value().Handle());
                CHECK(value.weak.Find(uint32{2})->Handle().IsNull());
                CHECK((value.required.Find(uint32{8})->asset == AssetId{Guid{0x4d4150, 4}}));
                CHECK(value.words.begin()->View() == "b");
                REQUIRE(value.tags.Count() == 2);
                auto tag = targetNames.Acquire(*value.tags.begin());
                CHECK(tag && tag.Value().View() == "IOS_2");
                CHECK(*value.colors.begin() == MapColor::Green);
                CHECK(value.classes.Contains(ClassRef<MapKeyBase>::Of<MapKeyBase>()));
                CHECK(value.bag.points.Find(uint32{2})->x == 11);
                CHECK(value.bag.words.Contains(StringView("bag")));
                REQUIRE(value.bags.Size() == 2);
                CHECK(value.bags[0].words.IsEmpty() &&
                      value.bags[1].words.Contains(StringView("second")));
            });

            auto again =
                SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(again);
            CHECK(test::graph::SameBytes(saved.Value().View(), again.Value().View()));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(sourceNames.Shutdown());
    CHECK(targetNames.Shutdown());
    CHECK(types.Shutdown());
}

// 파일 안 같은 키 둘은 손상이다(InvalidArgument — 1단계 판정): 정수 키, String 원소(byte 같음),
// Name 키(Core Name 규칙 — ASCII 대소문자만 다른 "Socket"·"SOCKET", None 표기 ""·"none"), Digest
// 키. byte가 다른 String("a"·"A")과 번호가 다른 Name("Socket_1"·"Socket_01" — 01은 번호가 아니다)은
// 다른 키라 받는다.
TEST(MapGraph_DuplicateKeysAreRejected) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    {
        ObjectRegistry objects(allocator, types, {0x4d4150, 3}, WithNames(&names));
        const auto counts = [&](uint32 first, uint32 second) noexcept {
            return MapPayload(allocator, WireType::U32, WireType::I64, 2,
                              [&](ArchiveWriter& out) noexcept {
                                  return out.WriteU32(first).IsOk() && out.WriteI64(1).IsOk() &&
                                         out.WriteU32(second).IsOk() && out.WriteI64(2).IsOk();
                              });
        };
        const auto words = [&](StringView first, StringView second) noexcept {
            return SetPayload(allocator, WireType::String, 2, [&](ArchiveWriter& out) noexcept {
                return out.WriteString(first).IsOk() && out.WriteString(second).IsOk();
            });
        };
        const auto platforms = [&](StringView first, StringView second) noexcept {
            return MapPayload(
                allocator, WireType::String, WireType::F32, 2, [&](ArchiveWriter& out) noexcept {
                    return out.WriteString(first).IsOk() && out.WriteF32(1.0f).IsOk() &&
                           out.WriteString(second).IsOk() && out.WriteF32(2.0f).IsOk();
                });
        };
        const auto load = [&](StringView field, uint32 wire, const Array<uint8>& payload) noexcept {
            const FieldBytes fields[] = {{HolderField(field), wire, payload.View()}};
            auto graph = HolderGraph(allocator, fields);
            return LoadCode(allocator, objects, types, graph.View());
        };

        CHECK(load("counts", MapWireTag, counts(5, 5)) == ErrorCode::InvalidArgument);
        CHECK(load("counts", MapWireTag, counts(5, 6)) == ErrorCode::None);
        CHECK(load("words", SetWireTag, words("a", "a")) == ErrorCode::InvalidArgument);
        CHECK(load("words", SetWireTag, words("a", "A")) == ErrorCode::None);
        CHECK(load("perPlatform", MapWireTag, platforms("Socket", "SOCKET")) ==
              ErrorCode::InvalidArgument);
        CHECK(load("perPlatform", MapWireTag, platforms("", "none")) == ErrorCode::InvalidArgument);
        CHECK(load("perPlatform", MapWireTag, platforms("Socket_1", "Socket_01")) ==
              ErrorCode::None);
        auto tags = SetPayload(allocator, WireType::String, 2, [](ArchiveWriter& out) noexcept {
            return out.WriteString("Tag_7").IsOk() && out.WriteString("tag_7").IsOk();
        });
        CHECK(load("tags", SetWireTag, tags) == ErrorCode::InvalidArgument);
        const Sha256Digest digest = DigestOf(1);
        auto flags = MapPayload(
            allocator, WireType::Bytes, WireType::Bool, 2, [&](ArchiveWriter& out) noexcept {
                const Span<const uint8> bytes(digest.bytes, Sha256Digest::Bytes);
                return out.WriteBytes(bytes).IsOk() && out.WriteBool(true).IsOk() &&
                       out.WriteBytes(bytes).IsOk() && out.WriteBool(false).IsOk();
            });
        CHECK(load("digestFlags", MapWireTag, flags) == ErrorCode::InvalidArgument);
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(names.Shutdown());
    CHECK(types.Shutdown());
}

// 키 자리(Map 키·Set 원소)는 값을 바꾸는 decode 정책을 쓰지 않는다(1.7절): 미선언 enum 키·원소는
// ReportAndDefault여도 InvalidArgument, 미등록 클래스 ClassRef 키·원소는 ReportAndNull이어도
// NotFound다. 값 자리(Map 값)의 미선언 enum은 정책대로 대체 값과 진단이다.
TEST(MapGraph_KeyPositionRejectsReplacementPolicies) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    Recorder hooks;
    ObjectGraphLoadOptions lenient;
    lenient.hooks = &hooks;
    lenient.policy.undeclaredEnum = UndeclaredEnumPolicy::ReportAndDefault;
    lenient.policy.unknownClass = UnknownClassPolicy::ReportAndNull;
    {
        ObjectRegistry objects(allocator, types, {0x4d4150, 4});
        const auto load = [&](StringView field, uint32 wire, const Array<uint8>& payload) noexcept {
            const FieldBytes fields[] = {{HolderField(field), wire, payload.View()}};
            auto graph = HolderGraph(allocator, fields);
            return LoadCode(allocator, objects, types, graph.View(), lenient);
        };

        auto colorKey = MapPayload(
            allocator, WireType::I64, WireType::String, 1, [](ArchiveWriter& out) noexcept {
                return out.WriteI64(0).IsOk() && out.WriteString("none").IsOk();
            });
        CHECK(load("colorNames", MapWireTag, colorKey) == ErrorCode::InvalidArgument);
        auto colorElement =
            SetPayload(allocator, WireType::I64, 1,
                       [](ArchiveWriter& out) noexcept { return out.WriteI64(8).IsOk(); });
        CHECK(load("colors", SetWireTag, colorElement) == ErrorCode::InvalidArgument);
        const Guid unknown{0x4d4150, 0xdead};
        auto classKey = MapPayload(
            allocator, WireType::Guid, WireType::String, 1, [&](ArchiveWriter& out) noexcept {
                return out.WriteGuid(unknown).IsOk() && out.WriteString("").IsOk();
            });
        CHECK(load("classNames", MapWireTag, classKey) == ErrorCode::NotFound);
        auto classElement =
            SetPayload(allocator, WireType::Guid, 1,
                       [&](ArchiveWriter& out) noexcept { return out.WriteGuid(unknown).IsOk(); });
        CHECK(load("classes", SetWireTag, classElement) == ErrorCode::NotFound);
        CHECK(hooks.count == 0);

        // 값 자리는 정책대로다(미선언 0 → 첫 열거자 Red, 진단 경로는 [guidColors[0]]).
        auto colorValue = MapPayload(
            allocator, WireType::Guid, WireType::I64, 1, [](ArchiveWriter& out) noexcept {
                return out.WriteGuid(Guid{1, 2}).IsOk() && out.WriteI64(0).IsOk();
            });
        const FieldBytes fields[] = {{HolderField("guidColors"), MapWireTag, colorValue.View()}};
        auto graph = HolderGraph(allocator, fields);
        auto loaded = LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(),
                                      graph.View(), lenient);
        REQUIRE(loaded);
        With(objects, loaded.Value().Handle(), [](MapHolder& value) noexcept {
            CHECK(*value.guidColors.Find(Guid{1, 2}) == MapColor::Red);
        });
        REQUIRE(hooks.count == 1);
        CHECK(hooks.entries[0].kind == FieldDiagnosticKind::UndeclaredEnumValue);
        CHECK(hooks.entries[0].steps == 1 &&
              hooks.entries[0].path[0].selector == PathSelector::Index &&
              hooks.entries[0].path[0].index == 0);
        loaded.Value().Reset();
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// MaxElements는 저장에서만 건다: 상한 2인 Map에 셋을 담으면 저장이 ArrayTooLong 진단(그 필드 경로,
// value = 3) 뒤 CapacityExceeded이고, 같은 셋을 담은 파일은 로드된다(로드는 MaxArrayElements와
// 그래프 한도만 — RF 결정 13).
TEST(MapGraph_MaxElementsLimitsSaveButNotLoad) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4d4150, 5});
        RootRef<MapHolder> holder = New(objects);
        With(objects, holder.Handle(), [](MapHolder& value) noexcept {
            Put(value.limited, uint32{1}, uint32{1});
            Put(value.limited, uint32{2}, uint32{2});
            Put(value.limited, uint32{3}, uint32{3});
        });
        Recorder hooks;
        ObjectGraphSaveOptions save;
        save.hooks = &hooks;
        auto saved =
            SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle(), save);
        REQUIRE(!saved);
        CHECK(saved.Error() == ErrorCode::CapacityExceeded);
        REQUIRE(hooks.count == 1);
        CHECK(hooks.entries[0].kind == FieldDiagnosticKind::ArrayTooLong);
        CHECK(hooks.entries[0].value == 3);
        CHECK(hooks.entries[0].steps == 1 &&
              hooks.entries[0].path[0].selector == PathSelector::None &&
              hooks.entries[0].path[0].fieldId == HolderField("limited"));

        auto three =
            MapPayload(allocator, WireType::U32, WireType::U32, 3, [](ArchiveWriter& out) noexcept {
                for (uint32 key = 1; key <= 3; ++key) {
                    if (!out.WriteU32(key) || !out.WriteU32(key)) {
                        return false;
                    }
                }

                return true;
            });
        const FieldBytes fields[] = {{HolderField("limited"), MapWireTag, three.View()}};
        auto graph = HolderGraph(allocator, fields);
        auto loaded =
            LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), graph.View());
        REQUIRE(loaded);
        With(objects, loaded.Value().Handle(),
             [](MapHolder& value) noexcept { CHECK(value.limited.Count() == 3); });
        loaded.Value().Reset();
        holder.Reset();
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// wire 형식 오류: 필드 wire가 Map이 아님(ValueArray — TypeMismatch), 저장 키 tag가 스키마 키 wire와
// 다름(TypeMismatch), 키 자리가 아닌 tag(F32 키 — InvalidArgument), count가 남은 byte보다 큼
// (InvalidArgument — 사전 검사), 남은 byte·사전 검사를 지나 항목 중간에서 잘린 입력
// (InvalidArgument), Set에 Map wire(TypeMismatch), Digest 키 길이 31(InvalidArgument), 값 struct
// 자리의 Record가 아닌 내용(InvalidArgument).
TEST(MapGraph_WireErrorsAreRejected) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4d4150, 6});
        const auto load = [&](StringView field, uint32 wire, const Array<uint8>& payload) noexcept {
            const FieldBytes fields[] = {{HolderField(field), wire, payload.View()}};
            auto graph = HolderGraph(allocator, fields);
            return LoadCode(allocator, objects, types, graph.View());
        };

        auto good =
            MapPayload(allocator, WireType::U32, WireType::I64, 1, [](ArchiveWriter& out) noexcept {
                return out.WriteU32(1).IsOk() && out.WriteI64(1).IsOk();
            });
        CHECK(load("counts", MapWireTag, good) == ErrorCode::None);
        CHECK(load("counts", static_cast<uint32>(WireType::ValueArray), good) ==
              ErrorCode::TypeMismatch);
        CHECK(load("words", MapWireTag, good) == ErrorCode::TypeMismatch);
        auto wrongKey =
            MapPayload(allocator, WireType::U64, WireType::I64, 1, [](ArchiveWriter& out) noexcept {
                return out.WriteU64(1).IsOk() && out.WriteI64(1).IsOk();
            });
        CHECK(load("counts", MapWireTag, wrongKey) == ErrorCode::TypeMismatch);
        auto floatKey =
            MapPayload(allocator, WireType::F32, WireType::I64, 1, [](ArchiveWriter& out) noexcept {
                return out.WriteF32(1).IsOk() && out.WriteI64(1).IsOk();
            });
        CHECK(load("counts", MapWireTag, floatKey) == ErrorCode::InvalidArgument);
        auto counted =
            MapPayload(allocator, WireType::U32, WireType::I64, 9, [](ArchiveWriter& out) noexcept {
                return out.WriteU32(1).IsOk() && out.WriteI64(1).IsOk();
            });
        CHECK(load("counts", MapWireTag, counted) == ErrorCode::InvalidArgument);
        auto trailing =
            MapPayload(allocator, WireType::U32, WireType::I64, 1, [](ArchiveWriter& out) noexcept {
                return out.WriteU32(1).IsOk() && out.WriteI64(1).IsOk() && out.WriteU32(0).IsOk();
            });
        CHECK(load("counts", MapWireTag, trailing) == ErrorCode::InvalidArgument);
        auto shortDigest = MapPayload(
            allocator, WireType::Bytes, WireType::Bool, 1, [](ArchiveWriter& out) noexcept {
                uint8 bytes[31] = {};
                return out.WriteBytes(Span<const uint8>(bytes, 31)).IsOk() &&
                       out.WriteBool(true).IsOk();
            });
        CHECK(load("digestFlags", MapWireTag, shortDigest) == ErrorCode::InvalidArgument);
        auto badRecord = MapPayload(allocator, WireType::String, WireType::Record, 1,
                                    [](ArchiveWriter& out) noexcept {
                                        uint8 junk[60] = {};
                                        return out.WriteString("p").IsOk() &&
                                               out.WriteBytes(Span<const uint8>(junk, 60)).IsOk();
                                    });
        CHECK(load("points", MapWireTag, badRecord) == ErrorCode::InvalidArgument);
        // 가변 키라 사전 검사는 합계 하한만 본다: 둘째 키의 길이 prefix가 남은 byte보다 길면 항목
        // 중간에서 잘린 입력이다(InvalidArgument).
        auto cut = MapPayload(
            allocator, WireType::String, WireType::F32, 2, [](ArchiveWriter& out) noexcept {
                const uint8 tail[] = {'x', 'y', 'z', '!'};
                return out.WriteString("a").IsOk() && out.WriteF32(1).IsOk() &&
                       out.WriteU64(100).IsOk() && out.WriteRaw(Span<const uint8>(tail, 4)).IsOk();
            });
        CHECK(load("ratios", MapWireTag, cut) == ErrorCode::InvalidArgument);
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 진단·수집 경로는 Map 항목·Set 원소의 삽입 순번이다(1.6절 — 키를 복사하지 않는다): non-null 소프트
// 키·원소 수집은 [softKeys[i]]·[assets[i]], Map 값 struct 안 멤버 수집은 [links[i], asset]이다.
// Required Map 값·Set 원소의 null은 rejectMissingRequired 저장이 RequiredMissing 진단 뒤
// InvalidArgument이고 경로는 그 항목 순번이다.
TEST(MapGraph_DiagnosticPathsUseInsertionIndex) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4d4150, 7});
        RootRef<MapHolder> holder = New(objects);
        Allocator& objectAllocator = objects.ObjectAllocator();
        With(objects, holder.Handle(), [&](MapHolder& value) noexcept {
            SoftObjectRef<> nullKey(objectAllocator);
            Put(value.softKeys, Move(nullKey), uint64{1});
            SoftObjectRef<> key(objectAllocator);
            REQUIRE(key.TryAssign(AssetId{Guid{7, 1}}, ""));
            Put(value.softKeys, Move(key), uint64{2});
            MapLink link(objectAllocator);
            REQUIRE(link.asset.TryAssign(AssetId{Guid{7, 2}}, ""));
            Put(value.links, uint32{4}, MapLink(objectAllocator));
            Put(value.links, uint32{5}, Move(link));
            SoftObjectRef<> element(objectAllocator);
            REQUIRE(element.TryAssign(AssetId{Guid{7, 3}}, ""));
            REQUIRE(value.assets.Add(Move(element)));
        });

        Recorder collected;
        ObjectGraphSaveOptions plain;
        plain.hooks = &collected;
        auto saved =
            SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle(), plain);
        REQUIRE(saved);
        Recorder loadHooks;
        ObjectGraphLoadOptions options;
        options.hooks = &loadHooks;
        auto loaded = LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(),
                                      saved.Value().View(), options);
        REQUIRE(loaded);
        // 1단계 Soft 수집: softKeys[1](null 키는 수집하지 않는다), links[1](값 struct 멤버),
        // assets[0].
        REQUIRE(loadHooks.softCount == 3);
        bool softKey = false;
        bool link = false;
        bool asset = false;
        for (uint32 index = 0; index < loadHooks.softCount; ++index) {
            const PathStep& step = loadHooks.softPaths[index];
            softKey = softKey || (step.fieldId == HolderField("softKeys") &&
                                  step.selector == PathSelector::Index && step.index == 1);
            link = link || (step.fieldId == HolderField("links") &&
                            step.selector == PathSelector::Index && step.index == 1);
            asset = asset || (step.fieldId == HolderField("assets") &&
                              step.selector == PathSelector::Index && step.index == 0);
        }

        CHECK(softKey && link && asset);
        // Required 위치(links[0].asset — 값 struct 멤버)의 null은 2c 진단이고 경로는 [links[0],
        // asset]이다.
        REQUIRE(loadHooks.count == 1);
        CHECK(loadHooks.entries[0].kind == FieldDiagnosticKind::RequiredMissing);
        CHECK(loadHooks.entries[0].steps == 2 &&
              loadHooks.entries[0].path[0].selector == PathSelector::Index &&
              loadHooks.entries[0].path[0].index == 0);

        // rejectMissingRequired 저장: Required Map 값의 null은 [required[1]]로 진단 뒤
        // InvalidArgument.
        With(objects, holder.Handle(), [&](MapHolder& value) noexcept {
            value.links.Clear();
            SoftObjectRef<> present(objectAllocator);
            REQUIRE(present.TryAssign(AssetId{Guid{7, 4}}, ""));
            Put(value.required, uint32{1}, Move(present));
            Put(value.required, uint32{2}, SoftObjectRef<>(objectAllocator));
        });
        Recorder rejected;
        ObjectGraphSaveOptions strict;
        strict.hooks = &rejected;
        strict.policy.rejectMissingRequired = true;
        auto refused =
            SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle(), strict);
        REQUIRE(!refused);
        CHECK(refused.Error() == ErrorCode::InvalidArgument);
        REQUIRE(rejected.count == 1);
        CHECK(rejected.entries[0].kind == FieldDiagnosticKind::RequiredMissing);
        CHECK(rejected.entries[0].path[0].fieldId == HolderField("required") &&
              rejected.entries[0].path[0].selector == PathSelector::Index &&
              rejected.entries[0].path[0].index == 1);

        // Required Set 원소의 null도 원소마다 거부한다([assets[1]] — 키 자리라도 Set 원소는 필드의
        // Required를 본다).
        With(objects, holder.Handle(), [&](MapHolder& value) noexcept {
            value.required.Clear();
            REQUIRE(value.assets.Add(SoftObjectRef<>(objectAllocator)));
        });
        Recorder nullElement;
        strict.hooks = &nullElement;
        auto refusedSet =
            SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle(), strict);
        REQUIRE(!refusedSet);
        CHECK(refusedSet.Error() == ErrorCode::InvalidArgument);
        REQUIRE(nullElement.count == 1);
        CHECK(nullElement.entries[0].kind == FieldDiagnosticKind::RequiredMissing);
        CHECK(nullElement.entries[0].path[0].fieldId == HolderField("assets") &&
              nullElement.entries[0].path[0].selector == PathSelector::Index &&
              nullElement.entries[0].path[0].index == 1);
        loaded.Value().Reset();
        holder.Reset();
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}
} // namespace map_graph_tests_file
