#pragma once

#include <AssetRuntime/BulkData.h>
#include <AssetRuntime/Package.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/OrderedSet.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Optional.h>
#include <Core/Result.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <Localization/Text.h>
#include <Meta/Annotations.h>
#include <Object/AssetId.h>
#include <Object/ClassRef.h>
#include <Object/InstancedStruct.h>
#include <Object/ObjectRefArray.h>
#include <Object/ObjectRefMap.h>
#include <Object/ObjectRefs.h>
#include <Object/SoftObjectRef.h>
#include <Object/StructRefArray.h>

// 리플렉션 성질 표의 "지원하는 모든 조합" fixture다(Docs/Design/AssetSystem.ko.md 14.1절,
// ReflectionExtensions.ko.md 1.3절). 값 종류마다 struct 하나(ComboXxx)가 그 종류를 표가 허용하는 모든
// 자리에 둔다: value(struct 멤버), array(Array 원소), fixed(정적 배열 원소), keyed(Map 키),
// mapped(Map 값), set(Set 원소), maybe(Optional 내부). 표가 막는 자리의 멤버는 두지 않는다. 클래스
// 필드 자리는 ComboAsset의 f* 필드가, EditorOnly 실효 플래그는 같은 모양의 ComboEditorAsset이 덮는다.
// 멤버 이름이 종류마다 같아서 시험의 생성기·비교기(Tests/AssetCombination.h)가 템플릿 하나로 모든
// 종류를 채우고 비교한다. 빠진 칸은 FixtureAsset_CombinationsCoverPropertyTable이 성질 표를 순회해
// 잡는다.
//
// 표 밖 축:
//   - 참조 종류: ObjectRef·WeakObjectRef 값은 null·같은 패키지(대표·하위·깊은 하위)·다른 패키지
//     (대표·하위·깊은 하위)를 돌아가며 가리킨다(시험 생성기의 대상 표).
//   - 지정자: Required(ComboRequiredAsset — Required를 받는 종류 × 그 종류의 모든 자리), bitfield
//     (ComboBits·ComboAsset::fBit), Validate hook(ComboChecked — 필드·멤버·Array·정적 배열·Map 값·
//     Optional·InstancedStruct 내용).
//   - 참조 struct(ComboRefLink — 필드·멤버·StructRefArray·정적 배열)와 Registry 컨테이너 struct
//     (ComboObjectRefs).
//   - 중첩 깊이: ComboTop → ComboMiddle → ComboLeaf(컨테이너 안 struct 안 컨테이너).
//   - 클래스 구조: 하위 객체(ComboPart·ComboPartDerived — Asset이 아닌 클래스, 상속), 다른 패키지
//     참조.
namespace rocks::asset_fixture {

ROCKS_ENUM()
enum class ComboTone : uint8 {
    None = 0,
    Low = 1,
    Mid = 2,
    High = 3
};

ROCKS_ENUM(Flags)
enum class ComboMask : uint16 {
    Read = 1,
    Write = 2,
    Exec = 4
};

// 값 struct 종류의 원소다(검증 hook 없음).
ROCKS_STRUCT()
struct ComboPoint {
    explicit ComboPoint(Allocator& allocator) noexcept : label(allocator) {
    }

    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() String label;
};

// 검증 hook struct다(low ≤ high가 아니면 쓰기·로드가 거부한다).
ROCKS_STRUCT()
struct ComboChecked {
    ROCKS_PROPERTY() int32 low = 0;
    ROCKS_PROPERTY() int32 high = 0;

    static Result<void> Validate(const ComboChecked& value) noexcept {
        return value.low <= value.high ? Result<void>::Success()
                                       : Result<void>::Failure(ErrorCode::InvalidArgument);
    }
};

// InstancedStruct에 담는 hook 없는 struct다.
ROCKS_STRUCT()
struct ComboPayload {
    ROCKS_PROPERTY() float factor = 0.0f;
    ROCKS_PROPERTY() int64 steps = 0;
};

// bitfield bool 멤버다(부호 없는 정수 폭 셋, 같은 byte를 나누는 일반 멤버).
ROCKS_STRUCT()
struct ComboBits {
    ROCKS_PROPERTY() uint8 first : 1 = 0;
    ROCKS_PROPERTY() uint8 second : 1 = 0;
    ROCKS_PROPERTY() uint16 wide : 1 = 0;
    ROCKS_PROPERTY() uint32 wider : 1 = 0;
    ROCKS_PROPERTY() uint8 plain = 0;
};

// ---- 종류별 조합 struct(값 struct) ----

ROCKS_STRUCT()
struct ComboBool {
    explicit ComboBool(Allocator& allocator) noexcept
        : array(allocator), keyed(allocator), mapped(allocator), set(allocator) {
    }

