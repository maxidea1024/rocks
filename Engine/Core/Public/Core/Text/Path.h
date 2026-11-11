#pragma once

#include <Core/Text/String.h>
#include <Platform/Path.h>

namespace rocks {

using PathStyle = platform::PathStyle;
enum class PathRoot {
    Relative,
    PosixRoot,
    WindowsCurrentDriveRoot,
    WindowsDriveRelative,
    WindowsDriveAbsolute,
    WindowsUnc
};

// Split의 결과는 원본 저장소를 빌린다. 원본이 변경·파괴되면 사용하지 않는다.
struct PathParts {
    StringView directory;
    StringView name;
    StringView extension;
};

// 파일 접근 없이 UTF-8 경로 문자열만 다룬다. NUL·잘못된 UTF-8은 InvalidArgument다.
// 문법을 받는 API는 Windows device namespace(\\?\, \\.\)에 Unsupported를 반환한다.
// Combine/Append와 String의 /, /=는 문법을 해석하지 않고 원문을 결합하므로 이 검사를 하지
// 않는다. Posix 문법에서는 '\\'가 문자다.
// view 결과는 입력을 빌린다. 입력의 수명·변경은 caller가 관리한다.
// Normalize/Join/MakeAbsolute/MakeRelative/IsUnder는 문자 단위의 논리 연산이다.
// 심볼릭 링크·정션·TOCTOU·실제 파일 동일성이나 접근 권한을 검사하지 않으므로
// 보안 경계 또는 파일 존재 검사에 사용하지 않는다.
class Path final {
public:
    static PathStyle HostStyle() noexcept;
    static Result<PathRoot> Root(StringView path, PathStyle style = HostStyle()) noexcept;

    // 반복 구분자와 '.'을 정리하고 '..'는 앞의 일반 컴포넌트와 상쇄한다. 상대 경로의
    // 남은 '..'는 보존하며 절대 루트 위로 벗어나면 InvalidArgument다. 빈 입력은 빈 결과,
    // 빈 상대 결과는 '.'이다. 루트를 제외한 끝 구분자는 제거한다. POSIX 선행 //도 /로
    // 축약하므로 특수 // 의미나 끝 구분자에 따른 실제 접근 의미는 보장하지 않는다.
    // Windows 상대 경로 첫 부분이 드라이브처럼 보이면 재해석 방지를 위해 ./를 붙인다.
    static Result<String> Normalize(Allocator& allocator, StringView path,
                                    PathStyle style = HostStyle()) noexcept;
    // UE의 filename 단계처럼 모든 역슬래시를 슬래시로 바꾼다. 문법과 상관없이 적용하며
    // '.', '..', 반복·끝 구분자는 보존한다. OS별 NormalizePath hook은 아직 적용하지 않는다.
    // 입력은 유효 UTF-8이고 NUL이 없어야 하며 실패해도 입력은 바뀌지 않는다.
    static Result<String> NormalizeFilename(Allocator& allocator, StringView path,
                                            PathStyle style = HostStyle()) noexcept;
    // 사전에 '/' 구분자로 바꾼 경로의 상대 디렉터리를 축약한다. UE5.6과 같이 선행 '..'·
    // '/..' 및 드라이브 제거 시도는 InvalidArgument다. 입력을 변경하지 않으며 빈 결과를 허용한다.
    // 이 함수는 역슬래시를 자동 변환하지 않는다. 필요하면 NormalizeFilename을 먼저 호출한다.
    static Result<String> CollapseRelativeDirectories(Allocator& allocator, StringView path,
                                                      PathStyle style = HostStyle()) noexcept;
    // UE PathAppend처럼 구분자만 보충해 원본 경로 표기를 결합한다. Join과 달리
    // 루트 검증·'.'/'..' 정규화는 수행하지 않는다. NUL·무효 UTF-8은 InvalidArgument다.
    // 두 입력은 호출이 끝날 때까지 살아 있어야 하며 반환 String은 allocator를 빌린다.
    static Result<String> Combine(Allocator& allocator, StringView left, StringView right) noexcept;
    // 성공할 때만 기존 String을 교체한다. 실패 시 내용과 버퍼 주소를 보존한다.
    // right는 left 자신의 전체 또는 부분 view여도 된다.
    static Result<void> Append(String& left, StringView right) noexcept;
    // 오른쪽은 순수 상대 경로여야 한다. 절대·드라이브 상대 RHS는 InvalidArgument다.
    // 빈 오른쪽은 base를 정규화한다. 입력은 결과보다 오래 살아야 하며 자기 view도 허용한다.
    static Result<String> Join(Allocator& allocator, StringView base, StringView relative,
                               PathStyle style = HostStyle()) noexcept;
    // base는 완전한 절대 디렉터리다. path는 순수 상대 또는 완전 절대이며 그 외는 오류다.
    static Result<String> MakeAbsolute(Allocator& allocator, StringView base, StringView path,
                                       PathStyle style = HostStyle()) noexcept;
    // 두 경로 모두 완전 절대여야 하며 base는 디렉터리다. 서로 다른 루트는 InvalidArgument다.
    // 정확히 같으면 '.'이다. Windows 드라이브 문자만 ASCII 대소문자를 무시한다.
    static Result<String> MakeRelative(Allocator& allocator, StringView base, StringView path,
                                       PathStyle style = HostStyle()) noexcept;
    // 두 경로는 비어 있지 않은 완전 절대 또는 순수 상대이며 같은 종류여야 한다.
    // 서로 다른 절대 루트는 false, 동일 경로는 true다.
    // 컴포넌트 단위 byte 비교이며 Windows 드라이브 문자 외에는 대소문자를 구분한다.
    // 상대 경로에서 상위 이름이 불명확해 포함 여부를 입증할 수 없으면 보수적으로 false다.
    static Result<bool> IsUnder(Allocator& allocator, StringView path, StringView directory,
                                PathStyle style = HostStyle()) noexcept;

