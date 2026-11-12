#pragma once

#include <Base/Endian.h>
#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Error.h>
#include <Core/Result.h>
#include <Platform/Memory.h>

namespace rocks {

// 외부 binary 형식을 Array<uint8> 끝에 덧붙여 쓰는 도구다. 쓰기마다 byte 순서를 명시한다. UE의
// FMemoryWriter·FArrayWriter처럼 외부 형식(BMP·PNG·WAV·DDS 등)의 출력이나 테스트 입력을 만들 때
// 쓰며, Rocks 자체 형식(Record·Archive)은 예산·한도를 갖는 Serialization 모듈의 ArchiveWriter로
// 쓴다.
//
// 실패는 이어진다(sticky). 할당 실패(OutOfMemory)·크기 넘침(Overflow)·범위 밖
// Patch(InvalidArgument)가 나면 그 호출은 target을 바꾸지 않고, 그 뒤의 쓰기는 아무것도 하지
// 않는다. 그러므로 target은 언제나 실패 전까지 성공한 쓰기들의 결과 그대로이며, 마지막에 Status()로
// 한 번 확인하면 된다:
//   if (body.Size() > 0xFFFFFFFFu) {             // 형식의 길이 필드가 u32다
//       return Result<void>::Failure(ErrorCode::CapacityExceeded);
//   }
//   Array<uint8> bytes(allocator);
//   ByteWriter writer(bytes);
//   const usize lengthAt = writer.Size();
//   writer.WriteU32BE(0);                       // 길이 자리(뒤에서 채운다)
//   writer.WriteBytes(body);
//   writer.PatchBigEndian<uint32>(lengthAt, static_cast<uint32>(body.Size()));
//   if (auto status = writer.Status(); !status) {
//       return status;                           // 첫 실패의 오류
//   }
// 외부 형식에서는 폭이 곧 의미이므로 Write·Patch의 T는 인자에서 추론하지
// 않는다(`WriteBigEndian(5)`는 컴파일 오류, `WriteBigEndian<uint16>(5)`처럼 쓴다).
//
// 수명: target을 빌린다. target은 writer보다 오래 살아야 하며 writer를 쓰는 동안 다른 경로로 바꾸지
// 않는다. 기존 내용 뒤에 덧붙인다(비우지 않는다). WriteBytes에 target 자신의 범위를 넘겨도 된다.
// 같은 writer의 병행 사용은 caller가 동기화한다.
class ByteWriter {
    // 템플릿 인자 추론을 막는다(폭을 호출자가 명시하게 한다).
    template <typename T>
    struct Exactly {
        using Type = T;
    };

public:
    explicit ByteWriter(Array<uint8>& target) noexcept : target_(&target) {
    }

    ByteWriter(const ByteWriter&) = delete;
    ByteWriter& operator=(const ByteWriter&) = delete;

    bool Failed() const noexcept {
        return error_ != ErrorCode::None;
    }

    // 첫 실패의 오류다. 실패가 없으면 ErrorCode::None이다.
    ErrorCode Error() const noexcept {
        return error_;
    }

    Result<void> Status() const noexcept {
        return Failed() ? Result<void>::Failure(error_) : Result<void>::Success();
    }

    // target의 현재 크기(다음 쓰기가 시작할 offset)다.
    usize Size() const noexcept {
        return target_->Size();
    }

    void WriteU8(uint8 value) noexcept {
        Append(Span<const uint8>(&value, 1));
    }

    void WriteI8(int8 value) noexcept {
        WriteU8(static_cast<uint8>(value));
    }

    // sizeof(T) byte를 little-endian으로 쓴다. T는 uint16·uint32·uint64와 같은 폭의 부호 있는
    // 정수이며 명시해야 한다(인자에서 추론하지 않는다).
    template <typename T>
    void WriteLittleEndian(typename Exactly<T>::Type value) noexcept {
        uint8 bytes[sizeof(T)];
        StoreLittleEndian<T>(bytes, value);
        Append(Span<const uint8>(bytes, sizeof(T)));
    }

