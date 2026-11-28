using System.Text.Json;

namespace Rocks.MetaCompiler;

// 생성 setter·getter(ROCKS_SETTER·ROCKS_GETTER — Docs/Design/ChangeNotification.ko.md CN-D13) 선언 해석이다. 매크로가 annotation(대상 필드
// 이름)을 단 멤버 함수를 선언하므로 클래스의 CXXMethodDecl마다 자리와 이름만 보고 AccessorModel을 만든다. 대상 필드의 종류·중복·Apply 검사는
// 값 종류가 정해진 뒤 SchemaValidator가 한다(RMC2903·RMC2904).
internal sealed partial class ClangAstParser
{
    // 클래스 members 중 ROCKS_SETTER·ROCKS_GETTER가 선언한 함수를 model.Accessors에 더한다.
    private void ParseAccessors(ClassModel model, JsonElement[] members, string qualified, bool isStruct)
    {
        foreach (JsonElement member in members)
        {
            if (Text(member, "kind") != "CXXMethodDecl")
            {
                continue;
            }

            object?[]? setter = Annotation(member, "rocks.setter");
            object?[]? getter = Annotation(member, "rocks.getter");
            if (setter is null && getter is null)
            {
                continue;
            }

            UpdateSite(member);
            string name = Text(member, "name");
            string symbol = qualified + "::" + name;
            string macro = setter is not null ? "ROCKS_SETTER" : "ROCKS_GETTER";
            if (setter is not null && getter is not null)
            {
                throw new MetadataException($"{symbol}: a function is either ROCKS_SETTER or ROCKS_GETTER, not both", "RMC2903",
                    "Declare the setter and the getter as two functions with their own macros.");
            }

            if (isStruct)
            {
                throw new MetadataException($"{symbol}: {macro} applies to ROCKS_CLASS fields only", "RMC2903",
                    "A struct value is written through the class field that holds it; put the setter on that class field.");
            }

            object?[] arguments = setter ?? getter!;
            if (arguments.Length != 1 || arguments[0] is not string field || !AsciiIdentifier.IsMatch(field))
            {
                throw new MetadataException($"{symbol}: {macro} needs the plain name of a field of this class", "RMC2903",
                    $"Write {macro}({(name.Length == 0 ? "Name" : name)}, field) with the C++ name of a ROCKS_PROPERTY field declared above it.");
            }

            if (!AsciiIdentifier.IsMatch(name))
            {
                throw new MetadataException($"{symbol}: the {macro} function name must be an ASCII identifier", "RMC2903",
                    "Use [A-Za-z_][A-Za-z0-9_]* for the function name.");
            }

            model.Accessors.Add(new AccessorModel(name, field, setter is not null, currentSite));
        }
    }
}
