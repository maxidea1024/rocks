# 머티리얼 시스템(MaterialSystem) 설계

## 15. 단계와 완료 기준

### 15.1 선행 작업

| 선행 | 담당 문서 | 막는 단계 | 이유 |
|---|---|---|---|
| SH1(워커·전처리기·두 층 키·너겟) | 셰이더 시스템 | MS2 | 생성 원문을 컴파일해 검증 |
| SH2(리플렉션 정규화·워커 규칙 검사·P14 셰이더 타입) | 셰이더 시스템 | MS2·MS4 | 7.2절 규칙 검사, pass 셰이더 타입 |
| SH3·P7(파라미터 구조체·`ShaderParameterLayout`) | 셰이더 시스템 | MS2c | 동적 layout 형식(결정 MD3). 데이터 형식 `ShaderParameterLayout`(ShaderCore, SH2a2 손 표)은 이미 있어 MS2c가 그 형식을 낸다. MetaCompiler(P7)는 C++ 쪽 생성만 바꾸므로 MS2c를 막지 않는다 |
| 셰이더 결정 4(파라미터 선언, 2026-11-18 MetaCompiler 방식으로 확정) | 셰이더 시스템 13·15절 | MS2 | MD3의 `ShaderParameterLayout` 데이터 형식(MSR4) |
| `ShaderParameterLayout` 확장(16절 MSR14): 이름을 소유하는 builder, 텍스처·샘플러 자원 slot 표, 머티리얼별 layout을 job 키 8행·라이브러리 항목에 넣는 경로 | 셰이더 시스템 13절 | MS2c | 지금 헤더는 cbuffer 멤버만 있고 정적 수명 목록이다(`ShaderParameterLayout.h`, `ShaderType.h:113`) |
| 셰이더 타입의 공급 기능 선언과 traits를 받는 거르기(16절 MSR15·MRB9) | 셰이더 시스템 12.2절·RenderBase 10.2절 | MS2b(MS-T13) | 지금 거르기 서명에 traits가 없다(`ShaderType.h:87–89`). 없으면 MS-T13은 MaterialCompiler 시험용 거르기로만 한다 |
| P6b(TargetProfile·라이브러리 묶음 id) | 셰이더 시스템 | MS3 | 셰이더 집합 쿠킹 |
| 셰이더 시스템 개정 MSR1–MSR11(16절) | 셰이더 시스템 | 각 단계 | |
| A1–A4(AssetRuntime·ProjectLayout·에셋 너겟·Cooker) | 에셋 시스템 | MS3 | 에셋·너겟·쿠킹 |
| A5(텍스처 에셋) | 에셋 시스템 | MS3 | 텍스처 파라미터 |
| NG0–NG3(문서 모델·Record 바이트 속성 GD23·`GraphMutator`·조각 codec·migration·고아 핀) | [노드 그래프](../NodeGraph/README.ko.md) | MS1 | 그래프 데이터·노드 속성 저장·migration |
| RB3–RB5(셰이더 집합·바인딩·상수 블록·정점 입력 타입) | 렌더 기반 | MS4 | 실행 시 바인딩 |
| Renderer 스펙 14절 계약(개정 3)과 MRD1–MRD12 처리(Renderer 14.7절) | Renderer | MS2(계약)·MS4(구현) | 생성 원문 계약 |
| Renderer RS0(Unlit base pass·depth prepass)·RS2(Deferred) | Renderer 17.2절 | MS4(RS0), MS-T6(RS2) | 렌더 연결과 두 경로 기준 영상. 반대로 Renderer RS0은 MS1–MS3과 MS4의 렌더 상태 부분을 선행으로 둔다(Renderer 17.1절) |
| TextureStreaming 바인딩 snapshot | 텍스처 스트리밍 | MS4 | 텍스처 handle 해석 |
| 정점 입력 타입 선언의 장치 없는 위치(16절 MRB8) | 렌더 기반 | MS3 | 쿠커의 거르기 |

### 15.2 단계

