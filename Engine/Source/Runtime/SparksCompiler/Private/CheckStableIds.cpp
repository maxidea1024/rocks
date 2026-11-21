#include <Core/Algorithms.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>

#include "CompilationChecker.h"
#include "SchemaInput.h"
#include "StringLiteralReader.h"
#include "TypeChecker.h"

// 안정 ID(Docs/Design/SparksBytecode.ko.md 6.1절, LC4d): 선언의 이름 키를 패키지 ID manifest
// (`<패키지>.rocksids.json`)와 맞춰, 사라진 데이터 연결 ID(S1005)와 manifest 형식 오류(S1006)를
// 알리고 manifest에 없는 선언을 발급 대기 목록으로 남긴다. 컴파일러는 GUID를 만들지 않는다.

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

// 현재 선언 하나의 ID 요구다(이름 키·옛 이름 키는 keys 글자 안의 구간).
struct WantedId {
    uint8 kind = NoIdKind;
    uint32 keyOffset = 0;
    uint32 keyLength = 0;
    uint32 formerOffset = 0;
    uint32 formerLength = 0; // 0이면 `@formerly` 없음
};

bool NameLess(const ManifestIdName& left, const ManifestIdName& right) noexcept {
    if (left.kind != right.kind) {
        return left.kind < right.kind;
    }

    return StringView::CompareBytes(left.name, right.name) < 0;
}

} // namespace

