//
// Fulcrum - A fast & nimble SPV Server for Bitcoin Cash
// Copyright (C) 2019-2020  Calin A. Culianu <calin.culianu@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program (see LICENSE.txt).  If not, see
// <https://www.gnu.org/licenses/>.
//
#include "App.h"
#include "BTC.h"
#include "Compat.h"
#include "Controller.h"
#include "Json.h"
#include "Logger.h"
#include "Servers.h"
#include "ThreadPool.h"
#include "Util.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegExp>
#include <QSslCipher>
#include <QSslEllipticCurve>
#include <QSslSocket>
#include <QtDebug>

#include <array>
#include <clocale>
#include <csignal>
#include <cstdlib>
#include <list>
#include <locale>
#include <tuple>
#include <utility>

App *App::_globalInstance = nullptr;

App::App(int argc, char *argv[])
    : QCoreApplication (argc, argv), tpool(std::make_unique<ThreadPool>(this))
{
    // Enforce "C" locale so JSON doesn't break. We do this because QCoreApplication
    // may set the C locale to something unexpected which may break JSON number
    // formatting & parsing.
    RAII localeParanoia([]{setCLocale();}, []{setCLocale();});

    assert(!_globalInstance);
    _globalInstance = this;
    register_MetaTypes();

    options = std::make_shared<Options>();
    options->interfaces = {{QHostAddress("0.0.0.0"), Options::DEFAULT_PORT_TCP}}; // start with default, will be cleared if -t specified
    setApplicationName(APPNAME);
    setApplicationVersion(QString("%1 %2").arg(VERSION).arg(VERSION_EXTRA));

    _logger = std::make_unique<ConsoleLogger>(this);

    try {
        parseArgs();
    } catch (const std::exception &e) {
        options->syslogMode = true; // suppress timestamp stuff
        qCritical("%s", e.what());
        qInfo() << "Use the -h option to show help.";
        std::exit(1);
    }
    if (options->syslogMode) {
        _logger = std::make_unique<SysLogger>(this);
    }

    connect(this, &App::aboutToQuit, this, &App::cleanup);
    connect(this, &App::setVerboseDebug, this, &App::on_setVerboseDebug);
    connect(this, &App::setVerboseTrace, this, &App::on_setVerboseTrace);
    QTimer::singleShot(0, this, &App::startup); // register to run after app event loop start
}

App::~App()
{
    qDebug() << "App d'tor";
    qInfo() << "Shudown complete";
    _globalInstance = nullptr;
    /// child objects will be auto-deleted, however most are already gone in cleanup() at this point.
}

void App::startup()
{
    static const auto getBannerWithTimeStamp = [] {
        QString ret; {
            QTextStream ts(&ret, QIODevice::WriteOnly|QIODevice::Truncate);
            ts << applicationName() << " " << applicationVersion() << " - " << QDateTime::currentDateTime().toString("ddd MMM d, yyyy hh:mm:ss.zzz t");
        } return ret;
    };
    // print banner to log now
    qInfo() << getBannerWithTimeStamp() << " - starting up ...";

    if ( ! Util::isClockSteady() ) {
        qDebug() << "High resolution clock provided by the std C++ library is not 'steady'. Log timestamps may drift if system time gets adjusted.";
    } else {
        qDebug() << "High resolution clock: isSteady = true";
    }
    try {
        BTC::CheckBitcoinEndiannessAndOtherSanityChecks();

        auto gotsig = [](int sig) {
            static int ct = 0;
            if (!ct++) {
                qInfo() << "Got signal: " << sig << ", exiting ...";
                app()->exit(sig);
            } else if (ct < 5) {
                std::printf("Duplicate signal %d already being handled, ignoring\n", sig);
            } else {
                std::printf("Signal %d caught more than 5 times, aborting\n", sig);
                std::abort();
            }
        };
        std::signal(SIGINT, gotsig);
        std::signal(SIGTERM, gotsig);
#ifdef Q_OS_UNIX
        std::signal(SIGQUIT, gotsig);
        std::signal(SIGHUP, SIG_IGN);
#endif

        controller = std::make_unique<Controller>(options);
        controller->startup(); // may throw

        if (!options->statsInterfaces.isEmpty()) {
            const auto num = options->statsInterfaces.count();
            qInfo() << "Stats HTTP: starting " << num << " " << Util::Pluralize("server", num) << " ...";
            // start 'stats' http servers, if any
            for (const auto & i : options->statsInterfaces)
                start_httpServer(i); // may throw
        }

    } catch (const Exception & e) {
        Fatal() << "Caught exception: " << e.what();
    }
}

void App::cleanup()
{
    qDebug() << __PRETTY_FUNCTION__ ;
    quitting = true;
    cleanup_WaitForThreadPoolWorkers();
    if (!httpServers.isEmpty()) {
        Log("Stopping Stats HTTP Servers ...");
        for (auto h : httpServers) { h->stop(); }
        httpServers.clear(); // deletes shared pointers
    }
    if (controller) { Log("Stopping Controller ... "); controller->cleanup(); controller.reset(); }
}

void App::cleanup_WaitForThreadPoolWorkers()
{
    constexpr int timeout = 5000;
    QElapsedTimer t0; t0.start();
    const int nJobs = tpool->extantJobs();
    if (nJobs)
        qInfo() << "Waiting for extant thread pool workers ...";
    const bool res = tpool->shutdownWaitForJobs(timeout);
    if (!res) {
        qWarning("After %d seconds, %d thread pool %s %s still active. App may abort with an error.",
                qRound(double(t0.elapsed())/1e3), nJobs, Util::Pluralize("worker", nJobs).toUtf8().constData(),
                qAbs(nJobs) == 1 ? "is" : "are");
    } else if (nJobs) {
        qDebug("Successfully waited for %d thread pool %s (elapsed: %0.3f secs)", nJobs,
              Util::Pluralize("worker", nJobs).toUtf8().constData(), t0.elapsed()/1e3);
    }
}


