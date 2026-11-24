#include "TextureImportTestSupport.h"

#include <AssetRuntime/AssetTypeHooks.h>
#include <AssetRuntime/MemoryBulkSource.h>
#include <Core/Memory/SystemAllocator.h>
#include <Meta/GeneratedType.h>
#include <Object/ObjectRegistry.h>
#include <TestSupport/Test.h>
#include <Texture/Texture.generated.h>
#include <TextureTools/TextureImport.h>

#include <string.h>

// 원본 묶음 재구성 hook 시험이다(Docs/Design/AssetSystem 12.14절 A5j2a — 14.1.103절 대조표):
// 2DArray·CubeArray 잇기(byte 순서·slice·처음 만들기 설정·대입 칸 우선·원본이 있던 배열), 빈 목록,
// 원소 판정(null·원본 없음·크기·형식·sRGB 여부·slice·numLayers·상한·byte 한도), 임포트한 에셋 보호,
// 필드 이름(대소문자), Propagate(바뀐 원소를 가리킬 때만), Volume(기본 타일 식·명시 타일·나머지
// 버림·512 상한·빈 원본 경고·asset-new 기본 원본·Propagate는 타일 유지).
using namespace rocks;
namespace texture_source_rebuild_tests_file {

using texture_import_test::TextureEnv;

namespace {

// hook sink 가짜다(원소 bulk는 공유 MemoryBulkSource — object는 기록만 한다).
class FakeSink final : public AssetSourceRebuildSink {
public:
    explicit FakeSink(Allocator& allocator) noexcept : bulk(allocator), warnings(allocator) {
    }

    Result<Span<const uint8>> ReadBulk(ObjectHandle, const BulkDataRef& ref) noexcept override {
        ++reads;
        return bulk.ReadBulk(ref);
    }

    Result<BulkDataRef> AddBulk(Array<uint8>&& raw) noexcept override {
        ++adds;
        return bulk.Add(Move(raw));
    }

    bool HasImportRecord() const noexcept override {
        return importRecord;
    }

    AssetSourceRebuildCause Cause() const noexcept override {
        return cause;
    }

    ObjectHandle ChangedSource() const noexcept override {
        return changed;
    }

    Result<void> Fail(AssetDiagnosticCode code, ErrorCode error, uint64 detail) noexcept override {
        failCode = code;
        failError = error;
        failDetail = detail;
        return Result<void>::Failure(error);
    }

    Result<void> Warn(AssetDiagnosticCode code, uint64 detail) noexcept override {
        return warnings.TryPushBack(texture_import_test::SeenWarning{code, detail});
    }

    MemoryBulkSource bulk;
    Array<texture_import_test::SeenWarning> warnings;
    bool importRecord = false;
    AssetSourceRebuildCause cause = AssetSourceRebuildCause::Edit;
    ObjectHandle changed{};
    AssetDiagnosticCode failCode = AssetDiagnosticCode::None;
    ErrorCode failError = ErrorCode::None;
    uint64 failDetail = 0;
    usize reads = 0;
    usize adds = 0;
};

// 텍스처 타입·hook 표·객체를 가진 장면이다.
struct RebuildScene {
    RebuildScene() noexcept
        : table(env.allocator, env.types), tokens(env.allocator), roots(env.allocator) {
        ready = env.ready && static_cast<bool>(RegisterTextureTypeHooks(nullptr, table, tokens));
    }

    ~RebuildScene() noexcept {
        roots.Clear();
        tokens.Clear();
    }

    const ObjectAccessContext& Access() const noexcept {
        return env.objects.OwnerAccess();
    }

    // type 텍스처를 만든다.
    ObjectHandle Make(TypeId type) noexcept {
        auto made = env.objects.NewObject<Texture>(Access(), type);
        if (!made) {
            return {};
        }

        const ObjectHandle handle = made.Value().Handle();
        return roots.TryPushBack(Move(made.Value())) ? handle : ObjectHandle{};
    }

