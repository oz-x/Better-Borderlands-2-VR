# Better Borderlands 2 VR (6DoF) para el Borderlands 2 normal

Mod nativo que convierte el **Borderlands 2 normal de Steam** (32-bit, DX9) en VR estereo con
seguimiento de cabeza **6DoF**. Como sigue siendo el juego normal, el **cooperativo online funciona
igual que siempre**: tus amigos pueden jugar en VR (con el mod) o en plano (sin el mod), con todos
los DLC, niveles y partidas que ya tienes.

No usa UEVR ni nada de praydog (UEVR ni siquiera soporta Unreal Engine 3).

## Requisitos

- Windows 10/11 con una GPU compatible con DirectX 12.
- Meta Quest 3 conectado al PC con **Virtual Desktop** (recomendado), **Quest Link / Air Link + SteamVR**,
  o cualquier visor con un runtime **OpenXR de 32 bits** (Virtual Desktop y SteamVR lo traen).
- El runtime OpenXR activo debe ser el que vayas a usar:
  - Virtual Desktop: se configura solo (VDXR).
  - SteamVR: SteamVR > Ajustes > OpenXR > "Establecer SteamVR como runtime OpenXR".

## Instalacion

1. Descomprime el ZIP y haz doble clic en **`instalar.bat`**. El instalador busca Borderlands 2
   (bibliotecas de Steam y Epic), te enseña dónde lo va a instalar y pide confirmación. Si no lo
   encuentra, te pide la ruta. Si ya tenías el mod, lo actualiza y conserva tu `bl2vr.ini`; si
   había otro `d3d9.dll` (p. ej. ReShade), lo guarda como `d3d9.dll.bak-<fecha>`.

   Instalación manual: copia `d3d9.dll`, `openxr_loader.dll` y `bl2vr.ini` en
   `...\steamapps\common\Borderlands 2\Binaries\Win32\`.
2. Conecta el visor (Virtual Desktop conectado / Link activo) **antes** de abrir el juego.
3. Abre Borderlands 2 desde Steam. El juego se pone en ventana con el tamaño adecuado para VR.

Para desinstalar, borra esos tres archivos. El mod pone el juego en ventana con la resolución VR y
el juego la guarda: después de desinstalar, vuelve a elegir tu resolución y pantalla completa en
Opciones > Vídeo.

## Controles (mandos del Quest)

El arma va en tu mano derecha: apuntas y disparas con el mando, como en un juego VR. La cabeza
mira libre y caminas hacia donde miras. El mod presenta los mandos al juego como un mando de
Xbox, así que todo funciona igual en solitario y en cooperativo (y vibran al disparar).

| Mando del Quest | Acción en Borderlands 2 |
|---|---|
| Gatillo derecho | Disparar |
| Gatillo izquierdo | Apuntar / zoom |
| Grip izquierdo | Habilidad de acción (LB) |
| Grip derecho | Granada (RB) |
| A | Saltar |
| B | Agacharse |
| X | Recargar / usar (mantener) |
| Y | Cambiar de arma |
| Stick izquierdo | Moverse (hacia donde miras); pulsado = esprintar |
| Stick derecho izquierda/derecha | Girar el cuerpo (suave; o por saltos con `SnapTurn`); pulsado = cuerpo a cuerpo |
| Stick derecho abajo | Inventario / mapa (Back) |
| Stick derecho arriba | Cruceta arriba |
| Botón ☰ (mando izquierdo) | Pausa |

- En los menús se navega con los sticks y A/B como con un mando de Xbox.
- Si en Virtual Desktop tienes activada la emulación de mando de Xbox, desactívala para evitar
  pulsaciones dobles.
- También funcionan un mando de Xbox/PS o teclado y ratón; en ese caso pon `AimMode=1` (apuntar
  con la cabeza) en `bl2vr.ini`.
- **F6**: recentrar (mira al frente y pulsa). También vale el recentrado del propio visor
  (mantener pulsado el botón Meta del mando).
- **F7**: alternar entre VR estereo y "modo cine" (pantalla virtual grande delante de ti).
- Los menus (pausa, inventario, tiendas, mapa...) aparecen solos en la pantalla virtual.

## Jugar con amigos

Cada amigo que quiera VR instala el mod en su PC. Los que no, juegan normal. No hace falta
nada mas: es el multijugador normal de Borderlands 2 (Steam / SHiFT).

## Ajustes (`bl2vr.ini`)

- `WorldScale`: escala del mundo (100 = tamaño real).
- `AimMode`: 2 = mando derecho (por defecto), 1 = cabeza, 0 = stick/ratón.
- `GunOffsetForward/Right/Up`: mueve el arma dentro de la mano (unidades ~ cm). El mod mide
  donde va el arma en el modelo y la pone en el mando; si la notas desplazada, ajusta esto.
- `TurnSpeed`: velocidad del giro con el stick derecho en grados/segundo (180 por defecto).
- `SnapTurn`: si es mayor que 0, el stick derecho gira a saltos de esos grados (p. ej. 30),
  más cómodo si te mareas.
- `HudScale` / `HudDistance`: tamaño y profundidad del HUD.
- `RenderWidth` / `RenderHeight`: resolucion por ojo (0 = automatica).
- `DesktopMirror=0`: no dibujar en la ventana del PC (algo mas de rendimiento).

## Como funciona (resumen tecnico)

- `d3d9.dll` es un proxy de Direct3D 9: crea el dispositivo del juego sobre Direct3D 12
  (D3D9On12) para poder entregar las imagenes a OpenXR sin copias por CPU.
- Engancha `FViewport::Draw` de Unreal Engine 3 y dibuja **cada fotograma dos veces** (ojo
  izquierdo y derecho), cambiando la camara mediante el punto de vista del `PlayerController`
  con la pose real de cada ojo (posicion + rotacion).
- El FOV del juego se ajusta al del visor y se quita el limite de 62 fps.
- Mandos: OpenXR → mando de Xbox virtual (XInput) para todas las acciones; la rotación del
  mando derecho se convierte en la rotación de apuntado del jugador, los brazos y el arma de
  primera persona se colocan en la pose del mando y los disparos salen desde el mando.
- El HUD de Scaleform se encoge al centro y se le da paralaje para que flote a ~2 m.

## Limitaciones conocidas

- La resolucion maxima por ojo esta limitada por el alto de tu monitor (el juego no permite
  ventanas mas grandes que el escritorio). En un monitor 1440p son ~1330x1380 por ojo.
- Durante las pantallas de carga el visor se queda en el ultimo fotograma / la pantalla de
  espera del runtime.
- No hay controladores de movimiento (manos): se juega con mando o teclado.
- Si la ventana del PC se ve negra, es solo el espejo del escritorio; en el visor se ve bien.
- Si algo falla, mira `Binaries\Win32\bl2vr.log`.
