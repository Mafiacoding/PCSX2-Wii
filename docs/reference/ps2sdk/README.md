# ps2sdk reference sources

Real, unmodified EE-kernel source files from the [ps2dev/ps2sdk](https://github.com/ps2dev/ps2sdk)
project (Academic Free License 2.0), kept here for direct reference/citation while
reverse-engineering this project's own SIF-RPC and module-loading implementation.

- `sifrpc.c` - `ee/kernel/src/sifrpc.c` - the real EE-side SIF RPC implementation
  (`sceSifBindRpc`, `sceSifCallRpc`, the `SifRpcCallPkt_t`/`rpc_number`/`recvbuf`/`recv_size`
  packet-field layout this project's own `call_cd`/`call_sid`/`rpc_number` census
  (Round 487-490) is cross-checked against).
- `loadfile.h` - `ee/kernel/include/loadfile.h` - the real LOADFILE RPC client API
  (`SifLoadModule`/`SifExecModuleFile`/etc.), relevant to this project's earlier
  `_LoadExecPS2`/module-loading trampoline work (Rounds 457-469).

Fetched verbatim from GitHub `master` on 2026-08-05, per explicit user request.
