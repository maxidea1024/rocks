# 픽셀 형식(PixelFormat) 설계

기준일: 2026-11-18, 개정 5(초안). UE 5.8이 정의하는 픽셀 형식 전체를 Rocks에 들이기 위한 조사와 구현 스펙이다. 범위는 다음과 같다.
- 형식 목록과 고정 번호표, UE 대응표, 제외 형식과 사유
- 형식 정보 표(`PixelFormatInfo`)와 형식 능력 플래그(`PixelFormatCaps`)
- backend 대응 표(D3D12·D3D11·Vulkan·OpenGL ES·Metal)와 장치 능력 질의(GX1)의 연결
- sRGB 표현, 깊이·스텐실, 비디오 형식(다중 plane)
- 이름 변경(`Format` → `PixelFormat`)과 구현 단계

배경은 사용자 지시(2026-11-17)다. "Format이 아니라 PixelFormat으로 해야 명확하지 않나? 그리고 무슨 픽셀 포맷이 두개밖에 없어?", "언리얼에서 지원하는거 full로 지원해야함". 이 지시에 따라 형식 부분은 AssetSystem A0-G에서 분리하여 렌더 쪽이 먼저 구현한다(15절).

**근거 표기.** UE 근거는 UE 5.8.4(`release`, commit `f45729c0b6f097038e72b52d366a62baeca3275b`, 로컬 클론) 원본이다. 경로는 `Engine/Source/Runtime/` 기준이며 다음 약어를 쓴다.

| 약어 | 경로 |
|---|---|
| `PF.h` | `Core/Public/PixelFormat.h` |
| `PF.cpp` | `Core/Private/Misc/PixelFormat.cpp` |
| `RHI.cpp` | `RHI/Private/RHI.cpp` |
| `RHIRes.h` | `RHI/Public/RHIResources.h` |
| `DXGI` | `RHICore/Internal/DXGIUtilities.h` |
| `D12S` | `D3D12RHI/Private/D3D12State.cpp`(D3D12 형식 표) |
| `D12D` | `D3D12RHI/Private/D3D12Device.cpp`(D3D12 능력 질의) |
| `D11` | `Windows/D3D11RHI/Private/D3D11Device.cpp` |
| `VK` | `VulkanRHI/Private/VulkanDevice.cpp` |
| `GL` | `OpenGLDrv/Private/OpenGLDevice.cpp` |
| `MTL` | `Apple/MetalRHI/Private/MetalRHI.cpp` |
| `RU` | `RenderCore/Private/RenderUtils.cpp` |

`Developer/`로 시작하는 경로는 `Engine/Source/Developer/` 기준이다. OpenGL ES 근거는 Khronos "OpenGL ES 3.2 (May 5, 2022)" 사양서 원문(`ES32` 쪽 번호)과 Android NDK 29.0.13113456 헤더(`gl2ext.h`·`gl32.h`, `sysroot/usr/include/GLES2`·`GLES3`)다. Vulkan 형식 이름은 UE가 동봉한 `ThirdParty/Vulkan/Include/vulkan/vulkan_core.h`로 확인하였다. Rocks 경로는 저장소 루트 기준이고 `origin/feature/engine-runtime`의 `ecc2b2f`에서 확인하였다.

**상태 표기.** AssetSystem과 같다. **구현**(동작·시험), **계약**(API·형식이 있고 미지원 값은 명시 오류), **후속**(API도 없음). 이 문서는 설계이며 코드는 아직 바꾸지 않았다. 확인하지 못한 사실은 그 자리에 **미확인**으로 명시한다.

**용어 방침.** 이름은 Rocks식으로 짓고 UE 이름(`EPixelFormat`·`PF_*`·`GPixelFormats`·`TexCreate_*`)은 대응표와 근거 열에만 둔다. BC1–7·ETC2·EAC·ASTC·sRGB·SRV·UAV·typeless처럼 그래픽 API와 업계에서 함께 쓰는 말은 그대로 쓴다.

**UE 레거시 제외.** UE가 하위 호환을 위해 남긴 형식·경로는 따르지 않는다(2026-11-16 사용자 지시). UE가 원문 주석으로 레거시·미지원이라 밝힌 형식은 5.2절에서 근거 줄과 함께 제외한다.

## 1. 결정 요약

| 항목 | 결정 | 근거 |
|---|---|---|
| 이름 | `enum class PixelFormat`, 정보 `PixelFormatInfo`, 능력 `PixelFormatCaps`. 형식 이름은 채널 대문자 + 비트 + 성분 타입(`RGBA8Unorm`·`BC7UnormSrgb`·`ASTC6x6Hdr`) | 사용자 지시 "PixelFormat으로", 기존 `RGBA8Unorm` 이름 유지 |
| 지원 범위 | UE `EPixelFormat` 94개 값(0–93) 가운데 `PF_Unknown`을 뺀 93개를 전부 검토하였다. 73개는 Rocks 형식으로 대응하고, 6개는 형식이 아닌 다른 개념(스텐실 view plane, ASTC + swizzle)으로 옮기고, 14개는 사유를 적어 제외한다. Rocks 형식은 86개다(권장안 (가) 기준, sRGB 변형 16개 포함) | 3·5절 |
| 같은 저장 형식 | UE가 이름만 다르게 둔 같은 저장 형식(`PF_G8`·`PF_R8`, `PF_FloatRGB`·`PF_FloatR11G11B10`, `*_FILTER` 변형 등)은 Rocks 형식 하나로 합친다. 회색·알파 같은 채널 의미는 형식이 아니라 텍스처 swizzle(A0-G)로 표현한다 | 5.3절 |
| 정보 표 | GraphicsTypes에 장치 없는 정보 표를 둔다. 블록 크기·블록 바이트·채널·성분 타입·압축 계열·깊이/스텐실 비트·plane·sRGB 짝 | 6절. UE `PF.cpp:31–150` |
| 능력 | 장치마다 형식별 능력 플래그를 시작 때 한 번 채운다(GX1). D3D12·Vulkan은 API 질의, OpenGL ES는 사양 표 + 확장 조건 정적 표, Metal은 GPU family 정적 표 | 7·8절 |
| 대체 금지 | 장치가 형식을 지원하지 않을 때 다른 형식으로 몰래 바꾸지 않는다. 생성은 `UnsupportedCapability`로 실패한다. 대체 형식 선택은 쿠킹 프로필이나 호출자가 능력 질의 결과로 한다 | 8.3절. UE의 대체 사슬(`VK:768`·`VK:781`·`GL:1039–1044`)은 쿠킹 바이트 계약(RenderBase 6.5절)과 맞지 않는다 |
| sRGB 표현 | sRGB 변형을 별도 형식으로 둔다(결정 PX1 (가), 2026-11-17) | 9절 |
| 깊이·스텐실 | UE의 "플랫폼이 고르는 깊이 형식" 대신 명시 형식 5개(`D16Unorm`·`D24Unorm`·`D32Float`·`D24UnormS8Uint`·`D32FloatS8Uint`)를 둔다. 스텐실 읽기는 형식이 아니라 view의 plane 선택이다 | 10절 |
| 비디오 형식 | `NV12`·`P010`을 번호표와 정보 표에 넣고(plane 2개) D3D12 생성까지 지금 구현한다(결정 PX3, 2026-11-17). 다른 backend는 그 backend 대응 때 한다 | 11절 |
| 저장 값 | 번호를 이번에 다시 매긴다. 기존 값 0–2가 저장된 쿠킹 데이터·fixture는 없다 | 12절 |
| 문서 위치 | RenderBase(974줄)에 넣지 않고 별도 문서로 둔다. RenderBase GX1·GX2·6.5절과 AssetSystem A0-G가 이 문서를 가리킨다 | 2절 |

## 2. 문서 위치와 교차 참조

UE 대응표와 backend 대응 표가 형식마다 한 행씩이므로 분량이 크다. RenderBase는 이미 974줄이고 형식 표는 렌더 기반 단계(RB)와 독립이므로 별도 문서로 둔다.

| 문서 | 위치 | 관계 |
|---|---|---|
| [렌더 기반](RenderBase/README.ko.md) | 4.4절 GX1(장치 능력) | 형식별 능력 질의는 이 문서 7·8절이 정한다. GX1의 나머지(셰이더 형식 목록·전역 한도·동작 표시)는 RenderBase에 남는다 |
| 〃 | 4.4절 GX2(자원 desc·view) | 깊이/스텐실 형식·view plane·sRGB 짝 view는 이 문서 9·10절, desc 구조는 GX2 |
| 〃 | 6.5절 텍스처 플랫폼 데이터 입력 계약 | 블록 압축 크기 공식·고정 번호표·직렬화 이름은 이 문서 6·12절. "블록 압축 형식 표(BC1–7·ASTC·ETC2와 GLES·Android 가용성)는 압축 단계 착수 전에 확정"의 형식·가용성 부분을 이 문서가 확정한다. 인코더와 프로필별 형식 선택은 에셋 쪽 |
| [에셋 시스템](AssetSystem/README.ko.md) | 1절 A0-G 행 | 형식 부분을 이 문서로 옮긴다. 개정안은 15절 |
| 〃 | 12.4.3절·12.6절 | 쿠킹 출력 형식 이름을 이 문서의 Rocks 이름으로 읽는다 |
| [셰이더 시스템](ShaderSystem/05-Prerequisites.ko.md) | 5.2절 G1–G3 | G1 "backend별 형식 지원 표"와 G2 "ES 확장 의존 형식"을 이 문서 8.6절이 구체화한다 |
| [텍스처 스트리밍](TextureStreaming.ko.md) | 4.3절 | 블록 압축 형식의 최소 스트리밍 mip(블록 4×4 이상)은 이 문서 정보 표의 블록 크기를 쓴다 |

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

## 4. UE 5.8 조사

### 4.1 `EPixelFormat` 전체

`PF.h:16–115`에 0–93의 값과 `PF_MAX = 94`가 있다. 정보 표 `GPixelFormats`는 `PF.cpp:31–150`이다. 아래 표의 "블록"은 BlockSizeX×BlockSizeY·BlockBytes, "성분"은 NumComponents, "Core 지원"은 `PF.cpp` 마지막 열 `Supported`다(backend가 시작 때 덮어쓴다, 4.3절). BlockSizeZ는 모든 행이 1(`PF_Unknown`·`PF_PLATFORM_HDR_*`는 0)이다. "UE backend 대응"은 형식 표에 실제 장치 형식이 있는 backend와 그 줄이다. "Rocks"는 5절의 처리다.