    template <class Fn>
    bool Edit(ObjectHandle handle, Fn&& fn) noexcept {
        auto borrowed = env.objects.ResolveForWrite<Texture>(Access(), handle);
        if (!borrowed) {
            return false;
        }

        auto value = borrowed.Value().Get();
        return value && fn(*value.Value());
    }

    // 원본(format, x, y, slices — byte는 seed 무늬)을 둔 원소를 만든다.
    ObjectHandle Element(FakeSink& sink, TypeId type, TextureSourceFormat format, uint32 x,
                         uint32 y, uint32 slices, uint8 seed, bool srgb = false) noexcept {
        const ObjectHandle handle = Make(type);
        Array<uint8> bytes(env.allocator);
        const usize count = usize{x} * y * slices * TextureSourceTexelBytes(format);
        if (handle.IsNull() || !bytes.TryResize(count)) {
            return {};
        }

        for (usize index = 0; index < count; ++index) {
            bytes[index] = static_cast<uint8>(seed + index);
        }

        auto ref = sink.bulk.Add(Move(bytes));
        if (!ref) {
            return {};
        }

        const BulkDataRef bulk = ref.Value();
        const bool edited = Edit(handle, [&](Texture& texture) noexcept {
            texture.source.format = format;
            texture.source.sizeX = x;
            texture.source.sizeY = y;
            texture.source.numSlices = slices;
            texture.source.bulk = bulk;
            texture.srgb = srgb ? 1 : 0;
            return true;
        });
        return edited ? handle : ObjectHandle{};
    }

    // 배열 owner의 sourceTextures에 handles를 차례로 넣는다(null 포함).
    bool SetList(ObjectHandle owner, bool cube, Span<const ObjectHandle> handles) noexcept {
        auto borrowed = env.objects.ResolveForWrite<Texture>(Access(), owner);
        if (!borrowed) {
            return false;
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return false;
        }

        for (ObjectHandle handle : handles) {
            Result<void> added = Result<void>::Success();
            if (cube) {
                auto& list = static_cast<TextureCubeArray*>(value.Value())->sourceTextures;
                added = env.objects.InsertReferenceAt(Access(), owner, list, list.Size(), handle);
            } else {
                auto& list = static_cast<Texture2DArray*>(value.Value())->sourceTextures;
                added = env.objects.InsertReferenceAt(Access(), owner, list, list.Size(), handle);
            }

            if (!added) {
                return false;
            }
        }

        return true;
    }

    bool SetSource2D(ObjectHandle owner, ObjectHandle source, int32 tileX, int32 tileY) noexcept {
        auto borrowed = env.objects.ResolveForWrite<Texture>(Access(), owner);
        if (!borrowed) {
            return false;
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return false;
        }

        auto& volume = static_cast<VolumeTexture&>(*value.Value());
        volume.source2DTileSizeX = tileX;
        volume.source2DTileSizeY = tileY;
        return static_cast<bool>(
            env.objects.SetReference(Access(), owner, volume.source2DTexture, source));
    }

    Result<void> Rebuild(ObjectHandle object, Span<const StringView> fields,
                         FakeSink& sink) noexcept {
        auto lease = env.objects.AcquireType(Access(), object);
        if (!lease) {
            return Result<void>::Failure(lease.Error());
        }

        return table.RebuildSource(env.objects, Access(), object, lease.Value().Id(), AssetId{},
                                   fields, sink);
    }

    const Texture* Read(ObjectHandle handle) noexcept {
        auto borrowed = env.objects.Resolve<Texture>(Access(), handle);
        if (!borrowed) {
            return nullptr;
        }

        auto value = borrowed.Value().Get();
        return value ? value.Value() : nullptr;
    }

