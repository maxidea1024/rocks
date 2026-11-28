## 7. 렌더 그래프

### 7.1 UE 5.8 방식

| 단계 | UE 5.8 | 근거 |
|---|---|---|
| 생성 | 스택 객체, prologue 감시 pass, async compute·병합·병렬 허용과 transient 할당기 결정. 병합은 tiled GPU 전용이고 async compute와 함께 켜지지 않는다 | `RC/Public/RenderGraphBuilder.h:50`, `RC/Private/RenderGraphBuilder.cpp:586–648`, `RC/Private/RenderGraphPrivate.h:224–234` |
| pass 추가 | 이름·파라미터 구조체(그래프 수명 할당, 넘긴 뒤 수정 금지)·플래그·람다. 파라미터 없는 pass는 자동 유지 | `RenderGraphBuilder.h:180, 203–207, 220–233`, `RenderGraphBuilder.inl:249–283` |
| 자원 수집 | 구조체 layout의 그래프 자원 목록을 훑어 subresource별 접근 상태를 모은다 | `RenderGraphBuilder.cpp:2384–2532`, `RC/Public/RenderGraphResources.h:72–131` |
| culling | 자원별 마지막 생산자로 간선을 만들고, 유지 pass·외부 출력·내보내기에서 역방향 DFS로 살린다. pass 추가 때 점진 계산 | `RenderGraphBuilder.cpp:1199–1325, 2329–2382` |
| 컴파일 | culling 정리 → render pass 병합(같은 타깃의 연속 raster pass) → async compute fork/join(수명을 구간 전체로 연장) | `:1327–1675` |
| barrier | 같은 자원의 병합 가능한 연속 상태를 합치고 바뀌는 곳에 전이. 같은 pipe면 생산 pass epilogue에서 begin, 소비 pass prologue에서 end | `:3783–3928, 4439–4584`, `RC/Private/RenderGraphResources.cpp:69–152` |
| 수명·할당 | 참조 수 0인 pass에서 해제, 처음 쓰는 pass에서 할당. 풀 자원 → transient 자원 → 첫/마지막 전이 → 전이 객체 미리 생성 | `RenderGraphBuilder.cpp:1962–2044, 3590–3780, 3930–4044` |
| 실행 | pass를 선언 순서대로 실행한다(재배치 없음). pipe 전환 → prologue barrier → attachment 시작 → 람다 → 끝 → epilogue barrier | `:2104–2167, 3424–3528` |
| 풀 | 텍스처는 렌더 타깃 풀, 버퍼는 그래프 버퍼 풀. 해제 fence와 새 할당 fence가 겹치지 않으면 같은 프레임 안에서도 같은 desc 자원을 통째로 재사용(aliasing 아님). 렌더 타깃 풀은 3프레임 이상 안 쓴 것을 풀 크기가 하한 설정을 넘을 때만 오래된 순으로 해제하고, 버퍼 풀은 30프레임 안 쓴 것을 해제한다 | `RC/Private/RenderTargetPool.cpp:211–245, 402–436`, `RC/Private/RenderGraphResourcePool.cpp:110–145, 214, 228` |
| transient aliasing | RHI transient 할당기(D3D12·Vulkan만 구현, 나머지는 풀). 대상은 UAV 버퍼와 공유 아닌 텍스처 | `RHI/Public/RHITransientResourceAllocator.h:535–555`, `RenderGraphBuilder.cpp:502–535` |
| 업로드 | 그래프 실행 앞부분에서 풀 버퍼를 lock으로 채운다 | `RenderGraphBuilder.h:309–333`, `RenderGraphBuilder.cpp:2691–2839` |
| pass 종류 | Raster·Compute·AsyncCompute·Copy(그래픽 pipe)·NeverCull·SkipRenderPass·NeverMerge·NeverParallel·Readback(=Copy+NeverCull) | `RC/Public/RenderGraphDefinitions.h:129–160` |
| task 기반 설정 | 설정 task·명령 목록 설정 task, task로 기록하는 dispatch pass, 기다리지 않는 비동기 pass 표지 | `RenderGraphBuilder.h:239–284`, `RenderGraphDefinitions.h:65–107` |
| 디버그 | 즉시 실행 모드, 검증(한 pass 안 상충 접근 `RenderGraphValidation.cpp:583`, 생산 전 읽기 `:653`, 생산 전 내보내기 `:427`, 파라미터 없는 pass의 플래그 `:597`), 전이 로그, 자원 덮어쓰기, 이벤트 구간. 그래프 설정 변수 31개 | `RC/Private/RenderGraphPrivate.cpp:18–426` |

