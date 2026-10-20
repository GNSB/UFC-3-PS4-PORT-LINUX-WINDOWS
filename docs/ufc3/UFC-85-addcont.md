# UFC-85 · Issue #50 · Bruce Lee y add-on content

Investigación estática del 2026-10-20. Sin ejecutar el juego, modificar el dump,
compilar ni comentar en GitHub. Este ticket entrega investigación y plan; **no
implementa soporte DLC ni cambia desbloqueos**.

## Resultado

Bruce Lee no es exclusivamente contenido de preventa: EA documenta que se obtiene
completando Career en cualquier dificultad y entrando al UFC Hall of Fame. Hay
también DLC de acceso al personaje y una recompensa por haber jugado entregas
anteriores. Podemos preservar la vía ganada jugando y proponer soporte para
add-ons presentes en el dump propio; no podemos conceder compras ausentes.

El montaje de DLC con datos es viable, pero no trivial: el runtime devuelve una
lista vacía y nunca monta add-ons. El binario 1.14 sí enumera, monta, busca un
manifiesto y desmonta. **Montar carpetas no demuestra que Bruce Lee vaya a
desbloquearse**: falta un dump de su add-on/licencia y resolver si ese producto
usa datos adicionales, solo licencia o una comprobación de cuenta.

## 1. Cómo se obtiene Bruce Lee

