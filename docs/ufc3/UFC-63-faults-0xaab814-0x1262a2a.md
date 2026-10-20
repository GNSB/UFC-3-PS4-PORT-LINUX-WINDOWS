# UFC-63 — faults v0.1.5 en SimThread y RenderThread

Estado: investigación estática, sin ejecutar el juego. Se identificaron los dos
accesos inválidos; **no hay evidencia suficiente para atribuir el productor del
estado incorrecto ni introducir un fix de runtime**. No se modifican
`runtime_ui.c`, `runtime_appearance.c`, HLE, GPU ni los parches experimentales.

## Resultado

| Log / sitio | Mecanismo comprobado | Atribución y límite |
| --- | --- | --- |
| `20261019_152630.log`, SimThread `+0xaab814` | Consulta de nivel de Ultimate Team; desreferencia de iterador nulo en la tabla de `UltimateTeamLevelConfig` | Compatible con configuración existente pero vacía. No es un objeto de presencia PSN nulo. Falta conocer el origen/contenido de la configuración para atribuirlo al servidor local, carga de contenido o corrupción. |
| `20261019_163015.log`, RenderThread `+0x1262a2a` | Allocator de clases: cabeza de free list **no nula, inválida (`0x64`)** | Misma firma de corrupción de metadatos de UFC-2. Windows activas confirmadas; no se identificó el write que dañó el nodo. No demuestra que el parche 1440p sea la causa. |

Ninguno demuestra el agotamiento por consumidor detenido de
[UFC-57](UFC-57.md) o [UFC-58](UFC-58-create-fighter-crash.md).
Un stall podría modificar el timing o agravar un defecto anterior, pero no convierte
una tabla vacía o un enlace `0x64` en evidencia de esos pools agotados.

## Fuentes y reproducción del análisis

Se leyeron los tres logs completos en
`/home/myzk/ufcCOntext/github-issues/user-eme/`, las notas anteriores y
`/home/myzk/Projects/ufc3-native/out/boot-linked.bin` (sólo lectura).
SHA256 de este último:
`cea8cdf4e01b63ea21e80758c73d0ac1ced53d5e35e5f96dd552ccc204a0f230`.
Es BBPROBE5, no una imagen plana desde el byte cero: longitud de imagen
48.465.196 bytes, inicio del payload en el archivo 6.759.544. No se tiene el
binario exacto del usuario ni un dump de memoria. Instrucciones, retornos y
registros son coherentes con esta imagen 1.14; no es una comparación de hashes
contra el ejecutable de su sesión.

Extracción usada (sólo genera un archivo temporal, no ejecuta código guest):

```sh
python3 - <<'PY'
import pathlib, struct
b = pathlib.Path('/home/myzk/Projects/ufc3-native/out/boot-linked.bin').read_bytes()
assert b[:8] == b'BBPROBE5'
n = struct.unpack_from('<Q', b, 8)[0]
pathlib.Path('/tmp/ufc63-image.bin').write_bytes(b[-n:])
PY
objdump -D -b binary -m i386:x86-64 -M intel \
  --start-address=0xaab6b0 --stop-address=0xaab840 /tmp/ufc63-image.bin
objdump -D -b binary -m i386:x86-64 -M intel \
  --start-address=0x31e0f0 --stop-address=0x31e220 /tmp/ufc63-image.bin
objdump -D -b binary -m i386:x86-64 -M intel \
  --start-address=0x12629c0 --stop-address=0x1262ae0 /tmp/ufc63-image.bin
```

Las direcciones de abajo son offsets guest. El runtime aplica las relocaciones
al cargar; para estas instrucciones basta el payload extraído.

## 1. SimThread: tabla de niveles de Ultimate Team

El primer retorno del log, `0x31e1f7`, sigue al `call 0xaab6b0` de
`0x31e1f2`. Su llamador `0x31e0f0` compara el nombre de la operación con:

- `0x1e6c749`: `ufc:01.00.00:ultimate-team:level:get-ultimate-team-level-data`.
- `0x1e6c787`: la variante `get-ultimate-team-level-data-last-game`.
- `0x1e6c7cf`: la variante `calculate-ultimate-team-level-data`.

