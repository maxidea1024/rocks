## 13. 이름 변경 계획

| 지금 | 바꾼 뒤 | 비고 |
|---|---|---|
| `enum class Format : uint32` | `enum class PixelFormat : uint8` | 번호는 5.2절 |
| `Format::Unknown`·`RGBA8Unorm`·`RGBA8UnormSrgb` | `PixelFormat::Unknown`·`RGBA8Unorm`·`RGBA8UnormSrgb` | 값 0·12·13 |
| `FormatName(Format)` | `PixelFormatName(PixelFormat)` | 모르는 값은 `"Unknown"` |
| — | `ParsePixelFormatName`·`PixelFormatNumber`·`PixelFormatFromNumber`·`IsValidPixelFormat` | `VertexFormat`과 같은 모양 |
| `BytesPerPixel(Format)` | 삭제. `GetPixelFormatInfo`·`PixelFormatImageBytes`·`PixelFormatRowBytes` | 블록 형식 |
| `MipChainBytes(w, h, mips, Format)` | `MipChainBytes(PixelFormat, w, h, depth, mips)` | 블록·3D |
| — | `PixelFormatCaps`(GraphicsTypes), `Device::GetPixelFormatCaps`(Graphics) | 7·8절 |
| `TextureDesc::format`(`Format`) | `PixelFormat` | `DeviceTypes.h:131` |
| `PipelineDesc::colorFormat`(`Format`) | `PixelFormat` | `DeviceTypes.h:218` |
| D3D12 `ResourceFormat(Format)` | `D3D12PixelFormats.cpp` 표 | `D3D12Device.cpp:116–123` |
| OpenGL `InternalFormat(Format)` | `OpenGLPixelFormats.cpp` 표 | `OpenGLDevice.cpp:418–425` |

사용처는 3절 표의 10개 파일 전부다. 문서 쪽 갱신 대상: `Docs/Design/EngineRuntime:35`(GraphicsTypes 설명), `Docs/Design/RenderBase` 4.1절 "자원 종류" 행·11.2절 "크기 계산", `Docs/Design/ShaderSystem/04-CurrentCodebase.ko.md:16`(GraphicsTypes 현황 행). 헤더 이름 `GraphicsTypes/PixelFormat.h`는 그대로 둔다.
