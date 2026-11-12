#include <Core/Encoding/Sha256.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks {
namespace {

constexpr uint32 RoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr uint32 RotateRight(uint32 value, uint32 count) noexcept {
    return (value >> count) | (value << (32u - count));
}

int HexValue(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

} // namespace

void Sha256Digest::ToHex(char (&out)[HexBytes + 1]) const noexcept {
    constexpr char Digits[] = "0123456789abcdef";
    for (usize index = 0; index < Bytes; ++index) {
        out[index * 2u] = Digits[bytes[index] >> 4];
        out[index * 2u + 1u] = Digits[bytes[index] & 0x0Fu];
    }
    out[HexBytes] = '\0';
}

Result<Sha256Digest> Sha256Digest::ParseHex(StringView text) noexcept {
    if (text.ByteCount() != HexBytes) {
        return Result<Sha256Digest>::Failure(ErrorCode::InvalidArgument);
    }
    Sha256Digest digest;
    for (usize index = 0; index < Bytes; ++index) {
        const int high = HexValue(text.Data()[index * 2u]);
        const int low = HexValue(text.Data()[index * 2u + 1u]);
        if (high < 0 || low < 0) {
            return Result<Sha256Digest>::Failure(ErrorCode::InvalidArgument);
        }
        digest.bytes[index] = static_cast<uint8>((high << 4) | low);
    }
    return Result<Sha256Digest>::Success(digest);
}

void Sha256::Reset() noexcept {
    state_[0] = 0x6a09e667u;
    state_[1] = 0xbb67ae85u;
    state_[2] = 0x3c6ef372u;
    state_[3] = 0xa54ff53au;
    state_[4] = 0x510e527fu;
    state_[5] = 0x9b05688cu;
    state_[6] = 0x1f83d9abu;
    state_[7] = 0x5be0cd19u;
    length_ = 0;
    used_ = 0;
}

void Sha256::Compress(const uint8* block) noexcept {
    uint32 schedule[64];
    for (uint32 index = 0; index < 16; ++index) {
        schedule[index] = (static_cast<uint32>(block[index * 4u]) << 24) |
                          (static_cast<uint32>(block[index * 4u + 1u]) << 16) |
                          (static_cast<uint32>(block[index * 4u + 2u]) << 8) |
                          static_cast<uint32>(block[index * 4u + 3u]);
    }
    for (uint32 index = 16; index < 64; ++index) {
        const uint32 s0 = RotateRight(schedule[index - 15], 7) ^
                          RotateRight(schedule[index - 15], 18) ^ (schedule[index - 15] >> 3);
        const uint32 s1 = RotateRight(schedule[index - 2], 17) ^
                          RotateRight(schedule[index - 2], 19) ^ (schedule[index - 2] >> 10);
        schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
    }
    uint32 a = state_[0];
    uint32 b = state_[1];
    uint32 c = state_[2];
    uint32 d = state_[3];
    uint32 e = state_[4];
    uint32 f = state_[5];
    uint32 g = state_[6];
    uint32 h = state_[7];
    for (uint32 index = 0; index < 64; ++index) {
        const uint32 sum1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
        const uint32 choose = (e & f) ^ (~e & g);
        const uint32 first = h + sum1 + choose + RoundConstants[index] + schedule[index];
        const uint32 sum0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
        const uint32 majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32 second = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::Update(Span<const uint8> data) noexcept {
    const usize size = data.Size();
    if (size > (0x1FFFFFFFFFFFFFFFull - length_)) {
        platform::Fatal("SHA-256 input longer than 2^61 - 1 bytes");
    }
    length_ += size;
    const uint8* bytes = data.Data();
    usize offset = 0;
    if (used_ != 0) {
        const usize take = size < 64u - used_ ? size : 64u - used_;
        platform::MemoryCopy(block_ + used_, bytes, take);
        used_ += take;
        offset = take;
        if (used_ < 64u) {
            return;
        }
        Compress(block_);
        used_ = 0;
    }
    for (; offset + 64u <= size; offset += 64u) {
        Compress(bytes + offset);
    }
    if (offset < size) {
        platform::MemoryCopy(block_, bytes + offset, size - offset);
        used_ = size - offset;
    }
}

Sha256Digest Sha256::Finish() noexcept {
    const uint64 bitLength = length_ * 8u;
    block_[used_++] = 0x80u;
    if (used_ > 56u) {
        platform::MemorySet(block_ + used_, 0, 64u - used_);
        Compress(block_);
        used_ = 0;
    }
    platform::MemorySet(block_ + used_, 0, 56u - used_);
    for (uint32 index = 0; index < 8; ++index) {
        block_[56u + index] = static_cast<uint8>(bitLength >> (56u - index * 8u));
    }
    Compress(block_);
    Sha256Digest digest;
    for (uint32 index = 0; index < 8; ++index) {
        digest.bytes[index * 4u] = static_cast<uint8>(state_[index] >> 24);
        digest.bytes[index * 4u + 1u] = static_cast<uint8>(state_[index] >> 16);
        digest.bytes[index * 4u + 2u] = static_cast<uint8>(state_[index] >> 8);
        digest.bytes[index * 4u + 3u] = static_cast<uint8>(state_[index]);
    }
    Reset();
    return digest;
}

} // namespace rocks
