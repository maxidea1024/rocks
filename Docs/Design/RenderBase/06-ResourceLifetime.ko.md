## 6. 장치 자원 수명

### 6.1 UE 5.8 방식

- 렌더 쪽 자원 기반 객체가 장치 자원 초기화·해제 가상 함수와 초기화·해제·갱신 함수를 가진다(`RC/Public/RenderResource.h:36, 83–108`). 초기화 상태로 소멸하면 Fatal이다(`RC/Private/RenderResource.cpp:247–256`).
- 게임 스레드는 자원 포인터를 캡처한 명령으로 초기화·갱신·해제를 넣는다(`:290–390`).
- 전역 자원은 단계 목록에 올렸다가 RHI 준비 뒤 일괄 초기화한다(`:121–140`, 호출 예 `D3D12RHI/Private/Windows/WindowsD3D12Device.cpp:2029`). 장치 재생성 때는 목록 전체를 해제하고(`:113–118`), 기능 수준이 바뀌면 그 수준이 다른 자원만 다시 만든다(`:142–162`). iOS는 첫 초기화 뒤 목록을 비운다(`:136–139`).
- 지연 정리는 프레임 끝 동기 한 번 뒤 delete(`RC/Private/RenderingThread.cpp:1323–1419`), RHI 자원은 참조 0에서 지연 삭제 목록(`RHI/Private/RHIResources.cpp:46–58`).

### 6.2 Rocks 규칙

- **장치 자원 소유자.** 렌더 lane 쪽 객체로, 장치 handle과 상태(`Pending`·`Ready`·`Failed`·`Retired`)를 가진다. 게임 lane은 생성 요청 때 곧바로 `RenderHandle`을 예약받고(5.4절), 실제 장치 자원 생성은 렌더 lane 작업이 한다. 생성이 실패하면 `Failed`가 되고 완료 통지로 알린다.
- **준비 전 사용.** `Pending`·`Failed` handle을 그리기에 쓰면 그 그리기는 내장 대체 자원(형식·차원이 맞는 typed fallback, 계획 9.6절)을 쓰고 진단 계수를 올린다.
- **회수.** 게임 lane의 해제 요청은 `Retire` lane 작업이다. 게임 lane은 last-use를 모르므로 `Retire`는 handle만 받고, last-use는 렌더 lane과 장치의 내부 집계가 정한다(계획 11.2절 "RHI 내부 last-use 집계가 최종 기준"). `Retire`는 새 lease를 막고, 이미 승인된 기록(그래프 import·기록 packet)의 lease와 마지막으로 쓴 모든 queue의 `GpuFencePoint`가 끝난 뒤 장치 자원을 해제하며, 회수 완료를 통지한다.
- **소멸 진단.** `Ready` 상태로 소유자가 파괴되면 모든 구성에서 `Retire` 경로로 넘기고, Development에서는 진단을 더한다(누수 Gate, 계획 29.6절).
- **장치 재생성.** 장치 손실 뒤 새 device epoch에서 소유자 목록을 다시 초기화한다. 구 장치 handle은 stale이다(계획 11.2절 device epoch, 9.6절 BR04). 장치 능력이 바뀌는 경우(장치 교체)도 같은 경로다.
- **갱신.** 버퍼·텍스처 내용 갱신은 GX8 업로드 ring을 쓰는 lane 작업이다. 같은 프레임 안 갱신 순서는 lane FIFO가 보장한다.

### 6.3 내장 자원 팩 초기화

5.5절 시작 순서에서 엔진 셰이더 집합 검사 뒤, `BuiltinReady` 전에 만든다. 계획 9.6절 팩이 fallback 셰이더(cooked shader variant)를 포함하므로 셰이더 집합 검사가 먼저다. UE와 다른 점: 정적 초기화 목록에 기대지 않고, 생성 실패 시 `BuiltinReady`를 게시하지 않으며 시작이 실패한다(UE에는 준비 완료 gate와 생성 실패 경로가 없다). 1차 내용은 11.1절이다.

### 6.4 완료 기준(RB2)

- 계획 11.2절 API04(recording·cancel·late submit·자원 교체·builtin·GPU completion 수명), 9.6절 BR02–BR04, BR01은 11.1절 1차 팩 범위로 판정한다(error material·글꼴은 그 단계에서 범위를 넓힌다).
- `Pending`·`Failed` handle 그리기의 대체 자원 사용과 계수.
- `Retire` 뒤 lease가 남은 동안 회수 0건, fence 완료 이벤트 뒤 회수(이벤트를 gate로 막아 단언).
- 장치 재생성 뒤 구 handle 사용이 stale 오류.

### 6.5 텍스처 플랫폼 데이터 입력 계약

