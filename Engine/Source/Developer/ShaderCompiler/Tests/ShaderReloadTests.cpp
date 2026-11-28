#include "ShaderManagerTestSupport.h"

#include <Core/FileRead.h>
#include <Base/InitializerList.h>
#include <Core/Text/String.h>
#include <Platform/File.h>
#include <ShaderCompiler/ShaderCompileManager.h>
#include <ShaderCompiler/ShaderLibraryCook.h>
#include <ShaderCompiler/ShaderLibraryReloader.h>
#include <ShaderCompiler/ShaderWatchSettle.h>
#include <ShaderCompilerCommon/ShaderDebugSymbols.h>
#include <ShaderCore/ShaderLibrary.h>
#include <ShaderCore/ShaderTypeRegistry.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// SH4 시험이다(Docs/Design/ShaderSystem 16.3절): 형식별 디버그 심볼(값 3 — DXC PDB·FXC PDB·SPIR-V
// debug module, GLES 없음, 심볼을 켜도 SPIR-V 코드 byte 불변, 디버그 이름 part)과 키, 라이브러리
// 밖 심볼 파일(SSD74), 의존 지도와 다시 만들기(SSD69–SSD71 — 바뀐 include의 의존 셰이더, 무관한
// 원문, 같은 byte면 revision 그대로, 실패 시 이전 라이브러리 유지와 진단 위치, 없던 include 파일,
// 형식 둘). 변경은 Reload에 직접 넣는다(감시기 없음 — 시간에 기대지 않는다). 실제 워커를 쓰므로
// 워커나 도구 저장소가 없으면 SKIP이다.
using namespace rocks;

