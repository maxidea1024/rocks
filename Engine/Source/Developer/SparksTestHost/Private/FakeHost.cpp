#include "FakeHost.h"

#include <Core/Text/NumberText.h>

// 가짜 host의 구현이다(Docs/Design/SparksVm.ko.md 6.3절). 서술자는 schema 타입 표기에서 따로
// 만들어 컴파일러가 실은 서술자와 글자로 대조한다(같은 규칙의 독립 구현 — 6.1절 서술자 대조).

namespace rocks::sparks::conformance::detail {

namespace {

constexpr uint32 FakeHandleShift = 32;
constexpr uint64 FakeGenerationMask = NumericLimits<uint32>::Max();
constexpr uint8 FakeScalarBank = 0;
constexpr uint8 FakeRefBank = 1;
constexpr uint8 FakeBufBank = 2;
constexpr uint32 FakeMaxSlots = 32;

struct FakeLetter {
    StringView name;
    StringView code;
};

constexpr FakeLetter FakeLetters[] = {
    {"bool", "b"},     {"int8", "i8"},    {"int16", "i16"},   {"int32", "i32"},
    {"int64", "i64"},  {"int", "i64"},    {"uint8", "u8"},    {"uint16", "u16"},
    {"uint32", "u32"}, {"uint64", "u64"}, {"float32", "f32"}, {"float64", "f64"},
    {"float", "f64"},  {"vec2", "v2"},    {"vec3", "v3"},     {"vec4", "v4"},
    {"quat", "q"},     {"Rotator", "r"},  {"Color", "c"},     {"Transform", "t"},
    {"mat4", "m"},     {"string", "s"},   {"Tag", "n"},       {"Clock", "e"}};

// 서술자 단순 글자의 배치다(긴 것 먼저 찾는다).
struct FakeCode {
    StringView code;
    uint8 bank;
    uint8 slots;
};

constexpr FakeCode FakeCodes[] = {
    {"b", 0, 1},   {"i8", 0, 1},  {"i16", 0, 1}, {"i32", 0, 1}, {"i64", 0, 1}, {"u8", 0, 1},
    {"u16", 0, 1}, {"u32", 0, 1}, {"u64", 0, 1}, {"f32", 0, 1}, {"f64", 0, 1}, {"e", 0, 1},
    {"n", 0, 1},   {"v2", 0, 2},  {"v3", 0, 3},  {"v4", 0, 4},  {"q", 0, 4},   {"r", 0, 3},
    {"c", 0, 4},   {"t", 0, 10},  {"m", 0, 16},  {"s", 2, 1},   {"st", 2, 1},  {"x", 1, 1},
    {"w", 1, 1},   {"void", 0, 0}};

// 서술자 text[at]부터 타입 하나를 읽는다(끝 위치·bank·slot 수). 틀리면 false.
bool FakeReadType(StringView text, usize& at, uint8& bank, uint8& slots) noexcept {
    const usize size = text.ByteCount();
    const char head = at < size ? text.Data()[at] : '\0';
    if ((head == 'A' || head == 'S' || head == 'M' || head == 'R' || head == '?') &&
        at + 1 < size && text.Data()[at + 1] == '<') {
        at += 2;
        uint8 innerBank = 0;
        uint8 innerSlots = 0;
        if (!FakeReadType(text, at, innerBank, innerSlots)) {
            return false;
        }

        if (head == 'M') {
            uint8 valueBank = 0;
            uint8 valueSlots = 0;
            if (at >= size || text.Data()[at] != ',' ||
                !FakeReadType(text, ++at, valueBank, valueSlots)) {
                return false;
            }
        }

        if (at >= size || text.Data()[at] != '>') {
            return false;
        }

        ++at;
        const bool scalar = innerBank == FakeScalarBank;
        if (head == 'R' || head == '?') {
            bank = head == '?' && !scalar ? innerBank : scalar ? FakeScalarBank : FakeBufBank;
            slots = static_cast<uint8>(scalar ? 1 + innerSlots : 1);
            return slots <= FakeMaxSlots;
        }

        bank = FakeBufBank;
        slots = 1;
        return true;
    }

    const FakeCode* best = nullptr;
    for (const FakeCode& code : FakeCodes) {
        if (at + code.code.ByteCount() <= size &&
            StringView(text.Data() + at, code.code.ByteCount()) == code.code &&
            (best == nullptr || code.code.ByteCount() > best->code.ByteCount())) {
            best = &code;
        }
    }

    if (best == nullptr) {
        return false;
    }

    at += best->code.ByteCount();
    bank = best->bank;
    slots = best->slots;
    return true;
}

StringView FakeTrim(StringView text) noexcept {
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

// `Owner.Member<…>`에서 타입 인자를 뗀다.
StringView FakeBaseName(StringView name) noexcept {
    for (usize index = 0; index < name.ByteCount(); ++index) {
        if (name.Data()[index] == '<') {
            return StringView(name.Data(), index);
        }
    }

    return name;
}

// `Owner.Member`를 둘로 나눈다(점이 없으면 owner가 비었다).
void FakeSplit(StringView name, StringView& owner, StringView& member) noexcept {
    for (usize index = name.ByteCount(); index > 0; --index) {
        if (name.Data()[index - 1] == '.') {
            owner = StringView(name.Data(), index - 1);
            member = StringView(name.Data() + index, name.ByteCount() - index);
            return;
        }
    }

    owner = StringView();
    member = name;
}

// schema 타입 이름 name을 찾는다.
const SchemaType* FakeFindType(const Compilation& compilation, StringView name,
                               const BindingSchema** schemaOut) noexcept {
    for (uint32 index = 0; index < compilation.SchemaCount(); ++index) {
        if (!compilation.SchemaIsValid(index)) {
            continue;
        }

        const BindingSchema& schema = compilation.Schema(index);
        for (const SchemaType& type : schema.Types()) {
            if (schema.Text(type.name) == name) {
                *schemaOut = &schema;
                return &type;
            }
        }
    }

    return nullptr;
}

} // namespace

FakeHost::FakeHost(Allocator& allocator, const Compilation& compilation) noexcept
    : allocator_(&allocator), compilation_(compilation), bindings_(allocator), objects_(allocator),
      names_(allocator), trace_(allocator) {
}

// schema 타입 표기 text의 서술자를 out에 쓴다(타입 매개변수 이름은 `x`). 모르는 타입은 false.
static bool FakeDescribe(const Compilation& compilation, StringView text,
                         Span<const StringView> typeParameters, StringBuilderBase& out) noexcept {
    text = FakeTrim(text);
    if (text.IsEmpty()) {
        out.Append("void");
        return true;
    }

    if (text.EndsWith("?")) {
        out.Append("?<");
        const bool inner = FakeDescribe(compilation, StringView(text.Data(), text.ByteCount() - 1),
                                        typeParameters, out);
        out.Append(">");
        return inner;
    }

    for (const StringView parameter : typeParameters) {
        if (parameter == text) {
            out.Append("x");
            return true;
        }
    }

    for (const FakeLetter& letter : FakeLetters) {
        if (letter.name == text) {
            out.Append(letter.code);
            return true;
        }
    }

    // 묶음 타입: Array<T>·Set<T>·Map<K, V>·Result<T>.
    struct Generic {
        StringView prefix;
        StringView code;
    };
    constexpr Generic Generics[] = {
        {"Array<", "A<"}, {"Set<", "S<"}, {"Map<", "M<"}, {"Result<", "R<"}};
    for (const Generic& generic : Generics) {
        if (!text.StartsWith(generic.prefix) || !text.EndsWith(">")) {
            continue;
        }

        const StringView inner(text.Data() + generic.prefix.ByteCount(),
                               text.ByteCount() - generic.prefix.ByteCount() - 1);
        out.Append(generic.code);
        usize depth = 0;
        usize begin = 0;
        bool ok = true;
        for (usize index = 0; index <= inner.ByteCount(); ++index) {
            const char c = index < inner.ByteCount() ? inner.Data()[index] : ',';
            depth += c == '<' ? 1 : 0;
            depth -= c == '>' && depth > 0 ? 1 : 0;
            if (c == ',' && depth == 0) {
                ok =
                    ok && FakeDescribe(compilation, StringView(inner.Data() + begin, index - begin),
                                       typeParameters, out);
                if (index < inner.ByteCount()) {
                    out.Append(",");
                }

                begin = index + 1;
            }
        }

        out.Append(">");
        return ok;
    }

    const BindingSchema* schema = nullptr;
    const SchemaType* type = FakeFindType(compilation, text, &schema);
    if (type == nullptr) {
        return false;
    }

    out.Append(type->kind == SchemaTypeKind::Enum     ? "e"
               : type->kind == SchemaTypeKind::Struct ? "st"
                                                      : "x");
    return true;
}

Result<VmHostBinding> FakeHost::Bind(const VmBindRequest& request,
                                     StringBuilderBase& reason) noexcept {
    FakeBinding binding(*allocator_);
    binding.kind = request.kind;
    StringView owner;
    StringView member;
    FakeSplit(FakeBaseName(request.name), owner, member);
    StringBuilder<256> expected(*allocator_);
    StringView resultType;
    switch (request.kind) {
    case BytecodeRefKind::NativeFunction: {
        const BindingSchema* schema = nullptr;
        const SchemaType* type = FakeFindType(compilation_, owner, &schema);
        const SchemaFunction* function = nullptr;
        for (uint32 index = 0; type != nullptr && index < type->functionCount; ++index) {
            const SchemaFunction& candidate = schema->Functions()[type->firstFunction + index];
            if (schema->Text(candidate.name) == member) {
                function = &candidate;
            }
        }

        if (function == nullptr) {
            reason.AppendFormat("no engine function '{}'", request.name);
            return Result<VmHostBinding>::Failure(ErrorCode::NotFound);
        }

        StringView parameters[8];
        uint32 parameterCount = 0;
        for (uint32 index = 0; index < function->typeParameterCount && index < 8; ++index) {
            parameters[parameterCount++] =
                schema->Text(schema->TypeParameters()[function->firstTypeParameter + index].name);
        }

        const Span<const StringView> typeParameters(parameters, parameterCount);
        bool ok = true;
        bool first = function->isStatic;
        if (!first) {
            expected.Append("x");
        }

        for (uint32 index = 0; index < function->parameterCount; ++index) {
            expected.Append(first ? "" : ",");
            first = false;
            ok =
                ok && FakeDescribe(
                          compilation_,
                          schema->Text(schema->Parameters()[function->firstParameter + index].type),
                          typeParameters, expected);
        }

        expected.Append(">");
        StringView returns = schema->Text(function->returns);
        ok = ok && FakeDescribe(compilation_, returns, typeParameters, expected);
        if (!ok) {
            reason.AppendFormat(
                "the engine function '{}' uses a type the fake host cannot describe", request.name);
            return Result<VmHostBinding>::Failure(ErrorCode::Unsupported);
        }

        binding.destroys = member == "RequestDestroy";
        binding.optional = returns.EndsWith("?");
        resultType =
            binding.optional ? StringView(returns.Data(), returns.ByteCount() - 1) : returns;
        for (const StringView parameter : typeParameters) {
            if (parameter == resultType) {
                resultType = request.typeArgument; // 타입 인자를 단 호출의 실제 타입
            }
        }

        if (function->isAsync) {
            binding.slots = 0; // 대기 handle — LC6(호출하면 ContractViolation)
            binding.bank = FakeRefBank;
            binding.literal = 1;
        }

        break;
    }
    case BytecodeRefKind::Field: {
        const BindingSchema* schema = nullptr;
        const SchemaType* type = FakeFindType(compilation_, owner, &schema);
        if (type == nullptr) {
            // Sparks 클래스 필드: schema가 없으므로 서술자의 배치를 그대로 받는다.
            expected.Append(request.descriptor);
            binding.optional = request.descriptor.StartsWith(">?<");
            break;
        }

        const SchemaField* field = nullptr;
        for (uint32 index = 0; index < type->fieldCount; ++index) {
            const SchemaField& candidate = schema->Fields()[type->firstField + index];
            if (schema->Text(candidate.name) == member) {
                field = &candidate;
            }
        }

        if (field == nullptr) {
            reason.AppendFormat("no engine field '{}'", request.name);
            return Result<VmHostBinding>::Failure(ErrorCode::NotFound);
        }

        expected.Append(">");
        const StringView text = schema->Text(field->type);
        if (!FakeDescribe(compilation_, text, Span<const StringView>(), expected)) {
            reason.AppendFormat("the engine field '{}' uses a type the fake host cannot describe",
                                request.name);
            return Result<VmHostBinding>::Failure(ErrorCode::Unsupported);
        }

        binding.optional = text.EndsWith("?");
        resultType = binding.optional ? StringView(text.Data(), text.ByteCount() - 1) : text;
        break;
    }
    case BytecodeRefKind::Context:
        expected.Append(">x");
        resultType = request.name;
        break;
    case BytecodeRefKind::Literal: {
        if (request.literal.IsEmpty()) {
            reason.Append("an empty literal");
            return Result<VmHostBinding>::Failure(ErrorCode::InvalidArgument);
        }

        // Tag·Soft<T>는 이름(Scalar), 하드 참조는 객체(Ref)다.
        const bool named = request.name == "Tag" || request.name.StartsWith("Soft<");
        expected.Append(named ? ">n" : ">x");
        resultType = request.name;
        String text(*allocator_);
        if (!text.TryAssign(request.literal) || !names_.TryPushBack(static_cast<String&&>(text))) {
            return Result<VmHostBinding>::Failure(ErrorCode::OutOfMemory);
        }

        binding.literal = names_.Size();
        break;
    }
    default:
        reason.Append("the fake host does not bind this kind");
        return Result<VmHostBinding>::Failure(ErrorCode::Unsupported);
    }

    if (expected.HasFailed()) {
        return Result<VmHostBinding>::Failure(expected.Status().Error());
    }

    if ((request.kind == BytecodeRefKind::NativeFunction ||
         request.kind == BytecodeRefKind::Field) &&
        expected.View() != request.descriptor) {
        reason.AppendFormat("descriptor '{}' does not match the engine signature '{}'",
                            request.descriptor, expected.View());
        return Result<VmHostBinding>::Failure(ErrorCode::InvalidArgument);
    }

    // 값 배치: 필드·문맥·리터럴은 `>` 뒤 타입, native는 결과(호출 때 쓴다).
    usize at = 0;
    const StringView text = expected.View();
    while (at < text.ByteCount() && text.Data()[at] != '>') {
        ++at;
    }

    // native 서술자의 피연산자 사이 `>`는 없다(`>`는 묶음 닫힘과 겹치므로 top-level만 본다).
    if (request.kind == BytecodeRefKind::NativeFunction) {
        usize depth = 0;
        for (at = 0; at < text.ByteCount(); ++at) {
            const char c = text.Data()[at];
            if (c == '<') {
                ++depth;
            } else if (c == '>' && depth > 0) {
                --depth;
            } else if (c == '>') {
                break;
            }
        }
    }

    ++at;
    uint8 bank = 0;
    uint8 slots = 0;
    if (!FakeReadType(text, at, bank, slots)) {
        reason.AppendFormat("cannot read the descriptor '{}'", text);
        return Result<VmHostBinding>::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 literal = binding.literal;
    binding.bank = bank;
    binding.slots = slots;
    binding.literal = request.kind == BytecodeRefKind::Literal ? literal : 0;
    binding.createsObject = bank == FakeRefBank && literal == 0;
    if (!binding.name.TryAssign(FakeBaseName(request.name)) ||
        !binding.descriptor.TryAssign(text) || !binding.resultType.TryAssign(resultType) ||
        !bindings_.TryPushBack(static_cast<FakeBinding&&>(binding))) {
        return Result<VmHostBinding>::Failure(ErrorCode::OutOfMemory);
    }

    // async native는 링크하되 부르면 ContractViolation이다(대기는 LC6).
    if (request.kind == BytecodeRefKind::NativeFunction && literal == 1) {
        bindings_[bindings_.Size() - 1].slots = 0;
        bindings_[bindings_.Size() - 1].literal = 1;
    }

    VmHostBinding result;
    result.id = bindings_.Size() - 1;
    result.bank = bank;
    result.slots = slots;
    return Result<VmHostBinding>::Success(result);
}

uint64 FakeHost::HandleOf(uint32 index) const noexcept {
    return (uint64(index + 1) << FakeHandleShift) | objects_[index].generation;
}

FakeObject* FakeHost::Find(uint64 handle) noexcept {
    const uint64 number = handle >> FakeHandleShift;
    if (number == 0 || number > objects_.Size()) {
        return nullptr;
    }

    FakeObject& object = objects_[number - 1];
    return object.alive && object.generation == (handle & FakeGenerationMask) ? &object : nullptr;
}

bool FakeHost::IsAlive(uint64 handle) noexcept {
    return Find(handle) != nullptr;
}

uint64 FakeHost::NewObject(StringView type) noexcept {
    FakeObject object{String(*allocator_), 1, true, Array<FakeFieldValue>(*allocator_)};
    if (!object.type.TryAssign(type) || !objects_.TryPushBack(static_cast<FakeObject&&>(object))) {
        outOfMemory_ = true;
        return 0;
    }

    return HandleOf(static_cast<uint32>(objects_.Size() - 1));
}

uint64 FakeHost::ObjectArgument(uint32 number, StringView type) noexcept {
    if (number == 0) {
        return 0;
    }

    while (objects_.Size() < number) {
        if (NewObject(type) == 0) {
            return 0;
        }
    }

    return HandleOf(number - 1);
}

void FakeHost::FormatHandle(StringBuilderBase& out, uint64 handle) const noexcept {
    const uint64 number = handle >> FakeHandleShift;
    if (handle == 0) {
        out.Append("none");
        return;
    }

    const bool alive = number != 0 && number <= objects_.Size() && objects_[number - 1].alive &&
                       objects_[number - 1].generation == (handle & FakeGenerationMask);
    out.AppendFormat("{}#{}", alive ? "" : "dead", number);
}

FakeFieldValue* FakeHost::FieldOf(FakeObject& object, uint64 binding,
                                  VmExecution& execution) noexcept {
    for (FakeFieldValue& value : object.fields) {
        if (value.binding == binding) {
            return &value;
        }
    }

    FakeFieldValue value;
    value.binding = binding;
    if (!object.fields.TryPushBack(value)) {
        outOfMemory_ = true;
        return nullptr;
    }

    FakeFieldValue& added = object.fields[object.fields.Size() - 1];
    const FakeBinding& field = bindings_[binding];
    if (field.createsObject && !field.optional) {
        // non-null 객체 필드는 처음 읽을 때 그 타입의 하위 객체를 만든다(6.3절). NewObject가
        // objects_를 다시 할당해도 added는 그 객체의 fields 저장소(따로 할당)를 가리켜 그대로다.
        StringBuilder<64> name(*allocator_);
        name.Append(field.resultType.View());
        added.slots[0] = NewObject(name.View());
    }

    // non-null 문자열·컨테이너 필드는 그 타입의 0 값(빈 글자·빈 컨테이너 — SL 3.2절)에서
    // 시작한다(필드 기본값 식은 instance 생성 — LC6).
    const StringView descriptor = field.descriptor.View();
    if (field.kind == BytecodeRefKind::Field && field.bank == FakeBufBank && !field.optional &&
        descriptor.ByteCount() > 1) {
        const StringView type(descriptor.Data() + 1, descriptor.ByteCount() - 1);
        VmBuffer* zero = nullptr;
        if (type == "s") {
            auto made = execution.NewString("");
            zero = made ? made.Value() : nullptr;
            outOfMemory_ = outOfMemory_ || !made;
        } else if (type.StartsWith("A<") || type.StartsWith("S<") || type.StartsWith("M<")) {
            usize at = 2;
            uint8 firstBank = 0;
            uint8 firstSlots = 0;
            uint8 secondBank = 0;
            uint8 secondSlots = 0;
            const bool map = type.StartsWith("M<");
            if (FakeReadType(type, at, firstBank, firstSlots) &&
                (!map || (at < type.ByteCount() && type.Data()[at] == ',' &&
                          FakeReadType(type, ++at, secondBank, secondSlots)))) {
                const uint32 marker =
                    map ? EncodeContainerMarker(secondBank, secondSlots, firstBank, firstSlots)
                        : EncodeContainerMarker(firstBank, firstSlots, 0, 0);
                auto made = execution.NewContainer(type.StartsWith("A<")   ? VmBufferKind::Array
                                                   : type.StartsWith("S<") ? VmBufferKind::Set
                                                                           : VmBufferKind::Map,
                                                   marker, Span<const uint64>());
                zero = made ? made.Value() : nullptr;
                outOfMemory_ = outOfMemory_ || (!made && made.Error() == ErrorCode::OutOfMemory);
            }
        }

        added.buffer = zero;
    }

    return &added;
}

void FakeHost::Record(StringView text) noexcept {
    String entry(*allocator_);
    if (!entry.TryAssign(text) || !trace_.TryPushBack(static_cast<String&&>(entry))) {
        outOfMemory_ = true;
    }
}

VmFaultKind FakeHost::GetContext(VmHostCall& call) noexcept {
    FakeBinding& binding = bindings_[call.Binding()];
    if (binding.literal == 0) {
        const uint64 created = NewObject(binding.resultType.View());
        if (created == 0) {
            call.Fail(ErrorCode::OutOfMemory);
            return VmFaultKind::None;
        }

        bindings_[call.Binding()].literal = created; // 문맥 객체 handle을 기억한다
    }

    if (call.Refs().Size() < 1) {
        return VmFaultKind::ContractViolation;
    }

    call.Refs()[0] = bindings_[call.Binding()].literal;
    return VmFaultKind::None;
}

VmFaultKind FakeHost::GetField(VmHostCall& call, uint64 object) noexcept {
    FakeObject* target = Find(object);
    if (target == nullptr) {
        return VmFaultKind::ContractViolation;
    }

    FakeFieldValue* value = FieldOf(*target, call.Binding(), call.Execution());
    if (value == nullptr) {
        call.Fail(ErrorCode::OutOfMemory);
        return VmFaultKind::None;
    }

    const FakeBinding& field = bindings_[call.Binding()];
    if (field.bank == FakeBufBank) {
        call.Execution().Retain(value->buffer);
        call.PutBuffer(0, value->buffer);
        return VmFaultKind::None;
    }

    Span<uint64> out = field.bank == FakeScalarBank ? call.Scalars() : call.Refs();
    for (uint32 slot = 0; slot < field.slots && slot < out.Size(); ++slot) {
        out[slot] = value->slots[slot];
    }

    return VmFaultKind::None;
}

VmFaultKind FakeHost::SetField(VmHostCall& call, uint64 object) noexcept {
    FakeObject* target = Find(object);
    if (target == nullptr) {
        return VmFaultKind::ContractViolation;
    }

    FakeFieldValue* value = FieldOf(*target, call.Binding(), call.Execution());
    if (value == nullptr) {
        call.Fail(ErrorCode::OutOfMemory);
        return VmFaultKind::None;
    }

    const FakeBinding& field = bindings_[call.Binding()];
    if (field.bank == FakeBufBank) {
        VmBuffer* next = call.Buffers()[0];
        call.Execution().Retain(next);
        call.Execution().Release(value->buffer);
        value->buffer = next;
        return VmFaultKind::None;
    }

    const Span<uint64> in = field.bank == FakeScalarBank ? call.Scalars() : call.Refs();
    for (uint32 slot = 0; slot < field.slots && slot < in.Size(); ++slot) {
        value->slots[slot] = in[slot];
    }

    return VmFaultKind::None;
}

VmFaultKind FakeHost::CallNative(VmHostCall& call) noexcept {
    const FakeBinding& binding = bindings_[call.Binding()];
    if (binding.literal == 1) {
        return VmFaultKind::ContractViolation; // async native(대기 — LC6)
    }

    // 인자 표기: 서술자의 피연산자마다 창의 값을 읽는다.
    StringBuilder<256> text(*allocator_);
    text.Append(binding.name.View()).Append("(");
    const StringView descriptor = binding.descriptor.View();
    usize at = 0;
    uint32 cursor[3] = {};
    uint64 receiver = 0;
    bool first = true;
    while (at < descriptor.ByteCount() && descriptor.Data()[at] != '>') {
        const usize begin = at;
        uint8 bank = 0;
        uint8 slots = 0;
        if (!FakeReadType(descriptor, at, bank, slots)) {
            return VmFaultKind::ContractViolation;
        }

        const StringView code(descriptor.Data() + begin, at - begin);
        text.Append(first ? "" : ", ");
        const uint32 offset = cursor[bank];
        cursor[bank] += slots;
        if (bank == FakeRefBank) {
            const uint64 handle = offset < call.Refs().Size() ? call.Refs()[offset] : 0;
            receiver = first ? handle : receiver;
            FormatHandle(text, handle);
        } else if (bank == FakeBufBank) {
            const VmBuffer* buffer =
                offset < call.Buffers().Size() ? call.Buffers()[offset] : nullptr;
            if (code == "s" && buffer != nullptr) {
                text.Append("\"").Append(VmStringOf(buffer)).Append("\"");
            } else {
                text.Append(buffer == nullptr ? "none" : "...");
            }
        } else if (offset < call.Scalars().Size()) {
            const uint64 bits = call.Scalars()[offset];
            if (code == "b") {
                text.Append(bits != 0 ? "true" : "false");
            } else if (code == "f32" || code == "f64") {
                text.AppendFormat("{}", platform::BitCast<double>(bits));
            } else if (code.StartsWith("i")) {
                text.AppendFormat("{}", static_cast<int64>(bits));
            } else {
                text.AppendFormat("{}", bits);
            }
        }

        first = false;
        if (at < descriptor.ByteCount() && descriptor.Data()[at] == ',') {
            ++at;
        }
    }

    text.Append(") -> ");
    // 창이 결과를 담지 못하면 계약 위반이다(6.2절 — 링크가 먼저 막지만 host도 쓰기 전에 본다).
    const usize room = binding.bank == FakeRefBank   ? call.Refs().Size()
                       : binding.bank == FakeBufBank ? call.Buffers().Size()
                                                     : call.Scalars().Size();
    if (binding.slots > room) {
        return VmFaultKind::ContractViolation;
    }

    // 객체 수신자(서술자 첫 피연산자 `x`)가 죽었으면 계약 위반이다(SL 8.4절 — thunk가 non-null·
    // 생존을 본다).
    if (descriptor.StartsWith("x") && !IsAlive(receiver)) {
        return VmFaultKind::ContractViolation;
    }

    // 결과: 객체는 새 객체, Buf는 빈 값(문자열은 빈 글자), Scalar는 0.
    if (binding.slots == 0) {
        text.Append("void");
    } else if (binding.bank == FakeRefBank) {
        const uint64 created = NewObject(binding.resultType.View());
        if (created == 0) {
            call.Fail(ErrorCode::OutOfMemory);
            return VmFaultKind::None;
        }

        call.Refs()[0] = created;
        FormatHandle(text, created);
    } else if (binding.bank == FakeBufBank) {
        const StringView result(descriptor.Data() + at + 1, descriptor.ByteCount() - at - 1);
        VmBuffer* made = nullptr;
        if (result == "s") {
            auto created = call.Execution().NewString("");
            if (!created) {
                call.Fail(created.Error());
                return VmFaultKind::None;
            }

            made = created.Value();
        }

        call.PutBuffer(0, made);
        text.Append(made == nullptr ? "none" : "\"\"");
    } else {
        for (uint32 slot = 0; slot < binding.slots; ++slot) {
            call.Scalars()[slot] = 0;
        }

        const StringView result(descriptor.Data() + at + 1, descriptor.ByteCount() - at - 1);
        text.Append(result == "b" ? "false" : "0");
    }

    if (binding.destroys) {
        if (FakeObject* target = Find(receiver)) {
            target->alive = false;
            ++target->generation;
        }
    }

    if (text.HasFailed()) {
        call.Fail(text.Status().Error());
        return VmFaultKind::None;
    }

    Record(text.View());
    if (outOfMemory_) {
        call.Fail(ErrorCode::OutOfMemory);
    }

    return VmFaultKind::None;
}

VmFaultKind FakeHost::MakeLiteral(VmHostCall& call) noexcept {
    const FakeBinding& binding = bindings_[call.Binding()];
    if ((binding.bank == FakeScalarBank ? call.Scalars().Size() : call.Refs().Size()) < 1) {
        return VmFaultKind::ContractViolation;
    }

    if (binding.bank == FakeScalarBank) {
        call.Scalars()[0] = binding.literal;
        return VmFaultKind::None;
    }

    const uint64 created = NewObject(binding.resultType.View());
    if (created == 0) {
        call.Fail(ErrorCode::OutOfMemory);
        return VmFaultKind::None;
    }

    call.Refs()[0] = created;
    return VmFaultKind::None;
}

uint64 FakeHost::HeldBuffers() const noexcept {
    // 같은 버퍼를 여러 필드가 쥘 수 있으므로 서로 다른 버퍼 수다.
    uint64 count = 0;
    for (usize object = 0; object < objects_.Size(); ++object) {
        for (usize field = 0; field < objects_[object].fields.Size(); ++field) {
            const VmBuffer* buffer = objects_[object].fields[field].buffer;
            if (buffer == nullptr) {
                continue;
            }

            bool seen = false;
            for (usize before = 0; before <= object && !seen; ++before) {
                const usize end = before == object ? field : objects_[before].fields.Size();
                for (usize at = 0; at < end && !seen; ++at) {
                    seen = objects_[before].fields[at].buffer == buffer;
                }
            }

            count += seen ? 0 : 1;
        }
    }

    return count;
}

void FakeHost::ReleaseAll(VmExecution& execution) noexcept {
    for (FakeObject& object : objects_) {
        for (FakeFieldValue& value : object.fields) {
            execution.Release(value.buffer);
            value.buffer = nullptr;
        }
    }
}

} // namespace rocks::sparks::conformance::detail
