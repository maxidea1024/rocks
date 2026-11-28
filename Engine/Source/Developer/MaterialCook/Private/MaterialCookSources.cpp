#include <Base/Compiler.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/Package.h>
#include <Core/Move.h>
#include <Core/Text/String.h>
#include <CurveAssets/ColorCurveAsset.generated.h>
#include <CurveAssets/ColorCurveAsset.h>
#include <CurveAtlas/ColorCurveAtlas.generated.h>
#include <CurveAtlas/ColorCurveAtlas.h>
#include <Material/Material.generated.h>
#include <Material/Material.h>
#include <Material/MaterialFunction.generated.h>
#include <Material/MaterialFunctionBase.generated.h>
#include <Material/MaterialFunction.h>
#include <Material/MaterialFunctionInstance.generated.h>
#include <Material/MaterialFunctionInstance.h>
#include <Material/MaterialGraphReferences.h>
#include <Material/ParameterCollection.generated.h>
#include <Material/ParameterCollection.h>
#include <MaterialCompiler/MaterialColorCurveSnapshot.h>
#include <MaterialCompiler/MaterialCurveAtlasSnapshot.h>
#include <MaterialCompiler/MaterialGraphSnapshot.h>
#include <MaterialCook/MaterialCookSources.h>
#include <NodeGraph/GraphClone.h>
#include <Texture/Texture.generated.h>
#include <Texture/Texture.h>

// 번역 입력 모으기다(Docs/Design/MaterialSystem 12.8절 MS3c).
namespace rocks {

using material::MaterialAssetReferenceRole;

// 소유한 그래프 snapshot이다(함수·머티리얼).
struct MaterialCookSources::OwnedGraph {
    explicit OwnedGraph(Allocator& allocator) noexcept : name(allocator), document(allocator) {
    }

    bool material = false;
    String name;
    nodegraph::GraphDocument document;
    material::MaterialGraphSnapshot snapshot;
};

// 소유한 색 커브다.
struct MaterialCookSources::OwnedCurve {
    explicit OwnedCurve(Allocator& allocator) noexcept
        : r(allocator), g(allocator), b(allocator), a(allocator) {
    }

    Guid asset;
    TangentCurve r;
    TangentCurve g;
    TangentCurve b;
    TangentCurve a;
    material::MaterialColorCurveSnapshot snapshot;
};

// 소유한 atlas 행 목록이다.
struct MaterialCookSources::OwnedAtlas {
    explicit OwnedAtlas(Allocator& allocator) noexcept : rows(allocator) {
    }

    Guid asset;
    Array<Guid> rows;
    material::MaterialCurveAtlasSnapshot snapshot;
};

// 소유한 컬렉션 layout 항목이다.
struct MaterialCookSources::OwnedCollection {
    explicit OwnedCollection(Allocator& allocator) noexcept : entries(allocator) {
    }

