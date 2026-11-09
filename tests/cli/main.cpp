// Regression tests for commandline::Parser as used by sources/hyperhdr/main.cpp.
//
// The registrations below mirror main(): flags are BooleanOption, --userdata is a
// value Option. Every Qt warning is captured; the original defect produced
// "QCommandLineParser: option not expecting values" once per registered flag.

#include <commandline/Parser.h>

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>
#include <memory>

using namespace commandline;

namespace
{
	QStringList g_messages;
	int g_failures = 0;

	void captureMessages(QtMsgType type, const QMessageLogContext&, const QString& msg)
	{
		if (type != QtDebugMsg && type != QtInfoMsg)
			g_messages.append(msg);
	}

	void check(bool condition, const char* what)
	{
		if (!condition)
		{
			++g_failures;
			std::fprintf(stderr, "FAIL: %s\n", what);
		}
	}

	struct Daemon
	{
		std::unique_ptr<Parser> parser = std::make_unique<Parser>("HyperHDR Daemon");
		BooleanOption* version = nullptr;
		Option* userData = nullptr;
		BooleanOption* silent = nullptr;
		BooleanOption* verbose = nullptr;
		BooleanOption* debug = nullptr;
		BooleanOption* wait = nullptr;

		explicit Daemon(const QString& defaultDataPath = QStringLiteral("/home/test/.hyperhdr"))
		{
			Parser& p = *parser;
			p.addHelpOption();
			version = &p.add<BooleanOption>(0x0, "version", "Show version information");
			userData = &p.add<Option>('u', "userdata", "Overwrite user data path, defaults to home directory of current user (%1)", defaultDataPath);
			p.add<BooleanOption>(0x0, "resetPassword", "Lost your password? Reset it with this option back to 'hyperhdr'");
			p.add<BooleanOption>(0x0, "deleteDatabase", "Start all over? This Option will delete the database");
			silent = &p.add<BooleanOption>('s', "silent", "Do not print any outputs");
			verbose = &p.add<BooleanOption>('v', "verbose", "Increase verbosity");
			debug = &p.add<BooleanOption>('d', "debug", "Show debug messages");
			p.add<BooleanOption>(0x0, "pipewire", "Force pipewire screen grabber if it's available");
			p.add<BooleanOption>(0x0, "desktop", "Show systray on desktop");
			p.add<BooleanOption>(0x0, "service", "Force HyperHdr to start as console service");
			wait = &p.addHidden<BooleanOption>(0x0, "wait-hyperhdr", "Do not exit if other HyperHdr instances are running, wait them to finish");
		}

		bool parse(QStringList args)
		{
			args.prepend(QStringLiteral("hyperhdr"));
			return parser->parse(args);
		}
	};

	void expectNoWarnings(const char* what)
	{
		if (!g_messages.isEmpty())
		{
			++g_failures;
			std::fprintf(stderr, "FAIL: %s produced %d Qt warning(s):\n", what, int(g_messages.size()));
			for (const QString& m : std::as_const(g_messages))
				std::fprintf(stderr, "    %s\n", qPrintable(m));
		}
		g_messages.clear();
	}
}

int main(int argc, char** argv)
{
	QCoreApplication app(argc, argv);
	qInstallMessageHandler(captureMessages);

	{
		Daemon d;
		check(d.parse({}), "no arguments parse");
		check(!d.parser->isSet(*d.version) && !d.parser->isSet(*d.debug), "no flags set by default");
		check(d.userData->value(*d.parser) == "/home/test/.hyperhdr", "--userdata default value");
		expectNoWarnings("no arguments");
	}
	{
		Daemon d;
		check(d.parse({ "--version" }), "--version parses");
		check(d.parser->isSet(*d.version), "--version is set");
		check(!d.parser->isSet(*d.wait), "--version does not set hidden wait flag");
		expectNoWarnings("--version");
	}
	{
		Daemon d;
		check(d.parse({ "--help" }), "--help parses");
		check(d.parser->isSet(QStringLiteral("help")), "--help is set");
		check(d.parser->helpText().contains("--userdata <userdata>"), "help shows --userdata takes a value");
		check(d.parser->helpText().contains("--debug") && !d.parser->helpText().contains("--debug <"), "help shows --debug as a flag");
		check(!d.parser->helpText().contains("wait-hyperhdr"), "hidden option absent from help");
		expectNoWarnings("--help");
	}
	for (const char* arg : { "--debug", "-d" })
	{
		Daemon d;
		check(d.parse({ arg }), "debug flag parses");
		check(d.parser->isSet(*d.debug), "debug flag is set");
		check(!d.parser->isSet(*d.verbose), "debug flag does not set verbose");
		check(d.parser->positionalArguments().isEmpty(), "debug flag consumes no positional value");
		expectNoWarnings(arg);
	}
	for (const char* arg : { "--verbose", "-v" })
	{
		Daemon d;
		check(d.parse({ arg }), "verbose flag parses");
		check(d.parser->isSet(*d.verbose), "verbose flag is set");
		expectNoWarnings(arg);
	}
	{
		// A flag followed by a word must leave the word positional, not consume it.
		Daemon d;
		check(d.parse({ "--debug", "extra" }), "--debug extra parses");
		check(d.parser->positionalArguments() == QStringList{ "extra" }, "flag does not consume the next word");
		expectNoWarnings("--debug extra");
	}
	for (const QStringList& args : { QStringList{ "--userdata", "/tmp/hh" }, QStringList{ "--userdata=/tmp/hh" }, QStringList{ "-u", "/tmp/hh" } })
	{
		Daemon d;
		check(d.parse(args), "--userdata parses");
		check(d.parser->isSet(*d.userData), "--userdata is set");
		check(d.userData->value(*d.parser) == "/tmp/hh", "--userdata value");
		expectNoWarnings(qPrintable(args.join(' ')));
	}
	{
		Daemon d;
		check(d.parse({ "-d", "-v", "-u", "/tmp/hh", "--version" }), "combined options parse");
		check(d.parser->isSet(*d.debug) && d.parser->isSet(*d.verbose) && d.parser->isSet(*d.version), "combined flags set");
		check(d.userData->value(*d.parser) == "/tmp/hh", "combined --userdata value");
		expectNoWarnings("combined options");
	}
	{
		Daemon d;
		check(!d.parse({ "--userdata" }), "--userdata without a value is rejected");
		check(d.parser->errorText().contains("Missing value"), "missing value error text");
		g_messages.clear();
	}
	{
		Daemon d;
		check(!d.parse({ "--not-an-option" }), "unknown option is rejected");
		check(d.parser->errorText().contains("not-an-option"), "unknown option error names the option");
		g_messages.clear();
	}
	{
		// Explicit values on flags must be refused rather than silently accepted.
		Daemon d;
		check(!d.parse({ "--debug=1" }), "value on a flag is rejected");
		g_messages.clear();
	}

	if (g_failures == 0)
		std::printf("commandline parser: all checks passed\n");
	return g_failures == 0 ? 0 : 1;
}
