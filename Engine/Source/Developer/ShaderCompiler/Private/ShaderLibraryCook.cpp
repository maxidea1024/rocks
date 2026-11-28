#include <ShaderCompiler/ShaderLibraryCook.h>

#include <Core/Algorithms.h>
#include <Core/Diagnostics/Log.h>
#include <Core/FileError.h>
#include <Core/FileRead.h>
#include <Platform/Process.h>
#include <ShaderCompilerCommon/ShaderDebugSymbols.h>
#include <ShaderCore/ShaderParameterLayout.h>
#include <ShaderCore/ShaderPermutation.h>
#include <stdio.h>
#include <string.h>

// 셰이더 라이브러리 쿠킹(Docs/Design/ShaderSystem 12.2.1절 SSD28)이다. 규칙은 공개 헤더 주석을
// 따른다.
namespace rocks::shader {

namespace library_cook_detail {

constexpr LogCategory LogShaderLibraryBuild{"ShaderCompiler"};
// 정의 값 문자열 byte 수(NUL 포함 — int32 최솟값 "-2147483648"보다 넉넉하다)다.
constexpr usize DefineValueBytes = 16;
// 파일 이름에 넣는 파일 SHA-256 hex 자리 수다.
constexpr usize FileShaPrefixHex = 16;
// 임시 파일 접미 버퍼 byte 수(".<pid>.tmp" NUL 포함)다.
constexpr usize TemporarySuffixBytes = 32;
// 묶음 id 16 hex와 NUL의 byte 수다.
constexpr usize BundleIdHexBytes = 2 * sizeof(uint64) + 1;

// job 하나가 가리키는 정의 저장소다. 자기 배열을 가리키므로 만든 뒤 옮기지 않는다(Array를 미리
// 예약).
struct JobStorage {
    ShaderDefine defines[MaxShaderPermutationDimensions] = {};
    char values[MaxShaderPermutationDimensions][DefineValueBytes] = {};
    usize defineCount = 0;
    Sha256Digest signature{};
};

// 항목 하나의 값 목록이다.
struct EntryValues {
    ShaderLibraryValue values[MaxShaderLibraryValues] = {};
    usize count = 0;
};

void CopyText(char* out, usize capacity, StringView text) noexcept {
    const usize bytes = text.ByteCount() < capacity - 1 ? text.ByteCount() : capacity - 1;
    if (bytes > 0) {
        memcpy(out, text.Data(), bytes);
    }

    out[bytes] = '\0';
}

Result<void> Fail(ErrorCode code) noexcept {
    return Result<void>::Failure(code);
}

ErrorCode OutcomeError(ShaderLibraryCookOutcome outcome, ErrorCode writeError) noexcept {
    switch (outcome) {
    case ShaderLibraryCookOutcome::Written:
    case ShaderLibraryCookOutcome::Unchanged:
    case ShaderLibraryCookOutcome::Built:
        return ErrorCode::None;
    case ShaderLibraryCookOutcome::JobsFailed:
        return ErrorCode::DependencyFailed;
    case ShaderLibraryCookOutcome::CorruptEntry:
        return ErrorCode::AlreadyExists;
    case ShaderLibraryCookOutcome::BuildFailed:
    case ShaderLibraryCookOutcome::BundleFailed:
        return ErrorCode::InvalidArgument;
    case ShaderLibraryCookOutcome::WriteFailed:
        return writeError == ErrorCode::None ? ErrorCode::InvalidArgument : writeError;
    }

    return ErrorCode::InvalidArgument;
}

// 조합의 정의를 storage에 채운다(차원 선언 순서 — SC10이 이름순으로 조립한다).
Result<void> FillDefines(const ShaderLibraryCombination& plan, JobStorage& storage) noexcept {
    const ShaderPermutationDomain& domain = plan.type->permutation;
    const usize dimensions = domain.dimensions.Size();
    uint32 valueIds[MaxShaderPermutationDimensions] = {};
    if (auto decoded =
            DecodeShaderPermutation(domain, plan.permutationId, Span<uint32>(valueIds, dimensions));
        !decoded) {
        return decoded;
    }

    for (usize index = 0; index < dimensions; ++index) {
        const ShaderPermutationDimension& dimension = domain.dimensions[index];
        const Result<int32> value = ShaderPermutationDefineValue(dimension, valueIds[index]);
        if (!value) {
            return Fail(value.Error());
        }

        (void)snprintf(storage.values[index], DefineValueBytes, "%d",
                       static_cast<int>(value.Value()));
        storage.defines[index] =
            ShaderDefine{dimension.defineName, StringView(storage.values[index])};
    }

    storage.defineCount = dimensions;
    storage.signature = ShaderParameterLayoutSignature(plan.type->parameterLayouts);
    return Result<void>::Success();
}

// 결과 값을 라이브러리 값 목록으로 옮긴다(값 id 오름차순, 3 DebugSymbols는 넣지 않는다).
void CollectValues(const ShaderJobValues& values, EntryValues& out) noexcept {
    constexpr ShaderNuggetValue Kept[] = {ShaderNuggetValue::Code, ShaderNuggetValue::BindingLayout,
                                          ShaderNuggetValue::GlesCopyTable};
    out.count = 0;
    for (const ShaderNuggetValue value : Kept) {
        if (values.Has(value)) {
            out.values[out.count++] =
                ShaderLibraryValue{static_cast<uint32>(value), values.Part(value).View()};
        }
    }
}

// path(NUL 종료)에 bytes를 임시 파일 → 덮어쓰지 않는 이동으로 쓴다. 이미 있으면 내용을 비교한다.
ShaderLibraryCookOutcome PublishFile(platform::FileSystem& files, Allocator& allocator,
                                     const String& path, Span<const uint8> bytes,
                                     ErrorCode& error) noexcept {
    const auto compareExisting = [&]() noexcept {
        Array<uint8> existing(allocator);
        bool existed = false;
        const Result<void> read =
            ReadWholeFile(files, path.View(), bytes.Size(), existing, existed);
        if (!read && read.Error() != ErrorCode::CapacityExceeded) {
            error = read.Error();
            return ShaderLibraryCookOutcome::WriteFailed;
        }

        if (!read || !existed || existing.Size() != bytes.Size() ||
            !(Sha256::Hash(existing.View()) == Sha256::Hash(bytes))) {
            return ShaderLibraryCookOutcome::CorruptEntry;
        }

        return ShaderLibraryCookOutcome::Unchanged;
    };

    bool exists = false;
    if (const platform::FileStatus checked =
            files.FileExists(path.CStr(), path.View().ByteCount(), exists);
        !checked) {
        error = FileErrorCode(checked);
        return ShaderLibraryCookOutcome::WriteFailed;
    }

    if (exists) {
        return compareExisting();
    }

    String temporary(allocator);
    char suffix[TemporarySuffixBytes];
    (void)snprintf(suffix, sizeof(suffix), ".%u.tmp",
                   static_cast<unsigned>(platform::CurrentProcessId()));
    if (!temporary.TryAssign(path.View()) || !temporary.TryAppend(StringView(suffix))) {
        error = ErrorCode::OutOfMemory;
        return ShaderLibraryCookOutcome::WriteFailed;
    }

    const auto removeTemporary = [&]() noexcept {
        (void)files.Delete(temporary.CStr(), temporary.View().ByteCount());
    };
    {
        platform::File file;
        platform::FileStatus status =
            file.Open(files, temporary.CStr(), temporary.View().ByteCount(),
                      platform::FileOpenMode::CreateAlways);
        if (status) {
            status = file.WriteAll(bytes.Data(), bytes.Size()).status;
        }

        if (status) {
            status = file.Flush();
        }

        const platform::FileStatus closed = file.IsOpen() ? file.Close() : platform::FileStatus{};
        if (!status || !closed) {
            error = FileErrorCode(!status ? status : closed);
            removeTemporary();
            return ShaderLibraryCookOutcome::WriteFailed;
        }
    }

    const platform::FileStatus moved = files.Move(temporary.CStr(), temporary.View().ByteCount(),
                                                  path.CStr(), path.View().ByteCount(), false);
    if (!moved) {
        removeTemporary();
        // 다른 쿠커가 먼저 게시했으면 그 byte와 비교한다.
        if (moved.code == platform::FileCode::AlreadyExists) {
            return compareExisting();
        }

        error = FileErrorCode(moved);
        return ShaderLibraryCookOutcome::WriteFailed;
    }

    return ShaderLibraryCookOutcome::Written;
}

// 결과의 include 폐포를 원문별 타입 목록에 더한다(ShaderSystem 16.3절 SSD69).
Result<void> CollectSources(const Array<ShaderLibraryCombination>& plans,
                            const Array<ShaderCompileJobResult>& results,
                            Array<ShaderLibrarySourceUse>& sources) noexcept {
    for (usize index = 0; index < results.Size(); ++index) {
        const uint64 type = plans[index].type->nameHash;
        for (const ShaderIncludeFile& include : results[index].preprocessed.includes) {
            ShaderLibrarySourceUse* found = nullptr;
            for (ShaderLibrarySourceUse& use : sources) {
                if (use.virtualPath.View() == include.virtualPath.View()) {
                    found = &use;
                    break;
                }
            }

            if (found == nullptr) {
                auto added = sources.TryEmplaceBack(sources.GetAllocator());
                if (!added || !added.Value()->virtualPath.TryAssign(include.virtualPath.View())) {
                    return Fail(ErrorCode::OutOfMemory);
                }

                found = added.Value();
            }

            bool known = false;
            for (const uint64 existing : found->types) {
                known = known || existing == type;
            }

            if (!known && !found->types.TryPushBack(type)) {
                return Fail(ErrorCode::OutOfMemory);
            }
        }
    }

    return Result<void>::Success();
}

enum class SymbolWrite : uint8 {
    Written,
    Unchanged,
    Failed
};

// 성공한 job의 값 3을 <directory>/<형식 id>/<이름>에 쓴다(SSD74). 같은 byte가 있으면 두고, 다르면
// 임시 파일에 쓴 뒤 Replace로 덮어쓴다(PDB 이름은 출력 binary hash라 주석만 바꾸면 같은 이름에 다른
// PDB가 온다 — 최신 원문을 따른다).
SymbolWrite WriteSymbolFile(platform::FileSystem& files, Allocator& allocator, StringView directory,
                            StringView formatId, const ShaderCompileJobResult& result) noexcept {
    const auto decoded =
        DecodeShaderDebugSymbols(result.values.Part(ShaderNuggetValue::DebugSymbols).View());
    if (!decoded) {
        return SymbolWrite::Failed;
    }

    const ShaderDebugSymbolsView& symbols = decoded.Value();
    String folder(allocator);
    String path(allocator);
    if (!folder.TryAssign(directory) || !folder.TryAppend("/") || !folder.TryAppend(formatId) ||
        !path.TryAssign(folder.View()) || !path.TryAppend("/")) {
        return SymbolWrite::Failed;
    }

    if (symbols.name.IsEmpty()) {
        char hex[Sha256Digest::HexBytes + 1];
        result.key.ToHex(hex);
        if (!path.TryAppend(StringView(hex)) ||
            !path.TryAppend(StringView(ShaderDebugSymbolExtension(symbols.kind)))) {
            return SymbolWrite::Failed;
        }
    } else if (!path.TryAppend(symbols.name)) {
        return SymbolWrite::Failed;
    }

    if (!platform::MakeDirectoryTree(files, folder.CStr(), folder.View().ByteCount())) {
        return SymbolWrite::Failed;
    }

    Array<uint8> existing(allocator);
    bool existed = false;
    if (ReadWholeFile(files, path.View(), symbols.contents.Size(), existing, existed) && existed &&
        existing.Size() == symbols.contents.Size() &&
        (existing.IsEmpty() ||
         memcmp(existing.Data(), symbols.contents.Data(), existing.Size()) == 0)) {
        return SymbolWrite::Unchanged;
    }

    String temporary(allocator);
    char suffix[TemporarySuffixBytes];
    (void)snprintf(suffix, sizeof(suffix), ".%u.tmp",
                   static_cast<unsigned>(platform::CurrentProcessId()));
    if (!temporary.TryAssign(path.View()) || !temporary.TryAppend(StringView(suffix))) {
        return SymbolWrite::Failed;
    }

    platform::File file;
    platform::FileStatus status = file.Open(files, temporary.CStr(), temporary.View().ByteCount(),
                                            platform::FileOpenMode::CreateAlways);
    if (status) {
        status = file.WriteAll(symbols.contents.Data(), symbols.contents.Size()).status;
    }

    if (status) {
        status = file.Flush();
    }

    const platform::FileStatus closed = file.IsOpen() ? file.Close() : platform::FileStatus{};
    if (status && closed &&
        files.Replace(temporary.CStr(), temporary.View().ByteCount(), path.CStr(),
                      path.View().ByteCount())) {
        return SymbolWrite::Written;
    }

    (void)files.Delete(temporary.CStr(), temporary.View().ByteCount());
    return SymbolWrite::Failed;
}

// 성공한 job의 값 3을 모두 쓰고 형식 줄에 센다.
void WriteSymbolFiles(const ShaderLibraryCookDesc& desc, platform::FileSystem& files,
                      Allocator& allocator, StringView formatId,
                      const Array<ShaderCompileJobResult>& results,
                      ShaderLibraryCookFormat& line) noexcept {
    if (desc.symbolDirectory.IsEmpty()) {
        return;
    }

    for (const ShaderCompileJobResult& result : results) {
        if (result.status != ShaderCompileStatus::Success ||
            !result.values.Has(ShaderNuggetValue::DebugSymbols)) {
            continue;
        }

        switch (WriteSymbolFile(files, allocator, desc.symbolDirectory, formatId, result)) {
        case SymbolWrite::Written:
            ++line.symbolFiles;
            break;
        case SymbolWrite::Unchanged:
            ++line.symbolsUnchanged;
            break;
        case SymbolWrite::Failed:
            ++line.symbolFailures;
            break;
        }
    }
}

// cookKey 입력 writer다(정수는 little-endian — SSD46).
class CookKeyWriter {
public:
    void U8(uint8 value) noexcept {
        hasher_.Update(Span<const uint8>(&value, 1));
    }

