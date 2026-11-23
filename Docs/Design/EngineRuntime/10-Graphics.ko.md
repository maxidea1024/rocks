# 엔진 런타임 — 10. Graphics

[목차](README.ko.md)

## 10. Graphics

### 10.1 계약

```cpp
namespace rocks::graphics {
enum class Backend : uint8 { D3D12, OpenGL, Vulkan, Metal };

Result<UniquePtr<Device>> CreateDevice(const DeviceDesc&);   // backend 모듈이 제공
class Device {
    Result<SwapChainHandle> CreateSwapChain(const SwapChainDesc&);   // 네이티브 창 핸들
    Result<BufferHandle> CreateBuffer(const BufferDesc&, Span<const uint8> initial);   // 정점·index
    Result<TextureHandle> CreateTexture(const TextureDesc&, Span<const uint8> initial);
    Result<PipelineHandle> CreatePipeline(const PipelineDesc&);      // 정점 배치·셰이더 입력 대조
    Result<CommandContext*> BeginFrame(SwapChainHandle, const ClearColor&);
    Result<GpuFencePoint> EndFrame();          // 제출·present, 완료 지점
    void Retire(Handle);                       // 마지막 사용 fence 뒤 회수
    void ReclaimCompleted();
    // 스레드 안전: LastSubmittedFence·IsFenceComplete·WaitFence·NotifyFence·IsLost
    // 소유권: IsOwnerThread·ReleaseOwnership·AcquireOwnership
};
}
```

- 자원은 slot+generation 핸들이다. `Retire`는 마지막으로 사용한 제출의 fence(`GpuFencePoint`: device epoch·queue·value)가 끝난 뒤 자원과 descriptor를 해제한다. OpenGL도 `glFenceSync`로 같은 규칙을 따른다(즉시 삭제하지 않는다). fence 완료는 다른 스레드에서 기다리거나 통지·작업 이벤트(`Graphics/GpuFenceEvent.h`)로 받는다. 장치 손실은 기다리는 쪽을 `Destroyed`로 깨운다([렌더 기반](../RenderBase.ko.md) GX7).
- 장치 스레드 모델([렌더 기반](../RenderBase.ko.md) 5.4절 GX11·GX10): 소유 스레드 하나가 제출·present·상태 변경·생성(지금은 생성도 — `DeviceInfo::concurrentResourceCreation` false)을 하고, 소유권은 `ReleaseOwnership`·`AcquireOwnership`으로 넘긴다. 소유 스레드 밖 호출은 Debug·Development에서 `WrongThread`다. 엔진 실행 때는 렌더 lane(RenderBase)이 장치를 소유한다.
- 정점 배치([렌더 기반](../RenderBase.ko.md) 10.4절 GX3a): 정점 원소 형식 20종(`GraphicsTypes/VertexFormat.h`), 속성(location·형식·stream·offset)·stream(stride·정점/인스턴스), index 버퍼(UInt16·UInt32), `Draw`·`DrawIndexed`(인스턴스 수), 정점 버퍼 없는 그리기. 셰이더 정점 입력 시맨틱은 `ATTRIBUTEn`이다.
- 셰이더는 backend별 원문을 `PipelineDesc`에 담는다(D3D12: HLSL을 런타임 `D3DCompile` SM 5.1, D3D11: 같은 HLSL을 SM 5.0, OpenGL ES: GLSL ES 3.20, 쿠킹된 `GLES31_PC` 출력과 같은 모양, Vulkan: 같은 HLSL을 개발용 컴파일 훅 `DeviceDesc::shaderCompiler`가 DXC로 SPIR-V로, 10.3절). 오프라인 셰이더 쿠킹은 후속이다.
- 그리기 상수는 64바이트 행렬 + 16바이트 색으로 제한한다(D3D12 root constants, OpenGL ES 루트 상수 에뮬레이션 배열 `vu_h`·`pu_h`, Vulkan push constant).
- 좌표 규약: 2026-11-17 사용자 결정으로 좌표계·단위·행렬 규약을 UE 5.8과 같게 바꾼다(행 벡터 `p·M`·행 우선 저장, 셰이더 `mul(v, M)`·행 우선 packing, 앞면은 화면 반시계, [셰이더 시스템](../ShaderSystem/07-BindingConventions.ko.md) 7.2절 6·8항). 메모리 배치는 전과 같아 GPU로 가는 바이트는 바뀌지 않는다. 렌더 코드는 C7에서 반영했다(2026-11-17): `DrawConstants::transform`은 행 우선, 샘플 렌더러 셰이더는 `mul(float4(position, 1), transform)`, D3D12는 `D3DCOMPILE_PACK_MATRIX_ROW_MAJOR`로 컴파일하고 OpenGL ES는 행 우선 byte를 vec4 배열로 그대로 올려 GLSL ES가 `mat4(vu_h[0..3]) * v`로 쓴다(P11). `PipelineDesc::cullMode`(None·Back·Front, 기본 None)가 컬을 정하고 앞면은 화면 반시계다(D3D12 `FrontCounterClockwise = TRUE`, OpenGL ES는 클립 공간 규약에 맞춘 `glFrontFace`를 장치가 정한다). 샘플 렌더러는 뒤면을 컬하고, 물체 world·view 행렬식 가운데 하나만 음수면 앞면 컬 파이프라인으로 바꾼다. OpenGL ES Windows 미리보기는 모든 패스를 `glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)`으로 그리고 스왑체인도 offscreen 텍스처에 그린 뒤 present가 Y를 뒤집어 창에 blit한다(P11, 2026-11-17). 그래서 RenderTarget·스왑체인 모두 첫 행이 이미지 위쪽이다(샘플링·readback 모두 D3D12와 같은 배치). Android는 셰이더가 클립 공간을 보정하고 앞면이 `GL_CW`다([OpenGL ES backend](../OpenGLESGraphics.ko.md) 5.3절). 텍스처는 첫 행이 위인 배치로 올리고 UV (0,0)이 좌상단이다.
- 동시에 진행하는 프레임 수는 렌더 lane 프레임 slot(2–3개)이 정하고, slot 재사용은 그 프레임의 모든 제출 fence 완료로 판정한다. D3D12는 끝난 제출의 기록 저장소를 다시 쓴다. 디버그 구성은 디버그 레이어를 켠다.

