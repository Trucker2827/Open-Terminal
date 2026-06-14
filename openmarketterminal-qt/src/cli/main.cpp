#include "cli/CommandDispatch.h"
#include <QCoreApplication>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("openterminalcli");
    app.setApplicationVersion(OPENMARKETTERMINAL_VERSION_STRING);
    QStringList args = app.arguments();
    args.removeFirst(); // drop argv[0]
    return openmarketterminal::cli::dispatch(args);
}
