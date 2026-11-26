#include <Core/Text/StringBuilder.h>
#include <MaterialCompiler/MaterialContextSource.h>
#include <MaterialSchema/MaterialContextFeature.h>
#include <MaterialSchema/MaterialContextValue.h>

#include "EntryDefineNames.h"
#include "HlslFloatLiteral.h"

// 문맥 원문 작성이다(Docs/Design/MaterialSystem 6.9절, 19.3절).
namespace rocks::material {

namespace {

constexpr StringView ContextHeader =
    "// Generated from the material context value table by WriteMaterialContextSource\n"
    "// (Docs/Design/MaterialSystem 6.9, 19.3). Do not edit: change the table and regenerate.\n"
    "//\n"
    "// A pass defines MATERIAL_SUPPLY_<FEATURE> 1 for every feature its shader type and vertex\n"
    "// input type supply before including MaterialCommon.rsh. Accessors of features that are not\n"
    "// supplied return the Unreal Engine default value, so generated material source compiles in\n"
    "// every pass.\n"
    "#pragma once\n\n"
    "#include \"/Engine/Private/Material/MaterialDouble.rsh\"\n\n"
    "// UV channel and custom interpolant vector limits (Docs/Design/MaterialSystem 6.9).\n"
    "#define MATERIAL_MAX_TEXCOORDS 8\n"
    "#define MATERIAL_MAX_INTERPOLANT_VECTORS 16\n";

// 원문 타입 이름이다. 원문 타입이 없는 핀 타입은 빈 문자열이다.
StringView ContextTypeName(MaterialPinType type) noexcept {
    switch (type) {
    case MaterialPinType::Float1:
        return "float";
    case MaterialPinType::Float2:
        return "float2";
    case MaterialPinType::Float3:
        return "float3";
    case MaterialPinType::Float4:
        return "float4";
    case MaterialPinType::Double1:
        return "MaterialDouble1";
    case MaterialPinType::Double2:
        return "MaterialDouble2";
    case MaterialPinType::Double3:
        return "MaterialDouble3";
    case MaterialPinType::Double4:
        return "MaterialDouble4";
    case MaterialPinType::Int1:
        return "int";
    case MaterialPinType::UInt1:
        return "uint";
    case MaterialPinType::Bool:
        return "bool";
    case MaterialPinType::Float3x3:
        return "float3x3";
    case MaterialPinType::Float4x4:
        return "float4x4";
    default:
        return {};
    }
}

// Float 벡터 성분 수다(아니면 0).
uint32 FloatComponents(MaterialPinType type) noexcept {
    const uint32 value = static_cast<uint32>(type);
    const uint32 first = static_cast<uint32>(MaterialPinType::Float1);
    const uint32 last = static_cast<uint32>(MaterialPinType::Float4);
    return value >= first && value <= last ? value - first + 1 : 0;
}

// 칸 이름(Rocks 이름의 lowerCamel)을 붙인다. previous면 previous<이름>이다.
void AppendFieldName(StringBuilder<>& out, StringView name, bool previous) noexcept {
    if (previous) {
        out << "previous" << name;
        return;
    }

    if (name.IsEmpty()) {
        return;
    }

    const char first = name.Data()[0];
    const char lower = first >= 'A' && first <= 'Z' ? static_cast<char>(first - 'A' + 'a') : first;
    out << StringView(&lower, 1) << StringView(name.Data() + 1, name.ByteCount() - 1);
}

// 공급하지 않았을 때의 값 리터럴이다(결정 MD15).
Result<void> AppendDefault(StringBuilder<>& out, const MaterialContextValue& value) noexcept {
    const uint32 components = FloatComponents(value.type);
    if (components == 1) {
        out << "(";
        if (auto literal = detail::AppendHlslFloatLiteral(out, value.defaultValue.values[0]);
            !literal) {
            return literal;
        }

        out << ")";
        return out.Status();
    }

    if (components > 1) {
        out << ContextTypeName(value.type) << "(";
        for (uint32 index = 0; index < components; ++index) {
            if (index > 0) {
                out << ", ";
            }

            if (auto literal =
                    detail::AppendHlslFloatLiteral(out, value.defaultValue.values[index]);
                !literal) {
                return literal;
            }
        }

        out << ")";
        return out.Status();
    }

    if (value.type == MaterialPinType::Bool) {
        out << "(false)";
        return out.Status();
    }

    // 행렬·Double·정수는 0이다(UE 템플릿 기본값).
    out << "((" << ContextTypeName(value.type) << ")0)";
    return out.Status();
}

// 매크로 인자 목록 "(c, a0, …)"이다.
void AppendParameters(StringBuilder<>& out, uint8 count) noexcept {
    out << "(c";
    for (uint8 index = 0; index < count; ++index) {
        out << ", a" << index;
    }

    out << ")";
}

// 접근자 하나다(supplied면 칸이나 pass 함수, 아니면 기본값).
Result<void> AppendAccessor(StringBuilder<>& out, const MaterialContextValue& value, bool previous,
                            bool supplied) noexcept {
    out << "#define MaterialContext" << (previous ? StringView("Previous") : StringView())
        << value.name;
    AppendParameters(out, value.argumentCount);
    out << " ";
    if (!supplied) {
        if (auto literal = AppendDefault(out, value); !literal) {
            return literal;
        }
    } else if (value.form == MaterialContextValueForm::Field) {
        out << "((c).";
        AppendFieldName(out, value.name, previous);
        out << ")";
    } else {
        out << "MaterialPass" << (previous ? StringView("Previous") : StringView()) << value.name;
        AppendParameters(out, value.argumentCount);
    }

    out << "\n";
    return out.Status();
}

// stage 단계 구조체의 칸들이다.
Result<void> AppendStruct(StringBuilder<>& out, StringView name, MaterialStageMask stage,
                          Span<const MaterialContextValue> table) noexcept {
    out << "\nstruct " << name << " {\n";
    if (stage == MaterialPixelStage) {
        // pass가 늘 채우는 고정 칸이다(7.7절, 보간 값 — 6.9절 MS2b4a).
        out << "    float4 svPosition;\n"
            << "    float4 interpolants[MATERIAL_MAX_INTERPOLANT_VECTORS];\n";
    }

    for (const MaterialContextValue& value : table) {
        if (value.form != MaterialContextValueForm::Field || (value.stages & stage) == 0) {
            continue;
        }

        const StringView type = ContextTypeName(value.type);
        if (type.IsEmpty()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 이전 프레임 값은 이전 정점 진입점만 읽는다(19.3.5절).
        const uint32 variants = value.hasPrevious && stage == MaterialVertexStage ? 2 : 1;
        for (uint32 variant = 0; variant < variants; ++variant) {
            out << "    " << type << " ";
            AppendFieldName(out, value.name, variant == 1);
            out << ";\n";
        }
    }

    out << "};\n";
    return out.Status();
}

} // namespace

Result<void> WriteMaterialContextSource(Allocator& allocator, String& out) noexcept {
    const Span<const MaterialContextValue> table = MaterialContextValues();
    StringBuilder<> text(allocator);
    text << ContextHeader;
    text << "\n// Supply flags (0 unless the pass defines them).\n";
    for (uint32 feature = 0; feature < MaterialContextFeatureCount; ++feature) {
        const StringView define =
            MaterialContextFeatureDefineName(static_cast<MaterialContextFeature>(feature));
        text << "#ifndef MATERIAL_SUPPLY_" << define << "\n#define MATERIAL_SUPPLY_" << define
             << " 0\n#endif\n";
    }

    if (auto vertex = AppendStruct(text, "MaterialVertexInputs", MaterialVertexStage, table);
        !vertex) {
        return vertex;
    }

    if (auto pixel = AppendStruct(text, "MaterialInputs", MaterialPixelStage, table); !pixel) {
        return pixel;
    }

    text << "\n// Accessors (MaterialContext<Name>, previous frame "
            "MaterialContextPrevious<Name>).\n";
    for (uint32 feature = 0; feature < MaterialContextFeatureCount; ++feature) {
        const MaterialContextFeature item = static_cast<MaterialContextFeature>(feature);
        bool any = false;
        for (const MaterialContextValue& value : table) {
            any = any || (value.feature == item && !ContextTypeName(value.type).IsEmpty());
        }

        if (!any) {
            continue;
        }

        text << "\n#if MATERIAL_SUPPLY_" << MaterialContextFeatureDefineName(item) << "\n";
        for (uint32 supplied = 2; supplied > 0; --supplied) {
            if (supplied == 1) {
                text << "#else\n";
            }

            for (const MaterialContextValue& value : table) {
                // 자원 타입 값(텍스처 서술자 등)은 원문 타입이 생기는 MS2c가 더한다.
                if (value.feature != item || ContextTypeName(value.type).IsEmpty()) {
                    continue;
                }

                for (uint32 variant = 0; variant < (value.hasPrevious ? 2u : 1u); ++variant) {
                    if (auto accessor = AppendAccessor(text, value, variant == 1, supplied == 2);
                        !accessor) {
                        return accessor;
                    }
                }
            }
        }

        text << "#endif\n";
    }

    // 채울 기능: 공급 ∧ 어느 진입점이든 사용(매크로는 쓰는 자리에서 펼쳐지므로 생성 파일 include
    // 뒤의 #if에서 사용 정의를 본다).
    text << "\n// Features the pass must fill: supplied and used by any entry point of the "
            "material.\n";
    for (uint32 feature = 0; feature < MaterialContextFeatureCount; ++feature) {
        const StringView define =
            MaterialContextFeatureDefineName(static_cast<MaterialContextFeature>(feature));
        text << "#define MATERIAL_CONTEXT_" << define << " (MATERIAL_SUPPLY_" << define << " && (";
        for (uint32 entry = 0; entry < IrEntryCount; ++entry) {
            if (entry > 0) {
                text << " || ";
            }

            text << "MATERIAL_" << detail::EntryDefineNames[entry] << "_USES_" << define;
        }

        text << "))\n";
    }

    if (auto status = text.Status(); !status) {
        return status;
    }

    return out.TryAssign(text.View());
}

} // namespace rocks::material
