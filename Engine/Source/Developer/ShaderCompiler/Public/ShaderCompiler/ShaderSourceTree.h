#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Platform/File.h>
#include <ShaderCore/ShaderVirtualPath.h>

// 셰이더 소스 트리와 가상 경로다(Docs/Design/ShaderSystem 3절, 11.1절 SC2).
//
// 셰이더 원문은 가상 경로로만 가리킨다. 가상 루트는 세 종류다.
// - /Engine: 엔진 Engine/Shaders(그 아래 Public·Private·Shared).
// - /Plugin/<이름>: 플러그인의 셰이더 디렉터리. 시험은 임시 fixture 디렉터리를 이렇게 mount한다.
// - /Project: 프로젝트의 셰이더 디렉터리.
// mount 규칙은 UE AddShaderSourceDirectoryMapping과 같다: 가상 디렉터리는 '/'로 시작하고 '/'로
// 끝나지 않으며 '.'이 없고, 같은 가상 디렉터리를 두 번 mount하지 않는다. 실제 디렉터리는 절대
// 경로로 있어야 하고, 그 아래 Generated 디렉터리가 있으면 거부한다(<루트>/Generated/…는 메모리
// 원문 — 생성 HLSL, SH3 — 전용 자리다).
//
// 가상 경로 규칙(UE CheckVirtualShaderFilePath 대응, ShaderCore CheckShaderVirtualPath):
// - mount된 루트 아래 파일이다: /Engine/…, /Project/…, /Plugin/<이름>/….
// - 성분은 비지 않은 ASCII [A-Za-z0-9_.-]이고 "."·".."이 아니다('\\'·빈 성분 거부).
// - 확장자: <루트>/Shared/ 아래는 .h(C++·HLSL 공용 헤더)만, 그 밖은 .rsf(엔트리)·.rsh(include).
// - 전체 길이는 MaxShaderVirtualPathBytes 미만이다.
//
// 사용 예(메인 프로세스):
//   ShaderSourceTree tree(allocator);
//   if (auto mounted = tree.Mount("/Engine", "C:/Rocks/Engine/Shaders"); !mounted) {
//       // mounted.code, mounted.message
//   }
//   Array<uint8> bytes(allocator);
//   if (auto loaded = tree.Load("/Engine/Public/Platform.rsh", bytes); !loaded) { ... }
//
// 스레드: Mount는 한 스레드에서 끝낸 뒤 Load를 여러 스레드가 동시에 불러도 된다(Load는 트리를
// 바꾸지 않는다). 파일 시스템 객체는 동시 호출을 받아야 한다(PhysicalFileSystem은 받는다).
namespace rocks::shader {

// 원문 파일 하나의 최대 byte 수(16 MiB)다. 넘으면 읽기 전에 거부한다.
inline constexpr usize MaxShaderSourceFileBytes = usize{16} * 1024 * 1024;
// 오류 세부 문구의 최대 byte 수(NUL 포함)다. 넘는 부분은 잘린다.
inline constexpr usize MaxShaderSourceMessageBytes = 256;

// 가상 경로 문법·상수(MaxShaderVirtualPathBytes·가상 루트 이름)는
// ShaderCore(ShaderCore/ShaderVirtualPath.h)에 있다(셰이더 타입 경로 검사와 같은 규칙). 저장소 루트
// 기준 엔진 셰이더 디렉터리(/Engine의 실제 디렉터리)다.
inline constexpr char EngineShaderSourceDirectory[] = "Engine/Shaders";

// 소스 트리·전처리기·정의 조립이 함께 쓰는 오류 코드다. 위치는 ShaderSourceStatus의 path·line에
// 있다.
enum class ShaderSourceError : uint8 {
    None,
    // 인자가 잘못되었다(빈 값, 엔트리 아닌 파일을 엔트리로 줌 등).
    InvalidArgument,
    OutOfMemory,
    // 가상 디렉터리가 mount 규칙에 맞지 않는다(/Engine·/Project·/Plugin/<이름> 밖, '.', 끝 '/').
    InvalidMount,
    // 실제 디렉터리가 절대 경로가 아니거나 없다.
    MountDirectoryNotFound,
    // 실제 디렉터리 아래에 Generated 디렉터리가 있다.
    GeneratedDirectoryExists,
    // 같은 가상 디렉터리를 두 번 mount했다.
    DuplicateMount,
    // 가상 경로가 규칙(성분·문자·길이)에 맞지 않는다.
    InvalidPath,
    // 확장자가 규칙에 맞지 않는다(Shared 밖 .h, Shared 안 .rsh 등).
    InvalidExtension,
    // 가상 경로의 루트가 mount되지 않았다.
    UnknownMount,
    // <루트>/Generated/… 는 메모리 원문 전용인데 아직 그 원문이 없다(SH3).
    GeneratedSource,
    FileNotFound,
    // 파일을 열거나 읽지 못했다.
    ReadFailed,
    // 파일이 원문 크기 한도를 넘는다.
    FileTooLarge,
    // 원문이 UTF-8이 아니거나 NUL byte가 있다.
    InvalidEncoding,
    // trigraph(??=, ??/ 등)를 만났다.
    Trigraph,
    UnterminatedComment,
    // 문자열·문자 리터럴이 줄 안에서 닫히지 않는다.
    UnterminatedLiteral,
    // 모르는·받지 않는 지시문이거나 지시문 형식이 틀렸다(#line 입력, #warning 등).
    InvalidDirective,
    // #include <…>, #include_next, 절대 OS 경로, 매크로로 만든 include.
    UnsupportedInclude,
    IncludeDepthExceeded,
    // #define 형식이 틀렸다(인자 중복, '#' 뒤 인자 아님, '##'가 양 끝, 내장 이름 재정의 등).
    InvalidMacroDefinition,
    // 다른 내용으로 같은 매크로를 다시 정의했다(#undef 없이).
    MacroRedefined,
    // 함수형 매크로 호출의 인자 수가 틀리거나 ')'가 없다.
    InvalidMacroCall,
    MacroDepthExceeded,
    // '##'가 토큰 하나를 만들지 못했다.
    InvalidPaste,
    // #else·#elif·#endif의 짝이 맞지 않거나 파일 끝까지 #endif가 없다.
    UnbalancedConditional,
    // #if 식의 문법이 틀렸거나 0으로 나눴다.
    InvalidExpression,
    // #if·#ifdef·#ifndef·#elif·defined에 컴파일러 내장 이름(__로 시작, __LINE__·__FILE__ 제외)이
    // 나왔다(SC3). SC10의 ROCKS_* 정의를 쓴다.
    CompilerBuiltinInCondition,
    // __DATE__·__TIME__·__TIMESTAMP__(결정성).
    NondeterministicMacro,
    // #error 지시문. message에 문구가 있다.
    ErrorDirective,
    // 출력이 출력 한도를 넘는다(전개 중간 결과 포함).
    OutputTooLarge,
    // 전처리 정의의 이름·값이 틀렸다(식별자 아님, 줄바꿈, 닫히지 않은 리터럴).
    InvalidDefine,
    // job 정의 이름이 ROCKS_ 또는 __로 시작한다(SC10).
    ReservedDefine,
    // 같은 정의 이름이 두 번 있다.
    DuplicateDefine,
    // 단계가 비트 하나가 아니거나 형식이 그 단계를 받지 않거나 자리만 둔 형식이다.
    UnsupportedStage
};

// 진단용 열거자 이름이다(정적 수명). 범위 밖이면 "unknown".
ROCKS_SHADERCOMPILER_API const char* ShaderSourceErrorName(ShaderSourceError code) noexcept;

// 결과다. path는 오류가 난 가상 경로(mount 오류는 가상 디렉터리, 모르면 빈 값), line은 1부터의
// 원문 줄(모르면 0), message는 영어 세부(매크로 이름, #error 문구 등)다. 모두 고정 버퍼라 할당
// 없이 돌려준다.
struct [[nodiscard]] ShaderSourceStatus {
    ShaderSourceError code = ShaderSourceError::None;
    uint32 line = 0;
    char path[MaxShaderVirtualPathBytes] = {};
    char message[MaxShaderSourceMessageBytes] = {};

