# Unreal 교환 형식과 추출 플러그인 설계

기준일: 2026-11-27, 개정 25(UX4b 명세 — 8.7절). 개정 24(UX8b 명세 — 7.2절). 개정 23(UX7a2·UX7b 명세 — 6.3절 13번·6.4절). 개정 22(UX3b 명세 — 5.5절). 개정 21(10.3절 오류 값 — `CorruptData` 이전). 개정 20(UX2b2 구현 — 텍스처 임포트 완료). 개정 19(UX2b1 구현 — 10.6절·4.6절). 개정 18(UX2b 명세 — 10.6절 종류별 임포트 공통, 4.6절 텍스처 임포트, UX2b1·UX2b2로 나눔). 개정 17(10.4절 14번 SCC를 Core 공용 `algo::ForEachStrongComponent`로 — 중복 유틸리티 제거, 동작 같음). 개정 16(UX1c 설계 — 임포트 계획·`--only` 닫힘·기존 에셋 조회·종류별 임포터 표·`asset-import-unreal`, 10.5절; A2f 프레임워크에 없는 네 가지를 요구 UXR8–UXR11로 넘김, 13절). 개정 15(UX 묶음 리뷰 반영 — 퇴화 다각형 자기 변, 옛 경로 soft 참조, 원본 복제 실패·스킨 inline reduction·영향 12개 초과, golden 묶음 12.3절, 리더 상한·일반 파일·플러그인 error). 개정 14(UX4를 UX4a·UX4b로 나눔, UX4a 머티리얼 계열 추출 — 8.6절). 개정 13(UX8을 UX8a·UX8b로 나눔, UX8a 애니메이션 클립 추출 — 7.1절). 개정 12(UX7을 UX7a·UX7b로 나눔, UX7a 스켈레톤·스킨 메시 추출 — 6.3절). 개정 11(UX3a 정적 메시 추출 — 5.4절). 개정 10(UX2·UX3을 추출 UX2a·UX3a와 임포트 UX2b·UX3b로 나눔, UX2a 텍스처 추출 — 4.5절). 개정 9(UX1b UE 플러그인 골격 구현 — 11.7절). 개정 8(UX1b를 UE 플러그인 골격 UX1b와 Rocks 임포트 연결 UX1c로 나눔). 개정 7(UX1a coverage 100% — 10.4절 끝 시험과 coverage 문단). 개정 6(UX1a 리더·경로 대응·임포트 순서 구현 — 10.4절, 3.4·3.5절 정렬·의존 `class`). 개정 5(UX1을 UX1a·UX1b로 나눔). 개정 4(UXR5 경계 정책 검사 구현 — 11.1절). 개정 3(13절 요구를 다른 문서에 반영하고 상태 열을 더함, 태그 키 이름). 개정 2(교차 검증 반영 — 18절). UE 에디터에서 텍스처·정적 메시·스켈레톤·스킨 메시·애니메이션 클립·머티리얼을 꺼내 Rocks 에셋으로 가져오는 경로를 정한다. 경로는 세 부분이다. UE 쪽 추출 플러그인 `RocksExport`, 둘 사이의 교환 형식 **Unreal 교환 묶음**, Rocks 쪽 임포터 플러그인 `UnrealExchange`다.

> **콘텐츠 해시 전환(예정, [ContentHash](../ContentHash.ko.md), 2026-11-28).** 이 문서가 콘텐츠 주소·동일성 판정으로 정한 SHA-256 값(교환 번들 blob 이름·해시와 MeshDescription digest(플러그인은 UE `FBlake3`로, 교환 번들 형식 버전을 올린다 — CH-B3c, rocks-10))은 `ContentHash`(BLAKE3-256, 32 byte)로 바뀐다. 하위 호환 없이 일괄 B(CH-B3b)에서 한 번에 바뀌고, 그때 본문과 형식 버전을 고친다. 외부 무결성·프로토콜 SHA-256(CH5)은 남는다.

- 배경: 사용자 지시(2026-11-19). 엔진 시험을 위해 UE에서 쓰던 콘텐츠를 Rocks로 가져온다. `.uasset`을 직접 해석하는 방식은 어려움이 많으므로 UE 플러그인이 Rocks가 읽을 수 있는 형태로 추출한다.
- 받는 쪽 자료구조: 텍스처는 [에셋 시스템](../AssetSystem/12-Textures/README.ko.md) 12절, 머티리얼은 [머티리얼 시스템](../MaterialSystem/README.ko.md) 4절, 메시·스켈레톤·애니메이션은 [메시·스켈레톤·애니메이션 에셋](../MeshAnimationAssets/README.ko.md)이 정한다. 이 문서는 추출과 대응만 다룬다.
- UE 근거: UE 5.8.4(`release`, commit `f45729c0b6f0`, 로컬 클론 `C:\github\UnrealEngine`). 경로 약어: `E/` = `Engine/Source/Runtime/Engine`, `EP/` = `E/Public`, `RT/` = `Engine/Source/Runtime`, `Ed/` = `Engine/Source/Editor/UnrealEd`, `Dev/` = `Engine/Source/Developer`.
- 좌표·단위는 UE와 Rocks가 같다(왼손, +Z 위, cm, 도). 교환 경로 어디에도 좌표 변환을 두지 않는다.
- 착수 시점은 사용자가 정한다. Rocks 쪽 임포트는 받는 에셋 타입의 단계(A5 텍스처, MA 메시·애니메이션, MaterialSystem MS3·MS6)가 끝나야 가능하다(14절). UE 플러그인은 Rocks 구현 순서와 무관한 별도 코드다.

## 목차

- [1. 결정 요약](01-Summary.ko.md)
- [2. 범위](02-Scope.ko.md)
- [3. 교환 묶음 형식](03-BundleFormat.ko.md)
- [4. 텍스처](04-Textures.ko.md)
- [5. 정적 메시](05-StaticMesh.ko.md)
- [6. 스켈레톤·스킨 메시](06-SkeletonSkinned.ko.md)
- [7. 애니메이션 클립](07-AnimationClip.ko.md)
- [8. 머티리얼](08-Material.ko.md)
- [9. 식별·경로·의존](09-IdentityPaths.ko.md)
- [10. Rocks 쪽 임포터 `UnrealExchange`](10-RocksImporter.ko.md)
- [11. UE 플러그인 `RocksExport`](11-UePlugin.ko.md)
- [12. 시험](12-Tests.ko.md)
- [13. 다른 문서에 반영할 요구](13-Requirements.ko.md)
- [14. 단계](14-Stages.ko.md)
- [15. 결정 사항](15-Decisions.ko.md)
- [16. 미확인 항목](16-Unverified.ko.md)
- [17. 개정 이력](17-Revisions.ko.md)
- [18. 교차 검증 기록(개정 2)](18-CrossValidation.ko.md)
