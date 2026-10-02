# 언리얼 6 렌더러 자체 구현 — 진행 상태

사용자 지시(2026-10-02): 렌더러 전체를 언리얼 6(ue6-main)의 구조로 자체 구현한다. Lumen, 가상화 지오메트리, 그림자, 그 밖의 전부. 목표는 "화면만 봐도 예쁜" 품질과 4K 6.06 ms(1440p·1080p는 그보다 훨씬 빠르게). 금지는 하나: "이건가? 아니네 → 검증 → 이건가? 아니네" 반복. 검증은 최소, 코드 작업 위주.

- 브랜치 `lumen-ue6`, 작업 트리 `C:\Users\USER\UnravelNext-ue6`. `redesign-v2` + `redesign-v2-refl` + `redesign-v2-fix` 병합에서 시작.
- 빌드: `Tools\CI\Build.ps1 -Track all -Jobs 16`(PowerShell에서). GPU 실행은 아직 한 번도 하지 않았다.
- 언리얼 소스(읽기 전용): `C:\Users\USER\RendererResearch\UnrealEngine-ue6-main`, `...\UnrealEngine-5.8.2`.
- ue6 기본 경로 확인: `r.LumenRef.Supported = false` → 기본은 Lumen(카드 표면 캐시 + 화면 프로브 수집 + radiance cache + 반사). `r.Lumen.HardwareRayTracing.LightingMode = 0` → hit 조명은 표면 캐시.

표기: ✔ 코드 작성·빌드 통과(GPU 미실행), ◐ 일부, ☐ 미착수.

## 1. Lumen

| 항목 | 상태 | 위치 |
|---|---|---|
| 세 브랜치 병합, 전체 빌드 | ✔ | 50af9e4 |
| 메시 카드 생성: 프레임 밖 작업 스레드 + 디스크 캐시(메시 내용 해시), 배율 1/4옥타브 묶음 | ✔ | `Passes/SurfaceCache/MeshCardCache.cpp` |
| 카드 씬: 인스턴스 추가·이동·숨김·제거·재질 변경(재캡처), 상주 해상도, 페이지 표 고정 크기 | ✔ | `MeshCardScene.cpp`, `SurfaceCacheCards.cpp` |
| 카드 캡처: 페이지마다 그 인스턴스의 원본 삼각형을 메시 셰이더로 정사영 → 알베도·법선·방출·깊이 | ✔ | `CardCapture.ms/.ps.hlsl` |
| 재할당 카드의 조명 이어받기(resample), 캡처 → 아틀라스 복사, 새 페이지 조명 상태 | ✔ | `CardResample.hlsl`, `CardCopy.hlsl` |
| 오래된 페이지 재캡처(예산의 1/8, 재질 변경은 먼저) | ✔ | `MeshCardScene::update` |
| 카드 조명: 갱신 선택, 직접광(타일당 8광원 + 태양, 스레드당 광선 1개), radiosity, FinalLighting | ✔ | `CardLighting.cpp`, `Card*.hlsl` |
| 프레임 순서: 카드 갱신이 GI 앞(`tracks::surfaceCache(fc, main)`), `FrameResources::cards` | ✔ | `ReflectionTrack.cpp`, `FrameRenderer.cpp` |
| 화면 프로브 광선·radiance cache 광선의 hit이 카드를 읽음(해시 셀 읽기 제거) | ✔ | `LgTrace.hlsl`, `LumenRadianceCacheTrace.hlsl` |
| 반사 hit이 카드를 읽음(프레임의 카드 사용) | ✔ | `ReflectionSystem.cpp`, `ReflectionShade.hlsli` |
| 레벨 로드 때 여러 갱신을 한 프레임에(로드 직후 몇 프레임 안에 캐시가 참) | ✔ | `mesh_cards_load_rounds` |
| 표면 캐시 피드백(고해상도 페이지 요청·퇴출) | ☐ | |
| 반사 경로를 Lumen 반사 단독 구현으로(스레드당 광선 1개, 옛 K/G/M 경로와 분리) | ✔ | `reflection.lumen_only`: `ReflectionLumenTrace.hlsl`, `ReflectionLumenHit.hlsli`, 굴절 서비스 `RefractionLumenTrace.hlsl` |
| 최종 수집만 도는 GI(옛 월드 캐시·그 화면 프로브 없음, 캐시 메모리도 잡지 않음) | ✔ | `gi.lumen_only`: `GiSystem.cpp`, `LumenGather.cpp` |
| 최종 수집의 컷 프레임·이동 중 처리 대조 반영 | ✔ | 1.1 표 |
| Lumen translucency volume(시야 정렬 격자 32 px × 26단, 칸당 3×3 광선 → radiance cache, 분리형 필터, SH + 시간 누적) | ✔ | `Passes/GI/LumenTranslucencyVolume.*` |
| 옛 월드 GI 캐시를 읽던 곳을 translucency volume으로: 입자, 볼륨 입자(매질), 물 표면, 유리(반투명 합성), coverage 조각(가는 형상·가장자리), 평면 반사 뷰, 잎 뒷면 | ✔ | `Passes/GI/GiSource.hlsli`, `Frame.h`의 `GiSource` |
| M 합성을 언리얼의 DiffuseIndirectComposite 구조로: 확산 × 짧은 거리 AO(다중 반사), rough specular × 스페큘러 가림, 그 위에 반사를 몫만큼; 클리어코트는 윗층 = 코트 거칠기로 추적한 반사, 아랫층 = rough specular | ✔ | `ShadeOpaque.hlsl`(part 2), `ReflectionInternal.hlsli`의 `reflTopLayerRoughness` |
| 설정 묶음과 기본값(`Config/quality`): 아래 스위치가 전부 기본 켬 | ✔ | `surface_cache.enabled`·`mesh_cards`, `gi.lumen`·`lumen_only`, `reflection.lumen`·`lumen_only`, `lumen.radiance_cache`·`short_range_ao`·`translucency_volume`, `output.screen_trace_source = 0` |

