#pragma once

#include <Platform/File.h>

namespace rocks::platform {

// Android APK 안의 파일(assets/)을 읽는 읽기 전용 파일 시스템이다(Docs/Design/AndroidPlatform.ko.md
// 7절 "APK 안 파일"). 경로는 assets/ 아래 상대 경로다(예: "Cooked/CookedManifest.rman"). 패키저가
// 비압축으로 넣은 파일이다.
// - Open: ReadExisting만 받는다(그 밖은 Unsupported). Read·Seek·Size가 되고 Write는 Unsupported다.
// - FileExists·Stat·DirectoryExists: 파일은 열어 본다. 디렉터리는 AAssetManager_openDir가 항목을
//   하나라도 돌려주면 있다고 본다(빈 디렉터리는 APK에 없다).
// - EnumerateDirectory: 그 디렉터리의 파일만 넘긴다(NDK AAssetDir는 하위 디렉터리를 돌려주지 않는다
//   — 하위 디렉터리 열거가 필요하면 매니페스트로 경로를 안다).
// - 쓰기·삭제·이동은 Unsupported다.
// - AAssetManager가 등록되기 전(APK가 아닌 실행 파일 포함)에는 Open·Stat이 NotFound,
//   FileExists·DirectoryExists가 false다. Android가 아니면 모든 연산이 Unsupported다.
//
// 사용 예:
//   platform::File manifest;
//   FileHandle* handle = nullptr;
//   if (auto opened = platform::ApkFileSystem().Open("Cooked/CookedManifest.rman", 27,
//                                                     platform::FileOpenMode::ReadExisting,
//                                                     handle)) { ... }
//
// 스레드: 객체는 상태가 없고(등록된 관리자만 읽는다) 동시 호출이 가능하다. 핸들은 File처럼
// 호출자가 직렬화한다.
ROCKS_PLATFORMBASE_API FileSystem& ApkFileSystem() noexcept;

// 액티비티의 AAssetManager*를 등록한다(Launch Android 액티비티 층이 onCreate에서 부른다). null은
// InvalidArgument, Android가 아니면 Unsupported다. 프로세스 수명 동안 유효해야 한다.
ROCKS_PLATFORMBASE_API FileStatus RegisterApkAssetManager(void* assetManager) noexcept;

} // namespace rocks::platform
