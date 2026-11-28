#include "PackageInternal.h"

#include <AssetRuntime/CookedManifest.h>

#include <Base/Endian.h>
#include <Core/Hash/Crc32.h>
#include <Core/Text/NameComponent.h>
#include <Serialization/Archive.h>

// 쿠킹 매니페스트 v2 reader·writer·검증·대조다(Docs/Design/AssetSystem 10절, 10.1절·10.3절 4항).
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
                                3 * sizeof(uint64) + 2 * DigestBytes + 3 * sizeof(uint32);
// bundle 하나의 최소 byte다(이름 길이 칸 + 구성원 개수 칸).
constexpr usize MinBundleBytes = LengthPrefixBytes + sizeof(uint32);
constexpr usize DependencyBytes = GuidBytes + sizeof(uint32);
// backend 하나의 최소 byte다(이름 길이 칸 + 형식 개수 칸 — 10.3절 4항).
constexpr usize MinBackendBytes = LengthPrefixBytes + sizeof(uint32);
// artifact 하나의 최소 byte다(kind + 이름·formatId 길이 칸 + bytes + sha256 + cookKey).
constexpr usize MinArtifactBytes =
    sizeof(uint8) + 2 * LengthPrefixBytes + sizeof(uint64) + 2 * DigestBytes;
// artifact 경로의 SHA-256 hex 앞 글자 수다(패키지 경로와 같다 — AS 4.2절).
constexpr usize ArtifactHashChars = 16;
// 비교 키·식별자 문자열 builder의 내장 용량이다. 키 길이는 경로 상한까지 다양하므로 작게 두고
// allocator로 자라게 한다(할당 실패는 그 오류로 돌려준다).
constexpr usize KeyInlineBytes = 8;
// u16 엔진 버전 칸의 상한이다.
constexpr uint32 MaxEngineVersionPart = 0xFFFF;

using Field = CookedManifestField;

template <class T>
Result<T> ManifestCorrupt(AssetDiagnostic* failure, Field field, uint64 detail, AssetId asset = {},
                          AssetId related = {}) noexcept {
    return FailAsset<T>(failure, AssetDiagnosticCode::CorruptPackage, ErrorCode::CorruptData, asset,
                        static_cast<uint32>(field), detail, related);
}