| 단계 | 내용 | 완료 기준 | 계획 대응 |
|---|---|---|---|
| MS1a | MaterialSchema 기반: 핀 타입 표(5.3절), 머티리얼 노드 정의 형식(단계·capability·문맥 기능·상태·선행 ID, 19.2·19.3절)과 등록표(엔진·플러그인 공용, 19.7절), `GraphSchema` 구현, 출력 노드(5.6절)와 `CreateDefaultNodes`, `NormalizeSemanticNode`(5.5절), `RegisterMaterialAttribute`(19.12절) | 저장/재열기 왕복, 화면 편집의 semantic hash 불변, 모르는 노드 보존, 노드 등록 해제 drain, 가짜 플러그인 등록·해제 | MAT0 |
| MS1b | 19장 노드 정의 전부(상태 "구현"·"TODO" 297개): 핀·속성 struct(리플렉션, 에셋 참조는 19.4.3절 형식)·메뉴·migration 자리, 문맥 값 표(19.3.2절), 단일 원본 `MaterialNodeCatalog.json`과 policy 검사(19.2.2절) | MS-T12 (a), policy 검사 | MAT0 |
| MS2a | MaterialCompiler 기반: IR(6.2절)·분석·최적화·HLSL 생성 골격(7.3절 진입점 함수와 정의 — 표면 함수는 Renderer `SurfaceAttributes` 계약대로, custom·previous 진입점은 MS2b·MS2d가 채움)·`MaterialCommon.rsh`; 상수·수학·`DynamicBranch`·`Operator`·이름 있는 재경유·출력 노드 스칼라 핀 lowering | MS-T1·T2·T3, V1·V2·V4·V5·V7·V8·V11·V12 음성 대조 | MAT0 |
| MS2b | 문맥 값 표 전부(19.3.2절)와 진입점별 사용 정의(19.3.1절)·previous 문맥(19.3.5절)·좌표·벡터·공간 변환·미분·스위치(19.3.4절 해석 시점별)·보간기·파티클·하늘·데칼 등 공급 노드 | MS-T3·MS-T13, V6·V15 음성 대조 | MAT0–MAT1 |
| MS2c | 텍스처·파라미터·상수 식 프로그램(9.4절)·동적 layout(MSR14 뒤)·scene·깊이 노드; TODO 노드와 속성 TODO의 `PrerequisiteMissing`(19.2.1절 — static 해석·DCE 뒤) | MS-T3·MS-T7(GPU readback은 MS4)·MS-T10, V9·V10 음성 대조, MS-T12 (b)의 MS2c 범위 | MAT0–MAT2 |
| MS2d | 표면: BSDF 노드·표면 연산을 closure 하나로 접기(MD1)·대표 노멀(19.3.3절)·`SurfaceFromShadingModel`·`SurfaceFromAttributes`·영역별 표면·추가 출력 노드·custom 출력 진입점 | MS-T3, 접기 규칙 골든, MS-T12 (b)의 MS2d 범위(`MaterialCacheOutput`·`SurfaceToon`) | MAT0·MAT4 |
| MS2e | 함수·레이어·속성: `MaterialSourceProvider`(19.4.2절), 함수 펼치기·scope(9.2절)·레이어 결합·Make/Break/Get/Set/Blend Attributes·`Blend`·`Aggregate`(MaterialAttributes 종류)·다른 머티리얼 참조 | MS-T9(메모리 provider), V3·V14 음성 대조 | MAT2–MAT3 |
| MS2f | 고급·보조: 임의 HLSL(7.5절)·노이즈 전 종류·Sobol·색 변환·Fresnel·BumpOffset·RotateAboutAxis 등 | MS-T3, V13 음성 대조 | MAT1–MAT2 |
| MS2 완료 | MS2a–MS2f 전부 | MaterialCompiler 시험용 pass 셰이더 타입(영역·단계별, 문맥 기능 전부 선언)과 시험 정점 입력 타입으로 생성 원문이 엔진 컴파일 프로필 모든 형식에서 컴파일·워커 규칙 검사 통과. Renderer pass가 생기면 그 조합도 같은 시험에 든다 | MAT0 |
| MS3a | Material 에셋 모듈 골격: `MaterialBase`(Abstract)와 `Material` 클래스만, 필드 없음(4.2절 필드는 MaterialSchema·MaterialRuntime이 있어야 하므로 MS3에서 더한다). 의존은 AssetRuntime뿐(3.2절의 MaterialSchema·MaterialRuntime 의존은 MS3에서 더한다). 메시 슬롯(`ObjectRef<MaterialBase>` — MeshAnimationAssets 4.3절)이 참조할 타입이 MS1·MS2보다 먼저 필요해서 앞당긴다(사용자 결정 2026-11-23 "Material 에셋 클래스만 먼저 만들어"). `MaterialInstance` 등 나머지 클래스는 MS3·MS6 | 두 클래스 등록, `Material`이 `MaterialBase`·`Asset`으로 판정됨(`ObjectRef<MaterialBase>` 대입 조건), `MaterialBase`가 추상이라 만들 수 없음, 에디터 패키지 저장·로드·재저장 왕복. **구현**(2026-11-23 — 모듈 `Engine/Source/Runtime/Material`, `RegisterMaterialTypes`, 시험 `Material_*`) | MAT0 |
| MS3b | 에셋 클래스 제대로(사용자 지시 2026-11-25 "타입만 임시로 선행 … 이젠 제대로"): 클래스당 파일로 나눔(이동 커밋), `MaterialBase`·`Material`·`MaterialInstance` 필드(4.2절, 19.9–19.11절 — `MaterialTraitsDesc`·`MaterialTraitOverrides`·파라미터 override·사용 대상·profile·물리 재질 TODO 필드), `graph : MaterialGraph` 필드와 MaterialSchema 의존. `MaterialFunction`·`MaterialFunctionInstance`·`ParameterCollection`도 같은 단계에서 필드까지 | 리플렉션 등록, 에디터 패키지 저장·로드·재저장 왕복, Hard·Build 참조 보고(4.4절), TODO 필드의 기본값 아닌 값 진단 | MAT0 |
| MS3 | MaterialCook(MS3b 클래스 위): 번역 너겟·job 목록·쿠킹 데이터·라이브러리 항목·퍼뮤테이션 보고 | MS-T4·T5, 원본 없이 쿠킹 데이터만으로 로드, 부분 쿠킹(함수 변경의 역의존만) | MAT0–MAT2 |
| MS4 | MaterialRuntime + Renderer 연결: 렌더 상태·상수 식 실행·상수 블록·텍스처 handle·기본 머티리얼·두 렌더 경로 | MS-T6·T7·T8, 계획 MT03 | MAT1 |
| MS5 | 에디터 지원(E1–E14): CompileCoordinator·PreviewSession·노드 미리보기·hot reload·통계 | 계획 MT04·MT05·MT07·MT09 | MAT1–MAT2 |
| MS6 | 함수·Attributes·컬렉션·인스턴스 static 변형·Dynamic Bool·Component Mask | 계획 MT06(함수·인스턴스), MS-T9 | MAT2 |
| MS7 | 레이어 스택·레이어 인스턴스·SF2(반투명 블렌드 모드)·DeferredDecal·PostProcess·UserInterface 영역 | 계획 MT06(레이어), 영역별 기준 영상 | MAT3 |
| MS8 | SF3 표면(고급 shading model preset·slab 직접 입력·표면 연산) | 기준 영상, GLES Deferred 거르기(결정 RN4) | MAT4 |
| MS9 | SF4 여러 closure 저장, Hair(P3) | 계획 14.4절 연구 gate·계획 11.1절 P3 gate 판정 | 연구 gate |

