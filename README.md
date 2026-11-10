# Rocks

C++23 기반 멀티플랫폼 3D 게임 엔진 프로젝트.

현재 단계는 **설계 및 구현 착수 준비**다. 첫 구현 대상은 Core 기반 라이브러리와 Object 시스템이며, 엔진 실행 파일이나 구현 완료된 기능은 아직 없다.

## 구현 원칙

- 안정성·정확성을 먼저 확보하고 확장성과 성능을 검증한다.
- 엔진 소유 Runtime과 공개 API는 STL을 사용하지 않는다. 외부 라이브러리 및 호스트 도구의 사용 범위는 설계 문서의 격리 규약을 따른다.
- **사람이 알아보기 좋은 코드 스타일 유지.** 스페이스 4칸, 적절한 빈 줄, 명확한 이름과 책임을 사용하며 탭 문자와 불필요한 한 줄 압축을 금지한다.
- 자체 `rocks` 네임스페이스를 사용하며 U/F/A 등 타입 분류용 접두사는 사용하지 않는다.
- Blueprint는 구현하지 않는다. 게임 로직은 C++와 자체 언어를 기준으로 설계한다. 재질 및 UI 등의 전문 편집기는 별도로 구현한다.
- 각 구현에 유닛·계약·장애 주입·수명·동시성 하네스와 재현 가능한 검증 증거를 연결한다.

## 설계 문서

- [전체 구현 계획](Docs/Design/EngineImplementationPlan.ko.md)
- [Core·Object·Input 상세 구현 계획](Docs/Design/CoreObjectInputDetailedPlan.ko.md)
- [작업 목록](Docs/Design/CoreObjectInputTasks.ko.csv)
- [예정 파일 구조](Docs/Design/CoreObjectInputFiles.ko.csv)
- [다회차 설계 검증 결과](Docs/Reviews/DesignReview.ko.md)
- [코드 스타일](Docs/CodingStyle.md)
- [PDF 배포본](Docs/Releases/)

작업 목록과 파일 구조는 구현 계획이다. 해당 파일이나 API가 이미 구현되었다는 의미가 아니다. 설계 검토 통과와 실제 컴파일·실행·성능 시험 통과를 구분한다.

## 착수 순서

1. C00에서 도구 버전, 빌드 진입점, 스타일 검사 및 최소 테스트 호스트를 구성한다.
2. Core의 결과·오류, 메모리 할당, 컨테이너와 수명 규약을 구현하고 하네스로 검증한다.
3. 최소 Object Registry와 참조·타입 메타데이터를 구현한 뒤 full STW GC의 정확성을 검증한다.
4. Foundation Gate를 충족한 뒤 점진·병렬 처리와 입력 및 후속 모듈로 확장한다.

실제 의존 순서와 완료 기준은 상세 계획과 작업 CSV를 따른다.
