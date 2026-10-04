# 에디터 크래시 수정과 최적화 분석 — 2026-10-03

## 실제 크래시와 수정

문제가 난 프로젝트는 `C:/Users/USER/UnravelGames/BathhouseTycoon`이다. 16:43:49 KST의 Unity crash dump와 그 프로젝트의 `Logs/Editor.log`에는 다음 종료 경로가 기록돼 있다.

1. Unity graphics-device shutdown이 장치에 연결된 `HostRenderer`를 먼저 삭제한다.
2. 뒤에 실행되는 `UnravelNextDataWorld.OnDisable → DetachVfx → nv_stream_detach`가 GPU의 마지막 상태를 CPU로 가져오려고 한다.
3. `NV_StreamExecutor.user`에 남은 `HostRenderer*`를 체크포인트 콜백이 역참조한다. 이미 삭제된 객체다.

네이티브 콜백의 user 값을 재사용하지 않는 renderer handle로 바꿨다. 호출마다 등록된 소유자를 강한 참조로 얻고, 소유자가 없어졌으면 결과 포인터·개수를 비운 뒤 `NV_INTERNAL`을 반ㅇ환한다. submit은 NativeVfx의 기존 commit 계약대로 성공을 반환하고 후속 readback이 종료를 보고한다.

체크포인트·orientation·event 배열은 소유자의 잠금 안에서 복사하고, renderer/stream별 CPU snapshot에 보존한다. 다른 renderer의 콜백이 이전 배열을 덮어쓰지 않는다. NativeVfx의 마지막 retained view가 StreamLink를 놓아 detach를 호출하면 snapshot을 해제한다. snapshot은 GPU 장치를 소유하지 않는다. 이미 graphics shutdown이 퇴역시킨 정상 handle에 대한 managed Dispose는 성공하는 no-op이다. 발급하지 않은 handle은 여전히 거부한다.

원본 프로젝트와 실제 게임의 관리 코드에는 다음도 적용했다.

- `NativeDataWorldHost`는 application/editor quitting 이후 GPU 제공자의 제거를 새 물리 GPU admission으로 처리하지 않는다.
- `UnravelNextDataWorld`는 `OnApplicationQuit`와 `EditorApplication.quitting`에서 장치가 살아 있을 때 VFX를 CPU로 분리한다. 실제 `EditorApplication.Exit`에서는 전자만으로 충분하지 않았다.

수정된 DLL SHA256은 `23092AE01764E23B45C491A2DFEFA7B96D2852A8B7E49A3CB2EAD767C5787775`이다. 네이티브 build 출력, 원본 Unity 설치물, 게임 Unity 설치물이 같다. 821개 커널과 17개 quality 파일도 같은 build의 것으로 배포했다. GPU ABI는 그대로다.

`Unravel/Artifacts/IntegratedEngine/CurrentSource-20261003T065905831517Z/source-products.json`을 현재 소스에 대해 다시 검증하고, 이 checkout에서 만든 NativeWorld/Vfx/Animation/Physics/TitanNative 5모듈을 설치했다. 실제 게임 프로세스가 로드한 6모듈의 경로·해시는 게임 `Artifacts/EditorShutdown20261003/loaded-native-products.txt`에 기록했다. 다른 checkout의 DLL로 빌드를 대체하지 않았다.

## 완료한 검증과 범위

- `unx_test_host_hostvfxlifetime`: 실제 DLL export를 통해 scene commit, 살아 있는 VFX submit/readback/checkpoint/orientation, 소유자 삭제 후 모든 콜백, CPU 배열 수명, 다른 renderer의 snapshot 독립성, 재사용되지 않는 handle, 중복 Dispose를 검사했다. 최종 실행 PASS (`Logs/Crash20261003/host-vfx-lifetime-final.log`).
- 실제 `BathhouseTycoon`의 saved scene을 D3D12로 실행했다. World와 외부 VFX executor가 활성인 상태에서 `EditorApplication.Exit`로 종료했다. 최종 native 수정본의 run 4와 추가 profile run 6은 모두 종료 0, native crash·VFX/물리 종료 exception·unknown renderer 경고 0이다 (`bath-editor-shutdown4.log`, `bath-editor-shutdown6-profile.log`). run 6은 1080p native 완료 프레임 120개를 기록한 뒤 정상 종료했다. probe의 `READY` 문구만으로 통과를 판단하지 않았다.
- 원본/게임의 관리 코드와 실제 DLL 연결은 Unity 자체 컴파일·플레이를 통과했다. 네이티브 수명 검사를 전체 게임의 모든 기능·화질·성능 인수로 확대하지 않는다.