| 값 | UE | 의미 | 블록 | 성분 | Core 지원 | UE backend 대응 | Rocks |
|---|---|---|---|---|---|---|---|
| 0 | `PF_Unknown` | 없음 | 0×0·0 | 0 | 0 | — | `Unknown` |
| 1 | `PF_A32B32G32R32F` | RGBA float32 | 1×1·16 | 4 | 1 | D12S:861, VK:772, GL:1002, MTL:800 | `RGBA32Float` |
| 2 | `PF_B8G8R8A8` | BGRA unorm8(메모리 B가 첫 바이트) | 1×1·4 | 4 | 1 | D12S:862(typeless), VK:766, GL:1073(RGBA8 + R·B swizzle), MTL:801 | `BGRA8Unorm` |
| 3 | `PF_G8` | 회색 unorm8. R 형식에 R → RGB 복제(`PF.h:21` 주석) | 1×1·1 | 1 | 1 | D12S:863, VK:767, GL:1076, MTL:802 | `R8Unorm`(합침) |
| 4 | `PF_G16` | 회색 unorm16 | 1×1·2 | 1 | 1 | D12S:864, VK:876·880, GL:1041(Android는 R16F 대체)·1043, MTL:803 | `R16Unorm`(합침) |
| 5 | `PF_DXT1` | BC1 | 4×4·8 | 3 | 1 | D12S:865, VK:849, GL:1102, MTL:914(macOS) | `BC1Unorm` |
| 6 | `PF_DXT3` | BC2 | 4×4·16 | 4 | 1 | D12S:866, VK:850, GL:1103, MTL:915 | `BC2Unorm` |
| 7 | `PF_DXT5` | BC3 | 4×4·16 | 4 | 1 | D12S:867, VK:851, GL:1104, MTL:916 | `BC3Unorm` |
| 8 | `PF_UYVY` | 4:2:2 packed YUV | 2×1·4 | 4 | 0 | 없음(D12S:869 UNKNOWN, D11:102 "Not supported", GL:1003 빈 값, MTL:978 Invalid) | 제외 |
| 9 | `PF_FloatRGB` | `PF_FloatR11G11B10`과 같음(`PF.h:27`) | 1×1·4 | 3 | 1 | D12S:903, VK:768(대체 사슬), GL:1063, MTL:896·918 | `RG11B10Ufloat`(합침) |
| 10 | `PF_FloatRGBA` | RGBA float16 | 1×1·8 | 4 | 1 | D12S:905, VK:769, GL:1065, MTL:979 | `RGBA16Float` |
| 11 | `PF_DepthStencil` | 플랫폼이 고르는 깊이 + 스텐실 | 1×1·4 | 1 | 0 | D12S:872·882, VK:835·842, GL:1006·1012, MTL:812·820 | `D24UnormS8Uint`·`D32FloatS8Uint`(명시) |
| 12 | `PF_ShadowDepth` | 플랫폼이 고르는 그림자 깊이 | 1×1·4 | 1 | 0 | D12S:890(D16), VK:770(D16), GL:1017(D16), MTL:902·949·954 | `D16Unorm`(명시) |
| 13 | `PF_R32_FLOAT` | R float32. 깊이 타깃 플래그와 함께면 D32F | 1×1·4 | 1 | 1 | D12S:893, VK:803, GL:1045, MTL:982 | `R32Float`(색), `D32Float`(깊이) |
| 14 | `PF_G16R16` | RG unorm16 | 1×1·4 | 2 | 1 | D12S:894, VK:773, GL:1089(데스크톱만), MTL:989 | `RG16Unorm` |
| 15 | `PF_G16R16F` | RG float16 | 1×1·4 | 2 | 1 | D12S:895, VK:774, GL:1046, MTL:994 | `RG16Float` |
| 16 | `PF_G16R16F_FILTER` | 15와 같은 저장 형식 | 1×1·4 | 2 | 1 | D12S:896, VK:775, GL:1047, MTL:995 | `RG16Float`(합침) |
| 17 | `PF_G32R32F` | RG float32 | 1×1·8 | 2 | 1 | D12S:897, VK:771, GL:1048, MTL:996 | `RG32Float` |
| 18 | `PF_A2B10G10R10` | RGB10 A2 unorm(DXGI R10G10B10A2) | 1×1·4 | 4 | 1 | D12S:898, VK:785, GL:1049, MTL:997 | `RGB10A2Unorm` |
| 19 | `PF_A16B16G16R16` | RGBA unorm16 | 1×1·8 | 4 | 1 | D12S:899, VK:786, GL:1019(RGBA16F로 다름), MTL:998 | `RGBA16Unorm`(합침) |
| 20 | `PF_D24` | 24비트 깊이 | 1×1·4 | 1 | 1 | D12S:900, VK:781(대체 사슬), GL:1018, MTL:905·960·964 | `D24Unorm`(명시) |
| 21 | `PF_R16F` | R float16 | 1×1·2 | 1 | 1 | D12S:901, VK:782, GL:1050, MTL:999 | `R16Float` |
| 22 | `PF_R16F_FILTER` | 21과 같은 저장 형식 | 1×1·2 | 1 | 1 | D12S:902, VK:783, GL:1051, MTL:1000 | `R16Float`(합침) |
| 23 | `PF_BC5` | BC5 | 4×4·16 | 2 | 1 | D12S:911, VK:853, GL:1083(데스크톱만), MTL:970 | `BC5Unorm` |
| 24 | `PF_V8U8` | RG snorm8 | 1×1·2 | 2 | 1 | D12S:910(SNORM), VK:802(UNORM으로 다름), GL:1082(데스크톱만), MTL:1001 | `RG8Snorm` |
| 25 | `PF_A1` | 1비트 알파 | 1×1·1 | 1 | 0 | D12S:912(R1_UNORM, Core 지원 0 유지), 그 밖 없음 | 제외 |
| 26 | `PF_FloatR11G11B10` | R11 G11 B10 unsigned float | 1×1·4 | 3 | 0(D12S:908 등이 1로 바꿈) | D12S:907, VK:784, GL:1064, MTL:898·922 | `RG11B10Ufloat` |
| 27 | `PF_A8` | 알파 unorm8 | 1×1·1 | 1 | 1 | D12S:913(A8_UNORM), VK:787(R8 + 000R), GL:1052(R8), MTL:1006(R8) | `A8Unorm` |
| 28 | `PF_R32_UINT` | R uint32 | 1×1·4 | 1 | 1 | D12S:914, VK:778, GL:1053, MTL:1007 | `R32Uint` |
| 29 | `PF_R32_SINT` | R sint32 | 1×1·4 | 1 | 1 | D12S:915, VK:779, GL:1054, MTL:1008 | `R32Sint` |
| 30 | `PF_PVRTC2` | PVRTC 2bpp | 8×4·8 | 4 | 0 | MTL:845(iOS)만 | 제외 |
| 31 | `PF_PVRTC4` | PVRTC 4bpp | 4×4·8 | 4 | 0 | MTL:847·849(iOS)만 | 제외 |
| 32 | `PF_R16_UINT` | R uint16 | 1×1·2 | 1 | 1 | D12S:916, VK:776, GL:1055, MTL:1017 | `R16Uint` |
| 33 | `PF_R16_SINT` | R sint16 | 1×1·2 | 1 | 1 | D12S:917, VK:777, GL:1056, MTL:1016 | `R16Sint` |
| 34 | `PF_R16G16B16A16_UINT` | RGBA uint16 | 1×1·8 | 4 | 1 | D12S:918, VK:795, GL:1021, MTL:1009 | `RGBA16Uint` |
| 35 | `PF_R16G16B16A16_SINT` | RGBA sint16 | 1×1·8 | 4 | 1 | D12S:919, VK:796, GL:1022, MTL:1010 | `RGBA16Sint` |
| 36 | `PF_R5G6B5_UNORM` | 16비트 묶음 R5 G6 B5 | 1×1·2 | 3 | 0(D12S:921 등이 1로) | D12S:920(B5G6R5), VK:788, GL:1031, MTL:909(iOS) | `R5G6B5Unorm` |
| 37 | `PF_R8G8B8A8` | RGBA unorm8 | 1×1·4 | 4 | 1 | D12S:924(typeless), VK:790, GL:1074, MTL:1011 | `RGBA8Unorm` |
| 38 | `PF_A8R8G8B8` | "legacy loading only, do NOT use"(`PF.h:56`) | 1×1·4 | 4 | 1 | MTL:1012만 | 제외 |
| 39 | `PF_BC4` | BC4 | 4×4·8 | 1 | 1 | D12S:868, VK:852, GL:1084(데스크톱만), MTL:968 | `BC4Unorm` |
| 40 | `PF_R8G8` | RG unorm8 | 1×1·2 | 2 | 1 | D12S:927, VK:801, GL:1058, MTL:1015 | `RG8Unorm` |
| 41 | `PF_ATC_RGB` | ATITC. "Unsupported Format"(`PF.h:59`) | 4×4·8 | 3 | 0 | 없음 | 제외 |
| 42 | `PF_ATC_RGBA_E` | 〃(`PF.h:60`) | 4×4·16 | 4 | 0 | 없음 | 제외 |
| 43 | `PF_ATC_RGBA_I` | 〃(`PF.h:61`) | 4×4·16 | 4 | 0 | 없음 | 제외 |
| 44 | `PF_X24_G8` | 깊이·스텐실 버퍼의 스텐실 읽기 SRV 전용. "Don't use for creating textures"(`PF.h:62`) | 1×1·1 | 1 | 0 | D12S:876·886, VK:836·843, GL:1016, MTL:815·823 | view plane으로 옮김(10절) |
| 45 | `PF_ETC1` | ETC1. "Unsupported Format"(`PF.h:63`) | 4×4·8 | 3 | 0 | 없음 | 제외 |
| 46 | `PF_ETC2_RGB` | ETC2 RGB8 | 4×4·8 | 3 | 0 | VK:869, GL:1109(Android), MTL:878 | `ETC2RGB8Unorm` |
| 47 | `PF_ETC2_RGBA` | ETC2 RGBA8(EAC 알파) | 4×4·16 | 4 | 0 | VK:870, GL:1110, MTL:880 | `ETC2RGBA8Unorm` |
| 48 | `PF_R32G32B32A32_UINT` | RGBA uint32 | 1×1·16 | 4 | 1 | D12S:928, VK:798, GL:1023, MTL:804 | `RGBA32Uint` |
| 49 | `PF_R16G16_UINT` | RG uint16 | 1×1·4 | 4(오기, 4.3절) | 1 | D12S:929, VK:793, GL:1027, MTL:805 | `RG16Uint` |
| 50 | `PF_ASTC_4x4` | ASTC LDR 4×4 | 4×4·16 | 4 | 0 | VK:860, GL:1117, MTL:851 | `ASTC4x4Unorm` |
| 51 | `PF_ASTC_6x6` | ASTC LDR 6×6 | 6×6·16 | 4 | 0 | VK:861, GL:1118, MTL:853 | `ASTC6x6Unorm` |
| 52 | `PF_ASTC_8x8` | ASTC LDR 8×8 | 8×8·16 | 4 | 0 | VK:862, GL:1119, MTL:855 | `ASTC8x8Unorm` |
| 53 | `PF_ASTC_10x10` | ASTC LDR 10×10 | 10×10·16 | 4 | 0 | VK:863, GL:1120, MTL:857 | `ASTC10x10Unorm` |
| 54 | `PF_ASTC_12x12` | ASTC LDR 12×12 | 12×12·16 | 4 | 0 | VK:864, GL:1121, MTL:859 | `ASTC12x12Unorm` |
| 55 | `PF_BC6H` | BC6H unsigned float | 4×4·16 | 3 | 1 | D12S:932(UF16), VK:854, GL:1087(데스크톱만), MTL:972 | `BC6HUfloat` |
| 56 | `PF_BC7` | BC7 | 4×4·16 | 4 | 1 | D12S:933(typeless), VK:855, GL:1086(데스크톱만), MTL:974 | `BC7Unorm` |
| 57 | `PF_R8_UINT` | R uint8 | 1×1·1 | 1 | 1 | D12S:934, VK:780, GL:1057, MTL:1018 | `R8Uint` |
| 58 | `PF_L8` | 휘도 8비트 | 1×1·1 | 1 | 0 | 없음(MTL:767 버퍼 표만) | 제외 |
| 59 | `PF_XGXR8` | 법선용 X·G·X·R 배치 | 1×1·4 | 4 | 1 | 없음 | 제외 |
| 60 | `PF_R8G8B8A8_UINT` | RGBA uint8 | 1×1·4 | 4 | 1 | D12S:925, VK:791, GL:1075, MTL:1013 | `RGBA8Uint` |
| 61 | `PF_R8G8B8A8_SNORM` | RGBA snorm8 | 1×1·4 | 4 | 1 | D12S:926, VK:792, GL:1061(RGBA8 unorm으로 대체), MTL:1014 | `RGBA8Snorm` |
| 62 | `PF_R16G16B16A16_UNORM` | RGBA unorm16 | 1×1·8 | 4 | 1 | D12S:936, VK:800, GL:1024, MTL:1021 | `RGBA16Unorm` |
| 63 | `PF_R16G16B16A16_SNORM` | RGBA snorm16 | 1×1·8 | 4 | 1 | D12S:937, VK:799, GL:1025(RGBA16 + SHORT), MTL:1022 | `RGBA16Snorm` |
| 64 | `PF_PLATFORM_HDR_0` | 플랫폼 HDR 출력 형식 | 0×0·0 | 0 | 0 | MTL:893·933(BGR10_XR_sRGB)·943(RGBA16Float)만 | 제외 |
| 65 | `PF_PLATFORM_HDR_1` | "Reserved"(`PF.h:83`) | 0×0·0 | 0 | 0 | 없음 | 제외 |
| 66 | `PF_PLATFORM_HDR_2` | "Reserved"(`PF.h:84`) | 0×0·0 | 0 | 0 | 없음 | 제외 |
| 67 | `PF_NV12` | 4:2:0 8비트 2-plane(R8 휘도 + RG8 ¼ 색차, `PF.cpp:110–111`) | 1×1·1(의미 없음) | 1 | 0 | D12S:938, D11:177 | `NV12` |
| 68 | `PF_R32G32_UINT` | RG uint32 | 1×1·8 | 2 | 1 | D12S:931, VK:797, GL:1059, MTL:807 | `RG32Uint` |
| 69 | `PF_ETC2_R11_EAC` | EAC R11 | 4×4·8 | 1 | 0 | VK:871, GL:1111, MTL:882 | `EACR11Unorm` |
| 70 | `PF_ETC2_RG11_EAC` | EAC RG11 | 4×4·16 | 2 | 0 | VK:872, GL:1112, MTL:884 | `EACRG11Unorm` |
| 71 | `PF_R8` | R unorm8 | 1×1·1 | 1 | 1 | D12S:935, VK:804, GL:1029, MTL:1019 | `R8Unorm` |
| 72 | `PF_B5G5R5A1_UNORM` | 16비트 묶음 B5 G5 R5 A1(DXGI 배치) | 1×1·2 | 4 | 0(D12S:923 등이 1로) | D12S:922, VK:789(대체 사슬), GL:1037, MTL:911(iOS) | `B5G5R5A1Unorm` |
| 73 | `PF_ASTC_4x4_HDR` | ASTC HDR 4×4 | 4×4·16 | 4 | 0 | VK:885, GL:1125, MTL:865 | `ASTC4x4Hdr` |
| 74 | `PF_ASTC_6x6_HDR` | ASTC HDR 6×6 | 6×6·16 | 4 | 0 | VK:886, GL:1126, MTL:867 | `ASTC6x6Hdr` |
| 75 | `PF_ASTC_8x8_HDR` | ASTC HDR 8×8 | 8×8·16 | 4 | 0 | VK:887, GL:1127, MTL:869 | `ASTC8x8Hdr` |
| 76 | `PF_ASTC_10x10_HDR` | ASTC HDR 10×10 | 10×10·16 | 4 | 0 | VK:888, GL:1128, MTL:871 | `ASTC10x10Hdr` |
| 77 | `PF_ASTC_12x12_HDR` | ASTC HDR 12×12 | 12×12·16 | 4 | 0 | VK:889, GL:1129, MTL:873 | `ASTC12x12Hdr` |
| 78 | `PF_G16R16_SNORM` | RG snorm16 | 1×1·4 | 2 | 1 | D12S:940, VK:805, GL:1090(데스크톱만), MTL:1027 | `RG16Snorm` |
| 79 | `PF_R8G8_UINT` | RG uint8 | 1×1·2 | 2 | 1 | D12S:941, VK:806, GL:1067, MTL:1028 | `RG8Uint` |
| 80 | `PF_R32G32B32_UINT` | RGB uint32 | 1×1·12 | 3 | 1 | D12S:942, VK:807, GL:1068, MTL:1029(Invalid) | `RGB32Uint` |
| 81 | `PF_R32G32B32_SINT` | RGB sint32 | 1×1·12 | 3 | 1 | D12S:943, VK:808, GL:1069, MTL:1031(Invalid) | `RGB32Sint` |
| 82 | `PF_R32G32B32F` | RGB float32 | 1×1·12 | 3 | 1 | D12S:944, VK:809, GL:1070, MTL:1033(Invalid) | `RGB32Float` |
| 83 | `PF_R8_SINT` | R sint8 | 1×1·1 | 1 | 1 | D12S:945, VK:810, GL:1071, MTL:1035 | `R8Sint` |
| 84 | `PF_R64_UINT` | R uint64(64비트 image atomic) | 1×1·8 | 1 | 0 | VK:822(SM5, R32G32 대체), MTL:1036(Invalid) | `R64Uint` |
| 85 | `PF_R9G9B9EXP5` | RGB9 E5 공유 지수 | 1×1·4 | 4 | 0 | D12S:946만(VK·GL 없음, MTL:1038 Invalid) | `RGB9E5Ufloat` |
| 86 | `PF_P010` | 4:2:0 10비트 2-plane(R16 + RG16 ¼, 상위 10비트, `PF.cpp:137–138`) | 1×1·2(의미 없음) | 1 | 0 | D12S:947, D11:187 | `P010` |
| 87 | `PF_ASTC_4x4_NORM_RG` | ASTC를 LA 끝점으로 인코딩, 실행 때 RG로 swizzle(`PF.h:105`) | 4×4·16 | 2 | 0 | 없음 | ASTC + swizzle로 옮김 |
| 88 | `PF_ASTC_6x6_NORM_RG` | 〃 | 6×6·16 | 2 | 0 | 없음 | 〃 |
| 89 | `PF_ASTC_8x8_NORM_RG` | 〃 | 8×8·16 | 2 | 0 | 없음 | 〃 |
| 90 | `PF_ASTC_10x10_NORM_RG` | 〃 | 10×10·16 | 2 | 0 | 없음 | 〃 |
| 91 | `PF_ASTC_12x12_NORM_RG` | 〃 | 12×12·16 | 2 | 0 | 없음 | 〃 |
| 92 | `PF_R16G16_SINT` | RG sint16 | 1×1·4 | 4(오기, 4.3절) | 1 | D12S:930, VK:794, GL:1028, MTL:806 | `RG16Sint` |
| 93 | `PF_R8G8B8` | RGB unorm8(24비트) | 1×1·3 | 3 | 0 | VK:811(샘플·RT 필수), GL:1077 | `RGB8Unorm` |

UE 블록 압축 판정 함수는 셋이다. BC 계열 `IsBlockCompressedFormat`(`PF.h:322–338`), ASTC 계열(`PF.h:295–320`), ETC 계열(`PF.h:280–293`)이다. BC만 최상위 크기가 4의 배수여야 한다(`RequiresBlock4Alignment`, `PF.h:451–455`). 깊이·스텐실 판정은 `PF.h:403–430`이며 `PF_R32_FLOAT`도 깊이로 본다.

### 4.2 UE가 레거시·미지원·플랫폼 전용으로 다루는 것

| 형식 | UE 취급 | 근거 |
|---|---|---|
| `PF_A8R8G8B8` | 옛 데이터 읽기 전용, 쓰지 말 것 | `PF.h:56` 주석, `CoreUObject/Public/UObject/NoExportTypes.h:252`. Metal만 대응(MTL:1012). 엔진 코드 한 곳이 셰이더 출력 형식 이름으로 쓴다(`Engine/Private/Particles/ParticleGpuSimulation.cpp:1816`) |
| `PF_ATC_RGB`·`PF_ATC_RGBA_E`·`PF_ATC_RGBA_I` | 미지원 형식 | `PF.h:59–61` 주석, Core 지원 0(`PF.cpp:79–81`), 대응 backend 없음 |
| `PF_ETC1` | 미지원 형식 | `PF.h:63` 주석, Core 지원 0(`PF.cpp:83`), 대응 backend 없음 |
| `PF_PVRTC2`·`PF_PVRTC4` | iOS Metal 전용 | Core 지원 0(`PF.cpp:66–67`), iOS 분기에서만 지원(MTL:830–850). 압축 모듈이 없다(`Developer/`의 `TextureFormat*`는 ASTC·DXT·ETC2·IntelISPCTexComp·Oodle·Uncompressed뿐) |
| `PF_PLATFORM_HDR_0` | 플랫폼 HDR 출력용 | Metal이 HDR 출력 지원 여부로 켠다(MTL:887–894, 933–943). 블록 정보도 Core 표에 없음(`PF.cpp:106`) |
| `PF_PLATFORM_HDR_1`·`_2` | 예약 | `PF.h:83–84` 주석 |
| `PF_UYVY` | 어느 RHI도 지원하지 않음 | D3D11 "TODO: Not supported"(D11:102), D3D12 UNKNOWN(D12S:869), GL 빈 값(GL:1003), Metal Invalid(MTL:978) |
| `PF_A1` | 지원 안 함 | Core 지원 0(`PF.cpp:59`)이고 어느 backend도 1로 바꾸지 않는다. 크기 계산만 특례(`RHI.cpp:1780–1786`) |
| `PF_L8` | 지원 안 함 | Core 지원 0(`PF.cpp:99`), backend 대응 없음, 채널 플래그 None(`RU:334`) |
| `PF_XGXR8` | 장치 형식 없음 | backend 대응 없음. 쿠킹 형식 이름 `XGXR8`은 `PF_B8G8R8A8`로 출력된다(`Developer/TextureFormatUncompressed/Private/TextureFormatUncompressed.cpp:134–137`) |
| `PF_X24_G8` | 텍스처 생성 금지, 스텐실 읽기 view 전용 | `PF.h:62` 주석, view 생성이 plane을 스텐실로 변환한다(`RHI/Private/RHIResources.cpp:348–372`) |
| `PF_ASTC_*_NORM_RG` | RHI의 swizzle 지원이 필요 | `PF.h:105` 주석. 쿠킹 형식 `ASTC_NormalRG_Precise`가 6×6만 낸다(`Developer/TextureFormatASTC/Private/TextureFormatASTC.cpp:106, 284–288`). 공개 RHI 형식 표 어디에도 장치 형식이 없다(Metal 버퍼 표 MTL:790–794는 Invalid) |
| `PF_G16R16F_FILTER`·`PF_R16F_FILTER` | 필터 가능 표시용 별칭 | 모든 backend가 비필터 형식과 같은 장치 형식을 준다(D12S:896·902, VK:775·783, GL:1047·1051, MTL:995·1000) |
| `PF_FloatRGB` | `PF_FloatR11G11B10`과 같음 | `PF.h:27` 주석, `NoExportTypes.h:217` |

### 4.3 정보 표(`FPixelFormatInfo`)

필드(`PF.h:462–512`):
- 이름 `Name`, 형식 값 `UnrealFormat`
- `BlockSizeX`·`BlockSizeY`·`BlockSizeZ`·`BlockBytes`·`NumComponents`
- 장치 형식 `PlatformFormat`(RHI가 채움), 능력 `Capabilities`(RHI가 채움), 지원 여부 `Supported`(1비트), 24비트 unorm 깊이 여부 `bIs24BitUnormDepthStencil`

크기 계산은 블록 수 ceil(폭/블록 폭) × ceil(높이/블록 높이) × 블록 바이트이고(`PF.cpp:186–218`) 깊이 방향 블록은 없다(`PF.cpp:158` 주석). mip 크기는 축마다 max(1, n >> mip)이다(`PF.cpp:220–239`). 채널 플래그는 정보 표가 아니라 별도 표 `GetPixelFormatValidChannels`(`RU:272–371`)에 있다.

**UE 표의 오기.** Rocks 정보 표는 아래를 따르지 않는다.
- `PF_R16G16_UINT`·`PF_R16G16_SINT`의 성분 수가 4다(`PF.cpp:87, 148`). 채널 표는 RG다(`RU:325, 368`).
- `PF_R16G16B16A16_UNORM`·`_SNORM`의 이름 문자열이 `"R16G16B16A16_UINT"`·`"R16G16B16A16_SINT"`다(`PF.cpp:104–105`).
- 채널 표가 `PF_R32_SINT`를 RGBA로 기재한다(`RU:305`).
- Vulkan이 `PF_V8U8`(snorm)을 `R8G8_UNORM`에 대응한다(VK:802). D3D12는 `R8G8_SNORM`이다(D12S:910).
- OpenGL이 `PF_A16B16G16R16`을 `GL_RGBA16F`에 대응한다(GL:1019). 같은 의미의 `PF_R16G16B16A16_UNORM`은 `GL_RGBA16`이다(GL:1024).
- OpenGL이 `PF_R16G16B16A16_SNORM`을 `GL_RGBA16`(unorm) + `GL_SHORT`로 대응한다(GL:1025).

