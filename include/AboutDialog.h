#pragma once

#include <QDialog>

// A minimal "About" window: app name + version, short-form license name,
// and author/GitHub attribution. No full license text here -- see the
// repo's LICENSE file for that.
class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(QWidget* parent = nullptr);
};