    void U32(uint32 value) noexcept {
        uint8 bytes[sizeof(uint32)];
        for (usize index = 0; index < sizeof(bytes); ++index) {
            bytes[index] = static_cast<uint8>(value >> (index * 8));
        }

        hasher_.Update(Span<const uint8>(bytes, sizeof(bytes)));
    }

    void U64(uint64 value) noexcept {
        U32(static_cast<uint32>(value));
        U32(static_cast<uint32>(value >> 32));
    }

    void Digest(const Sha256Digest& digest) noexcept {
        hasher_.Update(Span<const uint8>(digest.bytes, Sha256Digest::Bytes));
    }

    Sha256Digest Finish() noexcept {
        return hasher_.Finish();
    }

private:
    Sha256 hasher_;
};

// 라이브러리 cookKey다(Docs/Design/ShaderSystem 16.1절 SSD46). entries는 BuildShaderLibrary에 넘긴
// 항목 표(조합 순서), jobKeys는 정렬할 job 키 사본이다.
Sha256Digest LibraryCookKey(const ShaderLibraryCookDesc& desc, const ShaderFormatSnapshot& format,
                            const Sha256Digest& typeSet,
                            Span<const ShaderLibraryBuildEntry> entries,
                            Span<Sha256Digest> jobKeys) noexcept {
    CookKeyWriter writer;
    writer.U64(ShaderLibraryCookerGuid.high);
    writer.U64(ShaderLibraryCookerGuid.low);
    writer.U32(ShaderLibraryCookKeyVersion);
    writer.U32(ShaderLibraryVersion);
    writer.U8(static_cast<uint8>(desc.library));
    writer.U8(desc.includeDevelopment ? 1 : 0);
    writer.Digest(format.definition.sha256);
    writer.Digest(typeSet);
    writer.U32(static_cast<uint32>(entries.Size()));
    for (const ShaderLibraryBuildEntry& entry : entries) {
        writer.U64(entry.key.bundleId);
        writer.U64(entry.key.nameHash);
        writer.U64(entry.key.vertexInputType);
        writer.U32(entry.key.permutationId);
        writer.U32(entry.stage);
        writer.Digest(entry.layoutSignature);
        writer.U32(entry.runtimeFeatures);
        writer.U8(entry.developmentOnly ? 1 : 0);
    }

    algo::Sort(jobKeys, [](const Sha256Digest& left, const Sha256Digest& right) noexcept {
        return memcmp(left.bytes, right.bytes, Sha256Digest::Bytes) < 0;
    });
    writer.U32(static_cast<uint32>(jobKeys.Size()));
    for (const Sha256Digest& key : jobKeys) {
        writer.Digest(key);
    }

    return writer.Finish();
}

// 정규화한 머티리얼 묶음 하나다(같은 셰이더 집합 id를 합친 것 — 16.5절 SSD88).
struct NormalizedBundle {
    explicit NormalizedBundle(Allocator& allocator) noexcept : inputs(allocator) {
    }

