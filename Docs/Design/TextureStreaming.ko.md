# 텍스처 스트리밍 설계

기준일: 2026-11-19, 개정 4. [엔진 구현 계획](EngineImplementationPlan.ko.md) 9.5절 "필수 Texture Streaming"을 구현 스펙으로 정한다. 계획 9.5절은 요구와 수용 시험(TX01–TX08)을 정하고, 이 문서는 구조·계산식·실행 단계·Graphics 요구·단계를 정한다. 사용자 분담 결정(2026-11-16 "이것도 니가해")에 따라 렌더 세션이 맡는다. 압축·색 보정은 에셋 세션 몫이다.

UE 근거는 UE 5.8.4(`release`, commit `f45729c0b6f097038e72b52d366a62baeca3275b`, 로컬 클론)의 현재 기본 경로다. 경로는 `Engine/Source/Runtime/` 기준이며 다음 약어를 쓴다.
- `ES/` = `Engine/Private/Streaming/`
- `EP/` = `Engine/Private/`

UE가 하위 호환으로 남긴 옛 지표 경로와 deprecated 경로는 보지 않았다. UE 이름은 근거 열과 "UE 5.8 방식" 절에만 적는다.

관계 문서:
- [렌더 기반](RenderBase.ko.md)("RenderBase"): 5절 렌더 lane·완료 통지, 6절 자원 수명·`RenderHandle`·`Retire`, 6.5절 텍스처 플랫폼 데이터 입력 계약, 4.4절 GX 항목, 7.4절 그래프 자원 수명, 18.1절 화면 마일스톤.
- [에셋 시스템](AssetSystem.ko.md): 12.4–12.5절 텍스처 설정과 LOD bias.

상태: **설계**. 착수는 화면 마일스톤 뒤다(RenderBase 18.1절, 이 문서 10절). 화면 마일스톤은 상주 꼬리와 전체 상주만 쓴다.

## 1. 결정 요약

| 항목 | 결정 | 근거 |
|---|---|---|
| 두 층 | **상주 계획**(무엇을 몇 mip 상주시킬지. CPU 전용, 결정적)과 **mip 전송**(정해진 목표를 IO·업로드·교체로 실행)을 나눈다. 상주 계획은 기록 장치(fake)로 시험한다 | UE도 비동기 계산 task(`ES/AsyncTextureStreaming.cpp:1046–1104`)와 갱신 객체(`Engine/Public/RenderAssetUpdate.h:42–59`)를 나눈다. 계획 9.5절 "CPU fake" |
| 필요 mip 식 | texel 계수 × 화면 크기 ÷ AABB 표면까지 거리로 크기를 구하고 mip 수로 바꾸는 UE 식을 따른다(4.2절) | `ES/TextureInstanceView.cpp:355–433`, `ES/StreamingTexture.cpp:305–311` |
| 화면 크기 입력 | **표본 해상도** = 렌더 폭 × 2^(−텍스처 mip bias). temporal upscaler는 음수 mip bias를 걸므로 출력 해상도와 같아진다. mip bias가 없는 Native·Spatial 경로는 렌더 해상도가 된다 | UE는 출력 view 폭을 쓴다(`EP/UnrealClient.cpp:1772–1780`). Rocks는 bias가 없는 경로의 과요구를 피한다 |
| 방향 정보 | frustum을 쓰지 않는다. 전방위 거리와 "확정된 화면 그림" 신호로 보이는 것과 숨은 것을 나눈다 | UE view 입력은 위치·화면 크기·boost뿐이다(`EP/ContentStreaming.cpp:603–661`). 카메라가 돌 때 mip이 튀지 않는다 |
| 예산 | 상주 풀·pin 예약·전환 여유분·임시 peak·IO·decode staging·업로드 대역을 따로 센다. Rocks 자체 할당 회계가 기준이다 | 계획 9.5절. UE는 임시 예산 하나에 RHI 계측을 섞는다(`ES/AsyncTextureStreaming.cpp:656–684, 970–1019`) |
| 실행 전략 | 1차는 **재할당 교체** 하나다(새 mip 수의 텍스처 생성 → 공유 mip GPU 복사 → 새 mip 업로드 → 교체). 모든 backend 공통이다 | UE의 Vulkan·OpenGL 경로가 이것뿐이다(`ES/Texture2DStreamIn_IO_AsyncReallocate.cpp:11–91`, `RHI/Private/RHICommandList.cpp:2067–2125`) |
| 교체 게시 | 렌더 lane이 프레임 경계에서 바인딩 snapshot을 새 generation으로 게시한다. 그리기는 논리 handle을 들고 그래프 구성 때 snapshot으로 해석한다. descriptor를 제자리에서 덮어쓰지 않는다 | 계획 9.5절. UE는 간접 참조를 명령 순서로 바꾸고, D3D12 bindless는 새 heap을 할당한다(`D3D12RHI/Private/D3D12TextureReference.cpp:109`, `D3D12RHI/Private/D3D12BindlessDescriptors.cpp:548–550`) |
| 요청 식별 | 텍스처당 진행 중 전송 1개 + (asset revision, device generation, 요청 generation)으로 늦은 결과를 거른다 | 계획 9.5절. UE는 1개 제한만 둔다(`EP/StreamableRenderAsset.cpp:307–333`) |
| 대기 | 모든 단계는 완료 이벤트로 진행한다. sleep 폴링·시간 초과에 기대지 않는다 | UE 전역 정지는 sleep 폴링이므로(`ES/RenderAssetUpdate.cpp:75`) 따르지 않는다 |
| 대상 종류 | 1차는 2D·2DArray·Volume. Cube·CubeArray는 비스트리밍(UE와 같다) | `Engine/Classes/Engine/StreamableRenderAsset.h:92–108`, `Developer/TextureFormat/Public/Interfaces/ITextureFormat.h:49–63` |

## 2. 용어 대응

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| TextureStreaming(모듈) | `Engine/Private/Streaming` | 상주 계획과 mip 전송 |
| 상주 계획 `ResidencyPlanner` | 비동기 mip 계산 task | 필요 mip·예산·요청 목록을 계산한다 |
| 사용 기록 `TextureUseRecord` | texture instance(bounds, texel factor) | 텍스처가 장면에서 쓰이는 한 곳 |
| 관찰 지점 `StreamingViewpoint` | streaming view info | 위치·표본 해상도·boost |
| 상주 꼬리 `ResidentTail` | non-streaming mips | 늘 상주하는 가장 작은 mip들 |
| 상주 고정 `ResidencyPin` | force mip levels resident | owner·목표·기한·예산 승인을 가진 고정 요청(계획 9.5절 pin) |
| mip 전송 `MipTransfer` | render asset update | 텍스처 하나의 mip 올림·내림 작업 |
| 바인딩 snapshot `TextureBindingSnapshot`(계획 9.5) | texture reference + resource state | 물리 텍스처·논리 크기·상주 구간·generation |
| 스트리밍 staging pool | 잠긴 텍스처 메모리·IO buffer | IO 목적지 겸 업로드 원천 |

