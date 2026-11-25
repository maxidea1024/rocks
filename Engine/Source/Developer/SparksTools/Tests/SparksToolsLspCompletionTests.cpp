#include "SparksToolsLspTestSupport.h"

#include <SparksCompiler/Lexer.h>

// 언어 서버 자동완성 시험이다(Docs/Design/SparksTools.ko.md 11.6절·11.8절 — LC8f2): 자리마다 후보
// golden(이름·종류·순위·detail), core 숨김, Unity 별칭, 주석·문자열·선언 이름, prefix fuzz(토큰
// 경계마다 — 결정적), 할당 실패.
using namespace rocks;
namespace sparks_tools_lsp_completion_tests_file {

using namespace rocks::test::sparks_lsp;
using rocks::sparks::DiagnosticList;
using rocks::sparks::SourceSet;
using rocks::sparks::Token;
using rocks::sparks::Tokenize;

namespace {

// full 패키지 사례 원문이다(ASCII). `@@top`·`@@member`·`@@body`·`@@base` 자리에 사례 조각을 넣고
// 나머지 자리는 기본 조각으로 채운다. 조각의 `|`가 커서다.
constexpr StringView LspCompletionTemplate = R"(import Lib;
import Tools.Kit as Kit;
@@top

event Actor.Interacted();
event World.Dawn();

open class Panel : ScriptableComponent {
    var Width: int = 1;
    event Slid();

    open fn Act() {
    }
}

class Gate : Panel {
    event Closed();

    override fn Act() {
        @@gate
    }
}

class Door : @@base {
    var Swing: float = 1.0;
    var Next: Door?;
    var Arm: SceneComponent?;
    var Points: Array<Point> = [];
    var Marks: Array<int> = [];
    var Guards: Array<Guard> = [];
    var Name: string = "door";
    let Label: string = "door";
    event Opened(by: Actor);
    @@member

    on Start() {
        let total = Swing + 1.0;
        Log.Info("${total}");
        @@start
    }

    fn Use(d: Door, count: int): int {
        @@body
        return count;
    }
}

class Guard : Character {
    @@guard

    fn Shout(other: Guard) {
        @@shout
    }
}

class Shade : ScriptableComponent {
}

struct Point {
    var X: int = 0;
}

enum Mood {
    Calm,
    Angry
}

const Limit: int = 3;

fn Top(p: int): int {
    return p + Limit;
}
)";

constexpr StringView LspCompletionLib = "fn G(): int {\n    return 1;\n}\n";
constexpr StringView LspCompletionKit =
    "fn K(): int {\n    return 2;\n}\n\nstruct Tool {\n    var A: int = 0;\n}\n";
constexpr StringView LspCompletionBaseManifest =
    "{ name: \"Basic\", sparks: 1, profile: \"core\" }\n";
constexpr StringView LspCompletionGameManifest =
    "{ name: \"Game\", sparks: 1, profile: \"full\", dependencies: [\"Shared\"] }\n";
constexpr StringView LspCompletionSharedManifest =
    "{ name: \"Shared\", sparks: 1, profile: \"full\" }\n";
constexpr StringView LspCompletionShared = "fn Beat(): int {\n    return 1;\n}\n";
// import하지 않은 모듈의 확장 이벤트(보이지 않는다 — S0815).
constexpr StringView LspCompletionSiren = "event Actor.Alarmed();\n";

// 자리 하나의 기본 조각이다(사례가 그 자리를 쓰지 않을 때).
struct LspSlot {
    StringView name;
    StringView fill;
};

constexpr LspSlot LspCompletionSlots[] = {
    {"@@top", ""},  {"@@member", ""}, {"@@body", ""},  {"@@start", ""},
    {"@@gate", ""}, {"@@guard", ""},  {"@@shout", ""}, {"@@base", "ScriptableComponent"}};

// 사례 하나: 자리 이름·조각(`|`가 커서)·core 패키지(Basic)에서 여는지.
struct LspCompletionProbe {
    StringView label;
    StringView slot;
    StringView snippet;
    bool core = false;
};

constexpr LspCompletionProbe LspCompletionProbes[] = {
    {"member of a class value", "@@body", "_ = d.|;"},
    {"member prefix", "@@body", "_ = d.Sw|;"},
    {"member in the middle of a name", "@@body", "_ = d.Sw|ing;"},
    {"member after ?.", "@@body", "_ = Next?.|;"},
    {"member of string", "@@body", "_ = Name.|;"},
    {"member of Array<struct> (no == or <)", "@@body", "_ = Points.|;"},
    {"member of Array<int>", "@@body", "_ = Marks.|;"},
    {"enum cases", "@@body", "_ = Mood.|;"},
    {"prelude enum cases", "@@body", "_ = Clock.|;"},
    {"prelude namespace", "@@body", "_ = Log.|;"},
    {"import alias", "@@body", "_ = Kit.|;"},
    {"context name", "@@body", "_ = World.|;"},
    {"engine enum", "@@body", "_ = CollisionChannel.|;"},
    {"engine object members (visible extension events only)", "@@body", "_ = Owner.|;"},
    {"Unity GetComponent", "@@body", "_ = Owner.GetC|;"},
    {"override seen once", "@@gate", "_ = this.|;"},
    {"scope prefix", "@@body", "_ = co|;"},
    {"scope prefix (engine type)", "@@body", "_ = Sph|;"},
    {"keyword prefix", "@@body", "ret|"},
    {"digit prefix", "@@body", "_ = 1|;"},
    {"underscore prefix", "@@body", "_ = _|;"},
    {"scope at statement start", "@@body", "|"},
    {"local after its declaration", "@@start", "_ = tot|;"},
    {"local in its own initializer", "@@start", "let tally = tal|;"},
    {"for binder in the body", "@@start", "for (item in Marks) { _ = it|; }"},
    {"for binder after the loop", "@@start", "for (item in Marks) { }it|"},
    {"if-let binder", "@@start", "if (let found = Next) { _ = fo|; }"},
    {"comment", "@@body", "// d.|"},
    {"block comment", "@@body", "/* d.| */"},
    {"after a block comment", "@@body", "/* d. */_ = cou|;"},
    {"doc comment", "@@member", "/// d.|"},
    {"string", "@@body", "Log.Info(\"d.|\");"},
    {"unclosed string", "@@body", "Log.Info(\"d.|"},
    {"verbatim string", "@@body", "_ = @\"d.|\";"},
    {"interpolation", "@@body", "Log.Info(\"${co|}\");"},
    {"string inside an interpolation", "@@body", "Log.Info(\"${\"d.|\"}\");"},
    {"type position prefix", "@@member", "var Extra: Mo|;"},
    {"type after alias", "@@member", "var Extra: Kit.|;"},
    {"type argument", "@@member", "var Extra: Array<Po|>;"},
    {"declaration name", "@@member", "var Sw|"},
    {"on (hooks, fields, World)", "@@member", "on |"},
    {"on Unity prefix", "@@member", "on Upd|"},
    {"on object field", "@@member", "on Next.|"},
    {"on field of a field", "@@member", "on Next.Swing.|"},
    {"on four names", "@@member", "on Next.Next.Swing.|"},
    {"on string field", "@@member", "on Name.|"},
    {"on let field", "@@member", "on Label.|"},
    {"on engine object field", "@@member", "on Arm.|"},
    {"on engine changed field", "@@member", "on Arm.RelativeLocation.|"},
    {"on engine field without changed", "@@member", "on Arm.RelativeRotation.|"},
    {"on array of objects", "@@member", "on Guards.|"},
    {"on World", "@@member", "on World.|"},
    {"on this (not an Actor)", "@@member", "on this.|"},
    {"on (Actor class)", "@@guard", "on |"},
    {"on this (Actor class)", "@@guard", "on this.|"},
    {"emit (own events only)", "@@gate", "emit |"},
    {"emit (Actor class)", "@@shout", "emit |"},
    {"emit this", "@@shout", "emit this.|"},
    {"emit to a local", "@@shout", "emit other.|"},
    {"attribute", "@@member", "@|"},
    {"base list first (full)", "@@base", "|"},
    {"base list later (full)", "@@base", "ScriptableComponent, |"},
    {"base list skips the class itself", "@@base", "D|"},
    {"import", "@@top", "import |"},
    {"import segment", "@@top", "import Tools.|"},
    {"import dependency", "@@top", "import Shared.|"},
    {"core: base list first", "@@base", "|", true},
    {"core: base list later", "@@base", "ScriptableComponent, |", true},
    {"core: attribute", "@@member", "@|", true},
    {"core: on", "@@member", "on |", true},
    {"core: keyword o", "@@body", "o|", true},
    {"core: prelude Cl", "@@body", "_ = Cl|;", true},
};

// 사례 원문과 커서 위치다.
struct LspCompletionCase {
    explicit LspCompletionCase(Allocator& allocator) noexcept : text(allocator) {
    }

