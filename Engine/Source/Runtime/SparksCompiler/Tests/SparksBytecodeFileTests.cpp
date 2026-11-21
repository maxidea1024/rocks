#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <SparksCompiler/Compilation.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>
#include <SparksVm/BytecodeFile.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// `.sparkc` 쓰기·읽기와 verifier 시험이다(Docs/Design/SparksBytecode.ko.md 5절, LC4c). 왕복
// 동일성은 corpus `Codegen`의 `.bc` 사례마다 test host가 보고, 여기서는 형식
// 변조(magic·버전·구역·길이·잘림)와 verifier 거부(명령·레지스터·상수·참조·분기·끝 명령·표)의 사유,
// 할당 실패를 본다.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_bytecode_file_tests_file {

namespace {

constexpr StringView Program = R"(enum Side { Left, Right }

fn Twice(x: int): int {
    return x * 2;
}

fn Pick(side: Side, n: int): string {
    var total = 0;
    for (i in 0..<n) {
        total += Twice(i) * 70000;
    }

    switch (side) {
        case Side.Left: {
            return "left ${total}";
        }
        case Side.Right: {
            return "right";
        }
    }
}
)";

// Program을 컴파일한 모듈의 `.sparkc` byte를 out에 둔다.
bool CompileFile(Allocator& allocator, Array<uint8>& out) noexcept {
    SourceSet sources(allocator);
    DiagnosticList diagnostics(allocator);
    Compilation compilation(allocator, sources);
    auto manifest = sources.TryAdd("Game/SparksPackage.json5",
                                   R"({ name: "Game", sparks: 1, profile: "full" })", diagnostics);
    if (!manifest) {
        return false;
    }

    auto package = compilation.AddPackage(manifest.Value());
    auto source = sources.TryAdd("Game/Main.spark", Program, diagnostics);
    if (!package || !source) {
        return false;
    }

    auto module = compilation.AddModule(package.Value(), source.Value(), "Main.spark");
    if (!module || !compilation.Check(diagnostics) || diagnostics.Count() != 0) {
        return false;
    }

    const BytecodeModule* bytecode = compilation.ModuleBytecode(module.Value());
    return bytecode != nullptr && WriteBytecodeFile(*bytecode, out);
}

// bytes를 읽어 거부 사유를 돌려준다(통과면 None).
BytecodeLoadError ReadError(Allocator& allocator, Span<const uint8> bytes) noexcept {
    BytecodeModule module(allocator);
    BytecodeRejection rejection;
    auto read = ReadBytecodeFile(bytes, module, rejection);
    return read ? BytecodeLoadError::None : rejection.error;
}

// 파일의 구역 tag 위치다(머리 뒤 첫 구역부터 차례로).
usize SectionAt(Span<const uint8> bytes, usize index) noexcept {
    constexpr usize HeaderBytes = 4 + 4 + 4 + 4 + 32 + 32 + 1;
    usize at = HeaderBytes;
    for (usize section = 0; section < index; ++section) {
        const uint32 length = uint32(bytes[at + 4]) | (uint32(bytes[at + 5]) << 8) |
                              (uint32(bytes[at + 6]) << 16) | (uint32(bytes[at + 7]) << 24);
        at += 8 + length;
    }

    return at;
}

} // namespace

TEST(SparksBytecodeFile_RoundTripIsByteIdentical) {
    SystemAllocator allocator;
    Array<uint8> file(allocator);
    REQUIRE(CompileFile(allocator, file));
    BytecodeModule loaded(allocator);
    BytecodeRejection rejection;
    REQUIRE(ReadBytecodeFile(file.View(), loaded, rejection));
    CHECK(rejection.error == BytecodeLoadError::None);
    Array<uint8> again(allocator);
    REQUIRE(WriteBytecodeFile(loaded, again));
    REQUIRE(again.Size() == file.Size());
    bool same = true;
    for (usize index = 0; index < file.Size(); ++index) {
        same = same && file[index] == again[index];
    }

    CHECK(same);
    StringBuilder<4096> dump(allocator);
    REQUIRE(AppendBytecodeDump(dump, loaded));
    CHECK(dump.View().StartsWith("module \"Main\"\ncompiler \"sparks 0.4.0\"\nprofile full\n"));
    CHECK(dump.View().Contains("SWITCH"));
    CHECK(dump.View().Contains("LOADK"));
}

