#include <Texture/Texture.generated.h>
#include <Texture/Texture.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <GraphicsTypes/PixelFormat.h>
#include <Object/ModuleRegistration.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ClassDefaults.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>
#include <Texture/TextureSettingsCheck.h>

// 텍스처 타입 시험이다(Docs/Design/AssetSystem 12.8절 A5d): 등록 형태(상속·도구 이름·EditorOnly·
// 추상), 기본값 = UE 생성자 값, enum 값 = UE 값과 숨김·접두, 검증 hook, 그래프 왕복(에디터·쿠킹
// 정책), 기본값과 다른 위치(FindChangedFields), 설정 판정(CheckTextureSettings — 계약·영향 조건·값
// 판정·숨김 값).
using namespace rocks;
namespace texture_type_tests_file {

using graphics::PixelFormat;

namespace {

struct TextureFixture {
    explicit TextureFixture(Allocator& allocator) noexcept
        : types(allocator), registration(types, allocator) {
        REQUIRE(RegisterTextureTypes(registration));
    }

    ~TextureFixture() noexcept {
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    TypeRegistry types;
    ModuleRegistration registration;
};

const FieldSchema* FindField(const TypeSchema& schema, StringView name) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return &field;
        }
    }

    return nullptr;
}

template <class Edit>
void EditTexture(ObjectRegistry& objects, ObjectHandle handle, Edit&& edit) noexcept {
    auto borrowed = objects.Resolve<Texture2D>(objects.OwnerAccess(), handle);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    edit(*value.Value());
}

// path의 도구 이름 경로 텍스트가 expected인지다(최상위 또는 멤버 한 단계).
bool PathIs(const TypeLease& lease, const PropertyPathBuffer& path, StringView expected) noexcept {
    PropertyPathBuffer parsed;
    if (!ParsePropertyPath(lease, expected, parsed) || parsed.count != path.count) {
        return false;
    }

    for (usize index = 0; index < path.count; ++index) {
        if (!(parsed.steps[index].fieldId == path.steps[index].fieldId)) {
            return false;
        }
    }

    return true;
}

} // namespace

// 등록 형태: Texture는 추상 StreamableRenderAsset 파생, 차원 클래스 다섯은 구체 클래스다. 도구
// 이름은 C++ camelCase 이름(UE 이름은 노출하지 않는다), EditorOnly는 UE WITH_EDITORONLY_DATA
// 구간과 같다.
TEST(Texture_RegistrationShape) {
    SystemAllocator allocator;
    TextureFixture fixture(allocator);
    auto texture = fixture.types.Acquire(TypeIdOf<Texture>());
    auto streamable = fixture.types.Acquire(TypeIdOf<StreamableRenderAsset>());
    REQUIRE(texture && streamable);
    CHECK(HasFlag(texture.Value().Schema().flags, TypeFlags::Abstract));
    CHECK(HasFlag(streamable.Value().Schema().flags, TypeFlags::Abstract));
    CHECK(texture.Value().Schema().baseType == TypeIdOf<StreamableRenderAsset>());
    const TypeId concrete[] = {TypeIdOf<Texture2D>(), TypeIdOf<TextureCube>(),
                               TypeIdOf<Texture2DArray>(), TypeIdOf<TextureCubeArray>(),
                               TypeIdOf<VolumeTexture>()};
    for (TypeId type : concrete) {
        auto lease = fixture.types.Acquire(type);
        REQUIRE(lease);
        CHECK(!HasFlag(lease.Value().Schema().flags, TypeFlags::Abstract));
        CHECK(lease.Value().IsA(TypeIdOf<Texture>()));
        CHECK(lease.Value().Ops().completeFieldLayout);
    }

    auto texture2D = fixture.types.Acquire(TypeIdOf<Texture2D>());
    REQUIRE(texture2D);
    const TypeSchema& schema = texture2D.Value().Schema();
    const uint32 editorOnly = static_cast<uint32>(FieldFlags::EditorOnly);
    const StringView editorFields[] = {"source",
                                       "adjustBrightness",
                                       "mipGenSettings",
                                       "compositeTexture",
                                       "sourceColorSettings",
                                       "useLegacyGamma",
                                       "oodleTextureSdkVersion"};
    for (StringView name : editorFields) {
        const FieldSchema* field = FindField(schema, name);
        REQUIRE(field != nullptr);
        CHECK(HasFlag(field->flags, FieldFlags::EditorOnly));
    }

    const StringView runtimeFields[] = {"lodGroup",    "compressionSettings",   "srgb",
                                        "filter",      "platformData",          "addressX",
                                        "neverStream", "numCinematicMipLevels", "downscale"};
    for (StringView name : runtimeFields) {
        const FieldSchema* field = FindField(schema, name);
        REQUIRE(field != nullptr);
        CHECK((field->flags & editorOnly) == 0);
    }

    // 도구 이름은 C++ 이름(camelCase)이다 — UE 이름(PascalCase·b 접두)은 노출하지 않는다.
    CHECK(FindField(schema, "LODGroup") == nullptr && FindField(schema, "bNoTiling") == nullptr);
    CHECK(FindField(schema, "streamingHandle") == nullptr);
    auto volume = fixture.types.Acquire(TypeIdOf<VolumeTexture>());
    auto array = fixture.types.Acquire(TypeIdOf<Texture2DArray>());
    REQUIRE(volume && array);
    const FieldSchema* sources = FindField(array.Value().Schema(), "sourceTextures");
    REQUIRE(sources != nullptr);
    CHECK(sources->value.kind == ValueKind::Array &&
          HasFlag(sources->flags, FieldFlags::EditorOnly));
    CHECK(FindField(volume.Value().Schema(), "addressMode") != nullptr &&
          FindField(volume.Value().Schema(), "source2DTexture") != nullptr);
}

