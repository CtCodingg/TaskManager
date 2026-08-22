#include "AboutDialog.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QDialogButtonBox>
#include <QFont>
#include <QtGlobal>

#ifndef TM_VERSION_STRING
#define TM_VERSION_STRING "0.1.0"
#endif

namespace {
constexpr const char* kAuthorName = "CtCodingg";
constexpr const char* kGithubUrl = "https://github.com/CtCodingg";
// Short-form license label. TaskManager links against Qt Charts, which in
// the open-source Qt distribution is only available under GPLv3 (not
// LGPL) -- so the project as a whole is effectively GPL-3.0 licensed
// unless built against a commercial Qt license. See the repo's LICENCE
// file (also installed to bin/licences/) for the full text.
constexpr const char* kLicenseShortName = "GPL-3.0";
}

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("About TaskManager");
    setModal(true);
    setMinimumWidth(400);

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

    // --- Third-party software this build links against -------------------
    auto* modulesTitle = new QLabel("Third-party software");
    QFont sectionFont;
    sectionFont.setBold(true);
    modulesTitle->setFont(sectionFont);

    auto* modulesLabel = new QLabel(
        QString(
            "Qt %1 (Core, Gui, Widgets, Network) — LGPL-3.0<br>"
            "Qt %1 Charts — GPL-3.0"
        ).arg(qVersion()));
    modulesLabel->setTextFormat(Qt::RichText);

    auto* modulesHint = new QLabel(
        "No other third-party libraries are bundled. Platform APIs used "
        "directly (Linux: /proc, Netlink; Windows: PDH, IP Helper, ETW) are "
        "part of the OS, not redistributed with this app.");
    modulesHint->setObjectName("sectionHint");
    modulesHint->setWordWrap(true);

    // --- GPL-required notice: warranty disclaimer + redistribution terms,
    // roughly matching the "about box" text the GPL itself suggests for
    // interactive programs (see LICENCE, "How to Apply These Terms"). ----
    auto* separator = new QFrame();
    separator->setFrameShape(QFrame::HLine);

    auto* legalNotice = new QLabel(
        QString(
            "TaskManager comes with ABSOLUTELY NO WARRANTY. This is free "
            "software, and you are welcome to redistribute it under the "
            "terms of the %1 license (see LICENCE, installed alongside "
            "this binary in licences/). Source code is available at the "
            "GitHub link above."
        ).arg(kLicenseShortName));
    legalNotice->setObjectName("sectionHint");
    legalNotice->setWordWrap(true);

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
    layout->addSpacing(16);
    layout->addWidget(modulesTitle);
    layout->addWidget(modulesLabel);
    layout->addWidget(modulesHint);
    layout->addSpacing(12);
    layout->addWidget(separator);
    layout->addWidget(legalNotice);
    layout->addStretch();
    layout->addWidget(buttons);
}
