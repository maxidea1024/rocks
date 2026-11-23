#include <Core/Build/BuildInfo.h>
#include <Core/Memory/SystemAllocator.h>
#include <DeterministicMath/DeterministicMath.h>
#include <ShaderCompilerCommon/ShaderFormatBackend.h>
#include <ShaderCompilerCommon/ShaderWorkerProtocol.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;
namespace shader_worker_protocol_tests_file {

using namespace rocks::shader;

// 셰이더 워커 메시지(Docs/Design/ShaderSystem.ko.md 11.1절 SC5·SC6) 시험이다: 요청·응답 왕복, 손상
// 거부(잘림·magic·버전·byte 바꿈), 머리 채우기, 도구 표 SHA, 종료 코드 이름·진단, 프로필 이름.
namespace {

SystemAllocator& ProtocolAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

Sha256Digest Seeded(uint8 seed) noexcept {
    Sha256Digest digest;
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        digest.bytes[index] = static_cast<uint8>(seed + index);
    }

    return digest;
}

void FillRequest(ShaderWorkerRequest& request) noexcept {
    Allocator& allocator = ProtocolAllocator();
    request.kind = ShaderWorkerRequestKind::Compile;
    request.header.toolTable = Seeded(1);
    request.header.toolchain = Seeded(2);
    request.header.mathVersion = 7;
    ShaderWorkerProcessorVersion processor;
    memcpy(processor.formatId, "D3D12_SM6", 10);
    processor.version = 3;
    CHECK(request.header.processors.TryPushBack(processor));
    auto format = request.formats.TryEmplaceBack(allocator);
    REQUIRE(format);
    CHECK(format.Value()->id.TryAssign("D3D12_SM6"));
    const uint8 bytes[] = {'{', '}', 0x00, 0xff};
    CHECK(format.Value()->bytes.TryAppend(bytes));
    for (uint32 index = 0; index < 2; ++index) {
        auto job = request.jobs.TryEmplaceBack(allocator);
        REQUIRE(job);
        job.Value()->kind =
            index == 0 ? ShaderWorkerJobKind::Compile : ShaderWorkerJobKind::SpawnChild;
        job.Value()->key = Seeded(static_cast<uint8>(10 + index));
        CHECK(job.Value()->formatId.TryAssign("D3D12_SM6"));
        job.Value()->stage = index == 0 ? ShaderStagePixel : ShaderStageCompute;
        CHECK(job.Value()->entry.TryAssign("PSMain"));
        CHECK(job.Value()->source.TryAssign(
            "#line 1 \"/Engine/Private/A.rsf\"\nfloat4 PSMain() : SV_Target { return 1; }\n"));
        job.Value()->options.hlsl2021 = index == 0;
        job.Value()->options.optimization = static_cast<uint8>(index);
        CHECK(job.Value()->layout.TryAppend(bytes));
        if (index == 1) {
            auto first = job.Value()->hookArguments.TryEmplaceBack(allocator);
            auto second = job.Value()->hookArguments.TryEmplaceBack(allocator);
            REQUIRE(first && second);
            CHECK(first.Value()->TryAssign("C:/probe.exe") && second.Value()->TryAssign("sleep"));
        }
    }
}

} // namespace

TEST(ShaderWorkerProtocol_RequestRoundTrips) {
    ShaderWorkerRequest request(ProtocolAllocator());
    FillRequest(request);
    Array<uint8> bytes(ProtocolAllocator());
    REQUIRE(EncodeShaderWorkerRequest(request, bytes));
    CHECK(memcmp(bytes.Data(), "RSWI", 4) == 0);
    CHECK(ReadShaderWorkerMessageVersion(bytes.View(), ShaderWorkerInputMagic).Value() ==
          ShaderWorkerMessageVersion);

    ShaderWorkerRequest back(ProtocolAllocator());
    REQUIRE(DecodeShaderWorkerRequest(bytes.View(), back));
    CHECK(back.kind == ShaderWorkerRequestKind::Compile);
    CHECK(back.header.toolTable == Seeded(1) && back.header.toolchain == Seeded(2) &&
          back.header.mathVersion == 7);
    REQUIRE(back.header.processors.Size() == 1);
    CHECK(strcmp(back.header.processors[0].formatId, "D3D12_SM6") == 0 &&
          back.header.processors[0].version == 3);
    REQUIRE(back.formats.Size() == 1 && back.jobs.Size() == 2);
    CHECK(back.formats[0].id.View() == StringView("D3D12_SM6") &&
          back.formats[0].bytes.Size() == 4 && back.formats[0].bytes[3] == 0xff);
    for (usize index = 0; index < 2; ++index) {
        const ShaderWorkerJob& a = request.jobs[index];
        const ShaderWorkerJob& b = back.jobs[index];
        CHECK(a.kind == b.kind && a.key == b.key && a.stage == b.stage &&
              a.formatId.View() == b.formatId.View() && a.entry.View() == b.entry.View() &&
              a.source.View() == b.source.View() && a.options.hlsl2021 == b.options.hlsl2021 &&
              a.options.optimization == b.options.optimization &&
              a.layout.Size() == b.layout.Size() &&
              a.hookArguments.Size() == b.hookArguments.Size());
    }

    CHECK(back.jobs[1].hookArguments[1].View() == StringView("sleep"));

    // 같은 입력은 같은 byte다.
    Array<uint8> again(ProtocolAllocator());
    REQUIRE(EncodeShaderWorkerRequest(request, again));
    CHECK(again.Size() == bytes.Size() && memcmp(again.Data(), bytes.Data(), bytes.Size()) == 0);
}

