#pragma once

#include <AssetRuntime/BulkData.h>
#include <AssetRuntime/Package.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/OrderedSet.h>
#include <Core/Math/InterpCurve.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/Transform.h>
#include <Core/Optional.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <Curves/TangentCurve.h>
#include <Localization/Text.h>
#include <Meta/Annotations.h>
#include <Object/InstancedStruct.h>
#include <Object/ObjectRefArray.h>
#include <Object/ObjectRefs.h>
#include <Object/SoftObjectRef.h>

// 값 종류별 시험 에셋이다(Docs/Design/AssetSystem 14.1절 fixture 표). 단순(스칼라) → 값
// 묶음(struct· 컨테이너·Core 값 타입·커브·Text·InstancedStruct) → 큰 데이터(bulk) → 상속 → 검증
// hook → 복합(게임 데이터) 순서다. 값은 시험의 결정적 생성기(Tests/AssetFixtureValues.h)가 seed로
// 채우고 같은 seed의 기대값과 필드 단위로 비교한다.
namespace rocks::asset_fixture {

ROCKS_ENUM()
enum class FixtureKind : uint8 {
    None = 0,
    Small = 1,
    Large = 7
};

// 단순: 스칼라 필드(정수 폭 셋·실수 둘·bool·Name·String·enum·Guid).
ROCKS_CLASS()
struct SimpleAsset final : Asset {
    ROCKS_GENERATED_BODY()

    explicit SimpleAsset(Allocator& allocator) noexcept : label(allocator) {
    }

    ROCKS_PROPERTY() int32 count = 0;
    ROCKS_PROPERTY() int8 tiny = 0;
    ROCKS_PROPERTY() uint64 big = 0;
    ROCKS_PROPERTY() float ratio = 0.0f;
    ROCKS_PROPERTY() double precise = 0.0;
    ROCKS_PROPERTY() bool enabled = false;
    ROCKS_PROPERTY() Name tag;
    ROCKS_PROPERTY() String label;
    ROCKS_PROPERTY() FixtureKind kind = FixtureKind::None;
    ROCKS_PROPERTY() Guid key {};
};

// 값 묶음의 struct 원소다(Optional 멤버 포함).
ROCKS_STRUCT()
struct FixturePoint {
    ROCKS_GENERATED_BODY()

    explicit FixturePoint(Allocator& allocator) noexcept : label(allocator) {
    }

    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() int32 y = 0;
    ROCKS_PROPERTY() String label;
    ROCKS_PROPERTY() Optional<uint32> weight;
};

// 중첩 struct다(struct 안 struct와 struct 배열).
ROCKS_STRUCT()
struct FixtureGroup {
    ROCKS_GENERATED_BODY()

    explicit FixtureGroup(Allocator& allocator) noexcept : point(allocator), points(allocator) {
    }

    ROCKS_PROPERTY() FixturePoint point;
    ROCKS_PROPERTY() Array<FixturePoint> points;
};

// InstancedStruct에 담는 참조 없는 struct다.
ROCKS_STRUCT()
struct FixturePayload {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() float factor = 1.0f;
    ROCKS_PROPERTY() int32 steps = 0;
};

// 값 묶음: 중첩 struct·정적
// 배열·Array·Map·Set·Optional·Text·Bytes·InstancedStruct·커브(TangentCurve· InterpCurve)·UE 단위 값
// 타입(Transform·Vector·Rotator — 위치 cm, 각도 도).
ROCKS_CLASS()
struct ValueAsset final : Asset {
    ROCKS_GENERATED_BODY()

    explicit ValueAsset(Allocator& allocator) noexcept
        : group(allocator), numbers(allocator), names(allocator), words(allocator),
          bytes(allocator), curve(allocator), track(allocator) {
    }

    ROCKS_PROPERTY() FixtureGroup group;
    ROCKS_PROPERTY() int32 fixed[4] = {};
    ROCKS_PROPERTY() Array<int32> numbers;
    ROCKS_PROPERTY() OrderedMap<uint32, String> names;
    ROCKS_PROPERTY() OrderedSet<String> words;
    ROCKS_PROPERTY() Optional<String> maybe;
    ROCKS_PROPERTY() Optional<FixturePoint> maybePoint;
    ROCKS_PROPERTY() Text title;
    ROCKS_PROPERTY(MaxElements = 1048576) Array<uint8> bytes;
    ROCKS_PROPERTY() InstancedStruct<> any;
    ROCKS_PROPERTY() TangentCurve curve;
    ROCKS_PROPERTY() InterpCurve<float> track;
    ROCKS_PROPERTY() Transform placement;
    ROCKS_PROPERTY() Vec3 position;
    ROCKS_PROPERTY() Rotator<double> heading;
};

// 큰 데이터: 수 MB bulk(압축되는 무늬·압축되지 않는 무작위)·0 byte bulk·bulk 배열.
ROCKS_CLASS()
struct BulkAsset final : Asset {
    ROCKS_GENERATED_BODY()

