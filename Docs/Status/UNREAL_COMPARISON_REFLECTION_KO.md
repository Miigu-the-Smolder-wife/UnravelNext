# 반사: Unreal(Lumen Reflections)과 UnravelNext의 차이

2026-10-01, S2. 사용자 지시("언리얼은 이런 문제가 없던데, 무슨 차이야?")에 대한 답이다. 읽은 소스는 이 PC의 `C:\Users\USER\RendererResearch\UnrealEngine-ue6-main`(ue6-main, 2026-09-09)이고 코드는 복사하지 않았다. 아래 "Unreal" 열은 모두 [소스 확인]이다(파일은 표 아래). "우리" 열의 수치는 [실측](욕탕 홀 정지, 출력 1080p / 내부 720p, `Results/Local/Refl/README_KO.md` 2b)이거나 [코드]다.

## 1. 한 줄 답

Unreal의 반사가 조용한 이유는 필터가 더 좋아서가 아니라, **잡음이 큰 값을 처음부터 만들지 않고(hit의 빛을 이미 누적된 표면 캐시에서 읽음, 거친 면에는 반사 광선을 쏘지 않음), 남은 튀는 값은 에너지를 버리면서 눌러 버리기** 때문이다(광선 세기 상한, 톤맵 공간 평균, lobe 꼬리 절단). 우리는 hit마다 국소광 하나를 확률로 뽑고 갓 만든 캐시 셀을 읽어 꼬리가 두꺼운 값을 만든 뒤, 에너지를 지키는 필터로 그것을 줄이려 한다. 게다가 우리 층 분해에는 **어떤 필터도 거치지 않는 항(hit 알베도)** 이 있는데, Unreal에는 그런 항이 없다(광선의 값 전체를 필터한다).

## 2. 차이 표

