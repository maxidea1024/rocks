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

// 루트 뒤의 경로 구성 요소를 앞에서부터 하나씩 빌려 주는 범위다(Path::Components가 만든다).
//
//   auto parts = Path::Components("C:\\Game\\Content\\Maps", PathStyle::Windows);
//   if (parts) {
//       for (StringView part : parts.Value()) { ... }   // "Game", "Content", "Maps"
//   }
//
// - 루트("C:\\"·"/"·"\\\\server\\share\\")는 내지 않는다. 루트는 Path::RootPart로 얻는다.
// - 반복 구분자로 생기는 빈 구성 요소는 건너뛴다("a//b"는 "a", "b").
// - '.'·'..'는 정규화하지 않고 그대로 낸다. 정규화가 필요하면 Path::Normalize를 먼저 쓴다.
// - 구성 요소 view는 원본 경로를 빌린다. 원본이 바뀌거나 사라지면 쓰지 않는다. 결과 Result를 지역
//   변수에 담은 뒤 반복한다(임시 Result에 바로 range-for를 걸면 이전 컴파일러에서 수명 문제가
//   있다).
// - UE FPathViews::IterateComponents와 쓰임은 같지만 루트·빈 구성 요소를 다루는 방식은 다를 수
// 있다.
class PathComponentRange {
public:
    class Iterator {
    public:
        using value_type = StringView;
        using difference_type = isize;
        using reference = StringView;

        Iterator() noexcept = default;

        StringView operator*() const noexcept {
            return StringView(text_ + start_, end_ - start_);
        }

        Iterator& operator++() noexcept {
            Advance(end_);
            return *this;
        }

        Iterator operator++(int) noexcept {
            Iterator previous = *this;
            Advance(end_);
            return previous;
        }

        // 같은 경로의 같은 위치일 때 같다. 서로 다른 범위의 반복자는 비교하지 않는다.
        friend bool operator==(const Iterator& left, const Iterator& right) noexcept {
            return left.text_ == right.text_ && left.start_ == right.start_;
        }

    private:
        friend class PathComponentRange;

        Iterator(const char* text, usize size, usize from, bool windows) noexcept
            : text_(text), size_(size), windows_(windows) {
            Advance(from);
        }

        bool Separator(char value) const noexcept {
            return value == '/' || (windows_ && value == '\\');
        }

        void Advance(usize from) noexcept {
            usize cursor = from;
            while (cursor < size_ && Separator(text_[cursor])) {
                ++cursor;
            }
            start_ = cursor;
            end_ = cursor;
            while (end_ < size_ && !Separator(text_[end_])) {
                ++end_;
            }
        }

        const char* text_ = nullptr;
        usize size_ = 0;
        usize start_ = 0;
        usize end_ = 0;
        bool windows_ = false;
    };

    Iterator begin() const noexcept {
        return Iterator(path_.Data(), path_.ByteCount(), rootEnd_, windows_);
    }

    Iterator end() const noexcept {
        return Iterator(path_.Data(), path_.ByteCount(), path_.ByteCount(), windows_);
    }

private:
    friend class Path;

    PathComponentRange(StringView path, usize rootEnd, bool windows) noexcept
        : path_(path), rootEnd_(rootEnd), windows_(windows) {
    }

    StringView path_;
    usize rootEnd_ = 0;
    bool windows_ = false;
};

