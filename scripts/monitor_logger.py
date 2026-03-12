#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
持续后台抓取 ESP-IDF monitor 输出并记录到文件。
每次设备重启（检测到 Rebooting/ESP-ROM/rst:0x）时自动保存为新日志文件。
用法:
  cd /path/to/esp32-sms-forwarder
  source $IDF_PATH/export.sh   # 或已在该环境
  python3 scripts/monitor_logger.py -p /dev/ttyACM0
  nohup python3 scripts/monitor_logger.py -p /dev/ttyACM0 > /dev/null 2>&1 &   # 后台
"""

import argparse
import os
import pty
import re
import select
import subprocess
import sys
from datetime import datetime


# 检测到设备重启时开始新日志（匹配 monitor 输出中的典型重启行）
REBOOT_PATTERNS = [
    re.compile(r"Rebooting\s*\.\.\."),
    re.compile(r"^ESP-ROM:"),
    re.compile(r"rst:0x[0-9a-fA-F]+"),
]

# 去除 ANSI 转义（颜色、光标等）
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*[a-zA-Z]")


def strip_ansi(text: str) -> str:
    """去掉 ANSI 转义序列，并统一换行为 \\n，去掉行尾 \\r"""
    s = ANSI_ESCAPE.sub("", text)
    s = s.replace("\r\n", "\n").replace("\r", "\n")
    return s


def next_log_path(log_dir: str) -> str:
    """生成新日志文件路径：monitor_YYYYMMDD_HHMMSS.log"""
    os.makedirs(log_dir, exist_ok=True)
    name = f"monitor_{datetime.now().strftime('%Y%m%d_%H%M%S')}.log"
    return os.path.join(log_dir, name)


def is_reboot_line(line: str) -> bool:
    """判断当前行是否表示设备重启"""
    for pat in REBOOT_PATTERNS:
        if pat.search(line):
            return True
    return False


def main():
    parser = argparse.ArgumentParser(description="持续抓取 idf monitor 并按时序/重启分文件记录")
    parser.add_argument("-p", "--port", default="/dev/ttyACM0", help="串口，默认 /dev/ttyACM0")
    parser.add_argument(
        "-d", "--log-dir",
        default=None,
        help="日志目录，默认 <项目根>/monitor_logs",
    )
    parser.add_argument(
        "--idf-path",
        default=os.environ.get("IDF_PATH", "/root/esp/v5.5.3/esp-idf"),
        help="IDF 路径，默认环境变量 IDF_PATH 或 /root/esp/v5.5.3/esp-idf",
    )
    args = parser.parse_args()

    # 项目根：脚本在 scripts/ 下，上级即项目根
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_dir = os.path.dirname(script_dir)
    build_dir = os.path.join(project_dir, "build")
    elf_path = os.path.join(build_dir, "esp32-sms-forwarder.elf")
    if not os.path.isfile(elf_path):
        print(f"未找到 ELF: {elf_path}，请先 idf.py build", file=sys.stderr)
        sys.exit(1)

    log_dir = args.log_dir or os.path.join(project_dir, "monitor_logs")
    log_path = next_log_path(log_dir)
    print(f"[monitor_logger] 日志目录: {log_dir}", file=sys.stderr)
    print(f"[monitor_logger] 当前日志: {log_path}", file=sys.stderr)

    # 使用 PTY 启动 idf.py monitor，满足 “standard input must be TTY” 要求
    cmd = [
        "idf.py",
        "monitor",
        "-p", args.port,
    ]
    try:
        master_fd, slave_fd = pty.openpty()
    except OSError as e:
        print(f"无法创建 PTY: {e}", file=sys.stderr)
        sys.exit(1)

    def open_log():
        return open(log_path, "a", encoding="utf-8", errors="replace")

    try:
        proc = subprocess.Popen(
            cmd,
            cwd=project_dir,
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            env=os.environ.copy(),
        )
    except FileNotFoundError:
        os.close(master_fd)
        os.close(slave_fd)
        print("未找到 idf.py，请先执行: source $IDF_PATH/export.sh", file=sys.stderr)
        sys.exit(1)
    os.close(slave_fd)  # 子进程已继承，父进程不再使用

    current_log = open_log()
    buffer = b""
    last_was_blank = False  # 用于合并连续空行
    try:
        while True:
            r, _, _ = select.select([master_fd], [], [], 1.0)
            if not r:
                if proc.poll() is not None:
                    break
                continue
            try:
                chunk = os.read(master_fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            buffer += chunk
            while True:
                idx_n = buffer.find(b"\n")
                idx_r = buffer.find(b"\r")
                if idx_n == -1 and idx_r == -1:
                    break
                if idx_n >= 0 and (idx_r == -1 or idx_n <= idx_r):
                    pos = idx_n
                    sep = b"\n"
                else:
                    pos = idx_r
                    sep = b"\r"
                line_b = buffer[: pos + len(sep)]
                buffer = buffer[pos + len(sep) :]
                try:
                    raw_text = line_b.decode("utf-8", errors="replace")
                except Exception:
                    raw_text = line_b.decode("latin-1", errors="replace")
                text = strip_ansi(raw_text)
                is_blank = text.strip() == ""
                if is_blank and last_was_blank:
                    continue
                last_was_blank = is_blank
                current_log.write(text)
                current_log.flush()
                if is_reboot_line(text):
                    current_log.close()
                    log_path = next_log_path(log_dir)
                    print(f"[monitor_logger] 检测到重启，新日志: {log_path}", file=sys.stderr)
                    current_log = open_log()
                    current_log.write(text)
                    current_log.flush()
                    last_was_blank = False
        if buffer:
            try:
                raw_text = buffer.decode("utf-8", errors="replace")
            except Exception:
                raw_text = buffer.decode("latin-1", errors="replace")
            text = strip_ansi(raw_text)
            if not (last_was_blank and text.strip() == ""):
                current_log.write(text)
                current_log.flush()
    except KeyboardInterrupt:
        pass
    finally:
        current_log.close()
        try:
            os.close(master_fd)
        except OSError:
            pass
        try:
            proc.terminate()
            proc.wait(timeout=3)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
    print("[monitor_logger] 已退出", file=sys.stderr)


if __name__ == "__main__":
    main()
