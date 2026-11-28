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
| 입력 bulk | 그래프 semantic hash(5.5절, 파라미터 기본값·기본 텍스처 AssetId 제외) |
| 설정 전체 | 기본 속성(`MaterialTraitsDesc`, 인스턴스 사슬 override를 합친 값), 정규화한 static 값 목록(사슬 합침, 키 순), 번역 대상 `MaterialTranslationTarget` 정규 바이트 전체(품질·형식 사실·샘플러 상한·프로필 기능 — 12.8절), custom 속성 표 서명(등록 순서 무관 정렬), 노드 타입별 (NodeTypeId, schemaVersion, lowering 버전) 정렬 목록(루트 그래프와 펼친 함수·레이어 그래프 전부에 쓰인 것), `MaterialCommon.rsh` 계약 버전. 사용 대상은 넣지 않는다(8.4절) |
| 대상 프로필과 출력 설정 | 프로필 id·출력 설정은 넣지 않는다 — 번역이 읽는 프로필 값은 모두 번역 대상 바이트에 들어간다(텍스처 너겟이 결과에 닿지 않는 칸을 고정하는 것과 같은 규칙) |
| 의존 입력 | (역할 문자열, AssetId, 의존 키) 정렬 목록(역할: Function·Material·ColorCurve·CurveAtlas·Collection·Texture). 의존 키: 함수 = SHA-256(함수 그래프 semantic hash, usage, 표시 이름, 그 함수의 의존 목록 — 중첩 의존이 재귀로 든다), 함수 인스턴스 = SHA-256(override 정규 바이트, 부모 함수 키), MaterialSample 대상 머티리얼 = (그래프 semantic hash, 표시 이름 — snapshot에 기본 속성이 없어 번역이 읽지 않는다), 컬렉션 = layout 서명(base 사슬 포함), 색 커브 = 커브 내용 정규 바이트의 SHA-256(네 채널 키·조정), 커브 atlas = 행 AssetId 목록의 SHA-256, 텍스처 = (참조한 노드 id·속성 경로, AssetId, `MaterialTextureFacts` 정규 바이트) — 파라미터 노드의 기본 텍스처 필드만 AssetId 없이 사실로(기본 텍스처를 사실이 같은 텍스처로 바꾸면 적중한다, MS3d1 정정). static 변형은 루트 머티리얼(역할 Root)의 그래프 semantic hash와 의존을 넣고, 중간 인스턴스의 정체성은 넣지 않는다. AssetSystem 9절의 "같은 처리기 build key"를 일반화한다(16절 MAS6). **구현(MS3c)**: 재귀 키 대신 모은 닫힘 전체를 (역할, AssetId) 순 평평한 목록으로 쓴다 — 함수의 함수·커브까지 같은 목록에 들어 무효화 결과가 같다. 텍스처 역할은 목록에서 빼고 참조 위치 목록(그래프 asset·노드 id·필드 순 정렬)이 대신한다 |

값(`.rnug` 부분):
1. 생성 원문(`/Engine/Generated/Material.rsh` 텍스트)
2. 상수 블록 layout 표와 서명
3. 상수 식 프로그램
4. 텍스처 slot 목록(출처·종류·샘플러 출처)과 파라미터 출처(`ParameterKey`·이름 — 12.8절 MS3d2). 기본값과 기본 텍스처 AssetId는 넣지 않는다(쿠킹 처리기가 노드 hook으로 그래프에서 읽는다, 4.5절). 샘플 종류는 사용마다 Texture 값에 있고, 논리 크기 사용은 프로그램에서 유도하고, 전체 상주 표지는 MS4다
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
| 머티리얼 패키지(쿠킹) | 머티리얼 파라미터 표(`ParameterKey`·이름·기본값 — 인스턴스 평탄화의 바탕), `MaterialArtifact` 목록(번역 결과(artifact 정규 byte)가 같으면 한 항목, (형식 id, 품질) → 항목 번호 표): 셰이더 집합 id, `MaterialShaderTraits`, layout 표·서명, 상수 식 프로그램, 파라미터·텍스처 slot(텍스처·컬렉션은 Material의 Hard 참조 필드 번호), sampling scale 표(MS4) — 12.8절 MS3d2 |
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

### 12.8 MS3 구현 세부(묶음)

MS3(15.2절)은 다섯 묶음으로 나눈다. 묶음마다 교차 리뷰(두 명)를 거친다.