// 기본값 = UE 생성자 값(Texture.cpp 148행, 색도 좌표 0 — 262행), PaddingColor 알파 255, SRGB 1,
// 주소 Wrap. 레코드도 같은 값이다(그래프 저장 기준 — 바꾸지 않은 객체는 차이가 없다).
TEST(Texture_DefaultsMatchUe) {
    SystemAllocator allocator;
    TextureFixture fixture(allocator);
    {
        ObjectRegistry objects(allocator, fixture.types, {0x5458, 1});
        auto created = objects.NewObject<Texture2D>(objects.OwnerAccess(), TypeIdOf<Texture2D>());
        REQUIRE(created);
        const ObjectHandle handle = created.Value().Handle();
        EditTexture(objects, handle, [](Texture2D& texture) noexcept {
            CHECK(texture.adjustBrightness == 1.0f && texture.adjustSaturation == 1.0f &&
                  texture.adjustMaxAlpha == 1.0f && texture.adjustHue == 0.0f);
            CHECK(texture.chromaKeyThreshold == 1.0f / 255.0f);
            CHECK(texture.chromaKeyColor == Color(255, 0, 255, 255));
            CHECK(texture.paddingColor == Color(0, 0, 0, 255));
            CHECK(texture.compositeTextureMode == TextureCompositeMode::NormalRoughnessToAlpha);
            CHECK(texture.compositePower == 1.0f);
            CHECK(texture.srgb == 1 && texture.normalizeNormals == 0);
            CHECK(texture.mipGenSettings == TextureMipGenSettings::FromTextureGroup);
            CHECK(texture.virtualTextureStreamingPriority ==
                  VirtualTextureProducerPriority::Normal);
            CHECK(texture.sourceColorSettings.chromaticAdaptationMethod ==
                  TextureChromaticAdaptationMethod::Bradford);
            CHECK(texture.sourceColorSettings.redChromaticityCoordinate.x == 0.0 &&
                  texture.sourceColorSettings.whiteChromaticityCoordinate.y == 0.0);
            CHECK(texture.addressX == TextureAddress::Wrap &&
                  texture.borderColor == TextureBorderColor::TransparentBlack);
            CHECK(texture.source.numMips == 1 && texture.source.numLayers == 1);
            CHECK(texture.streamingHandle == StreamableRenderAsset::NoStreamingHandle);
        });
        Array<PropertyPathBuffer> changed(allocator);
        REQUIRE(FindChangedFields(objects, objects.OwnerAccess(), handle, allocator, changed));
        CHECK(changed.IsEmpty());
        created.Value().Reset();
        CHECK(objects.Shutdown(objects.OwnerAccess()));
    }
}

