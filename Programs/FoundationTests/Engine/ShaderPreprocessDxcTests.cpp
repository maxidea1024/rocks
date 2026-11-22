#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Path.h>
#include <ShaderCompiler/ShaderFormatDefinition.h>
#include <ShaderCompiler/ShaderPreprocessDefines.h>
#include <ShaderCompiler/ShaderPreprocessor.h>
#include <ShaderCompiler/ShaderSourceTree.h>
#include <ShaderToolLoader/ShaderToolLoader.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;
namespace shader_preprocess_dxc_tests_file {

using namespace rocks::shader;

// 측정 S7(Docs/Design/ShaderSystem.ko.md 5.4절, 11.1절 SC3)이다. 같은 fixture 원문을
// (가) Rocks 전처리 결과(PreprocessShader)로 DXC 컴파일하고,
// (나) DXC가 원문을 직접 전처리해 컴파일한다(include handler가 같은 소스 트리의 가상 경로를 주고,
//      SC10 정의를 -D로 같이 준다. 강제 include는 주 파일에서 Platform.rsh와 엔트리를 include해
//      같게 만든다).
// 두 산출물(DXIL·SPIR-V) byte가 같은지, 일부러 넣은 오류의 진단 위치(가상 경로·줄)가 같은지 본다.
// 컴파일러 내장 이름을 #if에 쓴 fixture는 Rocks 쪽 오류를 확인한다. 도구 저장소가 없거나(bootstrap
// 전, CI) DXC 호출 구현이 없는 호스트는 SKIP이며 실패가 아니다.
namespace {

constexpr usize ScratchPathBytes = 1024;
constexpr char PluginRoot[] = "/Plugin/S7";
constexpr char Entry[] = "/Plugin/S7/Main.rsf";
constexpr char ErrorEntry[] = "/Plugin/S7/ErrorMain.rsf";
constexpr char BuiltinEntry[] = "/Plugin/S7/BuiltinMain.rsf";
// (나)의 주 파일 이름이다(강제 include를 원문으로 적는다).
constexpr char WrapperName[] = "/Plugin/S7/S7Wrapper.rsf";
// 일부러 넣은 오류의 위치다.
constexpr char BrokenPath[] = "/Plugin/S7/Lib/Broken.rsh";
constexpr uint32 BrokenLine = 4;

// 대조 fixture다. include(상대·절대·#pragma once·include guard), 매크로(##·가변 인자·여러 줄
// 호출), SC10 조건(ROCKS_*·defined·true), ROCKS_IMAGE_FORMAT, __LINE__, 여러 줄 주석, job 정의
// (USE_EXTRA)를 모두 쓴다.
constexpr char MainSource[] =
    "// S7 fixture: Rocks preprocessing and DXC preprocessing must produce the same code.\n"
    "#include \"Lib/Common.rsh\"\n"
    "#include \"/Plugin/S7/Lib/Common.rsh\"\n"
    "#include \"Lib/Guarded.rsh\"\n"
    "#include \"./Lib/Guarded.rsh\"\n"
    "\n"
    "#define CAT(a, b) a ## b\n"
    "#define SCALE(x, ...) ((x) * Scale(__VA_ARGS__))\n"
    "\n"
    "ROCKS_IMAGE_FORMAT(rgba8) RWTexture2D<float4> Output : register(u0);\n"
    "RWStructuredBuffer<uint> Counters : register(u1);\n"
    "\n"
    "#if ROCKS_SHADER_MODEL_MAJOR >= 6 && defined(ROCKS_COMPUTE_SHADER) && true\n"
    "#define GROUP_SIZE 8\n"
    "#elif ROCKS_COMPILER_FXC\n"
    "#define GROUP_SIZE 4\n"
    "#else\n"
    "#define GROUP_SIZE 2\n"
    "#endif\n"
    "\n"
    "float Scale(float a, float b) { return a * b + /* multi\n"
    "   line comment */ COMMON_BIAS; }\n"
    "\n"
    "[numthreads(GROUP_SIZE, GROUP_SIZE, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    float value = SCALE(float(id.x), 2.0f, \\\n"
    "                        3.0f);\n"
    "    uint CAT(line, Number) = __LINE__;\n"
    "    Output[id.xy] = float4(value, CommonValue(id.y), float(lineNumber), GUARDED_VALUE);\n"
    "#if USE_EXTRA\n"
    "    InterlockedAdd(Counters[0], USE_EXTRA);\n"
    "#endif\n"
    "}\n";

constexpr char CommonSource[] = "#pragma once\n"
                                "#define COMMON_BIAS 0.25f\n"
                                "float CommonValue(uint v) { return float(v & 0xFFu) * 0.5f; }\n";

constexpr char GuardedSource[] = "#ifndef GUARDED_RSH\n"
                                 "#define GUARDED_RSH\n"
                                 "#define GUARDED_VALUE 1.0f\n"
                                 "#endif\n";

constexpr char ErrorMainSource[] = "#include \"Lib/Common.rsh\"\n"
                                   "#include \"Lib/Broken.rsh\"\n"
                                   "[numthreads(1, 1, 1)]\n"
                                   "void main() {}\n";

// BrokenLine(4)에 정의되지 않은 이름이 있다.
constexpr char BrokenSource[] = "// S7 error fixture\n"
                                "float BrokenValue()\n"
                                "{\n"
                                "    return undefined_symbol_s7 * 2.0f;\n"
                                "}\n";

constexpr char BuiltinMainSource[] = "#if __SHADER_TARGET_MAJOR >= 6\n"
                                     "#endif\n"
                                     "[numthreads(1, 1, 1)]\n"
                                     "void main() {}\n";

SystemAllocator& DxcTestAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

uint32 NextScratchSerial() noexcept {
    static uint32 serial = 0;
    return platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&serial, 1u) + 1u;
}

// 임시 fixture 디렉터리다. 소멸 때 지운다.
class Scratch final {
public:
    Scratch() noexcept = default;
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    ~Scratch() noexcept {
        if (length_ != 0) {
            (void)platform::DeleteDirectoryTree(platform::PhysicalFileSystem(), path_, length_);
        }
    }