saved scene에는 `shop_counter`, `vending_machine` stock 참조가 빠져 있다. 이 두 기존 콘텐츠 오류는 별도 파일에 기록하고 크래시 종료 경로를 계속 검사했다. assets나 GUID를 재생성하지 않았다. 초기 surface-cache feedback의 일부 probe 실패와 오래된 게임 bridge의 material-v1 경고도 로그에 보존한다. 이 보고서는 그런 콘텐츠/화질 문제까지 통과했다고 주장하지 않는다.

## 현재 소스의 GPU A/B

새로 링크한 `unx_gate_shadow_renderergate`로 실제 Train lounge export를 실행했다. RTX 4080, 4K 출력/1920×1080 내부, 기존 epic 품질, 600 warmup + 600 측정 프레임, 자동 노출, `gi.deterministic=true`, GPU lock. 4회 모두 종료 0, 다른 작업에 의한 GPU contention 0이다. 원시 JSON/CSV/PFM은 `Logs/Crash20261003/PerfCurrent`에 있다.

baseline/software 실행은 비교용 이미지를 얻기 위해 **매 프레임 GPU readback**을 넣는다. 그 패스 `s.gate.capture`만 중앙값 2.62 ms다. 아래 renderer scope 합은 각 프레임에서 그 패스를 제외한 뒤 중앙값을 계산했다. readback 없는 실제 프레임 시간을 재측정했다고 표기하지 않는다. 기존 readback 없는 600프레임 측정 17.42 ms와도 같은 비용 구조다.

| 실행 | 측정 GPU frame, readback 포함 | readback 제외 scope 합 | coverage hardware raster | 별도 coverage compute |
|---|---:|---:|---:|---:|
| 기본 | 19.961 ms | 17.328 ms | 2.818 ms | 0 |
| 기존 small-triangle compute 켬 | 20.380 ms | 17.735 ms | 2.578 ms | 0.633 ms |

compute는 126,264개 삼각형과 236,579개 fragment를 처리했고 hardware는 162,309개 삼각형을 처리했다. 기본은 288,573개 삼각형을 hardware로 처리한다. 두 경로가 저장한 총 fragment는 890,778개로 같다. geometry를 빼서 얻은 속도가 아니다. hardware 시간 0.24 ms 감소보다 별도 compute 비용 0.63 ms가 커서 총 scope 합은 **0.407 ms 증가**했다. 스위치는 기본 off를 유지한다.

두 final HDR 이미지는 bit-identical하지 않았다. 결정적 GI 스위치가 전체 파이프라인의 bit 재현성을 보장하지도 않으므로, 이 이미지 차이를 모두 래스터 오차라고 단정하지 않는다. compute 경로의 화질 인수나 속도 개선을 주장하지 않는다.

픽셀 처리 분해 진단도 실행했다. STAGE 3(픽셀 커널 즉시 종료)의 coverage raster는 **1.982 ms**, STAGE 4(면적·깊이·band-A 검사까지)는 **2.111 ms**다. 이 진단은 fragment를 저장하지 않고 depth bucket도 끈다. 동일 진단 조건끼리의 차이는 0.129 ms다. 정상 화면과 같은 부하/기능을 유지한 성능 결과가 아니며, 정상 2.818 ms를 단순히 단계별 비용으로 정확히 분해한 값도 아니다. 그래도 픽셀 셰이딩을 거의 제거한 뒤에도 큰 mesh/primitive/raster 비용이 남는다는 증거다.

## 몇 배 개선에 필요한 절감량

현재 Train baseline CSV의 분류별 중앙값이다. 동일 이름이 여러 번 나타나는 패스는 **프레임별로 먼저 합산**했다. 분류별 중앙값의 합은 전체 중앙값과 같지 않을 수 있다.

| 분류 | ms | 포함 범위 |
|---|---:|---|
| coverage 전체 | 5.062 | V의 목록/래스터, M의 fragment 셰이딩/합성, fragment 태양·국소 그림자와 광원 |
| GI + surface cache | 3.424 | Lumen gather/RC/LTV, 카드 direct/radiosity/feedback |
| TSR | 2.559 | dilate/decimate/thin/flicker/reject/AA/update/upscale |
| lighting + froxels | 2.018 | opaque MegaLights/셰이딩 및 공기 광량 목록 |
| VSM + 일반 receiver 그림자 | 1.111 | 페이지 유지/컬/래스터·일반 표면 shadow |
| reflection | 1.122 | screen trace, HW ray/hit lighting, temporal/reconstruction |
| 나머지 visibility | 0.611 | opaque 컬·래스터·HiZ |
| AS | 0.118 | static/dynamic TLAS 관련 |

