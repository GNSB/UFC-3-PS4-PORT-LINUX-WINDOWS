# UFC-40 · Factibilidad de ultrawide 21:9 en UFC 3

Ticket UFC-40, Claude 3, 16 oct 2026. **Solo investigación estática y documentación.** No se ejecutó el
juego, no se compiló, no hubo GDB, no se crearon parches, XML, scripts ni configs, y no se tocaron
partidas. No hay nada implementado. **No hay ningún resultado 21:9 medido ni observado.**

Formato confirmado por el usuario: **21:9**. La resolución exacta del monitor **no está confirmada**:
2560×1080 (64:27 ≈ 2,370) y 3440×1440 (43:18 ≈ 2,389) se usan solo como ejemplos. 32:9 queda fuera.

Base: anexo UFC-38 (`docs/ufc3/UFC-38-SUPERSAMPLING-TRIES.md` en `ec80a88`, revisado por el Jefe) y los
resultados de UFC-31/36/37/38 en el tablero. Los archivos de `/tmp` citados por el ticket
(`/tmp/ufc3-resolution-consumers.md`, `/tmp/UFC3_SUPERSAMPLING_TRIES.md`) **no existen** (comprobado con
`ls` el 16 oct); no se enlazan como si existieran. Las pruebas 4K/1800 (try-1b) siguen pendientes y aquí
**no se dan por funcionando**.

Convenciones: **V** = releído por mí en esta pasada (bytes del eboot, SPIR-V, código del repo) ·
**D** = documentado por otro ticket · **I** = inferencia razonada · **H** = hipótesis · **NM** = no medido.

Direcciones del eboot: vaddr del guest (sin la base `0x800000000`); offset de archivo = vaddr + `0x4000`
(`out/eboot.elf`, SHA-256 `08a4ffbc…c18f`, el mismo de las Pruebas 1–3 según UFC-38).

---

## 0. Respuesta corta

- **Hoy, sin cambios, 21:9 se ve como 16:9 con barras laterales (pillarbox), sin estirar ni recortar**
  (V, código del presenter). Es el comportamiento correcto como punto de partida, no es "soporte ultrawide".
- **Un 21:9 real (más campo de visión a los lados) requiere que el juego guest renderice a 21:9.** El
  host no puede inventar los píxeles laterales: la escena llega ya rasterizada a 16:9 (I fuerte).
- Para eso hacen falta **al menos tres cosas del guest, ninguna resuelta**:
  1. tamaños de escena/UI/display más anchos (los fija el selector `0x533180` y el singleton G; mismo
     problema abierto que en 4K, con un escritor de G desconocido);
  2. el **aspect de proyección** de la cámara: hay varios defaults **constantes 16:9** en el código de
     cámara (§3), así que agrandar solo los RT probablemente **estira** la escena (H fuerte);
  3. HUD/menús Scaleform: el modo de escalado del stage es desconocido (§5).
- **Viable en principio, no garantizado.** Es un trabajo de parche guest por firma más ajustes portables
  menores en el host. Ninguna parte es trivial y no se promete HUD ni FOV correctos sin arreglos.
- Una sola próxima prueba manual (§9), sin código, que sirve de baseline 16:9 vs ventana 21:9. Va
  **después** de try-1b (4K) y no compite con ella.

---

## 1. Qué se reutiliza de la investigación 4K y qué es nuevo

