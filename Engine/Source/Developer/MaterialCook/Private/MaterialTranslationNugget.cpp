#include <Base/NumericLimits.h>
#include <Core/Encoding/ByteReader.h>
#include <Core/Encoding/ByteWriter.h>
#include <Core/Move.h>
#include <MaterialCompiler/MaterialConstants.h>
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
constexpr uint8 LastScope = static_cast<uint8>(material::ParameterScopeKind::LayerBlend);
// 부호화한 Guid·원소 수의 byte 수다.
constexpr usize GuidBytes = 2 * sizeof(uint64);
constexpr usize CountBytes = sizeof(uint32);

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

// 길이 앞붙인 문자열이다. 할당 실패면 outOfMemory를 켠다.
bool ReadText(ByteReader& reader, String& out, bool& outOfMemory) noexcept {
    const uint32 length = reader.ReadLittleEndian<uint32>();
    const Span<const uint8> bytes = reader.Take(length);
    if (reader.Failed()) {
        return false;
    }

    if (!out.TryAssign(StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size()))) {
        outOfMemory = true;
        return false;
    }

    return true;
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
    // 나누기로 상한을 본다(32-bit usize에서 곱이 넘치지 않게).
    return !reader.Failed() && count <= reader.Remaining() / minimumElementBytes;
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
    if (reader.Failed() || scalar > LastScalar || texture > LastTextureKind ||
        type.components > material::MaxIrComponents || type.rows > material::MaxIrComponents) {
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
    case 13:
        if (auto counted = WriteCount(writer, value.origins.Size()); !counted) {
            return counted;
        }

        for (const material::IrParameterOrigin& origin : value.origins) {
            WriteGuid(writer, origin.identity);
            WriteGuid(writer, origin.owner);
            WriteGuid(writer, origin.parameterId);
            writer.WriteU8(static_cast<uint8>(origin.scope));
            WriteGuid(writer, origin.layer);
            if (auto counted = WriteCount(writer, origin.callPath.Size()); !counted) {
                return counted;
            }

            for (Guid call : origin.callPath) {
                WriteGuid(writer, call);
            }

            if (auto text = WriteText(writer, origin.name.View()); !text) {
                return text;
            }
        }

        break;
    case 14:
        if (auto counted = WriteCount(writer, value.collections.Size()); !counted) {
            return counted;
        }

        for (const material::IrCollection& collection : value.collections) {
            WriteGuid(writer, collection.asset);
            writer.WriteLittleEndian<uint32>(collection.vectors);
        }

        break;
    default:
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return writer.Status();
}

