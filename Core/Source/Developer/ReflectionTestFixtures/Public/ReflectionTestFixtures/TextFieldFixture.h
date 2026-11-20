#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Localization/Text.h>
#include <Meta/Annotations.h>
#include <Object/Object.h>

namespace rocks::reflection_fixture {

// 리플렉션 B9l Text PROPERTY fixture다(Docs/Design/ReflectionExtensions.ko.md 13절).
// TextFieldCaption은 Text 멤버를 담은 struct이고, TextFieldHolder는 필드·Array 원소·정적 배열
// 원소·Map 값·struct 멤버 자리의 Text를
// 담는다(TextGraphTests·TextFieldPathTests·TextFieldJsonTests).
ROCKS_STRUCT()
struct TextFieldCaption {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Text label;
    ROCKS_PROPERTY() uint32 order = 0;
};

ROCKS_CLASS()
struct TextFieldHolder final : Object {
    ROCKS_GENERATED_BODY()

    explicit TextFieldHolder(Allocator& allocator) noexcept : lines(allocator), byKey(allocator) {
    }

    ROCKS_PROPERTY() Text title;
    ROCKS_PROPERTY() Text note;
    ROCKS_PROPERTY() Array<Text> lines;
    ROCKS_PROPERTY() Text fixed[2];
    ROCKS_PROPERTY() OrderedMap<uint32, Text> byKey;
    ROCKS_PROPERTY() TextFieldCaption caption;
};

} // namespace rocks::reflection_fixture
