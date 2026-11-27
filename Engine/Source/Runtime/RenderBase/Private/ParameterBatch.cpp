#include <Base/Compiler.h>
#include <Graphics/ShaderBindings.h>
#include <RenderBase/ParameterBatch.h>
#include <RenderBase/ResourceParameters/DeviceBufferParameter.h>
#include <RenderBase/ResourceParameters/DeviceTextureParameter.h>

// 묶음 설정의 자원 부분(Docs/Design/RenderBase 9.13절 6항)이다. 규칙은 공개 헤더 주석을 따른다.
namespace rocks::render {

namespace parameter_batch_detail {

using shader::ShaderResourceShape;

// 자기 샘플러 이름의 접미사다(ShaderSystem 7.2절 9항 짝 규칙).
constexpr char SamplerSuffix[] = "Sampler";

Result<void> Fail(const char** reason, const char* text,
                  ErrorCode code = ErrorCode::InvalidArgument) noexcept {
    if (reason != nullptr) {
        *reason = text;
    }

    return Result<void>::Failure(code);
}

// 텍스처 형식의 값 계열이 셰이더 원소 타입을 받는가(None은 대조하지 않는다).
bool ComponentFits(shader::ShaderComponentType component, graphics::PixelFormat format) noexcept {
    const graphics::PixelShaderType wanted = graphics::PixelShaderTypeOf(component);
    return wanted == graphics::PixelShaderType::None ||
           graphics::GetPixelFormatInfo(format).shaderType == wanted;
}

// 버퍼 view 요약이 셰이더 자리의 모양을 받는가.
bool BufferViewFits(const ResourcePlacement& placement,
                    const graphics::BufferViewInfo& view) noexcept {
    switch (placement.shape) {
    case ShaderResourceShape::TypedBuffer:
        return view.kind == graphics::BufferViewKind::Typed &&
               view.shaderType == graphics::PixelShaderTypeOf(placement.component);
    case ShaderResourceShape::StructuredBuffer:
        return view.kind == graphics::BufferViewKind::Structured &&
               (placement.structureStride == 0 || view.stride == placement.structureStride);
    case ShaderResourceShape::RawBuffer:
        return view.kind == graphics::BufferViewKind::Raw;
    default:
        break;
    }

    return false;
}

} // namespace parameter_batch_detail

using namespace parameter_batch_detail;

ParameterBatch::ParameterBatch(Allocator& allocator, const ShaderResourceMap& map) noexcept
    : map_(&map), values_(allocator) {
}

Result<void> ParameterBatch::Initialize() noexcept {
    values_.Clear();
    if (!values_.TryResize(map_->Table().Size())) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    for (Value& value : values_) {
        value = Value{};
    }

    ++version_;
    return Result<void>::Success();
}

Result<usize> ParameterBatch::Entry(StringView name, ResourceParameterKind kind) const noexcept {
    if (values_.Size() != map_->Table().Size()) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    auto found = map_->Find(name);
    if (!found) {
        return found;
    }

    if (map_->Table()[found.Value()].kind != kind) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    return found;
}

Result<void> ParameterBatch::SetTexture(StringView name, graphics::TextureHandle texture) noexcept {
    auto entry = Entry(name, ResourceParameterKind::Texture);
    if (!entry) {
        return Result<void>::Failure(entry.Error());
    }

    values_[entry.Value()].given = true;
    values_[entry.Value()].handle = texture.slot;
    ++version_;
    return Result<void>::Success();
}

Result<void> ParameterBatch::SetSampler(StringView name,
                                        const graphics::SamplerDesc& sampler) noexcept {
    auto entry = Entry(name, ResourceParameterKind::Sampler);
    if (!entry) {
        return Result<void>::Failure(entry.Error());
    }

    if (!graphics::IsValidSamplerDesc(sampler)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    values_[entry.Value()].given = true;
    values_[entry.Value()].sampler = sampler;
    ++version_;
    return Result<void>::Success();
}

Result<void> ParameterBatch::ResetSampler(StringView name) noexcept {
    auto entry = Entry(name, ResourceParameterKind::Sampler);
    if (!entry) {
        return Result<void>::Failure(entry.Error());
    }

    values_[entry.Value()] = Value{};
    ++version_;
    return Result<void>::Success();
}

Result<void> ParameterBatch::SetBufferView(StringView name,
                                           graphics::BufferViewHandle view) noexcept {
    auto entry = Entry(name, ResourceParameterKind::BufferView);
    if (!entry) {
        return Result<void>::Failure(entry.Error());
    }

    values_[entry.Value()].given = true;
    values_[entry.Value()].handle = view.slot;
    ++version_;
    return Result<void>::Success();
}

bool ParameterBatch::Skips(usize parameter) const noexcept {
    const ResourceParameter& entry = map_->Table()[parameter];
    return map_->PlacementsOf(parameter).IsEmpty() ||
           (entry.HasMember() && shader::IsGraphResource(entry.memberKind)) ||
           entry.kind == ResourceParameterKind::WritableTexture ||
           entry.kind == ResourceParameterKind::WritableBuffer;
}

Result<void> ParameterBatch::SetParameters(const void* values,
                                           const shader::ShaderParameterLayout& layout) noexcept {
    const Span<const ResourceParameter> table = map_->Table();
    if (values_.Size() != table.Size() || values == nullptr || map_->Layout() == nullptr ||
        map_->Layout()->structId != layout.structId) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const uint8* bytes = static_cast<const uint8*>(values);
    for (usize index = 0; index < table.Size(); ++index) {
        const ResourceParameter& entry = table[index];
        if (!entry.HasMember()) {
            continue;
        }

        Value& value = values_[index];
        if (entry.memberKind == shader::ShaderParameterResourceKind::DeviceTexture) {
            const auto& member =
                *reinterpret_cast<const DeviceTextureValue*>(bytes + entry.memberOffset);
            if (entry.kind == ResourceParameterKind::Sampler) {
                if (member.sampler.given && !graphics::IsValidSamplerDesc(member.sampler.desc)) {
                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }

                value = Value{};
                value.given = member.sampler.given;
                value.sampler = member.sampler.desc;
            } else {
                value.given = true;
                value.handle = member.texture.slot;
            }
        } else if (entry.memberKind == shader::ShaderParameterResourceKind::DeviceBuffer) {
            const auto& member =
                *reinterpret_cast<const DeviceBufferValue*>(bytes + entry.memberOffset);
            value.given = true;
            value.handle = member.view.slot;
        }
    }

    ++version_;
    return Result<void>::Success();
}

Result<void> ParameterBatch::Validate(graphics::Device& device, const char** reason,
                                      usize* failedParameter) noexcept {
    const Span<const ResourceParameter> table = map_->Table();
    if (values_.Size() != table.Size()) {
        return Fail(reason, "the parameter batch is not initialized");
    }

    for (usize index = 0; index < table.Size(); ++index) {
        if (failedParameter != nullptr) {
            *failedParameter = index;
        }

        if (Skips(index)) {
            continue;
        }

        const Value& value = values_[index];
        const Span<const ResourcePlacement> placements = map_->PlacementsOf(index);
        switch (table[index].kind) {
        case ResourceParameterKind::WritableTexture:
        case ResourceParameterKind::WritableBuffer:
            break;
        case ResourceParameterKind::Texture: {
            if (!value.given || value.handle.IsNull()) {
                return Fail(reason, "a texture of the parameter batch has no value or is null");
            }

            auto info = device.GetTextureViewInfo(graphics::TextureHandle{value.handle});
            if (!info) {
                return Fail(reason, "a texture of the parameter batch is unknown to the device",
                            info.Error());
            }

            if (!info.Value().sampled || info.Value().access != graphics::ViewAccess::Read) {
                return Fail(reason, "a texture of the parameter batch has no shader resource view "
                                    "or is a read-write view");
            }

            for (const ResourcePlacement& placement : placements) {
                if (!graphics::TextureShapeAccepts(placement.shape, info.Value().dimension,
                                                   info.Value().sampleCount)) {
                    return Fail(reason, "a texture of the parameter batch differs from the "
                                        "shader in dimension or sample count");
                }

                if (!ComponentFits(placement.component, info.Value().format)) {
                    return Fail(reason, "a texture format of the parameter batch reads another "
                                        "value type (float, int, uint) than the shader");
                }
            }

            break;
        }
        case ResourceParameterKind::Sampler: {
            const bool shared = !placements.IsEmpty() && placements[0].sharedSampler;
            if (shared && !value.given) {
                return Fail(reason, "a shared sampler of the parameter batch has no value");
            }

            // 자기 샘플러 값의 비교 여부는 짝 텍스처 desc와 같아야 한다(셰이더 선언과 같은 종류).
            const StringView name = table[index].name;
            constexpr usize SuffixBytes = sizeof(SamplerSuffix) - 1;
            if (shared || !value.given || name.ByteCount() <= SuffixBytes) {
                break;
            }

            auto texture = map_->Find(StringView(name.Data(), name.ByteCount() - SuffixBytes));
            if (!texture || !values_[texture.Value()].given) {
                break;
            }

            auto info =
                device.GetTextureViewInfo(graphics::TextureHandle{values_[texture.Value()].handle});
            if (info && info.Value().comparison != value.sampler.comparison) {
                return Fail(reason, "a sampler value of the parameter batch differs from its "
                                    "texture in depth comparison");
            }

            break;
        }
        case ResourceParameterKind::BufferView: {
            if (!value.given || value.handle.IsNull()) {
                return Fail(reason, "a buffer view of the parameter batch has no value or is null");
            }

            auto info = device.GetBufferViewInfo(graphics::BufferViewHandle{value.handle});
            if (!info) {
                return Fail(reason, "a buffer view of the parameter batch is unknown to the device",
                            info.Error());
            }

            if (info.Value().access != graphics::ViewAccess::Read) {
                return Fail(reason, "a buffer view of the parameter batch is a read-write view");
            }

            for (const ResourcePlacement& placement : placements) {
                if (!BufferViewFits(placement, info.Value())) {
                    return Fail(reason, "a buffer view of the parameter batch differs from the "
                                        "shader in kind, value type or structure stride");
                }
            }

            break;
        }
        }
    }

    validatedVersion_ = version_;
    return Result<void>::Success();
}

Result<void> ParameterBatch::Apply(graphics::CommandContext& commands,
                                   const char** reason) const noexcept {
    const Span<const ResourceParameter> table = map_->Table();
    if (values_.Size() != table.Size()) {
        return Fail(reason, "the parameter batch is not initialized");
    }

    if constexpr (!IsReleaseBuild) {
        if (!IsValidated()) {
            return Fail(reason, "the parameter batch values changed after the last Validate");
        }
    }

    // null·값 없음은 모든 구성에서 막는다(장치 질의 없이 알 수 있다).
    for (usize index = 0; index < table.Size(); ++index) {
        if (Skips(index)) {
            continue;
        }

        const Value& value = values_[index];
        const Span<const ResourcePlacement> placements = map_->PlacementsOf(index);
        const bool shared = !placements.IsEmpty() && placements[0].sharedSampler;
        const bool needsValue = table[index].kind != ResourceParameterKind::Sampler || shared;
        if (needsValue && (!value.given || (table[index].kind != ResourceParameterKind::Sampler &&
                                            value.handle.IsNull()))) {
            return Fail(reason, "a texture, buffer view or shared sampler of the parameter batch "
                                "has no value or is null");
        }
    }

    for (usize index = 0; index < table.Size(); ++index) {
        if (Skips(index)) {
            continue;
        }

        const Value& value = values_[index];
        for (const ResourcePlacement& placement : map_->PlacementsOf(index)) {
            switch (table[index].kind) {
            case ResourceParameterKind::WritableTexture:
            case ResourceParameterKind::WritableBuffer:
                break;
            case ResourceParameterKind::Texture:
                commands.SetTexture(placement.stage, placement.slot,
                                    graphics::TextureHandle{value.handle});
                break;
            case ResourceParameterKind::Sampler: {
                auto set = commands.SetSampler(placement.stage, placement.slot,
                                               value.given ? &value.sampler : nullptr);
                if (!set) {
                    return Fail(reason, "the device rejected a sampler of the parameter batch",
                                set.Error());
                }

                break;
            }
            case ResourceParameterKind::BufferView:
                commands.SetBufferView(placement.slot, graphics::BufferViewHandle{value.handle});
                break;
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::render