### 10.2 D3D11Graphics(P15 D3D11 부분)

D3D11은 필수 backend다(2026-11-13 사용자 결정). D3D12의 SM 6.6 기준을 받지 못하는 구형 GPU·드라이버를 위한 Windows 대체 경로이고 셰이더 형식은 `D3D11_SM5`(FXC → DXBC SM 5.0)다. 2026-11-17 사용자 결정으로 SH5보다 앞당겨 구현했다. SH2 전이므로 셰이더는 D3D12와 같이 `PipelineDesc`의 HLSL 원문을 실행 중 `D3DCompile`(`vs_5_0`·`ps_5_0`)로 받고, 쿠킹된 셰이더는 SH2·SH5에서 잇는다. 근거는 UE 5.8 `Runtime/Windows/D3D11RHI`다(아래 경로는 그 아래 기준).

**장치.**
- feature level은 11_1·11_0을 요청하고 11_0을 최소로 둔다(UE `Private/Windows/WindowsD3D11Device.cpp:247–259`).
- 어댑터: `IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE)`로 소프트웨어 어댑터를 빼고 차례로 만든다(UE `:701–710`). 하드웨어 후보가 모두 실패하거나 `preferSoftware`이면 WARP(`EnumWarpAdapter`)다. D3D12와 같은 규칙이다.
- 필수 인터페이스는 `ID3D11Device5`·`ID3D11DeviceContext4`와 `ID3D11Fence` 생성이다(아래 "GPU 완료"). 없으면 다음 후보로 넘어가고 WARP까지 없으면 `Unsupported`다(Windows 10 1703 이상, WDDM 2.0 드라이버).
- `D3D11_CREATE_DEVICE_SINGLETHREADED`를 쓰지 않는다. 장치(생성 함수)는 runtime이 직렬화하는 free-threaded 객체가 된다. UE도 비동기 자원 생성을 끄지 않는 한 이 플래그를 빼고(`:1130`), 드라이버 동시 생성 지원은 `D3D11_FEATURE_THREADING`으로 따로 읽는다(`:1466–1469`).
- 검증: `enableValidation`이면 `D3D11_CREATE_DEVICE_DEBUG`와 DXGI debug factory를 켠다. SDK layer가 없으면(`DXGI_ERROR_SDK_COMPONENT_MISSING`) 끄고 다시 만든다. `ID3D11InfoQueue`와 DXGI info queue에 ERROR·CORRUPTION만 저장하는 filter를 걸고 `ValidationErrorCount`가 센다(D3D12와 같다). UE는 INFO·WARNING을 deny list로 거르고 특정 ID를 뺀다(`:1407–1449`).
- 생성 실패는 `DeviceCreateReport`(LoggingSystem 13절 모양: `code`·`nativeCode`(HRESULT)·`stage`·`detail`)로 돌려준다. 장치 안 실패는 `LastError`에 단계와 HRESULT를 함께 적는다.

