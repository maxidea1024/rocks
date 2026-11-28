#include "ExchangeTestSupport.h"

#include <Core/Memory/SystemAllocator.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <ReflectionText/TextJson.h>
#include <TestSupport/Test.h>
#include <Texture/Texture.generated.h>
#include <Texture/Texture.h>
#include <UnrealExchange/ExchangeTextureSettings.h>

// 설정 번역 시험이다(Docs/Design/UnrealExchange 10.6절 8번, 4.6절 3번): 처리 종류마다 양성·음성
// (bool 접두, enum 접두, Guid 순서, PerPlatform, 참조·참조 배열, struct·struct 배열, Ignore,
// Unsupported 기본값·비기본값, 모르는 키, 클래스 밖 키, 행 묶음·Flatten·EnumMap의 Rocks에 없는 값 —
// UX4b1a), 텍스처 표 행 수, 그리고 UE 5.8 엔진 콘텐츠
// 텍스처 20개의 설정(Tests/Settings/EngineTextureSettings.json — AssetImportData 본문을 뺀 사본)이
// 경고 없이 번역되고 ImportJson을 통과하는지.
using namespace rocks;
using namespace rocks::exchange_test;
namespace exchange_settings_tests_file {

namespace {

using Kind = ExchangeSettingKind;

constexpr ExchangeSettingRow MemberRows[] = {
    {"Mode", Kind::Enum, 0, nullptr, 0, {}, {}},
    {"bOn", Kind::Same, 0, nullptr, 0, {}, {}},
};

constexpr ExchangeSettingRow SampleRows[] = {
    {"bFlag", Kind::Same, 0, nullptr, 0, {}, {}},
    {"Plain", Kind::Same, 0, nullptr, 0, {}, {}},
    {"Group", Kind::Enum, 0, nullptr, 0, {}, {}},
    {"Id", Kind::Guid, 0, nullptr, 0, {}, {}},
    {"Scale", Kind::PerPlatformFloat, 0, nullptr, 0, {}, {}},
    {"Ref", Kind::Reference, 0, nullptr, 0, {}, {}},
    {"Refs", Kind::ReferenceArray, 0, nullptr, 0, {}, {}},
    {"Color", Kind::Struct, 0, MemberRows, 2, {}, {}},
    {"Layers", Kind::StructArray, 0, MemberRows, 2, {}, {}},
    {"Lighting", Kind::Ignore, 0, nullptr, 0, {}, {}},
    {"UserData", Kind::Unsupported, 0, nullptr, 0, "[]", {}},
    {"Only2D", Kind::Same, ExchangeClassBit(ExchangeClass::Texture2D), nullptr, 0, {}, {}},
    {"BadDefault", Kind::Unsupported, 0, nullptr, 0, "{", {}},
};

// settings 원문을 rows로 compact 번역해 out에 쓴다.
Result<void>
Translate(Allocator& allocator, StringView settings, ExchangeClass output,
          const OrderedMap<String, AssetId>* references, StringBuilder<>& out,
          Array<ExchangeSettingWarning>& warnings,
          Span<const ExchangeSettingRow> rows = Span<const ExchangeSettingRow>(SampleRows),
          Span<const StringView> groups = Span<const StringView>()) noexcept {
    JsonDocument document(allocator);
    if (!document.Parse(settings).Succeeded()) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    JsonWriterOptions options;
    options.compact = true;
    out.Clear();
    warnings.Clear();
    JsonWriter writer(out, options);
    ExchangeSettingInput input;
    input.settings = document.Root();
    input.rows = rows;
    input.output = output;
    input.references = references;
    input.groups = groups;
    if (auto translated = TranslateExchangeSettings(input, writer, warnings); !translated) {
        return translated;
    }

    // 경고 key는 문서를 가리키므로 문서가 사라지기 전에 detail만 남긴다.
    for (ExchangeSettingWarning& warning : warnings) {
        warning.key = StringView();
    }

    return writer.Finish();
}

TypeId TextureTypeOf(ExchangeClass type) noexcept {
    switch (type) {
    case ExchangeClass::Texture2D:
        return TypeIdOf<Texture2D>();
    case ExchangeClass::TextureCube:
        return TypeIdOf<TextureCube>();
    case ExchangeClass::Texture2DArray:
        return TypeIdOf<Texture2DArray>();
    case ExchangeClass::TextureCubeArray:
        return TypeIdOf<TextureCubeArray>();
    default:
        return TypeIdOf<VolumeTexture>();
    }
}

// 텍스처 타입과 Name pool을 가진 산 객체 환경이다.
struct TextureEnv {
    explicit TextureEnv(Allocator& allocator) noexcept
        : types(allocator), modules(types, allocator), names(allocator),
          objects(allocator, types, ContextId{0x5845ull, 1}, Options(names)),
          access(objects.OwnerAccess()), diagnostics(allocator) {
        registered = static_cast<bool>(RegisterTextureTypes(modules));
    }

