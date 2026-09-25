# 요청: 자주 바뀌는 공용 헤더 쪼개기 (인프라 → 코어, 2026-09-26)

사용자 결정(REBUILD_PLAN 14.3 ⑭ (b))에 따른 제안이다. 코어 소유 헤더이므로 적용은 코어가 한다. 측정 도구는 `Tools/CI/HeaderCost.ps1`이다(`ninja -t deps` + `.ninja_log` + `git log`).

## 1. 측정 [실측]

조건: 워크트리 84e7c8b, `build/infraBase`(모든 트랙), `-Jobs 4 -LowPriority`. `compile_s`는 그 헤더에 의존하는 목적 파일·커널의 마지막 컴파일 시간 합이다(-j4 경합 아래 값). `cost_s` = 최근 2일 커밋 수 × compile_s다.

| 헤더 | obj | 커널 | compile_s | 커밋(2일) | cost_s |
|---|---:|---:|---:|---:|---:|
| `unx/render/Frame.h` | 47 | 0 | 199.5 | 23 | 4,589 |
| `unx/render/RenderGraph.h` | 57 | 0 | 232.7 | 9 | 2,094 |
| `unx/render/GpuSceneLayout.h` | 52 | 0 | 218.5 | 7 | 1,530 |
| `unx/render/GpuScene.h` | 45 | 0 | 190.8 | 7 | 1,336 |
| `unx/render/D3D12.h` | 69 | 0 | 266.4 | 4 | 1,066 |
| `unx/render/Tracks.h` | 24 | 0 | 111.3 | 7 | 779 |

- 빌드 시간 [실측, -j4]: 전체 220 s. `Frame.h`를 건드리면 85 s(211 단계), `D3D12.h`는 86 s, `Math.h`는 91 s. `.cpp` 하나는 10~13 s다.
- `Frame.h`의 최근 23 커밋이 바꾼 부분[실측, diff hunk 귀속]: `ViewResources` 12, `DepthRasterRequest` 5, `FrameResources` 5, `FramePassContext` 5, `ViewDesc` 3, `FrameContext` 2, `RasterView` 2. 주석만 바꾼 커밋은 3개뿐이다. 나머지는 실제 코드 변경이라 다시 컴파일해야 한다.
- `Frame.h`를 포함하는 47 obj 가운데, 자기 소스나 거쳐 오는 프로젝트 헤더(Frame.h·Tracks.h·FrameRenderer.h 제외)에서 이름을 실제로 쓰는 obj 수[실측, grep]:
  - `ViewResources` 28, `FrameResources` 30, `ViewDesc` 28, `FrameContext` 40
  - `DepthRasterRequest`/`RasterView` 10
  - `FramePassContext` 47(모든 트랙 진입점)
- `Frame.h`가 `GpuSceneLayout.h`에서 쓰는 것은 `gpu::ViewKind` 하나다. 그런데 이 include 때문에 `FrameConstants`·`Instance`·`Material` 레이아웃이 바뀔 때마다(최근 7 커밋) Frame.h를 거쳐 47 obj가 다시 컴파일된다.
- `Tracks.h`가 `Frame.h`를 통째로 끌어온다. 예를 들어 `src/GpuLock.cpp`는 `tracks::pending` 선언 하나 때문에 `Frame.h` 전체에 묶여 있다. Host 테스트 4개와 `RendererAbi.cpp`도 Frame.h의 이름을 직접 쓰지 않는데 `HostRenderer.h`를 거쳐 묶여 있다.

## 2. 제안 (적용: 코어)

