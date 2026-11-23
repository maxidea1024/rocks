#include "IdManifest.h"
#include <Core/Algorithms.h>
#include <Core/Move.h>
#include <Core/Text/Json.h>

namespace rocks::sparks::tools::detail {

namespace {

constexpr int64 ManifestSchema = 1;

bool ByteLess(StringView left, StringView right) noexcept {
    return StringView::CompareBytes(left, right) < 0;
}

// 문자열 값을 out에 옮긴다. 값 false는 문자열이 아님.
Result<bool> TakeString(const JsonValue& value, String& out) noexcept {
    if (!value.IsValid() || !value.IsString()) {
        return Result<bool>::Success(false);
    }

    if (auto assigned = out.TryAssign(value.GetString()); !assigned) {
        return Result<bool>::Failure(assigned.Error());
    }

    return Result<bool>::Success(true);
}

// (kind, name)이 어느 항목의 이름·alias인지다(은퇴 항목 포함 — C# Validate의 이름 집합).
bool NameTaken(const Array<IdEntry>& entries, StringView kind, StringView name) noexcept {
    for (const IdEntry& entry : entries) {
        if (entry.kind.View() != kind) {
            continue;
        }

        if (entry.name.View() == name) {
            return true;
        }

        for (const String& alias : entry.aliases) {
            if (alias.View() == name) {
                return true;
            }
        }
    }

    return false;
}

} // namespace

bool IsMemberIdKind(StringView kind) noexcept {
    return kind == "field" || kind == "subobject" || kind == "function" || kind == "rpc" ||
           kind == "enumCase";
}

StringView IdOwner(StringView name) noexcept {
    usize slash = 0;
    for (usize at = 0; at < name.ByteCount(); ++at) {
        if (name.Data()[at] == '/') {
            slash = at + 1;
        }
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

Result<bool> ReadIdEntries(Allocator& allocator, StringView text,
                           Array<IdEntry>& entries) noexcept {
    JsonDocument document(allocator);
    const JsonError parsed = document.Parse(text);
    if (parsed.code == JsonErrorCode::OutOfMemory) {
        return Result<bool>::Failure(ErrorCode::OutOfMemory);
    }

    const JsonValue root = document.Root();
    const JsonValue schema = parsed.Succeeded() ? root.Find("schema") : JsonValue();
    const JsonValue list = parsed.Succeeded() ? root.Find("entries") : JsonValue();
    if (!schema.IsValid() || !schema.IsNumber() || !schema.TryGetInt64() ||
        schema.TryGetInt64().Value() != ManifestSchema || !list.IsValid() || !list.IsArray()) {
        return Result<bool>::Success(false);
    }

    for (usize index = 0; index < list.Size(); ++index) {
        const JsonValue item = list.At(index);
        IdEntry entry(allocator);
        bool ok = item.IsObject();
        String* const targets[] = {&entry.kind, &entry.name, &entry.id};
        const StringView keys[] = {"kind", "name", "id"};
        for (usize field = 0; ok && field < sizeof(keys) / sizeof(keys[0]); ++field) {
            auto taken = TakeString(item.Find(keys[field]), *targets[field]);
            if (!taken) {
                return Result<bool>::Failure(taken.Error());
            }

            ok = taken.Value();
        }

        const JsonValue aliases = ok ? item.Find("aliases") : JsonValue();
        const JsonValue retired = ok ? item.Find("retired") : JsonValue();
        ok = ok && aliases.IsValid() && aliases.IsArray() && retired.IsValid() && retired.IsBool();
        for (usize alias = 0; ok && alias < aliases.Size(); ++alias) {
            String name(allocator);
            auto taken = TakeString(aliases.At(alias), name);
            if (!taken) {
                return Result<bool>::Failure(taken.Error());
            }

            ok = taken.Value();
            if (ok) {
                if (auto pushed = entry.aliases.TryPushBack(Move(name)); !pushed) {
                    return Result<bool>::Failure(pushed.Error());
                }
            }
        }

        if (!ok) {
            return Result<bool>::Success(false);
        }

        entry.retired = retired.GetBool();
        if (auto pushed = entries.TryPushBack(Move(entry)); !pushed) {
            return Result<bool>::Failure(pushed.Error());
        }
    }

    return Result<bool>::Success(true);
}

Result<bool> RenameIdEntry(Allocator& allocator, Array<IdEntry>& entries, StringView kind,
                           StringView from, StringView to) noexcept {
    usize found = entries.Size();
    for (usize at = 0; at < entries.Size(); ++at) {
        if (entries[at].kind.View() == kind && entries[at].name.View() == from &&
            !entries[at].retired) {
            found = at;
            break;
        }
    }

    if (found == entries.Size() || (IsMemberIdKind(kind) && IdOwner(from) != IdOwner(to))) {
        return Result<bool>::Success(false);
    }

    if (from == to) {
        return Result<bool>::Success(true);
    }

    // 새 이름(type이면 옮길 멤버의 새 이름까지)이 이미 있으면 바꾸지 않는다.
    if (NameTaken(entries, kind, to)) {
        return Result<bool>::Success(false);
    }

    if (kind == "type") {
        String moved(allocator);
        for (const IdEntry& entry : entries) {
            const StringView name = entry.name.View();
            if (!IsMemberIdKind(entry.kind.View()) || IdOwner(name) != from) {
                continue;
            }

            moved.Clear();
            if (!moved.TryAppend(to) ||
                !moved.TryAppend(StringView(name.Data() + from.ByteCount(),
                                            name.ByteCount() - from.ByteCount()))) {
                return Result<bool>::Failure(ErrorCode::OutOfMemory);
            }

            if (NameTaken(entries, entry.kind.View(), moved.View())) {
                return Result<bool>::Success(false);
            }
        }
    }

    // 이름 하나를 바꾸고 옛 이름을 aliases에 더한다.
    auto rename = [&](IdEntry& entry, StringView prefix, StringView rest) noexcept {
        String old(allocator);
        String next(allocator);
        if (!old.TryAssign(entry.name.View()) || !next.TryAssign(prefix) || !next.TryAppend(rest) ||
            !entry.aliases.TryPushBack(Move(old))) {
            return false;
        }

        entry.name = Move(next);
        return true;
    };
    if (!rename(entries[found], to, StringView())) {
        return Result<bool>::Failure(ErrorCode::OutOfMemory);
    }

    if (kind == "type") {
        for (IdEntry& entry : entries) {
            if (!IsMemberIdKind(entry.kind.View()) || IdOwner(entry.name.View()) != from) {
                continue;
            }

            const StringView name = entry.name.View();
            if (!rename(entry, to,
                        StringView(name.Data() + from.ByteCount(),
                                   name.ByteCount() - from.ByteCount()))) {
                return Result<bool>::Failure(ErrorCode::OutOfMemory);
            }
        }
    }

    return Result<bool>::Success(true);
}

Result<bool> AddIdEntry(Allocator& allocator, Array<IdEntry>& entries, StringView kind,
                        StringView name, StringView id) noexcept {
    if (NameTaken(entries, kind, name)) {
        return Result<bool>::Success(false);
    }

    IdEntry entry(allocator);
    if (!entry.kind.TryAssign(kind) || !entry.name.TryAssign(name) || !entry.id.TryAssign(id) ||
        !entries.TryPushBack(Move(entry))) {
        return Result<bool>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<bool>::Success(true);
}

Result<void> AppendIdManifest(Allocator& allocator, StringBuilderBase& out,
                              Span<const IdEntry> entries) noexcept {
    Array<usize> order(allocator);
    if (auto resized = order.TryResize(entries.Size()); !resized) {
        return resized;
    }

    for (usize at = 0; at < order.Size(); ++at) {
        order[at] = at;
    }

    algo::Sort(order.View(), [&](usize left, usize right) noexcept {
        const int32 kind =
            StringView::CompareBytes(entries[left].kind.View(), entries[right].kind.View());
        return kind != 0 ? kind < 0
                         : ByteLess(entries[left].name.View(), entries[right].name.View());
    });
    JsonWriter writer(out);
    Array<StringView> aliases(allocator);
    bool ok = writer.BeginObject() && writer.Key("schema") && writer.Int64(ManifestSchema) &&
              writer.Key("entries") && writer.BeginArray();
    for (usize at = 0; ok && at < order.Size(); ++at) {
        const IdEntry& entry = entries[order[at]];
        aliases.Clear();
        for (const String& alias : entry.aliases) {
            if (auto pushed = aliases.TryPushBack(alias.View()); !pushed) {
                return pushed;
            }
        }

        algo::Sort(aliases.View(), ByteLess);
        ok = writer.BeginObject() && writer.Key("kind") && writer.String(entry.kind.View()) &&
             writer.Key("name") && writer.String(entry.name.View()) && writer.Key("id") &&
             writer.String(entry.id.View()) && writer.Key("aliases") && writer.BeginArray();
        for (usize alias = 0; ok && alias < aliases.Size(); ++alias) {
            ok = static_cast<bool>(writer.String(aliases[alias]));
        }

        ok = ok && writer.EndArray() && writer.Key("retired") && writer.Bool(entry.retired) &&
             writer.EndObject();
    }

    ok = ok && writer.EndArray() && writer.EndObject();
    return writer.Finish();
}

} // namespace rocks::sparks::tools::detail