void App::parseArgs()
{
    QCommandLineParser parser;
    parser.setApplicationDescription("A Bitcoin Cash Blockchain SPV Server.");
    parser.addHelpOption();
    parser.addVersionOption();

    static constexpr auto RPCUSER = "RPCUSER", RPCPASSWORD = "RPCPASSWORD"; // optional env vars we use below

    QList<QCommandLineOption> allOptions{
         { { "D", "datadir" },
           QString("Specify a directory in which to store the database and other assorted data files. This is a"
           " required option. If the specified path does not exist, it will be created. Note that the directory in"
           " question should ideally live on a fast drive such as an SSD and it should have plenty of free space"
           " available."),
           QString("path"),
         },
         { { "t", "tcp" },
           QString("Specify an <interface:port> on which to listen for TCP connections, defaults to 0.0.0.0:%1 (all"
           " interfaces, port %1 -- only if no other interfaces are specified via -t or -s)."
           " This option may be specified more than once to bind to multiple interfaces and/or ports."
           " Suggested values for port: %1 on mainnet and %2 on testnet.").arg(Options::DEFAULT_PORT_TCP).arg(Options::DEFAULT_PORT_TCP + 10000),
           QString("interface:port"),
         },
         { { "s", "ssl" },
           QString("Specify an <interface:port> on which to listen for SSL connections. Note that if this option is"
           " specified, then the `cert` and `key` options need to also be specified otherwise the app will refuse to run."
           " This option may be specified more than once to bind to multiple interfaces and/or ports."
           " Suggested values for port: %1 on mainnet and %2 on testnet.").arg(Options::DEFAULT_PORT_SSL).arg(Options::DEFAULT_PORT_SSL + 10000),
           QString("interface:port"),
         },
        {  { "w", "ws"},
           QString("Specify an <interface:port> on which to listen for Web Socket connections (unencrypted, ws://)."
           " This option may be specified more than once to bind to multiple interfaces and/or ports."
           " Suggested values for port: %1 on mainnet and %2 on testnet.").arg(Options::DEFAULT_PORT_WS).arg(Options::DEFAULT_PORT_WS + 10000),
           QString("interface:port"),
        },
        {  { "W", "wss"},
           QString("Specify an <interface:port> on which to listen for Web Socket Secure connections (encrypted, wss://)."
           " Note that if this option is specified, then the --cert and --key options (or alternatively, the --wss-cert"
           " and --wss-key options) need to also be specified otherwise the app will refuse to run."
           " This option may be specified more than once to bind to multiple interfaces and/or ports."
           " Suggested values for port: %1 on mainnet and %2 on testnet.").arg(Options::DEFAULT_PORT_WSS).arg(Options::DEFAULT_PORT_WSS + 10000),
           QString("interface:port"),
        },
        { { "c", "cert" },
           QString("Specify a PEM file to use as the server's SSL certificate. This option is required if the -s/--ssl"
           " and/or the -W/--wss options appear at all on the command-line. The file should contain either a single"
           " valid self-signed certificate or the full certificate chain if using CA-signed certificates."),
           QString("crtfile"),
        },
        { { "k", "key" },
          QString("Specify a PEM file to use as the server's SSL key. This option is required if the -s/--ssl and/or"
          " the -W/--wss options apear at all on the command-line. The file should contain an RSA private key."
          " EC, DH, and DSA keys are also supported, but their support is experimental."),
          QString("keyfile"),
        },
        { "wss-cert",
          QString("Specify a certificate PEM file to use specifically for only WSS ports. This option is intended to"
                  " allow WSS ports to use a CA-signed certificate (required by web browsers), whereas legacy Electrum"
                  " Cash ports may want to continue to use self-signed certificates. If this option is specified,"
                  " --wss-key must also be specified. If this option is missing, then WSS ports will just fall-back to"
                  " using the certificate specified by --cert."),
          QString("crtfile"),
        },
        { "wss-key",
          QString("Specify a private key PEM file to use for WSS. This key must go with the certificate specified in"
                  " --wss-cert. If this option is specified, --wss-cert must also be specified."),
          QString("keyfile"),
        },
        { { "a", "admin" },
          QString("Specify a <port> or an <interface:port> on which to listen for TCP connections for the admin RPC service."
                  " The admin service is used for sending special control commands to the server, such as stopping"
                  " the server, and it should *NOT* be exposed to the internet. This option is required if you wish to"
                  " use the FulcrumAdmin CLI tool to send commands to Fulcrum. It is recommended that you specify the"
                  " loopback address as the bind interface for this option such as: <port> by itself or 127.0.0.1:<port> for"
                  " IPv4 and/or ::1:<port> for IPv6. If no interface is specified, and just a port number by itself is"
                  " used, then IPv4 127.0.0.1 is the bind interface used (along with the specified port)."
                  " This option may be specified more than once to bind to multiple interfaces and/or ports."),
          QString("[interface:]port"),
         },
         { { "z", "stats" },
           QString("Specify listen address and port for the stats HTTP server. Format is same as the -s, -t or -a options,"
           " e.g.: <interface:port>. Default is to not start any starts HTTP servers. Also, like the -a option, you may"
           " specify a port number by itself here and 127.0.0.1:<port> will be assumed."
           " This option may be specified more than once to bind to multiple interfaces and/or ports."),
           QString("[interface:]port"),
         },
         { { "b", "bitcoind" },
           QString("Specify a <hostname:port> to connect to the bitcoind rpc service. This is a required option, along"
           " with -u and -p. This hostname:port should be the same as you specified in your bitcoin.conf file"
           " under rpcbind= and rpcport=."),
           QString("hostname:port"),
         },
         { "bitcoind-tls",
           QString("If specified, connect to the remote bitcoind via HTTPS rather than the usual HTTP. Historically,"
                   " bitcoind supported only JSON-RPC over HTTP; however, some implementations such as bchd support"
                   " HTTPS. If you are using " APPNAME " with bchd, you either need to start bchd with the `notls`"
                   " option, or you need to specify this option to " APPNAME "."),
         },
         { { "u", "rpcuser" },
           QString("Specify a username to use for authenticating to bitcoind. This is a required option, along"
           " with -b and -p. This option should be the same username you specified in your bitcoind.conf file"
           " under rpcuser=. For security, you may omit this option from the command-line and use the %1"
           " environment variable instead (the CLI arg takes precedence if both are present).").arg(RPCUSER),
           QString("username"),
         },
         { { "p", "rpcpassword" },
           QString("Specify a password to use for authenticating to bitcoind. This is a required option, along"
           " with -b and -u. This option should be the same password you specified in your bitcoind.conf file"
           " under rpcpassword=. For security, you may omit this option from the command-line and use the"
           " %1 environment variable instead (the CLI arg takes precedence if both are present).").arg(RPCPASSWORD),
           QString("password"),
         },
         { { "d", "debug" },
           QString("Print extra verbose debug output. This is the default on debug builds. This is the opposite of -q."
           " (Specify this options twice to get network-level trace debug output.)"),
         },
         { { "q", "quiet" },
           QString("Suppress debug output. This is the default on release builds. This is the opposite of -d."),
         },
         { { "S", "syslog" },
           QString("Syslog mode. If on Unix, use the syslog() facility to produce log messages."
                   " This option currently has no effect on Windows."),
         },
         { { "C", "checkdb" },
           QString("If specified, database consistency will be checked thoroughly for sanity & integrity."
                   " Note that these checks are somewhat slow to perform and under normal operation are not necessary."),
         },
         { { "T", "polltime" },
           QString("The number of seconds for the bitcoind poll interval. Bitcoind is polled once every `polltime`"
                   " seconds to detect mempool and blockchain changes. This value must be at least 0.5 and cannot exceed"
                   " 30. If not specified, defaults to %1 seconds.").arg(Options::defaultPollTimeSecs),
           QString("polltime"), QString::number(Options::defaultPollTimeSecs)
         },
         {
           "ts-format",
           QString("Specify log timestamp format, one of: \"none\", \"uptime\", \"localtime\", or \"utc\". "
                   "If unspecified, default is \"localtime\" (previous versions of " APPNAME " always logged using "
                   "\"uptime\")."),
           QString("keyword"),
         },
         {
           "tls-disallow-deprecated",
           QString("If specified, restricts the TLS protocol used by the server to non-deprecated v1.2 or newer,"
                   " disallowing connections from clients requesting TLS v1.1 or earlier. This option applies to all"
                   " SSL and WSS ports server-wide."),
         },
         {
           "dump-sh",
           QString("*** This is an advanced debugging option ***   Dump script hashes. If specified, after the database"
                   " is loaded, all of the script hashes in the database will be written to outputfile as a JSON array."),
           QString("outputfile"),
         },
     };

    if (!registeredTests.empty()) {
        // add --test option if we have any registered tests
        QStringList tests;
        for (const auto & [name, func] : registeredTests)
            tests.append(name);
        allOptions.push_back({
            "test",
            QString("Run a test and exit. This option may be specified multiple times. Available tests: %1").arg(tests.join(", ")),
            QString("test")
        });
    }
    if (!registeredBenches.empty()) {
        // add --bench option if we have any registered benches
        QStringList benches;
        for (const auto & [name, func] : registeredBenches)
            benches.append(name);
        allOptions.push_back({
            "bench",
            QString("Run a benchmark and exit. This option may be specified multiple times. Available benchmarks: %1").arg(benches.join(", ")),
            QString("benchmark")
        });
    }

    parser.addOptions(allOptions);
    parser.addPositionalArgument("config", "Configuration file (optional).", "[config]");
    parser.process(*this);

    // handle possible --test or --bench args before doing anything else, since
    // those immediately exit the app if they do run.
    try {
        int setCtr = 0;
        if (!registeredTests.empty() && parser.isSet("test")) {
            ++setCtr;
            // process tests and exit if we take this branch
            for (const auto & tname : parser.values("test")) {
                auto it = registeredTests.find(tname);
                if (it == registeredTests.end())
                    throw BadArgs(QString("No such test: %1").arg(tname));
                qInfo() << "Running test: " << it->first << " ...";
                it->second();
            }
        }
        if (!registeredBenches.empty() && parser.isSet("bench")) {
            ++setCtr;
            // process benches and exit if we take this branch
            for (const auto & tname : parser.values("bench")) {
                auto it = registeredBenches.find(tname);
                if (it == registeredBenches.end())
                    throw BadArgs(QString("No such bench: %1").arg(tname));
                qInfo() << "Running benchmark: " << it->first << " ...";
                it->second();
            }
        }
        if (setCtr)
            std::exit(0);
    } catch (const std::exception & e) {
        // bench or test execution failed with an exception
        qCritical("Caught exception: %s", e.what());
        std::exit(1);
    }

    const auto checkSupportsSsl = [] {
        if (!QSslSocket::supportsSsl())
            throw InternalError("SSL support is not compiled and/or linked to this version. Cannot proceed with SSL support. Sorry!");
    };

    ConfigFile conf;

    // First, parse config file (if specified) -- We will take whatever it specified that matches the above options
    // but CLI args take precedence over config file options.
    if (auto posArgs = parser.positionalArguments(); !posArgs.isEmpty()) {
        if (posArgs.size() > 1)
            throw BadArgs("More than 1 config file was specified. Please specify at most 1 config file.");
        const auto file = posArgs.first();
        if (!conf.open(file))
            throw BadArgs(QString("Unable to open config file %1").arg(file));
        // ok, at this point the config file is slurped up and we can check it below
    }

    // first warn user about dupes
    for (const auto & opt : allOptions) {
        static const auto DupeMsg = [](const QString &arg) {
            qInfo() << "'" << arg << "' specified both via the CLI and the configuration file. The CLI arg will take precedence.";
        };
        for (const auto & name : opt.names()) {
            if (name.length() == 1) continue;
            if (conf.hasValue(name) && parser.isSet(name)) {
                DupeMsg(name);
                conf.remove(name);
            }
        }
    }

    if (parser.isSet("d") || conf.boolValue("debug")) {
        //if (config.hasValue("debug"))
        options->verboseDebug = true;
    }
    // check for -d -d
    if (auto found = parser.optionNames(); found.count("d") + found.count("debug") > 1)
        options->verboseTrace = true;
    else {
        // check for multiple debug = true in configFile (only present if no -d on CLI, otherwise config keys are deleted)
        const auto l = conf.values("debug");
        int ctr = 0;
        for (const auto & str : l)
            ctr += (str.toInt() || QStringList{{"yes","true","on",""}}.contains(str.toLower())) ? 1 : 0;
        if (ctr > 1)
            options->verboseTrace = true;
    }
    if (parser.isSet("q") || conf.boolValue("quiet")) options->verboseDebug = false;
    if (parser.isSet("S") || conf.boolValue("syslog")) options->syslogMode = true;
    if (parser.isSet("C") || conf.boolValue("checkdb")) options->doSlowDbChecks = true;
    // parse --polltime
    // note despite how confusingly the below line reads, the CLI parser value takes precedence over the conf file here.
    const QString polltimeStr = conf.value("polltime", parser.value("T"));
    if (bool ok; (options->pollTimeSecs = polltimeStr.toDouble(&ok)) < options->minPollTimeSecs
            || !ok || options->pollTimeSecs > options->maxPollTimeSecs) {
        throw BadArgs(QString("The 'polltime' option must be a numeric value in the range [%1, %2]").arg(options->minPollTimeSecs).arg(options->maxPollTimeSecs));
    }
    // make sure -b -p and -u all present and specified exactly once
    using ReqOptsList = std::list<std::tuple<QString, QString, const char *>>;
    for (const auto & opt : ReqOptsList({{"D", "datadir", nullptr},
                                         {"b", "bitcoind", nullptr},
                                         {"u", "rpcuser", RPCUSER},
                                         {"p", "rpcpassword", RPCPASSWORD},}))
    {
        const auto & [s, l, env] = opt;
        const bool cliIsSet = parser.isSet(s);
        const bool confIsSet = conf.hasValue(l);
        const auto envVar = env ? std::getenv(env) : nullptr;
        if ((cliIsSet || confIsSet) && envVar)
            qWarning() << "Warning: " << l <<  " is specified both via the " << (cliIsSet ? "CLI" : "config file")
                      << " and the environement (as " << env << "). The " << (cliIsSet ? "CLI arg" : "config file setting")
                      << " will take precendence.";
        if (((!cliIsSet && !confIsSet) || conf.value(l, parser.value(s)).isEmpty()) && (!env || !envVar))
            throw BadArgs(QString("Required option missing or empty: -%1 (--%2)%3").arg(s).arg(l).arg(env ? QString(" (or env var: %1)").arg(env) : ""));
        else if (parser.values(s).count() > 1)
            throw BadArgs(QString("Option specified multiple times: -%1 (--%2)").arg(s).arg(l));
        else if (conf.values(l).count() > 1)
            throw BadArgs(QString("This option cannot be specified multiple times in the config file: %1").arg(l));
    }
    static const auto parseHostnamePortPair = [](const QString &s, bool allowImplicitLoopback = false) -> QPair<QString, quint16> {
        constexpr auto parsePort = [](const QString & portStr) -> quint16 {
            bool ok;
            quint16 port = portStr.toUShort(&ok);
            if (!ok || port == 0)
                throw BadArgs(QString("Bad port: %1").arg(portStr));
            return port;
        };
        auto toks = s.split(":");
        constexpr const char *msg1 = "Malformed host:port spec. Please specify a string of the form <host>:<port>";
        if (const auto len = toks.length(); len < 2) {
            if (allowImplicitLoopback && len == 1)
                // this option allows bare port number with the implicit ipv4 127.0.0.1 -- try that (may throw if bad port number)
                return QPair<QString, quint16>{QHostAddress(QHostAddress::LocalHost).toString(), parsePort(toks.front())};
            throw BadArgs(msg1);
        }
        QString portStr = toks.last();
        toks.removeLast(); // pop off port
        QString hostStr = toks.join(':'); // rejoin on ':' in case it was IPv6 which is full of colons
        if (hostStr.isEmpty())
            throw BadArgs(msg1);
        return {hostStr, parsePort(portStr)};
    };
    const auto parseInterface = [&options = options](const QString &s, bool allowImplicitLoopback = false) -> Options::Interface {
        const auto pair = parseHostnamePortPair(s, allowImplicitLoopback);
        const auto & hostStr = pair.first;
        const auto port = pair.second;
        QHostAddress h(hostStr);
        if (h.isNull())
            throw BadArgs(QString("Bad interface address: %1").arg(hostStr));
        options->hasIPv6Listener = options->hasIPv6Listener || h.protocol() == QAbstractSocket::NetworkLayerProtocol::IPv6Protocol;
        return {h, port};
    };
    const auto parseInterfaces = [&parseInterface](decltype(Options::interfaces) & interfaces, const QStringList & l,
                                                   bool supportsLoopbackImplicitly = false) {
        // functor parses -i and -z options, puts results in 'interfaces' passed-in reference.
        interfaces.clear();
        for (const auto & s : l)
            interfaces.push_back(parseInterface(s, supportsLoopbackImplicitly));
    };

    // grab datadir, check it's good, create it if needed
    options->datadir = conf.value("datadir", parser.value("D"));
    QFileInfo fi(options->datadir);
    if (auto path = fi.canonicalFilePath(); fi.exists()) {
        if (!fi.isDir()) // was a file and not a directory
            throw BadArgs(QString("The specified path \"%1\" already exists but is not a directory").arg(path));
        if (!fi.isReadable() || !fi.isExecutable() || !fi.isWritable())
            throw BadArgs(QString("Bad permissions for path \"%1\" (must be readable, writable, and executable)").arg(path));
        Util::AsyncOnObject(this, [path]{ qDebug() << "datadir: " << path; }); // log this after return to event loop so it ends up in syslog (if -S mode)
    } else { // !exists
        if (!QDir().mkpath(options->datadir))
            throw BadArgs(QString("Unable to create directory: %1").arg(options->datadir));
        path = QFileInfo(options->datadir).canonicalFilePath();
        // log this after return to event loop so it ends up in syslog (in case user specified -S mode)
        Util::AsyncOnObject(this, [path]{ qDebug() << "datadir: Created directory " << path; });
    }

    // parse bitcoind - conf.value is always unset if parser.value is set, hence this strange constrcution below (parser.value takes precedence)
    options->bitcoind = parseHostnamePortPair(conf.value("bitcoind", parser.value("b")));
    // --bitcoind-tls
    if ((options->bitcoindUsesTls = parser.isSet("bitcoind-tls") || conf.boolValue("bitcoind-tls"))) {
        // check that Qt actually supports SSL since we now know that we require it to proceed
        checkSupportsSsl();
        Util::AsyncOnObject(this, []{ qDebug() << "config: bitcoind-tls = true"; });
    }
    // grab rpcuser
    options->rpcuser = conf.value("rpcuser", parser.isSet("u") ? parser.value("u") : std::getenv(RPCUSER));
    // grab rpcpass
    options->rpcpassword = conf.value("rpcpassword", parser.isSet("p") ? parser.value("p") : std::getenv(RPCPASSWORD));
    bool tcpIsDefault = true;
    // grab bind (listen) interfaces for TCP -- this hard-to-read code here looks at both conf.value and parser, but conf.value only has values if parser does not (CLI parser takes precedence).
    if (auto l = conf.hasValue("tcp") ? conf.values("tcp") : parser.values("t");  !l.isEmpty()) {
        parseInterfaces(options->interfaces, l);
        tcpIsDefault = false;
        if (!options->interfaces.isEmpty())
            // save default publicTcp we will report now -- note this may get reset() to !has_value() later in
            // this function if user explicitly specified public_tcp_port=0 in the config file.
            options->publicTcp = options->interfaces.front().second;
    }
    // grab bind (listen) interfaces for WS -- this hard-to-read code here looks at both conf.value and parser, but conf.value only has values if parser does not (CLI parser takes precedence).
    if (auto l = conf.hasValue("ws") ? conf.values("ws") : parser.values("w");  !l.isEmpty()) {
        parseInterfaces(options->wsInterfaces, l);
        if (tcpIsDefault) options->interfaces.clear(); // they had default tcp setup, clear the default since they did end up specifying at least 1 real interface to bind to
        if (!options->wsInterfaces.isEmpty())
            // save default publicWs we will report now -- note this may get reset() to !has_value() later in
            // this function if user explicitly specified public_ws_port=0 in the config file.
            options->publicWs = options->wsInterfaces.front().second;
    }
    // grab bind (listen) interfaces for WSS -- this hard-to-read code here looks at both conf.value and parser, but conf.value only has values if parser does not (CLI parser takes precedence).
    if (auto l = conf.hasValue("wss") ? conf.values("wss") : parser.values("W");  !l.isEmpty()) {
        parseInterfaces(options->wssInterfaces, l);
        if (tcpIsDefault) options->interfaces.clear(); // they had default tcp setup, clear the default since they did end up specifying at least 1 real interface to bind to
        if (!options->wssInterfaces.isEmpty())
            // save default publicWss we will report now -- note this may get reset() to !has_value() later in
            // this function if user explicitly specified public_wss_port=0 in the config file.
            options->publicWss = options->wssInterfaces.front().second;
    }
    // grab bind (listen) interfaces for SSL (again, apologies for this hard to read expression below -- same comments as above apply here)
    if (auto l = conf.hasValue("ssl") ? conf.values("ssl") : parser.values("s"); !l.isEmpty()) {
        parseInterfaces(options->sslInterfaces, l);
        if (tcpIsDefault) options->interfaces.clear(); // they had default tcp setup, clear the default since they did end up specifying at least 1 real interface to bind to
        if (!options->sslInterfaces.isEmpty())
            // save default publicSsl we will report now -- note this may get reset() to !has_value() later in
            // this function if user explicitly specified public_ssl_port=0 in the config file.
            options->publicSsl = options->sslInterfaces.front().second;
    }
    // if they had either SSL or WSS, grab and validate the cert & key
    if (const bool hasSSL = !options->sslInterfaces.isEmpty(), hasWSS = !options->wssInterfaces.isEmpty(); hasSSL || hasWSS) {
        // check that Qt actually supports SSL since we now know that we require it to proceed
        checkSupportsSsl();
        QString cert    = conf.value("cert",     parser.value("c")),
                key     = conf.value("key",      parser.value("k")),
                wssCert = conf.value("wss-cert", parser.value("wss-cert")),
                wssKey  = conf.value("wss-key",  parser.value("wss-key"));
        // ensure --cert/--key and --wss-cert/--wss-key pairs are both specified together (or not specified at all)
        for (const auto & [c, k, txt] : { std::tuple(cert, key, static_cast<const char *>("`cert` and `key`")),
                                          std::tuple(wssCert, wssKey, static_cast<const char *>("`wss-cert` and `wss-key`")) }) {
            if (std::tuple(c.isEmpty(), k.isEmpty()) != std::tuple(k.isEmpty(), c.isEmpty()))
                throw BadArgs(QString("%1 must both be specified").arg(txt));
        }
        // . <-- at this point, cert.isEmpty() and/or wssCert.isEmpty() are synonymous for both the cert/key pair being either empty or non-empty

        // The rules are:  Default to using -c and -k.  (both must be present)
        // If they are using wss, allow --wss-cert and --wss-key (both must be present)
        // If the only secure port is wss, allow -c/-k to be missing (use --wss-cert and --wss-key instead).
        // Otherwise if no cert and key combo, throw.
        if ( cert.isEmpty() && (hasSSL || wssCert.isEmpty()) )  {
            throw BadArgs(QString("%1 option requires both -c/--cert and -k/--key options be specified")
                          .arg(hasSSL ? "SSL" : "WSS"));
        }
        // if they are using the wss-port and wss-key options, they *better* have a wss port
        if ( !wssCert.isEmpty() && !hasWSS )
            throw BadArgs("wss-cert option specified but no WSS listening ports defined");

        if (cert.isEmpty() && !wssCert.isEmpty()) {
            // copy over wssCert/wssKey to cert/key, clear wssCert/wssKey
            cert = wssCert;  wssCert.clear();
            key  = wssKey;   wssKey.clear();
        }
        // sanity check
        if (cert.isEmpty() || key.isEmpty()) throw InternalError("Internal Error: cert and/or key is empty");

        // the below always either returns a good certInfo object, or throws on error
        options->certInfo = makeCertInfo(this, cert, key);
        if (!wssCert.isEmpty()) {
            if (wssKey.isEmpty()) throw InternalError("Internal Error: wss-key is empty"); // sanity check
            options->wssCertInfo = makeCertInfo(this, wssCert, wssKey);
        }
    }
    // stats port -- this supports <port> by itself as well
    parseInterfaces(options->statsInterfaces, conf.hasValue("stats")
                                              ? conf.values("stats")
                                              : parser.values("z"), true);
    // admin port -- this supports <port> by itself as well
    parseInterfaces(options->adminInterfaces, conf.hasValue("admin")
                                              ? conf.values("admin")
                                              : parser.values("a"), true);
    // warn user if any of the admin rpc services are on non-loopback
    for (const auto &iface : options->adminInterfaces) {
        if (!iface.first.isLoopback()) {
            // print the warning later when logger is up
            Util::AsyncOnObject(this, [iface]{
                qWarning() << "Warning: Binding admin RPC port to non-loopback interface " << iface.first.toString() << ":" << iface.second << " is not recommended. Please ensure that this port is not globally reachable from the internet.";
            });
        }
    }

    /// misc conf-only variables ...
    options->donationAddress = conf.value("donation", options->donationAddress).left(80); // the 80 character limit is in case the user specified a crazy long string, no need to send all of it -- it's probably invalid anyway.
    options->bannerFile = conf.value("banner", options->bannerFile);
    if (conf.hasValue("hostname"))
        options->hostName = conf.value("hostname");
    if (conf.hasValue("public_tcp_port")) {
        bool ok = false;
        int val = conf.intValue("public_tcp_port", -1, &ok);
        if (!ok || val < 0 || val > UINT16_MAX)
            throw BadArgs("public_tcp_port parse error: not an integer from 0 to 65535");
        if (!val) options->publicTcp.reset();
        else options->publicTcp = val;
    }
    if (conf.hasValue("public_ssl_port")) {
        bool ok = false;
        int val = conf.intValue("public_ssl_port", -1, &ok);
        if (!ok || val < 0 || val > UINT16_MAX)
            throw BadArgs("public_ssl_port parse error: not an integer from 0 to 65535");
        if (!val) options->publicSsl.reset();
        else options->publicSsl = val;
    }
    if (conf.hasValue("public_ws_port")) {
        bool ok = false;
        int val = conf.intValue("public_ws_port", -1, &ok);
        if (!ok || val < 0 || val > UINT16_MAX)
            throw BadArgs("public_ws_port parse error: not an integer from 0 to 65535");
        if (!val) options->publicWs.reset();
        else options->publicWs = val;
    }
    if (conf.hasValue("public_wss_port")) {
        bool ok = false;
        int val = conf.intValue("public_wss_port", -1, &ok);
        if (!ok || val < 0 || val > UINT16_MAX)
            throw BadArgs("public_wss_port parse error: not an integer from 0 to 65535");
        if (!val) options->publicWss.reset();
        else options->publicWss = val;
    }
    const auto ConfParseBool = [conf](const QString &key, bool def = false) -> bool {
        if (!conf.hasValue(key)) return def;
        const QString str = conf.value(key);
        return (str.toInt() || QStringList{{"yes","true","on",""}}.contains(str.toLower()));
    };
    options->peerDiscovery = ConfParseBool("peering", options->peerDiscovery);
    // set default first.. which is if we have hostName defined and peerDiscovery enabled
    options->peerAnnounceSelf = options->hostName.has_value() && options->peerDiscovery;
    // now set from conf fiel, specifying our default
    options->peerAnnounceSelf = ConfParseBool("announce", options->peerAnnounceSelf);
    // 'peering_enforce_unique_ip'
    options->peeringEnforceUniqueIPs = ConfParseBool("peering_enforce_unique_ip", options->peeringEnforceUniqueIPs);

    if (conf.hasValue("max_clients_per_ip")) {
        bool ok = false;
        options->maxClientsPerIP = conf.intValue("max_clients_per_ip", 0, &ok);
        if (const auto val = conf.value("max_clients_per_ip");  !ok && !val.isEmpty())
            throw BadArgs(QString("max_clients_per_ip parse error: cannot parse '%1' as an integer").arg(val));
        // log this later in case we are in syslog mode
        Util::AsyncOnObject(this, [this]{
            qDebug() << "config: max_clients_per_ip = "
                    << (options->maxClientsPerIP > 0 ? QString::number(options->maxClientsPerIP) : "Unlimited");
        });
    }
    if (conf.hasValue("subnets_to_exclude_from_per_ip_limits")) {
        options->subnetsExcludedFromPerIPLimits.clear();
        const auto sl = conf.value("subnets_to_exclude_from_per_ip_limits").split(",");
        QStringList parsed;
        for (const auto & s : sl) {
            if (s.isEmpty())
                continue;
            auto subnet = Options::Subnet::fromString(s);
            if (!subnet.isValid())
                throw BadArgs(QString("subnets_to_exclude_from_per_ip_limits: Failed to parse %1").arg(s));
            options->subnetsExcludedFromPerIPLimits.push_back(subnet);
            parsed.push_back(subnet.toString());
        }
        // log this later in case we are in syslog mode
        Util::AsyncOnObject(this, [parsed]{
            qDebug() << "config: subnets_to_exclude_from_per_ip_limits = " << (parsed.isEmpty() ? "None" : parsed.join(", "));
        });
    }
    if (conf.hasValue("max_history")) {
        bool ok;
        int mh = conf.intValue("max_history", -1, &ok);
        if (!ok || mh < options->maxHistoryMin || mh > options->maxHistoryMax)
            throw BadArgs(QString("max_history: bad value. Specify a value in the range [%1, %2]")
                          .arg(options->maxHistoryMin).arg(options->maxHistoryMax));
        options->maxHistory = mh;
        // log this later in case we are in syslog mode
        Util::AsyncOnObject(this, [mh]{ qDebug() << "config: max_history = " << mh; });
    }
    if (conf.hasValue("max_buffer")) {
        bool ok;
        int mb = conf.intValue("max_buffer", -1, &ok);
        if (!ok || !options->isMaxBufferSettingInBounds(mb))
            throw BadArgs(QString("max_buffer: bad value. Specify a value in the range [%1, %2]")
                          .arg(options->maxBufferMin).arg(options->maxBufferMax));
        options->maxBuffer.store( mb );
        // log this later in case we are in syslog mode
        Util::AsyncOnObject(this, [mb]{ qDebug() << "config: max_buffer = " << mb; });
    }
    // pick up 'workqueue' and 'worker_threads' optional conf params
    if (conf.hasValue("workqueue")) {
        bool ok;
        int val = conf.intValue("workqueue", 0, &ok);
        if (!ok || val < 10)
            throw BadArgs("workqueue: bad value. Specify an integer >= 10");
        if (!tpool->setExtantJobLimit(val))
            throw BadArgs(QString("workqueue: Unable to set workqueue to %1; SetExtantJobLimit returned false.").arg(val));
        options->workQueue = val; // save advisory value for stats(), etc code
        // log this later in case we are in syslog mode
        Util::AsyncOnObject(this, [this]{ qDebug() << "config: workqueue = " << tpool->extantJobLimit(); });
    } else
        options->workQueue = tpool->extantJobLimit(); // so stats() knows what was auto-configured
    if (conf.hasValue("worker_threads")) {
        bool ok;
        int val = conf.intValue("worker_threads", 0, &ok);
        if (!ok || val < 0)
            throw BadArgs("worker_threads: bad value. Specify an integer >= 0");
        if (val > int(Util::getNVirtualProcessors()))
            throw BadArgs(QString("worker_threads: specified value of %1 exceeds the detected number of virtual processors of %2")
                          .arg(val).arg(Util::getNVirtualProcessors()));
        if (val > 0 && !tpool->setMaxThreadCount(val))
            throw BadArgs(QString("worker_threads: Unable to set worker threads to %1").arg(val));
        options->workerThreads = val; // save advisory value for stats(), etc code
        // log this later in case we are in syslog mode
        Util::AsyncOnObject(this, [val,this]{ qDebug() << "config: worker_threads = " << val << " (configured: " << tpool->maxThreadCount() << ")"; });
    } else
        options->workerThreads = tpool->maxThreadCount(); // so stats() knows what was auto-configured
    // max_pending_connections
    if (conf.hasValue("max_pending_connections")) {
        bool ok;
        auto val = conf.intValue("max_pending_connections", options->maxPendingConnections, &ok);
        if (!ok || val < options->minMaxPendingConnections || val > options->maxMaxPendingConnections)
            throw BadArgs(QString("max_pending_connections: Please specify an integer in the range [%1, %2]")
                          .arg(options->minMaxPendingConnections).arg(options->maxMaxPendingConnections));
        options->maxPendingConnections = val;
        // log this later in case we are in syslog mode
        Util::AsyncOnObject(this, [val]{ qDebug() << "config: max_pending_connections = " << val; });
    }

    // handle tor-related params: tor_hostname, tor_banner, tor_tcp_port, tor_ssl_port, tor_proxy, tor_user, tor_pass
    if (const auto thn = conf.value("tor_hostname").toLower(); !thn.isEmpty()) {
        options->torHostName = thn;
        if (!thn.endsWith(".onion"))
            throw BadArgs(QString("Bad tor_hostname specified: must end with .onion: %1").arg(thn));
        Util::AsyncOnObject(this, [thn]{ qDebug() << "config: tor_hostname = " << thn; });
    }
    if (conf.hasValue("tor_banner")) {
        const auto banner = conf.value("tor_banner");
        options->torBannerFile = banner;
        Util::AsyncOnObject(this, [banner]{ qDebug() << "config: tor_banner = " << banner; });
    }
    if (conf.hasValue("tor_tcp_port")) {
        bool ok = false;
        int val = conf.intValue("tor_tcp_port", -1, &ok);
        if (!ok || val < 0 || val > UINT16_MAX)
            throw BadArgs("tor_tcp_port parse error: not an integer from 0 to 65535");
        if (!val) options->torTcp.reset();
        else {
            options->torTcp = val;
            Util::AsyncOnObject(this, [val]{ qDebug() << "config: tor_tcp_port = " << val; });
        }
    }
    if (conf.hasValue("tor_ssl_port")) {
        bool ok = false;
        int val = conf.intValue("tor_ssl_port", -1, &ok);
        if (!ok || val < 0 || val > UINT16_MAX)
            throw BadArgs("torc_ssl_port parse error: not an integer from 0 to 65535");
        if (!val) options->torSsl.reset();
        else {
            options->torSsl = val;
            Util::AsyncOnObject(this, [val]{ qDebug() << "config: tor_ssl_port = " << val; });
        }
    }
    if (conf.hasValue("tor_ws_port")) {
        bool ok = false;
        int val = conf.intValue("tor_ws_port", -1, &ok);
        if (!ok || val < 0 || val > UINT16_MAX)
            throw BadArgs("tor_ws_port parse error: not an integer from 0 to 65535");
        if (!val) options->torWs.reset();
        else {
            options->torWs = val;
            Util::AsyncOnObject(this, [val]{ qDebug() << "config: tor_ws_port = " << val; });
        }
    }
    if (conf.hasValue("tor_wss_port")) {
        bool ok = false;
        int val = conf.intValue("tor_wss_port", -1, &ok);
        if (!ok || val < 0 || val > UINT16_MAX)
            throw BadArgs("tor_wss_port parse error: not an integer from 0 to 65535");
        if (!val) options->torWss.reset();
        else {
            options->torWss = val;
            Util::AsyncOnObject(this, [val]{ qDebug() << "config: tor_wss_port = " << val; });
        }
    }
    if (conf.hasValue("tor_proxy")) {
        options->torProxy = parseInterface(conf.value("tor_proxy"), true); // may throw if bad
        Util::AsyncOnObject(this, [val=options->torProxy]{ qDebug() << "config: tor_proxy = " << val.first.toString() << ":" << val.second; });
    }
    if (conf.hasValue("tor_user")) {
        options->torUser = conf.value("tor_user");
        Util::AsyncOnObject(this, [val=options->torUser]{ qDebug() << "config: tor_user = " << val; });
    }
    if (conf.hasValue("tor_pass")) {
        options->torUser = conf.value("tor_pass");
        Util::AsyncOnObject(this, []{ qDebug() << "config: tor_pass = <hidden>"; });
    }
    // /Tor params

    if (conf.hasValue("bitcoind_throttle")) {
        const QStringList vals = conf.value("bitcoind_throttle").trimmed().simplified().split(QRegExp("\\W+"), Compat::SplitBehaviorSkipEmptyParts);
        constexpr size_t N = 3;
        std::array<int, N> parsed = {0,0,0};
        size_t i = 0;
        bool ok = false;
        for (const auto & val : vals) {
            if (i >= N) { ok = false; break; }
            parsed[i++] = val.toInt(&ok);
            if (!ok) break;
        }
        Options::BdReqThrottleParams p { parsed[0], parsed[1], parsed[2] };
        ok = ok && i == N && p.isValid();
        if (!ok)
            // failed to parse.. abort...
            throw BadArgs("Failed to parse \"bitcoind_throttle\" -- out of range or invalid format. Please specify 3 positive integers in range.");
        options->bdReqThrottleParams.store(p);
        // log this later in case we are in syslog mode
        Util::AsyncOnObject(this, [p]{ qDebug() << "config: bitcoind_throttle = " << QString("(hi: %1, lo: %2, decay: %3)").arg(p.hi).arg(p.lo).arg(p.decay); });
    }
    if (conf.hasValue("max_subs_per_ip")) {
        bool ok;
        const int64_t subs = conf.int64Value("max_subs_per_ip", -1, &ok);
        if (!ok || !options->isMaxSubsPerIPSettingInBounds(subs))
            throw BadArgs(QString("max_subs_per_ip: bad value. Specify a value in the range [%1, %2]")
                          .arg(options->maxSubsPerIPMin).arg(options->maxSubsPerIPMax));
        options->maxSubsPerIP = subs;
        // log this later in case we are in syslog mode
        Util::AsyncOnObject(this, [subs]{ qDebug() << "config: max_subs_per_ip = " << subs; });
    }
    if (conf.hasValue("max_subs")) {
        bool ok;
        const int64_t subs = conf.int64Value("max_subs", -1, &ok);
        if (!ok || !options->isMaxSubsGloballySettingInBounds(subs))
            throw BadArgs(QString("max_subs: bad value. Specify a value in the range [%1, %2]")
                          .arg(options->maxSubsGloballyMin).arg(options->maxSubsGloballyMax));
        options->maxSubsGlobally = subs;
        // log this later in case we are in syslog mode
        Util::AsyncOnObject(this, [subs]{ qDebug() << "config: max_subs = " << subs; });
    }

    // DB options
    if (conf.hasValue("db_max_open_files")) {
        bool ok;
        const int64_t mof = conf.int64Value("db_max_open_files", 0, &ok);
        if (!ok || !options->db.isMaxOpenFilesSettingInBounds(mof))
            throw BadArgs(QString("db_max_open_files: bad value. Specify a value in the range [%1, %2] or -1.")
                          .arg(options->db.maxOpenFilesMin).arg(options->db.maxOpenFilesMax));
        options->db.maxOpenFiles = int(mof);
        // log this later in case we are in syslog mode
        Util::AsyncOnObject(this, [mof]{ qDebug() << "config: db_max_open_files = " << mof; });
    }
    if (conf.hasValue("db_keep_log_file_num")) {
        bool ok;
        const int64_t klfn = conf.int64Value("db_keep_log_file_num", -1, &ok);
        if (!ok || !options->db.isKeepLogFileNumInBounds(klfn))
            throw BadArgs(QString("db_keep_log_file_num: bad value. Specify a value in the range [%1, %2]")
                          .arg(options->db.minKeepLogFileNum).arg(options->db.maxKeepLogFileNum));
        options->db.keepLogFileNum = unsigned(klfn);
        // log this later in case we are in syslog mode
        Util::AsyncOnObject(this, [klfn]{ qDebug() << "config: db_keep_log_file_num = " << klfn; });
    }

    // warn user that no hostname was specified if they have peerDiscover turned on
    if (!options->hostName.has_value() && options->peerDiscovery && options->peerAnnounceSelf) {
        // do this when we return to event loop in case user is logging to -S (so it appears in syslog which gets set up after we return)
        Util::AsyncOnObject(this, []{
            qWarning() << "Warning: No 'hostname' variable defined in configuration. This server may not be peer-discoverable.";
        });
    }

    // parse --ts-format or ts-format= from conf (ts_format also supported from conf)
    if (auto fmt = parser.value("ts-format");
            !fmt.isEmpty() || !(fmt = conf.value("ts-format")).isEmpty() || !(fmt = conf.value("ts_format")).isEmpty()) {
        fmt = fmt.toLower().trimmed();
        if (fmt == "uptime" || fmt == "abs" || fmt == "abstime")
            options->logTimestampMode = Options::LogTimestampMode::Uptime;
        else if (fmt.startsWith("local"))
            options->logTimestampMode = Options::LogTimestampMode::Local;
        else if (fmt == "utc")
            options->logTimestampMode = Options::LogTimestampMode::UTC;
        else if (fmt == "none")
            options->logTimestampMode = Options::LogTimestampMode::None;
        else
            throw BadArgs(QString("ts-format: unrecognized value \"%1\"").arg(fmt));
        Util::AsyncOnObject(this, [this]{ DebugM("config: ts-format = ", options->logTimestampModeString()); });
    }
#ifdef Q_OS_UNIX
    else if (options->syslogMode) {
        options->logTimestampMode = Options::LogTimestampMode::None;
        Util::AsyncOnObject(this, []{ DebugM("syslog mode enabled, defaulting to \"--ts-format none\""); });
    }
#endif

    // --tls-disallow-deprecated from CLI and/or tls-disallow-deprecated from conf
    if (parser.isSet("tls-disallow-deprecated") || conf.boolValue("tls-disallow-deprecated")) {
        options->tlsDisallowDeprecated = true;
        Util::AsyncOnObject(this, []{ qInfo() << "TLS restricted to non-deprecated versions (version 1.2 or above)"; });
    }

    // parse --dump-*
    if (const auto outFile = parser.value("dump-sh"); !outFile.isEmpty()) {
        options->dumpScriptHashes = outFile; // we do no checking here, but Controller::startup will throw BadArgs if it cannot open this file for writing.
    }
}

