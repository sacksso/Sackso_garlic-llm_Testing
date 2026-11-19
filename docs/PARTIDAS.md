# Modificar partidas guardadas

Una IA de 9B **no puede editar un binario "a ojo"**: no ve los bytes ni sabe dónde está el dinero. Lo que funciona es una herramienta que hace lo exacto (buscar, comparar, escribir) y la IA para explicar, planear y escribir scripts. Por eso hay `pc/savetool.py` (solo necesita Python 3 en el PC).

Nunca sobrescribe el original: lo que cambia va a un archivo nuevo (`.editado`). **Guarda siempre una copia de la partida antes de probar.**

## Partidas de PS4 (zip exportado)
1. Exporta la partida **descifrada** a un zip con la herramienta que ya uses para ello (por ejemplo Apollo Save Tool) y pásala al PC.
2. `python pc/savetool.py info partida.zip` dice qué trae: entradas, tamaños y si cada archivo es texto, binario o está cifrado/comprimido. Si sale "entropía alta", está cifrado y no se puede editar.
3. **El truco para encontrar un valor** (dinero, vidas, nivel…): guarda con 1000 de dinero, gasta o gana hasta tener otro número (por ejemplo 1350), guarda otra vez y exporta las dos:
   `python pc/savetool.py trace partida1.zip partida2.zip 1000 1350`
   Sale la lista de posiciones donde el 1000 pasó a 1350 (casi siempre una o dos).
4. Cambiar el valor: `python pc/savetool.py set partida2.zip 300 i32 999999` (tipos: `i32 u32 i16 u16 i64 f32 f64`; si no sabes cuál, `trace` lo dice).
5. Importa el zip `.editado` con la misma herramienta con la que exportaste (ella vuelve a firmar la partida).

Si el zip trae varios archivos de datos, añade `--entry NOMBRE`. Otras órdenes: `hex` (ver bytes), `strings` (texto visible), `find` (buscar un valor), `diff` (todo lo que cambió entre dos partidas).

**Qué no se puede:** partidas cifradas por el juego con clave propia, partidas en la nube verificadas por el servidor y cualquier cosa que el juego compruebe con una firma o suma de control (algunos juegos las rechazan aunque el valor sea correcto).

## Partidas de Ren'Py (PC / Android, como *Influencing*)
Los `.save` de Ren'Py son zips con un `pickle` de Python dentro. `savetool` lo lee **sin ejecutar nada** del archivo (un `.save` ajeno con trampas no hace daño):
```
python pc/savetool.py renpy-dump 1-1-LT1.save --grep dinero     # ver variables (busca por texto del nombre)
python pc/savetool.py renpy-set  1-1-LT1.save money 99999       # cambiarla
```
El cambio se aplica en todas las copias de la variable (también en el historial de retroceso, para que el juego no la deshaga). Puede salir un aviso de "partida de otro dispositivo o modificada": acepta. Las partidas de Android están en la carpeta de datos del juego (`.../saves`) y en PC en `game/saves` o `%APPDATA%/RenPy/<juego>`.

## Usar la IA para esto
Pega en el chat (o adjunta con `@`) la salida de `info`, `diff` o `renpy-dump` y pregunta qué significa cada valor, cómo plantear la búsqueda o cómo escribir un script para algo más complejo. Revisa siempre lo que te propone antes de ejecutarlo: el modelo puede inventar nombres de funciones o posiciones.
