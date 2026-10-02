# S2 인계 노트 (2026-10-02)

S2(반사 새 경로 + 표면 캐시 역할) 세션이 멈추면서 남기는 한 장. 새 세션이 코드를 이어 쓸 때 읽는다.

## 1. 위치

- 작업 폴더 `C:\Users\USER\UnravelNext-refl`(UnravelNext의 worktree), 브랜치 `redesign-v2-refl`, 원격 `origin/redesign-v2-refl`.
- 코드 마지막 커밋 `d8a3024`(카드 조명, **빌드는 통과·GPU에서 한 번도 안 돌림**). 그 뒤는 이 노트 커밋뿐.
- 빌드: `Tools/CI/Build.ps1 -Track all -Jobs 8`(통과, 모든 커널 DXIL 204,800 B 아래). GPU 실행은 `Tools/CI/GpuLock.ps1`로만. 지금 `.gpulock/HOLD`가 놓여 있다(사용자 지시: GPU 실행 금지).
- 병합 상태: A의 `f8f16c9`(인터페이스 문서)까지, R의 `bd2d4f9`까지 들어와 있다. R의 그 뒤 커밋(`b5bab64`까지 11개)은 아직 병합 안 함.
- S2 줄에 걸린 GPU 실행은 없다.

## 2. 끝난 것 (커밋돼 있고 돌려 본 것)

| 항목 | 스위치 | 상태 |
|---|---|---|
| 반사 새 경로(언리얼 Lumen 반사 구조): 거칠기 0.4 아래 화소당 GGX-VNDF 광선 1개, BRDF 가중 이웃 재사용, 시간 누적 ≤ 12프레임, 양방향 필터, GGX 표본 편향 0.1 | `reflection.lumen` | 로비·홀·기차에서 실행됨 |
| 화면 추적(HZB) → 놓친 광선은 화면 추적 끝에서 월드 광선으로 이어감(끝 − 0.08 m), 월드 hit이 화면에 보이면 이전 프레임 색 | `reflection.lumen_screen_traces`, `lumen_screen_continue`, `lumen_scene_color_at_hit` | 실행됨 |
| prevSceneColor 소스 선택 | `output.screen_trace_source`(1 = 업스케일 이력, 0 = 업스케일 전 장면 색 = 언리얼 기본) | 둘 다 로비 8프레임 실행됨. **기본은 아직 1**: R의 `LumenGather.cpp`(369행 근처)가 색 텍스처 크기를 출력 크기로 가정한다. `RenderGraph::desc(view.prevSceneColor)`에서 크기를 받게 고친 뒤 0으로 돌린다 |
| 띠 dispatch: 추적 띠 = 262,144 / raysPerSample 작업, 인라인 65,536, 셀 8,192 | 상수(`ReflectionSystem.cpp`) | 실행됨 |
| 셀 해시 표면 캐시(대용품) | `surface_cache.enabled`(기본 false) | 실행됨. **한계는 4절** |
| A의 쌍 구조 직접광(셀·광원 쌍당 스레드 하나, 광선 1개) | `surface_cache.direct_pairs`(기본 true) | 라운지 통과(A의 실행) |
| DRED(장치 제거 때 breadcrumb·page fault) | 환경 변수 `UNX_DRED=1` | 동작. **패스 이름 문자열은 고친 뒤(b30745a) 확인 못 함** |
| 기준 경로 추적기 도구 선택지 | `unx_reference --no-sun-caustics --ev100` | 실행됨 |

문서: `Docs/Status/UNREAL_COMPARISON_REFLECTION_KO.md`(6절 이식 상태, 7절 판정 1), `Docs/Status/CLOUD_BRIEF_KO.md`의 S2 절들.

## 3. 쓰다 만 것: 카드 조명 (`surface_cache.mesh_cards`, 기본 false)

인터페이스는 `Docs/Status/MESH_CARDS_INTERFACE_KO.md`(A의 2~8절, S2의 9절). 커밋 `d8a3024`에 든 것은 **전부 컴파일만 됐다**. 값이 맞는지는 모른다.

