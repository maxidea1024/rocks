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

constexpr StringView EngineSchema =
    R"({ "format": 1, "package": "Rocks", "module": "Engine", "types": [
{ "name": "Actor", "id": "5a7e0000000000000000000000000001", "kind": "object", "scriptExtensible": true },
{ "name": "Component", "id": "5a7e0000000000000000000000000002", "kind": "object", "fields": [ { "name": "Owner", "id": "5a7e0000000000000000000000000201", "type": "Actor" } ] },
{ "name": "ScriptableComponent", "id": "5a7e0000000000000000000000000003", "kind": "object", "base": "Component", "scriptExtensible": true }
] })";

// 명령 종류를 고루 담은 프로그램이다(변조 시험이 opcode마다 첫 명령을 바꾼다).
constexpr StringView Program = R"(enum Side { Left, Right }

struct Box {
    var Spot: vec3 = vec3(1.0, 2.0, 3.0);
}

fn Fill(names: Array<string>, table: Map<string, int>): int {
    var copy = names;
    copy.Add("x");
    var counts = table;
    counts["k"] = 1;
    let box = Box();
    let y = box.Spot.Y;
    _ = y;
    let f = float(counts["k"] ?? 0);
    _ = f;
    _ = copy;
    return counts["k"] ?? 0;
}

class Door : ScriptableComponent {
    var Open = false;
    event Opened();

    on Enable() {
        Open = true;
    }

    async fn Fetch(): Result<int> {
        await NextTick();
        return 1;
    }

    async fn Use(): Result<int> {
        let n = try await Fetch();
        return n;
    }

    async fn Pulse() {
        await NextTick();
    }