## 3. UE 5.8 방식 요약

### 3.1 정책

**프레임 갱신.**
- 관리자는 매 프레임 호출된다(`EP/ContentStreaming.cpp:820–857`). 갱신은 기본 5프레임에 나눠 한다(`ES/TextureStreamingHelpers.cpp:284–287`).
- 비동기 계산 task는 stage 0에서 직전 주기의 데이터로 시작해 이후 stage와 병행한다. 끝나면 요청을 낸다(`ES/StreamingManagerTexture.cpp:2040–2056, 2104–2123`).
- task 안의 순서: view 계산 → 화면 크기 → 이상적 mip → 예산 → 요청 목록(`ES/AsyncTextureStreaming.cpp:1046–1104`).

**수요.**
- 텍스처 사용은 (bounds, texel factor, 강제 여부)다.
- texel factor = UV 밀도 ÷ 재질 sampling scale의 최대값 × 객체 scale(`EP/Materials/MaterialInterface.cpp:2479–2502`).
- UV 밀도는 삼각형별 √(면적/UV 면적)의 가중 평균이며, 위·아래 10%를 버린다(`ES/UVChannelDensity.h:36–60`).
- 정적 객체는 레벨 빌드 데이터로, 나머지는 동적 관리자로 다룬다(`ES/LevelTextureManager.cpp:149–270`).

**필요 mip 식.**
- view마다 화면 크기 = min(화면 폭, 상한) × 0.5 × boost. boost 가운데 1.5(`MaxHiddenPrimitiveViewBoost`)를 넘는 부분은 보이는 primitive에만 적용한다(`ES/AsyncTextureStreaming.cpp:86–104`).
- 사용 기록마다 크기 = texel factor × 화면 크기 ÷ AABB 표면까지 거리. HLOD 부모 거리 범위 mask가 있다(`ES/TextureInstanceView.cpp:355–433, 479–482`).
- 텍스처마다 최대값 × 0.71(`ES/StreamingTexture.h:145–149`), mip 수 = clamp(ceil(1 + log2(max(1, 크기))), 최소, 최대)(`ES/StreamingTexture.cpp:305–311`).
- 최근 0.5초 안에 화면에 그려진 것만 "보임"이다(`ES/StreamingManagerTexture.cpp:1880–1883`).
- 숨은 것은 크기 × 0.5로 한 mip 낮춘다. Terrain·강제 상주·LooksLowRes는 낮추지 않는다(`ES/StreamingTexture.cpp:364–372`).

**허용 범위.**
- 최대 = 쿠킹 mip 수 − 런타임 bias. 쿠킹은 cinematic mip을 남겨 두고, 시네마틱이 아니면 런타임 bias로 다시 뺀다(`EP/TextureDerivedData.cpp:1342`, `ES/StreamingTexture.cpp:187–193`).
- 최소 = 상주 꼬리 mip 수와 그룹 스트리밍 mip 수에서 정한 값(`ES/StreamingTexture.cpp:223–246`).
  - 상주 꼬리 = max(1, 형식 mip tail, 최소 상주 수 7). 블록 압축은 4×4 규칙을 따른다(`Developer/TextureFormat/Public/Interfaces/ITextureFormat.h:157–189`, `Engine/Config/BaseEngine.ini:2037`).
- 비스트리밍: NeverStream, UI 그룹, mip 1개, 2의 거듭제곱 아님, NoMipmaps, Cube(`EP/Texture.cpp:1833–1890`).
- `MipLoadOptions`: OnlyFirstMip은 bias 적용 뒤 첫 mip만 남기고 스트리밍을 끈다. Default·AllMips는 일반 경로다(`EP/Texture2D.cpp:776–795`).

**예산.**
- 풀 크기(`ES/StreamingManagerTexture.cpp:1818–1846`)에서 다음을 뺀다: 장치 계측 할당과 계산상 상주 크기의 차이, 여유분(`ES/AsyncTextureStreaming.cpp:656–684`).
- 넘치면 유지 우선순위가 낮은 것부터 한 mip씩 깎는다(`:740–806`). 가중치(`ES/StreamingTexture.cpp:392–406`):
  - 강제·terrain·LooksLowRes +2048
  - 보임 +1024
  - "huge가 아님"(8 MiB 미만, Lightmap·Shadowmap 제외) +512
  - 높은 우선순위 그룹 또는 200 KiB 이하 +256
  - 숨음은 마지막 렌더 시간에 따라 감소
- 남으면 이미 상주한 mip을 유지한다(사실상의 강등 hysteresis, `ES/AsyncTextureStreaming.cpp:552–589`).

**요청.**
- 텍스처당 진행 요청은 1개다(`ES/StreamingTexture.cpp:574`).
- 임시 메모리 예산은 50 MB이며 새 텍스처 전체 크기로 센다. 진행 중 요청이 없으면 상한을 넘어도 방향별로 1건을 허용한다. 그러지 않으면 큰 요청이 끝나지 않기 때문이다(`ES/AsyncTextureStreaming.cpp:970–1019`).
- 내림 요청을 먼저 낸다.
- 로드 우선순위(`ES/StreamingTexture.cpp:520–536`):
  - 보임 +1024
  - 강제·높은 우선순위 +512
  - 부족 mip이 2보다 많음 +256(LooksLowRes는 1보다 많음)
- 512 px 이상이면 보이는 mip까지 먼저 요청한다(`:500–509`).

### 3.2 실행

**갱신 객체.**
- 단계마다 다음을 등록하는 상태 기계다(`Engine/Internal/Streaming/RenderAssetUpdate.inl:22–107`).
  - 성공하면 실행할 스레드·함수
  - 취소되면 실행할 스레드·함수
- 대기 계수가 0이 되면 다음 단계가 지정 스레드에 예약된다.
- 게임 쪽 상주 수는 성공 통지 뒤에 갱신한다. 취소되면 요청 수를 상주 수로 되돌린다(`EP/StreamableRenderAsset.cpp:244–281`).

**스트림인(재할당).**
1. 렌더 스레드에서 새 mip 수의 텍스처를 만든다. 새 텍스처는 CopyDest, 옛 텍스처는 CopySrc로 전이한 뒤 공유 mip을 GPU 복사한다(`ES/Texture2DUpdate.cpp:62–81`, `RHI/Private/RHICommandList.cpp:2086–2100`, `RHI/Private/RHI.cpp:2146–2186`).
2. 새 mip을 쓰기 잠금하고, 잠긴 메모리를 목적지로 bulk 묶음 IO를 낸다(`ES/Texture2DStreamIn.cpp:68–89`, `ES/Texture2DStreamIn_IO.cpp:54–152`).
3. 잠금을 풀어 업로드를 기록하고 교체한다.

