#include <ShaderCompiler/ShaderCompileManager.h>

#include <AssetNugget/NuggetRecord.h>
#include <AssetNugget/NuggetRequest.h>
#include <Base/PlacementNew.h>
#include <Core/Encoding/Sha256.h>
#include <Platform/Atomics.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <Platform/Thread.h>
#include <ShaderCompiler/ShaderPreprocessDefines.h>
#include <ShaderCore/ShaderType.h>
#include <ShaderCompiler/ShaderWorkerChannel.h>
#include <stdio.h>
#include <string.h>

// 셰이더 컴파일 관리자(Docs/Design/ShaderSystem 10절, 11.1절 SC4·SC11·SC12)다. 규칙은 공개 헤더
// 주석을 따른다.
namespace rocks::shader {

namespace compile_manager_detail {

// 경로 버퍼 크기다.
constexpr usize MaxManagerPathBytes = 4096;
// u32 10진 자릿수 상한이다.
constexpr usize MaxPidDigits = 10;

using platform::AtomicFetchAdd;
using platform::AtomicFetchSub;
using platform::AtomicLoad;

bool AppendText(String& text, StringView part) noexcept {
    return static_cast<bool>(text.TryAppend(part));
}

// 다른 PID 작업 디렉터리 가운데 지울 이름을 모은다.
struct StaleScan {
    Allocator* allocator = nullptr;
    uint32 self = 0;
    Array<String>* names = nullptr;
    bool failed = false;
};

bool CollectStale(const platform::DirectoryEntry& entry, void* context) noexcept {
    auto& scan = *static_cast<StaleScan*>(context);
    if (entry.info.type != platform::FileType::Directory || entry.nameBytes == 0 ||
        entry.nameBytes > MaxPidDigits) {
        return true;
    }

    uint64 pid = 0;
    for (usize index = 0; index < entry.nameBytes; ++index) {
        const char digit = entry.name[index];
        if (digit < '0' || digit > '9') {
            return true;
        }

        pid = pid * 10 + static_cast<uint64>(digit - '0');
    }

    // 자기 PID는 따로 비우고, 살아 있어 보이는 PID(재사용 포함)는 남긴다(보수적).
    if (pid == scan.self || pid > NumericLimits<uint32>::Max() ||
        platform::IsProcessRunning(static_cast<uint32>(pid))) {
        return true;
    }

    auto name = scan.names->TryEmplaceBack(*scan.allocator);
    scan.failed =
        scan.failed || !name || !name.Value()->TryAssign(StringView(entry.name, entry.nameBytes));
    return true;
}

ShaderCompileStatus StatusOf(ShaderWorkerJobStatus status) noexcept {
    switch (status) {
    case ShaderWorkerJobStatus::Success:
        return ShaderCompileStatus::Success;
    case ShaderWorkerJobStatus::CompileError:
        return ShaderCompileStatus::CompileError;
    case ShaderWorkerJobStatus::ToolError:
        return ShaderCompileStatus::ToolError;
    case ShaderWorkerJobStatus::UnsupportedCapability:
        return ShaderCompileStatus::UnsupportedCapability;
    }

    return ShaderCompileStatus::ToolError;
}

bool HasSystemTool(const ShaderFormatDefinition& format) noexcept {
    for (usize index = 0; index < format.toolCount; ++index) {
        if (format.tools[index] != nullptr &&
            format.tools[index]->origin == ShaderToolOrigin::System) {
            return true;
        }
    }

    return false;
}

void ProvisionalJob(void* context, uint32) noexcept;

} // namespace compile_manager_detail

struct ShaderCompileManager::State {
    explicit State(Allocator& allocator) noexcept
        : allocator(allocator), channels(allocator), systemNames(allocator),
          systemVersions(allocator), systemTools(allocator) {
    }

    Allocator& allocator;
    Array<ShaderWorkerChannel*> channels;
    usize nextChannel = 0;
    // 도구 조회 결과(system 도구 지문 — 세션에 한 번)다.
    bool queried = false;
    Array<String> systemNames;
    Array<String> systemVersions;
    Array<ShaderSystemToolFingerprint> systemTools;
    // 계수(원자)다.
    uint64 total = 0;
    uint64 completed = 0;
    uint64 failed = 0;
    uint64 cacheHits = 0;
    uint64 provisional = 0;
    uint64 batches = 0;
    uint64 conflicts = 0;
    uint64 corruptHits = 0;
};

namespace compile_manager_detail {

void ProvisionalJob(void* context, uint32) noexcept {
    (void)AtomicFetchAdd(&static_cast<ShaderCompileManager::State*>(context)->provisional, 1u);
}

// 묶음 하나다(요청 byte·결과·ticket·job 색인).
struct Batch {
    explicit Batch(Allocator& allocator) noexcept
        : request(allocator), result(allocator), jobs(allocator) {
    }

