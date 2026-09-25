# AndroidPrybar

> **English** · [中文](README.zh-CN.md)

> **AndroidPrybar is a good-faith binary analysis tool, intended for binary vulnerability discovery and security research.** It is meant to help researchers understand and audit native code on their own or authorized targets; please use it only within the bounds of applicable law and proper authorization.

ARM64 **function-level VCPU (programmable virtual CPU)** + instruction-tracing framework. Drop any native function into the Unicorn engine and run it while keeping full, debugger-like control: per-instruction / basic-block / memory / SVC / external-call hooks, register read & write, single-step, breakpoints, CPU snapshots, and disassembly.

`trace()` **is just a wrapper over this VCPU.** You can call `trace()` directly for one-shot logging; if the trace log format doesn't suit you, modify it yourself — everything is ultimately a wrapper over the VCPU (`vc_make_handle` + the VCPU's internal hook-callback interfaces). `trace()` / `trace_unidbg_dump()` / `replace_trace()` are all wrappers over this VCPU layer.

The VCPU is the closed-source engine core; `trace/` is an **open-source** sample application built on top of it (the decoupling from the VCPU was done by AI, calling only the public `vc_*` interfaces). So the trace source itself is the best example of "how to use this VCPU" — take it as a template to customize, and `trace/build_trace.sh` rebuilds `libtrace.so`; the VCPU source is never required.

Supports multi-process, multi-thread, and high concurrency, tracing multiple functions at once. The project already ships a prebuilt `libtrace.so`, separated headers, and a minimal command-line example (`demo/`, pure C usage, no app shell).

## Changelog

- Added `vc_set_trace_crash_flush()`: force-flush the trace buffer to disk before the app crashes / terminates / exits, so the portion before the crash point is never lost.
- `trace_receiver.py` now decompresses via the lz4 C engine (`pip install lz4`), greatly speeding up `decode`; falls back to pure Python automatically if the package is missing.
- Added `trace_unidbg_dump()`: one-shot export of a Unidbg "mid-execution" dump package (memory segments / registers / arguments / symbols / JNI table / maps / rootfs).
- README: added a Frida example for calling `trace()`.
- Fixed a number of bugs.

## Where is `libtrace.so`

Prebuilt shared library: `libs/prebuilt/arm64-v8a/libtrace.so` (when running the demo, `adb push` it together with the executable to `/data/local/tmp/` on the device).

Public headers (**separated**): `include/vcpu.h` (VCPU core API) + `include/trace.h` (trace tooling, which already `#include "vcpu.h"` at the top). To use trace, just `#include "trace.h"`.

### Open-source layering (important)