AssetSystem A5가 쿠킹하는 텍스처 플랫폼 데이터를 렌더(GPU 업로드·텍스처 스트리밍)가 읽는 계약이다. 에셋 세션과 합의했다(2026-11-16). 분담은 다음과 같다. 압축(BC·ASTC·ETC2)·색 보정은 에셋 빌드(A 단계, 에셋 너겟 키)가 맡는다. GPU 업로드·스트리밍·mip 상주는 렌더가 맡는다. 무손실 원본 bulk는 에디터 전용이며 쿠킹 데이터에 들어가지 않으므로 렌더는 읽지 않는다. 플랫폼 데이터는 TargetProfile마다 만들고 에셋 너겟 키로 캐시한다(형식 선택이 프로필에 걸린다).

**UE 5.8 대응.**
- 플랫폼 데이터는 크기·cube 여부·slice 수·픽셀 형식·플랫폼별 추가 데이터(mip tail 수 포함)·mip 배열을 가진다(`Engine/Classes/Engine/Texture.h:880–895`).
- mip 하나는 크기(SizeX·SizeY, depth 또는 array 크기를 담는 SizeZ)와 bulk 하나를 가진다(`Engine/Public/TextureResource.h:45–57`).
- cube는 한 mip의 bulk 안에 face를 이어 둔다(`Engine/Private/TextureCube.cpp:392`). face 순서는 +X·−X·+Y·−Y·+Z·−Z다(`RHI/Public/RHIDefinitions.h:506–515`).
- 쿠킹은 mip을 셋으로 나눈다. optional(선택 다운로드)·스트리밍(별도 파일)·inline(에셋과 함께 상주)이다. inline 꼬리는 기본 7개와 형식 mip tail 수 가운데 큰 값이다(`Engine/Private/TextureDerivedData.cpp:1644, 3374–3392`, `Engine/Private/TextureDerivedDataTask.h:32`).

**메타.** 텍스처마다 다음을 가진다.
- 차원(2D·Cube·3D·2DArray·CubeArray).
- 형식·sRGB·swizzle. 모두 GraphicsTypes 값이다.
- 기준 크기: 폭·높이, 그리고 depth(3D) 또는 slice 수(array). cube는 face 6 × cube 수다.
- mip 수와 상주 꼬리 mip 수.
- mip마다: 크기(폭·높이·depth), 바이트 크기(패딩 제외), 덩어리 hash(Core 공용 hash. 스트리밍 전송이 읽은 뒤 대조한다, [텍스처 스트리밍](../TextureStreaming.ko.md) 5.1절), 위치(상주 꼬리 덩어리 안의 오프셋, 또는 스트리밍 덩어리 id·오프셋).
- 렌더는 상주 mip 수를 다시 계산하지 않고 메타 값을 쓴다.

**바이트 배치.**
- mip-major: 큰 mip(0)부터 둔다. 스트리밍이 mip 하나의 모든 slice를 함께 올리기 때문이다.
- 한 mip 안의 순서:
  - array는 slice 0부터.
  - cube는 face +X·−X·+Y·−Y·+Z·−Z 순(UE와 같다).
  - cube array는 cube 0의 face 6개, 다음 cube 순.
  - 3D는 z 0부터.
- 2D 이미지 하나 안은 첫 행이 위이고 행 사이 padding이 없다. 지금 `TextureDesc`의 초기 픽셀 규칙(`Engine/Source/Runtime/Graphics/Public/Graphics/DeviceTypes.h:108–128` 주석)을 확장한 것이다.
- 블록 압축 형식은 블록 행 단위다. 크기는 ceil(폭/블록 폭) × ceil(높이/블록 높이) × 블록 바이트다.
- 장치 정렬(D3D12 행 pitch 256 byte, placed 자원 정렬 등)은 렌더 업로드(GX8)가 맞춘다. 에셋 데이터는 장치 정렬을 담지 않는다.

**스트리밍 단위와 정렬.**
- 스트리밍 mip은 mip마다 따로 읽을 수 있는 덩어리 하나다.
- 상주 꼬리(가장 작은 mip들)는 한 덩어리로 묶어 에셋과 함께 읽는다.
- 상주 꼬리 mip 수는 max(설정한 최소 상주 mip 수, 형식이 요구하는 mip tail 수)이고, 쿠킹 때 정해 메타에 적는다. 최소 상주 mip 수의 기본값은 7이고, 블록 압축 형식은 가장 작은 스트리밍 mip이 블록 4×4 이상이 되도록 늘린다(UE와 같다, [텍스처 스트리밍](../TextureStreaming.ko.md) 4.3절).
- 스트리밍 덩어리의 시작 오프셋은 파일 안에서 4 KiB 정렬이고 패딩은 0으로 채운다. 메타의 크기는 패딩을 뺀 실제 크기다. 나중에 unbuffered 비동기 읽기나 mmap을 쓸 여지를 남기려는 것이고, 지금 직접 I/O를 쓰지는 않는다. 상주 꼬리 덩어리 안의 mip 오프셋은 16 byte 정렬이다.
- 덩어리를 패키지 안에 둘지 별도 파일에 둘지는 AssetSystem A1 패키지 형식이 정한다.