    String text;
    LspTestPlace place;
};

LspCompletionCase LspMakeCase(Allocator& allocator, StringView slot, StringView snippet) noexcept {
    LspCompletionCase made(allocator);
    StringView rest = LspCompletionTemplate;
    for (;;) {
        usize at = StringView::NotFound;
        const LspSlot* found = nullptr;
        for (const LspSlot& candidate : LspCompletionSlots) {
            const usize where = rest.Find(candidate.name, StringComparison::CaseSensitive);
            if (where != StringView::NotFound && (at == StringView::NotFound || where < at)) {
                at = where;
                found = &candidate;
            }
        }

        if (found == nullptr) {
            CHECK(made.text.TryAppend(rest));
            break;
        }

        CHECK(made.text.TryAppend(StringView(rest.Data(), at)));
        const StringView piece = found->name == slot ? snippet : found->fill;
        for (usize index = 0; index < piece.ByteCount(); ++index) {
            if (piece.Data()[index] == '|') {
                // 커서 = 지금까지 쓴 글자의 끝.
                LspTestPlace place;
                for (const char value : made.text.View()) {
                    if (value == '\n') {
                        ++place.line;
                        place.character = 0;
                    } else {
                        ++place.character;
                    }
                }

                made.place = place;
                continue;
            }

            CHECK(made.text.TryAppend(StringView(piece.Data() + index, 1)));
        }

        rest = StringView(rest.Data() + at + found->name.ByteCount(),
                          rest.ByteCount() - at - found->name.ByteCount());
    }

    return made;
}

// 자동완성 결과를 한 줄에 항목 하나로: `<label> <kind> <sortText> | <detail>[ | filter <f>]`.
void LspAppendCompletionText(Allocator& allocator, StringView result, String& out) noexcept {
    if (result.IsEmpty()) {
        CHECK(out.TryAppend("(none)\n"));
        return;
    }

    JsonDocument document(allocator);
    REQUIRE(document.Parse(result).Succeeded());
    const JsonValue root = document.Root();
    if (root.Find("isIncomplete").GetBool()) {
        CHECK(out.TryAppend("(incomplete)\n"));
    }

    const JsonValue items = root.Find("items");
    if (items.Size() == 0) {
        CHECK(out.TryAppend("(empty)\n"));
    }

    for (usize at = 0; at < items.Size(); ++at) {
        const JsonValue item = items.At(at);
        StringBuilder<256> line(allocator);
        line.AppendFormat("{} {} {} | {}", item.Find("label").GetString(),
                          item.Find("kind").TryGetInt64().Value(),
                          item.Find("sortText").GetString(), item.Find("detail").GetString());
        if (item.Find("filterText").IsValid()) {
            line.AppendFormat(" | filter {}", item.Find("filterText").GetString());
        }

        line.Append("\n");
        CHECK(!line.HasFailed());
        CHECK(out.TryAppend(line.View()));
    }
}

// 시험 작업 공간: full 패키지 Game(Main·Lib·Tools/Kit)과 core 패키지 Basic(Main).
struct LspCompletionWorkspace {
    explicit LspCompletionWorkspace(Allocator& allocator) noexcept
        : folder(allocator), schema(LspSchemaFolder(allocator)), root(folder.UriOf("")),
          game(folder.UriOf("Game/Main.spark")), basic(folder.UriOf("Basic/Main.spark")) {
        const LspCompletionCase base = LspMakeCase(allocator, "", "");
        CHECK(folder.Write("Game/SparksPackage.json5", LspCompletionGameManifest));
        CHECK(folder.Write("Game/Lib.spark", LspCompletionLib));
        CHECK(folder.Write("Game/Tools/Kit.spark", LspCompletionKit));
        CHECK(folder.Write("Game/Tools/Siren.spark", LspCompletionSiren));
        CHECK(folder.Write("Shared/SparksPackage.json5", LspCompletionSharedManifest));
        CHECK(folder.Write("Shared/Util/Beat.spark", LspCompletionShared));
        CHECK(folder.Write("Game/Main.spark", base.text.View()));
        CHECK(folder.Write("Basic/SparksPackage.json5", LspCompletionBaseManifest));
        CHECK(folder.Write("Basic/Main.spark", base.text.View()));
    }

