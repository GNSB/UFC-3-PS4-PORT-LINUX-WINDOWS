# UFC-75: revisión del PR #48 (CPUs sin BMI1/LZCNT/MOVBE: Ivy Bridge y Sandy Bridge)

PR: GNSB/UFC-3-PS4-PORT-LINUX-WINDOWS#48, de @deanzx06, rama `feat/ufc3-legacy-cpu` contra `main`
(+630/-3, 4 commits del 20/10/2026). Revisado sobre `ufc3-online` (d6fae00). Las referencias
`archivo:línea` son del PR (`git show pr-48:<archivo>`).

## Veredicto: tomarlo con cambios. No mergearlo tal como está

La idea sirve y la mayoría de los trampolines están bien. Los generé con los archivos del juego y
los emulé uno por uno contra la instrucción original (ver "Qué verifiqué"). Pero tal como está, el
PR:

- **No funciona en un Ivy Bridge.** No incluye el plan de traducción. Sin el plan, solo se
  reemplaza `strlen` y el juego muere en el primer `andn`, salvo que el usuario instale capstone y
  keystone y genere el plan a mano.
- **Lo enciende para todos en Linux.** Agrega un `bbport.ini` versionado con `legacy_cpu=1`.
- **Obliga a desactivar nuestro chequeo de CPU (UFC-59) entero** con `BB_SKIP_CPU_CHECK=1`.
- **Al adaptarlo del fork de T0ug perdió cosas que el original sí tenía:** las validaciones por
  hash, los tests, y la corrección de `blsr` y `blsmsk` con entrada cero.

Recomendación: pedirle al autor los cambios de la lista de abajo, o portarlo nosotros a
`ufc3-online` con esos arreglos. En ambos casos, como PR contra `ufc3-online`.

## Qué verifiqué (solo análisis estático, sin ejecutar el juego)

- **Generación del plan.** Corrí `tools/build_cpu_compat.py` sobre `orig/CUSA06534`, con capstone
  5.0.7 y keystone en un venv en /tmp. Salen 6.073 sitios y 0 sin resolver, igual que dice el PR:
  2927 andn, 2537 bextr, 414 movbe, 87 lzcnt, 56 blsi, 50 tzcnt, 1 blsr y 1 blsmsk. Hay 57 sitios
  con "spanning" (53 hacia adelante y 4 hacia atrás) y un parche en el lugar (0x17f4fdd).
- **Link real.** Apliqué el PR sobre `ufc3-online` en un árbol temporal (`link_modules.py` es
  idéntico en `main` y en `ufc3-online`, así que aplica limpio). Después corrí `prepare.py`,
  `link_libc.py` y `link_modules.py`:
  - con `BB_LEGACY_CPU=0`, `boot-linked.bin` sale **idéntico byte a byte** al de antes del PR;
  - con `BB_LEGACY_CPU=1` y el plan, se aplican los 6073 sitios, con 453.658 bytes de trampolines
    en un segmento R+X nuevo.
- **Barrido de la imagen parcheada.** En el eboot, en Fios2 y en los trampolines quedan 0
  instrucciones BMI/LZCNT/MOVBE. **En libc quedan 83** (ver problema 4).
- **Emulación diferencial con Unicorn 2.1.4** (modelo Haswell). Por cada sitio, ejecuté los bytes
  originales y el cuerpo del trampolín con 8 estados de registros, incluidos ceros y valores
  aleatorios, y memoria determinista bajo demanda. Comparé los GPR, RSP, la memoria escrita (fuera
  de la zona de push, más abajo de la red zone) y los flags definidos por cada instrucción.
  - `andn`, `tzcnt`, `lzcnt`, `movbe` (incluido el parche en el lugar) y `blsi`: sin diferencias
    reales.
  - Red zone y RSP intactos.
  - Las diferencias de `blsi` y `bextr` que mostró Unicorn son bugs de su QEMU viejo: el CF de
    BLSI, y `bextr` de 32 bits con los bits altos del registro sucios. `bextr` lo verifiqué aparte
    contra una referencia en Python: 25.370 casos y 0 diferencias.
  - `blsr` y `blsmsk` tienen bugs reales (problema 5).
