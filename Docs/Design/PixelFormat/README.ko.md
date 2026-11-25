# 픽셀 형식(PixelFormat) 설계

기준일: 2026-11-25, 개정 6(초안). UE 5.8이 정의하는 픽셀 형식 전체를 Rocks에 들이기 위한 조사와 구현 스펙이다. 범위는 다음과 같다.
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

## 목차

- [1. 결정 요약](01-Decisions.ko.md)
- [2. 문서 위치와 교차 참조](02-CrossReferences.ko.md)
- [3. 현재 Rocks 상태](03-CurrentState.ko.md)
- [4. UE 5.8 조사](04-UeSurvey.ko.md)
- [5. Rocks 형식 목록](05-FormatList.ko.md)
- [6. 형식 정보(`PixelFormatInfo`)](06-FormatInfo.ko.md)
- [7. 형식 능력 플래그(`PixelFormatCaps`)](07-FormatCaps.ko.md)
- [8. backend 대응 표](08-BackendMapping.ko.md)
- [9. sRGB 표현(결정 PX1)](09-Srgb.ko.md)
- [10. 깊이·스텐실](10-DepthStencil.ko.md)
- [11. 비디오 형식과 다중 plane](11-VideoFormats.ko.md)
- [12. 저장 값과 번호 재배정](12-StoredValues.ko.md)
- [13. 이름 변경 계획](13-RenamePlan.ko.md)
- [14. 구현 단계](14-ImplementationStages.ko.md)
- [15. A0-G와의 분담](15-A0G.ko.md)
- [16. 결정 필요 사항](16-OpenDecisions.ko.md)
- [17. 미확인 목록](17-Unverified.ko.md)
- [18. 개정 이력](18-Revisions.ko.md)