### 4.4 형식 능력(`EPixelFormatCapabilities`)과 판정

플래그(`PF.h:234–272`): `Texture1D`·`Texture2D`·`Texture3D`·`TextureCube`·`RenderTarget`·`DepthStencil`·`TextureMipmaps`·`TextureLoad`·`TextureSample`·`TextureGather`·`TextureAtomics`·`TextureBlendable`·`TextureStore`·`Buffer`·`VertexBuffer`·`IndexBuffer`·`BufferLoad`·`BufferStore`·`BufferAtomics`·`UAV`·`TypedUAVLoad`·`TypedUAVStore`·`TextureFilterable`·`LossyCompressible`. 조회는 `UE::PixelFormat::HasCapabilities`(`PF.h:531–537`)다. MSAA 표본 수는 이 플래그에 없다.

| backend | 판정 방식 | 위치 |
|---|---|---|
| D3D12 | 장치 생성 뒤 형식마다 `CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT)`를 원 형식·SRV 형식·UAV 형식·DSV 형식으로 각각 질의하고 `D3D12_FORMAT_SUPPORT1/2` 비트를 옮긴다. `TextureFilterable`은 `SHADER_SAMPLE`에서 함께 켠다(별도 비트 없음) | D12D:192–270, 호출 `D3D12RHI/Private/D3D12Adapter.cpp:1562` |
| D3D11 | D3D12와 같은 구조로 `CheckFormatSupport`·`FormatSupport2`를 질의 | D11:457–524 |
| Vulkan | 형식마다 우선순위 목록의 VkFormat을 차례로 `vkGetPhysicalDeviceFormatProperties`로 질의하고 요구 능력을 만족하는 첫 형식을 고른다. `optimalTilingFeatures`·`linearTilingFeatures`·`bufferFeatures` 비트를 옮기고 mip은 `vkGetPhysicalDeviceImageFormatProperties`의 `maxMipLevels > 1`, gather는 `shaderImageGatherExtended`로 정한다. 지원 여부는 기본 요구(`TextureSample`) 또는 버퍼 능력이다 | VK:730–897(대응), 1080–1250(질의), 1222(기본 요구), 1239(지원 판정) |
| OpenGL | 질의하지 않는다. 내부 형식 enum별 정적 분류(Texture·Render·Filterable·Image·DepthStencil)를 능력으로 옮긴다 | GL:384–500(분류), 502–559(옮김) |
| Metal | 기본 능력(`RHIInitDefaultPixelFormatCapabilities`)을 준 뒤 GPU family별로 몇 형식의 능력을 수정한다 | MTL:1049–1062, 기본 능력 `RHI.cpp:1752–1773` |

기본 능력 규칙(`RHI.cpp:1752–1773`): 지원 형식 가운데 BC 계열은 텍스처·mip·load·sample·gather·filter만, 그 밖은 모든 텍스처·버퍼·UAV 플래그를 주고 깊이 형식이 아니면 `DepthStencil`을 제외한다.

### 4.5 backend 대응

**D3D12.**
- 형식 표는 `InitializeD3D12PixelFormats`(D12S:858–949)다. 표에 없는 형식은 `PlatformFormat = 0`(`DXGI_FORMAT_UNKNOWN`)이다. ETC2·EAC·ASTC·PVRTC·ATC·`PF_R8G8B8`·`PF_R64_UINT`·`PF_L8`·`PF_XGXR8`·`PF_A8R8G8B8`이 여기에 해당한다.
- sRGB 짝이 있는 형식은 typeless로 생성한다: `B8G8R8A8_TYPELESS`·`R8G8B8A8_TYPELESS`·`BC1/2/3/7_TYPELESS`(D12S:862, 865–867, 924, 933).
- 깊이 형식도 typeless다: `R16_TYPELESS`(D16), `R24G8_TYPELESS`(D24S8), `R32G8X24_TYPELESS`(D32FS8)(D12S:872–890, 900).
- view 형식 결정 함수:
  - `FindShaderResourceFormat(format, bSRGB)`: typeless → UNORM 또는 UNORM_SRGB, 깊이 typeless → 깊이 읽기 형식(`DXGI:86–120`)
  - `FindDepthStencilFormat`: typeless → DSV 형식(`DXGI:166–176`)
  - `FindUnorderedAccessFormat`: typeless → UNORM, D32FS8은 `R32_FLOAT_X8X24_TYPELESS`(`DXGI:154–163`)
  - `GetPlatformTextureResourceFormat`: 깊이 타깃 플래그면 `R32_FLOAT` → `R32_TYPELESS` 등으로 변환한다(`DXGI:122–151`). `PF_R32_FLOAT`가 D32F 깊이로 쓰이는 경로다
  - `GetPlaneSliceFromViewFormat`: 깊이·스텐실·NV12·P010의 plane 번호(`DXGI:217–256`)
- 텍스처 생성: 자원 형식(`D3D12RHI/Private/D3D12Texture.cpp:230, 599`), DSV·RTV 형식(`:604–612`), SRV(`:1169`, `D3D12RHI/Private/D3D12SRV.cpp:170–180`), UAV(`D3D12RHI/Private/D3D12UAV.cpp:155–160`).
- 깊이 기본은 32비트 float + 스텐실이다(`r.D3D12.Depth24Bit` 기본 0, `D3D12RHI/Private/D3D12RHI.cpp:39–44`. 도움말 문자열은 1을 "(default)"로 적어 값과 다르다).

**D3D11.** D3D12와 같은 DXGI 유틸리티를 쓴다(`Windows/D3D11RHI/Private/D3D11RHI.cpp`, `D3D11SRV.cpp`). 형식 표(D11:92–188)의 차이는 다음과 같다.
- `PF_R9G9B9EXP5`를 대응하지 않는다(D3D12는 D12S:946).
- NV12·P010 텍스처에 SRV를 만들지 않는다(`Windows/D3D11RHI/Private/D3D11Texture.cpp:325–331`).
- 깊이 기본값은 D3D12와 같다(`r.D3D11.Depth24Bit` 기본 0, D11:29–34).

**Vulkan.**
- 대응은 `MapFormatSupport(UE 형식, {우선순위 VkFormat…}, component mapping, 요구 능력, 블록 바이트)`(VK:765–890, 정의 1226–1250)다. 첫 VkFormat이 요구를 못 채우면 다음 형식으로 대체하고 로그만 남긴다(VK:1211–1214). 대체 사슬이 있는 형식은 `PF_FloatRGB`·`PF_FloatR11G11B10`(→ R16G16B16 → RGBA16F, VK:768, 784), `PF_G16R16`·`PF_A16B16G16R16`·`PF_R16G16B16A16_UNORM/SNORM`(→ float16, VK:773, 786, 799–800), `PF_B5G5R5A1_UNORM`(→ BGRA8, VK:789), `PF_D24`(VK:781), 깊이·스텐실(VK:835–843), `PF_R64_UINT`(→ R32G32, VK:822)이다.
- component mapping으로 채널 의미를 구성한다. `PF_G8`은 R001(VK:767), `PF_A8`은 000R(VK:787), 스텐실 view는 RR01(VK:836)이다.
- 압축 계열은 플랫폼 hook으로 켠다. Windows 등 일반 플랫폼은 BC만(`VulkanRHI/Private/VulkanGenericPlatform.h:76–79`), Android는 ASTC·ETC2 항상, BC는 `r.Vulkan.SupportsBCTextureFormats`일 때(`VulkanRHI/Private/Android/VulkanAndroidPlatform.h:137–141`, `VulkanAndroidPlatform.cpp:71–72, 433–436`). ASTC HDR은 `VK_EXT_texture_compression_astc_hdr`일 때다(VK:883–890).
- 형식 질의는 4.4절 표와 같다.

**OpenGL(ES).**
- 형식 표는 `SetupTextureFormat(UE 형식, {internal, internal sRGB, format, type, 압축, BGRA})`(GL:992–1130)다. 구조체는 `OpenGLDrv/Private/OpenGLDrvPrivate.h:234–241`이고 `InternalFormat[0]`이 선형, `[1]`이 sRGB다.
- 플랫폼 분기가 많다.
  - 데스크톱(`PLATFORM_DESKTOP`)만: BC4·BC5·BC6H·BC7(RGTC·BPTC), `PF_V8U8`, `PF_G16R16`·`PF_G16R16_SNORM`(GL:1078–1091).
  - S3TC(BC1–3)는 `SupportsDXT`일 때다. 데스크톱 기본 클래스는 true(`OpenGLDrv/Private/OpenGLBase.h:95`), ES 클래스는 false(`OpenGLES.h:67`).
  - ETC2·EAC는 Android에서만 등록한다(GL:1106–1114, `OpenGLES.cpp:241`).
  - ASTC LDR은 `GL_KHR_texture_compression_astc_ldr`, ASTC HDR은 거기에 `_hdr`까지 있을 때다(`OpenGLDrv/Private/OpenGLDrv.cpp:294–296`). ASTC HDR도 LDR과 같은 internal format enum을 쓴다(GL:1125–1129).
  - Android는 `PF_G16`을 `GL_R16F`로 대체한다(GL:1039–1041).
  - ES에는 `GL_UNSIGNED_SHORT_1_5_5_5_REV`가 없어 `PF_B5G5R5A1_UNORM`을 `5_5_5_1` + R·B swizzle로 대응한다(GL:1033–1037, `OpenGL3.h:628`). 비트 배치가 실제로 일치하는지는 **미확인**이다(5_5_5_1은 알파가 최하위 비트다).
- Windows의 `GLES31_PC` 미리보기는 ES context가 아니라 데스크톱 GL 4.x context(`FWindowsOpenGL : FOpenGL4`, `OpenGLDrv/Private/Windows/WindowsOpenGL.h:19`)에서 위 데스크톱 분기를 탄다. Android는 `FAndroidOpenGL : FOpenGLES`(`OpenGLDrv/Private/Android/AndroidOpenGL.h:14`)다.
- sRGB는 생성 때 `InternalFormat[bSRGB]`로 정해진다(`OpenGLDrv/Private/OpenGLTexture.cpp:584–589`). SRV가 다른 형식을 요구하면 실패한다. "texture views are unsupported"(`OpenGLDrv/Private/OpenGLUAV.cpp:122–124`).
- 깊이 기본은 24비트다(`r.OpenGL.Depth24Bit` 기본 1, GL:72–76). `PF_D24`는 ES에서 `GL_DEPTH_COMPONENT24`(`OpenGLES.h:69`), 데스크톱 기본 클래스에서 `GL_DEPTH_COMPONENT16`(`OpenGLBase.h:117`)이다.

**OpenGL ES 3.2 사양 확인(Rocks 대응의 근거).**
- 색 형식(Table 8.10, `ES32` 162–163쪽):
  - color-renderable: R8, RG8, RGB8, RGB565, RGBA4, RGB5_A1, RGBA8, RGB10_A2, RGB10_A2UI, SRGB8_ALPHA8, R16F, RG16F, RGBA16F, R32F, RG32F, RGBA32F, R11F_G11F_B10F, 그리고 RGB 3성분이 아닌 정수 형식(R·RG·RGBA의 8I·8UI·16I·16UI·32I·32UI)이다. float 형식의 렌더 타깃이 ES 3.2 core다.
  - texture-filterable: unorm·snorm 8비트 형식 전부, RGB565, RGBA4, RGB5_A1, RGB10_A2, SRGB8, SRGB8_ALPHA8, R16F, RG16F, RGB16F, RGBA16F, R11F_G11F_B10F, RGB9_E5다. 32비트 float(R32F·RG32F·RGB32F·RGBA32F)는 filterable이 아니다.
  - 렌더 타깃이 아닌 형식: snorm 8비트, SRGB8, RGB16F, RGB32F, RGB9_E5, RGB 3성분 정수 형식(RGB8I…RGB32UI).
- 깊이·스텐실(Table 8.11, 164쪽): DEPTH_COMPONENT16·24·32F, DEPTH24_STENCIL8, DEPTH32F_STENCIL8, STENCIL_INDEX8.
- 압축(Table 8.17, 178–179쪽): ETC2·EAC 전 계열과 ASTC LDR 2D 블록 14종(4×4–12×12)과 각 sRGB가 core다. 3D 텍스처 열이 모두 비어 있어 ETC2·ASTC 3D 텍스처는 core가 아니다. cube map array는 PUNCHTHROUGH 두 형식을 뺀 전부가 가능하다.
- image load/store 형식(Table 8.26, 219–220쪽): RGBA32F, RGBA16F, R32F, RGBA32UI, RGBA16UI, RGBA8UI, R32UI, RGBA32I, RGBA16I, RGBA8I, R32I, RGBA8, RGBA8_SNORM.
- `DEPTH_STENCIL_TEXTURE_MODE`는 core 텍스처 파라미터다(Table 8.19, 189쪽). 사양 본문에 texture view가 없다(전문 검색 0건). 확장은 `GL_OES_texture_view`·`GL_EXT_texture_view`다(`gl2ext.h:726, 2204`).
- 확장 이름은 NDK 헤더로 확인하였다.
  - `GL_EXT_texture_compression_s3tc`(`gl2ext.h:2080`), `_s3tc_srgb`(`:2086`), `_rgtc`(`:2072`), `_bptc`(`:2058`)
  - `GL_KHR_texture_compression_astc_hdr`(`:201`), `_astc_sliced_3d`(`:237`), `GL_OES_texture_compression_astc`(`:649`)
  - `GL_EXT_texture_norm16`(`:2133`, 토큰 `GL_R16_EXT`·`GL_RG16_EXT`·`GL_RGBA16_EXT` `:2134–2136`), `GL_EXT_render_snorm`(`:1665`, `GL_R16_SNORM_EXT` 등 `:1669–1671`)
  - `GL_EXT_texture_format_BGRA8888`(`:2119`, `GL_BGRA8_EXT` `:1012`)
  - `GL_EXT_texture_sRGB_R8`(`:2150`, `GL_SR8_EXT` `:2151`), `GL_EXT_texture_sRGB_RG8`(`:2155`, `GL_SRG8_EXT` `:2156`)
  - `GL_OES_texture_float_linear`(`:690`), `GL_EXT_color_buffer_float`(`:1179`), `GL_EXT_color_buffer_half_float`(`:1183`)
- 각 확장 사양서의 세부(렌더 가능·필터 가능 여부, 불변 저장 `glTexStorage*`와의 조합)는 원문을 대조하지 않았다(**미확인**, 8.6절에 조건으로 적고 PF3 시험으로 검증).

**Metal(위치).**
- 텍스처 형식 표: MTL:798–1046(`GPixelFormats[...].PlatformFormat`), iOS·macOS 분기 MTL:830–977
- 버퍼 형식 표: MTL:700–796, `static_assert(PF_MAX == 94)` MTL:796
- 능력: MTL:1049–1062
- sRGB 변환: `UEToMetalFormat`(`Apple/MetalRHI/Private/MetalTexture.cpp:188–236`)

### 4.6 sRGB 처리

UE에서 sRGB는 형식이 아니라 텍스처 생성 플래그 `ETextureCreateFlags::SRGB`(`RHI/Public/RHIDefinitions.h:1100`, 별칭 `TexCreate_SRGB` `:1193`)다. 형식 하나에 플래그를 더해 backend가 sRGB 장치 형식을 고른다.

| backend | sRGB 장치 형식으로 바뀌는 지점 |
|---|---|
| D3D12·D3D11 | 자원은 typeless로 만들고 SRV·RTV 형식을 `FindShaderResourceFormat(format, bSRGB)`로 고른다(`DXGI:86–111`, `D3D12Texture.cpp:611–612, 1169, 1677–1680`). view마다 `bDisableSRGB`로 끌 수 있다(`RHIRes.h:2780`, 결정 `RHI/Private/RHIResources.cpp:415`). 짝이 없는 형식(R8 등)은 플래그가 있어도 선형 형식 그대로다 |
| Vulkan | 형식 표를 만든 뒤 sRGB 짝 표 `GVulkanSRGBFormat`을 만든다(VK:915–982). R8·RG8·RGB8·RGBA8·BGRA8·BC1/2/3/7·ETC2·ASTC LDR이 짝을 가진다. R8 sRGB는 ES3.1 기능 수준에서 쓰지 않는다(VK:923). 짝 형식을 샘플할 수 없으면 선형 형식으로 되돌린다(VK:956–963). sRGB 텍스처는 `MUTABLE_FORMAT`와 형식 목록(선형·sRGB)으로 만든다(`VulkanRHI/Private/VulkanTexture.cpp:387–399`) |
| OpenGL | 생성 때 `InternalFormat[1]`(GL 표의 sRGB 열)을 고른다(`OpenGLTexture.cpp:584`). 짝이 없으면 선형 형식이 그대로 들어 있다(예: `GL_RGBA32F` 두 칸, GL:1002) |
| Metal | `UEToMetalFormat(.., bSRGB)`가 RGBA8·BGRA8·BC1/2/3/7(macOS)·R8·ASTC(Apple GPU)를 `_sRGB` 형식으로 바꾼다(`MetalTexture.cpp:193–233`). ETC2 sRGB는 대응이 없다 |

쿠킹 쪽에서는 회색 8비트 + sRGB를 Windows에서 BGRA8로 대체한다(`Engine/Private/Texture.cpp:4438–4446`, Windows는 `SupportsGrayscaleSRGB() = false` `Core/Public/Windows/WindowsPlatformProperties.h:96–99`). R8 sRGB 장치 형식이 D3D에 없기 때문이다.

### 4.7 깊이·스텐실

