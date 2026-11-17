#include <Core/Encoding/Sha256.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Path.h>
#include <Platform/Process.h>
#include <ShaderToolLoader/ShaderToolLoader.h>
#include <TestSupport/DynamicLibraryFixture.h>
#include <TestSupport/ProcessProbe.h>
#include <TestSupport/Test.h>
#include <spirv-tools/libspirv.h>
#include <spirv_cross_c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace rocks;
using namespace rocks::platform;
using namespace rocks::shader;
using namespace rocks::test;

// 셰이더 도구 열기(ShaderToolLoader, Docs/Design/ShaderSystem.ko.md 9절 완료 기준)를 검사한다.
// - 실제 저장소: bootstrap한 사용자 저장소만으로 네 도구를 열고 함수 표를 실제로 부른다. 저장소가
//   없으면(bootstrap 전, CI) SKIP이며 실패가 아니다.
// - 대조 저장소: 시험 지원 코드가 쓴 데이터 전용 라이브러리(TestSupport/DynamicLibraryFixture.h)로
//   SHA-256·크기 불일치와 없는 파일을 열지 않음, 현재 디렉터리·PATH의 같은 이름 라이브러리를 쓰지
//   않음(P10 음성 대조 재사용)을 본다. 시험 라이브러리를 만들 수 없는 플랫폼은 SKIP이다.
namespace rocks::test {
bool RegisterProcessProbe(const char* mode, int (*function)(int, char**) noexcept) noexcept;
} // namespace rocks::test

namespace {

constexpr uint32 FixtureValue = 0x5EED7001u;
constexpr uint32 ProbeTimeout = 30'000;
constexpr usize PathCapacity = 1100;

uint32 NextSerial() noexcept {
    static uint32 serial = 0;
    return AtomicFetchAdd<MemoryOrder::Relaxed>(&serial, 1u) + 1u;
}

// 현재 디렉터리 아래의 절대 경로 작업 디렉터리다. 소멸 때 지운다(Windows는 올라온 DLL을 지울 수
// 없으므로 시험은 그 전에 모든 라이브러리를 닫는다).
struct Scratch {
    char path[1024]{};
    usize length = 0;

    bool Make(const char* tag) noexcept {
        char base[512];
        usize bytes = 0;
        if (!GetStandardDirectory(StandardDirectory::Working, base, sizeof(base), bytes)) {
            return false;
        }

        const int count = snprintf(
            path, sizeof(path), "%s%crocks-shadertool-%s-%llu-%u", base, NativePathSeparator(), tag,
            static_cast<unsigned long long>(MonotonicNanoseconds()), NextSerial());
        if (count <= 0 || static_cast<usize>(count) >= sizeof(path)) {
            return false;
        }

        length = static_cast<usize>(count);
        if (!MakeDirectoryTree(PhysicalFileSystem(), path, length)) {
            length = 0;
            return false;
        }

        return true;
    }

    // 이 디렉터리 아래 relative('/' 구분)의 절대 경로를 out에 쓰고 부모 디렉터리를 만든다.
    bool Child(const char* relative, char* out, usize capacity) const noexcept {
        const int count = snprintf(out, capacity, "%s%c%s", path, NativePathSeparator(), relative);
        if (count <= 0 || static_cast<usize>(count) >= capacity) {
            return false;
        }

        for (char* cursor = out + length + 1; *cursor != '\0'; ++cursor) {
            if (*cursor == '/') {
                *cursor = NativePathSeparator();
            }
        }

        char parent[PathCapacity];
        memcpy(parent, out, static_cast<usize>(count) + 1u);
        char* last = strrchr(parent, NativePathSeparator());
        if (last == nullptr) {
            return false;
        }

        *last = '\0';
        return static_cast<bool>(MakeDirectoryTree(PhysicalFileSystem(), parent, strlen(parent)));
    }

