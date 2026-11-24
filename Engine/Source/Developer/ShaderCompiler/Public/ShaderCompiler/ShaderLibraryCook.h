#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Platform/File.h>
#include <ShaderCompiler/ShaderCompileManager.h>
#include <ShaderCore/ShaderLibrary.h>
#include <ShaderCore/ShaderLibraryContents.h>
#include <ShaderCore/ShaderTypeRegistry.h>

// 셰이더 라이브러리 쿠킹이다(Docs/Design/ShaderSystem 12.2.1절 SSD5·SSD6·SSD28, UE
// FShaderLibraryCooker·FShaderCodeLibrary 쿠킹 쪽 대응).
//
// 형식마다 등록부의 타입 가운데 소속이 라이브러리에 맞는 것을 결정적 순서(타입 이름 해시 → 정점
// 입력 타입 id → 퍼뮤테이션 ID)로 열거하고 거른다(SSD5): (1) 요구 기능 ⊆ 형식 기능, (2) 개발 전용
// 타입· 퍼뮤테이션은 includeDevelopment일 때만, (3) 타입 거르기, (4) 정점 입력 축이면 정점 입력
// 타입 거르기. 통과한 조합마다 job 하나를 만들어(정의 = 퍼뮤테이션 차원 값, 옵션 = 형식 기본값에
// 타입의 hlsl2021, 레이아웃 서명 = 타입 표 목록의 서명) 관리자 Compile로 한 번에 컴파일하고, 결과로
// 라이브러리 v1 byte(BuildShaderLibrary)를 만들어 쓴다.
//
// - job 실패는 report.failures에 모으고 그 형식의 파일은 쓰지 않는다(JobsFailed).
// - 파일: <outputDirectory>/<Engine|Project>-<형식 id>-<파일 SHA-256 앞 16 hex>.rshl. 임시 파일에
// 다 쓰고
//   Flush한 뒤 덮어쓰지 않는 이동(Move replaceExisting = false)으로 게시한다. 같은 이름이 이미
//   있으면 byte가 같을 때 그대로 두고(Unchanged), 다르면 손상으로 보고 손대지 않는다(CorruptEntry).
// - 매니페스트·게시 묶음·정리는 SH2d(비에셋 산출물 훅)다.
// - 한계: 정점 입력 타입이 주는 정의(RenderBase 10절)와 개발 전용 표지의 엔진 정의
//   ROCKS_DEBUG_BUFFER(7.2절 12항)는 아직 넣지 않는다. 축은 열거하지만 원문이 같으면 코드가 같아
//   라이브러리에 한 번만 담긴다.
//
// 사용 예(쿠커):
//   ShaderLibraryCookDesc desc;
//   desc.allocator = &allocator;
//   desc.registry = &registry;                  // Freeze됨
//   desc.formats = snapshots;                   // LoadShaderFormatSnapshot
//   desc.manager = &manager;                    // Start됨
//   desc.outputDirectory = "Saved/Cooked/Windows/Shaders";
//   ShaderLibraryCookReport report(allocator);
//   if (auto cooked = CookShaderLibraries(desc, report); !cooked) {
//       for (const ShaderLibraryCookJobFailure& failure : report.failures) { ... }
//   }
namespace rocks::shader {

struct ShaderLibraryCookDesc {
    Allocator* allocator = nullptr;
    // Freeze된 등록부다.
    const ShaderTypeRegistry* registry = nullptr;
    // 형식 snapshot이다(이 순서로 쿠킹하고 보고한다).
    Span<const ShaderFormatSnapshot* const> formats;
    Span<const ShaderVertexInputType> vertexInputTypes;
    ShaderLibraryKind library = ShaderLibraryKind::Engine;
    // 개발 전용 타입·퍼뮤테이션을 담는가(SSD11 cookDevelopmentShaders).
    bool includeDevelopment = false;
    // Start된 관리자다.
    ShaderCompileManager* manager = nullptr;
    StringView outputDirectory;
    // 파일 시스템(null이면 실제 파일 시스템)이다.
    platform::FileSystem* files = nullptr;
};

enum class ShaderLibraryCookOutcome : uint8 {
    // 새 파일을 썼다.
    Written,
    // 같은 byte의 파일이 이미 있었다.
    Unchanged,
    // job이 실패해 쓰지 않았다(report.failures).
    JobsFailed,
    // 같은 이름의 다른 byte 파일이 있어 쓰지 않았다(AssetSystem CorruptEntry와 같은 판정).
    CorruptEntry,
    // 라이브러리 byte를 만들지 못했다(같은 키 둘 등 — 입력 오류).
    BuildFailed,
    // 파일 쓰기·이동이 실패했다.
    WriteFailed,
    // 메모리에 만들었다(CompileShaderLibrary — 파일을 쓰지 않는다).
    Built
};

ROCKS_SHADERCOMPILER_API const char*
ShaderLibraryCookOutcomeName(ShaderLibraryCookOutcome outcome) noexcept;

// 실패한 job 하나다.
struct ShaderLibraryCookJobFailure {
    explicit ShaderLibraryCookJobFailure(Allocator& allocator) noexcept : diagnostics(allocator) {
    }

