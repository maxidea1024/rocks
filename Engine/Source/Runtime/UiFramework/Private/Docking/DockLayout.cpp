#include <UiFramework/Docking/DockManager.h>
#include <Core/Encoding/ByteReader.h>
#include <Core/Text/Json.h>
#include <Core/Text/NumberText.h>

namespace rocks::ui {

namespace {

// 파일 형식 버전과 에디터가 지정한 배치 버전을 따로 검사한다.
constexpr uint32 DockLayoutMagic = 0x4b434f44;
constexpr uint32 DockLayoutFormat = 1;

} // namespace

Result<void> DockManager::SaveLayout(uint32 version, Array<uint8>& output) const noexcept {
    StringBuilder<> text(output.GetAllocator());
    JsonWriter writer(text);
    (void)writer.BeginObject();
    (void)writer.Key("formatVersion");
    (void)writer.UInt64(1);
    (void)writer.Key("layoutVersion");
    (void)writer.UInt64(version);
    (void)writer.Key("root");

    auto writeNode = [&](auto&& self, SlotHandle handle) noexcept -> void {
        if (handle.IsNull()) {
            (void)writer.Null();
            return;
        }

        const Node& node = nodes_[handle];
        (void)writer.BeginObject();
        (void)writer.Key("type");
        if (node.kind == Kind::Stack) {
            (void)writer.String("stack");
            (void)writer.Key("activeTab");
            (void)writer.String(tabs_[node.tabs[node.selected]].id.View());
            (void)writer.Key("tabs");
            (void)writer.BeginArray();
            for (usize index : node.tabs) {
                (void)writer.String(tabs_[index].id.View());
            }

            (void)writer.EndArray();
        } else {
            (void)writer.String(node.kind == Kind::Horizontal ? "horizontal" : "vertical");
            (void)writer.Key("fraction");
            (void)writer.Float(node.fraction);
            (void)writer.Key("first");
            self(self, node.first);
            (void)writer.Key("second");
            self(self, node.second);
        }

        (void)writer.EndObject();
    };
    writeNode(writeNode, root_);
    (void)writer.EndObject();
    auto written = writer.Finish();
    if (!written) {
        return written;
    }

    Array<uint8> bytes(output.GetAllocator());
    auto copied = bytes.TryAppend(Span<const uint8>(
        reinterpret_cast<const uint8*>(text.View().Data()), text.View().ByteCount()));
    if (copied) {
        output = Move(bytes);
    }

    return copied;
}

Result<void> DockManager::RestoreLayout(uint32 version, Span<const uint8> bytes) noexcept {
    if (spawning_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    JsonDocument document(allocator_);
    auto parsed = document.Parse({reinterpret_cast<const char*>(bytes.Data()), bytes.Size()});
    if (!parsed.Succeeded()) {
        return Result<void>::Failure(parsed.code == JsonErrorCode::OutOfMemory
                                         ? ErrorCode::OutOfMemory
                                         : ErrorCode::InvalidArgument);
    }

    const auto top = document.Root();
    const auto format = top.Find("formatVersion").TryGetUInt64();
    const auto layout = top.Find("layoutVersion").TryGetUInt64();
    const auto rootValue = top.Find("root");
    if (!format || format.Value() != 1 || !layout || layout.Value() != version ||
        !rootValue.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    SlotArray<Node> restored(allocator_);
    Array<SlotHandle> locations(allocator_);
    auto sized = locations.TryResize(tabs_.Size());
    if (!sized) {
        return sized;
    }

    auto readNode = [&](auto&& self, JsonValue value, SlotHandle parent,
                        uint32 depth) noexcept -> Result<SlotHandle> {
        const auto type = value.Find("type");
        if (depth > 64 || !type.IsString()) {
            return Result<SlotHandle>::Failure(ErrorCode::InvalidArgument);
        }

        auto slot = restored.TryEmplace(allocator_);
        if (!slot) {
            return slot;
        }

        const auto handle = slot.Value();
        restored[handle].parent = parent;
        const auto kind = type.GetString();
        if (kind == "stack") {
            const auto tabs = value.Find("tabs");
            const auto active = value.Find("activeTab");
            if (!active.IsString() || !tabs.IsArray() || !tabs.Size() ||
                tabs.Size() > tabs_.Size()) {
                return Result<SlotHandle>::Failure(ErrorCode::InvalidArgument);
            }

            auto& stack = restored[handle];
            stack.selected = MaxSize;
            for (usize i = 0; i < tabs.Size(); ++i) {
                const auto id = tabs.At(i);
                if (!id.IsString()) {
                    return Result<SlotHandle>::Failure(ErrorCode::InvalidArgument);
                }

                const usize tab = FindTab(id.GetString());
                if (tab == MaxSize) {
                    return Result<SlotHandle>::Failure(ErrorCode::NotFound);
                }

                if (!locations[tab].IsNull()) {
                    return Result<SlotHandle>::Failure(ErrorCode::InvalidArgument);
                }

                auto added = stack.tabs.TryPushBack(tab);
                if (!added) {
                    return Result<SlotHandle>::Failure(added.Error());
                }

                locations[tab] = handle;
                if (id.GetString() == active.GetString()) {
                    stack.selected = i;
                }
            }

            if (stack.selected == MaxSize) {
                return Result<SlotHandle>::Failure(ErrorCode::InvalidArgument);
            }
        } else {
            const auto ratio = value.Find("fraction");
            if ((kind != "horizontal" && kind != "vertical") || !ratio.IsNumber()) {
                return Result<SlotHandle>::Failure(ErrorCode::InvalidArgument);
            }

            const auto fraction = TryParseFloat(ratio.RawNumber());
            if (!fraction || !(fraction.Value() > 0 && fraction.Value() < 1)) {
                return Result<SlotHandle>::Failure(ErrorCode::InvalidArgument);
            }

            auto first = self(self, value.Find("first"), handle, depth + 1);
            if (!first) {
                return first;
            }

            auto second = self(self, value.Find("second"), handle, depth + 1);
            if (!second) {
                return second;
            }

            auto& split = restored[handle];
            split.kind = kind == "horizontal" ? Kind::Horizontal : Kind::Vertical;
            split.fraction = fraction.Value();
            split.first = first.Value();
            split.second = second.Value();
        }

        return slot;
    };

    SlotHandle root;
    if (!rootValue.IsNull()) {
        auto loaded = readNode(readNode, rootValue, {}, 1);
        if (!loaded) {
            return Result<void>::Failure(loaded.Error());
        }

        root = loaded.Value();
    }

    return ApplyLayout(Move(restored), locations, root);
}

Result<void> DockManager::RestoreLegacyLayout(uint32 version, Span<const uint8> bytes) noexcept {
    if (spawning_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    ByteReader reader(bytes);
    if (reader.ReadU32LE() != DockLayoutMagic || reader.ReadU32LE() != DockLayoutFormat ||
        reader.ReadU32LE() != version || reader.Failed()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const uint8 hasRoot = reader.ReadU8();
    if (reader.Failed() || hasRoot > 1) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    SlotArray<Node> restored(allocator_);
    Array<SlotHandle> locations(allocator_);
    auto sized = locations.TryResize(tabs_.Size());
    if (!sized) {
        return sized;
    }

    auto read = [&](auto&& self, SlotHandle parent, uint32 depth) noexcept -> Result<SlotHandle> {
        if (depth > 64 || reader.AtEnd()) {
            return Result<SlotHandle>::Failure(ErrorCode::InvalidArgument);
        }

        const uint8 tag = reader.ReadU8();
        if (tag > static_cast<uint8>(Kind::Vertical) || reader.Failed()) {
            return Result<SlotHandle>::Failure(ErrorCode::InvalidArgument);
        }

        auto node = restored.TryEmplace(allocator_);
        if (!node) {
            return node;
        }

        const auto handle = node.Value();
        restored[handle].parent = parent;
        restored[handle].kind = static_cast<Kind>(tag);
        if (tag != static_cast<uint8>(Kind::Stack)) {
            const float fraction = reader.ReadF32LE();
            if (reader.Failed() || !math::IsFinite(fraction) || fraction <= 0 || fraction >= 1) {
                return Result<SlotHandle>::Failure(ErrorCode::InvalidArgument);
            }

            auto first = self(self, handle, depth + 1);
            if (!first) {
                return first;
            }

            auto second = self(self, handle, depth + 1);
            if (!second) {
                return second;
            }

            auto& branch = restored[handle];
            branch.fraction = fraction;
            branch.first = first.Value();
            branch.second = second.Value();
        } else {
            const uint32 count = reader.ReadU32LE();
            const uint32 selected = reader.ReadU32LE();
            if (reader.Failed() || count == 0 || selected >= count || count > tabs_.Size() ||
                count > reader.Remaining() / 5) {
                return Result<SlotHandle>::Failure(ErrorCode::InvalidArgument);
            }

            auto& stack = restored[handle];
            stack.selected = selected;
            auto reserve = stack.tabs.TryReserve(count);
            if (!reserve) {
                return Result<SlotHandle>::Failure(reserve.Error());
            }

            for (uint32 i = 0; i < count; ++i) {
                const uint32 length = reader.ReadU32LE();
                const auto raw = reader.Take(length);
                if (reader.Failed() || length == 0) {
                    return Result<SlotHandle>::Failure(ErrorCode::InvalidArgument);
                }

                const usize tab = FindTab({reinterpret_cast<const char*>(raw.Data()), raw.Size()});
                if (tab == MaxSize) {
                    return Result<SlotHandle>::Failure(ErrorCode::NotFound);
                }

                if (!locations[tab].IsNull()) {
                    return Result<SlotHandle>::Failure(ErrorCode::InvalidArgument);
                }

                locations[tab] = handle;
                (void)stack.tabs.TryPushBack(tab);
            }
        }

        return Result<SlotHandle>::Success(handle);
    };

    SlotHandle root;
    if (hasRoot) {
        auto loaded = read(read, {}, 1);
        if (!loaded) {
            return Result<void>::Failure(loaded.Error());
        }

        root = loaded.Value();
    }

    if (!reader.AtEnd()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return ApplyLayout(Move(restored), locations, root);
}

Result<void> DockManager::ApplyLayout(SlotArray<Node>&& restored,
                                      const Array<SlotHandle>& locations,
                                      SlotHandle root) noexcept {
    // 파싱이 모두 끝난 뒤 실제로 열릴 탭만 생성한다. 생성 실패 시 새 뷰만 해제한다.
    Array<usize> created(allocator_);
    for (usize i = 0; i < tabs_.Size(); ++i) {
        if (!locations[i].IsNull() && !tabs_[i].header) {
            auto tracked = created.TryPushBack(i);
            if (!tracked) {
                for (usize j : created) {
                    DropViews(j);
                }

                return tracked;
            }

            auto opened = EnsureViews(i);
            if (!opened) {
                for (usize j : created) {
                    DropViews(j);
                }

                return opened;
            }
        }
    }

    EndDrag();
    nodes_ = Move(restored);
    root_ = root;
    for (usize i = 0; i < tabs_.Size(); ++i) {
        tabs_[i].stack = locations[i];
    }

    RebuildChildren();
    for (usize i = 0; i < tabs_.Size(); ++i) {
        if (locations[i].IsNull()) {
            DropViews(i);
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