**스트림인(D3D12 비동기 생성).** CPU 버퍼에 읽는다. 다른 스레드에서 새 mip만 초기 데이터로 넣어 copy queue로 생성한다. 그다음 공유 mip을 복사하고 교체한다(`ES/Texture2DStreamIn_IO_AsyncCreate.cpp:11–81`, `D3D12RHI/Private/D3D12Texture.cpp:1154–1400`). Vulkan·OpenGL은 이 경로를 지원하지 않는다.

**교체.**
- 렌더 스레드가 텍스처를 바꾸고 간접 참조를 갱신한다. 재질은 간접 참조를 묶으므로 uniform buffer를 다시 만들지 않는다(`EP/Rendering/StreamableTextureResource.cpp:244–265`, `EP/Materials/MaterialUniformExpressions.cpp:1712–1729`).
- D3D12 bindless는 descriptor를 갱신하면서 GPU가 쓰는 heap을 피해 새 heap을 할당한다(`D3D12RHI/Private/D3D12BindlessDescriptors.cpp:548–550`).
- 구 자원은 지연 삭제가 모든 queue sync point 뒤에 해제한다(`D3D12RHI/Private/D3D12RHI.cpp:319–380`).

**취소.**
- 계산 task가 대상을 정하고(`ES/AsyncTextureStreaming.cpp:958–967`) 게임 스레드가 취소를 표시한다.
- 진행 중 IO·GPU 작업이 끝난 뒤 취소 함수가 정리한다. 순서는 공급자 → 할당자다(`ES/TextureStreamIn.cpp:546–611`).

**종류.**
- 2D는 세 전략을 모두 쓴다.
- 2DArray·Volume은 CPU 버퍼 + 재할당이다(`ES/Texture2DArrayStreaming.cpp:27–109`).
- Cube·CubeArray는 스트리밍하지 않는다.
- 부분 상주(virtual)는 기본 플랫폼에서 꺼져 있다(`Core/Public/HAL/Platform.h:364–365`).

## 4. 상주 계획

### 4.1 입력

**사용 기록.** (bounds, texel 계수, 강제 여부, 사용자 handle, 소속 World)다. WorldIntegration이 렌더 컴포넌트를 등록할 때 만든다.
- texel 계수 = 메시 UV 밀도 × 객체 scale ÷ 재질 sampling scale이다.
- 메시 UV 밀도는 쿠킹 때 계산한다. 식은 UE와 같다(10% 절사 가중 평균).
- 재질 sampling scale: 머티리얼 시스템이 생기기 전에는 명시 값 칸만 두고 기본 1(UV0)을 쓴다.
- 동적 UV(애니메이션·world 좌표 UV)는 명시 scale·bias와 보수적 상한을 받는다(계획 9.5절. UE에는 없다).
- 정적·동적 구분과 레벨 빌드 데이터는 후속이다. 1차는 모든 기록을 동적으로 다룬다.

**관찰 지점.** (위치, 표본 해상도, boost)다. 렌더 경로의 view마다 하나다(계획 11.1절 ViewDesc).
- 표본 해상도 = 렌더 폭 × 2^(−텍스처 mip bias)다. temporal upscaler는 음수 mip bias로 출력 해상도 기준 표본을 고르므로 출력 폭과 같아진다. Native·Spatial 경로는 렌더 폭이다.
- boost: 기본 FOV보다 좁으면 1/clamp(FOV 비, 0.2, 1)이다(UE와 같다). 여기에 설정 boost를 곱한다. 1.5를 넘는 boost는 보이는 기록에만 적용한다(UE와 같다).
- 위치만 있는 보조 지점(시네마틱 look-ahead, 계획 9.5절)과 capture·split-screen 계수는 TS4다.

**보임 신호.**
- 렌더러가 사용 기록마다 "마지막으로 화면에 그린 프레임 번호"를 완료 통지로 돌려준다(RenderBase 5.2절).
- 상주 계획은 **프레임 F−k까지 확정된 통지**만 쓴다. k는 프레임 slot 수다. 그래서 렌더 lane 모드와 인라인 모드의 결과가 같다(RenderBase RB1 기준).
- 판정 창은 프레임 수 설정이며, 기본은 UE 0.5초에 해당하는 프레임 수다.
- 보임 신호 기록이 들어오기 전에는 모든 기록을 보임으로 본다.

**여러 World.**
- 한 장치에는 풀이 하나다. EditorWorld·PIE·게임 World(계획 11.1절)의 관찰 지점을 모두 합친다.
- 관찰 지점에 World별 가중치를 둔다(예: 포커스 없는 편집기 viewport는 boost 0.5). 사용 기록은 소속 World의 관찰 지점에만 대응한다.
- 가중 규칙의 세부는 TS4다.

**텍스처 메타.** RenderBase 6.5절 플랫폼 데이터 메타(mip 수, mip별 바이트 크기와 hash, 상주 꼬리 수, 차원, 형식)와 AssetSystem 12.4절 설정(4.4절)이다.

### 4.2 계산식

- 관찰 지점 v의 화면 크기: `S_v = min(sampleWidth_v, maxEffectiveScreenSize) × 0.5 × boost_v`.
- 사용 기록 i의 크기: `size_i = max_v(texel_i × S_v / max(d(v, AABB_i), 1))`. d는 AABB 표면까지 거리다. 보이는 기록의 최대값은 따로 둔다.
- 텍스처마다 다음을 구한다.
  - `size = max_i size_i × assetScale`. assetScale 기본값은 UE와 같은 0.71이고, 그룹별로 덮어쓸 수 있다.
  - `mips(size) = clamp(ceil(1 + log2(max(1, size))), minMips, maxMips)`.
  - 이상적 mip = max(보이는 기록의 mips, 숨은 기록의 mips(size × 0.5)). Terrain·pin·"저해상도로 보임"(요구가 최대 해상도의 2배 이상)인 텍스처는 숨은 축소를 하지 않는다(UE와 같다).
  - pin(4.6절)은 목표 mip이다.
- `ceil(log2(x))`는 부동소수 지수 비트로 정확히 구한다(Core `PlatformMath`). 호스트 사이 결과가 같다.
- 정렬 키(예산 축소·로드 순서)는 늘 텍스처 id를 마지막 키로 넣은 전순서다. 동점에서 호스트·스레드마다 순서가 달라지지 않는다.

### 4.3 허용 범위와 비스트리밍

**최대.** `maxMips` = 쿠킹 mip 수 − 런타임 bias(설정 전역 bias + 예산 bias + 비시네마틱 감산)이다.
- 쿠킹 때 정한 LOD bias·최대 크기는 이미 잘렸으므로 다시 더하지 않는다.
- 시네마틱 mip: 쿠킹은 min(`NumCinematicMipLevels`, 쿠킹 LOD bias)개를 남겨 둔다. 시네마틱 pin이 아닐 때 런타임 bias로 그만큼 뺀다(UE와 같다. AssetSystem 12.5절 개정, 12절).