    ~Scratch() noexcept {
        if (length != 0) {
            (void)DeleteDirectoryTree(PhysicalFileSystem(), path, length);
        }
    }
};

// path 파일의 SHA-256과 크기다.
bool HashFile(const char* path, Sha256Digest& digest, uint64& size) noexcept {
    File file;
    if (!file.Open(PhysicalFileSystem(), path, strlen(path), FileOpenMode::ReadExisting)) {
        return false;
    }

    Sha256 hasher;
    uint8 buffer[4096];
    size = 0;
    while (true) {
        const FileTransfer read = file.Read(buffer, sizeof(buffer));
        if (!read.status) {
            (void)file.Close();
            return false;
        }

        if (read.bytes == 0) {
            break;
        }

        size += read.bytes;
        hasher.Update(Span<const uint8>(buffer, read.bytes));
    }

    digest = hasher.Finish();
    return static_cast<bool>(file.Close());
}

// 고유한 시험 라이브러리 파일 이름이다(예: "RocksToolA17.dll").
void LibraryName(const char* stem, uint32 serial, char* out, usize capacity) noexcept {
    snprintf(out, capacity, "RocksTool%s%u%s", stem, serial, DynamicLibraryExtension());
}

// RocksValue를 export하는 라이브러리다. importName이 있으면 그 라이브러리의 RocksValue를
// import한다.
bool WriteFixture(const char* path, const char* importName) noexcept {
    const FixtureExport exports[] = {{"RocksValue", FixtureValue}};
    FixtureLibrary library;
    library.exports = exports;
    library.exportCount = 1;
    if (importName != nullptr) {
        library.importLibrary = importName;
        library.importSymbol = "RocksValue";
        library.importSlotExport = "RocksImport";
    }

    return WriteFixtureLibrary(path, library);
}

bool SkipWithoutFixtures(const char* test) noexcept {
    if (DynamicLibraryFixturesSupported()) {
        return false;
    }

    printf("[SKIP] %s: fixture libraries are not generated on this platform\n", test);
    return true;
}

// 기본 저장소 루트다. 이 플랫폼에 저장소가 없으면(Android) false다.
bool DefaultStoreRoot(char* root, usize capacity, usize& bytes) noexcept {
    return static_cast<bool>(GetShaderToolStoreRoot(root, capacity, bytes));
}

// 대조 도구 항목 하나(runtime 파일 하나)다.
struct FixtureTool {
    char storeKey[Sha256Digest::HexBytes + 1]{};
    char path[128]{};
    ShaderToolFile file;
    ShaderTool tool;

    FixtureTool(const char* key, const char* fileName, const Sha256Digest& digest,
                uint64 size) noexcept {
        snprintf(storeKey, sizeof(storeKey), "%s", key);
        snprintf(path, sizeof(path), "bin/%s", fileName);
        file.path = path;
        file.sha256 = digest;
        file.size = size;
        file.role = ShaderToolFileRole::Runtime;
        file.hasDigest = true;
        tool.name = "RocksFixtureTool";
        tool.version = "1";
        tool.origin = ShaderToolOrigin::Build;
        tool.storeKey = storeKey;
        tool.files = &file;
        tool.fileCount = 1;
    }
};

} // namespace

