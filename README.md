# Outlast Online

Mod multijugador (hasta 8 jugadores) para **Outlast** (Steam, versiones de 32 y 64 bits), escrito en C++.
Cada jugador ve a los demás como Miles, con animaciones, dentro de su propio mundo, y puede chatear con ellos.

## Instalación
Descomprime `dist/OutlastOnline.zip` y abre `OutlastOnlineLauncher.exe`. El launcher:
- busca Outlast solo (ruta guardada, bibliotecas de Steam, registro de GOG y Epic, rutas típicas en todas las unidades);
- si no lo encuentra, te deja elegir la carpeta con "Cambiar carpeta...";
- guarda la ruta en `%APPDATA%\OutlastOnline\launcher.ini`;
- instala o actualiza `dinput8.dll` (va dentro del propio .exe) y lanza el juego.

`OutlastOnlineLauncher.exe --setup` no lanza el juego automáticamente.
Instalación manual: copia `manual/dinput8.dll` a `Outlast/Binaries/Win64/`.

## Modo historia cooperativo (sincronización de niveles)
Cada jugador envía su `OLGame.CurrentCheckpointName`. El orden de la historia se lee de `OLCheckpointList` (96 checkpoints en `x_checkpoints`).
Cuando otro jugador está en un checkpoint más avanzado durante 3 s, se llama a `OLPlayerController.StartNewGameAtCheckpoint` para cargarlo.
Nadie retrocede. Se controla con `SyncLevels` y `SyncSaveToDisk` en el ini.

Para desinstalarlo, borra `Binaries/Win64/dinput8.dll`.

## Controles
| Tecla | Acción |
|---|---|
| **F9** | Menú de conexión: nombre, IP y puerto del servidor, **Unirse** u **Hospedar** |
| F8 | Hospedar rápidamente con el puerto configurado |
| F10 | Desconectar |
| F11 | Teletransportarte junto a otro jugador (si estáis en el mismo mapa) |
| T | Chat (Enter envía, Esc cancela) |
| M | Micrófono ON/OFF (chat de voz por proximidad; icono abajo a la izquierda) |
| F7 | Mostrar/ocultar el overlay |

En el menú: flechas/Tab para moverte, Enter para aceptar y Esc para cerrar. La IP y el puerto se guardan en `OutlastOnline.ini`.

## Jugar por Internet
* El host necesita abrir o redirigir el **puerto UDP** (7777 por defecto) en su router y permitir `OLGame.exe` en el firewall de Windows.
* Si no puedes abrir puertos, usa una VPN LAN (Radmin VPN, ZeroTier, Tailscale o Hamachi) y conéctate a la IP que te dé.
* Todos deben estar en el **mismo mapa** para verse. Si estáis en mapas distintos, el overlay muestra en cuál está cada uno.

## Configuración (`Binaries/Win64/OutlastOnline.ini`)
Se crea sola la primera vez que arrancas el juego.
* `Name`, `HostIP`, `Port`: también se pueden cambiar desde el menú F9.
* `AutoStart`: 0 = manual, 1 = hospedar al arrancar, 2 = unirse al arrancar.
* `AvatarMode`: `pawn` (por defecto: el otro jugador es un OLHero con animaciones) o `mesh` (solo el modelo, sin animaciones).
* `AvatarMesh`: ruta opcional de un SkeletalMesh para el avatar.
* `SendRate`: paquetes por segundo (5–60). `Debug=1` vuelca las propiedades del avatar al log.

El registro de ejecución se guarda en `Binaries/Win64/OutlastOnline.log`.

## Compilar
Necesitas Visual Studio 2022 o 2026 (C++) y CMake. `build_all.bat` compila las dos arquitecturas y genera el zip:
- `build32/`: `dinput8.dll` de 32 bits.
- `build/`: `dinput8.dll` de 64 bits, el launcher con las dos DLL incrustadas, `fakepeer` y `voicetest`.

A mano:
```
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
cmake --build build --config Release --target install_mod   # copia la DLL al juego
cmake --build build --config Release --target package_zip   # genera dist/OutlastOnline.zip
```

