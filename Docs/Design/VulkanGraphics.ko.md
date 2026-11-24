# Vulkan 그래픽 backend(VulkanGraphics) 설계

기준일: 2026-11-25, 개정 8(초안). Vulkan backend `VulkanGraphics`가 Graphics 장치 계약(`Graphics/Device.h`)을 Vulkan 1.1 위에서 어떻게 지키는지 정한다. [셰이더 시스템](ShaderSystem/README.ko.md) P9 "Vulkan 선행"의 산출물이다. 그 문서가 P9 산출물로 "EngineRuntime에 절을 더한다"고 적었지만, 내용이 커서 이 문서로 따로 두고 [엔진 런타임](EngineRuntime/10-Graphics.ko.md) 10절에는 요약과 링크만 둔다.

UE 근거는 UE 5.8.4(`release`, commit `f45729c0b6f097038e72b52d366a62baeca3275b`, 로컬 클론) 원본이다. 경로는 `Engine/Source/Runtime/VulkanRHI/` 기준이며 `Private/`는 `P/`, `Public/`은 `Pub/`로 줄인다. Vulkan 사양 근거는 Vulkan 1.4 사양(Vulkan-Docs)의 절 이름으로 적는다.

**용어 방침.** 구조는 UE를 따르되 이름은 Rocks 이름이다. UE 타입 이름은 13절 대응표와 근거 열에만 둔다.

**UE 레거시 제외.** UE가 하위 호환을 위해 남긴 경로(`VK_EXT_debug_marker` 대체 경로, 옛 render pass 1 호출, deprecated cvar)는 조사·설계 대상이 아니다.

상태 표기는 AssetSystem과 같다: **구현**(동작·시험), **계약**(API가 있고 미지원은 명시 오류), **후속**(API도 없음).

## 1. 결정 요약

| 항목 | 결정 | 근거 |
|---|---|---|
| 기준 버전 | Vulkan 1.1(instance·장치). 1.2 이상 core 기능은 쓰지 않거나 확장으로 고른다 | Android 기준 1.1(셰이더 형식 `VULKAN_SM5`의 `-fspv-target-env=vulkan1.1`). UE도 SM5 프로필이 1.1이다(`ThirdParty/Vulkan/profiles/VP_UE_desktop_vulkan.json:157–197`, `VP_UE_mobile_vulkan.json:55–94`) |
| 로더 | 시스템 `vulkan-1.dll`·`libvulkan.so`를 P10 `DynamicLibrary::OpenSystem`으로 연다. SDK import library를 링크하지 않는다(`VK_NO_PROTOTYPES`) | UE `P/Windows/VulkanWindowsPlatform.cpp:66`, `P/Android/VulkanAndroidPlatform.cpp:314` |
| 완료 추적 | 제출마다 `VkFence` 하나. timeline semaphore는 쓰지 않는다(4절) | 1.1 core에 없고 단일 graphics queue에서는 fence로 충분하다. UE도 timeline이 없으면 payload마다 fence다(`P/VulkanQueue.cpp:452–461`) |
| render pass | `VkRenderPass`·`VkFramebuffer`. 동적 렌더링은 쓰지 않는다 | 1.3 core다. UE는 동적 렌더링 세 기능이 모두 있을 때만 쓰고 나머지는 render pass 경로다(`P/VulkanExtensions.cpp:2232–2336`) |
| 메모리 | 자체 페이지 할당기. VMA를 쓰지 않는다 | UE도 자체 할당기다(`P/VulkanMemory.cpp:437–441, 2236–2251`) |
| 창 출력 | 중간 color 이미지에 그리고 `EndFrame`에서 swapchain 이미지를 얻어 blit 후 present | UE 기본(`r.Vulkan.DelayAcquireBackBuffer=1`, `P/VulkanDevice.cpp:86–93`, `P/VulkanViewport.cpp:745–756`). swapchain 형식(BGRA·RGBA)과 상관없이 파이프라인이 `RGBA8UnormSrgb` 하나로 호환된다 |
| 좌표·앞면 | 음수 viewport 높이와 `VK_FRONT_FACE_COUNTER_CLOCKWISE`·`Back → BACK_BIT`(D3D12와 같은 매핑). UE 매핑(`CLOCKWISE`·`CM_CW → FRONT_BIT`)은 컬링은 같지만 `SV_IsFrontFace`가 D3D12와 반대라 따르지 않는다(11절) | ShaderSystem 7.2절 8항, UE `P/VulkanPendingState.cpp:500–506`, `P/VulkanPipeline.cpp:936`, `P/VulkanState.cpp:186–188`, `Engine/Shaders/Private/MaterialTemplate.ush:4641–4648` |
| 셰이더 | 쿠킹된 `VULKAN_SM5` SPIR-V를 장치 셰이더(`Device::CreateShader`, `ShaderCodeKind::Spirv`)로 받고 파이프라인이 그 handle을 받는다. 코드와 값 1(바인딩 layout)은 셰이더 라이브러리 항목 그대로다. 장치는 실행 중 컴파일러를 갖지 않는다(13절) | ShaderSystem 16.1절 SSD18·SH2e2 메모, P4(Release 배제) |
| 검증 | `enableValidation`이면 `VK_LAYER_KHRONOS_validation`과 `VK_EXT_debug_utils` messenger. ERROR 심각도의 validation 메시지를 `ValidationErrorCount`가 센다(로더 GENERAL 메시지는 로그만) | UE `P/VulkanLayers.cpp:48,410`, `P/VulkanDebug.cpp:195–198`. UE는 세지 않고 로그만 남긴다 |

## 2. UE 5.8 VulkanRHI 조사

| 영역 | UE 5.8 | 근거 |
|---|---|---|
| 버전 | 플랫폼 기본 `VK_API_VERSION_1_1`. 실제 요청은 Vulkan Profiles 이름(`VP_UE_Vulkan_<FeatureLevel>`)의 API 버전이고 없으면 기본값 | `Pub/VulkanConfiguration.h:19`, `P/VulkanGenericPlatform.cpp:197–205`, `P/VulkanRHI.cpp:279–286, 615, 657` |
| 로더 | Windows `LoadLibraryW("vulkan-1.dll")`, Android `dlopen("libvulkan.so", RTLD_NOW \| RTLD_LOCAL)`. 기본 진입점 몇 개만 export로 얻고 나머지(장치 함수 포함)는 `vkGetInstanceProcAddr` | `P/Windows/VulkanWindowsPlatform.cpp:66–71, 115–127`, `P/Android/VulkanAndroidPlatform.cpp:314–375`, `P/VulkanLoader.h:171–176` |
| instance 확장 | 늘 `VK_KHR_surface`·`VK_EXT_swapchain_colorspace`, 플랫폼 surface, 디버그 빌드에서 `VK_EXT_debug_utils`. `get_physical_device_properties2`는 1.1 core 함수를 쓴다 | `P/VulkanExtensions.cpp:3025–3029`, `P/Windows/VulkanWindowsPlatform.cpp:163`, `P/Android/VulkanAndroidPlatform.cpp:452`, `P/VulkanRHI.cpp:166` |
| 검증 | `r.Vulkan.EnableValidation`(Debug 기본 2), 층 `VK_LAYER_KHRONOS_validation`. messenger는 수준별 심각도, 로그만 남기고 `VK_FALSE`, 메시지 ID별 중복 제거, 몇 ID 무시 | `P/VulkanLayers.cpp:23–48, 335–457`, `P/VulkanDebug.cpp:9–198` |
| 장치 선택 | 외장 → 내장 → CPU 순, CPU 장치는 `-AllowCPUDevices` 없으면 뺀다. `r.GraphicsAdapter`로 고정 | `P/VulkanRHI.cpp:202–263` |
| 큐 | 첫 GRAPHICS family, index 0. async compute·transfer는 조건부(1.1 기준에서는 쓰지 않는 조건) | `P/VulkanDevice.cpp:344–425`, `P/VulkanQueue.cpp:65,70` |
| 제출·완료 | `vkQueueSubmit`. timeline이 있으면 queue마다 timeline 하나를 모든 제출에 붙이고, 없으면 payload마다 fence와 `vkGetFenceStatus`·`vkWaitForFences` | `P/VulkanQueue.cpp:74–79, 430–548`, `P/VulkanSubmission.cpp:150–166` |
| 명령 버퍼 | queue·종류별 pool(`RESET_COMMAND_BUFFER_BIT`), 상태 기계, 완료 뒤 reset | `P/VulkanCommandBuffer.h:95–103`, `P/VulkanCommandBuffer.cpp:260–262, 404–434, 673` |
| 장치 손실 | `VK_ERROR_DEVICE_LOST`를 GPU 크래시로 보고 종료 | `P/VulkanUtil.cpp:409–470, 678–705` |
| swapchain | 이미지 3개 요청(min·max로 자름), vsync 끔이면 IMMEDIATE, 켬이면 MAILBOX 우선 후 FIFO. 이미지 형식은 늘 UNORM, 색 공간 SRGB_NONLINEAR. acquire semaphore는 이미지 수만큼 돌려 쓰고 렌더 완료 semaphore는 백버퍼마다. OUT_OF_DATE·SURFACE_LOST면 재생성 후 최대 4회 재시도, SUBOPTIMAL은 성공 | `P/VulkanSwapChain.cpp:239–549, 659–801, 860–870`, `P/VulkanViewport.cpp:110–111, 198–249, 449–452, 783`, `P/VulkanViewport.h:113` |
| 메모리 | 자체 할당기. GPU 페이지 128 MB, staging 32 MB, 64 MB 초과는 전용 할당. 이미지·버퍼는 다른 페이지(bufferImageGranularity 회피). upload `HOST_VISIBLE\|HOST_COHERENT`, readback `HOST_VISIBLE\|HOST_CACHED`(없으면 `HOST_VISIBLE`) | `P/VulkanMemory.cpp:40, 437–441, 2167–2251, 2772–2783, 5154–5174` |
| 지연 삭제 | 프레임 번호를 찍고 2프레임 뒤, 모든 queue의 GPU 동기점 뒤 해제 | `P/VulkanMemory.cpp:32–38, 4634–4742`, `P/VulkanSubmission.cpp:566–591` |
| 업로드 | 별도 upload context 명령 버퍼를 graphics queue에서 본 제출보다 먼저 낸다. UNDEFINED → TRANSFER_DST → 복사 → SHADER_READ_ONLY. 행은 빽빽하게(`bufferRowLength = 0`), `optimalBufferCopy*Alignment`는 쓰지 않는다 | `P/VulkanTexture.cpp:1320–1346, 1775`, `P/VulkanSubmission.cpp:245–254` |
| readback | HOST_CACHED staging 버퍼에 `vkCmdCopyImageToBuffer` | `P/VulkanRenderTarget.cpp:243–263, 366–389`, `P/VulkanMemory.cpp:4428–4506` |
| descriptor | 비-bindless는 형별 pool을 프레임 단위 묶음으로 쓰고 GPU 완료 뒤 `vkResetDescriptorPool`. push constant는 비-bindless layout에 없다 | `P/VulkanPendingState.cpp:30–319`, `P/VulkanSubmission.cpp:646–649`, `P/VulkanLayout.cpp:37–44` |
| render pass·PSO | 동적 렌더링이 없으면 RT 배치 hash로 render pass·framebuffer 캐시. `VkPipelineCache` 공유, PSO 파일 캐시 | `P/VulkanRenderTarget.cpp:637–680`, `P/VulkanRenderpass.cpp:52–107`, `P/VulkanPipeline.cpp:1945–1956, 2258–2276` |
| layout 전이 | 추적기 없이 전이마다 이전·이후 접근에서 layout을 정한다. 표본 color 이미지의 쉬는 layout은 `SHADER_READ_ONLY_OPTIMAL`. sync2가 있으면 `vkCmdPipelineBarrier2`, 없으면 `vkCmdPipelineBarrier` | `P/VulkanBarriers.cpp:197–229, 716, 1182–1238, 1632–1879` |
| timestamp | `vkCmdWriteTimestamp`, `64_BIT \| WITH_AVAILABILITY`로 polling(Adreno 때문에 WAIT 없음). `timestampPeriod > 0`일 때만 지원 | `P/VulkanCommandBuffer.cpp:177,196`, `P/VulkanSubmission.cpp:680–711`, `P/VulkanRHI.cpp:863` |
| calibration | `VK_EXT_calibrated_timestamps`의 `[DEVICE, 호스트 domain]`. Windows QPC, Android CLOCK_MONOTONIC. `maxDeviation`은 받고 버린다. 확장이 없으면 GPU 0·CPU 지금 | `P/VulkanDevice.cpp:1683–1733`, `P/Windows/VulkanWindowsPlatform.h:58`, `P/Android/VulkanAndroidPlatform.h:269`, `P/VulkanExtensions.cpp:1722–1749` |
| debug label | `vkCmdBegin/EndDebugUtilsLabelEXT`, 객체 이름 `vkSetDebugUtilsObjectNameEXT` | `P/VulkanDevice.cpp:577–706, 1658–1671`, `P/VulkanRHI.cpp:1353–1367` |
| 앞면·viewport | `frontFace = VK_FRONT_FACE_CLOCKWISE`, `CM_CW → VK_CULL_MODE_FRONT_BIT`, `CM_CCW → BACK_BIT`, viewport `y += height; height = -height` | `P/VulkanPipeline.cpp:936`, `P/VulkanState.cpp:186–188`, `P/VulkanPendingState.cpp:500–515` |
| 샘플러 | Point → NEAREST, 나머지 LINEAR. mip은 Trilinear·AnisoLinear만 LINEAR. 이방성은 장치 한계로 자르고 1이면 끔. border는 투명 검정·불투명 흰색 둘 | `P/VulkanState.cpp:16–57, 210–221` |