옛 경로(월드 GI 해시 캐시, 그 화면 프로브, 반사의 K/G/M·rays buffer·inline·shade·combine 패스, 해시 셀 표면 캐시)는 스위치를 끄면 그대로 돈다. GPU 실행으로 새 경로를 확인한 뒤 코드에서 지운다.

### 1.1 최종 수집·반사 대조 결과 (2026-10-02, 코드 대조)

구조와 상수는 언리얼과 같다(프로브 배치 16 px·적응 0.5, 8×8 추적, 중요도 표본, 세기 상한 10, 프로브 필터 3패스, 화소 시간 필터 10프레임, 반사 재구성 5표본·시간 12프레임·양방향 필터). **언리얼에 컷 프레임 전용 장치는 없다.** 컷 직후가 깨끗한 까닭은 필터에 들어가는 값이 조용하기 때문이다: 시야와 무관하게 남아 있는 표면 캐시·radiance cache, 그리고 청색 잡음. 고친 것(전부 코드 작성·빌드 통과, GPU 미실행):

| # | 내용 | 상태 |
|---|---|---|
| 1 | hit 조명을 결정적으로: 카드 읽기. 카드 없는 hit은 태양 원반 중심으로 그림자 광선 1개, 국소광 표본 없음 | ✔ |
| 2 | radiance cache 켬, 프로브 광선은 캐시를 8프로브 가중으로 읽음, hit 거리 = 캐시의 거리 | ✔ |
| 3 | 화면 추적을 캐시 범위 거리에서 끊음 | ✔ |
| 4 | 청색 잡음 타일(64×64 × 4채널, 프레임마다 황금비 회전): 광선 텍셀 중심, 화소 지터·프로브 고르기, 반사 광선 | ✔ |
| 5 | 거친 스페큘러 4표본을 Hammersley로, 0.9 편향을 극각 좌표에 | ✔ |
| 6 | 프로브 disocclusion 판정에 이력 유효성(같은 면) 검사 | ✔ |
| 7 | 보간 최소 가중 0.03 → 0.01 | ✔ |
| 8 | 화면 추적: hit의 이전 프레임 깊이 검사(보관한 장면 색의 알파에 그 프레임 깊이), 두께 단계 0 + 잎 hit 건너뜀, 입력은 업스케일 전 장면 색 | ✔ |
| 9 | 짧은 거리 AO를 시간 필터 뒤 M 합성에서(다중 반사 + 스페큘러 가림은 rough specular에만) | ✔ |
| 10 | 스냅 프레임의 노출 기준(수집이 잰 값)을 반사 필터의 상한·톤맵 평균에도 | ✔ |

