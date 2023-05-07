// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/convertcoinsdialog.h>
#include <qt/forms/ui_convertcoinsdialog.h>

#include <qt/addresstablemodel.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/coincontroldialog.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>

#include <chainparams.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/interface_ui.h>
#include <policy/fees.h>
#include <txmempool.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/wallet.h>

#include <array>
#include <chrono>
#include <fstream>
#include <memory>

#include <QFontMetrics>
#include <QScrollBar>
#include <QSettings>
#include <QTextDocument>

#include <logging.h>

using wallet::CCoinControl;
using wallet::DEFAULT_PAY_TX_FEE;
using wallet::DEFAULT_PAY_TX_FEE_TYPE;

static constexpr std::array confTargets{2, 4, 6, 12, 24, 48, 144, 504, 1008};
int getConversionConfTargetForIndex(int index) {
    if (index+1 > static_cast<int>(confTargets.size())) {
        return confTargets.back();
    }
    if (index < 0) {
        return confTargets[0];
    }
    return confTargets[index];
}
int getConversionIndexForConfTarget(int target) {
    for (unsigned int i = 0; i < confTargets.size(); i++) {
        if (confTargets[i] >= target) {
            return i;
        }
    }
    return confTargets.size() - 1;
}

static constexpr std::array deadlines{1, 2, 3, 4, 6, 9, 12, 24, 48, 144, 504, 1008, 0};
int getDeadlineForIndex(int index) {
    if (index+1 > static_cast<int>(deadlines.size())) {
        return deadlines.back();
    }
    if (index < 0) {
        return deadlines[0];
    }
    return deadlines[index];
}
int getIndexForDeadline(int target) {
    for (unsigned int i = 0; i < deadlines.size(); i++) {
        if (deadlines[i] >= target && target != 0) {
            return i;
        }
    }
    return deadlines.size() - 1;
}

ConvertCoinsDialog::ConvertCoinsDialog(const PlatformStyle *_platformStyle, QWidget *parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::ConvertCoinsDialog),
    clientModel(nullptr),
    model(nullptr),
    m_coin_control(new CCoinControl),
    fFeeMinimized(true),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);

    if (!_platformStyle->getImagesOnButtons()) {
        ui->clearButton->setIcon(QIcon());
        ui->convertButton->setIcon(QIcon());
    } else {
        ui->clearButton->setIcon(_platformStyle->SingleColorIcon(":/icons/remove"));
        ui->convertButton->setIcon(_platformStyle->SingleColorIcon(":/icons/send"));
    }

    ui->reqSlippage->setValue(DEFAULT_SLIPPAGE);
    ui->reqSlippage->setSingleStep(0.01);

    for (const int n : deadlines) {
        if (n > 0)
            ui->expirySelector->addItem(tr("%1 (%2 blocks)").arg(GUIUtil::formatNiceTimeOffset(n*Params().GetConsensus().nPowTargetSpacing)).arg(n));
        else
            ui->expirySelector->addItem(tr("No expiry"));
    }

    GUIUtil::setupAddressWidget(ui->lineEditCoinControlChange, this);

    #if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
        connect(ui->groupType, &QButtonGroup::idClicked, this, &ConvertCoinsDialog::updateConversionType);
    #else
        connect(ui->groupType, qOverload<int>(&QButtonGroup::buttonClicked), this, &ConvertCoinsDialog::updateConversionType);
    #endif

    connect(ui->reqAmountIn, &BitcoinAmountField::valueChanged, this, &ConvertCoinsDialog::onInputChanged);
    connect(ui->reqAmountOut, &BitcoinAmountField::valueChanged, this, &ConvertCoinsDialog::onOutputChanged);
    connect(ui->clearButton, &QPushButton::clicked, this, &ConvertCoinsDialog::clear);
    connect(ui->useAvailableBalanceButton, &QPushButton::clicked, this, &ConvertCoinsDialog::useAvailableBalanceClicked);

    // Coin Control
    connect(ui->pushButtonCoinControl, &QPushButton::clicked, this, &ConvertCoinsDialog::coinControlButtonClicked);
    connect(ui->checkBoxCoinControlChange, &QCheckBox::stateChanged, this, &ConvertCoinsDialog::coinControlChangeChecked);
    connect(ui->lineEditCoinControlChange, &QValidatedLineEdit::textEdited, this, &ConvertCoinsDialog::coinControlChangeEdited);

    // Coin Control: clipboard actions
    QAction *clipboardQuantityAction = new QAction(tr("Copy quantity"), this);
    QAction *clipboardAmountAction = new QAction(tr("Copy amount"), this);
    QAction *clipboardFeeAction = new QAction(tr("Copy fee"), this);
    QAction *clipboardAfterFeeAction = new QAction(tr("Copy after fee"), this);
    QAction *clipboardBytesAction = new QAction(tr("Copy bytes"), this);
    QAction *clipboardLowOutputAction = new QAction(tr("Copy dust"), this);
    QAction *clipboardChangeAction = new QAction(tr("Copy change"), this);
    connect(clipboardQuantityAction, &QAction::triggered, this, &ConvertCoinsDialog::coinControlClipboardQuantity);
    connect(clipboardAmountAction, &QAction::triggered, this, &ConvertCoinsDialog::coinControlClipboardAmount);
    connect(clipboardFeeAction, &QAction::triggered, this, &ConvertCoinsDialog::coinControlClipboardFee);
    connect(clipboardAfterFeeAction, &QAction::triggered, this, &ConvertCoinsDialog::coinControlClipboardAfterFee);
    connect(clipboardBytesAction, &QAction::triggered, this, &ConvertCoinsDialog::coinControlClipboardBytes);
    connect(clipboardLowOutputAction, &QAction::triggered, this, &ConvertCoinsDialog::coinControlClipboardLowOutput);
    connect(clipboardChangeAction, &QAction::triggered, this, &ConvertCoinsDialog::coinControlClipboardChange);
    ui->labelCoinControlQuantity->addAction(clipboardQuantityAction);
    ui->labelCoinControlAmount->addAction(clipboardAmountAction);
    ui->labelCoinControlFee->addAction(clipboardFeeAction);
    ui->labelCoinControlAfterFee->addAction(clipboardAfterFeeAction);
    ui->labelCoinControlBytes->addAction(clipboardBytesAction);
    ui->labelCoinControlLowOutput->addAction(clipboardLowOutputAction);
    ui->labelCoinControlChange->addAction(clipboardChangeAction);

    // init transaction fee section
    QSettings settings;
    if (!settings.contains("fConvertFeeSectionMinimized"))
        settings.setValue("fConvertFeeSectionMinimized", true);
    if (!settings.contains("nConvertFeeRadio") && settings.contains("nConvertTransactionFee") && settings.value("nConvertTransactionFee").toLongLong() > 0) // compatibility
        settings.setValue("nConvertFeeRadio", 1); // custom
    if (!settings.contains("nConvertFeeRadio"))
        settings.setValue("nConvertFeeRadio", 0); // recommended
    if (!settings.contains("nSmartFeeSliderPosition"))
        settings.setValue("nSmartFeeSliderPosition", 0);
    if (!settings.contains("nConvertTransactionFee"))
        settings.setValue("nConvertTransactionFee", (qint64)DEFAULT_PAY_TX_FEE);
    ui->groupFee->setId(ui->radioSmartFee, 0);
    ui->groupFee->setId(ui->radioCustomFee, 1);
    ui->groupFee->button((int)std::max(0, std::min(1, settings.value("nConvertFeeRadio").toInt())))->setChecked(true);
    ui->customFee->SetAllowEmpty(false);
    ui->customFee->setType(CASH, /** isUnscaled */ true);
    ui->customFee->setValue(settings.value("nConvertTransactionFee").toLongLong());
    minimizeFeeSection(settings.value("fConvertFeeSectionMinimized").toBool());

    connect(ui->customFee, &BitcoinAmountField::valueChanged, this, &ConvertCoinsDialog::updateFeeMinimizedLabel);

    GUIUtil::ExceptionSafeConnect(ui->convertButton, &QPushButton::clicked, this, &ConvertCoinsDialog::convertButtonClicked);
}