    ROCKS_PROPERTY() bool value = false;
    ROCKS_PROPERTY() Array<bool> array;
    ROCKS_PROPERTY() bool fixed[2] = {};
    ROCKS_PROPERTY() OrderedMap<bool, int32> keyed;
    ROCKS_PROPERTY() OrderedMap<int32, bool> mapped;
    ROCKS_PROPERTY() OrderedSet<bool> set;
    ROCKS_PROPERTY() Optional<bool> maybe;
};

ROCKS_STRUCT()
struct ComboUInt8 {
    explicit ComboUInt8(Allocator& allocator) noexcept
        : array(allocator), keyed(allocator), mapped(allocator), set(allocator) {
    }

    ROCKS_PROPERTY() uint8 value = 0;
    ROCKS_PROPERTY() Array<uint8> array;
    ROCKS_PROPERTY() uint8 fixed[2] = {};
    ROCKS_PROPERTY() OrderedMap<uint8, int32> keyed;
    ROCKS_PROPERTY() OrderedMap<int32, uint8> mapped;
    ROCKS_PROPERTY() OrderedSet<uint8> set;
    ROCKS_PROPERTY() Optional<uint8> maybe;
};

ROCKS_STRUCT()
struct ComboUInt16 {
    explicit ComboUInt16(Allocator& allocator) noexcept
        : array(allocator), keyed(allocator), mapped(allocator), set(allocator) {
    }

    ROCKS_PROPERTY() uint16 value = 0;
    ROCKS_PROPERTY() Array<uint16> array;
    ROCKS_PROPERTY() uint16 fixed[2] = {};
    ROCKS_PROPERTY() OrderedMap<uint16, int32> keyed;
    ROCKS_PROPERTY() OrderedMap<int32, uint16> mapped;
    ROCKS_PROPERTY() OrderedSet<uint16> set;
    ROCKS_PROPERTY() Optional<uint16> maybe;
};

ROCKS_STRUCT()
struct ComboUInt32 {
    explicit ComboUInt32(Allocator& allocator) noexcept
        : array(allocator), keyed(allocator), mapped(allocator), set(allocator) {
    }

    ROCKS_PROPERTY() uint32 value = 0;
    ROCKS_PROPERTY() Array<uint32> array;
    ROCKS_PROPERTY() uint32 fixed[2] = {};
    ROCKS_PROPERTY() OrderedMap<uint32, int32> keyed;
    ROCKS_PROPERTY() OrderedMap<int32, uint32> mapped;
    ROCKS_PROPERTY() OrderedSet<uint32> set;
    ROCKS_PROPERTY() Optional<uint32> maybe;
};

ROCKS_STRUCT()
struct ComboUInt64 {
    explicit ComboUInt64(Allocator& allocator) noexcept
        : array(allocator), keyed(allocator), mapped(allocator), set(allocator) {
    }

    ROCKS_PROPERTY() uint64 value = 0;
    ROCKS_PROPERTY() Array<uint64> array;
    ROCKS_PROPERTY() uint64 fixed[2] = {};
    ROCKS_PROPERTY() OrderedMap<uint64, int32> keyed;
    ROCKS_PROPERTY() OrderedMap<int32, uint64> mapped;
    ROCKS_PROPERTY() OrderedSet<uint64> set;
    ROCKS_PROPERTY() Optional<uint64> maybe;
};

ROCKS_STRUCT()
struct ComboInt8 {
    explicit ComboInt8(Allocator& allocator) noexcept
        : array(allocator), keyed(allocator), mapped(allocator), set(allocator) {
    }

    ROCKS_PROPERTY() int8 value = 0;
    ROCKS_PROPERTY() Array<int8> array;
    ROCKS_PROPERTY() int8 fixed[2] = {};
    ROCKS_PROPERTY() OrderedMap<int8, int32> keyed;
    ROCKS_PROPERTY() OrderedMap<int32, int8> mapped;
    ROCKS_PROPERTY() OrderedSet<int8> set;
    ROCKS_PROPERTY() Optional<int8> maybe;
};

ROCKS_STRUCT()
struct ComboInt16 {
    explicit ComboInt16(Allocator& allocator) noexcept
        : array(allocator), keyed(allocator), mapped(allocator), set(allocator) {
    }

    ROCKS_PROPERTY() int16 value = 0;
    ROCKS_PROPERTY() Array<int16> array;
    ROCKS_PROPERTY() int16 fixed[2] = {};
    ROCKS_PROPERTY() OrderedMap<int16, int32> keyed;
    ROCKS_PROPERTY() OrderedMap<int32, int16> mapped;
    ROCKS_PROPERTY() OrderedSet<int16> set;
    ROCKS_PROPERTY() Optional<int16> maybe;
};

ROCKS_STRUCT()
struct ComboInt32 {
    explicit ComboInt32(Allocator& allocator) noexcept
        : array(allocator), keyed(allocator), mapped(allocator), set(allocator) {
    }