## 3. 로더·instance·장치

**로더(구현 위치 `Private/VulkanApi.*`).**
- Windows `OpenSystem("vulkan-1.dll")`(System32만), Android `OpenSystem("libvulkan.so")`. 열지 못하면 생성이 `Unsupported`이고 report의 stage는 `LoadVulkanLoader`, nativeCode는 OS 오류다. 다른 backend는 그대로 동작한다.
- export에서 `vkGetInstanceProcAddr` 하나만 얻는다. 전역 함수(`vkEnumerateInstanceVersion`·`vkEnumerateInstance*Properties`·`vkCreateInstance`)와 instance 함수는 그것으로, 장치 함수는 `vkGetDeviceProcAddr`로 얻는다(UE는 장치 함수도 instance로 얻는다. 장치 함수 포인터가 loader trampoline을 거치지 않는 쪽을 고른다). instance 확장의 명령(debug utils label)은 `vkGetInstanceProcAddr`로 얻는다.
- 함수 표는 장치 객체 안에 둔다. 장치가 여러 개면 각자 표를 가진다.
- 드라이버 상주(전역 상태는 이것 하나): 처음 성공한 로더 열기가 로더를 한 번 더 열고 확장·층 없는 `VkInstance` 하나를 만들어 프로세스 끝까지 둔다(파괴하지 않는 정적 저장소, 한 번만 시도). 로더는 `vkDestroyInstance`에서 ICD 라이브러리를 내리는데, NVIDIA 드라이버(nvoglv64.dll)는 장치 파괴 뒤에도 끝나지 않은 자체 스레드를 남기는 일이 있어 그 스레드가 내려간 코드를 실행하면 프로세스가 죽는다(cdb로 확인: `<Unloaded_nvoglv64.dll>` 접근 위반, 장치를 만들고 부수는 EngineTests에서 수십 번에 한 번). 이 instance가 ICD 참조를 쥐어 장치 instance를 파괴해도 ICD가 내려가지 않는다. UE는 프로세스에 RHI instance 하나를 두고 종료 때만 내리므로(`VulkanRHI.cpp:587–600`) 이 상황이 없다. Rocks는 한 프로세스가 장치를 여러 번 만들고 부순다(시험·self-test·backend 전환).

**instance.**
- `vkEnumerateInstanceVersion`이 없거나 1.1 미만이면 `Unsupported`(stage `vkEnumerateInstanceVersion`).
- `apiVersion = VK_API_VERSION_1_1`로 만든다. 장치 1.2·1.3 core 기능을 기대지 않으므로 더 높이지 않는다.
- 확장: `VK_KHR_surface`와 플랫폼 surface(`VK_KHR_win32_surface`·`VK_KHR_android_surface`)는 있으면 켠다. 없으면 장치는 만들고 `CreateSwapChain`만 `Unsupported`다(offscreen은 동작). `VK_EXT_debug_utils`는 있으면 늘 켠다(검증이 꺼져도 캡처 도구 label과 객체 이름에 쓴다).
- 검증: `enableValidation`이고 `VK_LAYER_KHRONOS_validation`이 열거되면 켠다. 없으면 켜지 않고 계속한다(`DeviceInfo::validationEnabled` false, D3D12 debug layer와 같은 규칙). messenger 생성 정보를 `VkInstanceCreateInfo::pNext`에도 걸어 instance 생성·파괴 중 메시지도 센다.

**물리 장치.**
- 조건: API 1.1 이상, GRAPHICS family가 있다.
- 순서: 외장 → 내장 → 가상 → CPU. `preferSoftware`면 CPU 장치만 고르고 없으면 `Unsupported`다(가짜 대체 없음). 하드웨어가 하나도 없으면 CPU 장치로 내려가고 `DeviceInfo::software`를 참으로 둔다(D3D12의 WARP 대체와 같은 규칙).
- 이름은 `deviceName`(UTF-8)이다.

**장치 기능과 확장.** 1.1 core만 요구한다. 선택 항목은 있을 때만 켜고 `DeviceInfo`나 동작으로 공개한다.

| 항목 | 종류 | 없을 때 |
|---|---|---|
| `VK_KHR_swapchain` | 장치 확장(선택) | `CreateSwapChain`이 `Unsupported` |
| `samplerAnisotropy` | 1.0 기능(선택) | 이방성 필터가 이방성 없이 trilinear로 동작한다(UE와 같이 장치 한계로 자른다. `maxAnisotropy`의 계약 "장치 한계로 줄인다"와 같다) |
| `VK_KHR_calibrated_timestamps` 또는 `VK_EXT_calibrated_timestamps` | 장치 확장(선택) | `clockCalibration` false, `CalibrateClocks`는 `Unsupported` |
| 음수 viewport 높이 | 1.1 core(maintenance1) | 해당 없음 |
| `VK_KHR_dedicated_allocation`·`get_memory_requirements2` | 1.1 core | 해당 없음(전용 할당 요구를 따른다) |
| timeline semaphore·sync2·동적 렌더링·host query reset | 1.2·1.3 core | 쓰지 않는다(1.1 기준 단일 경로) |

## 4. 큐·제출·완료·회수(GX7)

**큐.** GRAPHICS를 가진 첫 family의 index 0 하나다(`QueueKind::Graphics`). present 지원은 swapchain을 만들 때 `vkGetPhysicalDeviceSurfaceSupportKHR`로 보고, 지원하지 않으면 `Unsupported`다. async compute·copy queue는 RenderBase의 멀티 큐 단계에서 더한다.

