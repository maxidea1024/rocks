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
