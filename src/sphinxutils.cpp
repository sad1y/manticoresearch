//
// Copyright (c) 2017-2022, Manticore Software LTD (https://manticoresearch.com)
// Copyright (c) 2001-2016, Andrew Aksyonoff
// Copyright (c) 2008-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

/// @file sphinxutils.cpp
/// Implementations for Sphinx utilities shared classes.

#include "sphinxutils.h"
#include "sphinxint.h"
#include "sphinxplugin.h"
#include "sphinxstem.h"
#include "icu.h"
#include "fileutils.h"
#include "threadutils.h"
#include "indexfiles.h"

#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#if HAVE_EXECINFO_H
#include <execinfo.h>
#endif

#if _WIN32
#include <io.h> // for ::open on windows
#include <dbghelp.h>
#pragma comment(linker, "/defaultlib:dbghelp.lib")
#pragma message("Automatically linking with dbghelp.lib")
#else
#include <sys/wait.h>
#include <signal.h>
#include <glob.h>
#endif

#if HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#include "libutils.h"

//////////////////////////////////////////////////////////////////////////
// STRING FUNCTIONS
//////////////////////////////////////////////////////////////////////////

static char * ltrim ( char * sLine )
{
	while ( *sLine && sphIsSpace(*sLine) )
		sLine++;
	return sLine;
}


static char * rtrim ( char * sLine )
{
	char * p = sLine + strlen(sLine) - 1;
	while ( p>=sLine && sphIsSpace(*p) )
		p--;
	p[1] = '\0';
	return sLine;
}


static char * trim ( char * sLine )
{
	return ltrim ( rtrim ( sLine ) );
}

// split alnums by non-alnums symbols
// (alnums are  [0..9a..zA..Z-_])
void sphSplit ( StrVec_t & dOut, const char * sIn )
{
	if ( !sIn )
		return;

	const char * p = sIn;
	while ( *p )
	{
		// skip non-alphas
		while ( (*p) && !sphIsAlpha(*p) )
			p++;
		if ( !(*p) )
			break;

		// this is my next token
		assert ( sphIsAlpha(*p) );
		const char * sNext = p;
		while ( sphIsAlpha(*p) )
			p++;
		if ( sNext!=p )
			dOut.Add().SetBinary ( sNext, int (p-sNext) );
	}
}

void sphSplitApply ( const char * sIn, int iSize, StrFunctor &&dFunc )
{
	if ( !sIn )
		return;

	if (!dFunc)
		return;

	const char * p = sIn;
	if ( iSize<0 ) iSize = (int) strlen (p);
	const char * pEnd = p + iSize;
	while ( p < pEnd )
	{
		// skip non-alphas
		while ( ( p<pEnd ) && !sphIsAlpha ( *p ) )
			p++;
		if ( p>=pEnd )
			break;

		// this is my next token
		assert ( sphIsAlpha ( *p ) );
		const char * sNext = p;
		while ( ( p<pEnd ) && sphIsAlpha ( *p ) )
			++p;
		if ( sNext!=p )
			dFunc ( sNext, int ( p - sNext ) );
	}
}


// split by any char from sBounds.
// if line starts from a bound char, first splitted str will be an empty string
void sphSplit ( StrVec_t & dOut, const char * sIn, int iLen, const char * sBounds )
{
	sph::Split ( sIn, iLen, sBounds, [&] ( const char * sToken, int iTokenLen ) {
		dOut.Add ().SetBinary ( sToken, iTokenLen );
	} );
}

StrVec_t sphSplit ( const char * sIn, int iLen, const char * sBounds )
{
	StrVec_t dResult;
	sphSplit ( dResult, sIn, iLen, sBounds );
	return dResult;
}

void sphSplit ( StrVec_t & dOut, const char * sIn, const char * sBounds )
{
	sphSplit ( dOut, sIn, -1, sBounds );
}

StrVec_t sphSplit( const char* sIn, const char* sBounds )
{
	return sphSplit ( sIn, -1, sBounds);
}

template < typename T1, typename T2 >
static bool sphWildcardMatchRec ( const T1 * sString, const T2 * sPattern )
{
	if ( !sString || !sPattern )
		return false;

	const T1 * s = sString;
	const T2 * p = sPattern;
	while ( *s )
	{
		switch ( *p )
		{
		case '\\':
			// escaped char, strict match the next one literally
			p++;
			if ( *s++!=*p++ )
				return false;
			break;

		case '?':
			// match any character
			s++;
			p++;
			break;

		case '%':
			// gotta match either 0 or 1 characters
			// well, lets look ahead and see what we need to match next
			p++;

			// just a shortcut, %* can be folded to just *
			if ( *p=='*' )
				break;

			// plain char after a hash? check the non-ambiguous cases
			if ( !sphIsWild(*p) )
			{
				if ( s[0]!=*p )
				{
					// hash does not match 0 chars
					// check if we can match 1 char, or it's a no-match
					if ( s[1]!=*p )
						return false;
					s++;
					break;
				} else
				{
					// hash matches 0 chars
					// check if we could ambiguously match 1 char too, though
					if ( s[1]!=*p )
						break;
					// well, fall through to "scan both options" route
				}
			}

			// could not decide yet
			// so just recurse both options
			if ( sphWildcardMatchRec ( s, p ) || sphWildcardMatchRec ( s+1, p ) )
				return true;
			return false;

		case '*':
			// skip all the extra stars and question marks
			for ( p++; *p=='*' || *p=='?'; p++ )
				if ( *p=='?' )
				{
					s++;
					if ( !*s )
						return p[1]=='\0';
				}

			// short-circuit trailing star
			if ( !*p )
				return true;

			// so our wildcard expects a real character
			// scan forward for its occurrences and recurse
			while (true)
			{
				if ( !*s )
					return false;
				if ( *s==*p && sphWildcardMatchRec ( s+1, p+1 ) )
					return true;
				s++;
			}
			break;

		default:
			// default case, strict match
			if ( *s++!=*p++ )
				return false;
			break;
		}
	}

	// eliminate trailing stars
	while ( *p=='*' )
		p++;

	// string done
	// pattern should be either done too, or a trailing star, or a trailing hash
	return p[0]=='\0'
		|| ( p[0]=='*' && p[1]=='\0' )
		|| ( p[0]=='%' && p[1]=='\0' );
}

template < typename T1, typename T2 >
static bool sphWildcardMatchDP ( const T1 * sString, const T2 * sPattern )
{
	assert ( sString && sPattern && *sString && *sPattern );

	const T1 * s = sString;
	const T2 * p = sPattern;
	bool bEsc = false;
	int iEsc = 0;

	const int iBufCount = 2;
	const int iBufLenMax = SPH_MAX_WORD_LEN*3+4+1;
	int dTmp [iBufCount][iBufLenMax];
	dTmp[0][0] = 1;
	dTmp[1][0] = 0;
	for ( int i=0; i<iBufLenMax; i++ )
		dTmp[0][i] = 1;

	while ( *p )
	{
		// count, flag and skip escape char
		if ( *p=='\\' )
		{
			iEsc++;
			p++;
			bEsc = true;
			continue;
		}

		s = sString;
		int iPattern = int (p - sPattern) + 1 - iEsc;
		int iPrev = ( iPattern + 1 ) % iBufCount;
		int iCur = iPattern % iBufCount;

		// check the 1st wildcard
		if ( !bEsc && ( *p=='*' || *p=='%' ) )
		{
			dTmp[iCur][0] = dTmp[iPrev][0];

		} else
		{
			dTmp[iCur][0] = 0;
		}

		while ( *s )
		{
			int j = int (s - sString) + 1;
			if ( !bEsc && *p=='*' )
			{
				dTmp[iCur][j] = dTmp[iPrev][j-1] || dTmp[iCur][j-1] || dTmp[iPrev][j];
			} else if ( !bEsc && *p=='%' )
			{
				dTmp[iCur][j] = dTmp[iPrev][j-1] || dTmp[iPrev][j];
			} else if ( *p==*s || ( !bEsc && *p=='?' ) )
			{
				dTmp[iCur][j] = dTmp[iPrev][j-1];
			} else
			{
				dTmp[iCur][j] = 0;
			}
			s++;
		}
		p++;
		bEsc = false;
	}

	return ( dTmp[( p-sPattern-iEsc ) % iBufCount][s-sString]!=0 );
}


template < typename T1, typename T2 >
bool sphWildcardMatchSpec ( const T1 * sString, const T2 * sPattern )
{
	int iLen = 0;
	int iStars = 0;
	const T2 * p = sPattern;
	while ( *p )
	{
		iStars += ( *p=='*' );
		iLen++;
		p++;
	}

	if ( iStars>10 || ( iStars>5 && iLen>17 ) )
		return sphWildcardMatchDP ( sString, sPattern );
	else
		return sphWildcardMatchRec ( sString, sPattern );
}


bool sphWildcardMatch ( const char * sString, const char * sPattern, const int * pPattern )
{
	if ( !sString || !sPattern || !*sString || !*sPattern )
		return false;

	// there are basically 4 codepaths, because both string and pattern may or may not contain utf-8 chars
	// pPattern and pString are pointers to unpacked utf-8, pPattern can be precalculated (default is NULL)

	int dString [ SPH_MAX_WORD_LEN + 1 ];
	const int * pString = ( sphIsUTF8 ( sString ) && sphUTF8ToWideChar ( sString, dString, SPH_MAX_WORD_LEN ) ) ? dString : nullptr;

	if ( !pString && !pPattern )
		return sphWildcardMatchSpec ( sString, sPattern ); // ascii vs ascii

	if ( pString && !pPattern )
		return sphWildcardMatchSpec ( pString, sPattern ); // utf-8 vs ascii

	if ( !pString && pPattern )
		return sphWildcardMatchSpec ( sString, pPattern ); // ascii vs utf-8

//	if ( pString && pPattern )
	return sphWildcardMatchSpec ( pString, pPattern ); // utf-8 vs utf-8

//	return false; // dead, but causes warn either by compiler, either by analysis. Leave as is.
}

//////////////////////////////////////////////////////////////////////////
// cases are covered by TEST (functions, size_parser) gtest_functions.cpp
int64_t sphGetSize64 ( const char * sValue, char ** ppErr, int64_t iDefault )
{
	if ( !sValue )
		return iDefault;

	if ( !strlen(sValue) )
		return iDefault;

	char * sEnd;
	int64_t iRes = strtoll ( sValue, &sEnd, 10 );

	switch ( *sEnd )
	{
	case 't': case 'T':	iRes *= 1024;
	// [[clang::fallthrough]];
	case 'g': case 'G':	iRes *= 1024;
	// [[clang::fallthrough]];
	case 'm': case 'M':	iRes *= 1024;
	// [[clang::fallthrough]];
	case 'k': case 'K':
		iRes *= 1024;
		++sEnd;
		break;

	case '\0': break;
	default:
		// an error happened; write address to ppErr
		if ( ppErr )
			*ppErr = sEnd;
		iRes = iDefault;
		break;
	}
	return iRes;
}

// cases are covered by TEST ( functions, time_parser ) gtest_functions.cpp
int64_t sphGetTime64 ( const char* sValue, char** ppErr, int64_t iDefault )
{
	if ( !sValue )
		return iDefault;

	if ( !strlen ( sValue ) )
		return iDefault;

	char* sEnd;
	int64_t iRes = strtoll ( sValue, &sEnd, 10 );

	switch ( *sEnd )
	{
		case 'w': case 'W' : iRes *= 7; // passthrow
		// [[clang::fallthrough]];
		case 'd': case 'D' : iRes *= 24; // passthrow
		// [[clang::fallthrough]];
		case 'h': case 'H' : iRes *= 3600 * 1000000LL; break;
		case 'm': case 'M':
			switch ( sEnd[1] )
			{
				case 's': case 'S' : iRes *= 1000; break;
				default: iRes *= 1000000*60; break;
			}
			break;

		case 'u': case 'U':
			switch ( sEnd[1] )
			{
				case 's': case 'S' : break; // no multiplier for useconds
				default:
					if ( ppErr )
						*ppErr = sEnd;
					iRes = iDefault;
			}
			break;

		case 's': case 'S': case '\0' : iRes *= 1000000; break; // sec is default
		default:
			if ( ppErr )
				*ppErr = sEnd;
			iRes = iDefault;
	}
	return iRes;
}


void CSphConfigSection::AddEntry ( const char * szKey, const char * szValue )
{
	int iTag = m_iTag;
	m_iTag++;
	if ( (*this)(szKey) )
	{
		if ( (*this)[szKey].m_bTag )
		{
			// override value or list with a new value
			SafeDelete ( (*this)[szKey].m_pNext ); // only leave the first array element
			(*this)[szKey] = CSphVariant ( szValue, iTag ); // update its value
			(*this)[szKey].m_bTag = false; // mark it as overridden

		} else
		{
			// chain to tail, to keep the order
			CSphVariant * pTail = &(*this)[szKey];
			while ( pTail->m_pNext )
				pTail = pTail->m_pNext;
			pTail->m_pNext = new CSphVariant ( szValue, iTag );
		}

	} else
	{
		// just add
		Add ( CSphVariant ( szValue, iTag ), szKey ); // FIXME! be paranoid, verify that it returned true
	}
}


int64_t CSphConfigSection::GetSize64 ( const char * sKey, int64_t iDefault ) const
{
	CSphVariant * pEntry = (*this)( sKey );
	if ( !pEntry )
	{
		sphLogDebug ( "'%s' - nothing specified, using default value " INT64_FMT, sKey, iDefault );
		return iDefault;
	}

	char * sErr = nullptr;
	auto iRes = sphGetSize64 ( pEntry->cstr(), &sErr, iDefault );

	if ( sErr && *sErr )
	{
		sphWarning ( "'%s = %s' parse error '%s'", sKey, pEntry->cstr(), sErr );
		iRes = iDefault;
	}
	return iRes;
}

int CSphConfigSection::GetSize ( const char * sKey, int iDefault ) const
{
	int64_t iSize = GetSize64 ( sKey, iDefault );
	if ( iSize>INT_MAX )
	{
		sphWarning ( "'%s = " INT64_FMT "' clamped to %d(INT_MAX)", sKey, iSize, INT_MAX );
		iSize = INT_MAX;
	}
	return (int)iSize;
}

int64_t CSphConfigSection::GetUsTime64S ( const char* sKey, int64_t iDefault ) const
{
	CSphVariant* pEntry = ( *this ) ( sKey );
	if ( !pEntry )
	{
		sphLogDebug ( "'%s' - nothing specified, using default value " INT64_FMT, sKey, iDefault );
		return iDefault;
	}

	char* sErr = nullptr;
	auto iRes = sphGetTime64 ( pEntry->cstr (), &sErr, iDefault );

	if ( sErr && *sErr )
	{
		sphWarning ( "'%s = %s' parse error '%s'", sKey, pEntry->cstr (), sErr );
		iRes = iDefault;
	}
	return iRes;
}

int64_t CSphConfigSection::GetUsTime64Ms ( const char* sKey, int64_t iDefault ) const
{
	CSphVariant* pEntry = ( *this ) ( sKey );
	if ( !pEntry )
	{
		sphLogDebug ( "'%s' - nothing specified, using default value " INT64_FMT, sKey, iDefault );
		return iDefault;
	}
	StringBuilder_c sTmp;
	sTmp << pEntry->strval () << "ms";
	char* sErr = nullptr;
	auto iRes = sphGetTime64 ( sTmp.cstr (), &sErr, iDefault );

	if ( sErr && *sErr )
	{
		sphWarning ( "'%s = %s' parse error '%s'", sKey, sTmp.cstr (), sErr );
		iRes = iDefault;
	}
	return iRes;
}