    LspFolder folder;
    String schema;
    String root;
    String game;
    String basic;
};

// 자리마다 후보 golden이다. core 사례는 같은 원문을 core 패키지 Basic에서 연다(Basic에는 Lib·Kit이
// 없어 import는 풀리지 않는다 — core 숨김만 본다).
constexpr StringView LspCompletionGolden = R"(== member of a class value
Arm 5 1Arm | var Door.Arm: SceneComponent?
DescribeState 2 1DescribeState | fn Rocks.ScriptableComponent.DescribeState(detailed: bool): string
Guards 5 1Guards | var Door.Guards: Array<Guard>
Label 5 1Label | let Door.Label: string
Marks 5 1Marks | var Door.Marks: Array<int>
Name 5 1Name | var Door.Name: string
Next 5 1Next | var Door.Next: Door?
Opened 23 1Opened | event Door.Opened(by: Actor)
Owner 5 1Owner | let Rocks.Component.Owner: Actor
Points 5 1Points | var Door.Points: Array<Point>
Swing 5 1Swing | var Door.Swing: float
Use 2 1Use | fn Door.Use(d: Door, count: int): int
== member prefix
Swing 5 1Swing | var Door.Swing: float
== member in the middle of a name
Swing 5 1Swing | var Door.Swing: float
== member after ?.
Arm 5 1Arm | var Door.Arm: SceneComponent?
DescribeState 2 1DescribeState | fn Rocks.ScriptableComponent.DescribeState(detailed: bool): string
Guards 5 1Guards | var Door.Guards: Array<Guard>
Label 5 1Label | let Door.Label: string
Marks 5 1Marks | var Door.Marks: Array<int>
Name 5 1Name | var Door.Name: string
Next 5 1Next | var Door.Next: Door?
Opened 23 1Opened | event Door.Opened(by: Actor)
Owner 5 1Owner | let Rocks.Component.Owner: Actor
Points 5 1Points | var Door.Points: Array<Point>
Swing 5 1Swing | var Door.Swing: float
Use 2 1Use | fn Door.Use(d: Door, count: int): int
== member of string
CharCount 2 1CharCount | fn string.CharCount(): int
Contains 2 1Contains | fn string.Contains(string): bool
EndsWith 2 1EndsWith | fn string.EndsWith(string): bool
Find 2 1Find | fn string.Find(string): int?
Length 10 1Length | let string.Length: int
Slice 2 1Slice | fn string.Slice(int, int): string
Split 2 1Split | fn string.Split(string): Array<string>
StartsWith 2 1StartsWith | fn string.StartsWith(string): bool
ToLowerAscii 2 1ToLowerAscii | fn string.ToLowerAscii(): string
ToUpperAscii 2 1ToUpperAscii | fn string.ToUpperAscii(): string
Trim 2 1Trim | fn string.Trim(): string
== member of Array<struct> (no == or <)
Add 2 1Add | fn Array.Add(T)
Clear 2 1Clear | fn Array.Clear()
Count 10 1Count | let Array.Count: int
Insert 2 1Insert | fn Array.Insert(int, T)
RemoveAt 2 1RemoveAt | fn Array.RemoveAt(int)
Reverse 2 1Reverse | fn Array.Reverse()
== member of Array<int>
Add 2 1Add | fn Array.Add(T)
Clear 2 1Clear | fn Array.Clear()
Contains 2 1Contains | fn Array.Contains(T): bool
Count 10 1Count | let Array.Count: int
IndexOf 2 1IndexOf | fn Array.IndexOf(T): int?
Insert 2 1Insert | fn Array.Insert(int, T)
Remove 2 1Remove | fn Array.Remove(T): bool
RemoveAt 2 1RemoveAt | fn Array.RemoveAt(int)
Reverse 2 1Reverse | fn Array.Reverse()
Sort 2 1Sort | fn Array.Sort()
== enum cases
Angry 20 1Angry | Mood.Angry
Calm 20 1Calm | Mood.Calm
== prelude enum cases
Game 20 1Game | Clock.Game
UI 20 1UI | Clock.UI
== prelude namespace
Debug 3 1Debug | fn Log.Debug(string)
Error 3 1Error | fn Log.Error(string)
Info 3 1Info | fn Log.Info(string)
Warning 3 1Warning | fn Log.Warning(string)
== import alias
K 3 1K | fn K(): int
Tool 22 1Tool | struct Tool
== context name
Audio 5 1Audio | let Rocks.World.Audio: AudioSystem
Dawn 23 1Dawn | event Rocks.World.Dawn()
GameStarted 23 1GameStarted | event Rocks.World.GameStarted()
Now 2 1Now | fn Rocks.World.Now(): float
Prepare 2 1Prepare | async fn Rocks.World.Prepare()
Service 2 1Service | fn Rocks.World.Service(): T
Spawn 2 1Spawn | fn Rocks.World.Spawn(): Actor
== engine enum
Camera 20 1Camera | Rocks.CollisionChannel.Camera
Visibility 20 1Visibility | Rocks.CollisionChannel.Visibility
== engine object members (visible extension events only)
Damaged 23 1Damaged | event Rocks.Actor.Damaged(amount: float)
Destroyed 23 1Destroyed | event Rocks.Actor.Destroyed()
Find 2 1Find | fn Rocks.Actor.Find(): T?
Find 2 1Find | Unity: GetComponent | filter GetComponent
GetForwardVector 2 1GetForwardVector | fn Rocks.Actor.GetForwardVector(): vec3
GetRightVector 2 1GetRightVector | fn Rocks.Actor.GetRightVector(): vec3
HasAuthority 5 1HasAuthority | let Rocks.Actor.HasAuthority: bool
Interacted 23 1Interacted | event Rocks.Actor.Interacted()
Location 10 1Location | var Rocks.Actor.Location: vec3
MoveTo 2 1MoveTo | async fn Rocks.Actor.MoveTo(target: vec3): bool
RequestDestroy 2 1RequestDestroy | fn Rocks.Actor.RequestDestroy()
Respawned 23 1Respawned | event Rocks.Actor.Respawned()
Tags 5 1Tags | let Rocks.Actor.Tags: TagSet
TriggerEntered 23 1TriggerEntered | event Rocks.Actor.TriggerEntered(other: Actor, trigger: Collider)
== Unity GetComponent
Find 2 1Find | Unity: GetComponent | filter GetComponent
== override seen once
Act 2 1Act | fn Gate.Act()
Closed 23 1Closed | event Gate.Closed()
DescribeState 2 1DescribeState | fn Rocks.ScriptableComponent.DescribeState(detailed: bool): string
Owner 5 1Owner | let Rocks.Component.Owner: Actor
Slid 23 1Slid | event Panel.Slid()
Width 5 1Width | var Panel.Width: int
== scope prefix
count 6 0count | param count: int
Color 7 4Color | type Color
Collider 7 5Collider | class Rocks.Collider : Rocks.SceneComponent
CollisionChannel 13 5CollisionChannel | enum Rocks.CollisionChannel
Component 7 5Component | class Rocks.Component
Connection 7 5Connection | class Rocks.Connection
const 14 6const | keyword
continue 14 6continue | keyword
== scope prefix (engine type)
SphereCollider 7 5SphereCollider | class Rocks.SphereCollider : Rocks.Collider
== keyword prefix
return 14 6return | keyword
== digit prefix
(empty)
== underscore prefix
(empty)
== scope at statement start
count 6 0count | param count: int
d 6 0d | param d: Door
Arm 5 1Arm | var Door.Arm: SceneComponent?
DescribeState 2 1DescribeState | fn Rocks.ScriptableComponent.DescribeState(detailed: bool): string
Guards 5 1Guards | var Door.Guards: Array<Guard>
Label 5 1Label | let Door.Label: string
Marks 5 1Marks | var Door.Marks: Array<int>
Name 5 1Name | var Door.Name: string
Next 5 1Next | var Door.Next: Door?
Opened 23 1Opened | event Door.Opened(by: Actor)
Owner 5 1Owner | let Rocks.Component.Owner: Actor
Points 5 1Points | var Door.Points: Array<Point>
Swing 5 1Swing | var Door.Swing: float
Use 2 1Use | fn Door.Use(d: Door, count: int): int
Door 7 2Door | class Door : ScriptableComponent
Gate 7 2Gate | class Gate : Panel
Guard 7 2Guard | class Guard : Character
Limit 21 2Limit | const Limit: int
Mood 13 2Mood | enum Mood
Panel 7 2Panel | class Panel : ScriptableComponent
Point 22 2Point | struct Point
Shade 7 2Shade | class Shade : ScriptableComponent
Top 3 2Top | fn Top(p: int): int
G 3 3G | fn G(): int
Kit 9 3Kit | import Tools.Kit
Action 9 4Action | Action
All 3 4All | async fn All()
Array 7 4Array | type Array<T>
Assert 3 4Assert | fn Assert(bool, string)
Asset 9 4Asset | Asset
Assets 9 4Assets | Assets
CVar 9 4CVar | CVar
CVarRef 7 4CVarRef | type CVarRef<T>
CVars 9 4CVars | CVars
Caller 6 4Caller | Caller: Connection
Class 7 4Class | type Class<T>
Clock 7 4Clock | type Clock
Color 7 4Color | type Color
Delay 3 4Delay | async fn Delay(float, Clock)
Format 9 4Format | Format
Input 6 4Input | Input: InputSystem
InputAction 7 4InputAction | type InputAction
LoadRequest 7 4LoadRequest | type LoadRequest<T>
Loc 9 4Loc | Loc
Log 9 4Log | Log
Map 7 4Map | type Map<K, V>
Math 9 4Math | Math
NextFixedTick 3 4NextFixedTick | async fn NextFixedTick()
NextTick 3 4NextTick | async fn NextTick()
Parse 9 4Parse | Parse
Race 3 4Race | async fn Race()
Random 9 4Random | Random
Result 7 4Result | type Result<T>
Rotator 7 4Rotator | type Rotator
Row 7 4Row | type Row<T>
Session 6 4Session | Session: PlaySession
Set 7 4Set | type Set<T>
Settings 9 4Settings | Settings
Soft 7 4Soft | type Soft<T>
Tag 7 4Tag | type Tag
TagSet 7 4TagSet | type TagSet
Text 7 4Text | type Text
Time 6 4Time | Time: Time
Transform 7 4Transform | type Transform
World 6 4World | World: World
bool 7 4bool | type bool
float 7 4float | type float
float32 7 4float32 | type float32
float64 7 4float64 | type float64
int 7 4int | type int
int16 7 4int16 | type int16
int32 7 4int32 | type int32
int64 7 4int64 | type int64
int8 7 4int8 | type int8
mat4 7 4mat4 | type mat4
quat 7 4quat | type quat
string 7 4string | type string
uint16 7 4uint16 | type uint16
uint32 7 4uint32 | type uint32
uint64 7 4uint64 | type uint64
uint8 7 4uint8 | type uint8
vec2 7 4vec2 | type vec2
vec3 7 4vec3 | type vec3
vec4 7 4vec4 | type vec4
void 7 4void | type void
Actor 7 5Actor | class Rocks.Actor
AudioSystem 7 5AudioSystem | class Rocks.AudioSystem
Character 7 5Character | class Rocks.Character : Rocks.Pawn
Collider 7 5Collider | class Rocks.Collider : Rocks.SceneComponent
CollisionChannel 13 5CollisionChannel | enum Rocks.CollisionChannel
Component 7 5Component | class Rocks.Component
Connection 7 5Connection | class Rocks.Connection
DataAsset 7 5DataAsset | class Rocks.DataAsset
Error 13 5Error | enum Rocks.Error
HandleBase 7 5HandleBase | class Rocks.HandleBase
Hit 22 5Hit | struct Rocks.Hit
InputSystem 7 5InputSystem | class Rocks.InputSystem
Interactable 8 5Interactable | interface Rocks.Interactable
LevelScript 7 5LevelScript | class Rocks.LevelScript : Rocks.Actor
LoopHandle 7 5LoopHandle | class Rocks.LoopHandle : Rocks.HandleBase
Pawn 7 5Pawn | class Rocks.Pawn : Rocks.Actor
PlaySession 7 5PlaySession | class Rocks.PlaySession
Pressable 8 5Pressable | interface Rocks.Pressable : Rocks.Interactable
SceneComponent 7 5SceneComponent | class Rocks.SceneComponent : Rocks.Component
ScriptableComponent 7 5ScriptableComponent | class Rocks.ScriptableComponent : Rocks.Component
SoundAsset 7 5SoundAsset | class Rocks.SoundAsset
SpawnParams 22 5SpawnParams | struct Rocks.SpawnParams
SphereCollider 7 5SphereCollider | class Rocks.SphereCollider : Rocks.Collider
Widget 7 5Widget | class Rocks.Widget
WidgetController 7 5WidgetController | class Rocks.WidgetController
World 7 5World | class Rocks.World
WorldService 7 5WorldService | class Rocks.WorldService
as 14 6as | keyword
async 14 6async | keyword
await 14 6await | keyword
break 14 6break | keyword
case 14 6case | keyword
class 14 6class | keyword
const 14 6const | keyword
continue 14 6continue | keyword
default 14 6default | keyword
else 14 6else | keyword
emit 14 6emit | keyword
enum 14 6enum | keyword
event 14 6event | keyword
false 14 6false | keyword
fn 14 6fn | keyword
for 14 6for | keyword
if 14 6if | keyword
import 14 6import | keyword
in 14 6in | keyword
interface 14 6interface | keyword
let 14 6let | keyword
null 14 6null | keyword
on 14 6on | keyword
open 14 6open | keyword
override 14 6override | keyword
return 14 6return | keyword
struct 14 6struct | keyword
switch 14 6switch | keyword
this 14 6this | keyword
true 14 6true | keyword
try 14 6try | keyword
var 14 6var | keyword
while 14 6while | keyword
== local after its declaration
total 6 0total | let total: float
== local in its own initializer
(empty)
== for binder in the body
item 6 0item | let item: int
== for binder after the loop
(empty)
== if-let binder
found 6 0found | let found: Door
Format 9 4Format | Format
for 14 6for | keyword
== comment
(empty)
== block comment
(empty)
== after a block comment
count 6 0count | param count: int
== doc comment
(empty)
== string
(empty)
== unclosed string
(empty)
== verbatim string
(empty)
== interpolation
count 6 0count | param count: int
Color 7 4Color | type Color
Collider 7 5Collider | class Rocks.Collider : Rocks.SceneComponent
CollisionChannel 13 5CollisionChannel | enum Rocks.CollisionChannel
Component 7 5Component | class Rocks.Component
Connection 7 5Connection | class Rocks.Connection
const 14 6const | keyword
continue 14 6continue | keyword
== string inside an interpolation
(empty)
== type position prefix
Mood 13 2Mood | enum Mood
== type after alias
Tool 22 1Tool | struct Tool
== type argument
Point 22 2Point | struct Point
== declaration name
(empty)
== on (hooks, fields, World)
Arm 5 1Arm | var Door.Arm: SceneComponent?
Awake 3 1Awake | on Awake()
CollisionEnter 3 1CollisionEnter | on CollisionEnter(other: Actor, hit: Hit)
CollisionEnter 3 1CollisionEnter | Unity: OnCollisionEnter | filter OnCollisionEnter
CollisionExit 3 1CollisionExit | on CollisionExit(other: Actor, hit: Hit)
CollisionExit 3 1CollisionExit | Unity: OnCollisionExit | filter OnCollisionExit
Destroy 3 1Destroy | on Destroy()
Destroy 3 1Destroy | Unity: OnDestroy | filter OnDestroy
Disable 3 1Disable | on Disable()
Disable 3 1Disable | Unity: OnDisable | filter OnDisable
Enable 3 1Enable | on Enable()
Enable 3 1Enable | Unity: OnEnable | filter OnEnable
FixedTick 3 1FixedTick | on FixedTick(dt: float)
FixedTick 3 1FixedTick | Unity: FixedUpdate | filter FixedUpdate
Guards 5 1Guards | var Door.Guards: Array<Guard>
Label 5 1Label | let Door.Label: string
LateTick 3 1LateTick | on LateTick(dt: float)
LateTick 3 1LateTick | Unity: LateUpdate | filter LateUpdate
Marks 5 1Marks | var Door.Marks: Array<int>
Name 5 1Name | var Door.Name: string
Next 5 1Next | var Door.Next: Door?
Owner 5 1Owner | let Rocks.Component.Owner: Actor
Points 5 1Points | var Door.Points: Array<Point>
Start 3 1Start | on Start()
Swing 5 1Swing | var Door.Swing: float
Tick 3 1Tick | on Tick(dt: float)
Tick 3 1Tick | Unity: Update | filter Update
TriggerEnter 3 1TriggerEnter | on TriggerEnter(other: Actor, trigger: Collider)
TriggerEnter 3 1TriggerEnter | Unity: OnTriggerEnter | filter OnTriggerEnter
TriggerExit 3 1TriggerExit | on TriggerExit(other: Actor, trigger: Collider)
TriggerExit 3 1TriggerExit | Unity: OnTriggerExit | filter OnTriggerExit
Validate 3 1Validate | on Validate()
Validate 3 1Validate | Unity: OnValidate | filter OnValidate
World 6 1World | World: World
== on Unity prefix
Tick 3 1Tick | Unity: Update | filter Update
== on object field
Arm 5 1Arm | var Door.Arm: SceneComponent?
Changed 23 1Changed | event Changed
Guards 5 1Guards | var Door.Guards: Array<Guard>
Marks 5 1Marks | var Door.Marks: Array<int>
Name 5 1Name | var Door.Name: string
Next 5 1Next | var Door.Next: Door?
Opened 23 1Opened | event Door.Opened(by: Actor)
Points 5 1Points | var Door.Points: Array<Point>
Swing 5 1Swing | var Door.Swing: float
== on field of a field
Changed 23 1Changed | event Changed
== on four names
(empty)
== on string field
Changed 23 1Changed | event Changed
== on let field
(empty)
== on engine object field
Changed 23 1Changed | event Changed
RelativeLocation 10 1RelativeLocation | var Rocks.SceneComponent.RelativeLocation: vec3
== on engine changed field
Changed 23 1Changed | event Changed
== on engine field without changed
(empty)
== on array of objects
Changed 23 1Changed | event Changed
Damaged 23 1Damaged | event Rocks.Actor.Damaged(amount: float)
Destroyed 23 1Destroyed | event Rocks.Actor.Destroyed()
Interacted 23 1Interacted | event Rocks.Actor.Interacted()
Respawned 23 1Respawned | event Rocks.Actor.Respawned()
TriggerEntered 23 1TriggerEntered | event Rocks.Actor.TriggerEntered(other: Actor, trigger: Collider)
== on World
Dawn 23 1Dawn | event Rocks.World.Dawn()
GameStarted 23 1GameStarted | event Rocks.World.GameStarted()
== on this (not an Actor)
(empty)
== on (Actor class)
Awake 3 1Awake | on Awake()
CollisionEnter 3 1CollisionEnter | on CollisionEnter(other: Actor, hit: Hit)
CollisionEnter 3 1CollisionEnter | Unity: OnCollisionEnter | filter OnCollisionEnter
CollisionExit 3 1CollisionExit | on CollisionExit(other: Actor, hit: Hit)
CollisionExit 3 1CollisionExit | Unity: OnCollisionExit | filter OnCollisionExit
Destroy 3 1Destroy | on Destroy()
Destroy 3 1Destroy | Unity: OnDestroy | filter OnDestroy
Disable 3 1Disable | on Disable()
Disable 3 1Disable | Unity: OnDisable | filter OnDisable
Enable 3 1Enable | on Enable()
Enable 3 1Enable | Unity: OnEnable | filter OnEnable
FixedTick 3 1FixedTick | on FixedTick(dt: float)
FixedTick 3 1FixedTick | Unity: FixedUpdate | filter FixedUpdate
HasAuthority 5 1HasAuthority | let Rocks.Actor.HasAuthority: bool
LateTick 3 1LateTick | on LateTick(dt: float)
LateTick 3 1LateTick | Unity: LateUpdate | filter LateUpdate
Location 10 1Location | var Rocks.Actor.Location: vec3
Start 3 1Start | on Start()
Tags 5 1Tags | let Rocks.Actor.Tags: TagSet
Tick 3 1Tick | on Tick(dt: float)
Tick 3 1Tick | Unity: Update | filter Update
TriggerEnter 3 1TriggerEnter | on TriggerEnter(other: Actor, trigger: Collider)
TriggerEnter 3 1TriggerEnter | Unity: OnTriggerEnter | filter OnTriggerEnter
TriggerExit 3 1TriggerExit | on TriggerExit(other: Actor, trigger: Collider)
TriggerExit 3 1TriggerExit | Unity: OnTriggerExit | filter OnTriggerExit
Validate 3 1Validate | on Validate()
Validate 3 1Validate | Unity: OnValidate | filter OnValidate
World 6 1World | World: World
this 14 1this | keyword
== on this (Actor class)
Damaged 23 1Damaged | event Rocks.Actor.Damaged(amount: float)
Destroyed 23 1Destroyed | event Rocks.Actor.Destroyed()
Interacted 23 1Interacted | event Rocks.Actor.Interacted()
Respawned 23 1Respawned | event Rocks.Actor.Respawned()
TriggerEntered 23 1TriggerEntered | event Rocks.Actor.TriggerEntered(other: Actor, trigger: Collider)
== emit (own events only)
Closed 23 1Closed | event Gate.Closed()
Owner 5 1Owner | let Rocks.Component.Owner: Actor
== emit (Actor class)
other 6 0other | param other: Guard
this 14 1this | keyword
== emit this
Interacted 23 1Interacted | event Rocks.Actor.Interacted()
== emit to a local
Interacted 23 1Interacted | event Rocks.Actor.Interacted()
== attribute
authorityOnly 14 1authorityOnly | @authorityOnly
cheat 14 1cheat | @cheat
cvar 14 1cvar | @cvar
deterministic 14 1deterministic | @deterministic
edit 14 1edit | @edit
editorButton 14 1editorButton | @editorButton
formerly 14 1formerly | @formerly
net 14 1net | @net
pure 14 1pure | @pure
restart 14 1restart | @restart
rpc 14 1rpc | @rpc
save 14 1save | @save
skipWhileBusy 14 1skipWhileBusy | @skipWhileBusy
tableRow 14 1tableRow | @tableRow
test 14 1test | @test
tick 14 1tick | @tick
worker 14 1worker | @worker
== base list first (full)
Panel 7 2Panel | class Panel : ScriptableComponent
Kit 9 3Kit | import Tools.Kit
Actor 7 5Actor | class Rocks.Actor
Character 7 5Character | class Rocks.Character : Rocks.Pawn
DataAsset 7 5DataAsset | class Rocks.DataAsset
LevelScript 7 5LevelScript | class Rocks.LevelScript : Rocks.Actor
Pawn 7 5Pawn | class Rocks.Pawn : Rocks.Actor
ScriptableComponent 7 5ScriptableComponent | class Rocks.ScriptableComponent : Rocks.Component
WidgetController 7 5WidgetController | class Rocks.WidgetController
WorldService 7 5WorldService | class Rocks.WorldService
== base list later (full)
Kit 9 3Kit | import Tools.Kit
Interactable 8 5Interactable | interface Rocks.Interactable
Pressable 8 5Pressable | interface Rocks.Pressable : Rocks.Interactable
== base list skips the class itself
DataAsset 7 5DataAsset | class Rocks.DataAsset
== import
Lib 9 2Lib | import Lib
Shared 9 2Shared | import Shared
Tools 9 2Tools | import Tools
== import segment
Kit 9 2Kit | import Tools.Kit
Siren 9 2Siren | import Tools.Siren
== import dependency
Util 9 2Util | import Shared.Util
== core: base list first
Actor 7 5Actor | class Rocks.Actor
Character 7 5Character | class Rocks.Character : Rocks.Pawn
DataAsset 7 5DataAsset | class Rocks.DataAsset
LevelScript 7 5LevelScript | class Rocks.LevelScript : Rocks.Actor
Pawn 7 5Pawn | class Rocks.Pawn : Rocks.Actor
ScriptableComponent 7 5ScriptableComponent | class Rocks.ScriptableComponent : Rocks.Component
WidgetController 7 5WidgetController | class Rocks.WidgetController
WorldService 7 5WorldService | class Rocks.WorldService
== core: base list later
Interactable 8 5Interactable | interface Rocks.Interactable
Pressable 8 5Pressable | interface Rocks.Pressable : Rocks.Interactable
== core: attribute
authorityOnly 14 1authorityOnly | @authorityOnly
edit 14 1edit | @edit
formerly 14 1formerly | @formerly
net 14 1net | @net
restart 14 1restart | @restart
save 14 1save | @save
skipWhileBusy 14 1skipWhileBusy | @skipWhileBusy
tableRow 14 1tableRow | @tableRow
== core: on
Arm 5 1Arm | var Door.Arm: SceneComponent?
Awake 3 1Awake | on Awake()
CollisionEnter 3 1CollisionEnter | on CollisionEnter(other: Actor, hit: Hit)
CollisionEnter 3 1CollisionEnter | Unity: OnCollisionEnter | filter OnCollisionEnter
CollisionExit 3 1CollisionExit | on CollisionExit(other: Actor, hit: Hit)
CollisionExit 3 1CollisionExit | Unity: OnCollisionExit | filter OnCollisionExit
Destroy 3 1Destroy | on Destroy()
Destroy 3 1Destroy | Unity: OnDestroy | filter OnDestroy
Disable 3 1Disable | on Disable()
Disable 3 1Disable | Unity: OnDisable | filter OnDisable
Enable 3 1Enable | on Enable()
Enable 3 1Enable | Unity: OnEnable | filter OnEnable
FixedTick 3 1FixedTick | on FixedTick(dt: float)
FixedTick 3 1FixedTick | Unity: FixedUpdate | filter FixedUpdate
Guards 5 1Guards | var Door.Guards: Array<Guard>
Label 5 1Label | let Door.Label: string
LateTick 3 1LateTick | on LateTick(dt: float)
LateTick 3 1LateTick | Unity: LateUpdate | filter LateUpdate
Marks 5 1Marks | var Door.Marks: Array<int>
Name 5 1Name | var Door.Name: string
Next 5 1Next | var Door.Next: Door?
Owner 5 1Owner | let Rocks.Component.Owner: Actor
Points 5 1Points | var Door.Points: Array<Point>
Start 3 1Start | on Start()
Swing 5 1Swing | var Door.Swing: float
Tick 3 1Tick | on Tick(dt: float)
Tick 3 1Tick | Unity: Update | filter Update
TriggerEnter 3 1TriggerEnter | on TriggerEnter(other: Actor, trigger: Collider)
TriggerEnter 3 1TriggerEnter | Unity: OnTriggerEnter | filter OnTriggerEnter
TriggerExit 3 1TriggerExit | on TriggerExit(other: Actor, trigger: Collider)
TriggerExit 3 1TriggerExit | Unity: OnTriggerExit | filter OnTriggerExit
World 6 1World | World: World
== core: keyword o
Opened 23 1Opened | event Door.Opened(by: Actor)
Owner 5 1Owner | let Rocks.Component.Owner: Actor
on 14 6on | keyword
override 14 6override | keyword
== core: prelude Cl
Clock 7 4Clock | type Clock
class 14 6class | keyword
)";

} // namespace

