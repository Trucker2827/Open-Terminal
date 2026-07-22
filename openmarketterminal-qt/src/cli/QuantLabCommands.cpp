// `quant` command family — AI Quant Lab modules from the CLI.
//
// Own TU, excluded from unity builds like the other cli command families
// (MSVC front-end capacity; see CommandDispatch.cpp).

#include "cli/QuantLabCommands.h"

#include "python/PythonRunner.h"
#include "python/PythonSetupManager.h"
#include "services/ai_quant_lab/AIQuantLabTypes.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#include <cstdio>

namespace openmarketterminal::cli {

// Shared CLI flag helpers (defined in CommandDispatch.cpp, declared for the
// edge-journal TUs in EdgeJournalShared.h — redeclared here to avoid pulling
// that header's heavier includes into this small TU).
bool take_string_option(QStringList& args, const QString& flag, QString& out);

namespace {

using services::quant::QuantModule;
using services::quant::all_quant_modules;

const QuantModule* find_module(const QVector<QuantModule>& modules, const QString& id) {
    for (const auto& m : modules)
        if (m.id == id)
            return &m;
    return nullptr;
}

int quant_list(const GlobalOpts& opts) {
    const auto modules = all_quant_modules();
    if (opts.json) {
        QJsonArray rows;
        for (const auto& m : modules)
            rows.append(QJsonObject{{"id", m.id}, {"label", m.label}, {"category", m.category},
                                    {"script", m.script}, {"description", m.description}});
        std::printf("%s\n", QJsonDocument(rows).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    QString category;
    for (const auto& m : modules) {
        if (m.category != category) {
            category = m.category;
            std::printf("%s\n", qUtf8Printable(category));
        }
        std::printf("  %-22s %s\n", qUtf8Printable(m.id), qUtf8Printable(m.description));
    }
    std::printf("\nusage: quant run <module_id> <command> ['{json params}']\n"
                "       (commands per module: run <module_id> check_status, or see the module panel)\n");
    return 0;
}

int quant_run(const GlobalOpts& opts, QStringList args) {
    QString timeout_raw;
    take_string_option(args, QStringLiteral("--timeout-sec"), timeout_raw);
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: quant run <module_id> <command> ['{json params}'] [--timeout-sec N]\n");
        return 2;
    }
    const QString module_id = args.takeFirst();
    const QString command = args.takeFirst();
    const QString params = args.isEmpty() ? QStringLiteral("{}") : args.takeFirst();
    if (!args.isEmpty()) {
        std::fprintf(stderr, "unknown args: %s\n", qUtf8Printable(args.join(' ')));
        return 2;
    }
    int timeout_sec = 600;
    if (!timeout_raw.isEmpty()) {
        bool ok = false;
        timeout_sec = timeout_raw.toInt(&ok);
        if (!ok || timeout_sec < 5 || timeout_sec > 7200) {
            std::fprintf(stderr, "--timeout-sec must be 5..7200\n");
            return 2;
        }
    }
    if (!params.isEmpty() && QJsonDocument::fromJson(params.toUtf8()).isNull() &&
        params != QLatin1String("{}")) {
        std::fprintf(stderr, "params must be a JSON object, got: %s\n", qUtf8Printable(params));
        return 2;
    }

    const auto modules = all_quant_modules();
    const QuantModule* module = find_module(modules, module_id);
    if (!module) {
        QStringList ids;
        for (const auto& m : modules) ids << m.id;
        std::fprintf(stderr, "unknown module '%s'. modules: %s\n",
                     qUtf8Printable(module_id), qUtf8Printable(ids.join(", ")));
        return 2;
    }

    auto& runner = python::PythonRunner::instance();
    const QString scripts_dir = runner.scripts_dir();
    const QString script_path = scripts_dir + "/" + module->script;
    if (!QFileInfo::exists(script_path)) {
        std::fprintf(stderr, "script not found: %s\n", qUtf8Printable(script_path));
        return 4;
    }
    // Same venv routing as the GUI's PythonRunner (gluonts/functime wrappers
    // ride venv-numpy1; everything else numpy2), same environment.
    QString python = python::PythonSetupManager::instance().python_path(
        python::PythonRunner::venv_for_script(module->script));
    if (!QFileInfo::exists(python))
        python = runner.python_path();
    if (python.isEmpty() || !QFileInfo::exists(python)) {
        std::fprintf(stderr, "no python interpreter found (run the app once to set up venvs)\n");
        return 4;
    }

    // Sub-package scripts (deep agent's agents/rdagents/cli.py) run as
    // -m modules when the package chain is importable — mirrors PythonRunner.
    QStringList full_args;
    bool module_form = module->script.contains('/');
    if (module_form) {
        QString check = scripts_dir;
        QStringList parts = module->script.split('/');
        parts.removeLast();
        for (const QString& p : parts) {
            check += "/" + p;
            if (!QFileInfo::exists(check + "/__init__.py")) { module_form = false; break; }
        }
    }
    if (module_form) {
        QString mod = module->script;
        mod.remove(QStringLiteral(".py"));
        mod.replace('/', '.');
        full_args << QStringLiteral("-m") << mod;
    } else {
        full_args << script_path;
    }
    full_args << command << params;

    QProcess proc;
    proc.setWorkingDirectory(scripts_dir);
    proc.setProcessEnvironment(runner.build_python_env());
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start(python, full_args);
    if (!proc.waitForStarted(10'000)) {
        std::fprintf(stderr, "failed to start %s\n", qUtf8Printable(python));
        return 4;
    }
    if (!proc.waitForFinished(timeout_sec * 1000)) {
        proc.kill();
        proc.waitForFinished(5'000);
        std::fprintf(stderr, "module timed out after %ds (raise with --timeout-sec)\n", timeout_sec);
        return 5;
    }
    const QByteArray out = proc.readAllStandardOutput();
    const QByteArray err = proc.readAllStandardError();
    if (!out.isEmpty())
        std::fwrite(out.constData(), 1, static_cast<size_t>(out.size()), stdout);
    if (!err.isEmpty() && (proc.exitCode() != 0 || !opts.json))
        std::fwrite(err.constData(), 1, static_cast<size_t>(err.size()), stderr);
    if (proc.exitStatus() != QProcess::NormalExit)
        return 5;
    return proc.exitCode();
}

} // namespace

int quant_lab_command(const GlobalOpts& opts, QStringList args) {
    const QString sub = args.isEmpty() ? QStringLiteral("list") : args.takeFirst();
    if (sub == QLatin1String("list"))
        return quant_list(opts);
    if (sub == QLatin1String("run"))
        return quant_run(opts, args);
    if (sub == QLatin1String("script")) {
        if (args.size() != 1) { std::fprintf(stderr, "usage: quant script <module_id>\n"); return 2; }
        const auto modules = all_quant_modules();
        const QuantModule* module = find_module(modules, args.first());
        if (!module) { std::fprintf(stderr, "unknown module '%s'\n", qUtf8Printable(args.first())); return 2; }
        std::printf("%s\n", qUtf8Printable(
            python::PythonRunner::instance().scripts_dir() + "/" + module->script));
        return 0;
    }
    std::fprintf(stderr, "usage: quant [list|run <module_id> <command> ['{json}']|script <module_id>]\n");
    return 2;
}

} // namespace openmarketterminal::cli
