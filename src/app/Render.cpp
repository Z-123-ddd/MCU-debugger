#include "app/Cli.h"

#include "core/GpioCodec.h"
#include "core/HexUtil.h"
#include "core/Logger.h"
#include "core/ResourcePaths.h"
#include "core/SvdTypes.h"
#include "ui/BitFieldStrip.h"
#include "core/DeviceIdentifier.h"
#include "core/DeviceTreeScanner.h"
#include "ui/PackageView.h"
#include "ui/PeripheralTree.h"
#include "ui/PortGridView.h"
#include "ui/RegisterBlockView.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QImage>
#include <QTreeWidget>
#include <QPixmap>
#include <QWidget>

namespace mrd::cli {

namespace {

bool saveWidget(QWidget *widget, const QString &path, QString *error = nullptr,
                const QSize &minimum = QSize(900, 500), double minRatio = 0.02,
                double *outRatio = nullptr)
{
    const QSize size = widget->sizeHint().expandedTo(widget->minimumSizeHint()).expandedTo(minimum);
    widget->resize(size);
    QPixmap pixmap(size);
    pixmap.fill(Theme::current().background);
    QPainter painter(&pixmap);
    widget->render(&painter);
    painter.end();

    // Guard against "successfully saved but blank" renders: require a
    // meaningful amount of non-background pixels.
    const QImage image = pixmap.toImage();
    const QRgb background = Theme::current().background.rgb();
    qint64 painted = 0;
    for (int y = 0; y < image.height(); y += 2) {
        for (int x = 0; x < image.width(); x += 2) {
            if (image.pixel(x, y) != background)
                ++painted;
        }
    }
    const qint64 sampled = qint64(image.width() / 2) * qint64(image.height() / 2);
    const double ratio = sampled > 0 ? double(painted) / double(sampled) : 0.0;
    if (outRatio)
        *outRatio = ratio;
    if (ratio < minRatio) {
        if (error)
            *error = QStringLiteral("%1 渲染结果几乎为空（非背景像素 %2%）")
                         .arg(path)
                         .arg(QString::number(ratio * 100.0, 'f', 1));
        return false;
    }
    return pixmap.save(path);
}

const SvdPeripheral *pickPeripheral(const SvdDevice &device)
{
    // Prefer a peripheral with many fields: it exercises the layout the most.
    const SvdPeripheral *best = nullptr;
    int bestFields = -1;
    for (const auto &peripheral : device.peripherals) {
        int fields = 0;
        for (const auto &reg : peripheral.registers)
            fields += reg.fields.size();
        if (fields > bestFields) {
            bestFields = fields;
            best = &peripheral;
        }
    }
    return best;
}

const SvdRegister *pickRegister(const SvdPeripheral &peripheral)
{
    const SvdRegister *best = nullptr;
    int bestFields = -1;
    for (const auto &reg : peripheral.registers) {
        if (reg.fields.size() > bestFields) {
            bestFields = reg.fields.size();
            best = &reg;
        }
    }
    return best;
}

} // namespace

Status renderSnapshots(const Options &options, const SvdDevice &device)
{
    const QString dir = options.renderDir;
    if (!QDir().mkpath(dir))
        return Status::failure(QStringLiteral("无法创建输出目录: %1").arg(dir));

    const SvdPeripheral *peripheral = pickPeripheral(device);
    if (!peripheral)
        return Status::failure(QStringLiteral("SVD 中没有外设，无法渲染"));

    const SvdRegister *reg = pickRegister(*peripheral);
    if (!reg)
        return Status::failure(QStringLiteral("外设 %1 中没有寄存器").arg(peripheral->name));

    QStringList written;

    // --- register block layout ---
    RegisterBlockView blockView;
    blockView.setPeripheral(peripheral);
    QHash<quint32, quint32> values;
    for (const auto &r : peripheral->registers)
        values.insert(peripheral->baseAddress + r.addressOffset, r.resetValue);
    blockView.setValues(values, true);
    blockView.setSelectedRegister(0);
    const QString blockPath = dir + QStringLiteral("/register-blocks-%1.png").arg(peripheral->name);
    QString renderError;
    if (!saveWidget(&blockView, blockPath, &renderError))
        return Status::failure(QStringLiteral("渲染寄存器块失败: %1").arg(renderError.isEmpty() ? blockPath : renderError));
    written << blockPath;

    // --- bit field strip ---
    BitFieldStrip strip;
    strip.setRegister(reg);
    strip.setValue(reg->resetValue, true);
    strip.setSelectedField(0);
    const QString stripPath = dir + QStringLiteral("/bitfields-%1-%2.png").arg(peripheral->name, reg->name);
    if (!saveWidget(&strip, stripPath, &renderError, QSize(904, 120)))
        return Status::failure(QStringLiteral("渲染位域条失败: %1").arg(renderError.isEmpty() ? stripPath : renderError));
    written << stripPath;

    // --- package view: pre-identification state (nothing rendered yet) ---
    PackageView package;
    package.showWaiting(QStringLiteral("连接目标后自动识别并渲染引脚布局"));
    const QString waitingPath = dir + QStringLiteral("/package-waiting.png");
    {
        // Before identification the view must draw *nothing but a hint*. A near
        // empty canvas is therefore the expected (and asserted) result: it proves
        // no package is pre-rendered.
        QString waitingError;
        double waitingRatio = 1.0;
        const bool saved = saveWidget(&package, waitingPath, &waitingError, QSize(900, 560),
                                      /*minRatio=*/0.0, &waitingRatio);
        if (!saved)
            return Status::failure(QStringLiteral("渲染等待态失败: %1").arg(waitingError));
        written << waitingPath;
        if (waitingRatio > 0.02) {
            return Status::failure(
                QStringLiteral("识别前不应绘制引脚: package-waiting.png 覆盖率 %1%（应 <2%）")
                    .arg(QString::number(waitingRatio * 100.0, 'f', 1)));
        }
        Logger::instance()->info(QStringLiteral("等待态覆盖率 %1%（识别前未绘制引脚，符合预期）")
                                     .arg(QString::number(waitingRatio * 100.0, 'f', 2)));
    }

    package.loadPackages(ResourcePaths::packageDirectory());
    const QStringList packages = package.packages();
    if (!packages.isEmpty()) {
        // Prefer a package describing this very chip (name prefix match); else first.
        QString chosen = packages.first();
        Q_UNUSED(chosen);
        for (const auto &description : package.descriptions()) {
            const QString chip = description.chip;
            if (chip.isEmpty() || device.name.isEmpty())
                continue;
            if (chip.startsWith(device.name, Qt::CaseInsensitive)
                || device.name.startsWith(chip, Qt::CaseInsensitive)) {
                chosen = description.packageName;
                break;
            }
        }
        package.setPackage(chosen);
        if (!device.name.isEmpty())
            package.setChipLabel(device.name);

        // Feed a synthetic pin state so all colour classes appear.
        for (const auto &p : device.peripherals) {
            if (!GpioCodec::isGpioPort(p))
                continue;
            const QString port = GpioCodec::portName(p);
            QHash<quint32, quint32> portValues;
            for (const auto &r : p.registers)
                portValues.insert(p.baseAddress + r.addressOffset, r.resetValue);
            bool valid = false;
            const QVector<GpioPin> pins = GpioCodec::decode(p, portValues, &valid);
            for (const auto &pin : pins)
                package.setLivePinState(port, pin.index, pin.mode, pin.outputHigh, true);
        }

        const QString packagePath = dir + QStringLiteral("/package-%1.png").arg(chosen);
        if (!saveWidget(&package, packagePath, &renderError))
            return Status::failure(
                QStringLiteral("渲染封装图失败: %1").arg(renderError.isEmpty() ? packagePath : renderError));
        written << packagePath;
    }

    // --- peripheral tree: pre-connection waiting state ----------------------
    // Same rule as the package view: nothing but a hint until the target's device
    // tree has been read back.
    {
        PeripheralTree waitingTree;
        waitingTree.setDevice(device); // an SVD is loaded...
        waitingTree.showWaiting(QStringLiteral("连接目标后自动读取设备树")); // ...but parked

        const QString waitingTreePath = dir + QStringLiteral("/peripheral-tree-waiting.png");
        double waitingRatio = 1.0;
        QString waitingTreeError;
        const bool saved = saveWidget(&waitingTree, waitingTreePath, &waitingTreeError, QSize(380, 560),
                                     /*minRatio=*/0.0, &waitingRatio);
        if (!saved)
            return Status::failure(QStringLiteral("渲染外设树等待态失败: %1").arg(waitingTreeError));
        written << waitingTreePath;

        // Structural assertion: a parked tree holds exactly one disabled hint row.
        if (auto *widget = waitingTree.findChild<QTreeWidget *>()) {
            const int tops = widget->topLevelItemCount();
            if (tops != 1)
                return Status::failure(
                    QStringLiteral("未连接时外设树应只有 1 条提示，实际 %1 条").arg(tops));
            QTreeWidgetItem *hint = widget->topLevelItem(0);
            if (hint->childCount() != 0 || (hint->flags() & Qt::ItemIsEnabled))
                return Status::failure(QStringLiteral("未连接时外设树的提示项不应可交互"));
            Logger::instance()->info(QStringLiteral("外设树等待态校验通过: 1 条提示，覆盖率 %1%")
                                         .arg(QString::number(waitingRatio * 100.0, 'f', 2)));
        }
    }

    // --- peripheral tree with device-tree states ---------------------------
    // Synthetic-but-realistic statuses so the "missing / clock off / present"
    // rendering can be eyeballed without a board.
    {
        const QHash<QString, DeviceTreeScanner::ClockBit> clocks = DeviceTreeScanner::clockMap(device);
        QVector<PeripheralStatus> statuses;
        int index = 0;
        for (const auto &peripheral : device.peripherals) {
            const DeviceTreeScanner::ClockBit bit = clocks.value(peripheral.name);
            // Put the first few peripherals in every state so all three render.
            const bool readOk = (index % 7) != 3;   // every 7th: bus rejects
            const bool gateOn = (index % 5) != 2;   // every 5th: clock off
            const quint32 value = readOk ? 0x00000000u : 0u;
            PeripheralStatus status = DeviceTreeScanner::classify(
                peripheral, bit, gateOn, readOk, value,
                readOk ? QString() : QStringLiteral("总线拒绝访问"));
            if (index % 7 == 3 && gateOn)
                status.reachability = PeripheralStatus::Missing;
            status.probeRegister = QStringLiteral("PROBE");
            statuses << status;
            ++index;
        }

        PeripheralTree tree;
        tree.setDevice(device);
        tree.setStatuses(statuses);
        const QString treePath = dir + QStringLiteral("/peripheral-tree.png");
        QString treeError;
        if (!saveWidget(&tree, treePath, &treeError, QSize(380, 560)))
            return Status::failure(QStringLiteral("渲染外设树失败: %1").arg(treeError));
        written << treePath;

        int onChip = 0;
        int clocked = 0;
        for (const auto &status : statuses) {
            if (status.onChip())
                ++onChip;
            if (status.clockKnown && status.clockEnabled)
                ++clocked;
        }
        Logger::instance()->info(QStringLiteral("外设树快照: %1 项，本芯片存在 %2，时钟已开 %3")
                                     .arg(statuses.size())
                                     .arg(onChip)
                                     .arg(clocked));

        // Structural check of the widget contents (the rendered text is not
        // machine-readable, the item model is).
        if (auto *widget = tree.findChild<QTreeWidget *>()) {
            int markedMissing = 0;
            int clockOn = 0;
            int clockOff = 0;
            int rows = 0;
            for (int g = 0; g < widget->topLevelItemCount(); ++g) {
                QTreeWidgetItem *group = widget->topLevelItem(g);
                for (int c = 0; c < group->childCount(); ++c) {
                    QTreeWidgetItem *item = group->child(c);
                    ++rows;
                    if (item->text(0).contains(QStringLiteral("本芯片未实现")))
                        ++markedMissing;
                    if (item->text(2) == QStringLiteral("开"))
                        ++clockOn;
                    else if (item->text(2) == QStringLiteral("关"))
                        ++clockOff;
                }
            }

            int expectedMissing = 0;
            int expectedOn = 0;
            int expectedOff = 0;
            for (const auto &status : statuses) {
                if (status.reachability == PeripheralStatus::Missing)
                    ++expectedMissing;
                if (status.clockKnown && status.clockEnabled)
                    ++expectedOn;
                if (status.clockKnown && !status.clockEnabled)
                    ++expectedOff;
            }

            const bool ok = rows == statuses.size() && markedMissing == expectedMissing
                            && clockOn == expectedOn && clockOff == expectedOff;
            if (!ok) {
                return Status::failure(
                    QStringLiteral("外设树渲染不一致: 行 %1/%2, 缺失标记 %3/%4, 时钟开 %5/%6, 时钟关 %7/%8")
                        .arg(rows)
                        .arg(statuses.size())
                        .arg(markedMissing)
                        .arg(expectedMissing)
                        .arg(clockOn)
                        .arg(expectedOn)
                        .arg(clockOff)
                        .arg(expectedOff));
            }
            Logger::instance()->info(QStringLiteral("外设树结构校验通过: %1 行, 缺失标记 %2, 时钟开 %3, 关 %4")
                                         .arg(rows)
                                         .arg(markedMissing)
                                         .arg(clockOn)
                                         .arg(clockOff));
        }
    }

    // --- port matrix, decoded from a real device snapshot ------------------
    // Uses the snapshot measured on N32H473CE (PMODE=0x028BFFFD) so the
    // "uninitialised vs analog" distinction is visible in the artifact:
    // PA0 is an output, PA9/PA11/PA12 are AF, PA1..PA8 are untouched -> 未初始化.
    {
        const SvdPeripheral *gpio = nullptr;
        for (const auto &peripheral : device.peripherals) {
            if (GpioCodec::portName(peripheral) == QLatin1String("PA")) {
                gpio = &peripheral;
                break;
            }
        }
        if (gpio) {
            QHash<quint32, quint32> snapshot;
            for (const auto &reg : gpio->registers) {
                quint32 value = 0;
                if (reg.name.endsWith(QLatin1String("PMODE")))
                    value = 0x028BFFFD; // measured on the target
                else if (reg.name.endsWith(QLatin1String("PUPD")))
                    value = 0x64100002;
                snapshot.insert(gpio->baseAddress + reg.addressOffset, value);
            }

            bool valid = false;
            const QVector<GpioPin> pins = GpioCodec::decode(*gpio, snapshot, &valid);

            PortGridWidget grid;
            grid.setPins(pins, valid);
            const QString gridPath = dir + QStringLiteral("/port-matrix-PA.png");
            QString gridError;
            if (!saveWidget(&grid, gridPath, &gridError, QSize(520, 460)))
                return Status::failure(QStringLiteral("渲染端口矩阵失败: %1").arg(gridError));
            written << gridPath;

            int uninit = 0;
            for (const auto &pin : pins) {
                if (pin.kind == GpioPin::Uninitialized)
                    ++uninit;
            }
            Logger::instance()->info(QStringLiteral("端口矩阵: %1 个引脚, 其中 %2 个未初始化")
                                         .arg(pins.size())
                                         .arg(uninit));
        }
    }

    // --- live package page, exactly as the GUI builds it after identification ---
    // Exercises PackagePage::renderForChip (JSON match, else synthesize from the
    // IO/pin counts) so the post-identification path is regression-tested too.
    if (!options.deviceName.isEmpty()) {
        // The CLI path may not have loaded the identification database yet.
        if (DeviceIdentifier::database().isEmpty()) {
            QString dbError;
            if (!DeviceIdentifier::loadDatabase(&dbError))
                Logger::instance()->warn(QStringLiteral("识别库加载失败: %1").arg(dbError));
        }

        int ioCount = 0;
        int pinCount = 0;
        QString packageHint;
        for (const auto &entry : DeviceIdentifier::database()) {
            if (entry.mcu.compare(options.deviceName, Qt::CaseInsensitive) == 0
                || entry.probeRsChip.compare(options.deviceName, Qt::CaseInsensitive) == 0) {
                ioCount = entry.ioCount;
                pinCount = entry.pinCount;
                packageHint = entry.packageName;
                break;
            }
        }

        PackagePage page;
        page.view()->loadPackages(ResourcePaths::packageDirectory());
        page.setPackages(page.view()->packages());
        const bool drawn = page.renderForChip(options.deviceName, ioCount, pinCount, packageHint);
        if (!drawn) {
            return Status::failure(QStringLiteral("无法为 %1 生成引脚布局（IO %2 / 脚 %3）")
                                       .arg(options.deviceName)
                                       .arg(ioCount)
                                       .arg(pinCount));
        }

        const QString livePath =
            dir + QStringLiteral("/package-live-%1.png").arg(options.deviceName);
        QString liveError;
        if (!saveWidget(&page, livePath, &liveError, QSize(900, 620)))
            return Status::failure(QStringLiteral("渲染实时引脚布局失败: %1").arg(liveError));
        written << livePath;
        Logger::instance()->info(QStringLiteral("实时引脚布局: %1 (IO %2 / 脚 %3)")
                                     .arg(options.deviceName)
                                     .arg(ioCount)
                                     .arg(pinCount));
    }

    for (const QString &path : written)
        Logger::instance()->info(QStringLiteral("已输出 %1").arg(path));
    return Status::success();
}

} // namespace mrd::cli
