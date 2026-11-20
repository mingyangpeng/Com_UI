#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
watch_zcode.py —— 监督 zcode 会话 sess_cac7f9f7-... 是否还在干活。

判定依据（可靠度从高到低）：
  1. rollout/model-io-sess_<ID>.jsonl 的最后修改时间：会话每做一轮模型调用都会追加，
     活着干活时通常几秒~几十秒就有新写入；长时间不增长 = 停了/卡住了。
  2. artifacts/sess_<ID>/ 下最新文件的修改时间：工具调用结果落盘时间。
  3. ZCode 主进程是否还在（全局进程，只能证明 app 开着，不能证明这个会话在跑）。

状态输出：
  - 控制台打印一行结果
  - 追加到 state 日志（默认 ~/.zcode/cli/watch/<ID>.status.jsonl）
  - 连续 N 次判定停滞后，写一个显眼的 ALERT 文件（用户可见，用于告警）

用法：
  python tools/watch_zcode.py                 # 检测一次（默认阈值 20 分钟）
  python tools/watch_zcode.py --stale-min 20  # 自定义停滞阈值
  python tools/watch_zcode.py --alert-after 2 # 连续 2 次停滞才写 ALERT（避免误报）
  python tools/watch_zcode.py --quiet         # 只写文件，不打印
"""
import argparse
import datetime as dt
import json
import os
import sys

SID = "cac7f9f7-23be-47a7-8b19-ce21ac34b1bb"
ZCODE_CLI = os.path.join(os.path.expanduser("~"), ".zcode", "cli")
ROLLOUT = os.path.join(ZCODE_CLI, "rollout", f"model-io-sess_{SID}.jsonl")
ARTIFACTS = os.path.join(ZCODE_CLI, "artifacts", f"sess_{SID}")
WATCH_DIR = os.path.join(ZCODE_CLI, "watch")
STATE_LOG = os.path.join(WATCH_DIR, f"{SID}.status.jsonl")
ALERT_FILE = os.path.join(WATCH_DIR, f"ALERT_{SID}.txt")


def newest_mtime(path, recursive=False):
    """返回目录下（含自身）最新的文件修改时间；文件则返回其 mtime。"""
    best = 0.0
    if os.path.isfile(path):
        return os.path.getmtime(path)
    if not os.path.isdir(path):
        return 0.0
    for dp, _, fns in os.walk(path):
        for fn in fns:
            try:
                m = os.path.getmtime(os.path.join(dp, fn))
                best = max(best, m)
            except OSError:
                pass
    return best


def zcode_process_alive():
    """Windows: 用 tasklist 判断 ZCode.exe 是否存在（只作辅证）。"""
    try:
        import subprocess
        r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq ZCode.exe"],
                           capture_output=True, timeout=15)
        out = (r.stdout or b"").decode("utf-8", errors="replace")
        return "ZCode.exe" in out
    except Exception:
        return None


def load_last_state():
    """读状态日志最后一条，取连续停滞计数。"""
    if not os.path.isfile(STATE_LOG):
        return {"stale_streak": 0}
    try:
        with open(STATE_LOG, encoding="utf-8") as f:
            lines = [l for l in f if l.strip()]
        if lines:
            return json.loads(lines[-1])
    except Exception:
        pass
    return {"stale_streak": 0}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stale-min", type=int, default=20,
                    help="超过多少分钟无写入判定为停滞（默认 20）")
    ap.add_argument("--alert-after", type=int, default=2,
                    help="连续几次停滞才写 ALERT 文件（默认 2）")
    ap.add_argument("--quiet", action="store_true", help="不打印，只写文件")
    args = ap.parse_args()

    now = dt.datetime.now()
    rollout_m = newest_mtime(ROLLOUT)
    artifact_m = newest_mtime(ARTIFACTS)
    live_m = max(rollout_m, artifact_m)
    idle_min = (now.timestamp() - live_m) / 60 if live_m else None
    proc_alive = zcode_process_alive()

    stale = idle_min is not None and idle_min > args.stale_min
    prev = load_last_state()
    streak = prev.get("stale_streak", 0) + 1 if stale else 0

    rec = {
        "checked_at": now.isoformat(timespec="seconds"),
        "session": SID,
        "rollout_mtime": dt.datetime.fromtimestamp(rollout_m).isoformat(timespec="seconds") if rollout_m else None,
        "artifact_mtime": dt.datetime.fromtimestamp(artifact_m).isoformat(timespec="seconds") if artifact_m else None,
        "idle_minutes": round(idle_min, 1) if idle_min is not None else None,
        "zcode_process_alive": proc_alive,
        "status": "STALE" if stale else "ACTIVE",
        "stale_streak": streak,
        "threshold_min": args.stale_min,
    }

    os.makedirs(WATCH_DIR, exist_ok=True)
    with open(STATE_LOG, "a", encoding="utf-8") as f:
        f.write(json.dumps(rec, ensure_ascii=False) + "\n")

    alerted = False
    if stale and streak >= args.alert_after:
        with open(ALERT_FILE, "w", encoding="utf-8") as f:
            f.write(
                f"ALERT: zcode 会话疑似停滞\n"
                f"会话 ID : {SID}\n"
                f"检测时间: {rec['checked_at']}\n"
                f"最后活动: {rec['rollout_mtime']}\n"
                f"静止时长: {rec['idle_minutes']} 分钟（阈值 {args.stale_min}）\n"
                f"连续停滞: {streak} 次\n"
                f"ZCode 进程存活: {proc_alive}\n\n"
                f"请到 zcode 界面检查该会话是否卡住，必要时手动续跑。\n"
            )
        alerted = True
    elif not stale and os.path.exists(ALERT_FILE):
        os.remove(ALERT_FILE)  # 恢复活动，清除告警

    if not args.quiet:
        print(json.dumps(rec, ensure_ascii=False))
        if alerted:
            print(f"⚠ 已写告警文件：{ALERT_FILE}")
    return 0 if not stale else 3  # 3 = 检测到停滞（供定时任务判断）


if __name__ == "__main__":
    sys.exit(main())
