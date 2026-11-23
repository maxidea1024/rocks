#include <ShaderWorker/ShaderWorker.h>

#include <Core/Build/BuildInfo.h>
#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <DeterministicMath/DeterministicMath.h>
#include <Platform/Fatal.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <ShaderCompilerCommon/ShaderFormatBackend.h>
#include <ShaderCompilerCommon/ShaderWorkerProtocol.h>
#include <ShaderFormatD3D/D3DShaderFormat.h>
#include <ShaderFormatVulkan/VulkanShaderFormat.h>
#include <ShaderToolLoader/ShaderToolLoader.h>
#include <stdio.h>
#include <string.h>

// 셰이더 컴파일 워커 본체(Docs/Design/ShaderSystem.ko.md 11절, 11.1절 SC4–SC7)다. 규칙은 공개 헤더
// 주석을 따른다.
namespace rocks::shader {

namespace shader_worker_detail {

constexpr StringView WorkDirectoryOption("--work-dir");
constexpr StringView ParentOption("--parent-pid");
constexpr StringView WorkerOption("--worker");
constexpr StringView KeepInputOption("--keep-input");
constexpr StringView TestHooksOption("--test-hooks");
// u32 10진 자릿수 상한이다.
constexpr usize MaxDecimalDigits = 10;
// "done "·"held "·"child " 뒤 숫자를 붙인 줄의 버퍼다.
constexpr usize ControlReplyBytes = 32;

bool ParseUnsigned(StringView text, uint64 limit, uint64& value) noexcept {
    if (text.IsEmpty() || text.ByteCount() > MaxDecimalDigits) {
        return false;
    }

    value = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') {
            return false;
        }

        value = value * 10 + static_cast<uint64>(digit - '0');
    }

    return value <= limit;
}

int32 Exit(ShaderWorkerExit code) noexcept {
    return static_cast<int32>(code);
}

// 표준 입력·출력 통로다. 출력은 줄마다 비운다(관리자가 줄 단위로 읽는다).
class StandardControl final : public ShaderWorkerControl {
public:
    bool ReadLine(char* buffer, usize capacity, usize& bytes) noexcept override {
        bytes = 0;
        if (capacity == 0 || fgets(buffer, static_cast<int>(capacity), stdin) == nullptr) {
            return false;
        }

        bytes = strlen(buffer);
        const bool complete = bytes > 0 && buffer[bytes - 1] == '\n';
        while (bytes > 0 && (buffer[bytes - 1] == '\n' || buffer[bytes - 1] == '\r')) {
            buffer[--bytes] = '\0';
        }

        // 너무 긴 줄의 나머지는 버린다.
        if (!complete) {
            int value = 0;
            while ((value = fgetc(stdin)) != EOF && value != '\n') {
            }
        }

        return true;
    }

    bool WriteLine(StringView text) noexcept override {
        return fwrite(text.Data(), 1, text.ByteCount(), stdout) == text.ByteCount() &&
               fputc('\n', stdout) != EOF && fflush(stdout) == 0;
    }
};

// 워커 한 실행의 상태다.
class Session final {
public:
    Session(const ShaderWorkerArguments& arguments, ShaderWorkerControl& control,
            Allocator& allocator) noexcept
        : arguments_(arguments), control_(control), allocator_(allocator), children_(allocator),
          formats_(allocator) {
    }

    ~Session() noexcept {
        // 띄운 자식(시험 hook)을 정리한다(Process::Close가 자손까지 끝낸다 — 11절 6항).
        children_.Clear();
        (void)log_.Close();
    }

    int32 Run() noexcept {
        OpenLog();
        Log("ShaderCompileWorker started");
        char line[MaxShaderWorkerControlLineBytes];
        usize bytes = 0;
        while (control_.ReadLine(line, sizeof(line), bytes)) {
            uint64 sequence = 0;
            if (!ParseUnsigned(StringView(line, bytes), NumericLimits<uint32>::Max(), sequence)) {
                Log("invalid control line");
                return Exit(ShaderWorkerExit::GeneralFailure);
            }

            bool endOfInput = false;
            const int32 code = Batch(static_cast<uint32>(sequence), endOfInput);
            if (code != Exit(ShaderWorkerExit::Success) || endOfInput) {
                return code;
            }
        }

        Log("input closed");
        return Exit(ShaderWorkerExit::Success);
    }

private:
    void OpenLog() noexcept {
        char path[MaxShaderWorkerPathBytes];
        if (Path("worker.log", path)) {
            (void)log_.Open(path, platform::FileOpenMode::CreateAlways);
        }
    }