**완료 추적.**
- 모든 `vkQueueSubmit`(프레임·업로드 묶음·readback)은 `VkFence` 하나를 신호하고 fence 값(1부터 증가)을 받는다. `WaitIdle`처럼 제출할 명령이 없을 때는 `submitCount = 0`과 fence로 "지금까지 제출한 모든 일의 완료"를 신호한다(사양 "vkQueueSubmit": 배치가 없어도 fence는 앞 제출의 완료 뒤 신호된다). D3D12 `Signal`과 같은 지점이 된다.
- fence는 pool에서 꺼내 쓴다. 소유 스레드는 비행 중 fence를 오래된 것부터 `vkGetFenceStatus`로 보며 완료 값을 올린다. fence 관찰 스레드(`FenceTimeline`)는 token으로 받은 fence를 `vkWaitForFences`로 기다린다. fence는 소유 스레드가 완료를 보았고 관찰 스레드가 token을 놓은 뒤에만 `vkResetFences`해 pool로 돌린다(같은 fence를 기다리는 동안 reset하지 않는다, 사양 "Fences"의 외부 동기화 규칙).
- timeline semaphore를 쓰지 않는 이유: 1.1 core가 아니고(Android 1.1 기기는 확장이 선택이다), 단일 graphics queue에서는 제출 순서와 fence가 같은 정보를 준다. 멀티 큐(async compute·copy)가 들어오면 queue 사이 대기 때문에 timeline이 필요하다. 그때 확장이 있으면 timeline, 없으면 지금 경로로 두 경로를 둔다.

**장치 손실.** `vkQueueSubmit`·`vkWaitForFences`·`vkGetFenceStatus`·acquire·present가 `VK_ERROR_DEVICE_LOST`를 돌려주면 `FenceTimeline::MarkLost`로 기다리는 쪽을 모두 `Destroyed`로 깨운다. 이후 `BeginFrame`·`EndFrame`은 `Destroyed`다. 손실은 Error 로그를 한 번 남긴다(LoggingSystem 13절). 사양상 손실 뒤에도 fence 대기·조회가 `VK_SUCCESS`일 수 있으므로 `TryReadback`·timestamp 회수는 먼저 손실 여부를 보고 손실이면 값을 읽지 않는다(`Destroyed`·disjoint).

**명령 버퍼.** pool 하나(`RESET_COMMAND_BUFFER_BIT`)에서 명령 버퍼 기록을 최대 8개 만들고, 마지막 제출의 fence가 끝난 것을 다시 쓴다(D3D12 기록 저장소와 같은 규칙과 상한). 모두 비행 중이면 소유 스레드는 가장 오래된 것을 기다리고, 다른 스레드(자원 생성·갱신)는 `Busy`다. 명령 버퍼와 pool의 외부 동기화는 장치 내부 잠금이 맡는다(모든 기록이 그 잠금 안이다).

**회수.** D3D12와 같다: `Retire`는 자원·view·descriptor set·메모리를 묶은 회수 항목을 남기고, 기록 중이면 그 프레임의 fence, 아니면 지금까지 제출한 마지막 fence, 아직 제출하지 않은 업로드 기록이 쓰면 그 묶음의 fence를 받는다. 실제 파괴는 소유 스레드의 `EndFrame`·`WaitIdle`·`ReclaimCompleted`가 한다. 회수 목록 용량은 자원을 만들 때 미리 확보한다(회수가 할당 실패로 GPU 사용 중 객체를 놓는 경로가 없다).

## 5. 메모리

**할당기(`Private/VulkanMemory.*`, 장부는 Graphics `GpuPageAllocator`).**
- 메모리 종류(memory type index)와 자원 모양(선형: 버퍼, 최적: 이미지)마다 페이지 목록을 둔다. 선형과 최적을 같은 페이지에 섞지 않으므로 `bufferImageGranularity`를 따로 맞추지 않는다(UE 버킷과 같은 방식).
- 페이지 크기는 장치 전용 64 MiB, 호스트 가시 16 MiB이고 힙 크기의 1/8을 넘지 않는다. 페이지 절반 이상의 요청과 `requiresDedicatedAllocation`·`prefersDedicatedAllocation` 자원은 전용 할당이다.
- 페이지 안 구간은 Graphics의 backend 공용 장부 `GpuPageAllocator`(first-fit, 해제 때 이웃 병합, 정렬 지원)가 나눈다. 장부는 Vulkan을 모르므로 CPU 시험으로 결정적으로 확인한다. 빈 페이지는 하나만 남기고 놓는다.
- 호스트 가시 페이지는 만들 때 한 번 map해 두고 구간 주소를 바로 준다.
- 종류 선택: 요구 bit를 모두 가진 첫 종류(UE와 같다). 정점·index 버퍼와 텍스처는 `DEVICE_LOCAL`, 업로드 ring·staging은 `HOST_VISIBLE | HOST_COHERENT`, readback은 `HOST_VISIBLE | HOST_CACHED`를 먼저 찾고 없으면 `HOST_VISIBLE | HOST_COHERENT`다. coherent가 아닌 readback은 읽기 전에 `nonCoherentAtomSize`로 맞춘 구간을 invalidate한다(이를 위해 그런 할당의 정렬과 크기를 atom 배수로 올린다).
- 메모리 예산 확장(`VK_EXT_memory_budget`)은 후속이다. 할당 실패(`VK_ERROR_OUT_OF_DEVICE_MEMORY`·`OUT_OF_HOST_MEMORY`)는 `OutOfMemory`다.

## 6. 자원과 descriptor

**버퍼.** `DEVICE_LOCAL` 메모리. Graphics `BufferUsage` 비트를 옮긴다: `Vertex`·`Index`·`Constant`(UNIFORM)·`Indirect`, 셰이더 용도는 구조화·raw면 `STORAGE_BUFFER`, 구조화가 아니면 읽기 `UNIFORM_TEXEL_BUFFER`·쓰기 `STORAGE_TEXEL_BUFFER`(raw 버퍼는 둘 다)이고 늘 `TRANSFER_SRC | TRANSFER_DST`(갱신·`ReadBuffer`)다. 초기 데이터와 갱신은 업로드 경로(8절)다.
- 버퍼 view(RenderBase GX2): typed는 `VkBufferView`, 구조화·raw는 (buffer, offset, range)다. typed view 형식은 버퍼의 모든 texel usage를 지원해야 한다(VUID-VkBufferViewCreateInfo-format-08778). view별 usage(`VkBufferUsageFlags2CreateInfo`)는 1.4 maintenance5라 쓰지 않고, 쓰기·atomic 형식 능력은 읽기도 되는 형식만 공개한다. view가 남은 버퍼를 `Retire`하면 버퍼·메모리를 orphan 목록으로 옮겨 마지막 view가 놓일 때 회수한다.
- 장치 능력(GX1): `VkPhysicalDeviceLimits`와 maintenance3 `maxMemoryAllocationSize`(1.1 core 속성)를 `DeviceCaps`로 옮긴다. 지원하면 `depthClamp`·`multiDrawIndirect`·`drawIndirectFirstInstance`·`imageCubeArray`·`vertexPipelineStoresAndAtomics`·`fragmentStoresAndAtomics`, `VK_EXT_shader_viewport_index_layer`, `VK_KHR_shader_atomic_int64` + `VK_EXT_shader_image_atomic_int64`(+ `shaderInt64`)을 켠다. MSAA 표본 수는 이미지 형식 질의 `sampleCounts`다.

**텍스처.** 2D `VkImage`, 형식 표(아래), `OPTIMAL` tiling.
- Sampled: 용도 `SAMPLED | TRANSFER_DST`. 쉬는 layout은 `SHADER_READ_ONLY_OPTIMAL`.
- RenderTarget: `SAMPLED | COLOR_ATTACHMENT | TRANSFER_SRC | TRANSFER_DST`(TRANSFER_DST는 `CopyPassTarget` 대상). 쉬는 layout도 `SHADER_READ_ONLY_OPTIMAL`(D3D12 PSR과 같다). 생성 때 업로드 기록에 UNDEFINED → 쉬는 layout 전이를 넣는다.
- 크기 상한은 min(16384, `maxImageDimension2D`)이다.
- 장치를 만들 때 1×1 흰색 Sampled 텍스처와 그 descriptor set을 만든다. 패스 시작과 `SetTexture(null)`·stale handle이 그것을 묶는다(D3D12와 같다).
- `ReadPixels`는 기록 중이면 `Busy`, RenderTarget이 아니거나 크기가 다르면 `InvalidArgument`다(D3D12와 같다). 비동기 readback과 같은 경로로 제출하고 그 fence를 기다린다.
- 형식 표(`Private/VulkanFormats.cpp`)는 `switch` 하나로 둔다. 대응 형식을 늘릴 때 행만 더하면 되도록 `PixelFormat` → (`VkFormat`, 필요한 형식 기능 bit)를 한 곳에서 정한다. 지금(PF1)은 아래 두 형식뿐이다. 장치 능력(`GetPixelFormatCaps`, PF3)은 생성 때 표의 형식마다 `vkGetPhysicalDeviceFormatProperties`의 optimal tiling 기능으로 정하고, 이 backend가 만들 수 있는 것(`Sampled`·`Mipmaps`·`Filterable`·`RenderTarget`·`Blendable`)만 공개한다(3D·cube·MSAA·storage·깊이는 기능이 있어도 공개하지 않는다, PF4). 텍스처·파이프라인 생성은 이 능력으로 판정하므로 "능력 있음 ⇔ 생성 성공"이다. 표에 없는 유효 형식과 능력 밖 용도(예: `DepthStencil`)는 생성이 `Unsupported`이며 `LastError`에 형식 이름을 남긴다(다른 형식으로 대체하지 않는다, PixelFormat.ko.md 8.3절). 업로드·readback 크기는 `PixelFormatImageBytes`·블록 크기로 계산한다. 형식을 넓힐 때의 Vulkan 주의점(BC1은 RGBA 블록, V8U8은 SNORM, A8은 R8 + swizzle)은 PixelFormat.ko.md 15·17절이다.

  | Rocks | Vulkan |
  |---|---|
  | `RGBA8Unorm` | `VK_FORMAT_R8G8B8A8_UNORM` |
  | `RGBA8UnormSrgb` | `VK_FORMAT_R8G8B8A8_SRGB` |