La rama que llama en `0x31e1f2` corresponde a la primera. Lee `PLAYER_ID`
(string en `0x1e6c813`), lo convierte a entero y pasa un resultado de stack de
16 bytes a `0xaab6b0`. El frame siguiente `0x5ed768` sigue a un despacho virtual
`call [rax+0x10]` en `0x5ed765`, coherente con el handler anterior. Los frames
`0x6059b1`, `0x605acd`, `0x13ffd5c`, `0x1400afa`, etc. no cambian la
identificación de la consulta en el punto del fallo.

`0xaab6b0` conserva el propietario en r14 y la salida en r15. Consulta un
objeto mediante `0xf6b9e0` y lee cuatro propiedades de categoría 7, IDs
`0x1e..0x21`, mediante `0xf66e30`. Si falta ese objeto o su campo `+0x188`,
retorna false. Después consulta la configuración `owner+0x180`:

```asm
aab7b8  mov rsi, [r14+0x180]  ; configuración
aab7bf  test rsi, rsi
aab7c2  je 0xaab81a           ; configuración ausente: false (protegido)
aab7c4  mov r8, [rsi+0x80]    ; begin de vector de punteros
aab7cb  mov rdi, [rsi+0x88]   ; end
aab7d2  sub rdi, r8
aab7d5  mov rsi, r8
aab7d8  sar rdi, 3           ; cantidad
; búsqueda binaria: compara [elemento+0x10] con ecx
; sale con rsi en el punto de inserción y rbx <= 0
aab809  lea rcx, [rsi-8]
aab80d  cmp rsi, r8
aab810  cmove rcx, rsi
aab814  cmp qword ptr [rcx], 0 ; FAULT: rcx = 0
aab818  jne 0xaab81c
aab81a  xor eax, eax         ; false
```

El registro nulo que se desreferencia es **rcx**, un iterador, no el
propietario ni la configuración. `rsi=0`, `rbx=0`, `rcx=0` son exactamente
compatibles con `begin=end=NULL`: no entra al cuerpo de búsqueda y el
`cmove` elige begin. Bajo invariantes normales del vector, éste es el caso
vacío. Los logs antiguos omiten r8..r15: no se puede certificar el contenido
de `config+0x80/+0x88` ni excluir corrupción/concurrencia. Tampoco son «todos
los registros cero»: rbp/rsp tienen valores, y los ocho registros altos no
se imprimieron.

Hay identificación adicional de la configuración: `0xaabbf0` llama al método
virtual `+0x60` de un proveedor con el string
`UltimateTeamLevelConfig::clone` (`0x1edf930`), y guarda el resultado en
`owner+0x180` en `0xaabc4b`. Este objeto **sí existe** en la ruta del fault:
si fuera NULL, `0xaab7c2` evitaría la desreferencia. Falta resolver el proveedor
virtual y el asset/respuesta concreta que pobló su vector de niveles.

### Net/PSN y servidor LAN

Se auditó `src/runtime_net.c`: `np_webapi_context`, `np_webapi_request`,
`np_webapi_send2`, `np_webapi_read`, `np_webapi_delete_request`, `profile_json`
y los resultados de availability/Plus/auth. El log muestra dos requests de
presencia `sdk:userProfile /v1/users/.../presence/gameStatus`, método 2;
CreateRequest almacena un ID y SendRequest lo acepta. El HLE actual responde
`{}` para esa ruta y HTTP 200; para `/profiles` construye `profiles`.
No hay lectura de respuesta WebAPI registrada en ese tramo. Es una emulación
limitada; por sí sola una respuesta `{}` no prueba un defecto de la consulta
de niveles, ni justifica inventarle un objeto de perfil.

