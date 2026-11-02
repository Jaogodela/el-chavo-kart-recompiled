"""DEBUG_SAULO: compares the [PERF-SUMMARY] lines of one or more logs across Internal render settings.

Usage: python tools/debug_saulo_compare.py log1.log log2.log ...
Remove with the DEBUG_SAULO telemetry during final review.
"""
import re
import sys

PAIR = re.compile(r"(\w+)=([^\s]+)")


def sessions(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if "[DEBUG_SAULO][PERF-SUMMARY]" in line:
                yield path, dict(PAIR.findall(line.split("[DEBUG_SAULO][PERF-SUMMARY]", 1)[1]))


def main(paths):
    rows = [row for path in paths for row in sessions(path)]
    if not rows:
        print("no [PERF-SUMMARY] lines found")
        return 1
    print(f"{'selected':>9} {'internal':>8} {'render':>6} {'aspect':>6} {'fps':>7} {'game_ms':>8} {'render_ms':>9} {'sync_ms':>8} {'fence%':>7}")
    groups = {}
    for path, row in rows:
        key = (row.get("selected"), row.get("render"), row.get("aspect"))
        groups.setdefault(key, []).append(row)
        print(f"{row.get('selected', '?'):>9} {row.get('internal', '?'):>8} {row.get('render', '?'):>6} {row.get('aspect', '?'):>6} "
              f"{row.get('gameplay_fps', '?'):>7} {row.get('game_cpu_ms', '?'):>8} {row.get('render_cpu_ms', '?'):>9} "
              f"{row.get('render_sync_ms', '?'):>8} {row.get('cp_fence_pct', '?'):>7}")
    for key, items in groups.items():
        by_internal = {}
        for row in items:
            by_internal.setdefault(row.get("internal"), []).append(row)
        if len(by_internal) < 2:
            continue
        def fps(percent):
            values = [float(r["gameplay_fps"]) for r in by_internal[percent]]
            return sum(values) / len(values)
        def game(percent):
            values = [float(r["game_cpu_ms"]) for r in by_internal[percent]]
            return sum(values) / len(values)
        ordered = sorted(by_internal, key=lambda p: int(p.rstrip("%")))
        low, high = ordered[0], ordered[-1]
        gain = fps(low) / fps(high) if fps(high) else 0.0
        same_game = abs(game(low) - game(high)) <= 0.25 * max(game(high), 0.5)
        if gain >= 1.2 and same_game:
            verdict = "graphics-side evidence: lower internal render raised FPS while game CPU time stayed similar"
        elif gain < 1.05:
            verdict = "FPS barely changed: the bottleneck is likely elsewhere"
        else:
            verdict = "inconclusive"
        print(f"selected={key[0]} render={key[1]} aspect={key[2]}: internal {high} -> {low}: fps x{gain:.2f}; {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