    ROCKS_PROPERTY() int32 value = 0;
    ROCKS_PROPERTY() Array<int32> array;
    ROCKS_PROPERTY() int32 fixed[2] = {};
    ROCKS_PROPERTY() OrderedMap<int32, int32> keyed;
    ROCKS_PROPERTY() OrderedMap<int32, int32> mapped;
    ROCKS_PROPERTY() OrderedSet<int32> set;
    ROCKS_PROPERTY() Optional<int32> maybe;
};

ROCKS_STRUCT()
struct ComboInt64 {
    explicit ComboInt64(Allocator& allocator) noexcept
        : array(allocator), keyed(allocator), mapped(allocator), set(allocator) {
    }

    ROCKS_PROPERTY() int64 value = 0;
    ROCKS_PROPERTY() Array<int64> array;
    ROCKS_PROPERTY() int64 fixed[2] = {};
    ROCKS_PROPERTY() OrderedMap<int64, int32> keyed;
    ROCKS_PROPERTY() OrderedMap<int32, int64> mapped;
    ROCKS_PROPERTY() OrderedSet<int64> set;
    ROCKS_PROPERTY() Optional<int64> maybe;
};

// 실수는 Map 키·Set 원소가 아니다(성질 표).
ROCKS_STRUCT()
struct ComboFloat {
    explicit ComboFloat(Allocator& allocator) noexcept : array(allocator), mapped(allocator) {
    }

    ROCKS_PROPERTY() float value = 0.0f;
    ROCKS_PROPERTY() Array<float> array;
    ROCKS_PROPERTY() float fixed[2] = {};
    ROCKS_PROPERTY() OrderedMap<int32, float> mapped;
    ROCKS_PROPERTY() Optional<float> maybe;
};

ROCKS_STRUCT()
struct ComboDouble {
    explicit ComboDouble(Allocator& allocator) noexcept : array(allocator), mapped(allocator) {
    }

    ROCKS_PROPERTY() double value = 0.0;
    ROCKS_PROPERTY() Array<double> array;
    ROCKS_PROPERTY() double fixed[2] = {};
    ROCKS_PROPERTY() OrderedMap<int32, double> mapped;
    ROCKS_PROPERTY() Optional<double> maybe;
};

ROCKS_STRUCT()
struct ComboGuid {
    explicit ComboGuid(Allocator& allocator) noexcept
        : array(allocator), keyed(allocator), mapped(allocator), set(allocator) {
    }

    ROCKS_PROPERTY() Guid value {};
    ROCKS_PROPERTY() Array<Guid> array;
    ROCKS_PROPERTY() Guid fixed[2] = {};
    ROCKS_PROPERTY() OrderedMap<Guid, int32> keyed;
    ROCKS_PROPERTY() OrderedMap<int32, Guid> mapped;
    ROCKS_PROPERTY() OrderedSet<Guid> set;
    ROCKS_PROPERTY() Optional<Guid> maybe;
};

ROCKS_STRUCT()
struct ComboString {
    explicit ComboString(Allocator& allocator) noexcept
        : value(allocator), array(allocator), fixed{String(allocator), String(allocator)},
          keyed(allocator), mapped(allocator), set(allocator) {
    }

    ROCKS_PROPERTY() String value;
    ROCKS_PROPERTY() Array<String> array;
    ROCKS_PROPERTY() String fixed[2];
    ROCKS_PROPERTY() OrderedMap<String, int32> keyed;
    ROCKS_PROPERTY() OrderedMap<int32, String> mapped;
    ROCKS_PROPERTY() OrderedSet<String> set;
    ROCKS_PROPERTY() Optional<String> maybe;
};

ROCKS_STRUCT()
struct ComboName {
    explicit ComboName(Allocator& allocator) noexcept
        : array(allocator), keyed(allocator), mapped(allocator), set(allocator) {
    }

    ROCKS_PROPERTY() Name value;
    ROCKS_PROPERTY() Array<Name> array;
    ROCKS_PROPERTY() Name fixed[2];
    ROCKS_PROPERTY() OrderedMap<Name, int32> keyed;
    ROCKS_PROPERTY() OrderedMap<int32, Name> mapped;
    ROCKS_PROPERTY() OrderedSet<Name> set;
    ROCKS_PROPERTY() Optional<Name> maybe;
};

ROCKS_STRUCT()
struct ComboAssetId {
    explicit ComboAssetId(Allocator& allocator) noexcept
        : array(allocator), keyed(allocator), mapped(allocator), set(allocator) {
    }

    ROCKS_PROPERTY() AssetId value {};
    ROCKS_PROPERTY() Array<AssetId> array;
    ROCKS_PROPERTY() AssetId fixed[2] = {};
    ROCKS_PROPERTY() OrderedMap<AssetId, int32> keyed;
    ROCKS_PROPERTY() OrderedMap<int32, AssetId> mapped;
    ROCKS_PROPERTY() OrderedSet<AssetId> set;
    ROCKS_PROPERTY() Optional<AssetId> maybe;
};

ROCKS_STRUCT()
struct ComboDigest {
    explicit ComboDigest(Allocator& allocator) noexcept
        : array(allocator), keyed(allocator), mapped(allocator), set(allocator) {
    }

