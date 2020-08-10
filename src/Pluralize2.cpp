//
// Fulcrum - A fast & nimble SPV Server for Bitcoin Cash
// Copyright (C) 2019-2020  Calin A. Culianu <calin.culianu@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program (see LICENSE.txt).  If not, see
// <https://www.gnu.org/licenses/>.
//
#include "Pluralize2.h"

Pluralize2::Pluralize2(int n, const QString &wordIn) {
    _wordIn = wordIn;
    _n = n;
}

QDebug operator<<(QDebug dbg, const Pluralize2 &p)
{
    QDebugStateSaver saver(dbg);
    dbg.resetFormat();
    dbg.noquote();
    if (qAbs(p._n) != 1) {
        QString word(p._wordIn);
        QString ending = QStringLiteral("s"); // default to 's' ending
        const auto wordend = word.right(2);
        // 's' or "sh" sound in English are pluralized with "es" rather than simple "s"
        // 'y' endings have the 'y' truncated and 'ies' appended in its stead for plurals as well.
        // TODO: suppored ALL CAPS? Not needed for now in this app, so we don't bother...
        if (wordend.endsWith('s') || wordend == QStringLiteral("sh"))
            ending = QStringLiteral("es");
        else if (wordend.endsWith('y')) {
            word.truncate(word.length()-1);  // remove training 'y'
            ending = QStringLiteral("ies");  // .. append 'ies' eg entry -> entries
        }
        dbg.nospace() << p._n << " " << word << ending;
    } else
        dbg << p._n << p._wordIn; // constant time copy (implicitly shared)

    return dbg;
}
