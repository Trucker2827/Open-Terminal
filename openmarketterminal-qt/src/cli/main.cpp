#include "cli/CommandDispatch.h"
#include <QCoreApplication>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("openterminalcli");
    QCoreApplication::setApplicationVersion(OPENMARKETTERMINAL_VERSION_STRING);
    QStringList args = QCoreApplication::arguments();
    args.removeFirst(); // drop argv[0]
    return openmarketterminal::cli::dispatch(args);
}
