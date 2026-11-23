# 엔진 런타임 — 12. 실행 경로(S6 완료 시)

[목차](README.ko.md)

## 12. 실행 경로(S6 완료 시)

1. Sandbox가 창과 Graphics 장치(`--graphics=d3d12|opengl`)를 만든다.
2. 렌더 lane(RenderBase `RenderLane`)을 시작해 장치 소유권을 lane으로 넘긴다. lane이 스왑체인·렌더러 자원을 만든다(표면 작업·`RenderLaneProcessor::OnLaneStart`).
3. Engine을 초기화하고 World를 하나 만든다. AssetManager가 `Content/`를 마운트한다.
4. 삼각형 Actor(매 Tick 회전)와 SpriteComponent를 가진 Actor(텍스처 에셋)를 Spawn한다. 텍스처는 `RenderHandle`을 예약하고 lane의 자원 작업이 만든다.
5. 매 프레임 `FrameEnd` hook이 프레임 slot을 확보하고(빈 slot이 없으면 가장 오래된 slot의 GPU 완료를 창 메시지를 펌프하며 기다린다), World에서 추출한 그리기 항목을 slot 프레임 arena에 담아 프레임 packet으로 넘긴다. 렌더러는 렌더 lane 안에서 그린다(지금은 Sandbox 샘플 렌더러, 이후 정식 Renderer).
6. 창을 닫으면 `RequestExit` → Engine 종료 hook(CP6)이 렌더 lane 종료 순서를 실행한다([렌더 기반](../RenderBase.ko.md) 5.5절): 새 프레임 차단 → World 파괴 → 진행 중 프레임 GPU 완료 → 최종 GC와 Retire drain 반복 → 내장 자원·스왑체인 Retire와 closure 확인 → lane Drain·장치 소유권 반납·Join → 장치 `WaitIdle`·소멸 → 창 파괴.