| 묶음 | 내용 | 막는 선행 |
|---|---|---|
| MS3c | `MaterialCook` 모듈, `MaterialShaderTraits`, 참조 동기화(`referencedTextures` — 4.2절), 번역 입력 모으기, 번역 너겟(키·값·bucket), 셰이더 집합 id | 쿠킹 문맥에서 Build 의존 열기(16절 MAS7), 형식 정의 `platformClasses`(MSR18 — 반영) |
| MS3d | `MaterialArtifact`(품질 × 형식 묶음)와 Material 쿠킹 처리기, 원본 없이 쿠킹 데이터만으로 로드. static 아닌 `MaterialInstance`·함수·컬렉션은 MS6까지 기본 처리기다. 엔진 기본 머티리얼 에셋은 MS4(렌더 연결)에서 만들고 그때 늘 쿠킹 목록에 넣는다 | MS3c |
| MS3e | 셰이더 job 목록(8.2절 축)과 퍼뮤테이션 보고(8.5절), 머티리얼별 job 상한 | MS3d. 조합은 `EnumerateMaterialShaderCombinations`(MSR19 반영), traits 해석은 RendererShaders RS0 |
| MS3f | 머티리얼 셰이더 컴파일·프로젝트 셰이더 라이브러리 항목(묶음 id), 형식별 텍스처 예산 오류(MS-T10) | 셰이더 시스템 MSR17(반영 — 등록부 원천 `MaterialShaderBundleSource`)·`warningsAsErrors`(7.8절) |
| MS3g | 사용 대상 유도(8.4절, MS-T11), 부분 쿠킹 시험(함수 변경의 역의존 — 역방향 Build 확장은 쿠커에 이미 있다) | MS3e |

**pass 셰이더 타입의 출처(3.2절 정정).** 쿠커가 훑는 pass 셰이더 타입은 ShaderCore 셰이더 타입 등록부에서 `ShaderTypeMaterialShader` 표지를 단 타입이다(셰이더 시스템 12.2절 — `RendererShaders`가 그 표지로 등록한다). 그래서 `MaterialCook`은 `RendererShaders`를 링크하지 않고 등록부 원천(`ShaderCookRegistrySource` — 엔진 라이브러리 단계와 같은 원천)만 본다. 아직 머티리얼 셰이더 타입이 없으면 job 목록이 빈다(보고에 0). 시험은 `ShaderTypeMaterialShader` 표지를 단 `ShaderTypeDesc`(엔트리 `MaterialTestPass.rsf` — 7.8절)를 시험 등록부에 등록해 쓴다.

**MS3c.**
- **모듈.** `Engine/Source/Developer/MaterialCook`(3.2절 표). 의존: Material·MaterialCompiler·MaterialSchema·AssetCooker·AssetNugget·AssetRuntime·Texture·CurveAssets·CurveAtlas·ShaderCore(묶음 id `shader::MaterialShaderBundleId`), MS3d부터 ShaderCompiler(묶음 원천 `MaterialShaderBundleSource` — 수명 객체 Startup에서 `MaterialShaderBundleSources().Register`, Shutdown에서 Unregister). RendererShaders는 링크하지 않는다(MRD18). Release 링크 금지.
- **`MaterialShaderTraits`**(MaterialSchema, 8.3절 표). 번역 결과에서 `MakeMaterialShaderTraits`(MaterialCompiler — 번역 결과·기본 속성·품질)로 만든다. 정규 바이트(`WriteMaterialShaderTraits` — 형식 버전 머리, 필드 순서 고정, little-endian, 이름은 UTF-8 문자열)는 셰이더 집합 id와 너겟 값에 들어간다. requiredCapabilities는 번역이 기록한 문맥 기능 집합(19.3절)과 6.4절 capability 비트다.
- **참조 동기화.** `Material`에 `referencedTextures : ObjectRefArray<Texture>`(Hard — UE `FMaterialCachedExpressionData::ReferencedTextures`)를 둔다. 그래프와 그 함수·레이어 닫힘이 참조하는 텍스처(텍스처 노드·파라미터 기본 텍스처·atlas)를 모은 정렬 집합이다. `parameterCollections`도 같은 방식으로 그래프 컬렉션 노드에서 채운다. 채우는 함수 `SyncMaterialReferences`(MaterialCook — 원본 공급으로 함수 닫힘을 펼친다)는 에디터 컴파일(MS5)·저장 명령이 부르고, 쿠킹은 번역 결과의 텍스처·컬렉션이 이 집합 안에 있는지 대조해 빠졌으면 쿠킹 실패다("다시 저장" 진단 — 쿠킹 대상 확장과 cookKey가 이 Hard 간선에 기대므로). 이로써 그래프 텍스처가 쿠킹되고, 텍스처 설정 변경이 Hard 의존 콘텐츠로 머티리얼 cookKey에 든다.
- **번역 입력 모으기**(`MaterialCookInputs`). 쿠킹 대상 머티리얼에서 Build 의존(4.6.1절 hook — 함수·MaterialSample·레이어·커브·atlas)을 전이로 열고(MAS7), Hard 참조(텍스처·컬렉션과 컬렉션 `base` 사슬)는 패키지 열기가 이미 연다. 그 결과로:
  - 원본 공급(`MaterialSourceProvider` 구현): 함수·MaterialSample 그래프 snapshot, 색 커브 → `MaterialColorCurveSnapshot`, 커브 atlas → `MaterialCurveAtlasSnapshot`.
  - 텍스처 사실: `referencedTextures`마다 `MaterialTextureFacts`(Texture 에셋 필드 그대로 — MaterialTextureFacts.h 주석의 "키에 넣지 않는다"는 고친다: V10·주소 모드가 결과를 바꾼다).
  - 컬렉션 layout: 컬렉션마다 `MaterialCollectionLayout`.
  - custom 속성 표: 등록된 표(`MaterialAttributeTable`)와 그 서명.
  - 의존 열기(opener)가 없으면 모으기는 InvalidArgument다(열린 패키지만으로 모으면 참조 동기화가 필드를 지운다 — 에디터도 열린 패키지 집합 위 opener를 준다). 컬렉션 base 순환·함수 인스턴스 사슬이 상한(`MaxReferenceChain`)을 넘으면 그 참조는 열지 못한 것으로 둔다(번역이 진단한다).
  - 번역 뒤 `MaterialTranslation.dependencies`가 모은 집합의 부분집합인지 대조한다. 아니면 쿠킹 실패다(hook 결함을 잡는다). 의존 열기가 원본 재구성 hook으로 의존 패키지를 dirty로 만들어도 쿠킹은 메모리 값을 쓰고(쿠커 규칙 — 저장하지 않는다), 번역 키도 같은 메모리 값에서 만들므로 일관된다.