    ROCKS_PROPERTY() Sha256Digest value {};
    ROCKS_PROPERTY() Array<Sha256Digest> array;
    ROCKS_PROPERTY() Sha256Digest fixed[2] = {};
    ROCKS_PROPERTY() OrderedMap<Sha256Digest, int32> keyed;
    ROCKS_PROPERTY() OrderedMap<int32, Sha256Digest> mapped;
    ROCKS_PROPERTY() OrderedSet<Sha256Digest> set;
    ROCKS_PROPERTY() Optional<Sha256Digest> maybe;
};

ROCKS_STRUCT()
struct ComboEnum {
    explicit ComboEnum(Allocator& allocator) noexcept
        : array(allocator), keyed(allocator), mapped(allocator), set(allocator) {
    }

    ROCKS_PROPERTY() ComboTone value = ComboTone::None;
    ROCKS_PROPERTY() Array<ComboTone> array;
    ROCKS_PROPERTY() ComboTone fixed[2] = {ComboTone::None, ComboTone::None};
    ROCKS_PROPERTY() OrderedMap<ComboTone, int32> keyed;
    ROCKS_PROPERTY() OrderedMap<int32, ComboTone> mapped;
    ROCKS_PROPERTY() OrderedSet<ComboTone> set;
    ROCKS_PROPERTY() Optional<ComboTone> maybe;
};

// Flags enum(값 0은 빈 조합)이다.
ROCKS_STRUCT()
struct ComboFlags {
    explicit ComboFlags(Allocator& allocator) noexcept
        : array(allocator), keyed(allocator), mapped(allocator), set(allocator) {
    }

    ROCKS_PROPERTY() ComboMask value {};
    ROCKS_PROPERTY() Array<ComboMask> array;
    ROCKS_PROPERTY() ComboMask fixed[2] = {};
    ROCKS_PROPERTY() OrderedMap<ComboMask, int32> keyed;
    ROCKS_PROPERTY() OrderedMap<int32, ComboMask> mapped;
    ROCKS_PROPERTY() OrderedSet<ComboMask> set;
    ROCKS_PROPERTY() Optional<ComboMask> maybe;
};

// ClassRef는 Optional 내부가 아니다(null이 이미 "없음").
ROCKS_STRUCT()
struct ComboClassRef {
    explicit ComboClassRef(Allocator& allocator) noexcept
        : array(allocator), keyed(allocator), mapped(allocator), set(allocator) {
    }

    ROCKS_PROPERTY() ClassRef<Asset> value;
    ROCKS_PROPERTY() Array<ClassRef<Asset>> array;
    ROCKS_PROPERTY() ClassRef<Asset> fixed[2];
    ROCKS_PROPERTY() OrderedMap<ClassRef<Asset>, int32> keyed;
    ROCKS_PROPERTY() OrderedMap<int32, ClassRef<Asset>> mapped;
    ROCKS_PROPERTY() OrderedSet<ClassRef<Asset>> set;
};

// Soft는 Optional 내부가 아니다.
ROCKS_STRUCT()
struct ComboSoft {
    explicit ComboSoft(Allocator& allocator) noexcept
        : value(allocator), array(allocator),
          fixed{SoftObjectRef<Object>(allocator), SoftObjectRef<Object>(allocator)},
          keyed(allocator), mapped(allocator), set(allocator) {
    }

    ROCKS_PROPERTY() SoftObjectRef<Object> value;
    ROCKS_PROPERTY() Array<SoftObjectRef<Object>> array;
    ROCKS_PROPERTY() SoftObjectRef<Object> fixed[2];
    ROCKS_PROPERTY() OrderedMap<SoftObjectRef<Object>, int32> keyed;
    ROCKS_PROPERTY() OrderedMap<int32, SoftObjectRef<Object>> mapped;
    ROCKS_PROPERTY() OrderedSet<SoftObjectRef<Object>> set;
};

// WeakObjectRef는 키·Set 원소·Optional 내부가 아니다.
ROCKS_STRUCT()
struct ComboWeak {
    explicit ComboWeak(Allocator& allocator) noexcept : array(allocator), mapped(allocator) {
    }

    ROCKS_PROPERTY() WeakObjectRef<Object> value;
    ROCKS_PROPERTY() Array<WeakObjectRef<Object>> array;
    ROCKS_PROPERTY() WeakObjectRef<Object> fixed[2];
    ROCKS_PROPERTY() OrderedMap<int32, WeakObjectRef<Object>> mapped;
};

// 값 struct는 키·Set 원소가 아니다.
ROCKS_STRUCT()
struct ComboStruct {
    explicit ComboStruct(Allocator& allocator) noexcept
        : value(allocator), array(allocator), fixed{ComboPoint(allocator), ComboPoint(allocator)},
          mapped(allocator) {
    }

