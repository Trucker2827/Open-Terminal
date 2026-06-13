#include "datahub/DataHubMetaTypes.h"

#include "core/logging/Logger.h"

namespace openmarketterminal::datahub {

void register_metatypes() {
    qRegisterMetaType<openmarketterminal::services::QuoteData>("openmarketterminal::services::QuoteData");
    qRegisterMetaType<openmarketterminal::services::HistoryPoint>("openmarketterminal::services::HistoryPoint");
    qRegisterMetaType<openmarketterminal::services::InfoData>("openmarketterminal::services::InfoData");
    qRegisterMetaType<openmarketterminal::services::NewsArticle>("openmarketterminal::services::NewsArticle");
    qRegisterMetaType<openmarketterminal::services::EconomicsResult>("openmarketterminal::services::EconomicsResult");
    qRegisterMetaType<QVector<openmarketterminal::services::HistoryPoint>>("QVector<openmarketterminal::services::HistoryPoint>");
    qRegisterMetaType<QVector<openmarketterminal::services::NewsArticle>>("QVector<openmarketterminal::services::NewsArticle>");
    qRegisterMetaType<QVector<double>>("QVector<double>");
    qRegisterMetaType<openmarketterminal::trading::TickerData>("openmarketterminal::trading::TickerData");
    qRegisterMetaType<openmarketterminal::trading::OrderBookData>("openmarketterminal::trading::OrderBookData");
    qRegisterMetaType<openmarketterminal::trading::Candle>("openmarketterminal::trading::Candle");
    qRegisterMetaType<openmarketterminal::trading::TradeData>("openmarketterminal::trading::TradeData");
    qRegisterMetaType<openmarketterminal::services::polymarket::OrderBook>("openmarketterminal::services::polymarket::OrderBook");

    // Prediction Markets (Polymarket, Kalshi, …)
    qRegisterMetaType<openmarketterminal::services::prediction::MarketKey>("openmarketterminal::services::prediction::MarketKey");
    qRegisterMetaType<openmarketterminal::services::prediction::PredictionMarket>("openmarketterminal::services::prediction::PredictionMarket");
    qRegisterMetaType<openmarketterminal::services::prediction::PredictionEvent>("openmarketterminal::services::prediction::PredictionEvent");
    qRegisterMetaType<openmarketterminal::services::prediction::PredictionOrderBook>("openmarketterminal::services::prediction::PredictionOrderBook");
    qRegisterMetaType<openmarketterminal::services::prediction::PriceHistory>("openmarketterminal::services::prediction::PriceHistory");
    qRegisterMetaType<openmarketterminal::services::prediction::PredictionTrade>("openmarketterminal::services::prediction::PredictionTrade");
    qRegisterMetaType<openmarketterminal::services::prediction::PredictionPosition>("openmarketterminal::services::prediction::PredictionPosition");
    qRegisterMetaType<openmarketterminal::services::prediction::OpenOrder>("openmarketterminal::services::prediction::OpenOrder");
    qRegisterMetaType<openmarketterminal::services::prediction::OrderResult>("openmarketterminal::services::prediction::OrderResult");
    qRegisterMetaType<openmarketterminal::services::prediction::AccountBalance>("openmarketterminal::services::prediction::AccountBalance");
    qRegisterMetaType<QVector<openmarketterminal::services::prediction::PredictionMarket>>("QVector<openmarketterminal::services::prediction::PredictionMarket>");
    qRegisterMetaType<QVector<openmarketterminal::services::prediction::PredictionEvent>>("QVector<openmarketterminal::services::prediction::PredictionEvent>");
    qRegisterMetaType<QVector<openmarketterminal::services::prediction::PredictionTrade>>("QVector<openmarketterminal::services::prediction::PredictionTrade>");
    qRegisterMetaType<QVector<openmarketterminal::services::prediction::PredictionPosition>>("QVector<openmarketterminal::services::prediction::PredictionPosition>");
    qRegisterMetaType<QVector<openmarketterminal::services::prediction::OpenOrder>>("QVector<openmarketterminal::services::prediction::OpenOrder>");
    qRegisterMetaType<openmarketterminal::services::DbnDataPoint>("openmarketterminal::services::DbnDataPoint");
    qRegisterMetaType<openmarketterminal::services::GovDataResult>("openmarketterminal::services::GovDataResult");
    qRegisterMetaType<openmarketterminal::trading::BrokerPosition>("openmarketterminal::trading::BrokerPosition");
    qRegisterMetaType<openmarketterminal::trading::BrokerHolding>("openmarketterminal::trading::BrokerHolding");
    qRegisterMetaType<openmarketterminal::trading::BrokerOrderInfo>("openmarketterminal::trading::BrokerOrderInfo");
    qRegisterMetaType<openmarketterminal::trading::BrokerQuote>("openmarketterminal::trading::BrokerQuote");
    qRegisterMetaType<openmarketterminal::trading::BrokerFunds>("openmarketterminal::trading::BrokerFunds");
    qRegisterMetaType<QVector<openmarketterminal::trading::BrokerPosition>>("QVector<openmarketterminal::trading::BrokerPosition>");
    qRegisterMetaType<QVector<openmarketterminal::trading::BrokerHolding>>("QVector<openmarketterminal::trading::BrokerHolding>");
    qRegisterMetaType<QVector<openmarketterminal::trading::BrokerOrderInfo>>("QVector<openmarketterminal::trading::BrokerOrderInfo>");
    qRegisterMetaType<QVector<openmarketterminal::trading::BrokerQuote>>("QVector<openmarketterminal::trading::BrokerQuote>");

    // F&O / Options chain (Phase 11 — Sensibull-style tab)
    qRegisterMetaType<openmarketterminal::services::options::OptionGreeks>("openmarketterminal::services::options::OptionGreeks");
    qRegisterMetaType<openmarketterminal::services::options::OptionChainRow>("openmarketterminal::services::options::OptionChainRow");
    qRegisterMetaType<openmarketterminal::services::options::OptionChain>("openmarketterminal::services::options::OptionChain");
    qRegisterMetaType<openmarketterminal::services::options::StrategyLeg>("openmarketterminal::services::options::StrategyLeg");
    qRegisterMetaType<openmarketterminal::services::options::Strategy>("openmarketterminal::services::options::Strategy");
    qRegisterMetaType<openmarketterminal::services::options::PayoffPoint>("openmarketterminal::services::options::PayoffPoint");
    qRegisterMetaType<openmarketterminal::services::options::StrategyAnalytics>(
        "openmarketterminal::services::options::StrategyAnalytics");
    qRegisterMetaType<QVector<openmarketterminal::services::options::OptionChainRow>>(
        "QVector<openmarketterminal::services::options::OptionChainRow>");
    qRegisterMetaType<QVector<openmarketterminal::services::options::PayoffPoint>>(
        "QVector<openmarketterminal::services::options::PayoffPoint>");
    qRegisterMetaType<QVector<openmarketterminal::services::options::StrategyLeg>>(
        "QVector<openmarketterminal::services::options::StrategyLeg>");
    qRegisterMetaType<openmarketterminal::services::options::OISample>("openmarketterminal::services::options::OISample");
    qRegisterMetaType<QVector<openmarketterminal::services::options::OISample>>(
        "QVector<openmarketterminal::services::options::OISample>");

    // Phase 8 — Geopolitics / Maritime / RelationshipMap
    qRegisterMetaType<openmarketterminal::services::geo::NewsEvent>("openmarketterminal::services::geo::NewsEvent");
    qRegisterMetaType<openmarketterminal::services::geo::HDXDataset>("openmarketterminal::services::geo::HDXDataset");
    qRegisterMetaType<openmarketterminal::services::geo::UniqueCountry>("openmarketterminal::services::geo::UniqueCountry");
    qRegisterMetaType<openmarketterminal::services::geo::UniqueCategory>("openmarketterminal::services::geo::UniqueCategory");
    qRegisterMetaType<QVector<openmarketterminal::services::geo::NewsEvent>>("QVector<openmarketterminal::services::geo::NewsEvent>");
    qRegisterMetaType<QVector<openmarketterminal::services::geo::HDXDataset>>("QVector<openmarketterminal::services::geo::HDXDataset>");
    qRegisterMetaType<QVector<openmarketterminal::services::geo::UniqueCountry>>("QVector<openmarketterminal::services::geo::UniqueCountry>");
    qRegisterMetaType<QVector<openmarketterminal::services::geo::UniqueCategory>>("QVector<openmarketterminal::services::geo::UniqueCategory>");
    qRegisterMetaType<openmarketterminal::services::geo::EventsPage>("openmarketterminal::services::geo::EventsPage");
    qRegisterMetaType<openmarketterminal::services::maritime::VesselData>("openmarketterminal::services::maritime::VesselData");
    qRegisterMetaType<QVector<openmarketterminal::services::maritime::VesselData>>("QVector<openmarketterminal::services::maritime::VesselData>");
    qRegisterMetaType<openmarketterminal::services::maritime::VesselsPage>("openmarketterminal::services::maritime::VesselsPage");
    qRegisterMetaType<openmarketterminal::services::maritime::VesselHistoryPage>("openmarketterminal::services::maritime::VesselHistoryPage");
    qRegisterMetaType<openmarketterminal::relmap::RelationshipData>("openmarketterminal::relmap::RelationshipData");

    LOG_INFO("DataHub", "Registered payload meta-types");
}

} // namespace openmarketterminal::datahub