/*static*/
Options::CertInfo App::makeCertInfo(const QObject *context, const QString &cert, const QString &key)
{
    Options::CertInfo ret;

    if (!context)
        throw InternalError("`context` may not be nullptr! FIXME!");
    if (!QFile::exists(cert))
        throw BadArgs(QString("Cert file not found: %1").arg(cert));
    if (!QFile::exists(key))
        throw BadArgs(QString("Key file not found: %1").arg(key));

    QFile certf(cert), keyf(key);
    if (!certf.open(QIODevice::ReadOnly))
        throw BadArgs(QString("Unable to open cert file %1: %2").arg(cert).arg(certf.errorString()));
    if (!keyf.open(QIODevice::ReadOnly))
        throw BadArgs(QString("Unable to open key file %1: %2").arg(key).arg(keyf.errorString()));

    ret.cert = QSslCertificate(&certf, QSsl::EncodingFormat::Pem);
    // proble key algorithm by trying all the algorithms Qt supports
    for (auto algo : {QSsl::KeyAlgorithm::Rsa, QSsl::KeyAlgorithm::Ec, QSsl::KeyAlgorithm::Dsa,
#if (QT_VERSION >= QT_VERSION_CHECK(5, 13, 0))
             // This was added in Qt 5.13+
         QSsl::KeyAlgorithm::Dh,
#endif
                     }) {
        keyf.seek(0);
        ret.key = QSslKey(&keyf, algo, QSsl::EncodingFormat::Pem);
        if (!ret.key.isNull())
            break;
    }
    // check key is ok
    if (ret.key.isNull()) {
        throw BadArgs(QString("Unable to read private key from %1. Please make sure the file is readable and "
                              "contains an RSA, DSA, EC, or DH private key in PEM format.").arg(key));
    } else if (ret.key.algorithm() == QSsl::KeyAlgorithm::Ec && QSslConfiguration::supportedEllipticCurves().isEmpty()) {
        throw BadArgs(QString("Private key `%1` is an elliptic curve key, however this Qt installation lacks"
                              " elliptic curve support. Please recompile and link Qt against the OpenSSL library"
                              " in order to enable elliptic curve support in Qt.").arg(key));
    }
    ret.file = cert; // this is only used for /stats port advisory info
    ret.keyFile = key; // this is only used for /stats port advisory info
    if (ret.cert.isNull())
        throw BadArgs(QString("Unable to read ssl certificate from %1. Please make sure the file is readable and "
                              "contains a valid certificate in PEM format.").arg(cert));
    else {
        if (!ret.cert.isSelfSigned()) {
            certf.seek(0);
            ret.certChain = QSslCertificate::fromDevice(&certf, QSsl::EncodingFormat::Pem);
            if (ret.certChain.size() < 2)
                throw BadArgs(QString("File '%1' does not appear to be a full certificate chain.\n"
                                      "Please make sure your CA signed certificate is the fullchain.pem file.")
                              .arg(cert));
        }
        Util::AsyncOnObject(context, [ret]{
            // We do this logging later. This is to ensure that it ends up in the syslog if user specified -S
            QString name;
#if (QT_VERSION >= QT_VERSION_CHECK(5, 12, 0))
            // Was added Qt 5.12+
            name = ret.cert.subjectDisplayName();
#else
            name = ret.cert.subjectInfo(QSslCertificate::Organization).join(", ");
#endif
            qInfo() << "Loaded SSL certificate: " << name << " "
                  << ret.cert.subjectInfo(QSslCertificate::SubjectInfo::EmailAddress).join(",")
                  //<< " self-signed: " << (options->sslCert.isSelfSigned() ? "YES" : "NO")
                  << " expires: " << (ret.cert.expiryDate().toString("ddd MMMM d yyyy hh:mm:ss"));
            if (Debug::isEnabled()) {
                QString cipherStr;
                for (const auto & ciph : QSslConfiguration::supportedCiphers()) {
                    if (!cipherStr.isEmpty()) cipherStr += ", ";
                    cipherStr += ciph.name();
                }
                if (cipherStr.isEmpty()) cipherStr = "(None)";
                qDebug() << "Supported ciphers: " << cipherStr;
                QString curvesStr;
                for (const auto & curve : QSslConfiguration::supportedEllipticCurves()) {
                    if (!curvesStr.isEmpty()) curvesStr += ", ";
                    curvesStr += curve.longName();
                }
                if (curvesStr.isEmpty()) curvesStr = "(None)";
                qDebug() << "Supported curves: " << curvesStr;
            }
        });
    }
    static const auto KeyAlgoStr = [](QSsl::KeyAlgorithm a) {
        switch (a) {
#if (QT_VERSION >= QT_VERSION_CHECK(5, 13, 0))
        // This was added in Qt 5.13+
        case QSsl::KeyAlgorithm::Dh: return "DH";
#endif
        case QSsl::KeyAlgorithm::Ec: return "EC";
        case QSsl::KeyAlgorithm::Dsa: return "DSA";
        case QSsl::KeyAlgorithm::Rsa: return "RSA";
        default: return "Other";
        }
    };
    Util::AsyncOnObject(context, [ret]{
        // We do this logging later. This is to ensure that it ends up in the syslog if user specified -S
        const auto algo = ret.key.algorithm();
        const auto algoName = KeyAlgoStr(algo);
        const auto keyTypeName = (ret.key.type() == QSsl::KeyType::PrivateKey ? "private" : "public");
        qInfo() << "Loaded key type: " << keyTypeName << " algorithm: " << algoName;
        if (algo != QSsl::KeyAlgorithm::Rsa)
            qWarning() << "Warning: " << algoName << " key support is experimental."
                      << " Please consider switching your SSL certificate and key to use 2048-bit RSA.";
    });

    return ret;
}

