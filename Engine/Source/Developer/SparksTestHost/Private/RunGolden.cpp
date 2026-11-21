#include <Core/Math/MathUtility.h>
#include <Core/Text/NumberText.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Memory.h>
#include <SparksCompiler/Compilation.h>
#include <SparksCompiler/Symbols.h>
#include <SparksVm/BytecodeFile.h>
#include <SparksVm/Vm.h>

#include "ConformanceInternal.h"

// `.run` 실행 golden이다(Docs/Design/SparksVm.ko.md 7절, LC5a). 줄마다 최상위 함수 호출 하나
// (`<함수>(<인자>, …) -> <결과> (<명령 수> steps)`)이며, 실행기는 `->` 앞만 읽어 `.sparkc` 왕복을
// 거친 모듈로 실행하고 줄 전체를 실제 결과로 다시 써서 파일과 비교한다. 값 표기는 Compilation의
// 시그니처 타입을 따른다.

namespace rocks::sparks::conformance::detail {

namespace {

// 값 표기의 모양이다(Scalar bank 값과 문자열 — LC5a·LC5b1).
enum class RunShape : uint8 {
    Void,
    String, // Buf bank — `"..."`(escape `\"` `\\` `\n` `\t` `\xHH`)
    Bool,
    Signed,
    Unsigned,
    Float32,
    Float64,
    Math, // 성분 double 묶음(vec·quat·Rotator·Color·Transform·mat4)
    Enum,
    Optional,
    Result,
    Unsupported
};

// 정수 타입의 이름과 폭(비트)이다.
struct RunIntegerName {
    StringView name;
    bool isSigned;
    uint32 bits;
};

constexpr RunIntegerName RunIntegers[] = {
    {"int", true, 64},     {"int8", true, 8},     {"int16", true, 16},
    {"int32", true, 32},   {"int64", true, 64},   {"uint8", false, 8},
    {"uint16", false, 16}, {"uint32", false, 32}, {"uint64", false, 64}};

// 수학 타입의 이름과 성분 수다.
struct RunMathName {
    StringView name;
    uint32 slots;
};

constexpr RunMathName RunMaths[] = {{"vec2", 2},    {"vec3", 3},  {"vec4", 4},       {"quat", 4},
                                    {"Rotator", 3}, {"Color", 4}, {"Transform", 10}, {"mat4", 16}};

constexpr uint32 RunBitsPerWord = 64;
constexpr StringView RunArrow = " -> ";

// 타입(옵션 표지를 볼지 정한다)의 모양이다. 정수는 bits·signed, 수학 타입은 slots를 채운다.
struct RunType {
    RunShape shape = RunShape::Unsupported;
    uint32 type = NoType; // Compilation 타입 번호(Void는 NoType)
    uint32 bits = 0;
    uint32 slots = 0; // Math의 성분 수
};

class RunValues final {
public:
    RunValues(const Compilation& compilation, StringBuilderBase& problem) noexcept
        : c_(compilation), problem_(problem) {
    }

    // type의 모양이다. honorOptional이 false면 옵션 표지를 무시한다(Optional의 안쪽).
    RunType TypeOf(uint32 type, bool honorOptional) const noexcept {
        RunType result;
        result.type = type;
        if (type == NoType) {
            result.shape = RunShape::Void;
            return result;
        }

        const TypeEntry& entry = c_.Types()[type];
        if (honorOptional && entry.optional) {
            result.shape = RunShape::Optional;
            return result;
        }

        if (entry.symbol == NoSymbol) {
            return result;
        }

        const Symbol& symbol = c_.Symbols()[entry.symbol];
        if (symbol.kind == SymbolKind::Enum) {
            result.shape = RunShape::Enum;
            return result;
        }

        const StringView name = symbol.name;
        if (name == "bool") {
            result.shape = RunShape::Bool;
        } else if (name == "string") {
            result.shape = RunShape::String;
        } else if (name == "float" || name == "float64") {
            result.shape = RunShape::Float64;
        } else if (name == "float32") {
            result.shape = RunShape::Float32;
        } else if (name == "void") {
            result.shape = RunShape::Void;
        } else if (name == "Result" && entry.argumentCount == 1) {
            result.shape = RunShape::Result;
        }

        for (const RunIntegerName& integer : RunIntegers) {
            if (integer.name == name) {
                result.shape = integer.isSigned ? RunShape::Signed : RunShape::Unsigned;
                result.bits = integer.bits;
            }
        }

        for (const RunMathName& math : RunMaths) {
            if (math.name == name && (symbol.kind == SymbolKind::PreludeType ||
                                      symbol.kind == SymbolKind::BuiltinType)) {
                result.shape = RunShape::Math;
                result.slots = math.slots;
            }
        }

        return result;
    }