    ROCKS_PROPERTY() ComboPoint value;
    ROCKS_PROPERTY() Array<ComboPoint> array;
    ROCKS_PROPERTY() ComboPoint fixed[2];
    ROCKS_PROPERTY() OrderedMap<int32, ComboPoint> mapped;
    ROCKS_PROPERTY() Optional<ComboPoint> maybe;
};

// InstancedStruct는 키·Set 원소·Optional 내부가 아니다. 내용은 ComboPayload·ComboChecked(hook)다.
ROCKS_STRUCT()
struct ComboInstanced {
    explicit ComboInstanced(Allocator& allocator) noexcept : array(allocator), mapped(allocator) {
    }

    ROCKS_PROPERTY() InstancedStruct<> value;
    ROCKS_PROPERTY() Array<InstancedStruct<>> array;
    ROCKS_PROPERTY() InstancedStruct<> fixed[2];
    ROCKS_PROPERTY() OrderedMap<int32, InstancedStruct<>> mapped;
};

// Text는 키·Set 원소·Optional 내부가 아니다(비어 있음·Invariant·Localized 형태).
ROCKS_STRUCT()
struct ComboText {
    explicit ComboText(Allocator& allocator) noexcept : array(allocator), mapped(allocator) {
    }

    ROCKS_PROPERTY() Text value;
    ROCKS_PROPERTY() Array<Text> array;
    ROCKS_PROPERTY() Text fixed[2];
    ROCKS_PROPERTY() OrderedMap<int32, Text> mapped;
};

// 검증 hook struct를 hook이 도는 모든 자리(멤버·Array·정적 배열·Map 값·Optional)에 둔다.
// InstancedStruct 내용 자리는 ComboInstanced가 덮는다.
ROCKS_STRUCT()
struct ComboCheckedSlots {
    explicit ComboCheckedSlots(Allocator& allocator) noexcept
        : array(allocator), mapped(allocator) {
    }

    ROCKS_PROPERTY() ComboChecked value;
    ROCKS_PROPERTY() Array<ComboChecked> array;
    ROCKS_PROPERTY() ComboChecked fixed[2];
    ROCKS_PROPERTY() OrderedMap<int32, ComboChecked> mapped;
    ROCKS_PROPERTY() Optional<ComboChecked> maybe;
};

// ---- 중첩 깊이(컨테이너 안 struct 안 컨테이너, 깊이 3) ----

ROCKS_STRUCT()
struct ComboLeaf {
    explicit ComboLeaf(Allocator& allocator) noexcept : values(allocator), tags(allocator) {
    }

    ROCKS_PROPERTY() int32 depth = 0;
    ROCKS_PROPERTY() Array<int32> values;
    ROCKS_PROPERTY() OrderedSet<Name> tags;
};

ROCKS_STRUCT()
struct ComboMiddle {
    explicit ComboMiddle(Allocator& allocator) noexcept
        : leaf(allocator), leaves(allocator), byKey(allocator) {
    }

    ROCKS_PROPERTY() ComboLeaf leaf;
    ROCKS_PROPERTY() Array<ComboLeaf> leaves;
    ROCKS_PROPERTY() OrderedMap<uint32, ComboLeaf> byKey;
    ROCKS_PROPERTY() Optional<ComboLeaf> maybe;
};

ROCKS_STRUCT()
struct ComboTop {
    explicit ComboTop(Allocator& allocator) noexcept : middle(allocator), middles(allocator) {
    }

    ROCKS_PROPERTY() ComboMiddle middle;
    ROCKS_PROPERTY() Array<ComboMiddle> middles;
};

// 모든 종류별 조합 struct를 멤버로 담는 값 struct다(클래스가 이것을 필드 하나로 담는다 — 조합
// struct가 struct 멤버 자리에도, 중첩 struct 안에도 놓인다).
ROCKS_STRUCT()
struct ComboValues {
    explicit ComboValues(Allocator& allocator) noexcept
        : cBool(allocator), cUInt8(allocator), cUInt16(allocator), cUInt32(allocator),
          cUInt64(allocator), cInt8(allocator), cInt16(allocator), cInt32(allocator),
          cInt64(allocator), cFloat(allocator), cDouble(allocator), cGuid(allocator),
          cString(allocator), cName(allocator), cAssetId(allocator), cDigest(allocator),
          cEnum(allocator), cFlags(allocator), cClassRef(allocator), cSoft(allocator),
          cWeak(allocator), cStruct(allocator), cInstanced(allocator), cText(allocator),
          cChecked(allocator), top(allocator) {
    }