namespace shader_reload_tests_file {

using namespace rocks::shader;
using namespace rocks::shader_source_test;
using namespace rocks::shader_manager_test;

namespace {

// DXIL·DXBC container의 디버그 이름 part 표지("ILDN" — PDB 이름을 가리킨다)다.
constexpr uint8 DebugNamePart[] = {'I', 'L', 'D', 'N'};
// SPIR-V magic과 머리 word 수, OpLine opcode다(SPIR-V 명세 2.3·3.32.2).
constexpr uint32 SpirvMagic = 0x07230203u;
constexpr usize SpirvHeaderWords = 5;
constexpr uint32 SpirvOpLine = 8;
constexpr uint32 SpirvOpcodeMask = 0xFFFFu;
constexpr uint32 SpirvWordCountShift = 16;

bool ContainsBytes(Span<const uint8> haystack, Span<const uint8> needle) noexcept {
    for (usize at = 0; at + needle.Size() <= haystack.Size(); ++at) {
        if (memcmp(haystack.Data() + at, needle.Data(), needle.Size()) == 0) {
            return true;
        }
    }

    return false;
}

bool SpirvHasLine(Span<const uint8> module) noexcept {
    if (module.Size() < SpirvHeaderWords * sizeof(uint32) || module.Size() % sizeof(uint32) != 0) {
        return false;
    }

    uint32 magic = 0;
    memcpy(&magic, module.Data(), sizeof(magic));
    if (magic != SpirvMagic) {
        return false;
    }

    usize word = SpirvHeaderWords;
    const usize words = module.Size() / sizeof(uint32);
    while (word < words) {
        uint32 instruction = 0;
        memcpy(&instruction, module.Data() + word * sizeof(uint32), sizeof(instruction));
        const uint32 count = instruction >> SpirvWordCountShift;
        if ((instruction & SpirvOpcodeMask) == SpirvOpLine) {
            return true;
        }

        if (count == 0) {
            return false;
        }

        word += count;
    }

    return false;
}

constexpr char ReloadSource[] = "#include \"Color.rsh\"\n"
                                "float4 PSMain() : SV_Target { return ReloadColor; }\n";
constexpr char ReloadWithNewInclude[] = "#include \"Color.rsh\"\n"
                                        "#include \"New.rsh\"\n"
                                        "float4 PSMain() : SV_Target { return ReloadColor; }\n";
constexpr char OtherSource[] =
    "float4 PSMain() : SV_Target { return float4(0.0, 0.0, 1.0, 1.0); }\n";
constexpr char RedColor[] =
    "#pragma once\nstatic const float4 ReloadColor = float4(1.0, 0.0, 0.0, 1.0);\n";
constexpr char GreenColor[] =
    "#pragma once\nstatic const float4 ReloadColor = float4(0.0, 1.0, 0.0, 1.0);\n";
// 2줄에 모르는 이름이 있다(include 파일 안의 위치로 보고된다).
constexpr char BrokenColor[] = "#pragma once\nstatic const float4 ReloadColor = colr;\n";
constexpr char NewInclude[] = "#pragma once\n";
// 심볼 시험 원문이다(PS 하나).
constexpr char SymbolSource[] = "float4 PSMain(float4 position : SV_Position) : SV_Target {\n"
                                "    return float4(position.x * 0.5, 0.25, 0.75, 1.0);\n"
                                "}\n";
constexpr char SymbolEntry[] = "/Project/Symbol.rsf";

ROCKS_SHADER_TYPE_NAME(ReloadPixelName, "ReloadPixel");
ROCKS_SHADER_TYPE_NAME(OtherPixelName, "OtherPixel");
ROCKS_SHADER_TYPE_NAME(SymbolPixelName, "SymbolPixel");

constexpr ShaderTypeDesc ReloadPixel = MakeShaderTypeDesc(
    ReloadPixelName, "/Project/Reload.rsf", "PSMain", ShaderStagePixel, ShaderTypeOwner::Project);
constexpr ShaderTypeDesc OtherPixel = MakeShaderTypeDesc(
    OtherPixelName, "/Project/Other.rsf", "PSMain", ShaderStagePixel, ShaderTypeOwner::Project);
constexpr ShaderTypeDesc SymbolPixel = MakeShaderTypeDesc(
    SymbolPixelName, SymbolEntry, "PSMain", ShaderStagePixel, ShaderTypeOwner::Project);

bool Has(const Array<uint64>& types, uint64 type) noexcept {
    for (const uint64 value : types) {
        if (value == type) {
            return true;
        }
    }

    return false;
}

bool SameBytes(const Array<uint8>& left, const Array<uint8>& right) noexcept {
    return left.Size() == right.Size() &&
           (left.IsEmpty() || memcmp(left.Data(), right.Data(), left.Size()) == 0);
}

} // namespace

// 형식마다 값 3: D3D12_SM6은 DXC PDB(이름 있음, DXIL에 디버그 이름 part), D3D11_SM5는 FXC
// PDB(DXBC에 디버그 이름 part, 디버그 정보는 뗌), VULKAN_SM5는 OpLine이 든 SPIR-V(이름 없음, 값 0은
// 심볼을 끈 job과 같은 byte), GLES31_PC는 값 3 없음. 심볼을 켜면 키가 다르다.
TEST(ShaderDebugSymbols_EachFormatEmitsItsSymbols) {
    ManagerCase test;
    const char* skip = nullptr;
    if (!test.Make("debugsymbols", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderDebugSymbols_EachFormatEmitsItsSymbols: %s\n", skip);
        }

        return;
    }

    REQUIRE(test.fixture.Write("Project/Shaders/Symbol.rsf", SymbolSource));
    ShaderFormatSnapshot d3d11(TestAllocator());
    ShaderFormatSnapshot gles(TestAllocator());
    REQUIRE(LoadShaderFormatSnapshot(ShaderFormatDirectory, "D3D11_SM5", d3d11));
    REQUIRE(LoadShaderFormatSnapshot(ShaderFormatDirectory, "GLES31_PC", gles));
    ShaderCompileManager manager(test.Config("SymbolsWork"));
    REQUIRE(manager.Start());
    const ShaderFormatSnapshot* const formats[] = {&test.d3d12, &d3d11, &test.vulkan, &gles};
    constexpr usize FormatCount = 4;
    Array<ShaderCompileJob> jobs(TestAllocator());
    for (const bool symbols : {false, true}) {
        for (const ShaderFormatSnapshot* format : formats) {
            ShaderCompileJob job;
            job.format = format;
            job.entryPath = SymbolEntry;
            job.entryPoint = "PSMain";
            job.stage = ShaderStagePixel;
            job.options = DefaultShaderCompileOptions(format->definition);
            job.options.debugSymbols = symbols;
            REQUIRE(jobs.TryPushBack(job));
        }
    }

    Array<ShaderCompileJobResult> results(TestAllocator());
    REQUIRE(manager.Compile(jobs.View(), results));
    PrintFailures(results, results.Size());
    REQUIRE(results.Size() == FormatCount * 2);
    for (const ShaderCompileJobResult& result : results) {
        REQUIRE(result.status == ShaderCompileStatus::Success);
    }

    for (usize index = 0; index < FormatCount; ++index) {
        const ShaderCompileJobResult& plain = results[index];
        const ShaderCompileJobResult& debug = results[FormatCount + index];
        CHECK(!(plain.key == debug.key));
        CHECK(!plain.values.Has(ShaderNuggetValue::DebugSymbols));
    }

    const auto symbolsOf = [&results](usize index) noexcept {
        return DecodeShaderDebugSymbols(
            results[FormatCount + index].values.Part(ShaderNuggetValue::DebugSymbols).View());
    };
    const auto codeOf = [&results](usize index) noexcept {
        return results[index].values.Part(ShaderNuggetValue::Code).View();
    };
    const Span<const uint8> debugName(DebugNamePart, sizeof(DebugNamePart));

    // D3D12_SM6.
    auto dxil = symbolsOf(0);
    REQUIRE(dxil);
    CHECK(dxil.Value().kind == ShaderDebugSymbolKind::Pdb &&
          dxil.Value().name.EndsWith(".pdb", StringComparison::CaseSensitive) &&
          !dxil.Value().contents.IsEmpty());
    CHECK(ContainsBytes(codeOf(FormatCount + 0), debugName) &&
          !ContainsBytes(codeOf(0), debugName));

    // D3D11_SM5.
    auto dxbc = symbolsOf(1);
    REQUIRE(dxbc);
    CHECK(dxbc.Value().kind == ShaderDebugSymbolKind::Pdb && !dxbc.Value().name.IsEmpty() &&
          !dxbc.Value().contents.IsEmpty());
    CHECK(ContainsBytes(codeOf(FormatCount + 1), debugName) &&
          !ContainsBytes(codeOf(1), debugName));

    // VULKAN_SM5.
    auto spirv = symbolsOf(2);
    REQUIRE(spirv);
    CHECK(spirv.Value().kind == ShaderDebugSymbolKind::SpirvModule && spirv.Value().name.IsEmpty());
    CHECK(SpirvHasLine(spirv.Value().contents) && !SpirvHasLine(codeOf(2)));
    CHECK(codeOf(2).Size() == codeOf(FormatCount + 2).Size() &&
          memcmp(codeOf(2).Data(), codeOf(FormatCount + 2).Data(), codeOf(2).Size()) == 0);

    // GLES31_PC.
    CHECK(!results[FormatCount + 3].values.Has(ShaderNuggetValue::DebugSymbols));
    manager.Shutdown();
}

// 라이브러리 만들기의 심볼 파일(SSD74): 값 3은 라이브러리 byte에 없고 <dir>/<형식>/<이름>에 쓴다.
// 다시 만들면 같은 byte라 두고, 파일이 바뀌어 있으면 덮어쓴다. 심볼을 끄면 라이브러리가 다르다.
TEST(ShaderLibraryCook_WritesSymbolFilesOutsideTheLibrary) {
    ManagerCase test;
    const char* skip = nullptr;
    if (!test.Make("symbolfiles", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderLibraryCook_WritesSymbolFilesOutsideTheLibrary: %s\n", skip);
        }

        return;
    }

    REQUIRE(test.fixture.Write("Project/Shaders/Symbol.rsf", SymbolSource));
    ShaderTypeRegistry registry(TestAllocator());
    const ShaderTypeDesc* const types[] = {&SymbolPixel};
    REQUIRE(registry.Register(types));
    REQUIRE(registry.Freeze());
    ShaderCompileManager manager(test.Config("SymbolFilesWork"));
    REQUIRE(manager.Start());
    char symbols[ManagerPathBytes];
    REQUIRE(test.fixture.Child("Saved/ShaderSymbols", symbols, sizeof(symbols)));
    const ShaderFormatSnapshot* const formats[] = {&test.vulkan};
    ShaderLibraryCookDesc desc;
    desc.allocator = &TestAllocator();
    desc.registry = &registry;
    desc.formats = formats;
    desc.library = ShaderLibraryKind::Project;
    desc.manager = &manager;
    desc.debugSymbols = true;
    desc.symbolDirectory = StringView(symbols);
    ShaderLibraryCookReport report(TestAllocator());
    Array<uint8> withSymbols(TestAllocator());
    REQUIRE(CompileShaderLibrary(desc, report, withSymbols));
    REQUIRE(report.formats.Size() == 1);
    CHECK(report.formats[0].symbolFiles == 1 && report.formats[0].symbolFailures == 0);
    ShaderLibraryView view;
    REQUIRE(view.Open(withSymbols.View()));
    ShaderLibraryEntry entry;
    REQUIRE(view.Find(ShaderLibraryKey{0, SymbolPixelName.hash, NoVertexInputType, 0}, entry));
    CHECK(view.Value(entry, static_cast<uint32>(ShaderNuggetValue::DebugSymbols)).IsEmpty());

    // 파일 이름은 이름 없는 SPIR-V라 "<job 키 hex>.spv"다(같은 job을 관리자로 다시 컴파일해 키를
    // 얻는다).
    ShaderCompileJob job;
    job.format = &test.vulkan;
    job.entryPath = SymbolEntry;
    job.entryPoint = "PSMain";
    job.stage = ShaderStagePixel;
    job.options = DefaultShaderCompileOptions(test.vulkan.definition);
    job.options.debugSymbols = true;
    ShaderCompileJobResult direct(TestAllocator());
    REQUIRE(manager.CompileNow(job, direct));
    REQUIRE(direct.status == ShaderCompileStatus::Success);
    char hex[Sha256Digest::HexBytes + 1];
    direct.key.ToHex(hex);
    char file[ManagerPathBytes];
    snprintf(file, sizeof(file), "%s/VULKAN_SM5/%s.spv", symbols, hex);
    Array<uint8> written(TestAllocator());
    bool existed = false;
    REQUIRE(ReadWholeFile(platform::PhysicalFileSystem(), StringView(file), 1u << 24, written,
                          existed));
    REQUIRE(existed);
    auto expected =
        DecodeShaderDebugSymbols(direct.values.Part(ShaderNuggetValue::DebugSymbols).View());
    REQUIRE(expected);
    CHECK(written.Size() == expected.Value().contents.Size() &&
          memcmp(written.Data(), expected.Value().contents.Data(), written.Size()) == 0);

    // 같은 byte면 두고, 바뀐 파일은 덮어쓴다.
    ShaderLibraryCookReport again(TestAllocator());
    Array<uint8> second(TestAllocator());
    REQUIRE(CompileShaderLibrary(desc, again, second));
    CHECK(again.formats[0].symbolFiles == 0 && again.formats[0].symbolsUnchanged == 1);
    CHECK(SameBytes(withSymbols, second));
    REQUIRE(test.fixture.Write("Saved/ShaderSymbols/stale.txt", "x"));
    {
        platform::File stale;
        REQUIRE(stale.Open(file, platform::FileOpenMode::CreateAlways));
        REQUIRE(stale.WriteAll("stale", 5).status);
        REQUIRE(stale.Close());
    }

    ShaderLibraryCookReport rewritten(TestAllocator());
    REQUIRE(CompileShaderLibrary(desc, rewritten, second));
    CHECK(rewritten.formats[0].symbolFiles == 1);
    REQUIRE(ReadWholeFile(platform::PhysicalFileSystem(), StringView(file), 1u << 24, written,
                          existed));
    CHECK(written.Size() == expected.Value().contents.Size());

    // 심볼을 끈 라이브러리는 키가 달라 코드가 같아도 다른 job이고, 심볼 파일을 쓰지 않는다.
    desc.debugSymbols = false;
    ShaderLibraryCookReport plain(TestAllocator());
    Array<uint8> withoutSymbols(TestAllocator());
    REQUIRE(CompileShaderLibrary(desc, plain, withoutSymbols));
    CHECK(plain.formats[0].symbolFiles == 0 && plain.formats[0].symbolsUnchanged == 0);
    manager.Shutdown();
}

// 의존 지도와 다시 만들기(SSD69–SSD71).
TEST(ShaderLibraryReloader_RebuildsDependentsAndKeepsTheOldLibraryOnFailure) {
    ManagerCase test;
    const char* skip = nullptr;
    if (!test.Make("reloader", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderLibraryReloader_RebuildsDependentsAndKeepsTheOldLibraryOnFailure: "
                   "%s\n",
                   skip);
        }

        return;
    }

