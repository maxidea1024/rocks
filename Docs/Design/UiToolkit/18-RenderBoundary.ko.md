## 18. 렌더 경계(UW6 상세 설계)

9.1절 UW6의 상세다. 지금 UI 렌더 경로는 단일 스레드 호스트(UiDemo)가 장치를 직접 잡고 창·표면마다 따로 제출한다. 이 장은 그 경로를 3.4절 lane 계약(Render lane은 Element·Widget을 읽지 않는다)과 3.7절 `UiRenderer` 계약으로 옮기는 설계다. UE 5.8 대응은 `FSlateRenderer`(SlateCore/Public/Rendering/SlateRenderer.h 260–569행)와 `FSlateRHIRenderer`(SlateRHIRenderer/Private/SlateRHIRenderer.h·.cpp)다. 리뷰(2026-11-26, 리뷰어 1명 — 21건)를 반영했다.

### 18.1 지금 상태(2026-11-26 조사)

- UiCore `WidgetSurfaceScene::Draw(graphics::Device&, graphics::CommandContext&, w, h)`와 UiWidget `ViewportWorld::Renderer`(장치·명령 문맥을 받는 delegate)가 그래픽 타입을 공개 헤더에 앞 선언으로 드러낸다. 모듈 의존(Build.cs)에는 Graphics가 없다. 앞 선언만으로 경계를 넘고 있다.
- UiRender `WidgetSurfaceHost`가 Element 트리를 자식 먼저 순회한다. 보관 표면·post buffer·viewport를 한 목록(`order_`)에 안쪽부터 모으고, 장면의 `Advance`(World tick)를 부르고, 표면마다 장치 프레임을 열어 제출한다. Element 기반 장면 painter(`ScenePainter`)도 이 객체에 있다.
- `DrawBatchBuffers`가 게임 쪽 `DrawBatch`를 참조로 들고 렌더 때 읽는다. 정점은 바뀐 범위만 `UpdateBuffer`한다. 흐림·둥근 상자 pipeline과 배경 텍스처는 그리기 중에 늦게 만든다.
- 브러시 등록부 `SlotArray<BrushTexture>`(장치 texture·pipeline handle)를 앱이 만들어 게임 쪽 코드(갤러리 텍스처·글자·도킹 창)에 넘긴다. `DrawCommand::resource`가 이 등록부의 slot이다. 셰이더 선택(단색·이미지·마스크·보관 합성)은 brush의 pipeline이 정한다.
- post buffer(`BlurToBuffer`)의 id를 게임 쪽 위젯이 그리기 명령의 resource로 쓴다(`WidgetSurface::SetPostBuffer`). 크기는 감싸는 pass 영역(Element geometry)이 정한다.
- `VectorImageAtlas`가 그리기 중 페이지를 래스터하고 `Sync(pipeline)`으로 장치 텍스처를 직접 만든다.
- UiDemo는 `RenderLane` 없이 창마다 스왑 체인을 두고, 바뀐 창만 다시 그리며 창 하나에 여러 번 제출한다(표면 수 + 창). 최소화·크기 0 창은 건너뛴다. `--snapshot`은 창 없이 오프스크린에 그리고, 끌기 미리보기는 별도 그리기 목록을 동기 픽셀 읽기로 만든다. `--self-test`의 Surface·Viewport·WorldWidget·GameHud 시험이 장치를 직접 쓴다.
- RenderBase·RenderGraph 사실:
  - `RenderLane`이 프레임 packet(`SubmitFrame` + 캡처 없는 함수)·자원(`ReserveHandle`·`PostResource`·`UpdateTexture`·`Retire`)·표면 작업(`PostSurface`·`PostSurfaceAndWait`)·`RequestReadback`·`TakeReadback`을 준다. 처리기(`RenderLaneProcessor`)에는 시작·정지 hook만 있고, lane 작업 함수는 `RenderLaneContext`만 받는다. `RenderHandle` 종류에 스왑 체인은 없다.
  - lane 대기(`BeginFrame`의 slot 대기, `WaitMarker`)는 기다리는 동안 설정한 펌프 hook을 부른다.
  - RenderGraph는 raster pass마다 장치 기록 하나(BeginPass·BeginFrame … EndFrame)를 연다. 스왑 체인 대상(`SwapChainTarget`)은 언제나 clear로 시작한다. 그 기록 끝 `EndFrame`이 present까지 한다. Graphics에 따로 부를 Present는 없다. 그래서 같은 스왑 체인에 raster pass 둘을 두면 각자 백버퍼를 잡고 각자 present한다.
  - 그래프 `Execute`는 첫 실패에서 남은 pass를 실행하지 않는다. 그래프는 pass를 재배치하지 않는다(선언 순서 = 실행 순서). 한 pass 안에서 같은 subresource를 서로 다르게 접근하면 거부한다. 병렬 기록(RG3)의 기록 함수는 장치 생성을 부를 수 없다.
  - `GraphPassContext::Commands()`가 명령 문맥을 주므로 pass 안에서 `CopyPassTarget`(배경 흐림 캡처)을 그대로 쓸 수 있다. 대상은 pass 대상과 크기·형식이 같아야 한다.
