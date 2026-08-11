"""Scoring diagnostics: WHY the model beats the mid or does not.

The trust flag compares two mean Brier scores. That answers *whether*, never
*why*, and the two failure modes it cannot tell apart have opposite remedies:

  * well calibrated but useless -- the model always sits near the mid, so it is
    honest and adds nothing. Low reliability error, low resolution.
  * sharp but miscalibrated -- the model looks edgy and loses money. High
    resolution, high reliability error.

Murphy's decomposition separates them:

    Brier = reliability - resolution + uncertainty

`uncertainty` is the base rate's own variance. It is identical for the model and
the mid over the same outcomes, which is exactly what makes the other two terms
comparable between them.

Measured on the live bet-eligible slice when this was written (4,043
observations), the model was worse on BOTH terms -- reliability 0.0184 vs the
mid's 0.0056, resolution 0.0756 vs 0.1093. Neither textbook failure mode: worse
calibrated AND less sharp than the price it was handed as an input feature.

Everything here is pure: `(probability, outcome)` pairs in, numbers out. No I/O,
no model, no state -- so it is testable against synthetic cases where the right
answer is known by construction.
"""
import math
import random

# Below this, a confident miss would be an infinity and one bad row would
# destroy the whole report. Clipping keeps it large but finite.
LOG_SCORE_CLIP = 1e-6


def _clip(p):
    return min(1.0 - LOG_SCORE_CLIP, max(LOG_SCORE_CLIP, float(p)))


def log_score(pairs):
    """Mean negative log-likelihood; None when empty. Lower is better.

    Punishes overconfidence far harder than Brier does. The RATIO between the
    two is itself the diagnostic: a model whose log score is proportionally
    worse than its Brier is overstating its certainty, which is what an
    independent shrinkage sweep found here (optimal lambda 0.1-0.2).
    """
    if not pairs:
        return None
    total = 0.0
    for p, y in pairs:
        q = _clip(p)
        total += -(math.log(q) if y else math.log(1.0 - q))
    return total / len(pairs)


def binned_brier(pairs, bins=10):
    """Brier of the BIN-MEAN forecasts; None when empty.

    The decomposition below is exact against THIS, not against the raw Brier.
    Murphy's identity assumes each forecast is its bin's representative, and the
    cross-term only vanishes when p is constant within a bin -- so continuous
    forecasts leave a small binning residual. Exposing that here keeps the
    approximation visible instead of hiding it behind a loose tolerance: the
    tests assert exactness against this value, and separately bound the residual
    against the raw Brier.
    """
    if not pairs:
        return None
    n = len(pairs)
    buckets = {}
    for p, y in pairs:
        idx = min(bins - 1, int(float(p) * bins))
        slot = buckets.setdefault(idx, [0, 0.0, 0.0])
        slot[0] += 1
        slot[1] += float(p)
        slot[2] += 1.0 if y else 0.0
    total = 0.0
    for count, p_sum, y_sum in buckets.values():
        p_bar = p_sum / count
        # sum over the bin of (p_bar - y_i)^2, expanded so we never revisit rows
        total += count * p_bar * p_bar - 2.0 * p_bar * y_sum + y_sum
    return total / n


def murphy_decomposition(pairs, bins=10):
    """`(reliability, resolution, uncertainty)`; None when empty.

    reliability -- mean squared gap between forecast and observed frequency
                   within each bin. LOWER is better (0 = perfectly calibrated).
    resolution  -- how far bin outcomes move from the base rate. HIGHER is
                   better (0 = the forecast carries no information).
    uncertainty -- base_rate * (1 - base_rate). Not a property of the forecast.

    Satisfies `reliability - resolution + uncertainty == brier` exactly (up to
    float error), which the tests assert -- if that identity drifts, the terms
    are describing something other than the Brier the reader is comparing.
    """
    if not pairs:
        return None
    n = len(pairs)
    outcomes = [1.0 if y else 0.0 for _, y in pairs]
    base = sum(outcomes) / n
    uncertainty = base * (1.0 - base)

    buckets = {}
    for (p, _), y in zip(pairs, outcomes):
        # Index by bin; p == 1.0 belongs in the last bin, not a new one.
        idx = min(bins - 1, int(float(p) * bins))
        slot = buckets.setdefault(idx, [0, 0.0, 0.0])
        slot[0] += 1
        slot[1] += float(p)
        slot[2] += y

    reliability = 0.0
    resolution = 0.0
    for count, p_sum, y_sum in buckets.values():
        p_bar = p_sum / count
        y_bar = y_sum / count
        reliability += count * (p_bar - y_bar) ** 2
        resolution += count * (y_bar - base) ** 2
    return reliability / n, resolution / n, uncertainty


def information_gain_bits(model_pairs, mid_pairs):
    """Bits per observation the model adds over the mid; None when unusable.

    Log score IS cross-entropy, so the difference in nats over ln(2) is the
    information gained. NEGATIVE means the model is destroying information --
    a sharper kill-switch than "beats the market sometimes", because it is a
    single signed number with a zero that means something.
    """
    if not model_pairs or not mid_pairs or len(model_pairs) != len(mid_pairs):
        return None
    model = log_score(model_pairs)
    mid = log_score(mid_pairs)
    if model is None or mid is None:
        return None
    return (mid - model) / math.log(2.0)


def paired_bootstrap_ci(model_scores, mid_scores, seed, samples=4000, alpha=0.05):
    """`(point, lo, hi)` for mean(model) - mean(mid); None when unusable.

    PAIRED: each resample draws the same index from both lists, because the two
    scores for one contract are not independent -- they score the same outcome.
    Resampling them separately would inflate the interval and hide a small but
    consistent per-contract difference.

    `seed` is required, not optional. This number is printed in a report that is
    regenerated on a schedule; one that wanders when nothing changed would train
    the reader to ignore it.
    """
    if not model_scores or not mid_scores or len(model_scores) != len(mid_scores):
        return None
    deltas = [m - d for m, d in zip(model_scores, mid_scores)]
    n = len(deltas)
    point = sum(deltas) / n
    rng = random.Random(seed)
    means = []
    for _ in range(samples):
        total = 0.0
        for _ in range(n):
            total += deltas[rng.randrange(n)]
        means.append(total / n)
    means.sort()
    lo = means[int((alpha / 2.0) * samples)]
    hi = means[min(samples - 1, int((1.0 - alpha / 2.0) * samples))]
    return point, lo, hi
