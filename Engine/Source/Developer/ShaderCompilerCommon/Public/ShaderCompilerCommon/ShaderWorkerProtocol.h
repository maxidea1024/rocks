#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>

// 셰이더 컴파일 워커 메시지와 종료 코드다(Docs/Design/ShaderSystem.ko.md 11절, 11.1절 SC4–SC7).
// 메인 프로세스(ShaderCompiler의 워커 채널)와 워커(ShaderWorker)가 같은 정의를 쓴다.
//
// 파일 형식(little-endian):
//   입력 `input.<seq>`  = "RSWI" + u32 메시지 버전 + Record(요청)
//   출력 `output.<seq>` = "RSWO" + u32 메시지 버전 + Record(응답)
// Record는 Serialization Record TLV다(schema·필드 Guid는 구현에 고정). 파일 하나는
// MaxShaderWorkerFileBytes를 넘지 않는다.
//
// 요청 머리(워커가 자기 것과 대조 — SC5·SC6): 도구 표 SHA-256(ShaderToolTableSha256), 툴체인 지문,
// 결정적 수학 버전, 형식별 처리기 버전 표(형식 id → 형식 정의 processor.version). 다르면 워커는
// 출력 없이 ShaderWorkerExit 코드로 끝난다.
//
// 사용 예(메인 프로세스):
//   ShaderWorkerRequest request(allocator);
//   request.kind = ShaderWorkerRequestKind::Compile;
//   const ShaderFormatDefinition* formats[] = {&format};
//   MakeShaderWorkerHeader(formats, request.header);
//   ... request.formats·jobs ...
//   Array<uint8> bytes(allocator);
//   if (auto encoded = EncodeShaderWorkerRequest(request, bytes); !encoded) { ... }
//
// 스레드: 모든 함수는 재진입 가능하며 전역 상태를 쓰지 않는다.
namespace rocks::shader {

// 메시지 형식 버전이다(필드·의미를 바꾸면 올린다 — 다른 버전의 워커는 BadMessageVersion으로
// 끝난다).
inline constexpr uint32 ShaderWorkerMessageVersion = 1;
// 파일 앞 magic(4 byte)과 버전(u32)의 크기다.
inline constexpr usize ShaderWorkerMagicBytes = 4;
inline constexpr usize ShaderWorkerPrefixBytes = ShaderWorkerMagicBytes + sizeof(uint32);
inline constexpr uint8 ShaderWorkerInputMagic[ShaderWorkerMagicBytes] = {'R', 'S', 'W', 'I'};
inline constexpr uint8 ShaderWorkerOutputMagic[ShaderWorkerMagicBytes] = {'R', 'S', 'W', 'O'};
// 입력·출력 파일 하나의 최대 byte 수(1 GiB)다. 묶음은 이보다 크게 만들지 않는다.
inline constexpr uint64 MaxShaderWorkerFileBytes = uint64{1} * 1024 * 1024 * 1024;
// system 도구 지문의 실행 파일 수(metal·metallib)다.
inline constexpr usize ShaderWorkerSystemToolExecutables = 2;

// 워커 종료 코드다(UE FSCWErrorCode 대응, SC6). BadToolchain은 Rocks가 더했다(다른 컴파일러·결정적
// 수학 버전으로 빌드된 워커).
enum class ShaderWorkerExit : int32 {
    Success = 0,
    GeneralFailure = 1,
    BadMessageVersion = 2,
    BadFormatVersion = 3,
    BadToolTable = 4,
    BadInputFile = 5,
    CannotDeleteInput = 6,
    CannotSaveOutput = 7,
    BadArguments = 8,
    OutOfMemory = 9,
    BadToolchain = 10
};

// 진단용 이름이다(정적 수명). 코드 밖 값은 "unknown".
ROCKS_SHADERCOMPILERCOMMON_API const char* ShaderWorkerExitName(int32 code) noexcept;

// 관리자가 job에 붙이는 진단 문구(영어, 정적 수명)다: 버전·툴체인 불일치는 워커를 다시 빌드하라는
// 안내, 도구 표 불일치는 다른 빌드의 도구 표라는 안내, 그 밖은 일반 문구다.
ROCKS_SHADERCOMPILERCOMMON_API const char* ShaderWorkerExitDiagnostic(int32 code) noexcept;

enum class ShaderWorkerRequestKind : uint8 {
    // 저장소 도구 확인과 system 도구 지문(11절 3항).
    ToolQuery = 1,
    Compile = 2
};

// job 종류다. Compile 밖은 `--test-hooks`로 띄운 워커만 받는 시험 hook(SC7)이다.
enum class ShaderWorkerJobKind : uint8 {
    Compile = 0,
    // 출력 pipe에 `held <seq>`를 쓰고 입력 pipe의 다음 줄까지 막힌다.
    Hold = 1,
    // hookArguments(실행 파일, 인자…)를 P1 Process로 자식으로 띄우고 `child <pid>`를 쓴 뒤 Hold.
    SpawnChild = 2,
    // 접근 위반.
    Crash = 3,
    // Fatal 경로.
    Assert = 4
};

enum class ShaderWorkerJobStatus : uint8 {
    Success = 0,
    CompileError = 1,
    // 도구를 열거나 부르지 못했다(저장소 없음·SHA 불일치·호출 실패).
    ToolError = 2,
    // 이 워커·호스트가 그 형식·경로를 만들지 못한다.
    UnsupportedCapability = 3
};

// 저장소 도구 확인 결과다(ShaderToolLoadCode를 메시지용으로 줄였다).
enum class ShaderWorkerToolState : uint8 {
    Ready = 0,
    NotBootstrapped = 1,
    DigestMismatch = 2,
    // 이 호스트 파일이 표에 없다.
    UnsupportedHost = 3,
    Failed = 4
};

struct ShaderWorkerProcessorVersion {
    char formatId[MaxShaderFormatIdBytes] = {};
    uint32 version = 0;
};

struct ShaderWorkerHeader {
    explicit ShaderWorkerHeader(Allocator& allocator) noexcept : processors(allocator) {
    }