- Sandbox 프레임(`ScreenRenderer`, 렌더 세션 소유)에는 UI pass를 끼울 자리가 없다.

### 18.2 목표 구조

UE와 같은 3단이다. 게임 lane이 Element 트리를 그리기 목록·batch로 만들고, draw buffer에 창별 결과를 담아 넘긴다. 렌더러가 그것을 프레임 arena에 복사한다. Render lane은 복사본만 읽어 그래프 pass로 기록한다. 장치 자원은 handle(게임 lane에서 안전한 값)로만 오간다.

```text
게임 lane                                   Render lane
ui::Application ── Paint ──► PaintList ──► DrawBatch
     │  SurfaceRecorder: 보관 표면·post buffer·viewport를 안쪽부터 수집
     └─► UiRenderer::DrawWindows(UiDrawBuffer) ─► UiFramePacket(프레임 arena 복사본)
                                                    │ SubmitFrame
                                                    ▼
                         그리기 대상마다 RenderGraph 하나:
                         표면·장면 pass(한 목록, 안쪽부터) → 대상 pass
                         Execute — 창 pass의 EndFrame이 그 창을 present
```

**그래프 단위.** 그리기 대상(창 또는 오프스크린 렌더 타깃)마다 그래프 하나를 만들어 실행한다. 한 창의 실패(표면 import 실패, present 실패)는 그 창만 멈춘다. UE는 그래프 하나에 스왑 체인을 여럿 두지만(SlateRHIRenderer.cpp 1485행, 8개까지), Rocks 그래프는 스왑 체인 pass마다 따로 present하므로 묶어도 얻는 것이 없다.

**모듈 경계.** UiCore·UiFramework·UiWidget·UiScreens 공개 헤더에 `graphics::`·`render::` 이름이 없다. 앞 선언도 없다. 정책 검사(`Rocks.ToolTests policy`)에 "UI Element·Widget 층 공개 헤더의 그래픽 이름공간 금지" 규칙을 더해 다시 새지 않게 한다. UiRender만 Graphics·RenderBase·RenderGraph에 의존한다. UiWorld는 UiRender에 의존하는 연결 모듈이라 규칙 대상 밖이다(18.4절).

### 18.3 UiCore 계약(UW6a)