| UE 형식 | 의미 | D3D12 | Vulkan | OpenGL | Metal |
|---|---|---|---|---|---|
| `PF_DepthStencil` | 플랫폼이 고르는 깊이 + 스텐실(`NoExportTypes.h:221–222`) | D32FS8 기본, D24S8은 설정(D12S:870–889) | D32FS8 우선 기본, D24S8은 설정(VK:832–845) | D24S8 기본(GL:1004–1015) | D24S8 가능하면, 아니면 D32FS8(MTL:809–828) |
| `PF_ShadowDepth` | 플랫폼이 고르는 그림자 깊이(`NoExportTypes.h:223–224`) | D16(D12S:890) | D16(VK:770) | D16(GL:1017, `OpenGLES.h:70`) | D16 또는 D32F(MTL:947–955) |
| `PF_D24` | 24비트 깊이, 스텐실 없음 | R24G8 typeless(D12S:900) | X8_D24 우선 대체 사슬(VK:781) | ES D24, 데스크톱 기본 D16(`OpenGLES.h:69`, `OpenGLBase.h:117`) | D24S8 또는 D32F(MTL:958–965) |
| `PF_R32_FLOAT` + 깊이 타깃 | D32F | `R32_TYPELESS` → `D32_FLOAT`(`DXGI:126, 172`) | 없음(`PF_D24` 사슬이 D32F를 포함) | 없음 | 없음 |
| `PF_X24_G8` | 스텐실 읽기 view 형식 | `X24_TYPELESS_G8_UINT`·`X32_TYPELESS_G8X24_UINT`, plane 1(D12S:876, 886, `DXGI:217–236`) | D/S VkFormat + stencil aspect, RR01(VK:836, `VulkanRHI/Private/VulkanView.cpp:192–199`) | `GL_DEPTH_STENCIL_TEXTURE_MODE = GL_STENCIL_INDEX` + swizzle (0, R, 0, 0)(`OpenGLUAV.cpp:127–139`) | `X24_Stencil8`·`X32_Stencil8`(MTL:815, 823) |

스텐실 읽기 view는 plane 선택으로 처리된다. view desc의 plane `ERHITexturePlane::Stencil`(`RHIRes.h:2639–2662`)을 고르면 형식이 `PF_X24_G8`로 정해지고, 반대로 `PF_X24_G8`를 주면 plane이 스텐실로 바뀐다(`RHI/Private/RHIResources.cpp:348–372`). 셰이더는 스텐실 값을 G 채널에서 읽는다(D3D `X24_TYPELESS_G8_UINT`의 G, GL swizzle의 G). 깊이·스텐실과 `PF_D24` 텍스처의 subresource plane 수는 2다(`RHIRes.h:1939`).

### 4.8 비디오 형식과 다중 plane

- `PF_NV12`: 휘도 R8 plane + ¼ 크기 색차 R8G8 plane(`PF.cpp:110–112`). `PF_P010`: R16 + R16G16, 상위 10비트 사용(`PF.cpp:137–139`). 블록 정보는 "의미 없음"으로 1을 넣었다.
- D3D12·D3D11만 대응한다(D12S:938–939, 947–948, D11:177–178, 187–188). Vulkan·OpenGL은 대응이 없고 Metal은 Invalid다(MTL:1024).
- D3D에서는 plane별 view 형식으로 plane을 고른다. NV12는 R8 → 0, R8G8 → 1, P010은 R16 → 0, R16G16 → 1이다(`DXGI:238–251`). 폭·높이는 2의 배수여야 한다(`DXGI:277–278, 315–316`). 단 `GetPlaneCount`는 깊이·스텐실만 2로 센다(`DXGI:258–269`).
- 텍스처 생성 때 SRV·RTV를 생성하지 않는다(`D3D12Texture.cpp:1704–1709`). 4 KiB 정렬 대상에서도 제외한다(`:187–191`).

### 4.9 압축 형식의 쿠킹 대상(위치)

쿠킹 자체는 에셋 쪽 일이므로 위치만 기재한다.
- 공통 기본 형식 이름 결정: `GetDefaultTextureFormatNamePerLayer`(`Engine/Private/Texture.cpp:4580`, 선언 `Engine/Public/TextureResource.h:690`), 설정별 기본 이름 `GetDefaultTextureFormatName`(`Texture.cpp:4201`, AssetSystem 12.4.3절이 인용).
- Windows: `Developer/Windows/WindowsTargetPlatfomControls/Public/GenericWindowsTargetPlatformControls.h:172–178`. 공통 기본값을 그대로 쓴다(BC 계열).
- Android: `Developer/Android/AndroidTargetPlatformControls/Private/AndroidTargetPlatformControls.cpp`
  - 공통 `:487–540`, ASTC `:542–602`(볼륨 텍스처는 ETC2로 되돌림 `:568–586`, ASTC HDR 미지원 시 대체 `:590–598`)
  - DXT `:604–626`(BC6H → RGBA16F, BC7 → DXT5)
  - ETC2 `:628–645`, 다중 `:647–`
- 압축 모듈: `Developer/TextureFormatDXT`(BC1–5), `TextureFormatIntelISPCTexComp`, `TextureFormatOodle`, `TextureFormatASTC`, `TextureFormatETC2`, `TextureFormatUncompressed`. BC6H·BC7을 어느 모듈이 내는지는 이 조사에서 확인하지 않았다(**미확인**).

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

## 7. 형식 능력 플래그(`PixelFormatCaps`)

| Rocks | 뜻 | UE 대응 | D3D12 판정 | Vulkan 판정(optimal tiling) | OpenGL ES 판정 |
|---|---|---|---|---|---|
| `Sampled` | 2D 텍스처로 만들고 셰이더에서 샘플·load | `Texture2D`·`TextureSample`·`TextureLoad` | `SUPPORT1_TEXTURE2D` + `SHADER_LOAD`/`SHADER_SAMPLE`(SRV 형식) | `SAMPLED_IMAGE_BIT` | 정적 표(Table 8.10·8.11·8.17 + 확장) |
| `Texture3D` | 3D 텍스처 | `Texture3D` | `SUPPORT1_TEXTURE3D` | `vkGetPhysicalDeviceImageFormatProperties`(3D) 성공 | 비압축 전부, ETC2·ASTC는 불가(Table 8.17), 확장 조건 8.6절 |
| `TextureCube` | cube(cube array 포함) | `TextureCube` | `SUPPORT1_TEXTURECUBE` | 이미지 형식 질의(CUBE_COMPATIBLE) | 전부(PUNCHTHROUGH 형식 제외, Rocks에 없음) |
| `Filterable` | 선형 필터 | `TextureFilterable` | `SHADER_SAMPLE`(UE와 같다, D12D:250) | `SAMPLED_IMAGE_FILTER_LINEAR_BIT` | Table 8.10 TF 열, 32F는 `GL_OES_texture_float_linear` |
| `Gather` | gather | `TextureGather` | `SHADER_GATHER` | `SAMPLED_IMAGE_BIT` + `shaderImageGatherExtended`(UE와 같다, VK:1173) | `Sampled`와 같음 |
| `Mipmaps` | mip 사슬 | `TextureMipmaps` | `SUPPORT1_MIP` | 이미지 형식 질의 `maxMipLevels > 1` | 전부 |
| `RenderTarget` | 색 렌더 타깃 | `RenderTarget` | `SUPPORT1_RENDER_TARGET` | `COLOR_ATTACHMENT_BIT` | Table 8.10 CR 열, snorm은 `GL_EXT_render_snorm`, norm16은 `GL_EXT_texture_norm16`(미확인) |
| `Blendable` | 렌더 타깃 블렌드 | `TextureBlendable` | `SUPPORT1_BLENDABLE` | `COLOR_ATTACHMENT_BLEND_BIT` | CR이면서 정수가 아님 |
| `Multisample` | MSAA 렌더 타깃(Rocks 추가) | 없음 | `D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS`로 표본 수 2·4·8 | 이미지 형식 질의 `sampleCounts` | `glGetInternalformativ(GL_SAMPLES)`(ES 3.0 이상, 사양 절 미대조) |
| `DepthStencil` | 깊이·스텐실 타깃 | `DepthStencil` | `SUPPORT1_DEPTH_STENCIL`(DSV 형식) | `DEPTH_STENCIL_ATTACHMENT_BIT` | Table 8.11 |
| `StorageRead` | typed UAV load | `TypedUAVLoad` | `SUPPORT2_UAV_TYPED_LOAD` | `STORAGE_IMAGE_BIT`(UE는 load·store를 한 비트로 봄, VK:1165) | Table 8.26 |
| `StorageWrite` | typed UAV store | `TypedUAVStore`·`TextureStore` | `SUPPORT2_UAV_TYPED_STORE` | `STORAGE_IMAGE_BIT` | Table 8.26 |
| `StorageAtomic` | image atomic | `TextureAtomics` | `SUPPORT2_UAV_ATOMIC_EXCHANGE` | `STORAGE_IMAGE_ATOMIC_BIT` | R32UI·R32I(Table 8.26 + 사양 8.23절 atomic 대상, 미대조) |
| `TexelBufferRead` | typed buffer SRV | `BufferLoad` | `SUPPORT1_BUFFER` | `UNIFORM_TEXEL_BUFFER_BIT` | Table 8.18(버퍼 텍스처 형식, 14.3절에서 대조) |
| `TexelBufferWrite` | typed buffer UAV | `BufferStore` | `SUPPORT2_UAV_TYPED_STORE` | `STORAGE_TEXEL_BUFFER_BIT`(읽기도 되는 형식만, 14.3절) | image 형식(Table 8.26) ∩ Table 8.18 |
| `TexelBufferAtomic` | typed buffer atomic | `BufferAtomics` | `SUPPORT2_UAV_ATOMIC_EXCHANGE` | `STORAGE_TEXEL_BUFFER_ATOMIC_BIT`(읽기도 되는 형식만) | R32UI·R32I |

UE 플래그 가운데 넣지 않는 것: `Texture1D`(Rocks 차원에 1D 없음, A0-G), `VertexBuffer`·`IndexBuffer`(별도 `VertexFormat`·`IndexFormat`), `UAV`(`StorageRead|StorageWrite`로 나눔), `LossyCompressible`(Vulkan 고정 비율 압축, VK:1183–1203, 후속), `AnyTexture`·`All*`(묶음 이름은 Rocks 상수로 둔다).

Metal 판정은 UE처럼 GPU family별 정적 표다(4.4절). Rocks Metal backend(SH5) 착수 때 Apple 형식 능력 표와 대조하여 정한다(**미확인**). D3D11은 D3D12와 같은 판정을 `CheckFormatSupport`로 한다.

## 8. backend 대응 표

### 8.1 표의 위치

- 형식 → 장치 형식 표는 backend 모듈 Private에 둔다(`D3D12Graphics/Private/D3D12PixelFormats.cpp`, `OpenGLGraphics/Private/OpenGLPixelFormats.cpp`, 이후 Vulkan·D3D11·Metal). GraphicsTypes에는 장치 enum을 두지 않는다.
- 표는 `PixelFormat` 번호로 바로 찾는 constexpr 배열이다. 정적 검사로 유효 번호마다 행이 하나 있는지(빈 행도 명시) 확인한다. 새 형식을 추가하면 모든 backend 표가 컴파일 단계에서 갱신을 요구받는다(UE Metal의 `static_assert(PF_MAX == 94)`, MTL:796과 같은 목적).
- 행 내용:
  - D3D12·D3D11: 자원 형식, SRV 형식(선형·sRGB), RTV, DSV, UAV, plane별 SRV 형식
  - Vulkan: VkFormat, component mapping(A8·스텐실 view), 필요한 장치 확장·기능
  - OpenGL ES: internal format, format, type, 압축 여부, 조건(ES 3.2 core / 확장 이름 / 데스크톱 context 전용), 정적 능력 기본값(Table 8.10 CR·TF 등)
  - Metal: MTLPixelFormat, GPU family 조건, 정적 능력

### 8.2 능력 질의(GX1)와의 연결

- 장치 생성 직후 backend가 위 표의 모든 형식을 질의하여 `DeviceCaps`의 형식 능력 배열(형식별 `PixelFormatCaps`, MSAA 표본 수 집합)을 채운다. 장치 수명 동안 바꾸지 않으므로 잠금 없이 여러 스레드가 읽는다.
- 공개 API(이름은 GX1 설계에서 확정):

```cpp
// 이 장치에서 format이 가진 능력이다. 장치 형식이 없으면 None이다.
PixelFormatCaps Device::GetPixelFormatCaps(PixelFormat format) const noexcept;

// format으로 만들 수 있는 MSAA 표본 수 집합이다(비트 n = 2^n 표본). 1표본은 늘 포함한다.
uint32 Device::GetPixelFormatSampleCounts(PixelFormat format) const noexcept;
```

- D3D12: 표의 자원·view 형식마다 `CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT)`를 부르고 7절 대응으로 옮긴다(UE D12D:202–270 구조).
- OpenGL ES: context 생성 뒤 버전과 확장 문자열을 읽고 표의 조건을 평가한다. MSAA는 `glGetInternalformativ`로 질의한다. 같은 표를 Windows 미리보기(데스크톱 GL 4.x context, ES 공통 API 부분집합)와 Android ES context가 함께 쓴다. 미리보기 context가 데스크톱 core 기능으로 형식을 지원하더라도 그 형식의 조건이 "ES 확장"이면 그 확장 이름이 문자열에 있을 때만 켠다. 미리보기 결과가 Android 장치보다 넓어지는 것을 막기 위해서다. 데스크톱 context에서만 켜는 형식은 표의 "데스크톱 전용" 조건으로 따로 표시한다.
- Vulkan(SH2): `vkGetPhysicalDeviceFormatProperties` + `vkGetPhysicalDeviceImageFormatProperties`(UE VK:1080–1219 구조). UE와 달리 우선순위 사슬로 다른 VkFormat을 고르지 않는다.

### 8.3 지원하지 않는 형식의 공개 방식

- 텍스처 생성은 desc의 용도에서 요구 능력을 계산하고(샘플 → `Sampled`, 렌더 타깃 → `RenderTarget`, MSAA → `Multisample` + 표본 수, 깊이 → `DepthStencil`, UAV → `StorageWrite` 등, GX2) 장치 능력에 없으면 `UnsupportedCapability`로 실패한다. A1이 `ErrorCode::UnsupportedCapability`를 더하기 전에는 `Unsupported`를 쓴다. `LastError`에 형식 이름·빠진 능력·backend를 기록한다(로그 문구는 영어).
- 장치 형식이 없는 형식(D3D12의 ETC2·ASTC 등)도 같은 오류다. 크래시·검증 계층 오류·조용한 대체가 없어야 한다.
- 대체 금지: backend는 다른 장치 형식으로 바꾸지 않는다. 형식 번호가 쿠킹 바이트 배치(RenderBase 6.5절)를 정하므로 바이트 크기가 다른 형식으로 바꾸면 업로드가 깨진다. UE의 대체 사슬(Vulkan `PF_FloatRGB` → RGBA16F 등, VK:768)이 바로 그 경우다. 대체는 두 곳에서만 한다.
  - 쿠킹: TargetProfile의 형식 표가 그 플랫폼에서 쓸 형식을 고른다(UE의 Android ASTC → ETC2 볼륨 대체와 같은 위치, 4.9절).
  - 실행: 렌더 타깃 형식을 고르는 렌더러 코드가 `GetPixelFormatCaps`로 확인하고 명시적으로 다른 형식을 고른다(예: 깊이 형식, 10절).

### 8.4 D3D12

| 번호 | Rocks | 자원 형식 → view 형식 | UE 근거 |
|---|---|---|---|
| 1 | `R8Unorm` | `R8_UNORM` | D12S:863, 935 |
| 2 | `R8UnormSrgb` | 없음(DXGI에 R8 sRGB 없음) | 4.6절 |
| 3 | `R8Uint` | `R8_UINT` | D12S:934 |
| 4 | `R8Sint` | `R8_SINT` | D12S:945 |
| 5 | `A8Unorm` | `A8_UNORM` | D12S:913 |
| 6 | `RG8Unorm` | `R8G8_UNORM` | D12S:927 |
| 7 | `RG8UnormSrgb` | 없음 | — |
| 8 | `RG8Snorm` | `R8G8_SNORM` | D12S:910 |
| 9 | `RG8Uint` | `R8G8_UINT` | D12S:941 |
| 10–11 | `RGB8Unorm`·`RGB8UnormSrgb` | 없음(DXGI에 24비트 형식 없음) | UE도 없음 |
| 12–13 | `RGBA8Unorm`·`RGBA8UnormSrgb` | `R8G8B8A8_TYPELESS` → `_UNORM`·`_UNORM_SRGB` | D12S:924, `DXGI:93, 105` |
| 14 | `RGBA8Snorm` | `R8G8B8A8_SNORM` | D12S:926 |
| 15 | `RGBA8Uint` | `R8G8B8A8_UINT` | D12S:925 |
| 16–17 | `BGRA8Unorm`·`BGRA8UnormSrgb` | `B8G8R8A8_TYPELESS` → `_UNORM`·`_UNORM_SRGB` | D12S:862, `DXGI:92, 104` |
| 18–21 | `R16Unorm`·`R16Uint`·`R16Sint`·`R16Float` | `R16_UNORM`·`R16_UINT`·`R16_SINT`·`R16_FLOAT` | D12S:864, 916, 917, 901 |
| 22–26 | `RG16Unorm`·`Snorm`·`Uint`·`Sint`·`Float` | `R16G16_UNORM`·`_SNORM`·`_UINT`·`_SINT`·`_FLOAT` | D12S:894, 940, 929, 930, 895 |
| 27–31 | `RGBA16Unorm`·`Snorm`·`Uint`·`Sint`·`Float` | `R16G16B16A16_UNORM`·`_SNORM`·`_UINT`·`_SINT`·`_FLOAT` | D12S:936, 937, 918, 919, 905 |
| 32–34 | `R32Uint`·`R32Sint`·`R32Float` | `R32_UINT`·`R32_SINT`·`R32_FLOAT` | D12S:914, 915, 893 |
| 35–36 | `RG32Uint`·`RG32Float` | `R32G32_UINT`·`R32G32_FLOAT` | D12S:931, 897 |
| 37–39 | `RGB32Uint`·`Sint`·`Float` | `R32G32B32_UINT`·`_SINT`·`_FLOAT` | D12S:942–944 |
| 40–41 | `RGBA32Uint`·`RGBA32Float` | `R32G32B32A32_UINT`·`_FLOAT` | D12S:928, 861 |
| 42 | `R64Uint` | 없음(UE도 대응 없음. UE 렌더러는 `R32G32_UINT`로 대체한다, `Renderer/Private/Nanite/NaniteCullRaster.cpp:6604`) | — |
| 43 | `R5G6B5Unorm` | `B5G6R5_UNORM`(비트 배치 같음, 5.1절) | D12S:920 |
| 44 | `B5G5R5A1Unorm` | `B5G5R5A1_UNORM` | D12S:922 |
| 45 | `RGB10A2Unorm` | `R10G10B10A2_UNORM` | D12S:898 |
| 46 | `RG11B10Ufloat` | `R11G11B10_FLOAT` | D12S:903, 907 |
| 47 | `RGB9E5Ufloat` | `R9G9B9E5_SHAREDEXP` | D12S:946 |
| 64 | `D16Unorm` | `R16_TYPELESS` → DSV `D16_UNORM`, SRV `R16_UNORM` | D12S:890, `DXGI:116, 173` |
| 65 | `D24Unorm` | `R24G8_TYPELESS` → DSV `D24_UNORM_S8_UINT`(스텐실 미사용), SRV `R24_UNORM_X8_TYPELESS` | D12S:900, `DXGI:114, 170` |
| 66 | `D32Float` | `R32_TYPELESS` → DSV `D32_FLOAT`, SRV `R32_FLOAT` | `DXGI:115, 126, 172` |
| 67 | `D24UnormS8Uint` | `R24G8_TYPELESS` → DSV `D24_UNORM_S8_UINT`, 깊이 SRV `R24_UNORM_X8_TYPELESS`(plane 0), 스텐실 SRV `X24_TYPELESS_G8_UINT`(plane 1) | D12S:872–876, `DXGI:222–228` |
| 68 | `D32FloatS8Uint` | `R32G8X24_TYPELESS` → DSV `D32_FLOAT_S8X24_UINT`, 깊이 SRV `R32_FLOAT_X8X24_TYPELESS`, 스텐실 SRV `X32_TYPELESS_G8X24_UINT` | D12S:882–886, `DXGI:230–236` |
| 80–85 | `BC1`–`BC3` 선형·sRGB | `BC1/2/3_TYPELESS` → `_UNORM`·`_UNORM_SRGB` | D12S:865–867, `DXGI:94–108` |
| 86 | `BC4Unorm` | `BC4_UNORM` | D12S:868 |
| 87 | `BC5Unorm` | `BC5_UNORM` | D12S:911 |
| 88 | `BC6HUfloat` | `BC6H_UF16` | D12S:932 |
| 89–90 | `BC7Unorm`·`BC7UnormSrgb` | `BC7_TYPELESS` → `_UNORM`·`_UNORM_SRGB` | D12S:933 |
| 96–126 | ETC2·EAC·ASTC 전부 | 없음 | UE도 없음 |
| 160 | `NV12` | `NV12`, plane SRV `R8_UNORM`(0)·`R8G8_UNORM`(1) | D12S:938, `DXGI:238–244` |
| 161 | `P010` | `P010`, plane SRV `R16_UNORM`(0)·`R16G16_UNORM`(1) | D12S:947, `DXGI:246–251` |

