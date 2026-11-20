#include <SparksCompiler/Diagnostic.h>

#include <Core/Algorithms.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>

#include "TextColumns.h"

namespace rocks::sparks {

namespace {

// 번호 오름차순이다. 새 코드는 번호 자리에 끼워 넣는다(FindDiagnosticCode가 이분 탐색한다).
constexpr DiagnosticCodeInfo CodeTable[] = {
    {DiagnosticCode::InvalidUtf8, "invalid-utf8"},
    {DiagnosticCode::LoneCarriageReturn, "lone-carriage-return"},
    {DiagnosticCode::SourceTooLarge, "source-too-large"},
    {DiagnosticCode::InvalidCharacter, "invalid-character"},
    {DiagnosticCode::UnderscoreInIdentifier, "underscore-in-identifier"},
    {DiagnosticCode::UnterminatedBlockComment, "unterminated-block-comment"},
    {DiagnosticCode::UnterminatedString, "unterminated-string"},
    {DiagnosticCode::InvalidEscape, "invalid-escape"},
    {DiagnosticCode::IntegerLiteralTooLarge, "integer-literal-too-large"},
    {DiagnosticCode::MalformedNumber, "malformed-number"},
    {DiagnosticCode::StringInInterpolation, "string-in-interpolation"},
    {DiagnosticCode::RangeOperator, "range-operator"},
    {DiagnosticCode::FloatLiteralOutOfRange, "float-literal-out-of-range"},
    {DiagnosticCode::NumericSuffix, "numeric-suffix"},
    {DiagnosticCode::ExpectedToken, "expected-token"},
    {DiagnosticCode::ExpectedExpression, "expected-expression"},
    {DiagnosticCode::ExpectedDeclaration, "expected-declaration"},
    {DiagnosticCode::ExpectedMember, "expected-member"},
    {DiagnosticCode::ExpectedStatement, "expected-statement"},
    {DiagnosticCode::ExpectedType, "expected-type"},
    {DiagnosticCode::ExpectedName, "expected-name"},
    {DiagnosticCode::MisplacedDocComment, "misplaced-doc-comment"},
    {DiagnosticCode::ChainedComparison, "chained-comparison"},
    {DiagnosticCode::ImportAfterDeclaration, "import-after-declaration"},
    {DiagnosticCode::InvalidAssignmentTarget, "invalid-assignment-target"},
    {DiagnosticCode::MisplacedAwait, "misplaced-await"},
    {DiagnosticCode::MisplacedExit, "misplaced-exit"},
    {DiagnosticCode::MisplacedAttribute, "misplaced-attribute"},
    {DiagnosticCode::NestingTooDeep, "nesting-too-deep"},
    {DiagnosticCode::RangeOutsideFor, "range-outside-for"},
    {DiagnosticCode::MisplacedDefault, "misplaced-default"},
    {DiagnosticCode::MixedCollectionLiteral, "mixed-collection-literal"},
    {DiagnosticCode::EmptyEnum, "empty-enum"},
    {DiagnosticCode::UnknownModule, "unknown-module"},
    {DiagnosticCode::UndeclaredDependency, "undeclared-dependency"},
    {DiagnosticCode::DuplicateImport, "duplicate-import"},
    {DiagnosticCode::SelfImport, "self-import"},
    {DiagnosticCode::RocksImport, "rocks-import"},
    {DiagnosticCode::NamingRule, "naming-rule"},
    {DiagnosticCode::ReservedName, "reserved-name"},
    {DiagnosticCode::HookName, "hook-name"},
    {DiagnosticCode::DuplicateDeclaration, "duplicate-declaration"},
    {DiagnosticCode::NameConflict, "name-conflict"},
    {DiagnosticCode::ImportConflict, "import-conflict"},
    {DiagnosticCode::MemberTypeName, "member-type-name"},
    {DiagnosticCode::HidesInheritedMember, "hides-inherited-member"},
    {DiagnosticCode::OverrideWithoutTarget, "override-without-target"},
    {DiagnosticCode::OverrideSignatureMismatch, "override-signature-mismatch"},
    {DiagnosticCode::MisplacedOpen, "misplaced-open"},
    {DiagnosticCode::UnknownName, "unknown-name"},
    {DiagnosticCode::UnknownType, "unknown-type"},
    {DiagnosticCode::NotAType, "not-a-type"},
    {DiagnosticCode::TypeArgumentCount, "type-argument-count"},
    {DiagnosticCode::ContextNameUnavailable, "context-name-unavailable"},
    {DiagnosticCode::InvalidBase, "invalid-base"},
    {DiagnosticCode::InheritanceCycle, "inheritance-cycle"},
    {DiagnosticCode::InterfaceNotImplemented, "interface-not-implemented"},
    {DiagnosticCode::InvalidService, "invalid-service"},
    {DiagnosticCode::UnknownMember, "unknown-member"},
    {DiagnosticCode::TypeMismatch, "type-mismatch"},
    {DiagnosticCode::NotCallable, "not-callable"},
    {DiagnosticCode::ArgumentCount, "argument-count"},
    {DiagnosticCode::ArgumentForm, "argument-form"},
    {DiagnosticCode::NotAValue, "not-a-value"},
    {DiagnosticCode::InvalidOperator, "invalid-operator"},
    {DiagnosticCode::NotComparable, "not-comparable"},
    {DiagnosticCode::ConditionNotBool, "condition-not-bool"},
    {DiagnosticCode::MissingReturn, "missing-return"},
    {DiagnosticCode::ReturnValue, "return-value"},
    {DiagnosticCode::TypeArgumentRule, "type-argument-rule"},
    {DiagnosticCode::MayBeNull, "may-be-null"},
    {DiagnosticCode::NotOptional, "not-optional"},
    {DiagnosticCode::SwitchCoverage, "switch-coverage"},
    {DiagnosticCode::DuplicateCase, "duplicate-case"},
    {DiagnosticCode::NoExpectedType, "no-expected-type"},
    {DiagnosticCode::MixedCollectionTypes, "mixed-collection-types"},
    {DiagnosticCode::InterpolationType, "interpolation-type"},
    {DiagnosticCode::InvalidCast, "invalid-cast"},
    {DiagnosticCode::AwaitTryOperand, "await-try-operand"},
    {DiagnosticCode::Unawaited, "unawaited"},
    {DiagnosticCode::ThisMisuse, "this-misuse"},
    {DiagnosticCode::ValidatedLiteralForm, "validated-literal-form"},
    {DiagnosticCode::StaticInstanceMismatch, "static-instance-mismatch"},
    {DiagnosticCode::IntegerDivision, "integer-division"},
    {DiagnosticCode::FloatToIntConversion, "float-to-int-conversion"},
    {DiagnosticCode::ConstantOverflow, "constant-overflow"},
    {DiagnosticCode::ConstantDivisionByZero, "constant-division-by-zero"},
    {DiagnosticCode::NonFiniteConstant, "non-finite-constant"},
    {DiagnosticCode::UnsignedNegation, "unsigned-negation"},
    {DiagnosticCode::MixedNumericTypes, "mixed-numeric-types"},
    {DiagnosticCode::InvalidConversion, "invalid-conversion"},
    {DiagnosticCode::FloatConstantInInteger, "float-constant-in-integer"},
    {DiagnosticCode::ManifestSyntax, "manifest-syntax"},
    {DiagnosticCode::UnknownManifestKey, "unknown-manifest-key"},
    {DiagnosticCode::MissingManifestKey, "missing-manifest-key"},
    {DiagnosticCode::InvalidManifestValue, "invalid-manifest-value"},
    {DiagnosticCode::UnsupportedLanguageVersion, "unsupported-language-version"},
    {DiagnosticCode::MissingDependency, "missing-dependency"},
    {DiagnosticCode::DependencyCycle, "dependency-cycle"},
    {DiagnosticCode::InvalidModulePath, "invalid-module-path"},
    {DiagnosticCode::ModulePathCaseConflict, "module-path-case-conflict"},
    {DiagnosticCode::ModuleFolderConflict, "module-folder-conflict"},
    {DiagnosticCode::PackageFolderConflict, "package-folder-conflict"},
    {DiagnosticCode::DuplicatePackage, "duplicate-package"},
    {DiagnosticCode::InvalidBindingSchema, "invalid-binding-schema"},
};

constexpr bool CodeTableIsSorted() noexcept {
    for (usize index = 1; index < sizeof(CodeTable) / sizeof(CodeTable[0]); ++index) {
        if (static_cast<uint16>(CodeTable[index - 1].code) >=
            static_cast<uint16>(CodeTable[index].code)) {
            return false;
        }
    }

    return true;
}

static_assert(CodeTableIsSorted(), "Sparks diagnostic code table must be sorted and unique");

// 8.1절 출력 형식의 들여쓰기다.
constexpr usize SourceLineIndent = 4;
constexpr usize NoteIndent = 2;
constexpr StringView FixPrefix = "fix: ";

// 표시에서 잘못된 UTF-8 byte 대신 쓰는 U+FFFD다.
constexpr uint32 ReplacementCharacter = 0xFFFDu;

// 원문 줄을 보일 때 공백으로 바꾸는 문자다: C0 제어 문자(tab·CR 포함), DEL, C1 제어 문자, 그리고
// 표시 순서를 바꾸는 bidi 제어 문자(ALM·LRM·RLM·LRE–RLO·LRI–PDI). 열 계산과 맞추고(한 문자 = 한 칸)
// 터미널 출력을 흐트러뜨리거나 원문을 다르게 보이게 하지 않으려는 것이다.
constexpr bool IsHiddenInDisplay(uint32 codePoint) noexcept {
    constexpr uint32 LastC0 = 0x1Fu;
    constexpr uint32 Delete = 0x7Fu;
    constexpr uint32 FirstC1 = 0x80u;
    constexpr uint32 LastC1 = 0x9Fu;
    constexpr uint32 ArabicLetterMark = 0x061Cu;
    constexpr uint32 LeftToRightMark = 0x200Eu;
    constexpr uint32 RightToLeftMark = 0x200Fu;
    constexpr uint32 FirstEmbedding = 0x202Au; // LRE·RLE·PDF·LRO·RLO
    constexpr uint32 LastEmbedding = 0x202Eu;
    constexpr uint32 FirstIsolate = 0x2066u; // LRI·RLI·FSI·PDI
    constexpr uint32 LastIsolate = 0x2069u;
    return codePoint <= LastC0 || codePoint == Delete ||
           (codePoint >= FirstC1 && codePoint <= LastC1) || codePoint == ArabicLetterMark ||
           codePoint == LeftToRightMark || codePoint == RightToLeftMark ||
           (codePoint >= FirstEmbedding && codePoint <= LastEmbedding) ||
           (codePoint >= FirstIsolate && codePoint <= LastIsolate);
}

// 원문 줄을 보이는 형태로 붙인다: 숨길 문자(IsHiddenInDisplay)는 공백, 잘못된 UTF-8 byte는 U+FFFD
// 하나씩.
Result<void> AppendDisplayLine(StringBuilderBase& out, StringView line) noexcept {
    usize offset = 0;
    while (offset < line.ByteCount()) {
        const StringView rest(line.Data() + offset, line.ByteCount() - offset);
        const usize invalid = rest.FindInvalidUtf8();
        const usize validBytes = invalid == IndexNone ? rest.ByteCount() : invalid;
        usize runBegin = 0;
        usize cursor = 0;
        while (cursor < validBytes) {
            usize width = 0;
            const uint32 codePoint =
                detail::DecodeCodePoint(rest.Data() + cursor, validBytes - cursor, width);
            if (!IsHiddenInDisplay(codePoint)) {
                cursor += width;
                continue;
            }

            if (auto appended =
                    out.TryAppend(StringView(rest.Data() + runBegin, cursor - runBegin));
                !appended) {
                return appended;
            }

            if (auto appended = out.TryAppendAscii(' '); !appended) {
                return appended;
            }

            cursor += width;
            runBegin = cursor;
        }

        if (auto appended =
                out.TryAppend(StringView(rest.Data() + runBegin, validBytes - runBegin));
            !appended) {
            return appended;
        }

        offset += validBytes;
        if (invalid != IndexNone) {
            if (auto appended = out.TryAppendCodePoint(ReplacementCharacter); !appended) {
                return appended;
            }

            ++offset;
        }
    }

    return Result<void>::Success();
}

// 설명을 줄마다 들여 쓴다. 끝의 LF는 빈 줄을 만들지 않고, 중간의 빈 줄은 들여쓰기 없이 쓴다.
Result<void> AppendNoteLines(StringBuilderBase& out, StringView text) noexcept {
    usize begin = 0;
    while (begin < text.ByteCount()) {
        usize end = begin;
        while (end < text.ByteCount() && text.Data()[end] != '\n') {
            ++end;
        }

        if (end > begin) {
            if (auto appended = out.TryAppendRepeated(' ', NoteIndent); !appended) {
                return appended;
            }
        }

        if (auto appended = out.TryAppend(StringView(text.Data() + begin, end - begin));
            !appended) {
            return appended;
        }

        if (auto appended = out.TryAppendAscii('\n'); !appended) {
            return appended;
        }

        begin = end + 1;
    }

    return Result<void>::Success();
}

uint32 CodeNumber(DiagnosticCode code) noexcept {
    return static_cast<uint16>(code);
}

} // namespace

Result<void> AppendDiagnosticCode(StringBuilderBase& out, uint32 number) noexcept {
    static_assert(DiagnosticCodeDigits == 4, "the format below writes four digits");
    return out.TryAppendFormat("S{:04}", number);
}

Span<const DiagnosticCodeInfo> AllDiagnosticCodes() noexcept {
    return Span<const DiagnosticCodeInfo>(CodeTable, sizeof(CodeTable) / sizeof(CodeTable[0]));
}

const DiagnosticCodeInfo* FindDiagnosticCode(uint32 number) noexcept {
    usize low = 0;
    usize high = sizeof(CodeTable) / sizeof(CodeTable[0]);
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const uint32 value = CodeNumber(CodeTable[middle].code);
        if (value == number) {
            return &CodeTable[middle];
        }

        if (value < number) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return nullptr;
}

Diagnostic::Diagnostic(Allocator& allocator) noexcept
    : message_(allocator), explanation_(allocator), fix_(allocator) {
}

DiagnosticList::DiagnosticList(Allocator& allocator) noexcept
    : allocator_(&allocator), items_(allocator) {
}

Result<Diagnostic> DiagnosticList::TryPrepare(DiagnosticCode code, SourceId source, bool hasSpan,
                                              SourceSpan span, StringView message,
                                              StringView explanation, StringView fix) noexcept {
    // 메시지와 수정 예는 한 줄이다(첫 줄 형식을 problem matcher가 읽는다). 설명만 여러 줄이다.
    const bool multiLine =
        message.Find("\n", StringComparison::CaseSensitive) != StringView::NotFound ||
        fix.Find("\n", StringComparison::CaseSensitive) != StringView::NotFound;
    if (message.IsEmpty() || multiLine || (hasSpan && span.begin > span.end)) {
        return Result<Diagnostic>::Failure(ErrorCode::InvalidArgument);
    }

    Diagnostic diagnostic(*allocator_);
    if (auto assigned = diagnostic.message_.TryAssign(message); !assigned) {
        return Result<Diagnostic>::Failure(assigned.Error());
    }

    if (auto assigned = diagnostic.explanation_.TryAssign(explanation); !assigned) {
        return Result<Diagnostic>::Failure(assigned.Error());
    }

    if (auto assigned = diagnostic.fix_.TryAssign(fix); !assigned) {
        return Result<Diagnostic>::Failure(assigned.Error());
    }

    diagnostic.code_ = code;
    diagnostic.source_ = source;
    diagnostic.hasSpan_ = hasSpan;
    diagnostic.span_ = hasSpan ? span : SourceSpan{};
    return Result<Diagnostic>::Success(Move(diagnostic));
}

Result<void> DiagnosticList::TryReserveMore(usize count) noexcept {
    return items_.TryEnsureCapacity(items_.Size() + count);
}

void DiagnosticList::Commit(Diagnostic&& diagnostic) noexcept {
    diagnostic.sequence_ = nextSequence_++;
    if (!items_.TryPushBack(Move(diagnostic))) {
        platform::Fatal("Sparks reserved diagnostic push failed");
    }
}

Result<void> DiagnosticList::TryAdd(DiagnosticCode code, SourceId source, SourceSpan span,
                                    StringView message, StringView explanation,
                                    StringView fix) noexcept {
    auto diagnostic = TryPrepare(code, source, true, span, message, explanation, fix);
    if (!diagnostic) {
        return Result<void>::Failure(diagnostic.Error());
    }

    if (auto reserved = TryReserveMore(1); !reserved) {
        return reserved;
    }

    Commit(Move(diagnostic.Value()));
    return Result<void>::Success();
}

Result<void> DiagnosticList::TryAddFileLevel(DiagnosticCode code, SourceId source,
                                             StringView message, StringView explanation,
                                             StringView fix) noexcept {
    auto diagnostic = TryPrepare(code, source, false, SourceSpan{}, message, explanation, fix);
    if (!diagnostic) {
        return Result<void>::Failure(diagnostic.Error());
    }

    if (auto reserved = TryReserveMore(1); !reserved) {
        return reserved;
    }

    Commit(Move(diagnostic.Value()));
    return Result<void>::Success();
}

void DiagnosticList::Sort(const SourceSet& sources) noexcept {
    algo::Sort(items_.View(), [&sources](const Diagnostic& left, const Diagnostic& right) noexcept {
        if (left.source_.index != right.source_.index) {
            const int paths = StringView::CompareBytes(sources.Get(left.source_).Path(),
                                                       sources.Get(right.source_).Path());
            if (paths != 0) {
                return paths < 0;
            }

            // 같은 경로의 서로 다른 파일은 더한 순서로 둔다.
            return left.source_.index < right.source_.index;
        }

        // 파일 전체 진단이 먼저다.
        if (left.hasSpan_ != right.hasSpan_) {
            return !left.hasSpan_;
        }

        if (left.span_.begin != right.span_.begin) {
            return left.span_.begin < right.span_.begin;
        }

        if (left.code_ != right.code_) {
            return CodeNumber(left.code_) < CodeNumber(right.code_);
        }

        return left.sequence_ < right.sequence_;
    });
}

Result<void> AppendDiagnostic(StringBuilderBase& out, const SourceSet& sources,
                              const Diagnostic& diagnostic) noexcept {
    const SourceFile& file = sources.Get(diagnostic.Source());
    SourceLocation location;
    if (diagnostic.HasSpan()) {
        location = file.Locate(diagnostic.Span().begin);
    }

    // 첫 줄: "경로:줄:열: error S코드: 메시지"(파일 전체 진단은 "경로: error S코드: 메시지").
    if (auto appended = diagnostic.HasSpan() ? out.TryAppendFormat("{}:{}:{}: error ", file.Path(),
                                                                   location.line, location.column)
                                             : out.TryAppendFormat("{}: error ", file.Path());
        !appended) {
        return appended;
    }

    if (auto appended = AppendDiagnosticCode(out, CodeNumber(diagnostic.Code())); !appended) {
        return appended;
    }

    if (auto appended = out.TryAppendFormat(": {}\n", diagnostic.Message()); !appended) {
        return appended;
    }

    if (diagnostic.HasSpan()) {
        const SourceSpan span = diagnostic.Span();
        // 원문 줄과 밑줄. 밑줄은 범위가 그 줄 안에 있는 code point 수다(최소 1, 줄 끝에서 자름).
        // BOM 안에서 시작하는 범위는 줄 시작(BOM 뒤)부터 센다.
        const uint32 lineBegin = file.LineBegin(location.line);
        const StringView line = file.LineText(location.line);
        const uint32 lineEnd = lineBegin + static_cast<uint32>(line.ByteCount());
        const uint32 underlineBegin = span.begin > lineBegin ? span.begin : lineBegin;
        const uint32 underlineEnd = span.end < lineEnd ? span.end : lineEnd;
        uint32 width = underlineBegin < underlineEnd
                           ? detail::CountColumns(file.Text().Data() + underlineBegin,
                                                  file.Text().Data() + underlineEnd)
                           : 0;
        if (width == 0) {
            width = 1;
        }

        if (auto appended = out.TryAppendRepeated(' ', SourceLineIndent); !appended) {
            return appended;
        }

        if (auto appended = AppendDisplayLine(out, line); !appended) {
            return appended;
        }

        if (auto appended = out.TryAppendAscii('\n'); !appended) {
            return appended;
        }

        if (auto appended = out.TryAppendRepeated(' ', SourceLineIndent + location.column - 1);
            !appended) {
            return appended;
        }

        if (auto appended = out.TryAppendRepeated('^', width); !appended) {
            return appended;
        }

        if (auto appended = out.TryAppendAscii('\n'); !appended) {
            return appended;
        }
    }

    if (!diagnostic.Explanation().IsEmpty()) {
        if (auto appended = AppendNoteLines(out, diagnostic.Explanation()); !appended) {
            return appended;
        }
    }

    if (!diagnostic.Fix().IsEmpty()) {
        if (auto appended = out.TryAppendRepeated(' ', NoteIndent); !appended) {
            return appended;
        }

        if (auto appended = out.TryAppend(FixPrefix); !appended) {
            return appended;
        }

        if (auto appended = out.TryAppend(diagnostic.Fix()); !appended) {
            return appended;
        }

        if (auto appended = out.TryAppendAscii('\n'); !appended) {
            return appended;
        }
    }

    return Result<void>::Success();
}

Result<void> AppendDiagnostics(StringBuilderBase& out, const SourceSet& sources,
                               const DiagnosticList& diagnostics) noexcept {
    for (const Diagnostic& diagnostic : diagnostics.View()) {
        if (auto appended = AppendDiagnostic(out, sources, diagnostic); !appended) {
            return appended;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::sparks
