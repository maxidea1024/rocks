# 렌더러(Renderer) 설계 — 13. backend 지원표

[목차](README.ko.md)

## 13. backend 지원표

**근거 종류.** "형식 정의"는 `Engine/Config/ShaderFormats/<id>.json`의 값(셰이더 시스템 12.1절), "사양"은 API 사양 최소 보장, "PixelFormat"은 [픽셀 형식](../PixelFormat/08-BackendMapping.ko.md)의 backend 표다. 표의 ○는 그 근거로 가능, △는 장치 능력(GX1)에 따라, ×는 불가, "미확인"은 확인하지 못함이다. 지원하지 않는 칸은 13.2절 처리 규칙을 따른다.

### 13.1 기능별 표

| 기능 | D3D12(`D3D12_SM6`) | D3D11(`D3D11_SM5`) | Vulkan(`VULKAN_SM5`) | Metal(`METAL_SM5`) | GLES(`GLES31_*`) | 근거 |
|---|---|---|---|---|---|---|
| compute(light grid·HZB·AO) | ○ | ○(CS 5.0) | ○ | ○ | ○(ES 3.1+) | 형식 정의 `features: compute`, 계획 11.1절 |
| PS typed buffer 읽기(grid·GPU Scene) | ○ | ○ | ○ | ○ | ○(texture buffer, 형식 대조 미확인) | 셰이더 시스템 7.2절 3·4항, PixelFormat 8.6절 |
| VS·PS storage buffer | ○ | ○ | ○(단계당 4) | ○(4) | ×(VS·PS 0) | 형식 정의 `storageBuffers`(GLES 형식의 값 4는 CS 기준), VS·PS 0은 셰이더 시스템 7.2절 4항 "구조화·raw 버퍼 SRV도 GLES 형식에서는 CS만" |
| PS·VS UAV 쓰기 | ×(규약) | ×(규약) | ×(규약) | ×(규약) | × | 셰이더 시스템 7.2절 4항 "UAV는 CS만" |
| 단계당 SRV 텍스처 | 넓음(128) | 16 | 넓음(장치 요구, RQ22) | 넓음(128) | 16 | 형식 정의 `sampledTextures`(결정 RN13 개정). D3D11은 셰이더 시스템 개정 13에서 16으로, Vulkan은 개정 14에서 128로 고쳤다(RQ22). 렌더러 자원은 모든 형식에서 16 안(7.4절) |
| 단계당 샘플러 | 16 | 16 | 16 | 16(장치 상한) | 16 | 형식 정의 `samplers`. 짝 규칙에서는 16장을 넘는 머티리얼 텍스처에 공유 샘플러가 필요하다(7.4절, RQ23) |
| MRT 수 | 8 | 8 | 8(결정 RN3, RQ11 반영: 셰이더 시스템 개정 14) | 8 | 4 | 형식 정의 `renderTargets` |
| Deferred 기본 layout(MRT 4) | ○ | ○ | ○ | ○ | ○ | 7.1절 |
| Deferred D·E(MRT 5–6) | ○ | ○ | ○(RQ11 뒤) | ○ | ×(결정 RN4) | 7.1절 |
| `D32FloatS8Uint` 깊이 | ○ | ○ | △(장치 질의) | ○ | ○(core) | PixelFormat 8.4–8.7·10절. Vulkan은 GX1 형식 질의로 확인하고, 없으면 그 장치에서 Deferred·Forward+ 모두 `Unsupported`(대체 형식 선택 여부는 PixelFormat 10절 결정을 따른다) |
| `RGBA16Float` SceneColor(렌더·블렌드·필터) | ○ | ○ | ○ | ○ | ○(core) | PixelFormat 8.6절 |
| `RG16Float` velocity | ○ | ○ | ○ | ○ | ○(core) | PixelFormat 8.6절 |
| `RGB10A2Unorm` GBufferA | ○ | ○ | ○ | ○ | ○(core) | PixelFormat 8.6절 |
| 비교 샘플러(PCF) | ○ | ○ | ○ | ○ | ○ | SX13, RenderBase 9.6절 |
| Forward+ MSAA 2·4 | △ | △ | △ | △ | △ | 형식별 `Multisample` 능력(PixelFormat 7절) |
| VS layer·viewport 출력(point atlas 한 번 그리기는 viewport, 후속) | ○(D3D12는 늘 된다) | △(OPTIONS3, D3D11 형식 포함은 SH5) | △(`VK_EXT_shader_viewport_index_layer`, viewport는 `multiViewport`) | 미확인 | ×(ES core에 없고 `GL_NV_viewport_array2` 확장만, Rocks는 쓰지 않음) | 셰이더 시스템 7.2절 7항·결정 12 |
| 깊이 clamp(CSM pancaking) | ○ | ○ | △(`depthClamp` 기능) | 미확인 | 미확인(ES 3.2 core 여부·확장 미확인) | RQ5. D3D는 rasterizer depth clip 끄기. 없는 backend는 셰이더에서 깊이를 clamp하는 공개 대체(9.2절) |
| indirect draw·dispatch | ○ | ○ | ○ | ○ | ○(단일 draw, multi-draw는 확장) | RQ3. GLES 3.1 `glDrawElementsIndirect` |
| async compute(RG4) | △ | × | △ | △ | × | RenderBase RG4, 계획 11.5절 |
| framebuffer fetch·subpass(GLES deferred 1-pass) | — | — | △(GX13) | 미확인 | △(확장, GX13) | RenderBase GX13 |
| 64 bit atomic(가상 그림자 유사) | △(SM 6.6 능력) | × | △(확장) | 미확인 | × | UE VSM 조건 `RU:1274–1328` |
| 하드웨어 RT(`RayScene`) | △ | × | △ | △ | × | 계획 14.4절. UE `RU:1903–1935` |
| HDR 출력(ST2084·scRGB) | △ | △ | △ | 미확인 | 미확인 | 계획 26절 |
| image 쓰기 `RG11B10Ufloat` | ○ | ○ | △ | 미확인 | ×(형식 정의 `unsupportedImageFormats`) | SceneColor를 compute로 쓰는 효과는 `RGBA16Float` 사용 |

