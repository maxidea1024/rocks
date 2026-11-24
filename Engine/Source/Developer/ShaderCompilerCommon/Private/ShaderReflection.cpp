#include <ShaderCompilerCommon/ShaderReflection.h>

#include <Base/NumericLimits.h>
#include <stdio.h>

// 형식 처리기 공용 바인딩 규칙(Docs/Design/ShaderSystem 16.1절 SSD33)이다. 규칙은 공개 헤더와 그
// 절을 따른다.
namespace rocks::shader {

namespace reflection_rules_detail {

// 상수 버퍼 슬롯 byte 수다(5항).
constexpr uint32 SlotBytes = ShaderConstantSlotBytes;
// 시맨틱 번호 상한(ATTRIBUTE0–15·TEXCOORD0–15, 7항)이다.
constexpr uint32 MaxSemanticIndex = 16;
// 숫자 진단 버퍼 byte 수다.
constexpr usize NumberBytes = 24;
// 짝 샘플러 접미다(9항).
constexpr StringView SamplerSuffix("Sampler");
constexpr StringView SharedWrapSamplerName("SharedWrapSampler");
constexpr StringView SharedClampSamplerName("SharedClampSampler");
constexpr StringView SharedWrapTextureName("SharedWrap");
constexpr StringView SharedClampTextureName("SharedClamp");
// PS 진단 버퍼 이름이다(Platform.rsh, 16.1절 SSD34).
constexpr StringView DebugBufferName("RocksDebugBuffer");

uint64 AlignUp(uint64 value, uint64 alignment) noexcept {
    return (value + alignment - 1) / alignment * alignment;
}

bool EqualsIgnoringCase(StringView left, StringView right) noexcept {
    return EqualsIgnoringAsciiCase(left, right);
}

bool IsSampledTexture(const ShaderReflectedResource& resource) noexcept {
    return resource.kind == ShaderResourceKind::SampledTexture;
}

class Rules {
public:
    Rules(const ShaderFormatDefinition& format, const ShaderReflection& reflection,
          Span<const ShaderParameterLayout> layouts, ShaderBindingLayout& out,
          String& diagnostics) noexcept
        : format_(format), reflection_(reflection), layouts_(layouts), out_(out),
          diagnostics_(diagnostics) {
    }

    bool Run() noexcept {
        out_.stage = reflection_.stage;
        out_.rootConstantBytes = 0;
        out_.attributeMask = 0;
        out_.renderTargetMask = 0;
        out_.interStageMask = 0;
        out_.resources.Clear();
        out_.members.Clear();
        if (reflection_.hasGlobals) {
            return Fail(
                "unnamed global uniforms ($Globals) are not allowed; declare constants in a "
                "named cbuffer, ConstantBuffer<T> or ROCKS_ROOT_CONSTANTS");
        }

        return CheckSignatures() && CheckResources() && CheckSamplers() && CheckLimits() && Emit();
    }

private:
    // 진단을 이어 쓴다(쓰기 실패는 진단만 잘린다).
    void Text(StringView part) noexcept {
        (void)diagnostics_.TryAppend(part);
    }

    void Number(uint64 value) noexcept {
        char text[NumberBytes];
        (void)snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(value));
        Text(StringView(text));
    }

    bool Fail(StringView message) noexcept {
        Text(message);
        return false;
    }

    // "<what> '<name>': <message>"
    bool FailNamed(StringView what, StringView name, StringView message) noexcept {
        Text(what);
        Text(" '");
        Text(name);
        Text("': ");
        return Fail(message);
    }

    // ---------------------------------------------------------------- 시맨틱(7항)

    bool IsSemantic(const ShaderReflectedSignature& element, StringView name) const noexcept {
        return EqualsIgnoringCase(element.semantic, name);
    }

    bool CheckIndexed(const ShaderReflectedSignature& element, uint32& used,
                      StringView what) noexcept {
        if (element.index >= MaxSemanticIndex || (used & (1u << element.index)) != 0) {
            Text(what);
            Text(" ");
            Text(element.semantic);
            Number(element.index);
            return Fail(" is out of range (0-15) or used twice");
        }

        used |= 1u << element.index;
        return true;
    }

