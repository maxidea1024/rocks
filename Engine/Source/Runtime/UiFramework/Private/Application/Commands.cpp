#include <UiFramework/Application/Commands.h>
#include <Core/Encoding/ByteReader.h>
#include <Core/Encoding/ByteWriter.h>

namespace rocks::ui {

namespace {

// 보조 키 상태 bit다(왼쪽·오른쪽 키마다 하나 — KeyModifierBit).
constexpr uint8 LeftShiftBit = 1u << 0;
constexpr uint8 RightShiftBit = 1u << 1;
constexpr uint8 LeftControlBit = 1u << 2;
constexpr uint8 RightControlBit = 1u << 3;
constexpr uint8 LeftAltBit = 1u << 4;
constexpr uint8 RightAltBit = 1u << 5;
constexpr uint8 LeftSuperBit = 1u << 6;
constexpr uint8 RightSuperBit = 1u << 7;
constexpr uint8 ShiftBits = LeftShiftBit | RightShiftBit;
constexpr uint8 ControlBits = LeftControlBit | RightControlBit;
constexpr uint8 AltBits = LeftAltBit | RightAltBit;
constexpr uint8 SuperBits = LeftSuperBit | RightSuperBit;
// 저장한 단축키의 보조 키 flag다(shift·control·alt·super 순서).
constexpr uint8 SavedShiftFlag = 1u << 0;
constexpr uint8 SavedControlFlag = 1u << 1;
constexpr uint8 SavedAltFlag = 1u << 2;
constexpr uint8 SavedSuperFlag = 1u << 3;
// 단축키 바인딩 파일의 표지("RCHD" little-endian)와 형식 버전이다.
constexpr uint32 BindingFileMagic = 0x44484352;
constexpr uint32 BindingFileFormat = 1;
constexpr uint8 SavedFlagMask = SavedShiftFlag | SavedControlFlag | SavedAltFlag | SavedSuperFlag;

} // namespace

uint8 KeyModifierBit(input::Key key) noexcept {
    switch (key) {
    case input::Key::LeftShift:
        return LeftShiftBit;
    case input::Key::RightShift:
        return RightShiftBit;
    case input::Key::LeftControl:
        return LeftControlBit;
    case input::Key::RightControl:
        return RightControlBit;
    case input::Key::LeftAlt:
        return LeftAltBit;
    case input::Key::RightAlt:
        return RightAltBit;
    case input::Key::LeftSuper:
        return LeftSuperBit;
    case input::Key::RightSuper:
        return RightSuperBit;
    default:
        return 0;
    }
}

KeyChord ChordFromModifiers(input::Key key, uint8 mask) noexcept {
    return {key, (mask & ShiftBits) != 0, (mask & ControlBits) != 0, (mask & AltBits) != 0,
            (mask & SuperBits) != 0};
}

Result<input::Key> FindChordKey(StringView name) noexcept {
    auto chord = KeyChord::FromInputName(name);
    if (!chord || chord.Value().mouse != input::MouseButton::Count ||
        chord.Value().gamepad != input::GamepadButton::Count) {
        return Result<input::Key>::Failure(ErrorCode::NotFound);
    }

    return Result<input::Key>::Success(chord.Value().key);
}

namespace {

bool Valid(KeyChord chord) noexcept {
    return chord.IsValid() && chord.mouse == input::MouseButton::Count &&
           chord.gamepad == input::GamepadButton::Count;
}

bool Valid(CommandChords chords) noexcept {
    return Valid(chords.primary) && Valid(chords.secondary) &&
           (chords.primary.key == input::Key::None || chords.primary != chords.secondary);
}

void WriteChord(ByteWriter& writer, KeyChord chord) noexcept {
    const StringView key = input::KeyName(chord.key);
    writer.WriteU8(static_cast<uint8>(key.ByteCount()));
    writer.WriteBytes({reinterpret_cast<const uint8*>(key.Data()), key.ByteCount()});
    writer.WriteU8(static_cast<uint8>(
        (chord.shift ? SavedShiftFlag : 0) | (chord.control ? SavedControlFlag : 0) |
        (chord.alt ? SavedAltFlag : 0) | (chord.super ? SavedSuperFlag : 0)));
}

Result<KeyChord> ReadChord(ByteReader& reader) noexcept {
    const auto bytes = reader.Take(reader.ReadU8());
    const auto flags = reader.ReadU8();
    if (reader.Failed() || bytes.IsEmpty() || (flags & ~SavedFlagMask) != 0) {
        return Result<KeyChord>::Failure(ErrorCode::CorruptData);
    }

    StringView name(reinterpret_cast<const char*>(bytes.Data()), bytes.Size());
    auto key = FindChordKey(name);
    if (!key) {
        return Result<KeyChord>::Failure(key.Error());
    }

    return Result<KeyChord>::Success({key.Value(), (flags & SavedShiftFlag) != 0,
                                      (flags & SavedControlFlag) != 0, (flags & SavedAltFlag) != 0,
                                      (flags & SavedSuperFlag) != 0});
}

bool RunAction(const SharedPtr<CommandAction>& action) noexcept {
    if (!action || (action->isVisible && !action->isVisible.Execute()) ||
        (action->canExecute && !action->canExecute.Execute())) {
        return false;
    }

    return action->execute.ExecuteIfBound();
}

} // namespace

CommandSet::CommandSet(Allocator& allocator) noexcept : commands_(allocator) {
}

const CommandInfo* CommandSet::Find(Guid id) const noexcept {
    for (const auto& command : commands_) {
        if (command.id == id) {
            return &command;
        }
    }

    return nullptr;
}

Result<void> CommandSet::Add(Guid id, StringView label, StringView description,
                             CommandChords defaults) noexcept {
    if (id == Guid{} || label.IsEmpty() || !Valid(defaults)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (Find(id)) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    auto& alloc = commands_.GetAllocator();
    CommandInfo command{id, String(alloc), String(alloc), defaults};
    auto stored = command.label.TryAssign(label);
    if (stored) {
        stored = command.description.TryAssign(description);
    }

    if (!stored) {
        return stored;
    }

    return commands_.TryPushBack(Move(command));
}

BindingManager::BindingManager(Allocator& allocator) noexcept : entries_(allocator) {
}

Result<void> BindingManager::Override(Guid id, CommandChords chords) noexcept {
    if (id == Guid{} || !Valid(chords)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 저장 순서는 등록 순서와 무관하게 명령 ID 순으로 고정한다.
    usize index = 0;
    while (index < entries_.Size() && GuidLess(entries_[index].id, id)) {
        ++index;
    }

    if (index < entries_.Size() && entries_[index].id == id) {
        entries_[index].chords = chords;
        return Result<void>::Success();
    }

    return entries_.TryInsert({id, chords}, index);
}

bool BindingManager::RestoreDefault(Guid id) noexcept {
    for (usize i = 0; i < entries_.Size(); ++i) {
        if (entries_[i].id == id) {
            (void)entries_.TryRemoveAt(i);
            return true;
        }
    }

    return false;
}

CommandChords BindingManager::Chords(const CommandInfo& command) const noexcept {
    for (const auto& entry : entries_) {
        if (entry.id == command.id) {
            return entry.chords;
        }
    }

    return command.defaults;
}

Result<void> BindingManager::Save(Array<uint8>& bytes) const noexcept {
    Array<uint8> snapshot(bytes.GetAllocator());
    ByteWriter writer(snapshot);
    writer.WriteU32LE(BindingFileMagic);
    writer.WriteU32LE(BindingFileFormat);
    for (const auto& entry : entries_) {
        writer.WriteU64LE(entry.id.high);
        writer.WriteU64LE(entry.id.low);
        WriteChord(writer, entry.chords.primary);
        WriteChord(writer, entry.chords.secondary);
    }

    auto written = writer.Status();
    if (written) {
        bytes = Move(snapshot);
    }

    return written;
}

Result<void> BindingManager::Load(Span<const uint8> bytes) noexcept {
    ByteReader reader(bytes);
    const uint32 magic = reader.ReadU32LE();
    const uint32 format = reader.ReadU32LE();
    if (reader.Failed() || magic != BindingFileMagic) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (format != BindingFileFormat) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    Array<Entry> loaded(entries_.GetAllocator());
    while (reader.Remaining()) {
        Guid id{reader.ReadU64LE(), reader.ReadU64LE()};
        auto first = ReadChord(reader);
        auto second = ReadChord(reader);
        if (!first || !second || id == Guid{} ||
            (!loaded.IsEmpty() && !GuidLess(loaded[loaded.Size() - 1].id, id))) {
            return Result<void>::Failure(!first && first.Error() != ErrorCode::CorruptData
                                             ? first.Error()
                                             : ErrorCode::CorruptData);
        }

        const CommandChords chords{first.Value(), second.Value()};
        if (!Valid(chords)) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        auto stored = loaded.TryPushBack({id, chords});
        if (!stored) {
            return stored;
        }
    }

    entries_ = Move(loaded);
    return Result<void>::Success();
}

CommandList::CommandList(Allocator& allocator, const CommandSet& commands,
                         const BindingManager& bindings) noexcept
    : commands_(commands), bindings_(bindings), actions_(allocator) {
}

SharedPtr<CommandAction> CommandList::Action(Guid id) const noexcept {
    for (const auto& entry : actions_) {
        if (entry.id == id) {
            return entry.action;
        }
    }

    return {};
}

Result<void> CommandList::Map(Guid id, CommandAction action) noexcept {
    if (!commands_.Find(id) || !action.execute) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (Action(id)) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    auto stored = SharedPtr<CommandAction>::TryMake(actions_.GetAllocator(), Move(action));
    if (!stored) {
        return Result<void>::Failure(stored.Error());
    }

    return actions_.TryPushBack({id, Move(stored.Value())});
}

bool CommandList::Unmap(Guid id) noexcept {
    for (usize i = 0; i < actions_.Size(); ++i) {
        if (actions_[i].id == id) {
            (void)actions_.TryRemoveAt(i);
            return true;
        }
    }

    return false;
}

bool CommandList::Execute(Guid id) const noexcept {
    return RunAction(Action(id));
}

bool CommandList::IsEnabled(Guid id) const noexcept {
    auto action = Action(id);
    return action && action->execute && (!action->canExecute || action->canExecute.Execute());
}

bool CommandList::IsChecked(Guid id) const noexcept {
    auto action = Action(id);
    return action && action->isChecked && action->isChecked.Execute();
}

bool CommandList::IsVisible(Guid id) const noexcept {
    auto action = Action(id);
    return action && (!action->isVisible || action->isVisible.Execute());
}

bool CommandList::Process(KeyChord chord, bool repeat) const noexcept {
    if (chord.key == input::Key::None) {
        return false;
    }

    for (const auto& entry : actions_) {
        auto keys = bindings_.Chords(*commands_.Find(entry.id));
        if ((chord == keys.primary || chord == keys.secondary) &&
            (!repeat || entry.action->repeat)) {
            const auto action = entry.action;
            return RunAction(action);
        }
    }

    return false;
}
} // namespace rocks::ui