    explicit operator bool() const noexcept {
        return code == ShaderSourceError::None;
    }
};

// 가상 파일 경로를 검사한다(위 규칙). mount 여부는 보지 않는다. 오류는 InvalidPath·
// InvalidExtension이다.
ROCKS_SHADERCOMPILER_API ShaderSourceStatus ValidateShaderVirtualPath(StringView path) noexcept;

// 가상 디렉터리(mount 이름)를 검사한다: "/Engine", "/Project", "/Plugin/<이름>"(이름은 비지 않은
// ASCII [A-Za-z0-9_-], MaxShaderPluginNameBytes 이하). 오류는 InvalidMount다.
ROCKS_SHADERCOMPILER_API ShaderSourceStatus
ValidateShaderVirtualDirectory(StringView directory) noexcept;

// 가상 루트 → 실제 디렉터리 mount 표다. 이동만 된다.
class ROCKS_SHADERCOMPILER_API ShaderSourceTree final {
public:
    // allocator와 files는 이 객체보다 오래 살아야 한다.
    explicit ShaderSourceTree(Allocator& allocator, platform::FileSystem& files =
                                                        platform::PhysicalFileSystem()) noexcept;

    ShaderSourceTree(ShaderSourceTree&& other) noexcept = default;
    ShaderSourceTree& operator=(ShaderSourceTree&& other) noexcept = default;
    ShaderSourceTree(const ShaderSourceTree&) = delete;
    ShaderSourceTree& operator=(const ShaderSourceTree&) = delete;
    ~ShaderSourceTree() noexcept = default;

