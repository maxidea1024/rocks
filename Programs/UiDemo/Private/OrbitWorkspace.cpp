#include "OrbitWorkspace.h"

#include <Core/Text/StringBuilder.h>
#include <UiFramework/Button.h>
#include <UiFramework/CheckBox.h>
#include <UiFramework/ScrollView.h>

#include <UiWidget/ProgressBarWidget.generated.h>

namespace rocks::ui {

namespace {

constexpr Guid StartCommand{0x6f726269745f7569, 1};
constexpr Guid EndCommand{0x6f726269745f7569, 2};
constexpr Color Paper{231, 238, 237};
constexpr Color Muted{163, 181, 189};
constexpr Color Mint{124, 239, 220};
constexpr Color Space{14, 24, 33};
constexpr Color Panel{25, 38, 49};
constexpr Color Line{61, 79, 89};

CanvasSlot Row(float left, float top, float right, float height) noexcept {
    return {.anchorMax = {1, 0}, .offset = {left, top, right, height}, .alignment = {0, 0}};
}

CanvasSlot Rect(float x, float y, float width, float height) noexcept {
    return {.offset = {x, y, width, height}, .alignment = {0, 0}};
}

Result<void> AddPanel(AnchorCanvas& canvas, Allocator& allocator, Color color,
                      CanvasSlot slot) noexcept {
    auto panel = SharedPtr<Frame>::TryMake(allocator, allocator);
    if (!panel) {
        return Result<void>::Failure(panel.Error());
    }
    panel.Value()->SetColor(LinearColor::FromSRGB(color));
    panel.Value()->SetVisibility(Visibility::HitTestInvisible);
    return canvas.TryAddSlot(panel.Value(), slot);
}

CanvasSlot Background() noexcept {
    return {.anchorMax = {1, 1}, .offset = {}, .alignment = {0, 0}, .zOrder = -1};
}

// 슬롯 등록 중 첫 실패를 보존한다. 중간에 실패한 캔버스는 창에 연결하지 않는다.
class CanvasLayout {
public:
    CanvasLayout(AnchorCanvas& canvas, Allocator& allocator) noexcept
        : canvas_(canvas), allocator_(allocator) {
    }

