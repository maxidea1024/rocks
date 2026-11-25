# 머티리얼 시스템(MaterialSystem) 설계

## 15. 단계와 완료 기준

### 15.1 선행 작업

| 선행 | 담당 문서 | 막는 단계 | 이유 |
|---|---|---|---|
| SH1(워커·전처리기·두 층 키·너겟) | 셰이더 시스템 | MS2 | 생성 원문을 컴파일해 검증 |
| SH2(리플렉션 정규화·워커 규칙 검사·P14 셰이더 타입) | 셰이더 시스템 | MS2·MS4 | 7.2절 규칙 검사, pass 셰이더 타입 |
| SH3·P7(파라미터 구조체·`ShaderParameterLayout`) | 셰이더 시스템 | MS2 | 동적 layout 형식(결정 MD3) |
| 셰이더 결정 4(파라미터 선언, 2026-11-18 MetaCompiler 방식으로 확정) | 셰이더 시스템 13·15절 | MS2 | MD3의 `ShaderParameterLayout` 데이터 형식(MSR4) |
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
| MS1 | MaterialSchema: NodeGraph 위 머티리얼 schema·노드 descriptor 등록·semantic hash 정규화(`NormalizeSemanticNode`)·migration 함수 | 저장/재열기 왕복, 화면 편집의 semantic hash 불변, 모르는 노드 보존, 노드 등록 해제 drain | MAT0 |
| MS2 | MaterialCompiler: 검증(V1–V12)·IR·최적화·HLSL 생성·상수 식·layout(SF1 표면 범위) | MS-T1·T2·T3, 생성 원문이 엔진 컴파일 프로필 모든 형식에서 컴파일·워커 규칙 검사 통과, V 규칙 음성 대조 | MAT0 |
| MS3a | Material 에셋 모듈 골격: `MaterialBase`(Abstract)와 `Material` 클래스만, 필드 없음(4.2절 필드는 MaterialSchema·MaterialRuntime이 있어야 하므로 MS3에서 더한다). 의존은 AssetRuntime뿐(3.2절의 MaterialSchema·MaterialRuntime 의존은 MS3에서 더한다). 메시 슬롯(`ObjectRef<MaterialBase>` — MeshAnimationAssets 4.3절)이 참조할 타입이 MS1·MS2보다 먼저 필요해서 앞당긴다(사용자 결정 2026-11-23 "Material 에셋 클래스만 먼저 만들어"). `MaterialInstance` 등 나머지 클래스는 MS3·MS6 | 두 클래스 등록, `Material`이 `MaterialBase`·`Asset`으로 판정됨(`ObjectRef<MaterialBase>` 대입 조건), `MaterialBase`가 추상이라 만들 수 없음, 에디터 패키지 저장·로드·재저장 왕복. **구현**(2026-11-23 — 모듈 `Engine/Source/Runtime/Material`, `RegisterMaterialTypes`, 시험 `Material_*`) | MAT0 |
| MS3 | Material 에셋 모듈·MaterialCook: 에셋 클래스·번역 너겟·job 목록·쿠킹 데이터·라이브러리 항목·퍼뮤테이션 보고 | MS-T4·T5, 원본 없이 쿠킹 데이터만으로 로드, 부분 쿠킹(함수 변경의 역의존만) | MAT0–MAT2 |
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

계획 14.9절 MT01–MT10은 위 단계의 완료 기준으로 그대로 쓴다(MT01 → MS1, MT02 → MS2, MT03 → MS4, MT04·MT05·MT07·MT09 → MS5, MT06 → MS6·MS7, MT08 → MS2·MS3, MT10 → MS1·MS5).

### 15.4 측정

| ID | 측정 | 쓰는 곳 |
|---|---|---|
| MM1 | 번역 시간(p50·p95, 노드 수별) | 11.2절 번역을 메인 프로세스 작업으로 둘지 |
| MM2 | 상수 식 평가 비용(인스턴스 수·프레임당 변경 수) | 9.4절 병렬 평가 |
| MM3 | GLES highp 전용의 성능 비용 | 13절 half 미지원 영향 |
| MM4 | 머티리얼당 job 수·쿠킹 시간(대표 프로젝트) | 8.5절 상한 기본값 |
| MM5 | 대표 콘텐츠의 머티리얼 텍스처 수 분포 대 GLES·D3D11 형식의 (pass, 퍼뮤테이션)별 예산(Renderer 7.4절 합계 13–15 조합), 16장을 넘는 머티리얼 비율 | 모바일 형식 오류 비율, 공유 샘플러(MSR13) 시점(10.4절 D3) |