    bool RejectSemantic(StringView where, const ShaderReflectedSignature& element) noexcept {
        Text(where);
        Text(" semantic ");
        Text(element.semantic);
        Number(element.index);
        if (IsSemantic(element, "SV_RenderTargetArrayIndex") ||
            IsSemantic(element, "SV_ViewportArrayIndex")) {
            return Fail(
                " needs vertexViewportLayerOutput in both the shader type's required features "
                "and the format's features (ShaderSystem 7.2 item 7, 16.1 SSD35)");
        }

        return Fail(" is not allowed (ShaderSystem 7.2 item 7)");
    }

    // 단계 사이 변수(TEXCOORDn)와 SV_Position이다. interStage는 TEXCOORD 수다.
    // VS layer·viewport 출력이 열렸는가(job 요구 기능과 형식 기능 둘 다 — 16.1절 SSD35).
    bool LayerOutputAllowed() const noexcept {
        return (reflection_.requiredFeatures & ShaderFeatureVertexViewportLayerOutput) != 0 &&
               (format_.features & ShaderFeatureVertexViewportLayerOutput) != 0;
    }

    bool CheckInterStage(Span<const ShaderReflectedSignature> elements, StringView where,
                         bool pixelInput, uint32& interStage) noexcept {
        uint32 used = 0;
        int64 previous = -1;
        for (const ShaderReflectedSignature& element : elements) {
            if (IsSemantic(element, "TEXCOORD")) {
                if (!CheckIndexed(element, used, where)) {
                    return false;
                }

                if (reflection_.signatureOrdered && static_cast<int64>(element.index) < previous) {
                    Text(where);
                    Text(" TEXCOORD");
                    Number(element.index);
                    return Fail(
                        " is declared after a higher TEXCOORD; declare inter-stage variables "
                        "in ascending n order (ShaderSystem 7.2 item 7)");
                }

                previous = element.index;
                ++interStage;
            } else if (!pixelInput && LayerOutputAllowed() &&
                       (IsSemantic(element, "SV_RenderTargetArrayIndex") ||
                        IsSemantic(element, "SV_ViewportArrayIndex"))) {
                continue;
            } else if (!IsSemantic(element, "SV_Position") &&
                       !(pixelInput && IsSemantic(element, "SV_IsFrontFace"))) {
                return RejectSemantic(where, element);
            }
        }

        out_.interStageMask = used;
        return true;
    }

    bool CheckSignatures() noexcept {
        uint32 interStage = 0;
        switch (reflection_.stage) {
        case ShaderStageVertex: {
            uint32 used = 0;
            for (const ShaderReflectedSignature& element : reflection_.inputs) {
                if (IsSemantic(element, "SV_VertexID") || IsSemantic(element, "SV_InstanceID")) {
                    continue;
                }

                if (!IsSemantic(element, "ATTRIBUTE")) {
                    return RejectSemantic("vertex input", element);
                }

                if (!CheckIndexed(element, used, "vertex input")) {
                    return false;
                }
            }

            out_.attributeMask = used;
            if (!CheckInterStage(reflection_.outputs.View(), "vertex output", false, interStage)) {
                return false;
            }

            break;
        }
        case ShaderStagePixel: {
            if (!CheckInterStage(reflection_.inputs.View(), "pixel input", true, interStage)) {
                return false;
            }

            uint32 used = 0;
            for (const ShaderReflectedSignature& element : reflection_.outputs) {
                if (IsSemantic(element, "SV_Depth")) {
                    continue;
                }

                if (!IsSemantic(element, "SV_Target")) {
                    return RejectSemantic("pixel output", element);
                }

                if (element.index >= MaxShaderRenderTargets ||
                    (used & (1u << element.index)) != 0) {
                    Text("pixel output SV_Target");
                    Number(element.index);
                    return Fail(" is out of range (0-7) or written twice");
                }

                used |= 1u << element.index;
            }

            out_.renderTargetMask = used;
            break;
        }
        case ShaderStageCompute:
            for (const ShaderReflectedSignature& element : reflection_.inputs) {
                if (!IsSemantic(element, "SV_DispatchThreadID") &&
                    !IsSemantic(element, "SV_GroupID") &&
                    !IsSemantic(element, "SV_GroupThreadID") &&
                    !IsSemantic(element, "SV_GroupIndex")) {
                    return RejectSemantic("compute input", element);
                }
            }

            if (!reflection_.outputs.IsEmpty()) {
                return RejectSemantic("compute output", reflection_.outputs[0]);
            }

            break;
        default:
            return Fail("shader stage must be exactly one stage bit");
        }

        interStage_ = interStage;
        return true;
    }

