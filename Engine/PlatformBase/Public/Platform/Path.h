#pragma once

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

} // namespace rocks::platform