    // 정렬 순 첫 묶음(원문·traits의 기준)이다.
    const MaterialShaderBundle* first = nullptr;
    uint64 bundleId = EngineShaderBundleId;
    // 합친 묶음 가운데 가장 작은 머티리얼이다(진단용).
    AssetId asset{};
    // 사용 대상 정점 입력 타입의 등록부 desc 사본이다(id 오름차순, 중복 없음).
    Array<ShaderVertexInputType> inputs;
};

bool BundleLess(const MaterialShaderBundle* left, const MaterialShaderBundle* right) noexcept {
    const uint64 leftId = MaterialShaderBundleId(left->shaderSetId);
    const uint64 rightId = MaterialShaderBundleId(right->shaderSetId);
    if (leftId != rightId) {
        return leftId < rightId;
    }

    const int set = memcmp(left->shaderSetId.bytes, right->shaderSetId.bytes, Sha256Digest::Bytes);
    if (set != 0) {
        return set < 0;
    }

    return GuidLess(left->asset.value, right->asset.value);
}

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    return left.Size() == right.Size() &&
           (left.IsEmpty() || memcmp(left.Data(), right.Data(), left.Size()) == 0);
}

// 문제 하나를 report에 더하고 영어 Log Error로 남긴다.
Result<void> AddBundleProblem(ShaderLibraryCookReport& report, StringView formatId,
                              const ShaderLibraryBundleProblem& problem) noexcept {
    if (!report.bundleProblems.TryPushBack(problem)) {
        return Fail(ErrorCode::OutOfMemory);
    }

    ShaderLibraryBundleProblem& stored = report.bundleProblems[report.bundleProblems.Size() - 1];
    CopyText(stored.formatId, sizeof(stored.formatId), formatId);
    char set[Sha256Digest::HexBytes + 1];
    char other[Sha256Digest::HexBytes + 1];
    char bundle[BundleIdHexBytes];
    problem.shaderSetId.ToHex(set);
    problem.otherShaderSetId.ToHex(other);
    (void)snprintf(bundle, sizeof(bundle), "%016llx",
                   static_cast<unsigned long long>(problem.bundleId));
    switch (problem.kind) {
    case ShaderLibraryBundleProblemKind::Collision:
        Log::Error(LogShaderLibraryBuild,
                   "Material shader sets {Set} (material {Asset}) and {Other} (material "
                   "{OtherAsset}) share the shader library bundle id {Bundle} in {Format} (a "
                   "64-bit prefix collision of SHA-256 shader set ids); change one of the "
                   "materials so that its shader set id differs",
                   StringView(set), problem.asset, StringView(other), problem.otherAsset,
                   StringView(bundle), formatId);
        break;
    case ShaderLibraryBundleProblemKind::Inconsistent:
        Log::Error(LogShaderLibraryBuild,
                   "Material shader set {Set} (material {Asset}) arrived more than once for "
                   "{Format} with different generated source or traits bytes (the material "
                   "shader bundle source broke its contract)",
                   StringView(set), problem.asset, formatId);
        break;
    case ShaderLibraryBundleProblemKind::UnknownVertexInput:
        Log::Error(LogShaderLibraryBuild,
                   "Material shader set {Set} (material {Asset}) uses vertex input type id "
                   "{VertexInput} for {Format}, which no loaded module registered",
                   StringView(set), problem.asset, problem.vertexInputType, formatId);
        break;
    }

    return Result<void>::Success();
}

// bundle의 정점 입력 타입을 group에 합친다(id 순, 중복 없음). 등록부에 없으면 문제다.
Result<void> MergeInputs(const ShaderTypeRegistry& registry, const MaterialShaderBundle& bundle,
                         StringView formatId, NormalizedBundle& group,
                         ShaderLibraryCookReport& report, bool& failed) noexcept {
    for (const VertexInputTypeId id : bundle.vertexInputTypes) {
        const ShaderVertexInputType* input =
            id == NoVertexInputType ? nullptr : registry.FindVertexInputType(id);
        if (input == nullptr) {
            ShaderLibraryBundleProblem problem;
            problem.kind = ShaderLibraryBundleProblemKind::UnknownVertexInput;
            problem.bundleId = group.bundleId;
            problem.shaderSetId = bundle.shaderSetId;
            problem.asset = bundle.asset;
            problem.vertexInputType = id;
            failed = true;
            if (auto added = AddBundleProblem(report, formatId, problem); !added) {
                return added;
            }

            continue;
        }

        usize at = 0;
        while (at < group.inputs.Size() && group.inputs[at].id < id) {
            ++at;
        }

        if (at < group.inputs.Size() && group.inputs[at].id == id) {
            continue;
        }

        if (!group.inputs.TryInsert(*input, at)) {
            return Fail(ErrorCode::OutOfMemory);
        }
    }

    return Result<void>::Success();
}

// desc.materialBundles 가운데 formatId 형식의 묶음을 정규화한다(16.5절 SSD88). 문제가 있으면
// failed가 참이고 report.bundleProblems에 정렬 순으로 남는다.
Result<void> NormalizeBundles(const ShaderLibraryCookDesc& desc, StringView formatId,
                              Allocator& allocator, Array<NormalizedBundle>& out,
                              ShaderLibraryCookReport& report, bool& failed) noexcept {
    out.Clear();
    failed = false;
    Array<const MaterialShaderBundle*> chosen(allocator);
    for (const MaterialShaderBundle& bundle : desc.materialBundles) {
        if (bundle.formatId == formatId && !chosen.TryPushBack(&bundle)) {
            return Fail(ErrorCode::OutOfMemory);
        }
    }

    algo::Sort(chosen.View(), BundleLess);
    for (const MaterialShaderBundle* bundle : chosen) {
        const uint64 bundleId = MaterialShaderBundleId(bundle->shaderSetId);
        NormalizedBundle* last = out.IsEmpty() ? nullptr : &out[out.Size() - 1];
        if (last != nullptr && last->first->shaderSetId == bundle->shaderSetId) {
            // 같은 셰이더 집합은 하나다 — 내용이 같아야 한다.
            if (!(last->first->materialSource == bundle->materialSource) ||
                !SameBytes(last->first->traits, bundle->traits)) {
                ShaderLibraryBundleProblem problem;
                problem.kind = ShaderLibraryBundleProblemKind::Inconsistent;
                problem.bundleId = bundleId;
                problem.shaderSetId = bundle->shaderSetId;
                problem.asset = bundle->asset;
                failed = true;
                if (auto added = AddBundleProblem(report, formatId, problem); !added) {
                    return added;
                }
            }

            if (auto merged = MergeInputs(*desc.registry, *bundle, formatId, *last, report, failed);
                !merged) {
                return merged;
            }

            continue;
        }

        if (last != nullptr && last->bundleId == bundleId) {
            ShaderLibraryBundleProblem problem;
            problem.kind = ShaderLibraryBundleProblemKind::Collision;
            problem.bundleId = bundleId;
            problem.shaderSetId = last->first->shaderSetId;
            problem.asset = last->asset;
            problem.otherShaderSetId = bundle->shaderSetId;
            problem.otherAsset = bundle->asset;
            failed = true;
            if (auto added = AddBundleProblem(report, formatId, problem); !added) {
                return added;
            }

            continue;
        }

        if (!out.TryEmplaceBack(allocator)) {
            return Fail(ErrorCode::OutOfMemory);
        }

        NormalizedBundle& group = out[out.Size() - 1];
        group.first = bundle;
        group.bundleId = bundleId;
        group.asset = bundle->asset;
        if (auto merged = MergeInputs(*desc.registry, *bundle, formatId, group, report, failed);
            !merged) {
            return merged;
        }
    }

    return Result<void>::Success();
}

// 라이브러리 파일 경로다.
Result<void> BuildPath(const ShaderLibraryCookDesc& desc, StringView formatId,
                       const Sha256Digest& fileSha, String& out) noexcept {
    char hex[Sha256Digest::HexBytes + 1];
    fileSha.ToHex(hex);
    const StringView pieces[] = {desc.outputDirectory,
                                 "/",
                                 StringView(ShaderLibraryKindName(desc.library)),
                                 "-",
                                 formatId,
                                 "-",
                                 StringView(hex, FileShaPrefixHex),
                                 ".",
                                 StringView(ShaderLibraryExtension)};
    for (const StringView piece : pieces) {
        if (auto appended = out.TryAppend(piece); !appended) {
            return appended;
        }
    }

    return Result<void>::Success();
}

} // namespace library_cook_detail