- **Choques con nuestro código.**
  - Ningún sitio parcheado se pisa con los hooks de direcciones fijas (`gpu/shim/bbport_guest_hooks.cpp`,
    `bbport_gnm_hooks.cpp`, `src/*.c`).
  - Con el FP determinista del online (`src/runtime_fp.c`), solo 1 de los 2300 sitios
    rcp/rsqrt cae cerca de un parche. En el peor caso, ese sitio queda sin parche FP y figura en el
    log. Ninguna instrucción movida por spanning es rcp/rsqrt.
  - Ninguna de las 57 instrucciones interiores movidas por spanning es destino de un puntero
    absoluto (69.486 relocations) ni de una referencia RIP-relative (73.483).

Los scripts están en `docs/ufc3/UFC-75/` (`cov.py`, `emu.py`, `emu_bextr.py`). Se corren desde
la raíz de un árbol con el PR aplicado, con `PLAN=<legacy-cpu-plan.json>` y `GAME=<orig/CUSA06534>`,
y necesitan `capstone`, `keystone-engine` y `unicorn`.

## Problemas, de mayor a menor

1. **El plan no viene en el PR, así que en un Ivy Bridge no hace nada útil.**
   `scripts/cpu_compat.py:158-181` busca `tools/legacy-cpu-plan.json`, `.json.xz` o
   `legacy_plan_data.py`, y si no los encuentra imprime un aviso y sigue. Ninguno de los tres está
   en el PR. El usuario de Windows tiene el Python embebido, sin pip, y no puede generarlo. Además,
   no conviene versionar el plan tal como sale: el campo `bytes` de cada sitio son ~35 KB de código
   del eboot (el JSON pesa 1,5 MB y el xz 50 KB). Opciones:
   - versionar el plan sin `bytes`, atado al SHA-256 de cada módulo (el plan ya trae
     `plan['modules']`), y comparar contra el archivo del usuario;
   - o generar las traducciones al linkear, sin dependencias externas.
