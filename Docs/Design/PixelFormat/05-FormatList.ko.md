## 5. Rocks 형식 목록

### 5.1 이름 규칙

- 채널 이름은 대문자, 비트 수는 채널 순서대로 묶는다(`RGBA8`, `RG11B10`, `RGB9E5`). 채널 순서는 메모리 순서다. 바이트 형식은 첫 바이트가 첫 채널이고, 묶음(packed) 형식은 첫 채널이 상위 비트다(Vulkan `PACK16`·`PACK32` 표기와 같다).
- 성분 타입 접미사는 `Unorm`·`Snorm`·`Uint`·`Sint`·`Float`·`Ufloat`이다. sRGB 형식은 `UnormSrgb`다.
- 압축 형식은 계열 이름 + 변형 + 성분 타입이다(`BC1Unorm`, `ETC2RGB8Unorm`, `EACR11Unorm`, `ASTC6x6UnormSrgb`, `ASTC6x6Hdr`).
- 깊이는 `D` + 비트 + 타입, 스텐실은 `S8Uint`를 부기한다(`D32FloatS8Uint`).
- 비디오 형식은 업계 이름 그대로다(`NV12`·`P010`).
- 정점 형식 `VertexFormat`의 이름(`Bgra8Unorm`·`Rgb10A2Unorm`)과 대소문자 규칙이 다르다. 별개 enum이므로 맞추지 않는다.

### 5.2 번호표와 정보

번호는 계열별 구간으로 구분한다(결정 PX2): 1–63 비압축 색, 64–79 깊이·스텐실, 80–95 BC, 96–111 ETC2·EAC, 112–159 ASTC, 160–175 비디오. 빈 번호는 쓰지 않는 값이고 `IsValidPixelFormat`이 거부한다. underlying 타입은 `uint8`이다.

표의 "타입"은 셰이더가 읽는 값의 계열이다(float·int·uint). "sRGB 짝"은 결정 PX1이 (가)일 때의 선형·sRGB 짝이다. (나)를 고르면 `*UnormSrgb` 16개 행이 없어지고 "sRGB 짝" 열은 "sRGB 플래그 허용"으로 바뀐다.