TEST(ShaderToolLoader_RejectsInvalidArguments) {
    const ShaderTool* dxc = FindShaderTool(DxcToolName);
    REQUIRE(dxc != nullptr);
    ShaderToolLibrary library;
    // 저장소 루트는 절대 경로만 받는다(현재 디렉터리 기준 경로를 쓰지 않는다).
    CHECK(library.Open(*dxc, nullptr, 0).code == ShaderToolLoadCode::InvalidArgument);
    CHECK(library.Open(*dxc, "", 0).code == ShaderToolLoadCode::InvalidArgument);
    CHECK(library.Open(*dxc, "relative/store", 14).code == ShaderToolLoadCode::InvalidArgument);
    CHECK(library.Open(*dxc, "/abs\0olute", 10).code == ShaderToolLoadCode::InvalidArgument);
    CHECK(!library.IsOpen() && library.Tool() == nullptr);

    // 시스템 도구와 이 호스트 파일이 없는 도구는 열지 않는다.
    const ShaderTool* metal = FindShaderTool(MetalToolName);
    REQUIRE(metal != nullptr);
    char root[MaxShaderToolPathBytes];
    usize rootBytes = 0;
    if (DefaultStoreRoot(root, sizeof(root), rootBytes)) {
        const ShaderToolLoadStatus opened = library.Open(*metal, root, rootBytes);
        CHECK(opened.code == ShaderToolLoadCode::UnsupportedHost);
        CHECK(opened.tool != nullptr && strcmp(opened.tool, MetalToolName) == 0);
        ShaderTool hostless = *dxc;
        hostless.files = nullptr;
        hostless.fileCount = 0;
        CHECK(library.Open(hostless, root, rootBytes).code == ShaderToolLoadCode::UnsupportedHost);
    }

    // 열리지 않은 라이브러리에서 함수를 찾지 않는다.
    DxcFunctions functions;
    DynamicLibraryLease lease;
    CHECK(library.FindFunctions(functions, lease).code == ShaderToolLoadCode::WrongState);
    CHECK(functions.address[DxcFunctions::CreateInstance] == nullptr && !lease.IsHeld());
    uint32 major = 1;
    uint32 minor = 1;
    const ShaderToolLoadStatus query = QueryDxcVersion(functions, major, minor);
    CHECK(query.code == ShaderToolLoadCode::WrongState ||
          query.code == ShaderToolLoadCode::UnsupportedHost);
    CHECK(major == 0 && minor == 0);

    // 안내문은 bootstrap 명령을 담는다.
    CHECK(strstr(ShaderToolLoadMessage(ShaderToolLoadCode::NotBootstrapped),
                 ShaderToolBootstrapCommand) != nullptr);
    CHECK(strstr(ShaderToolLoadMessage(ShaderToolLoadCode::DigestMismatch),
                 ShaderToolBootstrapCommand) != nullptr);
    CHECK(strcmp(ShaderToolLoadCodeName(ShaderToolLoadCode::DigestMismatch), "DigestMismatch") ==
          0);
}

TEST(ShaderToolLoader_EmptyStoreAsksForBootstrap) {
    char root[MaxShaderToolPathBytes];
    usize rootBytes = 0;
    if (!DefaultStoreRoot(root, sizeof(root), rootBytes)) {
        printf(
            "[SKIP] ShaderToolLoader_EmptyStoreAsksForBootstrap: no tool store on this platform\n");
        return;
    }

    // 빈 저장소에서는 어느 도구도 열리지 않고 bootstrap을 안내한다(호스트 파일이 없으면
    // UnsupportedHost).
    Scratch empty;
    REQUIRE(empty.Make("empty"));
    ShaderToolSet tools;
    const ShaderToolLoadStatus opened = tools.Open(empty.path, empty.length);
    CHECK(opened.code == ShaderToolLoadCode::NotBootstrapped ||
          opened.code == ShaderToolLoadCode::UnsupportedHost);
    CHECK(!tools.IsOpen() && !tools.HasFxc());
    CHECK(tools.Dxc().address[DxcFunctions::CreateInstance] == nullptr);
}