    // ---------------------------------------------------------------- 상수 버퍼(5항)

    // 리플렉션 타입의 규칙 byte 수·정렬·표 타입이다. 허용 타입이 아니면 false다.
    static bool Describe(const ShaderReflectedMember& member, uint32& bytes, uint32& alignment,
                         ShaderParameterType& type) noexcept {
        if (member.base != ShaderReflectedBase::Float && member.base != ShaderReflectedBase::Int &&
            member.base != ShaderReflectedBase::Uint) {
            return false;
        }

        if (member.rows == 4 && member.columns == 4 && member.base == ShaderReflectedBase::Float) {
            bytes = 64;
            alignment = SlotBytes;
            type = ShaderParameterType::Float4x4;
            return true;
        }

        if (member.rows != 1 || member.columns < 1 || member.columns > 4) {
            return false;
        }

        bytes = 4 * member.columns;
        alignment = member.columns == 1 ? 4 : (member.columns == 2 ? 8 : SlotBytes);
        const uint32 baseIndex = member.base == ShaderReflectedBase::Float
                                     ? 0
                                     : (member.base == ShaderReflectedBase::Int ? 4 : 8);
        type = static_cast<ShaderParameterType>(baseIndex + member.columns - 1);
        return true;
    }

    // parent 아래 멤버들(한 층)을 규칙으로 검사한다. base는 상수 버퍼 기준 절대 오프셋이다. 끝
    // 오프셋 (상대)을 end에 둔다.
    bool CheckLevel(const ShaderReflectedResource& resource, uint32 parent, uint64 base,
                    bool insideStruct, uint64& end) noexcept {
        end = 0;
        uint64 lastSlot = NumericLimits<uint64>::Max();
        ShaderReflectedBase lastBase = ShaderReflectedBase::Float;
        const uint32 last = resource.firstMember + resource.memberCount;
        for (uint32 index = resource.firstMember; index < last; ++index) {
            const ShaderReflectedMember& member = reflection_.members[index];
            if (member.parent != parent) {
                continue;
            }

            uint64 size = 0;
            uint64 expected = 0;
            if (member.base == ShaderReflectedBase::Struct) {
                if (insideStruct) {
                    return FailNamed("constant buffer member", member.name,
                                     "structs nest only one level inside a constant buffer");
                }

                if (member.elements != 0) {
                    return FailNamed("constant buffer member", member.name,
                                     "arrays of structs are not allowed in a constant buffer");
                }

                expected = AlignUp(end, SlotBytes);
                if (member.offset != expected) {
                    return Misplaced(member, expected);
                }

                uint64 childEnd = 0;
                if (!CheckLevel(resource, index, base + member.offset, true, childEnd)) {
                    return false;
                }

                if (childEnd == 0 || childEnd % SlotBytes != 0) {
                    return FailNamed("constant buffer struct", member.name,
                                     "struct size must be a multiple of 16 bytes made by explicit "
                                     "padding members");
                }

                size = childEnd;
                lastSlot = NumericLimits<uint64>::Max();
            } else {
                uint32 bytes = 0;
                uint32 alignment = 0;
                ShaderParameterType type = ShaderParameterType::Float;
                if (!Describe(member, bytes, alignment, type)) {
                    return FailNamed("constant buffer member", member.name,
                                     "type is not allowed (4-byte scalars, their 2-4 component "
                                     "vectors and float4x4 only; bool, double, half and other "
                                     "matrices are rejected)");
                }

                if (member.elements != 0 && bytes % SlotBytes != 0) {
                    return FailNamed("constant buffer member", member.name,
                                     "arrays need 16-byte elements; pack scalars into float4 "
                                     "arrays");
                }

                size = member.elements == 0 ? bytes : static_cast<uint64>(bytes) * member.elements;
                expected = AlignUp(end, alignment);
                if (member.offset != expected) {
                    return Misplaced(member, expected);
                }

                const uint64 absolute = base + member.offset;
                if (absolute / SlotBytes == lastSlot && member.base != lastBase) {
                    return FailNamed("constant buffer member", member.name,
                                     "a 16-byte slot must not mix float, int and uint members");
                }

                lastSlot = (absolute + size - 1) / SlotBytes;
                lastBase = member.base;
            }

            end = member.offset + size;
        }

        return true;
    }