**스왑체인.** flip model(`DXGI_SWAP_EFFECT_FLIP_DISCARD`, 버퍼 2개, `R8G8B8A8_UNORM` 버퍼에 sRGB RTV)이다. D3D11 runtime이 flip 버퍼를 돌리므로 버퍼 0과 RTV 하나만 쥔다. UE는 tearing을 지원할 때만 flip model이고 아니면 `DXGI_SWAP_EFFECT_DISCARD`다(`Private/Windows/WindowsD3D11Viewport.cpp:75–94`). Rocks는 Windows 10 이상만 대상으로 하므로 늘 flip model이다. `ResizeBuffers` 전에 버퍼·RTV 참조를 놓고 immediate context의 상태를 비운다.

**context 모델.**
- 소유 스레드 하나가 immediate context로 제출한다(업로드 실행, 프레임 실행, present, fence signal, query 회수).
- 프레임 기록은 deferred context 하나에 한다. `EndFrame`이 `FinishCommandList`로 닫고, 쌓인 업로드를 immediate context에서 실행한 뒤 `ExecuteCommandList`로 프레임을 실행한다.
- 이렇게 해야 GX8 순서 계약(쌓인 업로드는 그 제출보다 먼저 실행되고, 기록 중에 갱신하면 그 프레임 전체가 새 내용을 본다)이 D3D12와 같다. 기록 객체를 deferred context로 두는 모양은 병렬 기록(GX4·RG3)에서 기록 스레드마다 하나로 늘리면 된다.
- UE는 deferred context를 쓰지 않고 immediate context 하나를 RHI 스레드가 쓴다. 드라이버가 command list를 지원하지 않으면(`DriverCommandLists` 거짓) runtime이 기록·재생을 흉내 낸다. 이때 비용만 늘고 동작은 같다.

**GPU 완료(GX7).**
- `ID3D11Fence`를 쓴다. `ID3D11DeviceContext4::Signal` 바로 뒤 `Flush`해 신호가 GPU에 닿게 한다. fence 관찰 스레드가 `SetEventOnCompletion`으로 기다리는 `FenceTimeline` 구조와 장치 손실 판정(완료 값 `UINT64_MAX`, `GetDeviceRemovedReason`)은 D3D12와 같다.
- UE는 event query(`D3D11_QUERY_EVENT`)를 RHI 스레드가 polling한다(`Private/D3D11Commands.cpp:1648–1690`). 그러나 `GetData`는 immediate context 전용이어서 GX7의 "어느 스레드에서나 기다린다"를 지킬 수 없다. 그래서 fence가 없는 장치는 지원하지 않는다(위 필수 인터페이스).
- 회수 규칙(`Retire` → 마지막 사용 fence 뒤 해제, 기록 중이면 그 프레임의 fence)은 D3D12와 같다. D3D11 runtime이 쓰는 중인 객체를 스스로 늦게 지우더라도, 계약(`PendingRetireCount`, 해제 시점)이 backend마다 같도록 장부를 그대로 둔다.

