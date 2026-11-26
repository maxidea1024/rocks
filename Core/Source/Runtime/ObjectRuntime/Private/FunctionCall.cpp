#include <Object/FunctionCall.h>

#include <Object/TypeRegistry.h>
#include <Platform/Fatal.h>

#include "TypeRegistryInternal.h"

// 리플렉션 함수 조회(TypeRegistry::FindFunction·FindFunctionByName)와 범용 호출(FunctionCall)이다
// (Docs/Design/FunctionReflection.ko.md FD8–FD10).
namespace rocks {

namespace {

// 호출 frame 할당 태그다('FCAL').
constexpr uint32 FrameTag = 0x4643414Cu;

Span<const char> EffectiveFunctionName(const FunctionSchema& function) noexcept {
    return function.toolName.Size() != 0 ? function.toolName : function.name;
}

bool IsStaticFunction(const FunctionSchema& function) noexcept {
    return (function.flags & static_cast<uint32>(FunctionFlags::Static)) != 0;
}

bool ReturnsResult(const FunctionSchema& function) noexcept {
    return (function.flags & static_cast<uint32>(FunctionFlags::ReturnsResult)) != 0;
}

} // namespace

bool detail::SlotDescriptionMatches(const ParameterSchema& parameter, const RuntimeValueOps* value,
                                    const FrameSlotLayout& expected) noexcept {
    constexpr uint32 FormFlags = static_cast<uint32>(ParameterFlags::ObjectHandle);
    auto same = [](const ValueType& first, const ValueType& second) noexcept {
        return first.kind == second.kind && first.type == second.type;
    };
    return value != nullptr && same(parameter.value, expected.value) &&
           same(parameter.element, expected.element) && same(parameter.key, expected.key) &&
           (parameter.flags & FormFlags) == (expected.flags & FormFlags) &&
           value->size == expected.size && value->alignment == expected.alignment;
}

Result<FunctionHandle> TypeRegistry::FindFunction(TypeId type, Guid functionId) noexcept {
    platform::LockGuard lock(gate_);
    if (closed_) {
        return Result<FunctionHandle>::Failure(ErrorCode::ShuttingDown);
    }

    Record* record = FindLocked(type);
    if (record == nullptr) {
        return Result<FunctionHandle>::Failure(ErrorCode::NotFound);
    }

    if (record->kind != TypeKind::Class) {
        return Result<FunctionHandle>::Failure(ErrorCode::TypeMismatch);
    }

    for (Record* at = record; at; at = at->base) {
        for (usize index = 0; index < at->schema.functions.Size(); ++index) {
            if (at->schema.functions[index].id == functionId) {
                auto lease = LeaseLocked(at);
                if (!lease) {
                    return Result<FunctionHandle>::Failure(lease.Error());
                }

                return Result<FunctionHandle>::Success(FunctionHandle(
                    Move(lease.Value()), at->schema.functions[index], at->ops.functions[index]));
            }
        }
    }

    return Result<FunctionHandle>::Failure(ErrorCode::NotFound);
}

Result<FunctionHandle> TypeRegistry::FindFunctionByName(TypeId type,
                                                        Span<const char> name) noexcept {
    platform::LockGuard lock(gate_);
    if (closed_) {
        return Result<FunctionHandle>::Failure(ErrorCode::ShuttingDown);
    }

    Record* record = FindLocked(type);
    if (record == nullptr) {
        return Result<FunctionHandle>::Failure(ErrorCode::NotFound);
    }

    if (record->kind != TypeKind::Class) {
        return Result<FunctionHandle>::Failure(ErrorCode::TypeMismatch);
    }

    for (Record* at = record; at; at = at->base) {
        for (usize index = 0; index < at->schema.functions.Size(); ++index) {
            if (EqualsIgnoringAsciiCase(EffectiveFunctionName(at->schema.functions[index]), name)) {
                auto lease = LeaseLocked(at);
                if (!lease) {
                    return Result<FunctionHandle>::Failure(lease.Error());
                }

                return Result<FunctionHandle>::Success(FunctionHandle(
                    Move(lease.Value()), at->schema.functions[index], at->ops.functions[index]));
            }
        }
    }

    return Result<FunctionHandle>::Failure(ErrorCode::NotFound);
}

FunctionCall::~FunctionCall() noexcept {
    Release();
}

void FunctionCall::Release() noexcept {
    if (frame_ == nullptr) {
        return;
    }

    function_.Ops().destroy(frame_);
    static_cast<void>(allocator_->Deallocate(frame_));
    frame_ = nullptr;
}

Result<void> FunctionCall::Prepare() noexcept {
    if (frame_ != nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const FunctionSchema& schema = function_.Schema();
    auto memory = allocator_->Allocate(schema.frameSize, schema.frameAlignment, FrameTag);
    if (!memory) {
        return Result<void>::Failure(memory.Error());
    }

    if (!memory.Value()) {
        platform::Fatal("Allocator returned a null function call frame");
    }

    if (auto constructed = function_.Ops().construct(memory.Value(), *allocator_); !constructed) {
        static_cast<void>(allocator_->Deallocate(memory.Value()));
        return constructed;
    }

    frame_ = memory.Value();
    return Result<void>::Success();
}

Result<void> FunctionCall::SetArgument(ObjectRegistry& registry, const ObjectAccessContext& access,
                                       usize index, const FieldValue& value) noexcept {
    const FunctionSchema& schema = function_.Schema();
    if (frame_ == nullptr || index >= schema.parameters.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const RuntimeParameterOps& ops = function_.Ops().parameters[index];
    if (ops.write == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return ops.write(registry, access,
                     static_cast<uint8*>(frame_) + schema.parameters[index].offset, value,
                     *allocator_);
}

Result<void> FunctionCall::SetArgumentDescribed(usize index, const void* source,
                                                const FrameSlotLayout& expected) noexcept {
    const FunctionSchema& schema = function_.Schema();
    if (frame_ == nullptr || index >= schema.parameters.Size() || source == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const RuntimeValueOps* value = function_.Ops().parameters[index].value;
    if (!detail::SlotDescriptionMatches(schema.parameters[index], value, expected)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if (value->copy == nullptr) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    return value->copy(static_cast<uint8*>(frame_) + schema.parameters[index].offset, source,
                       *allocator_);
}

Result<void> FunctionCall::Invoke(ObjectRegistry& registry, const ObjectAccessContext& access,
                                  ObjectHandle self) noexcept {
    const FunctionSchema& schema = function_.Schema();
    if (frame_ == nullptr || IsStaticFunction(schema) != self.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 지난 호출의 실패 코드가 남지 않게 지운다(thunk는 함수가 돌아왔을 때만 쓴다).
    if (ReturnsResult(schema)) {
        *reinterpret_cast<ErrorCode*>(static_cast<uint8*>(frame_) + function_.Ops().errorOffset) =
            ErrorCode::None;
    }

    return function_.Ops().invoke(registry, access, self, frame_);
}

Result<void> FunctionCall::ReadResult(FieldValue& value) const noexcept {
    if (frame_ == nullptr || function_.Ops().result.read == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return function_.Ops().result.read(
        static_cast<const uint8*>(frame_) + function_.Schema().result.offset, value);
}

const void* FunctionCall::ResultDescribed(const FrameSlotLayout& expected) const noexcept {
    const RuntimeValueOps* value = function_.Ops().result.value;
    if (frame_ == nullptr || value == nullptr ||
        !detail::SlotDescriptionMatches(function_.Schema().result, value, expected)) {
        return nullptr;
    }

    return static_cast<const uint8*>(frame_) + function_.Schema().result.offset;
}

ErrorCode FunctionCall::FunctionError() const noexcept {
    if (frame_ == nullptr || !ReturnsResult(function_.Schema())) {
        return ErrorCode::None;
    }

    return *reinterpret_cast<const ErrorCode*>(static_cast<const uint8*>(frame_) +
                                               function_.Ops().errorOffset);
}

} // namespace rocks
