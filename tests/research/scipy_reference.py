"""Independent reference values for the research mathematics tests.

Generates tests/fixtures/research/scipy_reference_values.h from seeded
synthetic data with numpy and scipy, which share no code with the C++
implementation in research/:

  Welford mean / variance   numpy.mean, numpy.var (two-pass)
  RM-ANOVA (two arms)       scipy.stats.ttest_rel (F = t^2), scipy.stats.f.sf,
                            partial eta^2 = t^2 / (t^2 + df), CI from scipy.stats.t.ppf
  F survival                scipy.stats.f.sf
  Wilson interval           scipy.stats.binomtest(...).proportion_ci(method="wilson")
  Holm                      independent step-down written here

Usage (from the repository root):
  python -I tests/research/scipy_reference.py           # rewrite the header
  python -I tests/research/scipy_reference.py --check   # exit 1 if it is stale
"""

import argparse
import math
import pathlib
import sys

import numpy as np
import scipy
from scipy import stats

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "tests" / "fixtures" / "research" / "scipy_reference_values.h"

FRAMES, BLOCKS, ECHOES, MIN_REPLIES = 10, 5, 8, 3


def lit(x):
    if x is None:
        return "std::nullopt"
    if isinstance(x, (bool, np.bool_)):
        return "true" if x else "false"
    if isinstance(x, (int, np.integer)):
        return str(int(x))
    x = float(x)
    if math.isinf(x):
        return "std::numeric_limits<double>::infinity()"
    return repr(x)


def array(values, per_line=6):
    items = [lit(v) for v in values]
    lines = [", ".join(items[i:i + per_line]) for i in range(0, len(items), per_line)]
    return "{\n    " + ",\n    ".join(lines) + "}"


def holm(p):
    m = len(p)
    order = sorted(range(m), key=lambda i: (p[i], i))
    adjusted = [1.0] * m
    running = 0.0
    for rank, i in enumerate(order):
        running = max(running, min(1.0, (m - rank) * p[i]))
        adjusted[i] = running
    return adjusted


# ---- Tournament generation ----

def make_arm(rng, base_ms, drift, sigma, loss_per_block):
    """echoes[frame][block][echo]: RTT in ms, or None when lost."""
    frames = []
    for f in range(FRAMES):
        blocks = []
        for _ in range(BLOCKS):
            rtts = [base_ms * drift[f] * float(np.exp(rng.normal(0.0, sigma))) for _ in range(ECHOES)]
            lost = loss_per_block(rng)
            for i in rng.choice(ECHOES, size=lost, replace=False):
                rtts[int(i)] = None
            blocks.append(rtts)
        frames.append(blocks)
    return frames


def frame_values(arm):
    values = []
    for blocks in arm:
        means = []
        for echoes in blocks:
            x = [math.log10(r) for r in echoes if r is not None]
            if len(x) < MIN_REPLIES:
                return None
            means.append(float(np.mean(x)))
        values.append(float(np.mean(means)))
    return values


def anova_reference(inc, chal, level=0.95):
    t, p = stats.ttest_rel(chal, inc)
    df = len(inc) - 1
    d = np.asarray(chal) - np.asarray(inc)
    mean_d = float(np.mean(d))
    half = float(stats.t.ppf(0.5 + level / 2.0, df)) * float(np.std(d, ddof=1)) / math.sqrt(len(d))
    f = float(t) ** 2
    return {
        "f": f,
        "p": float(p),
        "p_from_f": float(stats.f.sf(f, 1, df)),
        "eta": f / (f + df),
        "mean_d": mean_d,
        "ci_low": mean_d - half,
        "ci_high": mean_d + half,
    }


def wilson(k, n, level=0.95):
    ci = stats.binomtest(k, n).proportion_ci(confidence_level=level, method="wilson")
    return float(ci.low), float(ci.high)


def arm_stats(arm):
    returned = [r for blocks in arm for echoes in blocks for r in echoes if r is not None]
    sent = FRAMES * BLOCKS * ECHOES
    return sent, sent - len(returned), (float(np.mean(returned)) if returned else None)


def feasibility(sent, lost, mean_rtt):
    low, high = wilson(lost, sent)
    rtt_over = mean_rtt is None or mean_rtt > 150.0
    return low > 0.03 or rtt_over, high <= 0.03 and not rtt_over


