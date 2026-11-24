#include "ui/BurnDialog.h"

#include "core/Logger.h"
#include "core/AppSettings.h"
#include "core/PartTable.h"
#include "core/ResourcePaths.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace mrd {
namespace {

/// The default split on these parts: a bootloader in the first 32 KB, the application
/// after it. Matches the supplied bootloader (0x08000000..0x08002ED8).
constexpr quint32 kDefaultBootloaderBase = 0x08000000u;
constexpr quint32 kDefaultApplicationBase = 0x08008000u;

QString addressText(quint32 value)
{
    return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0'));
}

bool parseAddress(const QString &text, quint32 *out)
{
    QString cleaned = text.trimmed();
    if (cleaned.isEmpty())
        return false;
    if (cleaned.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        cleaned = cleaned.mid(2);
    bool ok = false;
    const quint32 value = cleaned.toUInt(&ok, 16);
    if (ok && out)
        *out = value;
    return ok;
}

} // namespace

BurnDialog::BurnDialog(const QString &partName, bool connected, QWidget *parent)
    : QDialog(parent)
    , m_partName(partName)
    , m_connected(connected)
{
    // Offline the title says what the dialog is still good for, so it does not read as a
    // broken burn dialog.
    setWindowTitle(connected ? QStringLiteral("烧录固件")
                             : QStringLiteral("固件合并（未连接：不执行烧录）"));
    setMinimumWidth(620);

    auto *root = new QVBoxLayout(this);

    // --- images ----------------------------------------------------------
    auto *images = new QGroupBox(QStringLiteral("镜像"), this);
    auto *imagesForm = new QFormLayout(images);

    auto *appRow = new QHBoxLayout();
    m_appPath = new QLineEdit(images);
    m_appPath->setPlaceholderText(QStringLiteral("选择要烧录的 .bin（也接受 .hex / .axf）"));
    auto *browseApp = new QPushButton(QStringLiteral("浏览…"), images);
    appRow->addWidget(m_appPath, 1);
    appRow->addWidget(browseApp);
    imagesForm->addRow(QStringLiteral("固件文件:"), appRow);

    m_bootloaderCheck = new QCheckBox(
        QStringLiteral("使用 bootloader（合并内置 bootloader 与应用，一次写入）"), images);
    m_bootloaderCheck->setToolTip(
        QStringLiteral("勾选后默认使用应用内置的 LION_H473_Bootloader.hex；\n"
                       "也可自行指定其它 bootloader 文件。"));
    imagesForm->addRow(QString(), m_bootloaderCheck);

    auto *bootRow = new QHBoxLayout();
    m_bootloaderPath = new QLineEdit(images);
    m_bootloaderPath->setPlaceholderText(QStringLiteral("bootloader 的 .axf / .elf / .hex"));
    m_browseBootloader = new QPushButton(QStringLiteral("浏览…"), images);
    bootRow->addWidget(m_bootloaderPath, 1);
    bootRow->addWidget(m_browseBootloader);
    imagesForm->addRow(QStringLiteral("bootloader:"), bootRow);

    root->addWidget(images);

    // --- addresses -------------------------------------------------------
    // All three are editable: a second board revision may split the flash differently,
    // and the operator is the only one who knows that.
    auto *addresses = new QGroupBox(QStringLiteral("地址（可修改）"), this);
    auto *addressForm = new QFormLayout(addresses);

    m_appBase = new QLineEdit(addresses);
    m_appBase->setToolTip(
        QStringLiteral("固件（应用）放在哪个地址。\n"
                       "勾选 bootloader 时默认 0x08008000，否则 0x08000000。"));
    addressForm->addRow(QStringLiteral("固件起始地址:"), m_appBase);

    m_overallStart = new QLineEdit(addresses);
    m_overallStart->setToolTip(
        QStringLiteral("整体镜像（擦除与写入范围）的起始地址。\n"
                       "留空则按内容自动推导：取所有镜像里最小的地址。"));
    addressForm->addRow(QStringLiteral("整体起始地址:"), m_overallStart);

    m_overallEnd = new QLineEdit(addresses);
    m_overallEnd->setToolTip(
        QStringLiteral("整体镜像的结束地址（不含）。\n"
                       "留空则按内容自动推导，并按 4 KB 擦除扇区上对齐。"));
    addressForm->addRow(QStringLiteral("整体结束地址:"), m_overallEnd);

    auto *hint = new QLabel(
        QStringLiteral("留空的地址会自动推导。<b>点「分析」</b>可以看到推导结果，"
                       "确认无误后再烧录——烧录会先擦除这段范围。"),
        addresses);
    hint->setWordWrap(true);
    addressForm->addRow(hint);

    root->addWidget(addresses);

    // --- preview ---------------------------------------------------------
    m_preview = new QPlainTextEdit(this);
    m_preview->setReadOnly(true);
    m_preview->setMinimumHeight(170);
    m_preview->setPlaceholderText(QStringLiteral("点「分析」查看识别结果"));
    root->addWidget(m_preview, 1);

    // --- offline notice --------------------------------------------------
    if (!m_connected) {
        // Silent about this would be worse than the old "please connect first" refusal:
        // the operator sees a full dialog and only finds out at the last click.
        auto *notice = new QLabel(
            QStringLiteral("未连接目标：可以分析镜像、合并 bootloader 并另存，"
                           "但不能烧录。接上探针后重新打开即可写入。"),
            this);
        notice->setWordWrap(true);
        notice->setStyleSheet(QStringLiteral("color:#b26a00;padding:2px 0;"));
        root->addWidget(notice);
    }

    // --- buttons ---------------------------------------------------------
    auto *buttons = new QHBoxLayout();
    auto *analyseButton = new QPushButton(QStringLiteral("分析"), this);
    m_saveMergedButton = new QPushButton(QStringLiteral("合并另存到桌面"), this);
    m_saveMergedButton->setEnabled(false);
    m_saveMergedButton->setToolTip(
        QStringLiteral("按导入固件的名字，把合并后的镜像另存为 .bin 到桌面。\n"
                       "不改动原文件 —— 想先检查合并结果、或板子还没接上时用这个。"));
    m_burnButton = new QPushButton(QStringLiteral("烧录"), this);
    m_burnButton->setEnabled(false);
    if (!m_connected) {
        m_burnButton->setText(QStringLiteral("烧录（需连接）"));
        m_burnButton->setToolTip(
            QStringLiteral("擦写 Flash 需要探针。当前未连接，请先连接目标再打开本对话框。"));
    }
    auto *cancelButton = new QPushButton(QStringLiteral("取消"), this);
    buttons->addWidget(analyseButton);
    buttons->addStretch(1);
    buttons->addWidget(m_saveMergedButton);
    buttons->addWidget(m_burnButton);
    buttons->addWidget(cancelButton);
    root->addLayout(buttons);

    connect(browseApp, &QPushButton::clicked, this, &BurnDialog::onBrowseApplication);
    connect(m_browseBootloader, &QPushButton::clicked, this, &BurnDialog::onBrowseBootloader);
    connect(m_bootloaderCheck, &QCheckBox::toggled, this, &BurnDialog::onBootloaderToggled);
    connect(analyseButton, &QPushButton::clicked, this, &BurnDialog::onAnalyse);
    connect(m_saveMergedButton, &QPushButton::clicked, this, &BurnDialog::onSaveMerged);
    connect(m_burnButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

    onBootloaderToggled(false);
}

BurnDialog::~BurnDialog()
{
    m_image.discardScratch();
}

FirmwareImage BurnDialog::takeImage()
{
    FirmwareImage taken = m_image;
    m_image.scratchPath.clear(); // ownership moved: the destructor must leave the file
    return taken;
}

void BurnDialog::onBootloaderToggled(bool enabled)
{
    m_bootloaderPath->setEnabled(enabled);
    m_browseBootloader->setEnabled(enabled);

    // Unchecking must clear the path: plan() reads "hasBootloader() == path is non-empty",
    // so leaving it filled would keep merging a bootloader the user just un-ticked.
    if (!enabled) {
        m_bootloaderPath->clear();
        m_bootloaderPath->setToolTip(QString());
    }

    // Default to the bootloader that ships with the app. Filled in only when the field is
    // empty, so an operator who picked their own file keeps it when toggling the box.
    if (enabled && m_bootloaderPath->text().trimmed().isEmpty()) {
        const QString bundled = ResourcePaths::bundledBootloader();
        if (!bundled.isEmpty()) {
            m_bootloaderPath->setText(bundled);
            m_bootloaderPath->setToolTip(
                QStringLiteral("内置 bootloader：%1").arg(QFileInfo(bundled).fileName()));
        } else {
            m_bootloaderPath->setToolTip(
                QStringLiteral("未找到内置 bootloader，请手动选择 .hex/.axf"));
        }
    }

    // Keep the default in step with the checkbox, but never overwrite something the user
    // typed: that would silently change where the firmware lands.
    const QString current = m_appBase->text().trimmed();
    const quint32 want = enabled ? kDefaultApplicationBase : kDefaultBootloaderBase;
    if (current.isEmpty()
        || current.compare(addressText(kDefaultApplicationBase), Qt::CaseInsensitive) == 0
        || current.compare(addressText(kDefaultBootloaderBase), Qt::CaseInsensitive) == 0) {
        m_appBase->setText(addressText(want));
    }
    m_burnButton->setEnabled(false);
    m_analysed = false;
}

void BurnDialog::onBrowseApplication()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择固件"), m_appPath->text(),
        QStringLiteral("固件镜像 (*.bin *.hex *.axf *.elf);;所有文件 (*)"));
    if (!path.isEmpty()) {
        m_appPath->setText(path);
        m_burnButton->setEnabled(false);
        m_analysed = false;
    }
}