각 프레임의 해당 분류를 실제 배율로 나누어 다시 합산한 **산술 가정**은 다음과 같다. 구현하거나 측정한 개선율이 아니다. 표본 수·해상도·coverage 품질을 줄이는 가정은 없다.

| 가정 | 예상 scope 합 | 전체 배율 |
|---|---:|---:|
| coverage만 4배 | 13.482 ms | **1.285배** |
| coverage를 비용 0으로 제거하는 불가능한 상한 | 12.200 ms | **1.420배** |
| coverage 4배 + GI/TSR/lighting/reflection/VSM 각각 2배 | 8.167 ms | **2.122배** |
| coverage 4배 + 위 다섯 분류 각각 3배 | 6.396 ms | **2.709배** |

4K 6.06 ms 목표에는 현재 renderer scope 합 대비 약 **2.86배**가 필요하다. coverage 하나만 고쳐서는 도달할 수 없다. 작은 패스 하나의 몇 배 개선과 전체 프레임의 몇 배 개선을 구별해야 한다. 원시 프레임별 계산은 `Logs/Crash20261003/optimization-scenarios.json`에 보존했다.

## 구현 우선순위와 근거

### 1. coverage의 triangle/primitive 처리 구조

현재 small-triangle compute와 hardware mesh shader는 각각 전체 cluster 정점을 변형·투영한 뒤 같은 triangle primitive를 만든다. hardware는 그 뒤에야 compute가 맡은 triangle을 건너뛴다. 별도 compute가 vertex/primitive 준비의 중복 비용을 없애지 않는 것이 소스와 A/B에서 확인됐다.

우선 후보는 한 번의 정점 처리에서 small triangle을 UAV로 쓰고 나머지를 hardware로 내보내는 결합 경로다. 또 현재 primitive마다 vis/material/flags와 네 clip vertex, 네 UV, 네 packed normal 등 약 124 B의 payload를 전달한다. 일반 opaque triangle과 near-clipped/alpha-tested triangle을 분리해 필요 없는 네 번째 vertex·alpha UV payload를 제거하는 변형이 후보다. float 정밀도와 실제 coverage/alpha 규칙을 유지해야 한다. 새 중간 버퍼를 무조건 추가하는 것은 vertex traffic 증가 때문에 반대로 느릴 수 있다.

M의 light/heavy coverage는 DXIL 한도를 피하려고 PART 1/2로 나눠서 **같은 정렬·mask union·가중치·표면 복원을 두 번** 진행한다. 단순히 다시 합치면 한도를 넘는다. 순서/가중치/유효 fragment를 한 번 계산하고 필요한 fragment만 direct/indirect 작업 목록으로 넘기는 구조를 검토할 가치가 있다. 임시 레코드 트래픽과 실제 GPU 점유율을 함께 비교해야 한다. 이 경로 전체 4배는 목표 가정이며 달성 증거가 아니다.

### 2. TSR의 반복된 neighbourhood 변환과 halo 작업

4K 내부 1080p에서도 reject 0.920 ms, flicker 0.465 ms, thin 약 0.286 ms가 든다. reject는 16×16 출력을 위해 30×30 shared region을, flicker는 34×34 region을 처리한다. 입력 픽셀 수 대비 각각 3.52배/4.52배의 halo를 여러 단계에서 다시 다룬다. reject에는 일곱 shared colour 배열과 다수의 group barrier가 있다.

reject의 초기 두 단계는 RGB의 min/max/clamp다. 저장된 11/11/10-bit sqrt code가 엄격히 단조이므로, 이 단계는 매 이웃마다 unpack→square→clamp→sqrt→pack하는 대신 component code의 min/max/clamp로 동일한 quantized 결과를 만들 수 있는 후보다. NumPy float32로 2048개/1024개 code 전부의 round-trip과 단조성을 확인했다. **CPU 산술 증거일 뿐, 새 GPU 커널의 인수가 아니다.** GPU 결과 비교와 CUT/얇은 기하/유리·물 motion/history resize 검증이 필요하다.

