# A 세션 인계 노트 (2026-10-02)

사용자 결정(조정 세션 전달): 세션을 모두 멈추고 새 세션 하나가 코드를 이어 쓴다. 이 노트는 A가 하던 일의 상태다.

## 1. 브랜치

- 작업 트리 `C:\Users\USER\UnravelNext-fix`, 브랜치 `redesign-v2-fix`(origin에 푸시됨). 마지막 코드 커밋 **6ec67ad**(메시 카드 CPU 씬), 그 뒤는 이 노트의 커밋.
- 작업 트리에 커밋 안 된 코드 없음. 추적 안 되는 것은 `Results/Local/Fix-11/` 아래 실행 결과뿐.
- `origin/redesign-v2`보다 21 앞, 12 뒤(R·S2의 최근 커밋은 아직 안 받음). S2는 07:32에 이 브랜치를 한 번 병합했다(3b997db).
- Unity 쪽: `C:\Users\USER\Unravel` 커밋 **6481a58d**(광원별 Ray End Bias의 C# 브리지). **푸시 안 됨, Unity에서 컴파일·실행 안 해 봄.**
- 줄에 건 GPU 실행: 없음.

## 2. 끝난 것 (스위치, 상태 문서 위치)

| 항목 | 스위치 | 기록 |
|---|---|---|
| MegaLights 이식(타일·표본·추적·셰이딩·시간·공간 필터, 볼륨, 보조 뷰, 커버리지 층, A9 면광원 로브, 입자·입자 매질) | `shading.mega_lights` | `S_STATUS_KO.md` 12, 14 |
| 먼 거리 radiance cache(클립맵 4 × 48³, 프로브 32², 프레임 100개) | `lumen.radiance_cache` | 13.2 |
| 짧은 거리 AO / bent normal | `lumen.short_range_ao` | 13.1 |
| 표면 캐시 직접광의 (셀, 광원) 쌍 경로 | `surface_cache.direct_pairs`(기본 true) | 16, 17 |
| 광원별 Ray End Bias(씬 파일 `LEND` 블록, 호스트 ABI `UnxLightDesc` v2, 전역 기본 1 cm) | 광원 속성 + `shading.mega_lights_ray_end_bias_m` | INTERFACES v1.93, 17 |
| dispatch 구조 상한 감사와 수정 | — | `DISPATCH_BOUNDS_KO.md` |
| 메시 카드 생성기(메시당 CPU) | — | 18, 아래 3.1 |
| 메시 카드 CPU 씬(등록·거리별 해상도·아틀라스/페이지 표 할당·프레임 캡처 목록) | — | 아래 3.2 |

위 표의 GPU 항목은 정확도 홀드에서 실행은 됐지만(장치 제거 없음), **사용자 기준(게임 경로에서 컷 직후 1~4프레임과 움직이는 중의 화면)으로 통과한 것은 하나도 없다.** 컷 직후 자료(`S_STATUS` 17.2)는 MegaLights 끔·켬이 같은 오차라는 것만 보였다.

## 3. 메시 카드: 어디까지 썼나

인터페이스 `Docs/Status/MESH_CARDS_INTERFACE_KO.md`. **합의 상태: S2에 보냈고(08:2x, 대기열) 답을 받지 못했다.** 물어 둔 것 3개: (1) 페이지별 조명 상태를 S2 자기 버퍼로 분리, (2) 방출 저장 배율 1/16, (3) resample 갈고리를 뒤 단계로. 문서의 스위치 이름은 `surface_cache.mesh_cards`인데 조정 세션의 목록은 `surface_cache.cards`다(아직 어느 쪽도 코드에 없음: 하나로 정하면 됨).

문서에서 고쳐야 할 한 줄: 카드 페이지 번호 = 페이지 표 번호다(언리얼과 같음: 페이지 표와 카드 페이지 레코드가 같은 번호 공간). `MeshCardScene`은 그렇게 구현돼 있다.

### 3.1 생성기 (끝남, 7299370)

- `Native/Scene/src/MeshCards.cpp`, `unx/scene/MeshCards.h`: `scene::buildMeshCards(mesh, 양면 플래그 또는 재질, maxCards = 12, metresPerUnit)`. 언리얼 생성기(`C:\Program Files\Epic Games\UE_5.8\Engine\Source\Developer\MeshUtilities\Private\MeshCardRepresentationUtilities.cpp` — RendererResearch 쪽 트리에는 Developer 폴더가 없다)와 같은 단계·같은 값.
- 테스트 `unx_test_scene_meshcards`(인자 없이: 작은 메시 8종, 수십 ms). 인자로 .unxscene을 주면 장면 전체 보고 — **이것이 코어를 다 쓴다(아래 5.3).**
- 언리얼과 다른 점은 인터페이스 문서 7절의 8, 9번.
- **안 한 것: 쿡/로드 경로 연결과 결과 캐시.** 계획: 메시 내용 해시(위치·인덱스·양면 플래그·배율·생성기 판 번호)로 디스크 캐시, 없을 때만 생성. .unxscene 경로는 씬 로드 뒤, Unity 경로는 호스트의 메시 등록 때.
- **속도 문제(조정 08:45의 질문)에 대한 측정:** 로비 8,438 메시 1.9초(메시당 평균 약 7 코어·ms: 언리얼과 같은 규모). 기차 라운지는 98초인데 전부 소나무 200그루(메시당 7.5만 삼각형, 겹친 잎)이고, 그 시간의 82 %가 표면 조각 가시성 반구 광선이다(메시당 370만 개, 3번의 복셀 단계 합: 언리얼도 같은 수를 쏜다). 광선 하나에 약 6.5 µs — 원인은 알고리즘이 아니라 **내 BVH의 광선 속도**다(Embree 대비 수십 배 느림으로 추정, 재 보지 않음). 고칠 곳: `TriangleBvh::intersect`가 삼각형마다 인덱스 3개 → 위치 3개를 따라가 읽는다(삼각형을 BVH 순서로 미리 풀어 p0·e1·e2로 저장), 잎 4개·노드 36 B의 스칼라 순회(4폭 SIMD 노드). 열 광선은 이미 열별 삼각형 목록으로 바꿔 빠르다(전체의 18 %).

### 3.2 CPU 씬 (끝남, 6ec67ad)

- `Native/Render/Passes/Reflection/MeshCardScene.cpp`, `unx/refl/MeshCardScene.h`, 테스트 `unx_test_reflection_meshcardscene`(PASS).
- 들어 있는 것: 인스턴스 등록(크기 규칙, 최대 32장, 종횡비 bias), 프레임마다 카드별 거리 → 요청 단계, 거리 통 16개로 프레임 300페이지 선택, 물리 페이지/부분 할당 통 할당기, 페이지 표 span, 잠긴 단계 재할당, 숨김, 캡처 아틀라스(512²) 할당, GPU 레코드(`McMeshCardsGpu` 80 B, `McCardGpu` 112 B, `McCardPageGpu` 64 B, `McPageTableGpu` 8 B)와 dirty 목록, `validate()`.
- [실측, CPU] 로비 카메라에서: 카드 33,931장 중 22,462장이 범위·크기 통과, **98프레임에 다 채워짐**(프레임당 300페이지 / 262,144텍셀), 아틀라스 4096²의 59.1 %, 단계 낮춤 0, `update` 1.7~2.2 ms(단일 스레드: 언리얼은 이 루프를 병렬로 돈다).
  → "데운 뒤 컷 f60" 시험은 60프레임으로는 덜 채워진 상태다(가까운 것부터 채우므로 약 2/3). 데우기를 100프레임 이상으로.
- **안 들어 있는 것:** 피드백으로 올라오는 고해상도 페이지와 퇴출(`EvictOldestAllocation`, UnlockedAllocationHeap), 오래된 페이지 재캡처(LastCapturedPageHeap, 1/8 예산), 인스턴스 제거, 변형·배율 변경 인스턴스의 재캡처 규칙, 카드 공유. 읽어 둔 원본 위치: ue6-main `LumenSceneRendering.cpp` 1046~1345(요청 처리), `LumenScene.cpp` 1600~1976(매핑·재할당·계층), 2157(퇴출), `LumenSurfaceCacheFeedback.cpp` 302.

### 3.3 GPU 쪽 (**한 줄도 안 씀**)

정해 둔 설계(코드 없음):
- `Passes/SurfaceCache/MeshCards.hlsli`: 레코드 로더, `mcCardLocal`, `mcCardSample`(언리얼 `ComputeSurfaceCacheSample`: 페이지 표 → 아틀라스 텍셀 4개와 쌍선형 가중; gather 대신 정수 좌표 Load 4번), `mcFeedback`. 소비자 커널의 루트 상수를 아끼려고 서술자 인덱스·상수를 전부 작은 params 버퍼(`McScene`, 프레임 링) 하나에 담고 소비자는 그 SRV 인덱스 하나만 받는다. 지속 리소스의 서술자는 A가 장치 힙에서 직접 할당(안정된 인덱스), 소비자는 그래프 ref로 `b.use`만 선언.
- `FrameResources`에 `MeshCardRefs`(params 인덱스 + 버퍼 8 + 텍스처 4) 추가, `GiTrack`에서 GI 앞에 `recordMeshCards` 호출.
- 캡처: 페이지 × 서브메시마다 `DispatchMesh` 하나, 메시 셰이더가 그 인스턴스의 원본 삼각형 32개씩(`loadTriangle`/`loadVertex`), 카드 지역 좌표로 정사영(틀: `Passes/Water/WaterSunMap.ms.hlsl`). 깊이 0 = 카드 앞면, D32 + LESS, 뷰포트 = 캡처 사각형, 깊이 클립이 카드 깊이 범위 밖을 자른다. 컬은 파이프라인에서 끄고 픽셀 셰이더에서 (양면 아님 && 기하 법선·카드 Z ≤ 0)이면 버림(카드 축의 좌/우수가 방향마다 달라서).
- 픽셀 셰이더 출력: 알베도 `sqrt(saturate(baseColor (1 − metallic) + 0.45 lerp(0.04, baseColor, metallic)))`(S2가 `scMark`에 주던 정의), 카드 공간 법선 xy(정점 법선: M의 법선 텍스처는 M 내부 형식이라 못 읽음 → 차이 목록에 추가할 것), 방출 nits / 16(`MaterialEmissiveVisibleOnly`면 0), 알파 테스트.
- 복사: 캡처 타깃 → 아틀라스 4장(compute, 캡처당 dispatch 하나; 깊이는 R16_UNORM, 1.0 = 표면 없음).
- 확인용: `debug::registerView`로 "cards.albedo / normal / emissive / valid" 뷰(주 가시 화소의 인스턴스·위치·법선으로 카드 조회) — hit 조회 경로를 그림으로 확인.

## 4. 조정 08:50 목록 기준 남은 일

1. 생성기의 쿡/로드 연결 + 캐시, BVH 광선 속도(3.1).
2. GPU 레코드 업로드, `MeshCards.hlsli`, hit 조회 함수(3.3).
3. 캡처·복사, 피드백·퇴출·재캡처, 동적 인스턴스 규칙(3.2의 "안 들어 있는 것").
4. 프레임 순서 배선과 스위치.
5. Unity 브리지(6481a58d) 컴파일 확인.

## 5. 새 세션이 알아야 할 함정

1. **dispatch 구조 상한**(`DISPATCH_BOUNDS_KO.md`): dispatch 하나의 광선은 262,144개 이하로 나눈다. 욕탕 라운지의 장치 제거(device hung)는 `r.sc.cells`(셀 스레드 하나가 루프로 최대 18번 TraceRay)에서 났고, 쌍 경로(`r.sc.pairs.*`)로 바꾼 뒤 라운지 8·60프레임이 돌았다. **왜 라운지에서만 멈췄는지는 확정하지 못했다.** `surface_cache.direct_pairs_inline`(인라인 질의 변종)은 라운지에서 돌려 보지 않았다(사용자 승인 없음). `r.refl.trace`의 [loop] 안 추적은 S2 항목으로 남아 있다.
2. **루트 상수 충돌**(`S_STATUS` 12.1): 커널이 읽는 P[] 워드를 다른 기능이 이미 쓰고 있어 조용히 틀린 값을 읽은 사례 3건(8d0d065, 43fce2e, 738c2fa). 새 워드를 쓸 때는 그 커널의 기록 쪽 코드와 전부 대조할 것. 루트 상수는 48워드(P[0..11])뿐이다.
3. **카드 생성 시험의 CPU 사용**: `unx_test_scene_meshcards <scene>`과 `unx_test_reflection_meshcardscene <scene>`은 `Jobs`의 전 코어(31)로 장면 전체를 생성한다. 08:25~08:34에 다른 세션의 Unity 실행을 9.5분으로 늘렸다. 장면 인자를 주는 실행은 8스레드 이하·낮은 우선순위로만(지금 `Jobs`에는 스레드 수 제한 수단이 없다: 넣어야 함), 평소 시험은 인자 없이.
4. **hostabi 테스트 실패**: `unx_test_host_hostabi`가 내 빌드에서 "S VSM: sun level 0 can reach 4089109318 cluster entries"로 실패한다(GPU 인스턴스 용량 × 가장 큰 메시). 내 변경 때문인지 확인 안 함, I 트랙에 넘겼다.
5. WRL `ComPtr`에 `&`를 쓰면 포인터가 풀린다(`std::addressof` 사용; `IID_PPV_ARGS(&x)`는 그대로).
6. 커널 DXIL 상한 204,800 B: `FxLayerSetup`은 두 경로를 한 커널에 넣었다가 넘어 `ML=0,1` 변종으로 나눴다.
7. 게이트 로그는 `s.*` 패스만 적는다. 새 패스가 돌았는지는 `--out DIR`의 패스 CSV로 본다.
8. Bash here-doc에 작은따옴표·`\n`이 들어가면 깨진다(패치 스크립트는 파일로 써서 실행).
9. 코브 띠(로비 천장): 옛 경로는 발광면 반경 안 가림을 무시했고 표본 광선은 끝 1 cm 전에서 멈춘다. 광원별 끝 바이어스 0.4 m에서 띠 몫 15.1 %(옛 19.5 %, 1 cm 5.9 %). 레벨 쪽 수정이 같이 결정돼 있다(`MEGALIGHTS_COVE_MEMO_KO.md`).

## 6. 언리얼과 다르게 둔 점·품질을 내주는 값의 위치

- 메시 카드: `MESH_CARDS_INTERFACE_KO.md` 7절(1~9).
- 표면 캐시(옛 셀 방식): `SURFACE_CACHE_INTERFACE_KO.md` "언리얼과 다른 점".
- MegaLights·radiance cache·AO: `S_STATUS_KO.md` 12.4, 13.1, 13.2의 차이 항목, `UNREAL_COMPARISON_LIGHTS_KO.md`.
- 광원 격자 칸당 광원 수 상한 제안(머리 32 + 확률 꼬리 8): `LIGHT_GRID_BOUND_PROPOSAL_KO.md`(결정 안 됨, R 항목).
- 품질을 내주는 값(사용자 결정): `Config/quality/*.toml`에서 `QUALITY TRADE`로 검색(reflection.toml, surface_cache.toml).

## 7. 실행 스크립트·자료

`Results/Local/Fix-11/`의 `run_*.ps1`(전부 `Tools/CI/GpuLock.ps1 -Track A` 안에서 돌리던 것), `postgame/`의 분석 스크립트(`cut_analyse.py`, `cove_analyse.py`)와 결과(`cut_lobby_grid.png`, `cut_train_grid.png`, `meshcards_lobby.txt`, `meshcards_train.txt`, `meshcardscene_lobby.txt`). PFM은 아래에서 위 순서(읽을 때 `[::-1]`).