void BurnDialog::onBrowseBootloader()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 bootloader"), m_bootloaderPath->text(),
        QStringLiteral("bootloader (*.axf *.elf *.hex);;所有文件 (*)"));
    if (!path.isEmpty()) {
        m_bootloaderPath->setText(path);
        m_burnButton->setEnabled(false);
        m_analysed = false;
    }
}

FirmwarePlan BurnDialog::plan() const
{
    FirmwarePlan plan;
    plan.applicationPath = m_appPath->text().trimmed();
    if (m_bootloaderCheck->isChecked())
        plan.bootloaderPath = m_bootloaderPath->text().trimmed();
    parseAddress(m_appBase->text(), &plan.applicationBase);
    parseAddress(m_overallStart->text(), &plan.overallStart);
    parseAddress(m_overallEnd->text(), &plan.overallEnd);
    return plan;
}

bool BurnDialog::analyse(QString *error)
{
    m_analysed = false;
    // Release the previous merge's scratch before dropping the image that refers to it:
    // re-analysing (ticking the bootloader box twice, correcting a path) would otherwise
    // orphan one temp file per attempt, since the path lives only inside m_image.
    m_image.discardScratch();
    m_image = FirmwareImage(); // drop the previous success: it no longer describes the fields
    m_burnButton->setEnabled(false);
    if (m_saveMergedButton)
        m_saveMergedButton->setEnabled(false);

    const FirmwarePlan entered = plan();
    if (entered.applicationPath.isEmpty()) {
        if (error)
            *error = QStringLiteral("请先选择固件文件");
        m_preview->setPlainText(QStringLiteral("请先选择固件文件。"));
        return false;
    }
    // hasBootloader() only says "a path was given", so the checkbox has to be consulted
    // directly: checked-with-no-file would otherwise burn the application alone and look
    // like the bootloader had been written.
    if (m_bootloaderCheck->isChecked() && entered.bootloaderPath.isEmpty()) {
        if (error)
            *error = QStringLiteral("已勾选 bootloader 但未选择文件");
        m_preview->setPlainText(QStringLiteral("已勾选 bootloader 但未选择文件。"));
        return false;
    }

    const auto built = buildFirmwareImageForPart(entered, m_partName);
    if (!built) {
        if (error)
            *error = built.error;
        m_preview->setPlainText(QStringLiteral("分析失败：\n\n%1").arg(built.error));
        return false;
    }

    m_image = built.value;
    m_analysed = true;
    // Offline the analysis is still the point (it is what produces the merged file), so it
    // enables 合并另存 but must not re-arm 烧录.
    m_burnButton->setEnabled(m_connected);
    if (m_saveMergedButton)
        m_saveMergedButton->setEnabled(true);

    // Fill the derived addresses back into the fields, so what is shown is exactly what
    // will be erased if the user presses 烧录.
    m_appBase->setText(addressText(entered.effectiveApplicationBase(0x08000000u)));
    m_overallStart->setText(addressText(m_image.startAddress));
    m_overallEnd->setText(addressText(m_image.endAddress));

    QStringList lines;
    lines << QStringLiteral("型号：%1").arg(m_partName.isEmpty() ? QStringLiteral("(未知)")
                                                                : m_partName);
    lines << QStringLiteral("写入范围：%1 .. %2")
                 .arg(addressText(m_image.startAddress), addressText(m_image.endAddress));
    lines << QStringLiteral("写入长度：%1 字节（%2）")
                 .arg(m_image.span()).arg(PartTable::formatSize(m_image.span()));
    lines << QStringLiteral("向量表：SP=%1  Reset=%2")
                 .arg(addressText(m_image.initialSp), addressText(m_image.resetHandler));
    if (m_image.isMerged())
        lines << QStringLiteral("合并镜像：%1").arg(QFileInfo(m_image.scratchPath).fileName());
    lines << QString();
    for (const QString &note : m_image.notes)
        lines << QStringLiteral("· %1").arg(note);
    m_preview->setPlainText(lines.join(QLatin1Char('\n')));
    return true;
}