**최소.** `minMips` = max(상주 꼬리 mip 수(메타), 그룹 스트리밍 mip 수에서 정한 하한)이다.

**상주 꼬리 기본값.** RenderBase 6.5절이 이 문서로 넘긴 결정이다.
- max(1, 형식 mip tail 수, 최소 상주 mip 수 7)다.
- 블록 압축 형식은 스트리밍 mip의 가장 작은 것이 블록 4×4 이상이 되도록 늘린다(UE와 같다).
- 쿠킹이 메타에 기록한다.

**비스트리밍(전체 상주)** 대상:
- NeverStream, UI 그룹, mip 1개, NoMipmaps
- Cube·CubeArray
- 2의 거듭제곱이 아닌 크기(1차, UE와 같다)
- `MipLoadOptions` = OnlyFirstMip(첫 mip 하나만 쿠킹)
- Availability = CPU(GPU에는 placeholder만 있다)
- "전체 상주 전용" 표시(5.3절)

### 4.4 텍스처 설정 입력(AssetSystem 12.4절)

| 설정 | 쓰임 |
|---|---|
| `LODGroup` | 그룹 표 조회. 그룹 의미: Skybox 강제 상주, UI 비스트리밍, Terrain 축소 제외, Lightmap·Shadowmap 크기 계수, Character 계열 높은 우선순위 |
| `NeverStream` | 비스트리밍 |
| 전역 강제 상주 | 영구 pin(4.6절) |
| `NumCinematicMipLevels` | 시네마틱 pin일 때만 남겨 둔 mip을 허용(4.3절) |
| `MipGenSettings`(NoMipmaps)·크기 | 비스트리밍 판정 |
| `MipLoadOptions` | Default·AllMips는 일반 스트리밍. OnlyFirstMip은 비스트리밍 단일 mip(UE와 같다, AssetSystem 12.4.1절과 일치) |
| `VirtualTextureStreaming` | 이 체계 대상에서 제외(가상 텍스처는 후속) |
| `Availability` | CPU면 비스트리밍 |
| 그룹 `NumStreamedMips`·`HighPriorityLoad`·품질 등급별 최대 크기 | 하한·우선순위·품질 상한 |

### 4.5 예산

계획 9.5절대로 각 항목을 따로 센다. 단위는 Rocks 자체 할당 회계다. RenderBase 6절 자원 소유자가 장치 할당 크기 조회(GX1)로 기록한다. 장치 계측과 계산값을 섞지 않는다. sampler LOD clamp는 메모리 회수로 세지 않는다(계획 9.5절).

| 항목 | 내용 |
|---|---|
| 상주 풀 | 스트리밍 텍스처의 상주 mip 합. 상주 꼬리와 비스트리밍 텍스처는 별도 고정분으로 세고 풀에서 미리 뺀다. pin 텍스처의 mip은 pin 예약에서만 센다(두 번 세지 않음) |
| pin 예약 | 승인된 pin의 목표 mip 합(4.6절) |
| 전환 여유분 | 일반 승인이 쓰지 못하는 예약분. 예산이 가득 차도 축소 전환(새 작은 텍스처 생성)이 진행되게 한다 |
| 임시 peak | 진행 중 전환의 구+신 동시 할당 바이트. 새 전환은 할당 직전에 승인받고, 넘으면 다음 프레임으로 미룬다 |
| IO·decode staging | staging pool과 압축 해제 버퍼 사용량(5.4절) |
| 업로드 대역 | 프레임당 업로드 바이트 상한 |

**큰 전환의 기아 방지.** 임시 peak 상한은 "가장 큰 단일 전환(구+신)" 이상이어야 한다. 설정 검증이 이를 확인하고, 어기면 오류를 보고한다. 상한보다 큰 전환이 필요한 텍스처는 그 텍스처의 최대 mip을 예산 bias로 상한 안까지 낮추고 진단에 표시한다. UE의 "방향별 1건 예외"처럼 상한을 넘기지 않는다(TX03 상한 준수).

**배분.**
1. 이상적 mip 합이 (상주 풀 − pin 예약)을 넘으면 유지 우선순위 오름차순으로 한 mip씩 깎는다. 가중치 구성은 3.1절 UE와 같다. 숨음 감소는 마지막으로 그린 뒤 지난 프레임 수로 센다. 깎을 수 없는 것(pin·Terrain)은 목록에서 뺀다.
2. 예산이 남으면 이미 상주한 mip은 내리지 않는다(강등 hysteresis).
3. 유효 예산은 줄 때는 즉시 줄이고, 늘 때는 증가분이 (임시 peak 상한 + 여유분)보다 커야 늘린다(UE와 같다).
4. 텍스처별 최대 해상도 예산 bias(UE per-texture bias)는 TS4다.

**포화와 실패.**
- **전환 여유분도 쓸 수 없을 때**(축소할 새 텍스처조차 만들 수 없음): 그 텍스처의 바인딩 snapshot을 이미 상주한 typed builtin(RenderBase 11.1절)으로 바꾼다. 구 backing은 기록·fence가 끝난 뒤 해제하고, 상주 꼬리를 다시 만든다(계획 9.5절). 진단에 사유를 남긴다.
- **예산이 강제로 줄어 pin 예약보다 작아질 때**: 승인된 pin을 우선순위 낮은 것부터 철회하고 owner에게 실패를 통지한다. 조용히 품질을 낮추지 않는다.
- **장치 할당 실패**(새 텍스처 생성 실패): 그 전송을 오류로 끝내고 현재 상주를 유지한다. 실패 상한(4.7절)을 기록한다.

### 4.6 pin

- pin은 (owner, 텍스처, 목표 mip, 기한(프레임 번호 또는 무기한), 이유)다. 승인할 때 pin 예약에 목표 크기를 더한다. 예약이 넘치면 승인이 실패하고, 호출자는 명시 품질 조정이나 실패를 고른다(계획 9.5절).
- 영구 pin: 전역 강제 상주 설정, Skybox 그룹. 기한 pin: 시네마틱·capture 준비.
- 상주 꼬리는 pin 없이 늘 상주한다. 꼬리를 올리지 못하면(IO·hash 실패) 그 텍스처는 typed builtin fallback을 표시하고 오류를 보존한다. 이 상태는 `TargetReady`와 구분한다.
- 시네마틱 look-ahead(TS4):
  - 다음 shot의 관찰 지점을 미리 등록하고, 필요한 텍스처를 기한 pin으로 요청한다.
  - 준비 gate는 요구 mip의 GPU 상주(교체 게시)까지 기다리고, timeout을 품질 충족으로 보고하지 않는다.
  - 출력 job에 실제 상주 mip을 기록한다(계획 9.5절).

### 4.7 요청 목록

- 텍스처당 진행 중 전송은 1개다. 진행 중 전송이 목표와 어긋나면 취소한다(UE 조건과 같다).
  - 목표보다 2 mip 넘게 더 올리려 할 때
  - 상주 mip을 내리려 할 때
  - 너무 적게 올릴 때