## 2. 가상화 지오메트리 / 그림자 / TSR·후처리 / 그 밖 — 언리얼 대비 차이 (2026-10-02 조사)

### 2.1 지오메트리 (V) — 구조는 Nanite와 같다(클러스터 DAG, 그룹 오차 LOD, 2단계 HZB 컬링, vis buffer)

| Nanite | 우리 | 결과 |
|---|---|---|
| 소프트웨어 래스터(변 32 px 미만 클러스터) | 없음(메시 셰이더만) | 작은 삼각형이 주 뷰와 그림자 뷰에서 하드웨어 셋업 비용을 냄 |
| DAG 안의 복셀 클러스터(잎·가는 형상도 계속 단순화) | 없음(가는 그룹은 말단, 밴드 C 런타임 없음) | forest_thin: 원본 삼각형 1.6억, visible 758만 요청(상한 100만) → 8.98 ms |
| 래스터 빈·앞→뒤 깊이 버킷 | 고정 리스트 8개 | coverage 층이 가려진 조각을 저장(물가 61.8 %) |
| 클러스터 압축(양자화·스트립) | 무압축 | 메모리·대역폭 3~4배 [추정] |
| 페이지 스트리밍 + GPU 피드백 | 스트리머만 있고 연결 안 됨 | 전부 상주 |
| 그림자 뷰 HZB 가림 | 없음 | 가려진 캐스터도 래스터 |
| 레벨당 패스 1개(ping-pong) | 레벨당 prepare 패스 추가 | 순회 패스 2배 |
| 용량 visible 4M | 1M | 숲에서 넘침 |

실측 [문서 인용]: city_block 4K V 합계 0.238 ms. 현재 게임(실내·가벼운 그래픽)에서는 병목이 아니다.

### 2.2 그림자 (S)

| 언리얼 | 우리 | 결과 |
|---|---|---|
| 화면 공간 접촉 그림자(4표본, 0.015 × 깊이) | 없음 | 물체가 떠 보임, 접촉부 세부 없음 |
| MegaLights 화면 추적·청색 잡음·LightPowerDelta | 없음 | 접촉 누설, 광원 변화 반응 느림 |
| 항상 상주하는 굵은 페이지 + 페이지 팽창 | 없음 | 컷 직후 요청이 늘면 그 프레임에 페이지 빠짐 |
| 정적/동적 페이지 분리 + 병합 | 없음 | 움직이는 캐스터가 그 밑의 정적 형상까지 다시 그리게 함 |
| 페이지별 HZB, HZB로 거른 무효화 | 없음 | 전체 다시 그리기 비용 |
| SMRT 확률 광선 7×8 + TSR | 결정적 차단체 탐색 + 16탭 | 우리 쪽이 첫 프레임 잡음 없음(유지) |
| 16광원 one-pass 투영 | 슬롯 3 + 넘침 목록, MegaLights가 대체 | MegaLights 기본화로 해결 |

### 2.3 TSR·후처리·그 밖 (M)

