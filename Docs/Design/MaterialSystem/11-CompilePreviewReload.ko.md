# 머티리얼 시스템(MaterialSystem) 설계

## 11. 비동기 컴파일·대체 머티리얼·미리보기·hot reload

### 11.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 캐시 조회 | 메모리 셰이더 맵 → 비동기 DDC 조회 → 없으면 컴파일 시작. 쿠킹 데이터이거나 컴파일할 수 없으면 셰이더 맵 없음(기본 머티리얼 사용). 기본 머티리얼 자신이 그렇게 되면 Fatal | `RE/Private/Materials/MaterialShared.cpp:2998–3219`(조회 `:3043, 3066`, 실패 `:3150–3176`) |
| 컴파일 | 번역 → 컴파일 환경 → 셰이더 맵 `Compile`. 비동기면 완성 전 복제본을 게임 스레드 셰이더 맵으로 둔다 | `MaterialShared.cpp:3714–3816` |
| 우선순위 | 동기 High, 비영속(미리보기) Normal, 그 밖 Low | `RE/Private/Materials/MaterialShader.cpp:2904–2915` |
| 동기 컴파일 | 기본 머티리얼은 항상 동기 | `MaterialShared.cpp:2351–2354` |
| 컴파일 모드 | None·Background·Synchronous, 기본 Background | `RE/Public/MaterialShaderPrecompileMode.h:6–13` |
| 그리는 동안 대체 | 셰이더 맵이 없거나 미완성이면 대체 사슬을 따라 완성된 머티리얼을 쓴다. 에디터는 이때 미뤄 둔 job을 제출한다 | `RE/Private/Materials/MaterialRenderProxy.cpp:870–893` |
| 대체 대상 | 영역 기본 머티리얼(`UMaterial::GetDefaultMaterial(Domain)`). 기본 머티리얼은 대체가 없다 | `RE/Private/Materials/Material.cpp:441–450, 555–558, 1094–1109` |
| 기본 머티리얼 에셋 | `WorldGridMaterial`(Surface), `DefaultLightFunctionMaterial`, `DefaultDeferredDecalMaterial`, `DefaultPostProcessMaterial` | `CFG/BaseEngine.ini:155–156, 219–220` |
| 편집 반영 | `PostEditChange`가 렌더 명령을 비우고 진행 중 컴파일을 취소한 뒤 다시 캐시. 미리보기 머티리얼(`bIsPreviewMaterial`)은 전역 컴포넌트 재생성을 건너뛴다 | `RE/Private/Materials/Material.cpp:5356–5438`, `ED/MaterialEditor/Private/MaterialEditor.cpp:695` |
| 갱신 문맥 | `FMaterialUpdateContext`(컴포넌트 재등록·렌더 스레드 동기·render state 재생성) | `RE/Public/MaterialShared.h:3480–3535` |
| 노드 미리보기 | 노드를 출력으로 삼은 별도 `FMaterial`·proxy. 생성 때 `CacheShaders(None)`, 갱신 때 지연 삭제 | `ED/MaterialEditor/Private/MaterialEditor.h:57, 117`, `MaterialEditor.cpp:7634–7691` |
| `UMaterialInstance` proxy의 대체 구현 | — | 미확인(열람하지 않음) |

### 11.2 컴파일 흐름(에디터)

`CompileCoordinator`(EditorMaterial)가 맡는다. 계획 14.8절의 규칙(transaction revision·debounce·결과 수용 조건·취소 수명)을 구현하는 순서는 다음과 같다.
1. 편집 transaction이 끝나면 semantic hash를 다시 계산한다. 같으면(화면 편집만) 아무것도 하지 않는다.
2. debounce 뒤 번역 요청을 만든다. 입력은 그래프·Build 의존의 immutable snapshot과 미리보기 프로필이다.
3. **번역**은 메인 프로세스의 작업(WorkerScheduler)으로 한다. 번역 너겟을 먼저 찾는다. 번역은 도구·장치가 필요 없다. 걸리는 시간은 측정 MM1로 확인하고, 길면 별도 프로세스로 옮길지 정한다.
4. 번역 결과에서 job 목록(미리보기 프로필의 형식·정점 입력·pass)을 만들어 ShaderCompiler에 제출한다. 우선순위는 열린 머티리얼의 미리보기 > 선택 노드 미리보기 > 화면에 보이는 다른 머티리얼 > 백그라운드다(16절 MSR7).
5. 결과가 오면 수용 조건(세션 epoch·현재 semantic hash·프로필·compile generation)을 확인하고, 미리보기 프로필 범위의 완결성을 검사한다.
6. 통과하면 새 `MaterialShaderSet`(새 epoch)을 렌더 lane에 넘기고 프레임 경계에서 교체한다(RenderBase 9.8절, 11.5절).

