// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/wallet.h>

#include <qt/convertcoinsdialog.h>
#include <qt/forms/ui_convertcoinsdialog.h>

#include <qt/clientmodel.h>
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

    ui->reqSlippage->setValue(DEFAULT_SLIPPAGE);
    ui->reqSlippage->setSingleStep(0.01);

    #if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
        connect(ui->groupType, &QButtonGroup::idClicked, this, &ConvertCoinsDialog::updateConversionType);
    #else
        connect(ui->groupType, qOverload<int>(&QButtonGroup::buttonClicked), this, &ConvertCoinsDialog::updateConversionType);
    #endif

    connect(ui->reqAmountIn, &BitcoinAmountField::valueChanged, this, &ConvertCoinsDialog::onInputChanged);
    connect(ui->reqAmountOut, &BitcoinAmountField::valueChanged, this, &ConvertCoinsDialog::onOutputChanged);
    connect(ui->convertButton, &QPushButton::clicked, this, &ConvertCoinsDialog::convertButtonClicked);
    connect(ui->clearButton, &QPushButton::clicked, this, &ConvertCoinsDialog::clear);

    updateConversionType();
}

void ConvertCoinsDialog::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;

    if (_clientModel) {
        connect(_clientModel, &ClientModel::numBlocksChanged, this, &ConvertCoinsDialog::recalculate);
    }
}

void ConvertCoinsDialog::setModel(WalletModel *_model)
{
    this->model = _model;

    if(_model && _model->getOptionsModel())
    {
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &ConvertCoinsDialog::updateDisplayUnit);
        updateDisplayUnit();
    }
}

ConvertCoinsDialog::~ConvertCoinsDialog()
{
    delete ui;
}

CAmountType ConvertCoinsDialog::getInputType()
{
    return ui->radioTypeCashIn->isChecked() ? CASH : BOND;
}

CAmountType ConvertCoinsDialog::getOutputType()
{
    return !getInputType();
}

void ConvertCoinsDialog::updateConversionType()
{
    CAmountType inType = getInputType();
    CAmountType outType = getOutputType();

    if (inType != ui->reqAmountIn->type()) {
        // Conversion type has changed - set the amount field
        if (inputIsExact) {
            ui->reqAmountOut->setValue(ui->reqAmountIn->value());
        } else {
            ui->reqAmountIn->setValue(ui->reqAmountOut->value());
        }
    }

    calculatingInput = true;   // Prevents setting type from calling onInputChanged calculation
    calculatingOutput = true;  // Prevents setting type from calling onOutputChanged calculation
    ui->reqAmountIn->setType(inType);
    ui->reqAmountOut->setType(outType);
    calculatingInput = false;  // Set to false because setType() does not call onInputChanged on first load
    calculatingOutput = false; // Set to false because setType() does not call onOutputChanged on first load
}

void ConvertCoinsDialog::onInputChanged()
{
    if (calculatingInput) {
        // Already recalculating - don't recalculate again
        calculatingInput = false;
    } else {
        // Input changed by user
        inputIsExact = true;
        recalculate();
    }
}

void ConvertCoinsDialog::onOutputChanged()
{
    if (calculatingOutput) {
        // Already recalculating - don't recalculate again
        calculatingOutput = false;
    } else {
        // Output changed by user
        inputIsExact = false;
        recalculate();
    }
}

void ConvertCoinsDialog::recalculate()
{
    if (inputIsExact) {
        calculatingOutput = true;
        CAmount outputAmount = this->model->estimateConversionOutputAmount(ui->reqAmountIn->value(), getInputType());
        ui->reqAmountOut->setValue(outputAmount);
    } else {
        calculatingInput = true;
        CAmount inputAmount = this->model->estimateConversionInputAmount(ui->reqAmountOut->value(), getOutputType());
        ui->reqAmountIn->setValue(inputAmount);
    }
}

void ConvertCoinsDialog::convertButtonClicked()
{
    CAmount maxInput = ui->reqAmountIn->value();
    CAmount minOutput = ui->reqAmountOut->value();
    if (inputIsExact) {
        // Apply slippage to output
        minOutput = int(double(minOutput) * (1 - double(ui->reqSlippage->value()) / 100));
    } else {
        // Apply slippage to input
        maxInput = int(double(maxInput) / (1 - double(ui->reqSlippage->value()) / 100));
    }
    CAmountType remainderType = inputIsExact ? getOutputType() : getInputType();
    m_current_transaction = std::make_unique<WalletModelConversionTransaction>(maxInput, minOutput, getInputType(), getOutputType(), remainderType);
}

void ConvertCoinsDialog::clear()
{
    ui->reqAmountIn->clear();
    ui->reqAmountOut->clear();
    ui->reqSlippage->setValue(DEFAULT_SLIPPAGE);
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