    bool Misplaced(const ShaderReflectedMember& member, uint64 expected) noexcept {
        Text("constant buffer member '");
        Text(member.name);
        Text("' is at offset ");
        Number(member.offset);
        Text(" but the packing rule gives ");
        Number(expected);
        return Fail(" (add explicit padding members, ShaderSystem 7.2 item 5)");
    }

    // 구조체 하나만 감싼 버퍼(ConstantBuffer<T>·루트 상수 전개)면 그 구조체 색인, 아니면 parent
    // 없음이다.
    uint32 WrapperOf(const ShaderReflectedResource& resource) const noexcept {
        uint32 wrapper = NoShaderReflectedParent;
        uint32 topLevel = 0;
        const uint32 last = resource.firstMember + resource.memberCount;
        for (uint32 index = resource.firstMember; index < last; ++index) {
            const ShaderReflectedMember& member = reflection_.members[index];
            if (member.parent == NoShaderReflectedParent) {
                ++topLevel;
                wrapper = index;
            }
        }

        if (topLevel != 1) {
            return NoShaderReflectedParent;
        }

        const ShaderReflectedMember& member = reflection_.members[wrapper];
        return member.base == ShaderReflectedBase::Struct && member.elements == 0 &&
                       member.offset == 0 && member.name == resource.name
                   ? wrapper
                   : NoShaderReflectedParent;
    }

    const ShaderParameterLayout* FindLayout(StringView name) const noexcept {
        for (const ShaderParameterLayout& layout : layouts_) {
            if (layout.name == name) {
                return &layout;
            }
        }

        return nullptr;
    }

    // 손으로 쓴 표와 대조한다(7.3절 (a) — 이름·타입·오프셋·원소 수·전체 크기).
    bool CompareLayout(const ShaderReflectedResource& resource, uint32 level,
                       const ShaderParameterLayout& layout, uint64 totalBytes) noexcept {
        if (layout.rootConstants != resource.rootConstants) {
            return FailNamed("parameter layout", layout.name,
                             "root constants flag does not match the shader declaration");
        }

        usize member = 0;
        const uint32 last = resource.firstMember + resource.memberCount;
        for (uint32 index = resource.firstMember; index < last; ++index) {
            const ShaderReflectedMember& reflected = reflection_.members[index];
            if (reflected.parent != level) {
                continue;
            }

            uint32 bytes = 0;
            uint32 alignment = 0;
            ShaderParameterType type = ShaderParameterType::Float;
            if (reflected.base == ShaderReflectedBase::Struct ||
                !Describe(reflected, bytes, alignment, type)) {
                return FailNamed("parameter layout", layout.name,
                                 "hand-written layout tables cannot describe struct members yet "
                                 "(ShaderSystem SH3)");
            }

            if (member >= layout.members.Size()) {
                return FailNamed("parameter layout", layout.name,
                                 "the shader declares more members than the C++ layout table");
            }

            const ShaderParameterMember& expected = layout.members[member++];
            if (expected.name != reflected.name || expected.type != type ||
                expected.offset != reflected.offset || expected.arrayCount != reflected.elements) {
                Text("parameter layout '");
                Text(layout.name);
                Text("' member '");
                Text(expected.name);
                Text("' does not match the shader member '");
                Text(reflected.name);
                Text("' at offset ");
                Number(reflected.offset);
                return Fail(" (name, type, offset and array count must be the same)");
            }
        }

        if (member != layout.members.Size() || layout.totalBytes != totalBytes) {
            return FailNamed("parameter layout", layout.name,
                             "member count or total size does not match the shader declaration");
        }

        return true;
    }

    bool CheckConstantBuffer(const ShaderReflectedResource& resource, uint32& bytesOut) noexcept {
        const uint32 wrapper = WrapperOf(resource);
        const uint64 base = 0;
        uint64 end = 0;
        if (!CheckLevel(resource, wrapper, base, false, end)) {
            return false;
        }

        if (end == 0) {
            return FailNamed("constant buffer", resource.name, "constant buffer has no members");
        }

        const uint64 totalBytes = AlignUp(end, SlotBytes);
        if (totalBytes > MaxShaderConstantBufferBytes) {
            return FailNamed("constant buffer", resource.name,
                             "constant buffer is larger than 64 KiB");
        }

        if (const ShaderParameterLayout* layout = FindLayout(resource.name); layout != nullptr) {
            if (!CompareLayout(resource, wrapper, *layout, totalBytes)) {
                return false;
            }
        }

        bytesOut = static_cast<uint32>(totalBytes);
        // 값 1의 멤버(한 층)다.
        const uint32 last = resource.firstMember + resource.memberCount;
        for (uint32 index = resource.firstMember; index < last; ++index) {
            const ShaderReflectedMember& member = reflection_.members[index];
            if (member.parent != wrapper) {
                continue;
            }

            uint32 bytes = member.bytes;
            uint32 alignment = 0;
            ShaderParameterType type = ShaderParameterType::Float;
            if (member.base != ShaderReflectedBase::Struct &&
                Describe(member, bytes, alignment, type)) {
                bytes = member.elements == 0 ? bytes : bytes * member.elements;
            }

            if (!members_.TryPushBack(ShaderBindingMember{member.name, member.offset, bytes})) {
                return Fail("out of memory");
            }
        }

        return true;
    }

