#!/usr/bin/env python3
"""
unicornTrace tool — TCP receiver + LZ4 file decoder.

Usage:
  python trace_receiver.py receive [options]       # TCP receive from device
  python trace_receiver.py decode <input.lz4> [-o output.log]  # decode local .lz4 file

Examples:
  python trace_receiver.py receive                         # adb forward, port 9876
  python trace_receiver.py receive -p 12345 -o my.log      # custom port and output
  python trace_receiver.py receive --no-adb                # direct WiFi
  python trace_receiver.py decode trace_tid1234_0.lz4      # decode to .log
  python trace_receiver.py decode trace.lz4 --stdout       # decode to stdout
  adb pull /data/data/com.xxx/trace/ . && python trace_receiver.py decode *.lz4

Protocol:
  Each frame: [uint32 compressed_size][uint32 original_size][uint32 tid][compressed_data]
  End of data: EOF (file) or [0, 0, 0] (TCP)
  Compression: LZ4 block format
"""

import argparse
import glob
import os
import socket
import struct
import subprocess
import sys
import time

DEFAULT_PORT = 9876
FRAME_HEADER_SIZE = 12  # comp_size(4) + orig_size(4) + tid(4)
RECV_TIMEOUT = 30       # 设备无数据超时(秒), 防 force-stop 后永久阻塞


def lz4_decompress_block(src: bytes, original_size: int) -> bytes:
    dst = bytearray(original_size)
    si, di = 0, 0
    slen = len(src)

    while si < slen:
        token = src[si]; si += 1

        # literals
        lit_len = token >> 4
        if lit_len == 15:
            while si < slen:
                b = src[si]; si += 1
                lit_len += b
                if b != 255:
                    break
        end = di + lit_len
        dst[di:end] = src[si:si + lit_len]
        si += lit_len
        di = end

        if si >= slen:
            break

        # match offset
        offset = src[si] | (src[si + 1] << 8); si += 2
        if offset == 0:
            raise ValueError("LZ4: zero offset")

        # match length
        ml = (token & 0x0F) + 4
        if (token & 0x0F) == 15:
            while si < slen:
                b = src[si]; si += 1
                ml += b
                if b != 255:
                    break

        match_pos = di - offset
        for i in range(ml):
            dst[di] = dst[match_pos + i]
            di += 1

    return bytes(dst[:di])


def decode_frames(read_fn, label=""):
    """Decode LZ4 frames, yield (tid, decompressed_bytes) per frame.
    Truncated/corrupt final frame stops gracefully, preserving all good frames."""
    frame_count = 0

    while True:
        header = read_fn(FRAME_HEADER_SIZE)
        if len(header) == 0:
            break
        if len(header) < FRAME_HEADER_SIZE:
            sys.stderr.write(
                f"[!] {label}: truncated header "
                f"(got {len(header)}/{FRAME_HEADER_SIZE} bytes), stopping\n")
            break
        comp_size, orig_size, tid = struct.unpack("<III", header)
        if comp_size == 0:
            break

        compressed = read_fn(comp_size)
        if len(compressed) < comp_size:
            sys.stderr.write(
                f"[!] {label}: truncated final frame "
                f"(got {len(compressed)}/{comp_size} bytes), stopping\n")
            break
        try:
            decompressed = lz4_decompress_block(compressed, orig_size)
        except Exception as e:
            sys.stderr.write(
                f"[!] {label}: corrupt frame #{frame_count} ({e}), "
                f"stopping — kept {frame_count} good frames\n")
            break

        frame_count += 1
        yield tid, decompressed


def find_next_index(base, tid):
    """Find next available file index for given base and tid."""
    for idx in range(10000):
        path = f"{base}_tid{tid}_{idx}.log"
        if not os.path.exists(path):
            return idx
    return 0


# ── decode subcommand ──