    REQUIRE(test.fixture.Write("Project/Shaders/Reload.rsf", ReloadSource));
    REQUIRE(test.fixture.Write("Project/Shaders/Color.rsh", RedColor));
    REQUIRE(test.fixture.Write("Project/Shaders/Other.rsf", OtherSource));
    REQUIRE(test.fixture.Write("Project/Shaders/Unused.rsh", NewInclude));
    ShaderTypeRegistry registry(TestAllocator());
    const ShaderTypeDesc* const types[] = {&ReloadPixel, &OtherPixel};
    REQUIRE(registry.Register(types));
    REQUIRE(registry.Freeze());
    ShaderCompileManager manager(test.Config("ReloaderWork"));
    REQUIRE(manager.Start());
    ShaderLibraryReloaderDesc desc;
    desc.allocator = &TestAllocator();
    desc.registry = &registry;
    desc.library = ShaderLibraryKind::Project;
    desc.includeDevelopment = false;
    desc.manager = &manager;
    desc.formatDirectory = ShaderFormatDirectory;
    ShaderLibraryReloader libraries(desc);
    CHECK(libraries.Revision("D3D12_SM6") == 0 && libraries.FormatCount() == 0);
    ShaderLibraryCookReport built(TestAllocator());
    CHECK(libraries.Build("NO_SUCH_FORMAT", built).Error() == ErrorCode::NotFound);
    REQUIRE(libraries.Build("D3D12_SM6", built));
    CHECK(libraries.Build("D3D12_SM6", built));
    CHECK(libraries.Revision("D3D12_SM6") == 1 && libraries.FormatCount() == 1);
    Array<uint8> red(TestAllocator());
    auto copied = libraries.CopyLibrary("D3D12_SM6", red);
    REQUIRE(copied);
    CHECK(copied.Value() == 1 && !red.IsEmpty());
    CHECK(libraries.CopyLibrary("VULKAN_SM5", red).Error() == ErrorCode::NotFound);

