#!/bin/sh
# İki demo derlenir ve her wasm kendi html'ine base64 gömülür:
#   render_wasm.cpp → render.wasm → index.html  (kara delik + cisim fırlatma)
#   oda_wasm.cpp    → oda.wasm    → oda.html    (damalı oda + 3 yansıtıcı küre)
# Gereksinim: zig (brew install zig), python3.
set -e
cd "$(dirname "$0")"

derle(){
  zig c++ -target wasm32-freestanding -O2 \
    -nostdlib -ffreestanding -fno-exceptions -fno-rtti -fvisibility=hidden \
    -Wl,--no-entry -Wl,-z,stack-size=131072 -Wl,--strip-all \
    -o "$2" "$1"
}

derle render_wasm.cpp render.wasm
derle oda_wasm.cpp    oda.wasm

python3 - <<'EOF'
import base64, re, pathlib
for wasm, html in (('render.wasm','index.html'), ('oda.wasm','oda.html')):
    w = base64.b64encode(pathlib.Path(wasm).read_bytes()).decode()
    p = pathlib.Path(html); s = p.read_text()
    s2 = re.sub(r'const WASM_B64="[^"]*"', 'const WASM_B64="'+w+'"', s, count=1)
    assert s2 != s or w in s, html
    p.write_text(s2)
EOF
echo "OK: render.wasm $(wc -c < render.wasm | tr -d ' ') B → index.html · oda.wasm $(wc -c < oda.wasm | tr -d ' ') B → oda.html"
