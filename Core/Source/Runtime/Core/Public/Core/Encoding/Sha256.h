#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>

// SHA-256(FIPS 180-4)이다. 콘텐츠 동일성 판정(에셋 원본 해시·너겟 키·쿠킹 결과 대조)처럼
// 파일·네트워크에 기록해도 되는 안정 해시가 필요할 때 쓴다. 실행 중 해시 테이블 분포용
// Hash<T>(Core/Hash.h)나 손상 검출용 CRC32와 목적이 다르다. 비밀 값을 다루는 상수 시간 구현이
// 아니므로 MAC·서명 검증 같은 보안 경계에는 쓰지 않는다.
//
// 사용 예:
//   Sha256 hasher;
//   hasher.Update(header);
//   hasher.Update(payload);
//   const Sha256Digest digest = hasher.Finish();      // 이후 hasher는 처음 상태로 돌아간다
//   char hex[Sha256Digest::HexBytes + 1];
//   digest.ToHex(hex);                                 // "e3b0c442..."
//   const Sha256Digest same = Sha256::Hash(bytes);     // 한 번에 계산
namespace rocks {

struct Sha256Digest {
    static constexpr usize Bytes = 32;
    static constexpr usize HexBytes = 64;

    uint8 bytes[Bytes] = {};

    friend constexpr bool operator==(const Sha256Digest&, const Sha256Digest&) noexcept = default;

    // 64자 소문자 16진수와 NUL을 out에 쓴다.
    void ToHex(char (&out)[HexBytes + 1]) const noexcept;

    // 64자 16진수(대소문자 무관)를 읽는다. 길이·문자가 잘못되면 InvalidArgument다.
    static Result<Sha256Digest> ParseHex(StringView text) noexcept;
};

// 증분 SHA-256 계산기다. 입력을 여러 번에 나눠 Update해도 한 번에 넣은 결과와 같다. 한 객체를 여러
// 스레드가 동시에 쓰지 않는다.
class Sha256 {
public:
    Sha256() noexcept {
        Reset();
    }

    void Reset() noexcept;

    // 입력을 더한다. 전체 입력 길이는 2^61 - 1 byte까지다(넘으면 Fatal).
    void Update(Span<const uint8> data) noexcept;

    // 패딩을 붙여 다이제스트를 돌려주고 처음 상태로 돌아간다.
    Sha256Digest Finish() noexcept;

    static Sha256Digest Hash(Span<const uint8> data) noexcept {
        Sha256 hasher;
        hasher.Update(data);
        return hasher.Finish();
    }

private:
    void Compress(const uint8* block) noexcept;

    uint32 state_[8] = {};
    uint64 length_ = 0;
    uint8 block_[64] = {};
    usize used_ = 0;
};

} // namespace rocks
