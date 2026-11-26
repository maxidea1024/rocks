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

### 7.1 쿠킹 보장 샘플링 표(개정 7, 2026-11-26)

에셋 세션(rocks-c8) 요청이다. 텍스처 압축 단계 TD12([AssetSystem 12.17절](../AssetSystem/12-Textures/12.17-CompressionStage.ko.md))는 고른 형식(대체 포함)이 프로필 `graphicsBackends`의 모든 backend에서 샘플링 보장인지 묻고, 아니면 다음 대체로, 마지막에는 backend의 보장 비압축 형식으로 내린다. 지금까지는 판정할 곳이 없었다. backend 형식 대응표와 능력 질의는 backend 모듈 Private이고 장치가 있어야 답하며(8.1절), Metal 표는 SH5 전까지 없다. 쿠커(Developer — AssetCooker·TextureTools)는 장치 없이 답을 얻어야 한다. 그래서 GraphicsTypes에 장치 없는 공개 표를 둔다.

**보장의 뜻.** 표가 보장이라 한 칸은 엔진이 그 backend·플랫폼에서 받아 주는 모든 장치가 그 형식을 2D 텍스처로 만들고 샘플(선형 필터 칸이면 선형 필터까지)한다는 약속이다. 이 기계의 GPU가 아니라 두 근거만 쓴다.
- **API 최소(`ApiMinimum`)**: 엔진이 장치 생성 때 이미 요구하는 최소 수준의 사양 보장이다. D3D12·D3D11은 feature level 11_0(두 backend 모두 11_0 미만이면 생성 실패), Vulkan은 1.1 core의 필수 형식 표, OpenGL ES는 ES 3.2 context(Android는 ES 3.2를 만들지 못하면 생성 실패), Metal은 Apple7 GPU family다(UE 5.8과 같다 — UE `MetalDevice.cpp:203`이 Apple7 미만 Mac을 거부한다).
- **엔진 요구(`EngineRequirement`)**: 사양이 보장하지 않지만 엔진이 그 플랫폼의 장치에 요구하는 형식 능력이다. 장치 생성이 이 표를 장치 능력과 대조해 하나라도 없으면 그 장치를 받지 않는다(`Unsupported` — 아래 "장치 생성 강제"). 그래서 엔진 요구 칸도 "받아 준 모든 장치"에서 참이다.

능력 범위는 `Sampled`와 `Filterable` 두 비트다(`CookSamplingCaps`). 쿠커가 고르는 형식은 모두 필터 텍스처이고, 32F 대체 규칙(12.16.1 HDR_F32 — 필터가 Point가 아니면 32F 필터가 필요)만 `Sampled`만으로 되는지를 따로 묻는다. 3D·cube·mip·렌더 타깃·storage는 표에 넣지 않는다(한계 참고).

**키.** (backend, 플랫폼)이다.
- backend는 장치 계약의 `Backend` enum(`D3D12`·`D3D11`·`OpenGL`·`Vulkan`·`Metal`)을 그대로 쓴다. 쿠커가 GraphicsTypes만 링크해도 쓰도록 enum과 `BackendName`을 Graphics(`DeviceTypes.h`)에서 GraphicsTypes(`GraphicsBackend.h`)로 옮기고 이름 해석 `ParseBackendName`을 더한다. 이름은 매니페스트 backend 어휘(`IsKnownGraphicsBackend`, AssetSystem 10.3절 1항)와 같은 철자다.
- 플랫폼은 새 enum `GraphicsPlatform`(`Windows`·`Mac`·`Android`)이다. 이름은 `HostPlatformName()`·플랫폼 층 폴더·엔진 프로필 id와 같은 철자이고 `ParseGraphicsPlatformName`으로 읽는다. 실행 파일의 플랫폼은 `HostGraphicsPlatform()`(Base `IsWindows`·`IsMac`·`IsAndroid`)이다. Linux는 아직 빌드 대상이 아니다(BuildTargets BS6 보류). 넣을 때 Vulkan 열은 Windows와 같은 데스크톱 규칙으로 더한다.
- 플랫폼이 필요한 이유: 같은 backend도 플랫폼마다 받아 주는 장치가 다르다. Vulkan은 데스크톱(BC)과 Android(ASTC·ETC2)가 다르고, OpenGL은 Windows에서는 개발용 미리보기(GLES31_PC)일 뿐 쿠킹 대상이 아니다.
- 쿠커는 프로필의 플랫폼을 프로필 id로 얻는다(엔진 프로필 파일이 필수라 id가 곧 플랫폼 이름 — `Windows`·`Mac`·`Android`). 프로필에 플랫폼 칸을 따로 둘지는 에셋 세션이 정한다.

