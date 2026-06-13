// src/algo_engine/AlgoEngineTypes.cpp
#include "algo_engine/AlgoEngineTypes.h"

namespace {
const int reg_algo_metrics = qRegisterMetaType<openmarketterminal::algo::AlgoMetrics>("openmarketterminal::algo::AlgoMetrics");
const int reg_algo_trade = qRegisterMetaType<openmarketterminal::algo::AlgoTradeRecord>("openmarketterminal::algo::AlgoTradeRecord");
const int reg_ohlcv = qRegisterMetaType<openmarketterminal::algo::OhlcvCandle>("openmarketterminal::algo::OhlcvCandle");
} // namespace