    Sha256Digest toolTable;
    Sha256Digest toolchain;
    uint32 mathVersion = 0;
    Array<ShaderWorkerProcessorVersion> processors;
};

// 형식 정의 snapshot byte다(키에 넣은 바로 그 byte — 워커가 같은 파서로 읽는다).
struct ShaderWorkerFormat {
    explicit ShaderWorkerFormat(Allocator& allocator) noexcept : id(allocator), bytes(allocator) {
    }

    String id;
    Array<uint8> bytes;
};

// 컴파일 옵션의 메시지 표현이다(ShaderCompileOptions는 메인 프로세스 모듈에 있다 — SC9).
struct ShaderWorkerJobOptions {
    bool hlsl2021 = false;
    // 0–3(O0–O3).
    uint8 optimization = 3;
    bool debugSymbols = false;
    // 0(Row).
    uint8 matrixLayout = 0;
};

struct ShaderWorkerJob {
    explicit ShaderWorkerJob(Allocator& allocator) noexcept
        : formatId(allocator), entry(allocator), source(allocator), layout(allocator),
          hookArguments(allocator) {
    }

    ShaderWorkerJobKind kind = ShaderWorkerJobKind::Compile;
    Sha256Digest key;
    String formatId;
    // ShaderStageBits 한 비트(SC8).
    uint32 stage = 0;
    String entry;
    // 전처리된 원문(가상 경로 #line).
    String source;
    ShaderWorkerJobOptions options;
    // 파라미터 레이아웃 표 정규 byte(없으면 빈 값 — SH2).
    Array<uint8> layout;
    // SpawnChild의 실행 파일과 인자다.
    Array<String> hookArguments;
};

struct ShaderWorkerRequest {
    explicit ShaderWorkerRequest(Allocator& allocator) noexcept
        : header(allocator), formats(allocator), jobs(allocator) {
    }

    ShaderWorkerRequestKind kind = ShaderWorkerRequestKind::Compile;
    ShaderWorkerHeader header;
    Array<ShaderWorkerFormat> formats;
    Array<ShaderWorkerJob> jobs;
};

// 결과 값 하나다(ValueId — ShaderNuggetValue 번호, 10절).
struct ShaderWorkerValue {
    explicit ShaderWorkerValue(Allocator& allocator) noexcept : bytes(allocator) {
    }

