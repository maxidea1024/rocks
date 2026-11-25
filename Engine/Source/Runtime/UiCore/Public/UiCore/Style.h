#pragma once

// Element 스타일 형식이다(Docs/Design/UiToolkit 4.1절, 9.1절 UW3b — UE FSlateWidgetStyle 파생과
// FSlateBrush). 색은 ColorRef(테마 slot·전경 상속)로 들고, 그릴 때 테마로 푼다. 기본값은 UE 5.8
// 에디터 핵심 스타일(14장)이다.
//
// 사용 예:
//   const ButtonStyle& style = DefaultButtonStyle();
//   const Brush brush = style.normal.Resolve(list.Theme(), {});
//
// 형식마다 UiCore/Styles/ 아래 한 파일이고, 이 헤더는 그것을 모두 포함한다.

#include <UiCore/Styles/ButtonStyle.h>
#include <UiCore/Styles/CheckBoxStyle.h>
#include <UiCore/Styles/CoreStyles.h>
#include <UiCore/Styles/DockTabStyle.h>
#include <UiCore/Styles/FontStyle.h>
#include <UiCore/Styles/StyleBrush.h>
#include <UiCore/Styles/TextStyle.h>
#include <UiCore/Styles/Typeface.h>
