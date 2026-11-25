#include <ShaderCompilerCommon/ShaderGlesBindings.h>

#include "ShaderCompilerCommonInternal.h"

#include <ShaderCore/ShaderParameterLayout.h>
#include <stdio.h>
#include <string.h>

// GLES 배치와 상수 버퍼 에뮬레이션(Docs/Design/ShaderSystem 16.1절 SSD42)이다. 규칙은 공개 헤더와
// 그 절을 따른다.
namespace rocks::shader {

namespace gles_bindings_detail {

constexpr uint32 SlotBytes = ShaderConstantSlotBytes;
// 상수 버퍼 하나의 최대 슬롯 수다.
constexpr uint32 MaxSlots = MaxShaderConstantBufferBytes / ShaderConstantSlotBytes;
// VS 텍스처 유닛 시작이다(PS·CS는 0 — 7.2절 4항).
constexpr uint32 VertexTextureUnitBase = 16;
// 기본 타입 수(h·i·u)다.
constexpr usize BaseTypes = 3;
constexpr ShaderGlesUniformType TypeOrder[BaseTypes] = {
    ShaderGlesUniformType::Float, ShaderGlesUniformType::Int, ShaderGlesUniformType::Uint};
constexpr const char* VectorTypes[BaseTypes] = {"vec4", "ivec4", "uvec4"};
constexpr char Swizzle[] = "xyzw";
// 문자열 조각 버퍼 크기다.
constexpr usize TextBytes = 256;
// 행렬 열 수(float4x4)다.
constexpr uint32 MatrixColumns = 4;
// ESSL uniform 키워드다.
constexpr StringView UniformKeyword("uniform");

usize TypeIndex(ShaderReflectedBase base) noexcept {
    return base == ShaderReflectedBase::Int ? 1 : (base == ShaderReflectedBase::Uint ? 2 : 0);
}

char StageLetter(uint32 stage) noexcept {
    return stage == ShaderStageVertex ? 'v' : (stage == ShaderStagePixel ? 'p' : 'c');
}

bool Identifier(char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '_';
}

bool Append(String& text, StringView part) noexcept {
    return static_cast<bool>(text.TryAppend(part));
}

// 상수 버퍼의 잎 멤버(스칼라·벡터·행렬·배열 — 1단 구조체는 펼친다)다.
struct Leaf {
    StringView first;
    StringView second;
    const ShaderReflectedMember* member = nullptr;
    uint32 offset = 0;
};

bool Fail(String& diagnostics, StringView message, StringView name = StringView()) noexcept {
    (void)(Append(diagnostics, message) &&
           (name.IsEmpty() ||
            (Append(diagnostics, " '") && Append(diagnostics, name) && Append(diagnostics, "'"))));
    return false;
}

class Planner {
public:
    Planner(const ShaderReflection& reflection, ShaderBindingLayout& layout,
            Span<const ShaderSamplerUse> uses, GlesPlan& out, String& diagnostics) noexcept
        : reflection_(reflection), layout_(layout), uses_(uses), out_(out),
          diagnostics_(diagnostics), arrays_(out.header.GetAllocator()),
          samplers_(out.header.GetAllocator()), copies_(out.header.GetAllocator()) {
    }

