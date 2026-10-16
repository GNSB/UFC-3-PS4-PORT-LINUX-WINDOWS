# UFC-38 · Supersampling interno → salida 1080: alternativas try-1 / try-2 / try-3

Anexo del informe CTR/UFC (UFC-31/37). Ticket UFC-38, Claude 3, 16 oct 2026. Segunda pasada,
reconciliada con el resultado de UFC-36. **Solo documentación**: no se ejecutó el juego, no se compiló,
no hubo GDB/ptrace, no se crearon parches, scripts ni configuraciones, y no se tocaron partidas. **No hay
ninguna variante implementada ni autorizada.** Ningún resultado de try-1/2/3 está medido; las únicas
métricas citadas son las de Test 1 (baseline, documentadas por otros).

Convenciones de certeza:
**V** = releído por mí en esta pasada (bytes del eboot, SPIR-V, código del repo) ·
**D** = documentado por otro ticket, artefacto ya no disponible para releer ·
**I** = inferencia razonada · **H** = hipótesis · **NM** = no medido.

---

## 0. Procedencia y límites de la evidencia

### 0.1 Los artefactos de `/tmp` ya no existen (verificado el 16 oct)

`/tmp` se vació entre las pasadas. Comprobé con `ls` que **no existen**:
`/tmp/UFC3_SUPERSAMPLING_TRIES.md` (primera pasada de este anexo), `/tmp/ufc3-resolution-consumers.md`
y `/tmp/ufc3-test3-videoout-trace.sl2SGG/` (UFC-36/Test 3), `/tmp/ufc3-test1-linux.*`,
`/tmp/ufc3-test2-linux-1800p.*`, `/tmp/CTR_vs_UFC3_ANALYSIS.md`, `/tmp/CTR_vs_UFC3_REVIEW_CLAUDE3.md`,
`/tmp/ufc36/`, `/tmp/ufc3-videoout-probe.gdb`, `/tmp/CTR-NitroFueled-PC-analysis`.
Este documento **no enlaza** ninguno de ellos como si existiera.

**Cómo se reconstruyó** (sin repetir la ingeniería inversa):
- El texto completo de la primera pasada de este anexo, de `ufc3-resolution-consumers.md` y de `TEST3.md`
  se recuperó de las llamadas `Write` registradas en los logs del despachador:
  `~/.local/share/ufc3-flow/logs/UFC-38.log` y `~/.local/share/ufc3-flow/logs/UFC-36.log`
  (formato stream-json; existen). Eso es texto escrito por los agentes, no los artefactos originales.
- Los resultados de UFC-31/34/35/36/37 se leyeron del tablero (`http://127.0.0.1:8792/api/state`).
- El informe principal `CTR_vs_UFC3_ANALYSIS.md` **no se puede releer** (solo hay ediciones parciales en
  `UFC-37.log`). Por eso la nota de integración (§9) es una referencia, no una revisión del informe.

### 0.2 Qué se releyó en esta pasada (V) y qué no (D)

