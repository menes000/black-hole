#!/bin/sh
# render_wasm.cpp → render.wasm → index.html içine base64 gömme.
# Gereksinim: zig (brew install zig), python3.
set -e
cd "$(dirname "$0")"

zig c++ -target wasm32-freestanding -O2 \
  -nostdlib -ffreestanding -fno-exceptions -fno-rtti -fvisibility=hidden \
  -Wl,--no-entry -Wl,-z,stack-size=131072 -Wl,--strip-all \
  -o render.wasm render_wasm.cpp

python3 - <<'EOF'
import base64, re, pathlib
w = base64.b64encode(pathlib.Path('render.wasm').read_bytes()).decode()
p = pathlib.Path('index.html'); s = p.read_text()
s2 = re.sub(r'const WASM_B64="[^"]*"', 'const WASM_B64="'+w+'"', s, count=1)
assert s2 != s or w in s
p.write_text(s2)
EOF
echo "OK: $(wc -c < render.wasm | tr -d ' ') bayt gömüldü"