| | Unreal (Lumen Reflections) | 우리 (UnravelNext, redesign-v2-refl) | 영향 |
|---|---|---|---|
| 반사 광선을 쏘는 범위 | roughness ≤ 0.4까지. 0.3~0.4는 광선 결과와 "거친 스페큘러"를 선형으로 섞는다(α = (0.4 − r) / 0.1). 그 위는 광선 없음. 시선 각도와 무관 | M: r < 0.14. G: lobe 반각 2·atan(√3·α)·NoV < 22°. 정면에서는 r < 0.335지만 **스침각에서는 NoV가 곱해져 r 0.5(NoV 0.5), r 0.9(NoV 0.2)까지 G로 광선을 쏜다** [코드: `reflectionLobeHalfAngle`] | 욕탕 홀 천장은 46~56 %가 G다 [실측]. Unreal이면 광선을 쏘지 않을 면에 우리는 lobe 광선 4개를 쏜다 |
| 거친 면의 스페큘러 | GI 최종 수집(화면 프로브)에서 화소마다 lobe로 적분한 값. 자체 시간 필터가 있다 | K 경로(캐시 lobe)가 같은 역할 | 구조는 같다. 경계의 위치와 혼합 구간이 다르다(우리는 혼합 없이 화소 단위로 K/G가 갈린다) |
| 화소당 광선 | 1개(또는 2×1, 2×2에 1개 + 타일 지터). 가시 법선 GGX 표본, 청색 잡음 | M 1개, G 4개(간격 s ≤ 8 px 격자에 놓고 사이는 보간) | 광선 수는 우리가 같거나 많다 |
| **hit의 빛** | 기본은 **표면 캐시**(카드에 직접광 + 간접광이 이미 여러 프레임 누적된 값)를 읽는다. hit에서 조명을 확률로 뽑지 않는다. 선택 모드(hit lighting)에서도 간접광은 표면 캐시 | 캐시 셀의 간접광 + **국소광 1개를 확률로 뽑은 표본**. 컷 직후에는 셀이 갓 만들어져 값이 튄다 | 우리 f0 화소값의 표준편차/평균: 기둥 2.9, 뒷벽 5.4, 벽 4.0 [실측, 옛 장면]. 에너지 절반이 상위 1.5~5 % 화소에 있다. 이것이 잡음의 원천이다 |
| "데이터 없음" hit | 없다. 표면 캐시는 항상 값을 준다 | 엄격 읽기에서 보이는 모서리가 없으면 "데이터 없음", 필터가 이웃 평균으로 채운다 | f0에 천장·기둥이 수렴값의 5배 [실측] (원인 확정은 진단 조각 뒤) |
| 튀는 값 처리 | (1) 광선 세기 상한 40. (2) 평균을 전부 톤맵 공간 L / (1 + L / 10)에서 낸다. (3) GGX 표본의 꼬리 10 %를 잘라 낸다. (4) 이력이 없는 화소는 L / (1 + L)로 더 세게 누른다. **넷 다 에너지를 버린다** | 없음(에너지 보존). 값 가중도 뺐다 | Unreal은 밝은 반사 하이라이트가 어두워지는 대신 반짝임이 없다 |
| 공간 재구성(resolve) | 반경 8 px × min(8r, 1) 원판에서 이웃 광선 5개를 가져와, **이웃 광선의 hit 지점을 내 화소에서 본 방향으로 다시 계산**하고 내 lobe의 D / 그 광선의 pdf로 가중(비율 추정). hit 거리는 내 것으로 자른다. r ≤ 0.05는 재구성하지 않는다 | 5×5 B3 à-trous 3단 + 조밀 단, 기하 가중(평면·법선·roughness·hit 거리)과 lobe 발자국 가우시안 | 우리 쪽 지지가 훨씬 넓다(유효 표본 274 대 6). 가중의 기준이 다르다: Unreal은 BRDF, 우리는 기하 |
| **필터가 다루는 양** | 광선의 **값 전체**(hit 알베도·방출·직접광 포함). 분해 없음 | `base + residual + albedo × stochastic` 3층. stochastic은 hit 알베도로 나눈 뒤 필터하고 **그 프레임의 hit 알베도를 필터 없이 다시 곱한다** | 우리에게만 있는 항. 3절 |
| 시간 누적 | 최대 12프레임(거울은 2). 이력 두 개(반사상 재투영, 표면 재투영) 중 현재 이웃에 가까운 쪽 | 층 이력 8프레임(기존 경로는 32). M은 반사상, G는 표면 | 길이는 비슷하다 |
| 이력 범위 제한 | **공간 필터 전**(resolve 직후) 값의 5×5(모서리 뺀 21화소) 평균 ± 1σ, YCoCg·톤맵 공간. 범위를 벗어난 만큼 누적 프레임 수를 깎는다 | **공간 필터 뒤** 값의 3×3 평균 ± 3σ + 5 % | 우리 범위는 필터 뒤라 ±5 %로 좁다. 다만 범위를 꺼도 수렴 σ는 그대로였다 [실측: C 35 %, CN 35 %] — 지금 남은 잡음의 원인은 아니다 |
| 필터 순서 | resolve(공간) → 시간 → **시간 분산으로 조절하는 양방향 필터** | 공간 4패스 → 시간 → 합성 | Unreal은 시간 2차 모멘트를 쌓아, σ/평균 > 0.5인 화소만 뒤 필터를 건다 |
| 이력이 없는 첫 프레임 | 뒤 필터를 키운다: 표본 2배, 법선 허용각 4배, 휘도 가중 끔, 톤맵으로 누름. 2프레임 동안 | 특별 처리 없음. G는 f0에 모두 s = 1 | Unreal의 첫 프레임은 "흐리고 어둡지만 조용"하다 |

읽은 파일: `Engine/Shaders/Private/Lumen/` — `LumenReflections.usf`(광선 생성), `LumenReflectionResolve.usf`, `LumenReflectionDenoiserTemporal.usf`, `LumenReflectionDenoiserSpatial.usf`, `LumenReflectionDenoiserCommon.ush`, `LumenReflectionsCombine.ush`, `LumenReflectionTracing.usf`·`LumenReflectionHardwareRayTracing.usf`(세기 상한), `LumenHardwareRayTracingCommon.ush`(hit의 빛), `DiffuseIndirectComposite.usf`(혼합). `Engine/Source/Runtime/Renderer/Private/Lumen/LumenReflections.cpp`(기본값). roughness 한계 기본값 0.4는 5.8.2의 `Scene.cpp`.

## 3. 조정 세션의 질문에 대한 답