| 번호 | Rocks | 블록 | 바이트 | 채널 | 타입 | sRGB 짝 | UE 대응 |
|---|---|---|---|---|---|---|---|
| 0 | `Unknown` | — | 0 | — | — | — | `PF_Unknown` |
| 1 | `R8Unorm` | 1×1 | 1 | R | float | 2 | `PF_R8`, `PF_G8` |
| 2 | `R8UnormSrgb` | 1×1 | 1 | R | float | 1 | `PF_G8` + sRGB(Vulkan·Metal) |
| 3 | `R8Uint` | 1×1 | 1 | R | uint | — | `PF_R8_UINT` |
| 4 | `R8Sint` | 1×1 | 1 | R | int | — | `PF_R8_SINT` |
| 5 | `A8Unorm` | 1×1 | 1 | A | float | — | `PF_A8` |
| 6 | `RG8Unorm` | 1×1 | 2 | RG | float | 7 | `PF_R8G8` |
| 7 | `RG8UnormSrgb` | 1×1 | 2 | RG | float | 6 | `PF_R8G8` + sRGB(Vulkan) |
| 8 | `RG8Snorm` | 1×1 | 2 | RG | float | — | `PF_V8U8` |
| 9 | `RG8Uint` | 1×1 | 2 | RG | uint | — | `PF_R8G8_UINT` |
| 10 | `RGB8Unorm` | 1×1 | 3 | RGB | float | 11 | `PF_R8G8B8` |
| 11 | `RGB8UnormSrgb` | 1×1 | 3 | RGB | float | 10 | `PF_R8G8B8` + sRGB |
| 12 | `RGBA8Unorm` | 1×1 | 4 | RGBA | float | 13 | `PF_R8G8B8A8` |
| 13 | `RGBA8UnormSrgb` | 1×1 | 4 | RGBA | float | 12 | `PF_R8G8B8A8` + sRGB |
| 14 | `RGBA8Snorm` | 1×1 | 4 | RGBA | float | — | `PF_R8G8B8A8_SNORM` |
| 15 | `RGBA8Uint` | 1×1 | 4 | RGBA | uint | — | `PF_R8G8B8A8_UINT` |
| 16 | `BGRA8Unorm` | 1×1 | 4 | RGBA(메모리 B·G·R·A) | float | 17 | `PF_B8G8R8A8` |
| 17 | `BGRA8UnormSrgb` | 1×1 | 4 | RGBA | float | 16 | `PF_B8G8R8A8` + sRGB |
| 18 | `R16Unorm` | 1×1 | 2 | R | float | — | `PF_G16` |
| 19 | `R16Uint` | 1×1 | 2 | R | uint | — | `PF_R16_UINT` |
| 20 | `R16Sint` | 1×1 | 2 | R | int | — | `PF_R16_SINT` |
| 21 | `R16Float` | 1×1 | 2 | R | float | — | `PF_R16F`, `PF_R16F_FILTER` |
| 22 | `RG16Unorm` | 1×1 | 4 | RG | float | — | `PF_G16R16` |
| 23 | `RG16Snorm` | 1×1 | 4 | RG | float | — | `PF_G16R16_SNORM` |
| 24 | `RG16Uint` | 1×1 | 4 | RG | uint | — | `PF_R16G16_UINT` |
| 25 | `RG16Sint` | 1×1 | 4 | RG | int | — | `PF_R16G16_SINT` |
| 26 | `RG16Float` | 1×1 | 4 | RG | float | — | `PF_G16R16F`, `PF_G16R16F_FILTER` |
| 27 | `RGBA16Unorm` | 1×1 | 8 | RGBA | float | — | `PF_R16G16B16A16_UNORM`, `PF_A16B16G16R16` |
| 28 | `RGBA16Snorm` | 1×1 | 8 | RGBA | float | — | `PF_R16G16B16A16_SNORM` |
| 29 | `RGBA16Uint` | 1×1 | 8 | RGBA | uint | — | `PF_R16G16B16A16_UINT` |
| 30 | `RGBA16Sint` | 1×1 | 8 | RGBA | int | — | `PF_R16G16B16A16_SINT` |
| 31 | `RGBA16Float` | 1×1 | 8 | RGBA | float | — | `PF_FloatRGBA` |
| 32 | `R32Uint` | 1×1 | 4 | R | uint | — | `PF_R32_UINT` |
| 33 | `R32Sint` | 1×1 | 4 | R | int | — | `PF_R32_SINT` |
| 34 | `R32Float` | 1×1 | 4 | R | float | — | `PF_R32_FLOAT`(색) |
| 35 | `RG32Uint` | 1×1 | 8 | RG | uint | — | `PF_R32G32_UINT` |
| 36 | `RG32Float` | 1×1 | 8 | RG | float | — | `PF_G32R32F` |
| 37 | `RGB32Uint` | 1×1 | 12 | RGB | uint | — | `PF_R32G32B32_UINT` |
| 38 | `RGB32Sint` | 1×1 | 12 | RGB | int | — | `PF_R32G32B32_SINT` |
| 39 | `RGB32Float` | 1×1 | 12 | RGB | float | — | `PF_R32G32B32F` |
| 40 | `RGBA32Uint` | 1×1 | 16 | RGBA | uint | — | `PF_R32G32B32A32_UINT` |
| 41 | `RGBA32Float` | 1×1 | 16 | RGBA | float | — | `PF_A32B32G32R32F` |
| 42 | `R64Uint` | 1×1 | 8 | R | uint64 | — | `PF_R64_UINT` |
| 43 | `R5G6B5Unorm` | 1×1 | 2 | RGB | float | — | `PF_R5G6B5_UNORM` |
| 44 | `B5G5R5A1Unorm` | 1×1 | 2 | RGBA | float | — | `PF_B5G5R5A1_UNORM` |
| 45 | `RGB10A2Unorm` | 1×1 | 4 | RGBA | float | — | `PF_A2B10G10R10` |
| 46 | `RG11B10Ufloat` | 1×1 | 4 | RGB | float | — | `PF_FloatR11G11B10`, `PF_FloatRGB` |
| 47 | `RGB9E5Ufloat` | 1×1 | 4 | RGB | float | — | `PF_R9G9B9EXP5` |
| 64 | `D16Unorm` | 1×1 | 2 | D | float | — | `PF_ShadowDepth`(D16 선택) |
| 65 | `D24Unorm` | 1×1 | 4 | D | float | — | `PF_D24` |
| 66 | `D32Float` | 1×1 | 4 | D | float | — | `PF_R32_FLOAT` + 깊이 타깃 |
| 67 | `D24UnormS8Uint` | 1×1 | plane D 4 / S 1 | D·S | float·uint | — | `PF_DepthStencil`(24비트 선택) |
| 68 | `D32FloatS8Uint` | 1×1 | plane D 4 / S 1 | D·S | float·uint | — | `PF_DepthStencil`(32비트 선택) |
| 80 | `BC1Unorm` | 4×4 | 8 | RGBA | float | 81 | `PF_DXT1` |
| 81 | `BC1UnormSrgb` | 4×4 | 8 | RGBA | float | 80 | `PF_DXT1` + sRGB |
| 82 | `BC2Unorm` | 4×4 | 16 | RGBA | float | 83 | `PF_DXT3` |
| 83 | `BC2UnormSrgb` | 4×4 | 16 | RGBA | float | 82 | `PF_DXT3` + sRGB |
| 84 | `BC3Unorm` | 4×4 | 16 | RGBA | float | 85 | `PF_DXT5` |
| 85 | `BC3UnormSrgb` | 4×4 | 16 | RGBA | float | 84 | `PF_DXT5` + sRGB |
| 86 | `BC4Unorm` | 4×4 | 8 | R | float | — | `PF_BC4` |
| 87 | `BC5Unorm` | 4×4 | 16 | RG | float | — | `PF_BC5` |
| 88 | `BC6HUfloat` | 4×4 | 16 | RGB | float | — | `PF_BC6H` |
| 89 | `BC7Unorm` | 4×4 | 16 | RGBA | float | 90 | `PF_BC7` |
| 90 | `BC7UnormSrgb` | 4×4 | 16 | RGBA | float | 89 | `PF_BC7` + sRGB |
| 96 | `ETC2RGB8Unorm` | 4×4 | 8 | RGB | float | 97 | `PF_ETC2_RGB` |
| 97 | `ETC2RGB8UnormSrgb` | 4×4 | 8 | RGB | float | 96 | `PF_ETC2_RGB` + sRGB |
| 98 | `ETC2RGBA8Unorm` | 4×4 | 16 | RGBA | float | 99 | `PF_ETC2_RGBA` |
| 99 | `ETC2RGBA8UnormSrgb` | 4×4 | 16 | RGBA | float | 98 | `PF_ETC2_RGBA` + sRGB |
| 100 | `EACR11Unorm` | 4×4 | 8 | R | float | — | `PF_ETC2_R11_EAC` |
| 101 | `EACRG11Unorm` | 4×4 | 16 | RG | float | — | `PF_ETC2_RG11_EAC` |
| 112 | `ASTC4x4Unorm` | 4×4 | 16 | RGBA | float | 113 | `PF_ASTC_4x4`, `PF_ASTC_4x4_NORM_RG`(+ swizzle) |
| 113 | `ASTC4x4UnormSrgb` | 4×4 | 16 | RGBA | float | 112 | `PF_ASTC_4x4` + sRGB |
| 114 | `ASTC4x4Hdr` | 4×4 | 16 | RGBA | float | — | `PF_ASTC_4x4_HDR` |
| 115 | `ASTC6x6Unorm` | 6×6 | 16 | RGBA | float | 116 | `PF_ASTC_6x6`, `PF_ASTC_6x6_NORM_RG`(+ swizzle) |
| 116 | `ASTC6x6UnormSrgb` | 6×6 | 16 | RGBA | float | 115 | `PF_ASTC_6x6` + sRGB |
| 117 | `ASTC6x6Hdr` | 6×6 | 16 | RGBA | float | — | `PF_ASTC_6x6_HDR` |
| 118 | `ASTC8x8Unorm` | 8×8 | 16 | RGBA | float | 119 | `PF_ASTC_8x8`, `_NORM_RG`(+ swizzle) |
| 119 | `ASTC8x8UnormSrgb` | 8×8 | 16 | RGBA | float | 118 | `PF_ASTC_8x8` + sRGB |
| 120 | `ASTC8x8Hdr` | 8×8 | 16 | RGBA | float | — | `PF_ASTC_8x8_HDR` |
| 121 | `ASTC10x10Unorm` | 10×10 | 16 | RGBA | float | 122 | `PF_ASTC_10x10`, `_NORM_RG`(+ swizzle) |
| 122 | `ASTC10x10UnormSrgb` | 10×10 | 16 | RGBA | float | 121 | `PF_ASTC_10x10` + sRGB |
| 123 | `ASTC10x10Hdr` | 10×10 | 16 | RGBA | float | — | `PF_ASTC_10x10_HDR` |
| 124 | `ASTC12x12Unorm` | 12×12 | 16 | RGBA | float | 125 | `PF_ASTC_12x12`, `_NORM_RG`(+ swizzle) |
| 125 | `ASTC12x12UnormSrgb` | 12×12 | 16 | RGBA | float | 124 | `PF_ASTC_12x12` + sRGB |
| 126 | `ASTC12x12Hdr` | 12×12 | 16 | RGBA | float | — | `PF_ASTC_12x12_HDR` |
| 160 | `NV12` | plane별(11절) | plane별 | YUV | float | — | `PF_NV12` |
| 161 | `P010` | plane별(11절) | plane별 | YUV | float | — | `PF_P010` |