    void Log(const char* message) noexcept {
        if (log_.IsOpen()) {
            (void)log_.WriteAll(message, strlen(message));
            (void)log_.WriteAll("\n", 1);
        }
    }

    // <work-dir>/<name>이다.
    bool Path(const char* name, char (&out)[MaxShaderWorkerPathBytes]) const noexcept {
        const StringView directory = arguments_.workDirectory;
        const int written =
            snprintf(out, sizeof(out), "%.*s/%s", static_cast<int>(directory.ByteCount()),
                     directory.Data(), name);
        return written > 0 && static_cast<usize>(written) < sizeof(out);
    }

    bool Reply(const char* verb, uint64 value) noexcept {
        char text[ControlReplyBytes];
        const int written =
            snprintf(text, sizeof(text), "%s %llu", verb, static_cast<unsigned long long>(value));
        return written > 0 && control_.WriteLine(StringView(text, static_cast<usize>(written)));
    }

    static bool ReadFile(const char* path, Array<uint8>& bytes) noexcept {
        platform::File file;
        uint64 size = 0;
        if (!file.Open(path, platform::FileOpenMode::ReadExisting) || !file.Size(size) ||
            size > MaxShaderWorkerFileBytes || !bytes.TryResize(static_cast<usize>(size))) {
            return false;
        }

        const platform::FileTransfer read = file.ReadExact(bytes.Data(), bytes.Size());
        return static_cast<bool>(file.Close()) && static_cast<bool>(read.status);
    }

    // 출력을 임시 이름에 다 쓴 뒤 no-replace 이동으로 놓는다(관리자가 덜 쓴 출력을 읽지 않는다).
    bool SaveOutput(uint32 sequence, Span<const uint8> bytes) noexcept {
        char name[ControlReplyBytes];
        char temporary[MaxShaderWorkerPathBytes];
        char final[MaxShaderWorkerPathBytes];
        snprintf(name, sizeof(name), "output.%u.tmp", sequence);
        if (!Path(name, temporary)) {
            return false;
        }

        snprintf(name, sizeof(name), "output.%u", sequence);
        if (!Path(name, final)) {
            return false;
        }

        platform::File file;
        if (!file.Open(temporary, platform::FileOpenMode::CreateAlways)) {
            return false;
        }

        const platform::FileTransfer written = file.WriteAll(bytes.Data(), bytes.Size());
        if (!file.Close() || !written.status) {
            return false;
        }

        return static_cast<bool>(platform::PhysicalFileSystem().Move(temporary, strlen(temporary),
                                                                     final, strlen(final), false));
    }

    int32 Batch(uint32 sequence, bool& endOfInput) noexcept {
        char name[ControlReplyBytes];
        char input[MaxShaderWorkerPathBytes];
        snprintf(name, sizeof(name), "input.%u", sequence);
        Array<uint8> bytes(allocator_);
        if (!Path(name, input) || !ReadFile(input, bytes)) {
            Log("cannot read input file");
            return Exit(ShaderWorkerExit::BadInputFile);
        }

        if (!arguments_.keepInput && !platform::PhysicalFileSystem().Delete(input, strlen(input))) {
            Log("cannot delete input file");
            return Exit(ShaderWorkerExit::CannotDeleteInput);
        }

        const Result<uint32> version =
            ReadShaderWorkerMessageVersion(bytes.View(), ShaderWorkerInputMagic);
        if (!version) {
            Log("input file has no message prefix");
            return Exit(ShaderWorkerExit::BadInputFile);
        }

        if (version.Value() != ShaderWorkerMessageVersion) {
            Log("message version mismatch");
            return Exit(ShaderWorkerExit::BadMessageVersion);
        }

        ShaderWorkerRequest request(allocator_);
        if (const Result<void> decoded = DecodeShaderWorkerRequest(bytes.View(), request);
            !decoded) {
            Log("cannot decode input file");
            return Exit(decoded.Error() == ErrorCode::OutOfMemory ? ShaderWorkerExit::OutOfMemory
                                                                  : ShaderWorkerExit::BadInputFile);
        }

        if (const int32 checked = CheckHeader(request);
            checked != Exit(ShaderWorkerExit::Success)) {
            return checked;
        }

        ShaderWorkerResponse response(allocator_);
        if (request.kind == ShaderWorkerRequestKind::ToolQuery) {
            QueryTools(response);
        } else {
            for (const ShaderWorkerJob& job : request.jobs) {
                auto result = response.jobs.TryEmplaceBack(allocator_);
                if (!result) {
                    return Exit(ShaderWorkerExit::OutOfMemory);
                }

                if (job.kind != ShaderWorkerJobKind::Compile) {
                    if (!Hook(sequence, job, *result.Value(), endOfInput)) {
                        return Exit(ShaderWorkerExit::GeneralFailure);
                    }

                    if (endOfInput) {
                        // Hold 중 입력이 닫혔다(부모 종료) — 출력 없이 끝낸다.
                        return Exit(ShaderWorkerExit::Success);
                    }

                    continue;
                }

                CompileJob(job, *result.Value());
            }
        }

        Array<uint8> output(allocator_);
        if (const Result<void> encoded = EncodeShaderWorkerResponse(response, output); !encoded) {
            Log("cannot encode output");
            return Exit(encoded.Error() == ErrorCode::OutOfMemory
                            ? ShaderWorkerExit::OutOfMemory
                            : ShaderWorkerExit::CannotSaveOutput);
        }

        if (!SaveOutput(sequence, output.View())) {
            Log("cannot save output file");
            return Exit(ShaderWorkerExit::CannotSaveOutput);
        }

        return Reply("done", sequence) ? Exit(ShaderWorkerExit::Success)
                                       : Exit(ShaderWorkerExit::GeneralFailure);
    }

