#!/usr/bin/env python3
"""Prueba la API de la PS4 desde el PC (solo biblioteca estándar).

    python pc/probar_api.py 192.168.20.37 8080          # prueba los cuatro protocolos
    python pc/probar_api.py 192.168.20.37 8080 --cache  # compara la caché de prefijo (temperatura 0)
Antes, en la consola: /api on   (y /cache on para la segunda prueba).
"""
import json
import sys
import time
import urllib.error
import urllib.request


def call(base, path, body=None, stream=False, timeout=180):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(base + path, data=data, headers={"Content-Type": "application/json"},
                                 method="POST" if body is not None else "GET")
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            if not stream:
                return r.status, r.read().decode("utf-8", "replace"), time.time() - t0, None
            first = None
            chunks = []
            for line in r:
                if first is None:
                    first = time.time() - t0
                chunks.append(line.decode("utf-8", "replace"))
            return r.status, "".join(chunks), time.time() - t0, first
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace"), time.time() - t0, None
    except Exception as e:  # noqa: BLE001
        return 0, str(e), time.time() - t0, None


def show(name, res, expect=None):
    status, body, dt, first = res
    ok = status == 200 and (expect is None or expect in body)
    extra = f", primera respuesta a los {first:.1f} s" if first else ""
    print(f"[{'OK ' if ok else 'FALLA'}] {name}: HTTP {status}, {dt:.1f} s{extra}")
    if not ok:
        print("       ", body[:300].replace("\n", " "))
    return ok


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    host = args[0] if args else input("IP de la PS4: ")
    port = args[1] if len(args) > 1 else "8080"
    base = f"http://{host}:{port}"
    msgs = [{"role": "user", "content": "Responde solo con la palabra: hola"}]
    if "--cache" in sys.argv:
        story = "Eres Marta, una tendera amable de un pueblo. " * 40
        conv = [{"role": "system", "content": story}, {"role": "user", "content": "Hola, ¿qué vendes?"}]
        body = {"messages": conv, "temperature": 0, "max_tokens": 30}
        r1 = call(base, "/v1/chat/completions", body)
        a1 = json.loads(r1[1])["choices"][0]["message"]["content"] if r1[0] == 200 else ""
        conv2 = conv + [{"role": "assistant", "content": a1}, {"role": "user", "content": "¿Y cuánto cuesta el pan?"}]
        r2 = call(base, "/v1/chat/completions", {"messages": conv2, "temperature": 0, "max_tokens": 30})
        r3 = call(base, "/v1/chat/completions", {"messages": conv2, "temperature": 0, "max_tokens": 30})
        print(f"1.ª petición (lee todo): {r1[2]:.1f} s")
        print(f"2.ª (historia más larga): {r2[2]:.1f} s")
        print(f"3.ª (la misma otra vez):  {r3[2]:.1f} s")
        same = r2[0] == 200 and r3[0] == 200 and \
            json.loads(r2[1])["choices"][0]["message"]["content"] == json.loads(r3[1])["choices"][0]["message"]["content"]
        print("Con temperatura 0 las dos respuestas iguales deben dar el mismo texto:", "SÍ" if same else "NO (¡avisa!)")
        return
    ok = True
    ok &= show("información (OpenAI /v1/models)", call(base, "/v1/models"))
    ok &= show("información (Kobold /api/v1/model)", call(base, "/api/v1/model"))
    ok &= show("información (Ollama /api/tags)", call(base, "/api/tags"))
    ok &= show("OpenAI chat", call(base, "/v1/chat/completions", {"messages": msgs, "max_tokens": 20}), "choices")
    ok &= show("OpenAI chat en directo", call(base, "/v1/chat/completions", {"messages": msgs, "max_tokens": 20, "stream": True}, True), "[DONE]")
    ok &= show("OpenAI texto", call(base, "/v1/completions", {"prompt": "El cielo es de color", "max_tokens": 10}), "choices")
    ok &= show("KoboldCpp generate", call(base, "/api/v1/generate", {"prompt": "El cielo es de color", "max_length": 10}), "results")
    ok &= show("KoboldCpp con stop_sequence", call(base, "/api/v1/generate", {"prompt": "Uno, dos, tres, cuatro, cinco, seis, siete", "max_length": 30, "stop_sequence": ["cinco"]}), "results")
    ok &= show("KoboldCpp en directo", call(base, "/api/extra/generate/stream", {"prompt": "Hola", "max_length": 10}, True), "token")
    ok &= show("Ollama chat (en directo por defecto)", call(base, "/api/chat", {"model": "x", "messages": msgs, "options": {"num_predict": 20}}, True), "\"done\":true")
    ok &= show("Ollama generate", call(base, "/api/generate", {"model": "x", "prompt": "Di hola", "stream": False, "options": {"num_predict": 20}}), "response")
    print("\nTodo bien." if ok else "\nAlgo falló: mira /data/ps4ai_log.txt (líneas api:) en la consola.")


if __name__ == "__main__":
    main()
