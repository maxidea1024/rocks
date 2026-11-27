#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <Core/Text/Json.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>

// 텍스트·JSON codec의 진단 레코드와 모음이다(리플렉션 B9i — Docs/Design/ReflectionExtensions.ko.md
// 10.2절 "진단"). 대입 묶음(PrepareAssignments·Append)과 JSON import는 오류를 만나도 멈추지 않고
// 한도(TextCodecOptions::maxDiagnostics) 안에서 모두 모아 보고하며, 진단이 하나라도 있으면 아무것도
// 게시하지 않는다(사람이 고치는 설정 파일·명령행에 맞는 방식).
namespace rocks::reflection_text {

// 진단 종류다. 도구는 TextDiagnosticCodeName으로 고정 이름을 출력한다.
enum class TextDiagnosticCode : uint8 {
    // 경로·JSON 키가 스키마의 필드 도구 이름과 맞지 않는다(대소문자 무시).
    UnknownProperty,
    // 용도(TextUse)의 입력 제외 필드에 값을 주었다.
    ExcludedProperty,
    // 살아 있는 객체의 ReadOnly 필드를 다른 값으로 바꾸려 했다.
    ReadOnly,
    // 값 텍스트의 문법·형식이 종류에 맞지 않는다.
    InvalidValue,
    // 값이 종류·필드의 범위(정수 폭·MaxElements 등)를 넘는다.
    OutOfRange,
    // Map 키·Set 원소가 겹치거나 JSON 객체 키가 대소문자만 다르게 겹친다.
    DuplicateKey,
    // 한 묶음에서 같은 경로를 두 번 대입했다.
    DuplicateAssignment,
    // 숨김 열거자(EnumeratorFlags::Hidden)를 입력했다.
    HiddenEnumerator,
    // 타입 도구 이름·TypeId(ClassRef·InstancedStruct)를 찾지 못했다.
    UnknownType,
    // 참조 resolver가 텍스트를 풀지 못했다.
    Unresolved,
    // resolver 없이 non-null Hard·Weak 참조를 주는 등 이 설정으로 할 수 없는 입력이다.
    Unsupported,
    // 바뀐 값을 담은 struct의 검증 hook이 게시 전에
    // 거부했다(Docs/Design/CurveData/README.ko.md 9.1절 — 예: 커브 키 시간이 정렬되지 않았다).
    // 경로는 검증한 단위(그 struct를 담은 필드·멤버)다.
    InvalidStructValue
};

// 진단 위치의 종류다.
enum class TextSourceKind : uint8 {
    // 위치 없음(옵션·묶음 전체에 대한 진단).
    None,
    // 명령행 대입 — TextAssignment::sourceIndex(입력 순번).
    Assignment,
    // JSON 입력 — JsonValue::Position()(byte offset·line·column).
    Json
};

// 진단 위치다. kind에 따라 sourceIndex 또는 position 하나만 뜻이 있다.
struct TextSource {
    TextSourceKind kind = TextSourceKind::None;
    uint32 sourceIndex = 0;
    JsonPosition position;

    static TextSource Assignment(uint32 sourceIndex) noexcept {
        TextSource source;
        source.kind = TextSourceKind::Assignment;
        source.sourceIndex = sourceIndex;
        return source;
    }