    // virtualDirectory(ValidateShaderVirtualDirectory 규칙)를 realDirectory(UTF-8 절대 경로, 끝
    // 구분자 허용)에 mount한다. 오류: InvalidMount, DuplicateMount, MountDirectoryNotFound(상대
    // 경로·없음), GeneratedDirectoryExists, ReadFailed(디렉터리 확인 실패), OutOfMemory. 실패하면
    // 표는 바뀌지 않는다.
    ShaderSourceStatus Mount(StringView virtualDirectory, StringView realDirectory) noexcept;

    // virtualPath의 파일 byte를 bytes에 읽는다(내용을 바꾸지 않는다). 오류: InvalidPath·
    // InvalidExtension, UnknownMount, GeneratedSource, FileNotFound, FileTooLarge(maxBytes 초과,
    // 읽기 전에 판정), ReadFailed, OutOfMemory. 실패하면 bytes는 비어 있다.
    ShaderSourceStatus Load(StringView virtualPath, Array<uint8>& bytes,
                            usize maxBytes = MaxShaderSourceFileBytes) const noexcept;

    usize MountCount() const noexcept {
        return mounts_.Size();
    }

    // i번째 mount의 가상 디렉터리다(Mount 순서).
    StringView MountedDirectory(usize index) const noexcept {
        return mounts_[index].virtualDirectory.View();
    }

    Allocator& GetAllocator() const noexcept {
        return *allocator_;
    }

private:
    struct MountEntry {
        explicit MountEntry(Allocator& allocator) noexcept
            : virtualDirectory(allocator), realDirectory(allocator) {
        }

        String virtualDirectory;
        String realDirectory;
    };

    const MountEntry* FindMount(StringView virtualPath) const noexcept;

    Allocator* allocator_;
    platform::FileSystem* files_;
    Array<MountEntry> mounts_;
};

} // namespace rocks::shader
