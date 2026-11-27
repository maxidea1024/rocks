#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <Core/Text/Name.h>
#include <Core/Text/StringView.h>
#include <MaterialSchema/Nodes/Functions/FunctionsNodeEnums.h>

// 함수 그래프의 입출력 노드 하나다(Docs/Design/MaterialSystem 6.12절 MS2e2·MS2e3). 입력은
// FunctionInput, 출력은 FunctionOutput·LayerOutput이다.
namespace rocks::material::detail {

struct FunctionSignaturePin {
    // id 속성이다(호출 노드 동적 핀 키 P_<id>).
    Guid id;
    // 호출 핀 키다(P_<id>).
    Name key;
    // 원본(snapshot) 그래프의 노드 색인이다.
    usize node = 0;
    int32 sortPriority = 0;
    // 입력 종류다(출력은 쓰지 않는다).
    FunctionInputType type = FunctionInputType::Vector3;
    BlendInputRelevance relevance = BlendInputRelevance::General;
    // inputName·outputName 원문이다(snapshot 문서 수명).
    StringView name;
    // MaterialAttributes 형인가(입력은 inputType, 출력은 6.12절 MS2e3 판정).
    bool attributes = false;
};

} // namespace rocks::material::detail
