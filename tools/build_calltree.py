#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build_calltree.py — 从 unicornTrace 指令 trace 重建函数调用树/调用图。
脚本建树(非引擎内建): 部分/被打断的 trace 也能建, 解耦、可反复分析、零运行期开销。

支持两种 trace 格式:
  新(紧凑, 多线程): <so>+0x<off> 0x<PC>: <mnemonic> <ops> ...
  旧:               [time][<so> 0x<off>] [bytes] 0x<PC>: "<mnemonic> <ops>" ...

多线程: 新引擎一个 path → 目录 `<path去扩展名>/<so>+0x<入口>_tid<TID>_<idx>.log`(每线程一文件)。
  传【目录】→ 自动处理目录下所有 *.log/*.txt, 每个文件(=每线程)各出一棵树。
  传【单文件】→ 只处理该文件。

用法:
    python build_calltree.py <trace文件或目录> [so名] [入口偏移hex] [最大行数]
    例: python build_calltree.py mytrace/              # 目录, 自动每线程一棵树
        python build_calltree.py mytrace/xxx_tid123_0.log libfoo.so 1a2b4

输出(每个被处理文件旁): <base>_callgraph.txt + <base>_calltree.txt
"""
import re, sys, os, glob

# 新格式: libfoo.so+0x1a2b4 0x763118a2b4: stp d9, d8, ...
NEW = re.compile(r'([\w.\-]+)\+0x([0-9a-f]+) 0x[0-9a-f]+: ([a-z][a-z0-9.]*)')
# 旧格式: [..][libfoo.so 0x0001a2b4] [..] 0x..: "stp d9, d8, ..."
OLD = re.compile(r'([\w.\-]+) 0x([0-9a-f]+)\][^"]*"([a-z][a-z0-9.]*)')

def parse_file(path, so_filter, entry, maxn):
    edges = {}; total = 0; fcount = {}; stack = []; pending = None; entry_set = entry; n = 0
    detected_so = None
    with open(path, 'r', errors='ignore') as f:
        for line in f:
            m = NEW.search(line) or OLD.search(line)
            if not m: continue
            so = m.group(1); off = int(m.group(2), 16); mn = m.group(3)
            if so_filter and so != so_filter: continue
            if detected_so is None: detected_so = so
            if entry_set is None: entry_set = off
            if not stack: stack.append(entry_set)
            if pending is not None:
                d = edges.setdefault(pending, {})
                d[off] = d.get(off, 0) + 1
                fcount[off] = fcount.get(off, 0) + 1
                stack.append(off); total += 1; pending = None
            if mn in ('bl', 'blr'): pending = stack[-1] if stack else entry_set
            elif mn == 'ret':
                if len(stack) > 1: stack.pop()
            n += 1
            if n >= maxn: break
    return edges, total, fcount, (entry_set if entry_set is not None else 0), detected_so, n

def write_outputs(base, edges, total, fcount, entry, so):
    with open(base + '_callgraph.txt', 'w', encoding='utf-8') as o:
        o.write("// %s 调用图(入口 sub_%X)\n// sub_调用者 -> sub_被调(x次数)\n\n" % (so or '?', entry))
        for c in sorted(edges):
            items = sorted(edges[c].items())
            o.write('sub_%X -> %s\n' % (c, ' '.join('sub_%X(x%d)' % (o2, k) if k > 1 else 'sub_%X' % o2 for o2, k in items)))
    expanded = {}       # off -> 首次展开所在行号(便于回查)
    L = []              # 每个元素一行(不含 \n), 行号 = 索引+1, 精确
    # 图例 / 说明
    L += [
        "调用树  入口 sub_%X  (so=%s)" % (entry, so or '?'),
        "=" * 70,
        "读法: 每行一个函数 sub_<偏移(16进制)>; 缩进越深=被上一层调用; '->' = 调用。",
        "偏移可直接在 IDA/Ghidra 跳转(Jump to address)。",
        "",
        "标记说明:",
        "  (无标记)            该函数在此处首次出现, 其下方更深缩进就是它调用的函数(完整展开)。",
        "  [子调用见 L<行号>]   该函数前面已展开过一次; 为避免整棵子树到处重复, 此处不再重复。",
        "                      想看它调了啥, 跳到标注的行号(它首次展开的位置)。",
        "  [递归: 祖先]         该函数是当前这条调用链上方的某个祖先, 即它(间接)调用回了自己,",
        "                      形成递归循环; 再展开会无限循环, 故到此为止。",
        "  [...更深略]          超过最大深度, 省略更深层。",
        "=" * 70,
        "",
    ]
    def pr(off, depth, anc):
        pad = '    ' * depth; arrow = '-> ' if depth else ''
        ch = sorted(edges.get(off, {}))
        if off in anc:
            L.append('%s%ssub_%X    [递归: 祖先]' % (pad, arrow, off)); return
        if off in expanded:
            tag = ('    [子调用见 L%d]' % expanded[off]) if ch else ''
            L.append('%s%ssub_%X%s' % (pad, arrow, off, tag)); return
        expanded[off] = len(L) + 1          # 本函数即将成为第 len(L)+1 行(1-based)
        L.append('%s%ssub_%X' % (pad, arrow, off))
        if depth >= 60:
            if ch: L.append('%s    [...更深略]' % pad)
            return
        for o2 in ch: pr(o2, depth + 1, anc | {off})
    pr(entry, 0, set())
    with open(base + '_calltree.txt', 'w', encoding='utf-8') as o:
        o.write('\n'.join(L) + '\n')
        o.write("\n========== Summary ==========\n")
        o.write("Total function entries: %d  (%d unique)\n\nFunction call counts (top 50):\n" % (total, len(fcount)))
        for off, cnt in sorted(fcount.items(), key=lambda kv: -kv[1])[:50]:
            o.write("  sub_%-10X x%d\n" % (off, cnt))

def main():
    if len(sys.argv) < 2: print(__doc__); return
    target = sys.argv[1]
    so   = sys.argv[2] if len(sys.argv) > 2 else None
    entry= int(sys.argv[3], 16) if len(sys.argv) > 3 else None
    maxn = int(sys.argv[4]) if len(sys.argv) > 4 else 50_000_000

    if os.path.isdir(target):
        files = sorted(glob.glob(os.path.join(target, '*.log')) + glob.glob(os.path.join(target, '*.txt')))
        files = [f for f in files if not f.endswith(('_callgraph.txt', '_calltree.txt'))]
        if not files: print("目录里没有 .log/.txt 文件"); return
        sys.stderr.write("目录模式: %d 个文件(每线程一棵树)\n" % len(files))
    else:
        files = [target]

    for path in files:
        edges, total, fcount, ent, dso, n = parse_file(path, so, entry, maxn)
        if total == 0:
            sys.stderr.write("  %s: 0 调用(格式不符或空), 跳过\n" % os.path.basename(path)); continue
        base = os.path.splitext(path)[0]
        write_outputs(base, edges, total, fcount, ent, so or dso)
        sys.stderr.write("  %s: %d行/%d调用/%d唯一 → %s_calltree.txt\n" % (os.path.basename(path), n, total, len(fcount), os.path.basename(base)))

if __name__ == '__main__':
    main()