### 7.2 계획 11.3절과의 대조

| 계획 11.3·11.5절 | UE 그래프 | Rocks 결정 |
|---|---|---|
| DAG 검증 | 재배치·위상 정렬 없음. 의존은 앞쪽 pass만 가리키므로 순환이 생기지 않는다 | 선언 순서 실행, 재배치 없음(결정 R5). "DAG 검증"을 "의존 정합성 검증"으로 고친다. 계획 11.4절 "current color를 다시 자기 조명 입력으로 쓰는 순환 거부"는 한 pass 안 같은 subresource 읽기·쓰기 상충 검사로 한다 |
| 미사용 pass 제거 | 점진 culling | 같다 |
| lifetime 분석 | 참조 수 | 같다 |
| barrier·queue wait | begin·end 분리, cross-pipe fence | 같다. native split이 없는 backend는 end 지점에서 낮춘다(GX5) |
| transient alias | 지원 RHI만, 없으면 풀 | 같다(RG5). alias를 끈 1차에도 **풀**이 필요하다(계획에 없던 단계) |
| copy queue → compute overlap → alias 순서 | 그래프 pipe는 그래픽·async compute 둘뿐. Copy pass는 그래픽 pipe. 업로드는 그래프 앞 lock | copy queue 위치는 결정 R6(확정: 그래프 밖 업로드·스트리밍 계층, UE 그래프에 copy pipe가 없는 것과 같다). compute overlap은 RG4(8절) |
| 계획에 없음 | render pass 병합(tiled GPU 전용) | OpenGL ES·Android가 필수이므로 RG3에 넣는다(같은 attachment 연속 pass 병합 — subpass가 필요 없어 GX13과 무관, 개정 40) |
| 계획에 없음 | 병렬 기록·람다 형태에 따른 실행 방식 | RG3 |
| D3D11/OpenGL 직렬 lowering(11.5) | OpenGL 전이는 빈 함수, dispatch 앞뒤 memory barrier | 같다(GX5) |
| enhanced barriers(11.3) | RHI 안에서 고른다 | 그래프 쪽 접근 상태는 하나, backend가 lowering(GX5) |

### 7.3 Rocks 구조

- 그래프 객체 하나가 한 프레임(또는 한 offscreen 작업)의 pass를 모은다. 렌더 lane이 프레임 packet을 처리하면서 만든다. 그래프 수명 할당은 CP3 프레임 arena다.
- 자원: `GraphTexture`·`GraphBuffer`와 view(SRV·UAV, mip·slice 범위).
- pass: 이름·파라미터 구조체·종류(Raster·Compute·AsyncCompute·Copy)·플래그(`HasSideEffects`·attachment 생략·병합 금지·병렬 금지)·기록 함수. 파라미터 구조체가 의존 선언이다(9.6절). 파라미터 없는 pass, readback·present pass는 `HasSideEffects`다(계획 11.3절). 스왑체인 타깃 pass는 표시 없이 뿌리이고, 한 그래프에서 같은 스왑체인의 pass들은 백버퍼 하나를 함께 쓰며 마지막 pass만 present한다(8절 "스왑체인 공유 설계", 개정 64).
- 접근 상태는 멤버 종류와 pass 종류의 조합으로 정한다(Raster의 SRV는 그래픽 SRV, Compute의 SRV·UAV는 compute 접근, Copy는 복사 원본·대상). UAV는 CS만 허용되므로(셰이더 시스템 7.2절 4항) Raster pass에 UAV 쓰기 멤버가 오면 검증 오류다.
- 공유 값(`GraphSharedData`): 타입별 값 하나씩, 그래프 수명.
- 즉시 실행 디버그 모드: pass를 추가하는 즉시 수집·할당·전이·실행한다. 지연 모드와 결과가 같아야 한다.
- 이벤트 구간: pass마다 `GpuScope`(13절).

