#include "NuggetInternal.h"

#include <AssetNugget/NuggetKey.h>

#include <Base/Endian.h>
#include <Core/Algorithms.h>
#include <Core/Assert.h>
#include <Core/Build/BuildInfo.h>
#include <Core/Text/StringBuilder.h>
#include <DeterministicMath/DeterministicMath.h>
#include <Platform/Math/GenericPlatformMath.h>

// 에셋 너겟 키 기록이다(Docs/Design/AssetSystem.ko.md 9.1절 결정 2–4).
namespace rocks {

namespace {

// 정수·Guid 인코딩 폭이다.
constexpr usize U32Bytes = 4;
constexpr usize U64Bytes = 8;
constexpr usize GuidBytes = 16;
// WriteRaw trace의 16진 줄 하나에 담는 byte 수다.
constexpr usize RawTraceLineBytes = 32;

// NaN 단일화 값이다(quiet NaN, 부호 0 — 9.1절 결정 2).
constexpr uint32 CanonicalNan32 = 0x7fc00000u;
constexpr uint64 CanonicalNan64 = 0x7ff8000000000000ull;

// 공통 머리의 첫 기록이다(형식 식별).
constexpr StringView KeyMagic = "RocksAssetNugget";

// SCC key·구성원 key의 영역 구분 문자열이다(다른 키 기록과 같은 byte가 되지 않게).
constexpr StringView SccDomain = "RocksNuggetScc";
constexpr StringView MemberDomain = "RocksNuggetMember";

bool IsNan32(uint32 bits) noexcept {
    constexpr uint32 ExponentMask = 0x7f800000u;
    constexpr uint32 MantissaMask = 0x007fffffu;
    return (bits & ExponentMask) == ExponentMask && (bits & MantissaMask) != 0;
}

bool IsNan64(uint64 bits) noexcept {
    constexpr uint64 ExponentMask = 0x7ff0000000000000ull;
    constexpr uint64 MantissaMask = 0x000fffffffffffffull;
    return (bits & ExponentMask) == ExponentMask && (bits & MantissaMask) != 0;
}

// Guid의 정규 인코딩(high → low, 각각 big-endian — 표준 문자열 순서와 같고 byte 사전순이 값
// 순서와 같다)이다.
void EncodeGuid(Guid value, uint8 (&out)[GuidBytes]) noexcept {
    StoreBigEndian<uint64>(out, value.high);
    StoreBigEndian<uint64>(out + U64Bytes, value.low);
}

} // namespace

void NuggetKeyWriter::Emit(Span<const uint8> bytes) noexcept {
    if (error_ != ErrorCode::None || finished_) {
        return;
    }

    if (buffer_ != nullptr) {
        if (auto appended = buffer_->TryAppend(bytes); !appended) {
            Fail(appended.Error());
        }

        return;
    }

    hasher_.Update(bytes);
}

void NuggetKeyWriter::Tag(NuggetKeyTag tag) noexcept {
    const uint8 byte = static_cast<uint8>(tag);
    Emit(Span<const uint8>(&byte, 1));
}

void NuggetKeyWriter::Note(StringView text) noexcept {
    if (trace_ != nullptr && error_ == ErrorCode::None) {
        trace_->Line(text);
    }
}

void NuggetKeyWriter::WriteNone() noexcept {
    Note("none");
    Tag(NuggetKeyTag::None);
}

void NuggetKeyWriter::WriteBool(bool value) noexcept {
    Note(value ? "bool true" : "bool false");
    Tag(NuggetKeyTag::Bool);
    const uint8 byte = value ? 1 : 0;
    Emit(Span<const uint8>(&byte, 1));
}

void NuggetKeyWriter::WriteU32(uint32 value) noexcept {
    if (trace_ != nullptr) {
        char text[32] = {};
        FixedStringBuilder line{Span<char>(text)};
        (void)line.TryAppendFormat("u32 {}", value);
        Note(line.View());
    }

    Tag(NuggetKeyTag::U32);
    uint8 bytes[U32Bytes];
    StoreLittleEndian<uint32>(bytes, value);
    Emit(bytes);
}

void NuggetKeyWriter::WriteU64(uint64 value) noexcept {
    if (trace_ != nullptr) {
        char text[32] = {};
        FixedStringBuilder line{Span<char>(text)};
        (void)line.TryAppendFormat("u64 {}", value);
        Note(line.View());
    }

    Tag(NuggetKeyTag::U64);
    uint8 bytes[U64Bytes];
    StoreLittleEndian<uint64>(bytes, value);
    Emit(bytes);
}

void NuggetKeyWriter::WriteI64(int64 value) noexcept {
    if (trace_ != nullptr) {
        char text[32] = {};
        FixedStringBuilder line{Span<char>(text)};
        (void)line.TryAppendFormat("i64 {}", value);
        Note(line.View());
    }

    Tag(NuggetKeyTag::I64);
    uint8 bytes[U64Bytes];
    StoreLittleEndian<uint64>(bytes, static_cast<uint64>(value));
    Emit(bytes);
}

void NuggetKeyWriter::WriteF32(float value) noexcept {
    uint32 bits = platform::BitCast<uint32>(value);
    bits = IsNan32(bits) ? CanonicalNan32 : bits;
    if (trace_ != nullptr) {
        char text[32] = {};
        FixedStringBuilder line{Span<char>(text)};
        (void)line.TryAppendFormat("f32 0x{:08x}", bits);
        Note(line.View());
    }

    Tag(NuggetKeyTag::F32);
    uint8 bytes[U32Bytes];
    StoreLittleEndian<uint32>(bytes, bits);
    Emit(bytes);
}

void NuggetKeyWriter::WriteF64(double value) noexcept {
    uint64 bits = platform::BitCast<uint64>(value);
    bits = IsNan64(bits) ? CanonicalNan64 : bits;
    if (trace_ != nullptr) {
        char text[40] = {};
        FixedStringBuilder line{Span<char>(text)};
        (void)line.TryAppendFormat("f64 0x{:016x}", bits);
        Note(line.View());
    }

    Tag(NuggetKeyTag::F64);
    uint8 bytes[U64Bytes];
    StoreLittleEndian<uint64>(bytes, bits);
    Emit(bytes);
}

void NuggetKeyWriter::WriteGuid(Guid value) noexcept {
    if (trace_ != nullptr) {
        char text[48] = {};
        FixedStringBuilder line{Span<char>(text)};
        (void)line.TryAppendFormat("guid {}", value);
        Note(line.View());
    }

    Tag(NuggetKeyTag::Guid);
    uint8 bytes[GuidBytes];
    EncodeGuid(value, bytes);
    Emit(bytes);
}

void NuggetKeyWriter::WriteDigest(const Sha256Digest& value) noexcept {
    if (trace_ != nullptr) {
        char hex[Sha256Digest::HexBytes + 1];
        value.ToHex(hex);
        char text[Sha256Digest::HexBytes + 8] = {};
        FixedStringBuilder line{Span<char>(text)};
        (void)line.TryAppendFormat("digest {}", StringView(hex, Sha256Digest::HexBytes));
        Note(line.View());
    }

    Tag(NuggetKeyTag::Digest);
    Emit(value.bytes);
}

void NuggetKeyWriter::WriteString(StringView value) noexcept {
    if (!value.ValidateUtf8()) {
        Fail(ErrorCode::InvalidArgument);
        return;
    }

    if (trace_ != nullptr) {
        Note("string");
        Note(value);
    }

    Tag(NuggetKeyTag::String);
    uint8 length[U64Bytes];
    StoreLittleEndian<uint64>(length, value.ByteCount());
    Emit(length);
    Emit(Span<const uint8>(reinterpret_cast<const uint8*>(value.Data()), value.ByteCount()));
}

void NuggetKeyWriter::WriteBytes(Span<const uint8> value) noexcept {
    if (trace_ != nullptr) {
        char text[40] = {};
        FixedStringBuilder line{Span<char>(text)};
        (void)line.TryAppendFormat("bytes {}", value.Size());
        Note(line.View());
    }

    Tag(NuggetKeyTag::Bytes);
    uint8 length[U64Bytes];
    StoreLittleEndian<uint64>(length, value.Size());
    Emit(length);
    Emit(value);
}

namespace {

// 원소 수 기록(List·Map·Set)의 이름이다.
StringView CountName(NuggetKeyTag tag) noexcept {
    return tag == NuggetKeyTag::List  ? StringView("list")
           : tag == NuggetKeyTag::Map ? StringView("map")
                                      : StringView("set");
}

} // namespace

void NuggetKeyWriter::WriteList(uint64 count) noexcept {
    if (trace_ != nullptr) {
        char text[40] = {};
        FixedStringBuilder line{Span<char>(text)};
        (void)line.TryAppendFormat("{} {}", CountName(NuggetKeyTag::List), count);
        Note(line.View());
    }

    Tag(NuggetKeyTag::List);
    uint8 bytes[U64Bytes];
    StoreLittleEndian<uint64>(bytes, count);
    Emit(bytes);
}

void NuggetKeyWriter::WriteMap(uint64 count) noexcept {
    if (trace_ != nullptr) {
        char text[40] = {};
        FixedStringBuilder line{Span<char>(text)};
        (void)line.TryAppendFormat("{} {}", CountName(NuggetKeyTag::Map), count);
        Note(line.View());
    }

    Tag(NuggetKeyTag::Map);
    uint8 bytes[U64Bytes];
    StoreLittleEndian<uint64>(bytes, count);
    Emit(bytes);
}

void NuggetKeyWriter::WriteSet(uint64 count) noexcept {
    if (trace_ != nullptr) {
        char text[40] = {};
        FixedStringBuilder line{Span<char>(text)};
        (void)line.TryAppendFormat("{} {}", CountName(NuggetKeyTag::Set), count);
        Note(line.View());
    }

    Tag(NuggetKeyTag::Set);
    uint8 bytes[U64Bytes];
    StoreLittleEndian<uint64>(bytes, count);
    Emit(bytes);
}

void NuggetKeyWriter::WriteField(StringView name) noexcept {
    if (!name.ValidateUtf8()) {
        Fail(ErrorCode::InvalidArgument);
        return;
    }

    if (trace_ != nullptr) {
        Note("field");
        Note(name);
    }

    Tag(NuggetKeyTag::Field);
    uint8 length[U64Bytes];
    StoreLittleEndian<uint64>(length, name.ByteCount());
    Emit(length);
    Emit(Span<const uint8>(reinterpret_cast<const uint8*>(name.Data()), name.ByteCount()));
}

void NuggetKeyWriter::WriteStructBegin() noexcept {
    Note("struct");
    Tag(NuggetKeyTag::Struct);
}

void NuggetKeyWriter::WriteStructEnd() noexcept {
    Note("end");
    Tag(NuggetKeyTag::End);
}

void NuggetKeyWriter::WriteRaw(Span<const uint8> encoded) noexcept {
    if (trace_ != nullptr) {
        char text[40] = {};
        FixedStringBuilder line{Span<char>(text)};
        (void)line.TryAppendFormat("raw {}", encoded.Size());
        Note(line.View());
        // 내용도 남긴다(손으로 정렬해 흘린 Map·Set의 키 불일치를 trace로 찾게 — 16진 줄마다
        // RawTraceLineBytes byte).
        constexpr char Digits[] = "0123456789abcdef";
        char hex[2 + RawTraceLineBytes * 2] = {' ', ' '};
        for (usize offset = 0; offset < encoded.Size(); offset += RawTraceLineBytes) {
            const usize count = encoded.Size() - offset < RawTraceLineBytes
                                    ? encoded.Size() - offset
                                    : RawTraceLineBytes;
            for (usize index = 0; index < count; ++index) {
                const uint8 value = encoded[offset + index];
                hex[2 + index * 2] = Digits[value >> 4];
                hex[2 + index * 2 + 1] = Digits[value & 0x0f];
            }

            Note(StringView(hex, 2 + count * 2));
        }
    }

    Emit(encoded);
}

Result<Sha256Digest> NuggetKeyWriter::Finish() noexcept {
    if (error_ != ErrorCode::None) {
        return Result<Sha256Digest>::Failure(error_);
    }

    if (buffer_ != nullptr || finished_) {
        return Result<Sha256Digest>::Failure(ErrorCode::InvalidArgument);
    }

    finished_ = true;
    return Result<Sha256Digest>::Success(hasher_.Finish());
}

void WriteNuggetKeyHeader(NuggetKeyWriter& writer, const NuggetProcessorId& processor,
                          Guid projectGeneration) noexcept {
    writer.WriteString(KeyMagic);
    writer.WriteU32(NuggetKeyFormatVersion);
    writer.WriteGuid(EngineNuggetGeneration);
    writer.WriteGuid(projectGeneration);
    writer.WriteGuid(processor.processor);
    writer.WriteGuid(processor.version);
    writer.WriteU32(deterministic::Version);
    writer.WriteDigest(build::ToolchainFingerprint());
}

void WriteNuggetEncoder(NuggetKeyWriter& writer, StringView name, StringView version,
                        Span<const uint8> settings) noexcept {
    writer.WriteString(name);
    writer.WriteString(version);
    writer.WriteBytes(settings);
}

namespace {

// 구성원의 정규 인코딩 순서다(AssetId 16 byte 사전순).
bool MemberLess(const NuggetSccMember& left, const NuggetSccMember& right) noexcept {
    uint8 a[GuidBytes];
    uint8 b[GuidBytes];
    EncodeGuid(left.asset.value, a);
    EncodeGuid(right.asset.value, b);
    for (usize index = 0; index < GuidBytes; ++index) {
        if (a[index] != b[index]) {
            return a[index] < b[index];
        }
    }

    return false;
}

// 튜플의 정규 인코딩 byte(구성원 AssetId, u64 길이 + 역할, 의존 AssetId, key)다.
Result<void> EncodeDependency(const NuggetSccDependency& tuple, Array<uint8>& out) noexcept {
    NuggetKeyWriter writer(out);
    writer.WriteGuid(tuple.member.value);
    writer.WriteString(tuple.role);
    writer.WriteGuid(tuple.dependency.value);
    writer.WriteDigest(tuple.dependencyKey);
    return writer.Error();
}

} // namespace

Result<Sha256Digest> NuggetSccKey(Allocator& allocator, Span<const NuggetSccMember> members,
                                  Span<const NuggetSccDependency> external) noexcept {
    using Out = Sha256Digest;
    Array<NuggetSccMember> sorted(allocator);
    if (auto copied = sorted.TryAppend(members); !copied) {
        return Result<Out>::Failure(copied.Error());
    }

    algo::Sort(sorted.View(), MemberLess);
    if (sorted.IsEmpty()) {
        return Result<Out>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < sorted.Size(); ++index) {
        if (sorted[index].asset.IsNull() ||
            (index > 0 && sorted[index].asset == sorted[index - 1].asset)) {
            return Result<Out>::Failure(ErrorCode::InvalidArgument);
        }
    }

    // 튜플마다 정규 인코딩을 만들어 byte 사전순으로 정렬하고 완전 중복을 합친다.
    Array<Array<uint8>> encoded(allocator);
    if (auto reserved = encoded.TryReserve(external.Size()); !reserved) {
        return Result<Out>::Failure(reserved.Error());
    }

    // 정렬된 구성원 표에서 찾는다(튜플의 구성원은 SCC 안, 의존은 SCC 밖이어야 한다).
    auto isMember = [&sorted](AssetId asset) noexcept {
        const NuggetSccMember probe{asset, {}};
        const usize at = algo::LowerBound(sorted.View(), probe, MemberLess);
        return at < sorted.Size() && sorted[at].asset == asset;
    };
    for (const NuggetSccDependency& tuple : external) {
        if (tuple.member.IsNull() || tuple.dependency.IsNull() || !isMember(tuple.member) ||
            isMember(tuple.dependency)) {
            return Result<Out>::Failure(ErrorCode::InvalidArgument);
        }

        Array<uint8> bytes(allocator);
        if (auto made = EncodeDependency(tuple, bytes); !made) {
            return Result<Out>::Failure(made.Error());
        }

        // 자리는 위에서 잡았다.
        (void)encoded.TryPushBack(Move(bytes));
    }

    algo::Sort(encoded.View(), [](const Array<uint8>& left, const Array<uint8>& right) noexcept {
        return nugget_detail::CompareBytes(left.View(), right.View()) < 0;
    });

    usize unique = 0;
    for (usize index = 0; index < encoded.Size(); ++index) {
        if (index > 0 &&
            nugget_detail::CompareBytes(encoded[index].View(), encoded[index - 1].View()) == 0) {
            continue;
        }

        ++unique;
    }

    NuggetKeyWriter writer;
    writer.WriteString(SccDomain);
    writer.WriteList(sorted.Size());
    for (const NuggetSccMember& member : sorted) {
        writer.WriteGuid(member.asset.value);
        writer.WriteDigest(member.localKey);
    }

    writer.WriteList(unique);
    for (usize index = 0; index < encoded.Size(); ++index) {
        if (index > 0 &&
            nugget_detail::CompareBytes(encoded[index].View(), encoded[index - 1].View()) == 0) {
            continue;
        }

        writer.WriteRaw(encoded[index].View());
    }

    return writer.Finish();
}

Sha256Digest NuggetMemberKey(const Sha256Digest& sccKey, AssetId asset) noexcept {
    NuggetKeyWriter writer;
    writer.WriteString(MemberDomain);
    writer.WriteDigest(sccKey);
    writer.WriteGuid(asset.value);
    // 고정 문자열(유효한 UTF-8)과 고정 폭 값뿐이고 해시 기록기라 실패하지 않는다(불변식).
    auto key = writer.Finish();
    ROCKS_ASSERT(key);
    return key.Value();
}

} // namespace rocks
