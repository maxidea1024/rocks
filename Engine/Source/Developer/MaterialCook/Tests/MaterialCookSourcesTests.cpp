#include <AssetRuntime/Tests/AssetTestSupport.h>
#include <Base/Compiler.h>
#include <Core/Memory/SystemAllocator.h>
#include <CurveAssets/ColorCurveAsset.generated.h>
#include <CurveAssets/ColorCurveAsset.h>
#include <CurveAtlas/ColorCurveAtlas.generated.h>
#include <CurveAtlas/ColorCurveAtlas.h>
#include <CurveAtlas/CurveAtlasModule.h>
#include <Material/Material.generated.h>
#include <Material/Material.h>
#include <Material/MaterialFunction.generated.h>
#include <Material/MaterialFunction.h>
#include <Material/MaterialModule.h>
#include <Material/ParameterCollection.generated.h>
#include <Material/ParameterCollection.h>
#include <MaterialCook/MaterialCookSources.h>
#include <MaterialCook/MaterialReferenceSync.h>
#include <MaterialSchema/Nodes/Color/ColorRampProperties.generated.h>
#include <MaterialSchema/Nodes/Color/ColorRampProperties.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallProperties.h>
#include <MaterialSchema/Nodes/Parameters/CurveAtlasRowParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/CurveAtlasRowParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/ParameterCollectionValueProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/ParameterCollectionValueProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleProperties.h>
#include <ObjectSerialization/StructRecord.h>
#include <Texture/Texture.generated.h>
#include <Texture/Texture.h>

// 번역 입력 모으기·참조 동기화 시험이다(Docs/Design/MaterialSystem 12.8절 MS3c): 머티리얼 그래프 →
// 함수 그래프의 참조 닫힘을 의존 열기로 열어 함수 snapshot·색 커브·atlas 행·텍스처 사실(종류·주소
// 모드)·컬렉션 layout(UE base 배치)을 모으고, 참조가 (역할, AssetId) 순인지, 참조 동기화가 Hard
// 필드를 채우고 두 번째는 바꾸지 않는지, 대조가 빠진 텍스처를 찾는지 본다.
using namespace rocks;
namespace material_cook_sources_tests_file {

namespace {

constexpr uint64 MaterialIdValue = 0x9a0;
constexpr uint64 FunctionIdValue = 0x9a1;
constexpr uint64 BrickIdValue = 0x9a2;
constexpr uint64 SkyIdValue = 0x9a3;
constexpr uint64 CurveIdValue = 0x9a4;
constexpr uint64 AtlasIdValue = 0x9a5;
constexpr uint64 BaseCollectionIdValue = 0x9a6;
constexpr uint64 CollectionIdValue = 0x9a7;
// 자식 컬렉션 scalar 수다(float4 둘 — 4 + 1).
constexpr usize ChildScalars = 5;
// 큐브 텍스처의 패키지 없는 id다(열기가 실패한다 — 없는 에셋).
constexpr uint64 MissingIdValue = 0x9af;

// 시험용 의존 열기다(AssetId → 패키지 handle — 없으면 NotFound).
class TestOpener final : public CookDependencyOpener {
public:
    explicit TestOpener(Allocator& allocator) noexcept : ids_(allocator), packages_(allocator) {
    }

    bool Add(AssetId id, ObjectHandle package) noexcept {
        return ids_.TryPushBack(id).IsOk() && packages_.TryPushBack(package).IsOk();
    }

    Result<ObjectHandle> Open(AssetId id, AssetDiagnostic* failure) noexcept override {
        for (usize index = 0; index < ids_.Size(); ++index) {
            if (ids_[index] == id) {
                ++opens;
                return Result<ObjectHandle>::Success(packages_[index]);
            }
        }

        if (failure != nullptr) {
            failure->code = AssetDiagnosticCode::None;
        }

        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }

