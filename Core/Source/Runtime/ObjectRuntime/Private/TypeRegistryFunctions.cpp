#include "TypeRegistryFunctions.h"

#include <Base/PlacementNew.h>
#include <Core/Text/StringView.h>
#include <Meta/TypeSchema.h>

#include "TypeRegistryRegisterInternal.h"

// 클래스 등록의 리플렉션 함수 서술 검사와 레코드 블록 복사다(Docs/Design/FunctionReflection.ko.md
// FD10). 등록(TypeRegistry::Register)이 블록 크기를 셀 때 FunctionBlockLayout을, 잠금 안에서 기반
// 체인 이름 검사를 FunctionsConflictWithBase로, 레코드를 채울 때 CopyFunctions를 부른다.
namespace rocks::detail {

namespace {

constexpr bool IsPowerOfTwo(uint32 value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

constexpr usize AlignUp(usize value, usize alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

// FieldValue로 통째 읽고 쓰지 않는 칸 형식이다(struct·컨테이너 — 필드의 IsAggregateField와 같은
// 갈래). 그런 칸의 연산표에는 write·read가 없다(값 ops의 copy로 넣고 꺼낸다).
bool IsAggregateSlot(const ParameterSchema& slot) noexcept {
    switch (slot.value.kind) {
    case ValueKind::Struct:
    case ValueKind::Array:
    case ValueKind::Map:
    case ValueKind::Set:
        return true;
    case ValueKind::Optional:
        return slot.element.kind == ValueKind::Struct;
    default:
        return false;
    }
}

// 칸 하나의 서술이다: 값 종류가 있고 frame 범위·정렬 안이며, 값 ops가 있고 write·read는 단일 값
// 칸에만 있다. 강한 참조(ObjectReference)·InstancedStruct·정적 배열은 frame에 둘 수 없다(FD4).
bool ValidSlot(const ParameterSchema& slot, const RuntimeParameterOps& ops,
               const FunctionSchema& function) noexcept {
    const ValueKind kind = slot.value.kind;
    if ((slot.flags & ~KnownParameterFlags) != 0 || kind == ValueKind::Opaque ||
        kind == ValueKind::ObjectReference || kind == ValueKind::InstancedStruct ||
        kind == ValueKind::StaticArray || slot.element.kind == ValueKind::ObjectReference ||
        slot.key.kind == ValueKind::ObjectReference || ops.value == nullptr ||
        ops.value->size == 0 || ops.value->construct == nullptr || ops.value->destroy == nullptr ||
        !IsPowerOfTwo(static_cast<uint32>(ops.value->alignment)) ||
        slot.offset % ops.value->alignment != 0 || slot.offset > function.frameSize ||
        ops.value->size > function.frameSize - slot.offset) {
        return false;
    }

    const bool aggregate = IsAggregateSlot(slot);
    return aggregate ? (ops.write == nullptr && ops.read == nullptr && ops.value->copy != nullptr)
                     : (ops.write != nullptr && ops.read != nullptr);
}

bool ValidName(Span<const char> name) noexcept {
    return name.Size() != 0 && StringView(name.Data(), name.Size()).ValidateUtf8();
}

Span<const char> EffectiveFunctionName(const FunctionSchema& function) noexcept {
    return function.toolName.Size() != 0 ? function.toolName : function.name;
}

} // namespace

Result<FunctionBlockLayout> ValidateFunctionList(const TypeSchema& schema,
                                                 const RuntimeTypeOps& ops) noexcept {
    FunctionBlockLayout layout;
    const Span<const FunctionSchema> functions = schema.functions;
    if (functions.Size() > MaxTypeFunctions || ops.functions.Size() != functions.Size()) {
        return Result<FunctionBlockLayout>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < functions.Size(); ++index) {
        const FunctionSchema& function = functions[index];
        const RuntimeFunctionOps& functionOps = ops.functions[index];
        const bool isStatic = (function.flags & static_cast<uint32>(FunctionFlags::Static)) != 0;
        const bool isConst = (function.flags & static_cast<uint32>(FunctionFlags::Const)) != 0;
        const bool pure = (function.flags & static_cast<uint32>(FunctionFlags::Pure)) != 0;
        const bool returnsResult =
            (function.flags & static_cast<uint32>(FunctionFlags::ReturnsResult)) != 0;
        const bool hasResult = function.result.value.kind != ValueKind::Opaque;
        if (function.id == Guid{} || functionOps.functionId != function.id ||
            (function.flags & ~KnownFunctionFlags) != 0 || (isStatic && isConst) ||
            (pure && !hasResult) || !ValidName(function.name) ||
            (function.toolName.Size() != 0 && !IsAsciiIdentifier(function.toolName)) ||
            function.parameters.Size() > MaxFunctionParameters ||
            functionOps.parameters.Size() != function.parameters.Size() ||
            !IsPowerOfTwo(function.frameAlignment) || function.frameSize == 0 ||
            functionOps.construct == nullptr || functionOps.destroy == nullptr ||
            functionOps.invoke == nullptr || function.result.name.Size() != 0) {
            return Result<FunctionBlockLayout>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize parameter = 0; parameter < function.parameters.Size(); ++parameter) {
            const ParameterSchema& slot = function.parameters[parameter];
            if (!ValidName(slot.name) ||
                !ValidSlot(slot, functionOps.parameters[parameter], function)) {
                return Result<FunctionBlockLayout>::Failure(ErrorCode::InvalidArgument);
            }

            for (usize previous = 0; previous < parameter; ++previous) {
                if (StringView(slot.name.Data(), slot.name.Size()) ==
                    StringView(function.parameters[previous].name.Data(),
                               function.parameters[previous].name.Size())) {
                    return Result<FunctionBlockLayout>::Failure(ErrorCode::AlreadyExists);
                }
            }
        }

        // 반환 칸: 있으면 매개변수 칸과 같은 규칙, 없으면 연산도 없다. 실패 코드 칸은 Result
        // 함수만.
        if (hasResult
                ? !ValidSlot(function.result, functionOps.result, function)
                : (functionOps.result.value != nullptr || functionOps.result.write != nullptr ||
                   functionOps.result.read != nullptr)) {
            return Result<FunctionBlockLayout>::Failure(ErrorCode::InvalidArgument);
        }

        if (returnsResult && (functionOps.errorOffset % alignof(ErrorCode) != 0 ||
                              functionOps.errorOffset > function.frameSize ||
                              sizeof(ErrorCode) > function.frameSize - functionOps.errorOffset)) {
            return Result<FunctionBlockLayout>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize previous = 0; previous < index; ++previous) {
            if (function.id == functions[previous].id ||
                EqualsIgnoringAsciiCase(EffectiveFunctionName(function),
                                        EffectiveFunctionName(functions[previous]))) {
                return Result<FunctionBlockLayout>::Failure(ErrorCode::AlreadyExists);
            }
        }

        layout.parameters += function.parameters.Size();
        if (!AddBytes(layout.textBytes, function.name.Size()) ||
            !AddBytes(layout.textBytes, function.toolName.Size())) {
            return Result<FunctionBlockLayout>::Failure(ErrorCode::Overflow);
        }

        for (const ParameterSchema& slot : function.parameters) {
            if (!AddBytes(layout.textBytes, slot.name.Size())) {
                return Result<FunctionBlockLayout>::Failure(ErrorCode::Overflow);
            }
        }
    }

    layout.functions = functions.Size();
    return Result<FunctionBlockLayout>::Success(layout);
}

usize FunctionBlockLayout::Place(usize offset) noexcept {
    functionOffset = AlignUp(offset, alignof(FunctionSchema));
    parameterOffset =
        AlignUp(functionOffset + functions * sizeof(FunctionSchema), alignof(ParameterSchema));
    functionOpsOffset = AlignUp(parameterOffset + parameters * sizeof(ParameterSchema),
                                alignof(RuntimeFunctionOps));
    parameterOpsOffset = AlignUp(functionOpsOffset + functions * sizeof(RuntimeFunctionOps),
                                 alignof(RuntimeParameterOps));
    return parameterOpsOffset + parameters * sizeof(RuntimeParameterOps);
}

bool FunctionsConflictWithBase(Span<const FunctionSchema> functions,
                               Span<const FunctionSchema> base) noexcept {
    for (const FunctionSchema& function : functions) {
        for (const FunctionSchema& inherited : base) {
            if (function.id == inherited.id ||
                EqualsIgnoringAsciiCase(EffectiveFunctionName(function),
                                        EffectiveFunctionName(inherited))) {
                return true;
            }
        }
    }

    return false;
}

void CopyFunctions(const FunctionBlockLayout& layout, uint8* block, const TypeSchema& schema,
                   const RuntimeTypeOps& ops, uint8*& text, Span<const FunctionSchema>& functions,
                   Span<const RuntimeFunctionOps>& functionOps) noexcept {
    auto* copiedFunctions = reinterpret_cast<FunctionSchema*>(block + layout.functionOffset);
    auto* copiedParameters = reinterpret_cast<ParameterSchema*>(block + layout.parameterOffset);
    auto* copiedOps = reinterpret_cast<RuntimeFunctionOps*>(block + layout.functionOpsOffset);
    auto* copiedParameterOps =
        reinterpret_cast<RuntimeParameterOps*>(block + layout.parameterOpsOffset);
    usize next = 0;
    for (usize index = 0; index < schema.functions.Size(); ++index) {
        const FunctionSchema& source = schema.functions[index];
        FunctionSchema* function = ::new (copiedFunctions + index) FunctionSchema(source);
        function->name = CopyBytes(source.name, text);
        function->toolName = CopyBytes(source.toolName, text);
        RuntimeFunctionOps* functionOpsCopy =
            ::new (copiedOps + index) RuntimeFunctionOps(ops.functions[index]);
        const usize count = source.parameters.Size();
        for (usize parameter = 0; parameter < count; ++parameter) {
            ParameterSchema* slot = ::new (copiedParameters + next + parameter)
                ParameterSchema(source.parameters[parameter]);
            slot->name = CopyBytes(source.parameters[parameter].name, text);
            ::new (copiedParameterOps + next + parameter)
                RuntimeParameterOps(ops.functions[index].parameters[parameter]);
        }

        function->parameters = {copiedParameters + next, count};
        functionOpsCopy->parameters = {copiedParameterOps + next, count};
        next += count;
    }

    functions = {copiedFunctions, schema.functions.Size()};
    functionOps = {copiedOps, schema.functions.Size()};
}

} // namespace rocks::detail