| Elemento | De UFC-31/36/38 | Uso para 21:9 |
|---|---|---|
| Selector `0x533180` y sus campos S/desc | D/V | **Reutilizable.** Mismo punto de entrada; pero en 21:9 cambia **solo el ancho** (alto 1080 se mantiene en 2560×1080) |
| Modo "high"/"ultra" (`0x428b90`, default `R+0x92ec=1` en `0x4258db`) y pares `width0`/`lowres_width0` de G | D | **Reutilizable como bloqueo**: el tamaño de escena real puede venir de G, cuyo escritor no se conoce. Igual que en 2A |
| Infraestructura de parches por firma (`scripts/patches.py`, `run.sh:119`) | V | **Reutilizable** sin cambios |
| Composite Pro `ddbe162b` (`texelFetch`) / base `e5b29094` (`textureLod` escena + `texelFetch` UI) | V | **Reinterpretado**: en 21:9 el riesgo no es el mismo que en 4K (§4) |
| Presenter host: escalado lineal de un tap, FSR1 solo si sube | V | Reutilizable; en 21:9 el presenter ya respeta aspect (§6) |
| Estimación de memoria de RT (≈116 MB a 1080) | D/H | Escala lineal con píxeles (§8) |
| **Aspect de proyección / FOV de cámaras** | — | **Nuevo** (§3) |
| **Modo de escala del stage Scaleform, safe zone, FMV rect** | — | **Nuevo** (§5) |
| **Hardcodes 1920×1080 del host** (UI composition, temporal) | parcialmente | **Nuevo para 21:9** (§6.3) |

Lo que **no** se reutiliza: los offsets o parches de CTR (otro juego), el sitio de mancelli (sin efecto en
Pro, UFC-38), y la idea "cambiar swapchain/ventana basta" (descartada en §6).

---

## 2. Tamaño real de escena/RT (capa guest)

### 2.1 Qué fija el selector (V, desensamblado de `0x533180` releído)

Ruta normal (no 1800), tras `test r12d,r12d` (r12d = Neo):

```
0x53325a  b9 80 07 00 00   mov ecx,1920      ; Pro: ancho de render
0x53325f  bf 40 06 00 00   mov edi,1600      ; base
0x533264  ba 84 03 00 00   mov edx,900
0x533269  0f 45 f9         cmovne edi,ecx    ; Neo → 1920
0x53326c  b9 38 04 00 00   mov ecx,1080
0x533271  0f 45 d1         cmovne edx,ecx    ; Neo → 1080
→ S+0x38/3c, +0x40/44, +0x48/4c, +0x50/54 = render (1920×1080 Pro)
0x5332a0  movl $0x780,0x58(%r14)  ; S+0x58 = 1920  (fijo, también en la rama 1800)
0x5332a8  movl $0x438,0x5c(%r14)  ; S+0x5c = 1080  (fijo)
desc+0x5c/60 = render · desc+0x90/94 = render
0x5332d1  movl $0x780,0x78(%r15)  ; desc+0x78 = 1920 (fijo)
0x5332d9  movl $0x438,0x7c(%r15)  ; desc+0x7c = 1080 (fijo)
```

- Para 21:9 a 2560×1080, en la ruta Pro bastaría cambiar el **ancho** de render (imm `0x53325b`) y los
  **anchos fijos** `S+0x58` (imm `0x5332a4`) y `desc+0x78` (imm `0x5332d5`). El alto 1080 no cambia (I).
  Son 3 inmediatos de 32 bits, no 2 como en 2A. **Qué consume cada uno no está identificado**: UFC-36
  dejó sin resolver los consumidores de S+0x38.. y quién escribe G+0x4c..0x70 (D).
- La consulta `sceVideoOutGetResolutionStatus` (`video_out.cpp:302`) devuelve `BB_INTERNAL_WIDTH/HEIGHT`
  (`emulator_settings.h:23–24`, `video_out.cpp:495`), pero el juego solo compara el alto con 2160
  (`0x53320a: cmpl $0x870`, V; UFC-31 D). **`BB_INTERNAL_WIDTH=2560` por sí solo no cambia nada** (I fuerte).

### 2.2 El pipeline de render está parametrizado por variables con nombre (V cadenas, I uso)

El eboot tiene un intérprete de script de render (`settarget, settexture, setview, setdepth, setviewport,
resolvetarget…`, `0x1e1ba61`) y variables `width0, height0, halfwidth0, quarterwidth0, offscreenwidth0,
lowres_width0, lowres_offscreenwidth0` (`0x1e07e40`) más derivadas `*_div_`/`*_mul_` (`0x1e8d000`).
Es el singleton G de UFC-36.

- **I:** si `width0` (o `lowres_width0` en modo "high") vale 2560, los RT derivados (half, quarter,
  bloom) deberían seguirlo sin tocar cada uno. Eso es favorable para 21:9.