halo 중복은 separable min/max 또는 wave 내 이웃 공유로 줄이는 후보가 있다. precision을 낮추거나 rejection/anti-flicker 자체를 빼는 것을 성과로 계산하지 않는다.

### 3. GI·카드·reflection의 종속 조회와 material 작업 분리

GI/cache 3.42 ms와 reflection 1.12 ms를 별도로 줄여야 한다. `Lumen/LgTrace.hlsl`의 카드 hit은 direct/radiosity를 읽고 재질을 평가한다. 카드가 없는 hit은 태양·국소광의 추가 shadow ray와 RC/LTV 조회를 수행한다. monolithic hit shader 안에 다양한 재질·그림자·간접광 조회가 모이면 live register와 종속 메모리 접근이 커진다.

hit geometry 정보를 얇게 출력한 뒤 material/card/fallback 종류별 coherent 작업 목록에서 셰이딩하는 것이 후보다. 같은 card/같은 footprint를 읽는 lane 사이의 조회 공유도 검토 대상이다. ray 수나 fallback lighting을 없애는 방식은 제외한다. 추가 queue/write/read와 dispatch 비용 때문에 무조건 빠르다고 할 수 없다. 먼저 card 없는 hit 비율, shader register/occupancy, cache miss를 확인해야 한다.

정적 카드의 변하지 않은 direct 입력을 version/dependency 기준으로 재사용하는 것도 후보다. 태양·광원 함수·채널·동적 occluder·재질/SSS/eye 변경을 포함해 invalidation을 보존해야 한다. 이미 존재하는 예산/feedback 경로를 별도의 새 구현으로 계산하지 않는다.

### 4. light/froxel·graph 비용

정지 화면에서도 lighting/froxel 2.02 ms와 일반 VSM/shadow 1.11 ms가 남는다. light 종류/재질별 coherent 셰이딩과 일치하는 lookup의 공유가 후보이며, 조명·광원별 배율·채널·shadow 품질은 유지해야 한다.

현재 graph는 live 292 passes, barrier 1472개/284 batches, graphics command list 1개다. 이미 들어간 hierarchy work queue와 cull pass merge를 새 최적화로 계산하지 않는다. async compute는 과거 네 조합에서 더 느렸다는 현재 `output.toml`의 측정 기록이 있으므로 단순히 켜는 것을 해결책으로 삼지 않는다. 의미 있는 다음 후보는 중간 결과의 producer/consumer를 줄여 full-frame traffic과 barrier를 함께 줄이는 것이다. timestamp 비용을 실제 shader 실행 비용과 구별해야 한다.

source vertex pool은 현재 32-byte 전체 풀을 남기는 상태가 아니라 **28-byte lossless + handedness bits**를 소비한다. Train은 1,053,127,744 → 925,600,568 B다. 약 12.1% 감소이며 몇 배의 GPU frame 개선 근거가 아니다. 또 TLAS 0.118 ms는 이 씬의 첫 배수 최적화 대상이 아니다.

## 실제 Editor: 동적 표면을 먼저 줄여야 하는 근거

run 6에서 실제 Bathhouse saved World를 실행했다. VFX·fluid·weather 소비자가 살아 있고 1920×1080 native frame 120개를 수집했다. 앞 40개를 제외한 80개에서 GPU 중앙값은 **18.314 ms**, P95는 **19.020 ms**다. CPU record 중앙값 1.870 ms, submit 0.133 ms다. 프로파일의 646–657개 live scope를 잘리지 않게 수집했으며 프레임별 scope 합의 중앙값도 18.314 ms다. 원시 CSV는 게임의 `Artifacts/EditorShutdown20261003/native-frames.csv`, `native-passes.csv`; 요약은 `Logs/Crash20261003/actual-editor-profile.json`이다.

| 주요 scope | 프레임별 합의 중앙값 |
|---|---:|
| `r.as.streams` — 동적 표면 BLAS | **2.580 ms** |
| `v.coverage.stream` — 동적 표면 coverage | **1.822 ms** |
| `s.froxel.lists` | **0.897 ms** |
| `m.tsr.reject` | 0.553 ms |
| `pool surface` | 0.551 ms |
| `r.refl.lumen.trace` | 0.386 ms |
| `m.tsr.flicker` | 0.350 ms |

이는 정지 Train export와 다른 우선순위다. Editor에 대해서는 위의 일반 coverage/TSR/GI 후보보다 **동적 stream 소비 경로**를 먼저 다뤄야 한다. 실제 소스에서 확인한 원인은 다음과 같다.