    ~TextureEnv() {
        CHECK(objects.Shutdown(access));
        CHECK(names.Shutdown());
        CHECK(modules.Unregister());
        CHECK(types.Shutdown());
    }

    static RegistryOptions Options(NamePool& pool) noexcept {
        (void)pool.Initialize();
        RegistryOptions options;
        options.names = &pool;
        return options;
    }

    TypeRegistry types;
    ModuleRegistration modules;
    NamePool names;
    ObjectRegistry objects;
    ObjectAccessContext access;
    reflection_text::TextDiagnostics diagnostics;
    bool registered = false;
};

} // namespace

// 처리 종류마다의 번역(모든 키를 한 문서로 — 결과는 compact 원문 대조).
TEST(UnrealExchange_SettingsTranslateKinds) {
    SystemAllocator allocator;
    {
        OrderedMap<String, AssetId> references(allocator);
        String known(allocator);
        REQUIRE(known.TryAssign("/Game/T/Known.Known"));
        REQUIRE(references.Insert(Move(known), AssetId{Guid{1, 2}}));
        constexpr StringView Settings = R"({
"bFlag": true, "Plain": {"B": 1, "G": 2, "R": 3, "A": 4}, "Group": "TEXTUREGROUP_ProcBuilding_Face",
"Id": {"A": -1, "B": 305419896, "C": -2023406815, "D": 1},
"Scale": {"Default": 0.5, "PerPlatform": []},
"Ref": {"objectPath": "/Game/T/Known.Known"},
"Refs": [{"objectPath": "/Game/T/Known.Known"}, null, {"objectPath": "/Game/T/Gone.Gone"}],
"Color": {"Mode": "TSE_None", "bOn": false},
"Layers": [{"Mode": "TC_HDR_Compressed", "bOn": true}, {"Mode": 7}],
"Lighting": {"A": 1}, "UserData": [], "Only2D": 3})";
        StringBuilder<> out(allocator);
        Array<ExchangeSettingWarning> warnings(allocator);
        REQUIRE(
            Translate(allocator, Settings, ExchangeClass::Texture2D, &references, out, warnings));
        CHECK(out.View() ==
              StringView(
                  R"({"Flag":true,"Plain":{"B":1,"G":2,"R":3,"A":4},"Group":"ProcBuilding_Face",)"
                  R"("Id":"ffffffff-1234-5678-8765-432100000001","Scale":{"defaultValue":0.5},)"
                  R"("Ref":"/Game/T/Known.Known","Refs":["/Game/T/Known.Known"],)"
                  R"("Color":{"Mode":"None","On":false},)"
                  R"("Layers":[{"Mode":"HDR_Compressed","On":true},{"Mode":7}],"Only2D":3})"
                  "\n"));
        CHECK(warnings.IsEmpty());

        // 클래스 밖 키는 경고 없이 버린다.
        REQUIRE(Translate(allocator, R"({"Only2D": 3})", ExchangeClass::VolumeTexture, &references,
                          out, warnings));
        CHECK(out.View() == StringView("{}\n") && warnings.IsEmpty());