**업로드·readback(GX8).**
- D3D11에는 계속 map해 둔 채 GPU가 복사할 수 있는 업로드 heap이 없다. map은 immediate context 전용이고, map 중인 자원은 복사 원본이 될 수 없다.
- 그래서 업로드 ring(`UploadRingAllocator`, 기본 4 MiB)과 스트리밍 staging pool은 시스템 메모리다. 갱신(`UpdateBuffer`·`UpdateTexture`·`Upload*FromStaging`)은 "ring·staging에 쓰기 → 복사 기록"으로 쌓인다. 소유 스레드의 다음 제출이 immediate context에서 `UpdateSubresource`로 실행한다.
- `UpdateSubresource`는 원본을 호출 안에서 드라이버 내부 staging으로 복사한다. 그래서 GPU 복사가 한 번 늘지만 ring 구간은 그 묶음의 fence 뒤에 회수한다(D3D12와 같은 보수적 규칙, 결과가 완료 시점에 좌우되지 않는다).
- UE도 정적 버퍼 쓰기 lock은 CPU 메모리를 잡았다가 unlock 때 올리고(`Private/D3D11Buffer.cpp:278`, `:320`), 텍스처 갱신은 `UpdateSubresource`다(`Private/D3D11Texture.cpp:1188,1220`).
- 생성 초기 데이터는 `D3D11_SUBRESOURCE_DATA`로 생성 호출 안에서 올린다(장치 호출이라 스레드 안전, 생성 즉시 내용이 정해진다).
- staging에서 올리는 경로는 행 정렬 요구가 없어 언제나 그대로 올린다. D3D12처럼 ring으로 다시 옮기는 경로가 없다.
- readback: `D3D11_USAGE_STAGING` 텍스처로 `CopyResource`하고 fence가 끝난 뒤에만 `Map(READ)`으로 읽는다(UE `Private/D3D11RenderTarget.cpp:520–563`과 같은 staging 경로). `D3D11_MAP_FLAG_DO_NOT_WAIT`은 쓰지 않는다. 하드웨어 드라이버(GTX 1060)에서 runtime의 자원 사용 추적이 fence보다 늦게 풀려, 끝난 복사에도 `DXGI_ERROR_WAS_STILL_DRAWING`을 돌려주는 것을 확인했다. fence 뒤의 map은 GPU를 기다리지 않는다.
- 그리기별 상수(D3D12 루트 상수 대응, RenderBase 결정 R7·SX2)는 128 byte 동적 상수 버퍼(b0, VS·PS·CS) 하나다. 파이프라인이 선언한 크기(`drawConstantBytes`)만 셰이더가 읽고, `SetDrawConstants`마다 CPU 사본 전체를 deferred context에서 `Map(WRITE_DISCARD)`한다(UE uniform buffer와 같다, `Private/D3D11UniformBuffer.cpp:161`). `NO_OVERWRITE`는 deferred context의 상수 버퍼에 11.1 기능(`MapNoOverwriteOnDynamicConstantBuffer`)이 필요해 쓰지 않는다. SH2·P2의 b13 그리기 상수(셰이더 시스템 7.2절 4항)로 옮기는 것은 SH5다.

**장치 능력·버퍼(GX1·GX2, 2026-11-18).**
- `DeviceCaps`는 d3d11.h 상수와 feature level(FL 11.0은 UAV 8·VS UAV 없음, 11.1 이상은 UAV 64·모든 단계 UAV), `D3D11_FEATURE_D3D11_OPTIONS3`(VS 층 출력), DXGI 출력 HDR 색 공간이다. 64 bit atomic·multi draw indirect는 거짓이다. MSAA 표본 수는 `CheckMultisampleQualityLevels`다.
- 버퍼 용도는 bind·misc flag로 옮긴다(UE `Private/D3D11Buffer.cpp:38–83`). raw SRV는 `D3D11_SRV_DIMENSION_BUFFEREX` + RAW다. 버퍼 view slot n은 VS·PS `t(n + 1)`(`VSSetShaderResources`·`PSSetShaderResources`).
- 상수 버퍼는 `UpdateSubresource`에 대상 구간을 줄 수 없어(debug layer 288) 전체 갱신만 받고 부분 갱신은 `Unsupported`다. WARP가 `RGB9E5Ufloat` 버퍼 SRV 생성에서 멈추므로 WARP에서만 그 형식의 texel buffer 능력을 공개하지 않는다.
- `ReadBuffer`는 staging 버퍼로 구간 `CopySubresourceRegion`(원본 box) 뒤 fence를 기다려 `Map(READ)`이다.

**파이프라인·명령 기록 확장(GX3·GX4, 2026-11-18).**
- 파이프라인마다 blend(타깃별, `IndependentBlendEnable`, alpha-to-coverage)·래스터(컬·깊이 bias·clamp, scissor는 늘 켬)·깊이·스텐실 상태 객체와 기본도형을 만든다. compute는 `cs_5_0`이고 CS 상수는 그리기 상수 버퍼 slot 0을 같이 쓴다.
- `BeginPass`는 RTV 0–8과 DSV(읽기 전용 깊이는 깊이 텍스처마다 만든 `D3D11_DSV_READ_ONLY_*` view)를 묶고 load 동작을 `ClearRenderTargetView`·`ClearDepthStencilView`로 한다. 스텐실 기준은 깊이·스텐실 상태와 함께 건다.
- dispatch는 CS에 읽기 SRV(t1–t4)·UAV(u0–u3)를 묶고 끝나면 풀며, 그래픽 SRV는 그리기 직전에 묶는다(같은 버퍼의 SRV·UAV가 겹쳐 런타임이 binding을 풀지 않게). dispatch 뒤 첫 그리기는 정점·index 버퍼를 다시 묶는다. 간접 명령은 `DrawInstancedIndirect`·`DrawIndexedInstancedIndirect`·`DispatchIndirect`, 복사는 `CopySubresourceRegion`, UAV clear는 `ClearUnorderedAccessViewUint`다.

