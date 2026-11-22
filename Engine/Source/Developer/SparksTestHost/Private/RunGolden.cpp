#include <Core/Math/MathUtility.h>
#include <Core/Text/NumberText.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Clock.h>
#include <Platform/Memory.h>
#include <SparksCompiler/Compilation.h>
#include <SparksCompiler/Symbols.h>
#include <SparksVm/BytecodeFile.h>
#include <SparksVm/Vm.h>

#include "ConformanceInternal.h"
#include "FakeHost.h"

#include <Core/Memory/UniquePtr.h>
#include <Core/Move.h>
#include <SparksCompiler/PackageManifest.h>
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
constexpr StringView RunHookPrefix = "  hook ";

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
        : c_(&compilation), problem_(problem), host_(host) {
    }

    // 타입 번호를 풀 compilation을 바꾼다(reload한 revision의 함수·필드 — SparksReload K9).
    void Rebind(const Compilation& compilation) noexcept {
        c_ = &compilation;
    }

    // type의 모양이다. honorOptional이 false면 옵션 표지를 무시한다(Optional의 안쪽).
    RunType TypeOf(uint32 type, bool honorOptional) const noexcept {
        RunType result;
        result.type = type;
        if (type == NoType) {
            result.shape = RunShape::Void;
            return result;
        }

        const TypeEntry& entry = c_->Types()[type];
        if (honorOptional && entry.optional) {
            // 객체의 `T?`는 같은 Ref slot이다(0이 없음).
            result.shape = TypeOf(type, false).shape == RunShape::Object ? RunShape::Object
                                                                         : RunShape::Optional;
            return result;
        }

        if (entry.symbol == NoSymbol) {
            return result;
        }

        const Symbol& symbol = c_->Symbols()[entry.symbol];
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
            (symbol.kind == SymbolKind::EngineType && c_->SchemaIsValid(symbol.index) &&
             c_->Schema(symbol.index).Types()[symbol.item].kind != SchemaTypeKind::Enum &&
             c_->Schema(symbol.index).Types()[symbol.item].kind != SchemaTypeKind::Struct)) {
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
        for (const Symbol& item : c_->Symbols()) {
            if (item.kind == SymbolKind::EnumCase && item.owner == symbol) {
                return true;
            }
        }

        return false;
    }

    // Result<T>의 T다.
    uint32 ResultValue(uint32 type) const noexcept {
        const TypeEntry& entry = c_->Types()[type];
        return c_->TypeArguments()[entry.firstArgument];
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

        const SyntaxTree* tree = c_->ModuleTree(field.module);
        return tree == nullptr ? NoType
                               : c_->ExpressionType(field.module, tree->Child(field.node, 4));
    }

    // struct 값을 `Name(필드: 값, …)`로 쓴다. 필드 slot 위치는 TYPE 구역 규칙(bank 묶음 Scalar·
    // Ref·Buf, 선언 순)으로 다시 계산한다 — VM 배치가 어긋나면 golden이 드러낸다.
    void FormatStruct(StringBuilderBase& out, const RunType& type,
                      const VmBuffer* value) const noexcept {
        const uint32 symbol = c_->Types()[type.type].symbol;
        const Symbol& owner = c_->Symbols()[symbol];
        const SyntaxTree* tree = c_->ModuleTree(owner.module);
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

                const uint32 field = c_->NodeSymbol(owner.module, tree->Child(member, 2));
                if (field == NoSymbol) {
                    continue;
                }

                const RunType fieldType = TypeOf(FieldTypeOf(c_->Symbols()[field]), true);
                uint32 slots = 0;
                const bool bufferField = IsBuffer(fieldType);
                const bool known = Slots(fieldType, slots);
                const uint32 bank = bufferField ? 2 : known ? 0 : 1; // 모르는 타입은 Ref로 본다
                slots = known ? slots : 1;
                if (pass == 0) {
                    groups[bank] += slots;
                    continue;
                }

                out.Append(first ? "" : ", ").Append(c_->Symbols()[field].name).Append(": ");
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

        const TypeEntry& entry = c_->Types()[type.type];
        const bool map = buffer->kind == VmBufferKind::Map;
        const RunType key = TypeOf(c_->TypeArguments()[entry.firstArgument], true);
        const RunType value =
            map ? TypeOf(c_->TypeArguments()[entry.firstArgument + 1], true) : key;
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
        const TypeEntry& entry = c_->Types()[type.type];
        const StringView name = c_->Symbols()[entry.symbol].name;
        const bool map = name == "Map";
        const VmBufferKind kind = map             ? VmBufferKind::Map
                                  : name == "Set" ? VmBufferKind::Set
                                                  : VmBufferKind::Array;
        const RunType first = TypeOf(c_->TypeArguments()[entry.firstArgument], true);
        const RunType second =
            map ? TypeOf(c_->TypeArguments()[entry.firstArgument + 1], true) : RunType();
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

            const uint32 symbol = c_->Types()[type.type].symbol;
            const uint64 handle = host_->ObjectArgument(number.Value(), c_->Symbols()[symbol].name);
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
        const uint32 owner = c_->Types()[type].symbol;
        uint64 seen = 0;
        for (const Symbol& symbol : c_->Symbols()) {
            if (symbol.kind == SymbolKind::EnumCase && symbol.owner == owner) {
                if (seen == index) {
                    return symbol.name;
                }

                ++seen;
            }
        }

        return StringView();
    }

    const Compilation* c_;
    StringBuilderBase& problem_;
    FakeHost* host_ = nullptr;
    bool outOfMemory_ = false;
};

} // namespace

