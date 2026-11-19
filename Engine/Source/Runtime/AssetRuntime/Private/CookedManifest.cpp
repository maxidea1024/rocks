#include "PackageInternal.h"

#include <AssetRuntime/CookedManifest.h>

#include <Base/Endian.h>
#include <Core/Encoding/Crc32.h>
#include <Core/Text/NameComponent.h>
#include <Serialization/Archive.h>

// 쿠킹 매니페스트 v1 reader·writer·검증·대조다(Docs/Design/AssetSystem.ko.md 10절, 10.1절).
// reader는 byte를 모델로 풀기만 하고(구조 — 길이·잘림·개수), 칸 규칙과 항목 사이 규칙은 writer와
// 같은 ValidateCookedManifest가 본다(두 쪽 규칙이 갈리지 않는다).
namespace rocks {

namespace {

constexpr usize MagicBytes = 4;
constexpr usize GuidBytes = 16;
constexpr usize DigestBytes = Sha256Digest::Bytes;
constexpr usize LengthPrefixBytes = sizeof(uint64);
// magic·formatVersion·entryCount·crc32다(그보다 짧은 파일은 판정할 칸이 없다).
constexpr usize MinFileBytes = MagicBytes + 3 * sizeof(uint32);
// 항목 하나의 최소 byte다(문자열은 길이 칸만, 개수 칸은 0) — 개수 칸을 남은 byte로 묶는다.
constexpr usize MinEntryBytes = 2 * GuidBytes + LengthPrefixBytes + DigestBytes +
                                2 * sizeof(uint64) + DigestBytes + 3 * sizeof(uint32);
constexpr usize MinBundleBytes = LengthPrefixBytes;
constexpr usize DependencyBytes = GuidBytes + sizeof(uint32);
// 비교 키·식별자 문자열 builder의 내장 용량이다. 키 길이는 경로 상한까지 다양하므로 작게 두고
// allocator로 자라게 한다(할당 실패는 그 오류로 돌려준다).
constexpr usize KeyInlineBytes = 8;
// u16 엔진 버전 칸의 상한이다.
constexpr uint32 MaxEngineVersionPart = 0xFFFF;

using Field = CookedManifestField;

template <class T>
Result<T> Corrupt(AssetDiagnostic* failure, Field field, uint64 detail, AssetId asset = {},
                  AssetId related = {}) noexcept {
    return FailAsset<T>(failure, AssetDiagnosticCode::CorruptPackage, ErrorCode::InvalidArgument,
                        asset, static_cast<uint32>(field), detail, related);
}

template <class T>
Result<T> Resource(AssetDiagnostic* failure, ErrorCode error, uint64 detail = CookedManifestNoEntry,
                   AssetId asset = {}) noexcept {
    return FailAsset<T>(failure, AssetDiagnosticCode::None, error, asset,
                        static_cast<uint32>(Field::File), detail);
}

template <class T>
Result<T> Incompatible(AssetDiagnostic* failure, Field field) noexcept {
    return FailAsset<T>(failure, AssetDiagnosticCode::IncompatibleCook, ErrorCode::Unsupported, {},
                        static_cast<uint32>(field), CookedManifestNoEntry);
}

bool SameDigest(const Sha256Digest& left, const Sha256Digest& right) noexcept {
    for (usize index = 0; index < DigestBytes; ++index) {
        if (left.bytes[index] != right.bytes[index]) {
            return false;
        }
    }

    return true;
}

// UTF-8 byte 순서(부호 없는 사전 순 — AS 4.2절 정렬)다.
bool ByteLess(StringView left, StringView right) noexcept {
    const usize shorter =
        left.ByteCount() < right.ByteCount() ? left.ByteCount() : right.ByteCount();
    for (usize index = 0; index < shorter; ++index) {
        const uint8 a = static_cast<uint8>(left.Data()[index]);
        const uint8 b = static_cast<uint8>(right.Data()[index]);
        if (a != b) {
            return a < b;
        }
    }

    return left.ByteCount() < right.ByteCount();
}

// (asset, kind) 순서다.
bool DependencyLess(const CookedManifestDependency& left,
                    const CookedManifestDependency& right) noexcept {
    if (!(left.asset == right.asset)) {
        return GuidLess(left.asset.value, right.asset.value);
    }

    return static_cast<uint32>(left.kind) < static_cast<uint32>(right.kind);
}

// 경로 힌트(AS 4.2절)를 검사하고, 받으면 caseless 키를 key 뒤에 붙인다. 규칙 위반은
// InvalidArgument, scratch·key 할당 실패는 그 오류다.
Result<void> CheckPathHint(StringView path, Allocator& scratch, StringBuilderBase& key) noexcept {
    const usize bytes = path.ByteCount();
    if (bytes == 0 || bytes > CookedManifestMaxPathBytes || path.Data()[0] != '/') {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 마운트 + 이름 — 구성 요소 둘 이상이다.
    constexpr usize MinComponents = 2;
    usize components = 0;
    usize begin = 1;
    for (usize index = 1; index <= bytes; ++index) {
        if (index < bytes && path.Data()[index] != '/') {
            continue;
        }

        const StringView component(path.Data() + begin, index - begin);
        if (auto valid = ValidateNameComponent(component, scratch); !valid) {
            return valid;
        }

        if (auto slash = key.TryAppendAscii('/'); !slash) {
            return slash;
        }

        if (auto made = TryMakeNameComponentKey(component, key); !made) {
            return made;
        }

        ++components;
        begin = index + 1;
    }

    return components >= MinComponents ? Result<void>::Success()
                                       : Result<void>::Failure(ErrorCode::InvalidArgument);
}

// 진단 칸 하나의 실패를 만든다(규칙 위반 → CorruptPackage, 할당 실패 → 진단 None).
template <class T>
Result<T> FieldFailure(AssetDiagnostic* failure, ErrorCode error, Field field, uint64 detail,
                       AssetId asset) noexcept {
    if (error == ErrorCode::InvalidArgument) {
        return Corrupt<T>(failure, field, detail, asset);
    }

    return Resource<T>(failure, error, detail, asset);
}

Result<void> ValidateBundles(const CookedManifestEntry& entry, uint64 index, Allocator& scratch,
                             AssetDiagnostic* failure) noexcept {
    const usize count = entry.bundles.Size();
    if (count > CookedManifestMaxBundles ||
        (count != 0 &&
         (entry.flags & static_cast<uint32>(CookedManifestFlags::PrimaryAsset)) == 0)) {
        return Corrupt<void>(failure, Field::BundleCount, index, entry.asset);
    }

    Array<String> keys(scratch);
    if (auto reserved = keys.TryReserve(count); !reserved) {
        return Resource<void>(failure, reserved.Error(), index, entry.asset);
    }

    for (usize bundle = 0; bundle < count; ++bundle) {
        const StringView name = entry.bundles[bundle].View();
        if (auto valid = ValidateNameComponent(name, scratch); !valid) {
            return FieldFailure<void>(failure, valid.Error(), Field::Bundle, index, entry.asset);
        }

        if (bundle > 0 && !ByteLess(entry.bundles[bundle - 1].View(), name)) {
            return Corrupt<void>(failure, Field::Bundle, index, entry.asset);
        }

        StringBuilder<KeyInlineBytes> key(scratch);
        if (auto made = TryMakeNameComponentKey(name, key); !made) {
            return Resource<void>(failure, made.Error(), index, entry.asset);
        }

        // keys는 bundle 수만큼 예약했다(빈 String 추가는 할당하지 않는다).
        String& copy = *keys.TryEmplaceBack(scratch).Value();
        if (auto copied = copy.TryAppend(key.View()); !copied) {
            return Resource<void>(failure, copied.Error(), index, entry.asset);
        }

        for (usize other = 0; other < bundle; ++other) {
            if (keys[other].View() == key.View()) {
                return Corrupt<void>(failure, Field::Bundle, index, entry.asset);
            }
        }
    }

    return Result<void>::Success();
}

Result<void> ValidateDependencies(const CookedManifestEntry& entry, uint64 index,
                                  AssetDiagnostic* failure) noexcept {
    const usize count = entry.dependencies.Size();
    if (count > CookedManifestMaxDependencies) {
        return Corrupt<void>(failure, Field::DependencyCount, index, entry.asset);
    }

    for (usize position = 0; position < count; ++position) {
        const CookedManifestDependency& dependency = entry.dependencies[position];
        const bool runtimeKind = dependency.kind == PackageDependencyKind::Hard ||
                                 dependency.kind == PackageDependencyKind::Soft;
        if (dependency.asset.IsNull() || dependency.asset == entry.asset || !runtimeKind ||
            (position > 0 && !DependencyLess(entry.dependencies[position - 1], dependency))) {
            return Corrupt<void>(failure, Field::Dependency, index, entry.asset, dependency.asset);
        }
    }

    return Result<void>::Success();
}

// 항목 하나의 칸 규칙이다(항목 사이 규칙 — 정렬·경로 유일성·Hard 대상 — 은 호출자).
Result<void> ValidateEntry(const CookedManifestEntry& entry, uint64 index, Allocator& scratch,
                           StringBuilderBase& pathKey, AssetDiagnostic* failure) noexcept {
    if (entry.type == TypeId{}) {
        return Corrupt<void>(failure, Field::TypeId, index, entry.asset);
    }

    if (auto path = CheckPathHint(entry.pathHint.View(), scratch, pathKey); !path) {
        return FieldFailure<void>(failure, path.Error(), Field::PathHint, index, entry.asset);
    }

    if (entry.packageBytes < PackageHeaderBytes || entry.packageBytes > PackageMaxFileBytes) {
        return Corrupt<void>(failure, Field::PackageBytes, index, entry.asset);
    }

    if ((entry.flags & ~CookedManifestFlagsMask) != 0) {
        return Corrupt<void>(failure, Field::Flags, index, entry.asset);
    }

    if (auto bundles = ValidateBundles(entry, index, scratch, failure); !bundles) {
        return bundles;
    }

    return ValidateDependencies(entry, index, failure);
}

} // namespace

const CookedManifestEntry* CookedManifest::Find(AssetId asset) const noexcept {
    usize low = 0;
    usize high = entries.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (GuidLess(entries[middle].asset.value, asset.value)) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return low < entries.Size() && entries[low].asset == asset ? &entries[low] : nullptr;
}

Result<void> ValidateCookedManifest(const CookedManifest& manifest, Allocator& scratch,
                                    AssetDiagnostic* failure) noexcept {
    const StringView profile = manifest.profileId.View();
    // String은 유효한 UTF-8만 담으므로(Core String 불변식) UTF-8은 다시 보지 않는다 — byte의
    // UTF-8은 reader의 문자열 읽기가 본다.
    if (profile.IsEmpty() || profile.ByteCount() > PackageMaxStringBytes) {
        return Corrupt<void>(failure, Field::ProfileId, CookedManifestNoEntry);
    }

    const usize count = manifest.entries.Size();
    if (count > CookedManifestMaxEntries) {
        return Corrupt<void>(failure, Field::EntryCount, CookedManifestNoEntry);
    }

    // 경로 힌트 caseless 키와 그 항목 번호다(정렬해 이웃만 비교한다).
    Array<String> keys(scratch);
    Array<uint32> order(scratch);
    if (auto reserved = keys.TryReserve(count); !reserved) {
        return Resource<void>(failure, reserved.Error());
    }

    if (auto reserved = order.TryReserve(count); !reserved) {
        return Resource<void>(failure, reserved.Error());
    }

    for (usize index = 0; index < count; ++index) {
        const CookedManifestEntry& entry = manifest.entries[index];
        if (entry.asset.IsNull() ||
            (index > 0 && !GuidLess(manifest.entries[index - 1].asset.value, entry.asset.value))) {
            return Corrupt<void>(failure, Field::AssetId, index, entry.asset);
        }

        StringBuilder<KeyInlineBytes> key(scratch);
        if (auto valid = ValidateEntry(entry, index, scratch, key, failure); !valid) {
            return valid;
        }

        // keys는 항목 수만큼 예약했다(빈 String 추가는 할당하지 않는다).
        String& copy = *keys.TryEmplaceBack(scratch).Value();
        if (auto copied = copy.TryAppend(key.View()); !copied) {
            return Resource<void>(failure, copied.Error(), index, entry.asset);
        }

        (void)order.TryPushBack(static_cast<uint32>(index));
    }

    order.Sort([&keys](uint32 left, uint32 right) noexcept {
        return ByteLess(keys[left].View(), keys[right].View()) ||
               (keys[left].View() == keys[right].View() && left < right);
    });
    for (usize position = 1; position < order.Size(); ++position) {
        if (keys[order[position - 1]].View() == keys[order[position]].View()) {
            const uint32 index = order[position];
            return Corrupt<void>(failure, Field::PathHint, index, manifest.entries[index].asset);
        }
    }

    // Hard 의존 대상은 매니페스트에 있다(쿠킹은 Hard 전이 폐포 — AS 10.1절 6).
    for (usize index = 0; index < count; ++index) {
        const CookedManifestEntry& entry = manifest.entries[index];
        for (const CookedManifestDependency& dependency : entry.dependencies) {
            if (dependency.kind == PackageDependencyKind::Hard &&
                manifest.Find(dependency.asset) == nullptr) {
                return Corrupt<void>(failure, Field::Dependency, index, entry.asset,
                                     dependency.asset);
            }
        }
    }

    return Result<void>::Success();
}

namespace package_detail {

Result<void> WriteCookedManifestWithLimit(Allocator& allocator, const CookedManifest& manifest,
                                          uint64 maxFileBytes, Array<uint8>& out,
                                          AssetDiagnostic* failure) noexcept {
    if (auto valid = ValidateCookedManifest(manifest, allocator, failure); !valid) {
        return valid;
    }

    // 상한은 CRC 칸까지 포함한 파일 전체다(CRC 자리도 이 writer가 쓴다).
    ArchiveLimits limits;
    limits.maxTotalBytes = static_cast<usize>(maxFileBytes);
    limits.maxValueBytes = PackageMaxStringBytes;
    limits.maxOperations = NumericLimits<uint64>::Max();
    limits.maxWork = NumericLimits<uint64>::Max();
    ArchiveWriter writer(allocator, limits);
    // 처음 실패한 쓰기의 오류다(None이면 모두 성공).
    ErrorCode error = ErrorCode::None;
    auto step = [&](Result<void> result) noexcept {
        if (error == ErrorCode::None && !result) {
            error = result.Error();
        }
    };
    step(writer.WriteRaw(Span<const uint8>(CookedManifestMagic, MagicBytes)));
    step(writer.WriteU32(CookedManifestFormatVersion));
    step(writer.WriteU32(static_cast<uint32>(manifest.entries.Size())));
    step(writer.WriteString(manifest.profileId.View()));
    step(writer.WriteRaw(Span<const uint8>(manifest.profileSettings.bytes, DigestBytes)));
    step(writer.WriteRaw(Span<const uint8>(manifest.cookSettings.bytes, DigestBytes)));
    step(writer.WriteU32(manifest.cookerVersion));
    step(writer.WriteU32(manifest.cookedBy.majorVersion));
    step(writer.WriteU32(manifest.cookedBy.minorVersion));
    step(writer.WriteU32(manifest.cookedBy.patchVersion));
    for (usize index = 0; error == ErrorCode::None && index < manifest.entries.Size(); ++index) {
        const CookedManifestEntry& entry = manifest.entries[index];
        step(writer.WriteGuid(entry.asset.value));
        step(writer.WriteGuid(entry.type));
        step(writer.WriteString(entry.pathHint.View()));
        step(writer.WriteRaw(Span<const uint8>(entry.packageSha256.bytes, DigestBytes)));
        step(writer.WriteU64(entry.packageBytes));
        step(writer.WriteU64(entry.bulkRawBytes));
        step(writer.WriteRaw(Span<const uint8>(entry.cookKey.bytes, DigestBytes)));
        step(writer.WriteU32(entry.flags));
        step(writer.WriteU32(static_cast<uint32>(entry.bundles.Size())));
        for (const String& bundle : entry.bundles) {
            step(writer.WriteString(bundle.View()));
        }

        step(writer.WriteU32(static_cast<uint32>(entry.dependencies.Size())));
        for (const CookedManifestDependency& dependency : entry.dependencies) {
            step(writer.WriteGuid(dependency.asset.value));
            step(writer.WriteU32(static_cast<uint32>(dependency.kind)));
        }
    }

    // CRC32 자리를 같이 써 두고 다 쓴 뒤 채운다(AS 10.1절 1 — 앞의 모든 byte).
    step(writer.WriteU32(0));
    if (error != ErrorCode::None) {
        return Resource<void>(failure, error);
    }

    out = writer.TakeBytes();
    const usize body = out.Size() - sizeof(uint32);
    StoreLittleEndian<uint32>(out.Data() + body, Crc32(out.View().Slice(0, body)));
    return Result<void>::Success();
}

} // namespace package_detail

Result<void> WriteCookedManifest(Allocator& allocator, const CookedManifest& manifest,
                                 Array<uint8>& out, AssetDiagnostic* failure) noexcept {
    return package_detail::WriteCookedManifestWithLimit(allocator, manifest,
                                                        CookedManifestMaxFileBytes, out, failure);
}

Result<CookedManifest> ReadCookedManifest(Allocator& allocator, Span<const uint8> bytes,
                                          AssetDiagnostic* failure) noexcept {
    using Out = CookedManifest;
    if (bytes.Size() < MinFileBytes || bytes.Size() > CookedManifestMaxFileBytes) {
        return Corrupt<Out>(failure, Field::File, CookedManifestNoEntry);
    }

    for (usize index = 0; index < MagicBytes; ++index) {
        if (bytes[index] != CookedManifestMagic[index]) {
            return Corrupt<Out>(failure, Field::Magic, CookedManifestNoEntry);
        }
    }

    if (LoadLittleEndian<uint32>(bytes.Data() + MagicBytes) != CookedManifestFormatVersion) {
        return FailAsset<Out>(failure, AssetDiagnosticCode::UnsupportedVersion,
                              ErrorCode::Unsupported, {}, static_cast<uint32>(Field::FormatVersion),
                              CookedManifestNoEntry);
    }

    const usize body = bytes.Size() - sizeof(uint32);
    if (Crc32(bytes.Slice(0, body)) != LoadLittleEndian<uint32>(bytes.Data() + body)) {
        return Corrupt<Out>(failure, Field::Crc, CookedManifestNoEntry);
    }

    ArchiveLimits limits;
    limits.maxTotalBytes = body;
    limits.maxValueBytes = PackageMaxStringBytes;
    limits.maxOperations = NumericLimits<uint64>::Max();
    limits.maxWork = NumericLimits<uint64>::Max();
    const usize start = MagicBytes + sizeof(uint32);
    auto created = ArchiveReader::Create(bytes.Slice(start, body - start), limits);
    // body는 limits.maxTotalBytes 이하라 Create는 실패하지 않는다.
    ArchiveReader& in = created.Value();
    CookedManifest manifest(allocator);
    // MinFileBytes가 개수 칸을 보장한다.
    auto count = in.ReadU32();
    if (count.Value() > CookedManifestMaxEntries ||
        count.Value() > in.Remaining() / MinEntryBytes) {
        return Corrupt<Out>(failure, Field::EntryCount, CookedManifestNoEntry);
    }

    auto profile = in.ReadStringView();
    if (!profile) {
        return Corrupt<Out>(failure, Field::ProfileId, CookedManifestNoEntry);
    }

    if (auto assigned = manifest.profileId.TryAssign(profile.Value()); !assigned) {
        return Resource<Out>(failure, assigned.Error());
    }

    auto readDigest = [&](Sha256Digest& digest) noexcept {
        auto raw = in.ReadRaw(DigestBytes);
        if (raw) {
            for (usize index = 0; index < DigestBytes; ++index) {
                digest.bytes[index] = raw.Value()[index];
            }
        }

        return raw.IsOk();
    };
    if (!readDigest(manifest.profileSettings)) {
        return Corrupt<Out>(failure, Field::ProfileSettings, CookedManifestNoEntry);
    }

    if (!readDigest(manifest.cookSettings)) {
        return Corrupt<Out>(failure, Field::CookSettings, CookedManifestNoEntry);
    }

    auto cooker = in.ReadU32();
    if (!cooker) {
        return Corrupt<Out>(failure, Field::CookerVersion, CookedManifestNoEntry);
    }

    manifest.cookerVersion = cooker.Value();
    uint16* const parts[3] = {&manifest.cookedBy.majorVersion, &manifest.cookedBy.minorVersion,
                              &manifest.cookedBy.patchVersion};
    for (uint16* part : parts) {
        auto value = in.ReadU32();
        if (!value || value.Value() > MaxEngineVersionPart) {
            return Corrupt<Out>(failure, Field::EngineVersion, CookedManifestNoEntry);
        }

        *part = static_cast<uint16>(value.Value());
    }

    if (auto reserved = manifest.entries.TryReserve(count.Value()); !reserved) {
        return Resource<Out>(failure, reserved.Error());
    }

    for (uint32 index = 0; index < count.Value(); ++index) {
        auto added = manifest.entries.TryEmplaceBack(allocator);
        // 개수만큼 예약했다.
        CookedManifestEntry& entry = *added.Value();
        auto asset = in.ReadGuid();
        if (!asset) {
            return Corrupt<Out>(failure, Field::AssetId, index);
        }

        entry.asset = AssetId{asset.Value()};
        auto type = in.ReadGuid();
        if (!type) {
            return Corrupt<Out>(failure, Field::TypeId, index, entry.asset);
        }

        entry.type = type.Value();
        auto path = in.ReadStringView();
        if (!path) {
            return Corrupt<Out>(failure, Field::PathHint, index, entry.asset);
        }

        if (auto assigned = entry.pathHint.TryAssign(path.Value()); !assigned) {
            return Resource<Out>(failure, assigned.Error(), index, entry.asset);
        }

        if (!readDigest(entry.packageSha256)) {
            return Corrupt<Out>(failure, Field::PackageSha256, index, entry.asset);
        }

        auto packageBytes = in.ReadU64();
        if (!packageBytes) {
            return Corrupt<Out>(failure, Field::PackageBytes, index, entry.asset);
        }

        entry.packageBytes = packageBytes.Value();
        auto rawBytes = in.ReadU64();
        if (!rawBytes) {
            return Corrupt<Out>(failure, Field::BulkRawBytes, index, entry.asset);
        }

        entry.bulkRawBytes = rawBytes.Value();
        if (!readDigest(entry.cookKey)) {
            return Corrupt<Out>(failure, Field::CookKey, index, entry.asset);
        }

        auto flags = in.ReadU32();
        if (!flags) {
            return Corrupt<Out>(failure, Field::Flags, index, entry.asset);
        }

        entry.flags = flags.Value();
        auto bundles = in.ReadU32();
        if (!bundles || bundles.Value() > CookedManifestMaxBundles ||
            bundles.Value() > in.Remaining() / MinBundleBytes) {
            return Corrupt<Out>(failure, Field::BundleCount, index, entry.asset);
        }

        if (auto reserved = entry.bundles.TryReserve(bundles.Value()); !reserved) {
            return Resource<Out>(failure, reserved.Error(), index, entry.asset);
        }

        for (uint32 bundle = 0; bundle < bundles.Value(); ++bundle) {
            auto name = in.ReadStringView();
            if (!name) {
                return Corrupt<Out>(failure, Field::Bundle, index, entry.asset);
            }

            // bundle 수만큼 예약했다(빈 String 추가는 할당하지 않는다).
            String& slot = *entry.bundles.TryEmplaceBack(allocator).Value();
            if (auto assigned = slot.TryAssign(name.Value()); !assigned) {
                return Resource<Out>(failure, assigned.Error(), index, entry.asset);
            }
        }

        auto dependencies = in.ReadU32();
        if (!dependencies || dependencies.Value() > CookedManifestMaxDependencies ||
            dependencies.Value() > in.Remaining() / DependencyBytes) {
            return Corrupt<Out>(failure, Field::DependencyCount, index, entry.asset);
        }

        if (auto reserved = entry.dependencies.TryReserve(dependencies.Value()); !reserved) {
            return Resource<Out>(failure, reserved.Error(), index, entry.asset);
        }

        for (uint32 dependency = 0; dependency < dependencies.Value(); ++dependency) {
            // 개수를 남은 byte로 묶었으므로 의존 칸은 잘리지 않는다.
            const Guid target = in.ReadGuid().Value();
            const uint32 kind = in.ReadU32().Value();
            (void)entry.dependencies.TryPushBack(CookedManifestDependency{
                AssetId{target}, static_cast<PackageDependencyKind>(kind)});
        }
    }

    if (!in.AtEnd()) {
        return Corrupt<Out>(failure, Field::File, CookedManifestNoEntry);
    }

    if (auto valid = ValidateCookedManifest(manifest, allocator, failure); !valid) {
        return Result<Out>::Failure(valid.Error());
    }

    return Result<Out>::Success(Move(manifest));
}

Result<void> CheckCookedManifestProfile(const CookedManifest& manifest, StringView runtimeProfile,
                                        AssetDiagnostic* failure) noexcept {
    if (!(manifest.profileId.View() == runtimeProfile)) {
        return Incompatible<void>(failure, Field::ProfileId);
    }

    return Result<void>::Success();
}

Result<void> CheckCookedManifestMerge(const CookedManifest& existing,
                                      const CookedManifest& incoming,
                                      AssetDiagnostic* failure) noexcept {
    if (!(existing.profileId.View() == incoming.profileId.View())) {
        return Incompatible<void>(failure, Field::ProfileId);
    }

    if (!SameDigest(existing.profileSettings, incoming.profileSettings)) {
        return Incompatible<void>(failure, Field::ProfileSettings);
    }

    if (!SameDigest(existing.cookSettings, incoming.cookSettings)) {
        return Incompatible<void>(failure, Field::CookSettings);
    }

    if (existing.cookerVersion != incoming.cookerVersion) {
        return Incompatible<void>(failure, Field::CookerVersion);
    }

    return Result<void>::Success();
}

Result<void> CheckCookedPackage(const CookedManifest& manifest, const CookedManifestEntry& entry,
                                Span<const uint8> file, const PackageContents& contents,
                                AssetDiagnostic* failure) noexcept {
    const AssetId asset = entry.asset;
    // 크기·SHA-256을 먼저 본다(바뀐 파일을 해석한 결과로 판정하지 않는다).
    if (file.Size() != entry.packageBytes) {
        return Corrupt<void>(failure, Field::PackageBytes, CookedManifestNoEntry, asset);
    }

    if (!SameDigest(Sha256::Hash(file), entry.packageSha256)) {
        return Corrupt<void>(failure, Field::PackageSha256, CookedManifestNoEntry, asset);
    }

    if (contents.header.kind != PackageKind::Cooked) {
        return Corrupt<void>(failure, Field::PackageKind, CookedManifestNoEntry, asset);
    }

    if (!(contents.header.assetId == asset)) {
        return Corrupt<void>(failure, Field::AssetId, CookedManifestNoEntry, asset);
    }

    if (contents.exports.IsEmpty() || !(contents.exports[0].type == entry.type)) {
        return Corrupt<void>(failure, Field::TypeId, CookedManifestNoEntry, asset);
    }

    // 합이 u64를 넘으면(호출자가 준 내용이 손상) 같은 값일 수 없으므로 불일치다.
    uint64 rawBytes = 0;
    bool overflow = false;
    for (const PackageBulkEntry& bulk : contents.bulkEntries) {
        if (bulk.rawBytes > NumericLimits<uint64>::Max() - rawBytes) {
            overflow = true;
            break;
        }

        rawBytes += bulk.rawBytes;
    }

    if (overflow || rawBytes != entry.bulkRawBytes) {
        return Corrupt<void>(failure, Field::BulkRawBytes, CookedManifestNoEntry, asset);
    }

    const PackageSummary& summary = contents.summary;
    if (!(summary.profileId.View() == manifest.profileId.View()) ||
        !SameDigest(summary.profileSettings, manifest.profileSettings) ||
        summary.cookerVersion != manifest.cookerVersion) {
        return Corrupt<void>(failure, Field::PackageProfile, CookedManifestNoEntry, asset);
    }

    // 쿠킹 패키지 Dependencies는 Hard·Soft뿐이고 (asset, kind) 정렬이다(reader가 확인했다) —
    // 매니페스트 의존과 같은 순서로 하나씩 맞춘다.
    const Array<PackageDependencyEntry>& dependencies = contents.dependencies;
    const usize count = entry.dependencies.Size();
    for (usize index = 0; index < count || index < dependencies.Size(); ++index) {
        if (index >= count || index >= dependencies.Size() ||
            !(dependencies[index].asset == entry.dependencies[index].asset) ||
            dependencies[index].kind != entry.dependencies[index].kind) {
            const AssetId related =
                index < count ? entry.dependencies[index].asset : dependencies[index].asset;
            return Corrupt<void>(failure, Field::PackageDependencies, CookedManifestNoEntry, asset,
                                 related);
        }
    }

    return Result<void>::Success();
}

Result<void> AppendCookedPackagePath(StringBuilderBase& out, AssetId asset,
                                     const Sha256Digest& packageSha256) noexcept {
    // 4.2절: Packages/<a>/<A>-<h>.rpkg — a = A의 앞 2자, h = SHA-256 앞 16자.
    constexpr usize ShardChars = 2;
    constexpr usize HashChars = 16;
    StringBuilder<KeyInlineBytes> text(out.GetAllocator());
    if (auto formatted = text.TryAppendFormat("{}", asset); !formatted) {
        return formatted;
    }

    char hex[Sha256Digest::HexBytes + 1];
    packageSha256.ToHex(hex);
    const StringView id = text.View();
    ErrorCode error = ErrorCode::None;
    auto step = [&](Result<void> result) noexcept {
        if (error == ErrorCode::None && !result) {
            error = result.Error();
        }
    };
    step(out.TryAppend("Packages/"));
    step(out.TryAppend(StringView(id.Data(), ShardChars)));
    step(out.TryAppendAscii('/'));
    step(out.TryAppend(id));
    step(out.TryAppendAscii('-'));
    step(out.TryAppend(StringView(hex, HashChars)));
    step(out.TryAppend(".rpkg"));
    return error == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(error);
}

} // namespace rocks
