#include <Crypto/Random.h>

#include <Crypto/Bytes.h>
#include <Platform/SecureRandom.h>

namespace rocks::crypto {

Result<void> TryFillRandom(Span<uint8> output) noexcept {
    if (output.IsEmpty()) {
        return Result<void>::Success();
    }

    // 실패하면 일부만 채워졌을 수 있다. 예측 가능한 byte가 키·nonce로 쓰이지 않게 지운다(2절 "쓰기
    // 시작한 뒤 실패하면 지운다").
    if (!platform::TryFillSecureRandom(output.Data(), output.Size())) {
        SecureZero(output);
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    return Result<void>::Success();
}

Result<uint32> TryRandomUniform(uint32 upperBound) noexcept {
    if (upperBound == 0) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    // 2^32 mod upperBound보다 작은 값을 버리면 남은 구간의 길이가 upperBound의 배수라 나머지가
    // 균등하다.
    const uint32 minimum = (0u - upperBound) % upperBound;
    for (;;) {
        uint32 value = 0;
        if (auto filled = TryFillRandom(AsWritableBytes(Span<uint32>(&value, 1))); !filled) {
            return Result<uint32>::Failure(filled.Error());
        }

        if (value >= minimum) {
            return Result<uint32>::Success(value % upperBound);
        }
    }
}

} // namespace rocks::crypto
