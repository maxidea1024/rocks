// 벡터 조건 선택·논리곱·논리합이다(Docs/Design/MaterialSystem 7.7절). HLSL 2021은 벡터 ?:·&&·||를
// 막고 select·and·or를 준다. 생성 원문(Material.rsh)과 Double 연산(MaterialDouble.rsh)이 쓴다.
//
// 사용 예:
//   float3 chosen = MaterialSelect(mask, a, b);
#pragma once

#if ROCKS_HLSL2021
#define MaterialSelect(condition, whenTrue, whenFalse) select(condition, whenTrue, whenFalse)
#define MaterialAnd(left, right) and(left, right)
#define MaterialOr(left, right) or(left, right)
#else
#define MaterialSelect(condition, whenTrue, whenFalse) ((condition) ? (whenTrue) : (whenFalse))
#define MaterialAnd(left, right) ((left) && (right))
#define MaterialOr(left, right) ((left) || (right))
#endif