// enum 값 = UE 값(TextureDefines.h), 숨김 열거자, UE 접두 없음.
TEST(Texture_EnumValues) {
    CHECK(static_cast<uint32>(TextureGroup::EightBitData) == 32);
    CHECK(static_cast<uint32>(TextureGroup::Project01) == 34);
    CHECK(static_cast<uint32>(TextureGroup::Project48) == 81);
    CHECK(static_cast<uint32>(TextureMipGenSettings::NoMipmaps) == 13);
    CHECK(static_cast<uint32>(TextureMipGenSettings::Angular) == 21);
    CHECK(static_cast<uint32>(TextureCompressionSettings::LQ) == 13);
    CHECK(static_cast<uint32>(TextureCompressionSettings::HDR_F32) == 16);
    CHECK(static_cast<uint32>(TextureSourceEncoding::HLG) == 17);
    CHECK(static_cast<uint32>(TextureColorSpace::Custom) == 99);
    CHECK(static_cast<uint32>(TextureDownscaleOptions::Sharpen10) == 13);
    CHECK(static_cast<uint32>(VirtualTextureProducerPriority::Highest) == 7);
    SystemAllocator allocator;
    TextureFixture fixture(allocator);
    auto compression = fixture.types.AcquireValueType(
        GeneratedType<TextureCompressionSettings>::Id(), TypeKind::Enum);
    REQUIRE(compression);
    const EnumSchema& schema = compression.Value().Enum();
    // UE 접두(TC_)는 받지 않는다(ToolPrefix 없음).
    CHECK(schema.toolPrefix.Size() == 0);
    usize hidden = 0;
    for (const EnumeratorSchema& value : schema.values) {
        if (HasFlag(value.flags, EnumeratorFlags::Hidden)) {
            ++hidden;
            CHECK(value.value == 13 || value.value == 14);
        }
    }

    CHECK(hidden == 2);
}

// 검증 hook: TextureSource는 형식이 있으면 크기·slice가 0이 아니어야 하고, TexturePlatformData는
// PixelFormat 0(쿠킹 전)이거나 알려진 비압축 색 형식이어야 한다.
TEST(Texture_ValidationHooks) {
    TextureSource source;
    CHECK(TextureSource::Validate(source));
    source.format = TextureSourceFormat::RGBA8;
    CHECK(!TextureSource::Validate(source));
    source.sizeX = 4;
    source.sizeY = 4;
    source.numSlices = 1;
    CHECK(TextureSource::Validate(source));
    source.numSlices = 0;
    CHECK(!TextureSource::Validate(source));

    SystemAllocator allocator;
    TexturePlatformData data(allocator);
    CHECK(TexturePlatformData::Validate(data));
    data.pixelFormat = static_cast<uint8>(graphics::PixelFormat::RGBA8Unorm);
    CHECK(TexturePlatformData::Validate(data));
    // 블록 압축 색은 받고(AssetSystem 12.17.5), 깊이 형식은 거부한다.
    data.pixelFormat = static_cast<uint8>(graphics::PixelFormat::BC7Unorm);
    CHECK(TexturePlatformData::Validate(data));
    data.pixelFormat = static_cast<uint8>(graphics::PixelFormat::D32Float);
    CHECK(!TexturePlatformData::Validate(data));
    data.pixelFormat = 0xFF;
    CHECK(!TexturePlatformData::Validate(data));
}

// 기본값과 다른 위치: 최상위 단일 값, 비Atomic struct 잎(SourceColorSettings.ColorSpace), Atomic
// struct 통째(ChromaKeyColor), 배열 통째(LayerFormatSettings), non-null 참조(그래프 밖 대상 —
// CompositeTexture), 스키마 순서.
TEST(Texture_FindChangedFields) {
    SystemAllocator allocator;
    TextureFixture fixture(allocator);
    {
        ObjectRegistry objects(allocator, fixture.types, {0x5458, 2});
        const ObjectAccessContext access = objects.OwnerAccess();
        auto created = objects.NewObject<Texture2D>(access, TypeIdOf<Texture2D>());
        auto other = objects.NewObject<Texture2D>(access, TypeIdOf<Texture2D>());
        REQUIRE(created && other);
        const ObjectHandle handle = created.Value().Handle();
        EditTexture(objects, handle, [&](Texture2D& texture) noexcept {
            texture.lodBias = 2;
            texture.sourceColorSettings.colorSpace = TextureColorSpace::sRGB;
            texture.chromaKeyColor = Color(1, 2, 3, 4);
            CHECK(texture.layerFormatSettings.TryPushBack(TextureFormatSettings{}));
            CHECK(objects.SetReference(access, handle, texture.compositeTexture,
                                       other.Value().Handle()));
        });
        Array<PropertyPathBuffer> changed(allocator);
        REQUIRE(FindChangedFields(objects, access, handle, allocator, changed));
        auto lease = fixture.types.Acquire(TypeIdOf<Texture2D>());
        REQUIRE(lease);
        // 스키마 필드 순이다(선언 순이 아니다 — TypeSchema fields 배열 순).
        const StringView expected[] = {"lodBias", "chromaKeyColor",
                                       "sourceColorSettings.colorSpace", "compositeTexture",
                                       "layerFormatSettings"};
        REQUIRE(changed.Size() == sizeof(expected) / sizeof(expected[0]));
        for (usize index = 0; index < changed.Size(); ++index) {
            CHECK(PathIs(lease.Value(), changed[index], expected[index]));
        }

        created.Value().Reset();
        other.Value().Reset();
        CHECK(objects.Shutdown(objects.OwnerAccess()));
    }
}