쓴 것:
- `Passes/SurfaceCache/CardLayout.hlsli`: A의 `MeshCards.hlsli`가 오기 전의 **임시 대역**(레코드·로더·`mcCardSample`). 레코드 버퍼를 raw로 읽는다. A의 헤더가 오면 지우고 include를 바꾼다.
- `Passes/SurfaceCache/CardLighting.hlsli`: 아틀라스·select 버퍼 배치, 프로브 조회 `clProbeAt`(레벨 페이지 표로 이웃 페이지까지), 평면 가중, hit 읽기 `clReadCards`.
- 커널(패스 이름): `CardClear`(r.card.clear), `CardFrame`(r.card.frame: 카드 프레임 버퍼 = 읽는 쪽이 SRV 하나로 전부 찾는 표), `CardSelect.STAGE0/1/2`(우선순위 히스토그램 16통 → 최대 통 → 타일 목록; 예산 직접 = 아틀라스 타일 / 32, 간접 / 64), `CardDirectCull`·`CardDirectTrace`·`CardDirectStore`(타일당 8광원 + 태양, 스레드당 그림자 광선 1개, 262,144 띠, 균일 가시 비트, 해석적 조도, FinalLighting), `CardRadiosityTrace`·`CardRadiosityProbe`·`CardRadiosityIntegrate`(프로브 간격 4, 4×4 균등 반구 광선, 세기 상한, 이웃 4개 필터 + 평면 가중, SH, 4프레임 누적, FinalLighting).
- C++: `Passes/Reflection/CardLighting.cpp`·`include/unx/refl/CardLighting.h`(영속 아틀라스, 패스 기록, `prepare`/`record`/`declareRead`), `CardTestSet.cpp`·`CardSet.h`(`surface_cache.mesh_cards_test_set`: CPU로 만든 시험용 카드 집합 — 강체 인스턴스마다 축 방향 카드 6장, 재질 상수, 텍스처 없음. 렌더러 경로가 아니라 시험 기구다).
- 배선: `ReflectionSystem.cpp`가 광선 헤더 word 10(바이트 40)에 카드 프레임 SRV를 싣고, `ReflectionShade.hlsli`의 hit이 `clReadCards`로 직접 + 간접 조도를 받아 자기 재질로 셰이딩한다. 카드의 직접광에 태양이 들어 있으므로 카드를 읽은 hit은 태양을 따로 더하지 않는다. 스위치가 켜지면 셀 해시 버퍼는 만들지 않고 `surfaceCacheBuffer()`가 invalid를 돌려준다.

남은 것:
1. **A의 실제 카드 집합 연결**: `unx/refl/MeshCards.h`·`MeshCards.hlsli`가 아직 없다. `ReflectionSystem.cpp`의 `cardSet`은 지금 시험 집합에서만 온다. A의 것이 오면 `CardSet`을 `refl::meshCards(fc)`에서 채운다.
2. **R의 GI hit**(`LgTrace.hlsl`, `LumenRadianceCacheTrace.hlsl`)과 **굴절 hit**(`RefractionTrace.hlsl`)은 아직 카드를 안 읽는다. 지금은 스위치를 켜면 R의 hit이 월드 GI 캐시로 떨어진다. `ReflectionSystem`에 카드 프레임 접근자(`m_cardLighting->frame(fc)`, `declareRead`)를 밖으로 내는 함수가 없다 — 추가해야 한다. 조정 세션은 "`scRead` 서명 유지"를 원했지만 카드 조회에는 씬 인스턴스 번호가 필요하다(`RtSurface.sceneInstance`).
3. 넘침 라이브러리(`ReflectionTraceInline`)는 DXIL 한도에 붙어 있어 카드 읽기를 뺀 채로 컴파일한다(`REFL_NO_CARDS`). 넘침 작업의 hit은 표면 캐시 없이 셰이딩된다.
4. 피드백(고해상도 페이지 요청, 갱신 속도 4배)과 재할당 때 resample은 없다(A의 ③b와 같이). 새 페이지의 누적 프레임 수·균일 비트 초기화도 지금은 집합 전체가 다시 만들어질 때(`generation`)만 한다.
5. radiosity 광선의 가까운 뒷면 다시 쏘기(AvoidSelfIntersections)는 뺐다: 스레드당 추적 1번 규칙.
6. 직접광 추적은 압축 목록이 아니라 고정 대응(타일 × 9 × 64 스레드, 일 없는 스레드는 바로 반환)이다. 언리얼은 그림자 추적을 압축한다. 비용 차이는 재지 않았다.
7. 조정 세션 목록의 5번(반사 hit의 8광원 쌍 구조 hit lighting), 7번(셀 해시를 기본 경로에서 빼기: 지금도 `surface_cache.enabled` 기본 false라 기본 경로에는 없다)은 손대지 않았다.
8. 시험: `Results/Local/Refl/furnace_cards.ps1`(퍼니스, 시험 카드 집합)를 써 뒀지만 **돌리지 않았다**.