    Guid asset;
    Array<material::MaterialCollectionEntry> entries;
    uint32 vectors = 0;
};

namespace {

// float4 하나에 묶는 scalar 수다(UE DivideAndRoundUp(ScalarParameters.Num(), 4)).
constexpr uint32 ScalarsPerVector = 4;
// 컬렉션 base 사슬 깊이 상한이다(순환 방어 — 정상 사슬은 몇 단계다).
constexpr uint32 MaxCollectionChain = 64;

bool ReferenceLess(const MaterialCookReference& left, const MaterialCookReference& right) noexcept {
    if (left.role != right.role) {
        return static_cast<uint8>(left.role) < static_cast<uint8>(right.role);
    }

    return GuidLess(left.asset.value, right.asset.value);
}

// 대표 에셋 handle의 패키지 AssetId다(null이면 null). 오류: Registry 조회 오류.
Result<AssetId> AssetOfObject(const MaterialCookGatherContext& context,
                              ObjectHandle object) noexcept {
    if (object.IsNull()) {
        return Result<AssetId>::Success(AssetId{});
    }

    auto outermost = context.registry->GetOutermost(context.access, object);
    if (!outermost) {
        return Result<AssetId>::Failure(outermost.Error());
    }

    auto package = context.registry->Resolve<Package>(context.access, outermost.Value());
    if (!package) {
        return Result<AssetId>::Failure(package.Error());
    }

    auto pointer = package.Value().Get();
    if (!pointer) {
        return Result<AssetId>::Failure(pointer.Error());
    }

    return Result<AssetId>::Success(pointer.Value()->Id());
}

// 텍스처 종류다(UE 텍스처 클래스 — 2D 파생(atlas)도 2D).
bool TextureKindOf(const TypeLease& type, material::IrTextureKind& out) noexcept {
    if (type.IsA(TypeIdOf<Texture2D>())) {
        out = material::IrTextureKind::Texture2D;
    } else if (type.IsA(TypeIdOf<TextureCube>())) {
        out = material::IrTextureKind::TextureCube;
    } else if (type.IsA(TypeIdOf<Texture2DArray>())) {
        out = material::IrTextureKind::Texture2DArray;
    } else if (type.IsA(TypeIdOf<TextureCubeArray>())) {
        out = material::IrTextureKind::TextureCubeArray;
    } else if (type.IsA(TypeIdOf<VolumeTexture>())) {
        out = material::IrTextureKind::Texture3D;
    } else {
        return false;
    }

    return true;
}

} // namespace

MaterialCookSources::MaterialCookSources(Allocator& allocator) noexcept
    : allocator_(&allocator), graphs_(allocator), curves_(allocator), atlases_(allocator),
      ownedCollections_(allocator), textures_(allocator), collections_(allocator),
      references_(allocator) {
}

MaterialCookSources::~MaterialCookSources() = default;

void MaterialCookSources::Clear() noexcept {
    graphs_.Clear();
    curves_.Clear();
    atlases_.Clear();
    ownedCollections_.Clear();
    textures_.Clear();
    collections_.Clear();
    references_.Clear();
}

Result<void> MaterialCookSources::Gather(const MaterialCookGatherContext& context,
                                         const nodegraph::GraphDocument& root) noexcept {
    if (context.registry == nullptr || context.registry->Names() == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Clear();
    Array<MaterialCookReference> pending(*allocator_);
    {
        Array<MaterialGraphReference> found(*allocator_);
        if (auto collected =
                CollectMaterialGraphReferences(*context.registry->Names(), root, found);
            !collected) {
            return collected;
        }

        for (const MaterialGraphReference& item : found) {
            if (auto pushed = pending.TryPushBack(MaterialCookReference{item.role, item.asset, {}});
                !pushed) {
                return pushed;
            }
        }
    }

    // 닫힘을 연다(깊이 우선 — 같은 (역할, AssetId)는 한 번).
    while (!pending.IsEmpty()) {
        auto popped = pending.TryPopBack();
        if (!popped) {
            return Result<void>::Failure(popped.Error());
        }

        MaterialCookReference reference = popped.Value();
        bool seen = false;
        for (const MaterialCookReference& existing : references_) {
            seen = seen || (existing.role == reference.role && existing.asset == reference.asset);
        }

        if (seen) {
            continue;
        }

        if (auto visited = Visit(context, reference, pending); !visited) {
            return visited;
        }

        if (auto pushed = references_.TryPushBack(reference); !pushed) {
            return pushed;
        }
    }

    // 결정적 순서다(번역 키·참조 동기화가 쓴다).
    for (usize index = 1; index < references_.Size(); ++index) {
        for (usize at = index; at > 0 && ReferenceLess(references_[at], references_[at - 1]);
             --at) {
            const MaterialCookReference swap = references_[at];
            references_[at] = references_[at - 1];
            references_[at - 1] = swap;
        }
    }

    for (usize index = 1; index < textures_.Size(); ++index) {
        for (usize at = index; at > 0 && GuidLess(textures_[at].asset, textures_[at - 1].asset);
             --at) {
            const material::MaterialTextureFacts swap = textures_[at];
            textures_[at] = textures_[at - 1];
            textures_[at - 1] = swap;
        }
    }

    for (usize index = 1; index < ownedCollections_.Size(); ++index) {
        for (usize at = index;
             at > 0 && GuidLess(ownedCollections_[at]->asset, ownedCollections_[at - 1]->asset);
             --at) {
            UniquePtr<OwnedCollection> swap = Move(ownedCollections_[at]);
            ownedCollections_[at] = Move(ownedCollections_[at - 1]);
            ownedCollections_[at - 1] = Move(swap);
        }
    }

    for (const UniquePtr<OwnedCollection>& owned : ownedCollections_) {
        if (auto pushed = collections_.TryPushBack(material::MaterialCollectionLayout{
                owned->asset, owned->entries.View(), owned->vectors});
            !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

Result<void> MaterialCookSources::Visit(const MaterialCookGatherContext& context,
                                        MaterialCookReference& reference,
                                        Array<MaterialCookReference>& pending) noexcept {
    if (context.opener == nullptr) {
        return Result<void>::Success();
    }

    AssetDiagnostic failure;
    auto package = context.opener->Open(reference.asset, &failure);
    if (!package) {
        // 열지 못한 참조는 없는 에셋이다(번역이 진단한다).
        return package.Error() == ErrorCode::OutOfMemory ? Result<void>::Failure(package.Error())
                                                         : Result<void>::Success();
    }

    auto object = context.registry->FindChild(context.access, package.Value(), "");
    if (!object) {
        return object.Error() == ErrorCode::OutOfMemory ? Result<void>::Failure(object.Error())
                                                        : Result<void>::Success();
    }

    auto type = context.registry->AcquireType(context.access, object.Value());
    if (!type) {
        return Result<void>::Failure(type.Error());
    }

    const Guid asset = reference.asset.value;
    switch (reference.role) {
    case MaterialAssetReferenceRole::Function:
        if (!type.Value().IsA(TypeIdOf<MaterialFunctionBase>())) {
            return Result<void>::Success();
        }

        reference.object = object.Value();
        return AddGraph(context, asset, object.Value(), false, pending);
    case MaterialAssetReferenceRole::Material:
        if (!type.Value().IsA(TypeIdOf<Material>())) {
            return Result<void>::Success();
        }

        reference.object = object.Value();
        return AddGraph(context, asset, object.Value(), true, pending);
    case MaterialAssetReferenceRole::ColorCurve:
        if (!type.Value().IsA(TypeIdOf<ColorCurveAsset>())) {
            return Result<void>::Success();
        }

        reference.object = object.Value();
        return AddCurve(context, asset, object.Value());
    case MaterialAssetReferenceRole::CurveAtlas:
        if (!type.Value().IsA(TypeIdOf<ColorCurveAtlas>())) {
            return Result<void>::Success();
        }

        reference.object = object.Value();
        return AddAtlas(context, asset, object.Value());
    case MaterialAssetReferenceRole::Texture:
        if (!type.Value().IsA(TypeIdOf<Texture>())) {
            return Result<void>::Success();
        }

        reference.object = object.Value();
        return AddTexture(context, asset, object.Value());
    case MaterialAssetReferenceRole::Collection:
        if (!type.Value().IsA(TypeIdOf<ParameterCollection>())) {
            return Result<void>::Success();
        }

        reference.object = object.Value();
        return AddCollection(context, asset, object.Value());
    }

    return Result<void>::Success();
}

Result<void> MaterialCookSources::AddGraph(const MaterialCookGatherContext& context, Guid asset,
                                           ObjectHandle object, bool material,
                                           Array<MaterialCookReference>& pending) noexcept {
    ObjectRegistry& registry = *context.registry;
    // 함수 인스턴스는 기반 함수 그래프다(override 적용은 MS6 — 6.12절 MS2e2).
    ObjectHandle source = object;
    for (uint32 depth = 0; !material && depth < MaxCollectionChain; ++depth) {
        auto type = registry.AcquireType(context.access, source);
        if (!type) {
            return Result<void>::Failure(type.Error());
        }

        if (!type.Value().IsA(TypeIdOf<MaterialFunctionInstance>())) {
            break;
        }

#if ROCKS_WITH_EDITOR_DATA
        auto instance = registry.Resolve<MaterialFunctionInstance>(context.access, source);
        if (!instance) {
            return Result<void>::Failure(instance.Error());
        }

        auto pointer = instance.Value().Get();
        if (!pointer) {
            return Result<void>::Failure(pointer.Error());
        }

        source = pointer.Value()->parent.Handle();
#else
        source = ObjectHandle{};
#endif
        if (source.IsNull()) {
            return Result<void>::Success();
        }
    }

    auto owned = UniquePtr<OwnedGraph>::TryMake(*allocator_, *allocator_);
    if (!owned) {
        return Result<void>::Failure(owned.Error());
    }

    OwnedGraph& graph = *owned.Value();
    graph.material = material;
    material::MaterialFunctionUsage usage = material::MaterialFunctionUsage::General;
#if ROCKS_WITH_EDITOR_DATA
    if (material) {
        auto borrow = registry.Resolve<Material>(context.access, source);
        auto pointer = borrow ? borrow.Value().Get() : Result<Material*>::Failure(borrow.Error());
        if (!pointer) {
            return Result<void>::Failure(pointer.Error());
        }

        if (auto cloned = nodegraph::CloneGraphDocument(pointer.Value()->graph, graph.document);
            !cloned) {
            return cloned;
        }
    } else {
        auto borrow = registry.Resolve<MaterialFunction>(context.access, source);
        auto pointer =
            borrow ? borrow.Value().Get() : Result<MaterialFunction*>::Failure(borrow.Error());
        if (!pointer) {
            // 기반이 MaterialFunction이 아니면(다른 파생) 공급하지 않는다.
            return pointer.Error() == ErrorCode::OutOfMemory
                       ? Result<void>::Failure(pointer.Error())
                       : Result<void>::Success();
        }

        if (auto cloned = nodegraph::CloneGraphDocument(pointer.Value()->graph, graph.document);
            !cloned) {
            return cloned;
        }

        usage = pointer.Value()->usage;
    }
#endif

    // 진단 접두 이름은 패키지 이름의 마지막 조각이다(UE GetName — 에셋 이름).
    auto package = registry.GetOutermost(context.access, object);
    if (package) {
        String full(*allocator_);
        if (registry.GetName(context.access, package.Value(), full)) {
            const StringView text = full.View();
            usize start = 0;
            for (usize at = 0; at < text.ByteCount(); ++at) {
                start = text.Data()[at] == '/' ? at + 1 : start;
            }

            if (auto assigned =
                    graph.name.TryAssign(StringView(text.Data() + start, text.ByteCount() - start));
                !assigned) {
                return assigned;
            }
        }
    }

    graph.snapshot =
        material::MaterialGraphSnapshot{asset, graph.name.View(), &graph.document, usage};
    Array<MaterialGraphReference> found(*allocator_);
    if (auto collected = CollectMaterialGraphReferences(*registry.Names(), graph.document, found);
        !collected) {
        return collected;
    }

    for (const MaterialGraphReference& item : found) {
        if (auto pushed = pending.TryPushBack(MaterialCookReference{item.role, item.asset, {}});
            !pushed) {
            return pushed;
        }
    }

    return graphs_.TryPushBack(Move(owned.Value()));
}

Result<void> MaterialCookSources::AddCurve(const MaterialCookGatherContext& context, Guid asset,
                                           ObjectHandle object) noexcept {
    auto borrow = context.registry->Resolve<ColorCurveAsset>(context.access, object);
    auto pointer =
        borrow ? borrow.Value().Get() : Result<ColorCurveAsset*>::Failure(borrow.Error());
    if (!pointer) {
        return Result<void>::Failure(pointer.Error());
    }

    auto owned = UniquePtr<OwnedCurve>::TryMake(*allocator_, *allocator_);
    if (!owned) {
        return Result<void>::Failure(owned.Error());
    }

    OwnedCurve& curve = *owned.Value();
    const ColorCurveAsset& source = *pointer.Value();
    curve.asset = asset;
    if (auto copied = curve.r.TryCopyFrom(source.r); !copied) {
        return copied;
    }

    if (auto copied = curve.g.TryCopyFrom(source.g); !copied) {
        return copied;
    }

    if (auto copied = curve.b.TryCopyFrom(source.b); !copied) {
        return copied;
    }

    if (auto copied = curve.a.TryCopyFrom(source.a); !copied) {
        return copied;
    }

    curve.snapshot = material::MaterialColorCurveSnapshot{&curve.r, &curve.g, &curve.b, &curve.a,
                                                          source.Adjustments()};
    return curves_.TryPushBack(Move(owned.Value()));
}

Result<void> MaterialCookSources::AddAtlas(const MaterialCookGatherContext& context, Guid asset,
                                           ObjectHandle object) noexcept {
    auto owned = UniquePtr<OwnedAtlas>::TryMake(*allocator_, *allocator_);
    if (!owned) {
        return Result<void>::Failure(owned.Error());
    }

    OwnedAtlas& atlas = *owned.Value();
    atlas.asset = asset;
    {
        auto borrow = context.registry->Resolve<ColorCurveAtlas>(context.access, object);
        auto pointer =
            borrow ? borrow.Value().Get() : Result<ColorCurveAtlas*>::Failure(borrow.Error());
        if (!pointer) {
            return Result<void>::Failure(pointer.Error());
        }

        Array<ObjectHandle> handles(*allocator_);
        for (usize index = 0; index < pointer.Value()->curves.Size(); ++index) {
            if (auto pushed = handles.TryPushBack(pointer.Value()->curves[index]); !pushed) {
                return pushed;
            }
        }

        // 행마다 커브 AssetId다(null 원소는 Guid{} — ColorCurveAtlas::RowOf와 같은 순서).
        for (ObjectHandle handle : handles) {
            auto id = AssetOfObject(context, handle);
            if (!id) {
                return Result<void>::Failure(id.Error());
            }

            if (auto pushed = atlas.rows.TryPushBack(id.Value().value); !pushed) {
                return pushed;
            }
        }
    }

    atlas.snapshot = material::MaterialCurveAtlasSnapshot{atlas.rows.View()};
    if (auto pushed = atlases_.TryPushBack(Move(owned.Value())); !pushed) {
        return pushed;
    }

    // atlas는 샘플하는 2D 텍스처이기도 하다(6.10절 MS2c5).
    return AddTexture(context, asset, object);
}

Result<void> MaterialCookSources::AddTexture(const MaterialCookGatherContext& context, Guid asset,
                                             ObjectHandle object) noexcept {
    for (const material::MaterialTextureFacts& existing : textures_) {
        if (existing.asset == asset) {
            return Result<void>::Success();
        }
    }

    ObjectRegistry& registry = *context.registry;
    auto type = registry.AcquireType(context.access, object);
    if (!type) {
        return Result<void>::Failure(type.Error());
    }

    material::MaterialTextureFacts facts;
    facts.asset = asset;
    if (!TextureKindOf(type.Value(), facts.kind)) {
        return Result<void>::Success();
    }

    auto borrow = registry.Resolve<Texture>(context.access, object);
    auto pointer = borrow ? borrow.Value().Get() : Result<Texture*>::Failure(borrow.Error());
    if (!pointer) {
        return Result<void>::Failure(pointer.Error());
    }

    const Texture& texture = *pointer.Value();
    facts.compression = texture.compressionSettings;
    facts.lodGroup = texture.lodGroup;
    facts.srgb = texture.srgb != 0;
    facts.virtualTexture = texture.virtualTextureStreaming != 0;
    // 주소 모드는 클래스마다 다르다(2D X·Y, 배열 X·Y·Z, 볼륨 한 값, 큐브는 기본 Wrap).
    if (facts.kind == material::IrTextureKind::Texture2D) {
        const Texture2D& flat = static_cast<const Texture2D&>(texture);
        facts.address[0] = flat.addressX;
        facts.address[1] = flat.addressY;
    } else if (facts.kind == material::IrTextureKind::Texture2DArray) {
        const Texture2DArray& layered = static_cast<const Texture2DArray&>(texture);
        facts.address[0] = layered.addressX;
        facts.address[1] = layered.addressY;
        facts.address[2] = layered.addressZ;
    } else if (facts.kind == material::IrTextureKind::Texture3D) {
        const VolumeTexture& volume = static_cast<const VolumeTexture&>(texture);
        facts.address[0] = volume.addressMode;
        facts.address[1] = volume.addressMode;
        facts.address[2] = volume.addressMode;
    }

    return textures_.TryPushBack(facts);
}

Result<void> MaterialCookSources::AddCollection(const MaterialCookGatherContext& context,
                                                Guid asset, ObjectHandle object) noexcept {
    // UE GetParameterIndex 배치: 가장 깊은 base부터 [scalar float4 묶음][vector]를 쌓고 자기 것이
    // 맨 끝이다(ParameterCollection.cpp:681–732).
    ObjectRegistry& registry = *context.registry;
    Array<ObjectHandle> chain(*allocator_);
    for (ObjectHandle current = object; !current.IsNull();) {
        if (chain.Size() == MaxCollectionChain) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto pushed = chain.TryPushBack(current); !pushed) {
            return pushed;
        }

        auto borrow = registry.Resolve<ParameterCollection>(context.access, current);
        auto pointer =
            borrow ? borrow.Value().Get() : Result<ParameterCollection*>::Failure(borrow.Error());
        if (!pointer) {
            return Result<void>::Failure(pointer.Error());
        }

        current = pointer.Value()->base.Handle();
    }

    auto owned = UniquePtr<OwnedCollection>::TryMake(*allocator_, *allocator_);
    if (!owned) {
        return Result<void>::Failure(owned.Error());
    }

    OwnedCollection& collection = *owned.Value();
    collection.asset = asset;
    uint32 vector = 0;
    for (usize level = chain.Size(); level > 0; --level) {
        auto borrow = registry.Resolve<ParameterCollection>(context.access, chain[level - 1]);
        auto pointer =
            borrow ? borrow.Value().Get() : Result<ParameterCollection*>::Failure(borrow.Error());
        if (!pointer) {
            return Result<void>::Failure(pointer.Error());
        }

        const ParameterCollection& item = *pointer.Value();
        for (usize index = 0; index < item.scalars.Size(); ++index) {
            const material::MaterialCollectionEntry entry{
                item.scalars[index].id, vector + static_cast<uint32>(index / ScalarsPerVector),
                static_cast<uint8>(index % ScalarsPerVector)};
            if (auto pushed = collection.entries.TryPushBack(entry); !pushed) {
                return pushed;
            }
        }

        vector +=
            static_cast<uint32>((item.scalars.Size() + ScalarsPerVector - 1) / ScalarsPerVector);
        for (usize index = 0; index < item.vectors.Size(); ++index) {
            const material::MaterialCollectionEntry entry{item.vectors[index].id,
                                                          vector + static_cast<uint32>(index),
                                                          material::MaterialCollectionWholeVector};
            if (auto pushed = collection.entries.TryPushBack(entry); !pushed) {
                return pushed;
            }
        }

        vector += static_cast<uint32>(item.vectors.Size());
    }

    collection.vectors = vector;
    return ownedCollections_.TryPushBack(Move(owned.Value()));
}

usize MaterialCookSources::GraphCount() const noexcept {
    return graphs_.Size();
}

const material::MaterialGraphSnapshot& MaterialCookSources::Graph(usize index) const noexcept {
    return graphs_[index]->snapshot;
}

const material::MaterialTextureFacts* MaterialCookSources::FindTexture(Guid asset) const noexcept {
    for (const material::MaterialTextureFacts& facts : textures_) {
        if (facts.asset == asset) {
            return &facts;
        }
    }

    return nullptr;
}

const material::MaterialCollectionLayout*
MaterialCookSources::FindCollection(Guid asset) const noexcept {
    for (const material::MaterialCollectionLayout& layout : collections_) {
        if (layout.asset == asset) {
            return &layout;
        }
    }

    return nullptr;
}

const material::MaterialGraphSnapshot*
MaterialCookSources::FindFunction(Guid asset) const noexcept {
    for (const UniquePtr<OwnedGraph>& graph : graphs_) {
        if (!graph->material && graph->snapshot.asset == asset) {
            return &graph->snapshot;
        }
    }

    return nullptr;
}

const material::MaterialGraphSnapshot*
MaterialCookSources::FindMaterial(Guid asset) const noexcept {
    for (const UniquePtr<OwnedGraph>& graph : graphs_) {
        if (graph->material && graph->snapshot.asset == asset) {
            return &graph->snapshot;
        }
    }

    return nullptr;
}

const material::MaterialColorCurveSnapshot*
MaterialCookSources::FindColorCurve(Guid asset) const noexcept {
    for (const UniquePtr<OwnedCurve>& curve : curves_) {
        if (curve->asset == asset) {
            return &curve->snapshot;
        }
    }

    return nullptr;
}

const material::MaterialCurveAtlasSnapshot*
MaterialCookSources::FindCurveAtlas(Guid asset) const noexcept {
    for (const UniquePtr<OwnedAtlas>& atlas : atlases_) {
        if (atlas->asset == asset) {
            return &atlas->snapshot;
        }
    }

    return nullptr;
}

} // namespace rocks