void ConvertCoinsDialog::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;

    if (_clientModel) {
        connect(_clientModel, &ClientModel::numBlocksChanged, this, &ConvertCoinsDialog::updateNumberOfBlocks);
    }
}

void ConvertCoinsDialog::setModel(WalletModel *_model)
{
    this->model = _model;

    if(_model && _model->getOptionsModel())
    {
        connect(_model, &WalletModel::balanceChanged, this, &ConvertCoinsDialog::setBalance);
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &ConvertCoinsDialog::refreshBalance);
        refreshBalance();

        // Coin Control
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &ConvertCoinsDialog::updateDisplayUnitAndCoinControlLabels);
        connect(_model->getOptionsModel(), &OptionsModel::coinControlFeaturesChanged, this, &ConvertCoinsDialog::coinControlFeatureChanged);
        ui->frameCoinControl->setVisible(_model->getOptionsModel()->getCoinControlFeatures());
        coinControlUpdateLabels();

        // fee section
        for (const int n : confTargets) {
            ui->confTargetSelector->addItem(tr("%1 (%2 blocks)").arg(GUIUtil::formatNiceTimeOffset(n*Params().GetConsensus().nPowTargetSpacing)).arg(n));
        }
        connect(ui->confTargetSelector, qOverload<int>(&QComboBox::currentIndexChanged), this, &ConvertCoinsDialog::updateSmartFeeLabel);
        connect(ui->confTargetSelector, qOverload<int>(&QComboBox::currentIndexChanged), this, &ConvertCoinsDialog::coinControlUpdateLabels);

#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
        connect(ui->groupFee, &QButtonGroup::idClicked, this, &ConvertCoinsDialog::updateFeeSectionControls);
        connect(ui->groupFee, &QButtonGroup::idClicked, this, &ConvertCoinsDialog::coinControlUpdateLabels);
        connect(ui->groupFee, &QButtonGroup::idClicked, this, &ConvertCoinsDialog::updateFeeMinimizedLabel);
#else
        connect(ui->groupFee, qOverload<int>(&QButtonGroup::buttonClicked), this, &ConvertCoinsDialog::updateFeeSectionControls);
        connect(ui->groupFee, qOverload<int>(&QButtonGroup::buttonClicked), this, &ConvertCoinsDialog::coinControlUpdateLabels);
        connect(ui->groupFee, qOverload<int>(&QButtonGroup::buttonClicked), this, &ConvertCoinsDialog::updateFeeMinimizedLabel);