TEST(SparksBytecodeFile_FormatTampering) {
    SystemAllocator allocator;
    Array<uint8> file(allocator);
    REQUIRE(CompileFile(allocator, file));
    CHECK(ReadError(allocator, file.View()) == BytecodeLoadError::None);
    auto tampered = [&](auto&& change) noexcept {
        Array<uint8> copy(allocator);
        static_cast<void>(copy.TryAppend(file.View()));
        change(copy);
        return ReadError(allocator, copy.View());
    };
    CHECK(ReadError(allocator, Span<const uint8>()) == BytecodeLoadError::Truncated);
    CHECK(tampered([](Array<uint8>& bytes) noexcept { bytes[0] = 'X'; }) ==
          BytecodeLoadError::BadMagic);
    CHECK(tampered([](Array<uint8>& bytes) noexcept { bytes[4] += 1; }) ==
          BytecodeLoadError::UnsupportedVersion);
    CHECK(tampered([](Array<uint8>& bytes) noexcept {
              static_cast<void>(bytes.TryResize(bytes.Size() - 1));
          }) == BytecodeLoadError::Truncated);
    CHECK(tampered([](Array<uint8>& bytes) noexcept { static_cast<void>(bytes.TryPushBack(0)); }) ==
          BytecodeLoadError::BadSection);
    for (usize section = 0; section < 4; ++section) {
        // 구역 tag와 길이(내용보다 길게 — 다음 구역 tag를 읽게 된다).
        CHECK(tampered([&](Array<uint8>& bytes) noexcept {
                  bytes[SectionAt(file.View(), section)] = '?';
              }) == BytecodeLoadError::BadSection);
        const BytecodeLoadError longer = tampered(
            [&](Array<uint8>& bytes) noexcept { bytes[SectionAt(file.View(), section) + 4] += 1; });
        CHECK(longer == BytecodeLoadError::BadSection || longer == BytecodeLoadError::Truncated);
    }

    // 개수를 크게 바꾸면 할당 전에 잘린 구역으로 거부한다(KCON의 상수 수).
    CHECK(tampered([&](Array<uint8>& bytes) noexcept {
              bytes[SectionAt(file.View(), 0) + 8 + 3] = 0x7F;
          }) == BytecodeLoadError::BadSection);
}

