using System.Text.Json;
using System.Text.RegularExpressions;

namespace Rocks.MetaCompiler;

// Sparks binding schema의 함수(`functions` — Docs/Design/SparksBinding.ko.md EI1b B11–B16)다. ROCKS_FUNCTION(ScriptCallable)만 싣고, 이름은
// ScriptName 또는 도구 이름(Result 함수의 `TryX` → `X`), 매개변수·반환 표기는 필드와 같은 B4 표의 최상위 열이다.
internal static partial class SparksBinding
{
    private const string FunctionRemedy = "remove ScriptCallable from the function";

    private const string TryPrefix = "Try";

    // perSecond 매개변수가 될 수 있는 Sparks 형식이다(B11 — 이동·회전 속도).
    private static readonly HashSet<string> PerSecondTypes = new(StringComparer.Ordinal) { "float32", "float64", "vec2", "vec3", "vec4", "Rotator" };

    // 매개변수 이름은 lowerCamel ASCII다(SL 10.2 — 영문 소문자로 시작, 영문자·숫자).
    private static readonly Regex LowerCamel = new(@"^[a-z][A-Za-z0-9]*\z", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    // 노출 함수 하나의 규칙이다: 노출 타입의 함수, EditorOnly 아님, 이름·hook 충돌, 매개변수 이름, 표기, perSecond.
    private static void CheckFunction(SchemaModel schema, ClassModel owner, FunctionModel function, SparksNames names)
    {
        string symbol = owner.Name + "::" + function.Name;
        if (owner.EditorOnly || function.Spec.EditorOnly)
        {
            throw new MetadataException($"{symbol}: an EditorOnly function{(owner.EditorOnly ? " of the EditorOnly type " + owner.Name : "")} cannot be called from Sparks", "RMC2607",
                "Editor-only functions are not in shipping builds; remove ScriptCallable (editor buttons come with @editorButton).").At(function.Site);
        }

        if (!owner.ScriptType)
        {
            throw new MetadataException($"{symbol}: the function is callable from Sparks but {owner.Name} is not a Sparks type", "RMC2608",
                "Add ScriptType (or ScriptExtensible) to the type, or remove ScriptCallable from the function.").At(function.Site);
        }

        string source = function.Spec.ScriptName.Length != 0 ? $"ScriptName '{function.Spec.ScriptName}'" : $"'{function.EffectiveToolName}'";
        string name = FunctionName(function) ?? throw new MetadataException(
            $"{symbol}: Sparks function name from {source} must be PascalCase letters and digits without a Try prefix", "RMC2604",
            "Use ScriptName = \"PascalName\" (SL 10.3: Result functions named TryX are shown as X; other names keep no Try prefix and no underscores).").At(function.Site);
        if (names.IsReserved(name))
        {
            throw new MetadataException($"{symbol}: Sparks function name '{name}' is a reserved Sparks name", "RMC2606",
                "Expose the function under another name with ScriptName = \"...\" (prelude, built-in, core engine type and context names cannot be member names — " +
                "a member would hide them in derived Sparks classes).").At(function.Site);
        }

        if (Kind(schema, owner) == "object" && names.HookFor(name) is string hook)
        {
            throw new MetadataException($"{symbol}: Sparks function name '{name}' collides with the lifecycle hook '{hook}'", "RMC2606",
                "Expose the function under another name with ScriptName = \"...\" (for example RequestDestroy for Destroy).").At(function.Site);
        }

        foreach (FieldModel parameter in function.Parameters)
        {
            if (!LowerCamel.IsMatch(parameter.Name))
            {
                throw new MetadataException($"{symbol}: parameter name '{parameter.Name}' must be lowerCamel letters and digits for Sparks", "RMC2604",
                    "Rename the C++ parameter (Sparks shows C++ parameter names as they are).").At(function.Site);
            }

            if (names.Keywords.Contains(parameter.Name))
            {
                throw new MetadataException($"{symbol}: parameter name '{parameter.Name}' is a Sparks keyword", "RMC2606",
                    "Rename the C++ parameter.").At(function.Site);
            }

            _ = Notation(schema, parameter, $"{symbol}: parameter '{parameter.Name}'", topLevel: true, FunctionRemedy);
        }

        foreach (string perSecond in function.Spec.ScriptPerSecond)
        {
            FieldModel parameter = function.Parameters.FirstOrDefault(parameter => parameter.Name == perSecond) ?? throw new MetadataException(
                $"{symbol}: ScriptPerSecond names '{perSecond}', which is not a parameter", "RMC2603", "List parameter names of the function.").At(function.Site);
            string type = Notation(schema, parameter, symbol, topLevel: true, FunctionRemedy);
            if (!PerSecondTypes.Contains(type))
            {
                throw new MetadataException($"{symbol}: per-second parameter '{perSecond}' has the type {type}", "RMC2601",
                    "Per-second parameters are float32, float64, vec2, vec3, vec4 or Rotator rates.").At(function.Site);
            }
        }

        _ = Returns(schema, function, symbol);
    }

    // 함수의 Sparks 이름이다(B13): ScriptName, 없으면 도구 이름 — Result 함수는 `Try` + 대문자를 뗀다. 결과가 PascalCase ASCII가 아니거나
    // `Try` + 대문자로 시작하면 null이다. import 함수는 descriptor의 최종 이름이다.
    private static string? FunctionName(FunctionModel function)
    {
        if (function.Imported)
        {
            return function.Spec.ScriptName.Length != 0 ? function.Spec.ScriptName : null;
        }

        // ScriptName도 Try 접두를 쓸 수 없다(SL 8장 — 노출 API 이름).
        string name = function.Spec.ScriptName.Length != 0 ? function.Spec.ScriptName : function.EffectiveToolName;
        if (function.Spec.ScriptName.Length == 0 && function.ReturnsResult && StartsWithTry(name))
        {
            name = name[TryPrefix.Length..];
        }

        return Specifiers.IsScriptName(name) && !StartsWithTry(name) ? name : null;
    }

    // 노출 함수의 최종 Sparks 이름이다(descriptor scriptName — B15). 노출하지 않은 함수는 ""다.
    public static string ScriptFunctionName(FunctionModel function) =>
        function.Spec.ScriptCallable ? FunctionName(function) ?? "" : "";

    public static bool StartsWithTry(string name) =>
        name.Length > TryPrefix.Length && name.StartsWith(TryPrefix, StringComparison.Ordinal) && char.IsAsciiLetterUpper(name[TryPrefix.Length]);

    // 반환 표기다(B14): void면 null, Result<void>면 "Result<void>", 값 T, Result<T>는 "Result<T>"(T가 nullable 표기면 RMC2601).
    private static string? Returns(SchemaModel schema, FunctionModel function, string symbol)
    {
        if (function.Result is not FieldModel result)
        {
            return function.ReturnsResult ? "Result<void>" : null;
        }

        string type = Notation(schema, result, $"{symbol}: the return type", topLevel: true, FunctionRemedy);
        if (!function.ReturnsResult)
        {
            return type;
        }

        if (type.EndsWith('?'))
        {
            throw new MetadataException($"{symbol}: Result<{type}> cannot be shown to Sparks (a Result value is never null)", "RMC2601",
                "Return a non-null value type in Result, return T? without Result, or remove ScriptCallable.").At(function.Site);
        }

        return "Result<" + type + ">";
    }

    // 타입의 functions 배열이다(B16 — 노출 함수가 없으면 쓰지 않는다, 선언 순서, 기본값인 key는 생략).
    private static void WriteFunctions(Utf8JsonWriter json, SchemaModel schema, ClassModel type)
    {
        List<FunctionModel> functions = [.. type.Functions.Where(function => function.Spec.ScriptCallable)];
        if (functions.Count == 0)
        {
            return;
        }

        json.WriteStartArray("functions");
        foreach (FunctionModel function in functions)
        {
            string symbol = type.Name + "::" + function.Name;
            json.WriteStartObject();
            json.WriteString("name", FunctionName(function));
            json.WriteString("id", function.Id);
            if (function.IsStatic)
            {
                json.WriteBoolean("static", true);
            }

            if (function.Parameters.Count != 0)
            {
                json.WriteStartArray("parameters");
                foreach (FieldModel parameter in function.Parameters)
                {
                    json.WriteStartObject();
                    json.WriteString("name", parameter.Name);
                    json.WriteString("type", Notation(schema, parameter, symbol, topLevel: true, FunctionRemedy));
                    if (function.Spec.ScriptPerSecond.Contains(parameter.Name, StringComparer.Ordinal))
                    {
                        json.WriteBoolean("perSecond", true);
                    }

                    json.WriteEndObject();
                }

                json.WriteEndArray();
            }

            if (Returns(schema, function, symbol) is string returns)
            {
                json.WriteString("returns", returns);
            }

            if (function.Spec.ScriptEffects.Count != 0)
            {
                json.WriteStartArray("effects");
                foreach (string effect in function.Spec.ScriptEffects)
                {
                    json.WriteStringValue(effect);
                }

                json.WriteEndArray();
            }

            if (function.Spec.Pure)
            {
                json.WriteBoolean("pure", true);
            }

            if (function.Spec.ScriptOneShot)
            {
                json.WriteBoolean("oneShot", true);
            }

            json.WriteEndObject();
        }

        json.WriteEndArray();
    }
}
