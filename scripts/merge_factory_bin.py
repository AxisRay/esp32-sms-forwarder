#!/usr/bin/env python3
# 将 build/flash_args 中的各分区合并为单个可刷写固件，供 factory 包使用。
# 用法: python3 merge_factory_bin.py <build_dir> <output_bin> [idf_path]
# 若未传 idf_path，则从环境变量 IDF_PATH 读取。

import json
import os
import subprocess
import sys


def main():
    if len(sys.argv) < 3:
        print("用法: merge_factory_bin.py <build_dir> <output_bin> [idf_path]", file=sys.stderr)
        sys.exit(1)
    build_dir = os.path.abspath(sys.argv[1])
    output_bin = os.path.abspath(sys.argv[2])
    idf_path = (sys.argv[3] if len(sys.argv) > 3 else "").strip() or os.environ.get("IDF_PATH", "")
    if not idf_path:
        print("错误: 未设置 IDF_PATH 且未传入 idf_path", file=sys.stderr)
        sys.exit(1)
    idf_path = os.path.abspath(idf_path)

    flash_args_file = os.path.join(build_dir, "flash_args")
    if not os.path.isfile(flash_args_file):
        print(f"错误: 未找到 {flash_args_file}", file=sys.stderr)
        sys.exit(1)

    # 从 sdkconfig.json 读取芯片型号
    sdkconfig_json = os.path.join(build_dir, "config", "sdkconfig.json")
    chip = "esp32c3"
    if os.path.isfile(sdkconfig_json):
        try:
            with open(sdkconfig_json, "r", encoding="utf-8") as f:
                cfg = json.load(f)
                chip = cfg.get("IDF_TARGET", chip)
        except Exception:
            pass

    # ESP-IDF 中 esptool 位于 components/esptool_py/esptool/esptool.py
    esptool_py = os.path.join(idf_path, "components", "esptool_py", "esptool", "esptool.py")
    if not os.path.isfile(esptool_py):
        # 兼容旧版：部分版本可能在 esptool_py 根目录
        esptool_py = os.path.join(idf_path, "components", "esptool_py", "esptool.py")
    if not os.path.isfile(esptool_py):
        print(f"错误: 未找到 esptool（已尝试 esptool_py/esptool/esptool.py 与 esptool_py/esptool.py）", file=sys.stderr)
        sys.exit(1)

    # 使用 @flash_args：esptool 从该文件读取各分区偏移与路径（路径相对 build_dir），故在 build_dir 下执行
    cmd = [
        sys.executable,
        esptool_py,
        "--chip", chip,
        "merge_bin",
        "-o", output_bin,
        f"@{os.path.normpath(flash_args_file)}",
    ]
    ret = subprocess.run(cmd, cwd=build_dir)
    if ret.returncode != 0:
        sys.exit(ret.returncode)
    print(f"已生成合并固件: {output_bin}")


if __name__ == "__main__":
    main()