## Probar sin un segundo PC
`fakepeer.exe` simula un jugador "Bot" que camina alrededor tuyo y envía mensajes de chat:
```
fakepeer join 127.0.0.1 7777    # en el juego: F8 para hospedar
fakepeer host 7777              # en el juego: F9 -> IP 127.0.0.1 -> Unirse
```
Variables de entorno: `FAKEPEER_FRONT=1` (se queda quieto delante de ti), `FAKEPEER_FOLLOW=1` (te sigue), `FAKEPEER_SECONDS=N`.

## Cómo funciona
* **`dinput8.dll` proxy** (`src/dllmain.cpp`): el juego importa `DirectInput8Create`, así que Windows carga esta DLL. Las llamadas se reenvían a la DLL del sistema.
* **Hook de `UObject::ProcessEvent`** (`src/hook.cpp`, `mod.cpp`): un detour en la RVA `0x71E50` de `OLGame.exe`, con búsqueda por patrón como respaldo. Se usa `PlayerTick` para la lógica y `HUD.PostRender` para dibujar.
* **Reflexión UE3** (`src/ue3.cpp`): localiza GNames y GObjects escaneando `.data` y descubre en tiempo de ejecución los offsets de UProperty y UStruct. Las clases, funciones y propiedades se buscan por nombre, sin SDK generado.
* **Red** (`src/net.cpp`, `src/protocol.h`): UDP en estrella. El host acepta clientes y reenvía los estados, chat, ping y timeouts.
* **Avatares**: el mod hace `Actor.Spawn` de la clase del pawn local, desactiva la física y la colisión y mueve el avatar con interpolación y extrapolación. Copia la velocidad para que el árbol de animación camine y corra, y el estado agachado.

## Sincronización de animaciones
Cada paquete de estado (protocolo v4) lleva dos cosas más:
- **Variables del personaje** que mueve el árbol de animación, que se copian en el avatar en cada frame:
  - `LocomotionMode`, `SpecialMove`, `CamcorderState`, `BodySetup`, tipos de borde, rendija y puerta;
  - `bIsCrouched`, `bCamcorderDesired`, `bJumping`;
  - `CurrentLean`.
- **Animación de cada `AnimNodeSlot`** (`FullBodyAnimSlot`, `RightArmAnimSlot`, `LeftArmAnimSlot`): nombre, tiempo, velocidad y si está en bucle.
  En el avatar se reproduce con `PlayCustomAnim` y se corrige el desfase con `SetPosition`.
  Así se sincronizan saltos, trepar, bordes, rendijas, armarios, camas y puertas.
- Al agacharse se compensa la altura de la cápsula para que el avatar no se hunda.

Con `Debug=1`, el log registra (`[watch]`) qué propiedades del personaje cambian, útil para encontrar nuevas.

## Chat de voz por proximidad
`src/voice.cpp`: el micrófono se captura con waveIn (16 kHz mono, tramas de 20 ms) y pasa por un detector de voz con umbral y cola de 350 ms.
Cada trama se codifica en IMA-ADPCM (`src/adpcm.h`, 4:1, unos 64 kbps, 33 dB de SNR) y se envía como mensaje `Voice`, que el host reenvía.
La reproducción usa waveOut estéreo, con un colchón de 60 ms por jugador.
El volumen y la panorámica se calculan cada frame según la distancia y el ángulo respecto a la cámara (pleno a menos de 3 m, silencio en `VoiceRange`, mismo mapa).
El micrófono se elige en el menú F9 (`MicDevice` en el ini). `voicetest.exe` prueba el micrófono y el códec sin el juego (`MIC=nombre voicetest.exe`).

## Limitaciones conocidas
* Se sincronizan los jugadores (posición, rotación, agachado y chat) y el avance por checkpoints. Enemigos, puertas y scripts entre checkpoints son locales de cada jugador.
* La cabeza de Miles se muestra con un material incorrecto, porque el juego no incluye el material real de esa malla.
* Verificado con los ejecutables de Steam:
  - Win64: `ProcessEvent` en la RVA 0x71E50, UObject Index 0x38, Name 0x48.
  - Win32: `ProcessEvent` en la RVA 0x65180 (`__thiscall`), UObject Index 0x20, Name 0x2C.

  Si el ejecutable es otro, el mod lo busca por patrón. Si no lo encuentra, muestra un aviso y el juego se abre sin el mod.
* La configuración se comparte en `Binaries/OutlastOnline.ini`.
