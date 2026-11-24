#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Core/Diagnostics/LogCategory.h>
#include <Platform/DynamicLibrary.h>
#include <ShaderToolTable/ShaderToolTable.h>

// 셰이더 도구 열기 계층이다(Docs/Design/ShaderSystem 9절). 사용자 단위 저장소(bootstrap이
// 채우는 내용 주소 저장소, Rocks.Tooling.Common ThirdPartyStore)의 도구 파일을 도구 표
// (ShaderToolTable)와 대조한 뒤 P10 DynamicLibrary::Open으로 절대 경로에서 열고 함수 표를 얻는다.
// UE 5.8이 DXC·FXC를 전체 경로로 여는
// 방식(Developer/ShaderCompilerCommon/Private/DXCWrapper.cpp:69–81,
// Developer/Windows/ShaderFormatD3D/Private/D3DShaderCompiler.cpp:399–408)을
// SPIRV-Cross·SPIRV-Tools까지 넓힌 것이다.
//
// 참조 제한: 도구를 여는 것은 워커(Programs/ShaderCompileWorker)와 Developer/ShaderFormat*뿐이다.
// 워커가 생기기 전까지 이 모듈이 그 자리를 맡는다. 다른 모듈이 이 모듈을 참조하거나
// DynamicLibrary::Open을 부르면 정책 검사 shader-tool-binary-load가 거부한다.
//
// 열기 순서(ShaderToolLibrary::Open):
// 1. 표에서 이 호스트의 파일을 찾는다. 없으면 UnsupportedHost다(다른 호스트 산출물로 대신하지
//    않는다). status: system 도구(Apple metal)는 공유 라이브러리로 열지 않으므로 UnsupportedHost다
//    (P1 RunProcess로 실행한다).
// 2. 경로는 <저장소 루트>/<storeKey>/<파일 경로>다. 현재 디렉터리·PATH·실행 파일 옆은 보지 않는다.
// 3. role이 runtime인 모든 파일의 크기와 SHA-256을 표와 대조한다. 없으면 NotBootstrapped, 다르면
//    DigestMismatch다. 둘 다 bootstrap을 안내한다(ShaderToolLoadMessage).
// 4. 첫 runtime 파일을 DynamicLibrary::Open으로 연다(의존 라이브러리는 같은 디렉터리와 OS
// 디렉터리만
//    찾는다).
//
// 검사와 열기 사이에 저장소 파일을 바꾸는 경우는 막지 않는다. 저장소는 그 사용자만 쓰는
// 디렉터리이고, 거기에 쓸 수 있는 프로세스는 이미 그 사용자 권한으로 실행 중이다.
//
// 사용 예(워커):
//   char root[shader::MaxShaderToolPathBytes];
//   usize rootBytes = 0;
//   if (!shader::GetShaderToolStoreRoot(root, sizeof(root), rootBytes)) { ... }
//   shader::ShaderToolSet tools;
//   if (auto opened = tools.Open(root, rootBytes); !opened) {
//       fprintf(stderr, "%s: %s\n", opened.tool, shader::ShaderToolLoadMessage(opened.code));
//       return;                                     // job 실패
//   }
//   auto create = reinterpret_cast<DxcCreateInstanceProc>(
//       tools.Dxc().address[shader::DxcFunctions::CreateInstance]);
namespace rocks::shader {

// 셰이더 도구 열기의 로그 카테고리다("Shader" 아래, Docs/Design/LoggingSystem.ko.md 13절). 로더는
// 기록하지 않고 ShaderToolLoadStatus로 돌려준다. 호출자가 필수 도구 실패는 Error, 없어도 되는 선택
// 도구가 없으면 Debug로 남긴다.
inline constexpr LogCategory LogShaderTool{"Shader.Tool"};

// 저장소 경로·도구 경로의 최대 byte 수(NUL 포함)다.
inline constexpr usize MaxShaderToolPathBytes = 4096;

enum class ShaderToolLoadCode : uint8 {
    Success,
    // 저장소 루트가 null·빈 값·상대 경로이거나 표 항목이 잘못되었다(storeKey·파일 경로 없음).
    InvalidArgument,
    // 이 호스트의 파일이 표에 없거나 공유 라이브러리로 열지 않는 system 도구다. 이 플랫폼 구현이
    // 없는 기능(QueryDxcVersion 등)도 이 값이다.
    UnsupportedHost,
    // 저장소에 파일이 없다. bootstrap을 실행해야 한다.
    NotBootstrapped,
    // 저장소 파일의 크기나 SHA-256이 표와 다르다. 저장소 디렉터리를 지우고 bootstrap을 다시 한다.
    DigestMismatch,
    // 저장소 파일을 읽지 못했다(file에 OS 원인).
    ReadFailed,
    // 경로가 MaxShaderToolPathBytes를 넘는다.
    PathTooLong,
    // DynamicLibrary::Open이 실패했다(file에 원인, 예: 의존 라이브러리 없음 NotFound).
    LoadFailed,
    // 함수 표의 심볼이 없다(symbol에 이름). 표와 다른 버전의 라이브러리다.
    SymbolMissing,
    // 열리지 않은 라이브러리에서 함수를 찾았거나 이미 열린 객체를 다시 열었다.
    WrongState,
    // 도구 함수를 불렀지만 실패했다(QueryDxcVersion의 HRESULT 등, file.nativeError에 원인 값).
    CallFailed
};

// 열기 결과다. tool은 도구 이름(정적 수명, 표 항목의 name), symbol은 SymbolMissing일 때 없는 심볼
// 이름(정적 수명)이다. file은 ReadFailed·LoadFailed의 OS 원인이다.
struct [[nodiscard]] ShaderToolLoadStatus {
    ShaderToolLoadCode code = ShaderToolLoadCode::Success;
    platform::FileStatus file;
    const char* tool = nullptr;
    const char* symbol = nullptr;