int CSphConfigSection::GetSTimeS ( const char* sKey, int iDefault ) const
{
	int64_t iTimeUs = GetUsTime64S ( sKey, iDefault*1000000 ) / 1000000ll;
	if ( iTimeUs>INT_MAX )
	{
		sphWarning ( "'%s = " INT64_FMT "' clamped to %d(INT_MAX)", sKey, iTimeUs, INT_MAX );
		iTimeUs = INT_MAX;
	}
	return ( int ) iTimeUs;
}

int CSphConfigSection::GetMsTimeMs ( const char* sKey, int iDefault ) const
{
	int64_t iTimeUs = GetUsTime64Ms ( sKey, iDefault*1000 ) / 1000ll;
	if ( iTimeUs>INT_MAX )
	{
		sphWarning ( "'%s = " INT64_FMT "' clamped to %d(INT_MAX)", sKey, iTimeUs, INT_MAX );
		iTimeUs = INT_MAX;
	}
	return ( int ) iTimeUs;
}

//////////////////////////////////////////////////////////////////////////
// CONFIG PARSER
//////////////////////////////////////////////////////////////////////////

/// key flags
enum
{
	KEY_DEPRECATED		= 1UL<<0,
	KEY_LIST			= 1UL<<1,
	KEY_HIDDEN			= 1UL<<2,
	KEY_REMOVED			= 1UL<<3
};

/// key descriptor for validation purposes
struct KeyDesc_t
{
	const char *		m_sKey;		///< key name
	int					m_iFlags;	///< flags
	const char *		m_sExtra;	///< extra stuff (deprecated name, for now)
};

/// Mandatory pattern: KeyDesc_t g_dKeys...
/// It is searched by doc/check.pl when parsing the file
/// and used to determine and collect all options

/// allowed keys for source section
static KeyDesc_t g_dKeysSource[] =
{
	{ "type",					0, NULL },
	{ "sql_host",				0, NULL },
	{ "sql_user",				0, NULL },
	{ "sql_pass",				0, NULL },
	{ "sql_db",					0, NULL },
	{ "sql_port",				0, NULL },
	{ "sql_sock",				0, NULL },
	{ "mysql_connect_flags",	0, NULL },
	{ "mysql_ssl_key",			0, NULL }, // check.pl mysql_ssl
	{ "mysql_ssl_cert",			0, NULL }, // check.pl mysql_ssl
	{ "mysql_ssl_ca",			0, NULL }, // check.pl mysql_ssl
	{ "mssql_winauth",			0, NULL },
	{ "mssql_unicode",			KEY_REMOVED, NULL },
	{ "sql_query_pre",			KEY_LIST, NULL },
	{ "sql_query",				0, NULL },
	{ "sql_query_range",		0, NULL },
	{ "sql_range_step",			0, NULL },
	{ "sql_query_killlist",		0, NULL },
	{ "sql_attr_uint",			KEY_LIST, NULL },
	{ "sql_attr_bool",			KEY_LIST, NULL },
	{ "sql_attr_timestamp",		KEY_LIST, NULL },
	{ "sql_attr_str2ordinal",	KEY_REMOVED | KEY_LIST, NULL },
	{ "sql_attr_float",			KEY_LIST, NULL },
	{ "sql_attr_bigint",		KEY_LIST, NULL },
	{ "sql_attr_multi",			KEY_LIST, NULL },
	{ "sql_query_post",			KEY_LIST, NULL },
	{ "sql_query_post_index",	KEY_LIST, NULL },
	{ "sql_ranged_throttle",	0, NULL },
	{ "sql_query_info",			KEY_REMOVED, NULL },
	{ "xmlpipe_command",		0, NULL },
	{ "xmlpipe_field",			KEY_LIST, NULL },
	{ "xmlpipe_attr_uint",		KEY_LIST, NULL },
	{ "xmlpipe_attr_timestamp",	KEY_LIST, NULL },
	{ "xmlpipe_attr_str2ordinal", KEY_REMOVED | KEY_LIST, NULL },
	{ "xmlpipe_attr_bool",		KEY_LIST, NULL },
	{ "xmlpipe_attr_float",		KEY_LIST, NULL },
	{ "xmlpipe_attr_bigint",	KEY_LIST, NULL },
	{ "xmlpipe_attr_multi",		KEY_LIST, NULL },
	{ "xmlpipe_attr_multi_64",	KEY_LIST, NULL },
	{ "xmlpipe_attr_string",	KEY_LIST, NULL },
	{ "xmlpipe_attr_wordcount", KEY_REMOVED | KEY_LIST, NULL },
	{ "xmlpipe_attr_json",		KEY_LIST, NULL },
	{ "xmlpipe_field_string",	KEY_LIST, NULL },
	{ "xmlpipe_field_wordcount", KEY_REMOVED | KEY_LIST, NULL },
	{ "xmlpipe_fixup_utf8",		0, NULL },
	{ "sql_str2ordinal_column", KEY_LIST | KEY_REMOVED, NULL },
	{ "unpack_zlib",			KEY_LIST, NULL },
	{ "unpack_mysqlcompress",	KEY_LIST, NULL },
	{ "unpack_mysqlcompress_maxsize", 0, NULL },
	{ "odbc_dsn",				0, NULL },
	{ "sql_joined_field",		KEY_LIST, NULL },
	{ "sql_attr_string",		KEY_LIST, NULL },
	{ "sql_attr_str2wordcount", KEY_REMOVED | KEY_LIST, NULL },
	{ "sql_field_string",		KEY_LIST, NULL },
	{ "sql_field_str2wordcount", KEY_REMOVED | KEY_LIST, NULL },
	{ "sql_file_field",			KEY_LIST, NULL },
	{ "sql_column_buffers",		0, NULL },
	{ "sql_attr_json",			KEY_LIST, NULL },
	{ "hook_connect",			KEY_HIDDEN, NULL },
	{ "hook_query_range",		KEY_HIDDEN, NULL },
	{ "hook_post_index",		KEY_HIDDEN, NULL },
	{ "tsvpipe_command",		0, NULL },
	{ "tsvpipe_field",			KEY_LIST, NULL },
	{ "tsvpipe_attr_uint",		KEY_LIST, NULL },
	{ "tsvpipe_attr_timestamp",	KEY_LIST, NULL },
	{ "tsvpipe_attr_bool",		KEY_LIST, NULL },
	{ "tsvpipe_attr_float",		KEY_LIST, NULL },
	{ "tsvpipe_attr_bigint",	KEY_LIST, NULL },
	{ "tsvpipe_attr_multi",		KEY_LIST, NULL },
	{ "tsvpipe_attr_multi_64",	KEY_LIST, NULL },
	{ "tsvpipe_attr_string",	KEY_LIST, NULL },
	{ "tsvpipe_attr_json",		KEY_LIST, NULL },
	{ "tsvpipe_field_string",	KEY_LIST, NULL },
	{ "csvpipe_command",		0, NULL },
	{ "csvpipe_field",			KEY_LIST, NULL },
	{ "csvpipe_attr_uint",		KEY_LIST, NULL },
	{ "csvpipe_attr_timestamp",	KEY_LIST, NULL },
	{ "csvpipe_attr_bool",		KEY_LIST, NULL },
	{ "csvpipe_attr_float",		KEY_LIST, NULL },
	{ "csvpipe_attr_bigint",	KEY_LIST, NULL },
	{ "csvpipe_attr_multi",		KEY_LIST, NULL },
	{ "csvpipe_attr_multi_64",	KEY_LIST, NULL },
	{ "csvpipe_attr_string",	KEY_LIST, NULL },
	{ "csvpipe_attr_json",		KEY_LIST, NULL },
	{ "csvpipe_field_string",	KEY_LIST, NULL },
	{ "csvpipe_delimiter",		0, NULL },
	{ NULL,						0, NULL }
};

/// allowed keys for index section
static KeyDesc_t g_dKeysIndex[] =
{
	{ "source",					KEY_LIST, NULL },
	{ "path",					0, NULL },
	{ "docinfo",				KEY_REMOVED, NULL },
	{ "mlock",					KEY_DEPRECATED, "mlock in particular access_... option" },
	{ "morphology",				0, NULL },
	{ "stopwords",				0, NULL },
	{ "exceptions",				0, NULL },
	{ "wordforms",				KEY_LIST, NULL },
	{ "embedded_limit",			0, NULL },
	{ "min_word_len",			0, NULL },
	{ "charset_type",			KEY_REMOVED, NULL },
	{ "charset_table",			0, NULL },
	{ "ignore_chars",			0, NULL },
	{ "min_prefix_len",			0, NULL },
	{ "min_infix_len",			0, NULL },
	{ "max_substring_len",		KEY_DEPRECATED, "dict=keywords" },
	{ "prefix_fields",			0, NULL },
	{ "infix_fields",			0, NULL },
	{ "enable_star",			KEY_REMOVED, NULL },
	{ "ngram_len",				0, NULL },
	{ "ngram_chars",			0, NULL },
	{ "phrase_boundary",		0, NULL },
	{ "phrase_boundary_step",	0, NULL },
	{ "ondisk_dict",			KEY_REMOVED, NULL },
	{ "type",					0, NULL },
	{ "local",					KEY_LIST, NULL },
	{ "agent",					KEY_LIST, NULL },
	{ "agent_blackhole",		KEY_LIST, NULL },
	{ "agent_persistent",		KEY_LIST, NULL },
	{ "agent_retry_count",		0, NULL },
	{ "mirror_retry_count",		0, NULL },
	{ "agent_connect_timeout",	0, NULL },
	{ "ha_strategy",			0, NULL	},
	{ "agent_query_timeout",	0, NULL },
	{ "html_strip",				0, NULL },
	{ "html_index_attrs",		0, NULL },
	{ "html_remove_elements",	0, NULL },
	{ "preopen",				0, NULL },
	{ "inplace_enable",			0, NULL },
	{ "inplace_hit_gap",		0, NULL },
	{ "inplace_docinfo_gap",	KEY_REMOVED, NULL },
	{ "inplace_reloc_factor",	0, NULL },
	{ "inplace_write_factor",	0, NULL },
	{ "index_exact_words",		0, NULL },
	{ "min_stemming_len",		0, NULL },
	{ "overshort_step",			0, NULL },
	{ "stopword_step",			0, NULL },
	{ "blend_chars",			0, NULL },
	{ "expand_keywords",		0, NULL },
	{ "hitless_words",			0, NULL },
	{ "hit_format",				KEY_HIDDEN | KEY_DEPRECATED, "default value" },
	{ "rt_field",				KEY_LIST, NULL },
	{ "rt_attr_uint",			KEY_LIST, NULL },
	{ "rt_attr_bigint",			KEY_LIST, NULL },
	{ "rt_attr_float",			KEY_LIST, NULL },
	{ "rt_attr_timestamp",		KEY_LIST, NULL },
	{ "rt_attr_string",			KEY_LIST, NULL },
	{ "rt_attr_multi",			KEY_LIST, NULL },
	{ "rt_attr_multi_64",		KEY_LIST, NULL },
	{ "rt_attr_json",			KEY_LIST, NULL },
	{ "rt_attr_bool",			KEY_LIST, NULL },
	{ "rt_mem_limit",			0, NULL },
	{ "dict",					0, NULL },
	{ "index_sp",				0, NULL },
	{ "index_zones",			0, NULL },
	{ "blend_mode",				0, NULL },
	{ "regexp_filter",			KEY_LIST, NULL },
	{ "bigram_freq_words",		0, NULL },
	{ "bigram_index",			0, NULL },
	{ "index_field_lengths",	0, NULL },
	{ "divide_remote_ranges",	KEY_HIDDEN, NULL },
	{ "stopwords_unstemmed",	0, NULL },
	{ "global_idf",				0, NULL },
	{ "rlp_context",			KEY_REMOVED, NULL },
	{ "ondisk_attrs",			KEY_DEPRECATED, "access_plain_attrs = mmap" },
	{ "index_token_filter",		0, NULL },
	{ "morphology_skip_fields",	0, NULL },
	{ "killlist_target",		0, nullptr },
	{ "read_buffer_docs",		0, nullptr },
	{ "read_buffer_hits",		0, nullptr },
	{ "read_buffer_columnar",	0, nullptr },
	{ "read_unhinted",			0, nullptr },
	{ "attr_update_reserve",	0, nullptr },
	{ "access_plain_attrs",		0, nullptr },
	{ "access_blob_attrs",		0, nullptr },
	{ "access_doclists",		0, nullptr },
	{ "access_hitlists",		0, nullptr },
	{ "stored_fields",			0, nullptr },
	{ "stored_only_fields",		0, nullptr },
	{ "docstore_block_size",	0, nullptr },
	{ "docstore_compression",	0, nullptr },
	{ "docstore_compression_level",	0, nullptr },
	{ "columnar_attrs",			0, nullptr },
	{ "columnar_no_fast_fetch", 0, nullptr },
	{ "rowwise_attrs",			0, nullptr },
	{ "columnar_strings_no_hash", 0, nullptr },
	{ "columnar_compression_uint32", 0, nullptr },
	{ "columnar_compression_int64", 0, nullptr },
	{ "columnar_subblock",		0, nullptr },
	{ nullptr,					0, nullptr }
};

/// allowed keys for indexer section
static KeyDesc_t g_dKeysIndexer[] =
{
	{ "mem_limit",				0, NULL },
	{ "max_iops",				0, NULL },
	{ "max_iosize",				0, NULL },
	{ "max_xmlpipe2_field",		0, NULL },
	{ "max_file_field_buffer",	0, NULL },
	{ "write_buffer",			0, NULL },
	{ "on_file_field_error",	0, NULL },
	{ "on_json_attr_error",		KEY_DEPRECATED, "on_json_attr_error in common{..} section" },
	{ "json_autoconv_numbers",	KEY_DEPRECATED, "json_autoconv_numbers in common{..} section" },
	{ "json_autoconv_keynames",	KEY_DEPRECATED, "json_autoconv_keynames in common{..} section" },
	{ "lemmatizer_cache",		0, NULL },
	{ "ignore_non_plain",		0, NULL },
	{ NULL,						0, NULL }
};