| Evidencia | Estado | Detalle |
|---|---|---|
| Repo `ufc3-online` | V | `8de5098b464d886571c338530a4ed273c5395521`; submódulo `gpu/third_party/fsr-vulkan` sucio e intacto |
| `out/eboot.elf` | V | SHA-256 `08a4ffbc…c18f`, igual al de las Pruebas 1–3 |
| `out/sacredfire`, `out/gpu/libbbgpu.so` | V | `3a88e043…`, `89c01777…`: los mismos binarios de las pruebas |
| Bytes clave del eboot (§1.1) | V | releídos en `eboot.elf` con offset de archivo = vaddr + `0x4000` |
| Firma del fallback del selector | V | aparece 1 vez, en `0x533257` |
| Composite Pro `ddbe162b` | V | `user/cache/CUSA06534/0x00000000ddbe162b_0.spv` (`d35bbb89…`): `texelFetch` en la misma coordenada para sus dos entradas (líneas 98 y 142 de `spirv-cross`) |
| Composite base `e5b29094` | V | `user/shader/dumps/cs_0x00000000e5b29094_0.spv` (`582edf55…`): `texelFetch` para la UI, `textureLod` normalizado para la escena (líneas 78 y 82) |
| Captura base previa (no Pro) | V existe | `out/capture/frame_1791947138.txt` (`b85c3f64…`); no se volvió a inventariar |
| Código del repo citado en try-2/try-3 | V | `run.sh:119`, `scripts/patches.py:75`, `vk_scene_resolution.cpp:215`, `vk_rasterizer.cpp:3771–3775`, `texture_cache.cpp:106–109`, `fsr_pass.cpp` y `vk_presenter.cpp:431–438` |
| mancelli `memory_patcher.cpp:696+` | V existe | clon en `/home/myzk/ufcCOntext/mancelli-shadPS4` @ `12739d23…` |
| Sonda Test 3 (`videoout-probe.log`) | **D** | selector en rama 1800p, `STATUS=OK`; texto en el resultado de UFC-36 y en `TEST3.md` recuperado |
| Capturas Test 1/2/3 e inventario de RT | **D** | escena, depth y composite a 1920×1080, R32F 1600×900 solo con VO 2160; ningún RT > 1080 |
| Métricas Test 1 (`combat-60s-metrics.json`) | **D** | baseline de §6; el archivo ya no existe |
| Desensamblado de consumidores de UFC-36 | **D** | `0x428b90`, setters, `0x118b60`, getters `0x63920/0x63960`; solo se reverificaron los bytes de §1.1 |

---

## 1. Qué cambió con UFC-36 (resumen, sin duplicar su RE)

### 1.1 La ruta nativa 1800p existe y un modo "high" por defecto la devuelve a 1080

Cadena estática documentada por UFC-36 (D). Los bytes marcados con V se releyeron aquí:

```
selector 0x533180  (Neo ∧ Open>0 ∧ Query ok ∧ alto==2160 → rama 1800p; Test 3: tomada según la sonda, D)
  S+0x38/3c = 3200x1800   S+0x40/44 = 1600x900   S+0x48..0x5c = 1920x1080   S+0x67 = 1
  desc+0x5c/60 = 3200x1800   desc+0x78/7c y +0x90/94 = 1920x1080
    └─ 0x10d300 → 0x118b60: crea "RNA:VideoOutTex_High" = desc+0x5c/60 (3200x1800)
                            y "RNA:VideoOutTex_Low" = 1920x1080; registra primero el High
vfunc 0x428b90 (sin call directo; se desconoce cuándo corre)
  r13d = [LodManager+0x44]                         (0x428bf9: 44 8b 68 44, V)
  Neo: r13d≠0 → modo 2 · r13d==0 ∧ R+0x92ec==1 → modo 2 · r13d==0 ∧ R+0x92ec≠1 → modo 1
       (0x428c3b: 83 bb ec 92 00 00 01, V)
  R+0x92ec = 1 por defecto en el ctor              (0x4258db: 41 c7 87 ec 92 00 00 01 00 00 00, V)
  G+0x90 = modo; nombres de la tabla 0x2505200: 0 "medium", 1 "ultra", 2 "high"
  jmp 0x112130 (esi = modo==2)                     (0x428cc1: e9 → 0x112130, V)
     modo 1 "ultra" → set High (3200x1800) · modo 2 "high" → set Low (1920x1080)
getters del modo: 0x63920 → 1: G.width0/height0 · 2: G.lowres_width0/height0 ; 0x63960 → offscreen
setters virtuales: 0x4a0a50 escribe LodManager+0x40 (copiado a +0x44); 0x4a0a60 escribe R+0x92ec
```

Lo que significa para el diseño:
- **Código estático (D/V):** con los valores por defecto, Neo queda en modo 2 "high", que usa el set Low
  1080 y el par `lowres` del singleton G. Eso es coherente con escena/depth/composite a 1080.
- **Causalidad en la corrida: no trazada.** No hay ninguna sonda que haya observado `0x428b90`, el valor de
  `R+0x92ec` ni el set registrado en combate. **No se afirma que el modo "high" ocurriera en Test 3**; es
  la hipótesis H1 de UFC-36, falsable con la sonda de try-1c.
