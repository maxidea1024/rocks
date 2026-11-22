#include <Core/Algorithms.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>

#include "CompilationChecker.h"
#include "SchemaInput.h"
#include "StringLiteralReader.h"
#include "TypeChecker.h"

// 안정 ID(Docs/Design/SparksBytecode.ko.md 6.1절, LC4d): 선언의 이름 키를 패키지 ID manifest
// (`<패키지>.rocksids.json`)와 맞춰, 사라진 데이터 연결 ID(S1005)·manifest 형식 오류(S1006)·예약된
// 이름의 재사용(S1007)·한 ID의 이중 사용(S1008)을 알리고 manifest에 없는 선언을 발급 대기 목록으로
// 남긴다. 컴파일러는 GUID를 만들지 않는다.
//
// 맞춤은 두 번 돈다(선언 순서와 무관하게 같은 결과): 1) 현재 이름 키가 항목의 현재 이름과 같으면 그
// 항목(alias·은퇴 이름과 같으면 S1007), 2) 남은 선언은 `@formerly`의 옛 키(전체 키 또는 같은 주인
// 아래 이름)로, 주인 type이 `@formerly`로 이어졌으면 옛 주인 키 아래 같은 이름으로 잇는다.

namespace rocks::sparks::detail {

namespace {

constexpr StringView IdKindNames[] = {"type", "field", "subobject", "function", "rpc", "enumCase"};
constexpr uint8 TypeKind = 0;
constexpr uint8 FieldKind = 1;
constexpr uint8 SubobjectKind = 2;
constexpr uint8 FunctionKind = 3;
constexpr uint8 RpcKind = 4;
constexpr uint8 EnumCaseKind = 5;
constexpr uint8 NoIdKind = 0xFF;

// manifest 형식(`schema: 1`)과 ID 글자 수(소문자 16진 GUID)다.
constexpr int64 ManifestSchema = 1;
constexpr usize IdDigits = 32;

// 요구·항목 번호의 "없음" 표지다.
constexpr uint32 NoWanted = NumericLimits<uint32>::Max();
constexpr uint32 NoEntry = NumericLimits<uint32>::Max();

// 이름 키 조립용 builder의 inline 크기다.
constexpr usize KeyInlineBytes = 256;

uint8 IdKindOf(StringView name) noexcept {
    for (uint8 index = 0; index < sizeof(IdKindNames) / sizeof(IdKindNames[0]); ++index) {
        if (IdKindNames[index] == name) {
            return index;
        }
    }

    return NoIdKind;
}

bool IsLowerHexId(StringView text) noexcept {
    if (text.ByteCount() != IdDigits) {
        return false;
    }

    bool nonzero = false;
    for (usize index = 0; index < text.ByteCount(); ++index) {
        const char c = text.Data()[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }

        nonzero = nonzero || c != '0';
    }

    return nonzero;
}

bool Contains(StringView text, char wanted) noexcept {
    for (usize index = 0; index < text.ByteCount(); ++index) {
        if (text.Data()[index] == wanted) {
            return true;
        }
    }

    return false;
}

// Sparks 멤버 이름 키의 주인 type 키다(마지막 `/` 뒤 첫 `.`의 선언까지 — C#
// StableIdManifest.Owner와 같다). type 키면 그대로다.
StringView IdOwnerKey(StringView name) noexcept {
    usize slash = 0;
    for (usize index = 0; index < name.ByteCount(); ++index) {
        slash = name.Data()[index] == '/' ? index + 1 : slash;
    }

    usize dot = slash;
    while (dot < name.ByteCount() && name.Data()[dot] != '.') {
        ++dot;
    }

    usize member = dot + 1;
    while (member < name.ByteCount() && name.Data()[member] != '.') {
        ++member;
    }

    return member < name.ByteCount() ? StringView(name.Data(), member) : name;
}

// 맞춤 상태다.
enum class WantedState : uint8 {
    Unresolved,
    Direct,  // 현재 이름이 항목의 현재 이름과 같다
    Renamed, // `@formerly`로 이었다(발급 대기는 rename)
    Carried, // 주인 type의 rename을 따라 옮겨진다(발급 대기 없음 — 도구의 type rename이 옮긴다)
    Reported // S1007·S1008을 냈다
};

// 현재 선언 하나의 ID 요구다(이름 키·멤버 이름·`@formerly` 글자는 keys 글자 안의 구간).
struct WantedId {
    uint8 kind = NoIdKind;
    WantedState state = WantedState::Unresolved;
    uint32 module = 0;
    uint32 node = NoNode;    // 진단 위치(선언 이름·handler 경로)
    uint32 owner = NoWanted; // 멤버면 주인 type 요구의 번호
    uint32 entry = NoEntry;  // 이은 manifest 항목
    uint32 keyOffset = 0;
    uint32 keyLength = 0;
    uint32 memberOffset = 0; // 멤버 이름(주인 키 뒤 `.` 다음 — type은 길이 0)
    uint32 memberLength = 0;
    uint32 formerOffset = 0; // `@formerly`의 글자 그대로
    uint32 formerLength = 0; // 0이면 `@formerly` 없음
};

bool NameLess(const ManifestIdName& left, const ManifestIdName& right) noexcept {
    if (left.kind != right.kind) {
        return left.kind < right.kind;
    }

    return StringView::CompareBytes(left.name, right.name) < 0;
}

} // namespace

StringView TypeChecker::FormerName(uint32 module, uint32 declaration,
                                   Array<char>& scratch) noexcept {
    const SyntaxTree& tree = compilation_.trees_[module];
    const uint32 attributes = tree.Child(declaration, 1);
    if (attributes == NoNode || tree.Node(attributes).kind != SyntaxKind::List) {
        return StringView();
    }

    for (const uint32 attribute : tree.Children(attributes)) {
        if (tree.Node(attribute).kind != SyntaxKind::Attribute ||
            TokenText(module, attribute) != "formerly") {
            continue;
        }

        const uint32 list = tree.Child(attribute, 0);
        if (list == NoNode || tree.Node(list).childCount == 0) {
            return StringView();
        }

        const uint32 value = tree.Child(tree.Child(list, 0), 1);
        if (value == NoNode || tree.Node(value).kind != SyntaxKind::String) {
            return StringView();
        }

        // 보간 없는 문자열(검사기가 S0623으로 본다) — 조각 하나의 원문을 푼다.
        scratch.Clear();
        for (const uint32 part : tree.Children(value)) {
            StringLiteralReader reader = StringLiteralReader::ForPiece(TokenText(module, part));
            uint8 byte = 0;
            while (reader.Next(byte)) {
                if (auto pushed = scratch.TryPushBack(static_cast<char>(byte)); !pushed) {
                    binder_.error_ = pushed.Error();
                    return StringView();
                }
            }
        }

        return StringView(scratch.Data(), scratch.Size());
    }

    return StringView();
}

void TypeChecker::CheckStableIds() noexcept {
    Compilation& c = compilation_;
    // 진단이 난 원문(구문·타입 오류 등)의 모듈은 판정하지 않는다(입력 중 미완성 — SL §12.2).
    Array<uint8> unstable(allocator_);
    if (auto resized = unstable.TryResize(c.modules_.Size()); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (uint32 module = 0; module < c.modules_.Size(); ++module) {
        unstable[module] = 0;
    }

    for (usize index = 0; index < binder_.diagnostics_.Count(); ++index) {
        const SourceId source = binder_.diagnostics_[index].Source();
        for (uint32 module = 0; module < c.modules_.Size(); ++module) {
            if (c.modules_[module].source.index == source.index) {
                unstable[module] = 1;
            }
        }
    }

    // interface 구현 함수(Sparks·엔진 interface, 상속한 구현 포함)는 FunctionId를 받는다(SL §12.2).
    Array<uint8> implementations(allocator_);
    if (auto resized = implementations.TryResize(c.symbols_.Size()); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (usize index = 0; index < implementations.Size(); ++index) {
        implementations[index] = 0;
    }

    MarkInterfaceImplementations(implementations);
    for (uint32 package = 0; package < c.packages_.Size() && binder_.error_ == ErrorCode::None;
         ++package) {
        if (c.packages_[package].valid) {
            CheckPackageIds(package, unstable.View(), implementations.View());
        }
    }
}

void TypeChecker::MarkInterfaceImplementations(Array<uint8>& marks) noexcept {
    Compilation& c = compilation_;
    auto mark = [&](uint32 classSymbol, StringView name) noexcept {
        const uint32 implementation = binder_.FindMemberInChain(classSymbol, name);
        if (implementation != NoSymbol && c.symbols_[implementation].kind == SymbolKind::Method) {
            marks[implementation] = 1;
        }
    };
    for (uint32 classSymbol = 0; classSymbol < c.symbols_.Size(); ++classSymbol) {
        const Symbol& entry = c.symbols_[classSymbol];
        if (entry.kind != SymbolKind::Class || (entry.flags & SymbolFlags::Incomplete) != 0) {
            continue;
        }

        // 첫 기반은 class(또는 엔진 기반), 그 뒤는 interface다(NameBinder::CheckInterfaces와 같다).
        for (uint32 index = 1; index < entry.baseCount; ++index) {
            uint32 interfaceSymbol = c.symbolBases_[entry.firstBase + index];
            if (interfaceSymbol == NoSymbol) {
                continue;
            }

            if (c.symbols_[interfaceSymbol].kind == SymbolKind::Interface) {
                const Symbol& owner = c.symbols_[interfaceSymbol];
                const SyntaxTree& tree = c.trees_[owner.module];
                const uint32 methods = tree.Child(owner.node, 3);
                if (methods == NoNode || tree.Node(methods).kind != SyntaxKind::List) {
                    continue;
                }

                for (const uint32 method : tree.Children(methods)) {
                    const uint32 symbol = tree.Node(method).kind == SyntaxKind::InterfaceMethod
                                              ? c.NodeSymbol(owner.module, tree.Child(method, 2))
                                              : NoSymbol;
                    if (symbol != NoSymbol) {
                        mark(classSymbol, c.symbols_[symbol].name);
                    }
                }

                continue;
            }

            // 엔진 interface와 그 `base` 체인의 함수들.
            for (uint32 step = 0; interfaceSymbol != NoSymbol && step < c.symbols_.Size(); ++step) {
                const Symbol& owner = c.symbols_[interfaceSymbol];
                if (owner.kind != SymbolKind::EngineType) {
                    break;
                }

                const BindingSchema& schema = c.schemas_[owner.index].schema;
                const SchemaType& type = schema.Types()[owner.item];
                for (uint32 function = type.firstFunction;
                     function < type.firstFunction + type.functionCount; ++function) {
                    mark(classSymbol, schema.Text(schema.Functions()[function].name));
                }

                interfaceSymbol = owner.base;
            }
        }
    }
}

void TypeChecker::CheckPackageIds(uint32 package, Span<const uint8> unstable,
                                  Span<const uint8> implementations) noexcept {
    Compilation& c = compilation_;
    const StringView packageName = c.packages_[package].manifest.Name();
    Array<char> keys(allocator_);
    Array<WantedId> wanted(allocator_);
    auto append = [&](StringView text) noexcept {
        if (auto appended = keys.TryAppend(Span<const char>(text.Data(), text.ByteCount()));
            !appended) {
            binder_.error_ = appended.Error();
        }
    };
    // keys가 자라면 Data가 바뀌므로 view는 쓰는 자리에서 그때그때 만든다.
    auto view = [&](uint32 offset, uint32 length) noexcept {
        return StringView(keys.Data() + offset, length);
    };
    // 모듈 접두 `<패키지>/<경로>`(`.spark`를 뺀다)다.
    auto modulePath = [&](uint32 module) noexcept {
        StringView path = c.Text(c.modules_[module].path);
        constexpr StringView Extension = ".spark";
        if (path.EndsWith(Extension)) {
            path = StringView(path.Data(), path.ByteCount() - Extension.ByteCount());
        }

        return path;
    };
    // `@formerly("Old")`의 옛 이름(디코드한 byte는 scratch에 — FormerName).
    Array<char> scratch(allocator_);
    auto formerly = [&](uint32 module, uint32 declaration) noexcept -> StringView {
        return FormerName(module, declaration, scratch);
    };
    // 요구를 더한다. owner가 NoWanted면 type(키 `<패키지>/<모듈>.<이름>`), 아니면 그 주인 키 뒤
    // `.<이름>`이다.
    auto want = [&](uint8 kind, uint32 module, uint32 node, uint32 owner, StringView member,
                    StringView former) noexcept -> uint32 {
        WantedId entry;
        entry.kind = kind;
        entry.module = module;
        entry.node = node;
        entry.owner = owner;
        entry.keyOffset = static_cast<uint32>(keys.Size());
        if (owner == NoWanted) {
            append(packageName);
            append("/");
            append(modulePath(module));
        } else {
            const WantedId& parent = wanted[owner];
            if (auto appended = keys.TryAppend(
                    Span<const char>(keys.Data() + parent.keyOffset, parent.keyLength));
                !appended) {
                binder_.error_ = appended.Error();
            }
        }

        append(".");
        entry.memberOffset = static_cast<uint32>(keys.Size());
        append(member);
        entry.memberLength = owner == NoWanted ? 0 : static_cast<uint32>(member.ByteCount());
        entry.keyLength = static_cast<uint32>(keys.Size()) - entry.keyOffset;
        entry.formerOffset = static_cast<uint32>(keys.Size());
        append(former);
        entry.formerLength = static_cast<uint32>(former.ByteCount());
        if (auto pushed = wanted.TryPushBack(entry); !pushed) {
            binder_.error_ = pushed.Error();
            return NoWanted;
        }

        return static_cast<uint32>(wanted.Size() - 1);
    };

    // 현재 선언의 ID 요구(6.1절 "ID를 받는 선언"), 선언 순.
    for (uint32 module = 0; module < c.modules_.Size() && binder_.error_ == ErrorCode::None;
         ++module) {
        if (c.modules_[module].package != package || c.nodeBase_[module] == NoNode ||
            unstable[module] != 0) {
            continue;
        }

        const SyntaxTree& tree = c.trees_[module];
        const uint32 root = tree.Root();
        const uint32 declarations = root == NoNode ? NoNode : tree.Child(root, 1);
        if (declarations == NoNode) {
            continue;
        }

        for (const uint32 declaration : tree.Children(declarations)) {
            const SyntaxKind kind = tree.Node(declaration).kind;
            if (kind != SyntaxKind::Class && kind != SyntaxKind::Struct &&
                kind != SyntaxKind::Enum) {
                continue;
            }

            const uint32 typeName = tree.Child(declaration, 2);
            const uint32 typeSymbol = c.NodeSymbol(module, typeName);
            if (typeSymbol == NoSymbol) {
                continue;
            }

            const uint32 owner = want(TypeKind, module, typeName, NoWanted,
                                      c.symbols_[typeSymbol].name, formerly(module, declaration));
            const uint32 members = tree.Child(declaration, kind == SyntaxKind::Class ? 4 : 3);
            if (owner == NoWanted || members == NoNode ||
                tree.Node(members).kind != SyntaxKind::List) {
                continue;
            }

            for (const uint32 member : tree.Children(members)) {
                const SyntaxNode& entry = tree.Node(member);
                if (entry.kind == SyntaxKind::EnumCase || entry.kind == SyntaxKind::Field) {
                    const uint32 name = tree.Child(member, 2);
                    const uint32 symbol = c.NodeSymbol(module, name);
                    if (symbol == NoSymbol) {
                        continue;
                    }

                    // 소유 component는 subobject다: class의 `let` 필드이고 초기값이 component
                    // 생성식
                    // (`let Hinge = Hinge()`)이다. 객체 타입의 참조 필드(`@edit let Target:
                    // Door`)는 field다.
                    uint8 idKind = entry.kind == SyntaxKind::EnumCase ? EnumCaseKind : FieldKind;
                    const uint32 initializer =
                        entry.kind == SyntaxKind::Field ? tree.Child(member, 4) : NoNode;
                    if (kind == SyntaxKind::Class && (entry.flags & SyntaxFlags::Let) != 0 &&
                        initializer != NoNode && tree.Node(initializer).kind == SyntaxKind::Call) {
                        const uint32 callee = c.NodeSymbol(module, tree.Child(initializer, 0));
                        const uint32 type = c.symbols_[symbol].type;
                        if (callee != NoSymbol &&
                            (c.symbols_[callee].kind == SymbolKind::Class ||
                             c.symbols_[callee].kind == SymbolKind::EngineType) &&
                            type != NoType && IsObjectType(NonOptional(type))) {
                            idKind = SubobjectKind;
                        }
                    }

                    want(idKind, module, name, owner, c.symbols_[symbol].name,
                         formerly(module, member));
                    continue;
                }

                if (entry.kind == SyntaxKind::Handler) {
                    // `on(<경로>)`.
                    StringBuilder<KeyInlineBytes> name(allocator_);
                    name.Append("on(");
                    const uint32 path = tree.Child(member, 2);
                    bool first = true;
                    for (const uint32 segment : tree.Children(path)) {
                        name.Append(first ? "" : ".").Append(TokenText(module, segment));
                        first = false;
                    }

                    name.Append(")");
                    if (!binder_.Succeeded(name)) {
                        return;
                    }

                    want(FunctionKind, module, path, owner, name.View(), StringView());
                    continue;
                }

                if (entry.kind != SyntaxKind::Function || kind != SyntaxKind::Class) {
                    continue;
                }

                const uint32 name = tree.Child(member, 2);
                const uint32 symbol = c.NodeSymbol(module, name);
                if (symbol == NoSymbol) {
                    continue;
                }

                // FunctionId: RPC·`@cheat`·`@editorButton`·override·interface 구현(SL §12.2).
                const bool rpc = !RpcTarget(symbol).IsEmpty();
                const bool functionId = rpc || HasAttribute(module, member, "cheat") ||
                                        HasAttribute(module, member, "editorButton") ||
                                        (c.symbols_[symbol].flags & SymbolFlags::Override) != 0 ||
                                        implementations[symbol] != 0;
                if (functionId) {
                    want(FunctionKind, module, name, owner, c.symbols_[symbol].name, StringView());
                }

                if (rpc) {
                    want(RpcKind, module, name, owner, c.symbols_[symbol].name, StringView());
                }
            }
        }
    }

    if (binder_.error_ != ErrorCode::None) {
        return;
    }

    // manifest가 없으면 모두 발급 대기다.
    const Compilation::PackageIdManifest& source = c.packages_[package].ids;
    Array<ManifestIdName> names(allocator_);
    Array<StringView> current(allocator_); // 항목 번호 → 현재 이름
    Array<uint32> claimedBy(allocator_);   // 항목 번호 → 이은 요구(NoWanted)
    JsonDocument document(allocator_);
    bool manifestValid = false;
    if (source.present) {
        manifestValid = ReadIdManifest(package, document, names);
        if (binder_.error_ != ErrorCode::None) {
            return;
        }

        const usize count = manifestValid ? document.Root().Find("entries").Size() : 0;
        if (auto resized = current.TryResize(count); !resized) {
            binder_.error_ = resized.Error();
            return;
        }

        if (auto resized = claimedBy.TryResize(count); !resized) {
            binder_.error_ = resized.Error();
            return;
        }

        for (usize index = 0; index < count; ++index) {
            claimedBy[index] = NoWanted;
        }

        for (const ManifestIdName& name : names) {
            if (manifestValid && name.role != ManifestNameRole::Alias) {
                current[name.entry] = name.name;
            }
        }
    }

    auto find = [&](uint8 kind, StringView name) noexcept -> const ManifestIdName* {
        if (!manifestValid) {
            return nullptr;
        }

        usize low = 0;
        usize high = names.Size();
        ManifestIdName probe;
        probe.kind = kind;
        probe.name = name;
        while (low < high) {
            const usize middle = low + (high - low) / 2;
            if (NameLess(names[middle], probe)) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }

        return low < names.Size() && names[low].kind == kind && names[low].name == name
                   ? &names[low]
                   : nullptr;
    };
    auto report = [&](const WantedId& entry, DiagnosticCode code, StringView message,
                      StringView explanation, StringView fix) noexcept {
        binder_.Report(code, c.modules_[entry.module].source,
                       binder_.NodeSpan(entry.module, entry.node), message, explanation, fix);
    };

    // 1) 현재 이름 키가 항목의 현재 이름과 같으면 그 항목이다. alias·은퇴 이름과 같으면 S1007.
    for (WantedId& entry : wanted) {
        const StringView key = view(entry.keyOffset, entry.keyLength);
        const ManifestIdName* match = find(entry.kind, key);
        if (match == nullptr) {
            continue;
        }

        if (match->role == ManifestNameRole::Current) {
            entry.state = WantedState::Direct;
            entry.entry = match->entry;
            claimedBy[match->entry] = static_cast<uint32>(&entry - wanted.Data());
            continue;
        }

        entry.state = WantedState::Reported;
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        if (match->role == ManifestNameRole::Alias) {
            message.AppendFormat("'{}' is a former name of {} '{}' in the ID manifest.", key,
                                 IdKindNames[entry.kind], current[match->entry]);
        } else {
            message.AppendFormat("'{}' was retired in the ID manifest.", key);
        }

        if (binder_.Succeeded(message)) {
            report(entry, DiagnosticCode::IdNameReserved, message.View(),
                   "Saved, replicated and edited data may still refer to the old name, so it "
                   "cannot name a new declaration.",
                   "Choose another name.");
        }
    }

    // 2) `@formerly`와 주인 type의 rename. 옛 키는 전체 키(`/`가 있으면 — 모듈을 옮긴 type)이거나
    // 같은 주인(주인이 `@formerly`로 이어졌으면 옛 주인) 아래 이름이다.
    StringBuilder<KeyInlineBytes> probe(allocator_);
    StringBuilder<KeyInlineBytes> pendingFormer(allocator_);
    for (uint32 index = 0; index < wanted.Size() && binder_.error_ == ErrorCode::None; ++index) {
        if (wanted[index].state != WantedState::Unresolved) {
            continue;
        }

        const WantedId entry = wanted[index];
        const WantedId* owner = entry.owner == NoWanted ? nullptr : &wanted[entry.owner];
        const bool ownerRenamed = owner != nullptr && owner->state == WantedState::Renamed;
        auto ownerPrefix = [&](StringBuilderBase& out) noexcept {
            if (owner == nullptr) {
                out.Append(packageName).Append("/").Append(modulePath(entry.module));
            } else if (ownerRenamed) {
                out.Append(current[owner->entry]);
            } else {
                out.Append(view(owner->keyOffset, owner->keyLength));
            }
        };
        if (entry.formerLength != 0) {
            const StringView former = view(entry.formerOffset, entry.formerLength);
            probe.Clear();
            if (Contains(former, '/')) {
                probe.Append(former);
            } else {
                ownerPrefix(probe);
                probe.Append(".").Append(former);
            }

            if (!binder_.Succeeded(probe)) {
                return;
            }

            const ManifestIdName* previous = find(entry.kind, probe.View());
            if (previous != nullptr && previous->role != ManifestNameRole::Retired) {
                if (claimedBy[previous->entry] != NoWanted) {
                    const WantedId& other = wanted[claimedBy[previous->entry]];
                    wanted[index].state = WantedState::Reported;
                    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                    message.AppendFormat("The stable ID of '{}' is already used by '{}'.",
                                         current[previous->entry],
                                         view(other.keyOffset, other.keyLength));
                    if (binder_.Succeeded(message)) {
                        report(entry, DiagnosticCode::IdClaimedTwice, message.View(),
                               "Two declarations cannot share one stable ID; the old name is "
                               "still declared or another '@formerly' names it.",
                               "Remove '@formerly' here, or rename the other declaration first.");
                    }

                    continue;
                }

                wanted[index].state = WantedState::Renamed;
                wanted[index].entry = previous->entry;
                claimedBy[previous->entry] = index;
                // 발급 대기의 옛 이름은 항목의 현재 이름이다(alias로 찾았어도 — 도구의 rename은
                // 현재 이름으로 찾는다). 주인 type도 rename이면 그 rename 뒤의 이름(새 주인 키
                // 아래)이다.
                const StringView name = current[previous->entry];
                pendingFormer.Clear();
                if (ownerRenamed && name.StartsWith(current[owner->entry])) {
                    pendingFormer.Append(view(owner->keyOffset, owner->keyLength));
                    pendingFormer.Append(
                        StringView(name.Data() + current[owner->entry].ByteCount(),
                                   name.ByteCount() - current[owner->entry].ByteCount()));
                } else {
                    pendingFormer.Append(name);
                }

                if (!binder_.Succeeded(pendingFormer) ||
                    !AddPendingId(entry.kind, view(entry.keyOffset, entry.keyLength),
                                  pendingFormer.View())) {
                    return;
                }

                continue;
            }
        }

        if (ownerRenamed) {
            // 주인 type의 rename을 따라 옮겨진다(같은 멤버 이름).
            probe.Clear();
            probe.Append(current[owner->entry])
                .Append(".")
                .Append(view(entry.memberOffset, entry.memberLength));
            if (!binder_.Succeeded(probe)) {
                return;
            }

            const ManifestIdName* carried = find(entry.kind, probe.View());
            if (carried != nullptr && carried->role == ManifestNameRole::Current &&
                claimedBy[carried->entry] == NoWanted) {
                wanted[index].state = WantedState::Carried;
                wanted[index].entry = carried->entry;
                claimedBy[carried->entry] = index;
                continue;
            }
        }

        if (!AddPendingId(entry.kind, view(entry.keyOffset, entry.keyLength), StringView())) {
            return;
        }
    }

    if (!manifestValid || binder_.error_ != ErrorCode::None) {
        return;
    }

    // 사라진 데이터 연결 항목(S1005): 판정한 모듈의 활성 항목만 본다.
    Array<uint32> byPath(allocator_);
    for (uint32 module = 0; module < c.modules_.Size(); ++module) {
        if (c.modules_[module].package == package) {
            if (auto pushed = byPath.TryPushBack(module); !pushed) {
                binder_.error_ = pushed.Error();
                return;
            }
        }
    }

    algo::Sort(byPath.View(), [&](uint32 left, uint32 right) noexcept {
        return StringView::CompareBytes(modulePath(left), modulePath(right)) < 0;
    });
    const JsonValue entries = document.Root().Find("entries");
    const StringView text = c.sources_->Get(source.source).Text();
    for (usize index = 0; index < entries.Size() && binder_.error_ == ErrorCode::None; ++index) {
        const JsonValue item = entries.At(index);
        const uint8 kind = IdKindOf(item.Find("kind").GetString());
        if (claimedBy[index] != NoWanted || item.Find("retired").GetBool() ||
            (kind != TypeKind && kind != FieldKind && kind != SubobjectKind &&
             kind != EnumCaseKind)) {
            continue;
        }

        const StringView name = item.Find("name").GetString();
        if (!JudgedModule(package, name, unstable, byPath.View())) {
            continue;
        }

        // field와 subobject 사이로 선언 형태만 바뀐 경우는 따로 알린다(ID의 kind는 바뀌지 않는다).
        const WantedId* reshaped = nullptr;
        for (const WantedId& entry : wanted) {
            if ((kind == FieldKind || kind == SubobjectKind) &&
                (entry.kind == FieldKind || entry.kind == SubobjectKind) && entry.kind != kind &&
                view(entry.keyOffset, entry.keyLength) == name) {
                reshaped = &entry;
                break;
            }
        }

        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        if (reshaped != nullptr) {
            message.AppendFormat("{} '{}' in the ID manifest is now declared as a {}.",
                                 IdKindNames[kind], name, IdKindNames[reshaped->kind]);
        } else {
            message.AppendFormat("{} '{}' is in the ID manifest but is no longer declared.",
                                 IdKindNames[kind], name);
        }

        if (!binder_.Succeeded(message)) {
            return;
        }

        binder_.Report(DiagnosticCode::DataIdLost, source.source,
                       JsonValueSpan(text, item.Find("name").Position()), message.View(),
                       "Saved, replicated and edited data refer to declarations by their stable "
                       "ID, so that data would lose its link.",
                       reshaped != nullptr
                           ? StringView("Keep the declaration form (a 'let' field created with a "
                                        "component is a subobject), or retire the ID with "
                                        "'rocks ids retire' and migrate the data.")
                           : StringView("Add '@formerly(\"<old name>\")' to the renamed "
                                        "declaration (after a module move, the full old key "
                                        "'<package>/<module>.<Name>'), rename the ID with 'rocks "
                                        "ids rename', or retire it with 'rocks ids retire'."));
    }
}

bool TypeChecker::JudgedModule(uint32 package, StringView name, Span<const uint8> unstable,
                               Span<const uint32> byPath) const noexcept {
    // 이름 키 `<패키지>/<경로>.<…>`의 모듈이 이 컴파일에서 판정한 모듈인지다. 없는 모듈은 지운
    // 것으로 판정한다(컴파일은 패키지의 모듈을 모두 담는다 — 6.1절), 진단이 난·분석하지 않은 모듈은
    // 판정하지 않는다. byPath는 이 패키지 모듈의 경로 순 번호다.
    const Compilation& c = compilation_;
    const StringView packageName = c.packages_[package].manifest.Name();
    if (name.ByteCount() <= packageName.ByteCount() + 1 ||
        StringView(name.Data(), packageName.ByteCount()) != packageName ||
        name.Data()[packageName.ByteCount()] != '/') {
        return true;
    }

    const StringView rest(name.Data() + packageName.ByteCount() + 1,
                          name.ByteCount() - packageName.ByteCount() - 1);
    usize slash = 0;
    for (usize index = 0; index < rest.ByteCount(); ++index) {
        slash = rest.Data()[index] == '/' ? index + 1 : slash;
    }

    usize dot = slash;
    while (dot < rest.ByteCount() && rest.Data()[dot] != '.') {
        ++dot;
    }

    const StringView path(rest.Data(), dot);
    constexpr StringView Extension = ".spark";
    auto pathOf = [&](uint32 module) noexcept {
        StringView modulePath = c.Text(c.modules_[module].path);
        if (modulePath.EndsWith(Extension)) {
            modulePath =
                StringView(modulePath.Data(), modulePath.ByteCount() - Extension.ByteCount());
        }

        return modulePath;
    };
    usize low = 0;
    usize high = byPath.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (StringView::CompareBytes(pathOf(byPath[middle]), path) < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    if (low < byPath.Size() && pathOf(byPath[low]) == path) {
        const uint32 module = byPath[low];
        return c.nodeBase_[module] != NoNode && unstable[module] == 0;
    }

    return true;
}

bool TypeChecker::ReadIdManifest(uint32 package, JsonDocument& document,
                                 Array<ManifestIdName>& names) noexcept {
    // manifest 형식(6.1절 — C++ 모듈과 같은 `schema: 1`)이다. 어긋나면 S1006 하나를 내고 false.
    // 성공하면 names는 모든 항목의 현재 이름·alias(역할 표시)를 (kind, 이름) 순으로 담는다.
    Compilation& c = compilation_;
    const SourceId source = c.packages_[package].ids.source;
    const StringView text = c.sources_->Get(source).Text();
    auto reject = [&](JsonPosition position, StringView message) noexcept {
        binder_.Report(DiagnosticCode::InvalidIdManifest, source, JsonValueSpan(text, position),
                       message,
                       "The stable ID manifest is written by 'rocks ids' and the Sparks tools; it "
                       "follows the shared .rocksids.json format.",
                       "Regenerate the manifest with the tools (do not edit it by hand).");
        return false;
    };
    const JsonError parsed = document.Parse(text);
    if (parsed.code == JsonErrorCode::OutOfMemory) {
        binder_.error_ = ErrorCode::OutOfMemory;
        return false;
    }

    if (!parsed.Succeeded()) {
        return reject(parsed.position, "The ID manifest is not valid JSON.");
    }

    const JsonValue root = document.Root();
    const JsonValue schema = root.IsObject() ? root.Find("schema") : JsonValue();
    if (!schema.IsValid() || !schema.IsNumber() || !schema.TryGetInt64() ||
        schema.TryGetInt64().Value() != ManifestSchema) {
        return reject(root.Position(), "The ID manifest needs \"schema\": 1.");
    }

    const JsonValue entries = root.Find("entries");
    if (!entries.IsValid() || !entries.IsArray()) {
        return reject(root.Position(), "The ID manifest needs an \"entries\" array.");
    }

    const StringView packageName = c.packages_[package].manifest.Name();
    auto inPackage = [&](StringView key) noexcept {
        return key.ByteCount() > packageName.ByteCount() + 1 &&
               StringView(key.Data(), packageName.ByteCount()) == packageName &&
               key.Data()[packageName.ByteCount()] == '/';
    };
    auto add = [&](uint8 kind, ManifestNameRole role, const JsonValue& value,
                   usize entry) noexcept {
        ManifestIdName name;
        name.kind = kind;
        name.role = role;
        name.name = value.GetString();
        name.entry = static_cast<uint32>(entry);
        name.position = value.Position();
        if (auto pushed = names.TryPushBack(name); !pushed) {
            binder_.error_ = pushed.Error();
            return false;
        }

        return true;
    };
    Array<JsonValue> ids(allocator_);
    for (usize index = 0; index < entries.Size(); ++index) {
        const JsonValue item = entries.At(index);
        const JsonValue kind = item.IsObject() ? item.Find("kind") : JsonValue();
        const JsonValue name = item.IsObject() ? item.Find("name") : JsonValue();
        const JsonValue id = item.IsObject() ? item.Find("id") : JsonValue();
        const JsonValue aliases = item.IsObject() ? item.Find("aliases") : JsonValue();
        const JsonValue retired = item.IsObject() ? item.Find("retired") : JsonValue();
        if (!kind.IsValid() || !kind.IsString() || IdKindOf(kind.GetString()) == NoIdKind ||
            !name.IsValid() || !name.IsString() || !id.IsValid() || !id.IsString() ||
            !aliases.IsValid() || !aliases.IsArray() || !retired.IsValid() || !retired.IsBool()) {
            return reject(item.Position(),
                          "An ID manifest entry needs \"kind\", \"name\", \"id\", \"aliases\" and "
                          "\"retired\".");
        }

        if (!IsLowerHexId(id.GetString())) {
            return reject(id.Position(), "A stable ID is a nonzero lowercase 32-digit GUID.");
        }

        // 이 패키지의 이름 키만 둔다(C++ 이름·다른 패키지는 이 manifest에 없어야 한다).
        if (!inPackage(name.GetString())) {
            return reject(name.Position(), "The name does not belong to this package.");
        }

        if (auto pushed = ids.TryPushBack(id); !pushed) {
            binder_.error_ = pushed.Error();
            return false;
        }

        const uint8 idKind = IdKindOf(kind.GetString());
        const bool isRetired = retired.GetBool();
        if (!add(idKind, isRetired ? ManifestNameRole::Retired : ManifestNameRole::Current, name,
                 index)) {
            return false;
        }

        for (usize alias = 0; alias < aliases.Size(); ++alias) {
            const JsonValue value = aliases.At(alias);
            if (!value.IsString()) {
                return reject(value.Position(), "An alias is a name string.");
            }

            if (!inPackage(value.GetString())) {
                return reject(value.Position(), "The alias does not belong to this package.");
            }

            // 은퇴한 항목의 alias도 다시 쓰지 않는다(은퇴 이름과 같다).
            if (!add(idKind, isRetired ? ManifestNameRole::Retired : ManifestNameRole::Alias, value,
                     index)) {
                return false;
            }
        }
    }

    // 같은 (kind, 이름)이 둘이면(현재 이름·alias·은퇴 이름 사이 포함) 형식 오류다(C# Validate와
    // 같다).
    algo::Sort(names.View(), [](const ManifestIdName& left, const ManifestIdName& right) noexcept {
        if (left.kind != right.kind || left.name != right.name) {
            return NameLess(left, right);
        }

        return left.position.byteOffset < right.position.byteOffset;
    });
    for (usize index = 1; index < names.Size(); ++index) {
        if (names[index].kind == names[index - 1].kind &&
            names[index].name == names[index - 1].name) {
            return reject(names[index].position, "The name appears twice in the ID manifest.");
        }
    }

    algo::Sort(ids.View(), [](const JsonValue& left, const JsonValue& right) noexcept {
        const int compared = StringView::CompareBytes(left.GetString(), right.GetString());
        return compared != 0 ? compared < 0
                             : left.Position().byteOffset < right.Position().byteOffset;
    });
    for (usize index = 1; index < ids.Size(); ++index) {
        if (ids[index].GetString() == ids[index - 1].GetString()) {
            return reject(ids[index].Position(), "The stable ID appears twice in the ID manifest.");
        }
    }

    // 활성 멤버의 주인은 활성 type의 현재 이름이다(C# Validate와 같다).
    for (const ManifestIdName& name : names) {
        if (name.kind == TypeKind || name.role != ManifestNameRole::Current) {
            continue;
        }

        const StringView owner = IdOwnerKey(name.name);
        bool found = false;
        if (owner.ByteCount() != name.name.ByteCount()) {
            usize low = 0;
            usize high = names.Size();
            ManifestIdName probe;
            probe.kind = TypeKind;
            probe.name = owner;
            while (low < high) {
                const usize middle = low + (high - low) / 2;
                if (NameLess(names[middle], probe)) {
                    low = middle + 1;
                } else {
                    high = middle;
                }
            }

            found = low < names.Size() && names[low].kind == TypeKind && names[low].name == owner &&
                    names[low].role == ManifestNameRole::Current;
        }

        if (!found) {
            return reject(name.position,
                          "The member's owner is not an active type of the ID manifest.");
        }
    }

    return true;
}

bool TypeChecker::AddPendingId(uint8 kind, StringView name, StringView former) noexcept {
    Compilation& c = compilation_;
    Compilation::PendingIdRef entry;
    entry.kind = kind;
    entry.name.offset = static_cast<uint32>(c.text_.Size());
    entry.name.length = static_cast<uint32>(name.ByteCount());
    if (auto appended = c.text_.TryAppend(Span<const char>(name.Data(), name.ByteCount()));
        !appended) {
        binder_.error_ = appended.Error();
        return false;
    }

    entry.former.offset = static_cast<uint32>(c.text_.Size());
    entry.former.length = static_cast<uint32>(former.ByteCount());
    if (auto appended = c.text_.TryAppend(Span<const char>(former.Data(), former.ByteCount()));
        !appended) {
        binder_.error_ = appended.Error();
        return false;
    }

    if (auto pushed = c.pendingIds_.TryPushBack(entry); !pushed) {
        binder_.error_ = pushed.Error();
        return false;
    }

    return true;
}

} // namespace rocks::sparks::detail

namespace rocks::sparks {

PendingId Compilation::PendingIdAt(uint32 index) const noexcept {
    constexpr StringView Kinds[] = {"type", "field", "subobject", "function", "rpc", "enumCase"};
    const PendingIdRef& entry = pendingIds_[index];
    return PendingId{Kinds[entry.kind], Text(entry.name), Text(entry.former)};
}

Result<void> Compilation::AddIdManifest(uint32 package, SourceId id) noexcept {
    if (checked_ || package >= packages_.Size() || id.index >= sources_->Count() ||
        packages_[package].ids.present) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    packages_[package].ids = PackageIdManifest{id, true};
    return Result<void>::Success();
}

} // namespace rocks::sparks
