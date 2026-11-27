## 13. 참고 자료

UE 소스 경로는 로컬 클론 `C:\github\UnrealEngine`(5.8.4-release)의 `Engine/Source/` 기준이다. 조회일은 2026.11.16이다.

| ID | 자료 | 적용 범위 |
|---|---|---|
| U1 | `Runtime/SlateCore/Public/Widgets/SWidget.h` · `Private/Widgets/SWidget.cpp` | Element 계약·생명 주기·그리기·무효화 |
| U2 | `Runtime/SlateCore/Public/Widgets/DeclarativeSyntaxSupport.h` · `Types/SlateAttribute.h` · `Types/SlateAttributeMetaData.h` · `Widgets/InvalidateWidgetReason.h` | 선언적 생성·속성·무효화 사유 |
| U3 | `Runtime/SlateCore/Public/FastUpdate/` · `Private/FastUpdate/` | cache root·proxy·빠른 경로 |
| U4 | `Runtime/SlateCore/Public/Layout/` (`Geometry.h`·`LayoutUtils.h`·`Clipping.h`·`Visibility.h`) | geometry·배치·clip·보이기 |
| U5 | `Runtime/SlateCore/Public/Rendering/` (`DrawElements.h`·`ElementBatcher.h`·`SlateRenderer.h`·`RenderingCommon.h`) | 그리기 목록·batch·renderer 계약·viewport |
| U6 | `Runtime/Slate/Private/Framework/Application/SlateApplication.cpp` · `SlateUser.h` · `NavigationConfig.h` · `MenuStack.h` · `IInputProcessor.h` · `Runtime/Launch/Private/LaunchEngineLoop.cpp` | 라우팅·사용자·focus·navigation·menu·전처리기·엔진 프레임 순서 |
| U7 | `Runtime/SlateCore/Public/Input/` (`Reply.h`·`HittestGrid.h`·`DragAndDrop.h`·`Events.h`) | 응답·hit grid·drag |
| U8 | `Runtime/SlateCore/Public/Fonts/` · `Private/Fonts/LegacySlateFontInfoCache.cpp` · `Runtime/Slate/Public/Framework/Text/` · `Runtime/Core/Public/Internationalization/BreakIterator.h` | 글꼴·shaping·텍스트 레이아웃·경계 |
| U9 | `Runtime/ApplicationCore/Public/GenericPlatform/` (`GenericApplication.h`·`GenericWindow.h`·`ITextInputMethodSystem.h`·`Accessibility/`) · `Windows/` · `Mac/` | 플랫폼 창·IME·접근성 |
| U10 | `Runtime/SlateCore/Public/Styling/` · `Private/Styling/StyleColors.cpp` · `UMGCoreStyle.cpp` · `StarshipCoreStyle.cpp` · `Editor/EditorStyle/Private/StarshipStyle.cpp` | 스타일 체계·기본 룩 |
| U11 | `Runtime/UMG/Public/Components/` · `Blueprint/UserWidget.h` · `Blueprint/WidgetTree.h` · `Blueprint/UserWidgetPool.h` · `Slate/SObjectWidget.h` · `Editor/UMGEditor/` | Widget 층·Designer |
| U12 | `Runtime/FieldNotification/Public/` · `Engine/Plugins/Runtime/ModelViewViewModel/Source/` | 변경 통지·ViewModel |
| U13 | `Engine/Plugins/Runtime/CommonUI/Source/` | 화면 스택·입력 라우터·입력 장치 |
| U14 | `Runtime/Slate/Public/Framework/Docking/` · `Commands/` · `Developer/ToolMenus/` · `Editor/PropertyEditor/` · `Editor/UnrealEd/Public/Toolkits/` | 에디터 프레임워크 |
| S37 | [Invalidation in Slate and UMG](https://dev.epicgames.com/documentation/en-us/unreal-engine/invalidation-in-slate-and-umg-for-unreal-engine) | 무효화 개요 |
| S50 | [UMG Editor Reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/umg-editor-reference-for-unreal-engine) | Designer 작업 흐름 |
| S51 | [UMG Viewmodel](https://dev.epicgames.com/documentation/en-us/unreal-engine/umg-viewmodel-for-unreal-engine) | ViewModel 개요 |
