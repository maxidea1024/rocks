#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/OrderedSet.h>
#include <Core/Hash/Sha256.h>
#include <Core/Optional.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/AssetId.h>
#include <Object/ClassRef.h>
#include <Object/Object.h>
#include <Object/ObjectRefs.h>
#include <Object/SoftObjectRef.h>

namespace rocks::reflection_fixture {

// 리플렉션 B9f2 Map·Set PROPERTY fixture다(Docs/Design/ReflectionExtensions.ko.md 7.2절).
// MapColor는 값 0이 선언되지 않은 enum(키 자리 정책 시험)이고, MapKeyBase·MapKeyDerived는 ClassRef
// 키·원소의 Base·파생 클래스다. MapPoint는 allocator를 받는 값 struct(Map 값), MapVersioned는 버전
// 2 값 struct(Map 값 자리 옛 버전 1 Record의 마이그레이션), MapLink는 Required 멤버를 담는 값
// struct, MapBag은 Map·Set 멤버를 담는 struct(struct 안 Map — 관찰·2단계 선청구 대조)다.
// MapTagBag(Name 원소 Set)·MapPerPlatform(Name 키 Map — A5 PerPlatformFloat 모양)은 Name 담김
// 형질이 Set 원소·Map 키로 전파되는지 보는 struct다. MapHolder는 키 종류별 Map, 값 종류별
// Map(enum·digest·weak·Required Soft·값 struct), 원소 종류별 Set(bool 포함), 상한 2의 Map과 MapBag
// 멤버·원소 배열을 담고 그래프 왕복·키 중복·키 자리 정책·진단 경로·선청구·예약 상한·key 단계
// 경로(B9f3)로 시험한다(MapGraphTests·MapGraphLoadTests·MapPathTests). MapLockedHolder는 ReadOnly
// Map·Set·struct 필드다(MapPathTests).
ROCKS_ENUM()
enum class MapColor : uint8 {
    Red = 1,
    Green = 2,
    Blue = 4
};

ROCKS_CLASS()
struct MapKeyBase : Object {
    ROCKS_GENERATED_BODY()
};

ROCKS_CLASS()
struct MapKeyDerived final : MapKeyBase {
    ROCKS_GENERATED_BODY()
};

ROCKS_STRUCT()
struct MapPoint {
    ROCKS_GENERATED_BODY()

    explicit MapPoint(Allocator& allocator) noexcept : label(allocator) {
    }

    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() int32 y = 0;
    ROCKS_PROPERTY() String label;
};

ROCKS_STRUCT(Version = 2)
struct MapVersioned {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 value = 0;
    ROCKS_PROPERTY() int32 extra = 0;
};

ROCKS_STRUCT()
struct MapLink {
    ROCKS_GENERATED_BODY()

    explicit MapLink(Allocator& allocator) noexcept : asset(allocator) {
    }

    ROCKS_PROPERTY(Required) SoftObjectRef<> asset;
    ROCKS_PROPERTY() uint32 weight = 0;
};

ROCKS_STRUCT()
struct MapBag {
    ROCKS_GENERATED_BODY()

    explicit MapBag(Allocator& allocator) noexcept : points(allocator), words(allocator) {
    }

    ROCKS_PROPERTY() OrderedMap<uint32, MapPoint> points;
    ROCKS_PROPERTY() OrderedSet<String> words;
};

// Name 원소 Set 멤버 struct다(관찰 struct 제한 — Name 담김 형질이 Set 원소로 전파된다).
ROCKS_STRUCT()
struct MapTagBag {
    ROCKS_GENERATED_BODY()

    explicit MapTagBag(Allocator& allocator) noexcept : tags(allocator) {
    }

    ROCKS_PROPERTY() OrderedSet<Name> tags;
};

// Name 키 Map 멤버 struct다(A5 Downscale PerPlatformFloat 모양 — Name 담김 형질이 Map 키로
// 전파된다).
ROCKS_STRUCT()
struct MapPerPlatform {
    ROCKS_GENERATED_BODY()

    explicit MapPerPlatform(Allocator& allocator) noexcept : perPlatform(allocator) {
    }

    ROCKS_PROPERTY() float defaultValue = 0;
    ROCKS_PROPERTY() OrderedMap<Name, float> perPlatform;
};

ROCKS_CLASS()
struct MapHolder final : Object {
    ROCKS_GENERATED_BODY()

    explicit MapHolder(Allocator& allocator) noexcept
        : counts(allocator), ratios(allocator), perPlatform(allocator), colorNames(allocator),
          guidColors(allocator), digests(allocator), digestFlags(allocator), small(allocator),
          softKeys(allocator), classNames(allocator), points(allocator), versioned(allocator),
          links(allocator), weak(allocator), required(allocator), words(allocator), tags(allocator),
          colors(allocator), classes(allocator), assets(allocator), limited(allocator),
          bag(allocator), bags(allocator), tagBag(allocator), flags(allocator) {
    }