### 7.4 그래프 자원 수명

- **import.** `ImportTexture`·`ImportBuffer`는 처음 접근 상태와 함께 장치 자원을 들이며 그 자원의 `RenderResourceLease`를 얻는다. `Retire`된 자원이면 lease를 얻지 못하고 import가 오류를 돌려준다. 호출자는 내장 typed fallback을 import하거나 그 pass를 추가하지 않는다. 그래프는 조용히 대체하지 않는다.
- **export.** `ExportTexture`·`ExportBuffer`는 마지막 접근 상태(기본 SRV)를 정하고, 풀 자원의 사용권을 호출자의 `DeviceResourceOwner`로 넘긴다. 자원은 풀 소속 그대로다. 소유자의 `Retire`는 장치 해제가 아니라 마지막 사용 `GpuFencePoint`가 끝난 뒤 `GraphResourcePool` 반납이고, 아래 풀 축출 규칙은 반납 뒤부터 적용된다(결정 R15, 개정 16). history(이전 프레임 결과)가 이 경로다(7.5절). `DeviceResourceOwner`(RB2) 전에는 export가 돌려준 `RenderHandle`이 소유자이고, 풀이 그 handle의 Retire 대행자(`RenderRetireSink`)다(8절 RG2 구현, 개정 17).
- **풀 축출.** `GraphResourcePool`의 자원은 마지막 사용 `GpuFencePoint`가 완료되었고, 설정한 프레임 수 동안 쓰이지 않았고, 풀 메모리가 예산을 넘을 때 오래된 순으로 `Retire`한다. 계획 8.2절 "경과 프레임 수만으로 회수하지 않는다"에 따라 fence 완료가 필수 조건이다.
- **풀 키.** desc(크기·형식·용도·mip·array·샘플 수)이며 샘플러는 넣지 않는다(결정 R12).

### 7.5 렌더러 사용 요구(Renderer RQ15)

[렌더러](../Renderer/README.ko.md) 15.1절이 렌더 그래프에 요구하는 네 가지다. 렌더 그래프 설계 문서가 따로 없으므로 이 절에 둔다(개정 15). 구현은 8절 RG1의 남은 것과 Renderer RS0이다.

**UE 5.8 근거.**

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 그래프 공유 값 | 타입마다 정적 등록한 구조체 하나. 이미 있는데 만들거나 없는데 `GetChecked`하면 assert | `RC/Public/RenderGraphBlackboard.h:15–20, 56–61, 120–126`, 등록 예 `Renderer/Private/SceneTextures.cpp:82` |
| 장면 텍스처 소유 | view family(Rocks `ViewSet`)가 장면 텍스처 묶음 하나를 가지고 그래프 구성 때 채운다 | `Renderer/Private/SceneTextures.cpp:735–738` |
| history | 이전 결과를 외부 텍스처로 등록해 읽고, 새 결과를 추출 예약한다 | 읽기 `Renderer/Private/PostProcess/TemporalAA.cpp:803`, `Renderer/Private/HZB.cpp:97`. 추출 `Renderer/Private/PostProcess/TemporalAA.cpp:1003`, `Renderer/Private/DeferredShadingRenderer.cpp:607` |
| history 수명 | history는 풀 render target의 참조 계수 포인터다. 새 history로 바꾸면 이전 것은 참조가 풀 하나만 남아 다음 할당이 다시 쓴다 | `Renderer/Private/SceneRendering.h:943–950`, `RC/Private/RenderTargetPool.cpp:202, 754–760` |
| mip view | HZB pass 하나가 위 mip의 SRV를 읽고 아래 mip 여럿을 mip별 UAV로 쓴다. texture view가 없는 장치는 텍스처 전체 SRV를 쓴다 | `Renderer/Private/SceneTextureReductions.cpp:210, 214, 345, 355`. OpenGL ES는 texture view 없음(`OpenGLDrv/Private/OpenGLDevice.cpp:900–901`) |
| view 여럿 | 장면 렌더러(view family 하나)마다 그래프 하나이고, 그 안에서 기능 함수가 view를 돈다. 이름 구간은 형식 문자열로 만든다 | `Renderer/Private/SceneRenderBuilder.cpp:873`, `Renderer/Private/LightGridInjection.cpp:1100`, `RC/Public/RenderGraphEvent.h:479` |