**쿠킹 대상 조합.** 표가 칸을 가진 조합은 여섯이다: D3D12·Windows, D3D11·Windows, Vulkan·Windows, Vulkan·Android, OpenGL·Android, Metal·Mac. 그 밖의 조합(OpenGL·Windows 미리보기, Vulkan·Mac 등 9개)은 쿠킹 대상이 아니다. `IsCookSamplingTarget`이 거짓이고 모든 형식의 보장이 `None`이며 장치 생성은 대조하지 않는다.

**backend별 보장과 출처.**

| 조합 | 엔진이 받는 장치 | API 최소 근거 | 엔진 요구(장치 생성이 대조) |
|---|---|---|---|
| D3D12·Windows | FL 11_0 이상(`D3D12CreateDevice(…, D3D_FEATURE_LEVEL_11_0)`) | Microsoft "Format Support for Direct3D Feature Level 11.0 Hardware" 표의 Texture2D·Shader sample (any filter) 열: 8·16비트, 32비트 float(RGB32F는 샘플이 Optional이라 제외), 묶음, BC1–7 전부 Required | 없음 |
| D3D11·Windows | FL 11_0 이상(11_1 → 11_0 순으로 시도) | 같은 표 | 없음 |
| Vulkan·Windows | Vulkan 1.1, graphics queue | Vulkan 사양 "Required Format Support" 필수 표: R8·RG8·RGBA8(선형·sRGB·snorm)·BGRA8(선형·sRGB)·16F·R5G6B5·A1R5G5B5·A2B10G10R10·B10G11R11·E5B9G9R9는 샘플 + 선형 필터, 32F는 샘플만(선형 필터 없음), 16비트 unorm·snorm 샘플은 필수 아님 | BC 전부(`textureCompressionBC`가 정의하는 형식 — UE `VulkanGenericPlatform.h:76 SupportsBCTextureFormats()`가 데스크톱에서 늘 참), 16비트 unorm R·RG·RGBA(UE `SupportsR16UnormTextureFormat()` 데스크톱 참), 32F R·RG·RGBA의 선형 필터(UE 데스크톱 쿠킹 `bSupportFilteredFloat32Textures = true`, `GenericWindowsTargetPlatformControls.h:176`) |
| Vulkan·Android | Vulkan 1.1, graphics queue | 같은 필수 표 | ASTC LDR 전부(`textureCompressionASTC_LDR` 정의 형식)와 ETC2·EAC 전부(`textureCompressionETC2`). UE `VulkanAndroidPlatform.h:138–139`가 둘 다 참이고 Android Vulkan BC는 `r.Vulkan.SupportsBCTextureFormats` 기본 꺼짐이다. Vulkan 사양은 세 압축 묶음 가운데 하나만 요구하므로 엔진 요구로 둔다. ETC2는 지금 쿠킹하지 않지만(12.16.2 Multi 후속) OpenGL ES 열(ES 3.0 core)과 맞춘다 |
| OpenGL·Android | ES 3.2 context | ES 3.2 Table 8.10 TF 열(texture-filterable), 32F는 TF가 없어 샘플만(`GL_OES_texture_float_linear`는 확장), ETC2·EAC는 ES 3.0 core, ASTC LDR은 ES 3.2 core(`KHR_texture_compression_astc_ldr` 편입) | 없음 |
| Metal·Mac | Apple7 이상(SH5에서 생성이 거부) | Apple "Metal Feature Set Tables"(2026-10-23판) Apple7 열(각주 1: Metal 3·4 장치는 모두 Apple7 열 이상): 8·16비트 unorm·snorm·float와 묶음 32비트 형식은 Filter, 32비트 float는 샘플만(Note: 모든 형식은 샘플 가능) | BC 전부(`supportsBCTextureCompression` — 각주 9 "All Apple7 and Apple8 macOS devices have support"), 32F 선형 필터(`supports32BitFloatFiltering` — UE Mac 쿠킹도 `bSupportFilteredFloat32Textures = true`, `GenericMacTargetPlatformControls.h:99`). Metal backend(SH5)가 장치 생성 때 대조한다 |