### 11.3 대체 규칙

| 상황 | 그리는 것 | 표시 |
|---|---|---|
| 에디터, 처음 컴파일 중 | 영역 기본 머티리얼 | 컴파일 중 표시 |
| 에디터, 성공한 적이 있고 다시 컴파일 중 | 마지막 성공 셰이더 집합(계획 14.8절) | "현재 그래프와 다른 마지막 성공 버전" |
| 에디터, 컴파일 실패 | 마지막 성공 셰이더 집합, 없으면 기본 머티리얼 | 오류 위치(노드·핀) |
| 쿠킹 실행, 필요한 셰이더가 라이브러리에 없음(사용 대상 누락 등) | 영역 기본 머티리얼 | Development: 머티리얼·정점 입력 타입마다 한 번 오류 로그. Release: 오류 계수(진단 API)만 |
| 쿠킹 실행, 머티리얼 셰이더 집합 서명 불일치 | 영역 기본 머티리얼 | `IncompatibleCook` 진단 |
| 엔진 기본 머티리얼이 없거나 불완전 | — | 렌더 lane 시작 실패(`IncompatibleCook`, RenderBase 9.2절 완결성 검사) |

**검은 표면 금지.** 계획 11.1절대로 실패를 검은색이나 투명으로 처리하지 않는다. Surface 기본 머티리얼은 눈에 띄는 격자 무늬다(UE와 같은 성격, `CFG/BaseEngine.ini:155`).

**엔진 기본 머티리얼.**
- 영역마다 하나(Surface·DeferredDecal·LightFunction·PostProcess·UserInterface, Volume은 후속)다. 엔진 에셋이며, 그 셰이더는 엔진 내장 셰이더 라이브러리(셰이더 시스템 12.3절)에 들어간다.
- 모든 엔진 정점 입력 타입과 모든 pass에 대해 컴파일한다(`isDefaultMaterial` 특성, 8.3절). 그래서 사용 대상 거르기를 받지 않는다.
- 렌더 lane 시작의 엔진 셰이더 완결성 검사(RenderBase 9.2절)에 포함한다. UE는 기본 머티리얼을 처음 쓸 때 동기 컴파일하지만, Rocks는 쿠킹 라이브러리에서 시작 때 검사한다.
- 엔진 내장 라이브러리를 만드는 `shader-compile --engine`(셰이더 시스템 12.3절)은 지금 `Engine/Shaders` 원문만 다룬다. 엔진 기본 머티리얼 에셋을 번역해 넣는 경로를 더해야 한다(16절 MSR11). 필요 시 컴파일 형식(`GLES31_PC`)은 RenderBase SX14 셰이더 공급자가 엔진 기본 머티리얼 번역도 맡는다(16절 MRB1).
- 셰이더 시스템 12.2·12.3절은 "필요한 셰이더가 없으면 `IncompatibleCook`"이다. 머티리얼 셰이더는 그 예외로 위 표의 조합 단위 대체를 쓴다(16절 MSR10). 엔진 셰이더와 엔진 기본 머티리얼은 그 규칙 그대로다.