// 완료 기준: 저장소만으로 워커 자리 모듈이 모든 도구를 열고 함수 표를 실제로 부른다.
TEST(ShaderToolLoader_OpensEveryToolFromBootstrappedStore) {
    char root[MaxShaderToolPathBytes];
    usize rootBytes = 0;
    if (!DefaultStoreRoot(root, sizeof(root), rootBytes)) {
        printf("[SKIP] ShaderToolLoader_OpensEveryToolFromBootstrappedStore: no tool store on this "
               "platform\n");
        return;
    }

    ShaderToolSet tools;
    const ShaderToolLoadStatus opened = tools.Open(root, rootBytes);
    if (opened.code == ShaderToolLoadCode::NotBootstrapped ||
        opened.code == ShaderToolLoadCode::UnsupportedHost) {
        printf("[SKIP] ShaderToolLoader_OpensEveryToolFromBootstrappedStore: %s %s (%s)\n",
               opened.tool != nullptr ? opened.tool : "?", ShaderToolLoadCodeName(opened.code),
               ShaderToolLoadMessage(opened.code));
        return;
    }

    if (!opened) {
        fprintf(stderr, "shader tool open failed: %s %s symbol=%s native=%u\n",
                opened.tool != nullptr ? opened.tool : "?", ShaderToolLoadCodeName(opened.code),
                opened.symbol != nullptr ? opened.symbol : "-", opened.file.nativeError);
    }

    REQUIRE(opened);
    CHECK(tools.IsOpen());

    // DXC: COM 객체를 만들어 버전을 읽는다(Windows 구현).
    uint32 major = 0;
    uint32 minor = 0;
    const ShaderToolLoadStatus version = QueryDxcVersion(tools.Dxc(), major, minor);
    CHECK(version || version.code == ShaderToolLoadCode::UnsupportedHost);
    if (version) {
        CHECK(major == 1u);
    }

    // FXC: 표에 이 호스트 파일이 있으면 함께 열린다(Windows).
    if (tools.HasFxc()) {
        CHECK(tools.Fxc().address[FxcFunctions::Compile] != nullptr);
        CHECK(tools.Fxc().address[FxcFunctions::Reflect] != nullptr);
    }

    // SPIRV-Tools: 최소 compute 모듈을 검증한다.
    // 최소 compute 셰이더(SPIR-V 1.0): OpEntryPoint GLCompute "main", LocalSize 4 2 1, 빈 본문.
    const uint32_t module[] = {
        0x07230203u, 0x00010000u, 0x00000000u, 5u,          0u, 0x00020011u, 1u,
        0x0003000Eu, 0u,          1u,          0x0005000Fu, 5u, 1u,          0x6E69616Du,
        0x00000000u, 0x00060010u, 1u,          17u,         4u, 2u,          1u,
        0x00020013u, 2u,          0x00030021u, 3u,          2u, 0x00050036u, 2u,
        1u,          0u,          3u,          0x000200F8u, 4u, 0x000100FDu, 0x00010038u};
    const usize words = sizeof(module) / sizeof(module[0]);
    const SpirvToolsFunctions& spv = tools.SpirvTools();
    const auto versionString = reinterpret_cast<decltype(&spvSoftwareVersionString)>(
        spv.address[SpirvToolsFunctions::SoftwareVersionString]);
    const char* spvVersion = versionString();
    CHECK(spvVersion != nullptr && spvVersion[0] != '\0');
    const auto contextCreate = reinterpret_cast<decltype(&spvContextCreate)>(
        spv.address[SpirvToolsFunctions::ContextCreate]);
    const auto contextDestroy = reinterpret_cast<decltype(&spvContextDestroy)>(
        spv.address[SpirvToolsFunctions::ContextDestroy]);
    const auto optionsCreate = reinterpret_cast<decltype(&spvValidatorOptionsCreate)>(
        spv.address[SpirvToolsFunctions::ValidatorOptionsCreate]);
    const auto optionsDestroy = reinterpret_cast<decltype(&spvValidatorOptionsDestroy)>(
        spv.address[SpirvToolsFunctions::ValidatorOptionsDestroy]);
    const auto validate = reinterpret_cast<decltype(&spvValidateWithOptions)>(
        spv.address[SpirvToolsFunctions::ValidateWithOptions]);
    const auto diagnosticDestroy = reinterpret_cast<decltype(&spvDiagnosticDestroy)>(
        spv.address[SpirvToolsFunctions::DiagnosticDestroy]);
    spv_context context = contextCreate(SPV_ENV_VULKAN_1_1);
    REQUIRE(context != nullptr);
    spv_validator_options options = optionsCreate();
    spv_const_binary_t binary{module, words};
    spv_diagnostic diagnostic = nullptr;
    CHECK(validate(context, options, &binary, &diagnostic) == SPV_SUCCESS);
    diagnosticDestroy(diagnostic);
    // 음성 대조: 잘못된 magic은 검증이 거부한다.
    uint32_t broken[sizeof(module) / sizeof(module[0])];
    memcpy(broken, module, sizeof(module));
    broken[0] = 0xDEADBEEFu;
    spv_const_binary_t brokenBinary{broken, words};
    diagnostic = nullptr;
    CHECK(validate(context, options, &brokenBinary, &diagnostic) != SPV_SUCCESS);
    diagnosticDestroy(diagnostic);
    optionsDestroy(options);
    contextDestroy(context);

    // SPIRV-Cross: 같은 모듈을 ESSL 320으로 바꾼다.
    const SpirvCrossFunctions& cross = tools.SpirvCross();
    const auto getVersion = reinterpret_cast<decltype(&spvc_get_version)>(
        cross.address[SpirvCrossFunctions::GetVersion]);
    const auto crossCreate = reinterpret_cast<decltype(&spvc_context_create)>(
        cross.address[SpirvCrossFunctions::ContextCreate]);
    const auto crossDestroy = reinterpret_cast<decltype(&spvc_context_destroy)>(
        cross.address[SpirvCrossFunctions::ContextDestroy]);
    const auto parse = reinterpret_cast<decltype(&spvc_context_parse_spirv)>(
        cross.address[SpirvCrossFunctions::ContextParseSpirv]);
    const auto createCompiler = reinterpret_cast<decltype(&spvc_context_create_compiler)>(
        cross.address[SpirvCrossFunctions::ContextCreateCompiler]);
    const auto createOptions = reinterpret_cast<decltype(&spvc_compiler_create_compiler_options)>(
        cross.address[SpirvCrossFunctions::CompilerCreateCompilerOptions]);
    const auto setBool = reinterpret_cast<decltype(&spvc_compiler_options_set_bool)>(
        cross.address[SpirvCrossFunctions::CompilerOptionsSetBool]);
    const auto setUint = reinterpret_cast<decltype(&spvc_compiler_options_set_uint)>(
        cross.address[SpirvCrossFunctions::CompilerOptionsSetUint]);
    const auto install = reinterpret_cast<decltype(&spvc_compiler_install_compiler_options)>(
        cross.address[SpirvCrossFunctions::CompilerInstallCompilerOptions]);
    const auto compile = reinterpret_cast<decltype(&spvc_compiler_compile)>(
        cross.address[SpirvCrossFunctions::CompilerCompile]);
    unsigned crossMajor = 0;
    unsigned crossMinor = 0;
    unsigned crossPatch = 0;
    getVersion(&crossMajor, &crossMinor, &crossPatch);
    CHECK(crossMajor == static_cast<unsigned>(SPVC_C_API_VERSION_MAJOR));
    spvc_context crossContext = nullptr;
    REQUIRE(crossCreate(&crossContext) == SPVC_SUCCESS);
    spvc_parsed_ir ir = nullptr;
    spvc_compiler compiler = nullptr;
    spvc_compiler_options compilerOptions = nullptr;
    const char* source = nullptr;
    bool compiled =
        parse(crossContext, module, words, &ir) == SPVC_SUCCESS &&
        createCompiler(crossContext, SPVC_BACKEND_GLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP,
                       &compiler) == SPVC_SUCCESS &&
        createOptions(compiler, &compilerOptions) == SPVC_SUCCESS &&
        setUint(compilerOptions, SPVC_COMPILER_OPTION_GLSL_VERSION, 320u) == SPVC_SUCCESS &&
        setBool(compilerOptions, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE) == SPVC_SUCCESS &&
        install(compiler, compilerOptions) == SPVC_SUCCESS &&
        compile(compiler, &source) == SPVC_SUCCESS;
    CHECK(compiled);
    if (compiled) {
        CHECK(strstr(source, "#version 320 es") != nullptr);
        CHECK(strstr(source, "local_size_x = 4") != nullptr);
    }

    crossDestroy(crossContext);
    tools.Close();
    CHECK(!tools.IsOpen());
}

