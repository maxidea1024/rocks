#include "CrashReportingInternal.h"

// 모듈 PE 이미지에서 CodeView(RSDS) 레코드를
// 읽는다(Docs/Design/CrashHandlingAndLaunch.ko.md 14.2절). 메모리에 올라간 이미지(RVA 배치)를
// 바이트 배열로만 다루므로 플랫폼 헤더가 필요 없다. 모든 읽기는 available 안인지 먼저
// 확인한다(손상된 이미지에서 범위 밖을 읽지 않는다).
namespace rocks::platform::detail {

namespace {

constexpr uint16 DosSignature = 0x5A4D;          // "MZ"
constexpr uint32 NtSignature = 0x00004550;       // "PE\0\0"
constexpr uint16 Pe32PlusMagic = 0x020B;         // IMAGE_NT_OPTIONAL_HDR64_MAGIC
constexpr uint32 CodeViewType = 2;               // IMAGE_DEBUG_TYPE_CODEVIEW
constexpr uint32 CodeViewSignature = 0x53445352; // "RSDS"
constexpr usize DosHeaderBytes = 64;
constexpr usize NewHeaderOffsetField = 0x3C;
constexpr usize FileHeaderOffset = 4;
constexpr usize TimeDateStampOffset = FileHeaderOffset + 4;
constexpr usize OptionalHeaderOffset = FileHeaderOffset + 20;
constexpr usize SizeOfOptionalHeaderOffset = FileHeaderOffset + 16;
constexpr usize SizeOfImageOffset = 56;
constexpr usize RvaCountOffset = 108;
constexpr usize DataDirectoryOffset = 112;
constexpr usize DataDirectoryBytes = 8;
constexpr uint32 DebugDirectoryIndex = 6;
constexpr usize DebugEntryBytes = 28;
constexpr usize DebugTypeOffset = 12;
constexpr usize DebugSizeOffset = 16;
constexpr usize DebugAddressOffset = 20;
constexpr usize CodeViewHeaderBytes = 24; // signature, GUID, age
constexpr usize GuidBytes = 16;
constexpr usize GuidData1Bytes = 4;
constexpr usize GuidData2Bytes = 2;
constexpr usize GuidTailBytes = 8;
constexpr usize GuidClockBytes = 2;
constexpr uint32 BitsPerByte = 8;
constexpr uint32 BitsPerHexDigit = 4;
constexpr uint8 HexDigitMask = 0xF;
constexpr char HexDigits[] = "0123456789ABCDEF";

// offset부터 bytes가 available 안인가다(더하기 넘침 없이).
bool InRange(usize available, uint64 offset, uint64 bytes) noexcept {
    return offset <= available && bytes <= available - offset;
}

uint32 ReadLittle(const uint8* data, usize bytes) noexcept {
    uint32 value = 0;
    for (usize index = 0; index < bytes; ++index) {
        value |= uint32{data[index]} << (index * BitsPerByte);
    }

    return value;
}

void FormatGuid(const uint8* guid, char (&text)[PeGuidTextBytes]) noexcept {
    // 레지스트리 형식이다. Data1·Data2·Data3은 little-endian 정수다.
    usize length = 0;
    const auto putByte = [&](uint8 value) noexcept {
        text[length++] = HexDigits[value >> BitsPerHexDigit];
        text[length++] = HexDigits[value & HexDigitMask];
    };
    for (usize index = GuidData1Bytes; index-- > 0;) {
        putByte(guid[index]);
    }

    text[length++] = '-';
    for (usize field = 0; field < 2; ++field) {
        for (usize index = GuidData2Bytes; index-- > 0;) {
            putByte(guid[GuidData1Bytes + field * GuidData2Bytes + index]);
        }

        text[length++] = '-';
    }

    const uint8* tail = guid + GuidBytes - GuidTailBytes;
    for (usize index = 0; index < GuidTailBytes; ++index) {
        if (index == GuidClockBytes) {
            text[length++] = '-';
        }

        putByte(tail[index]);
    }

    text[length] = '\0';
}

} // namespace

usize PeImageSize(const uint8* image, usize headerBytes) noexcept {
    if (image == nullptr || headerBytes < DosHeaderBytes ||
        ReadLittle(image, sizeof(uint16)) != DosSignature) {
        return 0;
    }

    const uint32 headers = ReadLittle(image + NewHeaderOffsetField, sizeof(uint32));
    if (headers < DosHeaderBytes ||
        !InRange(headerBytes, headers, OptionalHeaderOffset + SizeOfImageOffset + sizeof(uint32)) ||
        ReadLittle(image + headers, sizeof(uint32)) != NtSignature ||
        ReadLittle(image + headers + OptionalHeaderOffset, sizeof(uint16)) != Pe32PlusMagic) {
        return 0;
    }

    return ReadLittle(image + headers + OptionalHeaderOffset + SizeOfImageOffset, sizeof(uint32));
}

bool ParsePeCodeView(const uint8* image, usize available, PeCodeView& out) noexcept {
    out = PeCodeView{};
    if (image == nullptr || available < DosHeaderBytes ||
        ReadLittle(image, sizeof(uint16)) != DosSignature) {
        return false;
    }

    const uint32 headers = ReadLittle(image + NewHeaderOffsetField, sizeof(uint32));
    if (headers < DosHeaderBytes ||
        !InRange(available, headers,
                 OptionalHeaderOffset + DataDirectoryOffset +
                     (DebugDirectoryIndex + 1) * DataDirectoryBytes) ||
        ReadLittle(image + headers, sizeof(uint32)) != NtSignature) {
        return false;
    }

    const uint8* optional = image + headers + OptionalHeaderOffset;
    const uint32 optionalBytes =
        ReadLittle(image + headers + SizeOfOptionalHeaderOffset, sizeof(uint16));
    if (ReadLittle(optional, sizeof(uint16)) != Pe32PlusMagic ||
        optionalBytes < DataDirectoryOffset + (DebugDirectoryIndex + 1) * DataDirectoryBytes ||
        ReadLittle(optional + RvaCountOffset, sizeof(uint32)) <= DebugDirectoryIndex) {
        return false;
    }

    out.timeDateStamp = ReadLittle(image + headers + TimeDateStampOffset, sizeof(uint32));
    out.sizeOfImage = ReadLittle(optional + SizeOfImageOffset, sizeof(uint32));
    if (out.sizeOfImage > available) {
        return false;
    }

    const uint8* debug = optional + DataDirectoryOffset + DebugDirectoryIndex * DataDirectoryBytes;
    const uint32 directory = ReadLittle(debug, sizeof(uint32));
    const uint32 directoryBytes = ReadLittle(debug + sizeof(uint32), sizeof(uint32));
    if (directory == 0 || directoryBytes == 0) {
        return true; // 디버그 정보 없는 이미지다(올바르다)
    }

    if (directoryBytes % DebugEntryBytes != 0 || !InRange(available, directory, directoryBytes)) {
        return false;
    }

    for (usize offset = 0; offset < directoryBytes; offset += DebugEntryBytes) {
        const uint8* entry = image + directory + offset;
        const uint32 rawAddress = ReadLittle(entry + DebugAddressOffset, sizeof(uint32));
        const uint32 rawBytes = ReadLittle(entry + DebugSizeOffset, sizeof(uint32));
        if (ReadLittle(entry + DebugTypeOffset, sizeof(uint32)) != CodeViewType ||
            rawAddress == 0) {
            continue;
        }

        if (rawBytes <= CodeViewHeaderBytes || !InRange(available, rawAddress, rawBytes)) {
            return false;
        }

        const uint8* record = image + rawAddress;
        if (ReadLittle(record, sizeof(uint32)) != CodeViewSignature) {
            continue;
        }

        FormatGuid(record + sizeof(uint32), out.guid);
        out.age = ReadLittle(record + sizeof(uint32) + GuidBytes, sizeof(uint32));
        // 링크가 넣은 경로에서 파일 이름만 남긴다(빌드 호스트 경로를 메타데이터에 남기지 않는다).
        const auto* name = reinterpret_cast<const char*>(record + CodeViewHeaderBytes);
        const usize limit = rawBytes - CodeViewHeaderBytes;
        usize start = 0;
        usize end = 0;
        for (; end < limit && name[end] != '\0'; ++end) {
            if (name[end] == '\\' || name[end] == '/') {
                start = end + 1;
            }
        }

        out.pdbName = name + start;
        out.pdbNameBytes = end - start;
        out.hasCodeView = true;
        return true;
    }

    return true;
}

} // namespace rocks::platform::detail