    // Result<T>의 T다.
    uint32 ResultValue(uint32 type) const noexcept {
        const TypeEntry& entry = c_.Types()[type];
        return c_.TypeArguments()[entry.firstArgument];
    }

    // Scalar slot 수다(지원하지 않는 모양은 false).
    bool Slots(const RunType& type, uint32& out) const noexcept {
        switch (type.shape) {
        case RunShape::Void:
            out = 0;
            return true;
        case RunShape::String:
            out = 1;
            return true;
        case RunShape::Bool:
        case RunShape::Signed:
        case RunShape::Unsigned:
        case RunShape::Float32:
        case RunShape::Float64:
        case RunShape::Enum:
            out = 1;
            return true;
        case RunShape::Math:
            out = type.slots;
            return true;
        case RunShape::Optional:
        case RunShape::Result: {
            if (IsBuffer(type)) {
                out = 1;
                return true;
            }

            const RunType inner = type.shape == RunShape::Optional
                                      ? TypeOf(type.type, false)
                                      : TypeOf(ResultValue(type.type), true);
            uint32 slots = 0;
            if (!Slots(inner, slots)) {
                return false;
            }

            out = 1 + slots;
            return true;
        }
        case RunShape::Unsupported:
            return false;
        }

        return false;
    }

    // 첫 slot 뒤가 모두 0인지다(없음·오류의 값 자리).
    static bool ZeroTail(Span<const uint64> slots) noexcept {
        for (usize index = 1; index < slots.Size(); ++index) {
            if (slots[index] != 0) {
                return false;
            }
        }

        return true;
    }

    // Buf bank 값(문자열·`string?`)인지다. Buf Result는 LC5b3이다.
    bool IsBuffer(const RunType& type) const noexcept {
        return type.shape == RunShape::String ||
               (type.shape == RunShape::Optional &&
                TypeOf(type.type, false).shape == RunShape::String);
    }

    // Buf 값 하나를 쓴다(문자열은 따옴표와 escape, null은 `none`).
    static void FormatBuffer(StringBuilderBase& out, const VmBuffer* buffer) noexcept {
        if (buffer == nullptr) {
            out.Append("none");
            return;
        }

        if (buffer->kind != VmBufferKind::String) {
            out.Append("?");
            return;
        }

        out.Append("\"");
        const StringView text = VmStringOf(buffer);
        for (usize index = 0; index < text.ByteCount(); ++index) {
            const char c = text.Data()[index];
            if (c == '"' || c == '\\') {
                out.AppendFormat("\\{}", c);
            } else if (c == '\n') {
                out.Append("\\n");
            } else if (c == '\t') {
                out.Append("\\t");
            } else if (static_cast<uint8>(c) < 0x20 || static_cast<uint8>(c) == 0x7F) {
                out.AppendFormat("\\x{:02x}", static_cast<uint32>(static_cast<uint8>(c)));
            } else {
                out.Append(StringView(&c, 1));
            }
        }

        out.Append("\"");
    }

