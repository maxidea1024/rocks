using System.Text.Json;

namespace Rocks.MetaCompiler;

// Sparks binding schema의 이벤트(`events` — Docs/Design/SparksBinding.ko.md EI1c B20–B26)다. ROCKS_PROPERTY(ScriptAssignable) 이벤트 필드만
// 싣고, 이름은 필드 규칙(B22 — ScriptName 또는 도구 이름의 첫 글자를 대문자로), 인자 표기는 함수 매개변수와 같은 B4 표의 최상위 열이다.
internal static partial class SparksBinding
{
    private const string EventRemedy = "remove ScriptAssignable from the event";

    // `X.Changed`는 늘 필드 X의 변경이라 이벤트 이름이 될 수 없다(SL 5.2 — S0809).
    private const string ChangedEventName = "Changed";

    // 노출 이벤트 하나의 규칙이다: 노출 타입의 object 종류, EditorOnly 아님, 이름·예약·hook 충돌, 인자 이름·표기.
    private static void CheckEvent(SchemaModel schema, ClassModel owner, FieldModel field, SparksNames names)
    {
        // 판정 순서는 필드(CheckField)와 같다: EditorOnly 타입 → 노출하지 않은 타입 → EditorOnly 이벤트 → asset 종류.
        string symbol = owner.Name + "::" + field.Name;
        if (owner.EditorOnly)
        {
            throw new MetadataException($"{symbol}: an event of the EditorOnly type {owner.Name} cannot be visible to Sparks", "RMC2607",
                "Editor-only types are not in shipping builds; remove ScriptAssignable from the event.").At(field.Site);
        }

        if (!owner.ScriptType)
        {
            throw new MetadataException($"{symbol}: the event is visible to Sparks but {owner.Name} is not a Sparks type", "RMC2608",
                "Add ScriptType (or ScriptExtensible) to the type, or remove ScriptAssignable from the event.").At(field.Site);
        }

        if (field.Spec.EditorOnly)
        {
            throw new MetadataException($"{symbol}: an EditorOnly event cannot be visible to Sparks", "RMC2607",
                "Editor-only events are not in shipping builds; remove ScriptAssignable or EditorOnly.").At(field.Site);
        }

        if (Kind(schema, owner) != "object")
        {
            throw new MetadataException($"{symbol}: an event of the asset type {owner.Name} cannot be visible to Sparks", "RMC2607",
                "Assets are shared and do not expose events to scripts (SL 5.5); remove ScriptAssignable from the event.").At(field.Site);
        }

        string name = FieldName(field) ?? throw new MetadataException(
            $"{symbol}: Sparks event name from '{field.EffectiveToolName}' must be letters and digits; name it with ScriptName", "RMC2604",
            "Add ScriptName = \"PascalName\" (SL 10.3: underscores other than a trailing one need an explicit Sparks name).").At(field.Site);
        if (name == ChangedEventName)
        {
            throw new MetadataException($"{symbol}: Sparks event name '{name}' is reserved for field changes", "RMC2606",
                "In Sparks, X.Changed always means a change of the field X (SL 5.2); expose the event under another name with ScriptName = \"...\".").At(field.Site);
        }

        if (names.IsReserved(name))
        {
            throw new MetadataException($"{symbol}: Sparks event name '{name}' is a reserved Sparks name", "RMC2606",
                "Expose the event under another name with ScriptName = \"...\" (prelude, built-in, core engine type and context names cannot be member names).").At(field.Site);
        }

        if (names.HookFor(name) is string hook)
        {
            throw new MetadataException($"{symbol}: Sparks event name '{name}' collides with the lifecycle hook '{hook}'", "RMC2606",
                "Expose the event under another name with ScriptName = \"...\" (engine events are named in the past tense, e.g. Destroyed).").At(field.Site);
        }

        foreach (FieldModel parameter in field.EventParameters)
        {
            if (names.Keywords.Contains(parameter.Name))
            {
                throw new MetadataException($"{symbol}: event argument name '{parameter.Name}' is a Sparks keyword", "RMC2606",
                    "Rename the argument in Params = \"...\".").At(field.Site);
            }

            _ = Notation(schema, parameter, $"{symbol}: argument '{parameter.Name}'", topLevel: true, EventRemedy);
        }
    }

    // 노출 이벤트인지다(이벤트 필드 + ScriptAssignable).
    private static bool IsScriptEvent(FieldModel field) => field.IsEvent && field.Spec.ScriptAssignable;

    // descriptor scriptName에 쓰는 노출 이벤트의 최종 Sparks 이름이다(B25 — 노출하지 않으면 "", 이름 규칙은 Resolve가 먼저 검사했다).
    public static string ScriptEventName(FieldModel field) => IsScriptEvent(field) ? FieldName(field) ?? "" : "";

    // 타입의 자기 노출 이벤트를 선언 순서로 `events`에 쓴다(B26 — 없으면 key가 없다).
    private static void WriteEvents(Utf8JsonWriter json, SchemaModel schema, ClassModel type)
    {
        List<FieldModel> events = type.OwnFields.Where(IsScriptEvent).ToList();
        if (events.Count == 0)
        {
            return;
        }

        json.WriteStartArray("events");
        foreach (FieldModel field in events)
        {
            string symbol = type.Name + "::" + field.Name;
            json.WriteStartObject();
            json.WriteString("name", FieldName(field));
            json.WriteString("id", field.Id);
            if (field.EventParameters.Count != 0)
            {
                json.WriteStartArray("parameters");
                foreach (FieldModel parameter in field.EventParameters)
                {
                    json.WriteStartObject();
                    json.WriteString("name", parameter.Name);
                    json.WriteString("type", Notation(schema, parameter, $"{symbol}: argument '{parameter.Name}'", topLevel: true, EventRemedy));
                    json.WriteEndObject();
                }

                json.WriteEndArray();
            }

            json.WriteEndObject();
        }

        json.WriteEndArray();
    }
}