La ruta desensamblada no obtiene el iterador de un retorno HLE de presencia,
WebAPI o perfil. La proximidad temporal de login/presencia no establece esa
causa. Tampoco excluye que un servicio anterior haya dejado datos incompletos.
El servidor local en `tools/ufc3_server.py` devuelve resultados vacíos para
`/xmshd/search` y mensajes vacíos para `/aruba/...`; los tamaños 44 y 16 bytes
del log concuerdan con esas respuestas. **No se estableció una cadena de datos
entre ellas y `UltimateTeamLevelConfig`**. El log del servidor de esa sesión
(`server-20261019_152633.log`, mencionado al final) no está entre los entregados.

Conclusión: defecto de manejo de tabla vacía en código guest, alcanzado en el
port; posible disparador de contenido/servidor incompleto, todavía sin atribuir.
No es evidencia de un objeto PSN nulo ni de agotamiento del allocator. Saltar
la instrucción o fabricar niveles ocultaría el estado incorrecto sin restaurar
la configuración necesaria; no se entrega ese supuesto fix.

## 2. RenderThread: free list corrupta, no acceso a NULL+0x64

`0x12629c0` es el allocator de clases descrito en
[UFC-2](UFC-2-write-window-investigation.md). El tramo se verificó otra vez
contra el payload de boot-linked:

```asm
1262a17  mov r14, [r15+rsi*8+0x18] ; slab/bucket
1262a1c  mov r13, [r14+0x18]       ; cabeza libre
1262a20  test r13, r13
1262a23  je 0x1262a54             ; lista vacía: camino normal alternativo
1262a25  inc word ptr [r14+0x20]
1262a2a  mov rax, [r13]           ; FAULT: r13 = 0x64
1262a2e  mov [r14+0x18], rax      ; pop
```

La dirección leída `0x64` identifica r13 aunque la versión vieja no lo
imprima: es una lectura de `[r13]`, no `[NULL+0x64]`. Una lista agotada con
head=0 habría tomado el salto de `0x1262a23`. Esto es **corrupción de un
puntero/enlace**, no demostración de OOM o pool agotado.

`rsi=1`, `rbx=0x3f` concuerdan con la clase de 32 bytes: el tamaño alineado
previo es 32, se le suma 31 y se divide por 32 (`>>5`). No es una reserva
multimegabyte de framebuffer que haya devuelto NULL.

Los cuatro primeros retornos son los mismos del caso UFC-2:
`0x7b495` (dispatcher virtual de allocator), `0x11bd0b` (selección de
allocator/categoría), `0xf3f7e` (constructor que solicita almacenamiento),
`0x116610` (llamada a `0xf3dc0`). En `0xf3dc0`, el tamaño de almacenamiento
se calcula a partir de stride/cantidad del descriptor; `0xf3f3f..0xf3f59`
redondea el stride a 16 y lo multiplica por la cuenta, y `0xf3f82` guarda el
resultado en `obj+0xa0`. El fault ocurre dentro de esa asignación, antes de
que el consumidor lea el buffer. El hilo RenderThread no convierte el fallo
en un error de dimensiones de imagen.

### Relación con el parche 1440p y las windows

Los XML UFC-45/46 sólo escriben tres inmediatos del selector `0x533180`:
`0x533229`, `0x53322f` (ancho High) y `0x533248` (alto High).
Cambian 3200x1800 a 3840x2160 o 2560x1440. Conservan los tamaños
1920x1080 de UI/output y 1600x900 del otro camino. Sí cambian las dimensiones
que el guest entrega en el descriptor High (`+0x5c/+0x60`), y con ello pueden
cambiar tamaños de recursos y presión/timing. No parchean el allocator ni
los offsets de estos faults. El log confirma tres writes aplicados; no trae
una captura del descriptor/recurso que demuestre desajuste entre reserva y
lectura. Además, instalar el parche no prueba qué opción VIDEO estaba activa.
La escena 1440p tiene menos píxeles que el original High 3200x1800; eso
no descarta un error de dimensionado, pero tampoco apoya OOM por crecimiento.

La evidencia nueva más útil está en el propio log de Career, antes de EntityLoader:

```text
Write windows: site +0x1262c9f (from +0xf409b) changed 72 GPU-owned bytes of page 0x10007e6000 in a window (268 left GPU-owned beside them); its writes to GPU pages read back from now on (UFC-48)
```