    async fn Both() {
        await All(Pulse(), Opened);
    }
}

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
    auto schema = sources.TryAdd("Schema/Engine.sparkbind.json", EngineSchema, diagnostics);
    if (!manifest || !schema || !compilation.AddBindingSchema(schema.Value())) {
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
    // 읽은 모듈을 바꾸고 verifier의 사유를 본다(바꿀 명령의 주소는 원본에서 먼저 찾는다).
    BytecodeModule original(allocator);
    BytecodeRejection originalRejection;
    REQUIRE(ReadBytecodeFile(file.View(), original, originalRejection));
    auto find = [&](Opcode op) noexcept {
        for (usize index = 0; index < original.words.Size(); ++index) {
            if (DecodeOpcode(original.words[index]) == op) {
                return index;
            }
        }

        return original.words.Size();
    };
    auto functionOf = [&](usize word) noexcept {
        for (uint32 index = 0; index < original.functions.Size(); ++index) {
            const BytecodeFunction& function = original.functions[index];
            if (word >= function.firstWord && word < function.firstWord + function.wordCount) {
                return index;
            }
        }

        return uint32(0);
    };
    const Opcode needed[] = {Opcode::LoadK,  Opcode::LoadStr, Opcode::Jmp,  Opcode::ToStr,
                             Opcode::Switch, Opcode::GetE,    Opcode::SetE, Opcode::GetSFS,
                             Opcode::TryS,   Opcode::Await,   Opcode::Conv, Opcode::Call,
                             Opcode::Ret,    Opcode::WaitEv,  Opcode::CallW};
    for (const Opcode op : needed) {
        REQUIRE(find(op) < original.words.Size());
    }

    auto verify = [&](auto&& change) noexcept {
        BytecodeModule module(allocator);
        BytecodeRejection rejection;
        if (!ReadBytecodeFile(file.View(), module, rejection)) {
            return BytecodeLoadError::Truncated;
        }

        change(module);
        return VerifyBytecode(module).error;
    };
    auto patch = [&](Opcode op, auto&& make) noexcept {
        const usize at = find(op);
        return verify([&](BytecodeModule& m) noexcept { m.words[at] = make(m.words[at]); });
    };
    auto patchExtra = [&](Opcode op, uint32 ax) noexcept {
        const usize at = find(op);
        return verify(
            [&](BytecodeModule& m) noexcept { m.words[at + 1] = EncodeAx(Opcode::Extra, ax); });
    };
    auto abc = [](uint32 word, uint32 a, uint32 b, uint32 c) noexcept {
        return EncodeABC(DecodeOpcode(word), a, b, c);
    };
    CHECK(verify([](BytecodeModule&) noexcept {}) == BytecodeLoadError::None);
    CHECK(verify([](BytecodeModule& m) noexcept { m.words[0] = 0xFF; }) ==
          BytecodeLoadError::BadOpcode);
    CHECK(verify([](BytecodeModule& m) noexcept { m.words[0] = EncodeAx(Opcode::Extra, 0); }) ==
          BytecodeLoadError::BadOpcode);
    CHECK(patch(Opcode::LoadK, [](uint32 w) noexcept {
              return EncodeABx(Opcode::LoadK, 255, DecodeBx(w));
          }) == BytecodeLoadError::BadRegister);
    CHECK(patch(Opcode::LoadK, [](uint32 w) noexcept {
              return EncodeABx(Opcode::LoadK, DecodeA(w), MaxBx);
          }) == BytecodeLoadError::BadConstant);
    CHECK(patch(Opcode::LoadStr, [](uint32 w) noexcept {
              return EncodeABx(Opcode::LoadStr, DecodeA(w), MaxBx);
          }) == BytecodeLoadError::BadConstant);
    CHECK(patch(Opcode::Jmp, [](uint32) noexcept {
              return EncodeAsBx(Opcode::Jmp, 0, MaxImmediate);
          }) == BytecodeLoadError::BadBranch);
    CHECK(patch(Opcode::Jmp, [](uint32 w) noexcept {
              return EncodeAsBx(Opcode::Jmp, 1, DecodeSBx(w));
          }) == BytecodeLoadError::BadOperand);
    CHECK(patchExtra(Opcode::ToStr, 200u << 16) == BytecodeLoadError::BadOperand);
    // TOSTR의 원본 slot 수(C)가 표시 종류와 다르다.
    CHECK(patch(Opcode::ToStr, [&](uint32 w) noexcept {
              return abc(w, DecodeA(w), DecodeB(w), 2);
          }) == BytecodeLoadError::BadOperand);
    CHECK(patch(Opcode::Switch, [](uint32 w) noexcept {
              return EncodeABx(Opcode::Switch, DecodeA(w), 7);
          }) == BytecodeLoadError::BadBranch);
    // 컨테이너: 표지가 정한 bank의 값 레지스터가 frame 밖, 쓰지 않는 표지 bit.
    CHECK(patch(Opcode::GetE, [&](uint32 w) noexcept {
              return abc(w, 255, DecodeB(w), DecodeC(w));
          }) == BytecodeLoadError::BadRegister);
    CHECK(patchExtra(Opcode::SetE, (1u << 7) | 1u) == BytecodeLoadError::BadOperand);
    // 여러 slot 필드(vec3 — C 3): slot 수 0, frame 밖.
    CHECK(patch(Opcode::GetSFS, [&](uint32 w) noexcept {
              return abc(w, DecodeA(w), DecodeB(w), 0);
          }) == BytecodeLoadError::BadOperand);
    CHECK(patch(Opcode::GetSFS, [&](uint32 w) noexcept { return abc(w, 254, DecodeB(w), 3); }) ==
          BytecodeLoadError::BadRegister);
    // TRY.S: 원본 slot 0, 결과가 frame 밖.
    CHECK(patch(Opcode::TryS, [&](uint32 w) noexcept {
              return abc(w, DecodeA(w), DecodeB(w), 0);
          }) == BytecodeLoadError::BadOperand);
    CHECK(patch(Opcode::TryS, [&](uint32 w) noexcept {
              return abc(w, 255, DecodeB(w), DecodeC(w));
          }) == BytecodeLoadError::BadRegister);
    // CONV: 숫자 타입이 아닌 종류.
    CHECK(patch(Opcode::Conv, [&](uint32 w) noexcept {
              return abc(w, DecodeA(w), DecodeB(w),
                         (static_cast<uint32>(ScalarKind::Flag) << 4) |
                             static_cast<uint32>(ScalarKind::F64));
          }) == BytecodeLoadError::BadOperand);
    // RET의 모양이 함수 표의 반환 모양과 다르다.
    CHECK(patch(Opcode::Ret, [&](uint32 w) noexcept {
              return abc(w, DecodeA(w), DecodeB(w) + 1, DecodeC(w));
          }) == BytecodeLoadError::BadOperand);
    // WAITEV(SparksRuntime 2.5절): 이벤트가 아닌 참조, frame 밖 레지스터. CALLW: 함수가 아닌 참조.
    uint32 notEvent = 0;
    while (notEvent < original.refs.Size() &&
           original.refs[notEvent].kind == BytecodeRefKind::Event) {
        ++notEvent;
    }

    REQUIRE(notEvent < original.refs.Size());
    CHECK(patchExtra(Opcode::WaitEv, notEvent) == BytecodeLoadError::BadReference);
    CHECK(patch(Opcode::WaitEv, [&](uint32 w) noexcept { return abc(w, 255, DecodeB(w), 0); }) ==
          BytecodeLoadError::BadRegister);
    uint32 notFunction = 0;
    while (notFunction < original.refs.Size() &&
           (original.refs[notFunction].kind == BytecodeRefKind::Function ||
            original.refs[notFunction].kind == BytecodeRefKind::ImportedFunction)) {
        ++notFunction;
    }

    REQUIRE(notFunction < original.refs.Size());
    CHECK(patch(Opcode::CallW, [&](uint32) noexcept {
              return EncodeABx(Opcode::CallW, 0, notFunction);
          }) == BytecodeLoadError::BadReference);
    // CALLW는 Ref 창 0번(하위 대기 handle 자리)이 있어야 한다(LC6c2 리뷰 B4).
    const uint32 spawning = functionOf(find(Opcode::CallW));
    CHECK(verify([&](BytecodeModule& m) noexcept {
              m.functions[spawning].frame[1] = m.functions[spawning].registers[1];
          }) == BytecodeLoadError::BadRegister);
    // 형식 v4(SparksRuntime 2.6절): held 구간이 할당 수 밖, CLASS 필드의 모르는 표지·Scalar held.
    CHECK(verify([&](BytecodeModule& m) noexcept {
              m.functions[0].heldFirst = m.functions[0].registers[1];
              m.functions[0].heldCount = 1;
          }) == BytecodeLoadError::BadFunction);
    REQUIRE(!original.classFields.IsEmpty());
    CHECK(verify([](BytecodeModule& m) noexcept { m.classFields[0].flags = 2; }) ==
          BytecodeLoadError::BadTable);
    CHECK(verify([](BytecodeModule& m) noexcept {
              for (BytecodeTypeField& field : m.classFields) {
                  if (field.bank == 0) {
                      field.flags = BytecodeFieldHeld;
                      return;
                  }
              }
          }) == BytecodeLoadError::BadTable);
    // await 표에서 빠진 AWAIT, 같은 주소의 await 항목 둘.
    const uint32 awaiting = functionOf(find(Opcode::Await));
    CHECK(verify([&](BytecodeModule& m) noexcept { m.functions[awaiting].awaitCount = 0; }) ==
          BytecodeLoadError::BadBranch);
    // 같은 모듈 함수 호출: 부르는 함수의 매개변수가 호출 창보다 크다.
    const uint32 caller = functionOf(find(Opcode::Call));
    CHECK(verify([&](BytecodeModule& m) noexcept {
              for (uint32 index = 0; index < m.functions.Size(); ++index) {
                  if (index != caller) {
                      m.functions[index].parameters[0] = 200;
                      m.functions[index].registers[0] = 200;
                      m.functions[index].frame[0] = 200;
                  }
              }
          }) == BytecodeLoadError::BadRegister);
    CHECK(verify([](BytecodeModule& m) noexcept {
              m.words[m.functions[0].firstWord + m.functions[0].wordCount - 1] =
                  EncodeABC(Opcode::MovS, 0, 0, 0);
          }) == BytecodeLoadError::BadEnd);
    CHECK(verify([](BytecodeModule& m) noexcept { m.functions[0].wordCount += 100000; }) ==
          BytecodeLoadError::BadFunction);
    CHECK(verify([](BytecodeModule& m) noexcept { m.functions[0].frame[0] = 300; }) ==
          BytecodeLoadError::BadFunction);
    CHECK(verify([](BytecodeModule& m) noexcept { m.functions[0].returnSlots = 30; }) ==
          BytecodeLoadError::BadFunction);
    CHECK(verify([](BytecodeModule& m) noexcept { m.functions[0].name = 9999; }) ==
          BytecodeLoadError::BadFunction);
    CHECK(verify([](BytecodeModule& m) noexcept { m.refs[0].name = 9999; }) ==
          BytecodeLoadError::BadTable);
    CHECK(verify([&](BytecodeModule& m) noexcept {
              for (BytecodeRef& ref : m.refs) {
                  if (ref.kind == BytecodeRefKind::Function) {
                      ref.detail = NoBytecodeIndex;
                  }
              }
          }) == BytecodeLoadError::BadTable);
    CHECK(verify([&](BytecodeModule& m) noexcept {
              const usize at = find(Opcode::Call);
              m.words[at] = EncodeABx(Opcode::CallN, 0, DecodeBx(m.words[at]));
          }) == BytecodeLoadError::BadReference);
    CHECK(verify([](BytecodeModule& m) noexcept { m.cases[0].target = 9999; }) ==
          BytecodeLoadError::BadBranch);
    CHECK(verify([](BytecodeModule& m) noexcept {
              static_cast<void>(m.locations.TryResize(m.locations.Size() - 1));
          }) == BytecodeLoadError::BadTable);

    // CLASS 구역(형식 버전 3 — SparksRuntime 2.1절): 이름·기반(앞선 클래스)·필드 위치·hook·handler·
    // 이벤트·초기화 함수.
    REQUIRE(original.classes.Size() == 1);
    REQUIRE(original.classes[0].fieldCount == 1);
    REQUIRE(original.classes[0].hookCount == 1);
    REQUIRE(original.classes[0].eventCount == 1);
    CHECK(verify([](BytecodeModule& m) noexcept { m.classes[0].name = 9999; }) ==
          BytecodeLoadError::BadTable);
    CHECK(verify([](BytecodeModule& m) noexcept { m.classes[0].baseClass = 0; }) ==
          BytecodeLoadError::BadTable);
    CHECK(verify([](BytecodeModule& m) noexcept { m.classes[0].fieldCount = 9; }) ==
          BytecodeLoadError::BadTable);
    CHECK(verify([](BytecodeModule& m) noexcept { m.classFields[0].offset = 5; }) ==
          BytecodeLoadError::BadTable);
    CHECK(verify([](BytecodeModule& m) noexcept {
              m.classFields[0].bank = 1;
              m.classFields[0].slots = 2;
          }) == BytecodeLoadError::BadTable);
    CHECK(verify([](BytecodeModule& m) noexcept { m.hooks[0].function = 9999; }) ==
          BytecodeLoadError::BadTable);
    CHECK(verify([](BytecodeModule& m) noexcept {
              static_cast<void>(m.hooks.TryPushBack(m.hooks[0])); // 같은 종류 두 번
              ++m.classes[0].hookCount;
          }) == BytecodeLoadError::BadTable);
    CHECK(verify([](BytecodeModule& m) noexcept { m.events[0].name = 9999; }) ==
          BytecodeLoadError::BadTable);
    CHECK(verify([](BytecodeModule& m) noexcept { m.classes[0].initializer = 9999; }) ==
          BytecodeLoadError::BadTable);
    CHECK(verify([](BytecodeModule& m) noexcept {
              m.classes[0].initializer = 0; // 함수 0은 this(Ref 1) 하나의 모양이 아니다
          }) == BytecodeLoadError::BadTable);
    // 밀집 배치: bank별 필드 합이 클래스 slot 수(LC6a 리뷰 S3).
    CHECK(verify([](BytecodeModule& m) noexcept { m.classes[0].slots[0] = 9; }) ==
          BytecodeLoadError::BadTable);
    // 같은 이름의 클래스 둘.
    CHECK(verify([](BytecodeModule& m) noexcept {
              BytecodeClass copy;
              copy.name = m.classes[0].name;
              static_cast<void>(m.classes.TryPushBack(copy));
          }) == BytecodeLoadError::BadTable);
    // 재진입 표시는 await가 있는 handler만.
    CHECK(verify([](BytecodeModule& m) noexcept {
              BytecodeHandler handler;
              handler.path = 0;
              handler.event = 0;
              handler.function = 0;
              handler.reentry = BytecodeReentry::Restart;
              m.classes[0].firstHandler = static_cast<uint32>(m.handlers.Size());
              m.classes[0].handlerCount = 1;
              static_cast<void>(m.handlers.TryPushBack(handler));
          }) == BytecodeLoadError::BadTable);
    // hook 함수 모양: this(Ref)가 없는 함수(최상위 Twice)는 hook이 될 수 없다.
    CHECK(verify([&](BytecodeModule& m) noexcept {
              for (uint32 index = 0; index < m.functions.Size(); ++index) {
                  if (m.String(m.functions[index].name) == "Twice") {
                      m.hooks[0].function = index;
                  }
              }
          }) == BytecodeLoadError::BadTable);
    // handler 경로 참조는 필드·문맥 참조, 이벤트 참조는 Event다.
    CHECK(verify([](BytecodeModule& m) noexcept {
              BytecodeHandler handler;
              handler.path = 0;
              handler.event = 0;
              handler.function = 0;
              handler.eventRef = 0; // 참조 0은 Event가 아니다
              m.classes[0].firstHandler = static_cast<uint32>(m.handlers.Size());
              m.classes[0].handlerCount = 1;
              static_cast<void>(m.handlers.TryPushBack(handler));
          }) == BytecodeLoadError::BadTable);

    // 머리의 profile은 0·1이다.
    Array<uint8> copy(allocator);
    REQUIRE(copy.TryAppend(file.View()));
    constexpr usize ProfileOffset = 4 + 4 + 4 + 4 + 32 + 32;
    copy[ProfileOffset] = 7;
    CHECK(ReadError(allocator, copy.View()) == BytecodeLoadError::BadTable);
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