형식 수: 비압축 색 47, 깊이·스텐실 5, BC 11, ETC2·EAC 6, ASTC 15, 비디오 2. 합계 86(sRGB 변형 16 포함)이다.

**ASTC 블록 크기.** UE는 2D 블록 5종(4×4·6×6·8×8·10×10·12×12)만 둔다. ES 3.2·Vulkan·Metal에는 5×4·5×5·6×5·8×5·8×6·10×5·10×6·10×8·12×10도 있다(`ES32` 178–179쪽). 이 문서는 UE 범위를 따르고 나머지 크기는 번호 구간(112–159)에 남는 자리로 둔다.

**BC1의 알파.** Rocks `BC1Unorm`은 D3D의 BC1 의미(1비트 punch-through 알파 포함)다. D3D12 `BC1_UNORM`, GL `COMPRESSED_RGBA_S3TC_DXT1_EXT`(UE GL:1102와 같다), Vulkan `BC1_RGBA_UNORM_BLOCK`이다. UE Vulkan은 `BC1_RGB_UNORM_BLOCK` + RGB1 mapping으로 알파를 버린다(VK:849). 이 동작은 따르지 않는다.

### 5.3 제외·이전·합친 형식

**제외(14개).**

| UE | 사유 |
|---|---|
| `PF_A8R8G8B8` | UE 원문이 옛 데이터 읽기 전용이라 밝힘(`PF.h:56`). 하위 호환 대상이 없는 Rocks에는 불필요하다. 의미는 `RGBA8Unorm`·`BGRA8Unorm`으로 표현된다 |
| `PF_ATC_RGB`·`PF_ATC_RGBA_E`·`PF_ATC_RGBA_I` | UE가 미지원 형식으로 표시(`PF.h:59–61`), 대응 backend·압축 모듈 없음 |
| `PF_ETC1` | UE가 미지원 형식으로 표시(`PF.h:63`). ETC2가 ETC1을 포함한다 |
| `PF_PVRTC2`·`PF_PVRTC4` | iOS Metal 전용(MTL:830–850), 압축 모듈 없음(4.2절). 같은 용도는 ASTC가 맡는다 |
| `PF_PLATFORM_HDR_0` | 텍스처 형식이 아니라 플랫폼이 고르는 HDR 출력 형식이다. HDR 출력은 RenderBase 14절 후속이며 그때 스왑체인 설정에 명시 형식(예: `RGB10A2Unorm`·`RGBA16Float`, Metal의 확장 범위 형식)으로 정한다 |
| `PF_PLATFORM_HDR_1`·`_2` | UE 예약 값(`PF.h:83–84`) |
| `PF_UYVY` | 어느 UE backend도 장치 형식을 주지 않는다(4.2절) |
| `PF_A1` | Core 지원 0이고 어느 backend도 켜지 않는다(4.2절) |
| `PF_L8` | Core 지원 0, backend 대응 없음. 휘도는 `R8Unorm` + swizzle `rrr1`로 표현된다 |
| `PF_XGXR8` | backend 대응 없음, 쿠킹이 BGRA8로 낸다(4.2절) |

