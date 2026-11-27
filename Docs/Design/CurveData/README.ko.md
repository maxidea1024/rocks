# 커브 데이터 계층 설계

개정 14(5.9절 쿠킹용 결정적 평가 — D22, CV11), 기준일 2026-11-27. 개정 13(CV10 묶음 끝 리뷰 반영 — 17.8절 I2·I4·I5·I6·I7, 12절, 15절 32), 2026-11-18. 이 문서는 Curve 단계(리플렉션 B9 직후, A0-G 앞 — 2026-11-15 사용자 결정)의 **값 타입 층** 설계다. 근거는 [에셋 시스템](../AssetSystem/01-Stages.ko.md) 1절 Curve 행, [Core 상세 명세](../CoreDetailedSpec.ko.md) G09, 이미 구현된 수학 층 [키 커브](../../Implementation/InterpCurve.ko.md)(`InterpCurve<T>`)다. UE 대응 대상은 UE 5.8.4의 `Engine/Source/Runtime/Engine/Classes/Curves`(`FRealCurve`·`FRichCurve`·`FSimpleCurve`·`FIndexedCurve`·`FKeyHandle`·`FKeyHandleMap`·`FIntegralCurve`·`FNameCurve`·`FStringCurve`·`FCompressedRichCurve`)와 평가 함수 `Public/Curves/CurveEvaluation.h`(`UE::Curves::EvalForTwoKeys`·`BezierInterp`·`WeightedEvalForTwoKeys`), 그리고 G09가 이 단계로 미룬 `FInterpCurve<T>`의 리플렉션·직렬화·`CalcBounds`(12절)다. 아래에서 `RC` = `Engine/Source/Runtime/Engine/Private/Curves/RichCurve.cpp`, `CE` = 같은 폴더 `CurveEvaluation.cpp`, `SC` = `SimpleCurve.cpp`, `KH` = `Classes/Curves/KeyHandle.h`, `UMU` = `Core/Public/Math/UnrealMathUtility.h`, `UPM` = `Core/Public/Math/UnrealPlatformMathSSE.h`다. 레거시·deprecated 경로(옛 버전 키 직렬화 보정, `UE_DEPRECATED` 함수, `FInterpCurvePoint` → `FRichCurveKey` 변환 생성자)는 대응하지 않는다.

에셋 층(`UCurveFloat`·`UCurveVector`·`UCurveLinearColor`·`UCurveTable`·`FRuntimeFloatCurve`·`FCurveTableRowHandle`)은 A1 에셋 시스템 뒤 A 단계의 일이라 범위 밖이다. 이 문서는 그 층이 기댈 경계(14절)만 정한다.

## 장 목차

- [요약·타입·모듈·데이터](01-Overview.ko.md) — 1–4절
- [평가 의미·자동 접선](02-Evaluation.ko.md) — 5–6절
- [키 편집·키 핸들](03-EditingHandles.ko.md) — 7–8절
- [리플렉션·직렬화·압축 커브](04-ReflectionSerialization.ko.md) — 9–11절
- [InterpCurve 미러·좌표·에셋 층 경계](05-InterpCurveBoundary.ko.md) — 12–14절
- [UE 5.8과 다른 점](06-UeDifferences.ko.md) — 15절
- [시험·구현 단계](07-TestsStages.ko.md) — 16–17절
- [결정·미확인·변경 이력](08-DecisionsRevisions.ko.md) — 18–20절