namespace {
    auto ParseParams(const SimpleHttpServer::Request &req) -> StatsMixin::StatsParams {
        StatsMixin::StatsParams params;
        const auto nvps = req.queryString.split('&');
        for (const auto & nvp : nvps) {
            auto nv = nvp.split('=');
            if (nv.size() == 2)
                params[nv.front()] = nv.back();
        }
        return params;
    }
}

void App::start_httpServer(const Options::Interface &iface)
{
    std::shared_ptr<SimpleHttpServer> server(new SimpleHttpServer(iface.first, iface.second, 16384));
    httpServers.push_back(server);
    server->tryStart(); // may throw, waits for server to start
    server->set404Message("Error: Unknown endpoint. /stats & /debug are the only valid endpoint I understand.\r\n");
    static const auto CRLF = QByteArrayLiteral("\r\n");
    server->addEndpoint("/stats",[this](SimpleHttpServer::Request &req){
        req.response.contentType = "application/json; charset=utf-8";
        auto stats = controller->statsSafe();
        stats = stats.isNull() ? QVariantList{QVariant()} : stats;
        req.response.data = Json::toUtf8(stats, false) + CRLF; // may throw -- calling code will handle exception
    });
    server->addEndpoint("/debug",[this](SimpleHttpServer::Request &req){
        req.response.contentType = "application/json; charset=utf-8";
        const auto params = ParseParams(req);
        auto stats = controller->debugSafe(params);
        stats = stats.isNull() ? QVariantList{QVariant()} : stats;
        req.response.data = Json::toUtf8(stats, false) + CRLF; // may throw -- caller will handle exception
    });
}