**샘플러.** `SamplerDesc` 조합마다 `VkSampler` 하나를 만들어 장치 수명 동안 공유한다(D3D12 샘플러 칸과 같은 규칙, 상한 256). Nearest: NEAREST·NEAREST·mip NEAREST, Bilinear: LINEAR·mip NEAREST, Trilinear: LINEAR·mip LINEAR, Anisotropic: Trilinear + `anisotropyEnable`(`maxAnisotropy`를 `maxSamplerAnisotropy`로 자르고 1이거나 기능이 없으면 끈다). 주소 Wrap → REPEAT, Clamp → CLAMP_TO_EDGE, Mirror → MIRRORED_REPEAT, Border → CLAMP_TO_BORDER. border 색은 세 고정 값이 Vulkan 내장 색과 일대일이다(`FLOAT_TRANSPARENT_BLACK`·`FLOAT_OPAQUE_BLACK`·`FLOAT_OPAQUE_WHITE`). `maxLod = VK_LOD_CLAMP_NONE`.

**descriptor.**
- 파이프라인 layout은 파이프라인마다 만든다(개정 6). 장치 셰이더의 값 1(자원 이름·종류, slot = Vulkan binding, 자원 모양(텍스처 차원·typed·구조화·raw), 원소 타입)을 파이프라인의 slot 이름 표(`PipelineDesc::bindings`)로 고정 slot에 잇고(`ResolvePipelineSlots`) set 0(VS·CS)·set 1(PS) layout을 만든다(ShaderSystem 7.2절 4항: set = 단계, binding = 값 1 slot). binding이 없는 단계는 장치 공용 빈 layout이다.
  - SRV 텍스처 → `SAMPLED_IMAGE`(SetTexture), 그 짝 샘플러 → `SAMPLER`, 버퍼 view(SetBufferView slot 0–3) → typed는 `UNIFORM_TEXEL_BUFFER`, 구조화·raw는 `STORAGE_BUFFER`, compute 쓰기 → typed는 `STORAGE_TEXEL_BUFFER`, 구조화·raw는 `STORAGE_BUFFER`, 텍스처는 `STORAGE_IMAGE`. 이름 표에 없는 자원(상수 버퍼 묶기는 GX6)은 생성이 `InvalidArgument`다.
  - 그리기별 상수 push constant 범위는 계약 최대 128 byte(VS·PS·CS, 7.2절 2항, RenderBase SX2)로 모든 파이프라인 layout과 공용 layout(두 set이 빈 layout)이 같다. 그래서 공용 layout으로 파이프라인이 선언한 n byte(`drawConstantBytes`)만 올리고, 파이프라인을 바꿔도 값과 descriptor set이 유지된다(사양 "Pipeline Layout Compatibility").
- 그리기 descriptor set은 그리기 직전(바뀐 것이 있을 때만) 지금 명령 버퍼 기록의 pool에서 받아 쓰고 파이프라인 layout으로 묶는다. pool은 기록마다 최대 8개이고 첫 pool이 set 1024개, 다음 pool은 앞 pool의 두 배다. 그 기록을 다시 쓸 때(앞 제출의 fence가 끝났다) `vkResetDescriptorPool`로 통째로 비운다. UE 비-bindless의 프레임 단위 pool과 같은 방식이다(`P/VulkanPendingState.cpp:30–319`). 텍스처마다 고정 set을 두던 개정 5 방식은 파이프라인마다 layout이 달라지면서 없앴다. 한 프레임에 descriptor를 바꾸는 그리기가 1024 × (2^8 − 1) = 261120번을 넘으면 그 뒤 그리기는 무시되고(`IgnoredDraws`, LastError) 처음 한 번 Warning 로그를 남긴다(GX1·GX2 리뷰 반영).

## 7. 파이프라인·render pass

- render pass는 (color 형식, load op CLEAR·LOAD) 조합마다 하나를 캐시한다. layout은 시작·끝 모두 `COLOR_ATTACHMENT_OPTIMAL`이고 전이는 바깥 barrier가 한다. 두 load op는 render pass 호환성 규칙(사양 "Render Pass Compatibility")상 호환이므로 파이프라인은 CLEAR 쪽으로 만들고 두 경우에 다 쓴다.
- framebuffer는 RenderTarget 텍스처와 swapchain 중간 이미지마다 하나다(자원과 함께 회수).
- 파이프라인 상태: 삼각형 목록, viewport·scissor 동적 상태, 깊이 없음, 블렌드는 D3D12와 같은 식, 래스터는 11절. 정점 입력은 `VertexLayout` 그대로(stream = binding, 인스턴스 stream은 `VK_VERTEX_INPUT_RATE_INSTANCE`, 간격 1)다.
- stride는 파이프라인에 들어가므로 파이프라인을 바꾸면 이미 묶은 정점 버퍼가 새 stride로 읽힌다(계약 "새 파이프라인 배치의 stride로 다시 묶인다"를 Vulkan이 그대로 만족한다).
- 정점 형식 대응은 RenderBase 10.4절 표의 Vulkan 열이다. 장치가 그 형식의 `VERTEX_BUFFER` 기능을 주지 않으면 파이프라인 생성이 `Unsupported`다(OpenGL의 `Bgra8Unorm`과 같은 공개 방식).
- 셰이더 입력 대조: 값 1의 정점 입력 마스크·attribute 타입(v3)으로 D3D12와 같은 규칙과 같은 진단 문구를 쓴다(`MatchShaderVertexInputs`). 이름 표 밖 자원이나 파이프라인이 선언한 `drawConstantBytes`보다 큰 루트 상수는 `InvalidArgument`다. 진입점 이름은 SPIR-V `OpEntryPoint`에서 읽는다(DXC가 HLSL 엔트리 이름을 그대로 둔다).
- `VkPipelineCache` 하나를 장치 안에서 공유한다(메모리 안, 파일 저장은 RenderBase 9.7절 파이프라인 캐시에서).
- index 버퍼 offset이 index 크기 배수가 아니거나 정점 버퍼 offset이 4 byte 배수가 아니면 묶지 않은 것으로 보고 그리기를 무시한다(`IgnoredDraws`). Vulkan은 index offset을 그 크기 배수로만 받고(`vkCmdBindIndexBuffer`), 속성 주소가 성분 크기 배수여야 한다(사양 "Vertex Input Address Calculation", 속성 offset은 이미 4 배수다). 이식 규칙으로 `Device.h`의 두 호출 주석에 적는다.

## 8. 이미지 layout과 동기화

- 명시 접근 상태와 전이(RenderBase GX5, `CommandContext::Transition`)를 따른다. 텍스처 mip마다 접근 상태를 추적한다(`committed`는 제출한 프레임 기록 끝, `current`는 기록 중 프레임 안). 제출하면 `current`를 `committed`로 삼고, 제출하지 못하면 되돌린다. D3D12와 같은 규칙이다.
- 접근 상태 → layout·단계·access(`Private/VulkanAccess.cpp`, UE `VulkanBarriers.cpp`의 `GetVkStageAndAccessFlags` 대응):
  - layout: 셰이더 읽기만 `SHADER_READ_ONLY_OPTIMAL`, `RenderTarget` `COLOR_ATTACHMENT_OPTIMAL`, `CopySource`·`CopyDestination` `TRANSFER_SRC·DST_OPTIMAL`, UAV와 서로 다른 읽기가 섞인 상태 `GENERAL`, 깊이 읽기·쓰기 `DEPTH_STENCIL_*`, `Unknown` `UNDEFINED`.
  - 추적 상태는 대표형이다(`CanonicalAccess`): D3D12 상태 대응에서 같은 상태가 되는 접근을 하나로 모은다. `SrvGraphics`는 compute 읽기를 포함하고(D3D12 `PIXEL | NON_PIXEL_SHADER_RESOURCE`), `VertexOrIndex`는 `Constant`를, UAV 비트 하나는 두 UAV 비트를 포함한다. 검증 장치의 `before` 비교도 대표형끼리 하므로 D3D12와 같은 기준이다.
  - 앞 상태는 쓰기 access만 원본 mask에 남긴다(읽기는 실행 의존으로 충분하다, WAR). barrier를 생략하는 것은 같은 layout에서 단계가 줄어드는 읽기 → 읽기뿐이다. 단계가 새로 생기는 읽기 → 읽기(예: 업로드 뒤 셰이더 읽기 상태 → 다른 단계 읽기)는 앞 쓰기가 새 단계에 보이도록 실행 의존을 잇는다(srcAccess 0, dstAccess는 새 읽기, barrier 사슬). 같은 상태 사이는 UAV면 생략 표시가 없을 때 쓰기 순서 barrier, 복사 대상처럼 attachment가 아닌 쓰기 상태면 WAW barrier다.
  - 렌더 타깃끼리의 순서(같은 대상이 렌더 타깃 상태로 남은 채 이어지는 pass, 풀에서 다시 쓴 대상, 전이로 닫았다 다시 연 LOAD pass)는 render pass의 바깥 의존(`VK_SUBPASS_EXTERNAL` → 0, color attachment 쓰기 → color attachment 읽기·쓰기)이 맡는다. 암묵 바깥 의존은 `TOP_OF_PIPE`·access 0이라 이 일을 하지 않는다.
  - 모든 텍스처는 `TRANSFER_SRC` usage를 가진다(readback·그래프 `CopySource`, D3D12도 허용한다). 렌더 타깃이 아닌 텍스처를 `RenderTarget`으로, 또는 아무 텍스처를 깊이 상태로 바꾸는 전이는 usage에 없는 layout이므로 무시하고 `IgnoredDraws`에 센다(깊이 텍스처는 GX4).
  - 버퍼에는 layout이 없다. 쓰기가 끼거나 단계가 새로 생기는 전이만 메모리 barrier로 낮추고(앞 상태가 `Unknown`·`Discard`면 모든 쓰기를 기다린다. UE도 `Discard`를 `Unknown`처럼 다룬다), 단계가 줄어드는 읽기 → 읽기는 아무것도 하지 않는다. D3D12가 버퍼 전이를 COMMON 암묵 승격에 맡기는 것과 달리 Vulkan에는 암묵 동기화가 없기 때문이다.
