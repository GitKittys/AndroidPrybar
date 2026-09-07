//
// Created by ASUS on 2026-03-16.
//

#include "SymbolTable.h"
#include "logging.h"

#include <elf.h>
#include <dlfcn.h>
#include <link.h>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

// ============================================================
// 纯磁盘 ELF 解析 — 只读 SHF_ALLOC + 在 PT_LOAD 内的 section
// ============================================================

// vaddr → 文件偏移（通过 PT_LOAD 段映射）
static off_t vaddr_to_offset(uint64_t vaddr, const Elf64_Phdr* phdrs, uint16_t phnum) {
    for (uint16_t i = 0; i < phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if (vaddr >= phdrs[i].p_vaddr &&
            vaddr < phdrs[i].p_vaddr + phdrs[i].p_filesz) {
            return (off_t)(vaddr - phdrs[i].p_vaddr + phdrs[i].p_offset);
        }
    }
    return -1;
}

// section 的 sh_addr 是否落在某个 PT_LOAD 段内
static bool section_in_pt_load(const Elf64_Shdr* sh, const Elf64_Phdr* phdrs, uint16_t phnum) {
    if (sh->sh_size == 0) return false;
    for (uint16_t i = 0; i < phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if (sh->sh_addr >= phdrs[i].p_vaddr &&
            sh->sh_addr + sh->sh_size <= phdrs[i].p_vaddr + phdrs[i].p_memsz) {
            return true;
        }
    }
    return false;
}

// 从磁盘 section headers 找 SHT_DYNSYM（SHF_ALLOC + 在 PT_LOAD 内），读符号
static bool load_dynsym_via_sections(int fd, const Elf64_Ehdr* ehdr,
                                     const Elf64_Shdr* shdrs, const Elf64_Phdr* phdrs,
                                     uint64_t load_bias, std::vector<SymEntry>& entries) {
    for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
        if (shdrs[i].sh_type != SHT_DYNSYM) continue;
        if (!(shdrs[i].sh_flags & SHF_ALLOC)) continue;
        if (!section_in_pt_load(&shdrs[i], phdrs, ehdr->e_phnum)) continue;
        if (shdrs[i].sh_link >= ehdr->e_shnum) continue;

        const Elf64_Shdr* strtab_sh = &shdrs[shdrs[i].sh_link];
        if (strtab_sh->sh_size == 0) continue;

        uint32_t nsyms = shdrs[i].sh_size / sizeof(Elf64_Sym);
        if (nsyms == 0 || nsyms > 500000) continue;

        auto* syms = (Elf64_Sym*)malloc(shdrs[i].sh_size);
        auto* strs = (char*)malloc(strtab_sh->sh_size);
        if (!syms || !strs) { free(syms); free(strs); return false; }

        if (pread(fd, syms, shdrs[i].sh_size, shdrs[i].sh_offset) != (ssize_t)shdrs[i].sh_size ||
            pread(fd, strs, strtab_sh->sh_size, strtab_sh->sh_offset) != (ssize_t)strtab_sh->sh_size) {
            free(syms); free(strs);
            return false;
        }

        entries.reserve(nsyms);
        for (uint32_t j = 0; j < nsyms; j++) {
            if (ELF64_ST_TYPE(syms[j].st_info) != STT_FUNC) continue;
            if (syms[j].st_value == 0) continue;
            if (syms[j].st_name >= strtab_sh->sh_size) continue;

            const char* name = &strs[syms[j].st_name];
            if (name[0] == '\0') continue;
            // 按 strtab 剩余长度截断：防 malformed strtab 内该名字无 null 终止 → 读越界成乱码
            size_t nlen = strnlen(name, strtab_sh->sh_size - syms[j].st_name);

            entries.push_back({syms[j].st_value + load_bias, (uint32_t)syms[j].st_size,
                               std::string(name, nlen)});
        }

        free(syms);
        free(strs);
        return true;
    }
    return false;
}