const char* ShaderLibraryCookOutcomeName(ShaderLibraryCookOutcome outcome) noexcept {
    switch (outcome) {
    case ShaderLibraryCookOutcome::Written:
        return "Written";
    case ShaderLibraryCookOutcome::Unchanged:
        return "Unchanged";
    case ShaderLibraryCookOutcome::JobsFailed:
        return "JobsFailed";
    case ShaderLibraryCookOutcome::CorruptEntry:
        return "CorruptEntry";
    case ShaderLibraryCookOutcome::BuildFailed:
        return "BuildFailed";
    case ShaderLibraryCookOutcome::WriteFailed:
        return "WriteFailed";
    case ShaderLibraryCookOutcome::Built:
        return "Built";
    case ShaderLibraryCookOutcome::BundleFailed:
        return "BundleFailed";
    }

    return "unknown";
}

const char* ShaderLibraryBundleProblemKindName(ShaderLibraryBundleProblemKind kind) noexcept {
    switch (kind) {
    case ShaderLibraryBundleProblemKind::Collision:
        return "Collision";
    case ShaderLibraryBundleProblemKind::Inconsistent:
        return "Inconsistent";
    case ShaderLibraryBundleProblemKind::UnknownVertexInput:
        return "UnknownVertexInput";
    }

    return "unknown";
}