    // text 전체를 Buf 값 하나로 읽어 host 버퍼를 만든다(`none`은 null). 틀리면 problem에 이유를
    // 쓰고 false.
    bool ParseBuffer(StringView text, const RunType& type, VmExecution& execution,
                     Array<VmBuffer*>& buffers, Allocator& allocator) noexcept {
        text = Trim(text);
        auto push = [&](VmBuffer* value) noexcept {
            if (auto pushed = buffers.TryPushBack(value); !pushed) {
                execution.Release(value);
                problem_.Append("out of memory");
                return false;
            }

            return true;
        };
        if (type.shape == RunShape::Optional && text == "none") {
            return push(nullptr);
        }

        Array<char> bytes(allocator);
        bool valid = text.ByteCount() >= 2 && text.StartsWith("\"") && text.EndsWith("\"");
        for (usize index = 1; valid && index + 1 < text.ByteCount(); ++index) {
            char c = text.Data()[index];
            if (c == '"') {
                valid = false;
                break;
            }

            if (c == '\\') {
                if (index + 2 >= text.ByteCount()) {
                    valid = false;
                    break;
                }

                const char escape = text.Data()[++index];
                if (escape == 'n') {
                    c = '\n';
                } else if (escape == 't') {
                    c = '\t';
                } else if (escape == '"' || escape == '\\') {
                    c = escape;
                } else if (escape == 'x' && index + 3 < text.ByteCount()) {
                    auto value = TryParseUInt32(StringView(text.Data() + index + 1, 2), 16);
                    valid = static_cast<bool>(value);
                    c = valid ? static_cast<char>(value.Value()) : '\0';
                    index += 2;
                } else {
                    valid = false;
                    break;
                }
            }

            if (!bytes.TryPushBack(c)) {
                problem_.Append("out of memory");
                return false;
            }
        }

        if (!valid) {
            problem_.AppendFormat("cannot read '{}' as a value of this parameter", text);
            return false;
        }

        auto created = execution.NewString(StringView(bytes.Data(), bytes.Size()));
        if (!created) {
            problem_.Append("out of memory");
            return false;
        }

        return push(created.Value());
    }

    // slots에서 값 하나를 읽어 쓴다(used는 읽은 slot 수).
    void Format(StringBuilderBase& out, const RunType& type,
                Span<const uint64> slots) const noexcept {
        switch (type.shape) {
        case RunShape::Void:
            out.Append("void");
            return;
        case RunShape::String:
            out.Append("?"); // Buf 값은 FormatBuffer
            return;
        case RunShape::Bool:
            out.Append(slots[0] == 0 ? "false" : slots[0] == 1 ? "true" : "bool?");
            return;
        case RunShape::Signed:
            out.AppendFormat("{}", static_cast<int64>(slots[0]));
            return;
        case RunShape::Unsigned:
            out.AppendFormat("{}", slots[0]);
            return;
        case RunShape::Float32: {
            // float 값이 아닌 slot(VM이 반올림을 빠뜨림)은 비트로 드러낸다.
            const double value = platform::BitCast<double>(slots[0]);
            if (platform::BitCast<uint64>(static_cast<double>(static_cast<float>(value))) !=
                slots[0]) {
                out.AppendFormat("float32?{:016x}", slots[0]);
            } else {
                out.AppendFormat("{}", static_cast<float>(value));
            }

            return;
        }
        case RunShape::Float64:
            out.AppendFormat("{}", platform::BitCast<double>(slots[0]));
            return;
        case RunShape::Math:
            out.Append("(");
            for (uint32 index = 0; index < type.slots; ++index) {
                out.AppendFormat("{}{}", index == 0 ? "" : ", ",
                                 platform::BitCast<double>(slots[index]));
            }

            out.Append(")");
            return;
        case RunShape::Enum: {
            const StringView name = CaseName(type.type, slots[0]);
            if (name.IsEmpty()) {
                out.AppendFormat("#{}", slots[0]);
            } else {
                out.Append(name);
            }

            return;
        }
        case RunShape::Optional:
            // 표현 불변식(표지 0·1, 없으면 뒤 slot 0 — SparksBytecode 2절)이 깨지면 드러낸다.
            if (slots[0] > 1) {
                out.AppendFormat("flag?{}", slots[0]);
            } else if (slots[0] == 0) {
                out.Append(ZeroTail(slots) ? "none" : "none?tail");
            } else {
                Format(out, TypeOf(type.type, false),
                       Span<const uint64>(slots.Data() + 1, slots.Size() - 1));
            }

            return;
        case RunShape::Result: {
            const RunType value = TypeOf(ResultValue(type.type), true);
            if (slots[0] != 0) {
                out.AppendFormat("error({}){}", slots[0] - 1, ZeroTail(slots) ? "" : "?tail");
            } else if (value.shape == RunShape::Void) {
                out.Append("ok");
            } else {
                out.Append("ok(");
                Format(out, value, Span<const uint64>(slots.Data() + 1, slots.Size() - 1));
                out.Append(")");
            }

            return;
        }
        case RunShape::Unsupported:
            out.Append("?");
            return;
        }
    }