    // ---------------------------------------------------------------- 자원

    bool CheckResources() noexcept {
        for (const ShaderReflectedResource& resource : reflection_.resources) {
            if (resource.name.IsEmpty() || resource.name.ByteCount() > MaxShaderBindingNameBytes) {
                return FailNamed("resource", resource.name,
                                 "resource names must be identifiers of at most 64 bytes");
            }

            if (resource.count != 1) {
                return FailNamed("resource", resource.name, "resource arrays are not supported");
            }

            if (resource.slot > NumericLimits<uint16>::Max()) {
                return FailNamed("resource", resource.name, "binding slot is out of range");
            }

            if (resource.debugBuffer || resource.name == DebugBufferName) {
                if (!resource.debugBuffer || resource.name != DebugBufferName ||
                    resource.kind != ShaderResourceKind::StorageBufferReadWrite) {
                    return FailNamed("resource", resource.name,
                                     "the PS diagnostic buffer location and name are reserved "
                                     "(RocksDebugBuffer, ShaderSystem 7.2 item 12)");
                }

                if (reflection_.stage != ShaderStagePixel ||
                    (format_.features & ShaderFeatureDebugPixelUav) == 0) {
                    return FailNamed("resource", resource.name,
                                     "the diagnostic buffer is only for pixel shaders of formats "
                                     "with debugPixelUav");
                }

                debugBuffer_ = true;
            }

            const bool constantBuffer = resource.kind == ShaderResourceKind::ConstantBuffer;
            if (resource.rootConstants) {
                if (!constantBuffer || rootSeen_) {
                    return FailNamed("root constants", resource.name,
                                     "a shader has at most one ROCKS_ROOT_CONSTANTS block");
                }

                rootSeen_ = true;
                const usize membersBefore = members_.Size();
                uint32 bytes = 0;
                if (!CheckConstantBuffer(resource, bytes)) {
                    return false;
                }

                (void)members_.TryResize(membersBefore);
                const uint32 limit = format_.limits.rootConstantBytes < MaxShaderRootConstantBytes
                                         ? format_.limits.rootConstantBytes
                                         : MaxShaderRootConstantBytes;
                if (bytes > limit) {
                    return FailNamed(
                        "root constants", resource.name,
                        "root constants are larger than the format's rootConstantBytes "
                        "limit (at most 128 bytes)");
                }

                out_.rootConstantBytes = bytes;
                continue;
            }

            if (const ShaderParameterLayout* layout = FindLayout(resource.name);
                layout != nullptr && layout->rootConstants) {
                return FailNamed("parameter layout", resource.name,
                                 "the C++ table marks root constants but the shader declares a "
                                 "constant buffer");
            }

            ShaderBindingResource binding;
            binding.kind = resource.kind;
            binding.slot = static_cast<uint16>(resource.slot);
            binding.name = resource.name;
            if (constantBuffer) {
                binding.firstMember = static_cast<uint32>(members_.Size());
                if (!CheckConstantBuffer(resource, binding.constantBufferBytes)) {
                    return false;
                }

                binding.memberCount = static_cast<uint32>(members_.Size()) - binding.firstMember;
            } else if (resource.memberCount != 0) {
                return FailNamed("resource", resource.name, "only constant buffers have members");
            }

            if (!pending_.TryPushBack(binding)) {
                return Fail("out of memory");
            }
        }

        return true;
    }

    // ---------------------------------------------------------------- 샘플러(9항)

    const ShaderBindingResource* FindPending(ShaderResourceKind kind,
                                             StringView name) const noexcept {
        for (const ShaderBindingResource& binding : pending_) {
            if (binding.kind == kind && binding.name == name) {
                return &binding;
            }
        }

        return nullptr;
    }

