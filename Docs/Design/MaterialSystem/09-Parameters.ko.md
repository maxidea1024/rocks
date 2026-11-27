# 머티리얼 시스템(MaterialSystem) 설계

## 9. 파라미터

### 9.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 종류 | Scalar·Vector·DoubleVector·Texture·TextureCollection·Font·RuntimeVirtualTexture·SparseVolumeTexture·StaticSwitch·ParameterCollection(실행 중 종류), StaticComponentMask(에디터 전용) | `RE/Public/Materials/MaterialParameters.h:187–207` |
| 식별 | 이름 + 연관(전역·레이어·혼합) + 레이어 index. 전역은 index 없음 | `MaterialParameters.h:25–46` |
| 메타데이터 | 값·primitive data index, 에디터 전용 그룹·설명·범위·정렬·Guid | `MaterialParameters.h:446–497` |
| 상수 layout | preshader 버퍼(`float4` 배열), VT·텍스처 컬렉션 묶음, 텍스처·샘플러 slot(최대 128), 공유 샘플러 `Wrap_`·`Clamp_WorldGroupSettings`. 이름 `"Material"`, 정점 전용 압축판 `"MaterialVS"` | `RE/Private/Materials/MaterialUniformExpressions.cpp:524–700, 960–1001` |
| 기본값 | `DefaultValues` 바이트 표에 오프셋으로 | `RE/Public/MaterialShared.h:729–737, 860`, `MaterialUniformExpressions.cpp:1105–1111` |
| preshader | opcode 집합(상수·파라미터·산술·TextureSize 등) | `RE/Public/Shader/Preshader.h:19–75` |
| 평가 | 렌더 스레드 `EvaluateUniformExpressions`가 버퍼를 채우고 uniform buffer를 만들거나 갱신, 결과를 캐시 | `RE/Private/Materials/MaterialRenderProxy.cpp:458–637`, 캐시 `RE/Public/Materials/MaterialRenderProxy.h:48–85` |
| 값 해석 순서 | proxy 값 → (에디터) 임시 override → 기본값 표 | `RE/Private/Shader/Preshader.cpp:390–465`, `MaterialUniformExpressions.cpp:1526–1543` |
| 인스턴스 | 타입별 해시 맵에 값, 없으면 부모 proxy로 재귀. 게임 스레드 변경은 렌더 명령으로 전달하고 캐시를 무효화 | `RE/Private/Materials/MaterialInstanceSupport.h:207, 259–265, 406–418`, `RE/Private/Materials/MaterialInstance.cpp:295–398, 608–625` |
| static | static switch·component mask·지형 레이어 가중치(랜드스케이프 바인딩)·레이어 구성이 셰이더 맵 키에 들어간다 | `RE/Public/StaticParameterSet.h:396–468`, `RE/Public/MaterialShared.h:1345–1348` |
| 컬렉션 | 머티리얼당 최대 2개(초과 시 번역 오류), 컬렉션 크기 `r.MPC.MaxVectorStorage` 1280(초과는 경고만), `float4` 배열 하나짜리 uniform buffer, 씬의 버퍼 → 없으면 기본 인스턴스 | `RE/Public/ParameterCollection.h:13–18`, `RE/Private/Materials/HLSLMaterialTranslator.cpp:5069–5072`, `RE/Private/Materials/ParameterCollection.cpp:25–30, 798–830`, `RR/Private/ShaderBaseClasses.cpp:119–141` |
| 함수 안 파라미터 | 일반 함수 호출은 연관을 바꾸지 않으므로 전역 연관이다. 그래서 같은 이름은 같은 파라미터가 된다(레이어·혼합 호출만 연관을 바꾼다, `RE/Private/Materials/MaterialExpressions.cpp:7238–7239, 7273–7274`) | 근거에서 추론. 번역기의 파라미터 소유자 스택(`RE/Private/Materials/HLSLMaterialTranslator.h:803–807`) 전체 경로는 추적하지 않았다(미확인) |

### 9.2 식별