- 내림 전송을 먼저 낸다.
- 올림 순서는 로드 우선순위 내림차순이다. 가중치는 3.1절 UE와 같다. 같으면 텍스처 id 순이다.
- 프레임당 새 전송 수·업로드 바이트는 4.5절 상한 안이다.
- **실패 상한.** IO 실패·hash 불일치·할당 실패가 난 mip 구간은 텍스처별 상한으로 기록한다. 그 구간은 다시 요청하지 않는다. 상한은 다음 셋으로만 풀린다(무한 재시도 금지).
  - asset revision 교체
  - device 재생성
  - 명시 재시도 명령
- 보이는 mip까지 먼저 올리는 분할 요청은 TS4다.

### 4.8 실행 주기와 결정성

- 상주 계획은 렌더 lane이 아니라 게임 쪽 작업(CP2 ParallelFor)으로 돈다.
- **주기 snapshot.** 계산 주기(기본 N = 5프레임, 설정)의 시작에 사용 기록·관찰 지점·확정 보임 신호를 고정한다.
  - 주기 중에 추가된 기록은 다음 주기에 넣는다.
  - 삭제된 기록은 즉시 목록에서 빼고, 그 기록이 기여한 결과는 이번 주기 결과에서 무시한다.
- 결과(요청 목록)는 프레임 packet으로 렌더 lane에 넘긴다.
- **결정성 계약**: "같은 고정 입력 snapshot이면 한 프레임 계산과 N프레임 분할 계산의 결과가 같고, 호스트 사이에도 같다". 입력이 주기 중에 바뀌는 경우는 이 계약 밖이다. 시험은 고정 snapshot으로 단언한다.

## 5. mip 전송

### 5.1 단계

텍스처 하나의 전송은 다음 단계를 거친다.
- 각 단계는 실행 lane과 취소 처리 lane을 함께 가진다.
- 진행은 대기 계수가 0이 되는 완료 이벤트로만 한다: IO `AsyncIoEvent`, GX7 fence 완료 이벤트, 렌더 lane 작업 완료.

| 단계 | lane | 올림 | 내림 |
|---|---|---|---|
| 1 승인 | 렌더 lane | 임시 peak·staging·업로드 예산 승인. 실패하면 다음 프레임으로 미룬다 | 임시 peak 승인 |
| 2 할당 | 렌더 lane | 새 mip 수의 장치 텍스처 생성(처음 상태 CopyDest), staging 확보 | 새 작은 텍스처 생성 |
| 3 읽기 | IO 완료 worker | RenderBase 6.5절 스트리밍 덩어리를 staging에 비동기로 읽는다. 패키지 압축이 있으면 Core 압축 모듈로 해제한 뒤 staging에 쓴다 | 없음 |
| 4 검증 | IO 완료 worker | 덩어리 크기와 쿠킹 메타의 hash를 Core 공용 hash로 대조한다. 다르면 `CorruptEntry`로 끝낸다(TX01) | 없음 |
| 5 기록 | 렌더 lane | 구 텍스처를 복사 원본으로 전이(GX5), 공유 mip GPU 복사(구 → 신), staging → 새 mip 업로드, 새 텍스처를 SRV로 전이, 제출. 다음 프레임 그래프보다 먼저 제출한다 | 공유 mip GPU 복사와 전이 |
| 6 게시 | 렌더 lane | 다음 프레임 경계에서 바인딩 snapshot을 새 generation으로 게시, 구 텍스처 `Retire` | 같다 |
| 7 정리 | 렌더 lane | 제출 fence 완료 뒤 staging 반환, 완료 통지(상주 mip 수) | 같다 |

- **resident 선언.** 기록은 같은 graphics queue 순서로 그래프보다 먼저 제출된다. 그 뒤에 게시하므로 이후의 그리기는 업로드된 mip을 본다. 그래서 "IO 완료만으로 resident 선언 금지"(계획 9.5절)를 지킨다. copy queue를 쓰는 후속 전략은 cross-queue fence 완료 이벤트 뒤에 게시한다.
- **게임 쪽 상주 수.** 완료 통지를 받은 뒤 갱신한다. 취소되면 요청 수를 상주 수로 되돌린다(UE와 같은 두 사본 방식).

### 5.2 취소와 세대

- 요청 식별은 (asset revision, device generation, 요청 generation)이다.
- **취소는 표시만 한다.** 진행 중 IO·GPU 작업이 완료 이벤트(성공이든 오류든)를 보낸 뒤, 그 단계의 취소 처리가 자원을 정리한다. 정리 순서는 IO 목적지(staging)를 쓰는 쪽이 먼저, 할당 쪽이 나중이다(UE와 같다).
- **늦은 결과.** IO 완료나 업로드 완료가 오면 식별자를 현재 요청과 대조한다. 다르면 게시하지 않고 정리만 한다. 제출된 업로드는 취소만으로 staging을 돌려주지 않고 fence 완료(또는 장치 손실 오류 완료)까지 보존한다(계획 9.5절).
- **부분 성공.** 계획상 취소(목표 변경)에만 허용한다. asset revision과 device generation이 그대로이고 업로드 기록이 이미 제출되었으면, 게시를 끝내고 완료로 보고한다. 장치 텍스처와 게임 쪽 상태를 맞추기 위해서다(UE와 같은 판단). revision이나 device가 바뀐 취소는 절대 게시하지 않는다(TX04 stale 게시 0).
- **asset revision 교체(재임포트).** 진행 중 전송을 모두 취소한다. 공유 mip을 GPU 복사하지 않는다(내용이 다르다). 새 메타의 상주 꼬리로 새 자원을 만들고, 바인딩 snapshot을 그 자원으로 게시한 뒤 다시 계획한다.
- **장치 손실.** GX7이 기다리는 쪽을 오류 완료로 깨운다. 진행 중 전송은 오류 완료로 정리 단계까지 진행한다.
  - 예산 회계(임시 peak·staging)를 0으로 되돌린다.
  - 게임 쪽 상주 수를 상주 꼬리로 되돌린다.
  - 새 장치에서 상주 꼬리부터 다시 만든다(RenderBase 6.2절 장치 재생성).
  - 승인된 pin은 새 장치에서 다시 승인한다. 실패하면 owner에게 통지한다.

### 5.3 바인딩 snapshot과 셰이더 쪽 규칙

**snapshot.**
- 렌더 lane은 텍스처마다 `{물리 텍스처, logicalExtent, logicalMipCount, residentBaseMip, residentMipCount, generation}`을 프레임 경계에서 게시한다(계획 9.5절).
- 프레임 packet과 재질·파라미터 묶음은 논리 handle(`RenderHandle`)만 든다. 그래프를 구성할 때 그 프레임의 snapshot으로 물리 텍스처를 찾아 import한다(RenderBase 7.4절).
- 게시 전에 구성된 그래프는 구 텍스처의 lease를 이미 잡고 있으므로, `Retire`가 그 그래프를 깨지 않는다.