    Array<uint8> request;
    ShaderWorkerBatchResult result;
    ShaderWorkerTicket ticket;
    Array<usize> jobs;
};

} // namespace compile_manager_detail

const char* ShaderCompileStatusName(ShaderCompileStatus status) noexcept {
    switch (status) {
    case ShaderCompileStatus::Success:
        return "Success";
    case ShaderCompileStatus::InvalidJob:
        return "InvalidJob";
    case ShaderCompileStatus::PreprocessError:
        return "PreprocessError";
    case ShaderCompileStatus::CompileError:
        return "CompileError";
    case ShaderCompileStatus::ToolError:
        return "ToolError";
    case ShaderCompileStatus::UnsupportedCapability:
        return "UnsupportedCapability";
    case ShaderCompileStatus::WorkerFailed:
        return "WorkerFailed";
    case ShaderCompileStatus::CacheFailed:
        return "CacheFailed";
    case ShaderCompileStatus::Cancelled:
        return "Cancelled";
    }

    return "unknown";
}

ShaderFormatStatus LoadShaderFormatSnapshot(const char* directory, StringView id,
                                            ShaderFormatSnapshot& out) noexcept {
    using namespace compile_manager_detail;
    ShaderFormatStatus status;
    char path[MaxManagerPathBytes];
    const int written = snprintf(path, sizeof(path), "%s/%.*s.json", directory,
                                 static_cast<int>(id.ByteCount()), id.Data());
    platform::File file;
    uint64 size = 0;
    if (written <= 0 || static_cast<usize>(written) >= sizeof(path) ||
        !file.Open(path, platform::FileOpenMode::ReadExisting) || !file.Size(size) ||
        !out.bytes.TryResize(static_cast<usize>(size))) {
        status.code = ShaderFormatErrorCode::ReadFailed;
        snprintf(status.path, sizeof(status.path), "%.*s", static_cast<int>(id.ByteCount()),
                 id.Data());
        return status;
    }

    const platform::FileTransfer read = file.ReadExact(out.bytes.Data(), out.bytes.Size());
    if (!file.Close() || !read.status) {
        status.code = ShaderFormatErrorCode::ReadFailed;
        snprintf(status.path, sizeof(status.path), "%.*s", static_cast<int>(id.ByteCount()),
                 id.Data());
        return status;
    }

    return ParseShaderFormatDefinition(out.bytes.View(), id, out.definition);
}

ShaderNuggetAction ClassifyShaderNuggetGet(const NuggetCacheGetResult& result,
                                           bool required) noexcept {
    if (result.status == NuggetStatus::Cancelled) {
        return ShaderNuggetAction::Cancel;
    }

    if (result.failed && required) {
        return ShaderNuggetAction::Fail;
    }

    if (result.status == NuggetStatus::Hit) {
        for (const NuggetValue& value : result.values) {
            if (value.id == static_cast<uint32>(ShaderNuggetValue::Code)) {
                return ShaderNuggetAction::UseHit;
            }
        }
    }

    return ShaderNuggetAction::Recompile;
}

ShaderCompileManager::ShaderCompileManager(const ShaderCompileManagerConfig& config) noexcept
    : config_(config) {
}

ShaderCompileManager::~ShaderCompileManager() noexcept {
    Shutdown();
}

Result<void> ShaderCompileManager::Start() noexcept {
    using namespace compile_manager_detail;
    if (state_ != nullptr) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    if (config_.allocator == nullptr || config_.tree == nullptr ||
        config_.reflection.types == nullptr || config_.workerExecutable.IsEmpty() ||
        config_.workRoot.IsEmpty() || config_.maxBatchJobs == 0 ||
        config_.maxBatchSourceBytes == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = *config_.allocator;
    platform::FileSystem& files = platform::PhysicalFileSystem();
    // 작업 디렉터리: 자기 PID를 비우고 죽은 PID를 지운다(SC4).
    const uint32 self = platform::CurrentProcessId();
    char own[MaxManagerPathBytes];
    const int ownBytes =
        snprintf(own, sizeof(own), "%.*s/%u", static_cast<int>(config_.workRoot.ByteCount()),
                 config_.workRoot.Data(), self);
    if (ownBytes <= 0 || static_cast<usize>(ownBytes) >= sizeof(own) ||
        !platform::MakeDirectoryTree(files, config_.workRoot.Data(),
                                     config_.workRoot.ByteCount())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Array<String> stale(allocator);
    StaleScan scan{&allocator, self, &stale, false};
    if (!files.EnumerateDirectory(config_.workRoot.Data(), config_.workRoot.ByteCount(),
                                  &CollectStale, &scan) ||
        scan.failed) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    for (const String& name : stale) {
        char path[MaxManagerPathBytes];
        const int bytes = snprintf(
            path, sizeof(path), "%.*s/%.*s", static_cast<int>(config_.workRoot.ByteCount()),
            config_.workRoot.Data(), static_cast<int>(name.ByteCount()), name.Data());
        if (bytes > 0 && static_cast<usize>(bytes) < sizeof(path)) {
            (void)platform::DeleteDirectoryTree(files, path, static_cast<usize>(bytes));
        }
    }

    bool exists = false;
    if (files.DirectoryExists(own, static_cast<usize>(ownBytes), exists) && exists &&
        !platform::DeleteDirectoryTree(files, own, static_cast<usize>(ownBytes))) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    auto memory = allocator.Allocate(sizeof(State), alignof(State));
    if (!memory) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    State* state = ::new (memory.Value()) State(allocator);
    state_ = state;
    uint32 count = config_.workerCount;
    if (count == 0) {
        const uint32 logical = platform::LogicalProcessorCount();
        count = logical > 1 ? logical - 1 : 1;
        count = count < MaxDefaultShaderWorkers ? count : MaxDefaultShaderWorkers;
    }

    for (uint32 index = 0; index < count; ++index) {
        char directory[MaxManagerPathBytes];
        const int bytes = snprintf(directory, sizeof(directory), "%s/%u", own, index);
        auto channelMemory =
            allocator.Allocate(sizeof(ShaderWorkerChannel), alignof(ShaderWorkerChannel));
        if (!channelMemory) {
            Shutdown();
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        auto* channel = ::new (channelMemory.Value()) ShaderWorkerChannel(allocator);
        if (!state->channels.TryPushBack(channel)) {
            channel->~ShaderWorkerChannel();
            (void)allocator.Deallocate(channel);
            Shutdown();
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        ShaderWorkerLaunch launch;
        launch.executable = config_.workerExecutable;
        launch.workDirectory = StringView(directory, static_cast<usize>(bytes));
        launch.worker = index;
        launch.keepInput = config_.keepInput;
        launch.testHooks = config_.testHooks;
        if (auto started = channel->Start(launch); !started) {
            Shutdown();
            return started;
        }
    }

    return Result<void>::Success();
}

void ShaderCompileManager::Shutdown() noexcept {
    if (state_ == nullptr) {
        return;
    }

    Allocator& allocator = state_->allocator;
    for (ShaderWorkerChannel* channel : state_->channels) {
        channel->~ShaderWorkerChannel();
        (void)allocator.Deallocate(channel);
    }

    state_->~State();
    (void)allocator.Deallocate(state_);
    state_ = nullptr;
}

ShaderCompileProgress ShaderCompileManager::Progress() const noexcept {
    using namespace compile_manager_detail;
    ShaderCompileProgress progress;
    if (state_ == nullptr) {
        return progress;
    }

    progress.total = AtomicLoad(&state_->total);
    progress.completed = AtomicLoad(&state_->completed);
    progress.failed = AtomicLoad(&state_->failed);
    progress.cacheHits = AtomicLoad(&state_->cacheHits);
    progress.provisional = AtomicLoad(&state_->provisional);
    progress.batches = AtomicLoad(&state_->batches);
    progress.conflicts = AtomicLoad(&state_->conflicts);
    progress.corruptHits = AtomicLoad(&state_->corruptHits);
    for (const ShaderWorkerChannel* channel : state_->channels) {
        progress.workersStarted += channel->WorkersStarted();
    }

    return progress;
}

namespace compile_manager_detail {

// 한 Compile 호출의 진행이다.
class Session final {
public:
    Session(ShaderCompileManager::State& state, const ShaderCompileManagerConfig& config,
            Span<const ShaderCompileJob> jobs, Array<ShaderCompileJobResult>& out,
            bool asynchronous) noexcept
        : state_(state), config_(config), jobs_(jobs), out_(out), asynchronous_(asynchronous),
          pending_(state.allocator), batches_(state.allocator) {
    }

    ~Session() noexcept {
        for (Batch* batch : batches_) {
            Destroy(batch);
        }
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Result<void> Run() noexcept {
        Allocator& allocator = state_.allocator;
        out_.Clear();
        for (usize index = 0; index < jobs_.Size(); ++index) {
            if (!out_.TryEmplaceBack(allocator) || !counted_.TryPushBack(Uncounted)) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        }

        (void)AtomicFetchAdd(&state_.total, static_cast<uint64>(jobs_.Size()));
        for (usize index = 0; index < jobs_.Size(); ++index) {
            Prepare(index);
        }

        if (auto looked = LookUp(); !looked) {
            return looked;
        }

        if (auto compiled = CompileMisses(); !compiled) {
            return compiled;
        }

        Publish();
        // 남은 job(없어야 한다)도 계수에 넣는다.
        for (usize index = 0; index < out_.Size(); ++index) {
            Count(index);
        }

        return Result<void>::Success();
    }

private:
    // job별 계수 상태다.
    static constexpr uint8 Uncounted = 0;
    static constexpr uint8 CountedCompleted = 1;
    static constexpr uint8 CountedFailed = 2;

    // job 하나의 최종 상태를 계수에 넣는다(진행 보고 12절 — 끝난 즉시). 성공으로 센 job이 뒤에
    // 실패하면(required 캐시 게시 실패) 완료에서 실패로 옮긴다.
    void Count(usize index) noexcept {
        const bool success = out_[index].status == ShaderCompileStatus::Success;
        if (counted_[index] == Uncounted) {
            counted_[index] = success ? CountedCompleted : CountedFailed;
            (void)AtomicFetchAdd(success ? &state_.completed : &state_.failed, 1u);
        } else if (counted_[index] == CountedCompleted && !success) {
            counted_[index] = CountedFailed;
            (void)AtomicFetchSub(&state_.completed, 1u);
            (void)AtomicFetchAdd(&state_.failed, 1u);
        }
    }

    // job을 실패로 끝낸다(진단을 바꾸고 바로 센다).
    void Fail(usize index, ShaderCompileStatus status, StringView message,
              StringView detail = StringView()) noexcept {
        ShaderCompileJobResult& result = out_[index];
        result.status = status;
        result.values.Reset();
        result.diagnostics.Clear();
        (void)(AppendText(result.diagnostics, message) &&
               (detail.IsEmpty() ||
                (AppendText(result.diagnostics, ": ") && AppendText(result.diagnostics, detail))));
        Count(index);
    }

    // 정의 조립·전처리·키다.
    void Prepare(usize index) noexcept {
        const ShaderCompileJob& job = jobs_[index];
        ShaderCompileJobResult& result = out_[index];
        if (job.format == nullptr || job.entryPoint.IsEmpty()) {
            Fail(index, ShaderCompileStatus::InvalidJob, "shader job has no format or entry point");
            return;
        }

        if (job.testHook != ShaderWorkerJobKind::Compile && !config_.testHooks) {
            Fail(index, ShaderCompileStatus::InvalidJob, "test hook jobs need a test hook manager");
            return;
        }

        ShaderDefineSet defines(state_.allocator);
        const ShaderSourceStatus built =
            BuildShaderPreprocessDefines(job.format->definition, job.stage, job.options.hlsl2021,
                                         job.defines, job.developmentOnly, defines);
        if (!built) {
            Fail(index, ShaderCompileStatus::InvalidJob, StringView(built.message));
            return;
        }

        // 정점 입력 연결 하나와 레이아웃 표의 생성 원문(16.2절 SSD52 — 표마다 하나, 중첩 구조체
        // 정의는 바깥 원문 안에 있다)이다.
        ShaderGeneratedInclude includes[MaxShaderParameterLayouts + 1];
        usize includeCount = 0;
        if (!job.vertexInputSource.IsEmpty()) {
            includes[includeCount++] = ShaderGeneratedInclude{
                StringView(VertexInputGeneratedIncludePath), job.vertexInputSource, StringView()};
        }

        for (const ShaderParameterLayout& layout : job.parameterLayouts) {
            if (!layout.source.IsEmpty() && includeCount < MaxShaderParameterLayouts + 1) {
                includes[includeCount++] =
                    ShaderGeneratedInclude{layout.sourcePath, StringView(), layout.source};
            }
        }

        const Span<const ShaderGeneratedInclude> generated(includes, includeCount);
        const ShaderSourceStatus preprocessed =
            PreprocessShader(*config_.tree, job.entryPath, defines.Defines(), result.preprocessed,
                             ShaderPreprocessLimits{}, generated);
        if (!preprocessed) {
            char location[MaxShaderVirtualPathBytes + 64];
            snprintf(location, sizeof(location), "%s:%u: %s", preprocessed.path, preprocessed.line,
                     ShaderSourceErrorName(preprocessed.code));
            Fail(index, ShaderCompileStatus::PreprocessError, StringView(location),
                 StringView(preprocessed.message));
            return;
        }

        const char* layoutReason = nullptr;
        if (!ValidateShaderParameterLayouts(job.parameterLayouts, &layoutReason)) {
            Fail(index, ShaderCompileStatus::InvalidJob, "shader parameter layout is invalid",
                 StringView(layoutReason));
            return;
        }

        if (HasSystemTool(job.format->definition) && !QueryTools()) {
            Fail(index, ShaderCompileStatus::ToolError, "shader tool query failed");
            return;
        }

        ShaderJobKeyInput input;
        input.projectGeneration =
            config_.cache != nullptr ? config_.cache->ProjectGeneration() : Guid{};
        input.format = &job.format->definition;
        input.systemTools = state_.systemTools.View();
        input.stage = job.stage;
        input.entry = job.entryPoint;
        input.requiredFeatures = job.requiredFeatures;
        input.preprocessedSha256 = Sha256::Hash(
            Span<const uint8>(reinterpret_cast<const uint8*>(result.preprocessed.text.Data()),
                              result.preprocessed.text.ByteCount()));
        input.options = &job.options;
        input.reflection = config_.reflection;
        const Sha256Digest layoutSignature = ShaderParameterLayoutSignature(job.parameterLayouts);
        input.layoutSignature = job.parameterLayouts.IsEmpty() ? nullptr : &layoutSignature;
        auto key = BuildShaderJobKey(input);
        if (!key) {
            Fail(index,
                 key.Error() == ErrorCode::UnsupportedCapability
                     ? ShaderCompileStatus::UnsupportedCapability
                     : ShaderCompileStatus::InvalidJob,
                 key.Error() == ErrorCode::UnsupportedCapability
                     ? StringView("this host cannot produce the shader format")
                     : StringView("shader job key inputs are invalid"),
                 job.format->definition.Id());
            return;
        }

        result.key = key.Value();
        result.hasKey = true;
        result.status = ShaderCompileStatus::Success;
    }

    // system 도구 지문을 워커 하나에 묻는다(세션에 한 번 — SC11).
    bool QueryTools() noexcept {
        if (state_.queried) {
            return true;
        }

        ShaderWorkerRequest request(state_.allocator);
        request.kind = ShaderWorkerRequestKind::ToolQuery;
        Array<uint8> bytes(state_.allocator);
        ShaderWorkerBatchResult result(state_.allocator);
        if (!MakeShaderWorkerHeader({}, request.header) ||
            !EncodeShaderWorkerRequest(request, bytes) || state_.channels.IsEmpty()) {
            return false;
        }

        state_.channels[0]->Run(bytes.View(), result);
        if (result.outcome != ShaderWorkerBatchOutcome::Completed) {
            return false;
        }

        for (const ShaderWorkerSystemTool& tool : result.response.systemTools) {
            if (!tool.found) {
                continue;
            }

            auto name = state_.systemNames.TryEmplaceBack(state_.allocator);
            auto version = state_.systemVersions.TryEmplaceBack(state_.allocator);
            if (!name || !version || !name.Value()->TryAssign(tool.name.View()) ||
                !version.Value()->TryAssign(tool.version.View())) {
                return false;
            }
        }

        // 문자열이 다 자란 뒤 view를 만든다.
        usize found = 0;
        for (const ShaderWorkerSystemTool& tool : result.response.systemTools) {
            if (!tool.found) {
                continue;
            }

            ShaderSystemToolFingerprint fingerprint;
            fingerprint.name = state_.systemNames[found].View();
            fingerprint.version = state_.systemVersions[found].View();
            for (usize index = 0; index < ShaderSystemToolExecutables; ++index) {
                fingerprint.executables[index] = tool.executables[index];
            }

            if (!state_.systemTools.TryPushBack(fingerprint)) {
                return false;
            }

            ++found;
        }

        state_.queried = true;
        return true;
    }

    void UseHit(usize index, const NuggetCacheGetResult& got) noexcept {
        Array<NuggetValue> values(state_.allocator);
        for (const NuggetValue& value : got.values) {
            auto copy = values.TryEmplaceBack(state_.allocator);
            if (!copy || !copy.Value()->bytes.TryAppend(value.bytes.View())) {
                Fail(index, ShaderCompileStatus::CacheFailed, "out of memory");
                return;
            }

            copy.Value()->id = value.id;
        }

        ShaderCompileJobResult& result = out_[index];
        if (!MoveShaderValuesFromNugget(values, result.values)) {
            (void)AtomicFetchAdd(&state_.corruptHits, 1u);
            (void)pending_.TryPushBack(index);
            return;
        }

        result.cacheHit = true;
        (void)AtomicFetchAdd(&state_.cacheHits, 1u);
        Count(index);
    }

    void Classify(usize index, const NuggetCacheGetResult& got) noexcept {
        const bool required = config_.cache->IsRequired();
        switch (ClassifyShaderNuggetGet(got, required)) {
        case ShaderNuggetAction::UseHit:
            UseHit(index, got);
            return;
        case ShaderNuggetAction::Cancel:
            Fail(index, ShaderCompileStatus::Cancelled, "the nugget cache was shut down");
            return;
        case ShaderNuggetAction::Fail:
            Fail(index, ShaderCompileStatus::CacheFailed, "the required nugget cache failed");
            return;
        case ShaderNuggetAction::Recompile:
            break;
        }

        if (got.status == NuggetStatus::Hit || got.status == NuggetStatus::Corrupt) {
            (void)AtomicFetchAdd(&state_.corruptHits, 1u);
        }

        if (got.failed) {
            (void)AppendText(out_[index].diagnostics,
                             "nugget cache unavailable; compiled without it\n");
        }

        (void)pending_.TryPushBack(index);
    }

    // 너겟 일괄 조회다(값 선택 없이 Record 전체 — 10절).
    Result<void> LookUp() noexcept {
        Array<usize> keyed(state_.allocator);
        for (usize index = 0; index < out_.Size(); ++index) {
            if (out_[index].status == ShaderCompileStatus::Success && out_[index].hasKey &&
                !keyed.TryPushBack(index)) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        }

        if (config_.cache == nullptr) {
            return pending_.TryAppend(keyed.View());
        }

        if (!asynchronous_ || config_.scheduler == nullptr) {
            for (const usize index : keyed) {
                NuggetCacheGet get;
                get.bucket = NuggetBucketShader;
                get.key = out_[index].key;
                Classify(index, config_.cache->Get(get));
            }

            return Result<void>::Success();
        }

        Array<NuggetGetItem> items(state_.allocator);
        for (const usize index : keyed) {
            NuggetGetItem item;
            item.bucket = NuggetBucketShader;
            item.key = out_[index].key;
            if (!items.TryPushBack(item)) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        }

        NuggetGetRequest request(state_.allocator);
        if (auto started = request.Start(*config_.cache, items.View(), *config_.scheduler);
            !started) {
            return started;
        }

        (void)request.Handle().Wait();
        auto results = request.Results();
        if (!results) {
            return Result<void>::Failure(results.Error());
        }

        for (usize item = 0; item < keyed.Size(); ++item) {
            Classify(keyed[item], results.Value()[item]);
        }

        return Result<void>::Success();
    }

    // 미적중을 묶음으로 나눠 채널에 맡기고 기다린다.
    Result<void> CompileMisses() noexcept {
        Allocator& allocator = state_.allocator;
        usize cursor = 0;
        while (cursor < pending_.Size()) {
            auto memory = allocator.Allocate(sizeof(Batch), alignof(Batch));
            if (!memory) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            Batch* batch = ::new (memory.Value()) Batch(allocator);
            if (!batches_.TryPushBack(batch)) {
                Destroy(batch);
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            usize sourceBytes = 0;
            while (cursor < pending_.Size() && batch->jobs.Size() < config_.maxBatchJobs) {
                const usize index = pending_[cursor];
                const usize bytes = out_[index].preprocessed.text.ByteCount();
                if (!batch->jobs.IsEmpty() && sourceBytes + bytes > config_.maxBatchSourceBytes) {
                    break;
                }

                sourceBytes += bytes;
                if (!batch->jobs.TryPushBack(index)) {
                    return Result<void>::Failure(ErrorCode::OutOfMemory);
                }

                ++cursor;
            }
        }

        // 요청을 모두 만든 뒤에 맡긴다(맡긴 묶음은 끝나기 전에 버리지 않는다).
        for (Batch* batch : batches_) {
            if (auto built = BuildRequest(*batch); !built) {
                return built;
            }
        }

        // 채널에 돌아가며 맡긴다.
        for (Batch* batch : batches_) {
            batch->ticket.request = batch->request.View();
            batch->ticket.out = &batch->result;
            batch->ticket.progress.job = &ProvisionalJob;
            batch->ticket.progress.context = &state_;
            ShaderWorkerChannel& channel =
                *state_.channels[state_.nextChannel++ % state_.channels.Size()];
            (void)AtomicFetchAdd(&state_.batches, 1u);
            channel.Post(batch->ticket);
        }

        for (Batch* batch : batches_) {
            ShaderWorkerChannel::Wait(batch->ticket);
            // 잠정 값을 실제 결과로 바꾼다(진행 보고 10.3절).
            (void)AtomicFetchSub(&state_.provisional,
                                 static_cast<uint64>(batch->result.reportedJobs));
            Collect(*batch);
        }

        return Result<void>::Success();
    }

    void Destroy(Batch* batch) noexcept {
        batch->~Batch();
        (void)state_.allocator.Deallocate(batch);
    }

    Result<void> BuildRequest(Batch& batch) noexcept {
        Allocator& allocator = state_.allocator;
        ShaderWorkerRequest request(allocator);
        request.kind = ShaderWorkerRequestKind::Compile;
        Array<const ShaderFormatDefinition*> formats(allocator);
        for (const usize index : batch.jobs) {
            const ShaderCompileJob& job = jobs_[index];
            bool known = false;
            for (const ShaderFormatDefinition* format : formats) {
                known = known || format == &job.format->definition;
            }

            if (!known) {
                auto format = request.formats.TryEmplaceBack(allocator);
                if (!formats.TryPushBack(&job.format->definition) || !format ||
                    !format.Value()->id.TryAssign(job.format->definition.Id()) ||
                    !format.Value()->bytes.TryAppend(job.format->bytes.View())) {
                    return Result<void>::Failure(ErrorCode::OutOfMemory);
                }
            }

            auto item = request.jobs.TryEmplaceBack(allocator);
            if (!item) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            ShaderWorkerJob& worker = *item.Value();
            worker.kind = job.testHook;
            worker.key = out_[index].key;
            worker.stage = job.stage;
            worker.requiredFeatures = job.requiredFeatures;
            worker.options.hlsl2021 = job.options.hlsl2021;
            worker.options.optimization = static_cast<uint8>(job.options.optimization);
            worker.options.debugSymbols = job.options.debugSymbols;
            worker.options.matrixLayout = static_cast<uint8>(job.options.matrixLayout);
            if (!worker.formatId.TryAssign(job.format->definition.Id()) ||
                !worker.entry.TryAssign(job.entryPoint) ||
                !worker.source.TryAssign(out_[index].preprocessed.text.View())) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            // 레이아웃 표 정규 byte다(Prepare가 검사했다 — SSD31).
            if (!job.parameterLayouts.IsEmpty()) {
                if (auto encoded =
                        EncodeShaderParameterLayouts(job.parameterLayouts, worker.layout);
                    !encoded) {
                    return encoded;
                }
            }
        }

        if (auto header = MakeShaderWorkerHeader(formats.View(), request.header); !header) {
            return header;
        }

        return EncodeShaderWorkerRequest(request, batch.request);
    }

    void Collect(Batch& batch) noexcept {
        const ShaderWorkerBatchResult& result = batch.result;
        if (result.outcome != ShaderWorkerBatchOutcome::Completed ||
            result.response.jobs.Size() != batch.jobs.Size()) {
            char detail[96];
            snprintf(detail, sizeof(detail), "%s, exit kind %u code %u",
                     ShaderWorkerBatchOutcomeName(result.outcome),
                     static_cast<unsigned>(result.exit.kind), result.exit.code);
            for (const usize index : batch.jobs) {
                Fail(index, ShaderCompileStatus::WorkerFailed, result.diagnostics.View(),
                     StringView(detail));
            }

            return;
        }

        for (usize item = 0; item < batch.jobs.Size(); ++item) {
            const usize index = batch.jobs[item];
            const ShaderWorkerJobResult& job = result.response.jobs[item];
            if (job.status != ShaderWorkerJobStatus::Success) {
                Fail(index, StatusOf(job.status), job.diagnostics.View());
                continue;
            }

            ShaderCompileJobResult& out = out_[index];
            out.values.Reset();
            bool hasCode = false;
            for (const ShaderWorkerValue& value : job.values) {
                Array<uint8> bytes(state_.allocator);
                if (value.id >= ShaderNuggetValueCount || !bytes.TryAppend(value.bytes.View())) {
                    hasCode = false;
                    break;
                }

                hasCode = hasCode || value.id == static_cast<uint32>(ShaderNuggetValue::Code);
                out.values.Set(static_cast<ShaderNuggetValue>(value.id), Move(bytes));
            }

            if (!hasCode) {
                Fail(index, ShaderCompileStatus::ToolError, "the worker result has no code value");
                continue;
            }

            if (!publish_.TryPushBack(index)) {
                Fail(index, ShaderCompileStatus::ToolError, "out of memory");
                continue;
            }

            Count(index);
        }
    }

    // 새로 만든 결과를 게시한다(값은 결과에 남기고 사본을 넘긴다 — Put이 값을 옮겨 간다).
    void Publish() noexcept {
        if (config_.cache == nullptr || publish_.IsEmpty()) {
            return;
        }

        Allocator& allocator = state_.allocator;
        Array<NuggetPutItem> items(allocator);
        for (const usize index : publish_) {
            auto item = items.TryEmplaceBack(allocator);
            if (!item) {
                return;
            }

            item.Value()->bucket = NuggetBucketShader;
            item.Value()->key = out_[index].key;
            ShaderJobValues copy(allocator);
            const ShaderJobValues& values = out_[index].values;
            for (uint32 id = 0; id < ShaderNuggetValueCount; ++id) {
                const auto value = static_cast<ShaderNuggetValue>(id);
                if (!values.Has(value)) {
                    continue;
                }

                Array<uint8> bytes(allocator);
                if (!bytes.TryAppend(values.Part(value).View())) {
                    return;
                }

                copy.Set(value, Move(bytes));
            }

            if (!MoveShaderValuesToNugget(copy, item.Value()->values)) {
                return;
            }
        }

        if (asynchronous_ && config_.scheduler != nullptr) {
            NuggetPutRequest request(allocator);
            if (!request.Start(*config_.cache, items.View(), *config_.scheduler)) {
                return;
            }

            (void)request.Handle().Wait();
            auto results = request.Results();
            if (!results) {
                return;
            }

            for (usize item = 0; item < publish_.Size(); ++item) {
                Published(publish_[item], results.Value()[item]);
            }

            return;
        }

        for (usize item = 0; item < publish_.Size(); ++item) {
            NuggetCachePut put;
            put.bucket = NuggetBucketShader;
            put.key = items[item].key;
            put.values = items[item].values.View();
            Published(publish_[item], config_.cache->Put(put));
        }
    }

    void Published(usize index, const NuggetCachePutResult& result) noexcept {
        if (result.status == NuggetStatus::Conflict) {
            // 결정성 위반이다: 통계와 진단을 남기고 이번 결과를 쓴다(10절).
            (void)AtomicFetchAdd(&state_.conflicts, 1u);
            (void)AppendText(out_[index].diagnostics,
                             "nugget conflict: the same key produced different shader bytes\n");
        }

        if (result.failed && config_.cache->IsRequired()) {
            Fail(index, ShaderCompileStatus::CacheFailed,
                 "the required nugget cache failed to store");
        }
    }

    ShaderCompileManager::State& state_;
    const ShaderCompileManagerConfig& config_;
    Span<const ShaderCompileJob> jobs_;
    Array<ShaderCompileJobResult>& out_;
    bool asynchronous_;
    Array<usize> pending_;
    Array<Batch*> batches_;
    Array<usize> publish_{state_.allocator};
    Array<uint8> counted_{state_.allocator};
};

} // namespace compile_manager_detail

Result<void> ShaderCompileManager::Compile(Span<const ShaderCompileJob> jobs,
                                           Array<ShaderCompileJobResult>& out) noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    compile_manager_detail::Session session(*state_, config_, jobs, out, true);
    return session.Run();
}

Result<void> ShaderCompileManager::CompileNow(const ShaderCompileJob& job,
                                              ShaderCompileJobResult& out) noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    Array<ShaderCompileJobResult> results(state_->allocator);
    compile_manager_detail::Session session(*state_, config_, Span<const ShaderCompileJob>(&job, 1),
                                            results, false);
    if (auto ran = session.Run(); !ran) {
        return ran;
    }

    out = Move(results[0]);
    return Result<void>::Success();
}

} // namespace rocks::shader