    // 의존 지도: include는 그것을 읽은 타입만, Platform.rsh는 둘 다.
    Array<uint64> dependents(TestAllocator());
    REQUIRE(libraries.DependentTypes("/Project/Color.rsh", dependents));
    CHECK(dependents.Size() == 1 && Has(dependents, ReloadPixelName.hash));
    REQUIRE(libraries.DependentTypes("/Engine/Public/Platform.rsh", dependents));
    CHECK(dependents.Size() == 2 && Has(dependents, OtherPixelName.hash));
    REQUIRE(libraries.DependentTypes("/Project/Unused.rsh", dependents));
    CHECK(dependents.IsEmpty());

    // 무관한 원문은 다시 만들지 않는다.
    ShaderReloadReport report(TestAllocator());
    const StringView unused[] = {"/Project/Unused.rsh"};
    REQUIRE(libraries.Reload(unused, false, report));
    CHECK(report.outcome == ShaderReloadOutcome::NoShaderAffected && report.formats == 0);

    // include를 바꾸면 의존 타입이 다시 만들어지고 revision이 오른다.
    REQUIRE(test.fixture.Write("Project/Shaders/Color.rsh", GreenColor));
    const StringView color[] = {"/Project/Color.rsh"};
    REQUIRE(libraries.Reload(color, false, report));
    CHECK(report.outcome == ShaderReloadOutcome::Rebuilt && report.affectedTypes == 1 &&
          report.formats == 1 && report.changedFormats == 1 && report.failedJobs == 0);
    CHECK(libraries.Revision("D3D12_SM6") == 2);
    Array<uint8> green(TestAllocator());
    REQUIRE(libraries.CopyLibrary("D3D12_SM6", green));
    CHECK(!SameBytes(red, green));