- **Sin resolver (D, UFC-36 §3):** quién escribe G+0x4c..0x70 (`width0`, `lowres…`); quién llama a los
  setters `0x4a0a50` y `0x4a0a60`; cuándo corre `0x428b90`; quién consume S+0x38/3c (3200×1800).
- **1800p no es 4K y no se etiqueta como checkerboard.** No hay cadenas ni pases de reconstrucción, ni
  targets 3840×2160. 3200×1800 sería el tamaño nativo del set High (I).

### 1.2 Pista del usuario: opción PS4 Pro en Preferences (NO confirmada)

El usuario recuerda una opción de Preferences: rendimiento 1080p/60 frente a calidad "4K"/30 con
checkerboard. Revisión estática de esta pasada:
- **Eboot (V):** no hay cadenas "PS4 Pro", "checkerboard" ni "supersampl". **Corrección a UFC-36:**
  "performance" y "quality" sí aparecen (17 y 30 coincidencias sin distinguir mayúsculas), pero en
  contextos ajenos: gameplay (`PERFORMANCE_OF_THE_NIGHT_COUNT`, `CROWD_FIGHT_QUALITY_*`), red
  (`monitor-connection-quality`) y stubs de Scaleform (`Stage::qualitySet() is not implemented`). Ninguna
  es una opción de menú Pro.
- **Pistas no seguidas (H):** el comando de consola `setgraphicslimits graphicslimitsvar` y el JSON de
  telemetría `"harware_type": "%s", "resolution": "%s"`. No se buscaron consumidores; podrían no tener
  relación con el modo.
- **Assets de UI (no concluyente):** los `.big` de `orig/CUSA06534/databigs` tienen cabecera `EB\0\3` y
  el contenido está comprimido. Un `grep` de texto plano no sirve (los "4k/4K" que salieron en la
  pasada interrumpida son ruido binario). No hay herramienta de desempaquetado en el repo y no se creó
  ninguna.
- **Partidas/config:** no se leyó ni se editó `PROFILE`/`EXTRADATA`.
- **Lo que sí encaja (I/H):** existe un interruptor real de dos modos en Neo ("ultra" → 1800, "high" →
  1080) con "high" por defecto, y un setter virtual (`0x4a0a60`) que podría escribirlo desde una
  preferencia. Que sea la opción que recuerda el usuario es **H**. Si existiera, la hipótesis del Jefe
  ("el selector elige capacidad 1800 pero la preferencia de rendimiento mantiene RT 1080") sería
  exactamente H1.
- **fps:** el cambio de flip rate 0/1 ocurre igual en Pruebas 1 y 3 (D), así que no está ligado a la rama
  1800. No hay evidencia de que "ultra" fije 30 fps. Si lo hiciera, cambiaría el ritmo y posiblemente la
  lógica: esa corrida no sería comparable con el baseline cap 60. **No se promete 4K60 ni 1800p60.**

---

## 2. try-1 · Diagnóstico de la ruta nativa 1800p / modo Pro (sin modificar nada)

**Tipo:** diagnóstico, en tres pasos. No produce supersampling por sí mismo.

**Problema.** Con VideoOut 2160 el selector elige 1800p (D, Test 3), pero el combate queda en 1080.
UFC-36 propone que el modo "high" por defecto lo devuelve a 1080. Falta (a) saber si existe una opción de
usuario que ponga "ultra" y (b) observar en la corrida qué modo y qué set quedan activos.

**Mecanismo.**
- **1a · Estático: hecho.** Selector (primera pasada) más la cadena de UFC-36 (§1.1). No se repite.
- **1b · Menú del juego: la única próxima prueba manual recomendada (§8).** Sin parches ni GDB: con la
  configuración de Test 3, buscar en Settings/Preferences una opción de resolución, rendimiento, calidad
  o Pro; si existe, ponerla en calidad y capturar en el ring. Es el camino de menor riesgo y responde
  directamente a la pista del usuario.
