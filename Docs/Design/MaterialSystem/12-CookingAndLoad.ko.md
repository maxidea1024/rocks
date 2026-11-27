# 머티리얼 시스템(MaterialSystem) 설계

## 12. 쿠킹과 에셋 너겟, 실행 시 로드

### 12.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 셰이더 맵 id | 머티리얼 Guid·경로 hash, 품질·feature level, 용도, static 값, 참조 함수·컬렉션, 셰이더·pipeline·vertex factory 의존(원문 hash 포함), 텍스처 참조·include·기본 속성 override·외부 코드 hash, 번역기 종류, 코드 형식, Substrate 설정, 플랫폼 layout | `RE/Public/MaterialShared.h:1304–1401`, 기록 `RE/Private/Materials/MaterialShader.cpp:1692–1834` |
| DDC 키 | `"MATSM_"` + 파생 데이터 버전 Guid + 셰이더 형식·버전 + 셰이더 맵 id + 셰이더 타입별 환경 hash + 속성 정의 + GBuffer 키 + 번역기 버전 | `MaterialShader.cpp:207–219, 256–317`, 버킷 `:320–324` |
| 번역 결과 캐시 | 별도 버킷 `"MaterialTranslation"` | `RE/Private/Materials/HLSLMaterialTranslator.cpp:209–212` |
| 쿠킹 | 플랫폼×품질별 리소스를 만들어 셰이더 맵을 패키지에 직렬화. 셰이더 라이브러리가 켜져 있으면 코드는 라이브러리로 보내고 hash만 기록 | `RE/Private/Materials/Material.cpp:2684–2720`, `RE/Private/Materials/MaterialShared.cpp:1514–1590`, `RC/Private/ShaderMap.cpp:309–331` |
| 실행 시 로드 | 라이브러리에서 hash로 코드를 읽거나 인라인 코드 | `RC/Private/ShaderMap.cpp:344–372` |
| PSO 미리 준비 | 셰이더 맵이 준비된 뒤 vertex factory마다 PSO 초기화 정보를 모은다 | `MaterialShared.cpp:3315–3348` |

### 12.2 번역 너겟

번역 처리기(MaterialCook)의 너겟이다. AssetSystem 9절 공통 규칙을 따른다.

| AssetSystem 9절 항목 | 번역 너겟 |
|---|---|
| 형식 버전, 처리기 Guid·버전 Guid | 번역 처리기. IR·생성 규칙이 바뀌면 처리기 버전 Guid를 바꾼다 |
| 외부 도구 | 없다(번역은 도구를 쓰지 않는다) |
| 결정적 수학 버전 | 상수 접기(6.5절)가 쓰므로 넣는다 |
| 툴체인 지문 | 공통 규칙대로 넣는다 |
| 에셋 TypeId | `Material` 또는 `MaterialInstance`(static 변형) |
| 입력 bulk | 그래프 semantic hash(5.5절, 파라미터 기본값 제외) |
| 설정 전체 | 기본 속성(`MaterialTraitsDesc`, 인스턴스 사슬 override를 합친 값), 정규화한 static 값 목록(사슬 합침), 품질 수준(기본과 같으면 "기본" 표지, 8.5절), 노드 타입별 (NodeTypeId, schemaVersion, lowering 버전) 정렬 목록(그래프에 쓰인 것만), `MaterialCommon.rsh` 계약 버전. 사용 대상은 넣지 않는다(8.4절) |
| 대상 프로필과 출력 설정 | 프로필 id·출력 설정 전체(공통 규칙). 여기서 번역이 실제로 쓰는 것은 프로필 형식들의 기능 등급 집합과 공통 상한이다 |
| 의존 입력 | (역할, AssetId, 의존 키) 정렬 목록. 의존 키는 의존 에셋 종류마다 정한다: 함수 = SHA-256(함수 그래프 semantic hash, 그 함수의 의존 키 목록), 함수 인스턴스 = SHA-256(override 정규 바이트, 부모 함수 키), 컬렉션 = layout 서명. static 변형은 루트 머티리얼(역할 Root)의 그래프 semantic hash와 의존을 넣고, 중간 인스턴스의 정체성은 넣지 않는다. AssetSystem 9절은 "같은 처리기 build key"를 쓰므로 이것은 그 일반화다(16절 MAS6) |

값(`.rnug` 부분):
1. 생성 원문(`/Engine/Generated/Material.rsh` 텍스트)
2. 상수 블록 layout 표와 서명
3. 상수 식 프로그램
4. 텍스처 slot 목록(종류·샘플 종류·샘플러 출처·논리 크기 사용·전체 상주 전용 표지)과 slot ↔ `ParameterKey` 대응. 기본값과 기본 텍스처 AssetId는 넣지 않는다(쿠킹 처리기가 그래프에서 읽는다, 4.5절)
5. `MaterialShaderTraits`
6. 텍스처 sampling scale 표(12.5절)
7. 원문 위치 지도(7.6절)
8. 진단·통계(IR 값 수·텍스처·보간·상수 블록 크기)
9. IR hash

### 12.3 셰이더 job

- MaterialCook이 8.2절 축으로 job 목록을 만든다: 통과한 (static 변형, 품질 수준, pass 셰이더 타입, 퍼뮤테이션, 정점 입력 타입, 형식)의 단계마다 셰이더 시스템 job 하나다. 정점 입력 타입은 선언 ∪ 유도 사용 대상(8.4절)이다.
- job 키는 셰이더 시스템 10절 그대로다. 생성 원문은 전처리 결과(7항)에 들어가고, 머티리얼 layout 서명은 8항에 들어간다.
- 같은 pass 원문·같은 생성 원문이면 머티리얼이 달라도 job 키가 같아 너겟을 공유한다.