- **H/riesgo:** RT de forma fija observados (960×1080, 96×1080, 1600×900 lineal, D) pueden no derivar de
  `width0`; quedarían en 16:9 y desalinearían efectos.
- `setviewport` es comando de consola/script (V cadena `0x1e1983c`); no se buscó si el script de UFC usa
  viewports absolutos.

### 2.3 Estado

| Capa | Estado |
|---|---|
| Sitios de inmediatos del selector para ancho | **V** (bytes) |
| Que cambiar esos inmediatos cambie la escena | **H** (mismo bloqueo que 2A: modo high/low y escritor de G, D) |
| Que los RT derivados escalen solos | **I** |
| Heap/memoria suficiente | **NM** (§8) |

---

## 3. Proyección de cámara: aspect ratio y FOV (capa guest, lo más nuevo)

Hallazgos (V, cadenas y desensamblado puntual; no RE exhaustivo):

| Sitio | Qué es | Evidencia |
|---|---|---|
| `0x4932a0..0x4932d0` | 4 getters virtuales consecutivos que devuelven constantes de `.rodata` `0x1e83b48..`: **0,6109 rad (35°)**, **1,7778 (16:9)**, 0,35, 650 | V. Interpretación (H): defaults de cámara FOV/aspect/near/far |
| `0xc649e9` (+`0xc64aa4`, `0xc64b07`, `0xc64b80`) | **CameraView RigOp** (cadenas `CameraRigFeature`, `CameraViewBinding`): parámetros con default **16:9**, **55,0**, 1,0, 1000,0; cada uno se reemplaza si hay binding (patrón `call *0x18` / `0xd524b0`) | V. Interpretación (H): aspect, FOV°, near, far con aspect por defecto constante |
| `0x7bab10` (tabla `0x1eb0938`) / `0x7ba940` | Nodo ANT "compute look at and fov given camera position and a cloud of points to frame": campos `CameraPos`, **`AspectRatio` (default 16:9 en `0x7bab47`)**, `Vertical/HorizontalFramingCorrection`, `AdditionalHorizontalFovInDegrees`, `Point0..7` | V |
| `0xd10fc1..0xd11041` | Asset de cámara animada: `AspectRatioAsset`, **`ForcedAspectRatio`**, **`FovIsHorizontal`**, `FovInDegrees`, `ProjectionMatrixAsset`… | V cadenas |
| `0x1146795..0x1146c9b` | Cámara de combate (framing): **`CameraVerticalFOV`**, `CameraVerticalFOVMin/Max`, `TargetAScreenLocation`, `ScreenSpaceTolerance`, `FOVSlider` | V cadenas |
| `0x29397c` | Ctor de un objeto de escena "Noa Scene" escribe `+0x8c = 0.0`, `+0x90 = 16/9` | V; uso desconocido |
| Cadena `LETTERBOX`/`Letterbox` (`0x1fcf888`) | Parámetro de pista de cámara cinemática junto a DOF/shake | V; las cinemáticas pueden dibujar sus propias barras |

Consecuencias:
- **H fuerte:** al menos parte del aspect de proyección **no se deriva del tamaño del RT**, sino de
  defaults constantes 16:9 sobreescribibles por bindings. Si el juego no alimenta un aspect vivo, agrandar
  los RT a 2560×1080 **estira la escena horizontalmente** (personas más anchas). Ese sería el modo de falla
  esperado de un parche que solo toque tamaños.
- **I favorable:** la cámara de combate trabaja con **FOV vertical** (`CameraVerticalFOV`). Con FOV
  vertical fijo, si el aspect se corrige, el resultado es **Hor+** (más campo a los lados, mismo alto):
  `hfov = 2·atan(tan(vfov/2) · aspect)`. No se conoce el vfov real de UFC en combate; 35° y 55° son
  defaults, no valores medidos.
- **Riesgo Vert-/recorte en cinemáticas:** las cámaras animadas pueden tener `FovIsHorizontal` y
  `ForcedAspectRatio`. Con FOV horizontal fijo, un aspect más ancho **recorta arriba/abajo** (zoom). El
  valor por asset es desconocido; los assets están comprimidos (`.big` con cabecera `EB\0\3`, UFC-38).
