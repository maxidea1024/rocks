## 3. 현재 Rocks 상태

`Engine/Source/Runtime/GraphicsTypes/Public/GraphicsTypes/PixelFormat.h:12–16`의 `enum class Format : uint32`에 `Unknown = 0`·`RGBA8Unorm = 1`·`RGBA8UnormSrgb = 2`만 있다. 초기 시제품의 상태 그대로다. 함수는 `BytesPerPixel`(`:19`, 픽셀당 4바이트 가정)·`FormatName`(`:22`)·`MipExtent`·`FullMipCount`·`MipChainBytes`(`:43`)다. 블록 압축 형식의 크기는 계산할 수 없다.

사용처는 10개 파일이다(`GpuTrace.cpp`의 `using Format = TraceStandardFormat`과 `CompressionBench`의 압축 형식 `FormatName`은 이름만 같은 별개 타입이다).

| 파일 | 줄 | 내용 |
|---|---|---|
| `Engine/Source/Runtime/GraphicsTypes/Public/GraphicsTypes/PixelFormat.h` | 12–16, 19, 22, 43 | 정의 |
| `Engine/Source/Runtime/GraphicsTypes/Private/PixelFormat.cpp` | 5–36 | `BytesPerPixel`·`FormatName`·`MipChainBytes` |
| `Engine/Source/Runtime/Graphics/Public/Graphics/DeviceTypes.h` | 7, 131, 218 | `TextureDesc::format`, `PipelineDesc::colorFormat` |
| `Engine/Source/Runtime/D3D12Graphics/Private/Win64/D3D12Device.cpp` | 116–123, 304, 706, 892, 1220, 1269, 1316, 2219, 2297 | DXGI 대응 `ResourceFormat`, 기록의 형식 칸, 흰 텍스처, 업로드·readback 크기 |
| `Engine/Source/Runtime/OpenGLGraphics/Private/Win64/OpenGLDevice.cpp` | 418–425, 457, 863, 984, 986, 1263, 1420, 1460, 1514 | GL 대응 `InternalFormat`, 기록의 형식 칸, 업로드·readback 크기 |
| `Programs/Sandbox/Private/GraphicsSelfTest.cpp` | 371, 382, 516–517, 596 | 시험 |
| `Programs/Sandbox/Private/RenderLaneSelfTest.cpp` | 91 | 시험 |
| `Programs/Sandbox/Private/SampleRenderer.h` | 52, 73 | 샘플 렌더러 |
| `Programs/Sandbox/Private/SampleRenderer.cpp` | 114 | 샘플 렌더러 |
| `Programs/Sandbox/Private/VertexSelfTest.cpp` | 354, 707 | 시험 |

정점 원소 형식은 별도 `VertexFormat`(`GraphicsTypes/VertexFormat.h`, RenderBase 10.4절 GX3a, 구현)이다. 그 enum은 고정 번호표·`VertexFormatName`·`ParseVertexFormatName`·`VertexFormatFromNumber`를 이미 갖추었으므로 픽셀 형식도 같은 API 모양을 따른다. Core `ErrorCode`에는 아직 `UnsupportedCapability`가 없고(`Core/Source/Runtime/Core/Public/Core/Error.h:10–22`, AssetSystem A1에서 추가) 지금은 `Unsupported`를 쓴다(RenderBase 개정 5 결정).