1. **`Frame.h`를 바뀌는 단위로 나눈다.** 내용 변경은 없고 위치만 옮긴다. `Frame.h`는 지금처럼 전부를 include해서 기존 코드가 그대로 컴파일되게 두고, 각 파일이 필요한 작은 헤더를 직접 include하도록 옮겨 간다.
   - `unx/render/ViewDesc.h`: `ViewDesc`. `Math.h`, 핸들 헤더(3), `ViewKind`(2)만 include한다.
   - `unx/render/FrameResources.h`: `ViewResources`, `FrameResources`(게시판, 변경 1위).
   - `unx/render/DepthRaster.h`: `RasterView`, `DepthRasterRequest`(S·V·FrameRenderer만 쓴다).
   - `unx/render/FrameContext.h`: `FrameContext`, `kGpuSimulation*`, `kDiscontinuity*`.
   - `unx/render/Frame.h`: `TrackState`, `FrameServices`, `FramePassContext`, `passBandCount`. 참조로만 쓰는 타입(`Device`, `RenderGraph`, `ShaderLibrary`, `QualityConfig`, `GpuScene`, `FrameResources`, `DepthRasterRequest`, `ViewResources`)은 전방 선언으로 받는다.
   - `FrameServices::renderView`의 반환형 `ViewResources`: MSVC에서 불완전 반환형으로 `std::function`을 선언할 수 있으면 그대로 둔다. 안 되면 출력 인자 `void(FramePassContext&, const ViewDesc&, ViewResources&)`로 바꾼다. 이것은 R이 쓰는 서비스라 인터페이스 변경이다.
2. **`gpu::ViewKind`를 작은 헤더로 옮긴다**(예: `unx/render/ViewKind.h`, `GpuSceneLayout.h`가 include). 그러면 `Frame.h`가 `GpuSceneLayout.h`를 include하지 않아도 된다.
3. **그래프 핸들 헤더를 분리한다.** `unx/render/GraphTypes.h`로 `Use`, `TextureDesc`, `BufferDesc`, `TextureRef`, `BufferRef`를 옮긴다. `ViewDesc`와 `ViewResources`는 핸들만 필요하다. `RenderGraph`, `PassBuilder`, `PassContext`, `PassBand`처럼 자주 바뀌는 클래스(최근 9 커밋 중 5+3+2+2)는 패스를 짜는 `.cpp`만 include한다.
4. **`Tracks.h`는 전방 선언만 쓴다.** 필요한 것은 `FramePassContext`, `ViewResources`, `DepthRasterRequest`의 전방 선언과 `RenderGraph::BandedPass`다. `BandedPass`는 중첩 타입이라 `GraphTypes.h`로 빼거나 `RenderGraph.h`를 include한다. `tracks::pending`은 `Tracks.h`에 남기되 `Frame.h` 없이 선언되게 한다.
5. **트랙 쪽 헤더는 이번에 바꾸지 않는다.** `VsmSystem.h`, `GiSystem.h` 같은 트랙 시스템 헤더가 `Frame.h` 대신 작은 헤더를 include하도록 바꾸는 것은 각 트랙이 한다. 코어가 1~4를 커밋하면 인프라가 HeaderCost로 트랙별 목록(어느 헤더가 무엇을 쓰는지)을 만들어 각 트랙에 직접 보낸다.

## 3. 예상 효과 [예상]

위 1절의 "실제로 쓰는 obj 수"로 셈했다.
- `ViewResources`/`FrameResources` 변경(최근 2일 17건): 다시 컴파일되는 obj가 47개에서 약 28~30개로 준다(-40 %).
- `DepthRasterRequest`/`RasterView` 변경(7건): 47개에서 약 10개로 준다(-80 %).
- `GpuSceneLayout.h` 변경(7건): Frame.h를 거치는 전파가 사라진다. 레이아웃 타입을 쓰는 obj만 남는다. 이 수는 적용 뒤 HeaderCost로 잰다.
- `RenderGraph.h` 변경(9건): 핸들만 쓰는 obj가 빠진다. 이 수도 적용 뒤 잰다.
- 이 효과는 PCH·unity 빌드·컴파일러 캐시와 곱해진다. 그쪽 측정은 `20260926_Infra_build_acceleration.md`에 적는다.

## 4. 확인

적용 커밋 뒤 인프라가 같은 워크트리 조건에서 HeaderCost와 증분 빌드 시간(`Frame.h`와 새 헤더 각각을 touch)을 다시 재서 이 파일 아래에 [실측]으로 붙인다.
