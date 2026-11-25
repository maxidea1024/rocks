# 머티리얼 시스템(MaterialSystem) 설계

## 18. 개정 이력

| 개정 | 날짜 | 내용 |
|---|---|---|
| 1(초안) | 2026-11-17 – 11-18 | 첫 작성. UE 5.8.4 조사(에셋 계층·파라미터·uniform expression·static 변형·컬렉션, 번역기 기본값과 IR 실험 상태, 템플릿 연결, 셰이더 맵 키·퍼뮤테이션·사용 플래그·품질, 비동기 컴파일·대체·미리보기, DDC·쿠킹, Substrate 기본 여부·Blendable/Adaptive·고전 경로 상태, 도메인·혼합·shading model 레거시 확인, 모바일·SM5 제약). 모듈 배치·에셋 모델·그래프 스키마·IR·코드 생성·퍼뮤테이션·파라미터·표면 모델·컴파일·쿠킹·backend 표·에디터 요구·단계·다른 문서 요구. 2026-11-18: 결정 MD1–MD9 확정(사용자 "권장안대로", MD4는 Renderer RN13에 맞춤), Renderer 개정 2(2dc9068) 14절 계약에 경계를 맞추고 다른 점을 10.4절에 정리, 교차 검증 리뷰 반영(셰이더 집합 id를 내용 hash로, 파라미터 기본값을 번역 키에서 제외, 사용 대상 유도를 cookKey 입력으로, 정점 입력 거르기의 장치 없는 위치, PinKey 정의, 의존 키 규칙, TextureStreaming 5.3 규칙, 요구 ID 접두어 `M`) |
| 2(초안) | 2026-11-18 | Renderer 개정 2와 교차 검증해 두 문서를 맞췄다(10.4절을 "정합 결과" 표로). D1–D9 판단: WPO는 정점 함수, 번호표 C++ `RenderData`·HLSL `/Engine/Shared/`(순환 해소), `AlphaHoldout`은 Renderer 후속 값, 거르기 인자 `MaterialShaderTraits`, "static 변형" 용어, pass별 경로와 `RENDER_PATH_*`, depth·그림자 pass의 마스크 전용 함수, 영역 이름 유지. 사용자 결정 반영: MD4를 Renderer RN13과 함께 형식별 SRV 상한으로 개정(1·7.4·13·15·16·17절, MSR1·MSR12·MSR13), 셰이더 시스템 결정 4 확정(MetaCompiler 방식)을 MD3·15.1·MSR4에 반영. 그 밖의 수정: Render Path Switch의 깊이 일치 규칙 V15, 경로 전용 pass에 `DepthAux`, 노멀 공간 변환 위치, 노드 미리보기 pass 이름, MaterialCook 의존에 `RendererShaders`, 용어 통일("도메인" → "영역", "혼합 방식" → "블렌드 모드"), Renderer 단계(RS0·RS2·RS5)와의 선후(10.3·15.1절), MS-T6 허용 오차, MRD1–MRD12 처리 상태(16절) |
| 3(초안) | 2026-11-18 | 셰이더 시스템 개정 13의 MSR1·MSR12·MSR13 반영 상태와 공유 출처일 때의 생성 규칙(13절 9항 대응)을 적었다 |
| 4(초안) | 2026-11-18 | 셰이더 시스템 개정 16의 MSR5 반영 상태를 적었다(Renderer RQ13과 같은 개정) |
| 5(초안) | 2026-11-18 | RenderBase 개정 16의 상수 버퍼 예산에 맞춰 9.7절 컬렉션 개수 근거와 크기 상한(GLES `vec4` 합에서 계산)을 고쳤다. 셰이더 시스템 개정 17의 공통 상한 폐기에 맞춰 backend 표 문구를 고쳤다 |
| 9(초안) | 2026-11-23 | 15.2절에 MS3a(에셋 클래스 골격 `MaterialBase`·`Material`, 필드 없음, 의존 AssetRuntime)를 더했다. 메시 슬롯 참조 타입이 필요해 MS1·MS2보다 먼저 한다(사용자 결정 2026-11-23). MS3은 이 골격에 4.2절 필드·쿠킹을 더한다 |
| 8(초안) | 2026-11-22 | [노드 그래프](../NodeGraph/README.ko.md) 개정 4에 맞춰 3.2절 MaterialSchema 의존에 ObjectRuntime(리플렉션 codec)을 적었다 |
| 7(초안) | 2026-11-22 | [노드 그래프](../NodeGraph/README.ko.md) 개정 2의 GR2를 반영해 그래프 데이터 모델·변경·undo·의미 바이트를 공통 `NodeGraph`로 옮겼다. 3.2절: MaterialSchema가 NodeGraph에, EditorMaterial이 GraphView에 의존. 4.3절: 노드 속성을 `InstancedStruct`에서 Record 바이트로, 배치는 `GraphDocumentLayout`, 사라진 핀은 고아 핀. 5.2절: 형식은 노드 그래프 6절 참조, 재경유는 의미 비교 때 접히는 공통 노드, 주석은 배치 데이터. 5.5절: semantic hash를 의미 바이트 + `NormalizeSemanticNode`로 정의(입력 기본값·활성 상태·고아 핀 포함, 간선 정규 순서, migration 뒤 계산). 14절: E1–E4·E13 제공 모듈. 15.1절: MS1 선행을 B9에서 NG0–NG3으로. 15.2절: MS1 내용 |
| 6(초안) | 2026-11-20 | 메시 에셋 스펙(MeshAnimationAssets MR4)과 Unreal 교환 스펙(UnrealExchange UXR4)의 요구를 받았다: 8.4절 메시 정점 입력 타입 둘(정적·skinned)과 morph 특성, 9.2절 외부 그래프 번역의 같은 이름 파라미터 병합·"공유" 표시 규칙, 11.3절 엔진 내장 "구운 표면" 머티리얼 |