    ROCKS_PROPERTY() ComboBool cBool;
    ROCKS_PROPERTY() ComboUInt8 cUInt8;
    ROCKS_PROPERTY() ComboUInt16 cUInt16;
    ROCKS_PROPERTY() ComboUInt32 cUInt32;
    ROCKS_PROPERTY() ComboUInt64 cUInt64;
    ROCKS_PROPERTY() ComboInt8 cInt8;
    ROCKS_PROPERTY() ComboInt16 cInt16;
    ROCKS_PROPERTY() ComboInt32 cInt32;
    ROCKS_PROPERTY() ComboInt64 cInt64;
    ROCKS_PROPERTY() ComboFloat cFloat;
    ROCKS_PROPERTY() ComboDouble cDouble;
    ROCKS_PROPERTY() ComboGuid cGuid;
    ROCKS_PROPERTY() ComboString cString;
    ROCKS_PROPERTY() ComboName cName;
    ROCKS_PROPERTY() ComboAssetId cAssetId;
    ROCKS_PROPERTY() ComboDigest cDigest;
    ROCKS_PROPERTY() ComboEnum cEnum;
    ROCKS_PROPERTY() ComboFlags cFlags;
    ROCKS_PROPERTY() ComboClassRef cClassRef;
    ROCKS_PROPERTY() ComboSoft cSoft;
    ROCKS_PROPERTY() ComboWeak cWeak;
    ROCKS_PROPERTY() ComboStruct cStruct;
    ROCKS_PROPERTY() ComboInstanced cInstanced;
    ROCKS_PROPERTY() ComboText cText;
    ROCKS_PROPERTY() ComboCheckedSlots cChecked;
    ROCKS_PROPERTY() ComboBits bits;
    ROCKS_PROPERTY() ComboTop top;
};

// ---- 참조 struct ----

// 강한 참조 하나를 담은 참조 struct다(필드·멤버·StructRefArray 원소·정적 배열 원소).
ROCKS_STRUCT()
struct ComboRefLink {
    ROCKS_PROPERTY() ObjectRef<Object> target;
    ROCKS_PROPERTY() int32 weight = 0;
};

// ObjectRef 종류의 모든 자리다(값 = struct 멤버, Array 원소 = ObjectRefArray, 정적 배열 원소, Map
// 값 = ObjectRefMap). Registry 컨테이너를 담으므로 클래스 필드·struct 멤버로만 놓인다.
ROCKS_STRUCT()
struct ComboObjectRefs {
    explicit ComboObjectRefs(Allocator& allocator) noexcept
        : array(allocator), mapped(allocator), links(allocator) {
    }

    ROCKS_PROPERTY() ObjectRef<Object> value;
    ROCKS_PROPERTY() ObjectRefArray<Object> array;
    ROCKS_PROPERTY() ObjectRef<Object> fixed[2];
    ROCKS_PROPERTY() ObjectRefMap<int32, Object> mapped;
    ROCKS_PROPERTY() ComboRefLink link;
    ROCKS_PROPERTY() StructRefArray<ComboRefLink> links;
    ROCKS_PROPERTY() ComboRefLink linkFixed[2];
};

// ---- 에셋 클래스 ----

// 모든 종류의 클래스 필드 자리(f*), 클래스 필드 자리의 컨테이너 다섯(Array·정적 배열·Map·Set·
// Optional), 조합 struct 묶음(values), ObjectRef 자리 묶음(refs), bulk, 클래스 bitfield를 담는다.
ROCKS_CLASS()
struct ComboAsset final : Asset {
    explicit ComboAsset(Allocator& allocator) noexcept
        : fString(allocator), fSoft(allocator), fStruct(allocator), fArray(allocator),
          fMap(allocator), fSet(allocator), fRefArray(allocator), fRefMap(allocator),
          fLinks(allocator), values(allocator), refs(allocator) {
    }

