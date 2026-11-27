#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <MaterialSchema/Nodes/Custom/CustomDefine.generated.h>
#include <MaterialSchema/Nodes/Custom/CustomDefine.h>
#include <MaterialSchema/Nodes/Custom/CustomNodeEnums.h>
#include <MaterialSchema/Nodes/Custom/CustomOutput.generated.h>
#include <MaterialSchema/Nodes/Custom/CustomOutput.h>

#include "CustomCodeScan.h"
#include "LoweringContext.h"
#include "LoweringTable.h"
#include "Translator.h"

// Custom HLSL lowering이다(Docs/Design/MaterialSystem 6.13절 MS2f5, 7.5절 V13 — UE
// UMaterialExpressionCustom::Compile·FHLSLMaterialTranslator::CustomExpression). 코드·선언은 빌더의
// Custom 함수 표 항목이고, 노드 결과는 CustomCall(주 출력)·CustomOutput(추가 출력)이다.
namespace rocks::material::detail {

namespace {

// 문맥 인자 이름이다(생성 함수의 첫 인자 — 6.13절 MS2f5).
constexpr StringView CustomContextName = "c";
// 정의 이름으로 쓸 수 없는 생성 원문 식별자 접두다(맨 식별자 c·s·m<숫자>는 따로 본다).
constexpr StringView CustomReservedPrefixes[] = {"Material", "MATERIAL_"};

// 허용 include 경로인가(/로 시작하는 가상 경로, ..·따옴표·역슬래시·제어 문자 없음).
bool IsAllowedIncludePath(StringView path) noexcept {
    if (path.IsEmpty() || path.Data()[0] != '/' ||
        path.Contains("..", StringComparison::CaseSensitive)) {
        return false;
    }

    for (usize at = 0; at < path.ByteCount(); ++at) {
        const char character = path.Data()[at];
        if (character == '"' || character == '\\' || static_cast<unsigned char>(character) < ' ') {
            return false;
        }
    }

    return true;
}

bool HasLineBreak(StringView text) noexcept {
    for (usize at = 0; at < text.ByteCount(); ++at) {
        if (text.Data()[at] == '\n' || text.Data()[at] == '\r') {
            return true;
        }
    }

    return false;
}

// 정의 이름으로 예약된 생성 원문 식별자인가(c·s·m<숫자>…·Material·MATERIAL_ 접두).
bool IsReservedDefineName(StringView name) noexcept {
    if (name == CustomContextName || name == StringView("s")) {
        return true;
    }

    if (name.ByteCount() >= 2 && name.Data()[0] == 'm' && name.Data()[1] >= '0' &&
        name.Data()[1] <= '9') {
        return true;
    }

    for (StringView prefix : CustomReservedPrefixes) {
        if (name.StartsWith(prefix, StringComparison::CaseSensitive)) {
            return true;
        }
    }

    return false;
}

// parts를 이어 붙인 문구로 V13을 진단한다.
Result<void> CustomRule(LoweringContext& context, Span<const StringView> parts) noexcept {
    SystemAllocator scratch;
    StringBuilder<> message(scratch);
    for (StringView part : parts) {
        message << part;
    }

    if (auto status = message.Status(); !status) {
        return status;
    }

    return context.Error(MaterialDiagnosticCode::CustomCodeRule, StringView(), message.View());
}

// 코드·정의 값의 V13 토큰 검사다(6.13절 MS2f5 목록). 위반을 모두 진단하고 위반이 있으면 false다.
// define이 비어 있지 않으면 정의 값 규칙(한 줄·주석 없음)도 본다.
Result<bool> CheckCustomTokens(LoweringContext& context, StringView text,
                               StringView define) noexcept {
    CustomCodeScan scan;
    ScanCustomCode(text, scan);
    bool clean = true;
    const auto report = [&](Span<const StringView> parts) noexcept -> Result<void> {
        clean = false;
        return CustomRule(context, parts);
    };

    for (usize index = 0; index < CustomForbiddenWordCount; ++index) {
        if (!scan.words[index]) {
            continue;
        }

        const StringView parts[] = {"Custom HLSL code must not contain '",
                                    CustomForbiddenWord(index), "'"};
        if (auto reported = report(parts); !reported) {
            return Result<bool>::Failure(reported.Error());
        }
    }

    if (scan.directive) {
        const StringView parts[] = {"Custom HLSL code must not contain '#", scan.directiveName,
                                    "'"};
        if (auto reported = report(parts); !reported) {
            return Result<bool>::Failure(reported.Error());
        }
    }

    struct Flagged {
        bool present;
        StringView message;
    };

    const Flagged flags[] = {
        {scan.lineContinuation, "Custom HLSL code must not contain line continuations"},
        {scan.unterminatedComment, "Custom HLSL code must not contain an unterminated comment"},
        {scan.unterminatedString, "Custom HLSL code must not contain an unterminated string"},
        {scan.closesBody, "Custom HLSL code must not close the function body"},
        {scan.leavesOpen, "Custom HLSL code must not leave a brace open"}};
    for (const Flagged& flag : flags) {
        if (!flag.present) {
            continue;
        }

        const StringView parts[] = {flag.message};
        if (auto reported = report(parts); !reported) {
            return Result<bool>::Failure(reported.Error());
        }
    }

    if (!define.IsEmpty() && HasLineBreak(text)) {
        const StringView parts[] = {"Custom HLSL define value must be a single line: ", define};
        if (auto reported = report(parts); !reported) {
            return Result<bool>::Failure(reported.Error());
        }
    }

    if (!define.IsEmpty() && scan.comment) {
        const StringView parts[] = {"Custom HLSL define value must not contain comments: ", define};
        if (auto reported = report(parts); !reported) {
            return Result<bool>::Failure(reported.Error());
        }
    }

    return Result<bool>::Success(clean);
}

// 이름 규칙(HLSL 식별자) 검사다. 어기면 진단하고 false다.
Result<bool> CheckCustomName(LoweringContext& context, StringView name) noexcept {
    if (IsCustomIdentifier(name)) {
        return Result<bool>::Success(true);
    }

    const StringView parts[] = {"Custom HLSL name is not a valid identifier: ", name};
    if (auto reported = CustomRule(context, parts); !reported) {
        return Result<bool>::Failure(reported.Error());
    }

    return Result<bool>::Success(false);
}

// 예약 이름 진단이다(false).
Result<bool> ReservedCustomName(LoweringContext& context, StringView name) noexcept {
    const StringView parts[] = {"Custom HLSL name is reserved: ", name};
    if (auto reported = CustomRule(context, parts); !reported) {
        return Result<bool>::Failure(reported.Error());
    }

    return Result<bool>::Success(false);
}

// 함수 인자 이름 목록(입력·텍스처 샘플러·추가 출력)이 겹치지 않고 문맥 인자 c가 없는지 본다.
// 겹치는 이름마다 한 번 진단하고 어기면 false다.
Result<bool> CheckCustomParameterNames(LoweringContext& context,
                                       Span<const StringView> names) noexcept {
    bool clean = true;
    for (usize index = 0; index < names.Size(); ++index) {
        if (names[index] == CustomContextName) {
            auto reserved = ReservedCustomName(context, names[index]);
            if (!reserved) {
                return reserved;
            }

            clean = false;
            continue;
        }

        bool repeated = false;
        bool earlier = false;
        for (usize other = 0; other < names.Size(); ++other) {
            if (other != index && names[other] == names[index]) {
                repeated = true;
                earlier = earlier || other < index;
            }
        }

        if (!repeated || earlier) {
            continue;
        }

        clean = false;
        const StringView parts[] = {"Custom HLSL name is used more than once: ", names[index]};
        if (auto reported = CustomRule(context, parts); !reported) {
            return Result<bool>::Failure(reported.Error());
        }
    }

    return Result<bool>::Success(clean);
}

// CustomOutputType 값의 IR 형이다(MaterialAttributes·범위 밖이면 false).
bool CustomOutputIrType(int64 value, IrType& out) noexcept {
    if (value < static_cast<int64>(CustomOutputType::Float1) ||
        value > static_cast<int64>(CustomOutputType::Float4)) {
        return false;
    }

    out = IrFloat(static_cast<uint8>(value - static_cast<int64>(CustomOutputType::Float1) + 1));
    return true;
}

// 추가 출력 하나다(이름 있는 것만 — UE RebuildOutputs).
struct CustomOutputSlot {
    StringView name;
    IrType type;
};

// 정의 하나다.
struct CustomDefineSlot {
    StringView name;
    StringView value;
};

// 인자 하나다(이름 있는 입력만 — UE Compile).
struct CustomArgumentSlot {
    StringView name;
    IrValueId value = NoIrValue;
};

Result<void> LowerCustomHlsl(LoweringContext& context) noexcept {
    auto properties = context.Properties();
    if (!properties || properties.Value() == nullptr) {
        return properties ? Result<void>::Success() : Result<void>::Failure(properties.Error());
    }

    NodePropertyReader& reader = *properties.Value();
    constexpr StringView Unreadable = "Custom HLSL properties cannot be read";
    auto code = reader.ReadText("code");
    auto description = reader.ReadText("description");
    auto outputType =
        reader.ReadInteger("outputType", static_cast<int64>(CustomOutputType::Float3));
    auto clip = reader.ReadInteger("containsClipInstruction",
                                   static_cast<int64>(CustomClipInstruction::Search));
    if (!code || !description || !outputType || !clip) {
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(), Unreadable);
    }

