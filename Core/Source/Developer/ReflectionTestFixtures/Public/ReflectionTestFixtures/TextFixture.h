#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/OrderedSet.h>
#include <Core/Encoding/Sha256.h>
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

// 리플렉션 B9i3b 명령행 표기 fixture다(Docs/Design/ReflectionExtensions.ko.md 10.4절 —
// ReflectionText TextValueTests·TextPathTests). TextHolder가 표의 종류(단일 값 15종, 참조 셋,
// ClassRef, 목록·정적 배열·Set·Map·Optional·struct)와 용도별 제외 플래그 필드를 모두 담는다.
// TextQuality는 접두·숨김 열거자, TextChannels는 숨김 비트가 있는 Flags enum, TextKind 계열은
// ClassRef 대상(도구 이름이 다른 클래스 포함)이다.
ROCKS_ENUM(ToolPrefix = "TQ_")
enum class TextQuality : uint8 {
    Low,
    High,
    Secret ROCKS_ENUMERATOR(Hidden)
};

ROCKS_ENUM(Flags)
enum class TextChannels : uint32 {
    None = 0,
    Red = 1,
    Green = 2,
    Blue = 4,
    Alpha ROCKS_ENUMERATOR(Hidden) = 8
};

ROCKS_STRUCT()
struct TextPoint {
    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() int32 y = 0;
};

ROCKS_CLASS(Abstract)
struct TextKindBase : Object {};

ROCKS_CLASS()
struct TextKindDerived final : TextKindBase {};

ROCKS_CLASS(Name = "Text.Renamed")
struct TextKindRenamed final : TextKindBase {};

ROCKS_CLASS()
struct TextStranger final : Object {};

ROCKS_CLASS()
struct TextHolder final : Object {
    explicit TextHolder(Allocator& allocator) noexcept
        : label(allocator), source(allocator), numbers(allocator), words(allocator),
          points(allocator), tags(allocator), scores(allocator), byQuality(allocator),
          byName(allocator), byClass(allocator), bySoft(allocator) {
    }

    ROCKS_PROPERTY() bool enabled = false;
    ROCKS_PROPERTY() uint8 small = 0;
    ROCKS_PROPERTY() uint16 medium = 0;
    ROCKS_PROPERTY() uint32 count = 0;
    ROCKS_PROPERTY() uint64 big = 0;
    ROCKS_PROPERTY() int8 tiny = 0;
    ROCKS_PROPERTY() int16 narrow = 0;
    ROCKS_PROPERTY() int32 offset = 0;
    ROCKS_PROPERTY() int64 wide = 0;
    ROCKS_PROPERTY() float ratio = 0;
    ROCKS_PROPERTY() double precise = 0;
    ROCKS_PROPERTY() Guid id {};
    ROCKS_PROPERTY() AssetId asset;
    ROCKS_PROPERTY() Sha256Digest digest {};
    ROCKS_PROPERTY() String label;
    ROCKS_PROPERTY() Name tag;
    ROCKS_PROPERTY() TextQuality quality = TextQuality::Low;
    ROCKS_PROPERTY() TextChannels channels = TextChannels::None;
    ROCKS_PROPERTY() ObjectRef<TextHolder> next;
    ROCKS_PROPERTY() WeakObjectRef<TextHolder> peer;
    ROCKS_PROPERTY() SoftObjectRef<TextHolder> source;
    ROCKS_PROPERTY() ClassRef<TextKindBase> kind;
    ROCKS_PROPERTY() Array<int32> numbers;
    ROCKS_PROPERTY() Array<String> words;
    ROCKS_PROPERTY() Array<TextPoint> points;
    ROCKS_PROPERTY() int32 triple[3] = {};
    ROCKS_PROPERTY() OrderedSet<Name> tags;
    ROCKS_PROPERTY() OrderedMap<String, int32> scores;
    ROCKS_PROPERTY() OrderedMap<TextQuality, String> byQuality;
    ROCKS_PROPERTY() OrderedMap<Name, TextPoint> byName;
    ROCKS_PROPERTY() OrderedMap<ClassRef<TextKindBase>, int32> byClass;
    ROCKS_PROPERTY() OrderedMap<SoftObjectRef<>, int32> bySoft;
    ROCKS_PROPERTY() Optional<int32> limit;
    ROCKS_PROPERTY() Optional<TextPoint> anchor;
    ROCKS_PROPERTY() TextPoint origin;
    ROCKS_PROPERTY(Transient) int32 cache = 0;
    ROCKS_PROPERTY(ReadOnly) int32 locked = 0;
    ROCKS_PROPERTY(TextExportTransient) int32 exportOnly = 0;
    ROCKS_PROPERTY(NonPIEDuplicateTransient) Guid instance {};
    ROCKS_PROPERTY(DuplicateTransient) TextPoint copied;
};

} // namespace rocks::reflection_fixture
