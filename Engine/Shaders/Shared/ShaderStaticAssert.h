// C++·HLSL 공용 정적 검사다(Docs/Design/EngineShaders 3.3절, UE HLSLStaticAssert.h 역할).
//
// ROCKS_SHADER_STATIC_ASSERT(condition, message): C++에서는 static_assert다. HLSL에서는 아무것도
// 하지 않는다 — HLSL 2018은 static_assert가 없고(DXC 1.9 확인), FXC(D3D11_SM5)는 static const를
// 상수식으로 보지 않아(X3058) 배열 길이 기법도 쓸 수 없다. Shared 헤더는 C++(RenderData·Renderer)도
// 컴파일하므로 같은 값의 검사는 C++ 쪽에서 빠지지 않는다. message는 영어 문자열이고 condition은
// 정수 상수식이다. 문장 끝 세미콜론은 호출자가 쓴다(HLSL에서는 빈 문장이 된다).
//
// 사용 예:
//   ROCKS_SHADER_STATIC_ASSERT(LightGridCellPixels == 64, "light grid cell must be 64 pixels");
#pragma once

#if defined(ROCKS_SHADER_MODEL_MAJOR)

#define ROCKS_SHADER_STATIC_ASSERT(condition, message)

#else

#define ROCKS_SHADER_STATIC_ASSERT(condition, message) static_assert(condition, message)

#endif