    ROCKS_PROPERTY() bool fBool = false;
    ROCKS_PROPERTY() uint8 fUInt8 = 0;
    ROCKS_PROPERTY() uint16 fUInt16 = 0;
    ROCKS_PROPERTY() uint32 fUInt32 = 0;
    ROCKS_PROPERTY() uint64 fUInt64 = 0;
    ROCKS_PROPERTY() int8 fInt8 = 0;
    ROCKS_PROPERTY() int16 fInt16 = 0;
    ROCKS_PROPERTY() int32 fInt32 = 0;
    ROCKS_PROPERTY() int64 fInt64 = 0;
    ROCKS_PROPERTY() float fFloat = 0.0f;
    ROCKS_PROPERTY() double fDouble = 0.0;
    ROCKS_PROPERTY() Guid fGuid {};
    ROCKS_PROPERTY() String fString;
    ROCKS_PROPERTY() Name fName;
    ROCKS_PROPERTY() AssetId fAssetId {};
    ROCKS_PROPERTY() Sha256Digest fDigest {};
    ROCKS_PROPERTY() ComboTone fEnum = ComboTone::None;
    ROCKS_PROPERTY() ComboMask fFlags {};
    ROCKS_PROPERTY() ClassRef<Asset> fClassRef;
    ROCKS_PROPERTY() SoftObjectRef<Object> fSoft;
    ROCKS_PROPERTY() WeakObjectRef<Object> fWeak;
    ROCKS_PROPERTY() ObjectRef<Object> fObjectRef;
    ROCKS_PROPERTY() ComboPoint fStruct;
    ROCKS_PROPERTY() InstancedStruct<> fInstanced;
    ROCKS_PROPERTY() Text fText;
    ROCKS_PROPERTY() ComboChecked fChecked;
    ROCKS_PROPERTY() BulkDataRef fBulk;
    ROCKS_PROPERTY() uint8 fBit : 1 = 0;
    ROCKS_PROPERTY() uint8 fOtherBit : 1 = 0;
    ROCKS_PROPERTY() Array<int32> fArray;
    ROCKS_PROPERTY() int32 fFixed[2] = {};
    ROCKS_PROPERTY() OrderedMap<int32, int32> fMap;
    ROCKS_PROPERTY() OrderedSet<int32> fSet;
    ROCKS_PROPERTY() Optional<int32> fMaybe;
    ROCKS_PROPERTY() ObjectRefArray<Object> fRefArray;
    ROCKS_PROPERTY() ObjectRef<Object> fRefFixed[2];
    ROCKS_PROPERTY() ObjectRefMap<int32, Object> fRefMap;
    ROCKS_PROPERTY() ComboRefLink fLink;
    ROCKS_PROPERTY() StructRefArray<ComboRefLink> fLinks;
    ROCKS_PROPERTY() ComboRefLink fLinkFixed[2];
    ROCKS_PROPERTY() ComboValues values;
    ROCKS_PROPERTY() ComboObjectRefs refs;
};

// ComboAsset과 같은 이름·모양의 필드를 모두 EditorOnly로 둔다(쿠킹 패키지에서 빠지고 에디터
// 패키지에서 왕복한다 — 모든 칸의 EditorOnly 실효 플래그). bitfield는 EditorOnly를 받지 않으면
// 빠진다.
ROCKS_CLASS()
struct ComboEditorAsset final : Asset {
    explicit ComboEditorAsset(Allocator& allocator) noexcept
        : fString(allocator), fSoft(allocator), fStruct(allocator), fArray(allocator),
          fMap(allocator), fSet(allocator), fRefArray(allocator), fRefMap(allocator),
          fLinks(allocator), values(allocator), refs(allocator) {
    }

    ROCKS_PROPERTY(EditorOnly) bool fBool = false;
    ROCKS_PROPERTY(EditorOnly) uint8 fUInt8 = 0;
    ROCKS_PROPERTY(EditorOnly) uint16 fUInt16 = 0;
    ROCKS_PROPERTY(EditorOnly) uint32 fUInt32 = 0;
    ROCKS_PROPERTY(EditorOnly) uint64 fUInt64 = 0;
    ROCKS_PROPERTY(EditorOnly) int8 fInt8 = 0;
    ROCKS_PROPERTY(EditorOnly) int16 fInt16 = 0;
    ROCKS_PROPERTY(EditorOnly) int32 fInt32 = 0;
    ROCKS_PROPERTY(EditorOnly) int64 fInt64 = 0;
    ROCKS_PROPERTY(EditorOnly) float fFloat = 0.0f;
    ROCKS_PROPERTY(EditorOnly) double fDouble = 0.0;
    ROCKS_PROPERTY(EditorOnly) Guid fGuid {};
    ROCKS_PROPERTY(EditorOnly) String fString;
    ROCKS_PROPERTY(EditorOnly) Name fName;
    ROCKS_PROPERTY(EditorOnly) AssetId fAssetId {};
    ROCKS_PROPERTY(EditorOnly) Sha256Digest fDigest {};
    ROCKS_PROPERTY(EditorOnly) ComboTone fEnum = ComboTone::None;
    ROCKS_PROPERTY(EditorOnly) ComboMask fFlags {};
    ROCKS_PROPERTY(EditorOnly) ClassRef<Asset> fClassRef;
    ROCKS_PROPERTY(EditorOnly) SoftObjectRef<Object> fSoft;
    ROCKS_PROPERTY(EditorOnly) WeakObjectRef<Object> fWeak;
    ROCKS_PROPERTY(EditorOnly) ObjectRef<Object> fObjectRef;
    ROCKS_PROPERTY(EditorOnly) ComboPoint fStruct;
    ROCKS_PROPERTY(EditorOnly) InstancedStruct<> fInstanced;
    ROCKS_PROPERTY(EditorOnly) Text fText;
    ROCKS_PROPERTY(EditorOnly) ComboChecked fChecked;
    ROCKS_PROPERTY(EditorOnly) BulkDataRef fBulk;
    ROCKS_PROPERTY(EditorOnly) uint8 fBit : 1 = 0;
    ROCKS_PROPERTY(EditorOnly) uint8 fOtherBit : 1 = 0;
    ROCKS_PROPERTY(EditorOnly) Array<int32> fArray;
    ROCKS_PROPERTY(EditorOnly) int32 fFixed[2] = {};
    ROCKS_PROPERTY(EditorOnly) OrderedMap<int32, int32> fMap;
    ROCKS_PROPERTY(EditorOnly) OrderedSet<int32> fSet;
    ROCKS_PROPERTY(EditorOnly) Optional<int32> fMaybe;
    ROCKS_PROPERTY(EditorOnly) ObjectRefArray<Object> fRefArray;
    ROCKS_PROPERTY(EditorOnly) ObjectRef<Object> fRefFixed[2];
    ROCKS_PROPERTY(EditorOnly) ObjectRefMap<int32, Object> fRefMap;
    ROCKS_PROPERTY(EditorOnly) ComboRefLink fLink;
    ROCKS_PROPERTY(EditorOnly) StructRefArray<ComboRefLink> fLinks;
    ROCKS_PROPERTY(EditorOnly) ComboRefLink fLinkFixed[2];
    ROCKS_PROPERTY(EditorOnly) ComboValues values;
    ROCKS_PROPERTY(EditorOnly) ComboObjectRefs refs;
};

// Required 멤버를 담은 값 struct(ClassRef·Soft)와 참조 struct(ObjectRef)다.
ROCKS_STRUCT()
struct ComboNeed {
    explicit ComboNeed(Allocator& allocator) noexcept : asset(allocator) {
    }

