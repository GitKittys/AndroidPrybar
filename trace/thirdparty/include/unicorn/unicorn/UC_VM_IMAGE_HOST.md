# Host-Oriented Shared VM Image (V2)

This fork adds a host-oriented template API for creating multiple Unicorn
contexts that reuse the same memory-layout description and host-backed mappings.

The goal is to reduce repeated setup cost when many host threads need to run the
same ARM64 VM workload, while still preserving normal Unicorn callback
semantics:

- each context still has its own CPU/register state
- hooks are still registered per context
- callbacks can still read registers
- callbacks can still write registers
- callbacks can still change PC

This V2 keeps the same conservative sharing boundary, but adds an image-level
idle context pool:

- it shares the memory-layout template
- it reuses the same host pointers for `uc_mem_map_ptr()` regions
- it can replay common control settings
- it can prewarm selected TB entry addresses
- it can recycle fully initialized `uc_engine` instances per image
- recycled contexts keep their own TB/JIT cache warm across reuse
- it still does NOT make TCG/TB/TLB globally shared across all live contexts

## New API

```c
typedef struct uc_vm_image uc_vm_image;

uc_err uc_vm_image_open(uc_arch arch, uc_mode mode, uc_vm_image **image);
void   uc_vm_image_close(uc_vm_image *image);

uc_err uc_vm_image_map(uc_vm_image *image, uint64_t address, uint64_t size,
                       uint32_t perms);
uc_err uc_vm_image_map_ptr(uc_vm_image *image, uint64_t address, uint64_t size,
                           uint32_t perms, void *ptr);

uc_err uc_vm_image_set_cpu_model(uc_vm_image *image, int model);
uc_err uc_vm_image_set_page_size(uc_vm_image *image, uint32_t page_size);
uc_err uc_vm_image_set_tcg_buffer_size(uc_vm_image *image, uint32_t size);
uc_err uc_vm_image_set_tlb_mode(uc_vm_image *image, int mode);
uc_err uc_vm_image_set_host_ptr_auto_map(uc_vm_image *image, bool enabled);
uc_err uc_vm_image_add_host_ptr_auto_range(uc_vm_image *image,
                                           uint64_t guest_base, uint64_t size,
                                           uint32_t perms, uint64_t host_base);
uc_err uc_vm_image_set_pool_capacity(uc_vm_image *image, uint32_t capacity);
uc_err uc_vm_image_add_prewarm(uc_vm_image *image, uint64_t address);

uc_err uc_open_from_image(const uc_vm_image *image, uc_engine **uc);
```

## Recommended Usage Pattern

1. Build one shared image on the host process startup.
2. Add the common code/data mappings to the image.
3. Optional: set an internal idle pool capacity.
4. Optional: register explicit guest->host auto-map ranges if you want
   host_ptr_auto_map without scanning process mappings.
5. Add optional hot entry addresses for TB prewarm.
6. For each worker thread, create one Unicorn context with
   `uc_open_from_image()`.
6. Register hooks on that context.
7. Set thread-local register state and thread-local memory regions.
8. Run `uc_emu_start()`.
9. Call `uc_close()` when done. If pooling is enabled, the context is reset and
   returned to the image's idle cache.

## Example

```c
#include <stdbool.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

static void hook_code_fast(uc_engine *uc, uint64_t address, uint32_t size,
                           void *user_data)
{
    uint64_t x0 = 0;
    uc_reg_read_fast(uc, UC_ARM64_REG_X0, &x0);
    (void)address;
    (void)size;
    (void)user_data;
}

static uc_vm_image *build_image(void *code_ptr, uint64_t code_base,
                                uint64_t code_size)
{
    uc_vm_image *image = NULL;

    if (uc_vm_image_open(UC_ARCH_ARM64, UC_MODE_ARM, &image) != UC_ERR_OK) {
        return NULL;
    }

    uc_vm_image_set_page_size(image, 0x1000);
    uc_vm_image_set_tcg_buffer_size(image, 0x40000000u);
    uc_vm_image_set_tlb_mode(image, UC_TLB_CPU);
    uc_vm_image_set_pool_capacity(image, 8);

    // Optional: allow auto-map only inside this explicit guest->host window.
    uc_vm_image_set_host_ptr_auto_map(image, true);
    uc_vm_image_add_host_ptr_auto_range(image, code_base, code_size,
                                        UC_PROT_READ | UC_PROT_EXEC,
                                        (uint64_t)(uintptr_t)code_ptr);

    // Shared host-backed code/data region.
    if (uc_vm_image_map_ptr(image, code_base, code_size, UC_PROT_ALL,
                            code_ptr) != UC_ERR_OK) {
        uc_vm_image_close(image);
        return NULL;
    }

    // Optional: prewarm a known hot function entry.
    uc_vm_image_add_prewarm(image, code_base);

    return image;
}

static uc_engine *create_worker_ctx(uc_vm_image *image)
{
    uc_engine *uc = NULL;
    uc_hook hh;

    if (uc_open_from_image(image, &uc) != UC_ERR_OK) {
        return NULL;
    }

    // Hooks remain private to this context.
    if (uc_hook_add(uc, &hh, UC_HOOK_CODE_FAST, hook_code_fast, NULL,
                    1, 0) != UC_ERR_OK) {
        uc_close(uc);
        return NULL;
    }

    return uc;
}
```