    // 내용이 그대로면 byte가 같아 revision이 그대로다.
    REQUIRE(libraries.Reload(color, false, report));
    CHECK(report.outcome == ShaderReloadOutcome::Unchanged && report.changedFormats == 0);
    CHECK(libraries.Revision("D3D12_SM6") == 2);

    // 실패하면 이전 라이브러리를 유지하고 진단 위치를 낸다.
    REQUIRE(test.fixture.Write("Project/Shaders/Color.rsh", BrokenColor));
    REQUIRE(libraries.Reload(color, false, report));
    CHECK(report.outcome == ShaderReloadOutcome::Failed && report.failedJobs == 1 &&
          report.changedFormats == 0 && libraries.LastReloadFailed());
    CHECK(report.failedType.View() == StringView("ReloadPixel") &&
          report.failedFormat.View() == StringView("D3D12_SM6"));
    REQUIRE(!report.diagnostics.IsEmpty());
    CHECK(report.diagnostics[0].virtualPath.View() == StringView("/Project/Color.rsh") &&
          report.diagnostics[0].line == 2 && report.diagnostics[0].column != 0 &&
          report.diagnostics[0].severity == ShaderDiagnosticSeverity::Error &&
          report.diagnostics[0].message.View().Contains("colr"));
    Array<uint8> kept(TestAllocator());
    auto keptRevision = libraries.CopyLibrary("D3D12_SM6", kept);
    REQUIRE(keptRevision);
    CHECK(keptRevision.Value() == 2 && SameBytes(kept, green));