/// allowed keys for searchd section
static KeyDesc_t g_dKeysSearchd[] =
{
	{ "address",				KEY_REMOVED, NULL },
	{ "port",					KEY_REMOVED, NULL },
	{ "listen",					KEY_LIST, NULL },
	{ "log",					0, NULL },
	{ "query_log",				0, NULL },
	{ "read_timeout",			KEY_DEPRECATED, "network_timeout" },
	{ "network_timeout",		0, NULL },
	{ "client_timeout",			0, NULL },
	{ "max_children",			KEY_REMOVED, NULL },
	{ "pid_file",				0, NULL },
	{ "max_matches",			KEY_REMOVED, NULL },
	{ "seamless_rotate",		0, NULL },
	{ "preopen_indexes",		0, NULL },
	{ "unlink_old",				0, NULL },
	{ "ondisk_dict_default",	KEY_REMOVED, NULL },
	{ "attr_flush_period",		0, NULL },
	{ "max_packet_size",		0, NULL },
	{ "mva_updates_pool",		KEY_REMOVED, NULL },
	{ "max_filters",			0, NULL },
	{ "max_filter_values",		0, NULL },
	{ "max_open_files",			0, NULL },
	{ "listen_backlog",			0, NULL },
	{ "listen_tfo",				0, NULL },
	{ "read_buffer",			KEY_DEPRECATED, "read_buffer_docs or read_buffer_hits" },
	{ "read_buffer_docs",		0, NULL },
	{ "read_buffer_hits",		0, NULL },
	{ "read_buffer_columnar",	0, NULL },
	{ "read_unhinted",			0, NULL },
	{ "max_batch_queries",		0, NULL },
	{ "subtree_docs_cache",		0, NULL },
	{ "subtree_hits_cache",		0, NULL },
	{ "workers",				KEY_DEPRECATED, "default value" },
	{ "prefork",				KEY_HIDDEN, NULL },
	{ "dist_threads",			KEY_DEPRECATED, "max_threads_per_query" },
	{ "max_threads_per_query",	0, NULL },
	{ "binlog_flush",			0, NULL },
	{ "binlog_path",			0, NULL },
	{ "binlog_max_log_size",	0, NULL },
	{ "thread_stack",			0, NULL },
	{ "expansion_limit",		0, NULL },
	{ "rt_flush_period",		0, NULL },
	{ "query_log_format",		0, NULL },
	{ "mysql_version_string",	0, NULL },
	{ "plugin_dir",				KEY_DEPRECATED, "plugin_dir in common{..} section" },
	{ "collation_server",		0, NULL },
	{ "collation_libc_locale",	0, NULL },
	{ "watchdog",				0, NULL },
	{ "prefork_rotation_throttle", KEY_REMOVED, NULL },
	{ "snippets_file_prefix",	0, NULL },
	{ "sphinxql_state",			0, NULL },
	{ "rt_merge_iops",			0, NULL },
	{ "rt_merge_maxiosize",		0, NULL },
	{ "ha_ping_interval",		0, NULL },
	{ "ha_period_karma",		0, NULL },
	{ "predicted_time_costs",	0, NULL },
	{ "persistent_connections_limit",	0, NULL },
	{ "ondisk_attrs_default",	KEY_REMOVED, NULL },
	{ "shutdown_timeout",		0, NULL },
	{ "query_log_min_msec",		0, NULL },
	{ "agent_connect_timeout",	0, NULL },
	{ "agent_query_timeout",	0, NULL },
	{ "agent_retry_delay",		0, NULL },
	{ "agent_retry_count",		0, NULL },
	{ "net_wait_tm",			0, NULL },
	{ "net_throttle_action",	0, NULL },
	{ "net_throttle_accept",	0, NULL },
	{ "net_send_job",			0, NULL },
	{ "net_workers",			0, NULL },
	{ "queue_max_length",		KEY_REMOVED, NULL },
	{ "qcache_ttl_sec",			0, NULL },
	{ "qcache_max_bytes",		0, NULL },
	{ "qcache_thresh_msec",		0, NULL },
	{ "sphinxql_timeout",		0, NULL },
	{ "hostname_lookup",		0, NULL },
	{ "grouping_in_utc",		0, NULL },
	{ "query_log_mode",			0, NULL },
	{ "prefer_rotate",			KEY_DEPRECATED, "seamless_rotate" },
	{ "shutdown_token",			0, NULL },
	{ "data_dir",				0, NULL },
	{ "node_address",			0, NULL },
	{ "server_id",				0, NULL },
	{ "access_plain_attrs",		0, nullptr },
	{ "access_blob_attrs",		0, nullptr },
	{ "access_doclists",		0, nullptr },
	{ "access_hitlists",		0, nullptr },
	{ "docstore_cache_size",	0, nullptr },
	{ "ssl_cert",				0, nullptr },
	{ "ssl_key",				0, nullptr },
	{ "ssl_ca",					0, nullptr },
	{ "max_connections",		0, nullptr },
	{ "threads",				0, nullptr },
	{ "jobs_queue_size",		0, nullptr },
	{ "not_terms_only_allowed",	0, nullptr },
	{ "query_log_commands",		0, nullptr },
	{ "auto_optimize",			0, nullptr },
	{ "pseudo_sharding",		0, nullptr },
	{ "optimize_cutoff",		0, nullptr },
	{ NULL,						0, NULL }
};

/// allowed keys for common section
static KeyDesc_t g_dKeysCommon[] =
{
	{ "lemmatizer_base",		0, NULL },
	{ "on_json_attr_error",		0, NULL },
	{ "json_autoconv_numbers",	0, NULL },
	{ "json_autoconv_keynames",	0, NULL },
	{ "rlp_root",				KEY_REMOVED, NULL },
	{ "rlp_environment",		KEY_REMOVED, NULL },
	{ "icu_data_dir",			KEY_REMOVED, NULL },
	{ "rlp_max_batch_size",		KEY_REMOVED, NULL },
	{ "rlp_max_batch_docs",		KEY_REMOVED, NULL },
	{ "plugin_dir",				0, NULL },
	{ "progressive_merge",		0, NULL },
	{ NULL,						0, NULL }
};

struct KeySection_t
{
	const char *		m_sKey;		///< key name
	KeyDesc_t *			m_pSection; ///< section to refer
	bool				m_bNamed; ///< true if section is named. false if plain
};

static KeySection_t g_dConfigSections[] =
{
	{ "source",		g_dKeysSource,	true },
	{ "index",		g_dKeysIndex,	true },
	{ "indexer",	g_dKeysIndexer,	false },
	{ "searchd",	g_dKeysSearchd,	false },
	{ "common",		g_dKeysCommon,	false },
	{ NULL,			NULL,			false }
};

//////////////////////////////////////////////////////////////////////////
bool CSphConfigParser::IsPlainSection ( const char * sKey )
{
	assert ( sKey );
	const KeySection_t * pSection = g_dConfigSections;
	while ( pSection->m_sKey && strcasecmp ( sKey, pSection->m_sKey ) )
		++pSection;
	return pSection->m_sKey && !pSection->m_bNamed;
}

bool CSphConfigParser::IsNamedSection ( const char * sKey )
{
	assert ( sKey );
	const KeySection_t * pSection = g_dConfigSections;
	while ( pSection->m_sKey && strcasecmp ( sKey, pSection->m_sKey ) )
		++pSection;
	return pSection->m_sKey && pSection->m_bNamed;
}

bool CSphConfigParser::AddSection ( const char * sType, const char * sName )
{
	m_sSectionType = sType;
	m_sSectionName = sName;

	if ( !m_tConf.Exists ( m_sSectionType ) )
		m_tConf.Add ( CSphConfigType(), m_sSectionType ); // FIXME! be paranoid, verify that it returned true

	if ( m_tConf[m_sSectionType].Exists ( m_sSectionName ) )
		return TlsMsg::Err ( "section '%s' (type='%s') already exists", sName, sType );
	m_tConf[m_sSectionType].Add ( CSphConfigSection(), m_sSectionName ); // FIXME! be paranoid, verify that it returned true

	return true;
}


void CSphConfigParser::AddKey ( const char * sKey, char * sValue )
{
	assert ( m_tConf.Exists ( m_sSectionType ) );
	assert ( m_tConf[m_sSectionType].Exists ( m_sSectionName ) );

	sValue = trim ( sValue );
	CSphConfigSection & tSec = m_tConf[m_sSectionType][m_sSectionName];
	tSec.AddEntry ( sKey, sValue );
}


bool CSphConfigParser::ValidateKey ( const char * sKey )
{
	// get proper descriptor table
	// OPTIMIZE! move lookup to AddSection
	const KeySection_t * pSection = g_dConfigSections;
	const KeyDesc_t * pDesc = NULL;
	while ( pSection->m_sKey && m_sSectionType!=pSection->m_sKey )
		++pSection;
	if ( pSection->m_sKey )
		pDesc = pSection->m_pSection;

	if ( !pDesc )
		return TlsMsg::Err( "unknown section type '%s'", m_sSectionType.cstr() );

	// check if the key is known
	while ( pDesc->m_sKey && strcasecmp ( pDesc->m_sKey, sKey ) )
		pDesc++;
	if ( !pDesc->m_sKey )
		return TlsMsg::Err( "unknown key name '%s'", sKey );

	// warn about deprecate keys
	if ( pDesc->m_iFlags & KEY_DEPRECATED )
		if ( ++m_iWarnings<=WARNS_THRESH )
			fprintf ( stdout, "WARNING: key '%s' is deprecated in %s line %d; use '%s' instead.\n",
				sKey, m_sFileName.cstr(), m_iLine, pDesc->m_sExtra );

	// warn about list/non-list keys
	if (!( pDesc->m_iFlags & KEY_LIST ))
	{
		CSphConfigSection & tSec = m_tConf[m_sSectionType][m_sSectionName];
		if ( tSec(sKey) && !tSec[sKey].m_bTag )
			if ( ++m_iWarnings<=WARNS_THRESH )
				fprintf ( stdout, "WARNING: key '%s' is not multi-value; value in %s line %d will be ignored.\n", sKey, m_sFileName.cstr(), m_iLine );
	}

	if ( pDesc->m_iFlags & KEY_REMOVED )
		if ( ++m_iWarnings<=WARNS_THRESH )
			fprintf ( stdout, "WARNING: key '%s' was permanently removed from configuration. Refer to documentation for details.\n", sKey );

	return true;
}

#if !_WIN32

bool TryToExec ( char * pBuffer, const char * szFilename, CSphVector<char> & dResult, const char * sArgs )
{
	using namespace TlsMsg;
	Err(); // clean any inherited msgs

	const int BUFFER_SIZE = 65536;

	int dPipe[2] = { -1, -1 };

	if ( pipe ( dPipe ) )
		return Err ( "pipe() failed (error=%s)", strerrorm(errno) );

	if ( !sArgs )
		pBuffer = trim ( pBuffer );
	else
		pBuffer = const_cast<char *>( sArgs );

	int iRead = dPipe[0];
	int iWrite = dPipe[1];

	int iChild = fork();

	if ( iChild==0 )
	{
		close ( iRead );
		close ( STDOUT_FILENO );
		dup2 ( iWrite, STDOUT_FILENO );
		searchd::CleanAfterFork ();

		CSphVector<CSphString> dTmpArgs;
		CSphVector<char *> dArgs;
		char * pArgs = nullptr;
		if ( !sArgs )
		{
			char * pPtr = pBuffer;
			while ( *pPtr )
			{
				if ( sphIsSpace ( *pPtr ) )
				{
					*pPtr = '\0';
					pArgs = trim ( pPtr+1 );
					break;
				}

				pPtr++;
			}
		} else
		{
			sphSplit ( dTmpArgs, sArgs, " " );
			dArgs.Resize ( dTmpArgs.GetLength() + 1 );
			ARRAY_FOREACH ( i, dTmpArgs )
				dArgs[i] = const_cast<char *>( dTmpArgs[i].cstr() );
			dArgs.Last() = nullptr;
		}

		if ( sArgs )
		{
			execvp ( dArgs[0], dArgs.Begin() );
		} else if ( pArgs )
			execl ( pBuffer, pBuffer, pArgs, szFilename, (char*)nullptr );
		else
			execl ( pBuffer, pBuffer, szFilename, (char*) nullptr );

		exit ( 1 );

	} else if ( iChild==-1 )
		return Err ( "fork failed: [%d] %s", errno, strerrorm(errno) );

	close ( iWrite );

	dResult.Reset();
	while (true)
	{
		dResult.ReserveGap ( BUFFER_SIZE );
		auto pBuf = (void *) dResult.End();
		auto iChunk = (int) read ( iRead, pBuf, BUFFER_SIZE );
		if ( iChunk>0 )
			dResult.AddN ( iChunk );

		if ( !iChunk ) // eof
			break;

		if ( iChunk==-1 && errno!=EINTR ) // we can get SIGCHLD just before eof, other is fail
		{
			Err ( "pipe read error: [%d] %s", errno, strerrorm (errno));
			break;
		}
	}
	close ( iRead );

	int iStatus, iResult;
	do
	{
		// can be interrupted by pretty much anything (e.g. SIGCHLD from other searchd children)
		iResult = waitpid ( iChild, &iStatus, 0 );

		// they say this can happen if child exited and SIGCHLD was ignored
		// a cleaner one would be to temporary handle it here, but can we be bothered
		if ( iResult==-1 && errno==ECHILD )
		{
			iResult = iChild;
			iStatus = 0;
		}

		if ( iResult==-1 && errno!=EINTR )
			return Err ( "waitpid() failed: [%d] %s", errno, strerrorm(errno) );
	}
	while ( iResult!=iChild );

	if ( WIFEXITED ( iStatus ) && WEXITSTATUS ( iStatus ) )
		// FIXME? read stderr and log that too
		return Err ( "error executing '%s' status = %d", pBuffer, WEXITSTATUS ( iStatus ) );

	if ( WIFSIGNALED ( iStatus ) )
		return Err ( "error executing '%s', killed by signal %d", pBuffer, WTERMSIG ( iStatus ) );

	if ( HasErr() )
		return false;

	dResult.Add('\0');
	return true;
}
#else
bool TryToExec ( char *, const char *, CSphVector<char> &, const char* )
{
	return true;
}
#endif


char * CSphConfigParser::GetBufferString ( char * szDest, int iMax, const char * & szSource )
{
	int nCopied = 0;

	while ( nCopied < iMax-1 && szSource[nCopied] && ( nCopied==0 || szSource[nCopied-1]!='\n' ) )
	{
		szDest [nCopied] = szSource [nCopied];
		nCopied++;
	}

	if ( !nCopied )
		return nullptr;

	szSource += nCopied;
	szDest [nCopied] = '\0';

	return szDest;
}

bool CSphConfigParser::ReParse ( const char * sFileName, const char * pBuffer )
{
	CSphConfig tOldConfig = m_tConf;
	m_tConf.Reset();
	if ( Parse ( sFileName, pBuffer ) )
		return true;
	m_tConf = tOldConfig;
	return false;
}