    usize opens = 0;

private:
    Array<AssetId> ids_;
    Array<ObjectHandle> packages_;
};

template <class T>
TypeId TypeOf() noexcept {
    return GeneratedType<T>::Id();
}

template <typename T>
ObjectHandle MakeAsset(asset_test::AssetEnv& env, uint64 id, RootRef<Package>& package,
                       TestOpener& opener) noexcept {
    ObjectRegistry& source = *env.source;
    auto made =
        NewPackage(source, source.OwnerAccess(), asset_test::FixedId(id), PackageKind::Editor);
    if (!made) {
        return ObjectHandle{};
    }

    package = Move(made.Value());
    if (!opener.Add(asset_test::FixedId(id), package.Handle())) {
        return ObjectHandle{};
    }

    return asset_test::Make<T>(source, TypeOf<T>(), package.Handle(), "");
}

template <typename Properties>
bool AddNode(asset_test::AssetEnv& env, nodegraph::GraphDocument& document, Guid id,
             StringView type, const Properties& properties) noexcept {
    ObjectRegistry& source = *env.source;
    auto lease = env.types.AcquireValueType(GeneratedType<Properties>::Id(), TypeKind::Struct);
    if (!lease) {
        return false;
    }

    auto saved =
        SaveStructRecord(env.allocator, source, source.OwnerAccess(), lease.Value(), &properties);
    auto name = source.Names()->TryIntern(type);
    if (!saved || !name) {
        return false;
    }

    if (document.graphs.IsEmpty()) {
        nodegraph::Graph main(source.ObjectAllocator());
        main.id = Guid{0x10, 1};
        document.mainGraph = main.id;
        if (!document.graphs.TryPushBack(Move(main))) {
            return false;
        }
    }

    nodegraph::GraphNode node(source.ObjectAllocator());
    node.id = id;
    node.type = name.Value();
    node.typeSchemaVersion = 1;
    return node.properties.bytes.TryAppend(saved.Value().View()).IsOk() &&
           document.graphs[0].nodes.TryPushBack(Move(node)).IsOk();
}

} // namespace

#if ROCKS_WITH_EDITOR_DATA
TEST(MaterialCook_GatherAndSyncReferences) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterMaterialTypes(env.modules));
    REQUIRE(RegisterCurveAtlasTypes(env.modules));
    ObjectRegistry& source = *env.source;
    const ObjectAccessContext access = source.OwnerAccess();
    TestOpener opener(env.allocator);

    RootRef<Package> packages[8];
    const ObjectHandle material = MakeAsset<Material>(env, MaterialIdValue, packages[0], opener);
    const ObjectHandle function =
        MakeAsset<MaterialFunction>(env, FunctionIdValue, packages[1], opener);
    const ObjectHandle brick = MakeAsset<Texture2D>(env, BrickIdValue, packages[2], opener);
    const ObjectHandle sky = MakeAsset<TextureCube>(env, SkyIdValue, packages[3], opener);
    const ObjectHandle curve = MakeAsset<ColorCurveAsset>(env, CurveIdValue, packages[4], opener);
    const ObjectHandle atlas = MakeAsset<ColorCurveAtlas>(env, AtlasIdValue, packages[5], opener);
    const ObjectHandle base =
        MakeAsset<ParameterCollection>(env, BaseCollectionIdValue, packages[6], opener);
    const ObjectHandle collection =
        MakeAsset<ParameterCollection>(env, CollectionIdValue, packages[7], opener);
    REQUIRE(!material.IsNull() && !function.IsNull() && !brick.IsNull() && !sky.IsNull() &&
            !curve.IsNull() && !atlas.IsNull() && !base.IsNull() && !collection.IsNull());

    REQUIRE(asset_test::WithBorrow<Texture2D>(source, brick, [&](Texture2D& value) noexcept {
        value.addressX = TextureAddress::Clamp;
        value.addressY = TextureAddress::Mirror;
        value.srgb = 0;
        return true;
    }));
    REQUIRE(asset_test::WithBorrow<ColorCurveAsset>(
        source, curve, [&](ColorCurveAsset& value) noexcept {
            return value.r.TryAddKey(0.0f, 0.25f).IsOk() && value.r.TryAddKey(1.0f, 0.75f).IsOk();
        }));
    REQUIRE(asset_test::WithBorrow<ColorCurveAtlas>(
        source, atlas, [&](ColorCurveAtlas& value) noexcept {
            return source.AddReference(access, atlas, value.curves, ObjectHandle{}).IsOk() &&
                   source.AddReference(access, atlas, value.curves, curve).IsOk();
        }));
    REQUIRE(asset_test::WithBorrow<ParameterCollection>(
        source, base, [&](ParameterCollection& value) noexcept {
            material::VectorCollectionParameter tint;
            tint.id = Guid{0x72, 1};
            return value.vectors.TryPushBack(tint).IsOk();
        }));
    REQUIRE(asset_test::WithBorrow<ParameterCollection>(
        source, collection, [&](ParameterCollection& value) noexcept {
            for (usize index = 0; index < ChildScalars; ++index) {
                material::ScalarCollectionParameter scalar;
                scalar.id = Guid{0x73, index};
                if (!value.scalars.TryPushBack(scalar)) {
                    return false;
                }
            }

            return source.SetReference(access, collection, value.base, base).IsOk();
        }));

    REQUIRE(asset_test::WithBorrow<MaterialFunction>(
        source, function, [&](MaterialFunction& value) noexcept {
            material::ColorRampProperties ramp;
            ramp.colorCurve.assetId = asset_test::FixedId(CurveIdValue);
            material::TextureObjectProperties cube;
            cube.texture.assetId = asset_test::FixedId(SkyIdValue);
            material::TextureObjectProperties missing;
            missing.texture.assetId = asset_test::FixedId(MissingIdValue);
            value.graph.domain = source.Names()->TryIntern("MaterialFunction").Value();
            return AddNode(env, value.graph, Guid{0x30, 1}, "Material.ColorRamp", ramp) &&
                   AddNode(env, value.graph, Guid{0x30, 2}, "Material.TextureObject", cube) &&
                   AddNode(env, value.graph, Guid{0x30, 3}, "Material.TextureObject", missing);
        }));
    REQUIRE(asset_test::WithBorrow<Material>(source, material, [&](Material& value) noexcept {
        material::FunctionCallProperties call(source.ObjectAllocator());
        call.materialFunction.assetId = asset_test::FixedId(FunctionIdValue);
        material::TextureSampleProperties sample(source.ObjectAllocator());
        sample.texture.assetId = asset_test::FixedId(BrickIdValue);
        material::CurveAtlasRowParameterProperties row;
        row.curve.assetId = asset_test::FixedId(CurveIdValue);
        row.atlas.assetId = asset_test::FixedId(AtlasIdValue);
        material::ParameterCollectionValueProperties fog;
        fog.collection.assetId = asset_test::FixedId(CollectionIdValue);
        value.graph.domain = source.Names()->TryIntern("Material").Value();
        return AddNode(env, value.graph, Guid{0x20, 1}, "Material.FunctionCall", call) &&
               AddNode(env, value.graph, Guid{0x20, 2}, "Material.TextureSample", sample) &&
               AddNode(env, value.graph, Guid{0x20, 3}, "Material.CurveAtlasRowParameter", row) &&
               AddNode(env, value.graph, Guid{0x20, 4}, "Material.ParameterCollectionValue", fog);
    }));

    const MaterialCookGatherContext gather{&source, access, &opener};
    MaterialCookSources sources(env.allocator);
    REQUIRE(asset_test::WithBorrow<Material>(source, material, [&](Material& value) noexcept {
        return sources.Gather(gather, value.graph).IsOk();
    }));

    // 함수 snapshot(다른 함수·머티리얼은 없다), 색 커브, atlas 행(null 행 포함 — RowOf 순서).
    const material::MaterialGraphSnapshot* functionSnapshot =
        sources.FindFunction(asset_test::FixedId(FunctionIdValue).value);
    REQUIRE(functionSnapshot != nullptr);
    REQUIRE(functionSnapshot->document != nullptr);
    CHECK(functionSnapshot->document->graphs.Size() == 1);
    CHECK(sources.FindMaterial(asset_test::FixedId(FunctionIdValue).value) == nullptr);
    const material::MaterialColorCurveSnapshot* curveSnapshot =
        sources.FindColorCurve(asset_test::FixedId(CurveIdValue).value);
    REQUIRE(curveSnapshot != nullptr);
    REQUIRE(curveSnapshot->r != nullptr);
    CHECK(curveSnapshot->r->KeyCount() == 2);
    const material::MaterialCurveAtlasSnapshot* atlasSnapshot =
        sources.FindCurveAtlas(asset_test::FixedId(AtlasIdValue).value);
    REQUIRE(atlasSnapshot != nullptr);
    REQUIRE(atlasSnapshot->curves.Size() == 2);
    CHECK(atlasSnapshot->curves[0] == Guid{});
    CHECK(atlasSnapshot->curves[1] == asset_test::FixedId(CurveIdValue).value);

    // 텍스처 사실: 2D(주소 모드·sRGB), 큐브, atlas(2D) — AssetId 순. 없는 텍스처는 빠진다.
    REQUIRE(sources.Textures().Size() == 3);
    const material::MaterialTextureFacts& brickFacts = sources.Textures()[0];
    CHECK(brickFacts.asset == asset_test::FixedId(BrickIdValue).value);
    CHECK(brickFacts.kind == material::IrTextureKind::Texture2D);
    CHECK(brickFacts.address[0] == TextureAddress::Clamp);
    CHECK(brickFacts.address[1] == TextureAddress::Mirror);
    CHECK(!brickFacts.srgb);
    CHECK(sources.Textures()[1].kind == material::IrTextureKind::TextureCube);
    CHECK(sources.Textures()[2].asset == asset_test::FixedId(AtlasIdValue).value);
    CHECK(sources.Textures()[2].kind == material::IrTextureKind::Texture2D);

    // 컬렉션 layout(UE GetParameterIndex): base vector가 0, 자식 scalar 다섯이 1–2, 모두 3칸이다.
    REQUIRE(sources.Collections().Size() == 1);
    const material::MaterialCollectionLayout& layout = sources.Collections()[0];
    CHECK(layout.asset == asset_test::FixedId(CollectionIdValue).value);
    CHECK(layout.vectors == 3);
    REQUIRE(layout.entries.Size() == 1 + ChildScalars);
    CHECK(layout.entries[0].parameterId == (Guid{0x72, 1}));
    CHECK(layout.entries[0].vector == 0);
    CHECK(layout.entries[0].component == material::MaterialCollectionWholeVector);
    CHECK(layout.entries[1].vector == 1);
    CHECK(layout.entries[1].component == 0);
    CHECK(layout.entries[ChildScalars].vector == 2);
    CHECK(layout.entries[ChildScalars].component == 0);

    // 참조는 (역할, AssetId) 순이고 없는 텍스처는 object null이다.
    const Span<const MaterialCookReference> references = sources.References();
    for (usize index = 1; index < references.Size(); ++index) {
        const MaterialCookReference& left = references[index - 1];
        const MaterialCookReference& right = references[index];
        CHECK(static_cast<uint8>(left.role) < static_cast<uint8>(right.role) ||
              (left.role == right.role && GuidLess(left.asset.value, right.asset.value)));
    }

    bool missingSeen = false;
    for (const MaterialCookReference& reference : references) {
        if (reference.asset == asset_test::FixedId(MissingIdValue)) {
            missingSeen = true;
            CHECK(reference.object.IsNull());
        }
    }

    CHECK(missingSeen);

    // 참조 동기화: 텍스처 셋(벽돌·하늘·atlas)·컬렉션 하나, 두 번째는 바뀌지 않는다.
    bool changed = false;
    REQUIRE(SyncMaterialReferences(gather, material, changed));
    CHECK(changed);
    REQUIRE(SyncMaterialReferences(gather, material, changed));
    CHECK(!changed);
    REQUIRE(asset_test::WithBorrow<Material>(source, material, [&](Material& value) noexcept {
        return value.referencedTextures.Size() == 3 && value.referencedTextures[0] == brick &&
               value.parameterCollections.Size() == 1 &&
               value.parameterCollections[0] == collection;
    }));
    AssetId missing{};
    auto inSync = MaterialReferencesInSync(gather, material, sources, missing);
    REQUIRE(inSync);
    CHECK(inSync.Value());

    // 필드에서 텍스처를 지우면 대조가 그 AssetId를 찾는다.
    REQUIRE(asset_test::WithBorrow<Material>(source, material, [&](Material& value) noexcept {
        return source.ClearReferences(access, material, value.referencedTextures).IsOk();
    }));
    inSync = MaterialReferencesInSync(gather, material, sources, missing);
    REQUIRE(inSync);
    CHECK(!inSync.Value());
    CHECK(missing == asset_test::FixedId(BrickIdValue));
}
#endif

} // namespace material_cook_sources_tests_file