- **번역 대상.** 품질 목록은 프로필 필드(MAS4)이고, 없으면 `[None]`(기본 입력 하나)이다. 형식 사실은 형식 정의에서 온다: `isMobile`·`isPc`·`isConsole`은 형식 사실 `platformClasses`(MSR18 — 집합 `ShaderPlatformClassBits`, 셋이 서로 독립이다. `GLES31_PC`는 isPc·isMobile 둘 다 참), `maxSamplers`는 형식 `limits.samplers`다. bindless·distance field·ray tracing·virtual texture·virtualized geometry·굽기 표지·모바일 흉내는 프로필 필드가 생길 때까지 UE 기본값(끔)으로 고정한다. 번역 키에는 대상 바이트 전체를 넣는다(결정적·단순). 공유는 결과로 정한다: 같은 실행 안에서 다른 대상의 `consultedTarget` 칸 값이 같으면 번역을 다시 하지 않고 그 결과를 자기 키로 Put하고, 결과가 같으면 셰이더 집합 id가 같아 artifact·셰이더가 하나다(8.5절 품질 공유와 같은 효과 — 6.9절 ContextAndSupply의 "consultedTarget 항목만 키에"를 이것으로 고친다).
- **키 세부(MS3c 리뷰 반영).** custom 속성 표 서명은 번역에 넘기는 표(`MaterialTranslationKeyDesc::attributes`, 없으면 내장 표만 — "builtin")의 등록 정의를 id 순으로 쓴다(id·이름·핀 형·기본값·빈도·숨김·custom 출력). static 값은 parameterId 순이고 같은 parameterId가 둘이면 InvalidArgument, Bool은 값만·ComponentMask는 mask만 쓴다(정규형). 키·모으기·hook은 같은 NamePool을 써야 한다(카탈로그 풀과 Registry 풀이 다르면 InvalidArgument). 노드 종류 목록은 쓰인 Name 집합을 한 번 모아 고른다. 플러그인 노드의 정의(lowering 버전·참조 선언)는 플러그인 카탈로그 연결(ModuleLoader 단계) 때 더한다 — 그 전에는 엔진 노드만 키에 든다.
- **번역 너겟.** 에셋 너겟이 아니라 머티리얼 번역 너겟이다(키에 AssetId가 없다 — static 변형 공유, 8.5절. 셰이더 job 너겟처럼 평평한 키를 Record 키로 쓴다, 16절 MAS6). bucket `material_translation`(소문자 규칙 — UE 버킷 이름 `MaterialTranslation`과의 대응은 12.1절 표). 키 머리는 `WriteNuggetKeyHeader`(정체성 `MaterialTranslationNuggetId`, 결정적 수학 버전, 툴체인 지문 — 머리가 이미 쓴다)이고 본문은 12.2절 표 순서다(키 배치 버전 먼저).
- **값.** part 0 머리(형식 버전, part 수, 진단 개수), part 1–9는 12.2절 목록 순서, part 6 sampling scale은 12.5절 표가 생기는 MS3d까지 빈 목록, part 8 통계는 지금 IR 값 수만이다. part 10 진단 전체(경고·정보 — 적중 때도 같은 경고를 재생한다, 텍스처 쿠킹과 같다), part 11 `userSceneTextures`, part 12 `consultedTarget`. 이름(`ParameterKey`·Name)은 NamePool 번호가 아니라 UTF-8 문자열로 쓴다. 루트 그래프의 진단은 asset을 비워 저장하고 재생할 때 쿠킹 대상 id를 붙인다(공유 키의 값에 먼저 쿠킹한 에셋 id가 남지 않게 — 함수 진단은 함수 id 그대로, 그 함수는 의존 키에 든다). 기본값과 기본 텍스처 AssetId는 넣지 않는다 — 적중이면 처리기가 그래프(함수·atlas 포함)에서 다시 모은다(미적중 때 IR 기본값과 같은지 시험이 본다). 해석은 형식 버전·part id·part 수·크기·열거 범위와 의미(IrType 성분·행 ≤ 4, 피연산자 번호 < 자기 명령 번호, 멤버 명령 번호 < 명령 수, 멤버 float4 번호 < float4 수, 첫 성분 + 성분 수 ≤ 4, 텍스처 명령의 slot 번호 < 텍스처 수)가 맞지 않으면 손상(적중을 미적중으로), 할당 실패는 그 오류다. 실패하면 out 내용은 정해지지 않는다. 번역이 실패한 결과는 Put하지 않는다. `verifyNuggets`면 다시 만든 값과 대조한다(다르면 NuggetMismatch).
- **셰이더 집합 id.** SHA-256(셰이더 집합 형식 버전, 생성 원문, layout 서명, traits 정규 바이트)이다(8.6절). 라이브러리 묶음 id(u64)는 ShaderCore `shader::MaterialShaderBundleId`(앞 8 byte little-endian, 0이면 1 — 0은 엔진 묶음)이고 MaterialCook은 따로 두지 않는다. 한 라이브러리에서 다른 집합 id가 같은 묶음 id면 쿠킹 오류다(셰이더 라이브러리 단계의 Collision 진단, MS3f).
- **실패와 경고.** 번역 오류가 하나라도 있으면 그 머티리얼 쿠킹 실패다: `AssetDiagnosticCode::MaterialTranslationFailed`(16절 MAS8), detail = 첫 오류의 `MaterialDiagnosticCode`, related = 진단 asset(함수일 수 있다). 노드·문구는 쿠킹 보고서(경고 목록과 같은 자리)로 간다. 경고는 CookContext warnings로 옮긴다.
- **MS3d로 넘기는 것.** 진단 재생·의존 부분집합 대조·적중 기본값 = 미적중 IR 기본값 시험은 처리기(MS3d)와 함께 한다. 값에 파라미터 ParameterKey(이름·association — 같은 함수를 두 레이어에서 쓰면 id만으로 가를 수 없다)와 텍스처 slot의 샘플 종류·논리 크기 사용·전체 상주 표지를 더하는 일도 MS3d다(값 형식 버전 2).
- **시험.** MS-T1(같은 그래프 두 번·저장 순서만 섞음 → 같은 키·같은 값 byte), MS-T5 번역 부분(그래프·함수 그래프·컬렉션 layout·static 값·노드 lowering 버전·번역 대상·custom 속성 표·커브 내용·atlas 행·텍스처 사실을 하나씩 바꾸면 키가 달라짐, 파라미터 기본값·기본 텍스처를 사실이 같은 텍스처로·화면 데이터·사용 대상만 바꾸면 같음), 값 왕복, 손상 값의 미적중, 진단 재생(적중·미적중 같은 경고), 의존 부분집합 대조 실패, 참조 동기화 누락 실패, 셰이더 집합 id가 같은 결과의 다른 입력에서 같음, 적중 때 모은 기본값이 미적중 IR 기본값과 같음.

