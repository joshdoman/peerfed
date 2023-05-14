// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/bitcoinunits.h>

#include <consensus/amount.h>
#include <logging.h>

#include <QStringList>

#include <cassert>

static constexpr auto MAX_DIGITS_BTC = 16;

BitcoinUnits::BitcoinUnits(QObject *parent, bool _displayAll):
        QAbstractListModel(parent),
        unitlist(availableUnits()),
        displayAll(_displayAll)
{
}

QList<BitcoinUnit> BitcoinUnits::availableUnits()
{
    QList<BitcoinUnit> unitlist;
    unitlist.append(Unit::CASH);
    unitlist.append(Unit::mCASH);
    unitlist.append(Unit::uCASH);
    unitlist.append(Unit::sCASH);
    unitlist.append(Unit::BOND);
    unitlist.append(Unit::mBOND);
    unitlist.append(Unit::uBOND);
    unitlist.append(Unit::sBOND);
    unitlist.append(Unit::sh_CASH);
    unitlist.append(Unit::sh_mCASH);
    unitlist.append(Unit::sh_uCASH);
    unitlist.append(Unit::sh_sCASH);
    unitlist.append(Unit::sh_BOND);
    unitlist.append(Unit::sh_mBOND);
    unitlist.append(Unit::sh_uBOND);
    unitlist.append(Unit::sh_sBOND);
    return unitlist;
}

