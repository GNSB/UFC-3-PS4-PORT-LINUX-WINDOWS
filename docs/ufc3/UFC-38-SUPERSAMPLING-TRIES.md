# UFC-38 · Supersampling interno → salida 1080: alternativas try-1 / try-2 / try-3

Anexo del informe CTR/UFC (UFC-31/37). Ticket UFC-38, Claude 3, 16 oct 2026. **Tercera pasada**: integra
la prueba **try-1b** que hicieron el usuario y el Jefe (menú + captura en el ring) y reconcilia todo lo
anterior con ella. **Solo documentación**: en esta pasada no ejecuté el juego, no compilé, no usé GDB, no
creé parches, scripts ni configuraciones y no toqué el proceso que sigue corriendo. Leí directamente los
artefactos de `logs/try1b-pro-options/` del repo principal; no copié ninguno al commit.

**Resultado en una línea.** La opción existe: **VIDEO → RESOLUTION → *Favor Resolution*** (dos valores:
*Favor Resolution* / *Favor Performance*). Con *Favor Resolution*, Pro y VideoOut 2160, el primer frame
capturado en el ring renderiza la escena a **3200×1800** y el propio juego la reduce a la salida
**1920×1080** en el composite final. Eso ya es supersampling 1800→1080 **sin parches**. Falta el control
*Favor Performance*, la revisión visual, el coste y la estabilidad.

Convenciones de certeza:
**V** = releído por mí en esta pasada (captura, logs, imágenes, bytes, SPIR-V, código del repo) ·
**D** = documentado por otro ticket, artefacto ya no disponible · **I** = inferencia razonada ·
**H** = hipótesis · **NM** = no medido.

---

## 0. Procedencia y límites de la evidencia

### 0.1 try-1b (nuevo, V salvo indicación)

Directorio: `/home/myzk/Projects/ufc3-native/logs/try1b-pro-options/` (repo principal, no versionado).

| Artefacto | Qué contiene (leído por mí) |
|---|---|
| `menu-favor-resolution.png` (1834×747) | Pestaña **VIDEO**, única fila **RESOLUTION** = `‹ Favor Resolution ›`; ayuda: *"Select between rendering the game with a smoother framerate or at a higher resolution."* El usuario confirma que solo hay dos valores: *Favor Resolution* y *Favor Performance* |
| `capture/frame_1792163000.txt` | SHA-256 `62833f1ea7673759b345b1daf9dc35872c84a63b86cf1a4f6c740c972474fb26` (V, coincide). 198 pases `#0..#197` (77 PASS, 121 COMPUTE). `presented buffer 0x109a470000` |
| `ring-favor-resolution.png` (1920×1080) | Captura **solo de la ventana** del juego en combate: HUD (nombres, stamina, reloj 3:54, aviso de tutorial) y overlay `60 FPS 16.7 ms` del host |
| `launch-capture.log` | `Frame capture: armed` → `Frame capture: 198 passes written to …/frame_1792163000.txt`; `PS4 Pro mode: 1`; `Window: 1920x1080 … the game's output 1920x1080 scaled to it`; `VideoOut: … frame limit 60 FPS`; `single-stepped writes … off (BB_WRITE_STEP)`; `Patches: 0 writes, 0 bytes applied`; `Settings: …/bbport.ini` (`show_fps=1`) |
| `launch.log` | Sesión anterior (11:58) con la misma cabecera y sin captura |

**Configuración declarada por el Jefe** (base `ufc3-online` `8de5098`, binarios Linux prebuilt originales):
`BB_NEO=1`, `BB_INTERNAL_WIDTH/HEIGHT=3840/2160`, ventana 1920×1080, `BB_FPS_LIMIT=60`, `BB_WRITE_STEP=0`,
write windows 0, preupload 0, `BB_WRITE_LOG=0`, mods 0, `BB_CAPTURE_ON_FLIP=1`; `BB_ONLINE_SERVER` y
`BB_DETERMINISTIC_FP` sin definir; `user/` y `bbport.ini` como copia aislada en el mismo directorio.
**El log es parcial:** confirma Neo 1, ventana, límite 60, write step off, 0 parches y el `bbport.ini`
aislado. **No** imprime `BB_INTERNAL_*`, preupload, write log, mods ni el entorno online/FP. Esos valores
quedan como D (declarados por el Jefe).