**엔진 내장 "구운 표면" 머티리얼**(Unreal 교환 UXR4, 2026-11-20). 다른 엔진에서 가져온 머티리얼을 그래프로 번역하지 못했을 때, 그쪽에서 구운 텍스처를 받는 부모 머티리얼이다([Unreal 교환 형식](../UnrealExchange.ko.md) 8.5절 결과 2번). 기본 머티리얼과 달리 대체용이 아니라 보통 머티리얼이며 사용 대상 거르기를 받는다.
- 영역 Surface, Opaque와 Masked(블렌드 모드는 static 파라미터), 기본 표면 모델.
- 텍스처 파라미터: BaseColor, Normal(접선 공간), Metallic, Roughness, Specular, Emissive(HDR), Opacity, OpacityMask, AmbientOcclusion. 스칼라 파라미터: Emissive 배율, OpacityMask 기준값. 비어 있는 텍스처 파라미터는 엔진 기본 텍스처(흰색·평면 법선·검정)를 쓴다.
- 엔진 콘텐츠 `/Engine` 마운트는 후속이다(AssetSystem 4.2절). 그 전까지는 `UnrealExchange` 플러그인이 자기 콘텐츠로 이 머티리얼을 갖고, `/Engine` 마운트가 생기면 엔진 콘텐츠로 옮긴다.

### 11.4 미리보기

- **미리보기 프로필.** 에디터 호스트의 현재 backend 형식 하나, 미리보기 메시의 정점 입력 타입, 미리보기 view의 렌더 경로 pass만 컴파일한다. Deferred·Forward+ 전환(계획 14.5절 "실시간 확인")은 다른 경로 pass를 추가로 요청한다.
- **노드 미리보기.** 선택 노드의 출력을 `EvaluateMaterialPreview`로 내는 번역을 따로 한다. 정점 입력은 엔진 기본, pass는 미리보기 전용 unlit pass `MaterialPreview` 하나다(Renderer 14.3절. UE도 BasePass 하나로 줄인다, `ED/MaterialEditor/Private/MaterialEditor.cpp:351–376`). 텍스처 객체·`Surface`·`MaterialAttributes`처럼 색으로 보일 수 없는 타입은 정해진 시각화(텍스처 샘플·BaseColor 등)를 쓴다. 동시에 컴파일하는 노드 미리보기 수에 상한을 둔다(계획 14.8절).
- **실시간 갱신.** 시간 의존 노드가 있으면 미리보기 viewport가 매 프레임 그린다. 끄기 선택(계획 14.5절 "realtime on/off")은 viewport 갱신만 멈춘다.
- **dynamic 파라미터 편집.** 인스턴스 에디터·머티리얼 에디터의 파라미터 값 변경은 상수 블록만 바꾼다(compile 0, 계획 MT04).

### 11.5 hot reload

| 원인 | 다시 하는 일 |
|---|---|
| 머티리얼 그래프 편집 | 그 머티리얼의 번역·job. 자손 인스턴스는 static 변형이 있으면 변형 번역, 없으면 값 배열만 다시 평탄화(9.6절) |
| 함수·함수 인스턴스 편집 | AssetIndex 역방향 Build 의존으로 쓰는 머티리얼만 다시 번역(계획 14.8절 "역의존 closure만") |
| 컬렉션 layout 편집 | 참조 머티리얼 다시 번역. 값만 바꾸면 컬렉션 상수 블록만 갱신 |
| pass 원문·`MaterialCommon.rsh` 편집(셰이더 시스템 SH4) | 번역은 그대로, job만 다시(전처리 결과가 바뀌어 job 키가 바뀐다) |
| 기본 텍스처 교체 | 셰이더 그대로, 텍스처 handle만 교체(4.5절) |

교체 규칙:
- 새 셰이더 집합은 새 epoch이고, 완결성·서명 검사를 통과해야 교체 후보가 된다(RenderBase 9.8절).
- 교체는 프레임 경계에서 셰이더 집합·상수 블록 layout·텍스처 목록을 한 묶음으로 한다(계획 14.8절). layout이 바뀌면 렌더 상태의 값을 `ParameterKey`로 옮긴다(9.6절).
- 이전 집합·상수 블록은 `Retire`하고, recording lease와 마지막 GPU fence가 끝난 뒤 회수한다(계획 11.2절, MT09).
