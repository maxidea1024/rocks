#pragma once

#include <Core/Result.h>
#include <Core/Text/String.h>
#include <SplashScreen/SplashTypes.h>

namespace rocks::platform {
class FileSystem;
} // namespace rocks::platform

// 스플래시 이미지 찾기와 크기 규칙이다(Docs/Design/SplashScreen 5절). 순수 계산과 파일 존재 확인만
// 하고 디코딩은 하지 않는다(디코딩은 스플래시 스레드의 presenter가 한다).
namespace rocks::splash {

// 이미지 파일 크기 상한이다. 넘으면 ImageInvalid다.
inline constexpr uint64 MaxSplashImageBytes = uint64{32} * 1024 * 1024;

// 이미지 가로·세로 상한(px)이다. 넘으면 ImageInvalid다.
inline constexpr uint32 MaxSplashImageDimension = 4096;

// 논리 크기의 가로·세로 하한(px)이다. 글자 띠가 들어갈 최소다. 작으면 ImageInvalid다.
inline constexpr uint32 MinSplashLogicalDimension = 160;

// 고해상도 변형 파일 이름에 붙는 꼬리다("Splash@2x.png").
inline constexpr StringView SplashDoubleSuffix = "@2x";

// 찾는 확장자 순서다(UE GetSplashFilename과 같다 — .png → .jpg → .bmp).
inline constexpr StringView SplashImageExtensions[] = {".png", ".jpg", ".bmp"};

// 스플래시 이미지 이름이다(Game "Splash", Studio "StudioSplash").
ROCKS_SPLASHSCREEN_API StringView SplashImageName(SplashKind kind) noexcept;

// 찾기 입력이다. 경로는 UTF-8이고 '/'·'\\' 구분자를 모두 받는다.
struct SplashImageQuery {
    SplashKind kind = SplashKind::Game;
    // 설정 ui.splash.<종류>.image다. 비어 있지 않으면 이 경로만 본다({ProjectDir}·{EngineDir}를
    // 펼친다). 고해상도 변형은 같은 이름에 "@2x"를 붙인 파일이다.
    StringView configuredPath;
    // 프로젝트 폴더다. 비면 프로젝트가 없다(Sandbox — 엔진 폴더만 본다).
    StringView projectDirectory;
    // 엔진 폴더(Engine/)다. 비면 엔진 자리를 보지 않는다.
    StringView engineDirectory;
};

// 찾은 경로다. 둘 중 적어도 하나는 비어 있지 않다.
struct SplashImagePaths {
    explicit SplashImagePaths(Allocator& allocator) noexcept
        : standard(allocator), doubled(allocator) {
    }

    // 1x 이미지(논리 크기 그대로)다. 비면 없다.
    String standard;
    // 고해상도(@2x) 이미지다. 비면 없다.
    String doubled;
};

// 5.2절 순서로 이미지를 찾는다: 설정 경로 → 프로젝트 Content/Splash → 엔진 Content/Splash. 각
// 자리에서 확장자 순서대로 1x·@2x를 보고, 하나라도 있는 첫 자리·첫 확장자를 쓴다. 오류: 어디에도
// 없으면 NotFound, 설정 경로에 모르는 {변수}가 있으면 InvalidArgument, 할당 실패는 그 오류, 파일
// 시스템 오류는 그 자리를 없는 것으로 본다(다음 자리로 간다).
//
// 사용 예:
//   splash::SplashImageQuery query;
//   query.kind = splash::SplashKind::Game;
//   query.engineDirectory = engineDir;
//   auto paths = splash::ResolveSplashImage(allocator, platform::PhysicalFileSystem(), query);
ROCKS_SPLASHSCREEN_API Result<SplashImagePaths>
ResolveSplashImage(Allocator& allocator, platform::FileSystem& files,
                   const SplashImageQuery& query) noexcept;

// 픽셀 크기다.
struct SplashSize {
    uint32 width = 0;
    uint32 height = 0;

    friend constexpr bool operator==(SplashSize, SplashSize) noexcept = default;
};

// 디코딩한 1x·@2x 크기에서 논리 크기를 정한다(5.3절). 없는 쪽은 {0, 0}이다. 규칙: 1x가 있으면 그
// 크기, @2x만 있으면 절반(가로·세로가 짝수여야 한다), 둘 다 있으면 @2x가 정확히 두 배, 모든 크기는
// MaxSplashImageDimension 이하, 논리 크기는 MinSplashLogicalDimension 이상. 어기거나 둘 다 없으면
// InvalidArgument다(사유 ImageInvalid).
ROCKS_SPLASHSCREEN_API Result<SplashSize> SplashLogicalSize(SplashSize standard,
                                                            SplashSize doubled) noexcept;

// 원본으로 쓸 변형이다.
enum class SplashImageVariant : uint8 {
    Standard,
    Double,
};

// 모니터 배율(1.0 = 96 DPI)과 변형 유무로 원본을 고른다(5.3절): 배율이 1.0을 넘고 @2x가 있으면
// Double, 아니면 Standard(1x가 없으면 Double). 둘 다 없으면 Standard다.
ROCKS_SPLASHSCREEN_API SplashImageVariant ChooseSplashVariant(double scale, bool hasStandard,
                                                              bool hasDouble) noexcept;

// 창 픽셀 크기다: 논리 크기 × 배율(반올림, 최소 1). 배율이 유한한 양수가 아니면 1.0으로 본다.
ROCKS_SPLASHSCREEN_API SplashSize SplashWindowSize(SplashSize logical, double scale) noexcept;

} // namespace rocks::splash