| Rocks | UE 5.8 대응 | 계약 |
|---|---|---|
| `UiRenderer` | `FSlateRenderer` | 게임 lane 전용 추상 계약이다. 기능: draw buffer 획득·반환(`AcquireDrawBuffer`·`ReleaseDrawBuffer`), 그리기(`DrawWindows`), 창 표면 생성·크기 변경·파괴(`CreateWindowSurface`·`ResizeWindowSurface`·`DestroyWindowSurface` — UE `CreateViewport`·`RequestResize`·`OnWindowDestroyed`), 텍스처(`CreateTexture`·`UpdateTexture`·`ReleaseTexture` — UE `CreateUpdatableTexture`), 렌더 타깃(`CreateRenderTarget`·`ResizeRenderTarget`), 화면 읽기(`RequestCapture`·`TakeCapture`). 자원은 `UiTextureId`, 창 표면은 `UiWindowSurfaceId`로 돌려준다. 모든 호출이 게임 lane 하나에서만 일어나므로 UE의 자원 critical section(`GetResourceCriticalSection`, SlateRenderer.h 551행)은 두지 않는다 |
| `UiTextureId` | `FSlateResourceHandle` | 렌더러가 발급하는 불투명 64 bit 값(slot·세대)이다. 0은 없음(`NoUiTexture`). `DrawCommand::resource`가 이것이고, 지금 `SlotArray<BrushTexture>` slot을 대체한다. 크기를 바꿔도 id는 유지된다(18.4절) |
| `UiTextureDesc` | (대응 없음 — 초기 픽셀이 `FSlateTextureData`) | 너비·높이·형식(`UiTextureFormat`: `Rgba8Srgb`·`Bgra8Srgb`·`Rgba8`·`R8`)·용도(정적·갱신·렌더 타깃)·sampling 종류(`UiTextureSampling`: `Image` 직선 알파 색, `Mask` R을 덮임률로, `Premultiplied` 미리 곱한 알파 — 보관 표면 합성)를 담는다. 셰이더는 명령의 둥근 상자 여부와 이 sampling 종류로 정해진다. brush가 pipeline을 들지 않는다 |
| `UiDrawBuffer` | `FSlateDrawBuffer` | 한 번 그리기의 대상 목록이다. 대상마다 `UiDrawTarget`(창 표면 id 또는 렌더 타깃 id, 크기, clear 색, `DrawBatch` 참조, 그 대상의 표면·장면 pass 목록)을 담는다. 다시 그릴 대상만 넣는다. 최소화·크기 0 창은 넣지 않는다. 모든 창이 그대로면 `DrawWindows`를 부르지 않는다. `DrawWindows`가 내용을 프레임 arena로 복사하고 돌아온 뒤에는 버퍼를 다시 써도 되므로 버퍼는 하나다. UE가 3개를 돌리는 이유(render thread가 버퍼를 직접 읽고 나중에 놓는다 — SlateRHIRenderer.cpp 685–718행)가 없고, 흐름 조절은 `BeginFrame` slot이 한다 |
| `SurfaceRecorder` | `SRetainerWidget`의 `FWidgetRenderer` 갱신(창보다 먼저 같은 RDG에 기록, SlateRHIRenderer.cpp 1478–1483·2124–2131행)·`SViewport` 그리기 | 지금 `WidgetSurfaceHost`의 Element 순회·갱신 판정(Discover·Update)을 그래픽 없이 옮긴 것이다. 대상을 그리기 전에 트리의 보관 표면·post buffer·viewport를 자식 먼저 찾아, 종류 태그가 붙은 pass 한 목록(`UiSurfacePass`: 보관 내용 `DrawBatch` 또는 장면 packet, 대상 렌더 타깃 id, 크기, clear)을 안쪽부터 쌓는다. RetainedPanel 안의 viewport는 장면 pass가 보관 pass보다 앞에 온다. 보관 표면·viewport·post buffer의 렌더 타깃은 이 객체가 `UiRenderer::CreateRenderTarget`·`ResizeRenderTarget`으로 게임 lane에서 만들고, post buffer는 채널별 id를 위젯에 준다(`SetPostBuffer`). 창에서 떨어진 표면의 타깃은 다음 기록이나 소멸 때 돌려준다 |
| `ViewportSource` | `ISlateViewport` | `WidgetSurfaceScene`을 대체한다. `Advance(frame)`(게임 lane, World tick), `Background()`, `Extract(UiSceneExtract&, ViewportTarget)`. 그래픽 타입이 없다. 장면을 다른 처리기가 그리면(Sandbox) `Extract`가 아무것도 게시하지 않고 그 처리기의 렌더 타깃 id만 알려 줄 수 있다 |
| `UiSceneExtract` | `FSceneViewport::Draw`가 넣는 render command | 장면 source가 이번 프레임의 장면 pass를 게시하는 통로다. 형태는 `template <class T> Result<void> Post(T&& packet, UiScenePassFunction<T> record)`. `UiScenePassFunction<T>`는 `void (*)(UiScenePassContext&, T&) noexcept`(캡처 없는 함수 — `RenderLane`과 같은 이유)다. `UiScenePassContext`는 UiCore에서 불완전 타입이고 UiRender가 정의한다(그래프·대상 그래프 텍스처·크기·장치). 기록 함수를 정의하는 앱 TU만 UiRender를 포함한다. `T`는 UiCore 수준 전달 조건(`UiSceneTransferable`: 참조·포인터 아님, nothrow 이동·소멸)을 만족해야 한다. RenderBase `RenderTransferable`의 등록·스키마 검사는 UiCore가 RenderBase를 몰라 걸 수 없다(한계 — 18.8절) |
| `NullUiRenderer` | `FSlateNullRenderer` | headless 실행·CPU 시험용이다. id를 발급하고 기록만 센다(그리지 않는다) |