    static TextSource Json(JsonPosition position) noexcept {
        TextSource source;
        source.kind = TextSourceKind::Json;
        source.position = position;
        return source;
    }
};

// 진단 하나의 읽기 view다. path(도구 이름 경로 텍스트 — 비어 있을 수 있다)와 message(사람이 읽는
// 설명)는 그것을 돌려준 TextDiagnostics의 버퍼를 빌린다. 그 모음의 다음 Add·Clear·소멸까지
// 유효하다.
struct TextDiagnostic {
    TextDiagnosticCode code = TextDiagnosticCode::InvalidValue;
    TextSource source;
    StringView path;
    StringView message;
};

// 진단 이름(열거자 철자 그대로 — "UnknownProperty" 등)이다. 정의되지 않은 값이면 빈 view다.
ROCKS_REFLECTIONTEXT_API StringView TextDiagnosticCodeName(TextDiagnosticCode code) noexcept;

// 진단 모음 기본 한도다(TextCodecOptions::maxDiagnostics 기본값).
inline constexpr uint32 DefaultMaxTextDiagnostics = 256;

// 진단을 순서대로 모은다. 한도(Limit)까지 저장하고, 넘친 진단은 저장하지 않고 개수만 센다(Omitted).
// 모든 진단은 오류다(경고 종류가 없다) — HasErrors가 참이면 codec은 아무것도 게시하지 않는다.
//
// 수명: 대입 묶음(PreparedAssignments)은 Prepare에 받은 모음을 가리켜 Append의 진단도 그곳에
// 쌓는다. 그래서 모음은 묶음보다 오래 살아야 하고 옮길 수 없다(복사·이동 금지). 스레드: 한
// 스레드에서 쓴다(동기화하지 않는다).
//
// 사용 예:
//   TextDiagnostics diagnostics(allocator);
//   auto prepared = PrepareAssignments(target, assignments, options, diagnostics);
//   for (usize i = 0; i < diagnostics.Count(); ++i) {
//       const TextDiagnostic item = diagnostics.Get(i);
//       // TextDiagnosticCodeName(item.code), item.source, item.path, item.message 출력
//   }
//   if (diagnostics.Omitted() != 0) {
//       // "... and N more" 출력
//   }
class ROCKS_REFLECTIONTEXT_API TextDiagnostics {
public:
    // allocator는 진단 텍스트·레코드 버퍼를 할당한다(모음보다 오래 살아야 한다). 한도는
    // DefaultMaxTextDiagnostics로 시작한다.
    explicit TextDiagnostics(Allocator& allocator) noexcept;

    TextDiagnostics(const TextDiagnostics&) = delete;
    TextDiagnostics& operator=(const TextDiagnostics&) = delete;
    TextDiagnostics(TextDiagnostics&&) = delete;
    TextDiagnostics& operator=(TextDiagnostics&&) = delete;

    // 저장 한도를 정한다. codec 진입점(PrepareAssignments·ImportJson)이
    // TextCodecOptions::maxDiagnostics로 부른다. 이미 저장한 진단은 그대로 두고 새 진단부터
    // 적용한다(지금 개수가 새 한도 이상이면 Full).
    // 전제: maxDiagnostics는 1 이상(ValidateTextCodecOptions가 검사한다) — 0이면 Fatal.
    void SetLimit(uint32 maxDiagnostics) noexcept;

    uint32 Limit() const noexcept {
        return limit_;
    }

    // 진단 하나를 더한다. path·message는 복사한다(호출 동안만 빌린다). 한도에 찼으면 저장하지 않고
    // Omitted를 1 올린 뒤 성공한다(포화 — uint64 최댓값에서 멈춘다).
    // 오류: code·source.kind가 정의되지 않은 값이면 InvalidArgument, 텍스트 합이 usize를 넘으면
    // Overflow, 할당 실패는 allocator 오류(OutOfMemory 등). 실패해도 저장한 진단의 내용은 바뀌지
    // 않는다(버퍼를 다시 잡았을 수 있으므로 이전 view는 Get으로 다시 얻는다).
    Result<void> Add(TextDiagnosticCode code, TextSource source, StringView path,
                     StringView message) noexcept;

    // 저장한 진단 수다(≤ Limit 또는 SetLimit 전의 개수).
    usize Count() const noexcept {
        return entries_.Size();
    }

    // index번째 진단의 view다(Add 순서). 전제: index < Count() — 아니면 Fatal.
    TextDiagnostic Get(usize index) const noexcept;

    // 한도 때문에 저장하지 못한 진단 수다.
    uint64 Omitted() const noexcept {
        return omitted_;
    }

    // 다음 Add가 저장되지 않는지(Count ≥ Limit) 본다. codec은 Full이면 남은 입력의 해석을 멈춰도
    // 된다.
    bool Full() const noexcept {
        return entries_.Size() >= limit_;
    }

    // 진단이 하나라도 있었는지(저장했거나 넘쳤거나) 본다.
    bool HasErrors() const noexcept {
        return entries_.Size() != 0 || omitted_ != 0;
    }

    // 모든 진단과 Omitted를 지운다. 버퍼는 재사용하고 한도는 그대로다. 돌려준 view는 무효가 된다.
    void Clear() noexcept;

private:
    struct Entry {
        TextDiagnosticCode code;
        TextSource source;
        usize textOffset;
        usize pathBytes;
        usize messageBytes;
    };

    String text_;
    Array<Entry> entries_;
    uint64 omitted_ = 0;
    uint32 limit_ = DefaultMaxTextDiagnostics;
};

} // namespace rocks::reflection_text