QString BitcoinUnits::longName(Unit unit)
{
    switch (unit) {
    case Unit::CASH: return QString("PFC");
    case Unit::mCASH: return QString("mPFC");
    case Unit::uCASH: return QString::fromUtf8("µPFC");
    case Unit::sCASH: return QString("sPFC");
    case Unit::BOND: return QString("PFB");
    case Unit::mBOND: return QString("mPFB");
    case Unit::uBOND: return QString::fromUtf8("µPFB");
    case Unit::sBOND: return QString("sPFB");
    case Unit::sh_CASH: return QString("UPFC");
    case Unit::sh_mCASH: return QString("mUPFC");
    case Unit::sh_uCASH: return QString::fromUtf8("µUPFC");
    case Unit::sh_sCASH: return QString("sUPFC");
    case Unit::sh_BOND: return QString("UPFB");
    case Unit::sh_mBOND: return QString("mUPFB");
    case Unit::sh_uBOND: return QString::fromUtf8("µUPFB");
    case Unit::sh_sBOND: return QString("sUPFB");
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

QString BitcoinUnits::shortName(Unit unit)
{
    switch (unit) {
    case Unit::CASH: return longName(unit);
    case Unit::mCASH: return longName(unit);
    case Unit::uCASH: return QString("c-bits");
    case Unit::sCASH: return QString("c-sat");
    case Unit::BOND: return longName(unit);
    case Unit::mBOND: return longName(unit);
    case Unit::uBOND: return QString("b-bits");
    case Unit::sBOND: return QString("b-sat");
    case Unit::sh_CASH: return longName(unit);
    case Unit::sh_mCASH: return longName(unit);
    case Unit::sh_uCASH: return QString("uc-bits");
    case Unit::sh_sCASH: return QString("uc-sats");
    case Unit::sh_BOND: return longName(unit);
    case Unit::sh_mBOND: return longName(unit);
    case Unit::sh_uBOND: return QString("ub-bits");
    case Unit::sh_sBOND: return QString("ub-sats");
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

QString BitcoinUnits::description(Unit unit)
{
    switch (unit) {
    case Unit::CASH: return QString("PeerFed Cash");
    case Unit::mCASH: return QString("Milli-PeerFed Cash (1 / 1" THIN_SP_UTF8 "000)");
    case Unit::uCASH: return QString("Micro-PeerFed Cash (c-bits) (1 / 1" THIN_SP_UTF8 "000" THIN_SP_UTF8 "000)");
    case Unit::sCASH: return QString("Satoshi-PeerFed Cash (c-sat) (1 / 100" THIN_SP_UTF8 "000" THIN_SP_UTF8 "000)");
    case Unit::BOND: return QString("PeerFed Bonds");
    case Unit::mBOND: return QString("Milli-PeerFed Bonds (1 / 1" THIN_SP_UTF8 "000)");
    case Unit::uBOND: return QString("Micro-PeerFed Bonds (b-bits) (1 / 1" THIN_SP_UTF8 "000" THIN_SP_UTF8 "000)");
    case Unit::sBOND: return QString("Satoshi-PeerFed Bonds (b-sat) (1 / 100" THIN_SP_UTF8 "000" THIN_SP_UTF8 "000)");
    case Unit::sh_CASH: return QString("Unscaled PeerFed Cash");
    case Unit::sh_mCASH: return QString("Milli-Unscaled PeerFed Cash (1 / 1" THIN_SP_UTF8 "000)");
    case Unit::sh_uCASH: return QString("Micro-Unscaled PeerFed Cash (uc-bits) (1 / 1" THIN_SP_UTF8 "000" THIN_SP_UTF8 "000)");
    case Unit::sh_sCASH: return QString("Satoshi-Unscaled PeerFed Cash (uc-sat) (1 / 100" THIN_SP_UTF8 "000" THIN_SP_UTF8 "000)");
    case Unit::sh_BOND: return QString("Unscaled PeerFed Bonds");
    case Unit::sh_mBOND: return QString("Milli-Unscaled PeerFed Bonds (1 / 1" THIN_SP_UTF8 "000)");
    case Unit::sh_uBOND: return QString("Micro-Unscaled PeerFed Bonds (ub-bits) (1 / 1" THIN_SP_UTF8 "000" THIN_SP_UTF8 "000)");
    case Unit::sh_sBOND: return QString("Satoshi-Unscaled PeerFed Bonds (ub-sat) (1 / 100" THIN_SP_UTF8 "000" THIN_SP_UTF8 "000)");
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

qint64 BitcoinUnits::factor(Unit unit)
{
    switch (unit) {
    case Unit::CASH: return 100'000'000;
    case Unit::mCASH: return 100'000;
    case Unit::uCASH: return 100;
    case Unit::sCASH: return 1;
    case Unit::BOND: return 100'000'000;
    case Unit::mBOND: return 100'000;
    case Unit::uBOND: return 100;
    case Unit::sBOND: return 1;
    case Unit::sh_CASH: return 100'000'000;
    case Unit::sh_mCASH: return 100'000;
    case Unit::sh_uCASH: return 100;
    case Unit::sh_sCASH: return 1;
    case Unit::sh_BOND: return 100'000'000;
    case Unit::sh_mBOND: return 100'000;
    case Unit::sh_uBOND: return 100;
    case Unit::sh_sBOND: return 1;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

int BitcoinUnits::decimals(Unit unit)
{
    switch (unit) {
    case Unit::CASH: return 8;
    case Unit::mCASH: return 5;
    case Unit::uCASH: return 2;
    case Unit::sCASH: return 0;
    case Unit::BOND: return 8;
    case Unit::mBOND: return 5;
    case Unit::uBOND: return 2;
    case Unit::sBOND: return 0;
    case Unit::sh_CASH: return 8;
    case Unit::sh_mCASH: return 5;
    case Unit::sh_uCASH: return 2;
    case Unit::sh_sCASH: return 0;
    case Unit::sh_BOND: return 8;
    case Unit::sh_mBOND: return 5;
    case Unit::sh_uBOND: return 2;
    case Unit::sh_sBOND: return 0;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

CAmountType BitcoinUnits::type(Unit unit)
{
    switch (unit) {
    case Unit::CASH: return CASH;
    case Unit::mCASH: return CASH;
    case Unit::uCASH: return CASH;
    case Unit::sCASH: return CASH;
    case Unit::BOND: return BOND;
    case Unit::mBOND: return BOND;
    case Unit::uBOND: return BOND;
    case Unit::sBOND: return BOND;
    case Unit::sh_CASH: return CASH;
    case Unit::sh_mCASH: return CASH;
    case Unit::sh_uCASH: return CASH;
    case Unit::sh_sCASH: return CASH;
    case Unit::sh_BOND: return BOND;
    case Unit::sh_mBOND: return BOND;
    case Unit::sh_uBOND: return BOND;
    case Unit::sh_sBOND: return BOND;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

bool BitcoinUnits::isShare(Unit unit)
{
    switch (unit) {
    case Unit::CASH: return false;
    case Unit::mCASH: return false;
    case Unit::uCASH: return false;
    case Unit::sCASH: return false;
    case Unit::BOND: return false;
    case Unit::mBOND: return false;
    case Unit::uBOND: return false;
    case Unit::sBOND: return false;
    case Unit::sh_CASH: return true;
    case Unit::sh_mCASH: return true;
    case Unit::sh_uCASH: return true;
    case Unit::sh_sCASH: return true;
    case Unit::sh_BOND: return true;
    case Unit::sh_mBOND: return true;
    case Unit::sh_uBOND: return true;
    case Unit::sh_sBOND: return true;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

BitcoinUnit BitcoinUnits::getUnitOfScaleType(Unit unit, bool isScaled)
{
    return (BitcoinUnit)(static_cast<int>(unit) % 8 + 8 * (!isScaled));
}

QString BitcoinUnits::format(Unit unit, const CAmount& nIn, bool fPlus, SeparatorStyle separators, bool justify)
{
    // Note: not using straight sprintf here because we do NOT want
    // localized number formatting.
    qint64 n = (qint64)nIn;
    qint64 coin = factor(unit);
    int num_decimals = decimals(unit);
    qint64 n_abs = (n > 0 ? n : -n);
    qint64 quotient = n_abs / coin;
    QString quotient_str = QString::number(quotient);
    if (justify) {
        quotient_str = quotient_str.rightJustified(MAX_DIGITS_BTC - num_decimals, ' ');
    }

    // Use SI-style thin space separators as these are locale independent and can't be
    // confused with the decimal marker.
    QChar thin_sp(THIN_SP_CP);
    int q_size = quotient_str.size();
    if (separators == SeparatorStyle::ALWAYS || (separators == SeparatorStyle::STANDARD && q_size > 4))
        for (int i = 3; i < q_size; i += 3)
            quotient_str.insert(q_size - i, thin_sp);

    if (n < 0)
        quotient_str.insert(0, '-');
    else if (fPlus && n > 0)
        quotient_str.insert(0, '+');

    if (num_decimals > 0) {
        qint64 remainder = n_abs % coin;
        QString remainder_str = QString::number(remainder).rightJustified(num_decimals, '0');
        return quotient_str + QString(".") + remainder_str;
    } else {
        return quotient_str;
    }
}


// NOTE: Using formatWithUnit in an HTML context risks wrapping
// quantities at the thousands separator. More subtly, it also results
// in a standard space rather than a thin space, due to a bug in Qt's
// XML whitespace canonicalisation
//
// Please take care to use formatHtmlWithUnit instead, when
// appropriate.

QString BitcoinUnits::formatWithUnit(Unit unit, const CAmount& amount, bool plussign, SeparatorStyle separators)
{
    return format(unit, amount, plussign, separators) + QString(" ") + shortName(unit);
}

QString BitcoinUnits::formatHtmlWithUnit(Unit unit, const CAmount& amount, bool plussign, SeparatorStyle separators)
{
    QString str(formatWithUnit(unit, amount, plussign, separators));
    str.replace(QChar(THIN_SP_CP), QString(THIN_SP_HTML));
    return QString("<span style='white-space: nowrap;'>%1</span>").arg(str);
}

QString BitcoinUnits::formatWithPrivacy(Unit unit, const CAmount& amount, SeparatorStyle separators, bool privacy)
{
    assert(amount >= 0);
    QString value;
    if (privacy) {
        value = format(unit, 0, false, separators, true).replace('0', '#');
    } else {
        value = format(unit, amount, false, separators, true);
    }
    return value + QString(" ") + shortName(unit);
}

bool BitcoinUnits::parse(Unit unit, const QString& value, CAmount* val_out)
{
    if (value.isEmpty()) {
        return false; // Refuse to parse invalid unit or empty string
    }
    int num_decimals = decimals(unit);

    // Ignore spaces and thin spaces when parsing
    QStringList parts = removeSpaces(value).split(".");

    if(parts.size() > 2)
    {
        return false; // More than one dot
    }
    QString whole = parts[0];
    QString decimals;

    if(parts.size() > 1)
    {
        decimals = parts[1];
    }
    if(decimals.size() > num_decimals)
    {
        return false; // Exceeds max precision
    }
    bool ok = false;
    QString str = whole + decimals.leftJustified(num_decimals, '0');

    if(str.size() > 18)
    {
        return false; // Longer numbers will exceed 63 bits
    }
    CAmount retvalue(str.toLongLong(&ok));
    if(val_out)
    {
        *val_out = retvalue;
    }
    return ok;
}

QString BitcoinUnits::getAmountColumnTitle(Unit unit)
{
    return QObject::tr("Amount") + " (" + shortName(unit) + ")";
}

int BitcoinUnits::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return displayAll ? 16 : 4;
}

int BitcoinUnits::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 4;
}

QVariant BitcoinUnits::data(const QModelIndex &index, int role) const
{
    int row = index.row();
    int col = displayAll ? 0 : index.column(); // ignore column if displaying all
    if(row >= 0 && row < (displayAll ? 16 : 4))
    {
        Unit unit = unitlist.at(row + col * 4);
        switch(role)
        {
        case Qt::EditRole:
        case Qt::DisplayRole:
            return QVariant(longName(unit));
        case Qt::ToolTipRole:
            return QVariant(description(unit));
        case UnitRole:
            return QVariant::fromValue(unit);
        }
    }
    return QVariant();
}

CAmount BitcoinUnits::maxMoney()
{
    return MAX_MONEY;
}

namespace {
qint8 ToQint8(BitcoinUnit unit)
{
    switch (unit) {
    case BitcoinUnit::CASH: return 0;
    case BitcoinUnit::mCASH: return 1;
    case BitcoinUnit::uCASH: return 2;
    case BitcoinUnit::sCASH: return 3;
    case BitcoinUnit::BOND: return 4;
    case BitcoinUnit::mBOND: return 5;
    case BitcoinUnit::uBOND: return 6;
    case BitcoinUnit::sBOND: return 7;
    case BitcoinUnit::sh_CASH: return 8;
    case BitcoinUnit::sh_mCASH: return 9;
    case BitcoinUnit::sh_uCASH: return 10;
    case BitcoinUnit::sh_sCASH: return 11;
    case BitcoinUnit::sh_BOND: return 12;
    case BitcoinUnit::sh_mBOND: return 13;
    case BitcoinUnit::sh_uBOND: return 14;
    case BitcoinUnit::sh_sBOND: return 15;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

BitcoinUnit FromQint8(qint8 num)
{
    switch (num) {
    case 0: return BitcoinUnit::CASH;
    case 1: return BitcoinUnit::mCASH;
    case 2: return BitcoinUnit::uCASH;
    case 3: return BitcoinUnit::sCASH;
    case 4: return BitcoinUnit::BOND;
    case 5: return BitcoinUnit::mBOND;
    case 6: return BitcoinUnit::uBOND;
    case 7: return BitcoinUnit::sBOND;
    case 8: return BitcoinUnit::sh_CASH;
    case 9: return BitcoinUnit::sh_mCASH;
    case 10: return BitcoinUnit::sh_uCASH;
    case 11: return BitcoinUnit::sh_sCASH;
    case 12: return BitcoinUnit::sh_BOND;
    case 13: return BitcoinUnit::sh_mBOND;
    case 14: return BitcoinUnit::sh_uBOND;
    case 15: return BitcoinUnit::sh_sBOND;
    }
    assert(false);
}
} // namespace

QDataStream& operator<<(QDataStream& out, const BitcoinUnit& unit)
{
    return out << ToQint8(unit);
}

QDataStream& operator>>(QDataStream& in, BitcoinUnit& unit)
{
    qint8 input;
    in >> input;
    unit = FromQint8(input);
    return in;
}
