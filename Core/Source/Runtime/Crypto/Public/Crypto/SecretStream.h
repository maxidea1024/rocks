#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Move.h>
#include <Core/Result.h>
#include <Crypto/Kdf.h>
#include <Crypto/Secret.h>

// 메시지 스트림(libsodium
// crypto_secretstream_xchacha20poly1305)이다(Docs/Design/Crypto.ko.md 9.1·9.4절). 메시지 경계가
// 있고 순서대로 소비하는 흐름(로그·파일 전송·채널 위의 레코드)에 쓴다. wire 형식은 libsodium과
// 같다: header 24 byte, 메시지마다 암호화된 tag 1 byte ‖ 암호문 ‖ MAC 16 byte.
//
// - 순서·절단·재전송·다른 스트림 메시지를 검출한다. 메시지마다 tag(Message·Push·Rekey·Final)를 함께
//   암호화한다. 스트림의 끝은 Final tag로만 확정된다(Reader::TryFinish가 강제한다).
// - 메시지 하나는 SecretStreamMaxMessageBytes(약 256 GiB)까지다. 스트림 길이와 메시지 수에는 제한이
//   없다(내부 카운터가 넘치기 전에 자동으로 rekey한다).
// - 키는 SecretStreamKey이고 스트림마다 header의 무작위 nonce로 새 하위 키를 만든다. 같은 키로 여러
//   스트림을 만들어도 된다.
// - 실패 규칙(2·11절): Writer·Reader 모두 move-only이고 옮겨 간 객체는 StaleHandle이다. Reader의
// 인증
//   실패(변조·절단·순서 위반·잘못된 키·AD 불일치·0–3 밖의 tag·Final 뒤 입력)는
//   AuthenticationFailed이고 output 전체를 지우며 Reset 전까지 같은 오류다. 공격자 입력에서는
//   InvalidArgument·Fatal이 나오지 않는다(libsodium misuse 조건은 부르기 전에 거른다).
// - 압축한 레코드를 보낼 때 공격자가 영향을 주는 데이터와 비밀을 한 압축 문맥에 넣지 않는다. 메시지
//   길이가 비밀을 흘린다(CRIME·BREACH와 같은 원리, 9.6절).
// - 한 객체는 한 번에 한 스레드만 쓴다. 상태는 키 저장소 슬롯에 있고 Reset·소멸 때 지운다.
//
// 사용 예:
//   auto key = crypto::SecretStreamKey::TryGenerate();
//   uint8 header[crypto::SecretStreamWriter::HeaderBytes];
//   auto writer = crypto::SecretStreamWriter::TryCreate(key.Value(), Span<uint8>(header));
//   auto sealed = writer.Value().TryPush(record, {}, crypto::SecretStreamTag::Message, out);
//   auto last = writer.Value().TryPush(tail, {}, crypto::SecretStreamTag::Final, out2);
//
//   auto reader = crypto::SecretStreamReader::TryCreate(key.Value(), header);
//   auto message = reader.Value().TryPull(sealedRecord, {}, plain);   // message.Value().bytes
//   if (auto done = reader.Value().TryFinish(); !done) { /* 절단 */ }
namespace rocks::crypto {

namespace detail {
struct KeyAccess;
struct SecretStreamTestAccess;
} // namespace detail

// tag는 메시지와 함께 암호화된다. 값은 libsodium TAG_* 상수와 같다.
// - Message: 일반 메시지다.
// - Push: 메시지 묶음의 끝 표시다(의미는 사용처가 정한다).
// - Rekey: 이 메시지 뒤에 양쪽이 키를 바꾼다(이전 키를 잊는다, 전방 비밀성).
// - Final: 스트림의 끝이다. 암묵적으로 rekey한다.
enum class SecretStreamTag : uint8 {
    Message = 0,
    Push = 1,
    Rekey = 2,
    Final = 3,
};

inline constexpr usize SecretStreamKeyBytes = 32;

// 메시지 하나의 평문 상한이다(libsodium MESSAGEBYTES_MAX = 64 × (2^32 − 2) byte, 약 256 GiB).
inline constexpr uint64 SecretStreamMaxMessageBytes = 64 * ((uint64{1} << 32) - 2);

// secretstream 키(32 byte)다. move-only이며 생성 뒤 바뀌지 않으므로 여러 스레드가 동시에 Writer·
// Reader를 만들어도 된다. 키 저장소에 있고 소멸 때 지운다. 평문 내보내기는 hazmat::TryExportKey다.
class SecretStreamKey {
public:
    SecretStreamKey() noexcept = default;

