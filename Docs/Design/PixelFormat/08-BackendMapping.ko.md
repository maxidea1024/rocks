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