    // text 전체를 type의 값 하나로 읽어 slots 뒤에 붙인다. 틀리면 problem에 이유를 쓰고 false.
    bool Parse(StringView text, const RunType& type, Array<uint64>& slots) noexcept {
        text = Trim(text);
        auto push = [&](uint64 value) noexcept {
            if (auto pushed = slots.TryPushBack(value); !pushed) {
                problem_.Append("out of memory");
                return false;
            }

            return true;
        };
        switch (type.shape) {
        case RunShape::Bool:
            if (text == "true" || text == "false") {
                return push(text == "true" ? 1 : 0);
            }

            break;
        case RunShape::Signed: {
            auto value = TryParseInt64(text);
            const int64 limit = type.bits == RunBitsPerWord ? NumericLimits<int64>::Max()
                                                            : (int64(1) << (type.bits - 1)) - 1;
            if (value && value.Value() <= limit && value.Value() >= -limit - 1) {
                return push(static_cast<uint64>(value.Value()));
            }

            break;
        }
        case RunShape::Unsigned: {
            auto value = TryParseUInt64(text);
            const uint64 limit = type.bits == RunBitsPerWord ? NumericLimits<uint64>::Max()
                                                             : (uint64(1) << type.bits) - 1;
            if (value && value.Value() <= limit) {
                return push(value.Value());
            }

            break;
        }
        case RunShape::Float32: {
            auto value = TryParseFloat(text);
            if (value && math::IsFinite(value.Value())) {
                return push(platform::BitCast<uint64>(static_cast<double>(value.Value())));
            }

            break;
        }
        case RunShape::Float64: {
            auto value = TryParseDouble(text);
            if (value && math::IsFinite(value.Value())) {
                return push(platform::BitCast<uint64>(value.Value()));
            }

            break;
        }
        case RunShape::Math: {
            if (!text.StartsWith("(") || !text.EndsWith(")")) {
                break;
            }

            const StringView inner(text.Data() + 1, text.ByteCount() - 2);
            uint32 count = 0;
            bool valid = true;
            ForEachItem(inner, [&](StringView item) noexcept {
                auto value = TryParseDouble(Trim(item));
                valid = valid && value && push(platform::BitCast<uint64>(value.Value()));
                ++count;
            });
            if (valid && count == type.slots) {
                return true;
            }

            break;
        }
        case RunShape::Enum: {
            for (uint64 index = 0;; ++index) {
                const StringView name = CaseName(type.type, index);
                if (name.IsEmpty()) {
                    break;
                }

                if (name == text) {
                    return push(index);
                }
            }

            break;
        }
        case RunShape::Optional: {
            const RunType inner = TypeOf(type.type, false);
            uint32 innerSlots = 0;
            if (!Slots(inner, innerSlots)) {
                break;
            }

            if (text == "none") {
                for (uint32 index = 0; index <= innerSlots; ++index) {
                    if (!push(0)) {
                        return false;
                    }
                }

                return true;
            }

            return push(1) && Parse(text, inner, slots);
        }
        case RunShape::Result: {
            const RunType value = TypeOf(ResultValue(type.type), true);
            uint32 valueSlots = 0;
            if (!Slots(value, valueSlots)) {
                break;
            }

            if (text.StartsWith("error(") && text.EndsWith(")")) {
                auto code = TryParseUInt64(StringView(text.Data() + 6, text.ByteCount() - 7));
                if (!code || code.Value() == NumericLimits<uint64>::Max() ||
                    !push(code.Value() + 1)) {
                    break;
                }

                for (uint32 index = 0; index < valueSlots; ++index) {
                    if (!push(0)) {
                        return false;
                    }
                }

                return true;
            }

            if (value.shape == RunShape::Void && text == "ok") {
                return push(0);
            }

            if (text.StartsWith("ok(") && text.EndsWith(")")) {
                return push(0) &&
                       Parse(StringView(text.Data() + 3, text.ByteCount() - 4), value, slots);
            }

            break;
        }
        case RunShape::Void:
        case RunShape::String:
        case RunShape::Unsupported:
            break;
        }

        if (problem_.ByteCount() == 0) {
            problem_.AppendFormat("cannot read '{}' as a value of this parameter", text);
        }

        return false;
    }