**MS3d(설계 — 개정 41, 교차 리뷰 반영).** 다섯 묶음으로 나눈다(묶음마다 교차 리뷰 두 명).

| 묶음 | 내용 |
|---|---|
| MS3d1 | 노드 파라미터 값 hook(UE `UMaterialExpression::GetParameterValue`), 파라미터 충돌 진단, 번역의 파라미터 출처, 번역 키의 텍스처 위치 정정 |
| MS3d2 | 번역 값 형식 버전 2(파라미터 출처·컬렉션), 런타임 형(`MaterialParameterInfo`·`MaterialArtifact`·`MaterialFormatArtifact`·상수 식 명령)과 변환 |
| MS3d3 | Material 쿠킹 처리기(대상·너겟·공유·진단 재생·의존 대조·파라미터 표·artifact·셰이더 묶음 Record), AssetCooker 개정 MAS9·MAS11 |
| MS3d4 | 머티리얼 셰이더 묶음 원천(`MaterialShaderBundleSource`)과 실행 범위 연결 |
| MS3d5 | 쿠킹 데이터만으로 로드 시험과 MS3c에서 넘긴 시험 |

- **MS3d1 파라미터 값 hook.** UE는 파라미터 기본값·이름·그룹을 번역이 아니라 노드마다 `GetParameterValue`로 모으고, 같은 파라미터가 다른 값으로 두 번 오면 오류다(`FMaterialCachedExpressionData::UpdateForExpressions`·`TryAddParameter` — `RE/Private/Materials/MaterialCachedData.cpp:173–219, 474–535`). Rocks도 노드 정의에 hook을 둔다.
  - `MaterialParameterMetadata`(MaterialSchema, 빌린 값 — 문자열은 hook에 준 reader 버퍼를 가리키고 그 호출 동안만 유효하다. UE `FMaterialParameterMetadata`): `type`(`MaterialParameterType` — UE 5.8 `EMaterialParameterType` 순서 Scalar·Vector·DoubleVector·Texture·TextureCollection·Font·RuntimeVirtualTexture·SparseVolumeTexture·StaticSwitch·ParameterCollection, 그 뒤 UE의 NumRuntime 밖 StaticComponentMask, Rocks 고유 StaticEnum·DynamicBool — `RE/Public/Materials/MaterialParameters.h:186–207`), `parameterId`, `name`, `group`, `sortPriority`, `primitiveDataIndex`(UE `PrimitiveDataIndex` — custom primitive data 파라미터는 IR 파라미터를 만들지 않으므로 에디터가 이 칸으로 안다, 없으면 −1), 수 기본값 `defaults[4]`(double), 텍스처 기본값 `texture`(AssetId), 커브 atlas 행이면 `atlasCurve`·`atlas`(AssetId — UE `bUsedAsAtlasPosition`·`ScalarCurve`·`ScalarAtlas`).
  - `MaterialNodeDefinition::ParameterValue(fn)` — `Result<bool> (*)(NodePropertyReader&, MaterialParameterMetadata&)`(reader의 Read는 non-const다). 파라미터 노드만 hook을 가진다. 선행이 없는 파라미터 노드(RVT·SVT·텍스처 컬렉션·font)도 hook을 두지만 그 노드는 `PrerequisiteMissing`이라 번역·쿠킹에 닿지 않는다.
  - **기본값 해석 하나.** `ResolveParameterDefaults(metadata, provider, out)`(MaterialCompiler 공개)가 hook 결과를 IR 기본값으로 바꾼다(atlas 행 번호는 atlas snapshot으로). 함수 인스턴스 override(UE `GetParameterOverrideValue` — `MaterialCachedData.cpp:504–516`)는 MS6이 이 함수 안에 더한다. lowering과 처리기는 이 함수만 쓴다 — 적중 때 처리기가 모은 기본값이 미적중 IR 기본값과 구조로 같다.
  - **파라미터 충돌(UE `TryAddParameter`).** 같은 IR 정체로 두 번째 파라미터 노드를 lowering할 때 hook 결과의 type·기본값이 첫 노드와 다르면 번역 오류 `ParameterConflict`(V8 옆 새 규칙 — 문구 UE "Parameter %s is defined with different values")다. 그래서 처리기가 어느 노드를 고르든 같은 값이다.
  - **번역의 파라미터 출처** `IrParameterOrigin`: 수 파라미터·파라미터 텍스처마다 소유 에셋 Guid, `parameterId`, scope, 이름 문자열을 모듈이 기록한다.
    - 소유 에셋: 파라미터 노드를 담은 그래프의 에셋이다. 함수 인스턴스를 거친 함수는 호출된 에셋(인스턴스 id — IR 정체와 같은 기준, 9.2절도 고친다), MaterialSample로 펼친 머티리얼은 그 머티리얼, 루트 그래프는 null(처리기가 루트 Material id로 채운다 — static 변형도 루트 Material이다).
    - callPath: 루트부터 그 그래프까지의 원본 호출 노드 id(FunctionCall·MaterialSample·레이어 노드 — 사본·합성 id가 아니다, 바깥 → 안). "공유" 파라미터는 빈 경로다.
    - scope 종류: 가장 안쪽 레이어 합성 호출 안이면 Layer·LayerBlend와 그 레이어 Guid(UE는 레이어 연관을 앞세운다 — `MaterialCachedData.cpp:463–472`; "공유"여도 레이어 연관은 남는다), 아니면 callPath가 있으면 FunctionCall, 없으면 Global.
    - IR 정체 Guid와 출처는 일대일이다(정체 hash의 호출 사본 id가 위 경로 전체를 담는다). 시험이 중첩·레이어·MaterialSample·인스턴스 경유를 본다.
  - **번역 키 정정(MS3c 결함).** 파라미터가 아닌 텍스처 참조 위치는 사실과 함께 AssetId를 키에 넣는다 — 모듈 텍스처 표가 AssetId로 slot을 합치고 정렬하므로(`MaterialIrModule.cpp:308–360`) AssetId가 빠지면 slot 수·순서가 다른 결과가 같은 키가 된다(UE도 `TextureReferencesHash`를 키에 넣는다). "사실이 같으면 적중" 편차는 파라미터 노드의 기본 텍스처 필드에만 남긴다. 그 필드인지는 노드 정의의 Parameter 표지와 hook의 `texture` 필드 이름으로 안다.
