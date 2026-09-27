## 재개 지점 (렌더 A, 2026-09-27 박막 배포 뒤; 주간 92 %, 스프린트 닫힘)
- 상태: 모든 작업이 커밋·배포됨. 박막(3254655, 3eaa208) 배포 = Unravel 9cb3434a(빌드 eb41b5e). 게이트 전체 ShadingTests 하드웨어 통과(박막 프레임은 비등방성 프레임 뒤에서 비눗방울 3.3e-5, 구리 산화막 8.4e-6), Unity UnravelNextSceneEditTests + UnravelNextD0SceneTests 2/2 통과, 렌더 이벤트 실패 0, TDR 0.
- Unity 속성: `_UnravelThinFilmThickness`(nm), `_UnravelThinFilmIor`, `_UnravelThinFilmCoverage`, `_UnravelThinFilmSubstrate`(0 상수, 1..5 금·구리·은·알루미늄·철), `_UnravelSubstrateIor`/`_UnravelSubstrateExtinction`. UnxMaterialDesc v5(216 B). C#의 배치 검사(RequireLayouts)도 크기를 같이 바꿔야 한다(첫 Unity 실행이 192 B 검사로 실패했었다).
- 비눗방울 캡처(`Results/A/ThinFilm/film_frame.png`)는 수치로는 맞지만 해만 있는 시험 장면이라 청록 하이라이트가 있는 어두운 구로 보인다. 비눗방울답게 보이려면 하늘 반사가 있는 장면, 투명 얇은 막(투과), 두께 변화가 필요하다.
- 다음(박막 남은 것): 투명 얇은 막(유리 등급, 비눗방울 투과) → 두께 텍스처 → 코트 아래 막(바깥 1.5: 임계각 주변 표 배치) → sheen·비등방성과 결합 → 하늘 있는 장면에서 비눗방울 캡처. 그다음 자동차 도장 → 스타일화 → 최적화.

## 재개 지점 (렌더 A, 2026-09-27 오전 후반)
- 면광원 커널 분리 완료(9a5d766): AreaLobes.hlsl(ShadeOpaque AREA_LOBES=1), 늘임 LTC 삭제. 전체 ShadingTests 통과. **배포는 아직**(디스크 대기) — 배포본(Unravel 72444ea1)에는 늘임 LTC 결함이 남아 있다. 다음 배포 때 같이 나간다.
- 남음: coverage 조각의 sheen·비등방 면광원(CoverageComposite DXIL 0.8 KB, 분리 필요), 좁은 로브 셀 무게, 박막, 최적화.

## 재개 지점 (렌더 A, 2026-09-27 오전)
- 끝낸 것: 크래시 수정(2acb060 FX 첫 틱 emitter table, ab26787 null import 가드; Unity 회귀 UnravelNextSceneEditTests), 비등방성 합류(cf5c495, 문서 954c2f3, 배포 Unravel 72444ea1), 면광원 구적 연구·GPU 미러(06706d0, 91883fd).
- 다음 첫 단계: 면광원 구적을 별도 커널로 셰이딩에 연결한다. ShadeOpaque에 인라인하면 LAYERED 변형이 DXIL 201~248 KB(한도 200 KB)다. 방법 후보: 셰이딩 전에 Layered·Sheen 타일에서 면광원 로브 복사휘도를 RGBA16F로 쓰는 커널(가시성은 ShadeOpaque와 같은 슬롯·overflow 목록, fallback 타일은 VSM 직접). 그다음 늘임 LTC(shAnisoLtc)를 지운다(알려진 결함, 배포 중). sheen+비등방 동시 재질은 검증에서 막는 안을 검토했다(렌더 C 소유 SceneData).
- 좁은 로브(α_b 0.0064) 면광원 정확도 2.7 %(게이트 2 %): 수평선에 걸친 큰 구, 긴 관에서 나온다. 셀 무게(중심값)와 float 조건수가 후보 원인이다.
- 남은 순서: 면광원 커널 → 박막 → 최적화(M 영역, 렌더 B 기준표 FEATURE_STATUS "최적화 기준표": m.lit.shade.b0 D0 3.42 ms, fp 3.82 ms). 렌더 이벤트 실패가 Unity 콘솔에 안 뜨는 결함(네이티브 로그만)도 남음.

## 재개 지점 (렌더 A, 2026-09-27 저녁; 주간 87 %, 90 % 상한 앞 정지)
- 상태: 모든 작업 커밋됨(UnravelNext bdc2b48까지, Unravel 751b8927까지). 마지막 배포 Unravel 751b8927 = 빌드 cbb572a(ShadingTests 하드웨어 통과). 배포 절차는 아래 그대로이며, Deploy.ps1은 상대 -Build 경로 결함을 faa3837로 고쳤다.
- 이번에 끝낸 것:
  - A13 UI 합성(Unravel 8df7b698): 파이프라인이 rendersUIOverlay로 그린다. SDR은 직접 그리고, HDR은 검정 위·흰색 위 두 장을 B + (W−B)·장면으로 합성한다(UI 흰색 = 종이 흰색). HDR 모니터 실화면은 확인하지 못했다.
  - A3 발광 입자 광원 완료(cccff0f, v1.79~v1.82). S·R은 렌더 B(c1bd9cd), 빛 플래그는 엔진 2(d8e0958). 성능 기록은 FX가 있는 게임 캡처에서 한다.
  - 메시 입자 합류(렌더 C 정의, 호출·UnxVfxMapMeshAsset·gpuInstances). C# 바인딩은 아직 없다(C가 요청하면 추가).
  - GI 화면 조도 텍스처(v1.80)는 M이 읽지 않는다. D0 이득 0 [실측, Results/M/GiScreenAB].
- 다음 순서:
  1. sheen 면광원: 지평선 절단 구면 삼각형 구적(MATERIAL_LAYERS 1.4 "채택 방향"). 먼저 크기별 오차로 차수 표를 정하고(LtcFit --polygon-check 확장), 그다음 HLSL을 ShadeOpaque LAYERED=2와 CoverageShade sheenOn에 넣는다(DXIL 여유 약 22 KB).
  2. 빔(엔진 2 계약 NativeVfx e72ca27f nv_copy_beam_paths, NV_BeamPath·점 식은 헤더 주석, 계약 시험 9점 8.9e-9 m): 호스트 ABI UnxFrameSetBeams(두 공개 사이 start·end 보간 + age 진행) → 리본 띠 경로에서 평가, Unity 전달(엔진 2가 복사 호출을 붙이겠다고 함), NativeVfx는 다음 배포와 함께.
  2. 비등방성 → 박막 → 자동차 도장 → 스타일화.
  3. 최적화: D0 10.27 ms, fp_1000 21.87 ms(s.froxel.integrate 4.65).

## 재개 지점 (렌더 A, 2026-09-27 06:20; 문맥 비움 전)
- 상태:
  - 모든 작업이 커밋되었다(UnravelNext ecb0546까지, Unravel 62724c31 이후 FEATURE_STATUS 측정).
  - 마지막 배포는 Unravel 7f41a91a(빌드 a0bb161)다.
  - 배포 절차: `Tools/CI/Build.ps1 -Track all -Committed` → 게이트 빌드로 전체 ShadingTests 하드웨어 → `Tools/UnitySlots/UnityLock.ps1 -Slot A ... -- Native/Host/Deploy.ps1 -Build ..\UnravelNext-gate\build\all` → Plugins·Native~ 커밋.
- 다음 순서(조정 결정):
  1. sheen 면광원. 알려진 결함이다: 면광원 아래 sheen 광택 없음. Charlie lobe가 고리 모양이라 LTC 적합 연구 (1) → (2) 2엽 → (3) 입체각 구적 순으로 한다. 코드는 `Passes/Common/MaterialModel.hlsli` modelSheen*, ShadeOpaque `LAYERED == 2`의 `scaleBase = keepS`, CoverageShade `sheenOn`이다.
  2. 발광 입자 광원. B와 계약했다: 장면 광원 버퍼 꼬리, 용량 = 빛 플래그 행 수, N + F_max ≤ 65,535, core 몫.
  3. 비등방성(G-buffer 접선) → 박막((c) 32칸) → 자동차 도장(코트 아래 금속 재설계) → 스타일화.
  4. 메시 입자·빔. 엔진 2와 계약했다: 자산 키, 방향 사원수 v4, 빔은 스트림 경로.
- UI 겹치기(A13) 확인은 캡처하지 못했다. 원인과 할 일:
  - 캡처 시험은 camera.targetTexture에 그리는데, Screen Space Overlay(uGUI, UI Toolkit)는 화면 백버퍼에만 그려진다.
  - UnravelNextPipeline은 SupportedRenderingFeatures.rendersUIOverlay를 두지 않으므로 Unity가 Render 뒤에 백버퍼 위에 겹쳐 그린다. 순서는 맞을 것으로 본다[예상].
  - HDR 출력(RGBA16F 선형 백버퍼)에서는 UI 밝기(paper white)가 맞는지 확인이 필요하다.
  - 할 일: PlayMode 시험에서 ScreenCapture.CaptureScreenshotAsTexture로 SDR·HDR 두 경우를 찍는다.
- 측정 [실측] 4K(`Results/M/D0Timing_0927`):
  - D0 중앙값 10.27 ms(M 4.21 / S 2.80 / R 2.49, 최상위 m.lit.shade.b0 3.35).
  - fp_1000 21.87 ms(S 13.99, 그중 s.froxel.integrate 4.65).
- 대기 중인 합류: 없음.

# I 트랙 상태 (통합: Unity 호스트 ↔ 새 렌더러) — 2026-09-25

표기: [실측] = 이 기계(i9-13900KF, RTX 4080, 드라이버 591.86, 모니터 2560×1440 143 Hz)에서 실행한 결과, [예상] = 비용식·가정.
빌드: `Tools/CI/Build.ps1 -Track I` (build/I, 트랙 V;M;S;R;I). 성능 측정은 `Tools/CI/GpuLock.ps1 -Track I` 아래에서만.
소유: UnravelNext `Native/Host/`, `Config/quality/host.toml`, 이 문서, `Results/I/` / 이전 저장소 `Assets/UnravelNextBridge/`.

> **재개 지점 (2026-09-26 전체 중단)**
> - D0 끝난 것: 없음(Unity 안 검증 0). 레벨 로더(UnravelNextScene·콘텐츠 변환·씬뷰 미리보기)와 1인칭 카메라(InterpolatedRoot, 렌더 시점 마우스)는 오프라인 컴파일만 통과했다. 이전 저장소 `Assets/UnravelNextBridge/Staging~/D0`(f0a8deb6)에 있다. D0 시험은 돌리지 못했다.
> - 반쯤 된 것: ① 변환기 단위·재질 규칙이 INTERFACES 8.5(core 5d13383)와 다르다. 텍스처 셰이더만 8.5로 고쳤다. ② 정적 콜라이더 호출 자리, 조준 밀기, F5/F9 시험 스크립트는 아직 없다. 물리 API는 `NativeDataStaticLevel.AddToStartup`, `PhysicsRayCast`, `PhysicsPushes`로 약속됐고 World의 `NativeWorldGame`은 c0798de1에 있다. ③ HostDynamic `--bench`(b7bf2bc)는 검증되지 않았다(첫 실행 exit 1). ④ turn 4 결과(1eb6f1b): restore 검사 exit 1, head Player 실행 0xC0000005 충돌, 둘 다 분석하지 않았다.
> - 다음 첫 단계: Unity를 모두 닫은 상태에서 Staging~/D0을 Assets로 옮기고, 변환기를 8.5에 맞추고, 슬롯 A에서 편집·플레이 Game view로 레벨이 보이는지 확인한다. 그다음 콜라이더·밀기·저장 스크립트와 D0 시험 순서다.

## 1. 호스트 경계 결정 (설계서 7.1-5)

### 1.1 Unity가 싣는 런타임 [실측]

