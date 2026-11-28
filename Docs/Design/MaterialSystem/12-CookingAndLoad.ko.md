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
| 의존 입력 | (역할 문자열, AssetId, 의존 키) 정렬 목록(역할: Function·Material·ColorCurve·CurveAtlas·Collection·Texture). 의존 키: 함수 = SHA-256(함수 그래프 semantic hash, usage, 표시 이름, 그 함수의 의존 목록 — 중첩 의존이 재귀로 든다), 함수 인스턴스 = SHA-256(override 정규 바이트, 부모 함수 키), MaterialSample 대상 머티리얼 = SHA-256(그래프 semantic hash, 기본 속성 바이트, 그 머티리얼의 의존 목록), 컬렉션 = layout 서명(base 사슬 포함), 색 커브 = 커브 내용 정규 바이트의 SHA-256(네 채널 키·조정), 커브 atlas = 행 AssetId 목록의 SHA-256, 텍스처 = AssetId 없이 (참조한 노드 id·속성 경로, `MaterialTextureFacts` 정규 바이트 — 기본 텍스처를 사실이 같은 텍스처로 바꾸면 적중한다). static 변형은 루트 머티리얼(역할 Root)의 그래프 semantic hash와 의존을 넣고, 중간 인스턴스의 정체성은 넣지 않는다. AssetSystem 9절의 "같은 처리기 build key"를 일반화한다(16절 MAS6) |

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
| 머티리얼 패키지(쿠킹) | `MaterialArtifact` 목록(품질 수준 × 형식 묶음별 — 번역 결과가 같으면 한 항목, 형식 id → 항목 번호 표): 셰이더 집합 id, `MaterialShaderTraits`, layout 표·서명, 상수 식 프로그램, 평탄화 기본값 배열, 텍스처 slot 목록과 기본 텍스처 Hard 참조, 컬렉션 Hard 참조, sampling scale 표 |
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
| MS3c | `MaterialCook` 모듈, `MaterialShaderTraits`, 참조 동기화(`referencedTextures` — 4.2절), 번역 입력 모으기, 번역 너겟(키·값·bucket), 셰이더 집합 id | 쿠킹 문맥에서 Build 의존 열기(16절 MAS7), 형식 정의 `platformClass`(MSR18) — 없으면 아래 고정값 |
| MS3d | `MaterialArtifact`(품질 × 형식 묶음)와 Material 쿠킹 처리기, 원본 없이 쿠킹 데이터만으로 로드. static 아닌 `MaterialInstance`·함수·컬렉션은 MS6까지 기본 처리기다. 엔진 기본 머티리얼 에셋은 MS4(렌더 연결)에서 만들고 그때 늘 쿠킹 목록에 넣는다 | MS3c |
| MS3e | 셰이더 job 목록(8.2절 축)과 퍼뮤테이션 보고(8.5절), 머티리얼별 job 상한 | MS3d. traits 거르기는 MSR15(아래) |
| MS3f | 머티리얼 셰이더 컴파일·프로젝트 셰이더 라이브러리 항목(묶음 id), 형식별 텍스처 예산 오류(MS-T10) | 셰이더 시스템 MSR17·`warningsAsErrors`(7.8절) |
| MS3g | 사용 대상 유도(8.4절, MS-T11), 부분 쿠킹 시험(함수 변경의 역의존 — 역방향 Build 확장은 쿠커에 이미 있다) | MS3e |

**pass 셰이더 타입의 출처(3.2절 정정).** 쿠커가 훑는 pass 셰이더 타입은 ShaderCore 셰이더 타입 등록부에서 `ShaderTypeMaterialShader` 표지를 단 타입이다(셰이더 시스템 12.2절 — `RendererShaders`가 그 표지로 등록한다). 그래서 `MaterialCook`은 `RendererShaders`를 링크하지 않고 등록부 원천(`ShaderCookRegistrySource` — 엔진 라이브러리 단계와 같은 원천)만 본다. 아직 머티리얼 셰이더 타입이 없으면 job 목록이 빈다(보고에 0). 시험은 `ShaderTypeMaterialShader` 표지를 단 `ShaderTypeDesc`(엔트리 `MaterialTestPass.rsf` — 7.8절)를 시험 등록부에 등록해 쓴다.