- **MS3d2 값 v2와 런타임 형.**
  - 번역 값 형식 버전 2: part 3 파라미터마다·part 4 파라미터 텍스처마다 출처(소유 Guid·parameterId·scope 종류 u8·layer·callPath 목록·이름)를, 새 part 13에 컬렉션(모듈 순 — `MaterialCollection<k>`, AssetId·layout 서명)을 더한다. 해석 의미 검사에 scope 종류 범위를 더한다. 텍스처 slot의 논리 크기 사용은 값에 넣지 않고 변환이 프로그램(TextureSize 명령의 slot)에서 유도한다. 전체 상주 표지(7.4절)는 상주 mip helper(MS4 TextureStreaming 연결)까지 늘 거짓이다(TODO). 샘플 종류는 slot이 아니라 Texture 값 즉시값에 사용마다 있으므로 slot에 두지 않는다. sampling scale(part 6)은 12.5절 IR 분석과 함께 MS4로 넘긴다(TODO — 지금 빈 목록).
  - 런타임 형(실행 시 로드가 읽으므로 Runtime 모듈 — 컴파일러 형을 쓰지 않는다). artifact는 **순수 값**이다(`ObjectRef`는 복사·이동이 막혀 배열 원소로 둘 수 없다 — UE도 텍스처 uniform 식이 `ReferencedTextures` 번호를 가진다, `MaterialUniformExpressions.h:451–458`):
    - MaterialSchema: `MaterialConstantOp`(6.10절 프로그램 연산 집합과 Constant·Parameter·Texture·TextureSize — UE `EPreshaderOpcode` 대응 표를 enum 주석에), `MaterialConstantInstruction`(op·scalar(Float·Double·Texture)·components·operandCount·operands[4]·immediates[4] — 즉시값 부호화는 IR 그대로, Parameter·Texture의 즉시값 [3]은 artifact 파라미터·텍스처 slot 번호, `MaterialConstantProgramVersion`), `MaterialConstantSlot`(칸 종류 Value·DoubleTile·DoubleOffset·float4 번호·첫 성분·성분 수·명령 번호), `MaterialParameterInfo`(`ParameterKey`·이름·그룹·sortPriority·`MaterialParameterType`·`defaults[4]`·기본 텍스처 번호).
    - Material: `MaterialTextureSlot`(출처 Asset·Parameter, 종류, 샘플러 출처 None·Own·SharedWrap·SharedClamp, 파라미터면 `parameters` 번호, `texture` = `Material::referencedTextures` 번호(Hard 간선은 그 필드가 이미 가진다), `logicalSize`, `wholeResident`), `MaterialArtifact`(`shaderSetId`, layout 서명(인스턴스 패키지의 부모 layout 대조 — 12.4절), traits 정규 바이트, float4 수, 상수 칸, 프로그램, 파라미터 번호 목록(모듈 파라미터 표 순 → `Material::parameters` 번호), 텍스처 slot, 컬렉션 = `Material::parameterCollections` 번호 목록(모듈 순), `userSceneTextures`), `MaterialFormatArtifact`(형식 id·quality·artifact 번호 — 품질은 여기에만 둔다, artifact는 품질 사이에 공유된다).
    - `Material` 필드: `parameters : Array<MaterialParameterInfo>`(머티리얼 단위 파라미터 표 — UE cached expression data의 파라미터 목록, `ParameterKey` 순, 모든 artifact의 파라미터 합집합. 인스턴스 override 해석(MS6)이 이 표를 본다), `artifacts`, `formatArtifacts`. 에디터 패키지에서는 빈다 — 처리기가 쓰기 동안만 채운다. 새 `ROCKS_PROPERTY`는 `.rocksids.json`에 등록하고, Material 클래스 기본값 지문이 바뀌어 머티리얼 전체가 다시 쿠킹된다.
    - 12.4절 표의 "평탄화 기본값 배열"은 이 파라미터 표다(인스턴스의 평탄화 배열은 MS6).
  - 변환 `MakeMaterialArtifact`(MaterialCook): IR 연산 → `MaterialConstantOp`(집합 밖은 InvalidArgument — 상수 빌더 계약 위반), IR 텍스처 종류·샘플러 → 런타임 enum.
