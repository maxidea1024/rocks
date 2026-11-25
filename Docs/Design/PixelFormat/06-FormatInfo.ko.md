## 6. 형식 정보(`PixelFormatInfo`)

GraphicsTypes에 장치 없는 constexpr 표 하나를 둔다. 번호표의 모든 유효 번호가 표에 있어야 하며 정적 검사로 보장한다.

| 필드 | 타입 | 뜻 | UE 대응 |
|---|---|---|---|
| `format` | `PixelFormat` | 자기 번호 | `UnrealFormat` |
| `name` | `const char*` | 안정 이름 = enum 이름 | `Name`(UE 이름 문자열은 오기가 있어 쓰지 않음, 4.3절) |
| `kind` | `PixelFormatKind` | `Color`·`Depth`·`DepthStencil`·`Compressed`·`Planar` | `IsDepthOrStencilFormat` 등 판정 함수 |
| `compression` | `PixelCompression` | `None`·`BC`·`ETC2`·`ASTC` | `IsBlockCompressedFormat`·`IsETC…`·`IsASTC…`(`PF.h:280–343`) |
| `blockWidth`·`blockHeight` | `uint8` | 블록 크기(비압축 1). 깊이 방향 블록은 두지 않는다 | `BlockSizeX/Y`(`BlockSizeZ`는 늘 1, `PF.cpp:158`) |
| `blockBytes` | `uint8` | 블록 하나의 바이트. 깊이·스텐실과 다중 plane은 0이고 plane 표를 쓴다 | `BlockBytes`(D/S는 backend가 덮어씀, D12S:873·883) |
| `channels` | `PixelChannelMask` | R·G·B·A·D·S 비트 | `GetPixelFormatValidChannels`(`RU:272`), `NumComponents` |
| `componentType` | `PixelComponentType` | `Unorm`·`Snorm`·`Uint`·`Sint`·`Float`·`Ufloat`(BC6H·RG11B10·RGB9E5) | `IsInteger`·`IsFloatFormat`(`PF.h:353–400`) |
| `shaderType` | `PixelShaderType` | 셰이더 읽기 계열 `Float`·`Int`·`Uint`(`R64Uint`는 `Uint64`) | 없음(Rocks 추가, 바인딩 검사용) |
| `srgb` | `bool` | sRGB 부호화 형식인지 | `TexCreate_SRGB`(형식 밖) |
| `srgbPair` | `PixelFormat` | sRGB ↔ 선형 짝, 없으면 `Unknown` | `FindShaderResourceFormat`·`GVulkanSRGBFormat` |
| `depthBits`·`stencilBits` | `uint8` | 깊이·스텐실 비트 | `bIs24BitUnormDepthStencil` |
| `planeCount` | `uint8` | 1(일반), 2(깊이+스텐실, 비디오) | `GetSubresourceCount`의 plane 수(`RHIRes.h:1939`) |
| `planes[2]` | `{PixelFormat format; uint8 widthShift, heightShift; uint8 bytes;}` | plane별 view 형식·크기 축소·texel 바이트 | `GetPlaneSliceFromViewFormat`(`DXGI:217–256`) |
| `topLevelBlockAligned` | `bool` | 최상위 mip 크기가 블록 배수여야 하는지(BC만 true) | `RequiresBlock4Alignment`(`PF.h:451–455`) |

크기 함수(현재 `BytesPerPixel`을 대체한다):

```cpp
// 형식 정보다. 번호표 밖이면 Unknown 행을 준다(IsValidPixelFormat으로 먼저 확인한다).
const PixelFormatInfo& GetPixelFormatInfo(PixelFormat format) noexcept;

// 폭 width의 블록 열 수다: ceil(width / blockWidth). 크기가 0이면 0이다.
uint32 PixelFormatBlockColumns(PixelFormat format, uint32 width) noexcept;
uint32 PixelFormatBlockRows(PixelFormat format, uint32 height) noexcept;

// 2D 이미지 하나의 바이트다(행 사이 padding 없음, RenderBase 6.5절 배치).
// 블록 열 수 × 블록 행 수 × blockBytes. 다중 plane 형식은 plane 합이다. 실패 시 0이다.
uint64 PixelFormatImageBytes(PixelFormat format, uint32 width, uint32 height) noexcept;

// 행 하나의 바이트다(블록 행 하나).
uint64 PixelFormatRowBytes(PixelFormat format, uint32 width) noexcept;

// 앞 mipLevels개 mip을 이은 바이트다. depth > 1이면 각 mip의 depth도 max(1, d >> level)이다.
uint64 MipChainBytes(PixelFormat format, uint32 width, uint32 height, uint32 depth,
                     uint32 mipLevels) noexcept;
```

`MipExtent`·`FullMipCount`는 그대로 둔다. 깊이·스텐실과 비디오 형식의 이미지 크기는 plane 크기의 합이고(개정 3, 14.1절) readback은 plane별이다(10절).
