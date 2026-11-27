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

### 14.4 쿠킹 보장 샘플링 표(PF7, 개정 7)

| ID | 내용 | 상태 | 위치 |
|---|---|---|---|
| PF7 | 7.1절 표·대체·이름표, D3D12·D3D11·Vulkan·OpenGL ES 장치 생성 대조 | **구현** | `GraphicsTypes/CookSampling.h`·`GraphicsBackend.h`·`GraphicsPlatform.h`, 시험 `GraphicsTypes/Tests/CookSamplingTests.cpp`, 장치 대조 Sandbox `CookSamplingSelfTest.cpp` |

- `Backend`·`BackendName`을 Graphics `DeviceTypes.h`에서 GraphicsTypes `GraphicsBackend.h`로 옮겼다(`DeviceTypes.h`가 포함하므로 사용처 변경 없음).
- 장치 생성 대조 위치: D3D12·D3D11 `Initialize`의 형식 능력 질의 직후, Vulkan `Initialize`의 `QueryFormatCaps`·`QueryDeviceCaps` 직후, OpenGL ES `Initialize`의 형식 능력 표 직후. 모두 stage `CookSamplingGuarantee`, 오류 `Unsupported`이고 진단 문장은 `WriteCookSamplingRejection` 하나를 함께 쓴다.

검증(2026-11-27, Sandbox `--texture-self-test`, 검증 계층 켬, lavapipe는 `VK_ADD_DRIVER_FILES`):

| 장치 | 대조 열 | 보장(필터·샘플만·엔진 요구) | 빠짐 |
|---|---|---|---|
| D3D12 GTX 1060 | D3D12·Windows | 36·0·0 | 0 |
| D3D12 WARP | D3D12·Windows | 36·0·0 | 0 |
| D3D11 GTX 1060 | D3D11·Windows | 36·0·0 | 0 |
| D3D11 WARP | D3D11·Windows | 36·0·0 | 0 |
| Vulkan GTX 1060 | Vulkan·Windows | 34·0·17 | 0 |
| Vulkan lavapipe(llvmpipe) | Vulkan·Windows | 34·0·17 | 0 |
| OpenGL ES 미리보기, ES 3.2 context | OpenGL·Android(보고만) | 32·4·0 | 0 |
| OpenGL ES 미리보기, 데스크톱 GL 4.3 core context | OpenGL·Android(보고만) | 32·4·0 | 10(ASTC LDR 전부 — 데스크톱 드라이버가 `GL_KHR_texture_compression_astc_ldr`를 내지 않고, 8.2절 규칙대로 확장 이름으로만 켠다) |

backend Private 표를 고친 곳은 없다(보장 칸과 다른 장치 능력이 없었다). 데스크톱 core 미리보기의 ASTC 차이는 미리보기 한계이고 Android 장치(ES 3.2 core)의 보장과 무관하다.