void TypeChecker::CheckStableIds() noexcept {
    Compilation& c = compilation_;
    // 진단이 난 원문(구문·타입 오류 등)의 모듈은 판정하지 않는다(입력 중 미완성 — SL §12.2).
    Array<uint8> unstable(allocator_);
    if (auto resized = unstable.TryResize(c.modules_.Size()); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (usize index = 0; index < binder_.diagnostics_.Count(); ++index) {
        const SourceId source = binder_.diagnostics_[index].Source();
        for (uint32 module = 0; module < c.modules_.Size(); ++module) {
            if (c.modules_[module].source.index == source.index) {
                unstable[module] = 1;
            }
        }
    }

    for (uint32 package = 0; package < c.packages_.Size() && binder_.error_ == ErrorCode::None;
         ++package) {
        if (c.packages_[package].valid) {
            CheckPackageIds(package, unstable.View());
        }
    }
}

void TypeChecker::CheckPackageIds(uint32 package, Span<const uint8> unstable) noexcept {
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
    auto view = [&](uint32 offset, uint32 length) noexcept {
        return StringView(keys.Data() + offset, length);
    };
    // 모듈 접두 `<패키지>/<경로>`(`.spark`를 뺀다)다.
    auto modulePrefix = [&](uint32 module) noexcept {
        append(packageName);
        append("/");
        StringView path = c.Text(c.modules_[module].path);
        constexpr StringView Extension = ".spark";
        if (path.EndsWith(Extension)) {
            path = StringView(path.Data(), path.ByteCount() - Extension.ByteCount());
        }

        append(path);
    };
    // `@formerly("Old")`의 옛 이름이다(없으면 빈 글자, 디코드한 byte는 scratch에).
    Array<char> scratch(allocator_);
    auto formerly = [&](uint32 module, uint32 declaration) noexcept -> StringView {
        const SyntaxTree& tree = c.trees_[module];
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
    };
    // 주인 키(`<모듈>.<타입>`) 뒤에 멤버를 붙인 요구를 더한다.
    auto want = [&](uint8 kind, uint32 module, uint32 ownerOffset, uint32 ownerLength,
                    StringView member, StringView former) noexcept {
        WantedId entry;
        entry.kind = kind;
        entry.keyOffset = static_cast<uint32>(keys.Size());
        if (ownerLength == 0) {
            modulePrefix(module);
        } else {
            append(view(ownerOffset, ownerLength));
        }

        append(".");
        append(member);
        entry.keyLength = static_cast<uint32>(keys.Size()) - entry.keyOffset;
        if (!former.IsEmpty()) {
            entry.formerOffset = static_cast<uint32>(keys.Size());
            if (ownerLength == 0) {
                modulePrefix(module);
            } else {
                append(view(ownerOffset, ownerLength));
            }

            append(".");
            append(former);
            entry.formerLength = static_cast<uint32>(keys.Size()) - entry.formerOffset;
        }

        if (auto pushed = wanted.TryPushBack(entry); !pushed) {
            binder_.error_ = pushed.Error();
        }

        return entry;
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

            const uint32 typeSymbol = c.NodeSymbol(module, tree.Child(declaration, 2));
            if (typeSymbol == NoSymbol) {
                continue;
            }

            const WantedId type = want(TypeKind, module, 0, 0, c.symbols_[typeSymbol].name,
                                       formerly(module, declaration));
            const uint32 ownerOffset = type.keyOffset;
            const uint32 ownerLength = type.keyLength;
            const uint32 members = tree.Child(declaration, kind == SyntaxKind::Class ? 4 : 3);
            if (members == NoNode || tree.Node(members).kind != SyntaxKind::List) {
                continue;
            }

            for (const uint32 member : tree.Children(members)) {
                const SyntaxNode& entry = tree.Node(member);
                if (entry.kind == SyntaxKind::EnumCase || entry.kind == SyntaxKind::Field) {
                    const uint32 symbol = c.NodeSymbol(module, tree.Child(member, 2));
                    if (symbol == NoSymbol) {
                        continue;
                    }

                    // 소유 component(`let` 필드의 객체 생성 값)는 subobject다.
                    uint8 idKind = entry.kind == SyntaxKind::EnumCase ? EnumCaseKind : FieldKind;
                    if (entry.kind == SyntaxKind::Field && (entry.flags & SyntaxFlags::Let) != 0 &&
                        c.symbols_[symbol].type != NoType &&
                        IsObjectType(NonOptional(c.symbols_[symbol].type))) {
                        idKind = SubobjectKind;
                    }

                    want(idKind, module, ownerOffset, ownerLength, c.symbols_[symbol].name,
                         formerly(module, member));
                    continue;
                }

                if (entry.kind == SyntaxKind::Handler) {
                    // `on(<경로>)`.
                    StringBuilder<128> name(allocator_);
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

                    want(FunctionKind, module, ownerOffset, ownerLength, name.View(), StringView());
                    continue;
                }

                if (entry.kind != SyntaxKind::Function || kind != SyntaxKind::Class) {
                    continue;
                }

                const uint32 symbol = c.NodeSymbol(module, tree.Child(member, 2));
                if (symbol == NoSymbol) {
                    continue;
                }

                // FunctionId: RPC·`@cheat`·`@editorButton`·override·interface 구현(SL §12.2).
                const bool rpc = !RpcTarget(symbol).IsEmpty();
                bool functionId = rpc || HasAttribute(module, member, "cheat") ||
                                  HasAttribute(module, member, "editorButton") ||
                                  (c.symbols_[symbol].flags & SymbolFlags::Override) != 0;
                const Symbol& owner = c.symbols_[typeSymbol];
                for (uint32 at = owner.firstBase;
                     at < owner.firstBase + owner.baseCount && !functionId; ++at) {
                    const uint32 base = c.symbolBases_[at];
                    functionId =
                        base != NoSymbol && c.symbols_[base].kind == SymbolKind::Interface &&
                        FindMember(TypeOfSymbol(base), c.symbols_[symbol].name) != NoSymbol;
                }

                if (functionId) {
                    want(FunctionKind, module, ownerOffset, ownerLength, c.symbols_[symbol].name,
                         StringView());
                }

                if (rpc) {
                    want(RpcKind, module, ownerOffset, ownerLength, c.symbols_[symbol].name,
                         StringView());
                }
            }
        }
    }

    if (binder_.error_ != ErrorCode::None) {
        return;
    }

    // manifest가 없으면 모두 발급 대기다.
    const Compilation::PackageIdManifest& source = c.packages_[package].ids;
    Array<uint8> used(allocator_);
    Array<ManifestIdName> names(allocator_);
    JsonDocument document(allocator_);
    bool manifestValid = false;
    if (source.present) {
        manifestValid = ReadIdManifest(package, document, names);
        if (binder_.error_ != ErrorCode::None) {
            return;
        }

        algo::Sort(names.View(), NameLess);
        if (auto resized =
                used.TryResize(manifestValid ? document.Root().Find("entries").Size() : 0);
            !resized) {
            binder_.error_ = resized.Error();
            return;
        }
    }

    auto find = [&](uint8 kind, StringView name) noexcept -> const ManifestIdName* {
        usize low = 0;
        usize high = names.Size();
        const ManifestIdName probe{kind, name, 0};
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

    for (const WantedId& entry : wanted) {
        const StringView key = view(entry.keyOffset, entry.keyLength);
        const StringView former = view(entry.formerOffset, entry.formerLength);
        const ManifestIdName* match = manifestValid ? find(entry.kind, key) : nullptr;
        if (match != nullptr) {
            used[match->entry] = 1;
            continue;
        }

        // `@formerly`의 옛 이름이 manifest에 있으면 그 ID를 잇는다(새 이름은 발급 대기의 rename).
        const ManifestIdName* previous =
            manifestValid && entry.formerLength != 0 ? find(entry.kind, former) : nullptr;
        if (previous != nullptr) {
            used[previous->entry] = 1;
        }

        if (!AddPendingId(entry.kind, key, previous != nullptr ? former : StringView())) {
            return;
        }
    }

    if (!manifestValid) {
        return;
    }

    // 사라진 데이터 연결 항목(S1005): 판정한 모듈의 항목만 본다.
    const JsonValue entries = document.Root().Find("entries");
    const StringView text = c.sources_->Get(source.source).Text();
    for (usize index = 0; index < entries.Size() && binder_.error_ == ErrorCode::None; ++index) {
        const JsonValue item = entries.At(index);
        const uint8 kind = IdKindOf(item.Find("kind").GetString());
        if (used[index] != 0 || item.Find("retired").GetBool() ||
            (kind != TypeKind && kind != FieldKind && kind != SubobjectKind &&
             kind != EnumCaseKind)) {
            continue;
        }

        const StringView name = item.Find("name").GetString();
        if (!JudgedModule(package, name, unstable)) {
            continue;
        }

        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("{} '{}' is in the ID manifest but is no longer declared.",
                             IdKindNames[kind], name);
        if (binder_.Succeeded(message)) {
            binder_.Report(
                DiagnosticCode::DataIdLost, source.source,
                JsonValueSpan(text, item.Find("name").Position()), message.View(),
                "Saved, replicated and edited data refer to declarations by their stable "
                "ID, so that data would lose its link.",
                "Add '@formerly(\"<old name>\")' to the renamed declaration, or retire "
                "the ID with 'rocks ids retire'.");
        }
    }
}

bool TypeChecker::JudgedModule(uint32 package, StringView name,
                               Span<const uint8> unstable) const noexcept {
    // 이름 키 `<패키지>/<경로>.<…>`의 모듈이 이 컴파일에서 판정한 모듈인지다(없는 모듈은 지운 것 —
    // 판정한다, 진단이 난·분석하지 않은 모듈은 판정하지 않는다).
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
    for (uint32 module = 0; module < c.modules_.Size(); ++module) {
        StringView modulePath = c.Text(c.modules_[module].path);
        if (c.modules_[module].package != package || !modulePath.EndsWith(Extension)) {
            continue;
        }

        modulePath = StringView(modulePath.Data(), modulePath.ByteCount() - Extension.ByteCount());
        if (modulePath == path) {
            return c.nodeBase_[module] != NoNode && unstable[module] == 0;
        }
    }

    return true;
}

bool TypeChecker::ReadIdManifest(uint32 package, JsonDocument& document,
                                 Array<ManifestIdName>& names) noexcept {
    // manifest 형식(6.1절 — C++ 모듈과 같은 `schema: 1`)이다. 어긋나면 S1006 하나를 내고 false.
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
        const StringView key = name.GetString();
        if (key.ByteCount() <= packageName.ByteCount() + 1 ||
            StringView(key.Data(), packageName.ByteCount()) != packageName ||
            key.Data()[packageName.ByteCount()] != '/') {
            return reject(name.Position(), "The name does not belong to this package.");
        }

        const uint8 idKind = IdKindOf(kind.GetString());
        if (retired.GetBool()) {
            continue; // 은퇴한 이름은 다시 쓰지 않는다(발급 대기로 남는다 — 도구가 거부한다)
        }

        if (auto pushed =
                names.TryPushBack(ManifestIdName{idKind, key, static_cast<uint32>(index)});
            !pushed) {
            binder_.error_ = pushed.Error();
            return false;
        }

        for (usize alias = 0; alias < aliases.Size(); ++alias) {
            const JsonValue value = aliases.At(alias);
            if (!value.IsString()) {
                return reject(value.Position(), "An alias is a name string.");
            }

            if (auto pushed = names.TryPushBack(
                    ManifestIdName{idKind, value.GetString(), static_cast<uint32>(index)});
                !pushed) {
                binder_.error_ = pushed.Error();
                return false;
            }
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