bool CSphConfigParser::Parse ( const char * sFileName, const char * pBuffer )
{
	using namespace TlsMsg;
	Err();

	const int L_STEPBACK	= 16;
	const int L_TOKEN		= 64;
	const int L_BUFFER		= 8192;

	FILE * fp = nullptr;
	if ( !pBuffer )
	{
		// open file
		fp = fopen ( sFileName, "rb" );
		if ( !fp )
			return false;
	}

	// init parser
	m_sFileName = sFileName;
	m_iLine = 0;
	m_iWarnings = 0;

	char sBuf [ L_BUFFER ] = { 0 };

	char * p = sBuf;
	char * pEnd = p;

	char sToken [ L_TOKEN ] = { 0 };
	int iToken = 0;
	int iCh = -1;

	enum { S_TOP, S_SKIP2NL, S_TOK, S_TYPE, S_SEC, S_CHR, S_VALUE, S_SECNAME, S_SECBASE, S_KEY } eState = S_TOP, eStack[8];
	int iStack = 0;

	int iValue = 0, iValueMax = 65535;
	auto * sValue = new char [ iValueMax+1 ];

	#define LOC_ERROR(...) { Err(__VA_ARGS__); break; }
	#define LOC_PUSH(_new) { assert ( iStack<int(sizeof(eStack)/sizeof(eState)) ); eStack[iStack++] = eState; eState = _new; }
	#define LOC_POP() { assert ( iStack>0 ); eState = eStack[--iStack]; }
	#define LOC_BACK() { p--; }

	for ( ; ; p++ )
	{
		// if this line is over, load next line
		if ( p>=pEnd )
		{
			char * szResult = pBuffer ? GetBufferString ( sBuf, L_BUFFER, pBuffer ) : fgets ( sBuf, L_BUFFER, fp );
			if ( !szResult )
				break; // FIXME! check for read error

			m_iLine++;
			size_t iLen = strlen(sBuf);
			if ( iLen<=0 ) LOC_ERROR ( "internal error; fgets() returned empty string" );

			p = sBuf;
			pEnd = sBuf + iLen;
			if ( pEnd[-1]!='\n' )
			{
				if ( iLen==L_BUFFER-1 )
					LOC_ERROR ( "line too long" );
			}
		}

		// handle S_TOP state
		if ( eState==S_TOP )
		{
			sBuf[L_BUFFER-1] = '\0'; // just safety
			if ( isspace(*p) )				continue;

			if ( *p=='#' )
			{
#if !_WIN32
				if ( !pBuffer && m_iLine==1 && p==sBuf && p[1]=='!' )
				{
					CSphVector<char> dResult;
					if ( TryToExec ( p+2, sFileName, dResult ) )
						Parse ( sFileName, &dResult[0] );
					break;
				} else
#endif
				{
					LOC_PUSH ( S_SKIP2NL );
					continue;
				}
			}

			if ( !sphIsAlpha(*p) )			LOC_ERROR ( "invalid token" );
											iToken = 0; LOC_PUSH ( S_TYPE ); LOC_PUSH ( S_TOK ); LOC_BACK(); continue;
		}

		// handle S_SKIP2NL state
		if ( eState==S_SKIP2NL )
		{
			LOC_POP ();
			p = pEnd;
			continue;
		}

		// handle S_TOK state
		if ( eState==S_TOK )
		{
			if ( !iToken && !sphIsAlpha(*p) )LOC_ERROR ( "internal error (non-alpha in S_TOK pos 0)" );
			if ( iToken==sizeof(sToken) )	LOC_ERROR ( "token too long" );
			if ( !sphIsAlpha(*p) )			{ LOC_POP (); sToken [ iToken ] = '\0'; iToken = 0; LOC_BACK(); continue; }
			if ( !iToken )					{ sToken[0] = '\0'; }
											sToken [ iToken++ ] = *p; continue;
		}

		// handle S_TYPE state
		if ( eState==S_TYPE )
		{
			if ( isspace(*p) )				continue;
			if ( *p=='#' )					{ LOC_PUSH ( S_SKIP2NL ); continue; }
			if ( !sToken[0] )				{ LOC_ERROR ( "internal error (empty token in S_TYPE)" ); }
			if ( IsPlainSection(sToken) )
			{
				if ( !AddSection ( sToken, sToken ) )
					break;
				sToken[0] = '\0';
				LOC_POP();
				LOC_PUSH ( S_SEC );
				LOC_PUSH ( S_CHR );
				iCh = '{';
				LOC_BACK();
				continue;
			}
			if ( IsNamedSection(sToken) )	{ m_sSectionType = sToken; sToken[0] = '\0'; LOC_POP (); LOC_PUSH ( S_SECNAME ); LOC_BACK(); continue; }
											LOC_ERROR ( "invalid section type '%s'", sToken );
		}

		// handle S_CHR state
		if ( eState==S_CHR )
		{
			if ( isspace(*p) )				continue;
			if ( *p=='#' )					{ LOC_PUSH ( S_SKIP2NL ); continue; }
			if ( *p!=iCh )					LOC_ERROR ( "expected '%c', got '%c'", iCh, *p );
											LOC_POP (); continue;
		}

		// handle S_SEC state
		if ( eState==S_SEC )
		{
			if ( isspace(*p) )				continue;
			if ( *p=='#' )					{ LOC_PUSH ( S_SKIP2NL ); continue; }
			if ( *p=='}' )					{ LOC_POP (); continue; }
			if ( sphIsAlpha(*p) )			{ LOC_PUSH ( S_KEY ); LOC_PUSH ( S_TOK ); LOC_BACK(); iValue = 0; sValue[0] = '\0'; continue; }
											LOC_ERROR ( "section contents: expected token, got '%c'", *p );
		}

		// handle S_KEY state
		if ( eState==S_KEY )
		{
			// validate the key
			if ( !ValidateKey ( sToken ) )
				break;

			// an assignment operator and a value must follow
			LOC_POP (); LOC_PUSH ( S_VALUE ); LOC_PUSH ( S_CHR ); iCh = '=';
			LOC_BACK(); // because we did not work the char at all
			continue;
		}

		// handle S_VALUE state
		if ( eState==S_VALUE )
		{
			if ( *p=='\n' )					{ AddKey ( sToken, sValue ); iValue = 0; LOC_POP (); continue; }
			if ( *p=='#' )					{ AddKey ( sToken, sValue ); iValue = 0; LOC_POP (); LOC_PUSH ( S_SKIP2NL ); continue; }
			if ( *p=='\\' )
			{
				// backslash at the line end: continuation operator; let the newline be unhanlded
				if ( p[1]=='\r' || p[1]=='\n' ) { LOC_PUSH ( S_SKIP2NL ); continue; }

				// backslash before number sign: comment start char escaping; advance and pass it
				if ( p[1]=='#' ) { p++; }

				// otherwise: just a char, pass it
			}
			if ( iValue<iValueMax )			{ sValue[iValue++] = *p; sValue[iValue] = '\0'; }
											continue;
		}

		// handle S_SECNAME state
		if ( eState==S_SECNAME )
		{
			if ( isspace(*p) )					{ continue; }
			if ( !sToken[0]&&!sphIsAlpha(*p))	{ LOC_ERROR ( "named section: expected name, got '%c'", *p ); }

			if ( !sToken[0] )				{ LOC_PUSH ( S_TOK ); LOC_BACK(); continue; }
			if ( !AddSection ( m_sSectionType.cstr(), sToken ) ) break;
			sToken[0] = '\0';
			if ( *p==':' )					{ eState = S_SECBASE; continue; }
			if ( *p=='{' )					{ eState = S_SEC; continue; }
											LOC_ERROR ( "named section: expected ':' or '{', got '%c'", *p );
		}

		// handle S_SECBASE state
		if ( eState==S_SECBASE )
		{
			if ( isspace(*p) )					{ continue; }
			if ( !sToken[0]&&!sphIsAlpha(*p))	{ LOC_ERROR ( "named section: expected parent name, got '%c'", *p ); }
			if ( !sToken[0] )					{ LOC_PUSH ( S_TOK ); LOC_BACK(); continue; }

			// copy the section
			assert ( m_tConf.Exists ( m_sSectionType ) );

			if ( !m_tConf [ m_sSectionType ].Exists ( sToken ) )
				LOC_ERROR ( "inherited section '%s': parent doesn't exist (parent name='%s', type='%s')",
					m_sSectionName.cstr(), sToken, m_sSectionType.cstr() );

			CSphConfigSection & tDest = m_tConf [ m_sSectionType ][ m_sSectionName ];
			tDest = m_tConf [ m_sSectionType ][ sToken ];

			// mark all values in the target section as "to be overridden"
			tDest.IterateStart ();
			while ( tDest.IterateNext() )
				tDest.IterateGet().m_bTag = true;

			LOC_BACK();
			eState = S_SEC;
			LOC_PUSH ( S_CHR );
			iCh = '{';
			continue;
		}

		LOC_ERROR ( "internal error (unhandled state %d)", eState );
	}

	#undef LOC_POP
	#undef LOC_PUSH
	#undef LOC_ERROR

	if ( !pBuffer )
		fclose ( fp );

	SafeDeleteArray ( sValue );

	if ( m_iWarnings>WARNS_THRESH )
		fprintf ( stdout, "WARNING: %d more warnings skipped.\n", m_iWarnings-WARNS_THRESH );

	if ( !HasErr() )
		return true;

	auto iCol = (int)(p-sBuf+1);

	int iCtx = Min ( L_STEPBACK, iCol ); // error context is upto L_STEPBACK chars back, but never going to prev line
	const char * sCtx = p-iCtx+1;
	if ( sCtx<sBuf )
		sCtx = sBuf;

	char sStepback [ L_STEPBACK+1 ];
	memcpy ( sStepback, sCtx, size_t ( iCtx ) );
	sStepback[iCtx] = '\0';

	return Err ( "ERROR: %s in %s line %d col %d.\n", szError (), m_sFileName.cstr (), m_iLine, iCol );
}

/////////////////////////////////////////////////////////////////////////////
const char * sphGetConfigFile ( const char * sHint )
{
	if ( sHint )
		return sHint;

	// fallback to defaults if there was no explicit config specified
#ifdef SYSCONFDIR
	static const char* sConfigFile = SYSCONFDIR "/manticore.conf";
	if ( sphIsReadable ( sConfigFile ) )
		return sConfigFile;
#endif

	static const char* sWorkingConfiFile = "./manticore.conf";
	if ( sphIsReadable ( sWorkingConfiFile ) )
		return sWorkingConfiFile;

	sphFatal ( "no readable config file (looked in "
#ifdef SYSCONFDIR
		SYSCONFDIR "/manticore.conf, "
#endif
		"./manticore.conf)." );
	return nullptr;
}

const char * sphLoadConfig ( const char * sOptConfig, bool bQuiet, bool bIgnoreIndexes, CSphConfigParser & cp )
{
	// fallback to defaults if there was no explicit config specified
	sOptConfig = sphGetConfigFile ( sOptConfig );

	if ( !bQuiet )
		fprintf ( stdout, "using config file '%s'...\n", sOptConfig );

	// load config
	if ( !cp.Parse ( sOptConfig ) )
		sphDie ( "failed to parse config file '%s': %s", sOptConfig, TlsMsg::szError() );

	CSphConfig & hConf = cp.m_tConf;
	if ( !bIgnoreIndexes && !hConf ( "index" ) )
		sphDie ( "no indexes found in config file '%s'", sOptConfig );

	return sOptConfig;
}

//////////////////////////////////////////////////////////////////////////
ESphLogLevel g_eLogLevel = SPH_LOG_INFO;        // current log level, can be changed on the fly
static void StdoutLogger ( ESphLogLevel eLevel, const char * sFmt, va_list ap )
{
	if ( eLevel>g_eLogLevel )
		return;

	switch ( eLevel )
	{
	case SPH_LOG_FATAL: fprintf ( stdout, "FATAL: " ); break;
	case SPH_LOG_WARNING: fprintf ( stdout, "WARNING: " ); break;
	case SPH_LOG_INFO: fprintf ( stdout, "WARNING: " ); break;
	case SPH_LOG_DEBUG:
	case SPH_LOG_VERBOSE_DEBUG:
	case SPH_LOG_VERY_VERBOSE_DEBUG: fprintf ( stdout, "DEBUG: " ); break;
	case SPH_LOG_RPL_DEBUG: fprintf ( stdout, "RPL: " ); break;
	}

	vfprintf ( stdout, sFmt, ap );
	fprintf ( stdout, "\n" );
}

static const int MAX_PREFIXES = 10;
static const char * g_dDisabledLevelLogs[SPH_LOG_MAX+1][MAX_PREFIXES] = {{0}};

void sphLogSupress ( const char * sNewPrefix, ESphLogLevel eLevel )
{
	for ( const char * &sPrefix : g_dDisabledLevelLogs[eLevel] )
		if ( !sPrefix )
		{
			sPrefix = sNewPrefix;
			return;
		} else if ( !strcmp ( sPrefix, sNewPrefix ) )
			return;

	// no space, just overwrite the last one
	g_dDisabledLevelLogs[eLevel][MAX_PREFIXES-1] = sNewPrefix;
}

void sphLogSupressRemove ( const char * sDelPrefix, ESphLogLevel eLevel )
{
	const char ** ppSource = g_dDisabledLevelLogs[eLevel];
	int i = 0;
	for ( const char *&sPrefix : g_dDisabledLevelLogs[eLevel] )
		if ( sPrefix && !strcmp (sDelPrefix, sPrefix) )
			ppSource[i++] = sPrefix;
	for (;i<MAX_PREFIXES;++i)
		g_dDisabledLevelLogs[eLevel][i] = nullptr;
}


volatile SphLogger_fn& g_pLogger()
{
	static SphLogger_fn pLogger = &StdoutLogger;
	return pLogger;
}

inline void Log ( ESphLogLevel eLevel, const char * sFmt, va_list ap )
{
	if ( !g_pLogger() ) return;
	for ( const char * sPrefix : g_dDisabledLevelLogs[eLevel] )
		if ( sPrefix && !strncmp ( sPrefix, sFmt, strlen ( sPrefix ) ) )
			return;
		else if ( !sPrefix )
			break;

	g_pLogger() ( eLevel, sFmt, ap );
}

void sphLogVa ( const char * sFmt, va_list ap, ESphLogLevel eLevel )
{
	Log ( eLevel, sFmt, ap );
}

void sphLogf ( ESphLogLevel eLevel, const char* sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	Log ( eLevel, sFmt, ap );
	va_end ( ap );
}

void sphWarning_impl ( const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	sphLogVa ( sFmt, ap );
	va_end ( ap );
}

void sphInfo_impl ( const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	sphLogVa ( sFmt, ap, SPH_LOG_INFO );
	va_end ( ap );
}

void sphLogFatal ( const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	sphLogVa ( sFmt, ap, SPH_LOG_FATAL );
	va_end ( ap );
}

void sphLogDebug_impl ( const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	sphLogVa ( sFmt, ap, SPH_LOG_DEBUG );
	va_end ( ap );
}

void sphLogDebugv_impl ( const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	sphLogVa ( sFmt, ap, SPH_LOG_VERBOSE_DEBUG );
	va_end ( ap );
}

void sphLogDebugvv_impl ( const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	sphLogVa ( sFmt, ap, SPH_LOG_VERY_VERBOSE_DEBUG );
	va_end ( ap );
}

void sphLogDebugRpl_impl ( const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	sphLogVa ( sFmt, ap, SPH_LOG_RPL_DEBUG );
	va_end ( ap );
}

namespace // use string builder with custom formatters
{
void CustomLogVa ( const char* sFmt, va_list ap, ESphLogLevel eLevel )
{
	StringBuilder_c sMyLine;
	sMyLine.vSprintf ( sFmt, ap );
	sphLogf ( eLevel, "%s", sMyLine.cstr() );
}
} // namespace

void CustomLog::Warning_impl ( const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	CustomLogVa ( sFmt, ap, SPH_LOG_WARNING );
	va_end ( ap );
}

void CustomLog::Info_impl ( const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	CustomLogVa ( sFmt, ap, SPH_LOG_INFO );
	va_end ( ap );
}

namespace TimePrefixed
{
	static int64_t g_uTimePrefix = 0;

	void TimeStart ()
	{
		g_uTimePrefix = sphMicroTimer ();
	}