**descriptor.**
- descriptor를 제자리에서 덮어쓰지 않는다. 프레임 slot이 2–3개라 이전 프레임이 아직 GPU에서 읽고 있기 때문이다.
- 비-bindless 바인딩(지금 규약)은 기록 때 snapshot의 물리 텍스처로 descriptor table·set을 만든다. 그래서 교체가 따로 필요 없다.
- 후속 bindless에서는 게시마다 새 descriptor index를 쓰고 옛 index는 GX7 fence 뒤에 회수한다. UE D3D12도 GPU가 쓰는 heap을 바꾸지 않는다.

**크기·mip 질의.**
- 셰이더의 텍스처 크기 질의와 재질의 텍스처 크기 입력은 논리 크기를 쓴다. 논리 크기는 텍스처마다 바뀌지 않는 값이라 파라미터 묶음의 상수로 둔다. 교체 때 상수 블록을 다시 쓰지 않는다.
- 명시 LOD 표본과 정수 `Load`는 셰이더 helper를 거친다.
  - helper는 `residentBaseMip = log2(논리 폭 ÷ textureSize(t, 0).x)`를 셰이더에서 계산한다. 논리 mip을 상주 구간으로 clamp한 뒤 물리 mip(= 논리 − residentBaseMip)으로 바꾼다.
  - `textureSize`는 모든 형식(HLSL `GetDimensions`, ESSL 3.x `textureSize`)에 있다. GLES uniform 에뮬레이션 수도 늘지 않는다.
- helper 없이 쓰는 명시 LOD·`Load`가 스트리밍 텍스처에 쓰이면 셰이더 컴파일 때 진단한다. 그런 텍스처는 "전체 상주 전용"으로 표시해 비스트리밍으로 돌린다. UE가 이 변환을 하는지는 확인하지 못했다(미확인).
- 도함수 기반 표본은 하드웨어가 상주 mip 안에서 고르므로 변환이 필요 없다.
- 모든 표본에 일률 sampling bias를 걸지 않는다(계획 9.5절).

### 5.4 staging pool과 IO

- 스트리밍 staging pool은 프레임 업로드 ring(GX8)과 따로 둔다. 2K BC3 텍스처의 mip 하나가 수 MB이므로 프레임 ring에 맞지 않는다.
- 할당 대상은 IO 목적지로 직접 쓸 수 있는 CPU 가시 메모리다. RenderBase 6.5절의 4 KiB 정렬과 맞물려, 압축이 없는 덩어리는 staging에 바로 읽는다. 패키지 압축이 있으면 CPU 버퍼(decode staging 예산)에 읽고, Core 압축 모듈이 staging에 해제한다.
- OpenGL ES는 staging이 CPU 메모리다.
  - 업로드는 `glTexSubImage*`, 블록 압축은 `glCompressedTexSubImage*`다.
  - client 메모리 업로드는 호출이 반환되면 staging을 다시 써도 된다.
  - 업로드를 pixel buffer object로 바꾸면 그 staging은 fence 뒤에 회수한다.
- 회수는 업로드 fence 완료 뒤다. 크기 상한은 4.5절 IO·decode staging 예산이다.
- IO 우선순위는 로드 우선순위(4.7절)로 정한다. 비동기 파일 읽기는 Core `Platform/AsyncFile.h`와 `Task/AsyncIoEvent.h`를 쓴다(중복 구현 금지). 덩어리가 패키지 안인지 별도 파일인지는 AssetSystem A1이 정한다.
- IO·hash 실패는 그 전송을 오류로 끝낸다. 그 텍스처는 현재 상주 상태를 유지하고 실패 상한(4.7절)에 기록한다. 상주 꼬리 실패는 4.6절 fallback이다.

### 5.5 종류별

- 2D: 재할당 전략 그대로다.
- 2DArray: 같은 단계다. 한 mip 덩어리가 모든 slice를 담고(RenderBase 6.5절 mip-major), 업로드는 slice별 subresource 기록이다.
- Volume: 같은 단계다. mip 하나가 subresource 하나(전체 depth)다.
- Cube·CubeArray: 1차는 비스트리밍이다. 6.5절 배치는 스트리밍할 수 있는 형태이므로 후속에서 다시 본다(결정 T5).

## 6. Graphics 요구(RenderBase 4.4절에 더할 것)

| GX | 요구 | UE 대응 |
|---|---|---|
| GX1 | 장치 할당 크기 조회(텍스처 desc → 정렬 포함 바이트), 능력 표시(mip 구간 복사, 다른 스레드 자원 생성, 부분 상주) | RHI 메모리 통계, `GRHISupportsAsyncTextureCreation` |
| GX4 | mip 구간 복사(원본 mip a → 대상 mip b, 모든 slice·face, mip마다 한 호출). OpenGL ES는 `glCopyImageSubData`(OpenGL ES 3.2 core, mip 지정·압축 형식 호환 복사 지원) | `CopySharedMips`(`RHI/Private/RHI.cpp:2146–2186`) |
| GX5 | 복사 원본·대상과 SRV 사이 전이 | 재할당 시 CopySrc·CopyDest 전이(`RHI/Private/RHICommandList.cpp:2086–2100`) |
| GX8 | 스트리밍 staging pool(프레임 ring과 별도, IO 목적지 가능, fence 뒤 회수) | 잠긴 텍스처 메모리·upload allocator |
| GX7 | 텍스처·descriptor·staging의 모든 queue fence 뒤 회수, 장치 손실 시 오류 완료(이미 계획됨) | 지연 삭제 queue |
| 후속 | 부분 초기화 생성(copy queue 비동기 생성용), 다른 스레드 생성·cross-queue 대기, bindless descriptor index 버전, 부분 상주(reserved/sparse) 자원 | D3D12 비동기 생성, bindless heap, virtual 텍스처 |

## 7. 계획 9.5절 요구 대조

