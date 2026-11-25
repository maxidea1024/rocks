// 모든 엔진 라이브러리·엔트리가 처음 include하는 작은 묶음이다(Docs/Design/EngineShaders 4.1절, 결정
// SLD2). include 목록은 정확히 넷이고 view·장면 텍스처는 넣지 않는다(UE Common.ush와 다른 점 — 작은
// 변경이 모든 셰이더의 키를 바꾸지 않게 한다). Platform.rsh는 전처리기가 강제로 앞에 넣는다.
//
// 사용 예:
//   #include "/Engine/Private/Common/ShaderPrelude.rsh"
#pragma once

#include "/Engine/Public/ShaderFeatures.rsh"
#include "/Engine/Private/Common/ShaderAssert.rsh"
#include "/Engine/Private/Math/MathConstants.rsh"
#include "/Engine/Private/Math/ScalarMath.rsh"