// Path::CollapseRelativeDirectories가 축약할 수 없는 '..'(앞에 지울 구성 요소가 없는
// '..')를 만났을 때의 처리다(UE 5.8 bCollapseAllPossible 대응).
// - RequireAll: InvalidArgument로 실패한다(UE 기본값 false). 모두 축약되는지 확인할 때
//   쓴다.
// - AllPossible: 그 '..'는 남기고 나머지 '.'·'..'를 모두 축약한 결과로 성공한다(UE
//   true). 앞의 '..'가 정상인 상대 경로("../../Game/./Content")를 정리할 때 쓴다. 모두
//   축약되었는지는 알려 주지 않는다. 필요하면 RequireAll로 확인한다.
// 모두 축약되는 입력이면 두 방식의 결과가 같다.
enum class PathCollapseMode : uint8 {
    RequireAll,
    AllPossible
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
    // '/' 구분자 경로의 '.'·'..' 디렉터리를 문자열에서 축약한 사본을 만든다(UE 5.8
    // FPaths::CollapseRelativeDirectories와 같은 규칙). 파일 시스템에 접근하지 않는다.
    // UE 호환 유틸이므로 아래 레거시 결과도 그대로 따른다. 루트 이탈 거부와 반복 구분자
    // 정리가 필요한 일반 경로 정리에는 Normalize를 쓴다.
    //
    //   "Game/Maps/../Content/./Hero.mesh" → "Game/Content/Hero.mesh"
    //   "C:/Game/../Saved"                 → "C:/Saved"
    //   "a/../../b"                        → InvalidArgument(AllPossible이면 "../b")
    //
    //   auto collapsed = Path::CollapseRelativeDirectories(allocator, "a/../../b",
    //                                                      PathStyle::Posix,
    //                                                      PathCollapseMode::AllPossible);
    //
    // 규칙:
    // - 역슬래시는 구분자로 보지 않는다. 필요하면 NormalizeFilename을 먼저 호출한다.
    // - 첫 '/'보다 앞에 있는 첫 ':'까지("C:", "Storage:")는 볼륨으로 보고 지우지 않는다.
    //   그 뒤의 ':'는 일반 문자다("a/b:c/../d" → "a/d", "C:D:a/../b" → "C:b").
    //   드라이브 상대 "C:a/../b"는 "C:b"다.
    // - 선행 "./" 하나와 중간·끝의 "/."를 지운다. 선행 "./"는 처음에 한 번만 지우므로
    //   "././a"는 "./a", 축약 뒤 맨 앞에 생긴 "./"도 남는다("a/.././b" → "./b").
    //   정확히 "/."·"C:/."인 입력은 그대로 둔다.
    // - "name/.."는 앞 구성 요소와 함께 지운다. '..' 앞의 겹친 구분자("a//..")는 하나로
    //   본다. '..' 뒤의 겹친 구분자는 정리하지 않는다.
    // - '.'·'..'로 시작하거나 '.'로 끝나는 이름(".git", "..name", "a..")은 일반 이름이다.
    //   이름 안의 "./"도 지우지 않는다("a.../x"·"a..//"는 그대로).
    // - 앞에 지울 구성 요소가 없는 '..'(선행 "..", 볼륨·루트 바로 뒤의 "/..", 남긴 '..'
    //   뒤의 '..')는 축약할 수 없다. 처리는 mode가 정한다(PathCollapseMode).
    // - UE 레거시: 끝의 "/.."는 루트 구분자까지 지운다("C:/a/.." → "C:", "/a/.."·"//.." →
    //   빈 결과). 끝에 '/'가 더 있으면 루트가 남는다("C:/a/../" → "C:/").
    // - UE 레거시: 겹친 구분자가 볼륨 바로 뒤나 '..' 뒤에 있으면 의미가 바뀐 결과가
    //   나온다. "C://../x"·"C:/a/..//../x" → "C:x"(성공), "a/..//x" → "/x",
    //   "/a/..//x" → "//x", AllPossible의 "..//../Game" → "..Game".
    // mode: 기본 RequireAll은 축약할 수 없는 '..'가 있으면 InvalidArgument다. UE의 false
    //   모드는 이때 일부만 축약한 문자열을 남기지만 Rocks는 결과를 내지 않는다.
    //   AllPossible은 축약할 수 없는 '..'로는 실패하지 않고 그 '..'를 남긴 결과를 돌려준다.
    //   정의되지 않은 mode 값은 InvalidArgument다.
    // 오류: 무효 UTF-8·NUL은 InvalidArgument, Windows 문법의 device namespace("//?/",
    //   "//./", "\\\\?\\", "\\\\.\\")는 Unsupported, 할당 실패는 allocator 오류다.
    //   문법(style)은 이 검증에만 쓰며 축약 규칙은 두 문법이 같다. 빈 결과도 성공이다.
    // 수명: 반환 String은 allocator를 쓰는 독립 사본이고 입력은 바꾸지 않는다. 입력 view는
    //   호출이 끝날 때까지 살아 있으면 된다.
    static Result<String>
    CollapseRelativeDirectories(Allocator& allocator, StringView path,
                                PathStyle style = HostStyle(),
                                PathCollapseMode mode = PathCollapseMode::RequireAll) noexcept;
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

    // 경로의 루트 부분을 빌린다. 상대 경로는 빈 view다.
    //   "/usr/lib"(POSIX) → "/",  "C:\\a" → "C:\\",  "C:a" → "C:",  "\\a" → "\\",
    //   "\\\\server\\share\\dir" → "\\\\server\\share\\"(공유 이름 뒤 구분자가 있으면 포함)
    // 오류는 Root와 같다(잘못된 UTF-8·NUL은 InvalidArgument, Windows device·NT namespace는
    // Unsupported, 불완전한 UNC는 InvalidArgument).
    static Result<StringView> RootPart(StringView path, PathStyle style = HostStyle()) noexcept;

    // 루트 뒤의 구성 요소 범위다(PathComponentRange 참고). 오류는 RootPart와 같다.
    // 조상 디렉터리를 차례로 보려면 Parent를 루트에 닿을 때까지 반복한다.
    static Result<PathComponentRange> Components(StringView path,
                                                 PathStyle style = HostStyle()) noexcept;

    // 끝 구분자를 무시한 마지막 구성 요소다(UE FPathViews::GetPathLeaf와 같은 용도).
    //   "a/b/" → "b",  "a/b.txt" → "b.txt",  "/"·"" → 빈 view,  "C:x" → "x"
    // FileName과 다른 점: FileName은 끝 구분자가 있으면 빈 값이고 루트도 이름으로 볼 수 있다
    // ("C:a" → "C:a", "\\\\server\\share" → "share"). PathLeaf는 루트를 구성 요소로 보지 않는다
    // ("C:a" → "a", "\\\\server\\share" → 빈 view). 오류는 RootPart와 같다.
    static Result<StringView> PathLeaf(StringView path, PathStyle style = HostStyle()) noexcept;

    // 구분자를 대상 플랫폼 표기로 바꾼 사본을 만든다(UE FPaths::MakePlatformFilename과 같은 용도).
    //   Windows 문법: "C:/Game/Saved" → "C:\\Game\\Saved"
    //   POSIX 문법: 그대로 복사한다. '\\'는 POSIX 파일 이름에 쓸 수 있는 문자이므로 바꾸지
    //   않는다(UE는 바꿀 수 있다).
    // 구분자 외의 표기('.'·'..'·반복 구분자)는 정규화하지 않는다. 오류는 RootPart와 같으며, 바꾼
    // 결과가 device·NT namespace가 되는 입력("//?/…", "/??/…")도 Unsupported다.
    static Result<String> MakePlatformFilename(Allocator& allocator, StringView path,
                                               PathStyle style = HostStyle()) noexcept;
};

} // namespace rocks