| 영역 | 차이 | 보이는 결과 |
|---|---|---|
| 시간 업스케일 | 셰이딩 거부·disocclusion 검사·깜빡임 억제·컷 프레임 공간 AA 없음 | 움직임 뒤 잔상, 조명 변화 지연, 컷 직후 흐렸다가 선명해짐 |
| 톤 파이프라인 | 곡선은 같음. blue correction·gamut 확장·ACES glow/red modifier·장면 기준 그레이딩 LUT 없음 | 채도 높은 파랑·빨강의 색상 이동 |
| 국소 노출 | 없음 | 역광에서 창이 날아가거나 실내가 뭉개짐 |
| 블룸·비네트·그레인 | 코드는 있고 기본 끔. 렌즈 플레어·샤픈 없음 | 평평한 인상 |
| 높이 안개·볼류메트릭 안개·국소 안개 | 없음(프록셀에 대기·국소광 공기만) | 거리감 없음 |
| 짧은 거리 AO·스페큘러 가림 | 이식돼 있고 꺼져 있음 | 물체가 뜸 |
| 서브서피스(Burley) | 없음(Standard로 셰이딩) | 피부·왁스가 플라스틱 |
| 반투명 속도 벡터 | 없음 | 물·유리·입자 잔상 |
| 구름 | 1/4 해상도, 시간 재구성 없음, 3.27 ms | 사실상 못 씀 |
| 모션 블러 | 업스케일 전 내부 해상도, 32 px 제한 | 짧고 거친 줄무늬 |

### 2.3.1 2026-10-02에 채운 것 (코드 작성·빌드 통과, GPU 미실행)

| 항목 | 내용 | 위치 |
|---|---|---|
| 시간 업스케일 = TSR 구조 | 벡터 팽창(3×3 최근접 깊이) + 최근접 가림체 scatter로 시차 disocclusion, 저해상도 guide 이력과 그에 대한 셰이딩 거부(3×3 연산 사슬을 그룹 메모리에서 한 패스로), 거부·이력 없음 화소의 공간 AA(가장자리 8화소 탐색), 유효도 가중 이력 갱신(16표본, 1 px/프레임 이동 시 4, 거부 시 2), 이력 clamp는 거부가 말하는 만큼만 | `Passes/Shading/Tsr*.hlsl`, `Tsr.hlsli`, `Upscale.cpp` (`output.upscale_tsr`, 기본 켬) |
| 톤 파이프라인 | 필름 곡선 앞뒤로 gamut 확장(1.0), blue correction(0.6), ACES glow, red modifier | `ShadingCommon.hlsli` `shFilm` |
| 블룸·비네트 기본값 | 블룸 0.082(언리얼 기본 세기 0.675 × 6단 틴트 / 6의 몫), 비네트 0.4(모서리 원 기준 cos⁴) | `Config/quality/shading.toml`, `PostFinal.hlsl` |

깜빡임(moire) 휴리스틱도 들어 있다(`TsrFlicker.hlsl`, `output.upscale_tsr_flickering`): 서 있는 화소의 luma가 지터 주기로 뒤집히면(타일 줄눈·격자) 그 진폭 안에서는 이력을 버리지 않는다. 언리얼은 반투명 이전 색을 따라가고, 여기서는 최종 장면 색을 따라간다.
TSR에서 아직 없는 것: history resurrection, reprojection field(자코비안·경계), thin geometry 검출, 출력보다 큰 이력 해상도.
반사에는 `reflection.lumen_downsample`(기본 1, 2 = 2×2당 광선 1개 + 이웃 블록 광선으로 resolve)이 있다. 언리얼의 DownsampleFactor와 같은 손잡이로, 첫 실행에서 시간과 그림을 둘 다 재고 정한다.
국소 노출·샤픈·렌즈 플레어는 언리얼에서도 기본 꺼짐이라 뒤로 둔다.

### 2.4 작업 순서