    ROCKS_PROPERTY(Required) ClassRef<Asset> kind;
    ROCKS_PROPERTY(Required) SoftObjectRef<Object> asset;
};

ROCKS_STRUCT()
struct ComboNeedLink {
    ROCKS_PROPERTY(Required) ObjectRef<Object> target;
};

// Required를 받는 종류(ObjectRef·ClassRef·SoftObjectRef) × 그 종류의 모든 자리(필드·struct 멤버·
// Array 원소·정적 배열 원소·Map 값·Set 원소)에 Required를 둔다. 저장은 null을 거부하고(쓰기 시험),
// 쿠킹 로드는 실효 누락을 CorruptPackage로 거부한다.
ROCKS_CLASS()
struct ComboRequiredAsset final : Asset {
    explicit ComboRequiredAsset(Allocator& allocator) noexcept
        : refArray(allocator), refMap(allocator), classArray(allocator), classMap(allocator),
          classSet(allocator), soft(allocator), softArray(allocator),
          softFixed{SoftObjectRef<Object>(allocator), SoftObjectRef<Object>(allocator)},
          softMap(allocator), softSet(allocator), need(allocator), needs(allocator) {
    }

    ROCKS_PROPERTY(Required) ObjectRef<Object> ref;
    ROCKS_PROPERTY(Required) ObjectRefArray<Object> refArray;
    ROCKS_PROPERTY(Required) ObjectRef<Object> refFixed[2];
    ROCKS_PROPERTY(Required) ObjectRefMap<int32, Object> refMap;
    ROCKS_PROPERTY(Required) ClassRef<Asset> classRef;
    ROCKS_PROPERTY(Required) Array<ClassRef<Asset>> classArray;
    ROCKS_PROPERTY(Required) ClassRef<Asset> classFixed[2];
    ROCKS_PROPERTY(Required) OrderedMap<int32, ClassRef<Asset>> classMap;
    ROCKS_PROPERTY(Required) OrderedSet<ClassRef<Asset>> classSet;
    ROCKS_PROPERTY(Required) SoftObjectRef<Object> soft;
    ROCKS_PROPERTY(Required) Array<SoftObjectRef<Object>> softArray;
    ROCKS_PROPERTY(Required) SoftObjectRef<Object> softFixed[2];
    ROCKS_PROPERTY(Required) OrderedMap<int32, SoftObjectRef<Object>> softMap;
    ROCKS_PROPERTY(Required) OrderedSet<SoftObjectRef<Object>> softSet;
    ROCKS_PROPERTY() ComboNeed need;
    ROCKS_PROPERTY() Array<ComboNeed> needs;
    ROCKS_PROPERTY() ComboNeedLink needLink;
};

// ---- 하위 객체(Asset이 아닌 클래스, 상속) ----

// 대표 에셋 아래 하위 객체다. 같은 패키지·다른 패키지 대상을 가리키는 참조 묶음을 가진다.
ROCKS_CLASS()
struct ComboPart : Object {
    explicit ComboPart(Allocator& allocator) noexcept : refs(allocator) {
    }

    ROCKS_PROPERTY() int32 index = 0;
    ROCKS_PROPERTY() ComboObjectRefs refs;
    ROCKS_PROPERTY() WeakObjectRef<Object> watcher;
};

// 상속한 하위 객체다(기반 필드 + 파생 필드).
ROCKS_CLASS()
struct ComboPartDerived final : ComboPart {
    explicit ComboPartDerived(Allocator& allocator) noexcept : ComboPart(allocator), note(allocator) {
    }

    ROCKS_PROPERTY() String note;
    ROCKS_PROPERTY() ObjectRef<ComboPart> next;
};

} // namespace rocks::asset_fixture
