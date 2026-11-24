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
    /// --bench: time the per-poll refresh path (no hardware needed)
    bool bench = false;
    /// --verbose: also log the per-step details (now DEBUG level).
    bool verbose = false;
    bool probeInfo = false;
    /// --bench-bus <seconds>: measure whether polling every variable slows the target
    /// (compares its own tick rate idle vs under read load).
    int benchBus = 0;
    /// --bench-read: time one full read of every static variable through the DLL and
    /// check whether the core keeps running (DHCSR.C_HALT + its own tick counter).
    bool benchRead = false;
    /// --read-var <name>: with --symbols + --probe-info, read that variable from the
    /// target and print its values (proves the snapshot path on real hardware).
    QString readVar;
    /// --bench-scope: time the variable panel instead of listing variables
    bool benchScope = false;
    /// --burn <path>: analyse a firmware image and (with --burn-commit) write it.
    QString burnPath;
    /// --burn-commit: actually erase and program. Without it, --burn only reports.
    bool burnCommit = false;
    /// --burn-force: erase the image's range first, so the write really happens even
    /// when the target already holds the same bytes.
    bool burnForce = false;
    /// --bootloader <path>: merge this .axf/.elf/.hex in front of the application.
    QString bootloaderPath;
    /// Address overrides for the burn layout; empty means "derive".
    QString bootloaderBase;
    QString appBase;
    QString overallStart;
    QString overallEnd;
    /// --dump-flash <path>: read the target's Flash into a .bin (read-only diagnostic).
    QString dumpFlashPath;
    /// --dump-base <addr> / --dump-len <bytes>: what to read for --dump-flash.
    QString dumpFlashBase;
    QString dumpFlashLength;
    bool printHelp = false;
    bool printVersion = false;
    /// --svd <path> / --svd=<path>
    QString svdPath;
    /// --render <dir>: offscreen PNG snapshots (used with --selftest)
    QString renderDir;
    /// --device <name>: override the chip name
    QString deviceName;
    /// --interface swd|jtag
    QString interfaceName;
    /// --speed <kHz>
    int speedKHz = 0;
    /// --port <PA|PB|...>: port grid to snapshot with --render
    QString portName;
    /// --log <path>: log file (also enabled by default in AppDataLocation)
    QString logPath;
    /// --symbols <path>: list the variables an .axf/.elf exposes (no hardware).
    QString symbolPath;
    QStringList errors;
};

Options parse(int argc, char *argv[]);

QString helpText();

/// `--selftest`: parse the SVD, run consistency checks, optionally render PNGs.
int runSelftest(const Options &options);

/// Time the GUI refresh path per poll; prints ms per stage.
int runBench(const Options &options);

/// `--symbols <file>`: parse a Keil .axf / ELF and list its sampleable variables.
/// Needs no hardware, so it doubles as a way to check a symbol file before
/// connecting.
int runSymbols(const Options &options);

/// --bench-scope <file>: time parsing, tree build and widget materialisation.
int runBenchScope(const Options &options);

/// `--probe-info`: load the J-Link DLL, print version/firmware/SN, try to connect.
int runProbeInfo(const Options &options);

/// Offscreen snapshots of the register block view, bit-field strip, port grid
/// and package view. Written into `options.renderDir`.
Status renderSnapshots(const Options &options, const SvdDevice &device);

} // namespace mrd::cli