// 완료 기준: 파일 SHA-256이나 크기가 표와 다르면 열지 않는다. 없으면 bootstrap을 안내한다.
TEST(ShaderToolLoader_RejectsDigestAndSizeMismatch) {
    if (SkipWithoutFixtures("ShaderToolLoader_RejectsDigestAndSizeMismatch")) {
        return;
    }

    Scratch store;
    REQUIRE(store.Make("digest"));
    const char* key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char name[64];
    LibraryName("A", NextSerial(), name, sizeof(name));
    char relative[200];
    snprintf(relative, sizeof(relative), "%s/bin/%s", key, name);
    char path[PathCapacity];
    REQUIRE(store.Child(relative, path, sizeof(path)));
    REQUIRE(WriteFixture(path, nullptr));
    Sha256Digest digest;
    uint64 size = 0;
    REQUIRE(HashFile(path, digest, size));

    // SHA-256 한 byte가 다르다.
    Sha256Digest wrong = digest;
    wrong.bytes[Sha256Digest::Bytes - 1] ^= 0x01u;
    FixtureTool tampered(key, name, wrong, size);
    ShaderToolLibrary library;
    ShaderToolLoadStatus opened = library.Open(tampered.tool, store.path, store.length);
    CHECK(opened.code == ShaderToolLoadCode::DigestMismatch);
    CHECK(opened.tool != nullptr && strcmp(opened.tool, "RocksFixtureTool") == 0);
    CHECK(!library.IsOpen() && !LibraryIsLoaded(name));

    // 크기가 다르다.
    FixtureTool resized(key, name, digest, size + 1u);
    CHECK(library.Open(resized.tool, store.path, store.length).code ==
          ShaderToolLoadCode::DigestMismatch);
    CHECK(!LibraryIsLoaded(name));

    // 다른 저장소 디렉터리(없는 파일)는 bootstrap 안내다.
    FixtureTool missing("fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210", name,
                        digest, size);
    CHECK(library.Open(missing.tool, store.path, store.length).code ==
          ShaderToolLoadCode::NotBootstrapped);
    CHECK(!LibraryIsLoaded(name));

    // 양성 대조: 표와 같으면 열고 함수를 찾는다. 없는 심볼은 SymbolMissing과 그 이름이다.
    FixtureTool matching(key, name, digest, size);
    REQUIRE(library.Open(matching.tool, store.path, store.length));
    CHECK(library.IsOpen() && library.Tool() == &matching.tool);
    CHECK(library.Open(matching.tool, store.path, store.length).code ==
          ShaderToolLoadCode::WrongState);
    DynamicSymbol symbols[] = {{"RocksValue", nullptr}};
    DynamicLibraryLease lease;
    CHECK(library.Find(symbols, 1, lease));
    REQUIRE(symbols[0].address != nullptr);
    uint32 value = 0;
    memcpy(&value, symbols[0].address, sizeof(value));
    CHECK(value == FixtureValue);
    DynamicSymbol absent[] = {{"RocksValue", nullptr}, {"RocksNoSuchSymbol", nullptr}};
    DynamicLibraryLease unused;
    const ShaderToolLoadStatus found = library.Find(absent, 2, unused);
    CHECK(found.code == ShaderToolLoadCode::SymbolMissing);
    CHECK(found.symbol != nullptr && strcmp(found.symbol, "RocksNoSuchSymbol") == 0);
    CHECK(library.Close().code == FileCode::Busy);
    lease.Reset();
    CHECK(library.Close());
    CHECK(!library.IsOpen() && library.Tool() == nullptr);
}