- `ViewportWorld::Renderer`는 `Delegate<Result<void>(World&, const ViewportCameraView&, UiSceneExtract&)>`가 된다. 게임 lane에서 World를 읽어 packet을 만들고 장면 pass를 게시한다. 장치 기록은 게시한 함수가 Render lane에서 한다. 지금 `WidgetSurfaceHost::ScenePainter`(Element를 받아 장치로 그리는 painter)는 없앤다. 그 쓰임(UiDemo viewport 시험)은 `ViewportSource` 구현으로 바꾼다.
- 이미지 brush: 앱이 장치 텍스처를 만들어 등록하던 것(갤러리 텍스처·글자 마스크)을 `UiRenderer::CreateTexture`로 바꾼다.
- 대상 하나 안의 순서: 표면·장면 pass(안쪽부터, 같은 표면은 그 프레임에 한 번) → 대상 pass. `SurfaceRecorder`가 목록 순서를 만들고 렌더러는 그 순서대로 기록만 한다.

### 18.4 UiRender 구현(UW6b)

**`LaneUiRenderer`(게임 lane, `UiRenderer` 구현).** `RenderLane&`와 lane 쪽 `UiLaneRenderer*`를 받는다. lane 쪽 주소는 표면·자원 작업 값에 담아 lane 함수가 찾게 한다. lane 객체의 주소이므로 게임 주소 전달 금지에 걸리지 않는다.
- 텍스처·렌더 타깃: `ReserveHandle` + `PostResource`로 만든다. `UpdateTexture`는 `RenderLane::UpdateTexture`, `ReleaseTexture`는 `Retire`다. `UiTextureId` → (`RenderHandle`, sampling) 표는 게임 lane에만 있다.
- 크기 변경: id는 유지하고 새 `RenderHandle`을 예약·생성한다. 이전 handle은 `Retire`한다. 이미 제출한 프레임은 복사 때 바꿔 넣은 옛 handle을 쓰고, Retire는 그 프레임의 GPU 완료 뒤 회수된다. 생성 실패(handle Failed)면 그 id의 그리기 명령을 건너뛴다(창의 다른 내용은 그린다).
- 창 표면: 생성은 `PostSurfaceAndWait`로 결과를 동기로 받는다. 그래서 도킹의 탭 띄우기가 지금처럼 실패를 바로 알고 패널을 원래 창에 남길 수 있다. 크기 변경은 `PostSurface`다. 파괴는 `PostSurfaceAndWait`가 돌아온 뒤에 native 창을 부순다. 처리되지 않은 프레임이 부서진 창에 present하지 않게 하려는 것이다(UE `OnWindowDestroyed`가 렌더 명령을 비우고 나서 창을 부수는 것과 같다, SlateRHIRenderer.cpp 835–853행). 창 표면 id → `SwapChainHandle` 표는 lane 쪽에 있다. 표면 생성 인자에 vsync를 둔다. 에디터 창 기본은 UE처럼 끔(`r.VSyncEditor` 기본 0, SlateRHIRenderer.cpp 182–197행)이다. 창 N개를 매번 vsync로 present하면 직렬로 기다리기 때문이다.
- `DrawWindows`의 순서:
  1. `BeginFrame`.
  2. 대상마다 정점·index·명령을 프레임 arena에 복사하고, 명령의 `UiTextureId`를 (`RenderHandle`, sampling)으로 바꾼다.
  3. 모든 대상·표면 기록 뒤 glyph·vector 페이지의 `UpdateTexture`를 게시한다(UE도 batch를 다 만든 뒤 `FontCache->UpdateCache()`, SlateRHIRenderer.cpp 1667행). lane은 자원 작업을 게시 순서대로 처리하므로 그 프레임보다 먼저 올라간다.
  4. `SubmitFrame`.
  5. atlas의 프레임 진행(`AdvanceFrame`).