	static void TimedLogVa ( const char* sPrefix, const char* sFmt, va_list ap, ESphLogLevel eLevel )
	{
		if ( eLevel>g_eLogLevel )
			return;

		StringBuilder_c sMyLine;
		sMyLine.Sprintf ( "%s[%t] %s", sPrefix, sphMicroTimer () - g_uTimePrefix, sFmt);
		CSphString sFormat;
		sMyLine.MoveTo (sFormat);
		sMyLine.vSprintf ( sFormat.cstr(), ap );
		sphLogf ( eLevel, "%s", sMyLine.cstr () );
	}

	void Warning ( const char* sPrefix, const char* sFmt, ... )
	{
		va_list ap;
		va_start ( ap, sFmt );
		TimedLogVa ( sPrefix, sFmt, ap, SPH_LOG_WARNING);
		va_end ( ap );
	}

	void Info ( const char* sPrefix, const char* sFmt, ... )
	{
		va_list ap;
		va_start ( ap, sFmt );
		TimedLogVa ( sPrefix, sFmt, ap, SPH_LOG_INFO );
		va_end ( ap );
	}

	void LogFatal ( const char* sPrefix, const char* sFmt, ... )
	{
		va_list ap;
		va_start ( ap, sFmt );
		TimedLogVa ( sPrefix, sFmt, ap, SPH_LOG_FATAL );
		va_end ( ap );
	}

	void LogDebug ( const char* sPrefix, const char* sFmt, ... )
	{
		va_list ap;
		va_start ( ap, sFmt );
		TimedLogVa ( sPrefix, sFmt, ap, SPH_LOG_DEBUG );
		va_end ( ap );
	}

	void LogDebugv ( const char* sPrefix, const char* sFmt, ... )
	{
		va_list ap;
		va_start ( ap, sFmt );
		TimedLogVa ( sPrefix, sFmt, ap, SPH_LOG_VERBOSE_DEBUG );
		va_end ( ap );
	}

	void LogDebugvv ( const char* sPrefix, const char* sFmt, ... )
	{
		va_list ap;
		va_start ( ap, sFmt );
		TimedLogVa ( sPrefix, sFmt, ap, SPH_LOG_VERY_VERBOSE_DEBUG );
		va_end ( ap );
	}
}

//////////////////////////////////////////////////////////////////////////
// CRASH REPORTING
//////////////////////////////////////////////////////////////////////////
inline static void Grow (char*, int) {};
inline static void Grow ( StringBuilder_c &tBuilder, int iInc ) { tBuilder.GrowEnough ( iInc ); }

inline static char* tail (char* p) { return p; }
inline static char* tail ( StringBuilder_c &tBuilder ) { return tBuilder.end(); }

template <typename Num, typename PCHAR>
static void NtoA_T ( PCHAR* ppOutput, Num uVal, int iBase=10, int iWidth=0, int iPrec=0, char cFill=' ' )
{
	assert ( ppOutput );
//	assert ( tail ( *ppOutput ) );
	assert ( iWidth>=0 );
	assert ( iPrec>=0 );
	assert ( iBase>0 && iBase<=16);

	const char cAllDigits[] = "fedcba9876543210123456789abcdef";
	// point to the '0'. This hack allows to process negative numbers,
	// since digit[x%10] for both x==2 and x==-2 is '2'.
	const char * cDigits = cAllDigits+sizeof(cAllDigits)/2-1;
	const char cZero = '0';
	auto &pOutput = *ppOutput;

	if ( !uVal )
	{
		if ( !iPrec && !iWidth )
		{
			*tail ( pOutput ) = cZero;
			++pOutput;
		} else
		{
			if ( !iPrec )
				++iPrec;
			else
				cFill = ' ';

			if ( iWidth )
			{
				if ( iWidth<iPrec )
					iWidth = iPrec;
				iWidth -= iPrec;
			}

			if ( iWidth>=0 )
			{
				Grow ( pOutput, iWidth );
				memset ( tail ( pOutput ), cFill, iWidth );
				pOutput += iWidth;
			}

			if ( iPrec>=0 )
			{
				Grow ( pOutput, iPrec );
				memset ( tail ( pOutput ), cZero, iPrec );
				pOutput += iPrec;
			}
		}
		return;
	}

	const BYTE uMaxIndex = 31; // 20 digits for MAX_INT64 in decimal; let it be 31 (32 digits max).
	char CBuf[uMaxIndex+1];
	char *pRes = &CBuf[uMaxIndex];


	BYTE uNegative = 0;
	if ( uVal<0 )
		++uNegative;

	while ( uVal )
	{
		*pRes-- = cDigits [ uVal % iBase ];
		uVal /= iBase;
	}

	auto uLen = (BYTE)( uMaxIndex - (pRes-CBuf) );
	if (!uLen)
		uNegative = 0;

	if ( iPrec && iWidth && cFill==cZero)
		cFill=' ';

	if ( iWidth )
		iWidth = iWidth - Max ( iPrec, uLen ) - uNegative;

	if ( uNegative && cFill==cZero )
	{
		*tail ( pOutput ) = '-';
		++pOutput;
		uNegative=0;
	}

	if ( iWidth>=0 )
	{
		Grow ( pOutput, iWidth );
		memset ( tail ( pOutput ), cFill, iWidth );
		pOutput += iWidth;
	}

	if ( uNegative )
	{
		*tail ( pOutput ) = '-';
		++pOutput;
	}

	if ( iPrec )
		iPrec -= uLen;

	if ( iPrec>=0 )
	{
		Grow ( pOutput, iPrec );
		memset ( tail ( pOutput ), cZero, iPrec );
		pOutput += iPrec;
	}

	Grow ( pOutput, uLen );
	memcpy ( tail ( pOutput ), pRes+1, uLen );
	pOutput+= uLen;
}

static const int nDividers = 10;
static const int Dividers[nDividers] = { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000 };

template < typename Num, typename PCHAR >
void IFtoA_T ( PCHAR * pOutput, Num nVal, int iPrec )
{
	assert ( iPrec<nDividers );
	auto &pBegin = *pOutput;
	if ( nVal<0 )
	{
		*tail ( pBegin ) = '-';
		++pBegin;
		nVal = -nVal;
	}
	auto iInt = nVal / Dividers[iPrec];
	auto iFrac = nVal % Dividers[iPrec];
	::NtoA_T ( &pBegin, iInt );
	*tail ( pBegin ) = '.';
	++pBegin;
	::NtoA_T ( &pBegin, iFrac, 10, 0, iPrec, '0' );
}

namespace tmtoa {
	using us_t = int64_t; // useconds

	static const us_t US = 1;
	static const us_t U5 = 5;
	static const us_t U5X = 50 * US;
	static const us_t U5C = 500 * US;
	static const us_t MS = 1000 * US;
	static const us_t MS5 = 5 * MS;
	static const us_t M5X = 50 * MS;
	static const us_t M5C = 500 * MS;
	static const us_t S = 1000 * MS;
	static const us_t S3 = 3 * S;
	static const us_t S3X = 30 * S;
	static const us_t M = 60 * S;
	static const us_t M3 = 3 * M;
	static const us_t M3X = 30 * M;
	static const us_t H = 60 * M;
	static const us_t H5 = 72 * M; // 1.2 hour
	static const us_t H5X = 12 * H;
	static const us_t D = 24 * H;
	static const us_t D5 = 84 * H; // 3.5 days
	static const us_t W = 7 * D;

	static const int NUM_SCALES = 7;
	static const char* sSuffixes[NUM_SCALES] = { "us", "ms", "s", "m", "h", "d", "w" };
	static const int iSufLens[NUM_SCALES] = { 2, 2, 1, 1, 1, 1, 1 }; // len of names
	static const int64_t TMScales[NUM_SCALES] = { US, MS, S, M, H, D, W };
	// how many rest digits causes new scale to add instead of frac
	static const int iPrecAfter[NUM_SCALES] = { 0, 3, 3, 2, 2, 2, 1 };

	static us_t calc_round ( int iScale, int iPrec )
	{
		// prec:	0	1	2	3	4	5	6	7	8	9	10	11	12	13
		// ---------------------------------------------------------------
		// usec 	0
		// msec 	U5C	U5X	U5	0
		// sec 		M5C	M5X MS5	U5C	U5X	U5	0
		// min 		S3X	S3	M5C	M5X MS5	U5C	U5X	U5	0
		// hour 	M3X	M3	S3X	S3	M5C	M5X	MS5	U5C	U5X	U5	0
		// day		H5X	H5	M3X	M3	S3X	S3	M5C	M5X	MS5	U5C	U5X	U5	0
		// week		D5	H5X	H5	M3X	M3	S3X	S3	M5C	M5X	MS5	U5C	U5X	U5	0
		static const int dStartPos[NUM_SCALES] = {13, 10, 7, 5, 3, 1, 0};
		static const us_t dRound[] = { D5, H5X, H5, M3X, M3, S3X, S3, M5C, M5X, MS5, U5C, U5X, U5, 0 };
		assert (iScale>=0 && iScale<NUM_SCALES);
		assert ( iPrec>=0 && iPrec<14);
		auto iIdx = dStartPos[iScale] + iPrec;
		if ( iIdx>12 )
			return 0;
		return dRound[iIdx];
	}
}

// format timespan expressed in useconds.
template < typename PCHAR >
void TMtoA_T ( PCHAR* pOutput, int64_t nVal, int iPrec )
{
	assert ( iPrec<nDividers );
	auto& pBegin = *pOutput;

	// correct sign, if necessary
	if ( nVal<0 )
	{
		*tail ( pBegin ) = '-';
		++pBegin;
		nVal = -nVal;
	}

	// find the range we deal with (from 6(week) to 0(usecs))
	int iSpan;
	for ( iSpan = 6; iSpan>0; --iSpan )
		if ( nVal>=tmtoa::TMScales[iSpan] )
			break;

	// round the value
	auto iRound = tmtoa::calc_round ( iSpan, iPrec );
	if ( iRound )
	{
		nVal += iRound;
		nVal -= nVal % ( iRound + iRound );
	}

	// after rounding range may change, recalc
	for ( iSpan = 6; iSpan>0; --iSpan )
		if ( nVal>=tmtoa::TMScales[iSpan] )
			break;

	while (true)
	{
		// multiplier and precision digits for range
		auto iMul = tmtoa::TMScales[iSpan];
		auto iPrecAfter = tmtoa::iPrecAfter[iSpan];

		// solid part
		::NtoA_T ( &pBegin, nVal / iMul );

		// rest
		nVal %= iMul;
		// check if rest of precision is enough to print extra values
		if ( iPrec>0 && iPrec<iPrecAfter )
		{
			if (iPrec==2) nVal /= (iMul/100);
			else if ( iPrec==1) nVal /= (iMul/10);
			if ( nVal ) // 0. Stop printing
			{
				*tail ( pBegin ) = '.';
				++pBegin;
				::NtoA_T ( &pBegin, nVal, 10, 0, iPrec, '0' );
			}
		}

		// print specifier
		Grow ( pBegin, tmtoa::iSufLens[iSpan] );
		memcpy ( tail ( pBegin ), tmtoa::sSuffixes[iSpan], tmtoa::iSufLens[iSpan] );
		pBegin += tmtoa::iSufLens[iSpan];

		// all is done
		if ( !iPrecAfter || iPrec<iPrecAfter )
			return;

		// the rest is 0. Stop
		if (nVal==0)
			return;

		// print space before continue.
		*tail ( pBegin ) = ' ';
		++pBegin;

		// go to next range
		iPrec -= iPrecAfter;
		--iSpan;
	}
}

// format timestamp expressed in useconds.
template < typename PCHAR >
void TMStoA_T ( PCHAR* pOutput, int64_t nVal, int iPrec )
{
	auto& pBegin = *pOutput;
	if ( !nVal )
	{
		Grow( pBegin, 5 );
		memcpy( tail( pBegin ), "never", 5 );
		pBegin += 5;
		return;
	}

	int64_t iTimespan = nVal - sphMicroTimer();
	if ( iTimespan<0 ) // past event
	{
		TMtoA_T ( pOutput, -iTimespan, iPrec );
		// print specifier
		Grow ( pBegin, 4 );
		memcpy ( tail ( pBegin ), " ago", 4 );
		pBegin += 4;
	} else if (iTimespan>0)
	{
		Grow ( pBegin, 3 );
		memcpy ( tail ( pBegin ), "in ", 3 );
		pBegin += 3;
		TMtoA_T ( pOutput, iTimespan, iPrec );
	} else
	{
		Grow ( pBegin, 3 );
		memcpy ( tail ( pBegin ), "now", 3 );
		pBegin += 3;
	}
}

template <typename Num>
inline static void NtoA ( char** ppOutput, Num uVal, int iBase = 10, int iWidth = 0, int iPrec = 0, char cFill = ' ' )
{
	NtoA_T ( ppOutput, uVal, iBase, iWidth, iPrec, cFill);
}

namespace sph {

#define DECLARE_NUMTOA(NTOA,TYPE) \
    template <> \
	int NTOA ( char * pOutput, TYPE uVal, int iBase, int iWidth, int iPrec, char cFill ) \
	{ \
		auto pBegin = pOutput; \
		::NtoA ( &pBegin, uVal, iBase, iWidth, iPrec, cFill ); \
		return int ( pBegin - pOutput ); \
	}

	// unsigned
	DECLARE_NUMTOA ( UItoA, unsigned int );
	DECLARE_NUMTOA ( UItoA, unsigned long );
	DECLARE_NUMTOA ( UItoA, unsigned long long);

	// signed
	DECLARE_NUMTOA ( ItoA, int );
	DECLARE_NUMTOA ( ItoA, long );
	DECLARE_NUMTOA ( ItoA, long long );

	// universal
	DECLARE_NUMTOA ( NtoA, unsigned int );
	DECLARE_NUMTOA ( NtoA, unsigned long );
	DECLARE_NUMTOA ( NtoA, unsigned long long );
	DECLARE_NUMTOA ( NtoA, int );
	DECLARE_NUMTOA ( NtoA, long );
	DECLARE_NUMTOA ( NtoA, long long );

#undef DECLARE_NUMTOA

	int IFtoA ( char * pOutput, int nVal, int iPrec )
	{
		auto pBegin = pOutput;
		IFtoA_T ( &pBegin, nVal, iPrec );
		return int ( pBegin - pOutput );
	}