메모리(실측 아님, 형식 × 크기): S2 아틀라스 4096² R11G11B10F 4장 256 MB + SH 1024² RGBA16F 3장 24 MB + 균일 비트 8 MB. A의 기하 201 MB와 합쳐 약 490 MB.

## 4. 함정

- **셀 해시 표면 캐시는 대용품이다.** 광선이 맞은 곳에만 셀이 생겨서 컷 직후 f1~f16에서 GI hit의 98~100 %가 유효 값을 못 읽었다. 정상 상태에서도 햇빛의 다중 반사를 약 4분의 1만 날랐다(로비, 기준 경로 추적기 대비 벽·바닥 20~57 % 어두움). 원인은 못 찾았다. 더 고치지 않기로 했다.
- **라운지 hang(TDR)**: `r.sc.cells`의 raygen 안 `[loop]`에서 광원 중심으로 쏘는 그림자 광선. any-hit·마스크·NaN 구간은 아님을 확인했고 원인은 모른다. 쌍 구조(스레드당 광선 1개)는 라운지를 통과한다. 그래서 새 커널은 **스레드당 TraceRay 1번, 루프 안 TraceRay 없음**으로 썼다. `r.sc.cells`의 루프 경로는 `direct_pairs=false` 등에서만 남아 있다.
- TDR이 날 수 있는 실행은 먼저 알리고 전달을 확인한 뒤에만 한다(사용자 화면이 리셋된다). 표면 캐시를 켠 라운지 실행은 S2에 금지돼 있었다.
- **직접광**은 광원 전체에 대한 해석적 적분(`mlLightUnshadowed` + Lambert 점) × 광원 중심으로의 그림자 광선 1개다. 광원 위 무작위 점 표본은 쓰지 않는다(누적 없이는 노이즈).
- 값 전체 필터, 수렴 수치, f299는 진전이 아니다. 판정은 게임 경로(`--capture-output`, `gi.deterministic=true`)의 **데운 뒤 컷 f60·f61·f63과 회전 중 프레임을 그림으로**.
- **퍼니스 시험**: `unx_test_reflection_reflectionanalytic.exe --furnace-only`(닫힌 방, Le 1, ρ 0.5, 기대 L = 2; "M mean"이 값 / 기대). 셀 해시용 `Results/Local/Refl/furnace_sc.ps1`, 카드용 `furnace_cards.ps1`. radiosity 없이 0.5, 다 켜면 0.99대가 나와야 한다.
- **판정 장면 고정 사본**: `Cache/ReflJudge/scenes/`(커밋 안 됨) — `bt_lobby_20261001_1802`, `bt_lounge_20261001_0500`, `bt_bath_20261001_1801`, `te_lounge_20261001_0735`(.unxscene). 게임 프로젝트의 원본은 게임 세션이 시험을 돌릴 때마다 다시 쓰여서 사본을 쓴다. 실행 도구는 `Results/Local/Refl/judge_run.py`, 잠금 줄은 `queue.py`.
- 빌드가 `build/all/bin`의 실행 파일·셰이더를 덮어쓰므로 GPU 실행이 걸려 있을 때는 빌드하지 않는다.
- 언리얼 소스(읽기 전용): `C:\Users\USER\RendererResearch\UnrealEngine-ue6-main`, `...\UnrealEngine-5.8.2`. 줄 단위 복사 금지.

## 5. 사용자 결정 항목 목록의 위치

- `Docs/Status/UNREAL_COMPARISON_REFLECTION_KO.md` 5절(언리얼이 품질을 내주는 곳: radiosity 광선 세기 상한 40, 누적 길이, 거칠기 한계, GGX 표본 편향 등 — 언리얼 기본값에서 시작해 사용자가 정한다).
- `Docs/Status/MESH_CARDS_INTERFACE_KO.md` 7절(언리얼과 다른 점)과 9절.
- `Docs/Status/CLOUD_BRIEF_KO.md`의 S2 절(07:30 기준 경로 추적기 판정, 08:15~08:40).