### 13.2 경로별 표

| 경로·기능 | D3D12 | D3D11 | Vulkan | Metal | GLES |
|---|---|---|---|---|---|
| Forward+(직접광·그림자·투명) | ○ | ○ | ○ | ○ | ○ |
| Forward+ MSAA | △ | △ | △ | △ | △ |
| Deferred 기본 shading model | ○ | ○ | ○ | ○ | ○(다중 pass, GX13 전) |
| Deferred 고급 shading model(GBufferD) | ○ | ○ | ○(RQ11 뒤) | ○ | `Unsupported` → 그 머티리얼은 쿠킹 오류 또는 Forward+ view에서만(결정 RN4) |
| Deferred 정적 그림자 mask(GBufferE) | ○ | ○ | ○(RQ11 뒤) | ○ | `Unsupported`(Forward+는 지원) |
| decal(Deferred GBuffer decal, Forward+ DBuffer) | ○ | ○ | ○ | ○ | ○(SRV 예산 안, 7.4절) |
| 정적 축 Baked | ○ | ○ | ○ | ○ | ○ |
| 동적 축(VoxelConeTracing·DynamicGI) | 각 방식 스펙 | 각 방식 스펙 | 각 방식 스펙 | 각 방식 스펙 | 각 방식 스펙 |
| 동적 축 RayTraced·PathTracing(하드웨어) | △ | × | △ | △ | × |

**처리 규칙.**
- 지원하지 않는 조합은 view 생성(`ViewDesc` 검증) 또는 쿠킹에서 `Unsupported`로 거부한다. 다른 경로·형식·효과로 조용히 바꾸지 않는다(계획 11.1절, PixelFormat 1절).
- 공개된 대체는 profile이 명시한 것만 쓴다(예: 동적 GI 낮춤 순서, 계획 14.3절). 쓴 대체는 resolved profile과 계측에 남는다.
- GLES Deferred는 framebuffer fetch(GX13) 전까지 GBuffer를 메모리에 쓰고 조명 pass가 다시 읽는 다중 pass다. UE 모바일도 다중 pass와 단일 pass(subpass) 둘을 둔다(`R/MobileShadingRenderer.cpp:1805–1814`).