**MS3c.**
- **모듈.** `Engine/Source/Developer/MaterialCook`(3.2절 표). 의존: Material·MaterialCompiler·MaterialSchema·AssetCooker·AssetNugget·AssetRuntime·Texture·CurveAssets·CurveAtlas·ShaderCore. Release 링크 금지.
- **`MaterialShaderTraits`**(MaterialSchema, 8.3절 표). 번역 결과에서 `MakeMaterialShaderTraits`(MaterialCompiler — 번역 결과·기본 속성·품질)로 만든다. 정규 바이트(`WriteMaterialShaderTraits` — 형식 버전 머리, 필드 순서 고정, little-endian, 이름은 UTF-8 문자열)는 셰이더 집합 id와 너겟 값에 들어간다. requiredCapabilities는 번역이 기록한 문맥 기능 집합(19.3절)과 6.4절 capability 비트다.
- **참조 동기화.** `Material`에 `referencedTextures : ObjectRefArray<Texture>`(Hard — UE `FMaterialCachedExpressionData::ReferencedTextures`)를 둔다. 그래프와 그 함수·레이어 닫힘이 참조하는 텍스처(텍스처 노드·파라미터 기본 텍스처·atlas)를 모은 정렬 집합이다. `parameterCollections`도 같은 방식으로 그래프 컬렉션 노드에서 채운다. 채우는 함수 `SyncMaterialReferences`(MaterialCook — 원본 공급으로 함수 닫힘을 펼친다)는 에디터 컴파일(MS5)·저장 명령이 부르고, 쿠킹은 번역 결과의 텍스처·컬렉션이 이 집합 안에 있는지 대조해 빠졌으면 쿠킹 실패다("다시 저장" 진단 — 쿠킹 대상 확장과 cookKey가 이 Hard 간선에 기대므로). 이로써 그래프 텍스처가 쿠킹되고, 텍스처 설정 변경이 Hard 의존 콘텐츠로 머티리얼 cookKey에 든다.
- **번역 입력 모으기**(`MaterialCookInputs`). 쿠킹 대상 머티리얼에서 Build 의존(4.6.1절 hook — 함수·MaterialSample·레이어·커브·atlas)을 전이로 열고(MAS7), Hard 참조(텍스처·컬렉션과 컬렉션 `base` 사슬)는 패키지 열기가 이미 연다. 그 결과로:
  - 원본 공급(`MaterialSourceProvider` 구현): 함수·MaterialSample 그래프 snapshot, 색 커브 → `MaterialColorCurveSnapshot`, 커브 atlas → `MaterialCurveAtlasSnapshot`.
  - 텍스처 사실: `referencedTextures`마다 `MaterialTextureFacts`(Texture 에셋 필드 그대로 — MaterialTextureFacts.h 주석의 "키에 넣지 않는다"는 고친다: V10·주소 모드가 결과를 바꾼다).
  - 컬렉션 layout: 컬렉션마다 `MaterialCollectionLayout`.
  - custom 속성 표: 등록된 표(`MaterialAttributeTable`)와 그 서명.
  - 번역 뒤 `MaterialTranslation.dependencies`가 모은 집합의 부분집합인지 대조한다. 아니면 쿠킹 실패다(hook 결함을 잡는다). 의존 열기가 원본 재구성 hook으로 의존 패키지를 dirty로 만들어도 쿠킹은 메모리 값을 쓰고(쿠커 규칙 — 저장하지 않는다), 번역 키도 같은 메모리 값에서 만들므로 일관된다.