    // OS 난수 키다. OS 난수 실패는 DependencyFailed, 키 저장소가 찼으면 OutOfMemory다.
    [[nodiscard]] static Result<SecretStreamKey> TryGenerate() noexcept;

    // keyBytes는 정확히 32 byte다(아니면 InvalidArgument). keyBytes는 호출자가 지운다.
    [[nodiscard]] static Result<SecretStreamKey> TryImport(Span<const uint8> keyBytes) noexcept;

    [[nodiscard]] bool IsEmpty() const noexcept {
        return key_.IsEmpty();
    }

private:
    friend struct detail::KeyAccess;

    explicit SecretStreamKey(SecretBytes&& key) noexcept : key_(Move(key)) {
    }

    SecretBytes key_;
};

// master에서 secretstream 키를 뽑는다(Kdf.h 타입 파생, 키 타입을 섞는다). 빈 master는 StaleHandle,
// 키 저장소가 찼으면 OutOfMemory다.
[[nodiscard]] Result<SecretStreamKey>
TryDeriveSecretStreamKey(const KdfKey& master, uint64 subkeyId, KdfContext context) noexcept;

// 보내는 쪽이다. header를 먼저 보내고 메시지마다 TryPush의 출력을 보낸다.
class ROCKS_CRYPTO_API SecretStreamWriter {
public:
    static constexpr usize HeaderBytes = 24;
    static constexpr usize OverheadBytes = 17; // 암호화된 tag 1 + MAC 16

    SecretStreamWriter() noexcept = default;
    ~SecretStreamWriter() noexcept;

    SecretStreamWriter(const SecretStreamWriter&) = delete;
    SecretStreamWriter& operator=(const SecretStreamWriter&) = delete;
    SecretStreamWriter(SecretStreamWriter&& other) noexcept;
    SecretStreamWriter& operator=(SecretStreamWriter&& other) noexcept;

    // header(HeaderBytes)를 OS 난수 nonce로 채우고 상태를 만든다. 받는 쪽은 첫 메시지보다 먼저
    // header를 받아야 한다. 빈 키는 StaleHandle, header 길이가 틀리면 InvalidArgument, 키 저장소가
    // 찼으면 OutOfMemory다. 실패하면 header를 쓰지 않는다.
    [[nodiscard]] static Result<SecretStreamWriter> TryCreate(const SecretStreamKey& key,
                                                              Span<uint8> header) noexcept;

    // message를 봉인해 output 앞 message.Size() + OverheadBytes byte에 쓰고 그 수를 돌려준다.
    // associatedData는 이 메시지에만 묶인다(받는 쪽이 같은 AD로 연다).
    // - 옮겨 간 객체·Reset 뒤는 StaleHandle이다.
    // - Final을 보낸 뒤의 호출, 열거값이 아닌 tag, output이 message·AD와 겹치면 InvalidArgument다
    //   (libsodium은 Final 뒤의 push를 막지 않고 그 메시지도 검증을 통과하므로 여기서 막는다).
    // - message가 SecretStreamMaxMessageBytes를 넘거나 output이 작으면 CapacityExceeded다.
    // 모든 오류는 쓰기 전에 돌아오고 상태를 바꾸지 않는다.
    [[nodiscard]] Result<usize> TryPush(Span<const uint8> message, Span<const uint8> associatedData,
                                        SecretStreamTag tag, Span<uint8> output) noexcept;

    // 메시지 없이 키를 바꾼다(전방 비밀성). 흔적이 남지 않으므로 받는 쪽도 같은 위치(같은 메시지
    // 사이)에서 Rekey를 불러야 한다. 빈 객체에서는 아무 일도 하지 않는다.
    void Rekey() noexcept;

    // 상태를 지우고 빈 객체가 된다.
    void Reset() noexcept;

    [[nodiscard]] bool IsEmpty() const noexcept {
        return state_ == nullptr;
    }

