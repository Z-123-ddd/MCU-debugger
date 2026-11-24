#include "ui/ChipPickerDialog.h"

#include "core/AppSettings.h"
#include "core/PartTable.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace mrd {

ChipPickerDialog::ChipPickerDialog(const QString &currentPart, const QString &currentPackage,
                                  QWidget *parent)
    : QDialog(parent)
    , m_currentPart(currentPart.trimmed())
    , m_currentPackage(currentPackage.trimmed())
{
    setWindowTitle(QStringLiteral("选择芯片型号"));
    setMinimumWidth(440);

    auto *root = new QVBoxLayout(this);

    auto *intro = new QLabel(
        QStringLiteral("型号不再自动推断。选择<b>系列 → 料号 → 封装</b>，"
                       "设备树与引脚图都按这一组合生成。\n"
                       "连接时会逐个访问这些外设；若大量外设对不上，会拒绝连接并说明。"),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto *form = new QFormLayout();
    m_series = new QComboBox(this);
    m_series->setMinimumWidth(240);
    m_series->addItems(PartTable::series());
    form->addRow(QStringLiteral("系列:"), m_series);

    m_parts = new QComboBox(this);
    m_parts->setMinimumWidth(240);
    form->addRow(QStringLiteral("料号:"), m_parts);

    m_packages = new QComboBox(this);
    m_packages->setMinimumWidth(240);
    form->addRow(QStringLiteral("封装:"), m_packages);
    root->addLayout(form);

    m_detail = new QLabel(this);
    m_detail->setWordWrap(true);
    m_detail->setTextFormat(Qt::RichText);
    root->addWidget(m_detail);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    m_ok->setText(QStringLiteral("使用该型号"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_series, &QComboBox::currentIndexChanged, this, &ChipPickerDialog::onSeriesChanged);
    // Connected once, permanently. A per-refill connection either stacks up or misses the
    // change that happens right after a refill, which is how the detail panel once
    // described the list's first entry instead of the selection.
    connect(m_parts, &QComboBox::currentIndexChanged, this, &ChipPickerDialog::onPartChanged);
    connect(m_packages, &QComboBox::currentIndexChanged, this, [this](int) {
        updateDetail(m_parts->currentText());
    });

    // Pre-select the saved order code; fall back to the first series when there is none.
    const QString startSeries = PartTable::seriesOf(m_currentPart);
    if (!startSeries.isEmpty()) {
        const int index = m_series->findText(startSeries, Qt::MatchFixedString);
        if (index >= 0)
            m_series->setCurrentIndex(index);
    }
    refreshParts();
    if (!m_currentPart.isEmpty()) {
        const int index = m_parts->findText(m_currentPart, Qt::MatchFixedString);
        if (index >= 0)
            m_parts->setCurrentIndex(index);
    }
    refreshPackages();
    if (!m_currentPackage.isEmpty()) {
        const int index = m_packages->findText(m_currentPackage, Qt::MatchFixedString);
        if (index >= 0)
            m_packages->setCurrentIndex(index);
    }
    updateDetail(m_parts->currentText());
}

QStringList ChipPickerDialog::seriesForTest() const
{
    QStringList out;
    for (int i = 0; i < m_series->count(); ++i)
        out << m_series->itemText(i);
    return out;
}

QString ChipPickerDialog::currentSeriesForTest() const
{
    return m_series->currentText();
}

QStringList ChipPickerDialog::partsForTest() const
{
    QStringList out;
    for (int i = 0; i < m_parts->count(); ++i)
        out << m_parts->itemText(i);
    return out;
}

QStringList ChipPickerDialog::packagesForTest() const
{
    QStringList out;
    for (int i = 0; i < m_packages->count(); ++i)
        out << m_packages->itemText(i);
    return out;
}

QString ChipPickerDialog::detailForTest() const
{
    return m_detail ? m_detail->text() : QString();
}

void ChipPickerDialog::selectSeriesForTest(const QString &series)
{
    const int index = m_series->findText(series, Qt::MatchFixedString);
    if (index >= 0)
        m_series->setCurrentIndex(index);
}

void ChipPickerDialog::selectPartForTest(const QString &part)
{
    const QString series = PartTable::seriesOf(part);
    if (!series.isEmpty())
        selectSeriesForTest(series);
    const int index = m_parts->findText(part, Qt::MatchFixedString);
    if (index >= 0)
        m_parts->setCurrentIndex(index);
}

void ChipPickerDialog::selectPackageForTest(const QString &package)
{
    const int index = m_packages->findText(package, Qt::MatchFixedString);
    if (index >= 0)
        m_packages->setCurrentIndex(index);
}

void ChipPickerDialog::onSeriesChanged()
{
    refreshParts();
    refreshPackages();
    updateDetail(m_parts->currentText());
}

void ChipPickerDialog::onPartChanged()
{
    refreshPackages();
    updateDetail(m_parts->currentText());
}

void ChipPickerDialog::refreshParts()
{
    m_parts->blockSignals(true);
    const QStringList parts = PartTable::partsInSeries(m_series->currentText());
    m_parts->clear();
    m_parts->addItems(parts);
    m_parts->blockSignals(false);
}

void ChipPickerDialog::refreshPackages()
{
    m_packages->blockSignals(true);
    const QStringList packages = PartTable::packagesOf(m_parts->currentText());
    m_packages->clear();
    m_packages->addItems(packages);
    m_packages->blockSignals(false);
    // A part with no package data cannot produce a pinout, so the OK button follows:
    // letting it be pressed would silently skip the pinout the user is choosing for.
    m_ok->setEnabled(m_parts->count() > 0 && m_packages->count() > 0);
}

bool ChipPickerDialog::isComplete() const
{
    return !m_parts->currentText().isEmpty() && !m_packages->currentText().isEmpty();
}

void ChipPickerDialog::updateDetail(const QString &part)
{
    if (!m_detail)
        return;
    const PartTable::Entry entry = PartTable::lookup(part);
    if (!entry.valid) {
        m_detail->clear();
        return;
    }
    const QString package = m_packages->currentText();
    QStringList lines;
    lines << QStringLiteral("<b>%1 · %2</b>").arg(entry.part, package.isEmpty()
                                                              ? QStringLiteral("?")
                                                              : package);
    lines << QStringLiteral("Flash %1 · RAM %2")
                 .arg(PartTable::formatSize(entry.flashBytes),
                      PartTable::formatSize(entry.ramBytes));
    if (entry.ioCount > 0)
        lines << QStringLiteral("%1 个 IO").arg(entry.ioCount);
    if (!entry.clock.isEmpty())
        lines << QStringLiteral("主频 %1").arg(entry.clock);
    m_detail->setText(lines.join(QStringLiteral("<br/>")));
}

QString ChipPickerDialog::selectedPart() const
{
    return m_parts->currentText();
}

QString ChipPickerDialog::selectedPackage() const
{
    return m_packages->currentText();
}

} // namespace mrd