// fallback: 无 section headers 时，从 PT_DYNAMIC → DT_SYMTAB/DT_STRTAB/DT_STRSZ 读符号
static bool load_dynsym_via_dynamic(int fd, const Elf64_Phdr* phdrs, uint16_t phnum,
                                    uint64_t load_bias, std::vector<SymEntry>& entries) {
    const Elf64_Phdr* dyn_phdr = nullptr;
    for (uint16_t i = 0; i < phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) { dyn_phdr = &phdrs[i]; break; }
    }
    if (!dyn_phdr || dyn_phdr->p_filesz == 0) return false;

    auto* dynents = (Elf64_Dyn*)malloc(dyn_phdr->p_filesz);
    if (!dynents) return false;
    if (pread(fd, dynents, dyn_phdr->p_filesz, dyn_phdr->p_offset) != (ssize_t)dyn_phdr->p_filesz) {
        free(dynents); return false;
    }

    uint64_t symtab_vaddr = 0, strtab_vaddr = 0, strsz = 0;
    uint64_t hash_vaddr = 0;
    for (const Elf64_Dyn* d = dynents; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:   symtab_vaddr = d->d_un.d_ptr; break;
            case DT_STRTAB:   strtab_vaddr = d->d_un.d_ptr; break;
            case DT_STRSZ:    strsz = d->d_un.d_val; break;
            case DT_HASH:     hash_vaddr = d->d_un.d_ptr; break;
            default: break;
        }
    }
    free(dynents);

    if (!symtab_vaddr || !strtab_vaddr || !strsz) return false;

    off_t sym_off = vaddr_to_offset(symtab_vaddr, phdrs, phnum);
    off_t str_off = vaddr_to_offset(strtab_vaddr, phdrs, phnum);
    if (sym_off < 0 || str_off < 0) return false;

    // nsyms: 优先 DT_HASH（nchain），否则用 (strtab - symtab) 估算
    uint32_t nsyms = 0;
    if (hash_vaddr) {
        off_t h_off = vaddr_to_offset(hash_vaddr, phdrs, phnum);
        if (h_off >= 0) {
            Elf64_Word hdr[2];
            if (pread(fd, hdr, sizeof(hdr), h_off) == sizeof(hdr))
                nsyms = hdr[1];
        }
    }
    if (nsyms == 0 && strtab_vaddr > symtab_vaddr)
        nsyms = (strtab_vaddr - symtab_vaddr) / sizeof(Elf64_Sym);
    if (nsyms == 0 || nsyms > 500000) return false;

    size_t sym_size = (size_t)nsyms * sizeof(Elf64_Sym);
    auto* syms = (Elf64_Sym*)malloc(sym_size);
    auto* strs = (char*)malloc(strsz);
    if (!syms || !strs) { free(syms); free(strs); return false; }

    if (pread(fd, syms, sym_size, sym_off) != (ssize_t)sym_size ||
        pread(fd, strs, strsz, str_off) != (ssize_t)strsz) {
        free(syms); free(strs); return false;
    }

    entries.reserve(nsyms);
    for (uint32_t j = 0; j < nsyms; j++) {
        if (ELF64_ST_TYPE(syms[j].st_info) != STT_FUNC) continue;
        if (syms[j].st_value == 0) continue;
        if (syms[j].st_name >= strsz) continue;

        const char* name = &strs[syms[j].st_name];
        if (name[0] == '\0') continue;
        // 按 strtab 剩余长度截断：防 malformed strtab 内该名字无 null 终止 → 读越界成乱码
        size_t nlen = strnlen(name, strsz - syms[j].st_name);

        entries.push_back({syms[j].st_value + load_bias, (uint32_t)syms[j].st_size,
                           std::string(name, nlen)});
    }

    free(syms);
    free(strs);
    return !entries.empty();
}

// ============================================================
// SymbolTable::load — 全从磁盘读，不碰内存中的 .dynsym/.dynstr
// ============================================================

bool SymbolTable::load(uint64_t so_base) {
    entries.clear();
    loaded = true;

    // 校验 ELF 魔数：候选 base 首 4 字节必须是 \x7fELF
    if (memcmp((void*)so_base, ELFMAG, SELFMAG) != 0) {
        LOGD("SymbolTable::load: no ELF magic at %lx", so_base);
        return false;
    }

    Dl_info dl;
    if (!dladdr((void*)so_base, &dl) || !dl.dli_fname) {
        LOGD("SymbolTable::load: dladdr failed for %lx", so_base);
        return false;
    }

    int fd = open(dl.dli_fname, O_RDONLY);
    if (fd < 0) {
        LOGD("SymbolTable::load: open failed for %s (errno=%d)", dl.dli_fname, errno);
        return false;
    }

    Elf64_Ehdr ehdr;
    if (pread(fd, &ehdr, sizeof(ehdr), 0) != sizeof(ehdr) ||
        memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr.e_ident[EI_CLASS] != ELFCLASS64) {
        close(fd); return false;
    }

    // 读 program headers → 计算 load_bias + 校验 PT_LOAD 自洽
    if (ehdr.e_phnum == 0 || ehdr.e_phnum > 256) { close(fd); return false; }
    size_t ph_total = (size_t)ehdr.e_phnum * sizeof(Elf64_Phdr);
    auto* phdrs = (Elf64_Phdr*)malloc(ph_total);
    if (!phdrs) { close(fd); return false; }
    if (pread(fd, phdrs, ph_total, ehdr.e_phoff) != (ssize_t)ph_total) {
        free(phdrs); close(fd); return false;
    }

    // load_bias = runtime_base - 第一个 PT_LOAD 的 p_vaddr
    uint64_t load_bias = so_base;
    bool has_pt_load = false;
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            load_bias = so_base - phdrs[i].p_vaddr;
            has_pt_load = true;
            break;
        }
    }
    if (!has_pt_load) { free(phdrs); close(fd); return false; }

    // 优先走 section headers（精确的 offset/size）
    bool loaded_ok = false;
    if (ehdr.e_shnum > 0 && ehdr.e_shentsize >= sizeof(Elf64_Shdr)) {
        size_t sh_total = (size_t)ehdr.e_shnum * ehdr.e_shentsize;
        auto* shdrs = (Elf64_Shdr*)malloc(sh_total);
        if (shdrs && pread(fd, shdrs, sh_total, ehdr.e_shoff) == (ssize_t)sh_total) {
            loaded_ok = load_dynsym_via_sections(fd, &ehdr, shdrs, phdrs, load_bias, entries);
        }
        free(shdrs);
    }

    // fallback: 无 section headers，走 PT_DYNAMIC
    if (!loaded_ok) {
        load_dynsym_via_dynamic(fd, phdrs, ehdr.e_phnum, load_bias, entries);
    }

    free(phdrs);
    close(fd);

    if (entries.empty()) {
        LOGD("SymbolTable::load: no symbols in %s (base=%lx)", dl.dli_fname, so_base);
        return false;
    }

    std::sort(entries.begin(), entries.end(),
              [](const SymEntry& a, const SymEntry& b) { return a.addr < b.addr; });

    LOGD("SymbolTable::load %s base=%lx bias=%lx symbols=%zu",
         dl.dli_fname, so_base, load_bias, entries.size());
    return true;
}