    bool CheckSamplers() noexcept {
        for (ShaderBindingResource& binding : pending_) {
            if (binding.kind == ShaderResourceKind::SampledTexture) {
                if (binding.name == SharedWrapTextureName ||
                    binding.name == SharedClampTextureName) {
                    return FailNamed("texture", binding.name,
                                     "this name would pair with a reserved shared sampler name");
                }

                char pair[MaxShaderBindingNameBytes + 8];
                const int written = snprintf(pair, sizeof(pair), "%.*s%s",
                                             static_cast<int>(binding.name.ByteCount()),
                                             binding.name.Data(), "Sampler");
                if (written > 0 &&
                    FindPending(ShaderResourceKind::Sampler, StringView(pair)) != nullptr) {
                    binding.samplerSource = ShaderSamplerSource::Own;
                }
            } else if (binding.kind == ShaderResourceKind::Sampler) {
                if (binding.name == SharedWrapSamplerName ||
                    binding.name == SharedClampSamplerName) {
                    binding.sharedSampler = true;
                    continue;
                }

                const StringView name = binding.name;
                const bool suffixed = name.ByteCount() > SamplerSuffix.ByteCount() &&
                                      name.EndsWith(SamplerSuffix, StringComparison::CaseSensitive);
                const StringView texture(name.Data(), name.ByteCount() - SamplerSuffix.ByteCount());
                if (!suffixed ||
                    FindPending(ShaderResourceKind::SampledTexture, texture) == nullptr) {
                    return FailNamed(
                        "sampler", name,
                        "a sampler pairs with the texture whose name it ends with "
                        "(X and XSampler) or is SharedWrapSampler or SharedClampSampler");
                }
            }
        }

        return reflection_.hasSamplerUses ? CheckSamplerUses() : true;
    }

    // SPIR-V 사용 쌍(16.1절 SSD36): 텍스처마다 출처 하나, 예약 이름이 아닌 샘플러는 자기
    // 텍스처로만.
    bool CheckSamplerUses() noexcept {
        for (ShaderBindingResource& texture : pending_) {
            if (texture.kind != ShaderResourceKind::SampledTexture) {
                continue;
            }

            ShaderSamplerSource source = ShaderSamplerSource::None;
            for (const ShaderSamplerUse& use : reflection_.samplerUses) {
                if (use.texture != texture.name) {
                    continue;
                }

                ShaderSamplerSource used = ShaderSamplerSource::Own;
                if (use.sampler == SharedWrapSamplerName) {
                    used = ShaderSamplerSource::SharedWrap;
                } else if (use.sampler == SharedClampSamplerName) {
                    used = ShaderSamplerSource::SharedClamp;
                } else {
                    const bool own =
                        use.sampler.ByteCount() ==
                            texture.name.ByteCount() + SamplerSuffix.ByteCount() &&
                        use.sampler.StartsWith(texture.name, StringComparison::CaseSensitive) &&
                        use.sampler.EndsWith(SamplerSuffix, StringComparison::CaseSensitive);
                    if (!own) {
                        Text("texture '");
                        Text(texture.name);
                        Text("' is sampled with '");
                        Text(use.sampler);
                        return Fail("', which is neither its own XSampler nor a shared sampler "
                                    "(ShaderSystem 7.2 item 9)");
                    }
                }

                if (source != ShaderSamplerSource::None && source != used) {
                    return FailNamed(
                        "texture", texture.name,
                        "a texture is sampled with exactly one sampler source (its own "
                        "sampler, SharedWrapSampler or SharedClampSampler)");
                }

                source = used;
            }

            if (source != ShaderSamplerSource::None) {
                texture.samplerSource = source;
            }
        }

        return true;
    }

    // ---------------------------------------------------------------- 상한(4항)

    bool Over(StringView what, uint64 value, uint32 limit) noexcept {
        Text(what);
        Text(" ");
        Number(value);
        Text(" exceeds the ");
        Text(format_.Id());
        Text(" limit ");
        Number(limit);
        return Fail(" (ShaderSystem 7.2 item 4)");
    }