- render pass 안에서는 layout 전이를 할 수 없다(사양 "Render Pass" 의 barrier 제한). 그래서 `Transition`·`SetTexture` 암묵 전이·`CopyPassTarget`이 barrier를 내야 하면 열린 render pass를 닫고 barrier를 낸 뒤 LOAD render pass로 다시 연다. 묶인 파이프라인·descriptor·push constant·동적 상태는 명령 버퍼 상태라 그대로 남는다. 패스 대상 자신이 렌더 타깃 상태를 떠났으면 다시 열지 않고(그 사이 그리기는 무시된다), 대상이 렌더 타깃으로 돌아오는 전이에서 다시 연다.
  - `BeginOffscreen`은 대상 mip 0을 추적 상태에서 `RenderTarget`으로 바꾸고(attachment 전이), `EndFrame`은 offscreen 대상을 그대로 둔다. swapchain 중간 이미지는 추적 텍스처가 아니며 attachment ↔ blit 원본 layout을 오간다.
  - 생성: 업로드로 채운 텍스처와 RenderTarget 모두 셰이더 읽기 상태(`SrvGraphics`)로 시작한다(D3D12와 같다).
  - 업로드·readback 기록은 프레임 기록보다 먼저 실행되므로 상태 중립이다: `committed`에서 복사 상태로 바꿨다가 되돌린다(이미 그 상태면 barrier가 없다). `CopyPassTarget`은 원본·대상을 `current`에서 바꿨다가 되돌린다.
  - `SetTexture`가 그래픽 셰이더 읽기 상태가 아닌 텍스처(그래픽 읽기 비트가 없거나 layout이 `SHADER_READ_ONLY_OPTIMAL`이 아님)를 받으면 암묵 전이로 메운다(렌더 그래프를 거치지 않는 그리기). descriptor의 image layout을 `SHADER_READ_ONLY_OPTIMAL`로 썼기 때문이다. D3D12는 `PIXEL_SHADER_RESOURCE` 비트만 본다.
  - 검증 층을 켠 장치에서 `before`가 추적 상태와 다르면 `ValidationErrorCount`를 올리고 `LastError`에 남긴다(GPU에는 추적 상태를 쓴다).
- barrier API는 1.1 core `vkCmdPipelineBarrier`다(sync2는 1.3).
- 업로드 묶음 명령 버퍼는 시작에 "앞 제출의 읽기 → 전송 쓰기"(WAR) 실행 의존, 끝에 "전송 쓰기 → 정점·index·셰이더 읽기" 메모리 의존을 둔다. 같은 queue의 제출 순서가 뒤 프레임까지 의존을 잇는다(사양 "Execution and Memory Dependencies"의 submission order 범위).
- 한 묶음 안에서 같은 버퍼 구간을 두 번 쓰거나(초기 데이터 + 갱신, 갱신 두 번) 앞 복사가 쓴 구간을 원본으로 읽으면(초기 데이터 직후의 `ReadBuffer`) 두 복사는 순서가 없다. 그래서 묶음 안 버퍼 쓰기 구간을 기록하고 대상이나 원본이 겹치는 복사 앞에 TRANSFER_WRITE → TRANSFER_READ·WRITE barrier를 둔다(WAW·RAW, synchronization validation으로 RAW 경고가 사라지는 것을 확인했다). 이미지는 복사마다 layout 전이 barrier가 있어 해당 없다.

## 9. 창 출력(swapchain)

- surface: Windows `vkCreateWin32SurfaceKHR`(`NativeSurface::window` = HWND, `instance` = HINSTANCE), Android `vkCreateAndroidSurfaceKHR`(`window` = `ANativeWindow*`, P13 전에는 시험하지 않는 계약).
- swapchain: 이미지 3개 요청(min·max로 자름), 형식은 `B8G8R8A8_UNORM` → `R8G8B8A8_UNORM` → `A8B8G8R8_UNORM_PACK32` 순으로 `SRGB_NONLINEAR` 색 공간이며 `TRANSFER_DST` 용도가 있어야 한다. 없으면 `Unsupported`. present 모드는 vsync면 FIFO(필수 지원), 아니면 IMMEDIATE → MAILBOX → FIFO 순이다. composite alpha는 OPAQUE가 있으면 그것, 아니면 지원하는 첫 값.
- 패스 대상은 swapchain마다 하나인 중간 이미지(`R8G8B8A8_UNORM`, `MUTABLE_FORMAT`, 그리기 view `R8G8B8A8_SRGB`, 크기는 `SwapChainDesc`·`ResizeSwapChain`의 크기)다. swapchain 이미지 크기는 surface의 `currentExtent`(Win32는 창 client 크기로 고정)이며 다르면 blit이 선형 필터로 늘인다(DXGI의 늘이기와 같은 동작). 같은 크기면 값 그대로다. 그래서 `BeginFrame`의 패스 대상 형식은 `RGBA8UnormSrgb`로 RenderTarget 텍스처와 같고, 같은 파이프라인·`CopyPassTarget`(UNORM ↔ SRGB는 같은 크기 부류라 byte 복사) 규칙이 그대로다.
- `EndFrame`: render pass를 닫고 swapchain 이미지를 얻어(`vkAcquireNextImageKHR`, acquire semaphore) 중간 이미지를 blit(UNORM → UNORM이라 값 변환 없이 채널 순서만 맞춘다)하고 `PRESENT_SRC_KHR`로 바꾼다. 제출은 acquire semaphore를 TRANSFER 단계에서 기다리고 그 이미지의 렌더 완료 semaphore를 신호하며, present는 그것을 기다린다. swapchain 이미지의 UNDEFINED → TRANSFER_DST barrier는 `srcStageMask = TRANSFER`로 semaphore 대기 단계와 사슬을 잇는다(TOP_OF_PIPE면 acquire보다 먼저 전이될 수 있다). 중간 이미지는 다음 프레임 전이 때 `srcStageMask`에 TRANSFER를 넣어 blit 읽기와의 WAR를 막는다.
- 렌더 완료 semaphore는 swapchain 이미지마다 둔다(같은 이미지를 다시 얻기 전에는 앞 present가 그것을 다 쓴 뒤다). acquire semaphore는 pool에서 꺼내고 그것을 기다린 제출의 fence 뒤 돌려준다. acquire는 성공했는데 제출이 실패하면 그 semaphore는 신호된 채 대기자가 없으므로 pool에 돌리지 않고 다음 idle 뒤 파괴한다.
- present 수명: `vkQueuePresentKHR`는 fence를 받지 않으므로 제출 fence가 끝나도 present의 semaphore 대기가 끝났다는 보장이 없다. 그래서 swapchain·그 semaphore·중간 이미지를 파괴하는 곳(`ResizeSwapChain`, `OUT_OF_DATE`·`SURFACE_LOST` 재생성, `Retire`한 swapchain의 회수)은 직전에 `vkQueueWaitIdle`로 present까지 끝낸다(UE도 `WaitUntilIdle` 뒤 파괴한다, `P/VulkanSwapChain.cpp:666–677`). `VK_EXT_swapchain_maintenance1`의 present fence는 후속이다.
- `OUT_OF_DATE`(acquire·present)는 swapchain을 다시 만들고 acquire를 한 번 더 한다. `SUBOPTIMAL`은 성공으로 본다. `SURFACE_LOST`면 이전 swapchain과 surface를 먼저 파괴하고(같은 창에 겹쳐 만들면 `NATIVE_WINDOW_IN_USE`) surface부터 다시 만든다. 그래도 실패하면 present 없이 제출하고 `DependencyFailed`(present 실패)를 돌려준다(계약: 완료 지점은 `LastSubmittedFence`).
- `ResizeSwapChain`은 GPU idle 뒤 중간 이미지와 swapchain을 다시 만든다(`oldSwapchain`으로 넘김). 기록 중 `Retire`하면 그 프레임의 present만 생략한다.

## 10. 업로드·readback(GX8)

- 업로드 ring: `HOST_VISIBLE | HOST_COHERENT` 버퍼 하나(기본 4 MiB)를 계속 map해 두고 `UploadRingAllocator`로 나눈다. 꽉 차면 D3D12와 같은 순서(소유 스레드: 쌓인 업로드 제출 → 가장 오래된 묶음 대기, 다른 스레드: 전용 staging)다.
- 업로드 기록은 별도 명령 버퍼다. 소유 스레드의 다음 제출(`EndFrame`·`WaitIdle`·`SubmitUploads`·`ReadPixels`·`RequestReadback`)이 그것을 먼저 제출한다. 그래서 갱신 뒤 기록한 그리기, 기록 중 갱신이면 그 프레임 전체가 새 내용을 본다.
- 텍스처 복사는 빽빽한 행(`bufferRowLength = 0`)이다. Vulkan은 원본 offset이 texel 크기(4) 배수이기만 하면 되므로 D3D12의 256 byte 행 정렬 재배치가 없다. staging에서 올릴 때 offset이 4의 배수가 아니면 ring으로 옮겨 올린다.
- staging pool: `StagingPool`에 `HOST_VISIBLE | HOST_COHERENT` 버퍼(`TRANSFER_SRC`)를 대 준다. IO가 바로 쓸 수 있는 고정 주소다.
- readback: `HOST_CACHED` 우선 버퍼에 `vkCmdCopyImageToBuffer`(빽빽한 행), 버퍼에 `TRANSFER_WRITE → HOST_READ` barrier, 곧바로 제출한다. `TryReadback`은 fence가 끝났을 때만 (필요하면 invalidate 뒤) 복사한다.

