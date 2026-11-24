#pragma once

#include "core/FirmwareImage.h"

#include <QDialog>
#include <QString>

class QCheckBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace mrd {

/// Collects what to burn and shows the analysis before anything is erased.
///
/// The reason this is a dialog and not a chain of message boxes: a board with a
/// bootloader needs a *layout* (two images plus three addresses), and that layout has to
/// be visible while it is being edited. Showing it only in a confirmation box means the
/// operator cannot correct an address without starting over.
///
/// The dialog performs the analysis itself (pure file work, no probe) so the preview
/// updates as the user types; MainWindow owns the actual burn because it owns the
/// session that has to be released first.
class BurnDialog : public QDialog
{
    Q_OBJECT

public:
    /// \a partName is the order code used for the Flash/RAM limits.
    ///
    /// \a connected says whether a target is attached. Merging and analysis are pure
    /// file work, so the dialog stays fully usable offline -- only the burn button is
    /// disabled, because erasing and writing needs a probe.
    explicit BurnDialog(const QString &partName, bool connected = true,
                        QWidget *parent = nullptr);

    /// Removes the merged scratch file if nobody took the image.
    ///
    /// Analysing a bootloader merge writes the combined buffer to a temp file (that is
    /// what `loadbin` needs to point at). Browsing a few candidates used to leave one of
    /// those behind per analysis -- 472 had piled up in %TEMP% before this existed.
    ~BurnDialog() override;


    /// Can this dialog actually burn? False when it was opened offline.
    bool canBurn() const { return m_connected; }

    /// The plan as currently entered.
    FirmwarePlan plan() const;
    /// The validated image from the last successful analysis. Invalid until analysed.
    const FirmwareImage &image() const { return m_image; }

    /// Take the analysed image, transferring ownership of its scratch file.
    ///
    /// A plain copy of image() would share the scratch *path* without sharing ownership:
    /// the dialog's destructor would then delete the file while J-Link is still reading it,
    /// because the burn runs asynchronously and outlives this dialog. After this call the
    /// dialog cleans nothing up and the caller must discardScratch() when done.
    FirmwareImage takeImage();
    /// True when the last analysis succeeded.
    bool isAnalysed() const { return m_analysed; }

    /// Test hooks: set the fields the way a user would, then analyse.
    void setApplicationPath(const QString &path);
    void setBootloaderEnabled(bool enabled);
    void setBootloaderPath(const QString &path);
    void setApplicationBaseText(const QString &text);
    void setOverallStartText(const QString &text);
    void setOverallEndText(const QString &text);
    /// Run the analysis and return the resulting message (empty on success).
    QString analyseForTest();
    /// The bootloader path currently in the field.
    QString bootloaderPathForTest() const;

    /// Merge and save a copy to the Desktop, named after the imported firmware.
    ///
    /// Separate from the automatic write-back after a burn: this one never touches the
    /// source file, so the merged artifact can be inspected (or handed to someone) before
    /// deciding to burn. Returns the written path, or empty with \a error set.
    QString saveMergedToDesktop(QString *error);

private slots:
    void onBrowseApplication();
    /// Click handler for the 合并另存 button.
    void onSaveMerged();
    void onBrowseBootloader();
    void onBootloaderToggled(bool enabled);
    void onAnalyse();

private:
    QString partName() const { return m_partName; }
    /// Recompute and refresh the preview; returns true on success.
    bool analyse(QString *error);

    QString m_partName;
    /// Whether a target is attached. Offline, everything except 烧录 still works.
    bool m_connected = true;
    FirmwareImage m_image;
    bool m_analysed = false;

    QLineEdit *m_appPath = nullptr;
    QCheckBox *m_bootloaderCheck = nullptr;
    QLineEdit *m_bootloaderPath = nullptr;
    QPushButton *m_browseBootloader = nullptr;
    QLineEdit *m_appBase = nullptr;
    QLineEdit *m_overallStart = nullptr;
    QLineEdit *m_overallEnd = nullptr;
    QPlainTextEdit *m_preview = nullptr;
    QPushButton *m_burnButton = nullptr;
    QPushButton *m_saveMergedButton = nullptr;
};

} // namespace mrd
