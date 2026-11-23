#pragma once

#include <AssetRuntime/Package.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/OrderedSet.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Optional.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <Localization/Text.h>
#include <Meta/Annotations.h>
#include <Object/AssetId.h>
#include <Object/ClassRef.h>
#include <Object/InstancedStruct.h>
#include <Object/SoftObjectRef.h>

// 에셋 너겟 키 기록 시험의 설정 struct다(Docs/Design/AssetSystem 9.1절 결정 2 — A3a
// WriteReflected). 키에 쓸 수 있는 모든 값 종류(정수 폭·부호, 실수 둘, bool, Guid·AssetId·Digest,
// String·Name, enum, Soft 참조, 배열·정적 배열·byte 배열·struct 배열, Set·Map·struct 값 Map,
// Optional 단일 값·struct, 중첩 struct, InstancedStruct, Transient)를 담는다. 객체 참조와 Text는
// 키에 쓸 수 없으므로 거부 시험의 별도 struct에 둔다.
namespace rocks::asset_fixture {

ROCKS_ENUM()
enum class KeyedMode : int16 {
    Off = -1,
    Fast = 0,
    Best = 9
};

// 중첩 struct다.
ROCKS_STRUCT()
struct KeyedPoint {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() int32 y = 0;
};

// 모든 값 종류의 설정이다.
ROCKS_STRUCT()
struct KeyedSettings {
    ROCKS_GENERATED_BODY()

    explicit KeyedSettings(Allocator& allocator) noexcept
        : label(allocator), source(allocator), numbers(allocator), points(allocator),
          payload(allocator), tags(allocator), scores(allocator), byIndex(allocator) {
    }

    ROCKS_PROPERTY() bool enabled = false;
    ROCKS_PROPERTY() int8 tiny = 0;
    ROCKS_PROPERTY() int16 narrow = 0;
    ROCKS_PROPERTY() int32 offset = 0;
    ROCKS_PROPERTY() uint8 small = 0;
    ROCKS_PROPERTY() uint16 medium = 0;
    ROCKS_PROPERTY() uint32 count = 0;
    ROCKS_PROPERTY() int64 wide = 0;
    ROCKS_PROPERTY() uint64 big = 0;
    ROCKS_PROPERTY() float ratio = 0.0f;
    ROCKS_PROPERTY() double precise = 0.0;
    ROCKS_PROPERTY() Guid id {};
    ROCKS_PROPERTY() AssetId asset;
    ROCKS_PROPERTY() Sha256Digest digest {};
    ROCKS_PROPERTY() String label;
    ROCKS_PROPERTY() Name tag;
    ROCKS_PROPERTY() KeyedMode mode = KeyedMode::Fast;
    ROCKS_PROPERTY() SoftObjectRef<> source;
    ROCKS_PROPERTY() Array<int32> numbers;
    ROCKS_PROPERTY() Array<KeyedPoint> points;
    ROCKS_PROPERTY() int32 triple[3] = {};
    ROCKS_PROPERTY() Array<uint8> payload;
    ROCKS_PROPERTY() OrderedSet<Name> tags;
    ROCKS_PROPERTY() OrderedMap<String, int32> scores;
    ROCKS_PROPERTY() OrderedMap<int32, KeyedPoint> byIndex;
    ROCKS_PROPERTY() Optional<int32> limit;
    ROCKS_PROPERTY() Optional<KeyedPoint> anchor;
    ROCKS_PROPERTY() KeyedPoint origin;
    ROCKS_PROPERTY() InstancedStruct<> boxed;
    ROCKS_PROPERTY(Transient) int32 cache = 0;
};

// 필드 이름만 다른 두 struct다(이름이 키에 들어가는지 — 같은 값이어도 키가 다르다).
ROCKS_STRUCT()
struct KeyedAlpha {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 alpha = 0;
};

ROCKS_STRUCT()
struct KeyedBeta {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 beta = 0;
};

// 키에 쓸 수 없는 값(Text — 지역화 원문이 바뀔 수 있다)과 클래스 참조(키에는 TypeId로 쓴다)를 담은
// struct다(거부·ClassRef 시험).
ROCKS_STRUCT()
struct KeyedUnsupported {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 plain = 0;
    ROCKS_PROPERTY() ClassRef<Asset> kind;
    ROCKS_PROPERTY() Text caption;
};

} // namespace rocks::asset_fixture
