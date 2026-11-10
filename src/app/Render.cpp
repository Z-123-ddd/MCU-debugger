#include "app/Cli.h"

#include "core/GpioCodec.h"
#include "core/HexUtil.h"
#include "core/Logger.h"
#include "core/ResourcePaths.h"
#include "core/SvdTypes.h"
#include "ui/BitFieldStrip.h"
#include "core/DeviceIdentifier.h"
#include "core/DeviceTreeScanner.h"
#include "ui/InspectorPanel.h"
#include "ui/PackageView.h"
#include "ui/PeripheralTree.h"
#include "ui/PortGridView.h"
#include "ui/RegisterBlockView.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QLayout>
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
                double *outRatio = nullptr, bool exactSize = false)
{
    // exactSize lets a snapshot prove a *narrow* layout: without it the widget's
    // own sizeHint wins and the image would silently be 904 px wide again.
    const QSize size = exactSize ? minimum
                                 : widget->sizeHint().expandedTo(widget->minimumSizeHint())
                                       .expandedTo(minimum);
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
    strip.resize(904, BitFieldStrip::preferredHeight());
    if (!saveWidget(&strip, stripPath, &renderError,
                    QSize(904, BitFieldStrip::preferredHeight()), 0.02, nullptr, true))
        return Status::failure(
            QStringLiteral("渲染位域条失败: %1").arg(renderError.isEmpty() ? stripPath : renderError));
    written << stripPath;

    // The bit strip must never clip: all 32 bit states stay visible at any dock
    // width -- the cells shrink, nothing is dropped. Regression for the bug where
    // the value row sat 12 px below the widget edge, so the "0/1" states of the
    // upper half were cut off, and for a narrow dock losing the high bits.
    {
        QStringList fails;
        for (const int probeWidth : {904, 520, 380, 300, 240, 160, 120, 96}) {
            strip.resize(probeWidth, BitFieldStrip::preferredHeight());
            const QRect painted = strip.paintedBitsRect();
            const int cell = strip.bitWidth();
            const int height = strip.height();

            const bool wideEnough = painted.right() < probeWidth;
            const bool tallEnough = painted.bottom() < height;
            const bool coversAll = cell * 32 <= probeWidth - 8;
            if (!wideEnough || !tallEnough || !coversAll)
                fails << QStringLiteral("%1px: 格宽 %2, 绘制区 %3x%4 / 控件 %5x%6")
                             .arg(probeWidth)
                             .arg(cell)
                             .arg(painted.width())
                             .arg(painted.height())
                             .arg(probeWidth)
                             .arg(height);

            Logger::instance()->info(
                QStringLiteral("位域条 %1px: 32 位 x %2px = %3px, 绘制高 %4, 控件高 %5")
                    .arg(probeWidth)
                    .arg(cell)
                    .arg(cell * 32)
                    .arg(painted.height())
                    .arg(height));
        }
        if (!fails.isEmpty())
            return Status::failure(
                QStringLiteral("位域条显示不全: %1").arg(fails.join(QStringLiteral("; "))));

        // Narrow snapshot: shows the scaled-down layout is still complete.
        strip.resize(340, BitFieldStrip::preferredHeight());
        strip.setSelectedField(0);
        const QString narrowPath = dir + QStringLiteral("/bitfields-narrow.png");
        if (!saveWidget(&strip, narrowPath, &renderError,
                        QSize(340, BitFieldStrip::preferredHeight()), 0.02, nullptr, true))
            return Status::failure(QStringLiteral("渲染窄宽度位域条失败: %1").arg(narrowPath));
        written << narrowPath;
    }

    // --- whole inspector panel at a narrow dock width ------------------------
    // The reported bug lived one level up: the dock may be dragged narrow
    // (setMinimumWidth(200)), so the panel -- not just the bare strip -- has to
    // keep the bit states visible. This drives the real widget tree.
    {
        InspectorPanel panel;
        panel.setRegister(reg, peripheral->baseAddress + reg->addressOffset, reg->resetValue, true);
        panel.resize(300, 620);
        // Hidden widgets lay out lazily: flush the layout requests the same way a
        // running event loop would, otherwise we measure a half-laid-out panel.
        for (int round = 0; round < 4; ++round) {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            if (QLayout *lay = panel.layout())
                lay->activate();
        }
        BitFieldStrip *inner = panel.bitStrip();
        const QRect painted = inner->paintedBitsRect();
        const int needed = BitFieldStrip::preferredHeight();
        const bool fits = QRect(0, 0, inner->width(), inner->height())
                              .contains(QRect(0, 0, painted.right() + 1, painted.bottom() + 1));
        Logger::instance()->info(
            QStringLiteral("检查器面板 300px: 位域条 %1x%2 (需要高 %3), 每格 %4px, 绘制区 %5x%6")
                .arg(inner->width())
                .arg(inner->height())
                .arg(needed)
                .arg(inner->bitWidth())
                .arg(painted.width())
                .arg(painted.height()));
        const int panelMin = panel.minimumSizeHint().width();
        if (panelMin > 320)
            return Status::failure(
                QStringLiteral("检查器面板最小宽度 %1px，dock 无法拖窄").arg(panelMin));
        if (!fits || inner->height() < needed)
            return Status::failure(
                QStringLiteral("检查器面板在窄栏下位状态被裁: 条高 %1 / 需要 %2, 绘制区 %3x%4")
                    .arg(inner->height())
                    .arg(needed)
                    .arg(painted.width())
                    .arg(painted.height()));

        const QString panelPath = dir + QStringLiteral("/inspector-narrow.png");
        if (!saveWidget(&panel, panelPath, &renderError, QSize(300, 620), 0.02, nullptr, true))
            return Status::failure(QStringLiteral("渲染检查器面板失败: %1").arg(panelPath));
        written << panelPath;
    }

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

        // Apply the same bonding annotation the GUI does: a GPIO port whose
        // registers answer on the bus but which this package does not bring out
        // must not be reported as usable.
        QString packagePath;
        for (const QString &candidate : QDir(ResourcePaths::packageDirectory())
                                            .entryList({QStringLiteral("*.json")}, QDir::Files)) {
            QString error;
            const PackageDescription description = PackageDescription::loadFromFile(
                ResourcePaths::packageDirectory() + QLatin1Char('/') + candidate, &error);
            if (!description.isValid() || description.chip.isEmpty())
                continue;
            if (description.chip.startsWith(device.name, Qt::CaseInsensitive)
                || device.name.startsWith(description.chip, Qt::CaseInsensitive)) {
                packagePath = candidate;
                const QHash<QString, QVector<int>> bonded = description.bondedPorts();
                for (auto &status : statuses) {
                    if (!status.name.startsWith(QLatin1String("GPIO")))
                        continue;
                    const QString port = QStringLiteral("P") + status.name.mid(4, 1);
                    status.bondedPins = bonded.contains(port) ? bonded.value(port).size() : 0;
                }
                        Logger::instance()->info(QStringLiteral("外设树按封装 %1 键合数据标注")
                                             .arg(description.packageName));
                QStringList bondedList;
                for (const QString &port : bonded.keys())
                    bondedList << QStringLiteral("%1(%2)").arg(port).arg(bonded.value(port).size());
                bondedList.sort();
                Logger::instance()->info(QStringLiteral("封装键合端口: %1")
                                             .arg(bondedList.join(QStringLiteral(", "))));
                break;
            }
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

            int expectedRows = 0;
            int expectedMissing = 0;
            int expectedOn = 0;
            int expectedOff = 0;
            for (const auto &status : statuses) {
                // Unbonded ports are not listed at all any more.
                if (status.unbonded())
                    continue;
                ++expectedRows;
                if (status.reachability == PeripheralStatus::Missing)
                    ++expectedMissing;
                if (status.clockKnown && status.clockEnabled)
                    ++expectedOn;
                if (status.clockKnown && !status.clockEnabled)
                    ++expectedOff;
            }

            const bool ok = rows == expectedRows && markedMissing == expectedMissing
                            && clockOn == expectedOn && clockOff == expectedOff;
            if (!ok) {
                return Status::failure(
                    QStringLiteral("外设树渲染不一致: 行 %1/%2, 未实现 %3/%4, "
                                   "时钟开 %5/%6, 时钟关 %7/%8")
                        .arg(rows)
                        .arg(expectedRows)
                        .arg(markedMissing)
                        .arg(expectedMissing)
                        .arg(clockOn)
                        .arg(expectedOn)
                        .arg(clockOff)
                        .arg(expectedOff));
            }
            // Explicit: no port the package does not bond may appear at all.
            {
                QStringList forbidden;
                for (const auto &status : statuses) {
                    if (status.unbonded())
                        forbidden << status.name;
                }
                QStringList leaked;
                for (int g = 0; g < widget->topLevelItemCount(); ++g) {
                    QTreeWidgetItem *group = widget->topLevelItem(g);
                    for (int c = 0; c < group->childCount(); ++c) {
                        const QString text = group->child(c)->text(0);
                        for (const QString &name : forbidden) {
                            if (text.startsWith(name))
                                leaked << name;
                        }
                    }
                }
                if (!leaked.isEmpty())
                    return Status::failure(
                        QStringLiteral("未引出的外设仍出现在树里: %1").arg(leaked.join(QStringLiteral(", "))));
                Logger::instance()->info(
                    QStringLiteral("已确认未引出的 %1 个外设未出现在树中: %2")
                        .arg(forbidden.size())
                        .arg(forbidden.join(QStringLiteral(", "))));
            }

            Logger::instance()->info(
                QStringLiteral("外设树结构校验通过: %1 行（已隐藏未引出端口）, 未实现 %2, 时钟开 %3, 关 %4")
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

        // Dedicated-only ports must not exist: this package bonds PH0/PH1/PH6,
        // but those are OSC_IN/OSC_OUT/NRST, so PH is not a GPIO port and gets
        // neither a matrix nor a tree group. They are drawn by their own names.
        {
            for (const QString &name : QDir(ResourcePaths::packageDirectory())
                                          .entryList({QStringLiteral("*.json")}, QDir::Files)) {
                const PackageDescription description = PackageDescription::loadFromFile(
                    ResourcePaths::packageDirectory() + QLatin1Char('/') + name);
                if (!description.isValid() || description.chip.isEmpty())
                    continue;
                if (!description.chip.startsWith(device.name, Qt::CaseInsensitive)
                    && !device.name.startsWith(description.chip, Qt::CaseInsensitive))
                    continue;

                const QHash<QString, QVector<int>> gpio = description.gpioPorts();
                if (!description.bondedPorts().contains(QStringLiteral("PH")))
                    break;
                if (gpio.contains(QStringLiteral("PH")))
                    return Status::failure(
                        QStringLiteral("%1 仍有 PH 端口矩阵（应为专用脚 OSC_IN/OSC_OUT/NRST）")
                            .arg(description.packageName));

                QStringList labels;
                for (const auto &pin : description.dedicatedPins()) {
                    if (pin.portName.compare(QStringLiteral("PH"), Qt::CaseInsensitive) == 0)
                        labels << pin.displayName;
                }
                labels.sort();
                const QStringList expected = {QStringLiteral("NRST"), QStringLiteral("OSC_IN"),
                                              QStringLiteral("OSC_OUT")};
                if (labels != expected)
                    return Status::failure(
                        QStringLiteral("专用引脚标签不正确: %1").arg(labels.join(QStringLiteral(", "))));
                Logger::instance()->info(
                    QStringLiteral("%1: PH 键合 3 脚但全部为专用功能，按名称显示为 %2（无 PH 端口矩阵）")
                        .arg(description.packageName, labels.join(QStringLiteral(" / "))));
                break;
            }
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
