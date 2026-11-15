#include <Crypto/Channel.h>

#include <Base/Endian.h>
#include <Crypto/Bytes.h>

#include <Platform/DataIndependentTiming.h>
#include <Platform/Memory.h>

// 명시 counter datagram 봉인·열기다(Docs/Design/SecureChannel.ko.md 3.2절). AEAD는 hazmat nonce
// 지정 API(ChaCha20-Poly1305 IETF)이고, 여기서는 counter·재전송 창·인증 실패 수를 관리한다.
namespace rocks::crypto {
namespace {

constexpr usize NonceBytes = 12;
constexpr uint64 WordBits = 64;
static_assert(DatagramWindowSize % WordBits == 0);

// nonce = 0^32 ‖ LE64(counter)다(Noise ChaChaPoly nonce와 같은 구성이라 빈 header의 봉인은 Noise
// transport 메시지와 같은 byte다).
Span<const uint8> MakeNonce(uint64 counter, uint8 (&nonce)[NonceBytes]) noexcept {
    platform::MemoryZero(nonce, 4);
    StoreLittleEndian<uint64>(nonce + 4, counter);
    return Span<const uint8>(nonce);
}

// header + 봉인이 datagram 상한 이하인가(합이 넘치지 않게 뺄셈으로 본다).
bool FitsDatagram(usize headerBytes, usize sealedBytes) noexcept {
    return headerBytes <= DatagramMaxBytes && sealedBytes <= DatagramMaxBytes - headerBytes;
}

} // namespace

Result<uint64> DatagramSealer::TrySeal(Span<const uint8> header, Span<const uint8> payload,
                                       Span<uint8> sealed) noexcept {
    platform::DataIndependentTimingScope dit;
    if (key_.IsEmpty()) {
        return Result<uint64>::Failure(ErrorCode::StaleHandle);
    }

    if (IsExhausted() || payload.Size() > DatagramMaxBytes ||
        !FitsDatagram(header.Size(), payload.Size() + DatagramTagBytes)) {
        return Result<uint64>::Failure(ErrorCode::CapacityExceeded);
    }

    // 출력 길이·겹침은 hazmat이 쓰기 전에 본다(CapacityExceeded·InvalidArgument).
    uint8 nonce[NonceBytes];
    auto written = hazmat::TrySealWithNonce(key_, MakeNonce(next_, nonce), header, payload, sealed);
    if (!written) {
        return Result<uint64>::Failure(written.Error());
    }

    const uint64 counter = next_;
    ++next_;
    return Result<uint64>::Success(counter);
}

Result<usize> DatagramOpener::TryOpen(uint64 counter, Span<const uint8> header,
                                      Span<const uint8> sealed, Span<uint8> payload) noexcept {
    platform::DataIndependentTimingScope dit;
    if (key_.IsEmpty()) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    // 겹침을 먼저 본다. 거부할 때 지우기가 호출자의 입력을 덮지 않게 한다(제자리는 허용).
    const bool inPlace = payload.Data() == sealed.Data();
    if ((Overlaps(sealed, payload) && !inPlace) || Overlaps(header, payload)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    if (sealed.Size() >= DatagramTagBytes && payload.Size() < sealed.Size() - DatagramTagBytes) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    // 여기부터는 받은 패킷에서 온 값이다. 창 검사는 복호보다 먼저이고 상태를 바꾸지 않는다.
    bool fresh = counter >= top_;
    if (!fresh && top_ - counter <= static_cast<uint64>(DatagramWindowSize)) {
        fresh = (window_[(counter / WordBits) % WindowWords] &
                 (uint64{1} << (counter % WordBits))) == 0;
    }

    if (IsExhausted() || sealed.Size() < DatagramTagBytes ||
        !FitsDatagram(header.Size(), sealed.Size()) || counter >= DatagramCounterLimit || !fresh) {
        SecureZero(payload);
        return Result<usize>::Failure(ErrorCode::AuthenticationFailed);
    }

    uint8 nonce[NonceBytes];
    auto opened =
        hazmat::TryOpenWithNonce(key_, MakeNonce(counter, nonce), header, sealed, payload);
    if (!opened) {
        // AEAD 불일치만 센다(hazmat이 payload를 지웠다). 소진 전이므로 넘치지 않는다.
        if (opened.Error() == ErrorCode::AuthenticationFailed) {
            ++failures_;
        }

        return opened;
    }

    // 인증된 counter만 기록한다. 창을 앞으로 밀 때는 창 밖으로 나가는 counter(새로 덮을 자리)의
    // bit를 지운다.
    if (counter >= top_) {
        if (counter - top_ >= static_cast<uint64>(DatagramWindowSize)) {
            platform::MemoryZero(window_, sizeof(window_));
        } else {
            for (uint64 cleared = top_; cleared < counter; ++cleared) {
                window_[(cleared / WordBits) % WindowWords] &= ~(uint64{1} << (cleared % WordBits));
            }
        }

        top_ = counter + 1;
    }

    window_[(counter / WordBits) % WindowWords] |= uint64{1} << (counter % WordBits);
    return opened;
}

} // namespace rocks::crypto