        // references가 없으면 참조는 모두 null(배열은 비운다).
        REQUIRE(Translate(allocator,
                          R"({"Ref": {"objectPath": "/Game/T/Known.Known"}, "Refs": [null]})",
                          ExchangeClass::Texture2D, nullptr, out, warnings));
        CHECK(out.View() == StringView(R"({"Ref":null,"Refs":[]})"
                                       "\n"));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// enum 접두 규칙: 첫 `_`까지 대문자·숫자일 때만 뗀다(소문자가 낀 접두·앞이나 끝의 `_`·접두 없음은
// 그대로).
TEST(UnrealExchange_SettingsEnumPrefix) {
    SystemAllocator allocator;
    {
        const ExchangeSettingRow rows[] = {{"E", Kind::Enum, 0, nullptr, 0, {}, {}}};
        StringBuilder<> out(allocator);
        Array<ExchangeSettingWarning> warnings(allocator);
        struct Case {
            StringView in;
            StringView out;
        };
        const Case cases[] = {
            {R"({"E": "TC_Default"})", "{\"E\":\"Default\"}\n"},
            {R"({"E": "TMGS2_Sharpen4"})", "{\"E\":\"Sharpen4\"}\n"},
            {R"({"E": "Default"})", "{\"E\":\"Default\"}\n"},
            {R"({"E": "Tc_Default"})", "{\"E\":\"Tc_Default\"}\n"},
            {R"({"E": "_Default"})", "{\"E\":\"_Default\"}\n"},
            {R"({"E": "TC_"})", "{\"E\":\"TC_\"}\n"},
            {R"({"E": "HDR_F32"})", "{\"E\":\"F32\"}\n"},
        };
        for (const Case& item : cases) {
            REQUIRE(Translate(allocator, item.in, ExchangeClass::Texture2D, nullptr, out, warnings,
                              Span<const ExchangeSettingRow>(rows)));
            CHECK(out.View() == item.out);
        }

        // 문자열·숫자가 아닌 enum 값은 거부한다.
        CHECK(Translate(allocator, R"({"E": true})", ExchangeClass::Texture2D, nullptr, out,
                        warnings, Span<const ExchangeSettingRow>(rows))
                  .Error() == ErrorCode::CorruptData);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 경고: PerPlatform이 비지 않음, Unsupported 비기본값, 모르는 최상위
// 키(ExchangeUnknownSettingDetail), struct 안 모르는 멤버(struct 행 번호).
TEST(UnrealExchange_SettingsWarnings) {
    SystemAllocator allocator;
    {
        StringBuilder<> out(allocator);
        Array<ExchangeSettingWarning> warnings(allocator);
        REQUIRE(Translate(
            allocator,
            R"({"Scale": {"Default": 1, "PerPlatform": [{"key": "Android", "value": 0.5}]},
"UserData": [{"subobject": "X"}], "Mystery": 1, "Color": {"Mode": "A_B", "Extra": 1}})",
            ExchangeClass::Texture2D, nullptr, out, warnings));
        CHECK(out.View() == StringView(R"({"Scale":{"defaultValue":1},"Color":{"Mode":"B"}})"
                                       "\n"));
        REQUIRE(warnings.Size() == 4);
        CHECK(warnings[0].code == AssetDiagnosticCode::UnsupportedSetting &&
              warnings[0].detail == 4);
        CHECK(warnings[1].detail == 10 && warnings[2].detail == ExchangeUnknownSettingDetail &&
              warnings[3].detail == 7);

        // PerPlatform 밖의 모르는 멤버도 경고다(값은 Default만 옮긴다).
        REQUIRE(Translate(allocator, R"({"Scale": {"Default": 3, "Extra": 1}})",
                          ExchangeClass::Texture2D, nullptr, out, warnings));
        CHECK(out.View() == StringView(R"({"Scale":{"defaultValue":3}})"
                                       "\n") &&
              warnings.Size() == 1 && warnings[0].detail == 4);

        // PerPlatform 칸이 없어도 된다.
        REQUIRE(Translate(allocator, R"({"Scale": {"Default": 2}})", ExchangeClass::Texture2D,
                          nullptr, out, warnings));
        CHECK(out.View() == StringView(R"({"Scale":{"defaultValue":2}})"
                                       "\n") &&
              warnings.IsEmpty());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 형식 오류: settings가 객체가 아님, Guid 칸 수·이름·범위·종류, PerPlatform의 Default, 참조 형식,
// 참조 배열·struct 배열·struct 종류, Unsupported 기본값 텍스트 오류.
TEST(UnrealExchange_SettingsRejects) {
    SystemAllocator allocator;
    {
        const StringView bad[] = {
            "[]",
            R"({"Id": {"A": 1, "B": 2, "C": 3}})",
            R"({"Id": {"A": 1, "B": 2, "C": 3, "E": 4}})",
            R"({"Id": {"A": 1, "B": 2, "C": 3, "D": 4294967296}})",
            R"({"Id": {"A": 1, "B": 2, "C": 3, "D": "x"}})",
            R"({"Id": {"A": -2147483649, "B": 2, "C": 3, "D": 4}})",
            R"({"Id": {"A": 1.5, "B": 2, "C": 3, "D": 4}})",
            R"({"Scale": {"Default": 1, "PerPlatform": {}}})",
            R"({"Id": "00000000-0000-0000-0000-000000000000"})",
            R"({"Scale": {"PerPlatform": []}})",
            R"({"Scale": 1})",
            R"({"Ref": {"objectPath": 3}})",
            R"({"Ref": {"objectPath": "/Game/A.A", "class": "X"}})",
            R"({"Ref": "/Game/A.A"})",
            R"({"Refs": {}})",
            R"({"Refs": [3]})",
            R"({"Layers": {}})",
            R"({"Layers": [1]})",
            R"({"Color": []})",
        };
        StringBuilder<> out(allocator);
        Array<ExchangeSettingWarning> warnings(allocator);
        for (const StringView text : bad) {
            CHECK(Translate(allocator, text, ExchangeClass::Texture2D, nullptr, out, warnings)
                      .Error() == ErrorCode::CorruptData);
        }

        // 행 표의 기본값 텍스트가 JSON이 아니면 표의 결함이다(덤프 내용이 아니다).
        CHECK(Translate(allocator, R"({"BadDefault": 1})", ExchangeClass::Texture2D, nullptr, out,
                        warnings)
                  .Error() == ErrorCode::InvalidArgument);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 행 묶음·Flatten·EnumMap의 Rocks에 없는 값(UX4b1a — 10.6절 8번): 묶음 행은 최상위 키 뒤 묶음
// 객체 안에 settings 순서로 쓰고 빈 묶음도 쓴다, 모르는 키 경고는 한 번, Flatten은 멤버 표의 출력
// 키로 형제 키를 쓰고 모르는 멤버는 그 행 번호로 경고, Rocks에 없는 enum 값은 키를 버리고 경고,
// 묶음 번호가 묶음 이름 수를 넘거나 멤버 표에 묶음이 있으면 표의 결함, Flatten 값이 객체가 아니면
// CorruptData다.
TEST(UnrealExchange_SettingsGroupsFlattenUnmapped) {
    SystemAllocator allocator;
    {
        constexpr ExchangeSettingRow PointRows[] = {
            {"X", Kind::Same, 0, nullptr, 0, {}, "pointX", 0},
            {"Y", Kind::Same, 0, nullptr, 0, {}, "pointY", 0},
        };
        constexpr ExchangeSettingRow ModeValues[] = {
            {"M_One", Kind::Same, 0, nullptr, 0, {}, "One", 0},
            {"M_Gone", Kind::Same, 0, nullptr, 0, {}, {}, 0},
        };
        const ExchangeSettingRow rows[] = {
            {"Top", Kind::Same, 0, nullptr, 0, {}, {}, 0},
            {"Inner", Kind::Same, 0, nullptr, 0, {}, "inner", 1},
            {"Point", Kind::Flatten, 0, PointRows, 2, {}, {}, 1},
            {"Mode", Kind::EnumMap, 0, ModeValues, 2, {}, "mode", 0},
            {"Late", Kind::Same, 0, nullptr, 0, {}, {}, 1},
        };
        const StringView groups[] = {"first", "second"};
        const Span<const ExchangeSettingRow> table(rows);
        StringBuilder<> out(allocator);
        Array<ExchangeSettingWarning> warnings(allocator);
        REQUIRE(Translate(allocator,
                          R"({"Late": 4, "Point": {"X": 2, "Y": 3, "Z": 9}, "Top": 1,
"Mystery": 0, "Inner": 5, "Mode": "M_One"})",
                          ExchangeClass::Material, nullptr, out, warnings, table,
                          Span<const StringView>(groups)));
        CHECK(out.View() ==
              StringView(R"({"Top":1,"mode":"One","first":{"Late":4,"pointX":2,"pointY":3,)"
                         R"("inner":5},"second":{}})"
                         "\n"));
        REQUIRE(warnings.Size() == 2);
        // 경고는 최상위 순회(모르는 키) 뒤에 묶음 순회(Flatten의 모르는 멤버) 순서다.
        CHECK(warnings[0].detail == ExchangeUnknownSettingDetail && warnings[1].detail == 2);

        // Rocks에 없는 enum 값은 키를 쓰지 않고 그 행 번호로 경고한다. 표에 없는 값은
        // CorruptData다.
        REQUIRE(Translate(allocator, R"({"Mode": "M_Gone", "Top": 2})", ExchangeClass::Material,
                          nullptr, out, warnings, table, Span<const StringView>(groups)));
        CHECK(out.View() == StringView(R"({"Top":2,"first":{},"second":{}})"
                                       "\n"));
        REQUIRE(warnings.Size() == 1);
        CHECK(warnings[0].code == AssetDiagnosticCode::UnsupportedSetting &&
              warnings[0].detail == 3);
        CHECK(Translate(allocator, R"({"Mode": "M_Other"})", ExchangeClass::Material, nullptr, out,
                        warnings, table, Span<const StringView>(groups))
                  .Error() == ErrorCode::CorruptData);

        // Flatten 값이 객체가 아니면 덤프 내용 오류다.
        CHECK(Translate(allocator, R"({"Point": [1, 2]})", ExchangeClass::Material, nullptr, out,
                        warnings, table, Span<const StringView>(groups))
                  .Error() == ErrorCode::CorruptData);

        // 묶음 이름이 모자라면(묶음 1 행이 있는데 이름이 없음) 표의 결함이다.
        CHECK(Translate(allocator, R"({"Top": 1})", ExchangeClass::Material, nullptr, out, warnings,
                        table)
                  .Error() == ErrorCode::InvalidArgument);

        // 멤버 표의 행에 묶음이 있으면 표의 결함이다.
        constexpr ExchangeSettingRow GroupedMembers[] = {
            {"A", Kind::Same, 0, nullptr, 0, {}, {}, 1},
        };
        const ExchangeSettingRow structRows[] = {
            {"S", Kind::Struct, 0, GroupedMembers, 1, {}, {}, 0},
        };
        CHECK(Translate(allocator, R"({"S": {"A": 1}})", ExchangeClass::Material, nullptr, out,
                        warnings, Span<const ExchangeSettingRow>(structRows),
                        Span<const StringView>(groups))
                  .Error() == ErrorCode::InvalidArgument);

        // 묶음 안의 Unsupported(기본값은 조용히, 아닌 값은 경고 — 최상위 경고 뒤)와 묶음 행의
        // classes 필터(집합 밖 클래스면 경고 없이 버림), Flatten 멤버 EnumMap의 Rocks에 없는
        // 값(경고 — 그 Flatten 행 번호).
        constexpr ExchangeSettingRow PointModes[] = {
            {"P_On", Kind::Same, 0, nullptr, 0, {}, "On", 0},
            {"P_Gone", Kind::Same, 0, nullptr, 0, {}, {}, 0},
        };
        const ExchangeSettingRow pointMembers[] = {
            {"X", Kind::Same, 0, nullptr, 0, {}, "pointX", 0},
            {"Mode", Kind::EnumMap, 0, PointModes, 2, {}, "pointMode", 0},
        };
        const ExchangeSettingRow groupedRows[] = {
            {"Old", Kind::Unsupported, 0, nullptr, 0, "0", {}, 1},
            {"Only2D",
             Kind::Same,
             ExchangeClassBit(ExchangeClass::Texture2D),
             nullptr,
             0,
             {},
             {},
             2},
            {"Point", Kind::Flatten, 0, pointMembers, 2, {}, {}, 2},
            {"Top", Kind::Same, 0, nullptr, 0, {}, {}, 0},
        };
        REQUIRE(Translate(allocator,
                          R"({"Old": 0, "Only2D": 1, "Point": {"X": 1, "Mode": "P_On"}, "Top": 1,
"Mystery": 2})",
                          ExchangeClass::Material, nullptr, out, warnings,
                          Span<const ExchangeSettingRow>(groupedRows),
                          Span<const StringView>(groups)));
        CHECK(out.View() ==
              StringView(R"({"Top":1,"first":{},"second":{"pointX":1,"pointMode":"On"}})"
                         "\n"));
        REQUIRE(warnings.Size() == 1);
        CHECK(warnings[0].detail == ExchangeUnknownSettingDetail);
        REQUIRE(Translate(allocator, R"({"Point": {"Mode": "P_Gone"}, "Old": 3, "Top": 1})",
                          ExchangeClass::Texture2D, nullptr, out, warnings,
                          Span<const ExchangeSettingRow>(groupedRows),
                          Span<const StringView>(groups)));
        CHECK(out.View() == StringView(R"({"Top":1,"first":{},"second":{}})"
                                       "\n"));
        REQUIRE(warnings.Size() == 2);
        CHECK(warnings[0].detail == 0 && warnings[1].detail == 2);

        // Flatten 멤버가 값 하나를 쓰는 종류가 아니거나(Ignore) 묶음이 있으면 표의 결함이다.
        constexpr ExchangeSettingRow IgnoredMember[] = {
            {"X", Kind::Ignore, 0, nullptr, 0, {}, {}, 0},
        };
        constexpr ExchangeSettingRow GroupedMember[] = {
            {"X", Kind::Same, 0, nullptr, 0, {}, "x", 1},
        };
        const ExchangeSettingRow badFlatten[] = {
            {"A", Kind::Flatten, 0, IgnoredMember, 1, {}, {}, 0},
            {"B", Kind::Flatten, 0, GroupedMember, 1, {}, {}, 0},
        };
        CHECK(Translate(allocator, R"({"A": {"X": 1}})", ExchangeClass::Material, nullptr, out,
                        warnings, Span<const ExchangeSettingRow>(badFlatten))
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(Translate(allocator, R"({"B": {"X": 1}})", ExchangeClass::Material, nullptr, out,
                        warnings, Span<const ExchangeSettingRow>(badFlatten))
                  .Error() == ErrorCode::InvalidArgument);

        // 멤버 표의 Flatten도 표의 결함이다.
        const ExchangeSettingRow NestedFlatten[] = {
            {"P", Kind::Flatten, 0, PointRows, 2, {}, {}, 0},
        };
        const ExchangeSettingRow flattenRows[] = {
            {"F", Kind::Flatten, 0, NestedFlatten, 1, {}, {}, 0},
        };
        CHECK(Translate(allocator, R"({"F": {"P": {"X": 1}}})", ExchangeClass::Material, nullptr,
                        out, warnings, Span<const ExchangeSettingRow>(flattenRows))
                  .Error() == ErrorCode::InvalidArgument);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 텍스처 표: 행 수가 문서 표와 같고 UE 키가 겹치지 않는다.
TEST(UnrealExchange_TextureSettingRows) {
    const Span<const ExchangeSettingRow> rows = ExchangeTextureSettingRows();
    CHECK(rows.Size() == ExchangeTextureSettingRowCount);
    for (usize index = 0; index < rows.Size(); ++index) {
        for (usize other = 0; other < index; ++other) {
            CHECK(rows[index].ueKey != rows[other].ueKey);
        }
    }

    CHECK(rows[0].ueKey == "AdjustBrightness" && rows[35].ueKey == "CompressionSettings" &&
          rows[65].kind == Kind::Unsupported && rows[72].ueKey == "bHasBeenPaintedInEditor");
}

// UE 5.8 엔진 콘텐츠 텍스처 20개: 경고 없이 번역되고 산 객체에 ImportJson·게시된다. 대표 값이
// 들어갔는지 본다(볼륨 텍스처의 HDR 압축·mip 없음·타일 크기·NeverStream).
TEST(UnrealExchange_TextureSettingsEngineContent) {
    SystemAllocator allocator;
    {
        Array<char> text(allocator);
        REQUIRE(ReadRepositoryFile(
            allocator,
            "/Plugins/Importers/UnrealExchange/Tests/Settings/EngineTextureSettings.json", text));
        JsonDocument fixture(allocator);
        REQUIRE(fixture.Parse(StringView(text.Data(), text.Size())).Succeeded());
        const JsonValue textures = fixture.Root().Find("textures");
        REQUIRE(textures.IsArray() && textures.Size() == 20);
        TextureEnv env(allocator);
        REQUIRE(env.registered);
        usize checkedVolume = 0;
        for (usize index = 0; index < textures.Size(); ++index) {
            const JsonValue entry = textures.At(index);
            const ExchangeClass type = FindExchangeClass(entry.Find("class").GetString());
            REQUIRE(IsExchangeTextureClass(type));
            StringBuilder<> out(allocator);
            JsonWriter writer(out);
            Array<ExchangeSettingWarning> warnings(allocator);
            ExchangeSettingInput input;
            input.settings = entry.Find("settings");
            input.rows = ExchangeTextureSettingRows();
            input.output = type;
            REQUIRE(TranslateExchangeSettings(input, writer, warnings));
            REQUIRE(writer.Finish());
            CHECK(warnings.IsEmpty());

            auto made = env.objects.NewObject<Object>(env.access, TextureTypeOf(type));
            REQUIRE(made);
            JsonDocument translated(allocator);
            REQUIRE(translated.Parse(out.View()).Succeeded());
            reflection_text::TextCodecOptions options;
            env.diagnostics.Clear();
            auto prepared =
                reflection_text::ImportJson(translated.Root(),
                                            reflection_text::AssignmentTarget::Live(
                                                env.objects, env.access, made.Value().Handle()),
                                            options, env.diagnostics);
            CHECK(prepared);
            if (!prepared) {
                continue;
            }

            REQUIRE(prepared.Value().Publish());
            if (entry.Find("objectPath").GetString() ==
                "/Engine/EngineMaterials/Hair/Hair_DualScattering.Hair_DualScattering") {
                auto borrowed =
                    env.objects.Resolve<VolumeTexture>(env.access, made.Value().Handle());
                REQUIRE(borrowed);
                const VolumeTexture& volume = *borrowed.Value().Get().Value();
                CHECK(volume.compressionSettings == TextureCompressionSettings::HDR &&
                      volume.mipGenSettings == TextureMipGenSettings::NoMipmaps &&
                      volume.source2DTileSizeX == 64 && volume.neverStream == 1 &&
                      volume.srgb == 0 && volume.addressMode == TextureAddress::Wrap);
                checkedVolume += 1;
            }
        }

        CHECK(checkedVolume == 1);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// bool 접두 음성(b 뒤가 소문자·한 글자 b는 그대로)과 Unsupported 기본값 비교(수치 비교라 -1.0은
// -1과 같고, 객체는 키 순서까지 본다).
TEST(UnrealExchange_SettingsPrefixAndDefaults) {
    SystemAllocator allocator;
    {
        const ExchangeSettingRow rows[] = {
            {"bake", Kind::Same, 0, nullptr, 0, {}, {}},
            {"b", Kind::Same, 0, nullptr, 0, {}, {}},
            {"bX", Kind::Same, 0, nullptr, 0, {}, {}},
            {"Bias", Kind::Unsupported, 0, nullptr, 0, "-1", {}},
            {"Pair", Kind::Unsupported, 0, nullptr, 0, R"({"A": 1, "B": 2})", {}},
        };
        StringBuilder<> out(allocator);
        Array<ExchangeSettingWarning> warnings(allocator);
        REQUIRE(Translate(allocator,
                          R"({"bake": 1, "b": 2, "bX": 3, "Bias": -1.0, "Pair": {"B": 2, "A": 1}})",
                          ExchangeClass::Texture2D, nullptr, out, warnings,
                          Span<const ExchangeSettingRow>(rows)));
        CHECK(out.View() == StringView(R"({"bake":1,"b":2,"X":3})"
                                       "\n"));
        REQUIRE(warnings.Size() == 1);
        CHECK(warnings[0].detail == 4);

        REQUIRE(Translate(allocator, R"({"Bias": 0, "Pair": {"A": 1, "B": 2}})",
                          ExchangeClass::Texture2D, nullptr, out, warnings,
                          Span<const ExchangeSettingRow>(rows)));
        CHECK(warnings.Size() == 1 && warnings[0].detail == 3);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 엔진 콘텐츠에 없는 값(0이 아닌 Guid, LayerFormatSettings 원소, 색 공간 struct, PerPlatform
// 기본값, 주소·그룹 enum)을 번역해 Texture2D에 ImportJson하고 필드 값을 본다.
TEST(UnrealExchange_TextureSettingsImportValues) {
    SystemAllocator allocator;
    {
        constexpr StringView Settings = R"({
"CompressionCacheId": {"A": 1, "B": 2, "C": -3, "D": 4},
"LayerFormatSettings": [{"CompressionSettings": "TC_Normalmap", "CompressionNoAlpha": true,
  "CompressionForceAlpha": false, "CompressionNone": false, "CompressionYCoCg": true, "SRGB": false}],
"Downscale": {"Default": 0.5, "PerPlatform": []},
"SourceColorSettings": {"EncodingOverride": "TSE_Linear", "ColorSpace": "TCS_Rec2020",
  "RedChromaticityCoordinate": {"X": 0.5, "Y": 0.25}, "GreenChromaticityCoordinate": {"X": 0, "Y": 0},
  "BlueChromaticityCoordinate": {"X": 0, "Y": 0}, "WhiteChromaticityCoordinate": {"X": 0, "Y": 0},
  "ChromaticAdaptationMethod": "TCAM_CAT02"},
"AddressX": "TA_Clamp", "AddressZ": "TA_Mirror", "bFlipGreenChannel": true,
"LODGroup": "TEXTUREGROUP_UI", "MipGenSettings": "TMGS_Sharpen4", "SRGB": false})";
        JsonDocument document(allocator);
        REQUIRE(document.Parse(Settings).Succeeded());
        StringBuilder<> out(allocator);
        JsonWriter writer(out);
        Array<ExchangeSettingWarning> warnings(allocator);
        ExchangeSettingInput input;
        input.settings = document.Root();
        input.rows = ExchangeTextureSettingRows();
        input.output = ExchangeClass::Texture2D;
        REQUIRE(TranslateExchangeSettings(input, writer, warnings));
        REQUIRE(writer.Finish());
        CHECK(warnings.IsEmpty());

        TextureEnv env(allocator);
        REQUIRE(env.registered);
        auto made = env.objects.NewObject<Object>(env.access, TypeIdOf<Texture2D>());
        REQUIRE(made);
        JsonDocument translated(allocator);
        REQUIRE(translated.Parse(out.View()).Succeeded());
        reflection_text::TextCodecOptions options;
        auto prepared = reflection_text::ImportJson(
            translated.Root(),
            reflection_text::AssignmentTarget::Live(env.objects, env.access, made.Value().Handle()),
            options, env.diagnostics);
        REQUIRE(prepared);
        REQUIRE(prepared.Value().Publish());
        auto borrowed = env.objects.Resolve<Texture2D>(env.access, made.Value().Handle());
        REQUIRE(borrowed);
        const Texture2D& texture = *borrowed.Value().Get().Value();
        CHECK(texture.compressionCacheId ==
              (Guid{(uint64{1} << 32) | 2, (uint64{0xfffffffd} << 32) | 4}));
        REQUIRE(texture.layerFormatSettings.Size() == 1);
        CHECK(texture.layerFormatSettings[0].compressionSettings ==
                  TextureCompressionSettings::Normalmap &&
              texture.layerFormatSettings[0].compressionNoAlpha == 1 &&
              texture.layerFormatSettings[0].compressionYCoCg == 1 &&
              texture.layerFormatSettings[0].srgb == 0);
        CHECK(texture.downscale.defaultValue == 0.5f);
        CHECK(texture.sourceColorSettings.encodingOverride == TextureSourceEncoding::Linear &&
              texture.sourceColorSettings.colorSpace == TextureColorSpace::Rec2020 &&
              texture.sourceColorSettings.redChromaticityCoordinate.x == 0.5 &&
              texture.sourceColorSettings.redChromaticityCoordinate.y == 0.25 &&
              texture.sourceColorSettings.chromaticAdaptationMethod ==
                  TextureChromaticAdaptationMethod::CAT02);
        CHECK(texture.addressX == TextureAddress::Clamp && texture.flipGreenChannel == 1 &&
              texture.lodGroup == TextureGroup::UI &&
              texture.mipGenSettings == TextureMipGenSettings::Sharpen4 && texture.srgb == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace exchange_settings_tests_file