TEST(ShaderWorkerProtocol_ResponseRoundTrips) {
    Allocator& allocator = ProtocolAllocator();
    ShaderWorkerResponse response(allocator);
    auto job = response.jobs.TryEmplaceBack(allocator);
    REQUIRE(job);
    job.Value()->status = ShaderWorkerJobStatus::Success;
    auto value = job.Value()->values.TryEmplaceBack(allocator);
    REQUIRE(value);
    value.Value()->id = 0;
    const uint8 code[] = {'D', 'X', 'B', 'C', 9};
    CHECK(value.Value()->bytes.TryAppend(code));
    auto failed = response.jobs.TryEmplaceBack(allocator);
    REQUIRE(failed);
    FailShaderJob(*failed.Value(), ShaderWorkerJobStatus::CompileError, "DXC compile failed",
                  "error: x");
    auto tool = response.tools.TryEmplaceBack(allocator);
    REQUIRE(tool);
    CHECK(tool.Value()->name.TryAssign("DirectXShaderCompiler"));
    tool.Value()->state = ShaderWorkerToolState::NotBootstrapped;
    auto system = response.systemTools.TryEmplaceBack(allocator);
    REQUIRE(system);
    CHECK(system.Value()->name.TryAssign("MetalToolchain"));
    system.Value()->found = true;
    CHECK(system.Value()->version.TryAssign("Apple metal version 1\nTarget: air64"));
    system.Value()->executables[1] = Seeded(5);

    Array<uint8> bytes(allocator);
    REQUIRE(EncodeShaderWorkerResponse(response, bytes));
    CHECK(memcmp(bytes.Data(), "RSWO", 4) == 0);
    ShaderWorkerResponse back(allocator);
    REQUIRE(DecodeShaderWorkerResponse(bytes.View(), back));
    REQUIRE(back.jobs.Size() == 2 && back.tools.Size() == 1 && back.systemTools.Size() == 1);
    CHECK(back.jobs[0].status == ShaderWorkerJobStatus::Success &&
          back.jobs[0].values.Size() == 1 && back.jobs[0].values[0].bytes.Size() == 5 &&
          back.jobs[0].diagnostics.IsEmpty());
    CHECK(back.jobs[1].status == ShaderWorkerJobStatus::CompileError &&
          back.jobs[1].values.IsEmpty() &&
          back.jobs[1].diagnostics.View() == StringView("DXC compile failed: error: x"));
    CHECK(back.tools[0].name.View() == StringView("DirectXShaderCompiler") &&
          back.tools[0].state == ShaderWorkerToolState::NotBootstrapped);
    CHECK(back.systemTools[0].found && back.systemTools[0].executables[1] == Seeded(5) &&
          back.systemTools[0].version.View() == StringView("Apple metal version 1\nTarget: air64"));
    // 요청 파일은 응답으로 읽지 않는다(magic이 다르다).
    ShaderWorkerRequest request(allocator);
    CHECK(DecodeShaderWorkerRequest(bytes.View(), request).Error() == ErrorCode::InvalidArgument);
}