`ParameterKey = { ownerAssetId, ParameterId, scope }`.
- **ownerAssetId** = 파라미터 노드가 있는 에셋(머티리얼 또는 함수)이다. 계획 14.7절의 "MaterialAssetId·ParameterId·layer instance 경로·function call-site NodeId 경로"와 같다.
- **scope** = 전역 | 함수 호출 경로(호출 위치 NodeId 목록) | 레이어(레이어 Guid, 그 안의 호출 경로) | 혼합(혼합 Guid, 그 안의 호출 경로).
- 계획 14.7절대로 함수 안 파라미터는 **호출 위치마다 독립**이다. 같은 함수를 두 번 부르면 두 파라미터다. 여러 호출이 공유해야 하는 파라미터는 파라미터 노드의 "공유" 표시로 전역 scope에 둔다. 이것은 UE와 다르다(9.1절: UE는 같은 이름이면 합쳐진다).
- **이름**은 표시·검색용이다. 인스턴스 에디터는 이름과 그룹으로 보여 주고, 저장은 키로 한다. 같은 scope의 같은 이름은 V8 오류다.
- **외부 그래프 번역**(Unreal 교환 UXR4, 2026-11-20): UE 그래프는 이름이 같은 파라미터 노드를 하나로 합친다. 번역기([Unreal 교환 형식](../UnrealExchange/08-Material.ko.md) 8.5절)는 이것을 Rocks 규칙으로 옮긴다. 머티리얼 그래프 안에서 같은 이름·같은 종류의 파라미터 노드 여러 개는 파라미터 노드 하나와 그 출력을 잇는 간선들로 바꾼다. 함수 안에서 UE global association인 파라미터는 위의 "공유" 표시를 붙인 전역 scope 파라미터로 옮긴다. 결과 그래프에는 같은 scope의 같은 이름이 남지 않으므로 V8을 통과한다. 인스턴스 override 하나는 Rocks 키 하나로 옮겨진다.
- 레이어 순서를 바꿔도 레이어 Guid가 그대로라 override가 남는다. UE는 index를 다시 매긴다(`RE/Public/Materials/MaterialParameters.h:78, 134` `RemapLayerIndex`).

### 9.3 종류와 단계

| 종류 | 값 | 재컴파일 | 단계 |
|---|---|---|---|
| Scalar | float | 없음 | MAT0 |
| Vector | float4(색·벡터) | 없음 | MAT0 |
| Texture | 텍스처 에셋 참조(종류는 그래프가 고정) | 없음(종류가 같으면) | MAT0 |
| Dynamic Bool | bool(셰이더 분기) | 없음 | MAT2 |
| Static Bool | bool | static 변형 | MAT0 |
| Static Component Mask | RGBA 4비트 | static 변형 | MAT2 |
| Static Enum(preset 선택 등) | 정수 | static 변형 | MAT2 |
| Collection 참조 | 컬렉션 에셋 | static 변형(layout이 원문에 들어간다) | MAT2 |
| Layer Stack | 레이어·혼합 함수 목록 | static 변형 | MAT3 |
| Double Vector(절대 위치 등) | `Double4`(5.3절 — uniform은 double high·low 두 slot) | 없음 | MS2c(19장) |
| 가상 텍스처·텍스처 컬렉션·sparse volume·커브 atlas | 해당 에셋 참조 | 없음 | TODO — 노드 서술과 속성은 MS1b, lowering은 선행(19.4.1절 `MPQ-RuntimeVirtualTexture`·`MPQ-TextureCollection`·`MPQ-SparseVolumeTexture`·`MPQ-CurveAsset`) 뒤 |
| font | — | — | TODO(`MPQ-Font`, 19.4.1절). 파라미터 노드 `FontSampleParameter`, 값은 (미리 구운 Font, 페이지). 함께 들일 비파라미터 노드 `FontSample`·`FontSignedDistance`, 단계 FT7 — [글꼴 설계](../Font/07-Consumers.ko.md) 7.4절 |

- **IR 정체(6.12절 MS2e2).** 번역은 파라미터를 ParameterKey에서 만든 정체 Guid로 구분한다: 주 그래프 파라미터는 ParameterId, 함수 안은 SHA-256(소유 에셋, ParameterId[, 호출 경로 — "공유"가 아니면]) 앞 128 bit. static 값 입력과 IR 파라미터·텍스처 표는 이 정체로 찾는다.

### 9.4 상수 식 프로그램과 상수 블록