    bool Make() noexcept {
        char base[ScratchPathBytes];
        usize bytes = 0;
        if (!platform::GetStandardDirectory(platform::StandardDirectory::Temporary, base,
                                            sizeof(base), bytes)) {
            return false;
        }

        const int count = snprintf(
            path_, sizeof(path_), "%s/rocks-shader-s7-%llu-%u", base,
            static_cast<unsigned long long>(platform::MonotonicNanoseconds()), NextScratchSerial());
        if (count <= 0 || static_cast<usize>(count) >= sizeof(path_)) {
            return false;
        }

        length_ = static_cast<usize>(count);
        return static_cast<bool>(
            platform::MakeDirectoryTree(platform::PhysicalFileSystem(), path_, length_));
    }

    const char* Path() const noexcept {
        return path_;
    }

    bool Write(const char* relative, const char* text) const noexcept {
        char path[ScratchPathBytes];
        const int count = snprintf(path, sizeof(path), "%s/%s", path_, relative);
        if (count <= 0 || static_cast<usize>(count) >= sizeof(path)) {
            return false;
        }

        char parent[ScratchPathBytes];
        memcpy(parent, path, static_cast<usize>(count) + 1);
        *strrchr(parent, '/') = '\0';
        if (!platform::MakeDirectoryTree(platform::PhysicalFileSystem(), parent, strlen(parent))) {
            return false;
        }

        platform::File file;
        if (!file.Open(path, platform::FileOpenMode::CreateAlways)) {
            return false;
        }

        const usize size = strlen(text);
        const platform::FileTransfer written = file.WriteAll(text, size);
        return static_cast<bool>(file.Close()) && static_cast<bool>(written.status) &&
               written.bytes == size;
    }

private:
    char path_[ScratchPathBytes] = {};
    usize length_ = 0;
};

// DXC가 요청한 이름을 가상 경로로 바꾼다: '\' → '/', 앞 "./" 제거, "/./" 접기, 앞 '/' 보장.
bool NormalizeDxcName(StringView name, char (&out)[MaxShaderVirtualPathBytes],
                      usize& bytes) noexcept {
    bytes = 0;
    usize cursor = 0;
    while (cursor + 1 < name.ByteCount() && name.Data()[cursor] == '.' &&
           (name.Data()[cursor + 1] == '/' || name.Data()[cursor + 1] == '\\')) {
        cursor += 2;
    }

    if (cursor >= name.ByteCount() || (name.Data()[cursor] != '/' && name.Data()[cursor] != '\\')) {
        out[bytes++] = '/';
    }

    for (; cursor < name.ByteCount(); ++cursor) {
        if (bytes + 1 >= MaxShaderVirtualPathBytes) {
            return false;
        }

        const char value = name.Data()[cursor] == '\\' ? '/' : name.Data()[cursor];
        // "/./"는 "/"로 접는다.
        if (value == '.' && bytes > 0 && out[bytes - 1] == '/' && cursor + 1 < name.ByteCount() &&
            (name.Data()[cursor + 1] == '/' || name.Data()[cursor + 1] == '\\')) {
            ++cursor;
            continue;
        }

        if (value == '/' && bytes > 0 && out[bytes - 1] == '/') {
            continue;
        }

        out[bytes++] = value;
    }

    return true;
}

// (나)의 include handler: 같은 소스 트리에서 가상 경로로 읽는다.
struct IncludeContext {
    explicit IncludeContext(const ShaderSourceTree& sourceTree) noexcept
        : tree(&sourceTree), loaded(DxcTestAllocator()) {
    }