    // 마지막 구분자 뒤의 이름을 빌린다. 끝이 구분자면 빈 view다. 문자열 분리 연산은
    // 불완전한 UNC 등 중간 표기도 허용하지만 device namespace는 거부한다.
    static Result<StringView> FileName(StringView path, PathStyle style = HostStyle()) noexcept;
    // 마지막 구분자 이전의 표기를 빌린다. 루트 보존 Parent와 달리 /file은 빈 view,
    // C:/file은 C:다. 반복 구분자도 입력 그대로 보존한다.
    static Result<StringView> Directory(StringView path, PathStyle style = HostStyle()) noexcept;
    // 마지막 컴포넌트 이전의 view다. 끝 구분자가 있으면 그것만 제거한 경로다.
    // 루트는 자기 자신을 반환하고 부모가 없는 상대 경로는 빈 view다.
    static Result<StringView> Parent(StringView path, PathStyle style = HostStyle()) noexcept;
    // 마지막 점 뒤의 확장자를 점 없이 빌린다. '.name'의 확장자는 'name'이며
    // '.', '..'과 끝의 점은 빈 확장자다. 원본 문자열을 변경하거나 정규화하지 않는다.
    static Result<StringView> Extension(StringView path, PathStyle style = HostStyle()) noexcept;
    // 파일명에서 마지막 점과 그 뒤를 제외한 부분이다. '.name'의 결과는 빈 view다.
    // 마지막 점 앞을 반환하므로 .은 빈 값, ..은 .이다. 끝의 점도 제외한다.
    static Result<StringView> BaseFileName(StringView path, PathStyle style = HostStyle()) noexcept;
    // directory, 확장자 없는 name, 점 없는 extension을 입력으로부터 빌린다.
    // 표기와 구분자를 바꾸지 않는다. '..'만 name으로 유지하고 '.'의 name은 빈 값이다.
    static Result<PathParts> Split(StringView path, PathStyle style = HostStyle()) noexcept;
    // 기존 확장자 구분점이 있을 때만 교체한다. 구분점이 없다면 원본 표기를 그대로 복사한다.
    // SetExtension은 기존 확장자가 없는 경우에도 새 확장자를 붙인다. 마지막 점은 빈
    // 확장자 구분점으로 취급하므로 "file."에 빈 확장자를 지정하면 "file"이다.
    // extension은 선행 점 하나를 허용하며 빈 값은 기존 구분점과 확장자를 제거한다.
    // 파일명이 없는 경로도 허용한다. extension의 구분자·NUL·무효 UTF-8은 InvalidArgument다.
    // 반환 값은 독립 소유이며 입력 view는 호출이 끝날 때까지 살아 있어야 한다.
    static Result<String> ChangeExtension(Allocator& allocator, StringView path,
                                          StringView extension,
                                          PathStyle style = HostStyle()) noexcept;
    static Result<String> SetExtension(Allocator& allocator, StringView path, StringView extension,
                                       PathStyle style = HostStyle()) noexcept;
};

} // namespace rocks