- 프레임을 앱 처리기가 따로 잡는 경우(Sandbox — 18.6절)를 위해 `RecordWindows(FrameArena&, UiDrawBuffer&) → UiFramePacket`도 공개한다.

**`UiLaneRenderer`(Render lane).** 렌더 lane 처리기가 소유한다.
- UI pipeline(`ShaderSet`·`PipelineCache` — UW1a 셰이더), 배경 텍스처 cache, 스왑 체인 표를 가진다.
- `AddPasses(RenderGraph&, UiFramePacket&, target)`: 대상의 표면·장면 pass를 목록 순서대로 기록하고 마지막에 대상 pass를 기록한다.
  - 표면 대상은 `ImportTexture`다. 장면 pass는 기록 함수를 부르고, 깊이는 장면 함수가 그래프에서 만든다.
  - 창 대상은 `SwapChainTarget`(clear 색은 창 바탕)이다. 그 pass의 EndFrame이 present한다.
  - pass 안의 명령 순회는 지금 `DrawBatchRenderer::Draw`(clip·둥근 상자·`CopyPassTarget` 흐림)를 그대로 쓴다.
- pass 안 캡처의 세 규칙:
  - (1) 배경·post buffer 복사 대상은 그래프 밖 lane 소유 장치 텍스처로 두고 import하지 않는다. 같은 pass 안에서 CopyDest와 읽기가 함께 일어나 그래프가 거부하기 때문이다. post buffer 렌더 타깃은 게임 lane이 만든 `UiTextureId`의 handle이지만, 그 pass에서는 선언 없이 장치 handle로 쓴다.
  - (2) pipeline과 배경 텍스처는 `AddPasses`에서(기록 함수 밖에서) 준비한다. 병렬 기록 함수는 장치 생성을 부를 수 없다.
  - (3) 배경 텍스처는 (크기, 형식) 키 cache다. `CopyPassTarget`은 대상과 크기·형식이 같아야 한다.
- 정점·index: 1차는 대상마다 `graph` 버퍼 생성 + 업로드로 매 프레임 전체를 올린다. 지금의 바뀐 범위 갱신(대상별 지속 버퍼 + 바뀐 범위만 arena 복사)은 후속이다(UR3 예산 측정 때 판단).
- 다른 처리기(Sandbox `ScreenRenderer`)도 같은 함수로 UI pass를 더한다.