// 설정 판정: 기본값은 이슈 없음. UE 새 텍스처 설정 값(CompositeTextureMode Disabled·
// AlphaCoverageThresholds 0.75·NormalizeNormals·UseNewMipFilter·OodleTextureSdkVersion 기록)만
// 바꾸면 이슈 없음(영향 조건 거짓·기록 필드). 조건이 참이면 같은 필드가 이슈다. 값 판정(MipGen
// Sharpen·EncodingOverride·ColorSpace·OnlyFirstMip)은 UnsupportedSetting, 숨김 압축 설정은
// InvalidSetting, 순서는 스키마 순서다.
TEST(Texture_CheckSettings) {
    SystemAllocator allocator;
    TextureFixture fixture(allocator);
    {
        ObjectRegistry objects(allocator, fixture.types, {0x5458, 3});
        const ObjectAccessContext access = objects.OwnerAccess();
        auto created = objects.NewObject<Texture2D>(access, TypeIdOf<Texture2D>());
        auto other = objects.NewObject<Texture2D>(access, TypeIdOf<Texture2D>());
        REQUIRE(created && other);
        const ObjectHandle handle = created.Value().Handle();
        auto lease = fixture.types.Acquire(TypeIdOf<Texture2D>());
        REQUIRE(lease);
        Array<TextureSettingIssue> issues(allocator);
        REQUIRE(CheckTextureSettings(objects, access, handle, allocator, issues));
        CHECK(issues.IsEmpty());

        // UE 새 텍스처 설정(SetModernSettingsForNewOrChangedTexture).
        EditTexture(objects, handle, [](Texture2D& texture) noexcept {
            texture.compositeTextureMode = TextureCompositeMode::Disabled;
            texture.alphaCoverageThresholds = Vec4d{0.0, 0.0, 0.0, 0.75};
            texture.normalizeNormals = 1;
            texture.useNewMipFilter = true;
            texture.chromaKeyThreshold = 0.5f;
            texture.sourceColorSettings.redChromaticityCoordinate = Vec2d{0.64, 0.33};
            texture.virtualTexturePrefetchMips = 3;
        });
        REQUIRE(CheckTextureSettings(objects, access, handle, allocator, issues));
        CHECK(issues.IsEmpty());

        // 영향 조건이 참이면 이슈다.
        EditTexture(objects, handle, [&](Texture2D& texture) noexcept {
            texture.chromaKeyTexture = true;
            texture.doScaleMipsForAlphaCoverage = true;
            CHECK(objects.SetReference(access, handle, texture.compositeTexture,
                                       other.Value().Handle()));
        });
        REQUIRE(CheckTextureSettings(objects, access, handle, allocator, issues));
        // 스키마 필드 순이다(선언 순이 아니다 — TypeSchema fields 배열 순).
        const StringView conditioned[] = {
            "doScaleMipsForAlphaCoverage", "compositeTextureMode", "compositeTexture",
            "chromaKeyThreshold",          "chromaKeyTexture",     "alphaCoverageThresholds"};
        REQUIRE(issues.Size() == sizeof(conditioned) / sizeof(conditioned[0]));
        for (usize index = 0; index < issues.Size(); ++index) {
            CHECK(issues[index].code == AssetDiagnosticCode::UnsupportedSetting);
            CHECK(PathIs(lease.Value(), issues[index].path, conditioned[index]));
        }

        // 값 판정과 숨김 값(새 객체).
        auto values = objects.NewObject<Texture2D>(access, TypeIdOf<Texture2D>());
        REQUIRE(values);
        EditTexture(objects, values.Value().Handle(), [](Texture2D& texture) noexcept {
            texture.mipGenSettings = TextureMipGenSettings::Sharpen4;
            texture.sourceColorSettings.encodingOverride = TextureSourceEncoding::Gamma22;
            texture.sourceColorSettings.colorSpace = TextureColorSpace::Rec2020;
            texture.mipLoadOptions = TextureMipLoadOptions::OnlyFirstMip;
            texture.compressionSettings = TextureCompressionSettings::LQ;
        });
        REQUIRE(CheckTextureSettings(objects, access, values.Value().Handle(), allocator, issues));
        // 스키마 필드 순이다. compressionSettings(LQ)만 InvalidSetting이다.
        const StringView valued[] = {"compressionSettings", "sourceColorSettings.encodingOverride",
                                     "sourceColorSettings.colorSpace", "mipLoadOptions",
                                     "mipGenSettings"};
        REQUIRE(issues.Size() == sizeof(valued) / sizeof(valued[0]));
        for (usize index = 0; index < issues.Size(); ++index) {
            CHECK(PathIs(lease.Value(), issues[index].path, valued[index]));
            CHECK(issues[index].code == (index == 0 ? AssetDiagnosticCode::InvalidSetting
                                                    : AssetDiagnosticCode::UnsupportedSetting));
        }

        // Custom 색 공간이면 색도 좌표가 계약으로 판정된다.
        EditTexture(objects, values.Value().Handle(), [](Texture2D& texture) noexcept {
            texture.mipGenSettings = TextureMipGenSettings::FromTextureGroup;
            texture.sourceColorSettings.encodingOverride = TextureSourceEncoding::None;
            texture.sourceColorSettings.colorSpace = TextureColorSpace::Custom;
            texture.sourceColorSettings.redChromaticityCoordinate = Vec2d{0.64, 0.33};
            texture.mipLoadOptions = TextureMipLoadOptions::Default;
            texture.compressionSettings = TextureCompressionSettings::Default;
        });
        REQUIRE(CheckTextureSettings(objects, access, values.Value().Handle(), allocator, issues));
        REQUIRE(issues.Size() == 2);
        CHECK(PathIs(lease.Value(), issues[0].path, "sourceColorSettings.colorSpace"));
        CHECK(
            PathIs(lease.Value(), issues[1].path, "sourceColorSettings.redChromaticityCoordinate"));
        values.Value().Reset();
        created.Value().Reset();
        other.Value().Reset();
        CHECK(objects.Shutdown(objects.OwnerAccess()));
    }
}

