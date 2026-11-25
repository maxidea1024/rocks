#include "PreludeMembers.h"

// 부록 B 시그니처(Docs/Design/SparksLanguage 3.8절)의 표다. 순서는 문서 순서이며 심볼 번호를
// 정한다(결정적이다).

namespace rocks::sparks::detail {

namespace {

using F = PreludeMemberForm;
using C = PreludeCondition;

constexpr PreludeMember Members[] = {
    // Math — `$N`·`$S`·`$I`·`$V`는 오버로딩 자리표시다.
    {"Math", "Min", F::Function, "(a: $N, b: $N): $N"},
    {"Math", "Max", F::Function, "(a: $N, b: $N): $N"},
    {"Math", "Abs", F::Function, "(x: $S): $S"},
    {"Math", "Sign", F::Function, "(x: $S): $S"},
    {"Math", "Clamp", F::Function, "(x: $N, lo: $N, hi: $N): $N"},
    {"Math", "Div", F::Function, "(a: $I, b: $I): $I"},
    {"Math", "Floor", F::Function, "(x: float): float"},
    {"Math", "Ceil", F::Function, "(x: float): float"},
    {"Math", "Round", F::Function, "(x: float): float"},
    {"Math", "Trunc", F::Function, "(x: float): float"},
    {"Math", "FloorToInt", F::Function, "(x: float): int"},
    {"Math", "CeilToInt", F::Function, "(x: float): int"},
    {"Math", "RoundToInt", F::Function, "(x: float): int"},
    {"Math", "TruncToInt", F::Function, "(x: float): int"},
    {"Math", "Sqrt", F::Function, "(x: float): float"},
    {"Math", "Pow", F::Function, "(x: float, y: float): float"},
    {"Math", "Exp", F::Function, "(x: float): float"},
    {"Math", "Ln", F::Function, "(x: float): float"},
    {"Math", "Sin", F::Function, "(x: float): float"},
    {"Math", "Cos", F::Function, "(x: float): float"},
    {"Math", "Tan", F::Function, "(x: float): float"},
    {"Math", "Asin", F::Function, "(x: float): float"},
    {"Math", "Acos", F::Function, "(x: float): float"},
    {"Math", "Atan", F::Function, "(x: float): float"},
    {"Math", "Atan2", F::Function, "(y: float, x: float): float"},
    {"Math", "Radians", F::Function, "(degrees: float): float"},
    {"Math", "Degrees", F::Function, "(radians: float): float"},
    {"Math", "Lerp", F::Function, "(a: float, b: float, t: float): float"},
    {"Math", "InverseLerp", F::Function, "(a: float, b: float, value: float): float"},
    {"Math", "EaseIn", F::Function, "(t: float): float"},
    {"Math", "EaseOut", F::Function, "(t: float): float"},
    {"Math", "EaseInOut", F::Function, "(t: float): float"},
    // NearlyEqual은 float 판과 벡터 판 둘이다($V가 있으면 벡터 판으로 맞춘다).
    {"Math", "NearlyEqual", F::Function, "(a: $F, b: $F, tolerance: float): bool"},
    {"Math", "Scale", F::Function, "(a: $V, b: $V): $V"},
    {"Math", "Pi", F::Constant, "float"},

    // 벡터 공통(vec2·vec3·vec4에 `$V`를 그 타입으로 바꿔 넣는다).
    {"vec2", "X", F::Field, "float"},
    {"vec2", "Y", F::Field, "float"},
    {"vec3", "X", F::Field, "float"},
    {"vec3", "Y", F::Field, "float"},
    {"vec3", "Z", F::Field, "float"},
    {"vec4", "X", F::Field, "float"},
    {"vec4", "Y", F::Field, "float"},
    {"vec4", "Z", F::Field, "float"},
    {"vec4", "W", F::Field, "float"},
    {VectorFamilyOwner, "Dot", F::Method, "(other: $V): float"},
    {VectorFamilyOwner, "Length", F::Method, "(): float"},
    {VectorFamilyOwner, "LengthSquared", F::Method, "(): float"},
    {VectorFamilyOwner, "Distance", F::Method, "(other: $V): float"},
    {VectorFamilyOwner, "Normalize", F::Method, "(): $V"},
    {"vec3", "Cross", F::Method, "(other: vec3): vec3"},

    {"quat", "X", F::Field, "float"},
    {"quat", "Y", F::Field, "float"},
    {"quat", "Z", F::Field, "float"},
    {"quat", "W", F::Field, "float"},
    {"quat", "Rotate", F::Method, "(v: vec3): vec3"},
    {"quat", "Inverse", F::Method, "(): quat"},
    {"quat", "ToRotator", F::Method, "(): Rotator"},
    {"quat", "FromAxisAngle", F::Static, "(axis: vec3, radians: float): quat"},

    {"Rotator", "Pitch", F::Field, "float"},
    {"Rotator", "Yaw", F::Field, "float"},
    {"Rotator", "Roll", F::Field, "float"},
    {"Rotator", "ToQuat", F::Method, "(): quat"},
    {"Rotator", "Forward", F::Method, "(): vec3"},
    {"Rotator", "Right", F::Method, "(): vec3"},
    {"Rotator", "Up", F::Method, "(): vec3"},

    {"Transform", "Location", F::Field, "vec3"},
    {"Transform", "Rotation", F::Field, "quat"},
    {"Transform", "Scale", F::Field, "vec3"},
    {"Transform", "TransformPosition", F::Method, "(v: vec3): vec3"},
    {"Transform", "InverseTransformPosition", F::Method, "(v: vec3): vec3"},
    {"Transform", "TransformVector", F::Method, "(v: vec3): vec3"},
    {"Transform", "ToMatrix", F::Method, "(): mat4"},

    {"mat4", "TransformPosition", F::Method, "(v: vec3): vec3"},

    {"Color", "R", F::Field, "float"},
    {"Color", "G", F::Field, "float"},
    {"Color", "B", F::Field, "float"},
    {"Color", "A", F::Field, "float"},

    {"string", "Length", F::Property, "int"},
    {"string", "CharCount", F::Method, "(): int"},
    {"string", "Contains", F::Method, "(text: string): bool"},
    {"string", "StartsWith", F::Method, "(text: string): bool"},
    {"string", "EndsWith", F::Method, "(text: string): bool"},
    {"string", "Find", F::Method, "(text: string): int?"},
    {"string", "Slice", F::Method, "(start: int, end: int): string"},
    {"string", "Split", F::Method, "(separator: string): Array<string>"},
    {"string", "Trim", F::Method, "(): string"},
    {"string", "ToUpperAscii", F::Method, "(): string"},
    {"string", "ToLowerAscii", F::Method, "(): string"},

    {"Format", "Fixed", F::Function, "(value: float, decimals: int): string"},
    {"Format", "Padded", F::Function, "(value: int, width: int): string"},
    {"Parse", "Int", F::Function, "(text: string): int?"},
    {"Parse", "Float", F::Function, "(text: string): float?"},

    {"Array", "Add", F::Method, "(value: T)"},
    {"Array", "Insert", F::Method, "(index: int, value: T)"},
    {"Array", "RemoveAt", F::Method, "(index: int)"},
    {"Array", "Clear", F::Method, "()"},
    {"Array", "Reverse", F::Method, "()"},
    {"Array", "Contains", F::Method, "(value: T): bool", C::Equatable},
    {"Array", "IndexOf", F::Method, "(value: T): int?", C::Equatable},
    {"Array", "Remove", F::Method, "(value: T): bool", C::Equatable},
    {"Array", "Sort", F::Method, "()", C::Ordered},
    {"Array", "Count", F::Property, "int", C::ValueElement},
    {"Array", "SlotCount", F::Property, "int", C::ObjectElement},
    {"Array", "AliveCount", F::Method, "(): int", C::ObjectElement},
    {"Array", "Compact", F::Method, "()", C::ObjectElement},

    {"Map", "Count", F::Property, "int"},
    {"Map", "ContainsKey", F::Method, "(key: K): bool"},
    {"Map", "Remove", F::Method, "(key: K): bool"},
    {"Map", "Keys", F::Method, "(): Array<K>"},
    {"Map", "Values", F::Method, "(): Array<V>"},
    {"Map", "Clear", F::Method, "()"},

    {"Set", "Count", F::Property, "int"},
    {"Set", "Add", F::Method, "(value: T): bool"},
    {"Set", "Remove", F::Method, "(value: T): bool"},
    {"Set", "Contains", F::Method, "(value: T): bool"},
    {"Set", "Clear", F::Method, "()"},

    {"Result", "Value", F::Property, "T?", C::NotVoid},
    {"Result", "Error", F::Property, "Error?"},

    {"Log", "Info", F::Function, "(message: string)"},
    {"Log", "Warning", F::Function, "(message: string)"},
    {"Log", "Error", F::Function, "(message: string)"},
    {"Log", "Debug", F::Function, "(message: string)"},
    {"", "Assert", F::Function, "(condition: bool, message: string)"},

    {"Random", "Int", F::Function, "(min: int, maxExclusive: int): int"},
    {"Random", "Float", F::Function, "(min: float, max: float): float"},
    {"Random", "Chance", F::Function, "(probability: float): bool"},

    {TimeTypeName, "Delta", F::Property, "float"},
    {TimeTypeName, "FixedDelta", F::Property, "float"},
    {TimeTypeName, "UiDelta", F::Property, "float"},
    {TimeTypeName, "GameSeconds", F::Property, "float"},
    {TimeTypeName, "Frame", F::Property, "int"},

    {"Clock", "Game", F::Case, ""},
    {"Clock", "UI", F::Case, ""},
    {"", "Delay", F::Function, "(seconds: float, clock: Clock): await void"},
    {"", "NextTick", F::Function, "(): await void"},
    {"", "NextFixedTick", F::Function, "(): await void"},
    // All·Race는 대기 1개 이상을 받는다(타입 검사가 따로 본다 — 3.10절, 6.1절).
    {"", "All", F::Function, "(...): await void"},
    {"", "Race", F::Function, "(...): await void"},

    {"Assets", "LoadAll", F::Function, "<T>(assets: Array<Soft<T>>): LoadRequest<Array<T>>"},
    {"Assets", "Find", F::Function, "<T>(path: string): Soft<T>?"},
    {"Soft", "Load", F::Method, "(): LoadRequest<T>"},
    {"LoadRequest", "IsDone", F::Property, "bool"},
    {"LoadRequest", "Progress", F::Property, "float"},

    {"Tag", "Find", F::Static, "(name: string): Tag?"},
    {"TagSet", "Has", F::Method, "(tag: Tag): bool"},
    {"TagSet", "HasAny", F::Method, "(tags: TagSet): bool"},
    {"TagSet", "HasAll", F::Method, "(tags: TagSet): bool"},
    {"TagSet", "Add", F::Method, "(tag: Tag)"},
    {"TagSet", "Remove", F::Method, "(tag: Tag)"},

    {"InputAction", "Value", F::Property, "vec3"},
    {"InputAction", "Started", F::Event, ""},
    {"InputAction", "Triggered", F::Event, ""},
    {"InputAction", "Ongoing", F::Event, ""},
    {"InputAction", "Completed", F::Event, ""},
    {"InputAction", "Cancelled", F::Event, ""},

    {"Text", "Find", F::Static, "(key: string): Text?"},
    {"CVars", "Find", F::Function, "<T>(name: string): CVarRef<T>?"},
    {"CVarRef", "Value", F::Property, "T"},
    {"Settings", "Get", F::Function, "<T>(): T"},
    {"Class", "Create", F::Method, "(): T"},
};

} // namespace

Span<const PreludeMember> PreludeMembers() noexcept {
    return Span<const PreludeMember>(Members, sizeof(Members) / sizeof(Members[0]));
}

} // namespace rocks::sparks::detail