**Rocks 규칙.**
- **`GraphSharedData`.** 타입마다 값 하나, 그래프 수명이다(프레임 arena). 타입은 정적 등록하고, 등록하지 않은 타입은 컴파일 오류다. 이미 있는 타입을 만들면 검증 오류, 없는 값을 반드시 읽는 조회도 검증 오류이고, 있을 때만 읽는 조회는 null을 돌려준다. 값에는 그 그래프의 handle만 담는다(다른 그래프 handle은 7.3절 검증이 거부한다). 값은 그래프가 파괴될 때 렌더 lane에서 파괴된다(8절 구현 메모의 pass 함수와 같다).
- **`SceneTextureSet` 전달(개정 16).** 프레임 packet 하나에 `ViewSet`이 여럿일 수 있다(EditorWorld·Tryout 등, Renderer 4.2·5.1절). 그래서 렌더러는 packet 전체의 장면 텍스처를 (ViewSet 번호, view 번호)로 찾는 표 하나(`SceneTextureTable`)를 `GraphSharedData`에 둔다. `ViewSet`마다 값을 두면 타입별 값 하나 규칙과 부딪치기 때문이다. view마다 크기가 다를 수 있어 장면 텍스처는 view마다 따로다(Renderer 5.1절). `ViewSet` 단위 값(GPU Scene 자원·공유 그림자)도 같은 방식으로 ViewSet 번호를 키로 한 표에 둔다. pass는 표에서 자기 (ViewSet, view)의 `SceneTextureSet`을 꺼내 공유 자원 구조체(9.6절)를 채운다. 장면 텍스처를 만들기 전에 꺼내면 검증 오류다(생산 전 읽기와 같은 성격).
- **history export·import.** `ViewHistory` 소유자(`DeviceResourceOwner`)가 history 텍스처를 가진다(Renderer 5.1절).
  - 이번 프레임은 이전 history를 `ImportTexture`(처음 상태 SRV)로 읽고, 새 history를 그래프 텍스처로 만들어 `ExportTexture`한다. 같은 텍스처를 한 프레임에 읽고 쓰지 않는다(이중 버퍼, UE TAA와 같다).
  - export는 그래프 실행이 성공했을 때만 소유자에게 넘어간다. 검증 실패로 그래프를 버리면(8절) export가 없고, `ViewHistory`는 그 history를 무효로 표시해 다음 프레임에 reset한다. 이전 프레임의 history를 한 번 더 쓰지 않는다(시간 순서가 어긋난 history를 쓰지 않는다).
  - **풀 반납(결정 R15).** history 텍스처는 풀 자원이다. 새 history export가 성공하면 소유자는 이전 history를 `Retire`하고, 그 자원은 마지막 사용 fence 뒤 `GraphResourcePool`로 돌아가 같은 desc의 다음 할당이 다시 쓴다(7.4절). 그래서 매 프레임 history를 만들고 놓아도 정상 상태에서는 장치 생성·해제 없이 2–3장이 돈다. UE도 같은 구조다(위 표 "history 수명"). import lease가 있으므로 GPU가 읽는 동안 반납되지 않는다.
  - history가 없는 프레임(첫 프레임·cut·크기 변경, Renderer 5.1절)은 import하지 않고, history 없음 퍼뮤테이션이나 내장 자원을 명시적으로 묶는다(7.4절 "조용히 대체하지 않는다").
