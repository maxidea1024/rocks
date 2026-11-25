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
