#include "ExchangeInternal.h"

#include <Core/Algorithms.h>
#include <Core/FileError.h>
#include <Platform/Memory.h>
#include <UnrealExchange/ExchangePaths.h>

// Unreal 교환 묶음 리더다(Docs/Design/UnrealExchange.ko.md 3절, 10.4절 2–12·16·17번). bundle.json을
// 묶음 수준으로, 에셋 JSON과 blob을 에셋 수준으로 검증한다.

namespace rocks {

namespace {

constexpr StringView BundleManifestFile = "bundle.json";
constexpr StringView AssetFilePrefix = "assets";
constexpr StringView AssetFileSuffix = ".json";
constexpr StringView BlobKey = "blob";

// 3.2절 비유한값 문자열이 뜻하는 double 비트다(quiet NaN, +Inf, -Inf).
constexpr uint64 ExchangeQuietNaNBits = 0x7ff8000000000000ull;
constexpr uint64 ExchangeInfinityBits = 0x7ff0000000000000ull;
constexpr uint64 ExchangeNegativeInfinityBits = 0xfff0000000000000ull;

constexpr StringView BundleKeys[] = {"formatVersion", "exporter", "projectSettings", "environment",
                                     "createdUtc",    "assets",   "redirects"};
constexpr StringView ExporterKeys[] = {"plugin", "pluginVersion", "engineVersion", "engineCommit"};
constexpr StringView ProjectSettingsKeys[] = {"substrate", "retainJpegFormat"};
constexpr StringView ManifestAssetKeys[] = {"objectPath", "class", "file"};
constexpr StringView RedirectKeys[] = {"from", "to"};
constexpr StringView AssetKeys[] = {"objectPath",  "class",    "ueClass", "dependencies",
                                    "diagnostics", "settings", "data"};
constexpr StringView DependencyKeys[] = {"objectPath", "class", "kind"};
constexpr StringView ExporterDiagnosticKeys[] = {"code", "severity", "message"};
constexpr StringView ExporterSeverities[] = {"error", "warning", "info"};
constexpr StringView BlobKeys[] = {"blob", "bytes", "layout", "components", "count"};

struct ExchangeClassEntry {
    ExchangeClass type;
    StringView name;
};

constexpr ExchangeClassEntry ExchangeClassTable[] = {
    {ExchangeClass::Texture2D, "Texture2D"},
    {ExchangeClass::TextureCube, "TextureCube"},
    {ExchangeClass::Texture2DArray, "Texture2DArray"},
    {ExchangeClass::TextureCubeArray, "TextureCubeArray"},
    {ExchangeClass::VolumeTexture, "VolumeTexture"},
    {ExchangeClass::StaticMesh, "StaticMesh"},
    {ExchangeClass::Skeleton, "Skeleton"},
    {ExchangeClass::SkeletalMesh, "SkeletalMesh"},
    {ExchangeClass::AnimSequence, "AnimSequence"},
    {ExchangeClass::Material, "Material"},
    {ExchangeClass::MaterialInstanceConstant, "MaterialInstanceConstant"},
    {ExchangeClass::MaterialFunction, "MaterialFunction"},
    {ExchangeClass::MaterialFunctionInstance, "MaterialFunctionInstance"},
    {ExchangeClass::MaterialParameterCollection, "MaterialParameterCollection"},
};

constexpr ExchangeBlobLayout ExchangeBlobLayouts[] = {
    ExchangeBlobLayout::U8,        ExchangeBlobLayout::U16,   ExchangeBlobLayout::U32,
    ExchangeBlobLayout::I32,       ExchangeBlobLayout::F16,   ExchangeBlobLayout::F32,
    ExchangeBlobLayout::F64,       ExchangeBlobLayout::Names, ExchangeBlobLayout::BoneInfluences,
    ExchangeBlobLayout::SparseVec3};

// 진단을 낼 파일·에셋과 마지막 거부 오류다.
struct ReadScope {
    ExchangeDiagnostics& diagnostics;
    ExchangeFile file = ExchangeFile::None;
    uint32 asset = ExchangeNoIndex;
    ErrorCode rejected = ErrorCode::None;
};

// 진단을 더하고 거부(false)를 돌려준다. 할당 실패는 Failure다.
Result<bool> Reject(ReadScope& scope, AssetDiagnosticCode code, ErrorCode error,
                    ExchangeIssue issue, JsonPosition position, StringView text) noexcept {
    ExchangeDiagnosticInput input;
    input.code = code;
    input.error = error;
    input.issue = issue;
    input.file = scope.file;
    input.asset = scope.asset;
    input.position = position;
    input.text = text;
    if (auto added = scope.diagnostics.TryAdd(input); !added) {
        return Result<bool>::Failure(added.Error());
    }

    scope.rejected = error;
    return Result<bool>::Success(false);
}

Result<bool> Corrupt(ReadScope& scope, ExchangeIssue issue, JsonPosition position,
                     StringView text) noexcept {
    return Reject(scope, AssetDiagnosticCode::CorruptBundle, ErrorCode::InvalidArgument, issue,
                  position, text);
}

Result<bool> OverLimit(ReadScope& scope, JsonPosition position, StringView text) noexcept {
    return Reject(scope, AssetDiagnosticCode::CorruptBundle, ErrorCode::CapacityExceeded,
                  ExchangeIssue::LimitExceeded, position, text);
}

Result<bool> Accept() noexcept {
    return Result<bool>::Success(true);
}

// 거부·실패면 참이다(호출자는 그대로 돌려준다).
bool Stopped(const Result<bool>& result) noexcept {
    return !result || !result.Value();
}

// 고정 스키마 객체의 모르는 키마다 UnknownField 경고를 낸다.
Result<bool> WarnUnknownKeys(ReadScope& scope, JsonValue object,
                             Span<const StringView> known) noexcept {
    for (usize index = 0; index < object.Size(); ++index) {
        const StringView key = object.KeyAt(index);
        bool found = false;
        for (const StringView& name : known) {
            found = found || key == name;
        }

        if (found) {
            continue;
        }

        ExchangeDiagnosticInput input;
        input.code = AssetDiagnosticCode::UnknownField;
        input.issue = ExchangeIssue::UnknownKey;
        input.file = scope.file;
        input.asset = scope.asset;
        input.position = object.ValueAt(index).Position();
        input.text = key;
        if (auto added = scope.diagnostics.TryAdd(input); !added) {
            return Result<bool>::Failure(added.Error());
        }
    }

    return Accept();
}

// object의 key 값이 type이어야 한다(없으면 MissingKey, 다르면 WrongType).
Result<bool> RequireValue(ReadScope& scope, JsonValue object, StringView key, JsonType type,
                          JsonValue& out) noexcept {
    out = object.Find(key);
    if (!out.IsValid()) {
        return Corrupt(scope, ExchangeIssue::MissingKey, object.Position(), key);
    }

    if (out.Type() != type) {
        return Corrupt(scope, ExchangeIssue::WrongType, out.Position(), key);
    }

    return Accept();
}

Result<bool> RequireString(ReadScope& scope, JsonValue object, StringView key,
                           StringView& out) noexcept {
    JsonValue value;
    auto required = RequireValue(scope, object, key, JsonType::String, value);
    if (Stopped(required)) {
        return required;
    }

    out = value.GetString();
    return Accept();
}

Result<bool> RequireBool(ReadScope& scope, JsonValue object, StringView key, bool& out) noexcept {
    JsonValue value;
    auto required = RequireValue(scope, object, key, JsonType::Bool, value);
    if (Stopped(required)) {
        return required;
    }

    out = value.GetBool();
    return Accept();
}

// 부호 없는 JSON 정수 키다. 정수가 아니면(실수·문자열 등) WrongType, 음수는 InvalidValue, limit을
// 넘으면 LimitExceeded다.
Result<bool> RequireUnsigned(ReadScope& scope, JsonValue object, StringView key, uint64 limit,
                             uint64& out) noexcept {
    const JsonValue value = object.Find(key);
    if (!value.IsValid()) {
        return Corrupt(scope, ExchangeIssue::MissingKey, object.Position(), key);
    }

    if (value.Type() != JsonType::Integer && value.Type() != JsonType::Unsigned) {
        return Corrupt(scope, ExchangeIssue::WrongType, value.Position(), key);
    }

    auto number = value.TryGetUInt64();
    if (!number) {
        return Corrupt(scope, ExchangeIssue::InvalidValue, value.Position(), key);
    }

    if (number.Value() > limit) {
        return OverLimit(scope, value.Position(), key);
    }

    out = number.Value();
    return Accept();
}

bool IsLess(StringView left, StringView right) noexcept {
    return StringView::CompareBytes(left, right) < 0;
}

// JSON 파일 하나를 읽어 document에 파싱한다. 루트는 객체여야 한다.
Result<bool> ReadJsonFile(platform::FileSystem& files, StringView directory, StringView relative,
                          Allocator& allocator, ReadScope& scope, JsonDocument& document) noexcept {
    StringBuilder<512> path(allocator);
    if (auto made = exchange_detail::AppendBundlePath(directory, relative, path); !made) {
        return Result<bool>::Failure(made.Error());
    }

    platform::File file;
    if (const platform::FileStatus opened = file.Open(files, path.CString(), path.ByteCount(),
                                                      platform::FileOpenMode::ReadExisting);
        !opened) {
        return Reject(scope, AssetDiagnosticCode::CorruptBundle, FileErrorCode(opened),
                      ExchangeIssue::ReadFailed, {}, relative);
    }

    uint64 size = 0;
    if (const platform::FileStatus sized = file.Size(size); !sized) {
        return Reject(scope, AssetDiagnosticCode::CorruptBundle, FileErrorCode(sized),
                      ExchangeIssue::ReadFailed, {}, relative);
    }

    if (size > UnrealExchangeMaxJsonBytes) {
        return OverLimit(scope, {}, relative);
    }

    Array<char> text(allocator);
    if (auto resized = text.TryResize(static_cast<usize>(size)); !resized) {
        return Result<bool>::Failure(resized.Error());
    }

    if (size != 0) {
        const platform::FileTransfer read = file.ReadExact(text.Data(), text.Size());
        if (!read.status) {
            return Reject(scope, AssetDiagnosticCode::CorruptBundle, FileErrorCode(read.status),
                          ExchangeIssue::ReadFailed, {}, relative);
        }
    }

    JsonReadOptions options;
    options.maxInputBytes = UnrealExchangeMaxJsonBytes;
    const JsonError parsed = document.Parse(StringView(text.Data(), text.Size()), options);
    if (parsed.code == JsonErrorCode::OutOfMemory) {
        return Result<bool>::Failure(ErrorCode::OutOfMemory);
    }

    if (!parsed.Succeeded()) {
        return Corrupt(scope, ExchangeIssue::JsonSyntax, parsed.position,
                       JsonErrorMessage(parsed.code));
    }

    if (!document.Warnings().IsEmpty()) {
        return Corrupt(scope, ExchangeIssue::ByteOrderMark, document.Warnings()[0].position,
                       relative);
    }

    if (!document.Root().IsObject()) {
        return Corrupt(scope, ExchangeIssue::WrongType, document.Root().Position(), relative);
    }

    return Accept();
}

// objectPath 문자열 키를 읽고 문법을 확인한다.
Result<bool> RequireObjectPath(ReadScope& scope, JsonValue object, StringView key,
                               StringView& out) noexcept {
    auto read = RequireString(scope, object, key, out);
    if (Stopped(read)) {
        return read;
    }

    if (!IsValidUnrealObjectPath(out)) {
        return Corrupt(scope, ExchangeIssue::ObjectPathSyntax, object.Find(key).Position(), out);
    }

    return Accept();
}

// 고정 스키마 하위 객체(exporter·projectSettings)를 읽는다.
Result<bool> RequireObject(ReadScope& scope, JsonValue object, StringView key,
                           Span<const StringView> known, JsonValue& out) noexcept {
    auto required = RequireValue(scope, object, key, JsonType::Object, out);
    if (Stopped(required)) {
        return required;
    }

    return WarnUnknownKeys(scope, out, known);
}

Result<bool> ReadExporter(ReadScope& scope, JsonValue root, ExchangeExporter& out) noexcept {
    JsonValue exporter;
    auto read =
        RequireObject(scope, root, "exporter", Span<const StringView>(ExporterKeys), exporter);
    if (Stopped(read)) {
        return read;
    }

    StringView* const fields[] = {&out.plugin, &out.pluginVersion, &out.engineVersion,
                                  &out.engineCommit};
    for (usize index = 0; index < sizeof(fields) / sizeof(fields[0]); ++index) {
        read = RequireString(scope, exporter, ExporterKeys[index], *fields[index]);
        if (Stopped(read)) {
            return read;
        }
    }

    return Accept();
}

Result<bool> ReadProjectSettings(ReadScope& scope, JsonValue root, ExchangeBundle& out) noexcept {
    JsonValue settings;
    auto read = RequireObject(scope, root, "projectSettings",
                              Span<const StringView>(ProjectSettingsKeys), settings);
    if (Stopped(read)) {
        return read;
    }

    read = RequireBool(scope, settings, "substrate", out.substrate);
    if (Stopped(read)) {
        return read;
    }

    return RequireBool(scope, settings, "retainJpegFormat", out.retainJpegFormat);
}

// 선택 키(environment·createdUtc)다. 있으면 타입만 본다.
Result<bool> ReadInformation(ReadScope& scope, JsonValue root, ExchangeBundle& out) noexcept {
    const JsonValue environment = root.Find("environment");
    if (environment.IsValid()) {
        if (!environment.IsObject()) {
            return Corrupt(scope, ExchangeIssue::WrongType, environment.Position(), "environment");
        }

        out.environment = environment;
    }

    const JsonValue created = root.Find("createdUtc");
    if (created.IsValid()) {
        if (!created.IsString()) {
            return Corrupt(scope, ExchangeIssue::WrongType, created.Position(), "createdUtc");
        }

        out.createdUtc = created.GetString();
    }

    return Accept();
}

// 목록 항목의 file이 assets + 패키지 경로 + .json과 같은지 본다.
bool IsExpectedAssetFile(StringView objectPath, StringView file) noexcept {
    const StringView package = UnrealPackagePath(objectPath);
    const usize expected =
        AssetFilePrefix.ByteCount() + package.ByteCount() + AssetFileSuffix.ByteCount();
    if (file.ByteCount() != expected) {
        return false;
    }

    return StringView(file.Data(), AssetFilePrefix.ByteCount()) == AssetFilePrefix &&
           StringView(file.Data() + AssetFilePrefix.ByteCount(), package.ByteCount()) == package &&
           StringView(file.Data() + AssetFilePrefix.ByteCount() + package.ByteCount(),
                      AssetFileSuffix.ByteCount()) == AssetFileSuffix;
}

Result<bool> ReadManifestAssets(ReadScope& scope, JsonValue root, Allocator& allocator,
                                Array<ExchangeAsset>& assets) noexcept {
    JsonValue list;
    auto read = RequireValue(scope, root, "assets", JsonType::Array, list);
    if (Stopped(read)) {
        return read;
    }

    if (list.Size() > UnrealExchangeMaxAssets) {
        return OverLimit(scope, list.Position(), "assets");
    }

    for (usize index = 0; index < list.Size(); ++index) {
        const JsonValue item = list.At(index);
        if (!item.IsObject()) {
            return Corrupt(scope, ExchangeIssue::WrongType, item.Position(), "assets");
        }

        read = WarnUnknownKeys(scope, item, Span<const StringView>(ManifestAssetKeys));
        if (Stopped(read)) {
            return read;
        }

        ExchangeAsset asset(allocator);
        read = RequireObjectPath(scope, item, "objectPath", asset.objectPath);
        if (Stopped(read)) {
            return read;
        }

        read = RequireString(scope, item, "class", asset.className);
        if (Stopped(read)) {
            return read;
        }

        read = RequireString(scope, item, "file", asset.file);
        if (Stopped(read)) {
            return read;
        }

        if (!IsExpectedAssetFile(asset.objectPath, asset.file)) {
            return Corrupt(scope, ExchangeIssue::FileNameMismatch, item.Find("file").Position(),
                           asset.file);
        }

        if (!assets.IsEmpty() && !IsLess(assets[assets.Size() - 1].objectPath, asset.objectPath)) {
            return Corrupt(scope, ExchangeIssue::NotSorted, item.Position(), asset.objectPath);
        }

        asset.exchangeClass = FindExchangeClass(asset.className);
        if (auto pushed = assets.TryPushBack(Move(asset)); !pushed) {
            return Result<bool>::Failure(pushed.Error());
        }
    }

    return Accept();
}

// objectPath 순 에셋 목록에서 이진 탐색한다.
uint32 SearchAssets(Span<const ExchangeAsset> assets, StringView objectPath) noexcept {
    usize low = 0;
    usize high = assets.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const int order = StringView::CompareBytes(assets[middle].objectPath, objectPath);
        if (order == 0) {
            return static_cast<uint32>(middle);
        }

        if (order < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return ExchangeNoIndex;
}

// from 순 redirect 목록에서 이진 탐색한다.
uint32 SearchRedirects(Span<const ExchangeRedirect> redirects, StringView from) noexcept {
    usize low = 0;
    usize high = redirects.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const int order = StringView::CompareBytes(redirects[middle].from, from);
        if (order == 0) {
            return static_cast<uint32>(middle);
        }

        if (order < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return ExchangeNoIndex;
}

Result<bool> ReadRedirects(ReadScope& scope, JsonValue root, Span<const ExchangeAsset> assets,
                           Array<ExchangeRedirect>& redirects) noexcept {
    JsonValue list;
    auto read = RequireValue(scope, root, "redirects", JsonType::Array, list);
    if (Stopped(read)) {
        return read;
    }

    if (list.Size() > UnrealExchangeMaxRedirects) {
        return OverLimit(scope, list.Position(), "redirects");
    }

    for (usize index = 0; index < list.Size(); ++index) {
        const JsonValue item = list.At(index);
        if (!item.IsObject()) {
            return Corrupt(scope, ExchangeIssue::WrongType, item.Position(), "redirects");
        }

        read = WarnUnknownKeys(scope, item, Span<const StringView>(RedirectKeys));
        if (Stopped(read)) {
            return read;
        }

        ExchangeRedirect redirect;
        read = RequireObjectPath(scope, item, "from", redirect.from);
        if (Stopped(read)) {
            return read;
        }

        read = RequireObjectPath(scope, item, "to", redirect.to);
        if (Stopped(read)) {
            return read;
        }

        if (!redirects.IsEmpty() && !IsLess(redirects[redirects.Size() - 1].from, redirect.from)) {
            return Corrupt(scope, ExchangeIssue::NotSorted, item.Position(), redirect.from);
        }

        if (SearchAssets(assets, redirect.from) != ExchangeNoIndex) {
            return Corrupt(scope, ExchangeIssue::RedirectConflict, item.Position(), redirect.from);
        }

        if (auto pushed = redirects.TryPushBack(redirect); !pushed) {
            return Result<bool>::Failure(pushed.Error());
        }
    }

    // 사슬 없음: to가 다른 항목의 from이면 안 된다(플러그인이 끝까지 푼다).
    for (usize index = 0; index < redirects.Size(); ++index) {
        if (SearchRedirects(redirects.View(), redirects[index].to) != ExchangeNoIndex) {
            return Corrupt(scope, ExchangeIssue::RedirectConflict, list.At(index).Position(),
                           redirects[index].from);
        }
    }

    return Accept();
}

bool IsLowerHex(StringView text) noexcept {
    if (text.ByteCount() != Sha256Digest::HexBytes) {
        return false;
    }

    for (usize index = 0; index < text.ByteCount(); ++index) {
        const char character = text.Data()[index];
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }

    return true;
}

// blob 참조 객체 하나를 읽는다(3.3절, 10.4절 11번).
Result<bool> ParseBlobRef(ReadScope& scope, JsonValue object, ExchangeBlobRef& out) noexcept {
    auto read = WarnUnknownKeys(scope, object, Span<const StringView>(BlobKeys));
    if (Stopped(read)) {
        return read;
    }

    StringView hex;
    read = RequireString(scope, object, BlobKey, hex);
    if (Stopped(read)) {
        return read;
    }

    if (!IsLowerHex(hex)) {
        return Corrupt(scope, ExchangeIssue::InvalidValue, object.Find(BlobKey).Position(),
                       BlobKey);
    }

    out.digest = Sha256Digest::ParseHex(hex).Value();
    read = RequireUnsigned(scope, object, "bytes", UnrealExchangeMaxBlobBytes, out.bytes);
    if (Stopped(read)) {
        return read;
    }

    StringView layoutName;
    read = RequireString(scope, object, "layout", layoutName);
    if (Stopped(read)) {
        return read;
    }

    bool known = false;
    for (const ExchangeBlobLayout layout : ExchangeBlobLayouts) {
        if (layoutName == StringView(ExchangeBlobLayoutName(layout))) {
            out.layout = layout;
            known = true;
        }
    }

    if (!known) {
        return Corrupt(scope, ExchangeIssue::InvalidValue, object.Find("layout").Position(),
                       "layout");
    }

    uint64 components = 0;
    read = RequireUnsigned(scope, object, "components", NumericLimits<uint32>::Max(), components);
    if (Stopped(read)) {
        return read;
    }

    const bool singleChannel = out.layout == ExchangeBlobLayout::BoneInfluences ||
                               out.layout == ExchangeBlobLayout::SparseVec3;
    if (components == 0 || (singleChannel && components != 1)) {
        return Corrupt(scope, ExchangeIssue::InvalidValue, object.Find("components").Position(),
                       "components");
    }

    out.components = static_cast<uint32>(components);
    read = RequireUnsigned(scope, object, "count", NumericLimits<uint64>::Max(), out.count);
    if (Stopped(read)) {
        return read;
    }

    // 고정 크기 원소: bytes = 원소 크기 × components × count(나눗셈으로 넘침 없이 확인).
    const uint64 element = ExchangeBlobElementBytes(out.layout);
    if (element != 0) {
        const uint64 unit = element * out.components;
        if (out.bytes % unit != 0 || out.bytes / unit != out.count) {
            return Corrupt(scope, ExchangeIssue::InvalidValue, object.Find("bytes").Position(),
                           "bytes");
        }
    }

    return Accept();
}

bool SameBlobUse(const ExchangeBlobRef& left, const ExchangeBlobRef& right) noexcept {
    return left.digest == right.digest && left.layout == right.layout &&
           left.components == right.components && left.count == right.count;
}

// settings·data 안의 blob 참조를 모은다(깊이 우선, 문서 순서). 같은 hex를 다른 bytes로 적으면
// BlobConflict다.
Result<bool> CollectBlobRefs(ReadScope& scope, JsonValue root, Allocator& allocator,
                             Array<ExchangeBlobRef>& blobs) noexcept {
    Array<JsonValue> pending(allocator);
    if (auto pushed = pending.TryPushBack(root); !pushed) {
        return Result<bool>::Failure(pushed.Error());
    }

    while (!pending.IsEmpty()) {
        // 비어 있지 않으므로 꺼내기는 실패하지 않는다.
        const JsonValue value = pending.TryPopBack().Value();

        if (value.IsObject() && value.Find(BlobKey).IsValid()) {
            ExchangeBlobRef blob;
            auto parsed = ParseBlobRef(scope, value, blob);
            if (Stopped(parsed)) {
                return parsed;
            }

            bool seen = false;
            for (const ExchangeBlobRef& previous : blobs) {
                if (previous.digest == blob.digest && previous.bytes != blob.bytes) {
                    char hex[Sha256Digest::HexBytes + 1];
                    blob.digest.ToHex(hex);
                    return Corrupt(scope, ExchangeIssue::BlobConflict, value.Position(),
                                   StringView(hex, Sha256Digest::HexBytes));
                }

                seen = seen || SameBlobUse(previous, blob);
            }

            if (!seen) {
                if (auto pushed = blobs.TryPushBack(blob); !pushed) {
                    return Result<bool>::Failure(pushed.Error());
                }
            }

            continue;
        }

        if (!value.IsObject() && !value.IsArray()) {
            continue;
        }

        // 뒤 원소부터 쌓아 문서 순서대로 꺼낸다.
        for (usize index = value.Size(); index != 0; --index) {
            const JsonValue child =
                value.IsObject() ? value.ValueAt(index - 1) : value.At(index - 1);
            if (auto pushed = pending.TryPushBack(child); !pushed) {
                return Result<bool>::Failure(pushed.Error());
            }
        }
    }

    return Accept();
}

Result<bool> ReadDependencies(ReadScope& scope, JsonValue root,
                              Array<ExchangeDependency>& dependencies) noexcept {
    JsonValue list;
    auto read = RequireValue(scope, root, "dependencies", JsonType::Array, list);
    if (Stopped(read)) {
        return read;
    }

    if (list.Size() > UnrealExchangeMaxDependencies) {
        return OverLimit(scope, list.Position(), "dependencies");
    }

    for (usize index = 0; index < list.Size(); ++index) {
        const JsonValue item = list.At(index);
        if (!item.IsObject()) {
            return Corrupt(scope, ExchangeIssue::WrongType, item.Position(), "dependencies");
        }

        read = WarnUnknownKeys(scope, item, Span<const StringView>(DependencyKeys));
        if (Stopped(read)) {
            return read;
        }

        ExchangeDependency dependency;
        read = RequireObjectPath(scope, item, "objectPath", dependency.objectPath);
        if (Stopped(read)) {
            return read;
        }

        read = RequireString(scope, item, "class", dependency.className);
        if (Stopped(read)) {
            return read;
        }

        StringView kind;
        read = RequireString(scope, item, "kind", kind);
        if (Stopped(read)) {
            return read;
        }

        if (kind == StringView("hard")) {
            dependency.kind = ExchangeDependencyKind::Hard;
        } else if (kind == StringView("soft")) {
            dependency.kind = ExchangeDependencyKind::Soft;
        } else {
            return Corrupt(scope, ExchangeIssue::InvalidValue, item.Find("kind").Position(),
                           "kind");
        }

        if (!dependencies.IsEmpty() &&
            !IsLess(dependencies[dependencies.Size() - 1].objectPath, dependency.objectPath)) {
            return Corrupt(scope, ExchangeIssue::NotSorted, item.Position(), dependency.objectPath);
        }

        dependency.targetClass = FindExchangeClass(dependency.className);
        if (auto pushed = dependencies.TryPushBack(dependency); !pushed) {
            return Result<bool>::Failure(pushed.Error());
        }
    }

    return Accept();
}

// 플러그인 진단 배열(3.5절 diagnostics)의 항목 형식을 확인한다.
Result<bool> ReadExporterDiagnostics(ReadScope& scope, JsonValue root, JsonValue& out) noexcept {
    auto read = RequireValue(scope, root, "diagnostics", JsonType::Array, out);
    if (Stopped(read)) {
        return read;
    }

    for (usize index = 0; index < out.Size(); ++index) {
        const JsonValue item = out.At(index);
        if (!item.IsObject()) {
            return Corrupt(scope, ExchangeIssue::WrongType, item.Position(), "diagnostics");
        }

        read = WarnUnknownKeys(scope, item, Span<const StringView>(ExporterDiagnosticKeys));
        if (Stopped(read)) {
            return read;
        }

        StringView text;
        read = RequireString(scope, item, "code", text);
        if (Stopped(read)) {
            return read;
        }

        read = RequireString(scope, item, "message", text);
        if (Stopped(read)) {
            return read;
        }

        StringView severity;
        read = RequireString(scope, item, "severity", severity);
        if (Stopped(read)) {
            return read;
        }

        bool known = false;
        for (const StringView& name : ExporterSeverities) {
            known = known || severity == name;
        }

        if (!known) {
            return Corrupt(scope, ExchangeIssue::InvalidValue, item.Find("severity").Position(),
                           "severity");
        }
    }

    return Accept();
}

// 에셋 JSON 하나를 읽어 asset을 채운다(10.4절 5–9·11번).
Result<bool> ReadAssetDocument(platform::FileSystem& files, StringView directory,
                               Allocator& allocator, ReadScope& scope,
                               ExchangeAsset& asset) noexcept {
    auto made = UniquePtr<JsonDocument>::TryMake(allocator, allocator);
    if (!made) {
        return Result<bool>::Failure(made.Error());
    }

    JsonDocument& document = *made.Value();
    auto read = ReadJsonFile(files, directory, asset.file, allocator, scope, document);
    if (Stopped(read)) {
        return read;
    }

    const JsonValue root = document.Root();
    read = WarnUnknownKeys(scope, root, Span<const StringView>(AssetKeys));
    if (Stopped(read)) {
        return read;
    }

    StringView objectPath;
    read = RequireString(scope, root, "objectPath", objectPath);
    if (Stopped(read)) {
        return read;
    }

    if (objectPath != asset.objectPath) {
        return Corrupt(scope, ExchangeIssue::ManifestMismatch, root.Find("objectPath").Position(),
                       objectPath);
    }

    StringView className;
    read = RequireString(scope, root, "class", className);
    if (Stopped(read)) {
        return read;
    }

    if (className != asset.className) {
        return Corrupt(scope, ExchangeIssue::ManifestMismatch, root.Find("class").Position(),
                       className);
    }

    read = RequireString(scope, root, "ueClass", asset.ueClass);
    if (Stopped(read)) {
        return read;
    }

    read = ReadDependencies(scope, root, asset.dependencies);
    if (Stopped(read)) {
        return read;
    }

    read = ReadExporterDiagnostics(scope, root, asset.exporterDiagnostics);
    if (Stopped(read)) {
        return read;
    }

    read = RequireValue(scope, root, "settings", JsonType::Object, asset.settings);
    if (Stopped(read)) {
        return read;
    }

    read = RequireValue(scope, root, "data", JsonType::Object, asset.data);
    if (Stopped(read)) {
        return read;
    }

    read = CollectBlobRefs(scope, asset.settings, allocator, asset.blobs);
    if (Stopped(read)) {
        return read;
    }

    read = CollectBlobRefs(scope, asset.data, allocator, asset.blobs);
    if (Stopped(read)) {
        return read;
    }

    asset.document = Move(made.Value());
    return Accept();
}

// 에셋을 실패 상태로 두고 문서에서 빌린 값을 놓는다.
void FailReadAsset(ExchangeAsset& asset) noexcept {
    asset.state = ExchangeAssetState::Failed;
    asset.ueClass = StringView();
    asset.settings = JsonValue();
    asset.data = JsonValue();
    asset.exporterDiagnostics = JsonValue();
    asset.dependencies.Clear();
    asset.blobs.Clear();
    asset.document.Reset();
}

// blob 사용 하나(검증 묶기용)다.
struct BlobUse {
    ExchangeBlobRef blob;
    uint32 asset;
};

int CompareDigests(const Sha256Digest& left, const Sha256Digest& right) noexcept {
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        if (left.bytes[index] != right.bytes[index]) {
            return left.bytes[index] < right.bytes[index] ? -1 : 1;
        }
    }

    return 0;
}

// (digest, bytes, 배치, components, count, 에셋) 순이다.
bool IsBlobUseBefore(const BlobUse& left, const BlobUse& right) noexcept {
    const int digest = CompareDigests(left.blob.digest, right.blob.digest);
    if (digest != 0) {
        return digest < 0;
    }

    const uint64 leftKey[] = {left.blob.bytes, static_cast<uint64>(left.blob.layout),
                              left.blob.components, left.blob.count};
    const uint64 rightKey[] = {right.blob.bytes, static_cast<uint64>(right.blob.layout),
                               right.blob.components, right.blob.count};
    for (usize index = 0; index < sizeof(leftKey) / sizeof(leftKey[0]); ++index) {
        if (leftKey[index] != rightKey[index]) {
            return leftKey[index] < rightKey[index];
        }
    }

    // 한 에셋의 같은 사용은 하나로 모았으므로 에셋 번호까지 같은 두 사용은 없다.
    return left.asset < right.asset;
}

// 에셋 하나를 blob 오류로 실패시킨다(이미 실패했으면 그대로 둔다).
Result<void> FailBlobAsset(ExchangeDiagnostics& diagnostics, ExchangeAsset& asset, uint32 index,
                           const ExchangeBlobRef& blob, ExchangeIssue issue,
                           ErrorCode error) noexcept {
    if (asset.state != ExchangeAssetState::Ready) {
        return Result<void>::Success();
    }

    char hex[Sha256Digest::HexBytes + 1];
    blob.digest.ToHex(hex);
    ExchangeDiagnosticInput input;
    input.code = AssetDiagnosticCode::CorruptBundle;
    input.error = error;
    input.issue = issue;
    input.file = ExchangeFile::Blob;
    input.asset = index;
    input.text = StringView(hex, Sha256Digest::HexBytes);
    if (auto added = diagnostics.TryAdd(input); !added) {
        return added;
    }

    FailReadAsset(asset);
    return Result<void>::Success();
}

// 묶음 전체의 blob 사용을 검사한다: 같은 hex의 다른 bytes(에셋 사이), 그리고 verify면 서로 다른
// (hex, 배치, components, count)마다 파일 하나씩 확인한다.
Result<void> CheckBundleBlobs(platform::FileSystem& files, StringView directory, bool verify,
                              Allocator& allocator, Array<ExchangeAsset>& assets,
                              ExchangeDiagnostics& diagnostics) noexcept {
    Array<BlobUse> uses(allocator);
    for (usize index = 0; index < assets.Size(); ++index) {
        if (assets[index].state != ExchangeAssetState::Ready) {
            continue;
        }

        for (const ExchangeBlobRef& blob : assets[index].blobs) {
            if (auto pushed = uses.TryPushBack(BlobUse{blob, static_cast<uint32>(index)});
                !pushed) {
                return pushed;
            }
        }
    }

    algo::Sort(uses.View(), [](const BlobUse& left, const BlobUse& right) noexcept {
        return IsBlobUseBefore(left, right);
    });

    // 같은 hex 묶음 안에 bytes가 둘 이상이면 그 hex를 쓰는 에셋 모두가 손상이다.
    for (usize start = 0; start < uses.Size();) {
        usize end = start + 1;
        bool conflict = false;
        while (end < uses.Size() && uses[end].blob.digest == uses[start].blob.digest) {
            conflict = conflict || uses[end].blob.bytes != uses[start].blob.bytes;
            ++end;
        }

        for (usize at = start; conflict && at < end; ++at) {
            if (auto failed = FailBlobAsset(diagnostics, assets[uses[at].asset], uses[at].asset,
                                            uses[at].blob, ExchangeIssue::BlobConflict,
                                            ErrorCode::InvalidArgument);
                !failed) {
                return failed;
            }
        }

        start = end;
    }

    if (!verify) {
        return Result<void>::Success();
    }

    for (usize start = 0; start < uses.Size();) {
        usize end = start + 1;
        while (end < uses.Size() && SameBlobUse(uses[end].blob, uses[start].blob)) {
            ++end;
        }

        bool live = false;
        for (usize at = start; at < end; ++at) {
            live = live || assets[uses[at].asset].state == ExchangeAssetState::Ready;
        }

        if (live) {
            exchange_detail::BlobCheckFailure failure;
            auto checked = exchange_detail::CheckBlobFile(files, directory, uses[start].blob,
                                                          allocator, nullptr, failure);
            if (!checked) {
                return Result<void>::Failure(checked.Error());
            }

            for (usize at = start; !checked.Value() && at < end; ++at) {
                if (auto failed = FailBlobAsset(diagnostics, assets[uses[at].asset], uses[at].asset,
                                                uses[at].blob, failure.issue, failure.error);
                    !failed) {
                    return failed;
                }
            }
        }

        start = end;
    }

    return Result<void>::Success();
}

// 의존의 묶음 안 대상을 정하고(redirect 풀기) 대상 class가 의존에 적은 class와 같은지 본다.
Result<void> ResolveDependencies(const ExchangeBundle& bundle, Array<ExchangeAsset>& assets,
                                 ExchangeDiagnostics& diagnostics) noexcept {
    for (usize index = 0; index < assets.Size(); ++index) {
        ExchangeAsset& asset = assets[index];
        if (asset.state != ExchangeAssetState::Ready) {
            continue;
        }

        for (ExchangeDependency& dependency : asset.dependencies) {
            const uint32 target =
                SearchAssets(assets.View(), bundle.ResolveRedirect(dependency.objectPath));
            if (target == ExchangeNoIndex) {
                continue;
            }

            if (dependency.className != assets[target].className) {
                ExchangeDiagnosticInput input;
                input.code = AssetDiagnosticCode::CorruptBundle;
                input.error = ErrorCode::InvalidArgument;
                input.issue = ExchangeIssue::ManifestMismatch;
                input.file = ExchangeFile::AssetJson;
                input.asset = static_cast<uint32>(index);
                input.related = target;
                input.text = dependency.objectPath;
                if (auto added = diagnostics.TryAdd(input); !added) {
                    return added;
                }

                FailReadAsset(asset);
                break;
            }

            dependency.target = target;
        }
    }

    return Result<void>::Success();
}

Result<void> AbortRead(ExchangeBundle& out, const Result<bool>& result,
                       const ReadScope& scope) noexcept {
    out.Clear();
    return Result<void>::Failure(!result ? result.Error() : scope.rejected);
}

} // namespace

const char* ExchangeClassName(ExchangeClass type) noexcept {
    for (const ExchangeClassEntry& entry : ExchangeClassTable) {
        if (entry.type == type) {
            return entry.name.Data();
        }
    }

    return "Unsupported";
}

ExchangeClass FindExchangeClass(StringView name) noexcept {
    for (const ExchangeClassEntry& entry : ExchangeClassTable) {
        if (entry.name == name) {
            return entry.type;
        }
    }

    return ExchangeClass::Unsupported;
}

bool IsExchangeTextureClass(ExchangeClass type) noexcept {
    switch (type) {
    case ExchangeClass::Texture2D:
    case ExchangeClass::TextureCube:
    case ExchangeClass::Texture2DArray:
    case ExchangeClass::TextureCubeArray:
    case ExchangeClass::VolumeTexture:
        return true;
    default:
        return false;
    }
}

bool IsExchangeMaterialFamily(ExchangeClass type) noexcept {
    switch (type) {
    case ExchangeClass::Material:
    case ExchangeClass::MaterialInstanceConstant:
    case ExchangeClass::MaterialFunction:
    case ExchangeClass::MaterialFunctionInstance:
        return true;
    default:
        return false;
    }
}

ExchangeBundle::ExchangeBundle(Allocator& allocator) noexcept
    : allocator_(&allocator), directory_(allocator), assets_(allocator), redirects_(allocator) {
}

uint32 ExchangeBundle::FindAsset(StringView objectPath) const noexcept {
    return SearchAssets(assets_.View(), objectPath);
}

StringView ExchangeBundle::ResolveRedirect(StringView objectPath) const noexcept {
    const uint32 found = SearchRedirects(redirects_.View(), objectPath);
    return found == ExchangeNoIndex ? objectPath : redirects_[found].to;
}

void ExchangeBundle::Clear() noexcept {
    formatVersion = 0;
    exporter = ExchangeExporter{};
    substrate = false;
    retainJpegFormat = false;
    createdUtc = StringView();
    environment = JsonValue();
    files_ = nullptr;
    directory_.Clear();
    assets_.Clear();
    redirects_.Clear();
    manifest_.Reset();
}

Result<void> ReadExchangeBundle(platform::FileSystem& files, StringView directory,
                                const ExchangeReadOptions& options, ExchangeBundle& out,
                                ExchangeDiagnostics& diagnostics) noexcept {
    out.Clear();
    Allocator& allocator = ExchangeBundleAccess::GetAllocator(out);
    ReadScope scope{diagnostics, ExchangeFile::BundleJson};
    auto made = UniquePtr<JsonDocument>::TryMake(allocator, allocator);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    JsonDocument& manifest = *made.Value();
    auto read = ReadJsonFile(files, directory, BundleManifestFile, allocator, scope, manifest);
    if (Stopped(read)) {
        return AbortRead(out, read, scope);
    }

    // formatVersion을 가장 먼저 본다(다른 버전은 스키마 차이와 무관하게 버전 오류).
    const JsonValue root = manifest.Root();
    const JsonValue version = root.Find("formatVersion");
    if (!version.IsValid()) {
        return AbortRead(
            out, Corrupt(scope, ExchangeIssue::MissingKey, root.Position(), "formatVersion"),
            scope);
    }

    if (version.Type() != JsonType::Integer && version.Type() != JsonType::Unsigned) {
        return AbortRead(
            out, Corrupt(scope, ExchangeIssue::WrongType, version.Position(), "formatVersion"),
            scope);
    }

    auto number = version.TryGetUInt64();
    if (!number || number.Value() != UnrealExchangeBundleFormatVersion) {
        return AbortRead(out,
                         Reject(scope, AssetDiagnosticCode::UnsupportedBundleVersion,
                                ErrorCode::Unsupported, ExchangeIssue::Version, version.Position(),
                                version.RawNumber()),
                         scope);
    }

    out.formatVersion = UnrealExchangeBundleFormatVersion;
    Array<ExchangeAsset>& assets = ExchangeBundleAccess::Assets(out);
    Array<ExchangeRedirect>& redirects = ExchangeBundleAccess::Redirects(out);
    read = WarnUnknownKeys(scope, root, Span<const StringView>(BundleKeys));
    if (!Stopped(read)) {
        read = ReadExporter(scope, root, out.exporter);
    }

    if (!Stopped(read)) {
        read = ReadProjectSettings(scope, root, out);
    }

    if (!Stopped(read)) {
        read = ReadInformation(scope, root, out);
    }

    if (!Stopped(read)) {
        read = ReadManifestAssets(scope, root, allocator, assets);
    }

    if (!Stopped(read)) {
        read = ReadRedirects(scope, root, assets.View(), redirects);
    }

    if (Stopped(read)) {
        return AbortRead(out, read, scope);
    }

    if (auto assigned = ExchangeBundleAccess::Directory(out).TryAssign(directory); !assigned) {
        out.Clear();
        return assigned;
    }

    ExchangeBundleAccess::SetFiles(out, &files);
    ExchangeBundleAccess::Manifest(out) = Move(made.Value());

    // 에셋 수준: 실패한 에셋만 Failed로 두고 계속한다.
    for (usize index = 0; index < assets.Size(); ++index) {
        ExchangeAsset& asset = assets[index];
        ReadScope assetScope{diagnostics, ExchangeFile::AssetJson, static_cast<uint32>(index)};
        if (asset.exchangeClass == ExchangeClass::Unsupported) {
            asset.state = ExchangeAssetState::Unsupported;
            ExchangeDiagnosticInput input;
            input.code = AssetDiagnosticCode::UnsupportedAsset;
            input.issue = ExchangeIssue::UnsupportedClass;
            input.file = ExchangeFile::BundleJson;
            input.asset = static_cast<uint32>(index);
            input.text = asset.className;
            if (auto added = diagnostics.TryAdd(input); !added) {
                out.Clear();
                return added;
            }

            continue;
        }

        auto loaded = ReadAssetDocument(files, directory, allocator, assetScope, asset);
        if (!loaded) {
            out.Clear();
            return Result<void>::Failure(loaded.Error());
        }

        if (!loaded.Value()) {
            FailReadAsset(asset);
            continue;
        }

        asset.state = ExchangeAssetState::Ready;
    }

    if (auto checked =
            CheckBundleBlobs(files, directory, options.verifyBlobs, allocator, assets, diagnostics);
        !checked) {
        out.Clear();
        return checked;
    }

    if (auto resolved = ResolveDependencies(out, assets, diagnostics); !resolved) {
        out.Clear();
        return resolved;
    }

    return Result<void>::Success();
}

Result<void> ReadExchangeNumber(JsonValue value, double& out) noexcept {
    if (value.IsNumber()) {
        // JSON 숫자(Integer·Unsigned·Double)는 언제나 double로 읽힌다.
        out = value.TryGetDouble().Value();
        return Result<void>::Success();
    }

    if (value.IsString()) {
        const StringView text = value.GetString();
        if (text == StringView("nan")) {
            out = platform::BitCast<double>(ExchangeQuietNaNBits);
            return Result<void>::Success();
        }

        if (text == StringView("inf")) {
            out = platform::BitCast<double>(ExchangeInfinityBits);
            return Result<void>::Success();
        }

        if (text == StringView("-inf")) {
            out = platform::BitCast<double>(ExchangeNegativeInfinityBits);
            return Result<void>::Success();
        }
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

} // namespace rocks
