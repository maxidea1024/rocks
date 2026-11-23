#include "ModuleManifest.h"

#include "ModuleLoaderInternal.h"

#include <Core/Memory/UniquePtr.h>
#include <Core/Move.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>
#include <Platform/File.h>

namespace rocks::detail {

namespace {

// manifest 형식 판이다(Rocks.Build ModuleManifests.Schema와 같다).
constexpr int64 ManifestSchema = 4;
// buildId(GUID 32자 소문자 16진)와 툴체인 지문(SHA-256 64자 소문자 16진)의 길이다.
constexpr usize BuildIdLength = 32;
constexpr usize FingerprintLength = 64;
// 최상위 키 수·행 키 수·plugins 항목 키 수다(정확히 이 키들만 있어야 한다).
constexpr usize TopLevelKeyCount = 6;
constexpr usize RowKeyCount = 8;
constexpr usize PluginKeyCount = 2;
// 플러그인 manifest 경로의 앞부분과, 그 뒤 조각의 최소 수(<플러그인
// 폴더>/Binaries/<Platform>/<파일>)다.
constexpr StringView PluginManifestPrefix = "../Plugins/";
constexpr usize PluginManifestMinimumParts = 4;
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

// 플러그인 manifest 상대 경로가 "../Plugins/<조각>/…/Binaries/<조각>/<fileName>"
// 모양인지다(3.15절). 조각은 파일 이름 규칙(IsPlainFileName)을 따른다 — 첫 "../" 말고는 상위 폴더로
// 나가지 않는다.
bool IsPluginManifestPath(StringView path, StringView fileName) noexcept {
    if (!path.StartsWith(PluginManifestPrefix)) {
        return false;
    }

    StringView parts[PluginManifestMinimumParts]{};
    usize count = 0;
    usize start = PluginManifestPrefix.ByteCount();
    for (usize index = start; index <= path.ByteCount(); ++index) {
        if (index != path.ByteCount() && path.Data()[index] != '/') {
            continue;
        }

        const StringView part(path.Data() + start, index - start);
        if (!IsPlainFileName(part)) {
            return false;
        }

        // 마지막 조각 넷(…/Binaries/<Platform>/<파일>과 그 앞 하나)만 기억한다.
        for (usize shift = 1; shift < PluginManifestMinimumParts; ++shift) {
            parts[shift - 1] = parts[shift];
        }

        parts[PluginManifestMinimumParts - 1] = part;
        ++count;
        start = index + 1;
    }

    return count >= PluginManifestMinimumParts && parts[3] == fileName && parts[1] == "Binaries";
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

// 정렬 전 행 하나의 위치다(어느 manifest의 몇째 행, 그 order).
struct RowLocation {
    usize document = 0;
    usize row = 0;
    uint64 order = 0;
};

// 해석 중 저장소 크기 합이다.
struct ManifestTotals {
    usize rows = 0;
    usize textBytes = 0;
    usize dependencies = 0;
};

// manifest 하나의 최상위 키와 머리 값(schema·buildId·engineVersion·toolchainFingerprint) 모양을
// 검사한다. 실행 파일 manifest는 플러그인 manifest를 열기 전에 이것부터 본다(3.15절 — 실행 파일
// manifest의 오류가 플러그인 파일 오류에 가려지지 않게).
bool CheckHeader(JsonValue root, ModuleLoadReport& report) noexcept {
    constexpr StringView topKeys[] = {"schema",  "buildId", "engineVersion", "toolchainFingerprint",
                                      "plugins", "modules"};
    if (!HasExactKeys(root, TopLevelKeyCount, topKeys)) {
        return Reject(report,
                      "Module manifest must be an object with exactly the keys schema, buildId, "
                      "engineVersion, toolchainFingerprint, plugins and modules");
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

    return true;
}

// manifest 하나(document번째)의 머리·plugins·행 모양을 검사하고 크기 합을 더한다. 플러그인
// manifest는 머리 값을 실행 파일 manifest(executable)와 대조한다. 행의 값 검사(이름·파일·의존)는
// 정렬 뒤에 한다.
bool CheckDocument(JsonValue root, JsonValue executable, Span<const ModuleManifestText> manifests,
                   usize document, ManifestTotals& totals, ModuleLoadReport& report) noexcept {
    if (!CheckHeader(root, report)) {
        return false;
    }

    // 플러그인 manifest는 실행 파일 manifest와 한 빌드여야 한다(UE는 BuildId가 다른 .modules를
    // 건너뛰고, Rocks는 실패한다 — MD13).
    const StringView plugin = manifests[document].plugin;
    if (document != 0) {
        constexpr StringView sameKeys[] = {"buildId", "engineVersion", "toolchainFingerprint"};
        for (StringView key : sameKeys) {
            const StringView value = root.Find(key).GetString();
            const StringView expected = executable.Find(key).GetString();
            if (value != expected) {
                return Reject(report,
                              "Plugin manifest {} {} '{}' differs from the executable manifest "
                              "'{}' (the plugin and the executable come from different builds)",
                              plugin, key, value, expected);
            }
        }
    }

    // 실행 파일 manifest의 plugins는 읽은 플러그인 manifest와 개수·이름이 같다. 플러그인 manifest는
    // 다른 manifest를 가리키지 않는다.
    const JsonValue plugins = root.Find("plugins");
    if (!plugins.IsArray()) {
        return Reject(report, "Module manifest plugins must be an array");
    }

    if (document != 0 && plugins.Size() != 0) {
        return Reject(report, "Plugin manifest {} must have an empty plugins list", plugin);
    }

    if (document == 0) {
        if (plugins.Size() != manifests.Size() - 1) {
            return Reject(report, "Module manifest lists {} plugin manifests but {} were read",
                          plugins.Size(), manifests.Size() - 1);
        }

        for (usize index = 0; index < plugins.Size(); ++index) {
            const JsonValue name =
                plugins.At(index).IsObject() ? plugins.At(index).Find("name") : JsonValue();
            if (!name.IsString() || name.GetString() != manifests[index + 1].plugin) {
                return Reject(report, "Module manifest plugins[{}] must name plugin {}", index,
                              manifests[index + 1].plugin);
            }
        }
    }

    const JsonValue modules = root.Find("modules");
    if (!modules.IsArray() || modules.Size() == 0) {
        return Reject(report, "Module manifest modules must be a non-empty array");
    }

    constexpr StringView rowKeys[] = {"name",         "order",        "file",      "size",
                                      "dependencies", "loadingPhase", "onRequest", "host"};
    uint64 previous = 0;
    for (usize index = 0; index < modules.Size(); ++index) {
        const JsonValue row = modules.At(index);
        if (!HasExactKeys(row, RowKeyCount, rowKeys)) {
            return Reject(report,
                          "Module manifest modules[{}] must have exactly the keys name, order, "
                          "file, size, dependencies, loadingPhase, onRequest and host",
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

        // order는 manifest 안에서 커지는 순서다(빌드가 Monolithic 표 순서로 매겨 나눈다).
        const JsonValue order = row.Find("order");
        auto orderValue = order.IsNumber() ? order.TryGetUInt64()
                                           : Result<uint64>::Failure(ErrorCode::InvalidArgument);
        if (!orderValue || (index != 0 && orderValue.Value() <= previous)) {
            return Reject(report,
                          "Module manifest modules[{}] order must be a non-negative integer "
                          "greater than the previous row's order",
                          index);
        }

        previous = orderValue.Value();
        const JsonValue host = row.Find("host");
        if (document != 0 && host.IsBool() && host.GetBool()) {
            return Reject(report, "Plugin manifest {} must not have a host row ({})", plugin,
                          name.GetString());
        }

        totals.textBytes += name.GetString().ByteCount() + 1 + file.GetString().ByteCount() + 1;
        totals.dependencies += dependencies.Size();
    }

    totals.rows += modules.Size();
    totals.textBytes += manifests[document].directory.ByteCount() + 1 + plugin.ByteCount() + 1;
    return true;
}

} // namespace

ModuleManifestData::ModuleManifestData(Allocator& allocator) noexcept
    : rows(allocator), entries(allocator), files(allocator), sizes(allocator), host(allocator),
      directories(allocator), plugins(allocator), text(allocator), dependencyNames(allocator),
      libraries(allocator), leases(allocator) {
}

ModuleManifestData::~ModuleManifestData() noexcept {
    CloseModuleBinaries(*this);
}

ModuleManifestPlugins::ModuleManifestPlugins(Allocator& allocator) noexcept
    : text(allocator), names(allocator), paths(allocator) {
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

bool ListModuleManifestPlugins(StringView text, StringView fileName, ModuleManifestPlugins& plugins,
                               ModuleLoadReport& report) noexcept {
    Allocator& allocator = plugins.text.GetAllocator();
    JsonDocument document(allocator);
    const JsonError parsed = document.Parse(text);
    if (!parsed.Succeeded()) {
        if (parsed.code == JsonErrorCode::OutOfMemory) {
            return RejectOutOfMemory(report);
        }

        return Reject(report, "Module manifest is not valid JSON at {}:{}: {}",
                      parsed.position.line, parsed.position.column, JsonErrorMessage(parsed.code));
    }

    // 머리를 먼저 본다 — 깨진 실행 파일 manifest가 가리키는 플러그인 파일을 열지 않는다.
    const JsonValue root = document.Root();
    if (!CheckHeader(root, report)) {
        return false;
    }

    const JsonValue list = root.Find("plugins");
    if (!list.IsArray()) {
        return Reject(report, "Module manifest plugins must be an array");
    }

    usize bytes = 0;
    constexpr StringView keys[] = {"name", "manifest"};
    for (usize index = 0; index < list.Size(); ++index) {
        const JsonValue entry = list.At(index);
        if (!HasExactKeys(entry, PluginKeyCount, keys) || !entry.Find("name").IsString() ||
            !entry.Find("manifest").IsString()) {
            return Reject(report,
                          "Module manifest plugins[{}] must be an object with exactly the string "
                          "keys name and manifest",
                          index);
        }

        const StringView name = entry.Find("name").GetString();
        const StringView path = entry.Find("manifest").GetString();
        if (!IsModuleIdentifier(name)) {
            return Reject(report, "Module manifest plugins[{}] name '{}' is not a C identifier",
                          index, name);
        }

        for (usize earlier = 0; earlier < index; ++earlier) {
            if (list.At(earlier).Find("name").GetString() == name) {
                return Reject(report, "Module manifest lists plugin {} twice", name);
            }
        }

        if (!IsPluginManifestPath(path, fileName)) {
            return Reject(report,
                          "Module manifest plugin {} manifest '{}' must be "
                          "../Plugins/<folder>/Binaries/<platform>/{}",
                          name, path, fileName);
        }

        bytes += name.ByteCount() + 1 + path.ByteCount() + 1;
    }

    if (!plugins.text.TryReserve(bytes) || !plugins.names.TryReserve(list.Size()) ||
        !plugins.paths.TryReserve(list.Size())) {
        return RejectOutOfMemory(report);
    }

    for (usize index = 0; index < list.Size(); ++index) {
        const usize name = plugins.text.Size();
        const char* storedName = Store(plugins.text, list.At(index).Find("name").GetString());
        const usize path = plugins.text.Size();
        const char* storedPath = Store(plugins.text, list.At(index).Find("manifest").GetString());
        if (storedName == nullptr || storedPath == nullptr || !plugins.names.TryPushBack(name) ||
            !plugins.paths.TryPushBack(path)) {
            platform::Fatal("ModuleManifest reserved plugin push failed");
        }
    }

    return true;
}

bool ParseModuleManifests(Span<const ModuleManifestText> manifests, ModuleManifestData& data,
                          ModuleLoadReport& report) noexcept {
    if (manifests.Size() == 0) {
        platform::Fatal("ParseModuleManifests needs the executable manifest");
    }

    Allocator& allocator = data.rows.GetAllocator();
    // 플러그인 manifest의 실패는 requested를 그 플러그인 이름으로 쓴다(manifest 파일 이름이 모두
    // 같다 — 3.15절).
    auto owner = [&](usize document) noexcept {
        if (document != 0) {
            WriteReportName(report, report.requested, manifests[document].plugin);
        }
    };

    // 1단계: 모든 manifest를 해석하고 모양을 검사하며 저장소 크기를 센다.
    Array<UniquePtr<JsonDocument>> documents(allocator);
    if (!documents.TryReserve(manifests.Size())) {
        return RejectOutOfMemory(report);
    }

    ManifestTotals totals;
    for (usize document = 0; document < manifests.Size(); ++document) {
        auto made = UniquePtr<JsonDocument>::TryMake(allocator, allocator);
        if (!made) {
            owner(document);
            return RejectOutOfMemory(report);
        }

        if (!documents.TryPushBack(Move(made.Value()))) {
            platform::Fatal("ModuleManifest reserved document push failed");
        }

        const JsonError parsed = documents[document]->Parse(manifests[document].text);
        if (!parsed.Succeeded()) {
            owner(document);
            if (parsed.code == JsonErrorCode::OutOfMemory) {
                return RejectOutOfMemory(report);
            }

            return Reject(report, "Module manifest is not valid JSON at {}:{}: {}",
                          parsed.position.line, parsed.position.column,
                          JsonErrorMessage(parsed.code));
        }

        if (!CheckDocument(documents[document]->Root(), documents[0]->Root(), manifests, document,
                           totals, report)) {
            owner(document);
            return false;
        }
    }

    // 2단계: 행을 order로 정렬한다(삽입 정렬 — manifest마다 이미 커지는 순서다). order는 모두
    // 다르다.
    Array<RowLocation> order(allocator);
    if (!order.TryReserve(totals.rows)) {
        return RejectOutOfMemory(report);
    }

    for (usize document = 0; document < manifests.Size(); ++document) {
        const JsonValue modules = documents[document]->Root().Find("modules");
        for (usize index = 0; index < modules.Size(); ++index) {
            RowLocation location;
            location.document = document;
            location.row = index;
            location.order = modules.At(index).Find("order").TryGetUInt64().Value();
            if (!order.TryPushBack(location)) {
                platform::Fatal("ModuleManifest reserved order push failed");
            }

            for (usize slot = order.Size() - 1; slot != 0; --slot) {
                if (order[slot - 1].order < order[slot].order) {
                    break;
                }

                if (order[slot - 1].order == order[slot].order) {
                    owner(document);
                    return Reject(report, "Module manifest order {} is used by two modules",
                                  order[slot].order);
                }

                const RowLocation moved = order[slot];
                order[slot] = order[slot - 1];
                order[slot - 1] = moved;
            }
        }
    }

    // 3단계: 저장소를 정확한 크기로 한 번 잡고(주소가 고정되어 행이 가리킬 수 있다) 정렬한 순서로
    // 행을 검사하며 담는다. 의존은 order가 더 작은 행(이미 담은 행)이어야 한다.
    const usize count = totals.rows;
    if (!data.rows.TryReserve(count) || !data.entries.TryReserve(count) ||
        !data.files.TryReserve(count) || !data.sizes.TryReserve(count) ||
        !data.host.TryReserve(count) || !data.directories.TryReserve(count) ||
        !data.plugins.TryReserve(count) || !data.text.TryReserve(totals.textBytes) ||
        !data.dependencyNames.TryReserve(totals.dependencies) ||
        !data.libraries.TryReserve(count) || !data.leases.TryReserve(count)) {
        return RejectOutOfMemory(report);
    }

    Array<const char*> folders(allocator);
    Array<const char*> owners(allocator);
    if (!folders.TryReserve(manifests.Size()) || !owners.TryReserve(manifests.Size())) {
        return RejectOutOfMemory(report);
    }

    for (const ModuleManifestText& manifest : manifests) {
        const char* folder = Store(data.text, manifest.directory);
        const char* plugin = Store(data.text, manifest.plugin);
        if (folder == nullptr || plugin == nullptr || !folders.TryPushBack(folder) ||
            !owners.TryPushBack(plugin)) {
            platform::Fatal("ModuleManifest reserved manifest push failed");
        }
    }

    usize hostRows = 0;
    for (const RowLocation& location : order) {
        const usize index = location.row;
        const JsonValue row = documents[location.document]->Root().Find("modules").At(index);
        const StringView name = row.Find("name").GetString();
        const StringView file = row.Find("file").GetString();
        auto reject = [&](auto... args) noexcept {
            owner(location.document);
            return Reject(report, args...);
        };
        if (!IsModuleIdentifier(name)) {
            return reject("Module manifest modules[{}] name '{}' is not a C identifier", index,
                          name);
        }

        for (const ModuleTableRow& earlier : data.rows) {
            if (StringView(earlier.name) == name) {
                return reject("Module manifest lists module {} twice", name);
            }
        }

        if (!IsPlainFileName(file)) {
            return reject("Module manifest module {} file '{}' must be a plain file name", name,
                          file);
        }

        const JsonValue size = row.Find("size");
        auto sizeValue = size.IsNumber() ? size.TryGetUInt64()
                                         : Result<uint64>::Failure(ErrorCode::InvalidArgument);
        if (!sizeValue) {
            return reject("Module manifest module {} size must be a non-negative integer", name);
        }

        const JsonValue phaseName = row.Find("loadingPhase");
        LoadingPhase phase = LoadingPhase::Default;
        if (!phaseName.IsString() || !ParsePhase(phaseName.GetString(), phase)) {
            return reject("Module manifest module {} loadingPhase must be a loading phase name",
                          name);
        }

        const JsonValue onRequest = row.Find("onRequest");
        const JsonValue host = row.Find("host");
        if (!onRequest.IsBool() || !host.IsBool()) {
            return reject("Module manifest module {} onRequest and host must be true or false",
                          name);
        }

        if (host.GetBool() && ++hostRows > 1) {
            return reject("Module manifest has more than one host row ({} is the second)", name);
        }

        const char* storedName = Store(data.text, name);
        const char* storedFile = Store(data.text, file);
        if (storedName == nullptr || storedFile == nullptr) {
            platform::Fatal("ModuleManifest reserved text push failed");
        }

        // 의존은 order가 더 작은 행 이름이다(표는 의존 순서 — 순환도 여기서 걸린다). 저장한 앞 행
        // 이름을 가리킨다.
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
                return reject("Module manifest module {} dependency #{} must name an earlier "
                              "module (a smaller order)",
                              name, dependency);
            }

            for (usize seen = dependencyStart; seen < data.dependencyNames.Size(); ++seen) {
                if (data.dependencyNames[seen] == resolved) {
                    return reject("Module manifest module {} lists dependency {} twice", name,
                                  StringView(resolved));
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
            !data.sizes.TryPushBack(sizeValue.Value()) || !data.host.TryPushBack(host.GetBool()) ||
            !data.entries.TryPushBack(nullptr) ||
            !data.directories.TryPushBack(folders[location.document]) ||
            !data.plugins.TryPushBack(location.document == 0 ? nullptr
                                                             : owners[location.document])) {
            platform::Fatal("ModuleManifest reserved row push failed");
        }
    }

    return true;
}

bool ParseModuleManifest(StringView text, ModuleManifestData& data,
                         ModuleLoadReport& report) noexcept {
    const ModuleManifestText manifest{text, StringView(), StringView()};
    return ParseModuleManifests(Span<const ModuleManifestText>(&manifest, 1), data, report);
}

bool PrepareModuleBinaries(ModuleManifestData& data, ModuleLoadReport& report) noexcept {
    Allocator& allocator = data.rows.GetAllocator();
    auto fail = [&](ModuleLoadCode code, usize index, StringView stage) noexcept {
        report.code = code;
        WriteReportName(report, report.failed, StringView(data.rows[index].name));
        WriteReportText(report, report.stage, stage);
        // 플러그인 행이면 requested는 플러그인 이름이다(manifest 파일 이름이 모두 같다 — 3.15절).
        if (data.plugins[index] != nullptr) {
            WriteReportName(report, report.requested, StringView(data.plugins[index]));
        }

        CloseModuleBinaries(data);
        return false;
    };

    for (usize index = 0; index < data.rows.Size(); ++index) {
        const StringView name = data.rows[index].name;

        StringBuilder<> path(allocator);
        if (!path.TryAppend(StringView(data.directories[index])) || !path.TryAppend("/") ||
            !path.TryAppend(StringView(data.files[index]))) {
            report.error = ErrorCode::OutOfMemory;
            WriteReportText(report, report.message,
                            "Out of memory while building the path of module {}", name);
            return fail(ModuleLoadCode::Incompatible, index, "Lookup");
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
                        index, "Lookup");
        }

        if (info.size != data.sizes[index]) {
            WriteReportText(report, report.message,
                            "Module {} file {} is {} bytes but the manifest expects {} (the "
                            "manifest and the binaries come "
                            "from different builds)",
                            name, path.View(), info.size, data.sizes[index]);
            return fail(ModuleLoadCode::Incompatible, index, "Lookup");
        }

        // 다른 폴더(플러그인 binaries 폴더)의 라이브러리도 그 import는 앞서 연 의존 라이브러리로
        // 풀린다(Windows는 같은 이름의 적재된 모듈, macOS·Linux는 빌드가 넣은 rpath — 3.15절).
        platform::DynamicLibrary library;
        platform::DynamicLibraryError error;
        const platform::FileStatus opened =
            library.Open(path.CString(), path.View().ByteCount(), &error);
        if (!opened) {
            report.nativeError = opened.nativeError;
            WriteReportText(report, report.message,
                            "Module {} library {} cannot be loaded (native error {}): {}", name,
                            path.View(), opened.nativeError, StringView(error.text));
            return fail(ModuleLoadCode::OsLoadFailed, index, "OsLoad");
        }

        void* address = nullptr;
        platform::DynamicLibraryLease lease;
        if (!library.Find(ExportedEntrySymbol, address, lease)) {
            WriteReportText(report, report.message, "Module {} library {} does not export {}", name,
                            path.View(), StringView(ExportedEntrySymbol));
            (void)library.Close();
            return fail(ModuleLoadCode::EntryMissing, index, "Entry");
        }

        const ModuleEntry* entry = reinterpret_cast<ExportedEntryFunction>(address)();
        if (!data.libraries.TryPushBack(Move(library)) || !data.leases.TryPushBack(Move(lease))) {
            platform::Fatal("ModuleManifest reserved library push failed");
        }

        if (entry == nullptr || entry->entryVersion != ModuleEntryVersion ||
            entry->name == nullptr || entry->create == nullptr || StringView(entry->name) != name) {
            WriteReportText(
                report, report.message,
                "Module {} entry does not match the manifest (entry version or name differs)",
                name);
            return fail(ModuleLoadCode::Incompatible, index, "Entry");
        }

        data.entries[index] = entry;
    }

    return true;
}

} // namespace rocks::detail