    explicit operator bool() const noexcept {
        return code == ShaderToolLoadCode::Success;
    }
};

// 진단용 열거자 이름이다(예: "NotBootstrapped"). 정적 수명이다.
ROCKS_SHADERTOOLLOADER_API const char* ShaderToolLoadCodeName(ShaderToolLoadCode code) noexcept;

// 사용자에게 보일 영어 안내문이다(정적 수명). NotBootstrapped·DigestMismatch는 bootstrap 명령
// (ShaderToolBootstrapCommand)을 담는다.
ROCKS_SHADERTOOLLOADER_API const char* ShaderToolLoadMessage(ShaderToolLoadCode code) noexcept;

// 저장소를 채우는 명령이다(저장소 루트에서 실행).
inline constexpr const char ShaderToolBootstrapCommand[] =
    "dotnet msbuild Rocks.proj /t:ThirdPartyBootstrap";

// 기본 저장소 루트(UTF-8 절대 경로, 끝 구분자 없음)를 buffer에 쓰고 길이를 bytes에 둔다(NUL도
// 쓴다). bootstrap(Rocks.Tooling.Common ThirdPartyStore.DefaultRoot)과 같은 위치다: Windows
// %LOCALAPPDATA%\Rocks\ThirdParty, macOS ~/Library/Application Support/Rocks/ThirdParty. 디렉터리가
// 있는지는 보지 않는다. 실패: buffer 부족 Overflow(bytes에 필요한 길이, NUL 제외), 개발 호스트가
// 아닌 플랫폼(Android) Unsupported, 그 밖은 GetStandardDirectory의 오류다.
ROCKS_SHADERTOOLLOADER_API platform::FileStatus GetShaderToolStoreRoot(char* buffer, usize capacity,
                                                                       usize& bytes) noexcept;

// 도구 공유 라이브러리 하나다. 이동만 된다. 함수 주소의 수명은 DynamicLibrary와 같다(Find가 준
// lease가 살아 있는 동안 유효하다).
class ROCKS_SHADERTOOLLOADER_API ShaderToolLibrary final {
public:
    ShaderToolLibrary() noexcept = default;
    ShaderToolLibrary(ShaderToolLibrary&& other) noexcept;
    ShaderToolLibrary& operator=(ShaderToolLibrary&& other) noexcept;
    ShaderToolLibrary(const ShaderToolLibrary&) = delete;
    ShaderToolLibrary& operator=(const ShaderToolLibrary&) = delete;
    ~ShaderToolLibrary() noexcept = default;

    // tool(도구 표 항목, 보통 FindShaderTool 결과)을 storeRoot(UTF-8 절대 경로, 길이 지정, NUL 종료
    // 불필요) 아래에서 대조하고 연다. 순서와 오류는 파일 주석을 따른다. tool은 이 객체보다 오래
    // 살아야 한다(생성 표는 정적 수명). 실패하면 닫힌 상태로 남는다.
    ShaderToolLoadStatus Open(const ShaderTool& tool, const char* storeRoot,
                              usize storeRootBytes) noexcept;