부모 결정(2026-11-26)과 다르게 확인한 것: Apple 표의 Apple7 열에는 ASTC LDR Filter가 있다(ASTC pixel formats — Apple2부터). 그래도 Mac ASTC는 보장으로 두지 않는다(결정대로 — Mac 프로필은 BC 표를 쓰므로 영향이 없고, Metal 대응표가 없는 동안 넓게 약속하지 않는다). ETC2·ASTC HDR·16비트 묶음(B5G6R5 등)·R8/RG8 sRGB도 같은 이유로 Mac에서 보장하지 않는다. SH5에서 Metal 대응표를 만들 때 다시 본다(17절).

**형식 표.** F = 샘플 + 선형 필터(API 최소), S = 샘플만(API 최소), E = 샘플 + 선형 필터(엔진 요구), — = 보장 없음. 대체 계열은 아래 "보장 비압축 대체"에 쓴다(L8 = 8비트 선형, S8 = 8비트 sRGB, HP = 고정밀, 없음 = 쿠킹 대상 아님). 정수 형식·`R64Uint`·깊이·스텐실·비디오는 쿠킹 텍스처 형식이 아니어서 모두 보장 없음으로 둔다(사양상 샘플이 되는 것도 약속하지 않는다 — 필요해지면 표를 넓힌다). 코드 표(`CookSampling.cpp`)는 이 표와 행마다 같고, 유효 형식마다 행이 정확히 하나인지 시험이 확인한다.

| 형식 | D3D12·W | D3D11·W | Vulkan·W | Vulkan·A | OpenGL·A | Metal·Mac | 대체 계열 |
|---|---|---|---|---|---|---|---|
| `R8Unorm`·`A8Unorm`·`RG8Unorm`·`RGBA8Unorm` | F | F | F | F | F | F | L8 |
| `RGBA8UnormSrgb` | F | F | F | F | F | F | S8 |
| `RG8Snorm`·`RGBA8Snorm` | F | F | F | F | F | F | HP |
| `R8UnormSrgb`·`RG8UnormSrgb` | — | — | — | — | — | — | S8 |
| `RGB8Unorm` | — | — | — | — | F | — | L8 |
| `RGB8UnormSrgb` | — | — | — | — | F | — | S8 |
| `BGRA8Unorm` | F | F | F | F | — | F | L8 |
| `BGRA8UnormSrgb` | F | F | F | F | — | F | S8 |
| `R16Unorm`·`RG16Unorm`·`RGBA16Unorm` | F | F | E | — | — | F | HP |
| `RG16Snorm`·`RGBA16Snorm` | F | F | — | — | — | F | HP |
| `R16Float`·`RG16Float`·`RGBA16Float` | F | F | F | F | F | F | HP |
| `R32Float`·`RG32Float`·`RGBA32Float` | F | F | E | S | S | E | HP |
| `RGB32Float` | — | — | — | — | S | — | HP |
| `R5G6B5Unorm` | F | F | F | F | F | — | L8 |
| `B5G5R5A1Unorm` | F | F | F | F | — | — | L8 |
| `RGB10A2Unorm`·`RG11B10Ufloat`·`RGB9E5Ufloat` | F | F | F | F | F | F | HP |
| `BC1Unorm`·`BC2Unorm`·`BC3Unorm`·`BC4Unorm`·`BC5Unorm`·`BC7Unorm` | F | F | E | — | — | E | L8 |
| `BC1UnormSrgb`·`BC2UnormSrgb`·`BC3UnormSrgb`·`BC7UnormSrgb` | F | F | E | — | — | E | S8 |
| `BC6HUfloat` | F | F | E | — | — | E | HP |
| `ETC2RGB8Unorm`·`ETC2RGBA8Unorm`·`EACR11Unorm`·`EACRG11Unorm` | — | — | — | E | F | — | L8 |
| `ETC2RGB8UnormSrgb`·`ETC2RGBA8UnormSrgb` | — | — | — | E | F | — | S8 |
| ASTC LDR `ASTC{4x4,6x6,8x8,10x10,12x12}Unorm` | — | — | — | E | F | — | L8 |
| ASTC LDR `ASTC{…}UnormSrgb` | — | — | — | E | F | — | S8 |
| ASTC HDR `ASTC{…}Hdr` | — | — | — | — | — | — | HP |
| 정수 16종(`R8Uint`·`R8Sint`·`RG8Uint`·`RGBA8Uint`, 16·32비트 Uint·Sint)·`R64Uint`·깊이 5종·`NV12`·`P010` | — | — | — | — | — | — | 없음 |