**`UiFramePacket`.** RenderTransferable 등록 타입이다.
- 배열은 프레임 arena Span이라 게임 쪽 주소를 들지 않는다.
- 장면 packet은 arena 원시 할당에 놓인 형식 지운 항목(기록 함수·부수기 함수)이다. `FrameArena::TryNew`(다음 `BeginFrame`의 Reset에서 게임 lane이 부순다)는 쓰지 않는다. `UiFramePacket` 소멸자가 부수기 함수를 한 번만 부른다. 이동하고 나면 원본은 비어 있다. `SubmitFrame`이 실패하면 packet이 게임 lane에서 소멸되며 같은 규칙으로 부서진다.

**glyph·vector atlas.**
- `GlyphCacheTextures`는 게임 lane에서 `text::GlyphCache`의 바뀐 페이지를 `UiRenderer::UpdateTexture`로 올린다. sampling은 Alpha → `Mask`, Color·거리장 → `Image`다.
- `VectorImageAtlas`는 래스터(게임 lane CPU)만 하고 페이지를 같은 방식으로 올린다. 보관 표면의 `RecordContent`도 벡터 이미지를 풀 수 있으므로 업로드는 위 3단계(모든 기록 뒤)다.
- 그리기 중 등록부를 바꾸는 지금 동작은 게임 lane 안의 일이라 lane 계약 위반이 아니다. 장치 쪽 `Sync(pipeline)`만 없어진다.

**지우는 것.**
- `WidgetSurfaceHost`: UiCore `SurfaceRecorder` + `UiLaneRenderer`로 대체한다.
- `DrawBatchBuffers`의 게임 `DrawBatch` 참조.
- `BrushTexture` 등록부.
- UiRender 객체의 장치 멤버 보관. `RetainedSurface`의 합성 pipeline은 `UiLaneRenderer`로, `PostBufferSet`의 크기 판정은 `SurfaceRecorder`로 옮긴다.

**UiWorld.** `WorldWidgetPresenter`(생성자에 장치·셰이더 집합, 게임 쪽 준비와 장면 pass 쪽 Submit이 한 클래스)를 두 클래스로 나눈다.
- 게임 쪽 기록기: UiCore 계약만 쓴다. Widget 표면은 `SurfaceRecorder`의 렌더 타깃이다.
- lane 쪽 평면 합성: UiRender 위의 장면 pass 함수다.

### 18.5 UiDemo를 RenderLane으로(UW6c)

- **lane 모드.** 갤러리 실행 기본은 `RenderLaneMode::Thread`다. `--self-test`·`--snapshot`은 두 모드를 모두 돌려 같은 결과인지 단언한다. Thread 모드도 명시 대기(표지·읽기)로 결정적으로 단언할 수 있고, 실제 실행 경로의 회귀를 비우지 않으려는 것이다.
- **처리기.** `UiDemoLaneProcessor`(Render lane):
  - `OnLaneStart`에서 엔진 셰이더 집합·`UiLaneRenderer`를 연다.
  - 프레임 함수가 packet의 대상마다 그래프를 만들어 `AddPasses` → `Execute`한다. present는 창 pass 안에서 끝난다. 한 대상의 실패는 기록하고 다음 대상으로 간다.
- **크기 조절 중 그리기(UW7a).** live-resize 콜백이 그 창만 담은 draw buffer로 `DrawWindows`를 부르고, `PostMarker`·`WaitMarker`로 그 프레임의 처리를 기다린다. 그래야 OS 크기 변경 루프가 다음 메시지로 가기 전에 새 크기의 그림이 present되어 있다.
  - 이 대기(콜백 안 `BeginFrame`의 slot 대기 포함) 동안 펌프 hook은 보낸 메시지만 처리한다(Win64 `PeekMessage(PM_NOREMOVE | PM_QS_SENDMESSAGE)` 수준). 일반 펌프(`PumpEvents`)를 부르면 콜백 안에서 다른 입력과 `WM_SIZE`가 처리되어 그리기가 중첩된다(UW7a 계약 위반). 펌프를 아예 끄면 Render lane의 스왑 체인 크기 변경·present가 창 스레드로 보낸 메시지를 기다리다 교착할 수 있다. UE도 같은 재진입을 겪는다(SlateRHIRenderer.cpp 785–787행).
  - `RenderLanePump`는 lane 시작 때 한 번 정해지므로, 펌프 함수가 창의 live-resize 상태를 보고 두 방식을 고른다.
  - 콜백 재진입 금지는 단언으로 지킨다.
