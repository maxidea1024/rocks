#pragma once

#include <AssetNugget/NuggetKey.h>
#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Guid.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Object/ObjectAccessContext.h>
#include <ShaderCompiler/ShaderCompileOptions.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>

// 셰이더 job 키다(Docs/Design/ShaderSystem 10절). 메인 프로세스가 NuggetKeyWriter에 10절 표의
// 1–7행 순서로 기록하고 그 SHA-256이 키다. 셰이더 job에는 AssetId·Build 의존 순환이 없으므로 SCC
// 식을 거치지 않고 이 키를 Record 키로 그대로 쓴다(bucket NuggetBucketShader).
//
// 1. 공통 머리 WriteNuggetKeyHeader(ShaderJobProcessor, 프로젝트 세대) — 결정적 수학 버전·툴체인
// 지문.
// 2. WriteList(도구 수) + 도구마다 WriteNuggetEncoder(이름, 버전, 설정): 형식 정의 tools 순서.
//    저장소 도구의 설정은 이 호스트 도구 표 파일 가운데 Runtime·Redist·Tool role의 SHA-256을 표
//    순서로 이은 byte(Symbols·Notice 제외), system 도구(Apple MetalToolchain)는 호출자가 준
//    지문(버전 줄· Target: 줄, metal·metallib SHA-256 64 byte — 워커 도구 조회 SC11이 채운다).
// 3. WriteString(형식 id), WriteDigest(형식 정의 byte SHA-256).
// 4. WriteU32(단계 — ShaderStageBits 한 비트), WriteString(엔트리).
// 5. WriteDigest(전처리 결과 SHA-256).
// 6. WriteReflected(ShaderCompileOptions), 레이아웃 서명 WriteDigest 또는 WriteNone.
// (7행 — 프로필 id·byte SHA는 개정 32에서 지웠다: 형식 정의 SHA가 3행에 있고 프로필의 형식
// 목록·순서는 출력 byte에 영향이 없다, 12.3.1절 SSD24.)
//
// 입력은 세션 시작 때 한 번 읽어 둔 snapshot이다(형식 정의의 sha256은 워커에 보내는 그 byte의
// 것). 키 함수는 디스크를 읽지 않고 도구를 열지 않는다.
//
// 사용 예:
//   ShaderJobKeyInput input;
//   input.projectGeneration = cache.ProjectGeneration();
//   input.format = &format;                     // 형식 정의 snapshot
//   input.stage = ShaderStagePixel;
//   input.entry = "PSMain";
//   input.preprocessedSha256 = Sha256::Hash(...preprocessed text bytes...);
//   input.options = &options;
//   input.reflection = {&registry, access, &types, &allocator};
//   auto key = BuildShaderJobKey(input);        // Result<Sha256Digest>
//
// 스레드: 재진입 가능하다. 리플렉션 문맥(registry·types)의 스레드 계약을 따른다.
namespace rocks {

class ObjectRegistry;
class TypeRegistry;

} // namespace rocks

namespace rocks::shader {

// 셰이더 job 처리기 하나다(형식마다 나누지 않는다 — 형식 쪽 출력 의미 변화는 형식 정의
// processor.version이 3행 SHA로 키에 들어간다). 키 기록 순서·번호 표를 바꾸면 버전 Guid를 바꾼다.
inline constexpr NuggetProcessorId ShaderJobProcessor{
    Guid{0x524f434b53534841ull, 0x4445524a4f420001ull},
    Guid{0x524f434b53534841ull, 0x4445524b45590002ull}};

// system 도구 지문의 실행 파일 수(metal·metallib)다.
inline constexpr usize ShaderSystemToolExecutables = 2;

// system 도구(형식 정의 도구 가운데 도구 표 origin이 System인 것) 하나의 지문이다. 워커 도구 조회
// (SH1c ToolQuery)가 채운다. view는 키 계산 동안 살아 있어야 한다.
struct ShaderSystemToolFingerprint {
    // 도구 표 이름(MetalToolchain)이다.
    StringView name;
    // `metal -v`의 버전 줄과 `Target:` 줄(경로가 든 줄은 뺀다)이다.
    StringView version;
    // xcrun --find로 고정한 metal·metallib 실행 파일의 SHA-256이다.
    Sha256Digest executables[ShaderSystemToolExecutables] = {};
};

// 옵션을 리플렉션으로 기록할 문맥이다. types에는 RegisterShaderCompilerTypes가 등록되어 있어야
// 한다.
struct ShaderKeyReflection {
    ObjectRegistry* registry = nullptr;
    ObjectAccessContext access{};
    TypeRegistry* types = nullptr;
    Allocator* allocator = nullptr;
};

// job 키 입력이다. 모든 포인터·view는 키 계산 동안 살아 있어야 한다.
struct ShaderJobKeyInput {
    // NuggetCache::ProjectGeneration()(엔진만 쓰면 null Guid)이다.
    Guid projectGeneration{};
    // Supported 형식 정의 snapshot이다(tools는 도구 표 항목을 가리킨다).
    const ShaderFormatDefinition* format = nullptr;
    // 형식이 system 도구를 쓰면 그 도구의 지문이다(없으면 UnsupportedCapability).
    Span<const ShaderSystemToolFingerprint> systemTools{};
    // ShaderStageBits 한 비트(SC8)다.
    uint32 stage = 0;
    StringView entry{};
    // PreprocessShader 결과 text byte의 SHA-256이다.
    Sha256Digest preprocessedSha256{};
    const ShaderCompileOptions* options = nullptr;
    ShaderKeyReflection reflection{};
    // 파라미터 레이아웃 서명(정규 인코딩 byte SHA-256)이다. 레이아웃이 없는 job은 null(WriteNone).
    const Sha256Digest* layoutSignature = nullptr;
};

// 키 2–7행을 writer에 쓴다(공통 머리 뒤). 시험이 공통 머리를 고정값으로 쓰는 기록 경로에도 쓴다.
// 오류: 형식이 null·자리만 둔 형식, 단계가 비트 하나가 아니거나 형식이 받지 않음, 엔트리·프로필
// id가 빔, 옵션·리플렉션 문맥이 null은 InvalidArgument. 옵션을 이 단계가 받지 않으면(debugSymbols)
// Unsupported. 이 호스트 파일이 없는 저장소 도구나 지문이 없는 system 도구는 UnsupportedCapability.
// 리플렉션 기록의 오류(타입 미등록 NotFound 등)는 그대로다. 실패하면 writer도 그 오류 상태다.
ROCKS_SHADERCOMPILER_API Result<void>
WriteShaderJobKeyBody(NuggetKeyWriter& writer, const ShaderJobKeyInput& input) noexcept;

// 키(공통 머리 + 2–7행의 SHA-256)다. trace가 있으면 기록 줄을 받는다(미적중 원인 추적·golden).
// 오류는 WriteShaderJobKeyBody와 같다.
ROCKS_SHADERCOMPILER_API Result<Sha256Digest>
BuildShaderJobKey(const ShaderJobKeyInput& input, NuggetKeyTrace* trace = nullptr) noexcept;

} // namespace rocks::shader