    // symbols[0..count)를 찾는다(DynamicLibrary::Find와 같은 계약). 없으면 SymbolMissing과 그 이름,
    // 열리지 않았으면 WrongState다.
    ShaderToolLoadStatus Find(platform::DynamicSymbol* symbols, usize count,
                              platform::DynamicLibraryLease& lease) const noexcept;

    // Functions(아래 *Functions 표)의 모든 심볼을 찾아 address에 채운다. 실패하면 address는 모두
    // null이고 lease는 바뀌지 않는다.
    template <class Functions>
    ShaderToolLoadStatus FindFunctions(Functions& functions,
                                       platform::DynamicLibraryLease& lease) const noexcept {
        platform::DynamicSymbol symbols[Functions::Count];
        for (usize index = 0; index < Functions::Count; ++index) {
            symbols[index].name = Functions::Symbols[index];
        }

        const ShaderToolLoadStatus found = Find(symbols, Functions::Count, lease);
        for (usize index = 0; index < Functions::Count; ++index) {
            functions.address[index] = found ? symbols[index].address : nullptr;
        }

        return found;
    }

    bool IsOpen() const noexcept {
        return library_.IsOpen();
    }

    // 연 도구의 표 항목이다. 열리지 않았으면 nullptr다.
    const ShaderTool* Tool() const noexcept {
        return tool_;
    }

    // 라이브러리를 내린다(DynamicLibrary::Close). lease가 남아 있으면 Busy이고 열린 채로 둔다.
    platform::FileStatus Close() noexcept;

private:
    platform::DynamicLibrary library_;
    const ShaderTool* tool_ = nullptr;
};

// 아래 함수 표는 SH0(측정)이 쓰는 최소 집합이다. address[i]는 Symbols[i]의 주소이고, 호출자는 그
// 도구의 공개 헤더(Engine/Source/ThirdParty/<Lib>, 소비 모듈의 Private include) 타입으로 바꿔
// 부른다. 예: reinterpret_cast<decltype(&spvc_context_create)>(
//         table.address[SpirvCrossFunctions::ContextCreate]).

// DXC(dxcompiler): DxcCreateInstance(REFCLSID, REFIID, LPVOID*) -> HRESULT(dxcapi.h
// DxcCreateInstanceProc).
struct DxcFunctions {
    enum Index : usize {
        CreateInstance,
        Count
    };

    static constexpr const char* Symbols[Count] = {"DxcCreateInstance"};
    void* address[Count] = {};
};

// FXC(d3dcompiler_47): D3DCompile·D3DReflect(Windows SDK d3dcompiler.h의 pD3DCompile·D3DReflect).
struct FxcFunctions {
    enum Index : usize {
        Compile,
        Reflect,
        Count
    };

    static constexpr const char* Symbols[Count] = {"D3DCompile", "D3DReflect"};
    void* address[Count] = {};
};

// SPIRV-Cross C API(spirv_cross_c.h): 버전, 문맥, SPIR-V 해석, backend 컴파일러와 옵션, GLSL 결합
// 샘플러 만들기(HLSL의 분리 Texture·SamplerState를 ESSL sampler2D로 바꾼다)와 그 목록·변수 이름
// 읽기, 출력.
struct SpirvCrossFunctions {
    enum Index : usize {
        GetVersion,
        ContextCreate,
        ContextDestroy,
        ContextReleaseAllocations,
        ContextGetLastErrorString,
        ContextParseSpirv,
        ContextCreateCompiler,
        CompilerCreateCompilerOptions,
        CompilerOptionsSetBool,
        CompilerOptionsSetUint,
        CompilerInstallCompilerOptions,
        CompilerBuildCombinedImageSamplers,
        CompilerGetCombinedImageSamplers,
        CompilerGetName,
        CompilerCompile,
        CompilerCreateShaderResources,
        ResourcesGetResourceListForType,
        CompilerGetDecoration,
        CompilerGetMemberDecoration,
        HlslResourceBindingInit,
        CompilerHlslAddResourceBinding,
        CompilerHlslAddVertexAttributeRemap,
        MslResourceBindingInit2,
        CompilerMslAddResourceBinding2,
        CompilerSetDecoration,
        CompilerSetName,
        CompilerBuildDummySamplerForCombinedImages,
        Count
    };

