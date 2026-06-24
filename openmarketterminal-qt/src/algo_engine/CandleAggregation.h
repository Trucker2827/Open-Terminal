// src/algo_engine/CandleAggregation.h
#pragma once
// Pure candle-aggregation helpers, split out of CandleDataFetcher so the
// backtest/feed math can be unit-tested without the network fetcher.
#include "algo_engine/AlgoEngineTypes.h"

#include <QVector>

namespace openmarketterminal::algo {

/// Aggregate `factor` consecutive bars into one (e.g. three 1m bars → one 3m
/// bar). open/open_time come from the first bar, close/close_time from the
/// last, high = max, low = min, volume = sum. A trailing group with fewer than
/// `factor` bars is dropped so we never emit a misleading partial bar. With
/// factor <= 1 the input is returned unchanged.
QVector<OhlcvCandle> aggregate_candles(const QVector<OhlcvCandle>& in, int factor);

} // namespace openmarketterminal::algo
