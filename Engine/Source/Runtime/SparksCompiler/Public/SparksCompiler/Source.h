#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>

// Sparks 원문 관리다(Docs/Design/SparksLanguage.ko.md 8.1절 "위치"·"원문 검사").
//
// 언제 쓰나: 컴파일러가 원문을 받는 첫 단계다. 원문 하나를 SourceFile로 보관하고 byte 위치를
// 줄·열로 바꾼다. 여러 파일은 SourceSet이 소유하고 진단은 SourceId로 파일을 가리킨다(파일 배열이
// 커져도 진단이 무효가 되지 않게 주소 대신 번호를 쓴다).
//
// 사용 예:
//   SourceSet sources(allocator);
//   DiagnosticList diagnostics(allocator);
//   auto id = sources.TryAdd("Props/Door.spark", text, diagnostics); // 원문 검사 진단이 붙는다
//   if (id && sources.Get(id.Value()).IsUsable()) { /* 어휘 분석 */ }
//
// 원문 검사(TryAdd가 한다):
// - 처음의 UTF-8 BOM(EF BB BF)은 허용하고 무시한다. 첫 줄의 열은 BOM 뒤부터 센다.
// - 16 MiB(MaxSourceBytes)를 넘으면 S0103이며 원문을 복사하지 않는다.
// - UTF-8이 아니면 S0101(처음 잘못된 byte 하나만).
// - LF가 뒤따르지 않는 CR은 S0102(처음 하나만). S0101이 있으면 이 검사는 하지 않는다.
// - S0101·S0103이 있으면 IsUsable()이 거짓이고 호출자는 뒤 단계를 돌리지 않는다.
//
// 수명·스레드: SourceSet과 그 SourceFile은 생성 allocator를 빌린다. allocator가 더 오래 살아야
// 한다. 한 SourceSet은 한 스레드에서 쓴다(읽기만 하는 동안은 여러 스레드가 Get·Locate를 불러도
// 된다).

namespace rocks::sparks {

class DiagnosticList;

// 원문 크기 상한이다(S0103). 위치를 uint32로 담고 한 모듈이 비정상적으로 커지는 것을 막는다.
inline constexpr usize MaxSourceBytes = usize{16} * 1024 * 1024;

// 원문 처음에 올 수 있는 UTF-8 BOM의 byte 수다.
inline constexpr usize Utf8BomBytes = 3;

// 원문 안의 byte 범위 [begin, end)다. begin == end면 그 위치의 빈 범위다(진단에는 `^` 하나로
// 보인다).
struct SourceSpan {
    uint32 begin = 0;
    uint32 end = 0;
};

// 1부터 세는 줄과 열이다. 열은 그 줄 처음부터 센 Unicode code point 수이며 tab도 1이다.
struct SourceLocation {
    uint32 line = 0;
    uint32 column = 0;
};

// SourceSet 안 파일의 번호다(추가한 순서, 0부터).
struct SourceId {
    uint32 index = 0;

    friend constexpr bool operator==(SourceId left, SourceId right) noexcept {
        return left.index == right.index;
    }
};

// 원문 하나다. SourceSet::TryAdd만 만든다. 이동은 SourceSet의 배열이 커질 때만 일어난다.
class SourceFile final {
public:
    SourceFile(SourceFile&&) noexcept = default;
    SourceFile& operator=(SourceFile&&) noexcept = default;
    SourceFile(const SourceFile&) = delete;
    SourceFile& operator=(const SourceFile&) = delete;
    ~SourceFile() noexcept = default;

    // 진단 첫 줄에 그대로 나오는 경로다(패키지 기준, '/' 구분 — 검사하지 않는다).
    StringView Path() const noexcept {
        return path_.View();
    }

    // 원문 전체 byte다(BOM 포함). UTF-8이 아닐 수 있다(S0101). 크기 초과(S0103)면 비어 있다.
    StringView Text() const noexcept {
        return StringView(text_.Data(), text_.Size());
    }

    // 원문 검사에서 뒤 단계를 막는 오류(S0101·S0103)가 없었다.
    bool IsUsable() const noexcept {
        return usable_;
    }

    // BOM 뒤의 첫 byte 위치다(BOM이 없으면 0).
    uint32 ContentBegin() const noexcept {
        return contentBegin_;
    }

    // 줄 수다. 빈 원문도 1줄이고, LF로 끝나는 원문은 마지막에 빈 줄이 하나 더 있다.
    uint32 LineCount() const noexcept {
        return static_cast<uint32>(lineStarts_.Size());
    }

    // byte 위치를 줄·열로 바꾼다. offset은 0부터 Text().ByteCount()까지(끝 위치 포함)다. 범위 밖은
    // 호출자 버그이므로 Fatal이다. BOM 안의 위치는 1줄 1열이다.
    SourceLocation Locate(uint32 offset) const noexcept;

    // 줄의 첫 byte 위치다. line은 1부터 LineCount()까지이며 범위 밖은 Fatal이다.
    uint32 LineBegin(uint32 line) const noexcept;

    // 줄 내용이다(줄 끝 LF와 그 앞의 CR 제외). line 범위는 LineBegin과 같다.
    StringView LineText(uint32 line) const noexcept;

private:
    friend class SourceSet;

    explicit SourceFile(Allocator& allocator) noexcept;

    String path_;
    Array<char> text_;
    Array<uint32> lineStarts_;
    uint32 contentBegin_ = 0;
    bool usable_ = false;
};

// 한 번의 컴파일이 다루는 원문 묶음이다. 파일을 더할 때 원문 검사를 하고 진단을 붙인다.
class SourceSet final {
public:
    explicit SourceSet(Allocator& allocator) noexcept;

    SourceSet(const SourceSet&) = delete;
    SourceSet& operator=(const SourceSet&) = delete;
    SourceSet(SourceSet&&) = delete;
    SourceSet& operator=(SourceSet&&) = delete;
    ~SourceSet() noexcept = default;

    // 원문을 복사해 더하고 원문 검사 진단을 diagnostics에 붙인다. path와 text는 호출 중에만 빌린다.
    // 검사에 걸려도 파일은 더해진다(진단이 그 파일을 가리켜야 하므로). 뒤 단계를 돌려도 되는지는
    // IsUsable()로 본다.
    // 실패: path가 UTF-8이 아니면 InvalidArgument, 파일 수가 uint32를 넘으면 CapacityExceeded,
    // 할당 실패는 그 오류. 실패하면 SourceSet과 diagnostics는 바뀌지 않는다.
    Result<SourceId> TryAdd(StringView path, StringView text, DiagnosticList& diagnostics) noexcept;

    // 파일이다. 이 SourceSet이 돌려준 id만 쓴다. 범위 밖은 Fatal이다. 참조는 다음 TryAdd까지
    // 유효하다.
    const SourceFile& Get(SourceId id) const noexcept;

    uint32 Count() const noexcept {
        return static_cast<uint32>(files_.Size());
    }

private:
    Allocator* allocator_;
    Array<SourceFile> files_;
};

} // namespace rocks::sparks