- **Framing de combate:** el nodo de framing usa `AspectRatio` (default 16:9) para encuadrar a los dos
  peleadores. Si queda en 16:9 con una pantalla 21:9, el encuadre se calcula para 16:9 y el borde extra es
  "gratis" (I). Si se le pasa 21:9, la cámara podría acercarse más (`AllowCameraToFrameCloserThanAuthored`
  existe como cadena). **No se sabe cuál de los dos ocurriría.**
- **Parche guest por firma para el aspect**: candidato a investigar, no a aplicar: las constantes 16:9 de
  `0x1e83b4c` (compartida por el getter `0x4932b0` y por el RigOp `0xc649e9`) y el inmediato de
  `0x7bab47`. Cambiar una constante de `.rodata` compartida puede afectar a otros consumidores; antes hay
  que listar todas las referencias (hay 3 coincidencias de `39 8e e3 3f` en el eboot; se identificaron las
  tres) y cualquier otro cálculo `w/h`.

### Estado

| Capa | Estado |
|---|---|
| Existen defaults 16:9 en código de cámara | **V** |
| La proyección en combate usa esos defaults (no aspect vivo) | **H** |
| Combate con FOV vertical → Hor+ si se corrige aspect | **I** |
| Comportamiento en cinemáticas/replays | **H** (FovIsHorizontal/ForcedAspectRatio por asset, desconocido) |

---

## 4. Viewport/scissor y composite UI/escena (capa guest + shaders)

### 4.1 Composite Pro `ddbe162b` (V, `spirv-cross` de `user/cache/CUSA06534/0x00000000ddbe162b_0.spv`)

- Compute 8×8; lee escena (`img0`) y UI (`img1`) con **`texelFetch` en la misma coordenada** que escribe
  la salida (`img2`), y consulta una **máscara de cobertura de UI** en buffers con direccionamiento por
  tiles cuyo pitch sale de un descriptor (`(data & 4095) + 63 >> 6`).
- **Reinterpretación para 21:9:** `texelFetch` no recorta si **escena, UI, máscara y salida tienen el
  mismo tamaño** (todas 2560×1080). El recorte de 2A aparecía porque escena y salida tenían tamaños
  distintos. En 21:9 el riesgo es otro: si la **UI o la máscara quedan en 1920** y la salida pasa a 2560,
  la UI ocupa los 1920 de la izquierda y la franja derecha lee fuera de rango (I).
- El tamaño de dispatch y los descriptores los arma el guest; no hay que tocar el shader si el guest crea
  todo coherente (I).

### 4.2 Composite base `e5b29094` (D/V UFC-38)

`textureLod` normalizado para la escena y `texelFetch` para la UI: con escena 21:9 y UI 21:9 funciona; con
UI 16:9 da el mismo problema de la franja (I).

### 4.3 Viewport/scissor

No hay evidencia capturada de viewports/scissors de UFC: la captura disponible
(`out/capture/frame_1791947138.txt`, base 1600×900) lista RT, formatos y draws, pero no viewports (V).
Lo que se sabe de viewports viene del código del host (§6.3). **NM.**

---

## 5. HUD, menús, cinemáticas, replays, career (capa guest)

| Contenido | Evidencia | Expectativa 21:9 |
|---|---|---|
| HUD/menús Scaleform/Flash | V cadenas: `StageScaleMode`, `showAll`, `noBorder`, `exactFit`, `noScale`, `scaleMode`; `Stage::stageWidthSet()/fullScreenWidthGet() is not implemented`; `Capabilities::screenResolutionX/YGet() not implemented` | Depende del modo usado: `showAll` → UI 16:9 centrada con márgenes (deseable); `noBorder` → recorte arriba/abajo; `exactFit` → **estirada**; `noScale` → anclada a una esquina. **Desconocido (H).** El tamaño del stage puede estar fijo a 1920×1080 |
| Safe zone | V: `graphics/render/render-safe-zone-mask`, `drawSafeZoneMask` (debug) | Sin relación directa con aspect (I) |
| Videos (FMV) | V: variables `FMVx/FMVy/FMVw/FMVh` del script de render | Rectángulo parametrizable; si se calcula para 1920 queda a la izquierda o se estira (H) |
| Cinemáticas (walkouts, presentaciones) | V: pista `LETTERBOX`, `FovIsHorizontal`, `ForcedAspectRatio` | Riesgo de recorte o barras propias (§3) |
| Replays | V: `Choose Replay Camera`, `ReplayChoice` (`0x7ba700`) | Mismo sistema de rigs que combate; sin evidencia propia |
| Career / selección de peleador | Retratos con tamaños fijos (§6.3) | Retratos de alto 1080 no cambian en 2560×1080 (I) |

