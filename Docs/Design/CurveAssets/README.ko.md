# 커브 에셋(CR10)

개정 4(2026-11-27). [컴포넌트 도입](../ComponentCatalog/04-Prerequisites.ko.md) CR10의 설계다. 커브 값 타입 층([CurveData](../CurveData/README.ko.md) — `TangentCurve`·`PlainCurve`, CV1–CV10 완료) 위에 UE 5.8의 커브 에셋 층(`UCurveFloat`·`UCurveVector`·`UCurveLinearColor`·`UCurveTable`·`FRuntimeFloatCurve` 계열·`FCurveTableRowHandle`·`UCurveLinearColorAtlas`)을 들인다. CurveData 14절이 정한 경계를 따른다.

> **콘텐츠 해시 전환(예정, [ContentHash](../ContentHash.ko.md), 2026-11-28).** 이 문서가 콘텐츠 주소·동일성 판정으로 정한 SHA-256 값(커브 atlas 쿠킹 해시)은 `ContentHash`(BLAKE3-256, 32 byte)로 바뀐다. 하위 호환 없이 일괄 B(CH-B3b)에서 한 번에 바뀌고, 그때 본문과 형식 버전을 고친다. 외부 무결성·프로토콜 SHA-256(CH5)은 남는다.

UE 경로 약어: `RE/` = `Engine/Source/Runtime/Engine/`, `UEd/` = `Engine/Source/Editor/UnrealEd/`(UE 5.8.4 — 로컬 클론 `C:\github\UnrealEngine`).

## 목차

1. [요약·범위·대응표·모듈](01-Overview.ko.md)
2. [커브 에셋 클래스(스칼라·벡터·색)](02-CurveAssetClasses.ko.md)
3. [인라인·에셋 커브 원천과 커브 표](03-SourcesAndTable.ko.md)
4. [색 커브 atlas](04-ColorCurveAtlas.ko.md)
5. [임포트·쿠킹·도구](05-ImportCook.ko.md)
6. [소비자 연결](06-Consumers.ko.md)
7. [결정·UE와 다른 점·단계·시험](07-DecisionsStagesTests.ko.md)

## 변경 이력

- 개정 4(2026-11-27): 쿠킹용 결정적 색 평가(2.4.5절, CAD12, CA7 — 사용자 결정, CurveData D22). atlas 굽기를 그것으로 바꾼다(4.3절, 7.2절 22 (g)).
- 개정 3(2026-11-27): atlas를 UE 방식으로 바꿨다(CAD5 — 렌더 rocks-2e·에셋 rocks-5f 권고). atlas가 자기 원본에 굽고 저장하며, 원본 재구성 hook이 Edit·New·Open(UE PostLoad 대응 — AssetSystem 12.14절 A5j2c)에서 굽는다. `disableAdjustments`는 UE처럼 EditorOnly, 정규화가 조정 항등값을 지킨다. 크기 상한, 7.2절 22를 새 차이로 고쳤다.
- 개정 2(2026-11-27): CA5 방식 확정(6.1절 — 소유 에셋의 `curveAssets` + 값 struct slot 번호, `TryResolveCurveSlot`). CA1–CA4 구현·리뷰 반영 내용(UE 5.8 Max·Min 형태, CSV UE 규칙·BOM·숫자 규칙, 쿠킹 Tangent 접선 재계산·한계)을 각 장에 맞췄다.
- 개정 1(2026-11-27): 설계 리뷰 반영. 원천은 참조 struct(객체 필드·StructRefArray만)이고 값 쪽은 평탄 값(CAD7·CAD9), 진동·입력 연결은 CA5 착수 때 방식 확정(6.1절 — 권장 slot 평탄화), 색 HSV는 UE float 식 따로(CAD10 — Core와 다름), 표 모드 불변식은 무시·쿠킹 비우기(CAD3), CSV는 UE 파서 규칙(CAD11), BOM·숫자 trim·비유한·JSON 항목 단위 건너뜀·진단 이름 `InvalidSource`·위치 정의, 쿠킹 Tangent 접선 재계산, atlas 너비 최소 2·역수 곱 표본·조정 중화·CA6 변경 목록, UE 순서·인용 정정.
- 개정 0(2026-11-27): 첫 초안. 사용자 결정(2026-11-27)으로 이 세션이 CR10을 맡는다. 에셋 세션(rocks-5f) 규약(리플렉션 클래스, 기본 쿠킹 처리기, A2f 임포터, CorruptData·InvalidArgument 구분)과 머티리얼 세션(rocks-2b) 요구(`CurveAtlasRowParameter`가 atlas 참조와 행 번호를 쓴다)를 반영했다.
