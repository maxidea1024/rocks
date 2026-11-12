#pragma once

#include <Base/Endian.h>
#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Platform/Memory.h>

namespace rocks {

// 외부 binary 형식을 읽는 범위 검사 cursor다. 형식마다 byte 순서가 다르므로(PNG·PSD·TrueType은
// big-endian, BMP·TGA·WAV·DDS는 little-endian) 읽기마다 순서를 명시한다. UE의 FMemoryReader처럼
// 외부 형식 파서가 쓰는 도구이며, Rocks 자체 형식(Record·Archive)은 예산·한도와 호출별 Result를
// 갖는 Serialization 모듈의 ArchiveReader로 읽는다.
//
// 실패는 이어진다(sticky). 범위를 넘는 읽기는 0이나 빈 범위를 돌려주고 Failed()를 세우며, 그 뒤의
// 읽기는 범위 안이어도 모두 실패한다. 위치는 마지막 성공 위치에 머문다. 그래서 여러 필드를 이어
// 읽고 마지막에 한 번만 확인하면 된다(UE FArchive::IsError와 같은 방식):
//   ByteReader reader(file);
//   const uint32 width = reader.ReadU32BE();
//   const uint32 height = reader.ReadU32BE();
//   const uint8 depth = reader.ReadU8();
//   if (reader.Failed()) {
//       return Result<Header>::Failure(ErrorCode::InvalidArgument);   // 잘린 입력
//   }
// 값이 형식에 맞지 않는 경우(잘못된 magic·범위 밖 필드)도 MarkFailed()로 같은 상태에 합칠 수 있다.
// 덩어리(chunk) 단위 형식은 Sub로 길이만큼의 독립 reader를 떼어 내 그 안에서만 읽는다. 반복 조건은
// `Remaining() != 0`으로 쓴다(실패하면 0이 되어 멈춘다). `!AtEnd()`는 실패한 뒤에도 참이고 위치가
// 더 나아가지 않으므로 잘린 입력에서 끝나지 않는다:
//   while (reader.Remaining() != 0) {
//       ByteReader chunk = reader.Sub(reader.ReadU32BE());
//       ...
//   }
//   if (!reader.AtEnd()) { ... }   // 잘렸거나 형식 오류
//
// 수명: caller의 불변 byte 범위를 빌린다. 원본은 reader와 Take·Peek·Sub 결과보다 오래 살아야 한다.
// 할당하지 않으며 모든 연산은 noexcept다. 복사하면 같은 원본의 독립 cursor가 된다. 같은 reader의
// 병행 사용은 caller가 동기화한다.
class ByteReader {
public:
    // 빈 입력이다(AtEnd()가 참).
    constexpr ByteReader() noexcept = default;

    constexpr explicit ByteReader(Span<const uint8> bytes) noexcept : bytes_(bytes) {
    }

    // 한 번이라도 실패했는지다.
    constexpr bool Failed() const noexcept {
        return failed_;
    }

    // 형식 의미 오류를 읽기 실패와 같은 상태로 만든다. 이후 읽기는 모두 실패한다.
    constexpr void MarkFailed() noexcept {
        failed_ = true;
    }

    // 다음에 읽을 offset이다. 실패한 뒤에는 마지막 성공 위치를 유지한다(오류 위치 진단용).
    constexpr usize Position() const noexcept {
        return position_;
    }

    constexpr usize Size() const noexcept {
        return bytes_.Size();
    }

    // 남은 byte 수다. 실패 상태면 0이다.
    constexpr usize Remaining() const noexcept {
        return failed_ ? 0 : bytes_.Size() - position_;
    }

    // 실패 없이 끝까지 읽었는지다. 형식 끝에서 남은 byte가 없음을 확인할 때 쓴다.
    constexpr bool AtEnd() const noexcept {
        return !failed_ && position_ == bytes_.Size();
    }

    // count byte를 더 읽을 수 있는지다(상태를 바꾸지 않는다).
    constexpr bool CanRead(usize count) const noexcept {
        return !failed_ && count <= bytes_.Size() - position_;
    }

    // reader가 빌린 전체 원본이다.
    constexpr Span<const uint8> Source() const noexcept {
        return bytes_;
    }

    // 절대 offset으로 이동한다. offset == Size()는 끝 위치로 허용한다. 범위 밖이면 실패다.
    constexpr bool Seek(usize offset) noexcept {
        if (failed_ || offset > bytes_.Size()) {
            failed_ = true;
            return false;
        }

        position_ = offset;
        return true;
    }

