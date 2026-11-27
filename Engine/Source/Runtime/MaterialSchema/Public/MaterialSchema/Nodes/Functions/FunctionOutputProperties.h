#pragma once

#include <Base/Guid.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>

// Function Output 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionFunctionOutput — RE/Public/Materials/MaterialExpressionFunctionOutput.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct FunctionOutputProperties {
    ROCKS_GENERATED_BODY()

    explicit FunctionOutputProperties(Allocator& allocator) noexcept : description(allocator) {
    }

    ROCKS_PROPERTY() Name outputName;
    ROCKS_PROPERTY() String description;
    ROCKS_PROPERTY() int32 sortPriority {};
    // 출력 id다(함수 호출 노드의 동적 핀 키 P_<id> — NodeGraph GD2).
    ROCKS_PROPERTY() Guid id {};
};

} // namespace rocks::material