- **MS3d3 처리기** `MaterialCookProcessor`(MaterialCook, `TypeIdOf<Material>()` — `RegisterMaterialCookProcessor(state, table)`). 등록 state(`MaterialCookServices` — 아래)가 카탈로그, 알려진 형식 정의 전부의 형식 사실(`Span<const ShaderFormatFacts>` — 등록 때는 프로필을 모르므로 Cook이 `context.profile`의 형식을 이 표에서 고른다), custom 속성 표(null이면 내장)를 준다.
  - **정체·cookKey.** 처리기 버전 Guid는 artifact 배치·처리 규칙이 바뀌면 올린다. `WriteSettingsKey`는 패키지 cookKey에 번역 결과를 바꾸지만 패키지 콘텐츠·SCC에 없는 입력을 쓴다: 엔진 노드 정의 서명(타입·schema·lowering 버전 전부), custom 속성 표 서명, 알려진 형식 사실 전부의 digest, `MaterialCommonContractVersion`·상수 프로그램·traits·번역 값·셰이더 집합 형식 버전. 그러면 형식 정의나 lowering만 바뀌어도 패키지가 재사용되지 않는다.
  1. 대표 에셋이 `Material`이 아니면 TypeMismatch, `context.dependencies`가 null이면 InvalidArgument.
  2. 모으기 → 참조 동기화 대조. 빠졌으면 `MaterialReferencesOutOfSync`(오류, related = 빠진 AssetId — "다시 저장").
  3. 번역 대상: 품질 목록은 MAS4 전까지 `[None]`(TODO MAS4), 형식은 프로필 `graphicsBackends` 순 각 backend의 `shaderFormats` 순이다. 형식 사실이 표에 없으면 InvalidArgument. 형식이 하나도 없으면(전용 서버) 번역하지 않고 빈 artifact로 쓴다. 대상 = `MakeMaterialTranslationTarget(quality, facts)`(isMobile·isPc·isConsole = platformClasses 비트, maxSamplers = limits.samplers, 나머지 끔).
  4. 대상마다: 앞선 대상의 결과 중 `consultedTarget` 칸이 모두 같은 것이 있으면 그 값을 쓰고 자기 키로 Put한다(번역 안 함 — `verifyNuggets`면 번역해 byte 대조). 아니면 키 → Get(bucket `material_translation`) → 해석. 미적중·손상·verify면 번역 → 생성 원문 → 상수 → traits → 값, verify면 byte 대조(다르면 NuggetMismatch), 성공 값만 Put. **artifact·진단 재생은 늘 값(해석본)에서 만든다**(적중·미적중이 같은 길 — MS-T1 byte 동일).
  5. 번역 오류면 `MaterialTranslationFailed`(detail = 첫 오류 코드 — MAS11로 보고서 `setting`에 간다). 경고는 `MaterialTranslationWarning`(경고, setting = 진단 코드, related = 진단 asset — 루트면 쿠킹 대상)으로 `context.warnings`에 옮기고, 대상 사이 같은 (asset, node, code, 문구)는 한 번만이다. 노드·문구는 `CookDiagnostic`에 칸이 없으므로 영어 Log(Warning·Error — 카테고리 LogMaterialCook)로 남긴다. Info는 Log만.
  6. 미적중으로 번역했으면 `MaterialTranslation.dependencies` ⊆ 모은 참조인지 대조한다. 밖이면 `MaterialDependencyUndeclared`(오류, related = 그 AssetId — 노드 참조 선언 결함).
  7. 파라미터 표: 값마다 출처로 소유 그래프(루트 또는 모은 snapshot)에서 parameterId가 같은 파라미터 노드를 찾아 hook과 `ResolveParameterDefaults`로 채운다(충돌은 번역이 이미 오류로 냈다 — 못 찾으면 InvalidArgument). 기본 텍스처·참조 텍스처는 `referencedTextures`에서, 컬렉션은 `parameterCollections`에서 번호를 찾는다(없으면 2단계가 이미 실패했다 — InvalidArgument).
  8. artifact: 정규 byte(리플렉션 부호화)가 같은 것은 하나다. 셰이더 집합 id만 같고 프로그램·slot이 다르면(형식 스위치가 uniform 식을 고름) 다른 artifact다. 순서는 대상 순 첫 등장, `formatArtifacts`는 대상 순이다.
  9. 셰이더 묶음 Record: bucket `material_shader_bundles`, 키 = 너겟 키 머리 + 쿠킹 대상 AssetId + 패키지 cookKey(MAS9 `CookContext::cookKey`), 값 = 머리 part와 셰이더 집합마다(형식 id·품질·집합 id·layout 서명·생성 원문·traits 정규 바이트 — 같은 집합 id는 원문·traits를 한 번만), 정점 입력 타입 id 목록. 같은 내용을 실행 안 표(`MaterialCookServices`)에도 넣는다.
  10. 쓰기: 파라미터 표·artifact를 대상 객체에 옮겨 넣고 `WriteCookedPackage`로 쓴 뒤 성공·실패 모두 빈 값으로 되돌린다(CurveTable 처리기 규칙 — 순수 값 배열이라 이동·되돌리기가 할당하지 않는다).
  - **static 변형 `MaterialInstance`**는 MS6이다. 그때까지 static override가 있는 인스턴스는 기본 처리기로 쿠킹되므로, 머티리얼 쿠킹 등록이 인스턴스에 검사 처리기를 두어 `MaterialStaticVariantPending`(경고 — 부모 artifact를 쓴다, related = 부모)을 낸다(조용히 대체하지 않는다).