**Dos sesiones (I).** Hay dos logs con cabecera idéntica (11:58 sin captura, 12:01 con captura), y la
foto del menú es de las 12:00. Lo más probable es que la opción se eligiera en la primera sesión y quedara
guardada en la copia aislada de `user/`. No verifiqué en la partida que el valor persistiera; lo que sí
consta es que el usuario eligió *Favor Resolution* y avisó "on ring" antes del trigger.

### 0.2 Evidencia anterior (segunda pasada)

`/tmp` se vació entre pasadas: la primera versión de este anexo, `ufc3-resolution-consumers.md`, la sonda y
las capturas de Test 1–3, `CTR_vs_UFC3_ANALYSIS.md`, `/tmp/ufc36/` y el script GDB de UFC-34 ya no
existen, y este documento no los enlaza como si existieran. Su texto se recuperó de los logs del
despachador (`~/.local/share/ufc3-flow/logs/UFC-38.log`, `UFC-36.log`) y del tablero.

| Evidencia | Estado | Detalle |
|---|---|---|
| Repo `ufc3-online` | V | `8de5098b464d886571c338530a4ed273c5395521` |
| `out/eboot.elf` | V | SHA-256 `08a4ffbc…c18f`, el mismo de las Pruebas 1–3 |
| `out/sacredfire`, `out/gpu/libbbgpu.so` | V | `3a88e043…`, `89c01777…` |
| Bytes clave del eboot (§2) | V | offset de archivo = vaddr + `0x4000` |
| Composite `ddbe162b` | V | `user/cache/CUSA06534/0x00000000ddbe162b_0.spv`: `texelFetch` en la misma coordenada para sus dos entradas |
| Composite `e5b29094` | V | `user/shader/dumps/cs_0x00000000e5b29094_0.spv`: `texelFetch` para la UI, `textureLod` con coordenada normalizada para la escena |
| Sonda Test 3, capturas Test 1/2/3 | **D** | selector en rama 1800p; escena/depth/composite a 1920×1080 en esas corridas |
| Métricas Test 1 | **D** | baseline de §7 |
| Desensamblado de consumidores de UFC-36 | **D** | `0x428b90`, setters, `0x118b60`, getters |

---

## 1. Resultado de try-1b: opción confirmada y primer frame positivo

### 1.1 Cadena del frame capturado (V, inventariada desde el texto de la captura; no se volcaron buffers)

| Pases | Target | Formato | Tamaño | Papel (I por formato/uso) |
|---|---|---|---|---|
| #128–133 | depth `0x1085980000` | D32SfloatS8Uint | **3200×1800** | prepass / depth de escena |
| #129–133 | color `0x108e8c0000` | B10G11R11UfloatPack32 | **3200×1800** | escena HDR |
| #135 / #140–161 | `0x10840a0000` | escrito R32Sfloat (#135), luego depth D32S8 | 3200×1800 | depth principal (alias de dirección) |
| #136 | write `0x108d140000` | B10G11R11 | 3200×1800 | compute sobre escena |
| #137–140 | `0x1082920000` | R8G8B8A8Unorm | 3200×1800 | buffer auxiliar a resolución completa |
| #141 / #142 | `0x1073a80000` / `0x1075890000` | R32Sfloat | 1600×900 / 800×450 | linear depth y su mip |
| #143–146 | `0x1075c70000`, `0x1075b20000` | R16Sfloat | 800×450 | efectos a cuarto de resolución |
| #147–171 | color `0x107c770000` | B10G11R11 | **3200×1800** | escena HDR final (entrada del tonemap) |
| #162 | `0x1076d20000` | R16G16B16A16Sfloat | **1600×1800** | media anchura (análogo del 960×1080 de 1080p) |
| #163–166 | `0x1076800000`, `0x10762e0000`, `0x1075dc0000` | R16G16B16A16Sfloat | 800×450 | cadena de bloom |
| #167 | `0x107def0000` | B10G11R11 | 3200×1800 | — |
| #168 | `0x1080f80000` | R8G8B8A8Unorm | **160×1800** | 1/20 de anchura (análogo del 96×1080) |
| #169–170 | `0x1081170000`, `0x1080f50000` | R8G8B8A8Unorm | 160×90 | reducción (¿luminancia?) |
| **#172** | color **`0x109ffe0000`** | **R8G8B8A8Unorm** | **3200×1800** | **tonemap**: muestrea la escena `0x107c770000` y el depth `0x10840a0000` |
| #173–196 | color **`0x109eee0000`** + depth `0x109c468000` | R8G8B8A8Unorm + D32S8 | **1920×1080** | **UI/HUD** |
| **#197** | COMPUTE **`e5b29094`**, groups **240×135×1** | — | — | **composite final** |

