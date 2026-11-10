#pragma once

#include "core/Result.h"

#include <QStringList>

namespace mrd {
struct SvdDevice;
}

namespace mrd::cli {

/// Parsed command line (see --help for the full list).
struct Options
{
    bool gui = true;
    bool selftest = false;
    bool probeInfo = false;
    bool printHelp = false;
    bool printVersion = false;
    /// --autoconnect: connect on GUI startup
    bool autoConnect = false;
    /// --svd <path> / --svd=<path>
    QString svdPath;
    /// --render <dir>: offscreen PNG snapshots (used with --selftest)
    QString renderDir;
    /// --device <name>: override the chip name (`probe-rs --chip`)
    QString deviceName;
    /// --backend jlink|probe-rs
    QString backend;
    /// --interface swd|jtag
    QString interfaceName;
    /// --speed <kHz>
    int speedKHz = 0;
    /// --port <PA|PB|...>: port grid to snapshot with --render
    QString portName;
    /// --log <path>: log file (also enabled by default in AppDataLocation)
    QString logPath;
    QStringList errors;
};

Options parse(int argc, char *argv[]);

QString helpText();

/// `--selftest`: parse the SVD, run consistency checks, optionally render PNGs.
int runSelftest(const Options &options);

/// `--probe-info`: load the J-Link DLL, print version/firmware/SN, try to connect.
int runProbeInfo(const Options &options);

/// Offscreen snapshots of the register block view, bit-field strip, port grid
/// and package view. Written into `options.renderDir`.
Status renderSnapshots(const Options &options, const SvdDevice &device);

} // namespace mrd::cli
