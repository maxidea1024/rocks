#include "LocalizationInternal.h"

namespace rocks::detail {

class TextResolver {
public:
    static Result<ResolvedText> Run(Allocator& allocator, const Text& text,
                                    const CatalogSnapshot& catalog,
                                    TextResolveLimits limits) noexcept {
        if (!text.node_ || limits.maxDepth > 64) {
            return Result<ResolvedText>::Failure(ErrorCode::InvalidArgument);
        }
        ResolvedText result(allocator);
        TextResolver resolver(allocator, catalog.data_, limits, result.diagnostics);
        auto resolved = resolver.Visit(text.node_, result.value, 1);
        if (!resolved) {
            return Result<ResolvedText>::Failure(resolved.Error());
        }
        return Result<ResolvedText>::Success(Move(result));
    }

private:
    TextResolver(Allocator& allocator, const CatalogData* catalog, TextResolveLimits limits,
                 TextDiagnostics& diagnostics) noexcept
        : allocator_(allocator), catalog_(catalog), limits_(limits), remaining_(limits.maxWork),
          diagnostics_(diagnostics) {
    }

    // 뺄셈 방식으로 budget을 소비해 검사 전에 누적 계수가 wrap하지 않는다.
    bool Spend(uint64 count) noexcept {
        if (count > remaining_) {
            return false;
        }
        remaining_ -= count;
        return true;
    }

    Result<void> Append(String& output, StringView text) noexcept {
        if (output.ByteCount() > limits_.maxOutputBytes ||
            text.ByteCount() > limits_.maxOutputBytes - output.ByteCount() ||
            !Spend(text.ByteCount())) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }
        return output.TryAppend(text);
    }

    Result<StringView> Translate(const TextNode& node) noexcept {
        if (catalog_) {
            for (usize index = 0; index < catalog_->entries.Size(); ++index) {
                if (!Spend(1)) {
                    return Result<StringView>::Failure(ErrorCode::CapacityExceeded);
                }
                const auto& entry = catalog_->entries[index];
                if (entry.id == node.id) {
                    if (entry.sourceRevision == node.sourceRevision) {
                        return Result<StringView>::Success(entry.translation.View());
                    }
                    if (diagnostics_.staleTranslations == ~uint64(0)) {
                        return Result<StringView>::Failure(ErrorCode::Overflow);
                    }
                    ++diagnostics_.staleTranslations;
                    return Result<StringView>::Success(node.source.View());
                }
            }
        }
        if (diagnostics_.missingTranslations == ~uint64(0)) {
            return Result<StringView>::Failure(ErrorCode::Overflow);
        }
        ++diagnostics_.missingTranslations;
        return Result<StringView>::Success(node.source.View());
    }

    Result<void> Visit(const TextNode* node, String& output, uint32 depth) noexcept {
        if (depth > limits_.maxDepth || !Spend(1)) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }
        if (node->kind == TextKind::Format) {
            return Format(*node, output, depth);
        }
        StringView source = node->source.View();
        if (node->kind == TextKind::Localized) {
            auto translated = Translate(*node);
            if (!translated) {
                return Result<void>::Failure(translated.Error());
            }
            source = translated.Value();
        }
        // String append의 UTF 검사와 출력 복사에 대해 각각 byte 단위 work를 부과한다.
        if (!Spend(source.ByteCount())) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }
        return Append(output, source);
    }

    Result<const TextNode*> FindArgument(const TextNode& node, StringView name) noexcept {
        for (usize index = 0; index < node.arguments.Size(); ++index) {
            if (!Spend(1)) {
                return Result<const TextNode*>::Failure(ErrorCode::CapacityExceeded);
            }
            const StringView candidate = node.arguments[index].name.View();
            if (candidate.ByteCount() != name.ByteCount()) {
                continue;
            }
            if (!Spend(name.ByteCount())) {
                return Result<const TextNode*>::Failure(ErrorCode::CapacityExceeded);
            }
            if (candidate == name) {
                return Result<const TextNode*>::Success(node.arguments[index].value);
            }
        }
        return Result<const TextNode*>::Failure(ErrorCode::InvalidArgument);
    }

    Result<void> Format(const TextNode& node, String& output, uint32 depth) noexcept {
        // 번역 선택과 중첩 이력을 먼저 해석한 pattern만 문법 검사한다. fallback도 같은 문법을
        // 따른다. maxDepth<=64와 전역 work budget으로 C++ 재귀와 임시 문자열의 총 확장을 제한한다.
        String pattern(allocator_);
        auto patternResult = Visit(node.pattern, pattern, depth + 1);
        if (!patternResult) {
            return patternResult;
        }
        const StringView view = pattern.View();
        usize position = 0;
        usize literal = 0;
        while (position < view.ByteCount()) {
            if (!Spend(1)) {
                return Result<void>::Failure(ErrorCode::CapacityExceeded);
            }
            const char value = view.Data()[position];
            if (value != '{' && value != '}') {
                ++position;
                continue;
            }
            // brace는 ASCII이므로 literal run의 양 끝은 UTF-8 scalar 경계다.
            // 한글/emoji byte를 개별 TryAppend에 넣어 잘못된 UTF-8로 거부하는 일을 피한다.
            auto run = Append(output, StringView(view.Data() + literal, position - literal));
            if (!run) {
                return run;
            }
            if (position + 1 < view.ByteCount() && view.Data()[position + 1] == value) {
                if (!Spend(1)) {
                    return Result<void>::Failure(ErrorCode::CapacityExceeded);
                }
                auto escaped = Append(output, StringView(view.Data() + position, 1));
                if (!escaped) {
                    return escaped;
                }
                position += 2;
                literal = position;
                continue;
            }
            if (value == '}') {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
            const usize start = ++position;
            while (position < view.ByteCount()) {
                if (!Spend(1)) {
                    return Result<void>::Failure(ErrorCode::CapacityExceeded);
                }
                if (view.Data()[position] == '}') {
                    break;
                }
                if (position - start >= 64) {
                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }
                ++position;
            }
            if (position == view.ByteCount()) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
            const StringView name(view.Data() + start, position - start);
            if (!Spend(name.ByteCount())) {
                return Result<void>::Failure(ErrorCode::CapacityExceeded);
            }
            if (!IsArgumentName(name)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
            auto argument = FindArgument(node, name);
            if (!argument) {
                return Result<void>::Failure(argument.Error());
            }
            // 인자의 해석 결과를 output으로 직접 보낸다. 그 결과의 brace를 이 parser에 되먹이지
            // 않는다.
            auto inserted = Visit(argument.Value(), output, depth + 1);
            if (!inserted) {
                return inserted;
            }
            literal = ++position;
        }
        return Append(output, StringView(view.Data() + literal, view.ByteCount() - literal));
    }

    Allocator& allocator_;
    const CatalogData* catalog_;
    TextResolveLimits limits_;
    uint64 remaining_;
    TextDiagnostics& diagnostics_;
};

} // namespace rocks::detail

namespace rocks {

Result<ResolvedText> ResolveText(Allocator& allocator, const Text& text,
                                 const CatalogSnapshot& catalog,
                                 TextResolveLimits limits) noexcept {
    return detail::TextResolver::Run(allocator, text, catalog, limits);
}

} // namespace rocks
