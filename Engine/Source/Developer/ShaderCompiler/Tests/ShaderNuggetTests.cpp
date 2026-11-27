#include "ShaderSourceTestSupport.h"

#include <AssetNugget/NuggetCache.h>
#include <AssetNugget/NuggetRecord.h>
#include <Base/InitializerList.h>
#include <ShaderCompiler/ShaderCompileOptions.h>
#include <ShaderCompiler/ShaderNugget.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace shader_nugget_tests_file {

using namespace rocks::shader;
using namespace rocks::shader_source_test;

// 셰이더 Record 값 표·너겟 위치(Docs/Design/ShaderSystem 10절)와 컴파일 옵션 파서(11.1절 SC9)
// 시험이다. 너겟 시험은 임시 root에 fixture Config/AssetNuggets.json을 둔다(실제
// Engine/Intermediate를 쓰지 않는다).
namespace {

Array<uint8> Bytes(std::initializer_list<uint8> values) noexcept {
    Array<uint8> bytes(TestAllocator());
    for (const uint8 value : values) {
        CHECK(bytes.TryPushBack(value));
    }

    return bytes;
}

bool SameBytes(const Array<uint8>& bytes, std::initializer_list<uint8> values) noexcept {
    if (bytes.Size() != values.size()) {
        return false;
    }

    usize index = 0;
    for (const uint8 value : values) {
        if (bytes[index++] != value) {
            return false;
        }
    }

    return true;
}

Sha256Digest KeyOf(uint8 seed) noexcept {
    Sha256Digest key;
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        key.bytes[index] = static_cast<uint8>(seed ^ index);
    }

    return key;
}

} // namespace

TEST(ShaderNugget_ValueTableIsFixed) {
    CHECK(static_cast<uint32>(ShaderNuggetValue::Code) == 0);
    CHECK(static_cast<uint32>(ShaderNuggetValue::BindingLayout) == 1);
    CHECK(static_cast<uint32>(ShaderNuggetValue::GlesCopyTable) == 2);
    CHECK(static_cast<uint32>(ShaderNuggetValue::DebugSymbols) == 3);
    CHECK(ShaderNuggetValueCount == 4);
    CHECK(NuggetBucketShader == StringView("shader"));
}

TEST(ShaderNugget_ValuesRoundTrip) {
    // 값 0과 1만 있는 job: 번호 오름차순 값 목록이 되고 다시 같은 값으로 돌아온다(빈 값도 있는
    // 값이다).
    ShaderJobValues values(TestAllocator());
    values.Set(ShaderNuggetValue::BindingLayout, Bytes({4, 5}));
    values.Set(ShaderNuggetValue::Code, Bytes({1, 2, 3}));
    values.Set(ShaderNuggetValue::DebugSymbols, Array<uint8>(TestAllocator()));
    Array<NuggetValue> record(TestAllocator());
    REQUIRE(MoveShaderValuesToNugget(values, record));
    CHECK(values.present == 0 && values.code.IsEmpty());
    REQUIRE(record.Size() == 3);
    CHECK(record[0].id == 0 && SameBytes(record[0].bytes, {1, 2, 3}));
    CHECK(record[1].id == 1 && SameBytes(record[1].bytes, {4, 5}));
    CHECK(record[2].id == 3 && record[2].bytes.IsEmpty());

    ShaderJobValues back(TestAllocator());
    REQUIRE(MoveShaderValuesFromNugget(record, back));
    CHECK(back.Has(ShaderNuggetValue::Code) && SameBytes(back.code, {1, 2, 3}));
    CHECK(back.Has(ShaderNuggetValue::BindingLayout) && SameBytes(back.bindingLayout, {4, 5}));
    CHECK(!back.Has(ShaderNuggetValue::GlesCopyTable));
    CHECK(back.Has(ShaderNuggetValue::DebugSymbols) && back.debugSymbols.IsEmpty());

    // Code 없는 job은 게시하지 않는다.
    ShaderJobValues noCode(TestAllocator());
    noCode.Set(ShaderNuggetValue::BindingLayout, Bytes({9}));
    CHECK(MoveShaderValuesToNugget(noCode, record).Error() == ErrorCode::InvalidArgument);
    CHECK(record.IsEmpty() && noCode.Has(ShaderNuggetValue::BindingLayout));
}

TEST(ShaderNugget_DamagedRecordsAreRejected) {
    auto record = [](std::initializer_list<uint32> ids) noexcept {
        Array<NuggetValue> values(TestAllocator());
        for (const uint32 id : ids) {
            auto added = values.TryEmplaceBack(TestAllocator());
            CHECK(added);
            if (added) {
                added.Value()->id = id;
            }
        }

        return values;
    };

    ShaderJobValues out(TestAllocator());
    // 값 0(Code)이 없으면 손상이다(다시 컴파일).
    Array<NuggetValue> empty = record({});
    CHECK(MoveShaderValuesFromNugget(empty, out).Error() == ErrorCode::NotFound);
    Array<NuggetValue> noCode = record({1, 2});
    CHECK(MoveShaderValuesFromNugget(noCode, out).Error() == ErrorCode::NotFound);
    // 모르는 번호·중복·오름차순 아님.
    Array<NuggetValue> unknown = record({0, 4});
    Array<NuggetValue> duplicate = record({0, 1, 1});
    Array<NuggetValue> unsorted = record({0, 2, 1});
    for (Array<NuggetValue>* bad : {&unknown, &duplicate, &unsorted}) {
        CHECK(MoveShaderValuesFromNugget(*bad, out).Error() == ErrorCode::CorruptData);
        CHECK(out.present == 0);
    }
}