- **1c · Sonda de solo lectura (solo diseño; GDB requiere autorización explícita).** Es la §5 de UFC-36:
  4 breakpoints de hardware sin detach tras el selector, hasta 8 aciertos cada uno o hasta llegar al
  ring. Luego se borran y se hace detach.

  | BP | Runtime | Registrar |
  |---|---|---|
  | 1 | `0x800428c9d` (call `0x122c410`) | `esi` (modo), `r13d`, `[rbx+0x92ec]`, `r12d` |
  | 2 | `0x800112182` (`0x112160` tras el prólogo) | `esi` (0 High / 1 Low), `[r12+0x5c]` |
  | 3 | `0x80048d264` (tras los getters) | dimensiones elegidas, `[G+0x90]`, volcado de `G+0x4c..0x70` (G = `[0x802982560]`) |
  | 4 | `0x8004a0a8b` (setter de `R+0x92ec`) | `ebx` (valor nuevo) y dirección de retorno, para identificar al llamador virtual |

  El script base de UFC-34 (`/tmp/ufc3-videoout-probe.gdb`) **ya no existe**: habría que reescribirlo a
  partir de su descripción en el tablero (UFC-34: try/finally por etapas, `STATUS`, detach garantizado).

**Archivos/símbolos.** `out/eboot.elf`: `0x533180` (selector), `0x118b60` (sets VideoOut), `0x428b90`,
`0x4258db`, `0x4a0a50/0x4a0a60`, `0x63920/0x63960`, `0x49fe4c` (consumidor de `S+0x67`).

**Dependencias/licencias.** Ninguna nueva: el menú del juego (1b) o GDB del sistema con una sonda propia
(1c). Nada de CTR ni de mancelli.

**Hipótesis.**
- H1 (UFC-36): el modo 2 "high" por defecto re-registra el set Low y elige `lowres` → todo a 1080.
- H1-pref: una opción del menú llama a `0x4a0a60` y pone "ultra".
- H2 (UFC-36): `G.width0` = 3200×1800 en la rama 1800.

**Riesgos.**
- 1b: cambiar la opción puede guardarse en la partida, así que hay que usar una copia aislada de `user/`.
  "Ultra" podría cambiar LOD, lógica o fps (el override vive en un objeto de LOD; H). El coste en GPU a
  3200×1800 es NM. Si "ultra" compone a 1800, la reducción a la ventana 1080 la hace el presenter del host
  con un muestreo lineal de un solo tap (`vk_presenter.cpp:431–438`, `pp_pass.cpp:175–176`, V). FSR1 no
  corre porque la entrada es mayor que la salida (`fsr_pass.cpp`, V). A razón 1,67 eso promedia solo en
  parte (H): la calidad del supersampling dependerá de ese filtro.
- 1c: GDB altera tiempos hasta el detach; si gdb muere de forma anormal, `-batch` mata el juego. La sonda
  no está reescrita ni validada.

**Criterio de éxito.**
- 1b: aparece una opción y, con "calidad", la captura del ring muestra la escena y el depth por encima de
  1080 y el buffer presentado a 3200×1800 (o al menos > 1080), con UI y HUD correctos.
- 1c: una tabla modo → set → dimensiones → llamador del setter, que confirme o descarte H1.

**Fracaso.**
- 1b: no hay ninguna opción, o la opción no cambia ningún RT. Se documenta y se pasa a 1c.
- 1c: no se ve `0x428b90` en la ventana de la sonda (corre antes o por otra ruta). Queda solo la vía
  estática.

**Rollback conceptual.** 1b: descartar la copia de `user/`. 1c: detach, nada persistente.

**Config, 1 variable.** Igual que Test 3 (Neo 1, `BB_INTERNAL_WIDTH/HEIGHT=3840/2160`, ventana 1920×1080,
cap 60, `BB_WRITE_STEP=0`, write windows y preupload en 0, `user/` aislado). La única variable es la opción
del menú (1b) o la sonda (1c).

**Prueba manual concreta.** Está en §8 (1b). La de 1c solo se hace con autorización.