    bool Run() noexcept {
        const uint32 stage = layout_.stage;
        const uint32 stageSet = stage == ShaderStagePixel ? 1u : 0u;
        const uint32 unitBase = stage == ShaderStageVertex ? VertexTextureUnitBase : 0u;
        uint32 textures = 0;
        uint32 images = 0;
        uint32 storage = 0;
        uint32 buffers = 0;
        uint32 samplerOrdinal = 0;
        for (ShaderBindingResource& resource : layout_.resources) {
            const uint32 binding = resource.slot;
            switch (resource.kind) {
            case ShaderResourceKind::ConstantBuffer: {
                const ShaderReflectedResource* reflected = FindBuffer(resource.name, false);
                if (reflected == nullptr) {
                    return Fail(diagnostics_, "GLES plan has no reflection for constant buffer",
                                resource.name);
                }

                if (!PlanBuffer(*reflected, static_cast<uint16>(buffers),
                                resource.constantBufferBytes)) {
                    return false;
                }

                resource.slot = static_cast<uint16>(buffers++);
                break;
            }
            case ShaderResourceKind::SampledTexture: {
                const uint32 unit = unitBase + textures;
                if (!PlanTexture(resource, stage, stageSet, binding, unit, textures++)) {
                    return false;
                }

                resource.slot = static_cast<uint16>(unit);
                break;
            }
            case ShaderResourceKind::StorageImage:
                if (!out_.bindings.TryPushBack(GlesResourceBinding{stageSet, binding, images})) {
                    return Fail(diagnostics_, "out of memory");
                }

                resource.slot = static_cast<uint16>(images++);
                break;
            case ShaderResourceKind::StorageBufferRead:
            case ShaderResourceKind::StorageBufferReadWrite:
                if (!out_.bindings.TryPushBack(GlesResourceBinding{stageSet, binding, storage})) {
                    return Fail(diagnostics_, "out of memory");
                }

                resource.slot = static_cast<uint16>(storage++);
                break;
            case ShaderResourceKind::Sampler:
                resource.slot = static_cast<uint16>(samplerOrdinal++);
                break;
            }
        }

        if (layout_.rootConstantBytes > 0) {
            const ShaderReflectedResource* root = FindBuffer(StringView(), true);
            if (root == nullptr) {
                return Fail(diagnostics_, "GLES plan has no root constants reflection");
            }

            if (!PlanBuffer(*root, ShaderGlesRootConstantsBuffer, layout_.rootConstantBytes)) {
                return false;
            }
        }

        SortShaderBindingResources(layout_);
        return EncodeHeader();
    }

private:
    const ShaderReflectedResource* FindBuffer(StringView name, bool root) const noexcept {
        for (const ShaderReflectedResource& resource : reflection_.resources) {
            if (resource.kind == ShaderResourceKind::ConstantBuffer &&
                resource.rootConstants == root && (root || resource.name == name)) {
                return &resource;
            }
        }

        return nullptr;
    }

    bool PlanTexture(const ShaderBindingResource& resource, uint32 stage, uint32 stageSet,
                     uint32 binding, uint32 unit, uint32 ordinal) noexcept {
        const ShaderSamplerUse* use = nullptr;
        for (const ShaderSamplerUse& candidate : uses_) {
            use = candidate.texture == resource.name ? &candidate : use;
        }

        if (use == nullptr && resource.shape != ShaderResourceShape::TypedBuffer) {
            // 샘플러 없이 Load만 쓰는 텍스처: dummy 샘플러와 결합한다(빈 샘플러 이름 —
            // CrossCompileSpirv glslDummySampler). 출처는 None이다.
            auto combined = out_.samplers.TryEmplaceBack(out_.header.GetAllocator());
            char name[TextBytes];
            (void)snprintf(name, sizeof(name), "%cs%u", StageLetter(stage), ordinal);
            if (!combined || !combined.Value()->texture.TryAssign(resource.name) ||
                !combined.Value()->name.TryAssign(StringView(name))) {
                return Fail(diagnostics_, "out of memory");
            }

            combined.Value()->unit = unit;
            out_.dummySampler = true;
            return static_cast<bool>(samplers_.TryPushBack(ShaderGlesSampler{
                       static_cast<uint16>(unit), StringView(), ShaderSamplerSource::None})) ||
                   Fail(diagnostics_, "out of memory");
        }

        if (use == nullptr) {
            // typed buffer(samplerBuffer)는 결합 목록 밖: SPIR-V binding을 유닛으로 바꾼다.
            const bool pushed =
                static_cast<bool>(
                    out_.bindings.TryPushBack(GlesResourceBinding{stageSet, binding, unit})) &&
                static_cast<bool>(samplers_.TryPushBack(ShaderGlesSampler{
                    static_cast<uint16>(unit), resource.name, ShaderSamplerSource::None}));
            return pushed || Fail(diagnostics_, "out of memory");
        }

        auto combined = out_.samplers.TryEmplaceBack(out_.header.GetAllocator());
        char name[TextBytes];
        (void)snprintf(name, sizeof(name), "%cs%u", StageLetter(stage), ordinal);
        if (!combined || !combined.Value()->texture.TryAssign(use->texture) ||
            !combined.Value()->sampler.TryAssign(use->sampler) ||
            !combined.Value()->name.TryAssign(StringView(name))) {
            return Fail(diagnostics_, "out of memory");
        }

        combined.Value()->unit = unit;
        // 이름은 머리를 만들 때 out_.samplers에서 가리킨다(배열이 다 찬 뒤).
        return static_cast<bool>(samplers_.TryPushBack(ShaderGlesSampler{
                   static_cast<uint16>(unit), StringView(), resource.samplerSource})) ||
               Fail(diagnostics_, "out of memory");
    }

