/*
 * hw2d_pie.S - ESP32-S3 PIE (Processor Instruction Extensions) 128-bit 算子
 *
 * 当前实现: 16bit 车道字节交换 (RGB565 LE<->BE), 8 像素/循环。
 * 算法: 32bit 元素内 out = ((q & 0xFF00FF00)>>8) | ((q & 0x00FF00FF)<<8)
 *       掩码保证移位不越 16bit 车道, 与标量参考实现逐位一致。
 * 约束: dst/src 均 16 字节对齐, nblk = 128bit 块数 (由 C 包装保证)。
 * 正确性由 hw2d_init() 开机自测背书: 与标量输出比对, 不一致自动回退。
 *
 * 说明: LUT 类算子 (滤镜查表) 需要 gather, PIE 无此能力, 不在此实现;
 * 双线性缩放为随机读, 同样不适合 PIE 向量化 (收益在顺序流算子)。
 */
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S3

    .section .rodata
    .align  16
pie_mask_hi:
    .word   0xFF00FF00, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00
pie_mask_lo:
    .word   0x00FF00FF, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF

    .text
    .align  4
    .global hw2d_pie_swap16_blk
    .type   hw2d_pie_swap16_blk, @function
/* void hw2d_pie_swap16_blk(void *dst a2, const void *src a3, uint32_t nblk a4) */
hw2d_pie_swap16_blk:
    entry   a1, 32
    movi    a5, pie_mask_hi
    ee.vld.128.ip   q1, a5, 16          /* q1 = 0xFF00FF00 x4 */
    ee.vld.128.ip   q2, a5, 16          /* q2 = 0x00FF00FF x4 */
    movi    a6, 8
    wsr.sar a6                          /* 移位量 8, vsl/vsr 共用 */
    loopnez a4, .Lswap_end
    ee.vld.128.ip   q0, a3, 16
    ee.andq q3, q0, q1
    ee.vsr.32       q3, q3              /* 高字节右移入低位 */
    ee.andq q4, q0, q2
    ee.vsl.32       q4, q4              /* 低字节左移入高位 */
    ee.orq  q3, q3, q4
    ee.vst.128.ip   q3, a2, 16
.Lswap_end:
    retw.n
    .size   hw2d_pie_swap16_blk, . - hw2d_pie_swap16_blk

#endif /* CONFIG_IDF_TARGET_ESP32S3 */