    TextureEnv env;
    AssetTypeHookTable table;
    Array<AssetTypeHookRegistration> tokens;
    Array<RootRef<Texture>> roots;
    bool ready = false;
};

const StringView SourceTextures[] = {"sourceTextures"};
const StringView Source2D[] = {"source2DTexture"};

} // namespace

// 2DArray 잇기: 원소 둘의 mip 0 byte를 원소 순으로 잇고(4×4 G8 slice 2), 처음 만들기는 첫 원소의
// Compression·LODGroup과 NoMipmaps·PowerOfTwo None·NeverStream을 가져오며(대입 칸은 둔다), 배열
// SRGB는 첫 원소의 sRGB 여부다. 원본이 있던 배열은 설정을 바꾸지 않는다. 필드 이름은 대소문자를
// 무시한다.
TEST(TextureSourceRebuild_ArrayConcat) {
    RebuildScene scene;
    REQUIRE(scene.ready);
    FakeSink sink(scene.env.allocator);
    const ObjectHandle a =
        scene.Element(sink, TypeIdOf<Texture2D>(), TextureSourceFormat::G8, 4, 4, 1, 10, true);
    const ObjectHandle b =
        scene.Element(sink, TypeIdOf<Texture2D>(), TextureSourceFormat::G8, 4, 4, 1, 100, true);
    REQUIRE(!a.IsNull() && !b.IsNull());
    REQUIRE(scene.Edit(a, [](Texture& texture) noexcept {
        texture.compressionSettings = TextureCompressionSettings::Masks;
        texture.lodGroup = TextureGroup::UI;
        return true;
    }));
    const ObjectHandle array = scene.Make(TypeIdOf<Texture2DArray>());
    const ObjectHandle list[] = {a, b};
    REQUIRE(scene.SetList(array, false, list));
    const StringView fields[] = {"SourceTextures", "lodGroup"};
    REQUIRE(scene.Edit(array, [](Texture& texture) noexcept {
        texture.lodGroup = TextureGroup::Skybox;
        return true;
    }));
    REQUIRE(scene.Rebuild(array, fields, sink));
    const Texture* made = scene.Read(array);
    REQUIRE(made != nullptr);
    CHECK(made->source.format == TextureSourceFormat::G8 && made->source.sizeX == 4 &&
          made->source.sizeY == 4 && made->source.numSlices == 2 && made->source.numMips == 1 &&
          made->srgb == 1);
    CHECK(made->compressionSettings == TextureCompressionSettings::Masks &&
          made->mipGenSettings == TextureMipGenSettings::NoMipmaps &&
          made->powerOfTwoMode == TexturePowerOfTwoSetting::None &&
          made->lodGroup == TextureGroup::Skybox && made->neverStream == 1);
    auto bytes = sink.bulk.ReadBulk(made->source.bulk);
    REQUIRE(bytes && bytes.Value().Size() == 32);
    CHECK(bytes.Value()[0] == 10 && bytes.Value()[15] == 25 && bytes.Value()[16] == 100 &&
          bytes.Value()[31] == 115);

    // 원본이 있던 배열: 다시 대입해도 설정을 바꾸지 않는다(원소 갱신은 다시 복사한다).
    REQUIRE(scene.Edit(array, [](Texture& texture) noexcept {
        texture.mipGenSettings = TextureMipGenSettings::SimpleAverage;
        texture.neverStream = 0;
        return true;
    }));
    REQUIRE(scene.Edit(b, [](Texture& texture) noexcept {
        texture.srgb = 1;
        return true;
    }));
    REQUIRE(scene.Rebuild(array, SourceTextures, sink));
    made = scene.Read(array);
    REQUIRE(made != nullptr);
    CHECK(made->mipGenSettings == TextureMipGenSettings::SimpleAverage && made->neverStream == 0);

    // 묶음 필드가 없으면 아무것도 하지 않는다.
    const usize adds = sink.adds;
    const StringView other[] = {"label"};
    REQUIRE(scene.Rebuild(array, other, sink));
    CHECK(sink.adds == adds);
}

// CubeArray: 원소 6면 × 2 → slice 12, slice가 6이 아닌 원소는 InvalidSetting(번호), 상한 42개.
TEST(TextureSourceRebuild_CubeArray) {
    RebuildScene scene;
    REQUIRE(scene.ready);
    FakeSink sink(scene.env.allocator);
    const ObjectHandle a =
        scene.Element(sink, TypeIdOf<TextureCube>(), TextureSourceFormat::RGBA8, 2, 2, 6, 1);
    const ObjectHandle b =
        scene.Element(sink, TypeIdOf<TextureCube>(), TextureSourceFormat::RGBA8, 2, 2, 6, 2);
    const ObjectHandle array = scene.Make(TypeIdOf<TextureCubeArray>());
    const ObjectHandle list[] = {a, b};
    REQUIRE(scene.SetList(array, true, list));
    REQUIRE(scene.Rebuild(array, SourceTextures, sink));
    const Texture* made = scene.Read(array);
    REQUIRE(made != nullptr);
    CHECK(made->source.numSlices == 12 && made->source.bulk.rawBytes == 2 * 2 * 4 * 12);

    const ObjectHandle flat =
        scene.Element(sink, TypeIdOf<TextureCube>(), TextureSourceFormat::RGBA8, 2, 2, 1, 3);
    const ObjectHandle bad = scene.Make(TypeIdOf<TextureCubeArray>());
    const ObjectHandle mixed[] = {a, flat};
    REQUIRE(scene.SetList(bad, true, mixed));
    CHECK(scene.Rebuild(bad, SourceTextures, sink).Error() == ErrorCode::InvalidArgument &&
          sink.failCode == AssetDiagnosticCode::InvalidSetting && sink.failDetail == 1);

    // 상한(42 = 256 / 6)을 넘는 43개.
    const ObjectHandle many = scene.Make(TypeIdOf<TextureCubeArray>());
    Array<ObjectHandle> cubes(scene.env.allocator);
    for (usize index = 0; index < 43; ++index) {
        REQUIRE(cubes.TryPushBack(a));
    }

    REQUIRE(scene.SetList(many, true, cubes.View()));
    CHECK(scene.Rebuild(many, SourceTextures, sink).Error() == ErrorCode::InvalidArgument &&
          sink.failCode == AssetDiagnosticCode::InvalidSetting && sink.failDetail == 42);
}

// 원소 판정: null·원본 없음·크기·형식·sRGB 여부(legacy gamma는 sRGB가 아님, G16 + SRGB on은
// 선형이라 G16과 같다)·numLayers·byte 한도(할당 전), 빈 목록 → 빈 원본, 임포트한 배열 보호.
TEST(TextureSourceRebuild_ArrayChecks) {
    RebuildScene scene;
    REQUIRE(scene.ready);
    FakeSink sink(scene.env.allocator);
    const TypeId plane = TypeIdOf<Texture2D>();
    const ObjectHandle good = scene.Element(sink, plane, TextureSourceFormat::G8, 4, 4, 1, 1, true);
    const ObjectHandle empty = scene.Make(plane);
    const ObjectHandle wide = scene.Element(sink, plane, TextureSourceFormat::G8, 8, 4, 1, 2, true);
    const ObjectHandle rgba =
        scene.Element(sink, plane, TextureSourceFormat::RGBA8, 4, 4, 1, 3, true);
    const ObjectHandle legacy =
        scene.Element(sink, plane, TextureSourceFormat::G8, 4, 4, 1, 4, true);
    REQUIRE(scene.Edit(legacy, [](Texture& texture) noexcept {
        texture.useLegacyGamma = 1;
        return true;
    }));
    const ObjectHandle layered =
        scene.Element(sink, plane, TextureSourceFormat::G8, 4, 4, 1, 5, true);
    REQUIRE(scene.Edit(layered, [](Texture& texture) noexcept {
        texture.source.numLayers = 2;
        return true;
    }));
    struct Case {
        ObjectHandle second;
        AssetDiagnosticCode code;
        ErrorCode error;
    };
    const Case cases[] = {
        {ObjectHandle{}, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument},
        {empty, AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument},
        {wide, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument},
        {rgba, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument},
        {legacy, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument},
        {layered, AssetDiagnosticCode::UnsupportedSetting, ErrorCode::Unsupported},
    };
    for (const Case& item : cases) {
        const ObjectHandle array = scene.Make(TypeIdOf<Texture2DArray>());
        const ObjectHandle list[] = {good, item.second};
        REQUIRE(scene.SetList(array, false, list));
        sink.failCode = AssetDiagnosticCode::None;
        CHECK(scene.Rebuild(array, SourceTextures, sink).Error() == item.error &&
              sink.failCode == item.code && sink.failDetail == 1);
        const Texture* left = scene.Read(array);
        REQUIRE(left != nullptr);
        CHECK(left->source.bulk.IsEmpty());
    }

    // G16 + SRGB on은 선형(UE GetGammaSpace)이라 G16 SRGB off와 섞을 수 있고 배열 SRGB는 off다.
    const ObjectHandle g16On =
        scene.Element(sink, plane, TextureSourceFormat::G16, 2, 2, 1, 6, true);
    const ObjectHandle g16Off =
        scene.Element(sink, plane, TextureSourceFormat::G16, 2, 2, 1, 7, false);
    const ObjectHandle linear = scene.Make(TypeIdOf<Texture2DArray>());
    const ObjectHandle pair[] = {g16On, g16Off};
    REQUIRE(scene.SetList(linear, false, pair));
    REQUIRE(scene.Rebuild(linear, SourceTextures, sink));
    const Texture* mixed = scene.Read(linear);
    REQUIRE(mixed != nullptr);
    CHECK(mixed->source.numSlices == 2 && mixed->srgb == 0);

    // byte 한도: 16384² RGBA32F 둘(8 GiB)은 읽기·할당 전에 CapacityExceeded다.
    const ObjectHandle huge = scene.Element(sink, plane, TextureSourceFormat::G8, 1, 1, 1, 8);
    REQUIRE(scene.Edit(huge, [](Texture& texture) noexcept {
        texture.source.format = TextureSourceFormat::RGBA32F;
        texture.source.sizeX = 16384;
        texture.source.sizeY = 16384;
        return true;
    }));
    const ObjectHandle large = scene.Make(TypeIdOf<Texture2DArray>());
    const ObjectHandle twice[] = {huge, huge};
    REQUIRE(scene.SetList(large, false, twice));
    const usize reads = sink.reads;
    CHECK(scene.Rebuild(large, SourceTextures, sink).Error() == ErrorCode::CapacityExceeded &&
          sink.failCode == AssetDiagnosticCode::InvalidSetting && sink.reads == reads);

    // 빈 목록 → 빈 원본, 임포트한 배열은 그대로·묶음 대입은 거부.
    const ObjectHandle cleared = scene.Make(TypeIdOf<Texture2DArray>());
    const ObjectHandle one[] = {good};
    REQUIRE(scene.SetList(cleared, false, one));
    REQUIRE(scene.Rebuild(cleared, SourceTextures, sink));
    const ObjectHandle emptied = scene.Make(TypeIdOf<Texture2DArray>());
    REQUIRE(scene.Edit(emptied, [&](Texture& texture) noexcept {
        texture.source = scene.Read(cleared)->source;
        return true;
    }));
    REQUIRE(scene.Rebuild(emptied, SourceTextures, sink));
    CHECK(scene.Read(emptied)->source.bulk.IsEmpty());
    sink.importRecord = true;
    REQUIRE(scene.Edit(emptied, [&](Texture& texture) noexcept {
        texture.source = scene.Read(cleared)->source;
        return true;
    }));
    REQUIRE(scene.Rebuild(emptied, SourceTextures, sink));
    CHECK(!scene.Read(emptied)->source.bulk.IsEmpty());
    sink.failCode = AssetDiagnosticCode::None;
    CHECK(scene.Rebuild(cleared, SourceTextures, sink).Error() == ErrorCode::InvalidArgument &&
          sink.failCode == AssetDiagnosticCode::InvalidSetting);
    sink.importRecord = false;
}

// Propagate: 바뀐 원소를 가리키는 배열만 다시 복사하고(대입 필드 없이), 가리키지 않으면 그대로다.
TEST(TextureSourceRebuild_Propagate) {
    RebuildScene scene;
    REQUIRE(scene.ready);
    FakeSink sink(scene.env.allocator);
    const ObjectHandle a =
        scene.Element(sink, TypeIdOf<Texture2D>(), TextureSourceFormat::G8, 2, 2, 1, 1);
    const ObjectHandle other =
        scene.Element(sink, TypeIdOf<Texture2D>(), TextureSourceFormat::G8, 2, 2, 1, 2);
    const ObjectHandle array = scene.Make(TypeIdOf<Texture2DArray>());
    const ObjectHandle list[] = {a};
    REQUIRE(scene.SetList(array, false, list));
    sink.cause = AssetSourceRebuildCause::Propagate;
    sink.changed = other;
    REQUIRE(scene.Rebuild(array, {}, sink));
    CHECK(scene.Read(array)->source.bulk.IsEmpty());
    sink.changed = a;
    REQUIRE(scene.Rebuild(array, {}, sink));
    CHECK(scene.Read(array)->source.numSlices == 1 && !scene.Read(array)->source.bulk.IsEmpty());
}

// Volume: 기본 타일(8×8 → 타일 4×4·깊이 4, 10×27 → 타일 5×13·깊이 4 — 정수 나눗셈 경계), 타일 순서
// byte, 명시 타일(2×2 → 깊이 16), 512 상한, 타일 0·음수 → 빈 원본 + SourceCleared, asset-new 기본
// 원본 1×1 RGBA8 (0, 0, 0, 255), 임포트한 Volume 보호, Propagate는 타일을 다시 정하지 않는다.
TEST(TextureSourceRebuild_Volume) {
    RebuildScene scene;
    REQUIRE(scene.ready);
    FakeSink sink(scene.env.allocator);
    const TypeId plane = TypeIdOf<Texture2D>();
    const ObjectHandle atlas = scene.Element(sink, plane, TextureSourceFormat::G8, 8, 8, 1, 0);
    const ObjectHandle volume = scene.Make(TypeIdOf<VolumeTexture>());
    REQUIRE(scene.SetSource2D(volume, atlas, 0, 0));
    REQUIRE(scene.Rebuild(volume, Source2D, sink));
    const Texture* made = scene.Read(volume);
    REQUIRE(made != nullptr);
    const auto* typed = static_cast<const VolumeTexture*>(made);
    CHECK(typed->source2DTileSizeX == 4 && typed->source2DTileSizeY == 4 &&
          made->source.sizeX == 4 && made->source.sizeY == 4 && made->source.numSlices == 4);
    auto bytes = sink.bulk.ReadBulk(made->source.bulk);
    REQUIRE(bytes && bytes.Value().Size() == 64);
    // 타일 1(오른쪽 위)의 첫 texel = 원본 (4, 0) = 4, 타일 2(왼쪽 아래)의 첫 texel = (0, 4) = 32.
    CHECK(bytes.Value()[0] == 0 && bytes.Value()[16] == 4 && bytes.Value()[32] == 32 &&
          bytes.Value()[16 + 5] == 4 + 8 + 1);

    const ObjectHandle odd = scene.Element(sink, plane, TextureSourceFormat::G8, 10, 27, 1, 0);
    REQUIRE(scene.SetSource2D(volume, odd, 0, 0));
    REQUIRE(scene.Rebuild(volume, Source2D, sink));
    made = scene.Read(volume);
    typed = static_cast<const VolumeTexture*>(made);
    CHECK(typed->source2DTileSizeX == 5 && typed->source2DTileSizeY == 13 &&
          made->source.numSlices == 4);

    // 명시 타일(대입한 타일은 다시 정하지 않는다).
    REQUIRE(scene.SetSource2D(volume, atlas, 2, 2));
    const StringView explicitFields[] = {"source2DTexture", "source2DTileSizeX"};
    REQUIRE(scene.Rebuild(volume, explicitFields, sink));
    CHECK(scene.Read(volume)->source.numSlices == 16);

    // 512 상한(64×64를 2×2 타일 → 1024장).
    const ObjectHandle big = scene.Element(sink, plane, TextureSourceFormat::G8, 64, 64, 1, 0);
    REQUIRE(scene.SetSource2D(volume, big, 2, 2));
    const StringView tileField[] = {"source2DTileSizeY"};
    REQUIRE(scene.Rebuild(volume, tileField, sink));
    CHECK(scene.Read(volume)->source.numSlices == 512);

    // 타일 0·음수 → 빈 원본 + 경고.
    const int32 tiles[] = {0, -3};
    for (int32 tile : tiles) {
        sink.warnings.Clear();
        REQUIRE(scene.SetSource2D(volume, atlas, tile, 4));
        REQUIRE(scene.Rebuild(volume, tileField, sink));
        CHECK(scene.Read(volume)->source.bulk.IsEmpty() && sink.warnings.Size() == 1 &&
              sink.warnings[0].code == AssetDiagnosticCode::SourceCleared);
    }

    // Propagate는 지금 타일 그대로(2×2 — 기본 4×4로 바꾸지 않는다).
    REQUIRE(scene.SetSource2D(volume, atlas, 2, 2));
    sink.cause = AssetSourceRebuildCause::Propagate;
    sink.changed = atlas;
    REQUIRE(scene.Rebuild(volume, {}, sink));
    CHECK(scene.Read(volume)->source.numSlices == 16);

    // asset-new 기본 원본.
    sink.cause = AssetSourceRebuildCause::New;
    sink.changed = ObjectHandle{};
    const ObjectHandle fresh = scene.Make(TypeIdOf<VolumeTexture>());
    REQUIRE(scene.Rebuild(fresh, {}, sink));
    const Texture* placeholder = scene.Read(fresh);
    REQUIRE(placeholder != nullptr);
    CHECK(placeholder->source.format == TextureSourceFormat::RGBA8 &&
          placeholder->source.sizeX == 1 && placeholder->source.numSlices == 1);
    auto texel = sink.bulk.ReadBulk(placeholder->source.bulk);
    REQUIRE(texel && texel.Value().Size() == 4);
    CHECK(texel.Value()[0] == 0 && texel.Value()[3] == 255);

    // 임포트한 Volume: 원본 2D가 없으면 타일 대입이 원본을 지우지 않고, 원본 2D 대입은 거부한다.
    sink.cause = AssetSourceRebuildCause::Edit;
    sink.importRecord = true;
    REQUIRE(scene.Rebuild(fresh, tileField, sink));
    CHECK(!scene.Read(fresh)->source.bulk.IsEmpty());
    REQUIRE(scene.SetSource2D(fresh, atlas, 0, 0));
    sink.failCode = AssetDiagnosticCode::None;
    CHECK(scene.Rebuild(fresh, Source2D, sink).Error() == ErrorCode::InvalidArgument &&
          sink.failCode == AssetDiagnosticCode::InvalidSetting);
    sink.importRecord = false;
}

} // namespace texture_source_rebuild_tests_file
