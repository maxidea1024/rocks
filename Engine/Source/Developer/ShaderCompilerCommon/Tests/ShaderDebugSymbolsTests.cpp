#include <Core/Memory/SystemAllocator.h>
#include <ShaderCompilerCommon/ShaderDebugSymbols.h>
#include <ShaderCompilerCommon/ShaderFormatBackend.h>
#include <TestSupport/Test.h>
#include <string.h>

using namespace rocks;
namespace shader_debug_symbols_tests_file {

using namespace rocks::shader;

// 값 3 DebugSymbols 배치(Docs/Design/ShaderSystem 16.3절 SSD73) 시험이다: 왕복, 이름 규칙, 손상
// 거부(잘림·버전·종류·이름 길이), 워커 결과에 더하기.
namespace {

SystemAllocator& SymbolsAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

} // namespace

TEST(ShaderDebugSymbols_RoundTripsAndRejectsCorruption) {
    const uint8 pdb[] = {'M', 'i', 'c', 'r', 'o', 0, 1, 2};
    Array<uint8> bytes(SymbolsAllocator());
    REQUIRE(EncodeShaderDebugSymbols(ShaderDebugSymbolKind::Pdb, "0123abcd.pdb", pdb, bytes));
    auto decoded = DecodeShaderDebugSymbols(bytes.View());
    REQUIRE(decoded);
    CHECK(decoded.Value().kind == ShaderDebugSymbolKind::Pdb &&
          decoded.Value().name == StringView("0123abcd.pdb") &&
          decoded.Value().contents.Size() == sizeof(pdb) &&
          memcmp(decoded.Value().contents.Data(), pdb, sizeof(pdb)) == 0);

    // 이름 없는 SPIR-V 모듈, 빈 내용.
    REQUIRE(EncodeShaderDebugSymbols(ShaderDebugSymbolKind::SpirvModule, StringView(), {}, bytes));
    decoded = DecodeShaderDebugSymbols(bytes.View());
    REQUIRE(decoded);
    CHECK(decoded.Value().kind == ShaderDebugSymbolKind::SpirvModule &&
          decoded.Value().name.IsEmpty() && decoded.Value().contents.IsEmpty());
    CHECK(StringView(ShaderDebugSymbolExtension(ShaderDebugSymbolKind::Pdb)) == ".pdb" &&
          StringView(ShaderDebugSymbolExtension(ShaderDebugSymbolKind::SpirvModule)) == ".spv" &&
          StringView(ShaderDebugSymbolExtension(static_cast<ShaderDebugSymbolKind>(9))) == ".bin");

    // 이름 규칙: 디렉터리 구분자·드라이브·NUL·"."·".."·너무 긴 이름·모르는 종류는 만들지 않는다.
    char longName[MaxShaderDebugSymbolNameBytes + 2] = {};
    memset(longName, 'a', MaxShaderDebugSymbolNameBytes + 1);
    const StringView badNames[] = {
        "a/b.pdb", "a\\b.pdb", "C:x.pdb", StringView("a\0b", 3), ".", "..", StringView(longName)};
    for (const StringView name : badNames) {
        CHECK(EncodeShaderDebugSymbols(ShaderDebugSymbolKind::Pdb, name, pdb, bytes).Error() ==
              ErrorCode::InvalidArgument);
    }

    CHECK(EncodeShaderDebugSymbols(static_cast<ShaderDebugSymbolKind>(0), "a.pdb", pdb, bytes)
              .Error() == ErrorCode::InvalidArgument);
    char maxName[MaxShaderDebugSymbolNameBytes + 1] = {};
    memset(maxName, 'b', MaxShaderDebugSymbolNameBytes);
    CHECK(EncodeShaderDebugSymbols(ShaderDebugSymbolKind::Pdb, StringView(maxName), pdb, bytes));

    // 손상: 잘린 머리, 이름 길이가 남은 byte보다 긺, 버전·종류가 다름, 이름 규칙 위반.
    REQUIRE(EncodeShaderDebugSymbols(ShaderDebugSymbolKind::Pdb, "x.pdb", pdb, bytes));
    for (usize size = 0; size < 7; ++size) {
        CHECK(DecodeShaderDebugSymbols(Span<const uint8>(bytes.Data(), size)).Error() ==
              ErrorCode::CorruptData);
    }

    CHECK(DecodeShaderDebugSymbols(Span<const uint8>(bytes.Data(), 7 + 4)).Error() ==
          ErrorCode::CorruptData);
    const auto corrupted = [&bytes](usize at, uint8 value) noexcept {
        Array<uint8> copy(SymbolsAllocator());
        (void)copy.TryAppend(bytes.View());
        copy[at] = value;
        return DecodeShaderDebugSymbols(copy.View()).Error() == ErrorCode::CorruptData;
    };
    CHECK(corrupted(0, 2));
    CHECK(corrupted(3, 1));
    CHECK(corrupted(4, 0));
    CHECK(corrupted(4, 3));
    CHECK(corrupted(7, '/'));
    CHECK(corrupted(6, 1));
}

// 워커 결과: 성공 job 끝에 값 3을 더하고, 실패 job에는 더하지 않으며, 잘못된 이름은 job을
// ToolError로 실패시킨다.
TEST(ShaderDebugSymbols_AttachToWorkerResults) {
    const uint8 symbols[] = {1, 2, 3};
    ShaderWorkerJobResult result(SymbolsAllocator());
    Array<uint8> code(SymbolsAllocator());
    REQUIRE(code.TryPushBack(uint8{7}));
    Array<uint8> layout(SymbolsAllocator());
    SucceedShaderJob(result, code, layout);
    AddShaderJobDebugSymbols(result, ShaderDebugSymbolKind::Pdb, "a.pdb", symbols);
    REQUIRE(result.status == ShaderWorkerJobStatus::Success && result.values.Size() == 2);
    CHECK(result.values[0].id == 0 && result.values[1].id == 3);
    auto decoded = DecodeShaderDebugSymbols(result.values[1].bytes.View());
    REQUIRE(decoded);
    CHECK(decoded.Value().name == StringView("a.pdb") && decoded.Value().contents.Size() == 3);

    ShaderWorkerJobResult failed(SymbolsAllocator());
    FailShaderJob(failed, ShaderWorkerJobStatus::CompileError, "bad");
    AddShaderJobDebugSymbols(failed, ShaderDebugSymbolKind::Pdb, "a.pdb", symbols);
    CHECK(failed.status == ShaderWorkerJobStatus::CompileError && failed.values.IsEmpty());

    ShaderWorkerJobResult named(SymbolsAllocator());
    Array<uint8> again(SymbolsAllocator());
    REQUIRE(again.TryPushBack(uint8{7}));
    SucceedShaderJob(named, again, layout);
    AddShaderJobDebugSymbols(named, ShaderDebugSymbolKind::Pdb, "dir/a.pdb", symbols);
    CHECK(named.status == ShaderWorkerJobStatus::ToolError && named.values.IsEmpty());
}

} // namespace shader_debug_symbols_tests_file