template <class T>
Result<T> ManifestResource(AssetDiagnostic* failure, ErrorCode error,
                           uint64 detail = CookedManifestNoEntry, AssetId asset = {}) noexcept {
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

// backend 이름 어휘다(10.3절 1항).
constexpr StringView KnownGraphicsBackends[] = {"D3D12", "D3D11", "Vulkan", "Metal", "OpenGL"};

// artifact 키 비교다(음수·0·양수 — kind 값 → 이름 byte 순 → formatId byte 순).
int CompareArtifactKey(CookArtifactKind leftKind, StringView leftName, StringView leftFormat,
                       CookArtifactKind rightKind, StringView rightName,
                       StringView rightFormat) noexcept {
    const uint8 a = static_cast<uint8>(leftKind);
    const uint8 b = static_cast<uint8>(rightKind);
    if (a != b) {
        return a < b ? -1 : 1;
    }

    if (!(leftName == rightName)) {
        return ByteLess(leftName, rightName) ? -1 : 1;
    }

    if (!(leftFormat == rightFormat)) {
        return ByteLess(leftFormat, rightFormat) ? -1 : 1;
    }

    return 0;
}

bool IsKnownArtifactName(StringView name) noexcept {
    return name == CookedArtifactEngineName || name == CookedArtifactProjectName;
}

bool IsKnownArtifactKind(CookArtifactKind kind) noexcept {
    return kind == CookArtifactKind::ShaderLibrary;
}

// backend 표(10.3절 4항 — 번호 순: 이름 어휘 → 이름 중복 → 형식 수 → 형식 id 문법 → 전역 중복)다.
Result<void> ValidateBackends(const CookedManifest& manifest, AssetDiagnostic* failure) noexcept {
    const Array<CookedManifestBackend>& backends = manifest.backends;
    if (backends.Size() > CookedManifestMaxBackends) {
        return ManifestCorrupt<void>(failure, Field::BackendCount, CookedManifestNoEntry);
    }

    for (usize index = 0; index < backends.Size(); ++index) {
        const CookedManifestBackend& backend = backends[index];
        if (!IsKnownGraphicsBackend(backend.name.View())) {
            return ManifestCorrupt<void>(failure, Field::Backend, index);
        }

        for (usize earlier = 0; earlier < index; ++earlier) {
            if (backends[earlier].name.View() == backend.name.View()) {
                return ManifestCorrupt<void>(failure, Field::Backend, index);
            }
        }

        const usize formats = backend.formats.Size();
        if (formats == 0 || formats > CookedManifestMaxFormatsPerBackend) {
            return ManifestCorrupt<void>(failure, Field::FormatCount, index);
        }

        for (usize format = 0; format < formats; ++format) {
            const StringView id = backend.formats[format].View();
            if (!IsValidShaderFormatId(id)) {
                return ManifestCorrupt<void>(failure, Field::ShaderFormat, index);
            }

            // 앞 형식 전부(앞 backend 전부와 같은 backend의 앞 형식)와 다르다.
            for (usize other = 0; other <= index; ++other) {
                const usize end = other == index ? format : backends[other].formats.Size();
                for (usize at = 0; at < end; ++at) {
                    if (backends[other].formats[at].View() == id) {
                        return ManifestCorrupt<void>(failure, Field::ShaderFormat, index);
                    }
                }
            }
        }
    }

    return Result<void>::Success();
}

bool HasShaderFormat(const CookedManifest& manifest, StringView id) noexcept {
    for (const CookedManifestBackend& backend : manifest.backends) {
        for (const String& format : backend.formats) {
            if (format.View() == id) {
                return true;
            }
        }
    }

    return false;
}

// artifact 표(10.3절 4항 — 번호 순: kind → 이름 → formatId가 backend 표에 있음 → bytes → 정렬)다.
Result<void> ValidateArtifacts(const CookedManifest& manifest, AssetDiagnostic* failure) noexcept {
    const Array<CookedManifestArtifact>& artifacts = manifest.artifacts;
    if (artifacts.Size() > CookedManifestMaxArtifacts) {
        return ManifestCorrupt<void>(failure, Field::ArtifactCount, CookedManifestNoEntry);
    }

    for (usize index = 0; index < artifacts.Size(); ++index) {
        const CookedManifestArtifact& artifact = artifacts[index];
        if (!IsKnownArtifactKind(artifact.kind)) {
            return ManifestCorrupt<void>(failure, Field::ArtifactKind, index);
        }

        if (!IsKnownArtifactName(artifact.name.View())) {
            return ManifestCorrupt<void>(failure, Field::ArtifactName, index);
        }

        if (!HasShaderFormat(manifest, artifact.formatId.View())) {
            return ManifestCorrupt<void>(failure, Field::ArtifactFormat, index);
        }

        if (artifact.bytes == 0 || artifact.bytes > CookedArtifactMaxBytes) {
            return ManifestCorrupt<void>(failure, Field::ArtifactBytes, index);
        }

        if (index > 0 && !CookedArtifactLess(artifacts[index - 1], artifact)) {
            return ManifestCorrupt<void>(failure, Field::ArtifactOrder, index);
        }
    }

    return Result<void>::Success();
}

// (asset, kind) 순서다.
bool ManifestDependencyLess(const CookedManifestDependency& left,
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
    if (error == ErrorCode::InvalidArgument || error == ErrorCode::CorruptData) {
        return ManifestCorrupt<T>(failure, field, detail, asset);
    }

    return ManifestResource<T>(failure, error, detail, asset);
}

Result<void> ValidateBundles(const CookedManifestEntry& entry, uint64 index, Allocator& scratch,
                             AssetDiagnostic* failure) noexcept {
    const usize count = entry.bundles.Size();
    if (count > CookedManifestMaxBundles ||
        (count != 0 &&
         (entry.flags & static_cast<uint32>(CookedManifestFlags::PrimaryAsset)) == 0)) {
        return ManifestCorrupt<void>(failure, Field::BundleCount, index, entry.asset);
    }

    Array<String> keys(scratch);
    if (auto reserved = keys.TryReserve(count); !reserved) {
        return ManifestResource<void>(failure, reserved.Error(), index, entry.asset);
    }

    for (usize bundle = 0; bundle < count; ++bundle) {
        const StringView name = entry.bundles[bundle].name.View();
        if (auto valid = ValidateNameComponent(name, scratch); !valid) {
            return FieldFailure<void>(failure, valid.Error(), Field::Bundle, index, entry.asset);
        }

        if (bundle > 0 && !ByteLess(entry.bundles[bundle - 1].name.View(), name)) {
            return ManifestCorrupt<void>(failure, Field::Bundle, index, entry.asset);
        }

        StringBuilder<KeyInlineBytes> key(scratch);
        if (auto made = TryMakeNameComponentKey(name, key); !made) {
            return ManifestResource<void>(failure, made.Error(), index, entry.asset);
        }

        // keys는 bundle 수만큼 예약했다(빈 String 추가는 할당하지 않는다).
        String& copy = *keys.TryEmplaceBack(scratch).Value();
        if (auto copied = copy.TryAppend(key.View()); !copied) {
            return ManifestResource<void>(failure, copied.Error(), index, entry.asset);
        }

        for (usize other = 0; other < bundle; ++other) {
            if (keys[other].View() == key.View()) {
                return ManifestCorrupt<void>(failure, Field::Bundle, index, entry.asset);
            }
        }
    }

    return Result<void>::Success();
}

// bundle 구성원(AS 10.1절 5 — 의존 검증 뒤): 개수 상한, AssetId 엄격 오름차순, null·자기 자신 금지,
// 그 항목의 런타임 의존 대상. 구성원과 의존은 모두 AssetId 오름차순이라 함께 걸으며 본다.
Result<void> ValidateBundleMembers(const CookedManifestEntry& entry, uint64 index,
                                   AssetDiagnostic* failure) noexcept {
    for (const CookedManifestBundle& bundle : entry.bundles) {
        if (bundle.members.Size() > CookedManifestMaxBundleMembers) {
            return ManifestCorrupt<void>(failure, Field::BundleMemberCount, index, entry.asset);
        }

        usize dependency = 0;
        for (usize position = 0; position < bundle.members.Size(); ++position) {
            const AssetId member = bundle.members[position];
            if (member.IsNull() || member == entry.asset ||
                (position > 0 && !GuidLess(bundle.members[position - 1].value, member.value))) {
                return ManifestCorrupt<void>(failure, Field::BundleMember, index, entry.asset,
                                             member);
            }

            while (dependency < entry.dependencies.Size() &&
                   GuidLess(entry.dependencies[dependency].asset.value, member.value)) {
                ++dependency;
            }

            if (dependency == entry.dependencies.Size() ||
                !(entry.dependencies[dependency].asset == member)) {
                return ManifestCorrupt<void>(failure, Field::BundleMember, index, entry.asset,
                                             member);
            }
        }
    }

    return Result<void>::Success();
}

Result<void> ValidateManifestDependencies(const CookedManifestEntry& entry, uint64 index,
                                          AssetDiagnostic* failure) noexcept {
    const usize count = entry.dependencies.Size();
    if (count > CookedManifestMaxDependencies) {
        return ManifestCorrupt<void>(failure, Field::DependencyCount, index, entry.asset);
    }

    for (usize position = 0; position < count; ++position) {
        const CookedManifestDependency& dependency = entry.dependencies[position];
        const bool runtimeKind = dependency.kind == PackageDependencyKind::Hard ||
                                 dependency.kind == PackageDependencyKind::Soft;
        if (dependency.asset.IsNull() || dependency.asset == entry.asset || !runtimeKind ||
            (position > 0 &&
             !ManifestDependencyLess(entry.dependencies[position - 1], dependency))) {
            return ManifestCorrupt<void>(failure, Field::Dependency, index, entry.asset,
                                         dependency.asset);
        }
    }

    return Result<void>::Success();
}

// 항목 하나의 칸 규칙이다(항목 사이 규칙 — 정렬·경로 유일성·Hard 대상 — 은 호출자).
Result<void> ValidateEntry(const CookedManifestEntry& entry, uint64 index, Allocator& scratch,
                           StringBuilderBase& pathKey, AssetDiagnostic* failure) noexcept {
    if (entry.type == TypeId{}) {
        return ManifestCorrupt<void>(failure, Field::TypeId, index, entry.asset);
    }

    if (auto path = CheckPathHint(entry.pathHint.View(), scratch, pathKey); !path) {
        return FieldFailure<void>(failure, path.Error(), Field::PathHint, index, entry.asset);
    }

    if (entry.packageBytes < PackageHeaderBytes || entry.packageBytes > PackageMaxFileBytes) {
        return ManifestCorrupt<void>(failure, Field::PackageBytes, index, entry.asset);
    }

    // sidecar가 없으면(0 byte) SHA-256도 0이다(AS 20.1 5).
    if (entry.sidecarBytes == 0 && !SameDigest(entry.sidecarSha256, Sha256Digest{})) {
        return ManifestCorrupt<void>(failure, Field::SidecarSha256, index, entry.asset);
    }

    if ((entry.flags & ~CookedManifestFlagsMask) != 0) {
        return ManifestCorrupt<void>(failure, Field::Flags, index, entry.asset);
    }

    if (auto bundles = ValidateBundles(entry, index, scratch, failure); !bundles) {
        return bundles;
    }

    if (auto dependencies = ValidateManifestDependencies(entry, index, failure); !dependencies) {
        return dependencies;
    }

    return ValidateBundleMembers(entry, index, failure);
}

} // namespace

