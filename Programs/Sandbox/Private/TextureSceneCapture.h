#pragma once

#include "CpuDisplayColor.h"

#include <Core/Containers/Array.h>
#include <WorldIntegration/Textures/PlatformTextureSource.h>

// 텍스처 장면 self-test의 표본점·CPU 기준·capture 대조 도우미다(Docs/Design/RenderBase 18.5절 7항).
// 메모리 장면과 쿠킹 장면이 함께 쓴다. capture는 SceneCaptureWidth × SceneCaptureHeight RGBA8(sRGB
// view 부호화)이고, 텍스처 표시는 전체화면이다(2D·배열·volume은 화면 = uv [0, 1], cube는 가로 십자
// 4 × 3 칸).
namespace sandbox {

using namespace rocks;

inline constexpr uint32 SceneCaptureWidth = 160;
inline constexpr uint32 SceneCaptureHeight = 90;
// GPU 결과와 CPU 기준의 채널 허용 차(8 bit)와 backend 사이 허용 차다(RenderBase 8절 기준).
inline constexpr int SceneCaptureTolerance = 2;
inline constexpr int SceneBackendTolerance = 8;
// 장면 하나가 가질 수 있는 표본점 수다(cube 면 여섯이 가장 많다).
inline constexpr uint32 MaxScenePoints = 8;
// 검사 이름 byte 수다.
inline constexpr usize MaxSceneCheckName = 112;

// 표본점 하나의 기대값이다. key(장면 번호 × MaxScenePoints + 점 번호)는 장치 사이 대조의
// 정체성이다.
struct SceneCheck {
    uint32 key = 0;
    uint32 slot = 0;
    uint32 x = 0;
    uint32 y = 0;
    DisplayPixel expected{};
    char what[MaxSceneCheckName] = {};
};

// mip level의 byte 전체다(스트리밍 mip은 그 bulk, 상주 mip은 꼬리 덩어리의 그 구간). 오류: reader의
// 오류, 범위 밖 InvalidArgument.
Result<Span<const uint8>> MipBytes(const TexturePlatformData& data, PlatformBulkReader reader,
                                   void* context, uint32 level) noexcept;

// 텍스처 표시 한 프레임(layer·mip)의 표본점 기대값을 checks 끝에 붙인다. bytes는 그 mip의 byte(배치
// 6.5절 — slice 다음 z 다음 행)이고 width·height·slices·depth는 그 mip의 크기다. CPU Availability와
// 표시 엔트리가 없는 정수 cube는 검정 한 점이다.
void AppendInspectorChecks(const screen::InspectorTexture& texture, uint32 layer, uint32 mip,
                           uint32 width, uint32 height, uint32 slices, uint32 depth,
                           graphics::PixelFormat format, Span<const uint8> bytes, uint32 sceneIndex,
                           uint32 slot, const char* sceneName, Array<SceneCheck>& checks) noexcept;

// capture 픽셀 하나를 대조한다(capture가 없거나 다르면 failures를 올리고 진단을 찍는다).
void CheckCapturePixel(const Array<uint8>& pixels, uint32 x, uint32 y, const DisplayPixel& expected,
                       int tolerance, const char* device, const char* mode, const char* what,
                       uint32& failures) noexcept;

// 처음 돈 장치의 표본점 값을 기준으로 남기고, 뒤 장치는 그 값과 SceneBackendTolerance 안인지
// 본다(group마다 따로 — "texture scenes"·"cooked textures"). captures는 slot마다 capture를 이은
// byte다(slotCount 자리).
void CompareWithReferenceBackend(const char* group, const char* device,
                                 Span<const SceneCheck> checks, Span<const uint8> captures,
                                 uint32 slotCount, uint32& failures) noexcept;

} // namespace sandbox
