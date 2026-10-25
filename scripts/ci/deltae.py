# M0 ΔE 基线测量编排（架构文档 679 行风险条款）
# 用法：python scripts/ci/deltae.py [rw2 目录，默认 D:/LUMINA/build/rw2-real/library]
# 依赖：先构建 deltae_baseline 工具（cmake --build build/engine --target deltae_baseline）
# 输出：JSON 汇总 + 控制台结论；基线 mean|ΔE| > 6 时 exit 3（触发降级决策信号）
import io
import json
import subprocess
import sys

EXE = "D:/LUMINA/build/engine/deltae_baseline.exe"
DEFAULT_DIR = "D:/LUMINA/build/rw2-real/library"
THRESHOLD = 6.0  # 架构文档 679 行：基线 >6 → 提前决策降级口径


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DIR
    try:
        raw = subprocess.check_output([EXE, target], stderr=subprocess.DEVNULL)
    except FileNotFoundError:
        print("deltae_baseline.exe not built; run: cmake --build build/engine --target deltae_baseline")
        return 2
    start = raw.find(b"{")
    data = json.loads(raw[start:].decode("utf-8", errors="replace"))
    if "error" in data:
        print("no samples:", data["error"])
        return 1
    s = data["summary"]
    print("=== M0 dE baseline (engine current pipeline vs embedded camera JPEG, dE76) ===")
    print("pairs=%d mean=%.2f median=%.2f p95=%.2f" % (s["n"], s["meanDE"], s["medianDE"], s["p95DE"]))
    for p in data["pairs"]:
        print("  %-70s mean=%6.2f p95=%6.2f" % (p["file"], p["meanDE"], p["p95DE"]))
    if s["meanDE"] > THRESHOLD:
        print("BASELINE EXCEEDS %.1f -> escalate (degrade decision point per architecture doc line 679)" % THRESHOLD)
        return 3
    print("baseline within threshold %.1f" % THRESHOLD)
    return 0


if __name__ == "__main__":
    sys.exit(main())
