#pragma once

#include <Base/Compiler.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>

// 그래픽 장치를 받아 주는 규칙이 갈리는 플랫폼이다(Docs/Design/PixelFormat 7.1절). 같은
// backend라도 플랫폼마다 엔진이 받는 장치가 다르다(데스크톱 Vulkan은 BC, Android Vulkan은
// ASTC·ETC2).
namespace rocks::graphics {

// 플랫폼이다. 이름은 HostPlatformName()·플랫폼 층 폴더·엔진 대상 프로필 id와 같은 철자다. 열거자
// 값은 0부터 빈틈 없이 쓴다(GraphicsPlatformCount개).
//
// 사용 예:
//   auto platform = ParseGraphicsPlatformName(profile.id.View());  // "Android" → Android
//   const GraphicsPlatform host = HostGraphicsPlatform();           // 이 실행 파일의 플랫폼
enum class GraphicsPlatform : uint8 {
    Windows,
    Mac,
    Android
};

// GraphicsPlatform 열거자 수다.
inline constexpr uint32 GraphicsPlatformCount = 3;

// "Windows" 같은 이름이다. 범위 밖이면 "Unknown"이다.
ROCKS_GRAPHICSTYPES_API const char* GraphicsPlatformName(GraphicsPlatform platform) noexcept;

// GraphicsPlatformName의 이름을 플랫폼으로 읽는다(대소문자 구분). 실패: 모르는 이름("Unknown"
// 포함) NotFound.
ROCKS_GRAPHICSTYPES_API Result<GraphicsPlatform>
ParseGraphicsPlatformName(StringView name) noexcept;

// 이 실행 파일이 빌드된 플랫폼이다(Base IsWindows·IsMac·IsAndroid — 빌드 시스템이 하나만 참으로
// 정한다).
constexpr GraphicsPlatform HostGraphicsPlatform() noexcept {
    if constexpr (IsAndroid) {
        return GraphicsPlatform::Android;
    } else if constexpr (IsMac) {
        return GraphicsPlatform::Mac;
    } else {
        return GraphicsPlatform::Windows;
    }
}

} // namespace rocks::graphics