    const ShaderSourceTree* tree;
    Array<Array<uint8>> loaded;
};

bool LoadInclude(void* context, StringView name, Span<const uint8>& bytes) noexcept {
    auto& include = *static_cast<IncludeContext*>(context);
    char path[MaxShaderVirtualPathBytes];
    usize length = 0;
    if (!NormalizeDxcName(name, path, length)) {
        return false;
    }

    auto added = include.loaded.TryEmplaceBack(DxcTestAllocator());
    if (!added) {
        return false;
    }

    if (!include.tree->Load(StringView(path, length), *added.Value())) {
        return false;
    }

    bytes = added.Value()->View();
    return true;
}

// DXC 진단의 첫 오류 위치("<파일>:<줄>:<열>: error")다. 파일은 NormalizeDxcName으로 맞춘다.
bool FirstErrorLocation(StringView diagnostics, char (&file)[MaxShaderVirtualPathBytes],
                        usize& fileBytes, uint32& line) noexcept {
    const usize marker = diagnostics.Find(StringView(": error"), StringComparison::CaseSensitive);
    if (marker == StringView::NotFound) {
        return false;
    }

    usize start = marker;
    while (start > 0 && diagnostics.Data()[start - 1] != '\n') {
        --start;
    }

    // marker 앞은 "<파일>:<줄>:<열>"이다. 뒤에서 ':' 두 개를 찾는다.
    usize column = marker;
    while (column > start && diagnostics.Data()[column - 1] != ':') {
        --column;
    }

    usize lineStart = column - 1;
    while (lineStart > start && diagnostics.Data()[lineStart - 1] != ':') {
        --lineStart;
    }

    if (column <= start || lineStart <= start) {
        return false;
    }

    line = 0;
    for (usize index = lineStart; index < column - 1; ++index) {
        line = line * 10 + static_cast<uint32>(diagnostics.Data()[index] - '0');
    }

    return NormalizeDxcName(StringView(diagnostics.Data() + start, lineStart - 1 - start), file,
                            fileBytes);
}

// DXC를 저장소에서 연다. 저장소가 없으면 false(SKIP)다.
struct DxcTool {
    ShaderToolLibrary library;
    platform::DynamicLibraryLease lease;
    DxcFunctions functions;
};

bool OpenDxc(DxcTool& tool, const char* test) noexcept {
    char root[MaxShaderToolPathBytes];
    usize rootBytes = 0;
    const ShaderTool* dxc = FindShaderTool(DxcToolName);
    if (dxc == nullptr || !GetShaderToolStoreRoot(root, sizeof(root), rootBytes)) {
        printf("[SKIP] %s: no tool store on this platform\n", test);
        return false;
    }

    const ShaderToolLoadStatus opened = tool.library.Open(*dxc, root, rootBytes);
    if (opened.code == ShaderToolLoadCode::NotBootstrapped ||
        opened.code == ShaderToolLoadCode::UnsupportedHost) {
        printf("[SKIP] %s: %s %s (%s)\n", test, DxcToolName, ShaderToolLoadCodeName(opened.code),
               ShaderToolLoadMessage(opened.code));
        return false;
    }

    if (!opened || !tool.library.FindFunctions(tool.functions, tool.lease)) {
        fprintf(stderr, "%s: DXC open failed: %s\n", test, ShaderToolLoadCodeName(opened.code));
        return false;
    }

    return true;
}

// 한 형식의 대조 준비: 소스 트리(저장소 Engine/Shaders + fixture), 형식 정의, 정의 조립.
struct S7Case {
    S7Case() noexcept
        : tree(DxcTestAllocator()), defines(DxcTestAllocator()), shader(DxcTestAllocator()) {
    }