### 10.1 파이프라인·패스·명령 기록 확장(GX3·GX4)

- render pass는 열쇠(색 형식·load·store 0–8, 깊이 형식·깊이/스텐실 load·store, 읽기 전용 깊이)마다 캐시한다. layout은 시작·끝 모두 attachment layout(색 `COLOR_ATTACHMENT_OPTIMAL`, 깊이 `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`·읽기 전용 `DEPTH_STENCIL_READ_ONLY_OPTIMAL`)이고 전이는 바깥 barrier(GX5)가 한다. 바깥 의존은 색·깊이 attachment 쓰기 → 읽기·쓰기다. 파이프라인은 같은 형식의 호환 render pass로 만든다(load·store·layout은 호환성에 들지 않는다).
- framebuffer는 attachment view 묶음(크기 포함)으로 캐시하고, attachment 텍스처를 놓을 때 그 view를 쓰는 framebuffer를 회수 목록으로 넘긴다(텍스처마다 framebuffer를 두던 방식을 없앴다). 스텐실이 있는 깊이 타깃은 깊이·스텐실 aspect 둘의 attachment view를 따로 만든다(기본 view는 깊이 aspect만 읽는다).
- 전이·dispatch·복사로 render pass를 닫으면 모든 attachment를 LOAD로 다시 연다. attachment가 attachment 상태를 떠났으면 열지 않는다.
- 파이프라인 상태: 타깃별 블렌드(`independentBlend`가 없으면 같아야 한다), alpha-to-coverage, 깊이 bias(`depthBiasClamp` 기능), `depthClampEnable`, 깊이·스텐실 상태, 기본도형, 동적 viewport·scissor·스텐실 기준. `SetViewport`는 음수 높이로 뒤집는다.
- compute: `ComputePipelineDesc::computeShader`(장치 셰이더)로 만들고 set 0에 값 1의 읽기(버퍼 view·텍스처)·쓰기(storage buffer·storage texel buffer·storage image) binding을 둔다. 모든 파이프라인 layout의 push constant 범위는 VS·PS·CS다. dispatch는 render pass 밖에서 기록하고 descriptor set은 compute bind point에 묶는다. 프레임 descriptor pool에 storage texel buffer 크기를 더했다.
- 버퍼 쓰기 명령(dispatch·`vkCmdCopyBuffer`·UAV clear의 `vkCmdFillBuffer`) 앞에는 앞선 읽기와의 실행 의존(WAR), 뒤에는 모든 버퍼 읽기·쓰기 단계(간접 인자·정점·index·셰이더·전송)로 가는 메모리 barrier를 둔다. 렌더 그래프 밖 기록의 보수적 순서이고 렌더 그래프가 버퍼 전이를 내면 좁힐 수 있다.
- `CopyTexture`는 두 mip 0을 복사 원본·대상 상태로 바꿔 `vkCmdCopyImage`(이미지 aspect 그대로)다.

## 11. 좌표·앞면·컬(C7, ShaderSystem 7.2절 8항)

- viewport는 `y = 높이`, `height = -높이`다(D3D와 같은 NDC y 위를 화면 위로). 깊이 범위 [0, 1]은 Vulkan 기본이다.
- 셰이더는 쿠킹 때 `-fvk-use-dx-position-w`로 컴파일된다(`VULKAN_SM5` 형식 정의, ShaderSystem 7.2절 8항).
- 행렬은 `-Zpr`(행 우선)이고 push constant의 `transform`은 Core `Mat4` 저장 순서 그대로다(전치 없음).
- 앞면·컬: `frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE`, `CullMode::Back` → `VK_CULL_MODE_BACK_BIT`, `CullMode::Front` → `VK_CULL_MODE_FRONT_BIT`, `None` → `VK_CULL_MODE_NONE`이다(D3D12 `FrontCounterClockwise = TRUE`와 같은 모양).
  - 근거: 사양 "Basic Polygon Rasterization"의 면적 a = −½Σ(x_f^i·y_f^{i+1} − x_f^{i+1}·y_f^i)는 framebuffer 좌표(y 아래)로 잰다. 음수 viewport 높이로 clip y = +1이 framebuffer 위쪽이 되므로 framebuffer는 표시 화면과 같고, 화면 반시계 삼각형은 a > 0, 곧 Vulkan의 "반시계"다.
  - UE는 `frontFace = VK_FRONT_FACE_CLOCKWISE`와 `CM_CW → VK_CULL_MODE_FRONT_BIT`를 쓴다(`P/VulkanPipeline.cpp:936`, `P/VulkanState.cpp:186–188`). 화면 시계 삼각형을 버리므로 컬링 결과는 같지만, 앞면이 화면 시계가 되어 `SV_IsFrontFace`가 D3D12와 반대다. UE는 이것을 머티리얼 셰이더에서 Vulkan일 때 부호를 뒤집어 보정한다(`Engine/Shaders/Private/MaterialTemplate.ush:4641–4648` "We need to flip SV_IsFrontFace for Vulkan when compiling with DXC"). ShaderSystem 7.2절 8항은 셰이더에 backend 분기를 두지 않으므로 Rocks는 장치 쪽 매핑으로 맞춘다.
  - 확인: Sandbox `RunCullingChecks`(창·offscreen의 None·Back·Front, 음수 행렬식 반전)와 `SV_IsFrontFace`로 색을 고르는 대조(모든 backend)를 D3D12와 픽셀 대조한다(측정 S5의 Vulkan 몫).

## 12. 계측·진단(GX9)

- timestamp: query pool 하나(slot 4 × 256), `vkCmdWriteTimestamp(BOTTOM_OF_PIPE)`. 쓸 수 있는 조건은 graphics family의 `timestampValidBits > 0`과 `timestampPeriod > 0`이다. 결과는 fence 완료 뒤 `vkGetQueryPoolResults(64_BIT)`(WAIT 없음)로 읽고 `NOT_READY`면 다음 회수로 미룬다. 값은 `timestampValidBits`로 자른다.
- query reset: render pass 안에서는 reset할 수 없고 host reset은 1.2라 쓰지 않는다. 그래서 slot 회수(Harvest)를 기록 밖에서만 한다(`BeginFrame`·`BeginOffscreen`의 render pass 앞, 기록 중이 아닐 때의 `CollectTimestamps`). 회수한 slot에 "reset 필요" 표시를 두고, 다음 프레임 명령 버퍼의 render pass 앞에서 표시된 slot을 `vkCmdResetQueryPool`한다. 표시는 그 프레임 제출이 성공했을 때만 지운다. 처음에는 모든 slot이 표시된 상태다. 기록 중 `WriteTimestamp`는 회수하지 않으므로 기록 중에 slot이 비워져 reset 없이 다시 쓰이는 경로가 없다. 업로드 통계(`UploadStats::pending`·`SubmitUploads`)와 섞이지 않는다.
- 주파수: `1e9 / timestampPeriod`를 가장 가까운 정수 Hz로 공개한다(`DeviceInfo::timestampFrequency`는 정수다). period가 정수 ns가 아닌 장치는 반올림만큼 변환 오차가 생긴다. disjoint 감지는 Vulkan에 없으므로 `timestampDisjointDetection` false다.
- calibration: `VK_KHR_calibrated_timestamps`(없으면 EXT)의 `[DEVICE, 호스트]`. 호스트 domain은 Windows `QUERY_PERFORMANCE_COUNTER`(PlatformBase `MonotonicTicks`와 같은 QPC), Android `CLOCK_MONOTONIC`(같은 원천, ns)이다. 두 domain을 장치가 주지 않으면 `clockCalibration` false다. `uncertaintyNanoseconds`는 드라이버가 준 `maxDeviation`(ns)이다(UE는 버린다. Rocks는 오차를 공개한다).
- debug marker: label 함수는 instance 확장 명령이라 `vkGetInstanceProcAddr`로 얻는다. `VK_EXT_debug_utils`가 켜져 있으면 `vkCmdBegin/EndDebugUtilsLabelEXT`(UTF-8 그대로, `MaxDebugMarkerNameBytes`로 자름)이고 `debugMarkers` 참이다. 없으면 거짓이고 두 호출은 아무것도 하지 않는다.
- 검증 messenger: ERROR 심각도의 validation 종류(`VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT`) 메시지만 원자 계수로 센다(`ValidationErrorCount`). 로더·드라이버의 GENERAL 메시지(예: 레지스트리에 남은 implicit layer JSON을 열지 못함)는 API 사용 오류가 아니므로 로그로만 남긴다(2026-11-18, Epic overlay layer 항목이 남은 기계에서 확인). 메시지는 `Graphics.Vulkan.Validation` 범주(`LogGraphicsVulkanValidation`, 장치 생성·손실은 `LogGraphicsVulkan`, 둘 다 `Graphics/GraphicsLog.h`)에 Warning으로 메시지 ID마다 한 번 남긴다(LoggingSystem 13절). 콜백은 드라이버 스레드에서도 불리므로 직렬화를 전제하는 `ValidationMessageFilter` 대신 같은 규칙(표 `MaxLoggedValidationIds`가 차면 새 ID는 남기지 않고 개수만 센다)을 잠금 없이 원자 연산과 고정 크기 ID 표로 지킨다.
- 실패 보존: 장치 안 실패는 `LastError`에 "단계: VkResult 이름(값)"을 적는다. 생성 실패는 `DeviceCreateReport`(code·nativeCode = `VkResult`·stage·detail)로 돌려준다.

