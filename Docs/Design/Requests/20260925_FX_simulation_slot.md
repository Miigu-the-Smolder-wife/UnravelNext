# 요청: FX 트랙 등록, C0 시뮬레이션 진입점, 입자 자원 게시 (FX 트랙, 2026-09-25)

WORLD_VFX_DESIGN_KO.md(이전 저장소 `Docs/Rebuild/`) 7절 V1(GPU 입자 모듈)을 UnravelNext에 넣기 위한 인터페이스 변경이다.
설계서 3.7-2는 `GpuScene::setParticleBuffers(ParticleBuffers)`를 제안했다. 아래 2의 이유로 같은 정보를 `FrameResources`로 게시하는
형태를 제안한다. 코어가 둘 중 하나를 고르면 그대로 따른다.

## 1. 트랙 등록 — 반영됨 (코어 1f2dbf3, INTERFACES v1.24)

- `cmake/Tracks.cmake` FX, `Build.ps1 -Track FX` → `build/FX`, INTERFACES 1절 FX 행, 빈 진입점 `Frame/Stubs/TrackFX.cpp`.

## 2. C0 진입점과 입자 자원

### 변경

1. (반영됨, v1.24) `Tracks.h`의 `void simulation(FramePassContext& fc);`. `FrameRenderer::record`가 `prepareScene`·메인 뷰 프레임
   상수 할당 다음, `atmosphere` 앞에서 부른다(설계서 4.1의 C0). 이 프레임에 제출할 시뮬레이션 tick이 없으면 FX는 패스를 내지 않는다
   (렌더 165 fps, tick 60 Hz → 2.75 프레임에 한 번).
   **남은 요청은 아래 2(`FrameResources::particles`)다.**
2. `FrameResources`에 입자 게시판(생산 FX `simulation`, 소비: FX 렌더 패스, S 프록셀(볼륨 셀), 필요 시 R):

```cpp
struct ParticleResources
{
    BufferRef state;          // 슬롯 SoA (FX 내부 형식; FX의 공개 HLSL 헤더로 읽음)
    BufferRef aliveList;      // tick n 생존 슬롯 목록 (uint), 개수는 counters
    BufferRef dyingList;      // tick n 안에 죽은 슬롯 목록 (보간: 죽음 시각까지 그림)
    BufferRef counters;       // raw: alive, dead, dying, ... (FX 헤더)
    BufferRef records[2];     // 렌더 레코드 tick n-1, n (32 B, 20260925_FX_particle_render_rules.md)
    BufferRef emitters[2];    // 이미터 표 tick n-1, n (원점, 프로그램, 매개변수)
    BufferRef programs;       // 프로그램 표 + 곡선 키 풀
    BufferRef binKeys, binValues;  // 셀 키 정렬 결과 (키, 슬롯)
    BufferRef ribbonVertices; // ribbon 기하 (위치·법선·uv, 이미터 원점 기준)
    BufferRef volumeCells;    // 볼륨 셀 (NV_MediumCell 형식, 96 B)
    uint32_t tick = 0;        // 가장 최근 tick (records[1])
};
ParticleResources particles;  // [FX]
```

3. 게시는 매 프레임이다(tick 패스가 없는 프레임도 지속 버퍼를 import해 채운다). 소비자는 참조를 자기 패스에서 `use`한다.

### 이유 (GpuScene 대신 FrameResources)

- 입자 버퍼는 FX가 소유한 지속 자원이고 tick마다 GPU가 다시 쓴다. INTERFACES 4의 규칙 8("지속 자원은 소유 트랙이 만들어 매 프레임
  import")과 5.1의 게시판이 그 모양이다. `GpuScene`은 호스트가 올리는 장면 레코드(업로드 경로)라 GPU가 쓰는 상태와 배리어가 맞지
  않는다(그래프 밖 자원이 되면 C0 쓰기와 렌더 패스 읽기 사이 배리어를 그래프가 못 낸다).
- 프레임 상수에 SRV 번호를 넣는 설계서 안도 필요 없다. 소비자는 `c.srv(ref)`로 인덱스를 얻는다(5.6의 방식).
- 이미터 원점: 이미터 표에 anchor 기준 float 원점을 두고, anchor − 카메라(double 차)를 FX가 CPU에서 float로 만들어 레코드와 함께
  준다. 프레임 상수 변경 없음.

### 영향

- 코어: `Tracks.h`, `FrameRenderer::record`, `Frame.h`(`FrameResources`), 빈 구현.
- 다른 트랙: 없음(새 필드). S가 볼륨 셀을 프록셀에 합칠 때(설계서 2.9 "볼륨 16") `particles.volumeCells`를 읽는다. 그 형식·비용은
  S와 따로 맞춘다.
- 비용 [실측 원형, `Results/FX/Prototype/repro_*.log`]: C0 파이프라인 524k 입자 tick당 169.6 µs(10 tick 리스트 9회 중앙값),
  프레임당 0.062 ms(×0.364). 게이트 ≤ 0.2 ms/tick.

## 3. 품질 키

- `Config/quality/fx.toml`(FX 소유): `fx.particles.*`(용량 배율, 정렬 패스, 연쇄 깊이 상한, 이벤트 상한 등).
- FX가 읽어야 하는 다른 트랙 키(INTERFACES 9): `atmosphere.froxels.tile_px`, `atmosphere.froxels.depth_slices`(S). 셀 키를
  프록셀 격자로 정할 경우(렌더 규칙 요청 파일)에만 필요하다.
