#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/NameComponent.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>

// 에셋의 논리 경로다(Docs/Design/AssetSystem 4.2절, 7.1절 10 — A2a). 형식은
// `/<Mount>/<Dir>/.../<Name>`이고 진실의 원천은 파일 위치다(에디터 패키지는 경로를 저장하지
// 않는다).
//
// 규칙(판정 순서대로 — 앞 단계 위반이 이긴다):
//   1. 비지 않고 '/'로 시작한다. '\'는 쓸 수 없다(구분자는 '/'만).
//   2. 빈 구성 요소('//')와 끝 '/'가 없다. 구성 요소는 둘 이상이다(마운트 + 이름).
//   3. 첫 구성 요소는 등록된 마운트다(지금은 `/Game` 하나 — 대소문자 무시로 맞추고 표기는 마운트의
//      철자로 바꾼다. UE FPackageName 마운트처럼).
//   4. 나머지 구성 요소마다 NFC로 정규화한 뒤 Core 이름 구성 요소 규칙(ValidateNameComponent — 1–64
//      byte, L*·N*·M*·'_'·'-', 첫 문자 L*·N*, Default_Ignorable·Windows 예약 이름 거부)을 지킨다.
//      그래서 '.'·'..'·공백·구두점은 들어갈 수 없다.
//   5. 정규화한 전체가 256 byte 이하다.
// 비교 키는 구성 요소마다 TryMakeNameComponentKey(NFC(simple case folding(NFD))) 키를 '/'로 이은
// 것이다(쿠킹 매니페스트 경로 힌트 키와 같은 방식 — 10.1절). 같은 키는 같은 경로다.
//
// 사용 예:
//   LogicalPathFailure why;
//   auto path = ParseLogicalPath(allocator, "/game/Characters/Hero", &why);
//   if (!path) { /* why.error·why.component로 InvalidPath 진단 */ }
//   path.Value().Text();   // "/Game/Characters/Hero"
//   path.Value().Key();    // "game/characters/hero"
//   path.Value().Name();   // "Hero"
namespace rocks {

// 논리 경로 전체(정규화 뒤 UTF-8)의 byte 상한이다(4.2절).
inline constexpr usize MaxLogicalPathBytes = 256;
// 프로젝트 콘텐츠 마운트다(UE `/Game/` → 프로젝트 Content).
inline constexpr StringView GameMount = "/Game";

// ParseLogicalPath가 거부한 사유다(규칙 번호 순).
enum class LogicalPathError : uint8 {
    None,
    Empty, // 빈 문자열
    MissingLeadingSlash,
    Backslash,        // '\' 구분자
    EmptyComponent,   // '//' 또는 끝 '/'
    MissingName,      // 마운트뿐이다
    UnknownMount,     // 등록되지 않은 마운트
    InvalidComponent, // 이름 구성 요소 규칙 위반(componentError·component·offset)
    TooLong,          // 정규화 뒤 256 byte 초과
    NotNfcOnDisk      // 파일 이름이 NFC가 아니고 파일 시스템이 이름을 정규화하지 않는다(7.1절 10)
};

// 거부 사유와 위치다. component는 0부터(0 = 마운트), offset은 그 구성 요소 안 byte 위치(규칙 4의
// 위반만 — ValidateNameComponent의 offset), 성공·할당 실패면 모두 0·None이다.
struct LogicalPathFailure {
    LogicalPathError error = LogicalPathError::None;
    uint32 component = 0;
    NameComponentError componentError = NameComponentError::None;
    usize offset = 0;
};

// 사유 이름이다(예: TooLong → "TooLong" — 진단·로그용, 정적 수명). 모르는 값은 "Unknown"이다.
ROCKS_ASSETDATABASE_API const char* LogicalPathErrorName(LogicalPathError error) noexcept;

// 검증·정규화한 논리 경로다. 값 타입이고 문자열은 allocator가 가진다.
class ROCKS_ASSETDATABASE_API LogicalPath {
public:
    LogicalPath(LogicalPath&&) noexcept = default;
    LogicalPath& operator=(LogicalPath&&) noexcept = default;
    LogicalPath(const LogicalPath&) = delete;
    LogicalPath& operator=(const LogicalPath&) = delete;

    // 같은 내용의 복사본이다. 오류: 할당 실패.
    Result<LogicalPath> TryClone() const noexcept;

    // 정규화한 경로("/Game/Dir/Name")다.
    StringView Text() const noexcept {
        return text_.View();
    }

    // 비교 키("game/dir/name")다.
    StringView Key() const noexcept {
        return key_.View();
    }

    // 마운트("/Game")다.
    StringView Mount() const noexcept;
    // 마운트 뒤 상대 부분("Dir/Name")이다.
    StringView Relative() const noexcept;
    // 마지막 구성 요소("Name")다.
    StringView Name() const noexcept;
    // 마지막 구성 요소 앞의 경로("/Game/Dir" — 마운트 바로 아래면 "/Game")다.
    StringView Directory() const noexcept;

    friend bool operator==(const LogicalPath& left, const LogicalPath& right) noexcept {
        return left.Key() == right.Key();
    }

private:
    // 빈 경로는 ParseLogicalPath·TryClone 안에서만 만든다(공개 생성자가 없다 — 모든 공개 값은
    // 검증한 경로다). 옮겨 간 값은 비어 있고 접근자는 빈 view를 준다.
    explicit LogicalPath(Allocator& allocator) noexcept;

    friend ROCKS_ASSETDATABASE_API Result<LogicalPath>
    ParseLogicalPath(Allocator&, StringView, LogicalPathFailure*) noexcept;

    String text_;
    String key_;
    usize mountBytes_ = 0;
};

// text를 규칙대로 검사·정규화한다. 오류: 규칙 위반 InvalidArgument(failure에 사유), 할당 실패는 그
// 오류(failure는 None).
ROCKS_ASSETDATABASE_API Result<LogicalPath>
ParseLogicalPath(Allocator& allocator, StringView text,
                 LogicalPathFailure* failure = nullptr) noexcept;

} // namespace rocks
