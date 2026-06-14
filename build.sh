#!/bin/sh
# Both demos are compiled and each wasm is base64-embedded into its own html:
#   render_wasm.cpp → render.wasm → index.html  (black hole + object launch)
#   oda_wasm.cpp    → oda.wasm    → oda.html    (checkered room + 3 reflective spheres)
# Requirements: zig (brew install zig), python3.
set -e
cd "$(dirname "$0")"

compile(){
  zig c++ -target wasm32-freestanding -O2 \
    -nostdlib -ffreestanding -fno-exceptions -fno-rtti -fvisibility=hidden \
    -Wl,--no-entry -Wl,-z,stack-size=131072 -Wl,--strip-all \
    -o "$2" "$1"
}

compile render_wasm.cpp render.wasm
compile oda_wasm.cpp    oda.wasm

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