    // count byte를 건너뛴다. 남은 byte가 모자라거나 이미 실패했으면 false이며 실패 상태가
    // 된다(위치는 그대로).
    constexpr bool Skip(usize count) noexcept {
        if (!CanRead(count)) {
            failed_ = true;
            return false;
        }

        position_ += count;
        return true;
    }

    // 다음 count byte를 빌려 주고 그만큼 나아간다. 실패하면 빈 범위다(count 0의 성공도 빈
    // 범위이므로 구분은 Failed()로 한다).
    constexpr Span<const uint8> Take(usize count) noexcept {
        if (!CanRead(count)) {
            failed_ = true;
            return Span<const uint8>();
        }

        const Span<const uint8> result(bytes_.Data() + position_, count);
        position_ += count;
        return result;
    }

    // 다음 count byte를 나아가지 않고 본다. 모자라면 빈 범위를 돌려줄 뿐 실패 상태로 만들지 않는다
    // (형식 판별용 magic 확인).
    constexpr Span<const uint8> Peek(usize count) const noexcept {
        if (!CanRead(count)) {
            return Span<const uint8>();
        }

        return Span<const uint8>(bytes_.Data() + position_, count);
    }

    // 다음 output.Size() byte를 output에 복사하고 나아간다. 성공하면 true, 모자라거나 이미
    // 실패했으면 false이며 output을 0으로 채운다. output은 원본과 겹치면 안 된다(제자리 처리는
    // Take로 빌려 쓴다).
    bool ReadInto(Span<uint8> output) noexcept {
        const Span<const uint8> source = Take(output.Size());
        if (failed_) {
            if (output.Size() != 0) {
                platform::MemorySet(output.Data(), 0, output.Size());
            }

            return false;
        }

        if (output.Size() != 0) {
            platform::MemoryCopy(output.Data(), source.Data(), output.Size());
        }

        return true;
    }

    // 다음 count byte를 독립 reader로 떼어 내고 그만큼 나아간다(chunk 본문 파싱). 모자라면 이
    // reader와 결과가 모두 실패 상태다. 결과의 offset은 0부터 시작한다.
    constexpr ByteReader Sub(usize count) noexcept {
        const Span<const uint8> part = Take(count);
        ByteReader result(part);
        result.failed_ = failed_;
        return result;
    }

    constexpr uint8 ReadU8() noexcept {
        const Span<const uint8> data = Take(1);
        return failed_ ? 0 : data[0];
    }

    constexpr int8 ReadI8() noexcept {
        return static_cast<int8>(ReadU8());
    }

    // sizeof(T) byte를 little-endian으로 읽는다. T는 uint16·uint32·uint64와 같은 폭의 부호 있는
    // 정수다. 실패하면 0이다.
    template <typename T>
    constexpr T ReadLittleEndian() noexcept {
        const Span<const uint8> data = Take(sizeof(T));
        return failed_ ? T(0) : LoadLittleEndian<T>(data.Data());
    }

    // sizeof(T) byte를 big-endian으로 읽는다. 조건은 ReadLittleEndian과 같다.
    template <typename T>
    constexpr T ReadBigEndian() noexcept {
        const Span<const uint8> data = Take(sizeof(T));
        return failed_ ? T(0) : LoadBigEndian<T>(data.Data());
    }

    constexpr uint16 ReadU16LE() noexcept {
        return ReadLittleEndian<uint16>();
    }

    constexpr uint16 ReadU16BE() noexcept {
        return ReadBigEndian<uint16>();
    }

    constexpr uint32 ReadU32LE() noexcept {
        return ReadLittleEndian<uint32>();
    }

    constexpr uint32 ReadU32BE() noexcept {
        return ReadBigEndian<uint32>();
    }

    constexpr uint64 ReadU64LE() noexcept {
        return ReadLittleEndian<uint64>();
    }

    constexpr uint64 ReadU64BE() noexcept {
        return ReadBigEndian<uint64>();
    }

    // IEEE-754 bit 표현을 그대로 옮긴다(NaN payload·부호 있는 0 보존). 실패하면 +0이다.
    constexpr float ReadF32LE() noexcept {
        return platform::BitCast<float>(ReadLittleEndian<uint32>());
    }

    constexpr float ReadF32BE() noexcept {
        return platform::BitCast<float>(ReadBigEndian<uint32>());
    }

    constexpr double ReadF64LE() noexcept {
        return platform::BitCast<double>(ReadLittleEndian<uint64>());
    }

    constexpr double ReadF64BE() noexcept {
        return platform::BitCast<double>(ReadBigEndian<uint64>());
    }

private:
    Span<const uint8> bytes_;
    usize position_ = 0;
    bool failed_ = false;
};

} // namespace rocks
