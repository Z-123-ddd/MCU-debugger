#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QComboBox;
class QLabel;
class QPushButton;

namespace mrd {

/// Pick the exact order code by hand.
///
/// Two dropdowns: series, then order code. Why not automatic: a DEV_ID identifies the
/// die, not the package or flash size, and the two cannot be told apart by reading memory
/// (a blank upper half and an unmapped window both answer 0xFF). Since a wrong package
/// produces a wrong pinout -- which is the whole point of the register/pinout view -- the
/// user picks the order code and the app stops guessing.
///
/// The series is pre-selected from the attached DEV_ID when that much is known, so on the
/// common path the user only has to confirm the order code.
class ChipPickerDialog : public QDialog
{
    Q_OBJECT

public:
    /// \a currentPart / \a currentPackage come from settings (either may be empty).
    explicit ChipPickerDialog(const QString &currentPart, const QString &currentPackage,
                             QWidget *parent = nullptr);

    /// The order code and package the user chose.
    QString selectedPart() const;
    QString selectedPackage() const;
    /// True when both are set (the OK button state mirrors this).
    bool isComplete() const;

    // --- test hooks ---------------------------------------------------------
    QStringList seriesForTest() const;
    QString currentSeriesForTest() const;
    QStringList partsForTest() const;
    void selectSeriesForTest(const QString &series);
    void selectPartForTest(const QString &part);
    QStringList packagesForTest() const;
    void selectPackageForTest(const QString &package);
    /// The current detail panel text (HTML).
    QString detailForTest() const;
    /// Accept without a human (the tests drive this).
    void acceptForTest() { accept(); }

private slots:
    void onSeriesChanged();
    void onPartChanged();

private:
    void refreshParts();
    /// Refill the package list for the current order code.
    void refreshPackages();
    /// Fill the detail panel for \a part / \a package (kept separate so it cannot be
    /// skipped: it once showed the list's first entry instead of the selection).
    void updateDetail(const QString &part);

    QComboBox *m_series = nullptr;
    QComboBox *m_parts = nullptr;
    QComboBox *m_packages = nullptr;
    QLabel *m_detail = nullptr;
    QPushButton *m_ok = nullptr;
    QString m_currentPart;
    QString m_currentPackage;
};

} // namespace mrd
