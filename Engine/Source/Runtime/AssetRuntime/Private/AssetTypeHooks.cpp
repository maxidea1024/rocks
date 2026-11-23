#include <AssetRuntime/AssetTypeHooks.h>
#include <Core/Algorithms.h>
#include <Object/TypeRegistry.h>
#include <Platform/Fatal.h>

// 에셋 타입 hook 표 구현이다(Docs/Design/AssetSystem 7.1절 15 — A2c1).
namespace rocks {

Result<void> AssetTagSink::Add(StringView key, StringView value) noexcept {
    // 형식 검사는 WritePackageTags와 같은 규칙이다(한 쌍을 써 보아 판정한다 — 규칙을 한 곳에 둔다).
    const PackageTagInput one[] = {{key, value}};
    if (auto written = WritePackageTags(tags_.GetAllocator(), one); !written) {
        return Result<void>::Failure(written.Error());
    }

    // (키, 값) 순으로 둔다 — 같은 쌍 찾기·삽입 자리가 이분 찾기다.
    auto before = [](const PackageTag& tag, const PackageTagInput& input) noexcept {
        const int compared = StringView::CompareBytes(tag.key.View(), input.key);
        return compared != 0 ? compared < 0
                             : StringView::CompareBytes(tag.value.View(), input.value) < 0;
    };
    const PackageTagInput input{key, value};
    const usize at = algo::LowerBound(tags_.View(), input, before);
    if (at < tags_.Size() && tags_[at].key.View() == key && tags_[at].value.View() == value) {
        return Result<void>::Success();
    }

    PackageTag tag(tags_.GetAllocator());
    if (auto assigned = tag.key.TryAssign(key); !assigned) {
        return assigned;
    }

    if (auto assigned = tag.value.TryAssign(value); !assigned) {
        return assigned;
    }

    return tags_.TryInsert(Move(tag), at);
}

Result<Array<uint8>> AssetTagSink::Write(Allocator& allocator) const noexcept {
    Array<PackageTagInput> inputs(allocator);
    if (auto reserved = inputs.TryReserve(tags_.Size()); !reserved) {
        return Result<Array<uint8>>::Failure(reserved.Error());
    }

    for (const PackageTag& tag : tags_) {
        (void)inputs.TryPushBack(PackageTagInput{tag.key.View(), tag.value.View()});
    }

    return WritePackageTags(allocator, inputs.View());
}

Result<void> AssetDependencySink::Add(AssetId asset) noexcept {
    if (asset.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    usize at = 0;
    while (at < dependencies_.Size() && GuidLess(dependencies_[at].asset.value, asset.value)) {
        ++at;
    }

    if (at < dependencies_.Size() && dependencies_[at].asset == asset) {
        return Result<void>::Success();
    }

    PackageDependencyEntry entry;
    entry.asset = asset;
    entry.kind = PackageDependencyKind::Build;
    return dependencies_.TryInsert(entry, at);
}

Result<void> AssetTypeHookContext::CallBaseTags(AssetTagSink& sink) const noexcept {
    auto entry = table.Find(hookType, true, true);
    if (!entry) {
        return Result<void>::Failure(entry.Error());
    }

    if (entry.Value() == nullptr) {
        return Result<void>::Success();
    }

    const AssetTypeHookContext next{registry, access, object, entry.Value()->type,
                                    table,    types,  asset};
    return entry.Value()->hooks.tags(next, sink);
}

Result<void>
AssetTypeHookContext::CallBaseBuildDependencies(AssetDependencySink& sink) const noexcept {
    auto entry = table.Find(hookType, false, true);
    if (!entry) {
        return Result<void>::Failure(entry.Error());
    }

    if (entry.Value() == nullptr) {
        return Result<void>::Success();
    }

    const AssetTypeHookContext next{registry, access, object, entry.Value()->type,
                                    table,    types,  asset};
    return entry.Value()->hooks.buildDependencies(next, sink);
}

AssetTypeHookRegistration::AssetTypeHookRegistration(AssetTypeHookRegistration&& other) noexcept
    : table_(other.table_), type(other.type) {
    other.table_ = nullptr;
}

AssetTypeHookRegistration&
AssetTypeHookRegistration::operator=(AssetTypeHookRegistration&& other) noexcept {
    if (this != &other) {
        Release();
        table_ = other.table_;
        type = other.type;
        other.table_ = nullptr;
    }

    return *this;
}

AssetTypeHookRegistration::~AssetTypeHookRegistration() {
    Release();
}

void AssetTypeHookRegistration::Release() noexcept {
    if (table_ != nullptr) {
        table_->Remove(type);
        table_ = nullptr;
    }
}

AssetTypeHookTable::AssetTypeHookTable(Allocator& allocator, TypeRegistry& types) noexcept
    : types_(&types), entries_(allocator) {
}

AssetTypeHookTable::~AssetTypeHookTable() {
    // 남은 등록 handle의 Release가 해제된 표를 쓰게 된다(수명 계약 위반).
    if (!entries_.IsEmpty()) {
        platform::Fatal("AssetTypeHookTable destroyed with live registrations");
    }
}

Result<AssetTypeHookRegistration> AssetTypeHookTable::Register(TypeId type,
                                                               AssetTypeHooks hooks) noexcept {
    using Out = AssetTypeHookRegistration;
    if (type == TypeId{} || (hooks.tags == nullptr && hooks.buildDependencies == nullptr)) {
        return Result<Out>::Failure(ErrorCode::InvalidArgument);
    }

    for (const Entry& entry : entries_) {
        if (entry.type == type) {
            return Result<Out>::Failure(ErrorCode::AlreadyExists);
        }
    }

    if (auto pushed = entries_.TryPushBack(Entry{type, hooks}); !pushed) {
        return Result<Out>::Failure(pushed.Error());
    }

    AssetTypeHookRegistration registration;
    registration.table_ = this;
    registration.type = type;
    return Result<Out>::Success(Move(registration));
}

void AssetTypeHookTable::Remove(TypeId type) noexcept {
    for (usize index = 0; index < entries_.Size(); ++index) {
        if (entries_[index].type == type) {
            (void)entries_.TryRemoveAt(index);
            return;
        }
    }
}

Result<const AssetTypeHookTable::Entry*> AssetTypeHookTable::Find(TypeId start, bool tags,
                                                                  bool skipStart) const noexcept {
    using Out = const Entry*;
    TypeId current = start;
    bool skip = skipStart;
    while (current != TypeId{}) {
        for (const Entry& entry : entries_) {
            if (!skip && entry.type == current &&
                (tags ? entry.hooks.tags != nullptr : entry.hooks.buildDependencies != nullptr)) {
                return Result<Out>::Success(&entry);
            }
        }

        skip = false;
        auto lease = types_->Acquire(current);
        if (!lease) {
            return Result<Out>::Failure(lease.Error());
        }

        current = lease.Value().Schema().baseType;
    }

    return Result<Out>::Success(nullptr);
}

Result<void> AssetTypeHookTable::CollectTags(ObjectRegistry& registry,
                                             const ObjectAccessContext& access, ObjectHandle object,
                                             TypeId type, AssetId asset,
                                             AssetTagSink& sink) const noexcept {
    auto entry = Find(type, true, false);
    if (!entry) {
        return Result<void>::Failure(entry.Error());
    }

    if (entry.Value() == nullptr) {
        return Result<void>::Success();
    }

    const AssetTypeHookContext context{registry, access,  object, entry.Value()->type,
                                       *this,    *types_, asset};
    return entry.Value()->hooks.tags(context, sink);
}

Result<void> AssetTypeHookTable::CollectBuildDependencies(
    ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object, TypeId type,
    AssetId asset, AssetDependencySink& sink) const noexcept {
    auto entry = Find(type, false, false);
    if (!entry) {
        return Result<void>::Failure(entry.Error());
    }

    if (entry.Value() == nullptr) {
        return Result<void>::Success();
    }

    const AssetTypeHookContext context{registry, access,  object, entry.Value()->type,
                                       *this,    *types_, asset};
    return entry.Value()->hooks.buildDependencies(context, sink);
}

} // namespace rocks