    char formatId[MaxShaderFormatIdBytes] = {};
    char typeName[MaxShaderTypeNameBytes + 1] = {};
    VertexInputTypeId vertexInputType = NoVertexInputType;
    uint32 permutationId = 0;
    ShaderCompileStatus status = ShaderCompileStatus::InvalidJob;
    String diagnostics;
};

// 형식 하나의 결과다.
struct ShaderLibraryCookFormat {
    explicit ShaderLibraryCookFormat(Allocator& allocator) noexcept : path(allocator) {
    }

    char formatId[MaxShaderFormatIdBytes] = {};
    ShaderLibraryCookOutcome outcome = ShaderLibraryCookOutcome::BuildFailed;
    // 라이브러리 파일 경로(Written·Unchanged·CorruptEntry)다.
    String path;
    Sha256Digest fileSha256{};
    uint32 jobs = 0;
    uint32 failedJobs = 0;
    uint32 entries = 0;
    uint32 codes = 0;
    // BuildFailed·WriteFailed의 영어 이유(정적 수명)다.
    const char* reason = "";
};

struct ShaderLibraryCookReport {
    explicit ShaderLibraryCookReport(Allocator& allocator) noexcept
        : formats(allocator), failures(allocator) {
    }

    Array<ShaderLibraryCookFormat> formats;
    Array<ShaderLibraryCookJobFailure> failures;
};

// desc대로 형식마다 라이브러리를 쿠킹한다(위 설명). report는 먼저 비우고 형식마다 한 줄을 남긴다.
// 반환: 모든 형식이 Written·Unchanged면 성공. 아니면 첫 실패 형식의 오류(JobsFailed
// DependencyFailed, CorruptEntry AlreadyExists, BuildFailed InvalidArgument, WriteFailed 파일
// 오류)다. 입력 오류: allocator·registry·manager가 null이거나 출력 디렉터리가 비었거나 정점 입력
// 타입이 잘못됨(id 0·이름 해시 불일치·중복) InvalidArgument, 등록부가 Freeze되지 않았으면 NotReady.
// 할당 실패는 OutOfMemory다. Compile 자체의 오류(관리자 미시작 등)는 그대로 돌려준다.
ROCKS_SHADERCOMPILER_API Result<void> CookShaderLibraries(const ShaderLibraryCookDesc& desc,
                                                          ShaderLibraryCookReport& report) noexcept;

// Development 공급자의 메모리 라이브러리다(Docs/Design/ShaderSystem 12.3.1절 SSD16, SH2e).
// desc.formats는 하나여야 하고 outputDirectory는 쓰지 않는다. CookShaderLibraries와 같은
// 열거·컴파일·만들기라 같은 형식· 등록부·includeDevelopment면 out은 쿠킹 파일과 같은 byte다(완료
// 기준). 결과 줄은 Built다. 오류는 CookShaderLibraries와 같다(파일 오류 없음).
ROCKS_SHADERCOMPILER_API Result<void> CompileShaderLibrary(const ShaderLibraryCookDesc& desc,
                                                           ShaderLibraryCookReport& report,
                                                           Array<uint8>& out) noexcept;

} // namespace rocks::shader