2. **Fuerza la traducción en todos los Linux que corren desde el repo.** El PR agrega
   `bbport.ini` con `legacy_cpu=1`, `show_fps=1` y `async_pipelines=1`.
   - `bbport.ini` está en `.gitignore:5` y `.gitignore:34` porque es el archivo de ajustes del
     usuario. `run.sh:16-21` usa `BB_CONFIG=$data/bbport.ini` con `data=.`, o sea justamente ese
     archivo.
   - Efectos: en Linux se traduce siempre, incluso en CPUs modernas. A todos se les prende el
     contador de FPS. Y un `git pull` falla para quien ya tiene su `bbport.ini` local ("untracked
     working tree file would be overwritten").
   - `async_pipelines` no existe en nuestro runtime, así que no tiene efecto. `legacy_cpu` solo lo
     lee el script de Python.
   - En Windows (`%LOCALAPPDATA%\SacredFire\bbport.ini`) y en el AppImage no aplica, salvo que se
     empaquete. Hay que sacar el archivo.
3. **No se coordina con UFC-59.**
   - `src/probe.c:47-76` corta con código 24 si falta BMI1, MOVBE o LZCNT. El PR pide
     `BB_SKIP_CPU_CHECK=1`, que también se saltea los chequeos de AVX, POPCNT y SSE4.2: un Core 2 o
     un Phenom pasaría y moriría con SIGILL.
   - Además, la detección se duplica: una en Python al linkear (`cpu_compat.py:56-102`, con
     shellcode CPUID en memoria RWX en Windows) y otra en C.
   - Propuesta: `link_modules` marca la imagen traducida (un bit en `flags` del header BBPROBE5 o
     una marca en `out/`) y `check_cpu` deja de exigir esas tres extensiones solo cuando la marca
     está y todos los sitios se aplicaron.
4. **El escaneo de las PRX se corta y deja 83 instrucciones sin traducir en libc.**
   - `tools/build_cpu_compat.py:171` hace un barrido lineal con `md.disasm`, que se detiene en el
     primer byte que no decodifica. En libc se corta en 0x912bb de 0xe8a5c, y en Fios2 en 0x46850
     de 0x5a700 (en Fios2 no queda nada pendiente).
   - Las que quedan en libc: 35 lzcnt, 35 bextr, 10 tzcnt y 3 andn, en los builtins de
     compiler-rt (`__udivmodti4`, `__divdf3`, `__floatuntidf`, `__mulsf3`, ...).
   - Hoy es latente: ni el eboot ni Fios2 importan esas funciones, y en libc solo se llaman entre
     ellas. Por eso la prueba de 30 minutos pasó.
   - Hay que usar rangos de FDE y símbolos, como ya hace el PR para el eboot.
5. **`blsr` y `blsmsk` con entrada cero dan mal** (`tools/build_cpu_compat.py:110-118`).
   - `blsr` deja CF=0, y debería ser CF=1.
   - `blsmsk` devuelve 0 con ZF=1 y CF=0, y debería devolver todos unos con ZF=0 y CF=1.
   - Es una regresión respecto del original: `T0ug/bloodborne_pc_Ivy-Bridge`
     `tools/build_cpu_compat.py:145-157` lo hace bien.
   - En este eboot es casi inofensivo: 0x126ed36 no lee los flags después, y 0x13380c0 solo cambia
     con un puntero NULL. Igual hay que arreglarlo.
6. **Las instrucciones cortas en PRX rompen la imagen.**
   - `cpu_compat.py:229` arma `go + b'\x90' * (len(original) - 5)`. Si el sitio mide menos de 5
     bytes, eso escribe 5 bytes sobre menos espacio, y la asignación al slice del bytearray inserta
     bytes y corre todo lo que viene después.
   - El generador de PRX (`build_cpu_compat.py:168-198`) no trata las instrucciones cortas.
   - Hoy no pasa solo porque el problema 4 corta el barrido antes de los `lzcnt` y `tzcnt` de 4
     bytes de libc. Arreglar el 4 sin arreglar este rompe la imagen.
   - `patch_image` debería rechazar `len(original) < 5` y verificar que la longitud del parche
     coincide. Lo mismo vale para `patch_libc` (`cpu_compat.py:139`) si `strlen` mide menos de 14
     bytes.
7. **Ignora los errores y aplica parcialmente sin avisar.**
   - `cpu_compat.py:202-203` hace `continue` cuando los bytes no coinciden: no cuenta ni avisa
     cuántos sitios se saltearon.
   - No compara `plan['modules']` con los SHA-256 reales.
   - `patch_libc` no valida el hash de libc ni la firma de `strlen`.
   - El original de T0ug (`scripts/cpu_compat.py:232-250`, `278-300`) sí hacía esas tres cosas.
   - Con un plan de otra versión, o con otro parche que toque esos bytes, el resultado es una
     traducción a medias y un SIGILL más tarde. Si falta algún sitio, debería fallar o al menos
     registrarlo en `link.json` y en el log.
8. **Faltan rechazos defensivos en el generador** (hoy hay 0 casos de cada uno, pero son trampas):
   - El operando principal RIP-relative nunca se revisa: `rip_fixups` siempre queda `[]`
     (`build_cpu_compat.py:189,242`), y keystone lo ensamblaría relativo al trampolín.
   - El spanning hacia adelante (`:276-280`) no rechaza un vecino que sea él mismo BMI/MOVBE (lo
     copiaría sin traducir) ni `loop`, `loope` o `xbegin`.
   - Los destinos de salto (`:209-218`) solo incluyen saltos directos: no tablas de salto, punteros
     ni landing pads. Lo verifiqué contra relocations y referencias RIP-relative y da 0, pero el
     generador debería hacerlo solo, como `runtime_fp.c`.
9. **La dirección 0x17f4fdd está fija en el código** (`build_cpu_compat.py:256`). El truco
   `mov r32, m32; bswap r32` mide lo mismo que `movbe r32, m32` con registros bajos y no toca los
   flags. Se puede generalizar a todas las cargas `movbe` de 32 bits y ahorrar trampolines.
10. **Detalles menores:**
    - `atomic_output` (`cpu_compat.py:104-118`) crea `boot-linked.bin` y `link.json` con modo 0600
      (antes 0644).
    - `MAX_IMAGE_SIZE` (`:22`) y `re` (`:14`) no se usan.
    - La rama `stub_address` (`:212-219`) es código muerto.
    - `patched_count` (`:244`) cuenta todas las operaciones.
    - `enabled()` toma como encendido si no puede detectar (`:102`) y solo acepta `legacy_cpu=1`
      sin espacios (`:97`).
    - No hay tests: el original traía `tests/test_cpu_compat.py`, de 813 líneas. Tampoco hay
      documentación.
11. **El PR apunta a `main`.** Nuestro trabajo está en `ufc3-online`. Aplica limpio en las dos
    porque `link_modules.py` es igual en ambas, pero debería ir contra `ufc3-online`.

## ¿En CPUs modernas queda totalmente apagado?

- **Si no hay `legacy_cpu=1`, sí.** `enabled()` detecta BMI1, LZCNT y MOVBE, no hace nada, y
  `boot-linked.bin` sale idéntico byte a byte (verificado). El único cambio es el modo 0600 de los
  archivos generados. El coste es el import y leer `/proc/cpuinfo` en cada link.
- **Con el `bbport.ini` del PR, en Linux desde el repo, no:** se traduce siempre (problema 2).
- **Si la detección falla** (`/proc/cpuinfo` ilegible, o la excepción del CPUID en Windows), se
  enciende sola (`cpu_compat.py:102`). Sería más seguro que quede apagado y avise.

## Licencia y crédito

- `T0ug/bloodborne_pc_Ivy-Bridge` y `Supermedo/bloodborne_pc` son GPL-2.0, igual que este repo,
  así que son compatibles.
- 84 de las 247 líneas de `cpu_compat.py` son idénticas al `cpu_compat.py` de T0ug, y el
  generador sigue la misma estructura. Los archivos no traen ninguna cabecera de copyright ni de
  origen. Hay que pedir una cabecera que nombre a T0ug y Supermedo (deadinside28) y el commit o la
  URL de origen.
- "Antigravity (Google DeepMind)" es el agente de código de Google, no un autor: el crédito debería
  ser para @deanzx06, con una nota de que se hizo con asistencia de IA.
- `deanzx06/UFC3-4-IvyBridge-Patch` no declara licencia, así que no hay que copiar nada de ahí sin
  que la agregue.
- Al agradecer, recordar que el método viene de T0ug.

## Cómo lo traería (si lo portamos nosotros)

1. Sin `bbport.ini`. Opt-in o opt-out solo con `BB_LEGACY_CPU`, o una clave nueva escrita por el
   launcher.
2. El plan en el repo sin bytes del juego, atado a los SHA-256, y abortar o avisar si algún sitio
   no aplica. Alternativa: generarlo al linkear con un decodificador mínimo propio, porque las
   codificaciones son pocas (VEX 0F38 F2/F3/F7, F3 0F BC/BD, 0F 38 F0/F1).
3. Corregir `blsr` y `blsmsk`, escanear las PRX por FDE, tratar o rechazar las instrucciones
   cortas, y rechazar RIP-relative y los vecinos BMI.
4. Hacer el traspaso con `probe.c`: marca en la imagen, y `check_cpu` relaja solo BMI1, MOVBE y
   LZCNT.
5. Tests como `emu.py`: emulación diferencial de cada trampolín más una referencia propia de
   `bextr`, `blsr` y `blsmsk`.
6. Cabeceras de licencia y crédito.

## Borrador de comentario de review (en inglés, para el PR)

> Thanks a lot for this! Ivy/Sandy Bridge support is something people have asked for (#26), and
> the trampoline approach is sound. I generated the plan from CUSA06534 with your tool (same
> 6,073 sites) and ran every trampoline body side by side with the original instruction under
> Unicorn (random and zero inputs, result + defined flags + memory + red zone). `andn`, `bextr`,
> `tzcnt`, `lzcnt`, `movbe` and `blsi` all match. With the feature off, `boot-linked.bin` is
> byte-identical to before. Nice work.
>
> A few things need to change before this can go in:
>
> 1. **The plan isn't in the PR.** Without `tools/legacy-cpu-plan.json` / `legacy_plan_data.py`,
>    `patch_image` prints a warning and only `strlen` gets replaced, so an Ivy Bridge user still
>    hits SIGILL on the first `andn`. Windows users (embedded Python, no pip) can't generate it.
>    Please don't commit it as generated, though: the `bytes` field is ~35 KB of the game's code.
>    Could you drop `bytes`, bind the plan to the module SHA-256s (`plan['modules']` is already
>    there) and check those at link time?
> 2. **Please remove `bbport.ini`.** It's the user's own settings file, it's in `.gitignore`, and
>    on Linux `run.sh` uses `./bbport.ini`. Committing it forces `legacy_cpu=1` (and `show_fps=1`)
>    on every CPU and breaks `git pull` for anyone who already has one. `async_pipelines` isn't a
>    setting in this tree.
> 3. **CPU check.** Asking users to set `BB_SKIP_CPU_CHECK=1` also skips the AVX/POPCNT/SSE4.2
>    checks. We'd rather have `link_modules.py` mark the image as translated (e.g. a flag bit in
>    the BBPROBE5 header) and have `check_cpu()` stop requiring BMI1/MOVBE/LZCNT only when every
>    site was applied. We can do the `probe.c` side.
> 4. **`blsr`/`blsmsk` with a zero source** (`tools/build_cpu_compat.py:110-118`): `blsr` must set
>    CF=1, and `blsmsk` must return all ones with CF=1, ZF=0. T0ug's original gets this right
>    (`mov first, -1; test; stc` and the `stc` in the `blsr` zero path); it got lost in the port.
> 5. **PRX scan stops early** (`build_cpu_compat.py:171`): `capstone.disasm` stops at the first
>    undecodable byte. In libc.prx it stops at 0x912bb of 0xe8a5c, leaving 83 BMI/LZCNT
>    instructions in the compiler-rt builtins (`__udivmodti4`, `__divdf3`, ...). Nothing in the
>    eboot imports them today, but please scan PRXs by FDE/symbol ranges like you do for the eboot.
> 6. **Sites shorter than 5 bytes in PRXs**: `go + b'\x90' * (len(original) - 5)`
>    (`cpu_compat.py:229`) inserts bytes into the bytearray when the site is under 5 bytes, which
>    shifts the rest of the image. Fixing (5) exposes this, because libc has 4-byte `lzcnt`/`tzcnt`.
>    Please handle or reject short sites, and assert that replacement lengths match.
> 7. **Fail loudly.** A byte mismatch currently skips the site silently (`cpu_compat.py:202`), and
>    neither the module hashes nor the libc/strlen signature are checked (T0ug's version did
>    this). A partial translation means a SIGILL later, so please report skipped sites in
>    `link.json` and refuse to mark the image as translated.
> 8. **Generator guards** (nothing hits these today, but they're traps): reject RIP-relative
>    operands in the translated instruction (`rip_fixups` is always empty); in forward spanning,
>    reject neighbours that are themselves BMI/MOVBE or `loop*`/`xbegin`; and consider absolute
>    pointers and RIP-relative references as branch targets, not just direct jumps.
> 9. **Nice to have:** the in-place `mov r32,[m]; bswap r32` trick for 0x17f4fdd works for every
>    32-bit `movbe` load into a legacy register (same length, no flags), so it doesn't need a
>    hard-coded address.
> 10. **Licensing/credit:** the upstream code is GPL-2.0, like this repo, so that's fine, but
>     please add a header to `cpu_compat.py` / `build_cpu_compat.py` naming T0ug's
>     bloodborne_pc_Ivy-Bridge and Supermedo's bloodborne_pc (with a commit or URL). Please also
>     credit yourself as the author rather than "Antigravity (Google DeepMind)"; a note that it
>     was AI-assisted is enough. Tests along the lines of T0ug's `tests/test_cpu_compat.py` would
>     help too.
> 11. Could you retarget the PR to `ufc3-online`? That's where development happens now. The files
>     apply cleanly there.
>
> Happy to help with any of this. Once (1)–(7) are in, we can ask Ivy Bridge users to test a build.