**Pase final #197 (V, literal de la captura):**
```
#197 COMPUTE dispatches 1, cs 00000000e5b29094
  samples 0x109eee0000 R8G8B8A8Unorm 1920x1080      ← UI
  samples 0x109ffe0000 R8G8B8A8Unorm 3200x1800      ← escena tonemapeada
  writes  0x109a470000 R8G8B8A8Srgb 1920x1080       ← = presented buffer
  note: groups 240x135x1                            ← 240·8 × 135·8 = 1920×1080
```

**Comprobaciones.** Todas las dimensiones y direcciones que pasó el Jefe coinciden con la captura (escena
B10G11R11 y depth D32S8 a 3200×1800; tonemap RGBA8 `0x109ffe0000` a 3200×1800; UI `0x109eee0000` a
1920×1080; escritura sRGB 1920×1080 en `0x109a470000` = buffer presentado; 198 pases; SHA-256).
No hay ningún target 3840×2160 (0 coincidencias de `3840x2160`; las 15 de `2160` son ruido de valores
float en volcados de buffers). Las formas derivadas cambian de forma proporcional a 1080p
(960→1600 = ½ anchura, 96→160 = 1/20, 1600×900 se mantiene como linear depth). La imagen del ring muestra
el combate y el HUD sin recorte ni zoom aparentes (revisión visual de **una** imagen; ver §1.3).

### 1.2 Qué demuestra y qué no

**Demuestra (V):**
- Existe una opción del juego, sin parches (`Patches: 0 writes`), que en Pro con VideoOut 2160 hace que la
  escena se renderice a 3200×1800 dentro de un frame de combate.
- La reducción 1800→1080 la hace **el juego**, en el composite `e5b29094`: lee la escena con `textureLod` y
  coordenada normalizada (SPIR-V, V) y escribe 1920×1080. Ahí se componen también la UI a 1080 (leída con
  `texelFetch`) y la escena.
- El host presenta ya 1920×1080 en una ventana de 1920×1080 (`the game's output 1920x1080 scaled to it`):
  el presenter **no** escala en esta corrida. Hay que separar dos etapas:
  1. la reducción interna del juego en el composite (3200×1800 → 1920×1080);
  2. el presenter del host, que aquí trabaja 1:1.
- **Corrección de la segunda pasada:** llamé "composite base" a `e5b29094` y "composite Pro" a
  `ddbe162b`. Es inexacto. `e5b29094` aparece aquí con `PS4 Pro mode: 1` y no implica `BB_NEO=0`. Lo que
  la evidencia sostiene (I) es que el motor elige el composite **según la relación entre escena y salida**
  (`ddbe162b` con `texelFetch` cuando la escena es 1080 en Test 1; `e5b29094` con `textureLod` cuando no
  lo es). Por lo tanto el riesgo de recorte de §4 (2A) **no aplica a la ruta que eligió el juego**.

**No demuestra:**
- **No es 4K nativo** y no hay evidencia de checkerboard: ningún target 3840×2160, ningún pase de
  reconstrucción identificado. 3200×1800 es el set "High" de §2 (I).
