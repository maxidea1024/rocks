# 엔진 런타임 — 14. 알려진 제한

[목차](README.ko.md)

## 14. 알려진 제한

- World 타입의 Level 그래프 저장은 구조 시험용이다: 부착 계층은 저장되지 않고(리플렉션 B9의 WeakObjectRef PROPERTY와 World 단계의 결정 뒤), native 파생이 든 그래프는 저장할 수 없으며, 로드한 객체는 레벨 편입 단계 전까지 World 장부 밖이다(4.6절). World 필드는 ReadOnly라 CU5 setter hook 전에는 경로로 편집할 수 없다.
- Tick마다 Borrow를 얻는 잠금 비용이 있다. 일괄 접근은 측정 후 도입한다.
- 렌더 lane(RB1)과 프레임 slot은 구현했다. 렌더 설정 snapshot(CP4), 대체 자원(RB2), 장치 재생성은 후속이다. 지금 Sandbox의 엔진 루프 경로는 self-test(`--self-test`)의 헤드리스 장면이며, World 추출(WorldIntegration)과 창 실행 경로는 화면 마일스톤에서 잇는다.
- macOS·Metal은 미구현이며 해당 경로는 `Unsupported`를 반환한다. Vulkan은 SH2 전까지 개발용 컴파일 훅이 있어야 파이프라인을 만든다(10.3절).