    // 고치면 이전 byte(초록)와 같아 revision 그대로이고 실패 표지가 풀린다.
    REQUIRE(test.fixture.Write("Project/Shaders/Color.rsh", GreenColor));
    REQUIRE(libraries.Reload(color, false, report));
    CHECK(report.outcome == ShaderReloadOutcome::Unchanged && !libraries.LastReloadFailed());

    // 없는 include를 더하면 실패(전처리 위치)이고, 그 파일을 만들면 지도에 없어도 다시 시도한다.
    REQUIRE(test.fixture.Write("Project/Shaders/Reload.rsf", ReloadWithNewInclude));
    const StringView entry[] = {"/Project/Reload.rsf"};
    REQUIRE(libraries.Reload(entry, false, report));
    CHECK(report.outcome == ShaderReloadOutcome::Failed);
    REQUIRE(!report.diagnostics.IsEmpty());
    CHECK(report.diagnostics[0].virtualPath.View() == StringView("/Project/Reload.rsf") &&
          report.diagnostics[0].line == 2 &&
          report.diagnostics[0].message.View().Contains("FileNotFound"));
    REQUIRE(test.fixture.Write("Project/Shaders/New.rsh", NewInclude));
    const StringView created[] = {"/Project/New.rsh"};
    REQUIRE(libraries.Reload(created, false, report));
    CHECK(report.outcome == ShaderReloadOutcome::Unchanged && report.affectedTypes == 0);
    REQUIRE(libraries.DependentTypes("/Project/New.rsh", dependents));
    CHECK(dependents.Size() == 1 && Has(dependents, ReloadPixelName.hash));

