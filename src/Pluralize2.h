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
#ifndef PLURALIZE2_H
#define PLURALIZE2_H

#include <QDebug>
#include <QString>

class Pluralize2
{
public:
   Pluralize2(int n, const QString &wordIn);

   QString _wordIn;
   int _n;
};

QDebug operator<<(QDebug dbg, const Pluralize2 &p);

#endif // PLURALIZE2_H