### 15.3 시험

| ID | 시험 | 판정 |
|---|---|---|
| MS-T1 | 같은 그래프 같은 바이트: (a) 같은 파일 두 번 번역 (b) 노드 저장 순서만 섞은 파일 (c) Windows·macOS (d) 작업 디렉터리·locale 변경 | 생성 원문·layout·상수 식·번역 너겟 값·쿠킹 패키지 byte 동일 |
| MS-T2 | 화면 편집만(노드 이동·주석·재경유 추가) | semantic hash 불변, 번역·job 0개 |
| MS-T3 | 골든 원문 | fixture 그래프의 생성 원문이 저장된 기대 원문과 같음(생성 규칙 변경은 처리기 버전과 함께 기대 원문 갱신) |
| MS-T4 | 퍼뮤테이션 수 | fixture 머티리얼 묶음 × 엔진 기본 프로필의 축별 수·job 수가 기대 표와 같음. 사용 대상 축소·품질 공유·static 변형 공유가 수를 줄이는지 확인 |
| MS-T5 | 너겟 무효화 | 그래프·함수·컬렉션 layout·static 값·노드 lowering 버전·프로필을 하나씩 바꾸면 번역 너겟 미적중. 파라미터 기본값(scalar·vector·텍스처)·컬렉션 값·화면 데이터·사용 대상만 바꾸면 번역 너겟 적중(기본값은 cookKey만, 사용 대상은 job 목록과 cookKey만 바뀐다) |
| MS-T6 | 기준 영상 | 같은 머티리얼 fixture를 Deferred·Forward+로 그린 결과가 Renderer가 정한 허용 오차(채널 ±8·2%로 시작, Renderer RS2 (a)·14.7절 MRD10) 안. backend별 결과가 D3D12와 셰이더 시스템 self-test 허용 오차(채널 ±8·2%) 안 |
| MS-T7 | 상수 식 | 상수 식으로 계산한 상수 블록 값과, 같은 식을 셰이더 안에서 계산한 값의 차이가 허용 오차 안(GPU readback) |
| MS-T8 | 대체 | 사용 대상이 빠진 조합·서명 불일치·라이브러리 항목 누락에서 기본 머티리얼로 그리고 진단 1회 |
| MS-T9 | override 보존 | 파라미터 이름 변경·레이어 순서 변경·함수 반복 호출에서 override가 키로 보존되고 서로 덮지 않음 |
| MS-T10 | 텍스처 예산 | (pass, 퍼뮤테이션, 형식)별 예산(Renderer 7.4절)을 넘는 머티리얼이 그 조합에서만 쿠킹 오류이고, 진단에 상한·사용 수·pass·퍼뮤테이션·형식이 나옴. 같은 머티리얼이 데스크톱 형식에서는 쿠킹되고 GLES·D3D11 형식에서만 오류인 fixture를 포함 |
| MS-T11 | 사용 대상 유도 | 메시를 추가한 뒤 부분 쿠킹하면 그 메시가 쓰는 머티리얼의 cookKey가 바뀌어 다시 쿠킹되고, 전체 쿠킹과 결과가 같음 |
| MS-T12 | 노드 목록 대조(19장) | (a) 등록된 엔진 노드 정의(id·상태·선행·범주)가 단일 원본 `MaterialNodeCatalog.json`(19.2.2절)의 "구현"·"TODO" 행과 같음 (b) "TODO" 노드와 값을 넣은 속성 TODO마다 lowering이 그 선행 ID의 `PrerequisiteMissing`을 노드 위치로 내고, static 해석·DCE로 빠진 노드는 진단이 없음 (c) "구현" 노드마다 골든 원문 fixture가 하나 이상 있음 |
| MS-T13 | 문맥 공급 기능(19.3절) | 기능을 쓰는 머티리얼이 그 기능을 선언하지 않은 pass와의 조합에서 V6, 선언한 pass에서는 통과. 생성 파일의 `MATERIAL_CONTEXT_*`가 진입점이 실제로 쓰는 기능과 같음(쓰지 않는 기능은 정의 없음) |