def cmd_decode(args):
    inputs = []
    for pattern in args.input:
        expanded = glob.glob(pattern)
        if expanded:
            inputs.extend(expanded)
        else:
            inputs.append(pattern)

    if not inputs:
        print("[!] No input files", file=sys.stderr)
        sys.exit(1)

    for input_path in inputs:
        if not os.path.isfile(input_path):
            print(f"[!] Not found: {input_path}", file=sys.stderr)
            continue

        file_size = os.path.getsize(input_path)
        base, _ = os.path.splitext(input_path)

        # 流式解码：边解边写到临时文件，不缓存到内存（GB 级 trace 不会 OOM）
        # 先写 base_tid{N}.log，解完后如果只有单 tid 则 rename 为 base.log
        tid_files = {}    # tid -> file handle
        tid_paths = {}    # tid -> file path
        tid_sizes = {}    # tid -> bytes written
        total_orig = 0
        frame_count = 0
        bytes_read = 0

        try:
            with open(input_path, "rb") as f:
                def tracked_read(n):
                    nonlocal bytes_read
                    data = f.read(n)
                    bytes_read += len(data)
                    return data

                for tid, data in decode_frames(tracked_read, input_path):
                    if tid not in tid_files:
                        if args.stdout:
                            tid_files[tid] = sys.stdout.buffer
                            tid_paths[tid] = "<stdout>"
                        else:
                            path = f"{base}_tid{tid}.log"
                            tid_files[tid] = open(path, "wb")
                            tid_paths[tid] = path
                        tid_sizes[tid] = 0

                    tid_files[tid].write(data)
                    total_orig += len(data)
                    tid_sizes[tid] += len(data)
                    frame_count += 1

                    if file_size > 1024 * 1024 and not args.stdout:
                        pct = bytes_read * 100 // file_size if file_size > 0 else 0
                        print(f"\r[*] decoding {input_path}: {pct}%  "
                              f"{total_orig/(1024*1024):.1f}MB decoded  "
                              f"{frame_count} frames",
                              end="", flush=True)
        finally:
            for tid, f in tid_files.items():
                if f is not sys.stdout.buffer:
                    f.flush()
                    f.close()

        if file_size > 1024 * 1024 and not args.stdout:
            print()

        if args.stdout:
            pass
        elif len(tid_files) <= 1:
            out_path = args.output if (args.output and len(inputs) == 1) else base + ".log"
            for tid, src_path in tid_paths.items():
                if src_path != out_path:
                    os.replace(src_path, out_path)
            print(f"[+] {input_path} -> {out_path}  "
                  f"{total_orig/(1024*1024):.1f}MB  {frame_count} frames")
        else:
            for tid in sorted(tid_paths):
                print(f"[+] {input_path} tid={tid} -> {tid_paths[tid]}  "
                      f"{tid_sizes[tid]/(1024*1024):.1f}MB")


# ── receive subcommand ──

def setup_adb_forward(port: int) -> bool:
    try:
        subprocess.run(
            ["adb", "forward", f"tcp:{port}", f"tcp:{port}"],
            check=True, capture_output=True, timeout=10,
        )
        return True
    except (subprocess.CalledProcessError, FileNotFoundError, subprocess.TimeoutExpired) as e:
        print(f"[!] adb forward failed: {e}", file=sys.stderr)
        return False


def remove_adb_forward(port: int):
    try:
        subprocess.run(
            ["adb", "forward", "--remove", f"tcp:{port}"],
            capture_output=True, timeout=5,
        )
    except Exception:
        pass


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf += chunk
    return buf


