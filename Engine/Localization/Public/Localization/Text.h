#pragma once

#include <Localization/Catalog.h>

namespace rocks {

namespace detail {
struct TextNode;
}

class Text;

// Format 호출 동안만 빌린다. 이름은 복사하고 value의 불변 이력을 보존한다.
struct TextArgument {
    StringView name;
    const Text* value = nullptr;
};

struct TextCreateLimits {
    usize maxSourceBytes = 1024 * 1024;
    uint32 maxArguments = 64;
};

// 불변 문구 이력 DAG의 이동 전용 소유권이다. 이미 존재하는 노드만 참조하므로 cycle을 만들 수 없다.
// Clone은 깊은 복사가 아닌 공유 보존이며 각 allocator는 마지막 노드 반환까지 살아 있어야 한다.
// 별도 래퍼의 읽기/Clone/Reset은 동시 호출 가능하나 같은 래퍼의 변경은 동기화한다.
// allocator는 실제 생성·해제 스레드를 지원해야 한다. allocator 콜백에서 같은 래퍼 변경은 금지한다.
class Text {
public:
    Text() noexcept = default;
    Text(Text&&) noexcept;
    Text& operator=(Text&&) noexcept;
    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;
    ~Text() noexcept;

    // 모든 원문 byte를 소유한다. source는 유효 UTF-8이어야 하며 내부 NUL도 길이 기반으로 보존한다.
    static Result<Text> Invariant(Allocator&, StringView, TextCreateLimits = {}) noexcept;
    static Result<Text> Localized(Allocator&, TextId, uint64 sourceRevision, StringView fallback,
                                  TextCreateLimits = {}) noexcept;
    // locale 독립 ASCII 10진수다. grouping/plural/통화 등 언어별 숫자 서식은 제공하지 않는다.
    static Result<Text> Integer(Allocator&, int64) noexcept;
    // 이름은 ASCII [A-Za-z_][A-Za-z0-9_]*, 최대 64 byte, 중복 금지다. value는 valid Text여야 한다.
    // 이름/value 오류와 중복 이름은 InvalidArgument다.
    // 초기 hard ceiling은 source 16 MiB, args 64개다. 실패해도 pattern/인자는 변경하지 않는다.
    // 문법과 missing args는 번역 선택 후 Resolve에서 검사하며 unused args는 허용한다.
    static Result<Text> Format(Allocator&, const Text& pattern, Span<const TextArgument>,
                               TextCreateLimits = {}) noexcept;
    Result<Text> Clone() const noexcept;
    void Reset() noexcept;
    bool IsValid() const noexcept;

private:
    explicit Text(detail::TextNode* node) noexcept : node_(node) {
    }

    detail::TextNode* node_ = nullptr;
    friend class detail::TextResolver;
};

struct TextResolveLimits {
    usize maxOutputBytes = 1024 * 1024;
    uint32 maxDepth = 64;
    uint64 maxWork = 8 * 1024 * 1024;
};

struct TextDiagnostics {
    uint64 missingTranslations = 0;
    uint64 staleTranslations = 0;
};

struct ResolvedText {
    explicit ResolvedText(Allocator& allocator) noexcept : value(allocator) {
    }

    ResolvedText(ResolvedText&&) noexcept = default;
    ResolvedText& operator=(ResolvedText&&) noexcept = default;
    ResolvedText(const ResolvedText&) = delete;
    ResolvedText& operator=(const ResolvedText&) = delete;

    String value;
    TextDiagnostics diagnostics;
};

// 명시 snapshot의 culture만 사용한다. invalid snapshot은 번역 없음이며 localized 원문으로
// fallback한다. id 누락은 missing, sourceRevision 불일치는 stale를 방문 횟수대로 센다. invariant는
// 진단 없이 해석한다. 문법은 {name}, {{, }}만 허용한다. 불완전 brace/없는 이름은 InvalidArgument다.
// 삽입 인자의 결과에 있는 brace는 다시 파싱하지 않는다. 결과 String은 입력/스냅샷과 독립 소유다.
// maxDepth는 root=1, pattern/인자 방문마다 +1이며 64 초과 설정은 InvalidArgument다.
// work는 노드 방문·원문/패턴 byte 검사·출력 byte·catalog/arg 후보 방문·arg 이름 비교 byte를
// 누적한다. output은 각 중간/최종 String의 payload 한도다. budget 초과는 CapacityExceeded, OOM은
// OutOfMemory다. 실패는 부분 결과를 반환하지 않는다. 입력 래퍼와 allocator는 호출 동안 유효해야
// 한다.
Result<ResolvedText> ResolveText(Allocator&, const Text&, const CatalogSnapshot&,
                                 TextResolveLimits = {}) noexcept;

} // namespace rocks