void BurnDialog::onAnalyse()
{
    QString error;
    if (!analyse(&error))
        m_burnButton->setEnabled(false);
}

void BurnDialog::setApplicationPath(const QString &path)
{
    m_appPath->setText(path);
}

void BurnDialog::setBootloaderEnabled(bool enabled)
{
    // setChecked() only emits toggled() when the state actually changes, so the defaults
    // would not be applied when it starts out unchecked. Run the handler explicitly and
    // keep the signal path working for real clicks.
    m_bootloaderCheck->setChecked(enabled);
    onBootloaderToggled(enabled);
}

void BurnDialog::setBootloaderPath(const QString &path)
{
    m_bootloaderPath->setText(path);
}

void BurnDialog::setApplicationBaseText(const QString &text)
{
    m_appBase->setText(text);
}

void BurnDialog::setOverallStartText(const QString &text)
{
    m_overallStart->setText(text);
}

void BurnDialog::setOverallEndText(const QString &text)
{
    m_overallEnd->setText(text);
}

QString BurnDialog::bootloaderPathForTest() const
{
    return m_bootloaderPath ? m_bootloaderPath->text() : QString();
}

QString BurnDialog::saveMergedToDesktop(QString *error)
{
    if (error)
        error->clear();
    if (!m_analysed || !m_image.isMerged()) {
        if (error)
            *error = QStringLiteral("需要先分析一个带 bootloader 的合并镜像");
        return QString();
    }

    // "按导入固件名称": the imported file's stem plus a marker, so the result cannot be
    // mistaken for the untouched original sitting in the same folder.
    const QString source = m_appPath->text().trimmed();
    const QFileInfo sourceInfo(source);
    const QString stem = sourceInfo.completeBaseName().isEmpty()
        ? QStringLiteral("firmware")
        : sourceInfo.completeBaseName();

    QString directory = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (directory.isEmpty() || !QDir(directory).exists()) {
        // No Desktop (some server profiles): fall back to the source folder rather than
        // writing somewhere the operator will not look.
        directory = sourceInfo.absolutePath();
    }
    const QString target =
        QDir(directory).filePath(QStringLiteral("%1_with_bootloader.bin").arg(stem));

    QFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("无法写入 %1：%2").arg(target, file.errorString());
        return QString();
    }
    const qint64 written = file.write(m_image.bytes);
    file.close();
    if (written != m_image.bytes.size()) {
        if (error)
            *error = QStringLiteral("只写入 %1/%2 字节到 %3")
                         .arg(written).arg(m_image.bytes.size()).arg(target);
        return QString();
    }

    Logger::instance()->info(QStringLiteral("合并镜像已另存: %1（%2 字节）")
                                 .arg(target)
                                 .arg(m_image.bytes.size()));
    return target;
}

void BurnDialog::onSaveMerged()
{
    QString error;
    const QString path = saveMergedToDesktop(&error);
    if (path.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("另存失败"), error);
        return;
    }
    QMessageBox::information(
        this, QStringLiteral("已另存"),
        QStringLiteral("合并镜像已保存到：\n%1\n\n原文件未被修改。").arg(path));
}

QString BurnDialog::analyseForTest()
{
    QString error;
    const bool ok = analyse(&error);
    return ok ? QString() : error;
}

} // namespace mrd