**다른 개념으로 이전(6개).**

| UE | Rocks 표현 |
|---|---|
| `PF_X24_G8` | 깊이·스텐실 텍스처 view의 plane 선택(`Stencil`). 형식이 아니다(10절) |
| `PF_ASTC_4x4_NORM_RG` … `_12x12_NORM_RG`(5개) | 저장 형식은 ASTC LDR과 같고 차이는 "LA 끝점을 RG로 읽는" swizzle뿐이다(`PF.h:105`). Rocks는 텍스처 swizzle 메타(A0-G, RenderBase 6.5절)가 있으므로 `ASTC*Unorm` + swizzle `agxx`류로 표현한다. 쿠킹이 어떤 swizzle을 기록할지는 압축 단계에서 에셋 쪽이 정한다 |

**같은 저장 형식으로 합침.** `PF_G8` → `R8Unorm`(+ swizzle `rrr1`), `PF_G16` → `R16Unorm`, `PF_FloatRGB` → `RG11B10Ufloat`, `PF_G16R16F_FILTER` → `RG16Float`, `PF_R16F_FILTER` → `R16Float`, `PF_A16B16G16R16` → `RGBA16Unorm`. 필터 가능 여부는 형식 이름이 아니라 능력 플래그(`Filterable`)가 나타낸다. "플랫폼이 고르는 깊이"(`PF_DepthStencil`·`PF_ShadowDepth`·`PF_D24`)는 명시 깊이 형식으로 대체한다(10절).

**UE에 없지만 넣는 것.** sRGB 변형 16개(PX1 (가) 기준, UE는 생성 플래그로 표현)와 `D32Float`(UE는 `PF_R32_FLOAT` + 깊이 타깃 플래그)다. 저장 형식이 UE에 있으므로 "UE 지원 범위 안"으로 본다. 그 밖의 DXGI·Vulkan 형식(R8Snorm·R16Snorm·RG32Sint·RGBA32Sint·ASTC 비정사각 블록 등)은 넣지 않는다.
