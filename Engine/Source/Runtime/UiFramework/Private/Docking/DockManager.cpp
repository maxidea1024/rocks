#include <UiFramework/Docking/DockManager.h>
#include <UiFramework/SizeBox.h>

#include "DockTabHeader.h"

namespace rocks::ui {

DockManager::Node::Node(Allocator& allocator) noexcept : tabs(allocator) {
}

DockManager::Tab::Tab(Allocator& allocator) noexcept
    : id(allocator), displayName(allocator), group(allocator) {
}

DockManager::DockManager(Allocator& allocator) noexcept
    : Element(allocator), allocator_(allocator), tabs_(allocator), nodes_(allocator),
      visible_(allocator) {
    SetClipToBounds(true);
}

Result<void> DockManager::TryAddChild(const SharedPtr<Element>&) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

DockManager::~DockManager() noexcept {
    ReleasePreview();
}

void DockManager::CloseWindowTabs() noexcept {
    EndDrag();
    root_ = {};
    nodes_.Clear();
    for (usize i = 0; i < tabs_.Size(); ++i) {
        tabs_[i].stack = {};
        DropViews(i);
    }

    RebuildChildren();
}

void DockManager::SetWindowHitTest(DockWindowHitTest&& callback) noexcept {
    EndDrag();
    windowHitTest_ = Move(callback);
}

bool DockManager::RemoveChild(Element&) noexcept {
    return false;
}

usize DockManager::FindTab(StringView id) const noexcept {
    for (usize i = 0; i < tabs_.Size(); ++i) {
        if (tabs_[i].id.View() == id) {
            return i;
        }
    }

    return MaxSize;
}

Result<void> DockManager::RegisterTab(const DockTab& tab) noexcept {
    if (spawning_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (!tab.header || !tab.content || tab.header.Get() == tab.content.Get() || tab.id.IsEmpty() ||
        tab.role > DockRole::Document || tab.header->Parent() || tab.content->Parent() ||
        !tab.id.ValidateUtf8()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (FindTab(tab.id) != MaxSize) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    if (tabs_.Size() >= MaxSize / 2) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    Tab record(allocator_);
    auto result = record.id.TryAssign(tab.id);
    if (!result) {
        return result;
    }

    result = visible_.TryEnsureCapacity(2 * (tabs_.Size() + 1));
    if (!result) {
        return result;
    }

    result = tabs_.TryEnsureCapacity(tabs_.Size() + 1);
    if (!result) {
        return result;
    }

    result = AttachViews(record, {tab.header, tab.content});
    if (!result) {
        return result;
    }

    record.role = tab.role;
    record.closable = tab.closable;
    return tabs_.TryPushBack(Move(record));
}

Result<void> DockManager::AttachViews(Tab& tab, const DockTabViews& views) noexcept {
    if (!views.header || !views.content || views.header.Get() == views.content.Get() ||
        views.header->Parent() || views.content->Parent()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 패널이 자기 영역 밖에 그리더라도 이웃 탭의 헤더나 입력 영역을 덮지 않게 한다. 머리는 UE
    // SDockTab처럼 탭 여백·상태별 brush·전경색을 맡는 DockTabHeader로 감싼다(스스로 자른다).
    auto header = SharedPtr<DockTabHeader>::TryMake(allocator_, allocator_, DefaultDockTabStyle());
    auto body = SharedPtr<SizeBox>::TryMake(allocator_, allocator_);
    if (!header) {
        return Result<void>::Failure(header.Error());
    }

    if (!body) {
        return Result<void>::Failure(body.Error());
    }

    body.Value()->SetClipToBounds(true);
    auto added = header.Value()->TryAddChild(views.header);
    if (!added) {
        return added;
    }

    added = body.Value()->TryAddChild(views.content);
    if (!added) {
        return added;
    }

    added = Element::TryAddChild(header.Value());
    if (!added) {
        return added;
    }

    added = Element::TryAddChild(body.Value());
    if (!added) {
        (void)Element::RemoveChild(*header.Value());
        return added;
    }

    tab.header = header.Value();
    tab.content = body.Value();
    return Result<void>::Success();
}

Result<void> DockManager::RegisterSpawner(StringView id, StringView displayName, StringView group,
                                          DockRole role, bool listed, bool closable,
                                          DockTabSpawner&& spawn) noexcept {
    if (spawning_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (id.IsEmpty() || !id.ValidateUtf8() || !displayName.ValidateUtf8() ||
        !group.ValidateUtf8() || !spawn.IsBound() || role > DockRole::Document) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (FindTab(id) != MaxSize) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    if (tabs_.Size() >= MaxSize / 2) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    Tab tab(allocator_);
    auto set = tab.id.TryAssign(id);
    if (!set) {
        return set;
    }

    set = tab.displayName.TryAssign(displayName);
    if (!set) {
        return set;
    }

    set = tab.group.TryAssign(group);
    if (!set) {
        return set;
    }

    set = visible_.TryEnsureCapacity(2 * (tabs_.Size() + 1));
    if (!set) {
        return set;
    }

    set = tabs_.TryEnsureCapacity(tabs_.Size() + 1);
    if (!set) {
        return set;
    }

    tab.role = role;
    tab.listed = listed;
    tab.closable = closable;
    tab.lazy = true;
    tab.spawn = Move(spawn);
    return tabs_.TryPushBack(Move(tab));
}

usize DockManager::RegisteredCount() const noexcept {
    return tabs_.Size();
}

Result<DockTabInfo> DockManager::TabInfo(usize index) const noexcept {
    if (index >= tabs_.Size()) {
        return Result<DockTabInfo>::Failure(ErrorCode::InvalidArgument);
    }

    const Tab& tab = tabs_[index];
    return Result<DockTabInfo>::Success(
        {tab.id.View(), tab.displayName.View().IsEmpty() ? tab.id.View() : tab.displayName.View(),
         tab.group.View(), tab.role, tab.listed, !tab.stack.IsNull()});
}

Result<void> DockManager::EnsureViews(usize index) noexcept {
    Tab& tab = tabs_[index];
    if (tab.header) {
        return Result<void>::Success();
    }

    if (!tab.spawn.IsBound()) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    spawning_ = true;
    auto result = tab.spawn.Execute(allocator_);
    spawning_ = false;
    if (!result) {
        return Result<void>::Failure(result.Error());
    }

    return AttachViews(tab, result.Value());
}

void DockManager::DropViews(usize index) noexcept {
    Tab& tab = tabs_[index];
    if (!tab.lazy || !tab.header) {
        return;
    }

    (void)Element::RemoveChild(*tab.header);
    (void)Element::RemoveChild(*tab.content);
    tab.header.Reset();
    tab.content.Reset();
}

bool DockManager::IsOpen(StringView id) const noexcept {
    const usize i = FindTab(id);
    return i != MaxSize && !tabs_[i].stack.IsNull();
}

bool DockManager::IsActive(StringView id) const noexcept {
    const usize i = FindTab(id);
    const Node* node = i == MaxSize ? nullptr : nodes_.Find(tabs_[i].stack);
    return node && node->tabs[node->selected] == i;
}

void DockManager::RebuildChildren() noexcept {
    visible_.Clear();
    for (const auto& node : nodes_) {
        if (node.kind != Kind::Stack || node.tabs.IsEmpty()) {
            continue;
        }

        for (usize k = 0; k < node.tabs.Size(); ++k) {
            const auto& header = tabs_[node.tabs[k]].header;
            static_cast<DockTabHeader&>(*header).SetForeground(k == node.selected);
            (void)visible_.TryPushBack(header);
        }

        (void)visible_.TryPushBack(tabs_[node.tabs[node.selected]].content);
    }

    InvalidateLayout();
}

Span<const SharedPtr<Element>> DockManager::ActiveChildren() const noexcept {
    return visible_.View();
}

Result<void> DockManager::OpenTab(StringView id) noexcept {
    if (spawning_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    const usize i = FindTab(id);
    if (i == MaxSize) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (!tabs_[i].stack.IsNull()) {
        return ActivateTab(id);
    }

    auto target = root_;
    while (!target.IsNull() && nodes_[target].kind != Kind::Stack) {
        target = nodes_[target].first;
    }

    return Place(i, target, DockSide::Center, MaxSize);
}

Result<void> DockManager::ActivateTab(StringView id) noexcept {
    if (spawning_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    const usize index = FindTab(id);
    if (index == MaxSize || tabs_[index].stack.IsNull()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    Node& node = nodes_[tabs_[index].stack];
    for (usize i = 0; i < node.tabs.Size(); ++i) {
        if (node.tabs[i] == index) {
            if (node.selected != i) {
                node.selected = i;
                RebuildChildren();
            }

            return Result<void>::Success();
        }
    }

    return Result<void>::Failure(ErrorCode::WrongContext);
}

void DockManager::ReplaceNode(SlotHandle oldNode, SlotHandle replacement) noexcept {
    const auto parent = nodes_[oldNode].parent;
    if (parent.IsNull()) {
        root_ = replacement;
    } else if (nodes_[parent].first == oldNode) {
        nodes_[parent].first = replacement;
    } else {
        nodes_[parent].second = replacement;
    }

    if (!replacement.IsNull()) {
        nodes_[replacement].parent = parent;
    }
}

void DockManager::CollapseEmpty(SlotHandle stack) noexcept {
    if (stack.IsNull() || !nodes_[stack].tabs.IsEmpty()) {
        return;
    }

    const auto parent = nodes_[stack].parent;
    if (parent.IsNull()) {
        root_ = {};
    } else {
        const auto sibling =
            nodes_[parent].first == stack ? nodes_[parent].second : nodes_[parent].first;
        ReplaceNode(parent, sibling);
        (void)nodes_.TryRemove(parent);
    }

    (void)nodes_.TryRemove(stack);
}

void DockManager::DetachTab(usize index) noexcept {
    const auto stack = tabs_[index].stack;
    if (stack.IsNull()) {
        return;
    }

    Node& node = nodes_[stack];
    for (usize i = 0; i < node.tabs.Size(); ++i) {
        if (node.tabs[i] != index) {
            continue;
        }

        (void)node.tabs.TryRemoveAt(i);
        if (node.selected > i || (node.selected && node.selected == node.tabs.Size())) {
            --node.selected;
        }
        break;
    }

    tabs_[index].stack = {};
}

Result<void> DockManager::CloseTab(StringView id) noexcept {
    if (spawning_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    const usize i = FindTab(id);
    if (i == MaxSize || tabs_[i].stack.IsNull()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (!tabs_[i].closable) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    EndDrag();
    const auto stack = tabs_[i].stack;
    DetachTab(i);
    CollapseEmpty(stack);
    RebuildChildren();
    DropViews(i);
    return Result<void>::Success();
}

Result<void> DockManager::Dock(StringView id, StringView relativeTo, DockSide side,
                               float share) noexcept {
    if (spawning_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    const usize i = FindTab(id);
    const usize other = FindTab(relativeTo);
    if (i == MaxSize || other == MaxSize || tabs_[other].stack.IsNull()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    return Place(i, tabs_[other].stack, side, MaxSize, share);
}

void DockManager::SetOnExternalDrop(Delegate<void(StringView, Vec2)>&& callback) noexcept {
    externalDrop_ = Move(callback);
}

Result<void> DockManager::TransferTabTo(StringView id, DockManager& destination,
                                        StringView relativeTo, DockSide side) noexcept {
    if (spawning_ || destination.spawning_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (this == &destination || side > DockSide::Bottom) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const usize index = FindTab(id);
    if (index == MaxSize) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (destination.FindTab(id) != MaxSize) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    SlotHandle target = destination.root_;
    if (!relativeTo.IsEmpty()) {
        const usize other = destination.FindTab(relativeTo);
        if (other == MaxSize || destination.tabs_[other].stack.IsNull()) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        target = destination.tabs_[other].stack;
    } else {
        while (!target.IsNull() && destination.nodes_[target].kind != Kind::Stack) {
            target = destination.nodes_[target].first;
        }
    }

    return Transfer(index, destination, target, side, MaxSize);
}

Result<void> DockManager::Transfer(usize index, DockManager& destination, SlotHandle target,
                                   DockSide side, usize insertion) noexcept {
    if (&allocator_ != &destination.allocator_) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    if (destination.tabs_.Size() >= MaxSize / 2) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    auto prepared = destination.visible_.TryEnsureCapacity(2 * (destination.tabs_.Size() + 1));
    if (!prepared) {
        return prepared;
    }

    prepared = destination.tabs_.TryEnsureCapacity(destination.tabs_.Size() + 1);
    if (!prepared) {
        return prepared;
    }

    // 생성기는 호출하지 않는다. 대상 배치가 실패하면 원래 부모와 탭 순서를 그대로 복구한다.
    const auto header = tabs_[index].header;
    const auto content = tabs_[index].content;
    auto attached = Result<void>::Success();
    if (header) {
        (void)Element::RemoveChild(*header);
        (void)Element::RemoveChild(*content);
        attached = destination.Element::TryAddChild(header);
        if (attached) {
            attached = destination.Element::TryAddChild(content);
        }
    }

    if (attached) {
        Tab staged(destination.allocator_);
        staged.header = header;
        staged.content = content;
        const usize next = destination.tabs_.Size();
        (void)destination.tabs_.TryPushBack(Move(staged));
        if (!tabs_[index].stack.IsNull()) {
            attached = destination.Place(next, target, side, insertion);
        }

        if (attached) {
            const auto stack = destination.tabs_[next].stack;
            const auto oldStack = tabs_[index].stack;
            DetachTab(index);
            CollapseEmpty(oldStack);
            destination.tabs_[next] = Move(tabs_[index]);
            destination.tabs_[next].stack = stack;
            (void)tabs_.TryRemoveAt(index);
            for (Node& node : nodes_) {
                for (usize& tab : node.tabs) {
                    if (tab > index) {
                        --tab;
                    }
                }
            }

            EndDrag();
            destination.EndDrag();
            RebuildChildren();
            return Result<void>::Success();
        }

        (void)destination.tabs_.TryRemoveAt(next);
    }

    (void)destination.Element::RemoveChild(*header);
    (void)destination.Element::RemoveChild(*content);
    (void)Element::TryAddChild(header);
    (void)Element::TryAddChild(content);
    return attached;
}

usize DockManager::SubtreeDepth(SlotHandle handle) const noexcept {
    const auto& node = nodes_[handle];
    if (node.kind == Kind::Stack) {
        return 1;
    }

    const usize first = SubtreeDepth(node.first);
    const usize second = SubtreeDepth(node.second);
    return 1 + (first > second ? first : second);
}

Result<void> DockManager::Place(usize index, SlotHandle target, DockSide side, usize insertion,
                                float share) noexcept {
    const auto source = tabs_[index].stack;
    if (side > DockSide::Bottom || !math::IsFinite(share) || share < 0.05f || share > 0.95f) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!target.IsNull() &&
        (!nodes_.Find(target) ||
         (nodes_[target].kind != Kind::Stack && (target != root_ || side == DockSide::Center)))) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (side != DockSide::Center && source == target && !target.IsNull() &&
        nodes_[target].tabs.Size() == 1) {
        return Result<void>::Success();
    }

    SlotHandle destination = target;
    SlotHandle split{};
    if (target.IsNull() || side != DockSide::Center) {
        if (target.IsNull() && !root_.IsNull()) {
            return Result<void>::Failure(ErrorCode::WrongContext);
        }

        usize depth = 0;
        for (auto p = target; !p.IsNull(); p = nodes_[p].parent) {
            ++depth;
        }

        // 루트를 새 분할로 감싸면 그 아래의 모든 가지가 한 단계 깊어진다.
        if (depth >= 64 || (target == root_ && !target.IsNull() && SubtreeDepth(target) >= 64)) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        auto added = nodes_.TryEmplace(allocator_);
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        destination = added.Value();
    }

    auto& list = nodes_[destination].tabs;
    auto reserved = list.TryEnsureCapacity(list.Size() + 1);
    if (!reserved) {
        if (destination != target) {
            (void)nodes_.TryRemove(destination);
        }

        return reserved;
    }

    if (!target.IsNull() && destination != target) {
        auto added = nodes_.TryEmplace(allocator_);
        if (!added) {
            (void)nodes_.TryRemove(destination);
            return Result<void>::Failure(added.Error());
        }

        split = added.Value();
    }

    // 메모리 확보가 끝난 뒤 기존 트리를 바꾼다. 슬롯 재할당 전의 노드 참조는 사용하지 않는다.
    if (source == destination && insertion != MaxSize) {
        const auto& current = nodes_[source].tabs;
        for (usize i = 0; i < current.Size(); ++i) {
            if (current[i] == index && i < insertion) {
                --insertion;
                break;
            }
        }
    }

    auto views = EnsureViews(index);
    if (!views) {
        if (!split.IsNull()) {
            (void)nodes_.TryRemove(split);
        }

        if (destination != target) {
            (void)nodes_.TryRemove(destination);
        }

        return views;
    }

    DetachTab(index);
    Node& dest = nodes_[destination];
    const usize at = insertion < dest.tabs.Size() ? insertion : dest.tabs.Size();
    (void)dest.tabs.TryInsert(index, at);
    dest.selected = at;
    tabs_[index].stack = destination;

    if (target.IsNull()) {
        root_ = destination;
    } else if (!split.IsNull()) {
        ReplaceNode(target, split);
        Node& branch = nodes_[split];
        branch.kind =
            side == DockSide::Left || side == DockSide::Right ? Kind::Horizontal : Kind::Vertical;
        const bool before = side == DockSide::Left || side == DockSide::Top;
        branch.first = before ? destination : target;
        branch.second = before ? target : destination;
        branch.fraction = before ? share : 1.0f - share;
        nodes_[target].parent = split;
        dest.parent = split;
    }

    if (source != destination) {
        CollapseEmpty(source);
    }

    RebuildChildren();
    return Result<void>::Success();
}

} // namespace rocks::ui