**No se promete HUD correcto.** El escenario más probable sin arreglos específicos es HUD a la izquierda o
estirado (H).

---

## 6. Presentación en el host (capa portable)

### 6.1 Lo que hace hoy (V)

- `vk_presenter.cpp:397`: `expected_ratio = ancho/alto` del buffer del guest.
- `vk_presenter.cpp:777–790` (`SetExpectedGameSize`): el frame intermedio se crea con el aspect del juego
  dentro del tamaño de la swapchain.
- `vk_presenter.cpp:641–652`: limpia la swapchain a negro y hace `blitImage` con `MakeImageBlitFit`
  (`:97–122`, centra con offsets), filtro lineal. **Resultado: pillarbox 16:9 en una ventana 21:9, sin
  estirar ni recortar.** Existe `MakeImageBlitStretch` (`:92`) pero no se usa.
- Ventana: `gpu/shim/window.cpp:74–80` acepta cualquier `BB_WINDOW_SIZE=WxH` entre 320×200 y 7680×4320
  (2560×1080 y 3440×1440 incluidos). `BB_FULLSCREEN=1` toma el tamaño del monitor. El launcher de Windows
  solo ofrece `1920x1080/2560x1440/3840x2160` (`sacredfire_launcher.c:61`); el de Linux no fija tamaño.
- Driver VideoOut (`driver.cpp:180–230`): exige `aspect_ratio == 0` (el único valor del SDK, 16:9 de la
  consola) pero **no limita el ancho**, solo `width <= pitch`. Un buffer de display 2560×1080 se aceptaría
  (V). Con eso el presenter mostraría 21:9 sin barras automáticamente (I).

### 6.2 Por qué cambiar swapchain/ventana no basta

El guest compone y entrega un buffer 16:9. El host solo puede pillarbox (correcto), estirar (deforma) o
recortar arriba/abajo (pierde imagen). **Ninguna opción del host da más campo de visión.** Un "fake
ultrawide" por extensión de bordes o reproyección no es aceptable para el criterio del ticket.

### 6.3 Hardcodes 1920×1080 del host que afectarían a un guest 21:9 (V)

| Sitio | Qué hace | Impacto 21:9 |
|---|---|---|
| `ui_composition.h:20–23` `NativeViewport` | Detecta viewports UI de 1920×1080 exacto | Solo se usa en la ruta escalada (redirect/temporal); con escala nativa no aplica (I). Habría que generalizarlo si se combina 21:9 con escalado |
| `ui_composition.h:33–37` `Scale` | Divide por 1920/1080 en coordenadas nativas | Ídem |
| `vk_temporal_upscaler.h:159`, `.cpp:202–203, 1406, 1411` | Asume guest 1920×1080 | Ruta temporal (forzada Off en UFC, D UFC-35); no bloquea un primer intento |
| `vk_scene_resolution.cpp:74–96` | Solo acepta 1920×1080 / 960×540 | try-3 ya descartada; no aplica |
| `vk_rasterizer.cpp:4056` | Factor relativo a 1920×1080 | Solo con `reduced`, que no se activa en UFC (D UFC-38) |
| `texture_cache.cpp:106–116` `T4Portrait` | Tamaños exactos de retratos (652×1080, 600×1080…) | Con alto 1080 no deberían cambiar (I); a 3440×1440 sí podrían |
| `vk_presenter.h:113–115` | Valores iniciales 1920×1080 | Se recalculan en cada frame (V); sin impacto |

