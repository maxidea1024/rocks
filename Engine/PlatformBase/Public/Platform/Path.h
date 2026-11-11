#pragma once

#include <Platform/File.h>

namespace rocks::platform {

// 호스트의 파일 경로 문법이다. 새 플랫폼은 문법을 명시적으로 선택해야 한다.
// 경로의 유효성이나 실제 파일 존재·접근 가능성은 판단하지 않는다.
enum class PathStyle {
    Windows,
    Posix,
    Unsupported
};

// 선택한 호스트 구현을 반환한다. 미지원 호스트는 Unsupported다.
PathStyle HostPathStyle() noexcept;
// 호스트 파일 경로의 기본 구분자를 반환한다. 경로 자체를 변환하지 않는다.
char NativePathSeparator() noexcept;

// 운영체제가 정하는 기본 디렉터리다(UE FPlatformProcess의 BaseDir·UserDir 계열 대응).
// - Executable: 실행 파일이 있는 디렉터리
// - Working: 현재 작업 디렉터리(다른 스레드가 바꿀 수 있는 순간 값)
// - Temporary: 사용자 임시 디렉터리
// - Home: 사용자 홈 디렉터리
// - UserData: 사용자별 앱 설정·저장 데이터(Windows Roaming AppData, macOS Library/Application
// Support)
// - UserCache: 지워져도 다시 만들 수 있는 캐시(Windows Local AppData, macOS Library/Caches)
enum class StandardDirectory : uint8 {
    Executable,
    Working,
    Temporary,
    Home,
    UserData,
    UserCache
};

// kind의 경로를 UTF-8로 buffer에 쓰고 길이를 bytes에 둔다(NUL 포함하지 않음, buffer에는 NUL도
// 쓴다). 끝 구분자는 없다(루트 제외). 구분자는 OS 표기 그대로다(Windows는 '\\'). 실패: buffer
// 부족은 Overflow이며 bytes에 필요한 길이(NUL 제외)를 둔다. 그 밖은 OS 원인 코드다. 앱 이름 하위
// 디렉터리를 만들지 않는다. 필요하면 MakeDirectoryTree로 만든다.
// Android: UserData·UserCache는 RegisterApplicationDirectories로 등록한 값이며 등록 전에는
// NotFound다. Temporary는 등록된 cache, 없으면 TMPDIR·/data/local/tmp, Home은 등록된 data, 없으면
// HOME이다.
FileStatus GetStandardDirectory(StandardDirectory kind, char* buffer, usize capacity,
                                usize& bytes) noexcept;

// 모바일 앱이 시작할 때 OS가 준 앱 전용 디렉터리(절대 경로, UTF-8)를 등록한다. Android에서는
// Context.getFilesDir()/getCacheDir() 또는 ANativeActivity의 internalDataPath와 cache 경로를
// 넘긴다. 여러 번 호출하면 마지막 값이 쓰이며 스레드 안전하다. 실패: null·빈 값·상대 경로·너무 긴
// 경로는 InvalidArgument. desktop(Windows·macOS)은 OS 기본 디렉터리를 쓰므로 Unsupported다.
FileStatus RegisterApplicationDirectories(const char* dataPath, const char* cachePath) noexcept;

} // namespace rocks::platform