    // 잎 멤버를 모은다(1단 구조체 펼침).
    bool CollectLeaves(const ShaderReflectedResource& resource, Array<Leaf>& leaves) noexcept {
        const uint32 last = resource.firstMember + resource.memberCount;
        for (uint32 index = resource.firstMember; index < last; ++index) {
            const ShaderReflectedMember& member = reflection_.members[index];
            if (member.parent == NoShaderReflectedParent) {
                if (member.base != ShaderReflectedBase::Struct &&
                    !leaves.TryPushBack(Leaf{member.name, StringView(), &member, member.offset})) {
                    return false;
                }

                continue;
            }

            const ShaderReflectedMember& parent = reflection_.members[member.parent];
            if (!leaves.TryPushBack(
                    Leaf{parent.name, member.name, &member, parent.offset + member.offset})) {
                return false;
            }
        }

        return true;
    }

    static uint64 LeafBytes(const ShaderReflectedMember& member) noexcept {
        const uint64 element = member.rows == MatrixColumns ? 64 : 4ull * member.columns;
        return member.elements == 0 ? element : element * member.elements;
    }

    bool PlanBuffer(const ShaderReflectedResource& resource, uint16 bufferId,
                    uint32 totalBytes) noexcept {
        Allocator& allocator = out_.header.GetAllocator();
        Array<Leaf> leaves(allocator);
        const uint32 slotCount = totalBytes / SlotBytes;
        if (slotCount == 0 || slotCount > MaxSlots || !CollectLeaves(resource, leaves)) {
            return Fail(diagnostics_, "GLES constant buffer emulation failed for", resource.name);
        }

        // 슬롯마다 기본 타입(5항 — 하나)과 그 타입 배열 안 색인이다.
        Array<uint8> types(allocator);
        Array<uint32> indices(allocator);
        if (!types.TryResize(slotCount) || !indices.TryResize(slotCount)) {
            return Fail(diagnostics_, "out of memory");
        }

        for (const Leaf& leaf : leaves) {
            const uint64 end = leaf.offset + LeafBytes(*leaf.member);
            for (uint64 slot = leaf.offset / SlotBytes; slot < slotCount && slot * SlotBytes < end;
                 ++slot) {
                types[static_cast<usize>(slot)] = static_cast<uint8>(TypeIndex(leaf.member->base));
            }
        }

        uint32 counts[BaseTypes] = {};
        for (uint32 slot = 0; slot < slotCount; ++slot) {
            indices[slot] = counts[types[slot]]++;
        }

        // 배열 표·선언이다(타입 순서 h·i·u — 머리 정렬과 같다).
        auto buffer = out_.buffers.TryEmplaceBack(allocator);
        if (!buffer || !buffer.Value()->name.TryAssign(resource.name)) {
            return Fail(diagnostics_, "out of memory");
        }

        GlesUniformBuffer& plan = *buffer.Value();
        uint32 arrayIndex[BaseTypes] = {};
        char arrayNames[BaseTypes][ShaderGlesArrayNameBytes] = {};
        for (usize type = 0; type < BaseTypes; ++type) {
            if (counts[type] == 0) {
                continue;
            }

            const ShaderGlesUniformArray array{bufferId, TypeOrder[type], counts[type]};
            arrayIndex[type] = static_cast<uint32>(arrays_.Size());
            char line[TextBytes];
            if (!ShaderGlesUniformArrayName(layout_.stage, array, arrayNames[type]) ||
                !arrays_.TryPushBack(array)) {
                return Fail(diagnostics_, "out of memory");
            }

            (void)snprintf(line, sizeof(line), "uniform highp %s %s[%u];\n", VectorTypes[type],
                           arrayNames[type], counts[type]);
            if (!Append(plan.declarations, StringView(line))) {
                return Fail(diagnostics_, "out of memory");
            }
        }

        // 복사 표: 같은 타입 배열로 이어지는 슬롯 구간마다 하나다.
        for (uint32 slot = 0; slot < slotCount;) {
            uint32 run = 1;
            while (slot + run < slotCount && types[slot + run] == types[slot] &&
                   indices[slot + run] == indices[slot] + run) {
                ++run;
            }

            const ShaderGlesCopy copy{bufferId, slot * SlotBytes, run * SlotBytes,
                                      static_cast<uint16>(arrayIndex[types[slot]]), indices[slot]};
            if (!copies_.TryPushBack(copy)) {
                return Fail(diagnostics_, "out of memory");
            }

            slot += run;
        }

        // 멤버 참조 바꿈이다.
        for (const Leaf& leaf : leaves) {
            const ShaderReflectedMember& member = *leaf.member;
            const usize type = TypeIndex(member.base);
            const char* array = arrayNames[type];
            const uint32 index = indices[leaf.offset / SlotBytes];
            const uint32 component = (leaf.offset % SlotBytes) / 4;
            const bool matrix = member.rows == MatrixColumns && member.columns == MatrixColumns;
            auto reference = plan.references.TryEmplaceBack(allocator);
            if (!reference) {
                return Fail(diagnostics_, "out of memory");
            }

            GlesUniformReference& entry = *reference.Value();
            const bool pathOk = entry.path.TryAssign(leaf.first) &&
                                (leaf.second.IsEmpty() ||
                                 (Append(entry.path, ".") && Append(entry.path, leaf.second)));
            char text[TextBytes];
            if (member.elements == 0) {
                if (matrix) {
                    (void)snprintf(text, sizeof(text), "mat4(%s[%u], %s[%u], %s[%u], %s[%u])",
                                   array, index, array, index + 1, array, index + 2, array,
                                   index + 3);
                } else if (component == 0 && member.columns == 4) {
                    (void)snprintf(text, sizeof(text), "%s[%u]", array, index);
                } else {
                    (void)snprintf(text, sizeof(text), "%s[%u].%.*s", array, index,
                                   static_cast<int>(member.columns), Swizzle + component);
                }

                if (!pathOk || !entry.replacement.TryAssign(StringView(text))) {
                    return Fail(diagnostics_, "out of memory");
                }

                continue;
            }

            // 배열 멤버: 원소 접근 함수(이름 = 버퍼_경로, 점은 밑줄).
            String function(allocator);
            if (!pathOk || !function.TryAssign(resource.name) || !Append(function, "_")) {
                return Fail(diagnostics_, "out of memory");
            }

            for (usize at = 0; at < entry.path.ByteCount(); ++at) {
                const char value = entry.path.Data()[at];
                if (!Append(function, StringView(value == '.' ? "_" : StringView(&value, 1)))) {
                    return Fail(diagnostics_, "out of memory");
                }
            }

            if (matrix) {
                (void)snprintf(
                    text, sizeof(text),
                    "highp mat4 %s(int i) { return mat4(%s[%u + 4 * i], %s[%u + 4 * i + 1], "
                    "%s[%u + 4 * i + 2], %s[%u + 4 * i + 3]); }\n",
                    function.CStr(), array, index, array, index, array, index, array, index);
            } else {
                (void)snprintf(text, sizeof(text), "highp %s %s(int i) { return %s[%u + i]; }\n",
                               VectorTypes[type], function.CStr(), array, index);
            }

            entry.indexed = true;
            if (!Append(plan.declarations, StringView(text)) ||
                !entry.replacement.TryAssign(function.View())) {
                return Fail(diagnostics_, "out of memory");
            }
        }

        return true;
    }

