#include <AssetRuntime/ContentSignature.h>

#include <Crypto/Bytes.h>
#include <string.h>

// 서명된 쿠킹 매니페스트 봉투의 쓰기·검증이다(Docs/Design/ContentSigning.ko.md 3·4·7절 — A1f).
namespace rocks {

namespace {

constexpr uint8 EnvelopeMagic[] = {'R', 'S', 'I', 'G'};
// 서명 메시지의 도메인 분리 label이다(끝 NUL 포함 — 3절).
constexpr uint8 SignatureLabel[] = "rocks.content-signature.v1";
constexpr usize SignatureLabelBytes = sizeof(SignatureLabel);
// 헤더 칸 offset이다(3절 표).
constexpr usize VersionOffset = 4;
constexpr usize AlgorithmOffset = 5;
constexpr usize ReservedOffset = 6;
constexpr usize KeyIdOffset = 8;
constexpr usize ContentVersionOffset = 24;
constexpr usize ScopeOffset = 32;
constexpr usize ManifestLengthOffset = 64;
constexpr usize KeyIdBytes = 16;
constexpr usize ReservedBytes = 2;
static_assert(KeyIdOffset + KeyIdBytes == ContentVersionOffset);
static_assert(ScopeOffset + Sha256Digest::Bytes == ManifestLengthOffset);
static_assert(ManifestLengthOffset + sizeof(uint64) == ContentEnvelopeHeaderBytes);

void StoreLe64(uint8* out, uint64 value) noexcept {
    for (usize index = 0; index < sizeof(uint64); ++index) {
        out[index] = static_cast<uint8>(value >> (8 * index));
    }
}

uint64 LoadLe64(const uint8* in) noexcept {
    uint64 value = 0;
    for (usize index = 0; index < sizeof(uint64); ++index) {
        value |= static_cast<uint64>(in[index]) << (8 * index);
    }

    return value;
}

// KeyId를 RFC 9562 byte 순서(BE64(high) ‖ BE64(low))로 쓴다(Crypto KeyId 규칙).
void StoreKeyId(uint8* out, crypto::KeyId id) noexcept {
    for (usize index = 0; index < sizeof(uint64); ++index) {
        out[index] = static_cast<uint8>(id.value.high >> (8 * (7 - index)));
        out[8 + index] = static_cast<uint8>(id.value.low >> (8 * (7 - index)));
    }
}

crypto::KeyId LoadKeyId(const uint8* in) noexcept {
    crypto::KeyId id;
    for (usize index = 0; index < sizeof(uint64); ++index) {
        id.value.high = (id.value.high << 8) | in[index];
        id.value.low = (id.value.low << 8) | in[8 + index];
    }

    return id;
}

bool IsScopeCharacter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '-';
}

bool IsScopeName(StringView name) noexcept {
    if (name.IsEmpty()) {
        return false;
    }

    for (char c : name) {
        if (!IsScopeCharacter(c)) {
            return false;
        }
    }

    return true;
}

bool ValidTrust(const ContentTrust& trust) noexcept {
    if (trust.keys.IsEmpty()) {
        return false;
    }

    for (usize index = 0; index < trust.keys.Size(); ++index) {
        if (trust.keys[index].id.IsNil()) {
            return false;
        }

        for (usize earlier = 0; earlier < index; ++earlier) {
            if (trust.keys[earlier].id == trust.keys[index].id) {
                return false;
            }
        }
    }

    return true;
}

Result<Span<const uint8>> EnvelopeFail(AssetDiagnostic* failure, AssetDiagnosticCode code,
                                       ErrorCode error, ContentEnvelopeField field) noexcept {
    return FailAsset<Span<const uint8>>(failure, code, error, {}, static_cast<uint32>(field));
}

} // namespace

const char* ContentEnvelopeFieldName(ContentEnvelopeField field) noexcept {
    switch (field) {
    case ContentEnvelopeField::File:
        return "File";
    case ContentEnvelopeField::Magic:
        return "Magic";
    case ContentEnvelopeField::FormatVersion:
        return "FormatVersion";
    case ContentEnvelopeField::Algorithm:
        return "Algorithm";
    case ContentEnvelopeField::Reserved:
        return "Reserved";
    case ContentEnvelopeField::Key:
        return "Key";
    case ContentEnvelopeField::ContentVersion:
        return "ContentVersion";
    case ContentEnvelopeField::Scope:
        return "Scope";
    case ContentEnvelopeField::ManifestLength:
        return "ManifestLength";
    case ContentEnvelopeField::Signature:
        return "Signature";
    }

    return "None";
}

