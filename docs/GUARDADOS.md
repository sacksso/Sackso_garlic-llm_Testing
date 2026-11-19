# Modificar una partida guardada con Garlic LLM (fase 10)

La IA de la consola **no lee bytes ni hace aritmética**; la app sí. Lo que cambia respecto a antes: los archivos binarios de un zip ya no se esconden ni se rechazan, y la app da a la IA datos exactos (hex, números, offsets) en vez de que los adivine.

## Qué se puede pedir (con `@`, también lo pide la IA sola y `/ok` o `/auto` lo traen)
| Escribes | Resultado |
|---|---|
| `@partida.zip` | Lista **todos** los archivos del zip con su tamaño (los binarios también, si es un zip pequeño). |
| `@zip:SAVEDATA/slot0.dat` | Informe: formato, texto que contiene, si los bytes parecen **cifrados/comprimidos** (entonces no se puede editar por offset) y los primeros 160 bytes en hex. |
| `@zip:archivo:0x100-0x1ff` | Hex de ese rango (máx. 512 bytes) y, al final, el valor de los primeros bytes como u16/u32/i32/f32/u64/f64. |
| `@zip?=12500` | Dónde está el número 12500 en **todos** los archivos (u16/u32/i32/u64 en little y big endian, float y double). Acepta `-3`, `0x1F4`, `12.5`. |
| `@zip:archivo?=12500` | Lo mismo, solo en un archivo. |
| `@antes.zip~despues.zip` | Qué bytes cambiaron entre dos partidas, con el valor viejo y el nuevo. |

`@zip`, `@repo` o un trozo del nombre (`@CUSA`) valen para el último zip usado.

## Método (dinero, vidas, objetos...)
1. Copia la partida por FTP a `/data/garlic_files/` (zip de tu exportador de partidas) y escribe `@zip?=VALOR` con el valor que **muestra el juego**.
2. Si sale en varios sitios (lo normal con números pequeños): en el juego **gasta o cambia ese valor**, exporta otra partida (`despues.zip`) y escribe `@antes.zip~despues.zip`. El offset cuyo número pasó del valor viejo al nuevo es el bueno. Cambios pequeños al lado (suma de comprobación, tiempo de juego) son normales.
3. `/poke OFFSET TIPO VALOR ARCHIVO [zip]`, por ejemplo `/poke 0x40 u32 999999 SAVEDATA/slot0.dat`. Tipos: `u8 u16 u32 u64 i8 i16 i32 i64 f32 f64` (little endian). Crea **`nombre-mod.zip`** en `/data/garlic_files/`; el original no se toca. La app lo vuelve a leer y comprueba que los bytes quedaron bien.
   - Si la IA termina su respuesta con una línea `/poke ...`, basta escribir **`/aplicar`**: la app ejecuta esa línea.
4. Baja `nombre-mod.zip`, restáuralo en la consola con tu herramienta de partidas (Save Mounter, Apollo...) y **prueba con copia de seguridad**.

## Límites honestos
- Si el informe avisa de bytes aleatorios, el archivo va cifrado o comprimido: no se puede editar por offset (haría falta otra herramienta).
- Si el juego guarda una suma de comprobación, hay que recalcularla; la app no lo hace. La comparación te dice qué bytes cambian al lado del valor.
- Máx. 6 MB por archivo del zip, sin zip64 ni zips cifrados. Todo se guarda sin comprimir.
- Edita solo tus partidas de un jugador; en juegos online puede haber sanciones.

## Programar mejor: erratas en BUSCAR
Cuando el modelo copia el texto de `BUSCAR` con **un carácter mal** en una línea larga (`ln` por `in`), `/aplicar` y `/auto` ya lo aplican si solo una línea difiere y el resto coincide (la nota dice `typo`). Las líneas cortas no se toleran (`x = 1` y `x = 2` significan cosas distintas).