/* static */
void App::customMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    // suppressions
    if ( msg.contains(QStringLiteral("QSslCertificate::isSelfSigned"))
         || msg.contains(QStringLiteral("Type conversion already registered")))
        return;
    // /suppressions

    const QByteArray umsg = msg.toUtf8();
    const char *file = context.file ? context.file : "";
    const char *function = context.function ? context.function : "";

    switch (type) {
    case QtDebugMsg:
        DebugM("[Qt] ", umsg.constData(), " (", file, ":", context.line, ", ", function, ")");
        break;
    case QtInfoMsg:
        Log("[Qt] %s (%s:%d, %s)", umsg.constData(), file, context.line, function);
        break;
    case QtCriticalMsg:
        Error("[Qt Critical] %s (%s:%d, %s)", umsg.constData(), file, context.line, function);
        break;
    case QtFatalMsg:
        Error("[Qt Fatal] %s (%s:%d, %s)", umsg.constData(), file, context.line, function);
        break;
    }
}

void App::miscPreAppFixups()
{
    if(qEnvironmentVariableIsSet("JOURNAL_STREAM")) {
        qputenv("QT_LOGGING_TO_CONSOLE", QByteArray("0"));
    } else {
        qputenv("QT_MESSAGE_PATTERN", "[%{time yyyy-MM-dd hh:mm:ss.zzz}] %{message}");
        //qInstallMessageHandler(customMessageHandler);
    }
#ifdef Q_OS_DARWIN
    // workaround for annoying macos keychain access prompt. see: https://doc.qt.io/qt-5/qsslsocket.html#setLocalCertificate
    setenv("QT_SSL_USE_TEMPORARY_KEYCHAIN", "1", 1);
#endif
}

