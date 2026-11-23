#include <AssetCooker/CookProcessor.h>

namespace rocks {

CookProcessorTable::CookProcessorTable(Allocator& allocator, TypeRegistry& types,
                                       const CookProcessor& fallback) noexcept
    : types_(&types), fallback_(&fallback), entries_(allocator) {
}

Result<void> CookProcessorTable::Register(TypeId type, const CookProcessor& processor) noexcept {
    if (type == TypeId{}) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (const Entry& entry : entries_) {
        if (entry.type == type) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }

    return entries_.TryPushBack(Entry{type, &processor});
}

Result<const CookProcessor*> CookProcessorTable::Find(TypeId type) const noexcept {
    using Out = const CookProcessor*;
    if (type == TypeId{}) {
        return Result<Out>::Failure(ErrorCode::InvalidArgument);
    }

    // AssetTypeHookTable::Find와 같은 규칙이다(가장 가까운 base부터).
    TypeId current = type;
    while (current != TypeId{}) {
        for (const Entry& entry : entries_) {
            if (entry.type == current) {
                return Result<Out>::Success(entry.processor);
            }
        }

        auto lease = types_->Acquire(current);
        if (!lease) {
            return Result<Out>::Failure(lease.Error());
        }

        current = lease.Value().Schema().baseType;
    }

    return Result<Out>::Success(fallback_);
}

} // namespace rocks
