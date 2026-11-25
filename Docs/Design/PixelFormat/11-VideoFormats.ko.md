## 11. 비디오 형식과 다중 plane

- `NV12`(8비트)·`P010`(10비트, 16비트 저장 상위 10비트)은 4:2:0 2-plane 형식이다. plane 0은 휘도(`R8Unorm`·`R16Unorm`, 원래 크기), plane 1은 색차 CbCr(`RG8Unorm`·`RG16Unorm`, 폭·높이 ½)이다(UE `PF.cpp:110–111, 137–138`, `DXGI:238–251`). 폭·높이는 2의 배수여야 한다(`DXGI:277–278, 315–316`).
- 정보 표의 `planes[2]`가 plane별 형식·크기 축소·texel 바이트를 준다. `PixelFormatImageBytes`는 두 plane의 합이다.
- 상태: 번호표·정보 표·크기 함수는 PF1, D3D12 생성·plane 업로드·plane view는 PF6에서 구현한다(결정 PX3, 2026-11-17 "생성까지 지금 구현"). GLES·D3D11·Vulkan·Metal은 그 backend의 대응 단계(PF4·PF5)에서 정하며 그 전에는 `UnsupportedCapability`다.
- backend 방향: D3D12는 DXGI `NV12`·`P010` + plane별 SRV(UE와 달리 SRV 생성), Vulkan은 multi-planar 형식 + plane aspect(`VK_IMAGE_ASPECT_PLANE_0/1_BIT`, sampler Y′CbCr 변환은 쓰지 않고 plane별 view), OpenGL ES·Metal은 그때 정한다(UE는 미지원).
- 포함 시기는 결정 PX3으로 정하였다(16절).