**Fix portable posible (pequeño, no urgente):** generalizar `NativeViewport`/`Scale` para que la
resolución "nativa" de UI sea la del guest en vez de 1920×1080. Solo hace falta si se usa la ruta escalada.

---

## 7. Límites 21:9

- **Resolución del monitor sin confirmar.** 2560×1080 mantiene el alto 1080 (menos riesgo: retratos, RT de
  alto fijo, memoria +33 %). 3440×1440 cambia ancho **y** alto: combina el problema 21:9 con el problema
  1440 de UFC-38 (memoria, heap, retratos, `T4Portrait`). **Recomendación: diseñar para 2560×1080 interno**
  y, si el monitor es 3440×1440, presentar 2560×1080 escalado a la ventana (el presenter ya escala con
  aspect correcto). 64:27 y 43:18 difieren menos de 1 %: la imagen 2560×1080 en 3440×1440 deja ~9 px de
  barra por lado (I, cálculo).
- **Hor+ depende del aspect de proyección** (§3), no de los RT.
- **Cinemáticas pueden quedar recortadas o con barras propias** aunque el combate funcione.
- **HUD**: sin solución conocida; puede requerir parches de UI/Scaleform aparte.
- **Combinación con 4K/1800:** no combinar en la misma prueba. 21:9 a 1800p (4267×1800) no se considera.
- **Online/determinismo:** el render no debería afectar la simulación, pero la cámara de combate y su
  framing **podrían** alimentar lógica (p. ej. visibilidad, LOD o selección de cámaras de replay). **No se
  asume seguro ni que cause desync.** 21:9 se valida solo offline; online queda deshabilitado para 21:9
  hasta tener evidencia, y ambos pares no tienen por qué usar el mismo aspect si se demuestra que es solo
  visual (H).

---

## 8. Riesgo de rendimiento y memoria (NM; estimación)

| Resolución interna | Píxeles | Relación vs 1920×1080 | RT estimados (escala lineal sobre ≈116 MB D/H) |
|---|---|---|---|
| 1920×1080 | 2,07 M | 1,00 | ≈116 MB (D) |
| 2560×1080 | 2,76 M | 1,33 | ≈155 MB (H) |
| 3440×1440 | 4,95 M | 2,39 | ≈277 MB (H) |

Más geometría visible a los lados (Hor+) suma draws/culling: coste de CPU/GPU **NM**. El baseline de
Test 1 (60,00 FPS, 1 % low 46,72, D) es la referencia; ningún número 21:9 está medido. Heap del motor
desconocido (UFC-38).

---

## 9. Única próxima prueba manual propuesta (NO ejecutada)

**T21-0 · Control de presentación 16:9 vs ventana 21:9, sin código ni parches.**
Va **después** de try-1b (4K). No necesita monitor ultrawide: se hace en ventana.

Objetivo: fijar el baseline 16:9 y confirmar en la máquina real que la ventana 21:9 hoy da pillarbox
correcto (sin estirar ni recortar) y que el guest no cambia. Es el control contra el que se compararán
geometría, FOV y HUD de cualquier variante futura.

1. Copia aislada nueva de `user/`. Config de Test 1: `BB_NEO=1`, VideoOut 1080
   (`BB_INTERNAL_WIDTH/HEIGHT=1920/1080`), cap 60, `BB_WRITE_STEP=0`, write windows y preupload en 0 (los
   defaults no se tocan), `BB_FSR1=0`, sin parches (`Patches: 0 writes`).
2. **Corrida A (baseline 16:9):** ventana 1920×1080. Menú principal → selección de peleador → combate.
   En el ring, con la cámara de combate por defecto: screenshot + `capture.trigger`.
3. **Corrida B (21:9):** misma config, **única variable** `BB_WINDOW_SIZE=2560x1080` (o el tamaño real del
   monitor si el usuario lo confirma). Mismos pasos y misma pelea.
4. Reportar las dos capturas y los dos screenshots. No medir rendimiento.