#endif

        connect(ui->customFee, &BitcoinAmountField::valueChanged, this, &ConvertCoinsDialog::coinControlUpdateLabels);
        connect(ui->optInRBF, &QCheckBox::stateChanged, this, &ConvertCoinsDialog::updateSmartFeeLabel);
        connect(ui->optInRBF, &QCheckBox::stateChanged, this, &ConvertCoinsDialog::coinControlUpdateLabels);
        CAmount requiredFee = model->wallet().getRequiredFee(1000);
        ui->customFee->SetMinValue(requiredFee);
        if (ui->customFee->value() < requiredFee) {
            ui->customFee->setValue(requiredFee);
        }
        ui->customFee->setSingleStep(requiredFee);
        updateFeeSectionControls();
        updateSmartFeeLabel();

        // set default rbf checkbox state
        ui->optInRBF->setCheckState(Qt::Checked);

        if (model->wallet().hasExternalSigner()) {
            //: "device" usually means a hardware wallet.
            ui->convertButton->setText(tr("Sign on device"));
            if (gArgs.GetArg("-signer", "") != "") {
                ui->convertButton->setEnabled(true);
                ui->convertButton->setToolTip(tr("Connect your hardware wallet first."));
            } else {
                ui->convertButton->setEnabled(false);
                //: "External signer" means using devices such as hardware wallets.
                ui->convertButton->setToolTip(tr("Set external signer script path in Options -> Wallet"));
            }
        } else if (model->wallet().privateKeysDisabled()) {
            ui->convertButton->setText(tr("Cr&eate Unsigned"));
            ui->convertButton->setToolTip(tr("Creates a Partially Signed Bitcoin Transaction (PSBT) for use with e.g. an offline %1 wallet, or a PSBT-compatible hardware wallet.").arg(PACKAGE_NAME));
        }

        QSettings settings;
        if (!settings.contains("nExpiry"))
            settings.setValue("nExpiry", model->wallet().getConversionDeadline() + 1); // Offset by one so that zero deadline is properly saved and not confused with not set
        ui->expirySelector->setCurrentIndex(getIndexForDeadline(settings.value("nExpiry").toInt() - 1));

        // set the smartfee-sliders default value (wallets default conf.target or last stored value)
        if (settings.value("nConvertConfTarget").toInt() == 0)
            ui->confTargetSelector->setCurrentIndex(getConversionIndexForConfTarget(model->wallet().getConfirmTarget()));
        else
            ui->confTargetSelector->setCurrentIndex(getConversionIndexForConfTarget(settings.value("nConvertConfTarget").toInt()));

        updateConversionType();
    }
}

ConvertCoinsDialog::~ConvertCoinsDialog()
{
    QSettings settings;
    settings.setValue("fConvertFeeSectionMinimized", fFeeMinimized);
    settings.setValue("nConvertFeeRadio", ui->groupFee->checkedId());
    settings.setValue("nConvertConfTarget", getConversionConfTargetForIndex(ui->confTargetSelector->currentIndex()));
    settings.setValue("fConvertAmountType", getFeeType());
    settings.setValue("nConvertTransactionFee", (qint64)ui->customFee->value());
    settings.setValue("nExpiry", getDeadlineForIndex(ui->expirySelector->currentIndex()) + 1); // Offset by one so that zero deadline is properly saved and not confused with not set
    delete ui;
}

CAmountType ConvertCoinsDialog::getInputType()
{
    return ui->radioTypeCashIn->isChecked() ? CASH : BOND;
}

CAmountType ConvertCoinsDialog::getOutputType()
{
    return ui->radioTypeCashIn->isChecked() ? BOND : CASH;
}

void ConvertCoinsDialog::updateConversionType()
{
    if (!model)
        return;

    CAmountType inType = getInputType();
    CAmountType outType = getOutputType();

    if (inType != ui->reqAmountIn->type() && !(ui->reqAmountIn->value() == 0 && ui->reqAmountOut->value() == 0)) {
        // Conversion type has changed and amount fields aren't both empty - flip the amounts
        if (inputIsExact) {
            ui->reqAmountOut->setValue(ui->reqAmountIn->value());
        } else {
            ui->reqAmountIn->setValue(ui->reqAmountOut->value());
        }
    }

    calculatingInput = true;   // Prevents setting type from calling onInputChanged calculation
    calculatingOutput = true;  // Prevents setting type from calling onOutputChanged calculation
    ui->reqAmountIn->setType(inType, !model->getOptionsModel()->getShowScaledAmount(getInputType()));
    ui->reqAmountOut->setType(outType, !model->getOptionsModel()->getShowScaledAmount(getOutputType()));
    calculatingInput = false;  // Set to false because setType() does not call onInputChanged on first load
    calculatingOutput = false; // Set to false because setType() does not call onOutputChanged on first load

    // Update smart fee label
    updateSmartFeeLabel();
    // Clear coin control selection
    m_coin_control->UnSelectAll();
    updateDisplayUnitAndCoinControlLabels();
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
    if (model && inputIsExact && ui->reqAmountIn->value() != 0) {
        calculatingOutput = true;
        CAmount inputAmount = ui->reqAmountIn->value();
        if (model && model->getOptionsModel()->getShowScaledAmount(getInputType())) {
            inputAmount = DescaleAmount(inputAmount, clientModel->getBestScaleFactor());
        }
        CAmount outputAmount = model->wallet().estimateConversionOutputAmount(inputAmount, getInputType());
        if (model && model->getOptionsModel()->getShowScaledAmount(getOutputType())) {
            outputAmount = ScaleAmount(outputAmount, clientModel->getBestScaleFactor());
        }
        ui->reqAmountOut->setValue(outputAmount);
    } else if (model && !inputIsExact && ui->reqAmountOut->value() != 0) {
        calculatingInput = true;
        CAmount outputAmount = ui->reqAmountOut->value();
        if (model && model->getOptionsModel()->getShowScaledAmount(getOutputType())) {
            outputAmount = DescaleAmount(outputAmount, clientModel->getBestScaleFactor());
        }
        CAmount inputAmount = model->wallet().estimateConversionInputAmount(outputAmount, getOutputType());
        if (model && model->getOptionsModel()->getShowScaledAmount(getOutputType())) {
            inputAmount = ScaleAmount(inputAmount, clientModel->getBestScaleFactor());
        }
        ui->reqAmountIn->setValue(inputAmount);
    }
}

