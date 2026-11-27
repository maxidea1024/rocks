## 8. 검증

동작 계약은 UE 5.8 소스의 행 단위 근거를 단 고정 기준값으로 검증한다. 기준값 파일에는 장면 정의, 기대값, 근거(UE 경로·행)를 함께 기록한다. UV6 픽셀 비교와 UV8 비교 캡처를 제외한 시험은 창·GPU 없이 CPU에서 결정적으로 실행한다. 시간 동작은 주입한 시계를 명시적으로 전진시켜 시험하며 실제 시간 대기에 의존하지 않는다(3.10절).

| ID | 대상 | 기록 값 | 장면 구성 |
|---|---|---|---|
| UV1 | 레이아웃 | Element별 geometry(로컬 크기·layout 변환) | 가로·세로 stack의 Auto·Fill·FillContent·최소·최대·축소 조합, 흐름 방향 반전, AnchorCanvas anchor·offset·정렬·자동 크기, Grid span, WrapStack, FitBox 방식, ScaleLayer, DPI·앱 배율 변화 |
| UV2 | 무효화 | 사유별 처리 횟수, 원하는 크기 재계산 횟수, 다시 그린 Element 수 | 같은 값 설정(0회), 사유 정규화와 RenderTransform·Visibility의 부모 Layout, 크기 불변 Layout(부모 미전파), cache root 빠른 경로 재사용, volatile, 기본 경로(창 전체 cache 꺼짐) |
| UV3 | 입력 라우팅 | 수신 Element·순서·처리 여부·응답 적용 순서 | 전처리기 구간, capture, preview tunnel·bubble, 보이기 5종, 겹친 layer, 비활성 Element 경로 자름, 팝업 바깥 클릭, modal 창, IME 조합 중 게임 전처리기 |
| UV4 | focus·navigation | 입력 후 사용자별 focus Element·원인 | tab 순서, bubble 중 navigation 변환, 기하 navigation, 규칙 6종, 사용자 2명, 비활성·접힘 Element 포함 |
| UV5 | 텍스트 | 줄바꿈 위치·glyph advance·BiDi 순서·grapheme 커서 위치·IME 조합 범위 | 고정 글꼴 파일, 한국어·영문·아랍어 혼합, culture별 보조 글꼴, 한국어 IME 조합 중 줄바꿈, culture 변경 무효화 |
| UV6 | 그리기 | 그리기 목록 덤프·batch 수·clip 상태 | clip 5종, 회전 render transform, Overlay layer, 지연 그리기, 같은 장면의 offscreen 픽셀 비교(GPU job) |
| UV7 | Widget 층 | 수명 사건 순서·GC 결과·통지 횟수·진단 코드 | Element 해제 ↔ 해체 통지, root 유지와 GC, slot 속성 반영, 구독 없는 통지 비용, Widget 풀 재사용, 결합 표·ID 검증 오류, Designer 미리보기와 런타임 결과 동일 |
| UV8 | 기본 룩 | 테마 색·글꼴 구성·글꼴 크기·여백·brush 값 | 세 기본 스타일 집합의 Rocks 값과 UE 출처 행 대조, UE 에디터와 같은 구성 화면 비교 캡처 |
| UV9 | 화면·도구 | 화면 스택 전환·입력 모드·focus 복원, 독 배치 저장·복원, 명령 단축키 처리 순서 | 스택·큐 push/pop, 뒤로 가기, 입력 차단 root, 입력 장치 전환, 배치 버전 올림, 미처리 키의 전역 명령 |

UV1·UV3·UV4는 소스 근거 기준값에 더해 UE 5.8로 빌드한 독립 덤프 프로그램을 실행한 결과를 1회 기준값으로 채택한다(UD6). 저장소에는 UE 코드가 아닌 덤프 결과만 기준값으로 넣는다.
