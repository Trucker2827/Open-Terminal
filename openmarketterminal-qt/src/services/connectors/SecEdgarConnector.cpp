#include "services/connectors/SecEdgarConnector.h"

#include "services/connectors/Connector.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>

namespace openmarketterminal::connectors {

SecEdgarConnector& SecEdgarConnector::instance() {
    static SecEdgarConnector s;
    return s;
}

SecEdgarConnector::SecEdgarConnector(QObject* parent) : QObject(parent) {
    qRegisterMetaType<openmarketterminal::connectors::EdgarFiling>();
    qRegisterMetaType<QVector<openmarketterminal::connectors::EdgarFiling>>();
    qRegisterMetaType<openmarketterminal::connectors::Provenance>("openmarketterminal::connectors::Provenance");
}

void SecEdgarConnector::fetch_filings(const QString& cik, int limit) {
    // Accept a numeric CIK in any form; SEC wants it zero-padded to 10 digits.
    QString digits;
    for (const QChar& c : cik.trimmed())
        if (c.isDigit())
            digits.append(c);
    if (digits.isEmpty()) {
        Provenance prov;
        prov.source = QStringLiteral("SEC EDGAR");
        prov.key_req = KeyRequirement::None;
        prov.source_url = QStringLiteral("https://data.sec.gov/submissions/");
        prov.fetched_at_ms = QDateTime::currentMSecsSinceEpoch();
        emit filings_failed(QStringLiteral("a numeric CIK is required (ticker→CIK lookup pending)"), prov);
        return;
    }
    const QString padded = QStringLiteral("%1").arg(digits.toLongLong(), 10, 10, QLatin1Char('0'));
    const QString url = QStringLiteral("https://data.sec.gov/submissions/CIK%1.json").arg(padded);
    const QString cache_key = QStringLiteral("edgar:submissions:%1").arg(padded);

    QPointer<SecEdgarConnector> self = this;
    ConnectorFetch::json(
        this, QStringLiteral("SEC EDGAR"), KeyRequirement::None, /*key_present=*/false, url, cache_key,
        /*ttl_seconds=*/3600, [self, padded, limit](Provenanced<QJsonDocument> r) {
            if (!self)
                return;
            if (!r.ok) {
                emit self->filings_failed(r.error, r.provenance);
                return;
            }
            const QJsonObject root = r.data.object();
            const QString company = root.value(QStringLiteral("name")).toString();
            const QJsonObject recent =
                root.value(QStringLiteral("filings")).toObject().value(QStringLiteral("recent")).toObject();
            const QJsonArray forms = recent.value(QStringLiteral("form")).toArray();
            const QJsonArray dates = recent.value(QStringLiteral("filingDate")).toArray();
            const QJsonArray accns = recent.value(QStringLiteral("accessionNumber")).toArray();
            const QJsonArray docs = recent.value(QStringLiteral("primaryDocument")).toArray();

            QVector<EdgarFiling> out;
            const int n = qMin(forms.size(), limit);
            out.reserve(n);
            for (int i = 0; i < n; ++i) {
                EdgarFiling f;
                f.form = forms.at(i).toString();
                f.filing_date = i < dates.size() ? dates.at(i).toString() : QString();
                f.accession = i < accns.size() ? accns.at(i).toString() : QString();
                f.primary_doc = i < docs.size() ? docs.at(i).toString() : QString();
                out.append(f);
            }
            emit self->filings_ready(padded, company, out, r.provenance);
        });
}

void SecEdgarConnector::fetch_filings_by_ticker(const QString& ticker, int limit) {
    const QString t = ticker.trimmed().toUpper();
    if (t.isEmpty()) {
        Provenance prov;
        prov.source = QStringLiteral("SEC EDGAR");
        prov.source_url = QStringLiteral("https://www.sec.gov/files/company_tickers.json");
        prov.fetched_at_ms = QDateTime::currentMSecsSinceEpoch();
        emit filings_failed(QStringLiteral("missing ticker"), prov);
        return;
    }
    // SEC's www host requires a descriptive, contact-style User-Agent.
    static const QString kSecUa = QStringLiteral("OpenMarketTerminal/1.0 (local-first; contact@openmarketterminal.local)");

    QPointer<SecEdgarConnector> self = this;
    ConnectorFetch::text(
        this, QStringLiteral("SEC EDGAR"), KeyRequirement::None, /*key_present=*/false,
        QStringLiteral("https://www.sec.gov/files/company_tickers.json"), QStringLiteral("edgar:tickers"),
        /*ttl_seconds=*/86400,
        [self, t, limit](Provenanced<QString> r) {
            if (!self)
                return;
            if (!r.ok) {
                emit self->filings_failed(QStringLiteral("ticker map fetch failed: %1").arg(r.error), r.provenance);
                return;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(r.data.toUtf8());
            const QJsonObject root = doc.object();
            QString cik;
            for (auto it = root.begin(); it != root.end(); ++it) {
                const QJsonObject e = it.value().toObject();
                if (e.value(QStringLiteral("ticker")).toString().toUpper() == t) {
                    cik = QString::number(static_cast<qlonglong>(e.value(QStringLiteral("cik_str")).toDouble()));
                    break;
                }
            }
            if (cik.isEmpty()) {
                emit self->filings_failed(QStringLiteral("ticker '%1' not found in SEC registry").arg(t),
                                          r.provenance);
                return;
            }
            self->fetch_filings(cik, limit); // chains to the data.sec.gov submissions fetch
        },
        kSecUa);
}

} // namespace openmarketterminal::connectors