TEST(ShaderNugget_OpensCacheUnderRootAndStoresRecords) {
    Fixture fixture;
    REQUIRE(fixture.Make("nugget"));
    REQUIRE(fixture.Write("Config/AssetNuggets.json",
                          "{ \"localPath\": \"Cache/Shader\", \"required\": true, "
                          "\"generation\": \"01234567-89ab-cdef-0123-456789abcdef\" }"));
    String root(TestAllocator());
    REQUIRE(ResolveShaderNuggetRoot(StringView(fixture.Path()), root));
    CHECK(root.View() == StringView(fixture.Path()));

    NuggetSystemClock clock;
    NuggetCache cache(TestAllocator(), platform::PhysicalFileSystem(), clock);
    NuggetSettingsFailure failure;
    REQUIRE(OpenShaderNuggetCache(cache, TestAllocator(), platform::PhysicalFileSystem(),
                                  root.View(), &failure));
    CHECK(cache.IsEnabled() && cache.IsRequired());
    const Guid generation{0x0123456789abcdefull, 0x0123456789abcdefull};
    CHECK(cache.ProjectGeneration() == generation);

    // 게시하고 Record 전체를 청해 같은 값을 얻는다.
    ShaderJobValues values(TestAllocator());
    values.Set(ShaderNuggetValue::Code, Bytes({0xD, 0x1, 0x1}));
    Array<NuggetValue> put(TestAllocator());
    REQUIRE(MoveShaderValuesToNugget(values, put));
    NuggetCachePut request;
    request.bucket = NuggetBucketShader;
    request.key = KeyOf(0x42);
    request.values = put.View();
    CHECK(cache.Put(request).status == NuggetStatus::Stored);
    NuggetCacheGet get;
    get.bucket = NuggetBucketShader;
    get.key = KeyOf(0x42);
    NuggetCacheGetResult got = cache.Get(get);
    REQUIRE(got.status == NuggetStatus::Hit);
    ShaderJobValues back(TestAllocator());
    REQUIRE(MoveShaderValuesFromNugget(got.values, back));
    CHECK(SameBytes(back.code, {0xD, 0x1, 0x1}));
    cache.Shutdown(0);

    // 설정 위치 아래에 저장소가 생겼다.
    char store[FixturePathBytes];
    REQUIRE(fixture.Child("Cache/Shader", store, sizeof(store)));
    bool exists = false;
    CHECK(platform::PhysicalFileSystem().DirectoryExists(store, strlen(store), exists) && exists);
}

