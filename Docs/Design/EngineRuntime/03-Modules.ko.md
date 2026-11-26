# 엔진 런타임 — 3. 모듈과 의존 방향

[목차](README.ko.md)

## 3. 모듈과 의존 방향

모든 엔진 모듈은 `Engine/Source/<Group>/<Module>/`에 두고 `EngineModuleRules`를 상속한다.

| 모듈 | 의존 | 책임 | 금지 |
|---|---|---|---|
| `Input` | Core | 장치 독립 입력 이벤트·프레임 입력 상태(키·마우스·게임패드), edge 판정 | 창·OS SDK 참조 |
| `PlatformWindow` | Input | 창 생성·이벤트 펌프·네이티브 핸들 노출, 입력 backend(Win32 메시지 키보드·마우스, XInput 게임패드) | Graphics·World 참조 |
| `GraphicsTypes` | Core | 그래픽 값 타입: 픽셀 형식·mip 크기 계산(`PixelFormat.h`), 필터·주소 모드·테두리 색·`SamplerDesc`(`Sampler.h`). 장치 계약·에셋·도구가 함께 쓴다 | 장치·창·World 참조 |
| `Graphics` | GraphicsTypes | 장치·스왑체인·버퍼·텍스처(mip·텍스처별 샘플러)·파이프라인·명령 기록 계약, backend 열거 | World·Object·창 모듈 참조 |
| `DxgiGraphics` | Core | D3D11·D3D12가 함께 쓰는 DXGI 어댑터 도우미(HDR 출력 판정·D3D 보장 자원 크기, GX1). 어댑터 질의는 Windows 빌드에만 있다 | Graphics·backend 참조 |
| `D3D12Graphics` | Graphics, DxgiGraphics | D3D12 구현(Windows) | 다른 backend 참조 |
| `D3D11Graphics` | Graphics, DxgiGraphics | D3D11 구현(Windows, feature level 11_0 이상, 10.2절) | 〃 |
| `OpenGLGraphics` | Graphics | OpenGL ES 3.2 구현(Windows `GLES31_PC` 미리보기 WGL, Android EGL, [OpenGL ES backend](../OpenGLESGraphics.ko.md)) | 〃 |
| `VulkanGraphics` | Graphics | Vulkan 1.1 구현(Windows·Android, 10.3절·[VulkanGraphics](../VulkanGraphics.ko.md)) | 다른 backend 참조 |
| `MetalGraphics` | Graphics | 자리만 둔다. 장치 생성은 `Unsupported`를 반환한다 | 동작하는 척하는 가짜 구현 |
| `Engine` | ObjectRuntime, World | 엔진 상태 기계·메인 루프·WorldContext·EngineSubsystem | Gameplay·Graphics·창 참조 |
| `Collision` | ObjectRuntime, CoreTypeBindings, Config | 충돌 데이터: 채널·응답·충돌 켬 상태, 충돌 profile 설정·표·해석, body 값(`BodySettings`) — [충돌 데이터](../CollisionData.ko.md). World·Mesh가 함께 쓴다 | 물리 엔진·World·Asset 참조 |
| `World` | ObjectRuntime, CoreTypeBindings, Config, Collision | World·Actor·Component·SceneComponent(WorldOnly 리플렉션 클래스 — 4.6절)·TickScheduler·WorldSubsystem | Engine·Graphics·Asset 참조 |
| `WorldTestFixtures`(Developer) | World | World 타입을 상속한 리플렉션 시험 타입(SaveActor·SaveComponent)과 그 identity 명세. EngineTests만 의존 | 제품 링크 |
| `Gameplay` | Engine, World, Input | GameInstance·GameDirector·GameState·PlayerState·Controller·Pawn·LocalPlayer·CameraDirector·PlayerStart(18–20절). 아직 없다(S10–S12). PlayerState는 모듈식 게임플레이 `PlayerStateComponent`의 소유 클래스다([컴포넌트 도입](../ComponentCatalog/09-Plugins.ko.md) CR48). PlayerController의 입력 컴포넌트 스택·햅틱 효과 관리가 입력·force feedback 컴포넌트의 선행이다([컴포넌트 도입](../ComponentCatalog/04-Prerequisites.ko.md) CR26) | Graphics 참조 |
| `AssetRuntime` | Serialization, Core | AssetId·에셋 파일 형식·AssetManager·TextureAsset | World·Graphics 참조 |
| `AssetTools`(Developer) | AssetRuntime | BMP 임포터·에셋 파일 기록 | Shipping 링크 |
| `RenderBase` | Graphics, Task, MetaSchema | 렌더 lane·프레임 slot·프레임 packet 전달·완료 통지·`RenderHandle` 예약([렌더 기반](../RenderBase/README.ko.md) RB1) | ObjectRuntime·World 참조 |
| `WorldIntegration` | World, Mesh, Material, Texture, RenderBase | 에셋을 참조하는 컴포넌트(메시 컴포넌트 D2b — 30장, SpriteComponent 등 렌더 컴포넌트), RenderScene 추출, 텍스처 GPU 캐시(`TextureRenderCache` — [렌더 기반](../RenderBase/18-ScreenMilestone.ko.md) 18.5절 8항). World 객체 → 에셋 참조는 `SoftObjectRef`(30.1절) | Engine 내부 상태 변경 |

정식 렌더러 모듈(`Renderer`, Forward+·Deferred, 계획 11절)은 아직 없다. 내장 메시·원문 셰이더로 그리던 최소 렌더러는 엔진 핵심이 아니라 시험 샘플이므로 `Programs/Sandbox/Private/SampleRenderer`로 옮겼다(2026-11-17). `Renderer` 이름은 정식 렌더러가 다시 쓴다.

Engine은 창·그래픽·게임플레이를 모른다. 이벤트 펌프와 렌더는 `EngineLoopHooks`의 확장 지점으로 실행 파일(Sandbox)이 연결한다. 헤드리스 서버·테스트는 같은 Engine을 hook 없이 실행한다.
