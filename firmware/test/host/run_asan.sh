#!/bin/sh
# host 端 sanitizer 算法测试 (预览/拍照链 + hw2d 单测 + SOF 模糊)
# 用法: 在 firmware/ 目录执行 test/host/run_asan.sh [asan]
# 默认 UBSAN + 测试自带哨兵红区; 传 "asan" 追加 AddressSanitizer
# (注意: 部分 macOS 环境 Apple clang ASAN runtime 会卡死在 dyld 初始化 ——
#  hello-world 级程序即复现, 与本代码无关; 该环境下用默认模式,
#  越界主要靠 UBSAN+红区+QEMU 堆毒化三层兜底)
set -e
export MallocNanoZone=0
SAN="undefined"
[ "$1" = "asan" ] && SAN="address,undefined"
OUT=${TMPDIR:-/tmp}/pvc_host_test
CFLAGS="-O1 -g -Wall -fsanitize=$SAN -fno-sanitize-recover=all -fno-omit-frame-pointer -I main"
cc $CFLAGS -o "$OUT-hw2d" test/host/test_hw2d.c main/hw2d.c
cc $CFLAGS -o "$OUT-pipe" test/host/test_pipeline.c main/hw2d.c main/pvc_jpeg_dims.c
"$OUT-hw2d"
"$OUT-pipe"
echo "sanitizer($SAN): ALL PASS"
