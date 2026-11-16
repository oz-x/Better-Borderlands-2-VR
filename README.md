# Better Borderlands 2 VR

Mod nativo de VR 6DoF para el **Borderlands 2 normal** (Steam, 32-bit, DirectX 9) con controles
de movimiento: el arma va en tu mano derecha, apuntas y disparas con el mando, te mueves con los
sticks y el HUD y los menús se ven dentro del visor. Como sigue siendo el juego normal, el
**cooperativo online funciona igual**: unos amigos pueden jugar en VR y otros en plano. Hecho para
Meta Quest 3 (Virtual Desktop o SteamVR); usa OpenXR, así que debería valer cualquier visor con un
runtime OpenXR de 32 bits.

No usa UEVR.

## Instalar

1. Descarga `BL2VR.zip` de la [última release](https://github.com/oz-x/Better-Borderlands-2-VR/releases/latest).
2. Descomprímelo y haz doble clic en `instalar.bat`: encuentra el juego (Steam o Epic), pide
   confirmación y copia el mod.
3. Conecta el visor y abre Borderlands 2 desde Steam.

Controles, ajustes y limitaciones: [`dist/LEEME.md`](dist/LEEME.md).

## Compilar

Requisitos: Visual Studio 2022 (C++ x86), CMake ≥ 3.24, Git.

```bash
mkdir third_party && cd third_party
git clone --depth 1 https://github.com/TsudaKageyu/minhook.git
git clone --depth 1 https://github.com/KhronosGroup/OpenXR-SDK.git
cmake -S OpenXR-SDK -B build-openxr -G "Visual Studio 17 2022" -A Win32 -DDYNAMIC_LOADER=ON -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF
cmake --build build-openxr --config Release --target openxr_loader
cd ..
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
bash tools/package.sh   # -> build/BL2VR.zip
```

## Estructura

| Archivo | Papel |
|---|---|
| `src/proxy.cpp`, `src/d3d9.def` | Proxy de `d3d9.dll`: reenvía todo al d3d9 del sistema. |
| `src/d3d.cpp` | Crea el dispositivo del juego con D3D9On12 y engancha Present/EndScene/Reset. |
| `src/stereo.cpp` | Hilo de render: copia el back buffer de cada ojo y lo entrega a OpenXR. |
| `src/xr.cpp` | Sesión OpenXR (D3D12), swapchains, capas de proyección y de pantalla. |
| `src/vr.cpp` | Hilo de juego: doble `FViewport::Draw`, cámara por ojo, apuntado con el mando, brazos y arma en la mano, origen del disparo, FOV del arma. |
| `src/xr_input.cpp` | Acciones OpenXR: poses de los mandos, botones, sticks y vibración. |
| `src/gamepad.cpp` | Mando de Xbox virtual (XInput): los mandos VR se traducen a botones del juego. |
| `src/hud.cpp` | Películas de Scaleform: HUD hacia los paneles VR, menús en la pantalla virtual. |
| `src/wrist.cpp` | HUD en VR: desvía el HUD a una textura propia y lo recoloca en la muñeca izquierda y en un panel central. |
| `src/ue3.cpp` | Reflexión mínima de Unreal Engine 3 (GObjects, GNames, ProcessEvent). |
| `dist/` | Lo que va en el ZIP: instalador (`instalar.ps1` / `instalar.bat`), `bl2vr.ini`, `LEEME.md`. |
| `tools/` | Utilidades de depuración (lanzar el juego, volcar imágenes por ojo, pilas). |

Los patrones de GObjects/GNames/ProcessEvent vienen de
[bl-sdk/unrealsdk](https://github.com/bl-sdk/unrealsdk). El resto de puntos de enganche
(`FViewport::Draw`, la llamada al punto de vista dentro de `ULocalPlayer::CalcSceneView`) se
localizan por firma en tiempo de ejecución.

## Depuración

En `bl2vr.ini`, sección `[Debug]`:

- `Trace=1` activa los disparadores por archivo en `Binaries\Win32`: `bl2vr_dump.txt` (guarda
  `bl2vr_left.bmp`/`bl2vr_right.bmp`), `bl2vr_trace.txt` (traza de llamadas D3D),
  `bl2vr_cmd.txt` (comandos de consola), `bl2vr_objs.txt` (lista objetos de una clase).
- `FakeHeadset=1` (+ `FakeYaw`/`FakePitch`) ejecuta la tubería estéreo sin visor.

## Licencia

[MIT](LICENSE). El ZIP incluye MinHook (BSD-2), el cargador de OpenXR (Apache-2.0) y jsoncpp;
sus avisos van en `TERCEROS.txt`.