// ============================================================
// 查找方法（不变）
// ============================================================

const char* SymbolTable::findExact(uint64_t addr) const {
    if (entries.empty()) return nullptr;

    auto it = std::lower_bound(entries.begin(), entries.end(), addr,
                               [](const SymEntry& e, uint64_t a) { return e.addr < a; });

    if (it != entries.end() && it->addr == addr) {
        return it->name.c_str();
    }
    return nullptr;
}

uint64_t SymbolTable::findAddrByName(const char* name) const {
    for (const auto& e : entries) {
        if (e.name == name) return e.addr;
    }
    return 0;
}

const SymEntry* SymbolTable::findContaining(uint64_t addr) const {
    if (entries.empty()) return nullptr;

    auto it = std::upper_bound(entries.begin(), entries.end(), addr,
                               [](uint64_t a, const SymEntry& e) { return a < e.addr; });

    if (it == entries.begin()) return nullptr;
    --it;

    if (it->size > 0) {
        if (addr < it->addr + it->size) {
            return &(*it);
        }
        return nullptr;
    }

    // size == 0: 用 4KB 阈值兜底
    if (addr - it->addr < 0x1000) {
        return &(*it);
    }
    return nullptr;
}


// ============================================================
// 全局符号缓存
// ============================================================

static std::unordered_map<uint64_t, SymbolTable> g_sym_cache;
static std::mutex g_sym_mutex;

static SymbolTable* getOrLoadTable(uint64_t addr) {
    Dl_info dl;
    if (!dladdr((void*)addr, &dl) || !dl.dli_fbase || !dl.dli_fname) return nullptr;

    uint64_t base = (uint64_t)dl.dli_fbase;

    // 校验 ELF 魔数：候选 base 首 4 字节必须是 \x7fELF
    if (memcmp((void*)base, ELFMAG, SELFMAG) != 0) return nullptr;

    std::lock_guard<std::mutex> lock(g_sym_mutex);
    auto it = g_sym_cache.find(base);
    if (it != g_sym_cache.end()) {
        return &it->second;
    }

    auto& table = g_sym_cache[base];
    table.load(base);
    return &table;
}

const SymEntry* lookupSymbol(uint64_t addr) {
    SymbolTable* table = getOrLoadTable(addr);
    if (!table) return nullptr;
    return table->findContaining(addr);
}

const char* lookupSymbolExact(uint64_t addr) {
    SymbolTable* table = getOrLoadTable(addr);
    if (!table) return nullptr;
    return table->findExact(addr);
}

SymbolTable* loadSymbolTableForSo(uint64_t so_base) {
    std::lock_guard<std::mutex> lock(g_sym_mutex);
    auto& table = g_sym_cache[so_base];
    if (!table.isLoaded()) {
        table.load(so_base);
    }
    return &table;
}

void clearSymbolCache() {
    std::lock_guard<std::mutex> lock(g_sym_mutex);
    g_sym_cache.clear();
}

uint64_t resolveSymbolInSo(const char* so_name, const char* sym_name) {
    struct FindCtx { const char* name; uint64_t base; };
    FindCtx ctx = {so_name, 0};
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
        auto* c = (FindCtx*)data;
        if (info->dlpi_name && strstr(info->dlpi_name, c->name)) {
            c->base = (uint64_t)info->dlpi_addr;
            return 1;
        }
        return 0;
    }, &ctx);
    if (!ctx.base) return 0;

    SymbolTable* table = loadSymbolTableForSo(ctx.base);
    if (!table || table->empty()) return 0;

    return table->findAddrByName(sym_name);
}