**Qué NO está probado.** Que la opción exista en esta build, su efecto, H1/H2, el modo en combate y el
costo de "ultra".

---

## 3. try-2 · Parche propio por firma (infraestructura UFC existente) — solo diseño

**Tipo:** variante de implementación. **No se creó XML ni `patches.bin`. No autorizada.**

**Infraestructura (V).** `scripts/patches.py` compila XML shadPS4/GoldHEN con `Signature` y
`SignatureOffset` (`locate()` en `:75`); si la firma falta o aparece repetida, omite el parche. `run.sh:119`
lo invoca en cada arranque con `BB_PATCHES_DIR` y `BB_PATCHES_CONFIG`, y el loader aplica `BBPATCH2` antes
de entrar al guest. En Test 3 el log registró `Patches: 0 writes` (D). No hace falta portar
`ApplyRenderResolution` de mancelli ni nada de CTR: sus offsets y su heap no aplican a UFC.

**Hallazgo sobre mancelli (de la primera pasada, bytes V).** Su sitio escribe los inmediatos de
`mov edi,1600`/`mov edx,900` del fallback (`0x533260`/`0x533265`). En Neo los pisan los `cmovne` siguientes,
así que **en modo Pro el parche de mancelli no tiene efecto**.

### Sub-variantes (una por prueba; ninguna está lista para recomendar)

| | Sitio (V) | Requiere | Idea | Dependencias sin resolver |
|---|---|---|---|---|
| **2U** forzar "ultra" | default `R+0x92ec` en `0x4258db` (imm `01`) | VO 2160, Neo 1 | Usa la ruta nativa del juego: set High 3200×1800 (I) | Solo tiene sentido si 1b no encuentra opción y 1c confirma H1. El setter `0x4a0a60` u otro llamador puede reescribir el valor después; `LodManager+0x44≠0` también fuerza "high". Efecto en LOD/lógica/fps desconocido |
| **2A** fallback Pro a W×H | `mov ecx,1920` (imm en `0x53325b`) y `mov ecx,1080` (imm en `0x53326d`) | VO 1080, Neo 1 | Todos los campos derivados del selector = W×H; mantiene Pro | (a) **Composite:** `ddbe162b` usa `texelFetch` (V); si el motor lo mantiene con escena W×H, la imagen sale recortada (cuadrante superior izquierdo). (b) **Modo High/Low:** con "high" el tamaño de escena sale de `G.lowres…`, cuyo escritor no se conoce (UFC-36 §3.1); el parche puede no cambiar la escena. (c) Heap |
| **2B** base, sitio mancelli | `0x533260` / `0x533265` | `BB_NEO=0` | El base ya compone escena ≠ 1080 con `e5b29094` (`textureLod`, V) | Cambia dos variables frente a Test 1 (Neo + parche) y necesita un control base medido que no existe. Pierde el modo Pro |
| **2C** rama 1800p | `0x533239` / `0x533234` | VO 2160 | Escena W×H dentro de la rama 1800 | Mezcla tamaños incoherentes (1600×900 de linear depth); **queda última** |

**Corrección respecto de la primera pasada.** La primera pasada recomendaba **2A** como la siguiente
prueba. Con UFC-36 deja de estar lista: el tamaño de escena puede depender del modo y de los pares de G, y
no solo de `ecx/eax`. La deducción "solo `ecx/eax` explica la escena en las tres configuraciones" sigue
siendo una correlación (D), no un consumidor identificado. 2A queda condicionada a resolver (a) y (b).

**Ejemplo de XML (NO creado; validar el formato contra `patches.py` antes de usarlo):**

```xml
<!-- NO CREADO: diseño 2A a 2560x1440 -->
<Patch><TitleID><ID>CUSA06534</ID></TitleID>
 <Metadata Title="UFC 3" Name="UFC3 Pro render 2560x1440 (try-2A)" AppVer="01.14" AppElf="eboot.bin" isEnabled="true">
  <PatchList>
   <Line Type="bytes32" Address="0x0093325b" Value="0x00000A00"
         Signature="45 85 e4 b9 80 07 00 00 bf 40 06 00 00 ba 84 03 00 00 0f 45 f9 b9 38 04 00 00 0f 45 d1" SignatureOffset="4"/>
   <Line Type="bytes32" Address="0x0093326d" Value="0x000005A0"
         Signature="45 85 e4 b9 80 07 00 00 bf 40 06 00 00 ba 84 03 00 00 0f 45 f9 b9 38 04 00 00 0f 45 d1" SignatureOffset="22"/>
  </PatchList></Metadata></Patch>
```

