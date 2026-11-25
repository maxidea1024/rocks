#include <ShaderCore/ShaderGlesBindingHeader.h>

#include "ShaderCoreInternal.h"

#include <Core/Encoding/ByteReader.h>
#include <Core/Encoding/ByteWriter.h>
#include <ShaderCore/ShaderParameterLayout.h>
#include <stdio.h>

// GLES 바인딩 머리 v1(Docs/Design/ShaderSystem 7.2절 4항, 12.2.1절 SSD26)이다. byte 형식과 규칙은
// 공개 헤더 주석을 따른다.
namespace rocks::shader {

namespace gles_header_detail {

constexpr uint32 AttributeBits = (1u << MaxShaderVertexAttributes) - 1;
// 텍스처 유닛 상한이다(PS·CS 0부터, VS 16부터 — 셰이더당 샘플러 16, 7.2절 4항).
constexpr uint32 MaxGlesTextureUnits = 32;
// 상수 버퍼 하나의 최대 vec4 수다.
constexpr uint32 MaxArrayVectors = MaxShaderConstantBufferBytes / ShaderConstantSlotBytes;
// 복사 byte 단위(원소 4 byte)다.
constexpr uint32 CopyGranule = 4;

bool IsUniformType(uint8 value) noexcept {
    return value == static_cast<uint8>(ShaderGlesUniformType::Float) ||
           value == static_cast<uint8>(ShaderGlesUniformType::Int) ||
           value == static_cast<uint8>(ShaderGlesUniformType::Uint);
}

// 읽기·쓰기 공통 규칙이다. 맞으면 null, 아니면 영어 이유다.
const char* CheckHeader(const ShaderGlesBindingHeader& header) noexcept {
    if (!core_detail::IsSingleShaderStage(header.stage)) {
        return "GLES binding header stage must be exactly one stage bit";
    }

    if ((header.attributeMask & ~AttributeBits) != 0 ||
        (header.attributeMask != 0 && header.stage != ShaderStageVertex)) {
        return "GLES binding header attribute mask is only for vertex shaders";
    }

    if (header.arrays.Size() > MaxShaderGlesArrays ||
        header.samplers.Size() > MaxShaderGlesSamplers ||
        header.copies.Size() > MaxShaderGlesCopies) {
        return "GLES binding header table is too large";
    }

    for (usize index = 0; index < header.arrays.Size(); ++index) {
        const ShaderGlesUniformArray& array = header.arrays[index];
        if (!IsUniformType(static_cast<uint8>(array.type)) || array.vec4Count == 0 ||
            array.vec4Count > MaxArrayVectors) {
            return "GLES uniform array type must be h, i or u with 1 to 4096 vectors";
        }

        if (index > 0) {
            const ShaderGlesUniformArray& previous = header.arrays[index - 1];
            const bool ordered = previous.buffer < array.buffer ||
                                 (previous.buffer == array.buffer && previous.type < array.type);
            if (!ordered) {
                return "GLES uniform arrays must be in (buffer, type) order without duplicates";
            }
        }
    }

    for (usize index = 0; index < header.samplers.Size(); ++index) {
        const ShaderGlesSampler& sampler = header.samplers[index];
        if (sampler.unit >= MaxGlesTextureUnits ||
            (index > 0 && header.samplers[index - 1].unit >= sampler.unit)) {
            return "GLES samplers must be in texture unit order without duplicates (units 0-31)";
        }

        if (!core_detail::IsShaderIdentifier(sampler.name, MaxShaderBindingNameBytes) ||
            static_cast<uint32>(sampler.source) >= ShaderSamplerSourceCount) {
            return "GLES sampler name must be an identifier and its source in the vocabulary";
        }
    }

    for (usize index = 0; index < header.copies.Size(); ++index) {
        const ShaderGlesCopy& copy = header.copies[index];
        if (copy.bytes == 0 || copy.bytes % CopyGranule != 0 ||
            copy.sourceOffset % CopyGranule != 0) {
            return "GLES copy ranges must be non-empty multiples of 4 bytes";
        }

        if (copy.arrayIndex >= header.arrays.Size() ||
            header.arrays[copy.arrayIndex].buffer != copy.buffer) {
            return "GLES copy target must be an array of the same constant buffer";
        }

        const uint64 targetEnd =
            static_cast<uint64>(copy.vec4Offset) * ShaderConstantSlotBytes + copy.bytes;
        if (targetEnd > static_cast<uint64>(header.arrays[copy.arrayIndex].vec4Count) *
                            ShaderConstantSlotBytes) {
            return "GLES copy ends past its target array";
        }

        if (index > 0) {
            const ShaderGlesCopy& previous = header.copies[index - 1];
            const uint64 previousEnd = static_cast<uint64>(previous.sourceOffset) + previous.bytes;
            const bool ordered =
                previous.buffer < copy.buffer ||
                (previous.buffer == copy.buffer && previousEnd <= copy.sourceOffset);
            if (!ordered) {
                return "GLES copies must be in (buffer, source offset) order without overlap";
            }
        }

        // 대상 구간 겹침(같은 배열)이다. 표 크기 상한이 작아 짝마다 본다.
        const uint64 targetStart = static_cast<uint64>(copy.vec4Offset) * ShaderConstantSlotBytes;
        for (usize other = 0; other < index; ++other) {
            const ShaderGlesCopy& earlier = header.copies[other];
            if (earlier.arrayIndex != copy.arrayIndex) {
                continue;
            }

            const uint64 earlierStart =
                static_cast<uint64>(earlier.vec4Offset) * ShaderConstantSlotBytes;
            const uint64 earlierEnd = earlierStart + earlier.bytes;
            if (earlierStart < targetEnd && targetStart < earlierEnd) {
                return "GLES copy targets must not overlap";
            }
        }
    }

    return nullptr;
}

char StageLetter(uint32 stage) noexcept {
    switch (stage) {
    case ShaderStageVertex:
        return 'v';
    case ShaderStagePixel:
        return 'p';
    case ShaderStageCompute:
        return 'c';
    default:
        return '\0';
    }
}

} // namespace gles_header_detail

Result<void> EncodeShaderGlesBindingHeader(const ShaderGlesBindingHeader& header,
                                           Array<uint8>& out) noexcept {
    using namespace gles_header_detail;
    if (CheckHeader(header) != nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const usize start = out.Size();
    ByteWriter writer(out);
    writer.WriteU32LE(ShaderGlesBindingHeaderVersion);
    writer.WriteU32LE(header.stage);
    writer.WriteU32LE(header.attributeMask);
    writer.WriteU32LE(static_cast<uint32>(header.arrays.Size()));
    for (const ShaderGlesUniformArray& array : header.arrays) {
        writer.WriteU16LE(array.buffer);
        writer.WriteU8(static_cast<uint8>(array.type));
        writer.WriteU32LE(array.vec4Count);
    }

    writer.WriteU32LE(static_cast<uint32>(header.samplers.Size()));
    for (const ShaderGlesSampler& sampler : header.samplers) {
        writer.WriteU16LE(sampler.unit);
        writer.WriteU8(static_cast<uint8>(sampler.name.ByteCount()));
        writer.WriteBytes(Span<const uint8>(reinterpret_cast<const uint8*>(sampler.name.Data()),
                                            sampler.name.ByteCount()));
        writer.WriteU8(static_cast<uint8>(sampler.source));
    }

    writer.WriteU32LE(static_cast<uint32>(header.copies.Size()));
    for (const ShaderGlesCopy& copy : header.copies) {
        writer.WriteU16LE(copy.buffer);
        writer.WriteU32LE(copy.sourceOffset);
        writer.WriteU32LE(copy.bytes);
        writer.WriteU16LE(copy.arrayIndex);
        writer.WriteU32LE(copy.vec4Offset);
    }

    if (auto written = writer.Status(); !written) {
        (void)out.TryResize(start);
        return written;
    }

    return Result<void>::Success();
}

ShaderReadStatus DecodeShaderGlesBindingHeader(Span<const uint8> bytes,
                                               ShaderGlesBindingHeader& out) noexcept {
    using namespace gles_header_detail;
    out.arrays.Clear();
    out.samplers.Clear();
    out.copies.Clear();
    ByteReader reader(bytes);
    const uint32 version = reader.ReadU32LE();
    if (reader.Failed()) {
        return ShaderReadStatus::Corrupt("GLES binding header is truncated");
    }

    if (version != ShaderGlesBindingHeaderVersion) {
        return ShaderReadStatus::Unsupported("GLES binding header version is not 1");
    }

    out.stage = reader.ReadU32LE();
    out.attributeMask = reader.ReadU32LE();
    const uint32 arrayCount = reader.ReadU32LE();
    if (reader.Failed() || arrayCount > MaxShaderGlesArrays) {
        return ShaderReadStatus::Corrupt("GLES uniform array table is truncated or too large");
    }

    for (uint32 index = 0; index < arrayCount; ++index) {
        ShaderGlesUniformArray array;
        array.buffer = reader.ReadU16LE();
        const uint8 type = reader.ReadU8();
        array.vec4Count = reader.ReadU32LE();
        if (reader.Failed() || !IsUniformType(type)) {
            return ShaderReadStatus::Corrupt("GLES uniform array is truncated or has an unknown "
                                             "type");
        }

        array.type = static_cast<ShaderGlesUniformType>(type);
        if (!out.arrays.TryPushBack(array)) {
            return ShaderReadStatus::Corrupt("out of memory reading the GLES binding header");
        }
    }

    const uint32 samplerCount = reader.ReadU32LE();
    if (reader.Failed() || samplerCount > MaxShaderGlesSamplers) {
        return ShaderReadStatus::Corrupt("GLES sampler table is truncated or too large");
    }

    for (uint32 index = 0; index < samplerCount; ++index) {
        ShaderGlesSampler sampler;
        sampler.unit = reader.ReadU16LE();
        const uint8 nameBytes = reader.ReadU8();
        const Span<const uint8> name = reader.Take(nameBytes);
        const uint8 source = reader.ReadU8();
        if (reader.Failed() || source >= ShaderSamplerSourceCount) {
            return ShaderReadStatus::Corrupt("GLES sampler is truncated or out of range");
        }

        sampler.name = StringView(reinterpret_cast<const char*>(name.Data()), name.Size());
        sampler.source = static_cast<ShaderSamplerSource>(source);
        if (!out.samplers.TryPushBack(sampler)) {
            return ShaderReadStatus::Corrupt("out of memory reading the GLES binding header");
        }
    }

    const uint32 copyCount = reader.ReadU32LE();
    if (reader.Failed() || copyCount > MaxShaderGlesCopies) {
        return ShaderReadStatus::Corrupt("GLES copy table is truncated or too large");
    }

    for (uint32 index = 0; index < copyCount; ++index) {
        ShaderGlesCopy copy;
        copy.buffer = reader.ReadU16LE();
        copy.sourceOffset = reader.ReadU32LE();
        copy.bytes = reader.ReadU32LE();
        copy.arrayIndex = reader.ReadU16LE();
        copy.vec4Offset = reader.ReadU32LE();
        if (reader.Failed()) {
            return ShaderReadStatus::Corrupt("GLES copy is truncated");
        }

        if (!out.copies.TryPushBack(copy)) {
            return ShaderReadStatus::Corrupt("out of memory reading the GLES binding header");
        }
    }

    if (!reader.AtEnd()) {
        return ShaderReadStatus::Corrupt("GLES binding header has trailing bytes");
    }

    if (const char* broken = CheckHeader(out); broken != nullptr) {
        return ShaderReadStatus::Corrupt(broken);
    }

    return ShaderReadStatus::Success();
}

bool ShaderGlesUniformArrayName(uint32 stage, const ShaderGlesUniformArray& array,
                                char (&out)[ShaderGlesArrayNameBytes]) noexcept {
    using namespace gles_header_detail;
    const char letter = StageLetter(stage);
    if (letter == '\0' || !IsUniformType(static_cast<uint8>(array.type))) {
        out[0] = '\0';
        return false;
    }

    const char type = static_cast<char>(array.type);
    if (array.buffer == ShaderGlesRootConstantsBuffer) {
        (void)snprintf(out, sizeof(out), "%cu_%c", letter, type);
    } else {
        (void)snprintf(out, sizeof(out), "%cc%u_%c", letter, static_cast<unsigned>(array.buffer),
                       type);
    }

    return true;
}

} // namespace rocks::shader
