// 픽셀 셰이더 출력 구조체 선언 매크로다(Docs/Design/EngineShaders 4.1절 A01). 렌더 타깃 1–8개와
// 깊이 출력(SV_Depth) 변형을 같은 멤버 이름(target0–target7·depth)으로 선언해 pass 원문이 출력
// 모양을 손으로 되풀이하지 않게 한다. 타입은 렌더 타깃 형식의 원소 타입(float4·uint4·int4 등)이다.
//
// 형식 상한: GLES 형식은 렌더 타깃 4개까지다(ShaderSystem 7.2절 4항 — 더 쓰는 타입은 거르기로
// 뺀다). conservative depth(SV_DepthGreaterEqual 등)는 워커 허용 시맨틱(7.2절 7항)에 없어 두지
// 않는다(필요해지면 SS 규칙을 먼저 연다).
//
// 사용 예:
//   ROCKS_PIXEL_OUTPUT_2(GBufferOutput, float4, uint4)
//   GBufferOutput Main(...) { GBufferOutput output; output.target0 = ...; output.target1 = ...;
//                             return output; }
//   ROCKS_PIXEL_OUTPUT_1_DEPTH(CopyDepthOutput, float4)   // output.depth : SV_Depth
#pragma once

#define ROCKS_PIXEL_OUTPUT_1(Name, T0) struct Name { T0 target0 : SV_Target0; };
#define ROCKS_PIXEL_OUTPUT_1_DEPTH(Name, T0) struct Name { T0 target0 : SV_Target0; float depth : SV_Depth; };

#define ROCKS_PIXEL_OUTPUT_2(Name, T0, T1) struct Name { T0 target0 : SV_Target0; T1 target1 : SV_Target1; };
#define ROCKS_PIXEL_OUTPUT_2_DEPTH(Name, T0, T1) struct Name { T0 target0 : SV_Target0; T1 target1 : SV_Target1; float depth : SV_Depth; };

#define ROCKS_PIXEL_OUTPUT_3(Name, T0, T1, T2) struct Name { T0 target0 : SV_Target0; T1 target1 : SV_Target1; T2 target2 : SV_Target2; };
#define ROCKS_PIXEL_OUTPUT_3_DEPTH(Name, T0, T1, T2) struct Name { T0 target0 : SV_Target0; T1 target1 : SV_Target1; T2 target2 : SV_Target2; float depth : SV_Depth; };

#define ROCKS_PIXEL_OUTPUT_4(Name, T0, T1, T2, T3) struct Name { T0 target0 : SV_Target0; T1 target1 : SV_Target1; T2 target2 : SV_Target2; T3 target3 : SV_Target3; };
#define ROCKS_PIXEL_OUTPUT_4_DEPTH(Name, T0, T1, T2, T3) struct Name { T0 target0 : SV_Target0; T1 target1 : SV_Target1; T2 target2 : SV_Target2; T3 target3 : SV_Target3; float depth : SV_Depth; };

#define ROCKS_PIXEL_OUTPUT_5(Name, T0, T1, T2, T3, T4) struct Name { T0 target0 : SV_Target0; T1 target1 : SV_Target1; T2 target2 : SV_Target2; T3 target3 : SV_Target3; T4 target4 : SV_Target4; };
#define ROCKS_PIXEL_OUTPUT_5_DEPTH(Name, T0, T1, T2, T3, T4) struct Name { T0 target0 : SV_Target0; T1 target1 : SV_Target1; T2 target2 : SV_Target2; T3 target3 : SV_Target3; T4 target4 : SV_Target4; float depth : SV_Depth; };

#define ROCKS_PIXEL_OUTPUT_6(Name, T0, T1, T2, T3, T4, T5) struct Name { T0 target0 : SV_Target0; T1 target1 : SV_Target1; T2 target2 : SV_Target2; T3 target3 : SV_Target3; T4 target4 : SV_Target4; T5 target5 : SV_Target5; };
#define ROCKS_PIXEL_OUTPUT_6_DEPTH(Name, T0, T1, T2, T3, T4, T5) struct Name { T0 target0 : SV_Target0; T1 target1 : SV_Target1; T2 target2 : SV_Target2; T3 target3 : SV_Target3; T4 target4 : SV_Target4; T5 target5 : SV_Target5; float depth : SV_Depth; };

#define ROCKS_PIXEL_OUTPUT_7(Name, T0, T1, T2, T3, T4, T5, T6) struct Name { T0 target0 : SV_Target0; T1 target1 : SV_Target1; T2 target2 : SV_Target2; T3 target3 : SV_Target3; T4 target4 : SV_Target4; T5 target5 : SV_Target5; T6 target6 : SV_Target6; };
#define ROCKS_PIXEL_OUTPUT_7_DEPTH(Name, T0, T1, T2, T3, T4, T5, T6) struct Name { T0 target0 : SV_Target0; T1 target1 : SV_Target1; T2 target2 : SV_Target2; T3 target3 : SV_Target3; T4 target4 : SV_Target4; T5 target5 : SV_Target5; T6 target6 : SV_Target6; float depth : SV_Depth; };

#define ROCKS_PIXEL_OUTPUT_8(Name, T0, T1, T2, T3, T4, T5, T6, T7) struct Name { T0 target0 : SV_Target0; T1 target1 : SV_Target1; T2 target2 : SV_Target2; T3 target3 : SV_Target3; T4 target4 : SV_Target4; T5 target5 : SV_Target5; T6 target6 : SV_Target6; T7 target7 : SV_Target7; };
#define ROCKS_PIXEL_OUTPUT_8_DEPTH(Name, T0, T1, T2, T3, T4, T5, T6, T7) struct Name { T0 target0 : SV_Target0; T1 target1 : SV_Target1; T2 target2 : SV_Target2; T3 target3 : SV_Target3; T4 target4 : SV_Target4; T5 target5 : SV_Target5; T6 target6 : SV_Target6; T7 target7 : SV_Target7; float depth : SV_Depth; };