**GPU 계측(GX9).**
- timestamp: query 객체(slot 4 × 256개)를 deferred context에서 `End`로 기록한다. 프레임 실행(`ExecuteCommandList`) 앞뒤를 slot별 `D3D11_QUERY_TIMESTAMP_DISJOINT`로 감싼다.
- 회수는 fence 완료 뒤 소유 스레드가 `GetData(DONOTFLUSH)`로 한다. 아직 값이 없으면 미완료로 남긴다.
- 주파수는 장치를 만들 때 disjoint query 한 번으로 얻어 `DeviceInfo::timestampFrequency`로 공개한다. disjoint 표시가 섰거나 주파수가 공개값과 다른 slot의 결과는 모두 disjoint다. 그래서 `timestampDisjointDetection`은 참이다.
- calibration: D3D11에는 `GetClockCalibration`이 없다. `CalibrateClocks`는 제출한 GPU 작업이 끝나기를 기다린 뒤 CPU 시각 t0을 읽는다. 이어서 timestamp(disjoint로 감쌈)와 event query를 넣고 `Flush`한 뒤, event가 끝날 때까지 `GetData`를 돌며 끝을 본 CPU 시각 t1을 읽는다.
  - timestamp는 t0과 t1 사이에 찍혔으므로 `cpuTicks`는 가운데, `uncertaintyNanoseconds`는 (t1 − t0)/2다. disjoint면 몇 번 다시 하고, 그래도 안 되면 실패다.
  - GPU가 쉬기를 기다리므로 비싸다. GpuTrace는 `calibrationIntervalFrames`(기본 60)마다만 부른다.
  - UE는 장치를 만들 때 한 번 같은 방식으로 재고 끝을 본 polling 시각을 CPU 시각으로 둔다(`Private/D3D11Query.cpp:389–498`).
- marker: deferred context에서 얻은 `ID3DUserDefinedAnnotation`의 `BeginEvent`·`EndEvent`(UTF-16)다. 얻지 못하면 `debugMarkers`가 거짓이다.

**스레드 모델(GX10·GX11).** `concurrentResourceCreation`은 참이다. 생성은 free-threaded 장치 호출이고, 갱신·staging 업로드는 내부 잠금 아래 CPU 쪽 업로드 기록에 쌓인다. immediate·deferred context는 소유 스레드만 만진다. context는 스레드에 묶이지 않으므로 소유권 이관은 검사만 바꾼다.

**파이프라인·정점 배치(GX3a).**
- 입력 layout은 파이프라인마다 VS 바이트코드로 만든다. 시맨틱은 `ATTRIBUTEn`, `InputSlot`은 stream이고, 인스턴스 stream은 `D3D11_INPUT_PER_INSTANCE_DATA`·간격 1이다(UE `Private/D3D11VertexDeclaration.cpp:57–62`). 속성이 없으면 입력 layout을 묶지 않는다.
- 셰이더 입력 대조는 `ID3D11ShaderReflection`으로 D3D12와 같은 규칙을 적용한다. `Bgra8Unorm`은 `CheckFormatSupport`의 IA 정점 버퍼 지원이 없으면 `Unsupported`다.
- 래스터: `FrontCounterClockwise = TRUE`(UE `Private/D3D11State.cpp:211`, D3D12와 같다), 깊이 clip 켬, scissor 끔. 컬 모드(`PipelineDesc::cullMode`)마다 래스터 상태를 하나씩 만들어 파이프라인이 고른다. HLSL은 `D3DCOMPILE_PACK_MATRIX_ROW_MAJOR`로 컴파일한다(D3D12와 같은 행 우선 규약).
- `CopyPassTarget`은 deferred context의 `CopyResource`다(백버퍼 `R8G8B8A8_UNORM`과 RenderTarget sRGB는 같은 typeless 묶음).
- 블렌드·래스터·깊이 상태 객체는 장치가 한 번 만들어 파이프라인끼리 공유한다.
- 자원 전이(GX5)는 없다. D3D11 runtime이 hazard를 추적한다.