Result<Sha256Digest> MakeContentScope(StringView productId, StringView channel) noexcept {
    if (!IsScopeName(productId) || !IsScopeName(channel)) {
        return Result<Sha256Digest>::Failure(ErrorCode::InvalidArgument);
    }

    Sha256 hash;
    constexpr uint8 Separator = 0;
    hash.Update(
        Span<const uint8>(reinterpret_cast<const uint8*>(productId.Data()), productId.ByteCount()));
    hash.Update(Span<const uint8>(&Separator, 1));
    hash.Update(
        Span<const uint8>(reinterpret_cast<const uint8*>(channel.Data()), channel.ByteCount()));
    return Result<Sha256Digest>::Success(hash.Finish());
}

Result<void> WriteContentEnvelope(Allocator& allocator, Span<const uint8> manifest,
                                  const crypto::Ed25519SigningKey& key, crypto::KeyId keyId,
                                  uint64 contentVersion, const Sha256Digest& scope,
                                  Array<uint8>& out) noexcept {
    if (keyId.IsNil()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (manifest.Size() > CookedManifestMaxFileBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    uint8 header[ContentEnvelopeHeaderBytes] = {};
    memcpy(header, EnvelopeMagic, sizeof(EnvelopeMagic));
    header[VersionOffset] = ContentEnvelopeFormatVersion;
    header[AlgorithmOffset] = static_cast<uint8>(crypto::SignatureAlgorithm::Ed25519ph);
    StoreKeyId(header + KeyIdOffset, keyId);
    StoreLe64(header + ContentVersionOffset, contentVersion);
    memcpy(header + ScopeOffset, scope.bytes, Sha256Digest::Bytes);
    StoreLe64(header + ManifestLengthOffset, manifest.Size());

    auto signer = crypto::Ed25519phSigner::TryCreate(allocator);
    if (!signer) {
        return Result<void>::Failure(signer.Error());
    }

    const Span<const uint8> pieces[] = {Span<const uint8>(SignatureLabel, SignatureLabelBytes),
                                        Span<const uint8>(header, ContentEnvelopeHeaderBytes),
                                        manifest};
    if (auto updated = signer.Value().TryUpdate(Span<const Span<const uint8>>(pieces)); !updated) {
        return updated;
    }

    auto signature = signer.Value().TryFinish(key);
    if (!signature) {
        return Result<void>::Failure(signature.Error());
    }

    Array<uint8> envelope(out.GetAllocator());
    if (auto reserved = envelope.TryReserve(ContentEnvelopeManifestOffset + manifest.Size());
        !reserved) {
        return reserved;
    }

    (void)envelope.TryAppend(Span<const uint8>(header, ContentEnvelopeHeaderBytes));
    (void)envelope.TryAppend(signature.Value().View());
    (void)envelope.TryAppend(manifest);
    out = Move(envelope);
    return Result<void>::Success();
}

Result<Span<const uint8>> VerifyContentEnvelope(Allocator& allocator, Span<const uint8> envelope,
                                                const ContentTrust& trust,
                                                ContentEnvelopeInfo* info,
                                                AssetDiagnostic* failure) noexcept {
    using Field = ContentEnvelopeField;
    if (!ValidTrust(trust)) {
        return FailAsset<Span<const uint8>>(failure, AssetDiagnosticCode::None,
                                            ErrorCode::InvalidArgument);
    }

    if (envelope.Size() < ContentEnvelopeManifestOffset ||
        envelope.Size() > ContentEnvelopeMaxBytes) {
        return EnvelopeFail(failure, AssetDiagnosticCode::CorruptPackage, ErrorCode::CorruptData,
                            Field::File);
    }

    const uint8* header = envelope.Data();
    if (memcmp(header, EnvelopeMagic, sizeof(EnvelopeMagic)) != 0) {
        return EnvelopeFail(failure, AssetDiagnosticCode::CorruptPackage, ErrorCode::CorruptData,
                            Field::Magic);
    }

    if (header[VersionOffset] != ContentEnvelopeFormatVersion) {
        return EnvelopeFail(failure, AssetDiagnosticCode::UnsupportedVersion,
                            ErrorCode::Unsupported, Field::FormatVersion);
    }

    if (header[AlgorithmOffset] != static_cast<uint8>(crypto::SignatureAlgorithm::Ed25519ph)) {
        return EnvelopeFail(failure, AssetDiagnosticCode::UnsupportedVersion,
                            ErrorCode::Unsupported, Field::Algorithm);
    }

    for (usize index = 0; index < ReservedBytes; ++index) {
        if (header[ReservedOffset + index] != 0) {
            return EnvelopeFail(failure, AssetDiagnosticCode::CorruptPackage,
                                ErrorCode::CorruptData, Field::Reserved);
        }
    }

    const crypto::KeyId keyId = LoadKeyId(header + KeyIdOffset);
    const TrustedContentKey* trusted = nullptr;
    for (const TrustedContentKey& candidate : trust.keys) {
        if (candidate.id == keyId) {
            trusted = &candidate;
            break;
        }
    }

    if (trusted == nullptr) {
        return EnvelopeFail(failure, AssetDiagnosticCode::ContentNotTrusted,
                            ErrorCode::AuthenticationFailed, Field::Key);
    }

    const uint64 contentVersion = LoadLe64(header + ContentVersionOffset);
    if (contentVersion < trust.minimumContentVersion) {
        return EnvelopeFail(failure, AssetDiagnosticCode::ContentNotTrusted,
                            ErrorCode::AuthenticationFailed, Field::ContentVersion);
    }

    Sha256Digest scope;
    memcpy(scope.bytes, header + ScopeOffset, Sha256Digest::Bytes);
    if (!(scope == trust.scope)) {
        return EnvelopeFail(failure, AssetDiagnosticCode::ContentNotTrusted,
                            ErrorCode::AuthenticationFailed, Field::Scope);
    }

    const uint64 manifestBytes = LoadLe64(header + ManifestLengthOffset);
    if (manifestBytes != envelope.Size() - ContentEnvelopeManifestOffset) {
        return EnvelopeFail(failure, AssetDiagnosticCode::CorruptPackage, ErrorCode::CorruptData,
                            Field::ManifestLength);
    }

    const Span<const uint8> manifest(envelope.Data() + ContentEnvelopeManifestOffset,
                                     static_cast<usize>(manifestBytes));
    auto verifier = crypto::Ed25519phVerifier::TryCreate(allocator);
    if (!verifier) {
        return FailAsset<Span<const uint8>>(failure, AssetDiagnosticCode::None, verifier.Error());
    }

    const Span<const uint8> pieces[] = {Span<const uint8>(SignatureLabel, SignatureLabelBytes),
                                        Span<const uint8>(header, ContentEnvelopeHeaderBytes),
                                        manifest};
    // 조각 넣기는 새로 만든 상태라 실패하지 않는다(실패는 끝난·실패한 상태뿐) — 그래도 오류를
    // 잇는다.
    ErrorCode error = ErrorCode::None;
    if (auto updated = verifier.Value().TryUpdate(Span<const Span<const uint8>>(pieces));
        !updated) {
        error = updated.Error();
    } else if (auto verified = verifier.Value().TryFinish(
                   trusted->key, Span<const uint8>(header + ContentEnvelopeHeaderBytes,
                                                   ContentEnvelopeSignatureBytes));
               !verified) {
        error = verified.Error();
    }

    if (error != ErrorCode::None) {
        if (error != ErrorCode::AuthenticationFailed) {
            return FailAsset<Span<const uint8>>(failure, AssetDiagnosticCode::None, error);
        }

        return EnvelopeFail(failure, AssetDiagnosticCode::ContentNotTrusted,
                            ErrorCode::AuthenticationFailed, Field::Signature);
    }

    if (info != nullptr) {
        info->keyId = keyId;
        info->contentVersion = contentVersion;
        info->scope = scope;
    }

    return Result<Span<const uint8>>::Success(manifest);
}

void FormatTrustedContentKey(const TrustedContentKey& key,
                             char (&out)[TrustedContentKeyTextChars + 1]) noexcept {
    uint8 id[KeyIdBytes];
    StoreKeyId(id, key.id);
    // 크기가 정확히 맞으므로 hex 쓰기는 실패하지 않는다.
    (void)crypto::TryToHex(Span<const uint8>(id), Span<char>(out, ContentKeyIdHexChars + 1));
    out[ContentKeyIdHexChars] = ' ';
    (void)crypto::TryToHex(
        key.key.View(), Span<char>(out + ContentKeyIdHexChars + 1, ContentPublicKeyHexChars + 1));
}

Result<TrustedContentKey> ParseTrustedContentKey(StringView text) noexcept {
    usize length = text.ByteCount();
    if (length != 0 && text.Data()[length - 1] == '\n') {
        --length;
        if (length != 0 && text.Data()[length - 1] == '\r') {
            --length;
        }
    }

    if (length != TrustedContentKeyTextChars || text.Data()[ContentKeyIdHexChars] != ' ') {
        return Result<TrustedContentKey>::Failure(ErrorCode::InvalidArgument);
    }

    // 공개키 byte는 바로 키 칸에 읽는다(길이가 늘 32라 Ed25519PublicKey::TryFromBytes의 길이 검사가
    // 할 일이 없다 — 점의 올바름은 검증 때 본다).
    TrustedContentKey key;
    uint8 id[KeyIdBytes];
    if (!crypto::TryFromHex(StringView(text.Data(), ContentKeyIdHexChars), Span<uint8>(id)) ||
        !crypto::TryFromHex(
            StringView(text.Data() + ContentKeyIdHexChars + 1, ContentPublicKeyHexChars),
            Span<uint8>(key.key.bytes))) {
        return Result<TrustedContentKey>::Failure(ErrorCode::InvalidArgument);
    }

    key.id = LoadKeyId(id);
    if (key.id.IsNil()) {
        return Result<TrustedContentKey>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<TrustedContentKey>::Success(key);
}

} // namespace rocks