TEST(SparksBytecodeFile_VerifierRejections) {
    SystemAllocator allocator;
    Array<uint8> file(allocator);
    REQUIRE(CompileFile(allocator, file));
    // 읽은 모듈을 바꾸고 verifier의 사유를 본다.
    auto verify = [&](auto&& change) noexcept {
        BytecodeModule module(allocator);
        BytecodeRejection rejection;
        if (!ReadBytecodeFile(file.View(), module, rejection)) {
            return BytecodeLoadError::Truncated;
        }

        change(module);
        return VerifyBytecode(module).error;
    };
    auto find = [](const BytecodeModule& module, Opcode op) noexcept {
        for (usize index = 0; index < module.words.Size(); ++index) {
            if (DecodeOpcode(module.words[index]) == op) {
                return index;
            }
        }

        return usize(0);
    };
    CHECK(verify([](BytecodeModule&) noexcept {}) == BytecodeLoadError::None);
    CHECK(verify([](BytecodeModule& m) noexcept { m.words[0] = 0xFF; }) ==
          BytecodeLoadError::BadOpcode);
    CHECK(verify([](BytecodeModule& m) noexcept { m.words[0] = EncodeAx(Opcode::Extra, 0); }) ==
          BytecodeLoadError::BadOpcode);
    CHECK(verify([&](BytecodeModule& m) noexcept {
              const usize at = find(m, Opcode::LoadK);
              m.words[at] = EncodeABx(Opcode::LoadK, 255, DecodeBx(m.words[at]));
          }) == BytecodeLoadError::BadRegister);
    CHECK(verify([&](BytecodeModule& m) noexcept {
              const usize at = find(m, Opcode::LoadK);
              m.words[at] = EncodeABx(Opcode::LoadK, DecodeA(m.words[at]), MaxBx);
          }) == BytecodeLoadError::BadConstant);
    CHECK(verify([&](BytecodeModule& m) noexcept {
              const usize at = find(m, Opcode::LoadStr);
              m.words[at] = EncodeABx(Opcode::LoadStr, DecodeA(m.words[at]), MaxBx);
          }) == BytecodeLoadError::BadConstant);
    CHECK(verify([&](BytecodeModule& m) noexcept {
              const usize at = find(m, Opcode::Jmp);
              m.words[at] = EncodeAsBx(Opcode::Jmp, 0, MaxImmediate);
          }) == BytecodeLoadError::BadBranch);
    CHECK(verify([&](BytecodeModule& m) noexcept {
              const usize at = find(m, Opcode::ToStr);
              m.words[at + 1] = EncodeAx(Opcode::Extra, 200u << 16);
          }) == BytecodeLoadError::BadOperand);
    CHECK(verify([&](BytecodeModule& m) noexcept {
              const usize at = find(m, Opcode::Switch);
              m.words[at] = EncodeABx(Opcode::Switch, DecodeA(m.words[at]), 7);
          }) == BytecodeLoadError::BadBranch);
    CHECK(verify([](BytecodeModule& m) noexcept {
              m.words[m.functions[0].firstWord + m.functions[0].wordCount - 1] =
                  EncodeABC(Opcode::MovS, 0, 0, 0);
          }) == BytecodeLoadError::BadEnd);
    CHECK(verify([](BytecodeModule& m) noexcept { m.functions[0].wordCount += 1000; }) ==
          BytecodeLoadError::BadFunction);
    CHECK(verify([](BytecodeModule& m) noexcept { m.functions[0].frame[0] = 300; }) ==
          BytecodeLoadError::BadFunction);
    CHECK(verify([](BytecodeModule& m) noexcept { m.functions[0].name = 9999; }) ==
          BytecodeLoadError::BadFunction);
    CHECK(verify([](BytecodeModule& m) noexcept { m.refs[0].name = 9999; }) ==
          BytecodeLoadError::BadTable);
    CHECK(verify([&](BytecodeModule& m) noexcept {
              const usize at = find(m, Opcode::Call);
              m.words[at] = EncodeABx(Opcode::CallN, 0, DecodeBx(m.words[at]));
          }) == BytecodeLoadError::BadReference);
    CHECK(verify([](BytecodeModule& m) noexcept { m.cases[0].target = 9999; }) ==
          BytecodeLoadError::BadBranch);
    CHECK(verify([](BytecodeModule& m) noexcept {
              static_cast<void>(m.locations.TryResize(m.locations.Size() - 1));
          }) == BytecodeLoadError::BadTable);
}

TEST(SparksBytecodeFile_AllocationFailureEverywhere) {
    SystemAllocator backing;
    Array<uint8> file(backing);
    REQUIRE(CompileFile(backing, file));
    uint64 readAttempts = 0;
    uint64 failures = 0;
    for (uint64 failAt = 0; failAt == 0 || failAt <= readAttempts; ++failAt) {
        test::NFailAllocator allocator(backing);
        {
            BytecodeModule module(allocator);
            BytecodeRejection rejection;
            if (failAt != 0) {
                allocator.FailOnAttempt(failAt);
            }

            auto read = ReadBytecodeFile(file.View(), module, rejection);
            const uint64 attempts = allocator.Attempts();
            allocator.DisableFailure();
            if (failAt == 0) {
                REQUIRE(read);
                readAttempts = attempts;
            } else if (!read) {
                ++failures;
                CHECK(read.Error() == ErrorCode::OutOfMemory);
            }
        }
        CHECK(backing.Stats().liveAllocations == 1); // file만 남는다
    }

    CHECK(failures > 0);
}

} // namespace sparks_bytecode_file_tests_file
