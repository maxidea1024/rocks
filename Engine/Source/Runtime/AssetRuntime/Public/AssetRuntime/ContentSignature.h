#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <AssetRuntime/CookedManifest.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Text/StringView.h>
#include <Crypto/KeyId.h>
#include <Crypto/Signature.h>

// 서명된 쿠킹 매니페스트 봉투 `CookedManifest.rsig` v1이다(Docs/Design/ContentSigning.ko.md 3·4·7절
// — A1f). 봉투 = 헤더(72 byte) ‖ Ed25519ph 서명(64 byte) ‖ 매니페스트 byte. 서명 메시지는
// "rocks.content-signature.v1\0" ‖ 헤더 ‖ 매니페스트 byte다. 검증은 같은 버퍼 안의 매니페스트
// byte를 돌려주므로 검증한 byte와 파싱하는 byte가 같다(CS1).
//
// 사용 예(로더 쪽):
//   ContentTrust trust;
//   trust.keys = trustedKeys;                   // 실행 파일 안의 신뢰 공개키(CS3)
//   trust.minimumContentVersion = 7;            // rollback 하한(CS4)
//   trust.scope = MakeContentScope("MyGame", "live").Value();
//   auto manifestBytes =
//       VerifyContentEnvelope(allocator, envelope.View(), trust, &info, &diagnostic);
//   if (!manifestBytes) { /* 시작하지 않는다(CS5) */ }
//   auto manifest = ReadCookedManifest(allocator, manifestBytes.Value(), &diagnostic);
namespace rocks {

// 봉투 배치다(3절).
inline constexpr usize ContentEnvelopeHeaderBytes = 72;
inline constexpr usize ContentEnvelopeSignatureBytes = crypto::Ed25519SignatureBytes;
inline constexpr usize ContentEnvelopeManifestOffset =
    ContentEnvelopeHeaderBytes + ContentEnvelopeSignatureBytes;
inline constexpr uint8 ContentEnvelopeFormatVersion = 1;
// 봉투 파일 크기 상한이다(매니페스트 상한 + 헤더·서명).
inline constexpr uint64 ContentEnvelopeMaxBytes =
    CookedManifestMaxFileBytes + ContentEnvelopeManifestOffset;
// 봉투 파일 이름이다(쿠킹 루트 바로 아래 — 11.4절).
inline constexpr StringView ContentEnvelopeFileName = "CookedManifest.rsig";

// 진단 칸(AssetDiagnostic::section)이다. 매니페스트 칸(CookedManifestField 100–)과 겹치지 않는다.
enum class ContentEnvelopeField : uint32 {
    File = 200,
    Magic,
    FormatVersion,
    Algorithm,
    Reserved,
    Key,
    ContentVersion,
    Scope,
    ManifestLength,
    Signature
};

// 칸 이름이다(예: Key → "Key" — 로그용, 정적 수명). 봉투 칸이 아닌 값은 "None"이다(진단 section 0 —
// 신뢰 정보 오류·할당 실패).
const char* ContentEnvelopeFieldName(ContentEnvelopeField field) noexcept;

// 신뢰하는 서명 공개키 하나다(CS3 — 콘텐츠 서명 전용 키).
struct TrustedContentKey {
    crypto::KeyId id{};
    crypto::Ed25519PublicKey key{};
};

// 실행 파일이 가진 신뢰 정보다(CS3·CS4). keys는 비지 않고 id가 서로 다르며 null id가 없어야 한다.
struct ContentTrust {
    Span<const TrustedContentKey> keys;
    // 이보다 낮은 contentVersion은 거부한다(rollback 하한).
    uint64 minimumContentVersion = 0;
    // MakeContentScope(productId, channel) — 봉투의 scope와 같아야 한다.
    Sha256Digest scope{};
};

// 검증한 봉투의 헤더 값이다(검증 성공 뒤에만 채운다).
struct ContentEnvelopeInfo {
    crypto::KeyId keyId{};
    uint64 contentVersion = 0;
    Sha256Digest scope{};
};

// scope = SHA-256(productId ‖ 0 ‖ channel)이다. productId·channel은 비지 않은 ASCII 식별자
// ([A-Za-z0-9_.-])다. 오류: 형식 InvalidArgument.
Result<Sha256Digest> MakeContentScope(StringView productId, StringView channel) noexcept;

// manifest byte를 봉투로 감싸 서명한다(5절 content-sign). 같은 입력이면 같은 byte다. out을 바꾼다
// (실패하면 그대로). 오류: null keyId·매니페스트가 CookedManifestMaxFileBytes 초과 InvalidArgument·
// CapacityExceeded, 빈 키 StaleHandle(Crypto), 할당 실패.
Result<void> WriteContentEnvelope(Allocator& allocator, Span<const uint8> manifest,
                                  const crypto::Ed25519SigningKey& key, crypto::KeyId keyId,
                                  uint64 contentVersion, const Sha256Digest& scope,
                                  Array<uint8>& out) noexcept;

// 봉투를 4절 순서로 검증하고 같은 버퍼 안의 매니페스트 byte를 돌려준다. 판정 순서와 진단(7절 2):
//   길이(헤더·서명 136 byte 미만, ContentEnvelopeMaxBytes 초과) CorruptPackage / InvalidArgument
//   (File) → magic CorruptPackage(Magic) → 형식 버전·알고리즘을 모르면 UnsupportedVersion /
//   Unsupported(FormatVersion·Algorithm) → 예약 byte CorruptPackage(Reserved) → 모르는 keyId,
//   contentVersion < 최소, scope 불일치는 ContentNotTrusted / AuthenticationFailed(Key·
//   ContentVersion·Scope) → 매니페스트 길이 ≠ 남은 byte CorruptPackage(ManifestLength) → 서명
//   불일치 ContentNotTrusted / AuthenticationFailed(Signature). 신뢰 정보가 잘못됐으면(키 없음·null
//   id·같은 id) 봉투를 보기 전에 InvalidArgument(진단 None)다. 검증 상태(SHA-512)는 allocator에서
//   할당하고 그 실패는 그 오류다(진단 None).
Result<Span<const uint8>> VerifyContentEnvelope(Allocator& allocator, Span<const uint8> envelope,
                                                const ContentTrust& trust,
                                                ContentEnvelopeInfo* info = nullptr,
                                                AssetDiagnostic* failure = nullptr) noexcept;

// 신뢰 키 한 줄 텍스트다(content-sign keygen의 공개 파일, 신뢰 목록에 넣는 값 — 7절 5): keyId
// 소문자 hex 32자(RFC 9562 byte 순서), 공백 하나, 공개키 소문자 hex 64자.
//
// 사용 예:
//   char line[TrustedContentKeyTextChars + 1];
//   FormatTrustedContentKey(key, line);            // "4041...4f 79b5...64"
//   auto parsed = ParseTrustedContentKey(line);    // 같은 key
inline constexpr usize ContentKeyIdHexChars = 32;
inline constexpr usize ContentPublicKeyHexChars = 64;
inline constexpr usize TrustedContentKeyTextChars =
    ContentKeyIdHexChars + 1 + ContentPublicKeyHexChars;

// key를 그 텍스트와 NUL로 쓴다.
void FormatTrustedContentKey(const TrustedContentKey& key,
                             char (&out)[TrustedContentKeyTextChars + 1]) noexcept;

// 그 텍스트 한 줄을 읽는다(hex 대소문자 무관, 끝의 "\n"·"\r\n" 하나는 받는다). 오류: 길이·구분자·
// hex가 아닌 문자, null keyId는 InvalidArgument다(공개키 점의 올바름은 검증 때 본다 — Crypto).
Result<TrustedContentKey> ParseTrustedContentKey(StringView text) noexcept;

} // namespace rocks
