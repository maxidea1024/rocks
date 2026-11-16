#include <GraphicsTypes/VertexFormat.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

using namespace rocks;
using namespace rocks::graphics;

namespace {

// RenderBase 10.4절 표 그대로다(번호·이름·byte·성분·셰이더 타입·정규화).
struct Row {
    uint32 number;
    const char* name;
    uint32 bytes;
    uint32 components;
    VertexShaderType shader;
    bool normalized;
};

constexpr Row Table[] = {
    {1, "Float1", 4, 1, VertexShaderType::Float, false},
    {2, "Float2", 8, 2, VertexShaderType::Float, false},
    {3, "Float3", 12, 3, VertexShaderType::Float, false},
    {4, "Float4", 16, 4, VertexShaderType::Float, false},
    {5, "Byte4Snorm", 4, 4, VertexShaderType::Float, true},
    {6, "UByte4", 4, 4, VertexShaderType::Uint, false},
    {7, "UByte4Unorm", 4, 4, VertexShaderType::Float, true},
    {8, "Bgra8Unorm", 4, 4, VertexShaderType::Float, true},
    {9, "Short2", 4, 2, VertexShaderType::Int, false},
    {10, "Short4", 8, 4, VertexShaderType::Int, false},
    {11, "Short2Snorm", 4, 2, VertexShaderType::Float, true},
    {12, "Short4Snorm", 8, 4, VertexShaderType::Float, true},
    {13, "Half2", 4, 2, VertexShaderType::Float, false},
    {14, "Half4", 8, 4, VertexShaderType::Float, false},
    {15, "UShort2", 4, 2, VertexShaderType::Uint, false},
    {16, "UShort4", 8, 4, VertexShaderType::Uint, false},
    {17, "UShort2Unorm", 4, 2, VertexShaderType::Float, true},
    {18, "UShort4Unorm", 8, 4, VertexShaderType::Float, true},
    {19, "Rgb10A2Unorm", 4, 4, VertexShaderType::Float, true},
    {20, "UInt1", 4, 1, VertexShaderType::Uint, false},
};

static_assert(sizeof(Table) / sizeof(Table[0]) == VertexFormatCount);

} // namespace

TEST(VertexFormat_TableSizesShaderTypesAndNumbers) {
    for (const Row& row : Table) {
        auto format = VertexFormatFromNumber(row.number);
        REQUIRE(format);
        CHECK(VertexFormatNumber(format.Value()) == row.number);
        CHECK(IsValidVertexFormat(format.Value()));
        CHECK(VertexFormatSize(format.Value()) == row.bytes);
        CHECK(VertexFormatComponents(format.Value()) == row.components);
        CHECK(VertexFormatShaderType(format.Value()) == row.shader);
        CHECK(IsNormalizedVertexFormat(format.Value()) == row.normalized);
        // 크기는 4 byte 배수다(배치 정렬 규칙과 맞는다).
        CHECK(row.bytes % 4 == 0);
        const char* name = VertexFormatName(format.Value());
        CHECK(platform::CStringCompare(name, row.name) == 0);
        auto parsed =
            ParseVertexFormatName(StringView(row.name, platform::CStringLength(row.name)));
        REQUIRE(parsed);
        CHECK(parsed.Value() == format.Value());
    }
}

TEST(VertexFormat_RejectsUnknownNumbersAndNames) {
    CHECK(VertexFormatFromNumber(0).Error() == ErrorCode::InvalidArgument);
    CHECK(VertexFormatFromNumber(VertexFormatCount + 1).Error() == ErrorCode::InvalidArgument);
    const auto unknown = static_cast<VertexFormat>(0);
    CHECK(!IsValidVertexFormat(unknown));
    CHECK(VertexFormatSize(unknown) == 0);
    CHECK(platform::CStringCompare(VertexFormatName(unknown), "Unknown") == 0);
    CHECK(ParseVertexFormatName(StringView("float4", 6)).Error() == ErrorCode::NotFound);
    CHECK(ParseVertexFormatName(StringView("", 0)).Error() == ErrorCode::NotFound);
    CHECK(platform::CStringCompare(VertexShaderTypeName(VertexShaderType::Int), "int") == 0);
}