// 그래프 왕복: 에디터 정책은 EditorOnly 포함(바꾼 값·원본 크기가 돌아온다), 쿠킹 정책은
// EditorOnly를 빼므로 로드한 객체는 그 필드가 기본값이다. SourceTextures(ObjectRefArray) 왕복.
TEST(Texture_GraphRoundTrip) {
    SystemAllocator allocator;
    TextureFixture fixture(allocator);
    {
        ObjectRegistry source(allocator, fixture.types, {0x5458, 4});
        const ObjectAccessContext access = source.OwnerAccess();
        auto created = source.NewObject<Texture2D>(access, TypeIdOf<Texture2D>());
        REQUIRE(created);
        EditTexture(source, created.Value().Handle(), [](Texture2D& texture) noexcept {
            texture.lodGroup = TextureGroup::Project48;
            texture.compressionSettings = TextureCompressionSettings::HDR;
            texture.srgb = 0;
            texture.source.format = TextureSourceFormat::RGBA16F;
            texture.source.sizeX = 8;
            texture.source.sizeY = 4;
            texture.source.numSlices = 1;
            texture.maxTextureSize = 512;
        });
        const uint32 policies[] = {0, static_cast<uint32>(FieldFlags::EditorOnly)};
        for (uint32 excluded : policies) {
            ObjectGraphSaveOptions options;
            options.policy.excludedFlags = excluded;
            auto saved =
                SaveObjectGraph(allocator, source, access, created.Value().Handle(), options);
            REQUIRE(saved);
            const Array<uint8> bytes = Move(saved.Value());
            ObjectRegistry target(allocator, fixture.types, {0x5458, 5});
            auto loaded = LoadObjectGraph(allocator, target, fixture.types, target.OwnerAccess(),
                                          bytes.View());
            REQUIRE(loaded);
            {
                auto borrowed =
                    target.Resolve<Texture2D>(target.OwnerAccess(), loaded.Value().Handle());
                REQUIRE(borrowed);
                auto value = borrowed.Value().Get();
                REQUIRE(value);
                const Texture2D& texture = *value.Value();
                CHECK(texture.lodGroup == TextureGroup::Project48);
                CHECK(texture.compressionSettings == TextureCompressionSettings::HDR);
                CHECK(texture.srgb == 0);
                if (excluded == 0) {
                    CHECK(texture.source.format == TextureSourceFormat::RGBA16F &&
                          texture.source.sizeX == 8 && texture.maxTextureSize == 512);
                } else {
                    CHECK(texture.source.format == TextureSourceFormat::Invalid &&
                          texture.maxTextureSize == 0);
                }
            }

            loaded.Value().Reset();
            CHECK(target.Shutdown(target.OwnerAccess()));
        }

        created.Value().Reset();
        CHECK(source.Shutdown(source.OwnerAccess()));
    }
}

} // namespace texture_type_tests_file
