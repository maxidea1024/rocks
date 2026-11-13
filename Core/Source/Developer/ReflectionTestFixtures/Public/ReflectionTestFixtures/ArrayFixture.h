#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/Vector3.h>
#include <Object/ObjectRefArray.h>
#include <Object/StructRefArray.h>
#include <ReflectionTestFixtures/ReflectionFixture.h>

namespace rocks::reflection_fixture {

struct ArrayOwner;

// B5 리플렉션 배열 필드 fixture다(Docs/Design/ReflectionFoundations.ko.md 5.6절). MetaCompiler 생성
// 코드로 ArrayGraphTests(손으로 쓴 특수화)와 같은 모양의 그래프를 만든다: 모든 컨테이너(Array·
// ObjectRefArray·StructRefArray)와 원소 종류(bool·uint8·int32·uint64·double·Guid·String·AssetId·
// Sha256Digest·일반·Flags enum·SoftObjectRef<T>·SoftObjectRef<>·Binds Vec3·참조 없는 struct),
// 인라인 struct 안 Registry 컨테이너, Required 참조 배열, MaxElements 지정, 다른 헤더의 원소
// enum(ReflectionFixture.h)과 상속한 배열 필드(ArrayChild).

// 참조 없는 struct 원소다(Array<ArrayTag>). 원소 struct 안 배열을 담는다.
ROCKS_STRUCT()
struct ArrayTag {
    explicit ArrayTag(Allocator& allocator) noexcept : label(allocator), codes(allocator) {
    }

    ROCKS_PROPERTY() uint32 id = 0;
    ROCKS_PROPERTY() String label;
    ROCKS_PROPERTY() Array<uint16> codes;
};

// 참조 struct 원소다(StructRefArray<ArraySlot>). 원소 struct 안 참조와 배열을 담는다.
ROCKS_STRUCT()
struct ArraySlot {
    explicit ArraySlot(Allocator& allocator) noexcept : name(allocator), weights(allocator) {
    }

    ROCKS_PROPERTY() ObjectRef<ArrayOwner> material;
    ROCKS_PROPERTY() String name;
    ROCKS_PROPERTY() Array<float> weights;
};

// 인라인 참조 struct다(클래스 필드). Registry 컨테이너 둘을 담으므로 ContainsReferenceArray다.
ROCKS_STRUCT()
struct ArrayBundle {
    explicit ArrayBundle(Allocator& allocator) noexcept : refs(allocator), slots(allocator) {
    }

    ROCKS_PROPERTY() ObjectRefArray<ArrayOwner> refs;
    ROCKS_PROPERTY() StructRefArray<ArraySlot> slots;
};

ROCKS_CLASS()
struct ArrayOwner : Object {
    explicit ArrayOwner(Allocator& allocator) noexcept
        : flags(allocator), longs(allocator), doubles(allocator), guids(allocator),
          assets(allocator), digests(allocator), ints(allocator), names(allocator),
          groups(allocator), channels(allocator), softs(allocator), anySofts(allocator),
          points(allocator), tags(allocator), links(allocator), required(allocator),
          slots(allocator), bundle(allocator) {
        ++constructed;
    }

    static inline uint32 constructed = 0;
    ROCKS_PROPERTY() Array<bool> flags;
    ROCKS_PROPERTY() Array<uint64> longs;
    ROCKS_PROPERTY() Array<double> doubles;
    ROCKS_PROPERTY() Array<Guid> guids;
    ROCKS_PROPERTY() Array<AssetId> assets;
    ROCKS_PROPERTY() Array<Sha256Digest> digests;
    ROCKS_PROPERTY() Array<int32> ints;
    ROCKS_PROPERTY(MaxElements = 4) Array<String> names;
    ROCKS_PROPERTY() Array<TextureGroup> groups;
    ROCKS_PROPERTY() Array<Channels> channels;
    ROCKS_PROPERTY() Array<SoftObjectRef<ArrayOwner>> softs;
    // 대상 Object(기본 인자 생략 철자 SoftObjectRef<>)인 소프트 참조 원소다.
    ROCKS_PROPERTY() Array<SoftObjectRef<>> anySofts;
    ROCKS_PROPERTY() Array<Vec3> points;
    ROCKS_PROPERTY() Array<ArrayTag> tags;
    // 원소 null을 허용한다(슬롯 배열).
    ROCKS_PROPERTY() ObjectRefArray<ArrayOwner> links;
    // 원소마다 non-null이어야 한다(기본 인자 생략 철자 ObjectRefArray<> — 대상 Object).
    ROCKS_PROPERTY(Required) ObjectRefArray<> required;
    ROCKS_PROPERTY() StructRefArray<ArraySlot> slots;
    ROCKS_PROPERTY() ArrayBundle bundle;
};

// 배열 필드를 상속한다(생성 클래스 Trace가 상속 배열 필드도 방문한다).
ROCKS_CLASS()
struct ArrayChild final : ArrayOwner {
    explicit ArrayChild(Allocator& allocator) noexcept : ArrayOwner(allocator), extra(allocator) {
    }

    ROCKS_PROPERTY() Array<uint8> extra;
};

} // namespace rocks::reflection_fixture
