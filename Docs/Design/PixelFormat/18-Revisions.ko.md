## 18. 개정 이력

- 개정 1(2026-11-17): 초안. UE 5.8.4 `EPixelFormat` 94개 값 전체, 정보 표·능력 플래그·판정 위치, D3D12·D3D11·Vulkan·OpenGL·Metal 형식 표, sRGB·깊이·스텐실·비디오 처리, 쿠킹 대상 위치를 조사하였다. OpenGL ES 3.2 사양서 Table 8.10·8.11·8.17·8.19·8.26과 NDK 헤더로 GLES 대응을 확인하였다. 사용자 지시 "PixelFormat으로", "언리얼에서 지원하는거 full로 지원", "시간이 걸리더라도 제대로해"에 따라 형식마다 근거를 달고 확인하지 못한 것은 미확인으로 남겼다.
- 개정 2(2026-11-17): 사용자 결정 반영. PX1 (가) sRGB 별도 형식, PX2 계열별 번호 구간, PX3 비디오 형식은 D3D12 생성까지 지금 구현(권장안이던 "생성은 미디어 단계까지 계약"이 아님). 1·11·16절과 PF6 행을 고쳤다.
- 개정 3(2026-11-17): PF2(D3D12)·PF3 틀·PF6(D3D12) 구현과 스펙 차이를 14.1절에 기록하였다. 깊이·스텐실 이미지 크기를 plane 합으로 바꾸었고, `TextureUsage::DepthStencil`·`CreateTextureView`·`ReadTexture`·`GetPixelFormatCaps`를 Graphics 계약에 더하였다.
- 개정 4(2026-11-18): PF4. D3D11·Vulkan·OpenGL ES 대응 표·능력 질의·깊이·스텐실·비디오·view·readback(14.2절). 8.6절 표의 norm16·SNORM 렌더 타깃·BGRA8 조건을 확장 사양으로 대조해 고치고 32F 블렌드 조건(`GL_EXT_float_blend`)을 더하였다.
- 개정 5(2026-11-18): RenderBase GX1·GX2(버퍼) 1단계와 함께 `Multisample` 능력·`GetPixelFormatSampleCounts`를 모든 backend에 넣고, GLES texel buffer 형식을 ES 3.2 Table 8.18로 대조해 공개했다(RQ9, 7절 표·14.3절). Vulkan texel buffer 쓰기 능력은 읽기도 되는 형식만, D3D11 `RGB9E5Ufloat` texel buffer는 공개하지 않는다.
