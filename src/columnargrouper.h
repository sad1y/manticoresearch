//
// Copyright (c) 2020-2022, Manticore Software LTD (http://manticoresearch.com)
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#ifndef _columnargrouper_
#define _columnargrouper_

#include "collation.h"
struct CSphColumnInfo;

class CSphGrouper;
CSphGrouper * CreateGrouperColumnarInt ( const CSphColumnInfo & tAttr );
CSphGrouper * CreateGrouperColumnarString ( const CSphColumnInfo & tAttr, ESphCollation eCollation );
CSphGrouper * CreateGrouperColumnarMVA ( const CSphColumnInfo & tAttr );

#endif // _columnargrouper_