    // Final을 보냈는가.
    [[nodiscard]] bool IsFinished() const noexcept {
        return finished_;
    }

private:
    friend struct detail::SecretStreamTestAccess;

    void* state_ = nullptr;
    bool finished_ = false;
};

// TryPull 결과다. bytes는 output에 쓴 평문 byte 수다.
struct SecretStreamMessage {
    usize bytes = 0;
    SecretStreamTag tag = SecretStreamTag::Message;
};

// 받는 쪽이다. Writer가 보낸 순서대로 TryPull을 부르고, 끝에 TryFinish로 스트림 완결을 확인한다.
class SecretStreamReader {
public:
    SecretStreamReader() noexcept = default;
    ~SecretStreamReader() noexcept;

    SecretStreamReader(const SecretStreamReader&) = delete;
    SecretStreamReader& operator=(const SecretStreamReader&) = delete;
    SecretStreamReader(SecretStreamReader&& other) noexcept;
    SecretStreamReader& operator=(SecretStreamReader&& other) noexcept;

    // header는 Writer가 만든 HeaderBytes byte다. 빈 키는 StaleHandle, header 길이가
    // 틀리면(메시지에서 온 byte) AuthenticationFailed, 키 저장소가 찼으면 OutOfMemory다. 잘못된
    // 키·변조된 header는 첫 TryPull에서 AuthenticationFailed로 드러난다.
    [[nodiscard]] static Result<SecretStreamReader> TryCreate(const SecretStreamKey& key,
                                                              Span<const uint8> header) noexcept;

    // sealed(TryPush 출력 하나)를 검증·복호해 output 앞에 평문을 쓴다.
    // - 옮겨 간 객체는 StaleHandle, output이 sealed·AD와 겹치면 InvalidArgument다(상태 불변).
    // - sealed가 OverheadBytes보다 짧거나 SecretStreamMaxMessageBytes + OverheadBytes보다 길면,
    //   MAC이 맞지 않으면, 0–3 밖의 tag면, Final을 받은 뒤의 호출이면 AuthenticationFailed다.
    //   output 전체를 지우고 Reset 전까지 모든 TryPull·TryFinish가 같은 오류다.
    // - output이 sealed.Size() - OverheadBytes보다 작으면 인증 전에 CapacityExceeded다(상태 불변).
    // 검증한 뒤에만 output에 평문을 쓴다.
    [[nodiscard]] Result<SecretStreamMessage> TryPull(Span<const uint8> sealed,
                                                      Span<const uint8> associatedData,
                                                      Span<uint8> output) noexcept;

    // 보내는 쪽 Rekey()와 같은 위치에서 부른다. 빈 객체·실패한 객체에서는 아무 일도 하지 않는다.
    void Rekey() noexcept;

    // Final을 받았는가.
    [[nodiscard]] bool IsFinished() const noexcept {
        return finished_;
    }

    // 스트림 완결을 확인한다. Final을 받았으면 성공이고, 받지 못했으면(절단) AuthenticationFailed다
    // (sticky). 옮겨 간 객체는 StaleHandle이다. 스트림을 완료로 취급하기 전에 반드시 부른다.
    [[nodiscard]] Result<void> TryFinish() noexcept;

    // 상태를 지우고 빈 객체가 된다(sticky 실패도 지운다).
    void Reset() noexcept;

    [[nodiscard]] bool IsEmpty() const noexcept {
        return state_ == nullptr;
    }

    // 인증에 실패했는가. 실패한 뒤의 TryPull·TryFinish는 늘 AuthenticationFailed다.
    [[nodiscard]] bool HasFailed() const noexcept {
        return failed_;
    }

private:
    friend struct detail::SecretStreamTestAccess;

    [[nodiscard]] Result<SecretStreamMessage> Fail(Span<uint8> output) noexcept;

    void* state_ = nullptr;
    bool finished_ = false;
    bool failed_ = false;
};

namespace hazmat {

// 키 byte를 평문으로 output에 복사한다(10.4절). output.Size()는 SecretStreamKeyBytes여야 한다.
[[nodiscard]] Result<void> TryExportKey(const SecretStreamKey& key, Span<uint8> output) noexcept;

} // namespace hazmat

} // namespace rocks::crypto