- **MS3d4 묶음 원천과 실행 범위 연결.**
  - `MaterialCookServices`(MaterialCook): 모듈 수명 원천 `MaterialShaderBundles`를 MaterialCook 모듈 Startup에서 `MaterialShaderBundleSources().Register`, Shutdown에서 Unregister한다. 명령마다 쓰는 것(카탈로그·형식 사실·실행 안 표·다시 만들기 hook)은 실행 범위 연결 `MaterialCookRunBinding`(RAII — 만들 때 원천에 걸고 없앨 때 뗀다, 쿠킹 실행기와 같은 스레드)이 쥔다. 처리기 등록 state도 이 연결이다.
  - `Collect(request, sink)`: `request.packages` 중 type이 Material인 것마다 (asset, cookKey)로 묶음을 찾는다 — 실행 안 표 → Record(Get — `request.nuggets`) → 다시 만들기 hook 순이다. Record는 셰이더 집합마다 (layout 서명, 생성 원문, traits)로 집합 id를 다시 계산해 대조하고, 맞지 않거나 해석이 실패하면 없는 것으로 본다. `request.verifyNuggets`면 늘 다시 만들어 대조한다. 찾은 묶음은 프로필 형식마다 `sink.Add`(형식 id·집합 id·원문·traits·정점 입력 타입·asset)이고, 프로필 형식·품질인데 묶음이 없으면 `MaterialShaderSetMissing`(sink.Report, 오류)다.
  - 다시 만들기 hook(`MaterialShaderSetRebuilder`, 호스트가 에디터 패키지 집합·opener로 구현): 에디터 패키지를 열어 모으기 → 그 대상들 번역 → 셰이더 집합 id 대조. 연결이 없거나 hook이 없거나 id가 다르면 `MaterialShaderSetMissing`과 영어 Log Error다. 다시 만든 수는 보고서 stat로 남긴다(캐시가 꺼졌거나 밀린 반복 쿠킹의 비용이 보이게).
  - 호스트(`asset-cook`)는 ShaderCompiler와 함께 MaterialCook을 Load하고 `MaterialCookRunBinding`을 만든다. Material 타입이 등록됐는데 원천이 없으면 명령 오류다(라이브러리 단계의 "원천 없음 Info"로 조용히 넘어가지 않게).
  - 정점 입력 타입: 선언 `usage`를 정점 입력 타입 id로 바꾸는 표는 RendererShaders RS0가 정점 입력 타입을 등록할 때 생긴다(TODO MS3g·RS0). 그 전에는 빈 목록이라 머티리얼 job이 없다(보고에 0).