backend Private 표와 맞춘 근거: Vulkan `R5G6B5Unorm`은 `R5G6B5_UNORM_PACK16`, `B5G5R5A1Unorm`은 `A1R5G5B5_UNORM_PACK16`, `RGB10A2Unorm`은 `A2B10G10R10_UNORM_PACK32`, `A8Unorm`은 R8 + 성분 대응이라(8.5절) 모두 필수 표에 든다. OpenGL `A8Unorm`은 R8 + swizzle(TF)이고, `BGRA8Unorm`·R8/RG8 sRGB·norm16은 확장이라 보장이 아니며 `B5G5R5A1Unorm`·`BGRA8UnormSrgb`는 대응이 없다(8.6절). D3D `RGB8*`·R8/RG8 sRGB는 DXGI 형식이 없다(8.4절).

**장치 생성 강제.** D3D12·D3D11·Vulkan·OpenGL ES backend는 형식 능력을 질의한 직후 `FindMissingCookSampling(backend, HostGraphicsPlatform(), 능력 배열)`을 부른다. 보장 칸의 능력이 장치에 없는 형식이 하나라도 있으면 장치 생성은 `Unsupported`로 실패하고 `DeviceCreateReport`·`LastError`에 첫 형식의 이름·backend·플랫폼을 적는다(문구는 영어). 런타임은 프로필 backend 순서의 다음 backend를 시도한다. API 최소 칸은 사양상 늘 통과하므로 그 실패는 backend Private 표의 오류를 뜻한다(시험이 잡는다). 엔진 요구 칸이 실제로 장치를 거르는 경우는 BC·16비트 unorm·32F 필터가 없는 데스크톱 Vulkan, ASTC LDR·ETC2가 없는 Android Vulkan이다. Metal은 SH5에서 같은 대조를 넣는다.