bool ConvertCoinsDialog::PrepareConversionText(QString& question_string, QString& informative_text, QString& detailed_text)
{
    WalletModel::UnlockContext ctx(model->requestUnlock());
    if(!ctx.isValid())
    {
        // Unlock wallet was cancelled
        return false;
    }

    if (!model || !model->getOptionsModel())
        return false;

    // Check if converting available balance
    bool usingAvailableBalance = false;
    if (inputIsExact) {
        interfaces::WalletBalances balances = model->getCachedBalance();
        if (getInputType() == CASH) {
            usingAvailableBalance = (ui->reqAmountIn->value() == balances.cash.balance);
        } else {
            usingAvailableBalance = (ui->reqAmountIn->value() == balances.bond.balance);
        }
    }

    // prepare conversion transaction
    CAmount maxInput = ui->reqAmountIn->value();
    CAmount minOutput = ui->reqAmountOut->value();
    if (model->getOptionsModel()->getShowScaledAmount(getInputType())) {
        maxInput = DescaleAmount(maxInput, clientModel->getBestScaleFactor());
    }
    if (model->getOptionsModel()->getShowScaledAmount(getOutputType())) {
        minOutput = DescaleAmount(minOutput, clientModel->getBestScaleFactor());
    }
    if (inputIsExact) {
        // Apply slippage to output
        minOutput = minOutput * (10000 - int(ui->reqSlippage->value() * 100)) / 10000;
    } else {
        // Apply slippage to input
        maxInput = maxInput * 10000 / (10000 - int(ui->reqSlippage->value() * 100));
    }
    CAmountType remainderType = inputIsExact ? getOutputType() : getInputType();
    bool fSubtractFeeFromInput = usingAvailableBalance;
    m_current_transaction = std::make_unique<WalletModelConversionTransaction>(maxInput, minOutput, getInputType(), getOutputType(), remainderType, fSubtractFeeFromInput);

    updateCoinControlState();

    WalletModel::ConvertCoinsReturn prepareStatus = model->prepareTransaction(*m_current_transaction, *m_coin_control);
    BitcoinUnit unit = model->getOptionsModel()->getDisplayUnit(m_current_transaction->getTransactionFeeType());

    // process prepareStatus and on error generate message shown to user
    processConvertCoinsReturn(prepareStatus, BitcoinUnits::formatWithUnit(unit, m_current_transaction->getTransactionFee()));

    if (prepareStatus.status != WalletModel::ConversionOK) {
        return false;
    }

    if (inputIsExact && fSubtractFeeFromInput) {
        // Subtract fee from input and recalculate minOutput applying slippage tolerance
        CAmount txFee = m_current_transaction->getTransactionFee();
        CAmount effectiveInput = maxInput - txFee;
        CAmount adjustedOutput = model->wallet().estimateConversionOutputAmount(effectiveInput, getInputType());
        minOutput = adjustedOutput * (10000 - int(ui->reqSlippage->value() * 100)) / 10000;
        // Copy the transaction and adjust the output amount
        CMutableTransaction mtx(*(m_current_transaction->getWtx()));
        for (unsigned int i = 0; i < mtx.vout.size(); i++) {
            CTxOut& txout = mtx.vout[i];
            if (txout.amountType == getOutputType() && !txout.scriptPubKey.IsConversionScript()) {
                txout.nValue = minOutput;
                break;
            }
        }
        // Sign the updated transaction
        if (!model->signConversion(mtx)) {
            return false;
        }
        // Replace the old transaction
        m_current_transaction->setWtx(MakeTransactionRef(std::move(mtx)));
    }

    CAmount txFee = m_current_transaction->getTransactionFee();
    if (model->getOptionsModel()->getShowScaledAmount(m_current_transaction->getTransactionFeeType())) {
        txFee = ScaleAmount(txFee, model->getBestScaleFactor());
    }

    if (model->getOptionsModel()->getShowScaledAmount(getInputType())) {
        maxInput = ScaleAmount(maxInput, clientModel->getBestScaleFactor());
    }
    if (model->getOptionsModel()->getShowScaledAmount(getOutputType())) {
        minOutput = ScaleAmount(minOutput, clientModel->getBestScaleFactor());
    }

    BitcoinUnit inputUnit = model->getOptionsModel()->getDisplayUnit(getInputType());
    BitcoinUnit outputUnit = model->getOptionsModel()->getDisplayUnit(getOutputType());

    QString inputAmountStr = BitcoinUnits::formatWithUnit(inputUnit, maxInput);
    QString outputAmountStr = BitcoinUnits::formatWithUnit(outputUnit, minOutput);

    QStringList formatted;
    if (inputIsExact) {
        formatted.append(tr("Convert %1").arg(inputAmountStr));
    } else {
        formatted.append(tr("Convert to %1").arg(outputAmountStr));
    }

    /*: Message displayed when attempting to create a transaction. Cautionary text to prompt the user to verify
        that the displayed transaction details represent the transaction the user intends to create. */
    question_string.append(tr("Do you want to create this transaction?"));
    question_string.append("<br /><span style='font-size:10pt;'>");
    if (model->wallet().privateKeysDisabled() && !model->wallet().hasExternalSigner()) {
        /*: Text to inform a user attempting to create a transaction of their current options. At this stage,
            a user can only create a PSBT. This string is displayed when private keys are disabled and an external
            signer is not available. */
        question_string.append(tr("Please, review your transaction proposal. This will produce a Partially Signed Bitcoin Transaction (PSBT) which you can save or copy and then sign with e.g. an offline %1 wallet, or a PSBT-compatible hardware wallet.").arg(PACKAGE_NAME));
    } else if (model->getOptionsModel()->getEnablePSBTControls()) {
        /*: Text to inform a user attempting to create a transaction of their current options. At this stage,
            a user can send their transaction or create a PSBT. This string is displayed when both private keys
            and PSBT controls are enabled. */
        question_string.append(tr("Please, review your transaction. You can create and send this transaction or create a Partially Signed Bitcoin Transaction (PSBT), which you can save or copy and then sign with, e.g., an offline %1 wallet, or a PSBT-compatible hardware wallet.").arg(PACKAGE_NAME));
    } else {
        /*: Text to prompt a user to review the details of the transaction they are attempting to send. */
        question_string.append(tr("Please, review your transaction."));
    }
    question_string.append("</span>%1");

    if(txFee > 0)
    {
        // append fee string if a fee is required
        question_string.append("<hr /><b>");
        question_string.append(tr("Transaction fee"));
        question_string.append("</b>");

        // append transaction size
        question_string.append(" (" + QString::number((double)m_current_transaction->getTransactionSize() / 1000) + " kB): ");

        // append transaction fee value
        question_string.append("<span style='color:#aa0000; font-weight:bold;'>");
        question_string.append(BitcoinUnits::formatHtmlWithUnit(unit, txFee));
        question_string.append("</span><br />");

        // append RBF message according to transaction's signalling
        question_string.append("<span style='font-size:10pt; font-weight:normal;'>");
        if (ui->optInRBF->isChecked()) {
            question_string.append(tr("You can increase the fee later (signals Replace-By-Fee, BIP-125)."));
        } else {
            question_string.append(tr("Not signalling Replace-By-Fee, BIP-125."));
        }
        question_string.append("</span>");
    }

    // add total amount in all subdivision units
    question_string.append("<hr />");

    QStringList alternativeInputUnits;
    for (const BitcoinUnit u : BitcoinUnits::availableUnits()) {
        if(u != unit && BitcoinUnits::type(u) == BitcoinUnits::type(inputUnit)
            && BitcoinUnits::isShare(u) == BitcoinUnits::isShare(inputUnit))
            alternativeInputUnits.append(BitcoinUnits::formatHtmlWithUnit(u, maxInput));
    }

    QStringList alternativeOutputUnits;
    for (const BitcoinUnit u : BitcoinUnits::availableUnits()) {
        if(u != unit && BitcoinUnits::type(u) == BitcoinUnits::type(outputUnit)
            && BitcoinUnits::isShare(u) == BitcoinUnits::isShare(outputUnit))
            alternativeOutputUnits.append(BitcoinUnits::formatHtmlWithUnit(u, minOutput));
    }

    QString inputLabel = inputIsExact ? tr("Input Amount") : tr("Max Input Amount");
    question_string.append(QString("<b>%1</b>: <b>%2</b>").arg(inputLabel)
        .arg(BitcoinUnits::formatHtmlWithUnit(inputUnit, maxInput)));
    question_string.append(QString("<br /><span style='font-size:10pt; font-weight:normal;'>(=%1)</span>")
        .arg(alternativeInputUnits.join(" " + tr("or") + " ")));

    question_string.append("<br/>");

    QString outputLabel = inputIsExact ? tr("Min Output Amount") : tr("Output Amount");
    question_string.append(QString("<b>%1</b>: <b>%2</b>").arg(outputLabel)
        .arg(BitcoinUnits::formatHtmlWithUnit(outputUnit, minOutput)));
    question_string.append(QString("<br /><span style='font-size:10pt; font-weight:normal;'>(=%1)</span>")
        .arg(alternativeOutputUnits.join(" " + tr("or") + " ")));

    question_string.append("<br/>");

    question_string = question_string.arg("<br /><br />" + formatted.at(0));

    return true;
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
        QString fileNameSuggestion = "";

        CAmount maxInput = ui->reqAmountIn->value();
        CAmount minOutput = ui->reqAmountOut->value();
        if (model->getOptionsModel()->getShowScaledAmount(getInputType())) {
            maxInput = DescaleAmount(maxInput, clientModel->getBestScaleFactor());
        }
        if (model->getOptionsModel()->getShowScaledAmount(getOutputType())) {
            minOutput = DescaleAmount(minOutput, clientModel->getBestScaleFactor());
        }
        if (inputIsExact) {
            // Apply slippage to output
            minOutput = minOutput * (10000 - int(ui->reqSlippage->value() * 100)) / 10000;
        } else {
            // Apply slippage to input
            maxInput = maxInput * 10000 / (10000 - int(ui->reqSlippage->value()) * 100);
        }
        if (model->getOptionsModel()->getShowScaledAmount(getInputType())) {
            maxInput = ScaleAmount(maxInput, clientModel->getBestScaleFactor());
        }
        if (model->getOptionsModel()->getShowScaledAmount(getOutputType())) {
            minOutput = ScaleAmount(minOutput, clientModel->getBestScaleFactor());
        }

        BitcoinUnit inputUnit = model->getOptionsModel()->getDisplayUnit(getInputType());
        BitcoinUnit outputUnit = model->getOptionsModel()->getDisplayUnit(getOutputType());
        QString inputAmountStr = BitcoinUnits::formatWithUnit(inputUnit, maxInput);
        QString outputAmountStr = BitcoinUnits::formatWithUnit(outputUnit, minOutput);
        if (inputIsExact) {
            fileNameSuggestion.append("Convert " + inputAmountStr + " to at least " + outputAmountStr);
        } else {
            fileNameSuggestion.append("Convert at most " + inputAmountStr + " to " + outputAmountStr);
        }

        fileNameSuggestion.append(".psbt");
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

void ConvertCoinsDialog::convertButtonClicked([[maybe_unused]] bool checked)
{
    if(!model || !model->getOptionsModel())
        return;

    QString question_string, informative_text, detailed_text;
    if (!PrepareConversionText(question_string, informative_text, detailed_text)) return;
    assert(m_current_transaction);

    const QString confirmation = tr("Confirm conversion");
    const bool enable_send{!model->wallet().privateKeysDisabled() || model->wallet().hasExternalSigner()};
    const bool always_show_unsigned{model->getOptionsModel()->getEnablePSBTControls()};
    auto confirmationDialog = new ConvertConfirmationDialog(confirmation, question_string, informative_text, detailed_text, SEND_CONFIRM_DELAY, enable_send, always_show_unsigned, this);
    confirmationDialog->setAttribute(Qt::WA_DeleteOnClose);
    // TODO: Replace QDialog::exec() with safer QDialog::show().
    const auto retval = static_cast<QMessageBox::StandardButton>(confirmationDialog->exec());

    if(retval != QMessageBox::Yes && retval != QMessageBox::Save)
    {
        return;
    }

    bool send_failure = false;
    if (retval == QMessageBox::Save) {
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
        }
    }
    if (!send_failure) {
        accept();
        m_coin_control->UnSelectAll();
        coinControlUpdateLabels();
    }
    m_current_transaction.reset();
}

