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

#include <node/interface_ui.h>
#include <wallet/coincontrol.h>

#include <QAction>
#include <QCursor>
#include <QMessageBox>
#include <QScrollBar>
#include <QSettings>
#include <QTextDocument>

#include <fstream>

using wallet::CCoinControl;

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

    if (inType != ui->reqAmountIn->type() && !(ui->reqAmountOut->value() == 0 && ui->reqAmountOut->value() == 0)) {
        // Conversion type has changed and amount fields aren't both empty - flip the amounts
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

    m_coin_control = std::make_unique<CCoinControl>(); // TODO: Implement coin control
    m_coin_control->m_feerate = CFeeRate(100000); // TODO: Implement fee rate (currently 0.001)
    m_coin_control->m_fee_type = getInputType(); // TODO: Allow user to choose fee type (input or output / cash or bond)

    WalletModel::ConvertCoinsReturn prepareStatus = model->prepareTransaction(*m_current_transaction, *m_coin_control);
    BitcoinUnit unit = BitcoinUnits::unitOfType(model->getOptionsModel()->getDisplayUnit(), m_current_transaction->getTransactionFeeType());

    // process prepareStatus and on error generate message shown to user
    processConvertCoinsReturn(prepareStatus, BitcoinUnits::formatWithUnit(unit, m_current_transaction->getTransactionFee()));

    if (prepareStatus.status != WalletModel::ConversionOK) {
        return;
    }

    bool send_failure = false;
    bool shouldSave = false; // TODO: Implement via message box
    if (shouldSave) {
        // "Create Unsigned" clicked
        CMutableTransaction mtx = CMutableTransaction{*(m_current_transaction->getWtx())};
        PartiallySignedTransaction psbtx(mtx);
        bool complete = false;
        // Fill without signing
        TransactionError err = model->wallet().fillPSBT(SIGHASH_ALL, /*sign=*/false, /*bip32derivs=*/true, /*n_signed=*/nullptr, psbtx, complete);
        assert(!complete);
        assert(err == TransactionError::OK);

        // Copy PSBT to clipboard and offer to save
        presentPSBT(psbtx);
    } else {
        // "Convert" clicked
        assert(!model->wallet().privateKeysDisabled() || model->wallet().hasExternalSigner());
        bool broadcast = true;
        if (model->wallet().hasExternalSigner()) {
            CMutableTransaction mtx = CMutableTransaction{*(m_current_transaction->getWtx())};
            PartiallySignedTransaction psbtx(mtx);
            bool complete = false;
            // Always fill without signing first. This prevents an external signer
            // from being called prematurely and is not expensive.
            TransactionError err = model->wallet().fillPSBT(SIGHASH_ALL, /*sign=*/false, /*bip32derivs=*/true, /*n_signed=*/nullptr, psbtx, complete);
            assert(!complete);
            assert(err == TransactionError::OK);
            send_failure = !signWithExternalSigner(psbtx, mtx, complete);
            // Don't broadcast when user rejects it on the device or there's a failure:
            broadcast = complete && !send_failure;
            if (!send_failure) {
                // A transaction signed with an external signer is not always complete,
                // e.g. in a multisig wallet.
                if (complete) {
                    // Prepare transaction for broadcast transaction if complete
                    const CTransactionRef tx = MakeTransactionRef(mtx);
                    m_current_transaction->setWtx(tx);
                } else {
                    presentPSBT(psbtx);
                }
            }
        }

        // Broadcast the transaction, unless an external signer was used and it
        // failed, or more signatures are needed.
        if (broadcast) {
            // now send the prepared transaction
            model->convertCoins(*m_current_transaction);
            Q_EMIT coinsConverted(m_current_transaction->getWtx()->GetHash());
            // update the cached total supply so that a follow-up transaction is calculated properly
            clientModel->updateBestSupplyPostConversion(ui->reqAmountIn->value(), ui->reqAmountOut->value(), getInputType(), getOutputType());
        }
    }
    if (!send_failure) {
        accept();
        m_coin_control->UnSelectAll();
        // coinControlUpdateLabels(); // TODO: Implement
    }
    m_current_transaction.reset();
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

void ConvertCoinsDialog::presentPSBT(PartiallySignedTransaction& psbtx)
{
    // Serialize the PSBT
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << psbtx;
    GUIUtil::setClipboard(EncodeBase64(ssTx.str()).c_str());
    QMessageBox msgBox;
    msgBox.setText("Unsigned Transaction");
    msgBox.setInformativeText("The PSBT has been copied to the clipboard. You can also save it.");
    msgBox.setStandardButtons(QMessageBox::Save | QMessageBox::Discard);
    msgBox.setDefaultButton(QMessageBox::Discard);
    switch (msgBox.exec()) {
    case QMessageBox::Save: {
        QString selectedFilter;
        QString fileNameSuggestion = "myconversion.psbt";
        QString filename = GUIUtil::getSaveFileName(this,
            tr("Save Transaction Data"), fileNameSuggestion,
            //: Expanded name of the binary PSBT file format. See: BIP 174.
            tr("Partially Signed Transaction (Binary)") + QLatin1String(" (*.psbt)"), &selectedFilter);
        if (filename.isEmpty()) {
            return;
        }
        std::ofstream out{filename.toLocal8Bit().data(), std::ofstream::out | std::ofstream::binary};
        out << ssTx.str();
        out.close();
        Q_EMIT message(tr("PSBT saved"), "PSBT saved to disk", CClientUIInterface::MSG_INFORMATION);
        break;
    }
    case QMessageBox::Discard:
        break;
    default:
        assert(false);
    } // msgBox.exec()
}

void ConvertCoinsDialog::processConvertCoinsReturn(const WalletModel::ConvertCoinsReturn &convertCoinsReturn, const QString &msgArg)
{
    QPair<QString, CClientUIInterface::MessageBoxFlags> msgParams;
    // Default to a warning message, override if error message is needed
    msgParams.second = CClientUIInterface::MSG_WARNING;

    // This comment is specific to SendCoinsDialog usage of WalletModel::SendCoinsReturn.
    // All status values are used only in WalletModel::prepareTransaction()
    switch(convertCoinsReturn.status)
    {
    case WalletModel::InvalidInputAmount:
        msgParams.first = tr("The input amount must be larger than 0.");
        break;
    case WalletModel::InvalidOutputAmount:
        msgParams.first = tr("The output amount must be larger than 0.");
        break;
    case WalletModel::InputAmountExceedsBalance:
        msgParams.first = tr("The input amount exceeds your balance.");
        break;
    case WalletModel::InputAmountWithFeeExceedsBalance:
        msgParams.first = tr("The input exceeds your balance when the %1 transaction fee is included.").arg(msgArg);
        break;
    case WalletModel::FeeExceedsOutputAmount:
        msgParams.first = tr("The %1 transaction fee exceeds the minimum output amount.").arg(msgArg);
        break;
    case WalletModel::ConversionCreationFailed:
        msgParams.first = tr("Conversion creation failed!");
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    // included to prevent a compiler warning.
    case WalletModel::ConversionOK:
    default:
        return;
    }

    Q_EMIT message(tr("Convert Coins"), msgParams.first, msgParams.second);
}

bool ConvertCoinsDialog::signWithExternalSigner(PartiallySignedTransaction& psbtx, CMutableTransaction& mtx, bool& complete) {
    TransactionError err;
    try {
        err = model->wallet().fillPSBT(SIGHASH_ALL, /*sign=*/true, /*bip32derivs=*/true, /*n_signed=*/nullptr, psbtx, complete);
    } catch (const std::runtime_error& e) {
        QMessageBox::critical(nullptr, tr("Sign failed"), e.what());
        return false;
    }
    if (err == TransactionError::EXTERNAL_SIGNER_NOT_FOUND) {
        //: "External signer" means using devices such as hardware wallets.
        QMessageBox::critical(nullptr, tr("External signer not found"), "External signer not found");
        return false;
    }
    if (err == TransactionError::EXTERNAL_SIGNER_FAILED) {
        //: "External signer" means using devices such as hardware wallets.
        QMessageBox::critical(nullptr, tr("External signer failure"), "External signer failure");
        return false;
    }
    if (err != TransactionError::OK) {
        tfm::format(std::cerr, "Failed to sign PSBT");
        processConvertCoinsReturn(WalletModel::ConversionCreationFailed);
        return false;
    }
    // fillPSBT does not always properly finalize
    complete = FinalizeAndExtractPSBT(psbtx, mtx);
    return true;
}