## 13. 스레드 모델(GX10·GX11)·셰이더 경로

**스레드.** D3D12와 같다. `concurrentResourceCreation`은 참이다: 생성은 Vulkan 객체 생성(스레드 안전)과 내부 잠금 아래 업로드 기록이고, queue·명령 버퍼·pool은 잠금 안에서만 만진다. 제출·present·fence 신호는 소유 스레드만 한다. Vulkan 객체는 스레드에 묶이지 않으므로 소유권 이관은 검사만 바꾼다.

**셰이더(SH2e2 뒤, 2026-11-24).**
- `CreateShader`는 코드 종류(`Spirv`)·4 byte 배수·값 1 decode와 단계를 검사하고 코드·값 1의 CPU 사본을 둔다. `VkShaderModule`은 파이프라인을 만들 때 만들고 만든 뒤 버린다(파이프라인이 module을 붙잡지 않는다). `Retire(ShaderHandle)`은 바로 지운다(GPU가 쓰지 않는다 — ShaderSystem 16.1절 SH2e2 메모 "장치 셰이더는 CPU 사본이다").
- SPIR-V는 셰이더 워커의 Developer `ShaderFormatVulkan`(`VULKAN_SM5`)이 쿠킹 때 만든다. DXC `-spirv -fspv-target-env=vulkan1.1 -fvk-use-dx-position-w -Zpr -fspv-reflect`로 컴파일하고 정점 입력·단계 사이 location(`ATTRIBUTEn`·`TEXCOORDn`)과 set·binding(set = 단계, binding = 종류 순서)을 다시 매긴다. 이어서 `SPV_GOOGLE_*`를 지우고 `spirv-val`(vulkan1.1)로 검증한 뒤 SPIRV-Reflect로 값 1을 만든다(ShaderSystem 16.1절 SSD29·SSD33). 그리기별 상수는 원문의 `ROCKS_ROOT_CONSTANTS`(push constant)다.
- Graphics·장치에는 HLSL 원문(`ShaderSource`)과 개발용 컴파일 훅(`ShaderSourceCompiler`·`DeviceDesc::shaderCompiler`)이 없다. SH2e2에서 Graphics 쪽 훅(`VulkanShaderCompiler`·`SpirvRewriteGraphics`·`DxcSpirv`)과 함께 지웠다.
- Sandbox는 개발 구성에서 Development 공급자가 `VULKAN_SM5` 메모리 라이브러리를 만들어 장치마다 셰이더 집합을 연다(RenderBase 9.2절). Release Sandbox에는 공급자가 없어 장치 생성까지만 되고 그리기 self-test는 NOT RUN이다.

## 14. 공개하는 제약(가짜 값 없음)

| 경우 | 공개 |
|---|---|
| 로더·1.1 instance·GRAPHICS queue가 없다 | 생성 `Unsupported`(report stage·`VkResult`) |
| surface·swapchain 확장이나 present 지원이 없다 | `CreateSwapChain` `Unsupported` |
| `preferSoftware`인데 CPU 장치가 없다 | 생성 `Unsupported` |
| 정점 형식의 `VERTEX_BUFFER` 기능이 없다 | `CreatePipeline` `Unsupported` |
| timestamp 불가(`timestampValidBits` 0) | `timestamps` false, `WriteTimestamp` `Unsupported` |
| calibration 확장·domain이 없다 | `clockCalibration` false, `CalibrateClocks` `Unsupported` |
| debug utils가 없다 | `debugMarkers` false |
| 코드 종류가 `Spirv`가 아니거나 값 1이 단계와 맞지 않는 장치 셰이더 | `CreateShader` `InvalidArgument` |

## 15. UE 대응표

| Rocks | UE 5.8 VulkanRHI |
|---|---|
| `VulkanGraphics` 모듈, `CreateVulkanDevice` | `VulkanRHI`, `FVulkanDynamicRHI` |
| 함수 표(`VulkanFunctions`) | `VulkanDynamicAPI`(`P/VulkanLoader.h`) |
| 제출 fence pool·`FenceTimeline` | `FVulkanFence`·`FVulkanSyncPoint`·interrupt thread |
| 명령 버퍼 기록(최대 8) | `FVulkanCommandBufferPool`·`FVulkanCmdBuffer` |
| 메모리 페이지·`GpuPageAllocator` | `FMemoryManager`·`FVulkanResourceHeap`·`FDeviceMemoryManager` |
| 업로드 기록 명령 버퍼 | `FVulkanUploadContext` |
| 업로드 ring·staging pool | `FStagingManager` |
| 텍스처별 descriptor set | `FVulkanDescriptorPoolSetContainer`(프레임 단위) |
| render pass·framebuffer 캐시 | `FVulkanRenderPassManager` |
| 중간 이미지 + blit present | `DelayAcquireBackBuffer` 경로(`FVulkanViewport`) |
| 검증 messenger 계수 | `DebugUtilsCallback`(로그만) |
| `CalibrateClocks` | `FVulkanDevice::GetCalibrationTimestamp` |
| 장치 셰이더(`CreateShader`, CPU 사본)·`ShaderFormatVulkan`(쿠킹) | `FVulkanShader`·`VulkanShaderFormat` |

## 16. 시험과 측정

- Graphics `GpuPageAllocator` CPU 시험(EngineTests): 정렬, first-fit, 이웃 병합, 가득 참, 빈 페이지 판정.
- 셰이더 워커 시험(EngineTests `--filter Shader`, DXC·SPIRV-Tools가 저장소에 없으면 SKIP): 쿠킹 경로의 location 재작성·push constant·binding 재배치·`SPV_GOOGLE` 제거·`spirv-val` 통과·잘못된 시맨틱 거부(ShaderSystem 16.1절). 장치 쪽은 `ShaderBindings_*`(장치 셰이더 검사·이름 표·정점 입력 타입)와 `VulkanDevice_*`의 코드 종류 거부다.
- Sandbox self-test(`--self-test`): D3D12가 통과하는 모든 검사(픽셀·GX7 fence·GX10/11 소유권·swapchain·C7 컬링(창·offscreen)·GX3a 정점 형식 20종·GX8 업로드·GX9 계측·렌더 lane)를 Vulkan 하드웨어에서 돌리고 D3D12와 픽셀 대조(채널 ±8·2%)한다. 검증 오류 집계는 형식이 다른 render pass에 파이프라인을 쓰는 의도적 오류로 확인한다.
- 측정 S4(ShaderSystem 5.4절): 검증 층 공급과 GPU 없는 환경의 소프트웨어 ICD. 결과는 17절.

## 17. 측정 결과

2026-11-17, Windows 11, Debug 구성. 도구: Vulkan SDK 1.4.363.0(LunarG, winget `KhronosGroup.VulkanSDK`, `C:\VulkanSDK\1.4.363.0`, `VK_LAYER_KHRONOS_validation` 포함), 시스템 로더 1.3.280, DXC 1.9.2609·SPIRV-Tools(사용자 도구 저장소).