    // 이벤트 손실(everything)은 모든 타입이 영향받는다.
    REQUIRE(libraries.Reload({}, true, report));
    CHECK(report.outcome == ShaderReloadOutcome::Unchanged && report.affectedTypes == 2);

    // 형식 둘이면 둘 다 다시 만든다.
    ShaderLibraryCookReport vulkan(TestAllocator());
    REQUIRE(libraries.Build("VULKAN_SM5", vulkan));
    CHECK(libraries.FormatCount() == 2 && libraries.Revision("VULKAN_SM5") == 1);
    REQUIRE(test.fixture.Write("Project/Shaders/Color.rsh", RedColor));
    REQUIRE(libraries.Reload(color, false, report));
    CHECK(report.outcome == ShaderReloadOutcome::Rebuilt && report.formats == 2 &&
          report.changedFormats == 2);
    CHECK(libraries.Revision("D3D12_SM6") == 3 && libraries.Revision("VULKAN_SM5") == 2);
    CHECK(StringView(ShaderReloadOutcomeName(ShaderReloadOutcome::Failed)) == "Failed");
    manager.Shutdown();
}

// 감시 변경 묶기 판정(ShaderSystem 16.6절 SSD100)이다. 시각을 넣어 보므로 실제 시간에 기대지
// 않는다.
TEST(ShaderWatchSettle_WaitsForQuietOrLimit) {
    constexpr uint64 Millisecond = 1000000;
    constexpr uint32 Settle = ShaderWatchSettleMilliseconds;
    constexpr uint32 Limit = ShaderWatchSettleLimitMilliseconds;
    static_assert(ShaderWatchSettlePollMilliseconds < ShaderWatchSettleMilliseconds &&
                      ShaderWatchSettleMilliseconds < ShaderWatchSettleLimitMilliseconds,
                  "watch poll < settle < limit");

    // 사건 하나: settle 전에는 기다리고, settle이 지나면 시작한다.
    const uint64 first = 1000 * Millisecond;
    CHECK(!ShaderWatchSettled(first, first, first, Settle, Limit));
    CHECK(!ShaderWatchSettled(first, first, first + (Settle - 1) * Millisecond, Settle, Limit));
    CHECK(ShaderWatchSettled(first, first, first + Settle * Millisecond, Settle, Limit));

    // 사건이 이어지면 마지막 사건 기준으로 다시 기다린다.
    const uint64 last = first + 40 * Millisecond;
    CHECK(!ShaderWatchSettled(first, last, first + Settle * Millisecond, Settle, Limit));
    CHECK(ShaderWatchSettled(first, last, last + Settle * Millisecond, Settle, Limit));

    // 사건이 계속 와도 첫 사건 뒤 limit이 지나면 시작한다.
    const uint64 busy = first + (Limit - 1) * Millisecond;
    CHECK(!ShaderWatchSettled(first, busy, busy, Settle, Limit));
    CHECK(ShaderWatchSettled(first, first + Limit * Millisecond, first + Limit * Millisecond,
                             Settle, Limit));

    // 순서가 어긋난 시각은 기다림이 끝나지 않지 않도록 바로 시작한다.
    CHECK(ShaderWatchSettled(last, first, last, Settle, Limit));
    CHECK(ShaderWatchSettled(first, last, first, Settle, Limit));
}

} // namespace shader_reload_tests_file
