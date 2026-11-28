#pragma once

#include <Base/Types.h>

namespace rocks {

// 객체가 어디서 만들어졌는지다(Docs/Design/Prefab 3.3절 10항·10.5절 — UE EComponentCreationMethod
// 대응). Registry 슬롯 표지로 두고 생성 때 정해져 바뀌지 않는다. graph 저장이 읽는다(선언
// subobject는 노드 종류로 따로 저장한다).
//   Default        — 일반 생성(NewObject·AddComponent·그래프 복원의 객체 노드).
//   ClassSubobject — 클래스 선언 subobject(ConstructionContext::CreateDeclaredSubobjects).
// PrefabNode·InstanceAdded·Runtime은 prefab 단계(PF3·PF4·PF5)에서 더한다.
enum class ObjectOrigin : uint8 {
    Default,
    ClassSubobject
};

} // namespace rocks
