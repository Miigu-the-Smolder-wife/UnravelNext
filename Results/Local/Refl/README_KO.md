# S2 — 반사·차폐 재구성 층 (재설계 V2 P2 재구성 A: L_rs, L_g, L_occ)

브랜치 `redesign-v2-refl`(시작 70b2d84), 작업 폴더 `C:\Users\USER\UnravelNext-refl`. 설계는 `Docs/Design/RENDERER_REDESIGN_V2_KO.md` 1.2·3·6절 P2 행·10.5다. 이 문서는 구현한 구조와 측정 기록이다. 수치에는 [실측]/[예상]을 붙이고, 실측에는 출력·내부 해상도와 커밋을 적는다.

**현재 상태 (2026-10-01 15시): 층 경로는 아직 기존 경로보다 좋다는 실측이 없다. 기본값은 모두 끔이다.** 하드웨어에서 돈 것은 초기 두 판뿐이고 둘 다 기존 경로보다 시끄러웠다(2절). 그 원인 중 캡처와 코드로 확인된 것을 고친 판(HEAD)은 빌드만 됐고, 사용자가 게임 중이라 GPU 실행은 보류다(`.gpulock/HOLD`). 게임 뒤 검증 목록은 3절이다.

## 1. 구현한 구조 (reflection.layers, 기본 false)

반사 값 하나를 셋으로 나눠 둔다: `값 = base + residual + albedo × stochastic`.

| 부분 | 내용 | 공간 필터 |
|---|---|---|
| base | M: hit의 방출·태양 항(직접 시야와 같은 추정기). G: 0 | 받지 않는다 (거울 상의 identity) |
| stochastic (L_rs) | hit의 확률 조명(캐시 조도·캐시 스펙큘러 + 국소광 표본 1개)을 hit의 방향 반사율(albedo)로 나눈 값 | M: hit 기하 안내(허상 깊이·hit 법선), G: 수신면 안내 + lobe 발자국 |
| residual (L_g) | G 작업의 `추정값 − albedo × stochastic` (M은 0) | 수신면 안내 + lobe 발자국(σ = blur_px / 2) |

- 값 분리: `RayTracing/HitShading.hlsli` `rtHitRadianceSplit`(반환값 식은 기존과 같다), `ReflectionShade.hlsli`, 광선 층 기록(슬롯당 16 B), 작업 층 기록(작업당 24 B, `ReflectionCombine`·인라인 경로가 같은 산술), `ReflectionResolve`가 화소 층(stochastic·residual RGBA16F, guide RGBA32UI)을 쓴다.
- 공간 재구성: `Passes/Reconstruct/LayerDenoise.hlsl` — à-trous 3단(5×5 B3, 간격 1·2·4 단위). 단위는 M 1 px, G는 max(표본 간격, blur_px / 14)(최대 8 px). 가중치는 기하뿐이다(값 가중 없음).
- 시간 보조: `LayerTemporal.hlsl` — 이력 최대 8프레임(`reflection.layer_history_frames`). M 화소는 거울 속 허상점(눈 → 거울 → hit 경로를 편 점)을 이전 뷰로 투영해 따라간다. G 화소는 수신면의 정확한 이전 위치 + lobe shift 규칙(기존 ReflectionAccumulate와 같다). 이력은 재구성된 층의 3×3 이웃 평균 ± 3σ(+5 %) 안으로 자른다.
- 합성: `LayerCompose.hlsl` — `값' = 값 + albedo × (stochastic' − stochastic) + (residual' − residual)`. 필터를 끄면 resolve 값과 같다.
- 캐시 데이터 없는 hit(컷 직후 hit 셀의 첫 갱신 전: 간접광 0): 그 화소의 stochastic은 필터에서 탭 가중 0, 중심일 때는 이웃 평균을 받는다(R과 합의).
- A/B: `reflection.layers = false`면 기존 경로(ReflectionAccumulate, 이력 32) 그대로다. `layer_filter`, `layer_history_frames`, `layer_view`(진단 1~7)가 따로 있다.
- L_occ: `GI/GiProbeGather.hlsl` — `gi.screen_occlusion_spatial`(기본 false)이면 프로브마다 주변 3×3 프로브의 이번 프레임 16점 추정(144점)을 평면·법선·값 가중으로 평균하고, 이력은 그 값을 누적한다.