void App::on_setVerboseDebug(bool b)
{
    options->verboseDebug = b;
    if (!b)
        options->verboseTrace = false;
}
void App::on_setVerboseTrace(bool b)
{
    options->verboseTrace = b;
    if (b)
        options->verboseDebug = true;
}

void App::on_requestMaxBufferChange(int m)
{
    if (Options::isMaxBufferSettingInBounds(m))
        options->maxBuffer.store( Options::clampMaxBufferSetting(m) );
    else
        qWarning() << __func__ << ": " << m << " is out of range, ignoring new max_buffer setting";
}

void App::on_bitcoindThrottleParamsChange(int hi, int lo, int decay)
{
    Options::BdReqThrottleParams p{hi, lo, decay};
    if (p.isValid())
        options->bdReqThrottleParams.store(p);
    else
        qWarning() << __func__ << ": arguments out of range, ignoring new bitcoind_throttle setting";
}

/* static */ std::map<QString, std::function<void()>> App::registeredTests, App::registeredBenches;
/* static */
void App::registerTestBenchCommon(const char *fname, const char *brief, NameFuncMap &map,
                                  const NameFuncMap::key_type &name, const NameFuncMap::mapped_type &func)
{
    if (_globalInstance) {
        qCritical() << fname << " cannot be called after the app has already started!"
                << " Ignoring request to register " << brief << " \"" << name << "\"";
        return;
    }
    const auto & [_, inserted] = map.insert({name, func});
    if (!inserted)
        qCritical() << fname << ": ignoring duplicate " << brief << " \"" << name << "\"";
}
/* static */
auto App::registerTest(const QString &name, const std::function<void()> &func) -> RegisteredTest
{
    registerTestBenchCommon(__func__, "test", registeredTests, name, func);
    return {};
}
/* static */
auto App::registerBench(const QString &name, const std::function<void()> &func) -> RegisteredBench
{
    registerTestBenchCommon(__func__, "bench", registeredBenches, name, func);
    return {};
}

void App::setCLocale()
{
    try {
        QLocale::setDefault(QLocale::c());
        std::setlocale(LC_ALL, "C");
        std::setlocale(LC_NUMERIC, "C");
        std::locale::global(std::locale::classic());
    } catch (const std::exception &e) {
        qWarning() << "Failed to set \"C\" locale: " << e.what();
    }
}