    bool Make(const char* formatId) noexcept {
        char working[ScratchPathBytes];
        char engine[ScratchPathBytes];
        usize bytes = 0;
        if (!scratch.Make() || !scratch.Write("Main.rsf", MainSource) ||
            !scratch.Write("Lib/Common.rsh", CommonSource) ||
            !scratch.Write("Lib/Guarded.rsh", GuardedSource) ||
            !scratch.Write("ErrorMain.rsf", ErrorMainSource) ||
            !scratch.Write("Lib/Broken.rsh", BrokenSource) ||
            !scratch.Write("BuiltinMain.rsf", BuiltinMainSource) ||
            !platform::GetStandardDirectory(platform::StandardDirectory::Working, working,
                                            sizeof(working), bytes)) {
            return false;
        }

        snprintf(engine, sizeof(engine), "%s/%s", working, EngineShaderSourceDirectory);
        if (!tree.Mount("/Engine", engine) || !tree.Mount(PluginRoot, scratch.Path())) {
            return false;
        }

        const ShaderFormatStatus loaded =
            LoadShaderFormatDefinition(ShaderFormatDirectory, formatId, format);
        const ShaderDefine job[] = {{"USE_EXTRA", "3"}};
        return static_cast<bool>(loaded) &&
               static_cast<bool>(BuildShaderPreprocessDefines(format, ShaderStageCompute,
                                                              format.hlsl2021, job, defines));
    }

    Scratch scratch;
    ShaderSourceTree tree;
    ShaderFormatDefinition format;
    ShaderDefineSet defines;
    PreprocessedShader shader;
};

// 두 방식으로 컴파일한 결과다.
struct S7Outputs {
    S7Outputs() noexcept : rocks(DxcTestAllocator()), direct(DxcTestAllocator()) {
    }

