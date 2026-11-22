#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Diagnostics/LogFileCipher.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Crypto/Kem.h>
#include <Crypto/SecretStream.h>

// 로그 파일 봉인이다(Docs/Design/LoggingSystem.ko.md 9.1절 L12·L13). FileLogSink의 LogFileCipher
// 구현이다. 보안 목표는 "사용자가 파일을 열어 읽을 수 없다"이며 그 이상(키 회전·보낸 쪽 서명·안전
// 삭제·크기와 시각 은닉)은 하지 않는다. 자체 암호 구현은 없고 Crypto API만 쓴다.
//
// 파일 형식(버전 1, 정수는 little-endian):
//
//   머리(LogFileHeaderBytes = 1156 byte)
//     magic        8   "RKLOGENC"
//     version      1   1
//     kem          1   KemAlgorithm(X-Wing = 1)
//     flags        1   LogFileFlag* 비트(0 또는 LogFileFlagCompressedBatches)
//     reserved     1   0
//     kemCiphertext 1120  개발자 공개키로 캡슐화한 암호문(X-Wing)
//     streamHeader 24   SecretStreamWriter header
//   기록(flush 묶음마다 하나, 끝까지 반복)
//     length       4   sealed byte 수
//     sealed       n   SecretStream 메시지(평문 묶음 + 17), tag Message, 정상 종료의 마지막은 빈
//                      평문 + tag Final
//
// 파일 키: 파일마다 X-Wing으로 공유 비밀을 만들고 Kdf 타입 파생(TryDeriveSecretStreamKey, context
// labels::LogFile, subkey 1)으로 SecretStreamKey를 뽑는다. 개발자 개인키(seed)만 열 수 있다.
//
// 사용 예(엔진 호스트, 공개키만 가진다):
//   crypto::LogFileSealer sealer(recipientPublicKeyBytes);
//   FileLogSinkConfig config;
//   config.cipher = &sealer;     // 키가 잘못되면 Open이 실패하고 sink가 꺼진다(평문 없음)
//
// 사용 예(개발자 도구, 개인키로 연다):
//   auto key = crypto::KemKeyPair::TryFromSeed(crypto::KemAlgorithm::XWing, seed);
//   auto summary = crypto::TryDecodeLogFile(key.Value(), fileBytes, allocator, &Print, &context);
//   if (summary && !summary.Value().endMarker) { /* 비정상 종료: 끝 표시 없음 */ }
namespace rocks::crypto {

inline constexpr uint8 LogFileMagic[8] = {'R', 'K', 'L', 'O', 'G', 'E', 'N', 'C'};
inline constexpr uint8 LogFileFormatVersion = 1;
// 머리 flags: 묶음 평문이 독립 zstd frame이다(압축 뒤 봉인, 로깅 L23). 읽는 쪽이 묶음마다 푼다.
inline constexpr uint8 LogFileFlagCompressedBatches = 1;
// X-Wing 암호문 길이다(KemInfo::ciphertextBytes).
inline constexpr usize LogFileKemCiphertextBytes = 1120;
inline constexpr usize LogFileHeaderBytes =
    sizeof(LogFileMagic) + 1 + 1 + 2 + LogFileKemCiphertextBytes + SecretStreamWriter::HeaderBytes;
// 기록 하나의 길이 머리 byte 수다.
inline constexpr usize LogFileRecordLengthBytes = 4;
// 개발자 키 파일 크기다: 공개키는 X-Wing 공개키 그대로, 개인키는 X-Wing seed 그대로다.
inline constexpr usize LogFilePublicKeyBytes = 1216;
inline constexpr usize LogFilePrivateKeyBytes = 32;

// 개발자 공개키로 로그 파일을 봉인한다. 한 FileLogSink가 쓴다(호출은 sink가 직렬화한다).
class ROCKS_CRYPTO_API LogFileSealer final : public LogFileCipher {
public:
    // recipientPublicKey는 X-Wing 공개키(LogFilePublicKeyBytes)다. 복사한다. 길이가 틀리면 생성은
    // 되지만 TryWriteHeader가 InvalidArgument로 실패한다(호스트가 실수로 평문 sink를 쓰지 않게,
    // 키 문제는 sink를 끄는 쪽으로 드러난다).
    // flags는 머리에 남길 LogFileFlag* 비트다(압축한 묶음을 봉인하면 LogFileFlagCompressedBatches).
    explicit LogFileSealer(Span<const uint8> recipientPublicKey, uint8 flags = 0) noexcept;