bool IsKnownGraphicsBackend(StringView name) noexcept {
    for (StringView known : KnownGraphicsBackends) {
        if (known == name) {
            return true;
        }
    }

    return false;
}

bool IsValidShaderFormatId(StringView id) noexcept {
    if (id.IsEmpty() || id.ByteCount() > ShaderFormatIdMaxBytes) {
        return false;
    }

    for (usize index = 0; index < id.ByteCount(); ++index) {
        const char c = id.Data()[index];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }

    return true;
}

bool CookedArtifactLess(const CookedManifestArtifact& left,
                        const CookedManifestArtifact& right) noexcept {
    return CompareArtifactKey(left.kind, left.name.View(), left.formatId.View(), right.kind,
                              right.name.View(), right.formatId.View()) < 0;
}

const CookedManifestArtifact* CookedManifest::FindArtifact(CookArtifactKind kind, StringView name,
                                                           StringView formatId) const noexcept {
    usize low = 0;
    usize high = artifacts.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const CookedManifestArtifact& probe = artifacts[middle];
        if (CompareArtifactKey(probe.kind, probe.name.View(), probe.formatId.View(), kind, name,
                               formatId) < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    if (low == artifacts.Size()) {
        return nullptr;
    }

    const CookedManifestArtifact& found = artifacts[low];
    return CompareArtifactKey(found.kind, found.name.View(), found.formatId.View(), kind, name,
                              formatId) == 0
               ? &found
               : nullptr;
}

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

namespace {

// 모델 규칙 판정이다: 위반은 CorruptPackage / CorruptData(읽기 — 파싱한 모델의 손상). 공개
// ValidateCookedManifest·쓰기는 호출자가 넘긴 모델이라 InvalidArgument로 바꾼다(AssetSystem 19장).
Result<void> ValidateManifestModel(const CookedManifest& manifest, Allocator& scratch,
                                   AssetDiagnostic* failure) noexcept;

} // namespace

Result<void> ValidateCookedManifest(const CookedManifest& manifest, Allocator& scratch,
                                    AssetDiagnostic* failure) noexcept {
    auto valid = ValidateManifestModel(manifest, scratch, failure);
    if (!valid && valid.Error() == ErrorCode::CorruptData) {
        if (failure != nullptr) {
            failure->error = ErrorCode::InvalidArgument;
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return valid;
}

namespace {

Result<void> ValidateManifestModel(const CookedManifest& manifest, Allocator& scratch,
                                   AssetDiagnostic* failure) noexcept {
    const StringView profile = manifest.profileId.View();
    // String은 유효한 UTF-8만 담으므로(Core String 불변식) UTF-8은 다시 보지 않는다 — byte의
    // UTF-8은 reader의 문자열 읽기가 본다.
    if (profile.IsEmpty() || profile.ByteCount() > PackageMaxStringBytes) {
        return ManifestCorrupt<void>(failure, Field::ProfileId, CookedManifestNoEntry);
    }

    const usize count = manifest.entries.Size();
    if (count > CookedManifestMaxEntries) {
        return ManifestCorrupt<void>(failure, Field::EntryCount, CookedManifestNoEntry);
    }

    if (auto backends = ValidateBackends(manifest, failure); !backends) {
        return backends;
    }

    // 경로 힌트 caseless 키와 그 항목 번호다(정렬해 이웃만 비교한다).
    Array<String> keys(scratch);
    Array<uint32> order(scratch);
    if (auto reserved = keys.TryReserve(count); !reserved) {
        return ManifestResource<void>(failure, reserved.Error());
    }

    if (auto reserved = order.TryReserve(count); !reserved) {
        return ManifestResource<void>(failure, reserved.Error());
    }

    for (usize index = 0; index < count; ++index) {
        const CookedManifestEntry& entry = manifest.entries[index];
        if (entry.asset.IsNull() ||
            (index > 0 && !GuidLess(manifest.entries[index - 1].asset.value, entry.asset.value))) {
            return ManifestCorrupt<void>(failure, Field::AssetId, index, entry.asset);
        }

        StringBuilder<KeyInlineBytes> key(scratch);
        if (auto valid = ValidateEntry(entry, index, scratch, key, failure); !valid) {
            return valid;
        }

        // keys는 항목 수만큼 예약했다(빈 String 추가는 할당하지 않는다).
        String& copy = *keys.TryEmplaceBack(scratch).Value();
        if (auto copied = copy.TryAppend(key.View()); !copied) {
            return ManifestResource<void>(failure, copied.Error(), index, entry.asset);
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
            return ManifestCorrupt<void>(failure, Field::PathHint, index,
                                         manifest.entries[index].asset);
        }
    }

    // Hard 의존 대상은 매니페스트에 있다(쿠킹은 Hard 전이 폐포 — AS 10.1절 6).
    for (usize index = 0; index < count; ++index) {
        const CookedManifestEntry& entry = manifest.entries[index];
        for (const CookedManifestDependency& dependency : entry.dependencies) {
            if (dependency.kind == PackageDependencyKind::Hard &&
                manifest.Find(dependency.asset) == nullptr) {
                return ManifestCorrupt<void>(failure, Field::Dependency, index, entry.asset,
                                             dependency.asset);
            }
        }

        // bundle 구성원도 매니페스트에 있다(구성원은 CookSoftReferences와 무관하게 쿠킹한다).
        for (const CookedManifestBundle& bundle : entry.bundles) {
            for (AssetId member : bundle.members) {
                if (manifest.Find(member) == nullptr) {
                    return ManifestCorrupt<void>(failure, Field::BundleMember, index, entry.asset,
                                                 member);
                }
            }
        }
    }

    return ValidateArtifacts(manifest, failure);
}

} // namespace

namespace package_detail {

Result<void> CheckCookedPathHint(StringView path, Allocator& scratch,
                                 StringBuilderBase& key) noexcept {
    return CheckPathHint(path, scratch, key);
}

bool Utf8ByteLess(StringView left, StringView right) noexcept {
    return ByteLess(left, right);
}

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
    step(writer.WriteU32(static_cast<uint32>(manifest.backends.Size())));
    for (const CookedManifestBackend& backend : manifest.backends) {
        step(writer.WriteString(backend.name.View()));
        step(writer.WriteU32(static_cast<uint32>(backend.formats.Size())));
        for (const String& format : backend.formats) {
            step(writer.WriteString(format.View()));
        }
    }

    for (usize index = 0; error == ErrorCode::None && index < manifest.entries.Size(); ++index) {
        const CookedManifestEntry& entry = manifest.entries[index];
        step(writer.WriteGuid(entry.asset.value));
        step(writer.WriteGuid(entry.type));
        step(writer.WriteString(entry.pathHint.View()));
        step(writer.WriteRaw(Span<const uint8>(entry.packageSha256.bytes, DigestBytes)));
        step(writer.WriteU64(entry.packageBytes));
        step(writer.WriteU64(entry.bulkRawBytes));
        step(writer.WriteU64(entry.sidecarBytes));
        step(writer.WriteRaw(Span<const uint8>(entry.sidecarSha256.bytes, DigestBytes)));
        step(writer.WriteRaw(Span<const uint8>(entry.cookKey.bytes, DigestBytes)));
        step(writer.WriteU32(entry.flags));
        step(writer.WriteU32(static_cast<uint32>(entry.bundles.Size())));
        for (const CookedManifestBundle& bundle : entry.bundles) {
            step(writer.WriteString(bundle.name.View()));
            step(writer.WriteU32(static_cast<uint32>(bundle.members.Size())));
            for (AssetId member : bundle.members) {
                step(writer.WriteGuid(member.value));
            }
        }

        step(writer.WriteU32(static_cast<uint32>(entry.dependencies.Size())));
        for (const CookedManifestDependency& dependency : entry.dependencies) {
            step(writer.WriteGuid(dependency.asset.value));
            step(writer.WriteU32(static_cast<uint32>(dependency.kind)));
        }
    }

    step(writer.WriteU32(static_cast<uint32>(manifest.artifacts.Size())));
    for (usize index = 0; error == ErrorCode::None && index < manifest.artifacts.Size(); ++index) {
        const CookedManifestArtifact& artifact = manifest.artifacts[index];
        step(writer.WriteU8(static_cast<uint8>(artifact.kind)));
        step(writer.WriteString(artifact.name.View()));
        step(writer.WriteString(artifact.formatId.View()));
        step(writer.WriteU64(artifact.bytes));
        step(writer.WriteRaw(Span<const uint8>(artifact.sha256.bytes, DigestBytes)));
        step(writer.WriteRaw(Span<const uint8>(artifact.cookKey.bytes, DigestBytes)));
    }

    // CRC32 자리를 같이 써 두고 다 쓴 뒤 채운다(AS 10.1절 1 — 앞의 모든 byte).
    step(writer.WriteU32(0));
    if (error != ErrorCode::None) {
        return ManifestResource<void>(failure, error);
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
        return ManifestCorrupt<Out>(failure, Field::File, CookedManifestNoEntry);
    }

    for (usize index = 0; index < MagicBytes; ++index) {
        if (bytes[index] != CookedManifestMagic[index]) {
            return ManifestCorrupt<Out>(failure, Field::Magic, CookedManifestNoEntry);
        }
    }

    if (LoadLittleEndian<uint32>(bytes.Data() + MagicBytes) != CookedManifestFormatVersion) {
        return FailAsset<Out>(failure, AssetDiagnosticCode::UnsupportedVersion,
                              ErrorCode::Unsupported, {}, static_cast<uint32>(Field::FormatVersion),
                              CookedManifestNoEntry);
    }

    const usize body = bytes.Size() - sizeof(uint32);
    if (Crc32(bytes.Slice(0, body)) != LoadLittleEndian<uint32>(bytes.Data() + body)) {
        return ManifestCorrupt<Out>(failure, Field::Crc, CookedManifestNoEntry);
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
        return ManifestCorrupt<Out>(failure, Field::EntryCount, CookedManifestNoEntry);
    }

    auto profile = in.ReadStringView();
    if (!profile) {
        return ManifestCorrupt<Out>(failure, Field::ProfileId, CookedManifestNoEntry);
    }

    if (auto assigned = manifest.profileId.TryAssign(profile.Value()); !assigned) {
        return ManifestResource<Out>(failure, assigned.Error());
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
        return ManifestCorrupt<Out>(failure, Field::ProfileSettings, CookedManifestNoEntry);
    }

    if (!readDigest(manifest.cookSettings)) {
        return ManifestCorrupt<Out>(failure, Field::CookSettings, CookedManifestNoEntry);
    }

    auto cooker = in.ReadU32();
    if (!cooker) {
        return ManifestCorrupt<Out>(failure, Field::CookerVersion, CookedManifestNoEntry);
    }

    manifest.cookerVersion = cooker.Value();
    uint16* const parts[3] = {&manifest.cookedBy.majorVersion, &manifest.cookedBy.minorVersion,
                              &manifest.cookedBy.patchVersion};
    for (uint16* part : parts) {
        auto value = in.ReadU32();
        if (!value || value.Value() > MaxEngineVersionPart) {
            return ManifestCorrupt<Out>(failure, Field::EngineVersion, CookedManifestNoEntry);
        }

        *part = static_cast<uint16>(value.Value());
    }

    // backend 표(10.3절 4항): 개수는 상한과 남은 byte로 먼저 묶는다.
    auto backendCount = in.ReadU32();
    if (!backendCount || backendCount.Value() > CookedManifestMaxBackends ||
        backendCount.Value() > in.Remaining() / MinBackendBytes) {
        return ManifestCorrupt<Out>(failure, Field::BackendCount, CookedManifestNoEntry);
    }

    if (auto reserved = manifest.backends.TryReserve(backendCount.Value()); !reserved) {
        return ManifestResource<Out>(failure, reserved.Error());
    }

    for (uint32 index = 0; index < backendCount.Value(); ++index) {
        auto name = in.ReadStringView();
        if (!name) {
            return ManifestCorrupt<Out>(failure, Field::Backend, index);
        }

        // backend 수만큼 예약했다(빈 이름·형식 목록 추가는 할당하지 않는다).
        CookedManifestBackend& backend = *manifest.backends.TryEmplaceBack(allocator).Value();
        if (auto assigned = backend.name.TryAssign(name.Value()); !assigned) {
            return ManifestResource<Out>(failure, assigned.Error(), index);
        }

        auto formats = in.ReadU32();
        if (!formats || formats.Value() > CookedManifestMaxFormatsPerBackend ||
            formats.Value() > in.Remaining() / LengthPrefixBytes) {
            return ManifestCorrupt<Out>(failure, Field::FormatCount, index);
        }

        if (auto reserved = backend.formats.TryReserve(formats.Value()); !reserved) {
            return ManifestResource<Out>(failure, reserved.Error(), index);
        }

        for (uint32 format = 0; format < formats.Value(); ++format) {
            auto id = in.ReadStringView();
            if (!id) {
                return ManifestCorrupt<Out>(failure, Field::ShaderFormat, index);
            }

            // 형식 수만큼 예약했다.
            String& slot = *backend.formats.TryEmplaceBack(allocator).Value();
            if (auto assigned = slot.TryAssign(id.Value()); !assigned) {
                return ManifestResource<Out>(failure, assigned.Error(), index);
            }
        }
    }

    if (auto reserved = manifest.entries.TryReserve(count.Value()); !reserved) {
        return ManifestResource<Out>(failure, reserved.Error());
    }

    for (uint32 index = 0; index < count.Value(); ++index) {
        auto added = manifest.entries.TryEmplaceBack(allocator);
        // 개수만큼 예약했다.
        CookedManifestEntry& entry = *added.Value();
        auto asset = in.ReadGuid();
        if (!asset) {
            return ManifestCorrupt<Out>(failure, Field::AssetId, index);
        }

        entry.asset = AssetId{asset.Value()};
        auto type = in.ReadGuid();
        if (!type) {
            return ManifestCorrupt<Out>(failure, Field::TypeId, index, entry.asset);
        }

        entry.type = type.Value();
        auto path = in.ReadStringView();
        if (!path) {
            return ManifestCorrupt<Out>(failure, Field::PathHint, index, entry.asset);
        }

        if (auto assigned = entry.pathHint.TryAssign(path.Value()); !assigned) {
            return ManifestResource<Out>(failure, assigned.Error(), index, entry.asset);
        }

        if (!readDigest(entry.packageSha256)) {
            return ManifestCorrupt<Out>(failure, Field::PackageSha256, index, entry.asset);
        }

        auto packageBytes = in.ReadU64();
        if (!packageBytes) {
            return ManifestCorrupt<Out>(failure, Field::PackageBytes, index, entry.asset);
        }

        entry.packageBytes = packageBytes.Value();
        auto rawBytes = in.ReadU64();
        if (!rawBytes) {
            return ManifestCorrupt<Out>(failure, Field::BulkRawBytes, index, entry.asset);
        }

        entry.bulkRawBytes = rawBytes.Value();
        auto sidecarBytes = in.ReadU64();
        if (!sidecarBytes) {
            return ManifestCorrupt<Out>(failure, Field::SidecarBytes, index, entry.asset);
        }

        entry.sidecarBytes = sidecarBytes.Value();
        if (!readDigest(entry.sidecarSha256)) {
            return ManifestCorrupt<Out>(failure, Field::SidecarSha256, index, entry.asset);
        }

        if (!readDigest(entry.cookKey)) {
            return ManifestCorrupt<Out>(failure, Field::CookKey, index, entry.asset);
        }

        auto flags = in.ReadU32();
        if (!flags) {
            return ManifestCorrupt<Out>(failure, Field::Flags, index, entry.asset);
        }

        entry.flags = flags.Value();
        auto bundles = in.ReadU32();
        if (!bundles || bundles.Value() > CookedManifestMaxBundles ||
            bundles.Value() > in.Remaining() / MinBundleBytes) {
            return ManifestCorrupt<Out>(failure, Field::BundleCount, index, entry.asset);
        }

        if (auto reserved = entry.bundles.TryReserve(bundles.Value()); !reserved) {
            return ManifestResource<Out>(failure, reserved.Error(), index, entry.asset);
        }

        for (uint32 bundle = 0; bundle < bundles.Value(); ++bundle) {
            auto name = in.ReadStringView();
            if (!name) {
                return ManifestCorrupt<Out>(failure, Field::Bundle, index, entry.asset);
            }

            // bundle 수만큼 예약했다(빈 이름·구성원 추가는 할당하지 않는다).
            CookedManifestBundle& slot = *entry.bundles.TryEmplaceBack(allocator).Value();
            if (auto assigned = slot.name.TryAssign(name.Value()); !assigned) {
                return ManifestResource<Out>(failure, assigned.Error(), index, entry.asset);
            }

            // 구성원 개수는 남은 byte로 먼저 묶는다(작은 파일이 큰 할당을 요구하지 못하게).
            auto members = in.ReadU32();
            if (!members || members.Value() > CookedManifestMaxBundleMembers ||
                members.Value() > in.Remaining() / GuidBytes) {
                return ManifestCorrupt<Out>(failure, Field::BundleMemberCount, index, entry.asset);
            }

            if (auto reserved = slot.members.TryReserve(members.Value()); !reserved) {
                return ManifestResource<Out>(failure, reserved.Error(), index, entry.asset);
            }

            for (uint32 member = 0; member < members.Value(); ++member) {
                // 개수를 남은 byte로 묶었다 — 구성원 칸은 잘리지 않는다(예약도 했다).
                (void)slot.members.TryPushBack(AssetId{in.ReadGuid().Value()});
            }
        }

        auto dependencies = in.ReadU32();
        if (!dependencies || dependencies.Value() > CookedManifestMaxDependencies ||
            dependencies.Value() > in.Remaining() / DependencyBytes) {
            return ManifestCorrupt<Out>(failure, Field::DependencyCount, index, entry.asset);
        }

        if (auto reserved = entry.dependencies.TryReserve(dependencies.Value()); !reserved) {
            return ManifestResource<Out>(failure, reserved.Error(), index, entry.asset);
        }

        for (uint32 dependency = 0; dependency < dependencies.Value(); ++dependency) {
            // 개수를 남은 byte로 묶었으므로 의존 칸은 잘리지 않는다.
            const Guid target = in.ReadGuid().Value();
            const uint32 kind = in.ReadU32().Value();
            (void)entry.dependencies.TryPushBack(CookedManifestDependency{
                AssetId{target}, static_cast<PackageDependencyKind>(kind)});
        }
    }

    // artifact 표(10.3절 4항): 개수는 상한과 남은 byte로 먼저 묶는다.
    auto artifactCount = in.ReadU32();
    if (!artifactCount || artifactCount.Value() > CookedManifestMaxArtifacts ||
        artifactCount.Value() > in.Remaining() / MinArtifactBytes) {
        return ManifestCorrupt<Out>(failure, Field::ArtifactCount, CookedManifestNoEntry);
    }

    if (auto reserved = manifest.artifacts.TryReserve(artifactCount.Value()); !reserved) {
        return ManifestResource<Out>(failure, reserved.Error());
    }

    for (uint32 index = 0; index < artifactCount.Value(); ++index) {
        // artifact 수만큼 예약했다(빈 이름 추가는 할당하지 않는다).
        CookedManifestArtifact& artifact = *manifest.artifacts.TryEmplaceBack(allocator).Value();
        auto kind = in.ReadU8();
        if (!kind) {
            return ManifestCorrupt<Out>(failure, Field::ArtifactKind, index);
        }

        // 모르는 값도 담는다 — 칸 규칙(ValidateCookedManifest)이 ArtifactKind로 거부한다.
        artifact.kind = static_cast<CookArtifactKind>(kind.Value());
        auto name = in.ReadStringView();
        if (!name) {
            return ManifestCorrupt<Out>(failure, Field::ArtifactName, index);
        }

        if (auto assigned = artifact.name.TryAssign(name.Value()); !assigned) {
            return ManifestResource<Out>(failure, assigned.Error(), index);
        }

        auto format = in.ReadStringView();
        if (!format) {
            return ManifestCorrupt<Out>(failure, Field::ArtifactFormat, index);
        }

        if (auto assigned = artifact.formatId.TryAssign(format.Value()); !assigned) {
            return ManifestResource<Out>(failure, assigned.Error(), index);
        }

        auto bytesField = in.ReadU64();
        if (!bytesField) {
            return ManifestCorrupt<Out>(failure, Field::ArtifactBytes, index);
        }

        artifact.bytes = bytesField.Value();
        if (!readDigest(artifact.sha256)) {
            return ManifestCorrupt<Out>(failure, Field::ArtifactSha256, index);
        }

        if (!readDigest(artifact.cookKey)) {
            return ManifestCorrupt<Out>(failure, Field::ArtifactCookKey, index);
        }
    }

    if (!in.AtEnd()) {
        return ManifestCorrupt<Out>(failure, Field::File, CookedManifestNoEntry);
    }

    if (auto valid = ValidateManifestModel(manifest, allocator, failure); !valid) {
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
    // 크기·SHA-256을 먼저 본다(바뀐 파일을 해석한 결과로 판정하지 않는다).
    if (auto checked = CheckCookedPackageFile(entry, file, failure); !checked) {
        return checked;
    }

    return CheckCookedPackageContents(manifest, entry, contents, failure);
}

Result<void> CheckCookedPackageFile(const CookedManifestEntry& entry, Span<const uint8> file,
                                    AssetDiagnostic* failure) noexcept {
    const AssetId asset = entry.asset;
    if (file.Size() != entry.packageBytes) {
        return ManifestCorrupt<void>(failure, Field::PackageBytes, CookedManifestNoEntry, asset);
    }

    if (!SameDigest(Sha256::Hash(file), entry.packageSha256)) {
        return ManifestCorrupt<void>(failure, Field::PackageSha256, CookedManifestNoEntry, asset);
    }

    return Result<void>::Success();
}

Result<void> CheckCookedPackageContents(const CookedManifest& manifest,
                                        const CookedManifestEntry& entry,
                                        const PackageContents& contents,
                                        AssetDiagnostic* failure) noexcept {
    const AssetId asset = entry.asset;
    if (contents.header.kind != PackageKind::Cooked) {
        return ManifestCorrupt<void>(failure, Field::PackageKind, CookedManifestNoEntry, asset);
    }

    if (!(contents.header.assetId == asset)) {
        return ManifestCorrupt<void>(failure, Field::AssetId, CookedManifestNoEntry, asset);
    }

    if (contents.exports.IsEmpty() || !(contents.exports[0].type == entry.type)) {
        return ManifestCorrupt<void>(failure, Field::TypeId, CookedManifestNoEntry, asset);
    }

    // 합이 u64를 넘으면(호출자가 준 내용이 손상) 같은 값일 수 없으므로 불일치다.
    uint64 rawBytes = 0;
    bool overflow = false;
    for (const PackageBulkEntry& bulk : contents.bulkEntries) {
        // sidecar entry는 상주하지 않으므로 bulkRawBytes에 들지 않는다(AS 20.1 4).
        if ((bulk.flags & static_cast<uint32>(BulkEntryFlags::Sidecar)) != 0) {
            continue;
        }

        if (bulk.rawBytes > NumericLimits<uint64>::Max() - rawBytes) {
            overflow = true;
            break;
        }

        rawBytes += bulk.rawBytes;
    }

    if (overflow || rawBytes != entry.bulkRawBytes) {
        return ManifestCorrupt<void>(failure, Field::BulkRawBytes, CookedManifestNoEntry, asset);
    }

    // 패키지 Sidecar 섹션의 정체가 매니페스트와 같다(AS 20.1 5).
    const uint64 sidecarBytes = contents.sidecarPresent ? contents.sidecar.fileBytes : 0;
    if (sidecarBytes != entry.sidecarBytes) {
        return ManifestCorrupt<void>(failure, Field::SidecarBytes, CookedManifestNoEntry, asset);
    }

    if (contents.sidecarPresent && !SameDigest(contents.sidecar.sha256, entry.sidecarSha256)) {
        return ManifestCorrupt<void>(failure, Field::SidecarSha256, CookedManifestNoEntry, asset);
    }

    const PackageSummary& summary = contents.summary;
    if (!(summary.profileId.View() == manifest.profileId.View()) ||
        !SameDigest(summary.profileSettings, manifest.profileSettings) ||
        summary.cookerVersion != manifest.cookerVersion) {
        return ManifestCorrupt<void>(failure, Field::PackageProfile, CookedManifestNoEntry, asset);
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
            return ManifestCorrupt<void>(failure, Field::PackageDependencies, CookedManifestNoEntry,
                                         asset, related);
        }
    }

    return Result<void>::Success();
}

namespace {

// 4.2절: Packages/<a>/<A>-<h><extension> — a = A의 앞 2자, h = SHA-256 앞 16자.
Result<void> AppendShardedPath(StringBuilderBase& out, AssetId asset, const Sha256Digest& sha256,
                               StringView extension) noexcept {
    constexpr usize ShardChars = 2;
    constexpr usize HashChars = 16;
    StringBuilder<KeyInlineBytes> text(out.GetAllocator());
    if (auto formatted = text.TryAppendFormat("{}", asset); !formatted) {
        return formatted;
    }

    char hex[Sha256Digest::HexBytes + 1];
    sha256.ToHex(hex);
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
    step(out.TryAppend(extension));
    return error == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(error);
}

} // namespace

Result<void> AppendCookedPackagePath(StringBuilderBase& out, AssetId asset,
                                     const Sha256Digest& packageSha256) noexcept {
    return AppendShardedPath(out, asset, packageSha256, ".rpkg");
}

Result<void> AppendCookedSidecarPath(StringBuilderBase& out, AssetId asset,
                                     const Sha256Digest& sidecarSha256) noexcept {
    // AS 20.1 3: 패키지와 같은 디렉터리 규칙, 확장자만 다르다.
    return AppendShardedPath(out, asset, sidecarSha256, ".rbsc");
}

Result<void> AppendCookedArtifactPath(StringBuilderBase& out,
                                      const CookedManifestArtifact& artifact) noexcept {
    // 10.3절 4항: Shaders/<이름>-<formatId>-<h>.rshl — h = SHA-256 앞 16자. 칸이 경로 조각이라 먼저
    // 본다(out은 그대로).
    if (!IsKnownArtifactKind(artifact.kind) || !IsKnownArtifactName(artifact.name.View()) ||
        !IsValidShaderFormatId(artifact.formatId.View())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    char hex[Sha256Digest::HexBytes + 1];
    artifact.sha256.ToHex(hex);
    ErrorCode error = ErrorCode::None;
    auto step = [&](Result<void> result) noexcept {
        if (error == ErrorCode::None && !result) {
            error = result.Error();
        }
    };
    step(out.TryAppend("Shaders/"));
    step(out.TryAppend(artifact.name.View()));
    step(out.TryAppendAscii('-'));
    step(out.TryAppend(artifact.formatId.View()));
    step(out.TryAppendAscii('-'));
    step(out.TryAppend(StringView(hex, ArtifactHashChars)));
    step(out.TryAppend(".rshl"));
    return error == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(error);
}

Result<void> CheckCookedArtifactFile(const CookedManifestArtifact& artifact, Span<const uint8> file,
                                     AssetDiagnostic* failure) noexcept {
    // 크기를 먼저 본다(CheckCookedPackageFile과 같은 순서 — 길이가 다르면 해시하지 않는다).
    if (file.Size() != artifact.bytes) {
        return ManifestCorrupt<void>(failure, Field::ArtifactBytes, CookedManifestNoEntry);
    }

    if (!SameDigest(Sha256::Hash(file), artifact.sha256)) {
        return ManifestCorrupt<void>(failure, Field::ArtifactSha256, CookedManifestNoEntry);
    }

    return Result<void>::Success();
}

} // namespace rocks