    DxcCompileResult rocks;
    DxcCompileResult direct;
};

// entry를 (가)·(나)로 컴파일한다. options는 형식별 DXC 인자(엔트리·프로필·언어 버전 등)다.
bool CompileBothWays(const DxcTool& tool, S7Case& test, const char* entry,
                     Span<const StringView> options, S7Outputs& outputs) noexcept {
    // (가) Rocks 전처리 결과.
    const ShaderSourceStatus preprocessed =
        PreprocessShader(test.tree, entry, test.defines.Defines(), test.shader);
    if (!preprocessed) {
        fprintf(stderr, "S7 preprocess failed: %s at %s:%u: %s\n",
                ShaderSourceErrorName(preprocessed.code), preprocessed.path, preprocessed.line,
                preprocessed.message);
        return false;
    }

    DxcCompileRequest rocks;
    rocks.source = test.shader.text.View();
    rocks.sourceName = entry;
    rocks.arguments = options;
    if (!CompileWithDxc(tool.functions, rocks, outputs.rocks)) {
        return false;
    }

    // (나) DXC 직접 전처리: 같은 정의를 -D로, 강제 include를 원문으로. DXC(clang)는 "/Engine/…"를
    // Windows에서 절대 경로로 보지 않아 포함한 파일 디렉터리 기준 후보만 찾으므로, 가상 루트를 검색
    // 경로("-I .")로 준다. 포함한 파일 기준 후보("<디렉터리>/Engine/…")는 트리에 없어 handler가
    // 거절한다.
    Array<String> defineText(DxcTestAllocator());
    Array<StringView> arguments(DxcTestAllocator());
    if (!arguments.TryAppend(options) || !arguments.TryPushBack("-I") ||
        !arguments.TryPushBack(".")) {
        return false;
    }

    for (const ShaderDefine& define : test.defines.Defines()) {
        auto added = defineText.TryEmplaceBack(DxcTestAllocator());
        if (!added || !added.Value()->TryAppend("-D") || !added.Value()->TryAppend(define.name) ||
            !added.Value()->TryAppend("=") || !added.Value()->TryAppend(define.value)) {
            return false;
        }
    }

    for (const String& text : defineText) {
        if (!arguments.TryPushBack(text.View())) {
            return false;
        }
    }

    char wrapper[2 * MaxShaderVirtualPathBytes];
    snprintf(wrapper, sizeof(wrapper), "#include \"%s\"\n#include \"%s\"\n",
             ShaderPlatformHeaderPath, entry);
    IncludeContext context(test.tree);
    DxcIncludeSource includes;
    includes.load = &LoadInclude;
    includes.context = &context;
    DxcCompileRequest direct;
    direct.source = wrapper;
    direct.sourceName = WrapperName;
    direct.arguments = arguments.View();
    direct.includes = &includes;
    return static_cast<bool>(CompileWithDxc(tool.functions, direct, outputs.direct));
}

// 같은 byte인지 보고 다르면 첫 차이를 찍는다.
bool SameObject(const char* what, const S7Outputs& outputs) noexcept {
    const Array<uint8>& a = outputs.rocks.object;
    const Array<uint8>& b = outputs.direct.object;
    if (a.Size() == b.Size() && (a.IsEmpty() || memcmp(a.Data(), b.Data(), a.Size()) == 0)) {
        printf("[S7] %s: %zu bytes identical (Rocks preprocessing vs DXC preprocessing)\n", what,
               static_cast<size_t>(a.Size()));
        return true;
    }

    usize first = 0;
    while (first < a.Size() && first < b.Size() && a[first] == b[first]) {
        ++first;
    }

    fprintf(stderr, "[S7] %s: objects differ (%zu vs %zu bytes, first difference at %zu)\n", what,
            static_cast<size_t>(a.Size()), static_cast<size_t>(b.Size()),
            static_cast<size_t>(first));
    return false;
}

void PrintDiagnostics(const char* what, const DxcCompileResult& result) noexcept {
    if (!result.diagnostics.IsEmpty()) {
        fprintf(stderr, "[S7] %s diagnostics:\n%.*s\n", what,
                static_cast<int>(result.diagnostics.ByteCount()), result.diagnostics.Data());
    }
}

// 한 형식에서 대조 셋(같은 산출물, 같은 오류 위치, 내장 이름 거부)을 한다.
void RunS7(const char* test, const char* formatId, Span<const StringView> options) noexcept {
    DxcTool tool;
    if (!OpenDxc(tool, test)) {
        return;
    }

    S7Case s7;
    REQUIRE(s7.Make(formatId));

    S7Outputs outputs;
    const bool compiled = CompileBothWays(tool, s7, Entry, options, outputs);
    if (!compiled && outputs.rocks.diagnostics.IsEmpty()) {
        // DXC 호출 구현이 없는 호스트(macOS 등)다.
        uint32 major = 0;
        uint32 minor = 0;
        if (QueryDxcVersion(tool.functions, major, minor).code ==
            ShaderToolLoadCode::UnsupportedHost) {
            printf("[SKIP] %s: DXC compile is implemented only on Windows\n", test);
            return;
        }
    }

    REQUIRE(compiled);
    if (!outputs.rocks.compiled || !outputs.direct.compiled) {
        PrintDiagnostics("Rocks preprocessed", outputs.rocks);
        PrintDiagnostics("DXC preprocessed", outputs.direct);
    }

    REQUIRE(outputs.rocks.compiled && outputs.direct.compiled);
    CHECK(SameObject(formatId, outputs));

    // 음성 대조: job 정의 값 하나만 바꾼 전처리 결과는 다른 산출물이다(대조가 전처리 차이를 본다).
    ShaderDefineSet changed(DxcTestAllocator());
    const ShaderDefine job[] = {{"USE_EXTRA", "4"}};
    REQUIRE(BuildShaderPreprocessDefines(s7.format, ShaderStageCompute, s7.format.hlsl2021, job,
                                         changed));
    PreprocessedShader changedShader(DxcTestAllocator());
    REQUIRE(PreprocessShader(s7.tree, Entry, changed.Defines(), changedShader));
    DxcCompileRequest request;
    request.source = changedShader.text.View();
    request.sourceName = Entry;
    request.arguments = options;
    DxcCompileResult changedResult(DxcTestAllocator());
    REQUIRE(CompileWithDxc(tool.functions, request, changedResult) && changedResult.compiled);
    CHECK(changedResult.object.Size() != outputs.rocks.object.Size() ||
          memcmp(changedResult.object.Data(), outputs.rocks.object.Data(),
                 changedResult.object.Size()) != 0);

    // 일부러 넣은 오류: 두 방식 모두 실패하고 첫 오류 위치(가상 경로·줄)가 같다.
    S7Outputs errors;
    REQUIRE(CompileBothWays(tool, s7, ErrorEntry, options, errors));
    CHECK(!errors.rocks.compiled && !errors.direct.compiled);
    char rocksFile[MaxShaderVirtualPathBytes];
    char directFile[MaxShaderVirtualPathBytes];
    usize rocksBytes = 0;
    usize directBytes = 0;
    uint32 rocksLine = 0;
    uint32 directLine = 0;
    const bool rocksFound =
        FirstErrorLocation(errors.rocks.diagnostics.View(), rocksFile, rocksBytes, rocksLine);
    const bool directFound =
        FirstErrorLocation(errors.direct.diagnostics.View(), directFile, directBytes, directLine);
    if (!rocksFound || !directFound) {
        PrintDiagnostics("Rocks preprocessed error", errors.rocks);
        PrintDiagnostics("DXC preprocessed error", errors.direct);
    }

    REQUIRE(rocksFound && directFound);
    const StringView rocksLocation(rocksFile, rocksBytes);
    const StringView directLocation(directFile, directBytes);
    printf("[S7] %s error location: Rocks %.*s:%u, DXC %.*s:%u\n", formatId,
           static_cast<int>(rocksBytes), rocksFile, rocksLine, static_cast<int>(directBytes),
           directFile, directLine);
    CHECK(rocksLocation == directLocation && rocksLine == directLine);
    CHECK(rocksLocation == StringView(BrokenPath) && rocksLine == BrokenLine);

    // 내장 이름을 #if에 쓴 fixture는 Rocks가 막는다.
    const ShaderSourceStatus builtin =
        PreprocessShader(s7.tree, BuiltinEntry, s7.defines.Defines(), s7.shader);
    CHECK(builtin.code == ShaderSourceError::CompilerBuiltinInCondition && builtin.line == 1);
}

} // namespace

TEST(ShaderPreprocessS7_DxilMatchesDxcPreprocessing) {
    const StringView options[] = {"-E", "main", "-T", "cs_6_6", "-HV", "2021", "-Zpr"};
    RunS7("ShaderPreprocessS7_DxilMatchesDxcPreprocessing", "D3D12_SM6", options);
}

TEST(ShaderPreprocessS7_SpirvMatchesDxcPreprocessing) {
    const StringView options[] = {"-E",     "main",   "-T",
                                  "cs_6_0", "-HV",    "2021",
                                  "-Zpr",   "-spirv", "-fspv-target-env=vulkan1.1"};
    RunS7("ShaderPreprocessS7_SpirvMatchesDxcPreprocessing", "VULKAN_SM5", options);
}

} // namespace shader_preprocess_dxc_tests_file
