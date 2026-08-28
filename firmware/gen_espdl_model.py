# PlatformIO pre 脚本: 补跑 esp-dl 模型嵌入的两步生成
#
# 背景: human_face_detect 组件用 CMake add_custom_command 在构建目录生成
#   1) espdl_models/human_face_detect.espdl   (pack_espdl_models.py 打包)
#   2) human_face_detect.espdl.S              (incbin 汇编包装)
# PlatformIO 的 SCons 层不执行 CMake 自定义命令, 会报 "Source ... not found"。
# 本脚本在每次构建前手动补生成 (幂等; 产物已存在则跳过)。
# 注意: 全新 checkout 首次构建时 managed_components 尚未下载, 本脚本会跳过,
# 构建报缺文件后再跑一次 `pio run` 即可 (脚本会打印提示)。
import os
import subprocess
import sys

Import("env")  # noqa: F821

BUILD_DIR = env.subst("$BUILD_DIR")  # noqa: F821
PROJ = env.subst("$PROJECT_DIR")  # noqa: F821
MC = os.path.join(PROJ, "managed_components")
ESPDL = os.path.join(MC, "espressif__esp-dl")
FACE = os.path.join(MC, "espressif__human_face_detect")

MODELS = [
    os.path.join(FACE, "models", "s3", "human_face_detect_msr_s8_v1.espdl"),
    os.path.join(FACE, "models", "s3", "human_face_detect_mnp_s8_v1.espdl"),
]
PACKED = os.path.join(BUILD_DIR, "espdl_models", "human_face_detect.espdl")
SFILE = os.path.join(BUILD_DIR, "human_face_detect.espdl.S")


def generate():
    if not os.path.isdir(FACE) or not os.path.isdir(ESPDL):
        print("[gen_espdl] managed_components 未就绪, 跳过 (首次构建失败后重跑一次)")
        return
    if not all(os.path.isfile(m) for m in MODELS):
        print("[gen_espdl] 模型文件缺失, 跳过")
        return

    if not os.path.isfile(PACKED) or any(
            os.path.getmtime(m) > os.path.getmtime(PACKED) for m in MODELS):
        os.makedirs(os.path.dirname(PACKED), exist_ok=True)
        pack = os.path.join(ESPDL, "fbs_loader", "pack_espdl_models.py")
        subprocess.check_call(
            [sys.executable, pack, "--model_path", *MODELS, "--out_file", PACKED])
        print(f"[gen_espdl] packed -> {PACKED}")

    if not os.path.isfile(SFILE) or os.path.getmtime(PACKED) > os.path.getmtime(SFILE):
        cmake = os.path.join(
            env.PioPlatform().get_package_dir("tool-cmake") or "",  # noqa: F821
            "bin", "cmake")
        if not os.path.isfile(cmake):
            cmake = "cmake"
        script = os.path.join(ESPDL, "fbs_loader", "cmake",
                              "data_file_embed_asm_aligned.cmake")
        subprocess.check_call(
            [cmake, "-D", f"DATA_FILE={PACKED}", "-D", f"SOURCE_FILE={SFILE}",
             "-D", "FILE_TYPE=BINARY", "-P", script],
            cwd=BUILD_DIR)
        print(f"[gen_espdl] embedded -> {SFILE}")


generate()