TEST(ShaderWorkerProtocol_RejectsDamagedFiles) {
    ShaderWorkerRequest request(ProtocolAllocator());
    FillRequest(request);
    Array<uint8> bytes(ProtocolAllocator());
    REQUIRE(EncodeShaderWorkerRequest(request, bytes));
    ShaderWorkerRequest back(ProtocolAllocator());

    // 메시지 버전이 다르면 Unsupported, magic·잘림은 InvalidArgument다.
    Array<uint8> copy(ProtocolAllocator());
    REQUIRE(copy.TryAppend(bytes.View()));
    copy[ShaderWorkerMagicBytes] ^= 0x01;
    CHECK(DecodeShaderWorkerRequest(copy.View(), back).Error() == ErrorCode::Unsupported);
    copy[ShaderWorkerMagicBytes] ^= 0x01;
    copy[0] = 'X';
    CHECK(DecodeShaderWorkerRequest(copy.View(), back).Error() == ErrorCode::InvalidArgument);
    CHECK(!ReadShaderWorkerMessageVersion(copy.View(), ShaderWorkerInputMagic));
    CHECK(!ReadShaderWorkerMessageVersion(bytes.View().Left(3), ShaderWorkerInputMagic));
    // 모든 잘린 길이는 거부한다.
    for (usize size = 0; size < bytes.Size(); ++size) {
        const ErrorCode error = DecodeShaderWorkerRequest(bytes.View().Left(size), back).Error();
        if (error != ErrorCode::InvalidArgument) {
            fprintf(stderr, "truncated request of %zu bytes: error %u\n", static_cast<size_t>(size),
                    static_cast<unsigned>(error));
        }

        CHECK(error == ErrorCode::InvalidArgument);
    }

    // Record 머리·TLV의 byte를 바꾸면 거부한다(길이·wire tag·schema 영역).
    for (usize offset = ShaderWorkerPrefixBytes; offset < ShaderWorkerPrefixBytes + 72; ++offset) {
        Array<uint8> damaged(ProtocolAllocator());
        REQUIRE(damaged.TryAppend(bytes.View()));
        damaged[offset] ^= 0xA5;
        ShaderWorkerRequest decoded(ProtocolAllocator());
        const Result<void> result = DecodeShaderWorkerRequest(damaged.View(), decoded);
        CHECK(!result || decoded.jobs.Size() == 2);
    }
}

TEST(ShaderWorkerProtocol_HeaderAndToolTable) {
    ShaderFormatDefinition format;
    REQUIRE(LoadShaderFormatDefinition(ShaderFormatDirectory, "VULKAN_SM5", format));
    const ShaderFormatDefinition* formats[] = {&format};
    ShaderWorkerHeader header(ProtocolAllocator());
    REQUIRE(MakeShaderWorkerHeader(formats, header));
    CHECK(header.toolchain == build::ToolchainFingerprint());
    CHECK(header.mathVersion == deterministic::Version);
    CHECK(header.toolTable == ShaderToolTableSha256());
    REQUIRE(header.processors.Size() == 1);
    CHECK(strcmp(header.processors[0].formatId, "VULKAN_SM5") == 0 &&
          header.processors[0].version == format.moduleVersion);

    // 도구 표 SHA는 항목 내용을 따른다(빈 표·파일 SHA 하나 다름).
    ShaderToolFile file{"bin/a.dll", Seeded(1), 10, ShaderToolFileRole::Runtime, true};
    ShaderTool tool{"Tool", "1.0", ShaderToolOrigin::Build, "key", &file, 1};
    const Sha256Digest one = ShaderToolTableSha256(Span<const ShaderTool>(&tool, 1));
    CHECK(!(one == ShaderToolTableSha256(Span<const ShaderTool>())));
    file.sha256 = Seeded(2);
    CHECK(!(one == ShaderToolTableSha256(Span<const ShaderTool>(&tool, 1))));
    file.sha256 = Seeded(1);
    CHECK(one == ShaderToolTableSha256(Span<const ShaderTool>(&tool, 1)));
    tool.version = "1.1";
    CHECK(!(one == ShaderToolTableSha256(Span<const ShaderTool>(&tool, 1))));
}

TEST(ShaderWorkerProtocol_ExitCodesAndProfiles) {
    CHECK(static_cast<int32>(ShaderWorkerExit::BadMessageVersion) == 2 &&
          static_cast<int32>(ShaderWorkerExit::BadFormatVersion) == 3 &&
          static_cast<int32>(ShaderWorkerExit::BadToolTable) == 4 &&
          static_cast<int32>(ShaderWorkerExit::BadInputFile) == 5 &&
          static_cast<int32>(ShaderWorkerExit::BadArguments) == 8 &&
          static_cast<int32>(ShaderWorkerExit::BadToolchain) == 10);
    CHECK(strcmp(ShaderWorkerExitName(4), "BadToolTable") == 0);
    CHECK(strcmp(ShaderWorkerExitName(99), "unknown") == 0);
    CHECK(strstr(ShaderWorkerExitDiagnostic(2), "rebuild ShaderCompileWorker") != nullptr);
    CHECK(strstr(ShaderWorkerExitDiagnostic(10), "rebuild ShaderCompileWorker") != nullptr);
    CHECK(strstr(ShaderWorkerExitDiagnostic(4), "tool table") != nullptr);

    char profile[MaxShaderProfileBytes];
    CHECK(MakeShaderProfile(ShaderStagePixel, ShaderVersion{6, 6}, profile) &&
          strcmp(profile, "ps_6_6") == 0);
    CHECK(MakeShaderProfile(ShaderStageCompute, ShaderVersion{5, 0}, profile) &&
          strcmp(profile, "cs_5_0") == 0);
    CHECK(!MakeShaderProfile(ShaderStageVertex | ShaderStagePixel, ShaderVersion{6, 0}, profile));
}

} // namespace shader_worker_protocol_tests_file
