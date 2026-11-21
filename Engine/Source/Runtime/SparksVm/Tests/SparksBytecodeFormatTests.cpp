#include <Core/Text/StringView.h>
#include <SparksVm/Bytecode.h>
#include <SparksVm/Opcode.h>
#include <TestSupport/Test.h>

// bytecode 형식 시험이다(Docs/Design/SparksBytecode.ko.md 4.3절): opcode 정보 표가 열거 순서와
// 같고, 이름이 겹치지 않고, 명령 단어 만들기·읽기가 모든 피연산자 범위에서 되돌아온다.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_bytecode_format_tests_file {

TEST(SparksBytecodeFormat_InfoTableMatchesOpcodes) {
    const usize count = static_cast<usize>(Opcode::Count);
    for (usize index = 0; index < count; ++index) {
        const OpcodeInfo& info = InfoOf(static_cast<Opcode>(index));
        CHECK(static_cast<usize>(info.op) == index);
        CHECK(info.name != nullptr && StringView(info.name).ByteCount() > 0);
        for (usize other = 0; other < index; ++other) {
            CHECK(StringView(info.name) != StringView(InfoOf(static_cast<Opcode>(other)).name));
        }
    }

    CHECK(StringView(InfoOf(Opcode::AddI64).name) == StringView("ADD.I64"));
    CHECK(StringView(InfoOf(Opcode::Extra).name) == StringView("EXTRA"));
    CHECK(InfoOf(Opcode::GetFS).extra);
    CHECK(!InfoOf(Opcode::MovS).extra);
    CHECK(StringView(ScalarKindName(ScalarKind::F64)) == StringView("F64"));
    CHECK(StringView(ScalarKindName(static_cast<ScalarKind>(200))) == StringView("?"));
    CHECK(StringView(BytecodeRefKindName(BytecodeRefKind::Literal)) == StringView("literal"));
}

TEST(SparksBytecodeFormat_WordsRoundTrip) {
    const uint32 abc = EncodeABC(Opcode::AddI64, 255, 7, 128);
    CHECK(DecodeOpcode(abc) == Opcode::AddI64);
    CHECK(DecodeA(abc) == 255);
    CHECK(DecodeB(abc) == 7);
    CHECK(DecodeC(abc) == 128);

    const uint32 abx = EncodeABx(Opcode::LoadK, 3, MaxBx);
    CHECK(DecodeOpcode(abx) == Opcode::LoadK);
    CHECK(DecodeA(abx) == 3);
    CHECK(DecodeBx(abx) == MaxBx);

    const int32 distances[] = {MinImmediate, -1, 0, 1, MaxImmediate};
    for (const int32 distance : distances) {
        const uint32 word = EncodeAsBx(Opcode::Brt, 9, distance);
        CHECK(DecodeOpcode(word) == Opcode::Brt);
        CHECK(DecodeA(word) == 9);
        CHECK(DecodeSBx(word) == distance);
    }

    const uint32 ax = EncodeAx(Opcode::Extra, MaxAx);
    CHECK(DecodeOpcode(ax) == Opcode::Extra);
    CHECK(DecodeAx(ax) == MaxAx);
    CHECK(DecodeAx(EncodeAx(Opcode::Fault, 1)) == 1);
}

} // namespace sparks_bytecode_format_tests_file
