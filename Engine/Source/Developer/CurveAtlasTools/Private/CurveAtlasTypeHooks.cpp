#include <AssetRuntime/Package.generated.h>
#include <Core/Encoding/Sha256.h>
#include <CurveAssets/ColorCurveAsset.generated.h>
#include <CurveAtlas/ColorCurveAtlas.generated.h>
#include <CurveAtlasTools/ColorCurveAtlasBake.h>
#include <CurveAtlasTools/CurveAtlasTypeHooks.h>
#include <Object/ObjectRegistry.h>

// 색 커브 atlas 타입 hook이다(Docs/Design/CurveAssets 4.2·4.3절).
namespace rocks {

namespace {

// UE CacheAndResetColorAdjustments(CurveLinearColorAtlas.cpp:164-189)의 항등값이다.
constexpr float IdentityBrightness = 1.0f;
constexpr float IdentityBrightnessCurve = 1.0f;
constexpr float IdentityVibrance = 0.0f;
constexpr float IdentitySaturation = 1.0f;
constexpr float IdentityRgbCurve = 1.0f;
constexpr float IdentityHue = 0.0f;
constexpr float IdentityMinAlpha = 0.0f;
constexpr float IdentityMaxAlpha = 1.0f;

// 너비 필드 이름이다(asset-new 대입 판정).
constexpr StringView ColorCurveAtlasWidthField = "width";

// 원본이 구운 결과와 같은지다(형식·크기·slice·mip·layer와 bulk 내용 — BulkDataRef::SameContent).
bool SameSource(const TextureSource& source, const ColorCurveAtlasPixels& pixels,
                const BulkDataRef& baked) noexcept {
    return source.format == TextureSourceFormat::RGBA16F && source.sizeX == pixels.sizeX &&
           source.sizeY == pixels.sizeY && source.numSlices == 1 && source.numMips == 1 &&
           source.numLayers == 1 && source.bulk.SameContent(baked);
}

// assignedFields에 name이 있는지다(대소문자 무시 — 텍스트 경로의 첫 마디, 12.14절).
bool HasField(Span<const StringView> assignedFields, StringView name) noexcept {
    for (StringView field : assignedFields) {
        if (EqualsIgnoringAsciiCase(field, name)) {
            return true;
        }
    }

    return false;
}

// asset-new의 팩토리 너비다(UE UCurveLinearColorAtlasFactory — 대입하지 않았을 때).
Result<void> ApplyFactoryWidth(const AssetTypeHookContext& context,
                               Span<const StringView> assignedFields,
                               AssetSourceRebuildSink& sink) noexcept {
    if (sink.Cause() != AssetSourceRebuildCause::New ||
        HasField(assignedFields, ColorCurveAtlasWidthField)) {
        return Result<void>::Success();
    }

    auto borrowed =
        context.registry.ResolveForWrite<ColorCurveAtlas>(context.access, context.object);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    value.Value()->width = NewColorCurveAtlasWidth;
    return Result<void>::Success();
}

// curves를 풀어 굽는다. current에 지금 원본을 둔다.
Result<void> BakeAtlas(const AssetTypeHookContext& context, AssetSourceRebuildSink& sink,
                       ColorCurveAtlasPixels& pixels, TextureSource& current) noexcept {
    Allocator& allocator = context.registry.ObjectAllocator();
    auto borrowed = context.registry.Resolve<ColorCurveAtlas>(context.access, context.object);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    const ColorCurveAtlas& atlas = *value.Value();
    current = atlas.source;
    const usize count = atlas.curves.Size();
    // 굽는 동안 커브를 빌려 둔다(포인터가 살아 있게).
    Array<Borrow<ColorCurveAsset>> holds(allocator);
    Array<const ColorCurveAsset*> curves(allocator);
    if (auto reserved = holds.TryReserve(count); !reserved) {
        return reserved;
    }

    if (auto reserved = curves.TryReserve(count); !reserved) {
        return reserved;
    }

    for (usize at = 0; at < count; ++at) {
        const ObjectHandle handle = atlas.curves[at];
        if (handle.IsNull()) {
            (void)curves.TryPushBack(nullptr);
            continue;
        }

        auto curve = context.registry.Resolve<ColorCurveAsset>(context.access, handle);
        if (!curve) {
            return sink.Fail(AssetDiagnosticCode::MissingDependency, curve.Error(), at);
        }

        auto curveValue = curve.Value().Get();
        if (!curveValue) {
            return sink.Fail(AssetDiagnosticCode::MissingDependency, curveValue.Error(), at);
        }

        (void)curves.TryPushBack(curveValue.Value());
        (void)holds.TryPushBack(Move(curve.Value()));
    }

    auto baked = BakeColorCurveAtlas(curves.View(), atlas.width, atlas.disableAdjustments, pixels);
    if (!baked && baked.Error() == ErrorCode::CapacityExceeded) {
        const uint64 over = atlas.EffectiveWidth() > MaxColorCurveAtlasDimension
                                ? atlas.EffectiveWidth()
                                : static_cast<uint64>(count);
        return sink.Fail(AssetDiagnosticCode::InvalidSetting, ErrorCode::CapacityExceeded, over);
    }

    return baked;
}

Result<void> RebuildColorCurveAtlas(const AssetTypeHookContext& context,
                                    Span<const StringView> assignedFields,
                                    AssetSourceRebuildSink& sink) noexcept {
    if (auto base = context.CallBaseRebuildSource(assignedFields, sink); !base) {
        return base;
    }

    // 텍스처 원소 재임포트 전파는 atlas와 무관하다(커브는 텍스처가 아니다).
    if (sink.Cause() == AssetSourceRebuildCause::Propagate) {
        return Result<void>::Success();
    }

    if (auto factory = ApplyFactoryWidth(context, assignedFields, sink); !factory) {
        return factory;
    }

    ColorCurveAtlasPixels pixels(context.registry.ObjectAllocator());
    TextureSource current;
    if (auto baked = BakeAtlas(context, sink, pixels, current); !baked) {
        return baked;
    }

    // 같으면 쓰지 않는다(낡지 않은 atlas를 열어도 dirty가 되지 않는다 — A5j2c).
    BulkDataRef baked;
    baked.rawBytes = pixels.bytes.Size();
    baked.sha256 = Sha256::Hash(pixels.bytes.View());
    if (SameSource(current, pixels, baked)) {
        return Result<void>::Success();
    }

    const uint32 sizeX = pixels.sizeX;
    const uint32 sizeY = pixels.sizeY;
    auto added = sink.AddBulk(Move(pixels.bytes));
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    auto borrowed =
        context.registry.ResolveForWrite<ColorCurveAtlas>(context.access, context.object);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    TextureSource source;
    source.format = TextureSourceFormat::RGBA16F;
    source.sizeX = sizeX;
    source.sizeY = sizeY;
    source.numSlices = 1;
    source.numMips = 1;
    source.numLayers = 1;
    source.bulk = added.Value();
    value.Value()->source = source;
    sink.MarkRebuilt();
    return Result<void>::Success();
}

// handle이 속한 패키지의 id다. 패키지 밖이거나 self(atlas)와 같은 패키지면 null이다 — 복제·ID
// 수정에서 context.asset은 새 id이고 context.object는 원본 객체라, 자기 패키지는 id가 아니라
// outermost로 가른다.
Result<AssetId> PackageIdOf(const AssetTypeHookContext& context, ObjectHandle handle,
                            ObjectHandle self) noexcept {
    auto outermost = context.registry.GetOutermost(context.access, handle);
    if (!outermost) {
        return Result<AssetId>::Failure(outermost.Error());
    }

    if (outermost.Value() == self) {
        return Result<AssetId>::Success(AssetId{});
    }

    auto type = context.registry.AcquireType(context.access, outermost.Value());
    if (!type) {
        return Result<AssetId>::Failure(type.Error());
    }

    if (!type.Value().IsA(TypeIdOf<Package>())) {
        return Result<AssetId>::Success(AssetId{});
    }

    auto package = context.registry.Resolve<Package>(context.access, outermost.Value());
    if (!package) {
        return Result<AssetId>::Failure(package.Error());
    }

    auto packageValue = package.Value().Get();
    if (!packageValue) {
        return Result<AssetId>::Failure(packageValue.Error());
    }

    return Result<AssetId>::Success(packageValue.Value()->Id());
}

Result<void> ColorCurveAtlasBuildDependencies(const AssetTypeHookContext& context,
                                              AssetDependencySink& sink) noexcept {
    if (auto base = context.CallBaseBuildDependencies(sink); !base) {
        return base;
    }

    auto self = context.registry.GetOutermost(context.access, context.object);
    if (!self) {
        return Result<void>::Failure(self.Error());
    }

    auto borrowed = context.registry.Resolve<ColorCurveAtlas>(context.access, context.object);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    for (ObjectHandle curve : value.Value()->curves.View()) {
        if (curve.IsNull()) {
            continue;
        }

        auto id = PackageIdOf(context, curve, self.Value());
        if (!id) {
            return Result<void>::Failure(id.Error());
        }

        if (id.Value().IsNull()) {
            continue;
        }

        if (auto added = sink.Add(id.Value()); !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

Result<void> NormalizeColorCurveAtlas(const AssetTypeHookContext& context,
                                      Span<const StringView> assignedFields) noexcept {
    if (auto base = context.CallBaseNormalizeSettings(assignedFields); !base) {
        return base;
    }

    auto borrowed =
        context.registry.ResolveForWrite<ColorCurveAtlas>(context.access, context.object);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    ColorCurveAtlas& atlas = *value.Value();
    atlas.width = atlas.EffectiveWidth();
    if (atlas.disableAdjustments) {
        atlas.adjustBrightness = IdentityBrightness;
        atlas.adjustBrightnessCurve = IdentityBrightnessCurve;
        atlas.adjustVibrance = IdentityVibrance;
        atlas.adjustSaturation = IdentitySaturation;
        atlas.adjustRGBCurve = IdentityRgbCurve;
        atlas.adjustHue = IdentityHue;
        atlas.adjustMinAlpha = IdentityMinAlpha;
        atlas.adjustMaxAlpha = IdentityMaxAlpha;
        atlas.chromaKeyTexture = false;
    }

    return Result<void>::Success();
}

} // namespace

Result<void> RegisterCurveAtlasTypeHooks(void* state, AssetTypeHookTable& table,
                                         Array<AssetTypeHookRegistration>& tokens) noexcept {
    (void)state;
    if (auto reserved = tokens.TryReserve(tokens.Size() + 1); !reserved) {
        return reserved;
    }

    AssetTypeHooks hooks;
    hooks.buildDependencies = &ColorCurveAtlasBuildDependencies;
    hooks.normalizeSettings = &NormalizeColorCurveAtlas;
    hooks.rebuildSource = &RebuildColorCurveAtlas;
    auto registered = table.Register(TypeIdOf<ColorCurveAtlas>(), hooks);
    if (!registered) {
        return Result<void>::Failure(registered.Error());
    }

    (void)tokens.TryPushBack(Move(registered.Value()));
    return Result<void>::Success();
}

} // namespace rocks