D3D11 표는 D3D12와 같다. `RGB9E5Ufloat`은 UE D3D11이 대응하지 않았으나(D11:92–188) DXGI 형식은 같으므로 Rocks는 D3D11에도 대응하고 능력 질의 결과를 따른다.

### 8.5 Vulkan

| 번호 | Rocks | VkFormat(`VK_FORMAT_` 생략) | UE 근거 |
|---|---|---|---|
| 1·2 | `R8Unorm`·`R8UnormSrgb` | `R8_UNORM`·`R8_SRGB` | VK:804, 923 |
| 3·4 | `R8Uint`·`R8Sint` | `R8_UINT`·`R8_SINT` | VK:780, 810 |
| 5 | `A8Unorm` | `R8_UNORM` + mapping (0,0,0,R), 샘플 전용(렌더 타깃·storage 능력 없음). `A8_UNORM`(`vulkan_core.h:2048`, maintenance5)은 쓰지 않는다 | VK:787 |
| 6·7 | `RG8Unorm`·`RG8UnormSrgb` | `R8G8_UNORM`·`R8G8_SRGB` | VK:801, 924 |
| 8 | `RG8Snorm` | `R8G8_SNORM`(UE는 UNORM, 4.3절) | VK:802 |
| 9 | `RG8Uint` | `R8G8_UINT` | VK:806 |
| 10·11 | `RGB8Unorm`·`RGB8UnormSrgb` | `R8G8B8_UNORM`·`R8G8B8_SRGB` | VK:811, 925 |
| 12·13 | `RGBA8Unorm`·`RGBA8UnormSrgb` | `R8G8B8A8_UNORM`·`_SRGB` | VK:790, 926 |
| 14·15 | `RGBA8Snorm`·`RGBA8Uint` | `R8G8B8A8_SNORM`·`_UINT` | VK:792, 791 |
| 16·17 | `BGRA8Unorm`·`BGRA8UnormSrgb` | `B8G8R8A8_UNORM`·`_SRGB` | VK:766, 921 |
| 18–21 | R16 4종 | `R16_UNORM`·`R16_UINT`·`R16_SINT`·`R16_SFLOAT` | VK:876, 776, 777, 782 |
| 22–26 | RG16 5종 | `R16G16_UNORM`·`_SNORM`·`_UINT`·`_SINT`·`_SFLOAT` | VK:773, 805, 793, 794, 774 |
| 27–31 | RGBA16 5종 | `R16G16B16A16_UNORM`·`_SNORM`·`_UINT`·`_SINT`·`_SFLOAT` | VK:800, 799, 795, 796, 769 |
| 32–34 | R32 3종 | `R32_UINT`·`R32_SINT`·`R32_SFLOAT` | VK:778, 779, 803 |
| 35·36 | `RG32Uint`·`RG32Float` | `R32G32_UINT`·`R32G32_SFLOAT` | VK:797, 771 |
| 37–39 | RGB32 3종 | `R32G32B32_UINT`·`_SINT`·`_SFLOAT` | VK:807–809 |
| 40·41 | `RGBA32Uint`·`RGBA32Float` | `R32G32B32A32_UINT`·`_SFLOAT` | VK:798, 772 |
| 42 | `R64Uint` | `R64_UINT`(storage 전용, `shaderImageInt64Atomics` 기능 필요 여부 미확인) | VK:822–829 |
| 43 | `R5G6B5Unorm` | `R5G6B5_UNORM_PACK16` | VK:788 |
| 44 | `B5G5R5A1Unorm` | `A1R5G5B5_UNORM_PACK16`(비트 배치가 DXGI B5G5R5A1과 같음) | VK:789 첫 후보 |
| 45 | `RGB10A2Unorm` | `A2B10G10R10_UNORM_PACK32` | VK:785 |
| 46 | `RG11B10Ufloat` | `B10G11R11_UFLOAT_PACK32` | VK:784 첫 후보 |
| 47 | `RGB9E5Ufloat` | `E5B9G9R9_UFLOAT_PACK32`(`vulkan_core.h:1931`, UE 대응 없음) | — |
| 64 | `D16Unorm` | `D16_UNORM` | VK:770 |
| 65 | `D24Unorm` | `X8_D24_UNORM_PACK32` | VK:781 첫 후보 |
| 66 | `D32Float` | `D32_SFLOAT` | VK:781 사슬 안 |
| 67 | `D24UnormS8Uint` | `D24_UNORM_S8_UINT`, 스텐실 view는 stencil aspect + mapping (R,R,0,1) | VK:835–836 |
| 68 | `D32FloatS8Uint` | `D32_SFLOAT_S8_UINT`, 스텐실 view 같음 | VK:842–843 |
| 80–85 | BC1–3 선형·sRGB | `BC1_RGBA_UNORM_BLOCK`·`_SRGB_BLOCK`, `BC2_*`, `BC3_*`(UE BC1은 RGB, 5.2절) | VK:849–851, 927–931 |
| 86–90 | BC4·BC5·BC6H·BC7 | `BC4_UNORM_BLOCK`·`BC5_UNORM_BLOCK`·`BC6H_UFLOAT_BLOCK`·`BC7_UNORM_BLOCK`·`BC7_SRGB_BLOCK` | VK:852–855, 931 |
| 96–99 | ETC2 | `ETC2_R8G8B8_UNORM_BLOCK`·`_SRGB_BLOCK`, `ETC2_R8G8B8A8_UNORM_BLOCK`·`_SRGB_BLOCK` | VK:869–870, 932–934 |
| 100·101 | EAC | `EAC_R11_UNORM_BLOCK`·`EAC_R11G11_UNORM_BLOCK` | VK:871–872 |
| 112–126 | ASTC | `ASTC_nxn_UNORM_BLOCK`·`_SRGB_BLOCK`·`_SFLOAT_BLOCK`(HDR은 `VK_EXT_texture_compression_astc_hdr` 또는 Vulkan 1.3 기능) | VK:860–864, 885–889, 935–949 |
| 160 | `NV12` | `G8_B8R8_2PLANE_420_UNORM`(`vulkan_core.h:1996`, UE 대응 없음) | — |
| 161 | `P010` | `G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16`(`vulkan_core.h:2006`, UE 대응 없음) | — |

Vulkan 기능 수준에서 지원이 보장되는 형식 집합(필수 형식 표)과의 대조는 SH2 VulkanGraphics 착수 때 한다(**미확인**). 능력 질의가 결과를 정하므로 표의 정확성에는 영향이 없다.

### 8.6 OpenGL ES

조건 표기: **core**는 ES 3.2 core(4.5절 사양 표), **확장 이름**은 그 확장이 있을 때만, **데스크톱**은 Windows 미리보기 context에서만이다. CR·TF는 Table 8.10의 color-renderable·texture-filterable이다. 내부 형식 이름의 `GL_`은 생략한다.

| 번호 | Rocks | internal / format / type | 조건 | CR·TF |
|---|---|---|---|---|
| 1 | `R8Unorm` | `R8` / `RED` / `UNSIGNED_BYTE` | core | CR·TF |
| 2 | `R8UnormSrgb` | `SR8_EXT` / `RED` / `UNSIGNED_BYTE` | `GL_EXT_texture_sRGB_R8` | TF(확장 사양 미대조) |
| 3 | `R8Uint` | `R8UI` / `RED_INTEGER` / `UNSIGNED_BYTE` | core | CR |
| 4 | `R8Sint` | `R8I` / `RED_INTEGER` / `BYTE` | core | CR |
| 5 | `A8Unorm` | `R8` + swizzle (0,0,0,R) | core | TF. swizzle 에뮬레이션은 샘플 전용이므로 `RenderTarget`·`Storage*` 능력을 주지 않는다 |
| 6 | `RG8Unorm` | `RG8` / `RG` / `UNSIGNED_BYTE` | core | CR·TF |
| 7 | `RG8UnormSrgb` | `SRG8_EXT` / `RG` / `UNSIGNED_BYTE` | `GL_EXT_texture_sRGB_RG8` | TF(미대조) |
| 8 | `RG8Snorm` | `RG8_SNORM` / `RG` / `BYTE` | core | TF, CR은 `GL_EXT_render_snorm` |
| 9 | `RG8Uint` | `RG8UI` / `RG_INTEGER` / `UNSIGNED_BYTE` | core | CR |
| 10 | `RGB8Unorm` | `RGB8` / `RGB` / `UNSIGNED_BYTE` | core | CR·TF |
| 11 | `RGB8UnormSrgb` | `SRGB8` / `RGB` / `UNSIGNED_BYTE` | core | TF |
| 12 | `RGBA8Unorm` | `RGBA8` / `RGBA` / `UNSIGNED_BYTE` | core | CR·TF, image |
| 13 | `RGBA8UnormSrgb` | `SRGB8_ALPHA8` / `RGBA` / `UNSIGNED_BYTE` | core | CR·TF |
| 14 | `RGBA8Snorm` | `RGBA8_SNORM` / `RGBA` / `BYTE` | core | TF, image, CR은 `GL_EXT_render_snorm` |
| 15 | `RGBA8Uint` | `RGBA8UI` / `RGBA_INTEGER` / `UNSIGNED_BYTE` | core | CR, image |
| 16 | `BGRA8Unorm` | `BGRA8_EXT` / `BGRA_EXT` / `UNSIGNED_BYTE` | `GL_EXT_texture_format_BGRA8888`(개정 1.4가 `BGRA8_EXT` 크기 내부 형식을 더함. 사양은 `TexStorage2D`를 언급하지 않으나 NVIDIA ES context에서 생성·왕복 확인, 개정 4) | CR·TF(확장 사양 ES 3.0 절) |
| 17 | `BGRA8UnormSrgb` | 없음 | — | UE는 `SRGB8_ALPHA8` + R·B swizzle(GL:1073)이나 렌더 타깃 쓰기 순서가 달라 따르지 않음 |
| 18 | `R16Unorm` | `R16_EXT` / `RED` / `UNSIGNED_SHORT` | `GL_EXT_texture_norm16` | CR·TF(확장 사양 Table 8.13, readback은 `RGBA`/`UNSIGNED_SHORT`) |
| 19·20 | `R16Uint`·`R16Sint` | `R16UI`·`R16I` / `RED_INTEGER` / `UNSIGNED_SHORT`·`SHORT` | core | CR |
| 21 | `R16Float` | `R16F` / `RED` / `HALF_FLOAT` | core | CR·TF |
| 22 | `RG16Unorm` | `RG16_EXT` / `RG` / `UNSIGNED_SHORT` | `GL_EXT_texture_norm16` | CR·TF |
| 23 | `RG16Snorm` | `RG16_SNORM_EXT` / `RG` / `SHORT` | `GL_EXT_texture_norm16` | TF, CR은 `GL_EXT_render_snorm`(readback `RGBA`/`SHORT`) |
| 24·25 | `RG16Uint`·`RG16Sint` | `RG16UI`·`RG16I` / `RG_INTEGER` | core | CR |
| 26 | `RG16Float` | `RG16F` / `RG` / `HALF_FLOAT` | core | CR·TF |
| 27 | `RGBA16Unorm` | `RGBA16_EXT` / `RGBA` / `UNSIGNED_SHORT` | `GL_EXT_texture_norm16` | CR·TF |
| 28 | `RGBA16Snorm` | `RGBA16_SNORM_EXT` / `RGBA` / `SHORT` | `GL_EXT_texture_norm16` | TF, CR은 `GL_EXT_render_snorm` |
| 29·30 | `RGBA16Uint`·`RGBA16Sint` | `RGBA16UI`·`RGBA16I` / `RGBA_INTEGER` | core | CR, image |
| 31 | `RGBA16Float` | `RGBA16F` / `RGBA` / `HALF_FLOAT` | core | CR·TF, image |
| 32·33 | `R32Uint`·`R32Sint` | `R32UI`·`R32I` / `RED_INTEGER` | core | CR, image·atomic |
| 34 | `R32Float` | `R32F` / `RED` / `FLOAT` | core | CR, image, TF는 `GL_OES_texture_float_linear`, 블렌드는 `GL_EXT_float_blend` |
| 35 | `RG32Uint` | `RG32UI` / `RG_INTEGER` / `UNSIGNED_INT` | core | CR |
| 36 | `RG32Float` | `RG32F` / `RG` / `FLOAT` | core | CR, TF는 `GL_OES_texture_float_linear`, 블렌드는 `GL_EXT_float_blend` |
| 37·38 | `RGB32Uint`·`RGB32Sint` | `RGB32UI`·`RGB32I` / `RGB_INTEGER` | core | 샘플만 |
| 39 | `RGB32Float` | `RGB32F` / `RGB` / `FLOAT` | core | 샘플만, TF는 `GL_OES_texture_float_linear`(대상 형식 미대조) |
| 40 | `RGBA32Uint` | `RGBA32UI` / `RGBA_INTEGER` / `UNSIGNED_INT` | core | CR, image |
| 41 | `RGBA32Float` | `RGBA32F` / `RGBA` / `FLOAT` | core | CR, image, TF는 `GL_OES_texture_float_linear`, 블렌드는 `GL_EXT_float_blend` |
| 42 | `R64Uint` | 없음 | — | — |
| 43 | `R5G6B5Unorm` | `RGB565` / `RGB` / `UNSIGNED_SHORT_5_6_5` | core | CR·TF |
| 44 | `B5G5R5A1Unorm` | 없음. ES에 `UNSIGNED_SHORT_1_5_5_5_REV`가 없어 비트 배치가 맞는 업로드 type이 없다 | — | UE 대응(GL:1033–1037)의 정확성 미확인 |
| 45 | `RGB10A2Unorm` | `RGB10_A2` / `RGBA` / `UNSIGNED_INT_2_10_10_10_REV` | core | CR·TF |
| 46 | `RG11B10Ufloat` | `R11F_G11F_B10F` / `RGB` / `UNSIGNED_INT_10F_11F_11F_REV` | core | CR·TF |
| 47 | `RGB9E5Ufloat` | `RGB9_E5` / `RGB` / `UNSIGNED_INT_5_9_9_9_REV` | core | TF(UE GL 대응 없음) |
| 64 | `D16Unorm` | `DEPTH_COMPONENT16` / `DEPTH_COMPONENT` / `UNSIGNED_SHORT` | core | 깊이 |
| 65 | `D24Unorm` | `DEPTH_COMPONENT24` / `DEPTH_COMPONENT` / `UNSIGNED_INT` | core | 깊이 |
| 66 | `D32Float` | `DEPTH_COMPONENT32F` / `DEPTH_COMPONENT` / `FLOAT` | core | 깊이 |
| 67 | `D24UnormS8Uint` | `DEPTH24_STENCIL8` / `DEPTH_STENCIL` / `UNSIGNED_INT_24_8` | core | 깊이·스텐실 |
| 68 | `D32FloatS8Uint` | `DEPTH32F_STENCIL8` / `DEPTH_STENCIL` / `FLOAT_32_UNSIGNED_INT_24_8_REV` | core | 깊이·스텐실 |
| 80·82·84 | BC1–3 | `COMPRESSED_RGBA_S3TC_DXT1/3/5_EXT` | `GL_EXT_texture_compression_s3tc` | TF |
| 81·83·85 | BC1–3 sRGB | `COMPRESSED_SRGB_ALPHA_S3TC_DXT1/3/5_EXT` | `GL_EXT_texture_compression_s3tc_srgb` | TF |
| 86·87 | `BC4Unorm`·`BC5Unorm` | `COMPRESSED_RED_RGTC1_EXT`·`COMPRESSED_RED_GREEN_RGTC2_EXT` | `GL_EXT_texture_compression_rgtc` | TF |
| 88 | `BC6HUfloat` | `COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_EXT` | `GL_EXT_texture_compression_bptc` | TF |
| 89·90 | BC7 | `COMPRESSED_RGBA_BPTC_UNORM_EXT`·`COMPRESSED_SRGB_ALPHA_BPTC_UNORM_EXT` | `GL_EXT_texture_compression_bptc` | TF |
| 96–99 | ETC2 | `COMPRESSED_RGB8_ETC2`·`COMPRESSED_SRGB8_ETC2`·`COMPRESSED_RGBA8_ETC2_EAC`·`COMPRESSED_SRGB8_ALPHA8_ETC2_EAC` | core(3D 불가) | TF |
| 100·101 | EAC | `COMPRESSED_R11_EAC`·`COMPRESSED_RG11_EAC` | core(3D 불가) | TF |
| 112–125 LDR | ASTC LDR | `COMPRESSED_RGBA_ASTC_nxn`·`COMPRESSED_SRGB8_ALPHA8_ASTC_nxn` | core(3D는 `GL_KHR_texture_compression_astc_sliced_3d` 또는 `_hdr`, 확장 사양 미대조) | TF |
| 114·117·120·123·126 | ASTC HDR | LDR과 같은 internal format, 내용이 HDR 블록 | `GL_KHR_texture_compression_astc_hdr` | TF |
| 160·161 | `NV12`·`P010` | 없음(외부 이미지 경로는 범위 밖) | — | — |

