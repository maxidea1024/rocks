#include <Crypto/Random.h>
#include <Crypto/Secret.h>

#include <Core/Move.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks::crypto {
namespace {

// 키 확장 상태·SIMD 접근이 정렬을 기대할 수 있어 arena 슬롯 최소 정렬과 같게 둔다.
constexpr usize SecretAlignment = 64;

} // namespace

SecretBytes::~SecretBytes() noexcept {
    Reset();
}

SecretBytes::SecretBytes(SecretBytes&& other) noexcept : data_(other.data_), size_(other.size_) {
    other.data_ = nullptr;
    other.size_ = 0;
}

SecretBytes& SecretBytes::operator=(SecretBytes&& other) noexcept {
    if (this != &other) {
        Reset();
        data_ = other.data_;
        size_ = other.size_;
        other.data_ = nullptr;
        other.size_ = 0;
    }

    return *this;
}

void SecretBytes::Reset() noexcept {
    if (data_ == nullptr) {
        return;
    }

    // 해제가 내용을 지운다(GetSecretAllocator 계약). 실패는 저장소가 이 할당기 것이 아니라는
    // 뜻이므로 결함이다.
    if (!GetSecretAllocator().Deallocate(data_)) {
        platform::Fatal("Crypto: SecretBytes storage was not owned by the secret allocator");
    }

    data_ = nullptr;
    size_ = 0;
}

Result<SecretBytes> SecretBytes::TryCreate(usize bytes) noexcept {
    if (bytes == 0) {
        return Result<SecretBytes>::Success(SecretBytes());
    }

    auto memory = GetSecretAllocator().Allocate(bytes, SecretAlignment);
    if (!memory) {
        return Result<SecretBytes>::Failure(memory.Error());
    }

    // arena 슬롯은 이미 0이지만 힙으로 간 할당은 아니다.
    uint8* const data = static_cast<uint8*>(memory.Value());
    platform::MemoryZero(data, bytes);
    return Result<SecretBytes>::Success(SecretBytes(data, bytes));
}

Result<SecretBytes> SecretBytes::TryGenerate(usize bytes) noexcept {
    auto secret = TryCreate(bytes);
    if (!secret) {
        return secret;
    }

    // 실패하면 secret이 소멸하며 저장소를 지운다.
    if (auto filled = TryFillRandom(secret.Value().MutableView()); !filled) {
        return Result<SecretBytes>::Failure(filled.Error());
    }

    return secret;
}

Result<SecretBytes> SecretBytes::TryCopyFrom(Span<const uint8> source) noexcept {
    auto secret = TryCreate(source.Size());
    if (!secret) {
        return secret;
    }

    platform::MemoryCopy(secret.Value().MutableView().Data(), source.Data(), source.Size());
    return secret;
}

} // namespace rocks::crypto