    explicit BulkAsset(Allocator& allocator) noexcept : chunks(allocator) {
    }

    ROCKS_PROPERTY() BulkDataRef compressible;
    ROCKS_PROPERTY() BulkDataRef incompressible;
    ROCKS_PROPERTY() BulkDataRef empty;
    ROCKS_PROPERTY() Array<BulkDataRef> chunks;
};

// 상속: 기반 에셋 클래스(파생 가능)와 파생 클래스. 기반 필드가 파생 객체에서 왕복되는지 본다.
ROCKS_CLASS()
struct BaseGameAsset : Asset {
    ROCKS_GENERATED_BODY()

    explicit BaseGameAsset(Allocator& allocator) noexcept : baseName(allocator) {
    }

    ROCKS_PROPERTY() int32 baseValue = 0;
    ROCKS_PROPERTY() String baseName;
};

ROCKS_CLASS()
struct DerivedGameAsset final : BaseGameAsset {
    ROCKS_GENERATED_BODY()

    explicit DerivedGameAsset(Allocator& allocator) noexcept
        : BaseGameAsset(allocator), extras(allocator) {
    }

    ROCKS_PROPERTY() float extra = 0.0f;
    ROCKS_PROPERTY() Array<float> extras;
};

// 검증 hook: low ≤ high가 아니면 로드(그래프 1단계)와 쓰기가 거부한다.
ROCKS_STRUCT()
struct FixtureRange {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 low = 0;
    ROCKS_PROPERTY() int32 high = 0;

    static Result<void> Validate(const FixtureRange& value) noexcept {
        return value.low <= value.high ? Result<void>::Success()
                                       : Result<void>::Failure(ErrorCode::InvalidArgument);
    }
};

ROCKS_CLASS()
struct ValidatedAsset final : Asset {
    ROCKS_GENERATED_BODY()

    explicit ValidatedAsset(Allocator& allocator) noexcept : ranges(allocator) {
    }

    ROCKS_PROPERTY() FixtureRange range;
    ROCKS_PROPERTY() Array<FixtureRange> ranges;
};

// 게임 데이터의 한 줄(struct 원소 — 이름·수치·Soft 참조).
ROCKS_STRUCT()
struct FixtureEntry {
    ROCKS_GENERATED_BODY()

    explicit FixtureEntry(Allocator& allocator) noexcept : id(allocator), icon(allocator) {
    }

    ROCKS_PROPERTY() String id;
    ROCKS_PROPERTY() int32 cost = 0;
    ROCKS_PROPERTY() float weight = 0.0f;
    ROCKS_PROPERTY() SoftObjectRef<Asset> icon;
};

// 게임 데이터 에셋의 하위 객체다(대표 에셋 아래 — 다른 하위 객체·다른 패키지 에셋 참조, 커브).
ROCKS_CLASS()
struct GameComponent final : Object {
    ROCKS_GENERATED_BODY()

    explicit GameComponent(Allocator& allocator) noexcept : response(allocator) {
    }

    ROCKS_PROPERTY() int32 slot = 0;
    ROCKS_PROPERTY() ObjectRef<GameComponent> next;
    ROCKS_PROPERTY() ObjectRef<Asset> material;
    ROCKS_PROPERTY() TangentCurve response;
};

// 복합(게임 데이터 같은 에셋): 여러 하위 객체·다른 에셋 Hard 참조 배열·Soft 참조·weak
// 참조·커브·bulk· struct 배열·Map을 함께 가진다.
ROCKS_CLASS()
struct GameDataAsset final : Asset {
    ROCKS_GENERATED_BODY()

    explicit GameDataAsset(Allocator& allocator) noexcept
        : title(allocator), dependencies(allocator), fallback(allocator), entries(allocator),
          byLevel(allocator), balance(allocator), components(allocator) {
    }

    ROCKS_PROPERTY() String title;
    ROCKS_PROPERTY() ObjectRefArray<Asset> dependencies;
    ROCKS_PROPERTY() SoftObjectRef<Asset> fallback;
    ROCKS_PROPERTY() WeakObjectRef<GameComponent> focus;
    ROCKS_PROPERTY() Array<FixtureEntry> entries;
    ROCKS_PROPERTY() OrderedMap<uint32, float> byLevel;
    ROCKS_PROPERTY() TangentCurve balance;
    ROCKS_PROPERTY() BulkDataRef table;
    ROCKS_PROPERTY() ObjectRefArray<GameComponent> components;
    ROCKS_PROPERTY() FixtureRange window;
};

} // namespace rocks::asset_fixture
