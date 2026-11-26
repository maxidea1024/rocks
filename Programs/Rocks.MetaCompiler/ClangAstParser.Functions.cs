using System.Text.Json;
using System.Text.RegularExpressions;

namespace Rocks.MetaCompiler;

// 리플렉션 함수(ROCKS_FUNCTION — Docs/Design/FunctionReflection.ko.md FD1·FD4·FD5·FD14) 선언 해석이다. 클래스의 CXXMethodDecl마다 자리·모양
// (static·const·noexcept·참조 한정자·삭제·기본 인자·이름 없는 매개변수·오버로드)을 보고 FunctionModel을 만든다. 매개변수·반환 형식은 쓴
// 철자를 합성 FieldModel에 담고, 정규 철자는 정규 형식 탐침(CanonicalizeAsync)이, 값 종류는 SchemaValidator가 정한다.
internal sealed partial class ClangAstParser
{
    // 함수 형식 철자(`R (A, B) const noexcept`)의 끝 한정자다.
    private static readonly Regex FunctionQualifiers = new(@"\)\s*(?<qualifiers>[^()]*)$", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    // `noexcept(true)`는 `noexcept`와 같다. 그 밖의 `noexcept(...)`(false·조건식)는 받지 않는다.
    private static readonly Regex NoexceptTrue = new(@"\bnoexcept\s*\(\s*true\s*\)", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    private static readonly Regex NoexceptExpression = new(@"\bnoexcept\s*\(", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    // 함수·매개변수 이름은 ASCII 식별자다(연산자 함수·유니코드 이름은 ID 명세·생성 thunk 이름·descriptor에 쓸 수 없다).
    private static readonly Regex AsciiIdentifier = new(@"^[A-Za-z_][A-Za-z0-9_]*\z", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    // 클래스 members 중 ROCKS_FUNCTION을 단 멤버를 해석해 model.Functions에 더한다. 그 밖의 멤버는 무시한다.
    // access는 클래스 첫 멤버의 접근 수준(class면 private)이고, private·protected 함수는 생성 thunk가 GeneratedType<T>의 friend로 부르므로
    // ROCKS_GENERATED_BODY가 있어야 한다(필드와 같다).
    private void ParseFunctions(ClassModel model, JsonElement[] members, string qualified, bool isStruct, string access, bool generatedBody)
    {
        Dictionary<string, int> overloads = new(StringComparer.Ordinal);
        foreach (JsonElement member in members)
        {
            if (Text(member, "kind") is "CXXMethodDecl" or "FunctionTemplateDecl")
            {
                string name = Text(member, "name");
                overloads[name] = overloads.GetValueOrDefault(name) + 1;
            }
        }

        foreach (JsonElement member in members)
        {
            if (Text(member, "kind") == "AccessSpecDecl")
            {
                access = Text(member, "access");
            }

            object?[]? annotation = Annotation(member, "rocks.function");
            if (annotation is null)
            {
                continue;
            }

            UpdateSite(member);
            string name = Text(member, "name");
            string symbol = qualified + "::" + name;
            if (!AsciiIdentifier.IsMatch(name))
            {
                throw new MetadataException($"{symbol}: a reflected function needs an ASCII identifier name (operators and conversion functions are not reflected)", "RMC2701",
                    "Reflect a named member function instead, for example `bool Equals(const T& other) const noexcept` for operator==.");
            }

            if (isStruct)
            {
                throw new MetadataException($"{symbol}: ROCKS_FUNCTION applies to ROCKS_CLASS member functions only", "RMC2701",
                    "Reflected functions belong to classes (like UE UFUNCTION); put library functions on a class as static member functions.");
            }

            if (annotation.Length != 1 || annotation[0] is not string specifiers)
            {
                throw new MetadataException($"{symbol}: invalid ROCKS_FUNCTION annotation arguments");
            }

            FunctionSpec spec = Specifiers.Function(specifiers, symbol, name);
            if (access != "public" && !generatedBody)
            {
                throw new MetadataException($"{symbol}: a private/protected reflected function requires ROCKS_GENERATED_BODY", "RMC2701",
                    "Make the function public or add ROCKS_GENERATED_BODY() to the class so the generated call can reach it.");
            }

            if (overloads.GetValueOrDefault(name) > 1)
            {
                throw new MetadataException($"{symbol}: a reflected function name cannot be overloaded", "RMC2702",
                    "Give the reflected function a name with no other C++ overload (the metadata compiler names its parameter types through &Class::Function).");
            }

            string qualType = member.TryGetProperty("type", out JsonElement type) ? Text(type, "qualType") : "";
            if (Flag(member, "explicitlyDeleted") || Flag(member, "isDeleted"))
            {
                throw new MetadataException($"{symbol}: a deleted function cannot be reflected", "RMC2701", "Remove ROCKS_FUNCTION from the deleted function.");
            }

            // 끝 반환형(`auto (A) const noexcept -> R`)은 `->` 앞이 한정자, 뒤가 반환이다. noexcept(true)는 noexcept로 읽는다.
            string shape = NoexceptTrue.Replace(qualType, "noexcept");
            string trailing = "";
            if (shape.LastIndexOf("->", StringComparison.Ordinal) is int arrow and >= 0 && shape.LastIndexOf(')') < arrow)
            {
                trailing = shape[(arrow + 2)..].Trim();
                shape = shape[..arrow].TrimEnd();
            }

            if (NoexceptExpression.IsMatch(shape))
            {
                throw new MetadataException($"{symbol}: a reflected function must be unconditionally noexcept", "RMC2701",
                    "Write plain noexcept (or noexcept(true)); the generated call cannot propagate exceptions.");
            }

            string qualifiers = FunctionQualifiers.Match(shape) is { Success: true } match ? match.Groups["qualifiers"].Value : "";
            if (!Regex.IsMatch(qualifiers, @"\bnoexcept\b", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1)))
            {
                throw new MetadataException($"{symbol}: a reflected function must be noexcept", "RMC2701",
                    "Declare the function noexcept; the engine reports failures with Result instead of exceptions.");
            }

            if (qualifiers.Contains('&', StringComparison.Ordinal))
            {
                throw new MetadataException($"{symbol}: a reference-qualified member function cannot be reflected", "RMC2701",
                    "Remove the & or && qualifier; the generated call invokes the function on the resolved object.");
            }

            bool isStatic = Text(member, "storageClass") == "static";
            bool isConst = Regex.IsMatch(qualifiers, @"\bconst\b", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));
            FunctionModel function = new()
            {
                Id = Identity("function", symbol),
                Site = currentSite,
                Name = name,
                Owner = qualified,
                Spec = spec,
                IsStatic = isStatic,
                IsConst = isConst
            };
            foreach (JsonElement parameter in Children(member).Where(child => Text(child, "kind") == "ParmVarDecl"))
            {
                string parameterName = Text(parameter, "name");
                if (parameterName.Length == 0)
                {
                    throw new MetadataException($"{symbol}: every parameter of a reflected function needs a name", "RMC2701",
                        "Name the parameter; scripts and bindings refer to arguments by name.");
                }

                if (!AsciiIdentifier.IsMatch(parameterName))
                {
                    throw new MetadataException($"{symbol}: parameter name '{parameterName}' must be an ASCII identifier", "RMC2701",
                        "Use [A-Za-z_][A-Za-z0-9_]* for reflected parameter names; descriptors and bindings store them as ASCII.");
                }

                if (parameter.TryGetProperty("init", out _))
                {
                    throw new MetadataException($"{symbol}: parameter '{parameterName}' has a default argument", "RMC2701",
                        "Remove the default argument; reflected calls pass every argument (UE keeps defaults only as editor metadata).");
                }

                string written = parameter.TryGetProperty("type", out JsonElement parameterType) ? Text(parameterType, "qualType") : "";
                function.Parameters.Add(Slot(function, parameterName, written));
            }

            if (function.Parameters.Count > FunctionRules.MaxParameters)
            {
                throw new MetadataException($"{symbol}: a reflected function takes at most {FunctionRules.MaxParameters} parameters", "RMC2701",
                    "Group related arguments in a reflected struct parameter.");
            }

            string returns = trailing.Length != 0 ? trailing
                : shape.Length != 0 && shape.IndexOf('(', StringComparison.Ordinal) is int open and > 0 ? shape[..open].Trim() : "";
            // void를 typedef로 쓴 반환과 void로 추론된 반환(`auto`)은 정규 형식 탐침 뒤 SchemaValidator가 결과 칸을 지운다. 추론 반환은 Clang이
            // 정의를 보고 정한 형식(정의가 클래스 밖이면 탐침이 묻는 `&Owner::Func`의 형식)이다.
            if (returns != "void")
            {
                function.Result = Slot(function, "", returns);
            }

            model.Functions.Add(function);
            if (model.Functions.Count > FunctionRules.MaxFunctions)
            {
                throw new MetadataException($"{qualified}: a class declares at most {FunctionRules.MaxFunctions} reflected functions", "RMC2701",
                    "Split the functions across classes or components.");
            }
        }
    }

    // 매개변수·반환 칸의 합성 필드다(이름·소유자·쓴 철자 — 값 종류와 대상은 SchemaValidator가 채운다, ID는 쓰지 않는다).
    private static FieldModel Slot(FunctionModel function, string name, string spelling) => new()
    {
        Id = "",
        Site = function.Site,
        Name = name,
        Owner = function.Owner,
        NativeSpelling = spelling,
        Spec = new(false, false, "", "")
    };
}
