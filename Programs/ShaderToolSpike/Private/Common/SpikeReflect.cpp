#include "SpikeTools.h"

#include <spirv_reflect.h>

// SPIRV-Reflect(Engine ThirdParty C 원본) 리플렉션 덤프다. 7.2절 3항의 정규화
// 입력(set·binding·종류· 상수 블록 오프셋·단계 사이 location)이 DXC 출력에서 어떻게 나오는지 보는
// 규약 검토 자료다.
namespace rocks::spike {

namespace {

const char* DescriptorTypeName(SpvReflectDescriptorType type) noexcept {
    switch (type) {
    case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
        return "sampler";
    case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        return "combined-image-sampler";
    case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        return "sampled-image";
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        return "storage-image";
    case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        return "uniform-texel-buffer";
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        return "storage-texel-buffer";
    case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        return "uniform-buffer";
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        return "storage-buffer";
    default:
        return "other";
    }
}

const char* Text(const char* value) noexcept {
    return value != nullptr ? value : "";
}

// 모듈을 열고 닫는 범위 객체다. 실패하면 IsOpen이 false다.
class ReflectModule final {
public:
    explicit ReflectModule(Span<const uint8> code) noexcept {
        open_ = spvReflectCreateShaderModule(code.Size(), code.Data(), &module_) ==
                SPV_REFLECT_RESULT_SUCCESS;
    }

    ReflectModule(const ReflectModule&) = delete;
    ReflectModule& operator=(const ReflectModule&) = delete;

    ~ReflectModule() noexcept {
        if (open_) {
            spvReflectDestroyShaderModule(&module_);
        }
    }

    bool IsOpen() const noexcept {
        return open_;
    }

    SpvReflectShaderModule* Get() noexcept {
        return &module_;
    }

private:
    SpvReflectShaderModule module_ = {};
    bool open_ = false;
};

void WriteBlock(const SpvReflectBlockVariable& block, Report& report) noexcept {
    report.UInt("size", block.size);
    report.UInt("paddedSize", block.padded_size);
    report.BeginArray("members");
    for (uint32 index = 0; index < block.member_count; ++index) {
        const SpvReflectBlockVariable& member = block.members[index];
        report.BeginObject();
        report.String("name", StringView(Text(member.name)));
        report.UInt("offset", member.offset);
        report.UInt("size", member.size);
        report.EndObject();
    }

    report.EndArray();
}

using EnumerateVariables = SpvReflectResult (*)(const SpvReflectShaderModule*, uint32_t*,
                                                SpvReflectInterfaceVariable**);

bool WriteVariables(SpvReflectShaderModule* module, EnumerateVariables enumerate, StringView key,
                    Report& report) noexcept {
    uint32 count = 0;
    if (enumerate(module, &count, nullptr) != SPV_REFLECT_RESULT_SUCCESS) {
        return false;
    }

    Array<SpvReflectInterfaceVariable*> variables(SpikeAllocator());
    if (!variables.TryResize(count) ||
        enumerate(module, &count, variables.Data()) != SPV_REFLECT_RESULT_SUCCESS) {
        return false;
    }

    report.BeginArray(key);
    for (const SpvReflectInterfaceVariable* variable : variables) {
        report.BeginObject();
        report.String("name", StringView(Text(variable->name)));
        const bool builtIn = (variable->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) != 0;
        if (builtIn) {
            report.UInt("builtIn", static_cast<uint64>(variable->built_in));
        } else {
            report.UInt("location", variable->location);
        }

        // DXC는 -fspv-reflect 없이는 시맨틱 decoration을 내지 않는다. 시맨틱은 변수 이름
        // (in.var.TEXCOORD0)에 남는다.
        report.String("semantic", StringView(Text(variable->semantic)));
        report.UInt("format", static_cast<uint64>(variable->format));
        report.EndObject();
    }

    report.EndArray();
    return true;
}

} // namespace

bool DumpSpirvReflection(Span<const uint8> code, Report& report) noexcept {
    ReflectModule module(code);
    if (!module.IsOpen()) {
        return false;
    }

    SpvReflectShaderModule* reflected = module.Get();
    report.String("entryPoint", StringView(Text(reflected->entry_point_name)));
    report.UInt("stage", static_cast<uint64>(reflected->shader_stage));

    uint32 count = 0;
    if (spvReflectEnumerateDescriptorBindings(reflected, &count, nullptr) !=
        SPV_REFLECT_RESULT_SUCCESS) {
        return false;
    }

    Array<SpvReflectDescriptorBinding*> bindings(SpikeAllocator());
    if (!bindings.TryResize(count) ||
        spvReflectEnumerateDescriptorBindings(reflected, &count, bindings.Data()) !=
            SPV_REFLECT_RESULT_SUCCESS) {
        return false;
    }

    report.BeginArray("bindings");
    for (const SpvReflectDescriptorBinding* binding : bindings) {
        report.BeginObject();
        report.String("name", StringView(Text(binding->name)));
        report.UInt("set", binding->set);
        report.UInt("binding", binding->binding);
        report.String("type", StringView(DescriptorTypeName(binding->descriptor_type)));
        report.UInt("count", binding->count);
        if (binding->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            report.UInt("imageFormat", static_cast<uint64>(binding->image.image_format));
        }

        if (binding->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            report.BeginObject("block");
            WriteBlock(binding->block, report);
            report.EndObject();
        }

        report.EndObject();
    }

    report.EndArray();

    if (spvReflectEnumeratePushConstantBlocks(reflected, &count, nullptr) !=
        SPV_REFLECT_RESULT_SUCCESS) {
        return false;
    }

    Array<SpvReflectBlockVariable*> pushConstants(SpikeAllocator());
    if (!pushConstants.TryResize(count) ||
        spvReflectEnumeratePushConstantBlocks(reflected, &count, pushConstants.Data()) !=
            SPV_REFLECT_RESULT_SUCCESS) {
        return false;
    }

    report.BeginArray("pushConstants");
    for (const SpvReflectBlockVariable* block : pushConstants) {
        report.BeginObject();
        report.String("name", StringView(Text(block->name)));
        WriteBlock(*block, report);
        report.EndObject();
    }

    report.EndArray();
    return WriteVariables(reflected, spvReflectEnumerateInputVariables, "inputs", report) &&
           WriteVariables(reflected, spvReflectEnumerateOutputVariables, "outputs", report);
}

bool ReadStorageImageFormat(Span<const uint8> code, uint32& format) noexcept {
    format = 0;
    ReflectModule module(code);
    if (!module.IsOpen()) {
        return false;
    }

    uint32 count = 0;
    if (spvReflectEnumerateDescriptorBindings(module.Get(), &count, nullptr) !=
        SPV_REFLECT_RESULT_SUCCESS) {
        return false;
    }

    Array<SpvReflectDescriptorBinding*> bindings(SpikeAllocator());
    if (!bindings.TryResize(count) ||
        spvReflectEnumerateDescriptorBindings(module.Get(), &count, bindings.Data()) !=
            SPV_REFLECT_RESULT_SUCCESS) {
        return false;
    }

    for (const SpvReflectDescriptorBinding* binding : bindings) {
        if (binding->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            format = static_cast<uint32>(binding->image.image_format);
            return true;
        }
    }

    return false;
}

} // namespace rocks::spike
