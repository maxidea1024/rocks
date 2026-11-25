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