1. **거친 면.** Unreal은 r > 0.4에 광선을 쏘지 않고, 0.3~0.4는 섞는다. 우리 G 경계는 lobe 반각에 NoV를 곱해 정하므로 스침각으로 보는 거친 천장·벽이 G가 된다. 욕탕 홀 천장의 반사 층 잡음(f0 화소 표준편차/평균 1.04, G 56 %)은 "거친 면에 lobe 광선을 쏘기 때문"이 맞다. 다만 σ 35 %가 수렴 뒤에도 남는 것은 그것만으로 설명되지 않는다(2번).
2. **재구성.** Unreal은 값 전체를 BRDF 가중으로 재사용한다. 우리의 "필터를 안 받는 hit 알베도"에 해당하는 항은 Unreal에 없다. 우리 쪽 실측: 이력 한계를 끄면 f3 수준이 달라지는데(이력이 실제로 값을 바꾼다) σ는 35 %로 같다. 즉 남은 잡음은 필터된 층에도 이력에도 없다. 합성식 `value′ = value + albedo × (S′ − S) + (R′ − R)`에서 필터를 받지 않는 것은 albedo뿐이다. lobe가 넓은 화소에서 albedo는 그 프레임 광선 1~4개의 hit 알베도 평균이라 화소·프레임마다 흔들린다. **확정은 진단 조각(f3의 알베도 층 고역 σ)이 나온 뒤에 한다.**
3. **시간.** 길이(12 대 8)는 차이가 아니다. 차이는 (가) 범위를 공간 필터 전의 잡음 폭으로 잡는 것, (나) 범위를 벗어나면 값을 자르기만 하지 않고 누적 프레임 수를 줄이는 것, (다) 시간 분산을 쌓아 뒤 필터를 조절하는 것, (라) 이력이 없는 2프레임 동안 필터를 넓히는 것이다. f0 과밝음에 해당하는 Unreal 쪽 현상은 없다(표면 캐시는 항상 값이 있고, 첫 프레임은 톤맵으로 누른다).
4. **품질을 내주는 곳**은 5절.

## 4. 채택할 것 (품질·부하를 낮추지 않는 것만)

우선순위 순. 각 항목은 원인이 측정이나 코드로 확인된 뒤에만 구현한다.

| # | 채택 | 근거 | 상태 |
|---|---|---|---|
| 1 | **lobe 화소는 값 전체를 재구성한다.** blur_px ≥ 1인 화소(G 전부, 거친 M)는 hit 알베도로 나누지 않거나, 알베도도 lobe 발자국으로 재구성한다. 거울(blur_px < 1)만 지금의 분해를 유지한다(반사상의 identity 보존은 거울에서만 의미가 있다) | Unreal은 값 전체를 필터한다. 우리 실측: 필터·이력과 무관하게 σ 35 % | 진단 조각으로 알베도 항 확인 → 구현 |
| 2 | **hit의 빛을 확률 표본 대신 결정적인 값으로.** D-1(hit의 국소광 결정적 합, 승인됨)과 GI hit 누적기 읽기(배선됨, 인라인 G 커널 예외 남음)가 Unreal의 표면 캐시에 해당한다 | 잡음의 원천. f0 화소 표준편차/평균 2.9~5.4 | D-1은 R의 분류 페이지 뒤. 누적기만으로는 욕탕 홀 반사 층 σ가 거의 그대로였다(f0 652 → 637 %, f299 25 → 24 % [실측, 옛 장면]). 국소광 표본이 남아서인지는 D-1 뒤에 다시 잰다 |
| 3 | **G 경계에서 NoV 곱을 빼고, K와 G 사이에 혼합 구간을 둔다.** 거친 면을 스침각으로 볼 때 G가 되는 것을 막는다. 값은 K 경로(캐시 lobe)가 낸다 | Unreal은 r만으로 정하고 0.1 구간을 섞는다. 욕탕 홀 천장 G 46~56 % | **작업량이 바뀌므로 timing 1회와 A/B 필요.** K 경로가 그 면의 lobe를 정확히 내는지(reflectionanalytic)부터 확인한다. 광선을 줄이는 변경이므로 "품질을 낮춰 잡음을 줄이는" 것이 되지 않게, K 값이 G 수렴값과 같은지를 통과 조건으로 한다 |
| 4 | **이력 범위를 공간 필터 전의 값에서 잡고, 벗어난 만큼 누적 프레임 수를 줄인다.** | Unreal의 순서. 우리 범위(±5 %)는 근거가 약하다 | 1번 뒤. 지금은 범위가 f0 과밝음이 끌리는 것을 막아 주므로 켠 채 둔다 |
| 5 | **이력이 없는 화소의 공간 필터를 넓힌다**(2프레임). 톤맵 누름은 빼고 폭만 | f0에 G는 s = 1이고 지지가 lobe 발자국뿐 | 1·2번 뒤 f0·f3를 다시 재고 판단 |
| 6 | 이웃 광선을 BRDF로 다시 가중(법선 맵·roughness가 화소마다 다른 면) | 우리 기하 가중은 법선·roughness가 다르면 탭을 버리기만 한다 | 뒤로. 측정으로 필요가 보이면 |