| 9.5절 요구 | 이 문서 | UE와의 차이 |
|---|---|---|
| UV density·material scale·bounds·view 거리/투영·render resolution으로 desired mip | 4.1–4.2절. 표본 해상도(결정 T1), 투영은 FOV boost로만 | UE는 출력 폭 |
| 동적 UV의 명시 scale/bias·보수적 상한 | 4.1절 | UE에는 없다 |
| editor/game/capture/split-screen 수요의 priority·hysteresis | 4.1절 여러 World, 4.5절 hysteresis. capture·split-screen·World 가중 세부는 TS4 | UE capture 등록 경로는 미확인 |
| GPU 상주·pin·IO/decode staging·upload·temporary peak 각각 제한 | 4.5절 | UE는 임시 예산 하나 |
| sampler LOD clamp를 메모리 회수로 집계하지 않음 | 4.5절 | — |
| reduced-mip allocation 교체 | 5.1절 재할당 교체 | 같다 |
| minLOD/SRV·descriptor snapshot을 frame 경계에서 갱신 | 5.3절 | UE는 명령 순서 |
| IO 완료만으로 resident 선언 금지 | 5.1절 | 같은 방향 |
| 요청 generation·취소·늦은 결과 대조·staging 보존 | 5.2절 | UE는 세대 번호 없음 |
| lease·fence 뒤 회수 | 5.1절 7단계, GX7 | 같다 |
| 최소 tail pin, 실패 fallback, `TargetReady` 구분 | 4.6절 | UE는 fallback 없음 |
| 시네마틱 look-ahead·GPU 준비 대기·job에 실제 residency 기록 | 4.6절(TS4) | UE는 preroll 위치·기한 강제 |
| binding snapshot 논리 크기·명시 LOD·Load 변환 | 5.3절 | UE 변환 미확인 |
| 포화 시 전환 여유분·builtin 교체·무한 retry 금지 | 4.5절 포화와 실패, 4.7절 실패 상한 | UE는 1건 예외 |
| 디버그(desired/budgeted/resident·UV 오류·대기·evict 사유·peak·fallback) | 8절 | UE에는 evict 사유·fallback·peak 표시가 없다 |

## 8. 진단

- 텍스처별: 이상적·예산·상주 mip, 진행 중 전송 단계, 마지막 evict 사유(예산·숨음·취소·포화), 실패 상한, pin owner, fallback 여부.
- 풀별: 상주·고정분·pin 예약·전환 여유분·임시 peak 최대값·IO·decode staging·업로드 바이트.
- 사용 기록별: texel 계수, 거리, 화면 크기 기여.
- 진단 보기(TS4): UV 밀도 정확도, 요구 해상도 시각화.
- 명령: 텍스처 목록, 한 텍스처 추적, 일시 정지, 강제 내림, 실패 상한 해제(명시 재시도), stress 모드.
  - stress 모드는 seed를 둔 결정적 순서 주입이다. IO 완료·fence 완료 gate의 순서를 바꾸고 취소를 끼워 넣는다. 시간 지연을 쓰지 않는다.

## 9. 모듈과 의존

- `TextureStreaming`(Engine/Source/Runtime)은 상주 계획(CPU 전용, 장치 없음)과 mip 전송(RenderBase 위)을 담는다.
- 의존은 RenderBase, AssetRuntime(텍스처 메타·설정), Task, Core(압축·hash·PlatformMath·비동기 파일)다.
- World에 의존하지 않는다. 사용 기록은 WorldIntegration이 넣는다.
- 상주 계획은 장치 없이 시험한다. 사용 기록·관찰 지점·메타를 넣고 요청 목록을 단언한다.
- **실행 중 스트리밍 상태의 위치.** 자산 객체(AssetSystem 12.1절의 `StreamableRenderAsset`)에는 게임 lane만 읽고 쓰는 값만 transient로 둔다. 스트리밍 표 handle 하나(UE `StreamingIndex`에 해당)가 그것이다.
  - 다음 상태는 이 모듈의 표에 둔다: 상주 mip 수(렌더 lane이 교체를 게시할 때 바뀐다), 진행 중 전송과 요청 세대(5.2절), 갱신 대기, 강제 상주·시네마틱 mip 사용 같은 실행 중 판정, mip 변경 통지 구독. 렌더 lane이 바꾸는 값을 게임 lane 객체에 직접 쓰지 않기 위해서다(RenderBase 5절 lane 소유).
  - 게임 쪽이 상주 mip 수를 읽어야 하면 sync 지점에서 표의 값을 복사한 snapshot으로 읽는다. 저장 설정(NeverStream·전 mip 강제 상주·NumCinematicMipLevels)은 자산 필드이고, 상주 계획이 메타로 읽는다(4.4절).

## 10. 단계와 완료 기준

착수는 화면 마일스톤 뒤다. 시험은 sleep 없이 IO·fence 완료를 gate로 막아 순서를 만들고 결정적으로 단언한다.

| 단계 | 내용 | 선행 | 완료 기준(계획 TX) |
|---|---|---|---|
| TS1 | 상주 계획(4절 1차 범위: 사용 기록·관찰 지점·확정 보임 신호·식·허용 범위·비스트리밍·예산 배분·hysteresis·요청 목록·실패 상한), 진단 통계 | 화면 마일스톤, A5 메타 | TX02: 다중 지점·이동·해상도·UV 변경·비정방형 텍스처·크기 기반 UV의 이상적 mip 기준값. 고정 snapshot에서 한 프레임 계산과 N프레임 분할 계산의 결과 동일, 호스트 사이 같은 결과, 정렬 전순서 |
| TS2 | mip 전송(재할당, 2D·2DArray·Volume), staging pool, hash 검증, 바인딩 snapshot·논리 handle 해석, 셰이더 helper | TS1, GX1·GX4·GX5·GX7·GX8, Core AsyncIoContext 완료 게시 보류(시험에서 IO 완료를 gate로 막는다, `feature/render-base` 7b9395a) | TX01(손상 mip의 `CorruptEntry`), TX04(IO·업로드 취소·revision·device 교체의 stale 게시 0, 부분 성공 조건), TX05(늦은 제출·작은 staging에서 fence 전 재사용 0), mip별 색 텍스처로 올림·내림 픽셀 대조(D3D12·Vulkan·GLES 미리보기), 명시 LOD·Load 변환 시험, 게시 전후 그래프 import lease |
| TS3 | 예산 항목 분리, 임시 peak 승인·상한 검증, 전환 여유분, pin 승인·철회, 포화 시 builtin 교체, 장치 할당 실패, fallback·`TargetReady` 구분, 장치 손실 복구 | TS2 | TX03(pin·요청 포화에서 상주·임시 peak 준수, 예산 강제 감소 뒤 진척·pin 철회 통지), TX06(cold·누락·폭주에서 fallback 유지, `TargetReady` 오표시 0), TX08(이동·언로드 누수 0, mip 진동·IO·CPU·GPU 비용 계측) |
| TS4 | 시네마틱 look-ahead·기한 pin·준비 gate·job residency 기록, capture·split-screen·World 가중, 보이는 mip 먼저 분할 요청, 텍스처별 예산 bias, 진단 보기 | TS3 | TX07(Forward+·Deferred·upscale·컷씬·offscreen 준비 gate 회귀) |
| 후속 | copy queue 비동기 생성, Cube·CubeArray 스트리밍, optional mip, 레벨 정적 빌드 데이터, bindless descriptor 버전, 부분 상주, 가상 텍스처, 메시 LOD 스트리밍과 풀 공유 | — | — |

## 11. 결정 사항

2026-11-19 T1–T8을 권장안대로 확정했다(사용자 지시 "결정사항들은 권장안대로 하면됨"). T9는 권장안이 "후속에서 정한다"이므로 보류로 남긴다.

