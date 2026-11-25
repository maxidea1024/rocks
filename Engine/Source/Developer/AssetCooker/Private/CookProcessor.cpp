#include <AssetCooker/CookProcessor.h>
#include <AssetRuntime/PackageFormat.h>

namespace rocks {

Result<void> CookProcessor::Cook(CookContext& context) const noexcept {
    return WriteCookedPackage(context, 0);
}

Result<void> CookProcessor::WriteCookedPackage(CookContext& context,
                                               uint32 minimumBulkAlignment) noexcept {
    const TargetProfile& profile = *context.profile;
    PackageWriteOptions options;
    options.kind = PackageKind::Cooked;
    options.profileId = profile.id.View();
    options.profileSettings = context.profileSettings;
    options.cookerVersion = context.cookerVersion;
    options.bulkCodec =
        profile.bulkCodec == CookBulkCodec::Zstd ? BulkCodec::Zstd : BulkCodec::None;
    const uint32 profileAlignment =
        profile.bulkAlignment == 0 ? PackageDefaultCookedBulkAlignment : profile.bulkAlignment;
    options.bulkAlignment =
        profileAlignment < minimumBulkAlignment ? minimumBulkAlignment : profileAlignment;
    options.bulkSource = context.bulkSource;
    auto written = WritePackage(*context.allocator, *context.registry, context.access,
                                context.package, options, *context.sink, context.failure);
    return written ? Result<void>::Success() : Result<void>::Failure(written.Error());
}

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
