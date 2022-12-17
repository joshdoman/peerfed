// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/bitcoinunits.h>

#include <consensus/amount.h>

#include <QStringList>

#include <cassert>

static constexpr auto MAX_DIGITS_BTC = 16;

BitcoinUnits::BitcoinUnits(QObject *parent):
        QAbstractListModel(parent),
        unitlist(availableUnits())
{
}

QList<BitcoinUnit> BitcoinUnits::availableUnits()
{
    QList<BitcoinUnit> unitlist;
    unitlist.append(Unit::CASH);
    unitlist.append(Unit::BOND);
    unitlist.append(Unit::mCASH);
    unitlist.append(Unit::mBOND);
    unitlist.append(Unit::uCASH);
    unitlist.append(Unit::uBOND);
    unitlist.append(Unit::sCASH);
    unitlist.append(Unit::sBOND);
    return unitlist;
}

QString BitcoinUnits::longName(Unit unit)
{
    switch (unit) {
    case Unit::CASH: return QString("CASH");
    case Unit::BOND: return QString("BOND");
    case Unit::mCASH: return QString("mCASH");
    case Unit::mBOND: return QString("mBOND");
    case Unit::uCASH: return QString::fromUtf8("µCASH (bits-c)");
    case Unit::uBOND: return QString::fromUtf8("µBOND (bits-b)");
    case Unit::sCASH: return QString("sCASH (sat-c)");
    case Unit::sBOND: return QString("sBOND (sat-c)");
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

QString BitcoinUnits::shortName(Unit unit)
{
    switch (unit) {
    case Unit::CASH: return longName(unit);
    case Unit::BOND: return longName(unit);
    case Unit::mCASH: return longName(unit);
    case Unit::mBOND: return longName(unit);
    case Unit::uCASH: return QString("bits-c");
    case Unit::uBOND: return QString("bits-b");
    case Unit::sCASH: return QString("sat-c");
    case Unit::sBOND: return QString("sat-b");
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

QString BitcoinUnits::description(Unit unit)
{
    switch (unit) {
    case Unit::CASH: return QString("Cash");
    case Unit::BOND: return QString("Bonds");
    case Unit::mCASH: return QString("Milli-Cash (1 / 1" THIN_SP_UTF8 "000)");
    case Unit::mBOND: return QString("Milli-Bonds (1 / 1" THIN_SP_UTF8 "000)");
    case Unit::uCASH: return QString("Micro-Cash (bits-c) (1 / 1" THIN_SP_UTF8 "000" THIN_SP_UTF8 "000)");
    case Unit::uBOND: return QString("Micro-Bonds (bits-b) (1 / 1" THIN_SP_UTF8 "000" THIN_SP_UTF8 "000)");
    case Unit::sCASH: return QString("Satoshi-Cash (sat-c) (1 / 100" THIN_SP_UTF8 "000" THIN_SP_UTF8 "000)");
    case Unit::sBOND: return QString("Satoshi-Bonds (sat-b) (1 / 100" THIN_SP_UTF8 "000" THIN_SP_UTF8 "000)");
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

qint64 BitcoinUnits::factor(Unit unit)
{
    switch (unit) {
    case Unit::CASH: return 100'000'000;
    case Unit::BOND: return 100'000'000;
    case Unit::mCASH: return 100'000;
    case Unit::mBOND: return 100'000;
    case Unit::uCASH: return 100;
    case Unit::uBOND: return 100;
    case Unit::sCASH: return 1;
    case Unit::sBOND: return 1;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

int BitcoinUnits::decimals(Unit unit)
{
    switch (unit) {
    case Unit::CASH: return 8;
    case Unit::BOND: return 8;
    case Unit::mCASH: return 5;
    case Unit::mBOND: return 5;
    case Unit::uCASH: return 2;
    case Unit::uBOND: return 2;
    case Unit::sCASH: return 0;
    case Unit::sBOND: return 0;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

CAmountType BitcoinUnits::type(Unit unit)
{
    switch (unit) {
    case Unit::CASH: return CASH;
    case Unit::BOND: return BOND;
    case Unit::mCASH: return CASH;
    case Unit::mBOND: return BOND;
    case Unit::uCASH: return CASH;
    case Unit::uBOND: return BOND;
    case Unit::sCASH: return CASH;
    case Unit::sBOND: return BOND;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

BitcoinUnit BitcoinUnits::unitOfType(Unit unit, CAmountType type)
{
    return (BitcoinUnit)((static_cast<int>(unit) / 2) * 2 + type);
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
    return unitlist.size();
}

QVariant BitcoinUnits::data(const QModelIndex &index, int role) const
{
    int row = index.row();
    if(row >= 0 && row < unitlist.size())
    {
        Unit unit = unitlist.at(row);
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
    case BitcoinUnit::BOND: return 1;
    case BitcoinUnit::mCASH: return 2;
    case BitcoinUnit::mBOND: return 3;
    case BitcoinUnit::uCASH: return 4;
    case BitcoinUnit::uBOND: return 5;
    case BitcoinUnit::sCASH: return 6;
    case BitcoinUnit::sBOND: return 7;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

BitcoinUnit FromQint8(qint8 num)
{
    switch (num) {
    case 0: return BitcoinUnit::CASH;
    case 1: return BitcoinUnit::BOND;
    case 2: return BitcoinUnit::mCASH;
    case 3: return BitcoinUnit::mBOND;
    case 4: return BitcoinUnit::uCASH;
    case 5: return BitcoinUnit::uBOND;
    case 6: return BitcoinUnit::sCASH;
    case 7: return BitcoinUnit::sBOND;
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