    static StringView Trim(StringView text) noexcept {
        usize begin = 0;
        usize end = text.ByteCount();
        while (begin < end && text.Data()[begin] == ' ') {
            ++begin;
        }

        while (end > begin && text.Data()[end - 1] == ' ') {
            --end;
        }

        return StringView(text.Data() + begin, end - begin);
    }

    // 괄호·따옴표 밖의 `,`로 나눈 항목마다 visit을 부른다(빈 text는 항목 없음).
    template <typename Visit>
    static void ForEachItem(StringView text, Visit&& visit) noexcept {
        if (Trim(text).IsEmpty()) {
            return;
        }

        usize depth = 0;
        usize begin = 0;
        bool quoted = false;
        for (usize index = 0; index <= text.ByteCount(); ++index) {
            const char c = index < text.ByteCount() ? text.Data()[index] : ',';
            if (quoted) {
                if (c == '\\') {
                    ++index; // escape 다음 글자는 건너뛴다
                } else if (c == '"') {
                    quoted = false;
                }

                continue;
            }

            if (c == '"') {
                quoted = true;
            } else if (c == '(') {
                ++depth;
            } else if (c == ')' && depth > 0) {
                --depth;
            } else if (c == ',' && depth == 0) {
                visit(StringView(text.Data() + begin, index - begin));
                begin = index + 1;
            }
        }
    }

private:
    // enum 타입의 index번째 case 이름이다(없으면 빈 글자). case는 선언 순 심볼이다.
    StringView CaseName(uint32 type, uint64 index) const noexcept {
        const uint32 owner = c_.Types()[type].symbol;
        uint64 seen = 0;
        for (const Symbol& symbol : c_.Symbols()) {
            if (symbol.kind == SymbolKind::EnumCase && symbol.owner == owner) {
                if (seen == index) {
                    return symbol.name;
                }

                ++seen;
            }
        }

        return StringView();
    }