**Dependencias/licencias.** UFC es GPL-2.0-or-later. El XML sería dato propio con bytes deducidos de nuestro
desensamblado. La idea del sitio base coincide con mancelli (fork de shadPS4, GPL): acreditarlo sin copiar
su C++. Nada de CTR. La firma contiene unos 29 bytes de código del juego, aceptable como firma, pero no hay
que publicar el eboot ni dumps.

**Hipótesis.** H2-a: con el parche el motor crea escena/depth/tonemap a W×H y conserva UI/display a 1080.
H2-b: elige un composite que escala. H2-c: el heap alcanza a 1440.

**Riesgos.**
1. Composite sin escala, que recorta la imagen (2A).
2. Parche sin efecto por el modo y los pares de G (2A/2C).
3. Calidad: un solo tap con `textureLod` a razón 1,33 promedia solo en parte (H).
4. Memoria: estimación desde una captura (D/H): unos 116 MB de targets a resolución completa a 1080,
   unos 206 MB a 1440 y unos 464 MB a 2160. Se desconoce el reparto del heap del motor, y `BB_DMEM_MB`
   no garantiza nada sobre él.
5. Targets fijos o derivados con formas raras (960×1080, 96×1080, la cadena de bloom).
6. `T4Portrait` compara tamaños exactos (`texture_cache.cpp:106–109`, V); si los retratos dependen de la
   altura de escena, el fix de retratos dejaría de activarse.
7. Variantes de shader nuevas: stutter con la caché fría (NM).
8. Online: no usar hasta probarlo offline; no se asume que sea seguro ni que cause desync.

**Criterio de éxito (cadena entera, no "se creó un RT").** En la captura de combate en el ring:
- escena B10G11R11 y depth D32S8 a W×H;
- tonemap a W×H;
- media resolución a W/2×H/2;
- composite que **muestrea** la escena y escribe sRGB 1920×1080, que es el buffer presentado;
- UI 1920×1080 intacta.

Además, sin recorte ni zoom, con HUD y retratos correctos, y un combate completo sin `Guest fault`, `STOP`
ni OOM. Recién entonces medir 60 s con el método de Test 1.

**Fracaso:** recorte, cadena a medias, crash/OOM o retratos rotos. No se reintenta con otro tamaño sin
análisis.

**Rollback conceptual.** Quitar `BB_PATCHES_DIR`/`BB_PATCHES_CONFIG` y verificar `Patches: 0 writes` en
`game.log`. Usar siempre una copia de `user/`.

**Config, 1 variable.**
- 2U = Test 3 + parche.
- 2A = Test 1 + parche.
- 2B = control base nuevo + parche.
- 2C = Test 3 + parche.

**Prueba manual (futura, solo con aprobación).** Copia nueva de `user/`, config de referencia + parche;
comprobar `N writes applied` en `game.log`; menú → selección de luchador (retratos) → combate → ring →
`capture.trigger`; inspección visual e inventario de la captura según el criterio de éxito.

**Escalonado.** **1440 (2560×1440) primero**, sin darlo por funcionando. 2880×1620 es opcional. **3840×2160
solo después** de tener la cadena entera estable a 1440 y una revisión del heap. Para 2U el tamaño lo fija
el juego (3200×1800), así que no hay escalón 1440.

**Qué NO está probado.** Nada: ni la sintaxis del XML, ni que el motor acepte más de 1080, ni el composite,
la memoria, la calidad o los FPS.

---

## 4. try-3 · Escalado de escena en el renderer (`vk_scene_resolution.cpp`) — DESCARTADA en su forma actual