- **캡처.** 갤러리 `--snapshot`, 끌기 미리보기, 자체 시험의 픽셀 확인은 오프스크린 대상(`UiDrawTarget`의 렌더 타깃 id)에 그린 뒤 `RequestCapture`(→ `RequestReadback`)·`TakeCapture`를 쓴다.
  - 결과를 기다리는 방법은 구현 때 RenderLane 경로로 확인한다. 후보는 `WaitFramesIdle` 뒤 `PostMarker`·`WaitMarker`다. 표지 작업이 inbox를 처리하므로 GPU 완료 뒤 도착한 읽기 결과가 ready가 된다. 명시 대기를 보장할 수 없으면 렌더 세션에 `WaitReadback(id)`를 요청한다(18.6절).
  - 끌기 미리보기는 1–2 프레임 늦게 떠도 된다(결과가 오면 `DragImage`를 만든다).
  - 지금 `DrawBatchRenderer::ReadPixels`의 오프스크린 직접 제출은 없앤다.
- **자체 시험 이관.** Surface·Viewport·WorldWidget·GameHud 시험을 lane + 오프스크린 대상 + 캡처 경로로 바꾼다. 회귀 기준은 UW6c 착수 전에 지금 코드로 같은 장치(D3D11 소프트웨어 장치)에서 갤러리 탭·자체 시험 장면을 캡처해 두고, 이관 뒤 byte 비교한다. 기준 파일은 시험 출력 폴더에 두고 저장소에 넣지 않는다.

### 18.6 Sandbox·게임 UI(렌더 세션 협의)

Sandbox `ScreenRenderer`는 렌더 세션(rocks-6a) 소유다. 게임 UI(UiScreens)를 Sandbox에 붙일 때는 `ScreenRenderer`가 장면 pass 뒤에 `UiLaneRenderer::AddPasses`를 부르게 한다. 지금 그래프는 스왑 체인 pass마다 백버퍼를 잡고 present하므로, 그대로는 장면 위에 UI를 그릴 수 없다. 렌더 세션에 다음을 요청한다.

1. 한 그래프에서 같은 스왑 체인을 대상으로 하는 raster pass 여럿이 백버퍼 하나를 공유하고, 마지막 pass에서만 present한다(뒤 pass는 load).
2. `ScreenPacket`에 UI packet 자리를 둔다.
3. (필요하면) `RenderLane::WaitReadback(id)` — 읽기 결과의 명시 대기(18.5절).
4. (선택) `RenderTransferable`의 등록 표지를 Core로 옮긴다. UiCore의 장면 packet도 같은 검사를 받게 하려는 것이다(18.3절).

UW6 구현 범위는 UiDemo까지다. Sandbox 연결은 게임 UI 단계(UT5)에서 한다. 요청은 UW6b 병합 때 보낸다.

### 18.7 시험(UW6d)