La respuesta de **EA_Blueberry, Community Admin**, en los
[foros oficiales de EA](https://forums.ea.com/discussions/ufc-franchise-discussion-en/re-bruce-lee-still-locked-after-achieving-goat/11389059)
describe Career en cualquier dificultad más Hall of Fame; incluye las divisiones
Bantamweight, Featherweight, Lightweight y Welterweight. También documenta acceso
por UFC/UFC 2 y una vía histórica mediante EA Access. Es una recompensa por cuenta
EA; no corresponde simular haber jugado o comprado entregas anteriores.

En esa misma conversación EA pregunta por el logro GOAT y la inducción, propone
sincronizar progreso con servidores y finalmente remite a soporte ante un fallo.
Por eso no prometemos que completar Career baste offline en el port. La regla
documentada para UFC 3 está clara; su funcionamiento concreto en **PS4 1.14**
necesita validación del usuario. La fuente no especifica número de parche.

La [publicación oficial de PlayStation del 29-01-2018](https://blog.playstation.com/archive/2018/01/29/4-new-playstation-store-releases-you-need-to-check-out-this-week/)
lista los cuatro DLC por categoría de peso y el bundle. Sus enlaces de Store
contienen estos IDs de la región del dump:

| Producto | CONTENT_ID publicado |
| --- | --- |
| Bantamweight | `EP0006-CUSA06534_00-UFC3FIGHTER00001` |
| Featherweight | `EP0006-CUSA06534_00-UFC3FIGHTER00002` |
| Lightweight | `EP0006-CUSA06534_00-UFC3FIGHTER00003` |
| Welterweight | `EP0006-CUSA06534_00-UFC3FIGHTER00004` |
| Bundle | `EP0006-CUSA06534_00-UFC3FIGHTERBUND1` |

Son referencias de catálogo, **no evidencia de que el usuario los tenga**, ni una
lista para reportar como instalada. No está demostrado que comprar el bundle
produzca una carpeta con su mismo label. Tampoco hay fundamento para exigir la
dificultad Pro del UFC original de 2014 ni para confundir ofertas de UFC 4 con UFC 3.

El ejecutable inspeccionado contiene `Flag_UnlockedBruceLee` en `0x1ebb16e` y
`ufc:01.00.00:career:progression:get-is-bruce-lee-unlocked` en `0x1ebf056`.
Es evidencia adicional de una ruta Career en esta versión, no una prueba de sus
condiciones completas. No se parcheó ese flag ni el save.

## 2. Evidencia local y llamadas del juego

Este worktree no contiene `orig/` ni `out/`. Se consultaron **solo en lectura** los
archivos existentes en `/home/myzk/Projects/ufc3-native/`:

- `orig/CUSA06534/sce_sys/param.sfo`: `TITLE_ID=CUSA06534`, `APP_VER=01.14`,
  `CATEGORY=gp`, `CONTENT_ID=EP0006-CUSA06534_00-EASPORTSUFC3GAME`.
- `orig/` contiene únicamente `CUSA06534`; no hay carpetas hermanas de DLC.
- SHA-256 de `orig/CUSA06534/eboot.bin`:
  `19ee0a2e837341e976a94ddca2d9510625511d72d0b0a70a40292e4c2a8f77fc`.
- Se reextrajo el SELF en memoria usando `scripts/prepare.py:parse_self`, sin
  ejecutar `prepare()` ni escribir salidas. Coincide byte a byte con el
  `out/eboot.elf` existente (SHA-256
  `08a4ffbc9b69934bfa9443fd116e49255c69d1c10834055c9a3f1f551399c18f`).
- Se cruzaron `out/analysis.json`, `out/import_names.txt`, `out/link.json`,
  `out/aerolib.inl` y las relocaciones de `out/boot.bin`; después se verificaron
  las llamadas con desensamblado x86-64. Las direcciones siguientes son VA del
  ELF relativas a la base guest, no direcciones del proceso host.

`src/import_names.inc` es un fallback diagnóstico que incluye nombres de otro
conjunto de imports: contiene List/Initialize/AppParamGetInt, pero omite Mount y
Unmount. Esa omisión **no** demuestra ausencia en el juego: los imports generados
de esta versión y el ELF sí los contienen, y `runtime_content_resolve()` ya
reconoce sus NID directamente.

| API / NID | GOT del eboot | Llamadas verificadas |
| --- | --- | --- |
| `sceAppContentInitialize` / `R9lA82OraNs` | `0x25c6748` | `0x11d6adc`, `0x12e6410`, `0x1d36a7c` |
| `sceAppContentAppParamGetInt` / `99b82IKXpH4` | `0x25c63c8` | `0x56b0f6`, `0x12e6437` |
| `sceAppContentGetAddcontInfoList` / `xnd8BJzAxmk` | `0x25c6740` | `0x1d36ad6`, `0x1d36b1c` |
| `sceAppContentAddcontMount` / `VANhIWcqYak` | `0x25c6730` | `0x1d36c08` |
| `sceAppContentAddcontUnmount` / `3rHWaV-1KC4` | `0x25c6738` | `0x1d36cbb` |

También importa TemporaryDataFormat, TemporaryDataGetAvailableSpaceKb y
TemporaryDataMount2. Son las ocho APIs AppContent encontradas. No aparecen imports
de `sceAppContentGetEntitlementKey`, `sceAppContentGetAddcontInfo` ni
`sceNpEntitlementAccess*`, ni una dependencia de esa última biblioteca. La única
importación directa sin nombre en aerolib es `qiOZEn26Kds` de `EAWebKit`.
Tampoco se encontró import de `sceKernelDlsym`. Esto delimita la evidencia; no es
una garantía sobre todas las rutas internas de bibliotecas o datos del juego.

La rutina en `0x1d36a45–0x1d36cdc` hace lo siguiente:

1. Carga AppContent (`0xb4`) e inicializa.
2. Consulta List con buffer NULL/capacidad 0 y luego con buffer dimensionado por
   el resultado. El service label sale de una configuración, no de una constante
   inmediata cero; el proveedor actual solo acepta cero.
3. Itera registros de **24 bytes**, consulta el estado en offset `0x14` y solo
   continúa si vale **4 (`Installed`)** (`0x1d36bf0`).
4. Llama Mount con el label; al recibir éxito forma
   **`<mount>/content/common/dimeContentInfo.xml`** (`0x1d36c47`) y llama una rutina
   local con esa ruta (`0x1d36c60`). Es evidencia de consumo de un manifiesto,
   no de que cualquier carpeta vacía sirva.
5. Procesa el contenido con llamadas internas/virtuales y finalmente desmonta.
   No se descompilaron todos esos destinos: no se afirma ausencia de red allí.

### Servidores EA y PSN: qué está demostrado

Hay imports `sceNpAuthGetAuthorizationCodeA`, `sceNpAuth*`,
`sceNpCommerceDialog*`, `sceNpWebApiCreateRequest`, `sceNpWebApiSendRequest2` y
`sceNpWebApiReadData`. La ausencia de NpEntitlementAccess no descarta consultas
web. El ELF contiene:

| VA | Cadena |
| --- | --- |
| `0x1f2bf8d` | `listEntitlements` |
| `0x1f3d20e` | `Blaze::Authentication::ListEntitlementsRequest` |
| `0x1fea642` | `/proxy/identity/pids/me/entitlements` |
| `0x1fea6f0` / `0x1fea719` | `HasEntitlement` / `ListEntitlements` (Nucleus) |
| `0x2080331` | `/v1/users/me/entitlements/%s?service_label=%d` (PSN web API) |
| `0x20803a4` | `/v1/users/me/entitlements?entitlement_type=%s&service_label=%d&start=%d&size=%d` |

**Conclusión acotada:** el juego incorpora consultas de derechos por EA/Nucleus y
PSN además de AppContent local. Las cadenas de SDK no prueban que Bruce Lee ni
cada DLC requieran esas consultas. No se reconstruyó una ruta completa desde su
selección hasta una respuesta EA; la necesidad de validación remota queda abierta.
La respuesta oficial sobre sincronización hace especialmente imprudente prometer
un desbloqueo Career enteramente offline.

En este port, `src/runtime_services.c` devuelve signed-out para WebApi offline y
cierra Commerce sin compra. `src/runtime_net.c:np_webapi_request()` solo admite
el grupo userProfile; otras peticiones fallan. Su simulación de sesión/PS Plus
para online no demuestra propiedad de DLC y no debe utilizarse para concederla.
No se propone hacer que EA/PSN devuelvan compras o recompensas inexistentes.

### Estado del proveedor

`src/runtime_content.c:addon_list()` siempre devuelve cero hits;
`addcont_mount()` devuelve `0x80d90005` (NOT_FOUND) sin tocar la salida;
`addcont_unmount()` solo devuelve éxito. `scripts/content_profile.py` genera
`mounted_addons=[]`; el formato `BBCONT01` solo lleva SKU y cuatro parámetros.
El SKU full del juego base no representa compras de add-ons.

## 3. Estructuras del dump y referencia shadPS4

El [reporte técnico del dumper xvortex #11](https://github.com/xvortex/ps4-dumper-vtx/issues/11)
describe datos descifrados bajo
`/mnt/sandbox/pfsmnt/CUSAXXXXX_00-DLCID-ac/` y paquetes en
`/user/addcont/CUSAXXXXX/DLCID/ac.pkg`. Propone una salida `CUSAXXXXX-ac/` con
subdirectorios por DLC. Advierte que el árbol montado puede omitir `param.sfo` y
que los DLC sin datos se registran aparte mediante licencias. Es un reporte de
trabajo del dumper, no una norma universal de nombres ni prueba de que todos los
dumpers generen `-addcont`.

Para el port se propone aceptar estas disposiciones **de archivos ya volcados y
descifrados**, seleccionando el DLC por su SFO, no por el nombre del directorio:

```text
orig/
  CUSA06534/eboot.bin
  CUSA06534-addcont/<entitlement>/sce_sys/param.sfo
  CUSA06534-addcont/<entitlement>/content/common/dimeContentInfo.xml

# Alias de importación posibles; no son rutas soportadas hoy:
orig/CUSA06534-ac/CUSA06534_00-<entitlement>-ac/sce_sys/param.sfo
orig/addcont/CUSA06534/<entitlement>/sce_sys/param.sfo
```

`<entitlement>` es un marcador, no un label que haya que crear. Los payloads
pueden tener otros archivos; el XML es el que esta rutina de UFC busca. Un `.pkg`
cifrado no es un directorio montable: extracción/descifrado queda fuera del MVP.
Un dump sin SFO necesita sus metadatos originales; no fabricaremos un SFO para
convertir un DLC ausente en presente.

La ruta solicitada `gpu/shadps4/core/libraries/app_content/` **no existe** en este
repo ni en el checkout compartido. `gpu/VENDOR.txt` fija la copia parcial en
`b4e7ae776071ceb1dd35004577a8719481e1d258`; AppContent no forma parte de ella.
Se consultó upstream y se fijó la referencia examinada en
`5674bf7e5b26ed57c0f38c46590694075afdbabb` (2026-10-20):

- [app_content.cpp](https://github.com/shadps4-emu/shadPS4/blob/5674bf7e5b26ed57c0f38c46590694075afdbabb/src/core/libraries/app_content/app_content.cpp):
  Initialize recorre `<addon-install-dir>/<TITLE_ID>/`, acepta directorios y
  `.zar`, lee `sce_sys/param.sfo`, filtra categoría `ac` cuando está presente y
  extrae el label del CONTENT_ID desde offset 20. List publica las entradas
  descubiertas; Mount busca el label y vuelve a validar el SFO antes de montar
  read-only en `/addcont<i>`. Unmount elimina el montaje. También carga
  licencias `.idx`/`.rif`, publica esos derechos como NoExtraData y emite
  EntitlementUpdate al encontrar contenido. GetEntitlementKey busca el label y
  copia una clave de una tabla inicializada a cero; no implementa aquí la
  obtención de claves originales. No debemos copiar esa respuesta como licencia.
- [app_content.h](https://github.com/shadps4-emu/shadPS4/blob/5674bf7e5b26ed57c0f38c46590694075afdbabb/src/core/libraries/app_content/app_content.h):
  label de 17 bytes más 3 de padding, estado u32: registro de 24 bytes; mount
  point de 16 bytes, key de 16. Installed=4 y NoExtraData=0.
- [rif.cpp](https://github.com/shadps4-emu/shadPS4/blob/5674bf7e5b26ed57c0f38c46590694075afdbabb/src/core/file_format/rif.cpp):
  lee RIDX/RIFA y selecciona entradas PS4AL para derechos sin datos. Es una
  referencia de representación; ese lector no constituye por sí mismo una
  verificación criptográfica de compra ni debe copiarse sin validar tamaños.

**Caso especial pendiente:** no tenemos el SFO, payload ni licencia original del
Bruce Lee de este usuario. No sabemos si encaja en el flujo `Installed` con XML.
El bucle analizado ignora NoExtraData: el soporte de licencias sin datos podría
requerir otro flujo del juego. No se marcarán artificialmente como Installed ni
se generará un manifiesto para forzar ese flujo.

## 4. Plan de implementación y esfuerzo

### MVP: add-ons con datos del dump propio

1. **Descubrimiento y validación.** Extender `scripts/content_profile.py` y su
   integración en `run.sh`/preparación de distribución. Partir de la ruta real del
   juego configurado y consultar hermanos `CUSA06534-addcont`, `CUSA06534-ac` y
   `addcont/CUSA06534`; admitir una raíz explícita si el dumper usa otra forma.
   Enumeración acotada, sin escanear todo el disco. Exigir SFO válido, categoría
   de add-on con datos, CONTENT_ID bien formado, label de hasta 16 caracteres y
   asociación de título/región compatible. Cualquier TITLE_ID del SFO debe ser
   consistente. No aceptar otro CUSA por renombrarlo; IDs de servicio adicionales
   solo si los declara el SFO base (en este dump están vacíos).
   Rechazar ambigüedades, duplicados conflictivos y escapes de la raíz mediante
   enlaces. Para UFC registrar si falta el XML, como diagnóstico de dump
   incompleto, sin inventarlo. Sin carpetas vacías ni IDs del catálogo como alta.
2. **Inventario verificable.** Introducir formato versionado o manifiesto adjunto
   al perfil: CONTENT_ID, label, ruta y clase de evidencia local. Conservar
   `BBCONT01` sin add-ons por compatibilidad. El inventario debe describir
   candidatos presentes, no un campo editable `purchased=true`. Revalidar SFO y
   presencia al cargar/montar para evitar perfiles obsoletos. Un SFO asocia
   archivos: no acredita criptográficamente una compra. El alcance asume el dump
   propio suministrado, sin descargas ni fabricación de derechos.
3. **Enumeración ABI.** En `src/runtime_content.c`, usar estructuras con asserts
   de tamaño/offset. List solo devuelve add-ons válidos presentes: consulta de
   cantidad con NULL/0, copia hasta capacidad, hits de lo efectivamente copiado,
   deja intactas las posiciones sobrantes. Orden estable por label. Mantener
   ausencia y errores para IDs ajenos; validar punteros, inicialización y service
   label. El soporte de labels distintos de cero requiere evidencia del dump y
   un mapeo explícito, no dar éxito indiscriminado.
4. **Montaje real.** Resolver label exactamente; montar la raíz extraída read-only
   en `/addcont0` para el primer add-on y `/addcont1`, etc. para otros simultáneos.
   Usar `runtime_file_mount()`/`runtime_file_unmount()`, controlar repetición,
   referencias y agotamiento. No superponer todos los DLC en `/addcont0` ni
   mezclarlos con `/app0`. Publicar el mount point solo después del éxito.
   Extender protección de escritura a `/addcontN`: `game_path()` hoy solo cubre
   `/app0` y `/hostapp`. Auditar todos los mutadores. Revisar `MAX_MOUNTS=16`,
   compartido con cinco montajes iniciales y SaveData; devolver error antes de
   agotar capacidad y no quitar montajes de saves.
5. **Estado de derechos.** Una sola tabla local respalda lista, búsqueda y mount.
   Installed solo para datos realmente presentes. No agregar API de entitlement
   sin una llamada que la requiera; si aparece, ausente = no entitlement,
   presente = únicamente lo sustentado por su dump. GetEntitlementKey no se
   necesita según los imports actuales; si se descubre una petición, falta de
   clave original debe producir error soportado, nunca una clave inventada.
   Evaluar EntitlementUpdate si el consumidor lo necesita. El juego base y el
   modo online no otorgan derechos adicionales.

### Fuera del MVP, hasta obtener evidencia

Licencias sin datos: aceptar solo registros originales volcados por el usuario,
con asociación verificable al juego; estudiar el flujo NoExtraData y cómo consulta
el juego esos derechos. Sin ellos, reportar ausencia. No transformar una mera
lista de nombres ni archivos del juego base en una compra. Consultas de EA/PSN:
determinar el predicado concreto antes de ampliar el backend; cualquier respuesta
local debe limitarse al mismo inventario y al progreso realmente ganado.

Career: dejar que la lógica del juego gane y persista la recompensa. Si falla,
investigar la lectura/escritura del progreso y la condición Hall of Fame, sin
forzar `Flag_UnlockedBruceLee`, logros, resultados de carrera ni compras.

| Trabajo | Estimación, un desarrollador |
| --- | --- |
| Inventario, SFO/asociaciones, perfil y rutas Linux/Windows | 1–2 días |
| ABI List/Mount/Unmount, permisos read-only, límites/lifecycle | 1–2 días |
| Pruebas host, diagnósticos y revisión con dump real | 1–2 días |
| **MVP con datos** | **3–6 días**, más espera por validación manual |
| Licencias sin datos / condición específica de Bruce Lee | 2–5 días adicionales de investigación inicial; implementación sin estimación firme hasta ver el dump |

No es un fix trivial de `AddcontMount`: sin lista, metadatos, permisos y contenido
real seguiría fallando. Si Bruce Lee es solo licencia o exige cuenta EA, el MVP
podría soportar otros add-ons y aun no resolver ese personaje. Esa ampliación
debe ser otro ticket con la evidencia correspondiente.

## 5. Verificación y próximos pasos

En este ticket se comprobaron hashes, SFO, imports y desensamblado; no se ejecutó
el juego. Solo se agrega documentación, por lo que no se compiló. No hay cambio
de comportamiento que probar todavía.

Para la implementación futura, extender `tests/test_content.py`,
`tests/test_content.c` y las pruebas de archivos con fixtures sintéticos: cero
DLC, uno/dos válidos, conteo/capacidad/canarios, ID ausente, SFO inválido o de otra
región, duplicados, carpeta retirada, enlaces que escapan, mount/unmount repetido,
colisión con SaveData y rechazo de todas las escrituras a `/addcontN`. Fixtures
de prueba no se convierten en entitlements de ejecución. Compilar y correr los
checks apropiados únicamente con `ufc3-build` bajo su turno; nunca `./build.sh`.

Validación manual posterior, por el usuario, de a una:

1. Sin DLC: lista vacía y Bruce Lee no concedido como compra; Career conserva
   su comportamiento normal.
2. Con un add-on propio real: log de CONTENT_ID/label/ruta, listado exacto,
   montaje y lectura de su XML/payload, selección si el juego lo reconoce.
   Agregar otro y verificar aislamiento.
3. Quitar ese add-on entre arranques: desaparece del inventario y no sobrevive
   un derecho comprado artificial. Un desbloqueo Career ganado puede persistir.
4. Con progreso propio al final de Career: Hall of Fame, guardado/reinicio y
   selección del personaje. Registrar si pide conexión; no equiparar este paso
   al montaje de un DLC. Esto puede ser una prueba larga y no se ejecuta aquí.

Falta recibir un inventario del DLC propio de este usuario (árbol, SFO y clase de
licencia/payload; no publicar claves), comprobar el flujo remoto específico y
validar en juego. El dump disponible hoy no permite certificar Bruce Lee.

## 6. Borrador corto para el issue #50 (inglés; no publicado)

> Bruce Lee can also be earned by completing Career Mode on any difficulty and
> entering the UFC Hall of Fame, according to [EA's official guidance](https://forums.ea.com/discussions/ufc-franchise-discussion-en/re-bruce-lee-still-locked-after-achieving-goat/11389059).
> That path has not yet been verified in this port. The current runtime does not
> mount DLC. Supporting add-ons dumped from your own console is feasible, but we
> still need to identify the files or original license your Bruce Lee add-on uses;
> mounting alone may not satisfy the game's account checks. We will not fake
> purchases or unlock paid content absent from your dump: having assets in the
> base game does not establish ownership of the add-on. Earned Career rewards are
> a separate, supported goal.
