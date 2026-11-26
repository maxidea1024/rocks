#pragma once

#include "PackageEditing.h"

#include <AssetRuntime/CookedManifest.h>
#include <Serialization/Archive.h>

// 쿠킹 매니페스트 시험 도구다(Docs/Design/AssetSystem 10.1절·10.3절 4항 v2): 모델 만들기, 검증하지
// 않는 시험 인코더(규칙을 어긴 모델을 reader에 넣는다 — writer와 같은 배치), CRC 다시 맞추기.
namespace rocks::asset_test {

// 시험 매니페스트의 고정 헤더 값이다(쿠킹 패키지 시험 옵션 CookedOptions와 같은 프로파일).
inline void FillManifestHeader(CookedManifest& manifest) noexcept {
    const PackageWriteOptions cooked = CookedOptions();
    (void)manifest.profileId.TryAssign(cooked.profileId);
    manifest.profileSettings = cooked.profileSettings;
    manifest.cookSettings =
        Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>("cook-settings"), 13));
    manifest.cookerVersion = cooked.cookerVersion;
    manifest.cookedBy = TestEngineVersion;
}

// 값 v로 채운 digest다.
inline Sha256Digest ManifestDigestOf(uint8 value) noexcept {
    Sha256Digest digest;
    for (uint8& byte : digest.bytes) {
        byte = value;
    }

    return digest;
}

// low id의 항목을 끝에 더한다(경로 /Game/<name>, 패키지 크기 1000 + low).
inline CookedManifestEntry* AddEntry(CookedManifest& manifest, uint64 low, StringView name,
                                     uint32 flags = 0) noexcept {
    auto added = manifest.entries.TryEmplaceBack(manifest.entries.GetAllocator());
    if (!added) {
        return nullptr;
    }

    CookedManifestEntry& entry = *added.Value();
    entry.asset = FixedId(low);
    entry.type = TestAssetType();
    constexpr uint64 BaseBytes = 1000;
    entry.packageBytes = BaseBytes + low;
    entry.bulkRawBytes = low;
    entry.packageSha256 = ManifestDigestOf(static_cast<uint8>(low));
    entry.cookKey = ManifestDigestOf(static_cast<uint8>(low + 1));
    entry.flags = flags;
    bool ok = entry.pathHint.TryAppend("/Game/").IsOk() && entry.pathHint.TryAppend(name).IsOk();
    return ok ? &entry : nullptr;
}

// bundle을 더한다(구성원은 low 값 목록 — 그대로 순서로 넣는다).
inline bool AddBundle(CookedManifestEntry& entry, StringView name,
                      Span<const uint64> members = {}) noexcept {
    auto added = entry.bundles.TryEmplaceBack(entry.bundles.GetAllocator());
    if (!added || !added.Value()->name.TryAssign(name)) {
        return false;
    }

    for (uint64 member : members) {
        if (!added.Value()->members.TryPushBack(FixedId(member))) {
            return false;
        }
    }

    return true;
}

inline bool AddDependency(CookedManifestEntry& entry, uint64 low,
                          PackageDependencyKind kind) noexcept {
    return entry.dependencies.TryPushBack(CookedManifestDependency{FixedId(low), kind}).IsOk();
}

// backend를 끝에 더한다(형식은 그대로 순서로 넣는다).
inline bool AddBackend(CookedManifest& manifest, StringView name,
                       Span<const StringView> formats) noexcept {
    auto added = manifest.backends.TryEmplaceBack(manifest.backends.GetAllocator());
    if (!added || !added.Value()->name.TryAssign(name)) {
        return false;
    }

    for (StringView format : formats) {
        auto slot = added.Value()->formats.TryEmplaceBack(manifest.backends.GetAllocator());
        if (!slot || !slot.Value()->TryAssign(format)) {
            return false;
        }
    }

    return true;
}

// artifact를 끝에 더한다(셰이더 라이브러리, sha256 = digest 값 v, cookKey = v + 1).
inline CookedManifestArtifact* AddArtifact(CookedManifest& manifest, StringView name,
                                           StringView formatId, uint64 bytes,
                                           uint8 digest) noexcept {
    auto added = manifest.artifacts.TryEmplaceBack(manifest.artifacts.GetAllocator());
    if (!added) {
        return nullptr;
    }

    CookedManifestArtifact& artifact = *added.Value();
    artifact.kind = CookArtifactKind::ShaderLibrary;
    artifact.bytes = bytes;
    artifact.sha256 = ManifestDigestOf(digest);
    artifact.cookKey = ManifestDigestOf(static_cast<uint8>(digest + 1));
    const bool ok =
        artifact.name.TryAssign(name).IsOk() && artifact.formatId.TryAssign(formatId).IsOk();
    return ok ? &artifact : nullptr;
}