- **MS3d5 시험.** 처리기 쿠킹 → 패키지 byte → 쿠킹 로드(EditorOnly 필드 없음)로 파라미터 표·artifact·formatArtifacts가 같고 graph가 비어 있음, 적중·미적중 같은 패키지 byte(MS-T1), 진단 재생(대상 사이 중복 제거), 의존 대조 실패, 동기화 누락 실패, 적중 기본값 = 미적중 IR 기본값(함수 안·레이어 안·MaterialSample 안·함수 인스턴스 경유·atlas 행), 파라미터 충돌 오류, 출처 일대일, 같은 원문·다른 프로그램 → artifact 둘, consultedTarget 공유(두 형식 → artifact 하나)와 verify 대조, 텍스처 AssetId 키(사실 같은 참조 텍스처 교체 → 다른 키, 파라미터 기본 텍스처 교체 → 같은 키), 형식 사실·lowering 버전만 바꿈 → cookKey 다름, 형식 없는 프로필, 묶음 원천(실행 안 표·Record 적중·Record 손상·축출 → 다시 만들기·hook 없음 → Report·프로필 형식 누락 Report·verify), static 변형 경고.

**MS3e traits 거르기(MSR19 반영).** job 목록과 보고는 ShaderCore `EnumerateMaterialShaderCombinations(types, inputs, facts, traits, bundleId, includeDevelopment, out)`로 만든다(셰이더 라이브러리 단계와 같은 열거 — 조합 수가 같다). traits는 정규 바이트(`Span<const uint8>` — ShaderCore는 MaterialSchema를 모른다)로 넘기고 pass 타입(`RendererShaders`)의 `materialShouldCompile`이 MaterialSchema로 해석한다. 해석기(RS0)가 오기 전에는 traits로 거르지 않으므로 보고서에 "traits 거르기 없음"을 남긴다(머티리얼 쪽에서 pass별 규칙을 짐작해 거르지 않는다 — 그 규칙은 pass 타입 소유다).

**UE와 다른 점(12.1절 대응).** UE 번역 키는 셰이더 맵 키 문자열 전체(플랫폼·`TextureReferencesHash`·Usage 포함 — `RE/Private/Materials/MaterialShared.cpp:3547`)다. Rocks는 사용 대상(8.4절)과 기본 텍스처 AssetId를 뺀다(의도한 편차 — 사용 대상은 job 목록만 바꾸고, 기본 텍스처는 사실만 결과에 닿는다). UE는 쓰는 품질 수준만 쿠킹하고 나머지를 기본 하나로 묶는다(`GetQualityLevelUsageForCooking` — `RE/Private/Materials/Material.cpp:2689–2714`). Rocks는 결과 공유로 같은 효과를 낸다(위).