    uint32 id = 0;
    Array<uint8> bytes;
};

struct ShaderWorkerJobResult {
    explicit ShaderWorkerJobResult(Allocator& allocator) noexcept
        : values(allocator), diagnostics(allocator) {
    }

    ShaderWorkerJobStatus status = ShaderWorkerJobStatus::Success;
    // ValueId 오름차순이다.
    Array<ShaderWorkerValue> values;
    // 실패 job의 진단(영어, 도구 원문)이다. 성공 job은 비어 있다.
    String diagnostics;
};

struct ShaderWorkerToolCheck {
    explicit ShaderWorkerToolCheck(Allocator& allocator) noexcept : name(allocator) {
    }

    String name;
    ShaderWorkerToolState state = ShaderWorkerToolState::Failed;
};

// system 도구(Apple MetalToolchain) 지문이다. found가 false면 이 호스트에 없다.
struct ShaderWorkerSystemTool {
    explicit ShaderWorkerSystemTool(Allocator& allocator) noexcept
        : name(allocator), version(allocator) {
    }

    String name;
    bool found = false;
    String version;
    Sha256Digest executables[ShaderWorkerSystemToolExecutables] = {};
};

struct ShaderWorkerResponse {
    explicit ShaderWorkerResponse(Allocator& allocator) noexcept
        : jobs(allocator), tools(allocator), systemTools(allocator) {
    }

    // 요청 job 순서 그대로다(Compile).
    Array<ShaderWorkerJobResult> jobs;
    // 저장소 도구 확인(ToolQuery, 도구 표 순서).
    Array<ShaderWorkerToolCheck> tools;
    Array<ShaderWorkerSystemTool> systemTools;
};

// tools(도구 표)의 정규 인코딩 SHA-256이다(이름·버전·출처·저장소 키·이 호스트 파일의 경로·SHA·크기·
// role — 길이 접두 UTF-8, little-endian). 워커와 메인 프로세스가 같은 생성 표에서 같은 값을 낸다.
ROCKS_SHADERCOMPILERCOMMON_API Sha256Digest
ShaderToolTableSha256(Span<const ShaderTool> tools = ShaderTools()) noexcept;

// 이 빌드의 요청 머리를 out에 채운다: 도구 표 SHA, build::ToolchainFingerprint(),
// deterministic::Version, formats의 (id, moduleVersion). 오류: 할당 실패.
ROCKS_SHADERCOMPILERCOMMON_API Result<void>
MakeShaderWorkerHeader(Span<const ShaderFormatDefinition* const> formats,
                       ShaderWorkerHeader& out) noexcept;

// 파일 앞(magic·버전)을 읽는다. 짧거나 magic이 다르면 InvalidArgument, 그 밖은 버전 값이다.
ROCKS_SHADERCOMPILERCOMMON_API Result<uint32>
ReadShaderWorkerMessageVersion(Span<const uint8> file,
                               const uint8 (&magic)[ShaderWorkerMagicBytes]) noexcept;

// 요청·응답 파일 byte(magic·버전 포함)를 만든다. out은 먼저 비운다. 오류: 크기 한도 초과
// CapacityExceeded, 할당 실패 OutOfMemory, 잘못된 값(UTF-8 아닌 문자열) InvalidArgument.
ROCKS_SHADERCOMPILERCOMMON_API Result<void>
EncodeShaderWorkerRequest(const ShaderWorkerRequest& request, Array<uint8>& out) noexcept;
ROCKS_SHADERCOMPILERCOMMON_API Result<void>
EncodeShaderWorkerResponse(const ShaderWorkerResponse& response, Array<uint8>& out) noexcept;

// 파일 byte를 읽는다(out은 먼저 비운다). magic이 다르거나 잘렸거나 Record·값이 손상되면
// InvalidArgument, 메시지 버전이 다르면 Unsupported, 할당 실패 OutOfMemory다. 실패하면 out은
// 정해지지 않은 상태다.
ROCKS_SHADERCOMPILERCOMMON_API Result<void>
DecodeShaderWorkerRequest(Span<const uint8> file, ShaderWorkerRequest& out) noexcept;
ROCKS_SHADERCOMPILERCOMMON_API Result<void>
DecodeShaderWorkerResponse(Span<const uint8> file, ShaderWorkerResponse& out) noexcept;

} // namespace rocks::shader