	int IFtoA ( char * pOutput, int64_t nVal, int iPrec )
	{
		auto pBegin = pOutput;
		IFtoA_T ( &pBegin, nVal, iPrec );
		return int ( pBegin - pOutput );
	}

static int SkipFmt64 ( const char * sFmt )
{
	// %lld %lli %llu
	if ( sFmt[0]!='\0' && sFmt[1]!='\0' && sFmt[0]=='l' && ( sFmt[1]=='d' || sFmt[1]=='i' || sFmt[1]=='u' ) )
		return 2;

	// %li %ld %u
	if ( sFmt[0]!='\0' && ( sFmt[0]=='d' || sFmt[0]=='i' || sFmt[0]=='u' ) )
		return 1;

	return 0;
}

template <typename PCHAR>
void vSprintf_T ( PCHAR * _pOutput, const char * sFmt, va_list ap )
{
	enum eStates { SNORMAL, SPERCENT, SHAVEFILL, SINWIDTH, SINPREC };
	eStates state = SNORMAL;
	size_t iPrec = 0;
	size_t iWidth = 0;
	char cFill = ' ';
	bool bHeadingSpace = true;
	auto &pOutput = *_pOutput;

	char c;
	while ( ( c = *sFmt++ )!=0 )
	{
		// handle regular chars
		if ( state==SNORMAL )
		{
			auto sPercent = strchr (sFmt-1, '%');
			if ( !sPercent ) // no formatters, only plain chars
			{
				auto uLen = strlen (sFmt-1);
				Grow ( pOutput, (int) uLen );
				memcpy ( tail ( pOutput ), sFmt-1, (int) uLen );
				pOutput += (int) uLen;
				sFmt+=uLen-1;
				continue;
			}

			auto uLen = sPercent - sFmt + 1;
			if ( uLen )
			{
				Grow ( pOutput, (int)uLen );
				memcpy ( tail ( pOutput ), sFmt - 1, (int)uLen );
				pOutput += uLen;
				sFmt+=uLen;
			}

			// handle percent
			state = SPERCENT;
			iPrec = 0;
			iWidth = 0;
			cFill = ' ';
			continue;
		}

		// handle percent
		if ( c=='%' && state!=SNORMAL )
		{
			state = SNORMAL;
			*tail ( pOutput ) = c;
			++pOutput;
			continue;
		}

		// handle modifiers
		switch ( c )
		{
		case '0':
			if ( state==SPERCENT )
			{
				cFill = '0';
				state = SHAVEFILL;
				break;
			}
		// [[clang::fallthrough]];
		case '1': case '2': case '3':
		case '4': case '5': case '6':
		case '7': case '8': case '9':
			if ( state==SPERCENT || state==SHAVEFILL )
			{
				state = SINWIDTH;
				iWidth = c - '0';
			} else if ( state==SINWIDTH )
				iWidth = iWidth * 10 + c - '0';
			else if ( state==SINPREC )
				iPrec = iPrec * 10 + c - '0';
			break;

		case '-':
			if ( state==SPERCENT )
				bHeadingSpace = false;
			else
				state = SNORMAL; // FIXME? means that bad/unhandled syntax with dash will be just ignored
			break;

		case '.':
			state = SINPREC;
			iPrec = 0;
			break;

		case 's': // string
			{
				const char * pValue = va_arg ( ap, const char * );
				if ( !pValue )
					pValue = "(null)";
				size_t iValue = strlen ( pValue );

				if ( iPrec && iPrec<iValue )
					iValue = iPrec;

				if ( iWidth && iValue>iWidth )
					iWidth = 0;

				if ( iWidth )
					iWidth-=iValue;

				Grow ( pOutput, (int) iWidth );
				if ( iWidth && bHeadingSpace )
				{
					memset ( tail ( pOutput ), ' ', (int) iWidth );
					pOutput += (int) iWidth;
				}

				Grow ( pOutput, (int) iValue );
				memcpy ( tail ( pOutput ), pValue, iValue );
				pOutput += (int) iValue;

				if ( iWidth && !bHeadingSpace )
				{
					memset ( tail ( pOutput ), ' ', (int) iWidth );
					pOutput += (int) iWidth;
				}

				state = SNORMAL;
				break;
			}

		case 'p': // pointer
			{
				void * pValue = va_arg ( ap, void * );
				auto uValue = uint64_t ( pValue );
				::NtoA_T ( &pOutput, uValue, 16, (int) iWidth, (int) iPrec, cFill );
				state = SNORMAL;
				break;
			}

		case 'x': // hex unsigned integer
		case 'u': // decimal unsigned
			{
				DWORD uValue = va_arg ( ap, DWORD );
				::NtoA_T ( &pOutput, uValue, ( c=='x' ) ? 16 : 10, (int) iWidth,(int)  iPrec, cFill );
				state = SNORMAL;
				break;
			}
		case 'd': // decimal integer
			{
				int iValue = va_arg ( ap, int );
				::NtoA_T ( &pOutput, iValue, 10, (int) iWidth, (int) iPrec, cFill );
				state = SNORMAL;
				break;
			}

		case 'i': // ignore (skip) current integer. Output nothing.
			{
				int VARIABLE_IS_NOT_USED iValue = va_arg ( ap, int );
				state = SNORMAL;
				break;
			}

		case 'l': // decimal int64
			{
				int64_t iValue = va_arg ( ap, int64_t );
				::NtoA_T ( &pOutput, iValue, 10, (int) iWidth, (int) iPrec, cFill );
				state = SNORMAL;
				sFmt += SkipFmt64 ( sFmt );
				break;
			}

		case 'U': // decimal uint64
			{
				uint64_t iValue = va_arg ( ap, uint64_t );
				::NtoA_T ( &pOutput, iValue, 10, (int) iWidth, (int) iPrec, cFill );
				state = SNORMAL;
				break;
			}

		case 'D': // fixed-point signed 64-bit
			{
				int64_t iValue = va_arg ( ap, int64_t );
				::IFtoA_T ( &pOutput, iValue, (int) iPrec );
				state = SNORMAL;
				break;
			}

		case 'F': // fixed-point signed 32-bit
			{
				int iValue = va_arg ( ap, int );
				::IFtoA_T ( &pOutput, iValue, (int) iPrec );
				state = SNORMAL;
				break;
			}

		case 't': // timespan given in int64 useconds
			{
				int64_t iValue = va_arg ( ap, int64_t );
				::TMtoA_T ( &pOutput, iValue, (int) iPrec );
				state = SNORMAL;
				break;
			}

		case 'T': // timestamp (related to now()) given in int64 useconds
			{
				int64_t iValue = va_arg ( ap, int64_t );
				::TMStoA_T ( &pOutput, iValue, (int) iPrec );
				state = SNORMAL;
				break;
			}

		case 'f': // float (fall-back to standard)
			{
				double fValue = va_arg ( ap, double );

				// ensure 32 is enough to take any float value.
				Grow ( pOutput, Max ( (int) iWidth, 32 ));

				// extract current format from source format line
				auto *pF = sFmt;
				while ( *--pF!='%' );

				if ( memcmp ( pF, "%f", 2 )!=0 )
				{

					// invoke standard sprintf
					char sFormat[32] = { 0 };
					memcpy ( sFormat, pF, sFmt - pF );
					pOutput += sprintf ( tail ( pOutput ), sFormat, fValue );
				} else
				{
					// plain %f - output arbitrary 6 or 8 digits
					pOutput += PrintVarFloat ( tail ( pOutput ), (float)fValue );
					assert (( sFmt - pF )==2 );
				}

				state = SNORMAL;
				break;
			}

		default:
			state = SNORMAL;
			*tail ( pOutput ) = c;
			++pOutput;
		}
	}

	// final zero
	*tail ( pOutput ) = c;
}

	int vSprintf ( char * pOutput, const char * sFmt, va_list ap )
	{
		auto pBegin = pOutput;
		sph::vSprintf_T ( &pOutput, sFmt, ap );
		return int ( pOutput - pBegin );
	}

	int Sprintf ( char * pOutput, const char * sFmt, ... )
	{
		auto pBegin = pOutput;
		va_list ap;
		va_start ( ap, sFmt );
		sph::vSprintf_T ( &pOutput, sFmt, ap );
		va_end ( ap );
		return int ( pOutput - pBegin );
	}

	void vSprintf ( StringBuilder_c &dOutput, const char * sFmt, va_list ap )
	{
		sph::vSprintf_T ( &dOutput, sFmt, ap );
	}

	void Sprintf ( StringBuilder_c& dOutput, const char * sFmt, ... )
	{
		va_list ap;
		va_start ( ap, sFmt );
		vSprintf ( dOutput, sFmt, ap );
		va_end ( ap );
	}

	int PrintVarFloat ( char* sBuffer, float fVal )
	{
		int iLen = sprintf ( sBuffer, "%f", fVal );
		auto fTest = strtof ( sBuffer, nullptr );
		if ( fTest!=fVal )
			return sprintf ( sBuffer, "%1.8f", fVal );
		return iLen;
	}

	SmallStringHash_T<CSphString> ParseKeyValueStrings ( const char * sBuf )
	{
		SmallStringHash_T<CSphString> hRes;
		sph::ParseKeyValues ( sBuf, [&] ( CSphString && sIdent, CSphString && sValue )
		{
			hRes.Add ( sValue, sIdent );
		});
		return hRes;
	}

	SmallStringHash_T<CSphVariant> ParseKeyValueVars ( const char * sBuf )
	{
		SmallStringHash_T<CSphVariant> hRes;
		sph::ParseKeyValues ( sBuf, [&] ( CSphString && sIdent, CSphString && sValue )
		{
			hRes.Add ( CSphVariant (sValue.cstr()), sIdent );
		});
		return hRes;
	}
} // namespace sph

static int sphVSprintf ( char * pOutput, const char * sFmt, va_list ap )
{
	auto iRes = sph::vSprintf (pOutput,sFmt,ap);
	pOutput[iRes++]='\n'; // final zero to EOL
	return iRes;
}

static char g_sSafeInfoBuf [ 1024 ];

void sphSafeInfo ( int iFD, const char * sFmt, ... )
{
	if ( iFD<0 || !sFmt )
		return;

	va_list ap;
	va_start ( ap, sFmt );
	int iLen = sphVSprintf ( g_sSafeInfoBuf, sFmt, ap ); // FIXME! make this vsnprintf
	va_end ( ap );
	sphWrite ( iFD, g_sSafeInfoBuf, size_t (iLen) );
}


static int sphSafeInfo ( char * pBuf, const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	int iLen = sphVSprintf ( pBuf, sFmt, ap ); // FIXME! make this vsnprintf
	va_end ( ap );
	return iLen;
}


volatile int& getParentPID ()
{
	static int iParentPID = 0;
	return iParentPID;
}

volatile bool& getSafeGDB ()
{
	static bool bHaveJemalloc = true;
	return bHaveJemalloc;
}

#if !_WIN32

#define SPH_BACKTRACE_ADDR_COUNT 128
#define SPH_BT_BINARY_NAME 2
#define SPH_BT_ADDRS 3
static void * g_pBacktraceAddresses [SPH_BACKTRACE_ADDR_COUNT];
static char g_pBacktrace[4096];
static const char g_sSourceTail[] = "> source.txt\n";
static const char * g_pArgv[128] = { "addr2line", "-e", "./searchd", "0x0", NULL };
static CSphString g_sBinaryName;
static auto& g_bSafeGDB = getSafeGDB ();

#if HAVE_SYS_PRCTL_H
static char g_sNameBuf[512] = {0};
static char g_sPid[30] = { 0 };
#endif

// if we already run under debugger (i.e. from a kind of IDE) - avoid any scripted crash holding.
bool IsDebuggerPresent()
{
	const int status_fd = ::open ( "/proc/self/status", O_RDONLY );
	if ( status_fd==-1 )
		return false;

	const ssize_t num_read = ::read ( status_fd, g_pBacktrace, sizeof ( g_pBacktrace ) - 1 );
	if ( num_read<=0 )
		return false;

	g_pBacktrace[num_read] = '\0';
	constexpr char tracerPidString[] = "TracerPid:";
	const auto tracer_pid_ptr = ::strstr ( g_pBacktrace, tracerPidString );
	if ( !tracer_pid_ptr )
		return false;


	for ( const char * characterPtr = tracer_pid_ptr + sizeof ( tracerPidString ) - 1;
		  characterPtr<=g_pBacktrace + num_read; ++characterPtr )
	{
		if ( ::isspace ( *characterPtr ) )
			continue;
		else
			return ::isdigit ( *characterPtr )!=0 && *characterPtr!='0';
	}

	return false;
}

static bool DumpGdb ( int iFD )
{
	auto & iParentPID = getParentPID ();
	if ( iParentPID>0 ) // most safe - ask watchdog to do everything
	{
		sphSafeInfo ( iFD, "Dump with GDB via watchdog");
		kill ( iParentPID, SIGUSR1 );
		sphSleepMsec (3*1000);
		return true;
	}
#if HAVE_SYS_PRCTL_H
	int iPos = sphSafeInfo ( g_sPid, "%d", getpid () );
	g_sPid[iPos-1] = '\0'; // make null-terminated from EOL string
	g_sNameBuf [ ::readlink ( "/proc/self/exe", g_sNameBuf, 511 ) ] = '\0';

	if ( g_bSafeGDB || iParentPID==-1 ) // jemalloc looks safe, or user explicitly asked to invoke gdb anyway
		return sphDumpGdb ( iFD, g_sNameBuf, g_sPid );

#endif
	sphSafeInfo ( iFD, "Dump with GDB is not available" );
	return false;
}

bool sphDumpGdb (int iFD, const char* sName, const char* sPid )
{
#if HAVE_SYS_PRCTL_H
	if ( IsDebuggerPresent ())
		return false;

#ifdef PR_SET_PTRACER
	// allow to trace us
	prctl ( PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0 );
#endif

	sigset_t signal_set;
	sigemptyset ( &signal_set );

	sigaddset ( &signal_set, SIGTERM );
	sigaddset ( &signal_set, SIGINT );
	sigaddset ( &signal_set, SIGSEGV );
	sigaddset ( &signal_set, SIGABRT );
	sigaddset ( &signal_set, SIGILL );
	sigaddset ( &signal_set, SIGUSR1 );
	sigaddset ( &signal_set, SIGHUP );

	// Block signals to child processes
	sigprocmask ( SIG_BLOCK, &signal_set, NULL );

	// Spawn helper process which will keep running when gdb is attached to us
	pid_t iPidIntermediate = fork ();
	if ( iPidIntermediate==-1 )
		return false;

	if ( !iPidIntermediate ) // the helper, run gdb
	{

		// Wathchdog 1: Used to kill sub processes to gdb which may hang
		pid_t iWait30 = fork ();
		if ( iWait30==-1 )
			_Exit ( 1 );
		if ( !iWait30 )
		{
			sphSleepMsec ( 30000 );
			_Exit ( 1 );
		}

		// Wathchdog 2: Give up on gdb, if it still does not finish even after killing its sub processes
		pid_t iWait60 = fork ();
		if ( iWait60==-1 )
		{
			kill ( iWait30, SIGKILL );
			_Exit ( 1 );
		}
		if ( !iWait60 )
		{
			sphSleepMsec ( 60000 );
			_Exit ( 1 );
		}

		// Worker: Spawns gdb
		pid_t iWorker = fork ();

		if ( !iWorker )
		{
			sphSafeInfo ( iFD, "Will run gdb on '%s', pid '%s'", sName, sPid );
			if ( dup2 ( iFD, STDOUT_FILENO )==-1 )
				_Exit ( 1 );
			if ( dup2 ( iFD, STDERR_FILENO )==-1 )
				_Exit ( 1 );
			execlp ( "gdb", "gdb", "-batch", "-n",
				"-ex", "info threads",
				"-ex", "thread apply all bt",
				"-ex", "echo \nMain thread:\n",
				"-ex", "bt",
				"-ex", "echo \nLocal variables:\n",
				"-ex", "info locals",
				"-ex", "detach",
				"-se", sName, "-p", sPid, nullptr );

			// If gdb failed to start, signal back
			_Exit ( 1 );
		}

		int iResult = 1;

		while ( iWorker || iWait30 || iWait60 )
		{
			int iStatus;
			pid_t iExited = wait ( &iStatus );
			if ( iExited == iWorker )
			{
				if ( WIFEXITED( iStatus ) && WEXITSTATUS( iStatus )==0 )
					iResult = 0; // Success
				else
					iResult = 2; // Failed to start gdb
				iWorker = 0;
				if ( iWait60 )
					kill ( iWait60, SIGKILL );
				if ( iWait30 )
					kill ( iWait30, SIGKILL );
			} else if ( iExited == iWait30 )
			{
				iWait30 = 0;
				if ( iWorker )
				{
					sphSafeInfo ( g_sNameBuf, "pkill -KILL -P %d", iWorker );
					auto iFoo = ::system ( g_sNameBuf );
					(void) iFoo; // that is unused
				}
			} else if ( iExited == iWait60 )
			{
				iWait60 = 0;
				if ( iWorker )
					kill ( iWorker, SIGKILL );
				if ( iWait30 )
					kill ( iWait30, SIGKILL );
			}
		}
		_Exit ( iResult );
	}

	// main process
	assert ( iPidIntermediate>0 );

	int iStatus;
	sigprocmask ( SIG_UNBLOCK, &signal_set, nullptr );
	pid_t iRes = waitpid ( iPidIntermediate, &iStatus, 0 );
	if ( iRes==-1 || iRes==0 )
		return false;

	// master branch is mirrored on github, so could generate more info here.
	if ( strncmp ( szGIT_BRANCH_ID, "git branch master", 17 ) == 0 ) {
		sphSafeInfo ( iFD, "You can obtain the sources of this version from https://github.com/manticoresoftware/manticoresearch/archive/%s.zip\n"
			"and set up debug env with this shippet (select wget or curl version below):\n\n"
   "  wget https://codeload.github.com/manticoresoftware/manticoresearch/zip/%s -O manticore.zip\n"
   "  curl https://codeload.github.com/manticoresoftware/manticoresearch/zip/%s -o manticore.zip",
   	szGIT_COMMIT_ID, szGIT_COMMIT_ID, szGIT_COMMIT_ID );
	sphSafeInfo ( iFD,
	  "\nUnpack the sources by command:\n"
	  "  mkdir -p /tmp/manticore && unzip manticore.zip -d /tmp/manticore\n\n"
	  "For comfortable debug also suggest to append a substitution def to your ~/.gdbinit file:\n"
	  "  set substitute-path \"%s\" /tmp/manticore/manticoresearch-%s", szGDB_SOURCE_DIR, szGIT_COMMIT_ID );
	}
	return true;
#else
	return false;
#endif
}

