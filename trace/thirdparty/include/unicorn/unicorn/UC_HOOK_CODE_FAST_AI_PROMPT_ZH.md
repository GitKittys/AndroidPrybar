# UC_HOOK_CODE_FAST AI Prompt

## 超短版
```text
这是一个 Unicorn fork，不是上游原版。它新增了 AArch64-only 的 UC_HOOK_CODE_FAST：每条指令执行前同步回调一次，回调签名和 hook_code 类似：
void (*uc_cb_hookcode_fast_t)(uc_engine *uc, uint64_t address, uint32_t size, void *user_data)
另有 uc_reg_read_fast(uc, regid, value)、uc_reg_read_fast_batch(...) 和 uc_reg_write_fast(...)，只允许在 UC_HOOK_CODE_FAST 回调里使用，用来按需读取或修改当前这条指令执行前的 live CPU 状态。在 hook_block、普通 hook_code、mem hook 或回调结束后调用会返回 UC_ERR_ARG。这个 fork 允许注册多个 UC_HOOK_CODE_FAST。要读写寄存器时优先用这些 fast API，不要假设这是上游 Unicorn 自带功能。
```

## 实战版
```text
请按这个前提理解当前项目：这是 Unicorn 的私有 fork，扩展了 ARM64 的 UC_HOOK_CODE_FAST。它不是批量 trace，而是每条指令执行前同步回调，回调签名是：
void (*uc_cb_hookcode_fast_t)(uc_engine *uc, uint64_t address, uint32_t size, void *user_data)

如果需要当前指令对应的寄存器，不再使用额外的 context record，而是直接在回调里调用：
uc_reg_read_fast(uc, regid, value)
uc_reg_read_fast_batch(uc, regs, vals, count)
uc_reg_write_fast(uc, regid, value)

语义：
- 这是执行前 hook，和原版 hook_code 一致
- uc_reg_read_fast() 读取的是当前这条指令执行前的 live CPU 状态
- uc_reg_read_fast_batch() 适合一次读取多个寄存器
- uc_reg_write_fast() 会直接修改当前 live CPU 状态；如果写 PC，会让当前 TB 退出后从新 PC 继续
- 如果在回调里改了寄存器，再次调用 uc_reg_read_fast() 会读到修改后的新值
- uc_mem_read() 在这个回调里读到的也是当前指令执行前的内存
- 支持像 hook_code 一样注册多个 UC_HOOK_CODE_FAST

限制：
- 仅 ARM64
- uc_reg_read_fast() 仅在 UC_HOOK_CODE_FAST 回调里有效
- 不要假设上游 Unicorn 自带这个 API
```

## 一句话版
```text
这个项目魔改了 Unicorn：新增 ARM64 的 UC_HOOK_CODE_FAST，逐条指令执行前同步回调，签名和 hook_code 一样；如果要读当前寄存器，直接在回调里用 uc_reg_read_fast() 按需读取。
```
