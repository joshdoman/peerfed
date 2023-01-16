// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/wallet.h>

#include <qt/convertcoinsdialog.h>
#include <qt/forms/ui_convertcoinsdialog.h>

#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <QAction>
#include <QCursor>
#include <QMessageBox>
#include <QScrollBar>
#include <QSettings>
#include <QTextDocument>

ConvertCoinsDialog::ConvertCoinsDialog(const PlatformStyle *_platformStyle, QWidget *parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::ConvertCoinsDialog),
    model(nullptr),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);

    if (!_platformStyle->getImagesOnButtons()) {
        ui->clearButton->setIcon(QIcon());
        ui->convertButton->setIcon(QIcon());
    } else {
        ui->clearButton->setIcon(_platformStyle->SingleColorIcon(":/icons/remove"));
        ui->convertButton->setIcon(_platformStyle->SingleColorIcon(":/icons/receiving_addresses"));
    }

    connect(ui->clearButton, &QPushButton::clicked, this, &ConvertCoinsDialog::clear);
}

void ConvertCoinsDialog::setModel(WalletModel *_model)
{
    this->model = _model;
}

ConvertCoinsDialog::~ConvertCoinsDialog()
{
    delete ui;
}

void ConvertCoinsDialog::clear()
{
    ui->reqAmountIn->clear();
    ui->reqAmountOut->clear();
    updateDisplayUnit();
}

void ConvertCoinsDialog::reject()
{
    clear();
}

void ConvertCoinsDialog::accept()
{
    clear();
}

void ConvertCoinsDialog::updateDisplayUnit()
{
    if(model && model->getOptionsModel())
    {
        ui->reqAmountIn->setDisplayUnit(model->getOptionsModel()->getDisplayUnit());
        ui->reqAmountOut->setDisplayUnit(model->getOptionsModel()->getDisplayUnit());
    }
}