데스크톱 전용 조건: Windows 미리보기 context는 데스크톱 GL 4.x core이므로 BC4–BC7(RGTC·BPTC)이 core 기능으로 있고 S3TC는 `GL_EXT_texture_compression_s3tc`로 노출된다. 표의 조건 이름이 ES 확장 이름이므로 데스크톱 context가 같은 이름을 노출하지 않으면 켜지지 않는다. 데스크톱 context가 ES 확장 이름을 어떻게 노출하는지(예: `GL_ARB_texture_compression_bptc`만 있는 경우)는 PF2에서 확인한다(**미확인**). 미리보기에서 BC를 쓸지는 쿠킹 프로필(GLES 미리보기 프로필 형식 표)이 정한다.

UE와 다른 점: UE는 ES에서 `PF_R8G8B8A8_SNORM`을 `GL_RGBA8`로 바꿔 셰이더가 풀게 하고(GL:1060–1061, 버퍼 텍스처 때문), Android `PF_G16`을 `GL_R16F`로 대체한다(GL:1039–1041). Rocks는 둘 다 하지 않는다(8.3절 대체 금지).

### 8.7 Metal

Rocks MetalGraphics는 SH5에서 구현한다. 그때 UE 표(MTL:798–1046)를 출발점으로 대응을 정하고 다음을 확인한다(**미확인**).
- UE가 Invalid로 둔 형식(`RGB32*` MTL:1029–1033, `RGB9E5` MTL:1038, `R64Uint` MTL:1036, NV12 MTL:1024)의 Metal 대응 여부
- `A8Unorm`(UE는 `R8Unorm`, MTL:1006), `R8UnormSrgb`(Apple GPU만, `MetalTexture.cpp:222`), `RG8UnormSrgb`, ETC2 sRGB, `D24Unorm`(macOS만 `Depth24Unorm_Stencil8`, MTL:958–965)
- GPU family별 능력 표

## 9. sRGB 표현(결정 PX1)

**결정됨(2026-11-17): (가).** 아래는 결정 때 비교한 내용이다.

**(가) sRGB 변형을 별도 형식으로 둔다(지금 방식).** 형식 하나가 texel 해석을 모두 정한다. `RGBA8Unorm`과 `RGBA8UnormSrgb`는 저장 바이트가 같고 정보 표의 `srgbPair`로 서로를 가리킨다.

**(나) UE처럼 형식 + 생성 플래그로 둔다.** `TextureDesc`에 `srgb` 플래그를 두고 형식은 선형 형식 하나다. 플래그는 짝이 있는 형식에서만 허용한다.

| 기준 | (가) 별도 형식 | (나) 생성 플래그 |
|---|---|---|
| 쿠킹 데이터 | 형식 번호 하나로 해석이 확정된다. RenderBase 6.5절의 메타 "형식·sRGB·swizzle"은 "형식·swizzle"로 줄어든다 | 형식과 sRGB 표시를 함께 저장한다(6.5절 지금 문구 그대로). (R32Float, sRGB) 같은 무효 조합을 로드 때 검사해야 한다 |
| 능력 질의 | 형식별로 자연스럽다. R8 sRGB(D3D 없음, Vulkan 선택, Metal Apple GPU만, GLES 확장)처럼 sRGB 지원이 형식마다 다른 것을 그대로 표현한다 | `(형식, sRGB)` 두 인자 질의가 필요하다. UE는 지원하지 않는 sRGB 요청을 선형으로 조용히 되돌린다(VK:956–963, D3D `DXGI:100–119`) |
| view 형식 | 선형 view는 "짝 형식으로 본 view"다. 짝 관계가 정보 표에 있어 검사가 간단하다 | view의 "sRGB 끄기" 플래그(UE `bDisableSRGB`)로 표현한다 |
| D3D12 | 짝 형식은 typeless 자원 + view 형식(UE와 같은 구현) | 같음 |
| Vulkan | 짝 view를 허용한 텍스처만 `MUTABLE_FORMAT` + 형식 목록(UE `VulkanTexture.cpp:387–399`) | 같음 |
| OpenGL ES | 생성 때 sRGB internal format을 고른다. ES 3.2 core에 texture view가 없어 짝 view는 불가(능력 `SrgbPairView` false) | 같음(UE도 불가, `OpenGLUAV.cpp:122–124`) |
| Metal | `_sRGB` 형식으로 만들고 짝 view는 `pixelFormatView` 용도 | 같음 |
| 스왑체인·렌더 타깃 | 렌더 타깃 형식이 sRGB 여부를 담는다(`PipelineDesc::colorFormat` 하나로 충분) | 파이프라인의 타깃 형식에도 플래그가 필요하다(UE `RenderTargetFlags`, `Apple/MetalRHI/Private/MetalPipeline.cpp:133`) |
| UE 대응성 | 대응표가 UE (형식, 플래그) → Rocks 형식 하나로 결정적이다. 구조는 UE와 다르다 | UE와 같은 구조 |
| 다른 API와의 정합 | DXGI·Vulkan·Metal·GL 모두 sRGB를 별도 형식 enum으로 둔다 | Rocks 계층에서만 형식 + 플래그 |
| 형식 수 | 86개(sRGB 16개 포함) | 70개 |
| 기존 코드 | 지금 코드와 같다(`RGBA8UnormSrgb`) | `TextureDesc`·`PipelineDesc`에 플래그 추가 |

**권장: (가).** 근거는 셋이다.
- 무효 조합이 생기지 않는다. 쿠킹 데이터·desc·파이프라인 어디에도 "sRGB 플래그를 허용하지 않는 형식"을 검사할 자리가 필요 없다.
- sRGB 지원이 형식마다 달라 능력 질의를 형식 단위로 하는 편이 정확하다. UE가 하는 조용한 선형 되돌림은 Rocks의 대체 금지(8.3절)와 맞지 않는다.
- backend 구현(typeless·mutable·짝 view)은 두 안이 같으므로 UE 구조와의 차이는 desc 표면에만 있다.

(가)를 고르면 RenderBase 6.5절 메타와 AssetSystem 12.4.3절 표의 "SRGB" 열은 "쿠킹 형식이 sRGB 형식인지"로 읽는다(15절 개정안). (나)를 고르면 번호표에서 `*UnormSrgb` 16개를 빼고 6절 `srgb`·`srgbPair` 대신 "sRGB 플래그 허용"을 둔다.

짝 view 규칙((가) 기준): `TextureDesc`에 "짝 형식 view 허용"을 두고(GX2) 허용한 텍스처만 짝 형식 view를 만들 수 있다. 장치 전역 능력 `SrgbPairView`(GX1)가 false인 장치(OpenGL ES)에서 허용을 요청하면 `UnsupportedCapability`다.

## 10. 깊이·스텐실

- 형식은 5개 명시 형식이다(5.2절). UE처럼 "플랫폼이 고르는 깊이 형식"을 enum에 두지 않는다. 플랫폼마다 다른 형식이 같은 번호로 쿠킹 데이터·그래프 키에 들어가면 바이트 크기와 정밀도가 실행 장치에 따라 달라지기 때문이다.
- 기본 깊이 형식은 렌더러가 능력 질의로 고른다. 권장 우선순위는 `D32FloatS8Uint` → `D24UnormS8Uint`이다(UE D3D12·Vulkan 기본, `D3D12RHI.cpp:39–44`, VK:57–61, 841–844). 스텐실이 필요 없으면 `D32Float` → `D24Unorm`이다. 그림자 깊이 기본은 `D16Unorm`이다(UE `PF_ShadowDepth`가 모든 backend에서 D16, 4.7절). 이 선택은 RenderBase(GX2·렌더러)가 정하며 형식 표에는 넣지 않는다.
- 스텐실 읽기는 view의 plane 선택이다. view desc에 plane(`Depth`·`Stencil`)을 두고(GX2) 스텐실 plane view는 uint 텍스처로 읽힌다. 셰이더는 스텐실 값을 **G 채널**에서 읽는다(UE와 같다, 4.7절). backend 처리:
  - D3D12: `X24_TYPELESS_G8_UINT`·`X32_TYPELESS_G8X24_UINT` + plane 1
  - Vulkan: stencil aspect + mapping (R,R,0,1)
  - OpenGL ES: `DEPTH_STENCIL_TEXTURE_MODE = STENCIL_INDEX`(ES 3.2 core, Table 8.19) + swizzle (0,R,0,0). GL은 이 모드가 텍스처 객체 상태이므로 같은 텍스처를 깊이와 스텐실로 동시에 읽을 수 없다(UE도 같은 제약, `OpenGLUAV.cpp:127–139`)
  - Metal: `X24_Stencil8`·`X32_Stencil8` view(SH5)
- 깊이 readback·복사는 plane 단위다. 깊이 plane은 `D16`=2·`D24`=4(상위 8비트 미정의)·`D32F`=4바이트, 스텐실 plane은 1바이트다. 장치 내부 배치(D3D12 `R32G8X24`는 8바이트, UE BlockBytes 5 D12S:883, GL 8 GL:1013)는 공개하지 않는다.
- `D24Unorm`·`D24UnormS8Uint`는 Apple GPU Metal에 없다(UE MTL:809–828, 958–965). 능력 질의가 정한다.

## 11. 비디오 형식과 다중 plane

- `NV12`(8비트)·`P010`(10비트, 16비트 저장 상위 10비트)은 4:2:0 2-plane 형식이다. plane 0은 휘도(`R8Unorm`·`R16Unorm`, 원래 크기), plane 1은 색차 CbCr(`RG8Unorm`·`RG16Unorm`, 폭·높이 ½)이다(UE `PF.cpp:110–111, 137–138`, `DXGI:238–251`). 폭·높이는 2의 배수여야 한다(`DXGI:277–278, 315–316`).
- 정보 표의 `planes[2]`가 plane별 형식·크기 축소·texel 바이트를 준다. `PixelFormatImageBytes`는 두 plane의 합이다.
- 상태: 번호표·정보 표·크기 함수는 PF1, D3D12 생성·plane 업로드·plane view는 PF6에서 구현한다(결정 PX3, 2026-11-17 "생성까지 지금 구현"). GLES·D3D11·Vulkan·Metal은 그 backend의 대응 단계(PF4·PF5)에서 정하며 그 전에는 `UnsupportedCapability`다.
- backend 방향: D3D12는 DXGI `NV12`·`P010` + plane별 SRV(UE와 달리 SRV 생성), Vulkan은 multi-planar 형식 + plane aspect(`VK_IMAGE_ASPECT_PLANE_0/1_BIT`, sampler Y′CbCr 변환은 쓰지 않고 plane별 view), OpenGL ES·Metal은 그때 정한다(UE는 미지원).
- 포함 시기는 결정 PX3으로 정하였다(16절).

## 12. 저장 값과 번호 재배정

형식 번호는 쿠킹 텍스처 메타에 저장된다(RenderBase 6.5절 "직렬화": 고정 번호표와 안정 이름, enum 순서 값 금지, 번호 재사용 금지). 이번에 번호를 다시 매겨도 되는지 확인하였다.

- `Assets/`에는 `Assets/Branding`(SVG·PNG·HTML)만 있다. 쿠킹 텍스처·패키지·에셋 너겟 값이 없다(`git ls-tree origin/feature/engine-runtime Assets`).
- 시험 fixture에 형식 값을 담은 바이너리가 없다. 형식을 쓰는 시험은 enum을 코드로 쓴다(3절 사용처).
- 형식 값을 파일에 쓰는 코드는 `prototype/texture-importer` 브랜치(6a2ffc5 "[Prototype] 텍스처 임포터·AssetRuntime 시제품(참고 보존, 병합하지 않음)")의 `TextureAsset.h:133`(`u32 format(graphics::Format)`)뿐이다. 이 브랜치는 병합하지 않으며 커밋된 쿠킹 파일도 없다.

따라서 **이번에 번호를 다시 매긴다.** 기존 값 1(`RGBA8Unorm`)·2(`RGBA8UnormSrgb`)는 새 번호 12·13이 된다. PF1 이후로 번호표는 추가만 한다. 직렬화는 `PixelFormatNumber`(바이너리)·`PixelFormatName`(텍스트·JSON)이며 와이어 폭은 AssetSystem A5 형식이 정한다(정점 형식처럼 u32 권장).

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

## 14. 구현 단계

ID는 이 문서가 부여한다(PF). 형식 부분을 A0-G에서 떼어 렌더 쪽이 A0-G 앞에 한다(15절). 순서는 사용자가 정한 대로 정보 표 → D3D12·OpenGL ES 대응 → 능력 질의 → D3D11·Vulkan이다.

| ID | 내용 | 의존 | 완료 기준 | 시험 |
|---|---|---|---|---|
| PF1 | GraphicsTypes: `PixelFormat` 번호표 86개, `PixelFormatInfo` 표, `PixelFormatCaps` enum, 크기 함수, 이름·번호 직렬화, 13절 이름 변경 전부. backend는 RGBA8 두 형식만 대응하고 나머지는 `Unsupported` | PX1·PX2 결정 | 빌드·format·policy 통과, 시험 통과, 기존 self-test 결과 불변 | `GraphicsTypes/Tests/PixelFormatTests.cpp`: 유효 번호 전부의 이름·번호 왕복, 번호표 밖 거부, 정보 표 정합(압축 ↔ 블록 > 1, sRGB 짝 대칭·저장 바이트 같음, 채널 수와 마스크 일치, 깊이 형식의 비트), 블록 크기 계산 기준값(BC1 5×3 → 2×1 블록 16바이트, ASTC6x6 13×13 → 3×3 블록 144바이트, ASTC12x12 1×1 → 16바이트, NV12 4×2 → 8 + 4 = 12바이트), mip 사슬 바이트(BC1 8×8 4단 = 32 + 8 + 8 + 8), UE `PF.cpp` 블록 값과의 대조(대응 형식 전부, 4.3절 오기 제외) |
| PF2 | D3D12·OpenGL ES 대응 표 전부와 생성·업로드·readback. D3D12 typeless·view 형식·plane view, 행 pitch(256바이트)·블록 행 단위 업로드. GL은 ES 공통 API(`glTexStorage*`, 압축은 `glCompressedTexSubImage*`) | PF1 | 두 backend에서 지원 형식 전부가 생성·업로드·readback 왕복을 통과, 검증 계층 오류 0 | Sandbox self-test 형식 순회. 형식마다 결정적 패턴(번호로 시드한 값, 블록 형식은 미리 인코딩한 고정 블록과 그 해석 값)을 올리고 되읽는다. D3D12는 자원 복사 readback으로 바이트 일치. GL은 color-renderable 형식은 FBO + `glReadPixels` 바이트 일치, 그 밖(snorm·RGB9E5·압축·RGB32)은 셰이더로 샘플하여 `RGBA32Float` 타깃에 쓴 값을 허용 오차로 비교. 깊이·스텐실은 clear 값 readback(plane별) |
| PF3 | 능력 질의(GX1 형식 부분): `DeviceCaps` 형식 능력 배열, D3D12 `CheckFeatureSupport`, GL 조건 평가 + `glGetInternalformativ`, 생성 검증과 `UnsupportedCapability` | PF2, GX1 설계 | 능력 표가 실제 생성 결과와 일치 | 형식 × 용도(샘플·3D·cube·렌더 타깃·블렌드·MSAA 표본 수·깊이·storage) 전 조합에서 "능력 있음 ⇔ 생성 성공"을 단언. 능력 없음은 `Unsupported`(이후 `UnsupportedCapability`)와 `LastError` 문구, 크래시·검증 오류 없음. 결정적이며 시간 의존 없음 |
| PF4 | Vulkan(SH2의 VulkanGraphics, P9)·D3D11(SH5) 대응 표와 능력 질의 | PF3, 해당 backend | PF2·PF3 시험이 그 backend에서 통과 | 같은 self-test. Vulkan은 CI에서 NOT RUN일 수 있으며 그때는 로컬 결과를 기록 |
| PF5 | Metal(SH5) 대응 표·능력 | SH5 | PF2·PF3 시험 | 같은 self-test(macOS) |
| PF6 | 비디오 형식 생성: D3D12 `NV12`·`P010` 텍스처 생성, plane별 업로드, plane view(SRV) 샘플. 다른 backend는 PF4·PF5에서 | PF1, PF2 D3D12 부분 | D3D12에서 NV12·P010 생성·plane 업로드·plane view 샘플 통과, 다른 backend는 `UnsupportedCapability` | plane별 결정적 패턴 왕복(plane 0 휘도, plane 1 색차), 홀수 크기 거부 |