#if HAVE_BACKTRACE & HAVE_BACKTRACE_SYMBOLS
const char * DoBacktrace ( int iDepth, int iSkip )
{
	if ( !iDepth || iDepth > SPH_BACKTRACE_ADDR_COUNT )
		iDepth = SPH_BACKTRACE_ADDR_COUNT;
	iDepth = backtrace ( g_pBacktraceAddresses, iDepth );
	char ** ppStrings = backtrace_symbols ( g_pBacktraceAddresses, iDepth );
	if ( !ppStrings )
		return NULL;
	char * pDst = g_pBacktrace;
	for ( int i=iSkip; i<iDepth; ++i )
	{
		const char * pStr = ppStrings[i];
		do
			*pDst++ = *pStr++;
		while (*pStr);
		*pDst++='\n';
	}
	*pDst = '\0';
	free ( ppStrings );
	return g_pBacktrace; ///< sorry, no backtraces on Windows...
}
#else
const char * DoBacktrace ( int, int )
{
	return nullptr; ///< sorry, no backtraces...
}
#endif

#define BOOST_STACKTRACE_GNU_SOURCE_NOT_REQUIRED 1
#include <boost/stacktrace.hpp>

void sphBacktrace ( int iFD, bool bSafe )
{
	if ( iFD<0 )
		return;

	sphSafeInfo ( iFD, "-------------- backtrace begins here ---------------" );
#ifdef COMPILER
	sphSafeInfo ( iFD, "Program compiled with " COMPILER );
#endif

#ifdef CONFIGURE_FLAGS
	sphSafeInfo ( iFD, "Configured with flags: " CONFIGURE_FLAGS );
#endif

#ifdef OS_UNAME
	sphSafeInfo ( iFD, "Host OS is " OS_UNAME );
#endif

	bool bOk = true;

	const void * pMyStack = nullptr;
	int iStackSize = 0;
	if ( !bSafe )
	{
		pMyStack = sphMyStack();
		iStackSize = sphMyStackSize();
	}
	sphSafeInfo ( iFD, "Stack bottom = 0x%p, thread stack size = 0x%x", pMyStack, iStackSize );

	while ( pMyStack && !bSafe )
	{
		sphSafeInfo ( iFD, "Trying manual backtrace:" );
		BYTE ** pFramePointer = NULL;

		int iFrameCount = 0;
		int iReturnFrameCount = sphIsLtLib() ? 2 : 1;

#ifdef __i386__
#define SIGRETURN_FRAME_OFFSET 17
		__asm __volatile__ ( "movl %%ebp,%0":"=r"(pFramePointer):"r"(pFramePointer) );
#endif

#ifdef __x86_64__
#define SIGRETURN_FRAME_OFFSET 23
		__asm __volatile__ ( "movq %%rbp,%0":"=r"(pFramePointer):"r"(pFramePointer) );
#endif

#ifndef SIGRETURN_FRAME_OFFSET
#define SIGRETURN_FRAME_OFFSET 0
#endif

		if ( !pFramePointer )
		{
			sphSafeInfo ( iFD, "Frame pointer is null, manual backtrace failed (did you build with -fomit-frame-pointer?)" );
			break;
		}

		if ( !pMyStack || (BYTE*) pMyStack > (BYTE*) &pFramePointer )
		{
			int iRound = Min ( 65536, iStackSize );
			pMyStack = (void *) ( ( (size_t) &pFramePointer + iRound ) & ~(size_t)65535 );
			sphSafeInfo ( iFD, "Something wrong with thread stack, manual backtrace may be incorrect (fp=0x%p)", pFramePointer );

			if ( pFramePointer > (BYTE**) pMyStack || pFramePointer < (BYTE**) pMyStack - iStackSize )
			{
				sphSafeInfo ( iFD, "Wrong stack limit or frame pointer, manual backtrace failed (fp=0x%p, stack=0x%p, stacksize=0x%x)",
					pFramePointer, pMyStack, iStackSize );
				break;
			}
		}

		sphSafeInfo ( iFD, "Stack looks OK, attempting backtrace." );

		BYTE** pNewFP = NULL;
		while ( pFramePointer < (BYTE**) pMyStack )
		{
			pNewFP = (BYTE**) *pFramePointer;
			sphSafeInfo ( iFD, "0x%p", iFrameCount==iReturnFrameCount ? *(pFramePointer + SIGRETURN_FRAME_OFFSET) : *(pFramePointer + 1) );

			bOk = pNewFP > pFramePointer;
			if ( !bOk ) break;

			pFramePointer = pNewFP;
			iFrameCount++;
		}

		if ( !bOk )
			sphSafeInfo ( iFD, "Something wrong in frame pointers, manual backtrace failed (fp=%p)", pNewFP );

		break;
	}

	int iDepth = 0;
#if HAVE_BACKTRACE
	sphSafeInfo ( iFD, "Trying system backtrace:" );
	iDepth = backtrace ( g_pBacktraceAddresses, SPH_BACKTRACE_ADDR_COUNT );
	if ( iDepth>0 )
		bOk = true;
#if HAVE_BACKTRACE_SYMBOLS
	sphSafeInfo ( iFD, "begin of system symbols:" );
	backtrace_symbols_fd ( g_pBacktraceAddresses, iDepth, iFD );
#elif !HAVE_BACKTRACE_SYMBOLS
	sphSafeInfo ( iFD, "begin of manual symbols:" );
	for ( int i=0; i<iDepth; i++ )
		sphSafeInfo ( iFD, "%p", g_pBacktraceAddresses[i] );
#endif // HAVE_BACKTRACE_SYMBOLS
#endif // !HAVE_BACKTRACE

	sphSafeInfo ( iFD, "Trying boost backtrace:" );
	sphSafeInfo ( iFD, to_string ( boost::stacktrace::stacktrace() ).c_str() );

	sphSafeInfo ( iFD, "-------------- backtrace ends here ---------------" );

	if ( bOk )
		sphSafeInfo ( iFD, "Please, create a bug report in our bug tracker (https://github.com/manticoresoftware/manticore/issues)\n"
			"and attach there:\n"
			"a) searchd log, b) searchd binary, c) searchd symbols.\n"
			"Look into the chapter 'Reporting bugs' in the manual\n"
			"(https://manual.manticoresearch.com/Reporting_bugs)" );

	if ( DumpGdb ( iFD ) )
		return;

	// convert all BT addresses to source code lines
	int iCount = Min ( iDepth, (int)( sizeof(g_pArgv)/sizeof(g_pArgv[0]) - SPH_BT_ADDRS - 1 ) );
	sphSafeInfo ( iFD, "--- BT to source lines (depth %d): ---", iCount );
	char * pCur = g_pBacktrace;
	for ( int i=0; i<iCount; i++ )
	{
		// early out on strings buffer overrun
		if ( pCur>=g_pBacktrace+sizeof(g_pBacktrace)-48 )
		{
			iCount = i;
			break;
		}
		g_pArgv[i+SPH_BT_ADDRS] = pCur;
		pCur += sphSafeInfo ( pCur, "0x%x", g_pBacktraceAddresses[i] );
		*(pCur-1) = '\0'; // make null terminated string from EOL string
	}
	g_pArgv[iCount+SPH_BT_ADDRS] = NULL;

	int iChild = fork();

	if ( iChild==0 )
	{
		// map stdout to log file
		if ( iFD!=1 )
		{
			close ( 1 );
			dup2 ( iFD, 1 );
		}

		execvp ( g_pArgv[0], const_cast<char **> ( g_pArgv ) ); // using execvp instead execv to auto find addr2line in directories

		// if we here - execvp failed, ask user to do conversion manually
		sphSafeInfo ( iFD, "conversion failed (error '%s'):\n"
			"  1. Run the command provided below over the crashed binary (for example, '%s'):\n"
			"  2. Attach the source.txt to the bug report.", strerrorm ( errno ), g_pArgv[SPH_BT_BINARY_NAME] );

		int iColumn = 0;
		for ( int i=0; g_pArgv[i]!=NULL; i++ )
		{
			const char * s = g_pArgv[i];
			while ( *s )
				s++;
			size_t iLen = s-g_pArgv[i];
			sphWrite ( iFD, g_pArgv[i], iLen );
			sphWrite ( iFD, " ", 1 );
			int iWas = iColumn % 80;
			iColumn += iLen;
			int iNow = iColumn % 80;
			if ( iNow<iWas )
				sphWrite ( iFD, "\n", 1 );
		}
		sphWrite ( iFD, g_sSourceTail, sizeof(g_sSourceTail)-1 );
		exit ( 1 );

	} else
	if ( iChild==-1 )
	{
		sphSafeInfo ( iFD, "fork for running execvp failed: [%d] %s", errno, strerrorm(errno) );
		return;
	}

	int iStatus, iResult;
	do
	{
		// can be interrupted by pretty much anything (e.g. SIGCHLD from other searchd children)
		iResult = waitpid ( iChild, &iStatus, 0 );

		// they say this can happen if child exited and SIGCHLD was ignored
		// a cleaner one would be to temporary handle it here, but can we be bothered
		if ( iResult==-1 && errno==ECHILD )
		{
			iResult = iChild;
			iStatus = 0;
		}

		if ( iResult==-1 && errno!=EINTR )
		{
			sphSafeInfo ( iFD, "waitpid() failed: [%d] %s", errno, strerrorm(errno) );
			return;
		}
	} while ( iResult!=iChild );

	sphSafeInfo ( iFD, "--- BT to source lines finished ---" );
}

void sphBacktraceSetBinaryName ( const char * sName )
{
	g_sBinaryName = sName;
	g_pArgv[SPH_BT_BINARY_NAME] = g_sBinaryName.cstr();
}

void sphBacktraceInit()
{
#if HAVE_BACKTRACE
	backtrace ( g_pBacktraceAddresses, SPH_BACKTRACE_ADDR_COUNT );
#endif // !HAVE_BACKTRACE

	// check that jemalloc is present
	bool bSafeGdb = true;
#if HAVE_DLOPEN
	void * fnJMalloc = dlsym ( RTLD_DEFAULT, "mallctl" );
	bSafeGdb = ( fnJMalloc!=nullptr );
#endif
#ifndef NDEBUG
	bSafeGdb = true;
#endif
	g_bSafeGDB = bSafeGdb;
}

#else // _WIN32

const char * DoBacktrace ( int, int )
{
	return NULL; ///< sorry, no backtraces on Windows...
}

void sphBacktrace ( EXCEPTION_POINTERS * pExc, const char * sFile )
{
	if ( !pExc || !sFile || !(*sFile) )
	{
		sphInfo ( "can't generate minidump" );
		return;
	}

	HANDLE hFile = CreateFile ( sFile, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0 );
	if ( hFile==INVALID_HANDLE_VALUE )
	{
		sphInfo ( "can't create minidump file '%s'", sFile );
		return;
	}

	MINIDUMP_EXCEPTION_INFORMATION tExcInfo;
	tExcInfo.ExceptionPointers = pExc;
	tExcInfo.ClientPointers = FALSE;
	tExcInfo.ThreadId = GetCurrentThreadId();

	bool bDumped = ( MiniDumpWriteDump ( GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpNormal, &tExcInfo, 0, 0 )==TRUE );
	CloseHandle ( hFile );

	if ( !bDumped )
		sphInfo ( "can't dump minidump" );
}

void sphBacktraceSetBinaryName ( const char * )
{
}

void sphBacktraceInit()
{
}

#endif // _WIN32


static bool g_bUnlinkOld = true;
void sphSetUnlinkOld ( bool bUnlink )
{
	g_bUnlinkOld = bUnlink;
}

bool sphGetUnlinkOld ()
{
	return g_bUnlinkOld;
}

void sphUnlinkIndex ( const char * sName, bool bForce )
{
	if ( !( g_bUnlinkOld || bForce ) )
		return;
	IndexFiles_c ( sName ).Unlink ();
}


void sphCheckDuplicatePaths ( const CSphConfig & hConf )
{
	if ( !hConf.Exists ( "index" ) )
		return;

	CSphOrderedHash < CSphString, CSphString, CSphStrHashFunc, 256 > hPaths;
	hConf["index"].IterateStart ();
	while ( hConf["index"].IterateNext() )
	{
		CSphConfigSection & hIndex = hConf["index"].IterateGet ();
		if ( hIndex ( "path" ) )
		{
			const CSphString & sIndex = hConf["index"].IterateGetKey ();
			if ( hPaths ( hIndex["path"].strval() ) )
				sphDie ( "duplicate paths: index '%s' has the same path as '%s'.\n", sIndex.cstr(), hPaths[hIndex["path"].strval()].cstr() );
			hPaths.Add ( sIndex, hIndex["path"].strval() );
		}
	}
}


