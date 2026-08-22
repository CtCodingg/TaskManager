#include "AboutDialog.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QFont>

#ifndef TM_VERSION_STRING
#define TM_VERSION_STRING "0.1.0"
#endif

namespace {
constexpr const char* kAuthorName = "CtCodingg";
constexpr const char* kGithubUrl = "https://github.com/CtCodingg";
// Short-form license label. TaskManager links against Qt Charts, which in
// the open-source Qt distribution is only available under GPLv3 (not
// LGPL) -- so the project as a whole is effectively GPL-3.0 licensed
// unless built against a commercial Qt license. See the repo's LICENSE
// file for the full text.
constexpr const char* kLicenseShortName = "GPL-3.0";
}

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("About TaskManager");
    setModal(true);
    setMinimumWidth(360);

    auto* titleLabel = new QLabel("TaskManager");
    QFont titleFont;
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);

    auto* versionLabel = new QLabel(QString("Version %1").arg(TM_VERSION_STRING));
    versionLabel->setObjectName("metricSubtle");

    auto* licenseLabel = new QLabel(QString("License: %1").arg(kLicenseShortName));
    licenseLabel->setObjectName("metricSubtle");

    auto* authorLabel = new QLabel(QString("Author: %1").arg(kAuthorName));

    auto* githubLabel = new QLabel(
        QString("<a href=\"%1\">%1</a>").arg(kGithubUrl));
    githubLabel->setOpenExternalLinks(true);
    githubLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &AboutDialog::accept);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(titleLabel);
    layout->addWidget(versionLabel);
    layout->addSpacing(12);
    layout->addWidget(licenseLabel);
    layout->addSpacing(12);
    layout->addWidget(authorLabel);
    layout->addWidget(githubLabel);
    layout->addStretch();
    layout->addWidget(buttons);
}