## 5. Unreal이 품질을 내주는 곳 — 따라 하지 않는다 (사용자 결정 항목)

- 광선 세기 상한 40(`MaxRayIntensity`): 밝은 반사가 어두워진다.
- 톤맵 공간 평균(범위 10)과 첫 프레임의 L / (1 + L): 하이라이트 에너지가 줄어든다.
- GGX 표본 꼬리 10 % 절단(`GGXSamplingBias`): lobe가 좁아진다.
- roughness 0.4 위는 광선 없음: 거친 금속·젖은 거친 면의 반사 디테일이 GI 프로브 해상도로 떨어진다. (4절 3번은 한계를 낮추자는 것이 아니라, 스침각에서 한계가 r 0.9까지 올라가는 것을 정면과 같게 맞추자는 것이다.)
- hit의 빛이 표면 캐시: 반사 속 물체에 스페큘러가 없고 해상도가 낮다. 우리는 hit을 실제 재질로 셰이딩한다.

이 다섯은 "반짝임이 없다"와 맞바꾼 것이다. 우리 규칙(품질·표본·부하를 낮춰 성능이나 조용함을 만들지 않는다)에서는 쓰지 않는다. 쓰려면 사용자 결정이 필요하다.

## 6. 이식 상태 (2026-10-01 밤)

사용자 결정으로 비교에서 이식으로 바뀌었다: Lumen의 구조를 우리 코드로 다시 써서 우리 경로를 대체하고, 조각별 품질 비교 없이 출하 기본 경로를 끝까지 조립한 뒤에 한꺼번에 판정한다.

| 구성 요소 | 상태 | 코드 |
|---|---|---|
| 거칠기 한계(0.4)와 혼합 구간(0.1), 시선각과 무관 | 구현 | `ReflectionClassify`, `ShadeOpaque` |
| 화소당 광선 1개(GGX 가시 법선 표본) | 구현(우리 M 작업 재사용) | `ReflectionTrace` |
| 이웃 광선 BRDF 재사용 resolve(5표본, 반경 8 px) | 구현 | `ReflectionReuseResolve` |
| 시간 누적(최대 12, 반사상·표면 두 이력, 이웃 범위 ±1σ, 신뢰도로 프레임 수 감소, 2차 모멘트) | 구현 | `ReflectionReuseTemporal` |
| 시간 분산으로 조절하는 양방향 필터, 이력 없는 2프레임 확대 | 구현 | `ReflectionReuseFilter` |
| 거친 면의 스페큘러 = GI 최종 수집 재사용 | 지금의 K 경로(화면 프로브)로 연결. R의 최종 수집 재구성이 나오면 그 버퍼로 바꾼다 | `ShadeOpaque` |
| **표면 캐시**: 카메라 주변 표면의 조명을 월드 공간에 상주(캡처, 직접광 타일당 8광원·예산 1/32, radiosity 프로브 4×4·예산 1/64·누적 4) | 구현. 카드 대신 월드 셀 해시. 규칙·다른 점은 `SURFACE_CACHE_INTERFACE_KO.md` | `Passes/SurfaceCache/*` |
| 반사 hit이 표면 캐시를 읽음 | 구현(`reflection.lumen_hit_surface_cache`) | `ReflectionShade` |
| GI hit이 표면 캐시를 읽음 | 함수·버퍼 접근자 제공. 호출은 R | `SURFACE_CACHE_INTERFACE_KO.md` |
| 평면 거울·고요한 물 | 우리 래스터 유지 | |
| 화면 공간 추적(HZB 50회, 두께 0.005, 이전 프레임 색) | 구현(`reflection.lumen_screen_traces`). 차이: 이전 깊이 검사·움직이는 물체의 속도 없음 | `ScreenTrace.hlsli`, `ReflectionScreenTrace`, `SCREEN_TRACE_INTERFACE_KO.md` |
| 화면 추적이 놓친 광선은 그 끝점에서 월드 광선을 잇는다(pull-back 8 cm) | 구현(`reflection.lumen_screen_trace_continue`) | `ReflectionScreenTrace`, `ReflectionTrace` |
| 월드 hit이 화면에 보이는 면이면 이전 프레임 색을 읽는다(깊이 1 %, 법선 85°) | 구현(`reflection.lumen_sample_scene_color_at_hit`). 차이: 법선 검사는 hit 화소의 셰이딩 법선, 이전 깊이 검사 없음 | `ReflectionSceneColorAtHit` |
| GGX 표본 꼬리 10 % 절단 | 구현(`reflection.lumen_ggx_sampling_bias`, QUALITY TRADE) | `ReflectionRay.hlsli`, `ReflectionReuse.hlsli` |
| 2×1·2×2 다운샘플 추적 + 타일 지터 | **넣지 않음.** 언리얼 출하 기본값은 `DownsampleFactor 1`(전체 해상도)이고 2는 품질 단계를 낮출 때만 쓴다. 부하를 낮추는 옵션이라 사용자 결정 없이는 만들지 않는다 | |
| 전경 반투명·물 패스, 반사 전용 radiance cache, far field | 미구현 | |

