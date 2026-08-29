#!/bin/sh
# host 端 sanitizer 算法测试 (预览/拍照链 + hw2d 单测 + SOF 模糊)
# 用法: 在 firmware/ 目录执行 test/host/run_asan.sh [asan]
# 默认 UBSAN + 测试自带哨兵红区; 传 "asan" 追加 AddressSanitizer
# (注意: 部分 macOS 环境 Apple clang ASAN runtime 会卡死在 dyld 初始化 ——
#  hello-world 级程序即复现, 与本代码无关。该环境下两条真 ASAN 通道:
#  1) CI 的 host-sanitizer job (ubuntu);
#  2) 本机 Linux 容器 (已验证, 本地已有镜像):
#     docker run --rm --platform linux/amd64 -v "$PWD":/fw -w /fw \
#       -e CC=gcc bekencorp/armino-idk:1.2 bash -c "./test/host/run_asan.sh asan"
#     容器内 "Unable to get registers" 为 Rosetta 下 ASAN 无害告警)
set -e
export MallocNanoZone=0
SAN="undefined"
[ "$1" = "asan" ] && SAN="address,undefined"
[ "$1" = "tsan" ] && SAN="thread"
OUT=${TMPDIR:-/tmp}/pvc_host_test
CFLAGS="-O1 -g -Wall -fsanitize=$SAN -fno-sanitize-recover=all -fno-omit-frame-pointer -I main ${CFLAGS_EXTRA:-}"
if [ "$1" = "tsan" ]; then
  # TSAN: 并发契约压测 (预览/worker 双线程各持 LUT 上下文)
  "${CC:-cc}" $CFLAGS -o "$OUT-thr" test/host/test_threads.c main/hw2d.c -lpthread
  "$OUT-thr"
else
  "${CC:-cc}" $CFLAGS -o "$OUT-hw2d" test/host/test_hw2d.c main/hw2d.c
  "${CC:-cc}" $CFLAGS -o "$OUT-pipe" test/host/test_pipeline.c main/hw2d.c main/pvc_jpeg_dims.c main/net/pvc_ota_util.c
  "$OUT-hw2d"
  "$OUT-pipe"
fi
echo "sanitizer($SAN): ALL PASS"