    SystemAllocator scratch;
    Array<StringView> includes(scratch);
    if (auto read = reader.ReadTextArray("includeFilePaths", includes); !read) {
        if (read.Error() == ErrorCode::OutOfMemory) {
            return read;
        }

        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(), Unreadable);
    }

    // 출력 형: Rocks 속성 집합은 번역기 기록이라 HLSL 구조체가 없다(편차 — 6.13절 MS2f5).
    constexpr StringView AttributesOutput =
        "Custom HLSL MaterialAttributes output is not supported";
    IrType returnType = IrFloat(1);
    if (!CustomOutputIrType(outputType.Value(), returnType)) {
        return outputType.Value() == static_cast<int64>(CustomOutputType::MaterialAttributes)
                   ? context.Error(MaterialDiagnosticCode::UnsupportedCapability, StringView(),
                                   AttributesOutput)
                   : context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                   Unreadable);
    }

    auto outputList =
        reader.OpenStructArray("additionalOutputs", GeneratedType<CustomOutput>::Schema());
    auto defineList =
        reader.OpenStructArray("additionalDefines", GeneratedType<CustomDefine>::Schema());
    if (!outputList || !defineList) {
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(), Unreadable);
    }

    bool valid = true;
    const auto check = [&](Result<bool> passed) noexcept -> Result<void> {
        if (!passed) {
            return Result<void>::Failure(passed.Error());
        }

        valid = valid && passed.Value();
        return Result<void>::Success();
    };

    Array<CustomOutputSlot> outputs(scratch);
    for (usize index = 0; index < outputList.Value().Count(); ++index) {
        auto element = outputList.Value().Element(index);
        if (!element) {
            return Result<void>::Failure(element.Error());
        }

        auto name = element.Value().ReadText("outputName");
        auto kind =
            element.Value().ReadInteger("outputType", static_cast<int64>(CustomOutputType::Float1));
        if (!name || !kind) {
            return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(), Unreadable);
        }

        if (name.Value().IsEmpty()) {
            continue;
        }

        IrType type = IrFloat(1);
        if (!CustomOutputIrType(kind.Value(), type)) {
            return kind.Value() == static_cast<int64>(CustomOutputType::MaterialAttributes)
                       ? context.Error(MaterialDiagnosticCode::UnsupportedCapability, StringView(),
                                       AttributesOutput)
                       : context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                       Unreadable);
        }

        if (auto checked = check(CheckCustomName(context, name.Value())); !checked) {
            return checked;
        }

        if (auto pushed = outputs.TryPushBack(CustomOutputSlot{name.Value(), type}); !pushed) {
            return pushed;
        }
    }

    Array<CustomDefineSlot> defines(scratch);
    for (usize index = 0; index < defineList.Value().Count(); ++index) {
        auto element = defineList.Value().Element(index);
        if (!element) {
            return Result<void>::Failure(element.Error());
        }

        auto name = element.Value().ReadText("defineName");
        auto value = element.Value().ReadText("defineValue");
        if (!name || !value) {
            return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(), Unreadable);
        }

        // 정의 이름은 예약되지 않은 식별자, 값은 한 줄·주석 없음이고 코드와 같은 토큰 검사를
        // 받는다(파일 범위로 나간다).
        if (auto checked = check(CheckCustomName(context, name.Value())); !checked) {
            return checked;
        }

        if (IsCustomIdentifier(name.Value()) && IsReservedDefineName(name.Value())) {
            if (auto checked = check(ReservedCustomName(context, name.Value())); !checked) {
                return checked;
            }
        }

        if (auto checked = check(CheckCustomTokens(context, value.Value(), name.Value()));
            !checked) {
            return checked;
        }

        if (auto pushed = defines.TryPushBack(CustomDefineSlot{name.Value(), value.Value()});
            !pushed) {
            return pushed;
        }
    }

    // V13: 코드 토큰·include 경로·clip 선언(7.5절).
    if (auto checked = check(CheckCustomTokens(context, code.Value(), StringView())); !checked) {
        return checked;
    }

    for (StringView path : includes) {
        if (IsAllowedIncludePath(path)) {
            continue;
        }

        valid = false;
        const StringView parts[] = {"Custom HLSL include path is not an allowed virtual path: ",
                                    path};
        if (auto reported = CustomRule(context, parts); !reported) {
            return reported;
        }
    }

    // 주석·문자열 밖 clip 호출·discard 토큰이다(No 선언 거부 — UE 부분 문자열보다 정확하다).
    CustomCodeScan scan;
    ScanCustomCode(code.Value(), scan);
    const bool tokens = scan.clipCall || scan.discardWord;
    // UE HasPixelDiscard: Yes, 또는 Search이면서 clip(·discard 부분 문자열 — 토큰 검출도 더한다.
    const bool discards = tokens ||
                          code.Value().Contains("clip(", StringComparison::CaseSensitive) ||
                          code.Value().Contains("discard", StringComparison::CaseSensitive);
    if (clip.Value() == static_cast<int64>(CustomClipInstruction::No) && tokens) {
        valid = false;
        if (auto reported =
                context.Error(MaterialDiagnosticCode::CustomCodeRule, StringView(),
                              "Custom HLSL uses clip or discard but containsClipInstruction is No");
            !reported) {
            return reported;
        }
    }

    if (!valid) {
        return Result<void>::Success();
    }

    // 입력: 이름 있는 입력만 인자다(이름 없는 입력은 건너뛴다 — UE). 동적 입력 핀의 표시 이름이
    // 입력 이름이다(이름 없으면 비어 있다 — 키 Input<n>).
    auto pins = context.DynamicPins();
    if (!pins) {
        return Result<void>::Failure(pins.Error());
    }

    Array<CustomArgumentSlot> arguments(scratch);
    usize inputNumber = 0;
    for (const nodegraph::PinDescriptor& pin : pins.Value()) {
        if (pin.direction != nodegraph::PinDirection::Input) {
            continue;
        }

        ++inputNumber;
        if (pin.displayName.IsEmpty()) {
            continue;
        }

        if (auto checked = check(CheckCustomName(context, pin.displayName)); !checked) {
            return checked;
        }

        // 연결이 없으면 값 없음이고 독(연결된 입력이 값 없이 온 것)이 아니다(키로 찾는다).
        auto value = context.DynamicInput(pin.key, Optional<float>(), false);
        if (!value) {
            return Result<void>::Failure(value.Error());
        }

        if (value.Value() == NoIrValue && context.Poisoned()) {
            return Result<void>::Success();
        }

        if (value.Value() == NoIrValue) {
            SystemAllocator messageScratch;
            StringBuilder<> message(messageScratch);
            message << "Custom material " << description.Value() << " missing input " << inputNumber
                    << " (" << pin.displayName << ")";
            if (auto status = message.Status(); !status) {
                return status;
            }

            return context.Error(MaterialDiagnosticCode::MissingInput, pin.displayName,
                                 message.View());
        }

        IrValueId argument = value.Value();
        const IrType type = context.TypeOf(argument);
        if (type.scalar == IrScalar::Texture) {
            const IrValue& texture = context.Builder().Value(argument);
            const Guid id{texture.immediate[0], texture.immediate[1]};
            const IrTextureSource source =
                static_cast<IrTextureSource>(texture.immediate[2] & IrTextureSourceMask);
            auto admitted = context.UseSampler(source, id);
            if (!admitted || !admitted.Value()) {
                return admitted ? Result<void>::Success() : Result<void>::Failure(admitted.Error());
            }

            // Custom은 <이름>Sampler 인자로 자기 샘플러를 넘긴다(6.10절 텍스처 표 — 자기 출처
            // slot).
            auto sampled = context.Builder().SampledTexture(argument, IrSamplerSource::Own);
            if (!sampled) {
                return Result<void>::Failure(sampled.Error());
            }

            argument = sampled.Value();
        } else if (type.IsOpaque() || type.IsMatrix()) {
            SystemAllocator messageScratch;
            StringBuilder<> message(messageScratch);
            message << "Bad type " << LoweringTypeName(type) << " for " << description.Value()
                    << " input " << pin.displayName;
            if (auto status = message.Status(); !status) {
                return status;
            }

            return context.Error(MaterialDiagnosticCode::TypeMismatch, pin.displayName,
                                 message.View());
        } else if (type.scalar != IrScalar::Float && type.scalar != IrScalar::Double) {
            // 정수·Bool은 float로 넘긴다(UE 고전 번역기 값은 float뿐이다).
            auto converted = context.Convert(argument, IrScalar::Float);
            if (!converted || converted.Value() == NoIrValue) {
                return converted ? Result<void>::Success()
                                 : Result<void>::Failure(converted.Error());
            }

            argument = converted.Value();
        }

        if (auto pushed = arguments.TryPushBack(CustomArgumentSlot{pin.displayName, argument});
            !pushed) {
            return pushed;
        }
    }

    // 함수 인자 이름(입력·텍스처 입력의 <이름>Sampler·추가 출력)이 겹치거나 c면 HLSL 재정의다.
    Array<String> samplers(scratch);
    for (const CustomArgumentSlot& argument : arguments) {
        if (context.TypeOf(argument.value).scalar != IrScalar::Texture) {
            continue;
        }

        String sampler(scratch);
        if (auto assigned = sampler.TryAssign(argument.name); !assigned) {
            return assigned;
        }

        if (auto appended = sampler.TryAppend("Sampler"); !appended) {
            return appended;
        }

        if (auto pushed = samplers.TryPushBack(Move(sampler)); !pushed) {
            return pushed;
        }
    }

    Array<StringView> parameterNames(scratch);
    for (const CustomArgumentSlot& argument : arguments) {
        if (auto pushed = parameterNames.TryPushBack(argument.name); !pushed) {
            return pushed;
        }
    }

    for (const String& sampler : samplers) {
        if (auto pushed = parameterNames.TryPushBack(sampler.View()); !pushed) {
            return pushed;
        }
    }

    for (const CustomOutputSlot& output : outputs) {
        if (auto pushed = parameterNames.TryPushBack(output.name); !pushed) {
            return pushed;
        }
    }

    if (auto checked = check(CheckCustomParameterNames(context, parameterNames.View())); !checked) {
        return checked;
    }

    if (!valid) {
        return Result<void>::Success();
    }

    // 표 항목: (노드, 문맥, 인자 형)이 같으면 기존 항목이다.
    const IrCustomContext customContext = context.Context() == EvaluationContext::Pixel
                                              ? IrCustomContext::Pixel
                                              : IrCustomContext::Vertex;
    const bool pixelDiscard =
        clip.Value() == static_cast<int64>(CustomClipInstruction::Yes) ||
        (clip.Value() == static_cast<int64>(CustomClipInstruction::Search) && discards);
    IrCustomTable& table = context.Builder().CustomFunctions();
    if (auto begun =
            table.Begin(context.Node().id, customContext, returnType, code.Value(), pixelDiscard);
        !begun) {
        return begun;
    }

    Result<void> added = Result<void>::Success();
    const auto add = [&](Result<void> step) noexcept {
        if (added) {
            added = Move(step);
        }
    };

    for (const CustomArgumentSlot& argument : arguments) {
        add(table.AddArgument(argument.name, context.TypeOf(argument.value)));
    }

    for (const CustomOutputSlot& output : outputs) {
        add(table.AddOutput(output.name, output.type));
    }

    for (const CustomDefineSlot& define : defines) {
        add(table.AddDefine(define.name, define.value));
    }

    for (StringView path : includes) {
        add(table.AddInclude(path));
    }

    if (!added) {
        table.Abandon();
        return added;
    }

    auto function = table.End();
    if (!function) {
        return Result<void>::Failure(function.Error());
    }

    Array<IrValueId> values(scratch);
    for (const CustomArgumentSlot& argument : arguments) {
        if (auto pushed = values.TryPushBack(argument.value); !pushed) {
            return pushed;
        }
    }

    auto call = context.Builder().CustomCall(function.Value(), values.View());
    if (!call) {
        return Result<void>::Failure(call.Error());
    }

    // 출력 핀: 첫 출력이 주 출력(키 Result 또는 ResultOut), 이후는 이름 있는 추가 출력 순서다.
    uint32 output = 0;
    bool main = true;
    for (const nodegraph::PinDescriptor& pin : pins.Value()) {
        if (pin.direction != nodegraph::PinDirection::Output) {
            continue;
        }

        if (main) {
            main = false;
            if (auto recorded = context.OutputByName(pin.key, call.Value()); !recorded) {
                return recorded;
            }

            continue;
        }

        auto extra = context.Builder().CustomOutput(call.Value(), output++);
        if (!extra) {
            return Result<void>::Failure(extra.Error());
        }

        if (auto recorded = context.OutputByName(pin.key, extra.Value()); !recorded) {
            return recorded;
        }
    }

    return Result<void>::Success();
}

constexpr LoweringEntry CustomEntries[] = {
    {"Material.CustomHlsl", &LowerCustomHlsl},
};

} // namespace

Span<const LoweringEntry> CustomLowerings() noexcept {
    return CustomEntries;
}

} // namespace rocks::material::detail