스위치: `reflection.lumen`(새 반사 경로), `surface_cache.enabled`(표면 캐시), `reflection.lumen_hit_surface_cache`(반사 hit이 캐시를 읽고 표시, 기본 true). 셋 다 켜야 조립된 상태다. 매개변수는 `reflection.lumen_*`(reflection.toml)와 `surface_cache.*`(surface_cache.toml), 기본값은 언리얼 것.

실행 확인(품질 판정 아님): 로비 1080p, 300프레임, 장치 제거 없음, S 오류 비트 0, 화면 정상(평균 휘도 0.41~0.51, NaN 0), 3프레임째부터 반사 hit의 98~99 %가 조명 받은 셀을 읽음. reflectionanalytic은 `reflection.lumen=true`에서 PASS(7줄).

**사용자 결정 항목**(품질을 내주는 값. 언리얼 기본값으로 켜 두었고 0 / false로 바꿀 수 있다. toml에 QUALITY TRADE로 표시):
- `reflection.lumen_max_roughness_to_trace = 0.4` — 그 위는 광선 없음.
- `reflection.lumen_max_ray_intensity = 40` — 반사 광선 세기 상한(노출 적용 단위).
- `reflection.lumen_tonemap_range = 10` — 평균을 톤맵 공간에서 낸다(밝은 하이라이트가 어두워진다).
- `reflection.lumen_disocclusion_tonemap = true` — 이력 없는 화소의 표본을 1 / (1 + 휘도)로 누른다.
- `surface_cache.radiosity_max_ray_intensity = 40` — radiosity 광선 세기 상한.
- `surface_cache.remainder_light = false` — 셀당 가장 센 8개 밖의 광원은 버린다(true: 나머지에서 1개를 더 뽑아 에너지를 지킨다).
- `surface_cache.entries_log2 = 22` — 저장 칸 수(255 MB). 언리얼 아틀라스는 2^24 텍셀. 로비는 2^21에서 가득 찼다(91 %).
- `reflection.lumen_ggx_sampling_bias = 0.1` — GGX 표본 꼬리 10 % 절단(lobe가 조금 좁아진다). 0이면 lobe 전체.
- (만들지 않음) 다운샘플 추적 2×1·2×2 — 언리얼에서도 기본이 아니다. 성능이 모자랄 때의 선택지로만 적어 둔다.

## 7. 판정 항목 1 — gi.lumen이 표면 캐시를 읽을 때의 간접광 수준 (2026-10-02 새벽)

증상(조정 세션): 로비에서 gi.lumen + 표면 캐시를 같이 켜면 간접광이 낮고 네모 조각과 푸른 기가 보인다.

### 7.1 소스로 확인한 것