### 설계와 다르게 한 것 (이유)

1. **hit instance를 안내 자료에서 뺐다.** 타일 한 장 한 장이 인스턴스인 바닥처럼 같은 평면의 이웃 인스턴스는 조명이 이어진다. instance로 끊으면 타일마다 필터가 멈춘다. 대신 허상 깊이(눈 → 거울 → hit 경로 길이)와 hit 법선으로 "거울 속 같은 면"을 가른다.
2. **D-1(반사 hit 국소광 LTC + VSM)은 넣지 않았다.** 화면 밖 hit의 국소광 그림자 페이지 요청·상주가 Shadow/*(R 소유, V2.5 L2~L6)에 걸린다. 층 구조는 D-1이 오면 stochastic에서 국소광 항만 빠지게 나눠 두었다.
3. **태양 그림자 광선 항(VSM이 hit를 갖지 않을 때)은 base에 두었다.** 반그림자 안에서만 0/1 잡음이 있고, 필터에 넣으면 거울 속 태양 그림자 경계가 흐려진다.
4. **L_g는 "추정값 − gbar"가 아니라 확률 몫을 뺀 추정값 전체다.** 차이만 필터하면, 제어변량(프로브 지도)이 lobe가 보지 않는 등을 보는 곳에서 잔차가 그 오차의 음수를 담는데 gbar는 화소별로 남아 절반만 상쇄된다(2절 둘째 판의 색 얼룩).
5. **분산 안내 값 가중(exp(−|ΔL|/kσ))을 뺐다.** 2절 참조. G 층은 lobe 발자국으로 대역 제한돼 있어 값 차이는 잡음이고, 이 잡음은 꼬리가 두껍다.
6. 층 σ(알파)를 쓰지 않는다. 이력 가중은 `1/(n+1)`이고 분산 비 가중(설계 1.2)은 넣지 않았다.

## 2. 측정 기록

모두 [실측], 1080p 출력(내부 1280×720), `bath_reference.unxscene` 정지 300프레임, `--auto-exposure`, `--capture-output` + `--capture-layers final,refl`, 사용자 게임 실행 중(GPU 시간은 판정용이 아님). 지표는 `Tools/Verify/motion_metrics.py`(타일 P95: 노출을 f299 평균 휘도에 맞춘 뒤, 반사 층 σ: view.reflection의 8×8 타일 고역 σ / 평균의 P95, 층 오차: f299 대비 타일 P95). 명령은 `judge_run.py`, 분석은 `judge_analyze.py`.

| 판 | 내용 | f3 타일 P95 / 반사 σ | f15 | f299 반사 σ | 실행 |
|---|---|---|---|---|---|
| A | 기존 경로 (`layers=false`), 미커밋 작업 트리(병합 5aac13f 위, 0663e47 직전) | 63.5 % / 145 % | 46.3 % / 43.7 % | 24.6 % | exit 0 |
| B 1판 | 층 켬, 탭 1·2·4 px, 잔차 = 추정값 − gbar, 값 가중 있음, 이력 범위 = 필터 σ | 54.3 % / 299 % | 45.6 % / 274 % | 255 % | exit 0, TDR 없음 |
| B 2판 | 1판 + G 탭을 표본 간격 단위로 | 57.8 % / 300 % | 45.2 % / 266 % | 241 % | exit 0, TDR 없음 |
| HEAD | 2판 + 3절의 수정 넷 | 미실행 | | | 빌드만 |

- B 1판은 R 브랜치 병합(af6f9fd, 노출 수정 포함) 전 빌드이고 A와 2판은 병합 뒤다. 타일 P95의 판 사이 비교에는 그 차이가 섞여 있다.
- 두 판 모두 반사 층 σ가 기존 경로보다 5~10배 나쁘다. 이력이 사실상 쌓이지 않은 수준이다(기존 경로 f3 = 누적 3프레임이 145 %).
- 확인된 원인(캡처·코드):
  - `evidence/pillar_refl_f0.png`(왼쪽부터 A f0, B 1판 f0, B 2판 f0, A f299): 기존 경로 f0(이력 없는 resolve)은 광택 기둥이 화소 단위 소금·후추다. 표본 대부분이 검고 드물게 아주 밝다(등 옆 핫스팟을 lobe 광선 4개 중 하나가 맞음). f0에는 hit 거리 이력이 없어 모든 G 화소가 s = 1 자기 표본이다(ReflectionClassify).
  - 같은 그림의 B 1·2판 f0: 밝은 표본 하나하나가 **모서리가 선 네모**로 남았다. 매끈한 B3 커널과 기하 가중으로는 평평한 면에 그런 경계가 생길 수 없고, 값 가중만이 만든다 → 값 가중 제거(ddacbf6).
  - `evidence/pillar_refl_f15.png`: B 2판 f15에 청록·붉은 색 얼룩 → 잔차 정의(1절 4번, 0663e47에 포함).
  - 코드: 이웃을 하나도 못 받은 화소는 필터 σ가 0이고(탭 1개의 표본 분산), 이력을 그 σ의 3배 + 5 % 안으로 잘랐다 → 그런 화소는 이력을 잃는다. 3×3 이웃 범위로 바꿨다(0663e47에 포함). **이것이 σ 255 %의 주원인인지는 미확인**이다(3절 1번).
  - 코드: G 탭 단위가 표본 간격뿐이라 f0(s = 1)에서 지지가 ±14 px였다 → blur_px까지 닿게(7e1de2e).
- DXIL [실측, HEAD 빌드]: ReflectionTraceInline.SKY0.JOB2.CORNERS1 199.9 KB(한도 200 KB = 204,800 B, 여유 약 4.9 KB), LayerDenoise 약 6 KB, LayerTemporal 29 KB, LayerCompose 3 KB.

## 3. 게임 뒤 GPU로 가릴 것 (순서대로, 조각당 잠금 1회 ≤ 10분)

1. 욕탕 정지 1080p B(HEAD, `layers=true`) 대 A: 반사 층 σ f3/f15/f299, 색 얼룩·네모가 없어졌는지. σ가 여전히 A보다 나쁘면 `layer_view=7`(이력 프레임 수)로 이력이 쌓이는지부터 본다.
2. 층 진단(`judge_run.py --modes diag --layers final,refl,reflmode`, `layer_view` 1·2·4): stochastic 원본/재구성, residual 재구성, 화소 모드. 2판 f3·f15의 8 px 블록이 resolve 입력인지 필터 출력인지 가린다(**미확인 가설이라 코드는 건드리지 않았다**).
3. 시험: `run_tests.ps1`(reflectionanalytic 켬/끔/이력 없음, planarmirror, gianalytic, hostmotion). 한 번도 못 돌렸다.
4. 판정 묶음: 목욕탕 라운지·욕탕·기차 라운지 × 정지·회전·컷 × 1080p·1440p, 반사 많은 시점(bath_mirror, bath_showers, train_window: `--camera-at` 좌표계가 Unity와 같은지 첫 캡처로 확인).
5. timing A/B/A/B(`timing_run.py`, 1440p·1080p): 층 켬의 r.refl.* 증가분(설계 [예상] +0.25 ms @960p). 기본값을 켜기 전에 필요하다.
6. L_occ: `gi.screen_occlusion_spatial` 켬/끔 컷 직후 gi 층(차폐가 곱해진 값) f0·f3 모자이크, 접촉부 크롭, r.gi.probes 시간.
7. 결함 큐 5(25471c1): 욕탕 샤워 거울 시점에서 거울 평면이 후보·카메라를 받는지(`ReflectionSystem::readStats` planarViews, 화면).
8. 10.5 A/B(32·공간 없음 대 8·공간): 1·4가 통과한 뒤 정지 수렴 σ와 회전 중 프레임별 σ.

미확인 가설 (코드로 고치지 않음):
- B 2판 f3·f15 반사 층의 8 px 블록의 출처(2번).
- σ 255 %의 주원인이 이력 절단인지(1번).
- G lobe가 등 옆 핫스팟을 드물게 맞는 두꺼운 꼬리 자체는 공간 평균으로만 줄인다. 원천에서 줄이려면 hit 조명을 광선 원뿔 발자국으로 평균하거나(R의 발자국 실험은 에너지 손실로 기각) R의 hit 셀 누적기 값을 G 광선이 읽는 방법이 있다 — 설계·R과 정할 일이다.