Evidencia (V, releída en esta pasada salvo indicación):
1. **No se activa en UFC.** La escena empieza solo con un pase de ≥5 MRT (`vk_rasterizer.cpp:3771–3772`), y
   `reduced` exige `scene_started` (`:3775`). En los combates UFC capturados ningún pase usa más que
   `color0` (D).
2. **Pensado para reducir y controlado por el temporal**, que en UFC está forzado a Off (D, UFC-35).
3. **La resolución hacia el nativo es *nearest*** (`vk_scene_resolution.cpp:215`). Un proxy mayor llegaría
   diezmado a cualquier consumidor nativo, y el composite Pro `ddbe162b` lee con `texelFetch` (V). Eso da
   cero beneficio de supersampling.
4. Solo acepta targets de 1920×1080 y 960×540, mientras que UFC usa 960×1080, 96×1080, 1600×900 y bloom
   separado (D).

**Descartada ≠ imposible.** Un escalador de escena **futuro** en el renderer (SSAA en el host para UFC)
sigue siendo viable en principio. Exigiría:
- detectar el inicio de escena UFC (1 MRT B10G11R11 + D32S8);
- resolver con filtro box/bilinear en vez de *nearest*;
- elegibilidad para las formas UFC;
- redirigir el `texelFetch` del composite con coordenadas escaladas;
- revisar descriptores, barreras, `T4Portrait`, readbacks y aliasing.

Es un desarrollo nuevo de esfuerzo y riesgo altos, y sin cambiar el contrato del motor los efectos de media
resolución seguirían a 1080. Solo se justificaría si try-1 y try-2 resultan inviables. Este punto no tiene
prueba manual ni rollback porque no hay nada que probar.

---

## 5. Matriz y condición para pasar al siguiente

| Orden | Try | Tipo | Valor | Riesgo | Esfuerzo | Corridas del usuario | Pasar al siguiente cuando… |
|---|---|---|---|---|---|---|---|
| 1 | **try-1b menú** | diagnóstico / posible solución sin parche | Muy alto | Bajo (copia de `user/`) | Muy bajo | 1 | Opción presente y RT > 1080 → validar la cadena y medir aparte (puede no ser 60 fps). Sin opción o sin efecto → 1c |
| 2 | try-1c sonda | diagnóstico | Alto | Medio (GDB) | Medio: reescribir la sonda + autorización | 1 | H1 confirmada → 2U. H1 descartada → resolver los escritores de G antes de 2A |
| 3 | try-2U "ultra" | variante (diseño) | Alto | Medio-alto (LOD/fps/heap) | Bajo (XML) | 1 | Cadena 1800 completa → 60 s comparables. Fracaso → 2A solo si (a) y (b) están resueltos |
| 4 | try-2A 1440 | variante (diseño) | Alto | Alto mientras composite y modo sigan abiertos | Bajo (XML) | 1 | Recorte → 2B. Cadena completa → escalón siguiente |
| 5 | try-2B | variante (diseño) | Medio (pierde Pro) | Medio | Bajo + control base | 2 | Comparar con Pro 1080 |
| 6 | try-2C | variante (diseño) | Bajo | Alto | Bajo | 1 | Solo si la rama 1800 aporta algo propio |
| — | try-3 | DESCARTADA | — | — | Alto (reformulación) | — | Solo si try-1 y try-2 son inviables |

---

## 6. Baseline (documentado, no medido aquí)

**Test 1** (Pro, VO 1080, cap 60, caché caliente, intervalos de flip del runtime), D:
- 60,00 FPS de media, 1% low 46,72;
- mediana 16,67 ms, p95 16,98, p99 17,64, máximo 36,84 ms;
- 6 intervalos > 25 ms, 0 > 50 ms, 0 compilaciones.

VRAM, CPU/GPU y latencia: NM. Tests 2 y 3 no son benchmarks. **Ningún resultado de try-1/2/3 está
medido.** Si una opción del juego activa 30 fps, esa corrida no es comparable con este baseline.

---

## 7. Bloqueos reales (actualizados)