| ID | 결정 | 내용 | 상태 |
|---|---|---|---|
| T1 | 화면 크기 입력 | 표본 해상도 = 렌더 폭 × 2^(−텍스처 mip bias). temporal upscale은 출력 해상도, Native·Spatial은 렌더 해상도와 같아진다. 계획 9.5절 문구를 개정한다 | 확정(2026-11-19) |
| T2 | 방향 정보 | 전방위 거리 + 확정 보임 신호(UE와 같다) | 확정(2026-11-19) |
| T3 | 보임 신호를 화면 마일스톤 렌더 기반에 넣을지 | 넣는다. 렌더러가 사용 기록별 마지막 그린 프레임을 완료 통지로 돌려준다 | 확정(2026-11-19) |
| T4 | 재질 sampling scale 공급 | 머티리얼 시스템 전에는 명시 값 + 기본 1. 머티리얼 컴파일러가 생기면 셰이더 빌드 결과로 공급(셰이더 시스템 후속 항목) | 확정(2026-11-19) |
| T5 | Cube·CubeArray·2의 거듭제곱이 아닌 크기의 스트리밍 | 1차는 UE처럼 비스트리밍, 후속에서 다시 본다 | 확정(2026-11-19) |
| T6 | 명시 LOD·`Load` 처리 | 셰이더 helper(`textureSize`로 상주 base 계산) + helper 없는 사용은 진단하고 전체 상주로 돌린다(5.3절) | 확정(2026-11-19) |
| T7 | 교체 게시 단위 | 프레임 경계 snapshot(계획 9.5절, 결정성). 지연이 한 프레임 늘어난다 | 확정(2026-11-19) |
| T8 | 상주 꼬리 기본값 | max(1, 형식 mip tail, 7) + 블록 4×4 규칙(UE와 같다) | 확정(2026-11-19) |
| T9 | 메시 LOD 스트리밍과 풀 공유 | 후속에서 정한다 | 보류(후속) |

## 12. 다른 문서 개정

| 문서 | 위치 | 개정 | 상태 |
|---|---|---|---|
| EngineImplementationPlan | 9.5절 | "render resolution"을 표본 해상도로(T1), 이 문서 참조 | 반영(계획 개정 1.14) |
| RenderBase | 4.4절 GX1·GX4·GX5·GX7·GX8 | 6절 요구를 더한다 | GX 구현 때 함께(TS2) |
| RenderBase | 6.5절 상주 꼬리 기본값, 메타 hash | 4.3절 값으로 확정(T8), 덩어리 hash를 메타에 넣는다 | 반영됨(6.5절에 기본값 7·덩어리 hash가 이미 있다) |
| RenderBase | 5.2절 완료 통지 | 보임 신호(T3)와 전송 완료 통지 | 반영(렌더 기반 개정 25) |
| AssetSystem | A5 플랫폼 데이터 메타, 12.5절 LOD bias | 덩어리 hash, 상주 꼬리 계산 규칙(4.3절), 시네마틱 mip을 남겨 두는 쿠킹 규칙(4.3절), 메시 UV 밀도(메시 에셋 단계) | 에셋 세션에 전달(A5 착수 전 반영) |
| ShaderSystem | 셰이더 helper | 스트리밍 텍스처의 명시 LOD·Load 변환 helper와 진단(T6) | 반영(셰이더 시스템 개정 20, 5.2절) |

## 13. 개정 이력

- 개정 1(2026-11-16): 초안이다. UE 5.8.4 텍스처 스트리밍 조사 두 갈래를 종합했다.
  - 정책: 수요·식·허용 범위·예산·요청
  - 실행: 갱신 상태 기계·IO·재할당·교체·취소·RHI 요구
  - 사용자 분담 결정에 따라 렌더 세션 문서다.
- 개정 2(2026-11-16): 리뷰 1건(UE 근거 약 50곳 대조, 차단 2·중요 12·사소 다수)을 반영했다.
  - 차단:
    - 부분 성공을 같은 revision·device의 계획상 취소로 한정했다. revision 교체 때는 공유 mip을 복사하지 않는다(5.2절).
    - descriptor를 제자리에서 덮어쓰지 않는다. 논리 handle을 그래프 구성 때 snapshot으로 해석하고, bindless는 index를 버전으로 관리한다(5.3절).
  - 중요:
    - 셰이더 helper 입력: 논리 크기는 불변 상수, 상주 base는 `textureSize`로 계산한다.
    - 시네마틱 mip 쿠킹 규칙을 정했다.
    - `MipLoadOptions` 해석을 바로잡았다.
    - 큰 전환의 기아 방지: 상한 검증과 예산 bias를 두었다.
    - 포화 시 builtin 교체, pin 철회, pin 이중 집계 제거, 장치 할당 실패를 정했다.
    - 실패 상한을 두어 무한 재시도를 막았다.
    - hash 검증 단계(TX01)를 더했다.
    - 장치 손실 복구를 정했다.
    - 전이(GX5)와 제출 순서, 게시 전후 그래프 lease를 정했다.
    - 주기 snapshot·확정 보임 신호·정렬 전순서로 결정성 계약을 한정했다.
    - 여러 World(editor·PIE) 수요를 넣었다.
  - 사소:
    - 표본 해상도(T1 UpscaleMode 조건).
    - `ceil(log2)`를 지수 비트로 계산한다.
    - stress 모드를 seed 순서 주입으로 바꿨다.
    - GLES 압축 업로드와 PBO를 구분했다. Core 압축 모듈을 쓴다고 명시했다.
    - TS2 선행에서 GX2를 빼고 GX5를 넣었다.
    - 누락된 9.5절 요구를 더했다: sampler clamp 비집계, job residency, decode staging, 비정방형 시험.
    - Volume의 subresource를 정정했다.
    - 절 참조를 정정했다.
  - 사실 정정:
    - sleep 폴링 경로(`ES/RenderAssetUpdate.cpp:75`)
    - UnrealClient 줄
    - 유지·로드 우선순위 가중치의 정확한 조건
    - 비동기 task 시점
    - 숨은 축소 예외·범위 mask·보임 전용 boost
    - D3D12 bindless의 새 heap 할당
- 개정 3(2026-11-17): 9절에 실행 중 스트리밍 상태의 위치를 적었다. 자산 객체에는 표 handle만 두고, 상주 mip 수·전송·요청 세대·갱신 대기·통지는 이 모듈의 표에 둔다(AssetSystem 개정 8의 `StreamableRenderAsset` 도입에 맞춤).
- 개정 4(2026-11-19): 11절 결정 T1–T8을 권장안대로 확정했다(T9는 보류). 12절 다른 문서 개정에 상태 열을 두고 계획 9.5절(표본 해상도, 개정 1.14)·렌더 기반 5.2절(보임 신호·전송 완료 통지)·셰이더 시스템 5.2절(명시 LOD·`Load` helper와 진단)을 반영했다. 렌더 기반 6.5절은 이미 반영돼 있었고, AssetSystem A5 메타 요구는 에셋 세션에 전달한다.
