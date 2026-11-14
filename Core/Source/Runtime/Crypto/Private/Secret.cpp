#include <Crypto/Random.h>
#include <Crypto/Secret.h>

#include "SecretArena.h"

#include <Core/Move.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks::crypto {
namespace {

// 키 확장 상태·SIMD 접근이 정렬을 기대할 수 있어 슬롯 최소 정렬과 같게 둔다.
constexpr usize SecretAlignment = 64;

// 크기가 저장소를 정한다. 할당과 해제가 같은 규칙을 쓰므로 따로 기록하지 않는다.
bool UsesSecretArena(usize bytes) noexcept {
    return bytes <= detail::SecretSlotMaxBytes;
}

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

    // 두 저장소 모두 해제가 내용을 지운다. 키 저장소는 남의 주소면 스스로 Fatal이다.
    if (UsesSecretArena(size_)) {
        detail::FreeSecretSlot(data_);
    } else if (!GetSecretAllocator().Deallocate(data_)) {
        platform::Fatal("Crypto: SecretBytes storage was not owned by the secret buffer allocator");
    }

    data_ = nullptr;
    size_ = 0;
}

Result<SecretBytes> SecretBytes::TryCreate(usize bytes) noexcept {
    if (bytes == 0) {
        return Result<SecretBytes>::Success(SecretBytes());
    }

    if (UsesSecretArena(bytes)) {
        // 키 저장소의 새 슬롯은 이미 0이다(새로 commit한 페이지이거나 해제 때 지운 슬롯).
        auto slot = detail::TryAllocateSecretSlot(bytes, SecretAlignment);
        if (!slot) {
            return Result<SecretBytes>::Failure(slot.Error());
        }

        return Result<SecretBytes>::Success(SecretBytes(static_cast<uint8*>(slot.Value()), bytes));
    }

    auto memory = GetSecretAllocator().Allocate(bytes, SecretAlignment);
    if (!memory) {
        return Result<SecretBytes>::Failure(memory.Error());
    }

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