// part index를 해석한다(diagnostics는 part 0의 수만큼). 할당 실패면 outOfMemory를 켜고 false다.
bool ReadPart(Span<const uint8> bytes, uint32 index, uint32 diagnostics,
              MaterialTranslationValue& out, bool& outOfMemory) noexcept {
    ByteReader reader(bytes);
    switch (index) {
    case 1:
        if (!out.source.TryAssign(
                StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size()))) {
            outOfMemory = true;
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
            if (reader.Failed() || kind > LastSlotKind) {
                return false;
            }

            if (!out.members.TryPushBack(member)) {
                outOfMemory = true;
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
                outOfMemory = true;
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
                outOfMemory = true;
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
                outOfMemory = true;
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
            if (reader.Failed()) {
                return false;
            }

            if (!out.locations.TryPushBack(location)) {
                outOfMemory = true;
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
            if (reader.Failed() || severity > LastSeverity ||
                !ReadText(reader, diagnostic.pin, outOfMemory) ||
                !ReadText(reader, diagnostic.message, outOfMemory)) {
                return false;
            }

            diagnostic.severity = static_cast<nodegraph::GraphDiagnosticSeverity>(severity);
            if (!out.diagnostics.TryPushBack(Move(diagnostic))) {
                outOfMemory = true;
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
            if (!ReadText(reader, name, outOfMemory)) {
                return false;
            }

            if (!out.userSceneTextures.TryPushBack(Move(name))) {
                outOfMemory = true;
                return false;
            }
        }

        break;
    }
    case 12:
        out.consultedTarget = reader.ReadLittleEndian<uint32>();
        break;
    case 13: {
        uint32 count = 0;
        // 정체·소유·ParameterId·layer(Guid 넷), scope, 경로 수, 이름 길이.
        constexpr usize OriginBytes = GuidBytes * 4 + 1 + CountBytes + CountBytes;
        if (!ReadCount(reader, OriginBytes, count)) {
            return false;
        }

        for (uint32 at = 0; at < count; ++at) {
            material::IrParameterOrigin origin(out.origins.GetAllocator());
            origin.identity = ReadGuid(reader);
            origin.owner = ReadGuid(reader);
            origin.parameterId = ReadGuid(reader);
            const uint8 scope = reader.ReadU8();
            origin.layer = ReadGuid(reader);
            uint32 calls = 0;
            constexpr usize CallBytes = GuidBytes;
            if (reader.Failed() || scope > LastScope || !ReadCount(reader, CallBytes, calls)) {
                return false;
            }

            origin.scope = static_cast<material::ParameterScopeKind>(scope);
            for (uint32 call = 0; call < calls; ++call) {
                const Guid id = ReadGuid(reader);
                if (reader.Failed()) {
                    return false;
                }

                if (!origin.callPath.TryPushBack(id)) {
                    outOfMemory = true;
                    return false;
                }
            }

            if (!ReadText(reader, origin.name, outOfMemory)) {
                return false;
            }

            if (!out.origins.TryPushBack(Move(origin))) {
                outOfMemory = true;
                return false;
            }
        }

        break;
    }
    case 14: {
        uint32 count = 0;
        constexpr usize CollectionBytes = GuidBytes + sizeof(uint32);
        if (!ReadCount(reader, CollectionBytes, count)) {
            return false;
        }

        for (uint32 at = 0; at < count; ++at) {
            material::IrCollection collection;
            collection.asset = ReadGuid(reader);
            collection.vectors = reader.ReadLittleEndian<uint32>();
            if (reader.Failed()) {
                return false;
            }

            if (!out.collections.TryPushBack(collection)) {
                outOfMemory = true;
                return false;
            }
        }

        break;
    }
    default:
        return false;
    }

    // part는 정확히 다 읽어야 한다(남은 byte는 손상).
    return !reader.Failed() && reader.Remaining() == 0;
}

// 해석한 값의 의미 검사다(12.8절 — 손상된 값이 상수 평가에서 범위 밖을 읽지 않게).
bool GuidBefore(Guid left, Guid right) noexcept {
    return left.high < right.high || (left.high == right.high && left.low < right.low);
}

// 정체 순 출처 표에서 identity가 있는가(이진 탐색).
bool HasOrigin(Span<const material::IrParameterOrigin> origins, Guid identity) noexcept {
    usize low = 0;
    usize high = origins.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (origins[middle].identity == identity) {
            return true;
        }

        if (GuidBefore(origins[middle].identity, identity)) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return false;
}

// 상수 프로그램 명령 하나의 연산·형이 상수 빌더 계약 안인가(artifact 변환의 전제 — 12.8절 MS3d2).
bool ProgramStep(const IrValue& step) noexcept {
    if (step.op == material::IrOp::Texture) {
        return step.type.scalar == material::IrScalar::Texture && step.operandCount == 0;
    }

    const bool leaf = step.op == material::IrOp::Constant || step.op == material::IrOp::Parameter;
    return (leaf || material::IsMaterialConstantProgramOp(step.op)) &&
           (!leaf || step.operandCount == 0) &&
           (step.type.scalar == material::IrScalar::Float ||
            step.type.scalar == material::IrScalar::Double) &&
           step.type.rows <= 1 && step.type.components >= 1;
}

// 해석한 값의 의미 검사다(12.8절 — 손상된 값이 상수 평가·artifact 변환에서 범위 밖을 읽거나 계약
// 위반으로 실패하지 않게, 손상은 다시 번역한다).
bool Consistent(const MaterialTranslationValue& value) noexcept {
    for (usize index = 0; index < value.steps.Size(); ++index) {
        const IrValue& step = value.steps[index];
        if (!ProgramStep(step)) {
            return false;
        }

        // 피연산자는 앞 명령이고, Texture 명령은 TextureSize의 피연산자로만 쓴다.
        for (uint8 operand = 0; operand < step.operandCount; ++operand) {
            if (step.operands[operand] >= index) {
                return false;
            }

            const bool texture = value.steps[step.operands[operand]].op == material::IrOp::Texture;
            if (texture != (step.op == material::IrOp::TextureSize)) {
                return false;
            }
        }

        const uint64 table = step.immediate[material::MaterialConstantTableImmediate];
        if ((step.op == material::IrOp::TextureSize && step.operandCount != 1) ||
            (step.op == material::IrOp::Texture && table >= value.textures.Size()) ||
            (step.op == material::IrOp::Parameter && table >= value.parameters.Size())) {
            return false;
        }
    }

    // 칸의 성분 수는 그 명령과 같고, Double 명령은 tile·offset 칸, Float 명령은 Value 칸이다.
    for (const material::MaterialConstantMember& member : value.members) {
        if (member.step >= value.steps.Size() || member.vector >= value.floatVectors ||
            member.firstComponent + member.components > material::MaxIrComponents) {
            return false;
        }

        const IrValue& step = value.steps[member.step];
        const bool wide = step.type.scalar == material::IrScalar::Double;
        const bool wideSlot = member.kind != material::MaterialConstantSlotKind::Value;
        if (step.op == material::IrOp::Texture || member.components != step.type.components ||
            wide != wideSlot) {
            return false;
        }
    }

    // 출처는 정체 순이고(중복 없음) 수 파라미터·파라미터 텍스처마다 하나 있다(12.8절 MS3d2).
    // scope가 FunctionCall이면 경로가 있고, Global·FunctionCall이면 레이어가 없다.
    for (usize index = 0; index < value.origins.Size(); ++index) {
        const material::IrParameterOrigin& origin = value.origins[index];
        if ((index > 0 && !GuidBefore(value.origins[index - 1].identity, origin.identity)) ||
            (origin.scope == material::ParameterScopeKind::FunctionCall &&
             origin.callPath.IsEmpty()) ||
            ((origin.scope == material::ParameterScopeKind::Global ||
              origin.scope == material::ParameterScopeKind::FunctionCall) &&
             !(origin.layer == Guid{}))) {
            return false;
        }
    }

    for (const material::IrParameter& parameter : value.parameters) {
        if (!HasOrigin(value.origins.View(), parameter.id)) {
            return false;
        }
    }

    for (const material::IrTexture& texture : value.textures) {
        if (texture.source == material::IrTextureSource::Parameter &&
            !HasOrigin(value.origins.View(), texture.id)) {
            return false;
        }
    }

    // 컬렉션은 asset 순(중복 없음)이고 null이 아니다(모듈 표와 같다).
    for (usize index = 0; index < value.collections.Size(); ++index) {
        const Guid asset = value.collections[index].asset;
        if (asset == Guid{} ||
            (index > 0 && !GuidBefore(value.collections[index - 1].asset, asset))) {
            return false;
        }
    }

    return true;
}

// layout 서명 byte다(셰이더가 보는 모양만 — float4 수와 멤버의 칸 종류·float4 번호·첫 성분·성분 수,
// IR 값·명령 번호 제외, 8.6절).
Result<void> WriteLayoutSignature(const MaterialTranslationValue& value,
                                  Array<uint8>& out) noexcept {
    ByteWriter writer(out);
    writer.WriteLittleEndian<uint32>(value.floatVectors);
    if (auto counted = WriteCount(writer, value.members.Size()); !counted) {
        return counted;
    }

    for (const material::MaterialConstantMember& member : value.members) {
        writer.WriteU8(static_cast<uint8>(member.kind));
        writer.WriteLittleEndian<uint32>(member.vector);
        writer.WriteU8(member.firstComponent);
        writer.WriteU8(member.components);
    }

    return writer.Status();
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
    out.origins.Clear();
    out.collections.Clear();
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
    for (const material::IrParameterOrigin& origin : module.ParameterOrigins()) {
        material::IrParameterOrigin copy(out.origins.GetAllocator());
        if (auto copied = material::CopyIrParameterOrigin(origin, copy); !copied) {
            return copied;
        }

        if (auto pushed = out.origins.TryPushBack(Move(copy)); !pushed) {
            return pushed;
        }
    }

    if (auto copied = out.collections.TryAppend(module.Collections()); !copied) {
        return copied;
    }

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

    if (parts[0].id != 0) {
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
    out.origins.Clear();
    out.collections.Clear();
    for (uint32 index = 1; index < MaterialTranslationValueParts; ++index) {
        bool outOfMemory = false;
        if (parts[index].id != index ||
            !ReadPart(parts[index].bytes.View(), index, diagnostics, out, outOfMemory)) {
            return Result<void>::Failure(outOfMemory ? ErrorCode::OutOfMemory
                                                     : ErrorCode::CorruptData);
        }
    }

    return Consistent(out) ? Result<void>::Success()
                           : Result<void>::Failure(ErrorCode::CorruptData);
}

Result<Sha256Digest> MaterialLayoutSignature(const MaterialTranslationValue& value) noexcept {
    Array<uint8> layout(value.members.GetAllocator());
    if (auto written = WriteLayoutSignature(value, layout); !written) {
        return Result<Sha256Digest>::Failure(written.Error());
    }

    return Result<Sha256Digest>::Success(Sha256::Hash(layout.View()));
}

Result<Sha256Digest> MaterialShaderSetId(const MaterialTranslationValue& value) noexcept {
    auto signature = MaterialLayoutSignature(value);
    if (!signature) {
        return signature;
    }

    Array<uint8> head(value.members.GetAllocator());
    {
        // 형식 버전과 생성 원문 길이(앞붙임 — 경계를 모호하지 않게)다.
        ByteWriter writer(head);
        writer.WriteLittleEndian<uint32>(MaterialShaderSetFormatVersion);
        writer.WriteLittleEndian<uint64>(static_cast<uint64>(value.source.ByteCount()));
        if (auto status = writer.Status(); !status) {
            return Result<Sha256Digest>::Failure(status.Error());
        }
    }

    Array<uint8> traits(value.members.GetAllocator());
    if (auto written = material::WriteMaterialShaderTraits(value.traits, traits); !written) {
        return Result<Sha256Digest>::Failure(written.Error());
    }

    const Sha256Digest layoutSignature = signature.Value();
    Sha256 hasher;
    hasher.Update(head.View());
    hasher.Update(Span<const uint8>(reinterpret_cast<const uint8*>(value.source.Data()),
                                    value.source.ByteCount()));
    hasher.Update(Span<const uint8>(layoutSignature.bytes, Sha256Digest::Bytes));
    hasher.Update(traits.View());
    return Result<Sha256Digest>::Success(hasher.Finish());
}

} // namespace rocks