    bool EncodeHeader() noexcept {
        ShaderGlesBindingHeader header(out_.header.GetAllocator());
        header.stage = layout_.stage;
        header.attributeMask = layout_.attributeMask;
        usize combined = 0;
        for (ShaderGlesSampler& sampler : samplers_) {
            if (sampler.name.IsEmpty()) {
                sampler.name = out_.samplers[combined++].name.View();
            }
        }

        if (!header.arrays.TryAppend(arrays_.View()) ||
            !header.samplers.TryAppend(samplers_.View()) ||
            !header.copies.TryAppend(copies_.View())) {
            return Fail(diagnostics_, "out of memory");
        }

        if (!EncodeShaderGlesBindingHeader(header, out_.header)) {
            return Fail(diagnostics_,
                        "GLES binding header breaks the value 2 rules (too many arrays, "
                        "samplers or copies)");
        }

        return true;
    }

    const ShaderReflection& reflection_;
    ShaderBindingLayout& layout_;
    Span<const ShaderSamplerUse> uses_;
    GlesPlan& out_;
    String& diagnostics_;
    Array<ShaderGlesUniformArray> arrays_;
    Array<ShaderGlesSampler> samplers_;
    Array<ShaderGlesCopy> copies_;
};

// 식별자 at에서 시작하는 토큰의 끝이다.
usize TokenEnd(StringView text, usize at) noexcept {
    while (at < text.ByteCount() && Identifier(text.Data()[at])) {
        ++at;
    }

    return at;
}

bool StartsToken(StringView text, usize at) noexcept {
    return at < text.ByteCount() && Identifier(text.Data()[at]) &&
           (at == 0 || !Identifier(text.Data()[at - 1]));
}

// `uniform <타입> name;`을 declarations로 바꾼다.
bool ReplaceDeclaration(String& essl, const GlesUniformBuffer& buffer,
                        String& diagnostics) noexcept {
    const StringView text = essl.View();
    for (usize at = 0; at < text.ByteCount(); ++at) {
        if (!StartsToken(text, at) || TokenEnd(text, at) - at != UniformKeyword.ByteCount() ||
            memcmp(text.Data() + at, UniformKeyword.Data(), UniformKeyword.ByteCount()) != 0) {
            continue;
        }

        // uniform 뒤 식별자 둘과 ;
        usize cursor = at + UniformKeyword.ByteCount();
        StringView words[2];
        usize count = 0;
        while (cursor < text.ByteCount() && text.Data()[cursor] != ';' && count <= 2) {
            if (StartsToken(text, cursor)) {
                const usize end = TokenEnd(text, cursor);
                if (count < 2) {
                    words[count] = StringView(text.Data() + cursor, end - cursor);
                }

                ++count;
                cursor = end;
            } else {
                ++cursor;
            }
        }

        if (count != 2 || cursor >= text.ByteCount() || words[1] != buffer.name.View()) {
            continue;
        }

        String rewritten(essl.GetAllocator());
        if (!rewritten.TryAppend(StringView(text.Data(), at)) ||
            !Append(rewritten, buffer.declarations.View()) ||
            !Append(rewritten,
                    StringView(text.Data() + cursor + 1, text.ByteCount() - cursor - 1))) {
            return Fail(diagnostics, "out of memory");
        }

        essl = Move(rewritten);
        return true;
    }

    return Fail(diagnostics, "GLES rewrite did not find the plain uniform declaration of",
                buffer.name.View());
}

class ReferenceRewriter {
public:
    ReferenceRewriter(const GlesPlan& plan, String& diagnostics) noexcept
        : plan_(plan), diagnostics_(diagnostics) {
    }