| 질문 | 언리얼(ue6-main) | 우리 |
|---|---|---|
| MegaLights가 켜지면 표면 캐시 직접광이 확률판으로 바뀌는가 | 아니다. `r.LumenScene.DirectLighting.Stochastic` 기본 0, MegaLights 분기 없음(`LumenSceneDirectLightingStochastic.inl` 3-5, 57-60). 기본은 타일당 가장 센 8개, 나머지는 버림(`LumenSceneDirectLighting.cpp` 89-91, `LumenSceneDirectLightingCulling.usf` BuildLightTilesCS) | 같다(`remainder_light=false`) |
| 기본 경로의 직접광 계산 | 결정적. 조도 = 광원의 해석적 적분(`DeferredLightingCommon.ush` 518 GetIrradianceForLight), 그림자 = 광원 위치로 쏜 광선 1개(`LumenSceneDirectLightingHardwareRayTracing.usf` 154-180). 광원 위 무작위 점은 확률판에서만 | 무작위 점 1개 + GI 마스크 그림자 광선이었다 → **`surface_cache.direct_analytic`(기본 true)로 고침** |
| hit이 읽는 값 | FinalLighting = (직접 + 간접) × 알베도 / π + 방출(`LumenSurfaceCache.ush` 50-55) | 같다(`scFinalLighting`; R의 hit은 직접 + 간접을 조도로 받아 실제 재질로 셰이딩) |
| hit에서의 보간 | 카드 최대 3장(법선 축 가중) × 텍셀 4개 이중선형 × 깊이 가시성(`LumenSurfaceCacheSampling.ush` 235-311) | 셀 하나(최근접). **차이로 남아 있다** |
| 방출면 | 카드 캡처의 EmissiveAtlas → FinalLighting → radiosity 광선이 읽어 퍼짐. 광선 세기 상한 40 × 1/PreExposure(`LumenRadiosity.usf` 188-190) | 같다(셀의 방출, 상한 40) |

### 7.2 실측

내 동결 로비 장면(`bt_lobby_20261001_1802`), 1080p, gi.lumen + reflection.lumen, 각 1회 [실측]. GI 층 평균에 2^ev100을 곱해 노출을 뺀 값, f299, 표면 캐시 없음 = 1.00.

| 설정 | GI 층 수준 |
|---|---|
| 표면 캐시 없음 | 1.00 |
| 표면 캐시, 이전 직접광(무작위 점) | 0.21 |
| + `remainder_light=true` | 0.22 |
| + `direct_stochastic=true` | 0.30 |
| 표면 캐시, 해석적 직접광(새 기본) | 0.30 |
| 해석적 + `remainder_light=true` | 0.30 |
| 해석적, `radiosity=false` | 0.19 |
| `direct_lighting=false` | 0.12 |
| 해석적 + `radiosity_max_ray_intensity=0` | 0.46 |
| 전부 켬(MegaLights + radiance cache + AO, 이전 직접광) | 0.40 |

- 8개 밖 광원은 원인이 아니다(remainder가 수준을 바꾸지 않는다).
- 해석적 = 확률판(편향 없는 추정)의 수준이므로, 이전의 무작위 점 방식이 직접광을 낮게 냈다.
- 1800프레임(`judge_run.py --modes long`): 해석적 0.33 → 0.35 → 0.37(f299 / f899 / f1799, 같은 실행의 캐시 없음 = 1.00). 수렴 지연이 아니다.

반사 hit에서의 성분(`reflection.lumen_surface_cache_view_component`, E / π, nits, f299, `sc_components.py`):

| 성분 | 값 |
|---|---|
| 셀 직접광 | 5.5 |
| 셀 간접광(상한 40) | 4.5 |
| 셀 간접광(상한 끔) | 8.3 |
| 같은 hit의 월드 GI 캐시 조도 | 19.3 |
| 같은 hit의 국소광 표본(기존 hit 셰이딩) | 2.2 |

화이트 퍼니스(`furnace_sc.ps1`, 닫힌 방 Le = 1, ρ = 0.5, 기대 L = 2): 표면 캐시를 켠 반사 hit 값 / 기대값 = 0.9995, radiosity를 끄면 0.4785(방출만: 0.5 근처), 표면 캐시 없음 0.9984. **radiosity의 다중 반사는 에너지를 보존한다.**

### 7.3 결론과 남은 것

1. 고친 것: 직접광을 언리얼 기본 구조로(d4d97e3). 수준 0.21 → 0.30, 계단·카운터의 얼룩이 줄었다.
2. radiosity 광선 세기 상한 40(언리얼 기본값, QUALITY TRADE)이 이 장면에서 간접광의 절반 가까이를 자른다. 로비는 ev100 3.5라 상한이 약 540 nits이고 발광면이 그보다 밝다. 사용자 결정 항목.
3. 상한을 꺼도 hit의 간접광은 8.3 대 19.3이다. 퍼니스에서는 두 경로가 같은 값을 내므로 차이는 광원·방출면이 있는 이 장면에서 기존 GI 캐시가 무엇을 더 담는가의 문제다. **어느 쪽이 맞는지 미확인**: 기준 경로 추적기(`unx_reference`)는 이 장면을 읽지 못한다(재질의 차폐 텍스처 정의가 없다는 오류).
4. 셀 보간(7.1 넷째 줄)은 아직 최근접이다.