- Unity 6000.6.0f1 에디터(`Unity.exe`)와 Player(`WindowsPlayer.exe`, 빌드된 `NativeDataWorld.exe`)가 모두 `D3D12SDKVersion = 618`,
  `D3D12SDKPath = .\D3D12\`를 export한다(PE export 표를 읽어 확인). 싣는 `D3D12Core.dll`은 **1.618.1.0**(에디터 `Editor\D3D12`, Player 템플릿
  `AgilitySDK\x64` 둘 다). 시스템 `D3D12Core.dll`은 10.0.26100.9278.
- UnravelNext는 1.618.5다. SDK 버전(618)이 같으므로 API·기능 집합이 같다: enhanced barriers, SM 6.6(드라이버가 6.8 노출), 메시 셰이더,
  DXR 1.1, 독립 디바이스(`ID3D12DeviceFactory`), 큐 우선순위 변경(`ID3D12CommandQueue1::SetProcessPriority`)이 헤더에 있다.
  Unity 디바이스 위의 실제 기능 조회(SM·메시 셰이더 tier·큐 우선순위)는 Unity 안 프로브(1.3)가 기록한다.

### 1.2 독립 실행 게이트 [실측]

`Native/Host/Gates/HostBoundary.cpp` (`unx_gate_host_hostboundary`). 같은 프레임 작업(4K RGBA16F 왕복 패스 18 + 의존 1그룹 패스 102 +
RGB10A2 출력 UAV 패스 = 121패스, 패스 사이 전역 UAV 배리어)을 네 가지 경계로 돌린다. "호스트 큐"(DIRECT)가 Unity 큐 역할을 하고, 프레임 앞에
작은 패스 하나, 뒤에 출력 전체 복사(Unity의 표시 blit 역할)를 한다. 프레임 2개 in flight, 출력 2개 교대, 1.5 s 워밍업, 600프레임 × 3회(회차마다
순서 반대). 큐 우선순위 HIGH. 원본: `Results/I/HostBoundary/standalone_{4K,1440p}_*.json`.

| 경계 | 4K 프레임 주기 중앙값 (회차별) | 1440p (회차별) | 융합 대비 |
|---|---|---|---|
| fused_list: 호스트 작업 + 프레임 + 호스트 작업을 리스트 하나로 (하한) | 2.658 (2.663 / 2.654 / 2.650) ms | 0.980 (0.976 / 0.981 / 0.982) ms | — |
| host_queue_lists: 프레임을 호스트 큐의 별도 리스트로 (= Unity `ExecuteCommandList`) | 2.699 (2.706 / 2.694 / 2.696) | 1.021 (1.035 / 1.020 / 1.020) | **+0.04 ms** |
| own_queue: 같은 디바이스의 자체 DIRECT 큐 + fence 왕복 | 2.996 (2.991 / 3.057 / 2.892) | 1.359 (1.323 / 1.394 / 1.343) | **+0.24~0.41 ms** |
| independent_device: 독립 디바이스 + 공유 텍스처·공유 fence | 2.972 (2.979 / 2.974 / 2.938) | 1.287 (1.280 / 1.342 / 1.318) | **+0.28~0.36 ms** |

- host_queue_lists의 추가분은 리스트 경계 두 번이다: 프레임 앞뒤 GPU 틈 각 **17.4 µs**(P0b의 ExecuteCommandLists 경계 ~15 µs와 같은 크기).
- 별도 큐·디바이스에서는 프레임 자체가 느려진다(4K 작업 2.60 → 2.94~2.95 ms, 1440p 0.92 → 1.31~1.41 ms) [실측]. 호스트 큐의 fence 대기는
  작업 끝에서 **0.39~0.48 ms** 뒤에 풀린다(gapAfter) [실측]. 원인은 두 그래픽스 컨텍스트의 시분할로 보지만 확인하지 않았다.
  두 번째 큐의 겹침 이득은 없고 손해만 있다. 설계서 4.3(async 이득 2~6 %, 큐 간 왕복 62~79 µs)과 같은 방향이고 크기는 더 크다.
- 4K own_queue 2회차에 P95 48 ms / P99 111 ms 이탈이 있었다(다른 세션의 잠금 없는 정확성 실행과 겹친 것으로 본다, 원인 미확정). 중앙값 판정에는 영향 없음.
- 공유 텍스처 비용: 독립 디바이스의 출력(simultaneous-access, 공유 힙)은 호스트 쪽 복사가 0.060 vs 0.040 ms(4K)로 느렸다. 같은 디바이스
  (host_queue_lists, own_queue)는 Unity 텍스처에 직접 쓰므로 복사가 없다.
- 싱글턴 [실측]: 같은 프로세스에서 `D3D12CreateDevice`를 두 번 부르면 같은 디바이스가 온다(`deviceSingleton: true`). 독립 디바이스는
  `ID3D12DeviceFactory` + `DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON`으로 만들어졌다.

### 1.3 Unity 안 프로브 [실측]

`UnravelNext.dll`(`Native/Host/src/Unity/UnityPlugin.cpp`, ABI 1)에 같은 작업의 Unity 안 판을 넣었다. Unity 디바이스에서 두 가지를 비교했다.
(a) Unity 큐의 리스트: `IUnityGraphicsD3D12v8::ExecuteCommandList`, 출력 상태 `UNORDERED_ACCESS` 선언.
(b) 자체 HIGH 큐 + fence 왕복.
Unity 큐에는 앞뒤로 타임스탬프 리스트를 넣었다. 프레임 구조는 before 이벤트 → 작업 이벤트 → Unity `Blit`(출력 → 1280×720 백버퍼) → after 이벤트다.
Player 빌드(IL2CPP, 창 1280×720, vsync 끔), 4K·1440p, 600프레임 × 3회(회차마다 순서 반대), GpuLock. 원본: `Results/I/HostBoundary/unity_player_20260925_093059.json`.
Unity 쪽 코드는 `Assets/UnravelNextBridge`(이전 저장소 커밋 0125b960)이고, Player는 `Builds/UnravelNextProbe`다.

| Unity 디바이스 위의 경계 | 4K 프레임 주기 (회차별) | 작업 | 1440p 주기 (회차별) | 작업 |
|---|---|---|---|---|
| Unity 큐의 리스트 | 2.813 / 2.763 / 2.767 ms | 2.64~2.69 | 1.036 / 1.037 / 1.041 ms | 0.92 |
| 자체 HIGH 큐 + fence | 2.949 / 3.023 / 2.930 ms | 2.98~3.00 | 1.323 / 1.287 / 1.458 ms | 1.08~1.16 |

- 차이: 자체 큐가 **4K +0.16~0.26 ms, 1440p +0.25~0.42 ms** 느리다. 독립 실행과 같은 방향이다. 자체 큐에서는 작업이 느려지고(4K +0.34 ms),
  Unity 큐의 대기가 작업 끝에서 0.42~0.93 ms 뒤에 풀린다.
- Unity 큐 경로에서 리스트 경계의 GPU 틈은 11~12 µs다. 작업 끝부터 Unity blit·after까지는 0.04~0.06 ms다.
  Unity `deltaTime` 중앙값이 주기와 같아(2.79 ms) GPU 병목 상태에서 잰 값이다.
- **순서 보존**: Unity `ExecuteCommandList`로 낸 작업 리스트와 큐에 직접 낸 타임스탬프 리스트·Signal·Wait가 제출 순서대로 실행됐다
  (before < 작업 시작 < 작업 끝 < after, 모든 프레임).
- CPU: 작업 이벤트(121패스 기록 + 제출)는 Unity 제출 스레드에서 0.21~0.65 ms였다. 4K에서 커지는 것은 Unity `ExecuteCommandList` 안의 대기로 보인다(미확인).

사실 [실측, Player 안]:
- 로드된 런타임: `<Player>\D3D12\D3D12Core.dll` **1.618.1.0**.
- Unity 디바이스: SM **6.8**, 메시 셰이더 tier 1, DXR tier 1.1, binding tier 3, heap tier 2, enhanced barriers 지원, 추가 형식 typed UAV load 지원.
  UnravelNext `Device`의 필수 조건을 모두 만족한다. Unity `SystemInfo`는 "Direct3D 12 [level 12.2]", 스레딩 모드 `NativeGraphicsJobsSplitThreading`, `maxQueuedFrames` 2.
- `D3D12CreateDevice`(FL 11_0, 12_2 둘 다)는 **Unity의 디바이스를 돌려준다**(싱글턴).
- Unity 큐: DIRECT, 우선순위 **NORMAL**, 플래그 `DISABLE_GPU_TIMEOUT`. `ID3D12CommandQueue1`은 있지만 `SetProcessPriority(HIGH)`는
  **0x887A0004(DXGI_ERROR_UNSUPPORTED)** 로 거부됐다. 그래서 Unity 큐 경로는 NORMAL 우선순위로 돈다. P0b 실측(설계서 1.4)에 따르면 다른 GPU 앱이
  있을 때 이 차이가 P99를 늘린다. 이 영향은 whole-frame P95 게이트에서 따로 잰다. 자체 HIGH 큐로 피하면 프레임마다 0.16~0.42 ms를 더 내므로 그 방법은 쓰지 않는다.
- 추가 큐(DIRECT HIGH, COMPUTE, COPY)를 Unity 디바이스에 만들 수 있다. 독립 디바이스(`ID3D12DeviceFactory`)는 Unity 런타임 경로로는 만들어지고,
  플러그인 폴더(D3D12Core 미배치)로는 0x887E0003으로 실패한다.
- 스왑체인(Player): R8G8B8A8_UNORM, 3버퍼, FLIP_DISCARD, `ALLOW_TEARING | FRAME_LATENCY_WAITABLE_OBJECT | ALLOW_MODE_SWITCH`,
  용도 RENDER_TARGET_OUTPUT | SHADER_INPUT(UAV 없음), sync interval 0, present 플래그 ALLOW_TEARING.
  렌더러 출력(RGB10A2)은 Unity가 백버퍼로 옮긴다(blit). 백버퍼에 UAV로 직접 쓸 수는 없다.

### 1.4 결정 [실측 근거]

**Unity의 D3D12 디바이스를 공유하고, 프레임은 Unity 그래픽스 큐에서 리스트 하나로 실행한다.** 제출은 Unity 제출 스레드의 큐 접근 이벤트
(`kUnityD3D12GraphicsQueueAccess_Allow` + `FlushCommandBuffers`)에서 `ExecuteCommandList`로 하고, 출력 텍스처 상태를 선언한다.
전용 큐·전용 디바이스는 독립 실행에서 +0.24~0.41 ms, Unity 안에서 +0.16~0.42 ms를 더 쓰고 이득이 없다.
또 이전 엔진의 물리 GPU·VFX GPU(`TitanSharedGpu`)가 Unity 디바이스에서 돌기 때문에, 같은 디바이스여야 그 출력(입자·Matter 표면)을 복사 없이 읽는다.
Agility 1.618.5 기능은 1.618.1에서도 모두 있다(SDK 618).
필요한 코어 변경은 `Docs/Design/Requests/20260925_I_unity_queue_device.md`에 요청했다: 외부 디바이스·큐로 `Device`를 만들고, 큐 실행 훅을 둔다.

## 2. C ABI (`Native/Host/include/unx/host/UnravelNextHost.h`, ABI 6)

- 규칙: 앞에 `{size, version}`이 있는 고정 크기 구조체만 넘긴다. 크기는 헤더의 `static_assert`와 C#의 `RequireLayouts`가 같이 확인한다.
  반환은 결과 코드와 `UnxLastError()`다. C++ 객체는 넘기지 않는다.
- 렌더러: `UnxRendererCreate`(Unity 디바이스·큐, 또는 `UNX_RENDERER_STANDALONE`), `UnxRendererDestroy`.
- 장면 적재: 텍스처, 재질(INTERFACES 8.1), 메시(스킨 스트림 포함), 스켈레톤, 인스턴스, 광원, 환경(태양·대기·바람)을 `scene::Scene`으로 모은다.
  `UnxSceneCommit`이 검증하고, V의 클러스터 빌더를 돌리고, `GpuScene`에 올리고, `FrameRenderer`를 만든다.
  `UnxSceneContentHash`는 장면 identity다.
- 프레임: `UnxFrameSetTransforms`·`SetSkeleton`·`SetSkeletons`(ABI 3: 모든 스켈레톤을 호출 한 번에, 포즈를 목록 순서로 이어 붙인 버퍼)·
  `SetInstanceVisible`·`SetSun`(코어 v1.8 GpuScene 갱신)으로 다음 프레임에 쓸 값을 모은다. 애니메이션 재설계의 `na_present_batch`가
  `SetSkeletons`의 버퍼를 그대로 채운다(ANIMATION_DESIGN_KO.md 3.6.2, 7.2).
- 시간대·날씨(ABI 4): `UnxFrameSetEnvironment`가 다음 프레임부터의 태양과 대기를 바꾼다(태양만이면 `UnxFrameSetSun`). 제출 스레드가 프레임
  기록 전에 `GpuScene::source()`의 태양·대기를 고치고, 대기 트랙은 파라미터·태양이 바뀌면 LUT를 다시 만든다. 떨어진 패킷의 환경은 다음
  패킷으로 넘어가고 현재 장면 저장에도 들어간다. 바람은 커밋 뒤 바꾸면 거부한다: 변형 상한(INTERFACES 6.4)이 바람 고정을 전제한다.
  코어에 `Requests/20260925_I_wind_change.md`를 요청했고 v1.23으로 반영됐다. S의 페이지 규칙 뒤 바람 거부를 풀었다(프레임마다 바람도 바뀐다).
  정확성 시험(현재 장면 저장에 환경·바람 거부 추가)은 게임이 끝난 뒤 돌린다.
  `UnxFrameQueue`는 카메라·시각·출력 텍스처를 스냅숏으로 떠서 티켓을 돌려준다. `UNX_EVENT_RENDER`(Unity 제출 스레드)가 그 티켓의
  프레임을 `FrameRenderer`로 기록한다. 실행은 `Queue::setExecuteHook`(코어 v1.9)으로 Unity의 `ExecuteCommandList`에 보내며, 이때 출력의
  상태를 `UNORDERED_ACCESS`로 선언한다. 결과 조회는 `UnxFrameStatsLatest`다.
  렌더러 코드: `src/Renderer/HostRenderer.*`. ABI 변환: `src/Unity/RendererAbi.cpp`.
- 장면 저장 `UnxSceneSave`(ABI 3): 커밋 뒤에는 호스트가 지금 보이는 장면을 쓴다. 렌더된·큐에 있는·아직 큐에 넣지 않은 갱신을 순서대로 겹치고
  숨긴 인스턴스는 뺀다. 포즈는 패킷과 공유(`shared_ptr`)해서 복사하지 않는다. (처음 구현은 커밋 때의 내용을 써서 모든 인스턴스가 원점에 있었다.)
- 감김 검사: `UnxSceneAddMesh`는 삼각형의 99 % 이상이 자기 정점 법선과 반대로 감긴 메시를 거부한다(앞면은 렌더러 공간에서 반시계).
  좌표계 반전 내보내기가 순서를 안 바꾸는 종류의 실수를 적재 때 잡는다(2.2의 검은 화면 원인).
- **검증 [실측]** `unx_test_host_hostabi`(정확성 실행, 잠금 없음, `-Tracks "V;M;S;R;C;I"` 빌드):
  - 왕복: SceneGen 여섯 장면(도시 354 인스턴스, 숲 1.1 M 인스턴스, 수변, 실내, 야간 도시 광원 512)에 스킨 캐릭터를 하나 더했다.
    DLL의 export(LoadLibrary + GetProcAddress, Unity P/Invoke와 같은 입구)로 넘긴 뒤 렌더러의 `scene::contentHash`가 원본과 **모두 같다**.
    ABI는 장면 이름·seed·카메라·경로를 싣지 않으므로 그것을 뺀 해시를 비교한다.
  - 프레임: 실내 장면 1440p, 64프레임(정지 카메라)이다. ABI 경로와 `FrameRenderer` 직접 경로의 마지막 프레임이 **비트 단위로 같다**(동일 픽셀 100 %).
    두 경로 모두 셰이딩의 확률 항 두 개(GI 프로브, 반사; `shading.experiment_disable = 6`)를 뺀 시험용 품질 사본을 쓴다.
    이 항을 넣으면 직접 경로 두 번끼리도 P99 ~300/1023만큼 다르다(아래 R 참고).
    이 검사는 호스트의 프레임 준비(카메라, 시각, 출력, 페이싱)를 확인하는 것이고, 화질 검증은 각 트랙이 기준 영상으로 한다.
  - 현재 장면 저장: 커밋 뒤 렌더된 프레임(인스턴스 0 이동, 인스턴스 1 숨김, 포즈 A), 큐에만 있는 프레임(인스턴스 0 다시 이동),
    대기 중 갱신(`SetSkeletons`로 포즈 B, 태양)을 만든 뒤 저장·적재하면 최신 값이 **그대로** 나오고 숨긴 인스턴스는 빠진다.
    길이가 틀린 포즈 버퍼는 아무것도 기록하기 전에 거부된다.
  - 참고(R 트랙에 알릴 것): GI 캐시는 실행마다 결정적이지 않고 64프레임에서 아직 수렴하지 않는다(실내 천장의 얼룩, 직접 경로끼리 평균 차 33~50/1023).
    프레임당 갱신은 200k 항목 중 7.8k, 항목당 이력은 최대 32회다. 품질·시간 안정성 게이트의 대상이다.

## 2.1 Unity 쪽 (`Assets/UnravelNextBridge`, 이전 저장소)

- `Runtime`: P/Invoke(`UnravelNextNative`, `UnravelNextRendererNative` — 구조체는 blittable, 크기 검사), `UnravelNextRenderer`(관리 래퍼,
  Unity 왼손 → 렌더러 오른손: Z 반전), SRP 에셋·파이프라인(게임 카메라마다 프레임을 큐에 넣고 렌더 이벤트 → Unity blit), 호스트 경계 프로브.
  - 좌표 변환: 점·방향은 z → −z, 행렬은 F·M·F, 탄젠트 w는 부호 반전. **삼각형 순서는 (a, b, c) → (a, c, b)**: Unity 자료에서는
    cross(b − a, c − a)가 이미 바깥 법선 쪽이고, 반전(det −1)이 그 부호를 뒤집는다. 처음에는 "시계 → 반시계라 그대로"로 잘못 두어
    모든 면이 뒷면으로 셰이딩돼 검게 나왔다(2.2 시각 확인, `--describe`로 모든 메시의 감김이 법선과 100 % 반대임을 확인).
- `DataWorld`: `UnravelNextDataWorld`는 이전 데이터 월드(`NativeDataWorldHost`)를 새 렌더러로 보인다.
  - 읽는 것: 호스트의 시작 개체군, 레시피의 비주얼 파트, `TitanMaterialLibrary`. 비균일 스케일은 강체 메시에 굽고, 스킨은 관절에 접는다.
    스킨 메시는 Titan cooked 형식(`FoliageProduct`)에서 가져온다.
  - 매 프레임: committed World의 `WorldAffine`과 직전 tick을 렌더 시각으로 보간한다(회전 slerp, 이동 lerp). 스킨 포즈는 committed Animation
    포즈 원본(`AcquirePose`)에서 가져오고, 포즈 원본이 실제로 쓴 alpha를 루트에도 쓴다. 권위 상태에는 쓰지 않는다.
  - `UnravelNextFrameMeter`(명령줄로 켜는 측정), 편집기 도구 `UnravelNextDataWorldScene`(원본 장면은 두고 사본에 컴포넌트를 더해 저장, Player 빌드).
  - 아직 못 보이는 것(로그로 알림, 조용히 빼지 않음): 소프트 바디(tick마다 변형되는 정점), Matter, 파괴 조각, VFX(FX 트랙), cooked solid 비주얼, 텍스처 배열 층.
  - 데이터 월드 캐릭터 [실측, cooked 파일 파싱]: 정점 550개, 삼각형 832개, 정점당 가중치 1개, 뼈 2개, morph 없음(형식 v7). 손실 없이 옮겨진다.
- C# 오프라인 컴파일 검사: Unity의 Roslyn(`DotNetSdk/.../csc.dll`)으로 프로젝트의 컴파일된 스크립트 어셈블리를 참조해 브리지 네 어셈블리를 컴파일한다.
  Unity 시간을 잡기 전에 컴파일 오류를 잡는다.
- 배포: `Native/Host/Deploy.ps1`이 DLL, 커널, 품질 파일을 `Native~`에 놓는다. DLL이 로드돼 있으면 거부한다.
  빌드 identity는 링크가 성공한 뒤 DLL 옆에 쓴 사본에서 읽는다. Player 빌드 후처리는 커널과 품질 파일을 `<Game>_Data/UnravelNext`로 복사한다.

## 2.2 데이터 월드를 새 렌더러로 (Player) [실측]

> **조건 표시(2026-09-25 저녁):** 이 절(2.2, 2.2.0~2.2.2)의 모든 Player·편집기 측정은 **50 Hz tick, World는 실시간의 83 % 속도** 조건이다
> (2.2.3). World 세션이 호스트 `Time.fixedDeltaTime`과 프로젝트 기본값을 1/60으로 고친 뒤 다시 잰다. GPU 프레임 시간은 이 조건과 거의 무관하지만,
> tick·시뮬·Unity 프레임 주기와 캐시 지연(움직임 속도)에 기대는 수치는 다시 재야 한다.

**데이터 월드가 Unity Player에서 새 렌더러로 뜬다.** 장면은 원본 `NativeDataWorld.unity`의 사본이다(`Assets/UnravelNextBridge/DataWorld/
NativeDataWorld_UnravelNext.unity`, 원본은 그대로). Player는 `Builds/UnravelNextDataWorld`(IL2CPP, 창 1280×720, vsync 끔)이다.
프레임은 Unity D3D12 큐에서 리스트 하나로 실행된다. 측정은 GpuLock, 1.5 s 워밍업, 해상도마다 서로 다른 완료 프레임 600개다.
원본: `Results/I/DataWorld/player_20260925_102722.json`(측정기의 형식 버그로 `max`는 잃었고 null로 복구했다. 다른 값은 원래대로다).

- 장면: 인스턴스 22(강체 상자·바닥·경첩·부표, 스킨 캐릭터, 영역 스트리밍 개체), 메시 10, 재질 6, 삼각형 2,584, 클러스터 89.
- 표시하지 못한 것(로그로 알림): 재질 1의 clearcoat·박막(요청 대기), 재질 2의 unlit(발광만으로 옮김), 캐릭터 법선(비균일 스케일, 요청 대기),
  소프트 바디·Matter·파괴 조각·VFX(렌더러 경로 없음).

| | GPU 프레임 중앙값 | P95 | P99 | CPU 기록 / 제출 (Unity 제출 스레드) | Unity 프레임 주기 중앙값 / P95 |
|---|---|---|---|---|---|
| 4K | **7.25 ms** | 8.14 | 10.29 | 0.26 / 0.06 ms | 12.30 / 18.28 ms |
| 1440p | **3.84 ms** | 4.53 | 4.82 | 0.30 / 0.07 ms | 5.49 / 16.67 ms |

- 트랙별 패스 중앙값 합(4K): **S 3.92**(`s.froxel.integrate` **2.76**, 가시성 0.36, 반영 0.15, VSM 무효화 0.13, markair 0.13, 페이지 래스터 0.12),
  **R 1.56**(반사 trace 0.76, 분류 0.26, GI 합 ~0.4), **M 1.21**(shade 0.69, resolve 0.29, edge 0.23), **V 0.17**. 합 6.87 ms, 나머지 ~0.4 ms는 배리어·틈이다.
  1440p: S 2.07(froxel 1.27), R 0.86, M 0.62, V 0.13.
- **주의(11:30 시각 확인으로 발견):** 이 실행을 포함해 2.2의 모든 데이터 월드 영상은 감김 결함(2.1) 때문에 모든 면이 뒷면으로 셰이딩되어
  빛 받는 면이 (0,0,0)이었다. 하늘만 맞다. 패스는 모든 픽셀에서 돌았으므로 비용 구성은 참고가 되지만, 올바른 영상의 측정이 아니다.
  고친 뒤 다시 잰다.
- 판정(결함 발견 전): 4K 7.25 ms는 목표 6.06 ms를 넘는다. 이 장면은 삼각형 2.6k로 거의 비어 있으므로 초과분은 부하가 아니라 트랙 패스의 고정비다.
  가장 큰 항은 프록셀 적분(설계 비용식 0.03 ms 대비 2.76 ms)이고 S 트랙에 알렸다.
  호스트 경계의 비용은 따로 쟀다(프로브, 1.3절: Unity 큐 리스트 경계 11~17 µs × 2).
- Unity 프레임 주기가 렌더러 GPU 시간보다 길다(4K 12.3 대 7.25 ms). 1440p P95는 16.7 ms로 tick 프레임에서 튀는 모양이다.
  이것은 데이터 월드 시뮬레이션(CPU tick, VFX GPU 등) 쪽 비용이라 감사 세션에 전달했다.

### 2.2.0 감김 수정 뒤 측정 (13:20~13:27) [실측, 유효]

DLL: UnravelNext 커밋 빌드 51e331b(ABI 4, 다른 트랙의 미커밋 변경 없음), 게임 종료 뒤, GpuLock(C의 기준 렌더는 잠금 중 멈춘다).
영상: 바닥·상자·캐릭터·바퀴가 빛을 받고 접촉 그림자가 있다(`player_20260925_132035_3840x2160.png`, 편집기 캡처도 같다).
금속 재질(metallic 0.85, roughness 0.3)에 어두운 반점·얼룩이 있다. **원인 분리[실측, 14:17~14:26, DLL 1371ac5(R의 GI 캐시 모서리 수정 뒤),
원해상도 캡처]:** 기본은 반점이 남는다. `shading.experiment_disable=4`(반사 G/M과 K 경로 스펙큘러 입사 끔)면 사라지고, `=2`(프로브 확산
조도 끔)면 그대로다. 그래서 R의 반사 결과(또는 경로 전환) 쪽이다. 네모 블록 모양이고 바닥 접촉부에서 짙다. R·M에 보냈다.
`player_cap_{default,disable4,disable2}_*_3840x2160.png`. Player 옵션 `-unxCaptureFull`(원해상도), `-unxQualityDir <폴더>`(품질 사본)를 더했다.
M의 추가 분리[실측, 14:34~14:36, 같은 조건]: `reflection.cache_lobe_half_angle_min_deg=0`(모두 K 경로, 광선 없음)은 반점이 없고, `=90`(모두 광선)은
반점이 남는다(거친 바닥의 firefly는 광선 강제 때의 1표본 잡음, 진단용). 그래서 R의 광선 hit 경로다. R은 광선 hit 발자국이 그 점의 모든 셀보다
거칠면 조회가 0을 돌려주던 결함을 고쳤다(0ff332b: 더 고운 레벨까지 찾는다). `player_cap_lobe{0,90}_*_3840x2160.png`.
0ff332b 뒤[실측, Player 렌더러 파일만 0ff332b, 앞 상자 정면에서 중앙값 절반보다 어두운 픽셀]: 기본 3.98 %(1371ac5는 4.15 %), `reflection.experiment_disable`
16(hit 캐시 조회 끔) 4.66 %, 2(hit 태양 가시성 없음) 4.77 %, 32(순회만, hit이 1) 0.00 %, K만 0.00 %. 순회·원점은 원인이 아니고 hit 셰이딩이다.
R에 보냈다(R이 반사 곱셈 수정을 준비 중).
886fee2(R fd008b7 비음수 추정·e277df1 포함, Player 재빌드, 새 World·VFX·Animation DLL, identity 사이드카 있음)[실측, 50 Hz tick 조건]: 반점 지표 4.48 %
(0ff332b 3.98 %)로 변화가 없다. 같은 빌드에서 R 통계(`-unxLogFile`, `reflection.stats_log_frames = 60`): 워밍업 뒤 비율 가지 D/C 0.469 %(보통)·0.332 %
(1024+256), log2(mean L / mean g)가 1 아래인 표본 39.6 %·42.7 %, 16배 이상 22.4 %·15.8 %, hit 캐시 no data 0 %. 워밍업(frame < 120)은 no data
6.1 %·16.6 %다. 비율 가지가 드물어 음수 잘림으로는 반점을 설명하기 어렵다. 성긴 G 표본(약 12k 표본이 669k 픽셀을 8×8까지 보간)의 분산으로 보여
R에 전달했다. 로그 `Results/I/DataWorld/rstats_{,scale_}886fee2.log`.
빌드된 Player의 렌더러 파일(DLL·커널·품질)은 Unity 재빌드 없이 커밋 빌드로 바꿀 수 있다(스크래치패드 `update_player_renderer.ps1`, 관리 코드와
ABI가 그대로일 때만; `<Data>/UnravelNext/renderer.build.json`에 커밋을 적는다).

| 실행 | 해상도 | GPU 프레임 중앙값 | P95 / P99 | CPU 기록 / 제출 | 호스트 동기화(메인) | Unity 프레임 주기 중앙값 / P95 |
|---|---|---|---|---|---|---|
| Player | 4K | **5.05 ms** | 5.39 / 5.57 | 0.26 / 0.05 ms | 0.048 ms | 6.15 / 13.9 ms |
| Player | 1440p | **2.82 ms** | 3.34 / 3.60 | 0.28 / 0.05 ms | 0.049 ms | 3.93 / 17.4 ms |
| 독립 실행(같은 장면, `unx_gate_host_hostscene`) | 4K | 4.83 ms | 5.36 / 5.57 | 0.43 / 0.10 ms | - | - |
| 독립 실행 | 1440p | 2.75 ms | 3.22 / 3.49 | 0.34 / 0.08 ms | - | - |
| 편집기 Play 모드 | 4K | 5.57 ms | 10.16 / 11.25 | 0.35 / 0.07 ms | 0.081 ms | 16.0 / 57.5 ms |
| 편집기 Play 모드 | 1440p | 3.03 ms | 3.86 / 5.67 | 0.32 / 0.07 ms | 0.073 ms | 12.1 / 21.5 ms |

- 원본: `Results/I/DataWorld/player_20260925_132035.json`(+ PNG, 내보낸 장면 `data_world.unxscene`), `editor_20260925_132428.json`(+ PNG),
  `Results/I/HostScene/I host scene data_world {4K,1440p}_*.json`.
- 트랙 합(Player 4K, 프레임당): M 2.12(`m.shade` 1.80), S 1.23(`s.froxel.integrate` 0.46, `s.shadow.visibility` 0.33), R 1.07(`r.refl.trace` 0.33,
  `r.gi.trace` 0.21), V 0.17. 1440p: M 0.98, R 0.88, S 0.64, V 0.13.
- 판정: 데이터 월드가 Unity Player에서 **4K 5.05 ms, 1440p 2.82 ms**로 목표(6.06 / 5.00 ms) 안이다. Unity 프레임 주기 중앙값도 4K 6.15 ms다.
  단, 재질 1의 clearcoat·박막은 아직 빠져 있다(코어 재질 v2 대기).
- Unity 호스팅 비용: 같은 장면을 Unity 밖에서 그리면 4K 4.83, 1440p 2.75 ms다. 차이(+0.22 / +0.07 ms)는 패스별로 보면 호스팅이 아니라
  내용 차이다. Player 장면은 움직이고(떨어지는 상자, 애니메이션) 내보낸 장면은 정지라 VSM 래스터가 +0.08 ms 더 들고, `m.shade`는 오히려
  Player가 0.2 ms 적다(GI·장면 상태). 리스트 경계 비용은 GPU 타임스탬프 밖이며 프로브로 따로 쟀다(1.3절: 11~17 µs × 2).
- 편집기는 개발 환경이다. GPU 중앙값은 Player와 비슷하지만(4K +0.5 ms) 편집기 자체 그리기 때문에 꼬리와 프레임 주기가 크다.

### 2.2.1 재측정 11:23~11:34 (무효: 감김 결함 + CPU 오염)

S의 공기 볼륨 커밋(f1f6f8a) 뒤의 DLL(792f315 + 다른 트랙의 미커밋 변경)로 Player와 편집기 Play 모드를 쟀다.
원본: `Results/I/DataWorld/player_20260925_112345.json`(+ PNG 두 장, 내보낸 장면 `data_world.unxscene`), `editor_20260925_112414.json`.
- 영상: 빛 받는 면이 모두 (0,0,0)이다(감김 결함). 이 캡처로 결함을 찾았다.
- 오염: 11:17:51부터 `unx_reference.exe`가 잠금 없이 CPU 약 17코어를 썼다(8.5분에 CPU 8,700 s). Unity 프레임 주기가 Player 4K P95 363 ms,
  편집기 4K 중앙값 146 ms로 무너졌고 GPU 꼬리도 커졌다(Player 4K P99 54 ms). 조율 세션이 C에 잠금 중 기준 렌더 워커를 멈추도록 요청했다.
- 참고로 남기는 GPU 중앙값[실측, 위 두 조건 아래]: Player 4K 5.60 / 1440p 4.11 ms(`s.froxel.integrate` 0.44 ms, 이전 2.76),
  편집기 4K 5.95 / 1440p 2.85 ms. 호스트 동기화(IL2CPP, 스킨 5체 + 강체 17개): 0.08 ms, 그중 `AcquirePose` 0.05 ms, 관절 변환+호출 0.006 ms.
- 내보낸 장면은 인스턴스가 모두 원점이었다(`UnxSceneSave` 결함, 2절에서 고침). 고친 브리지는 첫 동기화 뒤에 저장한다.

### 2.2.2 Unity 안의 설계 규모 부하 (`-unxScale 1024,256`, 13:23) [실측]

실제 개체의 committed 루트·포즈를 읽는 인스턴스를 더해 강체 1,024 + 스킨 256을 만든다(Player, IL2CPP). 원본 `player_scale_20260925_132306.json`.

| 해상도 | GPU 중앙값 (P95) | 호스트 동기화 중앙값 (P95, P99) | 그중 `AcquirePose` | 관절 변환 + `SetSkeletons` 한 번 | Unity 주기 중앙값 |
|---|---|---|---|---|---|
| 4K | 6.51 ms (7.02) | 0.735 ms (1.39, 1.80) | 0.333 ms | 0.055 ms | 13.4 ms |
| 1440p | 3.56 ms (3.99) | 0.705 ms (1.26, 1.49) | 0.315 ms | 0.052 ms | 5.04 ms |

- 루트(1,280개 World 모음·보간·변환·`SetTransforms`): 약 0.35 ms = 개당 0.27 µs(IL2CPP).
- 포즈: `AcquirePose`(Animation의 보간 평가·접촉·물리 오버레이)가 캐릭터당 1.3 µs(뼈 2개 캐릭터). 관절 변환은 관절당 약 0.1 µs다.
  설계 캐릭터(64본 × 256체 = 16k 관절)에서는 관리 코드 변환만 약 1.6 ms가 된다[예상, 위 실측으로 계산]. 애니메이션 재설계의
  `na_present_batch`가 `SetSkeletons` 버퍼를 네이티브에서 채우므로 이 관절 단위 관리 작업은 없어진다(프레임당 호출 2번).
- **일괄 표시 연결(스테이징, 다음 편집기 차례):** 어댑터가 애니메이션의 `NativeDataAnimation.Present(목록)`(A5, 400997db)을 쓴다. 스킨 파트는 개체가
  처음 보일 때 팔레트를 한 번 풀어 `NativeDataPresentation`에 들어가고(영역 개체는 적재 때 World에 없다), 프레임마다 활성·alpha를 둔 뒤
  호출 한 번으로 모든 캐릭터의 관절이 `UnxFrameSetSkeletons` 버퍼에 채워진다. 루트는 항목별 `PresentedAlpha`로 보간한다. 관절 단위 관리 작업이 없다.
  검증 스위치 `-unxPoseCheck N`: N프레임 동안 옛 경로(AcquirePose + 관리 변환)와 관절별 최대 차를 로그에 남긴다.
- 루트도 같은 방식으로 옮길 수 있다: World의 `nw_snapshot_read_components`로 모은 두 tick의 `WorldAffine`을 네이티브에서 보간·변환한다.
  지금 0.35 ms라 메인 스레드 1 ms 할당 안이지만, 인스턴스가 늘면 비례해서 는다.
- **4K Unity 프레임 주기 13.4 ms의 원인[실측]: 데이터 월드 tick.** 측정기에 Unity `FrameTimingManager`(메인·렌더 스레드, GPU, present
  대기; 빌드 때만 Frame Timing Stats를 켠다)와 고정 스텝 시간(FixedUpdate 맨 앞·맨 뒤 훅)을 넣어 다시 쟀다(13:50, 13:55).
  4K: 메인 스레드 12.96 ms 중앙값(주기 12.98), 렌더 스레드 1.41, Unity GPU 프레임 7.02(렌더러 6.44), present 대기 0.007 ms.
  **고정 스텝(= `NativeDataWorldHost.FixedUpdate` tick)이 스텝당 12.57 ms**(P95 18.5, 최대 23.0, 556 스텝, fixedDeltaTime 0.02 s)다.
  4K는 GPU 쪽 프레임이 7 ms라 61 %의 프레임에 tick이 들어가 중앙값이 tick 프레임이 된다. 1440p는 메인 스레드 중앙값 1.49 ms,
  P95 13.9 ms(tick 프레임)이고 present 대기 2.75 ms로 GPU 쪽이 경계다. GC(Gen0)는 프레임당 약 1회(4K 10.2 s에 843회)다.
  렌더러(GPU, 렌더 스레드, 내 동기화 0.74 ms)가 원인이 아니다. tick 비용은 감사 세션 영역이라 전달했다. 원본 `player_scale_20260925_135022.json`,
  `player_scale_20260925_135522_4K_partial.json`(아래 TDR 때문에 로그에서 되살린 4K 창).
- **TDR (13:56:45):** 두 번째 실행에서 4K 창과 캡처가 끝나고 1440p로 바꾸는 순간 Player의 D3D12 장치가 제거됐다(887a0005, 이유 887a0006
  DEVICE_HUNG). 잠금은 I만 잡고 있었다. 먼저 알아챈 곳은 tick 안의 NativeVfx(`PrepareCohortBatch`: DeviceFailed)이고 원인은 아직 모른다.
  같은 DLL로 13:23에는 전환이 정상이었다. 규칙대로 GPU 실행을 멈추고 조율 세션에 알렸다. 덤프 `Crash_2026-09-25_045643944`.
  - 드라이버 이벤트(nvlddmkm 153, 13:56:38~43)가 이 실행 안에 있다. 유력한 원인[M의 GBV 증거, 코어 확인 대기]: RenderGraph의 가져온 리소스
    뷰 캐시가 raw 포인터를 키로 쓰고 지우지 않는다. 4K→1440p 전환에서 Unity가 출력 RT를 다시 만들 때 새 RT가 옛 주소를 받으면,
    파괴된 텍스처의 서술자로 쓴다(주소 재사용에 달려 간헐적). 옛 VFX 충돌 커널(FP64, 입자 × 표면 6,912)도 후보였다.
  - 대응: 새 정확성 시험 `unx_test_host_hostswitch`(HostRenderer 하나, 출력 4K↔1440p 네 번, debug layer + GBV)는 37a69d6에서 오류 0으로
    통과했다(83 s). 코어 수정(4532054: 가져온 뷰 캐시가 항목이 살아 있는 동안 리소스 참조를 쥐고, 설명이 바뀌면 뷰를 다시 만들고, 그 프레임에
    가져오지 않은 항목은 GPU 뒤에 버린다) 뒤 c4a1d2b에서 같은 크기 출력을 매 프레임 새로 만드는 단계를 더해 GpuLock 아래 다시 돌렸다.
    오류 0, 장치 제거 없음(130 s). 단, 수정된 캐시가 참조를 쥐므로 재생성 단계에서 주소 재사용은 16번 중 0번이었다. 이 시험은 새 계약과 맞다는
    것이지 옛 결함의 재현을 보인 것이 아니다(그 증거는 M의 GBV 오류와 코어 회귀 시험).
  - 임시 규칙(조율 세션): 코어 수정 전까지 Player·편집기는 해상도마다 따로 띄운다(한 프로세스 안 전환 금지). 하드웨어 GPU를 쓰는 모든
    실행은 GpuLock -Track I 안에서 하나씩 한다. Player 규모 실행은 옛 VFX를 CPU로 돌린다(`-unxVfxCpu` → `EnableVfxGpu = false`, 측정 조건에 적는다).
  - 짧은 규모 실행(14:13, DLL 37a69d6, `-unxVfxCpu`, 해상도마다 120프레임): 4K와 1440p 모두 통과했고 장치 오류는 없다.
    `player_scale_short_20260925_141334.json`. 4K GPU 8.11 ms 중앙값(짧은 창이라 P95 19.5), 고정 스텝 8.39 ms(옛 VFX CPU 실행이라 성능 근거는 아니다).
- GPU: 4K 6.51 ms는 목표 6.06 ms를 넘는다. 가장 큰 항은 `m.shade` 2.38 ms(4K). 캐릭터 256체와 상자 1,024개가 화면을 많이 덮는다.

## 2.5 설계 동적 규모의 호스트 경로 [실측]

> **2026-09-26 변경(860af88, 9a342b9):** 게이트가 합성 장면의 고리 배치를 버리고 RPP-1 장면에서 돈다(조율 지적: 강체 1,024개가 지름 10 m 안에 겹쳐 대표성이 없었다).
> - 적재: 장면은 `UnxSceneLoad`(ABI 6 안의 선택 export)로 싣고, C의 배치 파일 `<장면>_bodies.json`(C 195fda3)을 읽는다.
> - 강체 1,024개: 정지 / restHeight까지 낙하 / 굴러감을 운동학적으로 재생한다. 주기가 넘어가면 teleport 플래그를 붙인다. 장면 t0의 3 × 3을 유지한다.
> - 캐릭터 256은 파일의 슬롯에, 카메라는 장면 camera 0이다. 파일이 없거나 오래되면 오류다. 시험 `HostBodies` 12/12.
> - 장면은 공유 `Cache/Scenes`를 덮지 않도록 `unx_scenegen --scene <s> --out <dir> --bodies <dir>/<s>_bodies.json`으로 따로 만든다.
> - city_block [실측, build/I 860af88]: GPU 4K 9.785 / 1440p 5.686 ms, `r.as.tlas.dynamic` 0.131 / 0.136 ms, refit 0.031 ms, 인스턴스 1,633.
> - 결과 JSON에는 큐별 head/tail/gap, 명령 목록·배리어 수, GpuLock 경합(경합 초·프레임, 경합 없는 GPU 시간)이 들어간다.
> - 아래는 옛 합성 장면의 기록이다.

`unx_gate_host_hostdynamic`(독립 실행, `UnravelNext.dll`의 export 경유, GpuLock): 강체 1,024개가 매 프레임 움직이고, 캐릭터 256체
(64본, 60k 삼각형 공유 메시, 정점당 가중치 2개)가 매 프레임 새 포즈를 받는다(RPP-1 N_dyn과 ARCHITECTURE 2.8 부하). 인스턴스 삼각형 1,540만.
원본: `Results/I/HostDynamic/host_dynamic_20260925_111846.json`, `..._112359.json`(패스별 분포 포함).

| | GPU 프레임 중앙값 | P95 | 호스트 갱신 호출(변환 1,024 + 포즈 256 + 큐) | CPU 기록 / 제출 |
|---|---|---|---|---|
| 4K (11:23, 프록시 전) | 8.27 ms | 10.77 | 0.140 ms | 0.69 / 0.08 ms |
| 1440p (11:23) | 5.79 ms | 6.51 | 0.134 ms | 0.69 / 0.08 ms |
| **4K (13:04, d3701c7 커밋 빌드)** | **4.69 ms** | 5.26 (P99 5.56, 최대 6.15) | 0.165 ms | 0.80 / 0.09 ms |
| **1440p (13:04)** | **3.08 ms** | 3.55 (P99 3.78) | 0.143 ms | 0.70 / 0.07 ms |

13:04 실행[실측]: 커밋 기준 빌드(`-Track all -Committed`, 다른 트랙의 작업 중 파일 없음), 게임 종료 뒤, 첫 프레임 제외 수정 뒤, 호스트는
`UnxFrameSetSkeletons` 한 번. 원본 `Results/I/HostDynamic/host_dynamic_20260925_130428.json`.
- **정정(R, 이후 실측):** 13:04의 `r.as.refit` 0.355 ms는 캐릭터 프록시 refit이 아니라 반사 정확 집합(원본 60k 삼각형 × 8 슬롯)이다. 정확 집합을
  끄면 프록시 256체 refit은 0.012 ms다. 군중 캡슐 BLAS는 오히려 느려 되돌렸다(5ec722c). 다음 과제는 정확 집합 비용과 동적 TLAS(1,281 인스턴스)다.
  아래 줄의 "프록시 refit" 해석은 이 정정으로 바뀐다.
- R 캐릭터 프록시(2c10e34): RayScene "deformed 763,392 tris (0 above proxy budget)" = 256 × 2,982. `r.as.refit` 1.61 → **0.355 ms**,
  `r.as.deform` 0.26 → 0.034 ms(4K). R 예상 약 0.5 ms보다 작다. 설계 2.8 할당 0.13 ms(근거리 64체만 refit, 군중 192체는 캡슐)와는 아직
  2.7배다(위 정정 참고: 이 값은 정확 집합이다).
- S VSM 변위 경계(dc6d35d): `s.vsm.raster.raster` 0.81 → **0.16 ms**, `s.shadow.visibility` 0.36 → 0.14 ms(4K).
- M: `m.shade` 1.32 → 0.78, `m.resolve` 0.38 → 0.24 ms(4K).
- 트랙 합(4K, 프레임당): R 1.63, S 1.51, M 1.12, V 0.17. 큰 항: `m.shade` 0.78, `s.froxel.integrate` 0.55, `r.refl.trace` 0.36,
  `r.as.refit` 0.36, `r.gi.trace` 0.29, `m.resolve` 0.24.
- 판정: 이 부하(설계 동적 규모, 인스턴스 삼각형 1,540만)에서 4K 4.69 ms, 1440p 3.08 ms로 목표(6.06 / 5.00 ms) 안이다. 장면에 수목·물·
  광원·VFX가 없으므로 RPP-1 전체 부하의 판정은 아니다.

- 큰 항(4K): `r.as.refit` **1.61 ms**(캐릭터 256 × 60k 원본 refit. RayScene이 "256 above proxy budget"을 보고한다. V의 클러스터 LOD 절단이
  생기면 8k 프록시로 간다, 설계 0.13 ms), `m.shade` 1.32, `s.vsm.raster.raster` 0.81, `s.froxel.integrate` 0.45, `m.resolve` 0.38,
  `r.refl.trace` 0.38, `s.shadow.visibility` 0.36, `r.gi.trace` 0.26, `r.as.deform` 0.26. 조율 세션을 거쳐 R·V·S·M에 전달했다.
- 게이트 결함(고침): 첫 실행의 4K 창에 렌더러의 첫 프레임이 들어갔다(`s.atmosphere.multiscatter` 23.3 ms 1회, 최대 45 ms).
  첫 프레임은 파이프라인 생성 때문에 1.5 s 워밍업이 끝난 뒤에 완료 보고가 온다. 이제 워밍업이 끝난 뒤 큐에 넣은 프레임만 센다
  (게이트와 Unity 측정기 모두). 이 LUT 최초 생성 23 ms 자체는 태양 변화 때 멈춤이 되므로 S에 전달했다.
- P95 10.8 ms(4K)는 같은 시각 잠금 없이 돈 기준 렌더러의 영향이 섞였을 수 있다. 오염 없는 조건에서 다시 잰다.
- 호스트 호출 비용은 스켈레톤마다 호출하던 때의 값이다. `UnxFrameSetSkeletons`(한 번)로 바꾼 뒤 다시 잰다.
- R의 캐릭터 프록시(2c10e34)가 이 장면에 적용되는지[실측, CPU]: 관 메시(60,096 삼각형)의 V LOD 절단은
  60096 / 2982 / 1274 / 932 / 466 / 232 / 116 / 58이고, `raytracing.character_proxy_triangles` 8000 안의 가장 고운 절단은 2,982 삼각형이다.
  확인: `unx_gate_host_hostdynamic --save-scene <f>`(내용만, 커밋 전 저장) → `unx_gate_host_hostscene --scene <f> --describe`("LOD cuts" 줄).
  R·S가 게임이 끝난 뒤 이 게이트로 refit·VSM 수정을 잰다(사용법을 두 세션에 보냈다).

### 2.2.3 데이터 월드 tick은 50 Hz였다 (2026-09-25 저녁 발견)

- `ProjectSettings/TimeManager.asset`의 Fixed Timestep이 0.02 s라 Unity `FixedUpdate`, 곧 데이터 월드 tick이 50 Hz로 돈다.
  `NativeDataWorldHost.FixedDeltaTime = 1/60`은 물리 설정에만 쓰이고 런타임에 `Time.fixedDeltaTime`을 맞추지 않는다(편집기 저작 도구만 맞춘다).
  그래서 Player에서 World는 tick마다 1/60 s를 나아가면서 초당 50번 tick한다. 곧 실시간의 83 % 속도다. 설계와 사용자 결정은 60 Hz다.
- 이 조건으로 잰 tick·시뮬 측정(2.2.2의 고정 스텝 12.57 ms 등)은 초당 부하를 1/6 적게 본 것이다. 수정 소유는 World·감사(호스트가
  Initialize에서 `Time.fixedDeltaTime`을 맞춤)와 조율(프로젝트 기본값)이다. 조율 세션에 알렸다.
- I 결함: 어댑터의 렌더 보간 alpha가 `Host.FixedDeltaTime`으로 나눠서, 0.02 s tick 간격이면 매 tick 마지막 3.3 ms 동안 포즈가 멈췄다.
  Unity의 실제 fixed step으로 나누게 고쳤다(스테이징, 다음 편집기 차례에 넣는다). 측정기는 `fixedDeltaTime`을 기록하므로 조건에 tick 주파수가 남는다.

> **정정(2026-09-25 밤):** 이 문서의 Player는 모두 **IL2CPP**다(프로젝트 Standalone 스크립팅 백엔드 = 1, `GameAssembly.dll`·`il2cpp_data`).
> 앞서 "Mono"로 적은 것은 틀렸다. 관리 코드 비용(루트 0.27 µs/개, 관절 약 0.1 µs, `AcquirePose` 1.3 µs/체)은 IL2CPP, 곧 출시 백엔드의 값이다.

### 2.2.4 60 Hz 데이터 월드 (2026-09-25 23:29) [실측]

World 세션의 수정(TimeManager 1/60, 호스트가 `Time.fixedDeltaTime`을 맞추고 매 tick 검사) 뒤, ABI 6 Player(렌더러 f8d09d3, 브리지 a756b0f5,
보간 alpha는 Unity 실제 fixed step으로 나눔, 옛 VFX CPU). 원본 `player_tick60_f8d09d3_20260925_232942.json`(+ identity).
- **tick 주파수 확인:** committed World tick이 초당 60.04(4K 창)·60.02(1440p)이고, World 시간(Δtick × 1/60)이 실시간의 1.0007·1.0003배다.
  고정 스텝 `fixedDeltaTime` = 0.01667. 50 Hz·83 % 속도 결함은 풀렸다.
- GPU 프레임 중앙값: **4K 4.11 ms**(P95 6.22), **1440p 2.22 ms**(P95 2.68). 트랙 합(4K) M 1.61, S 1.19, R 0.88, V 0.16.
- 4K P99 44 ms·최대 78 ms는 렌더러가 아니다. 같은 프레임에서 3 µs짜리 패스(`v.cull.instances.p2`, `r.gi.rehash`)까지 모든 패스가 최대 약 19 ms로
  늘었으니, 다른 프로세스가 GPU를 나눠 쓴 것이다(정확성 실행은 이제 잠금 밖). 꼬리 판정에는 쓰지 않는다.
- 고정 스텝(데이터 월드 tick) 중앙값 6.21 ms(4K 창)·8.41 ms(1440p 창)로 50 Hz 때 12.6 ms보다 짧다(World W2 충돌 표면 수정 포함).
  Unity 프레임 주기 중앙값 4K 5.07·1440p 3.02 ms. 메인 스레드 중앙값 1.06·0.90 ms. 호스트 동기화 0.044·0.036 ms.

### 2.2.5 사용자 보고: "화면이 뒤집힌 것 같다", "자글자글한 노이즈" (2026-09-26 0시) — 원인과 수정

보고는 23:29 Player 창(1280×720, 출력 3840×2160)에서 나왔다. 코드에서 찾은 원인은 네 가지다. 첫 셋은 표시 경로, 넷째는 이력이다.
- **행 순서 [코드 판독, 실측 예정]:**
  - 렌더러는 0행 = 위로 쓴다(D3D). Unity RT는 D3D에서 아래 행부터 저장하는 규약이다.
  - 그래서 `command.Blit(output, CameraTarget)`는 위아래를 뒤집어 보였을 것이다.
  - 측정기의 PNG는 스스로 행을 뒤집어 저장해서 바로 보였다.
- **표본 부족 [코드 판독]:** 3840→1280 bilinear Blit은 3×3 텍셀의 가운데 하나만 읽는다. 그래서 화면 픽셀마다 4K 텍셀 하나의 잡음이 그대로 보였다.
- **이중 인코딩 가능성 [예상, 실측 예정]:**
  - 출력(RGB10A2 UNORM)은 이미 sRGB OETF 값이다. 프로젝트는 Linear 색공간이고, 백버퍼는 쓸 때 인코딩한다.
  - 그러면 예전 Blit은 한 번 더 인코딩했을 수 있다(들뜬 색).
- **정지 물체가 매 프레임 "이동" [코드 판독]:**
  - 어댑터는 보이는 인스턴스 전부의 변환을 매 프레임 보내고, `GpuScene::updateTransforms`는 갱신마다 `transformRevision`을 올린다.
  - 그래서 정지 물체가 매 프레임 국소광 VSM 재목록(`VsmMoved` bit 31)에 올랐고, 정적 TLAS 키(`RayScene::staticKey`)도 매 프레임 바뀌었다.
  - 보간도 두 tick의 root가 같아도 쿼터니언 왕복으로 ulp만큼 다른 행렬을 냈다. 게다가 스케일을 버렸다.

수정:
- **호스트 (UnravelNext 5ab9478) [실측]:**
  - `beginFrame`이 GPU 장면이 가진 값과 비트 단위로 같은 변환·포즈 갱신을 버린다.
  - 정착 규칙상 결과는 같다(prev = current). 제자리 teleport도 같다.
  - HostFrameFlags 8/8. 정지 3프레임은 revision·움직임 없음, 이어진 실제 이동은 revision +1. HostAbi 통과.
- **표시 (브리지, stage → 다음 Unity 차례):**
  - `Runtime/Resources/UnravelNextPresent.shader` + `UnravelNextPresent.Record`. 파이프라인과 HostBoundaryProbe가 같이 쓴다.
  - 대상 픽셀마다 출력 텍셀의 정확한 면적 평균을 선형광으로 낸다(정수 비율 = 텍셀 상자, 그 밖 = 걸친 면적 가중). 확대는 픽셀 상자다.
  - 행 순서를 바로잡는다.
  - 대상이 인코딩하면(Linear 색공간의 카메라 대상이나 sRGB 텍스처) 선형 값을, 아니면 인코딩된 값을 쓴다.
  - CPU 에뮬레이션 [실측]: 3840→1280·2560→1280·동일 크기에서 정확한 면적 적분과 차이 0, 3840→1366에서 2e-5(float 반올림). FXC ps/vs_5_0 컴파일 통과.
  - `-unxPresentLegacy`(진단용)는 같은 빌드에서 예전 Blit을 보인다.
- **화면 검사 (측정기):**
  - 측정마다 `ScreenCapture.CaptureScreenshotAsTexture`로 실제 백버퍼를 `*_screen.png`로 저장한다.
  - 출력 되읽기와 64×36 셀 평균으로 비교해 네 가설(바로/뒤집힘 × 한 번/두 번 인코딩)의 MAD와 판정을 결과 JSON `screen`에 넣는다.
- **이력 연속성 (측정기 `history`):** 프레임당 바뀐 변환 수, 그중 정지 몸체 수(0이어야 한다), Restore/Cut 표시 수, teleport 수, 카메라가 바뀐 프레임 수.
- **어댑터 보간:** 두 tick의 root가 같으면 그대로 넘긴다(비트 동일). 아니면 TRS로 분해해 이동·스케일은 double로 선형 보간하고 회전은 slerp한다.
- **불연속 연결 (World 252202ff, stage; World가 새 NativeWorld.dll 설치를 알린 뒤 배포):**
  - `StateGeneration`이 바뀐 첫 프레임에 Restore 비트를 켠다. previous 발행의 세대가 다르면 그 프레임은 보간하지 않는다.
  - `CollectMotionBreaks(previous.Tick)`에 나온 인스턴스는 보간하지 않는다.
  - `CollectMotionBreaks(마지막 렌더 tick)`에 나온 인스턴스는 teleport 플래그를 받는다.

**실측 (2026-09-26 0:41~0:52).** Player 조건: 렌더러 e70384f, 브리지 ca392d1a, NativeWorld 367aeb76, NativePhysics np-p52g. 창 1280×720. 결과는 `Results/I/DataWorld/player_*_e70384f_*`.
- **화면 검사:**
  - 예전 Blit(`present_legacy_…_004151`)은 **뒤집힘 + sRGB 이중 인코딩**이었다. 셀 MAD 0.0046이고, 바로·이중 0.087, 한 번 인코딩 0.266이다. 4K와 1440p 출력이 같았다. 사용자 보고의 원인이 확인됐다.
  - 새 표시 경로(`present_area2_…_005046`)는 **바로 + 한 번 인코딩**이다. MAD는 4K 출력에서 0.0004, 1440p에서 0.0005로 8-bit 한 단계(0.0039)보다 작다. `*_screen.png`가 실제 백버퍼다.
  - 무효 실행 하나: 첫 판(`present_area_…_004406`)은 material Blit의 사각형이 이 SRP에서 그려지지 않아 화면이 검었다(MAD가 모든 가설에서 0.357). 화면 검사가 잡았다. 표시를 SV_VertexID 삼각형으로 바꿔(`SV_Position`에서 발자국, 텍스처 대상이면 `_ProjectionParams.x < 0`으로 행 반전) 다시 쟀다.
  - 편집기 Game 뷰(텍스처 대상)의 행 순서는 아직 실측하지 않았다.
- **history (area2):**
  - 바뀐 변환은 프레임당 중앙값 10, 최대 15(움직이는 몸체)다.
  - **두 프레임 연속 정지한 몸체의 변환 변화 0**. 막 멈춘 몸체는 보간값에서 정착값으로 한 번 바뀌는 것이 맞으므로 세지 않는다.
  - Restore·Cut·teleport 0, 카메라가 바뀐 프레임 0/863.
- **GPU 전후 (중앙값):**
  - 4K 4.11 → 4.24 ms, 1440p 2.22 → 2.19 ms.
  - 트랙별 패스 합은 그대로다(4K M 1.611→1.604, R 0.875→0.878, S 1.192→1.192, V 0.163→0.163).
  - 4K +0.13 ms는 측정된 패스 바깥이다(프레임 구간 − 패스 합 0.27 → 0.40 ms). 같은 빌드의 예전 Blit 실행도 4.24 ms라 I 변경 때문이 아니다. 원인(f8d09d3→e70384f 사이 변경이나 클록)은 열려 있다.
- **일괄 Present:** 캐릭터별 `AcquirePose` 경로와 최대 1.2e-7(관절 1,198개), 6.0e-8(1024+256 규모, 관절 61,338개)만큼 다르다(`-unxPoseCheck 120`).
- **설계 규모(`scale_present_…_004646`):**
  - 호스트 동기화 중앙값 0.599 ms(P95 1.10)로 전의 0.735 ms보다 줄었다. 그중 Present 0.275, 포즈 넘김 0.034 ms다.
  - GPU 4K 5.42 ms, 1440p 2.97 ms.
- R 통계(`rstats_…_004825`)는 `-unxLogFile` 없이 돌아 통계 줄이 없다(통계는 렌더러 로그 파일에만 나온다). GPU 4K 4.25 ms로 측정값 자체는 유효하다. 다음 차례에 로그를 붙여 다시 돈다.

4K 원본의 반점 자체는 R의 hit 셰이딩 분산 문제로 남고, 판단은 `-unxCaptureFull` 원본으로 한다(`present_area2_…_005046_3840x2160.png`).

### 2.2.6 2026-09-26 새벽 차례 (렌더러 583a2eb, 브리지 1dfdc44c) [실측]

Player 조건: NativeWorld 82bdc089(71fea21d + b943ccee), NativePhysics 1F6EF281(np-p52g), NativeVfx 04e44d9e. 식별 정보는 `Results/I/DataWorld/*_583a2eb_*`의 identity 파일에 있다.
- **편집기 Game 뷰 방향**(텍스처 대상, `editor_orient_583a2eb_*`): 바로 보이고 한 번 인코딩된다. MAD 0.00046이다. 표시 셰이더의 `_ProjectionParams.x < 0` 행 뒤집기가 편집기에서도 맞다.
- **호스트 통합 틈 ≈ 0** (설계 2.13 항, 목표 ≤ 0.10 ms): core의 목록 마크(v1.39)와 호스트 그래프 통계 v2(583a2eb)로 쟀다.
  - Player 4K·1440p에서 명령 목록 1개, 교차 큐 동기화 0, graphics head 0 / tail 0(P95 0.001) / gap 0 ms, 경합 0 s였다.
  - 앞서 보고한 "프레임 − 패스 합 0.27~0.70 ms"는 틀렸다. 프레임 중앙값에서 패스별 중앙값의 합을 뺀 값이었고, 꼬리가 긴 분포에서 두 값은 같지 않다.
  - 평균으로 보면 모든 실행(f8d09d3, e70384f, 583a2eb, 4K·1440p)에서 프레임 − 패스 합이 +0.0001 ms 이하다. HostDynamic(city_block 1440p)도 목록 1개, 틈 ≈ 0이다.
- **설계 규모 동기화 분해** (`scale_split_583a2eb_*`, 4K 중앙값):
  - Present 0.263 + 스켈레톤 0.031 + 루트 읽기 0.087 + 루트 루프 0.172 + SetTransforms 0.013 = 0.598 ms다.
  - 루트 루프(보간·부위·복제 변환)는 Burst 작업 후보다.
  - GPU는 4K 6.36 / 1440p 3.36 ms다.
- **복원 검사** (`-unxRestoreCheck`): 절차는 A 복원 → A2 연속 복원 → K2 → B 60 tick → C 복원이다. 판정 기준은 바닥 = 연속 복원 두 번, C ≤ 2 × 바닥, B > 4 × 바닥, Restore 표시 3회다.
  - **데이터 수준 통과** (`restore_check_583a2eb_20260926_050034`): committed World 루트(항목 22개)가 A = A2 = C다(0개 다름). B는 17개가 최대 0.64 m 움직였다. Restore 표시 3회가 닿았다.
  - 영상 수준은 아직 판정하지 못했다. 사용자가 데스크톱을 쓰는 동안 Player 창이 가려지면 Unity가 렌더를 건너뛴다(`runInBackground`와 무관). 그래서 모든 캡처가 같은 옛 프레임이었다.
  - 첫 실행(04:48)의 "C에서 강체 자세가 다르다"도 같은 원인으로 무효 처리했다. 이제 측정기는 10 s 동안 렌더러 프레임이 없으면 오류로 멈춘다.
- **사고**: Player tick 예외 "Animation origin must be rebased before root precision exceeds one millimetre"(시작 약 140 s 뒤)가 나서 호스트가 World tick을 멈췄다.
  - 애니메이션 몫이다. 자동 원점 재기준을 설계 중이다.
  - 몸체가 12 × 12 m 바닥을 벗어났는지 확인하려고, 다음 차례에 `-unxRootLog`(루트 초당 기록 200 s, 기본과 `NP_JOLT_NARROWPHASE=1`)로 잰다.
- f8d09d3 / e70384f A B A B는 첫 실행이 가려진 창 때문에 멈춰서 취소했다. 틈이 착시로 판정됐으니 두 렌더러 중앙값 차이(4.11 → 4.24 ms)만 남아 있고, 창을 앞에 둘 수 있을 때 잰다.

## 2.3 실행 절차 (재현)

```text
1. build:   powershell -File Tools/CI/Build.ps1 -Track I -Tracks "V;M;S;R;C;I"         (UnravelNext.dll, 커널, 시험·게이트)
            build/I는 항상 이 트랙 집합으로 구성한다. 같은 폴더에서 -Tracks를 바꿔 재구성하면 CMake가 대상마다
            CXXDependInfo.json을 다시 쓰고, Ninja가 dyndep 단언(edge && !edge->outputs_ready())으로 멈춘다(2026-09-25 [실측]).
            그때는 build/I를 지우고 새로 빌드한다(111 s). Build.ps1은 이제 트랙 집합이 바뀌면 폴더를 스스로 지운다.
            -Tracks를 빠뜨리면(-Track I 기본 V;M;S;R;I) 폴더가 지워지고 처음부터 빌드된다(-Jobs 4에서 438 s, 2026-09-26).
   측정용:  powershell -File Tools/CI/Build.ps1 -Track all -Committed                  (커밋 기준, ..\UnravelNext-gate\build\all:
            다른 트랙의 작업 중 파일에 영향받지 않는다. INTERFACES v1.12 3.5)
2. deploy:  powershell -File Native/Host/Deploy.ps1 [-Build ..\UnravelNext-gate\build\all] (Unity가 DLL을 싣고 있으면 거부;
            품질 파일은 그 빌드의 원본 트리에서 가져간다)
3. Unity:   감사 세션에 알리고, tasklist로 Unity.exe가 없는지 확인한 뒤
            Unity.exe -batchmode -quit -projectPath C:\Users\USER\Unravel -buildTarget Win64
                      -executeMethod UnravelNextBridge.DataWorld.Editor.UnravelNextDataWorldScene.BuildPlayer
            (원본 NativeDataWorld.unity는 두고 Assets/UnravelNextBridge/DataWorld/NativeDataWorld_UnravelNext.unity 사본 + Player)
4. 측정:   GpuLock.ps1 -Track I -- powershell -NoProfile -Command "Start-Process -Wait <Player>.exe
            '-screen-fullscreen 0 -screen-width 1280 -screen-height 720 -unxMeasureOut <json> -unxMeasureFrames 600'"
            (GUI exe라 Start-Process -Wait로 잠금을 실행 내내 잡는다)
            추가 인자: -unxExportScene <file.unxscene>(첫 동기화 뒤 현재 장면 저장), -unxScale 1024,256(설계 규모 부하 복제:
            실제 개체의 committed 루트·포즈를 읽는 인스턴스를 격자에 더한다)
   검사:   unx_gate_host_hostscene.exe --scene <file.unxscene> --describe      (CPU만: 태양·재질·메시별 NaN·감김·LOD 절단·RT 프록시·인스턴스)
   잠금 종류(v1.28): 정확성 시험(hostswitch, hostdeviceremoved, HostAbi)은 GpuLock.ps1 -Track I -Kind correctness -- ...,
            게이트·Player 측정은 기본(-Kind timing). 하드웨어 GPU 실행은 모두 잠금 안에서 하나씩(임시 규칙).
            unx_gate_host_hostdynamic.exe --save-scene <file.unxscene>          (게이트 장면 내용 저장, 잠금 없음; 디바이스는 만든다)
5. 편집기: Unity.exe -projectPath ... -executeMethod UnravelNextBridge.DataWorld.Editor.UnravelNextEditorRun.Play -unxMeasureOut <json>
            (창 있는 편집기, Play 모드에서 같은 측정을 하고 스스로 종료)
```

## 2.4 남은 일 (I)

- 4K 규모 실행의 Unity 프레임 주기(13.4 ms) 원인을 프로파일러로 찾는다.
- 루트 동기화의 네이티브 경로(`nw_snapshot_read_components` + 네이티브 보간), 포즈는 `na_present_batch`가 생기면 연결한다.
- 바람 해제 패치(`UnxFrameSetEnvironment`의 바람 거부 제거와 시험 전환)는 준비됐고 S의 페이지 메타 완료 알림을 기다린다.
- 재질 v2(clearcoat·박막)가 코어에 들어오면 `UnxMaterialDesc` v2와 데이터 월드 변환.
- 규모: C# 어댑터는 인스턴스·관절마다 관리 코드로 보간·복사한다. RPP 부하(캐릭터 256, 강체 1,024)에서 메인 스레드 비용을 재고
  (`hostSynchronizeMs`), 렌더 제출 1 ms 할당을 넘으면 네이티브 어댑터로 옮긴다. NativeWorld의 `nw_snapshot_previous`와
  `nw_snapshot_read_components`, Animation 포즈 리스를 C++에서 직접 읽는 방식이다.
- 표시 경로가 없는 것: 소프트 바디(tick마다 변형되는 정점 → GpuScene의 동적 정점 API 필요), Matter·VFX(FX 트랙), 파괴 조각(적재 뒤
  인스턴스 추가 → 용량 있는 인스턴스 풀 필요), 개체에 붙은 광원(프레임별 광원 갱신 API 필요). 필요할 때 코어에 요청한다.

## 3. 요청

- `20260925_I_host_module.md`: A(등록)는 반영됐다(코어 3b0c049, v1.6). B(GpuScene 프레임 갱신)도 반영됐다(6af7de8, v1.8).
- `20260925_I_unity_queue_device.md`: Unity 디바이스·큐 위의 `Device`와 실행 훅이 반영됐다(2238b68, v1.9).
- `20260925_I_material_layers.md`(대기): clearcoat와 박막 간섭. 데이터 월드 재질 1(모든 동적 상자와 캐릭터)이 둘 다 쓴다.
  반영 전까지는 기저 층만 옮기고 로그에 남긴다. 측정 결과에도 이 차이를 함께 적는다.
  코어 설계 `Docs/Design/MATERIAL_LAYERS_KO.md`(740e80b)를 검토해 알렸다: 데이터 월드 재질 1의 값(clearcoat 1 / 0.12, 막 420 nm·1.4·덮임 0.35,
  기판 1.5 / 2)은 모두 담긴다. 이전 엔진은 기판 η·κ가 RGB라 float3를 제안했다(금속 기판의 색). 이전 엔진은 막 바깥을 늘 공기로 봤고 RGB 세 대역
  평균 Airy를 썼으므로 외형이 달라질 수 있다(정의는 설계대로). 설계 밖 이전 기능(유전체 경계 막, sheen, 이방성, 디테일 층)은 기록만 했다.
  I 몫: `UnxMaterialDesc` version 2(필드 추가, v1 152 B도 받음), `.unxscene` v2 적재·내보내기.
- `20260925_I_wind_change.md`(반영): 코어 v1.23(bf897d9), S의 페이지 규칙(그린 순간의 바람을 페이지 메타에 기록, `windChangeBound` 판정)이 들어간 뒤 `UnxFrameSetEnvironment`의 바람 거부를 풀었다. 이제 바람 속도 >= 0과 단위 방향만 검사한다. `unx_test_host_hostabi --live`[실측, 커밋 빌드 27edf60, GpuLock]: 바꾼 바람이 현재 장면 저장에 최신 값으로 들어가고 음수 속도는 거부된다.
  S 검토(bbf2575, `20260925_S_wind_change_review.md`): 구조에 동의한다. windOffset이 무기억 상태 함수라 페이지를 그린 순간과 지금의
  바람 끝점만 비교하면 되고(P3 모델도 무기억이어야 한다는 조건을 계약에 넣자는 제안), revision 없이 페이지 메타에 그 순간의 바람을 저장한다.
  상한은 I 초안보다 좁은 K·[s1²·0.4·min(2, 1.7|Δt|) + |s1² − s0²| + 2 sin(Δθ/2)·s0²]를 제안했다. R도 같은 의견이다(revision 불필요;
  바람 잎 정확 집합은 매 프레임 현재 바람으로 변형하고, refit을 건너뛸 때만 같은 끝점 상한을 쓴다). 합의: windRevision 없이 끝점 상한
  `windChangeBound`와 windOffset 무기억 계약. 코어가 INTERFACES를 올리면 커밋 뒤 바람 변경 거부를 푼다.
- `20260925_I_device_removed_policy.md`(반영, 코어 67b5508 v1.27): 플러그인이 `UnityPluginLoad`에서 `DeviceRemovedPolicy::Throw`를 둔다.
  ABI 5: `DeviceRemovedError`는 `UNX_DEVICE_REMOVED`(−4)가 되고, 그 뒤 모든 렌더러 호출이 같은 코드를 돌려준다(파괴는 된다). 렌더 이벤트는
  제거 뒤 아무것도 하지 않는다. 시험 `unx_test_host_hostdeviceremoved`(RemoveDevice로 이 프로세스의 장치만 제거; TDR 아님)[실측, 커밋 빌드
  ff07562, GpuLock]: 제거 뒤 다음 프레임이 `DeviceRemovedError`("map VSM stats", hr 0x887A0005)를 던지고, `Queue::waitCpu`는 제거를 기록하고
  돌아오며, 렌더러 파괴가 끝나고 프로세스는 0으로 끝난다(87이 아니다). 통과. 같은 빌드에서 `unx_test_host_hostabi --live`(ABI 5)도 통과했다.
  **한계:** 장치 제거 뒤 다시 적재하지 않는다. C#은 렌더러를 버리고 배경만 지우며, 그 프로세스에서는 다시 만들지 않는다. 코어의
  `deviceWasRemoved()`가 프로세스 전역이고 되돌릴 수 없어서, Unity가 새 장치를 만들어도 모든 호출이 `UNX_DEVICE_REMOVED`를 돌려준다.
  13:56에는 Unity 자신도 D3D12 제거를 복구 불가로 처리했다. 복구(드라이버 재설정·TDR 뒤 새 장치로 다시 적재)는 재구축 계획 13.6
  P10 제품화의 "TDR·장치 제거 복구" 항목이다. 그때 코어에 장치별 제거 상태를 요청한다.
- 알려진 실패(VFX 세션, V3까지, 회귀 아님): NativeVfx V2가 설치되면 `NativeDataWorldPlayerValidation`의 "Data VFX did not execute on the shared
  GPU" 단언(EnableVfxGpu = true)이 실패한다. TitanNative 공유 GPU VFX 실행기가 퇴역했고, GPU 입자 모듈 연결은 V3(I가 FX 모듈을
  `VfxStreamExecutors.Provider` / `NV_StreamExecutor`로 잇는 일)다. EnableVfxGpu = false(`-unxVfxCpu`) 조건의 Player에는 영향이 없다.
  설치 뒤 TitanNative VFX 렌더러는 CPU 투영으로 그린다. `Docs/Rebuild/WORLD_VFX_DESIGN_KO.md` 9.4(a81466e5).
- `20260925_I_history_discontinuity.md`(반영, 코어 v1.35 8636d11; I ABI 6 b7d93ed): `UnxFrameSetDiscontinuity`(RESTORE | CUT; 두 비트 모두 이전 뷰 없음),
  `UnxFrameSetSimulation`(GPU 시뮬 비트, 지금은 0), `UnxTransformUpdate::flags`의 `UNX_TRANSFORM_TELEPORT`. 떨어진 패킷의 비트는 다음 것으로 OR.
  `unx_test_host_hostframeflags` 통과(커밋 빌드 b7d93ed). 어댑터 연결은 World의 StateGeneration·teleport 표시를 기다린다. 원래 설명: World 복원·카메라 컷의 렌더 이력 계약. 호스트가 복원을 감지해(NW_Info epoch·branch, tick 역행)
  불연속 비트를 넘기고, 렌더러는 모든 시간 상태를 재설정한다. 같음의 수준을 정해야 한다: 확률 항을 뺀 부분집합은 비트 동일,
  전체는 (i) 결정적 누적(I 권장) 또는 (ii) 실측 바닥 이하. 옛 시험 `WorldHierarchySkinAndVfxReachRealRenderedPixelsAcrossRestore`를 이 경로로 옮긴다.
- `20260925_I_skin_normals.md`(반영, INTERFACES `skin()`의 `cofactorNormal`, 단위 테스트 `skin_normals_use_the_cofactor`; e70384f Player에 들어 있다):
  스킨 법선을 관절 3×3의 여인수 × sign(det)으로 변환한다. 데이터 월드 캐릭터의 비균일 스케일(0.6, 0.8, 0.6)을 관절에 접을 때 필요했다.
- `20260925_I_history_discontinuity.md`의 어댑터 연결(반영, 브리지 ca392d1a): World 252202ff의 `StateGeneration`이 바뀌면 Restore 비트를 켜고,
  previous 발행의 세대가 다르거나 previous가 없으면 보간하지 않는다(복원·초기화·구조 commit 직후 previous = null). `CollectMotionBreaks(previous.Tick)`에
  나온 인스턴스는 보간하지 않고, `CollectMotionBreaks(마지막 렌더 tick)`에 나온 것은 teleport 플래그를 받는다. 복원 검사(아래 2.4)가 이 경로를 잰다.
- `20260926_I_profiler_gaps.md`(대기, core): `FrameTiming`에 큐별 `headMs`·`tailMs`·`gapMs`·`listBoundaries`와 contended 프레임 검출을 더해 달라는 요청이다.
  Player의 "프레임 구간 − 패스 합"(4K 0.27 → 0.40 ms)을 설계 2.13의 **호스트 통합 틈** 항(목표 ≤ 0.10 ms)으로 셋으로 나눠 재려는 것이다:
  렌더러 목록 경계 수, 경계당 ms, Unity 큐 작업 ms.
  - 호스트는 먼저 `UnxFrameGraphStatsLatest`(0eb268d, ABI 6 안의 선택 export; 브리지가 있는지 보고 쓴다)를 더했다.
  - HostAbi 1440p 독립 실행 [실측]: 패스 124개, 명령 목록 1개, 배리어 600개/115묶음, 교차 큐 동기화 0.
  - Player도 목록이 하나면, 그 틈은 목록 사이가 아니라 머리·꼬리거나 Unity 쪽이다.
- **V3 GPU 입자 연결(설계 결정, WORLD_VFX 1·3.3·3.7):**
  - 문제: Unity에서 VFX tick의 submit(nv_commit)은 메인 스레드이고, FX tick 기록은 렌더 스레드의 프레임 C0이다. 한 프레임에 고정 스텝이 둘이면 다음 tick의 prepare가 아직 기록되지 않은 tick의 되읽기를 막고 기다려 교착한다(I 발견).
  - 결정: (b) 항상 호스트 소유 시뮬레이션 큐, 기본 대안은 (f) 하이브리드다. 평소 tick은 C0이다. 히치 때만 메인 스레드가 그 tick을 claim해 호스트 소유 큐(compute 또는 호스트 direct)에 즉시 제출하고, 렌더 이벤트에서 Unity 큐에 queue->Wait를 넣는다.
  - 프레임의 그래픽스 큐는 Unity 소유라 메인 스레드에서 제출할 수 없다. 렌더 스레드는 메인을 기다리지 않으니 교착이 없다.
  - 나눔:
    - FX: 프레임 밖 `record(RenderGraph&, ShaderLibrary&, tickIndex, QueueType)`, 렌더가 읽는 tick 버퍼 링 ≥ 3, tick별 claim 상태 기계, 스트림마다 인스턴스.
    - core: 외부 디바이스 위의 고우선 compute 큐.
    - I: `NV_StreamExecutor` ABI, 링 fence 전달, 렌더 이벤트 Wait, fence 대기 비용 실측.
  - 구현 뒤 A/B: ① C0, ② (b), ③ (f) + 강제 2스텝. 항목은 GPU 프레임 중앙값·P95, 패스 합, 패스 바깥 틈, soft 겹침이다(조율 조건).

## 3.1 호스트 쪽 설계 조건 (날씨·시간대)

- **바람 모델은 무기억 상태 함수여야 한다**(P3 날씨 설계의 조건): 변위 windOffset은 (시각, 그 시각의 바람 상태: 방향·속도·돌풍 파라미터)만의
  함수여야 하고 과거 이력(적분된 위상, 관성 상태 등)에 기대면 안 된다. S의 페이지 판정과 R의 refit 생략이 "그린 순간의 바람"과 "지금의
  바람" 두 끝점만 비교하기 때문이다(`windChangeBound`). 돌풍·방향 회전처럼 이력이 있어 보이는 효과는 호스트가 바람 상태를 시각의 함수로
  내보내는 방식으로 만든다(예: 결정적 잡음을 시각으로 표본화). 합의 경위: `Requests/20260925_I_wind_change.md`,
  `20260925_S_wind_change_review.md`, R 의견(조율 세션 전달).
- 태양·대기는 프레임마다 바꿀 수 있다(`UnxFrameSetEnvironment`). 게임 하루 속도는 제한이 없다(사용자 결정, 2026-09-25): API에 속도 상한을 두지 않는다(태양 단위 방향, 바람 속도 >= 0만 검사). 대기 파라미터 변경은 다중산란 LUT 재생성(S: 텍셀당 256스레드 구조)을
  부르므로 날씨 전환은 대기를 매 프레임이 아니라 필요한 만큼만 바꾸는 쪽이 싸다. 비용은 게임 뒤 잰다.

## 4. 이전 저장소 변경

- `Assets/UnravelNextBridge/` 새 폴더만. `Assets/TitanSRP`는 고치지 않았다. 프로브는 실행 중에 `GraphicsSettings.defaultRenderPipeline`을
  바꾸므로 ProjectSettings는 그대로다.