TEST(SparksLsp_CompletionGolden) {
    SystemAllocator allocator;
    {
        LspCompletionWorkspace workspace(allocator);
        LspScript script(allocator);
        script.Initialize(1, workspace.root.View());
        int64 id = 100;
        int64 version = 1;
        bool openGame = false;
        bool openBasic = false;
        for (const LspCompletionProbe& probe : LspCompletionProbes) {
            const LspCompletionCase made = LspMakeCase(allocator, probe.slot, probe.snippet);
            const StringView uri = probe.core ? workspace.basic.View() : workspace.game.View();
            bool& opened = probe.core ? openBasic : openGame;
            if (opened) {
                script.Change(uri, ++version, made.text.View());
            } else {
                script.Open(uri, ++version, made.text.View());
                opened = true;
            }

            script.At(id++, "textDocument/completion", uri, made.place.line, made.place.character);
        }

        script.End(2);
        const StringView arguments[] = {"--schema", workspace.schema.View()};
        const LspRun run = LspRunScript(allocator, arguments, script.text.View());
        CHECK(run.code == LspTestClean);
        String text(allocator);
        id = 100;
        for (const LspCompletionProbe& probe : LspCompletionProbes) {
            CHECK(text.TryAppend("== "));
            CHECK(text.TryAppend(probe.label));
            CHECK(text.TryAppend("\n"));
            LspAppendCompletionText(allocator,
                                    LspResultText(allocator, run.output.View(), id++).View(), text);
        }

        LspExpectText(text.View(), LspCompletionGolden, "completion golden");
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_CompletionEdges) {
    // 열지 않은 문서·manifest·작업 공간 밖 문서는 빈 목록, 위치 인자가 틀리면 -32602.
    SystemAllocator allocator;
    {
        LspCompletionWorkspace workspace(allocator);
        const String manifest = workspace.folder.UriOf("Game/SparksPackage.json5");
        const String outside = workspace.folder.UriOf("Elsewhere/Loose.spark");
        LspScript script(allocator);
        script.Initialize(1, workspace.root.View())
            .At(10, "textDocument/completion", workspace.game.View(), 20, 8)
            .Open(manifest.View(), 1, LspTestFullManifest)
            .At(11, "textDocument/completion", manifest.View(), 0, 2)
            .Open(outside.View(), 1, "fn F() {\n    \n}\n")
            .At(12, "textDocument/completion", outside.View(), 1, 4)
            .Call(13, "textDocument/completion", "{\"textDocument\":{\"uri\":\"file:///x\"}}")
            .End(2);
        const StringView arguments[] = {"--schema", workspace.schema.View()};
        const LspRun run = LspRunScript(allocator, arguments, script.text.View());
        CHECK(run.code == LspTestClean);
        for (int64 at = 10; at <= 12; ++at) {
            const String result = LspResultText(allocator, run.output.View(), at);
            CHECK(result.View() == "{\"isIncomplete\":false,\"items\":[]}");
        }

        CHECK(LspJoined(allocator, run.output.View())
                  .View()
                  .Contains("{\"jsonrpc\":\"2.0\",\"id\":13,\"error\":{\"code\":-32602,",
                            StringComparison::CaseSensitive));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_CompletionBomAndCrlf) {
    // BOM·CRLF 문서의 같은 자리는 LF 문서와 같은 후보다(위치 변환·구멍 자리).
    SystemAllocator allocator;
    {
        LspCompletionWorkspace workspace(allocator);
        const LspCompletionCase made = LspMakeCase(allocator, "@@body", "_ = d.Sw|;");
        String crlf(allocator);
        CHECK(crlf.TryAppend("\xEF\xBB\xBF"));
        for (const char value : made.text.View()) {
            CHECK(crlf.TryAppend(value == '\n' ? StringView("\r\n") : StringView(&value, 1)));
        }

        LspScript script(allocator);
        script.Initialize(1, workspace.root.View())
            .Open(workspace.game.View(), 1, made.text.View())
            .At(10, "textDocument/completion", workspace.game.View(), made.place.line,
                made.place.character)
            .Change(workspace.game.View(), 2, crlf.View())
            .At(11, "textDocument/completion", workspace.game.View(), made.place.line,
                made.place.character)
            .End(2);
        const StringView arguments[] = {"--schema", workspace.schema.View()};
        const LspRun run = LspRunScript(allocator, arguments, script.text.View());
        CHECK(run.code == LspTestClean);
        const String lf = LspResultText(allocator, run.output.View(), 10);
        CHECK(lf.View().Contains("\"label\":\"Swing\"", StringComparison::CaseSensitive));
        CHECK(LspResultText(allocator, run.output.View(), 11).View() == lf.View());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_CompletionLimit) {
    // 후보가 상한(1,000)을 넘으면 앞 1,000개와 isIncomplete다.
    SystemAllocator allocator;
    {
        LspCompletionWorkspace workspace(allocator);
        String many(allocator);
        constexpr uint32 Count = 1100;
        for (uint32 at = 0; at < Count; ++at) {
            StringBuilder<64> line(allocator);
            line.AppendFormat("const Q{}: int = 1;\n", at + 10000);
            CHECK(!line.HasFailed());
            CHECK(many.TryAppend(line.View()));
        }

        CHECK(many.TryAppend("fn F() {\n    _ = Q;\n}\n"));
        const String uri = workspace.folder.UriOf("Game/Many.spark");
        LspScript script(allocator);
        script.Initialize(1, workspace.root.View())
            .Open(uri.View(), 1, many.View())
            .At(10, "textDocument/completion", uri.View(), Count + 1, 9)
            .End(2);
        const StringView arguments[] = {"--schema", workspace.schema.View()};
        const LspRun run = LspRunScript(allocator, arguments, script.text.View());
        CHECK(run.code == LspTestClean);
        const String result = LspResultText(allocator, run.output.View(), 10);
        JsonDocument document(allocator);
        REQUIRE(document.Parse(result.View()).Succeeded());
        CHECK(document.Root().Find("isIncomplete").GetBool());
        CHECK(document.Root().Find("items").Size() == 1000);
        CHECK(document.Root().Find("items").At(0).Find("label").GetString() == "Q10000");
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_CompletionFuzz) {
    // 원문의 모든 토큰 경계에서 요청한다: 오류 없이 응답하고, 같은 각본을 두 번 돌리면 같은
    // 출력이다.
    SystemAllocator allocator;
    {
        LspCompletionWorkspace workspace(allocator);
        const LspCompletionCase base = LspMakeCase(allocator, "", "");
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto added = sources.TryAdd("Main.spark", base.text.View(), diagnostics);
        REQUIRE(added);
        Array<Token> tokens(allocator);
        REQUIRE(Tokenize(sources, added.Value(), diagnostics, tokens));
        Array<uint32> offsets(allocator);
        for (const Token& token : tokens) {
            CHECK(offsets.TryPushBack(token.span.begin));
            CHECK(offsets.TryPushBack(token.span.end));
        }

        LspScript script(allocator);
        script.Initialize(1, workspace.root.View())
            .Open(workspace.game.View(), 1, base.text.View());
        int64 id = 100;
        uint32 previous = NumericLimits<uint32>::Max();
        for (const uint32 offset : offsets) {
            if (offset == previous) {
                continue;
            }

            previous = offset;
            LspTestPlace at;
            for (usize index = 0; index < offset; ++index) {
                if (base.text.View().Data()[index] == '\n') {
                    ++at.line;
                    at.character = 0;
                } else {
                    ++at.character;
                }
            }

            script.At(id++, "textDocument/completion", workspace.game.View(), at.line,
                      at.character);
        }

        script.End(2);
        const StringView arguments[] = {"--schema", workspace.schema.View()};
        const LspRun first = LspRunScript(allocator, arguments, script.text.View());
        const LspRun second = LspRunScript(allocator, arguments, script.text.View());
        CHECK(first.code == LspTestClean);
        CHECK(first.output.View() == second.output.View());
        for (int64 at = 100; at < id; ++at) {
            const String result = LspResultText(allocator, first.output.View(), at);
            CHECK(result.View().StartsWith("{\"isIncomplete\":false,\"items\":[",
                                           StringComparison::CaseSensitive));
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_CompletionAllocationFailure) {
    // 멤버·`on`·접두 자리의 자동완성 세션의 모든 할당 실패 — 3이거나 같은 출력.
    SystemAllocator backing;
    LspFolder folder(backing);
    constexpr StringView Small = "class Door : ScriptableComponent {\n    var Swing: int = 1;\n"
                                 "    on Start() {\n        _ = this.Sw;\n    }\n}\n";
    REQUIRE(folder.Write("Game/SparksPackage.json5", LspTestFullManifest));
    const String schema = LspSchemaFolder(backing);
    const String root = folder.UriOf("");
    const String main = folder.UriOf("Game/Main.spark");
    LspScript script(backing);
    script.Initialize(1, root.View()).Open(main.View(), 1, Small);
    script.At(10, "textDocument/completion", main.View(), 3, 17);
    script.At(11, "textDocument/completion", main.View(), 2, 7);
    script.End(2);
    const StringView arguments[] = {"--schema", schema.View()};
    LspSweepAllocations(backing, arguments, script.text.View(), LspTestClean);
}

} // namespace sparks_tools_lsp_completion_tests_file