시험 규칙: 형식 순회 시험은 장치가 지원하지 않는 형식을 "건너뜀"으로 기록하되, 그 판정은 PF3 이후 능력 질의 결과로만 한다(PF2에서는 backend 표의 "장치 형식 없음"만 건너뜀). 시간·경쟁에 기대지 않는다.

### 14.1 구현 상태(개정 3)

| ID | 상태 | 위치 |
|---|---|---|
| PF1 | **구현** | `GraphicsTypes/PixelFormat.h`·`PixelFormat.cpp`, 시험 `GraphicsTypes/Tests/PixelFormatTests.cpp` |
| PF2 | D3D12 **구현**. GLES·D3D11은 각 backend 대응 단계 | `D3D12Graphics/Private/Win64/D3D12PixelFormats.{h,cpp}`, 생성·업로드·readback·view는 `D3D12Device.cpp` |
| PF3 | 틀 **구현**. D3D12는 질의, D3D11·OpenGL은 대응 중인 RGBA8 두 형식만 정적 공개 | `Device::GetPixelFormatCaps`, `RequiredPixelFormatCaps`(`Graphics/DeviceTypes.h`) |
| PF6 | D3D12 **구현** | NV12·P010 생성·plane 업로드·plane readback·Y/UV view |

검증(2026-11-17, Sandbox `--self-test`, 검증 계층 켬): 형식 86개 × 용도에서 "능력 있음 ⇔ 생성 성공" 실패 0, 업로드·readback·`UpdateTexture` 왕복 실패 0, view 샘플 실패 0, 검증 계층 오류 0.

| 장치 | Sampled | RenderTarget | DepthStencil | 능력 없음 |
|---|---|---|---|---|
| D3D12 GTX 1060 | 60 | 38 | 5 | 26 |
| D3D12 WARP | 60 | 42 | 5 | 26 |
| D3D11·D3D11 WARP·OpenGL | 2 | 2 | 0 | 84 |

D3D12의 능력 없음 26개는 DXGI 형식이 없는 ETC2·EAC 6, ASTC 15, `R8UnormSrgb`·`RG8UnormSrgb`·`RGB8Unorm`·`RGB8UnormSrgb`·`R64Uint`다(8.4절).

**스펙과 다르게 정한 것(개정 3).**
- 깊이·스텐실 형식의 이미지 크기를 plane 합으로 정의하였다(PF1에서는 0). D3D12는 깊이 자원에도 CPU 데이터를 복사할 수 있고, plane별 크기를 합하면 비디오 형식과 같은 규칙이 된다. 초기 데이터 배치는 mip 0부터, mip 안은 plane 0 다음 plane 1이다. `PixelFormatPlaneCount`·`PixelFormatPlaneRowBytes`·`PixelFormatPlaneRows`를 더하였고, `PixelFormatRowBytes`는 단일 plane 형식만 값을 준다. 깊이 전용 형식의 `blockBytes`는 깊이 texel 바이트(D16 2·D24 4·D32F 4)다.
- Graphics 계약을 GX2 전에 일부 열었다.
  - `TextureUsage::DepthStencil`: 생성·DSV까지만 한다. 깊이 그리기 경로(attachment·깊이 상태)는 GX2·GX3이다. 깊이 형식은 용도와 상관없이 `ALLOW_DEPTH_STENCIL`로 만들어 Sampled 깊이 텍스처에도 CPU 데이터를 복사한다. 그래서 D3D12의 깊이 형식 Sampled 능력은 DSV 지원을 함께 요구한다.
  - view는 별도 handle 타입 대신 원 자원을 공유하는 **`TextureHandle`이다**(`CreateTextureView`, `TextureViewDesc{format, plane}`). 기존 `SetTexture`로 묶고 `Retire`로 놓으며, 원 텍스처를 먼저 놓아도 view가 자원 참조를 쥔다. view에는 업로드·readback·그리기 대상 지정을 하지 않는다. 형식·plane 규칙은 장치 무관 함수 `ResolveTextureViewFormat`이다.
  - `ReadTexture(handle, mip, plane, out)`: 렌더 타깃이 아닌 텍스처·블록 압축·깊이 plane·비디오 plane을 읽는 동기 시험 경로다(`ReadPixels`와 같은 readback 경로).
  - 새 virtual(`GetPixelFormatCaps`·`CreateTextureView`·`ReadTexture`)은 기본 구현이 각각 `None`·`Unsupported`·`Unsupported`다. 다른 backend 브랜치가 구현 없이도 빌드된다.
- 텍스처·파이프라인 생성은 형식 능력으로 판정한다. 파이프라인 색 형식은 `RenderTarget`(알파 블렌드면 `Blendable`도)이 필요하다. 능력이 없으면 `Unsupported`이고 `LastError`에 형식 이름을 기록한다(D3D12·OpenGL).
- `Multisample` 능력과 표본 수 질의(`GetPixelFormatSampleCounts`)는 GX2로 미뤘다(개정 5에서 구현, 14.3절). D3D12의 `Texture3D`·`TextureCube`·`Storage*`·`TexelBuffer*`는 질의해 공개하지만 그 차원·view API가 아직 없어 "능력 ⇔ 생성" 시험 범위 밖이다.
- `D24Unorm`은 DXGI `R24G8_TYPELESS` 자원의 깊이 plane만 쓴다. 장치 자원의 plane이 Rocks plane보다 많으면 올리지 않은 plane도 생성 직후 쉬는 상태(PIXEL_SHADER_RESOURCE)로 옮긴다(`FormatPlaneCount`, `D3D12_FEATURE_FORMAT_INFO`).
- 비디오 형식 plane은 SRV만 생성한다(plane RTV·UAV 없음). 비디오 형식의 Sampled 능력은 plane SRV 형식이 아니라 자원 형식 자체의 셰이더 지원으로 판정한다.
- 자원 상태(RG1 병합 뒤): D3D12 텍스처 상태는 mip별 committed·current로 추적한다(RenderBase GX5). 다중 plane 자원은 mip 하나의 상태를 그 mip의 모든 plane subresource가 함께 따르고, mip별 전이는 plane마다 barrier를 낸다(`TextureRecord::devicePlanes`). view는 상태를 따로 갖지 않고 원 텍스처 기록이 추적한다(`SetTexture`·`Transition`이 원 기록으로 향한다). 원 텍스처를 view보다 먼저 `Retire`하면 기록을 남겨 두었다가 마지막 view를 놓을 때 함께 놓는다. 업로드와 readback(`ReadTexture`)은 상태 중립이다: 그 mip의 committed 상태에서 복사 상태로 바꿨다가 되돌린다. 렌더 그래프의 텍스처 생성은 지금처럼 블록 압축을 거부하고 깊이 형식은 GX4에서 연다.

### 14.2 PF4 구현 상태(개정 4)

| backend | 상태 | 위치 |
|---|---|---|
| D3D11 | **구현** | `D3D11Graphics/Private/Win64/D3D11PixelFormats.{h,cpp}`(대응 표·능력 질의·D3D11 메모리 배치 변환), 생성·view·readback은 `D3D11Resources.cpp`·`D3D11Upload.cpp` |
| OpenGL ES | **구현** | `OpenGLGraphics/Private/Common/GlesFormats.{h,cpp}`(대응 표·확장 조건·능력·readback 변환), 생성·view는 `GlesDevice.cpp`, 업로드·`ReadTexture`는 `GlesDeviceTransfer.cpp`, 묶기는 `GlesDeviceFrame.cpp` |
| Vulkan | **구현** | `VulkanGraphics/Private/VulkanFormats.{h,cpp}`(대응 표·plane aspect·view 형식), 능력 질의·생성·view는 `VulkanResources.cpp`, 업로드·readback은 `VulkanUpload.cpp`, 기능 활성은 `VulkanDevice.cpp` |

**D3D11(2026-11-18).**
- 대응 표는 D3D12와 같은 DXGI 표다(8.4절). D3D12 표와 같은 행을 D3D11 모듈에 두었고, D3D 공용 도우미 모듈로 합치는 것은 후속이다(`D3D11Common.h` 주석의 기존 결정과 같다). `RGB9E5Ufloat`도 대응한다.
- 능력은 `CheckFormatSupport`와 `CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2)`로 D3D12와 같은 규칙으로 채운다(7절).
- 색·블록 압축 형식은 초기 데이터와 `UpdateSubresource`로 올린다. 깊이·스텐실과 비디오는 D3D11 메모리 배치가 Rocks 배치와 다르므로 CPU에서 바꾼 staging 텍스처를 `CopySubresourceRegion`으로 복사한다(`UpdateSubresource`는 깊이·스텐실 자원에 쓸 수 없다).
  - D24S8: texel 하나에 깊이 24비트와 스텐실 8비트(상위 byte)
  - D32FS8: texel 8바이트(float 깊이, 스텐실 byte, 빈 3바이트)
  - NV12·P010: plane 0 행들 뒤에 같은 행 간격의 plane 1 행들(두 plane의 행 바이트가 같아 Rocks 배치 그대로다)
- `ReadTexture`는 mip 하나를 staging 텍스처로 복사해 map한 뒤 plane을 꺼낸다(같은 배치 변환의 역).
- `CreateTextureView`: 깊이·스텐실은 view 형식(`R24_UNORM_X8_TYPELESS`·`X24_TYPELESS_G8_UINT` 등)으로, 비디오는 `CreateShaderResourceView1`의 `PlaneSlice`로 plane을 고른다. view는 원 텍스처 COM 참조를 쥐므로 원 텍스처를 먼저 놓아도 산다. D3D11에는 장치 상태 추적이 없어 D3D12의 보류 Retire가 필요 없다.
- NV12·P010은 D3D11에서도 생성·plane 업로드·readback·Y/UV view가 된다(UE D3D11은 SRV를 만들지 않는다, 4.8절).
- 검증: Sandbox `--self-test`에서 D3D11·D3D11 WARP 모두 Sampled 60·RenderTarget 38/42·DepthStencil 5·능력 없음 26, "능력 ⇔ 생성"·바이트 왕복·view 샘플 실패 0, 검증 계층 오류 0.

**Vulkan(2026-11-18).**
- 대응 표는 8.5절과 같다. 형식마다 `TextureFormatInfo`(이미지 형식·aspect·성분 대응·비디오 plane 형식·MUTABLE 필요 여부) 한 행이다. 주의점 반영: `BC1Unorm`은 `BC1_RGBA_UNORM_BLOCK`, `RG8Snorm`은 `R8G8_SNORM`, `A8Unorm`은 `R8_UNORM` + (0,0,0,R), `RGB9E5Ufloat`는 `E5B9G9R9_UFLOAT_PACK32`.
- 능력 질의(7절 Vulkan 열): optimal tiling 기능 bit에 더해, `CreateTexture`와 같은 usage·flags로 `vkGetPhysicalDeviceImageFormatProperties`가 성공해야 `Sampled`·`RenderTarget`을 준다(기능 bit만으로는 생성이 보장되지 않는다). `Mipmaps`는 `maxMipLevels > 1`, `Texture3D`·`TextureCube`는 그 차원(cube는 `CUBE_COMPATIBLE`, 층 6 이상) 질의 성공, `Gather`는 `shaderImageGatherExtended`(UE와 같다), storage·texel buffer는 기능 bit를 옮긴다. `Multisample`은 D3D와 같이 GX2로 미룬다. 성분 대응이 항등이 아닌 형식(`A8Unorm`)은 렌더 타깃·storage·texel buffer 능력을 주지 않는다.
- 장치 기능: `textureCompressionBC`·`ETC2`·`ASTC_LDR`는 지원하면 켠다. ASTC HDR(`*_SFLOAT_BLOCK`)은 `VK_EXT_texture_compression_astc_hdr` 확장과 그 기능을 켠 장치에서만 질의·공개한다(인스턴스 API 1.1이라 1.3 core 기능은 쓰지 않는다. 확장 없이 질의하면 검증 계층 `VUID-VkImageCreateInfo-format-parameter`). 형식 능력은 켠 기능에 따라 달라지므로 논리 장치를 만든 뒤 질의한다.
- 깊이·스텐실: 이미지 usage에 늘 `DEPTH_STENCIL_ATTACHMENT`를 넣고, barrier는 이미지 전체 aspect(DEPTH|STENCIL)로, 복사는 plane마다 aspect 하나(D24 4바이트·D32F 4바이트·스텐실 1바이트, Rocks plane 배치와 같다)로 한다. 기본 view는 깊이 aspect, 스텐실 view는 stencil aspect + mapping (R,R,0,1)로 값을 G에서 읽는다(10절).
- 비디오: `G8_B8R8_2PLANE_420_UNORM`·`G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16` multi-planar 이미지(`MUTABLE_FORMAT`)에 plane view(`R8`·`R8G8`, `R10X6`·`R10X6G10X6`)를 만든다. Y′CbCr 변환 객체는 만들지 않지만 multi-planar 이미지 생성 조건인 `samplerYcbcrConversion` 기능(Vulkan 1.1 core 기능, 선택)을 켜야 하므로, 그 기능이 없는 장치에서는 NV12·P010이 능력 없음이다. P010의 하위 6비트는 Vulkan 형식이 정의하지 않으므로 self-test는 그 비트를 0으로 둔 데이터로 왕복을 확인한다.
- 업로드: 원본(mip마다 plane을 잇는 배치)을 (mip, plane) 구역마다 복사 원본 정렬(texel 크기와 4의 최소공배수, 깊이·스텐실은 4의 배수 규칙)에 맞춰 ring에 옮긴다. `UploadTextureFromStaging`은 정렬이 맞는 단일 plane만 staging에서 바로 복사하고 나머지는 ring을 거친다.
- `ReadTexture`: mip 하나의 plane 하나를 host 버퍼로 복사하고 fence를 기다린다. 업로드 기록에 쌓으므로 앞선 업로드 뒤에 실행되고 상태 중립이다(제출한 상태로 되돌린다, GX5).
- `CreateTextureView`: sRGB 짝(같은 이미지, MUTABLE), 깊이·스텐실 plane, 비디오 plane view를 만든다. view 기록은 이미지·메모리를 쥐지 않고 상태 추적은 원 기록이 한다(전이·바인딩이 원 기록으로 간다). 원 텍스처를 먼저 Retire하면 원 기록은 남아 있다가 마지막 view를 놓을 때 함께 놓인다(D3D12와 같은 보류 Retire).
- 렌더 파이프라인: `alphaBlend`이면 색 형식에 `Blendable`도 요구한다(D3D와 같다).
- 검증: Sandbox `--self-test`에서 NVIDIA GTX 1060은 Sampled 58·RenderTarget 37·DepthStencil 5·능력 없음 24, lavapipe(llvmpipe)는 Sampled 60·RenderTarget 40·DepthStencil 5·능력 없음 23, 둘 다 "능력 ⇔ 생성"·바이트 왕복·view 샘플 실패 0, 검증 계층 오류 0(의도한 형식 불일치 탐지 시험 제외). D3D12와 수가 다른 것은 장치 차이다(예: GTX 1060은 `RGB32*`를 optimal tiling으로 샘플하지 못하고 `R8UnormSrgb`는 샘플한다). "능력 없음"은 능력 bit가 하나도 없는 형식 수다(샘플할 수 없어도 storage·texel buffer bit가 있는 형식은 들지 않는다).

**OpenGL ES(2026-11-18).**
- 대응 표는 8.6절이다. 조건은 ES 확장 이름으로 판정한다. Windows 미리보기의 데스크톱 core context(GLES31_PC)도 같은 이름을 공개할 때만 켠다. 그래서 데스크톱에서 core 기능인 norm16·BPTC·float 필터·float 블렌드도 그 context에서는 꺼진다. ASTC LDR은 ES context에서는 core로 보고, 데스크톱 core context에서는 `GL_KHR_texture_compression_astc_ldr`를 요구한다.
- 대응하지 않는 형식(Unsupported): `BGRA8UnormSrgb`(ES에 BGRA sRGB 내부 형식 없음), `R64Uint`, `B5G5R5A1Unorm`(비트 배치가 맞는 업로드 타입 `UNSIGNED_SHORT_1_5_5_5_REV`가 ES에 없음), `NV12`·`P010`(ES core에 다중 plane 텍스처 없음). 확장이 없는 형식도 능력 없음이다.
- 능력(7절 OpenGL ES 열):
  - 모든 대응 형식에 Sampled·Mipmaps·TextureCube·Gather를 준다.
  - Texture3D는 비압축 색 형식만 준다. 압축 형식의 3D 조건은 확장 사양을 대조하지 않았다.
  - Filterable은 Table 8.10 TF(32F는 `GL_OES_texture_float_linear`)를 따른다.
  - RenderTarget은 CR(SNORM은 `GL_EXT_render_snorm`, norm16 UNORM은 `GL_EXT_texture_norm16`)을 따른다. Blendable은 렌더 타깃이면서 정수가 아닐 때 준다. 32F는 `GL_EXT_float_blend`가 있어야 한다(없이 블렌드하면 `INVALID_OPERATION`, 확장 사양).
  - Storage는 Table 8.26, atomic은 R32UI·R32I다.
  - texel buffer(Table 8.18)는 개정 4에서 대조하지 않아 공개하지 않았다(개정 5에서 대조·공개, 14.3절).
  - 깊이는 Filterable이 아니다. 비교 모드 없이 선형 필터를 걸면 불완전 텍스처가 된다.