**직렬화.** 형식·sRGB·swizzle·차원은 GraphicsTypes가 가진 고정 번호표와 안정 이름(텍스트·JSON용)으로 기록하고 enum 순서 값을 쓰지 않는다. 번호는 한 번 정하면 재사용하지 않는다. 렌더 쪽 장치 형식 변환도 이 번호표를 기준으로 한다.

**swizzle의 뜻과 적용(2026-11-25 에셋 세션 합의, 개정 48).** 메타의 swizzle은 UE sampler type 계약 데이터다(단일 채널 복제, 노멀 채널 배치 — AssetSystem 12.4.3·12.16.4절). 렌더는 이것을 view swizzle로 적용하지 않는다.
- 텍스처 view swizzle은 늘 항등이다(`TextureDesc::swizzle` 기본값). 셰이더가 sampler type에 따라 복제한다. UE도 같다: 머티리얼 셰이더가 sampler type별 함수로 단일 채널을 복제한다(`Engine/Shaders/Private/MaterialTexture.ush:168–204`, sampler type은 `MaterialExpressionUtils.cpp` `GetSamplerTypeForTexture`).
- 값: Grayscale·LinearGrayscale·Alpha·DistanceFieldFont는 `.rrrr`이다. Color sampler(Displacementmap·HalfFloat·SingleFloat 포함)는 복제하지 않는다(r001 — 형식 규칙대로 읽은 값 그대로).
- 이유: D3D11에는 view swizzle이 없다(`DeviceFeatures::textureViewSwizzle`). view swizzle을 쓰면 backend마다 결과가 갈리거나, 셰이더 복제와 겹쳐 두 번 적용된다.
- 셰이더 공용 함수는 `/Engine/Private/Material/MaterialTextureSampling.rsh`(엔진 셰이더 A05)다. sampler type 함수(`ReplicateRedChannel`)와 쿠킹 swizzle 값을 그대로 받는 함수(`ApplyCookedChannelSwizzle` — 텍스처 표시처럼 sampler type 대신 메타를 읽는 도구용)를 둔다. 머티리얼 생성 코드는 sampler type 함수를 쓴다(MS 단계).
- `TextureDesc::swizzle`(view swizzle) 기능 자체는 장치 계약에 남는다(D3D12·Vulkan·GLES 시험, A0-G). 쿠킹 텍스처 경로가 쓰지 않을 뿐이다.

**렌더 쪽 검사.** 텍스처를 만들 때 다음을 확인한다(구현은 아래 "업로드").
- 메타의 mip 크기·바이트 크기가 위 공식과 맞는지. 다르면 `CorruptEntry`다.
- 차원·형식 조합을 장치가 지원하는지(`DeviceCaps`, GX1). 지원하지 않으면 `UnsupportedCapability`다.
- 상주 꼬리만으로 GPU 자원을 만들 수 있는지(가장 큰 상주 mip 기준으로 만들고, 스트리밍 mip은 상주가 늘 때 채운다). 텍스처 스트리밍 전(화면 마일스톤)에는 상주 꼬리와 스트리밍 mip bulk를 모두 읽어 한 번에 올린다.

