#pragma once

#include <Compression/Compression.h>
#include <Core/Memory/Allocator.h>
#include <Platform/Sync.h>

// 압축 시험 공용 도구: 결정적 데이터 생성, 비교, 할당 계수 allocator.
namespace rocks::test::compression {

inline constexpr CompressionFormat AllFormats[] = {
    CompressionFormat::Lz4, CompressionFormat::Zstd, CompressionFormat::Deflate,
    CompressionFormat::Zlib, CompressionFormat::Gzip};

enum class Pattern : uint8 {
    Zeros,
    Random,
    Text
};

// 고정 seed xorshift로 만든 시험 데이터다. 플랫폼과 무관하게 같은 byte다.
inline void Fill(Array<uint8>& data, usize size, Pattern pattern,
                 uint64 seed = 0x9e3779b97f4a7c15ull) {
    static constexpr char Words[] =
        "rocks engine compression stream frame block texture mesh audio level ";
    auto resized = data.TryResize(size);
    if (!resized) {
        return;
    }
    uint64 state = seed;
    for (usize index = 0; index < size; ++index) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        switch (pattern) {
        case Pattern::Zeros:
            data[index] = 0;
            break;
        case Pattern::Random:
            data[index] = static_cast<uint8>(state >> 32);
            break;
        case Pattern::Text:
            data[index] = static_cast<uint8>(Words[(index + (state >> 60)) % (sizeof(Words) - 1)]);
            break;
        }
    }
}

inline bool Same(Span<const uint8> first, Span<const uint8> second) {
    if (first.Size() != second.Size()) {
        return false;
    }
    for (usize index = 0; index < first.Size(); ++index) {
        if (first[index] != second[index]) {
            return false;
        }
    }
    return true;
}

inline bool Same(const Array<uint8>& first, Span<const uint8> second) {
    return Same(Span<const uint8>(first.Data(), first.Size()), second);
}

inline Span<const uint8> View(const Array<uint8>& data) {
    return Span<const uint8>(data.Data(), data.Size());
}

inline CompressionSettings Settings(CompressionFormat format, int32 level) {
    return CompressionSettings::TryMake(format, level).Value();
}

inline CompressionSettings DefaultSettings(CompressionFormat format) {
    return CompressionSettings::TryDefault(format).Value();
}

// 할당 수·현재 byte·최고 byte를 세는 스레드 안전 allocator다. 실제 할당은 inner에 맡긴다.
class CountingAllocator final : public Allocator {
public:
    explicit CountingAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        auto allocation =
            inner_.Allocate(bytes + Header, alignment > Header ? alignment : Header, tag);
        if (!allocation) {
            return allocation;
        }
        auto* base = static_cast<uint8*>(allocation.Value());
        *reinterpret_cast<usize*>(base) = bytes;
        platform::LockGuard lock(gate_);
        ++allocations_;
        current_ += bytes;
        peak_ = current_ > peak_ ? current_ : peak_;
        return Result<void*>::Success(base + Header);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (address == nullptr) {
            return Result<void>::Success();
        }
        auto* base = static_cast<uint8*>(address) - Header;
        const usize bytes = *reinterpret_cast<usize*>(base);
        {
            platform::LockGuard lock(gate_);
            current_ -= bytes;
        }
        return inner_.Deallocate(base);
    }

    uint64 Allocations() const noexcept {
        platform::LockGuard lock(gate_);
        return allocations_;
    }

    usize Current() const noexcept {
        platform::LockGuard lock(gate_);
        return current_;
    }

    usize Peak() const noexcept {
        platform::LockGuard lock(gate_);
        return peak_;
    }

    void ResetPeak() noexcept {
        platform::LockGuard lock(gate_);
        peak_ = current_;
    }

private:
    // 정렬 16을 유지하면서 크기를 기록하는 머리다. 코덱 요청 정렬(16)과 같다.
    static constexpr usize Header = 16;

    Allocator& inner_;
    mutable platform::Mutex gate_;
    uint64 allocations_ = 0;
    usize current_ = 0;
    usize peak_ = 0;
};

} // namespace rocks::test::compression