También hay cientos de windows/s antes de ese punto. Se observa interacción
CPU/GPU sobre memoria de la familia del allocator, y UFC-48 barra el sitio
desde entonces. **No demuestra que esa página contenga el head o el enlace
corrupto de este fault**: faltan r14/r15 y el historial del nodo. El evento
no dice que esos 72 bytes se perdieran; el código actual de UFC-48 toma
ownership CPU de los bytes cambiados y conserva los ambiguos por separado.
No trasladar sin más al runtime actual la descripción histórica de pérdida
de writes de UFC-2. Un enlace ya dañado, bytes ambiguos, readback/label tardío,
use-after-free u otro escritor siguen requiriendo evidencia por dirección y vida
de asignación. La posterior caída de actividad de windows no repara un enlace
que pudo corromperse antes.

Las escrituras de INDEX de CAREERSAVE1 y los tres EntityLoader son contexto
de transición, no evidencia de que el HLE de saves devolviera un objeto nulo.
No se modifica el guardado ni se aconseja borrar el save.

## Comparación limpia y límites

`test-755bf11-tests1-3.log` termina con cierre de ventana y exit 0. Usa
`ufc3-v0.1.6-9-g755bf11`, otra máquina (RX 7800 XT 16 GB, otro driver),
1920x1080, Fifo, offline, cero patches, y habilita las contrapresiones UFC-57/58.
También abre windows y pasa varias creaciones de EntityLoader/montajes Career.
Por tanto esos eventos y windows no bastan por sí solos para causar el fault.
No es un A/B controlado: cambian hardware, build, sincronización, resolución,
servidor y posiblemente save/acciones. No demuestra que los fixes 57/58
resuelvan estos dos fallos ni que quitar los parches los elimine.

## Una prueba manual para el tester

Una sesión Windows con el build actual, en la máquina RX 6700 XT y con una
copia del mismo save (conservar el original): offline, resolución normal sin
los experimentos UFC-45/46, y `BB_WRITE_STEP=0 BB_WRITE_WINDOW=0` establecidos
en el entorno antes de abrir el launcher. Repetir **una vez** la misma transición
Career post-pelea → guardado → carga que produjo el segundo fault; detenerse
al primer crash o al completar esa transición. Guardar el log completo y anotar
las acciones. No activar `BB_WRITE_LOG` (ver limitación del dump en UFC-2).

Verificar en el log cero writes de patches y ausencia de windows abiertas.
Si vuelve `+0x1262a2a`, el build actual imprime r8..r15: r13 identifica el
head inválido y r14 permite localizar el bucket. Un fallo en esta sesión
demostraría que ni el experimento ni windows activas en esa sesión son
necesarios; no identificaría automáticamente el escritor. Un pase sólo
establece una referencia de compatibilidad, sin atribuir qué cambio evitó
el fallo. No se solicita una batería de corridas ni se promete un fix.

## Qué falta para un arreglo y validación

Para el primer fallo: dump en el punto fatal con r14 (owner),
`[owner+0x180]` y su vector `+0x80/+0x88`, y seguimiento de la adquisición
virtual de `0xaabc0e`, incluyendo origen del `UltimateTeamLevelConfig`.
El log del servidor local podría orientar ese seguimiento. Hace falta conocer
el esquema y la tabla real antes de completar una respuesta del servidor.

Para el segundo: bucket/nodo concretos y el último escritor legítimo más el
write/readback/label que rompe el enlace; correlación por rango y vida de
asignación, no sólo por página. Es una continuación de UFC-2/UFC-48, no un
motivo para ampliar la contrapresión de UFC-57/58.

Entrega sólo documental: no hay fix seguro identificado con estos insumos.
No se compila ni se ejecutan `ufc3-build --test` o el cross-build Windows porque
no hay cambios compilables; no se afirma que pasen. Si se implementa un fix,
quedan exigidos ambos checks del ticket (ignorando el fallo conocido de
`file-mods-test`). `git diff --check` valida el documento. No se ejecutó el
juego, no se cambiaron rama, índice o historia git y no se generaron assets guest
dentro del repositorio.
