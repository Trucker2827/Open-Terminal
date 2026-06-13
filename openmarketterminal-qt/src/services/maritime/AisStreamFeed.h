#pragma once
// AisStreamFeed — live vessel positions from AISStream.io (free, user API key).
//
// Spawns scripts/maritime/aisstream_feed.py (a long-running WebSocket
// subprocess, same shape as exchange/ws_stream.py), reads one vessel JSON per
// line, keeps a rolling cache of recently-seen vessels, and publishes:
//   • maritime:vessels:area      → VesselsPage (the dashboard widget + screen)
//   • maritime:vessel:<imo>      → VesselData (per-vessel, when an IMO is known)
//
// Key comes from the "connectors.aisstream_key" setting. With no key, start()
// is a no-op — no subprocess, no feed (honest: the widget shows its empty state).

#include "services/maritime/MaritimeTypes.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

class QProcess;
class QTimer;

namespace openmarketterminal::services::maritime {

class AisStreamFeed : public QObject {
    Q_OBJECT
  public:
    static AisStreamFeed& instance();

    /// Start the feed if a key is configured and it isn't already running.
    /// Idempotent. Returns true if the feed is (now) active.
    bool start();
    void stop();
    bool is_running() const;

  private:
    explicit AisStreamFeed(QObject* parent = nullptr);
    Q_DISABLE_COPY(AisStreamFeed)

    void on_stdout_ready();
    void ingest_line(const QByteArray& line);
    void publish_area();

    QProcess* proc_ = nullptr;
    QTimer* publish_timer_ = nullptr;
    QByteArray stdout_buf_;

    // Rolling cache of recently-seen vessels, keyed by MMSI, newest-last for
    // simple capped eviction.
    QHash<qint64, VesselData> vessels_;
    QList<qint64> order_;
    static constexpr int kMaxVessels = 150;
};

} // namespace openmarketterminal::services::maritime
