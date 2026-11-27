#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <MaterialSchema/Nodes/Custom/CustomDefine.h>
#include <MaterialSchema/Nodes/Custom/CustomInput.h>
#include <MaterialSchema/Nodes/Custom/CustomNodeEnums.h>
#include <MaterialSchema/Nodes/Custom/CustomOutput.h>
#include <Meta/Annotations.h>

// Custom Hlsl 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE UMaterialExpressionCustom
// — RE/Public/Materials/MaterialExpressionCustom.h). 이름·기본값은 UE와 같다(b 접두를 떼고
// camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct CustomHlslProperties {
    ROCKS_GENERATED_BODY()

    explicit CustomHlslProperties(Allocator& allocator) noexcept
        : code(allocator), description(allocator), inputs(allocator), additionalOutputs(allocator),
          additionalDefines(allocator), includeFilePaths(allocator) {
    }

    // HLSL 본문이다. UE 생성자는 이름 없는 입력 하나를 더하지만 Rocks 새 노드는 입력이 없다(배열
    // 기본값은 정적 data로 주지 않는다).
    ROCKS_PROPERTY() String code;
    ROCKS_PROPERTY() CustomOutputType outputType = CustomOutputType::Float3;
    ROCKS_PROPERTY() String description;
    ROCKS_PROPERTY() Array<CustomInput> inputs;
    ROCKS_PROPERTY() Array<CustomOutput> additionalOutputs;
    ROCKS_PROPERTY() Array<CustomDefine> additionalDefines;
    ROCKS_PROPERTY() Array<String> includeFilePaths;
    ROCKS_PROPERTY() CustomClipInstruction containsClipInstruction = CustomClipInstruction::Search;
    ROCKS_PROPERTY() bool showCode = false;
};

} // namespace rocks::material