TEST(ShaderNugget_DefaultLocationAndSettingsErrors) {
    // 설정 파일이 없으면 기본 위치 <root>/Intermediate/AssetNuggets다.
    Fixture fixture;
    REQUIRE(fixture.Make("nuggetdefault"));
    {
        NuggetSystemClock clock;
        NuggetCache cache(TestAllocator(), platform::PhysicalFileSystem(), clock);
        REQUIRE(OpenShaderNuggetCache(cache, TestAllocator(), platform::PhysicalFileSystem(),
                                      fixture.Path(), nullptr));
        CHECK(cache.IsEnabled() && cache.ProjectGeneration() == Guid{});
        ShaderJobValues values(TestAllocator());
        values.Set(ShaderNuggetValue::Code, Bytes({7}));
        Array<NuggetValue> put(TestAllocator());
        REQUIRE(MoveShaderValuesToNugget(values, put));
        NuggetCachePut request;
        request.bucket = NuggetBucketShader;
        request.key = KeyOf(0x07);
        request.values = put.View();
        CHECK(cache.Put(request).status == NuggetStatus::Stored);
        cache.Shutdown(0);
        char store[FixturePathBytes];
        REQUIRE(fixture.Child("Intermediate/AssetNuggets", store, sizeof(store)));
        bool exists = false;
        CHECK(platform::PhysicalFileSystem().DirectoryExists(store, strlen(store), exists) &&
              exists);
    }

    // 모르는 키는 거부하고 칸을 알려 준다.
    Fixture bad;
    REQUIRE(bad.Make("nuggetbad"));
    REQUIRE(bad.Write("Config/AssetNuggets.json", "{ \"unknown\": 1 }"));
    NuggetSystemClock clock;
    NuggetCache cache(TestAllocator(), platform::PhysicalFileSystem(), clock);
    NuggetSettingsFailure failure;
    CHECK(OpenShaderNuggetCache(cache, TestAllocator(), platform::PhysicalFileSystem(), bad.Path(),
                                &failure)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(failure.field == NuggetSettingsField::UnknownKey);
}

TEST(ShaderNugget_RootFallsBackToEngineFolder) {
    // 프로젝트가 없으면 엔진 폴더(실행 파일 위쪽의 Engine/Config를 가진 폴더의 Engine)다. 캐시는
    // 열지 않는다(실제 Engine/Intermediate를 쓰지 않는다).
    String root(TestAllocator());
    REQUIRE(ResolveShaderNuggetRoot(StringView(), root));
    CHECK(root.View().EndsWith("Engine", StringComparison::CaseSensitive));
    String config(TestAllocator());
    REQUIRE(config.TryAppend(root.View()) && config.TryAppend("/Config"));
    bool exists = false;
    CHECK(
        platform::PhysicalFileSystem().DirectoryExists(config.Data(), config.ByteCount(), exists) &&
        exists);
}

TEST(ShaderCompileOptions_ParsesAssignments) {
    ShaderFormatDefinition format;
    REQUIRE(LoadShaderFormatDefinition(ShaderFormatDirectory, "D3D11_SM5", format));
    ShaderCompileOptions options = DefaultShaderCompileOptions(format);
    CHECK(options.hlsl2021 == format.hlsl2021 && !options.hlsl2021);
    CHECK(options.optimization == ShaderOptimization::O3 && !options.debugSymbols &&
          options.matrixLayout == ShaderMatrixLayout::Row);
    CHECK(ValidateShaderCompileOptions(options));

    const StringView given[] = {"optimization=O0", "hlsl2021=true", "matrixLayout=Row",
                                "debugSymbols=false"};
    REQUIRE(ParseShaderCompileOptions(given, options));
    CHECK(options.optimization == ShaderOptimization::O0 && options.hlsl2021);
    for (const char* level : {"O0", "O1", "O2", "O3"}) {
        char text[32];
        snprintf(text, sizeof(text), "optimization=%s", level);
        const StringView one[] = {StringView(text)};
        CHECK(ParseShaderCompileOptions(one, options));
        CHECK(static_cast<int>(options.optimization) == level[1] - '0');
    }

    // 빈 목록은 아무것도 바꾸지 않는다.
    const ShaderCompileOptions before = options;
    CHECK(ParseShaderCompileOptions({}, options));
    CHECK(options.optimization == before.optimization && options.hlsl2021 == before.hlsl2021);
}

TEST(ShaderCompileOptions_RejectsInvalidAssignments) {
    struct Case {
        StringView assignments[3];
        usize count;
        ShaderOptionError code;
        usize index;
        const char* name;
    };

    const Case cases[] = {
        {{"optimization"}, 1, ShaderOptionError::InvalidSyntax, 0, "optimization"},
        {{"=O1"}, 1, ShaderOptionError::InvalidSyntax, 0, "=O1"},
        {{"hlsl2021=true", "speed=fast"}, 2, ShaderOptionError::UnknownName, 1, "speed"},
        {{"Optimization=O1"}, 1, ShaderOptionError::UnknownName, 0, "Optimization"},
        {{"optimization=O4"}, 1, ShaderOptionError::InvalidValue, 0, "optimization"},
        {{"optimization=o1"}, 1, ShaderOptionError::InvalidValue, 0, "optimization"},
        {{"optimization= O1"}, 1, ShaderOptionError::InvalidValue, 0, "optimization"},
        {{"hlsl2021=1"}, 1, ShaderOptionError::InvalidValue, 0, "hlsl2021"},
        {{"hlsl2021=True"}, 1, ShaderOptionError::InvalidValue, 0, "hlsl2021"},
        {{"matrixLayout=Column"}, 1, ShaderOptionError::InvalidValue, 0, "matrixLayout"},
        {{"optimization=O1", "hlsl2021=true", "optimization=O1"},
         3,
         ShaderOptionError::Duplicate,
         2,
         "optimization"},
        {{"debugSymbols=true"}, 1, ShaderOptionError::Unsupported, 0, "debugSymbols"},
    };
    for (const Case& test : cases) {
        ShaderCompileOptions options;
        options.optimization = ShaderOptimization::O2;
        const ShaderOptionStatus status = ParseShaderCompileOptions(
            Span<const StringView>(test.assignments, test.count), options);
        if (status.code != test.code || status.index != test.index ||
            strcmp(status.name, test.name) != 0) {
            fprintf(stderr, "option case '%.*s': %s index %zu name %s\n",
                    static_cast<int>(test.assignments[0].ByteCount()), test.assignments[0].Data(),
                    ShaderOptionErrorName(status.code), static_cast<size_t>(status.index),
                    status.name);
        }

        CHECK(status.code == test.code && status.index == test.index &&
              strcmp(status.name, test.name) == 0);
        // 실패하면 옵션은 그대로다.
        CHECK(options.optimization == ShaderOptimization::O2 && !options.hlsl2021);
    }

    ShaderCompileOptions debug;
    debug.debugSymbols = true;
    CHECK(ValidateShaderCompileOptions(debug).code == ShaderOptionError::Unsupported);
}

} // namespace shader_nugget_tests_file