    bool Rewrite(StringView text, String& out) noexcept {
        usize at = 0;
        while (at < text.ByteCount()) {
            if (!StartsToken(text, at)) {
                if (!Append(out, StringView(text.Data() + at, 1))) {
                    return Fail(diagnostics_, "out of memory");
                }

                ++at;
                continue;
            }

            const usize end = TokenEnd(text, at);
            const StringView token(text.Data() + at, end - at);
            const GlesUniformBuffer* buffer = nullptr;
            for (const GlesUniformBuffer& candidate : plan_.buffers) {
                buffer = candidate.name.View() == token ? &candidate : buffer;
            }

            if (buffer == nullptr) {
                if (!Append(out, token)) {
                    return Fail(diagnostics_, "out of memory");
                }

                at = end;
                continue;
            }

            usize next = 0;
            if (!Replace(text, end, *buffer, out, next)) {
                return false;
            }

            at = next;
        }

        return true;
    }

private:
    // text[end]부터 `.path`(가장 긴 일치)를 찾아 바꾼다.
    bool Replace(StringView text, usize end, const GlesUniformBuffer& buffer, String& out,
                 usize& next) noexcept {
        const GlesUniformReference* best = nullptr;
        if (end < text.ByteCount() && text.Data()[end] == '.') {
            for (const GlesUniformReference& reference : buffer.references) {
                const StringView path = reference.path.View();
                const usize stop = end + 1 + path.ByteCount();
                const bool matches =
                    stop <= text.ByteCount() &&
                    memcmp(text.Data() + end + 1, path.Data(), path.ByteCount()) == 0 &&
                    (stop == text.ByteCount() || !Identifier(text.Data()[stop]));
                if (matches && (best == nullptr || path.ByteCount() > best->path.ByteCount())) {
                    best = &reference;
                }
            }
        }

        if (best == nullptr) {
            return Fail(diagnostics_,
                        "GLES constant buffer emulation needs member access; this shader reads a "
                        "whole buffer or struct (ShaderSystem 7.2 item 5)",
                        buffer.name.View());
        }

        const usize stop = end + 1 + best->path.ByteCount();
        if (!best->indexed) {
            next = stop;
            return Append(out, best->replacement.View()) || Fail(diagnostics_, "out of memory");
        }

        // `[식]`을 찾아 함수 인자로 옮긴다(식 안의 참조도 바꾼다).
        usize open = stop;
        while (open < text.ByteCount() && (text.Data()[open] == ' ' || text.Data()[open] == '\t')) {
            ++open;
        }

        if (open >= text.ByteCount() || text.Data()[open] != '[') {
            return Fail(diagnostics_, "GLES constant buffer array member is read without an index",
                        best->path.View());
        }

        usize depth = 0;
        usize close = open;
        for (; close < text.ByteCount(); ++close) {
            depth += text.Data()[close] == '[' ? 1 : 0;
            if (text.Data()[close] == ']' && --depth == 0) {
                break;
            }
        }

        if (close >= text.ByteCount()) {
            return Fail(diagnostics_, "unbalanced brackets in the ESSL output");
        }

        if (!Append(out, best->replacement.View()) || !Append(out, "(")) {
            return Fail(diagnostics_, "out of memory");
        }

        if (!Rewrite(StringView(text.Data() + open + 1, close - open - 1), out) ||
            !Append(out, ")")) {
            return false;
        }

        next = close + 1;
        return true;
    }

    const GlesPlan& plan_;
    String& diagnostics_;
};

} // namespace gles_bindings_detail

bool BuildGlesPlan(const ShaderReflection& reflection, ShaderBindingLayout& layout,
                   Span<const ShaderSamplerUse> uses, GlesPlan& out, String& diagnostics) noexcept {
    gles_bindings_detail::Planner planner(reflection, layout, uses, out, diagnostics);
    return planner.Run();
}

bool RewriteGlesUniforms(String& essl, const GlesPlan& plan, String& diagnostics) noexcept {
    using namespace gles_bindings_detail;
    for (const GlesUniformBuffer& buffer : plan.buffers) {
        if (!ReplaceDeclaration(essl, buffer, diagnostics)) {
            return false;
        }
    }

    if (plan.buffers.IsEmpty()) {
        return true;
    }

    String rewritten(essl.GetAllocator());
    ReferenceRewriter rewriter(plan, diagnostics);
    if (!rewriter.Rewrite(essl.View(), rewritten)) {
        return false;
    }

    essl = Move(rewritten);
    return true;
}

} // namespace rocks::shader
