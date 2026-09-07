# UC_HOOK_CODE_FAST Quick Start

This fork adds a private AArch64-only tracing hook named `UC_HOOK_CODE_FAST`.

Its goal is to keep the timing of `UC_HOOK_CODE` while making per-instruction
trace collection cheaper when the callback only reads a few registers.

## What It Does

`UC_HOOK_CODE_FAST` is called before each matched AArch64 instruction executes.

Compared with stock `UC_HOOK_CODE`:

- it still fires once per executed instruction
- it still runs before the instruction executes
- it uses the same simple callback shape as `UC_HOOK_CODE`
- it adds `uc_reg_read_fast()` for callback-local register reads

This is useful for trace engines that need:

- the current `pc`
- a random subset of general registers
- a random subset of SIMD or system registers
- accurate pre-instruction memory reads via `uc_mem_read()`

## Public API

Header definitions live in `include/unicorn/unicorn.h`.

### Callback Type

```c
typedef void (*uc_cb_hookcode_fast_t)(
    uc_engine *uc,
    uint64_t address,
    uint32_t size,
    void *user_data);
```

### Hook Type

```c
UC_HOOK_CODE_FAST
```

### Fast Register Read API

```c
uc_err uc_reg_read_fast(uc_engine *uc, int regid, void *value);
uc_err uc_reg_read_fast_batch(uc_engine *uc, int const *regs, void **vals,
                              int count);
uc_err uc_reg_write_fast(uc_engine *uc, int regid, const void *value);
```

`uc_reg_read_fast()` reads the live CPU state for the current
`UC_HOOK_CODE_FAST` callback.

## Minimal Example

```c
#include <inttypes.h>
#include <stdio.h>
#include <unicorn/unicorn.h>

static void hook_code_fast(uc_engine *uc, uint64_t address, uint32_t size,
                           void *user_data)
{
    uint64_t x0 = 0;
    uint64_t sp = 0;

    if (uc_reg_read_fast(uc, UC_ARM64_REG_X0, &x0) != UC_ERR_OK) {
        return;
    }

    if (uc_reg_read_fast(uc, UC_ARM64_REG_SP, &sp) != UC_ERR_OK) {
        return;
    }

    printf("pc=%" PRIx64 " size=%u x0=%" PRIx64 " sp=%" PRIx64 "\n",
           address, size, x0, sp);

    (void)user_data;
}

static uc_err install_fast_trace(uc_engine *uc, uc_hook *hh)
{
    return uc_hook_add(uc, hh, UC_HOOK_CODE_FAST, hook_code_fast, NULL, 1, 0);
}
```

## Timing Semantics

`UC_HOOK_CODE_FAST` is a pre-execution hook.

That means:

- the callback runs before the current instruction executes
- `uc_reg_read_fast()` returns the current pre-execution register state
- `uc_mem_read()` observes memory before the instruction executes
- `uc_emu_stop()` stops before the current instruction executes

This matches the timing model of stock `UC_HOOK_CODE`.

## Important Limits

### 1. AArch64 Only

This extension only works for `UC_ARCH_ARM64`.

### 2. Multiple Fast Hooks Are Allowed

You can register multiple `UC_HOOK_CODE_FAST` hooks, just like `UC_HOOK_CODE`.

For one instruction, Unicorn reuses the same live CPU context and dispatches
all matching `UC_HOOK_CODE_FAST` callbacks in hook order.

### 3. Not Combined With Other Hook Bits

`uc_hook_add()` expects the hook type to be exactly `UC_HOOK_CODE_FAST`.

Do not combine it like this:

```c
UC_HOOK_CODE_FAST | UC_HOOK_MEM_READ
```

### 4. `uc_reg_read_fast()` Is Callback-Local

`uc_reg_read_fast()` only works while a `UC_HOOK_CODE_FAST` callback is
currently running.

It returns `UC_ERR_ARG` in these cases:

- from `UC_HOOK_BLOCK`
- from regular `UC_HOOK_CODE`
- from `UC_HOOK_MEM_*`
- after the fast hook callback has returned

### 5. `uc_reg_read_fast()` Uses Live State

`uc_reg_read_fast()` reads the same live AArch64 state as `uc_reg_read()`, but
it is explicitly tied to the current `UC_HOOK_CODE_FAST` callback context.

If you mutate registers inside the callback and then call `uc_reg_read_fast()`
again, you will observe the updated value.

### 6. `uc_reg_read()` Still Exists

Regular `uc_reg_read()` can still work inside `UC_HOOK_CODE_FAST`, but
`uc_reg_read_fast()` is preferred because its valid usage window is explicit and
documented for this hook.

## Practical Guidance

If you integrate this fork into another project:

1. Register one or more `UC_HOOK_CODE_FAST` hooks as needed.
2. Treat the callback as a per-instruction pre-execution trace point.
3. Use `uc_reg_read_fast()` only for the registers you actually need.
4. Use `uc_reg_read_fast_batch()` when one instruction needs multiple register
   reads.
5. Use `uc_reg_write_fast()` if the callback must patch the current live CPU
   state.
6. Use `uc_mem_read()` inside the callback if you need current memory contents.
7. Do not assume this API exists in upstream Unicorn.

## Why This Is Faster Than The Previous Version

It still pays the cost of a per-instruction callback.

The main savings come from avoiding any eager per-instruction register context
construction:

- no public trace record is built for each instruction
- `uc_reg_read_fast()` reads the current callback-local live state on demand
- `uc_reg_read_fast_batch()` avoids repeated API entry/exit when several
  registers are needed
- `uc_reg_write_fast()` updates the current callback-local live state directly
- callbacks that only touch a few registers no longer pay to prepare all of
  them

So this extension is most useful when your trace callback needs register state
for many instructions but only reads a subset of registers on each one.

## Files To Check

If you need to port or extend this feature, the main implementation points are:

- `include/unicorn/unicorn.h`
- `include/uc_priv.h`
- `uc.c`
- `qemu/target/arm/helper-a64.c`
- `qemu/target/arm/unicorn_aarch64.c`
- `qemu/target/arm/translate-a64.c`