void sphConfigureCommon ( const CSphConfig & hConf )
{
	if ( !hConf("common") || !hConf["common"]("common") )
		return;

	CSphConfigSection & hCommon = hConf["common"]["common"];
	g_sLemmatizerBase = hCommon.GetStr ( "lemmatizer_base" );

	bool bJsonStrict = false;
	bool bJsonAutoconvNumbers;
	bool bJsonKeynamesToLowercase = false;

	if ( hCommon("on_json_attr_error") )
	{
		const CSphString & sVal = hCommon["on_json_attr_error"].strval();
		if ( sVal=="ignore_attr" )
			bJsonStrict = false;
		else if ( sVal=="fail_index" )
			bJsonStrict = true;
		else
			sphDie ( "unknown on_json_attr_error value (must be one of ignore_attr, fail_index)" );
	}

	if ( hCommon("json_autoconv_keynames") )
	{
		const CSphString & sVal = hCommon["json_autoconv_keynames"].strval();
		if ( sVal=="lowercase" )
			bJsonKeynamesToLowercase = true;
		else
			sphDie ( "unknown json_autoconv_keynames value (must be 'lowercase')" );
	}

	bJsonAutoconvNumbers = ( hCommon.GetInt ( "json_autoconv_numbers", 0 )!=0 );
	sphSetJsonOptions ( bJsonStrict, bJsonAutoconvNumbers, bJsonKeynamesToLowercase );

	if ( hCommon("plugin_dir") )
		sphPluginInit ( hCommon["plugin_dir"].cstr() );
	else
		sphPluginInit ( HARDCODED_PLUGIN_DIR );
}

bool sphIsChineseCode ( int iCode )
{
	return ( ( iCode>=0x2E80 && iCode<=0x2EF3 ) ||	// CJK radicals
		( iCode>=0x2F00 && iCode<=0x2FD5 ) ||	// Kangxi radicals
		( iCode>=0x3000 && iCode<=0x303F ) ||	// CJK Symbols and Punctuation
		( iCode>=0x3105 && iCode<=0x312D ) ||	// Bopomofo
		( iCode>=0x31C0 && iCode<=0x31E3 ) ||	// CJK strokes
		( iCode>=0x3400 && iCode<=0x4DB5 ) ||	// CJK Ideograph Extension A
		( iCode>=0x4E00 && iCode<=0x9FFF ) ||	// Ideograph
		( iCode>=0xF900 && iCode<=0xFAD9 ) ||	// compatibility ideographs
		( iCode>=0xFF00 && iCode<=0xFFEF ) ||	// Halfwidth and fullwidth forms
		( iCode>=0x20000 && iCode<=0x2FA1D ) );	// CJK Ideograph Extensions B/C/D, and compatibility ideographs
}

bool sphDetectChinese ( const BYTE * szBuffer, int iLength )
{
	if ( !szBuffer || !iLength )
		return false;

	const BYTE * pBuffer = szBuffer;
	while ( pBuffer<szBuffer+iLength )
	{
		int iCode = sphUTF8Decode ( pBuffer );
		if ( sphIsChineseCode ( iCode ) )
			return true;
	}

	return false;
}


#if HAVE_DLOPEN

CSphDynamicLibrary::CSphDynamicLibrary ( const char * sPath )
	: m_bReady ( false )
	, m_pLibrary ( nullptr )
{
	m_pLibrary = dlopen ( sPath, RTLD_NOW | RTLD_GLOBAL );
	if ( !m_pLibrary )
		sphLogDebug ( "dlopen(%s) failed", sPath );
	else
		sphLogDebug ( "dlopen(%s)=%p", sPath, m_pLibrary );
}

CSphDynamicLibrary::~CSphDynamicLibrary()
{
	if ( m_pLibrary )
		dlclose ( m_pLibrary );
};

bool CSphDynamicLibrary::LoadSymbols ( const char** sNames, void*** pppFuncs, int iNum )
{
	if ( !m_pLibrary )
		return false;

	if ( m_bReady )
		return true;

	for ( int i=0; i<iNum; ++i )
	{
		void* pResult = dlsym ( m_pLibrary, sNames[i] );
		if ( !pResult )
		{
			sphLogDebug ( "Symbol %s not found", sNames[i] );
			return false;
		}
		// yes, it is void*** - triple pointer.
		// void* is the legacy pointer (to the function, in this case).
		// void** is the variable where we store the pointer to the function.
		// that is to cast all different pointers to legacy void*.
		// we put the addresses to these variables into array, and it adds
		// one more level of indirection. void*** actually is void**[]
		*pppFuncs[i] = pResult;
	}
	m_bReady = true;
	return true;
};

#else

CSphDynamicLibrary::CSphDynamicLibrary ( const char * ) {};
bool CSphDynamicLibrary::LoadSymbols ( const char **, void ***, int ) { return false; }

#endif

// calculate new weights as inverse freqs of timers, giving also small probability to bad timers.
void RebalanceWeights ( const CSphFixedVector<int64_t> & dTimers, CSphFixedVector<float>& dWeights )
{
	assert ( dTimers.GetLength () );
	float fSum = 0.0;
	int iAlive = 0;

	// weights are proportional to frequencies (inverse to timers)
	CSphFixedVector<float> dFrequencies { dTimers.GetLength () };

	ARRAY_FOREACH ( i, dTimers )
	if ( dTimers[i]>0 )
	{
		dFrequencies[i] = ( 1.0f / dTimers[i] );
		fSum += dFrequencies[i];
		++iAlive;
	}

	// no statistics, all timers bad, keep previous weights
	if ( !iAlive )
		return;

	// if one or more bad (empty) timers provided, give fEmptiesPercent frac to all of them,
	// and also assume fEmptiesPercent/num_of_deads fraq per each of them.
	float fEmptyPercent = 0.0f;

	// balance weights
	float fCheck = 0;
	ARRAY_FOREACH ( i, dFrequencies )
	{
		// mirror weight is inverse of timer \ query time
		float fWeight = 100.0f * dFrequencies[i] / fSum;

		// mirror without response
		if ( dTimers[i]<=0 )
			fWeight = fEmptyPercent;

		assert ( fWeight>=0.0 && fWeight<=100.0 );
		dWeights[i] = fWeight;
		fCheck += fWeight;
	}
	assert ( fCheck<=100.000001 && fCheck>=99.99999);
}

/// collect warnings/errors from any suitable context.
Warner_c::Warner_c ( const char * sDel, const char * sPref, const char * sTerm )
	: m_sWarnings ( sDel, sPref, sTerm )
	, m_sErrors ( sDel, sPref, sTerm )
	, m_sDel ( sDel )
	, m_sPref ( sPref )
	, m_sTerm ( sTerm )
{}

Warner_c::Warner_c ( Warner_c &&rhs ) noexcept
{
	m_sWarnings = std::move (rhs.m_sWarnings);
	m_sErrors = std::move (rhs.m_sErrors);
	m_sDel = rhs.m_sDel;
	m_sPref = rhs.m_sPref;
	m_sTerm = rhs.m_sTerm;
}

Warner_c& Warner_c::operator= ( Warner_c && rhs ) noexcept
{
	if ( &rhs!=this )
	{
		m_sWarnings = std::move ( rhs.m_sWarnings );
		m_sErrors = std::move ( rhs.m_sErrors );
		m_sDel = rhs.m_sDel;
		m_sPref = rhs.m_sPref;
		m_sTerm = rhs.m_sTerm;
	}
	return *this;
}


void Warner_c::Err ( const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	m_sErrors.vSprintf ( sFmt, ap );
	va_end ( ap );
}

void Warner_c::Err ( const CSphString &sMsg )
{
	m_sErrors << sMsg;
}

void Warner_c::Warn ( const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	m_sWarnings.vSprintf ( sFmt, ap );
	va_end ( ap );
}

void Warner_c::Warn ( const CSphString &sMsg )
{
	m_sWarnings << sMsg;
}

void Warner_c::Clear ()
{
	m_sErrors.Clear ();
	m_sWarnings.Clear();
	if ( m_sDel || m_sPref || m_sTerm )
	{
		m_sErrors.StartBlock ( m_sDel, m_sPref, m_sTerm );
		m_sWarnings.StartBlock ( m_sDel, m_sPref, m_sTerm );
	}
}

const char * Warner_c::sError () const
{
	return m_sErrors.cstr();
}

const char * Warner_c::sWarning () const
{
	return m_sWarnings.cstr();
}

void Warner_c::AddStringsFrom ( const Warner_c &sSrc )
{
	if ( !sSrc.WarnEmpty () )
		m_sWarnings << sSrc.sWarning ();

	if ( !sSrc.ErrEmpty () )
		m_sWarnings << sSrc.sError ();
}

void Warner_c::MoveErrorsTo ( CSphString &sTarget )
{
	m_sErrors.FinishBlocks();
	m_sErrors.MoveTo ( sTarget );
}

void Warner_c::MoveWarningsTo ( CSphString &sTarget )
{
	m_sWarnings.FinishBlocks();
	m_sWarnings.MoveTo ( sTarget );
}

void Warner_c::MoveAllTo ( CSphString &sTarget )
{
	m_sErrors.FinishBlocks();
	m_sWarnings.FinishBlocks();
	StringBuilder_c sCollection ( "; ", m_sPref, m_sTerm );

	sCollection.StartBlock ( nullptr, "ERRORS: ");
	sCollection << m_sErrors.cstr();
	sCollection.FinishBlock();

	sCollection.StartBlock ( nullptr, "WARNINGS: " );
	sCollection << m_sWarnings.cstr();

	sCollection.FinishBlocks();
	sCollection.MoveTo ( sTarget );
	Clear();
}

namespace TlsMsg
{

	static StringBuilder_c* TlsMsgs ( bool bDoClear=true )
	{
		thread_local StringBuilder_c* pContainer;
		if (!pContainer)
		{
			static StringBuilder_c sMsgs;
			pContainer = &sMsgs;
		}

		if ( bDoClear )
			pContainer->Clear();
		return pContainer;
	}

	bool Err( const char* sFmt, ... )
	{
		StringBuilder_c sMsgs;
		va_list ap;
		va_start ( ap, sFmt );
		sMsgs.vSprintf( sFmt, ap );
		va_end ( ap );
		TlsMsgs ()->Swap ( sMsgs );
		return false;
	}

	bool Err( const CSphString& sMsg )
	{
		if (sMsg.IsEmpty())
			return true;
		*TlsMsgs() << sMsg;
		return false;
	}

	StringBuilder_c& Err() { return *TlsMsgs(); }
	const char* szError() { return TlsMsgs(false)->cstr(); }
	void MoveError ( CSphString& sError )
	{
		auto& sStream = *TlsMsgs( false );
		if ( sStream.IsEmpty())
			return;
		sStream.MoveTo(sError);
	}

	bool HasErr() { return !TlsMsgs(false)->IsEmpty(); }
}

const char * GetBaseName ( const CSphString & sFullPath )
{
	if ( sFullPath.IsEmpty() )
		return nullptr;

	const char * pStart = sFullPath.cstr();
	const char * pCur = pStart + sFullPath.Length() - 1;
	while ( pCur>pStart && pCur[-1]!='/' && pCur[-1]!='\\' )
		pCur--;

	return pCur;
}

static std::atomic<int64_t> g_iUID { 1 };
static int64_t g_iUidBase = 0;

int64_t UidShort()
{
	int64_t iVal = g_iUID.fetch_add (1, std::memory_order_relaxed);
	int64_t iUID = g_iUidBase + iVal;
	return iUID;
}

void UidShortSetup ( int iServer, int iStarted )
{
	int64_t iSeed = ( (int64_t)iServer & 0x7f ) << 56;
	iSeed += ((int64_t)iStarted ) << 24;
	g_iUidBase = iSeed;
	sphLogDebug ( "uid-short server_id %d, started %d, seed " INT64_FMT, iServer, iStarted, iSeed );
}

// RNG of the integers 0-255
static BYTE g_dPearsonRNG[256] = {
	    98,  6, 85,150, 36, 23,112,164,135,207,169,  5, 26, 64,165,219, //  1
	    61, 20, 68, 89,130, 63, 52,102, 24,229,132,245, 80,216,195,115, //  2
	    90,168,156,203,177,120,  2,190,188,  7,100,185,174,243,162, 10, //  3
	   237, 18,253,225,  8,208,172,244,255,126,101, 79,145,235,228,121, //  4
	   123,251, 67,250,161,  0,107, 97,241,111,181, 82,249, 33, 69, 55, //  5
		59,153, 29,  9,213,167, 84, 93, 30, 46, 94, 75,151,114, 73,222, //  6
	   197, 96,210, 45, 16,227,248,202, 51,152,252,125, 81,206,215,186, //  7
	    39,158,178,187,131,136,  1, 49, 50, 17,141, 91, 47,129, 60, 99, //  8
	   154, 35, 86,171,105, 34, 38,200,147, 58, 77,118,173,246, 76,254, //  9
	   133,232,196,144,198,124, 53,  4,108, 74,223,234,134,230,157,139, // 10
	   189,205,199,128,176, 19,211,236,127,192,231, 70,233, 88,146, 44, // 11
	   183,201, 22, 83, 13,214,116,109,159, 32, 95,226,140,220, 57, 12, // 12
	   221, 31,209,182,143, 92,149,184,148, 62,113, 65, 37, 27,106,166, // 13
	     3, 14,204, 72, 21, 41, 56, 66, 28,193, 40,217, 25, 54,179,117, // 14
	   238, 87,240,155,180,170,242,212,191,163, 78,218,137,194,175,110, // 15
		43,119,224, 71,122,142, 42,160,104, 48,247,103, 15, 11,138,239  // 16
};

BYTE Pearson8 ( const BYTE * pBuf, int iLen )
{
	const BYTE * pEnd = pBuf + iLen;
	BYTE iNew = 0;

	while ( pBuf<pEnd )
	{
		iNew = g_dPearsonRNG[ iNew ^ (*pBuf) ];
		pBuf++;
	}

	return iNew;
}


namespace { // to make logMutex static inside .o
//	CSphMutex & logMutex ()
//	{
//		return Single_T<CSphMutex, LogMessage_t> ();
//	}
}

LogMessage_t::LogMessage_t ( BYTE uLevel )
	: m_eLevel ( (ESphLogLevel) uLevel )
{
//	logMutex ().Lock ();
}

LogMessage_t::~LogMessage_t ()
{
//	logMutex ().Unlock ();
//	sphLogDebugv ( "%s", m_dLog.cstr() );
	sphLogf ( m_eLevel, "%s", m_dLog.cstr() );
}

LocMessage_c::LocMessage_c ( LocMessages_c* pOwner )
	: m_pOwner ( pOwner )
{
//	m_pOwner->m_tLock.Lock();
}

LocMessage_c::~LocMessage_c ()
{
//	m_pOwner->m_tLock.Unlock ();
	m_pOwner->Append ( m_dLog );
}

void LocMessages_c::Append ( StringBuilder_c& dMsg )
{
	auto pLeaf = new MsgList;
	dMsg.MoveTo(pLeaf->m_sMsg);
	pLeaf->m_pNext = m_sMsgs;
	m_sMsgs = pLeaf;
	++m_iMsgs;
}

void LocMessages_c::Swap ( LocMessages_c & rhs ) noexcept
{
	::Swap (m_sMsgs, rhs.m_sMsgs);
	::Swap ( m_iMsgs, rhs.m_iMsgs );
}


int LocMessages_c::Print () const
{
	for ( auto pHead = m_sMsgs; pHead; pHead = pHead->m_pNext )
		sphLogDebug ( "%s", pHead->m_sMsg.scstr() );
	return m_iMsgs;
}

LocMessages_c::~LocMessages_c()
{
	for ( auto pHead = m_sMsgs; pHead!=nullptr; )
	{
		auto pNext = pHead->m_pNext;
		SafeDelete ( pHead );
		pHead = pNext;
	}
}