// 시험 인코더가 덮어쓸 원시 값이다(없으면 모델 값 — AS 10.1절 1 배치 그대로).
struct ManifestRaw {
    bool overrideCount = false;
    uint32 entryCount = 0;
    // 엔진 버전 칸(0 주·1 부·2 패치)을 이 u32로 쓴다(EngineVersionPartNone이면 모델 값).
    static constexpr uint32 EngineVersionPartNone = NumericLimits<uint32>::Max();
    uint32 enginePart = EngineVersionPartNone;
    uint32 engineValue = 0;
    // 첫 항목의 bundle·의존 개수 칸을 이 값으로 쓴다(내용은 모델 그대로).
    bool overrideBundleCount = false;
    uint32 bundleCount = 0;
    bool overrideDependencyCount = false;
    uint32 dependencyCount = 0;
    // 첫 항목 첫 의존의 kind를 이 u32로 쓴다.
    bool overrideKind = false;
    uint32 kind = 0;
    // 프로파일 id 길이 칸을 이 값으로 쓴다(내용은 모델 그대로).
    bool overrideProfileLength = false;
    uint64 profileLength = 0;
    // backend 개수 칸·첫 backend의 형식 개수 칸·artifact 개수 칸을 이 값으로 쓴다(내용은 모델
    // 그대로).
    bool overrideBackendCount = false;
    uint32 backendCount = 0;
    bool overrideFormatCount = false;
    uint32 formatCount = 0;
    bool overrideArtifactCount = false;
    uint32 artifactCount = 0;
    // 다른 magic·formatVersion이다.
    uint8 magic[4] = {'R', 'M', 'A', 'N'};
    uint32 formatVersion = CookedManifestFormatVersion;
    // crc 앞에 덧붙일 byte 수다(0).
    usize trailing = 0;
};

