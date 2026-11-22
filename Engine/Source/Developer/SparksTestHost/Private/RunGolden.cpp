#include <Core/Math/MathUtility.h>
#include <Core/Text/NumberText.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Memory.h>
#include <SparksCompiler/Compilation.h>
#include <SparksCompiler/Symbols.h>
#include <SparksVm/BytecodeFile.h>
#include <SparksVm/Vm.h>

#include "ConformanceInternal.h"
#include "FakeHost.h"

#include <SparksVm/VmSession.h>

// `.run` 실행 golden이다(Docs/Design/SparksVm.ko.md 7절, LC5a). 줄마다 최상위 함수 호출 하나
// (`<함수>(<인자>, …) -> <결과> (<명령 수> steps)`)이며, 실행기는 `->` 앞만 읽어 `.sparkc` 왕복을
// 거친 모듈로 실행하고 줄 전체를 실제 결과로 다시 써서 파일과 비교한다. 값 표기는 Compilation의
// 시그니처 타입을 따른다.

namespace rocks::sparks::conformance::detail {

namespace {

// 값 표기의 모양이다(Scalar bank 값과 문자열 — LC5a·LC5b1).
enum class RunShape : uint8 {
    Void,
    String,    // Buf bank — `"..."`(escape `\"` `\\` `\n` `\t` `\xHH`)
    Container, // Buf bank — 반환만: Array `[a, b]`, Set `{a, b}`, Map `{k: v}`(LC5b2)
    Struct,    // Buf bank — 반환만: `Pair(A: 1, B: "x")`(LC5b3)
    Object, // Ref bank — 가짜 host 객체 `#n`(죽은 handle은 `dead#n`), `T?`의 없음은 `none`(LC5d1)
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
// 전달 기록 줄의 머리다(`  deliver #구독자 <함수> -> <결과> (<명령 수> steps)` — SparksRuntime
// 2.3절).
constexpr StringView RunDeliverPrefix = "  deliver ";
constexpr StringView RunResumePrefix = "  resume ";

// 호출 뒤 `Log.*` 기록 줄의 머리다(`  log <수준> "<메시지>"`).
constexpr StringView RunLogPrefix = "  log ";
// 호출 뒤 가짜 host의 native 호출 기록 줄의 머리다(`  host <이름>(<인자>) -> <결과>`).
constexpr StringView RunHostPrefix = "  host ";

// 타입(옵션 표지를 볼지 정한다)의 모양이다. 정수는 bits·signed, 수학 타입은 slots를 채운다.
struct RunType {
    RunShape shape = RunShape::Unsupported;
    uint32 type = NoType; // Compilation 타입 번호(Void는 NoType)
    uint32 bits = 0;
    uint32 slots = 0; // Math의 성분 수
};

class RunValues final {
public:
    RunValues(const Compilation& compilation, StringBuilderBase& problem,
              FakeHost* host = nullptr) noexcept
        : c_(compilation), problem_(problem), host_(host) {
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
            // 객체의 `T?`는 같은 Ref slot이다(0이 없음).
            result.shape = TypeOf(type, false).shape == RunShape::Object ? RunShape::Object
                                                                         : RunShape::Optional;
            return result;
        }

        if (entry.symbol == NoSymbol) {
            return result;
        }

        const Symbol& symbol = c_.Symbols()[entry.symbol];
        // Sparks enum, 언어 enum `Clock`, 엔진 enum(case는 EnumCase 심볼 — 선언 순).
        if (symbol.kind == SymbolKind::Enum ||
            (symbol.kind == SymbolKind::PreludeType && symbol.name == "Clock") ||
            (symbol.kind == SymbolKind::EngineType && HasCases(entry.symbol))) {
            result.shape = RunShape::Enum;
            return result;
        }

        if (symbol.kind == SymbolKind::Struct) {
            result.shape = RunShape::Struct;
            return result;
        }