계획 14.9절 MT01–MT10은 위 단계의 완료 기준으로 그대로 쓴다(MT01 → MS1, MT02 → MS2, MT03 → MS4, MT04·MT05·MT07·MT09 → MS5, MT06 → MS6·MS7, MT08 → MS2·MS3, MT10 → MS1·MS5).

### 15.4 측정

| ID | 측정 | 쓰는 곳 |
|---|---|---|
| MM1 | 번역 시간(p50·p95, 노드 수별) | 11.2절 번역을 메인 프로세스 작업으로 둘지 |
| MM2 | 상수 식 평가 비용(인스턴스 수·프레임당 변경 수) | 9.4절 병렬 평가 |
| MM3 | GLES highp 전용의 성능 비용 | 13절 half 미지원 영향 |
| MM4 | 머티리얼당 job 수·쿠킹 시간(대표 프로젝트) | 8.5절 상한 기본값 |
| MM5 | 대표 콘텐츠의 머티리얼 텍스처 수 분포 대 GLES·D3D11 형식의 (pass, 퍼뮤테이션)별 예산(Renderer 7.4절 합계 13–15 조합), 16장을 넘는 머티리얼 비율 | 모바일 형식 오류 비율, 공유 샘플러(MSR13) 시점(10.4절 D3) |