1. Lumen 마무리(1.1 표, 옛 GI 캐시 제거, 반사 정리, 기본값).
2. 시간 업스케일을 TSR 구조로(거부·disocclusion·공간 AA·깜빡임).
3. 톤 파이프라인·블룸·국소 노출·기본값("한눈에 예쁜" 부분).
4. 짧은 거리 AO + 스페큘러 가림, 접촉 그림자, MegaLights 화면 추적.
5. 안개(높이 + 볼류메트릭).
6. 그림자 페이지 구조(정적/동적, 굵은 페이지, HZB).
7. 서브서피스, 반투명 속도.
8. 지오메트리(소프트웨어 래스터, 복셀 클러스터, 압축, 스트리밍).

## 3. 언리얼과 다르게 둔 점

1. 카드 캡처는 클러스터 래스터가 아니라 메시의 원본 삼각형을 그린다(카드 해상도에 맞는 LOD 컷으로 바꿀 수 있음).
2. 카드 아틀라스 무압축(언리얼은 BC 압축): 기하 201 MB + 조명 약 290 MB.
3. 표면 캐시 피드백(고해상도 페이지) 없음 → 카드 해상도는 거리 규칙의 상주 단계뿐.
4. 카드 법선: 법선 맵의 평균 기울기까지 반영(언리얼과 같음). 재질 층(코트·시트)은 캡처에 없음.
5. radiosity 광선의 가까운 뒷면 다시 쏘기 없음(스레드당 광선 1개 규칙).
6. 카드가 없는 hit(스킨·바람 인스턴스): 언리얼은 0. 여기서는 태양만 그림자 광선 1개로 준다(간접광·국소광 없음).
7. 레벨 로드 때 갱신을 프레임당 8회 돌린다(언리얼은 프레임당 1회로 약 100프레임에 걸쳐 채운다).
8. 메시 카드 생성 결과를 디스크에 캐시한다(언리얼은 쿡 때 만든다).
9. 잎 뒷면 확산광: 언리얼은 화면 프로브를 뒤 반구로 적분한다. 여기서는 translucency volume에서 읽는다(낮은 주파수).
10. 평면 반사 뷰와 coverage 조각(가는 형상·가장자리)의 간접광: 주 뷰의 translucency volume에서 읽는다. 주 뷰 시야 밖의 점(거울 속 카메라 뒤쪽)은 가장 가까운 칸의 값을 받는다. 언리얼은 거울을 평면 카메라로 그리지 않고 반사 광선 + 표면 캐시로 그린다.
11. 화면 추적의 이전 깊이 검사는 언리얼의 수치(근평면 10 cm 기준 장치 깊이 차 0.005 × 0.5~2)를 그대로 옮겼다. 깊이는 fp16 알파에 둔다.
12. 클리어코트: 반사 분류·표본·필터가 코트 거칠기를 쓴다(언리얼의 TopLayerRoughness). 코트가 거울처럼 매끈하면 평면 반사 카메라 후보가 된다.

## 4. 품질을 내주는 값(언리얼 기본값에서 시작, 사용자 결정 대상)

- `surface_cache.radiosity_max_ray_intensity = 40`
- `surface_cache.radiosity_max_frames_accumulated = 4`
- `surface_cache.shadow_rays_opaque = false`(언리얼 기본은 true: 알파 마스크 무시)
- `gi.lumen_max_ray_intensity`, `reflection.lumen_max_ray_intensity = 40`, `reflection.lumen_max_roughness = 0.4`, `reflection.lumen_ggx_sampling_bias = 0.1`

## 5. 완성 뒤 한 번 돌릴 실행

- 장면: BathhouseTycoon lobby·bath·lounge, TrainExorcist lounge (`Cache/ReflJudge/scenes/`의 고정 사본).
- 게임 경로(`--capture-output`, 내부 해상도 → 출력 1080p·1440p·4K), `gi.deterministic=true`.
- 데운 뒤 컷 f60·f61·f63, 회전 중 프레임을 그림으로. 장치 제거 시 `UNX_DRED=1`.
- 패스별 시간(`--out DIR`의 패스 CSV)으로 6.06 ms 대비 표.