    // 머리 대조와 형식 정의 읽기다(SC5·SC6).
    int32 CheckHeader(const ShaderWorkerRequest& request) noexcept {
        const ShaderWorkerHeader& header = request.header;
        if (!(header.toolchain == build::ToolchainFingerprint()) ||
            header.mathVersion != deterministic::Version) {
            Log("toolchain fingerprint or deterministic math version mismatch");
            return Exit(ShaderWorkerExit::BadToolchain);
        }

        if (!(header.toolTable == ShaderToolTableSha256())) {
            Log("tool table mismatch");
            return Exit(ShaderWorkerExit::BadToolTable);
        }

        formats_.Clear();

        for (const ShaderWorkerFormat& format : request.formats) {
            ShaderFormatDefinition definition;
            if (!ParseShaderFormatDefinition(format.bytes.View(), format.id.View(), definition) ||
                !formats_.TryPushBack(definition)) {
                Log("invalid format definition in input");
                return Exit(ShaderWorkerExit::BadInputFile);
            }
        }

        for (const ShaderWorkerProcessorVersion& processor : header.processors) {
            const ShaderFormatDefinition* definition = FindFormat(StringView(processor.formatId));
            if (definition == nullptr || definition->moduleVersion != processor.version) {
                Log("processor version table does not match the format definitions");
                return Exit(ShaderWorkerExit::BadInputFile);
            }

            ShaderFormatBackend* backend = Backend(definition->module);
            if (backend != nullptr && backend->ProcessorVersion() != processor.version) {
                Log("shader format processor version mismatch");
                return Exit(ShaderWorkerExit::BadFormatVersion);
            }
        }

        return Exit(ShaderWorkerExit::Success);
    }

    const ShaderFormatDefinition* FindFormat(StringView id) const noexcept {
        for (usize index = 0; index < formats_.Size(); ++index) {
            if (formats_[index].Id() == id) {
                return &formats_[index];
            }
        }

        return nullptr;
    }

    ShaderFormatBackend* Backend(ShaderFormatModule module) noexcept {
        switch (module) {
        case ShaderFormatModule::D3D:
            return &d3d_;
        case ShaderFormatModule::Vulkan:
            return &vulkan_;
        case ShaderFormatModule::Metal:
        case ShaderFormatModule::OpenGL:
            break;
        }

        return nullptr;
    }

