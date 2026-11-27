#include <Object/FunctionBinding.h>

#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>

// 리플렉션 함수 바인딩의 바인딩 시점 검사와 호출이다(Docs/Design/EventReflection.ko.md ED3·ED5).
namespace rocks {

namespace {

// FNV-1a 64비트 상수다.
constexpr uint64 FnvOffset = 0xcbf29ce484222325ull;
constexpr uint64 FnvPrime = 0x100000001b3ull;

constexpr uint32 BitsPerByte = 8;
constexpr uint32 BitsPerWord = 64;
constexpr uint64 ByteMask = (1ull << BitsPerByte) - 1;

// value의 byte를 낮은 쪽부터 섞는다.
void Mix(uint64& hash, uint64 value) noexcept {
    for (uint32 shift = 0; shift < BitsPerWord; shift += BitsPerByte) {
        hash ^= (value >> shift) & ByteMask;
        hash *= FnvPrime;
    }
}

void MixType(uint64& hash, const ValueType& type) noexcept {
    Mix(hash, static_cast<uint64>(type.kind));
    Mix(hash, type.type.high);
    Mix(hash, type.type.low);
}

} // namespace

uint64 detail::FrameLayoutDigest(const FrameLayout& layout) noexcept {
    uint64 hash = FnvOffset;
    Mix(hash, layout.count);
    Mix(hash, layout.size);
    Mix(hash, layout.alignment);
    for (uint32 index = 0; index < layout.count; ++index) {
        const FrameSlotLayout& slot = layout.slots[index];
        MixType(hash, slot.value);
        MixType(hash, slot.element);
        MixType(hash, slot.key);
        Mix(hash, slot.flags);
        Mix(hash, slot.size);
        Mix(hash, slot.alignment);
        Mix(hash, slot.offset);
    }

    return hash;
}

bool FunctionSignatureMatches(const FunctionSchema& schema, const RuntimeFunctionOps& ops,
                              const FrameLayout& expected) noexcept {
    if ((schema.flags & static_cast<uint32>(FunctionFlags::ReturnsResult)) != 0 ||
        schema.result.value.kind != ValueKind::Opaque) {
        return false;
    }

    if (schema.parameters.Size() != expected.count || ops.parameters.Size() != expected.count ||
        schema.frameSize != expected.size || schema.frameAlignment != expected.alignment) {
        return false;
    }

    for (uint32 index = 0; index < expected.count; ++index) {
        if (!detail::SlotDescriptionMatches(schema.parameters[index], ops.parameters[index].value,
                                            expected.slots[index]) ||
            schema.parameters[index].offset != expected.slots[index].offset) {
            return false;
        }
    }

    return true;
}

Result<FunctionBinding> FunctionBinding::CreateWithLayout(ObjectRegistry& registry,
                                                          const ObjectAccessContext& access,
                                                          ObjectHandle target, Guid functionId,
                                                          const FrameLayout& expected) noexcept {
    auto type = registry.AcquireType(access, target);
    if (!type) {
        return Result<FunctionBinding>::Failure(type.Error());
    }

    auto function = registry.Types().FindFunction(type.Value().Id(), functionId);
    if (!function) {
        return Result<FunctionBinding>::Failure(function.Error());
    }

    const FunctionSchema& schema = function.Value().Schema();
    if ((schema.flags & static_cast<uint32>(FunctionFlags::Static)) != 0) {
        return Result<FunctionBinding>::Failure(ErrorCode::InvalidArgument);
    }

    if (!FunctionSignatureMatches(schema, function.Value().Ops(), expected)) {
        return Result<FunctionBinding>::Failure(ErrorCode::TypeMismatch);
    }

    // 연산표는 대상 객체가 살아 있는 동안 유효하다(살아 있는 slot이 타입 lease를 쥐고, 파생
    // 레코드가 기반 레코드를 의존으로 묶는다 — ED3). 함수 handle의 lease는 여기서 놓는다(바인딩이
    // 모듈 reload·Unregister를 막지 않게).
    FunctionBinding binding;
    binding.target_ = target;
    binding.functionId_ = functionId;
    binding.layoutDigest_ = expected.digest;
    binding.ops_ = &function.Value().Ops();
    return Result<FunctionBinding>::Success(binding);
}

Result<void> FunctionBinding::InvokeWithDigest(ObjectRegistry& registry,
                                               const ObjectAccessContext& access, void* frame,
                                               uint64 digest) const noexcept {
    if (ops_ == nullptr || frame == nullptr || digest != layoutDigest_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 대상 slot이 살아 있어야 캐시한 연산표를 부를 수 있다(사라졌으면 그 타입이 풀렸을 수 있다).
    if (auto alive = registry.InspectLifecycle(access, target_); !alive) {
        return Result<void>::Failure(alive.Error());
    }

    return ops_->invoke(registry, access, target_, frame);
}

} // namespace rocks