    usize HeaderBytes() const noexcept override {
        return LogFileHeaderBytes;
    }

    usize SealOverhead() const noexcept override {
        return LogFileRecordLengthBytes + SecretStreamWriter::OverheadBytes;
    }

    // 캡슐화·키 파생·스트림 생성 중 하나라도 실패하면 그 오류이고 out에 아무것도 남기지 않는다.
    Result<usize> TryWriteHeader(Span<uint8> out) noexcept override;

    // 길이 머리 + SecretStream 메시지를 쓴다. 머리를 쓰기 전이거나 끝 표시 뒤면 InvalidArgument다.
    Result<usize> TrySeal(Span<const uint8> batch, bool last, Span<uint8> out) noexcept override;

private:
    KemPublicKey recipient_;
    uint8 flags_ = 0;
    ErrorCode setupError_ = ErrorCode::None;
    SecretStreamWriter writer_;
};

// TryDecodeLogFile이 묶음 평문마다 부른다. 실패를 돌려주면 복호를 멈추고 그 오류를 돌려준다.
using LogFileDecodeOutput = Result<void> (*)(void* context, Span<const uint8> plaintext) noexcept;

// 복호 결과다.
struct LogFileDecodeSummary {
    // 복호한 묶음(기록) 수다(끝 표시 기록 포함).
    uint64 batches = 0;
    uint64 plaintextBytes = 0;
    // 끝 표시(Final)를 받았는지다. 거짓이면 비정상 종료한 파일이다(마지막 완전한 묶음까지
    // 복호했다).
    bool endMarker = false;
    // 끝 표시 없이 끝난 파일의 꼬리에 완성되지 않은 기록이 있어 버렸는지다(쓰는 중 종료). 길이
    // 머리가 손상돼 파일 끝을 넘는 길이를 가리켜도 같은 모양이다(인증으로 구별할 수 없다).
    bool partialTail = false;
    // 머리 flags에 LogFileFlagCompressedBatches가 있었는지다(묶음 평문이 zstd frame이다).
    bool compressedBatches = false;
};

// 봉인한 로그 파일의 머리에 LogFileFlagCompressedBatches가 있는지다(복호 전에 묶음 평문을 풀지
// 정할 때). 머리가 짧거나 magic이 다르면 false다.
bool LogFileHasCompressedBatches(Span<const uint8> file) noexcept;

// file(봉인한 로그 파일 전체)을 recipient 개인키로 열어 묶음마다 output에 평문을 넘긴다. 평문
// 버퍼는 allocator로 잡는다.
// - 머리가 짧거나 magic이 다르면 InvalidArgument, 모르는 버전·KEM·0이 아닌 예약 칸은 Unsupported다.
// - 완전한 기록의 인증이 실패하면(변조·다른 키·순서 위반·끝 표시 뒤의 byte) AuthenticationFailed다.
//   그 전까지의 묶음은 이미 output에 넘어갔다.
// - 끝 표시 없이 끝나면 성공이고 endMarker가 거짓이다(호출자가 "비정상 종료"를 알린다).
[[nodiscard]] Result<LogFileDecodeSummary>
TryDecodeLogFile(const KemKeyPair& recipient, Span<const uint8> file, Allocator& allocator,
                 LogFileDecodeOutput output, void* context) noexcept;

} // namespace rocks::crypto
