# I 트랙 상태 (통합: Unity 호스트 ↔ 새 렌더러) — 2026-09-25

표기: [실측] = 이 기계(i9-13900KF, RTX 4080, 드라이버 591.86, 모니터 2560×1440 143 Hz)에서 실행한 결과, [예상] = 비용식·가정.
빌드: `Tools/CI/Build.ps1 -Track I` (build/I, 트랙 V;M;S;R;I). 성능 측정은 `Tools/CI/GpuLock.ps1 -Track I` 아래에서만.
소유: UnravelNext `Native/Host/`, `Config/quality/host.toml`, 이 문서, `Results/I/` / 이전 저장소 `Assets/UnravelNextBridge/`.

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
Player 빌드(Mono, 창 1280×720, vsync 끔), 4K·1440p, 600프레임 × 3회(회차마다 순서 반대), GpuLock. 원본: `Results/I/HostBoundary/unity_player_20260925_093059.json`.
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

## 2. C ABI (`Native/Host/include/unx/host/UnravelNextHost.h`)

- v1: 버전·크기 필드가 앞에 있는 고정 크기 구조체만, 반환은 결과 코드 + `UnxLastError()`. 지금은 프로브 부분만 있다.
- 다음: 렌더러 생성·파괴, 장면 적재(텍스처·재질(INTERFACES 8.1)·메시·스켈레톤·인스턴스·광원·태양/대기 → `scene::Scene` → V 클러스터
  빌더 → `GpuScene`), 프레임 갱신(카메라, 태양), 렌더 이벤트(Unity 출력 텍스처로). 프레임 변환·본 갱신은 코어의 GpuScene 갱신 API(요청 B,
  코어 대기열) 뒤.
- World 어댑터 방향: 이전 World가 표시 보간용 직전 committed 스냅숏(`nw_snapshot_previous`)과 형식별 수집(`nw_snapshot_read_components`:
  `NW_COMPONENT_WORLD_AFFINE`, `NW_COMPONENT_SKIN_MATRIX`)을 준다. 호스트가 이것을 네이티브에서 직접 읽어 두 tick 사이를 보간하고(권위 상태에는
  쓰지 않는다), C#은 프레임마다 World 핸들·렌더 시각·카메라만 넘긴다.
  - 주의: `NW_Affine`은 비균일 스케일·전단을 담을 수 있다. 렌더러 인스턴스 변환은 회전·균일 스케일·이동만 허용하므로(INTERFACES 6.1),
    적재 때 비균일 스케일을 메시에 굽는다(정적). 스킨은 인스턴스 변환 = 캐릭터 루트, 팔레트 = 루트⁻¹ × SKIN_MATRIX.

## 3. 요청

- `Docs/Design/Requests/20260925_I_host_module.md`: A(등록) 반영됨(코어 3b0c049, v1.6). B(GpuScene 프레임 갱신)는 코어 대기열
  (`prevObjectToWorld` = 직전 렌더 프레임은 반영됨).

## 4. 이전 저장소 변경

- `Assets/UnravelNextBridge/` 새 폴더만. `Assets/TitanSRP`는 고치지 않았다. 프로브는 실행 중에 `GraphicsSettings.defaultRenderPipeline`을
  바꾸므로 ProjectSettings는 그대로다.