1. `Native/Render/RayTracing/RayScene.cpp::recordStreams`는 매 프레임 `maxTriangles × 3` 정점 전체로 BLAS를 만든다. prebuild 크기만 재사용하고 실제 build는 모두 다시 한다. 현재 flag는 `PREFER_FAST_BUILD`이며 update/content-version 재사용 경로가 없다.
2. `Native/Render/Passes/Visibility/VisibilityTrack.cpp`는 `ceil(maxTriangles / 32)` 그룹을 DispatchMesh한다. `StreamRaster.ms.hlsl`이 GPU draw args의 실제 수를 읽어 빈 그룹의 출력을 0으로 만들지만, 그룹 실행 자체는 최대 용량만큼 발생한다. water의 `edgeOnly` 역시 전체 수면 mesh를 준비한 뒤 픽셀에서 경계를 검사한다.
3. `Water/FluidTail.hlsl`은 실제 삼각형 뒤의 빈 슬롯 정점을 NaN으로 비활성화한다. pool은 고정 격자이고 유체는 생성된 표면 수가 바뀌므로 하나의 refit 정책으로 묶을 수 없다.

구체적인 첫 구현 후보는 **GPU live count로 indirect DispatchMesh**, 다음은 water edge/HiZ tile 후보 목록을 먼저 만들어 내부 수면의 coverage 준비를 줄이는 것이다. 둘 다 CPU readback으로 live count를 얻지 않아야 한다. 다중 view에서 같은 producer generation을 소비하는 계약과 마지막 사용 fence를 유지해야 한다.

RT는 고정 topology pool과 variable-topology fluid를 분리해야 한다. pool은 같은 vertex count와 active set을 유지하는 경우에만 `ALLOW_UPDATE` 최초 build와 후속 update를 비교할 수 있다. 변하지 않은 producer revision의 BLAS 재사용도 후보지만, pool buffer 변경·slot 재배치·capacity 증가·여러 카메라의 순서를 invalidation에 포함해야 한다. fluid의 NaN active set이 바뀌면 그대로 refit할 수 없다. 같은 finite degenerate 슬롯으로 빈 삼각형을 표현하는 별도 설계는 가능하지만 ray hit identity, bounds와 traversal 비용을 먼저 검증해야 한다.

DXR의 update는 inactive↔active 전환과 vertex count/index 내용 변경을 허용하지 않는다. finite degenerate triangle은 active 상태로 남아 변형할 수 있으며 ray intersection을 만들지 않는다. BLAS 변경 뒤에는 소비하는 TLAS도 다시 유효하게 해야 한다. 따라서 플래그만 추가하는 수정은 정답이 아니다. [Microsoft DXR 명세의 inactive primitive 및 update 제약](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#inactive-primitives-and-instances)을 확인했다.

80개 프레임에서 두 stream scope만 줄이는 산술 가정도 계산했다 (`actual-editor-scenarios.json`).

| 두 stream scope의 비용 가정 | 예상 native GPU 중앙값 | 전체 배율 |
|---|---:|---:|
| 각각 2배 개선 | 16.028 ms | 1.143배 |
| 각각 4배 개선 | 14.878 ms | 1.231배 |
| 비용을 0으로 제거하는 불가능한 상한 | 13.758 ms | 1.331배 |

Editor에서도 이 두 경로만으로 전체 몇 배를 만들 수 없다. froxel 목록, 일반 coverage, TSR, GI/cache/reflection의 준비·조회 중복까지 줄이는 여러 묶음이 필요하다. 이미 켜서 더 느렸던 small-triangle compute를 성과로 계산하지 않는다.

## 최적화 적용 이전 측정·인수의 한계

실제 Editor 실행은 짧은 120-frame 진단이고 위 정지 gate의 600+600-frame 장기 측정을 대체하지 않는다. `HostRenderer::fxRunPending`의 별도 immediate compute graph와 graphics queue wait, World CPU, Scene/Game view의 추가 카메라 및 Inspector 비용은 이 native frame timestamp로 전부 측정되지 않는다. 따라서 18.314 ms를 전체 Editor FPS로 환산하지 않는다. 이번에 구현·검증한 것은 종료 크래시 수정이며, 몇 배 최적화의 후보와 필요한 절감량은 측정과 소스로 분석했다. 전체 2–3배 개선을 구현하거나 달성했다고 주장하지 않는다.