    bool CheckLimits() noexcept {
        const ShaderFormatLimits& limits = format_.limits;
        uint32 counts[ShaderResourceKindCount] = {};
        // D3D register 공간(b·t·u·s)별 가장 큰 register + 1이다.
        uint32 d3dConstant = 0;
        uint32 d3dShaderResource = 0;
        uint32 d3dUnordered = 0;
        uint32 d3dSampler = 0;
        for (const ShaderBindingResource& binding : pending_) {
            // PS 진단 버퍼는 형식 상한·UAV 단계 밖의 예약 자원이다(16.1절 SSD34).
            if (debugBuffer_ && binding.name == DebugBufferName) {
                continue;
            }

            ++counts[static_cast<uint32>(binding.kind)];
            const uint32 end = static_cast<uint32>(binding.slot) + 1;
            switch (binding.kind) {
            case ShaderResourceKind::ConstantBuffer:
                d3dConstant = end > d3dConstant ? end : d3dConstant;
                break;
            case ShaderResourceKind::SampledTexture:
            case ShaderResourceKind::StorageBufferRead:
                d3dShaderResource = end > d3dShaderResource ? end : d3dShaderResource;
                break;
            case ShaderResourceKind::StorageBufferReadWrite:
            case ShaderResourceKind::StorageImage:
                d3dUnordered = end > d3dUnordered ? end : d3dUnordered;
                break;
            case ShaderResourceKind::Sampler:
                d3dSampler = end > d3dSampler ? end : d3dSampler;
                break;
            }
        }

        const auto count = [&](ShaderResourceKind kind) noexcept {
            return counts[static_cast<uint32>(kind)];
        };
        const uint32 storage = count(ShaderResourceKind::StorageBufferRead) +
                               count(ShaderResourceKind::StorageBufferReadWrite);
        const uint32 unordered = count(ShaderResourceKind::StorageBufferReadWrite) +
                                 count(ShaderResourceKind::StorageImage);
        if (count(ShaderResourceKind::ConstantBuffer) > limits.constantBuffers) {
            return Over("constant buffer count", count(ShaderResourceKind::ConstantBuffer),
                        limits.constantBuffers);
        }

        if (count(ShaderResourceKind::SampledTexture) > limits.sampledTextures) {
            return Over("sampled texture count", count(ShaderResourceKind::SampledTexture),
                        limits.sampledTextures);
        }

        if (storage > limits.storageBuffers) {
            return Over("structured and raw buffer count", storage, limits.storageBuffers);
        }

        if (count(ShaderResourceKind::StorageImage) > limits.storageImages) {
            return Over("storage image count", count(ShaderResourceKind::StorageImage),
                        limits.storageImages);
        }

        if (count(ShaderResourceKind::Sampler) > limits.samplers) {
            return Over("sampler count", count(ShaderResourceKind::Sampler), limits.samplers);
        }

        if (unordered > 0 && (reflection_.stage & limits.uavStages) == 0) {
            return Fail("unordered access (RW buffers and storage images) is only allowed in the "
                        "format's uavStages (ShaderSystem 7.2 item 4)");
        }

        if (limits.uavTotal != NoShaderLimit && unordered > limits.uavTotal) {
            return Over("unordered access view count", unordered, limits.uavTotal);
        }

        uint32 renderTargets = 0;
        for (uint32 index = 0; index < MaxShaderRenderTargets; ++index) {
            renderTargets =
                (out_.renderTargetMask & (1u << index)) != 0 ? index + 1 : renderTargets;
        }

        if (renderTargets > limits.renderTargets) {
            return Over("render target count", renderTargets, limits.renderTargets);
        }

        // 상수 버퍼 에뮬레이션 형식의 단계 vec4 합(4항 모바일 상한, 16.1절 SSD42)이다.
        const usize stageIndex = reflection_.stage == ShaderStageVertex
                                     ? 0
                                     : (reflection_.stage == ShaderStagePixel ? 1 : 2);
        if (limits.uniformVectors[stageIndex] != NoShaderLimit) {
            uint64 vectors = out_.rootConstantBytes / SlotBytes;
            for (const ShaderBindingResource& binding : pending_) {
                vectors += binding.kind == ShaderResourceKind::ConstantBuffer
                               ? binding.constantBufferBytes / SlotBytes
                               : 0;
            }

            if (vectors > limits.uniformVectors[stageIndex]) {
                return Over("emulated uniform vector count", vectors,
                            limits.uniformVectors[stageIndex]);
            }
        }

        // ES 3.2의 VS·PS storage block 최소 보장은 0이다(4항 모바일 상한).
        if (format_.backend == ShaderBackend::OpenGL && reflection_.stage != ShaderStageCompute &&
            storage > 0) {
            return Fail("OpenGL ES vertex and pixel shaders cannot use structured or raw buffers "
                        "(ShaderSystem 7.2 item 4)");
        }

        if (interStage_ > limits.interStageVectors) {
            return Over("inter-stage vector count", interStage_, limits.interStageVectors);
        }

        // D3D register 번호도 그 register 공간의 상한 안이어야 한다(SSD33).
        if (format_.backend == ShaderBackend::D3D12 || format_.backend == ShaderBackend::D3D11) {
            if (d3dConstant > limits.constantBuffers) {
                return Over("constant buffer register end", d3dConstant, limits.constantBuffers);
            }

            if (d3dShaderResource > limits.sampledTextures + limits.storageBuffers) {
                return Over("shader resource register end", d3dShaderResource,
                            limits.sampledTextures + limits.storageBuffers);
            }

            if (d3dUnordered > limits.storageBuffers + limits.storageImages) {
                return Over("unordered access register end", d3dUnordered,
                            limits.storageBuffers + limits.storageImages);
            }

            if (d3dSampler > limits.samplers) {
                return Over("sampler register end", d3dSampler, limits.samplers);
            }
        }

        return true;
    }

