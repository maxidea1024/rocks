#include <Core/Text/String.h>
#include <CoreTypeBindings/CoreValueTypes.generated.h>
#include <CoreTypeBindings/CoreValueTypes.h>
#include <MaterialSchema/Nodes/Functions/FunctionsNodeEnums.h>

#include "LoweringContext.h"
#include "LoweringTable.h"

// 함수 노드 lowering이다(Docs/Design/MaterialSystem 6.12절 MS2e2). 호출 노드·FunctionOutput은
// 번역하지 않는다(펼친 그래프에서 간선이 호출 출력 → FunctionOutput 사본 입력으로 넘어간다 —
// Translator::LowerCallOutput). FunctionInput 사본은 호출 노드 입력을 읽는다.
namespace rocks::material::detail {

namespace {

// FunctionInput의 Preview 입력·결과 출력 핀 키다(동적 핀 — 19.13.3절).
constexpr StringView FunctionInputPreviewPin = "Preview";
constexpr StringView FunctionInputResultPin = "Result";
// previewValue(Vec4) 칸 이름이다.
constexpr StringView FunctionPreviewChannels[MaxIrComponents] = {"x", "y", "z", "w"};

// 입력 종류의 IR 타입이다(UE FunctionTypeMapping — ME:16547–16562). 수 종류는 Float 기준이다.
IrType FunctionInputIrType(FunctionInputType type) noexcept {
    switch (type) {
    case FunctionInputType::Scalar:
        return IrFloat(1);
    case FunctionInputType::Vector2:
        return IrFloat(2);
    case FunctionInputType::Vector3:
        return IrFloat(3);
    case FunctionInputType::Vector4:
        return IrFloat(4);
    case FunctionInputType::Texture2D:
        return IrTextureType(IrTextureKind::Texture2D);
    case FunctionInputType::TextureCube:
        return IrTextureType(IrTextureKind::TextureCube);
    case FunctionInputType::Texture2DArray:
        return IrTextureType(IrTextureKind::Texture2DArray);
    case FunctionInputType::Texture3D:
        return IrTextureType(IrTextureKind::Texture3D);
    case FunctionInputType::StaticBool:
    case FunctionInputType::Bool:
        return IrBool(1);
    case FunctionInputType::MaterialAttributes:
        return IrAttributesType();
    case FunctionInputType::TextureExternal:
        return IrTextureType(IrTextureKind::External);
    case FunctionInputType::Surface:
        return IrSurfaceType();
    }

    return IrFloat(3);
}

// '<prefix><name>'' 문구로 code를 노드 위치에 진단한다.
Result<void> FunctionInputError(LoweringContext& context, MaterialDiagnosticCode code,
                                StringView prefix, StringView name) noexcept {
    SystemAllocator scratch;
    String message(scratch);
    Result<void> built = Result<void>::Success();
    const StringView parts[] = {prefix, name, "'"};
    for (StringView part : parts) {
        if (built) {
            built = message.TryAppend(part);
        }
    }

    if (!built) {
        return built;
    }

    return context.Error(code, StringView(), message.View());
}

// 값을 입력 종류로 맞춘다(UE ValidCast — 넓은 벡터는 앞 성분, 스칼라는 복제, 좁은 벡터는 V1,
// LWC(Double)는 그대로, 텍스처·속성 집합·표면은 같은 종류만).
Result<IrValueId> FitFunctionInput(LoweringContext& context, IrValueId value, IrType target,
                                   StringView name) noexcept {
    if (value == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    const IrType source = context.TypeOf(value);
    if (target.IsOpaque() || source.IsOpaque()) {
        if (source == target) {
            return Result<IrValueId>::Success(value);
        }

        SystemAllocator scratch;
        String message(scratch);
        Result<void> built = Result<void>::Success();
        const StringView parts[] = {"Cannot cast from ",
                                    LoweringTypeName(source),
                                    " to ",
                                    LoweringTypeName(target),
                                    " for function input '",
                                    name,
                                    "'"};
        for (StringView part : parts) {
            if (built) {
                built = message.TryAppend(part);
            }
        }

        if (!built) {
            return Result<IrValueId>::Failure(built.Error());
        }

        if (auto reported =
                context.Error(MaterialDiagnosticCode::TypeMismatch, StringView(), message.View());
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    MaterialPinType pin = MaterialPinType::Bool;
    if (target.scalar == IrScalar::Float) {
        const MaterialPinType first =
            source.scalar == IrScalar::Double ? MaterialPinType::Double1 : MaterialPinType::Float1;
        pin = static_cast<MaterialPinType>(static_cast<uint32>(first) + target.components - 1);
    }

    return context.Coerce(value, pin, StringView());
}

// FunctionInput이다(UE UMaterialExpressionFunctionInput::Compile — ME:16545–16629, 호출 연결은
// LinkIntoCaller — ME:14274–14303).
Result<void> LowerFunctionInput(LoweringContext& context) noexcept {
    auto properties = context.Properties();
    if (!properties) {
        return Result<void>::Failure(properties.Error());
    }

    if (properties.Value() == nullptr) {
        return Result<void>::Success();
    }

    int64 kind = static_cast<int64>(FunctionInputType::Vector3);
    bool usePreview = false;
    auto readKind =
        context.ReadInteger("inputType", static_cast<int64>(FunctionInputType::Vector3), kind);
    auto readPreview = readKind ? context.ReadBool("usePreviewValueAsDefault", false, usePreview)
                                : Result<bool>::Failure(readKind.Error());
    if (!readPreview) {
        return Result<void>::Failure(readPreview.Error());
    }

    if (!readKind.Value() || !readPreview.Value()) {
        return Result<void>::Success();
    }

    if (kind < static_cast<int64>(FunctionInputType::Scalar) ||
        kind > static_cast<int64>(FunctionInputType::Surface)) {
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "Function input type is not known");
    }

    const FunctionInputType type = static_cast<FunctionInputType>(kind);
    auto nameText = properties.Value()->ReadText("inputName");
    const StringView name = nameText ? nameText.Value() : StringView();
    // 호출 쪽 입력이 이어져 있으면 그 값이다. 아니면 usePreviewValueAsDefault일 때만 함수 안의
    // 미리보기 값이다(주 그래프 밖 미리보기 — 호출 없는 사본은 없다).
    IrValueId value = NoIrValue;
    bool resolved = false;
    Guid call;
    Name pin;
    if (context.FunctionInputBinding(call, pin)) {
        bool connected = false;
        auto caller = context.CallerInput(call, pin, connected);
        if (!caller) {
            return Result<void>::Failure(caller.Error());
        }

        if (connected) {
            if (caller.Value() == NoIrValue) {
                return Result<void>::Success();
            }

            value = caller.Value();
            resolved = true;
        } else if (!usePreview) {
            return FunctionInputError(context, MaterialDiagnosticCode::MissingInput,
                                      "Missing function input '", name);
        }
    }

    if (!resolved) {
        auto previewConnected = context.IsConnected(FunctionInputPreviewPin);
        if (!previewConnected) {
            return Result<void>::Failure(previewConnected.Error());
        }

        if (previewConnected.Value()) {
            auto preview = context.NamedDynamicInput(FunctionInputPreviewPin, {});
            if (!preview) {
                return Result<void>::Failure(preview.Error());
            }

            if (preview.Value() == NoIrValue) {
                return Result<void>::Success();
            }

            value = preview.Value();
        } else if (type == FunctionInputType::MaterialAttributes) {
            // UE CompilePreviewValue의 속성 집합은 기본값(칸 없음)이다.
            auto empty = context.MakeAttributes({});
            if (!empty) {
                return Result<void>::Failure(empty.Error());
            }

            value = empty.Value();
        } else if (type <= FunctionInputType::Vector4) {
            // previewValue 상수다(UE CompilePreviewValue — 입력 폭만큼).
            auto preview =
                properties.Value()->OpenStruct("previewValue", GeneratedType<Vec4>::Schema());
            float components[MaxIrComponents] = {};
            Result<void> read =
                preview ? Result<void>::Success() : Result<void>::Failure(preview.Error());
            for (usize at = 0; read && at < MaxIrComponents; ++at) {
                auto channel = preview.Value().ReadFloat(FunctionPreviewChannels[at], 0.0f);
                if (channel) {
                    components[at] = channel.Value();
                } else {
                    read = Result<void>::Failure(channel.Error());
                }
            }

            if (!read) {
                if (read.Error() == ErrorCode::OutOfMemory) {
                    return read;
                }

                return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                     "Node vector property cannot be read");
            }

            const usize width = static_cast<usize>(type) + 1;
            auto constant =
                context.FloatConstant(Span<const float>(components, width), StringView());
            if (!constant) {
                return Result<void>::Failure(constant.Error());
            }

            value = constant.Value();
        } else {
            return FunctionInputError(context, MaterialDiagnosticCode::MissingInput,
                                      "Missing Preview connection for function input '", name);
        }
    }

    auto fitted = FitFunctionInput(context, value, FunctionInputIrType(type), name);
    if (!fitted) {
        return Result<void>::Failure(fitted.Error());
    }

    return context.Output(FunctionInputResultPin, fitted.Value());
}

// 미리보기 Preview 입력만 계획 밖이다(호출 연결이 있으면 번역하지 않는다 — UE LinkIntoCaller).
constexpr LoweringEntry FunctionEntries[] = {
    {"Material.FunctionInput", &LowerFunctionInput, true, {}},
};

} // namespace

Span<const LoweringEntry> FunctionLowerings() noexcept {
    return FunctionEntries;
}

} // namespace rocks::material::detail