    const Compilation& c_;
    StringBuilderBase& problem_;
};

} // namespace

Result<void> CheckRunGolden(Allocator& allocator, const CaseInput& input,
                            const Compilation& compilation, uint32 mainModule,
                            Report& report) noexcept {
    StringBuilder<MessageInlineBytes> message(allocator);
    auto fail = [&](StringView reason) noexcept -> Result<void> {
        if (auto formatted = message.TryAppendFormat("{}: {}", input.name, reason); !formatted) {
            return formatted;
        }

        return report.TryAddFailure(message.View());
    };
    const BytecodeModule* bytecode =
        input.folder ? nullptr : compilation.ModuleBytecode(mainModule);
    if (bytecode == nullptr) {
        return fail("has a .run file but the module was not lowered to bytecode (a file case "
                    "without diagnostics is needed)");
    }

    // 실행은 `.sparkc` 왕복을 거친 모듈로 한다(로드 경로와 같게).
    Array<uint8> file(allocator);
    if (auto written = WriteBytecodeFile(*bytecode, file); !written) {
        return written;
    }

    BytecodeModule loaded(allocator);
    BytecodeRejection rejection;
    if (auto read = ReadBytecodeFile(file.View(), loaded, rejection); !read) {
        if (read.Error() != ErrorCode::InvalidArgument) {
            return read;
        }

        return fail("the .sparkc of the module was rejected");
    }

    StringBuilder<MessageInlineBytes> problem(allocator);
    RunValues values(compilation, problem);
    StringBuilder<MessageInlineBytes> actual(allocator);
    Array<uint64> arguments(allocator);
    VmExecution execution(allocator);
    Array<VmBuffer*> buffers(allocator); // host가 만든 문자열 인자(호출 뒤 놓는다)
    // 형식 오류로 일찍 돌아가도 인자 버퍼를 놓는다.
    struct BufferGuard {
        VmExecution& execution;
        Array<VmBuffer*>& buffers;
        ~BufferGuard() noexcept {
            for (VmBuffer* buffer : buffers) {
                execution.Release(buffer);
            }
        }
    } guard{execution, buffers};
    usize offset = 0;
    usize lineNumber = 0;
    while (offset < input.run.ByteCount()) {
        const StringView line = LineAt(input.run, offset);
        ++lineNumber;
        if (line.IsEmpty() || line.StartsWith("#")) {
            actual.Append(line).Append("\n");
            continue;
        }

        // 따옴표 밖의 첫 ` -> ` 앞이 호출이다.
        usize arrow = line.ByteCount();
        bool quoted = false;
        for (usize index = 0; index + RunArrow.ByteCount() <= line.ByteCount(); ++index) {
            const char c = line.Data()[index];
            if (quoted) {
                index += c == '\\' ? 1 : 0;
                quoted = c != '"';
                continue;
            }

            if (c == '"') {
                quoted = true;
            } else if (StringView(line.Data() + index, RunArrow.ByteCount()) == RunArrow) {
                arrow = index;
                break;
            }
        }

        const StringView call = RunValues::Trim(StringView(line.Data(), arrow));
        usize open = 0;
        while (open < call.ByteCount() && call.Data()[open] != '(') {
            ++open;
        }

        problem.Clear();
        if (open == call.ByteCount() || !call.EndsWith(")")) {
            problem.Append("expected '<function>(<arguments>)'");
        }

        const StringView name(call.Data(), open);
        uint32 function = NoBytecodeIndex;
        for (uint32 index = 0; problem.ByteCount() == 0 && index < loaded.functions.Size();
             ++index) {
            if (loaded.String(loaded.functions[index].name) == name) {
                function = index;
            }
        }

        const Symbol* symbol = nullptr;
        for (const Symbol& candidate : compilation.Symbols()) {
            if (candidate.kind == SymbolKind::Function && candidate.module == mainModule &&
                candidate.name == name) {
                symbol = &candidate;
            }
        }

        if (problem.ByteCount() == 0 && (function == NoBytecodeIndex || symbol == nullptr ||
                                         symbol->signature == NoSignature)) {
            problem.AppendFormat("no top-level function '{}'", name);
        }

        arguments.Clear();
        for (VmBuffer* buffer : buffers) {
            execution.Release(buffer);
        }

        buffers.Clear();
        RunType returned;
        if (problem.ByteCount() == 0) {
            const Signature& signature = compilation.Signatures()[symbol->signature];
            returned = values.TypeOf(signature.returnType, true);
            uint32 returnSlots = 0;
            if (!values.Slots(returned, returnSlots) || signature.isAsync) {
                problem.Append("the return type is not supported by .run yet");
            }

            uint32 parameter = 0;
            RunValues::ForEachItem(
                StringView(call.Data() + open + 1, call.ByteCount() - open - 2),
                [&](StringView item) noexcept {
                    if (problem.ByteCount() != 0) {
                        return;
                    }

                    if (parameter >= signature.parameterCount) {
                        problem.Append("too many arguments");
                        return;
                    }

                    const RunType type = values.TypeOf(
                        compilation.SignatureParameters()[signature.firstParameter + parameter],
                        true);
                    ++parameter;
                    uint32 slots = 0;
                    if (!values.Slots(type, slots)) {
                        problem.Append("a parameter type is not supported by .run yet");
                        return;
                    }

                    if (values.IsBuffer(type)) {
                        values.ParseBuffer(item, type, execution, buffers, allocator);
                    } else {
                        values.Parse(item, type, arguments);
                    }
                });
            if (problem.ByteCount() == 0 && parameter != signature.parameterCount) {
                problem.Append("too few arguments");
            }
        }

        if (problem.HasFailed()) {
            return Result<void>::Failure(problem.Status().Error());
        }

        if (problem.ByteCount() != 0) {
            StringBuilder<MessageInlineBytes> reason(allocator);
            reason.AppendFormat(".run line {}: {}", lineNumber, problem.View());
            if (reason.HasFailed()) {
                return Result<void>::Failure(reason.Status().Error());
            }

            return fail(reason.View());
        }

        auto called = execution.Call(loaded, function, arguments.View(), Span<const uint64>(),
                                     Span<VmBuffer* const>(buffers.Data(), buffers.Size()));
        if (!called) {
            if (called.Error() != ErrorCode::InvalidArgument) {
                return Result<void>::Failure(called.Error());
            }

            return fail(".run: the VM rejected the call (parameter slots do not match)");
        }

        const VmCallResult& result = called.Value();
        actual.Append(call).Append(RunArrow);
        switch (result.outcome) {
        case VmOutcome::Returned:
            if (values.IsBuffer(returned)) {
                RunValues::FormatBuffer(actual, execution.ReturnedBuffers()[0]);
            } else {
                values.Format(actual, returned, execution.ReturnedSlots());
            }

            break;
        case VmOutcome::Faulted:
            actual.AppendFormat("fault {} at {}:{}", VmFaultKindName(result.fault.kind),
                                result.fault.location.line, result.fault.location.column);
            break;
        case VmOutcome::Ended:
            actual.AppendFormat("ended error({})", result.errorCode - 1);
            break;
        }

        actual.AppendFormat(" ({} steps)\n", result.steps);
        // 반환 값과 인자 버퍼를 놓으면 ledger는 0이다(SparksVm 2.2절).
        execution.ReleaseReturned();
        for (VmBuffer* buffer : buffers) {
            execution.Release(buffer);
        }

        buffers.Clear();
        if (execution.LiveBuffers() != 0 || execution.LiveBytes() != 0) {
            StringBuilder<MessageInlineBytes> reason(allocator);
            reason.AppendFormat(
                ".run line {}: {} buffers ({} bytes) are still alive after the call", lineNumber,
                execution.LiveBuffers(), execution.LiveBytes());
            if (reason.HasFailed()) {
                return Result<void>::Failure(reason.Status().Error());
            }

            return fail(reason.View());
        }
    }

    if (actual.HasFailed()) {
        return Result<void>::Failure(actual.Status().Error());
    }

    if (actual.View() == input.run) {
        return Result<void>::Success();
    }

    const LineDifference difference = FindLineDifference(input.run, actual.View());
    if (auto formatted = message.TryAppendFormat("{}: run differ from .run at line {}\n"
                                                 "  expected: {}\n  actual:   {}\n  actual run:\n",
                                                 input.name, difference.line, difference.expected,
                                                 difference.actual);
        !formatted) {
        return formatted;
    }

    if (auto appended = message.TryAppend(actual.View()); !appended) {
        return appended;
    }

    return report.TryAddFailure(message.View());
}

} // namespace rocks::sparks::conformance::detail