- **No es un benchmark.** El overlay `60 FPS 16.7 ms` es un valor instantáneo, no una medición. El log
  tiene líneas `Frame stats` y `Frame pacing`, pero cubren toda la sesión (menús incluidos) y no se
  analizaron como benchmark. FPS en combate con *Favor Resolution*: **NM**.
- **No demuestra estabilidad**: un frame, no 20 minutos ni un combate completo.
- **No prueba H1 a nivel binario**: no se trazó qué setter (`0x4a0a60` u otro), qué campo
  (`R+0x92ec`, `LodManager+0x44`) ni qué modo (`G+0x90`) escribe la opción. Es coherente con H1 (I), no
  prueba.
- **No es un contraste controlado**: falta el mismo frame con *Favor Performance* y la misma configuración
  (§9).
- **No caracteriza la calidad del filtro**: el filtro del sampler de `e5b29094` (lineal/punto, mips) no
  está inventariado; con razón 1,67 un solo tap bilineal promedia solo en parte (H).

### 1.3 Pendientes que deja try-1b (no ejecutar ni pedir todos juntos)

1. **Control *Favor Performance*** con la misma env y una captura en el ring: confirma que la opción es la
   variable (esperado, H: escena 1920×1080 y composite `ddbe162b`). Es la próxima prueba (§9).
2. **Revisión visual**: una sola imagen de 1920×1080 con HUD y combate visibles no basta para prometer
   ausencia total de recorte, desalineación de UI o artefactos en cada pantalla. Hacen falta selección de
   luchador (retratos), entrada al octágono, repeticiones y menú de pausa.
3. **Coste global** (FPS en combate, frametimes, VRAM, GPU): **NM**. Solo después del control y con el
   método de Test 1. El juego describe la opción como "smoother framerate **or** higher resolution": no se
   asume 60 fps estables ni 30 fijos.
4. **Estabilidad**: combate completo y sesión larga sin `Guest fault`, `STOP` ni OOM.
5. **Retratos** (`T4Portrait`, comparación exacta de tamaños, `texture_cache.cpp:106–109`): sin verificar
   con la escena a 1800.
6. **Online**: no usar *Favor Resolution* en online hasta probarlo offline; no se asume que sea seguro ni
   que produzca desync.

---

## 2. Cadena estática de UFC-36 (D; bytes marcados V) — sigue siendo el modelo explicativo

```
selector 0x533180  (Neo ∧ Open>0 ∧ Query ok ∧ alto==2160 → rama 1800p)
  S+0x38/3c = 3200x1800   S+0x40/44 = 1600x900   S+0x48..0x5c = 1920x1080   S+0x67 = 1
  desc+0x5c/60 = 3200x1800   desc+0x78/7c y +0x90/94 = 1920x1080
    └─ 0x10d300 → 0x118b60: "RNA:VideoOutTex_High" = 3200x1800, "RNA:VideoOutTex_Low" = 1920x1080
vfunc 0x428b90
  r13d = [LodManager+0x44]                         (0x428bf9: 44 8b 68 44, V)
  Neo: r13d≠0 → modo 2 · r13d==0 ∧ R+0x92ec==1 → modo 2 · r13d==0 ∧ R+0x92ec≠1 → modo 1
       (0x428c3b: 83 bb ec 92 00 00 01, V)
  R+0x92ec = 1 por defecto en el ctor              (0x4258db: 41 c7 87 ec 92 00 00 01 00 00 00, V)
  G+0x90 = modo; tabla 0x2505200: 0 "medium", 1 "ultra", 2 "high"
  jmp 0x112130 (esi = modo==2)                     (0x428cc1, V)
     modo 1 "ultra" → set High (3200x1800) · modo 2 "high" → set Low (1920x1080)
setters virtuales: 0x4a0a50 → LodManager+0x40 (copiado a +0x44); 0x4a0a60 → R+0x92ec
```