- **mip view 그래프 자원.** `GraphTextureView`는 (텍스처, mip 범위, slice 범위, SRV·UAV)다. 상태 추적과 상충 검사는 subresource 단위이므로 한 Compute pass가 mip k의 SRV와 mip k+1…k+n의 UAV를 함께 가질 수 있다(HZB). 범위가 겹치면 한 pass 안 상충 접근이다. 장치 view는 GX2의 텍스처 view(mip·slice 범위)로 낮춘다. 버퍼도 같은 방식의 `GraphBufferView`(버퍼, 형식·범위, SRV·UAV)를 둔다(개정 36, 4.4절 "GX3·GX4 compute 연결 설계" (3)).
  - texture view가 없는 backend(OpenGL ES)는 SRV에 텍스처 전체를 묶는다. 그래서 mip view를 읽는 셰이더는 그래프가 view마다 주는 **view 기준 mip**(view를 지원하는 backend는 0, 아니면 view의 첫 mip)을 상수로 받아 명시 LOD로 읽는다(`SampleLevel`·`Load`). 크기 조회(`GetDimensions`)도 같은 기준 mip을 명시한다. 텍스처 전체가 묶인 GLES에서 mip 0 크기를 읽지 않기 위해서다(개정 16). backend마다 원문을 바꾸지 않는다. UAV는 GLES image 묶기가 level 하나를 받으므로 그대로 한 mip이다.
  - 같은 텍스처의 mip k 표본과 mip k+1 image 쓰기를 한 dispatch에서 하는 것이 ES 3.2에서 정의된 동작인지는 사양 대조가 남았다(**미확인**). RG1 GLES 시험에서 확인하고, 안 되면 GLES는 mip마다 pass를 나눈다. 개정 26: 그래프 HZB 시험(8절 RG1 남은 것 B부)이 두 GLES context에서 기댓값과 같다(사양 문구 대조는 여전히 남는다).
- **view 여럿을 한 그래프에.** 프레임 packet 하나가 그래프 하나다(7.3절). 그 안에 packet이 담은 `ViewSet` 여럿과 각 `ViewSet`의 view 여럿이 모두 들어간다. UE는 view family마다 그래프 하나다.
  - 선언 순서(개정 16): `ViewSet` A 공통 pass(장면 변경 적용·GPU Scene 업로드·sky LUT·공유 그림자, Renderer 6.2절 F1–F3·F7) → A의 view들(view 순서, view마다 그 view의 pass 전체) → `ViewSet` B 공통 pass → B의 view들 …. UE는 기능 함수 안에서 view를 도는 pass 단위다. Rocks가 view 단위인 이유: 한 view의 그래프 자원 수명이 그 view 구간에서 끝나므로 다음 view가 같은 desc 자원을 프레임 안 풀로 다시 쓸 수 있다(RG2). 선언 순서가 실행 순서다(결정 R5).
  - **그래프 구간 `GraphScope`.** 그래프에 pass 여러 개를 감싸는 이름 구간을 둔다(`GpuScope` 중첩, 13절). `ViewSet` 구간·view 구간·기능 구간(예: 그림자)에 쓴다. 구간 이름은 정적 이름(`TraceScopeName`, CP7 정적 이름 + 64 bit 해시)이므로 view 구간은 이름 `View`, ViewSet 구간은 `ViewSet`이고, 번호는 구간을 연 직후 trace 표지(`ROCKS_TRACE_MARK`, [다중 스레드 trace](../TraceSystem.ko.md) 표지 규칙)로 남긴다(개정 16). 캡처 도구의 GPU marker에는 번호가 보이지 않는다. UE는 형식 문자열로 구간 이름을 만든다(`RC/Public/RenderGraphEvent.h:479`).
  - view마다 다른 자원은 다른 그래프 자원이다. view 사이에 공유하는 자원(shadow atlas·GPU Scene)은 한 번 만들거나 import하고 여러 view의 pass가 읽는다. 공유할지(같은 light/caster revision 등)는 렌더러가 정한다(Renderer 5.1절).
- **완료 기준(RG1 남은 것에 더한다).** `GraphSharedData` 중복 생성·없는 값 필수 조회의 음성 대조와 다른 그래프 handle을 담은 값 거부, `ViewSet` 둘·view 셋인 packet의 `SceneTextureTable` 조회, 검증 실패 그래프의 export 0건과 history 무효 표시, history 반납 뒤 같은 desc 할당의 재사용(장치 생성 0건, fence gate 전에는 재사용 0건), 같은 텍스처의 mip k SRV + mip k+1 UAV pass 통과와 범위가 겹치는 pass 음성 대조, GLES view 기준 mip 경로의 HZB 축소 픽셀 대조(D3D12·Vulkan과 같은 결과), packet의 pass 순서·구간 중첩·trace 표지 기록, RG2에서 view 사이 풀 재사용 기록.