def confidence(eta, ci_low, ci_high, inc_feas, chal_feas, dg):
    if chal_feas[0]:
        s_ci = -1.0
    elif inc_feas[0] and chal_feas[1]:
        s_ci = 1.0
    elif ci_high < 0:
        s_ci = min(1.0, -ci_high / 0.05)
    elif ci_low > 0:
        s_ci = -min(1.0, ci_low / 0.05)
    else:
        s_ci = 0.0
    return 0.40 * eta + 0.35 * s_ci + 0.25 * max(-1.0, min(1.0, dg))


def emit_arm(name, arm):
    out = [f"inline const std::vector<std::vector<std::vector<std::optional<double>>>> {name} = {{"]
    for blocks in arm:
        out.append("  {")
        for echoes in blocks:
            out.append("    {" + ", ".join(lit(r) for r in echoes) + "},")
        out.append("  },")
    out.append("};")
    return "\n".join(out)


def build():
    rng = np.random.default_rng(20261119)
    parts = []

    # ---- Welford ----
    offset = 1e9 + rng.normal(0.0, 1.0, 1000)
    lognormal = rng.lognormal(math.log(20.0), 0.4, 500)
    for name, data in (("kWelfordOffset", offset), ("kWelfordLognormal", lognormal)):
        parts.append(f"inline const std::vector<double> {name} = {array(data)};")
        parts.append(f"inline constexpr double {name}Mean = {lit(np.mean(data))};")
        parts.append(f"inline constexpr double {name}Var0 = {lit(np.var(data, ddof=0))};")
        parts.append(f"inline constexpr double {name}Var1 = {lit(np.var(data, ddof=1))};")

    # ---- F survival ----
    f_cases = [(0.5, 1, 9), (4.0, 1, 9), (25.0, 1, 9), (20009.9, 1, 9), (3.2, 2, 18), (1.0, 5, 40)]
    parts.append("struct FCase { double f, d1, d2, sf; };")
    parts.append("inline const std::vector<FCase> kFSurvival = {\n" + ",\n".join(
        f"    {{{lit(f)}, {lit(d1)}, {lit(d2)}, {lit(stats.f.sf(f, d1, d2))}}}" for f, d1, d2 in f_cases) + "};")

    # ---- Wilson ----
    w_cases = [(0, 400), (5, 400), (12, 400), (13, 400), (106, 400), (119, 400), (250, 400), (400, 400), (1, 8)]
    parts.append("struct WilsonCase { std::size_t k, n; double low, high; };")
    parts.append("inline const std::vector<WilsonCase> kWilson = {\n" + ",\n".join(
        f"    {{{k}, {n}, {lit(wilson(k, n)[0])}, {lit(wilson(k, n)[1])}}}" for k, n in w_cases) + "};")

    # ---- Holm ----
    h_cases = [[0.03], [0.01, 0.04, 0.03, 0.005], [0.02, 0.02, 0.02], [0.5, 0.6], [0.04, 0.001], [0.0, 1.0]]
    parts.append("struct HolmCase { std::vector<double> p, adjusted; };")
    parts.append("inline const std::vector<HolmCase> kHolm = {\n" + ",\n".join(
        f"    {{{array(p)}, {array(holm(p))}}}" for p in h_cases) + "};")

    # ---- Tournaments ----
    drift = [float(np.exp(rng.normal(0.0, 0.08))) for _ in range(FRAMES)]
    no_loss = lambda r: 0
    some_loss = lambda r: int(r.integers(0, 4))  # 0..3 lost: every block keeps >= 5 replies.

    t1_inc = make_arm(rng, 20.0, drift, 0.05, no_loss)
    t1_chal = make_arm(rng, 8.0, drift, 0.05, no_loss)
    t2_inc = make_arm(rng, 15.0, drift, 0.08, some_loss)
    t2_fast = make_arm(rng, 12.0, drift, 0.08, some_loss)
    t2_same = make_arm(rng, 15.0, drift, 0.08, some_loss)
    t3_inc = make_arm(rng, 15.0, drift, 0.05, no_loss)
    t3_chal = make_arm(rng, 10.0, drift, 0.05, no_loss)
    t3_inc[4][2] = [None, None, None, None, None, None, t3_inc[4][2][6], t3_inc[4][2][7]]  # 2 replies: invalid block.
    # Repair: a fast but lossy incumbent against a slower loss-free challenger.
    t4_inc = make_arm(rng, 9.0, drift, 0.05, some_loss)
    t4_chal = make_arm(rng, 20.0, drift, 0.05, no_loss)

    for name, arm in (("kT1Incumbent", t1_inc), ("kT1Challenger", t1_chal), ("kT2Incumbent", t2_inc),
                      ("kT2Fast", t2_fast), ("kT2Same", t2_same), ("kT3Incumbent", t3_inc),
                      ("kT3Challenger", t3_chal), ("kT4Incumbent", t4_inc), ("kT4Challenger", t4_chal)):
        parts.append(emit_arm(name, arm))
        fv = frame_values(arm)
        if fv is not None:
            parts.append(f"inline const std::vector<double> {name}FrameValues = {array(fv)};")
        sent, lost, mean_rtt = arm_stats(arm)
        parts.append(f"inline constexpr std::size_t {name}Lost = {lost};")
        parts.append(f"inline constexpr double {name}MeanRtt = {lit(mean_rtt)};")
    # Micro-variance of frame 0, block 0 and variance of frame 0 block means.
    b0 = [math.log10(r) for r in t2_inc[0][0] if r is not None]
    means0 = [float(np.mean([math.log10(r) for r in e if r is not None])) for e in t2_inc[0]]
    parts.append(f"inline constexpr double kT2IncumbentBlock00Variance = {lit(np.var(b0, ddof=1))};")
    parts.append(f"inline constexpr double kT2IncumbentFrame0BlockMeanVariance = {lit(np.var(means0, ddof=1))};")

    parts.append("struct AnovaCase { double f, p, p_from_f, eta, mean_d, ci_low, ci_high; };")
    comparisons = {
        "kT1Anova": (t1_inc, t1_chal),
        "kT2FastAnova": (t2_inc, t2_fast),
        "kT2SameAnova": (t2_inc, t2_same),
        "kT4Anova": (t4_inc, t4_chal),
    }
    refs = {}
    for name, (inc, chal) in comparisons.items():
        r = anova_reference(frame_values(inc), frame_values(chal))
        refs[name] = r
        parts.append(f"inline constexpr AnovaCase {name} = {{{lit(r['f'])}, {lit(r['p'])}, {lit(r['p_from_f'])}, "
                     f"{lit(r['eta'])}, {lit(r['mean_d'])}, {lit(r['ci_low'])}, {lit(r['ci_high'])}}};")
    holm_t2 = holm([refs["kT2FastAnova"]["p"], refs["kT2SameAnova"]["p"]])
    parts.append(f"inline constexpr double kT2FastPHolm = {lit(holm_t2[0])};")
    parts.append(f"inline constexpr double kT2SamePHolm = {lit(holm_t2[1])};")

    # Confidence with stated goodness differences (inputs, not measurements).
    dg = {"kT1": 0.5, "kT2Fast": 0.3, "kT2Same": 0.0, "kT4": 2.0}
    for key, value in dg.items():
        parts.append(f"inline constexpr double {key}DeltaGoodness = {lit(value)};")
    for name, (inc, chal), key in (("kT1Confidence", comparisons["kT1Anova"], "kT1Anova"),
                                   ("kT2FastConfidence", comparisons["kT2FastAnova"], "kT2FastAnova"),
                                   ("kT2SameConfidence", comparisons["kT2SameAnova"], "kT2SameAnova"),
                                   ("kT4Confidence", comparisons["kT4Anova"], "kT4Anova")):
        r = refs[key]
        c = confidence(r["eta"], r["ci_low"], r["ci_high"], feasibility(*arm_stats(inc)), feasibility(*arm_stats(chal)),
                       dg[name.replace("Confidence", "")])
        parts.append(f"inline constexpr double {name} = {lit(c)};")

    header = [
        "#pragma once",
        "",
        "// GENERATED by tests/research/scipy_reference.py -- do not edit by hand.",
        f"// numpy {np.__version__}, scipy {scipy.__version__}. Independent reference values",
        "// for research_math_test.cpp; see the generator for the method of each value.",
        "",
        "#include <cstddef>",
        "#include <limits>",
        "#include <optional>",
        "#include <vector>",
        "",
        "namespace aafr::research_reference {",
        "",
    ]
    return "\n".join(header) + "\n\n".join(parts) + "\n\n}  // namespace aafr::research_reference\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    text = build()
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if current != text:
            print("scipy_reference_values.h is stale; rerun tests/research/scipy_reference.py")
            return 1
        print("scipy_reference_values.h is up to date")
        return 0
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