    ROCKS_PROPERTY() OrderedMap<uint32, int32> counts;
    ROCKS_PROPERTY() OrderedMap<String, float> ratios;
    ROCKS_PROPERTY() OrderedMap<Name, float> perPlatform;
    ROCKS_PROPERTY() OrderedMap<MapColor, String> colorNames;
    ROCKS_PROPERTY() OrderedMap<Guid, MapColor> guidColors;
    ROCKS_PROPERTY() OrderedMap<AssetId, Sha256Digest> digests;
    ROCKS_PROPERTY() OrderedMap<Sha256Digest, bool> digestFlags;
    ROCKS_PROPERTY() OrderedMap<int8, int16> small;
    ROCKS_PROPERTY() OrderedMap<SoftObjectRef<>, uint64> softKeys;
    ROCKS_PROPERTY() OrderedMap<ClassRef<MapKeyBase>, Name> classNames;
    ROCKS_PROPERTY() OrderedMap<String, MapPoint> points;
    ROCKS_PROPERTY() OrderedMap<uint32, MapVersioned> versioned;
    ROCKS_PROPERTY() OrderedMap<uint32, MapLink> links;
    ROCKS_PROPERTY() OrderedMap<uint32, WeakObjectRef<MapHolder>> weak;
    ROCKS_PROPERTY(Required) OrderedMap<uint32, SoftObjectRef<>> required;
    ROCKS_PROPERTY() OrderedSet<String> words;
    ROCKS_PROPERTY() OrderedSet<Name> tags;
    ROCKS_PROPERTY() OrderedSet<MapColor> colors;
    ROCKS_PROPERTY() OrderedSet<ClassRef<MapKeyBase>> classes;
    ROCKS_PROPERTY(Required) OrderedSet<SoftObjectRef<>> assets;
    ROCKS_PROPERTY(MaxElements = 2) OrderedMap<uint32, uint32> limited;
    ROCKS_PROPERTY() MapBag bag;
    ROCKS_PROPERTY() Array<MapBag> bags;
    ROCKS_PROPERTY() MapTagBag tagBag;
    ROCKS_PROPERTY() OrderedSet<bool> flags;
};

// 리플렉션 B9f3 경로 시험 보충이다: MapAccess는 Flags enum(Map 키 — `A+B` 표기), MapKeyLeaf는
// 시험이 따로 등록·해제하는 ClassRef 키 클래스, MapPathHolder는 Flags 키 Map·ClassRef 값 Map·상한
// 2의 Set·Optional<MapBag>(Optional 안 Map)·String·Soft 값 Map·String 키 값 struct Map(key 두 개
// 경로)을 담는다(MapPathEdgeTests·MapPathReentryTests).
ROCKS_ENUM(Flags)
enum class MapAccess : uint8 {
    Read = 1,
    Write = 2,
    Exec = 4
};

ROCKS_CLASS()
struct MapKeyLeaf final : MapKeyBase {
    ROCKS_GENERATED_BODY()
};

ROCKS_CLASS()
struct MapPathHolder final : Object {
    ROCKS_GENERATED_BODY()

    explicit MapPathHolder(Allocator& allocator) noexcept
        : byAccess(allocator), classValues(allocator), limitedSet(allocator), texts(allocator),
          softs(allocator), nested(allocator) {
    }

    ROCKS_PROPERTY() OrderedMap<MapAccess, uint32> byAccess;
    ROCKS_PROPERTY() OrderedMap<uint32, ClassRef<MapKeyBase>> classValues;
    ROCKS_PROPERTY(MaxElements = 2) OrderedSet<uint32> limitedSet;
    ROCKS_PROPERTY() Optional<MapBag> maybeBag;
    ROCKS_PROPERTY() OrderedMap<uint32, String> texts;
    ROCKS_PROPERTY() OrderedMap<uint32, SoftObjectRef<>> softs;
    ROCKS_PROPERTY() OrderedMap<String, MapBag> nested;
};

// ReadOnly Map·Set·struct 필드다(리플렉션 B9f3 경로 시험 — ReadOnly가 key 단계 쓰기·넣기·지우기·
// 비우기를 막고 struct 필드의 ReadOnly는 멤버 Map으로 전파된다. 조회·원소 수는 막지 않는다).
ROCKS_CLASS()
struct MapLockedHolder final : Object {
    ROCKS_GENERATED_BODY()

    explicit MapLockedHolder(Allocator& allocator) noexcept
        : locked(allocator), lockedWords(allocator), lockedBag(allocator) {
    }

    ROCKS_PROPERTY(ReadOnly) OrderedMap<uint32, uint32> locked;
    ROCKS_PROPERTY(ReadOnly) OrderedSet<String> lockedWords;
    ROCKS_PROPERTY(ReadOnly) MapBag lockedBag;
};

} // namespace rocks::reflection_fixture