- 선형 필터를 받지 않는 형식(정수·깊이·필터 확장 없는 32F)의 샘플러는 최근접 필터로 만든다. ES에서는 그 조합이 불완전 텍스처가 되어 `texelFetch`도 0을 읽는다. 이는 Filterable 능력이 없다고 공개한 형식의 규칙이다.
- `A8Unorm`은 R8에 텍스처 swizzle (0,0,0,R)을 건다. 렌더 타깃·storage 능력은 주지 않고, readback은 저장소 R8을 읽는다.
- 업로드: 블록 압축은 `glCompressedTexSubImage2D`로 올린다. D24는 하위 24비트를 32-bit 정규화 정수로 넓혀 `DEPTH_COMPONENT`/`UNSIGNED_INT`로 올린다. 깊이·스텐실은 두 plane을 엮어 올린다(`UNSIGNED_INT_24_8`, `FLOAT_32_UNSIGNED_INT_24_8_REV`).
- `ReadTexture`(pixel pack buffer):
  - 색 렌더 타깃 형식은 mip을 임시 framebuffer에 붙여 읽는다. 구현 선택 조합(`IMPLEMENTATION_COLOR_READ_FORMAT/TYPE`)이 업로드 배치와 같으면 그대로 읽는다.
  - 다르면 계열 표준 조합으로 읽고 CPU에서 정확히 되돌린다(ES 3.2 16.1.2절과 `EXT_texture_norm16`·`EXT_render_snorm`의 추가 조합). 표준 조합은 `RGBA`/`UNSIGNED_BYTE`·`BYTE`·`UNSIGNED_SHORT`·`SHORT`·`FLOAT`, `RGBA_INTEGER`/`UNSIGNED_INT`·`INT`, `RGB10_A2`다. 되돌리는 변환은 채널 고르기, half·11/10-bit float 묶기, BGRA 순서, 5_6_5 묶기다.
  - 깊이·스텐실 plane은 ES가 `glReadPixels`로 읽지 못한다. 그래서 셰이더가 `texelFetch`로 R32F(깊이)·R32UI(스텐실, `DEPTH_STENCIL_TEXTURE_MODE` = `STENCIL_INDEX`) 대상에 옮겨 읽는다. D16·D24는 정규화 값을 저장 정수로 반올림해 되돌린다.
  - 블록 압축과 색 렌더 타깃이 아닌 형식은 Unsupported다. 블록 압축은 ES에 `glGetCompressedTexImage`가 없고, 색 렌더 타깃이 아닌 형식(`SRGB8`·`RGB9_E5`·`RGB32*`·확장 sRGB R8/RG8, SNORM은 `EXT_render_snorm`이 없을 때)은 framebuffer에 붙일 수 없다.
  - `ReadPixels`·`RequestReadback`도 같은 읽기 조합을 쓴다. 비동기 readback은 `TryReadback`에서 변환한다.
- `CreateTextureView`: ES core에는 형식을 재해석하는 view가 없다.
  - 같은 텍스처 객체를 공유하는 view만 만든다: 같은 형식 view와 깊이·스텐실 plane view다.
  - 읽을 plane(`DEPTH_STENCIL_TEXTURE_MODE`)과 스텐실 G 채널 대응(swizzle (R,R,0,1))은 텍스처 객체 상태다. 그래서 `SetTexture`로 묶을 때마다 정한다. 한 그리기에 텍스처 하나만 묶으므로 서로 덮지 않는다.
  - sRGB 짝 view는 Unsupported다. `GL_EXT_texture_sRGB_decode`로 sRGB 원본을 선형으로 읽는 한 방향은 가능하지만 후속으로 둔다.
  - view는 자기 샘플러만 놓는다. 원 텍스처의 Retire는 마지막 view까지 보류한다(D3D12·Vulkan과 같다).
- 렌더 파이프라인: `alphaBlend`이면 색 형식에 `Blendable`도 요구한다.
- self-test 보강:
  - 정수 형식 파이프라인에 GLSL `ivec4`·`uvec4` 출력을 쓰고, view 시험에도 GLSL판을 쓴다. 그래서 OpenGL ES 예외 없이 "능력 ⇔ 파이프라인 생성"을 본다.
  - OpenGL ES에서만 `ReadTexture` Unsupported를 허용하고, 읽지 못한 형식 수를 요약에 적는다. sRGB 짝 view의 Unsupported도 OpenGL ES에서만 허용한다.
  - 왕복 데이터는 모든 backend에서 float의 NaN·무한대·비정규 수와 SNORM 최솟값(-128·-32768)을 피한다. 이 값들은 framebuffer 읽기 조합을 거치면 보존되지 않는다.
- 검증(Sandbox `--self-test`, NVIDIA GTX 1060):
  - ES 3.2 context: Sampled 77·RenderTarget 36·DepthStencil 5·능력 없음 9, 읽지 못한 형식 35(블록 압축 29, `R8UnormSrgb`·`RGB8UnormSrgb`·`RGB32*` 3종·`RGB9E5Ufloat`).
  - 데스크톱 core context: Sampled 53·RenderTarget 28·DepthStencil 5·능력 없음 33, 읽지 못한 형식 19(블록 압축 11, `EXT_render_snorm` 없는 SNORM 2종, `R8UnormSrgb`·`RGB8UnormSrgb`·`RGB32*` 3종·`RGB9E5Ufloat`).
  - 둘 다 "능력 ⇔ 생성"·바이트 왕복(깊이·스텐실 포함)·깊이/스텐실 view 샘플 실패 0, GL debug 오류 0이다.

### 14.3 GX1·GX2 연동(개정 5)

**MSAA 능력(2026-11-18, RenderBase GX1).** 14.1절에서 GX2로 미룬 `Multisample` 능력과 표본 수 질의를 넣었다. 공개 API는 8.2절 그대로 `Device::GetPixelFormatSampleCounts(format)`(비트 n = 2^n 표본, `SampleCountBit`)이다.
- 렌더 타깃·깊이 능력이 있는 형식만 값이 있고 1표본 비트를 늘 넣는다. 그 밖의 형식은 0이다. 2표본 이상이 하나라도 있으면 `Multisample`을 켠다.
- D3D12·D3D11: RTV(깊이는 DSV) 형식이 `MULTISAMPLE_RENDERTARGET`을 지원할 때 2–32표본을 `D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS`·`CheckMultisampleQualityLevels`로 묻는다(품질 수준 1 이상).
- Vulkan: 렌더 타깃·깊이와 같은 usage·flags의 `vkGetPhysicalDeviceImageFormatProperties` `sampleCounts`다(VkSampleCountFlags 비트가 Rocks 비트와 같다, 장치 framebuffer·sampled 표본 한도가 반영된다).
- OpenGL ES: `glGetInternalformativ(GL_TEXTURE_2D_MULTISAMPLE, …, GL_SAMPLES)`(ES 3.1 core)다. 데스크톱 core context는 데스크톱 값(GTX 1060 32표본까지)을 그대로 낸다.
- 측정(RGBA8Unorm·D32Float): D3D12·D3D11·Vulkan·GLES ES context GTX 1060 1/2/4/8, WARP 1/2/4/8/16, lavapipe 1/4/8, GL core context 1/2/4/8/16/32. `Multisample` 형식 수는 D3D12·D3D11 43, WARP 47, Vulkan 42, lavapipe 45, GLES 41(core context 33).
- MSAA 텍스처 생성(`TextureDesc` 표본 수)은 텍스처 쪽 GX2 단계다. 그때 "능력 ⇔ 생성" 시험을 표본 수 축으로 넓힌다.

**texel buffer 능력(RQ9).** OpenGL ES의 `TexelBufferRead`는 ES 3.2 버퍼 텍스처 내부 형식 표(Table 8.18, Khronos glTexBuffer ES3 refpage로 대조)에 있는 형식이다: R8·R8I·R8UI·RG8·RG8UI·RGBA8·RGBA8UI·R16I·R16UI·R16F·RG16I·RG16UI·RG16F·RGBA16I·RGBA16UI·RGBA16F·R32I·R32UI·R32F·RG32UI·RG32F·RGB32I·RGB32UI·RGB32F·RGBA32UI·RGBA32F(Rocks 형식이 있는 것). 표의 R16·RG16·RGBA16 UNORM은 ES core 내부 형식이 아니고 `GL_EXT_texture_norm16`의 버퍼 텍스처 조건을 대조하지 않아 공개하지 않는다. `TexelBufferWrite`는 그 가운데 image 형식(Table 8.26), atomic은 R32UI·R32I다. Renderer가 고른 `R32Uint`·`RGBA32Float`는 읽기·쓰기 모두 된다(RQ9 해결).
- Vulkan: 버퍼 view 형식은 버퍼의 모든 texel usage(읽기 용도면 UNIFORM, 쓰기 용도면 STORAGE)를 지원해야 하므로(VUID-VkBufferViewCreateInfo-format-08778) 쓰기·atomic 능력은 읽기도 되는 형식만 공개하고, 읽기·쓰기 버퍼의 view는 두 능력을 모두 요구한다(아니면 `Unsupported`).
- D3D11: WARP가 `RGB9E5Ufloat`에 `SUPPORT_BUFFER`를 알리고도 버퍼 SRV 생성 안에서 멈추므로 D3D11은 그 형식의 texel buffer 능력을 공개하지 않는다.
- 측정(typed view 생성 ⇔ 능력, 47개 비압축 단일 plane 색 형식): D3D12·D3D11 GTX 1060 읽기 36·쓰기 33, D3D12 WARP 39·36, D3D11 WARP 38·35, Vulkan GTX 1060 42·33, lavapipe 44·34, GLES(두 context) 26·10(성분 대응으로 흉내 내는 A8Unorm은 버퍼 텍스처에 대응을 걸 수 없어 GX1·GX2 리뷰 반영에서 뺐다). 모두 "능력 ⇔ 생성" 실패 0이다.

## 15. A0-G와의 분담

형식 부분(번호표·정보 표·backend 형식 대응·형식 능력 질의)은 렌더 쪽이 PF1–PF3으로 지금 한다. A0-G에는 차원·view swizzle·샘플러가 남는다. A0-G의 backend 형식 표는 PF2·PF3이 만든 표를 쓴다.

**AssetSystem 1절 A0-G 행 개정안**(실제 수정은 이 문서 확정 뒤 에셋 쪽이 한다).

현재:
> GraphicsTypes·Graphics: 차원(2D·Cube·3D·2DArray·CubeArray)·형식(R8·RG8·RGBA8·R16·RG16·RGBA16·R16F·RG16F·RGBA16F·R32F·RG32F·RGBA32F·BGRA8, sRGB 변형)·view swizzle(…)·샘플러 W축 주소·필터 5종(…)·**깊이 비교 샘플러**(…), 쿠킹 데이터 샘플러 D3D12·OpenGL. 착수 전 반영할 셰이더·렌더 요구: …

개정:
> GraphicsTypes·Graphics: 차원(2D·Cube·3D·2DArray·CubeArray)·view swizzle(D3D12 `Shader4ComponentMapping`, OpenGL 채널별 `TEXTURE_SWIZZLE_R/G/B/A` — `TEXTURE_SWIZZLE_RGBA`는 OpenGL ES에 없다)·샘플러 W축 주소·필터 5종(Point·Bilinear·Trilinear·AnisotropicPoint·AnisotropicLinear — 기존 `Nearest`·`Anisotropic`의 이름 변경 포함)·**깊이 비교 샘플러**(비교 함수, 그림자용 — [렌더 기반](RenderBase/README.ko.md) SX13), 쿠킹 데이터 샘플러 D3D12·OpenGL. **픽셀 형식은 [픽셀 형식](PixelFormat.ko.md) PF1–PF3(렌더 쪽, A0-G 전 완료)을 쓴다**: 번호표·정보 표·D3D12·OpenGL ES 형식 대응·형식 능력 질의. A0-G는 차원별 형식 능력(`Texture3D`·`TextureCube`)을 생성 검증에 쓰고, 차원 × 형식 조합의 self-test를 PF2 형식 순회에 차원 축으로 더한다. 착수 전 반영할 셰이더·렌더 요구: [셰이더 시스템](ShaderSystem/05-Prerequisites.ko.md) 5.2절 G1(backend 중립 정의, Vulkan·Metal·GLES로 옮길 수 있을 것, 지원 표와 `UnsupportedCapability` — 형식 지원 표는 픽셀 형식 8절)·G2(OpenGL 구현은 OpenGL ES 3.2 공통 API, 데스크톱 전용 DSA를 늘리지 않음, ES 확장 의존 형식은 픽셀 형식 8.6절 조건표, 필터 확장 `GL_EXT_texture_filter_anisotropic`)·G3(VulkanGraphics가 A0-G 범위를 받음, SH2), 텍스처 desc의 샘플러는 기본값이고 바인딩 때 다른 샘플러를 지정할 수 있게 둔다(렌더 기반 결정 R12, 바인딩 쪽은 GX2)

완료 기준 열은 "self-test 확장"에서 "self-test 확장(차원 × swizzle × 샘플러, 형식 축은 PF2 순회 재사용)"으로 개정한다.

**함께 고칠 다른 문서.**

| 문서 | 위치 | 개정 |
|---|---|---|
| AssetSystem | 12.4.3절 표의 형식 이름 | Rocks 이름으로 읽는다(R8 → `R8Unorm`, RGBA16F → `RGBA16Float`, R32F → `R32Float` 등). PX1 (가)이면 "SRGB" 열은 sRGB 형식 선택을 뜻한다 |
| 〃 | 12.6절 | "렌더러는 비압축만" → 압축 형식의 정의·backend 대응·능력은 픽셀 형식 문서에 있고 인코더·프로필 선택은 압축 단계 |
| RenderBase | 4.4절 GX1·GX2, 6.5절 메타·후속, 4.1절·11.2절 | 이 문서를 가리키게 한다(이번 커밋에서 교차 참조를 넣었다). PX1 확정 뒤 6.5절 메타 문구 확정 |
| ShaderSystem | 5.2절 G1·G2, 3절 현황 표 `:93` | G1 지원 표·G2 확장 목록을 이 문서 8절로 연결 |
| EngineRuntime | `:35` | GraphicsTypes 설명에 `PixelFormat`·정보 표·능력 플래그 |

## 16. 결정 필요 사항

| ID | 결정 | 선택지와 권장 | 기한 |
|---|---|---|---|
| PX1 | sRGB 표현 | **결정됨(2026-11-17)**: (가) 별도 형식. 9절 | — |
| PX2 | 번호 체계 | **결정됨(2026-11-17)**: 계열별 구간(5.2절, `uint8`) | — |
| PX3 | 비디오 형식 포함 시기 | **결정됨(2026-11-17)**: 번호·정보·크기 함수는 PF1, D3D12 생성은 PF6에서 지금 구현(권장안이던 "생성은 미디어 단계까지 계약"이 아님) | — |

## 17. 미확인 목록

| 항목 | 확인 시점 |
|---|---|
| ES 확장(`EXT_texture_norm16`·`EXT_texture_sRGB_R8/RG8`·`EXT_texture_format_BGRA8888`·`OES_texture_float_linear`의 대상 형식·`KHR_texture_compression_astc_sliced_3d`)의 렌더 가능·필터 가능·불변 저장 조합 세부 | PF2 착수 때 확장 사양서 대조, PF3 시험으로 검증 |
| `glGetInternalformativ`·버퍼 텍스처(Table 8.18)·image atomic 대상 형식의 사양 절 | PF3 |
| 데스크톱 GL context가 ES 확장 이름(s3tc·rgtc·bptc)을 노출하는 방식 | PF2 |
| UE ES의 `PF_B5G5R5A1_UNORM` 대응(5_5_5_1 + swizzle) 비트 배치 정확성 | Rocks는 미지원으로 두므로 필요 시 |
| Vulkan 필수 형식 표와의 대조, `R64Uint`에 필요한 장치 기능 | SH2(PF4) |
| Metal 대응 전반(8.7절) | SH5(PF5) |
| BC6H·BC7을 내는 UE 압축 모듈 | 압축 단계(에셋 쪽) |

## 18. 개정 이력

- 개정 1(2026-11-17): 초안. UE 5.8.4 `EPixelFormat` 94개 값 전체, 정보 표·능력 플래그·판정 위치, D3D12·D3D11·Vulkan·OpenGL·Metal 형식 표, sRGB·깊이·스텐실·비디오 처리, 쿠킹 대상 위치를 조사하였다. OpenGL ES 3.2 사양서 Table 8.10·8.11·8.17·8.19·8.26과 NDK 헤더로 GLES 대응을 확인하였다. 사용자 지시 "PixelFormat으로", "언리얼에서 지원하는거 full로 지원", "시간이 걸리더라도 제대로해"에 따라 형식마다 근거를 달고 확인하지 못한 것은 미확인으로 남겼다.
- 개정 2(2026-11-17): 사용자 결정 반영. PX1 (가) sRGB 별도 형식, PX2 계열별 번호 구간, PX3 비디오 형식은 D3D12 생성까지 지금 구현(권장안이던 "생성은 미디어 단계까지 계약"이 아님). 1·11·16절과 PF6 행을 고쳤다.
- 개정 3(2026-11-17): PF2(D3D12)·PF3 틀·PF6(D3D12) 구현과 스펙 차이를 14.1절에 기록하였다. 깊이·스텐실 이미지 크기를 plane 합으로 바꾸었고, `TextureUsage::DepthStencil`·`CreateTextureView`·`ReadTexture`·`GetPixelFormatCaps`를 Graphics 계약에 더하였다.
- 개정 4(2026-11-18): PF4. D3D11·Vulkan·OpenGL ES 대응 표·능력 질의·깊이·스텐실·비디오·view·readback(14.2절). 8.6절 표의 norm16·SNORM 렌더 타깃·BGRA8 조건을 확장 사양으로 대조해 고치고 32F 블렌드 조건(`GL_EXT_float_blend`)을 더하였다.
- 개정 5(2026-11-18): RenderBase GX1·GX2(버퍼) 1단계와 함께 `Multisample` 능력·`GetPixelFormatSampleCounts`를 모든 backend에 넣고, GLES texel buffer 형식을 ES 3.2 Table 8.18로 대조해 공개했다(RQ9, 7절 표·14.3절). Vulkan texel buffer 쓰기 능력은 읽기도 되는 형식만, D3D11 `RGB9E5Ufloat` texel buffer는 공개하지 않는다.
