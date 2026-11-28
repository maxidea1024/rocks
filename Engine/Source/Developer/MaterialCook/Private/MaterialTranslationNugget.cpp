#include <Base/NumericLimits.h>
#include <Core/Encoding/ByteReader.h>
#include <Core/Encoding/ByteWriter.h>
#include <Core/Move.h>
#include <MaterialCompiler/MaterialIrOp.h>
#include <MaterialCook/MaterialTranslationNugget.h>

// 번역 너겟 값의 부호화다(Docs/Design/MaterialSystem 12.8절 MS3c "값").
namespace rocks {

namespace {

using material::IrValue;

// 열거 값 상한이다(해석이 범위 밖을 손상으로 본다).
constexpr uint8 LastSlotKind = static_cast<uint8>(material::MaterialConstantSlotKind::DoubleOffset);
constexpr uint8 LastFrequency = static_cast<uint8>(material::IrFrequency::Pixel);
constexpr uint8 LastScalar = static_cast<uint8>(material::IrScalar::Attributes);
constexpr uint8 LastTextureKind = static_cast<uint8>(material::IrTextureKind::External);
constexpr uint8 LastParameterKind = static_cast<uint8>(material::IrParameterKind::DoubleVector);
constexpr uint8 LastTextureSource = static_cast<uint8>(material::IrTextureSource::Parameter);
constexpr uint8 LastTextureSampler = static_cast<uint8>(material::IrTextureSampler::SharedClamp);
constexpr uint8 LastSeverity = static_cast<uint8>(nodegraph::GraphDiagnosticSeverity::Error);

void WriteGuid(ByteWriter& writer, Guid value) noexcept {
    writer.WriteLittleEndian<uint64>(value.high);
    writer.WriteLittleEndian<uint64>(value.low);
}

Guid ReadGuid(ByteReader& reader) noexcept {
    const uint64 high = reader.ReadLittleEndian<uint64>();
    const uint64 low = reader.ReadLittleEndian<uint64>();
    return Guid{high, low};
}

Result<void> WriteText(ByteWriter& writer, StringView text) noexcept {
    if (text.ByteCount() > NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    writer.WriteLittleEndian<uint32>(static_cast<uint32>(text.ByteCount()));
    writer.WriteBytes(
        Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount()));
    return Result<void>::Success();
}

bool ReadText(ByteReader& reader, String& out) noexcept {
    const uint32 length = reader.ReadLittleEndian<uint32>();
    const Span<const uint8> bytes = reader.Take(length);
    return !reader.Failed() &&
           out.TryAssign(StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size()))
               .IsOk();
}

Result<void> WriteCount(ByteWriter& writer, usize count) noexcept {
    if (count > NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    writer.WriteLittleEndian<uint32>(static_cast<uint32>(count));
    return Result<void>::Success();
}

// 원소 수다(남은 byte로 상한 — 손상된 수가 큰 할당을 일으키지 않게).
bool ReadCount(ByteReader& reader, usize minimumElementBytes, uint32& count) noexcept {
    count = reader.ReadLittleEndian<uint32>();
    return !reader.Failed() &&
           static_cast<usize>(count) * minimumElementBytes <= reader.Remaining();
}

void WriteType(ByteWriter& writer, const material::IrType& type) noexcept {
    writer.WriteU8(static_cast<uint8>(type.scalar));
    writer.WriteU8(type.components);
    writer.WriteU8(type.rows);
    writer.WriteU8(static_cast<uint8>(type.texture));
}

bool ReadType(ByteReader& reader, material::IrType& type) noexcept {
    const uint8 scalar = reader.ReadU8();
    type.components = reader.ReadU8();
    type.rows = reader.ReadU8();
    const uint8 texture = reader.ReadU8();
    if (reader.Failed() || scalar > LastScalar || texture > LastTextureKind) {
        return false;
    }

    type.scalar = static_cast<material::IrScalar>(scalar);
    type.texture = static_cast<material::IrTextureKind>(texture);
    return true;
}

// 각 part를 쓴다.
Result<void> WriteParts(const MaterialTranslationValue& value, uint32 index,
                        Array<uint8>& bytes) noexcept {
    ByteWriter writer(bytes);
    switch (index) {
    case 0:
        writer.WriteLittleEndian<uint32>(MaterialTranslationValueVersion);
        writer.WriteLittleEndian<uint32>(MaterialTranslationValueParts);
        if (auto counted = WriteCount(writer, value.diagnostics.Size()); !counted) {
            return counted;
        }

        break;
    case 1:
        writer.WriteBytes(Span<const uint8>(reinterpret_cast<const uint8*>(value.source.Data()),
                                            value.source.ByteCount()));
        break;
    case 2:
        writer.WriteLittleEndian<uint32>(value.floatVectors);
        if (auto counted = WriteCount(writer, value.members.Size()); !counted) {
            return counted;
        }

        for (const material::MaterialConstantMember& member : value.members) {
            writer.WriteLittleEndian<uint32>(member.value);
            writer.WriteLittleEndian<uint32>(member.step);
            writer.WriteU8(static_cast<uint8>(member.kind));
            writer.WriteLittleEndian<uint32>(member.vector);
            writer.WriteU8(member.firstComponent);
            writer.WriteU8(member.components);
        }

        break;
    case 3:
        if (auto counted = WriteCount(writer, value.steps.Size()); !counted) {
            return counted;
        }

        for (const IrValue& step : value.steps) {
            WriteType(writer, step.type);
            writer.WriteU8(static_cast<uint8>(step.op));
            writer.WriteU8(step.operandCount);
            writer.WriteU8(static_cast<uint8>(step.frequency));
            for (material::IrValueId operand : step.operands) {
                writer.WriteLittleEndian<uint32>(operand);
            }

            for (uint64 immediate : step.immediate) {
                writer.WriteLittleEndian<uint64>(immediate);
            }
        }

        if (auto counted = WriteCount(writer, value.parameters.Size()); !counted) {
            return counted;
        }

        for (const material::IrParameter& parameter : value.parameters) {
            WriteGuid(writer, parameter.id);
            writer.WriteU8(static_cast<uint8>(parameter.kind));
            WriteType(writer, parameter.type);
        }

        break;
    case 4:
        if (auto counted = WriteCount(writer, value.textures.Size()); !counted) {
            return counted;
        }

        for (const material::IrTexture& texture : value.textures) {
            writer.WriteU8(static_cast<uint8>(texture.source));
            WriteGuid(writer, texture.id);
            writer.WriteU8(static_cast<uint8>(texture.kind));
            writer.WriteU8(static_cast<uint8>(texture.sampler));
        }

        break;
    case 5:
        if (auto traits = material::WriteMaterialShaderTraits(value.traits, bytes); !traits) {
            return traits;
        }

        break;
    case 6:
        // sampling scale(12.5절) — TODO(MS3d): 표가 생기면 채운다.
        writer.WriteLittleEndian<uint32>(0);
        break;
    case 7:
        if (auto counted = WriteCount(writer, value.locations.Size()); !counted) {
            return counted;
        }

        for (const material::MaterialSourceLocation& location : value.locations) {
            writer.WriteLittleEndian<uint32>(location.line);
            WriteGuid(writer, location.node);
        }

        break;
    case 8:
        writer.WriteLittleEndian<uint32>(value.irValues);
        break;
    case 9:
        writer.WriteBytes(Span<const uint8>(value.irHash.bytes, Sha256Digest::Bytes));
        break;
    case 10:
        for (const MaterialTranslationDiagnostic& diagnostic : value.diagnostics) {
            WriteGuid(writer, diagnostic.asset);
            WriteGuid(writer, diagnostic.graph);
            WriteGuid(writer, diagnostic.node);
            writer.WriteU8(static_cast<uint8>(diagnostic.severity));
            writer.WriteLittleEndian<uint32>(diagnostic.code);
            if (auto pin = WriteText(writer, diagnostic.pin.View()); !pin) {
                return pin;
            }

            if (auto message = WriteText(writer, diagnostic.message.View()); !message) {
                return message;
            }
        }

        break;
    case 11:
        if (auto counted = WriteCount(writer, value.userSceneTextures.Size()); !counted) {
            return counted;
        }

        for (const String& name : value.userSceneTextures) {
            if (auto text = WriteText(writer, name.View()); !text) {
                return text;
            }
        }

        break;
    case 12:
        writer.WriteLittleEndian<uint32>(value.consultedTarget);
        break;
    default:
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return writer.Status();
}

// part index를 해석한다(diagnostics는 part 0의 수만큼).
bool ReadPart(Span<const uint8> bytes, uint32 index, uint32 diagnostics,
              MaterialTranslationValue& out) noexcept {
    ByteReader reader(bytes);
    switch (index) {
    case 1:
        if (!out.source
                 .TryAssign(StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size()))
                 .IsOk()) {
            return false;
        }

        reader.Skip(bytes.Size());
        break;
    case 2: {
        out.floatVectors = reader.ReadLittleEndian<uint32>();
        uint32 count = 0;
        constexpr usize MemberBytes = 15;
        if (!ReadCount(reader, MemberBytes, count)) {
            return false;
        }

        for (uint32 at = 0; at < count; ++at) {
            material::MaterialConstantMember member;
            member.value = reader.ReadLittleEndian<uint32>();
            member.step = reader.ReadLittleEndian<uint32>();
            const uint8 kind = reader.ReadU8();
            member.vector = reader.ReadLittleEndian<uint32>();
            member.firstComponent = reader.ReadU8();
            member.components = reader.ReadU8();
            if (reader.Failed() || kind > LastSlotKind || !out.members.TryPushBack(member)) {
                return false;
            }

            out.members[out.members.Size() - 1].kind =
                static_cast<material::MaterialConstantSlotKind>(kind);
        }

        break;
    }
    case 3: {
        uint32 count = 0;
        constexpr usize StepBytes = 4 + 3 + 4 * sizeof(uint32) + 4 * sizeof(uint64);
        if (!ReadCount(reader, StepBytes, count)) {
            return false;
        }

        for (uint32 at = 0; at < count; ++at) {
            IrValue step;
            const bool typed = ReadType(reader, step.type);
            const uint8 op = reader.ReadU8();
            step.operandCount = reader.ReadU8();
            const uint8 frequency = reader.ReadU8();
            for (material::IrValueId& operand : step.operands) {
                operand = reader.ReadLittleEndian<uint32>();
            }

            for (uint64& immediate : step.immediate) {
                immediate = reader.ReadLittleEndian<uint64>();
            }

            if (!typed || reader.Failed() || op >= material::IrOpCount ||
                frequency > LastFrequency || step.operandCount > material::MaxIrOperands) {
                return false;
            }

            step.op = static_cast<material::IrOp>(op);
            step.frequency = static_cast<material::IrFrequency>(frequency);
            if (!out.steps.TryPushBack(step)) {
                return false;
            }
        }

        constexpr usize ParameterBytes = 16 + 1 + 4;
        if (!ReadCount(reader, ParameterBytes, count)) {
            return false;
        }

        for (uint32 at = 0; at < count; ++at) {
            material::IrParameter parameter;
            parameter.id = ReadGuid(reader);
            const uint8 kind = reader.ReadU8();
            if (!ReadType(reader, parameter.type) || kind > LastParameterKind) {
                return false;
            }

            parameter.kind = static_cast<material::IrParameterKind>(kind);
            if (!out.parameters.TryPushBack(parameter)) {
                return false;
            }
        }

        break;
    }
    case 4: {
        uint32 count = 0;
        constexpr usize TextureBytes = 1 + 16 + 1 + 1;
        if (!ReadCount(reader, TextureBytes, count)) {
            return false;
        }

        for (uint32 at = 0; at < count; ++at) {
            material::IrTexture texture;
            const uint8 source = reader.ReadU8();
            texture.id = ReadGuid(reader);
            const uint8 kind = reader.ReadU8();
            const uint8 sampler = reader.ReadU8();
            if (reader.Failed() || source > LastTextureSource || kind > LastTextureKind ||
                sampler > LastTextureSampler) {
                return false;
            }

            texture.source = static_cast<material::IrTextureSource>(source);
            texture.kind = static_cast<material::IrTextureKind>(kind);
            texture.sampler = static_cast<material::IrTextureSampler>(sampler);
            // 참조 텍스처의 기본 asset은 자기 자신이다(파라미터는 처리기가 그래프에서 채운다).
            texture.defaultAsset =
                texture.source == material::IrTextureSource::Asset ? texture.id : Guid{};
            if (!out.textures.TryPushBack(texture)) {
                return false;
            }
        }

        break;
    }
    case 5: {
        usize consumed = 0;
        if (!material::ReadMaterialShaderTraits(bytes, out.traits, consumed)) {
            return false;
        }

        reader.Skip(consumed);
        break;
    }
    case 6:
        if (reader.ReadLittleEndian<uint32>() != 0) {
            return false;
        }

        break;
    case 7: {
        uint32 count = 0;
        constexpr usize LocationBytes = 4 + 16;
        if (!ReadCount(reader, LocationBytes, count)) {
            return false;
        }

        for (uint32 at = 0; at < count; ++at) {
            material::MaterialSourceLocation location;
            location.line = reader.ReadLittleEndian<uint32>();
            location.node = ReadGuid(reader);
            if (reader.Failed() || !out.locations.TryPushBack(location)) {
                return false;
            }
        }

        break;
    }
    case 8:
        out.irValues = reader.ReadLittleEndian<uint32>();
        break;
    case 9:
        if (!reader.ReadInto(Span<uint8>(out.irHash.bytes, Sha256Digest::Bytes))) {
            return false;
        }

        break;
    case 10:
        for (uint32 at = 0; at < diagnostics; ++at) {
            MaterialTranslationDiagnostic diagnostic(out.diagnostics.GetAllocator());
            diagnostic.asset = ReadGuid(reader);
            diagnostic.graph = ReadGuid(reader);
            diagnostic.node = ReadGuid(reader);
            const uint8 severity = reader.ReadU8();
            diagnostic.code = reader.ReadLittleEndian<uint32>();
            if (reader.Failed() || severity > LastSeverity || !ReadText(reader, diagnostic.pin) ||
                !ReadText(reader, diagnostic.message)) {
                return false;
            }

            diagnostic.severity = static_cast<nodegraph::GraphDiagnosticSeverity>(severity);
            if (!out.diagnostics.TryPushBack(Move(diagnostic))) {
                return false;
            }
        }

        break;
    case 11: {
        uint32 count = 0;
        constexpr usize NameBytes = 4;
        if (!ReadCount(reader, NameBytes, count)) {
            return false;
        }

        for (uint32 at = 0; at < count; ++at) {
            String name(out.userSceneTextures.GetAllocator());
            if (!ReadText(reader, name) || !out.userSceneTextures.TryPushBack(Move(name))) {
                return false;
            }
        }

        break;
    }
    case 12:
        out.consultedTarget = reader.ReadLittleEndian<uint32>();
        break;
    default:
        return false;
    }

    // part는 정확히 다 읽어야 한다(남은 byte는 손상).
    return !reader.Failed() && reader.Remaining() == 0;
}

} // namespace

Result<void> MakeMaterialTranslationValue(NamePool& names,
                                          const material::MaterialTranslation& translation,
                                          const material::MaterialGeneratedSource& source,
                                          const material::MaterialConstants& constants,
                                          material::MaterialShaderTraits&& traits, Guid rootAsset,
                                          MaterialTranslationValue& out) noexcept {
    if (!translation.succeeded || !source.succeeded) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const material::IrModule& module = translation.module;
    if (auto assigned = out.source.TryAssign(source.text.View()); !assigned) {
        return assigned;
    }

    out.members.Clear();
    out.steps.Clear();
    out.parameters.Clear();
    out.textures.Clear();
    out.locations.Clear();
    out.diagnostics.Clear();
    out.userSceneTextures.Clear();
    if (auto copied = out.members.TryAppend(constants.members.View()); !copied) {
        return copied;
    }

    out.floatVectors = constants.floatVectors;
    if (auto copied = out.steps.TryAppend(constants.steps.View()); !copied) {
        return copied;
    }

    for (const material::IrParameter& parameter : module.Parameters()) {
        material::IrParameter stored = parameter;
        for (double& value : stored.defaults) {
            value = 0.0;
        }

        if (auto pushed = out.parameters.TryPushBack(stored); !pushed) {
            return pushed;
        }
    }

    for (const material::IrTexture& texture : module.Textures()) {
        material::IrTexture stored = texture;
        stored.defaultAsset =
            texture.source == material::IrTextureSource::Asset ? texture.id : Guid{};
        if (auto pushed = out.textures.TryPushBack(stored); !pushed) {
            return pushed;
        }
    }

    out.traits.customOutputs.Clear();
    out.traits = Move(traits);
    if (auto copied = out.locations.TryAppend(source.locations.View()); !copied) {
        return copied;
    }

    out.irValues = static_cast<uint32>(module.Values().Size());
    out.irHash = module.Hash();
    // 번역과 생성 진단(경고·정보)이다. 루트 그래프의 asset은 null로 담는다.
    const nodegraph::GraphDiagnosticList* lists[] = {&translation.diagnostics, &source.diagnostics};
    for (const nodegraph::GraphDiagnosticList* list : lists) {
        for (const nodegraph::GraphDiagnostic& item : list->items) {
            MaterialTranslationDiagnostic diagnostic(out.diagnostics.GetAllocator());
            diagnostic.asset = item.asset == rootAsset ? Guid{} : item.asset;
            diagnostic.graph = item.graph;
            diagnostic.node = item.node;
            diagnostic.severity = item.severity;
            diagnostic.code = item.code;
            if (!item.pin.IsNone()) {
                auto lease = names.Acquire(item.pin);
                if (!lease) {
                    return Result<void>::Failure(lease.Error());
                }

                if (auto assigned = diagnostic.pin.TryAssign(lease.Value().View()); !assigned) {
                    return assigned;
                }
            }

            if (auto assigned = diagnostic.message.TryAssign(item.message.View()); !assigned) {
                return assigned;
            }

            if (auto pushed = out.diagnostics.TryPushBack(Move(diagnostic)); !pushed) {
                return pushed;
            }
        }
    }

    for (Name name : translation.userSceneTextures) {
        auto lease = names.Acquire(name);
        if (!lease) {
            return Result<void>::Failure(lease.Error());
        }

        String text(out.userSceneTextures.GetAllocator());
        if (auto assigned = text.TryAssign(lease.Value().View()); !assigned) {
            return assigned;
        }

        if (auto pushed = out.userSceneTextures.TryPushBack(Move(text)); !pushed) {
            return pushed;
        }
    }

    out.consultedTarget = translation.consultedTarget;
    return Result<void>::Success();
}

Result<void> WriteMaterialTranslationNugget(const MaterialTranslationValue& value,
                                            Array<NuggetValue>& out) noexcept {
    out.Clear();
    for (uint32 index = 0; index < MaterialTranslationValueParts; ++index) {
        NuggetValue part(out.GetAllocator());
        part.id = index;
        if (auto written = WriteParts(value, index, part.bytes); !written) {
            return written;
        }

        if (auto pushed = out.TryPushBack(Move(part)); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

Result<void> ReadMaterialTranslationNugget(Span<const NuggetValue> parts,
                                           MaterialTranslationValue& out) noexcept {
    if (parts.Size() != MaterialTranslationValueParts) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    ByteReader head(parts[0].bytes.View());
    const uint32 version = head.ReadLittleEndian<uint32>();
    const uint32 count = head.ReadLittleEndian<uint32>();
    const uint32 diagnostics = head.ReadLittleEndian<uint32>();
    if (head.Failed() || head.Remaining() != 0 || version != MaterialTranslationValueVersion ||
        count != MaterialTranslationValueParts) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    out.members.Clear();
    out.steps.Clear();
    out.parameters.Clear();
    out.textures.Clear();
    out.locations.Clear();
    out.diagnostics.Clear();
    out.userSceneTextures.Clear();
    for (uint32 index = 1; index < MaterialTranslationValueParts; ++index) {
        if (parts[index].id != index ||
            !ReadPart(parts[index].bytes.View(), index, diagnostics, out)) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }
    }

    return Result<void>::Success();
}

Result<Sha256Digest> MaterialShaderSetId(const MaterialTranslationValue& value) noexcept {
    Array<uint8> layout(value.members.GetAllocator());
    if (auto written = WriteParts(value, 2, layout); !written) {
        return Result<Sha256Digest>::Failure(written.Error());
    }

    Array<uint8> traits(value.members.GetAllocator());
    if (auto written = material::WriteMaterialShaderTraits(value.traits, traits); !written) {
        return Result<Sha256Digest>::Failure(written.Error());
    }

    const Sha256Digest layoutSignature = Sha256::Hash(layout.View());
    Sha256 hasher;
    uint8 version[sizeof(uint32)] = {};
    for (usize at = 0; at < sizeof(uint32); ++at) {
        version[at] = static_cast<uint8>(MaterialShaderSetFormatVersion >> (at * 8));
    }

    hasher.Update(Span<const uint8>(version, sizeof(version)));
    hasher.Update(Span<const uint8>(reinterpret_cast<const uint8*>(value.source.Data()),
                                    value.source.ByteCount()));
    hasher.Update(Span<const uint8>(layoutSignature.bytes, Sha256Digest::Bytes));
    hasher.Update(traits.View());
    return Result<Sha256Digest>::Success(hasher.Finish());
}

uint64 MaterialShaderBundleId(const Sha256Digest& shaderSet) noexcept {
    uint64 bundle = 0;
    for (usize at = 0; at < sizeof(uint64); ++at) {
        bundle |= static_cast<uint64>(shaderSet.bytes[at]) << (at * 8);
    }

    return bundle == 0 ? 1 : bundle;
}

} // namespace rocks