| 대상 | 결과 |
|---|---|
| GTX 1060 3GB(NVIDIA 560.94, Vulkan 1.3.280) | Sandbox `--self-test` 전체 통과: 픽셀·GX7 fence·GX10/11 소유권·swapchain·C7 컬링(창·offscreen, None·Back·Front, 음수 행렬식, 행 우선 행렬)·`SV_IsFrontFace` 대조·GX3a 정점 형식 20종·GX8 업로드·GX9 timestamp·calibration·marker·렌더 lane. D3D12와 픽셀 대조 차이 0 채널. 검증 오류 0, 의도한 render pass 형식 불일치는 `VUID-vkCmdDraw-renderPass-02684`로 집계된다 |
| lavapipe(Mesa 26.2.7 `mesa-dist-win` release-msvc, llvmpipe LLVM 23.1.5, `VK_ADD_DRIVER_FILES`로 추가) | 같은 검사 전체 통과, D3D12와 차이 0 채널. calibrated timestamps가 PlatformBase 시계 domain(QPC)을 주지 않아 `clockCalibration` false이고 `CalibrateClocks`는 `Unsupported`(가짜 쌍 없음) |
| GX5 전이·렌더 그래프 RG1(8절, origin 병합 뒤) | 두 장치에서 Sandbox 렌더 그래프 self-test(그래프 텍스처 → SRV 합성, culling, history export → import, 명시 전이)가 검증 오류 0(before 불일치 포함)으로 통과하고 풀 생성 2·재사용 1이다. 전체 self-test 대상 D3D12·WARP·D3D11·D3D11 WARP·OpenGL ES 두 context·Vulkan 두 장치가 모두 통과하고 D3D12 vs Vulkan·Vulkan software 픽셀 차이 0이다 |
| 동기화 검증(`VK_LAYER_ENABLES`의 synchronization validation) | 두 장치 모두 `SYNC-HAZARD` 0건(GX5 전이 뒤에도 0건) |
| Android 교차 빌드(arm64, NDK 29, `-p:RocksPlatform=android`, EngineTests) | VulkanGraphics Android 구현(`libvulkan.so`, `VK_KHR_android_surface`, CLOCK_MONOTONIC domain)과 ShaderFormatVulkan 미지원 구현이 경고 0으로 컴파일된다. 기기 실행은 SH6·P13이다 |
| EngineTests | `GpuPageAllocator` 3건, `VulkanShaderCompiler` 2건(DXC·SPIRV-Tools 실제 호출 — SH2e2에서 컴파일 훅과 함께 지웠다), `VulkanDevice` 1건(로더 동적 로드·검증 층·빈 제출 fence) 통과, 전체 2135건 통과 |
| 드라이버 언로드 경쟁(3절 드라이버 상주) | 상주 전: EngineTests 전체가 간헐적으로 비정상 종료(하네스 2회, 직접 실행 6회 중 1회). cdb에서 `--filter Vulkan`을 반복해 10번째에 잡았다. 장치 수명 동안 NVIDIA가 만든 스레드 7개 중 하나가 `vkDestroyInstance`의 nvoglv64.dll 언로드 뒤에도 남아 `<Unloaded_nvoglv64.dll>+0xe76052`에서 접근 위반. 우리 fence watcher 스레드는 그 전에 끝났다. 상주 뒤: cdb 로그에 프로세스 끝까지 nvoglv64.dll·vulkan-1.dll 언로드 이벤트가 없고(결정적 확인), `--filter Vulkan` 직접 60회·cdb 20회와 EngineTests 전체 직접 8회가 모두 통과 |
| lavapipe 창 경로와 데스크톱 세션(2026-11-18) | 사용자가 원격 세션을 끊은 뒤(session 1 `Disc`) lavapipe의 swapchain·컬링 창 경로·렌더 lane 검사가 `VK_ERROR_MEMORY_MAP_FAILED`로 실패했다. 원인: mesa 26.2.7 `src/vulkan/wsi/wsi_common_win32.cpp`의 소프트웨어 present는 `GetDC(hwnd)`로 얻은 창 DC에 `StretchBlt`하고 실패하면 이 오류를 swapchain 상태로 남긴다(823–832행, 뒤 acquire도 같은 오류, 736행). 같은 경로를 흉내 낸 GDI 프로브: 끊긴 세션에서 숨긴 창으로의 `StretchBlt`는 FALSE, 보이는 창은 TRUE. 우리 코드와 무관함: 23:59에 통과한 리뷰 수정·드라이버 상주 이전 코드를 같은 끊긴 세션에서 다시 빌드해 돌리면 같은 항목이 같은 오류로 실패한다. 하드웨어 Vulkan·D3D12·D3D11·OpenGL ES의 창 경로는 끊긴 세션에서도 통과한다. 처리: `platform::QueryDesktopSessionState()`(PlatformWindow, WTSConnectState)가 `Disconnected`이고 장치가 lavapipe(소프트웨어, 이름 `llvmpipe`)면 Sandbox가 그 창 경로 검사를 근거를 단 NOT RUN으로 출력한다(`Programs/Sandbox/Private/SelfTestEnvironment.*`). 연결된 세션에서는 전부 돌린다 |
| best practices 검증 | 열거를 두 번 부르기로 고친 뒤 남는 경고는 RenderTarget 생성 때 UNDEFINED → SHADER_READ_ONLY 전이 하나다(D3D12도 생성 직후 내용이 정해지지 않는다. 오류로 세지 않는다) |

**측정 S4(검증 층 공급·소프트웨어 ICD).**
- 검증 층은 LunarG Vulkan SDK 설치로 공급한다(로컬). CI 공급(Khronos Vulkan-ValidationLayers 릴리스를 binary 항목으로 받는 것)은 SH2 때 정한다. 층이 없으면 장치는 검증 없이 만들어지고 `validationEnabled` false다.
- 소프트웨어 ICD는 lavapipe로 정한다. Windows 바이너리는 `mesa-dist-win`(pal1000) 릴리스이며 `lvp_icd.x86_64.json`을 `VK_ADD_DRIVER_FILES`로 더하면 `preferSoftware` 장치로 잡힌다. 등록하지 않은 기계에서는 "Vulkan software"가 NOT RUN이다. SwiftShader는 Windows 공식 바이너리가 없어 쓰지 않았다.

**앞면 매핑(측정 S5의 Vulkan 몫).** 11절 매핑에서 컬링 대조와 `SV_IsFrontFace` 대조가 모두 D3D12와 같다. 음성 대조로 UE 매핑(`VK_FRONT_FACE_CLOCKWISE`, `Back → FRONT_BIT`, `Front → BACK_BIT`)으로 바꿔 돌리면 컬링 대조(창·offscreen)와 D3D12 픽셀 대조는 그대로 통과하고 `SV_IsFrontFace` 대조만 실패한다(화면 반시계 삼각형이 뒷면으로 나온다). 그래서 컬링만 보는 시험으로는 이 차이를 잡지 못한다.

## 18. 남은 일

- GX6: 상수 버퍼 묶기(파라미터 묶음). 지금 장치는 루트 상수(push constant)와 이름 표의 고정 slot만 묶는다. compute 파이프라인 캐시는 GX3다.
- SH6·P13: Android 실행(surface 수명, `ANativeWindow`, 회전 `preTransform`).
- 멀티 큐(async compute·copy)와 timeline semaphore 경로, 동적 렌더링·sync2 선택 경로(장치가 주면).
- 파이프라인 캐시 파일 저장(RenderBase 9.7절), 메모리 예산 확장, 객체 debug 이름.
- 픽셀 형식 개편(`PixelFormat`)의 형식 표 확장.
- 분리 전이(begin·end, RG2)와 pipe 구분(RG4): 지금 `Transition`은 한 지점 barrier다. 버퍼 전이는 쓰기가 끼는 경우의 전역 메모리 barrier라 범위를 좁히지 않는다.
- `VK_KHR_image_format_list`(1.2 core)로 중간 이미지의 `MUTABLE_FORMAT` 대상 형식을 UNORM·SRGB 둘로 알려 압축(DCC 등)을 살리는 것.
- `Draw(firstVertex)`가 0이 아닐 때 `SV_VertexID`에 firstVertex가 들어간다(Vulkan `VertexIndex`). base vertex를 도입할 때 ShaderSystem 7.2절 7항 규칙(루트 상수로 넘김)을 따른다.

## 19. 개정 이력

- 개정 1(2026-11-17): 초안. P9 설계, UE 5.8 VulkanRHI 조사, Graphics 계약(GX3a·GX7–GX11·C7)의 Vulkan 대응. 교차 검토 반영: 앞면은 UE 매핑 대신 D3D12와 같은 매핑(`SV_IsFrontFace` 일치), present 수명(`vkQueueWaitIdle`), swapchain 크기와 중간 이미지 크기 분리, 업로드 묶음 안 WAW barrier, query reset을 기록 밖 회수와 프레임 앞 reset으로, 손실 뒤 값 소비 금지, 정점·index offset 정렬.
- 개정 2(2026-11-17): GX5 명시 접근 상태와 전이(RenderBase 개정 9–11, RG1)를 8절에 반영했다: mip별 `committed`·`current` 추적과 대표형 상태, 접근 → layout·stage·access, 단계가 새로 생기는 읽기 전이와 같은 쓰기 상태의 WAW, render pass 바깥 의존, render pass 안 barrier는 pass를 닫고 다시 연다, 버퍼는 쓰기가 끼거나 단계가 생기는 전이만 메모리 barrier, 업로드·readback 상태 중립, `SetTexture` 암묵 전이. 교차 검토(중요 3·사소 3)를 반영했다. 픽셀 형식 PF1(`PixelFormat`, RGBA8 두 형식만 대응, 나머지 `Unsupported`)과 OpenGL ES 전환 병합 뒤 측정(17절).
- 개정 3(2026-11-18): 드라이버 상주(3절). 로더가 `vkDestroyInstance`에서 ICD를 내린 뒤에도 남은 NVIDIA 드라이버 스레드가 내려간 코드를 실행해 EngineTests가 간헐적으로 죽던 문제를 cdb로 확인하고(17절), 프로세스 수명의 상주 instance로 막았다.
- 개정 4(2026-11-18): 로깅 LG7(`Graphics/GraphicsLog.h` 범주, 검증 메시지 ID 표 규칙, 12절)과 픽셀 형식 PF3(`GetPixelFormatCaps`, 능력으로 생성 판정, 6절)을 반영했다.
- 개정 5(2026-11-18): lavapipe 창 경로가 끊긴 데스크톱 세션에서 실패하는 원인을 mesa WSI 소스·GDI 프로브·이전 코드 대조로 확정하고, 세션 연결 판정(`QueryDesktopSessionState`)으로 그 경우만 근거를 단 NOT RUN으로 바꿨다(17절).
- 개정 6(2026-11-18): RenderBase GX1(장치 능력·MSAA 표본 수)·GX2 버퍼 1단계. 버퍼 용도·버퍼 view·`ReadBuffer`(6절), 파이프라인별 layout과 기록 단위 descriptor pool(6절 descriptor, 개정 5의 텍스처별 고정 set 대체), 켜는 장치 기능·확장, 업로드 끝 barrier에 간접 인자·상수·compute 단계를 더했다.
- 개정 7(2026-11-18): GX1·GX2 리뷰 반영(업로드 묶음 RAW barrier, descriptor pool 두 배 증가·경고, `atomic64`의 `shaderInt64`, `apiVersion` 실효 판, ray tracing 확장 공개, VS layer·viewport 출력과 `multiViewport`·viewport 수)과 RenderBase GX3·GX4(10.1절): render pass 열쇠·framebuffer 캐시·깊이 attachment view, 파이프라인 고정 기능 상태, compute 파이프라인과 쓰기 view, 간접 그리기·dispatch, 버퍼 쓰기 명령 barrier, 복사·UAV clear.
- 개정 8(2026-11-25): SH2e2 반영. 셰이더는 쿠킹된 SPIR-V를 장치 셰이더(`CreateShader`)로 받고 파이프라인 layout·셰이더 입력 대조는 값 1과 slot 이름 표에서 얻는다(1·6·7·10.1·11·13·14·15·16·18절). 개발용 컴파일 훅(`ShaderSourceCompiler`·`DeviceDesc::shaderCompiler`) 서술을 지웠다.