namespace library_cook_detail {

// 쿠킹과 공급자 메모리 라이브러리(SSD16)가 함께 쓰는 본체다. memory가 null이 아니면 파일을 쓰지
// 않고 (형식 하나의) v1 byte를 memory에 둔다 — 같은 입력이면 쿠킹 파일과 같은 byte다.
Result<void> RunLibraries(const ShaderLibraryCookDesc& desc, ShaderLibraryCookReport& report,
                          Array<uint8>* memory) noexcept {
    report.formats.Clear();
    report.failures.Clear();
    report.bundleProblems.Clear();
    report.sources.Clear();
    if (desc.allocator == nullptr || desc.registry == nullptr || desc.manager == nullptr ||
        (memory == nullptr && desc.outputDirectory.IsEmpty()) ||
        (memory != nullptr && desc.formats.Size() != 1) ||
        (desc.library == ShaderLibraryKind::Engine && !desc.materialBundles.IsEmpty())) {
        return Fail(ErrorCode::InvalidArgument);
    }

    if (!desc.registry->IsFrozen()) {
        return Fail(ErrorCode::NotReady);
    }

    Allocator& allocator = *desc.allocator;
    platform::FileSystem& files =
        desc.files != nullptr ? *desc.files : platform::PhysicalFileSystem();

    // 정점 입력 타입은 등록부 목록(Freeze 뒤 id 순)이다(RenderBase 10.6절 1항).
    const Span<const ShaderVertexInputType> inputs = desc.registry->VertexInputTypes();

    if (!report.formats.TryReserve(desc.formats.Size())) {
        return Fail(ErrorCode::OutOfMemory);
    }

    const Sha256Digest typeSet = desc.registry->TypeSetDigest();
    ErrorCode firstError = ErrorCode::None;
    Array<ShaderLibraryCombination> plans(allocator);
    for (const ShaderFormatSnapshot* snapshot : desc.formats) {
        if (snapshot == nullptr) {
            return Fail(ErrorCode::InvalidArgument);
        }

        (void)report.formats.TryPushBack(ShaderLibraryCookFormat(allocator));
        ShaderLibraryCookFormat& line = report.formats[report.formats.Size() - 1];
        CopyText(line.formatId, sizeof(line.formatId), snapshot->definition.Id());
        const ShaderFormatFacts facts = MakeShaderFormatFacts(snapshot->definition);
        if (auto enumerated =
                EnumerateShaderLibraryCombinations(desc.registry->Types(), inputs, facts,
                                                   desc.library, desc.includeDevelopment, plans);
            !enumerated) {
            return enumerated;
        }

        // 머티리얼 묶음(Project 라이브러리 — 16.5절 SSD88)이다. planBundles는 조합마다 묶음 번호
        // (엔진 셰이더는 IndexNone)다.
        Array<NormalizedBundle> bundles(allocator);
        bool bundleFailed = false;
        if (auto normalized = NormalizeBundles(desc, snapshot->definition.Id(), allocator, bundles,
                                               report, bundleFailed);
            !normalized) {
            return normalized;
        }

        line.materialBundles = static_cast<uint32>(bundles.Size());
        if (bundleFailed) {
            line.outcome = ShaderLibraryCookOutcome::BundleFailed;
            line.reason = "material shader bundles collide or break the bundle source contract";
            firstError = firstError == ErrorCode::None ? ErrorCode::InvalidArgument : firstError;
            continue;
        }

        Array<usize> planBundles(allocator);
        if (!planBundles.TryReserve(plans.Size())) {
            return Fail(ErrorCode::OutOfMemory);
        }

        for (usize index = 0; index < plans.Size(); ++index) {
            (void)planBundles.TryPushBack(IndexNone);
        }

        for (usize index = 0; index < bundles.Size(); ++index) {
            const NormalizedBundle& bundle = bundles[index];
            if (auto enumerated = EnumerateMaterialShaderCombinations(
                    desc.registry->Types(), bundle.inputs.View(), facts, bundle.first->traits,
                    bundle.bundleId, desc.includeDevelopment, plans);
                !enumerated) {
                return enumerated;
            }

            while (planBundles.Size() < plans.Size()) {
                if (!planBundles.TryPushBack(index)) {
                    return Fail(ErrorCode::OutOfMemory);
                }
            }
        }

        // job과 그 정의 저장소다(저장소는 예약한 뒤 옮기지 않는다).
        Array<JobStorage> storage(allocator);
        Array<ShaderCompileJob> jobs(allocator);
        Array<ShaderCompileJobResult> results(allocator);
        if (!storage.TryReserve(plans.Size()) || !jobs.TryReserve(plans.Size())) {
            return Fail(ErrorCode::OutOfMemory);
        }

        const ShaderCompileOptions defaults = DefaultShaderCompileOptions(snapshot->definition);
        for (usize planIndex = 0; planIndex < plans.Size(); ++planIndex) {
            const ShaderLibraryCombination& plan = plans[planIndex];
            (void)storage.TryPushBack(JobStorage{});
            JobStorage& defines = storage[storage.Size() - 1];
            if (auto filled = FillDefines(plan, defines); !filled) {
                return filled;
            }

            ShaderCompileJob job;
            job.format = snapshot;
            job.entryPath = plan.type->sourcePath;
            job.entryPoint = plan.type->entryPoint;
            job.stage = plan.type->stage;
            job.defines = Span<const ShaderDefine>(defines.defines, defines.defineCount);
            job.options = defaults;
            job.options.debugSymbols = desc.debugSymbols;
            if (plan.type->hlsl2021 != ShaderHlsl2021::FormatDefault) {
                job.options.hlsl2021 = plan.type->hlsl2021 == ShaderHlsl2021::On;
            }

            job.parameterLayouts = plan.type->parameterLayouts;
            job.requiredFeatures = ShaderLibraryCombinationFeatures(plan);
            job.developmentOnly = plan.developmentOnly;
            if (plan.vertexInputType != NoVertexInputType) {
                const ShaderVertexInputType* input =
                    desc.registry->FindVertexInputType(plan.vertexInputType);
                job.vertexInputSource = input != nullptr ? input->sourcePath : StringView();
            }

            // 머티리얼 job: 생성 원문을 잇고 경고를 오류로 다루지 않는다(16.4절 SSD79).
            if (planBundles[planIndex] != IndexNone) {
                job.materialSource = bundles[planBundles[planIndex]].first->materialSource;
                job.options.warningsAsErrors = false;
            }

            (void)jobs.TryPushBack(job);
        }

        line.jobs = static_cast<uint32>(jobs.Size());
        if (auto compiled = desc.manager->Compile(jobs.View(), results, desc.control); !compiled) {
            return compiled;
        }

        for (const ShaderCompileJobResult& result : results) {
            line.cacheHits += result.cacheHit ? 1u : 0u;
        }

        if (auto collected = CollectSources(plans, results, report.sources); !collected) {
            return collected;
        }

        WriteSymbolFiles(desc, files, allocator, snapshot->definition.Id(), results, line);

        // 성공 job의 진단(경고를 오류로 다루지 않는 job의 경고, D3D11 대체 경로 성공의 1차 FXC
        // 진단, 너겟 캐시 메모 — ShaderSystem 16.4절 SSD79·SSD80·SSD84)을 버리지 않고 남긴다.
        for (usize index = 0; index < results.Size(); ++index) {
            const ShaderCompileJobResult& result = results[index];
            if (result.status != ShaderCompileStatus::Success || result.diagnostics.IsEmpty()) {
                continue;
            }

            Log::Warning(LogShaderLibraryBuild,
                         "Shader {Type} (vertex input {VertexInput}, permutation {Permutation}) "
                         "for {Format} compiled with diagnostics:\n{Diagnostics}",
                         plans[index].type->name, static_cast<uint64>(plans[index].vertexInputType),
                         static_cast<uint64>(plans[index].permutationId), snapshot->definition.Id(),
                         result.diagnostics.View());
        }

        // 실패를 모은다.
        for (usize index = 0; index < results.Size(); ++index) {
            const ShaderCompileJobResult& result = results[index];
            if (result.status == ShaderCompileStatus::Success) {
                continue;
            }

            ++line.failedJobs;
            if (!report.failures.TryPushBack(ShaderLibraryCookJobFailure(allocator))) {
                return Fail(ErrorCode::OutOfMemory);
            }

            ShaderLibraryCookJobFailure& failure = report.failures[report.failures.Size() - 1];
            CopyText(failure.formatId, sizeof(failure.formatId), snapshot->definition.Id());
            CopyText(failure.typeName, sizeof(failure.typeName), plans[index].type->name);
            failure.vertexInputType = plans[index].vertexInputType;
            failure.permutationId = plans[index].permutationId;
            failure.bundleId = plans[index].bundleId;
            if (planBundles[index] != IndexNone) {
                failure.asset = bundles[planBundles[index]].asset;
            }

            failure.status = result.status;
            (void)failure.diagnostics.TryAssign(result.diagnostics.View());
        }

        if (line.failedJobs > 0) {
            line.outcome = ShaderLibraryCookOutcome::JobsFailed;
            firstError = firstError == ErrorCode::None ? ErrorCode::DependencyFailed : firstError;
            continue;
        }

        // 라이브러리 byte다.
        Array<EntryValues> values(allocator);
        Array<ShaderLibraryBuildEntry> entries(allocator);
        if (!values.TryReserve(results.Size()) || !entries.TryReserve(results.Size())) {
            return Fail(ErrorCode::OutOfMemory);
        }

        for (usize index = 0; index < results.Size(); ++index) {
            const ShaderLibraryCombination& plan = plans[index];
            (void)values.TryPushBack(EntryValues{});
            EntryValues& entryValues = values[values.Size() - 1];
            CollectValues(results[index].values, entryValues);
            ShaderLibraryBuildEntry entry;
            entry.key = ShaderLibraryCombinationKey(plan);
            entry.typeName = plan.type->name;
            entry.stage = plan.type->stage;
            entry.developmentOnly = plan.developmentOnly;
            entry.runtimeFeatures =
                ShaderLibraryCombinationFeatures(plan) & RuntimeCheckedShaderFeatures;
            entry.layoutSignature = storage[index].signature;
            entry.values = Span<const ShaderLibraryValue>(entryValues.values, entryValues.count);
            (void)entries.TryPushBack(entry);
        }

        // job 키 사본(정렬용)이다. 성공한 job은 키가 있다(키 계산이 컴파일 앞이다).
        Array<Sha256Digest> jobKeys(allocator);
        if (!jobKeys.TryReserve(results.Size())) {
            return Fail(ErrorCode::OutOfMemory);
        }

        for (const ShaderCompileJobResult& result : results) {
            (void)jobKeys.TryPushBack(result.key);
        }

        line.cookKey = LibraryCookKey(desc, *snapshot, typeSet, entries.View(),
                                      Span<Sha256Digest>(jobKeys.Data(), jobKeys.Size()));
        line.hasCookKey = true;
        ShaderLibraryBuildDesc build;
        build.kind = desc.library;
        build.facts = facts;
        build.formatDefinitionSha256 = snapshot->definition.sha256;
        build.typeSetSha256 = typeSet;
        build.entries = entries.View();
        Array<uint8> bytes(allocator);
        if (auto built = BuildShaderLibrary(build, allocator, bytes, &line.reason); !built) {
            if (built.Error() == ErrorCode::OutOfMemory) {
                return built;
            }

            line.outcome = ShaderLibraryCookOutcome::BuildFailed;
            firstError = firstError == ErrorCode::None ? ErrorCode::InvalidArgument : firstError;
            continue;
        }

        ShaderLibraryView view;
        if (const ShaderReadStatus opened = view.Open(bytes.View()); !opened) {
            // 만든 byte가 읽기 규칙을 어기면 만들기의 결함이다.
            line.outcome = ShaderLibraryCookOutcome::BuildFailed;
            line.reason = opened.reason;
            firstError = firstError == ErrorCode::None ? ErrorCode::InvalidArgument : firstError;
            continue;
        }

        line.fileSha256 = view.FileSha256();
        line.entries = view.EntryCount();
        line.codes = view.CodeCount();
        if (memory != nullptr) {
            *memory = Move(bytes);
            line.outcome = ShaderLibraryCookOutcome::Built;
            continue;
        }

        if (auto pathed = BuildPath(desc, snapshot->definition.Id(), line.fileSha256, line.path);
            !pathed) {
            return pathed;
        }

        if (const platform::FileStatus made = platform::MakeDirectoryTree(
                files, desc.outputDirectory.Data(), desc.outputDirectory.ByteCount());
            !made) {
            line.outcome = ShaderLibraryCookOutcome::WriteFailed;
            line.reason = "cannot create the shader library output directory";
            firstError = firstError == ErrorCode::None ? FileErrorCode(made) : firstError;
            continue;
        }

        ErrorCode writeError = ErrorCode::None;
        line.outcome = PublishFile(files, allocator, line.path, bytes.View(), writeError);
        if (line.outcome == ShaderLibraryCookOutcome::WriteFailed) {
            line.reason = "cannot write or publish the shader library file";
        }

        const ErrorCode error = OutcomeError(line.outcome, writeError);
        firstError = firstError == ErrorCode::None ? error : firstError;
    }

    return firstError == ErrorCode::None ? Result<void>::Success() : Fail(firstError);
}

} // namespace library_cook_detail

Result<void> CookShaderLibraries(const ShaderLibraryCookDesc& desc,
                                 ShaderLibraryCookReport& report) noexcept {
    return library_cook_detail::RunLibraries(desc, report, nullptr);
}

Result<void> CompileShaderLibrary(const ShaderLibraryCookDesc& desc,
                                  ShaderLibraryCookReport& report, Array<uint8>& out) noexcept {
    out.Clear();
    return library_cook_detail::RunLibraries(desc, report, &out);
}

} // namespace rocks::shader