## What Should Go Into The Shared Image

Good candidates:

- shared code pages
- shared read-only data pages
- host-backed tables that are identical for all worker contexts
- common hot function entry addresses for prewarm
- common control values such as page size, TCG buffer size and TLB mode

Usually keep these per-context:

- stacks
- per-thread scratch buffers
- mutable state that should not be shared between workers
- hooks and hook user-data
- register initialization

If a region must be private per worker, map it after `uc_open_from_image()`.

## Semantics And Limits

### Idle Context Pooling

If `uc_vm_image_set_pool_capacity(image, N)` is called with `N > 0`:

- `uc_open_from_image()` first tries to reuse an idle context from the image
- `uc_close()` may reset the context back to the image baseline and cache it
  instead of destroying it immediately
- recycled contexts keep their own internal TB/JIT cache, so hot paths stay
  warm across reuse

What is reset before a context becomes idle again:

- CPU/register state
- snapshotted memory state
- hook registrations
- exits state
- stop/quit/error runtime state

This means the next caller still receives a private `uc_engine`, but it is a
warm recycled one rather than a freshly created one.

### Hooks Are Still Per Context

`uc_vm_image` does not register hooks.

Register hooks after `uc_open_from_image()` on each returned context:

```c
uc_hook_add(uc, &hh, UC_HOOK_CODE_FAST, hook_code_fast, user_data, 1, 0);
```

That means:

- different worker contexts can install different hooks
- hook `user_data` stays private
- `uc_reg_read_fast()` and `uc_reg_write_fast()` keep their current behavior

### `uc_mem_map_ptr()` Really Reuses Host Backing

If you use `uc_vm_image_map_ptr()`, every derived context replays
`uc_mem_map_ptr()` with the same `guest_base`, `size`, `perms` and `ptr`.

This is the main V1 host-side reuse win.

### `uc_mem_map()` Is Only A Template

`uc_vm_image_map()` only stores the mapping template.

Each derived context still allocates its own Unicorn RAM for that region, just
like calling `uc_mem_map()` separately on each context.

### TLB/TB Are Not Globally Shared Across Live Contexts

This V2 still does not make these structures globally shared across all active
contexts at the same time:

- `TCGContext`
- TB cache
- TLB
- CPU state
- hook state

So this API is mainly about:

- reducing repeated initialization boilerplate
- reusing host-backed memory layout
- keeping context creation deterministic
- reusing pre-initialized warm contexts through an image-level idle pool

It is not yet a single global cross-context JIT cache.

### Prewarm Only Hits The Requested TB Entry

`uc_vm_image_add_prewarm(image, address)` uses `uc_ctl_request_cache()` when a
context is created.

It only prewarms the TB that starts at `address`.

It does not automatically prewarm:

- every block in the function
- every branch target
- every callee

## Threading Guidance

Treat `uc_vm_image` as immutable once worker threads start using it.

In practice:

- finish all `uc_vm_image_set_*`, `uc_vm_image_map*` and
  `uc_vm_image_add_prewarm()` calls first
- then create worker contexts from it
- avoid mutating the image while other threads are calling
  `uc_open_from_image()`
- if pooling is enabled, close all borrowed contexts before finally calling
  `uc_vm_image_close()`

## Why This V1 Exists

For a host-oriented ARM64 workload, the most annoying repeated cost is often:

- rebuilding the same mapping layout
- replaying the same `uc_mem_map_ptr()` calls
- recreating the same setup path for every worker

This V2 keeps that entry point, and adds a practical reuse path for
multi-threaded host workloads: warm contexts can be borrowed, reset and
returned without paying the full `uc_open + map + prewarm` cost every time.