- **상수 식 프로그램**(`MaterialConstants::steps`): 모든 `Uniform` 빈도 출력을 함께 계산하는 명령열 하나다. 명령은 모듈 번호 순 IR 값이고 피연산자는 앞 명령 번호이며 형식 버전을 가진다(6.10절 MS2c2 — 스택 기계는 버렸다).
  - 명령: 상수, 파라미터 읽기(모듈 파라미터 표 번호), 그리고 UE가 uniform 식으로 접는 연산과 같은 목록(사칙·Min·Max·Abs·Floor·Ceil·Round·Truncate·Frac·Saturate·Sign·Sqrt·Reciprocal·Length·Normalize·Dot·Cross·삼각·역삼각·Exp·Log 계열·Fmod·Modulo·Negate·Clamp·swizzle·구성·Float↔Double). Lerp·Pow·비교·선택은 UE처럼 접지 않는다(셰이더가 계산하고 피연산자가 출력이 된다). 컬렉션 값은 World마다 실행 중에 바뀌므로 명령이 아니라 컬렉션 cbuffer 읽기다.
  - UE preshader opcode(`RE/Public/Shader/Preshader.h:19–75`)와 같은 연산 집합이다.
  - 텍스처 크기는 명령이 아니라 텍스처 slot의 논리 크기 상수다(7.4절, TextureStreaming 5.3절).
  - 실행은 float32이고 Double 값만 double이다(LWC 파라미터 — UE도 double 두 칸이다). 실행 결과는 쿠킹 바이트가 아니므로 결정성 규칙(AssetSystem 10절) 대상이 아니다. 다만 같은 실행 파일에서는 같은 입력에 같은 결과를 낸다(멀티스레드 평가도 순서 무관).
- **상수 블록 layout**(`MaterialConstants`): 머티리얼 컴파일러가 7.4절 5항 규칙으로 정한다.
  - 순서: float 계열 slot(상수 식 출력을 크기 큰 것부터, 같은 크기는 값 번호 순으로 채움, Double은 tile·offset 두 칸) → int·uint slot → bool 비트 slot. MS2c2는 float 계열만 낸다(프로그램 값이 Float·Double뿐이다). int·uint·bool slot은 Dynamic Bool 파라미터(MAT2)가 생길 때 더한다.
  - 텍스처는 상수 블록 밖의 자원 slot `MaterialTexture<k>`(k는 모듈 텍스처 표의 (출처, id) 순 — 6.10절 MS2c3)이다. Texture Size는 slot의 논리 크기를 읽는 프로그램 명령이고 결과가 float 멤버가 된다(6.10절 MS2c3b).
  - layout 표 = 상수 블록 멤버 목록(`MaterialConstants` — 출력 값·칸·성분) + 모듈 텍스처 표(`IrModule::Textures()` — 출처·id·종류·기본 asset). 자기 샘플러를 쓰는 slot은 표에 칸을 두지 않고 생성 원문이 샘플 값에서 유도한다.
  - UE는 preshader 버퍼를 `float4` 배열 하나로 두고 이름 없이 오프셋으로 접근한다(`MaterialUniformExpressions.cpp:642`). Rocks는 GLES 에뮬레이션의 타입 분리 규칙 때문에 float·int·uint를 나눈다.
- **평가 위치.** 렌더 lane이 `MaterialRenderState`마다 (파라미터 version, 컬렉션 version)이 바뀐 때만 상수 식을 실행하고 상수 블록을 채운다(UE와 같은 위치, 9.1절). 게임 lane은 바뀐 파라미터 값만 프레임 packet으로 보낸다(RenderBase 1절 "불변 값 묶음"). 평가가 많으면 CP2 병렬 실행을 쓴다(구현 때 측정).
- **상수 블록 수명.** 머티리얼 렌더 상태의 상수 블록은 `Persistent`(RenderBase 9.4절)다. 값이 바뀌면 새 블록을 할당하고 이전 블록은 `Retire`한다(진행 중 프레임이 읽고 있을 수 있다). GLES는 CPU 사본이고 업로드 판단은 RenderBase 9.4절대로다.

### 9.5 static 파라미터와 변형

- static 값(Static Bool·Component Mask·Enum·컬렉션 참조·레이어 구성)과 **기본 속성 override**(블렌드 모드·양면·마스크 기준값·shading preset)를 인스턴스가 바꾸면 static 변형이 생긴다. UE도 기본 속성 override가 있으면 static 변형을 만든다(`RE/Private/Materials/MaterialInstance.cpp:2415–2424`, 키 `RE/Public/MaterialShared.h:1376` `BasePropertyOverridesHash`).
- 변형은 부모 머티리얼 그래프 + 인스턴스 사슬의 static 값으로 번역한다. 번역 너겟 키에 정규화한 static 값 목록이 들어간다(12.2절).
- static 변형이 없는 인스턴스는 부모 산출물(셰이더 집합)을 그대로 쓰고 자기 값 배열만 가진다.
- 에디터는 static 값을 바꿀 때 "새 변형 컴파일" 비용을 표시한다(계획 14.5절 "static variant"). dynamic 값만 바꾸면 compile job은 0이다(계획 MT04).
- `DynamicMaterialInstance`는 static 값을 바꿀 수 없다(`UnsupportedSetting`, 4.2절).

### 9.6 인스턴스 override 해석