**보장 비압축 대체.** `CookSamplingFallback(backends, platform, format, required)`가 TD12의 마지막 대체를 준다. 결과는 `backends` 모두에서 `required`(기본 `CookSamplingCaps`)가 보장인 비압축 형식이다.
1. `format`이 비압축 색 형식이고 이미 모든 backend에서 보장이면 그대로 준다.
2. 아니면 대체 계열 후보를 앞에서부터 본다: L8 `BGRA8Unorm` → `RGBA8Unorm`, S8 `BGRA8UnormSrgb` → `RGBA8UnormSrgb`, HP `RGBA16Float`. 앞 후보는 UE 비압축 대체(`NameBGRA8`·`NameRGBA16F`, Texture.cpp 4396–4475)와 같고, 뒤 후보는 OpenGL ES처럼 BGRA8을 보장하지 않는 backend를 위한 것이다(UE GL RHI는 BGRA를 흉내 내지만 Rocks backend는 다른 형식으로 바꾸지 않는다, 8.3절).
3. 대체 계열이 없거나(정수·깊이·비디오), 쿠킹 대상이 아닌 조합이 끼었거나, backend 목록이 비었거나, `required`에 `Sampled`가 없거나 `CookSamplingCaps` 밖 비트가 있으면 `Unknown`이다.
- 계열 후보 마지막(`RGBA8Unorm`·`RGBA8UnormSrgb`·`RGBA16Float`)은 여섯 조합 모두에서 F다. 그래서 쿠킹 대상 조합만 든 목록이면 계열이 있는 형식은 늘 답이 있다(시험이 단언).
- 채널 수는 줄이지 않는다(1채널도 4채널 대체 — UE와 같다). 8비트 snorm, 16비트·float, 묶음 고정밀 형식은 값 범위·정밀도를 지키려 HP(`RGBA16Float`)다. EAC R11·RG11은 쿠킹 입력이 8비트(TD3)라 L8이다.

**BC 4의 배수 규칙과의 관계.** BC는 상위 mip 폭·높이가 4의 배수여야 한다(`PixelFormatInfo::topLevelBlockAligned`, UE `RequiresBlock4Alignment`). 이것은 형식과 크기의 규칙이고 backend 보장과 독립이다. 쿠커는 12.17.2 4번(크기 규칙)을 먼저, 8번(이 표)을 나중에 적용한다. 이 표가 BC를 보장해도 크기 규칙이 비압축으로 바꾼 텍스처는 그대로 비압축이다. 반대로 이 표가 형식을 보장하지 않는 backend에서는 크기와 무관하게 대체한다.

**공개 API(GraphicsTypes).**
- `GraphicsTypes/GraphicsBackend.h`: `Backend`, `BackendCount`, `BackendName`, `ParseBackendName`(대소문자 구분, 모르면 `NotFound`).
- `GraphicsTypes/GraphicsPlatform.h`: `GraphicsPlatform`, `GraphicsPlatformCount`, `GraphicsPlatformName`, `ParseGraphicsPlatformName`, `HostGraphicsPlatform()`.
- `GraphicsTypes/CookSampling.h`: `CookSamplingCaps`, `CookSamplingBasis`(`None`·`ApiMinimum`·`EngineRequirement`), `IsCookSamplingTarget`, `CookSamplingGuarantee`(조합 하나, 또는 backend 목록의 교집합), `CookSamplingGuaranteeBasis`, `CookSamplingFallback`, `FindMissingCookSampling`.

**버린 대안.**
- 셰이더 형식 id(`D3D12_SM6`·`GLES31_ANDROID` 등)로 키 잡기: 형식 id가 플랫폼을 담지만 프로필이 backend마다 형식 목록을 여럿 가질 수 있고, 보장은 컴파일 대상이 아니라 장치 수용 규칙의 성질이다. 장치 생성 쪽은 셰이더 형식을 모른다.
- UE처럼 대상 플랫폼 하나의 가정으로 두기(UE `ITargetPlatformControls::GetTextureFormats`·`SupportsFeature` — 플랫폼이 모든 RHI의 지원을 가정): 프로필 backend 목록마다 다른 보장(OpenGL ES의 BGRA8 등)을 표현하지 못하고, 가정이 틀린 장치는 실행 중 텍스처 생성 실패로만 드러난다. Rocks는 같은 표를 장치 생성이 강제해 가정이 늘 참이 되게 한다.
- backend Private 표를 공개하기: 장치 enum(DXGI·VkFormat·GLenum)이 GraphicsTypes에 새고(8.1절 원칙), 그 표는 "이 장치에서 될 수 있는 것"이지 "모든 장치가 하는 것"이 아니다.
- 이 기계 GPU의 질의 결과를 표로 굳히기: 다른 기계에서 틀린다.
- 16비트 unorm·32F 필터를 데스크톱 Vulkan에서 보장 없음으로 두기: Windows 프로필 전체(D3D12·D3D11 포함)의 G16·HDR_F32 쿠킹이 `RGBA16Float`로 커진다. 데스크톱 GPU는 모두 지원하고 UE도 가정하므로 엔진 요구로 강제하는 쪽을 골랐다.