    // 도구를 처음 쓸 때 연다(워커 수명 동안 연 채로 둔다).
    const ShaderToolSet* Tools(String& failure) noexcept {
        if (!toolsTried_) {
            toolsTried_ = true;
            char root[MaxShaderToolPathBytes];
            usize rootBytes = 0;
            if (!GetShaderToolStoreRoot(root, sizeof(root), rootBytes)) {
                toolStatus_.code = ShaderToolLoadCode::UnsupportedHost;
            } else {
                toolStatus_ = tools_.Open(root, rootBytes);
            }
        }

        if (tools_.IsOpen()) {
            return &tools_;
        }

        (void)failure.TryAppend(toolStatus_.tool != nullptr ? StringView(toolStatus_.tool)
                                                            : StringView("shader tools"));
        (void)failure.TryAppend(" ");
        (void)failure.TryAppend(ShaderToolLoadCodeName(toolStatus_.code));
        (void)failure.TryAppend(": ");
        (void)failure.TryAppend(ShaderToolLoadMessage(toolStatus_.code));
        return nullptr;
    }

    void CompileJob(const ShaderWorkerJob& job, ShaderWorkerJobResult& result) noexcept {
        const ShaderFormatDefinition* format = FindFormat(job.formatId.View());
        if (format == nullptr) {
            FailShaderJob(result, ShaderWorkerJobStatus::ToolError,
                          "the job's format definition is missing from the request",
                          job.formatId.View());
            return;
        }

        ShaderFormatBackend* backend = Backend(format->module);
        if (backend == nullptr) {
            FailShaderJob(result, ShaderWorkerJobStatus::UnsupportedCapability,
                          "this shader compile worker has no processor for the format",
                          format->Id());
            return;
        }

        String failure(allocator_);
        const ShaderToolSet* tools = Tools(failure);
        if (tools == nullptr) {
            FailShaderJob(result, ShaderWorkerJobStatus::ToolError,
                          "shader tools are not available", failure.View());
            return;
        }

        backend->Compile(*format, job, tools, result);
    }

    // 저장소 도구마다 열어 확인한다(11절 3항). system 도구 지문은 SH1e(Metal)에서 채운다.
    void QueryTools(ShaderWorkerResponse& response) noexcept {
        char root[MaxShaderToolPathBytes];
        usize rootBytes = 0;
        const bool hasStore =
            static_cast<bool>(GetShaderToolStoreRoot(root, sizeof(root), rootBytes));
        for (const ShaderTool& tool : ShaderTools()) {
            if (tool.origin == ShaderToolOrigin::System) {
                auto system = response.systemTools.TryEmplaceBack(allocator_);
                if (system) {
                    (void)system.Value()->name.TryAssign(StringView(tool.name));
                }

                continue;
            }

            auto check = response.tools.TryEmplaceBack(allocator_);
            if (!check) {
                return;
            }

            (void)check.Value()->name.TryAssign(StringView(tool.name));
            if (!hasStore) {
                check.Value()->state = ShaderWorkerToolState::UnsupportedHost;
                continue;
            }

            ShaderToolLibrary library;
            const ShaderToolLoadStatus opened = library.Open(tool, root, rootBytes);
            switch (opened.code) {
            case ShaderToolLoadCode::Success:
                check.Value()->state = ShaderWorkerToolState::Ready;
                break;
            case ShaderToolLoadCode::NotBootstrapped:
                check.Value()->state = ShaderWorkerToolState::NotBootstrapped;
                break;
            case ShaderToolLoadCode::DigestMismatch:
                check.Value()->state = ShaderWorkerToolState::DigestMismatch;
                break;
            case ShaderToolLoadCode::UnsupportedHost:
                check.Value()->state = ShaderWorkerToolState::UnsupportedHost;
                break;
            default:
                check.Value()->state = ShaderWorkerToolState::Failed;
                break;
            }

            (void)library.Close();
        }
    }

    // 시험 hook(SC7)이다. false는 제어 통로 실패다.
    bool Hook(uint32 sequence, const ShaderWorkerJob& job, ShaderWorkerJobResult& result,
              bool& endOfInput) noexcept {
        if (!arguments_.testHooks) {
            FailShaderJob(result, ShaderWorkerJobStatus::ToolError,
                          "test hook jobs need a worker started with --test-hooks");
            return true;
        }

        switch (job.kind) {
        case ShaderWorkerJobKind::Crash: {
            Log("test hook: crash");
            volatile int32* volatile target = crashTarget_;
            *target = 1;
            return false;
        }
        case ShaderWorkerJobKind::Assert:
            Log("test hook: assert");
            platform::Fatal("ShaderCompileWorker test hook assert");
        case ShaderWorkerJobKind::SpawnChild:
            if (!SpawnChild(job, result)) {
                return true;
            }

            return Hold(sequence, endOfInput);
        case ShaderWorkerJobKind::Hold:
            return Hold(sequence, endOfInput);
        case ShaderWorkerJobKind::Compile:
            break;
        }

        return true;
    }