Criterios:
- **Geometría:** en B la imagen 16:9 está centrada con barras negras laterales; un objeto redondo (logo del
  octágono, cabeza) mide igual de ancho que alto en A y B (sin estirar).
- **FOV:** en B se ve exactamente el mismo contenido que en A (ni más ni menos). Esperado: sin Hor+.
- **HUD:** idéntico en A y B, dentro del área 16:9.
- **Guest:** en las dos capturas, escena/UI/composite en 1920×1080 y presentado 1920×1080.

Éxito = todo lo anterior se cumple; deja documentado el baseline. Fracaso = estiramiento, recorte o
cambio de tamaños en el guest (significaría un bug de presentación: ticket portable separado).
Rollback: quitar `BB_WINDOW_SIZE`. Nada persistente fuera de la copia de `user/`.

**Siguiente prueba posible (no propuesta todavía):** T21-1, un parche de ancho a 2560 (§10, ticket B),
comparado contra la corrida A con los mismos criterios: geometría (¿estira?), FOV (¿Hor+?), HUD (¿franja
o desplazamiento?) y cadena completa de RT a 2560×1080.

---

## 10. Propuesta mínima por tickets (sin archivos superpuestos)

| Ticket | Tipo | Alcance | Archivos | Depende de |
|---|---|---|---|---|
| **A. Consumidores de ancho** | research estática | Identificar quién lee S+0x38/0x58 y desc+0x78/0x90 y quién escribe `width0`/`lowres_width0` de G (compartido con 4K, UFC-36 §3) | solo `docs/` | ninguno; puede salir del trabajo 4K |
| **B. Parche de ancho 21:9** | implementación (XML) | XML por firma con los 3 inmediatos de ancho (§2.1), sin aspect; prueba T21-1 | `patches/` (XML nuevo) | A; T21-0 |
| **C. Aspect de proyección** | research + XML | Listar todos los consumidores de las constantes 16:9 (§3) y cálculos `w/h`; diseñar el parche de aspect (Hor+) | `docs/` y luego un XML **distinto** de B | T21-1 (para saber si estira) |
| **D. HUD/Scaleform** | research | Modo `StageScaleMode` real, tamaño del stage, rectángulo FMV | `docs/`; parche aparte si hace falta | B funcionando |
| **E. Host portable** (opcional) | código host | Generalizar `UiComposition::NativeViewport/Scale` a tamaño nativo del guest; opción launcher 2560×1080 | `ui_composition.h`, launchers | solo si se combina con escalado |

B y C usan archivos XML distintos para poder activarse por separado y probar de a una variable.
Online: ninguno de estos tickets lo habilita; validar offline primero.

---

## 11. Estado por capa (resumen)

| Capa | Hoy | Para 21:9 real | Certeza |
|---|---|---|---|
| Ventana/swapchain | Cualquier WxH | Listo | V |
| Presentación | Pillarbox correcto | Si el guest entrega 21:9, se muestra sin barras | V / I |
| Driver VideoOut | Acepta buffers más anchos | Listo | V |
| Composite | `texelFetch` 1:1 | OK si todo es del mismo tamaño | V / I |
| Tamaños de RT guest | 1920×1080 fijo | Parche de ancho + escritor de G sin resolver | V sitios / H efecto |
| Proyección/FOV | Defaults 16:9 | Parche de aspect necesario (probable) | V defaults / H uso |
| HUD/menús | 16:9 | Desconocido | H |
| Cinemáticas/replays | 16:9 | Riesgo de recorte/barras | H |
| Memoria/rendimiento | baseline Test 1 | +33 % píxeles a 2560×1080 | NM |
| Online | 16:9 | No habilitar sin evidencia | — |

---

## 12. Pendiente / datos que faltan

- Resolución exacta del monitor del usuario (no bloquea: el diseño apunta a 2560×1080 interno).
- try-1b (4K) y la cadena del modo high/low: siguen pendientes; el ticket A comparte su RE.
- Consumidores reales del aspect en combate (ticket C); no hay sonda diseñada para esto y no se pide GDB.
- Modo de escalado Scaleform y assets `.big` comprimidos sin herramienta de desempaquetado.
- Nada de esto está medido.