        // 객체: Sparks class·interface, 엔진 object·asset·interface(Ref bank).
        if (symbol.kind == SymbolKind::Class || symbol.kind == SymbolKind::Interface ||
            (symbol.kind == SymbolKind::EngineType && c_.SchemaIsValid(symbol.index) &&
             c_.Schema(symbol.index).Types()[symbol.item].kind != SchemaTypeKind::Enum &&
             c_.Schema(symbol.index).Types()[symbol.item].kind != SchemaTypeKind::Struct)) {
            result.shape = RunShape::Object;
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
        } else if ((name == "Array" || name == "Set") && entry.argumentCount == 1) {
            result.shape = RunShape::Container;
        } else if (name == "Map" && entry.argumentCount == 2) {
            result.shape = RunShape::Container;
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

    // symbol을 주인으로 하는 enum case가 있는지다(엔진 enum 판정).
    bool HasCases(uint32 symbol) const noexcept {
        for (const Symbol& item : c_.Symbols()) {
            if (item.kind == SymbolKind::EnumCase && item.owner == symbol) {
                return true;
            }
        }

        return false;
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
        case RunShape::Container:
        case RunShape::Struct:
        case RunShape::Object:
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

    // Buf bank 값(문자열·컨테이너와 그 `T?`)인지다. Buf Result는 LC5b3이다.
    bool IsBuffer(const RunType& type) const noexcept {
        const RunType inner = type.shape == RunShape::Optional ? TypeOf(type.type, false) : type;
        if (inner.shape == RunShape::Result) {
            // Buf Result: 값 타입이 Scalar 밖(Buf)이다(SparksBytecode 2절).
            const RunType value = TypeOf(ResultValue(inner.type), true);
            return value.shape != RunShape::Void && IsBuffer(value);
        }

        return inner.shape == RunShape::String || inner.shape == RunShape::Container ||
               inner.shape == RunShape::Struct;
    }

    // struct 필드의 타입이다(표기가 없으면 초기값의 타입).
    uint32 FieldTypeOf(const Symbol& field) const noexcept {
        if (field.type != NoType) {
            return field.type;
        }

        const SyntaxTree* tree = c_.ModuleTree(field.module);
        return tree == nullptr ? NoType
                               : c_.ExpressionType(field.module, tree->Child(field.node, 4));
    }

    // struct 값을 `Name(필드: 값, …)`로 쓴다. 필드 slot 위치는 TYPE 구역 규칙(bank 묶음 Scalar·
    // Ref·Buf, 선언 순)으로 다시 계산한다 — VM 배치가 어긋나면 golden이 드러낸다.
    void FormatStruct(StringBuilderBase& out, const RunType& type,
                      const VmBuffer* value) const noexcept {
        const uint32 symbol = c_.Types()[type.type].symbol;
        const Symbol& owner = c_.Symbols()[symbol];
        const SyntaxTree* tree = c_.ModuleTree(owner.module);
        const uint32 members = tree == nullptr ? NoNode : tree->Child(owner.node, 3);
        const uint64* payload = reinterpret_cast<const uint64*>(value->Bytes());
        uint32 groups[3] = {};
        // 첫 바퀴: bank별 slot 수, 둘째 바퀴: 쓰기.
        for (uint32 pass = 0; pass < 2; ++pass) {
            uint32 cursor[3] = {0, groups[0], groups[0] + groups[1]};
            bool first = true;
            if (pass == 1) {
                out.Append(owner.name).Append("(");
            }

            for (const uint32 member :
                 members == NoNode ? Span<const uint32>() : tree->Children(members)) {
                if (tree->Node(member).kind != SyntaxKind::Field) {
                    continue;
                }

                const uint32 field = c_.NodeSymbol(owner.module, tree->Child(member, 2));
                if (field == NoSymbol) {
                    continue;
                }

                const RunType fieldType = TypeOf(FieldTypeOf(c_.Symbols()[field]), true);
                uint32 slots = 0;
                const bool bufferField = IsBuffer(fieldType);
                const bool known = Slots(fieldType, slots);
                const uint32 bank = bufferField ? 2 : known ? 0 : 1; // 모르는 타입은 Ref로 본다
                slots = known ? slots : 1;
                if (pass == 0) {
                    groups[bank] += slots;
                    continue;
                }

                out.Append(first ? "" : ", ").Append(c_.Symbols()[field].name).Append(": ");
                first = false;
                if (bufferField) {
                    FormatValue(out, fieldType,
                                reinterpret_cast<const VmBuffer*>(
                                    static_cast<usize>(payload[cursor[bank]])));
                } else if (known) {
                    Format(out, fieldType, Span<const uint64>(payload + cursor[bank], slots));
                } else {
                    out.Append("?");
                }

                cursor[bank] += slots;
            }
        }

        out.Append(")");
    }

    // Buf 값 하나를 타입대로 쓴다(문자열, 컨테이너는 원소마다 — 원소 버퍼는 다시 이 함수로).
    void FormatValue(StringBuilderBase& out, const RunType& type,
                     const VmBuffer* buffer) const noexcept {
        if (type.shape == RunShape::Optional) {
            FormatValue(out, TypeOf(type.type, false), buffer);
            return;
        }

        if (buffer != nullptr && type.shape == RunShape::Struct &&
            buffer->kind == VmBufferKind::Struct) {
            FormatStruct(out, type, buffer);
            return;
        }

        if (buffer != nullptr && type.shape == RunShape::Result &&
            buffer->kind == VmBufferKind::Result) {
            const uint64* payload = reinterpret_cast<const uint64*>(buffer->Bytes());
            if (payload[0] != 0) {
                out.AppendFormat("error({})", payload[0] - 1);
                return;
            }

            out.Append("ok(");
            FormatValue(out, TypeOf(ResultValue(type.type), true),
                        buffer->count > 1
                            ? reinterpret_cast<const VmBuffer*>(static_cast<usize>(payload[1]))
                            : nullptr);
            out.Append(")");
            return;
        }

        if (type.shape != RunShape::Container || buffer == nullptr) {
            FormatBuffer(out, buffer);
            return;
        }

        const TypeEntry& entry = c_.Types()[type.type];
        const bool map = buffer->kind == VmBufferKind::Map;
        const RunType key = TypeOf(c_.TypeArguments()[entry.firstArgument], true);
        const RunType value = map ? TypeOf(c_.TypeArguments()[entry.firstArgument + 1], true) : key;
        out.Append(buffer->kind == VmBufferKind::Array ? "[" : "{");
        const uint32 stride = uint32(buffer->keySlots) + buffer->valueSlots;
        const uint64* slots = reinterpret_cast<const uint64*>(buffer->Bytes());
        auto part = [&](const RunType& part, const uint64* at, uint32 count) noexcept {
            if (IsBuffer(part)) {
                FormatValue(out, part,
                            reinterpret_cast<const VmBuffer*>(static_cast<usize>(at[0])));
            } else {
                Format(out, part, Span<const uint64>(at, count));
            }
        };
        for (uint32 element = 0; element < buffer->count; ++element) {
            const uint64* at = slots + uint64(element) * stride;
            out.Append(element == 0 ? "" : ", ");
            if (buffer->kind == VmBufferKind::Array) {
                part(value, at, buffer->valueSlots);
            } else if (buffer->kind == VmBufferKind::Set) {
                part(key, at, buffer->keySlots);
            } else {
                part(key, at, buffer->keySlots);
                out.Append(": ");
                part(value, at + buffer->keySlots, buffer->valueSlots);
            }
        }

        out.Append(buffer->kind == VmBufferKind::Array ? "]" : "}");
    }

    // text를 따옴표와 escape로 쓴다(문자열 값·로그 기록).
    static void AppendQuoted(StringBuilderBase& out, StringView text) noexcept {
        // escape 없는 구간은 통째로 붙인다(StringBuilder는 UTF-8을 검사하므로 다중 byte 문자를
        // byte 하나씩 붙일 수 없다).
        out.Append("\"");
        usize plain = 0;
        for (usize index = 0; index < text.ByteCount(); ++index) {
            const char c = text.Data()[index];
            const bool control = static_cast<uint8>(c) < 0x20 || static_cast<uint8>(c) == 0x7F;
            if (c != '"' && c != '\\' && !control) {
                continue;
            }

            out.Append(StringView(text.Data() + plain, index - plain));
            plain = index + 1;
            if (c == '"' || c == '\\') {
                out.AppendFormat("\\{}", c);
            } else if (c == '\n') {
                out.Append("\\n");
            } else if (c == '\t') {
                out.Append("\\t");
            } else {
                out.AppendFormat("\\x{:02x}", static_cast<uint32>(static_cast<uint8>(c)));
            }
        }

        out.Append(StringView(text.Data() + plain, text.ByteCount() - plain));
        out.Append("\"");
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

        AppendQuoted(out, VmStringOf(buffer));
    }

    // text 전체를 Buf 값 하나로 읽어 host 버퍼를 만든다(`none`은 null). 틀리면 problem에 이유를
    // 쓰고 false.
    bool ParseBuffer(StringView text, const RunType& type, VmExecution& execution,
                     Array<VmBuffer*>& buffers, Allocator& allocator) noexcept {
        text = Trim(text);
        auto push = [&](VmBuffer* value) noexcept {
            if (auto pushed = buffers.TryPushBack(value); !pushed) {
                execution.Release(value);
                OutOfMemory();
                return false;
            }

            return true;
        };
        if (type.shape == RunShape::Optional && text == "none") {
            return push(nullptr);
        }

        const RunType inner = type.shape == RunShape::Optional ? TypeOf(type.type, false) : type;
        if (inner.shape == RunShape::Container) {
            VmBuffer* created = nullptr;
            return ParseContainer(text, inner, execution, allocator, created) && push(created);
        }

        if (inner.shape != RunShape::String) {
            problem_.Append("a parameter type is not supported by .run yet");
            return false;
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
                OutOfMemory();
                return false;
            }
        }

        if (!valid) {
            problem_.AppendFormat("cannot read '{}' as a value of this parameter", text);
            return false;
        }

        auto created = execution.NewString(StringView(bytes.Data(), bytes.Size()));
        if (!created) {
            OutOfMemory();
            return false;
        }

        return push(created.Value());
    }

    // 컨테이너 표기(Array `[a, b]`, Set `{a, b}`, Map `{k: v}` — 원소는 원소 타입대로, 중첩 가능)를
    // 읽어 host 컨테이너를 만든다(VmExecution::NewContainer). 틀리면 problem에 이유를 쓰고 false.
    bool ParseContainer(StringView text, const RunType& type, VmExecution& execution,
                        Allocator& allocator, VmBuffer*& created) noexcept {
        const TypeEntry& entry = c_.Types()[type.type];
        const StringView name = c_.Symbols()[entry.symbol].name;
        const bool map = name == "Map";
        const VmBufferKind kind = map             ? VmBufferKind::Map
                                  : name == "Set" ? VmBufferKind::Set
                                                  : VmBufferKind::Array;
        const RunType first = TypeOf(c_.TypeArguments()[entry.firstArgument], true);
        const RunType second =
            map ? TypeOf(c_.TypeArguments()[entry.firstArgument + 1], true) : RunType();
        uint32 firstSlots = 0;
        uint32 secondSlots = 0;
        if (!Slots(first, firstSlots) || (map && !Slots(second, secondSlots))) {
            problem_.Append("a parameter type is not supported by .run yet");
            return false;
        }

        const StringView brackets = kind == VmBufferKind::Array ? "[]" : "{}";
        if (text.ByteCount() < 2 || text.Data()[0] != brackets.Data()[0] ||
            text.Data()[text.ByteCount() - 1] != brackets.Data()[1]) {
            problem_.AppendFormat("cannot read '{}' as a value of this parameter", text);
            return false;
        }

        Array<uint64> slots(allocator);
        Array<VmBuffer*> children(allocator); // 원소 버퍼(컨테이너가 참조를 더한 뒤 놓는다)
        auto part = [&](StringView item, const RunType& partType) noexcept {
            if (!IsBuffer(partType)) {
                return Parse(item, partType, slots);
            }

            const usize before = children.Size();
            if (!ParseBuffer(item, partType, execution, children, allocator)) {
                return false;
            }

            if (!slots.TryPushBack(
                    static_cast<uint64>(reinterpret_cast<usize>(children[before])))) {
                OutOfMemory();
                return false;
            }

            return true;
        };
        bool valid = true;
        ForEachItem(
            StringView(text.Data() + 1, text.ByteCount() - 2), [&](StringView item) noexcept {
                if (!valid) {
                    return;
                }

                if (!map) {
                    valid = part(item, first);
                    return;
                }

                const usize colon = FindTopLevel(item, ':');
                if (colon == item.ByteCount()) {
                    problem_.AppendFormat("cannot read '{}' as a map entry", Trim(item));
                    valid = false;
                    return;
                }

                valid =
                    part(StringView(item.Data(), colon), first) &&
                    part(StringView(item.Data() + colon + 1, item.ByteCount() - colon - 1), second);
            });
        const uint32 firstBank =
            static_cast<uint32>(IsBuffer(first) ? BytecodeBank::Buf : BytecodeBank::Scalar);
        const uint32 secondBank =
            static_cast<uint32>(IsBuffer(second) ? BytecodeBank::Buf : BytecodeBank::Scalar);
        const uint32 marker =
            map ? EncodeContainerMarker(secondBank, secondSlots, firstBank, firstSlots)
                : EncodeContainerMarker(firstBank, firstSlots, 0, 0);
        if (valid) {
            auto made = execution.NewContainer(kind, marker, slots.View());
            if (made) {
                created = made.Value();
            } else {
                if (made.Error() == ErrorCode::InvalidArgument) {
                    problem_.AppendFormat("cannot make '{}' (repeated key?)", text);
                } else if (made.Error() == ErrorCode::CapacityExceeded) {
                    problem_.AppendFormat("cannot make '{}' (too large)", text); // 리뷰 B3
                } else {
                    OutOfMemory();
                }

                valid = false;
            }
        }

        for (VmBuffer* child : children) {
            execution.Release(child);
        }

        return valid;
    }

    // 괄호·따옴표 밖의 첫 separator 위치다(없으면 text 길이).
    static usize FindTopLevel(StringView text, char separator) noexcept {
        usize found = text.ByteCount();
        ScanTopLevel(text, [&](usize index, char c) noexcept {
            if (c == separator && found == text.ByteCount()) {
                found = index;
            }
        });
        return found;
    }

    // slots에서 값 하나를 읽어 쓴다(used는 읽은 slot 수).
    void Format(StringBuilderBase& out, const RunType& type,
                Span<const uint64> slots) const noexcept {
        switch (type.shape) {
        case RunShape::Void:
            out.Append("void");
            return;
        case RunShape::String:
        case RunShape::Container:
        case RunShape::Struct:
            out.Append("?"); // Buf 값은 FormatValue
            return;
        case RunShape::Object:
            if (host_ != nullptr) {
                host_->FormatHandle(out, slots[0]);
            } else {
                out.Append(slots[0] == 0 ? "none" : "#?");
            }

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
                OutOfMemory();
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
        case RunShape::Object: {
            if (text == "none") {
                return push(0);
            }

            auto number = text.StartsWith("#")
                              ? TryParseUInt32(StringView(text.Data() + 1, text.ByteCount() - 1))
                              : Result<uint32>::Failure(ErrorCode::InvalidArgument);
            if (!number || number.Value() == 0 || host_ == nullptr) {
                break;
            }

            const uint32 symbol = c_.Types()[type.type].symbol;
            const uint64 handle = host_->ObjectArgument(number.Value(), c_.Symbols()[symbol].name);
            if (handle == 0) {
                OutOfMemory();
                return false;
            }

            return push(handle);
        }
        case RunShape::Void:
        case RunShape::String:
        case RunShape::Container:
        case RunShape::Struct:
        case RunShape::Unsupported:
            break;
        }

        if (problem_.ByteCount() == 0) {
            problem_.AppendFormat("cannot read '{}' as a value of this parameter", text);
        }

        return false;
    }

    // 표기를 읽다가 할당에 실패했는지다(보고 실패가 아니라 실행기의 OutOfMemory다).
    bool RanOutOfMemory() const noexcept {
        return outOfMemory_;
    }

    void OutOfMemory() noexcept {
        outOfMemory_ = true;
        problem_.Append("out of memory");
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

        usize begin = 0;
        ScanTopLevel(text, [&](usize index, char c) noexcept {
            if (c == ',') {
                visit(StringView(text.Data() + begin, index - begin));
                begin = index + 1;
            }
        });
        visit(StringView(text.Data() + begin, text.ByteCount() - begin));
    }

    // 괄호(`()` `[]` `{}`)·따옴표 밖의 글자마다 visit(위치, 글자)을 부른다.
    template <typename Visit>
    static void ScanTopLevel(StringView text, Visit&& visit) noexcept {
        usize depth = 0;
        bool quoted = false;
        for (usize index = 0; index < text.ByteCount(); ++index) {
            const char c = text.Data()[index];
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
            } else if (c == '(' || c == '[' || c == '{') {
                ++depth;
            } else if ((c == ')' || c == ']' || c == '}') && depth > 0) {
                --depth;
            } else if (depth == 0) {
                visit(index, c);
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
    FakeHost* host_ = nullptr;
    bool outOfMemory_ = false;
};

} // namespace

// session의 마지막 진입이 실행한 handler를 `  deliver` 줄로 쓴다(SparksRuntime 2.3절).
static void AppendDeliveries(StringBuilderBase& actual, const VmSession& session,
                             const FakeHost& host) noexcept {
    for (const VmSession::Delivery& delivery : session.Deliveries()) {
        actual.Append(delivery.resumed ? RunResumePrefix : RunDeliverPrefix);
        host.FormatHandle(actual, delivery.subscriber);
        const BytecodeModule& module = delivery.module->Module();
        actual.Append(" ").Append(module.String(module.functions[delivery.function].name));
        actual.Append(" -> ");
        switch (delivery.result.outcome) {
        case VmOutcome::Returned:
            actual.Append("ok");
            break;
        case VmOutcome::Faulted:
            actual.AppendFormat("fault {} at {}:{}", VmFaultKindName(delivery.result.fault.kind),
                                delivery.result.fault.location.line,
                                delivery.result.fault.location.column);
            break;
        case VmOutcome::Ended:
            actual.AppendFormat("ended error({})", delivery.result.errorCode - 1);
            break;
        case VmOutcome::Suspended:
            actual.Append("suspended");
            break;
        }

        actual.AppendFormat(" ({} steps)\n", delivery.result.steps);
    }
}

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

    // 가짜 host로 링크한다(SparksVm 6.1절 — 못 묶으면 사례 실패).
    FakeHost host(allocator, compilation);
    if (host.RanOutOfMemory()) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    VmLinkedModule linked(allocator);
    {
        StringBuilder<MessageInlineBytes> reason(allocator);
        if (auto link = LinkModule(loaded, &host, linked, reason); !link) {
            if (link.Error() != ErrorCode::InvalidArgument) {
                return link;
            }

            StringBuilder<MessageInlineBytes> message(allocator);
            message.AppendFormat(".run: the module was not linked: {}", reason.View());
            if (message.HasFailed()) {
                return Result<void>::Failure(message.Status().Error());
            }

            return fail(message.View());
        }
    }

    host.SetModule(&linked);
    Array<uint64> refArguments(allocator);
    StringBuilder<MessageInlineBytes> problem(allocator);
    RunValues values(compilation, problem, &host);
    StringBuilder<MessageInlineBytes> actual(allocator);
    Array<uint64> arguments(allocator);
    // 실행 단위는 session 하나다(SparksRuntime 2.2절 — 메서드 호출의 수신자는 session instance).
    VmSession session(allocator, &host);
    VmExecution& execution = session.Execution();
    if (auto added = session.AddModule(linked); !added) {
        return added;
    }

    // host가 필드에 보관한 버퍼는 어느 출구에서도 놓는다(LC5d1 리뷰 B5 — execution이 host보다 먼저
    // 선언돼 이 guard가 사라질 때 아직 살아 있다).
    struct HostGuard {
        FakeHost& host;
        VmExecution& execution;
        ~HostGuard() noexcept {
            host.ReleaseAll(execution);
        }
    } hostGuard{host, execution};
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

        // 로그 줄은 앞 호출의 기록이다 — 실행기가 다시 쓴다.
        if (line.StartsWith(RunLogPrefix) || line.StartsWith(RunHostPrefix) ||
            line.StartsWith(RunDeliverPrefix) || line.StartsWith(RunResumePrefix)) {
            continue;
        }

        // instance 지시(SparksRuntime 4절 — LC6a): `new #n Class`, `hook #n Kind[(인자)]`,
        // `fields #n`, `detach #n`. 결과는 `-> ok (N steps)`·fault·필드 표기와 log·host 줄이다.
        if (line.StartsWith("new ") || line.StartsWith("hook ") || line.StartsWith("fields ") ||
            line.StartsWith("detach ") || line.StartsWith("emit ") || line == "phase" ||
            line.StartsWith("phase ") || line.StartsWith("advance ") || line == "fixed" ||
            line.StartsWith("fixed ") || line == "stats" || line.StartsWith("stats ")) {
            usize arrowAt = line.ByteCount();
            for (usize index = 0; index + RunArrow.ByteCount() <= line.ByteCount(); ++index) {
                if (StringView(line.Data() + index, RunArrow.ByteCount()) == RunArrow) {
                    arrowAt = index;
                    break;
                }
            }

            const StringView directive = RunValues::Trim(StringView(line.Data(), arrowAt));
            // 단어 나누기: 지시, 대상 `#n`, 나머지.
            usize first = 0;
            while (first < directive.ByteCount() && directive.Data()[first] != ' ') {
                ++first;
            }

            usize second = first + 1;
            while (second < directive.ByteCount() && directive.Data()[second] != ' ') {
                ++second;
            }

            const StringView verb(directive.Data(), first);
            const StringView target =
                first < directive.ByteCount()
                    ? StringView(directive.Data() + first + 1,
                                 (second < directive.ByteCount() ? second : directive.ByteCount()) -
                                     first - 1)
                    : StringView();
            const StringView rest =
                second < directive.ByteCount()
                    ? RunValues::Trim(StringView(directive.Data() + second + 1,
                                                 directive.ByteCount() - second - 1))
                    : StringView();
            auto number =
                target.StartsWith("#")
                    ? TryParseUInt32(StringView(target.Data() + 1, target.ByteCount() - 1))
                    : Result<uint32>::Failure(ErrorCode::InvalidArgument);
            auto bad = [&](StringView why) noexcept -> Result<void> {
                StringBuilder<MessageInlineBytes> reason(allocator);
                reason.AppendFormat(".run line {}: {}", lineNumber, why);
                if (reason.HasFailed()) {
                    return Result<void>::Failure(reason.Status().Error());
                }

                return fail(reason.View());
            };
            host.ClearTrace();
            VmCallResult result;
            bool ran = false;
            bool delivered = false;
            if (verb == "phase") {
                // 새 스크립트 단계(SparksRuntime 2.3절 — 이월 항목 전달).
                actual.Append(directive).Append(RunArrow);
                if (auto opened = session.OpenPhase(); !opened) {
                    return Result<void>::Failure(opened.Error());
                }

                actual.AppendFormat("ok ({} queued)", session.QueuedCount());
                delivered = true;
            } else if (verb == "stats") {
                // session의 async 상태(SparksRuntime 2.4절): 대기 표 항목·정지 실행·skip 수.
                actual.Append(directive).Append(RunArrow);
                actual.AppendFormat("waits {}, suspended {}, skipped {}", session.WaitCount(),
                                    session.SuspendedCount(), session.SkippedWhileBusy());
            } else if (verb == "advance" || verb == "fixed") {
                // 시계 진행(SparksRuntime 2.4절 — LC6c): `advance <Game 초> [<UI 초>]`는 시계를
                // 진행하고(UI를 빼면 Game과 같다) 가변 재개 단계를, `fixed`는 고정 재개 단계를
                // 돈다. 재개된 실행은 `  resume` 줄이다.
                actual.Append(directive).Append(RunArrow);
                Result<void> stepped = Result<void>::Success();
                if (verb == "advance") {
                    auto seconds = TryParseDouble(target);
                    auto ui = TryParseDouble(rest.IsEmpty() ? target : rest);
                    if (!seconds || !ui) {
                        return bad("'advance' needs seconds");
                    }

                    stepped = session.Advance(seconds.Value(), ui.Value());
                    if (!stepped && stepped.Error() == ErrorCode::InvalidArgument) {
                        actual.Append("invalid");
                        stepped = Result<void>::Success();
                        delivered = true;
                        actual.Append("\n");
                        continue;
                    }
                } else {
                    stepped = session.FixedStep();
                }

                if (!stepped) {
                    return Result<void>::Failure(stepped.Error());
                }

                actual.AppendFormat("ok ({} suspended)", session.SuspendedCount());
                delivered = true;
            } else if (!number || number.Value() == 0) {
                return bad("expected an object '#<number>' after the directive");
            } else {
                actual.Append(directive).Append(RunArrow);
                if (verb == "new") {
                    uint32 classIndex = NoBytecodeIndex;
                    for (uint32 index = 0; index < loaded.classes.Size(); ++index) {
                        classIndex =
                            loaded.String(loaded.classes[index].name) == rest ? index : classIndex;
                    }

                    if (classIndex == NoBytecodeIndex) {
                        return bad("'new' needs a class of the module");
                    }

                    const uint64 handle = host.ObjectArgument(number.Value(), rest);
                    if (handle == 0) {
                        return Result<void>::Failure(ErrorCode::OutOfMemory);
                    }

                    auto created = session.CreateInstance(handle, linked, classIndex, result);
                    if (!created) {
                        if (created.Error() != ErrorCode::InvalidArgument) {
                            return Result<void>::Failure(created.Error());
                        }

                        return bad("the object already has an instance or is not alive");
                    }

                    ran = true;
                } else {
                    const uint64 handle = host.ObjectArgument(number.Value(), StringView());
                    const uint32 instance = handle == 0 ? NoBytecodeIndex : session.Find(handle);
                    if (instance == NoBytecodeIndex && verb != "emit") {
                        return bad("the object has no instance (use 'new' first)");
                    }

                    if (verb == "detach") {
                        if (auto detached = session.Detach(instance); !detached) {
                            return Result<void>::Failure(detached.Error());
                        }

                        actual.Append("ok");
                    } else if (verb == "fields") {
                        // 기반 사슬(기반 먼저)의 필드: `Class(Field: 값, …)`.
                        const VmLinkedModule* chainModules[64] = {};
                        uint32 chainClasses[64] = {};
                        uint32 depth = 0;
                        uint32 current = session.ClassOf(instance);
                        while (current != NoBytecodeIndex && depth < 64) {
                            chainModules[depth] = &linked;
                            chainClasses[depth++] = current;
                            current = loaded.classes[current].baseClass;
                        }

                        const VmSession::FieldView storage = session.Fields(instance);
                        actual.Append(loaded.String(loaded.classes[session.ClassOf(instance)].name))
                            .Append("(");
                        bool firstField = true;
                        for (uint32 at = depth; at > 0; --at) {
                            const BytecodeClass& entry = loaded.classes[chainClasses[at - 1]];
                            uint32 start[BytecodeBankCount] = {};
                            static_cast<void>(session.RegionStart(*chainModules[at - 1],
                                                                  chainClasses[at - 1], start));
                            const StringView className = loaded.String(entry.name);
                            for (uint32 field = entry.firstField;
                                 field < entry.firstField + entry.fieldCount; ++field) {
                                const BytecodeTypeField& slot = loaded.classFields[field];
                                const StringView fieldName = loaded.String(slot.name);
                                const Symbol* symbol = nullptr;
                                for (const Symbol& candidate : compilation.Symbols()) {
                                    if (candidate.kind == SymbolKind::Field &&
                                        candidate.module == mainModule &&
                                        candidate.name == fieldName &&
                                        candidate.owner != NoSymbol &&
                                        compilation.Symbols()[candidate.owner].name == className) {
                                        symbol = &candidate;
                                    }
                                }

                                actual.Append(firstField ? "" : ", ")
                                    .Append(fieldName)
                                    .Append(": ");
                                firstField = false;
                                const usize where = usize(start[slot.bank]) + slot.offset;
                                const RunType type =
                                    symbol == nullptr
                                        ? RunType()
                                        : values.TypeOf(values.FieldTypeOf(*symbol), true);
                                if (values.IsBuffer(type)) {
                                    values.FormatValue(actual, type, storage.buffers[where]);
                                } else if (type.shape == RunShape::Unsupported) {
                                    actual.Append("?");
                                } else {
                                    values.Format(
                                        actual, type,
                                        Span<const uint64>(
                                            (slot.bank == 0 ? storage.scalars : storage.refs)
                                                    .Data() +
                                                where,
                                            slot.slots));
                                }
                            }
                        }

                        actual.Append(")");
                    } else if (verb == "hook") {
                        // `Kind` 또는 `Kind(인자, …)` — 인자는 실수(Scalar) 또는 `#n`(Ref).
                        usize open = 0;
                        while (open < rest.ByteCount() && rest.Data()[open] != '(') {
                            ++open;
                        }

                        const StringView kindName(rest.Data(), open);
                        uint32 kind = static_cast<uint32>(BytecodeHookKind::Count);
                        for (uint32 index = 0; index < static_cast<uint32>(BytecodeHookKind::Count);
                             ++index) {
                            kind = kindName == BytecodeHookKindName(
                                                   static_cast<BytecodeHookKind>(index))
                                       ? index
                                       : kind;
                        }

                        if (kind == static_cast<uint32>(BytecodeHookKind::Count)) {
                            return bad("unknown hook kind");
                        }

                        arguments.Clear();
                        refArguments.Clear();
                        bool parsed = true;
                        if (open < rest.ByteCount()) {
                            if (!rest.EndsWith(")")) {
                                return bad("expected ')' after the hook arguments");
                            }

                            RunValues::ForEachItem(
                                StringView(rest.Data() + open + 1, rest.ByteCount() - open - 2),
                                [&](StringView item) noexcept {
                                    item = RunValues::Trim(item);
                                    if (item.StartsWith("#")) {
                                        auto object = TryParseUInt32(
                                            StringView(item.Data() + 1, item.ByteCount() - 1));
                                        const uint64 argument =
                                            object ? host.ObjectArgument(object.Value(), "Actor")
                                                   : 0;
                                        parsed =
                                            parsed && argument != 0 &&
                                            static_cast<bool>(refArguments.TryPushBack(argument));
                                        return;
                                    }

                                    auto real = TryParseDouble(item);
                                    parsed = parsed && real &&
                                             static_cast<bool>(arguments.TryPushBack(
                                                 platform::BitCast<uint64>(real.Value())));
                                });
                        }

                        if (!parsed) {
                            return bad("cannot read the hook arguments");
                        }

                        auto hooked = session.RunHook(instance, static_cast<BytecodeHookKind>(kind),
                                                      arguments.View(), refArguments.View());
                        if (!hooked) {
                            if (hooked.Error() != ErrorCode::InvalidArgument) {
                                return Result<void>::Failure(hooked.Error());
                            }

                            return bad("the VM rejected the hook arguments");
                        }

                        result = hooked.Value();
                        ran = true;
                    } else if (verb == "emit") {
                        // `emit #n Class.Event(인자)` — host 진입 발행(깊이 1). Class가 이 모듈
                        // 클래스면 이 모듈 경로, 아니면 엔진 이벤트(빈 경로)다.
                        usize open = 0;
                        while (open < rest.ByteCount() && rest.Data()[open] != '(') {
                            ++open;
                        }

                        const StringView eventName(rest.Data(), open);
                        usize dot = 0;
                        while (dot < eventName.ByteCount() && eventName.Data()[dot] != '.') {
                            ++dot;
                        }

                        const StringView owner(eventName.Data(), dot);
                        bool local = false;
                        for (const BytecodeClass& candidate : loaded.classes) {
                            local = local || loaded.String(candidate.name) == owner;
                        }

                        VmEventKey key;
                        key.path = local ? loaded.String(loaded.path) : StringView();
                        key.name = eventName;
                        arguments.Clear();
                        refArguments.Clear();
                        bool parsed = true;
                        if (open < rest.ByteCount()) {
                            if (!rest.EndsWith(")")) {
                                return bad("expected ')' after the event arguments");
                            }

                            RunValues::ForEachItem(
                                StringView(rest.Data() + open + 1, rest.ByteCount() - open - 2),
                                [&](StringView item) noexcept {
                                    item = RunValues::Trim(item);
                                    if (item.StartsWith("#")) {
                                        auto object = TryParseUInt32(
                                            StringView(item.Data() + 1, item.ByteCount() - 1));
                                        const uint64 argument =
                                            object ? host.ObjectArgument(object.Value(), "Actor")
                                                   : 0;
                                        parsed =
                                            parsed && argument != 0 &&
                                            static_cast<bool>(refArguments.TryPushBack(argument));
                                        return;
                                    }

                                    auto real = TryParseDouble(item);
                                    parsed = parsed && real &&
                                             static_cast<bool>(arguments.TryPushBack(
                                                 platform::BitCast<uint64>(real.Value())));
                                });
                        }

                        if (!parsed) {
                            return bad("cannot read the event arguments");
                        }

                        const uint64 publisher = host.ObjectArgument(number.Value(), StringView());
                        if (auto emitted = session.Emit(publisher, key, arguments.View(),
                                                        refArguments.View(), {});
                            !emitted) {
                            if (emitted.Error() == ErrorCode::OutOfMemory) {
                                return Result<void>::Failure(emitted.Error());
                            }

                            return bad("the session rejected the emit (queue limit)");
                        }

                        actual.Append("ok");
                        delivered = true;
                    } else {
                        return bad("unknown directive");
                    }
                }
            }

            if (ran) {
                switch (result.outcome) {
                case VmOutcome::Returned:
                    actual.Append("ok");
                    break;
                case VmOutcome::Faulted:
                    actual.AppendFormat("fault {} at {}:{}", VmFaultKindName(result.fault.kind),
                                        result.fault.location.line, result.fault.location.column);
                    break;
                case VmOutcome::Ended:
                    actual.AppendFormat("ended error({})", result.errorCode - 1);
                    break;
                case VmOutcome::Suspended:
                    actual.Append("suspended");
                    break;
                }

                actual.AppendFormat(" ({} steps)", result.steps);
            }

            actual.Append("\n");
            if (execution.DroppedLogs() != 0 || host.RanOutOfMemory()) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            for (uint32 index = 0; ran && index < execution.LogCount(); ++index) {
                actual.Append(RunLogPrefix)
                    .Append(VmLogLevelName(execution.LogLevel(index)))
                    .Append(" ");
                RunValues::AppendQuoted(actual, execution.LogMessage(index));
                actual.Append("\n");
            }

            for (const String& record : host.Trace()) {
                actual.Append(RunHostPrefix).Append(record.View()).Append("\n");
            }

            // 이 진입이 실행한 handler(SparksRuntime 2.3절): `  deliver #구독자 <함수> -> 결과`.
            if (ran || delivered) {
                AppendDeliveries(actual, session, host);
            }

            if (delivered) {
                for (uint32 index = 0; index < execution.LogCount(); ++index) {
                    actual.Append(RunLogPrefix)
                        .Append(VmLogLevelName(execution.LogLevel(index)))
                        .Append(" ");
                    RunValues::AppendQuoted(actual, execution.LogMessage(index));
                    actual.Append("\n");
                }
            }

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

        // `Class.Method(#this, …)`는 클래스 메서드다: 가짜 host 객체 #this가 수신자다(6.3절 —
        // instance 실행 모델은 LC6, 여기서는 필드를 host가 흉내 낸다).
        StringView owner;
        StringView member = name;
        for (usize index = 0; index < name.ByteCount(); ++index) {
            if (name.Data()[index] == '.') {
                owner = StringView(name.Data(), index);
                member = StringView(name.Data() + index + 1, name.ByteCount() - index - 1);
            }
        }

        const Symbol* symbol = nullptr;
        for (const Symbol& candidate : compilation.Symbols()) {
            const bool method = !owner.IsEmpty() && candidate.kind == SymbolKind::Method &&
                                candidate.owner != NoSymbol &&
                                compilation.Symbols()[candidate.owner].name == owner;
            if ((method || (owner.IsEmpty() && candidate.kind == SymbolKind::Function)) &&
                candidate.module == mainModule && candidate.name == member) {
                symbol = &candidate;
            }
        }

        if (problem.ByteCount() == 0 && (function == NoBytecodeIndex || symbol == nullptr ||
                                         symbol->signature == NoSignature)) {
            problem.AppendFormat("no top-level function or method '{}'", name);
        }

        arguments.Clear();
        refArguments.Clear();
        for (VmBuffer* buffer : buffers) {
            execution.Release(buffer);
        }

        buffers.Clear();
        RunType returned;
        if (problem.ByteCount() == 0) {
            const Signature& signature = compilation.Signatures()[symbol->signature];
            returned = values.TypeOf(signature.returnType, true);
            uint32 returnSlots = 0;
            if (!values.Slots(returned, returnSlots)) {
                problem.Append("the return type is not supported by .run yet");
            }

            uint32 parameter = 0;
            bool receiver = !owner.IsEmpty();
            RunValues::ForEachItem(
                StringView(call.Data() + open + 1, call.ByteCount() - open - 2),
                [&](StringView item) noexcept {
                    if (problem.ByteCount() != 0) {
                        return;
                    }

                    if (receiver) {
                        // 수신자(Ref 매개변수 0번 — this)는 그 클래스의 가짜 host 객체다.
                        receiver = false;
                        const StringView text = RunValues::Trim(item);
                        auto number =
                            text.StartsWith("#")
                                ? TryParseUInt32(StringView(text.Data() + 1, text.ByteCount() - 1))
                                : Result<uint32>::Failure(ErrorCode::InvalidArgument);
                        if (!number || number.Value() == 0) {
                            problem.AppendFormat("cannot read '{}' as the receiver", text);
                            return;
                        }

                        // 할당 실패는 사례 실패가 아니라 OutOfMemory다(LC5d1 리뷰 B6).
                        const uint64 handle = host.ObjectArgument(number.Value(), owner);
                        if (handle == 0 || !refArguments.TryPushBack(handle)) {
                            values.OutOfMemory();
                        }

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
                        values.Parse(item, type,
                                     type.shape == RunShape::Object ? refArguments : arguments);
                    }
                });
            if (problem.ByteCount() == 0 && parameter != signature.parameterCount) {
                problem.Append("too few arguments");
            }
        }

        if (problem.HasFailed()) {
            return Result<void>::Failure(problem.Status().Error());
        }

        if (values.RanOutOfMemory()) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        if (problem.ByteCount() != 0) {
            StringBuilder<MessageInlineBytes> reason(allocator);
            reason.AppendFormat(".run line {}: {}", lineNumber, problem.View());
            if (reason.HasFailed()) {
                return Result<void>::Failure(reason.Status().Error());
            }

            return fail(reason.View());
        }

        host.ClearTrace();
        // 메서드는 수신자 instance로 부른다(없으면 만든다 — 기본값 `$init`, SparksRuntime 2.2절).
        auto invoke = [&]() noexcept -> Result<VmCallResult> {
            const Span<VmBuffer* const> passed(buffers.Data(), buffers.Size());
            if (owner.IsEmpty()) {
                return execution.Call(linked, function, arguments.View(), refArguments.View(),
                                      passed);
            }

            uint32 classIndex = NoBytecodeIndex;
            for (uint32 index = 0; index < loaded.classes.Size(); ++index) {
                classIndex =
                    loaded.String(loaded.classes[index].name) == owner ? index : classIndex;
            }

            uint32 instance = session.Find(refArguments[0]);
            if (instance == NoBytecodeIndex && classIndex != NoBytecodeIndex) {
                VmCallResult init;
                auto created = session.CreateInstance(refArguments[0], linked, classIndex, init);
                if (!created) {
                    return Result<VmCallResult>::Failure(created.Error());
                }

                if (created.Value() == NoBytecodeIndex) {
                    return Result<VmCallResult>::Success(init); // 초기화 fault
                }

                instance = created.Value();
            }

            if (instance == NoBytecodeIndex) {
                return Result<VmCallResult>::Failure(ErrorCode::InvalidArgument);
            }

            return session.Call(
                instance, linked, function, arguments.View(),
                Span<const uint64>(refArguments.Data() + 1, refArguments.Size() - 1), passed);
        };
        auto called = invoke();
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
                values.FormatValue(actual, returned, execution.ReturnedBuffers()[0]);
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
        case VmOutcome::Suspended:
            actual.Append("suspended");
            break;
        }

        // 빠진 로그 기록은 golden을 거짓으로 다르게 만든다 — 할당 실패로 돌려준다.
        if (execution.DroppedLogs() != 0) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        actual.AppendFormat(" ({} steps)\n", result.steps);
        for (uint32 index = 0; index < execution.LogCount(); ++index) {
            actual.Append(RunLogPrefix)
                .Append(VmLogLevelName(execution.LogLevel(index)))
                .Append(" ");
            RunValues::AppendQuoted(actual, execution.LogMessage(index));
            actual.Append("\n");
        }

        // 가짜 host의 native 호출 기록(SparksVm 6.3절).
        for (const String& record : host.Trace()) {
            actual.Append(RunHostPrefix).Append(record.View()).Append("\n");
        }

        if (!owner.IsEmpty()) {
            AppendDeliveries(actual, session, host);
        }

        if (host.RanOutOfMemory()) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        // 반환 값과 인자 버퍼를 놓으면 ledger는 0이다(SparksVm 2.2절). host가 필드에 보관한 버퍼는
        // 남는다(V12) — 그때는 보관 수 이상인지만 보고, 사례 끝에 host가 놓은 뒤 0을 본다.
        execution.ReleaseReturned();
        for (VmBuffer* buffer : buffers) {
            execution.Release(buffer);
        }

        buffers.Clear();
        // session instance의 필드도 버퍼를 쥔다(SparksRuntime 2.2절): instance가 있으면 줄마다의 0
        // 검사는 하지 않고 사례 끝에 모두 Detach한 뒤 본다.
        const uint64 held = host.HeldBuffers();
        if (session.AttachedCount() == 0 &&
            (held == 0 ? execution.LiveBuffers() != 0 || execution.LiveBytes() != 0
                       : execution.LiveBuffers() < held)) {
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

    session.DetachAll();
    host.ReleaseAll(execution);
    if (execution.LiveBuffers() != 0 || execution.LiveBytes() != 0) {
        return fail(".run: buffers are still alive after the fake host released its fields");
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