- **평탄화.** 인스턴스를 로드하거나 편집할 때, 셰이더 집합을 가진 조상(머티리얼 또는 static 변형)의 layout 기준으로 값 배열을 만든다. slot마다 가장 가까운 override, 없으면 머티리얼 기본값이다.
  - 부모가 바뀌면(편집·hot reload) 자손 인스턴스의 배열을 다시 만든다. 자손 목록은 AssetIndex 역방향 의존으로 찾는다.
  - 그리기 경로에서 부모를 거슬러 찾지 않는다. UE는 렌더 스레드에서 부모 proxy로 재귀한다(9.1절).
- **`DynamicMaterialInstance`.** 부모의 평탄화 배열을 복사해 시작하고, `Set(ParameterKey, value)`로 slot 하나를 바꾸고 version을 올린다. slot index를 미리 얻는 API(`FindSlot`)를 두어 매 프레임 키 검색을 피한다. UE도 index 기반 설정을 둔다(`RE/Public/Materials/MaterialInstanceDynamic.h:41, 48`).
- **텍스처 값.** 텍스처 override는 텍스처 에셋의 논리 handle이다. 실제 장치 자원은 그래프 구성 때 TextureStreaming의 바인딩 snapshot으로 해석한다(TextureStreaming 결정 "교체 게시": descriptor를 제자리에서 덮지 않는다). 그래서 스트리밍이 mip을 바꿔도 머티리얼 상수 블록을 다시 만들지 않는다. UE도 간접 참조를 묶어 같은 효과를 낸다(`RE/Private/Materials/MaterialUniformExpressions.cpp:1712–1729`).
- **layout이 바뀔 때(hot reload).** 새 layout의 slot을 `ParameterKey`로 찾아 이전 값을 옮긴다. 없어진 키의 override는 에셋에는 남기고(복원 가능) 진단으로 표시한다. 타입이 바뀐 키는 기본값으로 두고 진단한다.

### 9.7 파라미터 컬렉션

- **에셋.** scalar·vector 파라미터 목록(`ParameterId`·이름·기본값). layout = scalar를 4개씩 묶은 `float4` + vector `float4` 배열이다(UE와 같은 배치, `RE/Private/Materials/ParameterCollection.cpp:720–731`).
- **머티리얼당 개수.** 처음에는 2개다. UE 값과 같다(`RE/Public/ParameterCollection.h:13–18`). pass 셰이더 하나의 상수 버퍼는 공유 slot 5 + 정적 조명 1 + 머티리얼 블록 1 + 컬렉션 2 + pass 1 = 최대 10으로 형식 정의 상한 12–14 안이다(RenderBase 9.5절, 개정 5). 늘리려면 이 예산을 다시 본다.
- **크기 상한(개정 5).** GLES는 상수 버퍼·루트 상수가 모두 uniform `vec4` 배열이라 단계의 합(렌더러 공유 slot·정적 조명·머티리얼 블록·컬렉션·pass 상수·루트 상수)이 PS 에뮬레이션 `vec4` 224 안이어야 하고, 셰이더 워커가 job 형식 상한으로 그 합을 검사한다(셰이더 시스템 7.2절 4항, RenderBase 9.5절). 그래서 컬렉션 하나의 `float4` 상한 = (224 − 렌더러 쪽 합 − 루트 상수 8 − 머티리얼 블록 상한) ÷ 2로 정한다. 렌더러 쪽 합이 구조체 확정(Renderer RS0) 뒤 정해지므로 값도 그때 정하고, 렌더러 추정(약 100, Renderer 15.2절)으로는 컬렉션당 약 50이다. 넘으면 컬렉션 에셋 저장·쿠킹 오류다(UE는 경고만 낸다).
- **바인딩.** 컬렉션은 공유 상수 slot(RenderBase 9.5절)이다. slot 등록은 정적 표가 아니라 컬렉션 에셋 identity로 하는 동적 등록이다(16절 MRB4). 생성 원문은 `cbuffer MaterialCollection<k>`로 내고, layout 서명은 컬렉션 layout의 SHA-256이다.
- **값.** World마다 `ParameterCollectionState`가 있고 게임 lane이 값을 바꾼다. 바뀐 값은 프레임 packet으로 넘어가고 렌더 lane이 컬렉션 상수 블록을 갱신한다.
- **키.** 컬렉션 **layout**(파라미터 목록·순서)이 번역 키에 들어간다. 기본값·실행 중 값은 들어가지 않는다. UE는 컬렉션 StateId를 셰이더 맵 키에 넣는다(`RE/Public/MaterialShared.h:1351–1354`).
