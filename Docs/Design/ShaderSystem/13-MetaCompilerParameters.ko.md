# 셰이더 시스템 설계 — 13. P7 MetaCompiler 셰이더 파라미터

[목차](README.ko.md)

## 13. P7 MetaCompiler 셰이더 파라미터

15절 결정 4(2026-11-18 확정, MetaCompiler 방식)의 내용이다. 바탕인 A0-B(B1–B8)는 구현되었다.

- **선언.** `ROCKS_SHADER_PARAMETERS(Name)` 구조체 종류를 MetaCompiler에 더한다. 파서·descriptor는 B4 `ROCKS_STRUCT`([리플렉션 기반](../ReflectionFoundations.ko.md) 4절)를 바탕으로 한다. 멤버는 ShaderCore 셰이더 전용 정렬 타입(7.2절 5항)과 셰이더 자원 값 타입(텍스처·샘플러·버퍼 SRV·UAV)만 받고, 다른 타입(Core `Matrix4`·`Vector3` 포함)은 MetaCompiler 오류다.
- **identity와 등록.** 공통 규칙대로 셰이더 파라미터 구조체도 identity 명세(`<Module>.rocksids.json`)에 항목을 두고, `ROCKS_STRUCT`와 같은 경로로 `TypeSchema`·TypeRegistry에 등록한다. 셰이더 파라미터 종류는 그 위에 허용 타입 제약과 HLSL 생성만 더한다. 셰이더 자원 멤버를 담을 리플렉션 값 종류는 `ValueKind`에 더한다. `ValueKind`는 닫힌 열거라 값을 덧붙이고 TypeRegistry 경계를 올리는 방식이다(ReflectionFoundations 3절).
- **생성물.**
  - C++: 멤버별 offset·크기 static_assert, `ShaderBindingLayout`과 대조할 레이아웃 표.
  - HLSL: `<out>/Generated/<Module>/Shaders/<Name>.rsh`(기존 `<out>/Generated/<Module>` 규칙). 이름 있는 `cbuffer`로 낸다(7.2절 1항). 가상 경로 `/Engine/Generated/<Module>/<Name>.rsh`(플러그인·프로젝트는 각자의 `Generated`)로 연결하며, UE `/Engine/Generated/`에 해당한다(`ShaderCore.cpp:2490,3081,3096`). 생성 HLSL은 셰이더 맵 키의 include 폐포에 들어간다. 설치 SDK로 내보내는 경로는 후속이다.
- **레이아웃 서명.** 정규화한 레이아웃(멤버 이름·타입·offset·크기, 자원 종류·순번)의 SHA-256이다. job 키 8항(10절)이다.
- **`ShaderParameterLayout`(데이터 형식).** 위 레이아웃 표를 C++ 타입과 무관한 데이터 형식으로 정한다: 멤버(이름·타입·offset·크기), 자원(종류·순번, 텍스처·샘플러 짝), 서명. MetaCompiler 생성물은 이 형식의 상수 표를 내고, [머티리얼 시스템](../MaterialSystem.ko.md)의 머티리얼 컴파일러는 C++ 구조체 없이 같은 형식의 **동적 layout**을 실행 중 데이터로 낸다(결정 MD3, 요구 MSR4). 워커의 리플렉션 대조(7.2절 5항), 바인딩 지도([렌더 기반](../RenderBase.ko.md) 9.3절, 머티리얼 요구 MRB2), GLES 복사 표가 두 출처를 같은 경로 하나로 다룬다. UE도 머티리얼 uniform buffer는 매크로가 아니라 실행 중 layout으로 만든다(`Runtime/Engine/Private/Materials/MaterialUniformExpressions.cpp:524`). 형식 정의는 P7 착수 때 확정하고, 머티리얼 단계 MS2 전이어야 한다.
- 그리기별 상수(7.2절 2항 루트 상수)도 이 종류로 선언하고 종류 표시 "그리기별 상수"를 붙인다(RenderBase 결정 R7). 표시가 있는 구조체는 `ROCKS_ROOT_CONSTANTS`로, 없는 구조체는 이름 있는 cbuffer로 생성하며, 표시가 있는데 128 byte를 넘거나 16 byte 배수가 아니면 MetaCompiler 오류다.

완료 기준: MetaCompiler 음성 대조(금지 타입), identity 명세 검증(일반 구조체와 같은 규칙), 7.3절 시험 (a)·(f)를 생성 구조체로 통과.