- **lane 분리.** Thread 모드 lane과 RenderBase 시험 장치(`RecordingDevice`)로 창 하나를 그린다. `DrawWindows`가 돌아온 직후 Element 트리를 파괴하고 `DrawBatch` 내용을 덮어쓴 뒤 그 프레임을 끝까지 처리한다. 기록된 그리기(정점 버퍼 내용·명령 수·index 범위)가 복사본과 같아야 한다. `UiFramePacket`은 리플렉션 스키마를 등록해 Development 전달 검사가 실제로 도는지 단언한다(스키마가 없으면 검사가 언제나 성공해 빈 단언이 된다).
- **기록 수.**
  - 프레임당 그래프 실행 수 = 그린 대상 수.
  - 대상 하나의 장치 기록 수 = 1 + 이번 프레임에 다시 그린 표면 pass 수 + 장면 pass가 연 pass 수.
  - 보관 표면 없는 창 하나는 기록 1회다. 바뀌지 않은 보관 표면은 기록 0회다. 바뀐 창이 없으면 `SubmitFrame` 0회다.
  - 9.1절 표의 "제출 수 = 1/프레임"은 이 뜻으로 고친다. RenderGraph가 raster pass마다 기록 하나를 연다(18.1절).
- **실패 격리.** 창 둘 중 하나의 표면 생성이 실패해도(시험 장치 실패 주입) 다른 창은 그려진다.
- **수명.** 표면 파괴 뒤 그 창 표면 id로 게시된 그리기가 없다. 크기 변경 전에 제출한 프레임은 옛 handle을 쓰고, 옛 handle은 그 프레임 완료 뒤 회수된다(`retiresCompleted`).
- **순서.** RetainedPanel 안 viewport: 장면 pass가 보관 pass보다 먼저 기록된다(기록 순서 단언).
- **headless.** `NullUiRenderer`로 Application 프레임을 돌린다. id 발급·반환 짝(누수 0)을 단언한다.
- **경계 검사.** 18.2절 policy 규칙의 양성·음성 fixture.
- **회귀.** 18.5절 기준 캡처와 byte 비교(두 lane 모드).

### 18.8 버린 대안과 한계

- **처리기 없이 게임 lane이 장치를 계속 쓰기(지금 방식 유지)**: 버린다. 3.4절 위반이고, 엔진 Render lane과 같은 장치를 쓸 수 없다.
- **Render lane이 Element 트리를 직접 순회(UiRender에 `WidgetSurfaceHost` 유지)**: 버린다. Element는 게임 lane 소유이고 UE도 Render thread에서 위젯을 읽지 않는다.
- **장면 렌더링을 UI 밖에 두고 텍스처만 참조(UE의 분리 방식 그대로)**: 기본 경로로는 버린다. Rocks는 lane 프레임 하나 안에서 장면 → 합성 순서를 지키는 편이 단순하다. 그래서 `UiSceneExtract`로 같은 프레임에 장면 pass를 싣는다. 다른 처리기가 장면을 그리는 경우는 `ViewportSource`가 그 렌더 타깃 id만 돌려주는 경로로 남긴다.
- **프레임 전체를 그래프 하나로**: 버린다. `Execute`가 첫 실패에서 멈춰 창 하나의 실패가 모든 창을 멈춘다. 스왑 체인 pass가 각자 present하므로 묶는 이득도 없다.
- **창 pass를 흐림 지점마다 나누기**: 버린다. 장치 `CopyPassTarget`이 pass 안 캡처를 계약으로 주고, 그래프 pass 안에서 그대로 쓸 수 있다.
- **draw buffer 3개 순환(UE 그대로)**: 버린다. 복사한 뒤 바로 재사용할 수 있다(18.3절).
- **한계.**
  - 장면 packet은 UiCore 수준 조건(`UiSceneTransferable`)만 검사한다. 게임 주소를 담은 구조체(World 포인터 필드)는 컴파일 검사를 통과한다. RenderTransfer 표지가 Core로 오면 같은 검사로 바꾼다.
  - 스왑 체인 백버퍼 공유가 생기기 전에는 장면 위 UI 합성을 Sandbox에서 할 수 없다(18.6절).
  - 정점은 매 프레임 전체 업로드다(바뀐 범위 갱신은 후속).
  - 텍스처 영역 갱신은 UW2와 같이 전체 갱신이다(GX8 대기).
