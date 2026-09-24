# 요청: I(통합) 트랙과 `Native/Host` 모듈 등록, GpuScene 프레임 갱신 (I 트랙, 2026-09-25)

## 왜 필요한가

REBUILD_PLAN 14.3 ⑦로 I(통합) 트랙이 시작됐다. I는 새 렌더러를 이전 엔진(Unity 호스트, World, 애니메이션, 물리)에 붙인다.
소유(쓰기): `Native/Host/`, `Config/quality/host.toml`, `Docs/Status/I_STATUS_KO.md`, `Results/I/`.
이전 저장소 쪽은 `Assets/UnravelNextBridge/`다.

`Native/Host`는 C ABI DLL `UnravelNext.dll`(Unity 네이티브 플러그인)과 그 테스트·게이트를 만든다. 지금은 빌드에 등록되지
않아서 I는 코드만 쓰고 있다. 등록돼야 빌드·정확성 실행·호스트 경계 측정(설계서 7.1-5, 4.3 끝줄)을 할 수 있다.

## 원하는 변경 A — 빌드 등록 (급함, I를 막고 있음)

1. **`cmake/Tracks.cmake`**: `set(UNX_ALL_TRACKS V M S R C I)`, `set(UNX_TRACK_OF_Host I)`.
2. **최상위 `CMakeLists.txt`**: `add_subdirectory(Native/Render)` 뒤(= `unx_renderer`·`unx_add_executable`이 정의된 뒤)에
   ```cmake
   unx_folder_enabled(Host host_on)
   if(host_on AND EXISTS "${CMAKE_SOURCE_DIR}/Native/Host/CMakeLists.txt")
     add_subdirectory(Native/Host)
   endif()
   ```
   `Native/Host/CMakeLists.txt`는 I가 소유한다(`Tools/<이름>/CMakeLists.txt`를 그 폴더 트랙이 고치는 것과 같은 방식).
   I는 그 안에서 공유 라이브러리 `unx_host`(OUTPUT_NAME `UnravelNext`, `unx_renderer` 링크)와 `Tests/*.cpp`·`Gates/*.cpp`
   실행 파일(`unx_add_executable` 사용)을 만든다. 공유 파일은 더 고칠 필요가 없다.
3. **`Tools/CI/Build.ps1`**: `-Track I` → `build/I`, 기본 `UNX_TRACKS` = `V;M;S;R;I`.
   호스트 DLL은 렌더러 전체를 링크해야 실제 프레임을 그린다. C는 필요 없다. 다른 트랙의 작성 중 파일이 빌드를 깨면 I는
   `-Tracks "I"`(나머지는 코어 빈 진입점)로 호스트·ABI만 계속 빌드한다.
4. **INTERFACES 1절 표**: `I 통합 | I 세션 | Native/Host/, Config/quality/host.toml` 행. 2.5절의 트랙 목록에 I를 추가한다.

참고: DLL은 `D3D12SDKVersion`를 export해도 효과가 없다(Agility 런타임은 exe의 export만 본다). Unity 6000.6의 에디터
(`Unity.exe`)와 Player(`WindowsPlayer.exe` 사본)가 모두 `D3D12SDKVersion = 618`, `D3D12SDKPath = .\D3D12\`를 export하고
`D3D12Core.dll` **1.618.1**을 싣는다 [실측, PE export·파일 버전]. UnravelNext는 1.618.5다. SDK 버전(618)이 같으므로 D3D12 API와
기능 집합은 같다. 그래서 `unx_host`는 `unx_agility_exports`를 링크하지 않는다(`unx_add_executable`을 쓰지 않는 DLL 타깃).

## 원하는 변경 B — GpuScene 프레임 갱신 (P6 통합에 필요, B는 A보다 늦어도 된다)

`GpuScene::upload`는 적재 때 한 번뿐이고, 인스턴스 변환·본 팔레트를 프레임마다 바꾸는 경로가 없다. 호스트는 World의 committed
두 tick(T_{n-1}, T_n)을 렌더 시각으로 보간해(설계서 5.2) 매 렌더 프레임에 넣어야 한다.

제안(이름·형태는 코어가 정한다):
```cpp
struct InstanceTransformUpdate { uint32_t instance; float3x4 objectToWorld; };
// 이번 프레임에 바뀐 인스턴스만. 그 인스턴스의 prevObjectToWorld = 직전 렌더 프레임의 objectToWorld,
// transformRevision += 1. 목록에 없는 인스턴스는 prev = current로 맞춘다(한 번 움직인 뒤 멈춘 물체의 움직임 벡터가 0이 되게).
void GpuScene::updateTransforms(uint64_t frameIndex, std::span<const InstanceTransformUpdate>);
// 스킨 인스턴스의 jointToModel(모델 공간 관절). 이전 팔레트 = 직전 렌더 프레임의 팔레트, deformRevision += 1.
void GpuScene::updateSkeleton(uint64_t frameIndex, uint32_t skeleton, std::span<const float3x4> jointToModel);
// 인스턴스 숨김/표시(스폰 풀·파괴). 적재 뒤 인스턴스 추가는 다음 요청에서(용량 있는 풀).
void GpuScene::setInstanceVisible(uint32_t instance, bool visible);
```
- **`prevObjectToWorld`의 뜻**: `GpuSceneLayout.h` 주석은 "previous tick"이지만 소비자는 **직전 렌더 프레임**을 전제한다.
  V의 `CullInstances/CullClusters/CullNodes`는 `prevObjectToWorld`를 `prevViewProj`(직전 프레임)의 HiZ로 투영하고, S의
  `VsmInvalidate`는 직전 위치의 페이지를 표시한다. 렌더 165 fps·tick 60 Hz에서는 두 뜻이 다르므로 주석을 "직전 렌더 프레임"으로
  고정해 주기를 요청한다. 호스트는 그 뜻으로 넣는다.
- 업로드 비용 [예상]: 동적 인스턴스 20k × 48 B + 본 256체 × 100 × 48 B ≈ 2.2 MB/프레임. 프레임별 업로드 링(framesInFlight 슬롯)
  에서 복사 패스 하나(0.01 ms 수준 [예상]). CPU 쪽 보간은 호스트가 한다.
- 태양·하늘: `FrameRenderer`가 매 프레임 `GpuScene::source()->sun`을 읽으므로 호스트는 자기가 가진 `scene::Scene`의 `sun`을 바꾸는
  것으로 충분하다(인터페이스 변경 없음). S가 태양 변경을 매 프레임 비교로 알아낸다는 전제다. 아니면 알려 달라.

## 다음 요청(측정 뒤)

호스트 경계(Unity D3D12 디바이스 + 자체 큐 / Unity 큐 제출 / 전용 디바이스)를 측정으로 정한 뒤, 필요하면 `Device`를 외부
디바이스로 만드는 생성자나 `RenderGraph::execute`의 제출 훅을 별도 요청으로 낸다. A와 B는 어느 방식이든 필요하다.

## 영향

- A: 공유 파일 3개(Tracks.cmake, 최상위 CMakeLists, Build.ps1) 몇 줄. 다른 트랙 빌드는 바뀌지 않는다(I 폴더는 I가 켜졌을 때만 구성).
- B: GpuScene 공개 함수 추가, 주석 한 줄. 트랙 코드는 바뀌지 않는다.