namespace {

// 이 실행 파일을 "--process-probe shader-tool-open <저장소> <키> <이름> <sha> <크기> [PATH
// 디렉터리]"로 작업 디렉터리 workingDirectory에서 띄우고 종료 코드를 준다. 실행하지 못했거나 시간이
// 다했으면 1000이다.
uint32 RunOpenProbe(const char* storeRoot, const char* key, const char* name,
                    const Sha256Digest& digest, uint64 size, const char* workingDirectory,
                    const char* pathDirectory) noexcept {
    char executable[4096];
    usize executableBytes = 0;
    if (!GetExecutablePath(executable, sizeof(executable), executableBytes)) {
        return 1000;
    }

    char hex[Sha256Digest::HexBytes + 1];
    digest.ToHex(hex);
    char sizeText[32];
    snprintf(sizeText, sizeof(sizeText), "%llu", static_cast<unsigned long long>(size));
    ProcessArgument arguments[8] = {{"--process-probe", 15},        {"shader-tool-open", 16},
                                    {storeRoot, strlen(storeRoot)}, {key, strlen(key)},
                                    {name, strlen(name)},           {hex, strlen(hex)},
                                    {sizeText, strlen(sizeText)},   {}};
    usize argumentCount = 7;
    if (pathDirectory != nullptr) {
        arguments[argumentCount++] = {pathDirectory, strlen(pathDirectory)};
    }

    ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments;
    info.argumentCount = argumentCount;
    info.workingDirectory = workingDirectory;
    info.workingDirectoryBytes = strlen(workingDirectory);
    info.output = ProcessStreamMode::Null;
    info.error = ProcessStreamMode::Null;
    ProcessRunOptions options;
    options.timeoutMilliseconds = ProbeTimeout;
    const ProcessRunResult run = RunProcess(info, options);
    if (!run.status || run.timedOut || run.exit.kind != ProcessExitKind::Exited) {
        return 1000;
    }

    return run.exit.code;
}

// probe 자식 쪽이다. 마지막 인자가 있으면 PATH 맨 앞에 더한 뒤 대조 항목을 ShaderToolLibrary로
// 연다. 종료 코드: 열림 0, NotBootstrapped 10, LoadFailed(NotFound) 11, DigestMismatch 12, 그 밖의
// 실패 13, 준비 실패 14, 열지 못했는데 같은 이름 라이브러리가 프로세스에 올라와 있으면 15다.
int ShaderToolOpenProbe(int argc, char** argv) noexcept {
    static char storage[16 * 1024];
    const char* arguments[10];
    const usize count = ReadUtf8Arguments(argc, argv, storage, sizeof(storage), arguments, 10);
    if (count != 8 && count != 9) {
        return 14;
    }

    if (count == 9 && !PrependSearchPath(arguments[8])) {
        return 14;
    }

    const Result<Sha256Digest> digest = Sha256Digest::ParseHex(StringView(arguments[6]));
    if (!digest) {
        return 14;
    }

    const char* storeRoot = arguments[3];
    const char* name = arguments[5];
    FixtureTool fixture(arguments[4], name, digest.Value(), strtoull(arguments[7], nullptr, 10));
    ShaderToolLibrary library;
    const ShaderToolLoadStatus opened = library.Open(fixture.tool, storeRoot, strlen(storeRoot));
    if (opened) {
        return library.Close() ? 0 : 13;
    }

    if (LibraryIsLoaded(name)) {
        return 15;
    }

    switch (opened.code) {
    case ShaderToolLoadCode::NotBootstrapped:
        return 10;
    case ShaderToolLoadCode::LoadFailed:
        return opened.file.code == FileCode::NotFound ? 11 : 13;
    case ShaderToolLoadCode::DigestMismatch:
        return 12;
    default:
        return 13;
    }
}

const bool ShaderToolOpenProbeRegistered =
    RegisterProcessProbe("shader-tool-open", &ShaderToolOpenProbe);

} // namespace