void ConvertCoinsDialog::clear()
{
    m_current_transaction.reset();

    ui->reqAmountIn->clear();
    ui->reqAmountOut->clear();
    ui->reqSlippage->setValue(DEFAULT_SLIPPAGE);

    // Clear coin control settings
    m_coin_control->UnSelectAll();
    ui->checkBoxCoinControlChange->setChecked(false);
    ui->lineEditCoinControlChange->clear();

    setupTabChain(nullptr);
    updateDisplayUnitAndCoinControlLabels();
}

void ConvertCoinsDialog::useAvailableBalanceClicked()
{
    if (!model) return;
    // Set 'isUsingAvailableBalance' to true and set input amount
    interfaces::WalletBalances balances = model->getCachedBalance();
    if (getInputType() == CASH) {
        ui->reqAmountIn->setValue(balances.cash.balance);
    } else {
        ui->reqAmountIn->setValue(balances.bond.balance);
    }
}

void ConvertCoinsDialog::reject()
{
    clear();
}

void ConvertCoinsDialog::accept()
{
    clear();
}

void ConvertCoinsDialog::updateDisplayUnitAndCoinControlLabels()
{
    if(model && model->getOptionsModel())
    {
        ui->reqAmountIn->setDisplayUnit(model->getOptionsModel()->getDisplayUnit(getInputType()));
        ui->reqAmountOut->setDisplayUnit(model->getOptionsModel()->getDisplayUnit(getOutputType()));
    }
    coinControlUpdateLabels();
}