    // ---------------------------------------------------------------- 출력

    bool Emit() noexcept {
        pending_.Sort([](const ShaderBindingResource& a, const ShaderBindingResource& b) noexcept {
            return a.kind < b.kind || (a.kind == b.kind && a.slot < b.slot);
        });
        for (usize index = 0; index < pending_.Size(); ++index) {
            const ShaderBindingResource& binding = pending_[index];
            if (index > 0 && pending_[index - 1].kind == binding.kind &&
                pending_[index - 1].slot == binding.slot) {
                Text("binding collision: '");
                Text(pending_[index - 1].name);
                Text("' and '");
                Text(binding.name);
                return Fail("' use the same slot");
            }

            ShaderBindingResource copy = binding;
            copy.firstMember = static_cast<uint32>(out_.members.Size());
            for (uint32 member = 0; member < binding.memberCount; ++member) {
                if (!out_.members.TryPushBack(members_[binding.firstMember + member])) {
                    return Fail("out of memory");
                }
            }

            if (!out_.resources.TryPushBack(copy)) {
                return Fail("out of memory");
            }
        }

        return true;
    }

    const ShaderFormatDefinition& format_;
    const ShaderReflection& reflection_;
    Span<const ShaderParameterLayout> layouts_;
    ShaderBindingLayout& out_;
    String& diagnostics_;
    Array<ShaderBindingResource> pending_{out_.resources.GetAllocator()};
    Array<ShaderBindingMember> members_{out_.resources.GetAllocator()};
    uint32 interStage_ = 0;
    bool rootSeen_ = false;
    bool debugBuffer_ = false;
};

} // namespace reflection_rules_detail

ShaderReflectedSignature SplitShaderSemantic(StringView semantic) noexcept {
    usize digits = semantic.ByteCount();
    while (digits > 0 && semantic.Data()[digits - 1] >= '0' && semantic.Data()[digits - 1] <= '9') {
        --digits;
    }

    ShaderReflectedSignature element;
    element.semantic = StringView(semantic.Data(), digits);
    uint64 index = 0;
    for (usize at = digits; at < semantic.ByteCount(); ++at) {
        index = index * 10 + static_cast<uint64>(semantic.Data()[at] - '0');
        if (index > NumericLimits<uint32>::Max()) {
            index = NumericLimits<uint32>::Max();
            break;
        }
    }

    element.index = static_cast<uint32>(index);
    return element;
}

bool NormalizeShaderReflection(const ShaderFormatDefinition& format,
                               const ShaderReflection& reflection,
                               Span<const ShaderParameterLayout> layouts, ShaderBindingLayout& out,
                               String& diagnostics) noexcept {
    reflection_rules_detail::Rules rules(format, reflection, layouts, out, diagnostics);
    if (!rules.Run()) {
        return false;
    }

    // 값 1 규칙(ShaderCore)도 통과해야 한다(이름 철자 등).
    Array<uint8> probe(out.resources.GetAllocator());
    if (!EncodeShaderBindingLayout(out, probe)) {
        (void)diagnostics.TryAppend("binding layout breaks the value 1 rules (resource or member "
                                    "names must be identifiers of at most 64 bytes)");
        return false;
    }

    return true;
}

} // namespace rocks::shader
