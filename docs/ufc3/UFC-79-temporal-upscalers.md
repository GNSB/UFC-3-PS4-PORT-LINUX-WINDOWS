# UFC-79 · Factibilidad de upscalers temporales (issue #46)

Investigación del 20 de octubre de 2026 sobre `ufc3-online` 0.1.10, HEAD
`73d3bfbfb378beea18c025dee59bf088f1192401`. Submódulo `gpu/third_party/fsr-vulkan`:
`c64f093404125e960813f95f1996c14780f0dd69`. Solo documentación: sin cambios del renderer,
compilaciones, ejecución del juego ni benchmarks. No se publicó ningún comentario en GitHub.
El [issue #46](https://github.com/GNSB/UFC-3-PS4-PORT-LINUX-WINDOWS/issues/46) no pudo
recuperarse mediante la consulta web; el alcance usado es el brief del ticket.

Convenciones: **V** = verificado en este checkout; **D** = documentado previamente, sin
repetir la prueba; **I** = inferencia; **NM** = no medido en UFC 3. Las fuentes externas
se consultaron en la fecha anterior; sus ramas `main` y matrices de soporte pueden cambiar.

## Decisión

**Priorizar FSR 3.1 como futuro upscaler temporal común para Linux y Windows, pero no
ofrecerlo todavía como función de UFC 3.** Mantener Off / FSR 1.0 espacial mientras se
investigan los datos temporales del juego. El backend Vulkan reutilizable ya existe;
la integración específica de UFC 3 no está resuelta. FSR 4 queda como investigación
posterior y DLSS/XeSS como opciones condicionadas por plataforma y licencia.

**No tenemos demostrado el conjunto motion vectors + depth + jitter que necesita un
upscaler temporal.** Hay profundidad de escena identificada y código heredado para generar
vectores y jitter; eso no equivale a entradas correctas de UFC 3. Tampoco está demostrado
que el juego carezca de un buffer de velocidad aprovechable: falta identificarlo o
descartarlo con evidencia. No alcanza con cambiar un selector del launcher.

## 1. Qué existe realmente en el port

### 1.1 Resolución y profundidad de UFC 3

Se leyeron primero [UFC-38](UFC-38-SUPERSAMPLING-TRIES.md) y
[UFC-40](UFC-40-ULTRAWIDE-FEASIBILITY.md). No hay un documento autónomo `UFC-36*`
en `docs/ufc3/` de esta base: su investigación de resolución de escena está conservada
en UFC-38 §0.2 y §2, que distingue evidencia recuperada de verificaciones posteriores.
No se accedió al repo principal ni se trataron sus logs ausentes como mediciones nuevas.

UFC-38 documenta, en la misma ejecución, escena/depth a 3200×1800 con *Favor Resolution*
y a 1920×1080 con *Favor Performance*, manteniendo UI y salida a 1920×1080 (**D** aquí).
La escena HDR es B10G11R11; aparece depth D32S8 y un alias R32F → D32S8. El tonemap
produce RGBA8 y el composite final combina escena y UI: `e5b29094` cuando difieren los
tamaños, `ddbe162b` en el caso 1080→1080. Esto ofrece candidatos de inserción antes de
UI, no una validación de reconstrucción temporal. El linear depth no sigue siempre el
tamaño de escena. UFC-40 advierte además sobre tamaños/aspect y hardcodes 1920×1080.

| Entrada o contrato | Evidencia actual | Falta para UFC 3 |
| --- | --- | --- |
| Color de escena sin HUD | Escena HDR y tonemap separados de UI en UFC-38 (**D**) | Elegir pase, espacio de color y exposición; comprobar AA/blur previos y todos los modos de cámara |
| Depth | Targets y alias identificados (**D**); texture cache y vistas Vulkan existen (**V**) | Validar correspondencia por píxel con color, sampleo del alias vigente, rango, reversed/infinite depth, near/far/FOV |
| Motion vectors | `CameraMotion` y `ObjectMotion` producen imágenes, con supuestos de Bloodborne (**V**) | Localizar velocidad nativa o adaptar cámara actual/anterior y movimiento de objetos, incluidos huesos/deformación de luchadores |
| Jitter | Halton y desplazamiento del viewport implementados (**V**) | Aplicarlo solo a geometría de UFC, confirmar signo, unidades y cobertura; evitar doble jitter/TAA |
| Historia y cortes | Reset y recursos temporales implementados (**V**) | Detectar cortes, replay, entradas, menús, resize y cambios de resolución de UFC |
| Transparencias/UI | Hay máscara reactiva y composición tardía heredadas (**V**) | Identificar sudor, pelo, partículas, transparencias y HUD del juego; no acumular UI en la historia |

### 1.2 Estado de `vk_temporal_upscaler`

No es un stub. Está compilado por el glob de `VIDEO_CORE` en
[gpu/CMakeLists.txt](../../gpu/CMakeLists.txt), instanciado en `Rasterizer` y enlazado con
`ffx-vulkan::portable` y `ffx-vulkan::fsr4-v07-vulkan`. El presenter consulta
`DisplayOverride`. Tiene FSR 3.1, TAA propio y selección de FSR 4 v07 / replay 4.1.1,
recursos/historia, escalado de targets host, máscaras reactivas y rutas de fallback (**V**).

La limitación decisiva está explícita en
[bbport_settings.cpp](../../gpu/shim/bbport_settings.cpp), `BbSettings::Load`:
después de leer el INI fuerza `upscaler = UpscalerOff` y `object_motion = false`
porque los targets, constantes y streams pertenecen al juego para el que se construyó
bbport. Las variables de entorno posteriores permiten experimentos; no constituyen
soporte validado. El constructor temporal puede imprimir «available» aun con la opción
Off; ese mensaje no prueba un dispatch ni calidad en UFC 3.

Dependencias concretas de la implementación heredada (**V**):

- [vk_temporal_upscaler.h/.cpp](../../gpu/shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.cpp):
  trigger compute por defecto `0x9a9cf8a9`, trigger VS de UI `0x34e8a281`, rutas
  1920×1080 y validaciones de color RGBA16F. El contrato de UFC documentado arriba es
  distinto. `Run`/`RunScaled` llaman a `RecordMotion`, describen color/depth/motion/output
  y despachan el contexto FSR. Requieren cámara disponible; no reconstruyen esa cámara
  a partir de cualquier juego automáticamente.
- [vk_camera_motion.cpp](../../gpu/shadps4/video_core/renderer_vulkan/vk_camera_motion.cpp):
  reconoce constantes con far=3000, inverso de far, tamaños en floats 4/5; lee view en
  8, inverse view en 180 y proyección en 52/57/62/63. La detección de G-buffer depende
  de al menos cinco MRT. `Ready()` requiere cámaras actual/anterior válidas y depth.
  No se comprobó ese layout en UFC.
- [camera_motion.comp](../../gpu/shadps4/video_core/host_shaders/camera_motion.comp):
  reconstruye posición desde depth y reproyecta con cámaras; emite RG16F en píxeles,
  anterior menos actual, sin jitter. Acepta movimiento de objetos si hay un valor
  válido y depth coincidente. **Cámara sola no describe puños, piernas, caras ni tela
  animados con cámara quieta**; usar ceros o solo reproyección de cámara degradaría la
  historia precisamente en el contenido principal de UFC (**I**).
- [vk_rasterizer.cpp](../../gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp):
  aplica el jitter como desplazamiento del viewport en geometría seleccionada por
  depth, con exclusiones de pases de pantalla completa; genera valores Halton desde
  el upscaler y mantiene la UI fuera. Hay que adaptar esas reglas, no solo pasar
  `jitterOffset` al SDK.

El launcher Windows confirma la oferta actual en
[sacredfire_launcher.c](../../packaging/windows/sacredfire_launcher.c):
`Off (bilinear)` / `AMD FSR 1.0`, mediante `BB_FSR1` y RCAS. Linux documenta esas
variables en [ufc3.conf.example](../../packaging/linux/ufc3.conf.example).
FSR1 se ejecuta en el presenter sobre la imagen final al ampliar; no necesita historia.
Los restos de launcher/strings de bbport para otros juegos no prueban soporte temporal
en el launcher de UFC.

## 2. Tabla de opciones

Esfuerzo relativo para una futura implementación por una persona: **medio** = adaptación
acotada tras validar inputs; **alto** = investigación de pases/animación más integración;
**muy alto** = además presenter o plataforma/proveedor nuevo. No son estimaciones de calendario.

| Opción | Requisitos y plataforma | Licencia / distribución GPL-2.0-or-later | Esfuerzo | Riesgo y decisión |
| --- | --- | --- | --- | --- |
| FSR 1.0 actual | Color final; Vulkan Linux/Windows, sin MV/depth/jitter | Shaders permisivos existentes; conservar avisos | Ya integrado | Bajo; mantener como fallback espacial |
| FSR 3.1 SR | Color, depth, MV, jitter, reset; AMD/NVIDIA/Intel con capacidades Vulkan apropiadas | SDK 1.1.4 y capa portable MIT; compatibles preservando avisos. Revisar por archivo las importaciones del SDK nuevo | Alto en inputs UFC; medio en backend ya existente | Mejor candidato común; aún sin activar para usuarios |
| FSR 3 FG analítico | Inputs temporales + color sin UI, optical flow, dos presentaciones y pacing | Código MIT seleccionado; la política WSI pertenece al host | Muy alto | No viable con el present actual sin modificarlo; separar de SR |
| FSR 4 oficial 4.1.1 | Oficialmente RX 7000 RDNA3 y RX 9000 RDNA4 discretas; proveedor firmado DX12 | Binarios AMD con términos restrictivos; no confundir con fuentes MIT seleccionadas | Muy alto para Vulkan nativo oficial | Sin integración oficial Vulkan demostrada aquí; no distribuir como parte GPL sin resolver licencia |
| FSR 4 v07 Vulkan | Color/depth/MV/jitter, capacidades de shaders y bundle externo v07 | Wrapper MIT; shaders/pesos externos requieren procedencia y términos propios | Alto tras inputs; assets aparte | Experimental, no equivale a FSR 4.1.1; posponer |
| FSR 4.1.1 replay `fsr411` | Mismos inputs; INT8 + extensiones concretas o FP8 matrices; assets de DLL | Código host GPL; extracción/conversión y redistribución de payload AMD sin permiso demostrado | Alto/muy alto | Ejecutabilidad técnica no resuelve licencia ni calidad UFC; posponer |
| DLSS SR / DLAA | RTX, color/depth/MV/jitter/exposición y NGX; Vulkan nativo Linux/Windows | Streamline MIT no cubre DLSS/NGX propietarios; combinación distribuible GPL sin resolver | Alto más licencia | No empaquetar ahora; posible investigación posterior con NGX directo |
| DLSS FG / MFG / DLSS 5 | Features y hardware/API específicos; no son un único upscaler | Mismo bloqueo de proveedor propietario | Muy alto | Fuera de la primera integración; no prometer soporte por número comercial |
| XeSS-SR | Color/MV/jitter, depth según modo; Vulkan en SDK Windows; Intel y ruta cross-vendor | Runtime binario Intel Simplified Software License; no MIT; combinación GPL sin resolver | Alto; Linux nativo además bloqueado por runtime publicado | No priorizar frente a FSR 3.1 |

Las filas comerciales se sustentan en las fuentes oficiales de las secciones siguientes.
Ninguna fila temporal tiene calidad, FPS, latencia o VRAM medidos en UFC 3.

## 3. FSR 3.1: integración, coste y frame generation

AMD documenta Vulkan y el contrato color/depth/MV/jitter, con exposición y máscaras
opcionales. No exige GPU AMD ni núcleos ML: puede ejecutarse en AMD, NVIDIA e Intel
que satisfagan capacidades y presupuesto; eso no garantiza rapidez en todas las GPUs.
SR y FG son piezas separables. Fuentes: [FSR 3](https://gpuopen.com/fidelityfx-super-resolution-3/)
y [contrato FSR 3.1](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-interpolation/).

La [licencia del SDK v1.1.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/sdk/LICENSE.txt)
es MIT. La licencia y el [NOTICE local](../../gpu/third_party/fsr-vulkan/NOTICE.md) de
FSR-Vulkan separan código portable, fuentes AMD MIT y assets ausentes. La baseline
portable usada por el host importa SDK 1.1.4 / upscaler 3.1.4; el submódulo también
contiene APIs versionadas para SR 3.1.5 y FG 3.1.6 del SDK 2.3. No asumir que el host
esté usando la API más reciente solo porque el submódulo la contiene.

Trabajo principal (**I**): mapear escena y depth de UFC, resolver MV de objetos,
inyectar jitter coherente, reemplazar detección de pases y componer después el HUD.
Revisar también AA del guest, exposición, mip bias, transiciones de imágenes, lifetime
y reset. La existencia de Vulkan y SPIR-V ya preparados reduce el trabajo del backend,
no el de ingeniería inversa del juego.

**Coste NM**: hay varios pases compute, superficies intermedias/historia, máscaras y
sincronización adicional. Reducir la escena ahorra raster/shading si ese trabajo domina;
no arregla límites de CPU, copia de memoria guest ni simulación. Como escala física,
una sola imagen RGBA16F ocupa aproximadamente 15,8 MiB a 1080p y 63,3 MiB a 4K, sin
padding; no es la memoria total de FSR. Comparar luego GPU ms total, ms de SR, p95/p99,
VRAM y calidad con igual resolución de salida. No hay base para prometer un porcentaje
de FPS ni trasladar cifras de Bloodborne.

**FG: no con el present actual; posible como proyecto separado.**
[Presenter::Present](../../gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp)
adquiere una imagen por presentación, somete trabajo con semáforos y llama una vez a
`swapchain.Present()`. No hay llamada de interpolación ni programación generated→real.
Repetir una imagen no genera un frame intermedio.

El submódulo sí ofrece FI/optical flow analíticos y helpers WSI de dos adquisiciones,
incluido fallback de un frame; su README atribuye a Q2RTX una referencia validada.
No están conectados al presenter de UFC. El backend antiguo deja la callback de
swapchain FG sin implementar por depender de Win32. Haría falta conservar frames y
recursos hasta sus fences, programar interpolado y real, preservar el real si falla
la segunda adquisición, separar HUD, resetear por cortes/resize y medir pacing/latencia.
FG aumenta cuadros mostrados, no frecuencia de simulación ni respuesta al input:
prioridad baja para un juego de lucha, aun si fuera técnicamente integrable (**I**).

## 4. FSR 4: hardware, licencia y alcance de `fsr4-bench`

**«Solo RDNA4» está desactualizado para el producto oficial actual.** AMD anunció
el 24-06-2026 FSR Upscaling 4.1.1 para RX 7000 discretas (RDNA3), además de RX 9000
(RDNA4). No extiende esa garantía a todas las APUs RDNA3, RDNA2, Intel o NVIDIA;
el fallback analítico no es la misma inferencia ML. El proveedor oficial sigue
distinto de los caminos Vulkan comunitarios del repo.
[Anuncio oficial SDK 2.3](https://gpuopen.com/learn/amd-fsr-sdk-2-3-blog/).

La [licencia actual del SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/docs/license.md)
permite por defecto redistribución binaria con avisos y prohíbe ingeniería inversa,
con excepciones explícitas para fuentes. **FSR 4 no hereda automáticamente MIT de
FSR 3**. Extraer modelos/SPIR-V de una DLL propia no demuestra permiso para convertirlos
o redistribuirlos; falta resolver esos derechos y la combinación GPL.

Hay tres piezas distintas (**V**):

1. `fsr-vulkan`: proveedor experimental source-v07 INT8/DOT4, sin pesos/shaders del
   modelo incluidos. Su README aclara que no proporciona el proveedor oficial 4.1.1.
2. [fsr411](../../gpu/shadps4/video_core/renderer_vulkan/fsr411/fsr411.h): replay de los
   dispatches de DLL AMD DX12, con paquetes `fsr4_411` externos. Variante INT8 y FP8;
   FP16 emulado es para pruebas. Los checks de `vk_instance.h` exigen capacidades,
   no simplemente un nombre de fabricante: para 4.1.1 se añade
   `VK_VALVE_shader_mixed_float_dot_product`; FP8 requiere matrices cooperativas y
   `VK_EXT_shader_float8`, entre otras condiciones. No es una garantía para «cualquier GPU».
3. [fsr4-bench](../../gpu/tools/fsr4_bench.cpp): target `EXCLUDE_FROM_ALL`, comparte
   núcleo con el renderer pero trabaja fuera del juego, sin WSI/present real.

Qué demuestra el benchmark por lectura de código (**V**, no ejecutado):

- Por defecto v07, entrada 2260×1272 → 3840×2160, preset 2, 900 frames. Necesita
  `BB_FSR4_DIR` o `fsr4_shaders`; puede tomar pases optimizados de `opt/`.
- Inicializa color gris, MV cero y depth 0,5; `BENCH_NOISE=1` sube datos pseudoaleatorios
  una vez. Esos inputs no tienen movimiento/articulación ni oclusiones reales coherentes.
- Activa `BB_FSR4_PROFILE`; muestra tiempos GPU de pases. `--stats` solicita estadísticas
  del driver cuando están disponibles. `BENCH_DUMP` escribe RGBA16F para comparar salidas.
- `--fsr411` cambia al replay y `BB_FSR411_DIR` / `fsr4_411`; mide el dispatch completo
  con timestamps, imprime promedio cada 300 frames. Un ensayo menor no produce ese
  promedio. Su jitter sintético y reset inicial tampoco validan el jitter de UFC.

[docs/upscaler.md](../upscaler.md) registra históricamente RX 7800 XT/RADV: v07 a
4K Balanced 5,8 ms antes y unos 4,0 ms después de optimizar; replay 4.1.1 1,09 ms para
720→1080 y 4,2 ms a 4K Balanced (**D**, no revalidados aquí). Son contexto del port
de Bloodborne y de su bench, no resultados UFC. El mismo documento corrige una
conclusión CPU/GPU por procesos ajenos que contaminaban la medición; por eso no se
usa su FPS como predicción. No se corrió el default de 900 frames ni se generaron assets.

## 5. DLSS: Streamline, NGX, Linux y GPL

**Linux nativo es posible con NGX directo.** El SDK publica `lib/Linux_x86_64`,
biblioteca NGX y runtime `.so`; su guía exige Vulkan 1.1+, RTX y consultas de soporte
de driver/extensiones. No es necesario convertir el port a Proton. Streamline es
otra capa: su guía publicada exige Windows y describe DLL/interposición, incluso
para Vulkan. No asumir soporte nativo Linux de Streamline porque admite Vulkan.
Fuentes: [guía DLSS SR](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf),
[runtime Linux](https://github.com/NVIDIA/DLSS/tree/main/lib/Linux_x86_64),
[guía Streamline](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md).

DLSS SR requiere color, depth y MV por frame, jitter subpíxel y exposición HDR;
también inicialización, selección de tamaño y reset. No sustituye los MV faltantes
con la capacidad ML de la GPU. La [integración Streamline DLSS](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md)
explica escalas de MV y jitter; cualquier adaptación desde RG16F en píxeles debe
respetar ese contrato. El bloqueo de inputs de UFC es compartido con FSR 3.1.

**Distribución: no hay base para empaquetar ahora una combinación DLSS/NGX + este
renderer GPL.** La [licencia de Streamline](https://github.com/NVIDIA-RTX/Streamline/blob/main/license.txt)
es MIT para su framework, con componentes aparte; no relicencia DLSS. La
[NVIDIA RTX SDKs License](https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt) §1–3
permite redistribución de SDK en aplicaciones bajo condiciones, exige términos
protectores y prohíbe someterlo a obligaciones de licencia abierta; suplemento §1
limita DLSS/NGX a sistemas NVIDIA. Redistribuible bajo términos NVIDIA no significa
compatible con GPL.

En el repo, `CONTRIBUTING.md` y los SPDX fijan GPL-2.0-or-later; no se identificó una
excepción de enlace para estos SDK. La [FAQ GPL de la FSF](https://www.gnu.org/licenses/gpl-faq.en.html#GPLPlugins)
considera relevante la combinación por llamadas/estructuras compartidas, también
con carga dinámica. **I jurídica para este diseño**, no una resolución judicial:
sin excepción válida de los titulares o una arquitectura separada revisada, no aprobar
la distribución del binario combinado. `dlopen`, descarga opcional o «el usuario trae
su DLL» no resuelven por sí solos ese problema. Conservar FSR MIT evita este bloqueo.

Finalmente, «DLSS 4/5» no identifica una sola función: SR/DLAA, FG y MFG requieren
evaluaciones separadas de hardware, API y runtime. NVIDIA describe DLSS 5 como
*3D-Guided Neural Rendering*, distinto del SR temporal; no hay integración Vulkan
Linux de esa función demostrada en este informe. No prometerla mediante el selector
de upscaling. [Descripción oficial de DLSS](https://www.nvidia.com/en-us/geforce/technologies/dlss/).

## 6. XeSS

XeSS-SR es temporal. La guía pide color jittered y MV; depth es necesario con MV a
baja resolución y puede omitirse en el modo de MV de alta resolución. El modo Intel
aprovecha su hardware; existe ruta cross-vendor, también Vulkan con features
específicas, no una garantía para toda GPU. La guía publicada enumera Windows 10/11
x64 y despliegue de `libxess.dll`/`.lib`: Vulkan **no implica runtime Linux nativo**.
No se identificó un `.so` oficial de XeSS-SR en el SDK consultado. Proton sería otro
producto/arquitectura respecto de este port nativo.
[Guía XeSS-SR 2.0](https://github.com/intel/xess/blob/main/doc/xess_sr_developer_guide_english.md).

Su [Intel Simplified Software License, octubre 2022](https://github.com/intel/xess/blob/main/LICENSE.txt)
autoriza redistribuir binarios sin modificar y con avisos; prohíbe modificación e
ingeniería inversa. No es MIT ni un runtime con fuente que pueda distribuirse bajo
GPL. Por la misma cuestión de combinación explicada para DLSS, no aprobar el bundle
integrado sin resolver compatibilidad/excepción. El repositorio público y los samples
no cambian la licencia del runtime. XeSS-FG/XeLL son integraciones distintas; no quedan
habilitadas por integrar SR y añadirían trabajo de presentación/latencia. El
[README XeSS 3](https://github.com/intel/xess/blob/main/README.md) separa esos componentes.

## 7. Próximo trabajo y cómo validar, sin ejecutarlo en este ticket

1. **Primero un ticket de inputs UFC.** Reusar la captura de pases existente para
   localizar velocidad o confirmar su ausencia; mapear view/projection actual/anterior,
   depth y transformaciones de luchadores. Registrar firmas y versión de eboot.
   Cámara quieta con luchadores animados debe producir MV correctos: es un criterio
   de aceptación, no una optimización opcional.
2. Después, prueba acotada de FSR 3.1 antes de UI, con reset y fallback espacial.
   Verificar signos/unidades y reproyección; si no se puede resolver movimiento de
   objetos, documentar esa limitación antes de exponer la opción.
3. Validación manual futura por el usuario, de a una: combate/cámara quieta, paneo,
   golpes y oclusiones; después cortes/replay, HUD y menús. Capturas de inputs/historia
   primero, benchmark sin capturas después. Comparar salida fija con Off/FSR1 y SR,
   medir GPU/CPU ms, colas de frametime, VRAM y latencia. Repetir por AMD/NVIDIA/Intel
   y Linux/Windows antes de anunciar esas combinaciones. Online requiere su propia
   validación; frames interpolados no deben modificar el tick de simulación.
4. Solo si SR queda correcto y útil, reevaluar FSR 4 por versión/proveedor y derechos
   de assets. FG requiere un ticket propio de WSI/pacing. DLSS necesita resolver
   primero distribución GPL; XeSS, además, runtime Linux.

**Verificación de este entregable:** revisar enlaces locales, evidencias, tabla y
`git diff --check`. No se compiló porque solo cambia Markdown; por tanto no hay un
resultado de compilación nuevo que atribuir a este ticket. Si una futura implementación
necesita compilar, usar exclusivamente `ufc3-build` y esperar su turno. No invocar los
comandos directos de CMake/ninja conservados en comentarios o docs heredados.

## 8. Draft reply for issue #46 (not posted)

> FSR 3.1 is our preferred first temporal upscaler: it has an MIT-licensed Vulkan
> path and supports multiple GPU vendors. The inherited renderer already contains
> temporal backends, but its camera/object-motion and pass detection target Bloodborne
> and are disabled for UFC 3. UFC depth is identified; valid motion vectors and jitter
> still need game-specific work. FSR 4.1.1 officially supports RDNA 3/4, but our Vulkan
> paths are experimental/replay implementations with separate asset/licensing concerns.
> DLSS supports native Linux through NGX, while DLSS/XeSS redistribution needs GPL
> compatibility review. Frame generation also requires presenter/pacing changes.
> Recommendation: retain FSR 1.0 now, validate UFC temporal inputs, then integrate
> FSR 3.1 SR before considering other providers or frame generation.