**공개 제약 요약.** fence(`ID3D11Fence`) 없는 장치는 `Unsupported`, 업로드는 드라이버 복사가 한 번 더 있다, calibration은 GPU 대기를 포함한다, timestamp 주파수가 바뀌면 그 구간을 disjoint로 버린다(추정값을 만들지 않는다).

**self-test.** Sandbox `--self-test`가 D3D11 하드웨어와 WARP에서 D3D12와 같은 검사를 한다. 검사 항목은 픽셀, 제출·완료·회수, 소유권, 스왑체인, 정점 형식 20종, 업로드·readback, 계측이다. 그 결과를 D3D12와 픽셀 대조한다. 검증 계층 집계는 index 용도가 아닌 버퍼를 index 버퍼로 묶는 의도적 오류로 확인한다. 이 오류는 WARP 장치를 제거하므로 검사용 장치와 따로 만든 단기 장치에서 한다.

**UE 대응표.**

| Rocks | UE 5.8 D3D11RHI |
|---|---|
| `D3D11Graphics` 모듈, `CreateD3D11Device` | `D3D11RHI`, `FD3D11DynamicRHIModule::CreateRHI` |
| 프레임 기록 deferred context | (없음, immediate context + RHI 스레드) |
| fence 관찰 스레드(`ID3D11Fence`) | `FD3D11Sync`(event query polling) |
| 업로드 ring(시스템 메모리) + `UpdateSubresource` | `FD3D11LockedData` + `UpdateSubresource` |
| 그리기 상수 동적 버퍼 | `FWinD3D11ConstantBuffer`·uniform buffer `Map(WRITE_DISCARD)` |
| `CalibrateClocks` | `FD3D11DynamicRHI::CalibrateTimers` |
| `PushMarker`·`PopMarker` | `Direct3DDeviceIMAnnotation->BeginEvent`·`EndEvent` |

### 10.3 VulkanGraphics(P9)

Vulkan은 모바일 주력이자 필수 backend다. ShaderSystem P9의 산출물로 SH2 전에 구현했다(2026-11-17 사용자 지시 "Vulkan까지는 작업을 미리하자"). 설계와 UE 5.8 VulkanRHI 조사, 계약별 대응은 [VulkanGraphics](../VulkanGraphics.ko.md)에 있고 여기에는 요지만 둔다.
- 기준은 Vulkan 1.1(Android)이다. 로더는 시스템 `vulkan-1.dll`·`libvulkan.so`를 P10 `OpenSystem`으로 연다. 선택 확장은 swapchain·calibrated timestamps·debug utils뿐이고 없으면 `Unsupported`·false로 공개한다.
- 완료 추적은 제출마다 `VkFence`이고, render pass는 `VkRenderPass`, 메모리는 자체 페이지 할당기(Graphics `GpuPageAllocator`)다.
- 창 출력은 중간 RGBA8 이미지에 그리고 `EndFrame`에서 swapchain 이미지에 blit한다(UE 기본 경로와 같다).
- 앞면은 음수 viewport 높이 + `VK_FRONT_FACE_COUNTER_CLOCKWISE`·`Back → BACK_BIT`다. UE 매핑(`CLOCKWISE`·`CM_CW → FRONT_BIT`)은 `SV_IsFrontFace`가 D3D12와 반대라 따르지 않는다.
- 셰이더는 실행 중 컴파일러가 없으므로 개발 프로그램이 Developer `ShaderFormatVulkan`(DXC `VULKAN_SM5` 옵션과 SPIR-V 재작성)을 `DeviceDesc::shaderCompiler`로 넘긴다. Release 실행 파일에는 컴파일러가 없다(SH2 전에는 Vulkan 파이프라인이 `Unsupported`).
- 확인: Sandbox self-test 전체가 GTX 1060과 lavapipe(소프트웨어 ICD, 측정 S4)에서 통과하고 D3D12와 픽셀 대조가 차이 0이다(VulkanGraphics 17절).

### 10.4 범위 밖

RenderGraph·Deferred/Forward 경로·깊이 버퍼·MSAA·멀티 큐·bindless·Metal backend 구현은 이번 범위가 아니다.