// 완료 기준(P10 음성 대조 재사용): 현재 디렉터리·PATH에 둔 같은 이름 라이브러리를 열지 않는다.
// - 도구 A가 저장소에 없고 같은 이름 A가 현재 디렉터리·PATH에만 있으면 bootstrap 안내로 끝나고 A는
//   올라오지 않는다.
// - 저장소의 A가 의존하는 B가 현재 디렉터리·PATH에만 있으면 A를 열지 못한다(NotFound).
// 양성 대조로 B를 A 옆에 두면 같은 probe가 연다. 시험 프로세스의 현재 디렉터리·환경을 바꾸지 않도록
// 자식 probe에서 연다.
TEST(ShaderToolLoader_IgnoresWorkingDirectoryAndPath) {
    if (SkipWithoutFixtures("ShaderToolLoader_IgnoresWorkingDirectoryAndPath") ||
        !ProcessSpawnExpected()) {
        return;
    }

    REQUIRE(ShaderToolOpenProbeRegistered);
    Scratch store;
    Scratch emptyStore;
    Scratch dependencyDirectory;
    Scratch elsewhere;
    REQUIRE(store.Make("probe-store"));
    REQUIRE(emptyStore.Make("probe-empty"));
    REQUIRE(dependencyDirectory.Make("probe-cwd"));
    REQUIRE(elsewhere.Make("probe-elsewhere"));
    const char* key = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    const uint32 serial = NextSerial();
    char nameA[64];
    char nameB[64];
    LibraryName("A", serial, nameA, sizeof(nameA));
    LibraryName("B", serial, nameB, sizeof(nameB));
    char relative[200];
    snprintf(relative, sizeof(relative), "%s/bin/%s", key, nameA);
    char pathA[PathCapacity];
    REQUIRE(store.Child(relative, pathA, sizeof(pathA)));
    REQUIRE(WriteFixture(pathA, nameB));
    Sha256Digest digestA;
    uint64 sizeA = 0;
    REQUIRE(HashFile(pathA, digestA, sizeA));
    char pathB[PathCapacity];
    REQUIRE(dependencyDirectory.Child(nameB, pathB, sizeof(pathB)));
    REQUIRE(WriteFixture(pathB, nullptr));
    // 같은 이름 A를 현재 디렉터리(dependencyDirectory)에도 둔다.
    char strayA[PathCapacity];
    REQUIRE(dependencyDirectory.Child(nameA, strayA, sizeof(strayA)));
    REQUIRE(WriteFixture(strayA, nameB));

    // 저장소가 비었다: 현재 디렉터리·PATH의 같은 이름 A를 쓰지 않는다.
    CHECK(RunOpenProbe(emptyStore.path, key, nameA, digestA, sizeA, dependencyDirectory.path,
                       nullptr) == 10);
    CHECK(RunOpenProbe(emptyStore.path, key, nameA, digestA, sizeA, elsewhere.path,
                       dependencyDirectory.path) == 10);

    // 의존 B가 현재 디렉터리에만·PATH에만 있다.
    CHECK(RunOpenProbe(store.path, key, nameA, digestA, sizeA, dependencyDirectory.path, nullptr) ==
          11);
    CHECK(RunOpenProbe(store.path, key, nameA, digestA, sizeA, elsewhere.path,
                       dependencyDirectory.path) == 11);

    // 양성 대조: B를 저장소의 A 옆에 두면 연다.
    char besideA[PathCapacity];
    snprintf(relative, sizeof(relative), "%s/bin/%s", key, nameB);
    REQUIRE(store.Child(relative, besideA, sizeof(besideA)));
    REQUIRE(WriteFixture(besideA, nullptr));
    CHECK(RunOpenProbe(store.path, key, nameA, digestA, sizeA, elsewhere.path, nullptr) == 0);
}