    // `held <seq>`를 쓰고 다음 입력 줄(내용은 보지 않는다)까지 막힌다. EOF면 endOfInput이다.
    bool Hold(uint32 sequence, bool& endOfInput) noexcept {
        if (!Reply("held", sequence)) {
            return false;
        }

        char line[MaxShaderWorkerControlLineBytes];
        usize bytes = 0;
        endOfInput = !control_.ReadLine(line, sizeof(line), bytes);
        return true;
    }

    bool SpawnChild(const ShaderWorkerJob& job, ShaderWorkerJobResult& result) noexcept {
        if (job.hookArguments.IsEmpty()) {
            FailShaderJob(result, ShaderWorkerJobStatus::ToolError,
                          "SpawnChild needs an executable");
            return false;
        }

        Array<platform::ProcessArgument> arguments(allocator_);
        for (usize index = 1; index < job.hookArguments.Size(); ++index) {
            const String& argument = job.hookArguments[index];
            if (!arguments.TryPushBack({argument.Data(), argument.ByteCount()})) {
                return false;
            }
        }

        platform::ProcessStartInfo info;
        info.executable = job.hookArguments[0].Data();
        info.executableBytes = job.hookArguments[0].ByteCount();
        info.arguments = arguments.Data();
        info.argumentCount = arguments.Size();
        info.input = platform::ProcessStreamMode::Null;
        info.output = platform::ProcessStreamMode::Null;
        info.error = platform::ProcessStreamMode::Null;
        auto child = children_.TryEmplaceBack();
        if (!child || !child.Value()->Start(info)) {
            FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "cannot start the child probe");
            return false;
        }

        return Reply("child", child.Value()->Id());
    }

    const ShaderWorkerArguments& arguments_;
    ShaderWorkerControl& control_;
    Allocator& allocator_;
    platform::File log_;
    Array<platform::Process> children_;
    Array<ShaderFormatDefinition> formats_;
    ShaderToolSet tools_;
    ShaderToolLoadStatus toolStatus_;
    bool toolsTried_ = false;
    D3DShaderFormatBackend d3d_;
    VulkanShaderFormatBackend vulkan_;
    // Crash hook이 쓰는 주소(null)다.
    int32* crashTarget_ = nullptr;
};

} // namespace shader_worker_detail

ShaderWorkerControl& StandardShaderWorkerControl() noexcept {
    static shader_worker_detail::StandardControl control;
    return control;
}

bool ParseShaderWorkerArguments(Span<const StringView> arguments,
                                ShaderWorkerArguments& out) noexcept {
    using namespace shader_worker_detail;
    out = ShaderWorkerArguments{};
    bool hasDirectory = false;
    bool hasParent = false;
    bool hasWorker = false;
    for (usize index = 0; index < arguments.Size(); ++index) {
        const StringView argument = arguments[index];
        const bool hasValue = index + 1 < arguments.Size();
        uint64 number = 0;
        if (argument == WorkDirectoryOption && hasValue && !hasDirectory &&
            !arguments[index + 1].IsEmpty() &&
            arguments[index + 1].ByteCount() < MaxShaderWorkerPathBytes) {
            out.workDirectory = arguments[++index];
            hasDirectory = true;
        } else if (argument == ParentOption && hasValue && !hasParent &&
                   ParseUnsigned(arguments[index + 1], NumericLimits<uint32>::Max(), number)) {
            out.parentProcessId = static_cast<uint32>(number);
            hasParent = true;
            ++index;
        } else if (argument == WorkerOption && hasValue && !hasWorker &&
                   ParseUnsigned(arguments[index + 1], NumericLimits<uint32>::Max(), number)) {
            out.worker = static_cast<uint32>(number);
            hasWorker = true;
            ++index;
        } else if (argument == KeepInputOption && !out.keepInput) {
            out.keepInput = true;
        } else if (argument == TestHooksOption && !out.testHooks) {
            out.testHooks = true;
        } else {
            return false;
        }
    }

    return hasDirectory && hasParent && hasWorker;
}

int32 RunShaderWorker(const ShaderWorkerArguments& arguments, ShaderWorkerControl& control,
                      Allocator& allocator) noexcept {
    shader_worker_detail::Session session(arguments, control, allocator);
    return session.Run();
}

} // namespace rocks::shader
