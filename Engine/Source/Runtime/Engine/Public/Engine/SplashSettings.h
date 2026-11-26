#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>

// 시작 화면(스플래시) 설정이다(Docs/Design/SplashScreen 10절). `ui.splash.*` 부팅 CVar이고 Engine
// 모듈이 등록한다 — 스플래시를 그리는 SplashScreen 모듈은 ClientOnly라 서버 대상에 없으므로, 설정
// 이름은 모든 대상에 있는 Engine 모듈이 가져야 같은 프로젝트 설정 파일을 서버도 읽는다. 값은
// 호스트가 부트에서 읽어 SplashScreen에 넘긴다(SplashScreen은 Config를 모른다).
//
// 프로젝트 설정 예(<Project>/Config/Engine.json):
//   "Settings": {
//     "ui.splash.game.show_progress_text": true,
//     "ui.splash.game.copyright": "(c) 2026 My Studio"
//   }
// `ui.splash.enabled`는 사용자 층에도 쓸 수 있다(플레이어가 끈다).
namespace rocks {

namespace config {
class ConfigContext;
} // namespace config

// 읽은 값이다. 문자열은 읽은 스레드에 묶인 설정 snapshot을 가리키므로 다음 Sync 전에 복사한다(부팅
// 값이라 내용은 바뀌지 않는다).
struct SplashSettingValues {
    // 스플래시를 띄우는지다(`ui.splash.enabled`, 기본 true).
    bool enabled = true;
    // 게임 스플래시 이미지 경로(UTF-8)다. 비면 프로젝트·엔진 Content/Splash에서 찾는다.
    StringView gameImage;
    // 게임 제목 칸을 보이는지·그 글자다. 글자가 비면 프로젝트 이름이다.
    bool gameShowTitle = true;
    StringView gameTitle;
    // 게임의 진행 문구 칸(기본 끔)과 진행 막대(기본 켬)다.
    bool gameShowProgressText = false;
    bool gameShowProgressBar = true;
    // 게임 저작권 칸 글자다. 비면 보이지 않는다.
    StringView gameCopyright;
    // Studio 스플래시 이미지 경로와 저작권 글자다. Studio 칸 표시는 고정이다(설계 6.3절).
    StringView studioImage;
    StringView studioCopyright;
};

// `ui.splash.*`를 등록한다. Engine 모듈의 RegisterSettings가 부르고, 호스트 없이 설정 문맥을 만드는
// 시험도 부른다. 오류는 ConfigContext::Register와 같다.
ROCKS_ENGINE_API Result<void> RegisterSplashSettings(config::ConfigContext& context) noexcept;

// 이 스레드에 묶인 snapshot에서 값을 읽는다. 전제: 문맥에 RegisterSplashSettings를 했다(등록 안 된
// 핸들은 게임 lane에서 Fatal이다 — CVar 규칙).
//
// 사용 예(호스트 부트, CompleteStartup 뒤):
//   const SplashSettingValues splash = ReadSplashSettings();
//   if (splash.enabled) { ... }
ROCKS_ENGINE_API SplashSettingValues ReadSplashSettings() noexcept;

} // namespace rocks
