## 16. Verse 검토와 차용

Epic의 "The road to UE6"(2026-06-17)는 게임플레이 프로그래밍 모델의 Verse 전환을 밝히고, Actor·Blueprint는 초기 UE6에 남았다가 새 framework가 성숙하면 deprecate된다고 기재하였다. Blueprint 즉시 제거는 2차 보도다.[R18] Rocks의 Unreal 기준은 UE 5.8이며 Verse는 언어 설계의 참고 대상이다.

| Verse 기능 | 판정 | Sparks 반영 |
|---|---|---|
| failure context(인덱싱·map 조회) | 의미 차용 | 인덱스·키 읽기는 `T?`. `[]`/`()` 구분 문법은 미채택 |
| `OnBegin<suspends>` | 차용 | handler 안에서 바로 `await`. `Start`는 준비 대기만(SD12) |
| 구조적 동시성(`race` 패배 취소, `branch` 수명) | 차용 | `All`·`Race`, 비구조적 `spawn` 대응 없음 |
| effect specifier | 구조 차용 | 실행 문맥, 재정의는 문맥을 넓힐 수 없음 |
| persistable·publish 호환성 검사 | 차용 | 저장 가능 타입, 매 컴파일 호환성 검사 |
| `weak_map(player, t)` | 목적 차용 | 엔진 객체 원소·키의 자동 정리 |
| 읽기·쓰기 접근 분리 | 단순화 차용 | Sparks 필드는 밖에서 읽기만 |
| `message` 지역화 타입 | 차용 | 화면 표시 API는 `Text`만 |
| 평가 순서 명시, lambda 없음, option 타입 | 일치 | — |
| int / int → rational | 목적만 차용 | `int / int`는 오류, 몫(`Math.Div`)과 비율(`float(a) / float(b)`)을 구분해 쓴다(SD17) |
| 투기 실행·rollback | 보류 | 엔진 C++ 쪽 transactional 기반이 없고 전체 계획 §23.4가 외부 효과 자동 rollback을 하지 않음. UE6의 C++ transactionalize 진행 시 재검토 |
| `live` 변수, `[]`·`:=`·`set` 문법, 포착 불가 오류의 섬 전체 중지 | 미채택 | 미출시 기능, 평범한 문법 요구, 오류 영향 범위 축소 |

Verse의 기호가 많은 문법과 failure·동시성 개념에는 학습 비용이 있다.[R19][R20] Sparks는 C 계열 문법, `T?`, VS Code 디버거로 이 비용을 낮춘다.
