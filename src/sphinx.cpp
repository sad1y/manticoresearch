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

#include "sphinx.h"
#include "sphinxstem.h"
#include "sphinxquery.h"
#include "sphinxutils.h"
#include "sphinxsort.h"
#include "fileutils.h"
#include "sphinxexpr.h"
#include "sphinxfilter.h"
#include "sphinxint.h"
#include "sphinxsearch.h"
#include "searchnode.h"
#include "sphinxjson.h"
#include "sphinxplugin.h"
#include "sphinxqcache.h"
#include "icu.h"
#include "attribute.h"
#include "secondaryindex.h"
#include "histogram.h"
#include "killlist.h"
#include "docstore.h"
#include "global_idf.h"
#include "indexformat.h"
#include "indexcheck.h"
#include "coroutine.h"
#include "columnarlib.h"
#include "columnarmisc.h"
#include "columnarfilter.h"
#include "mini_timer.h"
#include "sphinx_alter.h"
#include "conversion.h"
#include "binlog.h"
#include "task_info.h"
#include "client_task_info.h"
#include "chunksearchctx.h"
#include "lrucache.h"
#include "indexfiles.h"

#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>
#include <algorithm>

#if WITH_STEMMER
#include <libstemmer.h>
#endif

#if WITH_RE2
#include <string>
#include <re2/re2.h>
#endif

#if _WIN32
	#define snprintf	_snprintf
#else
	#include <unistd.h>
	#include <sys/time.h>
#endif

/////////////////////////////////////////////////////////////////////////////

// logf() is not there sometimes (eg. Solaris 9)
#if !_WIN32 && !HAVE_LOGF
static inline float logf ( float v )
{
	return (float) log ( v );
}
#endif

#if _WIN32
void localtime_r ( const time_t * clock, struct tm * res )
{
	tm * pRes = localtime ( clock );
	if ( pRes )
		*res = *pRes;
}

void gmtime_r ( const time_t * clock, struct tm * res )
{
	tm * pRes = gmtime ( clock );
	if ( pRes )
		*res = *pRes;
}
#endif

#include <boost/preprocessor/repetition/repeat.hpp>

#include "attrindex_builder.h"
#include "stripper/html_stripper.h"
#include "tokenizer/charset_definition_parser.h"
#include "tokenizer/multiform_container.h"
#include "tokenizer/tokenizer.h"
#include "queryfilter.h"
#include "indexing_sources/source_document.h"
#include "indexing_sources/source_stats.h"

// forward decl
void sphWarn ( const char * sTemplate, ... ) __attribute__ ( ( format ( printf, 1, 2 ) ) );

/////////////////////////////////////////////////////////////////////////////
// GLOBALS
/////////////////////////////////////////////////////////////////////////////

const char *		MAGIC_WORD_SENTENCE		= "\3sentence";		// emitted from source on sentence boundary, stored in dictionary
const char *		MAGIC_WORD_PARAGRAPH	= "\3paragraph";	// emitted from source on paragraph boundary, stored in dictionary

bool				g_bJsonStrict				= false;
bool				g_bJsonAutoconvNumbers		= false;
bool				g_bJsonKeynamesToLowercase	= false;

static const int	MIN_READ_BUFFER			= 8192;
static const int	MIN_READ_UNHINTED		= 1024;

static int 			g_iReadUnhinted 		= DEFAULT_READ_UNHINTED;

static int			g_iSplitThresh			= 8192;

CSphString			g_sLemmatizerBase		= GET_FULL_SHARE_DIR ();

// quick hack for indexer crash reporting
// one day, these might turn into a callback or something
int64_t		g_iIndexerCurrentDocID		= 0;
int64_t		g_iIndexerCurrentHits		= 0;
int64_t		g_iIndexerCurrentRangeMin	= 0;
int64_t		g_iIndexerCurrentRangeMax	= 0;
int64_t		g_iIndexerPoolStartDocID	= 0;
int64_t		g_iIndexerPoolStartHit		= 0;

static bool IndexBuildDone ( const BuildHeader_t & tBuildHeader, const WriteHeader_t & tWriteHeader, const CSphString & sFileName, CSphString & sError );

/////////////////////////////////////////////////////////////////////////////
// COMPILE-TIME CHECKS
/////////////////////////////////////////////////////////////////////////////

STATIC_SIZE_ASSERT ( SphOffset_t, 8 );

/////////////////////////////////////////////////////////////////////////////
// INTERNAL SPHINX CLASSES DECLARATIONS
/////////////////////////////////////////////////////////////////////////////

/// possible bin states
enum ESphBinState
{
	BIN_ERR_READ	= -2,	///< bin read error
	BIN_ERR_END		= -1,	///< bin end
	BIN_POS			= 0,	///< bin is in "expects pos delta" state
	BIN_DOC			= 1,	///< bin is in "expects doc delta" state
	BIN_WORD		= 2		///< bin is in "expects word delta" state
};


enum ESphBinRead
{
	BIN_READ_OK,			///< bin read ok
	BIN_READ_EOF,			///< bin end
	BIN_READ_ERROR,			///< bin read error
	BIN_PRECACHE_OK,		///< precache ok
	BIN_PRECACHE_ERROR		///< precache failed
};


/// aggregated hit info
struct CSphAggregateHit
{
	RowID_t			m_tRowID {INVALID_ROWID};	///< document ID
	SphWordID_t		m_uWordID {0};				///< word ID in current dictionary
	const BYTE *	m_sKeyword {nullptr};		///< word itself (in keywords dictionary case only)
	Hitpos_t		m_iWordPos {0};				///< word position in current document, or hit count in case of aggregate hit
	FieldMask_t		m_dFieldMask;				///< mask of fields containing this word, 0 for regular hits, non-0 for aggregate hits

	int GetAggrCount () const
	{
		assert ( !m_dFieldMask.TestAll ( false ) );
		return m_iWordPos;
	}

	void SetAggrCount ( int iVal )
	{
		m_iWordPos = iVal;
	}
};


/// bin, block input buffer
struct CSphBin
{
	static const int	MIN_SIZE	= 8192;
	static const int	WARN_SIZE	= 262144;

protected:
	ESphHitless			m_eMode;
	int					m_iSize = 0;

	BYTE *				m_dBuffer = nullptr;
	BYTE *				m_pCurrent = nullptr;
	int					m_iLeft = 0;
	int					m_iDone = 0;
	ESphBinState		m_eState { BIN_POS };
	bool				m_bWordDict;
	bool				m_bError = false;	// FIXME? sort of redundant, but states are a mess

	CSphAggregateHit	m_tHit;									///< currently decoded hit
	BYTE				m_sKeyword [ MAX_KEYWORD_BYTES ];	///< currently decoded hit keyword (in keywords dict mode)

#ifndef NDEBUG
	SphWordID_t			m_iLastWordID = 0;
	BYTE				m_sLastKeyword [ MAX_KEYWORD_BYTES ];
#endif

	int					m_iFile = -1;	///< my file
	SphOffset_t *		m_pFilePos = nullptr;	///< shared current offset in file

public:
	SphOffset_t			m_iFilePos = 0;		///< my current offset in file
	int					m_iFileLeft = 0;	///< how much data is still unread from the file

public:
	explicit 			CSphBin ( ESphHitless eMode = SPH_HITLESS_NONE, bool bWordDict = false );
						~CSphBin ();

	static int			CalcBinSize ( int iMemoryLimit, int iBlocks, const char * sPhase, bool bWarn = true );
	void				Init ( int iFD, SphOffset_t * pSharedOffset, const int iBinSize );

	SphWordID_t			ReadVLB ();
	int					ReadByte ();
	ESphBinRead			ReadBytes ( void * pDest, int iBytes );
	int					ReadHit ( CSphAggregateHit * pOut );

	DWORD				UnzipInt ();
	SphOffset_t			UnzipOffset ();

	bool				IsEOF () const;
	bool				IsDone () const;
	bool				IsError () const { return m_bError; }
	ESphBinRead			Precache ();
};

//////////////////////////////////////////////////////////////////////////

static const char * g_dRankerNames[] =
{
	"proximity_bm25",
	"bm25",
	"none",
	"wordcount",
	"proximity",
	"matchany",
	"fieldmask",
	"sph04",
	"expr",
	"export",
	NULL
};


const char * sphGetRankerName ( ESphRankMode eRanker )
{
	if ( eRanker<SPH_RANK_PROXIMITY_BM25 || eRanker>=SPH_RANK_TOTAL )
		return NULL;

	return g_dRankerNames[eRanker];
}

//////////////////////////////////////////////////////////////////////////

struct SkipCacheKey_t
{
	int64_t		m_iIndexId;
	SphWordID_t	m_tWordId;

	bool operator == ( const SkipCacheKey_t & tKey ) const { return m_iIndexId==tKey.m_iIndexId && m_tWordId==tKey.m_tWordId; }
};


struct SkipCacheUtil_t
{
	static DWORD GetHash ( SkipCacheKey_t tKey )
	{
		DWORD uCRC32 = sphCRC32 ( &tKey.m_iIndexId, sizeof(tKey.m_iIndexId) );
		return sphCRC32 ( &tKey.m_tWordId, sizeof(tKey.m_tWordId), uCRC32 );
	}

	static DWORD GetSize ( SkipData_t * pValue )	{ return pValue ? pValue->m_dSkiplist.GetLengthBytes() : 0; }
	static void Reset ( SkipData_t * & pValue )		{ SafeDelete(pValue); }
};


class SkipCache_c : public LRUCache_T<SkipCacheKey_t, SkipData_t*, SkipCacheUtil_t>
{
	using BASE = LRUCache_T<SkipCacheKey_t, SkipData_t*, SkipCacheUtil_t>;
	using BASE::BASE;

public:
	void					DeleteAll ( int64_t iIndexId ) { BASE::Delete ( [iIndexId]( const SkipCacheKey_t & tKey ){ return tKey.m_iIndexId==iIndexId; } ); }

	static void				Init ( int64_t iCacheSize );
	static void				Done()	{ SafeDelete(m_pSkipCache); }
	static SkipCache_c *	Get()	{ return m_pSkipCache; }

private:
	static SkipCache_c *	m_pSkipCache;
};

SkipCache_c * SkipCache_c::m_pSkipCache = nullptr;


void SkipCache_c::Init ( int64_t iCacheSize )
{
	assert ( !m_pSkipCache );
	if ( iCacheSize > 0 )
		m_pSkipCache = new SkipCache_c(iCacheSize);
}


void InitSkipCache ( int64_t iCacheSize )
{
	SkipCache_c::Init(iCacheSize);
}


void ShutdownSkipCache()
{
	SkipCache_c::Done();
}

/////////////////////////////////////////////////////////////////////

/// everything required to setup search term
class DiskIndexQwordSetup_c : public ISphQwordSetup
{
public:
	DiskIndexQwordSetup_c ( DataReaderFactory_c * pDoclist, DataReaderFactory_c * pHitlist, const BYTE * pSkips, int iSkiplistBlockSize, bool bSetupReaders, RowID_t iRowsCount )
		: m_pDoclist ( pDoclist )
		, m_pHitlist ( pHitlist )
		, m_pSkips ( pSkips )
		, m_iSkiplistBlockSize ( iSkiplistBlockSize )
		, m_bSetupReaders ( bSetupReaders )
		, m_iRowsCount ( iRowsCount )
	{
		SafeAddRef(pDoclist);
		SafeAddRef(pHitlist);
	}

	ISphQword *			QwordSpawn ( const XQKeyword_t & tWord ) const final;
	bool				QwordSetup ( ISphQword * ) const final;
	bool				Setup ( ISphQword * ) const;
	ISphQword *			ScanSpawn() const override;

private:
	DataReaderFactoryPtr_c		m_pDoclist;
	DataReaderFactoryPtr_c		m_pHitlist;
	const BYTE *				m_pSkips;
	int							m_iSkiplistBlockSize = 0;
	bool						m_bSetupReaders = false;
	RowID_t						m_iRowsCount = INVALID_ROWID;

private:
	bool SetupWithWrd ( const DiskIndexQwordTraits_c& tWord, CSphDictEntry& tRes ) const;
	bool SetupWithCrc ( const DiskIndexQwordTraits_c& tWord, CSphDictEntry& tRes ) const;
};

/// query word from the searcher's point of view
template < bool INLINE_HITS, bool DISABLE_HITLIST_SEEK >
class DiskIndexQword_c : public DiskIndexQwordTraits_c
{
public:
	DiskIndexQword_c ( bool bUseMinibuffer, bool bExcluded, int64_t iIndexId )
		: DiskIndexQwordTraits_c ( bUseMinibuffer, bExcluded )
		, m_iIndexId ( iIndexId )
	{}

	~DiskIndexQword_c()
	{
		if ( m_bSkipFromCache )
		{
			SkipCache_c::Get()->Release ( { m_iIndexId, m_uWordID } );
			m_pSkipData = nullptr;
			m_bSkipFromCache = false;
		}
	}

	void Reset () final
	{
		if ( m_rdDoclist )
			m_rdDoclist->Reset ();
		if ( m_rdHitlist )
			m_rdHitlist->Reset ();
		ResetDecoderState();
	}

	void GetHitlistEntry ()
	{
		assert ( !m_bHitlistOver );
		DWORD iDelta = m_rdHitlist->UnzipInt ();
		if ( iDelta )
		{
			m_iHitPos += iDelta;
		} else
		{
			m_iHitPos = EMPTY_HIT;
#ifndef NDEBUG
			m_bHitlistOver = true;
#endif
		}
	}


	RowID_t AdvanceTo ( RowID_t tRowID ) final
	{
		if ( m_tDoc.m_tRowID!=INVALID_ROWID && tRowID<=m_tDoc.m_tRowID )
			return m_tDoc.m_tRowID;

		bool bRewound = HintRowID (tRowID);
		if ( bRewound || m_tDoc.m_tRowID==INVALID_ROWID )
			ReadNext();

		while ( m_tDoc.m_tRowID < tRowID )
			ReadNext();

		return m_tDoc.m_tRowID;
	}


	bool HintRowID ( RowID_t tRowID ) final
	{
		// tricky bit
		// FindSpan() will match a block where tBaseRowIDPlus1[i] <= tRowID < tBaseRowIDPlus1[i+1]
		// meaning that the subsequent ids decoded will be strictly > RefValue
		// meaning that if previous (!) blocks end with tRowID exactly,
		// and we use tRowID itself as RefValue, that document gets lost!

		// first check if we're still inside the last block
		if ( m_iSkipListBlock==-1 )
		{
			if ( !m_pSkipData )
				return true;

			m_iSkipListBlock = FindSpan ( m_pSkipData->m_dSkiplist, tRowID );
			if ( m_iSkipListBlock<0 )
				return false;
		}
		else
		{
			assert(m_pSkipData);
			const auto & dSkiplist = m_pSkipData->m_dSkiplist;

			if ( m_iSkipListBlock < dSkiplist.GetLength()-1 )
			{
				int iNextBlock = m_iSkipListBlock+1;
				RowID_t tNextBlockRowID = dSkiplist[iNextBlock].m_tBaseRowIDPlus1;
				if ( tRowID>=tNextBlockRowID )
				{
					auto dSkips = VecTraits_T<SkiplistEntry_t> ( &dSkiplist[iNextBlock], dSkiplist.GetLength()-iNextBlock );
					m_iSkipListBlock = FindSpan ( dSkips, tRowID );
					if ( m_iSkipListBlock<0 )
						return false;

					m_iSkipListBlock += iNextBlock;
				}
			}
			else // we're already at our last block, no need to search
				return false;
		}

		assert(m_pSkipData);
		const SkiplistEntry_t & t = m_pSkipData->m_dSkiplist[m_iSkipListBlock];
		if ( t.m_iOffset<=m_rdDoclist->GetPos() )
			return false;

		m_rdDoclist->SeekTo ( t.m_iOffset, -1 );
		m_tDoc.m_tRowID = t.m_tBaseRowIDPlus1-1;
		m_uHitPosition = m_iHitlistPos = t.m_iBaseHitlistPos;

		return true;
	}

	const CSphMatch & GetNextDoc() override
	{
		ReadNext();
		return m_tDoc;
	}

	void SeekHitlist ( SphOffset_t uOff ) final
	{
		if ( uOff >> 63 )
		{
			m_uHitState = 1;
			m_uInlinedHit = (DWORD)uOff; // truncate high dword
		} else
		{
			m_uHitState = 0;
			m_iHitPos = EMPTY_HIT;
			if_const ( DISABLE_HITLIST_SEEK )
				assert ( m_rdHitlist->GetPos()==uOff ); // make sure we're where caller thinks we are.
			else
				m_rdHitlist->SeekTo ( uOff, READ_NO_SIZE_HINT );
		}
#ifndef NDEBUG
		m_bHitlistOver = false;
#endif
	}

	Hitpos_t GetNextHit () final
	{
		assert ( m_bHasHitlist );
		switch ( m_uHitState )
		{
		case 0: // read hit from hitlist
			GetHitlistEntry ();
			return m_iHitPos;

		case 1: // return inlined hit
			m_uHitState = 2;
			return m_uInlinedHit;

		case 2: // return end-of-hitlist marker after inlined hit
#ifndef NDEBUG
			m_bHitlistOver = true;
#endif
			m_uHitState = 0;
			return EMPTY_HIT;
		}
		sphDie ( "INTERNAL ERROR: impossible hit emitter state" );
		return EMPTY_HIT;
	}

	bool Setup ( const DiskIndexQwordSetup_c * pSetup ) override
	{
		return pSetup->Setup ( this );
	}

	using is_worddict =  std::integral_constant<bool, !DISABLE_HITLIST_SEEK>;

private:
	int m_iSkipListBlock = -1;

	inline void ReadNext()
	{
		RowID_t uDelta = m_rdDoclist->UnzipRowid();
		if ( uDelta )
		{
			m_bAllFieldsKnown = false;
			m_tDoc.m_tRowID += uDelta;

			if_const ( INLINE_HITS )
			{
				m_uMatchHits = m_rdDoclist->UnzipInt();
				const DWORD uFirst = m_rdDoclist->UnzipInt();
				if ( m_uMatchHits==1 && m_bHasHitlist )
				{
					DWORD uField = m_rdDoclist->UnzipInt(); // field and end marker
					m_iHitlistPos = uFirst | ( uField << 23 ) | ( U64C(1)<<63 );
					m_dQwordFields.UnsetAll();
					// want to make sure bad field data not cause crash
					m_dQwordFields.Set ( ( uField >> 1 ) & ( (DWORD)SPH_MAX_FIELDS-1 ) );
					m_bAllFieldsKnown = true;
				} else
				{
					m_dQwordFields.Assign32 ( uFirst );
					m_uHitPosition += m_rdDoclist->UnzipOffset();
					m_iHitlistPos = m_uHitPosition;
				}
			} else
			{
				SphOffset_t iDeltaPos = m_rdDoclist->UnzipOffset();
				assert ( iDeltaPos>=0 );

				m_iHitlistPos += iDeltaPos;

				m_dQwordFields.Assign32 ( m_rdDoclist->UnzipInt() );
				m_uMatchHits = m_rdDoclist->UnzipInt();
			}
		} else
			m_tDoc.m_tRowID = INVALID_ROWID;
	}

private:
	int64_t m_iIndexId = 0;
};


DiskIndexQwordTraits_c * sphCreateDiskIndexQword ( bool bInlineHits )
{
	if ( bInlineHits )
		return new DiskIndexQword_c<true,false> ( false, false, 0 );

	return new DiskIndexQword_c<false,false> ( false, false, 0 );
}

/////////////////////////////////////////////////////////////////////////////

#define WITH_QWORD(INDEX, NO_SEEK, NAME, ACTION)										\
do if ( (( const CSphIndex_VLN *)INDEX)->m_tSettings.m_eHitFormat==SPH_HIT_FORMAT_INLINE )	\
		{ using NAME = DiskIndexQword_c < true, NO_SEEK >; ACTION; }					\
	else																				\
		{ using NAME = DiskIndexQword_c < false, NO_SEEK >; ACTION; }					\
while(0)

/////////////////////////////////////////////////////////////////////////////

#define HITLESS_DOC_MASK 0x7FFFFFFF
#define HITLESS_DOC_FLAG 0x80000000


struct Slice64_t
{
	uint64_t	m_uOff;
	int			m_iLen;
};

struct DiskSubstringPayload_t : public ISphSubstringPayload
{
	explicit DiskSubstringPayload_t ( int iDoclists )
		: m_dDoclist ( iDoclists )
	{}
	CSphFixedVector<Slice64_t>	m_dDoclist;
};


template < bool INLINE_HITS >
class DiskPayloadQword_c : public DiskIndexQword_c<INLINE_HITS, false>
{
	typedef DiskIndexQword_c<INLINE_HITS, false> BASE;

public:
	DiskPayloadQword_c ( const DiskSubstringPayload_t * pPayload, bool bExcluded, DataReaderFactory_c * pDoclist, DataReaderFactory_c * pHitlist, int64_t iIndexId )
		: BASE ( true, bExcluded, iIndexId )
	{
		m_pPayload = pPayload;
		this->m_iDocs = m_pPayload->m_iTotalDocs;
		this->m_iHits = m_pPayload->m_iTotalHits;
		m_iDoclist = 0;

		this->SetDocReader ( pDoclist );
		this->SetHitReader ( pHitlist );
	}

	const CSphMatch & GetNextDoc() final
	{
		const CSphMatch & tMatch = BASE::GetNextDoc();
		assert ( &tMatch==&this->m_tDoc );
		if ( tMatch.m_tRowID==INVALID_ROWID && m_iDoclist<m_pPayload->m_dDoclist.GetLength() )
		{
			BASE::ResetDecoderState();
			SetupReader();
			BASE::GetNextDoc();
			assert ( this->m_tDoc.m_tRowID!=INVALID_ROWID );
		}

		return this->m_tDoc;
	}

	bool Setup ( const DiskIndexQwordSetup_c * ) final
	{
		if ( m_iDoclist>=m_pPayload->m_dDoclist.GetLength() )
			return false;

		SetupReader();
		return true;
	}

private:
	void SetupReader ()
	{
		uint64_t uDocOff = m_pPayload->m_dDoclist[m_iDoclist].m_uOff;
		int iHint = m_pPayload->m_dDoclist[m_iDoclist].m_iLen;
		m_iDoclist++;

		this->m_rdDoclist->SeekTo ( uDocOff, iHint );
	}

	const DiskSubstringPayload_t *	m_pPayload;
	int								m_iDoclist;
};


//////////////////////////////////////////////////////////////////////////

class CSphHitBuilder;

const char* CheckFmtMagic ( DWORD uHeader )
{
	if ( uHeader!=INDEX_MAGIC_HEADER )
	{
		FlipEndianess ( &uHeader );
		if ( uHeader==INDEX_MAGIC_HEADER )
#if USE_LITTLE_ENDIAN
			return "This instance is working on little-endian platform, but %s seems built on big-endian host.";
#else
			return "This instance is working on big-endian platform, but %s seems built on little-endian host.";
#endif
		else
			return "%s is invalid header file (too old index version?)";
	}
	return nullptr;
}


/// this pseudo-index used to store and manage the tokenizer
/// without any footprint in real files
//////////////////////////////////////////////////////////////////////////
class CSphTokenizerIndex : public CSphIndexStub
{
public:
						CSphTokenizerIndex ( const char * szIndexName ) : CSphIndexStub ( szIndexName, nullptr ) {}
	bool				GetKeywords ( CSphVector <CSphKeywordInfo> & , const char * , const GetKeywordsSettings_t & tSettings, CSphString * ) const final ;
	Bson_t				ExplainQuery ( const CSphString & sQuery ) const final;
};



bool CSphTokenizerIndex::GetKeywords ( CSphVector <CSphKeywordInfo> & dKeywords, const char * szQuery, const GetKeywordsSettings_t & tSettings, CSphString * ) const
{
	// short-cut if no query or keywords to fill
	if ( !szQuery || !szQuery[0] )
		return true;

	TokenizerRefPtr_c pTokenizer { m_pTokenizer->Clone ( SPH_CLONE_INDEX ) };
	pTokenizer->EnableTokenizedMultiformTracking ();

	// need to support '*' and '=' but not the other specials
	// so m_pQueryTokenizer does not work for us, gotta clone and setup one manually

	DictRefPtr_c pDict { GetStatelessDict ( m_pDict ) };

	if ( IsStarDict ( pDict->GetSettings().m_bWordDict ) )
	{
		pTokenizer->AddPlainChars ( "*" );
		pDict = new CSphDictStar ( pDict );
	}

	if ( m_tSettings.m_bIndexExactWords )
	{
		pTokenizer->AddPlainChars ( "=" );
		pDict = new CSphDictExact ( pDict );
	}

	dKeywords.Resize ( 0 );

	CSphVector<BYTE> dFiltered;
	FieldFilterRefPtr_c pFieldFilter;
	const BYTE * sModifiedQuery = (const BYTE *)szQuery;
	if ( m_pFieldFilter && szQuery )
	{
		pFieldFilter = m_pFieldFilter->Clone();
		if ( pFieldFilter && pFieldFilter->Apply ( sModifiedQuery, dFiltered, true ) )
			sModifiedQuery = dFiltered.Begin();
	}

	pTokenizer->SetBuffer ( sModifiedQuery, (int) strlen ( (const char*)sModifiedQuery) );

	CSphTemplateQueryFilter tAotFilter;
	tAotFilter.m_pTokenizer = pTokenizer;
	tAotFilter.m_pDict = pDict;
	tAotFilter.m_pSettings = &m_tSettings;
	tAotFilter.m_tFoldSettings = tSettings;
	tAotFilter.m_tFoldSettings.m_bStats = false;
	tAotFilter.m_tFoldSettings.m_bFoldWildcards = true;

	ExpansionContext_t tExpCtx;

	tAotFilter.GetKeywords ( dKeywords, tExpCtx );

	return true;
}


CSphIndex * sphCreateIndexTemplate ( const char * szIndexName )
{
	return new CSphTokenizerIndex(szIndexName);
}

Bson_t CSphTokenizerIndex::ExplainQuery ( const CSphString & sQuery ) const
{
	bool bWordDict = m_pDict->GetSettings().m_bWordDict;

	WordlistStub_c tWordlist;
	ExplainQueryArgs_t tArgs ( sQuery );
	tArgs.m_pDict = GetStatelessDict ( m_pDict );
	if ( IsStarDict ( bWordDict ) )
		tArgs.m_pDict = new CSphDictStarV8 ( tArgs.m_pDict, m_tSettings.m_iMinInfixLen>0 );
	if ( m_tSettings.m_bIndexExactWords )
		tArgs.m_pDict = new CSphDictExact ( tArgs.m_pDict );
	if ( m_pFieldFilter )
		tArgs.m_pFieldFilter = m_pFieldFilter->Clone();
	tArgs.m_pSettings = &m_tSettings;
	tArgs.m_pWordlist = &tWordlist;
	tArgs.m_pQueryTokenizer = m_pQueryTokenizer;
	tArgs.m_iExpandKeywords = m_tMutableSettings.m_iExpandKeywords;
	tArgs.m_iExpansionLimit = m_iExpansionLimit;
	tArgs.m_bExpandPrefix = ( bWordDict && IsStarDict ( bWordDict ) );

	return Explain ( tArgs );
}

//////////////////////////////////////////////////////////////////////////

UpdateContext_t::UpdateContext_t ( AttrUpdateInc_t & tUpd, const ISphSchema & tSchema )
	: m_tUpd ( tUpd )
	, m_tSchema ( tSchema )
	, m_dUpdatedAttrs ( tUpd.m_pUpdate->m_dAttributes.GetLength() )
	, m_dSchemaUpdateMask ( tSchema.GetAttrsCount() )
{}

//////////////////////////////////////////////////////////////////////////

bool IndexUpdateHelper_c::Update_CheckAttributes ( const CSphAttrUpdate & tUpd, const ISphSchema & tSchema, CSphString & sError )
{
	for ( const auto & tUpdAttr : tUpd.m_dAttributes )
	{
		const CSphString & sUpdAttrName = tUpdAttr.m_sName;
		int iUpdAttrId = tSchema.GetAttrIndex ( sUpdAttrName.cstr() );

		// try to find JSON attribute with a field
		if ( iUpdAttrId<0 )
		{
			CSphString sJsonCol;
			if ( sphJsonNameSplit ( sUpdAttrName.cstr(), &sJsonCol ) )
				iUpdAttrId = tSchema.GetAttrIndex ( sJsonCol.cstr() );
		}

		if ( iUpdAttrId<0 )
		{
			if ( tUpd.m_bIgnoreNonexistent )
				continue;

			sError.SetSprintf ( "attribute '%s' not found", sUpdAttrName.cstr() );
			return false;
		}

		// forbid updates on non-int columns
		const CSphColumnInfo & tCol = tSchema.GetAttr ( iUpdAttrId );
		if ( !( tCol.m_eAttrType==SPH_ATTR_BOOL || tCol.m_eAttrType==SPH_ATTR_INTEGER || tCol.m_eAttrType==SPH_ATTR_TIMESTAMP
			|| tCol.m_eAttrType==SPH_ATTR_UINT32SET || tCol.m_eAttrType==SPH_ATTR_INT64SET || tCol.m_eAttrType==SPH_ATTR_STRING
			|| tCol.m_eAttrType==SPH_ATTR_BIGINT || tCol.m_eAttrType==SPH_ATTR_FLOAT || tCol.m_eAttrType==SPH_ATTR_JSON ))
		{
			sError.SetSprintf ( "attribute '%s' can not be updated (must be boolean, integer, bigint, float, timestamp, string, MVA or JSON)", sUpdAttrName.cstr() );
			return false;
		}

		bool bSrcMva = ( tCol.m_eAttrType==SPH_ATTR_UINT32SET || tCol.m_eAttrType==SPH_ATTR_INT64SET );
		bool bDstMva = ( tUpdAttr.m_eType==SPH_ATTR_UINT32SET || tUpdAttr.m_eType==SPH_ATTR_INT64SET );
		if ( bSrcMva!=bDstMva )
		{
			sError.SetSprintf ( "attribute '%s' MVA flag mismatch", sUpdAttrName.cstr() );
			return false;
		}

		if( tCol.m_eAttrType==SPH_ATTR_UINT32SET && tUpdAttr.m_eType==SPH_ATTR_INT64SET )
		{
			sError.SetSprintf ( "attribute '%s' MVA bits (dst=%d, src=%d) mismatch", sUpdAttrName.cstr(), tCol.m_eAttrType, tUpdAttr.m_eType );
			return false;
		}

		if ( tCol.IsColumnar() )
		{
			sError.SetSprintf ( "unable to update columnar attribute '%s'", sUpdAttrName.cstr() );
			return false;
		}

	}

	return true;
}


void IndexUpdateHelper_c::Update_PrepareListOfUpdatedAttributes ( UpdateContext_t & tCtx, CSphString & sError )
{
	const auto & tUpd = *tCtx.m_tUpd.m_pUpdate;
	ARRAY_FOREACH ( i, tUpd.m_dAttributes )
	{
		const CSphString & sUpdAttrName = tUpd.m_dAttributes[i].m_sName;
		ESphAttr eUpdAttrType = tUpd.m_dAttributes[i].m_eType;
		UpdatedAttribute_t & tUpdAttr = tCtx.m_dUpdatedAttrs[i];

		int iUpdAttrId = tCtx.m_tSchema.GetAttrIndex ( sUpdAttrName.cstr() );

		if ( iUpdAttrId<0 )
		{
			CSphString sJsonCol;
			if ( sphJsonNameSplit ( sUpdAttrName.cstr(), &sJsonCol ) )
			{
				iUpdAttrId = tCtx.m_tSchema.GetAttrIndex ( sJsonCol.cstr() );
				if ( iUpdAttrId>=0 )
				{
					ExprParseArgs_t tExprArgs;
					tUpdAttr.m_pExpr = sphExprParse ( sUpdAttrName.cstr(), tCtx.m_tSchema, sError, tExprArgs );
				}
			}
		}

		if ( iUpdAttrId>=0 )
		{
			const CSphColumnInfo & tCol = tCtx.m_tSchema.GetAttr(iUpdAttrId);

			switch ( tCol.m_eAttrType )
			{
			case SPH_ATTR_FLOAT:
				if ( eUpdAttrType==SPH_ATTR_BIGINT )
					tUpdAttr.m_eConversion = CONVERSION_BIGINT2FLOAT;
				break;

			case SPH_ATTR_BIGINT:
				if ( eUpdAttrType==SPH_ATTR_FLOAT )
					tUpdAttr.m_eConversion = CONVERSION_FLOAT2BIGINT;
				break;

			default:
				break;
			}

			tUpdAttr.m_eAttrType = tCol.m_eAttrType;
			tUpdAttr.m_tLocator = tCol.m_tLocator;
			tUpdAttr.m_pHistogram = tCtx.m_pHistograms ? tCtx.m_pHistograms->Get(tCol.m_sName) : nullptr;
			tUpdAttr.m_bExisting = true;
			tUpdAttr.m_iSchemaAttr = iUpdAttrId;

			tCtx.m_dSchemaUpdateMask.BitSet(iUpdAttrId);
			tCtx.m_bBlobUpdate |= sphIsBlobAttr(tCol);
		}
		else
		{
			assert ( tUpd.m_bIgnoreNonexistent ); // should be handled by Update_CheckAttributes
			continue;
		}

		// this is a hack
		// Query parser tries to detect an attribute type. And this is wrong because, we should
		// take attribute type from schema. Probably we'll rewrite updates in future but
		// for now this fix just works.
		// Fixes cases like UPDATE float_attr=1 WHERE id=1;
		assert ( iUpdAttrId>=0 );
		if ( eUpdAttrType==SPH_ATTR_INTEGER && tCtx.m_tSchema.GetAttr(iUpdAttrId).m_eAttrType==SPH_ATTR_FLOAT )
		{
			const_cast<CSphAttrUpdate &>(tUpd).m_dAttributes[i].m_eType = SPH_ATTR_FLOAT;
			const_cast<CSphAttrUpdate &>(tUpd).m_dPool[i] = sphF2DW ( (float)tUpd.m_dPool[i] );
		}
	}
}


static void IncUpdatePoolPos ( UpdateContext_t & tCtx, int iAttr, int & iPos )
{
	switch ( tCtx.m_tUpd.m_pUpdate->m_dAttributes[iAttr].m_eType )
	{
	case SPH_ATTR_UINT32SET:
	case SPH_ATTR_INT64SET:
		iPos += tCtx.m_tUpd.m_pUpdate->m_dPool[iPos] + 1;
		break;

	case SPH_ATTR_STRING:
	case SPH_ATTR_BIGINT:
		iPos += 2;
		break;

	default:
		iPos += 1;
		break;
	}
}


static bool FitsInplaceJsonUpdate ( const UpdateContext_t & tCtx, int iAttr )
{
	// only json fields and no strings (strings go as full json updates)
	return tCtx.m_dUpdatedAttrs[iAttr].m_eAttrType==SPH_ATTR_JSON && tCtx.m_tUpd.m_pUpdate->m_dAttributes[iAttr].m_eType!=SPH_ATTR_STRING;
}


bool IndexUpdateHelper_c::Update_InplaceJson ( const RowsToUpdate_t& dRows, UpdateContext_t & tCtx, CSphString & sError, bool bDryRun )
{
	const auto& tUpd = *tCtx.m_tUpd.m_pUpdate;
	for ( const auto & tRow : dRows )
	{
		int iUpd = tRow.m_iIdx;
		int iPos = tUpd.GetRowOffset ( iUpd );
		ARRAY_CONSTFOREACH ( i, tUpd.m_dAttributes )
		{
			if ( !FitsInplaceJsonUpdate ( tCtx, i ) || !tCtx.m_dUpdatedAttrs[i].m_bExisting )
			{
				IncUpdatePoolPos ( tCtx, i, iPos );
				continue;
			}

			ESphAttr eAttr = tUpd.m_dAttributes[i].m_eType;

			bool bBigint = eAttr==SPH_ATTR_BIGINT;
			bool bDouble = eAttr==SPH_ATTR_FLOAT;

			ESphJsonType eType = bDouble ? JSON_DOUBLE : ( bBigint ? JSON_INT64 : JSON_INT32 );
			SphAttr_t uValue = bDouble
					? sphD2QW ( (double)sphDW2F ( tUpd.m_dPool[iPos] ) )
					: ( bBigint ? MVA_UPSIZE ( &tUpd.m_dPool[iPos] ) : tUpd.m_dPool[iPos] );

			if ( sphJsonInplaceUpdate ( eType, uValue, tCtx.m_dUpdatedAttrs[i].m_pExpr, tCtx.m_pBlobPool, tRow.m_pRow, !bDryRun ) )
			{
				assert ( tCtx.m_dUpdatedAttrs[i].m_iSchemaAttr>=0 );
				tCtx.m_tUpd.MarkUpdated ( iUpd );
				tCtx.m_uUpdateMask |= ATTRS_BLOB_UPDATED;
				// reset update bit to copy partial updated JSON into new blob
				tCtx.m_dSchemaUpdateMask.BitClear ( tCtx.m_dUpdatedAttrs[i].m_iSchemaAttr );
			} else
			{
				if ( bDryRun )
				{
					sError.SetSprintf ( "attribute '%s' can not be updated (not found or incompatible types)", tUpd.m_dAttributes[i].m_sName.cstr() );
					return false;
				} else
					++tCtx.m_iJsonWarnings;
			}

			IncUpdatePoolPos ( tCtx, i, iPos );
		}
	}

	return true;
}


bool IndexUpdateHelper_c::Update_Blobs ( const RowsToUpdate_t& dRows, UpdateContext_t & tCtx, bool & bCritical, CSphString & sError )
{
	const auto & tUpd = *tCtx.m_tUpd.m_pUpdate;

	// any blobs supplied in the update?
	if ( !tCtx.m_bBlobUpdate )
		return true;

	//	create a remap from attribute id in UPDATE to blob attr id
	CSphVector<int> dRemap ( tUpd.m_dAttributes.GetLength() );
	dRemap.Fill(-1);

	CSphVector<int> dBlobAttrIds;

	bool bNeedBlobBuilder = false;
	int iBlobAttrId = 0;
	for ( int i = 0, iAttrs = tCtx.m_tSchema.GetAttrsCount (); i < iAttrs; ++i )
	{
		const CSphColumnInfo & tAttr = tCtx.m_tSchema.GetAttr(i);
		if ( sphIsBlobAttr(tAttr) )
		{
			dBlobAttrIds.Add(i);
			ARRAY_CONSTFOREACH ( iUpd, tUpd.m_dAttributes )
			{
				const TypedAttribute_t & tTypedAttr = tUpd.m_dAttributes[iUpd];

				if ( sphIsBlobAttr ( tTypedAttr.m_eType ) && tAttr.m_sName==tTypedAttr.m_sName )
				{
					dRemap[iUpd] = iBlobAttrId;
					bNeedBlobBuilder = true;
				}
			}

			++iBlobAttrId;
		}
	}

	if ( !bNeedBlobBuilder )
		return true;

	CSphTightVector<BYTE> tBlobPool;
	CSphScopedPtr<BlobRowBuilder_i> pBlobRowBuilder ( sphCreateBlobRowBuilderUpdate ( tCtx.m_tSchema, tBlobPool, tCtx.m_dSchemaUpdateMask ) );

	const CSphColumnInfo * pBlobLocator = tCtx.m_tSchema.GetAttr ( sphGetBlobLocatorName() );

	for ( const auto & tRow : dRows )
	{
		int iUpd = tRow.m_iIdx;
		tBlobPool.Resize(0);
		ARRAY_CONSTFOREACH ( iBlobId, dBlobAttrIds )
		{
			int iCol = dBlobAttrIds[iBlobId];
			if ( tCtx.m_dSchemaUpdateMask.BitGet(iCol) )
				continue;

			const CSphColumnInfo & tAttr = tCtx.m_tSchema.GetAttr(iCol);
			int iLengthBytes = 0;
			const BYTE * pData = sphGetBlobAttr ( tRow.m_pRow, tAttr.m_tLocator, tCtx.m_pBlobPool, iLengthBytes );
			if ( !pBlobRowBuilder->SetAttr ( iBlobId, pData, iLengthBytes, sError ) )
				return false;
		}

		int iPos = tUpd.GetRowOffset ( iUpd );
		ARRAY_CONSTFOREACH ( iCol, tUpd.m_dAttributes )
		{
			ESphAttr eAttr = tUpd.m_dAttributes[iCol].m_eType;

			if ( !sphIsBlobAttr(eAttr) || FitsInplaceJsonUpdate ( tCtx, iCol ) || !tCtx.m_dUpdatedAttrs[iCol].m_bExisting )
			{
				IncUpdatePoolPos ( tCtx, iCol, iPos );
				continue;
			}

			int iBlobId = dRemap[iCol];

			switch ( eAttr )
			{
			case SPH_ATTR_UINT32SET:
			case SPH_ATTR_INT64SET:
				{
					DWORD uLength = tUpd.m_dPool[iPos++];
					if ( iBlobId!=-1 )
					{
						pBlobRowBuilder->SetAttr ( iBlobId, (const BYTE *)(tUpd.m_dPool.Begin()+iPos), uLength*sizeof(DWORD), sError );
						tCtx.m_tUpd.MarkUpdated ( iUpd );
						tCtx.m_uUpdateMask |= ATTRS_BLOB_UPDATED;
					}

					iPos += uLength;
				}
				break;

			case SPH_ATTR_STRING:
				{
					DWORD uOffset = tUpd.m_dPool[iPos++];
					DWORD uLength = tUpd.m_dPool[iPos++];
					if ( iBlobId!=-1 )
					{

						pBlobRowBuilder->SetAttr ( iBlobId, &tUpd.m_dBlobs[uOffset], uLength, sError );
						tCtx.m_tUpd.MarkUpdated ( iUpd );
						tCtx.m_uUpdateMask |= ATTRS_BLOB_UPDATED;
					}
				}
				break;

			default:
				IncUpdatePoolPos ( tCtx, iCol, iPos );
				break;
			}
		}

		pBlobRowBuilder->Flush();

		assert(pBlobLocator);
		if ( !Update_WriteBlobRow ( tCtx, const_cast<CSphRowitem*> (tRow.m_pRow), tBlobPool.Begin(), tBlobPool.GetLength(), iBlobAttrId, pBlobLocator->m_tLocator, bCritical, sError ) )
			return false;
	}

	return true;
}


bool IndexUpdateHelper_c::Update_HandleJsonWarnings ( UpdateContext_t & tCtx, int iUpdated, CSphString & sWarning, CSphString & sError )
{
	if ( !tCtx.m_iJsonWarnings )
		return true;

	sWarning.SetSprintf ( "%d attribute(s) can not be updated (not found or incompatible types)", tCtx.m_iJsonWarnings );
	if ( !iUpdated )
		sError = sWarning;

	return !!iUpdated;
}


void IndexUpdateHelper_c::Update_Plain ( const RowsToUpdate_t& dRows, UpdateContext_t & tCtx )
{
	const auto & tUpd = *tCtx.m_tUpd.m_pUpdate;

	for ( const auto & tRow : dRows )
	{
		int iPos = tUpd.GetRowOffset ( tRow.m_iIdx );
		ARRAY_CONSTFOREACH ( iCol, tUpd.m_dAttributes )
		{
			ESphAttr eAttr = tUpd.m_dAttributes[iCol].m_eType;
			const UpdatedAttribute_t & tUpdAttr = tCtx.m_dUpdatedAttrs[iCol];

			// already updated?
			if ( sphIsBlobAttr(eAttr) || tUpdAttr.m_eAttrType==SPH_ATTR_JSON || !tUpdAttr.m_bExisting )
			{
				IncUpdatePoolPos ( tCtx, iCol, iPos );
				continue;
			}

			const CSphAttrLocator & tLoc = tUpdAttr.m_tLocator;

			bool bBigint = eAttr==SPH_ATTR_BIGINT;
			SphAttr_t uValue = bBigint ? MVA_UPSIZE ( &tUpd.m_dPool[iPos] ) : tUpd.m_dPool[iPos];

			if ( tUpdAttr.m_eConversion==CONVERSION_BIGINT2FLOAT ) // handle bigint(-1) -> float attr updates
				uValue = sphF2DW ( float((int64_t)uValue) );
			else if ( tUpdAttr.m_eConversion==CONVERSION_FLOAT2BIGINT ) // handle float(1.0) -> bigint attr updates
				uValue = (int64_t)sphDW2F((DWORD)uValue);

			Histogram_i * pHistogram = tUpdAttr.m_pHistogram;
			if ( pHistogram )
			{
				SphAttr_t tOldValue = sphGetRowAttr ( tRow.m_pRow, tLoc );
				pHistogram->Delete ( tOldValue );
				pHistogram->UpdateCounter ( uValue );
			}

			sphSetRowAttr ( const_cast<CSphRowitem*> ( tRow.m_pRow ), tLoc, uValue );
			tCtx.m_tUpd.MarkUpdated ( tRow.m_iIdx );
			tCtx.m_uUpdateMask |= ATTRS_UPDATED;

			// next
			IncUpdatePoolPos ( tCtx, iCol, iPos );
		}
	}
}

//////////////////////////////////////////////////////////////////////////

struct FileDebugCheckReader_c final : public DebugCheckReader_i
{
	FileDebugCheckReader_c ( CSphAutoreader * pReader )
		: m_pReader ( pReader )
	{}
	~FileDebugCheckReader_c () final
	{}
	int64_t GetLengthBytes () final
	{
		return ( m_pReader ? m_pReader->GetFilesize() : 0 );
	}
	bool GetBytes ( void * pData, int iSize ) final
	{
		if ( !m_pReader )
			return false;

		m_pReader->GetBytes ( pData, iSize );
		return !m_pReader->GetErrorFlag();
	}

	bool SeekTo ( int64_t iOff, int iHint ) final
	{
		if ( !m_pReader )
			return false;

		m_pReader->SeekTo ( iOff, iHint );
		return !m_pReader->GetErrorFlag();
	}

	CSphAutoreader * m_pReader = nullptr;
};


class QueryMvaContainer_c
{
public:
	CSphVector<OpenHash_T<CSphVector<int64_t>, int64_t, HashFunc_Int64_t>*> m_tContainer;

	~QueryMvaContainer_c()
	{
		for ( auto i : m_tContainer )
			delete i;
	}
};


/// this is my actual VLN-compressed phrase index implementation
class CSphIndex_VLN : public CSphIndex, public IndexUpdateHelper_c, public IndexAlterHelper_c, public DebugCheckHelper_c
{
	friend class DiskIndexQwordSetup_c;
	friend class CSphMerger;
	friend class AttrIndexBuilder_c;
	friend struct SphFinalMatchCalc_t;
	friend class KeepAttrs_c;
	friend class AttrMerger_c;

public:
	explicit			CSphIndex_VLN ( const char * szIndexName, const char * szFilename );
						~CSphIndex_VLN() override;

	int					Build ( const CSphVector<CSphSource*> & dSources, int iMemoryLimit, int iWriteBuffer, CSphIndexProgress & tProgress ) final;

	bool				LoadHeaderLegacy ( const char * sHeaderName, bool bStripPath, CSphEmbeddedFiles & tEmbeddedFiles, FilenameBuilder_i * pFilenameBuilder, CSphString & sWarning );
	bool				LoadHeader ( const char * sHeaderName, bool bStripPath, CSphEmbeddedFiles & tEmbeddedFiles, FilenameBuilder_i * pFilenameBuilder, CSphString & sWarning );

	void				DebugDumpHeader ( FILE * fp, const char * sHeaderName, bool bConfig ) final;
	void				DebugDumpDocids ( FILE * fp ) final;
	void				DebugDumpHitlist ( FILE * fp, const char * sKeyword, bool bID ) final;
	void				DebugDumpDict ( FILE * fp ) final;
	void				SetDebugCheck ( bool bCheckIdDups, int iCheckChunk ) final;
	int					DebugCheck ( DebugCheckError_i& ) final;
	template <class Qword> void		DumpHitlist ( FILE * fp, const char * sKeyword, bool bID );

	bool				Prealloc ( bool bStripPath, FilenameBuilder_i * pFilenameBuilder, StrVec_t & dWarnings ) final;
	void				Dealloc () final;
	void				Preread () final;

	void				SetBase ( const char * sNewBase ) final;
	bool				Rename ( const char * sNewBase ) final;

	bool				Lock () final;
	void				Unlock () final;
	void				PostSetup() final {}

	bool				MultiQuery ( CSphQueryResult & pResult, const CSphQuery & tQuery, const VecTraits_T<ISphMatchSorter *> & dSorters, const CSphMultiQueryArgs & tArgs ) const final;
	bool				MultiQueryEx ( int iQueries, const CSphQuery * pQueries, CSphQueryResult* pResults, ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs & tArgs ) const final;
	bool				GetKeywords ( CSphVector <CSphKeywordInfo> & dKeywords, const char * szQuery, const GetKeywordsSettings_t & tSettings, CSphString * pError ) const final;
	template <class Qword> bool		DoGetKeywords ( CSphVector <CSphKeywordInfo> & dKeywords, const char * szQuery, const GetKeywordsSettings_t & tSettings, bool bFillOnly, CSphString * pError ) const;
	bool 				FillKeywords ( CSphVector <CSphKeywordInfo> & dKeywords ) const final;
	void				GetSuggest ( const SuggestArgs_t & tArgs, SuggestResult_t & tRes ) const final;

	bool				Merge ( CSphIndex * pSource, const VecTraits_T<CSphFilterSettings> & dFilters, bool bSupressDstDocids, CSphIndexProgress & tProgress ) final;

	template <class QWORDDST, class QWORDSRC>
	static bool			MergeWords ( const CSphIndex_VLN * pDstIndex, const CSphIndex_VLN * pSrcIndex, VecTraits_T<RowID_t> dDstRows, VecTraits_T<RowID_t> dSrcRows, CSphHitBuilder * pHitBuilder, CSphString & sError, CSphIndexProgress & tProgress);
	static bool			DoMerge ( const CSphIndex_VLN * pDstIndex, const CSphIndex_VLN * pSrcIndex, ISphFilter * pFilter, CSphString & sError, CSphIndexProgress & tProgress, bool bSrcSettings, bool bSupressDstDocids );
	ISphFilter *		CreateMergeFilters ( const VecTraits_T<CSphFilterSettings> & dSettings ) const;
	template <class QWORD>
	static bool			DeleteField ( const CSphIndex_VLN * pIndex, CSphHitBuilder * pHitBuilder, CSphString & sError, CSphSourceStats & tStat, int iKillField );

	int					UpdateAttributes ( AttrUpdateInc_t & tUpd, bool & bCritical, CSphString & sError, CSphString & sWarning ) final;
	void				UpdateAttributesOffline ( VecTraits_T<PostponedUpdate_t> & dUpdates, IndexSegment_c * /*pSeg*/) final;

	// the only txn we can replay is 'update attributes', but it is processed by dedicated branch in binlog, so we have nothing to do here.
	Binlog::CheckTnxResult_t ReplayTxn (Binlog::Blop_e, CSphReader&, CSphString & , Binlog::CheckTxn_fn&&) final { return {}; }
	bool				SaveAttributes ( CSphString & sError ) const final;
	DWORD				GetAttributeStatus () const final;

	bool				AddRemoveAttribute ( bool bAddAttr, const AttrAddRemoveCtx_t & tCtx, CSphString & sError ) final;
	bool				AddRemoveField ( bool bAdd, const CSphString & sFieldName, DWORD uFieldFlags, CSphString & sError ) final;

	void				FlushDeadRowMap ( bool bWaitComplete ) const final;
	bool				LoadKillList ( CSphFixedVector<DocID_t> * pKillList, KillListTargets_c & tTargets, CSphString & sError ) const final;
	bool				AlterKillListTarget ( KillListTargets_c & tTargets, CSphString & sError ) final;
	void				KillExistingDocids ( CSphIndex * pTarget ) final;

	bool				EarlyReject ( CSphQueryContext * pCtx, CSphMatch & tMatch ) const final;

	void				SetKeepAttrs ( const CSphString & sKeepAttrs, const StrVec_t & dAttrs ) final { m_sKeepAttrs = sKeepAttrs; m_dKeepAttrs = dAttrs; }
	RowID_t				GetRowidByDocid ( DocID_t tDocID ) const;
	int					Kill ( DocID_t tDocID ) final;
	int					KillMulti ( const VecTraits_T<DocID_t> & dKlist ) final;
	bool				IsAlive ( DocID_t tDocID ) const final;

	const CSphSourceStats &		GetStats () const final { return m_tStats; }
	int64_t *			GetFieldLens() const final { return m_tSettings.m_bIndexFieldLens ? m_dFieldLens.begin() : nullptr; }
	void				GetStatus ( CSphIndexStatus* ) const final;

	bool				PreallocWordlist();
	bool				PreallocAttributes();
	bool				PreallocDocidLookup();
	bool				PreallocKilllist();
	bool				PreallocHistograms ( StrVec_t & dWarnings );
	bool				PreallocDocstore();
	bool				PreallocColumnar();
	bool				PreallocSkiplist();

	CSphVector<SphAttr_t> 	BuildDocList () const final;

	// docstore-related section
	void				CreateReader ( int64_t iSessionId ) const final;
	bool				GetDoc ( DocstoreDoc_t & tDoc, DocID_t tDocID, const VecTraits_T<int> * pFieldIds, int64_t iSessionId, bool bPack ) const final;
	int					GetFieldId ( const CSphString & sName, DocstoreDataType_e eType ) const final;
	Bson_t				ExplainQuery ( const CSphString & sQuery ) const final;

	bool				CopyExternalFiles ( int iPostfix, StrVec_t & dCopied ) final;
	void				CollectFiles ( StrVec_t & dFiles, StrVec_t & dExt ) const final;

	bool				CheckEarlyReject ( const CSphVector<CSphFilterSettings> & dFilters, ISphFilter * pFilter, ESphCollation eCollation, const ISphSchema & tSchema ) const;

private:
	static const int			MIN_WRITE_BUFFER		= 262144;	///< min write buffer size
	static const int			DEFAULT_WRITE_BUFFER	= 1048576;	///< default write buffer size

private:
	// common stuff
	int								m_iLockFD;
	CSphSourceStats					m_tStats;			///< my stats
	CSphFixedVector<int64_t>		m_dFieldLens;	///< total per-field lengths summed over entire indexed data, in tokens
	CSphString						m_sKeepAttrs;			///< retain attributes of that index reindexing
	StrVec_t						m_dKeepAttrs;

private:
	int64_t						m_iDocinfo;				///< my docinfo cache size
	int64_t						m_iDocinfoIndex;		///< docinfo "index" entries count (each entry is 2x docinfo rows, for min/max)
	DWORD *						m_pDocinfoIndex;		///< docinfo "index", to accelerate filtering during full-scan (2x rows for each block, and 2x rows for the whole index, 1+m_uDocinfoIndex entries)
	int64_t						m_iMinMaxIndex;			///< stored min/max cache offset (counted in DWORDs)

	// !COMMIT slow setup data
	CSphMappedBuffer<DWORD>		m_tAttr;
	CSphMappedBuffer<BYTE>		m_tBlobAttrs;
	CSphMappedBuffer<BYTE>		m_tSkiplists;		///< (compressed) skiplists data
	CWordlist					m_tWordlist;		///< my wordlist

	DeadRowMap_Disk_c 			m_tDeadRowMap;

	CSphMappedBuffer<BYTE>		m_tDocidLookup;		///< speeds up docid-rowid lookups + used for applying killlist on startup
	LookupReader_c				m_tLookupReader;	///< used by getrowidbydocid

	CSphScopedPtr<Docstore_i> 	m_pDocstore {nullptr};
	CSphScopedPtr<columnar::Columnar_i> m_pColumnar {nullptr};

	DWORD						m_uVersion;				///< data files version
	volatile bool				m_bPassedRead;
	volatile bool				m_bPassedAlloc;
	bool						m_bIsEmpty;				///< do we have actually indexed documents (m_iTotalDocuments is just fetched documents, not indexed!)
	bool						m_bDebugCheck;
	bool						m_bCheckIdDups = false;

	mutable DWORD				m_uAttrsStatus = 0;

	DataReaderFactoryPtr_c		m_pDoclistFile;			///< doclist file
	DataReaderFactoryPtr_c		m_pHitlistFile;			///< hitlist file
	DataReaderFactoryPtr_c		m_pColumnarFile;			///< columnar file

	HistogramContainer_c *		m_pHistograms {nullptr};

private:
	CSphString					GetIndexFileName ( ESphExt eExt, bool bTemp=false ) const;
	CSphString					GetIndexFileName ( const char * szExt ) const;
	void						GetIndexFiles ( CSphVector<CSphString> & dFiles, const FilenameBuilder_i * pFilenameBuilder ) const override;

	bool						ParsedMultiQuery ( const CSphQuery & tQuery, CSphQueryResult & tResult, const VecTraits_T<ISphMatchSorter*> & dSorters, const XQQuery_t & tXQ, CSphDict * pDict, const CSphMultiQueryArgs & tArgs, CSphQueryNodeCache * pNodeCache, int64_t tmMaxTimer ) const;

	template <bool ROWID_LIMITS>
	void						ScanByBlocks ( const CSphQueryContext & tCtx, CSphQueryResultMeta & tMeta, const VecTraits_T<ISphMatchSorter *> & dSorters, CSphMatch & tMatch, int iCutoff, bool bRandomize, int iIndexWeight, int64_t tmMaxTimer, const RowIdBoundaries_t * pBoundaries = nullptr ) const;
	void						RunFullscanOnAttrs ( const RowIdBoundaries_t & tBoundaries, const CSphQueryContext & tCtx, CSphQueryResultMeta & tMeta, const VecTraits_T<ISphMatchSorter *> & dSorters, CSphMatch & tMatch, int iCutoff, bool bRandomize, int iIndexWeight, int64_t tmMaxTimer, bool & bStop ) const;
	void						RunFullscanOnIterator ( RowidIterator_i * pIterator, const CSphQueryContext & tCtx, CSphQueryResultMeta & tMeta, const VecTraits_T<ISphMatchSorter *> & dSorters, CSphMatch & tMatch, int iCutoff, bool bRandomize, int iIndexWeight, int64_t tmMaxTimer ) const;
	bool						MultiScan ( CSphQueryResult & tResult, const CSphQuery & dQuery, const VecTraits_T<ISphMatchSorter *> & dSorters, const CSphMultiQueryArgs & tArgs, int64_t tmMaxTimer ) const;

	template<bool USE_KLIST, bool RANDOMIZE, bool USE_FACTORS>
	void						MatchExtended ( CSphQueryContext & tCtx, const CSphQuery & tQuery, const VecTraits_T<ISphMatchSorter *>& dSorters, ISphRanker * pRanker, int iTag, int iIndexWeight ) const;

	const CSphRowitem *			FindDocinfo ( DocID_t tDocID ) const;

	const DWORD *				GetDocinfoByRowID ( RowID_t tRowID ) const;
	RowID_t						GetRowIDByDocinfo ( const CSphRowitem * pDocinfo ) const;

	void						SetupStarDict ( DictRefPtr_c &pDict ) const;
	void						SetupExactDict ( DictRefPtr_c &pDict ) const;

	bool						RelocateBlock ( int iFile, BYTE * pBuffer, int iRelocationSize, SphOffset_t * pFileSize, CSphBin & dMinBin, SphOffset_t * pSharedOffset );

	bool						SortDocidLookup ( int iFD, int nBlocks, int iMemoryLimit, int nLookupsInBlock, int nLookupsInLastBlock, CSphIndexProgress& tProgress );

	bool						CollectQueryMvas ( const CSphVector<CSphSource*> & dSources, QueryMvaContainer_c & tMvaContainer );

private:
	bool						JuggleFile ( ESphExt eExt, CSphString & sError, bool bNeedSrc=true, bool bNeedDst=true ) const;
	XQNode_t *					ExpandPrefix ( XQNode_t * pNode, CSphQueryResultMeta & tMeta, CSphScopedPayload * pPayloads, DWORD uQueryDebugFlags ) const;

	static std::pair<DWORD,DWORD>		CreateRowMapsAndCountTotalDocs ( const CSphIndex_VLN* pSrcIndex, const CSphIndex_VLN* pDstIndex, CSphFixedVector<RowID_t>& dSrcRowMap, CSphFixedVector<RowID_t>& dDstRowMap, const ISphFilter* pFilter, bool bSupressDstDocids, MergeCb_c& tMonitor );
	RowsToUpdateData_t			Update_CollectRowPtrs ( const UpdateContext_t & tCtx );
	RowsToUpdate_t				Update_PrepareGatheredRowPtrs ( RowsToUpdate_t & dWRows, const VecTraits_T<DocID_t> & dDocids );
	bool						Update_WriteBlobRow ( UpdateContext_t & tCtx, CSphRowitem * pDocinfo, const BYTE * pBlob, int iLength, int nBlobAttrs, const CSphAttrLocator & tBlobRowLoc, bool & bCritical, CSphString & sError ) override;
	void						Update_MinMax ( const RowsToUpdate_t& dRows, const UpdateContext_t & tCtx );
	bool						DoUpdateAttributes ( const RowsToUpdate_t& dRows, UpdateContext_t& tCtx, bool & bCritical, CSphString & sError );

	bool						Alter_IsMinMax ( const CSphRowitem * pDocinfo, int iStride ) const override;
	bool						AddRemoveColumnarAttr ( bool bAddAttr, const CSphString & sAttrName, ESphAttr eAttrType, const ISphSchema & tOldSchema, const ISphSchema & tNewSchema, CSphString & sError );
	bool						DeleteFieldFromDict ( int iFieldId, BuildHeader_t & tBuildHeader, CSphString & sError );
	bool						AddRemoveFromDocstore ( const CSphSchema & tOldSchema, const CSphSchema & tNewSchema, CSphString & sError );

	bool						Build_SetupInplace ( SphOffset_t & iHitsGap, int iHitsMax, int iFdHits ) const;
	bool						Build_SetupDocstore ( CSphScopedPtr<DocstoreBuilder_i> & pDocstore, CSphBitvec & dStoredFields, CSphBitvec & dStoredAttrs, CSphVector<CSphVector<BYTE>> & dTmpDocstoreAttrStorage );
	bool						Build_SetupBlobBuilder ( CSphScopedPtr<BlobRowBuilder_i> & pBuilder );
	bool						Build_SetupColumnar ( CSphScopedPtr<columnar::Builder_i> & pBuilder );
	bool						Build_SetupHistograms ( CSphScopedPtr<HistogramContainer_c> & pContainer, CSphVector<std::pair<Histogram_i*,int>> & dHistograms );

	void						Build_AddToDocstore ( DocstoreBuilder_i * pDocstoreBuilder, DocID_t tDocID, QueryMvaContainer_c & tMvaContainer, CSphSource & tSource, const CSphBitvec & dStoredFields, const CSphBitvec & dStoredAttrs, CSphVector<CSphVector<BYTE>> & dTmpDocstoreAttrStorage );
	bool						Build_StoreBlobAttrs ( DocID_t tDocId, SphOffset_t & tOffset, BlobRowBuilder_i & tBlobRowBuilderconst, QueryMvaContainer_c & tMvaContainer, AttrSource_i & tSource, bool bForceSource );
	void						Build_StoreColumnarAttrs ( DocID_t tDocId, columnar::Builder_i & tBuilder, CSphSource & tSource, QueryMvaContainer_c & tMvaContainer );
	void						Build_StoreHistograms ( CSphVector<std::pair<Histogram_i*,int>> dHistograms, CSphSource & tSource );

	bool						SpawnReader ( DataReaderFactoryPtr_c & m_pFile, ESphExt eExt, DataReaderFactory_c::Kind_e eKind, int iBuffer, FileAccess_e eAccess );
	bool						SpawnReaders();

	RowidIterator_i *			CreateColumnarAnalyzerOrPrefilter ( const CSphVector<CSphFilterSettings> & dFilters, CSphVector<CSphFilterSettings> & dModifiedFilters, bool & bFiltersChanged, const CSphVector<FilterTreeItem_t> & dFilterTree, const ISphFilter * pFilter, ESphCollation eCollation, const ISphSchema & tSchema, CSphString & sWarning ) const;

	bool						SplitQuery ( CSphQueryResult & tResult, const CSphQuery & tQuery, const VecTraits_T<ISphMatchSorter *> & dAllSorters, const CSphMultiQueryArgs & tArgs, int64_t tmMaxTimer ) const;
	RowidIterator_i *			SpawnIterators ( const CSphQuery & tQuery, CSphVector<CSphFilterSettings> & dModifiedFilters, CSphQueryContext & tCtx, CreateFilterContext_t & tFlx, const ISphSchema & tMaxSorterSchema, CSphQueryResultMeta & tMeta ) const;
};

class AttrMerger_c
{
	AttrIndexBuilder_c				m_tMinMax;
	HistogramContainer_c			m_tHistograms;
	CSphVector<PlainOrColumnar_t>	m_dAttrsForHistogram;
	CSphFixedVector<DocidRowidPair_t> m_dDocidLookup {0};
	CSphWriter						m_tWriterSPA;
	BlobRowBuilder_i * 				m_pBlobRowBuilder = nullptr;
	DocstoreBuilder_i * 			m_pDocstoreBuilder = nullptr;
	columnar::Builder_i *			m_pColumnarBuilder = nullptr;
	RowID_t							m_tResultRowID = 0;
	int64_t							m_iTotalBytes = 0;

	MergeCb_c & 					m_tMonitor;
	CSphString &					m_sError;
	int64_t							m_iTotalDocs;

private:
	bool CopyPureColumnarAttributes ( const CSphIndex_VLN& tIndex, const VecTraits_T<RowID_t>& dRowMap );
	bool CopyMixedAttributes ( const CSphIndex_VLN& tIndex, const VecTraits_T<RowID_t>& dRowMap );

public:
	AttrMerger_c ( MergeCb_c & tMonitor, CSphString & sError, int64_t iTotalDocs )
		: m_tMonitor ( tMonitor )
		, m_sError ( sError)
		, m_iTotalDocs ( iTotalDocs )
	{
	}

	~AttrMerger_c()
	{
		SafeDelete ( m_pColumnarBuilder );
		SafeDelete ( m_pDocstoreBuilder );
		SafeDelete ( m_pBlobRowBuilder );
	}

	bool Prepare ( const CSphIndex_VLN* pSrcIndex, const CSphIndex_VLN* pDstIndex );
	bool CopyAttributes ( const CSphIndex_VLN& tIndex, const VecTraits_T<RowID_t>& dRowMap, DWORD uAlive );
	bool FinishMergeAttributes ( const CSphIndex_VLN* pDstIndex, BuildHeader_t& tBuildHeader );
};


/////////////////////////////////////////////////////////////////////////////
// UTILITY FUNCTIONS
/////////////////////////////////////////////////////////////////////////////

/// indexer warning
void sphWarn ( const char * sTemplate, ... )
{
	va_list ap;
	va_start ( ap, sTemplate );
	fprintf ( stdout, "WARNING: " );
	vfprintf ( stdout, sTemplate, ap );
	fprintf ( stdout, "\n" );
	va_end ( ap );
}

//////////////////////////////////////////////////////////////////////////


void sphSleepMsec ( int iMsec )
{
	if ( iMsec<0 )
		return;

#if _WIN32
	Sleep ( iMsec );

#else
	struct timeval tvTimeout;
	tvTimeout.tv_sec = iMsec / 1000; // full seconds
	tvTimeout.tv_usec = ( iMsec % 1000 ) * 1000; // remainder is msec, so *1000 for usec

	select ( 0, NULL, NULL, NULL, &tvTimeout ); // FIXME? could handle EINTR
#endif
}


void SetUnhintedBuffer ( int iReadUnhinted )
{
	if ( iReadUnhinted<=0 )
		iReadUnhinted = DEFAULT_READ_UNHINTED;
	g_iReadUnhinted = Max ( iReadUnhinted, MIN_READ_UNHINTED );
}


int GetUnhintedBuffer()
{
	return g_iReadUnhinted;
}


// returns correct size even if iBuf is 0
int GetReadBuffer ( int iBuf )
{
	if ( !iBuf )
		return DEFAULT_READ_BUFFER;
	return Max ( iBuf, MIN_READ_BUFFER );
}

bool IsMlock ( FileAccess_e eType ) { return eType==FileAccess_e::MLOCK; }
bool IsOndisk ( FileAccess_e eType ) { return eType==FileAccess_e::FILE || eType==FileAccess_e::MMAP; }

bool FileAccessSettings_t::operator== ( const FileAccessSettings_t & tOther ) const
{
	return ( m_eAttr==tOther.m_eAttr && m_eBlob==tOther.m_eBlob && m_eDoclist==tOther.m_eDoclist && m_eHitlist==tOther.m_eHitlist &&
		m_iReadBufferDocList==tOther.m_iReadBufferDocList && m_iReadBufferHitList==tOther.m_iReadBufferHitList );
}

bool FileAccessSettings_t::operator!= ( const FileAccessSettings_t & tOther ) const
{
	return !operator==( tOther );
}


//////////////////////////////////////////////////////////////////////////

RowTagged_t::RowTagged_t ( const CSphMatch & tMatch )
{
	m_tID = tMatch.m_tRowID;
	m_iTag = tMatch.m_iTag;
}

RowTagged_t::RowTagged_t ( RowID_t tRowID, int iTag )
{
	m_tID = tRowID;
	m_iTag = iTag;
}

bool RowTagged_t::operator== ( const RowTagged_t & tRow ) const
{
	return ( m_tID==tRow.m_tID && m_iTag==tRow.m_iTag );
}

bool RowTagged_t::operator!= ( const RowTagged_t & tRow ) const
{
	return !( *this==tRow );
}


/////////////////////////////////////////////////////////////////////////////
void CSphEmbeddedFiles::Reset()
{
	m_dSynonyms.Reset();
	m_dStopwordFiles.Reset();
	m_dStopwords.Reset();
	m_dWordforms.Reset();
	m_dWordformFiles.Reset();
}


/////////////////////////////////////////////////////////////////////////////
// FILTER
/////////////////////////////////////////////////////////////////////////////
void CSphFilterSettings::SetExternalValues ( const SphAttr_t * pValues, int nValues )
{
	m_pValues = pValues;
	m_nValues = nValues;
}


bool CSphFilterSettings::operator == ( const CSphFilterSettings & rhs ) const
{
	// check name, mode, type
	if ( m_sAttrName!=rhs.m_sAttrName || m_bExclude!=rhs.m_bExclude || m_eType!=rhs.m_eType )
		return false;

	switch ( m_eType )
	{
		case SPH_FILTER_RANGE:
			return m_iMinValue==rhs.m_iMinValue && m_iMaxValue==rhs.m_iMaxValue;

		case SPH_FILTER_FLOATRANGE:
			return m_fMinValue==rhs.m_fMinValue && m_fMaxValue==rhs.m_fMaxValue;

		case SPH_FILTER_VALUES:
			if ( m_dValues.GetLength()!=rhs.m_dValues.GetLength() )
				return false;

			ARRAY_FOREACH ( i, m_dValues )
				if ( m_dValues[i]!=rhs.m_dValues[i] )
					return false;

			return true;

		case SPH_FILTER_STRING:
		case SPH_FILTER_USERVAR:
		case SPH_FILTER_STRING_LIST:
			if ( m_dStrings.GetLength ()!=rhs.m_dStrings.GetLength () )
				return false;
			ARRAY_FOREACH ( i, m_dStrings )
				if ( m_dStrings[i]!=rhs.m_dStrings[i] )
					return false;
			return ( m_eMvaFunc==rhs.m_eMvaFunc );

		default:
			assert ( 0 && "internal error: unhandled filter type in comparison" );
			return false;
	}
}


uint64_t CSphFilterSettings::GetHash() const
{
	uint64_t h = sphFNV64 ( &m_eType, sizeof(m_eType) );
	h = sphFNV64 ( &m_bExclude, sizeof(m_bExclude), h );
	switch ( m_eType )
	{
		case SPH_FILTER_VALUES:
			{
				int t = m_dValues.GetLength();
				h = sphFNV64 ( &t, sizeof(t), h );
				h = sphFNV64 ( m_dValues.Begin(), t*sizeof(SphAttr_t), h );
				break;
			}
		case SPH_FILTER_RANGE:
			h = sphFNV64 ( &m_iMaxValue, sizeof(m_iMaxValue), sphFNV64 ( &m_iMinValue, sizeof(m_iMinValue), h ) );
			break;
		case SPH_FILTER_FLOATRANGE:
			h = sphFNV64 ( &m_fMaxValue, sizeof(m_fMaxValue), sphFNV64 ( &m_fMinValue, sizeof(m_fMinValue), h ) );
			break;
		case SPH_FILTER_STRING:
		case SPH_FILTER_USERVAR:
		case SPH_FILTER_STRING_LIST:
			ARRAY_FOREACH ( iString, m_dStrings )
				h = sphFNV64cont ( m_dStrings[iString].cstr(), h );
			if ( m_eMvaFunc!=SPH_MVAFUNC_NONE )
				h = sphFNV64 ( &m_eMvaFunc, sizeof ( m_eMvaFunc ), h );
		break;
		case SPH_FILTER_NULL:
			break;
		default:
			assert ( 0 && "internal error: unhandled filter type in GetHash()" );
	}
	return h;
}


bool FilterTreeItem_t::operator == ( const FilterTreeItem_t & rhs ) const
{
	return ( m_iLeft==rhs.m_iLeft && m_iRight==rhs.m_iRight && m_iFilterItem==rhs.m_iFilterItem && m_bOr==rhs.m_bOr );
}


uint64_t FilterTreeItem_t::GetHash() const
{
	uint64_t uHash = sphFNV64 ( &m_iLeft, sizeof(m_iLeft) );
	uHash = sphFNV64 ( &m_iRight, sizeof(m_iRight), uHash );
	uHash = sphFNV64 ( &m_iFilterItem, sizeof(m_iFilterItem), uHash );
	uHash = sphFNV64 ( &m_bOr, sizeof(m_bOr), uHash );
	return uHash;
}


/////////////////////////////////////////////////////////////////////////////
// QUERY
/////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////

struct SelectBounds_t
{
	int		m_iStart;
	int		m_iEnd;
};
#define YYSTYPE SelectBounds_t
class SelectParser_t;

#include "bissphinxselect.h"

class SelectParser_t
{
public:
	int				GetToken ( YYSTYPE * lvalp );
	void			AddItem ( YYSTYPE * pExpr, ESphAggrFunc eAggrFunc=SPH_AGGR_NONE, YYSTYPE * pStart=NULL, YYSTYPE * pEnd=NULL );
	void			AddItem ( const char * pToken, YYSTYPE * pStart=NULL, YYSTYPE * pEnd=NULL );
	void			AliasLastItem ( YYSTYPE * pAlias );
	void			AddOption ( YYSTYPE * pOpt, YYSTYPE * pVal );

private:
	void			AutoAlias ( CSphQueryItem & tItem, YYSTYPE * pStart, YYSTYPE * pEnd );
	bool			IsTokenEqual ( YYSTYPE * pTok, const char * sRef );

public:
	CSphString		m_sParserError;
	const char *	m_pLastTokenStart;

	const char *	m_pStart;
	const char *	m_pCur;

	CSphQuery *		m_pQuery;
};

static int yylex ( YYSTYPE * lvalp, SelectParser_t * pParser )
{
	return pParser->GetToken ( lvalp );
}

static void yyerror ( SelectParser_t * pParser, const char * sMessage )
{
	pParser->m_sParserError.SetSprintf ( "%s near '%s'", sMessage, pParser->m_pLastTokenStart );
}

#include "bissphinxselect.c"

int SelectParser_t::GetToken ( YYSTYPE * lvalp )
{
	// skip whitespace, check eof
	while ( isspace ( *m_pCur ) )
		m_pCur++;
	if ( !*m_pCur )
		return 0;

	// begin working that token
	m_pLastTokenStart = m_pCur;
	lvalp->m_iStart = int ( m_pCur-m_pStart );

	// check for constant
	if ( isdigit ( *m_pCur ) )
	{
		char * pEnd = NULL;
		double VARIABLE_IS_NOT_USED fDummy = strtod ( m_pCur, &pEnd );
		m_pCur = pEnd;
		lvalp->m_iEnd = int ( m_pCur-m_pStart );
		return SEL_TOKEN;
	}

	// check for token
	if ( sphIsAttr ( m_pCur[0] ) || ( m_pCur[0]=='@' && sphIsAttr ( m_pCur[1] ) && !isdigit ( m_pCur[1] ) ) )
	{
		m_pCur++;
		while ( sphIsAttr ( *m_pCur ) ) m_pCur++;
		lvalp->m_iEnd = int ( m_pCur-m_pStart );

		#define LOC_CHECK(_str,_len,_ret) \
			if ( lvalp->m_iEnd==_len+lvalp->m_iStart && strncasecmp ( m_pStart+lvalp->m_iStart, _str, _len )==0 ) return _ret;

		LOC_CHECK ( "ID", 2, SEL_ID );
		LOC_CHECK ( "AS", 2, SEL_AS );
		LOC_CHECK ( "OR", 2, TOK_OR );
		LOC_CHECK ( "AND", 3, TOK_AND );
		LOC_CHECK ( "NOT", 3, TOK_NOT );
		LOC_CHECK ( "DIV", 3, TOK_DIV );
		LOC_CHECK ( "MOD", 3, TOK_MOD );
		LOC_CHECK ( "AVG", 3, SEL_AVG );
		LOC_CHECK ( "MIN", 3, SEL_MIN );
		LOC_CHECK ( "MAX", 3, SEL_MAX );
		LOC_CHECK ( "SUM", 3, SEL_SUM );
		LOC_CHECK ( "GROUP_CONCAT", 12, SEL_GROUP_CONCAT );
		LOC_CHECK ( "GROUPBY", 7, SEL_GROUPBY );
		LOC_CHECK ( "COUNT", 5, SEL_COUNT );
		LOC_CHECK ( "DISTINCT", 8, SEL_DISTINCT );
		LOC_CHECK ( "WEIGHT", 6, SEL_WEIGHT );
		LOC_CHECK ( "OPTION", 6, SEL_OPTION );
		LOC_CHECK ( "IS", 2, TOK_IS );
		LOC_CHECK ( "NULL", 4, TOK_NULL );
		LOC_CHECK ( "FOR", 3, TOK_FOR );
		LOC_CHECK ( "IN", 2, TOK_FUNC_IN );
		LOC_CHECK ( "RAND", 4, TOK_FUNC_RAND );

		#undef LOC_CHECK

		return SEL_TOKEN;
	}

	// check for equality checks
	lvalp->m_iEnd = 1+lvalp->m_iStart;
	switch ( *m_pCur )
	{
		case '<':
			m_pCur++;
			if ( *m_pCur=='>' ) { m_pCur++; lvalp->m_iEnd++; return TOK_NE; }
			if ( *m_pCur=='=' ) { m_pCur++; lvalp->m_iEnd++; return TOK_LTE; }
			return '<';

		case '>':
			m_pCur++;
			if ( *m_pCur=='=' ) { m_pCur++; lvalp->m_iEnd++; return TOK_GTE; }
			return '>';

		case '=':
			m_pCur++;
			if ( *m_pCur=='=' ) { m_pCur++; lvalp->m_iEnd++; }
			return TOK_EQ;

		case '\'':
		{
			const char cEnd = *m_pCur;
			for ( const char * s = m_pCur+1; *s; s++ )
			{
				if ( *s==cEnd && s-1>=m_pCur && *(s-1)!='\\' )
				{
					m_pCur = s+1;
					return TOK_CONST_STRING;
				}
			}
			return -1;
		}
	}

	// check for comment begin/end
	if ( m_pCur[0]=='/' && m_pCur[1]=='*' )
	{
		m_pCur += 2;
		lvalp->m_iEnd += 1;
		return SEL_COMMENT_OPEN;
	}
	if ( m_pCur[0]=='*' && m_pCur[1]=='/' )
	{
		m_pCur += 2;
		lvalp->m_iEnd += 1;
		return SEL_COMMENT_CLOSE;
	}

	// return char as a token
	return *m_pCur++;
}

void SelectParser_t::AutoAlias ( CSphQueryItem & tItem, YYSTYPE * pStart, YYSTYPE * pEnd )
{
	if ( pStart && pEnd )
	{
		tItem.m_sAlias.SetBinary ( m_pStart + pStart->m_iStart, pEnd->m_iEnd - pStart->m_iStart );
		sphColumnToLowercase ( const_cast<char *>( tItem.m_sAlias.cstr() ) ); // as in SqlParser_c
	} else
		tItem.m_sAlias = tItem.m_sExpr;
}

void SelectParser_t::AddItem ( YYSTYPE * pExpr, ESphAggrFunc eAggrFunc, YYSTYPE * pStart, YYSTYPE * pEnd )
{
	CSphQueryItem & tItem = m_pQuery->m_dItems.Add();
	tItem.m_sExpr.SetBinary ( m_pStart + pExpr->m_iStart, pExpr->m_iEnd - pExpr->m_iStart );
	sphColumnToLowercase ( const_cast<char *>( tItem.m_sExpr.cstr() ) );
	tItem.m_eAggrFunc = eAggrFunc;
	AutoAlias ( tItem, pStart, pEnd );
}

void SelectParser_t::AddItem ( const char * pToken, YYSTYPE * pStart, YYSTYPE * pEnd )
{
	CSphQueryItem & tItem = m_pQuery->m_dItems.Add();
	tItem.m_sExpr = pToken;
	tItem.m_eAggrFunc = SPH_AGGR_NONE;
	sphColumnToLowercase ( const_cast<char *>( tItem.m_sExpr.cstr() ) );
	AutoAlias ( tItem, pStart, pEnd );
}

void SelectParser_t::AliasLastItem ( YYSTYPE * pAlias )
{
	if ( pAlias )
	{
		CSphQueryItem & tItem = m_pQuery->m_dItems.Last();
		tItem.m_sAlias.SetBinary ( m_pStart + pAlias->m_iStart, pAlias->m_iEnd - pAlias->m_iStart );
		tItem.m_sAlias.ToLower();
	}
}

bool SelectParser_t::IsTokenEqual ( YYSTYPE * pTok, const char * sRef )
{
	auto iLen = (int) strlen(sRef);
	if ( iLen!=( pTok->m_iEnd - pTok->m_iStart ) )
		return false;
	return strncasecmp ( m_pStart + pTok->m_iStart, sRef, iLen )==0;
}

void SelectParser_t::AddOption ( YYSTYPE * pOpt, YYSTYPE * pVal )
{
	if ( IsTokenEqual ( pOpt, "sort_method" ) )
	{
		if ( IsTokenEqual ( pVal, "kbuffer" ) )
			m_pQuery->m_bSortKbuffer = true;
	} else if ( IsTokenEqual ( pOpt, "max_predicted_time" ) )
	{
		char szNumber[256];
		int iLen = pVal->m_iEnd-pVal->m_iStart;
		assert ( iLen < (int)sizeof(szNumber) );
		strncpy ( szNumber, m_pStart+pVal->m_iStart, iLen );
		int64_t iMaxPredicted = strtoull ( szNumber, NULL, 10 );
		m_pQuery->m_iMaxPredictedMsec = int(iMaxPredicted > INT_MAX ? INT_MAX : iMaxPredicted );
	}
}

bool ParseSelectList ( CSphString & sError, CSphQuery & tQuery )
{
	tQuery.m_dItems.Reset();
	if ( tQuery.m_sSelect.IsEmpty() )
		return true; // empty is ok; will just return everything

	SelectParser_t tParser;
	tParser.m_pStart = tQuery.m_sSelect.cstr();
	tParser.m_pCur = tParser.m_pStart;
	tParser.m_pQuery = &tQuery;

	yyparse ( &tParser );

	sError = tParser.m_sParserError;
	return sError.IsEmpty ();
}


int ExpandKeywords ( int iIndexOpt, QueryOption_e eQueryOpt, const CSphIndexSettings & tSettings, bool bWordDict )
{
	if ( tSettings.m_iMinInfixLen<=0 && tSettings.GetMinPrefixLen ( bWordDict )<=0 && !tSettings.m_bIndexExactWords )
		return KWE_DISABLED;

	int iOpt = KWE_DISABLED;
	if ( eQueryOpt==QUERY_OPT_DEFAULT )
		iOpt = iIndexOpt;
	else if ( eQueryOpt==QUERY_OPT_MORPH_NONE )
		iOpt = KWE_MORPH_NONE;
	 else
		iOpt = ( eQueryOpt==QUERY_OPT_ENABLED ? KWE_ENABLED : KWE_DISABLED );

	if ( ( iOpt & KWE_STAR )==KWE_STAR && tSettings.m_iMinInfixLen<=0 && tSettings.GetMinPrefixLen ( bWordDict )<=0 )
		iOpt ^= KWE_STAR;

	if ( ( iOpt & KWE_EXACT )==KWE_EXACT && !tSettings.m_bIndexExactWords )
		iOpt ^= KWE_EXACT;

	if ( ( iOpt & KWE_MORPH_NONE )==KWE_MORPH_NONE && !tSettings.m_bIndexExactWords )
		iOpt ^= KWE_MORPH_NONE;

	return iOpt;
}


/////////////////////////////////////////////////////////////////////////////
// QUERY STATS
/////////////////////////////////////////////////////////////////////////////
void CSphQueryStats::Add ( const CSphQueryStats & tStats )
{
	m_iFetchedDocs += tStats.m_iFetchedDocs;
	m_iFetchedHits += tStats.m_iFetchedHits;
	m_iSkips += tStats.m_iSkips;
}


/////////////////////////////////////////////////////////////////////////////
// SCHEMAS
/////////////////////////////////////////////////////////////////////////////

/// make string lowercase but keep case of JSON.field
void sphColumnToLowercase ( char * sVal )
{
	if ( !sVal || !*sVal )
		return;

	// make all chars lowercase but only prior to '.', ',', and '[' delimiters
	// leave quoted values unchanged
	for ( bool bQuoted=false; *sVal && *sVal!='.' && *sVal!=',' && *sVal!='['; sVal++ )
	{
		if ( !bQuoted )
			*sVal = (char) tolower ( *sVal );
		if ( *sVal=='\'' )
			bQuoted = !bQuoted;
	}
}

/////////////////////////////////////////////////////////////////////////////
// CHUNK READER
/////////////////////////////////////////////////////////////////////////////

CSphBin::CSphBin ( ESphHitless eMode, bool bWordDict )
	: m_eMode ( eMode )
	, m_bWordDict ( bWordDict )
{
	m_tHit.m_sKeyword = bWordDict ? m_sKeyword : nullptr;
	m_sKeyword[0] = '\0';

#ifndef NDEBUG
	m_sLastKeyword[0] = '\0';
#endif
}


int CSphBin::CalcBinSize ( int iMemoryLimit, int iBlocks, const char * sPhase, bool bWarn )
{
	if ( iBlocks<=0 )
		return CSphBin::MIN_SIZE;

	int iBinSize = ( ( iMemoryLimit/iBlocks + 2048 ) >> 12 ) << 12; // round to 4k

	if ( iBinSize<CSphBin::MIN_SIZE )
	{
		iBinSize = CSphBin::MIN_SIZE;
		sphWarn ( "%s: mem_limit=%d kb extremely low, increasing to %d kb",
			sPhase, iMemoryLimit/1024, iBinSize*iBlocks/1024 );
	}

	if ( iBinSize<CSphBin::WARN_SIZE && bWarn )
	{
		sphWarn ( "%s: merge_block_size=%d kb too low, increasing mem_limit may improve performance",
			sPhase, iBinSize/1024 );
	}

	return iBinSize;
}


void CSphBin::Init ( int iFD, SphOffset_t * pSharedOffset, const int iBinSize )
{
	assert ( !m_dBuffer );
	assert ( iBinSize>=MIN_SIZE );
	assert ( pSharedOffset );

	m_iFile = iFD;
	m_pFilePos = pSharedOffset;

	m_iSize = iBinSize;
	m_dBuffer = new BYTE [ iBinSize ];
	m_pCurrent = m_dBuffer;

	m_tHit.m_tRowID = INVALID_ROWID;
	m_tHit.m_uWordID = 0;
	m_tHit.m_iWordPos = EMPTY_HIT;
	m_tHit.m_dFieldMask.UnsetAll();

	m_bError = false;
}


CSphBin::~CSphBin ()
{
	SafeDeleteArray ( m_dBuffer );
}


int CSphBin::ReadByte ()
{
	BYTE r;

	if ( !m_iLeft )
	{
		if ( *m_pFilePos!=m_iFilePos )
		{
			if ( !SeekAndWarn ( m_iFile, m_iFilePos, "CSphBin::ReadBytes" ) )
			{
				m_bError = true;
				return BIN_READ_ERROR;
			}
			*m_pFilePos = m_iFilePos;
		}

		int n = m_iFileLeft > m_iSize
			? m_iSize
			: (int)m_iFileLeft;
		if ( n==0 )
		{
			m_iDone = 1;
			m_iLeft = 1;
		} else
		{
			assert ( m_dBuffer );

			if ( sphReadThrottled ( m_iFile, m_dBuffer, n )!=(size_t)n )
			{
				m_bError = true;
				return -2;
			}
			m_iLeft = n;

			m_iFilePos += n;
			m_iFileLeft -= n;
			m_pCurrent = m_dBuffer;
			*m_pFilePos += n;
		}
	}
	if ( m_iDone )
	{
		m_bError = true; // unexpected (!) eof
		return -1;
	}

	m_iLeft--;
	r = *(m_pCurrent);
	m_pCurrent++;
	return r;
}


ESphBinRead CSphBin::ReadBytes ( void * pDest, int iBytes )
{
	assert ( iBytes>0 );
	assert ( iBytes<=m_iSize );

	if ( m_iDone )
		return BIN_READ_EOF;

	if ( m_iLeft<iBytes )
	{
		if ( *m_pFilePos!=m_iFilePos )
		{
			if ( !SeekAndWarn ( m_iFile, m_iFilePos, "CSphBin::ReadBytes" ))
			{
				m_bError = true;
				return BIN_READ_ERROR;
			}
			*m_pFilePos = m_iFilePos;
		}

		int n = Min ( m_iFileLeft, m_iSize - m_iLeft );
		if ( n==0 )
		{
			m_iDone = 1;
			m_bError = true; // unexpected (!) eof
			return BIN_READ_EOF;
		}

		assert ( m_dBuffer );
		memmove ( m_dBuffer, m_pCurrent, m_iLeft );

		if ( sphReadThrottled ( m_iFile, m_dBuffer + m_iLeft, n )!=(size_t)n )
		{
			m_bError = true;
			return BIN_READ_ERROR;
		}

		m_iLeft += n;
		m_iFilePos += n;
		m_iFileLeft -= n;
		m_pCurrent = m_dBuffer;
		*m_pFilePos += n;
	}

	assert ( m_iLeft>=iBytes );
	m_iLeft -= iBytes;

	memcpy ( pDest, m_pCurrent, iBytes );
	m_pCurrent += iBytes;

	return BIN_READ_OK;
}


SphWordID_t CSphBin::ReadVLB ()
{
	SphWordID_t uValue = 0;
	int iByte, iOffset = 0;
	do
	{
		if ( ( iByte = ReadByte() )<0 )
			return 0;
		uValue += ( ( SphWordID_t ( iByte & 0x7f ) ) << iOffset );
		iOffset += 7;
	}
	while ( iByte & 0x80 );
	return uValue;
}

DWORD CSphBin::UnzipInt ()
{
	register int b = 0;
	register DWORD v = 0;
	do
	{
		b = ReadByte();
		if ( b<0 )
			b = 0;
		v = ( v<<7 ) + ( b & 0x7f );
	} while ( b & 0x80 );
	return v;
}

SphOffset_t CSphBin::UnzipOffset ()
{
	register int b = 0;
	register SphOffset_t v = 0;
	do
	{
		b = ReadByte();
		if ( b<0 )
			b = 0;
		v = ( v<<7 ) + ( b & 0x7f );
	} while ( b & 0x80 );
	return v;
}

int CSphBin::ReadHit ( CSphAggregateHit * pOut )
{
	// expected EOB
	if ( m_iDone )
	{
		pOut->m_uWordID = 0;
		return 1;
	}

	CSphAggregateHit & tHit = m_tHit; // shortcut
	while (true)
	{
		// SPH_MAX_WORD_LEN is now 42 only to keep ReadVLB() below
		// technically, we can just use different functions on different paths, if ever needed
		STATIC_ASSERT ( SPH_MAX_WORD_LEN*3<=127, KEYWORD_TOO_LONG );
		SphWordID_t uDelta = ReadVLB();

		if ( uDelta )
		{
			switch ( m_eState )
			{
				case BIN_WORD:
					if ( m_bWordDict )
					{
#ifdef NDEBUG
						// FIXME?! move this under PARANOID or something?
						// or just introduce an assert() checked release build?
						if ( uDelta>=sizeof(m_sKeyword) )
							sphDie ( "INTERNAL ERROR: corrupted keyword length (len=" UINT64_FMT ", deltapos=" UINT64_FMT ")",
								(uint64_t)uDelta, (uint64_t)(m_iFilePos-m_iLeft) );
#else
						assert ( uDelta>0 && uDelta<sizeof(m_sKeyword)-1 );
#endif

						ReadBytes ( m_sKeyword, (int)uDelta );
						m_sKeyword[uDelta] = '\0';
						tHit.m_uWordID = sphCRC32 ( m_sKeyword ); // must be in sync with dict!

#ifndef NDEBUG
						assert ( ( m_iLastWordID<tHit.m_uWordID )
							|| ( m_iLastWordID==tHit.m_uWordID && strcmp ( (char*)m_sLastKeyword, (char*)m_sKeyword )<0 ) );
						strncpy ( (char*)m_sLastKeyword, (char*)m_sKeyword, sizeof(m_sLastKeyword) );
#endif

					} else
						tHit.m_uWordID += uDelta;

					tHit.m_tRowID = INVALID_ROWID;
					tHit.m_iWordPos = EMPTY_HIT;
					tHit.m_dFieldMask.UnsetAll();
					m_eState = BIN_DOC;
					break;

				case BIN_DOC:
					// doc id
					m_eState = BIN_POS;
					tHit.m_tRowID += uDelta;
					tHit.m_iWordPos = EMPTY_HIT;
					break;

				case BIN_POS:
					if ( m_eMode==SPH_HITLESS_ALL )
					{
						tHit.m_dFieldMask.Assign32 ( (DWORD)ReadVLB() );
						m_eState = BIN_DOC;

					} else if ( m_eMode==SPH_HITLESS_SOME )
					{
						if ( uDelta & 1 )
						{
							tHit.m_dFieldMask.Assign32 ( (DWORD)ReadVLB() );
							m_eState = BIN_DOC;
						}
						uDelta >>= 1;
					}
					tHit.m_iWordPos += (DWORD)uDelta;
					*pOut = tHit;
					return 1;

				default:
					sphDie ( "INTERNAL ERROR: unknown bin state (state=%d)", m_eState );
			}
		} else
		{
			switch ( m_eState )
			{
				case BIN_POS:	m_eState = BIN_DOC; break;
				case BIN_DOC:	m_eState = BIN_WORD; break;
				case BIN_WORD:	m_iDone = 1; pOut->m_uWordID = 0; return 1;
				default:		sphDie ( "INTERNAL ERROR: unknown bin state (state=%d)", m_eState );
			}
		}
	}
}


bool CSphBin::IsEOF () const
{
	return m_iDone!=0 || m_iFileLeft<=0;
}


bool CSphBin::IsDone () const
{
	return m_iDone!=0 || ( m_iFileLeft<=0 && m_iLeft<=0 );
}


ESphBinRead CSphBin::Precache ()
{
	if ( m_iFileLeft > m_iSize-m_iLeft )
	{
		m_bError = true;
		return BIN_PRECACHE_ERROR;
	}

	if ( !m_iFileLeft )
		return BIN_PRECACHE_OK;

	if ( *m_pFilePos!=m_iFilePos )
	{
		if ( !SeekAndWarn ( m_iFile, m_iFilePos, "CSphBin::Precache" ))
		{
			m_bError = true;
			return BIN_PRECACHE_ERROR;
		}

		*m_pFilePos = m_iFilePos;
	}

	assert ( m_dBuffer );
	memmove ( m_dBuffer, m_pCurrent, m_iLeft );

	if ( sphReadThrottled ( m_iFile, m_dBuffer+m_iLeft, m_iFileLeft )!=(size_t)m_iFileLeft )
	{
		m_bError = true;
		return BIN_READ_ERROR;
	}

	m_iLeft += m_iFileLeft;
	m_iFilePos += m_iFileLeft;
	m_iFileLeft -= m_iFileLeft;
	m_pCurrent = m_dBuffer;
	*m_pFilePos += m_iFileLeft;

	return BIN_PRECACHE_OK;
}


CSphMultiQueryArgs::CSphMultiQueryArgs ( int iIndexWeight )
	: m_iIndexWeight ( iIndexWeight )
{
	assert ( iIndexWeight>0 );
}

/////////////////////////////////////////////////////////////////////////////
// INDEX
/////////////////////////////////////////////////////////////////////////////

std::atomic<long> CSphIndex::m_tIdGenerator {0};

CSphIndex::CSphIndex ( const char * sIndexName, const char * sFilename )
	: m_tSchema ( sFilename )
	, m_sIndexName ( sIndexName )
	, m_sFilename ( sFilename )
{
	m_iIndexId = m_tIdGenerator.fetch_add ( 1, std::memory_order_relaxed );
	m_tMutableSettings = MutableIndexSettings_c::GetDefaults();
}


CSphIndex::~CSphIndex ()
{
	QcacheDeleteIndex ( m_iIndexId );

	SkipCache_c * pSkipCache = SkipCache_c::Get();
	if ( pSkipCache )
		pSkipCache->DeleteAll(m_iIndexId);
}


void CSphIndex::SetInplaceSettings ( int iHitGap, float fRelocFactor, float fWriteFactor )
{
	m_iHitGap = iHitGap;
	m_fRelocFactor = fRelocFactor;
	m_fWriteFactor = fWriteFactor;
	m_bInplaceSettings = true;
}


void CSphIndex::SetFieldFilter ( ISphFieldFilter * pFieldFilter )
{
	SafeAddRef ( pFieldFilter );
	m_pFieldFilter = pFieldFilter;
}


void CSphIndex::SetTokenizer ( ISphTokenizer * pTokenizer )
{
	SafeAddRef ( pTokenizer );
	m_pTokenizer = pTokenizer;
}


void CSphIndex::SetupQueryTokenizer()
{
	bool bWordDict = m_pDict->GetSettings().m_bWordDict;

	// create and setup a master copy of query time tokenizer
	// that we can then use to create lightweight clones
	m_pQueryTokenizer = sphCloneAndSetupQueryTokenizer ( m_pTokenizer, IsStarDict ( bWordDict ), m_tSettings.m_bIndexExactWords, false );
	m_pQueryTokenizerJson = sphCloneAndSetupQueryTokenizer ( m_pTokenizer, IsStarDict ( bWordDict ), m_tSettings.m_bIndexExactWords, true );
}


ISphTokenizer *	CSphIndex::LeakTokenizer ()
{
	return m_pTokenizer.Leak();
}


void CSphIndex::SetDictionary ( CSphDict * pDict )
{
	SafeAddRef ( pDict );
	m_pDict = pDict;
}


CSphDict * CSphIndex::LeakDictionary ()
{
	return m_pDict.Leak();
}


void CSphIndex::Setup ( const CSphIndexSettings & tSettings )
{
	m_bStripperInited = true;
	m_tSettings = tSettings;
}

void CSphIndex::SetCacheSize ( int iMaxCachedDocs, int iMaxCachedHits )
{
	m_iMaxCachedDocs = iMaxCachedDocs;
	m_iMaxCachedHits = iMaxCachedHits;
}


float CSphIndex::GetGlobalIDF ( const CSphString & sWord, int64_t iDocsLocal, bool bPlainIDF ) const
{
	auto pIDFer = sph::GetIDFer ( m_sGlobalIDFPath );
	if ( !pIDFer )
		return 0.0;
	return pIDFer->GetIDF ( sWord, iDocsLocal, bPlainIDF );
}


int CSphIndex::UpdateAttributes ( AttrUpdateSharedPtr_t pUpd, bool & bCritical, CSphString & sError, CSphString & sWarning )
{
	AttrUpdateInc_t tUpdInc { std::move (pUpd) };
	return UpdateAttributes ( tUpdInc, bCritical, sError, sWarning );
}

CSphVector<SphAttr_t> CSphIndex::BuildDocList () const
{
	TlsMsg::Err(); // reset error
	return CSphVector<SphAttr_t>();
}

void CSphIndex::GetFieldFilterSettings ( CSphFieldFilterSettings & tSettings ) const
{
	if ( m_pFieldFilter )
		m_pFieldFilter->GetSettings ( tSettings );
}


void CSphIndex::SetMutableSettings ( const MutableIndexSettings_c & tSettings )
{
	m_tMutableSettings = tSettings;
}


int64_t CSphIndex::GetPseudoShardingMetric() const
{
	int64_t iTotalDocs = GetStats().m_iTotalDocuments;
	return iTotalDocs > g_iSplitThresh ? iTotalDocs : -1;
}

//////////////////////////////////////////////////////////////////////////

static void PooledAttrsToPtrAttrs ( const VecTraits_T<ISphMatchSorter *> & dSorters, const BYTE * pBlobPool, columnar::Columnar_i * pColumnar, bool bFinalizeMatches )
{
	dSorters.Apply ( [&] ( ISphMatchSorter * p )
	{
		if ( p )
			p->TransformPooled2StandalonePtrs ( [pBlobPool] ( const CSphMatch * ) { return pBlobPool; }, [pColumnar] ( const CSphMatch * ) { return pColumnar; }, bFinalizeMatches );
	});
}


CSphIndex * sphCreateIndexPhrase ( const char* szIndexName, const char * sFilename )
{
	return new CSphIndex_VLN ( szIndexName, sFilename );
}

//////////////////////////////////////////////////////////////////////////


CSphIndex_VLN::CSphIndex_VLN ( const char * szIndexName, const char * szFilename )
	: CSphIndex ( szIndexName, szFilename )
	, m_iLockFD ( -1 )
	, m_dFieldLens ( SPH_MAX_FIELDS )
{
	m_sFilename = szFilename;

	m_iDocinfo = 0;
	m_iDocinfoIndex = 0;
	m_pDocinfoIndex = NULL;

	m_uVersion = INDEX_FORMAT_VERSION;
	m_bPassedRead = false;
	m_bPassedAlloc = false;
	m_bIsEmpty = true;
	m_bDebugCheck = false;
	m_uAttrsStatus = 0;

	m_iMinMaxIndex = 0;

	m_dFieldLens.ZeroVec();
}


CSphIndex_VLN::~CSphIndex_VLN ()
{
	SafeDelete ( m_pHistograms );
	Unlock();
}

class DocIdIndexReader_c
{
public:
	DocIdIndexReader_c ( const VecTraits_T<int>& dIdx, const VecTraits_T<DocID_t>& dDocids )
		: m_pCur ( dIdx.begin() )
		, m_pEnd ( dIdx.end() )
		, m_dDocids ( dDocids )
	{}

	inline bool ReadDocID ( DocID_t & tDocID )
	{
		if ( m_pCur>=m_pEnd )
			return false;

		tDocID = m_dDocids[*m_pCur];
		++m_pCur;
		return true;
	}

	int GetIndex() const
	{
		return *( m_pCur-1 );
	}

private:
	const int * m_pCur;
	const int * m_pEnd;
	const VecTraits_T<DocID_t> & m_dDocids;
};

template <typename FUNCTOR>
void Intersect ( LookupReaderIterator_c& tReader1, DocIdIndexReader_c & tReader2, FUNCTOR && tFunctor )
{
	RowID_t tRowID1 = INVALID_ROWID;
	DocID_t tDocID1 = 0, tDocID2 = 0;
	bool bHaveDocs1 = tReader1.Read ( tDocID1, tRowID1 );
	bool bHaveDocs2 = tReader2.ReadDocID ( tDocID2 );

	while ( bHaveDocs1 && bHaveDocs2 )
	{
		if ( tDocID1 < tDocID2 )
		{
			tReader1.HintDocID ( tDocID2 );
			bHaveDocs1 = tReader1.Read ( tDocID1, tRowID1 );
		}
		else if (  tDocID1 > tDocID2 )
			bHaveDocs2 = tReader2.ReadDocID ( tDocID2 );
		else
		{
			tFunctor ( tRowID1, tReader2.GetIndex () );
			bHaveDocs1 = tReader1.Read ( tDocID1, tRowID1 );
			bHaveDocs2 = tReader2.ReadDocID ( tDocID2 );
		}
	}
}

// fill collect rows which will be updated in this index
RowsToUpdateData_t CSphIndex_VLN::Update_CollectRowPtrs ( const UpdateContext_t & tCtx )
{
	RowsToUpdateData_t dRowsToUpdate;
	const auto & dDocids = tCtx.m_tUpd.m_pUpdate->m_dDocids;

	// collect idxes of alive (not-yet-updated) rows
	CSphVector<int> dSorted;
	dSorted.Reserve ( dDocids.GetLength() - tCtx.m_tUpd.m_iAffected );
	ARRAY_CONSTFOREACH (i, dDocids)
		if ( !tCtx.m_tUpd.m_dUpdated.BitGet ( i ) )
			dSorted.Add ( i );

	if ( dSorted.IsEmpty () )
		return dRowsToUpdate;

	dSorted.Sort ( Lesser ( [&dDocids] ( int a, int b ) { return dDocids[a]<dDocids[b]; } ) );
	DocIdIndexReader_c tSortedReader ( dSorted, dDocids );
	LookupReaderIterator_c tLookupReader ( m_tDocidLookup.GetReadPtr() );
	Intersect ( tLookupReader, tSortedReader, [&dRowsToUpdate, this] ( RowID_t tRowID, int iIdx )
	{
		auto& dUpd = dRowsToUpdate.Add();
		dUpd.m_pRow = GetDocinfoByRowID ( tRowID );
		dUpd.m_iIdx = iIdx;
		assert ( dUpd.m_pRow );
	} );
	return dRowsToUpdate;
}

// We fill docinfo ptr for actual rows, and move out non-actual (the ones which doesn't point to existing document)
// note, it actually changes (rearranges) rows!
RowsToUpdate_t CSphIndex_VLN::Update_PrepareGatheredRowPtrs ( RowsToUpdate_t & dWRows, const VecTraits_T<DocID_t>& dDocids )
{
	RowsToUpdate_t dRows = dWRows; // that is actually to indicate that we CHANGE contents inside dWRows, so it should be passed by non-const reference.

	dRows.Sort ( Lesser ( [&dDocids] ( auto& a, auto& b ) { return dDocids[a.m_iIdx]<dDocids[b.m_iIdx]; } ) );
	LookupReaderIterator_c tLookupReader ( m_tDocidLookup.GetReadPtr() );

	RowID_t tRowID = INVALID_ROWID;
	DocID_t tDocID = 0;
	bool bHaveDocs = tLookupReader.Read ( tDocID, tRowID );
	bool bHaveDocsToUpdate = !dRows.IsEmpty();
	DocID_t tDocIDPrepared = bHaveDocsToUpdate ? dDocids[dRows[0].m_iIdx] : 0;
	int iReadIdx = 0;
	int iWriteIdx = 0;

	while ( bHaveDocs && bHaveDocsToUpdate )
	{
		if ( tDocID < tDocIDPrepared )
		{
			tLookupReader.HintDocID ( tDocIDPrepared );
			bHaveDocs = tLookupReader.Read ( tDocID, tRowID );
			continue;
		} else if ( tDocID == tDocIDPrepared )
		{
			dRows[iWriteIdx].m_pRow = GetDocinfoByRowID ( tRowID );
			assert ( dRows[iWriteIdx].m_pRow );
			Swap ( dRows[iWriteIdx].m_iIdx, dRows[iReadIdx].m_iIdx );
			bHaveDocs = tLookupReader.Read ( tDocID, tRowID );
			++iWriteIdx;
		}

		++iReadIdx;
		bHaveDocsToUpdate = iReadIdx < dRows.GetLength();
		if ( bHaveDocsToUpdate )
			tDocIDPrepared = dDocids[dRows[iReadIdx].m_iIdx];
	}
	return dWRows.Slice ( 0, iWriteIdx );
}


bool CSphIndex_VLN::Update_WriteBlobRow ( UpdateContext_t & tCtx, CSphRowitem * pDocinfo, const BYTE * pBlob,
		int iLength, int nBlobAttrs, const CSphAttrLocator & tBlobRowLoc, bool & bCritical, CSphString & sError )
{
	BYTE * pExistingBlob = m_tBlobAttrs.GetWritePtr() + sphGetRowAttr ( pDocinfo, tBlobRowLoc );
	DWORD uExistingBlobLen = sphGetBlobTotalLen ( pExistingBlob, nBlobAttrs );

	bCritical = false;

	// overwrite old record (because we have the write lock)
	if ( (DWORD)iLength<=uExistingBlobLen )
	{
		memcpy ( pExistingBlob, pBlob, iLength );
		return true;
	}

	BYTE * pOldBlobPool = m_tBlobAttrs.GetWritePtr();
	SphOffset_t tBlobSpaceUsed = *(SphOffset_t*)pOldBlobPool;
	SphOffset_t tSpaceLeft = m_tBlobAttrs.GetLengthBytes()-tBlobSpaceUsed;

	// not great: we have to resize our .spb file and create new memory maps
	if ( (SphOffset_t)iLength > tSpaceLeft )
	{
		SphOffset_t tSizeDelta = Max ( (SphOffset_t)iLength-tSpaceLeft, m_tSettings.m_tBlobUpdateSpace );
		CSphString sWarning;
		size_t tOldSize = m_tBlobAttrs.GetLengthBytes();
		if ( !m_tBlobAttrs.Resize ( tOldSize + tSizeDelta, sWarning, sError ) )
		{
			// try to map again, using old size
			if ( !m_tBlobAttrs.Resize ( tOldSize, sWarning, sError ) )
				bCritical = true;	// real bad, index unusable

			sError = "unable to resize .SPB file";
			return false;
		}

		// update blob pool ptr since it might be changed after resize
		tCtx.m_pBlobPool = m_tBlobAttrs.GetWritePtr();
	}

	BYTE * pEnd = m_tBlobAttrs.GetWritePtr() + tBlobSpaceUsed;
	memcpy ( pEnd, pBlob, iLength );

	sphSetRowAttr ( pDocinfo, tBlobRowLoc, tBlobSpaceUsed );

	tBlobSpaceUsed += iLength;

	*(SphOffset_t*)m_tBlobAttrs.GetWritePtr() = tBlobSpaceUsed;

	return true;
}


void CSphIndex_VLN::Update_MinMax ( const RowsToUpdate_t& dRows, const UpdateContext_t & tCtx )
{
	int iRowStride = tCtx.m_tSchema.GetRowSize();

	for ( const auto & tRow : dRows )
	{
		int64_t iBlock = int64_t ( tRow.m_pRow-tCtx.m_pAttrPool ) / ( iRowStride*DOCINFO_INDEX_FREQ );
		DWORD * pBlockRanges = m_pDocinfoIndex + ( iBlock * iRowStride * 2 );
		DWORD * pIndexRanges = m_pDocinfoIndex + ( m_iDocinfoIndex * iRowStride * 2 );
		assert ( iBlock>=0 && iBlock<m_iDocinfoIndex );

		ARRAY_CONSTFOREACH ( iCol, tCtx.m_tUpd.m_pUpdate->m_dAttributes )
		{
			const UpdatedAttribute_t & tUpdAttr = tCtx.m_dUpdatedAttrs[iCol];
			if ( !tUpdAttr.m_bExisting )
				continue;

			const CSphAttrLocator & tLoc = tUpdAttr.m_tLocator;
			if ( tLoc.IsBlobAttr() )
				continue;

			SphAttr_t uValue = sphGetRowAttr ( tRow.m_pRow, tLoc );

			// update block and index ranges
			for ( int i=0; i<2; i++ )
			{
				DWORD * pBlock = i ? pBlockRanges : pIndexRanges;
				SphAttr_t uMin = sphGetRowAttr ( pBlock, tLoc );
				SphAttr_t uMax = sphGetRowAttr ( pBlock+iRowStride, tLoc );
				if ( tUpdAttr.m_eAttrType==SPH_ATTR_FLOAT ) // update float's indexes assumes float comparision
				{
					float fValue = sphDW2F ( (DWORD) uValue );
					float fMin = sphDW2F ( (DWORD) uMin );
					float fMax = sphDW2F ( (DWORD) uMax );
					if ( fValue<fMin )
						sphSetRowAttr ( pBlock, tLoc, sphF2DW ( fValue ) );
					if ( fValue>fMax )
						sphSetRowAttr ( pBlock+iRowStride, tLoc, sphF2DW ( fValue ) );
				} else // update usual integers
				{
					if ( uValue<uMin )
						sphSetRowAttr ( pBlock, tLoc, uValue );
					if ( uValue>uMax )
						sphSetRowAttr ( pBlock+iRowStride, tLoc, uValue );
				}
			}
		}
	}
}

bool CSphIndex_VLN::DoUpdateAttributes ( const RowsToUpdate_t& dRows, UpdateContext_t& tCtx, bool& bCritical, CSphString& sError )
{
	if ( dRows.IsEmpty() )
		return true;

	tCtx.m_pHistograms = m_pHistograms;
	tCtx.m_pBlobPool = m_tBlobAttrs.GetWritePtr();
	tCtx.m_pAttrPool = m_tAttr.GetWritePtr ();
	tCtx.m_pSegment = this;

	if ( !Update_CheckAttributes ( *tCtx.m_tUpd.m_pUpdate, tCtx.m_tSchema, sError ) )
		return false;

	Update_PrepareListOfUpdatedAttributes ( tCtx, sError );

	// FIXME! FIXME! FIXME! overwriting just-freed blocks might hurt concurrent searchers;
	// should implement a simplistic MVCC-style delayed-free to avoid that

	// first pass, if needed
	if ( tCtx.m_tUpd.m_pUpdate->m_bStrict )
		if ( !Update_InplaceJson ( dRows, tCtx, sError, true ) )
			return false;

	// second pass
	tCtx.m_iJsonWarnings = 0;
	Update_InplaceJson ( dRows, tCtx, sError, false );

	if ( !Update_Blobs ( dRows, tCtx, bCritical, sError ) )
		return false;

	Update_Plain ( dRows, tCtx );
	Update_MinMax ( dRows, tCtx );
	return true;
}

int CSphIndex_VLN::UpdateAttributes ( AttrUpdateInc_t & tUpd, bool & bCritical, CSphString & sError, CSphString & sWarning )
{
	assert ( tUpd.m_pUpdate->m_dRowOffset.IsEmpty() || tUpd.m_pUpdate->m_dDocids.GetLength()==tUpd.m_pUpdate->m_dRowOffset.GetLength() );

	// check if we have to
	if ( !m_iDocinfo || tUpd.m_pUpdate->m_dDocids.IsEmpty() )
		return 0;

	UpdateContext_t tCtx ( tUpd, m_tSchema );
	int iUpdated = tUpd.m_iAffected;

	auto dRowsToUpdate = Update_CollectRowPtrs ( tCtx );
	if ( !DoUpdateAttributes ( dRowsToUpdate, tCtx, bCritical, sError ))
		return -1;

	iUpdated = tUpd.m_iAffected - iUpdated;

	if ( !Update_HandleJsonWarnings ( tCtx, iUpdated, sWarning, sError ) )
		return -1;

	if ( tCtx.m_uUpdateMask && m_bBinlog )
		Binlog::CommitUpdateAttributes ( &m_iTID, m_sIndexName.cstr(), *tUpd.m_pUpdate );

	m_uAttrsStatus |= tCtx.m_uUpdateMask; // FIXME! add lock/atomic?

	if ( m_bAttrsBusy.load ( std::memory_order_acquire ) )
	{
		auto& tNewUpdate = m_dPostponedUpdates.Add();
		tNewUpdate.m_pUpdate = MakeReusableUpdate(tUpd.m_pUpdate);
		tNewUpdate.m_dRowsToUpdate.SwapData ( dRowsToUpdate );
	}

	return iUpdated;
}

void CSphIndex_VLN::UpdateAttributesOffline ( VecTraits_T<PostponedUpdate_t> & dUpdates, IndexSegment_c *)
{
	if ( dUpdates.IsEmpty () )
		return;

	CSphString sError;
	bool bCritical;

	for ( auto & tUpdate : dUpdates )
	{
		AttrUpdateInc_t tUpdInc { tUpdate.m_pUpdate }; // dont move, keep update (need twice when split chunks)
		UpdateContext_t tCtx ( tUpdInc, m_tSchema );
		RowsToUpdate_t dRows = Update_PrepareGatheredRowPtrs ( tUpdate.m_dRowsToUpdate, tCtx.m_tUpd.m_pUpdate->m_dDocids );
		if ( !DoUpdateAttributes ( dRows, tCtx, bCritical, sError ) )
		{
			sphWarning ("UpdateAttributesOffline: %s", sError.cstr() );
			break;
		}
		m_uAttrsStatus |= tCtx.m_uUpdateMask; // FIXME! add lock/atomic?
	}
}

// safely rename an index file
bool CSphIndex_VLN::JuggleFile ( ESphExt eExt, CSphString & sError, bool bNeedSrc, bool bNeedDst ) const
{
	CSphString sExt = GetIndexFileName ( eExt );
	CSphString sExtNew = GetIndexFileName ( eExt, true );
	CSphString sExtOld;
	sExtOld.SetSprintf ( "%s.old", sExt.cstr() );

	if ( sph::rename ( sExt.cstr(), sExtOld.cstr() ) )
	{
		if ( bNeedSrc )
		{
			sError.SetSprintf ( "rename '%s' to '%s' failed: %s", sExt.cstr(), sExtOld.cstr(), strerror(errno) );
			return false;
		}
	}

	if ( sph::rename ( sExtNew.cstr(), sExt.cstr() ) )
	{
		if ( bNeedDst )
		{
			if ( bNeedSrc && !sph::rename ( sExtOld.cstr(), sExt.cstr() ) )
			{
				// rollback failed too!
				sError.SetSprintf ( "rollback rename to '%s' failed: %s; INDEX UNUSABLE; FIX FILE NAMES MANUALLY", sExt.cstr(), strerror(errno) );
			} else
			{
				// rollback went ok
				sError.SetSprintf ( "rename '%s' to '%s' failed: %s", sExtNew.cstr(), sExt.cstr(), strerror(errno) );
			}
			return false;
		}
	}

	// all done
	::unlink ( sExtOld.cstr() );
	return true;
}

bool CSphIndex_VLN::SaveAttributes ( CSphString & sError ) const
{
	if ( !m_uAttrsStatus || !m_iDocinfo )
		return true;

	DWORD uAttrStatus = m_uAttrsStatus;

	sphLogDebugvv ( "index '%s' attrs (%u) saving...", m_sIndexName.cstr(), uAttrStatus );

	if ( uAttrStatus & IndexUpdateHelper_c::ATTRS_UPDATED )
	{
		if ( !m_tAttr.Flush ( true, sError ) )
			return false;

		if ( m_pHistograms && !m_pHistograms->Save ( GetIndexFileName(SPH_EXT_SPHI), sError ) )
			return false;
	}

	if ( uAttrStatus & IndexUpdateHelper_c::ATTRS_BLOB_UPDATED )
	{
		if ( !m_tBlobAttrs.Flush ( true, sError ) )
			return false;
	}

	if ( uAttrStatus & IndexUpdateHelper_c::ATTRS_ROWMAP_UPDATED )
	{
		if ( !m_tDeadRowMap.Flush ( true, sError ) )
			return false;
	}

	if ( m_bBinlog )
		Binlog::NotifyIndexFlush ( m_sIndexName.cstr(), m_iTID, false );

	if ( m_uAttrsStatus==uAttrStatus )
		m_uAttrsStatus = 0;

	sphLogDebugvv ( "index '%s' attrs (%u) saved", m_sIndexName.cstr(), m_uAttrsStatus );

	return true;
}

DWORD CSphIndex_VLN::GetAttributeStatus () const
{
	return m_uAttrsStatus;
}


//////////////////////////////////////////////////////////////////////////

struct CmpDocidLookup_fn
{
	static inline bool IsLess ( const DocidRowidPair_t & a, const DocidRowidPair_t & b )
	{
		if ( a.m_tDocID==b.m_tDocID )
			return a.m_tRowID < b.m_tRowID;

		return a.m_tDocID < b.m_tDocID;
	}
};


struct CmpQueuedLookup_fn
{
	static DocidRowidPair_t * m_pStorage;

	static inline bool IsLess ( const int a, const int b )
	{
		if ( m_pStorage[a].m_tDocID==m_pStorage[b].m_tDocID )
			return m_pStorage[a].m_tRowID < m_pStorage[b].m_tRowID;

		return m_pStorage[a].m_tDocID < m_pStorage[b].m_tDocID;
	}
};

DocidRowidPair_t * CmpQueuedLookup_fn::m_pStorage = nullptr;

bool CSphIndex_VLN::Alter_IsMinMax ( const CSphRowitem * pDocinfo, int iStride ) const
{
	return pDocinfo-m_tAttr.GetWritePtr() >= m_iDocinfo*iStride;
}


bool CSphIndex_VLN::AddRemoveColumnarAttr ( bool bAddAttr, const CSphString & sAttrName, ESphAttr eAttrType, const ISphSchema & tOldSchema, const ISphSchema & tNewSchema, CSphString & sError )
{
	CSphScopedPtr<columnar::Builder_i> pBuilder ( CreateColumnarBuilder ( tNewSchema, m_tSettings, GetIndexFileName ( SPH_EXT_SPC, true ), sError ) );
	if ( !pBuilder )
		return false;

	if ( !Alter_AddRemoveColumnar ( bAddAttr, m_tSchema, tNewSchema, m_pColumnar.Ptr(), pBuilder.Ptr(), (DWORD)m_iDocinfo, m_sIndexName, sError ) )
		return false;

	return true;
}


bool CSphIndex_VLN::AddRemoveAttribute ( bool bAddAttr, const AttrAddRemoveCtx_t & tCtx, CSphString & sError )
{
	AttrEngine_e eAttrEngine = CombineEngines ( m_tSettings.m_eEngine, tCtx.m_eEngine );
	AttrAddRemoveCtx_t tNewCtx = tCtx;
	if ( eAttrEngine==AttrEngine_e::COLUMNAR )
		tNewCtx.m_uFlags |= CSphColumnInfo::ATTR_COLUMNAR;
	else
		tNewCtx.m_uFlags &= ~( CSphColumnInfo::ATTR_COLUMNAR_HASHES | CSphColumnInfo::ATTR_STORED );

	CSphSchema tNewSchema = m_tSchema;
	if ( !Alter_AddRemoveFromSchema ( tNewSchema, tNewCtx, bAddAttr, sError ) )
		return false;

	int iNewStride = tNewSchema.GetRowSize();

	int64_t iNewMinMaxIndex = m_iDocinfo * iNewStride;

	BuildHeader_t tBuildHeader ( m_tStats );
	tBuildHeader.m_iMinMaxIndex = iNewMinMaxIndex;

	*(DictHeader_t*)&tBuildHeader = *(DictHeader_t*)&m_tWordlist;

	CSphString sHeaderName = GetIndexFileName ( SPH_EXT_SPH, true );
	WriteHeader_t tWriteHeader;
	tWriteHeader.m_pSettings = &m_tSettings;
	tWriteHeader.m_pSchema = &tNewSchema;
	tWriteHeader.m_pTokenizer = m_pTokenizer;
	tWriteHeader.m_pDict = m_pDict;
	tWriteHeader.m_pFieldFilter = m_pFieldFilter;
	tWriteHeader.m_pFieldLens = m_dFieldLens.Begin();

	// save the header
	if ( !IndexBuildDone ( tBuildHeader, tWriteHeader, sHeaderName, sError ) )
		return false;

	// generate new .SPA, .SPB files
	CSphWriter tSPAWriter;
	CSphWriter tSPBWriter;
	tSPAWriter.SetBufferSize ( 524288 );
	tSPBWriter.SetBufferSize ( 524288 );

	CSphScopedPtr<WriteWrapper_c> pSPAWriteWrapper ( CreateWriteWrapperDisk(tSPAWriter) );
	CSphScopedPtr<WriteWrapper_c> pSPBWriteWrapper ( CreateWriteWrapperDisk(tSPBWriter) );

	CSphString sSPAfile = GetIndexFileName ( SPH_EXT_SPA, true );
	CSphString sSPBfile = GetIndexFileName ( SPH_EXT_SPB, true );
	CSphString sSPHIfile = GetIndexFileName ( SPH_EXT_SPHI, true );
	if ( !tSPAWriter.OpenFile ( sSPAfile, sError ) )
		return false;

	bool bHadBlobs = false;
	for ( int i = 0; i < m_tSchema.GetAttrsCount(); i++ )
		bHadBlobs |= sphIsBlobAttr ( m_tSchema.GetAttr(i) );

	bool bHaveBlobs = false;
	for ( int i = 0; i < tNewSchema.GetAttrsCount(); i++ )
		bHaveBlobs |= sphIsBlobAttr ( tNewSchema.GetAttr(i) );

	bool bBlob = sphIsBlobAttr ( tCtx.m_eType );
	bool bBlobsModified = bBlob && ( bAddAttr || bHaveBlobs==bHadBlobs );
	if ( bBlobsModified )
	{
		if ( !tSPBWriter.OpenFile ( sSPBfile, sError ) )
			return false;

		tSPBWriter.PutOffset(0);
	}

	if ( !tNewSchema.GetAttrsCount() )
	{
		sError = "index must have at least one attribute";
		return false;
	}

	bool bColumnar = bAddAttr ? tNewSchema.GetAttr ( tCtx.m_sName.cstr() )->IsColumnar() : m_tSchema.GetAttr ( tCtx.m_sName.cstr() )->IsColumnar();
	if ( bColumnar )
		AddRemoveColumnarAttr ( bAddAttr, tCtx.m_sName, tCtx.m_eType, m_tSchema, tNewSchema, sError );
	else
	{
		int64_t iTotalRows = m_iDocinfo + (m_iDocinfoIndex+1)*2;
		Alter_AddRemoveRowwiseAttr ( m_tSchema, tNewSchema, m_tAttr.GetWritePtr(), (DWORD)iTotalRows, m_tBlobAttrs.GetWritePtr(), *pSPAWriteWrapper, *pSPBWriteWrapper, bAddAttr, tCtx.m_sName );
	}

	if ( m_pHistograms )
	{
		if ( bAddAttr )
		{
			Histogram_i * pNewHistogram = CreateHistogram ( tCtx.m_sName, tCtx.m_eType );
			if ( pNewHistogram )
			{
				for ( DWORD i = 0; i < m_iDocinfo; i++ )
					pNewHistogram->Insert(0);

				m_pHistograms->Add ( pNewHistogram );
			}
		}
		else
			m_pHistograms->Remove ( tCtx.m_sName );

		if ( !m_pHistograms->Save ( sSPHIfile, sError ) )
			return false;
	}

	if ( !AddRemoveFromDocstore ( m_tSchema, tNewSchema, sError ) )
		return false;

	if ( tSPAWriter.IsError() )
	{
		sError.SetSprintf ( "error writing to %s", sSPAfile.cstr() );
		return false;
	}

	tSPAWriter.CloseFile();

	bool bHadColumnar = m_tSchema.HasColumnarAttrs();
	bool bHaveColumnar = tNewSchema.HasColumnarAttrs();

	bool bHadNonColumnar = m_tSchema.HasNonColumnarAttrs();
	bool bHaveNonColumnar = tNewSchema.HasNonColumnarAttrs();

	m_tAttr.Reset();

	if ( bColumnar )
	{
		if ( !JuggleFile ( SPH_EXT_SPC, sError, bHadColumnar, bHaveColumnar ) )
			return false;

		if ( tNewSchema.HasColumnarAttrs() )
		{
			m_pColumnar = CreateColumnarStorageReader ( GetIndexFileName(SPH_EXT_SPC).cstr(), (DWORD)m_iDocinfo, sError );
			if ( !m_pColumnar.Ptr() )
				return false;
		}
		else
			m_pColumnar.Reset();
	}
	else
	{
		if ( !JuggleFile ( SPH_EXT_SPA, sError, bHadNonColumnar, bHaveNonColumnar ) )
			return false;
	}

	if ( !JuggleFile ( SPH_EXT_SPH, sError ) )
		return false;

	if ( !JuggleFile ( SPH_EXT_SPHI, sError ) )
		return false;

	if ( bHaveNonColumnar && !m_tAttr.Setup ( GetIndexFileName(SPH_EXT_SPA), sError, true ) )
		return false;

	if ( bBlob )
	{
		m_tBlobAttrs.Reset();

		if ( bAddAttr || bHaveBlobs==bHadBlobs )
		{
			if ( tSPBWriter.IsError() )
			{
				sError.SetSprintf ( "error writing to %s", sSPBfile.cstr() );
				return false;
			}

			SphOffset_t tPos = tSPBWriter.GetPos();
			// FIXME!!! made single function from this mess as order matters here
			tSPBWriter.Flush(); // store collected data as SeekTo might got rid of buffer collected so far
			tSPBWriter.SeekTo ( 0 );
			tSPBWriter.PutOffset ( tPos );
			tSPBWriter.SeekTo ( tPos + m_tSettings.m_tBlobUpdateSpace, true );
			tSPBWriter.CloseFile();

			if ( !JuggleFile ( SPH_EXT_SPB, sError, bHadBlobs ) )
				return false;

			if ( !m_tBlobAttrs.Setup ( GetIndexFileName(SPH_EXT_SPB), sError, true ) )
				return false;
		} else
			::unlink ( GetIndexFileName(SPH_EXT_SPB).cstr() );
	}

	m_tSchema = tNewSchema;
	m_iMinMaxIndex = iNewMinMaxIndex;
	m_pDocinfoIndex = m_tAttr.GetWritePtr() + m_iMinMaxIndex;

	PrereadMapping ( m_sIndexName.cstr(), "attributes", IsMlock ( m_tMutableSettings.m_tFileAccess.m_eAttr ), IsOndisk ( m_tMutableSettings.m_tFileAccess.m_eAttr ), m_tAttr );

	if ( bBlobsModified )
		PrereadMapping ( m_sIndexName.cstr(), "blob attributes", IsMlock ( m_tMutableSettings.m_tFileAccess.m_eBlob ), IsOndisk ( m_tMutableSettings.m_tFileAccess.m_eBlob ), m_tBlobAttrs );

	return true;
}


void CSphIndex_VLN::FlushDeadRowMap ( bool bWaitComplete ) const
{
	// FIXME! handle errors
	CSphString sError;
	m_tDeadRowMap.Flush ( bWaitComplete, sError );
}


bool CSphIndex_VLN::LoadKillList ( CSphFixedVector<DocID_t> *pKillList, KillListTargets_c & tTargets, CSphString & sError ) const
{
	CSphString sSPK = GetIndexFileName(SPH_EXT_SPK);
	if ( !sphIsReadable ( sSPK.cstr() ) )
		return true;

	CSphAutoreader tReader;
	if ( !tReader.Open ( sSPK, sError ) )
		return false;

	DWORD nIndexes = tReader.GetDword();
	tTargets.m_dTargets.Resize ( nIndexes );
	for ( auto & tIndex : tTargets.m_dTargets )
	{
		tIndex.m_sIndex = tReader.GetString();
		tIndex.m_uFlags = tReader.GetDword();
	}

	if ( pKillList )
	{
		DWORD nKills = tReader.GetDword();

		pKillList->Reset(nKills);
		DocID_t tDocID = 0;
		for ( int i = 0; i < (int)nKills; i++ )
		{
			DocID_t tDelta = tReader.UnzipOffset();
			assert ( tDelta>0 );
			tDocID += tDelta;
			(*pKillList)[i] = tDocID;
		}
	}

	if ( tReader.GetErrorFlag() )
	{
		sError = tReader.GetErrorMessage();
		return false;
	}

	return true;
}


bool WriteKillList ( const CSphString & sFilename, const DocID_t * pKlist, int nEntries, const KillListTargets_c & tTargets, CSphString & sError )
{
	if ( !nEntries && !tTargets.m_dTargets.GetLength() )
		return true;

	CSphWriter tKillList;
	if ( !tKillList.OpenFile ( sFilename, sError ) )
		return false;

	tKillList.PutDword ( tTargets.m_dTargets.GetLength() );
	for ( const auto & tTarget : tTargets.m_dTargets )
	{
		tKillList.PutString ( tTarget.m_sIndex );
		tKillList.PutDword ( tTarget.m_uFlags );
	}

	tKillList.PutDword ( nEntries );

	if ( pKlist )
	{
		DocID_t tPrevDocID = 0;
		for ( int i = 0; i < nEntries; i++ )
		{
			DocID_t tDocID = pKlist[i];
			tKillList.ZipOffset ( tDocID-tPrevDocID );
			tPrevDocID = tDocID;
		}
	}

	tKillList.CloseFile();
	if ( tKillList.IsError() )
	{
		sError.SetSprintf ( "error writing kill list to %s", sFilename.cstr() );
		return false;
	}

	return true;
}


bool CSphIndex_VLN::AlterKillListTarget ( KillListTargets_c & tTargets, CSphString & sError )
{
	CSphFixedVector<DocID_t> dKillList(0);
	KillListTargets_c tOldTargets;
	if ( !LoadKillList ( &dKillList, tOldTargets, sError ) )
		return false;

	if ( !WriteKillList ( GetIndexFileName ( SPH_EXT_SPK, true ), dKillList.Begin(), dKillList.GetLength(), tTargets, sError ) )
		return false;

	if ( !JuggleFile ( SPH_EXT_SPK, sError, false ) )
		return false;

	return true;
}


void CSphIndex_VLN::KillExistingDocids ( CSphIndex * pTarget )
{
	// FIXME! collecting all docids is a waste of memory
	LookupReaderIterator_c tLookup ( m_tDocidLookup.GetWritePtr() );
	CSphFixedVector<DocID_t> dKillList ( m_iDocinfo );
	DocID_t tDocID;
	DWORD uDocidIndex = 0;
	while ( tLookup.ReadDocID(tDocID) )
		dKillList[uDocidIndex++] = tDocID;

	pTarget->KillMulti ( dKillList );
}


int CSphIndex_VLN::KillMulti ( const VecTraits_T<DocID_t> & dKlist )
{
	LookupReaderIterator_c tTargetReader ( m_tDocidLookup.GetWritePtr() );
	DocidListReader_c tKillerReader ( dKlist );

	int iTotalKilled;
	if ( !m_pKillHook )
		iTotalKilled = KillByLookup ( tTargetReader, tKillerReader, m_tDeadRowMap, [] ( DocID_t ) {} );
	else
		iTotalKilled = KillByLookup ( tTargetReader, tKillerReader, m_tDeadRowMap,
				[this] ( DocID_t tDoc ) { m_pKillHook->Kill ( tDoc ); } );

	if ( iTotalKilled )
		m_uAttrsStatus |= IndexUpdateHelper_c::ATTRS_ROWMAP_UPDATED;

	return iTotalKilled;
}


/////////////////////////////////////////////////////////////////////////////

struct CmpHit_fn
{
	inline bool IsLess ( const CSphWordHit & a, const CSphWordHit & b ) const
	{
		return ( a.m_uWordID<b.m_uWordID ) ||
				( a.m_uWordID==b.m_uWordID && a.m_tRowID<b.m_tRowID ) ||
				( a.m_uWordID==b.m_uWordID && a.m_tRowID==b.m_tRowID && HITMAN::GetPosWithField ( a.m_uWordPos )<HITMAN::GetPosWithField ( b.m_uWordPos ) );
	}
};


CSphString CSphIndex_VLN::GetIndexFileName ( ESphExt eExt, bool bTemp ) const
{
	CSphString sRes;
	sRes.SetSprintf ( bTemp ? "%s.tmp%s" : "%s%s", m_sFilename.cstr(), sphGetExt(eExt) );
	return sRes;
}


CSphString CSphIndex_VLN::GetIndexFileName ( const char * szExt ) const
{
	CSphString sRes;
	sRes.SetSprintf ( "%s.%s", m_sFilename.cstr(), szExt );
	return sRes;
}

void CSphIndex_VLN::GetIndexFiles ( CSphVector<CSphString> & dFiles, const FilenameBuilder_i * pFilenameBuilder ) const
{
	for ( int iExt=0; iExt<SPH_EXT_TOTAL; iExt++ )
	{
		if ( iExt==SPH_EXT_SPL )
			continue;

		CSphString sName = GetIndexFileName ( (ESphExt)iExt, false );
		if ( !sphIsReadable ( sName.cstr() ) )
			continue;

		dFiles.Add ( sName );
	}

	// might be pFilenameBuilder from parent RT index
	CSphScopedPtr<const FilenameBuilder_i> pFilename ( nullptr );
	if ( !pFilenameBuilder && GetIndexFilenameBuilder() )
	{
		pFilename = GetIndexFilenameBuilder() ( m_sIndexName.cstr() );
		pFilenameBuilder = pFilename.Ptr();
	}

	GetSettingsFiles ( m_pTokenizer, m_pDict, GetSettings(), pFilenameBuilder, dFiles );
}

void GetSettingsFiles ( const ISphTokenizer * pTok, const CSphDict * pDict, const CSphIndexSettings & tSettings, const FilenameBuilder_i * pFilenameBuilder, StrVec_t & dFiles )
{
	assert ( pTok );
	assert ( pDict );

	StringBuilder_c sFiles ( "," );
	sFiles += pDict->GetSettings().m_sStopwords.cstr();
	sFiles += pTok->GetSettings().m_sSynonymsFile.cstr();
	sFiles += tSettings.m_sHitlessFiles.cstr();

	for ( const CSphString & sName : pDict->GetSettings().m_dWordforms )
		dFiles.Add ( pFilenameBuilder ? pFilenameBuilder->GetFullPath ( sName ) : sName );

	StrVec_t dFileNames = sphSplit ( sFiles.cstr(), ", " );
	for ( const CSphString & sName : dFileNames )
		dFiles.Add ( pFilenameBuilder ? pFilenameBuilder->GetFullPath ( sName ) : sName );
}


class CSphHitBuilder
{
public:
	CSphHitBuilder ( const CSphIndexSettings & tSettings, const CSphVector<SphWordID_t> & dHitless, bool bMerging, int iBufSize, CSphDict * pDict, CSphString * sError );
	~CSphHitBuilder () {}

	bool	CreateIndexFiles ( const char * sDocName, const char * sHitName, const char * sSkipName, bool bInplace, int iWriteBuffer, CSphAutofile & tHit, SphOffset_t * pSharedOffset );
	void	HitReset ();

	void	cidxHit ( CSphAggregateHit * pHit );
	bool	cidxDone ( int iMemLimit, int & iMinInfixLen, int iMaxCodepointLen, DictHeader_t * pDictHeader );
	int		cidxWriteRawVLB ( int fd, CSphWordHit * pHit, int iHits );

	SphOffset_t		GetHitfilePos () const { return m_wrHitlist.GetPos (); }
	void			CloseHitlist () { m_wrHitlist.CloseFile (); }
	bool			IsError () const { return ( m_pDict->DictIsError() || m_wrDoclist.IsError() || m_wrHitlist.IsError() ); }
	void			HitblockBegin () { m_pDict->HitblockBegin(); }
	bool			IsWordDict () const { return m_pDict->GetSettings().m_bWordDict; }

private:
	void	DoclistBeginEntry ( RowID_t tDocid );
	void	DoclistEndEntry ( Hitpos_t uLastPos );
	void	DoclistEndList ();

	CSphWriter					m_wrDoclist;			///< wordlist writer
	CSphWriter					m_wrHitlist;			///< hitlist writer
	CSphWriter					m_wrSkiplist;			///< skiplist writer
	CSphFixedVector<BYTE>		m_dWriteBuffer;			///< my write buffer (for temp files)

	CSphAggregateHit			m_tLastHit;				///< hitlist entry
	Hitpos_t					m_iPrevHitPos {0};		///< previous hit position
	bool						m_bGotFieldEnd = false;
	BYTE						m_sLastKeyword [ MAX_KEYWORD_BYTES ];

	const CSphVector<SphWordID_t> &	m_dHitlessWords;
	DictRefPtr_c				m_pDict;
	CSphString *				m_pLastError;

	int							m_iSkiplistBlockSize = 0;
	SphOffset_t					m_iLastHitlistPos = 0;		///< doclist entry
	SphOffset_t					m_iLastHitlistDelta = 0;	///< doclist entry
	FieldMask_t					m_dLastDocFields;		///< doclist entry
	DWORD						m_uLastDocHits = 0;			///< doclist entry

	CSphDictEntry				m_tWord;				///< dictionary entry

	ESphHitFormat				m_eHitFormat;
	ESphHitless					m_eHitless;

	CSphVector<SkiplistEntry_t>	m_dSkiplist;
#ifndef NDEBUG
	bool m_bMerging;
#endif
};


CSphHitBuilder::CSphHitBuilder ( const CSphIndexSettings & tSettings,
	const CSphVector<SphWordID_t> & dHitless, bool bMerging, int iBufSize,
	CSphDict * pDict, CSphString * sError )
	: m_dWriteBuffer ( iBufSize )
	, m_dHitlessWords ( dHitless )
	, m_pDict ( pDict )
	, m_pLastError ( sError )
	, m_iSkiplistBlockSize ( tSettings.m_iSkiplistBlockSize )
	, m_eHitFormat ( tSettings.m_eHitFormat )
	, m_eHitless ( tSettings.m_eHitless )
#ifndef NDEBUG
	, m_bMerging ( bMerging )
#endif
{
	m_sLastKeyword[0] = '\0';
	HitReset();
	m_dLastDocFields.UnsetAll();
	SafeAddRef ( pDict );
	assert ( m_pDict );
	assert ( m_pLastError );

	m_pDict->SetSkiplistBlockSize ( m_iSkiplistBlockSize );
}


bool CSphHitBuilder::CreateIndexFiles ( const char * sDocName, const char * sHitName, const char * sSkipName, bool bInplace, int iWriteBuffer, CSphAutofile & tHit, SphOffset_t * pSharedOffset )
{
	// doclist and hitlist files
	m_wrDoclist.CloseFile();
	m_wrHitlist.CloseFile();
	m_wrSkiplist.CloseFile();

	m_wrDoclist.SetBufferSize ( m_dWriteBuffer.GetLength() );
	m_wrHitlist.SetBufferSize ( bInplace ? iWriteBuffer : m_dWriteBuffer.GetLength() );

	if ( !m_wrDoclist.OpenFile ( sDocName, *m_pLastError ) )
		return false;

	if ( bInplace )
	{
		sphSeek ( tHit.GetFD(), 0, SEEK_SET );
		m_wrHitlist.SetFile ( tHit, pSharedOffset, *m_pLastError );
	} else
	{
		if ( !m_wrHitlist.OpenFile ( sHitName, *m_pLastError ) )
			return false;
	}

	if ( !m_wrSkiplist.OpenFile ( sSkipName, *m_pLastError ) )
		return false;

	// put dummy byte (otherwise offset would start from 0, first delta would be 0
	// and VLB encoding of offsets would fuckup)
	BYTE bDummy = 1;
	m_wrDoclist.PutBytes ( &bDummy, 1 );
	m_wrHitlist.PutBytes ( &bDummy, 1 );
	m_wrSkiplist.PutBytes ( &bDummy, 1 );
	return true;
}


void CSphHitBuilder::HitReset()
{
	m_tLastHit.m_tRowID = INVALID_ROWID;
	m_tLastHit.m_uWordID = 0;
	m_tLastHit.m_iWordPos = EMPTY_HIT;
	m_tLastHit.m_sKeyword = m_sLastKeyword;
	m_iPrevHitPos = 0;
	m_bGotFieldEnd = false;
}


// doclist entry format
// (with the new and shiny "inline hit" format, that is)
//
// zint docid_delta
// zint doc_hits
// if doc_hits==1:
// 		zint field_pos
// 		zint field_no
// else:
// 		zint field_mask
// 		zint hlist_offset_delta
//
// so 4 bytes/doc minimum
// avg 4-6 bytes/doc according to our tests


void CSphHitBuilder::DoclistBeginEntry ( RowID_t tRowid )
{
	assert ( m_iSkiplistBlockSize>0 );

	// build skiplist
	// that is, save decoder state and doclist position per every 128 documents
	if ( ( m_tWord.m_iDocs & ( m_iSkiplistBlockSize-1 ) )==0 )
	{
		SkiplistEntry_t & tBlock = m_dSkiplist.Add();
		tBlock.m_tBaseRowIDPlus1 = m_tLastHit.m_tRowID+1;
		tBlock.m_iOffset = m_wrDoclist.GetPos();
		tBlock.m_iBaseHitlistPos = m_iLastHitlistPos;
	}

	// begin doclist entry
	m_wrDoclist.ZipInt ( tRowid - m_tLastHit.m_tRowID );
}


void CSphHitBuilder::DoclistEndEntry ( Hitpos_t uLastPos )
{
	// end doclist entry
	if ( m_eHitFormat==SPH_HIT_FORMAT_INLINE )
	{
		bool bIgnoreHits =
			( m_eHitless==SPH_HITLESS_ALL ) ||
			( m_eHitless==SPH_HITLESS_SOME && ( m_tWord.m_iDocs & HITLESS_DOC_FLAG ) );

		// inline the only hit into doclist (unless it is completely discarded)
		// and finish doclist entry
		m_wrDoclist.ZipInt ( m_uLastDocHits );
		if ( m_uLastDocHits==1 && !bIgnoreHits )
		{
			m_wrHitlist.SeekTo ( m_iLastHitlistPos );
			m_wrDoclist.ZipInt ( uLastPos & 0x7FFFFF );
			m_wrDoclist.ZipInt ( uLastPos >> 23 );
			m_iLastHitlistPos -= m_iLastHitlistDelta;
			assert ( m_iLastHitlistPos>=0 );

		} else
		{
			m_wrDoclist.ZipInt ( m_dLastDocFields.GetMask32() );
			m_wrDoclist.ZipOffset ( m_iLastHitlistDelta );
		}
	} else // plain format - finish doclist entry
	{
		assert ( m_eHitFormat==SPH_HIT_FORMAT_PLAIN );
		m_wrDoclist.ZipOffset ( m_iLastHitlistDelta );
		m_wrDoclist.ZipInt ( m_dLastDocFields.GetMask32() );
		m_wrDoclist.ZipInt ( m_uLastDocHits );
	}
	m_dLastDocFields.UnsetAll();
	m_uLastDocHits = 0;

	// update keyword stats
	m_tWord.m_iDocs++;
}


void CSphHitBuilder::DoclistEndList ()
{
	assert ( m_iSkiplistBlockSize>0 );

	// emit eof marker
	m_wrDoclist.ZipInt ( 0 );

	// emit skiplist
	// OPTIMIZE? placing it after doclist means an extra seek on searching
	// however placing it before means some (longer) doclist data moves while indexing
	if ( m_tWord.m_iDocs>m_iSkiplistBlockSize )
	{
		assert ( m_dSkiplist.GetLength() );
		assert ( m_dSkiplist[0].m_iOffset==m_tWord.m_iDoclistOffset );
		assert ( m_dSkiplist[0].m_tBaseRowIDPlus1==0 );
		assert ( m_dSkiplist[0].m_iBaseHitlistPos==0 );

		m_tWord.m_iSkiplistOffset = m_wrSkiplist.GetPos();

		// delta coding, but with a couple of skiplist specific tricks
		// 1) first entry is omitted, it gets reconstructed from dict itself
		// both base values are zero, and offset equals doclist offset
		// 2) docids are at least SKIPLIST_BLOCK apart
		// doclist entries are at least 4*SKIPLIST_BLOCK bytes apart
		// so we additionally subtract that to improve delta coding
		// 3) zero deltas are allowed and *not* used as any markers,
		// as we know the exact skiplist entry count anyway
		SkiplistEntry_t tLast = m_dSkiplist[0];
		for ( int i=1; i<m_dSkiplist.GetLength(); i++ )
		{
			const SkiplistEntry_t & t = m_dSkiplist[i];
			assert ( t.m_tBaseRowIDPlus1 - tLast.m_tBaseRowIDPlus1>=(DWORD)m_iSkiplistBlockSize );
			assert ( t.m_iOffset - tLast.m_iOffset>=4*m_iSkiplistBlockSize );
			m_wrSkiplist.ZipInt ( t.m_tBaseRowIDPlus1 - tLast.m_tBaseRowIDPlus1 - m_iSkiplistBlockSize );
			m_wrSkiplist.ZipOffset ( t.m_iOffset - tLast.m_iOffset - 4*m_iSkiplistBlockSize );
			m_wrSkiplist.ZipOffset ( t.m_iBaseHitlistPos - tLast.m_iBaseHitlistPos );
			tLast = t;
		}
	}

	// in any event, reset skiplist
	m_dSkiplist.Resize ( 0 );
}

static int strcmpp (const char* l, const char* r)
{
	const char* szEmpty = "";
	if ( !l )
		l = szEmpty;
	if ( !r )
		r = szEmpty;
	return strcmp ( l, r );
}

void CSphHitBuilder::cidxHit ( CSphAggregateHit * pHit )
{
	assert (
		( pHit->m_uWordID!=0 && pHit->m_iWordPos!=EMPTY_HIT && pHit->m_tRowID!=INVALID_ROWID ) || // it's either ok hit
		( pHit->m_uWordID==0 && pHit->m_iWordPos==EMPTY_HIT ) ); // or "flush-hit"

	/////////////
	// next word
	/////////////

	const bool bNextWord = ( m_tLastHit.m_uWordID!=pHit->m_uWordID ||	( m_pDict->GetSettings().m_bWordDict && strcmpp ( (const char*)m_tLastHit.m_sKeyword, (const char*)pHit->m_sKeyword ) ) ); // OPTIMIZE?
	const bool bNextDoc = bNextWord || ( m_tLastHit.m_tRowID!=pHit->m_tRowID );

	if ( m_bGotFieldEnd && ( bNextWord || bNextDoc ) )
	{
		// writing hits only without duplicates
		assert ( HITMAN::GetPosWithField ( m_iPrevHitPos )!=HITMAN::GetPosWithField ( m_tLastHit.m_iWordPos ) );
		HITMAN::SetEndMarker ( &m_tLastHit.m_iWordPos );
		m_wrHitlist.ZipInt ( m_tLastHit.m_iWordPos - m_iPrevHitPos );
		m_bGotFieldEnd = false;
	}


	if ( bNextDoc )
	{
		// finish hitlist, if any
		Hitpos_t uLastPos = m_tLastHit.m_iWordPos;
		if ( m_tLastHit.m_iWordPos!=EMPTY_HIT )
		{
			m_wrHitlist.ZipInt ( 0 );
			m_tLastHit.m_iWordPos = EMPTY_HIT;
			m_iPrevHitPos = EMPTY_HIT;
		}

		// finish doclist entry, if any
		if ( m_tLastHit.m_tRowID!=INVALID_ROWID )
			DoclistEndEntry ( uLastPos );
	}

	if ( bNextWord )
	{
		// finish doclist, if any
		if ( m_tLastHit.m_tRowID!=INVALID_ROWID )
		{
			// emit end-of-doclist marker
			DoclistEndList ();

			// emit dict entry
			m_tWord.m_uWordID = m_tLastHit.m_uWordID;
			m_tWord.m_sKeyword = m_tLastHit.m_sKeyword;
			m_tWord.m_iDoclistLength = m_wrDoclist.GetPos() - m_tWord.m_iDoclistOffset;
			m_pDict->DictEntry ( m_tWord );

			// reset trackers
			m_tWord.m_iDocs = 0;
			m_tWord.m_iHits = 0;

			m_tLastHit.m_tRowID = INVALID_ROWID;
			m_iLastHitlistPos = 0;
		}

		// flush wordlist, if this is the end
		if ( pHit->m_iWordPos==EMPTY_HIT )
		{
			m_pDict->DictEndEntries ( m_wrDoclist.GetPos() );
			return;
		}
#ifndef NDEBUG
		assert ( pHit->m_uWordID > m_tLastHit.m_uWordID
			|| ( m_pDict->GetSettings().m_bWordDict &&
				pHit->m_uWordID==m_tLastHit.m_uWordID && strcmp ( (const char*)pHit->m_sKeyword, (const char*)m_tLastHit.m_sKeyword )>0 )
			|| m_bMerging );
#endif // usually assert excluded in release, but this is 'paranoid' clause
		m_tWord.m_iDoclistOffset = m_wrDoclist.GetPos();
		m_tLastHit.m_uWordID = pHit->m_uWordID;
		if ( m_pDict->GetSettings().m_bWordDict )
		{
			assert ( strlen ( (const char *)pHit->m_sKeyword )<sizeof(m_sLastKeyword)-1 );
			strncpy ( (char*)const_cast<BYTE*>(m_tLastHit.m_sKeyword), (const char*)pHit->m_sKeyword, sizeof(m_sLastKeyword) ); // OPTIMIZE?
		}
	}

	if ( bNextDoc )
	{
		// begin new doclist entry for new doc id
		assert ( m_tLastHit.m_tRowID==INVALID_ROWID || pHit->m_tRowID>m_tLastHit.m_tRowID );
		assert ( m_wrHitlist.GetPos()>=m_iLastHitlistPos );

		DoclistBeginEntry ( pHit->m_tRowID );
		m_iLastHitlistDelta = m_wrHitlist.GetPos() - m_iLastHitlistPos;

		m_tLastHit.m_tRowID = pHit->m_tRowID;
		m_iLastHitlistPos = m_wrHitlist.GetPos();
	}

	///////////
	// the hit
	///////////

	if ( !pHit->m_dFieldMask.TestAll(false) ) // merge aggregate hits into the current hit
	{
		int iHitCount = pHit->GetAggrCount();
		assert ( m_eHitless );
		assert ( iHitCount );
		assert ( !pHit->m_dFieldMask.TestAll(false) );

		m_uLastDocHits += iHitCount;
		for ( int i=0; i<FieldMask_t::SIZE; i++ )
			m_dLastDocFields[i] |= pHit->m_dFieldMask[i];
		m_tWord.m_iHits += iHitCount;

		if ( m_eHitless==SPH_HITLESS_SOME )
			m_tWord.m_iDocs |= HITLESS_DOC_FLAG;

	} else // handle normal hits
	{
		Hitpos_t iHitPosPure = HITMAN::GetPosWithField ( pHit->m_iWordPos );

		// skip any duplicates and keep only 1st position in place
		// duplicates are hit with same position: [N, N] [N, N | FIELDEND_MASK] [N | FIELDEND_MASK, N] [N | FIELDEND_MASK, N | FIELDEND_MASK]
		if ( iHitPosPure==m_tLastHit.m_iWordPos )
			return;

		// storing previous hit that might have a field end flag
		if ( m_bGotFieldEnd )
		{
			if ( HITMAN::GetField ( pHit->m_iWordPos )!=HITMAN::GetField ( m_tLastHit.m_iWordPos ) ) // is field end flag real?
				HITMAN::SetEndMarker ( &m_tLastHit.m_iWordPos );

			m_wrHitlist.ZipInt ( m_tLastHit.m_iWordPos - m_iPrevHitPos );
			m_bGotFieldEnd = false;
		}

		/* duplicate hits from duplicated documents
		... 0x03, 0x03 ... 
		... 0x8003, 0x8003 ... 
		... 1, 0x8003, 0x03 ... 
		... 1, 0x03, 0x8003 ... 
		... 1, 0x8003, 0x04 ... 
		... 1, 0x03, 0x8003, 0x8003 ... 
		... 1, 0x03, 0x8003, 0x03 ... 
		*/

		assert ( m_tLastHit.m_iWordPos < pHit->m_iWordPos );

		// add hit delta without field end marker
		// or postpone adding to hitlist till got another uniq hit
		if ( iHitPosPure==pHit->m_iWordPos )
		{
			m_wrHitlist.ZipInt ( pHit->m_iWordPos - m_tLastHit.m_iWordPos );
			m_tLastHit.m_iWordPos = pHit->m_iWordPos;
		} else
		{
			assert ( HITMAN::IsEnd ( pHit->m_iWordPos ) );
			m_bGotFieldEnd = true;
			m_iPrevHitPos = m_tLastHit.m_iWordPos;
			m_tLastHit.m_iWordPos = HITMAN::GetPosWithField ( pHit->m_iWordPos );
		}

		// update matched fields mask
		m_dLastDocFields.Set ( HITMAN::GetField ( pHit->m_iWordPos ) );

		m_uLastDocHits++;
		m_tWord.m_iHits++;
	}
}


static void ReadSchemaColumn ( CSphReader & rdInfo, CSphColumnInfo & tCol, DWORD uVersion )
{
	tCol.m_sName = rdInfo.GetString ();
	if ( tCol.m_sName.IsEmpty () )
		tCol.m_sName = "@emptyname";

	tCol.m_sName.ToLower ();
	tCol.m_eAttrType = (ESphAttr) rdInfo.GetDword (); // FIXME? check/fixup?

	rdInfo.GetDword (); // ignore rowitem
	tCol.m_tLocator.m_iBitOffset = rdInfo.GetDword ();
	tCol.m_tLocator.m_iBitCount = rdInfo.GetDword ();
	tCol.m_bPayload = ( rdInfo.GetByte()!=0 );

	if ( uVersion>=61 )
		tCol.m_uAttrFlags = rdInfo.GetDword();

	if ( uVersion>=63 )
		tCol.m_eEngine = (AttrEngine_e)rdInfo.GetDword();

	// WARNING! max version used here must be in sync with RtIndex_c::Prealloc
}


static void ReadSchemaField ( CSphReader & rdInfo, CSphColumnInfo & tCol, DWORD uVersion )
{
	if ( uVersion>=57  )
	{
		tCol.m_sName = rdInfo.GetString();
		tCol.m_uFieldFlags = rdInfo.GetDword();
		tCol.m_bPayload = !!rdInfo.GetByte();
	}
	else
		ReadSchemaColumn ( rdInfo, tCol, uVersion );

	if ( uVersion<59 )
		tCol.m_uFieldFlags |= CSphColumnInfo::FIELD_INDEXED;
}


void ReadSchema ( CSphReader & rdInfo, CSphSchema & tSchema, DWORD uVersion )
{
	tSchema.Reset ();

	int iNumFields = rdInfo.GetDword();
	for ( int i=0; i<iNumFields; i++ )
	{
		CSphColumnInfo tCol;
		ReadSchemaField ( rdInfo, tCol, uVersion );
		tSchema.AddField ( tCol );
	}

	int iNumAttrs = rdInfo.GetDword();
	for ( int i=0; i<iNumAttrs; i++ )
	{
		CSphColumnInfo tCol;
		ReadSchemaColumn ( rdInfo, tCol, uVersion );
		tSchema.AddAttr ( tCol, false );
	}
}

static void ReadLocatorJson ( bson::Bson_c tNode, CSphAttrLocator & tLoc )
{
	using namespace bson;
	tLoc.m_iBitOffset = (int) Int ( tNode.ChildByName ( "pos" ) );
	tLoc.m_iBitCount = (int) Int ( tNode.ChildByName ( "bits" ) );
}

static void ReadSchemaColumnJson ( bson::Bson_c tNode, CSphColumnInfo & tCol )
{
	using namespace bson;
	tCol.m_sName = String ( tNode.ChildByName ( "name" ), "@emptyname" );
	tCol.m_sName.ToLower();
	tCol.m_uAttrFlags = (DWORD)Int ( tNode.ChildByName ( "flags" ), CSphColumnInfo::ATTR_NONE );
	tCol.m_bPayload = Bool ( tNode.ChildByName ( "payload" ), false );
	tCol.m_eEngine = (AttrEngine_e)Int ( tNode.ChildByName ( "engine" ), (DWORD)AttrEngine_e::DEFAULT );
	tCol.m_eAttrType = (ESphAttr)Int ( tNode.ChildByName ( "type" ) );
	ReadLocatorJson ( tNode.ChildByName ("locator"), tCol.m_tLocator );
}


static void ReadSchemaFieldJson ( bson::Bson_c tNode, CSphColumnInfo & tCol )
{
	using namespace bson;
	tCol.m_sName = String ( tNode.ChildByName ( "name" ) );
	tCol.m_uFieldFlags = (DWORD)Int ( tNode.ChildByName ( "flags" ), CSphColumnInfo::FIELD_INDEXED );
	tCol.m_bPayload = Bool ( tNode.ChildByName ( "payload" ) );
}


void ReadSchemaJson ( bson::Bson_c tNode, CSphSchema & tSchema )
{
	using namespace bson;
	tSchema.Reset ();

	Bson_c ( tNode.ChildByName ( "fields" ) ).ForEach ( [&tSchema] ( const NodeHandle_t& tNode )
	{
		CSphColumnInfo tCol;
		ReadSchemaFieldJson ( tNode, tCol );
		tSchema.AddField ( tCol );
	} );

	Bson_c ( tNode.ChildByName ( "attributes" ) ).ForEach ( [&tSchema] ( const NodeHandle_t& tNode )
	{
		CSphColumnInfo tCol;
		ReadSchemaColumnJson ( tNode, tCol );
		tSchema.AddAttr ( tCol, false );
	} );
}

static void WriteSchemaField ( CSphWriter & fdInfo, const CSphColumnInfo & tCol )
{
	fdInfo.PutString ( tCol.m_sName );
	fdInfo.PutDword ( tCol.m_uFieldFlags );
	fdInfo.PutByte ( tCol.m_bPayload );
}


static void WriteSchemaColumn ( CSphWriter & fdInfo, const CSphColumnInfo & tCol )
{
	fdInfo.PutString( tCol.m_sName );
	fdInfo.PutDword ( tCol.m_eAttrType );

	fdInfo.PutDword ( tCol.m_tLocator.CalcRowitem() ); // for backwards compatibility
	fdInfo.PutDword ( tCol.m_tLocator.m_iBitOffset );
	fdInfo.PutDword ( tCol.m_tLocator.m_iBitCount );

	fdInfo.PutByte ( tCol.m_bPayload );
	fdInfo.PutDword ( tCol.m_uAttrFlags );
	fdInfo.PutDword ( (DWORD)tCol.m_eEngine );
}


void WriteSchema ( CSphWriter & fdInfo, const CSphSchema & tSchema )
{
	fdInfo.PutDword ( tSchema.GetFieldsCount() );
	for ( int i=0; i<tSchema.GetFieldsCount(); i++ )
		WriteSchemaField ( fdInfo, tSchema.GetField(i) );

	fdInfo.PutDword ( tSchema.GetAttrsCount() );
	for ( int i=0; i<tSchema.GetAttrsCount(); i++ )
		WriteSchemaColumn ( fdInfo, tSchema.GetAttr(i) );
}

void operator<< ( JsonEscapedBuilder& tOut, const CSphAttrLocator& tLoc )
{
	auto _ = tOut.Object();
	tOut.NamedVal ( "pos", tLoc.m_iBitOffset );
	tOut.NamedVal ( "bits", tLoc.m_iBitCount );
}

namespace {

void DumpFieldToJson ( JsonEscapedBuilder& tOut, const CSphColumnInfo& tCol )
{
	auto _ = tOut.Object();
	tOut.NamedString ( "name", tCol.m_sName );
	tOut.NamedValNonDefault ( "flags", tCol.m_uFieldFlags, (DWORD)CSphColumnInfo::FIELD_INDEXED );
	tOut.NamedValNonDefault ( "payload", tCol.m_bPayload, false );
}

void DumpAttrToJson ( JsonEscapedBuilder& tOut, const CSphColumnInfo& tCol )
{
	auto _ = tOut.Object();
	tOut.NamedString ( "name", tCol.m_sName );
	tOut.NamedValNonDefault ( "flags", tCol.m_uAttrFlags, (DWORD)CSphColumnInfo::ATTR_NONE );
	tOut.NamedValNonDefault ( "payload", tCol.m_bPayload, false );
	tOut.NamedValNonDefault ( "engine", (DWORD)tCol.m_eEngine, (DWORD)AttrEngine_e::DEFAULT );
	tOut.NamedVal ( "type", tCol.m_eAttrType );
	tOut.NamedVal ( "locator", tCol.m_tLocator );
}
} // namespace

void operator<< ( JsonEscapedBuilder& tOut, const CSphSchema& tSchema )
{
	auto _ = tOut.ObjectW();
	if ( tSchema.GetFieldsCount() > 0 )
	{
		tOut.Named ( "fields" );
		auto _ = tOut.ArrayW();
		for ( int i = 0; i < tSchema.GetFieldsCount(); ++i )
			DumpFieldToJson ( tOut, tSchema.GetField ( i ) );
	}
	if ( tSchema.GetAttrsCount() > 0 )
	{
		tOut.Named ( "attributes" );
		auto _ = tOut.ArrayW();
		for ( int i = 0; i < tSchema.GetAttrsCount(); ++i )
			DumpAttrToJson ( tOut, tSchema.GetAttr ( i ) );
	}
}


void SaveIndexSettings ( CSphWriter & tWriter, const CSphIndexSettings & tSettings )
{
	tWriter.PutDword ( tSettings.RawMinPrefixLen() );
	tWriter.PutDword ( tSettings.m_iMinInfixLen );
	tWriter.PutDword ( tSettings.m_iMaxSubstringLen );
	tWriter.PutByte ( tSettings.m_bHtmlStrip ? 1 : 0 );
	tWriter.PutString ( tSettings.m_sHtmlIndexAttrs.cstr () );
	tWriter.PutString ( tSettings.m_sHtmlRemoveElements.cstr () );
	tWriter.PutByte ( tSettings.m_bIndexExactWords ? 1 : 0 );
	tWriter.PutDword ( tSettings.m_eHitless );
	tWriter.PutDword ( tSettings.m_eHitFormat );
	tWriter.PutByte ( tSettings.m_bIndexSP );
	tWriter.PutString ( tSettings.m_sZones );
	tWriter.PutDword ( tSettings.m_iBoundaryStep );
	tWriter.PutDword ( tSettings.m_iStopwordStep );
	tWriter.PutDword ( tSettings.m_iOvershortStep );
	tWriter.PutDword ( tSettings.m_iEmbeddedLimit );
	tWriter.PutByte ( tSettings.m_eBigramIndex );
	tWriter.PutString ( tSettings.m_sBigramWords );
	tWriter.PutByte ( tSettings.m_bIndexFieldLens );
	tWriter.PutByte ( tSettings.m_ePreprocessor==Preprocessor_e::ICU ? 1 : 0 );
	tWriter.PutString("");	// was: RLP context
	tWriter.PutString ( tSettings.m_sIndexTokenFilter );
	tWriter.PutOffset ( tSettings.m_tBlobUpdateSpace );
	tWriter.PutDword ( tSettings.m_iSkiplistBlockSize );
	tWriter.PutString ( tSettings.m_sHitlessFiles );
	tWriter.PutDword ( (DWORD)tSettings.m_eEngine );
}

void operator<< ( JsonEscapedBuilder& tOut, const CSphIndexSettings& tSettings )
{
	auto _ = tOut.ObjectW();
	tOut.NamedValNonDefault ( "min_prefix_len", tSettings.RawMinPrefixLen() );
	tOut.NamedValNonDefault ( "min_infix_len", tSettings.m_iMinInfixLen );
	tOut.NamedValNonDefault ( "max_substring_len", tSettings.m_iMaxSubstringLen );
	tOut.NamedValNonDefault ( "strip_html", tSettings.m_bHtmlStrip, false );
	tOut.NamedStringNonEmpty ( "html_index_attrs", tSettings.m_sHtmlIndexAttrs );
	tOut.NamedStringNonEmpty ( "html_remove_elements", tSettings.m_sHtmlRemoveElements );
	tOut.NamedValNonDefault ( "index_exact_words", tSettings.m_bIndexExactWords, false );
	tOut.NamedValNonDefault ( "hitless", tSettings.m_eHitless, SPH_HITLESS_NONE );
	tOut.NamedValNonDefault ( "hit_format", tSettings.m_eHitFormat, SPH_HIT_FORMAT_PLAIN );
	tOut.NamedValNonDefault ( "index_sp", tSettings.m_bIndexSP, false );
	tOut.NamedStringNonEmpty ( "zones", tSettings.m_sZones );
	tOut.NamedValNonDefault ( "boundary_step", tSettings.m_iBoundaryStep );
	tOut.NamedValNonDefault ( "stopword_step", tSettings.m_iStopwordStep, 1 );
	tOut.NamedValNonDefault ( "overshort_step", tSettings.m_iOvershortStep, 1 );
	tOut.NamedValNonDefault ( "embedded_limit", tSettings.m_iEmbeddedLimit );
	tOut.NamedValNonDefault ( "bigram_index", tSettings.m_eBigramIndex, SPH_BIGRAM_NONE );
	tOut.NamedStringNonEmpty ( "bigram_words", tSettings.m_sBigramWords );
	tOut.NamedValNonDefault ( "index_field_lens", tSettings.m_bIndexFieldLens, false );
	tOut.NamedValNonDefault ( "icu", (DWORD)tSettings.m_ePreprocessor, (DWORD)Preprocessor_e::NONE );
	tOut.NamedStringNonEmpty ( "index_token_filter", tSettings.m_sIndexTokenFilter );
	tOut.NamedValNonDefault ( "blob_update_space", tSettings.m_tBlobUpdateSpace );
	tOut.NamedValNonDefault ( "skiplist_block_size", tSettings.m_iSkiplistBlockSize, 32 );
	tOut.NamedStringNonEmpty ( "hitless_files", tSettings.m_sHitlessFiles );
	tOut.NamedValNonDefault ( "engine", (DWORD)tSettings.m_eEngine, (DWORD)AttrEngine_e::DEFAULT );
}

void IndexWriteHeader ( const BuildHeader_t & tBuildHeader, const WriteHeader_t & tWriteHeader, JsonEscapedBuilder& sJson, bool bForceWordDict, bool SkipEmbeddDict )
{
	auto _ = sJson.ObjectW();

	// human-readable sugar
	sJson.NamedString ( "meta_created_time_utc", sphCurrentUtcTime() );

	// version
	sJson.NamedVal ( "index_format_version", INDEX_FORMAT_VERSION );

	// index stats - json (put here to be similar with .meta)
	sJson.NamedValNonDefault ( "total_documents", tBuildHeader.m_iTotalDocuments );
	sJson.NamedValNonDefault ( "total_bytes", tBuildHeader.m_iTotalBytes );

	// schema
	sJson.NamedVal ( "schema", *tWriteHeader.m_pSchema );

	// index settings
	sJson.NamedVal ( "index_settings", *tWriteHeader.m_pSettings );

	// tokenizer info
	assert ( tWriteHeader.m_pTokenizer );
	sJson.Named ( "tokenizer_settings" );
	SaveTokenizerSettings ( sJson, tWriteHeader.m_pTokenizer, tWriteHeader.m_pSettings->m_iEmbeddedLimit );

	// dictionary info
	assert ( tWriteHeader.m_pDict );
	sJson.Named ( "dictionary_settings" );
	SaveDictionarySettings ( sJson, tWriteHeader.m_pDict, bForceWordDict, SkipEmbeddDict ? 0 : tWriteHeader.m_pSettings->m_iEmbeddedLimit );

	// wordlist checkpoints - json
	sJson.NamedValNonDefault ( "dict_checkpoints_offset", tBuildHeader.m_iDictCheckpointsOffset );
	sJson.NamedValNonDefault ( "dict_checkpoints", tBuildHeader.m_iDictCheckpoints );
	sJson.NamedValNonDefault ( "infix_codepoint_bytes", tBuildHeader.m_iInfixCodepointBytes );
	sJson.NamedValNonDefault ( "infix_blocks_offset", tBuildHeader.m_iInfixBlocksOffset );
	sJson.NamedValNonDefault ( "infix_block_words_size", tBuildHeader.m_iInfixBlocksWordsSize );

	sJson.NamedValNonDefault ( "docinfo", tBuildHeader.m_iDocinfo );
	sJson.NamedValNonDefault ( "docinfo_index", tBuildHeader.m_iDocinfoIndex );
	sJson.NamedValNonDefault ( "min_max_index", tBuildHeader.m_iMinMaxIndex );

	// field filter info
	CSphFieldFilterSettings tFieldFilterSettings;
	if ( tWriteHeader.m_pFieldFilter.Ptr() )
	{
		tWriteHeader.m_pFieldFilter->GetSettings ( tFieldFilterSettings );
		sJson.NamedVal ( "field_filter_settings", tFieldFilterSettings );
	}

	// average field lengths
	if ( tWriteHeader.m_pSettings->m_bIndexFieldLens )
	{
		sJson.Named ( "index_fields_lens" );
		auto _ = sJson.Array();
		for ( int i=0; i < tWriteHeader.m_pSchema->GetFieldsCount(); ++i )
		{
			sJson << tWriteHeader.m_pFieldLens[i];
		}
	}
}


bool IndexBuildDone ( const BuildHeader_t & tBuildHeader, const WriteHeader_t & tWriteHeader, const CSphString & sFileName, CSphString & sError )
{
	JsonEscapedBuilder sJson;
	IndexWriteHeader ( tBuildHeader, tWriteHeader, sJson, false );

	CSphWriter wrHeaderJson;
	if ( wrHeaderJson.OpenFile ( sFileName, sError ) )
	{
		wrHeaderJson.PutString ( (Str_t)sJson );
		wrHeaderJson.CloseFile();
		assert ( bson::ValidateJson ( sJson.cstr(), &sError ) );
		return true;
	}

	sphWarning ( "failed to serialize header to json: %s", sError.cstr() );
	return false;
}


bool CSphHitBuilder::cidxDone ( int iMemLimit, int & iMinInfixLen, int iMaxCodepointLen, DictHeader_t * pDictHeader )
{
	assert ( pDictHeader );

	if ( m_bGotFieldEnd )
	{
		HITMAN::SetEndMarker ( &m_tLastHit.m_iWordPos );
		m_wrHitlist.ZipInt ( m_tLastHit.m_iWordPos - m_iPrevHitPos );
		m_bGotFieldEnd = false;
	}

	// finalize dictionary
	// in dict=crc mode, just flushes wordlist checkpoints
	// in dict=keyword mode, also creates infix index, if needed

	if ( iMinInfixLen>0 && m_pDict->GetSettings().m_bWordDict )
	{
		pDictHeader->m_iInfixCodepointBytes = iMaxCodepointLen;
		if ( iMinInfixLen==1 )
		{
			sphWarn ( "min_infix_len must be greater than 1, changed to 2" );
			iMinInfixLen = 2;
		}
	}

	if ( !m_pDict->DictEnd ( pDictHeader, iMemLimit, *m_pLastError ) )
		return false;

	// close all data files
	m_wrDoclist.CloseFile ();
	m_wrHitlist.CloseFile ( true );
	m_wrSkiplist.CloseFile ();
	return !IsError();
}


inline int encodeVLB ( BYTE * buf, DWORD v )
{
	register BYTE b;
	register int n = 0;

	do
	{
		b = (BYTE)(v & 0x7f);
		v >>= 7;
		if ( v )
			b |= 0x80;
		*buf++ = b;
		n++;
	} while ( v );
	return n;
}


inline int encodeKeyword ( BYTE * pBuf, const char * pKeyword )
{
	auto iLen = (int) strlen ( pKeyword ); // OPTIMIZE! remove this and memcpy and check if thats faster
	assert ( iLen>0 && iLen<128 ); // so that ReadVLB()

	*pBuf = (BYTE) iLen;
	memcpy ( pBuf+1, pKeyword, iLen );
	return 1+iLen;
}


int CSphHitBuilder::cidxWriteRawVLB ( int fd, CSphWordHit * pHit, int iHits )
{
	assert ( pHit );
	assert ( iHits>0 );

	///////////////////////////////////////
	// encode through a small write buffer
	///////////////////////////////////////

	BYTE *pBuf, *maxP;
	int n = 0, w;
	SphWordID_t d1, l1 = 0;
	RowID_t d2, l2 = (RowID_t)-1; // rowids start from 0 and we can't have delta=0
	DWORD d3, l3 = 0; // !COMMIT must be wide enough

	int iGap = (int)Max ( 16*sizeof(DWORD) + ( m_pDict->GetSettings().m_bWordDict ? MAX_KEYWORD_BYTES : 0 ), 128u );
	pBuf = m_dWriteBuffer.Begin();
	maxP = m_dWriteBuffer.Begin() + m_dWriteBuffer.GetLength() - iGap;

	// hit aggregation state
	DWORD uHitCount = 0;
	DWORD uHitFieldMask = 0;

	const int iPositionShift = m_eHitless==SPH_HITLESS_SOME ? 1 : 0;

	while ( iHits-- )
	{
		// calc deltas
		d1 = pHit->m_uWordID - l1;
		d2 = pHit->m_tRowID - l2;
		d3 = pHit->m_uWordPos - l3;

		// ignore duplicate hits
		if ( d1==0 && d2==0 && d3==0 ) // OPTIMIZE? check if ( 0==(d1|d2|d3) ) is faster
		{
			pHit++;
			continue;
		}

		// checks below are intended handle several "fun" cases
		//
		// case 1, duplicate documents (same docid), different field contents, but ending with
		// the same keyword, ending up in multiple field end markers within the same keyword
		// eg. [foo] in positions {1, 0x800005} in 1st dupe, {3, 0x800007} in 2nd dupe
		//
		// case 2, blended token in the field end, duplicate parts, different positions (as expected)
		// for those parts but still multiple field end markers, eg. [U.S.S.R.] in the end of field

		// replacement of hit itself by field-end form
		if ( d1==0 && d2==0 && HITMAN::GetPosWithField ( pHit->m_uWordPos )==HITMAN::GetPosWithField ( l3 ) )
		{
			l3 = pHit->m_uWordPos;
			pHit++;
			continue;
		}

		// reset field-end inside token stream due of document duplicates
		if ( d1==0 && d2==0 && HITMAN::IsEnd ( l3 ) && HITMAN::GetField ( pHit->m_uWordPos )==HITMAN::GetField ( l3 ) )
		{
			l3 = HITMAN::GetPosWithField ( l3 );
			d3 = HITMAN::GetPosWithField ( pHit->m_uWordPos ) - l3;

			if ( d3==0 )
			{
				pHit++;
				continue;
			}
		}

		// non-zero delta restarts all the fields after it
		// because their deltas might now be negative
		if ( d1 ) d2 = pHit->m_tRowID+1;
		if ( d2 ) d3 = pHit->m_uWordPos;

		// when we moved to the next word or document
		bool bFlushed = false;
		if ( d1 || d2 )
		{
			// flush previous aggregate hit
			if ( uHitCount )
			{
				// we either skip all hits or the high bit must be available for marking
				// failing that, we can't produce a consistent index
				assert ( m_eHitless!=SPH_HITLESS_NONE );
				assert ( m_eHitless==SPH_HITLESS_ALL || !( uHitCount & 0x80000000UL ) );

				if ( m_eHitless!=SPH_HITLESS_ALL )
					uHitCount = ( uHitCount << 1 ) | 1;
				pBuf += encodeVLB ( pBuf, uHitCount );
				pBuf += encodeVLB ( pBuf, uHitFieldMask );
				assert ( pBuf<m_dWriteBuffer.Begin() + m_dWriteBuffer.GetLength() );

				uHitCount = 0;
				uHitFieldMask = 0;

				bFlushed = true;
			}

			// start aggregating if we're skipping all hits or this word is in a list of ignored words
			if ( ( m_eHitless==SPH_HITLESS_ALL ) ||
				( m_eHitless==SPH_HITLESS_SOME && m_dHitlessWords.BinarySearch ( pHit->m_uWordID ) ) )
			{
				uHitCount = 1;
				uHitFieldMask |= 1 << HITMAN::GetField ( pHit->m_uWordPos );
			}

		} else if ( uHitCount ) // next hit for the same word/doc pair, update state if we need it
		{
			uHitCount++;
			uHitFieldMask |= 1 << HITMAN::GetField ( pHit->m_uWordPos );
		}

		// encode enough restart markers
		if ( d1 ) pBuf += encodeVLB ( pBuf, 0 );
		if ( d2 && !bFlushed ) pBuf += encodeVLB ( pBuf, 0 );

		assert ( pBuf<m_dWriteBuffer.Begin() + m_dWriteBuffer.GetLength() );

		// encode deltas

		// encode keyword
		if ( d1 )
		{
			if ( m_pDict->GetSettings().m_bWordDict )
				pBuf += encodeKeyword ( pBuf, m_pDict->HitblockGetKeyword ( pHit->m_uWordID ) ); // keyword itself in case of keywords dict
			else
				pBuf += sphEncodeVLB8 ( pBuf, d1 ); // delta in case of CRC dict

			assert ( pBuf<m_dWriteBuffer.Begin() + m_dWriteBuffer.GetLength() );
		}

		// encode docid delta
		if ( d2 )
		{
			pBuf += sphEncodeVLB8 ( pBuf, d2 );
			assert ( pBuf<m_dWriteBuffer.Begin() + m_dWriteBuffer.GetLength() );
		}

		assert ( d3 );
		if ( !uHitCount ) // encode position delta, unless accumulating hits
		{
			pBuf += encodeVLB ( pBuf, d3 << iPositionShift );
			assert ( pBuf<m_dWriteBuffer.Begin() + m_dWriteBuffer.GetLength() );
		}

		// update current state
		l1 = pHit->m_uWordID;
		l2 = pHit->m_tRowID;
		l3 = pHit->m_uWordPos;

		pHit++;

		if ( pBuf>maxP )
		{
			w = (int)(pBuf - m_dWriteBuffer.Begin());
			assert ( w<m_dWriteBuffer.GetLength() );
			if ( !sphWriteThrottled ( fd, m_dWriteBuffer.Begin(), w, "raw_hits", *m_pLastError ) )
				return -1;
			n += w;
			pBuf = m_dWriteBuffer.Begin();
		}
	}

	// flush last aggregate
	if ( uHitCount )
	{
		assert ( m_eHitless!=SPH_HITLESS_NONE );
		assert ( m_eHitless==SPH_HITLESS_ALL || !( uHitCount & 0x80000000UL ) );

		if ( m_eHitless!=SPH_HITLESS_ALL )
			uHitCount = ( uHitCount << 1 ) | 1;
		pBuf += encodeVLB ( pBuf, uHitCount );
		pBuf += encodeVLB ( pBuf, uHitFieldMask );

		assert ( pBuf<m_dWriteBuffer.Begin() + m_dWriteBuffer.GetLength() );
	}

	pBuf += encodeVLB ( pBuf, 0 );
	pBuf += encodeVLB ( pBuf, 0 );
	pBuf += encodeVLB ( pBuf, 0 );
	assert ( pBuf<m_dWriteBuffer.Begin() + m_dWriteBuffer.GetLength() );
	w = (int)(pBuf - m_dWriteBuffer.Begin());
	assert ( w<m_dWriteBuffer.GetLength() );
	if ( !sphWriteThrottled ( fd, m_dWriteBuffer.Begin(), w, "raw_hits", *m_pLastError ) )
		return -1;
	n += w;

	return n;
}

/////////////////////////////////////////////////////////////////////////////

// OPTIMIZE?
inline bool SPH_CMPAGGRHIT_LESS ( const CSphAggregateHit & a, const CSphAggregateHit & b )
{
	if ( a.m_uWordID < b.m_uWordID )
		return true;

	if ( a.m_uWordID > b.m_uWordID )
		return false;

	if ( a.m_sKeyword )
	{
		int iCmp = strcmp ( (const char*)a.m_sKeyword, (const char*)b.m_sKeyword ); // OPTIMIZE?
		if ( iCmp!=0 )
			return ( iCmp<0 );
	}

	return
		( a.m_tRowID < b.m_tRowID ) ||
		( a.m_tRowID==b.m_tRowID && HITMAN::GetPosWithField ( a.m_iWordPos )<HITMAN::GetPosWithField ( b.m_iWordPos ) );
}


/// hit priority queue entry
struct CSphHitQueueEntry : public CSphAggregateHit
{
	int m_iBin;
};


/// hit priority queue
struct CSphHitQueue
{
public:
	CSphHitQueueEntry *		m_pData;
	int						m_iSize;
	int						m_iUsed;

public:
	/// create queue
	explicit CSphHitQueue ( int iSize )
	{
		assert ( iSize>0 );
		m_iSize = iSize;
		m_iUsed = 0;
		m_pData = new CSphHitQueueEntry [ iSize ];
	}

	/// destroy queue
	~CSphHitQueue ()
	{
		SafeDeleteArray ( m_pData );
	}

	/// add entry to the queue
	void Push ( CSphAggregateHit & tHit, int iBin )
	{
		// check for overflow and do add
		assert ( m_iUsed<m_iSize );
		auto & tEntry = m_pData[m_iUsed];

		tEntry.m_tRowID = tHit.m_tRowID;
		tEntry.m_uWordID = tHit.m_uWordID;
		tEntry.m_sKeyword = tHit.m_sKeyword; // bin must hold the actual data for the queue
		tEntry.m_iWordPos = tHit.m_iWordPos;
		tEntry.m_dFieldMask = tHit.m_dFieldMask;
		tEntry.m_iBin = iBin;

		int iEntry = m_iUsed++;

		// sift up if needed
		while ( iEntry )
		{
			int iParent = ( iEntry-1 ) >> 1;
			if ( SPH_CMPAGGRHIT_LESS ( m_pData[iEntry], m_pData[iParent] ) )
			{
				// entry is less than parent, should float to the top
				Swap ( m_pData[iEntry], m_pData[iParent] );
				iEntry = iParent;
			} else
				break;
		}
	}

	/// remove root (ie. top priority) entry
	void Pop ()
	{
		assert ( m_iUsed );
		if ( !(--m_iUsed) ) // empty queue? just return
			return;

		// make the last entry my new root
		m_pData[0] = m_pData[m_iUsed];

		// sift down if needed
		int iEntry = 0;
		while (true)
		{
			// select child
			int iChild = (iEntry<<1) + 1;
			if ( iChild>=m_iUsed )
				break;

			// select smallest child
			if ( iChild+1<m_iUsed )
				if ( SPH_CMPAGGRHIT_LESS ( m_pData[iChild+1], m_pData[iChild] ) )
					iChild++;

			// if smallest child is less than entry, do float it to the top
			if ( SPH_CMPAGGRHIT_LESS ( m_pData[iChild], m_pData[iEntry] ) )
			{
				Swap ( m_pData[iChild], m_pData[iEntry] );
				iEntry = iChild;
				continue;
			}

			break;
		}
	}
};

static const int MIN_KEYWORDS_DICT	= 4*1048576;	// FIXME! ideally must be in sync with impl (ENTRY_CHUNKS, KEYWORD_CHUNKS)

/////////////////////////////////////////////////////////////////////////////

bool CSphIndex_VLN::RelocateBlock ( int iFile, BYTE * pBuffer, int iRelocationSize,
	SphOffset_t * pFileSize, CSphBin & dMinBin, SphOffset_t * pSharedOffset )
{
	assert ( pBuffer && pFileSize && pSharedOffset );

	SphOffset_t iBlockStart = dMinBin.m_iFilePos;
	SphOffset_t iBlockLeft = dMinBin.m_iFileLeft;

	ESphBinRead eRes = dMinBin.Precache ();
	switch ( eRes )
	{
	case BIN_PRECACHE_OK:
		return true;
	case BIN_READ_ERROR:
		m_sLastError = "block relocation: preread error";
		return false;
	default:
		break;
	}

	int nTransfers = (int)( ( iBlockLeft+iRelocationSize-1) / iRelocationSize );

	SphOffset_t uTotalRead = 0;
	SphOffset_t uNewBlockStart = *pFileSize;

	for ( int i = 0; i < nTransfers; i++ )
	{
		if ( !SeekAndWarn ( iFile, iBlockStart + uTotalRead, "block relocation" ))
			return false;

		int iToRead = i==nTransfers-1 ? (int)( iBlockLeft % iRelocationSize ) : iRelocationSize;
		size_t iRead = sphReadThrottled ( iFile, pBuffer, iToRead );
		if ( iRead!=size_t(iToRead) )
		{
			m_sLastError.SetSprintf ( "block relocation: read error (%d of %d bytes read): %s", (int)iRead, iToRead, strerrorm(errno) );
			return false;
		}

		if ( !SeekAndWarn ( iFile, *pFileSize, "block relocation" ))
			return false;

		uTotalRead += iToRead;

		if ( !sphWriteThrottled ( iFile, pBuffer, iToRead, "block relocation", m_sLastError ) )
			return false;

		*pFileSize += iToRead;
	}

	assert ( uTotalRead==iBlockLeft );

	// update block pointers
	dMinBin.m_iFilePos = uNewBlockStart;
	*pSharedOffset = *pFileSize;

	return true;
}


bool LoadHitlessWords ( const CSphString & sHitlessFiles, ISphTokenizer * pTok, CSphDict * pDict, CSphVector<SphWordID_t> & dHitlessWords, CSphString & sError )
{
	assert ( dHitlessWords.GetLength()==0 );

	if ( sHitlessFiles.IsEmpty() )
		return true;

	StrVec_t dFiles;
	sphSplit ( dFiles, sHitlessFiles.cstr(), ", " );

	for ( const CSphString & sFilename : dFiles )
	{
		CSphAutofile tFile ( sFilename.cstr(), SPH_O_READ, sError );
		if ( tFile.GetFD()==-1 )
			return false;

		CSphVector<BYTE> dBuffer ( (int)tFile.GetSize() );
		if ( !tFile.Read ( &dBuffer[0], dBuffer.GetLength(), sError ) )
			return false;

		// FIXME!!! dict=keywords + hitless_words=some
		pTok->SetBuffer ( &dBuffer[0], dBuffer.GetLength() );
		while ( BYTE * sToken = pTok->GetToken() )
			dHitlessWords.Add ( pDict->GetWordID ( sToken ) );
	}

	dHitlessWords.Uniq();
	return true;
}


class DeleteOnFail_c : public ISphNoncopyable
{
public:
	DeleteOnFail_c() : m_bShitHappened ( true )
	{}
	~DeleteOnFail_c()
	{
		if ( m_bShitHappened )
		{
			ARRAY_FOREACH ( i, m_dWriters )
				m_dWriters[i]->UnlinkFile();

			ARRAY_FOREACH ( i, m_dAutofiles )
				m_dAutofiles[i]->SetTemporary();
		}
	}
	void AddWriter ( CSphWriter * pWr )
	{
		if ( pWr )
			m_dWriters.Add ( pWr );
	}
	void AddAutofile ( CSphAutofile * pAf )
	{
		if ( pAf )
			m_dAutofiles.Add ( pAf );
	}
	void AllIsDone()
	{
		m_bShitHappened = false;
	}
private:
	bool	m_bShitHappened;
	CSphVector<CSphWriter*> m_dWriters;
	CSphVector<CSphAutofile*> m_dAutofiles;
};


bool CSphIndex_VLN::CollectQueryMvas ( const CSphVector<CSphSource*> & dSources, QueryMvaContainer_c & tMvaContainer )
{
	CSphBitvec dQueryMvas ( m_tSchema.GetAttrsCount() );
	for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
		if ( tAttr.m_eSrc!=SPH_ATTRSRC_FIELD && ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET || tAttr.m_eAttrType==SPH_ATTR_INT64SET ) )
			dQueryMvas.BitSet(i);
	}

	if ( !dQueryMvas.BitCount() )
		return true;

	assert ( !tMvaContainer.m_tContainer.GetLength() );
	tMvaContainer.m_tContainer.Resize ( m_tSchema.GetAttrsCount() );
	for ( auto & i : tMvaContainer.m_tContainer )
		i = nullptr;

	for ( auto & pSource : dSources )
	{
		assert ( pSource );
		if ( !pSource->Connect ( m_sLastError ) )
			return false;

		for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
		{
			if ( !dQueryMvas.BitGet(i) )
				continue;

			auto * & pHash = tMvaContainer.m_tContainer[i];
			if ( !pHash )
				pHash = new OpenHash_T<CSphVector<int64_t>, int64_t, HashFunc_Int64_t>;

			if ( !pSource->IterateMultivaluedStart ( i, m_sLastError ) )
				return false;

			int64_t iDocID;
			int64_t iMvaValue;
			while ( pSource->IterateMultivaluedNext ( iDocID, iMvaValue) )
			{
				auto & tMva = pHash->Acquire ( iDocID );
				tMva.Add ( iMvaValue );
			}
		}

		pSource->Disconnect ();
	}

	return true;
}

struct Mva32Uniq_fn
{
	bool IsLess ( const uint64_t & iA, const uint64_t & iB ) const
	{
		DWORD uA = (DWORD)iA;
		DWORD uB = (DWORD)iB;

		return uA<uB;
	}

	bool IsEq ( const uint64_t & iA, const uint64_t & iB ) const
	{
		DWORD uA = (DWORD)iA;
		DWORD uB = (DWORD)iB;
		return uA==uB;
	}
};


static void SortMva ( CSphVector<int64_t> & tMva, bool bMva32 )
{
	if ( !bMva32 )
	{
		tMva.Uniq();
		return;
	}

	tMva.Sort ( Mva32Uniq_fn() );
	int iLeft = sphUniq ( tMva.Begin(), tMva.GetLength(), Mva32Uniq_fn() );
	tMva.Resize ( iLeft );
}


static const CSphVector<int64_t> * FetchMVA ( DocID_t tDocId, int iAttr, const CSphColumnInfo & tAttr, QueryMvaContainer_c & tMvaContainer, AttrSource_i & tSource, bool bForceSource )
{
	CSphVector<int64_t> * pMva = nullptr;

	if ( tAttr.m_eSrc==SPH_ATTRSRC_FIELD || bForceSource )
		pMva = tSource.GetFieldMVA(iAttr);
	else
	{
		assert ( tMvaContainer.m_tContainer[iAttr] );
		pMva = tMvaContainer.m_tContainer[iAttr]->Find(tDocId);
	}

	if ( pMva )
		SortMva ( *pMva, ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET ) );

	return pMva;
}


bool CSphIndex_VLN::Build_StoreBlobAttrs ( DocID_t tDocId, SphOffset_t & tOffset, BlobRowBuilder_i & tBlobRowBuilder, QueryMvaContainer_c & tMvaContainer, AttrSource_i & tSource, bool bForceSource )
{
	CSphString sError;
	int iBlobAttr = 0;

	for ( int i=0; i < m_tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);

		if ( !sphIsBlobAttr(tAttr) )
			continue;

		bool bOk = true;
		bool bFatal = false;

		switch ( tAttr.m_eAttrType )
		{
		case SPH_ATTR_UINT32SET:
		case SPH_ATTR_INT64SET:
			{
				const CSphVector<int64_t> * pMva = FetchMVA ( tDocId, i, tAttr, tMvaContainer, tSource, bForceSource );
				bOk = tBlobRowBuilder.SetAttr ( iBlobAttr++, pMva ? (const BYTE*)(pMva->Begin()) : nullptr, pMva ? pMva->GetLength()*sizeof(int64_t) : 0, sError );
			}
			break;

		case SPH_ATTR_STRING:
		case SPH_ATTR_JSON:
			{
				const CSphString & sStrAttr = tSource.GetStrAttr(i);
				bOk = tBlobRowBuilder.SetAttr ( iBlobAttr++, (const BYTE*)sStrAttr.cstr(), sStrAttr.Length(), sError );
				if ( !bOk )
					bFatal = tAttr.m_eAttrType==SPH_ATTR_JSON && g_bJsonStrict;
			}
			break;

		default:
			break;
		}

		if ( !bOk )
		{
			sError.SetSprintf ( "document " INT64_FMT ", attribute %s: %s", tDocId, tAttr.m_sName.cstr(), sError.cstr() );
			if ( bFatal )
			{
				m_sLastError = sError;
				return false;
			}
			else
				sphWarning ( "%s", sError.cstr() );
		}
	}

	tOffset = tBlobRowBuilder.Flush();
	return true;
}


void CSphIndex_VLN::Build_StoreColumnarAttrs ( DocID_t tDocId, columnar::Builder_i & tBuilder, CSphSource & tSource, QueryMvaContainer_c & tMvaContainer )
{
	int iColumnarAttrId = 0;
	for ( int i=0; i < m_tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
		if ( !tAttr.IsColumnar() )
			continue;

		switch ( tAttr.m_eAttrType )
		{
		case SPH_ATTR_STRING:
			{
				const CSphString & sStrAttr = tSource.GetStrAttr(i);
				tBuilder.SetAttr ( iColumnarAttrId, (const BYTE*)sStrAttr.cstr(), sStrAttr.Length() );
			}
			break;

		case SPH_ATTR_UINT32SET:
		case SPH_ATTR_INT64SET:
			{
				const CSphVector<int64_t> * pMva = FetchMVA ( tDocId, i, tAttr, tMvaContainer, tSource, false );
				tBuilder.SetAttr ( iColumnarAttrId, pMva ? pMva->Begin() : nullptr, pMva ? pMva->GetLength() : 0 );
			}
			break;

		default:
			tBuilder.SetAttr ( iColumnarAttrId, tSource.GetAttr(i) );
			break;
		}

		iColumnarAttrId++;
	}
}


void CSphIndex_VLN::Build_StoreHistograms ( CSphVector<std::pair<Histogram_i*,int>> dHistograms, CSphSource & tSource )
{
	for ( auto & i : dHistograms )
		i.first->Insert ( tSource.GetAttr ( i.second ) );
}

template <typename T>
void SourceCopyMva ( const BYTE * pData, int iLenBytes, CSphVector<int64_t> & dDst )
{
	const T * pSrc = (const T *)pData;
	int iValues = iLenBytes / sizeof(T);

	dDst.Resize ( iValues );
	int64_t * pDst = dDst.Begin();
	const int64_t * pDstEnd = pDst + iValues;
	while ( pDst<pDstEnd )
	{
		*pDst = sphUnalignedRead ( *pSrc );
		pSrc++;
		pDst++;
	}
}


static void ResetFileAccess ( CSphIndex * pIndex )
{
	MutableIndexSettings_c tMutable = pIndex->GetMutableSettings();
	tMutable.m_tFileAccess = FileAccessSettings_t();
	pIndex->SetMutableSettings ( tMutable );
}

class KeepAttrs_c : public AttrSource_i
{
public:
	explicit KeepAttrs_c ( QueryMvaContainer_c & tMvaContainer )
		: m_pIndex ( nullptr )
		, m_tMvaContainer ( tMvaContainer )
	{}

	void SetBlobSource ( AttrSource_i * pSource )
	{
		m_pBlobSource = pSource;
	}

	bool Init ( const CSphString & sKeepAttrs, const StrVec_t & dKeepAttrs, const CSphSchema & tSchema )
	{
		if ( sKeepAttrs.IsEmpty() && !dKeepAttrs.GetLength() )
			return false;

		m_pBlobRowLocator = tSchema.GetAttr ( sphGetBlobLocatorName() );
		m_bHasBlobAttrs = tSchema.HasBlobAttrs();
		m_iStride = tSchema.GetRowSize();

		CSphString sError;
		StrVec_t dWarnings;

		m_pIndex = (CSphIndex_VLN *)sphCreateIndexPhrase ( "keep-attrs", sKeepAttrs.cstr() );
		ResetFileAccess ( m_pIndex.Ptr() );

		if ( !m_pIndex->Prealloc ( false, nullptr, dWarnings ) )
		{
			if ( !m_pIndex->GetLastError().IsEmpty() )
				sError.SetSprintf ( "%s error: '%s'", sError.scstr(), m_pIndex->GetLastError().cstr() );

			sphWarn ( "unable to load 'keep-attrs' index (%s); ignoring --keep-attrs", sError.cstr() );

			m_pIndex.Reset();
		} else
		{
			// check schema
			if ( !tSchema.CompareTo ( m_pIndex->GetMatchSchema(), sError, false ) )
			{
				sphWarn ( "schemas are different (%s); ignoring --keep-attrs", sError.cstr() );
				m_pIndex.Reset();
			}
		}

		for ( const auto & i : dWarnings )
			sphWarn ( "%s", i.cstr() );

		if ( m_pIndex.Ptr() )
		{
			if ( dKeepAttrs.GetLength() )
			{
				m_dLocMva.Init ( tSchema.GetAttrsCount() );
				m_dLocString.Init ( tSchema.GetAttrsCount() );
				m_bKeepSomeAttrs = true;

				ARRAY_FOREACH ( i, dKeepAttrs )
				{
					int iCol = tSchema.GetAttrIndex ( dKeepAttrs[i].cstr() );
					if ( iCol==-1 )
					{
						sphWarn ( "no attribute found '%s'; ignoring --keep-attrs", dKeepAttrs[i].cstr() );
						m_pIndex.Reset();
						break;
					}

					const CSphColumnInfo & tCol = tSchema.GetAttr ( iCol );
					if ( tCol.m_eAttrType==SPH_ATTR_UINT32SET || tCol.m_eAttrType==SPH_ATTR_INT64SET )
					{
						m_dLocMva.BitSet ( iCol );
						m_bHasMva = true;
					} else if ( tCol.m_eAttrType==SPH_ATTR_STRING || tCol.m_eAttrType==SPH_ATTR_JSON )
					{
						m_dLocString.BitSet ( iCol );
						m_bHasString = true;
					} else
					{
						m_dLocPlain.Add ( tCol.m_tLocator );
					}
				}
			}

			m_dMvaField.Init ( tSchema.GetAttrsCount() );
			for ( int i=0; i<tSchema.GetAttrsCount(); i++ )
			{
				const CSphColumnInfo & tCol = tSchema.GetAttr ( i );
				if ( ( tCol.m_eAttrType==SPH_ATTR_UINT32SET || tCol.m_eAttrType==SPH_ATTR_INT64SET ) && tCol.m_eSrc==SPH_ATTRSRC_FIELD )
				{
					m_dMvaField.BitSet ( i );
				}
			}
		}

		if ( m_pIndex.Ptr() )
			m_pIndex->Preread();

		return ( m_pIndex.Ptr()!=nullptr );
	}

	bool Keep ( DocID_t tDocid )
	{
		if ( !m_pIndex.Ptr() )
			return false;

		m_tDocid = tDocid;
		m_pRow = m_pIndex->FindDocinfo ( tDocid );
		return ( m_pRow!=nullptr );
	}

	SphAttr_t GetAttr ( int iAttr ) override
	{
		assert ( 0 && "internal error" );
		return 0;
	}

	CSphVector<int64_t> * GetFieldMVA ( int iAttr ) override
	{
		if ( !m_pIndex.Ptr() || !m_pRow )
			return nullptr;

		// fallback to indexed data
		if ( m_bKeepSomeAttrs && ( !m_bHasMva || !m_dLocMva.BitGet ( iAttr ) ) )
		{
			if ( m_dMvaField.BitGet ( iAttr ) )
			{
				return m_pBlobSource->GetFieldMVA ( iAttr );
			} else
			{
				assert ( m_tMvaContainer.m_tContainer[iAttr] );
				return m_tMvaContainer.m_tContainer[iAttr]->Find ( m_tDocid );
			}
		}

		int iLen = 0;
		ESphAttr eAttr = SPH_ATTR_NONE;
		const BYTE * pData = GetBlobData ( iAttr, iLen, eAttr );
		assert ( eAttr==SPH_ATTR_UINT32SET || eAttr==SPH_ATTR_INT64SET );

		if ( eAttr==SPH_ATTR_INT64SET )
		{
			SourceCopyMva<int64_t> ( pData, iLen, m_dDataMva );
		} else
		{
			SourceCopyMva<DWORD> ( pData, iLen, m_dDataMva );
		}

		return &m_dDataMva;
	}

	/// returns string attributes for a given attribute
	virtual const CSphString & GetStrAttr ( int iAttr ) override
	{
		if ( !m_pIndex.Ptr() || !m_pRow )
			return m_sEmpty;

		// fallback to indexed data
		if ( m_bKeepSomeAttrs && ( !m_bHasString || !m_dLocString.BitGet ( iAttr ) ) )
		{
			assert ( m_pBlobSource );
			return m_pBlobSource->GetStrAttr ( iAttr );
		}

		int iLen = 0;
		ESphAttr eAttr = SPH_ATTR_NONE;
		const BYTE * pData = GetBlobData ( iAttr, iLen, eAttr );
		assert ( eAttr==SPH_ATTR_STRING || eAttr==SPH_ATTR_JSON );

		if ( !iLen )
			return m_sEmpty;

		m_sDataString.SetBinary ( (const char *)pData, iLen );
		return m_sDataString;
	}

	const CSphRowitem * GetRow ( CSphRowitem * pSrc )
	{
		if ( !m_pIndex.Ptr() || !m_pRow )
			return pSrc;

		// keep only blob attributes
		if ( m_bKeepSomeAttrs && !m_dLocPlain.GetLength() )
			return pSrc;

		if ( m_bKeepSomeAttrs )
		{
			// keep only some plain attributes
			ARRAY_FOREACH ( i, m_dLocPlain )
			{
				const CSphAttrLocator & tLoc = m_dLocPlain[i];
				SphAttr_t tAtrr = sphGetRowAttr ( m_pRow, tLoc );
				sphSetRowAttr ( pSrc, tLoc, tAtrr );
			}
			return pSrc;
		}
		else if ( m_bHasBlobAttrs )
		{
			// copy whole row except blob row offset
			assert(m_pBlobRowLocator);
			SphOffset_t tOffset = sphGetRowAttr ( pSrc, m_pBlobRowLocator->m_tLocator );
			memcpy ( pSrc, m_pRow, m_iStride*sizeof(CSphRowitem) );
			sphSetRowAttr ( pSrc, m_pBlobRowLocator->m_tLocator, tOffset );
			return pSrc;
		}

		// keep whole row
		return m_pRow;
	}

	void Reset()
	{
		m_pIndex.Reset();
	}

private:
	CSphScopedPtr<CSphIndex_VLN>	m_pIndex;
	CSphVector<CSphAttrLocator>		m_dLocPlain;
	CSphBitvec						m_dLocMva;
	CSphBitvec						m_dMvaField;
	CSphBitvec						m_dLocString;
	const CSphColumnInfo *			m_pBlobRowLocator = nullptr;

	bool							m_bKeepSomeAttrs = false;
	bool							m_bHasMva = false;
	bool							m_bHasString = false;
	bool							m_bHasBlobAttrs = false;

	int								m_iStride = 0;
	const CSphRowitem *				m_pRow = nullptr;
	CSphVector<int64_t>				m_dDataMva;
	CSphString						m_sDataString;
	const CSphString				m_sEmpty = "";

	AttrSource_i *					m_pBlobSource = nullptr;
	DocID_t							m_tDocid = 0;
	QueryMvaContainer_c &			m_tMvaContainer;


	const BYTE * GetBlobData ( int iAttr, int & iLen, ESphAttr & eAttr )
	{
		const BYTE * pPool = m_pIndex->m_tBlobAttrs.GetWritePtr();
		const CSphColumnInfo & tCol = m_pIndex->GetMatchSchema().GetAttr ( iAttr );
		assert ( tCol.m_tLocator.IsBlobAttr() );
		eAttr = tCol.m_eAttrType;

		return sphGetBlobAttr ( m_pRow, tCol.m_tLocator, pPool, iLen );
	}
};


void WarnAboutKillList ( const CSphVector<DocID_t> & dKillList, const KillListTargets_c & tTargets )
{
	if ( dKillList.GetLength() && !tTargets.m_dTargets.GetLength() )
		sphWarn ( "kill-list not empty but no killlist_target specified" );

	if ( !dKillList.GetLength() )
	{
		for ( const auto & tTarget : tTargets.m_dTargets )
			if ( tTarget.m_uFlags==KillListTarget_t::USE_KLIST )
			{
				sphWarn ( "killlist_target is specified, but kill-list is empty" );
				break;
			}
	}
}


bool CSphIndex_VLN::SortDocidLookup ( int iFD, int nBlocks, int iMemoryLimit, int nLookupsInBlock, int nLookupsInLastBlock, CSphIndexProgress & tProgress )
{
	tProgress.PhaseBegin ( CSphIndexProgress::PHASE_LOOKUP );
	assert (!tProgress.m_iDocids);
	tProgress.m_iDocidsTotal = m_tStats.m_iTotalDocuments;

	if ( !nBlocks )
		return true;

	DocidLookupWriter_c tWriter ( (DWORD)m_tStats.m_iTotalDocuments );
	if ( !tWriter.Open ( GetIndexFileName(SPH_EXT_SPT), m_sLastError ) )
		return false;

	DeleteOnFail_c tWatchdog;
	tWatchdog.AddWriter ( &tWriter.GetWriter() );

	RawVector_T<CSphBin> dBins;
	SphOffset_t iSharedOffset = -1;

	dBins.Reserve ( nBlocks );

	int iBinSize = CSphBin::CalcBinSize ( iMemoryLimit, nBlocks, "sort_lookup" );

	dBins.Reserve_static (nBlocks);
	for ( int i=0; i<nBlocks; ++i )
	{
		auto& dBin = dBins.Add();
		dBin.m_iFileLeft = ( ( i==nBlocks-1 ) ? nLookupsInLastBlock : nLookupsInBlock )*sizeof(DocidRowidPair_t);
		dBin.m_iFilePos = ( i==0 ) ? 0 : dBins[i-1].m_iFilePos + dBins[i-1].m_iFileLeft;
		dBin.Init ( iFD, &iSharedOffset, iBinSize );
	}

	CSphFixedVector<DocidRowidPair_t> dTopDocIDs ( nBlocks );
	CSphQueue<int, CmpQueuedLookup_fn> tLookupQueue ( nBlocks );

	CmpQueuedLookup_fn::m_pStorage = dTopDocIDs.Begin();

	for ( int i=0; i<nBlocks; ++i )
	{
		if ( dBins[i].ReadBytes ( &dTopDocIDs[i], sizeof(DocidRowidPair_t) )!=BIN_READ_OK )
		{
			m_sLastError.SetSprintf ( "sort_lookup: warmup failed (io error?)" );
			return false;
		}

		tLookupQueue.Push(i);
	}

	DWORD tProcessed = 0;
	while ( tLookupQueue.GetLength() )
	{
		int iBin = tLookupQueue.Root();

		tWriter.AddPair ( dTopDocIDs[iBin] );

		tLookupQueue.Pop();
		ESphBinRead eRes = dBins[iBin].ReadBytes ( &dTopDocIDs[iBin], sizeof(DocidRowidPair_t) );
		if ( eRes==BIN_READ_ERROR )
		{
			m_sLastError.SetSprintf ( "sort_lookup: failed to read entry" );
			return false;
		}

		if ( eRes==BIN_READ_OK )
			tLookupQueue.Push(iBin);

		tProcessed++;
		if ( ( tProcessed % 10000 )==0 )
		{
			tProgress.m_iDocids = tProcessed;
			tProgress.Show ( );
		}
	}

	if ( !tWriter.Finalize ( m_sLastError ) )
		return false;

	// clean up readers
	dBins.Reset();

	tWatchdog.AllIsDone();
	tProgress.m_iDocids = tProgress.m_iDocidsTotal;
	tProgress.PhaseEnd();
	return true;
}


bool CSphIndex_VLN::Build_SetupInplace ( SphOffset_t & iHitsGap, int iHitsMax, int iFdHits ) const
{
	if ( !m_bInplaceSettings )
		return true;

	const int HIT_SIZE_AVG = 4;
	const float HIT_BLOCK_FACTOR = 1.0f;

	if ( m_iHitGap )
		iHitsGap = (SphOffset_t) m_iHitGap;
	else
		iHitsGap = (SphOffset_t)( iHitsMax*HIT_BLOCK_FACTOR*HIT_SIZE_AVG );

	iHitsGap = Max ( iHitsGap, 1 );
	if ( !SeekAndWarn ( iFdHits, iHitsGap, "CSphIndex_VLN::Build" ))
		return false;

	return true;
}


void SetupDocstoreFields ( DocstoreAddField_i & tFields, const CSphSchema & tSchema )
{
	int iStored = 0;
	for ( int i = 0; i < tSchema.GetFieldsCount(); i++ )
		if ( tSchema.IsFieldStored(i) )
		{
			tFields.AddField ( tSchema.GetFieldName(i), DOCSTORE_TEXT );
			iStored++;
		}

	for ( int i = 0; i < tSchema.GetAttrsCount(); i++ )
		if ( tSchema.IsAttrStored(i) )
		{
			tFields.AddField ( tSchema.GetAttr(i).m_sName, DOCSTORE_ATTR );
			iStored++;
		}

	assert(iStored);
}


bool CheckStoredFields ( const CSphSchema & tSchema, const CSphIndexSettings & tSettings, CSphString & sError )
{
	for ( const auto & i : tSettings.m_dStoredFields )
		if ( tSchema.GetAttr ( i.cstr() ) )
		{
			sError.SetSprintf ( "existing attribute specified in stored_fields: '%s'\n", i.cstr() );
			return false;
		}

	for ( const auto & i : tSettings.m_dStoredOnlyFields )
		if ( tSchema.GetAttr ( i.cstr() ) )
		{
			sError.SetSprintf ( "existing attribute specified in stored_fields: '%s'\n", i.cstr() );
			return false;
		}

	return true;
}


bool CSphIndex_VLN::Build_SetupDocstore ( CSphScopedPtr<DocstoreBuilder_i> & pDocstore, CSphBitvec & dStoredFields, CSphBitvec & dStoredAttrs, CSphVector<CSphVector<BYTE>> & dTmpDocstoreAttrStorage )
{
	if ( !m_tSchema.HasStoredFields() && !m_tSchema.HasStoredAttrs() )
		return true;

	DocstoreBuilder_i * pBuilder = CreateDocstoreBuilder ( GetIndexFileName(SPH_EXT_SPDS), GetSettings(), m_sLastError );
	if ( !pBuilder )
		return false;

	SetupDocstoreFields ( *pBuilder, m_tSchema );

	dStoredFields.Init ( m_tSchema.GetFieldsCount() );
	dStoredAttrs.Init ( m_tSchema.GetAttrsCount() );

	for ( int i = 0; i < m_tSchema.GetFieldsCount(); i++ )
		if ( pBuilder->GetFieldId ( m_tSchema.GetFieldName(i), DOCSTORE_TEXT )!=-1 )
			dStoredFields.BitSet(i);

	for ( int i = 0; i < m_tSchema.GetAttrsCount(); i++ )
		if ( pBuilder->GetFieldId ( m_tSchema.GetAttr(i).m_sName, DOCSTORE_ATTR )!=-1 )
			dStoredAttrs.BitSet(i);

	dTmpDocstoreAttrStorage.Resize ( m_tSchema.GetAttrsCount() );

	pDocstore = pBuilder;
	return true;
}


bool CSphIndex_VLN::Build_SetupBlobBuilder ( CSphScopedPtr<BlobRowBuilder_i> & pBuilder )
{
	if ( !m_tSchema.HasBlobAttrs() )
		return true;

	pBuilder = sphCreateBlobRowBuilder ( m_tSchema, GetIndexFileName ( SPH_EXT_SPB, true ), m_tSettings.m_tBlobUpdateSpace, m_sLastError );
	return !!pBuilder.Ptr();
}


bool CSphIndex_VLN::Build_SetupColumnar ( CSphScopedPtr<columnar::Builder_i> & pBuilder )
{
	if ( !m_tSchema.HasColumnarAttrs() )
		return true;

	pBuilder = CreateColumnarBuilder ( m_tSchema, m_tSettings, GetIndexFileName ( SPH_EXT_SPC, true ), m_sLastError );
	return !!pBuilder.Ptr();
}


bool CSphIndex_VLN::Build_SetupHistograms ( CSphScopedPtr<HistogramContainer_c> & pContainer, CSphVector<std::pair<Histogram_i*,int>> & dHistograms )
{
	for ( int i = 0; i < m_tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
		Histogram_i * pHistogram = CreateHistogram ( tAttr.m_sName, tAttr.m_eAttrType );
		if ( pHistogram )
			dHistograms.Add ( { pHistogram, i } );
	}

	if ( !dHistograms.GetLength() )
		return true;

	pContainer = new HistogramContainer_c;
	for ( const auto & i : dHistograms )
		Verify ( pContainer->Add ( i.first ) );

	return true;
}


static VecTraits_T<const BYTE> GetAttrForDocstore ( DocID_t tDocID, int iAttr, const CSphSchema & tSchema, QueryMvaContainer_c & tMvaContainer, CSphSource & tSource, CSphVector<BYTE> & dTmpStorage )
{
	const CSphColumnInfo & tAttr = tSchema.GetAttr(iAttr);

	switch ( tAttr.m_eAttrType )
	{
	case SPH_ATTR_STRING:
	{
		const CSphString & sStrAttr = tSource.GetStrAttr(iAttr);
		return { (const BYTE*)sStrAttr.cstr(), sStrAttr.Length() };
	}

	case SPH_ATTR_UINT32SET:
	{
		const CSphVector<int64_t> * pMva = FetchMVA ( tDocID, iAttr, tAttr, tMvaContainer, tSource, false );
		dTmpStorage.Resize ( pMva->GetLength()*sizeof(DWORD) );
		DWORD * pAttrs = (DWORD*)dTmpStorage.Begin();
		for ( int iValue = 0; iValue < pMva->GetLength(); iValue++ )
			pAttrs[iValue] = (DWORD)(*pMva)[iValue];

		return dTmpStorage;
	}

	case SPH_ATTR_INT64SET:
	{
		const CSphVector<int64_t> * pMva = FetchMVA ( tDocID, iAttr, tAttr, tMvaContainer, tSource, false );
		return { pMva ? (const BYTE*)pMva->Begin() : nullptr, pMva ? (int64_t)pMva->GetLengthBytes() : 0 };
	}

	case SPH_ATTR_BIGINT:
	{
		int64_t iValue = tSource.GetAttr(iAttr);
		dTmpStorage.Resize ( sizeof(iValue) );
		memcpy ( dTmpStorage.Begin(), &iValue, dTmpStorage.GetLength() );
		return dTmpStorage;
	}

	default:
		// assume 32-bit integer
		uint32_t uValue = tSource.GetAttr(iAttr);
		dTmpStorage.Resize ( sizeof(uValue) );
		memcpy ( dTmpStorage.Begin(), &uValue, dTmpStorage.GetLength() );
		return dTmpStorage;
	}
}


void CSphIndex_VLN::Build_AddToDocstore ( DocstoreBuilder_i * pDocstoreBuilder, DocID_t tDocID, QueryMvaContainer_c & tMvaContainer, CSphSource & tSource, const CSphBitvec & dStoredFields, const CSphBitvec & dStoredAttrs, CSphVector<CSphVector<BYTE>> & dTmpDocstoreAttrStorage )
{
	if ( !pDocstoreBuilder )
		return;

	DocstoreBuilder_i::Doc_t tDoc;
	tSource.GetDocFields ( tDoc.m_dFields );

	assert ( tDoc.m_dFields.GetLength()==m_tSchema.GetFieldsCount() );

	// filter out non-hl fields (should already be null)
	int iField = 0;
	for ( int i = 0; i < dStoredFields.GetBits(); i++ )
	{
		if ( !dStoredFields.BitGet(i) )
			tDoc.m_dFields.Remove(iField);
		else
			iField++;
	}

	VecTraits_T<BYTE> * pAddedAttrs = tDoc.m_dFields.AddN ( dStoredAttrs.BitCount() );
	int iAttr = 0;
	for ( int i = 0; i < dStoredAttrs.GetBits(); i++ )
		if ( dStoredAttrs.BitGet(i) )
			pAddedAttrs[iAttr++] = GetAttrForDocstore ( tDocID, i, m_tSchema, tMvaContainer, tSource, dTmpDocstoreAttrStorage[i] );

	pDocstoreBuilder->AddDoc ( tSource.m_tDocInfo.m_tRowID, tDoc );
}


int CSphIndex_VLN::Build ( const CSphVector<CSphSource*> & dSources, int iMemoryLimit, int iWriteBuffer, CSphIndexProgress& tProgress )
{
	assert ( dSources.GetLength() );

	CSphVector<SphWordID_t> dHitlessWords;

	if ( !LoadHitlessWords ( m_tSettings.m_sHitlessFiles, m_pTokenizer, m_pDict, dHitlessWords, m_sLastError ) )
		return 0;

	// vars shared between phases
	RawVector_T<CSphBin> dBins;
	SphOffset_t iSharedOffset = -1;

	m_pDict->HitblockBegin();

	// setup sources
	ARRAY_FOREACH ( iSource, dSources )
	{
		CSphSource * pSource = dSources[iSource];
		assert ( pSource );

		pSource->SetDict ( m_pDict );
		pSource->Setup ( m_tSettings, nullptr );
	}

	// connect 1st source and fetch its schema
	// and don't disconnect it because some sources can't survive connect/disconnect
	CSphSource * pSource0 = dSources[0];
	if ( !pSource0->Connect ( m_sLastError )
		|| !pSource0->IterateStart ( m_sLastError )
		|| !pSource0->UpdateSchema ( &m_tSchema, m_sLastError )
		|| !pSource0->SetupMorphFields ( m_sLastError ) )
	{
		return 0;
	}

	if ( !CheckStoredFields ( m_tSchema, m_tSettings, m_sLastError ) )
		return 0;

	bool bHaveQueryMVAs = false;
	for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
		if ( ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET || tAttr.m_eAttrType==SPH_ATTR_INT64SET ) && tAttr.m_eSrc!=SPH_ATTRSRC_FIELD )
		{
			bHaveQueryMVAs = true;
			break;
		}
	}

	// non-SQL sources don't survive connect+disconnect+reconnect
	// and if we don't disconnect now we won't be able to fetch query MVAs from SQL sources
	QueryMvaContainer_c tQueryMvaContainer;
	if ( bHaveQueryMVAs )
	{
		pSource0->Disconnect();

		// temporary storage for MVAs that we fetch from queries
		// we might want to dump that to file later
		if ( !CollectQueryMvas ( dSources, tQueryMvaContainer ) )
			return 0;
	}

	if ( m_tSchema.GetFieldsCount()==0 )
	{
		m_sLastError.SetSprintf ( "No fields in schema - will not index" );
		return 0;
	}

	int iFieldLens = m_tSchema.GetAttrId_FirstFieldLen();

	const CSphColumnInfo * pBlobLocatorAttr = m_tSchema.GetAttr ( sphGetBlobLocatorName() );

	if ( !m_pTokenizer->SetFilterSchema ( m_tSchema, m_sLastError ) )
		return 0;

	int iHitBuilderBufferSize = ( iWriteBuffer>0 )
		? Max ( iWriteBuffer, MIN_WRITE_BUFFER )
		: DEFAULT_WRITE_BUFFER;

	CSphHitBuilder tHitBuilder ( m_tSettings, dHitlessWords, false, iHitBuilderBufferSize, m_pDict, &m_sLastError );

	////////////////////////////////////////////////
	// collect and partially sort hits
	////////////////////////////////////////////////

	CSphVector <DocID_t> dKillList;

	// adjust memory requirements
	int iOldLimit = iMemoryLimit;

	// book at least 2 MB for keywords dict, if needed
	int iDictSize = 0;
	if ( m_pDict->GetSettings().m_bWordDict )
		iDictSize = Max ( MIN_KEYWORDS_DICT, iMemoryLimit/8 );

	// reserve for sorting docid-rowid pairs
	int iDocidLookupSize = Max ( 32768, iMemoryLimit/16 );

	iMemoryLimit -= iDocidLookupSize+iDictSize;

	// do we have enough left for hits?
	int iHitsMax = 1048576;
	if ( iMemoryLimit < iHitsMax*(int)sizeof(CSphWordHit) )
	{
		iMemoryLimit = iOldLimit + iHitsMax*sizeof(CSphWordHit) - iMemoryLimit;
		sphWarn ( "collect_hits: mem_limit=%d kb too low, increasing to %d kb",	iOldLimit/1024, iMemoryLimit/1024 );
	} else
		iHitsMax = iMemoryLimit / sizeof(CSphWordHit);

	// allocate raw hits block
	CSphFixedVector<CSphWordHit> dHits ( iHitsMax + MAX_SOURCE_HITS );
	CSphWordHit * pHits = dHits.Begin();
	CSphWordHit * pHitsMax = dHits.Begin() + iHitsMax;

	int nDocidLookupsPerBlock = iDocidLookupSize/sizeof(DocidRowidPair_t);
	int nDocidLookup = 0;
	int nDocidLookupBlocks = 0;
	CSphFixedVector<DocidRowidPair_t> dDocidLookup ( nDocidLookupsPerBlock );

	// fallback blob source (for mva)
	KeepAttrs_c tPrevAttrs ( tQueryMvaContainer );
	const bool bGotPrevIndex = tPrevAttrs.Init ( m_sKeepAttrs, m_dKeepAttrs, m_tSchema );

	// create temp files
	CSphString sFileSPP = m_bInplaceSettings ? GetIndexFileName(SPH_EXT_SPP) : GetIndexFileName("tmp1");

	CSphAutofile fdLock ( GetIndexFileName("tmp0"), SPH_O_NEW, m_sLastError, true );
	CSphAutofile fdHits ( sFileSPP, SPH_O_NEW, m_sLastError, !m_bInplaceSettings );
	CSphAutofile fdTmpLookup ( GetIndexFileName("tmp2"), SPH_O_NEW, m_sLastError, true );

	CSphWriter tWriterSPA;
	bool bHaveNonColumnarAttrs = m_tSchema.HasNonColumnarAttrs();

	// write to temp file because of possible --keep-attrs option which loads prev index
	if ( bHaveNonColumnarAttrs && !tWriterSPA.OpenFile ( GetIndexFileName ( SPH_EXT_SPA, true ), m_sLastError ) )
		return 0;

	DeleteOnFail_c dFileWatchdog;

	if ( m_bInplaceSettings )
		dFileWatchdog.AddAutofile ( &fdHits );

	if ( fdLock.GetFD()<0 || fdHits.GetFD()<0 )
		return 0;

	CSphScopedPtr<BlobRowBuilder_i> pBlobRowBuilder(nullptr);
	if ( !Build_SetupBlobBuilder(pBlobRowBuilder) )
		return 0;

	CSphScopedPtr<columnar::Builder_i> pColumnarBuilder(nullptr);
	if ( !Build_SetupColumnar(pColumnarBuilder) )
		return 0;

	CSphScopedPtr<DocstoreBuilder_i> pDocstoreBuilder(nullptr);
	CSphBitvec dStoredFields, dStoredAttrs;
	CSphVector<CSphVector<BYTE>> dTmpDocstoreAttrStorage;
	if ( !Build_SetupDocstore ( pDocstoreBuilder, dStoredFields, dStoredAttrs, dTmpDocstoreAttrStorage ) )
		return 0;

	CSphScopedPtr<HistogramContainer_c> pHistogramContainer(nullptr);
	CSphVector<std::pair<Histogram_i*,int>> dHistograms;
	if ( !Build_SetupHistograms ( pHistogramContainer, dHistograms ) )
		return 0;

	SphOffset_t iHitsGap = 0;
	if ( !Build_SetupInplace ( iHitsGap, iHitsMax, fdHits.GetFD() ) )
		return 0;

	if ( !sphLockEx ( fdLock.GetFD(), false ) )
	{
		m_sLastError.SetSprintf ( "failed to lock '%s': another indexer running?", fdLock.GetFilename() );
		return 0;
	}

	m_tStats.Reset ();
	tProgress.PhaseBegin ( CSphIndexProgress::PHASE_COLLECT );

	CSphVector<int> dHitBlocks;
	dHitBlocks.Reserve ( 1024 );

	AttrIndexBuilder_c tMinMax(m_tSchema);
	RowID_t tRowID = 0;
	int64_t iHitsTotal = 0;

	ARRAY_FOREACH ( iSource, dSources )
	{
		// connect and check schema
		CSphSource * pSource = dSources[iSource];

		bool bNeedToConnect = iSource>0 || bHaveQueryMVAs;
		if ( bNeedToConnect )
		{
			if ( !pSource->Connect ( m_sLastError )
				|| !pSource->IterateStart ( m_sLastError )
				|| !pSource->UpdateSchema ( &m_tSchema, m_sLastError )
				|| !pSource->SetupMorphFields ( m_sLastError ) )
			{
				return 0;
			}
		}

		// joined filter
		bool bGotJoined = pSource->HasJoinedFields();

		// fallback blob source (for string and json )
		if ( bGotPrevIndex )
			tPrevAttrs.SetBlobSource ( pSource );

		// fetch documents
		for ( ;; )
		{
			// get next doc, and handle errors
			bool bEOF = false;
			if ( !pSource->IterateDocument ( bEOF, m_sLastError ) )
				return 0;

			// check if we have no more documents
			if ( bEOF )
				break;

			pSource->m_tDocInfo.m_tRowID = tRowID++;
			DocID_t tDocID = pSource->GetAttr(0);

			pSource->RowIDAssigned ( tDocID, tRowID-1 );
			bool bKeepRow = ( bGotPrevIndex && tPrevAttrs.Keep ( tDocID ) );

			// show progress bar
			if ( ( pSource->GetStats().m_iTotalDocuments % 1000 )==0 )
			{
				tProgress.m_iDocuments = m_tStats.m_iTotalDocuments + pSource->GetStats().m_iTotalDocuments;
				tProgress.m_iBytes = m_tStats.m_iTotalBytes + pSource->GetStats().m_iTotalBytes;
				tProgress.Show();
			}

			// update crashdump
			g_iIndexerCurrentDocID = pSource->m_tDocInfo.m_tRowID;
			g_iIndexerCurrentHits = pHits-dHits.Begin();

			// store mva, strings and JSON blobs
			if ( pBlobRowBuilder.Ptr() )
			{
				AttrSource_i * pBlobSource = pSource;
				if ( bKeepRow )
					pBlobSource = &tPrevAttrs;
				SphOffset_t tBlobOffset = 0;
				if ( !Build_StoreBlobAttrs ( tDocID, tBlobOffset, *pBlobRowBuilder.Ptr(), tQueryMvaContainer, *pBlobSource, bKeepRow ) )
					return 0;

				pSource->m_tDocInfo.SetAttr ( pBlobLocatorAttr->m_tLocator, tBlobOffset );
			}

			// store anything columnar
			if ( pColumnarBuilder.Ptr() )
				Build_StoreColumnarAttrs ( tDocID, *pColumnarBuilder.Ptr(), *pSource, tQueryMvaContainer );

			Build_StoreHistograms ( dHistograms, *pSource );

			// store hits
			while ( const ISphHits * pDocHits = pSource->IterateHits ( m_sLastWarning ) )
			{
				int iDocHits = pDocHits->GetLength();
#if PARANOID
				for ( int i=0; i<iDocHits; i++ )
				{
					assert ( pDocHits->m_dData[i].m_uDocID==pSource->m_tDocInfo.m_uDocID );
					assert ( pDocHits->m_dData[i].m_uWordID );
					assert ( pDocHits->m_dData[i].m_iWordPos );
				}
#endif

				assert ( ( pHits+iDocHits )<=( pHitsMax+MAX_SOURCE_HITS ) );

				memcpy ( pHits, pDocHits->Begin(), iDocHits*sizeof(CSphWordHit) );
				pHits += iDocHits;

				// check if we need to flush
				if ( pHits<pHitsMax	&& !( iDictSize && m_pDict->HitblockGetMemUse() > iDictSize ) )
					continue;

				// update crashdump
				g_iIndexerPoolStartDocID = tDocID;
				g_iIndexerPoolStartHit = pHits-dHits.Begin();

				// sort hits
				int iHits = int ( pHits - dHits.Begin() );
				{
					sphSort ( dHits.Begin(), iHits, CmpHit_fn() );
					m_pDict->HitblockPatch ( dHits.Begin(), iHits );
				}
				pHits = dHits.Begin();

				// flush hits, docs are flushed independently
				dHitBlocks.Add ( tHitBuilder.cidxWriteRawVLB ( fdHits.GetFD(), dHits.Begin(), iHits ) );

				m_pDict->HitblockReset ();

				if ( dHitBlocks.Last()<0 )
					return 0;

				// progress bar
				iHitsTotal += iHits;
				tProgress.m_iDocuments = m_tStats.m_iTotalDocuments + pSource->GetStats().m_iTotalDocuments;
				tProgress.m_iBytes = m_tStats.m_iTotalBytes + pSource->GetStats().m_iTotalBytes;
				tProgress.Show();
			}

			// update total field lengths
			if ( iFieldLens>=0 )
			{
				for ( int i=0; i < m_tSchema.GetFieldsCount(); i++ )
					m_dFieldLens[i] += pSource->m_tDocInfo.GetAttr ( m_tSchema.GetAttr ( i+iFieldLens ).m_tLocator );
			}

			if ( bHaveNonColumnarAttrs )
			{
				// store docinfo
				// with the advent of SPH_ATTR_TOKENCOUNT, now MUST be done AFTER iterating the hits
				// because field lengths are computed during that iterating
				const CSphRowitem * pRow = pSource->m_tDocInfo.m_pDynamic;
				if ( bKeepRow )
					pRow = tPrevAttrs.GetRow ( pSource->m_tDocInfo.m_pDynamic );
				tMinMax.Collect ( pRow );
				tWriterSPA.PutBytes ( pRow, sizeof(CSphRowitem)*m_tSchema.GetRowSize() );
			}

			dDocidLookup[nDocidLookup].m_tDocID = tDocID;
			dDocidLookup[nDocidLookup].m_tRowID = pSource->m_tDocInfo.m_tRowID;
			nDocidLookup++;

			if ( nDocidLookup==dDocidLookup.GetLength() )
			{
				dDocidLookup.Sort ( CmpDocidLookup_fn() );
				if ( !sphWriteThrottled ( fdTmpLookup.GetFD (), &dDocidLookup[0], nDocidLookup*sizeof(DocidRowidPair_t), "temp_docid_lookup", m_sLastError ) )
					return 0;

				nDocidLookup = 0;
				nDocidLookupBlocks++;
			}

			Build_AddToDocstore ( pDocstoreBuilder.Ptr(), tDocID, tQueryMvaContainer, *pSource, dStoredFields, dStoredAttrs, dTmpDocstoreAttrStorage );

			// go on, loop next document
		}

		// FIXME! uncontrolled memory usage; add checks and/or diskbased sort in the future?
		if ( pSource->IterateKillListStart ( m_sLastError ) )
		{
			DocID_t tKilllistDocID;
			while ( pSource->IterateKillListNext ( tKilllistDocID ) )
				dKillList.Add ( tKilllistDocID );
		} else if ( !m_sLastError.IsEmpty() )
			return 0;

		// fetch joined fields
		if ( bGotJoined )
		{
			// flush tail of regular hits
			int iHits = int ( pHits - dHits.Begin() );
			if ( iDictSize && m_pDict->HitblockGetMemUse() && iHits )
			{
				sphSort ( dHits.Begin(), iHits, CmpHit_fn() );
				m_pDict->HitblockPatch ( dHits.Begin(), iHits );
				pHits = dHits.Begin();
				iHitsTotal += iHits;
				dHitBlocks.Add ( tHitBuilder.cidxWriteRawVLB ( fdHits.GetFD(), dHits.Begin(), iHits ) );
				if ( dHitBlocks.Last()<0 )
					return 0;
				m_pDict->HitblockReset ();
			}

			while (true)
			{
				// get next doc, and handle errors
				ISphHits * pJoinedHits = pSource->IterateJoinedHits ( m_sLastError );
				if ( !pJoinedHits )
					return 0;

				// check for eof
				if ( pSource->m_tDocInfo.m_tRowID==INVALID_ROWID )
					break;

				int iJoinedHits = pJoinedHits->GetLength();
				memcpy ( pHits, pJoinedHits->Begin(), iJoinedHits*sizeof(CSphWordHit) );
				pHits += iJoinedHits;

				// check if we need to flush
				if ( pHits<pHitsMax && !( iDictSize && m_pDict->HitblockGetMemUse() > iDictSize ) )
					continue;

				// store hits
				int iStoredHits = int ( pHits - dHits.Begin() );
				sphSort ( dHits.Begin(), iStoredHits, CmpHit_fn() );
				m_pDict->HitblockPatch ( dHits.Begin(), iStoredHits );

				pHits = dHits.Begin();
				iHitsTotal += iStoredHits;

				dHitBlocks.Add ( tHitBuilder.cidxWriteRawVLB ( fdHits.GetFD(), dHits.Begin(), iStoredHits ) );
				if ( dHitBlocks.Last()<0 )
					return 0;
				m_pDict->HitblockReset ();
			}
		}

		// this source is over, disconnect and update stats
		pSource->Disconnect ();

		m_tStats.m_iTotalDocuments += pSource->GetStats().m_iTotalDocuments;
		m_tStats.m_iTotalBytes += pSource->GetStats().m_iTotalBytes;
	}

	if ( m_tStats.m_iTotalDocuments>=INT_MAX )
	{
		m_sLastError.SetSprintf ( "index over %d documents not supported (got documents count=" INT64_FMT ")", INT_MAX, m_tStats.m_iTotalDocuments );
		return 0;
	}

	// flush last hit block
	if ( pHits>dHits.Begin() )
	{
		int iHits = int ( pHits - dHits.Begin() );
		{
			sphSort ( dHits.Begin(), iHits, CmpHit_fn() );
			m_pDict->HitblockPatch ( dHits.Begin(), iHits );
		}
		iHitsTotal += iHits;

		dHitBlocks.Add ( tHitBuilder.cidxWriteRawVLB ( fdHits.GetFD(), dHits.Begin(), iHits ) );

		m_pDict->HitblockReset ();

		if ( dHitBlocks.Last()<0 )
			return 0;
	}

	// reset hits pool
	dHits.Reset(0);

	if ( nDocidLookup )
	{
		dDocidLookup.Sort ( CmpDocidLookup_fn(), 0, nDocidLookup-1 );
		if ( !sphWriteThrottled ( fdTmpLookup.GetFD(), &dDocidLookup[0], nDocidLookup*sizeof(DocidRowidPair_t), "temp_docid_lookup", m_sLastError ) )
			return 0;

		nDocidLookupBlocks++;
	}

	dDocidLookup.Reset(0);

	tProgress.m_iDocuments = m_tStats.m_iTotalDocuments;
	tProgress.m_iBytes = m_tStats.m_iTotalBytes;
	tProgress.PhaseEnd();

	if ( bHaveNonColumnarAttrs )
	{
		if ( m_tStats.m_iTotalDocuments  )
		{
			tMinMax.FinishCollect();
			const CSphTightVector<CSphRowitem> & dMinMaxRows = tMinMax.GetCollected();
			tWriterSPA.PutBytes ( dMinMaxRows.Begin(), dMinMaxRows.GetLength()*sizeof(CSphRowitem) );

			m_iDocinfoIndex = ( dMinMaxRows.GetLength() / m_tSchema.GetRowSize() / 2 ) - 1;
		}

		tWriterSPA.CloseFile();
		if ( tWriterSPA.IsError() )
		{
			m_sLastError = "error writing .SPA";
			return false;
		}
	}

	if ( pBlobRowBuilder.Ptr() && !pBlobRowBuilder->Done(m_sLastError) )
		return 0;

	std::string sError;
	if ( pColumnarBuilder.Ptr() && !pColumnarBuilder->Done(sError) )
	{
		m_sLastError = sError.c_str();
		return 0;
	}

	if ( pHistogramContainer.Ptr() && !pHistogramContainer->Save ( GetIndexFileName(SPH_EXT_SPHI), m_sLastError ) )
		return 0;

	if ( bGotPrevIndex )
		tPrevAttrs.Reset();

	CSphString sSPA = GetIndexFileName(SPH_EXT_SPA);
	CSphString sSPATmp = GetIndexFileName ( SPH_EXT_SPA, true );
	if ( bHaveNonColumnarAttrs && sph::rename ( sSPATmp.cstr(), sSPA.cstr() )!=0 )
	{
		m_sLastError.SetSprintf ( "failed to rename %s to %s", sSPATmp.cstr(), sSPA.cstr() );
		return false;
	}

	CSphString sSPB = GetIndexFileName(SPH_EXT_SPB);
	CSphString sSPBTmp = GetIndexFileName ( SPH_EXT_SPB, true );
	if ( m_tSchema.HasBlobAttrs() && sph::rename ( sSPBTmp.cstr(), sSPB.cstr() )!=0 )
	{
		m_sLastError.SetSprintf ( "failed to rename %s to %s", sSPBTmp.cstr(), sSPB.cstr() );
		return false;
	}

	CSphString sSPC = GetIndexFileName(SPH_EXT_SPC);
	CSphString sSPCTmp = GetIndexFileName ( SPH_EXT_SPC, true );
	if ( m_tSchema.HasColumnarAttrs() && sph::rename ( sSPCTmp.cstr(), sSPC.cstr() )!=0 )
	{
		m_sLastError.SetSprintf ( "failed to rename %s to %s", sSPCTmp.cstr(), sSPC.cstr() );
		return false;
	}

	if ( !WriteDeadRowMap ( GetIndexFileName(SPH_EXT_SPM), (DWORD)m_tStats.m_iTotalDocuments, m_sLastError ) )
		return 0;

	dKillList.Uniq();
	if ( !WriteKillList ( GetIndexFileName(SPH_EXT_SPK), dKillList.Begin(), dKillList.GetLength(), m_tSettings.m_tKlistTargets, m_sLastError ) )
		return 0;

	if ( pDocstoreBuilder.Ptr() )
		pDocstoreBuilder->Finalize();

	WarnAboutKillList ( dKillList, m_tSettings.m_tKlistTargets );

	m_iMinMaxIndex = m_tStats.m_iTotalDocuments*m_tSchema.GetRowSize();
	if ( !SortDocidLookup ( fdTmpLookup.GetFD(), nDocidLookupBlocks, iMemoryLimit, nDocidLookupsPerBlock, nDocidLookup, tProgress ) )
		return 0;

	///////////////////////////////////
	// sort and write compressed index
	///////////////////////////////////

	// initialize readers
	assert ( dBins.IsEmpty() );
	dBins.Reserve_static ( dHitBlocks.GetLength() );

	iSharedOffset = -1;

	float fReadFactor = 1.0f;
	int iRelocationSize = 0;
	iWriteBuffer = iHitBuilderBufferSize;

	if ( m_bInplaceSettings )
	{
		assert ( m_fRelocFactor > 0.005f && m_fRelocFactor < 0.95f );
		assert ( m_fWriteFactor > 0.005f && m_fWriteFactor < 0.95f );
		assert ( m_fWriteFactor+m_fRelocFactor < 1.0f );

		fReadFactor -= m_fRelocFactor + m_fWriteFactor;

		iRelocationSize = int ( iMemoryLimit * m_fRelocFactor );
		iWriteBuffer = int ( iMemoryLimit * m_fWriteFactor );
	}

	int iBinSize = CSphBin::CalcBinSize ( int ( iMemoryLimit * fReadFactor ),
		dHitBlocks.GetLength() + m_pDict->GetSettings().m_bWordDict, "sort_hits" );

	CSphFixedVector <BYTE> dRelocationBuffer ( iRelocationSize );
	iSharedOffset = -1;

	ARRAY_FOREACH ( i, dHitBlocks )
	{
		dBins.Emplace_back ( m_tSettings.m_eHitless, m_pDict->GetSettings().m_bWordDict );
		dBins[i].m_iFileLeft = dHitBlocks[i];
		dBins[i].m_iFilePos = ( i==0 ) ? iHitsGap : dBins[i-1].m_iFilePos + dBins[i-1].m_iFileLeft;
		dBins[i].Init ( fdHits.GetFD(), &iSharedOffset, iBinSize );
	}

	// if there were no hits, create zero-length index files
	int iRawBlocks = dBins.GetLength();

	//////////////////////////////
	// create new index files set
	//////////////////////////////

	tHitBuilder.CreateIndexFiles ( GetIndexFileName(SPH_EXT_SPD).cstr(), GetIndexFileName(SPH_EXT_SPP).cstr(), GetIndexFileName(SPH_EXT_SPE).cstr(), m_bInplaceSettings, iWriteBuffer, fdHits, &iSharedOffset );

	// dict files
	CSphAutofile fdTmpDict ( GetIndexFileName("tmp8"), SPH_O_NEW, m_sLastError, true );
	CSphAutofile fdDict ( GetIndexFileName(SPH_EXT_SPI), SPH_O_NEW, m_sLastError, false );
	if ( fdTmpDict.GetFD()<0 || fdDict.GetFD()<0 )
		return 0;

	m_pDict->DictBegin ( fdTmpDict, fdDict, iBinSize );

	//////////////
	// final sort
	//////////////

	if ( iRawBlocks )
	{
		int iLastBin = dBins.GetLength () - 1;
		SphOffset_t iHitFileSize = dBins[iLastBin].m_iFilePos + dBins [iLastBin].m_iFileLeft;

		CSphHitQueue tQueue ( iRawBlocks );
		CSphAggregateHit tHit;

		// initialize hitlist encoder state
		tHitBuilder.HitReset();

		// initial fill
		CSphFixedVector<BYTE> dActive ( iRawBlocks );
		for ( int i=0; i<iRawBlocks; i++ )
		{
			if ( !dBins[i].ReadHit ( &tHit ) )
			{
				m_sLastError.SetSprintf ( "sort_hits: warmup failed (io error?)" );
				return 0;
			}
			dActive[i] = ( tHit.m_uWordID!=0 );
			if ( dActive[i] )
				tQueue.Push ( tHit, i );
		}

		// init progress meter
		tProgress.PhaseBegin ( CSphIndexProgress::PHASE_SORT );
		assert ( !tProgress.m_iHits );
		tProgress.m_iHitsTotal = iHitsTotal;

		// while the queue has data for us
		// FIXME! analyze binsRead return code
		int iHitsSorted = 0;
		int iMinBlock = -1;
		while ( tQueue.m_iUsed )
		{
			int iBin = tQueue.m_pData->m_iBin;

			// pack and emit queue root
			if ( m_bInplaceSettings )
			{
				if ( iMinBlock==-1 || dBins[iMinBlock].IsEOF () || !dActive[iMinBlock] )
				{
					iMinBlock = -1;
					ARRAY_FOREACH ( i, dBins )
						if ( !dBins[i].IsEOF () && dActive[i] && ( iMinBlock==-1 || dBins[i].m_iFilePos < dBins[iMinBlock].m_iFilePos ) )
							iMinBlock = i;
				}

				int iToWriteMax = 3*sizeof(DWORD);
				if ( iMinBlock!=-1 && ( tHitBuilder.GetHitfilePos() + iToWriteMax ) > dBins[iMinBlock].m_iFilePos )
				{
					if ( !RelocateBlock ( fdHits.GetFD (), dRelocationBuffer.Begin(), iRelocationSize, &iHitFileSize, dBins[iMinBlock], &iSharedOffset ) )
						return 0;

					iMinBlock = (iMinBlock+1) % dBins.GetLength ();
				}
			}

			tHitBuilder.cidxHit ( tQueue.m_pData );
			if ( tHitBuilder.IsError() )
				return 0;

			// pop queue root and push next hit from popped bin
			tQueue.Pop ();
			if ( dActive[iBin] )
			{
				dBins[iBin].ReadHit ( &tHit );
				dActive[iBin] = ( tHit.m_uWordID!=0 );
				if ( dActive[iBin] )
					tQueue.Push ( tHit, iBin );
			}

			// progress
			if ( ++iHitsSorted==1000000 )
			{
				tProgress.m_iHits += iHitsSorted;
				tProgress.Show();
				iHitsSorted = 0;
			}
		}

		tProgress.m_iHits = tProgress.m_iHitsTotal; // sum might be less than total because of dupes!
		tProgress.PhaseEnd();
		dBins.Reset ();

		CSphAggregateHit tFlush;
		tFlush.m_tRowID = INVALID_ROWID;
		tFlush.m_uWordID = 0;
		tFlush.m_sKeyword = NULL;
		tFlush.m_iWordPos = EMPTY_HIT;
		tFlush.m_dFieldMask.UnsetAll();
		tHitBuilder.cidxHit ( &tFlush );

		if ( m_bInplaceSettings )
		{
			tHitBuilder.CloseHitlist();
			if ( !sphTruncate ( fdHits.GetFD () ) )
				sphWarn ( "failed to truncate %s", fdHits.GetFilename() );
		}
	}

	BuildHeader_t tBuildHeader ( m_tStats );
	if ( !tHitBuilder.cidxDone ( iMemoryLimit, m_tSettings.m_iMinInfixLen, m_pTokenizer->GetMaxCodepointLength(), &tBuildHeader ) )
		return 0;

	dRelocationBuffer.Reset(0);

	tBuildHeader.m_iDocinfo = m_tStats.m_iTotalDocuments;
	tBuildHeader.m_iDocinfoIndex = m_iDocinfoIndex;
	tBuildHeader.m_iMinMaxIndex = m_iMinMaxIndex;

	CSphString sHeaderName = GetIndexFileName(SPH_EXT_SPH);
	WriteHeader_t tWriteHeader;
	tWriteHeader.m_pSettings = &m_tSettings;
	tWriteHeader.m_pSchema = &m_tSchema;
	tWriteHeader.m_pTokenizer = m_pTokenizer;
	tWriteHeader.m_pDict = m_pDict;
	tWriteHeader.m_pFieldFilter = m_pFieldFilter;
	tWriteHeader.m_pFieldLens = m_dFieldLens.Begin();

	// we're done
	if ( !IndexBuildDone ( tBuildHeader, tWriteHeader, sHeaderName, m_sLastError ) )
		return 0;

	// when the party's over..
	ARRAY_FOREACH ( i, dSources )
		dSources[i]->PostIndex ();

	dFileWatchdog.AllIsDone();
	return 1;
} // NOLINT function length


/////////////////////////////////////////////////////////////////////////////
// MERGER HELPERS
/////////////////////////////////////////////////////////////////////////////

BYTE sphDoclistHintPack ( SphOffset_t iDocs, SphOffset_t iLen )
{
	// we won't really store a hint for small lists
	if ( iDocs<DOCLIST_HINT_THRESH )
		return 0;

	// for bigger lists len/docs varies 4x-6x on test indexes
	// so lets assume that 4x-8x should be enough for everybody
	SphOffset_t iDelta = Min ( Max ( iLen-4*iDocs, 0 ), 4*iDocs-1 ); // len delta over 4x, clamped to [0x..4x) range
	BYTE uHint = (BYTE)( 64*iDelta/iDocs ); // hint now must be in [0..256) range
	while ( uHint<255 && ( iDocs*uHint/64 )<iDelta ) // roundoff (suddenly, my guru math skillz failed me)
		uHint++;

	return uHint;
}

// !COMMIT eliminate this, move to dict (or at least couple with CWordlist)
template<bool WORDDICT>
class CSphDictReader
{
public:
	// current word
	SphWordID_t		m_uWordID = 0;
	SphOffset_t		m_iDoclistOffset = 0;
	int				m_iDocs = 0;
	int				m_iHits = 0;
	bool			m_bHasHitlist = true;
	int				m_iHint = 0;

private:
	ESphHitless		m_eHitless { SPH_HITLESS_NONE };
	CSphAutoreader	m_tMyReader;
	CSphReader *	m_pReader = nullptr;
	SphOffset_t		m_iMaxPos = 0;
	int				m_iSkiplistBlockSize = 0;
	char			m_sWord[MAX_KEYWORD_BYTES];
	int				m_iCheckpoint = 1;

public:
	CSphDictReader ( int iSkiplistBlockSize )
		: m_iSkiplistBlockSize ( iSkiplistBlockSize )
	{
		m_sWord[0] = '\0';
	}

	bool Setup ( const CSphString & sFilename, SphOffset_t iMaxPos, ESphHitless eHitless, CSphString & sError )
	{
		if ( !m_tMyReader.Open ( sFilename, sError ) )
			return false;

		Setup ( &m_tMyReader, iMaxPos, eHitless );
		return true;
	}

	void Setup ( CSphReader * pReader, SphOffset_t iMaxPos, ESphHitless eHitless )
	{
		m_pReader = pReader;
		m_pReader->SeekTo ( 1, READ_NO_SIZE_HINT );

		m_iMaxPos = iMaxPos;
		m_eHitless = eHitless;
		m_sWord[0] = '\0';
		m_iCheckpoint = 1;
	}

	bool Read()
	{
		assert ( m_iSkiplistBlockSize>0 );

		if ( m_pReader->GetPos()>=m_iMaxPos )
			return false;

		// get leading value
		SphWordID_t iWord0 = WORDDICT ? m_pReader->GetByte() : m_pReader->UnzipWordid();
		if ( !iWord0 )
		{
			// handle checkpoint
			m_iCheckpoint++;
			m_pReader->UnzipOffset();

			m_uWordID = 0;
			m_iDoclistOffset = 0;
			m_sWord[0] = '\0';

			if ( m_pReader->GetPos()>=m_iMaxPos )
				return false;

			iWord0 = WORDDICT ? m_pReader->GetByte() : m_pReader->UnzipWordid(); // get next word
		}
		if ( !iWord0 )
			return false; // some failure

		// get word entry
		if_const ( WORDDICT )
		{
			// unpack next word
			// must be in sync with DictEnd()!
			assert ( iWord0<=255 );
			auto uPack = (BYTE) iWord0;

			int iMatch, iDelta;
			if ( uPack & 0x80 )
			{
				iDelta = ( ( uPack>>4 ) & 7 ) + 1;
				iMatch = uPack & 15;
			} else
			{
				iDelta = uPack & 127;
				iMatch = m_pReader->GetByte();
			}
			assert ( iMatch+iDelta<(int)sizeof(m_sWord)-1 );
			assert ( iMatch<=(int)strlen(m_sWord) );

			m_pReader->GetBytes ( m_sWord + iMatch, iDelta );
			m_sWord [ iMatch+iDelta ] = '\0';

			m_iDoclistOffset = m_pReader->UnzipOffset();
			m_iDocs = m_pReader->UnzipInt();
			m_iHits = m_pReader->UnzipInt();
			m_iHint = 0;
			if ( m_iDocs>=DOCLIST_HINT_THRESH )
				m_iHint = m_pReader->GetByte();
			if ( m_iDocs > m_iSkiplistBlockSize )
				m_pReader->UnzipInt();

			m_uWordID = (SphWordID_t) sphCRC32 ( GetWord() ); // set wordID for indexing

		} else
		{
			m_uWordID += iWord0;
			m_iDoclistOffset += m_pReader->UnzipOffset();
			m_iDocs = m_pReader->UnzipInt();
			m_iHits = m_pReader->UnzipInt();
			if ( m_iDocs > m_iSkiplistBlockSize )
				m_pReader->UnzipOffset();
		}

		m_bHasHitlist =
			( m_eHitless==SPH_HITLESS_NONE ) ||
			( m_eHitless==SPH_HITLESS_SOME && !( m_iDocs & HITLESS_DOC_FLAG ) );
		m_iDocs = m_eHitless==SPH_HITLESS_SOME ? ( m_iDocs & HITLESS_DOC_MASK ) : m_iDocs;

		return true; // FIXME? errorflag?
	}

	int CmpWord ( const CSphDictReader & tOther ) const
	{
		if_const ( WORDDICT )
			return strcmp ( m_sWord, tOther.m_sWord );
		return ( m_uWordID < tOther.m_uWordID ) ? -1 : ( m_uWordID == tOther.m_uWordID ? 0 : 1 );
	}

	BYTE * GetWord() const { return (BYTE *)const_cast<char*>(m_sWord); }

	int GetCheckpoint() const { return m_iCheckpoint; }
};


ISphFilter * CSphIndex_VLN::CreateMergeFilters ( const VecTraits_T<CSphFilterSettings> & dSettings ) const
{
	CSphString sError, sWarning;
	ISphFilter * pResult = nullptr;
	CreateFilterContext_t tCtx;
	tCtx.m_pSchema = &m_tSchema;
	tCtx.m_pBlobPool = m_tBlobAttrs.GetReadPtr();

	for ( const auto& dSetting : dSettings )
		pResult = sphJoinFilters ( pResult, sphCreateFilter ( dSetting, tCtx, sError, sWarning ) );

	if ( pResult )
		pResult->SetColumnar(m_pColumnar.Ptr());

	return pResult;
}

static bool CheckDocsCount ( int64_t iDocs, CSphString & sError )
{
	if ( iDocs<INT_MAX )
		return true;

	sError.SetSprintf ( "index over %d documents not supported (got " INT64_FMT " documents)", INT_MAX, iDocs );
	return false;
}

namespace QwordIteration
{
	template<typename QWORD>
	inline void PrepareQword ( QWORD & tQword, const CSphDictReader<QWORD::is_worddict::value> & tReader ) //NOLINT
	{
		tQword.m_tDoc.m_tRowID = INVALID_ROWID;

		tQword.m_iDocs = tReader.m_iDocs;
		tQword.m_iHits = tReader.m_iHits;
		tQword.m_bHasHitlist = tReader.m_bHasHitlist;

		tQword.m_uHitPosition = 0;
		tQword.m_iHitlistPos = 0;

		if_const ( QWORD::is_worddict::value )
			tQword.m_rdDoclist->SeekTo ( tReader.m_iDoclistOffset, tReader.m_iHint );
	}

	template<typename QWORD>
	inline bool NextDocument ( QWORD & tQword, const CSphIndex_VLN * pSourceIndex )
	{
		while (true)
		{
			tQword.GetNextDoc();
			if ( tQword.m_tDoc.m_tRowID==INVALID_ROWID )
				return false;

			tQword.SeekHitlist ( tQword.m_iHitlistPos );
			return true;
		}
	}

	template < typename QWORD >
	inline bool NextDocument ( QWORD & tQword, const CSphIndex_VLN * pSourceIndex, const VecTraits_T<RowID_t> & dRows )
	{
		while (true)
		{
			tQword.GetNextDoc();
			if ( tQword.m_tDoc.m_tRowID==INVALID_ROWID )
				return false;

			if ( dRows[tQword.m_tDoc.m_tRowID]==INVALID_ROWID )
				continue;

			tQword.SeekHitlist ( tQword.m_iHitlistPos );
			return true;
		}
	}

	template<typename QWORD>
	inline void ConfigureQword ( QWORD & tQword, DataReaderFactory_c * pHits, DataReaderFactory_c * pDocs, int iDynamic )
	{
		tQword.SetHitReader ( pHits );
		tQword.m_rdHitlist->SeekTo ( 1, READ_NO_SIZE_HINT );

		tQword.SetDocReader ( pDocs );
		tQword.m_rdDoclist->SeekTo ( 1, READ_NO_SIZE_HINT );

		tQword.m_tDoc.Reset ( iDynamic );
	}
}; // namespace QwordIteration

class CSphMerger
{
public:
	explicit CSphMerger ( CSphHitBuilder * pHitBuilder )
		: m_pHitBuilder ( pHitBuilder )
	{}

	template < typename QWORD >
	inline void TransferData ( QWORD & tQword, SphWordID_t iWordID, const BYTE * sWord,
							const CSphIndex_VLN * pSourceIndex, const VecTraits_T<RowID_t>& dRows,
							MergeCb_c & tMonitor )
	{
		CSphAggregateHit tHit;
		tHit.m_uWordID = iWordID;
		tHit.m_sKeyword = sWord;
		tHit.m_dFieldMask.UnsetAll();

		while ( QwordIteration::NextDocument ( tQword, pSourceIndex, dRows ) && !tMonitor.NeedStop() )
		{
			if ( tQword.m_bHasHitlist )
				TransferHits ( tQword, tHit, dRows );
			else
			{
				// convert to aggregate if there is no hit-list
				tHit.m_tRowID = dRows[tQword.m_tDoc.m_tRowID];
				tHit.m_dFieldMask = tQword.m_dQwordFields;
				tHit.SetAggrCount ( tQword.m_uMatchHits );
				m_pHitBuilder->cidxHit ( &tHit );
			}
		}
	}

	template < typename QWORD >
	inline void TransferHits ( QWORD & tQword, CSphAggregateHit & tHit, const VecTraits_T<RowID_t> & dRows )
	{
		assert ( tQword.m_bHasHitlist );
		tHit.m_tRowID = dRows[tQword.m_tDoc.m_tRowID];
		for ( Hitpos_t uHit = tQword.GetNextHit(); uHit!=EMPTY_HIT; uHit = tQword.GetNextHit() )
		{
			tHit.m_iWordPos = uHit;
			m_pHitBuilder->cidxHit ( &tHit );
		}
	}

private:
	CSphHitBuilder *	m_pHitBuilder;
};


template < typename QWORDDST, typename QWORDSRC >
bool CSphIndex_VLN::MergeWords ( const CSphIndex_VLN * pDstIndex, const CSphIndex_VLN * pSrcIndex, VecTraits_T<RowID_t> dDstRows, VecTraits_T<RowID_t> dSrcRows, CSphHitBuilder * pHitBuilder, CSphString & sError, CSphIndexProgress & tProgress )
{
	auto& tMonitor = tProgress.GetMergeCb();
	CSphAutofile tDummy;
	pHitBuilder->CreateIndexFiles ( pDstIndex->GetIndexFileName("tmp.spd").cstr(), pDstIndex->GetIndexFileName("tmp.spp").cstr(), pDstIndex->GetIndexFileName("tmp.spe").cstr(), false, 0, tDummy, nullptr );

	static_assert ( QWORDDST::is_worddict::value == QWORDDST::is_worddict::value, "can't merge worddict with non-worddict" );

	CSphDictReader<QWORDDST::is_worddict::value> tDstReader ( pDstIndex->GetSettings().m_iSkiplistBlockSize );
	CSphDictReader<QWORDSRC::is_worddict::value> tSrcReader ( pSrcIndex->GetSettings().m_iSkiplistBlockSize );

	/// compress means: I don't want true merge, I just want to apply deadrows and filter
	bool bCompress = pDstIndex==pSrcIndex;

	if ( !tDstReader.Setup ( pDstIndex->GetIndexFileName(SPH_EXT_SPI), pDstIndex->m_tWordlist.GetWordsEnd(), pDstIndex->m_tSettings.m_eHitless, sError ) )
		return false;
	if ( !bCompress && !tSrcReader.Setup ( pSrcIndex->GetIndexFileName(SPH_EXT_SPI), pSrcIndex->m_tWordlist.GetWordsEnd(), pSrcIndex->m_tSettings.m_eHitless, sError ) )
		return false;

	/// prepare for indexing
	pHitBuilder->HitblockBegin();
	pHitBuilder->HitReset();

	/// setup qwords

	QWORDDST tDstQword ( false, false, pDstIndex->GetIndexId() );
	QWORDSRC tSrcQword ( false, false, pSrcIndex->GetIndexId() );

	DataReaderFactoryPtr_c tSrcDocs {
		NewProxyReader ( pSrcIndex->GetIndexFileName ( SPH_EXT_SPD ), sError,
			DataReaderFactory_c::DOCS, pSrcIndex->m_tMutableSettings.m_tFileAccess.m_iReadBufferDocList, FileAccess_e::FILE )
	};
	if ( !tSrcDocs )
		return false;

	DataReaderFactoryPtr_c tSrcHits {
		NewProxyReader ( pSrcIndex->GetIndexFileName ( SPH_EXT_SPP ), sError,
			DataReaderFactory_c::HITS, pSrcIndex->m_tMutableSettings.m_tFileAccess.m_iReadBufferHitList, FileAccess_e::FILE  )
	};
	if ( !tSrcHits )
		return false;

	if ( !sError.IsEmpty () || tMonitor.NeedStop () )
		return false;

	DataReaderFactoryPtr_c tDstDocs {
		NewProxyReader ( pDstIndex->GetIndexFileName ( SPH_EXT_SPD ), sError,
			DataReaderFactory_c::DOCS, pDstIndex->m_tMutableSettings.m_tFileAccess.m_iReadBufferDocList, FileAccess_e::FILE )
	};
	if ( !tDstDocs )
		return false;

	DataReaderFactoryPtr_c tDstHits {
		NewProxyReader ( pDstIndex->GetIndexFileName ( SPH_EXT_SPP ), sError,
			DataReaderFactory_c::HITS, pDstIndex->m_tMutableSettings.m_tFileAccess.m_iReadBufferHitList, FileAccess_e::FILE )
	};
	if ( !tDstHits )
		return false;

	if ( !sError.IsEmpty() || tMonitor.NeedStop () )
		return false;

	CSphMerger tMerger(pHitBuilder);

	QwordIteration::ConfigureQword<QWORDDST> ( tDstQword, tDstHits, tDstDocs, pDstIndex->m_tSchema.GetDynamicSize() );
	QwordIteration::ConfigureQword<QWORDSRC> ( tSrcQword, tSrcHits, tSrcDocs, pSrcIndex->m_tSchema.GetDynamicSize() );

	/// merge

	bool bDstWord = tDstReader.Read();
	bool bSrcWord = !bCompress && tSrcReader.Read();

	tProgress.PhaseBegin ( CSphIndexProgress::PHASE_MERGE );
	tProgress.Show();

	int iWords = 0;
	int iHitlistsDiscarded = 0;
	for ( ; bDstWord || bSrcWord; ++iWords )
	{
		if ( iWords==1000 )
		{
			tProgress.m_iWords += 1000;
			tProgress.Show();
			iWords = 0;
		}

		if ( tMonitor.NeedStop () )
			return false;

		const int iCmp = bCompress ? -1 : tDstReader.CmpWord ( tSrcReader );

		if ( !bSrcWord || ( bDstWord && iCmp<0 ) )
		{
			// transfer documents and hits from destination
			QwordIteration::PrepareQword<QWORDDST> ( tDstQword, tDstReader );
			tMerger.TransferData<QWORDDST> ( tDstQword, tDstReader.m_uWordID, tDstReader.GetWord(), pDstIndex, dDstRows, tMonitor );
			bDstWord = tDstReader.Read();

		} else if ( !bDstWord || ( bSrcWord && iCmp>0 ) )
		{
			// transfer documents and hits from source
			QwordIteration::PrepareQword<QWORDSRC> ( tSrcQword, tSrcReader );
			tMerger.TransferData<QWORDSRC> ( tSrcQword, tSrcReader.m_uWordID, tSrcReader.GetWord(), pSrcIndex, dSrcRows, tMonitor );
			bSrcWord = tSrcReader.Read();

		} else // merge documents and hits inside the word
		{
			assert ( iCmp==0 );

			bool bHitless = !tDstReader.m_bHasHitlist;
			if ( tDstReader.m_bHasHitlist!=tSrcReader.m_bHasHitlist )
			{
				++iHitlistsDiscarded;
				bHitless = true;
			}

			QwordIteration::PrepareQword<QWORDDST> ( tDstQword, tDstReader );
			QwordIteration::PrepareQword<QWORDSRC> ( tSrcQword, tSrcReader );

			CSphAggregateHit tHit;
			tHit.m_uWordID = tDstReader.m_uWordID; // !COMMIT m_sKeyword anyone?
			tHit.m_sKeyword = tDstReader.GetWord();
			tHit.m_dFieldMask.UnsetAll();

			// we assume that all the duplicates have been removed
			// and we don't need to merge hits from the same document

			// transfer hits from destination
			while ( QwordIteration::NextDocument ( tDstQword, pDstIndex, dDstRows ) )
			{
				if ( tMonitor.NeedStop () )
					return false;

				if ( bHitless )
				{
					while ( tDstQword.m_bHasHitlist && tDstQword.GetNextHit()!=EMPTY_HIT );

					tHit.m_tRowID = dDstRows[tDstQword.m_tDoc.m_tRowID];
					tHit.m_dFieldMask = tDstQword.m_dQwordFields;
					tHit.SetAggrCount ( tDstQword.m_uMatchHits );
					pHitBuilder->cidxHit ( &tHit );
				} else
					tMerger.TransferHits ( tDstQword, tHit, dDstRows );
			}

			// transfer hits from source
			while ( QwordIteration::NextDocument ( tSrcQword, pSrcIndex, dSrcRows ) )
			{
				if ( tMonitor.NeedStop () )
					return false;

				if ( bHitless )
				{
					while ( tSrcQword.m_bHasHitlist && tSrcQword.GetNextHit()!=EMPTY_HIT );

					tHit.m_tRowID = dSrcRows[tSrcQword.m_tDoc.m_tRowID];
					tHit.m_dFieldMask = tSrcQword.m_dQwordFields;
					tHit.SetAggrCount ( tSrcQword.m_uMatchHits );
					pHitBuilder->cidxHit ( &tHit );
				} else
					tMerger.TransferHits ( tSrcQword, tHit, dSrcRows );
			}

			// next word
			bDstWord = tDstReader.Read();
			bSrcWord = tSrcReader.Read();
		}
	}

	tProgress.m_iWords += iWords;
	tProgress.Show();

	if ( iHitlistsDiscarded )
		sphWarning ( "discarded hitlists for %u words", iHitlistsDiscarded );

	return true;
}


bool CSphIndex_VLN::Merge ( CSphIndex * pSource, const VecTraits_T<CSphFilterSettings> & dFilters, bool bSupressDstDocids, CSphIndexProgress& tProgress )
{
	// if no source provided - special pass. No preload/preread, just merge with filters
	if ( pSource )
	{
		{
			ResetFileAccess(this);

			StrVec_t dWarnings;
			if ( !Prealloc ( false, nullptr, dWarnings ) )
				return false;

			for ( const auto & i : dWarnings )
				sphWarn ( "%s", i.cstr() );

			Preread();
		}

		{
			ResetFileAccess(pSource);

			StrVec_t dWarnings;
			if ( !pSource->Prealloc ( false, nullptr, dWarnings ) )
			{
				m_sLastError.SetSprintf ( "source index preload failed: %s", pSource->GetLastError().cstr() );
				return false;
			}

			for ( const auto & i : dWarnings )
				sphWarn ( "%s", i.cstr() );

			pSource->Preread();
		}
	} else
	{
		if ( dFilters.IsEmpty() )
		{
			m_sLastError.SetSprintf ( "no source, no filters - nothing to merge" );
			return false;
		}
		// prepare for self merging - no supress dst, source same as destination. Will apply klists/filters only.
		bSupressDstDocids = false;
		pSource = this;
	}

	// create filters
	CSphScopedPtr<ISphFilter> pFilter(nullptr);
	pFilter = CreateMergeFilters ( dFilters );

	return CSphIndex_VLN::DoMerge ( this, (const CSphIndex_VLN *)pSource, pFilter.Ptr(), m_sLastError, tProgress, false, bSupressDstDocids );
}


std::pair<DWORD,DWORD> CSphIndex_VLN::CreateRowMapsAndCountTotalDocs ( const CSphIndex_VLN* pSrcIndex, const CSphIndex_VLN* pDstIndex, CSphFixedVector<RowID_t>& dSrcRowMap, CSphFixedVector<RowID_t>& dDstRowMap, const ISphFilter* pFilter, bool bSupressDstDocids, MergeCb_c& tMonitor )
{
	if ( pSrcIndex!=pDstIndex )
		dSrcRowMap.Reset ( pSrcIndex->m_iDocinfo );
	dDstRowMap.Reset ( pDstIndex->m_iDocinfo );

	int iStride = pDstIndex->m_tSchema.GetRowSize();

	DeadRowMap_Ram_c tExtraDeadMap(0);
	if ( bSupressDstDocids ) // skip monitoring supressions as they're used _only_ from indexer call and never from flavours of optimize.
	{
		tExtraDeadMap.Reset ( dDstRowMap.GetLength() );

		LookupReaderIterator_c tDstLookupReader ( pDstIndex->m_tDocidLookup.GetWritePtr() );
		LookupReaderIterator_c tSrcLookupReader ( pSrcIndex->m_tDocidLookup.GetWritePtr() );

		KillByLookup ( tDstLookupReader, tSrcLookupReader, tExtraDeadMap, [] (DocID_t) {} );
	}

	dSrcRowMap.Fill ( INVALID_ROWID );
	dDstRowMap.Fill ( INVALID_ROWID );

	const DWORD * pRow = pDstIndex->m_tAttr.GetWritePtr();
	int64_t iTotalDocs = 0;
	std::pair<DWORD, DWORD> tPerIndexDocs {0,0};

	// say to observer we're going to collect alive rows from dst index
	// (kills directed to that index must be collected to reapply at the finish)
	tMonitor.SetEvent ( MergeCb_c::E_COLLECT_START, pDstIndex->m_iChunk );
	for ( int i = 0; i < dDstRowMap.GetLength(); ++i, pRow+=iStride )
	{
		if ( pDstIndex->m_tDeadRowMap.IsSet(i) )
			continue;

		if ( bSupressDstDocids && tExtraDeadMap.IsSet(i) )
			continue;

		if ( pFilter )
		{
			CSphMatch tFakeMatch;
			tFakeMatch.m_tRowID = i;
			tFakeMatch.m_pStatic = pRow;
			if ( !pFilter->Eval ( tFakeMatch ) )
				continue;
		}

		dDstRowMap[i] = (RowID_t)iTotalDocs++;
	}
	tMonitor.SetEvent ( MergeCb_c::E_COLLECT_FINISHED, pDstIndex->m_iChunk );
	tPerIndexDocs.first = (DWORD)iTotalDocs;
	if ( dSrcRowMap.IsEmpty() )
		return tPerIndexDocs;

	// say to observer we're going to collect alive rows from src index (again, issue to kills).
	tMonitor.SetEvent ( MergeCb_c::E_COLLECT_START, pSrcIndex->m_iChunk );
	for ( int i = 0; i < dSrcRowMap.GetLength(); ++i )
	{
		if ( pSrcIndex->m_tDeadRowMap.IsSet(i) )
			continue;

		dSrcRowMap[i] = (RowID_t)iTotalDocs++;
	}
	tMonitor.SetEvent ( MergeCb_c::E_COLLECT_FINISHED, pSrcIndex->m_iChunk );
	tPerIndexDocs.second = DWORD ( iTotalDocs - tPerIndexDocs.first );
	return tPerIndexDocs;
}


bool AttrMerger_c::Prepare ( const CSphIndex_VLN* pSrcIndex, const CSphIndex_VLN* pDstIndex )
{
	auto sSPA = pDstIndex->GetIndexFileName ( SPH_EXT_SPA, true );
	if ( pDstIndex->m_tSchema.HasNonColumnarAttrs() && !m_tWriterSPA.OpenFile ( sSPA, m_sError ) )
		return false;

	if ( pDstIndex->m_tSchema.HasBlobAttrs() )
	{
		m_pBlobRowBuilder = sphCreateBlobRowBuilder ( pSrcIndex->m_tSchema, pDstIndex->GetIndexFileName ( SPH_EXT_SPB, true ), pSrcIndex->GetSettings().m_tBlobUpdateSpace, m_sError );
		if ( !m_pBlobRowBuilder )
			return false;
	}

	if ( pDstIndex->m_pDocstore )
	{
		m_pDocstoreBuilder = CreateDocstoreBuilder ( pDstIndex->GetIndexFileName ( SPH_EXT_SPDS, true ), pDstIndex->m_pDocstore->GetDocstoreSettings(), m_sError );
		if ( !m_pDocstoreBuilder )
			return false;

		for ( int i = 0; i < pDstIndex->m_tSchema.GetFieldsCount(); ++i )
			if ( pDstIndex->m_tSchema.IsFieldStored ( i ) )
				m_pDocstoreBuilder->AddField ( pDstIndex->m_tSchema.GetFieldName ( i ), DOCSTORE_TEXT );
	}

	if ( pDstIndex->m_tSchema.HasColumnarAttrs() )
	{
		m_pColumnarBuilder = CreateColumnarBuilder ( pDstIndex->m_tSchema, pDstIndex->m_tSettings, pDstIndex->GetIndexFileName ( SPH_EXT_SPC, true ), m_sError );
		if ( !m_pColumnarBuilder )
			return false;
	}

	m_tMinMax.Init ( pDstIndex->m_tSchema );

	m_dDocidLookup.Reset ( m_iTotalDocs );
	CreateHistograms ( m_tHistograms, m_dAttrsForHistogram, pDstIndex->m_tSchema );

	m_tResultRowID = 0;
	return true;
}


bool AttrMerger_c::CopyPureColumnarAttributes ( const CSphIndex_VLN& tIndex, const VecTraits_T<RowID_t>& dRowMap )
{
	assert ( !tIndex.m_tAttr.GetWritePtr() );
	assert ( tIndex.m_tSchema.GetAttr ( 0 ).IsColumnar() );

	auto dColumnarIterators = CreateAllColumnarIterators ( tIndex.m_pColumnar.Ptr(), tIndex.m_tSchema );
	CSphVector<int64_t> dTmp;

	int iChunk = tIndex.m_iChunk;
	m_tMonitor.SetEvent ( MergeCb_c::E_MERGEATTRS_START, iChunk );
	auto _ = AtScopeExit ( [this, iChunk] { m_tMonitor.SetEvent ( MergeCb_c::E_MERGEATTRS_FINISHED, iChunk ); } );
	for ( RowID_t tRowID = 0, tRows = (RowID_t)dRowMap.GetLength64(); tRowID < tRows; ++tRowID )
	{
		if ( dRowMap[tRowID] == INVALID_ROWID )
			continue;

		if ( m_tMonitor.NeedStop() )
			return false;

		// limit granted by caller code
		assert ( m_tResultRowID != INVALID_ROWID );

		ARRAY_FOREACH ( i, dColumnarIterators )
		{
			auto& tIt = dColumnarIterators[i];
			Verify ( AdvanceIterator ( tIt.first.get(), tRowID ) );
			SetColumnarAttr ( i, tIt.second, m_pColumnarBuilder, tIt.first.get(), dTmp );
		}

		ARRAY_FOREACH ( i, m_dAttrsForHistogram )
			m_tHistograms.Insert ( i, m_dAttrsForHistogram[i].Get ( nullptr, dColumnarIterators ) );

		if ( m_pDocstoreBuilder )
			m_pDocstoreBuilder->AddDoc ( m_tResultRowID, tIndex.m_pDocstore->GetDoc ( tRowID, nullptr, -1, false ) );

		m_dDocidLookup[m_tResultRowID] = { dColumnarIterators[0].first->Get(), m_tResultRowID };
		++m_tResultRowID;
	}
	return true;
}

bool AttrMerger_c::CopyMixedAttributes ( const CSphIndex_VLN& tIndex, const VecTraits_T<RowID_t>& dRowMap )
{
	auto dColumnarIterators = CreateAllColumnarIterators ( tIndex.m_pColumnar.Ptr(), tIndex.m_tSchema );
	CSphVector<int64_t> dTmp;

	int iColumnarIdLoc = tIndex.m_tSchema.GetAttr ( 0 ).IsColumnar() ? 0 : - 1;
	const CSphRowitem* pRow = tIndex.m_tAttr.GetWritePtr();
	assert ( pRow );
	int iStride = tIndex.m_tSchema.GetRowSize();
	CSphFixedVector<CSphRowitem> dTmpRow ( iStride );
	auto iStrideBytes = dTmpRow.GetLengthBytes();
	const CSphColumnInfo* pBlobLocator = tIndex.m_tSchema.GetAttr ( sphGetBlobLocatorName() );

	int iChunk = tIndex.m_iChunk;
	m_tMonitor.SetEvent ( MergeCb_c::E_MERGEATTRS_START, iChunk );
	auto _ = AtScopeExit ( [this, iChunk] { m_tMonitor.SetEvent ( MergeCb_c::E_MERGEATTRS_FINISHED, iChunk ); } );
	for ( RowID_t tRowID = 0, tRows = (RowID_t)dRowMap.GetLength64(); tRowID < tRows; ++tRowID, pRow += iStride )
	{
		if ( dRowMap[tRowID] == INVALID_ROWID )
			continue;

		if ( m_tMonitor.NeedStop() )
			return false;

		// limit granted by caller code
		assert ( m_tResultRowID != INVALID_ROWID );

		m_tMinMax.Collect ( pRow );

		if ( m_pBlobRowBuilder )
		{
			const BYTE* pOldBlobRow = tIndex.m_tBlobAttrs.GetWritePtr() + sphGetRowAttr ( pRow, pBlobLocator->m_tLocator );
			uint64_t	uNewOffset	= m_pBlobRowBuilder->Flush ( pOldBlobRow );

			memcpy ( dTmpRow.Begin(), pRow, iStrideBytes );
			sphSetRowAttr ( dTmpRow.Begin(), pBlobLocator->m_tLocator, uNewOffset );

			m_tWriterSPA.PutBytes ( dTmpRow.Begin(), iStrideBytes );
		} else if ( iStrideBytes )
			m_tWriterSPA.PutBytes ( pRow, iStrideBytes );

		ARRAY_FOREACH ( i, dColumnarIterators )
		{
			auto& tIt = dColumnarIterators[i];
			Verify ( AdvanceIterator ( tIt.first.get(), tRowID ) );
			SetColumnarAttr ( i, tIt.second, m_pColumnarBuilder, tIt.first.get(), dTmp );
		}

		DocID_t tDocID = iColumnarIdLoc >= 0 ? dColumnarIterators[iColumnarIdLoc].first->Get() : sphGetDocID ( pRow );

		ARRAY_FOREACH ( i, m_dAttrsForHistogram )
			m_tHistograms.Insert ( i, m_dAttrsForHistogram[i].Get ( pRow, dColumnarIterators ) );

		if ( m_pDocstoreBuilder )
			m_pDocstoreBuilder->AddDoc ( m_tResultRowID, tIndex.m_pDocstore->GetDoc ( tRowID, nullptr, -1, false ) );

		m_dDocidLookup[m_tResultRowID] = { tDocID, m_tResultRowID };
		++m_tResultRowID;
	}
	return true;
}


bool AttrMerger_c::CopyAttributes ( const CSphIndex_VLN& tIndex, const VecTraits_T<RowID_t>& dRowMap, DWORD uAlive )
{
	if ( !uAlive )
		return true;

	// that is very empyric, however is better than nothing.
	m_iTotalBytes += tIndex.m_tStats.m_iTotalBytes * ( (float)uAlive / (float)dRowMap.GetLength64() );

	if ( !tIndex.m_tAttr.GetWritePtr() )
		return CopyPureColumnarAttributes( tIndex, dRowMap );
	return CopyMixedAttributes ( tIndex, dRowMap );
}


bool AttrMerger_c::FinishMergeAttributes ( const CSphIndex_VLN* pDstIndex, BuildHeader_t& tBuildHeader )
{
	m_tMinMax.FinishCollect();
	assert ( m_tResultRowID==m_iTotalDocs );
	tBuildHeader.m_iDocinfo = m_iTotalDocs;
	tBuildHeader.m_iTotalDocuments = m_iTotalDocs;
	tBuildHeader.m_iTotalBytes = m_iTotalBytes;

	m_dDocidLookup.Sort ( CmpDocidLookup_fn() );
	if ( !WriteDocidLookup ( pDstIndex->GetIndexFileName ( SPH_EXT_SPT, true ), m_dDocidLookup, m_sError ) )
		return false;

	if ( pDstIndex->m_tSchema.HasNonColumnarAttrs() )
	{
		if ( m_iTotalDocs )
		{
			tBuildHeader.m_iMinMaxIndex = m_tWriterSPA.GetPos() / sizeof(CSphRowitem);
			const auto& dMinMaxRows		 = m_tMinMax.GetCollected();
			m_tWriterSPA.PutBytes ( dMinMaxRows.Begin(), dMinMaxRows.GetLengthBytes64() );
			tBuildHeader.m_iDocinfoIndex = ( dMinMaxRows.GetLength() / pDstIndex->m_tSchema.GetRowSize() / 2 ) - 1;
		}

		m_tWriterSPA.CloseFile();
		if ( m_tWriterSPA.IsError() )
			return false;
	}

	if ( m_pBlobRowBuilder && !m_pBlobRowBuilder->Done(m_sError) )
		return false;

	std::string sErrorSTL;
	if ( m_pColumnarBuilder && !m_pColumnarBuilder->Done(sErrorSTL) )
	{
		m_sError = sErrorSTL.c_str();
		return false;
	}

	if ( !m_tHistograms.Save ( pDstIndex->GetIndexFileName ( SPH_EXT_SPHI, true ), m_sError ) )
		return false;

	if ( !CheckDocsCount ( m_tResultRowID, m_sError ) )
		return false;

	if ( m_pDocstoreBuilder )
		m_pDocstoreBuilder->Finalize();

	return WriteDeadRowMap ( pDstIndex->GetIndexFileName ( SPH_EXT_SPM, true ), m_tResultRowID, m_sError );
}


bool CSphIndex_VLN::DoMerge ( const CSphIndex_VLN * pDstIndex, const CSphIndex_VLN * pSrcIndex, ISphFilter * pFilter, CSphString & sError, CSphIndexProgress & tProgress,
	bool bSrcSettings, bool bSupressDstDocids )
{
	auto & tMonitor = tProgress.GetMergeCb();
	assert ( pDstIndex && pSrcIndex );

	/// 'merge with self' - only apply filters/kill-lists, no real merge
	bool bCompress = pSrcIndex==pDstIndex;

	const CSphSchema & tDstSchema = pDstIndex->m_tSchema;
	const CSphSchema & tSrcSchema = pSrcIndex->m_tSchema;
	if ( !bCompress && !tDstSchema.CompareTo ( tSrcSchema, sError ) )
		return false;

	if ( !bCompress && pDstIndex->m_tSettings.m_eHitless!=pSrcIndex->m_tSettings.m_eHitless )
	{
		sError = "hitless settings must be the same on merged indices";
		return false;
	}

	if ( !bCompress && pDstIndex->m_pDict->GetSettings().m_bWordDict!=pSrcIndex->m_pDict->GetSettings().m_bWordDict )
	{
		sError.SetSprintf ( "dictionary types must be the same (dst dict=%s, src dict=%s )",
			pDstIndex->m_pDict->GetSettings().m_bWordDict ? "keywords" : "crc",
			pSrcIndex->m_pDict->GetSettings().m_bWordDict ? "keywords" : "crc" );
		return false;
	}

	// Create global list of documents to be merged from both sources
	// here we can also quickly consider to reject whole merge if N of final docs is >4G
	CSphFixedVector<RowID_t> dSrcRows{0}, dDstRows{0};
	auto tTotalDocs = CreateRowMapsAndCountTotalDocs ( pSrcIndex, pDstIndex, dSrcRows, dDstRows, pFilter, bSupressDstDocids, tMonitor );
	int64_t iTotalDocs = (int64_t)tTotalDocs.first + (int64_t)tTotalDocs.second;
	if ( iTotalDocs >= INVALID_ROWID )
		return false; // too many docs in merged segment (>4G even with filtered/killed), abort.

	BuildHeader_t tBuildHeader ( pDstIndex->m_tStats );

	// merging attributes
	{
		AttrMerger_c tAttrMerger ( tMonitor, sError, iTotalDocs );
		if ( !tAttrMerger.Prepare ( pSrcIndex, pDstIndex ) )
			return false;

		if ( !tAttrMerger.CopyAttributes ( *pDstIndex, dDstRows, tTotalDocs.first ) )
			return false;

		if ( !bCompress && !tAttrMerger.CopyAttributes ( *pSrcIndex, dSrcRows, tTotalDocs.second ) )
			return false;

		if ( !tAttrMerger.FinishMergeAttributes ( pDstIndex, tBuildHeader ) )
			return false;
	}

	const CSphIndex_VLN* pSettings = ( bSrcSettings ? pSrcIndex : pDstIndex );
	CSphAutofile tTmpDict ( pDstIndex->GetIndexFileName("spi.tmp"), SPH_O_NEW, sError, true );
	CSphAutofile tDict ( pDstIndex->GetIndexFileName ( SPH_EXT_SPI, true ), SPH_O_NEW, sError );

	if ( !sError.IsEmpty() || tTmpDict.GetFD()<0 || tDict.GetFD()<0 || tMonitor.NeedStop() )
		return false;

	DictRefPtr_c pDict { pSettings->m_pDict->Clone() };

	int iHitBufferSize = 8 * 1024 * 1024;
	CSphVector<SphWordID_t> dDummy;
	CSphHitBuilder tHitBuilder ( pSettings->m_tSettings, dDummy, true, iHitBufferSize, pDict, &sError );

	// FIXME? is this magic dict block constant any good?..
	pDict->DictBegin ( tTmpDict, tDict, iHitBufferSize );

	// merge dictionaries, doclists and hitlists
	if ( pDict->GetSettings().m_bWordDict )
	{
		WITH_QWORD ( pDstIndex, false, QwordDst,
			WITH_QWORD ( pSrcIndex, false, QwordSrc,
				if ( !CSphIndex_VLN::MergeWords < QwordDst, QwordSrc > ( pDstIndex, pSrcIndex, dDstRows, dSrcRows, &tHitBuilder, sError, tProgress ) )
					return false;
		));
	} else
	{
		WITH_QWORD ( pDstIndex, true, QwordDst,
			WITH_QWORD ( pSrcIndex, true, QwordSrc,
				if ( !CSphIndex_VLN::MergeWords < QwordDst, QwordSrc > ( pDstIndex, pSrcIndex, dDstRows, dSrcRows, &tHitBuilder, sError, tProgress ) )
					return false;
		));
	}

	if ( tMonitor.NeedStop () )
		return false;

	// finalize
	CSphAggregateHit tFlush;
	tFlush.m_tRowID = INVALID_ROWID;
	tFlush.m_uWordID = 0;
	tFlush.m_sKeyword = (const BYTE*)""; // tricky: assertion in cidxHit calls strcmp on this in case of empty index!
	tFlush.m_iWordPos = EMPTY_HIT;
	tFlush.m_dFieldMask.UnsetAll();
	tHitBuilder.cidxHit ( &tFlush );

	int iMinInfixLen = pSettings->m_tSettings.m_iMinInfixLen;
	if ( !tHitBuilder.cidxDone ( iHitBufferSize, iMinInfixLen, pSettings->m_pTokenizer->GetMaxCodepointLength(), &tBuildHeader ) )
		return false;

	CSphString sHeaderName = pDstIndex->GetIndexFileName ( SPH_EXT_SPH, true );

	WriteHeader_t tWriteHeader;
	tWriteHeader.m_pSettings = &pSettings->m_tSettings;
	tWriteHeader.m_pSchema = &pSettings->m_tSchema;
	tWriteHeader.m_pTokenizer = pSettings->m_pTokenizer;
	tWriteHeader.m_pDict = pSettings->m_pDict;
	tWriteHeader.m_pFieldFilter = pSettings->m_pFieldFilter;
	tWriteHeader.m_pFieldLens = pSettings->m_dFieldLens.Begin();

	IndexBuildDone ( tBuildHeader, tWriteHeader, sHeaderName, sError );

	return true;
}


bool sphMerge ( const CSphIndex * pDst, const CSphIndex * pSrc, VecTraits_T<CSphFilterSettings> dFilters, CSphIndexProgress & tProgress, CSphString& sError )
{
	auto pDstIndex = (const CSphIndex_VLN*) pDst;
	auto pSrcIndex = (const CSphIndex_VLN*) pSrc;

//	CSphScopedPtr<ISphFilter> pFilter { pDstIndex->CreateMergeFilters ( dFilters ) };
	std::unique_ptr<ISphFilter> pFilter { pDstIndex->CreateMergeFilters ( dFilters ) };
	return CSphIndex_VLN::DoMerge ( pDstIndex, pSrcIndex, pFilter.get(), sError, tProgress, dFilters.IsEmpty(), false );
}

template < typename QWORD >
bool CSphIndex_VLN::DeleteField ( const CSphIndex_VLN * pIndex, CSphHitBuilder * pHitBuilder, CSphString & sError, CSphSourceStats & tStat, int iKillField )
{
	assert ( iKillField>=0 );

	CSphAutofile tDummy;
	pHitBuilder->CreateIndexFiles ( pIndex->GetIndexFileName("tmp.spd").cstr(), pIndex->GetIndexFileName("tmp.spp").cstr(), pIndex->GetIndexFileName("tmp.spe").cstr(), false, 0, tDummy, nullptr );

	CSphDictReader<QWORD::is_worddict::value> tWordsReader ( pIndex->GetSettings().m_iSkiplistBlockSize );
	if ( !tWordsReader.Setup ( pIndex->GetIndexFileName(SPH_EXT_SPI), pIndex->m_tWordlist.GetWordsEnd(), pIndex->m_tSettings.m_eHitless, sError ) )
		return false;

	/// prepare for indexing
	pHitBuilder->HitblockBegin();
	pHitBuilder->HitReset();

	/// setup qword
	QWORD tQword ( false, false, pIndex->GetIndexId() );
	DataReaderFactoryPtr_c tDocs {
		NewProxyReader ( pIndex->GetIndexFileName ( SPH_EXT_SPD ), sError,
			DataReaderFactory_c::DOCS, pIndex->m_tMutableSettings.m_tFileAccess.m_iReadBufferDocList, FileAccess_e::FILE )
	};
	if ( !tDocs )
		return false;

	DataReaderFactoryPtr_c tHits {
		NewProxyReader ( pIndex->GetIndexFileName ( SPH_EXT_SPP ), sError,
			DataReaderFactory_c::HITS, pIndex->m_tMutableSettings.m_tFileAccess.m_iReadBufferHitList, FileAccess_e::FILE  )
	};
	if ( !tHits )
		return false;

	if ( !sError.IsEmpty () || sphInterrupted () )
		return false;

	QwordIteration::ConfigureQword ( tQword, tHits, tDocs, pIndex->m_tSchema.GetDynamicSize() );

	/// process
	while ( tWordsReader.Read () )
	{
		if ( sphInterrupted () )
			return false;

		bool bHitless = !tWordsReader.m_bHasHitlist;
		QwordIteration::PrepareQword ( tQword, tWordsReader );

		CSphAggregateHit tHit;
		tHit.m_uWordID = tWordsReader.m_uWordID; // !COMMIT m_sKeyword anyone?
		tHit.m_sKeyword = tWordsReader.GetWord();
		tHit.m_dFieldMask.UnsetAll();

		// transfer hits
		while ( QwordIteration::NextDocument ( tQword, pIndex ) )
		{
			if ( sphInterrupted () )
				return false;

			if ( bHitless )
			{
				tHit.m_tRowID = tQword.m_tDoc.m_tRowID;
				tHit.m_dFieldMask = tQword.m_dQwordFields; // fixme! what field mask on hitless? m.b. write 0 here?
				tHit.m_dFieldMask.DeleteBit (iKillField);
				if ( tHit.m_dFieldMask.TestAll ( false ) )
					continue;
				tHit.SetAggrCount ( tQword.m_uMatchHits );
				pHitBuilder->cidxHit ( &tHit );
			} else
			{
				assert ( tQword.m_bHasHitlist );
				tHit.m_tRowID = tQword.m_tDoc.m_tRowID;
				for ( Hitpos_t uHit = tQword.GetNextHit(); uHit!=EMPTY_HIT; uHit = tQword.GetNextHit() )
				{
					int iField = HITMAN::GetField ( uHit );
					if ( iKillField==iField )
						continue;

					if ( iField>iKillField )
						HITMAN::DecrementField ( uHit );

					tHit.m_iWordPos = uHit;
					pHitBuilder->cidxHit ( &tHit );
				}
			}
		}
	}

	return true;
}


bool CSphIndex_VLN::DeleteFieldFromDict ( int iFieldId, BuildHeader_t & tBuildHeader, CSphString & sError )
{
	CSphAutofile tTmpDict ( GetIndexFileName ( "spi.tmp" ), SPH_O_NEW, sError, true );
	CSphAutofile tNewDict ( GetIndexFileName ( SPH_EXT_SPI, true ), SPH_O_NEW, sError );

	if ( !sError.IsEmpty () || tTmpDict.GetFD ()<0 || tNewDict.GetFD ()<0 || sphInterrupted () )
		return false;

	DictRefPtr_c pDict { m_pDict->Clone () };

	int iHitBufferSize = 8 * 1024 * 1024;
	CSphVector<SphWordID_t> dDummy;
	CSphHitBuilder tHitBuilder ( m_tSettings, dDummy, true, iHitBufferSize, pDict, &sError );

	// FIXME? is this magic dict block constant any good?..
	pDict->DictBegin ( tTmpDict, tNewDict, iHitBufferSize );

	// merge dictionaries, doclists and hitlists
	if ( pDict->GetSettings().m_bWordDict )
	{
		WITH_QWORD ( this, false, Qword,
			if ( !CSphIndex_VLN::DeleteField <Qword> ( this, &tHitBuilder, sError, tBuildHeader, iFieldId ) )
				return false;
		);
	} else
	{
		WITH_QWORD ( this, true, Qword,
			if ( !CSphIndex_VLN::DeleteField <Qword> ( this, &tHitBuilder, sError, tBuildHeader, iFieldId ) )
				return false;
		);
	}

	if ( sphInterrupted() )
		return false;

	// finalize
	CSphAggregateHit tFlush;
	tFlush.m_tRowID = INVALID_ROWID;
	tFlush.m_uWordID = 0;
	tFlush.m_sKeyword = (const BYTE*)""; // tricky: assertion in cidxHit calls strcmp on this in case of empty index!
	tFlush.m_iWordPos = EMPTY_HIT;
	tFlush.m_dFieldMask.UnsetAll();
	tHitBuilder.cidxHit ( &tFlush );

	int iMinInfixLen = m_tSettings.m_iMinInfixLen;
	if ( !tHitBuilder.cidxDone ( iHitBufferSize, iMinInfixLen, m_pTokenizer->GetMaxCodepointLength(), &tBuildHeader ) )
		return false;

	/// as index is w-locked, we can also detach doclist/hitlist/dictionary and juggle them.
	tTmpDict.Close();
	tNewDict.Close();

	m_tWordlist.Reset();
	if ( !JuggleFile ( SPH_EXT_SPI, sError ) )	return false;
	m_tWordlist.m_iDictCheckpointsOffset= tBuildHeader.m_iDictCheckpointsOffset;
	m_tWordlist.m_iDictCheckpoints		= tBuildHeader.m_iDictCheckpoints;
	m_tWordlist.m_iInfixCodepointBytes	= tBuildHeader.m_iInfixCodepointBytes;
	m_tWordlist.m_iInfixBlocksOffset	= tBuildHeader.m_iInfixBlocksOffset;
	m_tWordlist.m_iInfixBlocksWordsSize = tBuildHeader.m_iInfixBlocksWordsSize;
	m_tWordlist.m_dCheckpoints.Reset ( m_tWordlist.m_iDictCheckpoints );
	if ( !PreallocWordlist() )					return false;

	m_tSkiplists.Reset ();
	if ( !JuggleFile ( SPH_EXT_SPE, sError ) )	return false;
	if ( !PreallocSkiplist() )					return false;

	m_pDoclistFile = nullptr;
	m_pHitlistFile = nullptr;
	if ( !JuggleFile ( SPH_EXT_SPD, sError ) )	return false;
	if ( !JuggleFile ( SPH_EXT_SPP, sError ) )	return false;
	if ( !SpawnReaders() )						return false;

	return true;
}


bool CSphIndex_VLN::AddRemoveFromDocstore ( const CSphSchema & tOldSchema, const CSphSchema & tNewSchema, CSphString & sError )
{
	int iOldNumStored = 0;
	for ( int i = 0; i < tOldSchema.GetFieldsCount(); i++ )
		if ( tOldSchema.IsFieldStored(i) )
			iOldNumStored++;

	for ( int i = 0; i < tOldSchema.GetAttrsCount(); i++ )
		if ( tOldSchema.IsAttrStored(i) )
			iOldNumStored++;

	int iNewNumStored = 0;
	for ( int i = 0; i < tNewSchema.GetFieldsCount(); i++ )
		if ( tNewSchema.IsFieldStored(i) )
			iNewNumStored++;

	for ( int i = 0; i < tNewSchema.GetAttrsCount(); i++ )
		if ( tNewSchema.IsAttrStored(i) )
			iOldNumStored++;

	if ( iOldNumStored==iNewNumStored )
		return true;

	CSphScopedPtr<DocstoreBuilder_i> pDocstoreBuilder(nullptr);
	if ( iNewNumStored )
	{
		pDocstoreBuilder = CreateDocstoreBuilder ( GetIndexFileName ( SPH_EXT_SPDS, true ), m_pDocstore->GetDocstoreSettings(), sError );
		if ( !pDocstoreBuilder.Ptr() )
			return false;

		Alter_AddRemoveFromDocstore ( *pDocstoreBuilder, m_pDocstore.Ptr(), (DWORD)m_iDocinfo, tNewSchema );
	}

	if ( !JuggleFile ( SPH_EXT_SPDS, sError, !!iOldNumStored, !!iNewNumStored ) )
		return false;

	m_pDocstore.Reset();
	PreallocDocstore();

	return true;
}


bool CSphIndex_VLN::AddRemoveField ( bool bAddField, const CSphString & sFieldName, DWORD uFieldFlags, CSphString & sError )
{
	CSphSchema tOldSchema = m_tSchema;
	CSphSchema tNewSchema = m_tSchema;
	if ( !Alter_AddRemoveFieldFromSchema ( bAddField, tNewSchema, sFieldName, uFieldFlags, sError ) )
		return false;

	auto iRemoveIdx = m_tSchema.GetFieldIndex ( sFieldName.cstr () );
	m_tSchema = tNewSchema;

	BuildHeader_t tBuildHeader ( m_tStats );
	tBuildHeader.m_iDocinfo = m_iDocinfo;
	tBuildHeader.m_iDocinfoIndex = m_iDocinfoIndex;
	tBuildHeader.m_iMinMaxIndex = m_iMinMaxIndex;

	*(DictHeader_t *) &tBuildHeader = *(DictHeader_t *) &m_tWordlist;

	if ( !bAddField && !DeleteFieldFromDict ( iRemoveIdx, tBuildHeader, sError ) )
		return false;

	if ( !AddRemoveFromDocstore ( tOldSchema, tNewSchema, sError ) )
		return false;

	CSphString sHeaderName = GetIndexFileName ( SPH_EXT_SPH, true );
	WriteHeader_t tWriteHeader;
	tWriteHeader.m_pSettings = &m_tSettings;
	tWriteHeader.m_pSchema = &tNewSchema;
	tWriteHeader.m_pTokenizer = m_pTokenizer;
	tWriteHeader.m_pDict = m_pDict;
	tWriteHeader.m_pFieldFilter = m_pFieldFilter;
	tWriteHeader.m_pFieldLens = m_dFieldLens.Begin ();

	// save the header
	if ( !IndexBuildDone ( tBuildHeader, tWriteHeader, sHeaderName, sError ) ) 	return false;
	if ( !JuggleFile ( SPH_EXT_SPH, sError ) )		return false;

	return true;
}


/////////////////////////////////////////////////////////////////////////////
// THE SEARCHER
/////////////////////////////////////////////////////////////////////////////

SphWordID_t CSphDictTraits::GetWordID ( BYTE * )
{
	assert ( 0 && "not implemented" );
	return 0;
}


SphWordID_t CSphDictStar::GetWordID ( BYTE * pWord )
{
	char sBuf [ 16+3*SPH_MAX_WORD_LEN ];
	assert ( strlen ( (const char*)pWord ) < 16+3*SPH_MAX_WORD_LEN );

	if ( m_pDict->GetSettings().m_bStopwordsUnstemmed && m_pDict->IsStopWord ( pWord ) )
		return 0;

	m_pDict->ApplyStemmers ( pWord );

	auto iLen = (int) strlen ( (const char*)pWord );
	assert ( iLen < 16+3*SPH_MAX_WORD_LEN - 1 );
	// stemmer might squeeze out the word
	if ( iLen && !pWord[0] )
		return 0;

	memcpy ( sBuf, pWord, iLen+1 );

	if ( iLen )
	{
		if ( sBuf[iLen-1]=='*' )
		{
			iLen--;
			sBuf[iLen] = '\0';
		} else
		{
			sBuf[iLen] = MAGIC_WORD_TAIL;
			iLen++;
			sBuf[iLen] = '\0';
		}
	}

	return m_pDict->GetWordID ( (BYTE*)sBuf, iLen, !m_pDict->GetSettings().m_bStopwordsUnstemmed );
}


SphWordID_t	CSphDictStar::GetWordIDNonStemmed ( BYTE * pWord )
{
	return m_pDict->GetWordIDNonStemmed ( pWord );
}


//////////////////////////////////////////////////////////////////////////

SphWordID_t	CSphDictStarV8::GetWordID ( BYTE * pWord )
{
	char sBuf [ 16+3*SPH_MAX_WORD_LEN ];

	auto iLen = (int) strlen ( (const char*)pWord );
	iLen = Min ( iLen, 16+3*SPH_MAX_WORD_LEN - 1 );

	if ( !iLen )
		return 0;

	bool bHeadStar = ( pWord[0]=='*' );
	bool bTailStar = ( pWord[iLen-1]=='*' ) && ( iLen>1 );
	bool bMagic = ( pWord[0]<' ' );

	if ( !bHeadStar && !bTailStar && !bMagic )
	{
		if ( m_pDict->GetSettings().m_bStopwordsUnstemmed && IsStopWord ( pWord ) )
			return 0;

		m_pDict->ApplyStemmers ( pWord );

		// stemmer might squeeze out the word
		if ( !pWord[0] )
			return 0;

		if ( !m_pDict->GetSettings().m_bStopwordsUnstemmed && IsStopWord ( pWord ) )
			return 0;
	}

	iLen = (int) strlen ( (const char*)pWord );
	assert ( iLen < 16+3*SPH_MAX_WORD_LEN - 2 );

	if ( !iLen || ( bHeadStar && iLen==1 ) )
		return 0;

	if ( bMagic ) // pass throu MAGIC_* words
	{
		memcpy ( sBuf, pWord, iLen );
		sBuf[iLen] = '\0';

	} else if ( m_bInfixes )
	{
		////////////////////////////////////
		// infix or mixed infix+prefix mode
		////////////////////////////////////

		// handle head star
		if ( bHeadStar )
		{
			memcpy ( sBuf, pWord+1, iLen-- ); // chops star, copies trailing zero, updates iLen
		} else
		{
			sBuf[0] = MAGIC_WORD_HEAD;
			memcpy ( sBuf+1, pWord, ++iLen ); // copies everything incl trailing zero, updates iLen
		}

		// handle tail star
		if ( bTailStar )
		{
			sBuf[--iLen] = '\0'; // got star, just chop it away
		} else
		{
			sBuf[iLen] = MAGIC_WORD_TAIL; // no star, add tail marker
			sBuf[++iLen] = '\0';
		}

	} else
	{
		////////////////////
		// prefix-only mode
		////////////////////

		// always ignore head star in prefix mode
		if ( bHeadStar )
		{
			pWord++;
			iLen--;
		}

		// handle tail star
		if ( !bTailStar )
		{
			// exact word search request, always (ie. both in infix/prefix mode) mangles to "\1word\1" in v.8+
			sBuf[0] = MAGIC_WORD_HEAD;
			memcpy ( sBuf+1, pWord, iLen );
			sBuf[iLen+1] = MAGIC_WORD_TAIL;
			sBuf[iLen+2] = '\0';
			iLen += 2;

		} else
		{
			// prefix search request, mangles to word itself (just chop away the star)
			memcpy ( sBuf, pWord, iLen );
			sBuf[--iLen] = '\0';
		}
	}

	// calc id for mangled word
	return m_pDict->GetWordID ( (BYTE*)sBuf, iLen, !bHeadStar && !bTailStar );
}

//////////////////////////////////////////////////////////////////////////

SphWordID_t CSphDictExact::GetWordID ( BYTE * pWord )
{
	auto iLen = (int) strlen ( (const char*)pWord );
	iLen = Min ( iLen, 16+3*SPH_MAX_WORD_LEN - 1 );

	if ( !iLen )
		return 0;

	if ( pWord[0]=='=' )
		pWord[0] = MAGIC_WORD_HEAD_NONSTEMMED;

	if ( pWord[0]<' ' )
		return m_pDict->GetWordIDNonStemmed ( pWord );

	return m_pDict->GetWordID ( pWord );
}

/////////////////////////////////////////////////////////////////////////////

void CSphMatchComparatorState::FixupLocators ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema, bool bRemapKeyparts )
{
	for ( int i = 0; i < CSphMatchComparatorState::MAX_ATTRS; ++i )
	{
		sphFixupLocator ( m_tLocator[i], pOldSchema, pNewSchema );

		// update string keypart into str_ptr
		if ( bRemapKeyparts && m_eKeypart[i]==SPH_KEYPART_STRING )
			m_eKeypart[i] = SPH_KEYPART_STRINGPTR;

		// update columnar attrs
		if ( pOldSchema )
		{
			int iOldAttrId = m_dAttrs[i];
			if ( iOldAttrId!=-1 )
				m_dAttrs[i] = pNewSchema->GetAttrIndex ( pOldSchema->GetAttr(iOldAttrId).m_sName.cstr() );
		}
	}
}

//////////////////////////////////////////////////////////////////////////

inline bool sphGroupMatch ( SphAttr_t iGroup, const SphAttr_t * pGroups, int iGroups )
{
	if ( !pGroups ) return true;
	const SphAttr_t * pA = pGroups;
	const SphAttr_t * pB = pGroups+iGroups-1;
	if ( iGroup==*pA || iGroup==*pB ) return true;
	if ( iGroup<(*pA) || iGroup>(*pB) ) return false;

	while ( pB-pA>1 )
	{
		const SphAttr_t * pM = pA + ((pB-pA)/2);
		if ( iGroup==(*pM) )
			return true;
		if ( iGroup<(*pM) )
			pB = pM;
		else
			pA = pM;
	}
	return false;
}


bool CSphIndex_VLN::EarlyReject ( CSphQueryContext * pCtx, CSphMatch & tMatch ) const
{
	tMatch.m_pStatic = GetDocinfoByRowID ( tMatch.m_tRowID );
	pCtx->CalcFilter ( tMatch );
	if ( !pCtx->m_pFilter )
		return false;

	if ( !pCtx->m_pFilter->Eval ( tMatch ) )
	{
		pCtx->FreeDataFilter ( tMatch );
		return true;
	}

	return false;
}


CSphVector<SphAttr_t> CSphIndex_VLN::BuildDocList () const
{
	TlsMsg::Err(); // clean err
	CSphVector<SphAttr_t> dResult;
	if ( !m_iDocinfo )
		return dResult;

	// new[] might fail on 32bit here
	int64_t iSizeMax = (size_t)m_iDocinfo;
	if ( iSizeMax!=m_iDocinfo )
	{
		TlsMsg::Err ( "doc-list build size_t overflow (docs count=%l, size max=%l)", m_iDocinfo, iSizeMax );
		return dResult;
	}

	int iStride = m_tSchema.GetRowSize();
	dResult.Resize ( m_iDocinfo );

	const CSphRowitem * pRow = m_tAttr.GetWritePtr();
	for ( SphAttr_t & tDst : dResult )
	{
		tDst = sphGetDocID ( pRow );
		pRow += iStride;
	}

	dResult.Uniq();
	return dResult;
}


RowID_t CSphIndex_VLN::GetRowidByDocid ( DocID_t tDocID ) const
{
	return m_tLookupReader.Find ( tDocID );
}


int	CSphIndex_VLN::Kill ( DocID_t tDocID )
{
	// FIXME! docid might not be unique
	if ( m_tDeadRowMap.Set ( GetRowidByDocid ( tDocID ) ) )
	{
		m_uAttrsStatus |= IndexUpdateHelper_c::ATTRS_ROWMAP_UPDATED;
		if ( m_pKillHook )
			m_pKillHook->Kill ( tDocID );
		return 1;
	}

	return 0;
}

bool CSphIndex_VLN::IsAlive ( DocID_t tDocID ) const
{
	RowID_t tRow = GetRowidByDocid ( tDocID );
	if ( tRow==INVALID_ROWID )
		return false;

	return ( !m_tDeadRowMap.IsSet ( tRow ) );
}


inline const CSphRowitem * CSphIndex_VLN::FindDocinfo ( DocID_t tDocID ) const
{
	RowID_t tRowID = GetRowidByDocid ( tDocID );
	return tRowID==INVALID_ROWID ? nullptr : GetDocinfoByRowID ( tRowID );
}


inline const CSphRowitem * CSphIndex_VLN::GetDocinfoByRowID ( RowID_t tRowID ) const
{
	//  GetCachedRowSize() is used to avoid several virtual calls
	return m_tAttr.GetWritePtr() + (int64_t)tRowID*m_tSchema.GetCachedRowSize();
}


inline RowID_t CSphIndex_VLN::GetRowIDByDocinfo ( const CSphRowitem * pDocinfo ) const
{
	return RowID_t ( ( pDocinfo - m_tAttr.GetWritePtr() ) / m_tSchema.GetCachedRowSize() );
}


inline void CalcContextItem ( CSphMatch & tMatch, const CSphQueryContext::CalcItem_t & tCalc )
{
	switch ( tCalc.m_eType )
	{
	case SPH_ATTR_BOOL:
	case SPH_ATTR_INTEGER:
	case SPH_ATTR_TIMESTAMP:
		tMatch.SetAttr ( tCalc.m_tLoc, tCalc.m_pExpr->IntEval(tMatch) );
		break;

	case SPH_ATTR_BIGINT:
	case SPH_ATTR_JSON_FIELD:
		tMatch.SetAttr ( tCalc.m_tLoc, tCalc.m_pExpr->Int64Eval(tMatch) );
		break;

	case SPH_ATTR_STRINGPTR:
		tMatch.SetAttr ( tCalc.m_tLoc, (SphAttr_t)tCalc.m_pExpr->StringEvalPacked ( tMatch ) );
		break;

	case SPH_ATTR_FACTORS:
	case SPH_ATTR_FACTORS_JSON:
		tMatch.SetAttr ( tCalc.m_tLoc, (SphAttr_t)tCalc.m_pExpr->FactorEvalPacked ( tMatch ) ); // FIXME! a potential leak of *previous* value?
		break;

	case SPH_ATTR_INT64SET_PTR:
	case SPH_ATTR_UINT32SET_PTR:
		tMatch.SetAttr ( tCalc.m_tLoc, (SphAttr_t)tCalc.m_pExpr->Int64Eval ( tMatch ) );
		break;

	default:
		tMatch.SetAttrFloat ( tCalc.m_tLoc, tCalc.m_pExpr->Eval(tMatch) );
		break;
	}
}


inline void CalcContextItems ( CSphMatch & tMatch, const VecTraits_T<CSphQueryContext::CalcItem_t> & dItems )
{
	for ( auto & i : dItems )
		CalcContextItem ( tMatch, i );
}


void CSphQueryContext::CalcFilter ( CSphMatch & tMatch ) const
{
	CalcContextItems ( tMatch, m_dCalcFilter );
}


void CSphQueryContext::CalcSort ( CSphMatch & tMatch ) const
{
	CalcContextItems ( tMatch, m_dCalcSort );
}


void CSphQueryContext::CalcFinal ( CSphMatch & tMatch ) const
{
	CalcContextItems ( tMatch, m_dCalcFinal );
}


void CSphQueryContext::CalcItem ( CSphMatch & tMatch, const CalcItem_t & tCalc ) const
{
	CalcContextItem ( tMatch, tCalc );
}


static inline void FreeDataPtrAttrs ( CSphMatch & tMatch, const CSphVector<CSphQueryContext::CalcItem_t> & dItems, const IntVec_t & dItemIndexes )
{
	if ( !tMatch.m_pDynamic )
		return;

	for ( auto i : dItemIndexes )
	{
		const auto & tItem = dItems[i];

		BYTE * pData = (BYTE *)tMatch.GetAttr ( tItem.m_tLoc );
		// delete[] pData;
		if ( pData )
		{
			sphDeallocatePacked ( pData );
			tMatch.SetAttr ( tItem.m_tLoc, 0 );
		}
	}
}

void CSphQueryContext::FreeDataFilter ( CSphMatch & tMatch ) const
{
	FreeDataPtrAttrs ( tMatch, m_dCalcFilter, m_dCalcFilterPtrAttrs );
}


void CSphQueryContext::FreeDataSort ( CSphMatch & tMatch ) const
{
	FreeDataPtrAttrs ( tMatch, m_dCalcSort, m_dCalcSortPtrAttrs );
}

void CSphQueryContext::ExprCommand ( ESphExprCommand eCmd, void * pArg )
{
	ARRAY_FOREACH ( i, m_dCalcFilter )
		m_dCalcFilter[i].m_pExpr->Command ( eCmd, pArg );
	ARRAY_FOREACH ( i, m_dCalcSort )
		m_dCalcSort[i].m_pExpr->Command ( eCmd, pArg );
	ARRAY_FOREACH ( i, m_dCalcFinal )
		m_dCalcFinal[i].m_pExpr->Command ( eCmd, pArg );
}


void CSphQueryContext::SetBlobPool ( const BYTE * pBlobPool )
{
	ExprCommand ( SPH_EXPR_SET_BLOB_POOL, (void*)pBlobPool );
	if ( m_pFilter )
		m_pFilter->SetBlobStorage ( pBlobPool );
	if ( m_pWeightFilter )
		m_pWeightFilter->SetBlobStorage ( pBlobPool );
}


void CSphQueryContext::SetColumnar ( const columnar::Columnar_i * pColumnar )
{
	ExprCommand ( SPH_EXPR_SET_COLUMNAR, (void*)pColumnar );
}


void CSphQueryContext::SetDocstore ( const Docstore_i * pDocstore, int64_t iDocstoreSessionId )
{
	DocstoreSession_c::InfoRowID_t tSessionInfo;
	tSessionInfo.m_pDocstore = pDocstore;
	tSessionInfo.m_iSessionId = iDocstoreSessionId;

	ExprCommand ( SPH_EXPR_SET_DOCSTORE_ROWID, &tSessionInfo );
}


/// FIXME, perhaps
/// this rather crappy helper class really serves exactly 1 (one) simple purpose
///
/// it passes a sorting queue internals (namely, weight and float sortkey, if any,
/// of the current-worst queue element) to the MIN_TOP_WORST() and MIN_TOP_SORTVAL()
/// expression classes that expose those to the cruel outside world
///
/// all the COM-like EXTRA_xxx message back and forth is needed because expressions
/// are currently parsed and created earlier than the sorting queue
///
/// that also is the reason why we mischievously return 0 instead of clearly failing
/// with an error when the sortval is not a dynamic float; by the time we are parsing
/// expressions, we do not *yet* know that; but by the time we create a sorting queue,
/// we do not *want* to leak select expression checks into it
///
/// alternatively, we probably want to refactor this and introduce Bind(), to parse
/// expressions once, then bind them to actual searching contexts (aka index or segment,
/// and ranker, and sorter, and whatever else might be referenced by the expressions)
struct ContextExtra final : public ISphExtra
{
	ISphRanker * m_pRanker;
	ISphMatchSorter * m_pSorter;

	ContextExtra ( ISphRanker* pRanker, ISphMatchSorter* pSorter)
		: m_pRanker ( pRanker )
		, m_pSorter ( pSorter )
		{}

	bool ExtraDataImpl ( ExtraData_e eData, void ** ppArg ) final
	{
		if ( eData!=EXTRA_GET_QUEUE_WORST && eData!=EXTRA_GET_QUEUE_SORTVAL )
			return m_pRanker->ExtraData ( eData, ppArg );

		if ( !m_pSorter )
			return false;

		const CSphMatch * pWorst = m_pSorter->GetWorst();
		if ( !pWorst )
			return false;

		if ( eData==EXTRA_GET_QUEUE_WORST )
		{
			*ppArg = (void*)pWorst;
			return true;
		};

		assert ( eData==EXTRA_GET_QUEUE_SORTVAL );
		const CSphMatchComparatorState & tCmp = m_pSorter->GetState();
		if ( tCmp.m_eKeypart[0]==SPH_KEYPART_FLOAT && tCmp.m_tLocator[0].m_bDynamic
			&& tCmp.m_tLocator[0].m_iBitCount==32 && ( tCmp.m_tLocator[0].m_iBitOffset%32==0 )
			&& tCmp.m_dAttrs[1]==-1 )
		{
			*(int*)ppArg = tCmp.m_tLocator[0].m_iBitOffset/32;
			return true;
		}

		// min_top_sortval() only works with order by float_expr for now
		return false;
	}
};


void CSphQueryContext::SetupExtraData ( ISphRanker * pRanker, ISphMatchSorter * pSorter )
{
	ContextExtra tExtra ( pRanker, pSorter );
	ExprCommand ( SPH_EXPR_SET_EXTRA_DATA, &tExtra );
}


template<bool USE_KLIST, bool RANDOMIZE, bool USE_FACTORS>
void CSphIndex_VLN::MatchExtended ( CSphQueryContext& tCtx, const CSphQuery & tQuery, const VecTraits_T<ISphMatchSorter *> & dSorters, ISphRanker * pRanker, int iTag, int iIndexWeight ) const
{
	QueryProfile_c * pProfile = tCtx.m_pProfile;
	CSphScopedProfile tProf (pProfile, SPH_QSTATE_UNKNOWN);

	if_const ( USE_FACTORS )
	{
		pRanker->ExtraData ( EXTRA_SET_MATCHTAG, (void**)&iTag );
	}

	int iCutoff = tQuery.m_iCutoff;
	if ( iCutoff<=0 )
		iCutoff = -1;

	// do searching
	CSphMatch * pMatch = pRanker->GetMatchesBuffer();
	while (true)
	{
		// ranker does profile switches internally in GetMatches()
		int iMatches = pRanker->GetMatches();
		if ( iMatches<=0 )
			break;

		SwitchProfile ( pProfile, SPH_QSTATE_SORT );

		for ( int i=0; i<iMatches; i++ )
		{
			CSphMatch & tMatch = pMatch[i];
			if_const ( USE_KLIST )
			{
				if ( m_tDeadRowMap.IsSet ( tMatch.m_tRowID ) )
					continue;
			}

			tMatch.m_iWeight *= iIndexWeight;
			tCtx.CalcSort ( tMatch );

			if ( tCtx.m_pWeightFilter && !tCtx.m_pWeightFilter->Eval ( tMatch ) )
			{
				tCtx.FreeDataSort ( tMatch );
				continue;
			}

			tMatch.m_iTag = iTag;

			bool bRand = false;
			bool bNewMatch = false;
			for ( ISphMatchSorter * pSorter: dSorters )
			{
				// all non-random sorters are in the beginning,
				// so we can avoid the simple 'first-element' assertion
				if_const ( RANDOMIZE )
				{
					if ( !bRand && pSorter->IsRandom() )
					{
						bRand = true;
						tMatch.m_iWeight = ( sphRand() & 0xffff ) * iIndexWeight;

						if ( tCtx.m_pWeightFilter && !tCtx.m_pWeightFilter->Eval ( tMatch ) )
							break;
					}
				}

				bNewMatch |= pSorter->Push ( tMatch );

				if_const ( USE_FACTORS )
				{
					RowTagged_t tJustPushed = pSorter->GetJustPushed();
					VecTraits_T<RowTagged_t> dJustPopped = pSorter->GetJustPopped();
					pRanker->ExtraData ( EXTRA_SET_MATCHPUSHED, (void**)&tJustPushed );
					pRanker->ExtraData ( EXTRA_SET_MATCHPOPPED, (void**)&dJustPopped );
				}
			}

			tCtx.FreeDataFilter ( tMatch );
			tCtx.FreeDataSort ( tMatch );

			if ( bNewMatch )
				if ( --iCutoff==0 )
					break;
		}

		if ( iCutoff==0 )
			break;
	}
}

//////////////////////////////////////////////////////////////////////////


struct SphFinalMatchCalc_t final : MatchProcessor_i, ISphNoncopyable
{
	const CSphQueryContext &	m_tCtx;
	int							m_iTag;

	SphFinalMatchCalc_t ( int iTag, const CSphQueryContext & tCtx )
		: m_tCtx ( tCtx )
		, m_iTag ( iTag )
	{}

	bool ProcessInRowIdOrder() const final
	{
		// columnar expressions don't like random access, they are optimized for sequental access
		// that's why if we have a columnar expression, we need to call Process it ascending RowId order
		return m_tCtx.m_dCalcFinal.any_of ( []( const CSphQueryContext::CalcItem_t & i ){ return i.m_pExpr && i.m_pExpr->IsColumnar(); } );
	}

	void Process ( CSphMatch * pMatch ) final
	{
		// fixme! tag is signed int,
		// for distr. tags from remotes set with | 0x80000000,
		// i e in terms of signed int they're <0!
		// Is it intention, or bug?
		// If intention, lt us use uniformely either <0, either &0x80000000
		// conditions to avoid messing. If bug, shit already happened!
		if ( pMatch->m_iTag>=0 )
			return;

		m_tCtx.CalcFinal ( *pMatch );
		pMatch->m_iTag = m_iTag;
	}

	void Process ( VecTraits_T<CSphMatch *> & dMatches ) final
	{
		CSphVector<CSphQueryContext::CalcItem_t *> dColumnWise, dRowWise;

		// process columnar items in column-wise order (and the rest in rowwise order)
		for ( auto & i : m_tCtx.m_dCalcFinal )
			if ( i.m_pExpr->IsColumnar() )
				dColumnWise.Add(&i);
			else
				dRowWise.Add(&i);

		for ( const auto & pItem : dColumnWise )
			for ( auto & pMatch : dMatches )
			{
				assert(pMatch);
				if ( pMatch->m_iTag>=0 )
					continue;

				m_tCtx.CalcItem ( *pMatch, *pItem );
			}

		for ( auto & pMatch : dMatches )
			for ( const auto & pItem : dRowWise )
			{
				assert(pMatch);
				if ( pMatch->m_iTag>=0 )
					continue;

				m_tCtx.CalcItem ( *pMatch, *pItem );
			}

		for ( auto & pMatch : dMatches )
		{
			assert(pMatch);
			if ( pMatch->m_iTag>=0 )
				continue;

			pMatch->m_iTag = m_iTag;
		}
	}
};


/// scoped thread scheduling helper
/// either makes the current thread low priority while the helper lives, or does nothing
class ScopedThreadPriority_c
{
private:
	bool m_bRestore;

public:
	ScopedThreadPriority_c ( bool bLowPriority )
	{
		m_bRestore = false;
		if ( !bLowPriority )
			return;

#if _WIN32
		if ( !SetThreadPriority ( GetCurrentThread(), THREAD_PRIORITY_IDLE ) )
			return;
#else
		struct sched_param p;
		p.sched_priority = 0;
#ifdef SCHED_IDLE
		int iPolicy = SCHED_IDLE;
#else
		int iPolicy = SCHED_OTHER;
#endif
		if ( pthread_setschedparam ( pthread_self (), iPolicy, &p ) )
			return;
#endif

		m_bRestore = true;
	}

	~ScopedThreadPriority_c ()
	{
		if ( !m_bRestore )
			return;

#if _WIN32
		if ( !SetThreadPriority ( GetCurrentThread(), THREAD_PRIORITY_NORMAL ) )
			return;
#else
		struct sched_param p;
		p.sched_priority = 0;
		if ( pthread_setschedparam ( pthread_self (), SCHED_OTHER, &p ) )
			return;
#endif
	}
};

//////////////////////////////////////////////////////////////////////////

template <bool HAVE_DEAD>
class RowIterator_T : public ISphNoncopyable
{
public:
	RowIterator_T ( const RowIdBoundaries_t & tBoundaries, const DeadRowMap_Disk_c & tDeadRowMap )
		: m_tRowID ( tBoundaries.m_tMinRowID )
		, m_tBoundaries ( tBoundaries )
		, m_tDeadRowMap	( tDeadRowMap )
	{}

	inline bool GetNextRowIdBlock ( RowIdBlock_t & dRowIdBlock );
	DWORD GetNumProcessed() const { return m_tRowID-m_tBoundaries.m_tMinRowID; }

private:
	static const int MAX_COLLECTED = 128;

	RowID_t						m_tRowID {INVALID_ROWID};
	RowIdBoundaries_t			m_tBoundaries;
	CSphFixedVector<RowID_t>	m_dCollected {MAX_COLLECTED+1};		// store 128 values + end marker (same as .spa attr block size)
	const DeadRowMap_Disk_c &	m_tDeadRowMap;
};

template <>
bool RowIterator_T<true>::GetNextRowIdBlock ( RowIdBlock_t & dRowIdBlock )
{
	RowID_t * pRowIdStart = m_dCollected.Begin();
	RowID_t * pRowIdMax = pRowIdStart + m_dCollected.GetLength()-1;
	RowID_t * pRowID = pRowIdStart;

	while ( pRowID<pRowIdMax && m_tRowID<=m_tBoundaries.m_tMaxRowID )
	{
		if ( !m_tDeadRowMap.IsSet(m_tRowID) )
			*pRowID++ = m_tRowID;

		m_tRowID++;
	}

	return ReturnIteratorResult ( pRowID, pRowIdStart, dRowIdBlock );
}

template <>
bool RowIterator_T<false>::GetNextRowIdBlock ( RowIdBlock_t & dRowIdBlock )
{
	RowID_t * pRowIdStart = m_dCollected.Begin();
	int64_t iDelta = Min ( RowID_t(m_dCollected.GetLength()-1), int64_t(m_tBoundaries.m_tMaxRowID)-m_tRowID+1 );
	assert ( iDelta>=0 );
	RowID_t * pRowIdMax = pRowIdStart + iDelta;
	RowID_t * pRowID = pRowIdStart;

	// fixme! use sse?
	while ( pRowID<pRowIdMax )
		*pRowID++ = m_tRowID++;

	return ReturnIteratorResult ( pRowID, pRowIdStart, dRowIdBlock );
}

// adds killlist filtering to a rowid iterator
class RowIteratorAlive_c : public ISphNoncopyable
{
public:
	RowIteratorAlive_c ( RowidIterator_i * pIterator, const DeadRowMap_Disk_c & tDeadRowMap )
		: m_pIterator	( pIterator )
		, m_tDeadRowMap	( tDeadRowMap )
	{
		assert(pIterator);
	}

	FORCE_INLINE bool GetNextRowIdBlock ( RowIdBlock_t & dRowIdBlock )
	{
		RowIdBlock_t dIteratorRowIDs;
		if ( !m_pIterator->GetNextRowIdBlock(dIteratorRowIDs) )
			return false;

		m_dCollected.Resize ( dIteratorRowIDs.GetLength() );

		RowID_t * pRowIdStart = m_dCollected.Begin();
		RowID_t * pRowID = pRowIdStart;

		for ( auto i : dIteratorRowIDs )
		{
			if ( !m_tDeadRowMap.IsSet(i) )
				*pRowID++ = i;
		}

		dRowIdBlock = RowIdBlock_t ( pRowIdStart, pRowID - pRowIdStart );
		return true;	// always return true, even if all values were filtered out. next call will fetch more values
	}

	DWORD GetNumProcessed() const
	{
		return (DWORD)m_pIterator->GetNumProcessed();
	}

private:
	RowidIterator_i *				m_pIterator;
	CSphVector<RowID_t>				m_dCollected {0};
	const DeadRowMap_Disk_c &		m_tDeadRowMap;
};

//////////////////////////////////////////////////////////////////////////

template <bool HAS_FILTER_CALC, bool HAS_SORT_CALC, bool HAS_FILTER, bool HAS_RANDOMIZE, bool HAS_MAX_TIMER, bool HAS_CUTOFF, typename ITERATOR, typename TO_STATIC>
void Fullscan ( ITERATOR & tIterator, TO_STATIC && fnToStatic, const CSphQueryContext & tCtx, CSphQueryResultMeta & tMeta, const VecTraits_T<ISphMatchSorter *> & dSorters, CSphMatch & tMatch, int iCutoff, int iIndexWeight, int64_t tmMaxTimer, bool & bStop )
{
	RowIdBlock_t dRowIDs;
	while ( !bStop && tIterator.GetNextRowIdBlock(dRowIDs) )
		for ( auto & i : dRowIDs )
		{
			tMatch.m_tRowID = i;
			tMatch.m_pStatic = fnToStatic(i);

			// early filter only (no late filters in full-scan because of no @weight)
			if_const ( HAS_FILTER_CALC )
				tCtx.CalcFilter(tMatch);

			if_const ( HAS_FILTER )
			{
				if ( !tCtx.m_pFilter->Eval(tMatch) )
				{
					if_const ( HAS_FILTER_CALC )
						tCtx.FreeDataFilter ( tMatch );

					continue;
				}
			}

			if_const ( HAS_RANDOMIZE )
				tMatch.m_iWeight = ( sphRand() & 0xffff ) * iIndexWeight;

			if_const ( HAS_SORT_CALC )
				tCtx.CalcSort(tMatch);

			bool bNewMatch = false;
			dSorters.Apply ( [&tMatch, &bNewMatch] ( ISphMatchSorter * p ) { bNewMatch |= p->Push ( tMatch ); } );

			// stringptr expressions should be duplicated (or taken over) at this point
			if_const ( HAS_FILTER_CALC )
				tCtx.FreeDataFilter ( tMatch );

			if_const ( HAS_SORT_CALC )
				tCtx.FreeDataSort ( tMatch );

			if_const ( HAS_CUTOFF )
			{
				if ( bNewMatch && --iCutoff==0 )
				{
					bStop = true;
					break;
				}
			}

			// handle timer
			if_const ( HAS_MAX_TIMER )
			{
				if ( sph::TimeExceeded ( tmMaxTimer ) )
				{
					tMeta.m_sWarning = "query time exceeded max_query_time";
					bStop = true;
					break;
				}
			}
		}

	tMeta.m_tStats.m_iFetchedDocs = (DWORD)tIterator.GetNumProcessed();
}


template <typename ITERATOR, typename TO_STATIC>
void RunFullscan ( ITERATOR & tIterator, TO_STATIC && fnToStatic, const CSphQueryContext & tCtx, CSphQueryResultMeta & tMeta, const VecTraits_T<ISphMatchSorter *>& dSorters, CSphMatch & tMatch, int iCutoff, bool bRandomize, int iIndexWeight, int64_t tmMaxTimer, bool & bStop )
{
	bool bHasFilterCalc = !tCtx.m_dCalcFilter.IsEmpty();
	bool bHasSortCalc = !tCtx.m_dCalcSort.IsEmpty();
	bool bHasFilter = !!tCtx.m_pFilter;
	bool bHasTimer = tmMaxTimer>0;
	bool bHasCutoff = iCutoff!=-1;
	int iIndex = bHasFilterCalc*32 + bHasSortCalc*16 + bHasFilter*8 + bRandomize*4 + bHasTimer*2 + bHasCutoff;

	switch ( iIndex )
	{
#define DECL_FNSCAN( _, n, params ) case n: Fullscan<!!(n&32), !!(n&16), !!(n&8), !!(n&4), !!(n&2), !!(n&1), ITERATOR, TO_STATIC> params; break;
	BOOST_PP_REPEAT ( 64, DECL_FNSCAN, ( tIterator, std::forward<TO_STATIC> ( fnToStatic ), tCtx, tMeta, dSorters, tMatch, iCutoff, iIndexWeight, tmMaxTimer, bStop ) )
#undef DECL_FNSCAN
		default:
			assert ( 0 && "Internal error" );
			break;
	}
}


void CSphIndex_VLN::RunFullscanOnAttrs ( const RowIdBoundaries_t & tBoundaries, const CSphQueryContext & tCtx, CSphQueryResultMeta & tMeta, const VecTraits_T<ISphMatchSorter *> & dSorters, CSphMatch & tMatch, int iCutoff, bool bRandomize, int iIndexWeight, int64_t tmMaxTimer, bool & bStop ) const
{
	const CSphRowitem * pStart = m_tAttr.GetWritePtr();
	int iStride = m_tSchema.GetRowSize();
	auto fnToStatic = [pStart, iStride]( RowID_t tRowID ){ return pStart+(int64_t)tRowID*iStride; };

	if ( m_tDeadRowMap.HasDead() )
	{
		RowIterator_T<true> tIt ( tBoundaries, m_tDeadRowMap );
		RunFullscan ( tIt, fnToStatic, tCtx, tMeta, dSorters, tMatch, iCutoff, bRandomize, iIndexWeight, tmMaxTimer, bStop );
	}
	else
	{
		RowIterator_T<false> tIt ( tBoundaries, m_tDeadRowMap );
		RunFullscan ( tIt, fnToStatic, tCtx, tMeta, dSorters, tMatch, iCutoff, bRandomize, iIndexWeight, tmMaxTimer, bStop );
	}
}


void CSphIndex_VLN::RunFullscanOnIterator ( RowidIterator_i * pIterator, const CSphQueryContext & tCtx, CSphQueryResultMeta & tMeta, const VecTraits_T<ISphMatchSorter *> & dSorters, CSphMatch & tMatch, int iCutoff, bool bRandomize, int iIndexWeight, int64_t tmMaxTimer ) const
{
	bool bStop = false;
	const CSphRowitem * pStart = m_tAttr.GetWritePtr();
	int iStride = m_tSchema.GetRowSize();
	auto fnToStatic = [pStart, iStride]( RowID_t tRowID ){ return pStart+(int64_t)tRowID*iStride; };

	if ( m_tDeadRowMap.HasDead() )
	{
		RowIteratorAlive_c tIt ( pIterator, m_tDeadRowMap );
		RunFullscan ( tIt, fnToStatic, tCtx, tMeta, dSorters, tMatch, iCutoff, bRandomize, iIndexWeight, tmMaxTimer, bStop );
	}
	else
		RunFullscan ( *pIterator, fnToStatic, tCtx, tMeta, dSorters, tMatch, iCutoff, bRandomize, iIndexWeight, tmMaxTimer, bStop );
}


template <bool ROWID_LIMITS>
void CSphIndex_VLN::ScanByBlocks ( const CSphQueryContext & tCtx, CSphQueryResultMeta & tMeta, const VecTraits_T<ISphMatchSorter *> & dSorters, CSphMatch & tMatch, int iCutoff, bool bRandomize, int iIndexWeight, int64_t tmMaxTimer, const RowIdBoundaries_t * pBoundaries ) const
{
	int iStartIndexEntry = 0;
	int iEndIndexEntry = (int)m_iDocinfoIndex;
	if ( ROWID_LIMITS )
	{
		assert(pBoundaries);

		iStartIndexEntry = pBoundaries->m_tMinRowID / DOCINFO_INDEX_FREQ;
		iEndIndexEntry =   pBoundaries->m_tMaxRowID / DOCINFO_INDEX_FREQ + 1;
	}

	int iStride = m_tSchema.GetRowSize();
	for ( int64_t iIndexEntry=iStartIndexEntry; iIndexEntry!=iEndIndexEntry; iIndexEntry++ )
	{
		// block-level filtering
		const DWORD * pMin = &m_pDocinfoIndex[ iIndexEntry*iStride*2 ];
		const DWORD * pMax = pMin + iStride;
		if ( tCtx.m_pFilter && !tCtx.m_pFilter->EvalBlock ( pMin, pMax ) )
			continue;

		RowIdBoundaries_t tBlockBoundaries;
		tBlockBoundaries.m_tMinRowID = RowID_t ( iIndexEntry*DOCINFO_INDEX_FREQ );
		tBlockBoundaries.m_tMaxRowID = (RowID_t)Min ( ( iIndexEntry+1 )*DOCINFO_INDEX_FREQ, m_iDocinfo ) - 1;

		bool bStop = false;

		if ( ROWID_LIMITS )
		{
			// clamp block start/end to match limits. need this only for first/last blocks
			if ( iIndexEntry==iStartIndexEntry || iIndexEntry==iEndIndexEntry-1 )
			{
				assert(pBoundaries);
				tBlockBoundaries.m_tMinRowID = Max ( tBlockBoundaries.m_tMinRowID, pBoundaries->m_tMinRowID );
				tBlockBoundaries.m_tMaxRowID = Min ( tBlockBoundaries.m_tMaxRowID, pBoundaries->m_tMaxRowID );
			}
		}

		RunFullscanOnAttrs ( tBlockBoundaries, tCtx, tMeta, dSorters, tMatch, iCutoff, bRandomize, iIndexWeight, tmMaxTimer, bStop );

		if ( bStop || !iCutoff )
			break;
	}
}


RowidIterator_i * CSphIndex_VLN::CreateColumnarAnalyzerOrPrefilter ( const CSphVector<CSphFilterSettings> & dFilters, CSphVector<CSphFilterSettings> & dModifiedFilters, bool & bFiltersChanged, const CSphVector<FilterTreeItem_t> & dFilterTree, const ISphFilter * pFilter, ESphCollation eCollation, const ISphSchema & tSchema, CSphString & sWarning ) const
{
	bFiltersChanged = false;

	if ( !m_pColumnar.Ptr() || dFilterTree.GetLength() || !pFilter )
		return nullptr;

	std::vector<columnar::Filter_t> dColumnarFilters;
	std::vector<int> dFilterMap;
	dFilterMap.resize ( dFilters.GetLength() );
	ARRAY_FOREACH ( i, dFilters )
	{
		dFilterMap[i] = -1;
		const CSphColumnInfo * pCol = tSchema.GetAttr ( dFilters[i].m_sAttrName.cstr() );
		bool bColumnarFilter = pCol && ( pCol->IsColumnar() || pCol->IsColumnarExpr() || pCol->IsStoredExpr() );
		bool bRowIdFilter = dFilters[i].m_sAttrName=="@rowid";
		if ( ( bColumnarFilter || bRowIdFilter )  && AddColumnarFilter ( dColumnarFilters, dFilters[i], eCollation, tSchema, sWarning ) )
			dFilterMap[i] = (int)dColumnarFilters.size()-1;
	}

	if ( dColumnarFilters.empty() || ( dColumnarFilters.size()==1 && dColumnarFilters[0].m_sName=="@rowid" ) )
		return nullptr;

	std::vector<int> dDeletedFilters;
	std::vector<columnar::BlockIterator_i *> dIterators;
	dIterators = m_pColumnar->CreateAnalyzerOrPrefilter ( dColumnarFilters, dDeletedFilters, *pFilter );
	if ( dIterators.empty() )
		return nullptr;

	const CSphFilterSettings * pRowIdFilter = nullptr;
	ARRAY_FOREACH ( i, dFilters )
		if ( dFilters[i].m_sAttrName=="@rowid" )
		{
			dDeletedFilters.push_back(i);
			pRowIdFilter = &dFilters[i];
			break;
		}

	dModifiedFilters.Resize(0);
	bFiltersChanged = true;
	for ( size_t i=0; i < dFilterMap.size(); i++ )
		if ( dFilterMap[i]==-1 || !std::binary_search ( dDeletedFilters.begin(), dDeletedFilters.end(), dFilterMap[i] ) )
			dModifiedFilters.Add ( dFilters[i] );

	RowIdBoundaries_t tBoundaries;
	if ( pRowIdFilter )
		tBoundaries = GetFilterRowIdBoundaries ( *pRowIdFilter, RowID_t(m_iDocinfo) );

	if ( dIterators.size()==1 )
		return CreateIteratorWrapper ( dIterators[0], pRowIdFilter ? &tBoundaries : nullptr );

	return CreateIteratorIntersect ( dIterators, pRowIdFilter ? &tBoundaries : nullptr );
}


static void UpdateSpawnedIterators ( bool bChanged, bool & bRecreateFilters, CSphVector<CSphFilterSettings> & dOriginalFilters, CSphVector<CSphFilterSettings> & dModifiedFilters, const CSphVector<CSphFilterSettings> * & pOriginalFilters, CSphVector<RowidIterator_i*> & dIterators, RowidIterator_i * pIterator )
{
	if ( pIterator )
		dIterators.Add(pIterator);

	if ( bChanged )
	{
		bRecreateFilters = true;
		dOriginalFilters = dModifiedFilters;
		pOriginalFilters = &dOriginalFilters;
	}
}


RowidIterator_i * CSphIndex_VLN::SpawnIterators ( const CSphQuery & tQuery, CSphVector<CSphFilterSettings> & dModifiedFilters, CSphQueryContext & tCtx, CreateFilterContext_t & tFlx, const ISphSchema & tMaxSorterSchema, CSphQueryResultMeta & tMeta ) const
{
	CSphVector<CSphFilterSettings> dOriginalFilters;
	const CSphVector<CSphFilterSettings> * pOriginalFilters = &tQuery.m_dFilters;

	CSphVector<RowidIterator_i*> dIterators;
	bool bRecreateFilters = false;

	// try to spawn an iterator from a secondary index
	{
		bool bChanged = false;
		RowidIterator_i * pIterator = m_pHistograms ? CreateFilteredIterator ( *pOriginalFilters, dModifiedFilters, bChanged, tQuery.m_dFilterTree, tQuery.m_dIndexHints, *m_pHistograms, m_tDocidLookup.GetWritePtr(), RowID_t(m_iDocinfo) ) : nullptr;
		UpdateSpawnedIterators ( bChanged, bRecreateFilters, dOriginalFilters, dModifiedFilters, pOriginalFilters, dIterators, pIterator );
	}

	// try to spawn analyzers or prefilters from columnar storage
	{
		// if we already created an iterator at prev stage, we need to recreate filters here, so we won't be doing unnecessary minmax eval
		if ( bRecreateFilters && pOriginalFilters->GetLength() )
		{
			SafeDelete ( tCtx.m_pFilter );
			tFlx.m_pFilters = &dModifiedFilters;
			tCtx.CreateFilters ( tFlx, tMeta.m_sError, tMeta.m_sWarning );
			bRecreateFilters = false;
		}

		bool bChanged = false;
		RowidIterator_i * pIterator = CreateColumnarAnalyzerOrPrefilter ( *pOriginalFilters, dModifiedFilters, bChanged, tQuery.m_dFilterTree, tCtx.m_pFilter, tQuery.m_eCollation, tMaxSorterSchema, tMeta.m_sWarning );
		UpdateSpawnedIterators ( bChanged, bRecreateFilters, dOriginalFilters, dModifiedFilters, pOriginalFilters, dIterators, pIterator );
	}

	if ( bRecreateFilters )
	{
		SafeDelete ( tCtx.m_pFilter );
		tFlx.m_pFilters = &dModifiedFilters;
		tCtx.CreateFilters ( tFlx, tMeta.m_sError, tMeta.m_sWarning );
	}

	switch ( dIterators.GetLength() )
	{
	case 0:		return nullptr;
	case 1:		return dIterators[0];
	default:	return CreateIteratorIntersect ( dIterators, nullptr );	// both columnar iterator wrappers and secondary index iterators support rowid filtering, so no need for it here
	}
}


static RowIdBoundaries_t RemoveRowIdFilter ( CSphQueryContext & tCtx, CreateFilterContext_t & tFlx, CSphVector<CSphFilterSettings> & dModifiedFilters, CSphQueryResultMeta & tMeta, int64_t iDocinfo, bool & bHaveRowidFilter )
{
	const CSphFilterSettings * pRowIdFilter = nullptr;
	for ( const auto & tFilter : tCtx.m_tQuery.m_dFilters )
		if ( tFilter.m_sAttrName=="@rowid" )
		{
			pRowIdFilter = &tFilter;
			break;
		}

	bHaveRowidFilter = !!pRowIdFilter;

	if ( !pRowIdFilter )
		return { 0, RowID_t(iDocinfo)-1 };

	dModifiedFilters.Resize(0);
	for ( const auto & tFilter : tCtx.m_tQuery.m_dFilters )
		if ( &tFilter!=pRowIdFilter )
			dModifiedFilters.Add(tFilter);

	SafeDelete ( tCtx.m_pFilter );
	tFlx.m_pFilters = &dModifiedFilters;
	tCtx.CreateFilters ( tFlx, tMeta.m_sError, tMeta.m_sWarning );

	return GetFilterRowIdBoundaries ( *pRowIdFilter, RowID_t(iDocinfo) );
}


bool CSphIndex_VLN::MultiScan ( CSphQueryResult & tResult, const CSphQuery & tQuery, const VecTraits_T<ISphMatchSorter *> & dSorters, const CSphMultiQueryArgs & tArgs, int64_t tmMaxTimer ) const
{
	assert ( tArgs.m_iTag>=0 );
	auto& tMeta = *tResult.m_pMeta;

	// check if index is ready
	if ( !m_bPassedAlloc )
	{
		tMeta.m_sError = "index not preread";
		return false;
	}

	// check if index supports scans
	if ( !m_tSchema.GetAttrsCount() )
	{
		tMeta.m_sError = "need attributes to run fullscan";
		return false;
	}

	// we count documents only (before filters)
	if ( tQuery.m_iMaxPredictedMsec )
		tMeta.m_bHasPrediction = true;

	if ( tArgs.m_uPackedFactorFlags & SPH_FACTOR_ENABLE )
		tMeta.m_sWarning.SetSprintf ( "packedfactors() will not work with a fullscan; you need to specify a query" );

	// check if index has data
	if ( m_bIsEmpty || m_iDocinfo<=0 )
		return true;

	// start counting
	int64_t tmQueryStart = sphMicroTimer();
	int64_t tmCpuQueryStart = sphTaskCpuTimer();

	ScopedThreadPriority_c tPrio ( tQuery.m_bLowPriority );

	// select the sorter with max schema
	int iMaxSchemaIndex = GetMaxSchemaIndexAndMatchCapacity ( dSorters ).first;
	const ISphSchema & tMaxSorterSchema = *(dSorters[iMaxSchemaIndex]->GetSchema());
	auto dSorterSchemas = SorterSchemas ( dSorters, iMaxSchemaIndex);

	// setup calculations and result schema
	CSphQueryContext tCtx ( tQuery );
	if ( !tCtx.SetupCalc ( tMeta, tMaxSorterSchema, m_tSchema, m_tBlobAttrs.GetWritePtr(), m_pColumnar.Ptr(), dSorterSchemas ) )
		return false;

	// set blob pool for string on_sort expression fix up
	tCtx.SetBlobPool ( m_tBlobAttrs.GetWritePtr() );
	tCtx.SetColumnar ( m_pColumnar.Ptr() );

	// setup filters
	CreateFilterContext_t tFlx;
	tFlx.m_pFilters = &tQuery.m_dFilters;
	tFlx.m_pFilterTree = &tQuery.m_dFilterTree;
	tFlx.m_pSchema = &tMaxSorterSchema;
	tFlx.m_pBlobPool = m_tBlobAttrs.GetWritePtr();
	tFlx.m_pColumnar = m_pColumnar.Ptr();
	tFlx.m_eCollation = tQuery.m_eCollation;
	tFlx.m_bScan = true;
	tFlx.m_pHistograms = m_pHistograms;
	tFlx.m_iTotalDocs = m_iDocinfo;

	if ( !tCtx.CreateFilters ( tFlx, tMeta.m_sError, tMeta.m_sWarning ) )
		return false;

	if ( CheckEarlyReject ( tQuery.m_dFilters, tCtx.m_pFilter, tQuery.m_eCollation, tMaxSorterSchema ) )
	{
		tMeta.m_iQueryTime += (int)( ( sphMicroTimer()-tmQueryStart )/1000 );
		tMeta.m_iCpuTime += sphTaskCpuTimer ()-tmCpuQueryStart;
		return true;
	}

	// setup sorters
	for ( auto & i : dSorters )
	{
		i->SetBlobPool ( m_tBlobAttrs.GetWritePtr() );
		i->SetColumnar ( m_pColumnar.Ptr() );
	}

	// prepare to work them rows
	bool bRandomize = dSorters[0]->IsRandom();

	CSphMatch tMatch;
	tMatch.Reset ( tMaxSorterSchema.GetDynamicSize() );
	tMatch.m_iWeight = tArgs.m_iIndexWeight;
	// fixme! tag also used over bitmask | 0x80000000,
	// which marks that match comes from remote.
	// using -1 might be also interpreted as 0xFFFFFFFF in such context!
	// Does it intended?
	tMatch.m_iTag = tCtx.m_dCalcFinal.GetLength() ? -1 : tArgs.m_iTag;

	SwitchProfile ( tMeta.m_pProfile, SPH_QSTATE_FULLSCAN );

	// run full scan with block and row filtering for everything else
	int iCutoff = ( tQuery.m_iCutoff<=0 ) ? -1 : tQuery.m_iCutoff;

	// we don't modify the original filters because iterators may use some data from them (to avoid copying)
	CSphVector<CSphFilterSettings> dModifiedFilters;
	CSphScopedPtr<RowidIterator_i> pIterator ( SpawnIterators ( tQuery, dModifiedFilters, tCtx, tFlx, tMaxSorterSchema, tMeta ) );
	if ( pIterator )
		RunFullscanOnIterator ( pIterator.Ptr(), tCtx, tMeta, dSorters, tMatch, iCutoff, bRandomize, tArgs.m_iIndexWeight, tmMaxTimer );
	else
	{
		bool bHaveRowidFilter = false;
		RowIdBoundaries_t tBoundaries = RemoveRowIdFilter ( tCtx, tFlx, dModifiedFilters, tMeta, m_iDocinfo, bHaveRowidFilter );

		bool bAllColumnar = dModifiedFilters.all_of ( [this]( const CSphFilterSettings & tFilter )
							{
								const CSphColumnInfo * pCol = m_tSchema.GetAttr ( tFilter.m_sAttrName.cstr() );
								return pCol ? ( pCol->IsColumnar() || pCol->IsColumnarExpr() ) : false;
							} );

		if ( bAllColumnar )
		{
			bool bStop = false;
			RunFullscanOnAttrs ( tBoundaries, tCtx, tMeta, dSorters, tMatch, iCutoff, bRandomize, tArgs.m_iIndexWeight, tmMaxTimer, bStop );
		}
		else
		{
			if ( bHaveRowidFilter )
				ScanByBlocks<true> ( tCtx, tMeta, dSorters, tMatch, iCutoff, bRandomize, tArgs.m_iIndexWeight, tmMaxTimer, &tBoundaries );
			else
				ScanByBlocks<false> ( tCtx, tMeta, dSorters, tMatch, iCutoff, bRandomize, tArgs.m_iIndexWeight, tmMaxTimer );
		}
	}

	SwitchProfile ( tMeta.m_pProfile, SPH_QSTATE_FINALIZE );

	// do final expression calculations
	if ( tCtx.m_dCalcFinal.GetLength() )
	{
		DocstoreSession_c tSession;
		int64_t iSessionUID = tSession.GetUID();

		// spawn buffered readers for the current session
		// put them to a global hash
		if ( m_pDocstore )
			m_pDocstore->CreateReader ( iSessionUID );

		DocstoreSession_c::InfoRowID_t tSessionInfo;
		tSessionInfo.m_pDocstore = m_pDocstore.Ptr();
		tSessionInfo.m_iSessionId = iSessionUID;

		for ( auto & i : tCtx.m_dCalcFinal )
		{
			assert ( i.m_pExpr );
			if ( m_pDocstore )
				i.m_pExpr->Command ( SPH_EXPR_SET_DOCSTORE_ROWID, &tSessionInfo );
		}

		SphFinalMatchCalc_t tFinal ( tArgs.m_iTag, tCtx );
		dSorters.Apply ( [&] ( ISphMatchSorter * p ) { p->Finalize ( tFinal, false, tArgs.m_bFinalizeSorters ); } );
	}

	if ( tArgs.m_bModifySorterSchemas )
	{
		SwitchProfile ( tMeta.m_pProfile, SPH_QSTATE_DYNAMIC );
		PooledAttrsToPtrAttrs ( dSorters, m_tBlobAttrs.GetReadPtr(), m_pColumnar.Ptr(), tArgs.m_bFinalizeSorters );
	}

	// done
	tResult.m_pBlobPool = m_tBlobAttrs.GetWritePtr();
	tResult.m_pDocstore = m_pDocstore.Ptr() ? this : nullptr;
	tResult.m_pColumnar = m_pColumnar.Ptr();

	tMeta.m_iQueryTime += (int)( ( sphMicroTimer()-tmQueryStart )/1000 );
	tMeta.m_iCpuTime += sphTaskCpuTimer ()-tmCpuQueryStart;

	return true;
}

//////////////////////////////////////////////////////////////////////////////

ISphQword * DiskIndexQwordSetup_c::QwordSpawn ( const XQKeyword_t & tWord ) const
{
	if ( !tWord.m_pPayload )
	{
		WITH_QWORD ( m_pIndex, false, Qword, return new Qword ( tWord.m_bExpanded, tWord.m_bExcluded, m_pIndex->GetIndexId() ) );
	} else
	{
		if ( m_pIndex->GetSettings().m_eHitFormat==SPH_HIT_FORMAT_INLINE )
			return new DiskPayloadQword_c<true> ( (const DiskSubstringPayload_t *)tWord.m_pPayload, tWord.m_bExcluded, m_pDoclist, m_pHitlist, m_pIndex->GetIndexId() );
		else
			return new DiskPayloadQword_c<false> ( (const DiskSubstringPayload_t *)tWord.m_pPayload, tWord.m_bExcluded, m_pDoclist, m_pHitlist, m_pIndex->GetIndexId() );
	}
	return NULL;
}


bool DiskIndexQwordSetup_c::QwordSetup ( ISphQword * pWord ) const
{
	auto * pMyWord = (DiskIndexQwordTraits_c*)pWord;

	// setup attrs
	pMyWord->m_tDoc.Reset ( m_iDynamicRowitems );
	pMyWord->m_tDoc.m_tRowID = INVALID_ROWID;

	return pMyWord->Setup ( this );
}

bool DiskIndexQwordSetup_c::SetupWithWrd ( const DiskIndexQwordTraits_c& tWord, CSphDictEntry& tRes ) const
{
	auto* pIndex = (CSphIndex_VLN*)const_cast<CSphIndex*> ( m_pIndex );
	const char * sWord = tWord.m_sDictWord.cstr();
	int iWordLen = sWord ? (int) strlen ( sWord ) : 0;
	if ( tWord.m_sWord.Ends("*") )
	{
		iWordLen = Max ( iWordLen-1, 0 );

		// might match either infix or prefix
		int iMinLen = Max ( pIndex->m_tSettings.GetMinPrefixLen ( true ), pIndex->m_tSettings.m_iMinInfixLen );
		if ( pIndex->m_tSettings.GetMinPrefixLen ( true ) )
			iMinLen = Min ( iMinLen, pIndex->m_tSettings.GetMinPrefixLen ( true ) );
		if ( pIndex->m_tSettings.m_iMinInfixLen )
			iMinLen = Min ( iMinLen, pIndex->m_tSettings.m_iMinInfixLen );

		// bail out term shorter than prefix or infix allowed
		if ( iWordLen<iMinLen )
			return false;
	}

	// leading special symbols trimming
	if ( tWord.m_sDictWord.Begins("*") )
	{
		++sWord;
		iWordLen = Max ( iWordLen-1, 0 );
		// bail out term shorter than infix allowed
		if ( iWordLen<pIndex->m_tSettings.m_iMinInfixLen )
			return false;
	}

	const CSphWordlistCheckpoint * pCheckpoint = pIndex->m_tWordlist.FindCheckpointWrd ( sWord, iWordLen, false );
	if ( !pCheckpoint )
		return false;

	// decode wordlist chunk
	const BYTE * pBuf = pIndex->m_tWordlist.AcquireDict ( pCheckpoint );
	assert ( pBuf );
	assert ( m_iSkiplistBlockSize>0 );

	KeywordsBlockReader_c tCtx ( pBuf, m_iSkiplistBlockSize );
	while ( tCtx.UnpackWord() )
	{
		// block is sorted
		// so once keywords are greater than the reference word, no more matches
		assert ( tCtx.GetWordLen()>0 );
		int iCmp = sphDictCmpStrictly ( sWord, iWordLen, tCtx.GetWord(), tCtx.GetWordLen() );
		if ( iCmp<0 )
			return false;
		if ( iCmp==0 )
			break;
	}
	if ( tCtx.GetWordLen()<=0 )
		return false;
	tRes = tCtx;
	return true;
}

bool DiskIndexQwordSetup_c::SetupWithCrc ( const DiskIndexQwordTraits_c& tWord, CSphDictEntry& tRes ) const
{
	auto * pIndex = (CSphIndex_VLN *)const_cast<CSphIndex *>(m_pIndex);
	const CSphWordlistCheckpoint * pCheckpoint = pIndex->m_tWordlist.FindCheckpointCrc ( tWord.m_uWordID );
	if ( !pCheckpoint )
		return false;

	const BYTE * pBuf = pIndex->m_tWordlist.AcquireDict ( pCheckpoint );
	assert ( pBuf );
	assert ( m_iSkiplistBlockSize>0 );
	return pIndex->m_tWordlist.GetWord ( pBuf, tWord.m_uWordID, tRes );
}

bool DiskIndexQwordSetup_c::Setup ( ISphQword * pWord ) const
{
	// there was a dynamic_cast here once but it's not necessary
	// maybe it worth to rewrite class hierarchy to avoid c-cast here?
	DiskIndexQwordTraits_c & tWord = *(DiskIndexQwordTraits_c*)pWord;

	// setup stats
	tWord.m_iDocs = 0;
	tWord.m_iHits = 0;

	auto * pIndex = (CSphIndex_VLN *)const_cast<CSphIndex *>(m_pIndex);

	// !COMMIT FIXME!
	// the below stuff really belongs in wordlist
	// which in turn really belongs in dictreader
	// which in turn might or might not be a part of dict

	// binary search through checkpoints for a one whose range matches word ID
	assert ( pIndex->m_bPassedAlloc );
	assert ( !pIndex->m_tWordlist.m_tBuf.IsEmpty() );

	// empty index?
	if ( !pIndex->m_tWordlist.m_dCheckpoints.GetLength() )
		return false;

	CSphDictEntry tRes;
	if ( pIndex->m_pDict->GetSettings().m_bWordDict )
	{
		if ( !SetupWithWrd ( tWord, tRes ) )
			return false;
	} else if ( !SetupWithCrc ( tWord, tRes ) )
		return false;

	const ESphHitless eMode = pIndex->m_tSettings.m_eHitless;
	tWord.m_iDocs = eMode==SPH_HITLESS_SOME ? ( tRes.m_iDocs & HITLESS_DOC_MASK ) : tRes.m_iDocs;
	tWord.m_iHits = tRes.m_iHits;
	tWord.m_bHasHitlist =
		( eMode==SPH_HITLESS_NONE ) ||
		( eMode==SPH_HITLESS_SOME && !( tRes.m_iDocs & HITLESS_DOC_FLAG ) );

	if ( m_bSetupReaders )
	{
		tWord.SetDocReader ( m_pDoclist );

		// read in skiplist
		// OPTIMIZE? maybe add an option to decompress on preload instead?
		if ( m_pSkips && tRes.m_iDocs>m_iSkiplistBlockSize )
		{
			int iSkips = tRes.m_iDocs/m_iSkiplistBlockSize;
			const int SMALL_SKIP_THRESH = 256;
			bool bNeedCache = iSkips > SMALL_SKIP_THRESH;

			bool & bFromCache = tWord.m_bSkipFromCache;

			SkipCache_c * pSkipCache = SkipCache_c::Get();
			bFromCache = bNeedCache && pSkipCache && pSkipCache->Find ( { m_pIndex->GetIndexId(), tWord.m_uWordID }, tWord.m_pSkipData );
			if ( !bFromCache )
			{
				tWord.m_pSkipData = new SkipData_t;
				tWord.m_pSkipData->Read ( m_pSkips, tRes, tWord.m_iDocs, m_iSkiplistBlockSize );
				bFromCache = bNeedCache && pSkipCache && pSkipCache->Add ( { m_pIndex->GetIndexId(), tWord.m_uWordID }, tWord.m_pSkipData );
			}
		}

		tWord.m_rdDoclist->SeekTo ( tRes.m_iDoclistOffset, tRes.m_iDoclistHint );
		tWord.SetHitReader ( m_pHitlist );
	}

	return true;
}

QwordScan_c::QwordScan_c ( int iRowsCount )
	: m_iRowsCount ( iRowsCount )
{
	m_bDone = ( m_iRowsCount==0 );

	m_iDocs = m_iRowsCount;
	m_sWord = "";
	m_sDictWord = "";
	m_bExcluded = true;
	m_dQwordFields.SetAll();
}

const CSphMatch & QwordScan_c::GetNextDoc()
{
	if ( m_bDone )
	{
		m_tDoc.m_tRowID = INVALID_ROWID;
		return m_tDoc;
	}

	if ( m_tDoc.m_tRowID==INVALID_ROWID )
		m_tDoc.m_tRowID = 0;
	else
		m_tDoc.m_tRowID++;

	if ( m_tDoc.m_tRowID>=m_iRowsCount )
	{
		m_bDone = true;
		m_tDoc.m_tRowID = INVALID_ROWID;
	}

	return m_tDoc;
}

ISphQword * DiskIndexQwordSetup_c::ScanSpawn() const
{
	return new QwordScan_c ( m_iRowsCount );
}

//////////////////////////////////////////////////////////////////////////////

bool CSphIndex_VLN::Lock ()
{
	CSphString sName = GetIndexFileName(SPH_EXT_SPL);
	sphLogDebug ( "Locking the index via file %s", sName.cstr() );
	return RawFileLock ( sName, m_iLockFD, m_sLastError );
}

void CSphIndex_VLN::Unlock()
{
	CSphString sName = GetIndexFileName(SPH_EXT_SPL);
	if ( m_iLockFD<0 )
		return;
	sphLogDebug ( "Unlocking the index (lock %s)", sName.cstr() );
	RawFileUnLock ( sName, m_iLockFD );
}


void CSphIndex_VLN::Dealloc ()
{
	if ( !m_bPassedAlloc )
		return;

	m_pDoclistFile = nullptr;
	m_pHitlistFile = nullptr;
	m_pColumnar = nullptr;

	m_tAttr.Reset ();
	m_tBlobAttrs.Reset();
	m_tSkiplists.Reset ();
	m_tWordlist.Reset ();
	m_tDeadRowMap.Dealloc();
	m_tDocidLookup.Reset();
	m_pDocstore.Reset();

	m_iDocinfo = 0;
	m_iMinMaxIndex = 0;

	m_bPassedRead = false;
	m_bPassedAlloc = false;
	m_uAttrsStatus = 0;

	QcacheDeleteIndex ( m_iIndexId );

	SkipCache_c * pSkipCache = SkipCache_c::Get();
	if ( pSkipCache )
		pSkipCache->DeleteAll(m_iIndexId);

	m_iIndexId = m_tIdGenerator.fetch_add ( 1, std::memory_order_relaxed );
}


void LoadIndexSettings ( CSphIndexSettings & tSettings, CSphReader & tReader, DWORD uVersion )
{
	tSettings.SetMinPrefixLen ( tReader.GetDword() );
	tSettings.m_iMinInfixLen = tReader.GetDword ();
	tSettings.m_iMaxSubstringLen = tReader.GetDword();

	tSettings.m_bHtmlStrip = !!tReader.GetByte ();
	tSettings.m_sHtmlIndexAttrs = tReader.GetString ();
	tSettings.m_sHtmlRemoveElements = tReader.GetString ();

	tSettings.m_bIndexExactWords = !!tReader.GetByte ();
	tSettings.m_eHitless = (ESphHitless)tReader.GetDword();

	tSettings.m_eHitFormat = (ESphHitFormat)tReader.GetDword();
	tSettings.m_bIndexSP = !!tReader.GetByte();

	tSettings.m_sZones = tReader.GetString();

	tSettings.m_iBoundaryStep = (int)tReader.GetDword();
	tSettings.m_iStopwordStep = (int)tReader.GetDword();

	tSettings.m_iOvershortStep = (int)tReader.GetDword();
	tSettings.m_iEmbeddedLimit = (int)tReader.GetDword();

	tSettings.m_eBigramIndex = (ESphBigram)tReader.GetByte();
	tSettings.m_sBigramWords = tReader.GetString();

	tSettings.m_bIndexFieldLens = ( tReader.GetByte()!=0 );

	tSettings.m_ePreprocessor = tReader.GetByte()==1 ? Preprocessor_e::ICU : Preprocessor_e::NONE;
	tReader.GetString(); // was: RLP context

	tSettings.m_sIndexTokenFilter = tReader.GetString();
	tSettings.m_tBlobUpdateSpace = tReader.GetOffset();

	if ( uVersion<56 )
		tSettings.m_iSkiplistBlockSize = 128;
	else
		tSettings.m_iSkiplistBlockSize = (int)tReader.GetDword();

	if ( uVersion>=60 )
		tSettings.m_sHitlessFiles = tReader.GetString();

	if ( uVersion>=63 )
		tSettings.m_eEngine = (AttrEngine_e)tReader.GetDword();
}


void LoadIndexSettingsJson ( bson::Bson_c tNode, CSphIndexSettings & tSettings )
{
	using namespace bson;
	tSettings.SetMinPrefixLen ( (int)Int ( tNode.ChildByName ( "min_prefix_len" ) ) );
	tSettings.m_iMinInfixLen = (int)Int ( tNode.ChildByName ( "min_infix_len" ) );
	tSettings.m_iMaxSubstringLen = (int)Int ( tNode.ChildByName ( "max_substring_len" ) );
	tSettings.m_bHtmlStrip = Bool ( tNode.ChildByName ( "strip_html" ) );
	tSettings.m_sHtmlIndexAttrs = String ( tNode.ChildByName ( "html_index_attrs" ) );
	tSettings.m_sHtmlRemoveElements = String ( tNode.ChildByName ( "html_remove_elements" ) );
	tSettings.m_bIndexExactWords = Bool ( tNode.ChildByName ( "index_exact_words" ) );
	tSettings.m_eHitless = (ESphHitless)Int ( tNode.ChildByName ( "hitless" ), SPH_HITLESS_NONE );
	tSettings.m_eHitFormat = (ESphHitFormat)Int ( tNode.ChildByName ( "hit_format" ), SPH_HIT_FORMAT_PLAIN );
	tSettings.m_bIndexSP = Bool ( tNode.ChildByName ( "index_sp" ) );
	tSettings.m_sZones = String ( tNode.ChildByName ( "zones" ) );
	tSettings.m_iBoundaryStep = (int)Int ( tNode.ChildByName ( "boundary_step" ) );
	tSettings.m_iStopwordStep = (int)Int ( tNode.ChildByName ( "stopword_step" ), 1 );
	tSettings.m_iOvershortStep = (int)Int ( tNode.ChildByName ( "overshort_step" ), 1 );
	tSettings.m_iEmbeddedLimit = (int)Int ( tNode.ChildByName ( "embedded_limit" ) );
	tSettings.m_eBigramIndex = (ESphBigram)Int ( tNode.ChildByName ( "bigram_index" ), SPH_BIGRAM_NONE );
	tSettings.m_sBigramWords = String ( tNode.ChildByName ( "bigram_words" ) );
	tSettings.m_bIndexFieldLens = Bool ( tNode.ChildByName ( "index_field_lens" ) );
	tSettings.m_ePreprocessor = (Preprocessor_e)Int ( tNode.ChildByName ( "icu" ), (DWORD)Preprocessor_e::NONE  );
	tSettings.m_sIndexTokenFilter = String ( tNode.ChildByName ( "index_token_filter" ) );
	tSettings.m_tBlobUpdateSpace = Int ( tNode.ChildByName ( "blob_update_space" ) );
	tSettings.m_iSkiplistBlockSize = (int)Int ( tNode.ChildByName ( "skiplist_block_size" ), 32 );
	tSettings.m_sHitlessFiles = String ( tNode.ChildByName ( "hitless_files" ) );
	tSettings.m_eEngine = (AttrEngine_e)Int ( tNode.ChildByName ( "engine" ), (DWORD)AttrEngine_e::DEFAULT );
}


bool CSphIndex_VLN::LoadHeaderLegacy ( const char * sHeaderName, bool bStripPath, CSphEmbeddedFiles & tEmbeddedFiles, FilenameBuilder_i * pFilenameBuilder, CSphString & sWarning )
{
	const int MAX_HEADER_SIZE = 32768;
	CSphFixedVector<BYTE> dCacheInfo ( MAX_HEADER_SIZE );

	CSphAutoreader rdInfo ( dCacheInfo.Begin(), MAX_HEADER_SIZE ); // to avoid mallocs
	if ( !rdInfo.Open ( sHeaderName, m_sLastError ) )
		return false;

	// magic header
	const char* sFmt = CheckFmtMagic ( rdInfo.GetDword () );
	if ( sFmt )
	{
		m_sLastError.SetSprintf ( sFmt, sHeaderName );
		return false;
	}

	// version
	m_uVersion = rdInfo.GetDword();
	if ( m_uVersion<=1 || m_uVersion>INDEX_FORMAT_VERSION )
	{
		m_sLastError.SetSprintf ( "%s is v.%u, binary is v.%u", sHeaderName, m_uVersion, INDEX_FORMAT_VERSION );
		return false;
	}

	// we don't support anything prior to v54
	DWORD uMinFormatVer = 54;
	if ( m_uVersion<uMinFormatVer )
	{
		m_sLastError.SetSprintf ( "indexes prior to v.%u are no longer supported (use index_converter tool); %s is v.%u", uMinFormatVer, sHeaderName, m_uVersion );
		return false;
	}

	// schema
	ReadSchema ( rdInfo, m_tSchema, m_uVersion );

	// check schema for dupes
	for ( int iAttr=0; iAttr<m_tSchema.GetAttrsCount(); iAttr++ )
	{
		const CSphColumnInfo & tCol = m_tSchema.GetAttr(iAttr);
		for ( int i=0; i<iAttr; i++ )
			if ( m_tSchema.GetAttr(i).m_sName==tCol.m_sName )
				sWarning.SetSprintf ( "duplicate attribute name: %s", tCol.m_sName.cstr() );
	}

	// dictionary header (wordlist checkpoints, infix blocks, etc)
	m_tWordlist.m_iDictCheckpointsOffset = rdInfo.GetOffset();
	m_tWordlist.m_iDictCheckpoints = rdInfo.GetDword();
	m_tWordlist.m_iInfixCodepointBytes = rdInfo.GetByte();
	m_tWordlist.m_iInfixBlocksOffset = rdInfo.GetDword();
	m_tWordlist.m_iInfixBlocksWordsSize = rdInfo.GetDword();

	m_tWordlist.m_dCheckpoints.Reset ( m_tWordlist.m_iDictCheckpoints );

	// index stats
	m_tStats.m_iTotalDocuments = rdInfo.GetDword ();
	m_tStats.m_iTotalBytes = rdInfo.GetOffset ();

	LoadIndexSettings ( m_tSettings, rdInfo, m_uVersion );

	CSphTokenizerSettings tTokSettings;

	// tokenizer stuff
	if ( !tTokSettings.Load ( pFilenameBuilder, rdInfo, tEmbeddedFiles, m_sLastError ) )
		return false;

	if ( bStripPath )
		StripPath ( tTokSettings.m_sSynonymsFile );

	StrVec_t dWarnings;
	TokenizerRefPtr_c pTokenizer { Tokenizer::Create ( tTokSettings, &tEmbeddedFiles, pFilenameBuilder, dWarnings, m_sLastError ) };
	if ( !pTokenizer )
		return false;

	// dictionary stuff
	CSphDictSettings tDictSettings;
	tDictSettings.Load ( rdInfo, tEmbeddedFiles, sWarning );

	if ( bStripPath )
	{
		ARRAY_FOREACH ( i, tDictSettings.m_dWordforms )
			StripPath ( tDictSettings.m_dWordforms[i] );
	}

	DictRefPtr_c pDict { tDictSettings.m_bWordDict
		? sphCreateDictionaryKeywords ( tDictSettings, &tEmbeddedFiles, pTokenizer, m_sIndexName.cstr(), bStripPath, m_tSettings.m_iSkiplistBlockSize, pFilenameBuilder, m_sLastError )
		: sphCreateDictionaryCRC ( tDictSettings, &tEmbeddedFiles, pTokenizer, m_sIndexName.cstr(), bStripPath, m_tSettings.m_iSkiplistBlockSize, pFilenameBuilder, m_sLastError )};

	if ( !pDict )
		return false;

	if ( tDictSettings.m_sMorphFingerprint!=pDict->GetMorphDataFingerprint() )
		sWarning.SetSprintf ( "different lemmatizer dictionaries (index='%s', current='%s')",
			tDictSettings.m_sMorphFingerprint.cstr(),
			pDict->GetMorphDataFingerprint().cstr() );

	SetDictionary ( pDict );

	pTokenizer = Tokenizer::CreateMultiformFilter ( pTokenizer, pDict->GetMultiWordforms () );
	SetTokenizer ( pTokenizer );
	SetupQueryTokenizer();

	// initialize AOT if needed
	m_tSettings.m_uAotFilterMask = sphParseMorphAot ( tDictSettings.m_sMorphology.cstr() );

	m_iDocinfo = rdInfo.GetOffset ();
	m_iDocinfoIndex = rdInfo.GetOffset ();
	m_iMinMaxIndex = rdInfo.GetOffset ();

	FieldFilterRefPtr_c pFieldFilter;
	CSphFieldFilterSettings tFieldFilterSettings;
	tFieldFilterSettings.Load(rdInfo);
	if ( tFieldFilterSettings.m_dRegexps.GetLength() )
		pFieldFilter = sphCreateRegexpFilter ( tFieldFilterSettings, m_sLastError );

	if ( !sphSpawnFilterICU ( pFieldFilter, m_tSettings, tTokSettings, sHeaderName, m_sLastError ) )
		return false;

	SetFieldFilter ( pFieldFilter );

	if ( m_tSettings.m_bIndexFieldLens )
		for ( int i=0; i < m_tSchema.GetFieldsCount(); i++ )
			m_dFieldLens[i] = rdInfo.GetOffset(); // FIXME? ideally 64bit even when off is 32bit..

	// post-load stuff.. for now, bigrams
	CSphIndexSettings & s = m_tSettings;
	if ( s.m_eBigramIndex!=SPH_BIGRAM_NONE && s.m_eBigramIndex!=SPH_BIGRAM_ALL )
	{
		BYTE * pTok;
		m_pTokenizer->SetBuffer ( (BYTE*)const_cast<char*> ( s.m_sBigramWords.cstr() ), s.m_sBigramWords.Length() );
		while ( ( pTok = m_pTokenizer->GetToken() )!=NULL )
			s.m_dBigramWords.Add() = (const char*)pTok;
		s.m_dBigramWords.Sort();
	}


	if ( rdInfo.GetErrorFlag() )
		m_sLastError.SetSprintf ( "%s: failed to parse header (unexpected eof)", sHeaderName );

	return !rdInfo.GetErrorFlag();
}

bool CSphIndex_VLN::LoadHeader ( const char* sHeaderName, bool bStripPath, CSphEmbeddedFiles & tEmbeddedFiles, FilenameBuilder_i * pFilenameBuilder, CSphString & sWarning )
{
	using namespace bson;

	CSphVector<BYTE> dData;
	if ( !sphJsonParse ( dData, sHeaderName, m_sLastError ) )
		return false;

	Bson_c tBson ( dData );
	if ( tBson.IsEmpty() || !tBson.IsAssoc() )
	{
		m_sLastError = "Something wrong read from json header - it is either empty, either not root object.";
		return false;
	}

	// version
	m_uVersion = (DWORD)Int ( tBson.ChildByName ( "index_format_version" ) );
	if ( m_uVersion<=1 || m_uVersion>INDEX_FORMAT_VERSION )
	{
		m_sLastError.SetSprintf ( "%s is v.%u, binary is v.%u", sHeaderName, m_uVersion, INDEX_FORMAT_VERSION );
		return false;
	}

	// we don't support anything prior to v54
	DWORD uMinFormatVer = 54;
	if ( m_uVersion<uMinFormatVer )
	{
		m_sLastError.SetSprintf ( "indexes prior to v.%u are no longer supported (use index_converter tool); %s is v.%u", uMinFormatVer, sHeaderName, m_uVersion );
		return false;
	}

	// index stats
	m_tStats.m_iTotalDocuments = Int ( tBson.ChildByName ( "total_documents" ) );
	m_tStats.m_iTotalBytes = Int ( tBson.ChildByName ( "total_bytes" ) );

	// schema
	ReadSchemaJson ( tBson.ChildByName ( "schema" ), m_tSchema );

	// check schema for dupes
	for ( int iAttr = 0; iAttr < m_tSchema.GetAttrsCount(); ++iAttr )
	{
		const CSphColumnInfo& tCol = m_tSchema.GetAttr ( iAttr );
		for ( int i = 0; i < iAttr; ++i )
			if ( m_tSchema.GetAttr ( i ).m_sName == tCol.m_sName )
				sWarning.SetSprintf ( "duplicate attribute name: %s", tCol.m_sName.cstr() );
	}

	// index settings
	LoadIndexSettingsJson ( tBson.ChildByName ( "index_settings" ), m_tSettings );

	CSphTokenizerSettings tTokSettings;
	// tokenizer stuff
	if ( !tTokSettings.Load ( pFilenameBuilder, tBson.ChildByName ( "tokenizer_settings" ), tEmbeddedFiles, m_sLastError ) )
		return false;

	// dictionary stuff
	CSphDictSettings tDictSettings;
	tDictSettings.Load ( tBson.ChildByName ( "dictionary_settings" ), tEmbeddedFiles, sWarning );

	// dictionary header (wordlist checkpoints, infix blocks, etc)
	m_tWordlist.m_iDictCheckpointsOffset = Int ( tBson.ChildByName ( "dict_checkpoints_offset" ) );
	m_tWordlist.m_iDictCheckpoints = (int)Int ( tBson.ChildByName ( "dict_checkpoints" ) );
	m_tWordlist.m_iInfixCodepointBytes = (int)Int ( tBson.ChildByName ( "infix_codepoint_bytes" ) );
	m_tWordlist.m_iInfixBlocksOffset = Int ( tBson.ChildByName ( "infix_blocks_offset" ) );
	m_tWordlist.m_iInfixBlocksWordsSize = (int)Int ( tBson.ChildByName ( "infix_block_words_size" ) );

	m_tWordlist.m_dCheckpoints.Reset ( m_tWordlist.m_iDictCheckpoints );

	if ( bStripPath )
	{
		StripPath ( tTokSettings.m_sSynonymsFile );
		for ( auto& i : tDictSettings.m_dWordforms )
			StripPath ( i );
	}

	StrVec_t dWarnings;
	TokenizerRefPtr_c pTokenizer { Tokenizer::Create ( tTokSettings, &tEmbeddedFiles, pFilenameBuilder, dWarnings, m_sLastError ) };
	if ( !pTokenizer )
		return false;

	DictRefPtr_c pDict { tDictSettings.m_bWordDict
		? sphCreateDictionaryKeywords ( tDictSettings, &tEmbeddedFiles, pTokenizer, m_sIndexName.cstr(), bStripPath, m_tSettings.m_iSkiplistBlockSize, pFilenameBuilder, m_sLastError )
		: sphCreateDictionaryCRC ( tDictSettings, &tEmbeddedFiles, pTokenizer, m_sIndexName.cstr(), bStripPath, m_tSettings.m_iSkiplistBlockSize, pFilenameBuilder, m_sLastError )};

	if ( !pDict )
		return false;

	if ( tDictSettings.m_sMorphFingerprint!=pDict->GetMorphDataFingerprint() )
		sWarning.SetSprintf ( "different lemmatizer dictionaries (index='%s', current='%s')",
			tDictSettings.m_sMorphFingerprint.cstr(),
			pDict->GetMorphDataFingerprint().cstr() );

	SetDictionary ( pDict );

	pTokenizer = Tokenizer::CreateMultiformFilter ( pTokenizer, pDict->GetMultiWordforms () );
	SetTokenizer ( pTokenizer );
	SetupQueryTokenizer();

	// initialize AOT if needed
	m_tSettings.m_uAotFilterMask = sphParseMorphAot ( tDictSettings.m_sMorphology.cstr() );

	m_iDocinfo = Int ( tBson.ChildByName ( "docinfo" ) );
	m_iDocinfoIndex = Int ( tBson.ChildByName ( "docinfo_index" ) );
	m_iMinMaxIndex = Int ( tBson.ChildByName ( "min_max_index" ) );

	FieldFilterRefPtr_c pFieldFilter;
	auto tFieldFilterSettingsNode = tBson.ChildByName ( "field_filter_settings" );
	if ( !IsNullNode(tFieldFilterSettingsNode) )
	{
		CSphFieldFilterSettings tFieldFilterSettings;
		Bson_c ( tFieldFilterSettingsNode ).ForEach ( [&tFieldFilterSettings] ( const NodeHandle_t& tNode ) {
			tFieldFilterSettings.m_dRegexps.Add ( String ( tNode ) );
		} );

		if ( !tFieldFilterSettings.m_dRegexps.IsEmpty() )
			pFieldFilter = sphCreateRegexpFilter ( tFieldFilterSettings, m_sLastError );
	}

	if ( !sphSpawnFilterICU ( pFieldFilter, m_tSettings, tTokSettings, sHeaderName, m_sLastError ) )
		return false;

	SetFieldFilter ( pFieldFilter );

	auto tIndexFieldsLenNode = tBson.ChildByName ( "index_fields_lens" );
	if ( m_tSettings.m_bIndexFieldLens )
	{
		assert (!IsNullNode ( tIndexFieldsLenNode ));
		m_dFieldLens.Reset ( m_tSchema.GetFieldsCount() );
		int i = 0;
		Bson_c ( tIndexFieldsLenNode ).ForEach ( [&i,this] ( const NodeHandle_t& tNode ) {
			m_dFieldLens[i++] = Int ( tNode );
		} );
	}

	// post-load stuff.. for now, bigrams
	CSphIndexSettings & s = m_tSettings;
	if ( s.m_eBigramIndex!=SPH_BIGRAM_NONE && s.m_eBigramIndex!=SPH_BIGRAM_ALL )
	{
		BYTE * pTok;
		m_pTokenizer->SetBuffer ( (BYTE*)const_cast<char*> ( s.m_sBigramWords.cstr() ), s.m_sBigramWords.Length() );
		while ( ( pTok = m_pTokenizer->GetToken() )!=nullptr )
			s.m_dBigramWords.Add() = (const char*)pTok;
		s.m_dBigramWords.Sort();
	}

	return true;
}


void CSphIndex_VLN::DebugDumpHeader ( FILE * fp, const char * sHeaderName, bool bConfig )
{
	CreateFilenameBuilder_fn fnCreateFilenameBuilder = GetIndexFilenameBuilder();
	CSphScopedPtr<FilenameBuilder_i> pFilenameBuilder ( fnCreateFilenameBuilder ? fnCreateFilenameBuilder ( m_sIndexName.cstr() ) : nullptr );

	CSphEmbeddedFiles tEmbeddedFiles;
	CSphString sWarning;
	if ( !LoadHeader ( sHeaderName, false, tEmbeddedFiles, pFilenameBuilder.Ptr(), sWarning ) )
		sphDie ( "failed to load header: %s", m_sLastError.cstr ());

	if ( !sWarning.IsEmpty () )
		fprintf ( fp, "WARNING: %s\n", sWarning.cstr () );

	///////////////////////////////////////////////
	// print header in index config section format
	///////////////////////////////////////////////

	if ( bConfig )
	{
		fprintf ( fp, "\nsource $dump\n{\n" );

		fprintf ( fp, "\tsql_query = SELECT id \\\n" );
		for ( int i=0; i < m_tSchema.GetFieldsCount(); i++ )
			fprintf ( fp, "\t, %s \\\n", m_tSchema.GetFieldName(i) );
		for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
		{
			const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
			fprintf ( fp, "\t, %s \\\n", tAttr.m_sName.cstr() );
		}
		fprintf ( fp, "\tFROM documents\n" );

		if ( m_tSchema.GetAttrsCount() )
			fprintf ( fp, "\n" );

		for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
		{
			const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
			if ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET )
				fprintf ( fp, "\tsql_attr_multi = uint %s from field\n", tAttr.m_sName.cstr() );
			else if ( tAttr.m_eAttrType==SPH_ATTR_INT64SET )
				fprintf ( fp, "\tsql_attr_multi = bigint %s from field\n", tAttr.m_sName.cstr() );
			else if ( tAttr.m_eAttrType==SPH_ATTR_INTEGER && tAttr.m_tLocator.IsBitfield() )
				fprintf ( fp, "\tsql_attr_uint = %s:%d\n", tAttr.m_sName.cstr(), tAttr.m_tLocator.m_iBitCount );
			else if ( tAttr.m_eAttrType==SPH_ATTR_TOKENCOUNT )
			{; // intendedly skip, as these are autogenerated by index_field_lengths=1
			} else
				fprintf ( fp, "\t%s = %s\n", sphTypeDirective ( tAttr.m_eAttrType ), tAttr.m_sName.cstr() );
		}

		fprintf ( fp, "}\n\nindex $dump\n{\n\tsource = $dump\n\tpath = $dump\n" );

		DumpSettingsCfg ( fp, *this, pFilenameBuilder.Ptr() );

		fprintf ( fp, "}\n" );
		return;
	}

	///////////////////////////////////////////////
	// print header and stats in "readable" format
	///////////////////////////////////////////////

	fprintf ( fp, "version: %u\n",			m_uVersion );
	fprintf ( fp, "idbits: 64\n" );

	fprintf ( fp, "fields: %d\n", m_tSchema.GetFieldsCount() );
	for ( int i = 0; i < m_tSchema.GetFieldsCount(); i++ )
		fprintf ( fp, "  field %d: %s\n", i, m_tSchema.GetFieldName(i) );

	fprintf ( fp, "attrs: %d\n", m_tSchema.GetAttrsCount() );
	for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
		fprintf ( fp, "  attr %d: %s, %s", i, tAttr.m_sName.cstr(), sphTypeName ( tAttr.m_eAttrType ) );
		if ( tAttr.m_eAttrType==SPH_ATTR_INTEGER && tAttr.m_tLocator.m_iBitCount!=32 )
			fprintf ( fp, ", bits %d", tAttr.m_tLocator.m_iBitCount );
		fprintf ( fp, ", bitoff %d\n", tAttr.m_tLocator.m_iBitOffset );
	}

	// skipped min doc, wordlist checkpoints
	fprintf ( fp, "total-documents: " INT64_FMT "\n", m_tStats.m_iTotalDocuments );
	fprintf ( fp, "total-bytes: " INT64_FMT "\n", int64_t(m_tStats.m_iTotalBytes) );

	DumpReadable ( fp, *this, tEmbeddedFiles, pFilenameBuilder.Ptr() );

	fprintf ( fp, "\nmin-max-index: " INT64_FMT "\n", m_iMinMaxIndex );
}


void CSphIndex_VLN::DebugDumpDocids ( FILE * fp )
{
	const int iRowStride = m_tSchema.GetRowSize();

	const int64_t iNumMinMaxRow = (m_iDocinfoIndex+1)*iRowStride*2;
	const int64_t iNumRows = m_iDocinfo;

	const int64_t iDocinfoSize = iRowStride*m_iDocinfo*sizeof(DWORD);
	const int64_t iMinmaxSize = iNumMinMaxRow*sizeof(CSphRowitem);

	fprintf ( fp, "docinfo-bytes: docinfo=" INT64_FMT ", min-max=" INT64_FMT ", total=" UINT64_FMT "\n", iDocinfoSize, iMinmaxSize, (uint64_t)m_tAttr.GetLengthBytes() );
	fprintf ( fp, "docinfo-stride: %d\n", (int)(iRowStride*sizeof(DWORD)) );
	fprintf ( fp, "docinfo-rows: " INT64_FMT "\n", iNumRows );

	if ( !m_tAttr.GetLength64() )
		return;

	DWORD * pDocinfo = m_tAttr.GetWritePtr();
	for ( int64_t iRow=0; iRow<iNumRows; iRow++, pDocinfo+=iRowStride )
		printf ( INT64_FMT". id=" INT64_FMT "\n", iRow+1, sphGetDocID ( pDocinfo ) );

	printf ( "--- min-max=" INT64_FMT " ---\n", iNumMinMaxRow );
	for ( int64_t iRow=0; iRow<(m_iDocinfoIndex+1)*2; iRow++, pDocinfo+=iRowStride )
		printf ( "id=" INT64_FMT "\n", sphGetDocID ( pDocinfo ) );
}


void CSphIndex_VLN::DebugDumpHitlist ( FILE * fp, const char * sKeyword, bool bID )
{
	WITH_QWORD ( this, false, Qword, DumpHitlist<Qword> ( fp, sKeyword, bID ) );
}


template < class Qword >
void CSphIndex_VLN::DumpHitlist ( FILE * fp, const char * sKeyword, bool bID )
{
	// get keyword id
	SphWordID_t uWordID = 0;
	BYTE * sTok = NULL;
	if ( !bID )
	{
		CSphString sBuf ( sKeyword );

		m_pTokenizer->SetBuffer ( (const BYTE*)sBuf.cstr(), (int) strlen ( sBuf.cstr() ) );
		sTok = m_pTokenizer->GetToken();

		if ( !sTok )
			sphDie ( "keyword=%s, no token (too short?)", sKeyword );

		uWordID = m_pDict->GetWordID ( sTok );
		if ( !uWordID )
			sphDie ( "keyword=%s, tok=%s, no wordid (stopped?)", sKeyword, sTok );

		fprintf ( fp, "keyword=%s, tok=%s, wordid=" UINT64_FMT "\n", sKeyword, sTok, uint64_t(uWordID) );

	} else
	{
		uWordID = (SphWordID_t) strtoull ( sKeyword, NULL, 10 );
		if ( !uWordID )
			sphDie ( "failed to convert keyword=%s to id (must be integer)", sKeyword );

		fprintf ( fp, "wordid=" UINT64_FMT "\n", uint64_t(uWordID) );
	}

	// open files
	DataReaderFactoryPtr_c pDoclist {
		NewProxyReader ( GetIndexFileName ( SPH_EXT_SPD ), m_sLastError, DataReaderFactory_c::DOCS,
			m_tMutableSettings.m_tFileAccess.m_iReadBufferDocList, FileAccess_e::FILE )
	};
	if ( !pDoclist )
		sphDie ( "failed to open doclist: %s", m_sLastError.cstr() );

	DataReaderFactoryPtr_c pHitlist {
		NewProxyReader ( GetIndexFileName ( SPH_EXT_SPP ), m_sLastError, DataReaderFactory_c::HITS,
			m_tMutableSettings.m_tFileAccess.m_iReadBufferHitList, FileAccess_e::FILE )
	};
	if ( !pHitlist )
		sphDie ( "failed to open hitlist: %s", m_sLastError.cstr ());


	// aim
	DiskIndexQwordSetup_c tTermSetup ( pDoclist, pHitlist, m_tSkiplists.GetWritePtr(), m_tSettings.m_iSkiplistBlockSize, true, RowID_t(m_iDocinfo) );
	tTermSetup.SetDict ( m_pDict );
	tTermSetup.m_pIndex = this;

	Qword tKeyword ( false, false, m_iIndexId );
	tKeyword.m_uWordID = uWordID;
	tKeyword.m_sWord = sKeyword;
	tKeyword.m_sDictWord = (const char *)sTok;
	if ( !tTermSetup.QwordSetup ( &tKeyword ) )
		sphDie ( "failed to setup keyword" );

	// press play on tape
	while (true)
	{
		tKeyword.GetNextDoc();
		if ( tKeyword.m_tDoc.m_tRowID==INVALID_ROWID )
			break;

		tKeyword.SeekHitlist ( tKeyword.m_iHitlistPos );

		int iHits = 0;
		if ( tKeyword.m_bHasHitlist )
			for ( Hitpos_t uHit = tKeyword.GetNextHit(); uHit!=EMPTY_HIT; uHit = tKeyword.GetNextHit() )
			{
				fprintf ( fp, "doc=%u, hit=0x%08x\n", tKeyword.m_tDoc.m_tRowID, (uint32_t)uHit );
				iHits++;
			}

		if ( !iHits )
		{
			uint64_t uOff = tKeyword.m_iHitlistPos;
			fprintf ( fp, "doc=%u, NO HITS, inline=%d, off=" UINT64_FMT "\n", tKeyword.m_tDoc.m_tRowID, (int)(uOff>>63), (uOff<<1)>>1 );
		}
	}
}


void CSphIndex_VLN::DebugDumpDict ( FILE * fp )
{
	if ( !m_pDict->GetSettings().m_bWordDict )
	{
		sphDie ( "DebugDumpDict() only supports dict=keywords for now" );
	}

	fprintf ( fp, "keyword,docs,hits,offset\n" );
	m_tWordlist.DebugPopulateCheckpoints();
	ARRAY_FOREACH ( i, m_tWordlist.m_dCheckpoints )
	{
		KeywordsBlockReader_c tCtx ( m_tWordlist.AcquireDict ( &m_tWordlist.m_dCheckpoints[i] ), m_tSettings.m_iSkiplistBlockSize );
		while ( tCtx.UnpackWord() )
			fprintf ( fp, "%s,%d,%d," INT64_FMT "\n", tCtx.GetWord(), tCtx.m_iDocs, tCtx.m_iHits, int64_t(tCtx.m_iDoclistOffset) );
	}
}

//////////////////////////////////////////////////////////////////////////

bool CSphIndex_VLN::SpawnReader ( DataReaderFactoryPtr_c & m_pFile, ESphExt eExt, DataReaderFactory_c::Kind_e eKind, int iBuffer, FileAccess_e eAccess )
{
	if ( m_tMutableSettings.m_bPreopen || eAccess!=FileAccess_e::FILE )
	{
		m_pFile = NewProxyReader ( GetIndexFileName(eExt), m_sLastError, eKind, iBuffer, eAccess );
		if ( !m_pFile )
			return false;
	}

	return true;
}


bool CSphIndex_VLN::SpawnReaders()
{
	if ( !SpawnReader ( m_pDoclistFile, SPH_EXT_SPD, DataReaderFactory_c::DOCS, m_tMutableSettings.m_tFileAccess.m_iReadBufferDocList, m_tMutableSettings.m_tFileAccess.m_eDoclist ) )
		return false;

	if ( !SpawnReader ( m_pHitlistFile, SPH_EXT_SPP, DataReaderFactory_c::HITS, m_tMutableSettings.m_tFileAccess.m_iReadBufferHitList, m_tMutableSettings.m_tFileAccess.m_eHitlist ) )
		return false;

	return true;
}


bool CSphIndex_VLN::PreallocWordlist()
{
	if ( !sphIsReadable ( GetIndexFileName(SPH_EXT_SPI).cstr(), &m_sLastError ) )
		return false;

	// might be no dictionary at this point for old index format
	bool bWordDict = m_pDict && m_pDict->GetSettings().m_bWordDict;

	// only checkpoint and wordlist infixes are actually read here; dictionary itself is just mapped
	if ( !m_tWordlist.Preread ( GetIndexFileName(SPH_EXT_SPI), bWordDict, m_tSettings.m_iSkiplistBlockSize, m_sLastError ) )
		return false;

	if ( ( m_tWordlist.m_tBuf.GetLengthBytes()<=1 )!=( m_tWordlist.m_dCheckpoints.GetLength()==0 ) )
		sphWarning ( "wordlist size mismatch (size=%zu, checkpoints=%d)", m_tWordlist.m_tBuf.GetLengthBytes(), m_tWordlist.m_dCheckpoints.GetLength() );

	// make sure checkpoints are loadable
	// pre-11 indices use different offset type (this is fixed up later during the loading)
	assert ( m_tWordlist.m_iDictCheckpointsOffset>0 );

	return true;
}


bool CSphIndex_VLN::PreallocAttributes()
{
	if ( m_bIsEmpty || m_bDebugCheck )
		return true;

	if ( !m_tSchema.HasNonColumnarAttrs() )
		return true;

	if ( !m_tAttr.Setup ( GetIndexFileName(SPH_EXT_SPA), m_sLastError, true ) )
		return false;

	if ( !CheckDocsCount ( m_iDocinfo, m_sLastError ) )
		return false;

	m_pDocinfoIndex = m_tAttr.GetWritePtr() + m_iMinMaxIndex;

	if ( m_tSchema.GetAttr ( sphGetBlobLocatorName() ) )
	{
		if ( !m_tBlobAttrs.Setup ( GetIndexFileName(SPH_EXT_SPB), m_sLastError, true ) )
			return false;
	}

	return true;
}


bool CSphIndex_VLN::PreallocDocidLookup()
{
	if ( m_bIsEmpty || m_bDebugCheck )
		return true;

	if ( !m_tDocidLookup.Setup ( GetIndexFileName(SPH_EXT_SPT), m_sLastError, false ) )
		return false;

	m_tLookupReader.SetData ( m_tDocidLookup.GetWritePtr() );

	return true;
}


bool CSphIndex_VLN::PreallocKilllist()
{
	if ( m_bDebugCheck )
		return true;

	return m_tDeadRowMap.Prealloc ( (DWORD)m_iDocinfo, GetIndexFileName(SPH_EXT_SPM), m_sLastError );
}


bool CSphIndex_VLN::PreallocHistograms ( StrVec_t & dWarnings )
{
	if ( m_bDebugCheck )
		return true;

	// we have histograms since v.56, but in v.61 we switched to streamed histograms with no backward compatibility
	if ( m_uVersion<61 )
		return true;

	CSphString sHistogramFile = GetIndexFileName(SPH_EXT_SPHI);
	if ( !sphIsReadable ( sHistogramFile.cstr() ) )
		return true;

	SafeDelete ( m_pHistograms );
	m_pHistograms = new HistogramContainer_c;

	if ( !m_pHistograms->Load ( sHistogramFile, m_sLastError ) )
	{
		SafeDelete ( m_pHistograms );
		if ( !m_sLastError.IsEmpty() )
			dWarnings.Add(m_sLastError);
	}

	// even if we fail to load the histograms, return true (histograms are optional anyway)
	return true;
}


bool CSphIndex_VLN::PreallocDocstore()
{
	if ( m_uVersion<57 )
		return true;

	if ( !m_tSchema.HasStoredFields() && !m_tSchema.HasStoredAttrs() )
		return true;

	m_pDocstore = CreateDocstore ( m_iIndexId, GetIndexFileName(SPH_EXT_SPDS), m_sLastError );

	return !!m_pDocstore;
}


bool CSphIndex_VLN::PreallocColumnar()
{
	if ( m_uVersion<61 )
		return true;

	if ( !m_tSchema.HasColumnarAttrs() )
		return true;

	m_pColumnar = CreateColumnarStorageReader ( GetIndexFileName(SPH_EXT_SPC).cstr(), (DWORD)m_iDocinfo, m_sLastError );
	return !!m_pColumnar;
}


bool CSphIndex_VLN::PreallocSkiplist()
{
	if ( m_bDebugCheck )
		return true;

	return m_tSkiplists.Setup ( GetIndexFileName(SPH_EXT_SPE), m_sLastError, false );
}


bool CSphIndex_VLN::Prealloc ( bool bStripPath, FilenameBuilder_i * pFilenameBuilder, StrVec_t & dWarnings )
{
	MEMORY ( MEM_INDEX_DISK );

	Dealloc();

	CSphEmbeddedFiles tEmbeddedFiles;

	// preload schema
	if ( !LoadHeader ( GetIndexFileName ( SPH_EXT_SPH ).cstr(), bStripPath, tEmbeddedFiles, pFilenameBuilder, m_sLastWarning ) )
	{
		sphWarning ( "Unable to load json! Will retry with plain legacy sph..." );
		if ( !LoadHeaderLegacy ( GetIndexFileName ( SPH_EXT_SPH ).cstr(), bStripPath, tEmbeddedFiles, pFilenameBuilder, m_sLastWarning ) )
		{
			sphWarning ( "Unable to load header.." );
			return false;
		}
	}

	m_bIsEmpty = !m_iDocinfo;

	tEmbeddedFiles.Reset();

	// verify that data files are readable
	if ( !sphIsReadable ( GetIndexFileName(SPH_EXT_SPD).cstr(), &m_sLastError ) )
		return false;

	if ( !sphIsReadable ( GetIndexFileName(SPH_EXT_SPP).cstr(), &m_sLastError ) )
		return false;

	if ( !sphIsReadable ( GetIndexFileName(SPH_EXT_SPE).cstr(), &m_sLastError ) )
		return false;

	if ( !SpawnReaders() )
		return false;

	if ( !PreallocWordlist() )		return false;
	if ( !PreallocAttributes() )	return false;
	if ( !PreallocDocidLookup() )	return false;
	if ( !PreallocKilllist() )		return false;
	if ( !PreallocHistograms(dWarnings) ) return false;
	if ( !PreallocDocstore() )		return false;
	if ( !PreallocColumnar() )		return false;
	if ( !PreallocSkiplist() )		return false;

	// almost done
	m_bPassedAlloc = true;

	return true;
}


void CSphIndex_VLN::Preread()
{
	MEMORY ( MEM_INDEX_DISK );

	sphLogDebug ( "CSphIndex_VLN::Preread invoked '%s'(%s)", m_sIndexName.cstr(), m_sFilename.cstr() );

	assert ( m_bPassedAlloc );
	if ( m_bPassedRead )
		return;

	///////////////////
	// read everything
	///////////////////

	PrereadMapping ( m_sIndexName.cstr(), "attributes", IsMlock ( m_tMutableSettings.m_tFileAccess.m_eAttr ), IsOndisk ( m_tMutableSettings.m_tFileAccess.m_eAttr ), m_tAttr );
	PrereadMapping ( m_sIndexName.cstr(), "blobs", IsMlock ( m_tMutableSettings.m_tFileAccess.m_eBlob ), IsOndisk ( m_tMutableSettings.m_tFileAccess.m_eBlob ), m_tBlobAttrs );
	PrereadMapping ( m_sIndexName.cstr(), "skip-list", IsMlock ( m_tMutableSettings.m_tFileAccess.m_eAttr ), false, m_tSkiplists );
	PrereadMapping ( m_sIndexName.cstr(), "dictionary", IsMlock ( m_tMutableSettings.m_tFileAccess.m_eAttr ), false, m_tWordlist.m_tBuf );
	PrereadMapping ( m_sIndexName.cstr(), "docid-lookup", IsMlock ( m_tMutableSettings.m_tFileAccess.m_eAttr ), false, m_tDocidLookup );
	m_tDeadRowMap.Preread ( m_sIndexName.cstr(), "kill-list", IsMlock ( m_tMutableSettings.m_tFileAccess.m_eAttr ) );

	m_bPassedRead = true;
	sphLogDebug ( "Preread successfully finished" );
}

void CSphIndex_VLN::SetBase ( const char * sNewBase )
{
	m_sFilename = sNewBase;
}


bool CSphIndex_VLN::Rename ( const char * sNewBase )
{
	if ( m_sFilename==sNewBase )
		return true;

	IndexFiles_c dFiles ( m_sFilename, nullptr, m_uVersion );
	if ( !dFiles.RenameBase ( sNewBase ) )
	{
		m_sLastError = dFiles.ErrorMsg ();
		return false;
	}

	if ( !dFiles.RenameLock ( sNewBase, m_iLockFD ) )
	{
		m_sLastError = dFiles.ErrorMsg ();
		return false;
	}

	SetBase ( sNewBase );

	return true;
}

//////////////////////////////////////////////////////////////////////////

CSphQueryContext::CSphQueryContext ( const CSphQuery & q )
	: m_tQuery ( q )
{}

CSphQueryContext::~CSphQueryContext ()
{
	ResetFilters();
}

void CSphQueryContext::ResetFilters()
{
	SafeDelete ( m_pFilter );
	SafeDelete ( m_pWeightFilter );
	m_dUserVals.Reset();
}

void CSphQueryContext::BindWeights ( const CSphQuery & tQuery, const CSphSchema & tSchema, CSphString & sWarning )
{
	const int HEAVY_FIELDS = SPH_MAX_FIELDS;

	// defaults
	m_iWeights = Min ( tSchema.GetFieldsCount(), HEAVY_FIELDS );
	for ( int i=0; i<m_iWeights; ++i )
		m_dWeights[i] = 1;

	// name-bound weights
	CSphString sFieldsNotFound;
	if ( !tQuery.m_dFieldWeights.IsEmpty() )
	{
		for ( auto& tWeight : tQuery.m_dFieldWeights )
		{
			int j = tSchema.GetFieldIndex ( tWeight.first.cstr() );
			if ( j<0 )
			{
				if ( sFieldsNotFound.IsEmpty() )
					sFieldsNotFound = tWeight.first;
				else
					sFieldsNotFound.SetSprintf ( "%s %s", sFieldsNotFound.cstr(), tWeight.first.cstr() );
			}

			if ( j>=0 && j<HEAVY_FIELDS )
				m_dWeights[j] = tWeight.second;
		}

		if ( !sFieldsNotFound.IsEmpty() )
			sWarning.SetSprintf ( "Fields specified in field_weights option not found: [%s]", sFieldsNotFound.cstr() );

		return;
	}

	// order-bound weights
	if ( !tQuery.m_dWeights.IsEmpty() )
	{
		for ( int i=0, iLim=Min ( m_iWeights, tQuery.m_dWeights.GetLength() ); i<iLim; ++i )
			m_dWeights[i] = (int) tQuery.m_dWeights[i];
	}
}

static ESphEvalStage GetEarliestStage ( ESphEvalStage eStage, const CSphColumnInfo & tIn, const CSphVector<const ISphSchema *> & dSchemas )
{
	for ( const auto * pSchema : dSchemas )
	{
		const CSphColumnInfo * pCol = pSchema->GetAttr ( tIn.m_sName.cstr() );
		if ( !pCol || pCol->IsColumnar() )
			continue;

		eStage = Min ( eStage, pCol->m_eStage );
	}

	return eStage;
}


bool CSphQueryContext::SetupCalc ( CSphQueryResultMeta & tMeta, const ISphSchema & tInSchema, const CSphSchema & tSchema, const BYTE * pBlobPool, const columnar::Columnar_i * pColumnar, const CSphVector<const ISphSchema *> & dInSchemas )
{
	m_dCalcFilter.Resize(0);
	m_dCalcSort.Resize(0);
	m_dCalcFinal.Resize(0);

	m_dCalcFilterPtrAttrs.Resize(0);
	m_dCalcSortPtrAttrs.Resize(0);

	// quickly verify that all my real attributes can be stashed there
	if ( tInSchema.GetAttrsCount() < tSchema.GetAttrsCount() )
	{
		tMeta.m_sError.SetSprintf ( "INTERNAL ERROR: incoming-schema mismatch (incount=%d, mycount=%d)",
			tInSchema.GetAttrsCount(), tSchema.GetAttrsCount() );
		return false;
	}

	// now match everyone
	for ( int iIn=0; iIn<tInSchema.GetAttrsCount(); iIn++ )
	{
		const CSphColumnInfo & tIn = tInSchema.GetAttr(iIn);

		// recalculate stage as sorters set column at earlier stage
		// FIXME!!! should we update column?
		ESphEvalStage eStage = GetEarliestStage ( tIn.m_eStage, tIn, dInSchemas );

		switch ( eStage )
		{
			case SPH_EVAL_STATIC:
			{
				// this check may significantly slow down queries with huge schema attribute count
#ifndef NDEBUG
				const CSphColumnInfo * pMy = tSchema.GetAttr ( tIn.m_sName.cstr() );
				if ( !pMy )
				{
					tMeta.m_sError.SetSprintf ( "INTERNAL ERROR: incoming-schema attr missing from index-schema (in=%s)",
						sphDumpAttr(tIn).cstr() );
					return false;
				}

				// static; check for full match
				if (!( tIn==*pMy ))
				{
					assert ( 0 );
					tMeta.m_sError.SetSprintf ( "INTERNAL ERROR: incoming-schema mismatch (in=%s, my=%s)",
						sphDumpAttr(tIn).cstr(), sphDumpAttr(*pMy).cstr() );
					return false;
				}
#endif
				break;
			}

			case SPH_EVAL_PREFILTER:
			case SPH_EVAL_PRESORT:
			case SPH_EVAL_FINAL:
			{
				ISphExprRefPtr_c pExpr { tIn.m_pExpr };
				if ( !pExpr )
				{
					tMeta.m_sError.SetSprintf ( "INTERNAL ERROR: incoming-schema expression missing evaluator (stage=%d, in=%s)",
						(int)eStage, sphDumpAttr(tIn).cstr() );
					return false;
				}

				// an expression that index/searcher should compute
				CalcItem_t tCalc;
				tCalc.m_eType = tIn.m_eAttrType;
				tCalc.m_tLoc = tIn.m_tLocator;
				tCalc.m_pExpr = std::move(pExpr);
				tCalc.m_pExpr->Command ( SPH_EXPR_SET_BLOB_POOL, (void*)&pBlobPool );
				tCalc.m_pExpr->Command ( SPH_EXPR_SET_COLUMNAR, (void*)pColumnar );

				switch ( eStage )
				{
					case SPH_EVAL_PREFILTER:	AddToFilterCalc(tCalc); break;
					case SPH_EVAL_PRESORT:		AddToSortCalc(tCalc); break;
					case SPH_EVAL_FINAL:		m_dCalcFinal.Add(tCalc); break;
					default:					break;
				}
				break;
			}

			case SPH_EVAL_SORTER:
				// sorter tells it will compute itself; so just skip it
			case SPH_EVAL_POSTLIMIT:
				break;

			default:
				tMeta.m_sError.SetSprintf ( "INTERNAL ERROR: unhandled eval stage=%d", (int)eStage );
				return false;
		}
	}

	// ok, we can emit matches in this schema (incoming for sorter, outgoing for index/searcher)
	return true;
}


void CSphQueryContext::AddToFilterCalc ( const CalcItem_t & tCalc )
{
	m_dCalcFilter.Add(tCalc);
	if ( sphIsDataPtrAttr ( tCalc.m_eType ) )
		m_dCalcFilterPtrAttrs.Add ( m_dCalcFilter.GetLength()-1 );
}


void CSphQueryContext::AddToSortCalc ( const CalcItem_t & tCalc )
{
	m_dCalcSort.Add(tCalc);
	if ( sphIsDataPtrAttr ( tCalc.m_eType ) )
		m_dCalcSortPtrAttrs.Add ( m_dCalcSort.GetLength()-1 );
}


bool CSphIndex::IsStarDict ( bool bWordDict ) const
{
	return m_tSettings.GetMinPrefixLen ( bWordDict )>0 || m_tSettings.m_iMinInfixLen>0;
}


void CSphIndex_VLN::SetupStarDict ( DictRefPtr_c &pDict ) const
{
	// spawn wrapper, and put it in the box
	// wrapper type depends on version; v.8 introduced new mangling rules
	if ( IsStarDict ( pDict->GetSettings().m_bWordDict ) )
		pDict = new CSphDictStarV8 ( pDict, m_tSettings.m_iMinInfixLen>0 );

	// FIXME? might wanna verify somehow that the tokenizer has '*' as a character
}

void CSphIndex_VLN::SetupExactDict ( DictRefPtr_c &pDict ) const
{
	if ( m_tSettings.m_bIndexExactWords )
		pDict = new CSphDictExact ( pDict );
}


bool CSphIndex_VLN::GetKeywords ( CSphVector <CSphKeywordInfo> & dKeywords,
	const char * szQuery, const GetKeywordsSettings_t & tSettings, CSphString * pError ) const
{
	WITH_QWORD ( this, false, Qword, return DoGetKeywords<Qword> ( dKeywords, szQuery, tSettings, false, pError ) );
	return false;
}

void CSphIndex_VLN::GetSuggest ( const SuggestArgs_t & tArgs, SuggestResult_t & tRes ) const
{
	if ( m_tWordlist.m_tBuf.IsEmpty() || !m_tWordlist.m_dCheckpoints.GetLength() )
		return;

	assert ( !tRes.m_pWordReader );
	tRes.m_pWordReader = new KeywordsBlockReader_c ( m_tWordlist.m_tBuf.GetWritePtr(), m_tSettings.m_iSkiplistBlockSize );
	tRes.m_bHasExactDict = m_tSettings.m_bIndexExactWords;

	sphGetSuggest ( &m_tWordlist, m_tWordlist.m_iInfixCodepointBytes, tArgs, tRes );

	KeywordsBlockReader_c * pReader = (KeywordsBlockReader_c *)tRes.m_pWordReader;
	SafeDelete ( pReader );
	tRes.m_pWordReader = NULL;
}


DWORD sphParseMorphAot ( const char * sMorphology )
{
	if ( !sMorphology || !*sMorphology )
		return 0;

	StrVec_t dMorphs;
	sphSplit ( dMorphs, sMorphology );

	DWORD uAotFilterMask = 0;
	for ( int j=0; j<AOT_LENGTH; ++j )
	{
		char buf_all[20];
		sprintf ( buf_all, "lemmatize_%s_all", AOT_LANGUAGES[j] ); // NOLINT
		ARRAY_FOREACH ( i, dMorphs )
		{
			if ( dMorphs[i]==buf_all )
			{
				uAotFilterMask |= (1UL) << j;
				break;
			}
		}
	}

	return uAotFilterMask;
}


template < class Qword >
bool CSphIndex_VLN::DoGetKeywords ( CSphVector <CSphKeywordInfo> & dKeywords,
	const char * szQuery, const GetKeywordsSettings_t & tSettings, bool bFillOnly, CSphString * pError ) const
{
	if ( !bFillOnly )
		dKeywords.Resize ( 0 );

	if ( !m_bPassedAlloc )
	{
		if ( pError )
			*pError = "index not preread";
		return false;
	}

	// short-cut if no query or keywords to fill
	if ( ( bFillOnly && !dKeywords.GetLength() ) || ( !bFillOnly && ( !szQuery || !szQuery[0] ) ) )
		return true;

	DictRefPtr_c pDict { GetStatelessDict ( m_pDict ) };
	SetupStarDict ( pDict );
	SetupExactDict ( pDict );

	CSphVector<BYTE> dFiltered;
	FieldFilterRefPtr_c pFieldFilter;
	const BYTE * sModifiedQuery = (const BYTE *)szQuery;
	if ( m_pFieldFilter && szQuery )
	{
		pFieldFilter = m_pFieldFilter->Clone();
		if ( pFieldFilter && pFieldFilter->Apply ( sModifiedQuery, dFiltered, true ) )
			sModifiedQuery = dFiltered.Begin();
	}

	// FIXME!!! missed bigram, add flags to fold blended parts, show expanded terms

	// prepare for setup
	DataReaderFactory_c * tDummy1 = nullptr;
	DataReaderFactory_c * tDummy2 = nullptr;

	DiskIndexQwordSetup_c tTermSetup ( tDummy1, tDummy2, m_tSkiplists.GetWritePtr(), m_tSettings.m_iSkiplistBlockSize, false, RowID_t(m_iDocinfo) );
	tTermSetup.SetDict ( pDict );
	tTermSetup.m_pIndex = this;

	Qword tQueryWord ( false, false, m_iIndexId );

	if ( bFillOnly )
	{
		BYTE sWord[MAX_KEYWORD_BYTES];

		ARRAY_FOREACH ( i, dKeywords )
		{
			CSphKeywordInfo &tInfo = dKeywords[i];
			int iLen = tInfo.m_sTokenized.Length ();
			memcpy ( sWord, tInfo.m_sTokenized.cstr (), iLen );
			sWord[iLen] = '\0';

			SphWordID_t iWord = pDict->GetWordID ( sWord );
			if ( iWord )
			{
				tQueryWord.Reset ();
				tQueryWord.m_sWord = tInfo.m_sTokenized;
				tQueryWord.m_sDictWord = ( const char * ) sWord;
				tQueryWord.m_uWordID = iWord;
				tTermSetup.QwordSetup ( &tQueryWord );

				tInfo.m_iDocs += tQueryWord.m_iDocs;
				tInfo.m_iHits += tQueryWord.m_iHits;
			}
		}
		return true;
	}

	bool bWordDict = pDict->GetSettings ().m_bWordDict;

	TokenizerRefPtr_c pTokenizer { m_pTokenizer->Clone ( SPH_CLONE_INDEX ) };
	pTokenizer->EnableTokenizedMultiformTracking ();

	// need to support '*' and '=' but not the other specials
	// so m_pQueryTokenizer does not work for us, gotta clone and setup one manually
	if ( IsStarDict ( bWordDict ) )
		pTokenizer->AddPlainChars ( "*" );
	if ( m_tSettings.m_bIndexExactWords )
		pTokenizer->AddPlainChars ( "=" );

	ExpansionContext_t tExpCtx;
	// query defined options
	tExpCtx.m_iExpansionLimit = ( tSettings.m_iExpansionLimit ? tSettings.m_iExpansionLimit : m_iExpansionLimit );
	bool bExpandWildcards = ( bWordDict && IsStarDict ( bWordDict ) && !tSettings.m_bFoldWildcards );

	CSphPlainQueryFilter tAotFilter;
	tAotFilter.m_pTokenizer = pTokenizer;
	tAotFilter.m_pDict = pDict;
	tAotFilter.m_pSettings = &m_tSettings;
	tAotFilter.m_pTermSetup = &tTermSetup;
	tAotFilter.m_pQueryWord = &tQueryWord;
	tAotFilter.m_tFoldSettings = tSettings;
	tAotFilter.m_tFoldSettings.m_bFoldWildcards = !bExpandWildcards;

	tExpCtx.m_pWordlist = &m_tWordlist;
	tExpCtx.m_iMinPrefixLen = m_tSettings.GetMinPrefixLen ( bWordDict );
	tExpCtx.m_iMinInfixLen = m_tSettings.m_iMinInfixLen;
	tExpCtx.m_bHasExactForms = ( m_pDict->HasMorphology() || m_tSettings.m_bIndexExactWords );
	tExpCtx.m_bMergeSingles = false;
	tExpCtx.m_eHitless = m_tSettings.m_eHitless;

	pTokenizer->SetBuffer ( sModifiedQuery, (int) strlen ( (const char *)sModifiedQuery) );

	tAotFilter.GetKeywords ( dKeywords, tExpCtx );
	return true;
}


bool CSphIndex_VLN::FillKeywords ( CSphVector <CSphKeywordInfo> & dKeywords ) const
{
	GetKeywordsSettings_t tSettings;
	tSettings.m_bStats = true;

	WITH_QWORD ( this, false, Qword, return DoGetKeywords<Qword> ( dKeywords, NULL, tSettings, true, NULL ) );
	return false;
}


bool CSphQueryContext::CreateFilters ( CreateFilterContext_t & tCtx, CSphString & sError, CSphString & sWarning )
{
	if ( !tCtx.m_pFilters || tCtx.m_pFilters->IsEmpty () )
		return true;
	if ( !sphCreateFilters ( tCtx, sError, sWarning ) )
		return false;

	m_pFilter = tCtx.m_pFilter;
	m_pWeightFilter = tCtx.m_pWeightFilter;
	m_dUserVals.SwapData ( tCtx.m_dUserVals );
	tCtx.m_pFilter = nullptr;
	tCtx.m_pWeightFilter = nullptr;

	return true;
}


static int sphQueryHeightCalc ( const XQNode_t * pNode )
{
	if ( !pNode->m_dChildren.GetLength() )
	{
		// exception, pre-cached OR of tiny (rare) keywords is just one node
		if ( pNode->GetOp()==SPH_QUERY_OR )
		{
#ifndef NDEBUG
			// sanity checks
			// this node must be only created for a huge OR of tiny expansions
			assert ( pNode->m_dWords.GetLength() );
			ARRAY_FOREACH ( i, pNode->m_dWords )
			{
				assert ( pNode->m_dWords[i].m_iAtomPos==pNode->m_dWords[0].m_iAtomPos );
				assert ( pNode->m_dWords[i].m_bExpanded );
			}
#endif
			return 1;
		}
		return pNode->m_dWords.GetLength();
	}

	if ( pNode->GetOp()==SPH_QUERY_BEFORE )
		return 1;

	int iMaxChild = 0;
	int iHeight = 0;
	ARRAY_FOREACH ( i, pNode->m_dChildren )
	{
		int iBottom = sphQueryHeightCalc ( pNode->m_dChildren[i] );
		int iTop = pNode->m_dChildren.GetLength()-i-1;
		if ( iBottom+iTop>=iMaxChild+iHeight )
		{
			iMaxChild = iBottom;
			iHeight = iTop;
		}
	}

	return iMaxChild+iHeight;
}
#ifdef __clang__
#ifndef NDEBUG
#define SPH_EXTNODE_STACK_SIZE (0x120)
#else
#define SPH_EXTNODE_STACK_SIZE (160)
#endif
#else
#if _WIN32
#define SPH_EXTNODE_STACK_SIZE (600)
#else
#define SPH_EXTNODE_STACK_SIZE (160)
#endif
#endif

int ConsiderStack ( const struct XQNode_t * pRoot, CSphString & sError )
{
	int iHeight = 0;
	if ( pRoot )
		iHeight = sphQueryHeightCalc ( pRoot );

	auto iStackNeed = iHeight * SPH_EXTNODE_STACK_SIZE;
	int64_t iQueryStack = sphGetStackUsed ()+iStackNeed;
	auto iMyStackSize = sphMyStackSize ();
	if ( iMyStackSize>=iQueryStack )
		return -1;

	// align as stack of tree + 32K
	// (being run in new coro, most probably you'll start near the top of stack, so 32k should be enouth)
	iQueryStack = iStackNeed + 32*1024;
	if ( g_iMaxCoroStackSize>=iQueryStack )
		return (int)iQueryStack;

	sError.SetSprintf ( "query too complex, not enough stack (thread_stack=%dK or higher required)", (int) (( iQueryStack+1024-( iQueryStack % 1024 )) / 1024 ));
	return 0;
}


static XQNode_t * CloneKeyword ( const XQNode_t * pNode )
{
	assert ( pNode );

	XQNode_t * pRes = new XQNode_t ( pNode->m_dSpec );
	pRes->m_dWords = pNode->m_dWords;
	return pRes;
}


static XQNode_t * ExpandKeyword ( XQNode_t * pNode, const CSphIndexSettings & tSettings, int iExpandKeywords, bool bWordDict )
{
	assert ( pNode );

	if ( tSettings.m_bIndexExactWords && ( iExpandKeywords & KWE_MORPH_NONE )==KWE_MORPH_NONE )
	{
		pNode->m_dWords[0].m_sWord.SetSprintf ( "=%s", pNode->m_dWords[0].m_sWord.cstr() );
		return pNode;
	}

	XQNode_t * pExpand = new XQNode_t ( pNode->m_dSpec );
	pExpand->SetOp ( SPH_QUERY_OR, pNode );

	if ( tSettings.m_iMinInfixLen>0 && ( iExpandKeywords & KWE_STAR )==KWE_STAR )
	{
		assert ( pNode->m_dChildren.GetLength()==0 );
		assert ( pNode->m_dWords.GetLength()==1 );
		XQNode_t * pInfix = CloneKeyword ( pNode );
		pInfix->m_dWords[0].m_sWord.SetSprintf ( "*%s*", pNode->m_dWords[0].m_sWord.cstr() );
		pInfix->m_pParent = pExpand;
		pExpand->m_dChildren.Add ( pInfix );
	} else if ( tSettings.GetMinPrefixLen ( bWordDict )>0 && ( iExpandKeywords & KWE_STAR )==KWE_STAR )
	{
		assert ( pNode->m_dChildren.GetLength()==0 );
		assert ( pNode->m_dWords.GetLength()==1 );
		XQNode_t * pPrefix = CloneKeyword ( pNode );
		pPrefix->m_dWords[0].m_sWord.SetSprintf ( "%s*", pNode->m_dWords[0].m_sWord.cstr() );
		pPrefix->m_pParent = pExpand;
		pExpand->m_dChildren.Add ( pPrefix );
	}

	if ( tSettings.m_bIndexExactWords && ( iExpandKeywords & KWE_EXACT )==KWE_EXACT )
	{
		assert ( pNode->m_dChildren.GetLength()==0 );
		assert ( pNode->m_dWords.GetLength()==1 );
		XQNode_t * pExact = CloneKeyword ( pNode );
		pExact->m_dWords[0].m_sWord.SetSprintf ( "=%s", pNode->m_dWords[0].m_sWord.cstr() );
		pExact->m_pParent = pExpand;
		pExpand->m_dChildren.Add ( pExact );
	}

	return pExpand;
}

void sphQueryExpandKeywords ( XQNode_t ** ppNode, const CSphIndexSettings & tSettings, int iExpandKeywords, bool bWordDict )
{
	assert ( ppNode );
	assert ( *ppNode );
	auto& pNode = *ppNode;
	// only if expansion makes sense at all
	assert ( tSettings.m_iMinInfixLen>0 || tSettings.GetMinPrefixLen ( bWordDict )>0 || tSettings.m_bIndexExactWords );
	assert ( iExpandKeywords!=KWE_DISABLED );

	// process children for composite nodes
	if ( pNode->m_dChildren.GetLength() )
	{
		ARRAY_FOREACH ( i, pNode->m_dChildren )
		{
			sphQueryExpandKeywords ( &pNode->m_dChildren[i], tSettings, iExpandKeywords, bWordDict );
			pNode->m_dChildren[i]->m_pParent = pNode;
		}
		return;
	}

	// if that's a phrase/proximity node, create a very special, magic phrase/proximity node
	if ( pNode->GetOp()==SPH_QUERY_PHRASE || pNode->GetOp()==SPH_QUERY_PROXIMITY || pNode->GetOp()==SPH_QUERY_QUORUM )
	{
		assert ( pNode->m_dWords.GetLength()>1 );
		ARRAY_FOREACH ( i, pNode->m_dWords )
		{
			auto * pWord = new XQNode_t ( pNode->m_dSpec );
			pWord->m_dWords.Add ( pNode->m_dWords[i] );
			pNode->m_dChildren.Add ( ExpandKeyword ( pWord, tSettings, iExpandKeywords, bWordDict ) );
			pNode->m_dChildren.Last()->m_iAtomPos = pNode->m_dWords[i].m_iAtomPos;
			pNode->m_dChildren.Last()->m_pParent = pNode;
		}
		pNode->m_dWords.Reset();
		pNode->m_bVirtuallyPlain = true;
		return;
	}

	// skip empty plain nodes
	if ( pNode->m_dWords.GetLength()<=0 )
		return;

	// process keywords for plain nodes
	assert ( pNode->m_dWords.GetLength()==1 );

	XQKeyword_t & tKeyword = pNode->m_dWords[0];
	if ( tKeyword.m_sWord.Begins("=")
		|| tKeyword.m_sWord.Begins("*")
		|| tKeyword.m_sWord.Ends("*") )
		return;

	// do the expansion
	pNode = ExpandKeyword ( pNode, tSettings, iExpandKeywords, bWordDict );
}


// transform the "one two three"/1 quorum into one|two|three (~40% faster)
static void TransformQuorum ( XQNode_t ** ppNode )
{
	XQNode_t *& pNode = *ppNode;

	// recurse non-quorum nodes
	if ( pNode->GetOp()!=SPH_QUERY_QUORUM )
	{
		ARRAY_FOREACH ( i, pNode->m_dChildren )
			TransformQuorum ( &pNode->m_dChildren[i] );
		return;
	}

	// skip quorums with thresholds other than 1
	if ( pNode->m_iOpArg!=1 )
		return;

	// transform quorums with a threshold of 1 only
	assert ( pNode->GetOp()==SPH_QUERY_QUORUM && pNode->m_dChildren.GetLength()==0 );
	CSphVector<XQNode_t*> dArgs;
	ARRAY_FOREACH ( i, pNode->m_dWords )
	{
		XQNode_t * pAnd = new XQNode_t ( pNode->m_dSpec );
		pAnd->m_dWords.Add ( pNode->m_dWords[i] );
		dArgs.Add ( pAnd );
	}

	pNode->m_dWords.Reset();
	pNode->SetOp ( SPH_QUERY_OR, dArgs );
}


struct BinaryNode_t
{
	int m_iLo;
	int m_iHi;
};

static void BuildExpandedTree ( const XQKeyword_t & tRootWord, ISphWordlist::Args_t & dWordSrc, XQNode_t * pRoot )
{
	assert ( dWordSrc.m_dExpanded.GetLength() );
	pRoot->m_dWords.Reset();

	// build a binary tree from all the other expansions
	CSphVector<BinaryNode_t> dNodes;
	dNodes.Reserve ( dWordSrc.m_dExpanded.GetLength() );

	XQNode_t * pCur = pRoot;

	dNodes.Add();
	dNodes.Last().m_iLo = 0;
	dNodes.Last().m_iHi = ( dWordSrc.m_dExpanded.GetLength()-1 );

	while ( dNodes.GetLength() )
	{
		BinaryNode_t tNode = dNodes.Pop();
		if ( tNode.m_iHi<tNode.m_iLo )
		{
			pCur = pCur->m_pParent;
			continue;
		}

		int iMid = ( tNode.m_iLo+tNode.m_iHi ) / 2;
		dNodes.Add ();
		dNodes.Last().m_iLo = tNode.m_iLo;
		dNodes.Last().m_iHi = iMid-1;
		dNodes.Add ();
		dNodes.Last().m_iLo = iMid+1;
		dNodes.Last().m_iHi = tNode.m_iHi;

		if ( pCur->m_dWords.GetLength() )
		{
			assert ( pCur->m_dWords.GetLength()==1 );
			XQNode_t * pTerm = CloneKeyword ( pRoot );
			Swap ( pTerm->m_dWords, pCur->m_dWords );
			pCur->m_dChildren.Add ( pTerm );
			pTerm->m_pParent = pCur;
		}

		XQNode_t * pChild = CloneKeyword ( pRoot );
		pChild->m_dWords.Add ( tRootWord );
		pChild->m_dWords.Last().m_sWord = dWordSrc.GetWordExpanded ( iMid );
		pChild->m_dWords.Last().m_bExpanded = true;
		pChild->m_bNotWeighted = pRoot->m_bNotWeighted;

		pChild->m_pParent = pCur;
		pCur->m_dChildren.Add ( pChild );
		pCur->SetOp ( SPH_QUERY_OR );

		pCur = pChild;
	}
}


/// do wildcard expansion for keywords dictionary
/// (including prefix and infix expansion)
XQNode_t * sphExpandXQNode ( XQNode_t * pNode, ExpansionContext_t & tCtx )
{
	assert ( pNode );

	// process children for composite nodes
	if ( pNode->m_dChildren.GetLength() )
	{
		ARRAY_FOREACH ( i, pNode->m_dChildren )
		{
			pNode->m_dChildren[i] = sphExpandXQNode ( pNode->m_dChildren[i], tCtx );
			pNode->m_dChildren[i]->m_pParent = pNode;
		}
		return pNode;
	}

	// if that's a phrase/proximity node, create a very special, magic phrase/proximity node
	if ( pNode->GetOp()==SPH_QUERY_PHRASE || pNode->GetOp()==SPH_QUERY_PROXIMITY || pNode->GetOp()==SPH_QUERY_QUORUM )
	{
		assert ( pNode->m_dWords.GetLength()>1 );
		ARRAY_FOREACH ( i, pNode->m_dWords )
		{
			XQNode_t * pWord = new XQNode_t ( pNode->m_dSpec );
			pWord->m_dWords.Add ( pNode->m_dWords[i] );
			pNode->m_dChildren.Add ( sphExpandXQNode ( pWord, tCtx ) );
			pNode->m_dChildren.Last()->m_iAtomPos = pNode->m_dWords[i].m_iAtomPos;
			pNode->m_dChildren.Last()->m_pParent = pNode;

			// tricky part
			// current node may have field/zone limits attached
			// normally those get pushed down during query parsing
			// but here we create nodes manually and have to push down limits too
			pWord->CopySpecs ( pNode );
		}
		pNode->m_dWords.Reset();
		pNode->m_bVirtuallyPlain = true;
		return pNode;
	}

	// skip empty plain nodes
	if ( pNode->m_dWords.GetLength()<=0 )
		return pNode;

	// process keywords for plain nodes
	assert ( pNode->m_dChildren.GetLength()==0 );
	assert ( pNode->m_dWords.GetLength()==1 );

	// might be a pass to only fixup of the tree
	if ( tCtx.m_bOnlyTreeFix )
		return pNode;

	assert ( tCtx.m_pResult );

	// check the wildcards
	const char * sFull = pNode->m_dWords[0].m_sWord.cstr();

	// no wildcards, or just wildcards? do not expand
	if ( !sphHasExpandableWildcards ( sFull ) )
		return pNode;

	bool bUseTermMerge = ( tCtx.m_bMergeSingles && pNode->m_dSpec.m_dZones.IsEmpty() );
	ISphWordlist::Args_t tWordlist ( bUseTermMerge, tCtx.m_iExpansionLimit, tCtx.m_bHasExactForms, tCtx.m_eHitless, tCtx.m_pIndexData );

	if ( !sphExpandGetWords ( sFull, tCtx, tWordlist ) )
	{
		tCtx.m_pResult->m_sWarning.SetSprintf ( "Query word length is less than min %s length. word: '%s' ", ( tCtx.m_iMinInfixLen>0 ? "infix" : "prefix" ), sFull );
		return pNode;
	}

	// no real expansions?
	// mark source word as expanded to prevent warning on terms mismatch in statistics
	if ( !tWordlist.m_dExpanded.GetLength() && !tWordlist.m_pPayload )
	{
		tCtx.m_pResult->AddStat ( pNode->m_dWords.Begin()->m_sWord, 0, 0 );
		pNode->m_dWords.Begin()->m_bExpanded = true;
		return pNode;
	}

	// copy the original word (iirc it might get overwritten),
	const XQKeyword_t tRootWord = pNode->m_dWords[0];
	tCtx.m_pResult->AddStat ( tRootWord.m_sWord, tWordlist.m_iTotalDocs, tWordlist.m_iTotalHits );

	// and build a binary tree of all the expansions
	if ( tWordlist.m_dExpanded.GetLength() )
	{
		BuildExpandedTree ( tRootWord, tWordlist, pNode );
	}

	if ( tWordlist.m_pPayload )
	{
		ISphSubstringPayload * pPayload = tWordlist.m_pPayload;
		tWordlist.m_pPayload = NULL;
		tCtx.m_pPayloads->Add ( pPayload );

		if ( pNode->m_dWords.GetLength() )
		{
			// all expanded fit to single payload
			pNode->m_dWords.Begin()->m_bExpanded = true;
			pNode->m_dWords.Begin()->m_pPayload = pPayload;
		} else
		{
			// payload added to expanded binary tree
			assert ( pNode->GetOp()==SPH_QUERY_OR );
			assert ( pNode->m_dChildren.GetLength() );

			XQNode_t * pSubstringNode = new XQNode_t ( pNode->m_dSpec );
			pSubstringNode->SetOp ( SPH_QUERY_OR );

			XQKeyword_t tSubstringWord = tRootWord;
			tSubstringWord.m_bExpanded = true;
			tSubstringWord.m_pPayload = pPayload;
			pSubstringNode->m_dWords.Add ( tSubstringWord );

			pNode->m_dChildren.Add ( pSubstringNode );
			pSubstringNode->m_pParent = pNode;
		}
	}

	return pNode;
}


bool sphHasExpandableWildcards ( const char * sWord )
{
	const char * pCur = sWord;
	int iWilds = 0;

	for ( ; *pCur; pCur++ )
		if ( sphIsWild ( *pCur ) )
			iWilds++;

	int iLen = int ( pCur - sWord );
	return ( iWilds && iWilds<iLen );
}

bool sphExpandGetWords ( const char * sWord, const ExpansionContext_t & tCtx, ISphWordlist::Args_t & tWordlist )
{
	// fix for the case '=*term' that should count as infix
	if ( sWord[0]=='=' && sWord[1]=='*' )
		sWord++;

	if ( !sphIsWild ( *sWord ) || tCtx.m_iMinInfixLen==0 )
	{
		// do prefix expansion
		// remove exact form modifier, if any
		const char * sPrefix = sWord;
		if ( *sPrefix=='=' )
			sPrefix++;

		// skip leading wildcards
		// (in case we got here on non-infixed index path)
		const char * sWildcard = sPrefix;
		while ( sphIsWild ( *sPrefix ) )
		{
			sPrefix++;
			sWildcard++;
		}

		// compute non-wildcard prefix length
		int iPrefix = 0;
		const char * sCodes = sPrefix;
		for ( ; *sCodes && !sphIsWild ( *sCodes ); sCodes+=sphUtf8CharBytes ( *sCodes ) )
			iPrefix++;

		// do not expand prefixes under min length
		if ( iPrefix<tCtx.m_iMinPrefixLen )
			return false;

		int iBytes = int ( sCodes - sPrefix );
		// prefix expansion should work on nonstemmed words only
		char sFixed[MAX_KEYWORD_BYTES];
		if ( tCtx.m_bHasExactForms )
		{
			sFixed[0] = MAGIC_WORD_HEAD_NONSTEMMED;
			memcpy ( sFixed+1, sPrefix, iBytes );
			sPrefix = sFixed;
			iBytes++;
		}

		tCtx.m_pWordlist->GetPrefixedWords ( sPrefix, iBytes, sWildcard, tWordlist );

	} else
	{
		// do infix expansion
		assert ( sphIsWild ( *sWord ) );
		assert ( tCtx.m_iMinInfixLen>0 );

		// find the longest substring of non-wildcards
		int iCodepoints = 0;
		int iInfixCodepoints = 0;
		int iInfixBytes = 0;
		const char * sMaxInfix = NULL;
		const char * sInfix = sWord;

		for ( const char * s = sWord; *s; )
		{
			int iCodeLen = sphUtf8CharBytes ( *s );

			if ( sphIsWild ( *s ) )
			{
				sInfix = s + 1;
				iCodepoints = 0;
			} else
			{
				iCodepoints++;
				if ( s - sInfix + iCodeLen > iInfixBytes )
				{
					sMaxInfix = sInfix;
					iInfixBytes = int ( s - sInfix ) + iCodeLen;
					iInfixCodepoints = iCodepoints;
				}
			}

			s += iCodeLen;
		}

		// do not expand infixes under min_infix_len
		if ( iInfixCodepoints < tCtx.m_iMinInfixLen )
			return false;

		// ignore heading star
		tCtx.m_pWordlist->GetInfixedWords ( sMaxInfix, iInfixBytes, sWord, tWordlist );
	}

	return true;
}

XQNode_t * CSphIndex_VLN::ExpandPrefix ( XQNode_t * pNode, CSphQueryResultMeta & tMeta, CSphScopedPayload * pPayloads, DWORD uQueryDebugFlags ) const
{
	if ( !pNode || !m_pDict->GetSettings().m_bWordDict
			|| ( m_tSettings.GetMinPrefixLen ( m_pDict->GetSettings().m_bWordDict )<=0 && m_tSettings.m_iMinInfixLen<=0 ) )
		return pNode;

	assert ( m_bPassedAlloc );
	assert ( !m_tWordlist.m_tBuf.IsEmpty() );

	ExpansionContext_t tCtx;
	tCtx.m_pWordlist = &m_tWordlist;
	tCtx.m_pResult = &tMeta;
	tCtx.m_iMinPrefixLen = m_tSettings.GetMinPrefixLen ( m_pDict->GetSettings().m_bWordDict );
	tCtx.m_iMinInfixLen = m_tSettings.m_iMinInfixLen;
	tCtx.m_iExpansionLimit = m_iExpansionLimit;
	tCtx.m_bHasExactForms = ( m_pDict->HasMorphology() || m_tSettings.m_bIndexExactWords );
	tCtx.m_bMergeSingles = ( uQueryDebugFlags & QUERY_DEBUG_NO_PAYLOAD )==0;
	tCtx.m_pPayloads = pPayloads;
	tCtx.m_eHitless = m_tSettings.m_eHitless;

	pNode = sphExpandXQNode ( pNode, tCtx );
	pNode->Check ( true );

	return pNode;
}


// transform the (A B) NEAR C into A NEAR B NEAR C
static void TransformNear ( XQNode_t ** ppNode )
{
	XQNode_t *& pNode = *ppNode;
	if ( pNode->GetOp()==SPH_QUERY_NEAR )
	{
		assert ( pNode->m_dWords.GetLength()==0 );
		CSphVector<XQNode_t*> dArgs;
		int iStartFrom;

		// transform all (A B C) NEAR D into A NEAR B NEAR C NEAR D
		do
		{
			dArgs.Reset();
			iStartFrom = 0;
			ARRAY_FOREACH ( i, pNode->m_dChildren )
			{
				XQNode_t * pChild = pNode->m_dChildren[i]; ///< shortcut
				if ( pChild->GetOp()==SPH_QUERY_AND && pChild->m_dChildren.GetLength()>0 )
				{
					ARRAY_FOREACH ( j, pChild->m_dChildren )
					{
						if ( j==0 && iStartFrom==0 )
						{
							// we will remove the node anyway, so just replace it with 1-st child instead
							pNode->m_dChildren[i] = pChild->m_dChildren[j];
							pNode->m_dChildren[i]->m_pParent = pNode;
							iStartFrom = i+1;
						} else
						{
							dArgs.Add ( pChild->m_dChildren[j] );
						}
					}
					pChild->m_dChildren.Reset();
					SafeDelete ( pChild );
				} else if ( iStartFrom!=0 )
				{
					dArgs.Add ( pChild );
				}
			}

			if ( iStartFrom!=0 )
			{
				pNode->m_dChildren.Resize ( iStartFrom + dArgs.GetLength() );
				ARRAY_FOREACH ( i, dArgs )
				{
					pNode->m_dChildren [ i + iStartFrom ] = dArgs[i];
					pNode->m_dChildren [ i + iStartFrom ]->m_pParent = pNode;
				}
			}
		} while ( iStartFrom!=0 );
	}

	ARRAY_FOREACH ( i, pNode->m_dChildren )
		TransformNear ( &pNode->m_dChildren[i] );
}


/// tag excluded keywords (rvals to operator NOT)
static void TagExcluded ( XQNode_t * pNode, bool bNot )
{
	if ( pNode->GetOp()==SPH_QUERY_ANDNOT )
	{
		assert ( pNode->m_dChildren.GetLength()==2 );
		assert ( pNode->m_dWords.GetLength()==0 );
		TagExcluded ( pNode->m_dChildren[0], bNot );
		TagExcluded ( pNode->m_dChildren[1], !bNot );

	} else if ( pNode->m_dChildren.GetLength() )
	{
		// FIXME? check if this works okay with "virtually plain" stuff?
		ARRAY_FOREACH ( i, pNode->m_dChildren )
			TagExcluded ( pNode->m_dChildren[i], bNot );
	} else
	{
		// tricky bit
		// no assert on length here and that is intended
		// we have fully empty nodes (0 children, 0 words) sometimes!
		ARRAY_FOREACH ( i, pNode->m_dWords )
			pNode->m_dWords[i].m_bExcluded = bNot;
	}
}


/// optimize phrase queries if we have bigrams
static void TransformBigrams ( XQNode_t * pNode, const CSphIndexSettings & tSettings )
{
	assert ( tSettings.m_eBigramIndex!=SPH_BIGRAM_NONE );
	assert ( tSettings.m_eBigramIndex==SPH_BIGRAM_ALL || tSettings.m_dBigramWords.GetLength() );

	if ( pNode->GetOp()!=SPH_QUERY_PHRASE )
	{
		ARRAY_FOREACH ( i, pNode->m_dChildren )
			TransformBigrams ( pNode->m_dChildren[i], tSettings );
		return;
	}

	CSphBitvec bmRemove;
	bmRemove.Init ( pNode->m_dWords.GetLength() );

	for ( int i=0; i<pNode->m_dWords.GetLength()-1; i++ )
	{
		// check whether this pair was indexed
		bool bBigram = false;
		switch ( tSettings.m_eBigramIndex )
		{
			case SPH_BIGRAM_NONE:
				break;
			case SPH_BIGRAM_ALL:
				bBigram = true;
				break;
			case SPH_BIGRAM_FIRSTFREQ:
				bBigram = tSettings.m_dBigramWords.BinarySearch ( pNode->m_dWords[i].m_sWord )!=NULL;
				break;
			case SPH_BIGRAM_BOTHFREQ:
				bBigram =
					( tSettings.m_dBigramWords.BinarySearch ( pNode->m_dWords[i].m_sWord )!=NULL ) &&
					( tSettings.m_dBigramWords.BinarySearch ( pNode->m_dWords[i+1].m_sWord )!=NULL );
				break;
		}
		if ( !bBigram )
			continue;

		// replace the pair with a bigram keyword
		// FIXME!!! set phrase weight for this "word" here
		pNode->m_dWords[i].m_sWord.SetSprintf ( "%s%c%s",
			pNode->m_dWords[i].m_sWord.cstr(),
			MAGIC_WORD_BIGRAM,
			pNode->m_dWords[i+1].m_sWord.cstr() );

		// only mark for removal now, we will sweep later
		// so that [a b c] would convert to ["a b" "b c"], not just ["a b" c]
		bmRemove.BitClear ( i );
		bmRemove.BitSet ( i+1 );
	}

	// remove marked words
	int iOut = 0;
	ARRAY_FOREACH ( i, pNode->m_dWords )
		if ( !bmRemove.BitGet(i) )
			pNode->m_dWords[iOut++] = pNode->m_dWords[i];
	pNode->m_dWords.Resize ( iOut );

	// fixup nodes that are not real phrases any more
	if ( pNode->m_dWords.GetLength()==1 )
		pNode->SetOp ( SPH_QUERY_AND );
}


/// create a node from a set of lemmas
/// WARNING, tKeyword might or might not be pointing to pNode->m_dWords[0]
/// Called from the daemon side (searchd) in time of query
static void TransformAotFilterKeyword ( XQNode_t * pNode, LemmatizerTrait_i * pLemmatizer, const XQKeyword_t & tKeyword, const CSphWordforms * pWordforms, const CSphIndexSettings & tSettings )
{
	assert ( pNode->m_dWords.GetLength()<=1 );
	assert ( pNode->m_dChildren.GetLength()==0 );

	XQNode_t * pExact = NULL;
	if ( pWordforms )
	{
		// do a copy, because patching in place is not an option
		// short => longlonglong wordform mapping would crash
		// OPTIMIZE? forms that are not found will (?) get looked up again in the dict
		char sBuf [ MAX_KEYWORD_BYTES ];
		strncpy ( sBuf, tKeyword.m_sWord.cstr(), sizeof(sBuf)-1 );
		if ( pWordforms->ToNormalForm ( (BYTE*)sBuf, true, false ) )
		{
			if ( !pNode->m_dWords.GetLength() )
				pNode->m_dWords.Add ( tKeyword );
			pNode->m_dWords[0].m_sWord = sBuf;
			pNode->m_dWords[0].m_bMorphed = true;
			return;
		}
	}

	StrVec_t dLemmas;
	DWORD uLangMask = tSettings.m_uAotFilterMask;
	for ( int i=AOT_BEGIN; i<AOT_LENGTH; ++i )
	{
		if ( uLangMask & (1UL<<i) )
		{
			if ( i==AOT_RU )
				sphAotLemmatizeRu ( dLemmas, (const BYTE*)tKeyword.m_sWord.cstr() );
			else if ( i==AOT_DE )
				sphAotLemmatizeDe ( dLemmas, (const BYTE*)tKeyword.m_sWord.cstr() );
			else if ( i==AOT_UK )
				sphAotLemmatizeUk ( dLemmas, (const BYTE*)tKeyword.m_sWord.cstr(), pLemmatizer );
			else
				sphAotLemmatize ( dLemmas, (const BYTE*)tKeyword.m_sWord.cstr(), i );
		}
	}

	// post-morph wordforms
	if ( pWordforms && pWordforms->m_bHavePostMorphNF )
	{
		char sBuf [ MAX_KEYWORD_BYTES ];
		ARRAY_FOREACH ( i, dLemmas )
		{
			strncpy ( sBuf, dLemmas[i].cstr(), sizeof(sBuf)-1 );
			if ( pWordforms->ToNormalForm ( (BYTE*)sBuf, false, false ) )
				dLemmas[i] = sBuf;
		}
	}

	if ( dLemmas.GetLength() && tSettings.m_bIndexExactWords )
	{
		pExact = CloneKeyword ( pNode );
		if ( !pExact->m_dWords.GetLength() )
			pExact->m_dWords.Add ( tKeyword );

		pExact->m_dWords[0].m_sWord.SetSprintf ( "=%s", tKeyword.m_sWord.cstr() );
		pExact->m_pParent = pNode;
	}

	if ( !pExact && dLemmas.GetLength()<=1 )
	{
		// zero or one lemmas, update node in-place
		if ( !pNode->m_dWords.GetLength() )
			pNode->m_dWords.Add ( tKeyword );
		if ( dLemmas.GetLength() )
		{
			pNode->m_dWords[0].m_sWord = dLemmas[0];
			pNode->m_dWords[0].m_bMorphed = true;
		}
	} else
	{
		// multiple lemmas, create an OR node
		pNode->SetOp ( SPH_QUERY_OR );
		ARRAY_FOREACH ( i, dLemmas )
		{
			pNode->m_dChildren.Add ( new XQNode_t ( pNode->m_dSpec ) );
			pNode->m_dChildren.Last()->m_pParent = pNode;
			XQKeyword_t & tLemma = pNode->m_dChildren.Last()->m_dWords.Add();
			tLemma.m_sWord = dLemmas[i];
			tLemma.m_iAtomPos = tKeyword.m_iAtomPos;
			tLemma.m_bFieldStart = tKeyword.m_bFieldStart;
			tLemma.m_bFieldEnd = tKeyword.m_bFieldEnd;
			tLemma.m_bMorphed = true;
		}
		pNode->m_dWords.Reset();
		if ( pExact )
			pNode->m_dChildren.Add ( pExact );
	}
}


/// AOT morph guesses transform
/// replaces tokens with their respective morph guesses subtrees
/// used in lemmatize_ru_all morphology processing mode that can generate multiple guesses
/// in other modes, there is always exactly one morph guess, and the dictionary handles it
/// Called from the daemon side (searchd)
void TransformAotFilter ( XQNode_t * pNode, LemmatizerTrait_i * pLemmatizer, const CSphWordforms * pWordforms, const CSphIndexSettings & tSettings )
{
	if ( !pNode )
		return;
	// case one, regular operator (and empty nodes)
	ARRAY_FOREACH ( i, pNode->m_dChildren )
		TransformAotFilter ( pNode->m_dChildren[i], pLemmatizer, pWordforms, tSettings );
	if ( pNode->m_dChildren.GetLength() || pNode->m_dWords.GetLength()==0 )
		return;

	// case two, operator on a bag of words
	// FIXME? check phrase vs expand_keywords vs lemmatize_ru_all?
	if ( pNode->m_dWords.GetLength()
		&& ( pNode->GetOp()==SPH_QUERY_PHRASE || pNode->GetOp()==SPH_QUERY_PROXIMITY || pNode->GetOp()==SPH_QUERY_QUORUM ) )
	{
		assert ( pNode->m_dWords.GetLength() );

		ARRAY_FOREACH ( i, pNode->m_dWords )
		{
			XQNode_t * pNew = new XQNode_t ( pNode->m_dSpec );
			pNew->m_pParent = pNode;
			pNew->m_iAtomPos = pNode->m_dWords[i].m_iAtomPos;
			pNode->m_dChildren.Add ( pNew );
			TransformAotFilterKeyword ( pNew, pLemmatizer, pNode->m_dWords[i], pWordforms, tSettings );
		}

		pNode->m_dWords.Reset();
		pNode->m_bVirtuallyPlain = true;
		return;
	}

	// case three, plain old single keyword
	assert ( pNode->m_dWords.GetLength()==1 );
	TransformAotFilterKeyword ( pNode, pLemmatizer, pNode->m_dWords[0], pWordforms, tSettings );
}

void TransformAotFilter ( XQNode_t * pNode, const CSphWordforms * pWordforms, const CSphIndexSettings & tSettings )
{
	if ( !tSettings.m_uAotFilterMask )
		return;

	int iAotLang = ( tSettings.m_uAotFilterMask & ( 1<<AOT_UK ) ) ? AOT_UK : AOT_LENGTH;
	CSphScopedPtr<LemmatizerTrait_i> tLemmatizer ( CreateLemmatizer ( iAotLang ) );
	TransformAotFilter ( pNode, tLemmatizer.Ptr(), pWordforms, tSettings );
}

void sphTransformExtendedQuery ( XQNode_t ** ppNode, const CSphIndexSettings & tSettings, bool bHasBooleanOptimization, const ISphKeywordsStat * pKeywords )
{
	TransformQuorum ( ppNode );
	( *ppNode )->Check ( true );
	TransformNear ( ppNode );
	( *ppNode )->Check ( true );
	if ( tSettings.m_eBigramIndex!=SPH_BIGRAM_NONE )
		TransformBigrams ( *ppNode, tSettings );
	TagExcluded ( *ppNode, false );
	( *ppNode )->Check ( true );

	// boolean optimization
	if ( bHasBooleanOptimization )
		sphOptimizeBoolean ( ppNode, pKeywords );
}


static void SetupSplitFilter ( CSphFilterSettings & tFilter, int iPart, int iTotal )
{
	tFilter.m_eType = SPH_FILTER_RANGE;
	tFilter.m_sAttrName = "@rowid";
	tFilter.m_iMinValue = iPart;
	tFilter.m_iMaxValue = iTotal;
}

// basically the same code as QueryDiskChunks in an RT index
static bool RunSplitQuery ( const CSphIndex * pIndex, const CSphQuery & tQuery, CSphQueryResultMeta & tResult, VecTraits_T<ISphMatchSorter *> & dSorters, const CSphMultiQueryArgs & tArgs, QueryProfile_c * pProfiler, const SmallStringHash_T<int64_t> * pLocalDocs, int64_t iTotalDocs, const char * szIndexName, int iSplit, int64_t tmMaxTimer )
{
	assert ( !dSorters.IsEmpty () );

	// counter of tasks we will issue now
	int iJobs = iSplit;
	assert ( iJobs>=1 );

	Threads::ClonableCtx_T<DiskChunkSearcherCtx_t, DiskChunkSearcherCloneCtx_t> tClonableCtx { dSorters, tResult };

	auto iConcurrency = tQuery.m_iCouncurrency;
	if ( !iConcurrency )
		iConcurrency = GetEffectiveDistThreads();

	tClonableCtx.LimitConcurrency ( iConcurrency );

	auto iStart = sphMicroTimer();
	sphLogDebugv ( "Started: " INT64_FMT, sphMicroTimer()-iStart );

	// because disk chunk search within the loop will switch the profiler state
	SwitchProfile ( pProfiler, SPH_QSTATE_INIT );

	std::atomic<bool> bInterrupt {false};
	std::atomic<int32_t> iCurChunk { 0 };
	Threads::Coro::ExecuteN ( tClonableCtx.Concurrency ( iJobs ), [&]
	{
		auto iChunk = iCurChunk.fetch_add ( 1, std::memory_order_acq_rel );
		if ( iChunk>=iJobs || bInterrupt )
			return; // already nothing to do, early finish.

		auto tCtx = tClonableCtx.CloneNewContext ( &iChunk );
		Threads::Coro::Throttler_c tThrottler ( session::GetThrottlingPeriodMS() );
		int iTick=1; // num of times coro rescheduled by throttler
		while ( !bInterrupt ) // some earlier job met error; abort.
		{
			myinfo::SetThreadInfo ( "%d ch %d:", iTick, iChunk );
			auto & dLocalSorters = tCtx.m_dSorters;
			CSphQueryResultMeta tChunkMeta;
			CSphQueryResult tChunkResult;
			tChunkResult.m_pMeta = &tChunkMeta;
			CSphQueryResultMeta & tThMeta = tCtx.m_tMeta;
			tChunkMeta.m_pProfile = tThMeta.m_pProfile;

			CSphMultiQueryArgs tMultiArgs ( tArgs.m_iIndexWeight );
			tMultiArgs.m_iTag = tArgs.m_bModifySorterSchemas ? iChunk+1 : tArgs.m_iTag;
			tMultiArgs.m_uPackedFactorFlags = tArgs.m_uPackedFactorFlags;
			tMultiArgs.m_pLocalDocs = pLocalDocs;
			tMultiArgs.m_iTotalDocs = iTotalDocs;
			tMultiArgs.m_bModifySorterSchemas = false;

			CSphQuery tQueryWithExtraFilter = tQuery;
			SetupSplitFilter ( tQueryWithExtraFilter.m_dFilters.Add(), iChunk, iJobs );
			bInterrupt = !pIndex->MultiQuery ( tChunkResult, tQueryWithExtraFilter, dLocalSorters, tMultiArgs );

			if ( !iChunk )
				tThMeta.MergeWordStats ( tChunkMeta );

			tThMeta.m_bHasPrediction |= tChunkMeta.m_bHasPrediction;

			if ( tThMeta.m_bHasPrediction )
				tThMeta.m_tStats.Add ( tChunkMeta.m_tStats );

			if ( iChunk < iJobs-1 && tmMaxTimer>0 && sph::TimeExceeded ( tmMaxTimer ) )
			{
				tThMeta.m_sWarning = "query time exceeded max_query_time";
				bInterrupt = true;
			}

			if ( tThMeta.m_sWarning.IsEmpty() && !tChunkMeta.m_sWarning.IsEmpty() )
				tThMeta.m_sWarning = tChunkMeta.m_sWarning;

			if ( bInterrupt && !tChunkMeta.m_sError.IsEmpty() )
				// FIXME? maybe handle this more gracefully (convert to a warning)?
				tThMeta.m_sError = tChunkMeta.m_sError;

			iChunk = iCurChunk.fetch_add ( 1, std::memory_order_acq_rel );
			if ( iChunk>=iJobs || bInterrupt )
				return; // all is done

			// yield and reschedule every quant of time. It gives work to other tasks
			if ( tThrottler.ThrottleAndKeepCrashQuery() )
				// report current disk chunk processing
				++iTick;
		}
	});

	tClonableCtx.Finalize();
	return !bInterrupt;
}


bool CSphIndex_VLN::SplitQuery ( CSphQueryResult & tResult, const CSphQuery & tQuery, const VecTraits_T<ISphMatchSorter *> & dAllSorters, const CSphMultiQueryArgs & tArgs, int64_t tmMaxTimer ) const
{
	auto & tMeta = *tResult.m_pMeta;
	QueryProfile_c * pProfile = tMeta.m_pProfile;

	CSphVector<ISphMatchSorter*> dSorters;
	dSorters.Reserve ( dAllSorters.GetLength() );
	dAllSorters.Apply ([&dSorters] ( ISphMatchSorter* p ) { if ( p ) dSorters.Add(p); });

	int iSplit = Max ( Min ( (int)m_tStats.m_iTotalDocuments, tArgs.m_iSplit ), 1 );
	int64_t iTotalDocs = tArgs.m_iTotalDocs ? tArgs.m_iTotalDocs : m_tStats.m_iTotalDocuments;
	bool bOk = RunSplitQuery ( this, tQuery, *tResult.m_pMeta, dSorters, tArgs, pProfile, tArgs.m_pLocalDocs, iTotalDocs, m_sIndexName.cstr(), iSplit, tmMaxTimer );
	if ( !bOk )
		return false;

	tResult.m_pBlobPool = m_tBlobAttrs.GetWritePtr();
	tResult.m_pDocstore = m_pDocstore.Ptr() ? this : nullptr;
	tResult.m_pColumnar = m_pColumnar.Ptr();

	if ( tArgs.m_bModifySorterSchemas )
	{
		SwitchProfile ( pProfile, SPH_QSTATE_DYNAMIC );
		PooledAttrsToPtrAttrs ( dSorters, m_tBlobAttrs.GetWritePtr(), m_pColumnar.Ptr(), tArgs.m_bFinalizeSorters );
	}

	return true;
}

/// one regular query vs many sorters (like facets, or similar for common-tree optimization)
bool CSphIndex_VLN::MultiQuery ( CSphQueryResult & tResult, const CSphQuery & tQuery, const VecTraits_T<ISphMatchSorter *> & dAllSorters, const CSphMultiQueryArgs & tArgs ) const
{
	auto & tMeta = *tResult.m_pMeta;
	QueryProfile_c * pProfile = tMeta.m_pProfile;

	int64_t tmMaxTimer = 0;
	sph::MiniTimer_c dTimerGuard;
	if ( tQuery.m_uMaxQueryMsec>0 )
		tmMaxTimer = dTimerGuard.MiniTimerEngage ( tQuery.m_uMaxQueryMsec ); // max_query_time

	const QueryParser_i * pQueryParser = tQuery.m_pQueryParser;
	assert ( pQueryParser );

	if ( tArgs.m_iSplit>1 )
		return SplitQuery ( tResult, tQuery, dAllSorters, tArgs, tmMaxTimer );

	MEMORY ( MEM_DISK_QUERY );

	// to avoid the checking of a ppSorters's element for NULL on every next step, just filter out all nulls right here
	CSphVector<ISphMatchSorter*> dSorters;
	dSorters.Reserve ( dAllSorters.GetLength() );
	dAllSorters.Apply ([&dSorters] ( ISphMatchSorter* p) { if ( p ) dSorters.Add(p); });

	// if we have anything to work with
	if ( dSorters.IsEmpty () )
		return false;

	// non-random at the start, random at the end
	dSorters.Sort ( CmpPSortersByRandom_fn() );

	// fast path for scans
	if ( pQueryParser->IsFullscan ( tQuery ) )
		return MultiScan ( tResult, tQuery, dSorters, tArgs, tmMaxTimer );

	SwitchProfile ( pProfile, SPH_QSTATE_DICT_SETUP );

	DictRefPtr_c pDict { GetStatelessDict ( m_pDict ) };
	SetupStarDict ( pDict );
	SetupExactDict ( pDict );

	CSphVector<BYTE> dFiltered;
	const BYTE * sModifiedQuery = (const BYTE *)tQuery.m_sQuery.cstr();

	FieldFilterRefPtr_c pFieldFilter;
	if ( m_pFieldFilter && sModifiedQuery )
	{
		pFieldFilter = m_pFieldFilter->Clone();
		if ( pFieldFilter && pFieldFilter->Apply ( sModifiedQuery, dFiltered, true ) )
			sModifiedQuery = dFiltered.Begin();
	}

	// parse query
	SwitchProfile ( pProfile, SPH_QSTATE_PARSE );

	XQQuery_t tParsed;
	if ( !pQueryParser->ParseQuery ( tParsed, (const char*)sModifiedQuery, &tQuery, m_pQueryTokenizer, m_pQueryTokenizerJson, &m_tSchema, pDict, m_tSettings ) )
	{
		// FIXME? might wanna reset profile to unknown state
		tMeta.m_sError = tParsed.m_sParseError;
		return false;
	}

	// check again for fullscan
	if ( pQueryParser->IsFullscan ( tParsed ) )
		return MultiScan ( tResult, tQuery, dSorters, tArgs, tmMaxTimer );

	if ( !tParsed.m_sParseWarning.IsEmpty() )
		tMeta.m_sWarning = tParsed.m_sParseWarning;

	// transform query if needed (quorum transform, etc.)
	SwitchProfile ( pProfile, SPH_QSTATE_TRANSFORMS );
	sphTransformExtendedQuery ( &tParsed.m_pRoot, m_tSettings, tQuery.m_bSimplify, this );

	bool bWordDict = pDict->GetSettings().m_bWordDict;
	int iExpandKeywords = ExpandKeywords ( m_tMutableSettings.m_iExpandKeywords, tQuery.m_eExpandKeywords, m_tSettings, bWordDict );
	if ( iExpandKeywords!=KWE_DISABLED )
	{
		sphQueryExpandKeywords ( &tParsed.m_pRoot, m_tSettings, iExpandKeywords, bWordDict );
		tParsed.m_pRoot->Check ( true );
	}

	// this should be after keyword expansion
	TransformAotFilter ( tParsed.m_pRoot, pDict->GetWordforms(), m_tSettings );

	// expanding prefix in word dictionary case
	CSphScopedPayload tPayloads;
	XQNode_t * pPrefixed = ExpandPrefix ( tParsed.m_pRoot, tMeta, &tPayloads, tQuery.m_uDebugFlags );
	if ( !pPrefixed )
		return false;
	tParsed.m_pRoot = pPrefixed;

	auto iStackNeed = ConsiderStack ( tParsed.m_pRoot, tMeta.m_sError );
	if ( !iStackNeed  )
		return false;

	return Threads::Coro::ContinueBool ( iStackNeed, [&] {

	// flag common subtrees
	int iCommonSubtrees = 0;
	if ( m_iMaxCachedDocs && m_iMaxCachedHits )
		iCommonSubtrees = sphMarkCommonSubtrees ( 1, &tParsed );

	tParsed.m_bNeedSZlist = tQuery.m_bZSlist;

	CSphQueryNodeCache tNodeCache ( iCommonSubtrees, m_iMaxCachedDocs, m_iMaxCachedHits );
	bool bResult = ParsedMultiQuery ( tQuery, tResult, dSorters, tParsed, pDict, tArgs, &tNodeCache, tmMaxTimer );

	if ( tArgs.m_bModifySorterSchemas )
	{
		SwitchProfile ( pProfile, SPH_QSTATE_DYNAMIC );
		PooledAttrsToPtrAttrs ( dSorters, m_tBlobAttrs.GetWritePtr(), m_pColumnar.Ptr(), tArgs.m_bFinalizeSorters );
	}

	return bResult;
	});
}


/// many regular queries with one sorter attached to each query.
/// returns true if at least one query succeeded. The failed queries indicated with pResult->m_iMultiplier==-1
bool CSphIndex_VLN::MultiQueryEx ( int iQueries, const CSphQuery * pQueries, CSphQueryResult * pResults, ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs & tArgs ) const
{
	// ensure we have multiple queries
	if ( iQueries==1 )
		return MultiQuery ( pResults[0], pQueries[0], { ppSorters, 1}, tArgs );

	MEMORY ( MEM_DISK_QUERYEX );

	assert ( ppSorters );

	DictRefPtr_c pDict { GetStatelessDict ( m_pDict ) };
	SetupStarDict ( pDict );
	SetupExactDict ( pDict );

	CSphFixedVector<XQQuery_t> dXQ ( iQueries );
	CSphScopedPayload tPayloads;
	bool bResult = false;
	bool bResultScan = false;
	int iStackNeed = 0;
	bool bWordDict = pDict->GetSettings().m_bWordDict;
	for ( int i=0; i<iQueries; ++i )
	{
		const auto & tCurQuery = pQueries[i];
		auto & tCurResult = pResults[i];
		auto & tMeta = *tCurResult.m_pMeta;

		// nothing to do without a sorter
		if ( !ppSorters[i] )
		{
			tMeta.m_iMultiplier = -1; ///< show that this particular query failed
			continue;
		}

		// fast path for scans
		if ( tCurQuery.m_sQuery.IsEmpty() )
		{
			int64_t tmMaxTimer = 0;
			sph::MiniTimer_c tTimerGuard;
			if ( tCurQuery.m_uMaxQueryMsec>0 )
				tmMaxTimer = tTimerGuard.MiniTimerEngage ( tCurQuery.m_uMaxQueryMsec ); // max_query_time

			if ( MultiScan ( tCurResult, tCurQuery, { ppSorters+i, 1 }, tArgs, tmMaxTimer ) )
				bResultScan = true;
			else
				tMeta.m_iMultiplier = -1; ///< show that this particular query failed
			continue;
		}

		tMeta.m_tIOStats.Start();

		// parse query
		const QueryParser_i * pQueryParser = tCurQuery.m_pQueryParser;
		assert ( pQueryParser );

		if ( pQueryParser->ParseQuery ( dXQ[i], tCurQuery.m_sQuery.cstr(), &tCurQuery, m_pQueryTokenizer, m_pQueryTokenizerJson, &m_tSchema, pDict, m_tSettings ) )
		{
			// transform query if needed (quorum transform, keyword expansion, etc.)
			sphTransformExtendedQuery ( &dXQ[i].m_pRoot, m_tSettings, tCurQuery.m_bSimplify, this );

			int iExpandKeywords = ExpandKeywords ( m_tMutableSettings.m_iExpandKeywords, tCurQuery.m_eExpandKeywords, m_tSettings, bWordDict );
			if ( iExpandKeywords!=KWE_DISABLED )
			{
				sphQueryExpandKeywords ( &dXQ[i].m_pRoot, m_tSettings, iExpandKeywords, bWordDict );
				dXQ[i].m_pRoot->Check ( true );
			}

			// this should be after keyword expansion
			TransformAotFilter ( dXQ[i].m_pRoot, pDict->GetWordforms(), m_tSettings );

			// expanding prefix in word dictionary case
			XQNode_t * pPrefixed = ExpandPrefix ( dXQ[i].m_pRoot, tMeta, &tPayloads, tCurQuery.m_uDebugFlags );
			if ( pPrefixed )
			{
				dXQ[i].m_pRoot = pPrefixed;
				iStackNeed = ConsiderStack ( dXQ[i].m_pRoot, tMeta.m_sError );
				if ( iStackNeed!=0 )
				{
					bResult = true;
				} else
				{
					tMeta.m_iMultiplier = -1;
					SafeDelete ( dXQ[i].m_pRoot );
				}
			} else
			{
				tMeta.m_iMultiplier = -1;
				SafeDelete ( dXQ[i].m_pRoot );
			}
		} else
		{
			tMeta.m_sError = dXQ[i].m_sParseError;
			tMeta.m_iMultiplier = -1;
		}
		if ( !dXQ[i].m_sParseWarning.IsEmpty() )
			tMeta.m_sWarning = dXQ[i].m_sParseWarning;

		tMeta.m_tIOStats.Stop();
	}

	// continue only if we have at least one non-failed
	if ( bResult )
		Threads::Coro::Continue ( iStackNeed, [&]
	{
		int iCommonSubtrees = 0;
		if ( m_iMaxCachedDocs && m_iMaxCachedHits )
			iCommonSubtrees = sphMarkCommonSubtrees ( iQueries, &dXQ[0] );

		CSphQueryNodeCache tNodeCache ( iCommonSubtrees, m_iMaxCachedDocs, m_iMaxCachedHits );
		bResult = false;
		for ( int j=0; j<iQueries; ++j )
		{
			const auto & tCurQuery = pQueries[j];
			auto & tCurResult = pResults[j];
			auto & tMeta = *tCurResult.m_pMeta;

			// fullscan case
			if ( tCurQuery.m_sQuery.IsEmpty() )
				continue;

			tMeta.m_tIOStats.Start();

			int64_t tmMaxTimer = 0;
			sph::MiniTimer_c tTimerGuard;
			if ( tCurQuery.m_uMaxQueryMsec>0 )
				tmMaxTimer = tTimerGuard.MiniTimerEngage ( tCurQuery.m_uMaxQueryMsec ); // max_query_time

			if ( dXQ[j].m_pRoot && ppSorters[j] && ParsedMultiQuery ( tCurQuery, tCurResult, { ppSorters+j, 1 }, dXQ[j], pDict, tArgs, &tNodeCache, tmMaxTimer ) )
			{
				bResult = true;
				tMeta.m_iMultiplier = iCommonSubtrees ? iQueries : 1;
			} else
				tMeta.m_iMultiplier = -1;

			tMeta.m_tIOStats.Stop();
		}
	});

	if ( tArgs.m_bModifySorterSchemas )
	{
		SwitchProfile ( pResults[0].m_pMeta->m_pProfile, SPH_QSTATE_DYNAMIC );
		PooledAttrsToPtrAttrs (  { ppSorters, iQueries }, m_tBlobAttrs.GetWritePtr(), m_pColumnar.Ptr(), tArgs.m_bFinalizeSorters );
	}

	return bResult || bResultScan;
}


bool CSphIndex_VLN::CheckEarlyReject ( const CSphVector<CSphFilterSettings> & dFilters, ISphFilter * pFilter, ESphCollation eCollation, const ISphSchema & tSchema ) const
{
	if ( !pFilter || dFilters.IsEmpty() )
		return false;

	bool bHaveColumnar = false;

	if ( m_pColumnar )
	{
		// check .SPC file minmax
		CSphString sWarning;
		int iNonColumnar = 0;
		std::vector<columnar::Filter_t> dColumnarFilters;
		ARRAY_FOREACH ( i, dFilters )
		{
			const CSphColumnInfo * pCol = m_tSchema.GetAttr ( dFilters[i].m_sAttrName.cstr() );
			if ( !pCol )
				continue;

			if ( pCol->IsColumnar() || pCol->IsColumnarExpr() )
				AddColumnarFilter ( dColumnarFilters, dFilters[i], eCollation, tSchema, sWarning );
			else
				iNonColumnar++;
		}

		bHaveColumnar = !dColumnarFilters.empty();

		// can't process mixed plain and columnar attributes
		if ( iNonColumnar )
			return false;

		if ( m_pColumnar->EarlyReject ( dColumnarFilters, *pFilter ) )
			return true;
	}

	if ( bHaveColumnar )
		return false;

	if ( m_iDocinfoIndex )
	{
		// check .SPA file minmax
		DWORD uStride = m_tSchema.GetRowSize();
		DWORD * pMinEntry = const_cast<DWORD*> ( &m_pDocinfoIndex [ m_iDocinfoIndex*uStride*2 ] );
		DWORD * pMaxEntry = pMinEntry + uStride;

		if ( !pFilter->EvalBlock ( pMinEntry, pMaxEntry ) )
			return true;
	}

	return false;
}


static const CSphVector<CSphFilterSettings> * SetupRowIdBoundaries ( const CSphVector<CSphFilterSettings> & dFilters, CSphVector<CSphFilterSettings> & dModifiedFilters, RowID_t uTotalDocs, ISphRanker & tRanker )
{
	const CSphFilterSettings * pRowIdFilter = nullptr;
	for ( auto & i : dFilters )
		if ( i.m_sAttrName=="@rowid" )
		{
			pRowIdFilter = &i;
			break;
		}

	if ( !pRowIdFilter )
		return &dFilters;

	RowIdBoundaries_t tBoundaries = GetFilterRowIdBoundaries ( *pRowIdFilter, uTotalDocs );
	tRanker.ExtraData ( EXTRA_SET_BOUNDARIES, (void**)&tBoundaries );

	dModifiedFilters.Resize(0);
	for ( auto & i : dFilters )
		if ( &i!=pRowIdFilter )
			dModifiedFilters.Add(i);

	return &dModifiedFilters;
}


bool CSphIndex_VLN::ParsedMultiQuery ( const CSphQuery & tQuery, CSphQueryResult & tResult, const VecTraits_T<ISphMatchSorter *> & dSorters, const XQQuery_t & tXQ, CSphDict * pDict, const CSphMultiQueryArgs & tArgs, CSphQueryNodeCache * pNodeCache, int64_t tmMaxTimer ) const
{
	assert ( !tQuery.m_sQuery.IsEmpty() && tQuery.m_eMode!=SPH_MATCH_FULLSCAN ); // scans must go through MultiScan()
	assert ( tArgs.m_iTag>=0 );

	auto& tMeta = *tResult.m_pMeta;

	// start counting
	int64_t tmQueryStart = sphMicroTimer();
	auto tmCpuQueryStart = sphTaskCpuTimer ();

	QueryProfile_c * pProfile = tMeta.m_pProfile;
	ESphQueryState eOldState = SPH_QSTATE_UNKNOWN;
	if ( pProfile )
		eOldState = pProfile->Switch ( SPH_QSTATE_INIT );

	ScopedThreadPriority_c tPrio ( tQuery.m_bLowPriority );

	///////////////////
	// setup searching
	///////////////////

	// non-ready index, empty response!
	if ( !m_bPassedAlloc )
	{
		tMeta.m_sError = "index not preread";
		return false;
	}

	// select the sorter with max schema
	int iMaxSchemaIndex = GetMaxSchemaIndexAndMatchCapacity ( dSorters ).first;
	const ISphSchema & tMaxSorterSchema = *(dSorters[iMaxSchemaIndex]->GetSchema());
	auto dSorterSchemas = SorterSchemas ( dSorters, iMaxSchemaIndex);

	// setup calculations and result schema
	CSphQueryContext tCtx ( tQuery );
	tCtx.m_pProfile = pProfile;
	tCtx.m_pLocalDocs = tArgs.m_pLocalDocs;
	tCtx.m_iTotalDocs = ( tArgs.m_iTotalDocs ? tArgs.m_iTotalDocs : m_tStats.m_iTotalDocuments );

	if ( !tCtx.SetupCalc ( tMeta, tMaxSorterSchema, m_tSchema, m_tBlobAttrs.GetWritePtr(), m_pColumnar.Ptr(), dSorterSchemas ) )
		return false;

	// set blob pool for string on_sort expression fix up
	tCtx.SetBlobPool ( m_tBlobAttrs.GetWritePtr() );
	tCtx.m_uPackedFactorFlags = tArgs.m_uPackedFactorFlags;

	// open files
	DataReaderFactoryPtr_c pDoclist = m_pDoclistFile;
	DataReaderFactoryPtr_c pHitlist = m_pHitlistFile;

	if ( !pDoclist || !pHitlist )
		SwitchProfile ( pProfile, SPH_QSTATE_OPEN );

	if ( !pDoclist )
	{
		pDoclist = NewProxyReader ( GetIndexFileName ( SPH_EXT_SPD ), tMeta.m_sError, DataReaderFactory_c::DOCS, m_tMutableSettings.m_tFileAccess.m_iReadBufferDocList, FileAccess_e::FILE );
		if ( !pDoclist )
			return false;
	}

	if ( !pHitlist )
	{
		pHitlist = NewProxyReader ( GetIndexFileName ( SPH_EXT_SPP ), tMeta.m_sError, DataReaderFactory_c::HITS, m_tMutableSettings.m_tFileAccess.m_iReadBufferHitList, FileAccess_e::FILE );
		if ( !pHitlist )
			return false;
	}

	pDoclist->SetProfile ( pProfile );
	pHitlist->SetProfile ( pProfile );

	SwitchProfile ( pProfile, SPH_QSTATE_INIT );

	// setup search terms
	DiskIndexQwordSetup_c tTermSetup ( pDoclist, pHitlist, m_tSkiplists.GetWritePtr(), m_tSettings.m_iSkiplistBlockSize, true, RowID_t(m_iDocinfo) );

	tTermSetup.SetDict ( pDict );
	tTermSetup.m_pIndex = this;
	tTermSetup.m_iDynamicRowitems = tMaxSorterSchema.GetDynamicSize();
	tTermSetup.m_iMaxTimer = tmMaxTimer;
	tTermSetup.m_pWarning = &tMeta.m_sWarning;
	tTermSetup.m_pCtx = &tCtx;
	tTermSetup.m_pNodeCache = pNodeCache;
	tTermSetup.m_bHasWideFields = ( m_tSchema.GetFieldsCount()>32 );

	// setup prediction constrain
	CSphQueryStats tQueryStats;
	bool bCollectPredictionCounters = ( tQuery.m_iMaxPredictedMsec>0 );
	int64_t iNanoBudget = (int64_t)( tQuery.m_iMaxPredictedMsec) * 1000000; // from milliseconds to nanoseconds
	tQueryStats.m_pNanoBudget = &iNanoBudget;
	if ( bCollectPredictionCounters )
		tTermSetup.m_pStats = &tQueryStats;

	// bind weights
	tCtx.BindWeights ( tQuery, m_tSchema, tMeta.m_sWarning );

	// setup query
	// must happen before index-level reject, in order to build proper keyword stats
	CSphScopedPtr<ISphRanker> pRanker ( sphCreateRanker ( tXQ, tQuery, tMeta, tTermSetup, tCtx, tMaxSorterSchema )  );
	if ( !pRanker.Ptr() )
		return false;

	if ( ( tArgs.m_uPackedFactorFlags & SPH_FACTOR_ENABLE ) && tQuery.m_eRanker!=SPH_RANK_EXPR )
		tMeta.m_sWarning.SetSprintf ( "packedfactors() and bm25f() requires using an expression ranker" );

	tCtx.SetupExtraData ( pRanker.Ptr(), dSorters.GetLength()==1 ? dSorters[0] : nullptr );

	BYTE * pBlobPool = m_tBlobAttrs.GetWritePtr();
	pRanker->ExtraData ( EXTRA_SET_BLOBPOOL, (void**)&pBlobPool );

	int iMatchPoolSize = 0;
	dSorters.Apply ( [&iMatchPoolSize] ( const ISphMatchSorter * p ) { iMatchPoolSize += p->GetMatchCapacity(); } );

	pRanker->ExtraData ( EXTRA_SET_POOL_CAPACITY, (void**)&iMatchPoolSize );

	// check for the possible integer overflow in m_dPool.Resize
	int64_t iPoolSize = 0;
	if ( pRanker->ExtraData ( EXTRA_GET_POOL_SIZE, (void**)&iPoolSize ) && iPoolSize>INT_MAX )
	{
		tMeta.m_sError.SetSprintf ( "ranking factors pool too big (%d Mb), reduce max_matches", (int)( iPoolSize/1024/1024 ) );
		return false;
	}

	// empty index, empty response!
	if ( m_bIsEmpty )
		return true;

	CSphVector<CSphFilterSettings> dModifiedFilters;
	const CSphVector<CSphFilterSettings> * pFilters = SetupRowIdBoundaries ( tQuery.m_dFilters, dModifiedFilters, RowID_t(m_iDocinfo), *pRanker );

	// setup filters
 	CreateFilterContext_t tFlx;
	tFlx.m_pFilters = pFilters;
	tFlx.m_pFilterTree = &tQuery.m_dFilterTree;
	tFlx.m_pSchema = &tMaxSorterSchema;
	tFlx.m_pBlobPool = m_tBlobAttrs.GetWritePtr();
	tFlx.m_pColumnar = m_pColumnar.Ptr();
	tFlx.m_eCollation = tQuery.m_eCollation;
	tFlx.m_bScan = tQuery.m_sQuery.IsEmpty ();
	tFlx.m_pHistograms = m_pHistograms;

	if ( !tCtx.CreateFilters ( tFlx, tMeta.m_sError, tMeta.m_sWarning ) )
		return false;

	if ( CheckEarlyReject ( *pFilters, tCtx.m_pFilter, tQuery.m_eCollation, tMaxSorterSchema ) )
	{
		tMeta.m_iQueryTime += (int)( ( sphMicroTimer()-tmQueryStart )/1000 );
		tMeta.m_iCpuTime += sphTaskCpuTimer ()-tmCpuQueryStart;
		return true;
	}

	for ( auto & i : dSorters )
	{
		i->SetBlobPool ( m_tBlobAttrs.GetWritePtr() );
		i->SetColumnar ( m_pColumnar.Ptr() );
	}

	bool bHaveRandom = false;
	dSorters.Apply ( [&bHaveRandom] ( const ISphMatchSorter * p ) { bHaveRandom |= p->IsRandom(); } );

	bool bUseFactors = !!( tCtx.m_uPackedFactorFlags & SPH_FACTOR_ENABLE );
	bool bHaveDead = m_tDeadRowMap.HasDead();

	//////////////////////////////////////
	// find and weight matching documents
	//////////////////////////////////////

	bool bFinalPass = !!tCtx.m_dCalcFinal.GetLength();
	int iMyTag = bFinalPass ? -1 : tArgs.m_iTag;

	// a shortcut to avoid listing all args every time
	void (CSphIndex_VLN::*pFunc)(CSphQueryContext &, const CSphQuery &, const VecTraits_T<ISphMatchSorter*>&, ISphRanker *, int, int) const = nullptr;

	switch ( tQuery.m_eMode )
	{
		case SPH_MATCH_ALL:
		case SPH_MATCH_PHRASE:
		case SPH_MATCH_ANY:
		case SPH_MATCH_EXTENDED:
		case SPH_MATCH_EXTENDED2:
		case SPH_MATCH_BOOLEAN:
			if ( bHaveDead )
			{
				if ( bHaveRandom )
				{
					if ( bUseFactors )
						pFunc = &CSphIndex_VLN::MatchExtended<true, true, true>;
					else
						pFunc = &CSphIndex_VLN::MatchExtended<true, true, false>;
				}
				else
				{
					if ( bUseFactors )
						pFunc = &CSphIndex_VLN::MatchExtended<true, false, true>;
					else
						pFunc = &CSphIndex_VLN::MatchExtended<true, false, false>;
				}
			}
			else
			{
				if ( bHaveRandom )
				{
					if ( bUseFactors )
						pFunc = &CSphIndex_VLN::MatchExtended<false, true, true>;
					else
						pFunc = &CSphIndex_VLN::MatchExtended<false, true, false>;
				}
				else
				{
					if ( bUseFactors )
						pFunc = &CSphIndex_VLN::MatchExtended<false, false, true>;
					else
						pFunc = &CSphIndex_VLN::MatchExtended<false, false, false>;
				}
			}

			(*this.*pFunc)( tCtx, tQuery, dSorters, pRanker.Ptr(), iMyTag, tArgs.m_iIndexWeight );
			break;

		default:
			sphDie ( "INTERNAL ERROR: unknown matching mode (mode=%d)", tQuery.m_eMode );
	}

	////////////////////
	// cook result sets
	////////////////////

	SwitchProfile ( pProfile, SPH_QSTATE_FINALIZE );

	// adjust result sets
	if ( bFinalPass )
	{
		// GotUDF means promise to UDFs that final-stage calls will be evaluated
		// a) over the final, pre-limit result set
		// b) in the final result set order
		bool bGotUDF = false;

		DocstoreSession_c tSession;
		int64_t iSessionUID = tSession.GetUID();

		// spawn buffered readers for the current session
		// put them to a global hash
		if ( m_pDocstore )
			m_pDocstore->CreateReader ( iSessionUID );

		DocstoreSession_c::InfoRowID_t tSessionInfo;
		tSessionInfo.m_pDocstore = m_pDocstore.Ptr();
		tSessionInfo.m_iSessionId = iSessionUID;

		for ( auto & i : tCtx.m_dCalcFinal )
		{
			assert ( i.m_pExpr );
			if ( !bGotUDF )
				i.m_pExpr->Command ( SPH_EXPR_GET_UDF, &bGotUDF );

			if ( m_pDocstore )
				i.m_pExpr->Command ( SPH_EXPR_SET_DOCSTORE_ROWID, &tSessionInfo );
		}

		SphFinalMatchCalc_t tFinal ( tArgs.m_iTag, tCtx );
		dSorters.Apply ( [&] ( ISphMatchSorter * p ) { p->Finalize ( tFinal, bGotUDF, tArgs.m_bFinalizeSorters ); } );
	}

	pRanker->FinalizeCache ( tMaxSorterSchema );

	tResult.m_pBlobPool = m_tBlobAttrs.GetWritePtr();
	tResult.m_pDocstore = m_pDocstore.Ptr() ? this : nullptr;
	tResult.m_pColumnar = m_pColumnar.Ptr();

	// query timer
	int64_t tmWall = sphMicroTimer() - tmQueryStart;
	tMeta.m_iQueryTime += (int)( tmWall/1000 );
	tMeta.m_iCpuTime += sphTaskCpuTimer ()-tmCpuQueryStart;

#if 0
	printf ( "qtm %d, %d, %d, %d, %d\n", int(tmWall), tQueryStats.m_iFetchedDocs, tQueryStats.m_iFetchedHits, tQueryStats.m_iSkips, dSorters[0]->GetTotalCount() );
#endif

	SwitchProfile ( pProfile, eOldState );

	if ( bCollectPredictionCounters )
	{
		tMeta.m_tStats.Add ( tQueryStats );
		tMeta.m_bHasPrediction = true;
	}

	return true;
}


void CSphIndex_VLN::CreateReader ( int64_t iSessionId ) const
{
	if ( !m_pDocstore.Ptr() )
		return;

	m_pDocstore->CreateReader(iSessionId);
}


bool CSphIndex_VLN::GetDoc ( DocstoreDoc_t & tDoc, DocID_t tDocID, const VecTraits_T<int> * pFieldIds, int64_t iSessionId, bool bPack ) const
{
	if ( !m_pDocstore.Ptr() )
		return false;

	RowID_t tRowID = GetRowidByDocid ( tDocID );
	if ( tRowID==INVALID_ROWID || m_tDeadRowMap.IsSet(tRowID) )
		return false;

	tDoc = m_pDocstore->GetDoc ( tRowID, pFieldIds, iSessionId, bPack );
	return true;
}


int CSphIndex_VLN::GetFieldId ( const CSphString & sName, DocstoreDataType_e eType ) const
{
	if ( !m_pDocstore.Ptr() )
		return -1;

	return m_pDocstore->GetFieldId ( sName, eType );
}


//////////////////////////////////////////////////////////////////////////
// INDEX STATUS
//////////////////////////////////////////////////////////////////////////

void CSphIndex_VLN::GetStatus ( CSphIndexStatus* pRes ) const
{
	assert ( pRes );
	if ( !pRes )
		return;

	pRes->m_iMapped = m_tAttr.GetLengthBytes ()
			+m_tBlobAttrs.GetLengthBytes ()
			+m_tWordlist.m_tBuf.GetLengthBytes ()
			+m_tDeadRowMap.GetLengthBytes ()
			+m_tSkiplists.GetLengthBytes ();

	pRes->m_iMappedResident = m_tAttr.GetCoreSize ()
			+m_tBlobAttrs.GetCoreSize ()
			+m_tWordlist.m_tBuf.GetCoreSize ()
			+m_tDeadRowMap.GetCoreSize ()
			+m_tSkiplists.GetCoreSize ();

	pRes->m_iDead = m_tDeadRowMap.GetNumDeads();

	if ( m_pDoclistFile )
	{
		pRes->m_iMappedDocs = m_pDoclistFile->GetMappedsize ();
		pRes->m_iMappedResidentDocs = m_pDoclistFile->GetCoresize ();
		pRes->m_iMapped += pRes->m_iMappedDocs;
		pRes->m_iMappedResident += pRes->m_iMappedResidentDocs;
	}

	if ( m_pHitlistFile )
	{
		pRes->m_iMappedHits = m_pHitlistFile->GetMappedsize ();
		pRes->m_iMappedResidentHits = m_pHitlistFile->GetCoresize ();
		pRes->m_iMapped += pRes->m_iMappedHits;
		pRes->m_iMappedResident += pRes->m_iMappedResidentHits;
	}

	pRes->m_iRamUse = sizeof(CSphIndex_VLN) + m_dFieldLens.GetLengthBytes() + pRes->m_iMappedResident;
	pRes->m_iDiskUse = 0;

	CSphVector<IndexFileExt_t> dExts = sphGetExts();
	for ( const auto & i : dExts )
		if ( i.m_eExt!=SPH_EXT_SPL )
		{
			CSphString sFile;
			sFile.SetSprintf ( "%s%s", m_sFilename.cstr(), i.m_szExt );
			struct_stat st;
			if ( stat ( sFile.cstr(), &st )==0 )
				pRes->m_iDiskUse += st.st_size;
		}
//	sphWarning ( "Chunks: %d, RAM: %d, DISK: %d", pRes->m_iNumChunks, (int) pRes->m_iRamUse, (int) pRes->m_iMapped );
}


//////////////////////////////////////////////////////////////////////////
// INDEX CHECKING
//////////////////////////////////////////////////////////////////////////

void CSphIndex_VLN::SetDebugCheck ( bool bCheckIdDups, int )
{
	m_bDebugCheck = true;
	m_bCheckIdDups = bCheckIdDups;
}


// no strnlen on some OSes (Mac OS)
#if !HAVE_STRNLEN
size_t strnlen ( const char * s, size_t iMaxLen )
{
	if ( !s )
		return 0;

	size_t iRes = 0;
	while ( *s++ && iRes<iMaxLen )
		++iRes;
	return iRes;
}
#endif


int CSphIndex_VLN::DebugCheck ( DebugCheckError_i& tReporter )
{
	CSphScopedPtr<DiskIndexChecker_i> pIndexChecker ( CreateDiskIndexChecker ( *this, tReporter ) );

	pIndexChecker->Setup ( m_iDocinfo, m_iDocinfoIndex, m_iMinMaxIndex, m_bCheckIdDups );

	// check if index is ready
	if ( !m_bPassedAlloc )
		tReporter.Fail ( "index not preread" );

	CSphString sError;
	if ( !pIndexChecker->OpenFiles(sError) )
		return 1;

	if ( !LoadHitlessWords ( m_tSettings.m_sHitlessFiles, m_pTokenizer, m_pDict, pIndexChecker->GetHitlessWords(), m_sLastError ) )
		tReporter.Fail ( "unable to load hitless words: %s", m_sLastError.cstr() );

	CSphSavedFile tStat;
	const CSphTokenizerSettings & tTokenizerSettings = m_pTokenizer->GetSettings ();
	if ( !tTokenizerSettings.m_sSynonymsFile.IsEmpty() && !tStat.Collect ( tTokenizerSettings.m_sSynonymsFile.cstr(), &sError ) )
		tReporter.Fail ( "unable to open exceptions '%s': %s", tTokenizerSettings.m_sSynonymsFile.cstr(), sError.cstr() );

	const CSphDictSettings & tDictSettings = m_pDict->GetSettings ();
	const char * pStop = tDictSettings.m_sStopwords.cstr();
	while (true)
	{
		// find next name start
		while ( pStop && *pStop && isspace(*pStop) ) pStop++;
		if ( !pStop || !*pStop ) break;

		const char * sNameStart = pStop;

		// find next name end
		while ( *pStop && !isspace(*pStop) ) pStop++;

		CSphString sStopFile;
		sStopFile.SetBinary ( sNameStart, int ( pStop-sNameStart ) );

		if ( !tStat.Collect ( sStopFile.cstr(), &sError ) )
			tReporter.Fail ( "unable to open stopwords '%s': %s", sStopFile.cstr(), sError.cstr() );
	}

	if ( !tDictSettings.m_dWordforms.GetLength() )
	{
		ARRAY_FOREACH ( i, tDictSettings.m_dWordforms )
		{
			if ( !tStat.Collect ( tDictSettings.m_dWordforms[i].cstr(), &sError ) )
				tReporter.Fail ( "unable to open wordforms '%s': %s", tDictSettings.m_dWordforms[i].cstr(), sError.cstr() );
		}
	}

	pIndexChecker->Check();

	tReporter.Done();

	return (int)Min ( tReporter.GetNumFails(), 255 ); // this is the exitcode; so cap it
} // NOLINT function length


static void AddFields ( const char * sQuery, CSphSchema & tSchema )
{
	CSphColumnInfo tField;

	const char * sToken = sQuery;
	if ( !sToken )
	{
		tField.m_sName = "dummy_field"; // for query with only all fields, @*
		tSchema.AddField ( tField );
		return;
	}

	const char * OPTION_RELAXED = "@@relaxed";
	const auto OPTION_RELAXED_LEN = (int) strlen ( OPTION_RELAXED );
	if ( strncmp ( sToken, OPTION_RELAXED, OPTION_RELAXED_LEN )==0 && !sphIsAlpha ( sToken[OPTION_RELAXED_LEN] ) )
		sToken += OPTION_RELAXED_LEN;

	while ( *sToken )
	{
		if ( *sToken!='@' )
		{
			sToken++;
			continue;
		}

		sToken++;
		if ( !*sToken )
			break;
		if ( *sToken=='!' || *sToken=='*' )
			sToken++;
		if ( !*sToken )
			break;
		bool bBlock = ( *sToken=='(' );
		if ( bBlock )
			sToken++;
		if ( !*sToken )
			break;

		// handle block with field names
		while ( *sToken )
		{
			const char * sField = sToken;
			while ( *sToken && sphIsAlpha( *sToken ) )
				sToken++;

			int iLen = int ( sToken - sField );
			if ( iLen )
			{
				tField.m_sName.SetBinary ( sField, iLen );
				if ( !tSchema.GetField ( tField.m_sName.cstr() ) )
					tSchema.AddField ( tField );
			}

			if ( !bBlock )
				break;

			if ( *sToken && *sToken==',' )
				sToken++;

			if ( *sToken && *sToken==')' )
				break;
		}
	}

	if ( !tSchema.GetFieldsCount() )
	{
		tField.m_sName = "dummy_field"; // for query with only all fields, @*
		tSchema.AddField ( tField );
	}
}

Bson_t EmptyBson ()
{
	Bson_t dEmpty;
	return dEmpty;
}

Bson_t Explain ( ExplainQueryArgs_t & tArgs )
{
	if ( tArgs.m_sQuery.IsEmpty() )
		return EmptyBson ();

	CSphScopedPtr<QueryParser_i> pQueryParser ( sphCreatePlainQueryParser() );

	CSphVector<BYTE> dFiltered;
	const BYTE * sModifiedQuery = (const BYTE *)tArgs.m_sQuery.cstr();

	if ( tArgs.m_pFieldFilter && sModifiedQuery )
	{
		if ( tArgs.m_pFieldFilter->Apply ( sModifiedQuery, dFiltered, true ) )
			sModifiedQuery = dFiltered.Begin();
	}

	CSphSchema tSchema;
	const CSphSchema * pSchema = tArgs.m_pSchema;
	if ( !pSchema )
	{
		pSchema = &tSchema;
		// need to fill up schema with fields from query
		AddFields ( tArgs.m_sQuery.cstr(), tSchema );
	}

	XQQuery_t tParsed;
	if ( !pQueryParser->ParseQuery ( tParsed, (const char*)sModifiedQuery, nullptr, tArgs.m_pQueryTokenizer, nullptr, pSchema, tArgs.m_pDict, *tArgs.m_pSettings ) )
	{
		TlsMsg::Err ( tParsed.m_sParseError );
		return EmptyBson ();
	}

	sphTransformExtendedQuery ( &tParsed.m_pRoot, *tArgs.m_pSettings, false, nullptr );

	bool bWordDict = tArgs.m_pDict->GetSettings().m_bWordDict;
	int iExpandKeywords = ExpandKeywords ( tArgs.m_iExpandKeywords, QUERY_OPT_DEFAULT, *tArgs.m_pSettings, bWordDict );
	if ( iExpandKeywords!=KWE_DISABLED )
	{
		sphQueryExpandKeywords ( &tParsed.m_pRoot, *tArgs.m_pSettings, iExpandKeywords, bWordDict );
		tParsed.m_pRoot->Check ( true );
	}

	// this should be after keyword expansion
	TransformAotFilter ( tParsed.m_pRoot, tArgs.m_pDict->GetWordforms(), *tArgs.m_pSettings );

	//// expanding prefix in word dictionary case
	if ( tArgs.m_bExpandPrefix )
	{
		CSphQueryResultMeta tMeta;
		CSphScopedPayload tPayloads;

		ExpansionContext_t tExpCtx;
		tExpCtx.m_pWordlist = tArgs.m_pWordlist;
		tExpCtx.m_pResult = &tMeta;
		tExpCtx.m_iMinPrefixLen = tArgs.m_pSettings->GetMinPrefixLen ( tArgs.m_pDict->GetSettings().m_bWordDict );
		tExpCtx.m_iMinInfixLen = tArgs.m_pSettings->m_iMinInfixLen;
		tExpCtx.m_iExpansionLimit = tArgs.m_iExpansionLimit;
		tExpCtx.m_bHasExactForms = ( tArgs.m_pDict->HasMorphology() || tArgs.m_pSettings->m_bIndexExactWords );
		tExpCtx.m_bMergeSingles = false;
		tExpCtx.m_pPayloads = &tPayloads;
		tExpCtx.m_pIndexData = tArgs.m_pIndexData;

		tParsed.m_pRoot = sphExpandXQNode ( tParsed.m_pRoot, tExpCtx );
	}

	return sphExplainQuery ( tParsed.m_pRoot, *pSchema, tParsed.m_dZones );
}

Bson_t CSphIndex_VLN::ExplainQuery ( const CSphString & sQuery ) const
{
	ExplainQueryArgs_t tArgs ( sQuery );
	tArgs.m_pSchema = &GetMatchSchema();
	tArgs.m_pDict = GetStatelessDict ( m_pDict );
	SetupStarDict ( tArgs.m_pDict );
	SetupExactDict ( tArgs.m_pDict );
	if ( m_pFieldFilter )
		tArgs.m_pFieldFilter = m_pFieldFilter->Clone();
	tArgs.m_pSettings = &m_tSettings;
	tArgs.m_pWordlist = &m_tWordlist;
	tArgs.m_pQueryTokenizer = m_pQueryTokenizer;
	tArgs.m_iExpandKeywords = m_tMutableSettings.m_iExpandKeywords;
	tArgs.m_iExpansionLimit = m_iExpansionLimit;
	tArgs.m_bExpandPrefix = ( m_pDict->GetSettings().m_bWordDict && IsStarDict ( m_pDict->GetSettings().m_bWordDict ) );

	return Explain ( tArgs );
}


bool CSphIndex_VLN::CopyExternalFiles ( int iPostfix, StrVec_t & dCopied )
{
	CSphVector<std::pair<CSphString,CSphString>> dExtFiles;
	if ( m_pTokenizer && !m_pTokenizer->GetSettings().m_sSynonymsFile.IsEmpty() )
	{
		CSphString sRenameTo;
		sRenameTo.SetSprintf ( "exceptions_chunk%d.txt", iPostfix );
		dExtFiles.Add ( { m_pTokenizer->GetSettings().m_sSynonymsFile, sRenameTo } );
		const_cast<CSphTokenizerSettings &>(m_pTokenizer->GetSettings()).m_sSynonymsFile = sRenameTo;
	}

	if ( m_pDict )
	{
		const CSphString & sStopwords = m_pDict->GetSettings().m_sStopwords;
		if ( !sStopwords.IsEmpty() )
		{
			StringBuilder_c sNewStopwords(" ");
			StrVec_t dStops = sphSplit ( sStopwords.cstr(), sStopwords.Length(), " \t," );
			ARRAY_FOREACH ( i, dStops )
			{
				CSphString sTmp;
				sTmp.SetSprintf ( "stopwords_chunk%d_%d.txt", iPostfix, i );
				dExtFiles.Add ( { dStops[i], sTmp } );

				sNewStopwords << sTmp;
			}

			const_cast<CSphDictSettings &>(m_pDict->GetSettings()).m_sStopwords = sNewStopwords.cstr();
		}

		StrVec_t dNewWordforms;
		ARRAY_FOREACH ( i, m_pDict->GetSettings().m_dWordforms )
		{
			CSphString sTmp;
			sTmp.SetSprintf ( "wordforms_chunk%d_%d.txt", iPostfix, i );
			dExtFiles.Add( { m_pDict->GetSettings().m_dWordforms[i], sTmp } );
			dNewWordforms.Add(sTmp);
		}

		const_cast<CSphDictSettings &>(m_pDict->GetSettings()).m_dWordforms = dNewWordforms;
	}

	CSphString sPathOnly = GetPathOnly(m_sFilename);
	for ( const auto & i : dExtFiles )
	{
		CSphString sDest;
		sDest.SetSprintf ( "%s%s", sPathOnly.cstr(), i.second.cstr() );
		if ( !CopyFile ( i.first, sDest, m_sLastError ) )
			return false;

		dCopied.Add(sDest);
	}

	BuildHeader_t tBuildHeader(m_tStats);
	*(DictHeader_t*)&tBuildHeader = *(DictHeader_t*)&m_tWordlist;
	tBuildHeader.m_iDocinfo = m_iDocinfo;
	tBuildHeader.m_iDocinfoIndex = m_iDocinfoIndex;
	tBuildHeader.m_iMinMaxIndex = m_iMinMaxIndex;

	WriteHeader_t tWriteHeader;
	tWriteHeader.m_pSettings = &m_tSettings;
	tWriteHeader.m_pSchema = &m_tSchema;
	tWriteHeader.m_pTokenizer = m_pTokenizer;
	tWriteHeader.m_pDict = m_pDict;
	tWriteHeader.m_pFieldFilter = m_pFieldFilter;
	tWriteHeader.m_pFieldLens = m_dFieldLens.Begin();

	// save the header
	return IndexBuildDone ( tBuildHeader, tWriteHeader, GetIndexFileName(SPH_EXT_SPH), m_sLastError );
}

void CSphIndex_VLN::CollectFiles ( StrVec_t & dFiles, StrVec_t & dExt ) const
{
	if ( m_pTokenizer && !m_pTokenizer->GetSettings().m_sSynonymsFile.IsEmpty() )
		dExt.Add ( m_pTokenizer->GetSettings ().m_sSynonymsFile );

	if ( !m_pDict )
		return;

	const CSphString & sStopwords = m_pDict->GetSettings().m_sStopwords;
	if ( !sStopwords.IsEmpty() )
		sphSplit ( dExt, sStopwords.cstr(), sStopwords.Length(), " \t," );

	m_pDict->GetSettings ().m_dWordforms.Apply ( [&dExt] ( const CSphString & a ) { dExt.Add ( a ); } );

	dFiles.Add ( GetIndexFileName ( SPH_EXT_SPH ) );
	dFiles.Add ( GetIndexFileName ( SPH_EXT_SPD ) );
	dFiles.Add ( GetIndexFileName ( SPH_EXT_SPP ) );
	dFiles.Add ( GetIndexFileName ( SPH_EXT_SPE ) );
	dFiles.Add ( GetIndexFileName ( SPH_EXT_SPI ) );
	dFiles.Add ( GetIndexFileName ( SPH_EXT_SPM ) );
	if ( !m_bIsEmpty )
	{
		if ( m_tSchema.HasNonColumnarAttrs() )
			dFiles.Add ( GetIndexFileName ( SPH_EXT_SPA ) );

		dFiles.Add ( GetIndexFileName ( SPH_EXT_SPT ) );
		if ( m_tSchema.GetAttr ( sphGetBlobLocatorName () ) )
			dFiles.Add ( GetIndexFileName ( SPH_EXT_SPB ) );

		if ( m_uVersion>=63 && m_tSchema.HasColumnarAttrs() )
			dFiles.Add ( GetIndexFileName ( SPH_EXT_SPC ) );
	}

	if ( m_uVersion>=55 )
		dFiles.Add ( GetIndexFileName ( SPH_EXT_SPHI ) );

	if ( m_uVersion>=57 && ( m_tSchema.HasStoredFields() || m_tSchema.HasStoredAttrs() ) )
		dFiles.Add ( GetIndexFileName ( SPH_EXT_SPDS ) );

	CSphString sPath = GetIndexFileName ( SPH_EXT_SPK );
	if ( sphIsReadable ( sPath ) )
		dFiles.Add ( sPath );
}

//////////////////////////////////////////////////////////////////////////

/// morphology
enum
{
	SPH_MORPH_STEM_EN,
	SPH_MORPH_STEM_RU_UTF8,
	SPH_MORPH_STEM_CZ,
	SPH_MORPH_STEM_AR_UTF8,
	SPH_MORPH_SOUNDEX,
	SPH_MORPH_METAPHONE_UTF8,
	SPH_MORPH_AOTLEMMER_BASE,
	SPH_MORPH_AOTLEMMER_RU_UTF8 = SPH_MORPH_AOTLEMMER_BASE,
	SPH_MORPH_AOTLEMMER_EN,
	SPH_MORPH_AOTLEMMER_DE_UTF8,
	SPH_MORPH_AOTLEMMER_UK,
	SPH_MORPH_AOTLEMMER_BASE_ALL,
	SPH_MORPH_AOTLEMMER_RU_ALL = SPH_MORPH_AOTLEMMER_BASE_ALL,
	SPH_MORPH_AOTLEMMER_EN_ALL,
	SPH_MORPH_AOTLEMMER_DE_ALL,
	SPH_MORPH_AOTLEMMER_UK_ALL,
	SPH_MORPH_LIBSTEMMER_FIRST,
	SPH_MORPH_LIBSTEMMER_LAST = SPH_MORPH_LIBSTEMMER_FIRST + 64
};


/////////////////////////////////////////////////////////////////////////////
// BASE DICTIONARY INTERFACE
/////////////////////////////////////////////////////////////////////////////

void CSphDict::DictBegin ( CSphAutofile &, CSphAutofile &, int )						{}
void CSphDict::DictEntry ( const CSphDictEntry & )										{}
void CSphDict::DictEndEntries ( SphOffset_t )											{}
bool CSphDict::DictEnd ( DictHeader_t *, int, CSphString & )							{ return true; }
bool CSphDict::DictIsError () const														{ return true; }

/////////////////////////////////////////////////////////////////////////////
// CRC32/64 DICTIONARIES
/////////////////////////////////////////////////////////////////////////////

struct CSphTemplateDictTraits : DictStub_c
{
	CSphTemplateDictTraits ();
protected:
				~CSphTemplateDictTraits () override;

public:
	void		LoadStopwords ( const char * sFiles, const ISphTokenizer * pTokenizer, bool bStripFile ) final;
	void		LoadStopwords ( const CSphVector<SphWordID_t> & dStopwords ) final;
	void		WriteStopwords ( CSphWriter & tWriter ) const final;
	void		WriteStopwords ( JsonEscapedBuilder & tOut ) const final;
	bool		LoadWordforms ( const StrVec_t & dFiles, const CSphEmbeddedFiles * pEmbedded, const ISphTokenizer * pTokenizer, const char * sIndex ) final;
	void		WriteWordforms ( CSphWriter & tWriter ) const final;
	void		WriteWordforms ( JsonEscapedBuilder & tOut ) const final;
	const CSphWordforms *	GetWordforms() final { return m_pWordforms; }
	void		DisableWordforms() final { m_bDisableWordforms = true; }
	int			SetMorphology ( const char * szMorph, CSphString & sMessage ) final;
	bool		HasMorphology() const final;
	void		ApplyStemmers ( BYTE * pWord ) const final;
	const CSphMultiformContainer * GetMultiWordforms () const final;
	uint64_t	GetSettingsFNV () const final;
	static void			SweepWordformContainers ( const CSphVector<CSphSavedFile> & dFiles );

protected:
	CSphVector < int >	m_dMorph;
#if WITH_STEMMER
	CSphVector < sb_stemmer * >	m_dStemmers;
	StrVec_t			m_dDescStemmers;
#endif
	CSphScopedPtr<LemmatizerTrait_i> m_tLemmatizer { nullptr };

	int					m_iStopwords;	///< stopwords count
	SphWordID_t *		m_pStopwords;	///< stopwords ID list
	CSphFixedVector<SphWordID_t> m_dStopwordContainer;

protected:
	int					ParseMorphology ( const char * szMorph, CSphString & sError );
	SphWordID_t			FilterStopword ( SphWordID_t uID ) const;	///< filter ID against stopwords list
	CSphDict *			CloneBase ( CSphTemplateDictTraits * pDict ) const;
	bool				HasState () const final;

	bool				m_bDisableWordforms;

private:
	CSphWordforms *				m_pWordforms;
	static CSphVector<CSphWordforms*>		m_dWordformContainers;

	CSphWordforms *		GetWordformContainer ( const CSphVector<CSphSavedFile> & dFileInfos, const StrVec_t * pEmbeddedWordforms, const ISphTokenizer * pTokenizer, const char * sIndex );
	CSphWordforms *		LoadWordformContainer ( const CSphVector<CSphSavedFile> & dFileInfos, const StrVec_t * pEmbeddedWordforms, const ISphTokenizer * pTokenizer, const char * sIndex );

	int					InitMorph ( const char * szMorph, int iLength, CSphString & sError );
	int					AddMorph ( int iMorph ); ///< helper that always returns ST_OK
	bool				StemById ( BYTE * pWord, int iStemmer ) const;
	void				AddWordform ( CSphWordforms * pContainer, char * sBuffer, int iLen, ISphTokenizer * pTokenizer, const char * szFile, const CSphVector<int> & dBlended, int iFileId );
};

CSphVector<CSphWordforms*> CSphTemplateDictTraits::m_dWordformContainers;


/// common CRC32/64 dictionary stuff
struct CSphDiskDictTraits : CSphTemplateDictTraits
{
	void DictBegin ( CSphAutofile & tTempDict, CSphAutofile & tDict, int iDictLimit ) override;
	void DictEntry ( const CSphDictEntry & tEntry ) override;
	void DictEndEntries ( SphOffset_t iDoclistOffset ) override;
	bool DictEnd ( DictHeader_t * pHeader, int iMemLimit, CSphString & sError ) override;
	bool DictIsError () const final { return m_wrDict.IsError(); }
	void SetSkiplistBlockSize ( int iSize ) final { m_iSkiplistBlockSize=iSize; }

protected:
	~CSphDiskDictTraits () override = default; // fixme! remove

protected:
	CSphTightVector<CSphWordlistCheckpoint>	m_dCheckpoints;		///< checkpoint offsets
	CSphWriter			m_wrDict;			///< final dict file writer
	CSphString			m_sWriterError;		///< writer error message storage
	int					m_iEntries = 0;			///< dictionary entries stored
	SphOffset_t			m_iLastDoclistPos = 0;
	SphWordID_t			m_iLastWordID = 0;
	int					m_iSkiplistBlockSize = 0;
};


template < bool CRCALGO >
struct CCRCEngine
{
	inline static SphWordID_t		DoCrc ( const BYTE * pWord );
	inline static SphWordID_t		DoCrc ( const BYTE * pWord, int iLen );
};

/// specialized CRC32/64 implementations
template < bool CRC32DICT >
struct CSphDictCRC : public CSphDiskDictTraits, CCRCEngine<CRC32DICT>
{
	typedef CCRCEngine<CRC32DICT> tHASH;
	SphWordID_t		GetWordID ( BYTE * pWord ) override;
	SphWordID_t		GetWordID ( const BYTE * pWord, int iLen, bool bFilterStops ) override;
	SphWordID_t		GetWordIDWithMarkers ( BYTE * pWord ) override;
	SphWordID_t		GetWordIDNonStemmed ( BYTE * pWord ) override;
	bool			IsStopWord ( const BYTE * pWord ) const final;

	CSphDict *		Clone () const override { return CloneBase ( new CSphDictCRC<CRC32DICT>() ); }
protected:
	~CSphDictCRC() override {} // fixme! remove
};

struct CSphDictTemplate final : public CSphTemplateDictTraits, CCRCEngine<false> // based on flv64
{
	SphWordID_t		GetWordID ( BYTE * pWord ) final;
	SphWordID_t		GetWordID ( const BYTE * pWord, int iLen, bool bFilterStops ) final;
	SphWordID_t		GetWordIDWithMarkers ( BYTE * pWord ) final;
	SphWordID_t		GetWordIDNonStemmed ( BYTE * pWord ) final;
	bool			IsStopWord ( const BYTE * pWord ) const final;

	CSphDict *		Clone () const final { return CloneBase ( new CSphDictTemplate() ); }

protected:
	~CSphDictTemplate () final 	{} // fixme! remove
};

/////////////////////////////////////////////////////////////////////////////

CSphWordforms::CSphWordforms()
	: m_iRefCount ( 0 )
	, m_uTokenizerFNV ( 0 )
	, m_bHavePostMorphNF ( false )
	, m_pMultiWordforms ( NULL )
{
}


CSphWordforms::~CSphWordforms()
{
	if ( m_pMultiWordforms )
	{
		m_pMultiWordforms->m_Hash.IterateStart ();
		while ( m_pMultiWordforms->m_Hash.IterateNext () )
		{
			CSphMultiforms * pWordforms = m_pMultiWordforms->m_Hash.IterateGet ();
			ARRAY_FOREACH ( i, pWordforms->m_pForms )
				SafeDelete ( pWordforms->m_pForms[i] );

			SafeDelete ( pWordforms );
		}

		SafeDelete ( m_pMultiWordforms );
	}
}


bool CSphWordforms::IsEqual ( const CSphVector<CSphSavedFile> & dFiles )
{
	if ( m_dFiles.GetLength()!=dFiles.GetLength() )
		return false;

	// don't check file time AND check filename w/o path
	// that way same file in different dirs will be only loaded once
	ARRAY_FOREACH ( i, m_dFiles )
	{
		const CSphSavedFile & tF1 = m_dFiles[i];
		const CSphSavedFile & tF2 = dFiles[i];
		CSphString sFile1 = tF1.m_sFilename;
		CSphString sFile2 = tF2.m_sFilename;
		StripPath(sFile1);
		StripPath(sFile2);

		if ( sFile1!=sFile2 || tF1.m_uCRC32!=tF2.m_uCRC32 || tF1.m_uSize!=tF2.m_uSize )
			return false;
	}

	return true;
}


bool CSphWordforms::ToNormalForm ( BYTE * pWord, bool bBefore, bool bOnlyCheck ) const
{
	int * pIndex = m_hHash ( (char *)pWord );
	if ( !pIndex )
		return false;

	if ( *pIndex<0 || *pIndex>=m_dNormalForms.GetLength () )
		return false;

	if ( bBefore==m_dNormalForms[*pIndex].m_bAfterMorphology )
		return false;

	if ( m_dNormalForms[*pIndex].m_sWord.IsEmpty() )
		return false;

	if ( bOnlyCheck )
		return true;

	strcpy ( (char *)pWord, m_dNormalForms[*pIndex].m_sWord.cstr() ); // NOLINT
	return true;
}

/////////////////////////////////////////////////////////////////////////////

CSphTemplateDictTraits::CSphTemplateDictTraits ()
	: m_iStopwords	( 0 )
	, m_pStopwords	( NULL )
	, m_dStopwordContainer ( 0 )
	, m_bDisableWordforms ( false )
	, m_pWordforms	( NULL )
{
}


CSphTemplateDictTraits::~CSphTemplateDictTraits ()
{
#if WITH_STEMMER
	ARRAY_FOREACH ( i, m_dStemmers )
		sb_stemmer_delete ( m_dStemmers[i] );
#endif

	if ( m_pWordforms )
		--m_pWordforms->m_iRefCount;
}


SphWordID_t CSphTemplateDictTraits::FilterStopword ( SphWordID_t uID ) const
{
	if ( !m_iStopwords )
		return uID;

	// OPTIMIZE: binary search is not too good, could do some hashing instead
	SphWordID_t * pStart = m_pStopwords;
	SphWordID_t * pEnd = m_pStopwords + m_iStopwords - 1;
	do
	{
		if ( uID==*pStart || uID==*pEnd )
			return 0;

		if ( uID<*pStart || uID>*pEnd )
			return uID;

		SphWordID_t * pMid = pStart + (pEnd-pStart)/2;
		if ( uID==*pMid )
			return 0;

		if ( uID<*pMid )
			pEnd = pMid;
		else
			pStart = pMid;
	} while ( pEnd-pStart>1 );

	return uID;
}


int CSphTemplateDictTraits::ParseMorphology ( const char * sMorph, CSphString & sMessage )
{
	int iRes = ST_OK;
	for ( const char * sStart=sMorph; ; )
	{
		while ( *sStart && ( sphIsSpace ( *sStart ) || *sStart==',' ) )
			++sStart;
		if ( !*sStart )
			break;

		const char * sWordStart = sStart;
		while ( *sStart && !sphIsSpace ( *sStart ) && *sStart!=',' )
			++sStart;

		if ( sStart > sWordStart )
		{
			switch ( InitMorph ( sWordStart, int ( sStart - sWordStart ), sMessage ) )
			{
				case ST_ERROR:		return ST_ERROR;
				case ST_WARNING:	iRes = ST_WARNING; break;
				default:			break;
			}
		}
	}
	return iRes;
}


int CSphTemplateDictTraits::InitMorph ( const char * szMorph, int iLength, CSphString & sMessage )
{
	if ( iLength==0 )
		return ST_OK;

	if ( iLength==4 && !strncmp ( szMorph, "none", iLength ) )
		return ST_OK;

	if ( iLength==7 && !strncmp ( szMorph, "stem_en", iLength ) )
	{
		if ( m_dMorph.Contains ( SPH_MORPH_AOTLEMMER_EN ) )
		{
			sMessage.SetSprintf ( "stem_en and lemmatize_en clash" );
			return ST_ERROR;
		}

		if ( m_dMorph.Contains ( SPH_MORPH_AOTLEMMER_EN_ALL ) )
		{
			sMessage.SetSprintf ( "stem_en and lemmatize_en_all clash" );
			return ST_ERROR;
		}

		stem_en_init ();
		return AddMorph ( SPH_MORPH_STEM_EN );
	}

	if ( iLength==7 && !strncmp ( szMorph, "stem_ru", iLength ) )
	{
		if ( m_dMorph.Contains ( SPH_MORPH_AOTLEMMER_RU_UTF8 ) )
		{
			sMessage.SetSprintf ( "stem_ru and lemmatize_ru clash" );
			return ST_ERROR;
		}

		if ( m_dMorph.Contains ( SPH_MORPH_AOTLEMMER_RU_ALL ) )
		{
			sMessage.SetSprintf ( "stem_ru and lemmatize_ru_all clash" );
			return ST_ERROR;
		}

		stem_ru_init ();
		return AddMorph ( SPH_MORPH_STEM_RU_UTF8 );
	}

	for ( int j=0; j<AOT_LENGTH; ++j )
	{
		char buf[20];
		char buf_all[20];
		sprintf ( buf, "lemmatize_%s", AOT_LANGUAGES[j] ); // NOLINT
		sprintf ( buf_all, "lemmatize_%s_all", AOT_LANGUAGES[j] ); // NOLINT

		if ( iLength==12 && !strncmp ( szMorph, buf, iLength ) )
		{
			if ( j==AOT_RU && m_dMorph.Contains ( SPH_MORPH_STEM_RU_UTF8 ) )
			{
				sMessage.SetSprintf ( "stem_ru and lemmatize_ru clash" );
				return ST_ERROR;
			}

			if ( j==AOT_EN && m_dMorph.Contains ( SPH_MORPH_STEM_EN ) )
			{
				sMessage.SetSprintf ( "stem_en and lemmatize_en clash" );
				return ST_ERROR;
			}

			// no test for SPH_MORPH_STEM_DE since we doesn't have it.

			if ( m_dMorph.Contains ( SPH_MORPH_AOTLEMMER_BASE_ALL+j ) )
			{
				sMessage.SetSprintf ( "%s and %s clash", buf, buf_all );
				return ST_ERROR;
			}

			CSphString sDictFile;
			sDictFile.SetSprintf ( "%s/%s.pak", g_sLemmatizerBase.cstr(), AOT_LANGUAGES[j] );
			if ( !sphAotInit ( sDictFile, sMessage, j ) )
				return ST_ERROR;

			if ( j==AOT_UK && !m_tLemmatizer.Ptr() )
				m_tLemmatizer = CreateLemmatizer ( j );

			// add manually instead of AddMorph(), because we need to update that fingerprint
			int iMorph = j + SPH_MORPH_AOTLEMMER_BASE;
			if ( j==AOT_RU )
				iMorph = SPH_MORPH_AOTLEMMER_RU_UTF8;
			else if ( j==AOT_DE )
				iMorph = SPH_MORPH_AOTLEMMER_DE_UTF8;
			else if ( j==AOT_UK )
				iMorph = SPH_MORPH_AOTLEMMER_UK;

			if ( !m_dMorph.Contains ( iMorph ) )
			{
				if ( m_sMorphFingerprint.IsEmpty() )
					m_sMorphFingerprint.SetSprintf ( "%s:%08x"
						, sphAotDictinfo(j).first.cstr()
						, sphAotDictinfo(j).second );
				else
					m_sMorphFingerprint.SetSprintf ( "%s;%s:%08x"
					, m_sMorphFingerprint.cstr()
					, sphAotDictinfo(j).first.cstr()
					, sphAotDictinfo(j).second );
				m_dMorph.Add ( iMorph );
			}
			return ST_OK;
		}

		if ( iLength==16 && !strncmp ( szMorph, buf_all, iLength ) )
		{
			if ( j==AOT_RU && ( m_dMorph.Contains ( SPH_MORPH_STEM_RU_UTF8 ) ) )
			{
				sMessage.SetSprintf ( "stem_ru and lemmatize_ru_all clash" );
				return ST_ERROR;
			}

			if ( m_dMorph.Contains ( SPH_MORPH_AOTLEMMER_BASE+j ) )
			{
				sMessage.SetSprintf ( "%s and %s clash", buf, buf_all );
				return ST_ERROR;
			}

			CSphString sDictFile;
			sDictFile.SetSprintf ( "%s/%s.pak", g_sLemmatizerBase.cstr(), AOT_LANGUAGES[j] );
			if ( !sphAotInit ( sDictFile, sMessage, j ) )
				return ST_ERROR;

			if ( j==AOT_UK && !m_tLemmatizer.Ptr() )
				m_tLemmatizer = CreateLemmatizer ( j );

			return AddMorph ( SPH_MORPH_AOTLEMMER_BASE_ALL+j );
		}
	}

	if ( iLength==7 && !strncmp ( szMorph, "stem_cz", iLength ) )
	{
		stem_cz_init ();
		return AddMorph ( SPH_MORPH_STEM_CZ );
	}

	if ( iLength==7 && !strncmp ( szMorph, "stem_ar", iLength ) )
		return AddMorph ( SPH_MORPH_STEM_AR_UTF8 );

	if ( iLength==9 && !strncmp ( szMorph, "stem_enru", iLength ) )
	{
		stem_en_init ();
		stem_ru_init ();
		AddMorph ( SPH_MORPH_STEM_EN );
		return AddMorph ( SPH_MORPH_STEM_RU_UTF8 );
	}

	if ( iLength==7 && !strncmp ( szMorph, "soundex", iLength ) )
		return AddMorph ( SPH_MORPH_SOUNDEX );

	if ( iLength==9 && !strncmp ( szMorph, "metaphone", iLength ) )
		return AddMorph ( SPH_MORPH_METAPHONE_UTF8 );

#if WITH_STEMMER
	const int LIBSTEMMER_LEN = 11;
	const int MAX_ALGO_LENGTH = 64;
	if ( iLength > LIBSTEMMER_LEN && iLength - LIBSTEMMER_LEN < MAX_ALGO_LENGTH && !strncmp ( szMorph, "libstemmer_", LIBSTEMMER_LEN ) )
	{
		CSphString sAlgo;
		sAlgo.SetBinary ( szMorph+LIBSTEMMER_LEN, iLength - LIBSTEMMER_LEN );

		sb_stemmer * pStemmer = NULL;

		pStemmer = sb_stemmer_new ( sAlgo.cstr(), "UTF_8" );

		if ( !pStemmer )
		{
			sMessage.SetSprintf ( "unknown stemmer libstemmer_%s; skipped", sAlgo.cstr() );
			return ST_WARNING;
		}

		AddMorph ( SPH_MORPH_LIBSTEMMER_FIRST + m_dStemmers.GetLength () );
		ARRAY_FOREACH ( i, m_dStemmers )
		{
			if ( m_dStemmers[i]==pStemmer )
			{
				sb_stemmer_delete ( pStemmer );
				return ST_OK;
			}
		}

		m_dStemmers.Add ( pStemmer );
		m_dDescStemmers.Add ( sAlgo );
		return ST_OK;
	}
#endif

	if ( iLength==11 && !strncmp ( szMorph, "icu_chinese", iLength ) )
		return ST_OK;

	sMessage.SetBinary ( szMorph, iLength );
	sMessage.SetSprintf ( "unknown stemmer %s", sMessage.cstr() );
	return ST_ERROR;
}


int CSphTemplateDictTraits::AddMorph ( int iMorph )
{
	if ( !m_dMorph.Contains ( iMorph ) )
		m_dMorph.Add ( iMorph );
	return ST_OK;
}



void CSphTemplateDictTraits::ApplyStemmers ( BYTE * pWord ) const
{
	// try wordforms
	if ( m_pWordforms && m_pWordforms->ToNormalForm ( pWord, true, m_bDisableWordforms ) )
		return;

	// check length
	if ( m_tSettings.m_iMinStemmingLen<=1 || sphUTF8Len ( (const char*)pWord )>=m_tSettings.m_iMinStemmingLen )
	{
		// try stemmers
		ARRAY_FOREACH ( i, m_dMorph )
			if ( StemById ( pWord, m_dMorph[i] ) )
				break;
	}

	if ( m_pWordforms && m_pWordforms->m_bHavePostMorphNF )
		m_pWordforms->ToNormalForm ( pWord, false, m_bDisableWordforms );
}

const CSphMultiformContainer * CSphTemplateDictTraits::GetMultiWordforms () const
{
	return m_pWordforms ? m_pWordforms->m_pMultiWordforms : NULL;
}

uint64_t CSphTemplateDictTraits::GetSettingsFNV () const
{
	uint64_t uHash = (uint64_t)m_pWordforms;

	if ( m_pStopwords )
		uHash = sphFNV64 ( m_pStopwords, m_iStopwords*sizeof(*m_pStopwords), uHash );

	uHash = sphFNV64 ( &m_tSettings.m_iMinStemmingLen, sizeof(m_tSettings.m_iMinStemmingLen), uHash );
	DWORD uFlags = 0;
	if ( m_tSettings.m_bWordDict )
		uFlags |= 1<<0;
	if ( m_tSettings.m_bStopwordsUnstemmed )
		uFlags |= 1<<2;
	uHash = sphFNV64 ( &uFlags, sizeof(uFlags), uHash );

	uHash = sphFNV64 ( m_dMorph.Begin(), m_dMorph.GetLength()*sizeof(m_dMorph[0]), uHash );
#if WITH_STEMMER
	ARRAY_FOREACH ( i, m_dDescStemmers )
		uHash = sphFNV64 ( m_dDescStemmers[i].cstr(), m_dDescStemmers[i].Length(), uHash );
#endif

	return uHash;
}


CSphDict * CSphTemplateDictTraits::CloneBase ( CSphTemplateDictTraits * pDict ) const
{
	assert ( pDict );
	pDict->m_tSettings = m_tSettings;
	pDict->m_iStopwords = m_iStopwords;
	pDict->m_pStopwords = m_pStopwords;
	pDict->m_dSWFileInfos = m_dSWFileInfos;
	pDict->m_dWFFileInfos = m_dWFFileInfos;
	pDict->m_pWordforms = m_pWordforms;
	if ( m_pWordforms )
		m_pWordforms->m_iRefCount++;

	pDict->m_dMorph = m_dMorph;
#if WITH_STEMMER
	assert ( m_dDescStemmers.GetLength()==m_dStemmers.GetLength() );
	pDict->m_dDescStemmers = m_dDescStemmers;
	ARRAY_FOREACH ( i, m_dDescStemmers )
	{
		pDict->m_dStemmers.Add ( sb_stemmer_new ( m_dDescStemmers[i].cstr(), "UTF_8" ) );
		assert ( pDict->m_dStemmers.Last() );
	}
#endif
	if ( m_tLemmatizer.Ptr() )
		pDict->m_tLemmatizer = CreateLemmatizer ( AOT_UK );

	return pDict;
}

bool CSphTemplateDictTraits::HasState() const
{
#if !WITH_STEMMER
	return ( m_tLemmatizer.Ptr() );
#else
	return ( m_dDescStemmers.GetLength()>0 || m_tLemmatizer.Ptr() );
#endif
}

/////////////////////////////////////////////////////////////////////////////

template<>
SphWordID_t CCRCEngine<true>::DoCrc ( const BYTE * pWord )
{
	return sphCRC32 ( pWord );
}


template<>
SphWordID_t CCRCEngine<false>::DoCrc ( const BYTE * pWord )
{
	return (SphWordID_t) sphFNV64 ( pWord );
}


template<>
SphWordID_t CCRCEngine<true>::DoCrc ( const BYTE * pWord, int iLen )
{
	return sphCRC32 ( pWord, iLen );
}


template<>
SphWordID_t CCRCEngine<false>::DoCrc ( const BYTE * pWord, int iLen )
{
	return (SphWordID_t) sphFNV64 ( pWord, iLen );
}


template < bool CRC32DICT >
SphWordID_t CSphDictCRC<CRC32DICT>::GetWordID ( BYTE * pWord )
{
	// apply stopword filter before stemmers
	if ( GetSettings().m_bStopwordsUnstemmed && !FilterStopword ( tHASH::DoCrc ( pWord ) ) )
		return 0;

	// skip stemmers for magic words
	if ( pWord[0]>=0x20 )
		ApplyStemmers ( pWord );

	// stemmer might squeeze out the word
	if ( !pWord[0] )
		return 0;

	return GetSettings().m_bStopwordsUnstemmed
		? tHASH::DoCrc ( pWord )
		: FilterStopword ( tHASH::DoCrc ( pWord ) );
}


template < bool CRC32DICT >
SphWordID_t CSphDictCRC<CRC32DICT>::GetWordID ( const BYTE * pWord, int iLen, bool bFilterStops )
{
	SphWordID_t uId = tHASH::DoCrc ( pWord, iLen );
	return bFilterStops ? FilterStopword ( uId ) : uId;
}


template < bool CRC32DICT >
SphWordID_t CSphDictCRC<CRC32DICT>::GetWordIDWithMarkers ( BYTE * pWord )
{
	ApplyStemmers ( pWord + 1 );
	SphWordID_t uWordId = tHASH::DoCrc ( pWord + 1 );
	auto iLength = strlen ( (const char *)(pWord + 1) );
	pWord [iLength + 1] = MAGIC_WORD_TAIL;
	pWord [iLength + 2] = '\0';
	return FilterStopword ( uWordId ) ? tHASH::DoCrc ( pWord ) : 0;
}


template < bool CRC32DICT >
SphWordID_t CSphDictCRC<CRC32DICT>::GetWordIDNonStemmed ( BYTE * pWord )
{
	// this method can generally receive both '=stopword' with a marker and 'stopword' without it
	// so for filtering stopwords, let's handle both
	int iOff = ( pWord[0]<' ' );
	SphWordID_t uWordId = tHASH::DoCrc ( pWord+iOff );
	if ( !FilterStopword ( uWordId ) )
		return 0;

	return tHASH::DoCrc ( pWord );
}


template < bool CRC32DICT >
bool CSphDictCRC<CRC32DICT>::IsStopWord ( const BYTE * pWord ) const
{
	return FilterStopword ( tHASH::DoCrc ( pWord ) )==0;
}


//////////////////////////////////////////////////////////////////////////
SphWordID_t CSphDictTemplate::GetWordID ( BYTE * pWord )
{
	// apply stopword filter before stemmers
	if ( GetSettings().m_bStopwordsUnstemmed && !FilterStopword ( DoCrc ( pWord ) ) )
		return 0;

	// skip stemmers for magic words
	if ( pWord[0]>=0x20 )
		ApplyStemmers ( pWord );

	return GetSettings().m_bStopwordsUnstemmed
		? DoCrc ( pWord )
		: FilterStopword ( DoCrc ( pWord ) );
}


SphWordID_t CSphDictTemplate::GetWordID ( const BYTE * pWord, int iLen, bool bFilterStops )
{
	SphWordID_t uId = DoCrc ( pWord, iLen );
	return bFilterStops ? FilterStopword ( uId ) : uId;
}


SphWordID_t CSphDictTemplate::GetWordIDWithMarkers ( BYTE * pWord )
{
	ApplyStemmers ( pWord + 1 );
	// stemmer might squeeze out the word
	if ( !pWord[1] )
		return 0;
	SphWordID_t uWordId = DoCrc ( pWord + 1 );
	auto iLength = (int) strlen ( (const char *)(pWord + 1) );
	pWord [iLength + 1] = MAGIC_WORD_TAIL;
	pWord [iLength + 2] = '\0';
	return FilterStopword ( uWordId ) ? DoCrc ( pWord ) : 0;
}


SphWordID_t CSphDictTemplate::GetWordIDNonStemmed ( BYTE * pWord )
{
	SphWordID_t uWordId = DoCrc ( pWord + 1 );
	if ( !FilterStopword ( uWordId ) )
		return 0;

	return DoCrc ( pWord );
}

bool CSphDictTemplate::IsStopWord ( const BYTE * pWord ) const
{
	return FilterStopword ( DoCrc ( pWord ) )==0;
}

//////////////////////////////////////////////////////////////////////////

void CSphTemplateDictTraits::LoadStopwords ( const char * sFiles, const ISphTokenizer * pTokenizer, bool bStripFile )
{
	assert ( !m_pStopwords );
	assert ( !m_iStopwords );

	// tokenize file list
	if ( !sFiles || !*sFiles )
		return;

	m_dSWFileInfos.Resize ( 0 );

	TokenizerRefPtr_c tTokenizer ( pTokenizer->Clone ( SPH_CLONE_INDEX ) );
	CSphFixedVector<char> dList ( 1+(int)strlen(sFiles) );
	strcpy ( dList.Begin(), sFiles ); // NOLINT

	char * pCur = dList.Begin();
	char * sName = NULL;

	CSphVector<SphWordID_t> dStop;

	while (true)
	{
		// find next name start
		while ( *pCur && ( isspace(*pCur) || *pCur==',' ) ) pCur++;
		if ( !*pCur ) break;
		sName = pCur;

		// find next name end
		while ( *pCur && !( isspace(*pCur) || *pCur==',' ) ) pCur++;
		if ( *pCur ) *pCur++ = '\0';

		CSphString sFileName = sName;
		bool bGotFile = sphIsReadable ( sFileName );
		if ( !bGotFile )
		{
			if ( bStripFile )
			{
				StripPath ( sFileName );
				bGotFile = sphIsReadable ( sFileName );
			}
			if ( !bGotFile )
			{
				if ( !bStripFile )
					StripPath ( sFileName );
				sFileName.SetSprintf ( "%s/stopwords/%s", GET_FULL_SHARE_DIR(), sFileName.cstr() );
				bGotFile = sphIsReadable ( sFileName );
			}
		}

		CSphFixedVector<BYTE> dBuffer ( 0 );
		CSphSavedFile tInfo;
		tInfo.Collect ( sFileName.cstr() );

		// need to store original name to compatible with original behavior of load order
		// from path defined; from tool CWD; from SHARE_DIR
		tInfo.m_sFilename = sName; 
		m_dSWFileInfos.Add ( tInfo );

		if ( !bGotFile )
		{
			StringBuilder_c sError;
			sError.Appendf ( "failed to load stopwords from either '%s' or '%s'", sName, sFileName.cstr() );
			if ( bStripFile )
				sError += ", current work directory";
			sphWarn ( "%s", sError.cstr() );
			continue;
		}

		// open file
		FILE * fp = fopen ( sFileName.cstr(), "rb" );
		if ( !fp )
		{
			sphWarn ( "failed to load stopwords from '%s'", sFileName.cstr() );
			continue;
		}

		struct_stat st = {0};
		if ( fstat ( fileno (fp) , &st )==0 )
			dBuffer.Reset ( st.st_size );
		else
		{
			fclose ( fp );
			sphWarn ( "stopwords: failed to get file size for '%s'", sFileName.cstr() );
			continue;
		}

		// tokenize file
		int iLength = (int)fread ( dBuffer.Begin(), 1, (size_t)st.st_size, fp );

		BYTE * pToken;
		tTokenizer->SetBuffer ( dBuffer.Begin(), iLength );
		while ( ( pToken = tTokenizer->GetToken() )!=NULL )
			if ( m_tSettings.m_bStopwordsUnstemmed )
				dStop.Add ( GetWordIDNonStemmed ( pToken ) );
			else
				dStop.Add ( GetWordID ( pToken ) );

		// close file
		fclose ( fp );
	}

	// sort stopwords
	dStop.Uniq();

	// store IDs
	if ( dStop.GetLength() )
	{
		m_dStopwordContainer.Reset ( dStop.GetLength() );
		ARRAY_FOREACH ( i, dStop )
			m_dStopwordContainer[i] = dStop[i];

		m_iStopwords = m_dStopwordContainer.GetLength ();
		m_pStopwords = m_dStopwordContainer.Begin();
	}
}


void CSphTemplateDictTraits::LoadStopwords ( const CSphVector<SphWordID_t> & dStopwords )
{
	m_dStopwordContainer.Reset ( dStopwords.GetLength() );
	ARRAY_FOREACH ( i, dStopwords )
		m_dStopwordContainer[i] = dStopwords[i];

	m_iStopwords = m_dStopwordContainer.GetLength ();
	m_pStopwords = m_dStopwordContainer.Begin();
}


void CSphTemplateDictTraits::WriteStopwords ( CSphWriter & tWriter ) const
{
	tWriter.PutDword ( (DWORD)m_iStopwords );
	for ( int i = 0; i < m_iStopwords; ++i )
		tWriter.ZipOffset ( m_pStopwords[i] );
}

void CSphTemplateDictTraits::WriteStopwords ( JsonEscapedBuilder& tOut ) const
{
	if ( !m_iStopwords )
		return;

	tOut.Named ( "stopwords_list" );
	auto _ = tOut.Array();
	for ( int i = 0; i < m_iStopwords; ++i )
		tOut << cast2signed ( m_pStopwords[i] );
}


void CSphTemplateDictTraits::SweepWordformContainers ( const CSphVector<CSphSavedFile> & dFiles )
{
	for ( int i = 0; i < m_dWordformContainers.GetLength (); )
	{
		CSphWordforms * WC = m_dWordformContainers[i];
		if ( WC->m_iRefCount==0 && !WC->IsEqual ( dFiles ) )
		{
			delete WC;
			m_dWordformContainers.Remove ( i );
		} else
			++i;
	}
}


static const int MAX_REPORT_LEN = 1024;

static void AddStringToReport ( CSphString & sReport, const CSphString & sString, bool bLast )
{
	int iLen = sReport.Length();
	if ( iLen + sString.Length() + 2 > MAX_REPORT_LEN )
		return;

	char * szReport = const_cast<char *>( sReport.cstr() );
	strcat ( szReport, sString.cstr() );	// NOLINT
	iLen += sString.Length();
	if ( bLast )
		szReport[iLen] = '\0';
	else
	{
		szReport[iLen] = ' ';
		szReport[iLen+1] = '\0';
	}
}


static void ConcatReportStrings ( const CSphTightVector<CSphString> & dStrings, CSphString & sReport )
{
	sReport.Reserve ( MAX_REPORT_LEN );
	*const_cast<char *>(sReport.cstr()) = '\0';

	ARRAY_FOREACH ( i, dStrings )
		AddStringToReport ( sReport, dStrings[i], i==dStrings.GetLength()-1 );
}


static void ConcatReportStrings ( const CSphTightVector<CSphNormalForm> & dStrings, CSphString & sReport )
{
	sReport.Reserve ( MAX_REPORT_LEN );
	*const_cast<char *>(sReport.cstr()) = '\0';

	ARRAY_FOREACH ( i, dStrings )
		AddStringToReport ( sReport, dStrings[i].m_sForm, i==dStrings.GetLength()-1 );
}


CSphWordforms * CSphTemplateDictTraits::GetWordformContainer ( const CSphVector<CSphSavedFile> & dFileInfos, const StrVec_t * pEmbedded, const ISphTokenizer * pTokenizer, const char * sIndex )
{
	uint64_t uTokenizerFNV = pTokenizer->GetSettingsFNV();
	ARRAY_FOREACH ( i, m_dWordformContainers )
		if ( m_dWordformContainers[i]->IsEqual ( dFileInfos ) )
		{
			CSphWordforms * pContainer = m_dWordformContainers[i];
			if ( uTokenizerFNV==pContainer->m_uTokenizerFNV )
				return pContainer;

			CSphTightVector<CSphString> dErrorReport;
			ARRAY_FOREACH ( j, dFileInfos )
				dErrorReport.Add ( dFileInfos[j].m_sFilename );

			CSphString sAllFiles;
			ConcatReportStrings ( dErrorReport, sAllFiles );
			sphWarning ( "index '%s': wordforms file '%s' is shared with index '%s', "
				"but tokenizer settings are different",
				sIndex, sAllFiles.cstr(), pContainer->m_sIndexName.cstr() );
		}

	CSphWordforms * pContainer = LoadWordformContainer ( dFileInfos, pEmbedded, pTokenizer, sIndex );
	if ( pContainer )
		m_dWordformContainers.Add ( pContainer );

	return pContainer;
}




void CSphTemplateDictTraits::AddWordform ( CSphWordforms * pContainer, char * sBuffer, int iLen,
	ISphTokenizer * pTokenizer, const char * szFile, const CSphVector<int> & dBlended, int iFileId )
{
	StrVec_t dTokens;

	bool bSeparatorFound = false;
	bool bAfterMorphology = false;

	// parse the line
	pTokenizer->SetBuffer ( (BYTE*)sBuffer, iLen );

	bool bFirstToken = true;
	bool bStopwordsPresent = false;
	bool bCommentedWholeLine = false;

	BYTE * pFrom = nullptr;
	while ( ( pFrom = pTokenizer->GetToken () )!=nullptr )
	{
		if ( *pFrom=='#' )
		{
			bCommentedWholeLine = bFirstToken;
			break;
		}

		if ( *pFrom=='~' && bFirstToken )
		{
			bAfterMorphology = true;
			bFirstToken = false;
			continue;
		}

		bFirstToken = false;

		if ( *pFrom=='>' )
		{
			bSeparatorFound = true;
			break;
		}

		if ( *pFrom=='=' && *pTokenizer->GetBufferPtr()=='>' )
		{
			pTokenizer->GetToken();
			bSeparatorFound = true;
			break;
		}

		if ( GetWordID ( pFrom, (int)strlen ( (const char*)pFrom ), true ) )
			dTokens.Add ( (const char*)pFrom );
		else
			bStopwordsPresent = true;
	}

	if ( !dTokens.GetLength() )
	{
		if ( !bCommentedWholeLine )
			sphWarning ( "index '%s': all source tokens are stopwords (wordform='%s', file='%s'). IGNORED.", pContainer->m_sIndexName.cstr(), sBuffer, szFile );
		return;
	}

	if ( !bSeparatorFound )
	{
		sphWarning ( "index '%s': no wordform separator found (wordform='%s', file='%s'). IGNORED.", pContainer->m_sIndexName.cstr(), sBuffer, szFile );
		return;
	}

	BYTE * pTo = pTokenizer->GetToken ();
	if ( !pTo )
	{
		sphWarning ( "index '%s': no destination token found (wordform='%s', file='%s'). IGNORED.", pContainer->m_sIndexName.cstr(), sBuffer, szFile );
		return;
	}

	if ( *pTo=='#' )
	{
		sphWarning ( "index '%s': misplaced comment (wordform='%s', file='%s'). IGNORED.", pContainer->m_sIndexName.cstr(), sBuffer, szFile );
		return;
	}

	CSphVector<CSphNormalForm> dDestTokens;
	bool bFirstDestIsStop = !GetWordID ( pTo, (int) strlen ( (const char*)pTo ), true );
	CSphNormalForm & tForm = dDestTokens.Add();
	tForm.m_sForm = (const char *)pTo;
	tForm.m_iLengthCP = pTokenizer->GetLastTokenLen();

	// what if we have more than one word in the right part?
	const BYTE * pDestToken;
	while ( ( pDestToken = pTokenizer->GetToken() )!=nullptr )
	{
		bool bStop = ( !GetWordID ( pDestToken, (int) strlen ( (const char*)pDestToken ), true ) );
		if ( !bStop )
		{
			CSphNormalForm & tNewForm = dDestTokens.Add();
			tNewForm.m_sForm = (const char *)pDestToken;
			tNewForm.m_iLengthCP = pTokenizer->GetLastTokenLen();
		}

		bStopwordsPresent |= bStop;
	}

	// we can have wordforms with 1 destination token that is a stopword
	if ( dDestTokens.GetLength()>1 && bFirstDestIsStop )
		dDestTokens.Remove(0);

	if ( !dDestTokens.GetLength() )
	{
		sphWarning ( "index '%s': destination token is a stopword (wordform='%s', file='%s'). IGNORED.", pContainer->m_sIndexName.cstr(), sBuffer, szFile );
		return;
	}

	if ( bStopwordsPresent )
		sphWarning ( "index '%s': wordform contains stopwords (wordform='%s'). Fix your wordforms file '%s'.", pContainer->m_sIndexName.cstr(), sBuffer, szFile );

	// we disabled all blended, so we need to filter them manually
	bool bBlendedPresent = false;
	if ( dBlended.GetLength() )
		ARRAY_FOREACH ( i, dDestTokens )
		{
			int iCode;
			const BYTE * pBuf = (const BYTE *) dDestTokens[i].m_sForm.cstr();
			while ( ( iCode = sphUTF8Decode ( pBuf ) )>0 && !bBlendedPresent )
				bBlendedPresent = ( dBlended.BinarySearch ( iCode )!=nullptr );
		}

	if ( bBlendedPresent )
		sphWarning ( "invalid mapping (destination contains blended characters) (wordform='%s'). Fix your wordforms file '%s'.", sBuffer, szFile );

	if ( bBlendedPresent && dDestTokens.GetLength()>1 )
	{
		sphWarning ( "blended characters are not allowed with multiple destination tokens (wordform='%s', file='%s'). IGNORED.", sBuffer, szFile );
		return;
	}

	if ( dTokens.GetLength()>1 || dDestTokens.GetLength()>1 )
	{
		auto * pMultiWordform = new CSphMultiform;
		pMultiWordform->m_iFileId = iFileId;
		pMultiWordform->m_dNormalForm.Resize ( dDestTokens.GetLength() );
		ARRAY_FOREACH ( i, dDestTokens )
			pMultiWordform->m_dNormalForm[i] = dDestTokens[i];

		for ( int i = 1; i < dTokens.GetLength(); i++ )
			pMultiWordform->m_dTokens.Add ( dTokens[i] );

		if ( !pContainer->m_pMultiWordforms )
			pContainer->m_pMultiWordforms = new CSphMultiformContainer;

		CSphMultiforms ** ppWordforms = pContainer->m_pMultiWordforms->m_Hash ( dTokens[0] );
		if ( ppWordforms )
		{
			auto * pWordforms = *ppWordforms;
			for ( CSphMultiform *pStoredMF : pWordforms->m_pForms )
			{
				if ( pStoredMF->m_dTokens.GetLength()==pMultiWordform->m_dTokens.GetLength() )
				{
					bool bSameTokens = true;
					ARRAY_FOREACH_COND ( iToken, pStoredMF->m_dTokens, bSameTokens )
						if ( pStoredMF->m_dTokens[iToken]!=pMultiWordform->m_dTokens[iToken] )
							bSameTokens = false;

					if ( bSameTokens )
					{
						CSphString sStoredTokens, sStoredForms;
						ConcatReportStrings ( pStoredMF->m_dTokens, sStoredTokens );
						ConcatReportStrings ( pStoredMF->m_dNormalForm, sStoredForms );
						sphWarning ( "index '%s': duplicate wordform found - overridden ( current='%s', old='%s %s > %s' ). Fix your wordforms file '%s'.",
							pContainer->m_sIndexName.cstr(), sBuffer, dTokens[0].cstr(), sStoredTokens.cstr(), sStoredForms.cstr(), szFile );

						pStoredMF->m_dNormalForm.Resize ( pMultiWordform->m_dNormalForm.GetLength() );
						ARRAY_FOREACH ( iForm, pMultiWordform->m_dNormalForm )
							pStoredMF->m_dNormalForm[iForm] = pMultiWordform->m_dNormalForm[iForm];

						pStoredMF->m_iFileId = iFileId;

						SafeDelete ( pMultiWordform );
						break; // otherwise, we crash next turn
					}
				}
			}

			if ( pMultiWordform )
			{
				pWordforms->m_pForms.Add ( pMultiWordform );

				// sort forms by files and length
				// but do not sort if we're loading embedded
				if ( iFileId>=0 )
					pWordforms->m_pForms.Sort ( Lesser ( [] ( const CSphMultiform* pA, const CSphMultiform* pB ) {
						assert ( pA && pB );
						return ( pA->m_iFileId == pB->m_iFileId ) ? pA->m_dTokens.GetLength() > pB->m_dTokens.GetLength() : pA->m_iFileId > pB->m_iFileId;
					} ) );

				pWordforms->m_iMinTokens = Min ( pWordforms->m_iMinTokens, pMultiWordform->m_dTokens.GetLength () );
				pWordforms->m_iMaxTokens = Max ( pWordforms->m_iMaxTokens, pMultiWordform->m_dTokens.GetLength () );
				pContainer->m_pMultiWordforms->m_iMaxTokens = Max ( pContainer->m_pMultiWordforms->m_iMaxTokens, pWordforms->m_iMaxTokens );
			}
		} else
		{
			auto * pNewWordforms = new CSphMultiforms;
			pNewWordforms->m_pForms.Add ( pMultiWordform );
			pNewWordforms->m_iMinTokens = pMultiWordform->m_dTokens.GetLength ();
			pNewWordforms->m_iMaxTokens = pMultiWordform->m_dTokens.GetLength ();
			pContainer->m_pMultiWordforms->m_iMaxTokens = Max ( pContainer->m_pMultiWordforms->m_iMaxTokens, pNewWordforms->m_iMaxTokens );
			pContainer->m_pMultiWordforms->m_Hash.Add ( pNewWordforms, dTokens[0] );
		}

		// let's add destination form to regular wordform to keep destination from being stemmed
		// FIXME!!! handle multiple destination tokens and ~flag for wordforms
		if ( !bAfterMorphology && dDestTokens.GetLength()==1 && !pContainer->m_hHash.Exists ( dDestTokens[0].m_sForm ) )
		{
			CSphStoredNF tStoredForm;
			tStoredForm.m_sWord = dDestTokens[0].m_sForm;
			tStoredForm.m_bAfterMorphology = bAfterMorphology;
			pContainer->m_bHavePostMorphNF |= bAfterMorphology;
			if ( !pContainer->m_dNormalForms.GetLength()
				|| pContainer->m_dNormalForms.Last().m_sWord!=dDestTokens[0].m_sForm
				|| pContainer->m_dNormalForms.Last().m_bAfterMorphology!=bAfterMorphology )
				pContainer->m_dNormalForms.Add ( tStoredForm );

			pContainer->m_hHash.Add ( pContainer->m_dNormalForms.GetLength()-1, dDestTokens[0].m_sForm );
		}
	} else
	{
		if ( bAfterMorphology )
		{
			BYTE pBuf [16+3*SPH_MAX_WORD_LEN];
			memcpy ( pBuf, dTokens[0].cstr(), dTokens[0].Length()+1 );
			ApplyStemmers ( pBuf );
			dTokens[0] = (char *)pBuf;
		}

		// check wordform that source token is a new token or has same destination token
		int * pRefTo = pContainer->m_hHash ( dTokens[0] );
		assert ( !pRefTo || ( *pRefTo>=0 && *pRefTo<pContainer->m_dNormalForms.GetLength() ) );
		if ( pRefTo )
		{
			// replace with a new wordform
			if ( pContainer->m_dNormalForms[*pRefTo].m_sWord!=dDestTokens[0].m_sForm || pContainer->m_dNormalForms[*pRefTo].m_bAfterMorphology!=bAfterMorphology )
			{
				CSphStoredNF & tRefTo = pContainer->m_dNormalForms[*pRefTo];
				sphWarning ( "index '%s': duplicate wordform found - overridden ( current='%s', old='%s%s > %s' ). Fix your wordforms file '%s'.",
					pContainer->m_sIndexName.cstr(), sBuffer, tRefTo.m_bAfterMorphology ? "~" : "", dTokens[0].cstr(), tRefTo.m_sWord.cstr(), szFile );

				tRefTo.m_sWord = dDestTokens[0].m_sForm;
				tRefTo.m_bAfterMorphology = bAfterMorphology;
				pContainer->m_bHavePostMorphNF |= bAfterMorphology;
			} else
				sphWarning ( "index '%s': duplicate wordform found ( '%s' ). Fix your wordforms file '%s'.", pContainer->m_sIndexName.cstr(), sBuffer, szFile );
		} else
		{
			CSphStoredNF tStoredForm;
			tStoredForm.m_sWord = dDestTokens[0].m_sForm;
			tStoredForm.m_bAfterMorphology = bAfterMorphology;
			pContainer->m_bHavePostMorphNF |= bAfterMorphology;
			if ( !pContainer->m_dNormalForms.GetLength()
				|| pContainer->m_dNormalForms.Last().m_sWord!=dDestTokens[0].m_sForm
				|| pContainer->m_dNormalForms.Last().m_bAfterMorphology!=bAfterMorphology)
				pContainer->m_dNormalForms.Add ( tStoredForm );

			pContainer->m_hHash.Add ( pContainer->m_dNormalForms.GetLength()-1, dTokens[0] );
		}
	}
}


CSphWordforms * CSphTemplateDictTraits::LoadWordformContainer ( const CSphVector<CSphSavedFile> & dFileInfos,
	const StrVec_t * pEmbeddedWordforms, const ISphTokenizer * pTokenizer, const char * sIndex )
{
	// allocate it
	auto * pContainer = new CSphWordforms();
	pContainer->m_dFiles = dFileInfos;
	pContainer->m_uTokenizerFNV = pTokenizer->GetSettingsFNV();
	pContainer->m_sIndexName = sIndex;

	TokenizerRefPtr_c pMyTokenizer ( pTokenizer->Clone ( SPH_CLONE_INDEX ) );
	const CSphTokenizerSettings & tSettings = pMyTokenizer->GetSettings();
	CSphVector<int> dBlended;

	// get a list of blend chars and set add them to the tokenizer as simple chars
	if ( tSettings.m_sBlendChars.Length() )
	{
		CSphVector<char> dNewCharset;
		dNewCharset.Append ( tSettings.m_sCaseFolding.cstr (), tSettings.m_sCaseFolding.Length ());

		CSphVector<CSphRemapRange> dRemaps;
		if ( sphParseCharset ( tSettings.m_sBlendChars.cstr(), dRemaps ) )
			ARRAY_FOREACH ( i, dRemaps )
				for ( int j = dRemaps[i].m_iStart; j<=dRemaps[i].m_iEnd; ++j )
				{
					dNewCharset.Add ( ',' );
					dNewCharset.Add ( ' ' );
					dNewCharset.Add ( char(j) );
					dBlended.Add ( j );
				}

		dNewCharset.Add(0);

		// sort dBlended for binary search
		dBlended.Sort ();

		CSphString sError;
		pMyTokenizer->SetCaseFolding ( dNewCharset.Begin(), sError );

		// disable blend chars
		pMyTokenizer->SetBlendChars ( NULL, sError );
	}

	// add wordform-specific specials
	pMyTokenizer->AddSpecials ( "#=>~" );

	if ( pEmbeddedWordforms )
	{
		CSphTightVector<CSphString> dFilenames;
		dFilenames.Resize ( dFileInfos.GetLength() );
		ARRAY_FOREACH ( i, dFileInfos )
			dFilenames[i] = dFileInfos[i].m_sFilename;

		CSphString sAllFiles;
		ConcatReportStrings ( dFilenames, sAllFiles );

		for ( auto & sWordForm : (*pEmbeddedWordforms) )
			AddWordform ( pContainer, const_cast<char*>( sWordForm.cstr() ), sWordForm.Length(), pMyTokenizer, sAllFiles.cstr(), dBlended, -1 );
	} else
	{
		char sBuffer [ 6*SPH_MAX_WORD_LEN + 512 ]; // enough to hold 2 UTF-8 words, plus some whitespace overhead

		ARRAY_FOREACH ( i, dFileInfos )
		{
			CSphAutoreader rdWordforms;
			const char * szFile = dFileInfos[i].m_sFilename.cstr();
			CSphString sError;
			if ( !rdWordforms.Open ( szFile, sError ) )
			{
				sphWarning ( "index '%s': %s", sIndex, sError.cstr() );
				SafeDelete ( pContainer );
				return nullptr;
			}

			int iLen;
			while ( ( iLen = rdWordforms.GetLine ( sBuffer, sizeof(sBuffer) ) )>=0 )
				AddWordform ( pContainer, sBuffer, iLen, pMyTokenizer, szFile, dBlended, i );
		}
	}

	return pContainer;
}


bool CSphTemplateDictTraits::LoadWordforms ( const StrVec_t & dFiles, const CSphEmbeddedFiles * pEmbedded, const ISphTokenizer * pTokenizer, const char * sIndex )
{
	if ( pEmbedded )
	{
		m_dWFFileInfos.Resize ( pEmbedded->m_dWordformFiles.GetLength() );
		ARRAY_FOREACH ( i, m_dWFFileInfos )
			m_dWFFileInfos[i] = pEmbedded->m_dWordformFiles[i];
	} else
	{
		m_dWFFileInfos.Reserve ( dFiles.GetLength() );
		CSphSavedFile tFile;
		ARRAY_FOREACH ( i, dFiles )
			if ( !dFiles[i].IsEmpty() )
			{
				if ( tFile.Collect ( dFiles[i].cstr() ) )
					m_dWFFileInfos.Add ( tFile );
				else
					sphWarning ( "index '%s': wordforms file '%s' not found", sIndex, dFiles[i].cstr() );
			}
	}

	if ( !m_dWFFileInfos.GetLength() )
		return false;

	SweepWordformContainers ( m_dWFFileInfos );

	m_pWordforms = GetWordformContainer ( m_dWFFileInfos, pEmbedded ? &(pEmbedded->m_dWordforms) : nullptr, pTokenizer, sIndex );
	if ( m_pWordforms )
	{
		m_pWordforms->m_iRefCount++;
		if ( m_pWordforms->m_bHavePostMorphNF && !m_dMorph.GetLength() )
			sphWarning ( "index '%s': wordforms contain post-morphology normal forms, but no morphology was specified", sIndex );
	}

	return !!m_pWordforms;
}


void CSphTemplateDictTraits::WriteWordforms ( CSphWriter & tWriter ) const
{
	if ( !m_pWordforms )
	{
		tWriter.PutDword(0);
		return;
	}

	int nMultiforms = 0;
	if ( m_pWordforms->m_pMultiWordforms )
		for ( const auto& tMF : m_pWordforms->m_pMultiWordforms->m_Hash )
			nMultiforms += tMF.second ? tMF.second->m_pForms.GetLength() : 0;

	tWriter.PutDword ( m_pWordforms->m_hHash.GetLength()+nMultiforms );
	for ( const auto& tForm : m_pWordforms->m_hHash )
	{
		CSphString sLine;
		sLine.SetSprintf ( "%s%s > %s", m_pWordforms->m_dNormalForms[tForm.second].m_bAfterMorphology ? "~" : "",
			tForm.first.cstr(), m_pWordforms->m_dNormalForms[tForm.second].m_sWord.cstr() );
		tWriter.PutString ( sLine );
	}

	if ( m_pWordforms->m_pMultiWordforms )
	{
		CSphMultiformContainer::CSphMultiformHash & tHash = m_pWordforms->m_pMultiWordforms->m_Hash;
		tHash.IterateStart();
		while ( tHash.IterateNext() )
		{
			const CSphString & sKey = tHash.IterateGetKey();
			CSphMultiforms * pMF = tHash.IterateGet();
			if ( !pMF )
				continue;

			ARRAY_FOREACH ( i, pMF->m_pForms )
			{
				CSphString sLine, sTokens, sForms;
				ConcatReportStrings ( pMF->m_pForms[i]->m_dTokens, sTokens );
				ConcatReportStrings ( pMF->m_pForms[i]->m_dNormalForm, sForms );

				sLine.SetSprintf ( "%s %s > %s", sKey.cstr(), sTokens.cstr(), sForms.cstr() );
				tWriter.PutString ( sLine );
			}
		}
	}
}

void CSphTemplateDictTraits::WriteWordforms ( JsonEscapedBuilder & tOut ) const
{
	if ( !m_pWordforms )
		return;

	bool bHaveData = ( m_pWordforms->m_hHash.GetLength() != 0 );

	using HASHIT = std::pair<CSphString, CSphMultiforms*>;
	auto * pMulti = m_pWordforms->m_pMultiWordforms; // shortcut
	if ( pMulti )
		bHaveData |= ::any_of ( pMulti->m_Hash, [] ( const HASHIT& tMF ) { return tMF.second && !tMF.second->m_pForms.IsEmpty(); } );

	if ( !bHaveData )
		return;

	tOut.Named ( "word_forms" );
	auto _ = tOut.ArrayW();

	if ( m_pWordforms->m_hHash.GetLength() )
		for ( const auto& tForm : m_pWordforms->m_hHash )
		{
			CSphString sLine;
			sLine.SetSprintf ( "%s%s > %s", m_pWordforms->m_dNormalForms[tForm.second].m_bAfterMorphology ? "~" : "", tForm.first.cstr(), m_pWordforms->m_dNormalForms[tForm.second].m_sWord.cstr() );
			tOut.FixupSpacedAndAppendEscaped ( sLine.cstr() );
		}

	if ( !pMulti )
		return;

	for ( const HASHIT& tForms : pMulti->m_Hash )
	{
		if ( !tForms.second )
			continue;

		for ( const CSphMultiform* pMF : tForms.second->m_pForms )
		{
			CSphString sLine, sTokens, sForms;
			ConcatReportStrings ( pMF->m_dTokens, sTokens );
			ConcatReportStrings ( pMF->m_dNormalForm, sForms );
			sLine.SetSprintf ( "%s %s > %s", tForms.first.cstr(), sTokens.cstr(), sForms.cstr() );
			tOut.FixupSpacedAndAppendEscaped ( sLine.cstr() );
		}
	}
}


int CSphTemplateDictTraits::SetMorphology ( const char * szMorph, CSphString & sMessage )
{
	m_dMorph.Reset ();
#if WITH_STEMMER
	ARRAY_FOREACH ( i, m_dStemmers )
		sb_stemmer_delete ( m_dStemmers[i] );
	m_dStemmers.Reset ();
#endif

	if ( !szMorph )
		return ST_OK;

	CSphString sOption = szMorph;
	sOption.ToLower ();

	CSphString sError;
	int iRes = ParseMorphology ( sOption.cstr(), sMessage );
	if ( iRes==ST_WARNING && sMessage.IsEmpty() )
		sMessage.SetSprintf ( "invalid morphology option %s; skipped", sOption.cstr() );
	return iRes;
}


bool CSphTemplateDictTraits::HasMorphology() const
{
	return ( m_dMorph.GetLength()>0 );
}


/// common id-based stemmer
bool CSphTemplateDictTraits::StemById ( BYTE * pWord, int iStemmer ) const
{
	char szBuf [ MAX_KEYWORD_BYTES ];

	// safe quick strncpy without (!) padding and with a side of strlen
	char * p = szBuf;
	char * pMax = szBuf + sizeof(szBuf) - 1;
	BYTE * pLastSBS = NULL;
	while ( *pWord && p<pMax )
	{
		pLastSBS = ( *pWord )<0x80 ? pWord : pLastSBS;
		*p++ = *pWord++;
	}
	int iLen = int ( p - szBuf );
	*p = '\0';
	pWord -= iLen;

	switch ( iStemmer )
	{
	case SPH_MORPH_STEM_EN:
		stem_en ( pWord, iLen );
		break;

	case SPH_MORPH_STEM_RU_UTF8:
		// skip stemming in case of SBC at the end of the word
		if ( pLastSBS && ( pLastSBS-pWord+1 )>=iLen )
			break;

		// stem only UTF8 tail
		if ( !pLastSBS )
		{
			stem_ru_utf8 ( (WORD*)pWord );
		} else
		{
			stem_ru_utf8 ( (WORD *)( pLastSBS+1 ) );
		}
		break;

	case SPH_MORPH_STEM_CZ:
		stem_cz ( pWord );
		break;

	case SPH_MORPH_STEM_AR_UTF8:
		stem_ar_utf8 ( pWord );
		break;

	case SPH_MORPH_SOUNDEX:
		stem_soundex ( pWord );
		break;

	case SPH_MORPH_METAPHONE_UTF8:
		stem_dmetaphone ( pWord );
		break;

	case SPH_MORPH_AOTLEMMER_RU_UTF8:
		sphAotLemmatizeRuUTF8 ( pWord );
		break;

	case SPH_MORPH_AOTLEMMER_EN:
		sphAotLemmatize ( pWord, AOT_EN );
		break;

	case SPH_MORPH_AOTLEMMER_DE_UTF8:
		sphAotLemmatizeDeUTF8 ( pWord );
		break;

	case SPH_MORPH_AOTLEMMER_UK:
		sphAotLemmatizeUk ( pWord, m_tLemmatizer.Ptr() );
		break;

	case SPH_MORPH_AOTLEMMER_RU_ALL:
	case SPH_MORPH_AOTLEMMER_EN_ALL:
	case SPH_MORPH_AOTLEMMER_DE_ALL:
	case SPH_MORPH_AOTLEMMER_UK_ALL:
		// do the real work somewhere else
		// this is mostly for warning suppressing and making some features like
		// index_exact_words=1 vs expand_keywords=1 work
		break;

	default:
#if WITH_STEMMER
		if ( iStemmer>=SPH_MORPH_LIBSTEMMER_FIRST && iStemmer<SPH_MORPH_LIBSTEMMER_LAST )
		{
			sb_stemmer * pStemmer = m_dStemmers [iStemmer - SPH_MORPH_LIBSTEMMER_FIRST];
			assert ( pStemmer );

			const sb_symbol * sStemmed = sb_stemmer_stem ( pStemmer, (sb_symbol*)pWord, (int) strlen ( (const char*)pWord ) );
			int iStemmedLen = sb_stemmer_length ( pStemmer );

			memcpy ( pWord, sStemmed, iStemmedLen );
			pWord[iStemmedLen] = '\0';
		} else
			return false;

	break;
#else
		return false;
#endif
	}

	return strcmp ( (char *)pWord, szBuf )!=0;
}

void CSphDiskDictTraits::DictBegin ( CSphAutofile & , CSphAutofile & tDict, int )
{
	m_wrDict.CloseFile ();
	m_wrDict.SetFile ( tDict, NULL, m_sWriterError );
	m_wrDict.PutByte ( 1 );
}

bool CSphDiskDictTraits::DictEnd ( DictHeader_t * pHeader, int, CSphString & sError )
{
	// flush wordlist checkpoints
	pHeader->m_iDictCheckpointsOffset = m_wrDict.GetPos();
	pHeader->m_iDictCheckpoints = m_dCheckpoints.GetLength();
	ARRAY_FOREACH ( i, m_dCheckpoints )
	{
		assert ( m_dCheckpoints[i].m_iWordlistOffset );
		m_wrDict.PutOffset ( m_dCheckpoints[i].m_uWordID );
		m_wrDict.PutOffset ( m_dCheckpoints[i].m_iWordlistOffset );
	}

	// done
	m_wrDict.CloseFile ();
	if ( m_wrDict.IsError() )
		sError = m_sWriterError;
	return !m_wrDict.IsError();
}

void CSphDiskDictTraits::DictEntry ( const CSphDictEntry & tEntry )
{
	assert ( m_iSkiplistBlockSize>0 );

	// insert wordlist checkpoint
	if ( ( m_iEntries % SPH_WORDLIST_CHECKPOINT )==0 )
	{
		if ( m_iEntries ) // but not the 1st entry
		{
			assert ( tEntry.m_iDoclistOffset > m_iLastDoclistPos );
			m_wrDict.ZipInt ( 0 ); // indicate checkpoint
			m_wrDict.ZipOffset ( tEntry.m_iDoclistOffset - m_iLastDoclistPos ); // store last length
		}

		// restart delta coding, once per SPH_WORDLIST_CHECKPOINT entries
		m_iLastWordID = 0;
		m_iLastDoclistPos = 0;

		// begin new wordlist entry
		assert ( m_wrDict.GetPos()<=UINT_MAX );

		CSphWordlistCheckpoint & tCheckpoint = m_dCheckpoints.Add();
		tCheckpoint.m_uWordID = tEntry.m_uWordID;
		tCheckpoint.m_iWordlistOffset = m_wrDict.GetPos();
	}

	assert ( tEntry.m_iDoclistOffset>m_iLastDoclistPos );
	m_wrDict.ZipOffset ( tEntry.m_uWordID - m_iLastWordID ); // FIXME! slow with 32bit wordids
	m_wrDict.ZipOffset ( tEntry.m_iDoclistOffset - m_iLastDoclistPos );

	m_iLastWordID = tEntry.m_uWordID;
	m_iLastDoclistPos = tEntry.m_iDoclistOffset;

	assert ( tEntry.m_iDocs );
	assert ( tEntry.m_iHits );
	m_wrDict.ZipInt ( tEntry.m_iDocs );
	m_wrDict.ZipInt ( tEntry.m_iHits );

	// write skiplist location info, if any
	if ( tEntry.m_iDocs > m_iSkiplistBlockSize )
		m_wrDict.ZipOffset ( tEntry.m_iSkiplistOffset );

	m_iEntries++;
}

void CSphDiskDictTraits::DictEndEntries ( SphOffset_t iDoclistOffset )
{
	assert ( iDoclistOffset>=m_iLastDoclistPos );
	m_wrDict.ZipInt ( 0 ); // indicate checkpoint
	m_wrDict.ZipOffset ( iDoclistOffset - m_iLastDoclistPos ); // store last doclist length
}

//////////////////////////////////////////////////////////////////////////
// KEYWORDS STORING DICTIONARY, INFIX HASH BUILDER
//////////////////////////////////////////////////////////////////////////

template < int SIZE >
struct Infix_t
{
	DWORD m_Data[SIZE];

#ifndef NDEBUG
	BYTE m_TrailingZero { 0 };
#endif

	void Reset ()
	{
		for ( int i=0; i<SIZE; i++ )
			m_Data[i] = 0;
	}

	bool operator == ( const Infix_t<SIZE> & rhs ) const;
};


template<>
bool Infix_t<2>::operator == ( const Infix_t<2> & rhs ) const
{
	return m_Data[0]==rhs.m_Data[0] && m_Data[1]==rhs.m_Data[1];
}


template<>
bool Infix_t<3>::operator == ( const Infix_t<3> & rhs ) const
{
	return m_Data[0]==rhs.m_Data[0] && m_Data[1]==rhs.m_Data[1] && m_Data[2]==rhs.m_Data[2];
}


template<>
bool Infix_t<5>::operator == ( const Infix_t<5> & rhs ) const
{
	return m_Data[0]==rhs.m_Data[0] && m_Data[1]==rhs.m_Data[1] && m_Data[2]==rhs.m_Data[2]
		&& m_Data[3]==rhs.m_Data[3] && m_Data[4]==rhs.m_Data[4];
}


struct InfixIntvec_t
{
public:
	// do not change the order of fields in this union - it matters a lot
	union
	{
		DWORD			m_dData[4];
		struct
		{
			int				m_iDynLen;
			int				m_iDynLimit;
			DWORD *			m_pDynData;
		};
	};

public:
	InfixIntvec_t()
	{
		m_dData[0] = 0;
		m_dData[1] = 0;
		m_dData[2] = 0;
		m_dData[3] = 0;
	}

	~InfixIntvec_t()
	{
		if ( IsDynamic() )
			SafeDeleteArray ( m_pDynData );
	}

	bool IsDynamic() const
	{
		return ( m_dData[0] & 0x80000000UL )!=0;
	}

	void Add ( DWORD uVal )
	{
		if ( !m_dData[0] )
		{
			// empty
			m_dData[0] = uVal | ( 1UL<<24 );

		} else if ( !IsDynamic() )
		{
			// 1..4 static entries
			int iLen = m_dData[0] >> 24;
			DWORD uLast = m_dData [ iLen-1 ] & 0xffffffUL;

			// redundant
			if ( uVal==uLast )
				return;

			// grow static part
			if ( iLen<4 )
			{
				m_dData[iLen] = uVal;
				m_dData[0] = ( m_dData[0] & 0xffffffUL ) | ( ++iLen<<24 );
				return;
			}

			// dynamize
			DWORD * pDyn = new DWORD[16];
			pDyn[0] = m_dData[0] & 0xffffffUL;
			pDyn[1] = m_dData[1];
			pDyn[2] = m_dData[2];
			pDyn[3] = m_dData[3];
			pDyn[4] = uVal;
			m_iDynLen = 0x80000005UL; // dynamic flag, len=5
			m_iDynLimit = 16; // limit=16
			m_pDynData = pDyn;

		} else
		{
			// N dynamic entries
			int iLen = m_iDynLen & 0xffffffUL;
			if ( uVal==m_pDynData[iLen-1] )
				return;
			if ( iLen>=m_iDynLimit )
			{
				m_iDynLimit *= 2;
				DWORD * pNew = new DWORD [ m_iDynLimit ];
				for ( int i=0; i<iLen; i++ )
					pNew[i] = m_pDynData[i];
				SafeDeleteArray ( m_pDynData );
				m_pDynData = pNew;
			}

			m_pDynData[iLen] = uVal;
			m_iDynLen++;
		}
	}

	bool operator == ( const InfixIntvec_t & rhs ) const
	{
		// check dynflag, length, maybe first element
		if ( m_dData[0]!=rhs.m_dData[0] )
			return false;

		// check static data
		if ( !IsDynamic() )
		{
			for ( int i=1; i<(int)(m_dData[0]>>24); i++ )
				if ( m_dData[i]!=rhs.m_dData[i] )
					return false;
			return true;
		}

		// check dynamic data
		const DWORD * a = m_pDynData;
		const DWORD * b = rhs.m_pDynData;
		const DWORD * m = a + ( m_iDynLen & 0xffffffUL );
		while ( a<m )
			if ( *a++!=*b++ )
				return false;
		return true;
	}

public:
	int GetLength() const
	{
		if ( !IsDynamic() )
			return m_dData[0] >> 24;
		return m_iDynLen & 0xffffffUL;
	}

	DWORD operator[] ( int iIndex )const
	{
		if ( !IsDynamic() )
			return m_dData[iIndex] & 0xffffffUL;
		return m_pDynData[iIndex];
	}
};


template < int SIZE >
struct InfixHashEntry_t
{
	Infix_t<SIZE>	m_tKey;		///< key, owned by the hash
	InfixIntvec_t	m_tValue;	///< data, owned by the hash
	int				m_iNext;	///< next entry in hash arena
};


template < int SIZE >
class InfixBuilder_c : public ISphInfixBuilder
{
protected:
	static const int							LENGTH = 1048576;

protected:
	int											m_dHash [ LENGTH ];		///< all the hash entries
	CSphSwapVector < InfixHashEntry_t<SIZE> >	m_dArena;
	CSphVector<InfixBlock_t>					m_dBlocks;
	CSphTightVector<BYTE>						m_dBlocksWords;

public:
				InfixBuilder_c();
	void		AddWord ( const BYTE * pWord, int iWordLength, int iCheckpoint, bool bHasMorphology ) override;
	void		SaveEntries ( CSphWriter & wrDict ) override;
	int64_t		SaveEntryBlocks ( CSphWriter & wrDict ) override;
	int			GetBlocksWordsSize() const override { return m_dBlocksWords.GetLength(); }

protected:
	/// add new entry
	void AddEntry ( const Infix_t<SIZE> & tKey, DWORD uHash, int iCheckpoint )
	{
		uHash &= ( LENGTH-1 );

		int iEntry = m_dArena.GetLength();
		InfixHashEntry_t<SIZE> & tNew = m_dArena.Add();
		tNew.m_tKey = tKey;
		tNew.m_tValue.m_dData[0] = 0x1000000UL | iCheckpoint; // len=1, data=iCheckpoint
		tNew.m_iNext = m_dHash[uHash];
		m_dHash[uHash] = iEntry;
	}

	/// get value pointer by key
	InfixIntvec_t * LookupEntry ( const Infix_t<SIZE> & tKey, DWORD uHash )
	{
		uHash &= ( LENGTH-1 );
		int iEntry = m_dHash [ uHash ];
		int iiEntry = 0;

		while ( iEntry )
		{
			if ( m_dArena[iEntry].m_tKey==tKey )
			{
				// mtf it, if needed
				if ( iiEntry )
				{
					m_dArena[iiEntry].m_iNext = m_dArena[iEntry].m_iNext;
					m_dArena[iEntry].m_iNext = m_dHash[uHash];
					m_dHash[uHash] = iEntry;
				}
				return &m_dArena[iEntry].m_tValue;
			}
			iiEntry = iEntry;
			iEntry = m_dArena[iEntry].m_iNext;
		}
		return NULL;
	}
};


template < int SIZE >
InfixBuilder_c<SIZE>::InfixBuilder_c()
{
	// init the hash
	for ( int i=0; i<LENGTH; i++ )
		m_dHash[i] = 0;
	m_dArena.Reserve ( 1048576 );
	m_dArena.Resize ( 1 ); // 0 is a reserved index
}


/// single-byte case, 2-dword infixes
template<>
void InfixBuilder_c<2>::AddWord ( const BYTE * pWord, int iWordLength, int iCheckpoint, bool bHasMorphology )
{
	if ( bHasMorphology && *pWord!=MAGIC_WORD_HEAD_NONSTEMMED )
		return;

	if ( *pWord<0x20 ) // skip heading magic chars, like NONSTEMMED maker
	{
		pWord++;
		iWordLength--;
	}

	Infix_t<2> sKey;
	for ( int p=0; p<=iWordLength-2; p++ )
	{
		sKey.Reset();

		BYTE * pKey = (BYTE*)sKey.m_Data;
		const BYTE * s = pWord + p;
		const BYTE * sMax = s + Min ( 6, iWordLength-p );

		DWORD uHash = 0xffffffUL ^ g_dSphinxCRC32 [ 0xff ^ *s ];
		*pKey++ = *s++; // copy first infix byte

		while ( s<sMax )
		{
			uHash = (uHash >> 8) ^ g_dSphinxCRC32 [ (uHash ^ *s) & 0xff ];
			*pKey++ = *s++; // copy another infix byte

			InfixIntvec_t * pVal = LookupEntry ( sKey, uHash );
			if ( pVal )
				pVal->Add ( iCheckpoint );
			else
				AddEntry ( sKey, uHash, iCheckpoint );
		}
	}
}

static int Utf8CodeLen ( BYTE iCode )
{
	if ( !iCode )
		return 0;

	// check for 7-bit case
	if ( iCode<128 )
		return 1;

	// get number of bytes
	int iBytes = 0;
	while ( iCode & 0x80 )
	{
		iBytes++;
		iCode <<= 1;
	}

	return iBytes;
}

/// UTF-8 case, 3/5-dword infixes
template < int SIZE >
void InfixBuilder_c<SIZE>::AddWord ( const BYTE * pWord, int iWordLength, int iCheckpoint, bool bHasMorphology )
{
	if ( bHasMorphology && *pWord!=MAGIC_WORD_HEAD_NONSTEMMED )
		return;

	if ( *pWord<0x20 ) // skip heading magic chars, like NONSTEMMED maker
	{
		pWord++;
		iWordLength--;
	}

	const BYTE * pWordMax = pWord+iWordLength;
#ifndef NDEBUG
	bool bInvalidTailCp = false;
#endif
	int iCodes = 0; // codepoints in current word
	BYTE dBytes[SPH_MAX_WORD_LEN+1]; // byte offset for each codepoints

	// build an offsets table into the bytestring
	dBytes[0] = 0;
	for ( const BYTE * p = pWord; p<pWordMax && iCodes<SPH_MAX_WORD_LEN; )
	{
		int iLen = 0;
		BYTE uVal = *p;
		while ( uVal & 0x80 )
		{
			uVal <<= 1;
			iLen++;
		}
		if ( !iLen )
			iLen = 1;
		
		// break on tail cut codepoint
		if ( p+iLen>pWordMax )
		{
#ifndef NDEBUG
			bInvalidTailCp = true;
#endif
			break;
		}

		// skip word with large codepoints
		if ( iLen>SIZE )
			return;

		assert ( iLen>=1 && iLen<=4 );
		p += iLen;

		dBytes[iCodes+1] = dBytes[iCodes] + (BYTE)iLen;
		iCodes++;
	}
	assert ( pWord[dBytes[iCodes]]==0 || iCodes==SPH_MAX_WORD_LEN || bInvalidTailCp );

	// generate infixes
	Infix_t<SIZE> sKey;
	for ( int p=0; p<=iCodes-2; p++ )
	{
		sKey.Reset();
		BYTE * pKey = (BYTE*)sKey.m_Data;
		const BYTE * pKeyMax = pKey+sizeof(sKey.m_Data);

		const BYTE * s = pWord + dBytes[p];
		const BYTE * sMax = pWord + dBytes[ p+Min ( 6, iCodes-p ) ];

		// copy first infix codepoint
		DWORD uHash = 0xffffffffUL;
		do
		{
			uHash = (uHash >> 8) ^ g_dSphinxCRC32 [ (uHash ^ *s) & 0xff ];
			*pKey++ = *s++;
		} while ( ( *s & 0xC0 )==0x80 );

		assert ( s - ( pWord + dBytes[p] )==(dBytes[p+1] - dBytes[p]) );

		while ( s<sMax && pKey<pKeyMax && pKey+Utf8CodeLen ( *s )<=pKeyMax )
		{
			// copy next infix codepoint
			do
			{
				uHash = (uHash >> 8) ^ g_dSphinxCRC32 [ (uHash ^ *s) & 0xff ];
				*pKey++ = *s++;
			} while ( ( *s & 0xC0 )==0x80 && pKey<pKeyMax );

			assert ( sphUTF8Len ( (const char *)sKey.m_Data, sizeof(sKey.m_Data) )>=2 );

			InfixIntvec_t * pVal = LookupEntry ( sKey, uHash );
			if ( pVal )
				pVal->Add ( iCheckpoint );
			else
				AddEntry ( sKey, uHash, iCheckpoint );
		}

		assert ( (size_t)( pKey-(BYTE*)sKey.m_Data )<=int(sizeof(sKey.m_Data)) );
	}
}


template < int SIZE >
struct InfixHashCmp_fn
{
	InfixHashEntry_t<SIZE> * m_pBase;

	explicit InfixHashCmp_fn ( InfixHashEntry_t<SIZE> * pBase )
		: m_pBase ( pBase )
	{}

	bool IsLess ( int a, int b ) const
	{
		return strncmp ( (const char*)m_pBase[a].m_tKey.m_Data, (const char*)m_pBase[b].m_tKey.m_Data, sizeof(DWORD)*SIZE )<0;
	}
};


static inline int ZippedIntSize ( DWORD v )
{
	if ( v < (1UL<<7) )
		return 1;
	if ( v < (1UL<<14) )
		return 2;
	if ( v < (1UL<<21) )
		return 3;
	if ( v < (1UL<<28) )
		return 4;
	return 5;
}


const char * g_sTagInfixEntries = "infix-entries";

template < int SIZE >
void InfixBuilder_c<SIZE>::SaveEntries ( CSphWriter & wrDict )
{
	// intentionally local to this function
	// we mark the block end with an editcode of 0
	const int INFIX_BLOCK_SIZE = 64;

	wrDict.PutBytes ( g_sTagInfixEntries, strlen ( g_sTagInfixEntries ) );

	CSphVector<int> dIndex;
	dIndex.Resize ( m_dArena.GetLength()-1 );
	for ( int i=0; i<m_dArena.GetLength()-1; i++ )
		dIndex[i] = i+1;

	InfixHashCmp_fn<SIZE> fnCmp ( m_dArena.Begin() );
	dIndex.Sort ( fnCmp );

	m_dBlocksWords.Reserve ( m_dArena.GetLength()/INFIX_BLOCK_SIZE*sizeof(DWORD)*SIZE );
	int iBlock = 0;
	int iPrevKey = -1;
	ARRAY_FOREACH ( iIndex, dIndex )
	{
		InfixIntvec_t & dData = m_dArena[dIndex[iIndex]].m_tValue;
		const BYTE * sKey = (const BYTE*) m_dArena[dIndex[iIndex]].m_tKey.m_Data;
		int iChars = ( SIZE==2 )
			? (int) strnlen ( (const char*)sKey, sizeof(DWORD)*SIZE )
			: sphUTF8Len ( (const char*)sKey, (int) sizeof(DWORD)*SIZE );
		assert ( iChars>=2 && iChars<int(1 + sizeof ( Infix_t<SIZE> ) ) );

		// keep track of N-infix blocks
		auto iAppendBytes = (int) strnlen ( (const char*)sKey, sizeof(DWORD)*SIZE );
		if ( !iBlock )
		{
			int iOff = m_dBlocksWords.GetLength();
			m_dBlocksWords.Resize ( iOff+iAppendBytes+1 );

			InfixBlock_t & tBlock = m_dBlocks.Add();
			tBlock.m_iInfixOffset = iOff;
			tBlock.m_iOffset = (DWORD)wrDict.GetPos();

			memcpy ( m_dBlocksWords.Begin()+iOff, sKey, iAppendBytes );
			m_dBlocksWords[iOff+iAppendBytes] = '\0';
		}

		// compute max common prefix
		// edit_code = ( num_keep_chars<<4 ) + num_append_chars
		int iEditCode = iChars;
		if ( iPrevKey>=0 )
		{
			const BYTE * sPrev = (const BYTE*) m_dArena[dIndex[iPrevKey]].m_tKey.m_Data;
			const BYTE * sCur = (const BYTE*) sKey;
			const BYTE * sMax = sCur + iAppendBytes;

			int iKeepChars = 0;
			if_const ( SIZE==2 )
			{
				// SBCS path
				while ( sCur<sMax && *sCur && *sCur==*sPrev )
				{
					sCur++;
					sPrev++;
				}
				iKeepChars = (int)( sCur- ( const BYTE* ) sKey );

				assert ( iKeepChars>=0 && iKeepChars<16 );
				assert ( iChars-iKeepChars>=0 );
				assert ( iChars-iKeepChars<16 );

				iEditCode = ( iKeepChars<<4 ) + ( iChars-iKeepChars );
				iAppendBytes = ( iChars-iKeepChars );
				sKey = sCur;

			} else
			{
				// UTF-8 path
				const BYTE * sKeyMax = sCur; // track max matching sPrev prefix in [sKey,sKeyMax)
				while ( sCur<sMax && *sCur && *sCur==*sPrev )
				{
					// current byte matches, move the pointer
					sCur++;
					sPrev++;

					// tricky bit
					// if the next (!) byte is a valid UTF-8 char start (or eof!)
					// then we just matched not just a byte, but a full char
					// so bump the matching prefix boundary and length
					if ( sCur>=sMax || ( *sCur & 0xC0 )!=0x80 )
					{
						sKeyMax = sCur;
						iKeepChars++;
					}
				}

				assert ( iKeepChars>=0 && iKeepChars<16 );
				assert ( iChars-iKeepChars>=0 );
				assert ( iChars-iKeepChars<16 );

				iEditCode = ( iKeepChars<<4 ) + ( iChars-iKeepChars );
				iAppendBytes -= (int)( sKeyMax-sKey );
				sKey = sKeyMax;
			}
		}

		// write edit code, postfix
		wrDict.PutByte ( (BYTE)iEditCode );
		wrDict.PutBytes ( sKey, iAppendBytes );

		// compute data length
		int iDataLen = ZippedIntSize ( dData[0] );
		for ( int j=1; j<dData.GetLength(); j++ )
			iDataLen += ZippedIntSize ( dData[j] - dData[j-1] );

		// write data length, data
		wrDict.ZipInt ( iDataLen );
		wrDict.ZipInt ( dData[0] );
		for ( int j=1; j<dData.GetLength(); j++ )
			wrDict.ZipInt ( dData[j] - dData[j-1] );

		// mark block end, restart deltas
		iPrevKey = iIndex;
		if ( ++iBlock==INFIX_BLOCK_SIZE )
		{
			iBlock = 0;
			iPrevKey = -1;
			wrDict.PutByte ( 0 );
		}
	}

	// put end marker
	if ( iBlock )
		wrDict.PutByte ( 0 );

	const char * pBlockWords = (const char *)m_dBlocksWords.Begin();
	ARRAY_FOREACH ( i, m_dBlocks )
		m_dBlocks[i].m_sInfix = pBlockWords+m_dBlocks[i].m_iInfixOffset;

	if ( wrDict.GetPos()>UINT_MAX ) // FIXME!!! change to int64
		sphDie ( "INTERNAL ERROR: dictionary size " INT64_FMT " overflow at infix save", wrDict.GetPos() );
}


const char * g_sTagInfixBlocks = "infix-blocks";

template < int SIZE >
int64_t InfixBuilder_c<SIZE>::SaveEntryBlocks ( CSphWriter & wrDict )
{
	// save the blocks
	wrDict.PutBytes ( g_sTagInfixBlocks, strlen ( g_sTagInfixBlocks ) );

	SphOffset_t iInfixBlocksOffset = wrDict.GetPos();
	assert ( iInfixBlocksOffset<=INT_MAX );

	wrDict.ZipInt ( m_dBlocks.GetLength() );
	ARRAY_FOREACH ( i, m_dBlocks )
	{
		auto iBytes = strlen ( m_dBlocks[i].m_sInfix );
		wrDict.PutByte ( BYTE(iBytes) );
		wrDict.PutBytes ( m_dBlocks[i].m_sInfix, iBytes );
		wrDict.ZipInt ( m_dBlocks[i].m_iOffset ); // maybe delta these on top?
	}

	return iInfixBlocksOffset;
}


ISphInfixBuilder * sphCreateInfixBuilder ( int iCodepointBytes, CSphString * pError )
{
	assert ( pError );
	*pError = CSphString();
	switch ( iCodepointBytes )
	{
	case 0:		return NULL;
	case 1:		return new InfixBuilder_c<2>(); // upto 6x1 bytes, 2 dwords, sbcs
	case 2:		return new InfixBuilder_c<3>(); // upto 6x2 bytes, 3 dwords, utf-8
	case 3:		return new InfixBuilder_c<5>(); // upto 6x3 bytes, 5 dwords, utf-8
	default:	pError->SetSprintf ( "unhandled max infix codepoint size %d", iCodepointBytes ); return NULL;
	}
}

//////////////////////////////////////////////////////////////////////////
// KEYWORDS STORING DICTIONARY
//////////////////////////////////////////////////////////////////////////

class CSphDictKeywords final : public CSphDictCRC<true>
{
public:
	// OPTIMIZE? change pointers to 8:24 locators to save RAM on x64 gear?
	struct HitblockKeyword_t
	{
		SphWordID_t					m_uWordid;			// locally unique word id (crc value, adjusted in case of collsion)
		HitblockKeyword_t *			m_pNextHash;		// next hashed entry
		char *						m_pKeyword;			// keyword
	};

	struct HitblockException_t
	{
		HitblockKeyword_t *			m_pEntry;			// hash entry
		SphWordID_t					m_uCRC;				// original unadjusted crc

		bool operator < ( const HitblockException_t & rhs ) const
		{
			return m_pEntry->m_uWordid < rhs.m_pEntry->m_uWordid;
		}
	};

	struct DictKeyword_t
	{
		char *						m_sKeyword;
		SphOffset_t					m_uOff;
		int							m_iDocs;
		int							m_iHits;
		BYTE						m_uHint;
		int64_t						m_iSkiplistPos;		///< position in .spe file
	};

	struct DictBlock_t
	{
		SphOffset_t					m_iPos;
		int							m_iLen;
	};

public:
	explicit		CSphDictKeywords ();

	void			HitblockBegin () final { m_bHitblock = true; }
	void			HitblockPatch ( CSphWordHit * pHits, int iHits ) const final;
	const char *	HitblockGetKeyword ( SphWordID_t uWordID ) final;
	int				HitblockGetMemUse () final { return m_iMemUse; }
	void			HitblockReset () final;

	void			DictBegin ( CSphAutofile & tTempDict, CSphAutofile & tDict, int iDictLimit ) final;
	void			DictEntry ( const CSphDictEntry & tEntry ) final;
	void			DictEndEntries ( SphOffset_t ) final {}
	bool			DictEnd ( DictHeader_t * pHeader, int iMemLimit, CSphString & sError ) final;

	SphWordID_t		GetWordID ( BYTE * pWord ) final;
	SphWordID_t		GetWordIDWithMarkers ( BYTE * pWord ) final;
	SphWordID_t		GetWordIDNonStemmed ( BYTE * pWord ) final;
	SphWordID_t		GetWordID ( const BYTE * pWord, int iLen, bool bFilterStops ) final;
	CSphDict *		Clone () const final { return CloneBase ( new CSphDictKeywords() ); }

protected:
					~CSphDictKeywords () final;

private:
	struct DictKeywordTagged_t : public DictKeyword_t
	{
		int m_iBlock;

		static inline bool IsLess ( const DictKeywordTagged_t & a, const DictKeywordTagged_t & b );
	};

	static const int				SLOTS			= 65536;
	static const int				ENTRY_CHUNK		= 65536;
	static const int				KEYWORD_CHUNK	= 1048576;
	static const int				DICT_CHUNK		= 65536;

	HitblockKeyword_t *				m_dHash [ SLOTS ];	///< hash by wordid (!)
	CSphVector<HitblockException_t>	m_dExceptions;

	bool							m_bHitblock;		///< should we store words on GetWordID or not
	int								m_iMemUse;			///< current memory use by all the chunks
	int								m_iDictLimit;		///< allowed memory limit for dict block collection

	CSphVector<HitblockKeyword_t*>	m_dEntryChunks;		///< hash chunks, only used when indexing hitblocks
	HitblockKeyword_t *				m_pEntryChunk;
	int								m_iEntryChunkFree;

	CSphVector<BYTE*>				m_dKeywordChunks;	///< keyword storage
	BYTE *							m_pKeywordChunk;
	int								m_iKeywordChunkFree;

	CSphVector<DictKeyword_t*>		m_dDictChunks;		///< dict entry chunks, only used when sorting final dict
	DictKeyword_t *					m_pDictChunk;
	int								m_iDictChunkFree;

	int								m_iTmpFD;			///< temp dict file descriptor
	CSphWriter						m_wrTmpDict;		///< temp dict writer
	CSphVector<DictBlock_t>			m_dDictBlocks;		///< on-disk locations of dict entry blocks

	char							m_sClippedWord[MAX_KEYWORD_BYTES]; ///< keyword storage for cliiped word

private:
	SphWordID_t				HitblockGetID ( const char * pWord, int iLen, SphWordID_t uCRC );
	HitblockKeyword_t *		HitblockAddKeyword ( DWORD uHash, const char * pWord, int iLen, SphWordID_t uID );

	void					DictReadEntry ( CSphBin & dBin, DictKeywordTagged_t & tEntry, BYTE * pKeyword );
	void					DictFlush ();
};

//////////////////////////////////////////////////////////////////////////

CSphDictKeywords::CSphDictKeywords ()
	: m_bHitblock ( false )
	, m_iMemUse ( 0 )
	, m_iDictLimit ( 0 )
	, m_pEntryChunk ( NULL )
	, m_iEntryChunkFree ( 0 )
	, m_pKeywordChunk ( NULL )
	, m_iKeywordChunkFree ( 0 )
	, m_pDictChunk ( NULL )
	, m_iDictChunkFree ( 0 )
	, m_iTmpFD ( -1 )
{
	memset ( m_dHash, 0, sizeof(m_dHash) );
}

CSphDictKeywords::~CSphDictKeywords ()
{
	HitblockReset();
}

void CSphDictKeywords::HitblockReset()
{
	m_dExceptions.Resize ( 0 );

	ARRAY_FOREACH ( i, m_dEntryChunks )
		SafeDeleteArray ( m_dEntryChunks[i] );
	m_dEntryChunks.Resize ( 0 );
	m_pEntryChunk = NULL;
	m_iEntryChunkFree = 0;

	ARRAY_FOREACH ( i, m_dKeywordChunks )
		SafeDeleteArray ( m_dKeywordChunks[i] );
	m_dKeywordChunks.Resize ( 0 );
	m_pKeywordChunk = NULL;
	m_iKeywordChunkFree = 0;

	m_iMemUse = 0;

	memset ( m_dHash, 0, sizeof(m_dHash) );
}

CSphDictKeywords::HitblockKeyword_t * CSphDictKeywords::HitblockAddKeyword ( DWORD uHash, const char * sWord, int iLen, SphWordID_t uID )
{
	assert ( iLen<MAX_KEYWORD_BYTES );

	// alloc entry
	if ( !m_iEntryChunkFree )
	{
		m_pEntryChunk = new HitblockKeyword_t [ ENTRY_CHUNK ];
		m_iEntryChunkFree = ENTRY_CHUNK;
		m_dEntryChunks.Add ( m_pEntryChunk );
		m_iMemUse += sizeof(HitblockKeyword_t)*ENTRY_CHUNK;
	}
	HitblockKeyword_t * pEntry = m_pEntryChunk++;
	m_iEntryChunkFree--;

	// alloc keyword
	iLen++;
	if ( m_iKeywordChunkFree < iLen )
	{
		m_pKeywordChunk = new BYTE [ KEYWORD_CHUNK ];
		m_iKeywordChunkFree = KEYWORD_CHUNK;
		m_dKeywordChunks.Add ( m_pKeywordChunk );
		m_iMemUse += KEYWORD_CHUNK;
	}

	// fill it
	memcpy ( m_pKeywordChunk, sWord, iLen );
	m_pKeywordChunk[iLen-1] = '\0';
	pEntry->m_pKeyword = (char*)m_pKeywordChunk;
	pEntry->m_uWordid = uID;
	m_pKeywordChunk += iLen;
	m_iKeywordChunkFree -= iLen;

	// mtf it
	pEntry->m_pNextHash = m_dHash [ uHash ];
	m_dHash [ uHash ] = pEntry;

	return pEntry;
}

SphWordID_t CSphDictKeywords::HitblockGetID ( const char * sWord, int iLen, SphWordID_t uCRC )
{
	if ( iLen>MAX_KEYWORD_BYTES-4 ) // fix of very long word (zones)
	{
		memcpy ( m_sClippedWord, sWord, MAX_KEYWORD_BYTES-4 );
		memset ( m_sClippedWord+MAX_KEYWORD_BYTES-4, 0, 4 );

		CSphString sOrig;
		sOrig.SetBinary ( sWord, iLen );
		sphWarn ( "word overrun buffer, clipped!!!\n"
			"clipped (len=%d, word='%s')\noriginal (len=%d, word='%s')",
			MAX_KEYWORD_BYTES-4, m_sClippedWord, iLen, sOrig.cstr() );

		sWord = m_sClippedWord;
		iLen = MAX_KEYWORD_BYTES-4;
		uCRC = sphCRC32 ( m_sClippedWord, MAX_KEYWORD_BYTES-4 );
	}

	// is this a known one? find it
	// OPTIMIZE? in theory we could use something faster than crc32; but quick lookup3 test did not show any improvements
	const DWORD uHash = (DWORD)( uCRC % SLOTS );

	HitblockKeyword_t * pEntry = m_dHash [ uHash ];
	HitblockKeyword_t ** ppEntry = &m_dHash [ uHash ];
	while ( pEntry )
	{
		// check crc
		if ( pEntry->m_uWordid!=uCRC )
		{
			// crc mismatch, try next entry
			ppEntry = &pEntry->m_pNextHash;
			pEntry = pEntry->m_pNextHash;
			continue;
		}

		// crc matches, check keyword
		register int iWordLen = iLen;
		register const char * a = pEntry->m_pKeyword;
		register const char * b = sWord;
		while ( *a==*b && iWordLen-- )
		{
			if ( !*a || !iWordLen )
			{
				// known word, mtf it, and return id
				(*ppEntry) = pEntry->m_pNextHash;
				pEntry->m_pNextHash = m_dHash [ uHash ];
				m_dHash [ uHash ] = pEntry;
				return pEntry->m_uWordid;
			}
			a++;
			b++;
		}

		// collision detected!
		// our crc is taken as a wordid, but keyword does not match
		// welcome to the land of very tricky magic
		//
		// pEntry might either be a known exception, or a regular keyword
		// sWord might either be a known exception, or a new one
		// if they are not known, they needed to be added as exceptions now
		//
		// in case sWord is new, we need to assign a new unique wordid
		// for that, we keep incrementing the crc until it is unique
		// a starting point for wordid search loop would be handy
		//
		// let's scan the exceptions vector and work on all this
		//
		// NOTE, beware of the order, it is wordid asc, which does NOT guarantee crc asc
		// example, assume crc(w1)==X, crc(w2)==X+1, crc(w3)==X (collides with w1)
		// wordids will be X, X+1, X+2 but crcs will be X, X+1, X
		//
		// OPTIMIZE, might make sense to use binary search
		// OPTIMIZE, add early out somehow
		SphWordID_t uWordid = uCRC + 1;
		const int iExcLen = m_dExceptions.GetLength();
		int iExc = m_dExceptions.GetLength();
		ARRAY_FOREACH ( i, m_dExceptions )
		{
			const HitblockKeyword_t * pExcWord = m_dExceptions[i].m_pEntry;

			// incoming word is a known exception? just return the pre-assigned wordid
			if ( m_dExceptions[i].m_uCRC==uCRC && strncmp ( pExcWord->m_pKeyword, sWord, iLen )==0 )
				return pExcWord->m_uWordid;

			// incoming word collided into a known exception? clear the matched entry; no need to re-add it (see below)
			if ( pExcWord==pEntry )
				pEntry = NULL;

			// find first exception with wordid greater or equal to our candidate
			if ( pExcWord->m_uWordid>=uWordid && iExc==iExcLen )
				iExc = i;
		}

		// okay, this is a new collision
		// if entry was a regular word, we have to add it
		if ( pEntry )
		{
			m_dExceptions.Add();
			m_dExceptions.Last().m_pEntry = pEntry;
			m_dExceptions.Last().m_uCRC = uCRC;
		}

		// need to assign a new unique wordid now
		// keep scanning both exceptions and keywords for collisions
		while (true)
		{
			// iExc must be either the first exception greater or equal to current candidate, or out of bounds
			assert ( iExc==iExcLen || m_dExceptions[iExc].m_pEntry->m_uWordid>=uWordid );
			assert ( iExc==0 || m_dExceptions[iExc-1].m_pEntry->m_uWordid<uWordid );

			// candidate collides with a known exception? increment it, and keep looking
			if ( iExc<iExcLen && m_dExceptions[iExc].m_pEntry->m_uWordid==uWordid )
			{
				uWordid++;
				while ( iExc<iExcLen && m_dExceptions[iExc].m_pEntry->m_uWordid<uWordid )
					iExc++;
				continue;
			}

			// candidate collides with a keyword? must be a regular one; add it as an exception, and keep looking
			HitblockKeyword_t * pCheck = m_dHash [ (DWORD)( uWordid % SLOTS ) ];
			while ( pCheck )
			{
				if ( pCheck->m_uWordid==uWordid )
					break;
				pCheck = pCheck->m_pNextHash;
			}

			// no collisions; we've found our unique wordid!
			if ( !pCheck )
				break;

			// got a collision; add it
			HitblockException_t & tColl = m_dExceptions.Add();
			tColl.m_pEntry = pCheck;
			tColl.m_uCRC = pCheck->m_uWordid; // not a known exception; hence, wordid must equal crc

			// and keep looking
			uWordid++;
			continue;
		}

		// and finally, we have that precious new wordid
		// so hash our new unique under its new unique adjusted wordid
		pEntry = HitblockAddKeyword ( (DWORD)( uWordid % SLOTS ), sWord, iLen, uWordid );

		// add it as a collision too
		m_dExceptions.Add();
		m_dExceptions.Last().m_pEntry = pEntry;
		m_dExceptions.Last().m_uCRC = uCRC;

		// keep exceptions list sorted by wordid
		m_dExceptions.Sort();

		return pEntry->m_uWordid;
	}

	// new keyword with unique crc
	pEntry = HitblockAddKeyword ( uHash, sWord, iLen, uCRC );
	return pEntry->m_uWordid;
}


inline bool CSphDictKeywords::DictKeywordTagged_t::IsLess ( const DictKeywordTagged_t & a, const DictKeywordTagged_t & b )
{
	return strcmp ( a.m_sKeyword, b.m_sKeyword ) < 0;
};


void CSphDictKeywords::DictReadEntry ( CSphBin & dBin, DictKeywordTagged_t & tEntry, BYTE * pKeyword )
{
	int iKeywordLen = dBin.ReadByte ();
	if ( iKeywordLen<0 )
	{
		// early eof or read error; flag must be raised
		assert ( dBin.IsError() );
		return;
	}

	assert ( iKeywordLen>0 && iKeywordLen<MAX_KEYWORD_BYTES-1 );
	if ( dBin.ReadBytes ( pKeyword, iKeywordLen )!=BIN_READ_OK )
	{
		assert ( dBin.IsError() );
		return;
	}
	pKeyword[iKeywordLen] = '\0';

	assert ( m_iSkiplistBlockSize>0 );

	tEntry.m_sKeyword = (char*)pKeyword;
	tEntry.m_uOff = dBin.UnzipOffset();
	tEntry.m_iDocs = dBin.UnzipInt();
	tEntry.m_iHits = dBin.UnzipInt();
	tEntry.m_uHint = (BYTE) dBin.ReadByte();
	if ( tEntry.m_iDocs > m_iSkiplistBlockSize )
		tEntry.m_iSkiplistPos = dBin.UnzipOffset();
	else
		tEntry.m_iSkiplistPos = 0;
}


void CSphDictKeywords::DictBegin ( CSphAutofile & tTempDict, CSphAutofile & tDict, int iDictLimit )
{
	m_iTmpFD = tTempDict.GetFD();
	m_wrTmpDict.CloseFile ();
	m_wrTmpDict.SetFile ( tTempDict, NULL, m_sWriterError );

	m_wrDict.CloseFile ();
	m_wrDict.SetFile ( tDict, NULL, m_sWriterError );
	m_wrDict.PutByte ( 1 );

	m_iDictLimit = Max ( iDictLimit, KEYWORD_CHUNK + DICT_CHUNK*(int)sizeof(DictKeyword_t) ); // can't use less than 1 chunk
}


bool CSphDictKeywords::DictEnd ( DictHeader_t * pHeader, int iMemLimit, CSphString & sError )
{
	DictFlush ();
	m_wrTmpDict.CloseFile (); // tricky: file is not owned, so it won't get closed, and iTmpFD won't get invalidated

	if ( m_dDictBlocks.IsEmpty() )
		m_wrDict.CloseFile();

	if ( m_wrTmpDict.IsError() || m_wrDict.IsError() )
	{
		sError.SetSprintf ( "dictionary write error (out of space?)" );
		return false;
	}

	if ( m_dDictBlocks.IsEmpty() )
	{
		pHeader->m_iDictCheckpointsOffset = m_wrDict.GetPos ();
		pHeader->m_iDictCheckpoints = 0;
		return true;
	}

	// infix builder, if needed
	CSphScopedPtr<ISphInfixBuilder> pInfixer { sphCreateInfixBuilder ( pHeader->m_iInfixCodepointBytes, &sError ) };
	if ( !sError.IsEmpty() )
		return false;

	assert ( m_iSkiplistBlockSize>0 );

	// initialize readers
	RawVector_T<CSphBin> dBins;
	dBins.Reserve_static ( m_dDictBlocks.GetLength() );

	int iMaxBlock = 0;
	ARRAY_FOREACH ( i, m_dDictBlocks )
		iMaxBlock = Max ( iMaxBlock, m_dDictBlocks[i].m_iLen );

	iMemLimit = Max ( iMemLimit, iMaxBlock*m_dDictBlocks.GetLength() );
	int iBinSize = CSphBin::CalcBinSize ( iMemLimit, m_dDictBlocks.GetLength(), "sort_dict" );

	SphOffset_t iSharedOffset = -1;
	ARRAY_FOREACH ( i, m_dDictBlocks )
	{
		auto& dBin = dBins.Add();
		dBin.m_iFileLeft = m_dDictBlocks[i].m_iLen;
		dBin.m_iFilePos = m_dDictBlocks[i].m_iPos;
		dBin.Init ( m_iTmpFD, &iSharedOffset, iBinSize );
	}

	// keywords storage
	CSphFixedVector<BYTE> dKeywords { MAX_KEYWORD_BYTES * dBins.GetLength() };
	BYTE* pKeywords = dKeywords.begin();

	// do the sort
	CSphQueue < DictKeywordTagged_t, DictKeywordTagged_t > qWords ( dBins.GetLength() );
	DictKeywordTagged_t tEntry;

	ARRAY_FOREACH ( i, dBins )
	{
		DictReadEntry ( dBins[i], tEntry, pKeywords + i*MAX_KEYWORD_BYTES );
		if ( dBins[i].IsError() )
		{
			sError.SetSprintf ( "entry read error in dictionary sort (bin %d of %d)", i, dBins.GetLength() );
			return false;
		}

		tEntry.m_iBlock = i;
		qWords.Push ( tEntry );
	}

	bool bHasMorphology = HasMorphology();
	CSphKeywordDeltaWriter tLastKeyword;
	int iWords = 0;
	while ( qWords.GetLength() )
	{
		const DictKeywordTagged_t & tWord = qWords.Root();
		auto iLen = (const int) strlen ( tWord.m_sKeyword ); // OPTIMIZE?

		// store checkpoints as needed
		if ( ( iWords % SPH_WORDLIST_CHECKPOINT )==0 )
		{
			// emit a checkpoint, unless we're at the very dict beginning
			if ( iWords )
			{
				m_wrDict.ZipInt ( 0 );
				m_wrDict.ZipInt ( 0 );
			}

			BYTE * sClone = new BYTE [ iLen+1 ]; // OPTIMIZE? pool these?
			memcpy ( sClone, tWord.m_sKeyword, iLen );
			sClone[iLen] = '\0';

			CSphWordlistCheckpoint & tCheckpoint = m_dCheckpoints.Add ();
			tCheckpoint.m_sWord = (char*) sClone;
			tCheckpoint.m_iWordlistOffset = m_wrDict.GetPos();

			tLastKeyword.Reset();
		}
		iWords++;

		// write final dict entry
		assert ( iLen );
		assert ( tWord.m_uOff );
		assert ( tWord.m_iDocs );
		assert ( tWord.m_iHits );

		tLastKeyword.PutDelta ( m_wrDict, (const BYTE *)tWord.m_sKeyword, iLen );
		m_wrDict.ZipOffset ( tWord.m_uOff );
		m_wrDict.ZipInt ( tWord.m_iDocs );
		m_wrDict.ZipInt ( tWord.m_iHits );
		if ( tWord.m_uHint )
			m_wrDict.PutByte ( tWord.m_uHint );
		if ( tWord.m_iDocs > m_iSkiplistBlockSize )
			m_wrDict.ZipOffset ( tWord.m_iSkiplistPos );

		// build infixes
		if ( pInfixer )
			pInfixer->AddWord ( (const BYTE*)tWord.m_sKeyword, iLen, m_dCheckpoints.GetLength(), bHasMorphology );

		// next
		int iBin = tWord.m_iBlock;
		qWords.Pop ();

		if ( !dBins[iBin].IsDone() )
		{
			DictReadEntry ( dBins[iBin], tEntry, pKeywords + iBin*MAX_KEYWORD_BYTES );
			if ( dBins[iBin].IsError() )
			{
				sError.SetSprintf ( "entry read error in dictionary sort (bin %d of %d)", iBin, dBins.GetLength() );
				return false;
			}

			tEntry.m_iBlock = iBin;
			qWords.Push ( tEntry );
		}
	}

	// end of dictionary block
	m_wrDict.ZipInt ( 0 );
	m_wrDict.ZipInt ( 0 );

	// flush infix hash entries, if any
	if ( pInfixer )
		pInfixer->SaveEntries ( m_wrDict );

	// flush wordlist checkpoints (blocks)
	pHeader->m_iDictCheckpointsOffset = m_wrDict.GetPos();
	pHeader->m_iDictCheckpoints = m_dCheckpoints.GetLength();

	ARRAY_FOREACH ( i, m_dCheckpoints )
	{
		auto iLen = (const int) strlen ( m_dCheckpoints[i].m_sWord );

		assert ( m_dCheckpoints[i].m_iWordlistOffset>0 );
		assert ( iLen>0 && iLen<MAX_KEYWORD_BYTES );

		m_wrDict.PutDword ( iLen );
		m_wrDict.PutBytes ( m_dCheckpoints[i].m_sWord, iLen );
		m_wrDict.PutOffset ( m_dCheckpoints[i].m_iWordlistOffset );

		SafeDeleteArray ( m_dCheckpoints[i].m_sWord );
	}

	// flush infix hash blocks
	if ( pInfixer )
	{
		pHeader->m_iInfixBlocksOffset = pInfixer->SaveEntryBlocks ( m_wrDict );
		pHeader->m_iInfixBlocksWordsSize = pInfixer->GetBlocksWordsSize();
		if ( pHeader->m_iInfixBlocksOffset>UINT_MAX ) // FIXME!!! change to int64
			sphDie ( "INTERNAL ERROR: dictionary size " INT64_FMT " overflow at dictend save", pHeader->m_iInfixBlocksOffset );
	}

	// flush header
	// mostly for debugging convenience
	// primary storage is in the index wide header
	m_wrDict.PutBytes ( "dict-header", 11 );
	m_wrDict.ZipInt ( pHeader->m_iDictCheckpoints );
	m_wrDict.ZipOffset ( pHeader->m_iDictCheckpointsOffset );
	m_wrDict.ZipInt ( pHeader->m_iInfixCodepointBytes );
	m_wrDict.ZipInt ( (DWORD)pHeader->m_iInfixBlocksOffset );

	// about it
	m_wrDict.CloseFile ();
	if ( m_wrDict.IsError() )
		sError.SetSprintf ( "dictionary write error (out of space?)" );
	return !m_wrDict.IsError();
}

struct DictKeywordCmp_fn
{
	inline bool IsLess ( CSphDictKeywords::DictKeyword_t * a, CSphDictKeywords::DictKeyword_t * b ) const
	{
		return strcmp ( a->m_sKeyword, b->m_sKeyword ) < 0;
	}
};

void CSphDictKeywords::DictFlush ()
{
	if ( !m_dDictChunks.GetLength() )
		return;

	assert ( m_dDictChunks.GetLength() && m_dKeywordChunks.GetLength() );
	assert ( m_iSkiplistBlockSize>0 );

	// sort em
	int iTotalWords = m_dDictChunks.GetLength()*DICT_CHUNK - m_iDictChunkFree;
	CSphVector<DictKeyword_t*> dWords ( iTotalWords );

	int iIdx = 0;
	ARRAY_FOREACH ( i, m_dDictChunks )
	{
		int iWords = DICT_CHUNK;
		if ( i==m_dDictChunks.GetLength()-1 )
			iWords -= m_iDictChunkFree;

		DictKeyword_t * pWord = m_dDictChunks[i];
		for ( int j=0; j<iWords; j++ )
			dWords[iIdx++] = pWord++;
	}

	dWords.Sort ( DictKeywordCmp_fn() );

	// write em
	DictBlock_t & tBlock = m_dDictBlocks.Add();
	tBlock.m_iPos = m_wrTmpDict.GetPos ();

	ARRAY_FOREACH ( i, dWords )
	{
		const DictKeyword_t * pWord = dWords[i];
		auto iLen = (int) strlen ( pWord->m_sKeyword );
		m_wrTmpDict.PutByte ( (BYTE)iLen );
		m_wrTmpDict.PutBytes ( pWord->m_sKeyword, iLen );
		m_wrTmpDict.ZipOffset ( pWord->m_uOff );
		m_wrTmpDict.ZipInt ( pWord->m_iDocs );
		m_wrTmpDict.ZipInt ( pWord->m_iHits );
		m_wrTmpDict.PutByte ( pWord->m_uHint );
		assert ( ( pWord->m_iDocs > m_iSkiplistBlockSize )==( pWord->m_iSkiplistPos!=0 ) );
		if ( pWord->m_iDocs > m_iSkiplistBlockSize )
			m_wrTmpDict.ZipOffset ( pWord->m_iSkiplistPos );
	}

	tBlock.m_iLen = (int)( m_wrTmpDict.GetPos() - tBlock.m_iPos );

	// clean up buffers
	ARRAY_FOREACH ( i, m_dDictChunks )
		SafeDeleteArray ( m_dDictChunks[i] );
	m_dDictChunks.Resize ( 0 );
	m_pDictChunk = NULL;
	m_iDictChunkFree = 0;

	ARRAY_FOREACH ( i, m_dKeywordChunks )
		SafeDeleteArray ( m_dKeywordChunks[i] );
	m_dKeywordChunks.Resize ( 0 );
	m_pKeywordChunk = NULL;
	m_iKeywordChunkFree = 0;

	m_iMemUse = 0;
}

void CSphDictKeywords::DictEntry ( const CSphDictEntry & tEntry )
{
	// they say, this might just happen during merge
	// FIXME! can we make merge avoid sending such keywords to dict and assert here?
	if ( !tEntry.m_iDocs )
		return;

	assert ( tEntry.m_iHits );
	assert ( tEntry.m_iDoclistLength>0 );
	assert ( m_iSkiplistBlockSize>0 );

	DictKeyword_t * pWord = NULL;
	auto iLen = (int) strlen ( (const char*)tEntry.m_sKeyword ) + 1;

	while (true)
	{
		// alloc dict entry
		if ( !m_iDictChunkFree )
		{
			if ( m_iDictLimit && ( m_iMemUse + (int)sizeof(DictKeyword_t)*DICT_CHUNK )>m_iDictLimit )
				DictFlush ();

			m_pDictChunk = new DictKeyword_t [ DICT_CHUNK ];
			m_iDictChunkFree = DICT_CHUNK;
			m_dDictChunks.Add ( m_pDictChunk );
			m_iMemUse += sizeof(DictKeyword_t)*DICT_CHUNK;
		}

		// alloc keyword
		if ( m_iKeywordChunkFree < iLen )
		{
			if ( m_iDictLimit && ( m_iMemUse + KEYWORD_CHUNK )>m_iDictLimit )
			{
				DictFlush ();
				continue; // because we just flushed pWord
			}

			m_pKeywordChunk = new BYTE [ KEYWORD_CHUNK ];
			m_iKeywordChunkFree = KEYWORD_CHUNK;
			m_dKeywordChunks.Add ( m_pKeywordChunk );
			m_iMemUse += KEYWORD_CHUNK;
		}
		// aw kay
		break;
	}

	pWord = m_pDictChunk++;
	m_iDictChunkFree--;
	pWord->m_sKeyword = (char*)m_pKeywordChunk;
	memcpy ( m_pKeywordChunk, tEntry.m_sKeyword, iLen );
	m_pKeywordChunk[iLen-1] = '\0';
	m_pKeywordChunk += iLen;
	m_iKeywordChunkFree -= iLen;

	pWord->m_uOff = tEntry.m_iDoclistOffset;
	pWord->m_iDocs = tEntry.m_iDocs;
	pWord->m_iHits = tEntry.m_iHits;
	pWord->m_uHint = sphDoclistHintPack ( tEntry.m_iDocs, tEntry.m_iDoclistLength );
	pWord->m_iSkiplistPos = 0;
	if ( tEntry.m_iDocs > m_iSkiplistBlockSize )
		pWord->m_iSkiplistPos = tEntry.m_iSkiplistOffset;
}

SphWordID_t CSphDictKeywords::GetWordID ( BYTE * pWord )
{
	SphWordID_t uCRC = CSphDictCRC<true>::GetWordID ( pWord );
	if ( !uCRC || !m_bHitblock )
		return uCRC;

	auto iLen = (int) strlen ( (const char *)pWord );
	return HitblockGetID ( (const char *)pWord, iLen, uCRC );
}

SphWordID_t CSphDictKeywords::GetWordIDWithMarkers ( BYTE * pWord )
{
	SphWordID_t uCRC = CSphDictCRC<true>::GetWordIDWithMarkers ( pWord );
	if ( !uCRC || !m_bHitblock )
		return uCRC;

	auto iLen = (int) strlen ( (const char *)pWord );
	return HitblockGetID ( (const char *)pWord, iLen, uCRC );
}

SphWordID_t CSphDictKeywords::GetWordIDNonStemmed ( BYTE * pWord )
{
	SphWordID_t uCRC = CSphDictCRC<true>::GetWordIDNonStemmed ( pWord );
	if ( !uCRC || !m_bHitblock )
		return uCRC;

	auto iLen = (int) strlen ( (const char *)pWord );
	return HitblockGetID ( (const char *)pWord, iLen, uCRC );
}

SphWordID_t CSphDictKeywords::GetWordID ( const BYTE * pWord, int iLen, bool bFilterStops )
{
	SphWordID_t uCRC = CSphDictCRC<true>::GetWordID ( pWord, iLen, bFilterStops );
	if ( !uCRC || !m_bHitblock )
		return uCRC;

	return HitblockGetID ( (const char *)pWord, iLen, uCRC ); // !COMMIT would break, we kind of strcmp inside; but must never get called?
}

/// binary search for the first hit with wordid greater than or equal to reference
static CSphWordHit * FindFirstGte ( CSphWordHit * pHits, int iHits, SphWordID_t uID )
{
	if ( pHits->m_uWordID==uID )
		return pHits;

	CSphWordHit * pL = pHits;
	CSphWordHit * pR = pHits + iHits - 1;
	if ( pL->m_uWordID > uID || pR->m_uWordID < uID )
		return NULL;

	while ( pR-pL!=1 )
	{
		CSphWordHit * pM = pL + ( pR-pL )/2;
		if ( pM->m_uWordID < uID )
			pL = pM;
		else
			pR = pM;
	}

	assert ( pR-pL==1 );
	assert ( pL->m_uWordID<uID );
	assert ( pR->m_uWordID>=uID );
	return pR;
}

/// full crc and keyword check
static inline bool FullIsLess ( const CSphDictKeywords::HitblockException_t & a, const CSphDictKeywords::HitblockException_t & b )
{
	if ( a.m_uCRC!=b.m_uCRC )
		return a.m_uCRC < b.m_uCRC;
	return strcmp ( a.m_pEntry->m_pKeyword, b.m_pEntry->m_pKeyword ) < 0;
}

/// sort functor to compute collided hits reordering
struct HitblockPatchSort_fn
{
	const CSphDictKeywords::HitblockException_t * m_pExc;

	explicit HitblockPatchSort_fn ( const CSphDictKeywords::HitblockException_t * pExc )
		: m_pExc ( pExc )
	{}

	bool IsLess ( int a, int b ) const
	{
		return FullIsLess ( m_pExc[a], m_pExc[b] );
	}
};

/// do hit block patching magic
void CSphDictKeywords::HitblockPatch ( CSphWordHit * pHits, int iHits ) const
{
	if ( !pHits || iHits<=0 )
		return;

	const CSphVector<HitblockException_t> & dExc = m_dExceptions; // shortcut
	CSphVector<CSphWordHit*> dChunk;

	// reorder hit chunks for exceptions (aka crc collisions)
	for ( int iFirst = 0; iFirst < dExc.GetLength()-1; )
	{
		// find next span of collisions, iFirst inclusive, iMax exclusive ie. [iFirst,iMax)
		// (note that exceptions array is always sorted)
		SphWordID_t uFirstWordid = dExc[iFirst].m_pEntry->m_uWordid;
		assert ( dExc[iFirst].m_uCRC==uFirstWordid );

		int iMax = iFirst+1;
		SphWordID_t uSpan = uFirstWordid+1;
		while ( iMax < dExc.GetLength() && dExc[iMax].m_pEntry->m_uWordid==uSpan )
		{
			iMax++;
			uSpan++;
		}

		// check whether they are in proper order already
		bool bSorted = true;
		for ( int i=iFirst; i<iMax-1 && bSorted; i++ )
			if ( FullIsLess ( dExc[i+1], dExc[i] ) )
				bSorted = false;

		// order is ok; skip this span
		if ( bSorted )
		{
			iFirst = iMax;
			continue;
		}

		// we need to fix up these collision hits
		// convert them from arbitrary "wordid asc" to strict "crc asc, keyword asc" order
		// lets begin with looking up hit chunks for every wordid
		dChunk.Resize ( iMax-iFirst+1 );

		// find the end
		dChunk.Last() = FindFirstGte ( pHits, iHits, uFirstWordid+iMax-iFirst );
		if ( !dChunk.Last() )
		{
			assert ( iMax==dExc.GetLength() && pHits[iHits-1].m_uWordID==uFirstWordid+iMax-1-iFirst );
			dChunk.Last() = pHits+iHits;
		}

		// find the start
		dChunk[0] = FindFirstGte ( pHits, int ( dChunk.Last()-pHits ), uFirstWordid );
		assert ( dChunk[0] && dChunk[0]->m_uWordID==uFirstWordid );

		// find the chunk starts
		for ( int i=1; i<dChunk.GetLength()-1; i++ )
		{
			dChunk[i] = FindFirstGte ( dChunk[i-1], int ( dChunk.Last()-dChunk[i-1] ), uFirstWordid+i );
			assert ( dChunk[i] && dChunk[i]->m_uWordID==uFirstWordid+i );
		}

		CSphWordHit * pTemp;
		if ( iMax-iFirst==2 )
		{
			// most frequent case, just two collisions
			// OPTIMIZE? allocate buffer for the smaller chunk, not just first chunk
			pTemp = new CSphWordHit [ dChunk[1]-dChunk[0] ];
			memcpy ( pTemp, dChunk[0], ( dChunk[1]-dChunk[0] )*sizeof(CSphWordHit) );
			memmove ( dChunk[0], dChunk[1], ( dChunk[2]-dChunk[1] )*sizeof(CSphWordHit) );
			memcpy ( dChunk[0] + ( dChunk[2]-dChunk[1] ), pTemp, ( dChunk[1]-dChunk[0] )*sizeof(CSphWordHit) );
		} else
		{
			// generic case, more than two
			CSphVector<int> dReorder ( iMax-iFirst );
			ARRAY_FOREACH ( i, dReorder )
				dReorder[i] = i;

			HitblockPatchSort_fn fnSort ( &dExc[iFirst] );
			dReorder.Sort ( fnSort );

			// OPTIMIZE? could skip heading and trailing blocks that are already in position
			pTemp = new CSphWordHit [ dChunk.Last()-dChunk[0] ];
			CSphWordHit * pOut = pTemp;

			ARRAY_FOREACH ( i, dReorder )
			{
				int iChunk = dReorder[i];
				int iChunkHits = int ( dChunk[iChunk+1] - dChunk[iChunk] );
				memcpy ( pOut, dChunk[iChunk], iChunkHits*sizeof(CSphWordHit) );
				pOut += iChunkHits;
			}

			assert ( ( pOut-pTemp )==( dChunk.Last()-dChunk[0] ) );
			memcpy ( dChunk[0], pTemp, ( dChunk.Last()-dChunk[0] )*sizeof(CSphWordHit) );
		}

		// patching done
		SafeDeleteArray ( pTemp );
		iFirst = iMax;
	}
}

const char * CSphDictKeywords::HitblockGetKeyword ( SphWordID_t uWordID )
{
	const DWORD uHash = (DWORD)( uWordID % SLOTS );

	HitblockKeyword_t * pEntry = m_dHash [ uHash ];
	while ( pEntry )
	{
		// check crc
		if ( pEntry->m_uWordid!=uWordID )
		{
			// crc mismatch, try next entry
			pEntry = pEntry->m_pNextHash;
			continue;
		}

		return pEntry->m_pKeyword;
	}

	assert ( m_dExceptions.GetLength() );
	ARRAY_FOREACH ( i, m_dExceptions )
		if ( m_dExceptions[i].m_pEntry->m_uWordid==uWordID )
			return m_dExceptions[i].m_pEntry->m_pKeyword;

	sphWarning ( "hash missing value in operator [] (wordid=" INT64_FMT ", hash=%u)", (int64_t)uWordID, uHash );
	assert ( 0 && "hash missing value in operator []" );
	return "\31oops";
}

//////////////////////////////////////////////////////////////////////////
// KEYWORDS STORING DICTIONARY
//////////////////////////////////////////////////////////////////////////

class CRtDictKeywords final : public ISphRtDictWraper
{
private:
	DictRefPtr_c		m_pBase;
	SmallStringHash_T<int, 8192>	m_hKeywords;

	CSphVector<BYTE>		m_dPackedKeywords {0};

	CSphString				m_sWarning;
	int						m_iKeywordsOverrun = 0;
	CSphString				m_sWord; // For allocation reuse.
	const bool				m_bStoreID = false;

protected:
	~CRtDictKeywords () final = default; // Is here since protected. fixme! remove

public:
	explicit CRtDictKeywords ( CSphDict * pBase, bool bStoreID )
		: m_pBase ( pBase )
		, m_bStoreID ( bStoreID )
	{
		SafeAddRef ( pBase );
		m_dPackedKeywords.Add ( 0 ); // avoid zero offset at all costs
	}

	SphWordID_t GetWordID ( BYTE * pWord ) final
	{
		SphWordID_t tWordID = m_pBase->GetWordID ( pWord );
		if ( tWordID )
			return AddKeyword ( pWord, tWordID );
		return 0;
	}

	SphWordID_t GetWordIDWithMarkers ( BYTE * pWord ) final
	{
		SphWordID_t tWordID = m_pBase->GetWordIDWithMarkers ( pWord );
		if ( tWordID )
			return AddKeyword ( pWord, tWordID );
		return 0;
	}

	SphWordID_t GetWordIDNonStemmed ( BYTE * pWord ) final
	{
		SphWordID_t tWordID = m_pBase->GetWordIDNonStemmed ( pWord );
		if ( tWordID )
			return AddKeyword ( pWord, tWordID );
		return 0;
	}

	SphWordID_t GetWordID ( const BYTE * pWord, int iLen, bool bFilterStops ) final
	{
		SphWordID_t tWordID = m_pBase->GetWordID ( pWord, iLen, bFilterStops );
		if ( tWordID )
			return AddKeyword ( pWord, tWordID );
		return 0;
	}

	const BYTE * GetPackedKeywords () final { return m_dPackedKeywords.Begin(); }
	int GetPackedLen () final { return m_dPackedKeywords.GetLength(); }
	void ResetKeywords() final
	{
		m_dPackedKeywords.Resize ( 0 );
		m_dPackedKeywords.Add ( 0 ); // avoid zero offset at all costs
		m_hKeywords.Reset();
	}

	void LoadStopwords ( const char * sFiles, const ISphTokenizer * pTokenizer, bool bStripFile ) final { m_pBase->LoadStopwords ( sFiles, pTokenizer, bStripFile ); }
	void LoadStopwords ( const CSphVector<SphWordID_t> & dStopwords ) final { m_pBase->LoadStopwords ( dStopwords ); }
	void WriteStopwords ( CSphWriter & tWriter ) const final { m_pBase->WriteStopwords ( tWriter ); }
	void WriteStopwords ( JsonEscapedBuilder & tOut ) const final { m_pBase->WriteStopwords ( tOut ); }
	bool LoadWordforms ( const StrVec_t & dFiles, const CSphEmbeddedFiles * pEmbedded, const ISphTokenizer * pTokenizer, const char * sIndex ) final
		{ return m_pBase->LoadWordforms ( dFiles, pEmbedded, pTokenizer, sIndex ); }
	void WriteWordforms ( CSphWriter & tWriter ) const final { m_pBase->WriteWordforms ( tWriter ); }
	void WriteWordforms ( JsonEscapedBuilder & tOut ) const final { m_pBase->WriteWordforms ( tOut ); }
	int SetMorphology ( const char * szMorph, CSphString & sMessage ) final { return m_pBase->SetMorphology ( szMorph, sMessage ); }
	void Setup ( const CSphDictSettings & tSettings ) final { m_pBase->Setup ( tSettings ); }
	const CSphDictSettings & GetSettings () const final { return m_pBase->GetSettings(); }
	const CSphVector <CSphSavedFile> & GetStopwordsFileInfos () const final { return m_pBase->GetStopwordsFileInfos(); }
	const CSphVector <CSphSavedFile> & GetWordformsFileInfos () const final { return m_pBase->GetWordformsFileInfos(); }
	const CSphMultiformContainer * GetMultiWordforms () const final { return m_pBase->GetMultiWordforms(); }
	bool IsStopWord ( const BYTE * pWord ) const final { return m_pBase->IsStopWord ( pWord ); }
	const char * GetLastWarning() const final { return m_iKeywordsOverrun ? m_sWarning.cstr() : nullptr; }
	void ResetWarning () final { m_iKeywordsOverrun = 0; }
	uint64_t GetSettingsFNV () const final { return m_pBase->GetSettingsFNV(); }

private:
	SphWordID_t AddKeyword ( const BYTE * pWord, SphWordID_t tWordID )
	{
		auto iLen = (int) strlen ( ( const char * ) pWord );
		// stemmer might squeeze out the word
		if ( !iLen )
			return 0;

		// fix of very long word (zones)
		if ( iLen>( SPH_MAX_WORD_LEN * 3 ) )
		{
			int iClippedLen = SPH_MAX_WORD_LEN * 3;
			m_sWord.SetBinary ( ( const char * ) pWord, iClippedLen );
			if ( m_iKeywordsOverrun )
			{
				m_sWarning.SetSprintf ( "word overrun buffer, clipped!!! clipped='%s', length=%d(%d)", m_sWord.cstr ()
										, iClippedLen, iLen );
			} else
			{
				m_sWarning.SetSprintf ( ", clipped='%s', length=%d(%d)", m_sWord.cstr (), iClippedLen, iLen );
			}
			iLen = iClippedLen;
			m_iKeywordsOverrun++;
		} else
		{
			m_sWord.SetBinary ( ( const char * ) pWord, iLen );
		}

		int * pOff = m_hKeywords ( m_sWord );
		if ( pOff )
		{
			return *pOff;
		}

		int iOff = m_dPackedKeywords.GetLength ();
		int iPackedLen = iOff + iLen + 1;
		if ( m_bStoreID )
			iPackedLen += sizeof ( tWordID );
		m_dPackedKeywords.Resize ( iPackedLen );
		m_dPackedKeywords[iOff] = ( BYTE ) ( iLen & 0xFF );
		memcpy ( m_dPackedKeywords.Begin () + iOff + 1, pWord, iLen );
		if ( m_bStoreID )
			memcpy ( m_dPackedKeywords.Begin () + iOff + 1 + iLen, &tWordID, sizeof(tWordID) );

		m_hKeywords.Add ( iOff, m_sWord );

		return iOff;
	}
};

ISphRtDictWraper * sphCreateRtKeywordsDictionaryWrapper ( CSphDict * pBase, bool bStoreID )
{
	return new CRtDictKeywords ( pBase, bStoreID );
}


//////////////////////////////////////////////////////////////////////////
// DICTIONARY FACTORIES
//////////////////////////////////////////////////////////////////////////

static void SetupDictionary ( DictRefPtr_c & pDict, const CSphDictSettings & tSettings, const CSphEmbeddedFiles * pFiles, const ISphTokenizer * pTokenizer, const char * sIndex, bool bStripFile, FilenameBuilder_i * pFilenameBuilder, CSphString & sError )
{
	assert ( pTokenizer );

	pDict->Setup ( tSettings );
	if ( pDict->SetMorphology ( tSettings.m_sMorphology.cstr (), sError )==CSphDict::ST_ERROR )
	{
		pDict = nullptr;
		return;
	}

	if ( pFiles && pFiles->m_bEmbeddedStopwords )
		pDict->LoadStopwords ( pFiles->m_dStopwords );
	else
	{
		CSphString sStopwordFile = tSettings.m_sStopwords;
		if ( !sStopwordFile.IsEmpty() )
		{
			if ( pFilenameBuilder )
				sStopwordFile = pFilenameBuilder->GetFullPath(sStopwordFile);

			pDict->LoadStopwords ( sStopwordFile.cstr(), pTokenizer, bStripFile );
		}
	}

	StrVec_t dWordformFiles;
	if ( pFilenameBuilder )
	{
		dWordformFiles.Resize ( tSettings.m_dWordforms.GetLength() );
		ARRAY_FOREACH ( i, tSettings.m_dWordforms )
			dWordformFiles[i] = pFilenameBuilder->GetFullPath ( tSettings.m_dWordforms[i] );
	}

	pDict->LoadWordforms ( pFilenameBuilder ? dWordformFiles : tSettings.m_dWordforms, pFiles && pFiles->m_bEmbeddedWordforms ? pFiles : NULL, pTokenizer, sIndex );
}

CSphDict * sphCreateDictionaryTemplate ( const CSphDictSettings & tSettings, const CSphEmbeddedFiles * pFiles, const ISphTokenizer * pTokenizer, const char * sIndex, bool bStripFile,
	FilenameBuilder_i * pFilenameBuilder, CSphString & sError )
{
	DictRefPtr_c pDict { new CSphDictTemplate() };
	SetupDictionary ( pDict, tSettings, pFiles, pTokenizer, sIndex, bStripFile, pFilenameBuilder, sError );
	return pDict.Leak();
}


CSphDict * sphCreateDictionaryCRC ( const CSphDictSettings & tSettings, const CSphEmbeddedFiles * pFiles, const ISphTokenizer * pTokenizer, const char * sIndex, bool bStripFile,
	int iSkiplistBlockSize, FilenameBuilder_i * pFilenameBuilder, CSphString & sError )
{
	DictRefPtr_c pDict { new CSphDictCRC<false> };
	SetupDictionary ( pDict, tSettings, pFiles, pTokenizer, sIndex, bStripFile, pFilenameBuilder, sError );
	// might be empty due to wrong morphology setup
	if ( pDict.Ptr() )
		pDict->SetSkiplistBlockSize ( iSkiplistBlockSize );
	return pDict.Leak ();
}


CSphDict * sphCreateDictionaryKeywords ( const CSphDictSettings & tSettings, const CSphEmbeddedFiles * pFiles, const ISphTokenizer * pTokenizer, const char * sIndex, bool bStripFile,
	int iSkiplistBlockSize, FilenameBuilder_i * pFilenameBuilder, CSphString & sError )
{
	DictRefPtr_c pDict { new CSphDictKeywords() };
	SetupDictionary ( pDict, tSettings, pFiles, pTokenizer, sIndex, bStripFile, pFilenameBuilder, sError );
	// might be empty due to wrong morphology setup
	if ( pDict.Ptr() )
		pDict->SetSkiplistBlockSize ( iSkiplistBlockSize );
	return pDict.Leak ();
}

const CSphSourceStats& GetStubStats()
{
	static CSphSourceStats tDummy;
	return tDummy;
}

void sphShutdownWordforms ()
{
	CSphVector<CSphSavedFile> dEmptyFiles;
	CSphDiskDictTraits::SweepWordformContainers ( dEmptyFiles );
}

CSphDict * GetStatelessDict ( CSphDict * pDict )
{
	if ( !pDict )
		return nullptr;

	if ( pDict->HasState () )
		return pDict->Clone();

	SafeAddRef ( pDict );
	return pDict;
}


//////////////////////////////////////////////////////////////////////////
ISphFieldFilter::ISphFieldFilter()
	: m_pParent ( NULL )
{
}


ISphFieldFilter::~ISphFieldFilter()
{
	SafeRelease ( m_pParent );
}


void ISphFieldFilter::SetParent ( ISphFieldFilter * pParent )
{
	SafeAddRef ( pParent );
	SafeRelease ( m_pParent );
	m_pParent = pParent;
}


#if WITH_RE2
class CSphFieldRegExps : public ISphFieldFilter
{
protected:
	~CSphFieldRegExps() override;
public:
						CSphFieldRegExps () = default;
						CSphFieldRegExps ( const StrVec_t& m_dRegexps, CSphString &	sError );

	int					Apply ( const BYTE * sField, int iLength, CSphVector<BYTE> & dStorage, bool ) final;
	void				GetSettings ( CSphFieldFilterSettings & tSettings ) const final;
	ISphFieldFilter *	Clone() const final;

	bool				AddRegExp ( const char * sRegExp, CSphString & sError );

private:
	struct RegExp_t
	{
		CSphString	m_sFrom;
		CSphString	m_sTo;

		RE2 *		m_pRE2;
	};

	CSphVector<RegExp_t>	m_dRegexps;
	bool					m_bCloned = true;
};

CSphFieldRegExps::CSphFieldRegExps ( const StrVec_t &dRegexps, CSphString &sError )
	: m_bCloned ( false )
{
	for ( const auto &sRegexp : dRegexps )
		AddRegExp ( sRegexp.cstr (), sError );
}


CSphFieldRegExps::~CSphFieldRegExps ()
{
	if ( !m_bCloned )
	{
		for ( auto & dRegexp : m_dRegexps )
			SafeDelete ( dRegexp.m_pRE2 );
	}
}


int CSphFieldRegExps::Apply ( const BYTE * sField, int iLength, CSphVector<BYTE> & dStorage, bool )
{
	dStorage.Resize ( 0 );
	if ( !sField || !*sField )
		return 0;

	bool bReplaced = false;
	std::string sRe2 = ( iLength ? std::string ( (const char *) sField, iLength ) : (const char *) sField );
	ARRAY_FOREACH ( i, m_dRegexps )
	{
		assert ( m_dRegexps[i].m_pRE2 );
		bReplaced |= ( RE2::GlobalReplace ( &sRe2, *m_dRegexps[i].m_pRE2, m_dRegexps[i].m_sTo.cstr() )>0 );
	}

	if ( !bReplaced )
		return 0;

	auto iDstLen = (int) sRe2.length();
	dStorage.Resize ( iDstLen+4 ); // string SAFETY_GAP
	strncpy ( (char *)dStorage.Begin(), sRe2.c_str(), dStorage.GetLength() );
	return iDstLen;
}


void CSphFieldRegExps::GetSettings ( CSphFieldFilterSettings & tSettings ) const
{
	tSettings.m_dRegexps.Resize ( m_dRegexps.GetLength() );
	ARRAY_FOREACH ( i, m_dRegexps )
		tSettings.m_dRegexps[i].SetSprintf ( "%s => %s", m_dRegexps[i].m_sFrom.cstr(), m_dRegexps[i].m_sTo.cstr() );
}


bool CSphFieldRegExps::AddRegExp ( const char * sRegExp, CSphString & sError )
{
	if ( m_bCloned )
		return false;

	const char sSplitter [] = "=>";
	const char * sSplit = strstr ( sRegExp, sSplitter );
	if ( !sSplit )
	{
		sError = "mapping token (=>) not found";
		return false;
	} else if ( strstr ( sSplit + strlen ( sSplitter ), sSplitter ) )
	{
		sError = "mapping token (=>) found more than once";
		return false;
	}

	m_dRegexps.Resize ( m_dRegexps.GetLength () + 1 );
	RegExp_t & tRegExp = m_dRegexps.Last();
	tRegExp.m_sFrom.SetBinary ( sRegExp, int ( sSplit-sRegExp ) );
	tRegExp.m_sTo = sSplit + strlen ( sSplitter );
	tRegExp.m_sFrom.Trim();
	tRegExp.m_sTo.Trim();

	RE2::Options tOptions;
	tOptions.set_encoding ( RE2::Options::Encoding::EncodingUTF8 );
	tRegExp.m_pRE2 = new RE2 ( tRegExp.m_sFrom.cstr(), tOptions );

	std::string sRE2Error;
	if ( !tRegExp.m_pRE2->CheckRewriteString ( tRegExp.m_sTo.cstr(), &sRE2Error ) )
	{
		sError.SetSprintf ( "\"%s => %s\" is not a valid mapping: %s", tRegExp.m_sFrom.cstr(), tRegExp.m_sTo.cstr(), sRE2Error.c_str() );
		SafeDelete ( tRegExp.m_pRE2 );
		m_dRegexps.Remove ( m_dRegexps.GetLength() - 1 );
		return false;
	}

	return true;
}


ISphFieldFilter * CSphFieldRegExps::Clone() const
{
	auto * pCloned = new CSphFieldRegExps;
	pCloned->m_dRegexps = m_dRegexps;

	return pCloned;
}
#endif


#if WITH_RE2
ISphFieldFilter * sphCreateRegexpFilter ( const CSphFieldFilterSettings & tFilterSettings, CSphString & sError )
{
	return new CSphFieldRegExps ( tFilterSettings.m_dRegexps, sError );
}
#else
ISphFieldFilter * sphCreateRegexpFilter ( const CSphFieldFilterSettings &, CSphString & )
{
	return nullptr;
}
#endif


/////////////////////////////////////////////////////////////////////////////
// GENERIC SOURCE
/////////////////////////////////////////////////////////////////////////////

bool ParseMorphFields ( const CSphString & sMorphology, const CSphString & sMorphFields, const CSphVector<CSphColumnInfo> & dFields, CSphBitvec & tMorphFields, CSphString & sError )
{
	if ( sMorphology.IsEmpty() || sMorphFields.IsEmpty() )
		return true;

	CSphString sFields = sMorphFields;
	sFields.ToLower();
	sFields.Trim();
	if ( !sFields.Length() )
		return true;

	OpenHash_T<int, int64_t, HashFunc_Int64_t> hFields;
	ARRAY_FOREACH ( i, dFields )
		hFields.Add ( sphFNV64 ( dFields[i].m_sName.cstr() ), i );

	StringBuilder_c sMissed;
	int iFieldsGot = 0;
	tMorphFields.Init ( dFields.GetLength() );
	tMorphFields.Set(); // all fields have morphology by default

	for ( const char * sCur=sFields.cstr(); ; )
	{
		while ( *sCur && ( sphIsSpace ( *sCur ) || *sCur==',' ) )
			++sCur;
		if ( !*sCur )
			break;

		const char * sStart = sCur;
		while ( *sCur && !sphIsSpace ( *sCur ) && *sCur!=',' )
			++sCur;

		if ( sCur==sStart )
			break;

		int * pField = hFields.Find ( sphFNV64 ( sStart, int ( sCur - sStart ) ) );
		if ( !pField )
		{
			const char * sSep = sMissed.GetLength() ? ", " : "";
			sMissed.Appendf ( "%s%.*s", sSep, (int)(sCur - sStart), sStart );
			break;
		}

		iFieldsGot++;
		tMorphFields.BitClear ( *pField );
	}

	// no fields set - need to reset bitvec to skip checks on indexing
	if ( !iFieldsGot )
		tMorphFields.Init ( 0 );

	if ( sMissed.GetLength() )
		sError.SetSprintf ( "morphology_skip_fields contains out of schema fields: %s", sMissed.cstr() );

	return ( !sMissed.GetLength() );
}


bool SchemaConfigureCheckAttribute ( const CSphSchema & tSchema, const CSphColumnInfo & tCol, CSphString & sError )
{
	if ( tCol.m_sName.IsEmpty() )
	{
		sError.SetSprintf ( "column number %d has no name", tCol.m_iIndex );
		return false;
	}

	if ( tSchema.GetAttr ( tCol.m_sName.cstr() ) )
	{
		sError.SetSprintf ( "can not add multiple attributes with same name '%s'", tCol.m_sName.cstr () );
		return false;
	}

	if ( CSphSchema::IsReserved ( tCol.m_sName.cstr() ) )
	{
		sError.SetSprintf ( "%s is not a valid attribute name", tCol.m_sName.cstr() );
		return false;
	}

	return true;
}

/////////////////////////////////////////////////////////////////////////////


void sphSetJsonOptions ( bool bStrict, bool bAutoconvNumbers, bool bKeynamesToLowercase )
{
	g_bJsonStrict = bStrict;
	g_bJsonAutoconvNumbers = bAutoconvNumbers;
	g_bJsonKeynamesToLowercase = bKeynamesToLowercase;
}


void SetPseudoShardingThresh ( int iThresh )
{
	g_iSplitThresh = iThresh;
}

//////////////////////////////////////////////////////////////////////////

int sphDictCmp ( const char * pStr1, int iLen1, const char * pStr2, int iLen2 )
{
	assert ( pStr1 && pStr2 );
	assert ( iLen1 && iLen2 );
	const int iCmpLen = Min ( iLen1, iLen2 );
	return memcmp ( pStr1, pStr2, iCmpLen );
}

int sphDictCmpStrictly ( const char * pStr1, int iLen1, const char * pStr2, int iLen2 )
{
	assert ( pStr1 && pStr2 );
	assert ( iLen1 && iLen2 );
	const int iCmpLen = Min ( iLen1, iLen2 );
	const int iCmpRes = memcmp ( pStr1, pStr2, iCmpLen );
	return iCmpRes==0 ? iLen1-iLen2 : iCmpRes;
}


ISphWordlist::Args_t::Args_t ( bool bPayload, int iExpansionLimit, bool bHasExactForms, ESphHitless eHitless, cRefCountedRefPtr_t pIndexData )
	: m_bPayload ( bPayload )
	, m_iExpansionLimit ( iExpansionLimit )
	, m_bHasExactForms ( bHasExactForms )
	, m_eHitless ( eHitless )
	, m_pIndexData ( pIndexData )
{
	m_sBuf.Reserve ( 2048 * SPH_MAX_WORD_LEN * 3 );
	m_dExpanded.Reserve ( 2048 );
	m_pPayload = nullptr;
	m_iTotalDocs = 0;
	m_iTotalHits = 0;
}


ISphWordlist::Args_t::~Args_t ()
{
	SafeDelete ( m_pPayload );
}


void ISphWordlist::Args_t::AddExpanded ( const BYTE * sName, int iLen, int iDocs, int iHits )
{
	SphExpanded_t & tExpanded = m_dExpanded.Add();
	tExpanded.m_iDocs = iDocs;
	tExpanded.m_iHits = iHits;
	int iOff = m_sBuf.GetLength();
	tExpanded.m_iNameOff = iOff;

	m_sBuf.Resize ( iOff + iLen + 1 );
	memcpy ( m_sBuf.Begin()+iOff, sName, iLen );
	m_sBuf[iOff+iLen] = '\0';
}


const char * ISphWordlist::Args_t::GetWordExpanded ( int iIndex ) const
{
	assert ( m_dExpanded[iIndex].m_iNameOff<m_sBuf.GetLength() );
	return (const char *)m_sBuf.Begin() + m_dExpanded[iIndex].m_iNameOff;
}


struct DiskExpandedEntry_t
{
	int		m_iNameOff;
	int		m_iDocs;
	int		m_iHits;
};

struct DiskExpandedPayload_t
{
	int			m_iDocs;
	int			m_iHits;
	uint64_t	m_uDoclistOff;
	int			m_iDoclistHint;
};


struct DictEntryDiskPayload_t
{
	explicit DictEntryDiskPayload_t ( bool bPayload, ESphHitless eHitless )
	{
		m_bPayload = bPayload;
		m_eHitless = eHitless;
		if ( bPayload )
			m_dWordPayload.Reserve ( 1000 );

		m_dWordExpand.Reserve ( 1000 );
		m_dWordBuf.Reserve ( 8096 );
	}

	void Add ( const CSphDictEntry & tWord, int iWordLen )
	{
		if ( !m_bPayload || !sphIsExpandedPayload ( tWord.m_iDocs, tWord.m_iHits ) ||
			m_eHitless==SPH_HITLESS_ALL || ( m_eHitless==SPH_HITLESS_SOME && ( tWord.m_iDocs & HITLESS_DOC_FLAG )!=0 ) ) // FIXME!!! do we need hitless=some as payloads?
		{
			DiskExpandedEntry_t & tExpand = m_dWordExpand.Add();

			int iOff = m_dWordBuf.GetLength();
			tExpand.m_iNameOff = iOff;
			tExpand.m_iDocs = tWord.m_iDocs;
			tExpand.m_iHits = tWord.m_iHits;
			m_dWordBuf.Resize ( iOff + iWordLen + 1 );
			memcpy ( m_dWordBuf.Begin() + iOff + 1, tWord.m_sKeyword, iWordLen );
			m_dWordBuf[iOff] = (BYTE)iWordLen;

		} else
		{
			DiskExpandedPayload_t & tExpand = m_dWordPayload.Add();
			tExpand.m_iDocs = tWord.m_iDocs;
			tExpand.m_iHits = tWord.m_iHits;
			tExpand.m_uDoclistOff = tWord.m_iDoclistOffset;
			tExpand.m_iDoclistHint = tWord.m_iDoclistHint;
		}
	}

	void Convert ( ISphWordlist::Args_t & tArgs )
	{
		if ( !m_dWordExpand.GetLength() && !m_dWordPayload.GetLength() )
			return;

		int iTotalDocs = 0;
		int iTotalHits = 0;
		if ( m_dWordExpand.GetLength() )
		{
			LimitExpanded ( tArgs.m_iExpansionLimit, m_dWordExpand );

			const BYTE * sBase = m_dWordBuf.Begin();
			ARRAY_FOREACH ( i, m_dWordExpand )
			{
				const DiskExpandedEntry_t & tCur = m_dWordExpand[i];
				int iDocs = tCur.m_iDocs;

				if ( m_eHitless==SPH_HITLESS_SOME )
					iDocs = ( tCur.m_iDocs & HITLESS_DOC_MASK );

				tArgs.AddExpanded ( sBase + tCur.m_iNameOff + 1, sBase[tCur.m_iNameOff], iDocs, tCur.m_iHits );

				iTotalDocs += iDocs;
				iTotalHits += tCur.m_iHits;
			}
		}

		if ( m_dWordPayload.GetLength() )
		{
			LimitExpanded ( tArgs.m_iExpansionLimit, m_dWordPayload );

			DiskSubstringPayload_t * pPayload = new DiskSubstringPayload_t ( m_dWordPayload.GetLength() );
			// sorting by ascending doc-list offset gives some (15%) speed-up too
			sphSort ( m_dWordPayload.Begin(), m_dWordPayload.GetLength(), bind ( &DiskExpandedPayload_t::m_uDoclistOff ) );

			ARRAY_FOREACH ( i, m_dWordPayload )
			{
				const DiskExpandedPayload_t & tCur = m_dWordPayload[i];
				assert ( m_eHitless==SPH_HITLESS_NONE || ( m_eHitless==SPH_HITLESS_SOME && ( tCur.m_iDocs & HITLESS_DOC_FLAG )==0 ) );

				iTotalDocs += tCur.m_iDocs;
				iTotalHits += tCur.m_iHits;
				pPayload->m_dDoclist[i].m_uOff = tCur.m_uDoclistOff;
				pPayload->m_dDoclist[i].m_iLen = tCur.m_iDoclistHint;
			}

			pPayload->m_iTotalDocs = iTotalDocs;
			pPayload->m_iTotalHits = iTotalHits;
			tArgs.m_pPayload = pPayload;
		}
		tArgs.m_iTotalDocs = iTotalDocs;
		tArgs.m_iTotalHits = iTotalHits;
	}

	// sort expansions by frequency desc
	// clip the less frequent ones if needed, as they are likely misspellings
	template < typename T >
	void LimitExpanded ( int iExpansionLimit, CSphVector<T> & dVec ) const
	{
		if ( !iExpansionLimit || dVec.GetLength()<=iExpansionLimit )
			return;

		sphSort ( dVec.Begin(), dVec.GetLength(), ExpandedOrderDesc_T<T>() );
		dVec.Resize ( iExpansionLimit );
	}

	bool								m_bPayload;
	ESphHitless							m_eHitless;
	CSphVector<DiskExpandedEntry_t>		m_dWordExpand;
	CSphVector<DiskExpandedPayload_t>	m_dWordPayload;
	CSphVector<BYTE>					m_dWordBuf;
};

static bool operator < ( const InfixBlock_t & a, const char * b )
{
	return strcmp ( a.m_sInfix, b )<0;
}

static bool operator == ( const InfixBlock_t & a, const char * b )
{
	return strcmp ( a.m_sInfix, b )==0;
}

static bool operator < ( const char * a, const InfixBlock_t & b )
{
	return strcmp ( a, b.m_sInfix )<0;
}


bool sphLookupInfixCheckpoints ( const char * sInfix, int iBytes, const BYTE * pInfixes, const CSphVector<InfixBlock_t> & dInfixBlocks, int iInfixCodepointBytes, CSphVector<DWORD> & dCheckpoints )
{
	assert ( pInfixes );

	char dInfixBuf[3*SPH_MAX_WORD_LEN+4];
	memcpy ( dInfixBuf, sInfix, iBytes );
	dInfixBuf[iBytes] = '\0';

	// lookup block
	int iBlock = FindSpan ( dInfixBlocks, dInfixBuf );
	if ( iBlock<0 )
		return false;
	const BYTE * pBlock = pInfixes + dInfixBlocks[iBlock].m_iOffset;

	// decode block and check for exact infix match
	// block entry is { byte edit_code, byte[] key_append, zint data_len, zint data_deltas[] }
	// zero edit_code marks block end
	BYTE sKey[32];
	while (true)
	{
		// unpack next key
		int iCode = *pBlock++;
		if ( !iCode )
			break;

		BYTE * pOut = sKey;
		if ( iInfixCodepointBytes==1 )
		{
			pOut = sKey + ( iCode>>4 );
			iCode &= 15;
			while ( iCode-- )
				*pOut++ = *pBlock++;
		} else
		{
			int iKeep = ( iCode>>4 );
			while ( iKeep-- )
				pOut += sphUtf8CharBytes ( *pOut ); ///< wtf? *pOut (=sKey) is NOT initialized?
			assert ( pOut-sKey<=(int)sizeof(sKey) );
			iCode &= 15;
			while ( iCode-- )
			{
				int i = sphUtf8CharBytes ( *pBlock );
				while ( i-- )
					*pOut++ = *pBlock++;
			}
			assert ( pOut-sKey<=(int)sizeof(sKey) );
		}
		assert ( pOut-sKey<(int)sizeof(sKey) );
#ifndef NDEBUG
		*pOut = '\0'; // handy for debugging, but not used for real matching
#endif

		if ( pOut==sKey+iBytes && memcmp ( sKey, dInfixBuf, iBytes )==0 )
		{
			// found you! decompress the data
			int iLast = 0;
			int iPackedLen = sphUnzipInt ( pBlock );
			const BYTE * pMax = pBlock + iPackedLen;
			while ( pBlock<pMax )
			{
				iLast += sphUnzipInt ( pBlock );
				dCheckpoints.Add ( (DWORD)iLast );
			}
			return true;
		}

		int iSkip = sphUnzipInt ( pBlock );
		pBlock += iSkip;
	}
	return false;
}


// calculate length, upto iInfixCodepointBytes chars from infix start
int sphGetInfixLength ( const char * sInfix, int iBytes, int iInfixCodepointBytes )
{
	int iBytes1 = Min ( 6, iBytes );
	if ( iInfixCodepointBytes!=1 )
	{
		int iCharsLeft = 6;
		const char * s = sInfix;
		const char * sMax = sInfix + iBytes;
		while ( iCharsLeft-- && s<sMax )
			s += sphUtf8CharBytes(*s);
		iBytes1 = (int)( s - sInfix );
	}

	return iBytes1;
}


static int BuildUtf8Offsets ( const char * sWord, int iLen, int * pOff, int DEBUGARG ( iBufSize ) )
{
	const BYTE * s = (const BYTE *)sWord;
	const BYTE * sEnd = s + iLen;
	int * pStartOff = pOff;
	*pOff = 0;
	pOff++;
	while ( s<sEnd )
	{
		sphUTF8Decode ( s );
		*pOff = int ( s-(const BYTE *)sWord );
		pOff++;
	}
	assert ( pOff-pStartOff<iBufSize );
	return int ( pOff - pStartOff - 1 );
}

void sphBuildNGrams ( const char * sWord, int iLen, char cDelimiter, CSphVector<char> & dNGrams )
{
	int dOff[SPH_MAX_WORD_LEN+1];
	int iCodepoints = BuildUtf8Offsets ( sWord, iLen, dOff, sizeof ( dOff ) );
	if ( iCodepoints<3 )
		return;

	dNGrams.Reserve ( iLen*3 );
	for ( int iChar=0; iChar<=iCodepoints-3; iChar++ )
	{
		int iStart = dOff[iChar];
		int iEnd = dOff[iChar+3];
		int iGramLen = iEnd - iStart;

		char * sDst = dNGrams.AddN ( iGramLen + 1 );
		memcpy ( sDst, sWord+iStart, iGramLen );
		sDst[iGramLen] = cDelimiter;
	}
	// n-grams split by delimiter
	// however it's still null terminated
	dNGrams.Last() = '\0';
}

template <typename T>
int sphLevenshtein ( const T * sWord1, int iLen1, const T * sWord2, int iLen2 )
{
	if ( !iLen1 )
		return iLen2;
	if ( !iLen2 )
		return iLen1;

	int dTmp [ 3*SPH_MAX_WORD_LEN+1 ]; // FIXME!!! remove extra length after utf8->codepoints conversion

	for ( int i=0; i<=iLen2; i++ )
		dTmp[i] = i;

	for ( int i=0; i<iLen1; i++ )
	{
		dTmp[0] = i+1;
		int iWord1 = sWord1[i];
		int iDist = i;

		for ( int j=0; j<iLen2; j++ )
		{
			int iDistNext = dTmp[j+1];
			dTmp[j+1] = ( iWord1==sWord2[j] ? iDist : ( 1 + Min ( Min ( iDist, iDistNext ), dTmp[j] ) ) );
			iDist = iDistNext;
		}
	}

	return dTmp[iLen2];
}

int sphLevenshtein ( const char * sWord1, int iLen1, const char * sWord2, int iLen2 )
{
	return sphLevenshtein<char> ( sWord1, iLen1, sWord2, iLen2 );
}

int sphLevenshtein ( const int * sWord1, int iLen1, const int * sWord2, int iLen2 )
{
	return sphLevenshtein<int> ( sWord1, iLen1, sWord2, iLen2 );
}

// sort by distance(uLen) desc, checkpoint index(uOff) asc
struct CmpHistogram_fn
{
	inline bool IsLess ( const Slice_t & a, const Slice_t & b ) const
	{
		return ( a.m_uLen>b.m_uLen || ( a.m_uLen==b.m_uLen && a.m_uOff<b.m_uOff ) );
	}
};

// convert utf8 to unicode string
static int DecodeUtf8 ( const BYTE * sWord, int * pBuf )
{
	if ( !sWord )
		return 0;

	int * pCur = pBuf;
	while ( *sWord )
	{
		*pCur = sphUTF8Decode ( sWord );
		pCur++;
	}
	return int ( pCur - pBuf );
}


bool SuggestResult_t::SetWord ( const char * sWord, const ISphTokenizer * pTok, bool bUseLastWord )
{
	assert ( pTok->IsQueryTok() );
	TokenizerRefPtr_c pTokenizer ( pTok->Clone ( SPH_CLONE ) );
	pTokenizer->SetBuffer ( (BYTE *)const_cast<char*>(sWord), (int) strlen ( sWord ) );

	const BYTE * pToken = pTokenizer->GetToken();
	for ( ; pToken!=NULL; )
	{
		m_sWord = (const char *)pToken;
		if ( !bUseLastWord )
			break;

		if ( pTokenizer->TokenIsBlended() )
			pTokenizer->SkipBlended();
		pToken = pTokenizer->GetToken();
	}


	m_iLen = m_sWord.Length();
	m_iCodepoints = DecodeUtf8 ( (const BYTE *)m_sWord.cstr(), m_dCodepoints );
	m_bUtf8 = ( m_iCodepoints!=m_iLen );

	bool bValidWord = ( m_iCodepoints>=3 );
	if ( bValidWord )
		sphBuildNGrams ( m_sWord.cstr(), m_iLen, '\0', m_dTrigrams );

	return bValidWord;
}

void SuggestResult_t::Flattern ( int iLimit )
{
	int iCount = Min ( m_dMatched.GetLength(), iLimit );
	m_dMatched.Resize ( iCount );
}

struct SliceInt_t
{
	int		m_iOff;
	int		m_iEnd;
};

static void SuggestGetChekpoints ( const ISphWordlistSuggest * pWordlist, int iInfixCodepointBytes, const CSphVector<char> & dTrigrams, CSphVector<Slice_t> & dCheckpoints, SuggestResult_t & tStats )
{
	CSphVector<DWORD> dWordCp; // FIXME!!! add mask that trigram matched
	// v1 - current index, v2 - end index
	CSphVector<SliceInt_t> dMergeIters;

	int iReserveLen = 0;
	int iLastLen = 0;
	const char * sTrigram = dTrigrams.Begin();
	const char * sTrigramEnd = sTrigram + dTrigrams.GetLength();
	while (true)
	{
		auto iTrigramLen = (int) strlen ( sTrigram );
		int iInfixLen = sphGetInfixLength ( sTrigram, iTrigramLen, iInfixCodepointBytes );

		// count how many checkpoint we will get
		iReserveLen = Max ( iReserveLen, dWordCp.GetLength () - iLastLen );
		iLastLen = dWordCp.GetLength();

		dMergeIters.Add().m_iOff = dWordCp.GetLength();
		pWordlist->SuffixGetChekpoints ( tStats, sTrigram, iInfixLen, dWordCp );

		sTrigram += iTrigramLen + 1;
		if ( sTrigram>=sTrigramEnd )
			break;

		if ( sphInterrupted() )
			return;
	}
	if ( !dWordCp.GetLength() )
		return;

	for ( int i=0; i<dMergeIters.GetLength()-1; i++ )
	{
		dMergeIters[i].m_iEnd = dMergeIters[i+1].m_iOff;
	}
	dMergeIters.Last().m_iEnd = dWordCp.GetLength();

	// v1 - checkpoint index, v2 - checkpoint count
	dCheckpoints.Reserve ( iReserveLen );
	dCheckpoints.Resize ( 0 );

	// merge sorting of already ordered checkpoints
	while (true)
	{
		DWORD iMinCP = UINT_MAX;
		DWORD iMinIndex = UINT_MAX;
		ARRAY_FOREACH ( i, dMergeIters )
		{
			const SliceInt_t & tElem = dMergeIters[i];
			if ( tElem.m_iOff<tElem.m_iEnd && dWordCp[tElem.m_iOff]<iMinCP )
			{
				iMinIndex = i;
				iMinCP = dWordCp[tElem.m_iOff];
			}
		}

		if ( iMinIndex==UINT_MAX )
			break;

		if ( dCheckpoints.GetLength()==0 || iMinCP!=dCheckpoints.Last().m_uOff )
		{
			dCheckpoints.Add().m_uOff = iMinCP;
			dCheckpoints.Last().m_uLen = 1;
		} else
		{
			dCheckpoints.Last().m_uLen++;
		}

		assert ( iMinIndex!=UINT_MAX && iMinCP!=UINT_MAX );
		assert ( dMergeIters[iMinIndex].m_iOff<dMergeIters[iMinIndex].m_iEnd );
		dMergeIters[iMinIndex].m_iOff++;
	}
	dCheckpoints.Sort ( CmpHistogram_fn() );
}


struct CmpSuggestOrder_fn
{
	bool IsLess ( const SuggestWord_t & a, const SuggestWord_t & b ) const
	{
		if ( a.m_iDistance==b.m_iDistance )
			return a.m_iDocs>b.m_iDocs;

		return a.m_iDistance<b.m_iDistance;
	}
};


static void SuggestMergeDocs ( CSphVector<SuggestWord_t> & dMatched )
{
	if ( !dMatched.GetLength() )
		return;

	dMatched.Sort ( bind ( &SuggestWord_t::m_iNameHash ) );

	int iSrc = 1;
	int iDst = 1;
	while ( iSrc<dMatched.GetLength() )
	{
		if ( dMatched[iDst-1].m_iNameHash==dMatched[iSrc].m_iNameHash )
		{
			dMatched[iDst-1].m_iDocs += dMatched[iSrc].m_iDocs;
			iSrc++;
		} else
		{
			dMatched[iDst++] = dMatched[iSrc++];
		}
	}

	dMatched.Resize ( iDst );
}

template <bool SINGLE_BYTE_CHAR>
void SuggestMatchWords ( const ISphWordlistSuggest * pWordlist, const CSphVector<Slice_t> & dCheckpoints, const SuggestArgs_t & tArgs, SuggestResult_t & tRes )
{
	// walk those checkpoints, check all their words

	const int iMinWordLen = ( tArgs.m_iDeltaLen>0 ? Max ( 0, tRes.m_iCodepoints - tArgs.m_iDeltaLen ) : -1 );
	const int iMaxWordLen = ( tArgs.m_iDeltaLen>0 ? tRes.m_iCodepoints + tArgs.m_iDeltaLen : INT_MAX );

	OpenHash_T<int, int64_t, HashFunc_Int64_t> dHashTrigrams;
	const char * sBuf = tRes.m_dTrigrams.Begin ();
	const char * sEnd = sBuf + tRes.m_dTrigrams.GetLength();
	while ( sBuf<sEnd )
	{
		dHashTrigrams.Add ( sphCRC32 ( sBuf ), 1 );
		while ( *sBuf ) sBuf++;
		sBuf++;
	}
	int dCharOffset[SPH_MAX_WORD_LEN+1];
	int dDictWordCodepoints[SPH_MAX_WORD_LEN];

	const int iQLen = Max ( tArgs.m_iQueueLen, tArgs.m_iLimit );
	const int iRejectThr = tArgs.m_iRejectThr;
	int iQueueRejected = 0;
	int iLastBad = 0;
	bool bSorted = true;
	const bool bMergeWords = tRes.m_bMergeWords;
	const bool bHasExactDict = tRes.m_bHasExactDict;
	const int iMaxEdits = tArgs.m_iMaxEdits;
	const bool bNonCharAllowed = tArgs.m_bNonCharAllowed;
	tRes.m_dMatched.Reserve ( iQLen * 2 );
	CmpSuggestOrder_fn fnCmp;

	ARRAY_FOREACH ( i, dCheckpoints )
	{
		DWORD iCP = dCheckpoints[i].m_uOff;
		pWordlist->SetCheckpoint ( tRes, iCP );

		ISphWordlistSuggest::DictWord_t tWord;
		while ( pWordlist->ReadNextWord ( tRes, tWord ) )
		{
			const char * sDictWord = tWord.m_sWord;
			int iDictWordLen = tWord.m_iLen;
			int iDictCodepoints = iDictWordLen;

			// for stemmer \ lematizer suggest should match only original words
			if ( bHasExactDict && sDictWord[0]!=MAGIC_WORD_HEAD_NONSTEMMED )
				continue;

			if ( bHasExactDict )
			{
				// skip head MAGIC_WORD_HEAD_NONSTEMMED char
				sDictWord++;
				iDictWordLen--;
				iDictCodepoints--;
			}

			if_const ( SINGLE_BYTE_CHAR )
			{
				if ( iDictWordLen<=iMinWordLen || iDictWordLen>=iMaxWordLen )
					continue;
			}

			int iChars = 0;

			const BYTE * s = (const BYTE *)sDictWord;
			const BYTE * sDictWordEnd = s + iDictWordLen;
			bool bGotNonChar = false;
			while ( !bGotNonChar && s<sDictWordEnd )
			{
				dCharOffset[iChars] = int ( s - (const BYTE *)sDictWord );
				int iCode = sphUTF8Decode ( s );
				if ( !bNonCharAllowed )
					bGotNonChar = ( iCode<'A' || ( iCode>'Z' && iCode<'a' ) ); // skip words with any numbers or special characters

				if_const ( !SINGLE_BYTE_CHAR )
				{
					dDictWordCodepoints[iChars] = iCode;
				}
				iChars++;
			}
			dCharOffset[iChars] = int ( s - (const BYTE *)sDictWord );
			iDictCodepoints = iChars;

			if_const ( !SINGLE_BYTE_CHAR )
			{
				if ( iDictCodepoints<=iMinWordLen || iDictCodepoints>=iMaxWordLen )
					continue;
			}

			// skip word in case of non char symbol found
			if ( bGotNonChar )
				continue;

			// FIXME!!! should we skip in such cases
			// utf8 reference word			!=	single byte dictionary word
			// single byte reference word	!=	utf8 dictionary word

			bool bGotMatch = false;
			for ( int iChar=0; iChar<=iDictCodepoints-3 && !bGotMatch; iChar++ )
			{
				int iStart = dCharOffset[iChar];
				int iEnd = dCharOffset[iChar+3];
				bGotMatch = ( dHashTrigrams.Find ( sphCRC32 ( sDictWord + iStart, iEnd - iStart ) )!=NULL );
			}

			// skip word in case of no trigrams matched
			if ( !bGotMatch )
				continue;

			int iDist = INT_MAX;
			if_const ( SINGLE_BYTE_CHAR )
				iDist = sphLevenshtein ( tRes.m_sWord.cstr(), tRes.m_iLen, sDictWord, iDictWordLen );
			else
				iDist = sphLevenshtein ( tRes.m_dCodepoints, tRes.m_iCodepoints, dDictWordCodepoints, iDictCodepoints );

			// skip word in case of too many edits
			if ( iDist>iMaxEdits )
				continue;

			SuggestWord_t tElem;
			tElem.m_iNameOff = tRes.m_dBuf.GetLength();
			tElem.m_iLen = iDictWordLen;
			tElem.m_iDistance = iDist;
			tElem.m_iDocs = tWord.m_iDocs;

			// store in k-buffer up to 2*QLen words
			if ( !iLastBad || fnCmp.IsLess ( tElem, tRes.m_dMatched[iLastBad] ) )
			{
				tElem.m_iNameHash = bMergeWords ? sphCRC32 ( sDictWord, iDictWordLen ) : 0;
				tRes.m_dMatched.Add ( tElem );
				BYTE * sWord = tRes.m_dBuf.AddN ( iDictWordLen+1 );
				memcpy ( sWord, sDictWord, iDictWordLen );
				sWord[iDictWordLen] = '\0';
				iQueueRejected = 0;
				bSorted = false;
			} else
			{
				iQueueRejected++;
			}

			// sort k-buffer in case of threshold overflow
			if ( tRes.m_dMatched.GetLength()>iQLen*2 )
			{
				if ( bMergeWords )
					SuggestMergeDocs ( tRes.m_dMatched );
				int iTotal = tRes.m_dMatched.GetLength();
				tRes.m_dMatched.Sort ( CmpSuggestOrder_fn() );
				bSorted = true;

				// there might be less than necessary elements after merge operation
				if ( iTotal>iQLen )
				{
					iQueueRejected += iTotal - iQLen;
					tRes.m_dMatched.Resize ( iQLen );
				}
				iLastBad = tRes.m_dMatched.GetLength()-1;
			}
		}

		if ( sphInterrupted () )
			break;

		// stop dictionary unpacking in case queue rejects a lot of matched words
		if ( iQueueRejected && iQueueRejected>iQLen*iRejectThr )
			break;
	}

	// sort at least once or any unsorted
	if ( !bSorted )
	{
		if ( bMergeWords )
			SuggestMergeDocs ( tRes.m_dMatched );
		tRes.m_dMatched.Sort ( CmpSuggestOrder_fn() );
	}
}


void sphGetSuggest ( const ISphWordlistSuggest * pWordlist, int iInfixCodepointBytes, const SuggestArgs_t & tArgs, SuggestResult_t & tRes )
{
	assert ( pWordlist );

	CSphVector<Slice_t> dCheckpoints;
	SuggestGetChekpoints ( pWordlist, iInfixCodepointBytes, tRes.m_dTrigrams, dCheckpoints, tRes );
	if ( !dCheckpoints.GetLength() )
		return;

	if ( tRes.m_bUtf8 )
		SuggestMatchWords<false> ( pWordlist, dCheckpoints, tArgs, tRes );
	else
		SuggestMatchWords<true> ( pWordlist, dCheckpoints, tArgs, tRes );

	if ( sphInterrupted() )
		return;

	tRes.Flattern ( tArgs.m_iLimit );
}

//////////////////////////////////////////////////////////////////////////
// CSphQueryResultMeta
//////////////////////////////////////////////////////////////////////////

void RemoveDictSpecials ( CSphString & sWord )
{
	if ( sWord.cstr()[0]==MAGIC_WORD_HEAD )
	{
		*const_cast<char *>( sWord.cstr() ) = '*';
	} else if ( sWord.cstr()[0]==MAGIC_WORD_HEAD_NONSTEMMED )
	{
		*const_cast<char *>( sWord.cstr() ) = '=';
	} else
	{
		const char * p = strchr ( sWord.cstr(), MAGIC_WORD_BIGRAM );
		if ( p )
			*const_cast<char *>(p) = ' ';
	}
}

const CSphString & RemoveDictSpecials ( const CSphString & sWord, CSphString & sFixed )
{
	const CSphString * pFixed = &sWord;
	if ( sWord.cstr()[0]==MAGIC_WORD_HEAD )
	{
		sFixed = sWord;
		*const_cast<char *>( sFixed.cstr() ) = '*';
		pFixed = &sFixed;
	} else if ( sWord.cstr()[0]==MAGIC_WORD_HEAD_NONSTEMMED )
	{
		sFixed = sWord;
		*const_cast<char *>( sFixed.cstr() ) = '=';
		pFixed = &sFixed;
	} else
	{
		const char * p = strchr ( sWord.cstr(), MAGIC_WORD_BIGRAM );
		if ( p )
		{
			sFixed.SetSprintf ( "\"%s\"", sWord.cstr() );
			*( const_cast<char *> ( sFixed.cstr() ) + ( p - sWord.cstr() ) + 1 ) = ' ';
			pFixed = &sFixed;
		}
	}

	return *pFixed;
}

void CSphQueryResultMeta::AddStat ( const CSphString & sWord, int64_t iDocs, int64_t iHits )
{
	CSphString sBuf;
	const CSphString & tFixed = RemoveDictSpecials ( sWord, sBuf );
	WordStat_t & tStats = m_hWordStats.AddUnique ( tFixed );
	tStats.first += iDocs;
	tStats.second += iHits;
}


void CSphQueryResultMeta::MergeWordStats ( const CSphQueryResultMeta & tOther )
{
	const auto & hOtherStats = tOther.m_hWordStats;
	if ( !m_hWordStats.GetLength () )
		// nothing has been set yet; just copy
		m_hWordStats = hOtherStats;
	else
		for ( auto & tStat : hOtherStats )
			AddStat ( tStat.first, tStat.second.first, tStat.second.second );
}

///< sort wordstat to achieve reproducable result over different runs
CSphFixedVector<SmallStringHash_T<CSphQueryResultMeta::WordStat_t>::KeyValue_t *> CSphQueryResultMeta::MakeSortedWordStat () const
{
	using kv_t = SmallStringHash_T<WordStat_t>::KeyValue_t;
	CSphFixedVector<kv_t*> dWords { m_hWordStats.GetLength() };

	int i = 0;
	for ( auto & tStat : m_hWordStats )
		dWords[i++] = &tStat;

	dWords.Sort ( Lesser ( [] ( kv_t * l, kv_t * r ) { return l->first<r->first; } ) );
	return dWords;
}

//////////////////////////////////////////////////////////////////////////

CSphVector<const ISphSchema *> SorterSchemas ( const VecTraits_T<ISphMatchSorter *> & dSorters, int iSkipSorter )
{
	CSphVector<const ISphSchema *> dSchemas;
	if ( !dSorters.IsEmpty() )
	{
		dSchemas.Reserve ( dSorters.GetLength() - 1 );
		ARRAY_FOREACH ( i, dSorters )
		{
			if ( i==iSkipSorter || !dSorters[i] )
				continue;

			const ISphSchema * pSchema = dSorters[i]->GetSchema();
			dSchemas.Add ( pSchema );
		}
	}
	return dSchemas;
}

std::pair<int, int> GetMaxSchemaIndexAndMatchCapacity ( const VecTraits_T<ISphMatchSorter*> & dSorters )
{
	int iMaxSchemaSize = -1;
	int iMaxSchemaIndex = -1;
	int iMatchPoolSize = 0;
	ARRAY_FOREACH ( i, dSorters )
	{
		iMatchPoolSize += dSorters[i]->GetMatchCapacity();
		if ( dSorters[i]->GetSchema ()->GetAttrsCount ()>iMaxSchemaSize )
		{
			iMaxSchemaSize = dSorters[i]->GetSchema ()->GetAttrsCount ();
			iMaxSchemaIndex = i;
		}
	}
	return {iMaxSchemaIndex, iMatchPoolSize};
}


volatile int &sphGetTFO () noexcept
{
	static int iTFO = 0;
	return iTFO;
}

volatile bool& sphGetbCpuStat () noexcept
{
	static bool bCpuStat = false;
	return bCpuStat;
}
