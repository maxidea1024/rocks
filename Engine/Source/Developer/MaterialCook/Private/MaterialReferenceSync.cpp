#include <Base/Compiler.h>
#include <Material/Material.generated.h>
#include <Material/Material.h>
#include <Material/ParameterCollection.generated.h>
#include <Texture/Texture.generated.h>
#include <MaterialCook/MaterialReferenceSync.h>

// 참조 동기화다(Docs/Design/MaterialSystem 12.8절 MS3c).
namespace rocks {

namespace {

using material::MaterialAssetReferenceRole;

bool IsTextureRole(MaterialAssetReferenceRole role) noexcept {
    return role == MaterialAssetReferenceRole::Texture ||
           role == MaterialAssetReferenceRole::CurveAtlas;
}

// sources에서 역할이 맞는 열린 참조의 handle을 out에 모은다((역할, AssetId) 순 — sources 순서, 같은
// handle은 한 번).
Result<void> HandlesOf(const MaterialCookSources& sources, bool textures,
                       Array<ObjectHandle>& out) noexcept {
    for (const MaterialCookReference& reference : sources.References()) {
        const bool wanted = textures ? IsTextureRole(reference.role)
                                     : reference.role == MaterialAssetReferenceRole::Collection;
        if (!wanted || reference.object.IsNull()) {
            continue;
        }

        bool seen = false;
        for (ObjectHandle existing : out) {
            seen = seen || existing == reference.object;
        }

        if (!seen) {
            if (auto pushed = out.TryPushBack(reference.object); !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

template <typename T>
bool SameHandles(const ObjectRefArray<T>& field, const Array<ObjectHandle>& wanted) noexcept {
    if (field.Size() != wanted.Size()) {
        return false;
    }

    for (usize index = 0; index < wanted.Size(); ++index) {
        if (!(field[index] == wanted[index])) {
            return false;
        }
    }

    return true;
}

template <typename T>
bool ContainsAll(const ObjectRefArray<T>& field, const Array<ObjectHandle>& wanted,
                 ObjectHandle& missing) noexcept {
    for (ObjectHandle handle : wanted) {
        bool found = false;
        for (usize index = 0; index < field.Size(); ++index) {
            found = found || field[index] == handle;
        }

        if (!found) {
            missing = handle;
            return false;
        }
    }

    return true;
}

template <typename T>
Result<void> Replace(const MaterialCookGatherContext& context, ObjectHandle material,
                     ObjectRefArray<T>& field, const Array<ObjectHandle>& wanted) noexcept {
    if (auto cleared = context.registry->ClearReferences(context.access, material, field);
        !cleared) {
        return cleared;
    }

    for (ObjectHandle handle : wanted) {
        if (auto added = context.registry->AddReference(context.access, material, field, handle);
            !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> SyncMaterialReferences(const MaterialCookGatherContext& context, ObjectHandle material,
                                    bool& changed) noexcept {
    changed = false;
#if ROCKS_WITH_EDITOR_DATA
    if (context.registry == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = context.registry->ObjectAllocator();
    MaterialCookSources sources(allocator);
    {
        auto borrow = context.registry->Resolve<Material>(context.access, material);
        auto pointer = borrow ? borrow.Value().Get() : Result<Material*>::Failure(borrow.Error());
        if (!pointer) {
            return Result<void>::Failure(pointer.Error());
        }

        if (auto gathered = sources.Gather(context, pointer.Value()->graph); !gathered) {
            return gathered;
        }
    }

    Array<ObjectHandle> textures(allocator);
    Array<ObjectHandle> collections(allocator);
    if (auto collected = HandlesOf(sources, true, textures); !collected) {
        return collected;
    }

    if (auto collected = HandlesOf(sources, false, collections); !collected) {
        return collected;
    }

    auto borrow = context.registry->Resolve<Material>(context.access, material);
    auto pointer = borrow ? borrow.Value().Get() : Result<Material*>::Failure(borrow.Error());
    if (!pointer) {
        return Result<void>::Failure(pointer.Error());
    }

    Material& value = *pointer.Value();
    if (!SameHandles(value.referencedTextures, textures)) {
        if (auto replaced = Replace(context, material, value.referencedTextures, textures);
            !replaced) {
            return replaced;
        }

        changed = true;
    }

    if (!SameHandles(value.parameterCollections, collections)) {
        if (auto replaced = Replace(context, material, value.parameterCollections, collections);
            !replaced) {
            return replaced;
        }

        changed = true;
    }

    return Result<void>::Success();
#else
    (void)context;
    (void)material;
    return Result<void>::Failure(ErrorCode::Unsupported);
#endif
}

Result<bool> MaterialReferencesInSync(const MaterialCookGatherContext& context,
                                      ObjectHandle material, const MaterialCookSources& sources,
                                      AssetId& missing) noexcept {
    Allocator& allocator = context.registry->ObjectAllocator();
    Array<ObjectHandle> textures(allocator);
    Array<ObjectHandle> collections(allocator);
    if (auto collected = HandlesOf(sources, true, textures); !collected) {
        return Result<bool>::Failure(collected.Error());
    }

    if (auto collected = HandlesOf(sources, false, collections); !collected) {
        return Result<bool>::Failure(collected.Error());
    }

    auto borrow = context.registry->Resolve<Material>(context.access, material);
    auto pointer = borrow ? borrow.Value().Get() : Result<Material*>::Failure(borrow.Error());
    if (!pointer) {
        return Result<bool>::Failure(pointer.Error());
    }

    ObjectHandle absent{};
    if (ContainsAll(pointer.Value()->referencedTextures, textures, absent) &&
        ContainsAll(pointer.Value()->parameterCollections, collections, absent)) {
        return Result<bool>::Success(true);
    }

    for (const MaterialCookReference& reference : sources.References()) {
        if (reference.object == absent) {
            missing = reference.asset;
            break;
        }
    }

    return Result<bool>::Success(false);
}

} // namespace rocks