    static constexpr const char* Symbols[Count] = {
        "spvc_get_version",
        "spvc_context_create",
        "spvc_context_destroy",
        "spvc_context_release_allocations",
        "spvc_context_get_last_error_string",
        "spvc_context_parse_spirv",
        "spvc_context_create_compiler",
        "spvc_compiler_create_compiler_options",
        "spvc_compiler_options_set_bool",
        "spvc_compiler_options_set_uint",
        "spvc_compiler_install_compiler_options",
        "spvc_compiler_build_combined_image_samplers",
        "spvc_compiler_get_combined_image_samplers",
        "spvc_compiler_get_name",
        "spvc_compiler_compile",
        "spvc_compiler_create_shader_resources",
        "spvc_resources_get_resource_list_for_type",
        "spvc_compiler_get_decoration",
        "spvc_compiler_get_member_decoration",
        "spvc_hlsl_resource_binding_init",
        "spvc_compiler_hlsl_add_resource_binding",
        "spvc_compiler_hlsl_add_vertex_attribute_remap",
        "spvc_msl_resource_binding_init_2",
        "spvc_compiler_msl_add_resource_binding_2",
        "spvc_compiler_set_decoration",
        "spvc_compiler_set_name",
        "spvc_compiler_build_dummy_sampler_for_combined_images"};
    void* address[Count] = {};
};

// SPIRV-Tools C API(spirv-tools/libspirv.h): 버전, 문맥, 검증(spirv-val), 최적화기, 결과 해제.
struct SpirvToolsFunctions {
    enum Index : usize {
        SoftwareVersionString,
        ContextCreate,
        ContextDestroy,
        ValidatorOptionsCreate,
        ValidatorOptionsDestroy,
        ValidateWithOptions,
        DiagnosticDestroy,
        OptimizerCreate,
        OptimizerDestroy,
        OptimizerRegisterPerformancePasses,
        OptimizerOptionsCreate,
        OptimizerOptionsDestroy,
        OptimizerRun,
        BinaryDestroy,
        Count
    };

    static constexpr const char* Symbols[Count] = {"spvSoftwareVersionString",
                                                   "spvContextCreate",
                                                   "spvContextDestroy",
                                                   "spvValidatorOptionsCreate",
                                                   "spvValidatorOptionsDestroy",
                                                   "spvValidateWithOptions",
                                                   "spvDiagnosticDestroy",
                                                   "spvOptimizerCreate",
                                                   "spvOptimizerDestroy",
                                                   "spvOptimizerRegisterPerformancePasses",
                                                   "spvOptimizerOptionsCreate",
                                                   "spvOptimizerOptionsDestroy",
                                                   "spvOptimizerRun",
                                                   "spvBinaryDestroy"};
    void* address[Count] = {};
};

// 워커가 쓰는 도구 묶음이다. DXC·SPIRV-Cross·SPIRV-Tools는 필수이고 FXC는 표에 이 호스트 파일이
// 있을 때만 연다(Windows 전용 도구). 열린 동안 모든 함수 표가 유효하다(lease를 쥔다). 복사·이동하지
// 않는다.
class ROCKS_SHADERTOOLLOADER_API ShaderToolSet final {
public:
    ShaderToolSet() noexcept = default;
    ShaderToolSet(const ShaderToolSet&) = delete;
    ShaderToolSet& operator=(const ShaderToolSet&) = delete;

    ~ShaderToolSet() noexcept {
        Close();
    }

    // tools(기본은 생성 도구 표)에서 네 도구를 찾아 storeRoot 아래에서 연다. 하나라도 실패하면 연
    // 것을 모두 닫고 그 결과를 돌려준다. 필수 도구가 표에 없거나 이 호스트 파일이 없으면
    // UnsupportedHost다. 이미 열려 있으면 WrongState다. tools는 이 객체가 열려 있는 동안 살아
    // 있어야 한다.
    ShaderToolLoadStatus Open(const char* storeRoot, usize storeRootBytes,
                              Span<const ShaderTool> tools = ShaderTools()) noexcept;

    bool IsOpen() const noexcept {
        return dxc_.IsOpen();
    }

    // FXC를 열었는지다(이 호스트 파일이 표에 있었다).
    bool HasFxc() const noexcept {
        return fxc_.IsOpen();
    }

    const DxcFunctions& Dxc() const noexcept {
        return dxcFunctions_;
    }

    const FxcFunctions& Fxc() const noexcept {
        return fxcFunctions_;
    }

    const SpirvCrossFunctions& SpirvCross() const noexcept {
        return spirvCrossFunctions_;
    }

    const SpirvToolsFunctions& SpirvTools() const noexcept {
        return spirvToolsFunctions_;
    }