    // sizeof(T) byte를 big-endian으로 쓴다. 조건은 WriteLittleEndian과 같다.
    template <typename T>
    void WriteBigEndian(typename Exactly<T>::Type value) noexcept {
        uint8 bytes[sizeof(T)];
        StoreBigEndian<T>(bytes, value);
        Append(Span<const uint8>(bytes, sizeof(T)));
    }

    void WriteU16LE(uint16 value) noexcept {
        WriteLittleEndian<uint16>(value);
    }

    void WriteU16BE(uint16 value) noexcept {
        WriteBigEndian<uint16>(value);
    }

    void WriteU32LE(uint32 value) noexcept {
        WriteLittleEndian<uint32>(value);
    }

    void WriteU32BE(uint32 value) noexcept {
        WriteBigEndian<uint32>(value);
    }

    void WriteU64LE(uint64 value) noexcept {
        WriteLittleEndian<uint64>(value);
    }

    void WriteU64BE(uint64 value) noexcept {
        WriteBigEndian<uint64>(value);
    }

    // IEEE-754 bit 표현을 그대로 쓴다(NaN payload·부호 있는 0 보존).
    void WriteF32LE(float value) noexcept {
        WriteLittleEndian<uint32>(platform::BitCast<uint32>(value));
    }

    void WriteF32BE(float value) noexcept {
        WriteBigEndian<uint32>(platform::BitCast<uint32>(value));
    }

    void WriteF64LE(double value) noexcept {
        WriteLittleEndian<uint64>(platform::BitCast<uint64>(value));
    }

    void WriteF64BE(double value) noexcept {
        WriteBigEndian<uint64>(platform::BitCast<uint64>(value));
    }

    // bytes를 그대로 붙인다. target 자신의 전체·부분 범위여도 된다(성장 중에도 원본을 먼저
    // 복사한다).
    void WriteBytes(Span<const uint8> bytes) noexcept {
        Append(bytes);
    }

    // 0인 byte count개를 붙인다(정렬 padding·예약 필드). 크기가 표현 범위를 넘으면 Overflow다.
    void WriteZeros(usize count) noexcept {
        if (Failed() || count == 0) {
            return;
        }

        auto tail = target_->TryAppendUninitialized(count);
        if (!tail) {
            error_ = tail.Error();
            return;
        }

        platform::MemorySet(tail.Value().Data(), 0, count);
    }

    // 이미 쓴 [offset, offset + sizeof(T)) 범위를 little-endian 값으로 고쳐 쓴다(뒤에서 길이·offset
    // 채우기). 범위가 target 밖이면 InvalidArgument로 실패한다. T는 명시한다.
    template <typename T>
    void PatchLittleEndian(usize offset, typename Exactly<T>::Type value) noexcept {
        if (uint8* at = PatchTarget(offset, sizeof(T)); at != nullptr) {
            StoreLittleEndian<T>(at, value);
        }
    }

    // PatchLittleEndian의 big-endian 판이다.
    template <typename T>
    void PatchBigEndian(usize offset, typename Exactly<T>::Type value) noexcept {
        if (uint8* at = PatchTarget(offset, sizeof(T)); at != nullptr) {
            StoreBigEndian<T>(at, value);
        }
    }

private:
    void Append(Span<const uint8> bytes) noexcept {
        if (Failed() || bytes.Size() == 0) {
            return;
        }

        if (auto appended = target_->TryAppend(bytes); !appended) {
            error_ = appended.Error();
        }
    }

    uint8* PatchTarget(usize offset, usize width) noexcept {
        if (Failed()) {
            return nullptr;
        }

        if (width > target_->Size() || offset > target_->Size() - width) {
            error_ = ErrorCode::InvalidArgument;
            return nullptr;
        }

        return target_->Data() + offset;
    }

    Array<uint8>* target_;
    ErrorCode error_ = ErrorCode::None;
};

} // namespace rocks
