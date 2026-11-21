#include "ModuleManifest.h"

#include "ModuleLoaderInternal.h"

#include <Core/Move.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>
#include <Platform/File.h>

namespace rocks::detail {

namespace {

// manifest 형식 판이다(Rocks.Build ModuleManifests.Schema와 같다).
constexpr int64 ManifestSchema = 2;
// buildId(GUID 32자 소문자 16진)와 툴체인 지문(SHA-256 64자 소문자 16진)의 길이다.
constexpr usize BuildIdLength = 32;
constexpr usize FingerprintLength = 64;
// 최상위 키 수와 행 키 수다(정확히 이 키들만 있어야 한다).
constexpr usize TopLevelKeyCount = 5;
constexpr usize RowKeyCount = 7;
// 공유 라이브러리마다 하나뿐인 export 진입점 이름이다(Core/Module/ModuleEntry.h).
constexpr const char* ExportedEntrySymbol = "RocksModuleEntry";

using ExportedEntryFunction = const ModuleEntry* (*)() noexcept;

constexpr StringView ManifestStage = "Manifest";

bool IsLowerHex(StringView value, usize length) noexcept {
    if (value.ByteCount() != length) {
        return false;
    }

    for (usize index = 0; index < length; ++index) {
        const char character = value.Data()[index];
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }

    return true;
}

// 경로 구분자·드라이브·상위 폴더가 없는 파일 이름인지다(manifest 폴더 밖을 가리키지 않게).
bool IsPlainFileName(StringView value) noexcept {
    if (value.IsEmpty() || value == "." || value == "..") {
        return false;
    }

    for (usize index = 0; index < value.ByteCount(); ++index) {
        const char character = value.Data()[index];
        if (character == '/' || character == '\\' || character == ':' || character == '\0') {
            return false;
        }
    }

    return true;
}

bool ParsePhase(StringView name, LoadingPhase& phase) noexcept {
    constexpr LoadingPhase phases[] = {LoadingPhase::EarliestPossible,
                                       LoadingPhase::PostConfigInit,
                                       LoadingPhase::PreDefault,
                                       LoadingPhase::Default,
                                       LoadingPhase::PostDefault,
                                       LoadingPhase::PostEngineInit,
                                       LoadingPhase::None};
    for (LoadingPhase candidate : phases) {
        if (name == StringView(LoadingPhaseName(candidate))) {
            phase = candidate;
            return true;
        }
    }

    return false;
}

// manifest 형식 오류를 report에 쓴다(Incompatible, 단계 "Manifest"). 늘 false다.
template <typename... Args>
bool Reject(ModuleLoadReport& report, StringView format, const Args&... args) noexcept {
    report.code = ModuleLoadCode::Incompatible;
    WriteReportText(report, report.stage, ManifestStage);
    WriteReportText(report, report.message, format, args...);
    return false;
}

bool RejectOutOfMemory(ModuleLoadReport& report) noexcept {
    report.error = ErrorCode::OutOfMemory;
    return Reject(report, "Out of memory while reading the module manifest");
}

// 객체가 정확히 count개 키를 가지며 names의 키가 모두 있는지다(모르는 키·빠진 키 거부).
bool HasExactKeys(JsonValue object, usize count, Span<const StringView> names) noexcept {
    if (!object.IsObject() || object.Size() != count) {
        return false;
    }

    for (StringView name : names) {
        if (!object.Find(name).IsValid()) {
            return false;
        }
    }

    return true;
}

// 바이트를 저장소 끝에 NUL과 함께 붙이고 그 시작 주소를 돌려준다(저장소는 미리 잡아 두었다).
const char* Store(Array<char>& text, StringView value) noexcept {
    const usize start = text.Size();
    for (usize index = 0; index < value.ByteCount(); ++index) {
        if (!text.TryPushBack(value.Data()[index])) {
            return nullptr;
        }
    }

    if (!text.TryPushBack('\0')) {
        return nullptr;
    }

    return text.Data() + start;
}

} // namespace

ModuleManifestData::ModuleManifestData(Allocator& allocator) noexcept
    : rows(allocator), entries(allocator), files(allocator), sizes(allocator),
      executable(allocator), text(allocator), dependencyNames(allocator), libraries(allocator),
      leases(allocator) {
}

ModuleManifestData::~ModuleManifestData() noexcept {
    CloseModuleBinaries(*this);
}

void CloseModuleBinaries(ModuleManifestData& data) noexcept {
    // lease를 먼저 놓아야 Close가 Busy가 아니다. 적재 역순으로 내린다(의존하는 쪽부터).
    for (usize index = data.leases.Size(); index != 0; --index) {
        data.leases[index - 1].Reset();
    }

    for (usize index = data.libraries.Size(); index != 0; --index) {
        (void)data.libraries[index - 1].Close();
    }

    data.leases.Clear();
    data.libraries.Clear();
}

bool ParseModuleManifest(StringView text, ModuleManifestData& data,
                         ModuleLoadReport& report) noexcept {
    Allocator& allocator = data.rows.GetAllocator();
    JsonDocument document(allocator);
    const JsonError parsed = document.Parse(text);
    if (!parsed.Succeeded()) {
        if (parsed.code == JsonErrorCode::OutOfMemory) {
            return RejectOutOfMemory(report);
        }

        return Reject(report, "Module manifest is not valid JSON at {}:{}: {}",
                      parsed.position.line, parsed.position.column, JsonErrorMessage(parsed.code));
    }

    const JsonValue root = document.Root();
    constexpr StringView topKeys[] = {"schema", "buildId", "engineVersion", "toolchainFingerprint",
                                      "modules"};
    if (!HasExactKeys(root, TopLevelKeyCount, topKeys)) {
        return Reject(report,
                      "Module manifest must be an object with exactly the keys schema, buildId, "
                      "engineVersion, toolchainFingerprint and modules");
    }

    const JsonValue schema = root.Find("schema");
    auto schemaValue = schema.IsNumber() ? schema.TryGetInt64()
                                         : Result<int64>::Failure(ErrorCode::InvalidArgument);
    if (!schemaValue || schemaValue.Value() != ManifestSchema) {
        return Reject(report, "Module manifest schema must be {}", ManifestSchema);
    }

    const JsonValue buildId = root.Find("buildId");
    if (!buildId.IsString() || !IsLowerHex(buildId.GetString(), BuildIdLength)) {
        return Reject(report, "Module manifest buildId must be {} lowercase hexadecimal digits",
                      BuildIdLength);
    }

    const JsonValue engineVersion = root.Find("engineVersion");
    if (!engineVersion.IsString() || engineVersion.GetString().IsEmpty()) {
        return Reject(report, "Module manifest engineVersion must be a non-empty string");
    }

    const JsonValue fingerprint = root.Find("toolchainFingerprint");
    if (!fingerprint.IsString() || !IsLowerHex(fingerprint.GetString(), FingerprintLength)) {
        return Reject(
            report, "Module manifest toolchainFingerprint must be {} lowercase hexadecimal digits",
            FingerprintLength);
    }

    const JsonValue modules = root.Find("modules");
    if (!modules.IsArray() || modules.Size() == 0) {
        return Reject(report, "Module manifest modules must be a non-empty array");
    }

    // 문자열 저장소와 의존 배열은 정확한 크기로 한 번 잡는다(주소가 고정되어 행이 가리킬 수 있다).
    constexpr StringView rowKeys[] = {"name",         "file",      "size",      "dependencies",
                                      "loadingPhase", "onRequest", "executable"};
    usize textBytes = 0;
    usize dependencyTotal = 0;
    for (usize index = 0; index < modules.Size(); ++index) {
        const JsonValue row = modules.At(index);
        if (!HasExactKeys(row, RowKeyCount, rowKeys)) {
            return Reject(
                report,
                "Module manifest modules[{}] must have exactly the keys name, file, size, "
                "dependencies, loadingPhase, onRequest and executable",
                index);
        }

        const JsonValue name = row.Find("name");
        const JsonValue file = row.Find("file");
        const JsonValue dependencies = row.Find("dependencies");
        if (!name.IsString() || !file.IsString() || !dependencies.IsArray()) {
            return Reject(report,
                          "Module manifest modules[{}] name and file must be strings and "
                          "dependencies an array",
                          index);
        }

        textBytes += name.GetString().ByteCount() + 1 + file.GetString().ByteCount() + 1;
        dependencyTotal += dependencies.Size();
    }

    const usize count = modules.Size();
    if (!data.rows.TryReserve(count) || !data.entries.TryReserve(count) ||
        !data.files.TryReserve(count) || !data.sizes.TryReserve(count) ||
        !data.executable.TryReserve(count) || !data.text.TryReserve(textBytes) ||
        !data.dependencyNames.TryReserve(dependencyTotal) || !data.libraries.TryReserve(count) ||
        !data.leases.TryReserve(count)) {
        return RejectOutOfMemory(report);
    }

    usize executableRows = 0;
    for (usize index = 0; index < count; ++index) {
        const JsonValue row = modules.At(index);
        const StringView name = row.Find("name").GetString();
        const StringView file = row.Find("file").GetString();
        if (!IsModuleIdentifier(name)) {
            return Reject(report, "Module manifest modules[{}] name '{}' is not a C identifier",
                          index, name);
        }

        for (const ModuleTableRow& earlier : data.rows) {
            if (StringView(earlier.name) == name) {
                return Reject(report, "Module manifest lists module {} twice", name);
            }
        }

        if (!IsPlainFileName(file)) {
            return Reject(report, "Module manifest module {} file '{}' must be a plain file name",
                          name, file);
        }

        const JsonValue size = row.Find("size");
        auto sizeValue = size.IsNumber() ? size.TryGetUInt64()
                                         : Result<uint64>::Failure(ErrorCode::InvalidArgument);
        if (!sizeValue) {
            return Reject(report, "Module manifest module {} size must be a non-negative integer",
                          name);
        }

        const JsonValue phaseName = row.Find("loadingPhase");
        LoadingPhase phase = LoadingPhase::Default;
        if (!phaseName.IsString() || !ParsePhase(phaseName.GetString(), phase)) {
            return Reject(report,
                          "Module manifest module {} loadingPhase must be a loading phase name",
                          name);
        }

        const JsonValue onRequest = row.Find("onRequest");
        const JsonValue executable = row.Find("executable");
        if (!onRequest.IsBool() || !executable.IsBool()) {
            return Reject(
                report, "Module manifest module {} onRequest and executable must be true or false",
                name);
        }

        if (executable.GetBool() && ++executableRows > 1) {
            return Reject(report,
                          "Module manifest has more than one executable row ({} is the second)",
                          name);
        }

        const char* storedName = Store(data.text, name);
        const char* storedFile = Store(data.text, file);
        if (storedName == nullptr || storedFile == nullptr) {
            platform::Fatal("ModuleManifest reserved text push failed");
        }

        // 의존은 앞 행 이름이다(표는 의존 순서). 저장한 앞 행 이름을 가리킨다.
        const JsonValue dependencies = row.Find("dependencies");
        const usize dependencyStart = data.dependencyNames.Size();
        for (usize dependency = 0; dependency < dependencies.Size(); ++dependency) {
            const JsonValue value = dependencies.At(dependency);
            const char* resolved = nullptr;
            if (value.IsString()) {
                for (const ModuleTableRow& earlier : data.rows) {
                    if (StringView(earlier.name) == value.GetString()) {
                        resolved = earlier.name;
                    }
                }
            }

            if (resolved == nullptr) {
                return Reject(
                    report, "Module manifest module {} dependency #{} must name an earlier module",
                    name, dependency);
            }

            for (usize seen = dependencyStart; seen < data.dependencyNames.Size(); ++seen) {
                if (data.dependencyNames[seen] == resolved) {
                    return Reject(report, "Module manifest module {} lists dependency {} twice",
                                  name, StringView(resolved));
                }
            }

            if (!data.dependencyNames.TryPushBack(resolved)) {
                platform::Fatal("ModuleManifest reserved dependency push failed");
            }
        }

        ModuleTableRow tableRow;
        tableRow.name = storedName;
        tableRow.entry = nullptr;
        tableRow.dependencies =
            dependencies.Size() == 0 ? nullptr : data.dependencyNames.Data() + dependencyStart;
        tableRow.dependencyCount = dependencies.Size();
        tableRow.phase = phase;
        tableRow.onRequest = onRequest.GetBool();
        if (!data.rows.TryPushBack(tableRow) || !data.files.TryPushBack(storedFile) ||
            !data.sizes.TryPushBack(sizeValue.Value()) ||
            !data.executable.TryPushBack(executable.GetBool()) ||
            !data.entries.TryPushBack(nullptr)) {
            platform::Fatal("ModuleManifest reserved row push failed");
        }
    }

    return true;
}

bool PrepareModuleBinaries(StringView directory, ModuleManifestData& data,
                           ModuleEntryFunction executableEntry, ModuleLoadReport& report) noexcept {
    Allocator& allocator = data.rows.GetAllocator();
    auto fail = [&](ModuleLoadCode code, StringView module, StringView stage) noexcept {
        report.code = code;
        WriteReportName(report, report.failed, module);
        WriteReportText(report, report.stage, stage);
        CloseModuleBinaries(data);
        return false;
    };

    for (usize index = 0; index < data.rows.Size(); ++index) {
        const StringView name = data.rows[index].name;
        if (data.executable[index] && executableEntry == nullptr) {
            report.error = ErrorCode::InvalidArgument;
            WriteReportText(
                report, report.message,
                "Module manifest has executable row {} but no executable entry was given", name);
            return fail(ModuleLoadCode::Incompatible, name, ManifestStage);
        }

        StringBuilder<> path(allocator);
        if (!path.TryAppend(directory) || !path.TryAppend("/") ||
            !path.TryAppend(StringView(data.files[index]))) {
            report.error = ErrorCode::OutOfMemory;
            WriteReportText(report, report.message,
                            "Out of memory while building the path of module {}", name);
            return fail(ModuleLoadCode::Incompatible, name, "Lookup");
        }

        platform::FileInfo info;
        const platform::FileStatus stat =
            platform::PhysicalFileSystem().Stat(path.CString(), path.View().ByteCount(), info);
        if (!stat) {
            report.nativeError = stat.nativeError;
            WriteReportText(report, report.message, "Module {} file {} cannot be inspected", name,
                            path.View());
            return fail(stat.code == platform::FileCode::NotFound ? ModuleLoadCode::NotFound
                                                                  : ModuleLoadCode::OsLoadFailed,
                        name, "Lookup");
        }

        if (info.size != data.sizes[index]) {
            WriteReportText(report, report.message,
                            "Module {} file {} is {} bytes but the manifest expects {} (the "
                            "manifest and the binaries come "
                            "from different builds)",
                            name, path.View(), info.size, data.sizes[index]);
            return fail(ModuleLoadCode::Incompatible, name, "Lookup");
        }

        const ModuleEntry* entry = nullptr;
        if (data.executable[index]) {
            entry = &executableEntry();
        } else {
            platform::DynamicLibrary library;
            platform::DynamicLibraryError error;
            const platform::FileStatus opened =
                library.Open(path.CString(), path.View().ByteCount(), &error);
            if (!opened) {
                report.nativeError = opened.nativeError;
                WriteReportText(report, report.message,
                                "Module {} library {} cannot be loaded (native error {}): {}", name,
                                path.View(), opened.nativeError, StringView(error.text));
                return fail(ModuleLoadCode::OsLoadFailed, name, "OsLoad");
            }

            void* address = nullptr;
            platform::DynamicLibraryLease lease;
            if (!library.Find(ExportedEntrySymbol, address, lease)) {
                WriteReportText(report, report.message, "Module {} library {} does not export {}",
                                name, path.View(), StringView(ExportedEntrySymbol));
                (void)library.Close();
                return fail(ModuleLoadCode::EntryMissing, name, "Entry");
            }

            entry = reinterpret_cast<ExportedEntryFunction>(address)();
            if (!data.libraries.TryPushBack(Move(library)) ||
                !data.leases.TryPushBack(Move(lease))) {
                platform::Fatal("ModuleManifest reserved library push failed");
            }
        }

        if (entry == nullptr || entry->entryVersion != ModuleEntryVersion ||
            entry->name == nullptr || entry->create == nullptr || StringView(entry->name) != name) {
            WriteReportText(
                report, report.message,
                "Module {} entry does not match the manifest (entry version or name differs)",
                name);
            return fail(ModuleLoadCode::Incompatible, name, "Entry");
        }

        data.entries[index] = entry;
    }

    return true;
}

} // namespace rocks::detail