    void Add(const SharedPtr<Element>& child, CanvasSlot slot) noexcept {
        if (status_) {
            status_ = canvas_.TryAddSlot(child, slot);
        }
    }
    void Add(const Result<SharedPtr<TextLabel>>& label, CanvasSlot slot) noexcept {
        if (!status_) {
            return;
        }
        if (!label) {
            status_ = Result<void>::Failure(label.Error());
            return;
        }
        Add(label.Value(), slot);
    }
    void Fill(Color color, CanvasSlot slot) noexcept {
        if (status_) {
            status_ = AddPanel(canvas_, allocator_, color, slot);
        }
    }
    ErrorCode Error() const noexcept {
        return status_.Error();
    }

private:
    AnchorCanvas& canvas_;
    Allocator& allocator_;
    Result<void> status_ = Result<void>::Success();
};

} // namespace

OrbitWorkspace::OrbitWorkspace(Allocator& allocator, ObjectRegistry& objects,
                               DemoText& font) noexcept
    : allocator_(allocator), objects_(objects), font_(font), definitions_(allocator),
      bindings_(allocator) {
}

const SharedPtr<CommandList>& OrbitWorkspace::Commands() const noexcept {
    return commands_;
}

Result<void> OrbitWorkspace::BindCommands() noexcept {
    auto list = SharedPtr<CommandList>::TryMake(allocator_, allocator_, definitions_, bindings_);
    if (!list) {
        return Result<void>::Failure(list.Error());
    }
    commands_ = Move(list.Value());
    auto result = definitions_.Add(StartCommand, "출발점", "공전 진행률을 0%로 옮긴다.",
                                   {{input::Key::Home}, {}});
    if (result) {
        result = definitions_.Add(EndCommand, "종료점", "공전 진행률을 100%로 옮긴다.",
                                  {{input::Key::End}, {}});
    }
    if (result) {
        result = commands_->Map(
            StartCommand, {.execute = Delegate<void()>::CreateRaw(this, &OrbitWorkspace::Rewind)});
    }
    if (result) {
        result = commands_->Map(EndCommand, {.execute = Delegate<void()>::CreateLambda(
                                                 [this]() noexcept { Seek(1); })});
    }
    return result;
}

Result<SharedPtr<TextLabel>> OrbitWorkspace::Label(StringView value, int32 size,
                                                   Color color) noexcept {
    auto label = SharedPtr<TextLabel>::TryMake(allocator_, allocator_);
    if (!label) {
        return label;
    }
    label.Value()->SetColor(LinearColor::FromSRGB(color));
    auto set = font_.SetText(*label.Value(), value, size);
    if (!set) {
        return Result<SharedPtr<TextLabel>>::Failure(set.Error());
    }
    return label;
}

Result<SharedPtr<Frame>> OrbitWorkspace::Tab(StringView name) noexcept {
    ElementBuildScope scope(allocator_);
    auto tab = Build<Frame>({.content = Label(name, 13, Paper),
                             .color = LinearColor::FromSRGB({45, 61, 73}),
                             .padding = {16, 4, 4, 0}});
    if (!tab) {
        return Result<SharedPtr<Frame>>::Failure(tab.Error());
    }
    return Result<SharedPtr<Frame>>::Success(tab.Value().AsSharedPtr());
}

Result<SharedPtr<Element>> OrbitWorkspace::Preview(SlotHandle map) noexcept {
    auto canvas = SharedPtr<AnchorCanvas>::TryMake(allocator_, allocator_);
    auto orbit = SharedPtr<OrbitView>::TryMake(allocator_, allocator_, map);
    auto hud = CreateGameHud(objects_);
    if (!canvas) {
        return Result<SharedPtr<Element>>::Failure(canvas.Error());
    }
    if (!orbit) {
        return Result<SharedPtr<Element>>::Failure(orbit.Error());
    }
    if (!hud) {
        return Result<SharedPtr<Element>>::Failure(hud.Error());
    }

    orbit_ = Move(orbit.Value());
    orbit_->SetClipToBounds(true);
    hud_ = Move(hud.Value());
    auto progress = objects_.Resolve<ProgressBarWidget>(objects_.OwnerAccess(), hud_.progress);
    if (!progress) {
        return Result<SharedPtr<Element>>::Failure(progress.Error());
    }
    auto bar = progress.Value().Get().Value()->CachedElement();
    if (!bar) {
        return Result<SharedPtr<Element>>::Failure(ErrorCode::NotReady);
    }
    auto* native = static_cast<ProgressBar*>(bar.Get());
    native->SetColor(LinearColor::FromSRGB(Line));
    native->SetFillColor(LinearColor::FromSRGB(Mint));

    auto& view = *canvas.Value();
    view.SetClipToBounds(true);
    CanvasSlot chart{.anchorMax = {1, 1}, .offset = {12, 100, 12, 62}, .alignment = {0, 0}};
    CanvasSlot legend{.anchorMin = {0, 1},
                      .anchorMax = {0, 1},
                      .offset = {49, -52, 150, 22},
                      .alignment = {0, 0}};
    auto dot = legend;
    dot.offset = {31, -44, 7, 7};
    CanvasSlot meter{
        .anchorMin = {0, 1}, .anchorMax = {1, 1}, .offset = {30, -20, 30, 4}, .alignment = {0, 0}};
    CanvasLayout layout(view, allocator_);
    layout.Fill(Space, Background());
    layout.Add(Label("CASSINI   /   SATURN", 14, Mint), Row(30, 24, 24, 24));
    layout.Add(Label("토성 궤도", 32, Paper), Row(28, 46, 24, 50));
    layout.Add(orbit_, chart);
    layout.Fill(Mint, dot);
    layout.Add(Label("탐사선 위치", 13, Muted), legend);
    layout.Add(hud_.root, meter);
    if (layout.Error() != ErrorCode::None) {
        return Result<SharedPtr<Element>>::Failure(layout.Error());
    }
    return Result<SharedPtr<Element>>::Success(canvas.Value());
}

Result<SharedPtr<Element>> OrbitWorkspace::Inspector() noexcept {
    ElementBuildScope scope(allocator_);
    auto slider = SharedPtr<Slider>::TryMake(allocator_, allocator_);
    auto check = SharedPtr<CheckBox>::TryMake(allocator_, allocator_);
    if (!slider || !check) {
        return Result<SharedPtr<Element>>::Failure(ErrorCode::OutOfMemory);
    }
    auto percent = Label("0%", 36, Mint);
    if (!percent) {
        return Result<SharedPtr<Element>>::Failure(percent.Error());
    }
    auto display = Label("미리보기에 진행 막대 표시", 15, Paper);
    if (!display) {
        return Result<SharedPtr<Element>>::Failure(display.Error());
    }
    slider_ = Move(slider.Value());
    percent_ = Move(percent.Value());
    slider_->SetColor(LinearColor::FromSRGB(Line));
    slider_->SetFillColor(LinearColor::FromSRGB(Mint));
    slider_->SetOnChanged(Delegate<void(float)>::CreateRaw(this, &OrbitWorkspace::Seek));

    check.Value()->SetColor(LinearColor::FromSRGB(Mint));
    (void)check.Value()->SetState(CheckState::Checked);
    check.Value()->SetOnChanged(
        Delegate<void(CheckState)>::CreateLambda([this](CheckState state) noexcept {
            hud_.root->SetVisibility(state == CheckState::Checked ? Visibility::Visible
                                                                  : Visibility::Hidden);
        }));
    CanvasSlot maximum{.anchorMin = {1, 0},
                       .anchorMax = {1, 0},
                       .offset = {-66, 220, 40, 22},
                       .alignment = {0, 0}};
    auto padded = check.Value()->SetPadding({33, 0, 0, 0});
    if (!padded) {
        return Result<SharedPtr<Element>>::Failure(padded.Error());
    }
    auto attached = check.Value()->TryAddChild(display.Value());
    if (!attached) {
        return Result<SharedPtr<Element>>::Failure(attached.Error());
    }

    auto fill = [](Color color) noexcept {
        return Make<Frame>({.color = LinearColor::FromSRGB(color),
                            .common = {.visibility = Visibility::HitTestInvisible}});
    };
    const AnchorCanvas::SlotArgs slots[] = {
        {Background(), fill(Panel)},
        {Row(28, 23, 24, 42), Label("궤도 설정", 26, Paper)},
        {Row(28, 77, 28, 1), fill(Line)},
        {Row(28, 101, 28, 26), Label("공전 진행률", 16, Paper)},
        {Row(26, 132, 28, 55), percent_},
        {Row(28, 188, 28, 32), slider_},
        {Rect(28, 220, 35, 22), Label("0%", 13, Muted)},
        {maximum, Label("100%", 13, Muted)},
        {Row(28, 261, 28, 1), fill(Line)},
        {Row(28, 285, 28, 24), check.Value()},
        {Rect(28, 340, 187, 44),
         Make<Button>({.content = Label("출발점으로 되돌리기", 15, Space),
                       .color = LinearColor::FromSRGB(Mint),
                       .pressedColor = LinearColor::FromSRGB({85, 191, 178}),
                       .padding = {18, 10, 12, 0},
                       .onClicked = Bind(this, &OrbitWorkspace::Rewind)})},
        {Row(28, 402, 28, 24), Label("Home  출발점     End  종료점", 13, Muted)},
    };
    auto scroll = Build<ScrollView>(
        {.content = Make<AnchorCanvas>({.slots = slots, .common = {.clipToBounds = true}})});
    if (!scroll) {
        return Result<SharedPtr<Element>>::Failure(scroll.Error());
    }
    return Result<SharedPtr<Element>>::Success(scroll.Value().AsSharedPtr());
}

Result<SharedPtr<Element>> OrbitWorkspace::BuildTimeline() noexcept {
    auto panel = SharedPtr<AnchorCanvas>::TryMake(allocator_, allocator_);
    auto timeline = SharedPtr<Timeline>::TryMake(allocator_, allocator_);
    auto phase = Label("0°", 18, Mint);
    if (!panel || !timeline || !phase) {
        return Result<SharedPtr<Element>>::Failure(!panel      ? panel.Error()
                                                   : !timeline ? timeline.Error()
                                                               : phase.Error());
    }
    timeline_ = Move(timeline.Value());
    phase_ = Move(phase.Value());
    timeline_->SetColor(LinearColor::FromSRGB(Panel));
    timeline_->SetFillColor(LinearColor::FromSRGB(Mint));
    auto styled = timeline_->SetRulerColor(LinearColor::FromSRGB(Muted));
    if (!styled) {
        return Result<SharedPtr<Element>>::Failure(styled.Error());
    }
    timeline_->SetOnChanged(Delegate<void(float)>::CreateRaw(this, &OrbitWorkspace::Seek));
    const TimelineMark marks[] = {{0.25f, LinearColor::FromSRGB({208, 159, 108})},
                                  {0.5f, LinearColor::FromSRGB({208, 159, 108})},
                                  {0.75f, LinearColor::FromSRGB({208, 159, 108})}};
    auto marked = timeline_->SetMarks(marks);
    if (!marked) {
        return Result<SharedPtr<Element>>::Failure(marked.Error());
    }

    CanvasLayout layout(*panel.Value(), allocator_);
    layout.Fill(Panel, Background());
    layout.Add(Label("공전 위상", 15, Paper), Rect(24, 9, 130, 25));
    layout.Add(phase_, {.anchorMin = {1, 0},
                        .anchorMax = {1, 0},
                        .offset = {-83, 7, 65, 27},
                        .alignment = {0, 0}});
    layout.Add(timeline_, Row(20, 38, 20, 35));
    constexpr StringView labels[] = {"0°", "90°", "180°", "270°", "360°"};
    for (usize i = 0; i < 5; ++i) {
        const float anchor = static_cast<float>(i) * 0.25f;
        const float offset = 26.0f - 52.0f * anchor;
        layout.Add(Label(labels[i], 12, Muted), {.anchorMin = {anchor, 0},
                                                 .anchorMax = {anchor, 0},
                                                 .offset = {offset, 75, 38, 20},
                                                 .alignment = {anchor, 0}});
    }
    panel.Value()->SetClipToBounds(true);
    if (layout.Error() != ErrorCode::None) {
        return Result<SharedPtr<Element>>::Failure(layout.Error());
    }
    return Result<SharedPtr<Element>>::Success(panel.Value());
}

Result<void> OrbitWorkspace::SetProgress(float value) noexcept {
    auto progress =
        objects_.ResolveForWrite<ProgressBarWidget>(objects_.OwnerAccess(), hud_.progress);
    if (!progress) {
        return Result<void>::Failure(progress.Error());
    }
    const auto percent = static_cast<int32>(value * 100 + 0.5f);
    if (percent != displayed_) {
        StringBuilder<> text(allocator_);
        auto result = text.TryAppendFormat("{}%", percent);
        if (!result) {
            return result;
        }
        result = font_.SetText(*percent_, text.View(), 36);
        if (!result) {
            return result;
        }
        displayed_ = percent;
    }
    if (timeline_ && phase_) {
        auto updated = timeline_->SetValue(value);
        if (!updated) {
            return updated;
        }
        const int32 angle = static_cast<int32>(value * 360 + 0.5f);
        if (angle != phaseDegrees_) {
            StringBuilder<> text(allocator_);
            updated = text.TryAppendFormat("{}°", angle);
            if (updated) {
                updated = font_.SetText(*phase_, text.View(), 18);
            }
            if (!updated) {
                return updated;
            }
            phaseDegrees_ = angle;
        }
    }
    (void)slider_->SetValue(value);
    orbit_->SetProgress(value);
    return progress.Value().Get().Value()->SetValue(value);
}

void OrbitWorkspace::Seek(float value) noexcept {
    (void)SetProgress(value);
}

void OrbitWorkspace::Rewind() noexcept {
    Seek(0);
}

Result<SharedPtr<DockManager>> OrbitWorkspace::Create(SlotHandle map) noexcept {
    auto area = SharedPtr<DockManager>::TryMake(allocator_, allocator_);
    if (!area) {
        return area;
    }
    auto commands = BindCommands();
    if (!commands) {
        return Result<SharedPtr<DockManager>>::Failure(commands.Error());
    }
    auto preview = Preview(map);
    if (!preview) {
        return Result<SharedPtr<DockManager>>::Failure(preview.Error());
    }
    auto inspector = Inspector();
    if (!inspector) {
        return Result<SharedPtr<DockManager>>::Failure(inspector.Error());
    }
    auto timeline = BuildTimeline();
    auto timelineTab = Tab("타임라인");
    if (!timeline || !timelineTab) {
        return Result<SharedPtr<DockManager>>::Failure(!timeline ? timeline.Error()
                                                                 : timelineTab.Error());
    }
    auto previewTab = Tab("궤도 미리보기");
    auto inspectorTab = Tab("속성");
    if (!previewTab || !inspectorTab) {
        return Result<SharedPtr<DockManager>>::Failure(!previewTab ? previewTab.Error()
                                                                   : inspectorTab.Error());
    }

    if (!area.Value()->RegisterTab(
            {"Preview", previewTab.Value(), preview.Value(), DockRole::Document, false}) ||
        !area.Value()->RegisterTab(
            {"Controls", inspectorTab.Value(), inspector.Value(), DockRole::Panel, false}) ||
        !area.Value()->RegisterTab(
            {"Timeline", timelineTab.Value(), timeline.Value(), DockRole::Panel, false}) ||
        !area.Value()->OpenTab("Preview") ||
        !area.Value()->Dock("Controls", "Preview", DockSide::Right, 0.36f) ||
        !area.Value()->Dock("Timeline", "Preview", DockSide::Bottom, 0.22f)) {
        return Result<SharedPtr<DockManager>>::Failure(ErrorCode::InvalidArgument);
    }
    constexpr float initial = 0.64f;
    (void)slider_->SetValue(initial);
    auto ready = SetProgress(initial);
    if (!ready) {
        return Result<SharedPtr<DockManager>>::Failure(ready.Error());
    }
    return area;
}

} // namespace rocks::ui
