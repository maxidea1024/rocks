#pragma once

#include <Core/Guid.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Platform/File.h>

// 프로젝트의 에셋 너겟 설정이다(Docs/Design/AssetSystem.ko.md 9.1절 결정 4·10, "A3c1 구현 세부").
// 파일 `Config/AssetNuggets.json`(엄격 JSON):
//   { "backend": "filesystem" | "rockpile" | "off" | "auto", "required": bool,
//     "localPath": 문자열, "generation": Guid 표준 문자열 }
// 모든 칸이 선택이고 모르는 키는 거부한다. 파일이 없으면 기본값이다. 결과에 영향을 주는 값은
// `generation`(프로젝트 세대 — 키 공통 머리) 하나뿐이다.
//
// 사용 예:
//   NuggetSettingsFailure failure;
//   auto settings = ReadNuggetCacheSettings(allocator, files, projectRoot, &failure);
//   if (!settings) { /* failure.field·line·column으로 진단 */ }
//   String root(allocator);
//   ResolveNuggetLocalPath(settings.Value(), projectRoot, root);
namespace rocks {

// 설정 파일의 프로젝트 상대 경로와 크기 상한이다.
inline constexpr StringView NuggetSettingsFile = "Config/AssetNuggets.json";
inline constexpr usize NuggetSettingsMaxBytes = 64 * 1024;
// localPath 기본값(프로젝트 루트 상대)이다.
inline constexpr StringView NuggetDefaultLocalPath = "Intermediate/AssetNuggets";

// backend 선택이다. Auto는 Rockpile이 생기기 전까지 FileSystem이다. Rockpile은 계약(열기
// Unsupported). Off는 모든 Get Miss·Put Dropped.
enum class NuggetBackendKind : uint8 {
    FileSystem,
    Rockpile,
    Off,
    Auto
};

struct NuggetCacheSettings {
    explicit NuggetCacheSettings(Allocator& allocator) noexcept : localPath(allocator) {
    }

    NuggetBackendKind backend = NuggetBackendKind::FileSystem;
    // 켜면 열기·도중 backend 실패가 요청 오류다(끄면 미적중·Dropped + 진단).
    bool required = false;
    // 상대면 프로젝트 루트 기준, 절대면 그대로다.
    String localPath;
    // 프로젝트 세대(없으면 null)다.
    Guid generation{};
};

// 거부한 칸이다.
enum class NuggetSettingsField : uint8 {
    None,
    // 파일이 상한보다 크다.
    File,
    // JSON 문법(line·column이 위치).
    Json,
    // 루트가 객체가 아니다.
    Root,
    // 모르는 키.
    UnknownKey,
    Backend,
    Required,
    LocalPath,
    Generation
};

struct NuggetSettingsFailure {
    NuggetSettingsField field = NuggetSettingsField::None;
    // Json일 때의 위치(1부터)다. 그 밖은 0이다.
    uint32 line = 0;
    uint32 column = 0;
};

// projectRoot/Config/AssetNuggets.json을 읽는다. 없으면 기본값(filesystem·false·
// NuggetDefaultLocalPath·null 세대)이다. 오류: 거부(InvalidArgument — failure에 칸, 상한 초과
// 파일은 CapacityExceeded·File), 파일 읽기 오류는 그 오류(FileErrorCode), 할당 실패 OutOfMemory.
// failure는 null이어도 된다.
Result<NuggetCacheSettings> ReadNuggetCacheSettings(Allocator& allocator,
                                                    platform::FileSystem& files,
                                                    StringView projectRoot,
                                                    NuggetSettingsFailure* failure) noexcept;

// backend 루트 경로를 out에 둔다(localPath가 상대면 projectRoot + "/" + localPath, 루트에 붙은
// 경로(POSIX 루트·Windows 드라이브 절대·UNC)면 그대로 — Path::Root). 오류: 쓸 수 없는 localPath
// (Windows 현재 드라이브 루트·드라이브 상대·device namespace — 설정 읽기가 이미 거부한다)
// InvalidArgument·Unsupported, 할당 실패.
Result<void> ResolveNuggetLocalPath(const NuggetCacheSettings& settings, StringView projectRoot,
                                    String& out) noexcept;

} // namespace rocks