**Retirados:**
- "UFC-36 no existe": su resultado está integrado en §1.
- "try-1 solo con GDB": 1b no necesita GDB.

**Vigentes:**
- 1c necesita autorización explícita para GDB y una sonda reescrita, porque el script de UFC-34 se perdió.
- Cualquier variante de try-2 necesita aprobación del Jefe, y luego una validación del XML con
  `patches.py` sobre una copia en `/tmp`, sin lanzar el juego.
- 2A: composite `texelFetch` y modo High/Low/escritor de los pares de G sin resolver.
- 2B: falta un control base medido.
- Heap del motor desconocido: bloquea 2160 (no 1440).
- Assets de UI comprimidos: la existencia de la opción Pro solo se puede confirmar en el menú (1b) o
  desempaquetando los `.big`, cosa que no se hizo.
- El informe principal y las capturas de Test 1–3 ya no se pueden releer: todo lo que dependa de ellos es D.

---

## 8. Única próxima prueba manual recomendada (no ejecutada; no hay nada que probar ahora)

**try-1b, menú de opciones en modo Pro con VO 2160.**
1. Copia aislada nueva de `user/` (la de Test 3 se perdió con `/tmp`).
2. Config de Test 3 sin sonda: `BB_NEO=1`, `BB_INTERNAL_WIDTH=3840`, `BB_INTERNAL_HEIGHT=2160`, ventana
   1920×1080, cap 60, `BB_WRITE_STEP=0`, write windows y preupload en 0.
3. En el menú principal, recorrer Settings/Preferences (y Visuals/Display si los hay). Anotar o fotografiar
   cualquier opción de resolución, rendimiento, calidad, Pro o framerate, con el texto exacto en pantalla.
4. **Si no existe:** cerrar y reportar "no hay opción". No hace falta captura.
5. **Si existe:**
   - Primero entrar a un combate con el valor por defecto y hacer `capture.trigger` en el ring (control).
   - Después cambiar solo esa opción a calidad o resolución, entrar al mismo combate y hacer otra captura
     en el ring.
   - Reportar si el juego avisa de un cambio de fps.
   - No medir rendimiento en esta corrida.
6. Revisión posterior (la hace el Jefe o un worker): inventario de los RT de las dos capturas según el
   criterio de cadena entera de §3.

---

## 9. Nota de integración (para el informe principal; no se tocó)

El informe principal `/tmp/CTR_vs_UFC3_ANALYSIS.md` ya no existe y no se pudo revisar. Si se reconstruye o
se mueve a `docs/`:
- Agregar en las secciones de resolución y en el plan una referencia a este anexo
  (`docs/ufc3/UFC-38-SUPERSAMPLING-TRIES.md`).
- La prioridad de resolución pasa de "consumidores del selector" (UFC-37) a **try-1b (menú) → try-1c
  (sonda) → try-2U/2A**.
- Corregir la recomendación de mancelli: en Pro no tiene efecto.

La misma nota está en `/tmp/ufc38-integration-note.md`, que es efímero.

UFC-40 (21:9) depende de esta revisión. Lo reutilizable es la cadena del selector y del modo, los dos
composites (`texelFetch` frente a `textureLod`) y la infraestructura de parches por firma.

---

## 10. Cinco acciones priorizadas

1. **Jefe:** revisar este anexo. Si lo aprueba, pedir al usuario **solo** try-1b (§8).
2. **Usuario (una corrida):** try-1b, el menú más dos capturas si la opción existe.
3. **Si no hay opción o no tiene efecto:** decidir si se autoriza GDB para try-1c y encargar reescribir la
   sonda de solo lectura de 4 BP.
4. **Según 1c:** con H1 confirmada, diseñar el XML 2U (o reutilizar el setter identificado). Con H1
   descartada, resolver estáticamente los escritores de G+0x4c..0x70 y la elección de composite antes de
   tocar 2A.
5. **Solo con una variante estable a 1440 o 1800 y la cadena entera verificada:** revisar el heap y
   plantear 2160. try-3 sigue descartada salvo que try-1 y try-2 sean inviables. 21:9 (UFC-40) va después.