QWidget *ConvertCoinsDialog::setupTabChain(QWidget *prev)
{
    QWidget::setTabOrder(prev, ui->convertButton);
    QWidget::setTabOrder(ui->convertButton, ui->clearButton);
    return ui->clearButton;
}

void ConvertCoinsDialog::setBalance(const interfaces::WalletBalances& balances)
{
    if(model && model->getOptionsModel())
    {
        CAmount cashBalance = balances.cash.balance;
        CAmount bondBalance = balances.bond.balance;
        if (model->wallet().hasExternalSigner()) {
            ui->labelBalanceName->setText(tr("External balance:\n"));
        } else if (model->wallet().privateKeysDisabled()) {
            cashBalance = balances.cash.watch_only_balance;
            bondBalance = balances.bond.watch_only_balance;
            ui->labelBalanceName->setText(tr("Watch-only balance:\n"));
        } else {
            ui->labelBalanceName->setText(tr("Balance:\n"));
        }
        ui->labelBalance->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(BOND), bondBalance) + "\n" + BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(CASH), cashBalance));
    }
}

void ConvertCoinsDialog::refreshBalance()
{
    setBalance(model->getCachedBalance());
    ui->customFee->setDisplayUnit(BitcoinUnits::getUnitOfScaleType(model->getOptionsModel()->getDisplayUnit(CASH), /** isScaled */ false));
    updateSmartFeeLabel();
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

void ConvertCoinsDialog::minimizeFeeSection(bool fMinimize)
{
    ui->buttonChooseFee  ->setVisible(fMinimize);
    ui->buttonMinimizeFee->setVisible(!fMinimize);
    ui->frameFeeSelection->setVisible(!fMinimize);
    ui->horizontalLayoutSmartFee->setContentsMargins(0, (fMinimize ? 0 : 6), 0, 0);
    fFeeMinimized = fMinimize;
}

void ConvertCoinsDialog::on_buttonChooseFee_clicked()
{
    minimizeFeeSection(false);
}

void ConvertCoinsDialog::on_buttonMinimizeFee_clicked()
{
    updateFeeMinimizedLabel();
    minimizeFeeSection(true);
}

void ConvertCoinsDialog::updateFeeSectionControls()
{
    ui->confTargetSelector      ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee           ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee2          ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee3          ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelFeeEstimation      ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelCustomFeeWarning   ->setEnabled(ui->radioCustomFee->isChecked());
    ui->labelCustomPerKilobyte  ->setEnabled(ui->radioCustomFee->isChecked());
    ui->customFee               ->setEnabled(ui->radioCustomFee->isChecked());
}

void ConvertCoinsDialog::updateFeeMinimizedLabel()
{
    if(!model || !model->getOptionsModel())
        return;

    if (ui->radioSmartFee->isChecked())
        ui->labelFeeMinimized->setText(ui->labelSmartFee->text());
    else {
        BitcoinUnit unit = model->getOptionsModel()->getDisplayUnit(getFeeType());
        // Ensure displayed fee is at least the required fee (if user types in zero and then selects another field, the custom fee will default to the required fee rate but updateFeeMinimizedLabel will not be triggered)
        CAmount requiredFee = model->wallet().getRequiredFee(1000);
        CAmount displayedFee = std::max(ui->customFee->value(), requiredFee);
        // Apply estimated conversion rate if showing bond amount
        if (getFeeType() == BOND)
            displayedFee = model->wallet().estimateConvertedAmount(displayedFee, CASH, /** roundedUp */ true);
        // Apply scale factor if showing scaled amount
        if (model->getOptionsModel()->getShowScaledAmount(getFeeType()))
            displayedFee = ScaleAmount(displayedFee, model->getBestScaleFactor());
        ui->labelFeeMinimized->setText(BitcoinUnits::formatWithUnit(unit, displayedFee) + "/kvB");
    }
}

void ConvertCoinsDialog::updateCoinControlState()
{
    if (!model)
        return;
    if (ui->radioCustomFee->isChecked()) {
        m_coin_control->fIsScaledFeeRate = false;
        m_coin_control->m_feerate = CFeeRate(ui->customFee->value());
    } else {
        m_coin_control->m_feerate.reset();
    }
    m_coin_control->m_fee_type = getFeeType();
    m_coin_control->m_conversion_deadline = getDeadlineForIndex(ui->expirySelector->currentIndex());
    // Avoid using global defaults when sending money from the GUI
    // Either custom fee will be used or if not selected, the confirmation target from dropdown box
    m_coin_control->m_confirm_target = getConversionConfTargetForIndex(ui->confTargetSelector->currentIndex());
    m_coin_control->m_signal_bip125_rbf = ui->optInRBF->isChecked();
    // Include watch-only for wallets without private key
    m_coin_control->fAllowWatchOnly = model->wallet().privateKeysDisabled() && !model->wallet().hasExternalSigner();
}

void ConvertCoinsDialog::updateNumberOfBlocks(int count, const QDateTime& blockDate, double nVerificationProgress, SyncType synctype, SynchronizationState sync_state) {
    if (sync_state == SynchronizationState::POST_INIT) {
        updateSmartFeeLabel();
        refreshBalance();
    }
}

CAmountType ConvertCoinsDialog::getFeeType()
{
    return getInputType();
}

void ConvertCoinsDialog::updateSmartFeeLabel()
{
    if(!model || !model->getOptionsModel())
        return;
    updateCoinControlState();
    m_coin_control->m_feerate.reset(); // Explicitly use only fee estimation rate for smart fee labels
    int returned_target;
    FeeReason reason;
    CFeeRate feeRate = CFeeRate(model->wallet().getMinimumFee(1000, *m_coin_control, &returned_target, &reason));
    if (model->getOptionsModel()->getShowScaledAmount(getFeeType())) {
        feeRate = CFeeRate(ScaleAmount(feeRate.GetFeePerK(), model->getBestScaleFactor())); // Display fee rate scaled using latest scale factor
    }
    BitcoinUnit unit = model->getOptionsModel()->getDisplayUnit(getFeeType());
    ui->labelSmartFee->setText(BitcoinUnits::formatWithUnit(unit, feeRate.GetFeePerK()) + "/kvB");

    if (reason == FeeReason::FALLBACK) {
        ui->labelSmartFee2->show(); // (Smart fee not initialized yet. This usually takes a few blocks...)
        ui->labelFeeEstimation->setText("");
        ui->fallbackFeeWarningLabel->setVisible(true);
        int lightness = ui->fallbackFeeWarningLabel->palette().color(QPalette::WindowText).lightness();
        QColor warning_colour(255 - (lightness / 5), 176 - (lightness / 3), 48 - (lightness / 14));
        ui->fallbackFeeWarningLabel->setStyleSheet("QLabel { color: " + warning_colour.name() + "; }");
        ui->fallbackFeeWarningLabel->setIndent(GUIUtil::TextWidth(QFontMetrics(ui->fallbackFeeWarningLabel->font()), "x"));
    }
    else
    {
        ui->labelSmartFee2->hide();
        ui->labelFeeEstimation->setText(tr("Estimated to begin confirmation within %n block(s).", "", returned_target));
        ui->fallbackFeeWarningLabel->setVisible(false);
    }

    updateFeeMinimizedLabel();
}

// Coin Control: copy label "Quantity" to clipboard
void ConvertCoinsDialog::coinControlClipboardQuantity()
{
    GUIUtil::setClipboard(ui->labelCoinControlQuantity->text());
}

// Coin Control: copy label "Amount" to clipboard
void ConvertCoinsDialog::coinControlClipboardAmount()
{
    GUIUtil::setClipboard(ui->labelCoinControlAmount->text().left(ui->labelCoinControlAmount->text().indexOf(" ")));
}

// Coin Control: copy label "Fee" to clipboard
void ConvertCoinsDialog::coinControlClipboardFee()
{
    GUIUtil::setClipboard(ui->labelCoinControlFee->text().left(ui->labelCoinControlFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "After fee" to clipboard
void ConvertCoinsDialog::coinControlClipboardAfterFee()
{
    GUIUtil::setClipboard(ui->labelCoinControlAfterFee->text().left(ui->labelCoinControlAfterFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "Bytes" to clipboard
void ConvertCoinsDialog::coinControlClipboardBytes()
{
    GUIUtil::setClipboard(ui->labelCoinControlBytes->text().replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "Dust" to clipboard
void ConvertCoinsDialog::coinControlClipboardLowOutput()
{
    GUIUtil::setClipboard(ui->labelCoinControlLowOutput->text());
}

// Coin Control: copy label "Change" to clipboard
void ConvertCoinsDialog::coinControlClipboardChange()
{
    GUIUtil::setClipboard(ui->labelCoinControlChange->text().left(ui->labelCoinControlChange->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: settings menu - coin control enabled/disabled by user
void ConvertCoinsDialog::coinControlFeatureChanged(bool checked)
{
    ui->frameCoinControl->setVisible(checked);

    if (!checked && model) { // coin control features disabled
        m_coin_control = std::make_unique<CCoinControl>();
    }

    coinControlUpdateLabels();
}

// Coin Control: button inputs -> show actual coin control dialog
void ConvertCoinsDialog::coinControlButtonClicked()
{
    auto dlg = new CoinControlDialog(*m_coin_control, model, platformStyle);
    connect(dlg, &QDialog::finished, this, &ConvertCoinsDialog::coinControlUpdateLabels);
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

// Coin Control: checkbox custom change address
void ConvertCoinsDialog::coinControlChangeChecked(int state)
{
    if (state == Qt::Unchecked)
    {
        m_coin_control->destChange = CNoDestination();
        ui->labelCoinControlChangeLabel->clear();
    }
    else
        // use this to re-validate an already entered address
        coinControlChangeEdited(ui->lineEditCoinControlChange->text());

    ui->lineEditCoinControlChange->setEnabled((state == Qt::Checked));
}

// Coin Control: custom change address changed
void ConvertCoinsDialog::coinControlChangeEdited(const QString& text)
{
    if (model && model->getAddressTableModel())
    {
        // Default to no change address until verified
        m_coin_control->destChange = CNoDestination();
        ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:red;}");

        const CTxDestination dest = DecodeDestination(text.toStdString());

        if (text.isEmpty()) // Nothing entered
        {
            ui->labelCoinControlChangeLabel->setText("");
        }
        else if (!IsValidDestination(dest)) // Invalid address
        {
            ui->labelCoinControlChangeLabel->setText(tr("Warning: Invalid Bitcoin address"));
        }
        else // Valid address
        {
            if (!model->wallet().isSpendable(dest)) {
                ui->labelCoinControlChangeLabel->setText(tr("Warning: Unknown change address"));

                // confirmation dialog
                QMessageBox::StandardButton btnRetVal = QMessageBox::question(this, tr("Confirm custom change address"), tr("The address you selected for change is not part of this wallet. Any or all funds in your wallet may be sent to this address. Are you sure?"),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);

                if(btnRetVal == QMessageBox::Yes)
                    m_coin_control->destChange = dest;
                else
                {
                    ui->lineEditCoinControlChange->setText("");
                    ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:black;}");
                    ui->labelCoinControlChangeLabel->setText("");
                }
            }
            else // Known change address
            {
                ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:black;}");

                // Query label
                QString associatedLabel = model->getAddressTableModel()->labelForAddress(text);
                if (!associatedLabel.isEmpty())
                    ui->labelCoinControlChangeLabel->setText(associatedLabel);
                else
                    ui->labelCoinControlChangeLabel->setText(tr("(no label)"));

                m_coin_control->destChange = dest;
            }
        }
    }
}

// Coin Control: update labels
void ConvertCoinsDialog::coinControlUpdateLabels()
{
    if (!model || !model->getOptionsModel())
        return;

    updateCoinControlState();

    // set pay amounts
    CoinControlDialog::payAmounts.clear();
    CoinControlDialog::fSubtractFeeFromAmount = false;

    if (m_coin_control->HasSelected())
    {
        // actual coin control calculation
        CoinControlDialog::updateLabels(*m_coin_control, model, this);

        // show coin control stats
        ui->labelCoinControlAutomaticallySelected->hide();
        ui->widgetCoinControl->show();
    }
    else
    {
        // hide coin control stats
        ui->labelCoinControlAutomaticallySelected->show();
        ui->widgetCoinControl->hide();
        ui->labelCoinControlInsuffFunds->hide();
    }
}

ConvertConfirmationDialog::ConvertConfirmationDialog(const QString& title, const QString& text, const QString& informative_text, const QString& detailed_text, int _secDelay, bool enable_send, bool always_show_unsigned, QWidget* parent)
    : QMessageBox(parent), secDelay(_secDelay), m_enable_send(enable_send)
{
    setIcon(QMessageBox::Question);
    setWindowTitle(title); // On macOS, the window title is ignored (as required by the macOS Guidelines).
    setText(text);
    setInformativeText(informative_text);
    setDetailedText(detailed_text);
    setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    if (always_show_unsigned || !enable_send) addButton(QMessageBox::Save);
    setDefaultButton(QMessageBox::Cancel);
    yesButton = button(QMessageBox::Yes);
    if (confirmButtonText.isEmpty()) {
        confirmButtonText = yesButton->text();
    }
    m_psbt_button = button(QMessageBox::Save);
    updateButtons();
    connect(&countDownTimer, &QTimer::timeout, this, &ConvertConfirmationDialog::countDown);
}

int ConvertConfirmationDialog::exec()
{
    updateButtons();
    countDownTimer.start(1s);
    return QMessageBox::exec();
}

void ConvertConfirmationDialog::countDown()
{
    secDelay--;
    updateButtons();

    if(secDelay <= 0)
    {
        countDownTimer.stop();
    }
}

void ConvertConfirmationDialog::updateButtons()
{
    if(secDelay > 0)
    {
        yesButton->setEnabled(false);
        yesButton->setText(confirmButtonText + (m_enable_send ? (" (" + QString::number(secDelay) + ")") : QString("")));
        if (m_psbt_button) {
            m_psbt_button->setEnabled(false);
            m_psbt_button->setText(m_psbt_button_text + " (" + QString::number(secDelay) + ")");
        }
    }
    else
    {
        yesButton->setEnabled(m_enable_send);
        yesButton->setText(confirmButtonText);
        if (m_psbt_button) {
            m_psbt_button->setEnabled(true);
            m_psbt_button->setText(m_psbt_button_text);
        }
    }
}