**Lectura con try-1b (I).** Por defecto el juego queda en "high" (set Low 1080), lo que coincide con Tests
1–3 (D). *Favor Resolution* produce exactamente el tamaño del set High (3200×1800). La correspondencia más
simple es *Favor Performance* ↔ "high" (modo 2) y *Favor Resolution* ↔ "ultra" (modo 1), escrita
probablemente por `0x4a0a60`. **Sigue siendo H** hasta trazarla. Ya no hace falta para obtener 1800p, pero
sí para explicar el mecanismo o reutilizarlo sin menú.

**§1.2 de la segunda pasada, corregido.** El usuario recordaba "4K/30 con checkerboard". El menú real dice
"smoother framerate or a higher resolution": no menciona 4K, 30 fps ni checkerboard, y la captura no
muestra nada de eso. La búsqueda estática de cadenas no la encontró porque el texto vive en los assets
comprimidos de UI, no en el eboot (I).

---

## 3. try-1 · Diagnóstico de la ruta nativa 1800p / modo Pro

- **1a · Estático:** hecho (§2).
- **1b · Menú: HECHO, positivo** (§1). Opción presente y el primer frame con escena a 3200×1800 → salida
  1080. Falta el control *Favor Performance* (§9).
- **1c · Sonda GDB de solo lectura: degradada a opcional.** Ya no es necesaria para obtener 1800p. Solo
  tendría sentido para confirmar H1 a nivel binario (qué setter/campo escribe la opción) o para forzar el
  modo sin menú (por ejemplo, para defaults del launcher). Sigue requiriendo autorización explícita y
  reescribir el script de UFC-34 (perdido). Diseño de 4 BP sin cambios:

  | BP | Runtime | Registrar |
  |---|---|---|
  | 1 | `0x800428c9d` (call `0x122c410`) | `esi` (modo), `r13d`, `[rbx+0x92ec]`, `r12d` |
  | 2 | `0x800112182` (`0x112160` tras el prólogo) | `esi` (0 High / 1 Low), `[r12+0x5c]` |
  | 3 | `0x80048d264` (tras los getters) | dimensiones, `[G+0x90]`, `G+0x4c..0x70` (G = `[0x802982560]`) |
  | 4 | `0x8004a0a8b` (setter de `R+0x92ec`) | `ebx` y dirección de retorno |

  Con la opción conocida, la sonda se haría cambiando el menú en caliente (BP 4 debería saltar al pasar de
  *Performance* a *Resolution*, H).

---

## 4. try-2 · Parches por firma — diseños condicionados, NO recomendados ahora

**No se creó XML ni `patches.bin`. No autorizados.** try-1b obtuvo sin parches lo que 2U buscaba y una
forma más coherente de lo que buscaba 2A. **No se recomienda ningún parche para conseguir lo ya
observado.** Las sub-variantes quedan como diseño, solo para casos que el menú no cubre.

| | Sitio (V) | Para qué quedaría | Condición para reabrirlo |
|---|---|---|---|
| **2U** forzar "ultra" | default `R+0x92ec` en `0x4258db` | Tener "ultra" sin pasar por el menú | Solo si hiciera falta un default sin interacción **y** 1c confirmara H1. Mientras tanto, el menú lo cubre |
| **2A** fallback Pro a W×H | imm `0x53325b` / `0x53326d` | Tamaños distintos de 1800 (1440, 2160) | Solo si 1800 resulta insuficiente o demasiado caro tras medir. Ahora se sabe que el motor tiene una ruta de composite que escala (`e5b29094`), pero sigue abierto si un W×H arbitrario la activa y si el modo usa los pares `lowres` de G |
| **2B** base, sitio mancelli | `0x533260` / `0x533265` | Supersampling sin Pro | Sin motivo actual: pierde Pro y el menú ya da 1800 en Pro |
| **2C** rama 1800p | `0x533239` / `0x533234` | — | Última; mezcla tamaños incoherentes |

Se mantienen del diseño anterior:
- La infraestructura existe (`scripts/patches.py:75` `locate()`; `run.sh:119`) y el rollback se verifica
  con `Patches: 0 writes`.
