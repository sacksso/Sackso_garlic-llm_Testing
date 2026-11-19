# Usar la IA de la PS4 desde el móvil, el PC y juegos

La consola sirve una API en tu red Wi-Fi/cable. Nada sale a internet. Mientras responde a un cliente, la pantalla de la PS4 muestra una nota y el chat de la consola queda a un lado.

## 1. Encender y ver las direcciones
1. En la PS4: `/api on` (o L3 en el mando). Luego `/api` muestra algo como `192.168.20.37:8080`.
2. Para que se encienda sola al abrir la app, déjala encendida: se recuerda.
3. Móvil y PC deben estar en **la misma red** que la consola.

## 2. Desde el móvil o el PC: la página de chat
Abre `http://IP-DE-LA-PS4:8080/` en el navegador. Es un chat completo (respuesta en directo, ajustes, historial local). Los ajustes (⚙) muestran las direcciones para copiar.

## 3. Desde otras apps y juegos
La API habla cuatro "idiomas". Usa el que pida tu app:

| La app pide… | Dirección | Notas |
|---|---|---|
| OpenAI / "custom OpenAI" / text-generation-webui | `http://IP:8080/v1` | Clave: cualquiera (o la de `/data/garlic_api_key.txt` si existe). Modelo: cualquiera. |
| KoboldCpp / KoboldAI | `http://IP:8080` | Soporta `generate`, el stream y las secuencias de parada (`stop_sequence`). |
| Ollama | `http://IP:8080` | Nombre de modelo: cualquiera. |
| Texto en bruto (`/v1/completions`) | `http://IP:8080/v1` | Un prompt tal cual, sin plantilla. |

Si el juego **no deja escribir el puerto** y espera uno fijo, cámbialo en la consola: `/puerto 5001` (KoboldCpp), `/puerto 11434` (Ollama) o `/puerto 5000` (text-generation-webui). Vale la próxima vez que enciendas la API (`/api off`, `/api on`).

### El juego *Influencing* (Golden Crow)
Su autor dice que la IA local se puede usar con Textgen, KoboldCPP u Ollama (o AI Horde). No he podido ver sus menús, así que prueba en este orden en sus ajustes de IA:
1. Elige **KoboldCPP**, dirección `http://IP:8080` (si pide solo el puerto o lo fija en 5001, haz `/puerto 5001`).
2. Si no funciona, elige **Ollama**, dirección `http://IP:8080` (o `/puerto 11434` si la tiene fija).
3. Si no, **Textgen / OpenAI**, dirección `http://IP:8080/v1` (o `/puerto 5000`).
Si lo logras, dime cuál y qué ajustes tenía, y lo dejo anotado.

## 4. Velocidad (importante para juegos)
- La PS4 lee el prompt a unos **75 tokens por segundo** y escribe a unos **35**. Un juego que manda 2 000 tokens de personaje e historia tarda ~25 s antes de la primera palabra.
- **`/cache on`** lo arregla cuando cada petición empieza igual que la anterior (el personaje, la historia hasta ahí): solo se lee lo nuevo. En el log aparece `api: cache hit: N of the M prompt tokens come from the saved state`. Es nuevo y no se ha probado en la consola: ver la prueba de abajo.
- Una petición a la vez: si el juego manda otra mientras responde, recibe "busy" (503).
- El texto que cabe por petición es de unos 15 000 caracteres (~4 000 tokens) y el contexto total es de 16 384 tokens.
- El modelo no "piensa" en la API (respuestas directas), y los ajustes `top_p`, `top_k`, penalizaciones, etc. se ignoran; sí se respetan `temperature`, longitud máxima, `seed` y las secuencias de parada.

## 5. Probar sin el juego
En el PC, con Python: `python pc/probar_api.py 192.168.20.37 8080` prueba los cuatro protocolos y muestra tiempos. Para ver si la caché es correcta: mismo comando con `--cache` (lanza dos veces la misma conversación con temperatura 0: las dos respuestas deben ser idénticas, y la segunda mucho más rápida).

## 6. Seguridad
La API no tiene contraseña por defecto: cualquiera en tu red puede usarla. Para pedir clave, escribe una en `/data/garlic_api_key.txt` y reinicia la API; las apps la mandan como `Authorization: Bearer clave`. No abras el puerto del router a internet.

## 7. Si algo falla
- La app no conecta: comprueba `/api`, que estén en la misma red y que no haya un firewall en el PC.
- "busy": el juego mandó una petición mientras la consola respondía otra.
- Respuestas raras con `/cache on`: apágala (`/cache off`) y mándame `ps4ai_log.txt` (líneas `api:`).
