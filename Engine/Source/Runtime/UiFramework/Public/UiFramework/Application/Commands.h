#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Array.h>
#include <Core/Delegate.h>
#include <Core/Text/String.h>
#include <UiCore/KeyChord.h>

namespace rocks::ui {

// 왼쪽과 오른쪽 보조 키를 따로 추적한다. 라우터와 키 선택기가 같은 조합을 만든다.
ROCKS_UIFRAMEWORK_API uint8 KeyModifierBit(input::Key key) noexcept;
ROCKS_UIFRAMEWORK_API KeyChord ChordFromModifiers(input::Key key, uint8 mask) noexcept;
ROCKS_UIFRAMEWORK_API Result<input::Key> FindChordKey(StringView name) noexcept;

struct CommandChords {
    KeyChord primary;
    KeyChord secondary;
};

struct CommandInfo {
    Guid id;
    String label;
    String description;
    CommandChords defaults;
};

class ROCKS_UIFRAMEWORK_API CommandSet {
public:
    explicit CommandSet(Allocator& allocator) noexcept;
    Result<void> Add(Guid id, StringView label, StringView description,
                     CommandChords defaults = {}) noexcept;
    const CommandInfo* Find(Guid id) const noexcept;

private:
    Array<CommandInfo> commands_;
};

class ROCKS_UIFRAMEWORK_API BindingManager {
public:
    explicit BindingManager(Allocator& allocator) noexcept;
    Result<void> Override(Guid id, CommandChords chords) noexcept;
    bool RestoreDefault(Guid id) noexcept;
    CommandChords Chords(const CommandInfo& command) const noexcept;
    Result<void> Save(Array<uint8>& bytes) const noexcept;
    Result<void> Load(Span<const uint8> bytes) noexcept;

private:
    struct Entry {
        Guid id;
        CommandChords chords;
    };
    Array<Entry> entries_;
};

struct CommandAction {
    Delegate<void()> execute{};
    Delegate<bool()> canExecute{};
    Delegate<bool()> isChecked{};
    Delegate<bool()> isVisible{};
    bool repeat = false;
};

class ROCKS_UIFRAMEWORK_API CommandList {
public:
    CommandList(Allocator& allocator, const CommandSet& commands,
                const BindingManager& bindings) noexcept;
    Result<void> Map(Guid id, CommandAction action) noexcept;
    bool Unmap(Guid id) noexcept;
    bool Execute(Guid id) const noexcept;
    bool Process(KeyChord chord, bool repeat) const noexcept;
    bool IsEnabled(Guid id) const noexcept;
    bool IsVisible(Guid id) const noexcept;
    bool IsChecked(Guid id) const noexcept;

private:
    struct Entry {
        Guid id;
        SharedPtr<CommandAction> action;
    };
    // 콜백 안에서도 명령을 해제할 수 있으므로 실행 중인 동작을 별도로 유지한다.
    SharedPtr<CommandAction> Action(Guid id) const noexcept;
    const CommandSet& commands_;
    const BindingManager& bindings_;
    Array<Entry> actions_;
};

} // namespace rocks::ui