// 검증하지 않고 manifest를 AS 10.3절 4항 v2 배치로 쓴다(끝에 CRC32).
inline Array<uint8> EncodeManifest(Allocator& allocator, const CookedManifest& manifest,
                                   const ManifestRaw& raw = {}) noexcept {
    ArchiveLimits limits;
    limits.maxTotalBytes = static_cast<usize>(CookedManifestMaxFileBytes);
    limits.maxValueBytes = static_cast<usize>(CookedManifestMaxFileBytes);
    limits.maxOperations = NumericLimits<uint64>::Max();
    limits.maxWork = NumericLimits<uint64>::Max();
    ArchiveWriter out(allocator, limits);
    bool ok = out.WriteRaw(Span<const uint8>(raw.magic, 4)).IsOk();
    ok = ok && out.WriteU32(raw.formatVersion).IsOk();
    ok = ok && out.WriteU32(raw.overrideCount ? raw.entryCount
                                              : static_cast<uint32>(manifest.entries.Size()))
                   .IsOk();
    if (raw.overrideProfileLength) {
        ok = ok && out.WriteU64(raw.profileLength).IsOk() &&
             out.WriteRaw(Span<const uint8>(
                              reinterpret_cast<const uint8*>(manifest.profileId.View().Data()),
                              manifest.profileId.View().ByteCount()))
                 .IsOk();
    } else {
        ok = ok && out.WriteString(manifest.profileId.View()).IsOk();
    }

    ok = ok && out.WriteRaw(Span<const uint8>(manifest.profileSettings.bytes, 32)).IsOk();
    ok = ok && out.WriteRaw(Span<const uint8>(manifest.cookSettings.bytes, 32)).IsOk();
    ok = ok && out.WriteU32(manifest.cookerVersion).IsOk();
    const uint32 parts[3] = {manifest.cookedBy.majorVersion, manifest.cookedBy.minorVersion,
                             manifest.cookedBy.patchVersion};
    for (uint32 part = 0; part < 3; ++part) {
        ok = ok && out.WriteU32(part == raw.enginePart ? raw.engineValue : parts[part]).IsOk();
    }

    ok =
        ok && out.WriteU32(raw.overrideBackendCount ? raw.backendCount
                                                    : static_cast<uint32>(manifest.backends.Size()))
                  .IsOk();
    for (usize index = 0; index < manifest.backends.Size(); ++index) {
        const CookedManifestBackend& backend = manifest.backends[index];
        ok = ok && out.WriteString(backend.name.View()).IsOk();
        ok = ok && out.WriteU32(index == 0 && raw.overrideFormatCount
                                    ? raw.formatCount
                                    : static_cast<uint32>(backend.formats.Size()))
                       .IsOk();
        for (const String& format : backend.formats) {
            ok = ok && out.WriteString(format.View()).IsOk();
        }
    }

    for (usize index = 0; index < manifest.entries.Size(); ++index) {
        const CookedManifestEntry& entry = manifest.entries[index];
        const bool first = index == 0;
        ok = ok && out.WriteGuid(entry.asset.value).IsOk() && out.WriteGuid(entry.type).IsOk() &&
             out.WriteString(entry.pathHint.View()).IsOk() &&
             out.WriteRaw(Span<const uint8>(entry.packageSha256.bytes, 32)).IsOk() &&
             out.WriteU64(entry.packageBytes).IsOk() && out.WriteU64(entry.bulkRawBytes).IsOk() &&
             out.WriteRaw(Span<const uint8>(entry.cookKey.bytes, 32)).IsOk() &&
             out.WriteU32(entry.flags).IsOk();
        ok = ok && out.WriteU32(first && raw.overrideBundleCount
                                    ? raw.bundleCount
                                    : static_cast<uint32>(entry.bundles.Size()))
                       .IsOk();
        for (const CookedManifestBundle& bundle : entry.bundles) {
            ok = ok && out.WriteString(bundle.name.View()).IsOk() &&
                 out.WriteU32(static_cast<uint32>(bundle.members.Size())).IsOk();
            for (AssetId member : bundle.members) {
                ok = ok && out.WriteGuid(member.value).IsOk();
            }
        }

        ok = ok && out.WriteU32(first && raw.overrideDependencyCount
                                    ? raw.dependencyCount
                                    : static_cast<uint32>(entry.dependencies.Size()))
                       .IsOk();
        for (usize position = 0; position < entry.dependencies.Size(); ++position) {
            const CookedManifestDependency& dependency = entry.dependencies[position];
            const uint32 kind = first && position == 0 && raw.overrideKind
                                    ? raw.kind
                                    : static_cast<uint32>(dependency.kind);
            ok = ok && out.WriteGuid(dependency.asset.value).IsOk() && out.WriteU32(kind).IsOk();
        }
    }

    ok = ok &&
         out.WriteU32(raw.overrideArtifactCount ? raw.artifactCount
                                                : static_cast<uint32>(manifest.artifacts.Size()))
             .IsOk();
    for (const CookedManifestArtifact& artifact : manifest.artifacts) {
        ok = ok && out.WriteU8(static_cast<uint8>(artifact.kind)).IsOk() &&
             out.WriteString(artifact.name.View()).IsOk() &&
             out.WriteString(artifact.formatId.View()).IsOk() &&
             out.WriteU64(artifact.bytes).IsOk() &&
             out.WriteRaw(Span<const uint8>(artifact.sha256.bytes, 32)).IsOk() &&
             out.WriteRaw(Span<const uint8>(artifact.cookKey.bytes, 32)).IsOk();
    }

    for (usize index = 0; ok && index < raw.trailing; ++index) {
        ok = out.WriteU8(0).IsOk();
    }

    uint8 crc[4];
    StoreLittleEndian<uint32>(crc, Crc32(out.View()));
    ok = ok && out.WriteRaw(Span<const uint8>(crc, 4)).IsOk();
    return ok ? out.TakeBytes() : Array<uint8>(allocator);
}

// 끝 4 byte CRC32를 앞의 byte로 다시 맞춘다(4 byte보다 짧으면 그대로).
inline void FixManifestCrc(Array<uint8>& bytes) noexcept {
    if (bytes.Size() < sizeof(uint32)) {
        return;
    }

    const usize body = bytes.Size() - sizeof(uint32);
    StoreLittleEndian<uint32>(bytes.Data() + body, Crc32(bytes.View().Slice(0, body)));
}

// 실패 진단이 기대 칸·항목의 CorruptPackage인지 본다.
inline bool CorruptAt(const AssetDiagnostic& failure, CookedManifestField field,
                      uint64 detail) noexcept {
    return failure.code == AssetDiagnosticCode::CorruptPackage &&
           failure.error == ErrorCode::InvalidArgument &&
           failure.section == static_cast<uint32>(field) && failure.detail == detail;
}

} // namespace rocks::asset_test