def cmd_receive(args):
    adb_forwarded = False
    if not args.no_adb:
        print(f"[*] Setting up adb forward tcp:{args.port} -> tcp:{args.port}")
        if not setup_adb_forward(args.port):
            print("[!] Continuing without adb forward...", file=sys.stderr)
        else:
            adb_forwarded = True
            print("[+] adb forward OK")

    print(f"[*] Connecting to {args.host}:{args.port} ...")

    for attempt in range(30):
        try:
            sock = socket.create_connection((args.host, args.port), timeout=5)
            break
        except (ConnectionRefusedError, OSError):
            if attempt == 0:
                print("[*] Waiting for device trace to start...")
            time.sleep(1)
    else:
        print("[!] Could not connect after 30 attempts", file=sys.stderr)
        if adb_forwarded:
            remove_adb_forward(args.port)
        sys.exit(1)

    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.settimeout(RECV_TIMEOUT)
    print("[+] Connected!")

    out_base = args.output
    if out_base.endswith(".log"):
        out_base = out_base[:-4]

    tid_files = {}    # tid -> file
    tid_sizes = {}    # tid -> bytes written
    total_compressed = 0
    total_original = 0
    frame_count = 0
    t_start = time.monotonic()

    try:
        while True:
            header = recv_exact(sock, FRAME_HEADER_SIZE)
            comp_size, orig_size, tid = struct.unpack("<III", header)

            if comp_size == 0:
                print("\n[+] End frame received")
                break

            compressed = recv_exact(sock, comp_size)
            try:
                decompressed = lz4_decompress_block(compressed, orig_size)
            except Exception as e:
                sys.stderr.write(
                    f"\n[!] corrupt frame #{frame_count} ({e}), "
                    f"stopping — kept {frame_count} good frames\n")
                break

            if tid not in tid_files:
                if args.stdout:
                    tid_files[tid] = sys.stdout.buffer
                else:
                    idx = find_next_index(out_base, tid)
                    path = f"{out_base}_tid{tid}_{idx}.log"
                    tid_files[tid] = open(path, "wb")
                    print(f"[*] New thread tid={tid} -> {path}")
                tid_sizes[tid] = 0

            tid_files[tid].write(decompressed)
            tid_sizes[tid] += len(decompressed)

            total_compressed += comp_size
            total_original += len(decompressed)
            frame_count += 1

            elapsed = time.monotonic() - t_start
            if elapsed > 0:
                rate = total_original / (1024 * 1024 * elapsed)
                ratio = total_original / total_compressed if total_compressed > 0 else 0
                info = " ".join(f"t{t}:{s/(1024*1024):.1f}M"
                                for t, s in sorted(tid_sizes.items()))
                print(
                    f"\r[*] frames={frame_count}  "
                    f"total={total_original / (1024*1024):.1f}MB  "
                    f"ratio={ratio:.1f}x  "
                    f"speed={rate:.1f}MB/s  "
                    f"[{info}]",
                    end="", flush=True,
                )
    except socket.timeout:
        print(f"\n[!] No data for {RECV_TIMEOUT}s — device likely stopped/crashed")
    except ConnectionError:
        print("\n[!] Connection lost")
    except KeyboardInterrupt:
        print("\n[*] Interrupted by user")
    finally:
        sock.close()
        for tid, f in tid_files.items():
            if f is not sys.stdout.buffer:
                f.flush()
                f.close()
        if adb_forwarded:
            remove_adb_forward(args.port)

    elapsed = time.monotonic() - t_start
    print(f"\n[+] Done. {total_original / (1024*1024):.1f}MB in {elapsed:.1f}s  "
          f"{len(tid_files)} thread(s)")
    if total_compressed > 0:
        print(f"    Compression ratio: {total_original/total_compressed:.2f}x")
    for tid, size in sorted(tid_sizes.items()):
        print(f"    tid={tid}: {size/(1024*1024):.1f}MB")


# ── main ──

def main():
    parser = argparse.ArgumentParser(
        description="unicornTrace tool — TCP receiver + LZ4 decoder",
        usage="%(prog)s {receive,decode} [options]",
    )
    sub = parser.add_subparsers(dest="command")

    # receive
    p_recv = sub.add_parser("receive", help="receive TCP trace from device")
    p_recv.add_argument("-p", "--port", type=int, default=DEFAULT_PORT)
    p_recv.add_argument("-o", "--output", default="trace.log")
    p_recv.add_argument("--stdout", action="store_true")
    p_recv.add_argument("--no-adb", action="store_true")
    p_recv.add_argument("--host", default="127.0.0.1")

    # decode
    p_dec = sub.add_parser("decode", help="decode .lz4 trace file(s) to text")
    p_dec.add_argument("input", nargs="+", help=".lz4 file(s) or glob pattern")
    p_dec.add_argument("-o", "--output", default=None, help="output file (default: replace .lz4 with .log)")
    p_dec.add_argument("--stdout", action="store_true")

    args = parser.parse_args()

    if args.command == "receive":
        cmd_receive(args)
    elif args.command == "decode":
        cmd_decode(args)
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()