**한계.**
- 차원(3D·cube)·mip·배열 보장은 표에 없다. 압축 볼륨(12.17.2 2번 — 프로필 칸)은 지금처럼 쿠커 규칙이 정한다. Vulkan의 압축 3D는 사양 필수가 아니다(필요하면 열을 더하고 장치 생성 대조를 넓힌다).
- Metal 열은 Rocks Metal 대응표 없이 Apple 표와 UE로 정했다(SH5에서 대조, 17절).
- Android 장치 생성 강제는 이 기계에서 실행하지 못했다(Android 장치 없음). 데스크톱 장치만 시험했다.

**UE 대응.**

| Rocks | UE 5.8 | 차이 |
|---|---|---|
| `CookSamplingGuarantee`(backend × 플랫폼 표) | `ITargetPlatformControls::GetTextureFormats` → `GetDefaultTextureFormatNamePerLayer`(플랫폼마다 형식 이름), RHI의 `GPixelFormats[].Supported` | UE는 대상 플랫폼 하나가 모든 RHI를 가정하고 표가 없다. Rocks는 backend마다 칸과 근거 등급을 둔다 |
| 엔진 요구(데스크톱 Vulkan BC·16비트 unorm·32F 필터) | `FVulkanGenericPlatform::SupportsBCTextureFormats`·`SupportsR16UnormTextureFormat`, Windows `bSupportFilteredFloat32Textures = true` | UE는 가정만 하고 장치를 거르지 않는다. Rocks는 장치 생성이 대조한다 |
| 엔진 요구(Android Vulkan ASTC·ETC2) | `FVulkanAndroidPlatform::SupportsASTCTextureFormats`·`SupportsETC2TextureFormats` | 같음(Rocks는 대조) |
| Metal·Mac Apple7 + BC + 32F 필터 | `MetalDevice.cpp:203`(Apple7 미만 거부), `GenericMacTargetPlatformControls.h:99` | Rocks는 SH5에서 장치 생성 대조 |
| `CookSamplingFallback` | `GetDefaultTextureFormatName`의 `NameBGRA8`·`NameRGBA16F` 대체(Texture.cpp 4396–4475) | BGRA8을 보장하지 않는 backend를 위해 RGBA8 후보를 더한다 |
| `GraphicsPlatform` | `ITargetPlatform` 이름(Windows·Mac·Android) | enum 셋, 데이터 없음 |

**시험.**
- `GraphicsTypes/Tests/CookSamplingTests.cpp`(EngineTests): 형식 86개 × 조합 6개 기대표 전부(이 절 표를 시험 쪽에 따로 옮긴 표), 쿠킹 대상이 아닌 조합 9개는 모든 형식 `None`, Unknown·무효 번호·범위 밖 enum은 `None`, 근거 등급, 교집합(빈 목록 `None`, 대상 아닌 backend가 끼면 `None`), 대체(형식마다·조합 목록마다 결과가 모든 backend에서 보장된 비압축 형식인지, 계열 후보 순서, `required` 경계), `FindMissingCookSampling`(모두 갖춘 배열·짧은 배열·한 칸만 빠진 배열), 이름 왕복과 거부.
- Sandbox `--self-test`·`--texture-self-test`의 장치 대조: 만든 장치마다 보장 칸이 `GetPixelFormatCaps`에 있는지 다시 확인하고 칸 수를 적는다. OpenGL ES 미리보기(Windows)는 쿠킹 대상이 아니므로 Android 열과 대조해 차이를 보고만 한다(8.2절 — 미리보기 context는 Android 장치보다 넓지 않게 판정하지만, ES 확장이 없는 데스크톱 드라이버는 더 좁을 수 있다).