- **El parche de mancelli no tiene efecto en Pro**: los `cmovne` pisan sus inmediatos.
- Licencias: XML propio con firma de unos 29 bytes; acreditar la idea del sitio a mancelli sin copiar su
  C++; nada de CTR ni del eboot publicado.
- Escalonado: si alguna vez se reabre 2A, 1440 primero y 2160 solo con la cadena entera estable y el heap
  revisado.

Estimación de memoria de la segunda pasada (D/H, de una captura 1080): unos 116 MB de targets a 1080 y unos
464 MB a 2160. A 1800 sería del orden de 116 × 2,78 ≈ 320 MB (I, sin medir). No se midió VRAM en try-1b.

---

## 5. try-3 · Escalado de escena en el renderer — DESCARTADA (sin cambios)

1. No se activa en UFC: exige ≥5 MRT al inicio de escena (`vk_rasterizer.cpp:3771–3775`, V).
2. Está pensada para reducir y controlada por el temporal, forzado a Off en UFC (D).
3. Resuelve hacia el nativo con *nearest* (`vk_scene_resolution.cpp:215`, V).
4. Solo acepta 1920×1080 y 960×540.

Con try-1b tiene todavía menos sentido: el juego ya hace la cadena completa a 1800 (efectos de media
resolución incluidos, 1600×1800 / 800×450) y la reduce en su composite. Un SSAA del host solo se
justificaría para resoluciones que el juego no ofrece, y ni así antes de 2A.

---

## 6. Matriz y orden actualizados

| Orden | Paso | Tipo | Estado | Riesgo | Corridas | Pasar al siguiente cuando… |
|---|---|---|---|---|---|---|
| ✔ | try-1b menú | diagnóstico / solución sin parche | **Hecho: opción confirmada + primer frame 3200×1800 → 1080** | Bajo | — | — |
| 1 | **Control *Favor Performance*** (§9) | control de una variable | Pendiente | Bajo | 1 | Escena a 1080 con la misma env → la opción es la causa. Si sigue a 1800 → revisar persistencia o config |
| 2 | Revisión visual + estabilidad con *Favor Resolution* | validación | Pendiente | Bajo | 1–2 | Sin recorte/HUD roto en pantallas clave y combate completo sin faults |
| 3 | Benchmark *Favor Resolution* vs *Favor Performance* | medición (método Test 1) | Pendiente | Bajo | 2 (una por corrida) | Coste conocido; decidir si queda por defecto en el launcher |
| 4 | try-1c sonda | diagnóstico opcional | Condicionado | Medio (GDB) | 1 | Solo si hace falta el mecanismo exacto (default sin menú) |
| 5 | try-2U / 2A | variantes de parche | Diseño condicionado | Medio-alto | 1 c/u | Solo con una necesidad que el menú no cubra (§4) |
| 6 | try-2B / 2C | variantes de parche | Sin motivo actual | — | — | — |
| — | try-3 | — | DESCARTADA | — | — | — |

---

## 7. Baseline (documentado, no medido aquí)

**Test 1** (Pro, VO 1080, cap 60, caché caliente, intervalos de flip del runtime), D:
- 60,00 FPS de media, 1% low 46,72;
- mediana 16,67 ms, p95 16,98, p99 17,64, máximo 36,84 ms;
- 6 intervalos > 25 ms, 0 > 50 ms, 0 compilaciones.

try-1b **no es comparable** con este baseline: no hubo benchmark, la VO es distinta (2160) y el overlay es
instantáneo. La comparación válida será *Favor Resolution* contra *Favor Performance* con la misma env de
try-1b, en corridas separadas y con el mismo método de Test 1.

---

## 8. Bloqueos y pendientes reales

**Retirados con try-1b:**
- "La opción Pro no está confirmada" / "no se puede confirmar sin desempaquetar los `.big`": confirmada en
  el menú.
- "Ningún RT > 1080 en las capturas": era cierto para Tests 1–3 con la opción por defecto. Con *Favor
  Resolution* la escena está a 3200×1800.