    // 함수 표를 비우고 lease를 놓은 뒤 모든 라이브러리를 내린다. 열리지 않았으면 아무 일도 하지
    // 않는다.
    void Close() noexcept;

private:
    ShaderToolLibrary dxc_;
    ShaderToolLibrary fxc_;
    ShaderToolLibrary spirvCross_;
    ShaderToolLibrary spirvTools_;
    platform::DynamicLibraryLease dxcLease_;
    platform::DynamicLibraryLease fxcLease_;
    platform::DynamicLibraryLease spirvCrossLease_;
    platform::DynamicLibraryLease spirvToolsLease_;
    DxcFunctions dxcFunctions_;
    FxcFunctions fxcFunctions_;
    SpirvCrossFunctions spirvCrossFunctions_;
    SpirvToolsFunctions spirvToolsFunctions_;
};

// DXC 버전(IDxcVersionInfo::GetVersion)을 얻는다. 워커 도구 조회(11절)에 쓰고, 열기 시험이 함수
// 표를 실제로 부를 수 있음을 확인하는 데도 쓴다. Windows 구현만 있다(Private/Win64가 dxcapi.h를
// Windows 헤더와 함께 include한다). 다른 플랫폼은 UnsupportedHost, 함수 표가 비었으면 WrongState,
// DXC 호출 실패는 CallFailed(file.nativeError에 HRESULT)다.
ROCKS_SHADERTOOLLOADER_API ShaderToolLoadStatus QueryDxcVersion(const DxcFunctions& functions,
                                                                uint32& major,
                                                                uint32& minor) noexcept;

// DXC가 #include 파일을 찾을 때 부르는 원문 공급자다(IDxcIncludeHandler). name은 DXC가 요청한 파일
// 이름(UTF-8)이며 DXC 표기 그대로다: 주 파일 이름이 상대 경로면 "./"가 붙고, 포함한 파일 디렉터리
// 기준 후보는 '\'로 이어질 수 있다. 찾으면 true와 bytes(CompileWithDxc가 끝날 때까지 살아 있는
// UTF-8 원문)를 둔다. false면 DXC는 그 후보가 없다고 보고 다음 검색 경로를 본다.
struct DxcIncludeSource {
    bool (*load)(void* context, StringView name, Span<const uint8>& bytes) noexcept = nullptr;
    void* context = nullptr;
};

// DXC 컴파일 한 번의 입력이다. 모든 view는 호출 동안 살아 있어야 한다.
struct DxcCompileRequest {
    // UTF-8 원문이다.
    StringView source;
    // 주 파일 이름(진단 위치와 상대 include의 기준)이다. 비면 DXC 기본 이름이다.
    StringView sourceName;
    // DXC 명령 인자(UTF-8)다. 예: {"-E", "main", "-T", "cs_6_6", "-HV", "2021", "-DUSE_FOG=1"}.
    Span<const StringView> arguments;
    // null이면 #include를 찾지 못한다.
    const DxcIncludeSource* includes = nullptr;
};

// DXC 컴파일 결과다. compiled가 false면 컴파일 오류이고 diagnostics에 DXC 진단(영어, 원문 그대로)이
// 있다. 성공해도 경고가 diagnostics에 있을 수 있다.
struct DxcCompileResult {
    explicit DxcCompileResult(Allocator& allocator) noexcept
        : object(allocator), diagnostics(allocator) {
    }

    Array<uint8> object;
    String diagnostics;
    bool compiled = false;
};

// DXC(IDxcCompiler3::Compile)로 request를 컴파일한다. 측정 S7(Docs/Design/ShaderSystem 11.1절
// SC3 — Rocks 전처리 결과와 DXC 직접 전처리의 DXIL 대조)과 형식 모듈이 쓴다. 결과는 먼저 비운다.
// 반환: DXC가 돌았으면 Success(컴파일 오류는 result.compiled로 본다), 함수 표가 비었으면
// WrongState, 인자가 UTF-8이 아니면 InvalidArgument, COM 호출 실패는 CallFailed(file.nativeError에
// HRESULT), 메모리 부족은 CallFailed, Windows 밖은 UnsupportedHost다.
//
// 사용 예:
//   const StringView arguments[] = {"-E", "main", "-T", "ps_6_6"};
//   DxcCompileRequest request;
//   request.source = text;
//   request.sourceName = "/Engine/Private/Sample.rsf";
//   request.arguments = arguments;
//   DxcCompileResult result(allocator);
//   if (auto ran = CompileWithDxc(tools.Dxc(), request, result); ran && result.compiled) { ... }
ROCKS_SHADERTOOLLOADER_API ShaderToolLoadStatus CompileWithDxc(const DxcFunctions& functions,
                                                               const DxcCompileRequest& request,
                                                               DxcCompileResult& result) noexcept;

} // namespace rocks::shader
