# Vendored MinHook

Upstream: https://github.com/TsudaKageyu/minhook (BSD-2-Clause), pinned to commit
`d94c64d32ea37bc4f5ee47d580709f70c6fb6080` (2026-06-13). Nine of the ten source files are
byte-for-byte that commit.

`src/hook.c` has the thread-freeze code removed (taken from
https://github.com/blancodagoat/texoverride, also BSD-2-Clause; its header comment explains
the change). Why we need it: upstream `MH_EnableHook` suspends every other thread via
`CreateToolhelp32Snapshot`/`Thread32Next`, FiveM blocks that call, and MinHook reports the
failed enumeration as `MH_ERROR_MEMORY_ALLOC`, so no hook can ever be enabled inside FiveM
with stock MinHook. We patch from the init thread right after `DllMain`, before the game's
render loop exists, so there is nothing to freeze anyway. The END-key eject patches while the
game is running and accepts a theoretical race; it is a dev convenience.

Verify:

    git clone https://github.com/TsudaKageyu/minhook.git /tmp/minhook
    cd /tmp/minhook && git checkout d94c64d32ea37bc4f5ee47d580709f70c6fb6080
    diff -r --strip-trailing-cr include src <this repo>/third_party/minhook/include <this repo>/third_party/minhook/src

Exactly one file should differ: `src/hook.c`.