- `trace/` — the open-source trace-layer source, linked against the closed-source `libvcpu.a` to build `libtrace.so`.
- `libs/arm64-v8a/libvcpu.a` **/** `libvcpu.so` — closed-source binary assets: **the VCPU + Unicorn engine merged together, with all internal symbols stripped, exposing only the** `vc_*` **interfaces.** The `.a` is for static embedding into trace; the `.so` is for those who load the raw VCPU API dynamically.
- **Two ways to use it:** ① clone and go — use `libs/prebuilt/arm64-v8a/libtrace.so` + `include/*.h` directly; ② after editing the trace source, run `NDK=/path bash trace/build_trace.sh` to rebuild `libtrace.so` (needs only the open-source trace source + the ready-made `libvcpu.a`).

---

## Quick start

> **Two layers of usage, pick as needed:**
>
> - **Ready-made wrappers** (no logic to write, one call for results): `trace()`, `trace_unidbg_dump()`, `replace_trace()`.
> - **The low-level VCPU** (write your own callbacks, drive execution yourself): `vc_make_handle()` + `vc_hook_add()` + register read/write + single-step / breakpoints / CPU snapshots / disassembly.
>
> The wrappers are **all built on** the VCPU API below. So you can use it as a "one-click trace tool" or as a "programmable ARM64 VCPU" — same thing.

## Documentation

Detailed usage is split into two docs, read what you need:

- **[docs/VCPU.md](docs/VCPU.md)** — the low-level programmable VCPU: `vc_make_handle` to get a handle, the various Hooks, register read/write, single-step / breakpoints, CPU snapshots, disassembly, jump control, memory monitoring, plus a "modify registers / return values at runtime" walkthrough. **Read this too if you want to build your own trace.**
- **[docs/TRACE.md](docs/TRACE.md)** — the ready-made trace tooling: `trace()`, Frida invocation, automatic thread tracing, crash-safe flushing, `replace_trace()`, `trace_unidbg_dump()`, the trace output format, and the companion decode / call-tree tools.

## API cheat sheet

| API                                               | Purpose                                                                          |
| ------------------------------------------------- | -------------------------------------------------------------------------------- |
| `trace(func, path)`                               | Quick trace (path is a directory → local `.lz4`, `"tcp:PORT"` → remote)          |
| `trace(func, path, &ctx)`                         | trace with a ctx                                                                  |
| `freeTrace(wrapper)`                              | Release a trace handle                                                            |
| `replace_trace(func, path)`                       | Global replace-style trace                                                        |
| `restore_function(func)`                          | Restore a replaced function                                                       |
| `trace_unidbg_dump(func, dumpDir)`                | Export a Unidbg mid-execution dump package (auto-flushed on completion, returns a callable pointer) |
| `trace_unidbg_dump_finish(wrapper)`               | Release the dump handle (files already flushed; just reclaims resources)          |
| `vc_make_handle(func, &ctx)`                      | Create a raw VM handle                                                            |
| `vc_free(ctx)`                                    | Release the VM context                                                            |
| `vc_hook_add(ctx, &hh, type, cb, ud, begin, end)` | Register a hook                                                                   |
| `vc_hook_del(ctx, hh)`                            | Remove a hook                                                                     |
| `vc_reg_read / vc_reg_write`                      | Register read/write                                                               |
| `vc_reg_read_batch / vc_reg_write_batch`          | Batch read/write                                                                  |
| `vc_emu_stop(ctx)`                                | Stop the VM                                                                       |
| `vc_single_step(ctx, count)`                      | Pause after N instructions                                                        |
| `vc_set_until(ctx, addr)`                         | Set a temporary breakpoint                                                        |
| `vc_disasm(addr, count, out)`                     | Disassemble                                                                       |
| `vc_context_save / restore / free`                | CPU snapshot                                                                      |
| `vc_lookup_symbol(ctx, addr)`                     | Resolve address to symbol                                                         |
| `vc_set_jump_blacklist(names, ranges, n)`         | Set the jump blacklist                                                            |
| `vc_clear_jump_blacklist()`                       | Clear the blacklist                                                               |
| `vc_set_external_jump_enabled(enabled)`           | Global jump switch                                                                |
| `vc_set_auto_trace_threads(ctx, enable)`          | Auto-trace threads created inside the traced function (**off by default**; take ctx via the 3-arg form and pass true to enable) |
| `vc_set_trace_crash_flush(enable)`                | Auto-flush the trace buffer to disk before crash/terminate/exit (file mode), so the pre-crash portion isn't lost |
| `trace_read / trace_write`                        | Memory monitoring                                                                 |

## Performance reference

| Mode                   | Speed          | Use case                              |
| ---------------------- | -------------- | ------------------------------------- |
| vc_make_handle (default) | ~2-5x slower  | Functional validation, external-call monitoring |
| trace() instruction-level | ~50-100x slower | Detailed analysis, reverse engineering |
| vc_make_handle + CODE  | ~10-20x slower | Custom per-instruction monitoring     |
| vc_single_step(ctx, 1) | ~100-200x slower | Precise debugging                    |

---

## Demo

`demo/` is a minimal command-line example (**no Android app shell**): `main.cpp` defines its own C function, wraps it with `trace()` → runs it in the VCPU → writes a trace log. The core is just three steps (`trace()` for a pointer → call it → `freeTrace()`).

Full steps for building, `adb push`-ing to the device, and `decode`-ing the result are in **[demo/README.md](demo/README.md)**.

## Project structure

```text
AndroidPrybar/
|-- include/                         ← public headers (separated)
|   |-- vcpu.h                       ←   VCPU core API (matches libvcpu.a)
|   `-- trace.h                      ←   trace tooling API (#include "vcpu.h")
|-- libs/
|   |-- arm64-v8a/
|   |   |-- libvcpu.a                ←   closed-source asset: VCPU+engine merged, symbols hidden (static)
|   |   |-- libvcpu.so               ←   same as above (dynamic, for raw VCPU use)
|   |   |-- libcapstone.a            ←   trace dependency (disassembly)
|   |   `-- libdobby.a               ←   trace dependency (inline hook)
|   `-- prebuilt/arm64-v8a/
|       `-- libtrace.so              ←   prebuilt product (clone and go)
|-- trace/                           ← open-source trace layer
|   |-- src/                         ←   trace source (EastTrace/JniTrace/…)
|   |-- include/                     ←   trace's own headers (incl. ARM64Emulator.h compat shim)
|   |-- Utils/                       ←   common utility headers (symbols live in libvcpu.a)
|   |-- thirdparty/include/          ←   unicorn/capstone/dobby headers used at build time
|   |-- trace.exports                ←   export-symbol version script
|   |-- CMakeLists.txt / build_trace.sh  ← two ways to rebuild
|-- demo/                            ← minimal command-line example (no app shell)
|   |-- main.cpp                     ←   trace's own C function → produces a log
|   `-- build.sh                     ←   NDK-build an arm64 executable, links libtrace.so
|-- tools/
|   |-- trace_receiver.py            ← TCP receiver + LZ4 decoder
|   `-- build_calltree.py            ← trace → function call tree / call graph
`-- README.md
```

### `.claude/skills/unicorn-trace/` — Claude Code usage skill

The repo ships a **Claude Code skill: `unicorn-trace`**. When you open this repo with Claude Code, it automatically loads the full usage of libtrace, so the AI knows how to call `trace()` / `vc_make_handle` / the various hooks without re-explaining each time.

- `SKILL.md`: a concise quick reference (the two entry points, API cheat sheet, hook types, worked-example index, trace format, performance, limits).
- `GUIDE.md`: the full guide (complete API + worked examples + trace format).
- To use it in another project: copy `.claude/skills/unicorn-trace/` into that project's `.claude/skills/` or the user-level `~/.claude/skills/`.

## Community / contact

You're welcome to scan the QR code and join the group to learn and discuss Android native / trace / VM topics together.

Author WeChat: `klovemh3344`

Group chat: import FacaiTrace

## A few personal words

I worked on this tool on and off for two years. Back then there weren't many good, publicly available trace tools, and the underlying ideas were rarely shared, so a lot of it I had to feel out, test, and rebuild from scratch on my own — just getting a JIT that supports automatic argument passing took a long time.

Now I'm sharing it. Use it if it helps, change it, copy it, no attribution required. If it helped you, you're welcome to join the group and chat — that's a bit of motivation for me to keep maintaining it.
