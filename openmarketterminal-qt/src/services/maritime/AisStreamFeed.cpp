#include "services/maritime/AisStreamFeed.h"

#include "core/logging/Logger.h"
#include "datahub/DataHub.h"
#include "python/PythonRunner.h"
#include "python/PythonSetupManager.h"
#include "storage/repositories/SettingsRepository.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

namespace openmarketterminal::services::maritime {

namespace {
constexpr const char* TAG = "AisStreamFeed";
#ifdef _WIN32
constexpr QChar kPathSep = ';';
#else
constexpr QChar kPathSep = ':';
#endif
} // namespace

AisStreamFeed& AisStreamFeed::instance() {
    static AisStreamFeed s;
    return s;
}

AisStreamFeed::AisStreamFeed(QObject* parent) : QObject(parent) {
    publish_timer_ = new QTimer(this);
    publish_timer_->setInterval(2000);  // coalesce a burst of ticks into one publish
    connect(publish_timer_, &QTimer::timeout, this, &AisStreamFeed::publish_area);
}

bool AisStreamFeed::is_running() const {
    return proc_ != nullptr;
}

bool AisStreamFeed::start() {
    if (proc_)
        return true;  // already running

    auto key_r = openmarketterminal::SettingsRepository::instance().get(
        QStringLiteral("connectors.aisstream_key"));
    const QString key = key_r.is_ok() ? key_r.value().trimmed() : QString();
    if (key.isEmpty()) {
        LOG_DEBUG(TAG, "No AISStream key configured — live vessel feed inactive.");
        return false;
    }

    QString python_exe = python::PythonSetupManager::instance().python_path("venv-numpy2");
    if (python_exe.isEmpty() || !QFileInfo::exists(python_exe))
        python_exe = python::PythonRunner::instance().python_path();
    const QString scripts_dir = python::PythonRunner::instance().scripts_dir();
    const QString script = scripts_dir + QStringLiteral("/maritime/aisstream_feed.py");
    if (python_exe.isEmpty() || !QFileInfo::exists(script)) {
        LOG_ERROR(TAG, QString("Cannot start: python='%1' script='%2'").arg(python_exe, script));
        return false;
    }

    proc_ = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("PYTHONIOENCODING", "utf-8");
    env.insert("PYTHONUNBUFFERED", "1");
    env.insert("PYTHONDONTWRITEBYTECODE", "1");
    env.insert("OPENMARKETTERMINAL_AISSTREAM_KEY", key);
    const QString pypath = env.value("PYTHONPATH");
    env.insert("PYTHONPATH", pypath.isEmpty() ? scripts_dir : (scripts_dir + kPathSep + pypath));
    proc_->setProcessEnvironment(env);
    proc_->setWorkingDirectory(scripts_dir);
    proc_->setProcessChannelMode(QProcess::SeparateChannels);

    connect(proc_, &QProcess::readyReadStandardOutput, this, &AisStreamFeed::on_stdout_ready);
    connect(proc_, &QProcess::errorOccurred, this, [](QProcess::ProcessError e) {
        LOG_WARN(TAG, QString("QProcess error %1").arg(static_cast<int>(e)));
    });

    stdout_buf_.clear();
    proc_->start(python_exe, {script});
    publish_timer_->start();
    LOG_INFO(TAG, "Live AIS vessel feed started (AISStream.io).");
    return true;
}

void AisStreamFeed::stop() {
    if (publish_timer_)
        publish_timer_->stop();
    if (proc_) {
        proc_->disconnect(this);
        proc_->terminate();
        if (!proc_->waitForFinished(1500))
            proc_->kill();
        proc_->deleteLater();
        proc_ = nullptr;
    }
}

void AisStreamFeed::on_stdout_ready() {
    if (!proc_)
        return;
    stdout_buf_ += proc_->readAllStandardOutput();
    int nl;
    while ((nl = stdout_buf_.indexOf('\n')) >= 0) {
        const QByteArray line = stdout_buf_.left(nl);
        stdout_buf_.remove(0, nl + 1);
        if (!line.trimmed().isEmpty())
            ingest_line(line);
    }
}

void AisStreamFeed::ingest_line(const QByteArray& line) {
    const QJsonObject o = QJsonDocument::fromJson(line).object();
    if (o.contains("ready")) {
        LOG_INFO(TAG, "AISStream subscription live.");
        return;
    }
    if (o.contains("error")) {
        LOG_WARN(TAG, "AISStream: " + o.value("error").toString());
        return;
    }
    const qint64 mmsi = static_cast<qint64>(o.value("mmsi").toDouble());
    if (mmsi == 0)
        return;

    VesselData v;
    v.imo = o.value("imo").toString();
    v.name = o.value("name").toString();
    v.latitude = o.value("lat").toDouble();
    v.longitude = o.value("lon").toDouble();
    v.speed = o.value("speed").toDouble();
    v.angle = o.value("angle").toDouble();
    v.to_port = o.value("to_port").toString();   // from_port / route_progress: AIS carries neither
    v.last_updated = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));

    if (!vessels_.contains(mmsi))
        order_.append(mmsi);
    vessels_.insert(mmsi, v);
    while (order_.size() > kMaxVessels) {
        const qint64 old = order_.takeFirst();
        vessels_.remove(old);
    }

    // Per-vessel topic for any consumer tracking this exact IMO.
    if (!v.imo.isEmpty())
        datahub::DataHub::instance().publish(QStringLiteral("maritime:vessel:") + v.imo,
                                             QVariant::fromValue(v));
}

void AisStreamFeed::publish_area() {
    if (vessels_.isEmpty())
        return;
    VesselsPage page;
    page.vessels.reserve(vessels_.size());
    // Newest-first so the freshest positions head the list.
    for (auto it = order_.crbegin(); it != order_.crend(); ++it) {
        auto vit = vessels_.constFind(*it);
        if (vit != vessels_.constEnd())
            page.vessels.append(vit.value());
    }
    page.total_count = page.vessels.size();
    page.found_count = page.vessels.size();
    datahub::DataHub::instance().publish(QStringLiteral("maritime:vessels:area"),
                                         QVariant::fromValue(page));
}

} // namespace openmarketterminal::services::maritime