### 12.4 쿠킹 데이터

| 위치 | 내용 |
|---|---|
| 머티리얼 패키지(쿠킹) | `MaterialArtifact` 목록(품질 수준별): 셰이더 집합 id, `MaterialShaderTraits`, layout 표·서명, 상수 식 프로그램, 평탄화 기본값 배열, 텍스처 slot 목록과 기본 텍스처 Hard 참조, 컬렉션 Hard 참조, sampling scale 표 |
| 인스턴스 패키지(쿠킹) | 부모 Hard 참조, 평탄화 값 배열(부모 layout 서명과 함께). static 변형이면 자기 `MaterialArtifact` |
| 프로젝트 셰이더 라이브러리(형식마다) | 머티리얼 셰이더 항목(8.6절 조회 키 → 코드 SHA). 셰이더 시스템 12.3절 프로젝트 라이브러리이며 묶음 id 열(RenderBase SX5)로 셰이더 집합을 묶는다 |
| 쿠킹 보고서 | 머티리얼별 퍼뮤테이션 수와 증감(8.5절) |

셰이더 코드는 패키지에 넣지 않는다. 패키지는 셰이더 집합 id만 가진다. UE의 "라이브러리가 켜졌을 때 hash만 기록"과 같은 구조를 항상 쓴다.

**쿠킹 품질 수준.** TargetProfile에 쿠킹할 머티리얼 품질 수준 목록을 둔다(기본 하나, 16절 AS4). 실행 중 품질 전환은 쿠킹한 수준 안에서만 된다.

### 12.5 텍스처 sampling scale(TextureStreaming T4)

TextureStreaming 결정 T4는 "머티리얼 컴파일러가 생기면 셰이더 빌드 결과로 공급"한다고 남겼다. Rocks는 IR 분석으로 정한다.
- 텍스처 샘플의 UV 식이 `TexCoord(i) × k + b`(k·b가 `Constant` 또는 `Static`)이면 (채널 i, scale |k|의 최대 성분)이다.
- k가 `Uniform`이면 기본값으로 계산한 scale과 "파라미터 의존" 표지를 둔다. 실행 중 값이 바뀌면 렌더 상태가 상수 식으로 다시 계산해 스트리밍에 알린다(측정 뒤 필요 여부 결정, 후속).
- 그 밖(world 좌표 UV·임의 식)은 "알 수 없음"이다. 텍스처 샘플 노드의 명시 scale 속성을 쓰고, 없으면 TextureStreaming의 보수적 상한 규칙(계획 9.5절 "동적 UV")을 따른다.
- 출력: 텍스처 slot마다 (UV 채널, scale, 출처: 정적·파라미터·명시·알 수 없음).
- UE는 에디터에서 debug view 셰이더로 그려 재서 `TextureStreamingData`에 저장한다(`RE/Public/Materials/MaterialInterface.h:200–211, 422`, `RE/Private/Materials/MaterialInterface.cpp:2453`, `RE/Private/DebugViewModeHelpers.cpp:337`). 측정 경로의 세부는 미확인이다. Rocks는 GPU 없이 결정적으로 계산할 수 있는 IR 분석을 먼저 쓴다.

### 12.6 실행 시 로드

1. AssetRuntime이 머티리얼 패키지를 로드한다(CPU ready). 텍스처·컬렉션·부모는 Hard 참조로 함께 로드된다.
2. `Material` 모듈이 `MaterialArtifact`로 `MaterialRenderState` 생성 요청을 렌더 lane에 보낸다(Render ready 단계, AssetSystem 3절 "Render ready 계약").
3. 렌더 lane의 MaterialRuntime이 셰이더 라이브러리에서 그 셰이더 집합 id 항목을 연다. 장치 셰이더는 처음 쓸 때 만든다(RenderBase 9.2절).
4. 열 때 layout 서명을 대조한다. 다르면 `IncompatibleCook`이고 기본 머티리얼로 대체한다(11.3절).
5. 그리기에서 (pass, 정점 입력 타입) 조합이 집합에 없으면 그 조합만 기본 머티리얼로 대체한다(11.3절).
6. 인스턴스는 부모 렌더 상태의 셰이더 집합을 공유하고 자기 상수 블록을 가진다.

**PSO 미리 준비**는 후속이다. RenderBase 9.7절 `PipelinePrewarm`에 (머티리얼 셰이더 집합, 정점 입력 타입, pass) 목록을 넘기는 형태로 둔다(16절 MRB6). UE는 셰이더 맵이 준비된 뒤에만 PSO를 모은다(12.1절).

### 12.7 결정성

- 번역 너겟 값과 쿠킹 머티리얼 패키지는 같은 도구 빌드에서 호스트와 무관하게 같은 바이트다(AssetSystem 10절). 6.6절 규칙과 상수 접기 규칙(6.5절)이 이것을 보장하고, 패키지에 들어가는 셰이더 집합 id는 번역 결과 내용 hash라 호스트별 값이 섞이지 않는다(8.6절).
- 셰이더 job 결과의 호스트 사이 동일성은 셰이더 시스템 10절 측정(S3)을 따른다.