- **번역 대상.** 품질 목록은 프로필 필드(MAS4)이고, 없으면 `[None]`(기본 입력 하나)이다. 형식 사실은 형식 정의에서 온다: `isMobile`·`isPc`·`isConsole`은 형식 정의 `platformClass`(MSR18 — 없으면 GLES·`*_ANDROID`·`*_IOS`를 Mobile, 그 밖을 Pc로 보는 임시 규칙을 MaterialCook 안에 두고 TODO로 표시), `maxSamplers`는 형식 `limits.samplers`다. bindless·distance field·ray tracing·virtual texture·virtualized geometry·굽기 표지·모바일 흉내는 프로필 필드가 생길 때까지 UE 기본값(끔)으로 고정한다. 번역 키에는 대상 바이트 전체를 넣는다(결정적·단순). 공유는 결과로 정한다: 같은 실행 안에서 다른 대상의 `consultedTarget` 칸 값이 같으면 번역을 다시 하지 않고 그 결과를 자기 키로 Put하고, 결과가 같으면 셰이더 집합 id가 같아 artifact·셰이더가 하나다(8.5절 품질 공유와 같은 효과 — 6.9절 ContextAndSupply의 "consultedTarget 항목만 키에"를 이것으로 고친다).
- **번역 너겟.** 에셋 너겟이 아니라 머티리얼 번역 너겟이다(키에 AssetId가 없다 — static 변형 공유, 8.5절. 셰이더 job 너겟처럼 평평한 키를 Record 키로 쓴다, 16절 MAS6). bucket `material_translation`(소문자 규칙 — UE 버킷 이름 `MaterialTranslation`과의 대응은 12.1절 표). 키 머리는 `WriteNuggetKeyHeader`(정체성 `MaterialTranslationNuggetId`, 결정적 수학 버전, 툴체인 지문 — 머리가 이미 쓴다)이고 본문은 12.2절 표 순서다(키 배치 버전 먼저).
- **값.** part 0 머리(형식 버전, part 수, 진단 개수), part 1–9는 12.2절 목록 순서, part 10 진단 전체(경고·정보 — 적중 때도 같은 경고를 재생한다, 텍스처 쿠킹과 같다), part 11 `userSceneTextures`, part 12 `consultedTarget`. 이름(`ParameterKey`·Name)은 NamePool 번호가 아니라 UTF-8 문자열로 쓴다. 루트 그래프의 진단은 asset을 비워 저장하고 재생할 때 쿠킹 대상 id를 붙인다(공유 키의 값에 먼저 쿠킹한 에셋 id가 남지 않게 — 함수 진단은 함수 id 그대로, 그 함수는 의존 키에 든다). 기본값과 기본 텍스처 AssetId는 넣지 않는다 — 적중이면 처리기가 그래프(함수·atlas 포함)에서 다시 모은다(미적중 때 IR 기본값과 같은지 시험이 본다). 해석은 형식 버전·part 수·크기가 맞지 않으면 손상(적중을 미적중으로). 번역이 실패한 결과는 Put하지 않는다. `verifyNuggets`면 다시 만든 값과 대조한다(다르면 NuggetMismatch).
- **셰이더 집합 id.** SHA-256(셰이더 집합 형식 버전, 생성 원문, layout 서명, traits 정규 바이트)이다(8.6절). 라이브러리 묶음 id(u64 — `ShaderLibrary.h`)는 그 앞 8 byte(little-endian)이고, 0이면 1로 둔다(0은 엔진 묶음). 한 라이브러리에서 다른 집합 id가 같은 묶음 id면 쿠킹 오류다(MS3f).
- **실패와 경고.** 번역 오류가 하나라도 있으면 그 머티리얼 쿠킹 실패다: `AssetDiagnosticCode::MaterialTranslationFailed`(16절 MAS8), detail = 첫 오류의 `MaterialDiagnosticCode`, related = 진단 asset(함수일 수 있다). 노드·문구는 쿠킹 보고서(경고 목록과 같은 자리)로 간다. 경고는 CookContext warnings로 옮긴다.
- **시험.** MS-T1(같은 그래프 두 번·저장 순서만 섞음 → 같은 키·같은 값 byte), MS-T5 번역 부분(그래프·함수 그래프·컬렉션 layout·static 값·노드 lowering 버전·번역 대상·custom 속성 표·커브 내용·atlas 행·텍스처 사실을 하나씩 바꾸면 키가 달라짐, 파라미터 기본값·기본 텍스처를 사실이 같은 텍스처로·화면 데이터·사용 대상만 바꾸면 같음), 값 왕복, 손상 값의 미적중, 진단 재생(적중·미적중 같은 경고), 의존 부분집합 대조 실패, 참조 동기화 누락 실패, 셰이더 집합 id가 같은 결과의 다른 입력에서 같음, 적중 때 모은 기본값이 미적중 IR 기본값과 같음.

**MS3e traits 거르기(MSR15 전).** 셰이더 시스템 거르기 서명(`ShaderShouldCompileFunction`)에 traits가 없으므로, 그때까지 job 목록은 타입 거르기(정점 입력·퍼뮤테이션·형식)만으로 만들고 보고서에 "traits 거르기 없음"을 남긴다(머티리얼 쪽에서 pass별 규칙을 짐작해 거르지 않는다 — 그 규칙은 pass 타입 소유다). MSR15는 traits를 정규 바이트(`Span<const uint8>` — ShaderCore는 MaterialSchema를 모른다)로 넘기고, pass 타입(`RendererShaders`)이 MaterialSchema로 해석한다(16절 MSR19).

**UE와 다른 점(12.1절 대응).** UE 번역 키는 셰이더 맵 키 문자열 전체(플랫폼·`TextureReferencesHash`·Usage 포함 — `RE/Private/Materials/MaterialShared.cpp:3547`)다. Rocks는 사용 대상(8.4절)과 기본 텍스처 AssetId를 뺀다(의도한 편차 — 사용 대상은 job 목록만 바꾸고, 기본 텍스처는 사실만 결과에 닿는다). UE는 쓰는 품질 수준만 쿠킹하고 나머지를 기본 하나로 묶는다(`GetQualityLevelUsageForCooking` — `RE/Private/Materials/Material.cpp:2689–2714`). Rocks는 결과 공유로 같은 효과를 낸다(위).