**업로드(SM7, 개정 48 — 18.5절 7항).** RenderBase 쪽은 `CookedTexture.h` 하나다. 에셋 타입(Texture 모듈)을 모르는 중립 입력을 받는다. `TexturePlatformData`와 패키지 bulk를 이 입력으로 바꾸는 일은 텍스처 에셋을 아는 층이 한다. 그 층은 WorldIntegration이다(SM8, 개정 54 — 18.5절 8항): 변환 `PlatformTextureSource.h`와, 텍스처 에셋마다 handle을 예약하고 lease·Retire 순서를 지키는 `TextureRenderCache`다. UE도 장치 자원 만들기는 Engine 모듈의 텍스처 자원 클래스가 하고 RenderCore는 텍스처 에셋을 모른다.
- 입력 `CookedTextureSource`: 차원·형식·sRGB·기준 크기(폭·높이·depth·slice 수)·상주 꼬리 mip 수·샘플러·mip 목록(크기·패딩 제외 byte 수, 스트리밍 mip은 그 bulk byte, 상주 mip은 꼬리 덩어리 안 오프셋)·꼬리 덩어리 byte다. byte는 빌린다.
- 검사 `CheckCookedTexture`(장치 능력 값과 그 형식의 `PixelFormatCaps`를 받는다 — 장치 없이 시험한다) → `graphics::TextureDesc`. 오류는 두 갈래이고, 원인은 `CookedTextureIssue` 칸으로 따로 돌려준다.
  - 데이터가 계약과 다름(`CorruptEntry`)은 `ErrorCode::InvalidArgument`다. 원인: 모르는 차원·형식, 깊이·다중 plane 형식, 형식의 sRGB와 sRGB 칸이 다름(PX1 (가) — sRGB는 형식에 들어 있다), 0 크기, 차원별 크기 규칙(cube 정사각·slice 6 배수·volume slice 1), mip 수 0·전체 mip 수 초과, mip 크기 사슬, mip byte 수 ≠ 공식, 상주 꼬리 수 0·mip 수 초과, 스트리밍 mip bulk 크기 ≠ byte 수(잘린 bulk), 꼬리 오프셋이 16 byte 정렬 아님·앞 mip과 겹침·덩어리 밖(잘린 꼬리), BC 최상위 블록 배수 아님, 샘플러 값 범위 밖.
  - 장치가 못 함은 `ErrorCode::UnsupportedCapability`다. 원인: 형식 능력(`Sampled`, mip 둘 이상이면 `Mipmaps`, volume이면 `Texture3D`, cube 계열이면 `TextureCube`) 없음, cube array 기능 없음, 장치 한도(`CheckTextureLimits` — 한 변·slice 수) 초과.
- 바이트: 장치 초기 픽셀 배치(`TextureDesc` 주석)가 이 절의 mip-major 배치와 같다. 그래서 mip을 큰 것부터 잇기만 한다(꼬리의 16 byte 정렬 패딩은 뺀다). 블록 압축 형식 크기는 PixelFormat 블록 정보로 계산한다(`PixelFormatImageBytes` — ASTC처럼 블록 배수가 아닌 크기는 블록 수를 올림한다). 장치 행 pitch·정렬은 장치 업로드(GX8)가 맞춘다.
- 샘플러: 쿠킹 값 그대로다(필터·세 축 주소·border 색). `maxAnisotropy` 0은 "전역 값"이고 `CookedTextureDefaultMaxAnisotropy`(4 — UE `r.MaxAnisotropy` 기본값, `Core/Private/HAL/ConsoleManager.cpp:4360`, 적용은 `RHI/Public/RHIUtilities.h:699` `ComputeAnisotropyRT`)로 바꾸고 1–16으로 자른다. 전역 값 CVar(`r.MaxAnisotropy`)는 렌더러 설정 단계에서 더한다.
- 생성: `PrepareCookedTexture`가 검사·묶기를 게임 lane에서 하고(바이트를 `RenderBlob`으로 복사 — 패키지 bulk는 게임 스레드 전용이다), 렌더 lane 작업 `CreateCookedTextureOnLane`이 장치 텍스처를 만들어 `RenderHandle`에 잇고 `ResourceReady`·`ResourceFailed`를 통지한다(6.2절 장치 자원 소유자). 게시 전에 그린 handle은 typed 대체로 그린다(6.2절 "준비 전 사용").
- 패키지 게시에서 시작한다: 쿠킹 텍스처에는 PostLoad hook이 없고(AssetSystem 12.15절 A5k), 샘플러도 쿠킹 때 정해진다. 그래서 업로드는 AssetManager 준비 callback(패키지 공개) 뒤 게임 lane이 낸다(`TextureRenderCache::Update`). 상주 수명은 그 lease가 정한다(lease를 놓기 전에 handle을 `Retire`한다).
- CPU Availability 텍스처(AssetSystem 12.4.1절)는 쿠킹 mip을 올리지 않는다. 그리는 쪽이 내장 검정 2D(`BuiltinTexture::Black2D`)로 대체한다(UE는 쿠킹 때 작은 검정 placeholder를 GPU에 둔다 — `Engine/Private/Texture.cpp:4477–4481`). Availability는 에셋 칸이라 변환 층이 본다.

**후속.** optional mip(선택 다운로드), 가상 텍스처(AssetSystem 12.4절), 플랫폼 tiling 배치. 블록 압축 형식 표(BC1–7·ASTC·ETC2와 GLES·Android 가용성)는 압축 단계 착수 전에 에셋 세션과 함께 확정한다. 형식 정의·블록 크기·고정 번호표·backend 가용성은 [픽셀 형식](../PixelFormat/05-FormatList.ko.md) 5·6·8·12절이 정하고, 프로필별 형식 선택과 인코더가 에셋 세션 몫으로 남는다. 메타의 "sRGB" 항목은 픽셀 형식 결정 PX1에 따라 형식에 포함되거나 따로 남는다.