- "2A: composite `texelFetch` recorta": el juego usa `e5b29094` (`textureLod`) cuando la escena es mayor.
- "1c es el paso siguiente": pasa a ser opcional.

**Vigentes:**
- Falta el control *Favor Performance* con la misma env (§9).
- Revisión visual completa, estabilidad, coste global y retratos: NM (§1.3).
- El filtro del composite `e5b29094` (sampler) no está inventariado: la calidad de la reducción 1,67:1 no
  está caracterizada.
- H1 binaria (setter/campo/modo) sin traza; 1c requiere autorización y reescribir la sonda.
- Cualquier parche de try-2 requiere aprobación del Jefe y no se recomienda ahora.
- Heap del motor desconocido: bloquea 2160 vía 2A.
- El informe principal y las capturas de Test 1–3 ya no se pueden releer: todo lo que dependa de ellos es D.

---

## 9. Próxima prueba sugerida (una sola; no ejecutada)

**Control *Favor Performance*, misma env que try-1b, captura en el ring.** Va antes de cualquier benchmark.

1. Misma configuración exacta de try-1b (§0.1), con la misma copia aislada de `user/` y el mismo
   `bbport.ini`. La única variable es el menú.
2. En VIDEO → RESOLUTION, poner **Favor Performance**. Foto del menú.
3. Entrar a un combate equivalente (mismo modo/arena si se puede). En el ring, `capture.trigger` con
   `BB_CAPTURE_ON_FLIP=1`.
4. No medir rendimiento en esta corrida.
5. Revisión posterior (Jefe o worker), solo del texto de la captura:
   - dimensiones de escena, depth y tonemap;
   - shader del pase final (esperado, H: `ddbe162b` con escena 1920×1080; si aparece `e5b29094` con escena
     1080, también vale);
   - que el buffer presentado siga a 1920×1080 sRGB.

**Lectura:**
- Escena a 1080 → la opción es la causa del 1800 (contraste de una variable).
- Escena a 1800 con *Performance* → revisar si el valor persistió o si hay otra variable de por medio,
  antes de seguir.

Después, por separado: revisión visual y estabilidad, y luego el benchmark de las dos opciones (§6).

---

## 10. Nota de integración y UFC-40 (21:9)

**Informe principal** (`/tmp/CTR_vs_UFC3_ANALYSIS.md` ya no existe; si se reconstruye o se mueve a `docs/`):
- Referenciar este anexo.
- La prioridad de resolución pasa a ser: **opción *Favor Resolution* (Pro + VO 2160) → control → validación
  → benchmark**. Los parches quedan solo para necesidades que el menú no cubre.
- Corregir: el parche de mancelli no tiene efecto en Pro; `e5b29094` no es exclusivo del modo base.

**UFC-40 (21:9).** Puede reutilizar el **contrato ya observado** del composite que escala: escena a W×H
leída con `textureLod` normalizado, UI a 1920×1080 leída con `texelFetch`, salida 1920×1080 con groups
240×135. Eso indica (I) que el motor ya separa el tamaño de escena del de salida. **No se promete
ultrawide funcional**: no hay evidencia de cambio de aspecto, FOV, HUD anclado ni de una salida que no sea
16:9.

---

## 11. Acciones priorizadas

1. **Jefe:** revisar esta tercera pasada. Si la aprueba, pedir al usuario **solo** el control *Favor
   Performance* (§9).
2. **Worker (después):** inventariar la captura de control y contrastarla con §1.1, sin volcar buffers.
3. **Usuario (corrida aparte):** validación visual y estabilidad con *Favor Resolution* (§1.3, puntos 2,
   4 y 5).
4. **Después:** benchmark de *Favor Resolution* contra *Favor Performance* con el método de Test 1, una
   opción por corrida. Con eso, decidir si el launcher la recomienda o deja Pro + VO 2160 como preset.
5. **Solo si aparece una necesidad que el menú no cubre** (default sin interacción, otro tamaño): 1c y
   luego 2U/2A, con autorización. try-3 sigue descartada. UFC-40 reutiliza el contrato de §10 sin
   prometer 21:9.