// session의 마지막 진입이 실행한 handler를 `  deliver` 줄로 쓴다(SparksRuntime 2.3절).
static void AppendDeliveries(StringBuilderBase& actual, const VmSession& session,
                             const FakeHost& host) noexcept {
    for (const VmSession::Delivery& delivery : session.Deliveries()) {
        actual.Append(delivery.hook      ? RunHookPrefix
                      : delivery.resumed ? RunResumePrefix
                                         : RunDeliverPrefix);
        host.FormatHandle(actual, delivery.subscriber);
        if (delivery.hook) {
            actual.Append(" ").Append(BytecodeHookKindName(delivery.hookKind));
        } else {
            const BytecodeModule& module = delivery.module->Module();
            actual.Append(" ").Append(module.String(module.functions[delivery.function].name));
        }

        actual.Append(" -> ");
        switch (delivery.result.outcome) {
        case VmOutcome::Returned:
            actual.Append(delivery.result.skipped ? "skipped" : "ok");
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

namespace {

// G4 주입 하나다(0이면 보통 실행).
struct RunInjection {
    uint32 await = 0;
    bool cancel = false;
    uint32 reload = 0; // Gate G5: n번째 ApplyReload를 되돌린다(SparksReload 6절)
};

// session의 관찰 가능한 상태(instance별 module·class·필드 값과 Buf 포인터, 연결·거부·정지 실행·
// 대기·큐 수)를 out에 담는다(실패한 reload가 session을 바꾸지 않았는지 대조 — SparksReload K3).
// 할당 실패면 false.
bool SnapshotSession(const VmSession& session, Array<uint64>& out) noexcept {
    out.Clear();
    bool ok = out.TryPushBack(session.ConnectionCount()) &&
              out.TryPushBack(session.DroppedConnections()) &&
              out.TryPushBack(session.SuspendedCount()) && out.TryPushBack(session.WaitCount()) &&
              out.TryPushBack(session.QueuedCount()) && out.TryPushBack(session.AttachedCount());
    for (uint32 instance = 0; ok && session.ModuleOf(instance) != nullptr; ++instance) {
        const VmSession::FieldView fields = session.Fields(instance);
        ok = out.TryPushBack(uint64(reinterpret_cast<uptr>(session.ModuleOf(instance)))) &&
             out.TryPushBack(session.ClassOf(instance)) && out.TryPushBack(fields.scalars.Size()) &&
             out.TryPushBack(fields.refs.Size()) && out.TryPushBack(fields.buffers.Size()) &&
             out.TryAppend(fields.scalars) && out.TryAppend(fields.refs);
        for (VmBuffer* const buffer : fields.buffers) {
            ok = ok && out.TryPushBack(uint64(reinterpret_cast<uptr>(buffer)));
        }
    }

    return ok;
}

// G5가 주입하는 reload 적용 수 상한이다(적용 표시 bit 수 — 넘는 적용은 행렬에서 빠진다).
constexpr uint32 RunMaxGateReloads = 64;

// 사례 하나의 revision 원문 상한이다(쓰임 표시 bit 수).
constexpr usize RunMaxRevisionFiles = 32;

// `.run`의 실행 단위(모듈 하나 — SparksReload 6절 LC7c2a)다: compilation 모듈 번호와 `.sparkc` 왕복
// bytecode·링크. 링크는 session이 가리키므로 옮기지 않는다(UniquePtr로 둔다).
struct RunUnit {
    explicit RunUnit(Allocator& allocator) noexcept : loaded(allocator), linked(allocator) {
    }

    uint32 module = NoModule;
    BytecodeModule loaded;
    VmLinkedModule linked;
};

// 지금 revision의 실행 단위 하나다(reload가 바꾼다).
struct RunCode {
    const BytecodeModule* code = nullptr;
    const VmLinkedModule* link = nullptr;
    uint32 module = NoModule; // currentCompilation의 모듈 번호
};

// `fields`가 읽는 클래스 사슬 상한이다(VmSession 기반 깊이와 같다).
constexpr uint32 RunMaxChain = 64;

// reload한 revision 하나다(SparksReload K9): 컴파일 결과와 `.sparkc` 왕복 bytecode·링크.
struct RunRevision {
    explicit RunRevision(Allocator& allocator) noexcept
        : sources(allocator), compilation(allocator, sources), module(allocator),
          linked(allocator) {
    }

    SourceSet sources;
    Compilation compilation;
    BytecodeModule module;
    VmLinkedModule linked;
    uint32 main = NoModule;
};

// revision 원문을 파일 사례와 같은 설정(schema·manifest·모듈 경로)으로 컴파일하고 `.sparkc` 왕복 뒤
// host로 링크한다. 진단·거부는 false와 why, 할당 실패는 Result 실패다.
Result<bool> CompileRevision(Allocator& allocator, const CaseInput& input, const CaseFile& source,
                             FakeHost& host, RunRevision& revision,
                             StringBuilderBase& why) noexcept {
    auto refuse = [&](StringView text) noexcept -> Result<bool> {
        if (auto appended = why.TryAppendFormat("revision '{}' {}", source.path, text); !appended) {
            return Result<bool>::Failure(appended.Error());
        }

        return Result<bool>::Success(false);
    };
    DiagnosticList diagnostics(allocator);
    for (const CaseFile& schema : input.schemas) {
        auto id = revision.sources.TryAdd(schema.path, schema.text, diagnostics);
        if (!id) {
            return Result<bool>::Failure(id.Error());
        }

        if (auto added = revision.compilation.AddBindingSchema(id.Value()); !added) {
            return Result<bool>::Failure(added.Error());
        }
    }

    auto manifest = revision.sources.TryAdd(ManifestFileName, FileCaseManifest, diagnostics);
    if (!manifest) {
        return Result<bool>::Failure(manifest.Error());
    }

    auto package = revision.compilation.AddPackage(manifest.Value());
    if (!package) {
        return Result<bool>::Failure(package.Error());
    }

    // 진단 경로는 사례 이름 그대로다(원문 위치는 보고하지 않는다 — 진단이 있으면 실패).
    auto file = revision.sources.TryAdd(input.name, source.text, diagnostics);
    if (!file) {
        return Result<bool>::Failure(file.Error());
    }

    auto module = revision.compilation.AddModule(package.Value(), file.Value(), FileCaseModulePath);
    if (!module) {
        return Result<bool>::Failure(module.Error());
    }

    revision.main = module.Value();
    if (auto checked = revision.compilation.Check(diagnostics); !checked) {
        return Result<bool>::Failure(checked.Error());
    }

    const BytecodeModule* bytecode = revision.compilation.ModuleBytecode(revision.main);
    if (diagnostics.Count() != 0 || bytecode == nullptr) {
        return refuse("has diagnostics");
    }

    Array<uint8> bytes(allocator);
    if (auto written = WriteBytecodeFile(*bytecode, bytes); !written) {
        return Result<bool>::Failure(written.Error());
    }

    BytecodeRejection rejection;
    if (auto read = ReadBytecodeFile(bytes.View(), revision.module, rejection); !read) {
        if (read.Error() != ErrorCode::InvalidArgument) {
            return Result<bool>::Failure(read.Error());
        }

        return refuse("was rejected by the bytecode reader");
    }

    StringBuilder<MessageInlineBytes> reason(allocator);
    if (auto link = LinkModule(revision.module, &host, revision.linked, reason); !link) {
        if (link.Error() != ErrorCode::InvalidArgument) {
            return Result<bool>::Failure(link.Error());
        }

        return refuse("was not linked");
    }

    return Result<bool>::Success(true);
}

// 폴더 사례 revision 하나다(SparksReload 6절 LC7c2b): 덮어쓴 파일·compilation과 묶음 모듈의 새
// 실행 단위(session이 참조할 수 있어 사례 끝까지 보관한다).
struct RunFolderRevision {
    explicit RunFolderRevision(Allocator& allocator) noexcept
        : sources(allocator), compilation(allocator, sources), files(allocator), units(allocator) {
    }

    SourceSet sources;
    Compilation compilation;
    Array<CaseFile> files;
    Array<UniquePtr<RunUnit>> units;
};

// 모듈 경로 글자다(없으면 빈 글자).
StringView RunModulePath(const BytecodeModule& module) noexcept {
    return module.path == NoBytecodeIndex ? StringView() : module.String(module.path);
}

// 폴더 revision을 컴파일한다: 사례 파일에 revision 파일을 같은 경로로 덮어 다시 컴파일하고, 묶음
// (원문이 바뀐 모듈 ∪ 재컴파일 범위 ∪ 그것을 함수 import하는 모듈의 전이 폐포)의 모듈만 새
// revision으로 링크한 뒤 그 import를 묶음 밖 지금 모듈에 잇는다. pairs는 묶음 쌍, next는 적용 뒤
// 실행 단위 표다(codes와 같은 순서). 진단·거부는 false와 why, 할당 실패는 Result 실패다.
Result<bool> CompileFolderRevision(Allocator& allocator, const CaseInput& input,
                                   const CaseRevision& source, FakeHost& host,
                                   const Compilation& before, Span<const RunCode> codes,
                                   RunFolderRevision& revision, Array<VmReloadModule>& pairs,
                                   Array<RunCode>& next, StringBuilderBase& why,
                                   RunTimings& spent) noexcept {
    uint64 mark = platform::MonotonicNanoseconds();
    // 단계 시간을 spent의 칸에 더하고 다음 단계를 시작한다(LC7c3 측정).
    auto lap = [&](uint64& slot) noexcept {
        const uint64 now = platform::MonotonicNanoseconds();
        slot += now - mark;
        mark = now;
    };
    auto refuse = [&](StringView text) noexcept -> Result<bool> {
        if (auto appended = why.TryAppendFormat("revision '{}' {}", source.version, text);
            !appended) {
            return Result<bool>::Failure(appended.Error());
        }

        return Result<bool>::Success(false);
    };
    // 덮어쓰기(같은 경로) — 모듈 추가·삭제는 다루지 않는다.
    for (const CaseFile& file : input.files) {
        StringView text = file.text;
        for (const CaseFile& replaced : source.files) {
            text = replaced.path == file.path ? replaced.text : text;
        }

        if (auto pushed = revision.files.TryPushBack(CaseFile{file.path, text}); !pushed) {
            return Result<bool>::Failure(pushed.Error());
        }
    }

    for (const CaseFile& replaced : source.files) {
        bool known = false;
        for (const CaseFile& file : input.files) {
            known = known || file.path == replaced.path;
        }

        if (!known) {
            return refuse("adds a file (adding or removing modules is not supported)");
        }
    }

    DiagnosticList diagnostics(allocator);
    for (const CaseFile& schema : input.schemas) {
        auto id = revision.sources.TryAdd(schema.path, schema.text, diagnostics);
        if (!id) {
            return Result<bool>::Failure(id.Error());
        }

        if (auto added = revision.compilation.AddBindingSchema(id.Value()); !added) {
            return Result<bool>::Failure(added.Error());
        }
    }

    CaseInput overlay = input;
    overlay.files = revision.files.View();
    StringView layout;
    auto added = AddFolderSources(allocator, overlay, revision.sources, diagnostics,
                                  revision.compilation, layout);
    if (!added) {
        return Result<bool>::Failure(added.Error());
    }

    if (!added.Value()) {
        return refuse(layout);
    }

    if (auto checked = revision.compilation.Check(diagnostics); !checked) {
        return Result<bool>::Failure(checked.Error());
    }

    if (diagnostics.Count() != 0 || revision.compilation.ModuleCount() != codes.Size()) {
        return refuse("has diagnostics or another module list");
    }

    lap(spent.compile);
    // 새 compilation 모듈을 실행 단위(모듈 경로)로 맞춘다.
    if (!next.TryAppend(codes)) {
        return Result<bool>::Failure(ErrorCode::OutOfMemory);
    }

    for (usize unit = 0; unit < codes.Size(); ++unit) {
        next[unit].module = NoModule;
        for (uint32 module = 0; module < revision.compilation.ModuleCount(); ++module) {
            const BytecodeModule* bytecode = revision.compilation.ModuleBytecode(module);
            if (bytecode != nullptr &&
                RunModulePath(*bytecode) == RunModulePath(*codes[unit].code)) {
                next[unit].module = module;
            }
        }

        if (next[unit].module == NoModule) {
            return refuse("has diagnostics or another module list");
        }
    }

    // 묶음: 원문이 바뀐 모듈과 재컴파일 범위(SparksReload K10), 그것을 함수 import하는 모듈.
    Array<uint8> bundle(allocator);
    if (!bundle.TryResize(codes.Size())) {
        return Result<bool>::Failure(ErrorCode::OutOfMemory);
    }

    for (usize unit = 0; unit < codes.Size(); ++unit) {
        const BytecodeModule& now = *revision.compilation.ModuleBytecode(next[unit].module);
        bool changed = false;
        for (usize at = 0; at < BytecodeHashBytes; ++at) {
            changed = changed || now.contentHash[at] != codes[unit].code->contentHash[at];
        }

        bundle[unit] = changed ? 1 : 0;
    }

    Array<uint32> scopeModules(allocator);
    Array<uint64> scopeFunctions(allocator);
    if (auto scoped =
            FindRecompileScope(before, revision.compilation, scopeModules, scopeFunctions);
        !scoped) {
        if (scoped.Error() != ErrorCode::InvalidArgument) {
            return Result<bool>::Failure(scoped.Error());
        }

        return refuse("has no recompile scope");
    }

    for (usize unit = 0; unit < codes.Size(); ++unit) {
        for (const uint32 module : scopeModules) {
            bundle[unit] |= module == next[unit].module ? 1 : 0;
        }

        for (const uint64 function : scopeFunctions) {
            bundle[unit] |= uint32(function >> 32) == next[unit].module ? 1 : 0;
        }
    }

    for (bool grew = true; grew;) {
        grew = false;
        for (usize unit = 0; unit < codes.Size(); ++unit) {
            const BytecodeModule& now = *revision.compilation.ModuleBytecode(next[unit].module);
            for (usize ref = 0; bundle[unit] == 0 && ref < now.refs.Size(); ++ref) {
                if (now.refs[ref].kind != BytecodeRefKind::ImportedFunction) {
                    continue;
                }

                const StringView name = now.String(now.refs[ref].name);
                for (usize other = 0; other < codes.Size(); ++other) {
                    const StringView path = RunModulePath(*codes[other].code);
                    if (bundle[other] != 0 && name.ByteCount() > path.ByteCount() &&
                        name.StartsWith(path) && name.Data()[path.ByteCount()] == ':') {
                        bundle[unit] = 1;
                        grew = true;
                    }
                }
            }
        }
    }

    lap(spent.bundle);
    // 묶음 모듈의 새 revision: `.sparkc` 왕복 → 링크 → import(묶음 밖은 지금 모듈).
    Array<VmLinkedModule*> fresh(allocator);
    Array<const VmLinkedModule*> targets(allocator);
    for (usize unit = 0; unit < codes.Size(); ++unit) {
        if (bundle[unit] == 0) {
            if (auto pushed = targets.TryPushBack(codes[unit].link); !pushed) {
                return Result<bool>::Failure(pushed.Error());
            }

            continue;
        }

        auto made = UniquePtr<RunUnit>::TryMake(allocator, allocator);
        if (!made) {
            return Result<bool>::Failure(made.Error());
        }

        if (auto pushed = revision.units.TryPushBack(Move(made.Value())); !pushed) {
            return Result<bool>::Failure(pushed.Error());
        }

        RunUnit& created = *revision.units[revision.units.Size() - 1];
        created.module = next[unit].module;
        Array<uint8> bytes(allocator);
        if (auto written =
                WriteBytecodeFile(*revision.compilation.ModuleBytecode(created.module), bytes);
            !written) {
            return Result<bool>::Failure(written.Error());
        }

        BytecodeRejection rejection;
        if (auto read = ReadBytecodeFile(bytes.View(), created.loaded, rejection); !read) {
            if (read.Error() != ErrorCode::InvalidArgument) {
                return Result<bool>::Failure(read.Error());
            }

            return refuse("was rejected by the bytecode reader");
        }

        StringBuilder<MessageInlineBytes> reason(allocator);
        if (auto link = LinkModule(created.loaded, &host, created.linked, reason); !link) {
            if (link.Error() != ErrorCode::InvalidArgument) {
                return Result<bool>::Failure(link.Error());
            }

            return refuse("was not linked");
        }

        next[unit].code = &created.loaded;
        next[unit].link = &created.linked;
        VmReloadModule pair;
        pair.old = codes[unit].link;
        pair.fresh = &created.linked;
        if (!pairs.TryPushBack(pair) || !fresh.TryPushBack(&created.linked)) {
            return Result<bool>::Failure(ErrorCode::OutOfMemory);
        }
    }

    if (fresh.IsEmpty()) {
        return refuse("changes no module");
    }

    StringBuilder<MessageInlineBytes> reason(allocator);
    if (auto imported = LinkImports(fresh.View(), targets.View(), reason); !imported) {
        if (imported.Error() != ErrorCode::InvalidArgument) {
            return Result<bool>::Failure(imported.Error());
        }

        return refuse("imports were not linked");
    }

    lap(spent.link);
    return Result<bool>::Success(true);
}

Result<void> RunScriptCase(Allocator& allocator, const CaseInput& input,
                           const Compilation& compilation, uint32 mainModule, Report& report,
                           const RunInjection& injection, uint32& awaitsSeen,
                           uint64& reloadsApplied) noexcept {
    const bool gateOnly = injection.await != 0 || injection.reload != 0;
    // 적용에 성공한 reload(ApplyReload 호출 순 bit — G5는 이것만 주입한다: 앞 단계에서 스스로
    // 거부되는 적용에는 되돌릴 임시 반영이 없다).
    reloadsApplied = 0;
    uint32 reloadsSeen = 0;
    StringBuilder<MessageInlineBytes> message(allocator);
    auto fail = [&](StringView reason) noexcept -> Result<void> {
        if (gateOnly) {
            return Result<void>::Success(); // 주입 실행은 출력·지시 오류를 보지 않는다(누수만)
        }

        if (auto formatted = message.TryAppendFormat("{}: {}", input.name, reason); !formatted) {
            return formatted;
        }

        return report.TryAddFailure(message.View());
    };
    // 실행 단위(SparksReload 6절 LC7c2a): 파일 사례는 그 모듈 하나, 폴더 사례는 모든 모듈이다.
    // 실행은 `.sparkc` 왕복을 거친 모듈로 하고(로드 경로와 같게) 가짜 host로 링크한다(SparksVm
    // 6.1절 — 못 묶으면 사례 실패).
    FakeHost host(allocator, compilation);
    if (host.RanOutOfMemory()) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    Array<UniquePtr<RunUnit>> units(allocator);
    const uint32 firstModule = input.folder ? 0 : mainModule;
    const uint32 endModule = input.folder ? compilation.ModuleCount() : mainModule + 1;
    for (uint32 module = firstModule; module < endModule; ++module) {
        const BytecodeModule* bytecode = compilation.ModuleBytecode(module);
        if (bytecode == nullptr) {
            return fail("has a .run file but a module was not lowered to bytecode (a case "
                        "without diagnostics is needed)");
        }

        auto made = UniquePtr<RunUnit>::TryMake(allocator, allocator);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        if (auto pushed = units.TryPushBack(Move(made.Value())); !pushed) {
            return pushed;
        }

        RunUnit& unit = *units[units.Size() - 1];
        unit.module = module;
        Array<uint8> file(allocator);
        if (auto written = WriteBytecodeFile(*bytecode, file); !written) {
            return written;
        }

        BytecodeRejection rejection;
        if (auto read = ReadBytecodeFile(file.View(), unit.loaded, rejection); !read) {
            if (read.Error() != ErrorCode::InvalidArgument) {
                return read;
            }

            return fail("the .sparkc of a module was rejected");
        }

        StringBuilder<MessageInlineBytes> reason(allocator);
        if (auto link = LinkModule(unit.loaded, &host, unit.linked, reason); !link) {
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

    if (units.IsEmpty()) {
        return fail("has a .run file but no module");
    }

    // 다른 모듈 함수 참조(import)를 푼다(SparksVm 6.5절).
    {
        Array<VmLinkedModule*> linking(allocator);
        for (UniquePtr<RunUnit>& unit : units) {
            if (auto pushed = linking.TryPushBack(&unit->linked); !pushed) {
                return pushed;
            }
        }

        StringBuilder<MessageInlineBytes> reason(allocator);
        if (auto imported = LinkImports(linking.View(), reason); !imported) {
            if (imported.Error() != ErrorCode::InvalidArgument) {
                return imported;
            }

            StringBuilder<MessageInlineBytes> message(allocator);
            message.AppendFormat(".run: the imports were not linked: {}", reason.View());
            if (message.HasFailed()) {
                return Result<void>::Failure(message.Status().Error());
            }

            return fail(message.View());
        }
    }

    // 지금 revision의 실행 단위 표다(reload가 바꾼다). links는 가짜 host의 가상 호출 범위다.
    Array<RunCode> codes(allocator);
    Array<const VmLinkedModule*> links(allocator);
    for (UniquePtr<RunUnit>& unit : units) {
        RunCode entry;
        entry.code = &unit->loaded;
        entry.link = &unit->linked;
        entry.module = unit->module;
        if (auto pushed = codes.TryPushBack(entry); !pushed) {
            return pushed;
        }

        if (auto pushed = links.TryPushBack(&unit->linked); !pushed) {
            return pushed;
        }
    }

    host.SetModules(links.View());
    Array<uint64> refArguments(allocator);
    StringBuilder<MessageInlineBytes> problem(allocator);
    RunValues values(compilation, problem, &host);
    StringBuilder<MessageInlineBytes> actual(allocator);
    Array<uint64> arguments(allocator);
    // reload한 revision(SparksReload K9)은 session보다 오래 산다(session이 옛 revision을 참조할 수
    // 있다). 지금 revision의 bytecode·링크·compilation은 아래 포인터가 가리킨다.
    Array<UniquePtr<RunRevision>> revisions(allocator);
    usize revisionsUsed = 0; // reload가 쓴 revision 원문 bit(쓰지 않은 원문은 사례 실패 — 오타)
    Array<UniquePtr<RunFolderRevision>> folderRevisions(allocator); // 폴더 사례 revision
    usize folderRevisionsUsed = 0;
    const Compilation* currentCompilation = &compilation;
    // 실행 단위는 session 하나다(SparksRuntime 2.2절 — 메서드 호출의 수신자는 session instance).
    VmSession session(allocator, &host);
    VmExecution& execution = session.Execution();
    for (const RunCode& entry : codes) {
        if (auto added = session.AddModule(*entry.link); !added) {
            return added;
        }
    }

    // 실행 단위에서 이름을 찾는다(`<모듈 경로>:이름`으로 고를 수 있다): 찾은 수(1이면 unit·index에
    // 둔다 — 0은 없음, 2 이상은 여러 모듈에 있다).
    auto split = [](StringView text, StringView& path, StringView& name) noexcept {
        path = StringView();
        name = text;
        for (usize at = text.ByteCount(); at > 0; --at) {
            if (text.Data()[at - 1] == ':') {
                path = StringView(text.Data(), at - 1);
                name = StringView(text.Data() + at, text.ByteCount() - at);
                return;
            }
        }
    };
    auto findClass = [&](StringView text, usize& unit, uint32& index) noexcept {
        StringView path;
        StringView name;
        split(text, path, name);
        uint32 found = 0;
        for (usize at = 0; at < codes.Size(); ++at) {
            const BytecodeModule& code = *codes[at].code;
            if (!path.IsEmpty() && code.String(code.path) != path) {
                continue;
            }

            for (uint32 candidate = 0; candidate < code.classes.Size(); ++candidate) {
                if (code.String(code.classes[candidate].name) == name) {
                    unit = at;
                    index = candidate;
                    ++found;
                }
            }
        }

        return found;
    };
    auto findFunction = [&](StringView text, usize& unit, uint32& index) noexcept {
        StringView path;
        StringView name;
        split(text, path, name);
        uint32 found = 0;
        for (usize at = 0; at < codes.Size(); ++at) {
            const BytecodeModule& code = *codes[at].code;
            if (!path.IsEmpty() && code.String(code.path) != path) {
                continue;
            }

            for (uint32 candidate = 0; candidate < code.functions.Size(); ++candidate) {
                if (code.String(code.functions[candidate].name) == name) {
                    unit = at;
                    index = candidate;
                    ++found;
                }
            }
        }

        return found;
    };
    auto unitOf = [&](const VmLinkedModule* link) noexcept {
        for (usize at = 0; at < codes.Size(); ++at) {
            if (codes[at].link == link) {
                return at;
            }
        }

        return codes.Size();
    };

    session.InjectAwait(injection.await, injection.cancel);
    // G4: 주입 실행은 어느 출구에서든 CloseSession 뒤 ledger·유지 자원 0을 본다(누수는 실패 기록).
    struct LeakGate {
        bool active;
        const CaseInput& input;
        const RunInjection& injection;
        VmSession& session;
        FakeHost& host;
        VmExecution& execution;
        Report& report;
        uint32& awaitsSeen;
        Allocator& allocator;
        ~LeakGate() noexcept {
            awaitsSeen = session.AwaitCount();
            if (!active) {
                return;
            }

            // 강제 정리(CloseSession) 전에 Detach 뒤 정지 실행·대기·연결이 0인지 본다(리뷰 4).
            session.DetachAll();
            const bool lingering = session.SuspendedCount() != 0 || session.WaitCount() != 0 ||
                                   session.ConnectionCount() != 0;
            static_cast<void>(session.CloseSession());
            host.ReleaseAll(execution);
            if (lingering || !session.InjectionFired() || execution.LiveBuffers() != 0 ||
                execution.LiveBytes() != 0 || host.HeldLive() != 0 ||
                host.ReleaseAnomalies() != 0) {
                StringBuilder<MessageInlineBytes> leak(allocator);
                if (injection.reload != 0) {
                    leak.AppendFormat("{}: G5 {} after the rollback injected at reload {}",
                                      input.name,
                                      !session.InjectionFired() ? "injection did not fire"
                                                                : "leak or over-release",
                                      injection.reload);
                } else {
                    leak.AppendFormat("{}: G4 {} after the {} injected at await {}", input.name,
                                      !session.InjectionFired() ? "injection did not fire"
                                                                : "leak or over-release",
                                      injection.cancel ? "cancel" : "fault", injection.await);
                }
                if (!leak.HasFailed()) {
                    static_cast<void>(report.TryAddFailure(leak.View()));
                }
            }
        }
    } leakGate{gateOnly, input, injection, session, host, execution, report, awaitsSeen, allocator};

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
            line.StartsWith(RunDeliverPrefix) || line.StartsWith(RunResumePrefix) ||
            line.StartsWith(RunHookPrefix)) {
            continue;
        }

        // instance 지시(SparksRuntime 4절 — LC6a): `new #n Class`, `hook #n Kind[(인자)]`,
        // `fields #n`, `detach #n`. 결과는 `-> ok (N steps)`·fault·필드 표기와 log·host 줄이다.
        if (line.StartsWith("new ") || line.StartsWith("hook ") || line.StartsWith("fields ") ||
            line.StartsWith("detach ") || line.StartsWith("emit ") || line == "phase" ||
            line.StartsWith("phase ") || line.StartsWith("advance ") || line == "fixed" ||
            line.StartsWith("fixed ") || line == "stats" || line.StartsWith("stats ") ||
            line.StartsWith("request ") || line.StartsWith("state ") || line == "apply" ||
            line == "budget" || line.StartsWith("budget ") || line.StartsWith("limits ") ||
            line.StartsWith("load ") || line.StartsWith("apply ") || line == "endframe" ||
            line.StartsWith("endframe ") || line == "close" || line.StartsWith("close ") ||
            line.StartsWith("reload ")) {
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
            } else if (verb == "apply" || verb == "endframe" || verb == "close") {
                // 지연 적용·프레임 끝 정리·session 종료(SparksRuntime 2.6절 — LC6d2). 돌린 수명
                // hook은 `  hook #n <종류> -> 결과` 줄이다.
                actual.Append(directive).Append(RunArrow);
                Result<void> applied = verb == "apply"      ? session.ApplyRequests()
                                       : verb == "endframe" ? session.EndFrame()
                                                            : session.CloseSession();
                if (!applied) {
                    return Result<void>::Failure(applied.Error());
                }

                if (verb == "close") {
                    actual.AppendFormat("ok (ledger {} buffers)", execution.LiveBuffers());
                } else {
                    actual.AppendFormat("ok ({} pending)", session.PendingRequests());
                }

                delivered = verb != "close";
            } else if (verb == "reload") {
                // `reload <버전> [keep|cancel]`(SparksReload K9): 사례 옆 `<사례>.<버전>.spark`를
                // 같은 설정으로 컴파일·링크하고 PlanReload·ApplyReload한다(기본 cancel — Tryout).
                // 다시 부른 Start는 `  hook` 줄이다.
                actual.Append(directive).Append(RunArrow);
                if (!rest.IsEmpty() && rest != "keep" && rest != "cancel") {
                    return bad("'reload' takes 'keep' or 'cancel' after the version");
                }

                // 묶음 쌍과 적용 뒤 실행 단위 표(파일 사례는 쌍 하나 — 폴더 사례는 SparksReload 6절
                // LC7c2b의 묶음).
                Array<VmReloadModule> pairs(allocator);
                Array<RunCode> next(allocator);
                RunTimings spent; // 이 reload의 단계 시간(적용되면 input.timings에 더한다)
                const uint64 started = platform::MonotonicNanoseconds();
                const Compilation* nextCompilation = nullptr;
                StringBuilder<MessageInlineBytes> why(allocator);
                if (!input.folder) {
                    const CaseFile* source = nullptr;
                    for (usize at = 0; at < input.revisions.Size() && at < RunMaxRevisionFiles;
                         ++at) {
                        if (input.revisions[at].path == target) {
                            source = &input.revisions[at];
                            revisionsUsed |= usize(1) << at;
                        }
                    }

                    if (source == nullptr) {
                        return bad("'reload' needs a <case>.<version>.spark file next to the case");
                    }

                    auto made = UniquePtr<RunRevision>::TryMake(allocator, allocator);
                    if (!made) {
                        return Result<void>::Failure(made.Error());
                    }

                    // session이 참조할 수 있으므로 계획 전에 보관한다.
                    if (auto pushed = revisions.TryPushBack(Move(made.Value())); !pushed) {
                        return pushed;
                    }

                    RunRevision& revision = *revisions[revisions.Size() - 1];
                    auto compiled = CompileRevision(allocator, input, *source, host, revision, why);
                    spent.compile += platform::MonotonicNanoseconds() - started;
                    if (!compiled) {
                        return Result<void>::Failure(compiled.Error());
                    }

                    if (!compiled.Value()) {
                        return bad(why.View());
                    }

                    VmReloadModule pair;
                    pair.old = codes[0].link;
                    pair.fresh = &revision.linked;
                    RunCode changed;
                    changed.code = &revision.module;
                    changed.link = &revision.linked;
                    changed.module = revision.main;
                    if (!pairs.TryPushBack(pair) || !next.TryPushBack(changed)) {
                        return Result<void>::Failure(ErrorCode::OutOfMemory);
                    }

                    nextCompilation = &revision.compilation;
                } else {
                    const CaseRevision* source = nullptr;
                    for (usize at = 0;
                         at < input.folderRevisions.Size() && at < RunMaxRevisionFiles; ++at) {
                        if (input.folderRevisions[at].version == target) {
                            source = &input.folderRevisions[at];
                            folderRevisionsUsed |= usize(1) << at;
                        }
                    }

                    if (source == nullptr) {
                        return bad("'reload' needs a <case>.<version> folder next to the case");
                    }

                    auto made = UniquePtr<RunFolderRevision>::TryMake(allocator, allocator);
                    if (!made) {
                        return Result<void>::Failure(made.Error());
                    }

                    if (auto pushed = folderRevisions.TryPushBack(Move(made.Value())); !pushed) {
                        return pushed;
                    }

                    RunFolderRevision& revision = *folderRevisions[folderRevisions.Size() - 1];
                    auto compiled =
                        CompileFolderRevision(allocator, input, *source, host, *currentCompilation,
                                              codes.View(), revision, pairs, next, why, spent);
                    if (!compiled) {
                        return Result<void>::Failure(compiled.Error());
                    }

                    if (!compiled.Value()) {
                        return bad(why.View());
                    }

                    nextCompilation = &revision.compilation;
                }

                // 판정 거부·적용 실패면 session이 그대로여야 한다(SparksReload K3·6절 G5 — 주입
                // 실행에서도 기록한다).
                Array<uint64> before(allocator);
                if (!SnapshotSession(session, before)) {
                    return Result<void>::Failure(ErrorCode::OutOfMemory);
                }

                auto unchanged = [&]() noexcept -> Result<void> {
                    Array<uint64> after(allocator);
                    if (!SnapshotSession(session, after)) {
                        return Result<void>::Failure(ErrorCode::OutOfMemory);
                    }

                    bool same = after.Size() == before.Size();
                    for (usize at = 0; same && at < after.Size(); ++at) {
                        same = after[at] == before[at];
                    }

                    if (same) {
                        return Result<void>::Success();
                    }

                    StringBuilder<MessageInlineBytes> changed(allocator);
                    changed.AppendFormat("{}: a refused or failed reload changed the session",
                                         input.name);
                    if (changed.HasFailed()) {
                        return Result<void>::Failure(changed.Status().Error());
                    }

                    return report.TryAddFailure(changed.View());
                };
                VmReloadPlan plan(allocator);
                StringBuilder<MessageInlineBytes> reason(allocator);
                const uint64 planning = platform::MonotonicNanoseconds();
                auto planned = session.PlanReload(pairs.View(),
                                                  rest == "keep" ? VmReloadExecutions::Keep
                                                                 : VmReloadExecutions::Cancel,
                                                  plan, reason);
                if (!planned) {
                    if (planned.Error() != ErrorCode::InvalidArgument) {
                        return Result<void>::Failure(planned.Error());
                    }

                    if (auto kept = unchanged(); !kept) {
                        return kept;
                    }

                    actual.AppendFormat("rejected ({})", reason.View());
                } else {
                    // 적용 실패(OutOfMemory·InvalidArgument)도 대조한다(할당 실패 전수 시험이 모든
                    // 실패 자리에서 본다 — LC7b 리뷰). G5는 n번째 적용을 되돌린다.
                    if (++reloadsSeen == injection.reload) {
                        session.InjectReloadRollback();
                    }

                    const uint64 applying = platform::MonotonicNanoseconds();
                    spent.plan += applying - planning;
                    auto applied = session.ApplyReload(plan);
                    spent.apply += platform::MonotonicNanoseconds() - applying;
                    if (applied && input.timings != nullptr) {
                        input.timings->compile += spent.compile;
                        input.timings->bundle += spent.bundle;
                        input.timings->link += spent.link;
                        input.timings->plan += spent.plan;
                        input.timings->apply += spent.apply;
                        ++input.timings->reloads;
                    }

                    if (applied && reloadsSeen <= RunMaxGateReloads) {
                        reloadsApplied |= uint64(1) << (reloadsSeen - 1);
                    }

                    if (!applied) {
                        if (auto kept = unchanged(); !kept) {
                            return kept;
                        }
                    }

                    if (!applied && applied.Error() != ErrorCode::InvalidArgument) {
                        return Result<void>::Failure(applied.Error());
                    }

                    // 적용 거부(새 `$init` fault — 기본값 레코드 재합성 실패, SparksReload 5절)는
                    // session이 그대로다.
                    if (!applied) {
                        actual.Append("apply failed (InvalidArgument)");
                    } else {
                        const VmReloadReport& report = applied.Value();
                        if (report.restartError != ErrorCode::None) {
                            return Result<void>::Failure(report.restartError);
                        }

                        actual.Append("ok (");
                        if (input.folder) {
                            actual.AppendFormat("modules {}, ", report.modules);
                        }

                        actual.AppendFormat("instances {}, moved {}, defaulted {}, lost {}, "
                                            "removed {}, ",
                                            report.instances, report.moved, report.defaulted,
                                            report.lost, report.removed);
                        actual.AppendFormat("cancelled {}, restarted {}, previous {})",
                                            report.cancelled, report.restarted, report.previous);
                        for (usize at = 0; at < next.Size(); ++at) {
                            codes[at] = next[at];
                            links[at] = next[at].link;
                        }

                        host.SetModules(links.View());
                        currentCompilation = nextCompilation;
                        values.Rebind(*nextCompilation);
                        delivered = true;
                    }
                }
            } else if (verb == "budget") {
                // 예산 거부 집계(SparksRuntime 2.6절 — LC6d4).
                actual.Append(directive).Append(RunArrow);
                actual.AppendFormat("connections dropped {}, executions skipped {}",
                                    session.DroppedConnections(), session.SkippedExecutions());
            } else if (verb == "limits") {
                // `limits <connections> <executions> [<instructions>]`: 시험용으로 instance 구독
                // 연결·동시 실행 상한(과 조각 명령 예산 — reload 기본값 레코드의 fault 시험)을
                // 줄인다(host는 session을 만들 때 정한다).
                actual.Append(directive).Append(RunArrow);
                usize space = 0;
                while (space < rest.ByteCount() && rest.Data()[space] != ' ') {
                    ++space;
                }

                auto connections = TryParseUInt32(target);
                auto executions = TryParseUInt32(StringView(rest.Data(), space));
                const bool hasInstructions = space < rest.ByteCount();
                auto instructions = hasInstructions
                                        ? TryParseUInt64(StringView(rest.Data() + space + 1,
                                                                    rest.ByteCount() - space - 1))
                                        : Result<uint64>::Success(0);
                if (!connections || !executions || !instructions) {
                    return bad("'limits' needs connection and execution counts");
                }

                VmSessionBudgets budgets = session.Budgets();
                budgets.connectionsPerInstance = connections.Value();
                budgets.executionsPerInstance = executions.Value();
                if (hasInstructions) {
                    budgets.fragmentInstructions = instructions.Value();
                }
                if (!session.SetBudgets(budgets)) {
                    return bad("'limits' needs non-zero counts");
                }

                actual.Append("ok");
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
                    // `advance <Game 초> [<UI 초>] [paused] [x<횟수>]` — 횟수만큼 진행하고 마지막
                    // 진행의 기록만 쓴다(격리 창 같은 긴 프레임 시험).
                    StringView options = rest;
                    uint32 repeat = 1;
                    for (usize at = options.ByteCount(); at > 0; --at) {
                        if (options.Data()[at - 1] == 'x' &&
                            (at == 1 || options.Data()[at - 2] == ' ')) {
                            auto count = TryParseUInt32(
                                StringView(options.Data() + at, options.ByteCount() - at));
                            if (!count || count.Value() == 0) {
                                return bad("'advance' needs a repeat count after 'x'");
                            }

                            repeat = count.Value();
                            options = RunValues::Trim(StringView(options.Data(), at - 1));
                            break;
                        }
                    }

                    constexpr StringView Paused = "paused";
                    const bool paused = options.EndsWith(Paused);
                    const StringView uiText = RunValues::Trim(StringView(
                        options.Data(), options.ByteCount() - (paused ? Paused.ByteCount() : 0)));
                    auto seconds = TryParseDouble(target);
                    auto ui = TryParseDouble(uiText.IsEmpty() ? target : uiText);
                    if (!seconds || !ui) {
                        return bad("'advance' needs seconds");
                    }

                    for (uint32 step = 0; step < repeat && stepped; ++step) {
                        stepped = session.Advance(seconds.Value(), ui.Value(), paused);
                    }
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
                    usize unit = 0;
                    uint32 classIndex = NoBytecodeIndex;
                    const uint32 found = findClass(rest, unit, classIndex);
                    if (found == 0) {
                        return bad("'new' needs a class of the modules");
                    }

                    if (found > 1) {
                        return bad("'new' names a class of several modules (use <path>:Class)");
                    }

                    StringView typePath;
                    StringView typeName;
                    split(rest, typePath, typeName);

                    const uint64 handle = host.ObjectArgument(number.Value(), typeName);
                    if (handle == 0) {
                        return Result<void>::Failure(ErrorCode::OutOfMemory);
                    }

                    auto created =
                        session.CreateInstance(handle, *codes[unit].link, classIndex, result);
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
                    if (instance == NoBytecodeIndex && verb != "emit" && verb != "load") {
                        return bad("the object has no instance (use 'new' first)");
                    }

                    if (verb == "load") {
                        // `load #요청 #객체` | `load #요청 error <코드>`(SparksRuntime 2.6절 —
                        // LC6d5): host가 로드 요청을 끝낸다.
                        uint64 value = 0;
                        uint64 code = 0;
                        if (rest.StartsWith("error ")) {
                            auto parsed =
                                TryParseUInt32(StringView(rest.Data() + 6, rest.ByteCount() - 6));
                            if (!parsed || parsed.Value() == 0) {
                                return bad("'load' needs a non-zero error code");
                            }

                            code = parsed.Value();
                        } else {
                            auto object = rest.StartsWith("#")
                                              ? TryParseUInt32(StringView(rest.Data() + 1,
                                                                          rest.ByteCount() - 1))
                                              : Result<uint32>::Failure(ErrorCode::InvalidArgument);
                            if (!object || object.Value() == 0) {
                                return bad("'load' needs '#<object>' or 'error <code>'");
                            }

                            value = host.ObjectArgument(object.Value(), "DataAsset");
                        }

                        if (!host.CompleteLoad(handle, value, code)) {
                            return bad("'load' needs a load request");
                        }

                        session.CompleteLoad(handle, value, code);
                        actual.Append("ok");
                    } else if (verb == "state") {
                        // instance의 fault 처리 상태(SparksRuntime 2.6절 — LC6d3).
                        const VmSession::InstanceState state = session.StateOf(instance);
                        actual.Append(state == VmSession::InstanceState::Isolated ? "isolated"
                                      : state == VmSession::InstanceState::TicksStopped
                                          ? "ticks stopped"
                                          : "normal");
                    } else if (verb == "request") {
                        // `request #n enable|disable|destroy`(지연 적용 요청 —
                        // SparksRuntime 2.6절).
                        const bool destroy = rest == "destroy";
                        if (!destroy && rest != "enable" && rest != "disable") {
                            return bad("'request' needs enable, disable or destroy");
                        }

                        auto requested = destroy
                                             ? session.RequestDestroy(instance)
                                             : session.RequestEnabled(instance, rest == "enable");
                        if (!requested) {
                            return Result<void>::Failure(requested.Error());
                        }

                        actual.Append("ok");
                    } else if (verb == "detach") {
                        if (auto detached = session.Detach(instance); !detached) {
                            return Result<void>::Failure(detached.Error());
                        }

                        actual.Append("ok");
                    } else if (verb == "fields") {
                        // 기반 사슬(기반 먼저 — 다른 모듈 기반 포함)의 필드: `Class(Field: 값, …)`.
                        // 각 클래스는 그 실행 단위의 bytecode·심볼로 읽는다(LC7c2a).
                        const VmLinkedModule* chainModules[RunMaxChain] = {};
                        uint32 chainClasses[RunMaxChain] = {};
                        const uint32 depth = session.ClassChain(
                            instance, Span<const VmLinkedModule*>(chainModules, RunMaxChain),
                            Span<uint32>(chainClasses, RunMaxChain));
                        for (uint32 at = 0; at < depth; ++at) {
                            if (unitOf(chainModules[at]) == codes.Size()) {
                                return bad("'fields' found a class of an unknown module");
                            }
                        }

                        if (depth == 0) {
                            return bad("'fields' needs an instance");
                        }

                        const VmSession::FieldView storage = session.Fields(instance);
                        const BytecodeModule& own = *codes[unitOf(chainModules[0])].code;
                        actual.Append(own.String(own.classes[chainClasses[0]].name)).Append("(");
                        bool firstField = true;
                        for (uint32 at = depth; at > 0; --at) {
                            const RunCode& owner = codes[unitOf(chainModules[at - 1])];
                            const BytecodeModule* code = owner.code;
                            const BytecodeClass& entry = code->classes[chainClasses[at - 1]];
                            uint32 start[BytecodeBankCount] = {};
                            static_cast<void>(session.RegionStart(*chainModules[at - 1],
                                                                  chainClasses[at - 1], start));
                            const StringView className = code->String(entry.name);
                            for (uint32 field = entry.firstField;
                                 field < entry.firstField + entry.fieldCount; ++field) {
                                const BytecodeTypeField& slot = code->classFields[field];
                                const StringView fieldName = code->String(slot.name);
                                const Symbol* symbol = nullptr;
                                for (const Symbol& candidate : currentCompilation->Symbols()) {
                                    if (candidate.kind == SymbolKind::Field &&
                                        candidate.module == owner.module &&
                                        candidate.name == fieldName &&
                                        candidate.owner != NoSymbol &&
                                        currentCompilation->Symbols()[candidate.owner].name ==
                                            className) {
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
                        // 할당 실패는 형식 오류가 아니다(할당 실패 전수 — LC7a).
                        ErrorCode exhausted = ErrorCode::None;
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
                                        if (!object || object.Value() == 0) {
                                            parsed = false;
                                            return;
                                        }

                                        const uint64 argument =
                                            host.ObjectArgument(object.Value(), "Actor");
                                        if (argument == 0) {
                                            exhausted = ErrorCode::OutOfMemory;
                                        } else if (auto pushed = refArguments.TryPushBack(argument);
                                                   !pushed) {
                                            exhausted = pushed.Error();
                                        }

                                        return;
                                    }

                                    auto real = TryParseDouble(item);
                                    if (!real) {
                                        parsed = false;
                                    } else if (auto pushed = arguments.TryPushBack(
                                                   platform::BitCast<uint64>(real.Value()));
                                               !pushed) {
                                        exhausted = pushed.Error();
                                    }
                                });
                        }

                        if (exhausted != ErrorCode::None) {
                            return Result<void>::Failure(exhausted);
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
                        usize unit = 0;
                        uint32 classIndex = NoBytecodeIndex;
                        const bool local = findClass(owner, unit, classIndex) != 0;
                        VmEventKey key;
                        key.path =
                            local ? codes[unit].code->String(codes[unit].code->path) : StringView();
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
                    actual.Append(result.skipped ? "skipped" : "ok");
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

        const StringView qualified(call.Data(), open);
        usize unit = 0;
        uint32 function = NoBytecodeIndex;
        const uint32 functionsFound =
            problem.ByteCount() == 0 ? findFunction(qualified, unit, function) : 0;
        if (functionsFound > 1) {
            problem.AppendFormat("'{}' is a function of several modules (use <path>:Name)",
                                 qualified);
        }

        StringView functionPath;
        StringView name;
        split(qualified, functionPath, name);

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
        for (const Symbol& candidate : currentCompilation->Symbols()) {
            const bool method = !owner.IsEmpty() && candidate.kind == SymbolKind::Method &&
                                candidate.owner != NoSymbol &&
                                currentCompilation->Symbols()[candidate.owner].name == owner;
            if ((method || (owner.IsEmpty() && candidate.kind == SymbolKind::Function)) &&
                functionsFound == 1 && candidate.module == codes[unit].module &&
                candidate.name == member) {
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
            const Signature& signature = currentCompilation->Signatures()[symbol->signature];
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
                        currentCompilation
                            ->SignatureParameters()[signature.firstParameter + parameter],
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
                return execution.Call(*codes[unit].link, function, arguments.View(),
                                      refArguments.View(), passed);
            }

            const BytecodeModule& code = *codes[unit].code;
            uint32 classIndex = NoBytecodeIndex;
            for (uint32 index = 0; index < code.classes.Size(); ++index) {
                classIndex = code.String(code.classes[index].name) == owner ? index : classIndex;
            }

            uint32 instance = session.Find(refArguments[0]);
            if (instance == NoBytecodeIndex && classIndex != NoBytecodeIndex) {
                VmCallResult init;
                auto created =
                    session.CreateInstance(refArguments[0], *codes[unit].link, classIndex, init);
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
                instance, *codes[unit].link, function, arguments.View(),
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
            if (result.skipped) {
                actual.Append("skipped"); // 격리된 instance(반환 값 없음)
            } else if (values.IsBuffer(returned)) {
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

    // 사례 옆 revision 원문은 모두 `reload`가 써야 한다(이름 오타가 조용히 빠지지 않게).
    if (input.revisions.Size() > RunMaxRevisionFiles) {
        return fail(".run: too many revision files next to the case");
    }

    if (input.folderRevisions.Size() > RunMaxRevisionFiles) {
        return fail(".run: too many revision folders next to the case");
    }

    for (usize at = 0; at < input.folderRevisions.Size(); ++at) {
        if ((folderRevisionsUsed & (usize(1) << at)) == 0) {
            StringBuilder<MessageInlineBytes> unused(allocator);
            unused.AppendFormat(".run: revision folder '{}' is not used by any 'reload'",
                                input.folderRevisions[at].version);
            if (unused.HasFailed()) {
                return Result<void>::Failure(unused.Status().Error());
            }

            return fail(unused.View());
        }
    }

    for (usize at = 0; at < input.revisions.Size(); ++at) {
        if ((revisionsUsed & (usize(1) << at)) == 0) {
            StringBuilder<MessageInlineBytes> unused(allocator);
            unused.AppendFormat(".run: revision '{}' is not used by any 'reload'",
                                input.revisions[at].path);
            if (unused.HasFailed()) {
                return Result<void>::Failure(unused.Status().Error());
            }

            return fail(unused.View());
        }
    }

    // Gate G1(SparksRuntime 4절): Detach 뒤 정지 실행·대기·연결이 0이고(강제 정리 전), CloseSession
    // 뒤 실행 문맥 ledger와 유지 자원이 0이며 과다·이중 해제가 없다.
    session.DetachAll();
    if (session.SuspendedCount() != 0 || session.WaitCount() != 0 ||
        session.ConnectionCount() != 0) {
        return fail(".run: executions, waits or connections remain after detaching everything");
    }

    if (auto closed = session.CloseSession(); !closed) {
        return Result<void>::Failure(closed.Error());
    }

    if (host.ReleaseAnomalies() != 0) {
        return fail(".run: a held resource was released twice or was not held");
    }

    host.ReleaseAll(execution);
    if (execution.LiveBuffers() != 0 || execution.LiveBytes() != 0) {
        return fail(".run: buffers are still alive after the fake host released its fields");
    }

    if (host.HeldLive() != 0) {
        return fail(".run: held resources are still alive after CloseSession");
    }

    if (actual.HasFailed()) {
        return Result<void>::Failure(actual.Status().Error());
    }

    if (gateOnly || actual.View() == input.run) {
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

} // namespace

Result<void> CheckRunGolden(Allocator& allocator, const CaseInput& input,
                            const Compilation& compilation, uint32 mainModule,
                            Report& report) noexcept {
    const usize failures = report.Failures().Size();
    uint32 awaits = 0;
    uint64 reloads = 0;
    if (auto ran = RunScriptCase(allocator, input, compilation, mainModule, report, RunInjection(),
                                 awaits, reloads);
        !ran) {
        return ran;
    }

    // Gate G4(SparksRuntime 4절): 통과한 사례는 모든 AWAIT에서 fault·취소를 하나씩 주입해 누수를
    // 본다.
    if (!input.gateSweep || report.Failures().Size() != failures) {
        return Result<void>::Success();
    }

    for (uint32 await = 1; await <= awaits; ++await) {
        const bool modes[] = {false, true};
        for (const bool cancel : modes) {
            RunInjection injection;
            injection.await = await;
            injection.cancel = cancel;
            uint32 seen = 0;
            uint64 seenReloads = 0;
            if (auto ran = RunScriptCase(allocator, input, compilation, mainModule, report,
                                         injection, seen, seenReloads);
                !ran) {
                return ran;
            }
        }
    }

    // Gate G5(SparksReload 6절): ApplyReload까지 간 reload마다 임시 반영 뒤 되돌리기를 주입한다
    // (출력은 비교하지 않는다 — 상태 대조·주입 발생·누수만).
    for (uint32 reload = 1; reload <= RunMaxGateReloads; ++reload) {
        if ((reloads & (uint64(1) << (reload - 1))) == 0) {
            continue;
        }

        RunInjection injection;
        injection.reload = reload;
        uint32 seen = 0;
        uint64 seenReloads = 0;
        if (auto ran = RunScriptCase(allocator, input, compilation, mainModule, report, injection,
                                     seen, seenReloads);
            !ran) {
            return ran;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::sparks::conformance::detail
