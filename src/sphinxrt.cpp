//
// Copyright (c) 2017-2021, Manticore Software LTD (https://manticoresearch.com)
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
#include "sphinxint.h"
#include "sphinxrt.h"
#include "sphinxpq.h"
#include "sphinxsearch.h"
#include "sphinxsort.h"
#include "sphinxutils.h"
#include "fileutils.h"
#include "sphinxplugin.h"
#include "icu.h"
#include "sphinxqcache.h"
#include "attribute.h"
#include "killlist.h"
#include "histogram.h"
#include "accumulator.h"
#include "indexcheck.h"
#include "indexsettings.h"
#include "indexformat.h"
#include "coroutine.h"
#include "mini_timer.h"
#include "binlog.h"
#include "memio.h"
#include "secondaryindex.h"
#include "columnarrt.h"
#include "columnarmisc.h"
#include "sphinx_alter.h"
#include "taskoptimize.h"
#include "chunksearchctx.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <atomic>

#if _WIN32
#include <errno.h>
#else
#include <unistd.h>
#include <sys/time.h>

#endif

using namespace Threads;

//////////////////////////////////////////////////////////////////////////

#define RTDICT_CHECKPOINT_V5			48
#define SPH_RT_DOUBLE_BUFFER_PERCENT	10

//////////////////////////////////////////////////////////////////////////

#ifndef NDEBUG
#define Verify(_expr) assert(_expr)
#else
#define Verify(_expr) _expr
#endif

#define LOG_LEVEL_RTRDIAG false
#define LOG_LEVEL_RTDDIAG false
#define LOG_LEVEL_RTDIAGV false
#define LOG_LEVEL_RTDIAGVV false
#define LOG_LEVEL_DEBUGV false
#define LOG_COMPONENT_RTSEG __LINE__ << " " << CoCurrentScheduler()->Name() << " "

#define RTRLOG LOGINFO ( RTRDIAG, RTSEG )
#define RTDLOG LOGINFO ( RTDDIAG, RTSEG )
#define RTLOGV LOGINFO ( RTDIAGV, RTSEG )
#define RTLOGVV LOGINFO ( RTDIAGVV, RTSEG )

//////////////////////////////////////////////////////////////////////////
// GLOBALS
//////////////////////////////////////////////////////////////////////////

/// check for concurrent changes during binlog replay (used only in asserts)
static auto&	g_bRTChangesAllowed		= RTChangesAllowed ();

// optimize mode for disk chunks merge fixme! retire?
static bool g_bProgressiveMerge = true;

//////////////////////////////////////////////////////////////////////////
volatile bool &RTChangesAllowed () noexcept
{
	static volatile bool bRTChangesAllowed = false;
	return bRTChangesAllowed;
}

volatile int &AutoOptimizeCutoffMultiplier() noexcept
{
	static int iAutoOptimizeCutoffMultiplier = 1;
	return iAutoOptimizeCutoffMultiplier;
}

volatile int &AutoOptimizeCutoff() noexcept
{
	static int iAutoOptimizeCutoff = sphCpuThreadsCount() * 2;
	return iAutoOptimizeCutoff;
}

volatile EnqueueForOptimizeFnPtr& EnqueueForOptimizeExecutor() noexcept
{
	static EnqueueForOptimizeFnPtr EnqueueForOptimizeFn = nullptr;
	return EnqueueForOptimizeFn;
}

void EnqueueForOptimizeWeak ( CSphString sIndex, OptimizeTask_t tTask )
{
	auto * EnqueueForOptimizeFn = EnqueueForOptimizeExecutor();
	if (EnqueueForOptimizeFn)
		EnqueueForOptimizeFn ( std::move ( sIndex ), std::move ( tTask ) );
}

//////////////////////////////////////////////////////////////////////////

// Variable Length Byte (VLB) encoding
// store int variable in as much bytes as actually needed to represent it
template < typename T, typename P >
static inline void ZipT ( CSphVector < BYTE, P > & dOut, T uValue )
{
	do
	{
		BYTE bOut = (BYTE)( uValue & 0x7f );
		uValue >>= 7;
		if ( uValue )
			bOut |= 0x80;
		dOut.Add ( bOut );
	} while ( uValue );
}

/* // note difference: sphZipValue packs in BE order (most significant bits cames first)
 template<typename T, typename WRITER>
inline int sphZipValue ( WRITER fnPut, T tValue )
{
	int nBytes = sphCalcZippedLen ( tValue );
	for ( int i = nBytes-1; i>=0; --i )
		fnPut ( ( 0x7f & ( tValue >> ( 7 * i ) ) ) | ( i ? 0x80 : 0 ) );

	return nBytes;
}
 */

#define SPH_MAX_KEYWORD_LEN (3*SPH_MAX_WORD_LEN+4)
STATIC_ASSERT ( SPH_MAX_KEYWORD_LEN<255, MAX_KEYWORD_LEN_SHOULD_FITS_BYTE );


// Variable Length Byte (VLB) decoding
template < typename T >
static inline void UnzipT ( T * pValue, const BYTE *& pIn )
{
	T uValue = 0;
	BYTE uIn;
	int iOff = 0;

	do
	{
		uIn = *pIn++;
		uValue += ( T ( uIn & 0x7FU ) ) << iOff;
		iOff += 7;
	} while ( uIn & 0x80U );

	*pValue = uValue;
}

template < typename T >
static inline T UnzipT ( const BYTE *& pIn )
{
	T uValue = 0;
	BYTE uIn;
	int iOff = 0;

	do
	{
		uIn = *pIn++;
		uValue += ( T ( uIn & 0x7FU ) ) << iOff;
		iOff += 7;
	} while ( uIn & 0x80U );

	return uValue;
}

/* // Note difference: in code below zipped bytes came in BE order (most significant first)
 * #define SPH_VARINT_DECODE(_type,_getexpr) \
	register DWORD b = _getexpr; \
	register _type res = 0; \
	while ( b & 0x80 ) \
	{ \
		res = ( res<<7 ) + ( b & 0x7f ); \
		b = _getexpr; \
	} \
	res = ( res<<7 ) + b; \
	return res;

 */

// Variable Length Byte (VLB) skipping (BE/LE agnostic)
static inline void SkipZipped ( const BYTE *& pIn )
{
	while ( *pIn & 0x80U )
		++pIn;
	++pIn; // jump over last one
}

#define ZipDword ZipT<DWORD>
#define ZipQword ZipT<uint64_t>
#define UnzipDword UnzipT<DWORD>
#define UnzipQword UnzipT<uint64_t>

#define ZipDocid ZipQword
#define ZipWordid ZipQword
#define UnzipWordid UnzipQword

//////////////////////////////////////////////////////////////////////////

InsertDocData_t::InsertDocData_t ( const ISphSchema & tSchema )
{
	m_tDoc.Reset ( tSchema.GetRowSize() );
	m_dFields.Resize ( tSchema.GetFieldsCount() );

	const CSphColumnInfo * pId = tSchema.GetAttr ( sphGetDocidName() );
	assert(pId);

	if ( pId->IsColumnar() )
	{
		int iColumnar = 0;
		for ( int i = 0; i < tSchema.GetAttrsCount(); i++ )
		{
			if ( tSchema.GetAttr(i).m_sName == sphGetDocidName() )
				m_iColumnarID = iColumnar;

			iColumnar++;
		}

		m_dColumnarAttrs.Resize(iColumnar);
	}
	else
	{
		m_tDocIDLocator = pId->m_tLocator;
		m_tDocIDLocator.m_bDynamic = true;
	}
}

void InsertDocData_t::SetID ( SphAttr_t tDocID )
{
	if ( m_iColumnarID>=0 )
	{
		m_dColumnarAttrs[m_iColumnarID] = tDocID;
		return;
	}
	
	m_tDoc.SetAttr ( m_tDocIDLocator, tDocID );
}


SphAttr_t InsertDocData_t::GetID() const
{
	if ( m_iColumnarID>=0 )
		return m_dColumnarAttrs[m_iColumnarID];

	return m_tDoc.GetAttr(m_tDocIDLocator);
}

//////////////////////////////////////////////////////////////////////////

struct CmpHitPlain_fn
{
	inline static bool IsLess ( const CSphWordHit & a, const CSphWordHit & b )
	{
		return 	( a.m_uWordID<b.m_uWordID ) ||
			( a.m_uWordID==b.m_uWordID && a.m_tRowID<b.m_tRowID ) ||
			( a.m_uWordID==b.m_uWordID && a.m_tRowID==b.m_tRowID && a.m_uWordPos<b.m_uWordPos );
	}
};


struct CmpHitKeywords_fn
{
	const BYTE * m_pBase;
	explicit CmpHitKeywords_fn ( const BYTE * pBase ) : m_pBase ( pBase ) {}
	inline bool IsLess ( const CSphWordHit & a, const CSphWordHit & b ) const
	{
		const BYTE * pPackedA = m_pBase + a.m_uWordID;
		const BYTE * pPackedB = m_pBase + b.m_uWordID;
		int iCmp = sphDictCmpStrictly ( (const char *)pPackedA+1, *pPackedA, (const char *)pPackedB+1, *pPackedB );
		return 	( iCmp<0 ) ||
			( iCmp==0 && a.m_tRowID<b.m_tRowID ) ||
			( iCmp==0 && a.m_tRowID==b.m_tRowID && a.m_uWordPos<b.m_uWordPos );
	}
};


RtSegment_t::RtSegment_t ( DWORD uDocs )
	: m_tDeadRowMap ( uDocs )
{
}

RtSegment_t::~RtSegment_t ()
{
	if ( m_pRAMCounter )
		FixupRAMCounter ( -GetUsedRam() );
}


void RtSegment_t::UpdateUsedRam() const NO_THREAD_SAFETY_ANALYSIS
{
	int64_t iUsedRam = 0;
	iUsedRam += m_dWords.AllocatedBytes();
	iUsedRam += m_dDocs.AllocatedBytes();
	iUsedRam += m_dHits.AllocatedBytes();
	iUsedRam += m_dBlobs.AllocatedBytes();
	iUsedRam += m_dKeywordCheckpoints.AllocatedBytes();
	iUsedRam += m_dRows.AllocatedBytes();
	iUsedRam += m_dInfixFilterCP.AllocatedBytes();
	iUsedRam += m_pDocstore.Ptr() ? m_pDocstore->AllocatedBytes() : 0;
	iUsedRam += m_pColumnar.Ptr() ? m_pColumnar->AllocatedBytes() : 0;
	FixupRAMCounter ( iUsedRam - std::exchange ( m_iUsedRam, iUsedRam ) );
}


int64_t RtSegment_t::GetUsedRam() const
{
	if ( !m_iUsedRam )
		UpdateUsedRam();

	return m_iUsedRam;
}


void RtSegment_t::FixupRAMCounter ( int64_t iDelta ) const
{
	if ( !m_pRAMCounter )
		return;

	m_pRAMCounter->fetch_add ( iDelta, std::memory_order_relaxed );
}


DWORD RtSegment_t::GetMergeFactor() const
{
	return m_uRows;
}


int RtSegment_t::GetStride () const NO_THREAD_SAFETY_ANALYSIS
{
	return int ( m_dRows.GetLength() / m_uRows );
}

const CSphRowitem * RtSegment_t::FindAliveRow ( DocID_t tDocid ) const
{
	RowID_t tRowID = GetRowidByDocid(tDocid);
	if ( tRowID==INVALID_ROWID || m_tDeadRowMap.IsSet(tRowID) )
		return nullptr;

	return GetDocinfoByRowID(tRowID);
}


const CSphRowitem * RtSegment_t::GetDocinfoByRowID ( RowID_t tRowID ) const NO_THREAD_SAFETY_ANALYSIS
{
	return m_dRows.GetLength() ? &m_dRows[tRowID*GetStride()] : nullptr;
}


RowID_t RtSegment_t::GetRowidByDocid ( DocID_t tDocID ) const
{
	RowID_t * pRowID = m_tDocIDtoRowID.Find(tDocID);
	return pRowID ? *pRowID : INVALID_ROWID;
}


int RtSegment_t::Kill ( DocID_t tDocID )
{
	if ( m_tDeadRowMap.Set ( GetRowidByDocid ( tDocID ) ) )
	{
		assert ( m_tAliveRows>0 );
		m_tAliveRows.fetch_sub ( 1, std::memory_order_relaxed );

		// on runtime killing expected only from single fiber, so no mutex here required as no race for pKillHook
		if ( m_pKillHook )
			m_pKillHook->Kill ( tDocID );
		return 1;
	}

	return 0;
}


int	RtSegment_t::KillMulti ( const VecTraits_T<DocID_t> & dKlist )
{
	int iTotalKilled = 0;

	// fixme: implement more efficient batch killer
	for ( auto iDocID : dKlist )
		iTotalKilled += Kill ( iDocID );

	return iTotalKilled;
}


void RtSegment_t::SetupDocstore ( const CSphSchema * pSchema )
{
	assert ( !m_pDocstore );
	m_pDocstore = CreateDocstoreRT();
	assert ( m_pDocstore );

	if ( pSchema )
		SetupDocstoreFields ( *m_pDocstore, *pSchema );
}


void RtSegment_t::BuildDocID2RowIDMap ( const CSphSchema & tSchema )
{
	m_tDocIDtoRowID.Reset(m_uRows);

	if ( !tSchema.GetAttr(0).IsColumnar() )
	{
		int iStride = GetStride();
		RowID_t tRowID = 0;
		FakeRL_t _ {m_tLock}; // no need true lock as the func is in game during build/merge when segment is not yet published

		for ( int i=0; i<m_dRows.GetLength(); i+=iStride )
			m_tDocIDtoRowID.Add ( sphGetDocID ( &m_dRows[i] ), tRowID++ );
	}
	else
	{
		std::string sError;
		CSphScopedPtr<columnar::Iterator_i> pIt ( m_pColumnar->CreateIterator ( sphGetDocidName(), {}, sError ) );
		assert ( pIt.Ptr() );
		for ( RowID_t tRowID = 0; tRowID<m_uRows; tRowID++ )
		{
			Verify ( AdvanceIterator ( pIt.Ptr(), tRowID ) );
			m_tDocIDtoRowID.Add ( pIt->Get(), tRowID );
		}
	}
}

//////////////////////////////////////////////////////////////////////////

class RtDocWriter_c
{
	CSphTightVector<BYTE> &		m_dDocs;
	RowID_t						m_tLastRowID {INVALID_ROWID};

public:
	explicit RtDocWriter_c ( CSphTightVector<BYTE> & dDocs )
		: m_dDocs ( dDocs )
	{}

	inline void operator<< ( const RtDoc_t & tDoc)
	{
		m_dDocs.ReserveGap ( 5 + 5 * sizeof ( DWORD ) );
		ZipDword ( m_dDocs, tDoc.m_tRowID - std::exchange ( m_tLastRowID, tDoc.m_tRowID ) );
		ZipDword ( m_dDocs, tDoc.m_uDocFields );
		ZipDword ( m_dDocs, tDoc.m_uHits );
		if ( tDoc.m_uHits == 1 )
		{
			ZipDword ( m_dDocs, tDoc.m_uHit & 0xffffffUL );
			ZipDword ( m_dDocs, tDoc.m_uHit >> 24 );
		} else
			ZipDword ( m_dDocs, tDoc.m_uHit );
	}

	DWORD WriterPos () const
	{
		return m_dDocs.GetLength();
	}

	void ZipRestart ()
	{
		m_tLastRowID = INVALID_ROWID;
	}
};


RtDocReader_c::RtDocReader_c ( const RtSegment_t * pSeg, const RtWord_t & tWord )
{
	Init ( pSeg, tWord );
}

void RtDocReader_c::Init ( const RtSegment_t* pSeg, const RtWord_t& tWord )
{
	m_pDocs = ( pSeg->m_dDocs.begin() ? pSeg->m_dDocs.begin() + tWord.m_uDoc : nullptr );
	m_iLeft = tWord.m_uDocs;
	m_tDoc.m_tRowID = INVALID_ROWID;
}

bool RtDocReader_c::UnzipDoc ()
{
	if ( !m_iLeft || !m_pDocs )
		return false;
	const BYTE* pIn = m_pDocs;
	m_tDoc.m_tRowID += UnzipDword ( pIn );
	UnzipDword ( &m_tDoc.m_uDocFields, pIn );
	UnzipDword ( &m_tDoc.m_uHits, pIn );
	if ( m_tDoc.m_uHits == 1 )
	{
		auto a = UnzipDword ( pIn );
		auto b = UnzipDword ( pIn );
		m_tDoc.m_uHit = a + ( b << 24 );
	} else
		UnzipDword ( &m_tDoc.m_uHit, pIn );
	m_pDocs = pIn;
	--m_iLeft;
	return true;
}

class RtWordWriter_c
{
	CSphTightVector<BYTE> &				m_dWords;
	CSphVector<RtWordCheckpoint_t> &	m_dCheckpoints;
	CSphVector<BYTE> &					m_dKeywordCheckpoints;

	CSphKeywordDeltaWriter				m_tLastKeyword;
	SphWordID_t							m_uLastWordID;
	DWORD								m_uLastDoc;
	int									m_iWords;

	bool								m_bKeywordDict;
	int									m_iWordsCheckpoint;
	const ESphHitless 					m_eHitlessMode = SPH_HITLESS_NONE;

public:
	RtWordWriter_c ( CSphTightVector<BYTE> & dWords, CSphVector<RtWordCheckpoint_t> & dCheckpoints,
				  CSphVector<BYTE> & dKeywordCheckpoints, bool bKeywordDict, int iWordsCheckpoint, ESphHitless eHitlessMode )
		: m_dWords ( dWords )
		, m_dCheckpoints ( dCheckpoints )
		, m_dKeywordCheckpoints ( dKeywordCheckpoints )
		, m_uLastWordID ( 0 )
		, m_uLastDoc ( 0 )
		, m_iWords ( 0 )
		, m_bKeywordDict ( bKeywordDict )
		, m_iWordsCheckpoint ( iWordsCheckpoint )
		, m_eHitlessMode ( eHitlessMode )
	{
		assert ( dWords.IsEmpty() );
		assert ( dCheckpoints.IsEmpty() );
		assert ( dKeywordCheckpoints.IsEmpty() );
	}

	void operator<< ( const RtWord_t& tWord ) // we never chain calls, so void as return type is ok.
	{
		if ( ++m_iWords==m_iWordsCheckpoint )
		{
			RtWordCheckpoint_t & tCheckpoint = m_dCheckpoints.Add();
			if ( !m_bKeywordDict )
				tCheckpoint.m_uWordID = tWord.m_uWordID;
			else
			{
				int iLen = tWord.m_sWord[0];
				assert ( iLen && iLen-1<SPH_MAX_KEYWORD_LEN );
				tCheckpoint.m_uWordID = sphPutBytes ( &m_dKeywordCheckpoints, tWord.m_sWord+1, iLen+1 );
				m_dKeywordCheckpoints.Last() = '\0'; // checkpoint is NULL terminating string

				// reset keywords delta encoding
				m_tLastKeyword.Reset();
			}
			tCheckpoint.m_iOffset = m_dWords.GetLength();

			m_uLastWordID = 0;
			m_uLastDoc = 0;
			m_iWords = 1;
		}

		if ( !m_bKeywordDict )
			ZipWordid ( m_dWords, tWord.m_uWordID - m_uLastWordID );
		else
			m_tLastKeyword.PutDelta ( *this, tWord.m_sWord+1, tWord.m_sWord[0] );

		m_dWords.ReserveGap ( 3+3*sizeof(DWORD) );

		DWORD uDocs = tWord.m_uDocs;
		if ( !tWord.m_bHasHitlist && m_eHitlessMode==SPH_HITLESS_SOME )
			uDocs |= HITLESS_DOC_FLAG;

		ZipDword ( m_dWords, uDocs );
		ZipDword ( m_dWords, tWord.m_uHits );
		ZipDword ( m_dWords, tWord.m_uDoc - m_uLastDoc );

		m_uLastWordID = tWord.m_uWordID;
		m_uLastDoc = tWord.m_uDoc;
	}

	void PutBytes ( const BYTE * pData, int iLen ) const
	{
		sphPutBytes ( &m_dWords, pData, iLen );
	}
};

RtWordReader_c::RtWordReader_c ( const RtSegment_t * pSeg, bool bWordDict, int iWordsCheckpoint, ESphHitless eHitlessMode )
	: m_bWordDict ( bWordDict )
	, m_iWordsCheckpoint ( iWordsCheckpoint )
	, m_eHitlessMode ( eHitlessMode )
{
	m_tWord.m_uWordID = 0;
	Reset ( pSeg );
	if ( bWordDict )
		m_tWord.m_sWord = m_tPackedWord;
}

void RtWordReader_c::Reset ( const RtSegment_t * pSeg )
{
	m_pCur = pSeg->m_dWords.Begin();
	m_pMax = m_pCur + pSeg->m_dWords.GetLength();

	m_tWord.m_uDoc = 0;
	m_iWords = 0;
}


const RtWord_t* RtWordReader_c::UnzipWord ()
{
	if (m_pCur >= m_pMax)
		return nullptr;

	if ( ++m_iWords==m_iWordsCheckpoint )
	{
		m_tWord.m_uDoc = 0;
		m_iWords = 1;
		++m_iCheckpoint;
		if ( !m_bWordDict )
			m_tWord.m_uWordID = 0;
	}

	const BYTE * pIn = m_pCur;
	if ( m_bWordDict )
	{
		// 1dddmmmm -> delta, match (packed into 1 byte)
		// 0ddddddd mmmmmmmm -> delta, match (occupy 2 bytes)
		// then delta bytes of symbols
		// overwrites previous token: delta symbols starting from match. At the end (delta+match) put 0-terminator
		BYTE iMatch, iDelta, uPacked;
		uPacked = *pIn++;
		if ( uPacked & 0x80 )
		{
			iDelta = ( ( uPacked>>4 ) & 7 ) + 1;
			iMatch = uPacked & 15;
		} else
		{
			iDelta = uPacked & 127;
			iMatch = *pIn++;
		}
		m_tPackedWord[0] = iMatch+iDelta;
		memcpy ( m_tPackedWord+1+iMatch, pIn, iDelta );
		m_tPackedWord[1+m_tPackedWord[0]] = 0;
		pIn += iDelta;
	} else
		m_tWord.m_uWordID += UnzipWordid ( pIn );

	UnzipDword ( &m_tWord.m_uDocs, pIn );
	UnzipDword ( &m_tWord.m_uHits, pIn );
	m_tWord.m_uDoc +=  UnzipDword ( pIn );
	m_tWord.m_bHasHitlist = ( m_eHitlessMode==SPH_HITLESS_NONE || ( m_eHitlessMode==SPH_HITLESS_SOME && !( m_tWord.m_uDocs & HITLESS_DOC_FLAG ) ) );
	m_tWord.m_uDocs = ( m_eHitlessMode==SPH_HITLESS_NONE ? m_tWord.m_uDocs : ( m_tWord.m_uDocs & HITLESS_DOC_MASK ) );
	m_pCur = pIn;
	return &m_tWord;
}


class RtHitWriter_c
{
	CSphTightVector<BYTE>& m_dHits;
	DWORD m_uLastHit = 0;

public:
	explicit RtHitWriter_c ( CSphTightVector<BYTE>& dHits )
		: m_dHits ( dHits )
	{}

	inline void operator<< ( DWORD uValue )
	{
		ZipDword ( m_dHits, uValue - std::exchange ( m_uLastHit, uValue ) );
	}

	void ZipRestart()
	{
		m_uLastHit = 0;
	}

	DWORD WriterPos() const
	{
		return m_dHits.GetLength();
	}
};

RtHitReader_c::RtHitReader_c ( const RtSegment_t& dSeg, const RtDoc_t& dDoc )
	: m_pCur ( dSeg.m_dHits.begin() + dDoc.m_uHit)
	, m_uLeft ( dDoc.m_uHits )
	, m_uValue ( EMPTY_HIT )
{}

void RtHitReader_c::Seek ( const RtSegment_t& dSeg, const RtDoc_t& dDoc )
{
	Seek ( dSeg.m_dHits.begin() + dDoc.m_uHit, dDoc.m_uHits );
}

void RtHitReader_c::Seek ( const BYTE* pHits, DWORD uHits )
{
	m_pCur = pHits;
	m_uLeft = uHits;
	m_uValue = EMPTY_HIT;
}

DWORD RtHitReader_c::UnzipHit ()
{
	if ( !m_pCur || !m_uLeft )
		return EMPTY_HIT;
	m_uValue += UnzipDword ( m_pCur );
	--m_uLeft;
	return m_uValue;
}


ByteBlob_t GetHitsBlob ( const RtSegment_t& tSeg, const RtDoc_t& tDoc )
{
	const BYTE* pHits = &tSeg.m_dHits[tDoc.m_uHit];
	const BYTE* pEnd = pHits;
	for ( auto i = 0U; i < tDoc.m_uHits; ++i )
		SkipZipped ( pEnd );
	return { pHits, pEnd - pHits };
}

//////////////////////////////////////////////////////////////////////////

uint64_t MemoryReader_c::UnzipOffset()
{
	assert ( m_pCur );
	assert ( m_pCur<m_pData+m_iLen );
	return UnzipQword ( m_pCur );
}

DWORD MemoryReader_c::UnzipInt()
{
	assert ( m_pCur );
	assert ( m_pCur<m_pData+m_iLen );
	return UnzipDword ( m_pCur );
}

void MemoryWriter_c::ZipOffset ( uint64_t uVal )
{
	ZipQword ( m_dBuf, uVal );
}

void MemoryWriter_c::ZipInt ( DWORD uVal )
{
	ZipDword ( m_dBuf, uVal );
}
//////////////////////////////////////////////////////////////////////////

/// forward ref
class RtIndex_c;

/// TLS indexing accumulator (we disallow two uncommitted adds within one thread; and so need at most one)
static thread_local RtAccum_t * g_pTlsAccum;


struct ChunkStats_t
{
	CSphSourceStats				m_Stats;
	CSphFixedVector<int64_t>	m_dFieldLens { SPH_MAX_FIELDS };

	void Init ( const CSphSourceStats & s, const CSphFixedVector<int64_t> & dLens )
	{
		assert ( m_dFieldLens.GetLength() == dLens.GetLength() );
		m_Stats = s;
		ARRAY_FOREACH ( i, dLens )
			m_dFieldLens[i] = dLens[i];
	}

	ChunkStats_t () = default;
	ChunkStats_t ( const CSphSourceStats & s, const CSphFixedVector<int64_t> & dLens )
	{
		Init ( s, dLens );
	}
};

struct RtAttrMergeContext_t
{
	int								m_iNumBlobs;
	RowID_t &						m_tResultRowID;
	columnar::Builder_i *			m_pColumnarBuilder;

	RtAttrMergeContext_t ( int iNumBlobs, RowID_t & tResultRowID, columnar::Builder_i * pColumnarBuilder )
		: m_iNumBlobs ( iNumBlobs )
		, m_tResultRowID ( tResultRowID )
		, m_pColumnarBuilder ( pColumnarBuilder )
	{}
};


struct RtQword_t;
struct SaveDiskDataContext_t;

// kind of 'mini served_desc' inside index - manages state of one disk chunk
class DiskChunk_c final : public ISphRefcountedMT
{
	CSphIndex *		m_pIndex;
	bool			m_bOwned;

private:
	DiskChunk_c ( CSphIndex* pIndex, bool bOwned ) : m_pIndex ( pIndex ), m_bOwned ( bOwned ) {}

protected:
	~DiskChunk_c () final
	{
		if ( !m_pIndex || !m_bOwned )
			return;

		CSphString sDeleted = m_pIndex->GetFilename ();
		SafeDelete( m_pIndex );
		if ( m_bFinallyUnlink )
			sphUnlinkIndex ( sDeleted.cstr (), true );
	}

public:
	inline static CSphRefcountedPtr<const DiskChunk_c> make ( CSphIndex* pIndex ) { return CSphRefcountedPtr<const DiskChunk_c> { pIndex ? new DiskChunk_c ( pIndex, true ) : nullptr }; }
	inline static CSphRefcountedPtr<const DiskChunk_c> wrap ( CSphIndex* pIndex ) { return CSphRefcountedPtr<const DiskChunk_c> { pIndex ? new DiskChunk_c ( pIndex, false ) : nullptr }; }
	explicit operator CSphIndex* () const		{ return m_pIndex; }
	CSphIndex & Idx() 					{ return *m_pIndex; }
	CSphIndex & CastIdx () const		{ return *const_cast<CSphIndex *>(m_pIndex); } // const breakage!
	const CSphIndex & Cidx() const		{ return *m_pIndex; }

	mutable std::atomic<bool>			m_bOptimizing { false };	// to protect from simultaneous optimizing one and same chunk
	mutable bool						m_bFinallyUnlink = false; // unlink index files on destroy
	mutable Threads::CoroRWLock_c 		m_tLock;        // fine-grain lock
};

using DiskChunkRefPtr_t = CSphRefcountedPtr<DiskChunk_c>;
using ConstDiskChunkRefPtr_t = CSphRefcountedPtr<const DiskChunk_c>;

using ConstRtSegmentSlice_t = VecTraits_T<ConstRtSegmentRefPtf_t>;
using DiskChunkSlice_t = VecTraits_T<DiskChunkRefPtr_t>;

template <typename CHUNK>
class RefCountedVec_T final : public ISphRefcountedMT, public LazyVector_T<CHUNK>
{
protected:
	~RefCountedVec_T () final = default;

public:
	RefCountedVec_T() = default;
	using BASE = LazyVector_T<CHUNK>;
};

using DiskChunkVec_c = RefCountedVec_T<ConstDiskChunkRefPtr_t>;
using RtSegVec_c = RefCountedVec_T<ConstRtSegmentRefPtf_t>;

using DiskChunkVecRefPtr_t		= CSphRefcountedPtr<DiskChunkVec_c>;
using RtSegVecRefPtr_t			= CSphRefcountedPtr<RtSegVec_c>;
using ConstDiskChunkVecRefPtr_t	= CSphRefcountedPtr<const DiskChunkVec_c>;
using ConstRtSegVecRefPtr_t		= CSphRefcountedPtr<const RtSegVec_c>;

template<typename CHUNK>
CSphRefcountedPtr<RefCountedVec_T<CHUNK>> SliceToVec ( const VecTraits_T<CHUNK> & dSegments )
{
	CSphRefcountedPtr<RefCountedVec_T<CHUNK>> dResult { new RefCountedVec_T<CHUNK> };
	dResult->Reserve ( dSegments.GetLength () );
	for ( const auto & dSeg : dSegments )
		dResult->Add ( dSeg );
	return dResult;
}

struct ConstRtData {
	ConstDiskChunkVecRefPtr_t m_pChunks;
	ConstRtSegVecRefPtr_t m_pSegs;
};
//using MutableRtData = std::pair<DiskChunkVecRefPtr_t, RtSegVecRefPtr_t>;
/*
class FiberPool_c
{
	struct HelperFiber_t
	{
		CSphString							m_sName;
		Threads::RoledSchedulerSharedPtr_t	m_tScheduler;
	};

	CSphFixedVector<HelperFiber_t>	m_dFibers;				// helpers to serialize update/change tasks
	std::atomic<DWORD>				m_iFiber;

public:

	explicit FiberPool_c (int iHelpers)
			: m_dFibers { iHelpers }
			, m_iFiber {0}
	{}

	Threads::RoledSchedulerSharedPtr_t GetFiber ()
	{
		auto iIdx = m_iFiber.fetch_add ( 1, std::memory_order_relaxed ) % m_dFibers.GetLength();
		auto& tSched = m_dFibers[iIdx].m_tScheduler;
		if ( !tSched )
		{
			if ( m_dFibers[iIdx].m_sName.IsEmpty () )
				m_dFibers[iIdx].m_sName.SetSprintf ( "FP_%d", iIdx );

			tSched = MakeAloneScheduler ( CoCurrentScheduler (), m_dFibers[iIdx].m_sName.cstr () );
		}
		return tSched;
	}
};
*/
// main dataset of RT index. Keeps disk chunks and RAM segments
// * provides read-only snapshots for searches
// * provides serialized access for changing chunks and segments
// * provides fiber workers for undependent processing
class RtData_c
{
	mutable RwLock_t			m_tLock;	// guards access to vec of pointers, very short-term
	ConstDiskChunkVecRefPtr_t	m_pChunks GUARDED_BY ( m_tLock );
	ConstRtSegVecRefPtr_t		m_pSegments GUARDED_BY ( m_tLock );
	mutable int					m_iMaxDiskChunkId = -1;
	RoledSchedulerSharedPtr_t	m_tSelectorGuard;		// serialize changing chunks and segs vec
	RoledSchedulerSharedPtr_t	m_tSegmentMerger;		// fiber for merging RAM segments. FiberID=-2 for now
//	FiberPool_c					m_dFibers;				// helpers to serialize update/change tasks
	std::atomic<int>			m_iNextOp {1};
	std::atomic<int>			m_iSaveActive {0};

	friend class RtWriter_c;

public:
	explicit RtData_c ()
//			: m_dFibers { 10 }
	{
		m_pChunks = new DiskChunkVec_c;
		m_pSegments = new RtSegVec_c;
	}

	~RtData_c () = default;

	void InitWorkers()
	{
		if ( !m_tSelectorGuard )
			m_tSelectorGuard = MakeAloneScheduler ( CoCurrentScheduler(), "serial");

		if ( !m_tSegmentMerger )
			m_tSegmentMerger = MakeAloneScheduler ( CoCurrentScheduler (), "merger" );
	}

	int MakeChunkId ()
	{
		if ( m_iMaxDiskChunkId<0 )
		{
			int & iCh = m_iMaxDiskChunkId;
			auto pChunks = DiskChunks();
			pChunks->for_each ( [&iCh] ( ConstDiskChunkRefPtr_t & pIdx ) { iCh = Max ( iCh, pIdx->Cidx().m_iChunk ); } );
		}
		++m_iMaxDiskChunkId;
		return m_iMaxDiskChunkId;
	}

	ConstDiskChunkRefPtr_t DiskChunkByID ( int iChunkID ) const
	{
		ScRL_t rLock ( m_tLock );
		for ( auto& pChunk : *m_pChunks )
			if ( pChunk->Cidx().m_iChunk == iChunkID )
				return pChunk;
		return ConstDiskChunkRefPtr_t (nullptr);
	}

	ConstDiskChunkVecRefPtr_t DiskChunks () const
	{
		ScRL_t rLock ( m_tLock );
		return m_pChunks;
	}

	ConstRtSegVecRefPtr_t RamSegs () const
	{
		ScRL_t rLock ( m_tLock );
		return m_pSegments;
	}

	ConstRtData RtData () const
	{
		ScRL_t rLock ( m_tLock );
		return { m_pChunks, m_pSegments };
	}

	bool IsEmpty() const
	{
		ScRL_t rLock ( m_tLock );
		return m_pChunks->IsEmpty() && m_pSegments->IsEmpty();
	}

	int GetDiskChunksCount () const
	{
		ScRL_t rLock ( m_tLock );
		return m_pChunks->GetLength ();
	}

	ConstRtData RtDataUnl () const REQUIRES ( SerialChunkAccess () ) NO_THREAD_SAFETY_ANALYSIS
	{
		return { m_pChunks, m_pSegments };
	}

	Threads::SchedRole SerialChunkAccess () const RETURN_CAPABILITY ( m_tSelectorGuard )
	{
		return m_tSelectorGuard;
	}

	Threads::SchedRole MergeSegmentsWorker () const RETURN_CAPABILITY ( m_tSegmentMerger )
	{
		return m_tSegmentMerger;
	}

	inline int GetNextOpTicket ()
	{
		auto iRes = m_iNextOp.fetch_add ( 1, std::memory_order_relaxed );
		if ( !iRes ) // zero tag has special meaning, skip it
			iRes = m_iNextOp.fetch_add ( 1, std::memory_order_relaxed );
		return iRes;
	}

	inline void SetSaveActive ( bool bSave )
	{
		m_iSaveActive.fetch_add ( bSave ? 1 : -1, std::memory_order_relaxed );
	}

	inline bool IsSaveActive() const
	{
		return !!m_iSaveActive.load ( std::memory_order_relaxed );
	}
};

// helper for easier access to ConstRtData members
// note: that is pointer to CONST vector of CONST chunks everywhere, keep this constage from casts!
struct RtGuard_t
{
	ConstRtData				m_tSegmentsAndChunks;
	const DiskChunkVec_c &	m_dDiskChunks;
	const RtSegVec_c &		m_dRamSegs;

	explicit RtGuard_t ( ConstRtData tData )
			: m_tSegmentsAndChunks { std::move ( tData ) }
			, m_dDiskChunks { *m_tSegmentsAndChunks.m_pChunks }
			, m_dRamSegs { *m_tSegmentsAndChunks.m_pSegs }
	{}
};

// created with null set of ram segments and disk chunks
// on d-tr any not-null set will replace chunks and segments from the owner
class RtWriter_c : ISphNoncopyable
{
public:
	RtData_c&					m_tOwner;
	DiskChunkVecRefPtr_t		m_pNewDiskChunks;
	RtSegVecRefPtr_t			m_pNewRamSegs;

	explicit RtWriter_c ( RtData_c & tOwner ) REQUIRES ( tOwner.SerialChunkAccess () )
		: m_tOwner ( tOwner )
	{
		Threads::CheckAcquiredSched ( tOwner.SerialChunkAccess() );
	}

	enum eUnsafe { unsafe };
	RtWriter_c ( RtData_c & tOwner, eUnsafe )
		: m_tOwner ( tOwner )
	{}

	~RtWriter_c()
	{
		if ( !m_pNewDiskChunks && !m_pNewRamSegs )
			return;

		ScWL_t wLock ( m_tOwner.m_tLock );

		// use leak since we convert 'data*' to 'const data*' here.
		if ( m_pNewDiskChunks )
			m_tOwner.m_pChunks = m_pNewDiskChunks.Leak();

		if ( m_pNewRamSegs )
			m_tOwner.m_pSegments = m_pNewRamSegs.Leak();
	}

	enum Copy_e { copy };
	enum Empty_e { empty };

	void InitRamSegs ( Empty_e ) { m_pNewRamSegs = new RtSegVec_c (); }
	void InitDiskChunks ( Empty_e ) { m_pNewDiskChunks = new DiskChunkVec_c (); }

	void InitRamSegs ( Copy_e ) EXCLUDES ( m_tOwner.m_tLock )
	{
		InitRamSegs ( empty );
		for ( const auto & pSeg : *m_tOwner.RamSegs() )
			m_pNewRamSegs->Add ( pSeg );
	}

	void InitDiskChunks ( Copy_e ) EXCLUDES ( m_tOwner.m_tLock )
	{
		InitDiskChunks ( empty );
		for ( const auto & pChunk : *m_tOwner.DiskChunks() )
			m_pNewDiskChunks->Add ( pChunk );
	}
};

template<typename INT>
class Waitable_T
{
	using EVENT = Threads::CoroMultiEvent_c;
	EVENT m_tWaitableChanged;
	std::atomic<INT> m_iValue {0};

public:
	Waitable_T() = default;
	Waitable_T(Waitable_T&) = delete;
	Waitable_T& operator= (const Waitable_T&) = delete;

	void Inc()
	{
		m_iValue.fetch_add ( 1, std::memory_order_relaxed );
		m_tWaitableChanged.SetEvent();
	}

	void Dec()
	{
		m_iValue.fetch_sub ( 1, std::memory_order_relaxed );
		m_tWaitableChanged.SetEvent();
	}

	void SetValue ( INT iValue )
	{
		m_iValue.store ( iValue, std::memory_order_relaxed );
		m_tWaitableChanged.SetEvent();
	}

	INT GetValue () const
	{
		return m_iValue.load ( std::memory_order_relaxed );
	}

	template<typename PRED>
	void WaitFor ( PRED&& fnPred )
	{
		while ( !fnPred ( m_iValue.load ( std::memory_order_relaxed ) ) )
			m_tWaitableChanged.WaitEvent();
	}
};

CSphVector<int> GetChunkIds ( const VecTraits_T<DiskChunkRefPtr_t> & dChunks )
{
	CSphVector<int> dIds;
	dChunks.for_each ( [&dIds] ( const DiskChunkRefPtr_t & pIdx )
	{
		if ( !pIdx->m_bFinallyUnlink )
			dIds.Add ( pIdx->Idx ().m_iChunk );
	});
	return dIds;
}

enum class WriteState_e : int
{
	ENABLED,	// normal
	DISCARD,	// disabled, current result will not be necessary (can escape to don't waste resources)
	DISABLED,	// disabled, current stage must be completed first
};

class RtIndex_c final : public RtIndex_i, public ISphNoncopyable, public ISphWordlist, public ISphWordlistSuggest,
		public IndexUpdateHelper_c, public IndexAlterHelper_c, public DebugCheckHelper_c
{
public:
						RtIndex_c ( const CSphSchema & tSchema, const char * sIndexName, int64_t iRamSize, const char * sPath, bool bKeywordDict );
						~RtIndex_c () final;

	void				InitOneshotIndex ( const VecTraits_T<CSphIndex*>& dChunks );
	void				InitDictionaryInOneshot ();
	bool				AddDocument ( InsertDocData_t & tDoc, bool bReplace, const CSphString & sTokenFilterOptions, CSphString & sError, CSphString & sWarning, RtAccum_t * pAccExt ) override;
	virtual bool		AddDocument ( ISphHits * pHits, const InsertDocData_t & tDoc, bool bReplace, const DocstoreBuilder_i::Doc_t * pStoredDoc, CSphString & sError, CSphString & sWarning, RtAccum_t * pAccExt );
	bool				DeleteDocument ( const VecTraits_T<DocID_t> & dDocs, CSphString & sError, RtAccum_t * pAccExt ) final;
	bool				Commit ( int * pDeleted, RtAccum_t * pAccExt ) final;
	void				RollBack ( RtAccum_t * pAccExt ) final;
	int					CommitReplayable ( RtSegment_t * pNewSeg, const CSphVector<DocID_t> & dAccKlist ); // returns total killed documents
	void				ForceRamFlush ( const char * szReason ) final;
	bool				IsFlushNeed() const final;
	bool				ForceDiskChunk() final;
	bool				AttachDiskIndex ( CSphIndex * pIndex, bool bTruncate, bool & bFatal, StrVec_t & dWarnings, CSphString & sError ) final;
	bool				Truncate ( CSphString & sError ) final;
	bool				CheckValidateOptimizeParams ( OptimizeTask_t& tTask ) const;
	bool				CheckValidateChunk ( int& iChunk, int iChunks, bool bByOrder ) const;
	void				Optimize ( OptimizeTask_t tTask ) final;
	void				CheckStartAutoOptimize ();
	void				ClassicOptimize ();
	void				CommonMerge ( std::function<bool ( OptimizeTask_t& )>&& fnSelector );
	void				DropDiskChunk ( int iChunk );
	bool				CompressOneChunk ( int iChunk, int& iAffected );
	bool				MergeTwoChunks ( int iA, int iB );
	bool				SplitOneChunk ( int iChunkID, const char* szUvarFilter, int& iAffected );
	bool				SplitOneChunkFast ( int iChunkID, const char * szUvarFilter, bool& bResult, int& iAffected );
	int					ChunkIDByChunkIdx (int iChunkIdx) const;

	// helpers
	ConstDiskChunkRefPtr_t	MergeDiskChunks (  const char* szParentAction, const ConstDiskChunkRefPtr_t& pChunkA, const ConstDiskChunkRefPtr_t& pChunkB, CSphIndexProgress& tProgress, VecTraits_T<CSphFilterSettings> dFilters );
	bool				PublishMergedChunks ( const char * szParentAction,std::function<bool ( int, DiskChunkVec_c & )> && fnPusher) REQUIRES ( m_tRtChunks.SerialChunkAccess () );
	bool 				RenameOptimizedChunk ( const ConstDiskChunkRefPtr_t& pChunk, const char * szParentAction );
	bool				SkipOrDrop ( int iChunk, const CSphIndex& dChunk, bool bCheckAlive, int& iAffected );
	void				ProcessDiskChunk ( int iChunk, VisitChunk_fn&& fnVisitor ) final;
	template <typename VISITOR>
	void				ProcessDiskChunkByID ( int iChunkID, VISITOR&& fnVisitor ) const;
	template <typename VISITOR>
	void				ProcessDiskChunkByID ( VecTraits_T<int> dChunkIDs, VISITOR&& fnVisitor ) const;
	ISphTokenizer *		CloneIndexingTokenizer() const final { return m_pTokenizerIndexing->Clone ( SPH_CLONE_INDEX ); }

	void				SetKillHookFor ( IndexSegment_c* pAccum, int iDiskChunkID ) const;
	void				SetKillHookFor ( IndexSegment_c* pAccum, VecTraits_T<int> dDiskChunkIDs ) const;

	Binlog::CheckTnxResult_t ReplayTxn ( Binlog::Blop_e eOp,CSphReader& tReader, CSphString & sError, Binlog::CheckTxn_fn&& fnCanContinue ) override; // cb from binlog
	Binlog::CheckTnxResult_t ReplayCommit ( CSphReader& tReader, Binlog::CheckTxn_fn&& fnCanContinue );
	Binlog::CheckTnxResult_t ReplayReconfigure ( CSphReader& tReader, CSphString & sError, Binlog::CheckTxn_fn&& fnCanContinue );

public:
#if _WIN32
#pragma warning(push,1)
#pragma warning(disable:4100)
#endif

	int					Kill ( DocID_t tDocID ) final;
	int					KillMulti ( const VecTraits_T<DocID_t> & dKlist ) final;
	bool				IsAlive ( DocID_t tDocID ) const final;

	bool				Prealloc ( bool bStripPath, FilenameBuilder_i * pFilenameBuilder, StrVec_t & dWarnings ) final;
	void				Preread () final;
	void				PostSetup() final;
	bool				IsRT() const final { return true; }

	int					UpdateAttributes ( AttrUpdateInc_t & tUpd, bool & bCritical, CSphString & sError, CSphString & sWarning ) final;
	bool				SaveAttributes ( CSphString & sError ) const final;
	DWORD				GetAttributeStatus () const final { return m_uDiskAttrStatus; }

	bool				AddRemoveAttribute ( bool bAdd, const CSphString & sAttrName, ESphAttr eAttrType, AttrEngine_e eEngine, CSphString & sError ) final;
	bool				AddRemoveField ( bool bAdd, const CSphString & sFieldName, DWORD uFieldFlags, CSphString & sError ) final;

	int					DebugCheck ( FILE * fp ) final;
#if _WIN32
#pragma warning(pop)
#endif

	bool				EarlyReject ( CSphQueryContext * pCtx, CSphMatch & ) const final;
	const CSphSourceStats &		GetStats () const final { return m_tStats; }
	int64_t *			GetFieldLens() const final { return m_tSettings.m_bIndexFieldLens ? m_dFieldLens.Begin() : nullptr; }
	void				GetStatus ( CSphIndexStatus* ) const final;

	bool				MultiQuery ( CSphQueryResult& tResult, const CSphQuery& tQuery, const VecTraits_T<ISphMatchSorter*>& dAllSorters, const CSphMultiQueryArgs& tArgs ) const final;
	void				DoGetKeywords ( CSphVector<CSphKeywordInfo>& dKeywords, const char* sQuery, const GetKeywordsSettings_t& tSettings, bool bFillOnly, CSphString* pError, const RtGuard_t& tGuard ) const;
	bool				GetKeywords ( CSphVector<CSphKeywordInfo>& dKeywords, const char* sQuery, const GetKeywordsSettings_t& tSettings, CSphString* pError ) const final;
	bool				FillKeywords ( CSphVector <CSphKeywordInfo> & dKeywords ) const final;
	void				AddKeywordStats ( BYTE* sWord, const BYTE* sTokenized, CSphDict* pDict, bool bGetStats, int iQpos, RtQword_t* pQueryWord, CSphVector<CSphKeywordInfo>& dKeywords, const RtSegVec_c& dRamSegs ) const;

	bool				RtQwordSetup ( RtQword_t * pQword, int iSeg, const RtGuard_t& tGuard ) const;
	bool				RtQwordSetupSegment ( RtQword_t* pQword, const RtSegment_t* pCurSeg, bool bSetup ) const;

	bool				IsWordDict () const { return m_bKeywordDict; }
	int					GetWordCheckoint() const { return m_iWordsCheckpoint; }
	int					GetMaxCodepointLength() const { return m_iMaxCodepointLength; }

	bool				IsSameSettings ( CSphReconfigureSettings & tSettings, CSphReconfigureSetup & tSetup, StrVec_t & dWarnings, CSphString & sError ) const final;
	bool				Reconfigure ( CSphReconfigureSetup & tSetup ) final;
	int64_t				GetLastFlushTimestamp() const final;
	void				IndexDeleted() final { m_bIndexDeleted = true; }
	bool				CopyExternalFiles ( int iPostfix, StrVec_t & dCopied ) final;
	void				CollectFiles ( StrVec_t & dFiles, StrVec_t & dExt ) const final;
	void				ProhibitSave() final;
	void				EnableSave() final;
	void				LockFileState ( CSphVector<CSphString> & dFiles ) final;

	void				SetDebugCheck ( bool bCheckIdDups, int iCheckChunk ) final;

	void				CreateReader ( int64_t iSessionId ) const final;
	bool				GetDoc ( DocstoreDoc_t & tDoc, DocID_t tDocID, const VecTraits_T<int> * pFieldIds, int64_t iSessionId, bool bPack ) const final;
	int					GetFieldId ( const CSphString & sName, DocstoreDataType_e eType ) const final;
	Bson_t				ExplainQuery ( const CSphString & sQuery ) const final;
	uint64_t			GetSchemaHash () const final { return m_uSchemaHash; }

protected:
	CSphSourceStats		m_tStats;
	bool				m_bDebugCheck = false;
	bool				m_bCheckIdDups = false;
	int					m_iCheckChunk = -1;
	CSphFixedVector<int>	m_dChunkNames { 0 };

private:
	static const DWORD			META_HEADER_MAGIC	= 0x54525053;	///< my magic 'SPRT' header
	static const DWORD			META_VERSION		= 19;			///< current version fixme! Also change version in indextool.cpp, and support the changes!

	int							m_iStride;
	uint64_t					m_uSchemaHash = 0;
	std::atomic<int64_t>		m_iRamChunksAllocatedRAM { 0 };

	std::atomic<bool>			m_bOptimizeStop { false };
	Waitable_T<int>				m_tOptimizeRuns;
	friend class OptimizeGuard_c;

	int64_t						m_iSoftRamLimit;
	int64_t						m_iDoubleBufferLimit;
	CSphString					m_sPath;
	bool						m_bPathStripped = false;
	RtData_c					m_tRtChunks;	// that is main set of disk chunks and RAM segments
	bool						m_bSegMergeQueued GUARDED_BY ( m_tRtChunks.SerialChunkAccess() ) = false;
	int							m_iLockFD = -1;

	bool						m_bIndexDeleted = false;

	int64_t						m_iSavedTID;
	int64_t						m_tmSaved;
	mutable DWORD				m_uDiskAttrStatus = 0;

	bool						m_bKeywordDict;
	int							m_iWordsCheckpoint = RTDICT_CHECKPOINT_V5;
	int							m_iMaxCodepointLength = 0;
	TokenizerRefPtr_c			m_pTokenizerIndexing;
	bool						m_bLoadRamPassedOk = true;
	std::atomic<WriteState_e>	m_eSaving { WriteState_e::ENABLED };
	bool						m_bHasFiles = false;

	// fixme! make this *Lens atomic together with disk/ram data, to avoid any kind of race among them
	CSphFixedVector<int64_t>	m_dFieldLens { SPH_MAX_FIELDS };	///< total field lengths over entire index
	CSphFixedVector<int64_t>	m_dFieldLensRam { SPH_MAX_FIELDS };	///< field lengths summed over current RAM chunk
	CSphFixedVector<int64_t>	m_dFieldLensDisk { SPH_MAX_FIELDS };	///< field lengths summed over all disk chunks
	CSphBitvec					m_tMorphFields;
	CSphVector<SphWordID_t>		m_dHitlessWords;

	CSphScopedPtr<DocstoreFields_i> m_pDocstoreFields {nullptr};	// rt index doesn't have its own docstore, but it must keep all fields to get their ids for GetDoc

	RtAccum_t *					CreateAccum ( RtAccum_t * pAccExt, CSphString & sError ) final;

	int							CompareWords ( const RtWord_t * pWord1, const RtWord_t * pWord2 ) const;

	CSphFixedVector<RowID_t> CopyAttributesFromAliveDocs ( RtSegment_t& tDstSeg, const RtSegment_t & tSrcSeg, RtAttrMergeContext_t & tCtx ) const;
	void						MergeKeywords ( RtSegment_t & tSeg, const RtSegment_t & tSeg1, const RtSegment_t & tSeg2, const VecTraits_T<RowID_t> & dRowMap1, const VecTraits_T<RowID_t> & dRowMap2 ) const;
	RtSegment_t *				MergeTwoSegments ( const RtSegment_t * pA, const RtSegment_t * pB ) const;
	static void					CopyWord ( RtSegment_t& tDstSeg, RtWord_t& tDstWord, RtDocWriter_c& tDstDoc, const RtSegment_t& tSrcSeg, const RtWord_t* pSrcWord, const VecTraits_T<RowID_t>& dRowMap );
	void						UpdateAttributesOffline ( VecTraits_T<PostponedUpdate_t>& dUpdates, IndexSegment_c * pSeg ) override;

	bool						UpdateAttributesInRamSegment ( const RowsToUpdate_t& dRows, UpdateContext_t& tCtx, bool& bCritical, CSphString& sError );

	void						DeleteFieldFromDict ( RtSegment_t * pSeg, int iKillField );
	void						AddFieldToRamchunk ( const CSphString & sFieldName, DWORD uFieldFlags, const CSphSchema & tNewSchema );
	void						RemoveFieldFromRamchunk ( const CSphString & sFieldName, const CSphSchema & tOldSchema, const CSphSchema & tNewSchema );

	bool						LoadMeta ( FilenameBuilder_i * pFilenameBuilder, bool bStripPath, DWORD & uVersion, bool & bRebuildInfixes, StrVec_t & dWarnings );
	bool						PreallocDiskChunks ( FilenameBuilder_i * pFilenameBuilder, StrVec_t & dWarnings );
	void						SaveMeta ( int64_t iTID, VecTraits_T<int> dChunkNames );
	void						SaveMeta ();
	void						SaveDiskHeader ( SaveDiskDataContext_t & tCtx, const ChunkStats_t & tStats ) const;
	void						SaveDiskData ( const char * szFilename, const ConstRtSegmentSlice_t & tSegs, const ChunkStats_t & tStats ) const;
	bool						SaveDiskChunk ( bool bForced ) REQUIRES ( m_tRtChunks.SerialChunkAccess () );
	CSphIndex *					PreallocDiskChunk ( const char * sChunk, int iChunk, FilenameBuilder_i * pFilenameBuilder, StrVec_t & dWarnings, CSphString & sError, const char * sName=nullptr ) const;
	bool						LoadRamChunk ( DWORD uVersion, bool bRebuildInfixes );
	bool						SaveRamChunk ();

	bool						WriteAttributes ( SaveDiskDataContext_t & tCtx, CSphString & sError ) const;
	bool						WriteDocs ( SaveDiskDataContext_t & tCtx, CSphWriter & tWriterDict, CSphString & sError ) const;
	void						WriteCheckpoints ( SaveDiskDataContext_t & tCtx, CSphWriter & tWriterDict ) const;
	static bool					WriteDeadRowMap ( SaveDiskDataContext_t & tCtx, CSphString & sError );

	void						GetPrefixedWords ( const char * sSubstring, int iSubLen, const char * sWildcard, Args_t & tArgs ) const final;
	void						GetInfixedWords ( const char * sSubstring, int iSubLen, const char * sWildcard, Args_t & tArgs ) const final;
	void						GetSuggest ( const SuggestArgs_t & tArgs, SuggestResult_t & tRes ) const final;

	void						SuffixGetChekpoints ( const SuggestResult_t & tRes, const char * sSuffix, int iLen, CSphVector<DWORD> & dCheckpoints ) const final;
	void						SetCheckpoint ( SuggestResult_t & tRes, DWORD iCP ) const final;
	bool						ReadNextWord ( SuggestResult_t & tRes, DictWord_t & tWord ) const final;

	ConstRtSegmentRefPtf_t		AdoptSegment ( RtSegment_t * pNewSeg );
	int							ApplyKillList ( const CSphVector<DocID_t> & dAccKlist ) REQUIRES ( m_tRtChunks.SerialChunkAccess () );

	bool						AddRemoveColumnarAttr ( RtGuard_t & tGuard, bool bAdd, const CSphString & sAttrName, ESphAttr eAttrType, const CSphSchema & tOldSchema, const CSphSchema & tNewSchema, CSphString & sError );
	void						AddRemoveRowwiseAttr ( RtGuard_t & tGuard, bool bAdd, const CSphString & sAttrName, ESphAttr eAttrType, const CSphSchema & tOldSchema, const CSphSchema & tNewSchema, CSphString & sError );

	bool						Update_WriteBlobRow ( UpdateContext_t& tCtx, CSphRowitem* pDocinfo, const BYTE* pBlob, int iLength, int nBlobAttrs, const CSphAttrLocator& tBlobRowLoc, bool& bCritical, CSphString& sError ) override;
	bool						Update_DiskChunks ( AttrUpdateInc_t& tUpd, const DiskChunkSlice_t& dDiskChunks, CSphString& sError );

	void						GetIndexFiles ( CSphVector<CSphString> & dFiles, const FilenameBuilder_i * pParentBuilder ) const override;
	DocstoreBuilder_i::Doc_t *	FetchDocFields ( DocstoreBuilder_i::Doc_t & tStoredDoc, CSphSource_StringVector & tSrc ) const;

	CSphString					MakeChunkName(int iChunkID);
	void						UnlinkRAMChunk ( const char * szInfo=nullptr );
	void 						WaitRAMSegmentsUnlocked () const REQUIRES ( m_tRtChunks.SerialChunkAccess () );
	void						StartMergeSegments (bool bHasNewSegment) REQUIRES ( m_tRtChunks.SerialChunkAccess () );
	bool						NeedStoreWordID () const override;
	int64_t						GetMemLimit() const final { return m_iSoftRamLimit; }

	void						DebugCheckRam ( DebugCheckError_c & tReporter );
	int							DebugCheckDisk ( DebugCheckError_c & tReporter, FILE * fp );

	void						SetSchema ( const CSphSchema & tSchema );

	void						SetMemLimit ( int64_t iMemLimit );
	void						AlterSave ( bool bSaveRam );
	void 						BinlogCommit ( RtSegment_t * pSeg, const CSphVector<DocID_t> & dKlist );
	void						StopOptimize();
};


RtIndex_c::RtIndex_c ( const CSphSchema & tSchema, const char * sIndexName, int64_t iRamSize, const char * sPath, bool bKeywordDict )
	: RtIndex_i ( sIndexName, sPath )
	, m_sPath ( sPath )
	, m_iSavedTID ( m_iTID )
	, m_tmSaved ( sphMicroTimer() )
	, m_bKeywordDict ( bKeywordDict )
{
	MEMORY ( MEM_INDEX_RT );

	SetSchema ( tSchema );
	SetMemLimit ( iRamSize );
}


RtIndex_c::~RtIndex_c ()
{
	int64_t tmSave = sphMicroTimer();
	bool bValid = m_pTokenizer && m_pDict && m_bLoadRamPassedOk;

	if ( bValid )
	{
		SaveRamChunk();
		SaveMeta();
	}

	if ( m_iLockFD>=0 )
		::close ( m_iLockFD );

	if ( bValid )
		Binlog::NotifyIndexFlush ( m_sIndexName.cstr(), m_iTID, sphInterrupted () );

	if ( m_bIndexDeleted )
	{
		CSphString sFile;
		sFile.SetSprintf ( "%s.meta", m_sPath.cstr() );
		::unlink ( sFile.cstr() );
		sFile.SetSprintf ( "%s.ram", m_sPath.cstr() );
		::unlink ( sFile.cstr() );
		sFile.SetSprintf ( "%s%s", m_sPath.cstr(), sphGetExt ( SPH_EXT_SETTINGS ).cstr() );
		::unlink ( sFile.cstr() );
	}

	tmSave = sphMicroTimer() - tmSave;
	if ( tmSave>=1000 && bValid )
	{
		sphInfo ( "rt: index %s: ramchunk saved in %d.%03d sec",
			m_sIndexName.cstr(), (int)(tmSave/1000000), (int)((tmSave/1000)%1000) );
	}
}

void RtIndex_c::ProcessDiskChunk ( int iChunk, VisitChunk_fn&& fnVisitor )
{
	auto pDiskChunks = m_tRtChunks.DiskChunks();
	if ( iChunk < 0 || iChunk >= pDiskChunks->GetLength() )
		fnVisitor ( nullptr );
	else
		fnVisitor ( &( *pDiskChunks )[iChunk]->Cidx() );
}

template<typename VISITOR>
void RtIndex_c::ProcessDiskChunkByID ( int iChunkID, VISITOR&& fnVisitor ) const
{
	auto pChunks = m_tRtChunks.DiskChunks();
	for ( const auto& pChunk : *pChunks )
		if ( iChunkID == pChunk->Cidx().m_iChunk )
		{
			fnVisitor ( pChunk );
			break;
		}
}

template<typename VISITOR>
void RtIndex_c::ProcessDiskChunkByID ( VecTraits_T<int> dChunkIDs, VISITOR&& fnVisitor ) const
{
	auto pChunks = m_tRtChunks.DiskChunks();
	for ( int iDiskChunkID : dChunkIDs )
		for ( const auto& pChunk : *pChunks )
			if ( iDiskChunkID == pChunk->Cidx().m_iChunk )
			{
				fnVisitor ( pChunk );
				break;
			}
}

bool RtIndex_c::IsFlushNeed() const
{
	// m_iTID get managed by binlog that is why wo binlog there is no need to compare it 
	if ( Binlog::IsActive() && m_iTID<=m_iSavedTID )
		return false;

	return m_eSaving.load(std::memory_order_relaxed)==WriteState_e::ENABLED;
}

static int64_t SegmentsGetUsedRam ( const ConstRtSegmentSlice_t& dSegments )
{
	int64_t iTotal = 0;
	for ( const RtSegment_t * pSeg : dSegments )
		iTotal += pSeg->GetUsedRam();

	return iTotal;
}

static int64_t SegmentsGetDeadRows ( const VecTraits_T<RtSegmentRefPtf_t> & dSegments )
{
	int64_t iTotal = 0;
	for ( RtSegment_t * pSeg : dSegments )
		iTotal += pSeg->m_tDeadRowMap.GetNumDeads();

	return iTotal;
}

// save RAM chunk to .ram, flush disk chunks dead-row-maps.
void RtIndex_c::ForceRamFlush ( const char* szReason )
{
	if ( !IsFlushNeed() )
		return;

	int64_t tmSave = sphMicroTimer();

	ScopedScheduler_c tSerialFiber ( m_tRtChunks.SerialChunkAccess() );

	int64_t iUsedRam = SegmentsGetUsedRam ( *m_tRtChunks.RamSegs() );
	if ( !SaveRamChunk () )
	{
		sphWarning ( "rt: index %s: ramchunk save FAILED! (error=%s)", m_sIndexName.cstr(), m_sLastError.cstr() );
		return;
	}

	SaveMeta();
	m_tRtChunks.DiskChunks ()->for_each ( [] ( ConstDiskChunkRefPtr_t & pIdx ) { pIdx->Cidx().FlushDeadRowMap ( true ); } );
	Binlog::NotifyIndexFlush ( m_sIndexName.cstr(), m_iTID, false );

	int64_t iWasTID = m_iSavedTID;
	int64_t tmDelta = sphMicroTimer() - m_tmSaved;
	m_iSavedTID = m_iTID;
	m_tmSaved = sphMicroTimer();

	tmSave = sphMicroTimer() - tmSave;
	sphInfo ( "rt: index %s: ramchunk saved ok (mode=%s, last TID=" INT64_FMT ", current TID=" INT64_FMT ", "
		"ram=%d.%03d Mb, time delta=%d sec, took=%d.%03d sec)"
		, m_sIndexName.cstr(), szReason, iWasTID, m_iTID, (int)(iUsedRam/1024/1024), (int)((iUsedRam/1024)%1000)
		, (int) (tmDelta/1000000), (int)(tmSave/1000000), (int)((tmSave/1000)%1000) );
}


int64_t RtIndex_c::GetLastFlushTimestamp() const
{
	return m_tmSaved;
}


//////////////////////////////////////////////////////////////////////////
// INDEXING
//////////////////////////////////////////////////////////////////////////
CSphSource_StringVector::CSphSource_StringVector ( const VecTraits_T<VecTraits_T<const char >> &dFields, const CSphSchema & tSchema )
	: CSphSource_Document ( "$blobvector" )
{
	m_tSchema = tSchema;
	m_dFieldLengths.Reserve ( dFields.GetLength () );
	m_dFields.Reserve ( dFields.GetLength() + 1 );
	for ( const auto& dField : dFields )
	{
		m_dFields.Add ( (BYTE*)const_cast<char*> ( dField.begin() ) );
		m_dFieldLengths.Add ( dField.GetLength () );
		assert ( dField.begin() || dField.IsEmpty () );
	}
	m_dFields.Add (nullptr);
	m_iMaxHits = 0; // force all hits build
}

bool CSphSource_StringVector::Connect ( CSphString & )
{
	// no AddAutoAttrs() here; they should already be in the schema
	m_tHits.Reserve ( 1024 );
	return true;
}

void CSphSource_StringVector::Disconnect ()
{
	m_tHits.Reset();
}


DocstoreBuilder_i::Doc_t * RtIndex_c::FetchDocFields ( DocstoreBuilder_i::Doc_t & tStoredDoc, CSphSource_StringVector & tSrc ) const
{
	if ( !m_tSchema.HasStoredFields() )
		return nullptr;

	tSrc.GetDocFields ( tStoredDoc.m_dFields );
	assert ( tStoredDoc.m_dFields.GetLength()==m_tSchema.GetFieldsCount() );

	// filter out non-hl fields (should already be null)
	int iField = 0;
	for ( int i = 0; i < m_tSchema.GetFieldsCount(); i++ )
	{
		if ( !m_tSchema.IsFieldStored(i) )
			tStoredDoc.m_dFields.Remove(iField);
		else
			iField++;
	}

	return &tStoredDoc;
}


bool RtIndex_c::AddDocument ( InsertDocData_t & tDoc, bool bReplace, const CSphString & sTokenFilterOptions, CSphString & sError, CSphString & sWarning, RtAccum_t * pAccExt )
{
	assert ( g_bRTChangesAllowed );
	assert ( m_tSchema.GetAttrIndex ( sphGetDocidName() )==0 );
	assert ( m_tSchema.GetAttr ( sphGetDocidName() )->m_eAttrType==SPH_ATTR_BIGINT );

	DocID_t tDocID = tDoc.GetID();

	// here is only point related to current index - generate unique autoID, or check that provided is not duplicate.
	if ( !tDocID || !bReplace )
	{
		RtGuard_t tGuard ( m_tRtChunks.RtData () );
		if ( !tDocID ) // docID wasn't provided, need to generate autoID
		{
			bReplace = false; // with absent docID we effectively fall to plain 'insert' - nothing to kill
			do
				tDocID = UidShort ();
			while ( tGuard.m_dRamSegs.any_of (
					[tDocID] ( ConstRtSegmentRefPtf_t & p ) { return p->FindAliveRow ( tDocID ); } ) );

			tDoc.SetID ( tDocID );
		} else
		{
			// docID was provided, but that is new insert and we need to check for duplicates
			assert ( !bReplace && tDocID!=0 );
			if ( tGuard.m_dRamSegs.any_of ( [tDocID] ( ConstRtSegmentRefPtf_t & p ) { return p->FindAliveRow ( tDocID ); })
				|| tGuard.m_dDiskChunks.any_of ( [tDocID] ( ConstDiskChunkRefPtr_t & p ) { return p->Cidx().IsAlive(tDocID); }))
			{
				sError.SetSprintf ( "duplicate id '" INT64_FMT "'", tDocID );
				return false; // already exists and not deleted; INSERT fails
			}
		}
	}

	TokenizerRefPtr_c tTokenizer { CloneIndexingTokenizer() };

	if (!tTokenizer)
	{
		sError.SetSprintf ( "internal error: no indexing tokenizer available" );
		return false;
	}

	MEMORY ( MEM_INDEX_RT );

	auto pAcc = ( RtAccum_t * ) AcquireAccum ( m_pDict, pAccExt, m_bKeywordDict, true, &sError );
	if ( !pAcc )
		return false;

	tDoc.m_tDoc.m_tRowID = pAcc->GenerateRowID();

	// OPTIMIZE? do not create filter on each(!) INSERT
	if ( !m_tSettings.m_sIndexTokenFilter.IsEmpty() )
	{
		tTokenizer = ISphTokenizer::CreatePluginFilter ( tTokenizer, m_tSettings.m_sIndexTokenFilter, sError );
		if ( !tTokenizer )
			return false;
		if ( !tTokenizer->SetFilterSchema ( m_tSchema, sError ) )
			return false;
		if ( !sTokenFilterOptions.IsEmpty() )
			if ( !tTokenizer->SetFilterOptions ( sTokenFilterOptions.cstr(), sError ) )
				return false;
	}

	// OPTIMIZE? do not create filter on each(!) INSERT
	if ( m_tSettings.m_uAotFilterMask )
		tTokenizer = sphAotCreateFilter ( tTokenizer, m_pDict, m_tSettings.m_bIndexExactWords, m_tSettings.m_uAotFilterMask );

	CSphSource_StringVector tSrc ( tDoc.m_dFields, m_tSchema );

	// SPZ setup
	if ( m_tSettings.m_bIndexSP && !tTokenizer->EnableSentenceIndexing ( sError ) )
		return false;

	if ( !m_tSettings.m_sZones.IsEmpty() && !tTokenizer->EnableZoneIndexing ( sError ) )
		return false;

	if ( m_tSettings.m_bHtmlStrip && !tSrc.SetStripHTML ( m_tSettings.m_sHtmlIndexAttrs.cstr(), m_tSettings.m_sHtmlRemoveElements.cstr(),
			m_tSettings.m_bIndexSP, m_tSettings.m_sZones.cstr(), sError ) )
		return false;

	// OPTIMIZE? do not clone filters on each INSERT
	FieldFilterRefPtr_c pFieldFilter;
	if ( m_pFieldFilter )
		pFieldFilter = m_pFieldFilter->Clone();

	tSrc.Setup ( m_tSettings, nullptr );
	tSrc.SetTokenizer ( tTokenizer );
	tSrc.SetDict ( pAcc->m_pDict );
	tSrc.SetFieldFilter ( pFieldFilter );
	tSrc.SetMorphFields ( m_tMorphFields );
	if ( !tSrc.Connect ( m_sLastError ) )
		return false;

	m_tSchema.CloneWholeMatch ( tSrc.m_tDocInfo, tDoc.m_tDoc );

	bool bEOF = false;
	if ( !tSrc.IterateStart ( sError ) || !tSrc.IterateDocument ( bEOF, sError ) )
		return false;

	ISphHits * pHits = tSrc.IterateHits ( sError );
	pAcc->GrabLastWarning ( sWarning );

	DocstoreBuilder_i::Doc_t tStoredDoc;
	DocstoreBuilder_i::Doc_t * pStoredDoc = FetchDocFields ( tStoredDoc, tSrc );
	if ( !AddDocument ( pHits, tDoc, bReplace, pStoredDoc, sError, sWarning, pAcc ) )
		return false;

	m_tStats.m_iTotalBytes += tSrc.GetStats().m_iTotalBytes;

	return true;
}


RtAccum_t * RtIndex_i::AcquireAccum ( CSphDict * pDict, RtAccum_t * pAcc,
	bool bWordDict, bool bSetTLS, CSphString* pError )
{
	if ( !pAcc )
		pAcc = g_pTlsAccum;

	if ( pAcc && pAcc->GetIndex() && pAcc->GetIndex()!=this )
	{
		if ( pError )
			pError->SetSprintf ( "current txn is working with another index ('%s')", pAcc->GetIndex()->GetName() );
		return nullptr;
	}

	if ( pAcc && pAcc->GetIndex() && pAcc->GetSchemaHash()!=GetSchemaHash() )
	{
		if ( pError )
			pError->SetSprintf ( "current txn is working with index's another schema ('%s'), restart session", pAcc->GetIndex()->GetName() );
		return nullptr;
	}

	if ( !pAcc )
	{
		pAcc = new RtAccum_t ( bWordDict );
		if ( bSetTLS )
		{
			g_pTlsAccum = pAcc;
			Threads::OnExitThread( [pAcc] { delete pAcc; } );
		}
	}

	assert ( pAcc->GetIndex()==nullptr || pAcc->GetIndex()==this );
	pAcc->SetIndex ( this );
	pAcc->SetupDict ( this, pDict, bWordDict );
	return pAcc;
}

RtAccum_t * RtIndex_c::CreateAccum ( RtAccum_t * pAccExt, CSphString & sError )
{
	return AcquireAccum ( m_pDict, pAccExt, m_bKeywordDict, false, &sError);
}


bool RtIndex_c::AddDocument ( ISphHits * pHits, const InsertDocData_t & tDoc, bool bReplace, const DocstoreBuilder_i::Doc_t * pStoredDoc, CSphString & sError, CSphString & sWarning, RtAccum_t * pAccExt )
{
	assert ( g_bRTChangesAllowed );

	auto * pAcc = (RtAccum_t *)pAccExt;

	if ( pAcc )
		pAcc->AddDocument ( pHits, tDoc, bReplace, m_tSchema.GetRowSize(), pStoredDoc );

	return !!pAcc;
}


RtAccum_t::RtAccum_t ( bool bKeywordDict )
	: m_bKeywordDict ( bKeywordDict )
{}

RtAccum_t::~RtAccum_t()
{
	ARRAY_FOREACH ( i, m_dCmd )
		SafeDelete ( m_dCmd[i] );
}

void RtAccum_t::SetupDict ( const RtIndex_i * pIndex, CSphDict * pDict, bool bKeywordDict )
{
	if ( pIndex!=m_pIndex || pDict!=m_pRefDict || bKeywordDict!=m_bKeywordDict )
	{
		m_bKeywordDict = bKeywordDict;
		m_pRefDict = pDict;
		m_pDict = GetStatelessDict ( pDict );
		if ( m_bKeywordDict )
		{
			m_pDict = m_pDictRt = sphCreateRtKeywordsDictionaryWrapper ( m_pDict, pIndex->NeedStoreWordID() );
		}
	}
}

void RtAccum_t::ResetDict ()
{
	assert ( !m_bKeywordDict || m_pDictRt );
	if ( m_pDictRt )
		m_pDictRt->ResetKeywords();

	m_dPackedKeywords.Reset(0);
}

const BYTE * RtAccum_t::GetPackedKeywords () const
{
	return m_dPackedKeywords.IsEmpty() ? m_pDictRt->GetPackedKeywords() : m_dPackedKeywords.begin();
}

int RtAccum_t::GetPackedLen () const
{
	return m_dPackedKeywords.IsEmpty() ? m_pDictRt->GetPackedLen() : m_dPackedKeywords.GetLength();
}

void RtAccum_t::Sort ()
{
	if ( !m_bKeywordDict )
		m_dAccum.Sort ( CmpHitPlain_fn() );
	else
	{
		assert ( m_pDictRt );
		const BYTE * pPackedKeywords = GetPackedKeywords();
		m_dAccum.Sort ( CmpHitKeywords_fn ( pPackedKeywords ) );
	}
}

void RtAccum_t::CleanupPart()
{
	m_dAccumRows.Resize(0);
	m_dBlobs.Resize(0);
	m_pColumnarBuilder.Reset();
	m_dPerDocHitsCount.Resize(0);
	m_dAccum.Resize(0);
	m_pDocstore.Reset();

	ResetDict();
	ResetRowID();
}

void RtAccum_t::Cleanup()
{
	CleanupPart();

	SetIndex ( nullptr );
	m_uAccumDocs = 0;
	m_dAccumKlist.Reset ();
	
	ARRAY_FOREACH ( i, m_dCmd )
		SafeDelete ( m_dCmd[i] );

	m_dCmd.Reset();
}


void RtAccum_t::SetupDocstore()
{
	if ( m_pDocstore.Ptr() )
		return;

	m_pDocstore = CreateDocstoreRT();
	assert ( m_pDocstore.Ptr() );
	SetupDocstoreFields ( *m_pDocstore.Ptr(), m_pIndex->GetInternalSchema() );
}

bool RtAccum_t::SetupDocstore ( RtIndex_i & tIndex, CSphString & sError )
{
	const CSphSchema & tSchema = tIndex.GetInternalSchema();
	if ( !m_pDocstore.Ptr() && !tSchema.HasStoredFields() )
		return true;

	// might be a case when replicated trx was wo docstore but index has docstore
	if ( !m_pDocstore.Ptr() )
		m_pDocstore = CreateDocstoreRT();

	assert ( m_pDocstore.Ptr() );
	SetupDocstoreFields ( *m_pDocstore.Ptr(), tSchema );
	return m_pDocstore->CheckFieldsLoaded ( sError );
}


void RtAccum_t::AddDocument ( ISphHits * pHits, const InsertDocData_t & tDoc, bool bReplace, int iRowSize, const DocstoreBuilder_i::Doc_t * pStoredDoc )
{
	MEMORY ( MEM_RT_ACCUM );

	// FIXME? what happens on mixed insert/replace?
	m_bReplace = bReplace;

	DocID_t tDocID = tDoc.GetID();

	// schedule existing copies for deletion
	m_dAccumKlist.Add ( tDocID );

	// reserve some hit space on first use
	if ( pHits && pHits->GetLength() && !m_dAccum.GetLength() )
		m_dAccum.Reserve ( 128*1024 );

	// accumulate row data; expect fully dynamic rows
	assert ( !tDoc.m_tDoc.m_pStatic );
	assert (!( !tDoc.m_tDoc.m_pDynamic && iRowSize!=0 ));
	assert (!( tDoc.m_tDoc.m_pDynamic && (int)tDoc.m_tDoc.m_pDynamic[-1]!=iRowSize ));

	CSphRowitem * pRow = nullptr;
	if ( iRowSize )
	{
		m_dAccumRows.Append ( tDoc.m_tDoc.m_pDynamic, iRowSize );
		pRow = &m_dAccumRows [ m_dAccumRows.GetLength() - iRowSize ];
	}

	CSphString sError;

	int iStrAttr = 0;
	int iBlobAttr = 0;
	int iColumnarAttr = 0;
	int iMva = 0;

	const char ** ppStr = tDoc.m_dStrings.Begin();
	const CSphSchema & tSchema = m_pIndex->GetInternalSchema();
	for ( int i=0; i<tSchema.GetAttrsCount(); ++i )
	{
		const CSphColumnInfo & tColumn = tSchema.GetAttr(i);

		switch ( tColumn.m_eAttrType )
		{
		case SPH_ATTR_STRING:
		case SPH_ATTR_JSON:
			{
				const BYTE * pStr = ppStr ? (const BYTE *) ppStr[iStrAttr++] : nullptr;
				ByteBlob_t dStr;
				if ( tColumn.m_eAttrType==SPH_ATTR_STRING )
					dStr = {pStr, pStr ? (int) strlen ((const char *) pStr ) : 0};
				else // SPH_ATTR_JSON - packed len + data
					dStr = sphUnpackPtrAttr ( pStr );

				if ( tColumn.IsColumnar() )
					m_pColumnarBuilder->SetAttr ( iColumnarAttr, dStr.first, dStr.second );
				else
					m_pBlobWriter->SetAttr ( iBlobAttr, dStr.first, dStr.second, sError );
			}
			break;

		case SPH_ATTR_UINT32SET:
		case SPH_ATTR_INT64SET:
			{
				const int64_t * pMva = &tDoc.m_dMvas[iMva];
				int nValues = (int)*pMva++;
				iMva += nValues+1;

				if ( tColumn.IsColumnar() )
					m_pColumnarBuilder->SetAttr ( iColumnarAttr, pMva, nValues );
				else
					m_pBlobWriter->SetAttr ( iBlobAttr, (const BYTE*)pMva, nValues*sizeof(int64_t), sError );
			}
			break;

		default:
			if ( tColumn.IsColumnar() )
				m_pColumnarBuilder->SetAttr ( iColumnarAttr, tDoc.m_dColumnarAttrs[iColumnarAttr] );

			break;
		}

		if ( tColumn.IsColumnar() )
			++iColumnarAttr;
		else if ( sphIsBlobAttr(tColumn) )
			++iBlobAttr;
	}

	if ( m_pBlobWriter )
	{
		const CSphColumnInfo * pBlobLoc = tSchema.GetAttr ( sphGetBlobLocatorName() );
		assert ( pBlobLoc );

		sphSetRowAttr ( pRow, pBlobLoc->m_tLocator, m_pBlobWriter->Flush() );
	}

	// handle index_field_lengths
	DWORD * pFieldLens = nullptr;
	if ( m_pIndex->GetSettings().m_bIndexFieldLens )
	{
		int iFirst = tSchema.GetAttrId_FirstFieldLen();
		assert ( tSchema.GetAttr ( iFirst ).m_eAttrType==SPH_ATTR_TOKENCOUNT );
		assert ( tSchema.GetAttr ( iFirst+tSchema.GetFieldsCount()-1 ).m_eAttrType==SPH_ATTR_TOKENCOUNT );
		pFieldLens = pRow + ( tSchema.GetAttr ( iFirst ).m_tLocator.m_iBitOffset / 32 );
		memset ( pFieldLens, 0, sizeof(int)*tSchema.GetFieldsCount() ); // NOLINT
	}

	// accumulate hits
	int iHits = 0;
	if ( pHits && !pHits->IsEmpty() )
	{
		CSphWordHit tLastHit;
		tLastHit.m_tRowID = INVALID_ROWID;
		tLastHit.m_uWordID = 0;
		tLastHit.m_uWordPos = 0;

		m_dAccum.ReserveGap ( pHits->GetLength() );
		iHits = 0;
		for ( CSphWordHit * pHit = pHits->Begin(); pHit<pHits->End(); ++pHit )
		{
			// ignore duplicate hits
			if ( *pHit==tLastHit )
				continue;

			// update field lengths
			if ( pFieldLens && HITMAN::GetField ( pHit->m_uWordPos )!=HITMAN::GetField ( tLastHit.m_uWordPos ) )
				pFieldLens [ HITMAN::GetField ( tLastHit.m_uWordPos ) ] = HITMAN::GetPos ( tLastHit.m_uWordPos );

			// need original hit for duplicate removal
			tLastHit = *pHit;
			// reset field end for not very last position
			if ( HITMAN::IsEnd ( pHit->m_uWordPos ) && pHit!=&pHits->Last() &&
				pHit->m_tRowID==pHit[1].m_tRowID && pHit->m_uWordID==pHit[1].m_uWordID && HITMAN::IsEnd ( pHit[1].m_uWordPos ) )
				pHit->m_uWordPos = HITMAN::GetPosWithField ( pHit->m_uWordPos );

			// accumulate
			m_dAccum.Add ( *pHit );
			++iHits;
		}
		if ( pFieldLens )
			pFieldLens [ HITMAN::GetField ( tLastHit.m_uWordPos ) ] = HITMAN::GetPos ( tLastHit.m_uWordPos );
	}
	// make sure to get real count without duplicated hits
	m_dPerDocHitsCount.Add ( iHits );

	if ( pStoredDoc )
	{
		SetupDocstore();
		m_pDocstore->AddDoc ( m_uAccumDocs, *pStoredDoc );
	}

	++m_uAccumDocs;
}


// cook checkpoints - make NULL terminating strings from offsets
static void FixupSegmentCheckpoints ( RtSegment_t * pSeg )
{
	assert ( pSeg &&
		( !pSeg->m_dWordCheckpoints.GetLength() || pSeg->m_dKeywordCheckpoints.GetLength() ) );
	if ( !pSeg->m_dWordCheckpoints.GetLength() )
		return;

	const char * pBase = (const char *)pSeg->m_dKeywordCheckpoints.Begin();
	assert ( pBase );
	for ( auto & dCheckpoint : pSeg->m_dWordCheckpoints )
		dCheckpoint.m_sWord = pBase + dCheckpoint.m_uWordID;
}


void RtAccum_t::CreateSegmentHits ( RtSegment_t * pSeg, int iWordsCheckpoint, ESphHitless eHitless, const VecTraits_T<SphWordID_t> & dHitlessWords )
{
	assert(pSeg);

	CSphWordHit& tClosingHit = m_dAccum.Add();
	tClosingHit.m_uWordID = WORDID_MAX;
	tClosingHit.m_tRowID = INVALID_ROWID;
	tClosingHit.m_uWordPos = EMPTY_HIT;

	RtDoc_t tDoc;
	RtWord_t tWord;
	RtDocWriter_c tOutDoc ( pSeg->m_dDocs );
	RtWordWriter_c tOutWord ( pSeg->m_dWords, pSeg->m_dWordCheckpoints, pSeg->m_dKeywordCheckpoints, m_bKeywordDict, iWordsCheckpoint, eHitless );
	RtHitWriter_c tOutHit ( pSeg->m_dHits );

	const BYTE * pPacketBase = m_bKeywordDict ? GetPackedKeywords() : nullptr;

	Hitpos_t uEmbeddedHit = EMPTY_HIT;
	Hitpos_t uPrevHit = EMPTY_HIT;

	for ( const CSphWordHit & tHit : m_dAccum )
	{
		// new keyword or doc; flush current doc
		if ( tHit.m_uWordID!=tWord.m_uWordID || tHit.m_tRowID!=tDoc.m_tRowID )
		{
			if ( tDoc.m_tRowID!=INVALID_ROWID )
			{
				++tWord.m_uDocs;
				tWord.m_uHits += tDoc.m_uHits;

				if ( uEmbeddedHit )
				{
					assert ( tDoc.m_uHits==1 );
					tDoc.m_uHit = uEmbeddedHit;
				}

				tOutDoc << tDoc;
				tDoc.m_uDocFields = 0;
				tDoc.m_uHits = 0;
				tDoc.m_uHit = tOutHit.WriterPos();
			}

			tDoc.m_tRowID = tHit.m_tRowID;
			tOutHit.ZipRestart ();
			uEmbeddedHit = EMPTY_HIT;
			uPrevHit = EMPTY_HIT;
		}

		// new keyword; flush current keyword
		if ( tHit.m_uWordID!=tWord.m_uWordID )
		{
			tOutDoc.ZipRestart ();
			if ( tWord.m_uWordID )
			{
				if ( m_bKeywordDict )
				{
					const BYTE * pPackedWord = pPacketBase + tWord.m_uWordID;
					assert ( pPackedWord[0] && pPackedWord[0]+1<GetPackedLen() );
					tWord.m_sWord = pPackedWord;
				}
				tOutWord << tWord;
			}

			tWord.m_uWordID = tHit.m_uWordID;
			tWord.m_uDocs = 0;
			tWord.m_uHits = 0;
			tWord.m_uDoc = tOutDoc.WriterPos();
			uPrevHit = EMPTY_HIT;
			if ( eHitless==SPH_HITLESS_NONE || eHitless==SPH_HITLESS_ALL || !tWord.m_uWordID || tHit.m_uWordPos==EMPTY_HIT )
			{
				tWord.m_bHasHitlist = ( eHitless==SPH_HITLESS_NONE || !tWord.m_uWordID );
			} else
			{
				SphWordID_t tWordID = tWord.m_uWordID;
				if ( m_bKeywordDict && dHitlessWords.GetLength() )
				{
					const BYTE * pPackedWord = pPacketBase + tWord.m_uWordID;
					DWORD uLen = pPackedWord[0];
					assert ( uLen && (int)uLen+1<GetPackedLen() );
					memcpy ( &tWordID, pPackedWord + uLen + 1, sizeof ( tWordID ) );
				}
				tWord.m_bHasHitlist = ( dHitlessWords.BinarySearch ( tWordID )==nullptr );
			}
		}

		// might be a duplicate
		if ( uPrevHit==tHit.m_uWordPos )
			continue;

		// just a new hit
		if ( !tWord.m_bHasHitlist )
		{
			if ( !tDoc.m_uHits )
				uEmbeddedHit = tHit.m_uWordPos;
			tDoc.m_uHits = 1; // FIXME!!! hitless hit-count always 1
		} else if ( !tDoc.m_uHits )
		{
			uEmbeddedHit = tHit.m_uWordPos;
			++tDoc.m_uHits;
		} else
		{
			if ( uEmbeddedHit )
				tOutHit << std::exchange ( uEmbeddedHit, 0 );

			tOutHit << tHit.m_uWordPos;
			++tDoc.m_uHits;
		}
		uPrevHit = tHit.m_uWordPos;

		const int iField = HITMAN::GetField ( tHit.m_uWordPos );
		if ( iField<32 )
			tDoc.m_uDocFields |= ( 1UL<<iField );
	}
}


RtSegment_t * RtAccum_t::CreateSegment ( int iRowSize, int iWordsCheckpoint, ESphHitless eHitless, const VecTraits_T<SphWordID_t> & dHitlessWords )
{
	if ( !m_uAccumDocs )
		return nullptr;

	MEMORY ( MEM_RT_ACCUM );
	auto * pSeg = new RtSegment_t ( m_uAccumDocs );
	FakeWL_t tFakeLock {pSeg->m_tLock};
	CreateSegmentHits ( pSeg, iWordsCheckpoint, eHitless, dHitlessWords );

	if ( m_bKeywordDict )
		FixupSegmentCheckpoints(pSeg);

	pSeg->m_uRows = m_uAccumDocs;
	pSeg->m_tAliveRows.store ( m_uAccumDocs, std::memory_order_relaxed );
	pSeg->m_dRows.SwapData(m_dAccumRows);
	pSeg->m_dBlobs.SwapData(m_dBlobs);
	pSeg->m_pDocstore.Swap(m_pDocstore);

	if ( m_pColumnarBuilder )
	{
		assert(m_pIndex);
		pSeg->m_pColumnar = CreateColumnarRT ( m_pIndex->GetInternalSchema(), m_pColumnarBuilder.Ptr() );
	}

	pSeg->BuildDocID2RowIDMap ( m_pIndex->GetInternalSchema() );

	m_tNextRowID = 0;

	// done
	return pSeg;
}


struct AccumDocHits_t
{
	DocID_t	m_tDocID;
	int		m_iDocIndex;
	int		m_iHitIndex;
	int		m_iHitCount;
};


struct CmpDocHitIndex_t
{
	inline static bool IsLess ( const AccumDocHits_t & a, const AccumDocHits_t & b )
	{
		return ( a.m_tDocID<b.m_tDocID || ( a.m_tDocID==b.m_tDocID && a.m_iDocIndex<b.m_iDocIndex ) );
	}
};


void RtAccum_t::CleanupDuplicates ( int iRowSize )
{
	if ( m_uAccumDocs<=1 )
		return;

	assert ( m_uAccumDocs==(DWORD)m_dPerDocHitsCount.GetLength() );
	CSphVector<AccumDocHits_t> dDocHits ( m_dPerDocHitsCount.GetLength() );

	assert(m_pIndex);
	const CSphSchema & tSchema = m_pIndex->GetInternalSchema();
	bool bColumnarId = tSchema.GetAttr(0).IsColumnar();

	// create temporary columnar accessor; don't take ownership of built attributes
	CSphScopedPtr<columnar::Columnar_i> pColumnar ( CreateColumnarRT ( m_pIndex->GetInternalSchema(), m_pColumnarBuilder.Ptr(), false ) );

	std::string sError;
	CSphScopedPtr<columnar::Iterator_i> pColumnarIdIterator(nullptr);
	if ( bColumnarId )
	{
		pColumnarIdIterator = pColumnar->CreateIterator ( sphGetDocidName(), {}, sError );
		assert ( pColumnarIdIterator.Ptr() );
	}

	int iHitIndex = 0;
	CSphRowitem * pRow = m_dAccumRows.Begin();
	for ( DWORD i=0; i<m_uAccumDocs; i++, pRow+=iRowSize )
	{
		AccumDocHits_t & tElem = dDocHits[i];
		if ( !bColumnarId )
			tElem.m_tDocID = sphGetDocID ( pRow );
		else
		{
			assert ( pColumnarIdIterator.Ptr() );
			Verify ( AdvanceIterator ( pColumnarIdIterator.Ptr(), i ) );
			tElem.m_tDocID = pColumnarIdIterator->Get();
		}

		tElem.m_iDocIndex = i;
		tElem.m_iHitIndex = iHitIndex;
		tElem.m_iHitCount = m_dPerDocHitsCount[i];
		iHitIndex += m_dPerDocHitsCount[i];
	}

	dDocHits.Sort ( CmpDocHitIndex_t() );

	DocID_t uPrev = 0;
	if ( !dDocHits.any_of ( [&] ( const AccumDocHits_t &dDoc ) {
			bool bRes = dDoc.m_tDocID==uPrev;
			uPrev = dDoc.m_tDocID;
			return bRes;
		} ) )
		return;

	CSphFixedVector<RowID_t> dRowMap(m_uAccumDocs);
	for ( auto & i : dRowMap )
		i = 0;

	// identify duplicates to kill
	if ( m_bReplace )
	{
		// replace mode, last value wins, precending values are duplicate
		for ( DWORD i=0; i<m_uAccumDocs-1; ++i )
			if ( dDocHits[i].m_tDocID==dDocHits[i+1].m_tDocID )
				dRowMap[dDocHits[i].m_iDocIndex] = INVALID_ROWID;
	} else
	{
		// insert mode, first value wins, subsequent values are duplicates
		for ( DWORD i=1; i<m_uAccumDocs; ++i )
			if ( dDocHits[i].m_tDocID==dDocHits[i-1].m_tDocID )
				dRowMap[dDocHits[i].m_iDocIndex] = INVALID_ROWID;
	}

	RowID_t tNextRowID = 0;
	for ( auto & i : dRowMap )
		if ( i!=INVALID_ROWID )
			i = tNextRowID++;

	// remove duplicate hits
	int iDstRow = 0;
	for ( int i=0, iLen=m_dAccum.GetLength(); i<iLen; ++i )
	{
		const auto & dSrcHit = m_dAccum[i];
		RowID_t tSrcRowID = dRowMap[dSrcHit.m_tRowID];
		if ( tSrcRowID!=INVALID_ROWID )
		{
			CSphWordHit & tDstHit = m_dAccum[iDstRow];
			if ( i!=iDstRow )
			{
				tDstHit = dSrcHit;
				tDstHit.m_tRowID = tSrcRowID;
			}
			++iDstRow;
		}
	}

	m_dAccum.Resize( iDstRow);

	// remove duplicates
	if ( m_pColumnarBuilder )
	{
		CSphVector<RowID_t> dKilled;
		ARRAY_FOREACH ( i, dRowMap )
			if ( dRowMap[i]==INVALID_ROWID )
				dKilled.Add(i);

		if ( m_pColumnarBuilder )
			m_pColumnarBuilder->Kill(dKilled);
	}

	iDstRow = 0;
	ARRAY_FOREACH ( i, dRowMap )
		if ( dRowMap[i]!=INVALID_ROWID )
		{
			if ( i!=iDstRow )
			{
				// remove duplicate docinfo
				memcpy ( &m_dAccumRows[iDstRow * iRowSize], &m_dAccumRows[i * iRowSize], iRowSize * sizeof ( CSphRowitem ) );

				// remove duplicate docstore
				if ( m_pDocstore )
					m_pDocstore->SwapRows ( iDstRow, i );
			}
			++iDstRow;
		}

	m_dAccumRows.Resize ( iDstRow * iRowSize );
	m_uAccumDocs = iDstRow;
	if ( m_pDocstore )
		m_pDocstore->DropTail ( iDstRow );
}


void RtAccum_t::GrabLastWarning ( CSphString & sWarning )
{
	if ( m_pDictRt && m_pDictRt->GetLastWarning() )
	{
		sWarning = m_pDictRt->GetLastWarning();
		m_pDictRt->ResetWarning();
	}
}


void RtAccum_t::SetIndex ( RtIndex_i * pIndex )
{
	m_pIndex = pIndex;
	m_pBlobWriter.Reset();
	if ( !pIndex )
		return;
		
	const CSphSchema & tSchema = pIndex->GetInternalSchema();
	if ( tSchema.HasBlobAttrs() )
		m_pBlobWriter = sphCreateBlobRowBuilder ( tSchema, m_dBlobs );

	if ( !m_pColumnarBuilder )
		m_pColumnarBuilder = CreateColumnarBuilderRT(tSchema);

	m_uSchemaHash = pIndex->GetSchemaHash();
}


RowID_t RtAccum_t::GenerateRowID()
{
	return m_tNextRowID++;
}


void RtAccum_t::ResetRowID()
{
	m_tNextRowID=0;
}

ReplicationCommand_t * RtAccum_t::AddCommand ( ReplicationCommand_e eCmd )
{
	return AddCommand ( eCmd, CSphString(), CSphString() );
}

ReplicationCommand_t * RtAccum_t::AddCommand ( ReplicationCommand_e eCmd, const CSphString & sCluster, const CSphString & sIndex )
{
	// all writes to RT index go as single command to serialize accumulator
	if ( eCmd==ReplicationCommand_e::RT_TRX && m_dCmd.GetLength() && m_dCmd.Last()->m_eCommand==ReplicationCommand_e::RT_TRX )
		return m_dCmd.Last();

	auto* pCmd = new ReplicationCommand_t ();
	m_dCmd.Add ( pCmd );
	pCmd->m_eCommand = eCmd;
	pCmd->m_sCluster = sCluster;
	pCmd->m_sIndex = sIndex;

	return pCmd;
}

void RtIndex_c::CopyWord ( RtSegment_t& tDstSeg, RtWord_t& tDstWord, RtDocWriter_c& tDstDoc, const RtSegment_t& tSrcSeg, const RtWord_t* pSrcWord, const VecTraits_T<RowID_t>& dRowMap )
{
	RtDocReader_c tSrcDocs ( &tSrcSeg, *pSrcWord );
	// copy docs
	while ( tSrcDocs.UnzipDoc() )
	{
		auto tNewRowID = dRowMap[tSrcDocs->m_tRowID];
		if ( tNewRowID==INVALID_ROWID )
			continue;

		RtDoc_t tDoc = *tSrcDocs;
		tDoc.m_tRowID = tNewRowID;

		++tDstWord.m_uDocs;
		tDstWord.m_uHits += tSrcDocs->m_uHits;

		if ( tSrcDocs->m_uHits!=1 )
		{
			tDoc.m_uHit = tDstSeg.m_dHits.GetLength ();
			tDstSeg.m_dHits.Append ( GetHitsBlob ( tSrcSeg, *tSrcDocs ) );

			// this is reference of what append (hitsblob) above does.
//			RtHitWriter_c tOutHit ( &tDst );
//			tDoc.m_uHit = tOutHit.ZipHitPos();
//			for ( DWORD uValue=tInHit.UnzipHit(); uValue; uValue=tInHit.UnzipHit() )
//				tOutHit.ZipHit ( uValue );
		}

		// copy doc
		tDstDoc << tDoc;
	}
}

template<typename FN>
inline void ProcessField ( RtDoc_t & tOutDoc, DWORD uHit, int iKillField, FN&& fnProcessor )
{
	assert ( iKillField >=0 );

	int iField = HITMAN::GetField ( uHit );
	if ( iKillField==iField )
		return;

	if ( iField>iKillField )
	{
		--iField;
		HITMAN::DecrementField ( uHit );
	}

	++tOutDoc.m_uHits;
	if ( iField<32 )
		tOutDoc.m_uDocFields |= ( 1UL << iField );

	fnProcessor ( uHit );
}


static void CopyWordWithoutField ( CSphTightVector<BYTE> * pOutHits, RtDocWriter_c & tOutDocs, RtWord_t & tOutWord, const RtSegment_t & tSrc, RtDocReader_c & tInDocs, int iKillField  )
{
	assert ( iKillField>=0 );

	while ( tInDocs.UnzipDoc() )
	{
		RtDoc_t tOutDoc = *tInDocs;
		tOutDoc.m_uHits = tOutDoc.m_uDocFields = 0;

		if ( tInDocs->m_uHits!=1 )
		{
			RtHitReader_c tInHits ( tSrc, *tInDocs );
			RtHitWriter_c tOutHits ( *pOutHits );
			tOutDoc.m_uHit = tOutHits.WriterPos();
			while ( tInHits.UnzipHit() )
				ProcessField ( tOutDoc, *tInHits, iKillField, [&tOutHits] ( Hitpos_t x ) { tOutHits << x; } );
		} else
			ProcessField ( tOutDoc, tOutDoc.m_uHit, iKillField, [&tOutDoc] ( Hitpos_t x ) { tOutDoc.m_uHit = x; } );

		if ( !tOutDoc.m_uHits )
			continue;

		tOutDocs << tOutDoc;

		++tOutWord.m_uDocs;
		tOutWord.m_uHits += tOutDoc.m_uHits;
	}
}


void RtIndex_c::DeleteFieldFromDict ( RtSegment_t * pSeg, int iKillField )
{
	assert ( iKillField>=0 );

	CSphTightVector<BYTE> dWords;
	CSphVector<RtWordCheckpoint_t> dWordCheckpoints;
	CSphTightVector<BYTE> dDocs;
	CSphTightVector<BYTE> dHits;
	CSphVector<BYTE> dKeywordCheckpoints;

	const RtSegment_t & tInSeg = *pSeg;
	dWords.Reserve ( tInSeg.m_dWords.GetLength () );
	dDocs.Reserve ( tInSeg.m_dDocs.GetLength () );
	dHits.Reserve ( tInSeg.m_dHits.GetLength () );

	RtWordWriter_c tOutWords ( dWords, dWordCheckpoints, dKeywordCheckpoints,  m_bKeywordDict, m_iWordsCheckpoint, m_tSettings.m_eHitless );
	RtWordReader_c tInWord ( &tInSeg, m_bKeywordDict, m_iWordsCheckpoint, m_tSettings.m_eHitless );

	RtWord_t tOutWord;
	while ( tInWord.UnzipWord() )
	{
		tOutWord = *tInWord;
		tOutWord.m_uDocs = tOutWord.m_uHits = 0;

		RtDocWriter_c tOutDocs ( dDocs );
		tOutWord.m_uDoc = tOutDocs.WriterPos();

		RtDocReader_c tInDocs ( &tInSeg, *tInWord );
		CopyWordWithoutField ( &dHits, tOutDocs, tOutWord, tInSeg, tInDocs, iKillField );

		// append word to the dictionary
		if ( tOutWord.m_uDocs )
			tOutWords << tOutWord;
	}

	// swap data to refreshed
	RtSegment_t & tOutSeg = *pSeg;
	tOutSeg.m_dWords.SwapData ( dWords );
	tOutSeg.m_dWordCheckpoints.SwapData ( dWordCheckpoints );
	tOutSeg.m_dDocs.SwapData ( dDocs );
	tOutSeg.m_dHits.SwapData ( dHits );
	tOutSeg.m_dKeywordCheckpoints.SwapData ( dKeywordCheckpoints );

	if ( m_bKeywordDict )
		FixupSegmentCheckpoints ( pSeg );

	BuildSegmentInfixes ( &tOutSeg, m_pDict->HasMorphology (), m_bKeywordDict, m_tSettings.m_iMinInfixLen, m_iWordsCheckpoint, ( m_iMaxCodepointLength>1 ), m_tSettings.m_eHitless );
}


// iterate over alive rows range
class RtLiveRows_c
{
public:
	class Iterator_c
	{
	public:
		Iterator_c ( const RtLiveRows_c & tOwner, bool bBegin )
			: m_tOwner { tOwner }
			, m_tRowID { bBegin ? tOwner.FirstAliveRow () : tOwner.EndRow() }
		{}

		RowID_t operator*()	const { return m_tRowID; };
		bool operator!= ( const Iterator_c & rhs ) const { return m_tRowID!=rhs.m_tRowID; }

		Iterator_c & operator++ ()
		{
			m_tRowID = m_tOwner.NextAliveRow(m_tRowID);
			return *this;
		}
		
	private:
		const RtLiveRows_c & m_tOwner;
		RowID_t m_tRowID = 0;
	};

	explicit RtLiveRows_c ( const RtSegment_t & tSeg )
		: m_tRowIDMax ( tSeg.m_uRows )
		, m_tDeadRowMap ( tSeg.m_tDeadRowMap )
	{}

	// c++11 style iteration
	Iterator_c begin () const { return { *this, true }; }
	Iterator_c end() const { return { *this, false }; }

private:
	RowID_t		m_tRowID = 0;
	RowID_t		m_tRowIDMax = 0;
	const DeadRowMap_Ram_c & m_tDeadRowMap;

	RowID_t SkipDeadRows ( RowID_t tRowID ) const
	{
		while ( tRowID<m_tRowIDMax && m_tDeadRowMap.IsSet(tRowID) )
			++tRowID;

		return tRowID;
	}

	RowID_t FirstAliveRow() const			{ return SkipDeadRows(m_tRowID); }
	RowID_t EndRow() const					{ return m_tRowIDMax; }
	RowID_t NextAliveRow ( RowID_t tRowID ) const { return SkipDeadRows ( tRowID+1 ); }
};

template <typename BLOOM_TRAITS>
inline bool BuildBloom_T ( const BYTE * sWord, int iLen, int iInfixCodepointCount, bool bUtf8, int iKeyValCount, BLOOM_TRAITS & tBloom )
{
	if ( iLen<iInfixCodepointCount )
		return false;
	// byte offset for each codepoints
	BYTE dOffsets [ SPH_MAX_WORD_LEN+1 ] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
		20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42 };
	assert ( iLen<=SPH_MAX_WORD_LEN || ( bUtf8 && iLen<=SPH_MAX_WORD_LEN*3 ) );
	int iCodes = iLen;
	if ( bUtf8 )
	{
		// build an offsets table into the bytestring
		iCodes = 0;
		const BYTE * s = sWord;
		const BYTE * sEnd = sWord + iLen;
		while ( s<sEnd )
		{
			int iCodepoints = sphUtf8CharBytes ( *s );
			assert ( iCodepoints>=1 && iCodepoints<=4 );
			dOffsets[iCodes+1] = dOffsets[iCodes] + (BYTE)iCodepoints;
			s += iCodepoints;
			iCodes++;
		}
	}
	if ( iCodes<iInfixCodepointCount )
		return false;

	int iKeyBytes = iKeyValCount * 64;
	for ( int i=0; i<=iCodes-iInfixCodepointCount && tBloom.IterateNext(); i++ )
	{
		int iFrom = dOffsets[i];
		int iTo = dOffsets[i+iInfixCodepointCount];
		uint64_t uHash64 = sphFNV64 ( sWord+iFrom, iTo-iFrom );

		uHash64 = ( uHash64>>32 ) ^ ( (DWORD)uHash64 );
		int iByte = (int)( uHash64 % iKeyBytes );
		int iPos = iByte/64;
		uint64_t uVal = U64C(1) << ( iByte % 64 );

		tBloom.Set ( iPos, uVal );
	}
	return true;
}

// explicit instantiations
bool BuildBloom ( const BYTE * sWord, int iLen, int iInfixCodepointCount, bool bUtf8, int iKeyValCount
				  , BloomCheckTraits_t &tBloom )
{
	return BuildBloom_T ( sWord, iLen, iInfixCodepointCount, bUtf8, iKeyValCount, tBloom );
}

bool BuildBloom ( const BYTE * sWord, int iLen, int iInfixCodepointCount, bool bUtf8, int iKeyValCount
				  , BloomGenTraits_t &tBloom )
{
	return BuildBloom_T ( sWord, iLen, iInfixCodepointCount, bUtf8, iKeyValCount, tBloom );
}

void BuildSegmentInfixes ( RtSegment_t * pSeg, bool bHasMorphology, bool bKeywordDict, int iMinInfixLen, int iWordsCheckpoint, bool bUtf8, ESphHitless eHitlessMode )
{
	if ( !pSeg || !bKeywordDict || !iMinInfixLen )
		return;

	int iBloomSize = ( pSeg->m_dWordCheckpoints.GetLength()+1 ) * BLOOM_PER_ENTRY_VALS_COUNT * BLOOM_HASHES_COUNT;
	pSeg->m_dInfixFilterCP.Resize ( iBloomSize );
	// reset filters
	pSeg->m_dInfixFilterCP.Fill ( 0 );

	uint64_t * pRough = pSeg->m_dInfixFilterCP.Begin();
	RtWordReader_c rdDictRough ( pSeg, true, iWordsCheckpoint, eHitlessMode );
	while ( rdDictRough.UnzipWord() )
	{
		const BYTE * pDictWord = rdDictRough->m_sWord+1;
		if ( bHasMorphology && *pDictWord!=MAGIC_WORD_HEAD_NONSTEMMED )
			continue;

		int iLen = rdDictRough->m_sWord[0];
		if ( *pDictWord<0x20 ) // anyway skip heading magic chars in the prefix, like NONSTEMMED maker
		{
			pDictWord++;
			iLen--;
		}

		uint64_t * pVal = pRough + rdDictRough.Checkpoint() * BLOOM_PER_ENTRY_VALS_COUNT * BLOOM_HASHES_COUNT;
		BloomGenTraits_t tBloom0 ( pVal );
		BloomGenTraits_t tBloom1 ( pVal+BLOOM_PER_ENTRY_VALS_COUNT );
		BuildBloom ( pDictWord, iLen, BLOOM_NGRAM_0, bUtf8, BLOOM_PER_ENTRY_VALS_COUNT, tBloom0 );
		BuildBloom ( pDictWord, iLen, BLOOM_NGRAM_1, bUtf8, BLOOM_PER_ENTRY_VALS_COUNT, tBloom1 );
	}
}


CSphFixedVector<RowID_t> RtIndex_c::CopyAttributesFromAliveDocs ( RtSegment_t & tDstSeg, const RtSegment_t & tSrcSeg, RtAttrMergeContext_t & tCtx ) const REQUIRES ( tDstSeg.m_tLock )
{
	CSphFixedVector<RowID_t> dRowMap { tSrcSeg.m_uRows };
	dRowMap.Fill ( INVALID_ROWID );

	// mark us busy - updates to this seg will be collected.
	tSrcSeg.m_bAttrsBusy.store ( true, std::memory_order_release );

	// perform merging attrs in single fiber - that eliminates concurrency with optimize
	ScopedScheduler_c tSerialFiber { m_tRtChunks.SerialChunkAccess() };

	auto dColumnarIterators = CreateAllColumnarIterators ( tSrcSeg.m_pColumnar.Ptr(), m_tSchema );
	CSphVector<int64_t> dTmp;

	const CSphColumnInfo * pBlobRowLocator = m_tSchema.GetAttr ( sphGetBlobLocatorName() );

	// ensure no update will break (resize) blob when we're merging
	SccRL_t rLock ( tSrcSeg.m_tLock );

	for ( auto tRowID : RtLiveRows_c(tSrcSeg) )
	{
		CSphRowitem * pRow = tSrcSeg.m_dRows.Begin() + (int64_t)tRowID * m_iStride;
		auto pNewRow = tDstSeg.m_dRows.AddN ( m_iStride );
		memcpy ( pNewRow, pRow, m_iStride*sizeof(CSphRowitem) );

		if ( tCtx.m_iNumBlobs )
		{
			assert ( pBlobRowLocator ) ;
			int64_t iOldOffset = sphGetRowAttr ( pRow , pBlobRowLocator->m_tLocator );
			int64_t iNewOffset = sphCopyBlobRow ( tDstSeg.m_dBlobs, tSrcSeg.m_dBlobs, iOldOffset, tCtx.m_iNumBlobs );
			sphSetRowAttr ( pNewRow, pBlobRowLocator->m_tLocator, iNewOffset );
		}

		ARRAY_FOREACH ( i, dColumnarIterators )
		{
			auto & tIt = dColumnarIterators[i];
			Verify ( AdvanceIterator ( tIt.first.get(), tRowID ) );
			SetColumnarAttr ( i, tIt.second, tCtx.m_pColumnarBuilder, tIt.first.get(), dTmp );
		}

		if ( tDstSeg.m_pDocstore )
			tDstSeg.m_pDocstore->AddPackedDoc ( tCtx.m_tResultRowID, tSrcSeg.m_pDocstore.Ptr(), tRowID );

		dRowMap[tRowID] = tCtx.m_tResultRowID++;
	}
	return dRowMap;
}


int RtIndex_c::CompareWords ( const RtWord_t * pWord1, const RtWord_t * pWord2 ) const
{
	if ( !pWord1 )
		return pWord2 ? 1 : 0;

	if ( !pWord2 )
		return -1;

	if ( m_bKeywordDict )
		return sphDictCmpStrictly ( (const char *)pWord1->m_sWord+1, *pWord1->m_sWord, (const char *)pWord2->m_sWord+1, *pWord2->m_sWord );

	if ( pWord1->m_uWordID<pWord2->m_uWordID )
		return -1;

	if ( pWord1->m_uWordID>pWord2->m_uWordID )
		return 1;

	return 0;
}


void RtIndex_c::MergeKeywords ( RtSegment_t & tSeg, const RtSegment_t & tSeg1, const RtSegment_t & tSeg2,
		const VecTraits_T<RowID_t> & dRowMap1, const VecTraits_T<RowID_t> & dRowMap2 ) const
{
	tSeg.m_dWords.Reserve ( Max ( tSeg1.m_dWords.GetLength(), tSeg2.m_dWords.GetLength() ) );
	tSeg.m_dDocs.Reserve ( Max ( tSeg1.m_dDocs.GetLength(), tSeg2.m_dDocs.GetLength() ) );
	tSeg.m_dHits.Reserve ( Max ( tSeg1.m_dHits.GetLength(), tSeg2.m_dHits.GetLength() ) );

	RtDocWriter_c tOutDoc ( tSeg.m_dDocs );
	RtWordWriter_c tOut ( tSeg.m_dWords, tSeg.m_dWordCheckpoints, tSeg.m_dKeywordCheckpoints, m_bKeywordDict, m_iWordsCheckpoint, m_tSettings.m_eHitless );
	RtWordReader_c tIn1 ( &tSeg1, m_bKeywordDict, m_iWordsCheckpoint, m_tSettings.m_eHitless );
	RtWordReader_c tIn2 ( &tSeg2, m_bKeywordDict, m_iWordsCheckpoint, m_tSettings.m_eHitless );

	const RtWord_t* pWords1 = tIn1.UnzipWord();
	const RtWord_t* pWords2 = tIn2.UnzipWord();

	while ( pWords1 || pWords2 )
	{
		int iCmp = CompareWords ( pWords1, pWords2 );
		RtWord_t tWord = iCmp<=0 ? *pWords1 : *pWords2;
		tWord.m_uDocs = 0;
		tWord.m_uHits = 0;
		tWord.m_uDoc = tOutDoc.WriterPos();

		// if words are equal, copy both
		if ( iCmp<=0 )
			CopyWord ( tSeg, tWord, tOutDoc, tSeg1, pWords1, dRowMap1 );

		if ( iCmp>=0 )
			CopyWord ( tSeg, tWord, tOutDoc, tSeg2, pWords2, dRowMap2 );

		// append non-empty word to the dictionary
		if ( tWord.m_uDocs )
			tOut << tWord;

		// move forward. Beware, tWord refer to static buffer inside tIn1 or tIn2, so call UnzipWord() before storing the word is NOT safe!
		if ( iCmp <= 0 )
			pWords1 = tIn1.UnzipWord(); // move forward

		if ( iCmp >= 0 )
			pWords2 = tIn2.UnzipWord(); // move forward

		tOutDoc.ZipRestart();
	}
}


// it seems safe to kill documents directly during merging.
// already killed will not come to the merged.
// killed after pass of merge attributes will survive, and need to be killed finally by separate killmulti
RtSegment_t* RtIndex_c::MergeTwoSegments ( const RtSegment_t* pA, const RtSegment_t* pB ) const
{
	////////////////////
	// merge attributes
	////////////////////
	RTRLOG << "MergeTwoSegments invoked";

	int nBlobAttrs = 0;
	for ( int i = 0; i < m_tSchema.GetAttrsCount(); ++i )
		if ( sphIsBlobAttr ( m_tSchema.GetAttr(i) ) )
			++nBlobAttrs;

	RowID_t tNextRowID = 0;

	CSphScopedPtr<ColumnarBuilderRT_i> pColumnarBuilder ( CreateColumnarBuilderRT(m_tSchema) );
	RtAttrMergeContext_t tCtx ( nBlobAttrs, tNextRowID, pColumnarBuilder.Ptr() );

	auto * pSeg = new RtSegment_t (0);
	FakeWL_t _ { pSeg->m_tLock }; // as pSeg is just created - we don't need real guarding and use fake lock to mute thread safety warnings

	if ( m_tSchema.HasStoredFields() )
		pSeg->SetupDocstore ( &m_tSchema );

	// we might need less because of killed, but we can not know yet. Reserving more than necessary is strictly not desirable!
	pSeg->m_dRows.Reserve ( m_iStride * Max ( pA->m_tAliveRows.load ( std::memory_order_relaxed ), pB->m_tAliveRows.load (std::memory_order_relaxed ) ) );
	pSeg->m_dBlobs.Reserve ( [pA, pB]() NO_THREAD_SAFETY_ANALYSIS { return Max ( pA->m_dBlobs.GetLength(), pB->m_dBlobs.GetLength() ); }() );

	CSphFixedVector<RowID_t> dRowMapA = CopyAttributesFromAliveDocs ( *pSeg, *pA, tCtx );
	CSphFixedVector<RowID_t> dRowMapB = CopyAttributesFromAliveDocs ( *pSeg, *pB, tCtx );

	assert ( tNextRowID<=INT_MAX );
	pSeg->m_uRows = tNextRowID;
	pSeg->m_tAliveRows.store ( pSeg->m_uRows, std::memory_order_relaxed );
	pSeg->m_tDeadRowMap.Reset ( pSeg->m_uRows );
	pSeg->m_pColumnar = CreateColumnarRT ( m_tSchema, pColumnarBuilder.Ptr() );

	RTRLOG << "MergeTwoSegments: new seg has " << pSeg->m_uRows << " rows";

	// merged segment might be completely killed by committed data
	if ( !pSeg->m_uRows )
	{
		SafeRelease ( pSeg );
		return nullptr;
	}

	assert ( pSeg->GetStride() == m_iStride );
	pSeg->BuildDocID2RowIDMap ( m_tSchema );
	MergeKeywords ( *pSeg, *pA, *pB, dRowMapA, dRowMapB );

	if ( m_bKeywordDict )
		FixupSegmentCheckpoints ( pSeg );

	BuildSegmentInfixes ( pSeg, m_pDict->HasMorphology(), m_bKeywordDict, m_tSettings.m_iMinInfixLen, m_iWordsCheckpoint, ( m_iMaxCodepointLength>1 ), m_tSettings.m_eHitless );

	assert ( pSeg->m_uRows );
	assert ( pSeg->m_tAliveRows==pSeg->m_uRows );

	return pSeg;
}

namespace GatherUpdates {

	struct UpdHashFn { static inline uintptr_t Hash ( const CSphAttrUpdate* pK ) { return (uintptr_t) pK; } };
	using HashedUpd_t = CSphOrderedHash<PostponedUpdate_t*, AttrUpdateSharedPtr_t, UpdHashFn, 256>;

	inline bool CanBePeeked (int i, const CSphVector<HashedUpd_t>& dUpdates, const CSphVector<HashedUpd_t::Iterator_c>& dHeads)
	{
		ARRAY_CONSTFOREACH ( j, dHeads )
			if ( dHeads[i]->first!=dHeads[j]->first && dUpdates[j].Exists ( dHeads[i]->first ) )
				return false;
		return true;
	};

	inline void Peek ( int i, const CSphVector<HashedUpd_t> & dUpdates, CSphVector<HashedUpd_t::Iterator_c> & dHeads, CSphVector<PostponedUpdate_t> & dSerializedUpdates )
	{
		auto a = dHeads[i]->first;
		auto& tNewUpdate = dSerializedUpdates.Add();
		tNewUpdate.m_pUpdate = a;
		ARRAY_FOREACH ( j, dHeads )
			if ( a==dHeads[j]->first )
			{
				tNewUpdate.m_dRowsToUpdate.Append ( dHeads[j]->second->m_dRowsToUpdate );
				dHeads[j]->second->m_pUpdate = nullptr;
				dHeads[j]->second->m_dRowsToUpdate.Reset();
				++dHeads[j];
				if ( dHeads[j]==dUpdates[j].end() )
					dHeads.RemoveFast (j--);
			}
	};

	const VecTraits_T<PostponedUpdate_t>& AccessPostponedUpdates ( const ConstRtSegmentRefPtf_t& pSeg ) { return pSeg->m_dPostponedUpdates; }
	const VecTraits_T<PostponedUpdate_t>& AccessPostponedUpdates ( const ConstDiskChunkRefPtr_t& pChunk ) { return pChunk->Cidx ().m_dPostponedUpdates; }

	template<typename CHUNK_OR_SEG>
	CSphVector<PostponedUpdate_t> FromChunksOrSegments ( VecTraits_T<CHUNK_OR_SEG> dChunksOrSegmengs )
	{
		CSphVector<PostponedUpdate_t> dResult;
		CSphVector<HashedUpd_t> dUpdates;

		for ( const auto& dSeg : dChunksOrSegmengs )
		{
			const VecTraits_T<PostponedUpdate_t>& dPostponedUpdates = AccessPostponedUpdates (dSeg);
			if ( dPostponedUpdates.IsEmpty () )
				continue;
			auto& dHash = dUpdates.Add();
			for ( auto& tPostponedUpdate : dPostponedUpdates )
				dHash.Add ( &tPostponedUpdate, tPostponedUpdate.m_pUpdate );
		}

		if ( dUpdates.IsEmpty() )
			return dResult;

		CSphVector<HashedUpd_t::Iterator_c> dHeads ( dUpdates.GetLength() );
		ARRAY_CONSTFOREACH ( i, dUpdates )
			dHeads[i] = dUpdates[i].begin();

		while (!dHeads.IsEmpty())
			ARRAY_FOREACH ( i, dHeads )
				if ( CanBePeeked ( i, dUpdates, dHeads ) )
				{
					Peek ( i, dUpdates, dHeads, dResult );
					break;
				}

		return dResult;
	}

}; // namespace


bool RtIndex_c::UpdateAttributesInRamSegment ( const RowsToUpdate_t& dRows, UpdateContext_t& tCtx, bool& bCritical, CSphString& sError ) REQUIRES ( m_tRtChunks.SerialChunkAccess() )
{
	if ( tCtx.m_tUpd.m_pUpdate->m_bStrict )
		if ( !Update_InplaceJson ( dRows, tCtx, sError, true ) )
			return false;

	// second pass
	int iSaveWarnings = tCtx.m_iJsonWarnings;
	tCtx.m_iJsonWarnings = 0;
	Update_InplaceJson ( dRows, tCtx, sError, false );
	tCtx.m_iJsonWarnings += iSaveWarnings;

	if ( !Update_Blobs ( dRows, tCtx, bCritical, sError ) )
		return false;

	Update_Plain ( dRows, tCtx );
	return true;
}

// that is 2-nd part of postponed updates. We may have one or several update set, stored from old segments.
void RtIndex_c::UpdateAttributesOffline ( VecTraits_T<PostponedUpdate_t> & dUpdates, IndexSegment_c * pSeg ) NO_THREAD_SAFETY_ANALYSIS
{
	if ( dUpdates.IsEmpty() )
		return;

	assert ( pSeg && "for RT index UpdateAttributesOffline should be called only with non-null pSeg!" );
	auto * pSegment = (RtSegment_t *) pSeg;

	CSphString sError;
	bool bCritical;
	for ( auto & tUpdate : dUpdates )
	{
		AttrUpdateInc_t tUpdInc { std::move ( tUpdate.m_pUpdate ) };
		UpdateContext_t tCtx ( tUpdInc, m_tSchema );
		Update_PrepareListOfUpdatedAttributes ( tCtx, sError );

		// actualize list of updates in context of new segment
		const auto & dDocids = tUpdate.m_pUpdate->m_dDocids;
		ARRAY_FOREACH ( i, tUpdate.m_dRowsToUpdate )
		{
			auto& tRow = tUpdate.m_dRowsToUpdate[i];
			auto pRow = pSegment->FindAliveRow ( dDocids[tRow.m_iIdx] );
			if ( pRow )
				tRow.m_pRow = pRow;
			else
				tUpdate.m_dRowsToUpdate.RemoveFast ( i-- );
		}

		tCtx.m_pAttrPool = pSegment->m_dRows.begin();
		tCtx.m_pBlobPool = pSegment->m_dBlobs.begin();
		tCtx.m_pSegment = pSeg;
		UpdateAttributesInRamSegment ( tUpdate.m_dRowsToUpdate, tCtx, bCritical, sError );
	}
}

bool RtIndex_c::Commit ( int * pDeleted, RtAccum_t * pAccExt )
{
	assert ( g_bRTChangesAllowed );
	MEMORY ( MEM_INDEX_RT );

	auto pAcc = ( RtAccum_t * ) AcquireAccum ( m_pDict, pAccExt, m_bKeywordDict );
	if ( !pAcc )
		return true;

	// empty txn, just ignore
	if ( !pAcc->m_uAccumDocs && pAcc->m_dAccumKlist.IsEmpty() )
	{
		pAcc->Cleanup();
		return true;
	}

	// phase 0, build a new segment
	// accum and segment are thread local; so no locking needed yet
	// segment might be NULL if we're only killing rows this txn
	pAcc->CleanupDuplicates ( m_tSchema.GetRowSize() );
	pAcc->Sort();

	RtSegmentRefPtf_t pNewSeg { pAcc->CreateSegment ( m_tSchema.GetRowSize(), m_iWordsCheckpoint, m_tSettings.m_eHitless, m_dHitlessWords ) };
	assert ( !pNewSeg || pNewSeg->m_uRows>0 );
	assert ( !pNewSeg || pNewSeg->m_tAliveRows>0 );

	BuildSegmentInfixes ( pNewSeg, m_pDict->HasMorphology(), m_bKeywordDict, m_tSettings.m_iMinInfixLen, m_iWordsCheckpoint, ( m_iMaxCodepointLength>1 ), m_tSettings.m_eHitless );

	// clean up parts we no longer need
	pAcc->CleanupPart();

	// sort accum klist, too
	pAcc->m_dAccumKlist.Uniq ();

	// now on to the stuff that needs locking and recovery
	auto iKilled = CommitReplayable ( pNewSeg, pAcc->m_dAccumKlist );
	if ( pDeleted )
		*pDeleted = iKilled;

	// done; cleanup accum
	pAcc->Cleanup();

	// reset accumulated warnings
	CSphString sWarning;
	pAcc->GrabLastWarning ( sWarning );
	return true;
}

ConstRtSegmentRefPtf_t RtIndex_c::AdoptSegment ( RtSegment_t * pNewSeg )
{
	ConstRtSegmentRefPtf_t tResult {pNewSeg};
	if ( pNewSeg )
	{
		pNewSeg->AddRef ();
		pNewSeg->m_pRAMCounter = &m_iRamChunksAllocatedRAM;
		pNewSeg->UpdateUsedRam();
	}
	return tResult;
}

// CommitReplayable -> ApplyKillList
// AttachDiskIndex -> ApplyKillList
int RtIndex_c::ApplyKillList ( const CSphVector<DocID_t> & dAccKlist )
{
	if ( dAccKlist.IsEmpty() )
		return 0;

	assert ( CoCurrentScheduler() == m_tRtChunks.SerialChunkAccess() );

	int iKilled = 0;
	auto pChunks = m_tRtChunks.DiskChunks();
	for ( auto& pChunk : *pChunks )
		iKilled += pChunk->CastIdx().KillMulti ( dAccKlist );

	auto pSegs = m_tRtChunks.RamSegs();
	for ( auto& pSeg : *pSegs )
		iKilled += const_cast<RtSegment_t*> ( pSeg.Ptr() )->KillMulti ( dAccKlist );

	return iKilled;
}

inline std::pair<int,int> Find2Minimums ( const VecTraits_T<ConstRtSegmentRefPtf_t>& dSegments )
{
	assert ( dSegments.GetLength() > 1 );
	auto fnLess = [&dSegments] ( int a, int b ) { return dSegments[a]->GetMergeFactor() < dSegments[b]->GetMergeFactor(); };

	int a, b;
	if ( fnLess ( 0, 1 ) )
		a = 0, b = 1;
	else
		a = 1, b = 0;

	for ( int i = 2; i < dSegments.GetLength(); ++i )
		if ( fnLess ( i, a ) )
		{
			b = a;
			a = i;
		} else if ( fnLess ( i, b ) )
			b = i;
	return { a, b };
}

constexpr int MAX_SEGMENTS = 32;
constexpr int MAX_PROGRESSION_SEGMENT = 8;
constexpr int64_t MAX_SEGMENT_VECTOR_LEN = INT_MAX;

enum class CheckMerge_e { MERGE, NOMERGE, FLUSH };
CheckMerge_e CheckWeCanMerge ( std::pair<int, int>& tSmallest, const VecTraits_T<ConstRtSegmentRefPtf_t>& dSegments, int64_t iRamLimit ) NO_THREAD_SAFETY_ANALYSIS
{
	const int iSegs = dSegments.GetLength ();
	RTLOGV << "CheckWeCanMerge(" << dSegments.GetLength() << " segs, ram limit " << iRamLimit << " bytes)";

	// enforce RAM usage limit
	auto iRamLeft = iRamLimit - SegmentsGetUsedRam ( dSegments );

	// skip merging if no memory left
	if ( iRamLeft<=0 )
		return CheckMerge_e::FLUSH;

	// if N of segments is not so big - no merge need
	if ( iSegs < ( MAX_SEGMENTS - MAX_PROGRESSION_SEGMENT ) )
		return CheckMerge_e::NOMERGE;

	// take 2 smallest segments
	assert ( iSegs > 1 );
	tSmallest = Find2Minimums ( dSegments );
	ConstRtSegmentRefPtf_t & pA = dSegments[tSmallest.first];
	ConstRtSegmentRefPtf_t & pB = dSegments[tSmallest.second];

	// exit if progression is kept AND lesser MAX_SEGMENTS limit
	if ( pB->GetMergeFactor() > pA->GetMergeFactor() * 2 && iSegs<MAX_SEGMENTS )
		return CheckMerge_e::NOMERGE;

	int64_t iAlive = pA->m_tAliveRows.load ( std::memory_order_relaxed ) + pB->m_tAliveRows.load ( std::memory_order_relaxed );
	DWORD uRows = pA->m_uRows + pB->m_uRows;

	int64_t iEstimatedMergedSize=0;
	int64_t iMaxFutureVecLen=0;

	// check whether we have enough RAM
#define ESTIMATE( _v ) pA->_v.Relimit ( 0, ( (int64_t)pA->_v.GetLength() + pB->_v.GetLength() ) * iAlive / uRows )
#define LOC_ESTIMATE( _v ) do { auto _t=ESTIMATE(_v); iEstimatedMergedSize+=_t; if ( iMaxFutureVecLen<_t) iMaxFutureVecLen=_t; } while (0)

	LOC_ESTIMATE ( m_dWords );
	LOC_ESTIMATE ( m_dDocs );
	LOC_ESTIMATE ( m_dHits );
	LOC_ESTIMATE ( m_dBlobs );
	LOC_ESTIMATE ( m_dKeywordCheckpoints );
	LOC_ESTIMATE ( m_dRows );

#undef LOC_ESTIMATE
#undef ESTIMATE

	if ( iEstimatedMergedSize > iRamLeft ) // can't merge anymore, dump if segments count limit's reached
		return ( iSegs >= MAX_SEGMENTS ) ? CheckMerge_e::FLUSH : CheckMerge_e::NOMERGE;
	return ( iMaxFutureVecLen > MAX_SEGMENT_VECTOR_LEN ) ? CheckMerge_e::FLUSH : CheckMerge_e::MERGE;
}

static StringBuilder_c & operator<< ( StringBuilder_c & dOut, CheckMerge_e eVal )
{
	switch ( eVal )
	{
	case CheckMerge_e::MERGE: return dOut << "MERGE";
	case CheckMerge_e::NOMERGE: return dOut << "NOMERGE";
	case CheckMerge_e::FLUSH: return dOut << "FLUSH";
	default: dOut.Sprintf ( "UNKNWN(%d)", (int)eVal );
	}
	return dOut;
}

void RtIndex_c::StartMergeSegments ( bool bHasNewSegment )
{
	// protect from flood of calls
	if ( m_bSegMergeQueued )
		return;

	m_bSegMergeQueued = true;

	RTLOGV << "StartMergeSegments(" << (bHasNewSegment?"with new":"no new") << ")";

	// all the rest is postponed and will be executed aside of caller.
	CoGo ( [this, bHasNewSegment]
	{
		Threads::CheckRole_c _ ( m_tRtChunks.SerialChunkAccess () );

		// collect all RAM segments not occupied by any op (only ops we know is 'merge segments' and 'save ram chunk')
		LazyVector_T<ConstRtSegmentRefPtf_t> dSegments;
		for ( const auto & dSeg : *m_tRtChunks.RamSegs () )
			if ( !dSeg->m_iLocked )
				dSegments.Add ( dSeg );

		// fixme! Now is old naive way used: if no active save, limit to SoftRamLimit,
		// if saving op is in progress - use limit 10% of SoftRamLimit (aka doubleBufferLimit).
		// todo: think about and m.b. implement another limits.

		std::pair<int,int> tSmallest;
		CheckMerge_e eMergeAction { CheckMerge_e::NOMERGE };
		if ( bHasNewSegment )
			eMergeAction = CheckWeCanMerge ( tSmallest, dSegments, m_tRtChunks.IsSaveActive() ? m_iDoubleBufferLimit : m_iSoftRamLimit );

		RTLOGV << "CheckWeCanMerge returned " << eMergeAction;

		if ( eMergeAction == CheckMerge_e::FLUSH )
		{
			m_bSegMergeQueued = false;
			// here it might be no race, as we're in serial worker.
			SaveDiskChunk ( false );
			return;
		}

		int iMergeOp = 0;

		RtSegmentRefPtf_t pMerged { nullptr };

		if ( eMergeAction == CheckMerge_e::MERGE )
		{
			assert ( dSegments.GetLength ()>=2 );

			ConstRtSegmentRefPtf_t pA = dSegments[tSmallest.first];
			ConstRtSegmentRefPtf_t pB = dSegments[tSmallest.second];
			iMergeOp = m_tRtChunks.GetNextOpTicket();
			pA->m_iLocked = pB->m_iLocked = iMergeOp; // mark them as retiring.

			// that will collect killed docIDs
			KillAccum_t dKillOnMerge;
			pA->SetKillHook ( &dKillOnMerge );
			pB->SetKillHook ( &dKillOnMerge );

			// we leave serial fiber, so kills may come there. They will be collected in dKillOnMerge.
			{
				ScopedScheduler_c _ { m_tRtChunks.MergeSegmentsWorker () };
				pMerged = MergeTwoSegments ( pA, pB );
			}

			pA->SetKillHook ( pMerged );
			pB->SetKillHook ( pMerged );
			RTLOGV << "after merge " << pMerged << ", has " << dKillOnMerge.m_dDocids.GetLength () << " killed";

			// apply postponed kills and updates (if any)
			if ( pMerged )
			{
				pMerged->KillMulti ( dKillOnMerge.m_dDocids );
				if ( pMerged->m_tAliveRows.load ( std::memory_order_relaxed ) )
				{
					// some updates might be applied to pA and pB during the merge. Now it is time to apply them also
					// to the merged segment.
					LazyVector_T<ConstRtSegmentRefPtf_t> dOld;
					dOld.Add ( pA );
					dOld.Add ( pB );
					auto dUpdates = GatherUpdates::FromChunksOrSegments ( dOld );
					UpdateAttributesOffline ( dUpdates, pMerged );
				}
			}
		}

		m_bSegMergeQueued = false;

		// merged might be killed during merge op
		// as we run in serial fiber, there is no concurrency, and so, killing hooks of retired sources will not fire.
		assert ( CoCurrentScheduler() == m_tRtChunks.SerialChunkAccess() );

		if ( pMerged && !pMerged->m_tAliveRows.load ( std::memory_order_relaxed ) )
			pMerged = nullptr;

		// we collect after merge, as some data might be changed during the merge
		for ( auto& pSeg : dSegments )
			if ( !pSeg->m_tAliveRows.load ( std::memory_order_relaxed ) )
			{
				if ( !iMergeOp )
					iMergeOp = m_tRtChunks.GetNextOpTicket();
				pSeg->m_iLocked = iMergeOp;
			}
		dSegments.Reset ();

		// nothing merged, and also nothing killed - nothing to do, exit.
		if ( !pMerged && !iMergeOp )
			return;

		RtWriter_c tNewSet ( m_tRtChunks );
		tNewSet.InitRamSegs ( RtWriter_c::empty );
		for ( const auto & pSeg : *m_tRtChunks.RamSegs() )
			if ( pSeg->m_iLocked!=iMergeOp )
				tNewSet.m_pNewRamSegs->Add ( pSeg );

		if ( pMerged )
			tNewSet.m_pNewRamSegs->Add ( AdoptSegment ( pMerged ) );

		RTRLOG << "after merge " << tNewSet.m_pNewRamSegs->GetLength () << " segments on-board";

		// chain next step
		StartMergeSegments ( true );
	}, m_tRtChunks.SerialChunkAccess () );
}

int RtIndex_c::CommitReplayable ( RtSegment_t * pNewSeg, const CSphVector<DocID_t> & dAccKlist ) REQUIRES_SHARED ( pNewSeg->m_tLock )
{
	// store statistics, because pNewSeg just might get merged
	int iNewDocs = pNewSeg ? (int)pNewSeg->m_uRows : 0;

	// helpers for SPH_ATTR_TOKENCOUNT attributes
	CSphVector<int64_t> dLens;
	int iFirstFieldLenAttr = m_tSchema.GetAttrId_FirstFieldLen();
	if ( pNewSeg && iFirstFieldLenAttr>=0 )
	{
		assert ( pNewSeg->GetStride()==m_iStride );
		int iFields = m_tSchema.GetFieldsCount(); // shortcut
		dLens.Resize ( iFields );
		dLens.Fill ( 0 );
		for ( DWORD i=0; i<pNewSeg->m_uRows; ++i )
			for ( int j=0; j<iFields; ++j )
				dLens[j] += sphGetRowAttr ( pNewSeg->GetDocinfoByRowID(i), m_tSchema.GetAttr ( j+iFirstFieldLenAttr ).m_tLocator );
	}

	// We're going to modify segments, so fall into serial fiber. From here no concurrent changes may happen
	ScopedScheduler_c tSerialFiber { m_tRtChunks.SerialChunkAccess() };

	RTLOGV << "CommitReplayable";

	// first of all, binlog txn data for recovery
	BinlogCommit ( pNewSeg, dAccKlist );

	// 1. Apply kill-list to existing chunks/segments
	int iTotalKilled = ApplyKillList ( dAccKlist );

	// 2. Add new RAM-segment (if any). As we 1-st kill, then add - whole change is *not* atomic, ACID is broken here.
	if ( pNewSeg )
	{
		RtWriter_c tNewState ( m_tRtChunks );
		tNewState.InitRamSegs ( RtWriter_c::copy );
		tNewState.m_pNewRamSegs->Add ( AdoptSegment ( pNewSeg ) );
	}

	// update stats
	m_tStats.m_iTotalDocuments += iNewDocs - iTotalKilled;

	if ( dLens.GetLength() )
		for ( int i = 0; i < m_tSchema.GetFieldsCount(); ++i )
		{
			m_dFieldLensRam[i] += dLens[i];
			m_dFieldLens[i] = m_dFieldLensRam[i] + m_dFieldLensDisk[i];
		}

	// backoff segments merging and m.b. saving disk chunk (that is not our deal, other worker will do it).
	StartMergeSegments ( pNewSeg != nullptr );

	// tell about DELETE affected_rows
	return iTotalKilled;
}

void RtIndex_c::RollBack ( RtAccum_t * pAccExt )
{
	assert ( g_bRTChangesAllowed );

	auto pAcc = ( RtAccum_t * ) AcquireAccum ( m_pDict, pAccExt, m_bKeywordDict );
	if ( !pAcc )
		return;

	pAcc->Cleanup ();
}

bool RtIndex_c::DeleteDocument ( const VecTraits_T<DocID_t> & dDocs, CSphString & sError, RtAccum_t * pAccExt )
{
	assert ( g_bRTChangesAllowed );
	MEMORY ( MEM_RT_ACCUM );

	auto pAcc = ( RtAccum_t * ) AcquireAccum ( m_pDict, pAccExt, m_bKeywordDict, true, &sError );
	if ( !pAcc )
		return false;

	if ( dDocs.IsEmpty() )
		return true;

	// !COMMIT should handle case when uDoc what inserted in current txn here
	pAcc->m_dAccumKlist.Append ( dDocs );

	return true;
}

//////////////////////////////////////////////////////////////////////////
// LOAD/SAVE
//////////////////////////////////////////////////////////////////////////

struct Checkpoint_t
{
	uint64_t m_uWord;
	uint64_t m_uOffset;
};


bool RtIndex_c::ForceDiskChunk()
{
	MEMORY ( MEM_INDEX_RT );

	ScopedScheduler_c tSerialFiber ( m_tRtChunks.SerialChunkAccess() );
	return SaveDiskChunk ( true );
}


struct SaveDiskDataContext_t : public BuildHeader_t
{
	SphOffset_t						m_tDocsOffset {0};
	SphOffset_t						m_tLastDocPos {0};
	CSphScopedPtr<ISphInfixBuilder>	m_pInfixer {nullptr};
	CSphVector<Checkpoint_t>		m_dCheckpoints;
	CSphVector<BYTE>				m_dKeywordCheckpoints;
	CSphVector<CSphVector<RowID_t>>	m_dRowMaps;
	const char *					m_szFilename;
	const ConstRtSegmentSlice_t&	m_tRamSegments;

	SaveDiskDataContext_t ( const char * szFilename, const ConstRtSegmentSlice_t& tSegs )
		: m_szFilename ( szFilename )
		, m_tRamSegments ( tSegs )
	{
		m_dRowMaps.Reserve ( m_tRamSegments.GetLength() );
		for ( const auto & pSeg : m_tRamSegments )
		{
			auto& dRowMap = m_dRowMaps.Add();
			dRowMap.Reserve ( pSeg->m_uRows );
			dRowMap.Resize ( pSeg->m_uRows ); // need space for ALL rows, incl. dead (they will be INVALID_ROWID)
			dRowMap.Fill ( INVALID_ROWID );
		}
		assert ( m_dRowMaps.GetLength() == m_tRamSegments.GetLength() );
	}
};


bool RtIndex_c::WriteAttributes ( SaveDiskDataContext_t & tCtx, CSphString & sError ) const
{
	CSphString sSPA, sSPB, sSPT, sSPHI, sSPDS, sSPC;
	CSphWriter tWriterSPA;

	sSPA.SetSprintf ( "%s%s",	tCtx.m_szFilename, sphGetExt(SPH_EXT_SPA).cstr() );
	sSPB.SetSprintf ( "%s%s",	tCtx.m_szFilename, sphGetExt(SPH_EXT_SPB).cstr() );
	sSPT.SetSprintf ( "%s%s",	tCtx.m_szFilename, sphGetExt(SPH_EXT_SPT).cstr() );
	sSPHI.SetSprintf ( "%s%s",	tCtx.m_szFilename, sphGetExt(SPH_EXT_SPHI).cstr() );
	sSPDS.SetSprintf ( "%s%s",	tCtx.m_szFilename, sphGetExt(SPH_EXT_SPDS).cstr() );
	sSPC.SetSprintf ( "%s%s",	tCtx.m_szFilename, sphGetExt(SPH_EXT_SPC).cstr() );

	if ( !tWriterSPA.OpenFile ( sSPA.cstr(), sError ) )
		return false;

	const CSphColumnInfo * pBlobLocatorAttr = m_tSchema.GetAttr ( sphGetBlobLocatorName() );
	AttrIndexBuilder_c tMinMaxBuilder(m_tSchema);

	CSphScopedPtr<BlobRowBuilder_i> pBlobRowBuilder(nullptr);
	if ( pBlobLocatorAttr )
	{
		pBlobRowBuilder = sphCreateBlobRowBuilder ( m_tSchema, sSPB, m_tSettings.m_tBlobUpdateSpace, sError );
		if ( !pBlobRowBuilder )
			return false;
	}

	CSphScopedPtr<DocstoreBuilder_i> pDocstoreBuilder(nullptr);
	if ( m_tSchema.HasStoredFields() )
	{
		pDocstoreBuilder = CreateDocstoreBuilder ( sSPDS, m_tSettings, sError );
		if ( !pDocstoreBuilder )
			return false;

		SetupDocstoreFields ( *pDocstoreBuilder, m_tSchema );
	}

	CSphScopedPtr<columnar::Builder_i> pColumnarBuilder(nullptr);
	if ( m_tSchema.HasColumnarAttrs() )
	{
		pColumnarBuilder = CreateColumnarBuilder ( m_tSchema, m_tSettings, sSPC, sError );
		if ( !pColumnarBuilder.Ptr() )
			return false;
	}

	HistogramContainer_c tHistograms;
	CSphVector<PlainOrColumnar_t> dAttrsForHistogram;
	CreateHistograms ( tHistograms, dAttrsForHistogram, m_tSchema );

	tCtx.m_iTotalDocuments = 0;
	for ( const auto & i : tCtx.m_tRamSegments )
		tCtx.m_iTotalDocuments += i->m_tAliveRows.load ( std::memory_order_relaxed );

	CSphFixedVector<DocidRowidPair_t> dLookup ( tCtx.m_iTotalDocuments );

	int iColumnarIdLoc = -1;
	if ( m_tSchema.GetAttr(0).IsColumnar() )
		iColumnarIdLoc = 0;

	CSphVector<int64_t> dTmp;
	RowID_t tNextRowID = 0;
	int iStride = m_tSchema.GetRowSize();
	auto iStrideBytes = sizeof ( CSphRowitem ) * iStride;
	CSphFixedVector<CSphRowitem> dNewRow { iStride };
	CSphRowitem * pNewRow = dNewRow.Begin();
	ARRAY_FOREACH ( i, tCtx.m_tRamSegments )
	{
		const auto & tSeg = *tCtx.m_tRamSegments[i];

		SccRL_t rLock ( tSeg.m_tLock );
		tSeg.m_bAttrsBusy.store ( true, std::memory_order_release );

		auto dColumnarIterators = CreateAllColumnarIterators ( tSeg.m_pColumnar.Ptr(), m_tSchema );

		for ( auto tRowID : RtLiveRows_c(tSeg) )
		{
			const CSphRowitem * pRow = tSeg.m_dRows.Begin() + (int64_t)tRowID*iStride;
			tMinMaxBuilder.Collect(pRow);
			if ( pBlobLocatorAttr )
			{
				auto tSrcOffset = sphGetRowAttr ( pRow, pBlobLocatorAttr->m_tLocator );
				auto tTargetOffset = pBlobRowBuilder->Flush ( tSeg.m_dBlobs.Begin() + tSrcOffset );

				memcpy ( pNewRow, pRow, iStrideBytes );
				sphSetRowAttr ( pNewRow, pBlobLocatorAttr->m_tLocator, tTargetOffset );
				tWriterSPA.PutBytes ( pNewRow, (int64_t)iStrideBytes );
			} else
				tWriterSPA.PutBytes ( pRow, (int64_t)iStrideBytes );

			DocID_t tDocID;

			ARRAY_FOREACH ( iIterator, dColumnarIterators )
			{
				auto & tIterator = dColumnarIterators[iIterator];
				Verify ( AdvanceIterator ( tIterator.first.get(), tRowID ) );
				SetColumnarAttr ( iIterator, tIterator.second, pColumnarBuilder.Ptr(), tIterator.first.get(), dTmp );
			}

			tDocID = iColumnarIdLoc>=0 ? dColumnarIterators[iColumnarIdLoc].first->Get() : sphGetDocID(pRow);

			ARRAY_FOREACH ( iHistogram, dAttrsForHistogram )
				tHistograms.Insert ( iHistogram, dAttrsForHistogram[iHistogram].Get ( pRow, dColumnarIterators ) );

			dLookup[tNextRowID] = { tDocID, tNextRowID };
			if ( pDocstoreBuilder )
			{
				assert ( tSeg.m_pDocstore );
				pDocstoreBuilder->AddDoc ( tNextRowID, tSeg.m_pDocstore->GetDoc ( tRowID, nullptr, -1, false ) );
			}

			tCtx.m_dRowMaps[i][tRowID] = tNextRowID++;
		}
	}

	std::string sErrorSTL;
	if ( pColumnarBuilder.Ptr() && !pColumnarBuilder->Done(sErrorSTL) )
	{
		sError = sErrorSTL.c_str();
		return false;
	}

	if ( pBlobRowBuilder && !pBlobRowBuilder->Done ( sError ) )
		return false;

	if ( pDocstoreBuilder )
		pDocstoreBuilder->Finalize();

	dLookup.Sort ( bind ( &DocidRowidPair_t::m_tDocID ) );

	if ( !WriteDocidLookup ( sSPT, dLookup, sError ) )
		return false;

	dLookup.Reset(0);

	if ( !tHistograms.Save ( sSPHI, sError ) )
		return false;

	tMinMaxBuilder.FinishCollect();

	tCtx.m_iDocinfo = tNextRowID;
	if ( tCtx.m_iDocinfo && m_tSchema.HasNonColumnarAttrs() )
	{
		const CSphTightVector<CSphRowitem> & dMinMaxRows = tMinMaxBuilder.GetCollected();

		tCtx.m_iMinMaxIndex = tWriterSPA.GetPos () / sizeof ( CSphRowitem );
		tCtx.m_iDocinfoIndex = ( dMinMaxRows.GetLength() / m_tSchema.GetRowSize() / 2 ) - 1;
		tWriterSPA.PutBytes ( dMinMaxRows.Begin(), dMinMaxRows.GetLength()*sizeof(CSphRowitem) );
	}

	tWriterSPA.CloseFile();
	return !tWriterSPA.IsError();
}


bool RtIndex_c::WriteDocs ( SaveDiskDataContext_t & tCtx, CSphWriter & tWriterDict, CSphString & sError ) const
{
	CSphWriter tWriterHits, tWriterDocs, tWriterSkips;

	CSphString sName;
	sName.SetSprintf ( "%s%s", tCtx.m_szFilename, sphGetExt(SPH_EXT_SPP).cstr() );
	if ( !tWriterHits.OpenFile ( sName.cstr(), sError ) )
		return false;

	sName.SetSprintf ( "%s%s", tCtx.m_szFilename, sphGetExt(SPH_EXT_SPD).cstr() );
	if ( !tWriterDocs.OpenFile ( sName.cstr(), sError ) )
		return false;

	sName.SetSprintf ( "%s%s", tCtx.m_szFilename, sphGetExt(SPH_EXT_SPE).cstr() );
	if ( !tWriterSkips.OpenFile ( sName.cstr(), sError ) )
		return false;

	tWriterHits.PutByte(1);
	tWriterDocs.PutByte(1);
	tWriterSkips.PutByte(1);

	int iSegments = tCtx.m_tRamSegments.GetLength();

	RawVector_T<RtWordReader_c> dWordReaders;
	dWordReaders.Reserve_static ( iSegments );
	CSphVector<const RtWord_t*> dWords(iSegments);
	for ( int i = 0; i < iSegments; ++i )
	{
		dWordReaders.Emplace_back ( tCtx.m_tRamSegments[i], m_bKeywordDict, m_iWordsCheckpoint, m_tSettings.m_eHitless );
		dWords[i] = dWordReaders.Last().UnzipWord();
	}

	// loop keywords
	int iWords = 0;
	CSphKeywordDeltaWriter tLastWord;
	SphWordID_t uLastWordID = 0;
	CSphVector<SkiplistEntry_t> dSkiplist;

	tCtx.m_tLastDocPos = 0;

	bool bHasMorphology = m_pDict->HasMorphology();
	int iSkiplistBlockSize = m_tSettings.m_iSkiplistBlockSize;
	assert ( iSkiplistBlockSize>0 );

	while (true)
	{
		// find keyword with min id
		const RtWord_t * pWord = nullptr;
		for ( auto & i : dWords )
			if ( CompareWords ( i, pWord ) < 0 )
				pWord = i;

		if ( !pWord )
			break;

		SphOffset_t uDocpos = tWriterDocs.GetPos();
		SphOffset_t uLastHitpos = 0;
		RowID_t tLastRowID = INVALID_ROWID;
		RowID_t tSkiplistRowID = INVALID_ROWID;
		int iDocs = 0;
		int iHits = 0;
		dSkiplist.Resize(0);

		// loop all segments that have this keyword
		CSphBitvec tSegsWithWord ( iSegments );

		ARRAY_FOREACH ( iSegment, dWords )
		{
			if ( !CompareWords ( dWords[iSegment], pWord ) )
				tSegsWithWord.BitSet(iSegment);
			else
				continue;

			RtDocReader_c tDocReader ( tCtx.m_tRamSegments[iSegment], *dWords[iSegment] );
			while ( tDocReader.UnzipDoc() )
			{
				const auto* pDoc = (const RtDoc_t*)tDocReader;
				RowID_t tRowID = tCtx.m_dRowMaps[iSegment][pDoc->m_tRowID];
				if ( tRowID==INVALID_ROWID )
					continue;

				// build skiplist, aka save decoder state as needed
				if ( ( iDocs & ( iSkiplistBlockSize-1 ) )==0 )
				{
					SkiplistEntry_t & t = dSkiplist.Add();
					t.m_tBaseRowIDPlus1 = tSkiplistRowID+1;
					t.m_iOffset = tWriterDocs.GetPos();
					t.m_iBaseHitlistPos = uLastHitpos;
				}

				++iDocs;
				iHits += pDoc->m_uHits;
				tSkiplistRowID = tRowID;

				tWriterDocs.ZipOffset ( tRowID - std::exchange ( tLastRowID, tRowID ) );
				tWriterDocs.ZipInt ( pDoc->m_uHits );
				if ( pDoc->m_uHits==1 )
				{
					tWriterDocs.ZipInt ( pDoc->m_uHit & 0x7FFFFFUL );
					tWriterDocs.ZipInt ( pDoc->m_uHit >> 23 );
				} else
				{
					tWriterDocs.ZipInt ( pDoc->m_uDocFields );
					tWriterDocs.ZipOffset ( tWriterHits.GetPos() - std::exchange ( uLastHitpos, tWriterHits.GetPos() ) );
				}

				// loop hits from current segment
				if ( pDoc->m_uHits>1 )
				{
					DWORD uLastHit = 0;
					RtHitReader_c tInHits ( *tCtx.m_tRamSegments[iSegment], *pDoc );
					while ( DWORD uValue = tInHits.UnzipHit() )
						tWriterHits.ZipInt ( uValue - std::exchange ( uLastHit, uValue ) );
					tWriterHits.ZipInt(0);
				}
			}
		}

		// write skiplist
		int64_t iSkiplistOff = tWriterSkips.GetPos();
		for ( int i=1; i<dSkiplist.GetLength(); ++i )
		{
			const SkiplistEntry_t & tPrev = dSkiplist[i-1];
			const SkiplistEntry_t & tCur = dSkiplist[i];
			assert ( tCur.m_tBaseRowIDPlus1 - tPrev.m_tBaseRowIDPlus1>=(DWORD)iSkiplistBlockSize );
			assert ( tCur.m_iOffset - tPrev.m_iOffset>=4*iSkiplistBlockSize );
			tWriterSkips.ZipInt ( tCur.m_tBaseRowIDPlus1 - tPrev.m_tBaseRowIDPlus1 - iSkiplistBlockSize );
			tWriterSkips.ZipOffset ( tCur.m_iOffset - tPrev.m_iOffset - 4*iSkiplistBlockSize );
			tWriterSkips.ZipOffset ( tCur.m_iBaseHitlistPos - tPrev.m_iBaseHitlistPos );
		}

		// write dict entry if necessary
		if ( tWriterDocs.GetPos()!=uDocpos )
		{
			tWriterDocs.ZipInt ( 0 ); // docs over

			if ( ( iWords%SPH_WORDLIST_CHECKPOINT )==0 )
			{
				if ( iWords )
				{
					SphOffset_t uOff = m_bKeywordDict ? 0 : uDocpos - tCtx.m_tLastDocPos;
					tWriterDict.ZipInt ( 0 );
					tWriterDict.ZipOffset ( uOff ); // store last hitlist length
				}

				// restart delta coding, once per SPH_WORDLIST_CHECKPOINT entries
				tCtx.m_tLastDocPos = 0;
				uLastWordID = 0;
				tLastWord.Reset();

				// begin new wordlist entry
				Checkpoint_t & tChk = tCtx.m_dCheckpoints.Add();
				tChk.m_uOffset = tWriterDict.GetPos();
				if ( m_bKeywordDict )
					tChk.m_uWord = sphPutBytes ( &tCtx.m_dKeywordCheckpoints, pWord->m_sWord, pWord->m_sWord[0]+1 ); // copy word len + word itself to checkpoint storage
				else
					tChk.m_uWord = pWord->m_uWordID;
			}

			++iWords;

			if ( m_bKeywordDict )
			{
				tLastWord.PutDelta ( tWriterDict, pWord->m_sWord+1, pWord->m_sWord[0] );
				tWriterDict.ZipOffset ( uDocpos );
			} else
			{
				assert ( pWord->m_uWordID!=uLastWordID );
				tWriterDict.ZipOffset ( pWord->m_uWordID - uLastWordID );
				uLastWordID = pWord->m_uWordID;
				assert ( uDocpos>tCtx.m_tLastDocPos );
				tWriterDict.ZipOffset ( uDocpos - tCtx.m_tLastDocPos );
			}

			DWORD iDocsCount = iDocs;
			if ( !pWord->m_bHasHitlist && m_tSettings.m_eHitless==SPH_HITLESS_SOME )
				iDocsCount |= HITLESS_DOC_FLAG;

			tWriterDict.ZipInt ( iDocsCount );
			tWriterDict.ZipInt ( iHits );

			if ( m_bKeywordDict )
			{
				BYTE uHint = sphDoclistHintPack ( iDocs, tWriterDocs.GetPos()-tCtx.m_tLastDocPos );
				if ( uHint )
					tWriterDict.PutByte ( uHint );

				// build infixes
				if ( tCtx.m_pInfixer )
					tCtx.m_pInfixer->AddWord ( pWord->m_sWord+1, pWord->m_sWord[0], tCtx.m_dCheckpoints.GetLength(), bHasMorphology );
			}

			// emit skiplist pointer
			if ( iDocs>iSkiplistBlockSize )
				tWriterDict.ZipOffset ( iSkiplistOff );

			tCtx.m_tLastDocPos = uDocpos;
		}

		// read next words
		for ( int i = 0; i < tSegsWithWord.GetBits(); ++i )
			if ( tSegsWithWord.BitGet(i) )
				dWords[i] = dWordReaders[i].UnzipWord();
	}

	tCtx.m_tDocsOffset = tWriterDocs.GetPos();
	return true;
}


void RtIndex_c::WriteCheckpoints ( SaveDiskDataContext_t & tCtx, CSphWriter & tWriterDict ) const
{
	if ( tWriterDict.GetPos()==1 )
	{
		tCtx.m_iDictCheckpointsOffset = 1;
		return;
	}

	// write checkpoints
	SphOffset_t uOff = m_bKeywordDict ? 0 : tCtx.m_tDocsOffset - tCtx.m_tLastDocPos;

	// however plain index becomes m_bIsEmpty and full scan does not work there
	// we'll get partly working RT ( RAM chunk works and disk chunks give empty result set )
	tWriterDict.ZipInt ( 0 ); // indicate checkpoint
	tWriterDict.ZipOffset ( uOff ); // store last doclist length

	// flush infix hash entries, if any
	if ( tCtx.m_pInfixer )
		tCtx.m_pInfixer->SaveEntries ( tWriterDict );

	tCtx.m_iDictCheckpointsOffset = tWriterDict.GetPos();
	if ( m_bKeywordDict )
	{
		const char * pCheckpoints = (const char *)tCtx.m_dKeywordCheckpoints.Begin();
		for ( const auto & i : tCtx.m_dCheckpoints )
		{
			const char * pPacked = pCheckpoints + i.m_uWord;
			int iLen = *pPacked;
			assert ( iLen && (int)i.m_uWord+1+iLen<=tCtx.m_dKeywordCheckpoints.GetLength() );
			tWriterDict.PutDword ( iLen );
			tWriterDict.PutBytes ( pPacked+1, iLen );
			tWriterDict.PutOffset ( i.m_uOffset );
		}
	} else
	{
		for ( const auto & i : tCtx.m_dCheckpoints )
		{
			tWriterDict.PutOffset ( i.m_uWord );
			tWriterDict.PutOffset ( i.m_uOffset );
		}
	}

	// flush infix hash blocks
	if ( tCtx.m_pInfixer )
	{
		tCtx.m_iInfixBlocksOffset = tCtx.m_pInfixer->SaveEntryBlocks ( tWriterDict );
		tCtx.m_iInfixBlocksWordsSize = tCtx.m_pInfixer->GetBlocksWordsSize();

		if ( tCtx.m_iInfixBlocksOffset>UINT_MAX )
			sphWarning ( "INTERNAL ERROR: dictionary size " INT64_FMT " overflow at infix save", tCtx.m_iInfixBlocksOffset );
	}

	// flush header
	// mostly for debugging convenience
	// primary storage is in the index wide header
	tWriterDict.PutBytes ( "dict-header", 11 );
	tWriterDict.ZipInt ( tCtx.m_dCheckpoints.GetLength() );
	tWriterDict.ZipOffset ( tCtx.m_iDictCheckpointsOffset );
	tWriterDict.ZipInt ( m_pTokenizer->GetMaxCodepointLength() );
	tWriterDict.ZipInt ( (DWORD)tCtx.m_iInfixBlocksOffset );
}


bool RtIndex_c::WriteDeadRowMap ( SaveDiskDataContext_t & tCtx, CSphString & sError ) // static
{
	CSphString sName;
	sName.SetSprintf ( "%s%s", tCtx.m_szFilename, sphGetExt(SPH_EXT_SPM).cstr() );

	return ::WriteDeadRowMap ( sName, tCtx.m_iDocinfo, sError );
}

// SaveDiskChunk -> SaveDiskData
// RO save RAM chunks from tSegs into new disk chunk (nothing added/released, just disk files created)
void RtIndex_c::SaveDiskData ( const char * szFilename, const ConstRtSegmentSlice_t& tSegs, const ChunkStats_t & tStats ) const
{
	CSphString sError; // FIXME!!! report collected (sError) errors

	RTDLOG << "SaveDiskData to " << szFilename << ", " << tSegs.GetLength() << " segments";

	SaveDiskDataContext_t tCtx ( szFilename, tSegs ); // only RAM segments here in game.
	if ( m_tSettings.m_iMinInfixLen && m_pDict->GetSettings().m_bWordDict )
		tCtx.m_pInfixer = sphCreateInfixBuilder ( m_pTokenizer->GetMaxCodepointLength(), &sError );

	// fixme: handle errors
	WriteAttributes ( tCtx, sError );
	WriteDeadRowMap ( tCtx, sError );

	CSphWriter tWriterDict;
	CSphString sSPI;
	sSPI.SetSprintf ( "%s%s", szFilename, sphGetExt ( SPH_EXT_SPI ).cstr() );
	tWriterDict.OpenFile ( sSPI.cstr(), sError );
	tWriterDict.PutByte ( 1 );

	WriteDocs ( tCtx, tWriterDict, sError );
	WriteCheckpoints ( tCtx, tWriterDict );
	WriteDeadRowMap ( tCtx, sError );

	SaveDiskHeader ( tCtx, tStats );
}


static void FixupIndexSettings ( CSphIndexSettings & tSettings )
{
	tSettings.m_eHitFormat = SPH_HIT_FORMAT_INLINE;
	tSettings.m_iBoundaryStep = 0;
	tSettings.m_iStopwordStep = 1;
	tSettings.m_iOvershortStep = 1;
}


// SaveDiskChunk -> SaveDiskData -> SaveDiskHeader
void RtIndex_c::SaveDiskHeader ( SaveDiskDataContext_t & tCtx, const ChunkStats_t & tStats ) const
{
	tCtx.m_iDictCheckpoints = tCtx.m_dCheckpoints.GetLength ();
	tCtx.m_iInfixCodepointBytes = ( m_tSettings.m_iMinInfixLen && m_pDict->GetSettings ().m_bWordDict )
			? m_pTokenizer->GetMaxCodepointLength ()
			: 0;

	tCtx.m_iTotalBytes = tStats.m_Stats.m_iTotalBytes;

	CSphIndexSettings tSettings = m_tSettings;
	FixupIndexSettings ( tSettings );

	WriteHeader_t tWriteHeader;
	tWriteHeader.m_pSettings = &tSettings;
	tWriteHeader.m_pSchema = &m_tSchema;
	tWriteHeader.m_pTokenizer = m_pTokenizer;
	tWriteHeader.m_pDict = m_pDict;
	tWriteHeader.m_pFieldFilter = m_pFieldFilter;
	tWriteHeader.m_pFieldLens = m_dFieldLens.Begin();

	CSphWriter tWriter;
	CSphString sName, sError;
	sName.SetSprintf ( "%s%s", tCtx.m_szFilename, sphGetExt ( SPH_EXT_SPH ).cstr () );
	if ( !tWriter.OpenFile ( sName.cstr (), sError ) )
		return; // fixme! report error...

	IndexWriteHeader ( tCtx, tWriteHeader, tWriter, m_bKeywordDict, true );
}

void RtIndex_c::SaveMeta ( int64_t iTID, VecTraits_T<int> dChunkNames )
{
	if ( m_eSaving.load ( std::memory_order_relaxed ) != WriteState_e::ENABLED )
		return;

	// sanity check
	if ( m_iLockFD<0 )
		return;

	// write new meta
	CSphString sMeta, sMetaNew;
	sMeta.SetSprintf ( "%s.meta", m_sPath.cstr() );
	sMetaNew.SetSprintf ( "%s.meta.new", m_sPath.cstr() );

	CSphString sError;
	CSphWriter wrMeta;
	if ( !wrMeta.OpenFile ( sMetaNew, sError ) )
		sphDie ( "failed to serialize meta: %s", sError.cstr() ); // !COMMIT handle this gracefully
	wrMeta.PutDword ( META_HEADER_MAGIC );
	wrMeta.PutDword ( META_VERSION );
	wrMeta.PutDword ( (DWORD)m_tStats.m_iTotalDocuments ); // FIXME? we don't expect over 4G docs per just 1 local index
	wrMeta.PutOffset ( m_tStats.m_iTotalBytes ); // FIXME? need PutQword ideally
	wrMeta.PutOffset ( iTID );

	// meta v.4, save disk index format and settings, too
	wrMeta.PutDword ( INDEX_FORMAT_VERSION );
	WriteSchema ( wrMeta, m_tSchema );
	SaveIndexSettings ( wrMeta, m_tSettings );
	SaveTokenizerSettings ( wrMeta, m_pTokenizer, m_tSettings.m_iEmbeddedLimit );
	SaveDictionarySettings ( wrMeta, m_pDict, m_bKeywordDict, m_tSettings.m_iEmbeddedLimit );

	// meta v.5
	wrMeta.PutDword ( m_iWordsCheckpoint );

	// meta v.7
	wrMeta.PutDword ( m_iMaxCodepointLength );
	wrMeta.PutByte ( BLOOM_PER_ENTRY_VALS_COUNT );
	wrMeta.PutByte ( BLOOM_HASHES_COUNT );

	// meta v.11
	CSphFieldFilterSettings tFieldFilterSettings;
	if ( m_pFieldFilter.Ptr() )
		m_pFieldFilter->GetSettings(tFieldFilterSettings);
	tFieldFilterSettings.Save(wrMeta);

	// meta v.12
	wrMeta.PutDword ( dChunkNames.GetLength () );
	wrMeta.PutBytes ( dChunkNames.Begin(), dChunkNames.GetLengthBytes() );

	// meta v.17+
	wrMeta.PutOffset(m_iSoftRamLimit);

	wrMeta.CloseFile();

	// no need to remove old but good meta in case new meta failed to save
	if ( wrMeta.IsError() )
	{
		sphWarning ( "%s", sError.cstr() );
		return;
	}

	// rename
	if ( sph::rename ( sMetaNew.cstr(), sMeta.cstr() ) )
		sphDie ( "failed to rename meta (src=%s, dst=%s, errno=%d, error=%s)",
			sMetaNew.cstr(), sMeta.cstr(), errno, strerrorm(errno) ); // !COMMIT handle this gracefully

	SaveMutableSettings ( m_tMutableSettings, m_sPath );
}

void RtIndex_c::SaveMeta()
{
	SaveMeta ( m_iTID, GetChunkIds ( *m_tRtChunks.DiskChunks() ) );
}

// looks like spinlock, but actually we switch to parallel strand and back on every tick, so it should not burn CPU
void RtIndex_c::WaitRAMSegmentsUnlocked () const
{
	while ( m_tRtChunks.RamSegs ()->any_of ( [] ( ConstRtSegmentRefPtf_t & a ) { return a->m_iLocked; } ) )
		Threads::ScopedScheduler_c ( m_tRtChunks.MergeSegmentsWorker () );
}

// i.e. create new disk chunk from ram segments
bool RtIndex_c::SaveDiskChunk ( bool bForced )
{
	if ( m_eSaving.load(std::memory_order_relaxed) != WriteState_e::ENABLED ) // fixme! review, m.b. refactor
		return true;

	assert ( CoCurrentScheduler() == m_tRtChunks.SerialChunkAccess () );

	RTDLOG << "SaveDiskChunk( " << ( bForced ? "forced" : "not forced" ) << ")";

	int iSaveOp = m_tRtChunks.GetNextOpTicket();

	// if forced - wait all segments. Otherwise, can continue with subset of currently available segments
	// note that segments may be locked by currently executing SaveDiskChunk. If so, we wait them finished and continue.
	// that will cause another disk chunk written right after just finished, since op is forced it is ok.
	if ( bForced )
		WaitRAMSegmentsUnlocked ();

	// collect all non-occupied non-empty segments and lock them
	KillAccum_t dKillOnSave;
	LazyVector_T<ConstRtSegmentRefPtf_t> dSegments;
	for ( const auto & pSeg : *m_tRtChunks.RamSegs () )
		if ( !pSeg->m_iLocked )
		{
			pSeg->m_iLocked = iSaveOp;
			if ( pSeg->m_tAliveRows.load ( std::memory_order_relaxed )!=0 )
			{
				pSeg->SetKillHook ( &dKillOnSave );
				dSegments.Add ( pSeg );
			}
		}

	RTDLOG << "SaveDiskChunk process " << dSegments.GetLength() << " segments.";
	if ( dSegments.IsEmpty() )
		return true;

	m_tRtChunks.SetSaveActive ( true );
	auto tFinallySetSaveUnactive = AtScopeExit ( [this] { m_tRtChunks.SetSaveActive ( false ); } );

	int64_t tmSaveWall = -sphMicroTimer(); // all time including waiting
	int64_t tmSave; // only active time
	MEMORY ( MEM_INDEX_RT );

	int iChunkID = m_tRtChunks.MakeChunkId();
	auto sChunk = MakeChunkName ( iChunkID );

	// as we're going to switch fiber, we need to freeze reliable stat and m_iTID (as they could change)
	ChunkStats_t tStats ( m_tStats, m_dFieldLensRam );
	auto iTID = m_iTID;
	CSphIndex * pNewChunk = nullptr;
	{
		// as separate subtask we 1-st flush segments to disk, and then load just flushed segment
		// if forced, continue to work in the same fiber; otherwise split to merge fiber
		ScopedScheduler_c tMergeFiber { bForced ? CoCurrentScheduler () : m_tRtChunks.MergeSegmentsWorker() };
		tmSave = -sphMicroTimer();
		SaveDiskData ( sChunk.cstr (), dSegments, tStats );
		// fixme! process errors.

		// bring new disk chunk online
		auto fnFnameBuilder = GetIndexFilenameBuilder ();
		StrVec_t dWarnings; // fixme!
		CSphScopedPtr<FilenameBuilder_i> pFilenameBuilder { fnFnameBuilder ? fnFnameBuilder ( m_sIndexName.cstr () ) : nullptr };
		pNewChunk = PreallocDiskChunk ( sChunk.cstr (), iChunkID, pFilenameBuilder.Ptr (), dWarnings, m_sLastError );
		tmSave += sphMicroTimer();
	}

	// here we back into serial fiber. As we're switched, we can't rely on m_iTID and index stats anymore
	if ( !pNewChunk )
	{
		sphWarning ( "rt: index %s failed to load disk chunk after RAM save: %s", m_sIndexName.cstr (), m_sLastError.cstr () );
		return false;
	}

	// applying postponed kills is ok now, since no other kills would happen as we're in serial fiber.
	if ( !dKillOnSave.m_dDocids.IsEmpty() )
	{
		RTLOGV << "SaveDiskChunk: apply postponed kills";
		tmSave -= sphMicroTimer ();
		dKillOnSave.m_dDocids.Uniq ();
		pNewChunk->KillMulti ( dKillOnSave.m_dDocids );
		dKillOnSave.m_dDocids.Reset();
		tmSave += sphMicroTimer ();
	}

	auto dUpdates = GatherUpdates::FromChunksOrSegments ( dSegments );
	if ( !dUpdates.IsEmpty () )
	{
		RTLOGV << "SaveDiskChunk: apply postponed updates";
		pNewChunk->UpdateAttributesOffline ( dUpdates, pNewChunk );
		dUpdates.Reset();
	}

	int iSegments = dSegments.GetLength ();
	dSegments.Reset (); // we don't need them anymore

	// update field lengths (offline, will swap under lock)
	CSphFixedVector<int64_t> dNewFieldLensRam { 0 };
	CSphFixedVector<int64_t> dNewFieldLensDisk { 0 };
	if ( m_tSchema.GetAttrId_FirstFieldLen ()>=0 )
	{
		dNewFieldLensRam.Reset ( SPH_MAX_FIELDS );
		dNewFieldLensDisk.Reset ( SPH_MAX_FIELDS );
		ARRAY_FOREACH ( i, tStats.m_dFieldLens )
		{
			dNewFieldLensRam[i] = m_dFieldLensRam[i] - tStats.m_dFieldLens[i];
			dNewFieldLensDisk[i] = m_dFieldLensDisk[i] + tStats.m_dFieldLens[i];
		}
	}

	int iDiskChunks;
	// now new disk chunk is loaded, kills and updates applied - we ready to change global index state now.
	{
		RtWriter_c tNewSet ( m_tRtChunks );
		tNewSet.InitDiskChunks ( RtWriter_c::copy );
		tNewSet.m_pNewDiskChunks->Add ( DiskChunk_c::make ( pNewChunk ) );
		SaveMeta ( iTID, GetChunkIds ( *tNewSet.m_pNewDiskChunks ) );

		Binlog::NotifyIndexFlush ( m_sIndexName.cstr(), iTID, false );
		m_iSavedTID = iTID;

		tNewSet.InitRamSegs ( RtWriter_c::empty );

		for ( const auto & pSeg : *m_tRtChunks.RamSegs() )
			if ( pSeg->m_iLocked!=iSaveOp )
				tNewSet.m_pNewRamSegs->Add ( pSeg );

		// update field lengths
		if ( m_tSchema.GetAttrId_FirstFieldLen ()>=0 )
		{
			m_dFieldLensRam.SwapData ( dNewFieldLensRam );
			m_dFieldLensDisk.SwapData ( dNewFieldLensDisk );
		}
		iDiskChunks = tNewSet.m_pNewDiskChunks->GetLength();
	}
	// from this point all readers will see new state of the index.

	// abandon .ram file
	UnlinkRAMChunk ( "SaveDiskChunk" );

	m_tmSaved = sphMicroTimer ();
	tmSaveWall += m_tmSaved;

	StringBuilder_c sInfo;
	sInfo.Sprintf ( "rt: index %s: diskchunk %d(%d), segments %d %s saved in %.6D (%.6D) sec", m_sIndexName.cstr (), iChunkID
					, iDiskChunks, iSegments, bForced ? "forcibly" : "", tmSave, tmSaveWall );
	sphInfo ( "%s", sInfo.cstr() );
	Preread();
	CheckStartAutoOptimize();
	return true;
}

CSphIndex * RtIndex_c::PreallocDiskChunk ( const char * sChunk, int iChunk, FilenameBuilder_i * pFilenameBuilder, StrVec_t & dWarnings, CSphString & sError, const char * sName ) const
{
	MEMORY ( MEM_INDEX_DISK );

	// !COMMIT handle errors gracefully instead of dying
	CSphScopedPtr<CSphIndex> pDiskChunk { sphCreateIndexPhrase ( ( sName ? sName : sChunk ), sChunk ) };
	if ( !pDiskChunk )
	{
		sError.SetSprintf ( "disk chunk %s: alloc failed", sChunk );
		return nullptr;
	}

	pDiskChunk->m_iExpansionLimit = m_iExpansionLimit;
	pDiskChunk->SetMutableSettings ( m_tMutableSettings );
	pDiskChunk->SetBinlog ( false );
	pDiskChunk->m_iChunk = iChunk;

	if ( m_bDebugCheck )
		pDiskChunk->SetDebugCheck ( m_bCheckIdDups, -1 );

	if ( !pDiskChunk->Prealloc ( m_bPathStripped, pFilenameBuilder, dWarnings ) )
	{
		sError.SetSprintf ( "disk chunk %s: prealloc failed: %s", sChunk, pDiskChunk->GetLastError().cstr() );
		pDiskChunk = nullptr;
	}

	return pDiskChunk.LeakPtr();
}


bool RtIndex_c::LoadMeta ( FilenameBuilder_i * pFilenameBuilder, bool bStripPath, DWORD & uVersion, bool & bRebuildInfixes, StrVec_t & dWarnings )
{
	// check if we have a meta file (kinda-header)
	CSphString sMeta;
	sMeta.SetSprintf ( "%s.meta", m_sPath.cstr() );

	// no readable meta? no disk part yet
	if ( !sphIsReadable ( sMeta.cstr() ) )
		return true;

	// opened and locked, lets read
	CSphAutoreader rdMeta;
	if ( !rdMeta.Open ( sMeta, m_sLastError ) )
		return false;

	if ( rdMeta.GetDword()!=META_HEADER_MAGIC )
	{
		m_sLastError.SetSprintf ( "invalid meta file %s", sMeta.cstr() );
		return false;
	}

	uVersion = rdMeta.GetDword();
	if ( uVersion==0 || uVersion>META_VERSION )
	{
		m_sLastError.SetSprintf ( "%s is v.%u, binary is v.%u", sMeta.cstr(), uVersion, META_VERSION );
		return false;
	}

	DWORD uMinFormatVer = 14;
	if ( uVersion<uMinFormatVer )
	{
		m_sLastError.SetSprintf ( "indexes with meta prior to v.%u are no longer supported (use index_converter tool); %s is v.%u", uMinFormatVer, sMeta.cstr(), uVersion );
		return false;
	}

	m_tStats.m_iTotalDocuments = rdMeta.GetDword();
	m_tStats.m_iTotalBytes = rdMeta.GetOffset();
	m_iTID = rdMeta.GetOffset();

	// tricky bit
	// we started saving settings into .meta from v.4 and up only
	// and those reuse disk format version, aka INDEX_FORMAT_VERSION
	// anyway, starting v.4, serialized settings take precedence over config
	// so different chunks can't have different settings any more
	CSphTokenizerSettings tTokenizerSettings;
	CSphDictSettings tDictSettings;
	CSphEmbeddedFiles tEmbeddedFiles;

	// load them settings
	DWORD uSettingsVer = rdMeta.GetDword();
	CSphSchema tSchema;
	ReadSchema ( rdMeta, tSchema, uSettingsVer );
	SetSchema ( tSchema );
	LoadIndexSettings ( m_tSettings, rdMeta, uSettingsVer );
	if ( !tTokenizerSettings.Load ( pFilenameBuilder, rdMeta, tEmbeddedFiles, m_sLastError ) )
		return false;

	{
		CSphString sWarning;
		tDictSettings.Load ( rdMeta, tEmbeddedFiles, sWarning );
		if ( !sWarning.IsEmpty() )
			dWarnings.Add(sWarning);
	}

	m_bKeywordDict = tDictSettings.m_bWordDict;

	// initialize AOT if needed
	DWORD uPrevAot = m_tSettings.m_uAotFilterMask;
	m_tSettings.m_uAotFilterMask = sphParseMorphAot ( tDictSettings.m_sMorphology.cstr() );
	if ( m_tSettings.m_uAotFilterMask!=uPrevAot )
	{
		CSphString sWarning;
		sWarning.SetSprintf ( "index '%s': morphology option changed from config has no effect, ignoring", m_sIndexName.cstr() );
		dWarnings.Add(sWarning);
	}

	if ( bStripPath )
	{
		StripPath ( tTokenizerSettings.m_sSynonymsFile );
		ARRAY_FOREACH ( i, tDictSettings.m_dWordforms )
			StripPath ( tDictSettings.m_dWordforms[i] );
	}

	// recreate tokenizer
	m_pTokenizer = ISphTokenizer::Create ( tTokenizerSettings, &tEmbeddedFiles, pFilenameBuilder, dWarnings, m_sLastError );
	if ( !m_pTokenizer )
		return false;

	// recreate dictionary
	m_pDict = sphCreateDictionaryCRC ( tDictSettings, &tEmbeddedFiles, m_pTokenizer, m_sIndexName.cstr(), bStripPath, m_tSettings.m_iSkiplistBlockSize, pFilenameBuilder, m_sLastError );
	if ( !m_sLastError.IsEmpty() )
		m_sLastError.SetSprintf ( "index '%s': %s", m_sIndexName.cstr(), m_sLastError.cstr() );

	if ( !m_pDict )
		return false;

	if ( !m_sLastError.IsEmpty() )
		dWarnings.Add(m_sLastError);

	m_pTokenizer = ISphTokenizer::CreateMultiformFilter ( m_pTokenizer, m_pDict->GetMultiWordforms () );

	m_iWordsCheckpoint = rdMeta.GetDword();

	// check that infixes definition changed - going to rebuild infixes
	m_iMaxCodepointLength = rdMeta.GetDword();
	int iBloomKeyLen = rdMeta.GetByte();
	int iBloomHashesCount = rdMeta.GetByte();
	bRebuildInfixes = ( iBloomKeyLen!=BLOOM_PER_ENTRY_VALS_COUNT || iBloomHashesCount!=BLOOM_HASHES_COUNT );

	if ( bRebuildInfixes )
	{
		CSphString sWarning;
		sWarning.SetSprintf ( "infix definition changed (from len=%d, hashes=%d to len=%d, hashes=%d) - rebuilding...",
			(int)BLOOM_PER_ENTRY_VALS_COUNT, (int)BLOOM_HASHES_COUNT, iBloomKeyLen, iBloomHashesCount );
		dWarnings.Add(sWarning);
	}

	FieldFilterRefPtr_c pFieldFilter;
	CSphFieldFilterSettings tFieldFilterSettings;
	tFieldFilterSettings.Load(rdMeta);
	if ( tFieldFilterSettings.m_dRegexps.GetLength() )
		pFieldFilter = sphCreateRegexpFilter ( tFieldFilterSettings, m_sLastError );

	if ( !sphSpawnFilterICU ( pFieldFilter, m_tSettings, tTokenizerSettings, sMeta.cstr(), m_sLastError ) )
		return false;

	SetFieldFilter ( pFieldFilter );

	int iLen = (int)rdMeta.GetDword();
	m_dChunkNames.Reset ( iLen );
	rdMeta.GetBytes ( m_dChunkNames.Begin(), iLen*sizeof(int) );

	if ( uVersion>=17 )
		m_iSoftRamLimit = rdMeta.GetOffset();

	return true;
}


bool RtIndex_c::PreallocDiskChunks ( FilenameBuilder_i * pFilenameBuilder, StrVec_t & dWarnings ) NO_THREAD_SAFETY_ANALYSIS
{
	// load disk chunks, if any
	RtWriter_c tWriter { m_tRtChunks };
	tWriter.InitDiskChunks ( RtWriter_c::empty );
	ARRAY_FOREACH ( iName, m_dChunkNames )
	{
		int iChunkIndex = m_dChunkNames[iName];
		CSphString sChunk;
		sChunk.SetSprintf ( "%s.%d", m_sPath.cstr(), iChunkIndex );
		auto pChunk = DiskChunk_c::make ( PreallocDiskChunk ( sChunk.cstr(), iChunkIndex, pFilenameBuilder, dWarnings, m_sLastError ) );
		if ( !pChunk )
			return false;

		auto* pIndex = (const CSphIndex*)*pChunk;

		// tricky bit
		// outgoing match schema on disk chunk should be identical to our internal (!) schema
		if ( !m_tSchema.CompareTo ( pIndex->GetMatchSchema(), m_sLastError ) )
			return false;

		tWriter.m_pNewDiskChunks->Add ( pChunk );

		// update field lengths
		if ( m_tSchema.GetAttrId_FirstFieldLen()>=0 )
		{
			int64_t * pLens = pIndex->GetFieldLens();
			if ( pLens )
				for ( int i=0; i < pIndex->GetMatchSchema().GetFieldsCount(); ++i )
					m_dFieldLensDisk[i] += pLens[i];
		}
	}

	m_dChunkNames.Reset(0);
	return true;
}


bool RtIndex_c::Prealloc ( bool bStripPath, FilenameBuilder_i * pFilenameBuilder, StrVec_t & dWarnings )
{
	MEMORY ( MEM_INDEX_RT );

	// locking uber alles
	// in RT backend case, we just must be multi-threaded
	// so we simply lock here, and ignore Lock/Unlock hassle caused by forks
	assert ( m_iLockFD<0 );

	CSphString sLock;
	sLock.SetSprintf ( "%s.lock", m_sPath.cstr() );
	m_iLockFD = ::open ( sLock.cstr(), SPH_O_NEW, 0644 );
	if ( m_iLockFD<0 )
	{
		m_sLastError.SetSprintf ( "failed to open %s: %s", sLock.cstr(), strerrorm(errno) );
		return false;
	}

	if ( !sphLockEx ( m_iLockFD, false ) )
	{
		if ( !m_bDebugCheck )
		{
			m_sLastError.SetSprintf ( "failed to lock %s: %s", sLock.cstr(), strerrorm(errno) );
			SafeClose ( m_iLockFD );
			return false;
		} else
		{
			SafeClose ( m_iLockFD );
		}
	}

	DWORD uVersion = 0;
	bool bRebuildInfixes = false;
	if ( !LoadMeta ( pFilenameBuilder, bStripPath, uVersion, bRebuildInfixes, dWarnings ) )
		return false;

	if ( m_bDebugCheck )
		return true;

	CSphString sMutableFile;
	sMutableFile.SetSprintf ( "%s%s", m_sPath.cstr(), sphGetExt ( SPH_EXT_SETTINGS ).cstr() );
	if ( !m_tMutableSettings.Load ( sMutableFile.cstr(), m_sIndexName.cstr() ) )
		return false;
	SetMemLimit ( m_tMutableSettings.m_iMemLimit );

	m_bPathStripped = bStripPath;

	if ( m_tSchema.HasColumnarAttrs() && !IsColumnarLibLoaded() )
	{
		m_sLastError.SetSprintf ( "failed to load index with columnar attributes without columnar library" );
		return false;
	}

	m_tRtChunks.InitWorkers();
	ScopedScheduler_c tSerialFiber ( m_tRtChunks.SerialChunkAccess() );

	m_bLoadRamPassedOk = false;

	if ( !PreallocDiskChunks ( pFilenameBuilder, dWarnings ) )
		return false;

	// load ram chunk
	m_bLoadRamPassedOk = LoadRamChunk ( uVersion, bRebuildInfixes );

	// field lengths
	ARRAY_FOREACH ( i, m_dFieldLens )
		m_dFieldLens[i] = m_dFieldLensDisk[i] + m_dFieldLensRam[i];

	// set up values for on timer save
	m_iSavedTID = m_iTID;
	m_tmSaved = sphMicroTimer();

	return m_bLoadRamPassedOk;
}


void RtIndex_c::Preread ()
{
	for ( auto& pDiskChunk : *m_tRtChunks.DiskChunks() )
		pDiskChunk->CastIdx().Preread();
}

template<typename P>
static bool CheckVectorLength ( int iLen, int64_t iMinLen, const char * sAt, CSphString & sError )
{
	auto iSaneLen = Min ( iMinLen, P::SANE_SIZE );
	if ( iLen>=0 && iLen<iSaneLen )
		return true;

	sError.SetSprintf ( "broken index, %s length overflow (len=%d, max=" INT64_FMT ")", sAt, iLen, iSaneLen );
	return false;
}

template < typename T >
static void SaveVector ( CSphWriter & tWriter, const VecTraits_T < T > & tVector )
{
	STATIC_ASSERT ( IS_TRIVIALLY_COPYABLE(T), NON_TRIVIAL_VECTORS_ARE_UNSERIALIZABLE );
	tWriter.PutDword ( tVector.GetLength() );
	if ( tVector.GetLength() )
		tWriter.PutBytes ( tVector.Begin(), tVector.GetLengthBytes() );
}


template < typename T, typename P >
static bool LoadVector ( CSphReader & tReader, CSphVector < T, P > & tVector,
	int64_t iMinLen, const char * sAt, CSphString & sError )
{
	STATIC_ASSERT ( IS_TRIVIALLY_COPYABLE(T), NON_TRIVIAL_VECTORS_ARE_UNSERIALIZABLE );
	int iSize = tReader.GetDword();
	if ( !CheckVectorLength<P> ( iSize, iMinLen, sAt, sError ) )
		return false;

	tVector.Resize ( iSize );
	if ( tVector.GetLength() )
		tReader.GetBytes ( tVector.Begin(), (int) tVector.GetLengthBytes() );

	return true;
}

bool RtIndex_c::SaveRamChunk ()
{
	if ( m_eSaving.load ( std::memory_order_relaxed ) != WriteState_e::ENABLED )
		return true;

	MEMORY ( MEM_INDEX_RT );

	CSphString sChunk, sNewChunk;
	sChunk.SetSprintf ( "%s.ram", m_sPath.cstr() );
	sNewChunk.SetSprintf ( "%s.ram.new", m_sPath.cstr() );

	CSphWriter wrChunk;
	if ( !wrChunk.OpenFile ( sNewChunk, m_sLastError ) )
		return false;

	auto pSegments = m_tRtChunks.RamSegs();
	auto& dSegments = *pSegments;
	wrChunk.PutDword ( 0 );
	wrChunk.PutDword ( dSegments.GetLength() );

	// no locks here, because it's only intended to be called from dtor
	for ( const RtSegment_t * pSeg : dSegments )
	{
		SccRL_t rLock ( pSeg->m_tLock );
		wrChunk.PutDword ( pSeg->m_uRows );
		wrChunk.PutDword ( (DWORD)pSeg->m_tAliveRows.load ( std::memory_order_relaxed ) );
		wrChunk.PutDword ( 0 );
		SaveVector ( wrChunk, pSeg->m_dWords );
		if ( m_bKeywordDict )
			SaveVector ( wrChunk, pSeg->m_dKeywordCheckpoints );

		auto pCheckpoints = (const char *)pSeg->m_dKeywordCheckpoints.Begin();
		wrChunk.PutDword ( pSeg->m_dWordCheckpoints.GetLength() );
		for ( const auto& dWordCheckpoint : pSeg->m_dWordCheckpoints )
		{
			wrChunk.PutOffset ( dWordCheckpoint.m_iOffset );
			if ( m_bKeywordDict )
				wrChunk.PutOffset ( dWordCheckpoint.m_sWord-pCheckpoints );
			else
				wrChunk.PutOffset ( dWordCheckpoint.m_uWordID );
		}

		SaveVector ( wrChunk, pSeg->m_dDocs );
		SaveVector ( wrChunk, pSeg->m_dHits );
		SaveVector ( wrChunk, pSeg->m_dRows );
		pSeg->m_tDeadRowMap.Save ( wrChunk );
		SaveVector ( wrChunk, pSeg->m_dBlobs );

		if ( pSeg->m_pDocstore.Ptr() )
			pSeg->m_pDocstore->Save(wrChunk);

		wrChunk.PutByte ( pSeg->m_pColumnar.Ptr() ? 1 : 0 );
		if ( pSeg->m_pColumnar.Ptr() )
			pSeg->m_pColumnar->Save(wrChunk);

		// infixes
		SaveVector ( wrChunk, pSeg->m_dInfixFilterCP );
	}

	// field lengths
	wrChunk.PutDword ( m_tSchema.GetFieldsCount() );
	for ( int i=0; i < m_tSchema.GetFieldsCount(); ++i )
		wrChunk.PutOffset ( m_dFieldLensRam[i] );

	wrChunk.CloseFile();
	if ( wrChunk.IsError() )
		return false;

	// rename
	if ( sph::rename ( sNewChunk.cstr(), sChunk.cstr() ) )
		sphDie ( "failed to rename ram chunk (src=%s, dst=%s, errno=%d, error=%s)",
			sNewChunk.cstr(), sChunk.cstr(), errno, strerrorm(errno) ); // !COMMIT handle this gracefully

	return true;
}


bool RtIndex_c::LoadRamChunk ( DWORD uVersion, bool bRebuildInfixes ) NO_THREAD_SAFETY_ANALYSIS
{
	MEMORY ( MEM_INDEX_RT );

	CSphString sChunk;
	sChunk.SetSprintf ( "%s.ram", m_sPath.cstr() );

	if ( !sphIsReadable ( sChunk.cstr(), &m_sLastError ) )
		return true;

	m_bHasFiles = true;

	CSphAutoreader rdChunk;
	if ( !rdChunk.Open ( sChunk, m_sLastError ) )
		return false;

	int64_t iFileSize = rdChunk.GetFilesize();

	bool bHasMorphology = ( m_pDict && m_pDict->HasMorphology() ); // fresh and old-format index still has no dictionary at this point
	rdChunk.GetDword ();

	auto iSegmentCount = (int) rdChunk.GetDword();
	if ( !CheckVectorLength<RtSegVec_c::BASE> ( iSegmentCount, iFileSize, "ram-chunks", m_sLastError ) )
		return false;

	RtWriter_c tWriter ( m_tRtChunks );
	tWriter.InitRamSegs ( RtWriter_c::empty );
	for ( int i = 0; i < iSegmentCount; ++i )
	{

		DWORD uRows = rdChunk.GetDword();

		RtSegmentRefPtf_t pSeg {new RtSegment_t ( uRows )};
		pSeg->m_uRows = uRows;
		pSeg->m_tAliveRows.store ( rdChunk.GetDword (), std::memory_order_relaxed );

		rdChunk.GetDword ();
		if ( !LoadVector ( rdChunk, pSeg->m_dWords, iFileSize, "ram-words", m_sLastError ) )
			return false;

		if ( m_bKeywordDict && !LoadVector ( rdChunk, pSeg->m_dKeywordCheckpoints, iFileSize, "ram-checkpoints", m_sLastError ) )
			return false;

		auto * pCheckpoints = (const char *)pSeg->m_dKeywordCheckpoints.Begin();

		auto iCheckpointCount = (int) rdChunk.GetDword();
		if ( !CheckVectorLength<decltype( pSeg->m_dWordCheckpoints)> ( iCheckpointCount, iFileSize, "ram-checkpoints", m_sLastError ) )
			return false;

		pSeg->m_dWordCheckpoints.Resize ( iCheckpointCount );
		ARRAY_FOREACH ( i, pSeg->m_dWordCheckpoints )
		{
			pSeg->m_dWordCheckpoints[i].m_iOffset = (int)rdChunk.GetOffset();
			SphOffset_t uOff = rdChunk.GetOffset();
			if ( m_bKeywordDict )
				pSeg->m_dWordCheckpoints[i].m_sWord = pCheckpoints + uOff;
			else
				pSeg->m_dWordCheckpoints[i].m_uWordID = (SphWordID_t)uOff;
		}

		if ( !LoadVector ( rdChunk, pSeg->m_dDocs, iFileSize, "ram-doclist", m_sLastError ) )
			return false;

		if ( !LoadVector ( rdChunk, pSeg->m_dHits, iFileSize, "ram-hitlist", m_sLastError ) )
			return false;

		if ( !LoadVector ( rdChunk, pSeg->m_dRows, iFileSize, "ram-attributes", m_sLastError ) )
			return false;

		pSeg->m_tDeadRowMap.Load ( uRows, rdChunk, m_sLastError );

		if ( !LoadVector ( rdChunk, pSeg->m_dBlobs, iFileSize, "ram-blobs", m_sLastError ) )
			return false;

		if ( uVersion>=15 && m_tSchema.HasStoredFields() )
		{
			pSeg->m_pDocstore = CreateDocstoreRT();
			SetupDocstoreFields ( *pSeg->m_pDocstore.Ptr(), m_tSchema );
			assert ( pSeg->m_pDocstore.Ptr() );
			if ( !pSeg->m_pDocstore->Load ( rdChunk ) )
				return false;
		}

		if ( uVersion>=19 && rdChunk.GetByte() )
		{
			pSeg->m_pColumnar = CreateColumnarRT ( m_tSchema, rdChunk, m_sLastError );
			if ( !pSeg->m_pColumnar )
				return false;
		}

		// infixes
		if ( !LoadVector ( rdChunk, pSeg->m_dInfixFilterCP, iFileSize, "ram-infixes", m_sLastError ) )
			return false;

		if ( bRebuildInfixes )
			BuildSegmentInfixes ( pSeg, bHasMorphology, m_bKeywordDict, m_tSettings.m_iMinInfixLen, m_iWordsCheckpoint, ( m_iMaxCodepointLength>1 ), m_tSettings.m_eHitless );

		pSeg->BuildDocID2RowIDMap(m_tSchema);
		tWriter.m_pNewRamSegs->Add ( AdoptSegment ( pSeg ) );
	}

	// field lengths
	auto iFields = (int) rdChunk.GetDword();
	assert ( iFields==m_tSchema.GetFieldsCount() );

	for ( int i=0; i<iFields; ++i )
		m_dFieldLensRam[i] = rdChunk.GetOffset();

	// all done
	if ( rdChunk.GetErrorFlag() )
		return false;
	return true;
}


void RtIndex_c::PostSetup()
{
	RtIndex_i::PostSetup();

	CSphScopedPtr<DocstoreFields_i> pDocstoreFields;
	if ( m_tSchema.HasStoredFields() )
	{
		pDocstoreFields = CreateDocstoreFields();
		SetupDocstoreFields ( *pDocstoreFields.Ptr(), m_tSchema );
	}
	m_pDocstoreFields.Swap ( pDocstoreFields );
	pDocstoreFields = nullptr;

	m_iMaxCodepointLength = m_pTokenizer->GetMaxCodepointLength();

	// bigram filter
	if ( m_tSettings.m_eBigramIndex!=SPH_BIGRAM_NONE && m_tSettings.m_eBigramIndex!=SPH_BIGRAM_ALL )
	{
		m_pTokenizer->SetBuffer ( (BYTE*)const_cast<char*> ( m_tSettings.m_sBigramWords.cstr() ), m_tSettings.m_sBigramWords.Length() );

		BYTE * pTok = nullptr;
		while ( ( pTok = m_pTokenizer->GetToken() )!=nullptr )
			m_tSettings.m_dBigramWords.Add() = (const char*)pTok;

		m_tSettings.m_dBigramWords.Sort();
	}

	// FIXME!!! handle error
	m_pTokenizerIndexing = m_pTokenizer->Clone ( SPH_CLONE_INDEX );
	TokenizerRefPtr_c pIndexing { ISphTokenizer::CreateBigramFilter ( m_pTokenizerIndexing, m_tSettings.m_eBigramIndex, m_tSettings.m_sBigramWords, m_sLastError ) };
	if ( pIndexing )
		m_pTokenizerIndexing = pIndexing;

	const CSphDictSettings & tDictSettings = m_pDict->GetSettings();
	if ( !ParseMorphFields ( tDictSettings.m_sMorphology, tDictSettings.m_sMorphFields, m_tSchema.GetFields(), m_tMorphFields, m_sLastError ) )
		sphWarning ( "index '%s': %s", m_sIndexName.cstr(), m_sLastError.cstr() );

	// hitless
	CSphString sHitlessFiles = m_tSettings.m_sHitlessFiles;
	if ( GetIndexFilenameBuilder() )
	{
		CSphScopedPtr<const FilenameBuilder_i> pFilenameBuilder ( GetIndexFilenameBuilder() ( m_sIndexName.cstr() ) );
		if ( pFilenameBuilder.Ptr() )
			sHitlessFiles = pFilenameBuilder->GetFullPath ( sHitlessFiles );
	}

	if ( !LoadHitlessWords ( sHitlessFiles, m_pTokenizerIndexing, m_pDict, m_dHitlessWords, m_sLastError ) )
		sphWarning ( "index '%s': %s", m_sIndexName.cstr(), m_sLastError.cstr() );

	// still need index files for index just created from config
	if ( !m_bHasFiles )
	{
		SaveRamChunk ();
		SaveMeta ();
	}
}

struct MemoryDebugCheckReader_c : public DebugCheckReader_i
{
	MemoryDebugCheckReader_c ( const BYTE * pData, const BYTE * pDataEnd )
		: m_pData ( pData )
		, m_pDataEnd ( pDataEnd )
		, m_pCur ( pData )
	{}

	int64_t GetLengthBytes () override
	{
		return ( m_pDataEnd - m_pData );
	}
	bool GetBytes ( void * pData, int iSize ) override
	{
		if ( m_pCur && m_pCur+iSize<=m_pDataEnd )
		{
			memcpy ( pData, m_pCur, iSize );
			m_pCur += iSize;
			return true;
		} else
		{
			return false;
		}
	}

	bool SeekTo ( int64_t iOff, int iHint ) final
	{
		if ( m_pData && m_pData+iOff<m_pDataEnd )
		{
			m_pCur = m_pData + iOff;
			return true;
		} else
		{
			return false;
		}
	}

	const BYTE * m_pData = nullptr;
	const BYTE * m_pDataEnd = nullptr;
	const BYTE * m_pCur = nullptr;
};

int RtIndex_c::DebugCheck ( FILE * fp )
{
	// FIXME! remove copypasted code from CSphIndex_VLN::DebugCheck

	DebugCheckError_c tReporter(fp);

	if ( m_iLockFD<0 && m_iCheckChunk==-1 )
		sphWarning ( "failed to load RAM chunks, checking only %d disk chunks", m_dChunkNames.GetLength() );

	if ( m_iStride!=m_tSchema.GetRowSize() )
		tReporter.Fail ( "wrong attribute stride (current=%d, should_be=%d)", m_iStride, m_tSchema.GetRowSize() );

	if ( m_iSoftRamLimit<=0 )
		tReporter.Fail ( "wrong RAM limit (current=" INT64_FMT ")", m_iSoftRamLimit );

	if ( m_iTID<0 )
		tReporter.Fail ( "index TID < 0 (current=" INT64_FMT ")", m_iTID );

	if ( m_iSavedTID<0 )
		tReporter.Fail ( "index saved TID < 0 (current=" INT64_FMT ")", m_iSavedTID );

	if ( m_iTID<m_iSavedTID )
		tReporter.Fail ( "index TID < index saved TID (current=" INT64_FMT ", saved=" INT64_FMT ")", m_iTID, m_iSavedTID );

	if ( m_iWordsCheckpoint!=RTDICT_CHECKPOINT_V5 )
		tReporter.Fail ( "unexpected number of words per checkpoint (expected 48, got %d)", m_iWordsCheckpoint );

	tReporter.Msg ( "checking schema..." );
	DebugCheckSchema ( m_tSchema, tReporter );

	if ( m_iCheckChunk!=-1 )
		DebugCheckRam ( tReporter );

	int iFailsPlain = DebugCheckDisk ( tReporter, fp );

	tReporter.Done();

	return int ( tReporter.GetNumFails() + iFailsPlain );
}

void RtIndex_c::DebugCheckRam ( DebugCheckError_c & tReporter ) NO_THREAD_SAFETY_ANALYSIS
{
	auto pRamSegs = m_tRtChunks.RamSegs();
	auto& dRamSegs = *pRamSegs;
	ARRAY_FOREACH ( iSegment, dRamSegs )
	{
		tReporter.Msg ( "checking RT segment %d(%d)...", iSegment, dRamSegs.GetLength() );

		if ( !dRamSegs[iSegment] )
		{
			tReporter.Fail ( "missing RT segment (segment=%d)", iSegment );
			continue;
		}

		const RtSegment_t & tSegment = *dRamSegs[iSegment];
		if ( !tSegment.m_uRows )
		{
			tReporter.Fail ( "empty RT segment (segment=%d)", iSegment );
			continue;
		}

		const BYTE * pCurWord = tSegment.m_dWords.Begin();
		const BYTE * pMaxWord = pCurWord+tSegment.m_dWords.GetLength();
		const BYTE * pCurDoc = tSegment.m_dDocs.Begin();
		const BYTE * pMaxDoc = pCurDoc+tSegment.m_dDocs.GetLength();
		const BYTE * pCurHit = tSegment.m_dHits.Begin();
		const BYTE * pMaxHit = pCurHit+tSegment.m_dHits.GetLength();

		CSphVector<RtWordCheckpoint_t> dRefCheckpoints;
		int nWordsRead = 0;
		int nCheckpointWords = 0;
		int iCheckpointOffset = 0;
		SphWordID_t uPrevWordID = 0;
		DWORD uPrevDocOffset = 0;
		DWORD uPrevHitOffset = 0;

		RtWord_t tWord;
		tWord.m_bHasHitlist = false;

		BYTE sWord[SPH_MAX_KEYWORD_LEN+2], sLastWord[SPH_MAX_KEYWORD_LEN+2];
		memset ( sWord, 0, sizeof(sWord) );
		memset ( sLastWord, 0, sizeof(sLastWord) );

		int iLastWordLen = 0, iWordLen = 0;

		while ( pCurWord && pCurWord<pMaxWord )
		{
			bool bCheckpoint = ++nCheckpointWords==m_iWordsCheckpoint;
			if ( bCheckpoint )
			{
				nCheckpointWords = 1;
				iCheckpointOffset = int ( pCurWord - tSegment.m_dWords.Begin() );
				tWord.m_uDoc = 0;
				if ( !m_bKeywordDict )
					tWord.m_uWordID = 0;
			}

			const BYTE * pIn = pCurWord;
			if ( m_bKeywordDict )
			{
				BYTE iMatch, iDelta, uPacked;
				uPacked = *pIn++;

				if ( pIn>=pMaxWord )
				{
					tReporter.Fail ( "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead );
					break;
				}

				if ( uPacked & 0x80 )
				{
					iDelta = ( ( uPacked>>4 ) & 7 ) + 1;
					iMatch = uPacked & 15;
				} else
				{
					iDelta = uPacked & 127;
					iMatch = *pIn++;
					if ( pIn>=pMaxWord )
					{
						tReporter.Fail ( "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead );
						break;
					}

					if ( iDelta<=8 && iMatch<=15 )
					{
						sLastWord[sizeof(sLastWord)-1] = '\0';
						tReporter.Fail ( "wrong word-delta (segment=%d, word=%d, last_word=%s, last_len=%d, match=%d, delta=%d)",
							iSegment, nWordsRead, sLastWord+1, iLastWordLen, iMatch, iDelta );
					}
				}

				if ( iMatch+iDelta>=(int)sizeof(sWord)-2 || iMatch>iLastWordLen )
				{
					sLastWord[sizeof(sLastWord)-1] = '\0';
					tReporter.Fail ( "wrong word-delta (segment=%d, word=%d, last_word=%s, last_len=%d, match=%d, delta=%d)",
						iSegment, nWordsRead, sLastWord+1, iLastWordLen, iMatch, iDelta );

					pIn += iDelta;
					if ( pIn>=pMaxWord )
					{
						tReporter.Fail ( "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead );
						break;
					}
				} else
				{
					iWordLen = iMatch+iDelta;
					sWord[0] = (BYTE)iWordLen;
					memcpy ( sWord+1+iMatch, pIn, iDelta );
					sWord[1+iWordLen] = 0;
					pIn += iDelta;
					if ( pIn>=pMaxWord )
					{
						tReporter.Fail ( "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead );
						break;
					}
				}

				auto iCalcWordLen = (int) strlen ( (const char *)sWord+1 );
				if ( iWordLen!=iCalcWordLen )
				{
					sWord[sizeof(sWord)-1] = '\0';
					tReporter.Fail ( "word length mismatch (segment=%d, word=%d, read_word=%s, read_len=%d, calc_len=%d)", iSegment, nWordsRead, sWord+1, iWordLen, iCalcWordLen );
				}

				if ( !iWordLen )
					tReporter.Fail ( "empty word in word list (segment=%d, word=%d)", iSegment, nWordsRead );

				const BYTE * pStr = sWord+1;
				const BYTE * pStringStart = pStr;
				while ( pStringStart-pStr < iWordLen )
				{
					if ( !*pStringStart )
					{
						CSphString sErrorStr;
						sErrorStr.SetBinary ( (const char*)pStr, iWordLen );
						tReporter.Fail ( "embedded zero in a word list string (segment=%d, offset=%u, string=%s)", iSegment, (DWORD)(pStringStart-pStr), sErrorStr.cstr() );
					}

					pStringStart++;
				}

				if ( iLastWordLen && iWordLen )
				{
					if ( sphDictCmpStrictly ( (const char *)sWord+1, iWordLen, (const char *)sLastWord+1, iLastWordLen )<=0 )
					{
						sWord[sizeof(sWord)-1] = '\0';
						sLastWord[sizeof(sLastWord)-1] = '\0';
						tReporter.Fail ( "word order decreased (segment=%d, word=%d, read_word=%s, last_word=%s)", iSegment, nWordsRead, sWord+1, sLastWord+1 );
					}
				}

				memcpy ( sLastWord, sWord, iWordLen+2 );
				iLastWordLen = iWordLen;
			} else
			{
				tWord.m_uWordID += UnzipWordid ( pIn );
				if ( pIn>=pMaxWord )
					tReporter.Fail ( "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead );

				if ( tWord.m_uWordID<=uPrevWordID )
				{
					tReporter.Fail ( "wordid decreased (segment=%d, word=%d, wordid=" UINT64_FMT ", previd=" UINT64_FMT ")", iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, (uint64_t)uPrevWordID );
				}

				uPrevWordID = tWord.m_uWordID;
			}

			UnzipDword ( &tWord.m_uDocs, pIn );
			if ( pIn>=pMaxWord )
			{
				sWord[sizeof(sWord)-1] = '\0';
				tReporter.Fail ( "invalid docs/hits (segment=%d, word=%d, read_word=%s, docs=%u, hits=%u)", iSegment, nWordsRead, sWord+1, tWord.m_uDocs, tWord.m_uHits );
			}

			UnzipDword ( &tWord.m_uHits, pIn );
			if ( pIn>=pMaxWord )
				tReporter.Fail ( "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead );

			tWord.m_uDoc += UnzipDword ( pIn );
			if ( pIn>pMaxWord )
				tReporter.Fail ( "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead );

			pCurWord = pIn;

			if ( !tWord.m_uDocs || !tWord.m_uHits || tWord.m_uHits<tWord.m_uDocs )
			{
				sWord[sizeof(sWord)-1] = '\0';
				tReporter.Fail ( "invalid docs/hits (segment=%d, word=%d, read_wordid=" UINT64_FMT ", read_word=%s, docs=%u, hits=%u)",
					iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, sWord+1, tWord.m_uDocs, tWord.m_uHits );
			}

			if ( bCheckpoint )
			{
				RtWordCheckpoint_t & tCP = dRefCheckpoints.Add();
				tCP.m_iOffset = iCheckpointOffset;

				if ( m_bKeywordDict )
				{
					tCP.m_sWord = new char [sWord[0]+1];
					memcpy ( (void *)tCP.m_sWord, sWord+1, sWord[0]+1 );
				} else
					tCP.m_uWordID = tWord.m_uWordID;
			}

			sWord[sizeof(sWord)-1] = '\0';

			if ( uPrevDocOffset && tWord.m_uDoc<=uPrevDocOffset )
				tReporter.Fail ( "doclist offset decreased (segment=%d, word=%d, read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, prev_doclist_offset=%u)",
					iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, sWord+1, tWord.m_uDoc, uPrevDocOffset );

			// read doclist
			auto uDocOffset = DWORD ( pCurDoc-tSegment.m_dDocs.Begin() );
			if ( tWord.m_uDoc!=uDocOffset )
			{
				tReporter.Fail ( "unexpected doclist offset (wordid=" UINT64_FMT "(%s)(%d), doclist_offset=%u, expected_offset=%u)",
					(uint64_t)tWord.m_uWordID, sWord+1, nWordsRead, tWord.m_uDoc, uDocOffset );

				if ( uDocOffset>=(DWORD)tSegment.m_dDocs.GetLength() )
				{
					tReporter.Fail ( "doclist offset pointing past doclist (segment=%d, word=%d, read_word=%s, doclist_offset=%u, doclist_size=%d)",
						iSegment, nWordsRead, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() );

					nWordsRead++;
					continue;
				} else
					pCurDoc = tSegment.m_dDocs.Begin()+uDocOffset;
			}

			// read all docs from doclist
			RtDoc_t tDoc;
			RowID_t tPrevRowID = INVALID_ROWID;

			for ( DWORD uDoc=0; uDoc<tWord.m_uDocs && pCurDoc<pMaxDoc; uDoc++ )
			{
				bool bEmbeddedHit = false;
				pIn = pCurDoc;

				tDoc.m_tRowID += UnzipDword ( pIn );
				if ( pIn>=pMaxDoc )
				{
					tReporter.Fail ( "reading past doclist end (segment=%d, word=%d, read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, doclist_size=%d)",
						iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() );
					break;
				}

				UnzipDword ( &tDoc.m_uDocFields, pIn );
				if ( pIn>=pMaxDoc )
				{
					tReporter.Fail ( "reading past doclist end (segment=%d, word=%d, read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, doclist_size=%d)",
						iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() );
					break;
				}

				UnzipDword ( &tDoc.m_uHits, pIn );
				if ( pIn>=pMaxDoc )
				{
					tReporter.Fail ( "reading past doclist end (segment=%d, word=%d, read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, doclist_size=%d)",
						iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() );
					break;
				}

				if ( tDoc.m_uHits==1 )
				{
					bEmbeddedHit = true;

					auto a = UnzipDword ( pIn );
					if ( pIn>=pMaxDoc )
					{
						tReporter.Fail ( "reading past doclist end (segment=%d, word=%d, read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, doclist_size=%d)",
							iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() );
						break;
					}

					auto b = UnzipDword ( pIn );
					if ( pIn>pMaxDoc )
					{
						tReporter.Fail ( "reading past doclist end (segment=%d, word=%d, read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, doclist_size=%d)",
							iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() );
						break;
					}

					tDoc.m_uHit = HITMAN::Create ( b, a );
				} else
				{
					UnzipDword ( &tDoc.m_uHit, pIn );
					if ( pIn>pMaxDoc )
					{
						tReporter.Fail ( "reading past doclist end (segment=%d, word=%d, read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, doclist_size=%d)",
							iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() );
						break;
					}
				}

				pCurDoc = pIn;

				if ( uDoc && tDoc.m_tRowID<=tPrevRowID )
				{
					tReporter.Fail ( "rowid decreased (segment=%d, word=%d, read_wordid=" UINT64_FMT ", read_word=%s, rowid=%u, prev_rowid=%u)",
						iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, sWord+1, tDoc.m_tRowID, tPrevRowID );
				}

				if ( tDoc.m_tRowID>=tSegment.m_uRows )
					tReporter.Fail ( "invalid rowid (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u)", iSegment, nWordsRead, tWord.m_uWordID, tDoc.m_tRowID );

				if ( bEmbeddedHit )
				{
					DWORD uFieldId = HITMAN::GetField ( tDoc.m_uHit );
					DWORD uFieldMask = tDoc.m_uDocFields;
					int iCounter = 0;
					for ( ; uFieldMask; iCounter++ )
						uFieldMask &= uFieldMask - 1;

					if ( iCounter!=1 || tDoc.m_uHits!=1 )
					{
						tReporter.Fail ( "embedded hit with multiple occurences in a document found (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u)",
							iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID );
					}

					if ( (int)uFieldId>m_tSchema.GetFieldsCount() || uFieldId>SPH_MAX_FIELDS )
					{
						tReporter.Fail ( "invalid field id in an embedded hit (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u, field_id=%u, total_fields=%d)",
							iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID, uFieldId, m_tSchema.GetFieldsCount() );
					}

					if ( !( tDoc.m_uDocFields & ( 1 << uFieldId ) ) )
					{
						tReporter.Fail ( "invalid field id: not in doclist mask (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u, field_id=%u, field_mask=%u)",
							iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID, uFieldId, tDoc.m_uDocFields );
					}
				} else
				{
					auto uExpectedHitOffset = DWORD ( pCurHit-tSegment.m_dHits.Begin() );
					if ( tDoc.m_uHit!=uExpectedHitOffset )
					{
						tReporter.Fail ( "unexpected hitlist offset (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u, offset=%u, expected_offset=%u",
							iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID, tDoc.m_uHit, uExpectedHitOffset );
					}

					if ( tDoc.m_uHit && tDoc.m_uHit<=uPrevHitOffset )
					{
						tReporter.Fail ( "hitlist offset decreased (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u, offset=%u, prev_offset=%u",
							iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID, tDoc.m_uHit, uPrevHitOffset );
					}

					// check hitlist
					DWORD uHitlistEntry = 0;
					DWORD uLastPosInField = 0;
					DWORD uLastFieldId = 0;
					bool bLastInFieldFound = false;

					for ( DWORD uHit = 0; uHit < tDoc.m_uHits && pCurHit; uHit++ )
					{
						uHitlistEntry += UnzipDword ( pCurHit );
						if ( pCurHit>pMaxHit )
						{
							tReporter.Fail ( "reading past hitlist end (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u)", iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID );
							break;
						}

						DWORD uPosInField = HITMAN::GetPos ( uHitlistEntry );
						bool bLastInField = HITMAN::IsEnd ( uHitlistEntry );
						DWORD uFieldId = HITMAN::GetField ( uHitlistEntry );

						if ( (int)uFieldId>m_tSchema.GetFieldsCount() || uFieldId>SPH_MAX_FIELDS )
						{
							tReporter.Fail ( "invalid field id in a hitlist (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u, field_id=%u, total_fields=%d)",
								iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID, uFieldId, m_tSchema.GetFieldsCount() );
						}

						if ( !( tDoc.m_uDocFields & ( 1 << uFieldId ) ) )
						{
							tReporter.Fail ( "invalid field id: not in doclist mask (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u, field_id=%u, field_mask=%u)",
								iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID, uFieldId, tDoc.m_uDocFields );
						}

						if ( uLastFieldId!=uFieldId )
						{
							bLastInFieldFound = false;
							uLastPosInField = 0;
						}

						if ( uLastPosInField && uPosInField<=uLastPosInField )
						{
							tReporter.Fail ( "hit position in field decreased (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u, pos=%u, last_pos=%u)",
								iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID, uPosInField, uLastPosInField );
						}

						if ( bLastInField && bLastInFieldFound )
							tReporter.Fail ( "duplicate last-in-field hit found (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u)", iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID );

						uLastPosInField = uPosInField;
						uLastFieldId = uFieldId;
						bLastInFieldFound |= bLastInField;
					}

					uPrevHitOffset = tDoc.m_uHit;
				}

				DWORD uAvailFieldMask = ( 1 << m_tSchema.GetFieldsCount() ) - 1;
				if ( tDoc.m_uDocFields & ~uAvailFieldMask )
				{
					tReporter.Fail ( "wrong document field mask (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u, mask=%u, total_fields=%d",
						iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID, tDoc.m_uDocFields, m_tSchema.GetFieldsCount() );
				}

				tPrevRowID = tDoc.m_tRowID;
			}

			uPrevDocOffset = tWord.m_uDoc;
			nWordsRead++;
		}

		if ( pCurDoc!=pMaxDoc )
			tReporter.Fail ( "unused doclist entries found (segment=%d, doclist_size=%d)", iSegment, tSegment.m_dDocs.GetLength() );

		if ( pCurHit!=pMaxHit )
			tReporter.Fail ( "unused hitlist entries found (segment=%d, hitlist_size=%d)", iSegment, tSegment.m_dHits.GetLength() );

		if ( dRefCheckpoints.GetLength()!=tSegment.m_dWordCheckpoints.GetLength() )
			tReporter.Fail ( "word checkpoint count mismatch (read=%d, calc=%d)", tSegment.m_dWordCheckpoints.GetLength(), dRefCheckpoints.GetLength() );

		for ( int i=0; i < Min ( dRefCheckpoints.GetLength(), tSegment.m_dWordCheckpoints.GetLength() ); i++ )
		{
			const RtWordCheckpoint_t & tRefCP = dRefCheckpoints[i];
			const RtWordCheckpoint_t & tCP = tSegment.m_dWordCheckpoints[i];
			const int iLen = m_bKeywordDict ? (const int) strlen ( tCP.m_sWord ) : 0;
			if ( m_bKeywordDict && ( !tCP.m_sWord || ( !strlen ( tRefCP.m_sWord ) || !strlen ( tCP.m_sWord ) ) ) )
			{
				tReporter.Fail ( "empty word checkpoint %d ((segment=%d, read_word=%s, read_len=%u, readpos=%d, calc_word=%s, calc_len=%u, calcpos=%d)",
					i, iSegment, tCP.m_sWord, (DWORD)strlen ( tCP.m_sWord ), tCP.m_iOffset,
					tRefCP.m_sWord, (DWORD)strlen ( tRefCP.m_sWord ), tRefCP.m_iOffset );
			} else if ( sphCheckpointCmpStrictly ( tCP.m_sWord, iLen, tCP.m_uWordID, m_bKeywordDict, tRefCP ) || tRefCP.m_iOffset!=tCP.m_iOffset )
			{
				if ( m_bKeywordDict )
				{
					tReporter.Fail ( "word checkpoint %d differs (segment=%d, read_word=%s, readpos=%d, calc_word=%s, calcpos=%d)",
						i, iSegment, tCP.m_sWord, tCP.m_iOffset, tRefCP.m_sWord, tRefCP.m_iOffset );
				} else
				{
					tReporter.Fail ( "word checkpoint %d differs (segment=%d, readid=" UINT64_FMT ", readpos=%d, calcid=" UINT64_FMT ", calcpos=%d)",
						i, iSegment, (uint64_t)tCP.m_uWordID, tCP.m_iOffset, (int64_t)tRefCP.m_uWordID, tRefCP.m_iOffset );
				}
			}
		}

		if ( m_bKeywordDict )
			ARRAY_FOREACH ( i, dRefCheckpoints )
				SafeDeleteArray ( dRefCheckpoints[i].m_sWord );

		dRefCheckpoints.Reset ();

		MemoryDebugCheckReader_c tAttrs ( (const BYTE *)tSegment.m_dRows.begin(), (const BYTE *)tSegment.m_dRows.end() );
		MemoryDebugCheckReader_c tBlobs ( tSegment.m_dBlobs.begin(), tSegment.m_dBlobs.end() );
		DebugCheck_Attributes ( tAttrs, tBlobs, tSegment.m_uRows, 0, m_tSchema, tReporter );
		DebugCheck_DeadRowMap ( tSegment.m_tDeadRowMap.GetLengthBytes(), tSegment.m_uRows, tReporter );

		DWORD uCalcAliveRows = tSegment.m_tDeadRowMap.GetNumAlive();
		if ( tSegment.m_tAliveRows.load(std::memory_order_relaxed)!=uCalcAliveRows )
			tReporter.Fail ( "alive row count mismatch (segment=%d, expected=%u, current=%u)", iSegment, uCalcAliveRows,
					tSegment.m_tAliveRows.load ( std::memory_order_relaxed ) );
	}

} // NOLINT function length

int RtIndex_c::DebugCheckDisk ( DebugCheckError_c & tReporter, FILE * fp )
{
	CreateFilenameBuilder_fn fnCreateFilenameBuilder = GetIndexFilenameBuilder();
	CSphScopedPtr<FilenameBuilder_i> pFilenameBuilder ( fnCreateFilenameBuilder ? fnCreateFilenameBuilder ( m_sIndexName.cstr() ) : nullptr );

	VecTraits_T<int> dChunks = m_dChunkNames.Slice();
	if ( m_iCheckChunk!=-1 )
	{
		int iChunk = dChunks.GetFirst (  [&] ( int & v ) { return m_iCheckChunk==v; }  );
		if ( iChunk==-1 )
		{
			tReporter.Fail ( "failed to find disk chunk %s.%d, disk chunks total %d", m_sPath.cstr(), m_iCheckChunk, m_dChunkNames.GetLength() );
			return 1;
		}

		dChunks = m_dChunkNames.Slice ( iChunk, 1 );
	}

	int iFailsPlain = 0;
	StrVec_t dWarnings;
	ARRAY_FOREACH ( i, dChunks )
	{
		int iChunk = dChunks[i];
		CSphString sChunk;
		sChunk.SetSprintf ( "%s.%d", m_sPath.cstr(), iChunk );
		tReporter.Msg ( "checking disk chunk, extension %d, %d(%d)...", dChunks[i], i, m_dChunkNames.GetLength() );

		CSphScopedPtr<CSphIndex> pIndex ( PreallocDiskChunk ( sChunk.cstr(), iChunk, pFilenameBuilder.Ptr(), dWarnings, m_sLastError ) );
		if ( pIndex.Ptr() )
		{
			iFailsPlain += pIndex->DebugCheck ( fp );
		} else
		{
			tReporter.Fail ( "%s", m_sLastError.cstr() );
			m_sLastError = "";
			iFailsPlain++;
		}
	}

	for ( const auto & i : dWarnings )
		tReporter.Msg ( "warning: %s", i.cstr() );

	return iFailsPlain;
}


void RtIndex_c::SetDebugCheck ( bool bCheckIdDups, int iCheckChunk )
{
	m_bDebugCheck = true;
	m_bCheckIdDups = bCheckIdDups;
	m_iCheckChunk = iCheckChunk;
	ProhibitSave();
}

//////////////////////////////////////////////////////////////////////////
// SEARCHING
//////////////////////////////////////////////////////////////////////////

struct RtQword_t final : public ISphQword
{
public:
	RtQword_t () = default;

	const CSphMatch & GetNextDoc() final
	{
		while (true)
		{
			if ( !m_tDocReader.UnzipDoc() )
			{
				m_tMatch.m_tRowID = INVALID_ROWID;
				return m_tMatch;
			}

			if ( !m_pSeg->m_tDeadRowMap.IsSet ( m_tDocReader->m_tRowID ) )
				break;
		}

		const auto* pDoc = (const RtDoc_t*)m_tDocReader;
		m_tMatch.m_tRowID = pDoc->m_tRowID;
		m_dQwordFields.Assign32 ( pDoc->m_uDocFields );
		m_uMatchHits = pDoc->m_uHits;
		m_iHitlistPos = (uint64_t(pDoc->m_uHits)<<32) + pDoc->m_uHit;
		m_bAllFieldsKnown = false;
		return m_tMatch;
	}

	void SeekHitlist ( SphOffset_t uOff ) final
	{
		auto iHits = (int)(uOff>>32);
		if ( iHits==1 )
		{
			m_uNextHit = DWORD(uOff);
		} else
		{
			m_uNextHit = 0;
			m_tHitReader.Seek ( m_pHits + DWORD(uOff), iHits );
		}
	}

	Hitpos_t GetNextHit () final
	{
		if ( !m_uNextHit )
			return Hitpos_t ( m_tHitReader.UnzipHit() );
		else if ( m_uNextHit==0xffffffffUL )
			return EMPTY_HIT;
		else
			return Hitpos_t ( std::exchange ( m_uNextHit, 0xffffffffUL ) );
	}

	bool SetupScan ( const RtIndex_c * pIndex, int iSegment, const RtGuard_t& tGuard ) final
	{
		return pIndex->RtQwordSetup ( this, iSegment, tGuard );
	}

	void SetupReader ( const RtSegment_t * pSeg, const RtWord_t & tWord )
	{
		m_pSeg = pSeg;
		m_tDocReader.Init ( pSeg, tWord );
		m_pHits = pSeg->m_dHits.begin();
	}

private:
	RtDocReader_c		m_tDocReader;
	RtHitReader_c		m_tHitReader;

	CSphMatch			m_tMatch;
	DWORD				m_uNextHit {0};

	const BYTE* m_pHits {nullptr};
	const RtSegment_t * m_pSeg {nullptr};
};


//////////////////////////////////////////////////////////////////////////


struct RtSubstringPayload_t : public ISphSubstringPayload
{
	RtSubstringPayload_t ( int iSegmentCount, int iDoclists )
		: m_dSegment2Doclists ( iSegmentCount )
		, m_dDoclist ( iDoclists )
	{}
	CSphFixedVector<Slice_t>	m_dSegment2Doclists;
	CSphFixedVector<Slice_t>	m_dDoclist;
};


struct RtQwordPayload_t final : public ISphQword
{
public:
	explicit RtQwordPayload_t ( const RtSubstringPayload_t * pPayload )
		: m_pPayload ( pPayload )
	{
		m_tMatch.Reset ( 0 );
		m_iDocs = m_pPayload->m_iTotalDocs;
		m_iHits = m_pPayload->m_iTotalHits;

		m_uDoclist = 0;
		m_uDoclistLeft = 0;
		m_pSegment = nullptr;
		m_uHitEmbeded = EMPTY_HIT;
	}

	const CSphMatch & GetNextDoc() final
	{
		m_iHits = 0;
		while ( true )
			if ( !m_tDocReader.UnzipDoc() )
			{
				if ( !m_uDoclistLeft )
				{
					m_tMatch.m_tRowID = INVALID_ROWID;
					return m_tMatch;
				}
				SetupReader();
			} else if ( !m_pSegment->m_tDeadRowMap.IsSet ( m_tDocReader->m_tRowID ) )
				break;

		const auto* pDoc = (const RtDoc_t*) m_tDocReader;
		m_tMatch.m_tRowID = pDoc->m_tRowID;
		m_dQwordFields.Assign32 ( pDoc->m_uDocFields );
		m_bAllFieldsKnown = false;
		m_iHits = pDoc->m_uHits;
		m_uHitEmbeded = pDoc->m_uHit;
		m_tHitReader.Seek ( *m_pSegment, *pDoc );
		return m_tMatch;
	}

	void SeekHitlist ( SphOffset_t ) final
	{}

	Hitpos_t GetNextHit () final
	{
		if ( m_iHits>1 )
			return Hitpos_t ( m_tHitReader.UnzipHit() );
		return ( m_iHits==1 ) ? Hitpos_t ( std::exchange ( m_uHitEmbeded, EMPTY_HIT ) ) : EMPTY_HIT;
	}

	bool SetupScan ( const RtIndex_c *, int iSegment, const RtGuard_t& tGuard ) final
	{
		m_tDocReader.Reset();

		if ( iSegment<0 )
		{
			m_pSegment = nullptr;
			m_uDoclist = 0;
			m_uDoclistLeft = 0;
			return false;
		}

		m_pSegment = tGuard.m_dRamSegs[iSegment];
		m_uDoclist = m_pPayload->m_dSegment2Doclists[iSegment].m_uOff;
		m_uDoclistLeft = m_pPayload->m_dSegment2Doclists[iSegment].m_uLen;

		if ( !m_uDoclistLeft )
			return false;

		SetupReader();
		return true;
	}

private:
	void SetupReader ()
	{
		assert ( m_uDoclistLeft );
		RtWord_t tWord;
		tWord.m_uDoc = m_pPayload->m_dDoclist[m_uDoclist].m_uOff;
		tWord.m_uDocs = m_pPayload->m_dDoclist[m_uDoclist].m_uLen;
		m_tDocReader.Init ( m_pSegment, tWord );
		++m_uDoclist;
		--m_uDoclistLeft;
	}

	const RtSubstringPayload_t *	m_pPayload;
	CSphMatch					m_tMatch;
	RtDocReader_c				m_tDocReader;
	RtHitReader_c				m_tHitReader;
	const RtSegment_t *			m_pSegment;

	DWORD						m_uDoclist;
	DWORD						m_uDoclistLeft;
	DWORD						m_uHitEmbeded;
};

struct RtScanQword_t final : public QwordScan_c
{
public:
	explicit RtScanQword_t ( int iRowsTotal )
		: QwordScan_c ( iRowsTotal )
	{
	}

	bool SetupScan ( const RtIndex_c * pIndex, int iSegment, const RtGuard_t& tGuard ) final
	{
		m_tDoc.Reset ( 0 );

		if ( iSegment<0 )
			return false;

		const auto& pSegment = tGuard.m_dRamSegs[iSegment];

		m_iRowsCount = pSegment->m_uRows;
		m_iDocs = m_iRowsCount;
		m_bDone = ( m_iRowsCount==0 );
		m_dQwordFields.SetAll();

		return ( pSegment->m_tAliveRows.load(std::memory_order_relaxed)>0 );
	}
};


//////////////////////////////////////////////////////////////////////////

class RtQwordSetup_t final : public ISphQwordSetup
{
public:
	explicit RtQwordSetup_t ( const RtGuard_t& tGuard );
	ISphQword *	QwordSpawn ( const XQKeyword_t & ) const final;
	bool		QwordSetup ( ISphQword * pQword ) const final;
	void				SetSegment ( int iSegment ) { m_iSeg = iSegment; }
	ISphQword *			ScanSpawn() const final;

private:
	const RtGuard_t&	m_tGuard;
	int					m_iSeg;
};


RtQwordSetup_t::RtQwordSetup_t ( const RtGuard_t& tGuard )
	: m_tGuard ( tGuard )
	, m_iSeg ( -1 )
{ }


ISphQword * RtQwordSetup_t::QwordSpawn ( const XQKeyword_t & tWord ) const
{
	if ( !tWord.m_pPayload )
		return new RtQword_t ();
	else
		return new RtQwordPayload_t ( (const RtSubstringPayload_t *)tWord.m_pPayload );
}


bool RtQwordSetup_t::QwordSetup ( ISphQword * pQword ) const
{
	// there was two dynamic_casts here once but they're not necessary
	// maybe it's worth to rewrite class hierarchy to avoid c-casts here?
	const auto * pIndex = (const RtIndex_c *)m_pIndex;
	return pQword->SetupScan ( pIndex, m_iSeg, m_tGuard );
}


bool RtIndex_c::EarlyReject ( CSphQueryContext * pCtx, CSphMatch & tMatch ) const
{
	auto pSegment = (RtSegment_t*)const_cast<void*>(pCtx->m_pIndexData);
	tMatch.m_pStatic = pSegment->GetDocinfoByRowID ( tMatch.m_tRowID );

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

ISphQword * RtQwordSetup_t::ScanSpawn () const
{
	return new RtScanQword_t ( (int)m_pIndex->GetStats().m_iTotalDocuments );
}


// WARNING, setup is pretty tricky
// for RT queries, we setup qwords several times
// first pass (with NULL segment arg) should sum all stats over all segments
// others passes (with non-NULL segments) should setup specific segment (including local stats)
bool RtIndex_c::RtQwordSetupSegment ( RtQword_t * pQword, const RtSegment_t * pCurSeg, bool bSetup ) const
{
	if ( !pCurSeg )
		return false;

	SphWordID_t uWordID = pQword->m_uWordID;
	const char * sWord = pQword->m_sDictWord.cstr();
	int iWordLen = pQword->m_sDictWord.Length();
	bool bPrefix = false;
	if ( m_bKeywordDict && iWordLen && sWord[iWordLen-1]=='*' ) // crc star search emulation
	{
		iWordLen = iWordLen-1;
		bPrefix = true;
	}

	if ( !iWordLen )
		return false;

	// prevent prefix matching for explicitly setting prohibited by config, to be on pair with plain index (or CRC kind of index)
	if ( bPrefix && ( ( m_tSettings.GetMinPrefixLen ( m_bKeywordDict ) && iWordLen< m_tSettings.GetMinPrefixLen ( m_bKeywordDict ) )
		|| ( m_tSettings.m_iMinInfixLen && iWordLen< m_tSettings.m_iMinInfixLen ) ) )
		return false;

	// no checkpoints - check all words
	// no checkpoints matched - check only words prior to 1st checkpoint
	// checkpoint found - check words at that checkpoint
	RtWordReader_c tReader ( pCurSeg, m_bKeywordDict, m_iWordsCheckpoint, m_tSettings.m_eHitless );

	if ( pCurSeg->m_dWordCheckpoints.GetLength() )
	{
		const RtWordCheckpoint_t * pCp = m_bKeywordDict
			  ? sphSearchCheckpointWrd ( sWord, iWordLen, false, pCurSeg->m_dWordCheckpoints )
			  : sphSearchCheckpointCrc ( uWordID, pCurSeg->m_dWordCheckpoints );

		const BYTE * pWords = pCurSeg->m_dWords.Begin();

		if ( !pCp )
		{
			tReader.m_pMax = pWords + pCurSeg->m_dWordCheckpoints.Begin()->m_iOffset;
		} else
		{
			tReader.m_pCur = pWords + pCp->m_iOffset;
			// if next checkpoint exists setup reader range
			if ( ( pCp+1 )<= ( &pCurSeg->m_dWordCheckpoints.Last() ) )
				tReader.m_pMax = pWords + pCp[1].m_iOffset;
		}
	}

	// find the word between checkpoints
	if ( m_bKeywordDict )
	{
		int iCmp = 0;
		do {
			if ( !tReader.UnzipWord() )
				return false;
			iCmp = sphDictCmpStrictly ( (const char*)tReader->m_sWord + 1, tReader->m_sWord[0], sWord, iWordLen );
		} while ( iCmp < 0 );

		if ( !!iCmp )
			return false;

		assert ( !iCmp );
		const auto* pWord = (const RtWord_t*)tReader;
		pQword->m_iDocs += pWord->m_uDocs;
		pQword->m_iHits += pWord->m_uHits;
		pQword->m_bHasHitlist &= pWord->m_bHasHitlist;
		if ( bSetup )
			pQword->SetupReader ( pCurSeg, *pWord );

		return true;
	}

	assert ( !m_bKeywordDict );
	do
		if ( !tReader.UnzipWord() )
			return false;
	while ( tReader->m_uWordID < uWordID );

	if ( tReader->m_uWordID>uWordID )
		return false;

	const auto* pWord = (const RtWord_t*)tReader;
	pQword->m_iDocs += pWord->m_uDocs;
	pQword->m_iHits += pWord->m_uHits;
	pQword->m_bHasHitlist &= pWord->m_bHasHitlist;
	if ( bSetup )
		pQword->SetupReader ( pCurSeg, *pWord );

	return true;
}

struct RtExpandedEntry_t
{
	DWORD	m_uHash;
	int		m_iNameOff;
	int		m_iDocs;
	int		m_iHits;
};

struct RtExpandedPayload_t
{
	int		m_iDocs;
	int		m_iHits;
	DWORD	m_uDoclistOff;
};

struct RtExpandedTraits_fn
{
	inline bool IsLess ( const RtExpandedEntry_t & a, const RtExpandedEntry_t & b ) const
	{
		assert ( m_sBase );
		if ( a.m_uHash!=b.m_uHash )
		{
			return a.m_uHash<b.m_uHash;
		} else
		{
			const BYTE * pA = m_sBase + a.m_iNameOff;
			const BYTE * pB = m_sBase + b.m_iNameOff;
			if ( pA[0]!=pB[0] )
				return pA[0]<pB[0];

			return ( sphDictCmp ( (const char *)pA+1, pA[0], (const char *)pB+1, pB[0] )<0 );
		}
	}

	inline bool IsEqual ( const RtExpandedEntry_t * a, const RtExpandedEntry_t * b ) const
	{
		assert ( m_sBase );
		if ( a->m_uHash!=b->m_uHash )
			return false;

		const BYTE * pA = m_sBase + a->m_iNameOff;
		const BYTE * pB = m_sBase + b->m_iNameOff;
		if ( pA[0]!=pB[0] )
			return false;

		return ( sphDictCmp ( (const char *)pA+1, pA[0], (const char *)pB+1, pB[0] )==0 );
	}

	explicit RtExpandedTraits_fn ( const BYTE * sBase )
		: m_sBase ( sBase )
	{ }
	const BYTE * m_sBase;
};


struct DictEntryRtPayload_t
{
	DictEntryRtPayload_t ( bool bPayload, int iSegments )
	{
		m_bPayload = bPayload;
		m_iSegExpansionLimit = iSegments;
		if ( bPayload )
		{
			m_dWordPayload.Reserve ( 1000 );
			m_dSeg.Resize ( iSegments );
			ARRAY_FOREACH ( i, m_dSeg )
			{
				m_dSeg[i].m_uOff = 0;
				m_dSeg[i].m_uLen = 0;
			}
		}

		m_dWordExpand.Reserve ( 1000 );
		m_dWordBuf.Reserve ( 8096 );
	}

	void Add ( const RtWord_t * pWord, int iSegment )
	{
		if ( !m_bPayload || !sphIsExpandedPayload ( pWord->m_uDocs, pWord->m_uHits ) )
		{
			RtExpandedEntry_t & tExpand = m_dWordExpand.Add();

			int iOff = m_dWordBuf.GetLength();
			int iWordLen = pWord->m_sWord[0] + 1;
			tExpand.m_uHash = sphCRC32 ( pWord->m_sWord, iWordLen );
			tExpand.m_iNameOff = iOff;
			tExpand.m_iDocs = pWord->m_uDocs;
			tExpand.m_iHits = pWord->m_uHits;
			m_dWordBuf.Append ( pWord->m_sWord, iWordLen );
		} else
		{
			RtExpandedPayload_t & tExpand = m_dWordPayload.Add();
			tExpand.m_iDocs = pWord->m_uDocs;
			tExpand.m_iHits = pWord->m_uHits;
			tExpand.m_uDoclistOff = pWord->m_uDoc;

			m_dSeg[iSegment].m_uOff = m_dWordPayload.GetLength();
			m_dSeg[iSegment].m_uLen++;
		}
	}

	void Convert ( ISphWordlist::Args_t & tArgs )
	{
		if ( !m_dWordExpand.GetLength() && !m_dWordPayload.GetLength() )
			return;

		int iTotalDocs = 0;
		int iTotalHits = 0;
		if ( !m_dWordExpand.IsEmpty() )
		{
			int iRtExpansionLimit = tArgs.m_iExpansionLimit * m_iSegExpansionLimit;
			if ( tArgs.m_iExpansionLimit && m_dWordExpand.GetLength()>iRtExpansionLimit )
			{
				// sort expansions by frequency desc
				// clip the less frequent ones if needed, as they are likely misspellings
				sphSort ( m_dWordExpand.Begin(), m_dWordExpand.GetLength(), ExpandedOrderDesc_T<RtExpandedEntry_t>() );
				m_dWordExpand.Resize ( iRtExpansionLimit );
			}

			// lets merge statistics for same words from different segments
			// as hash produce a lot tiny allocations here
			const BYTE * sBase = m_dWordBuf.Begin();
			RtExpandedTraits_fn fnCmp ( sBase );
			sphSort ( m_dWordExpand.Begin(), m_dWordExpand.GetLength(), fnCmp );

			const RtExpandedEntry_t * pLast = m_dWordExpand.Begin();
			tArgs.AddExpanded ( sBase+pLast->m_iNameOff+1, sBase[pLast->m_iNameOff], pLast->m_iDocs, pLast->m_iHits );
			iTotalDocs += pLast->m_iDocs;
			iTotalHits += pLast->m_iHits;
			for ( int i=1; i<m_dWordExpand.GetLength(); ++i )
			{
				const RtExpandedEntry_t * pCur = m_dWordExpand.Begin() + i;

				if ( fnCmp.IsEqual ( pLast, pCur ) )
				{
					tArgs.m_dExpanded.Last().m_iDocs += pCur->m_iDocs;
					tArgs.m_dExpanded.Last().m_iHits += pCur->m_iHits;
				} else
				{
					tArgs.AddExpanded ( sBase + pCur->m_iNameOff + 1, sBase[pCur->m_iNameOff],
							pCur->m_iDocs, pCur->m_iHits );
					pLast = pCur;
				}
				iTotalDocs += pCur->m_iDocs;
				iTotalHits += pCur->m_iHits;
			}
		}

		if ( m_dWordPayload.GetLength() )
		{
			DWORD uExpansionLimit = tArgs.m_iExpansionLimit;
			int iPayloads = 0;
			for ( auto& tSeg: m_dSeg )
			{
				// reverse per segment offset to payload doc-list as offset was the end instead of start
				assert ( tSeg.m_uOff>=tSeg.m_uLen );
				tSeg.m_uOff = tSeg.m_uOff - tSeg.m_uLen;

				// per segment expansion limit clip
				if ( uExpansionLimit && tSeg.m_uLen>uExpansionLimit )
				{
					// sort expansions by frequency desc
					// per segment clip the less frequent ones if needed, as they are likely misspellings
					sphSort ( m_dWordPayload.Begin()+tSeg.m_uOff, tSeg.m_uLen,
							ExpandedOrderDesc_T<RtExpandedPayload_t>() );
					tSeg.m_uLen = uExpansionLimit;
				}

				iPayloads += tSeg.m_uLen;
				// sort by ascending doc-list offset
				sphSort ( m_dWordPayload.Begin()+tSeg.m_uOff, tSeg.m_uLen,
						bind ( &RtExpandedPayload_t::m_uDoclistOff ) );
			}

			auto * pPayload = new RtSubstringPayload_t ( m_dSeg.GetLength(), iPayloads );

			Slice_t * pDst = pPayload->m_dDoclist.Begin();
			ARRAY_FOREACH ( i, m_dSeg )
			{
				const Slice_t & tSeg = m_dSeg[i];
				const RtExpandedPayload_t * pSrc = m_dWordPayload.Begin() + tSeg.m_uOff;
				const RtExpandedPayload_t * pEnd = pSrc + tSeg.m_uLen;
				pPayload->m_dSegment2Doclists[i].m_uOff = pPayload->m_dDoclist.Idx(pDst);
				pPayload->m_dSegment2Doclists[i].m_uLen = tSeg.m_uLen;
				while ( pSrc!=pEnd )
				{
					pDst->m_uOff = pSrc->m_uDoclistOff;
					pDst->m_uLen = pSrc->m_iDocs;
					iTotalDocs += pSrc->m_iDocs;
					iTotalHits += pSrc->m_iHits;
					++pDst;
					++pSrc;
				}
			}
			pPayload->m_iTotalDocs = iTotalDocs;
			pPayload->m_iTotalHits = iTotalHits;
			tArgs.m_pPayload = pPayload;
		}

		tArgs.m_iTotalDocs = iTotalDocs;
		tArgs.m_iTotalHits = iTotalHits;
	}

	bool							m_bPayload;
	CSphVector<RtExpandedEntry_t>	m_dWordExpand;
	CSphVector<RtExpandedPayload_t>	m_dWordPayload;
	CSphVector<BYTE>				m_dWordBuf;
	CSphVector<Slice_t>				m_dSeg;
	int								m_iSegExpansionLimit = 0;
};


void RtIndex_c::GetPrefixedWords ( const char * sSubstring, int iSubLen, const char * sWildcard, Args_t & tArgs ) const
{
	int dWildcard [ SPH_MAX_WORD_LEN + 1 ];
	int * pWildcard = ( sphIsUTF8 ( sWildcard ) && sphUTF8ToWideChar ( sWildcard, dWildcard, SPH_MAX_WORD_LEN ) ) ? dWildcard : nullptr;

	const auto& dSegments = *(const RtSegVec_c*)tArgs.m_pIndexData.Ptr();
	DictEntryRtPayload_t tDict2Payload ( tArgs.m_bPayload, dSegments.GetLength() );
	const int iSkipMagic = ( BYTE(*sSubstring)<0x20 ); // whether to skip heading magic chars in the prefix, like NONSTEMMED maker
	ARRAY_FOREACH ( iSeg, dSegments )
	{
		const RtSegment_t * pCurSeg = dSegments[iSeg];
		RtWordReader_c tReader ( pCurSeg, true, m_iWordsCheckpoint, m_tSettings.m_eHitless );

		// find initial checkpoint or check words prior to 1st checkpoint
		if ( pCurSeg->m_dWordCheckpoints.GetLength() )
		{
			const RtWordCheckpoint_t * pCurCheckpoint = sphSearchCheckpointWrd( sSubstring, iSubLen, true, pCurSeg->m_dWordCheckpoints );
			if ( pCurCheckpoint )
			{
				// there could be valid data prior 1st checkpoint that should be unpacked and checked
				auto iCheckpointNameLen = (int) strlen ( pCurCheckpoint->m_sWord );
				if ( pCurCheckpoint!=pCurSeg->m_dWordCheckpoints.Begin()
					|| ( sphDictCmp ( sSubstring, iSubLen, pCurCheckpoint->m_sWord, iCheckpointNameLen )==0 && iSubLen==iCheckpointNameLen ) )
				{
					tReader.m_pCur = pCurSeg->m_dWords.Begin() + pCurCheckpoint->m_iOffset;
				}
			}
		}

		// find the word between checkpoints
		while ( tReader.UnzipWord() )
		{
			const auto* pWord = (const RtWord_t*)tReader;
			int iCmp = sphDictCmp ( sSubstring, iSubLen, (const char *)pWord->m_sWord+1, pWord->m_sWord[0] );
			if ( iCmp<0 )
				break;
			else if ( iCmp==0 && iSubLen<=pWord->m_sWord[0] && sphWildcardMatch ( (const char *)pWord->m_sWord+1+iSkipMagic, sWildcard, pWildcard ) )
				tDict2Payload.Add ( pWord, iSeg );
			// FIXME!!! same case 'boxi*' matches 'box' document at plain index
			// but masked by a checkpoint search
		}
	}

	tDict2Payload.Convert ( tArgs );
}


bool ExtractInfixCheckpoints ( const char * sInfix, int iBytes, int iMaxCodepointLength, int iDictCpCount, const CSphTightVector<uint64_t> & dFilter, CSphVector<DWORD> & dCheckpoints )
{
	if ( !dFilter.GetLength() )
		return false;

	int iStart = dCheckpoints.GetLength();

	uint64_t dVals[ BLOOM_PER_ENTRY_VALS_COUNT * BLOOM_HASHES_COUNT ];
	memset ( dVals, 0, sizeof(dVals) );

	BloomGenTraits_t tBloom0 ( dVals );
	BloomGenTraits_t tBloom1 ( dVals+BLOOM_PER_ENTRY_VALS_COUNT );
	if ( !BuildBloom ( (const BYTE *)sInfix, iBytes, BLOOM_NGRAM_0, ( iMaxCodepointLength>1 ), BLOOM_PER_ENTRY_VALS_COUNT, tBloom0 ) )
		return false;
	BuildBloom ( (const BYTE *)sInfix, iBytes, BLOOM_NGRAM_1, ( iMaxCodepointLength>1 ), BLOOM_PER_ENTRY_VALS_COUNT, tBloom1 );

	for ( int iDictCp=0; iDictCp<iDictCpCount+1; iDictCp++ )
	{
		const uint64_t * pCP = dFilter.Begin() + iDictCp * BLOOM_PER_ENTRY_VALS_COUNT * BLOOM_HASHES_COUNT;
		const uint64_t * pSuffix = dVals;

		bool bMatched = true;
		for ( int iElem=0; iElem<BLOOM_PER_ENTRY_VALS_COUNT*BLOOM_HASHES_COUNT; iElem++ )
		{
			uint64_t uFilter = *pCP++;
			uint64_t uSuffix = *pSuffix++;
			if ( ( uFilter & uSuffix )!=uSuffix )
			{
				bMatched = false;
				break;
			}
		}

		if ( bMatched )
			dCheckpoints.Add ( (DWORD)iDictCp );
	}

	return ( dCheckpoints.GetLength()!=iStart );
}


void RtIndex_c::GetInfixedWords ( const char * sSubstring, int iSubLen, const char * sWildcard, Args_t & tArgs ) const
{
	// sanity checks
	if ( !sSubstring || iSubLen<=0 )
		return;

	// find those prefixes
	CSphVector<DWORD> dPoints;
	const int iSkipMagic = ( tArgs.m_bHasExactForms ? 1 : 0 ); // whether to skip heading magic chars in the prefix, like NONSTEMMED maker
	const auto& dSegments = *(const RtSegVec_c*)tArgs.m_pIndexData.Ptr();

	DictEntryRtPayload_t tDict2Payload ( tArgs.m_bPayload, dSegments.GetLength() );
	ARRAY_FOREACH ( iSeg, dSegments )
	{
		const RtSegment_t * pSeg = dSegments[iSeg];
		if ( !pSeg->m_dWords.GetLength() )
			continue;

		dPoints.Resize ( 0 );
		if ( !ExtractInfixCheckpoints ( sSubstring, iSubLen, m_iMaxCodepointLength, pSeg->m_dWordCheckpoints.GetLength(), pSeg->m_dInfixFilterCP, dPoints ) )
			continue;

		int dWildcard [ SPH_MAX_WORD_LEN + 1 ];
		int * pWildcard = ( sphIsUTF8 ( sWildcard ) && sphUTF8ToWideChar ( sWildcard, dWildcard, SPH_MAX_WORD_LEN ) ) ? dWildcard : nullptr;

		// walk those checkpoints, check all their words
		for ( DWORD uPoint : dPoints )
		{
			auto iNext = (int)uPoint;
			auto iCur = iNext-1;
			RtWordReader_c tReader ( pSeg, true, m_iWordsCheckpoint, m_tSettings.m_eHitless );
			if ( iCur>=0 )
				tReader.m_pCur = pSeg->m_dWords.Begin() + pSeg->m_dWordCheckpoints[iCur].m_iOffset;
			if ( iNext<pSeg->m_dWordCheckpoints.GetLength() )
				tReader.m_pMax = pSeg->m_dWords.Begin() + pSeg->m_dWordCheckpoints[iNext].m_iOffset;

			while ( tReader.UnzipWord() )
			{
				if ( tArgs.m_bHasExactForms && tReader->m_sWord[1]!=MAGIC_WORD_HEAD_NONSTEMMED )
					continue;

				// check it
				if ( !sphWildcardMatch ( (const char*)tReader->m_sWord+1+iSkipMagic, sWildcard, pWildcard ) )
					continue;

				// matched, lets add
				tDict2Payload.Add ( (const RtWord_t*)tReader, iSeg );
			}
		}
	}

	tDict2Payload.Convert ( tArgs );
}

void RtIndex_c::GetSuggest ( const SuggestArgs_t & tArgs, SuggestResult_t & tRes ) const
{
	RtGuard_t tGuard ( m_tRtChunks.RtData() );
	const auto& dSegments = tGuard.m_dRamSegs;

	// segments and disk chunks dictionaries produce duplicated entries
	tRes.m_bMergeWords = true;

	if ( dSegments.GetLength() )
	{
		assert ( !tRes.m_pWordReader && !tRes.m_pSegments );
		CSphScopedPtr<RtWordReader_c> pReader { new RtWordReader_c ( dSegments[0], true, m_iWordsCheckpoint, m_tSettings.m_eHitless ) };
		tRes.m_pWordReader = pReader.Ptr();
		tRes.m_pSegments = tGuard.m_tSegmentsAndChunks.m_pSegs;
		tRes.m_bHasExactDict = m_tSettings.m_bIndexExactWords;

		// FIXME!!! cache InfixCodepointBytes as it is slow - GetMaxCodepointLength is charset_table traverse
		sphGetSuggest ( this, m_pTokenizer->GetMaxCodepointLength(), tArgs, tRes );

		tRes.m_pWordReader = nullptr;
		tRes.m_pSegments = nullptr;
	}

	int iWorstCount = 0;
	auto& dDiskChunks = tGuard.m_dDiskChunks;
	// check disk chunks from recent to oldest
	for ( int i = dDiskChunks.GetLength() - 1; i >= 0; --i )
	{
		int iWorstDist = 0;
		int iWorstDocs = 0;
		if ( tRes.m_dMatched.GetLength() )
		{
			iWorstDist = tRes.m_dMatched.Last().m_iDistance;
			iWorstDocs = tRes.m_dMatched.Last().m_iDocs;
		}

		dDiskChunks[i]->Cidx().GetSuggest ( tArgs, tRes );

		// stop checking in case worst element is same several times during loop
		if ( tRes.m_dMatched.GetLength() && iWorstDist==tRes.m_dMatched.Last().m_iDistance && iWorstDocs==tRes.m_dMatched.Last().m_iDocs )
		{
			iWorstCount++;
			if ( iWorstCount>2 )
				break;
		} else
		{
			iWorstCount = 0;
		}
	}
}

void RtIndex_c::SuffixGetChekpoints ( const SuggestResult_t & tRes, const char * sSuffix, int iLen, CSphVector<DWORD> & dCheckpoints ) const
{
	const auto& dSegments = *(const RtSegVec_c*)tRes.m_pSegments.Ptr();
	assert ( dSegments.GetLength()<0xFF );

	ARRAY_FOREACH ( iSeg, dSegments )
	{
		const RtSegment_t * pSeg = dSegments[iSeg];
		if ( !pSeg->m_dWords.GetLength () )
			continue;

		int iStart = dCheckpoints.GetLength();
		if ( !ExtractInfixCheckpoints ( sSuffix, iLen, m_iMaxCodepointLength, pSeg->m_dWordCheckpoints.GetLength(), pSeg->m_dInfixFilterCP, dCheckpoints ) )
			continue;

		DWORD iSegPacked = (DWORD)iSeg<<24;
		for ( int i=iStart; i<dCheckpoints.GetLength(); i++ )
		{
			assert ( ( dCheckpoints[i] & 0xFFFFFF )==dCheckpoints[i] );
			dCheckpoints[i] |= iSegPacked;
		}
	}
}

void RtIndex_c::SetCheckpoint ( SuggestResult_t & tRes, DWORD iCP ) const
{
	assert ( tRes.m_pWordReader && tRes.m_pSegments );
	const auto& dSegments = *(const RtSegVec_c*)tRes.m_pSegments.Ptr();
	auto* pReader = (RtWordReader_c*)tRes.m_pWordReader;

	int iSeg = iCP>>24;
	assert ( iSeg>=0 && iSeg<dSegments.GetLength() );
	const RtSegment_t * pSeg = dSegments[iSeg];
	pReader->Reset ( pSeg );

	int iNext = (int)( iCP & 0xFFFFFF );
	int iCur = iNext-1;

	if ( iCur>0 )
		pReader->m_pCur = pSeg->m_dWords.Begin() + pSeg->m_dWordCheckpoints[iCur].m_iOffset;
	if ( iNext<pSeg->m_dWordCheckpoints.GetLength() )
		pReader->m_pMax = pSeg->m_dWords.Begin() + pSeg->m_dWordCheckpoints[iNext].m_iOffset;
}

bool RtIndex_c::ReadNextWord ( SuggestResult_t & tRes, DictWord_t & tWord ) const
{
	assert ( tRes.m_pWordReader );
	auto& tReader = *(RtWordReader_c*)tRes.m_pWordReader;

	if ( !tReader.UnzipWord() )
		return false;

	const auto* pWord = (const RtWord_t*)tReader;
	tWord.m_sWord = (const char *)( pWord->m_sWord + 1 );
	tWord.m_iLen = pWord->m_sWord[0];
	tWord.m_iDocs = pWord->m_uDocs;
	return true;
}


bool RtIndex_c::RtQwordSetup ( RtQword_t * pQword, int iSeg, const RtGuard_t& tGuard ) const
{
	// segment-specific setup pass
	if ( iSeg>=0 )
		return RtQwordSetupSegment ( pQword, tGuard.m_dRamSegs[iSeg], true );

	// stat-only pass
	// loop all segments, gather stats, do not setup anything
	pQword->m_iDocs = 0;
	pQword->m_iHits = 0;
	if ( tGuard.m_dRamSegs.IsEmpty() )
		return true;

	// we care about the results anyway though
	// because if all (!) segments miss this word, we must notify the caller, right?
	bool bFound = false;
	for ( const auto& pSeg : tGuard.m_dRamSegs )
		bFound |= RtQwordSetupSegment ( pQword, pSeg, false );

	// sanity check
	assert (!( bFound==true && pQword->m_iDocs==0 ) );
	return bFound;
}


void SetupExactDict ( DictRefPtr_c &pDict, ISphTokenizer * pTokenizer, bool bAddSpecial )
{
	assert ( pTokenizer );
	pTokenizer->AddPlainChar ( '=' );
	if ( bAddSpecial )
		pTokenizer->AddSpecials ( "=" );
	pDict = new CSphDictExact ( pDict );
}


void SetupStarDict ( DictRefPtr_c& pDict, ISphTokenizer * pTokenizer )
{
	assert ( pTokenizer );
	pTokenizer->AddPlainChar ( '*' );
	pDict = new CSphDictStarV8 ( pDict, true );
}

class SphRtFinalMatchCalc_c : public MatchProcessor_i, ISphNoncopyable
{
public:
	SphRtFinalMatchCalc_c ( int iSegments, const CSphQueryContext & tCtx )
		: m_tCtx ( tCtx )
		, m_iSegments ( iSegments )
	{
		m_dSegments.Init ( iSegments );
	}

	bool NextSegment ( int iSeg )
	{
		m_iSeg = iSeg;

		bool bSegmentGotRows = m_dSegments.BitGet ( iSeg );

		// clear current row
		m_dSegments.BitClear ( iSeg );

		// also clear 0 segment as it got forced to process
		m_dSegments.BitClear ( 0 );

		// also force to process 0 segment to mark all used segments
		return ( iSeg==0 || bSegmentGotRows );
	}

	bool HasSegments () const							{ return ( m_iSeg==0 || m_dSegments.BitCount()>0 );	}
	void Process ( CSphMatch * pMatch ) final			{ ProcessMatch ( pMatch ); }
	void Process ( VecTraits_T<CSphMatch *> & dMatches ) final { dMatches.for_each ( [this]( CSphMatch * pMatch ){ ProcessMatch(pMatch); } ); }
	bool ProcessInRowIdOrder() const final				{ return m_tCtx.m_dCalcFinal.any_of ( []( const CSphQueryContext::CalcItem_t & i ){ return i.m_pExpr && i.m_pExpr->IsColumnar(); } );	}

private:
	const CSphQueryContext &	m_tCtx;
	int							m_iSeg = 0;
	int							m_iSegments;
	// count per segments matches
	// to skip iteration of matches at sorter and pool setup for segment without matches at sorter
	CSphBitvec					m_dSegments;

	inline void ProcessMatch ( CSphMatch * pMatch )
	{
		int iMatchSegment = pMatch->m_iTag-1;
		if ( iMatchSegment==m_iSeg )
			m_tCtx.CalcFinal ( *pMatch );

		// count all used segments at 0 pass
		if ( m_iSeg==0 && iMatchSegment<m_iSegments )
			m_dSegments.BitSet ( iMatchSegment );
	}
};


class SorterSchemaTransform_c
{
public:
			explicit SorterSchemaTransform_c ( int iNumChunks ) { m_dDiskChunkData.Resize(iNumChunks); }

	void	Set ( int iChunk, const CSphQueryResult & tChunkResult ) { m_dDiskChunkData[iChunk].Set(tChunkResult); }
	void	Transform ( ISphMatchSorter * pSorter, const RtGuard_t& tGuard );

private:
	struct DiskChunkData_t
	{
		const BYTE *			m_pBlobPool;
		columnar::Columnar_i *	m_pColumnar;

		void Set ( const CSphQueryResult & tChunkResult )
		{
			m_pBlobPool = tChunkResult.m_pBlobPool;
			m_pColumnar = tChunkResult.m_pColumnar;
		}
	};

	CSphVector<DiskChunkData_t>	m_dDiskChunkData;
};


void SorterSchemaTransform_c::Transform ( ISphMatchSorter * pSorter, const RtGuard_t& tGuard )
{
	assert(pSorter);

	CSphBitvec tLockedSegs ( tGuard.m_dRamSegs.GetLength() );
	auto tUnlockChunks = AtScopeExit ( [&tGuard, &tLockedSegs]() NO_THREAD_SAFETY_ANALYSIS {
		ARRAY_FOREACH ( i, tGuard.m_dRamSegs )
			if ( tLockedSegs.BitGet ( i ) )
				tGuard.m_dRamSegs[i]->m_tLock.Unlock();
	});

	auto fnGetBlobPoolFromMatch = [&tGuard,&tLockedSegs,this] ( const CSphMatch * pMatch ) NO_THREAD_SAFETY_ANALYSIS
	{
		int nRamChunks = tGuard.m_dRamSegs.GetLength ();
		int iChunkId = pMatch->m_iTag-1;
		if ( iChunkId>=nRamChunks )
			return m_dDiskChunkData[iChunkId - nRamChunks].m_pBlobPool;

		if ( !tLockedSegs.BitGet ( iChunkId ) )
		{
			tLockedSegs.BitSet ( iChunkId );
			tGuard.m_dRamSegs[iChunkId]->m_tLock.ReadLock();
		}
		return (const BYTE *) tGuard.m_dRamSegs[iChunkId]->m_dBlobs.Begin();
	};

	auto fnGetColumnarFromMatch = [&tGuard,this] ( const CSphMatch * pMatch ) -> columnar::Columnar_i *
	{
		int nRamChunks = tGuard.m_dRamSegs.GetLength ();
		int iChunkId = pMatch->m_iTag-1;
		if ( iChunkId<nRamChunks )
			return tGuard.m_dRamSegs[iChunkId]->m_pColumnar.Ptr();

		return m_dDiskChunkData[iChunkId-nRamChunks].m_pColumnar;
	};

	pSorter->TransformPooled2StandalonePtrs ( fnGetBlobPoolFromMatch, fnGetColumnarFromMatch );
}


static void QueryDiskChunks ( const CSphQuery & tQuery, CSphQueryResultMeta & tResult, const CSphMultiQueryArgs & tArgs, const RtGuard_t & tGuard, VecTraits_T<ISphMatchSorter *> & dSorters, QueryProfile_c * pProfiler, bool bGotLocalDF, const SmallStringHash_T<int64_t> * pLocalDocs, int64_t iTotalDocs, const char * szIndexName, SorterSchemaTransform_c & tSSTransform, int64_t tmMaxTimer )
{
	// counter of tasks we will issue now
	int iJobs = tGuard.m_dDiskChunks.GetLength();
	if ( !iJobs )
		return;

	assert ( !dSorters.IsEmpty () );

	// the context
	ClonableCtx_T<DiskChunkSearcherCtx_t, DiskChunkSearcherCloneCtx_t> tClonableCtx { dSorters, tResult };

	auto iConcurrency = tQuery.m_iCouncurrency;
	if ( !iConcurrency )
		iConcurrency = GetEffectiveDistThreads();

	tClonableCtx.LimitConcurrency ( iConcurrency );

	auto iStart = sphMicroTimer();
	sphLogDebugv ( "Started: " INT64_FMT, sphMicroTimer()-iStart );

	// because disk chunk search within the loop will switch the profiler state
	SwitchProfile ( pProfiler, SPH_QSTATE_INIT );

	std::atomic<bool> bInterrupt {false};
	std::atomic<int32_t> iCurChunk { iJobs-1 };
	CoExecuteN ( tClonableCtx.Concurrency ( iJobs ), [&]
	{
		auto iChunk = iCurChunk.fetch_sub ( 1, std::memory_order_acq_rel );
		if ( iChunk<0 || bInterrupt )
			return; // already nothing to do, early finish.

		auto tCtx = tClonableCtx.CloneNewContext();
		Threads::CoThrottler_c tThrottler ( session::ThrottlingPeriodMS () );
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
			// storing index in matches tag for finding strings attrs offset later, biased against default zero and segments
			tMultiArgs.m_iTag = tGuard.m_dRamSegs.GetLength ()+iChunk+1;
			tMultiArgs.m_uPackedFactorFlags = tArgs.m_uPackedFactorFlags;
			tMultiArgs.m_bLocalDF = bGotLocalDF;
			tMultiArgs.m_pLocalDocs = pLocalDocs;
			tMultiArgs.m_iTotalDocs = iTotalDocs;

			// we use sorters in both disk chunks and ram chunks,
			// that's why we don't want to move to a new schema before we searched ram chunks
			tMultiArgs.m_bModifySorterSchemas = false;

			bInterrupt = !tGuard.m_dDiskChunks[iChunk]->Cidx().MultiQuery ( tChunkResult, tQuery, dLocalSorters, tMultiArgs );

			// check terms inconsistency among disk chunks
			tCtx.m_tMeta.MergeWordStats ( tChunkMeta );

			tSSTransform.Set ( iChunk, tChunkResult );

			if ( tThMeta.m_bHasPrediction )
				tThMeta.m_tStats.Add ( tChunkMeta.m_tStats );

			if ( iChunk && tmMaxTimer>0 && sph::TimeExceeded ( tmMaxTimer ) )
			{
				tThMeta.m_sWarning = "query time exceeded max_query_time";
				bInterrupt = true;
			}

			if ( tThMeta.m_sWarning.IsEmpty() && !tChunkMeta.m_sWarning.IsEmpty() )
				tThMeta.m_sWarning = tChunkMeta.m_sWarning;

			if ( bInterrupt && !tChunkMeta.m_sError.IsEmpty() )
				// FIXME? maybe handle this more gracefully (convert to a warning)?
				tThMeta.m_sError = tChunkMeta.m_sError;

			iChunk = iCurChunk.fetch_sub ( 1, std::memory_order_acq_rel );
			if ( iChunk<0 || bInterrupt )
				return; // all is done

			// yield and reschedule every quant of time. It gives work to other tasks
			if ( tThrottler.ThrottleAndKeepCrashQuery() )
				// report current disk chunk processing
				++iTick;
		}
	});

	tClonableCtx.Finalize();
}


void FinalExpressionCalculation ( CSphQueryContext & tCtx, const VecTraits_T<RtSegmentRefPtf_t> & dRamChunks, VecTraits_T<ISphMatchSorter *> & dSorters )
{
	if ( tCtx.m_dCalcFinal.IsEmpty() )
		return;

	const int iSegmentsTotal = dRamChunks.GetLength ();

	// at 0 pass processor also fills bitmask of segments these has matches at sorter
	// then skip sorter processing for these 'empty' segments
	SphRtFinalMatchCalc_c tFinal ( iSegmentsTotal, tCtx );

	ARRAY_FOREACH_COND ( iSeg, dRamChunks, tFinal.HasSegments() )
	{
		if ( !tFinal.NextSegment ( iSeg ) )
			continue;

		// set blob pool for string on_sort expression fix up
		SccRL_t rLock ( dRamChunks[iSeg]->m_tLock );
		tCtx.SetBlobPool ( dRamChunks[iSeg]->m_dBlobs.Begin() );
		tCtx.SetColumnar ( dRamChunks[iSeg]->m_pColumnar.Ptr() );

		dSorters.Apply ( [&tFinal] ( ISphMatchSorter * pTop ) { pTop->Finalize ( tFinal, false ); } );
	}
}

// perform initial query transformations and expansion.
static int PrepareFTSearch ( const RtIndex_c * pThis, bool bIsStarDict, bool bKeywordDict, int iExpandKeywords, int iExpansionLimit, const char * szModifiedQuery, const CSphIndexSettings & tSettings, const QueryParser_i * pQueryParser, const CSphQuery & tQuery, const CSphSchema & tSchema, cRefCountedRefPtr_t pIndexData, ISphTokenizer * pTokenizer, ISphTokenizer * pQueryTokenizer, CSphDict * pDict, CSphQueryResultMeta & tMeta, QueryProfile_c * pProfiler, CSphScopedPayload * pPayloads, XQQuery_t & tParsed )
{
	// OPTIMIZE! make a lightweight clone here? and/or remove double clone?
	TokenizerRefPtr_c pQueryTokenizerJson { pTokenizer };
	sphSetupQueryTokenizer ( pQueryTokenizerJson, bIsStarDict, tSettings.m_bIndexExactWords, true );

	if ( !pQueryParser->ParseQuery ( tParsed, szModifiedQuery, &tQuery, pQueryTokenizer, pQueryTokenizerJson, &tSchema, pDict, tSettings ) )
	{
		tMeta.m_sError = tParsed.m_sParseError;
		return 0;
	}

	if ( !tParsed.m_sParseWarning.IsEmpty () )
		tMeta.m_sWarning = tParsed.m_sParseWarning;

	// transform query if needed (quorum transform, etc.)
	SwitchProfile ( pProfiler, SPH_QSTATE_TRANSFORMS );

	// FIXME!!! provide segments list instead index
	sphTransformExtendedQuery ( &tParsed.m_pRoot, tSettings, tQuery.m_bSimplify, pThis );

	int iExpandKw = ExpandKeywords ( iExpandKeywords, tQuery.m_eExpandKeywords, tSettings, bKeywordDict );
	if ( iExpandKw!=KWE_DISABLED )
	{
		sphQueryExpandKeywords ( &tParsed.m_pRoot, tSettings, iExpandKw, bKeywordDict );
		tParsed.m_pRoot->Check ( true );
	}

	// this should be after keyword expansion
	TransformAotFilter ( tParsed.m_pRoot, pDict->GetWordforms (), tSettings );

	// expanding prefix in word dictionary case
	if ( bKeywordDict && bIsStarDict )
	{
		ExpansionContext_t tExpCtx;
		tExpCtx.m_pWordlist = pThis;
		tExpCtx.m_pBuf = nullptr;
		tExpCtx.m_pResult = &tMeta;
		tExpCtx.m_iMinPrefixLen = tSettings.GetMinPrefixLen ( bKeywordDict );
		tExpCtx.m_iMinInfixLen = tSettings.m_iMinInfixLen;
		tExpCtx.m_iExpansionLimit = iExpansionLimit;
		tExpCtx.m_bHasExactForms = ( pDict->HasMorphology () || tSettings.m_bIndexExactWords );
		tExpCtx.m_bMergeSingles = ( tQuery.m_uDebugFlags & QUERY_DEBUG_NO_PAYLOAD )==0;
		tExpCtx.m_pPayloads = pPayloads;
		tExpCtx.m_pIndexData = std::move ( pIndexData );

		tParsed.m_pRoot = sphExpandXQNode ( tParsed.m_pRoot, tExpCtx ); // here magics happens
	}

	return ConsiderStack ( tParsed.m_pRoot, tMeta.m_sError );
}


bool SetupFilters ( const CSphQuery & tQuery, const ISphSchema * pSchema, bool bFullscan, CSphQueryContext & tCtx, CSphString & sError, CSphString & sWarning )
{
	CreateFilterContext_t tFlx;
	tFlx.m_pFilters = &tQuery.m_dFilters;
	tFlx.m_pFilterTree = &tQuery.m_dFilterTree;
	tFlx.m_pSchema = pSchema;
	tFlx.m_eCollation = tQuery.m_eCollation;
	tFlx.m_bScan = bFullscan;

	return tCtx.CreateFilters ( tFlx, sError, sWarning );
}


void PerformFullScan ( const VecTraits_T<RtSegmentRefPtf_t> & dRamChunks, int iMaxDynamicSize, int iIndexWeight, int iStride, int iCutoff, int64_t tmMaxTimer, QueryProfile_c * pProfiler, CSphQueryContext & tCtx, VecTraits_T<ISphMatchSorter*> & dSorters, CSphString & sWarning )
{
	bool bRandomize = dSorters[0]->IsRandom();

	SwitchProfile ( pProfiler, SPH_QSTATE_FULLSCAN );

	// full scan
	// FIXME? OPTIMIZE? add shortcuts here too?
	CSphMatch tMatch;
	tMatch.Reset ( iMaxDynamicSize );
	tMatch.m_iWeight = iIndexWeight;

	ARRAY_FOREACH ( iSeg, dRamChunks )
	{
		RtSegment_t & tSeg = *dRamChunks[iSeg];
		SccRL_t rLock ( tSeg.m_tLock );
		auto pBlobs = tSeg.m_dBlobs.Begin();
		tCtx.SetBlobPool(pBlobs);
		for ( auto * pSorter : dSorters )
			pSorter->SetBlobPool(pBlobs);

		auto pColumnar = tSeg.m_pColumnar.Ptr();
		tCtx.SetColumnar(pColumnar);
		for ( auto * pSorter : dSorters )
			pSorter->SetColumnar(pColumnar);

		if ( tCtx.m_pFilter )
			tCtx.m_pFilter->SetColumnar(pColumnar);

		for ( auto tRowID : RtLiveRows_c(tSeg) )
		{
			tMatch.m_tRowID = tRowID;
			tMatch.m_pStatic = tSeg.m_dRows.Begin() + (int64_t)tRowID*iStride;

			tCtx.CalcFilter ( tMatch );
			if ( tCtx.m_pFilter && !tCtx.m_pFilter->Eval ( tMatch ) )
			{
				tCtx.FreeDataFilter ( tMatch );
				continue;
			}

			if ( bRandomize )
				tMatch.m_iWeight = ( sphRand() & 0xffff ) * iIndexWeight;

			tCtx.CalcSort ( tMatch );

			// storing segment in matches tag for finding strings attrs offset later, biased against default zero
			tMatch.m_iTag = iSeg+1;

			bool bNewMatch = false;
			for ( auto * pSorter: dSorters )
				bNewMatch |= pSorter->Push ( tMatch );

			// stringptr expressions should be duplicated (or taken over) at this point
			tCtx.FreeDataFilter ( tMatch );
			tCtx.FreeDataSort ( tMatch );

			// handle cutoff
			if ( bNewMatch )
				if ( --iCutoff==0 )
					break;

			// handle timer
			if ( tmMaxTimer && sph::TimeExceeded ( tmMaxTimer ) )
			{
				sWarning = "query time exceeded max_query_time";
				iSeg = dRamChunks.GetLength() - 1;	// outer break
				break;
			}
		}

		if ( !iCutoff )
			break;
	}
}


static bool DoFullScanQuery ( const RtSegVec_c & dRamChunks, const ISphSchema & tMaxSorterSchema, const CSphQuery & tQuery, int iIndexWeight, int iStride, int64_t tmMaxTimer, QueryProfile_c * pProfiler, CSphQueryContext & tCtx, VecTraits_T<ISphMatchSorter*> & dSorters, CSphQueryResultMeta & tMeta )
{
	// probably redundant, but just in case
	SwitchProfile ( pProfiler, SPH_QSTATE_INIT );

	// search segments no looking to max_query_time
	// FIXME!!! move searching at segments before disk chunks as result set is safe with kill-lists
	if ( !dRamChunks.IsEmpty () )
	{
		if ( !SetupFilters ( tQuery, &tMaxSorterSchema, true, tCtx, tMeta.m_sError, tMeta.m_sWarning ) )
			return false;
		// FIXME! OPTIMIZE! check if we can early reject the whole index

		// do searching
		int iCutoff = tQuery.m_iCutoff;
		if ( iCutoff<=0 )
			iCutoff = -1;

		PerformFullScan ( dRamChunks, tMaxSorterSchema.GetDynamicSize(), iIndexWeight, iStride, iCutoff, tmMaxTimer, pProfiler, tCtx, dSorters, tMeta.m_sWarning );
	}

	FinalExpressionCalculation ( tCtx, dRamChunks, dSorters );
	return true;
}


static void PerformFullTextSearch ( const RtSegVec_c & dRamChunks, RtQwordSetup_t & tTermSetup, ISphRanker * pRanker, int iIndexWeight, int iCutoff, QueryProfile_c * pProfiler, CSphQueryContext & tCtx, VecTraits_T<ISphMatchSorter*> & dSorters )
{
	bool bRandomize = dSorters[0]->IsRandom();
	// query matching
	ARRAY_FOREACH ( iSeg, dRamChunks )
	{
		const RtSegment_t * pSeg = dRamChunks[iSeg];
		SccRL_t rLock ( pSeg->m_tLock );
		SwitchProfile ( pProfiler, SPH_QSTATE_INIT_SEGMENT );

		tTermSetup.SetSegment ( iSeg );
		pRanker->Reset ( tTermSetup );

		// for lookups to work
		tCtx.m_pIndexData = pSeg;

		// set blob pool for string on_sort expression fix up
		const BYTE * pBlobPool = pSeg->m_dBlobs.Begin ();
		tCtx.SetBlobPool ( pBlobPool );
		for ( auto * pSorter : dSorters )
			pSorter->SetBlobPool ( pBlobPool );

		auto pColumnar = pSeg->m_pColumnar.Ptr();
		tCtx.SetColumnar(pColumnar);
		for ( auto * pSorter : dSorters )
			pSorter->SetColumnar(pColumnar);

		if ( tCtx.m_pFilter )
			tCtx.m_pFilter->SetColumnar(pColumnar);

		pRanker->ExtraData ( EXTRA_SET_BLOBPOOL, (void**)&pBlobPool );

		CSphMatch * pMatch = pRanker->GetMatchesBuffer();
		while (true)
		{
			// ranker does profile switches internally in GetMatches()
			int iMatches = pRanker->GetMatches();
			if ( iMatches<=0 )
				break;

			SwitchProfile ( pProfiler, SPH_QSTATE_SORT );

			for ( int i=0; i<iMatches; i++ )
			{
				CSphMatch & tMatch = pMatch[i];

				tMatch.m_pStatic = pSeg->GetDocinfoByRowID ( tMatch.m_tRowID );
				tMatch.m_iWeight *= iIndexWeight;
				if ( bRandomize )
					tMatch.m_iWeight = ( sphRand() & 0xffff ) * iIndexWeight;

				tCtx.CalcSort ( tMatch );

				if ( tCtx.m_pWeightFilter && !tCtx.m_pWeightFilter->Eval ( tMatch ) )
				{
					tCtx.FreeDataSort ( tMatch );
					continue;
				}

				// storing segment in matches tag for finding strings attrs offset later, biased against default zero
				tMatch.m_iTag = iSeg+1;

				bool bNewMatch = false;
				for ( auto* pSorter : dSorters )
				{
					bNewMatch |= pSorter->Push ( tMatch );

					if ( tCtx.m_uPackedFactorFlags & SPH_FACTOR_ENABLE )
					{
						RowID_t tJustPushed = pSorter->GetJustPushed();
						VecTraits_T<RowID_t> dJustPopped = pSorter->GetJustPopped();
						pRanker->ExtraData ( EXTRA_SET_MATCHPUSHED, (void**)&tJustPushed );
						pRanker->ExtraData ( EXTRA_SET_MATCHPOPPED, (void**)&dJustPopped );
					}
				}

				// stringptr expressions should be duplicated (or taken over) at this point
				tCtx.FreeDataFilter ( tMatch );
				tCtx.FreeDataSort ( tMatch );

				if ( bNewMatch )
					if ( --iCutoff==0 )
						break;
			}

			if ( iCutoff==0 )
			{
				iSeg = dRamChunks.GetLength();
				break;
			}
		}
	}
}

static void DumpScore(ISphRanker& pRanker, int iMatchPoolSize) {

  const auto iMatches = pRanker.GetMatches();
  auto* pMatches = pRanker.GetMatchesBuffer();
  auto count = std::min(iMatches, iMatchPoolSize);

  for (auto i=0; i<count; i++ )
  {
    CSphMatch & tMatch = pMatches[i];
//    BYTE* feature = pRanker.GetTextFeatures(tMatch);
//    pRanker->ExtraData ( EXTRA_SET_MATCHPOPPED, (void**)&dJustPopped );
//    tMatch.SetAttr()

  }
}

static bool DoFullTextSearch ( const RtSegVec_c & dRamChunks, const ISphSchema & tMaxSorterSchema, const CSphQuery & tQuery, const char * szIndexName, int iIndexWeight, int iMatchPoolSize, int iStackNeed, RtQwordSetup_t & tTermSetup, QueryProfile_c * pProfiler, CSphQueryContext & tCtx, VecTraits_T<ISphMatchSorter*> & dSorters, XQQuery_t & tParsed, CSphQueryResultMeta & tMeta, ISphMatchSorter * pSorter )
{
	// set zonespanlist settings
	tParsed.m_bNeedSZlist = tQuery.m_bZSlist;

	return Threads::CoContinueBool ( iStackNeed, [&] {

	// setup query
	// must happen before index-level reject, in order to build proper keyword stats
	CSphScopedPtr<ISphRanker> pRanker ( nullptr );
	pRanker = sphCreateRanker ( tParsed, tQuery, tMeta, tTermSetup, tCtx, tMaxSorterSchema );
	if ( !pRanker.Ptr () )
		return false;

	tCtx.SetupExtraData ( pRanker.Ptr (), pSorter );

	pRanker->ExtraData ( EXTRA_SET_POOL_CAPACITY, (void **) &iMatchPoolSize );

	// check for the possible integer overflow in m_dPool.Resize
	int64_t iPoolSize = 0;
	if ( pRanker->ExtraData ( EXTRA_GET_POOL_SIZE, (void **) &iPoolSize ) && iPoolSize>INT_MAX )
	{
		tMeta.m_sError.SetSprintf ( "ranking factors pool too big (%d Mb), reduce max_matches",
				(int) ( iPoolSize / 1024 / 1024 ) );
		return false;
	}

	// probably redundant, but just in case
	SwitchProfile ( pProfiler, SPH_QSTATE_INIT );

	// search segments no looking to max_query_time
	// FIXME!!! move searching at segments before disk chunks as result set is safe with kill-lists
	if ( !dRamChunks.IsEmpty () )
	{
		if ( !SetupFilters ( tQuery, &tMaxSorterSchema, false, tCtx, tMeta.m_sError, tMeta.m_sWarning ) )
			return false;
		// FIXME! OPTIMIZE! check if we can early reject the whole index

		// do searching
		int iCutoff = tQuery.m_iCutoff;
		if ( iCutoff<=0 )
			iCutoff = -1;
		PerformFullTextSearch ( dRamChunks, tTermSetup, pRanker.Ptr (), iIndexWeight, iCutoff, pProfiler, tCtx, dSorters );
	}

	FinalExpressionCalculation ( tCtx, dRamChunks, dSorters );

	//////////////////////
	// copying match's attributes to external storage in result set
	//////////////////////

    if(pRanker->CanExportTextFeatures()) {
      DumpScore(*pRanker, iMatchPoolSize);
    }

	SwitchProfile ( pProfiler, SPH_QSTATE_FINALIZE );
	pRanker->FinalizeCache ( tMaxSorterSchema );
	return true;
	});
}

// Extract subset of chunks/segments for ops with list of chunk, i.e. for 'select .. from rt.0.2.4'
// if dChunks is empty (i.e. no subset required) - returns whole tOrigin, i.e. all chunks/segments
// if tOrigin has 3 disk chunks + 2 ram chunks, as {0d,1d,2d;0r,1r} and dChunks is {0,2,4} -
// result will have 2 disk chunks + 1 ram chunk, as {0d,2d;1r}
ConstRtData FilterReaderChunks ( ConstRtData tOrigin, const VecTraits_T<int64_t>& dChunks )
{
	if ( dChunks.IsEmpty() )
		return tOrigin;

	CSphVector<int> dOrderedChunks;
	dChunks.for_each ( [&dOrderedChunks] ( int64_t iVal ) { dOrderedChunks.Add ( (int) iVal ); } );
	dOrderedChunks.Uniq(); // implies also Sort()

	auto iDiskBound = tOrigin.m_pChunks->GetLength();
	auto iAllBound = iDiskBound + tOrigin.m_pSegs->GetLength();
	int iDiskSelected = 0;
	int iRamSelected = 0;
	dOrderedChunks.any_of ( [&] ( int iVal )
	{
		if ( iVal<iDiskBound )
		{
			++iDiskSelected;
			return false;
		}
		if ( iVal<iAllBound )
		{
			++iRamSelected;
			return false;
		}
		return true;
	});

	auto pChunks = new DiskChunkVec_c;
	pChunks->Resize ( iDiskSelected );
	for ( int i = 0; i<iDiskSelected; ++i )
		( *pChunks )[i] = ( *tOrigin.m_pChunks )[dOrderedChunks[i]];

	auto pSegments = new RtSegVec_c;
	pSegments->Resize ( iRamSelected );
	for ( int i = 0; i<iRamSelected; ++i )
		( *pSegments )[i] = ( *tOrigin.m_pSegs )[dOrderedChunks[iDiskSelected+i]-iDiskBound];

	ConstDiskChunkVecRefPtr_t pConstChunks;
	ConstRtSegVecRefPtr_t pConstSegments;
	pConstChunks = pChunks;
	pConstSegments = pSegments;
	return { pConstChunks, pConstSegments };
}

// FIXME! missing MVA, index_exact_words support
// FIXME? any chance to factor out common backend agnostic code?
// FIXME? do we need to support pExtraFilters?
bool RtIndex_c::MultiQuery ( CSphQueryResult & tResult, const CSphQuery & tQuery, const VecTraits_T<ISphMatchSorter *> & dAllSorters, const CSphMultiQueryArgs & tArgs ) const
{
	// to avoid the checking of a ppSorters's element for NULL on every next step,
	// just filter out all nulls right here
	CSphVector<ISphMatchSorter*> dSorters;
	dSorters.Reserve ( dAllSorters.GetLength() );
	dAllSorters.Apply ([&dSorters] ( ISphMatchSorter* p ) { if ( p ) dSorters.Add(p); });
	auto& tMeta = *tResult.m_pMeta;

	// if we have anything to work with
	if ( dSorters.IsEmpty() )
	{
		tMeta.m_iQueryTime = 0;
		return false;
	}

	assert ( tArgs.m_iTag==0 );

	MEMORY ( MEM_RT_QUERY );

	// start counting
	tMeta.m_iQueryTime = 0;
	int64_t tmQueryStart = sphMicroTimer();
	auto tmCpuQueryStart = sphTaskCpuTimer();
	QueryProfile_c * pProfiler = tMeta.m_pProfile;
	CSphScopedProfile tProf ( pProfiler, SPH_QSTATE_DICT_SETUP );

	// force ext2 mode for them
	// FIXME! eliminate this const breakage
	const_cast<CSphQuery*> ( &tQuery )->m_eMode = SPH_MATCH_EXTENDED2;

	auto tRtData = m_tRtChunks.RtData();

	// debug hack (don't use ram chunk in debug modeling mode)
	if_const( MODELING )
		tRtData.m_pSegs = new RtSegVec_c;

	tRtData = FilterReaderChunks ( tRtData, tQuery.m_dIntSubkeys );
	RtGuard_t tGuard ( std::move ( tRtData ) );
	auto& dDiskChunks = tGuard.m_dDiskChunks;

	// wrappers
	TokenizerRefPtr_c pQueryTokenizer { m_pTokenizer->Clone ( SPH_CLONE_QUERY ) };
	sphSetupQueryTokenizer ( pQueryTokenizer, IsStarDict ( m_bKeywordDict ), m_tSettings.m_bIndexExactWords, false );

	DictRefPtr_c pDict { GetStatelessDict ( m_pDict ) };

	if ( m_bKeywordDict && IsStarDict ( m_bKeywordDict ) )
		SetupStarDict ( pDict, pQueryTokenizer );

	if ( m_tSettings.m_bIndexExactWords )
		SetupExactDict ( pDict, pQueryTokenizer );

	// calculate local idf for RT with disk chunks
	// in case of local_idf set but no external hash no full-scan query and RT has disk chunks
	const SmallStringHash_T<int64_t> * pLocalDocs = tArgs.m_pLocalDocs;
	SmallStringHash_T<int64_t> hLocalDocs;
	int64_t iTotalDocs = ( tArgs.m_iTotalDocs ? tArgs.m_iTotalDocs : m_tStats.m_iTotalDocuments );
	bool bGotLocalDF = tArgs.m_bLocalDF;
	if ( tArgs.m_bLocalDF && !tArgs.m_pLocalDocs && !tQuery.m_sQuery.IsEmpty() && !dDiskChunks.IsEmpty() )
	{
		SwitchProfile ( pProfiler, SPH_QSTATE_LOCAL_DF );
		GetKeywordsSettings_t tSettings;
		tSettings.m_bStats = true;
		CSphVector < CSphKeywordInfo > dKeywords;
		DoGetKeywords ( dKeywords, tQuery.m_sQuery.cstr(), tSettings, false, nullptr, tGuard );
		for ( auto & tKw : dKeywords )
			if ( !hLocalDocs.Exists ( tKw.m_sNormalized ) ) // skip dupes
				hLocalDocs.Add ( tKw.m_iDocs, tKw.m_sNormalized );

		pLocalDocs = &hLocalDocs;
		iTotalDocs = GetStats().m_iTotalDocuments;
		bGotLocalDF = true;
	}

	SwitchProfile ( pProfiler, SPH_QSTATE_INIT );

	// FIXME! each result will point to its own MVA and string pools

	//////////////////////
	// search disk chunks
	//////////////////////

	tMeta.m_bHasPrediction = tQuery.m_iMaxPredictedMsec>0;

	int64_t tmMaxTimer = 0;
	sph::MiniTimer_c dTimerGuard;
	if ( tQuery.m_uMaxQueryMsec>0 )
		tmMaxTimer = dTimerGuard.MiniTimerEngage ( tQuery.m_uMaxQueryMsec ); // max_query_time

	SorterSchemaTransform_c tSSTransform ( dDiskChunks.GetLength() );

	if ( !dDiskChunks.IsEmpty() )
		QueryDiskChunks ( tQuery, tMeta, tArgs, tGuard, dSorters, pProfiler, bGotLocalDF, pLocalDocs, iTotalDocs, m_sIndexName.cstr(), tSSTransform, tmMaxTimer );

	////////////////////
	// search RAM chunk
	////////////////////

	SwitchProfile ( pProfiler, SPH_QSTATE_INIT );

	// select the sorter with max schema
	// uses GetAttrsCount to get working facets (was GetRowSize)
	int iMaxSchemaIndex, iMatchPoolSize;
	std::tie ( iMaxSchemaIndex, iMatchPoolSize ) = GetMaxSchemaIndexAndMatchCapacity ( dSorters );

	if ( iMaxSchemaIndex==-1 )
		return false;

	const ISphSchema & tMaxSorterSchema = *( dSorters[iMaxSchemaIndex]->GetSchema ());
	auto dSorterSchemas = SorterSchemas ( dSorters, iMaxSchemaIndex );

	// setup calculations and result schema
	CSphQueryContext tCtx ( tQuery );
	tCtx.m_pProfile = pProfiler;
	tCtx.m_pLocalDocs = pLocalDocs;
	tCtx.m_iTotalDocs = iTotalDocs;
	tCtx.m_uPackedFactorFlags = tArgs.m_uPackedFactorFlags;

	if ( !tCtx.SetupCalc ( tMeta, tMaxSorterSchema, m_tSchema, nullptr, nullptr, dSorterSchemas ) )
		return false;

	// setup search terms
	RtQwordSetup_t tTermSetup ( tGuard );
	tTermSetup.SetDict ( pDict );
	tTermSetup.m_pIndex = this;
	tTermSetup.m_iDynamicRowitems = tMaxSorterSchema.GetDynamicSize();
	if ( tQuery.m_uMaxQueryMsec>0 )
		tTermSetup.m_iMaxTimer = dTimerGuard.MiniTimerEngage ( tQuery.m_uMaxQueryMsec ); // max_query_time
	tTermSetup.m_pWarning = &tMeta.m_sWarning;
	tTermSetup.SetSegment ( -1 );
	tTermSetup.m_pCtx = &tCtx;
	tTermSetup.m_bHasWideFields = ( m_tSchema.GetFieldsCount()>32 );

	// setup prediction constrain
	CSphQueryStats tQueryStats;
	int64_t iNanoBudget = (int64_t)(tQuery.m_iMaxPredictedMsec) * 1000000; // from milliseconds to nanoseconds
	tQueryStats.m_pNanoBudget = &iNanoBudget;
	if ( tMeta.m_bHasPrediction )
		tTermSetup.m_pStats = &tQueryStats;

	// bind weights
	tCtx.BindWeights ( tQuery, m_tSchema, tMeta.m_sWarning );

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
	SwitchProfile ( pProfiler, SPH_QSTATE_PARSE );

	XQQuery_t tParsed;
	// FIXME!!! provide segments list instead index to tTermSetup.m_pIndex

	const QueryParser_i * pQueryParser = tQuery.m_pQueryParser;
	assert ( pQueryParser );

	CSphScopedPayload tPayloads;

	// FIXME!!! add proper
	// - qcache invalidation after INSERT \ DELETE \ UPDATE and for plain index afte UPDATE #256
	// - qcache duplicates removal from killed document at segment #263
	tCtx.m_bSkipQCache = true;

	int iStackNeed = -1;
	bool bFullscan = pQueryParser->IsFullscan ( tQuery ); // use this
	// no need to create ranker, etc if there's no query
	if ( !bFullscan )
		iStackNeed = PrepareFTSearch (this, IsStarDict ( m_bKeywordDict ), m_bKeywordDict, m_tMutableSettings.m_iExpandKeywords, m_iExpansionLimit, (const char *) sModifiedQuery, m_tSettings, pQueryParser, tQuery, m_tSchema, (cRefCountedRefPtr_t) tGuard.m_tSegmentsAndChunks.m_pSegs, m_pTokenizer->Clone ( SPH_CLONE_QUERY ), pQueryTokenizer, pDict, tMeta, pProfiler, &tPayloads, tParsed );

	// empty index, empty result. Must be AFTER PrepareFTSearch, since it prepares list of words
	if ( tGuard.m_dRamSegs.IsEmpty() )
	{
		for ( auto i : dSorters )
			tSSTransform.Transform ( i, tGuard );

		tResult.m_pDocstore = m_tSchema.HasStoredFields () ? this : nullptr;
		tMeta.m_iQueryTime = 0;
		return true;
	}

	if ( !iStackNeed )
		return false;

	bool bResult;
	if ( bFullscan || pQueryParser->IsFullscan ( tParsed ) )
		bResult = DoFullScanQuery ( tGuard.m_dRamSegs, tMaxSorterSchema, tQuery, tArgs.m_iIndexWeight, m_iStride, tmMaxTimer, pProfiler, tCtx, dSorters, tMeta );
	else
		bResult = DoFullTextSearch ( tGuard.m_dRamSegs, tMaxSorterSchema, tQuery, m_sIndexName.cstr(), tArgs.m_iIndexWeight, iMatchPoolSize, iStackNeed, tTermSetup, pProfiler, tCtx, dSorters, tParsed, tMeta, dSorters.GetLength()==1 ? dSorters[0] : nullptr );

	if (!bResult)
		return false;

	MEMORY ( MEM_RT_RES_STRINGS );

	SwitchProfile ( pProfiler, SPH_QSTATE_DYNAMIC );

	// create new standalone schema for sorters (independent of any external indexes/pools/storages)
	// modify matches inside the sorters to work with the new schema
	for ( auto i : dSorters )
		tSSTransform.Transform ( i, tGuard );

	if ( tMeta.m_bHasPrediction )
		tMeta.m_tStats.Add ( tQueryStats );

	tResult.m_pDocstore = m_tSchema.HasStoredFields() ? this : nullptr;
	tMeta.m_iQueryTime = int ( ( sphMicroTimer()-tmQueryStart )/1000 );
	tMeta.m_iCpuTime += sphTaskCpuTimer ()-tmCpuQueryStart;
	return true;
}


void RtIndex_c::AddKeywordStats ( BYTE * sWord, const BYTE * sTokenized, CSphDict * pDict, bool bGetStats, int iQpos, RtQword_t * pQueryWord, CSphVector <CSphKeywordInfo> & dKeywords, const RtSegVec_c& dRamSegs ) const
{
	assert ( !bGetStats || pQueryWord );

	SphWordID_t iWord = pDict->GetWordID ( sWord );
	if ( !iWord )
		return;

	if ( bGetStats )
	{
		pQueryWord->Reset();
		pQueryWord->m_uWordID = iWord;
		pQueryWord->m_sWord = (const char *)sTokenized;
		pQueryWord->m_sDictWord = (const char *)sWord;
		for ( const auto& pSeg : dRamSegs )
			RtQwordSetupSegment ( pQueryWord, pSeg, false );
	}

	CSphKeywordInfo & tInfo = dKeywords.Add();
	tInfo.m_sTokenized = (const char *)sTokenized;
	tInfo.m_sNormalized = (const char*)sWord;
	tInfo.m_iDocs = bGetStats ? pQueryWord->m_iDocs : 0;
	tInfo.m_iHits = bGetStats ? pQueryWord->m_iHits : 0;
	tInfo.m_iQpos = iQpos;

	RemoveDictSpecials ( tInfo.m_sNormalized );
}


struct CSphRtQueryFilter : public ISphQueryFilter, public ISphNoncopyable
{
	const RtIndex_c *	m_pIndex;
	RtQword_t *			m_pQword;
	bool				m_bGetStats = false;
	const RtSegVec_c&	m_tGuard;

	CSphRtQueryFilter ( const RtIndex_c * pIndex, RtQword_t * pQword, const RtSegVec_c& tGuard )
		: m_pIndex ( pIndex )
		, m_pQword ( pQword )
		, m_tGuard ( tGuard )
	{}

	void AddKeywordStats ( BYTE * sWord, const BYTE * sTokenized, int iQpos, CSphVector <CSphKeywordInfo> & dKeywords ) final
	{
		assert ( m_pIndex && m_pQword );
		m_pIndex->AddKeywordStats ( sWord, sTokenized, m_pDict, m_tFoldSettings.m_bStats, iQpos, m_pQword, dKeywords, m_tGuard );
	}
};


static void HashKeywords ( CSphVector<CSphKeywordInfo> & dKeywords, SmallStringHash_T<CSphKeywordInfo> & hKeywords )
{
	for ( CSphKeywordInfo & tSrc : dKeywords )
	{
		CSphKeywordInfo & tDst = hKeywords.AddUnique ( tSrc.m_sNormalized );
		tDst.m_sTokenized = std::move ( tSrc.m_sTokenized );
		tDst.m_sNormalized = std::move ( tSrc.m_sNormalized );
		tDst.m_iQpos = tSrc.m_iQpos;
		tDst.m_iDocs += tSrc.m_iDocs;
		tDst.m_iHits += tSrc.m_iHits;
	}
}


void RtIndex_c::DoGetKeywords ( CSphVector<CSphKeywordInfo> & dKeywords, const char * sQuery, const GetKeywordsSettings_t & tSettings, bool bFillOnly, CSphString * pError, const RtGuard_t& tGuard ) const
{
	if ( !bFillOnly )
		dKeywords.Resize ( 0 );

	if ( ( bFillOnly && !dKeywords.GetLength() ) || ( !bFillOnly && ( !sQuery || !sQuery[0] ) ) )
		return;

	RtQword_t tQword;

	TokenizerRefPtr_c pTokenizer { m_pTokenizer->Clone ( SPH_CLONE_INDEX ) };
	pTokenizer->EnableTokenizedMultiformTracking ();

	// need to support '*' and '=' but not the other specials
	// so m_pQueryTokenizer does not work for us, gotta clone and setup one manually
	DictRefPtr_c pDict { GetStatelessDict ( m_pDict ) };

	if ( IsStarDict ( m_bKeywordDict ) )
	{
		if ( m_bKeywordDict )
			SetupStarDict ( pDict, pTokenizer );
		else
			pTokenizer->AddPlainChar ( '*' );
	}

	if ( m_tSettings.m_bIndexExactWords )
		SetupExactDict ( pDict, pTokenizer, false );

	CSphVector<BYTE> dFiltered;
	FieldFilterRefPtr_c pFieldFilter;
	const BYTE * sModifiedQuery = (const BYTE *)sQuery;
	if ( m_pFieldFilter && sQuery )
	{
		pFieldFilter = m_pFieldFilter->Clone();
		if ( pFieldFilter && pFieldFilter->Apply ( sModifiedQuery, dFiltered, true ) )
			sModifiedQuery = dFiltered.Begin();
	}

	// FIXME!!! missing bigram

	if ( !bFillOnly )
	{
		ExpansionContext_t tExpCtx;

		// query defined options
		tExpCtx.m_iExpansionLimit = tSettings.m_iExpansionLimit ? tSettings.m_iExpansionLimit : m_iExpansionLimit;
		bool bExpandWildcards = ( m_bKeywordDict && IsStarDict ( m_bKeywordDict ) && !tSettings.m_bFoldWildcards );

		CSphRtQueryFilter tAotFilter ( this, &tQword, tGuard.m_dRamSegs );
		tAotFilter.m_pTokenizer = pTokenizer;
		tAotFilter.m_pDict = pDict;
		tAotFilter.m_pSettings = &m_tSettings;
		tAotFilter.m_tFoldSettings = tSettings;
		tAotFilter.m_tFoldSettings.m_bFoldWildcards = !bExpandWildcards;

		tExpCtx.m_pWordlist = this;
		tExpCtx.m_iMinPrefixLen = m_tSettings.GetMinPrefixLen ( m_bKeywordDict );
		tExpCtx.m_iMinInfixLen = m_tSettings.m_iMinInfixLen;
		tExpCtx.m_bHasExactForms = ( m_pDict->HasMorphology() || m_tSettings.m_bIndexExactWords );
		tExpCtx.m_bMergeSingles = false;
		tExpCtx.m_pIndexData = tGuard.m_tSegmentsAndChunks.m_pSegs;

		pTokenizer->SetBuffer ( sModifiedQuery, (int) strlen ( (const char*)sModifiedQuery ) );
		tAotFilter.GetKeywords ( dKeywords, tExpCtx );
	} else
	{
		BYTE sWord[SPH_MAX_KEYWORD_LEN];

		ARRAY_FOREACH ( i, dKeywords )
		{
			CSphKeywordInfo & tInfo = dKeywords[i];
			int iLen = tInfo.m_sTokenized.Length();
			memcpy ( sWord, tInfo.m_sTokenized.cstr(), iLen );
			sWord[iLen] = '\0';

			SphWordID_t iWord = pDict->GetWordID ( sWord );
			if ( iWord )
			{
				tQword.Reset();
				tQword.m_uWordID = iWord;
				tQword.m_sWord = tInfo.m_sTokenized;
				tQword.m_sDictWord = (const char *)sWord;
				for ( const auto& pSeg : tGuard.m_dRamSegs )
					RtQwordSetupSegment ( &tQword, pSeg, false );

				tInfo.m_iDocs += tQword.m_iDocs;
				tInfo.m_iHits += tQword.m_iHits;
			}
		}
	}

	// get stats from disk chunks too
	if ( !tSettings.m_bStats )
		return;

	if ( bFillOnly )
	{
		for ( auto& pChunk : tGuard.m_dDiskChunks )
			pChunk->Cidx().FillKeywords ( dKeywords );
	} else
	{
		// bigram and expanded might differs need to merge infos
		CSphVector<CSphKeywordInfo> dChunkKeywords;
		SmallStringHash_T<CSphKeywordInfo> hKeywords;
		for ( auto& pChunk: tGuard.m_dDiskChunks )
		{
			pChunk->Cidx().GetKeywords ( dChunkKeywords, (const char*)sModifiedQuery, tSettings, pError );
			HashKeywords ( dChunkKeywords, hKeywords );
			dChunkKeywords.Resize ( 0 );
		}

		if ( hKeywords.GetLength() )
		{
			// merge keywords from RAM parts with disk keywords into hash
			HashKeywords ( dKeywords, hKeywords );
			dKeywords.Resize ( 0 );
			dKeywords.Reserve ( hKeywords.GetLength() );

			hKeywords.IterateStart();
			while ( hKeywords.IterateNext() )
			{
				const CSphKeywordInfo & tSrc = hKeywords.IterateGet();
				dKeywords.Add ( tSrc );
			}
			sphSort ( dKeywords.Begin(), dKeywords.GetLength(), bind ( &CSphKeywordInfo::m_iQpos ) );
		}
	}
}


bool RtIndex_c::GetKeywords ( CSphVector<CSphKeywordInfo> & dKeywords, const char * sQuery, const GetKeywordsSettings_t & tSettings, CSphString * pError ) const
{
	RtGuard_t tGuard ( m_tRtChunks.RtData() );
	DoGetKeywords ( dKeywords, sQuery, tSettings, false, pError, tGuard );
	return true;
}


bool RtIndex_c::FillKeywords ( CSphVector<CSphKeywordInfo> & dKeywords ) const
{
	RtGuard_t tGuard ( m_tRtChunks.RtData() );
	DoGetKeywords ( dKeywords, nullptr, GetKeywordsSettings_t(), true, nullptr, tGuard );
	return true;
}


// for each RamSegment collect list of rows and indexes in update ctx
CSphFixedVector<RowsToUpdateData_t> Update_CollectRowPtrs ( UpdateContext_t & tCtx, const ConstRtSegmentSlice_t& tSegments  )
{
	CSphFixedVector<RowsToUpdateData_t> dUpdateSets { tSegments.GetLength () };

	// collect idxes of alive (not-yet-updated) rows
	const auto& dDocids = tCtx.m_tUpd.m_pUpdate->m_dDocids;
	ARRAY_CONSTFOREACH ( i, dDocids )
		if ( !tCtx.m_tUpd.m_dUpdated.BitGet ( i ) )
			ARRAY_CONSTFOREACH ( j, tSegments )
			{
				auto pRow = tSegments[j]->FindAliveRow ( dDocids[i] );
				if ( !pRow )
					continue;

				auto& dUpd = dUpdateSets[j].Add();
				dUpd.m_pRow = pRow;
				dUpd.m_iIdx = i;
			}
	return dUpdateSets;
}

// when RAM segment is used in merge or saving to disk - it's data finally became merged into new segment or chunk,
// and source segment then unlinked. If during that operation attributes was updated by aside 'update attributes' call,
// the changes will not participate in final result and so, will be discarded with source segment unlink.
// to deal with this, we set 'dirty' bit on segment to show that updates to it must be also collected and then applied
// to final resulting chunk/segment. That bit set before merging attributes and exists till the end of segment's lifetime.
// Here is first part of postponed merge - after update we collect docs updated in segment and store them into vec of
// updates (as it might happen be more than one update during the operation)
static void AddDerivedUpdate ( const RowsToUpdate_t & dRows, const UpdateContext_t & tCtx )
{
	auto & tUpd = tCtx.m_tUpd;
	// count exact N of affected rows (no need to waste space for reserve in this route at all)
	auto iRows = dRows.count_of ( [&tUpd] ( auto & i ) { return tUpd.m_dUpdated.BitGet ( i.m_iIdx ); } );

	if ( !iRows )
		return;

	auto * pSegment = (RtSegment_t *) tCtx.m_pSegment;
	assert (pSegment);

	auto& tNewDerivedUpdate = pSegment->m_dPostponedUpdates.Add();
	tNewDerivedUpdate.m_pUpdate = MakeReusableUpdate ( tUpd.m_pUpdate );
	tNewDerivedUpdate.m_dRowsToUpdate.Reserve (iRows);

	// collect indexes of actually updated rows and docids
	dRows.for_each ( [&tUpd, &tNewDerivedUpdate] ( auto & i ) {
		if ( tUpd.m_dUpdated.BitGet ( i.m_iIdx ) )
			tNewDerivedUpdate.m_dRowsToUpdate.Add ().m_iIdx = i.m_iIdx;
	});
}

bool RtIndex_c::Update_DiskChunks ( AttrUpdateInc_t& tUpd, const DiskChunkSlice_t& dDiskChunks, CSphString & sError )
{
	bool bCritical = false;
	CSphString sWarning;

	// That seems to be only place where order of disk chunks is important.
	// About deduplication: order of new-born disk chunks is important, as they may contain replaces for older docs.
	// Since we don't consider killed documents during update, it is important to update from freshest to oldest,
	// this order ensures that actual documents updated. However, for optimized (merged/splitted) chunks that is not
	// important as kill-lists applied during merge, and so resulting chunks have only 'freshest' version, because
	// all previous are effectively excluded during merge pass.
	// (If we refactor updates to scan rows taking in account kill-lists, order will not be important at all).
	for ( int iChunk = dDiskChunks.GetLength()-1; iChunk>=0; --iChunk )
	{
		if ( tUpd.AllApplied () )
			break;

		auto& pDiskChunk = dDiskChunks[iChunk];

		// acquire fine-grain lock
		SccWL_t wLock ( pDiskChunk->m_tLock );

		int iRes = pDiskChunk->CastIdx().UpdateAttributes ( tUpd, bCritical, sError, sWarning );

		// FIXME! need to handle critical failures here (chunk is unusable at this point)
		assert ( !bCritical );

		// FIXME! maybe emit a warning to client as well?
		if ( iRes<0 )
			return false;

		// update stats
		m_uDiskAttrStatus |= pDiskChunk->Cidx().GetAttributeStatus();
	}

	return true;
}

// thread-safe, as segment is locked up level before calling UpdateAttributesInRamSegment
bool RtIndex_c::Update_WriteBlobRow ( UpdateContext_t & tCtx, CSphRowitem * pDocinfo, const BYTE * pBlob, int iLength,
		int nBlobAttrs, const CSphAttrLocator & tBlobRowLoc, bool & bCritical, CSphString & sError ) NO_THREAD_SAFETY_ANALYSIS
{
	// fixme! Ensure pSegment->m_tLock acquired exclusively...
	auto* pSegment = (RtSegment_t*)tCtx.m_pSegment;
	assert ( pSegment );

	bCritical = false;

	CSphTightVector<BYTE> & dBlobPool = pSegment->m_dBlobs;

	BYTE * pExistingBlob = dBlobPool.Begin() + sphGetRowAttr ( pDocinfo, tBlobRowLoc );
	DWORD uExistingBlobLen = sphGetBlobTotalLen ( pExistingBlob, nBlobAttrs );

	// overwrite old record
	if ( (DWORD)iLength<=uExistingBlobLen )
	{
		memcpy ( pExistingBlob, pBlob, iLength );
		return true;
	}

	int iPoolSize = dBlobPool.GetLength();
	dBlobPool.Append ( pBlob, iLength );

	sphSetRowAttr ( pDocinfo, tBlobRowLoc, iPoolSize );

	// update blob pool ptrs since they could have changed after the resize
	tCtx.m_pBlobPool = dBlobPool.begin();
	return true;
}


// FIXME! might be inconsistent in case disk chunk update fails
int RtIndex_c::UpdateAttributes ( AttrUpdateInc_t & tUpd, bool & bCritical, CSphString & sError, CSphString & sWarning )
{
	const auto& tUpdc = *tUpd.m_pUpdate;
	assert ( tUpdc.m_dRowOffset.IsEmpty() || tUpdc.m_dDocids.GetLength()==tUpdc.m_dRowOffset.GetLength() );

	if ( tUpdc.m_dDocids.IsEmpty()  || tUpd.AllApplied () )
		return 0;

	// FIXME!!! grab Writer lock to prevent segments retirement during commit(merge)
	if ( m_tRtChunks.IsEmpty() )
		return 0;

	if ( m_eSaving.load ( std::memory_order_relaxed ) == WriteState_e::DISABLED )
	{
		sError = "index is locked now, try again later";
		return -1;
	}

	int iUpdated = tUpd.m_iAffected;

	UpdateContext_t tCtx ( tUpd, m_tSchema );
	if ( !Update_CheckAttributes ( *tCtx.m_tUpd.m_pUpdate, tCtx.m_tSchema, sError ) )
		return -1;

	Update_PrepareListOfUpdatedAttributes ( tCtx, sError );

	// do update in serial fiber. That ensures no concurrency with set of chunks changing, however need to dispatch
	// with changers themselves (merge segments, merge chunks, save disk chunks).
	// fixme! Find another way (dedicated fiber?), as long op in serial fiber may pause another ops.
	RtGuard_t tGuard ( m_tRtChunks.RtData() );
	auto dRamUpdateSets = Update_CollectRowPtrs ( tCtx, tGuard.m_dRamSegs );

	ScopedScheduler_c tSerialFiber ( m_tRtChunks.SerialChunkAccess() );
	ARRAY_CONSTFOREACH ( i, dRamUpdateSets )
	{
		if ( dRamUpdateSets[i].IsEmpty() )
			continue;

		auto* pSeg = const_cast<RtSegment_t*> ( (const RtSegment_t*)tGuard.m_dRamSegs[i] );
		SccWL_t wLock ( pSeg->m_tLock );

		// point context to target segment
		tCtx.m_pAttrPool = pSeg->m_dRows.begin();
		tCtx.m_pBlobPool = pSeg->m_dBlobs.begin();
		tCtx.m_pSegment = pSeg;
		if ( !UpdateAttributesInRamSegment ( dRamUpdateSets[i], tCtx, bCritical, sError ) )
			return -1;

		if ( pSeg->m_bAttrsBusy.load ( std::memory_order_acquire ) )
			AddDerivedUpdate ( dRamUpdateSets[i], tCtx ); // segment is now saving/merging - add postponed update.

		if ( tUpd.AllApplied () )
			break;
	}

	if ( !Update_DiskChunks ( tUpd, tGuard.m_dDiskChunks, sError ) ) // fixme!
		sphWarn ( "INTERNAL ERROR: index %s update failure: %s", m_sIndexName.cstr(), sError.cstr() );

	// bump the counter, binlog the update!
	Binlog::CommitUpdateAttributes ( &m_iTID, m_sIndexName.cstr(), tUpdc );

	iUpdated = tUpd.m_iAffected - iUpdated;
	if ( !Update_HandleJsonWarnings ( tCtx, iUpdated, sWarning, sError ) )
		return -1;

	// all done
	return iUpdated;
}


bool RtIndex_c::SaveAttributes ( CSphString & sError ) const
{
	DWORD uStatus = m_uDiskAttrStatus;
	bool bAllSaved = true;

	const auto& pDiskChunks = m_tRtChunks.DiskChunks();

	if ( pDiskChunks->IsEmpty() || ( m_eSaving.load ( std::memory_order_acquire ) == WriteState_e::DISCARD ) )
		return true;

	for ( auto& pChunk : *pDiskChunks )
		bAllSaved &= pChunk->Cidx().SaveAttributes ( sError );

	if ( uStatus==m_uDiskAttrStatus )
		m_uDiskAttrStatus = 0;

	return bAllSaved;
}


class OptimizeGuard_c : ISphNoncopyable
{
	RtIndex_c & m_tIndex;
	bool m_bPreviousOptimizeState;

public:
	explicit OptimizeGuard_c ( RtIndex_c& tIndex )
		: m_tIndex ( tIndex )
		, m_bPreviousOptimizeState ( tIndex.m_bOptimizeStop.load ( std::memory_order_acquire ) )
	{
		m_tIndex.StopOptimize();
	}

	~OptimizeGuard_c ()
	{
		m_tIndex.m_bOptimizeStop.store ( m_bPreviousOptimizeState, std::memory_order_release );
	}
};

// fixme! is NOT lru-safe, ensure index locked exclusively!
// (last is true as it w-locked before 'alter' called, however optimize call need special glance!)
bool RtIndex_c::AddRemoveColumnarAttr ( RtGuard_t & tGuard, bool bAdd, const CSphString & sAttrName, ESphAttr eAttrType, const CSphSchema & tOldSchema, const CSphSchema & tNewSchema, CSphString & sError )
{
	for ( auto& pConstSeg : tGuard.m_dRamSegs )
	{
		auto* pSeg = const_cast<RtSegment_t*> ( pConstSeg.Ptr() );
		assert ( pSeg );
		CSphScopedPtr<ColumnarBuilderRT_i> pBuilder ( CreateColumnarBuilderRT ( tNewSchema ) );

		if ( !Alter_AddRemoveColumnar ( bAdd, tOldSchema, tNewSchema, pSeg->m_pColumnar.Ptr(), pBuilder.Ptr(), pSeg->m_uRows, m_sPath, sError ) )
			return false;

		pSeg->m_pColumnar = CreateColumnarRT ( tNewSchema, pBuilder.Ptr() );
		pSeg->UpdateUsedRam();
	}

	return true;
}

// fixme! is NOT lru-safe, ensure index locked exclusively!
// (last is true as it w-locked before 'alter' called, however optimize call need special glance!)
void RtIndex_c::AddRemoveRowwiseAttr ( RtGuard_t & tGuard, bool bAdd, const CSphString & sAttrName, ESphAttr eAttrType, const CSphSchema & tOldSchema, const CSphSchema & tNewSchema, CSphString & sError )
{
	bool bHadBlobs = false;
	for ( int i = 0; i < tOldSchema.GetAttrsCount() && !bHadBlobs; ++i )
		bHadBlobs |= sphIsBlobAttr ( tOldSchema.GetAttr(i) );

	bool bHaveBlobs = false;
	for ( int i = 0; i < tNewSchema.GetAttrsCount() && !bHaveBlobs; ++i )
		bHaveBlobs |= sphIsBlobAttr ( tNewSchema.GetAttr(i) );

	bool bBlob = sphIsBlobAttr(eAttrType);
	bool bBlobsModified = bBlob && ( bAdd || bHaveBlobs==bHadBlobs );

	// now modify the ramchunk
	for ( auto& pRSeg : tGuard.m_dRamSegs )
	{
		assert ( pRSeg );

		CSphTightVector<CSphRowitem> dSPA;
		CSphTightVector<BYTE> dSPB;
		CSphScopedPtr<WriteWrapper_c> pSPAWriteWrapper ( CreateWriteWrapperMem ( dSPA ) );
		CSphScopedPtr<WriteWrapper_c> pSPBWriteWrapper ( CreateWriteWrapperMem ( dSPB ) );
		dSPA.Reserve ( pRSeg->m_uRows * m_iStride );

		auto * pWSeg = const_cast<RtSegment_t*> ( pRSeg.Ptr() );
		SccWL_t _ (pWSeg->m_tLock);

		const CSphRowitem* pDocinfo = pWSeg->m_dRows.begin();
		dSPB.Reserve ( pWSeg->m_dBlobs.GetLength() / 2 );		// reserve half of our current blobs, just in case
		if ( !Alter_AddRemoveRowwiseAttr ( tOldSchema, tNewSchema, pDocinfo, pRSeg->m_uRows, pWSeg->m_dBlobs.begin(), *pSPAWriteWrapper, *pSPBWriteWrapper, bAdd, sAttrName ) )
			sphWarning ( "%s attribute to %s: %s", bAdd ? "adding" : "removing", m_sPath.cstr(), sError.cstr() );
		pWSeg->m_dRows.SwapData(dSPA);
		if ( bBlob || bBlobsModified )
			pWSeg->m_dBlobs.SwapData(dSPB);

		pRSeg->UpdateUsedRam();
	}
}

// fixme! Need fine-grain locking, not const_cast!
void RtIndex_c::AddFieldToRamchunk ( const CSphString & sFieldName, DWORD uFieldFlags, const CSphSchema & tNewSchema )
{
	if ( !(uFieldFlags & CSphColumnInfo::FIELD_STORED) )
		return;

	assert ( m_pDocstoreFields.Ptr() );
	m_pDocstoreFields->AddField ( sFieldName, DOCSTORE_TEXT );

	auto pSegs = m_tRtChunks.RamSegs();
	for ( auto& pConstSeg : *pSegs )
	{
		auto* pSeg = const_cast<RtSegment_t*> ( pConstSeg.Ptr() );
		CSphScopedPtr<DocstoreRT_i> pNewDocstore ( CreateDocstoreRT() );
		Alter_AddRemoveFieldFromDocstore ( *pNewDocstore, pSeg->m_pDocstore.Ptr(), pSeg->m_uRows, tNewSchema );
		pSeg->m_pDocstore.Swap(pNewDocstore);

		pSeg->UpdateUsedRam();
	}
}


void RtIndex_c::RemoveFieldFromRamchunk ( const CSphString & sFieldName, const CSphSchema & tOldSchema, const CSphSchema & tNewSchema )
{
	bool bRemoveStored = m_pDocstoreFields.Ptr() && m_pDocstoreFields->GetFieldId ( sFieldName, DOCSTORE_TEXT )!=-1;
	bool bLastStored = !tNewSchema.HasStoredFields();
	if ( bRemoveStored )
		m_pDocstoreFields->RemoveField ( sFieldName, DOCSTORE_TEXT );

	int iFieldId = tOldSchema.GetFieldIndex ( sFieldName.cstr () );
	auto pSegs = m_tRtChunks.RamSegs();
	for ( auto& pConstSeg : *pSegs )
	{
		auto* pSeg = const_cast<RtSegment_t*> ( pConstSeg.Ptr() );
		assert ( pSeg );
		DeleteFieldFromDict ( pSeg, iFieldId );
		if ( bRemoveStored )
		{
			if ( !bLastStored )
			{
				CSphScopedPtr<DocstoreRT_i> pNewDocstore ( CreateDocstoreRT() );
				Alter_AddRemoveFieldFromDocstore ( *pNewDocstore, pSeg->m_pDocstore.Ptr(), pSeg->m_uRows, tNewSchema );
				pSeg->m_pDocstore.Swap(pNewDocstore);
			}
			else
				pSeg->m_pDocstore.Reset();
		}
		pSeg->UpdateUsedRam();
	}
}


bool RtIndex_c::AddRemoveField ( bool bAdd, const CSphString & sFieldName, DWORD uFieldFlags, CSphString & sError )
{
	OptimizeGuard_c tStopOptimize ( *this );

	// go to serial fiber.
	ScopedScheduler_c tSerialFiber ( m_tRtChunks.SerialChunkAccess() );

	CSphSchema tOldSchema = m_tSchema;
	CSphSchema tNewSchema = m_tSchema;

	if ( !Alter_AddRemoveFieldFromSchema ( bAdd, tNewSchema, sFieldName, uFieldFlags, sError ) )
		return false;

	m_tSchema = tNewSchema;

	RtGuard_t tGuard ( m_tRtChunks.RtData() );

	// modify the in-memory data of disk chunks
	// fixme: we can't rollback in-memory changes, so we just show errors here for now
	for ( auto& pChunk : tGuard.m_dDiskChunks )
		if ( !pChunk->CastIdx().AddRemoveField ( bAdd, sFieldName, uFieldFlags, sError ) )
			sphWarning ( "%s attribute to %s.%d: %s", bAdd ? "adding" : "removing", m_sPath.cstr(), pChunk->Cidx().m_iChunk, sError.cstr() );

	if ( bAdd )
		AddFieldToRamchunk ( sFieldName, uFieldFlags, tNewSchema );
	else
		RemoveFieldFromRamchunk ( sFieldName, tOldSchema, tNewSchema );

	// fixme: we can't rollback at this point
	AlterSave ( true );

	return true;
}

void RtIndex_c::AlterSave ( bool bSaveRam )
{
	if ( bSaveRam )
	{
		Verify ( SaveRamChunk () );
	}

	SaveMeta ();

	// fixme: notify that it was ALTER that caused the flush
	Binlog::NotifyIndexFlush ( m_sIndexName.cstr (), m_iTID, false );

	QcacheDeleteIndex ( GetIndexId() );
}

bool RtIndex_c::AddRemoveAttribute ( bool bAdd, const CSphString & sAttrName, ESphAttr eAttrType, AttrEngine_e eEngine, CSphString & sError )
{
	if ( !m_tRtChunks.DiskChunks()->IsEmpty() && !m_tSchema.GetAttrsCount() )
	{
		sError = "index must already have attributes";
		return false;
	}

	// here must be exclusively LOCKED access, we don't rely from the topmost lock and go isolated ourselves

	// stop all optimize tasks
	OptimizeGuard_c tStopOptimize ( *this );

	// go to serial fiber.
	ScopedScheduler_c tSerialFiber ( m_tRtChunks.SerialChunkAccess() );

	// wait all secondary service tasks (merge segments, save disk chunk) to finish and release all the segments.
	WaitRAMSegmentsUnlocked();

	// as we're in serial, here all index data exclusively belongs to us. No new commits, merges, flushes, etc. until
	// we're finished.

	// combine per-index and per-attribute engine settings
	AttrEngine_e eAttrEngine = m_tSettings.m_eEngine;
	if ( eEngine!=AttrEngine_e::DEFAULT )
		eAttrEngine = eEngine;

	CSphSchema tOldSchema = m_tSchema;
	CSphSchema tNewSchema = m_tSchema;
	if ( !Alter_AddRemoveFromSchema ( tNewSchema, sAttrName, eAttrType, eAttrEngine, bAdd, sError ) )
		return false;

	m_tSchema = tNewSchema;
	m_iStride = m_tSchema.GetRowSize();

	RtGuard_t tGuard ( m_tRtChunks.RtData() );

	// modify the in-memory data of disk chunks
	// fixme: we can't rollback in-memory changes, so we just show errors here for now
	for ( auto& pChunk : tGuard.m_dDiskChunks )
		if ( !pChunk->CastIdx().AddRemoveAttribute ( bAdd, sAttrName, eAttrType, eEngine, sError ) )
			sphWarning ( "%s attribute to %s.%d: %s", bAdd ? "adding" : "removing", m_sPath.cstr(), pChunk->Cidx().m_iChunk, sError.cstr() );

	bool bColumnar = bAdd ? tNewSchema.GetAttr ( sAttrName.cstr() )->IsColumnar() : tOldSchema.GetAttr ( sAttrName.cstr() )->IsColumnar();
	if ( bColumnar )
	{
		if ( !AddRemoveColumnarAttr ( tGuard, bAdd, sAttrName, eAttrType, tOldSchema, tNewSchema, sError ) )
			return false;
	} else
		AddRemoveRowwiseAttr ( tGuard, bAdd, sAttrName, eAttrType, tOldSchema, tNewSchema, sError );

	// fixme: we can't rollback at this point
	AlterSave ( true );
	return true;
}


//////////////////////////////////////////////////////////////////////////
// MAGIC CONVERSIONS
//////////////////////////////////////////////////////////////////////////

CSphString RtIndex_c::MakeChunkName ( int iChunkID )
{
	CSphString sChunk;
	sChunk.SetSprintf ( "%s.%d", m_sPath.cstr(), iChunkID );
	return sChunk;
}

// make tiny ref index - grab chunks as disk chunks; no changes; no heavy initialization
void RtIndex_c::InitOneshotIndex ( const VecTraits_T<CSphIndex*>& dChunks ) NO_THREAD_SAFETY_ANALYSIS
{
	ProhibitSave();
	m_bLoadRamPassedOk = false;

	auto pFirst = dChunks.First();

	// copy tokenizer, dict, etc. settings from first chunk.
	// fixme! There is no check about absent/non-compatible settings between grabbed plain and rt
	m_tSettings = pFirst->GetSettings();
	m_pTokenizer = pFirst->GetTokenizer()->Clone ( SPH_CLONE_INDEX );
//	m_pDict = pFirst->GetDictionary()->Clone();

	RtWriter_c tWriter { m_tRtChunks, RtWriter_c::unsafe };
	tWriter.InitDiskChunks ( RtWriter_c::empty );
	for ( CSphIndex* pIndex : dChunks )
	{
		assert ( pIndex );
		auto pChunk = DiskChunk_c::wrap ( pIndex );
		tWriter.m_pNewDiskChunks->Add ( pChunk );

		m_tStats.m_iTotalBytes += pIndex->GetStats().m_iTotalBytes;
		m_tStats.m_iTotalDocuments += pIndex->GetStats().m_iTotalDocuments;

		// update field lengths
		if ( m_tSchema.GetAttrId_FirstFieldLen()>=0 )
		{
			int64_t * pLens = pIndex->GetFieldLens();
			if ( pLens )
				for ( int i=0; i < pIndex->GetMatchSchema().GetFieldsCount(); ++i )
					m_dFieldLensDisk[i] += pLens[i];
		}
	}
}

void RtIndex_c::InitDictionaryInOneshot () NO_THREAD_SAFETY_ANALYSIS
{
	if ( m_pDict )
		return;

	m_pDict = m_tRtChunks.DiskChunks()->operator[] (0)->Cidx().GetDictionary()->Clone();
}

// CSphinxqlSession::Execute->HandleMysqlAttach->AttachDiskIndex
bool RtIndex_c::AttachDiskIndex ( CSphIndex* pIndex, bool bTruncate, bool & bFatal, StrVec_t & dWarnings, CSphString & sError )
{
	// from the next line we work in index simple scheduler. That made everything much simpler
	// (no need to care about locks and order of access to ram segments and disk chunks)
	ScopedScheduler_c tSerialFiber ( m_tRtChunks.SerialChunkAccess() );

	bFatal = false;

	if ( bTruncate && !Truncate ( sError ) )
		return false;

	// safeguards
	// we do not support some of the disk index features in RT just yet
#define LOC_ERROR(_arg) { sError = _arg; return false; }
	const CSphIndexSettings & tSettings = pIndex->GetSettings();
	if ( tSettings.m_iStopwordStep!=1 )
		LOC_ERROR ( "ATTACH currently requires stopword_step=1 in disk index (RT-side support not implemented yet)" );

	bool bEmptyRT = m_tRtChunks.IsEmpty();
	// ATTACH to exist index require these checks
	if ( !bEmptyRT )
	{
		if ( m_pTokenizer->GetSettingsFNV()!=pIndex->GetTokenizer()->GetSettingsFNV() )
			LOC_ERROR ( "ATTACH currently requires same tokenizer settings (RT-side support not implemented yet)" );
		if ( m_pDict->GetSettingsFNV()!=pIndex->GetDictionary()->GetSettingsFNV() )
			LOC_ERROR ( "ATTACH currently requires same dictionary settings (RT-side support not implemented yet)" );
		if ( !GetMatchSchema().CompareTo ( pIndex->GetMatchSchema(), sError, true ) )
			LOC_ERROR ( "ATTACH currently requires same attributes declaration (RT-side support not implemented yet)" );
	}
#undef LOC_ERROR

	// note: that is important. Active readers prohibited by topmost w-lock, but internal processes not!
	if ( bTruncate && !Truncate ( sError ) )
		return false;

	int iTotalKilled = 0;

	// attach to non-empty RT: first flush RAM segments to disk chunk, then apply upcoming index'es docs as k-list.
	if ( !bEmptyRT )
	{
		auto dIndexDocs = pIndex->BuildDocList();
		if ( TlsMsg::HasErr () )
		{
			sError.SetSprintf ( "ATTACH failed, %s", TlsMsg::szError () );
			return false;
		}

		if ( !m_tRtChunks.RamSegs()->IsEmpty() && !SaveDiskChunk ( true ) )
		{
			bFatal = true;
			return false;
		}

		iTotalKilled = ApplyKillList ( dIndexDocs );
	}

	// rename that source index to our last chunk
	int iChunk = m_tRtChunks.MakeChunkId();
	if ( !pIndex->Rename ( MakeChunkName ( iChunk ).cstr() ) )
	{
		sError.SetSprintf ( "ATTACH failed, %s", pIndex->GetLastError().cstr() );
		bFatal = !bEmptyRT; // as attach failed, but we applied k-list - index is not consistent now.
		return false;
	}

	// copy schema from new index
	SetSchema ( pIndex->GetMatchSchema() );
	m_tStats.m_iTotalBytes += pIndex->GetStats().m_iTotalBytes;
	m_tStats.m_iTotalDocuments += pIndex->GetStats().m_iTotalDocuments-iTotalKilled;

	// copy tokenizer, dict etc settings from new index
	m_tSettings = pIndex->GetSettings();
	m_tSettings.m_dBigramWords.Reset();

	m_pTokenizer = pIndex->GetTokenizer()->Clone ( SPH_CLONE_INDEX );
	m_pDict = pIndex->GetDictionary()->Clone ();
	PostSetup();
	CSphString sName;
	sName.SetSprintf ( "%s_%d", m_sIndexName.cstr(), iChunk ); // idx name is cosmetic thing
	pIndex->SetName ( sName.cstr() );
	pIndex->SetBinlog ( false );
	pIndex->m_iChunk = iChunk;

	// FIXME? what about copying m_TID etc?


	{	// update disk chunk list
		RtWriter_c tNewSet ( m_tRtChunks );
		tNewSet.InitDiskChunks ( RtWriter_c::copy );
		tNewSet.m_pNewDiskChunks->Add ( DiskChunk_c::make ( pIndex ) );
	}

	// resave header file
	SaveMeta();

	// FIXME? do something about binlog too?
	// Binlog::NotifyIndexFlush ( m_sIndexName.cstr(), m_iTID, false );

	// all done, reset cache
	QcacheDeleteIndex ( GetIndexId() );
	return true;
}

//////////////////////////////////////////////////////////////////////////
// TRUNCATE
//////////////////////////////////////////////////////////////////////////

// kill RAM chunk file
void RtIndex_c::UnlinkRAMChunk ( const char* szInfo )
{
	CSphString sFile;
	sFile.SetSprintf ( "%s.ram", m_sPath.cstr() );
	if ( ::unlink ( sFile.cstr() ) && errno != ENOENT && szInfo )
		sphWarning ( "rt: %s failed to unlink %s: (errno=%d, error=%s)", szInfo, sFile.cstr(), errno, strerrorm ( errno ) );
}

bool RtIndex_c::Truncate ( CSphString& )
{
	// TRUNCATE will drop everything; so all 'optimizing' should be discarded as useless
	OptimizeGuard_c tStopOptimize ( *this );

	// do truncate in serial fiber. As it is re-enterable, don't care if we already there.
	ScopedScheduler_c tSerialFiber ( m_tRtChunks.SerialChunkAccess() );

	// update and save meta
	// indicate 0 disk chunks, we are about to kill them anyway
	// current TID will be saved, so replay will properly skip preceding txns
	m_tStats.Reset();
	SaveMeta ( m_iTID, { nullptr, 0 } );

	// allow binlog to unlink now-redundant data files
	Binlog::NotifyIndexFlush ( m_sIndexName.cstr(), m_iTID, false );

	// kill RAM chunk file
	UnlinkRAMChunk ( "truncate" );

	// kill all disk chunks files
	m_tRtChunks.DiskChunks()->for_each ( [] ( ConstDiskChunkRefPtr_t& t ) { t->m_bFinallyUnlink = true; } );

	{
		// remove all chunks and segments
		RtWriter_c tChangeset ( m_tRtChunks );
		tChangeset.InitRamSegs ( RtWriter_c::empty );
		tChangeset.InitDiskChunks ( RtWriter_c::empty );
	}

	// reset cache
	QcacheDeleteIndex ( GetIndexId() );
	return true;
}

void RtIndex_c::SetKillHookFor ( IndexSegment_c* pAccum, int iDiskChunkID ) const
{
	// that ensures no concurrency
	ScopedScheduler_c tSerialFiber ( m_tRtChunks.SerialChunkAccess() );
	ProcessDiskChunkByID ( iDiskChunkID, [pAccum] ( const DiskChunk_c* p ) { p->Cidx().SetKillHook ( pAccum ); } );
}

void RtIndex_c::SetKillHookFor ( IndexSegment_c* pAccum, VecTraits_T<int> dDiskChunkIDs ) const
{
	// that ensures no concurrency
	ScopedScheduler_c tSerialFiber ( m_tRtChunks.SerialChunkAccess() );
	ProcessDiskChunkByID ( dDiskChunkIDs, [pAccum] ( const DiskChunk_c* p ) { p->Cidx().SetKillHook ( pAccum ); } );
}

// track internals of merge to catch critical points necessary for concurrent kill and update
class RTMergeCb_c: public MergeCb_c
{
	KillAccum_t m_tKilledWhileMerge;
	RtIndex_c* m_pOwner;
	CSphVector<int> m_dTrackedChunks;

public:
	explicit RTMergeCb_c ( std::atomic<bool>* pStop, RtIndex_c* pOwner )
		: MergeCb_c ( pStop )
		, m_pOwner ( pOwner )
	{}

	void SetEvent ( Event_e eEvent, int64_t iPayload ) override
	{
		assert ( m_pOwner );
		RTLOGV << "SetEvent (" << eEvent << ", " << iPayload << ")";
		switch ( eEvent )
		{
		case E_COLLECT_START:
			m_dTrackedChunks.Add ( (int)iPayload );
			m_pOwner->SetKillHookFor ( &m_tKilledWhileMerge, (int)iPayload );
			break;
		case E_MERGEATTRS_START: // enter serial state/rlock
			m_pOwner->ProcessDiskChunkByID ( (int)iPayload, [] ( const DiskChunk_c* p ) NO_THREAD_SAFETY_ANALYSIS { p->m_tLock.ReadLock(); } );
			break;
		case E_MERGEATTRS_FINISHED: // leave serial state/rlock
			m_pOwner->ProcessDiskChunkByID ( (int)iPayload, [] ( const DiskChunk_c* p ) NO_THREAD_SAFETY_ANALYSIS { p->m_tLock.Unlock(); } );
			break;
		default:
			break;
		}
	}

	~RTMergeCb_c() override
	{
		assert ( m_pOwner );
		m_pOwner->SetKillHookFor ( nullptr, m_dTrackedChunks );
	}

	CSphVector<DocID_t> GetKilled()
	{
		CSphVector<DocID_t> dDocs;
		m_tKilledWhileMerge.m_dDocids.SwapData ( dDocs );
		dDocs.Uniq();
		return dDocs;
	}

	bool HasKilled() const
	{
		return !m_tKilledWhileMerge.m_dDocids.IsEmpty();
	}
};

static StringBuilder_c & operator<< ( StringBuilder_c & dOut, MergeCb_c::Event_e eVal )
{
	switch ( eVal )
	{
	case MergeCb_c::E_IDLE: return dOut << "E_IDLE";
	case MergeCb_c::E_COLLECT_START: return dOut << "E_COLLECT_START";
	case MergeCb_c::E_COLLECT_FINISHED: return dOut << "E_COLLECT_FINISHED";
	case MergeCb_c::E_MERGEATTRS_START: return dOut << "E_MERGEATTRS_START";
	case MergeCb_c::E_MERGEATTRS_FINISHED: return dOut << "E_MERGEATTRS_FINISHED";
	case MergeCb_c::E_KEYWORDS: return dOut << "E_KEYWORDS";
	case MergeCb_c::E_FINISHED: return dOut << "E_FINISHED";
	default: dOut.Sprintf ( "UNKNWN_", (int)eVal );
	}
	return dOut;
}

static StringBuilder_c& operator<< ( StringBuilder_c& dOut, OptimizeTask_t::OptimizeVerb_e eVerb )
{
	switch ( eVerb )
	{
	case OptimizeTask_t::eManualOptimize: return dOut << "eManualOptimize";
	case OptimizeTask_t::eDrop: return dOut << "eDrop";
	case OptimizeTask_t::eCompress: return dOut << "eCompress";
	case OptimizeTask_t::eSplit: return dOut << "eSplit";
	case OptimizeTask_t::eMerge: return dOut << "eMerge";
	case OptimizeTask_t::eAutoOptimize: return dOut << "eAutoOptimize";
	default: dOut.Sprintf ( "eUnknown_", (int)eVerb );
	}
	return dOut;
}

static StringBuilder_c& operator<< ( StringBuilder_c& dOut, const OptimizeTask_t& tTask )
{

	switch ( tTask.m_eVerb )
	{
	case OptimizeTask_t::eManualOptimize: return dOut << "eManualOptimize, cutoff = " << tTask.m_iCutoff;
	case OptimizeTask_t::eDrop: return dOut << "eDrop " << tTask.m_iFrom;
	case OptimizeTask_t::eCompress: return dOut << "eCompress " << tTask.m_iFrom;
	case OptimizeTask_t::eSplit: return dOut << "eSplit " << tTask.m_iFrom << " filtering with '" << tTask.m_sUvarFilter << "'";
	case OptimizeTask_t::eMerge: return dOut << "eMerge from " << tTask.m_iFrom << " to " << tTask.m_iTo;
	case OptimizeTask_t::eAutoOptimize: return dOut << "eAutoOptimize, cutoff = " << tTask.m_iCutoff;
	default: return dOut << tTask.m_eVerb << " cutoff=" << tTask.m_iCutoff << " From=" << tTask.m_iFrom << " To=" << tTask.m_iTo << " VarFilter= '" << tTask.m_sUvarFilter << "' bByOrder=" << tTask.m_bByOrder;
	}
}

//////////////////////////////////////////////////////////////////////////
// OPTIMIZE
//////////////////////////////////////////////////////////////////////////

static int64_t GetEffectiveSize (const CSphIndexStatus& tStatus, int64_t iTotalDocs )
{
	if ( !tStatus.m_iDead ) // no docs killed
		return tStatus.m_iDiskUse;

	if ( tStatus.m_iDead==iTotalDocs ) // all docs killed
		return 0;

	// has killed docs: use naive formulae, simple part of used disk proportional to partial of alive docs
	auto fPart = double ( tStatus.m_iDead ) / double ( iTotalDocs );
	return int64_t ( tStatus.m_iDiskUse * ( 1.0f-fPart ) );
}

static int64_t GetChunkSize ( const CSphIndex& tIndex )
{
	CSphIndexStatus tDisk;
	tIndex.GetStatus ( &tDisk );
	return GetEffectiveSize ( tDisk, tIndex.GetStats().m_iTotalDocuments );
}

struct ChunkAndSize_t
{
	int m_iId;
	int64_t m_iSize;
};

static ChunkAndSize_t GetNextSmallestChunkByID ( const DiskChunkVec_c& dDiskChunks, int iChunkID )
{
	int iRes = -1;
	int64_t iLastSize = INT64_MAX;
	for ( const auto& pDiskChunk : dDiskChunks )
	{
		if ( pDiskChunk->m_bOptimizing.load(std::memory_order_relaxed) )
			continue;
		const CSphIndex& dDiskChunk = pDiskChunk->Cidx();
		int64_t iSize = GetChunkSize ( dDiskChunk );
		if ( iSize < iLastSize && iChunkID != dDiskChunk.m_iChunk )
		{
			iLastSize = iSize;
			iRes = dDiskChunk.m_iChunk;
		}
	}
	return { iRes, iLastSize };
}

static int GetNumOfOptimizingNow ( const DiskChunkVec_c& dDiskChunks )
{
	return (int)dDiskChunks.count_of ( [] ( auto& i ) { return i->m_bOptimizing.load ( std::memory_order_relaxed ); } );
}

int RtIndex_c::ChunkIDByChunkIdx ( int iChunkIdx ) const
{
	if ( iChunkIdx < 0 )
		return iChunkIdx;

	return m_tRtChunks.DiskChunks()->operator[] ( iChunkIdx )->Cidx().m_iChunk;
}

void RtIndex_c::DropDiskChunk ( int iChunkID )
{
	sphLogDebug( "rt optimize: index %s: drop disk chunk %d",  m_sIndexName.cstr(), iChunkID );

	// work in serial fiber
	ScopedScheduler_c tSerialFiber ( m_tRtChunks.SerialChunkAccess() );
	{
		RtWriter_c tChangeset ( m_tRtChunks );
		tChangeset.InitDiskChunks ( RtWriter_c::empty );
		auto pChunks = m_tRtChunks.RtDataUnl().m_pChunks;
		for ( auto& pChunk : *pChunks )
			if ( iChunkID == pChunk->Cidx().m_iChunk )
				pChunk->m_bFinallyUnlink = true;
			else
				tChangeset.m_pNewDiskChunks->Add ( pChunk );
	}
	SaveMeta();
}

// perform merge, preload result, rename to final chunk and return preallocated result scheduled to dispose
ConstDiskChunkRefPtr_t RtIndex_c::MergeDiskChunks ( const char* szParentAction, const ConstDiskChunkRefPtr_t& pChunkA, const ConstDiskChunkRefPtr_t& pChunkB, CSphIndexProgress& tProgress, VecTraits_T<CSphFilterSettings> dFilters )
{
	CSphString sError;

	const CSphIndex& tChunkA = pChunkA->Cidx();
	const CSphIndex& tChunkB = pChunkB->Cidx();
	CSphString sFirst = tChunkA.GetFilename();

	// fixme! implicit dependency (merging creates file suffixed .tmp, that is implicit here.
	CSphString sProcessed;
	sProcessed.SetSprintf ( "%s.tmp", sFirst.cstr() );

	ConstDiskChunkRefPtr_t pChunk;

	// note: klist for merged chunk will be attached during merge at the moment of copying alive rows.
	if ( !sphMerge ( &tChunkA, &tChunkB, dFilters, tProgress, sError ) )
	{
		sphWarning ( "rt %s: index %s: failed to %s %s (error %s)", szParentAction, m_sIndexName.cstr(), dFilters.IsEmpty() ? "merge" : "split", sFirst.cstr(), sError.cstr() );
		return pChunk;
	}

	// check forced exit after long operation
	if ( tProgress.GetMergeCb().NeedStop() )
		return pChunk;

	auto fnFnameBuilder = GetIndexFilenameBuilder();
	CSphScopedPtr<FilenameBuilder_i> pFilenameBuilder { fnFnameBuilder ? fnFnameBuilder ( m_sIndexName.cstr() ) : nullptr };

	// prealloc new (optimized) chunk
	CSphString sChunk;
	sChunk.SetSprintf ( "%s.tmp", sFirst.cstr() );

	StrVec_t dWarnings; // FIXME! report warnings
	pChunk = DiskChunk_c::make ( PreallocDiskChunk ( sChunk.cstr(), tChunkA.m_iChunk, pFilenameBuilder.Ptr(), dWarnings, sError, tChunkA.GetName() ) );

	if ( pChunk )
		pChunk->m_bFinallyUnlink = true; // on destroy files will be deleted. Caller must explicitly reset this flag if chunk is usable
	else
		sphWarning ( "rt %s: index %s: failed to prealloc", szParentAction, m_sIndexName.cstr() );

	return pChunk;
}

bool RtIndex_c::RenameOptimizedChunk ( const ConstDiskChunkRefPtr_t& pChunk, const char* szParentAction )
{
	if ( !pChunk )
		return false;

	CSphIndex& tChunk = pChunk->CastIdx(); // const breakage is ok since we don't yet published the index
	// prepare new chunk to publish
	int iResID = m_tRtChunks.MakeChunkId();
	auto sNewchunk = MakeChunkName ( iResID );
	tChunk.m_iChunk = iResID;

	// rename merged disk chunk to valid chunk name
	if ( tChunk.Rename ( sNewchunk.cstr() ) )
		return true;

	sphWarning ( "rt %s: index %s: processed to cur rename failed (error %s)", szParentAction, m_sIndexName.cstr(), tChunk.GetLastError().cstr() );
	return false;
}

bool RtIndex_c::PublishMergedChunks ( const char* szParentAction, std::function<bool ( int, DiskChunkVec_c& )>&& fnPusher ) REQUIRES ( m_tRtChunks.SerialChunkAccess() )
{
	bool bReplaced = false;
	RtWriter_c tChangeset ( m_tRtChunks );
	tChangeset.InitDiskChunks ( RtWriter_c::empty );
	for ( auto& pChunk : *m_tRtChunks.DiskChunks() )
	{
		if ( fnPusher ( pChunk->Cidx().m_iChunk, *tChangeset.m_pNewDiskChunks ) )
			bReplaced = true;
		else
			tChangeset.m_pNewDiskChunks->Add ( pChunk );
	}

	if ( !bReplaced )
	{
		sphWarning ( "rt %s: index %s: unable to locate victim chunk after merge, leave everything unchanged", szParentAction, m_sIndexName.cstr() );
		tChangeset.m_pNewDiskChunks = nullptr; // discard changes, i.e. disk chunk set will NOT be modified
		return false;
	}
	return true;
}

static int64_t NumAliveDocs ( const CSphIndex& dChunk )
{
	CSphIndexStatus tStatus;
	dChunk.GetStatus ( &tStatus );
	return dChunk.GetStats().m_iTotalDocuments - tStatus.m_iDead;
}

void RtIndex_c::BinlogCommit ( RtSegment_t * pSeg, const CSphVector<DocID_t> & dKlist ) REQUIRES ( pSeg->m_tLock )
{
	Binlog::Commit ( Binlog::COMMIT, &m_iTID, m_sIndexName.cstr(), false, [pSeg,&dKlist,this] (CSphWriter& tWriter) REQUIRES ( pSeg->m_tLock )
	{
		if ( !pSeg || !pSeg->m_uRows )
			tWriter.ZipOffset ( 0 );
		else
		{
			tWriter.ZipOffset ( pSeg->m_uRows );
			Binlog::SaveVector ( tWriter, pSeg->m_dWords );
			tWriter.ZipOffset ( pSeg->m_dWordCheckpoints.GetLength() );
			if ( !m_bKeywordDict )
			{
				ARRAY_FOREACH ( i, pSeg->m_dWordCheckpoints )
				{
					tWriter.ZipOffset ( pSeg->m_dWordCheckpoints[i].m_iOffset );
					tWriter.ZipOffset ( pSeg->m_dWordCheckpoints[i].m_uWordID );
				}
			} else
			{
				const char * pBase = (const char *)pSeg->m_dKeywordCheckpoints.Begin();
				ARRAY_FOREACH ( i, pSeg->m_dWordCheckpoints )
				{
					tWriter.ZipOffset ( pSeg->m_dWordCheckpoints[i].m_iOffset );
					tWriter.ZipOffset ( pSeg->m_dWordCheckpoints[i].m_sWord - pBase );
				}
			}

			Binlog::SaveVector ( tWriter, pSeg->m_dDocs );
			Binlog::SaveVector ( tWriter, pSeg->m_dHits );
			Binlog::SaveVector ( tWriter, pSeg->m_dRows );
			Binlog::SaveVector ( tWriter, pSeg->m_dBlobs );
			Binlog::SaveVector ( tWriter, pSeg->m_dKeywordCheckpoints );

			tWriter.PutByte ( pSeg->m_pDocstore.Ptr() ? 1 : 0 );
			if ( pSeg->m_pDocstore.Ptr() )
				pSeg->m_pDocstore->Save(tWriter);

			tWriter.PutByte ( pSeg->m_pColumnar.Ptr() ? 1 : 0 );
			if ( pSeg->m_pColumnar.Ptr() )
				pSeg->m_pColumnar->Save(tWriter);
		}

		Binlog::SaveVector ( tWriter, dKlist );
	});
}

Binlog::CheckTnxResult_t RtIndex_c::ReplayCommit ( CSphReader& tReader, Binlog::CheckTxn_fn&& fnCanContinue )
{
	CSphRefcountedPtr<RtSegment_t> pSeg;
	CSphVector<DocID_t> dKlist;

	DWORD uRows = tReader.UnzipOffset();
	if ( uRows )
	{
		pSeg = new RtSegment_t(uRows);
		FakeWL_t _ ( pSeg->m_tLock );
		pSeg->m_uRows = uRows;
		pSeg->m_tAliveRows.store ( uRows, std::memory_order_relaxed );

		Binlog::LoadVector ( tReader, pSeg->m_dWords );
		pSeg->m_dWordCheckpoints.Resize ( (int) tReader.UnzipOffset() ); // FIXME! sanity check
		ARRAY_FOREACH ( i, pSeg->m_dWordCheckpoints )
		{
			pSeg->m_dWordCheckpoints[i].m_iOffset = (int) tReader.UnzipOffset();
			pSeg->m_dWordCheckpoints[i].m_uWordID = (SphWordID_t )tReader.UnzipOffset();
		}
		Binlog::LoadVector ( tReader, pSeg->m_dDocs );
		Binlog::LoadVector ( tReader, pSeg->m_dHits );
		Binlog::LoadVector ( tReader, pSeg->m_dRows );
		Binlog::LoadVector ( tReader, pSeg->m_dBlobs );
		Binlog::LoadVector ( tReader, pSeg->m_dKeywordCheckpoints );

		bool bHaveDocstore = !!tReader.GetByte();
		if ( bHaveDocstore )
		{
			pSeg->SetupDocstore ( &(GetInternalSchema()) );
			assert ( pSeg->m_pDocstore.Ptr() );
			Verify ( pSeg->m_pDocstore->Load(tReader) );
		}

		bool bHaveColumnar = !!tReader.GetByte();
		if ( bHaveColumnar )
		{
			CSphString sError;
			pSeg->m_pColumnar = CreateColumnarRT ( GetInternalSchema(), tReader, sError );

			if ( !pSeg->m_pColumnar )
			{
				sphWarning ( "binlog: commit: columnar load error: %s",
							 tReader.GetErrorMessage().cstr() );
			}
		}

		pSeg->BuildDocID2RowIDMap ( GetInternalSchema() );
	}

	Binlog::LoadVector ( tReader, dKlist );

	Binlog::CheckTnxResult_t tRes = fnCanContinue();
	if ( tRes.m_bValid && tRes.m_bApply )
	{
		// in case dict=keywords
		// + cook checkpoint
		// + build infixes
		if ( IsWordDict() && pSeg )
		{
			FixupSegmentCheckpoints ( pSeg );
				BuildSegmentInfixes ( pSeg, GetDictionary()->HasMorphology(), IsWordDict(), GetSettings().m_iMinInfixLen,
				GetWordCheckoint(), ( GetMaxCodepointLength()>1 ), GetSettings().m_eHitless );
		}

		// actually replay
		FakeRL_t _ ( pSeg.operator RtSegment_t*()->m_tLock);
		CommitReplayable ( pSeg, dKlist );
		tRes.m_bApply = true;
	}
	return tRes;
}

Binlog::CheckTnxResult_t RtIndex_c::ReplayReconfigure ( CSphReader& tReader, CSphString & sError, Binlog::CheckTxn_fn&& fnCanContinue )
{
	CSphTokenizerSettings tTokenizerSettings;
	CSphDictSettings tDictSettings;
	CSphEmbeddedFiles tEmbeddedFiles;
	CSphScopedPtr<const FilenameBuilder_i> pFilenameBuilder ( nullptr );
	if ( GetIndexFilenameBuilder() )
		pFilenameBuilder = GetIndexFilenameBuilder() ( m_sIndexName.cstr() );

	CSphReconfigureSettings tSettings;
	LoadIndexSettings ( tSettings.m_tIndex, tReader, INDEX_FORMAT_VERSION );
	if ( !tSettings.m_tTokenizer.Load ( pFilenameBuilder.Ptr(), tReader, tEmbeddedFiles, sError ) )
	{
		sError.SetSprintf ( "failed to load settings, %s", sError.cstr() );
		return {};
	}

	tSettings.m_tDict.Load ( tReader, tEmbeddedFiles, sError );
	tSettings.m_tFieldFilter.Load(tReader);

	Binlog::CheckTnxResult_t tRes = fnCanContinue();
	if ( tRes.m_bValid && tRes.m_bApply )
	{
		sError = "";
		CSphReconfigureSetup tSetup;
		StrVec_t dWarnings;
		bool bSame = IsSameSettings ( tSettings, tSetup, dWarnings, sError );

		if ( !sError.IsEmpty() )
			sphWarning ( "binlog: reconfigure: wrong settings (index=%s, error=%s)", m_sIndexName.cstr(), sError.cstr() );

		if ( !bSame )
			tRes.m_bApply = Reconfigure ( tSetup );
	}
	return tRes;
}

Binlog::CheckTnxResult_t RtIndex_c::ReplayTxn (Binlog::Blop_e eOp,CSphReader& tReader, CSphString & sError, Binlog::CheckTxn_fn&& fnCanContinue)
{
	switch ( eOp )
	{
	case Binlog::COMMIT: return ReplayCommit(tReader, std::move(fnCanContinue));
	case Binlog::RECONFIGURE: return ReplayReconfigure(tReader, sError, std::move(fnCanContinue));
	default: assert (false && "unknown op provided to replay");
	}
	return {};
}


bool RtIndex_c::SkipOrDrop ( int iChunkID, const CSphIndex& dChunk, bool bCheckAlive, int &iAffected )
{
	auto iTotalDocs = dChunk.GetStats().m_iTotalDocuments;
	auto iAliveDocs = NumAliveDocs ( dChunk );

	// all docs killed
	if ( !iAliveDocs )
	{
		sphLogDebug ( "common merge - drop %d, all (" INT64_FMT ") killed", iChunkID, iTotalDocs );
		DropDiskChunk ( iChunkID );
		++iAffected;
		return true;
	}

	// no docs killed
	if ( bCheckAlive && iTotalDocs == iAliveDocs )
	{
		sphLogDebug ( "common merge - skip compressing %d, no killed", iChunkID );
		return true;
	}

	++iAffected;
	return false;
}

bool RtIndex_c::CompressOneChunk ( int iChunkID, int& iAffected )
{
	auto pVictim = m_tRtChunks.DiskChunkByID ( iChunkID );
	if ( !pVictim )
	{
		sphWarning ( "rt optimize: index %s: compress of chunk %d failed, no chunk with such ID!", m_sIndexName.cstr(), iChunkID );
		return false;
	}
	const CSphIndex& tVictim = pVictim->Cidx();
	if ( SkipOrDrop ( iChunkID, tVictim, true, iAffected ) )
		return true;

	sphLogDebug ( "compress %d (%d kb)", iChunkID, (int)( GetChunkSize ( tVictim ) / 1024 ) );

	pVictim->m_bOptimizing.store ( true, std::memory_order_relaxed );
	auto tResetOptimizing = AtScopeExit ( [pVictim] { pVictim->m_bOptimizing.store ( false, std::memory_order_relaxed ); } );

	// merge data to disk ( data is constant during that phase )
	RTMergeCb_c tMonitor ( &m_bOptimizeStop, this );
	CSphIndexProgress tProgress ( &tMonitor );
	auto pCompressed = MergeDiskChunks ( "compress", pVictim, pVictim, tProgress, { nullptr, 0 } );

	auto tFinallyStopCollectingUpdates = AtScopeExit ( [pVictim] { pVictim->CastIdx().ResetPostponedUpdates(); } );

	if ( !pCompressed )
		return false;

	if ( !RenameOptimizedChunk ( pCompressed, "compress" ) )
		return false;

	if ( tMonitor.HasKilled() && tMonitor.NeedStop() ) // if we should interrupt now, but if no kills, can continue.
		return false;

	CSphIndex& tCompressed = pCompressed->CastIdx(); // const breakage is ok since we don't yet published the index

	// going to modify list of chunks; so fall into serial fiber
	ScopedScheduler_c tSerialFiber ( m_tRtChunks.SerialChunkAccess() );

	// reset kill hook explicitly to override default order of destruction
	SetKillHookFor ( nullptr, iChunkID );

	// and also apply already collected kills
	int iKilled = tCompressed.KillMulti ( tMonitor.GetKilled() );

	// and also apply collected updates
	auto& dUpdates = pVictim->CastIdx().m_dPostponedUpdates;
	if ( !dUpdates.IsEmpty() )
	{
		tCompressed.UpdateAttributesOffline ( dUpdates, &tCompressed );
		dUpdates.Reset();
	}

	if ( !PublishMergedChunks ( "compress", [iChunkID, pCompressed] ( int iChunk, DiskChunkVec_c& tRes ) {
			if ( iChunk==iChunkID )
				tRes.Add ( pCompressed );
			return iChunk==iChunkID; } ) )
		return false;

	sphLogDebug ( "compressed a=%s, new=%s, killed=%d", tVictim.GetFilename(), tCompressed.GetFilename(), iKilled );
	pVictim->m_bFinallyUnlink = true;
	pCompressed->m_bFinallyUnlink = false;
	SaveMeta();
	Preread();
	++iAffected;
	return true;
}

// catch common cases where we can't work at all (erroneous settings, etc.), or can redirect to another action
bool RtIndex_c::SplitOneChunkFast ( int iChunkID, const char* szUvarFilter, bool& bResult, int& iAffected )
{
	if ( !szUvarFilter )
	{
		sphLogDebug ( "zero filter provided to split chunk %d, perform simple compress", iChunkID );
		bResult = CompressOneChunk ( iChunkID, iAffected );
		return true;
	}

	auto pVictim = m_tRtChunks.DiskChunkByID ( iChunkID );
	if ( !pVictim )
	{
		sphWarning ( "rt optimize: index %s: split of chunk %d failed, no chunk with such ID!", m_sIndexName.cstr(), iChunkID );
		bResult = false;
		return true;
	}
	const CSphIndex& tVictim = pVictim->Cidx();
	if ( SkipOrDrop ( iChunkID, tVictim, false, iAffected ) )
	{
		bResult = true;
		return true;
	}

	if ( !NumAliveDocs ( tVictim ) )
	{
		sphLogDebug ( "chunk empty, nothing to split" );
		bResult = false;
		return true;
	}

	// create filter by @uservar - break if it is not available.
	if ( !UservarsAvailable() )
	{
		sphLogDebug ( "no global variables found" );
		bResult = false;
		return true;
	}

	const UservarIntSet_c pUservar = Uservars ( szUvarFilter );
	if ( !pUservar )
	{
		sphLogDebug ( "undefined global variable '%s'", szUvarFilter );
		bResult = false;
		return true;
	}
	return false;
}

// compress iChunk with filter id by uservar include and exclude,
// then replace original chunk with one of pieces, and insert second piece just after the first.
bool RtIndex_c::SplitOneChunk ( int iChunkID, const char* szUvarFilter, int& iAffected )
{
	bool bResult;
	if ( SplitOneChunkFast ( iChunkID, szUvarFilter, bResult, iAffected ) )
		return bResult;

	auto pVictim = m_tRtChunks.DiskChunkByID ( iChunkID );
	assert ( pVictim && "non-existent chunks should be already rejected by SplitOneChunkFast" );
	CSphIndex& tVictim = pVictim->CastIdx(); // non-const need to invoke 'merge'
	sphLogDebug ( "split %d (%d kb) with %s", iChunkID, (int)( GetChunkSize ( tVictim ) / 1024 ), szUvarFilter );

	pVictim->m_bOptimizing.store ( true, std::memory_order_relaxed );
	auto tResetOptimizing = AtScopeExit ( [pVictim] { pVictim->m_bOptimizing.store ( false, std::memory_order_relaxed ); } );

	const UservarIntSet_c pUservar = Uservars ( szUvarFilter );
	assert ( pUservar ); // detailed check already performed in splitOneChunkFast

	// create negative (exclusion) filter
	CSphVector<CSphFilterSettings> dFilters;
	auto & dFilter = dFilters.Add ();

	dFilter.m_sAttrName = "id";
	dFilter.m_eType = SPH_FILTER_VALUES;
	dFilter.SetExternalValues ( pUservar->Begin(), pUservar->GetLength() );
	dFilter.m_bExclude = true;

	// prepare for real split (merge)
	RTMergeCb_c tMonitor ( &m_bOptimizeStop, this );
	CSphIndexProgress tProgress ( &tMonitor );

	auto iOriginallyAlive = NumAliveDocs ( tVictim );

	// get 1-st chunk - one which doesn't match filter the filter
	auto pChunkE = MergeDiskChunks ( "1-st part of split", pVictim, pVictim, tProgress, dFilters );

	auto tFinallyStopCollectingUpdatesE = AtScopeExit ( [pVictim] { pVictim->CastIdx().ResetPostponedUpdates();	} );

	// check forced exit after long operation (that is - after merge)
	if ( !pChunkE || tMonitor.NeedStop() )
		return false;

	CSphIndex& tIndexE = pChunkE->CastIdx(); // const breakage is ok since we don't yet published the index

	// if nothing is alive after filter applied - fast break, nothing to do.
	auto iExcludedAlive = NumAliveDocs ( tIndexE );
	if ( !iExcludedAlive )
	{
		// fool protect - either nothing, either all is filtered. No need to continue.
		sphLogDebug ( "filter selected nothing, no point to split" );
		return false;
	} else if ( iExcludedAlive == iOriginallyAlive )
	{
		// fool protect - either nothing, either all is filtered. No need to continue.
		sphLogDebug ( "filter selected everything, no point to split" );
		return false;
	}

	if ( !RenameOptimizedChunk ( pChunkE, "1-st part of split" ) )
		return false;

	// prepare <I>ncluded chunk - one with included docs, it will be placed instead of original one
	dFilter.m_bExclude = false;
	auto pChunkI = MergeDiskChunks ( "2-nd part of split", pVictim, pVictim, tProgress, dFilters );

	// check forced exit after long operation (that is - after merge)
	if ( !pChunkI || tMonitor.NeedStop() )
		return false;

	CSphIndex& tIndexI = pChunkI->CastIdx(); // const breakage is ok since we don't yet published the index

	if ( tMonitor.HasKilled() && tMonitor.NeedStop() ) // if we should interrupt now, but if no kills, can continue.
		return false;

	if ( !RenameOptimizedChunk ( pChunkI, "2-nd part of split" ) )
		return false;

	// going to modify list of chunks; so fall into serial fiber
	ScopedScheduler_c tSerialFiber ( m_tRtChunks.SerialChunkAccess() );

	// reset kill hook explicitly to override default order of destruction
	SetKillHookFor ( nullptr, iChunkID );

	// apply collected kill-list before including chunks to the set
	// as we are in serial worker, that is safe here; no new kills may arrive.
	int iKilled = 0;
	if ( tMonitor.HasKilled() )
	{
		auto dKilled = tMonitor.GetKilled();
		iKilled += tIndexE.KillMulti ( dKilled );
		iKilled += tIndexI.KillMulti ( dKilled );
	}

	// and also apply collected updates
	auto& dUpdates = pVictim->CastIdx().m_dPostponedUpdates;
	if ( !dUpdates.IsEmpty() )
	{
		tIndexI.UpdateAttributesOffline ( dUpdates, &tIndexI );
		tIndexE.UpdateAttributesOffline ( dUpdates, &tIndexE );
		dUpdates.Reset();
	}

	if ( !PublishMergedChunks ( "split",
				 [iChunkID, pChunkI, pChunkE] ( int iChunk, DiskChunkVec_c& tRes ) {
		if ( iChunk==iChunkID )
		{
			tRes.Add ( pChunkI );
			tRes.Add ( pChunkE );
			return true;
		}
		return false; } ) )
		return false;

	sphLogDebug ( "split a=%s, b=%s, killed=%d", tIndexE.GetFilename(), tIndexI.GetFilename(), iKilled );
	pVictim->m_bFinallyUnlink = true;
	pChunkI->m_bFinallyUnlink = false;
	pChunkE->m_bFinallyUnlink = false;
	SaveMeta();
	Preread();
	++iAffected;
	return true;
}

bool RtIndex_c::MergeTwoChunks ( int iAID, int iBID )
{
	auto pA = m_tRtChunks.DiskChunkByID ( iAID );
	if ( !pA )
	{
		sphWarning ( "rt optimize: index %s: merge chunks %d and %d failed, chunk ID %d is not valid!", m_sIndexName.cstr(), iAID, iBID, iAID );
		return false;
	}

	auto pB = m_tRtChunks.DiskChunkByID ( iBID );
	if ( !pB )
	{
		sphWarning ( "rt optimize: index %s: merge chunks %d and %d failed, chunk ID %d is not valid!", m_sIndexName.cstr(), iAID, iBID, iBID );
		return false;
	}

	pA->m_bOptimizing.store ( true, std::memory_order_relaxed );
	auto tResetOptimizingA = AtScopeExit ( [pA] { pA->m_bOptimizing.store ( false, std::memory_order_relaxed ); } );
	pB->m_bOptimizing.store ( true, std::memory_order_relaxed );
	auto tResetOptimizingB = AtScopeExit ( [pB] { pB->m_bOptimizing.store ( false, std::memory_order_relaxed ); } );

	sphLogDebug ( "common merge - merging %d (%d kb) with %d (%d kb)",
			iAID,
			(int)( GetChunkSize ( pA->Cidx() ) / 1024 ),
			iBID,
			(int)( GetChunkSize ( pB->Cidx() ) / 1024 ) );

	// merge data to disk ( data is constant during that phase )
	RTMergeCb_c tMonitor ( &m_bOptimizeStop, this );
	CSphIndexProgress tProgress ( &tMonitor );

	// get 1-st chunk - one which doesn't match filter the filter
	auto pMerged = MergeDiskChunks ( "common merge", pA, pB, tProgress, { nullptr, 0 } );

	auto tFinallyStopCollectingUpdates = AtScopeExit ( [pA, pB] {
		pA->CastIdx().ResetPostponedUpdates();
		pB->CastIdx().ResetPostponedUpdates();
	} );

	// check forced exit after long operation (that is - after merge)
	if ( !pMerged || tMonitor.NeedStop() )
		return false;

	if ( !RenameOptimizedChunk ( pMerged, "common merge" ) )
		return false;

	CSphIndex& tMerged = pMerged->CastIdx(); // const breakage is ok since we don't yet published the index

	// going to modify list of chunks; so fall into serial fiber
	ScopedScheduler_c tSerialFiber ( m_tRtChunks.SerialChunkAccess() );

	// reset kill hook explicitly to override default order of destruction
	SetKillHookFor ( nullptr, iAID );
	SetKillHookFor ( nullptr, iBID );

	// apply collected kill-list before including chunks to the set
	// as we are in serial worker, that is safe here; no new kills may arrive.
	int iKilled = 0;
	if ( tMonitor.HasKilled() )
		iKilled = tMerged.KillMulti ( tMonitor.GetKilled() );

	// and also apply collected updates
	CSphVector<ConstDiskChunkRefPtr_t> tUpdated;
	tUpdated.Add ( pA );
	tUpdated.Add ( pB );
	auto dUpdates = GatherUpdates::FromChunksOrSegments ( tUpdated );
	if ( !dUpdates.IsEmpty() )
	{
		tMerged.UpdateAttributesOffline ( dUpdates, &tMerged );
		dUpdates.Reset();
	}

	if ( !PublishMergedChunks ( "optimize", [iAID, iBID, pMerged] ( int iChunk, DiskChunkVec_c& tRes ) {
			 if ( iChunk == iBID )
				 tRes.Add ( pMerged );
			 return ( iChunk == iAID || iChunk == iBID );
		 } ) )
		return false;

	sphLogDebug ( "optimized a=%s, b=%s, new=%s, killed=%d", pA->Cidx().GetFilename(), pB->Cidx().GetFilename(), tMerged.GetFilename(), iKilled );

	pA->m_bFinallyUnlink = true;
	pB->m_bFinallyUnlink = true;
	pMerged->m_bFinallyUnlink = false;
	SaveMeta();
	Preread();
	return true;
}

void RtIndex_c::StopOptimize()
{
	m_bOptimizeStop.store ( true, std::memory_order_release );
	m_tOptimizeRuns.WaitFor ( [] ( int i ) { return i <= 0; } );
}

void RtIndex_c::CommonMerge ( std::function<bool ( OptimizeTask_t& )>&& fnSelector )
{
	if ( m_bOptimizeStop.load ( std::memory_order_acquire ) )
		return;

	sphLogDebug ( "rt optimize: index %s: optimization started", m_sIndexName.cstr() );

	int64_t tmStart = sphMicroTimer();
	int iChunks = 0;

	{
		m_tOptimizeRuns.Inc();
		auto tDeferDec = AtScopeExit ( [this] { m_tOptimizeRuns.Dec(); } );

		OptimizeTask_t tAction;
		bool bBreak = false;
		while ( fnSelector ( tAction ) && !sphInterrupted () && !m_bOptimizeStop && !bBreak )
		{
			RTDLOG << "CommonMerge step " << tAction;
			switch ( tAction.m_eVerb )
			{
			case OptimizeTask_t::eDrop:
				DropDiskChunk ( tAction.m_iFrom );
				++iChunks;
				break;
			case OptimizeTask_t::eCompress:
				if (!CompressOneChunk ( tAction.m_iFrom, iChunks ))
					bBreak = true;
				break;
			case OptimizeTask_t::eSplit:
				if (!SplitOneChunk ( tAction.m_iFrom, tAction.m_sUvarFilter.cstr(), iChunks ))
					bBreak = true;
				break;
			default:
				if ( !MergeTwoChunks(tAction.m_iFrom,tAction.m_iTo) )
					bBreak = true;
				++iChunks;
			}
		}
	}

	int64_t tmPass = sphMicroTimer() - tmStart;
	int iDiskChunks = m_tRtChunks.GetDiskChunksCount();

	if ( sphInterrupted() )
		LogWarning ( "rt: index %s: optimization terminated chunk(s) %d ( left %d ) in %.3t", m_sIndexName.cstr(), iChunks, iDiskChunks, tmPass );
	else if ( iChunks > 0 )
		LogInfo ( "rt: index %s: optimized %s chunk(s) %d ( left %d ) in %.3t", m_sIndexName.cstr(), g_bProgressiveMerge ? "progressive" : "regular", iChunks, iDiskChunks, tmPass );
}

bool RtIndex_c::CheckValidateChunk ( int& iChunk, int iChunks, bool bByOrder ) const
{
	if ( bByOrder )
	{
		if ( iChunk >= iChunks || iChunk < 0 )
		{
			sphWarning ( "rt: index %s: Optimize step: provided chunk %d is out of range [0..%d]", m_sIndexName.cstr(), iChunk, iChunks - 1 );
			return false;
		}
		iChunk = ChunkIDByChunkIdx ( iChunk );
	}
	auto pChunk = m_tRtChunks.DiskChunkByID ( iChunk );
	if ( !pChunk )
	{
		sphWarning ( "rt: index %s: Optimize step: provided chunk ID %d is invalid", m_sIndexName.cstr(), iChunk );
		return false;
	}

	if ( pChunk->m_bOptimizing.load ( std::memory_order_relaxed ) )
	{
		sphWarning ( "rt: index %s: Optimize step: provided chunk %d is now occupied in another optimize operation", m_sIndexName.cstr(), iChunk );
		return false;
	}
	return true;
}

bool RtIndex_c::CheckValidateOptimizeParams ( OptimizeTask_t& tTask ) const
{
	auto iChunks = m_tRtChunks.GetDiskChunksCount();
	switch ( tTask.m_eVerb )
	{
	case OptimizeTask_t::eMerge:
		if ( !CheckValidateChunk ( tTask.m_iTo, iChunks, tTask.m_bByOrder ) )
			return false;

		// no break; check also m_iFrom param then
	case OptimizeTask_t::eDrop:
	case OptimizeTask_t::eCompress:
	case OptimizeTask_t::eSplit:
		if ( !CheckValidateChunk ( tTask.m_iFrom, iChunks, tTask.m_bByOrder ) )
			return false;
	default: break;
	}
	tTask.m_bByOrder=false;
	return true;
}

void RtIndex_c::Optimize ( OptimizeTask_t tTask )
{
	RTDLOG << "Optimize invoked with " << tTask;
	if ( !CheckValidateOptimizeParams ( tTask ) )
		return;

	RTDLOG << "Optimize checked with " << tTask;

	bool bProgressive = g_bProgressiveMerge;
	switch ( tTask.m_eVerb ) // process all 'single' manual commands
	{
	case OptimizeTask_t::eMerge: case OptimizeTask_t::eDrop: case OptimizeTask_t::eCompress: case OptimizeTask_t::eSplit:
		{
			auto fnCoro = MakeCoroExecutor ( [this, &tTask]() { CoYield(); } ); // once yield - return tTask. Then end of the function - return false, finish optimize.
			CommonMerge ( [&fnCoro, &tTask] ( OptimizeTask_t& tRes ) -> bool {
				bool bRes = !fnCoro();
				tRes = tTask;
				return bRes;
			} );
		}
		return;
	case OptimizeTask_t::eAutoOptimize:
		bProgressive = true;
	default:
		break;
	}

	if ( m_tOptimizeRuns.GetValue() > 0 )
	{
		RTDLOG << "Escape optimize as " << m_tOptimizeRuns.GetValue() << " tasks is already running";
		sphLogDebug ("Escape optimize as %d tasks is already running", m_tOptimizeRuns.GetValue() );
		return;
	}

	if (!bProgressive)
	{
		RTDLOG << "Start ClassicOptimize()";
		ClassicOptimize(); // fixme! retire, remove
		return;
	}

	auto fnCoro = MakeCoroExecutor ( [this,&tTask]() {
	int iCutoff = tTask.m_iCutoff;
	if ( !iCutoff )
		iCutoff = AutoOptimizeCutoff();

	while ( true )
	{
		auto pChunks = m_tRtChunks.DiskChunks();
		if ( ( pChunks->GetLength() - GetNumOfOptimizingNow ( *pChunks ) ) <= iCutoff )
			break;

		// merge 'smallest' to 'smaller' and get 'merged' that names like 'A'+.tmp
		// however 'merged' got placed at 'B' position and 'merged' renamed to 'B' name

		auto chA = GetNextSmallestChunkByID ( *pChunks, -1 );
		if ( !chA.m_iSize ) // empty chunk - just remove
		{
			RTDLOG << "Optimize: drop chunk " << chA.m_iId;
			tTask.m_iFrom = chA.m_iId;
			tTask.m_eVerb = OptimizeTask_t::eDrop;
			CoYield();
			continue;
		}

		auto chB = GetNextSmallestChunkByID ( *pChunks, chA.m_iId );
		if ( chA.m_iId < 0 || chB.m_iId < 0 )
		{
//			sphWarning ( "Couldn't find smallest chunk" );
			break;
		}

		// we need to make sure that A is the oldest one
		// indexes go from oldest to newest so A must go before B (A is always older than B)
		// this is not required by bitmap killlists, but by some other stuff (like ALTER RECONFIGURE)
		if ( chA.m_iId > chB.m_iId )
			Swap ( chB, chA );

		RTDLOG << "Optimize: merge chunks " << chA.m_iId << " and " << chB.m_iId;
		tTask.m_iFrom = chA.m_iId;
		tTask.m_iTo = chB.m_iId;
		tTask.m_eVerb = OptimizeTask_t::eMerge;
		CoYield();
	}

	RTDLOG << "Optimize: start compressing pass for the rest of " << m_tRtChunks.GetDiskChunksCount() << " chunks.";
	// optimize (wipe deletes) in the rest of the chunks
	for ( int i = 0; i < m_tRtChunks.GetDiskChunksCount(); ++i )
	{
		tTask.m_iFrom = ChunkIDByChunkIdx ( i );
		tTask.m_eVerb = OptimizeTask_t::eCompress;
		RTDLOG << "Optimize: compress chunk " << tTask.m_iFrom << " (" << i << ")";
		CoYield();
	}
	});

	CommonMerge ( [fnCoro = std::move ( fnCoro ), &tTask] ( OptimizeTask_t& tTask2 ) -> bool {
		bool bRes = !fnCoro();
		tTask2 = tTask;
		return bRes;
	} );
}

void RtIndex_c::ClassicOptimize ()
{
	OptimizeTask_t tTask;
	auto fnCoro = MakeCoroExecutor ( [this,&tTask]() {
		while ( m_tRtChunks.GetDiskChunksCount() >=2 )
		{
			tTask.m_iFrom = ChunkIDByChunkIdx ( 0 );
			tTask.m_iTo = ChunkIDByChunkIdx ( 1 );
			tTask.m_eVerb = OptimizeTask_t::eMerge;
			CoYield();
		}
	});

	CommonMerge ( [fnCoro = std::move ( fnCoro ), &tTask] ( OptimizeTask_t& tTask2 ) -> bool {
		bool bRes = !fnCoro();
		tTask2 = tTask;
		return bRes;
	} );
}

void RtIndex_c::CheckStartAutoOptimize()
{
	int iCutoff = AutoOptimizeCutoffMultiplier();
	RTLOGV << "CheckStartAutoOptimize with cutoff=" << iCutoff;
	if ( !iCutoff )
		return;

	iCutoff *= AutoOptimizeCutoff();

	if ( m_tRtChunks.GetDiskChunksCount()<=iCutoff )
		return;

	OptimizeTask_t tTask;
	tTask.m_eVerb = OptimizeTask_t::eAutoOptimize;
	tTask.m_iCutoff = iCutoff;

	RTDLOG << "EnqueueForOptimizeWeak for " << m_sIndexName << ", auto-optimize with cutoff " << iCutoff;

	EnqueueForOptimizeWeak ( m_sIndexName, tTask );
}


//////////////////////////////////////////////////////////////////////////
// STATUS
//////////////////////////////////////////////////////////////////////////

void RtIndex_c::GetStatus ( CSphIndexStatus * pRes ) const
{
	assert ( pRes );
	if ( !pRes )
		return;

	RtGuard_t tGuard ( m_tRtChunks.RtData() );

	int64_t iUsedRam = SegmentsGetUsedRam ( tGuard.m_dRamSegs );
	pRes->m_iDead = SegmentsGetDeadRows ( tGuard.m_dRamSegs );

	pRes->m_iRamChunkSize = iUsedRam + tGuard.m_dRamSegs.GetLength()*int(sizeof(RtSegment_t));
	pRes->m_iRamUse = sizeof( RtIndex_c ) + pRes->m_iRamChunkSize;
	pRes->m_iRamRetired = m_iRamChunksAllocatedRAM.load(std::memory_order_relaxed) - iUsedRam;

	pRes->m_iMemLimit = m_iSoftRamLimit;

	CSphString sError;
	char sFile [ SPH_MAX_FILENAME_LEN ];
	const char * sFiles[] = { ".meta", ".ram" };
	for ( const char * sName : sFiles )
	{
		snprintf ( sFile, sizeof(sFile), "%s%s", m_sFilename.cstr(), sName );
		CSphAutofile fdRT ( sFile, SPH_O_READ, sError );
		int64_t iFileSize = fdRT.GetSize();
		if ( iFileSize>0 )
			pRes->m_iDiskUse += iFileSize; // that uses disk, but not occupies
	}
	CSphIndexStatus tDisk;
	for ( const auto& pChunk : tGuard.m_dDiskChunks )
	{
		pChunk->Cidx().GetStatus ( &tDisk );
		pRes->m_iRamUse += tDisk.m_iRamUse;
		pRes->m_iDiskUse += tDisk.m_iDiskUse;
		pRes->m_iMapped += tDisk.m_iMapped;
		pRes->m_iMappedResident += tDisk.m_iMappedResident;
		pRes->m_iMappedDocs += tDisk.m_iMappedDocs;
		pRes->m_iMappedResidentDocs += tDisk.m_iMappedResidentDocs;
		pRes->m_iMappedHits += tDisk.m_iMappedHits;
		pRes->m_iMappedResidentHits += tDisk.m_iMappedResidentHits;
		pRes->m_iDead += tDisk.m_iDead;
	}

	pRes->m_iNumRamChunks = tGuard.m_dRamSegs.GetLength();
	pRes->m_iNumChunks = tGuard.m_dDiskChunks.GetLength();

	pRes->m_iTID = m_iTID;
	pRes->m_iSavedTID = m_iSavedTID;
//	sphWarning ( "Chunks: %d, RAM: %d, DISK: %d", pRes->m_iNumChunks, (int) pRes->m_iRamUse, (int) pRes->m_iDiskUse );
}

//////////////////////////////////////////////////////////////////////////
// RECONFIGURE
//////////////////////////////////////////////////////////////////////////

bool CreateReconfigure ( const CSphString & sIndexName, bool bIsStarDict, const ISphFieldFilter * pFieldFilter,
	const CSphIndexSettings & tIndexSettings, uint64_t uTokHash, uint64_t uDictHash, int iMaxCodepointLength, int64_t iMemLimit,
	bool bSame, CSphReconfigureSettings & tSettings, CSphReconfigureSetup & tSetup, StrVec_t & dWarnings, CSphString & sError )
{
	CreateFilenameBuilder_fn fnCreateFilenameBuilder = GetIndexFilenameBuilder();
	CSphScopedPtr<FilenameBuilder_i> pFilenameBuilder ( fnCreateFilenameBuilder ? fnCreateFilenameBuilder ( sIndexName.cstr() ) : nullptr );

	// FIXME!!! check missed embedded files
	TokenizerRefPtr_c pTokenizer { ISphTokenizer::Create ( tSettings.m_tTokenizer, nullptr, pFilenameBuilder.Ptr(), dWarnings, sError ) };
	if ( !pTokenizer )
	{
		sError.SetSprintf ( "'%s' failed to create tokenizer, error '%s'", sIndexName.cstr(), sError.cstr() );
		return true;
	}

	// dict setup second
	DictRefPtr_c tDict { sphCreateDictionaryCRC ( tSettings.m_tDict, nullptr, pTokenizer, sIndexName.cstr(), false, tIndexSettings.m_iSkiplistBlockSize, pFilenameBuilder.Ptr(), sError ) };
	if ( !tDict )
	{
		sError.SetSprintf ( "'%s' failed to create dictionary, error '%s'", sIndexName.cstr(), sError.cstr() );
		return true;
	}

	// multiforms right after dict
	pTokenizer = ISphTokenizer::CreateMultiformFilter ( pTokenizer, tDict->GetMultiWordforms() );

	// bigram filter
	if ( tSettings.m_tIndex.m_eBigramIndex!=SPH_BIGRAM_NONE && tSettings.m_tIndex.m_eBigramIndex!=SPH_BIGRAM_ALL )
	{
		pTokenizer->SetBuffer ( (BYTE*)const_cast<char*> ( tSettings.m_tIndex.m_sBigramWords.cstr() ), tSettings.m_tIndex.m_sBigramWords.Length() );

		BYTE * pTok = nullptr;
		while ( ( pTok = pTokenizer->GetToken() )!=nullptr )
			tSettings.m_tIndex.m_dBigramWords.Add() = (const char*)pTok;

		tSettings.m_tIndex.m_dBigramWords.Sort();
	}

	bool bNeedExact = ( tDict->HasMorphology() || tDict->GetWordformsFileInfos().GetLength() );
	if ( tSettings.m_tIndex.m_bIndexExactWords && !bNeedExact )
		tSettings.m_tIndex.m_bIndexExactWords = false;

	if ( tDict->GetSettings().m_bWordDict && tDict->HasMorphology() && bIsStarDict && !tSettings.m_tIndex.m_bIndexExactWords )
		tSettings.m_tIndex.m_bIndexExactWords = true;

	// field filter
	FieldFilterRefPtr_c tFieldFilter;

	// re filter
	bool bReFilterSame = true;
	CSphFieldFilterSettings tFieldFilterSettings;
	if ( pFieldFilter )
		pFieldFilter->GetSettings ( tFieldFilterSettings );
	if ( tFieldFilterSettings.m_dRegexps.GetLength()!=tSettings.m_tFieldFilter.m_dRegexps.GetLength() )
	{
		bReFilterSame = false;
	} else
	{
		CSphVector<uint64_t> dFieldFilter;
		ARRAY_FOREACH ( i, tFieldFilterSettings.m_dRegexps )
			dFieldFilter.Add ( sphFNV64 ( tFieldFilterSettings.m_dRegexps[i].cstr() ) );
		dFieldFilter.Uniq();
		uint64_t uMyFF = sphFNV64 ( dFieldFilter.Begin(), sizeof(dFieldFilter[0]) * dFieldFilter.GetLength() );

		dFieldFilter.Resize ( 0 );
		ARRAY_FOREACH ( i, tSettings.m_tFieldFilter.m_dRegexps )
			dFieldFilter.Add ( sphFNV64 ( tSettings.m_tFieldFilter.m_dRegexps[i].cstr() ) );
		dFieldFilter.Uniq();
		uint64_t uNewFF = sphFNV64 ( dFieldFilter.Begin(), sizeof(dFieldFilter[0]) * dFieldFilter.GetLength() );

		bReFilterSame = ( uMyFF==uNewFF );
	}

	if ( !bReFilterSame && tSettings.m_tFieldFilter.m_dRegexps.GetLength () )
	{
		tFieldFilter = sphCreateRegexpFilter ( tSettings.m_tFieldFilter, sError );
		if ( !tFieldFilter )
		{
			sError.SetSprintf ( "'%s' failed to create field filter, error '%s'", sIndexName.cstr (), sError.cstr () );
			return true;
		}
	}

	// icu filter
	bool bIcuSame = ( tIndexSettings.m_ePreprocessor==tSettings.m_tIndex.m_ePreprocessor );
	if ( !bIcuSame )
	{
		if ( !sphSpawnFilterICU ( tFieldFilter, tSettings.m_tIndex, tSettings.m_tTokenizer, sIndexName.cstr (), sError ) )
		{
			sError.SetSprintf ( "'%s' failed to create field filter, error '%s'", sIndexName.cstr (), sError.cstr () );
			return true;
		}
	}

	// compare options
	if ( !bSame || uTokHash!=pTokenizer->GetSettingsFNV() || uDictHash!=tDict->GetSettingsFNV() ||
		iMaxCodepointLength!=pTokenizer->GetMaxCodepointLength() || sphGetSettingsFNV ( tIndexSettings )!=sphGetSettingsFNV ( tSettings.m_tIndex ) ||
		!bReFilterSame || !bIcuSame || tSettings.m_tMutableSettings.HasSettings() )
	{
		tSetup.m_pTokenizer = pTokenizer.Leak();
		tSetup.m_pDict = tDict.Leak();
		tSetup.m_tIndex = tSettings.m_tIndex;
		tSetup.m_pFieldFilter = tFieldFilter.Leak();
		tSetup.m_tMutableSettings = tSettings.m_tMutableSettings;
		return false;
	}

	return true;
}


bool RtIndex_c::IsSameSettings ( CSphReconfigureSettings & tSettings, CSphReconfigureSetup & tSetup, StrVec_t & dWarnings, CSphString & sError ) const
{
	bool bSame = true;
	if ( tSettings.m_bChangeSchema && m_uSchemaHash!=SchemaFNV ( tSettings.m_tSchema ) )
	{
		tSetup.m_tSchema = tSettings.m_tSchema;
		tSetup.m_bChangeSchema = true;
		bSame = false;
	}

	return CreateReconfigure ( m_sIndexName, IsStarDict ( m_bKeywordDict ), m_pFieldFilter, m_tSettings, m_pTokenizer->GetSettingsFNV(), m_pDict->GetSettingsFNV(), m_pTokenizer->GetMaxCodepointLength(),
		GetMemLimit(), bSame, tSettings, tSetup, dWarnings, sError );
}

bool RtIndex_c::Reconfigure ( CSphReconfigureSetup & tSetup )
{
	// strength single-fiber access (don't rely to upstream w-lock)
	ScopedScheduler_c tSerialFiber ( m_tRtChunks.SerialChunkAccess() );

	if ( !ForceDiskChunk() )
		return false;

	if ( tSetup.m_bChangeSchema )
		SetSchema ( tSetup.m_tSchema );

	if ( tSetup.m_tMutableSettings.HasSettings() )
	{
		m_tMutableSettings.Combine ( tSetup.m_tMutableSettings );
		SetMemLimit ( m_tMutableSettings.m_iMemLimit );
	}

	Setup ( tSetup.m_tIndex );
	SetTokenizer ( tSetup.m_pTokenizer );
	SetDictionary ( tSetup.m_pDict );
	SetFieldFilter ( tSetup.m_pFieldFilter );

	m_iMaxCodepointLength = m_pTokenizer->GetMaxCodepointLength();
	SetupQueryTokenizer();

	// FIXME!!! handle error
	m_pTokenizerIndexing = m_pTokenizer->Clone ( SPH_CLONE_INDEX );
	TokenizerRefPtr_c pIndexing { ISphTokenizer::CreateBigramFilter ( m_pTokenizerIndexing, m_tSettings.m_eBigramIndex, m_tSettings.m_sBigramWords, m_sLastError ) };
	if ( pIndexing )
		m_pTokenizerIndexing = pIndexing;

	AlterSave ( false );

	return true;
}


int	RtIndex_c::Kill ( DocID_t tDocID )
{
	assert ( 0 && "No external kills for RT");
	return 0;
}


int RtIndex_c::KillMulti ( const VecTraits_T<DocID_t> & dKlist )
{
	assert ( 0 && "No external kills for RT");
	return 0;
}

bool RtIndex_c::IsAlive ( DocID_t tDocID ) const
{
	assert ( 0 && "No external kills for RT");
	return false;
}

uint64_t sphGetSettingsFNV ( const CSphIndexSettings & tSettings )
{
	uint64_t uHash = 0;

	DWORD uFlags = 0;
	if ( tSettings.m_bHtmlStrip )
		uFlags |= 1<<1;
	if ( tSettings.m_bIndexExactWords )
		uFlags |= 1<<2;
	if ( tSettings.m_bIndexFieldLens )
		uFlags |= 1<<3;
	if ( tSettings.m_bIndexSP )
		uFlags |= 1<<4;
	uHash = sphFNV64 ( &uFlags, sizeof(uFlags), uHash );

	int iMinPrefixLen = tSettings.RawMinPrefixLen();

	uHash = sphFNV64 ( &tSettings.m_eHitFormat, sizeof(tSettings.m_eHitFormat), uHash );
	uHash = sphFNV64 ( tSettings.m_sHtmlIndexAttrs.cstr(), tSettings.m_sHtmlIndexAttrs.Length(), uHash );
	uHash = sphFNV64 ( tSettings.m_sHtmlRemoveElements.cstr(), tSettings.m_sHtmlRemoveElements.Length(), uHash );
	uHash = sphFNV64 ( tSettings.m_sZones.cstr(), tSettings.m_sZones.Length(), uHash );
	uHash = sphFNV64 ( &tSettings.m_eHitless, sizeof(tSettings.m_eHitless), uHash );
	uHash = sphFNV64 ( tSettings.m_sHitlessFiles.cstr(), tSettings.m_sHitlessFiles.Length(), uHash );
	uHash = sphFNV64 ( &tSettings.m_eBigramIndex, sizeof(tSettings.m_eBigramIndex), uHash );
	uHash = sphFNV64 ( tSettings.m_sBigramWords.cstr(), tSettings.m_sBigramWords.Length(), uHash );
	uHash = sphFNV64 ( &tSettings.m_uAotFilterMask, sizeof(tSettings.m_uAotFilterMask), uHash );
	uHash = sphFNV64 ( &tSettings.m_ePreprocessor, sizeof(tSettings.m_ePreprocessor), uHash );
	uHash = sphFNV64 ( tSettings.m_sIndexTokenFilter.cstr(), tSettings.m_sIndexTokenFilter.Length(), uHash );
	uHash = sphFNV64 ( &iMinPrefixLen, sizeof(iMinPrefixLen), uHash );
	uHash = sphFNV64 ( &tSettings.m_iMinInfixLen, sizeof(tSettings.m_iMinInfixLen), uHash );
	uHash = sphFNV64 ( &tSettings.m_iMaxSubstringLen, sizeof(tSettings.m_iMaxSubstringLen), uHash );
	uHash = sphFNV64 ( &tSettings.m_iBoundaryStep, sizeof(tSettings.m_iBoundaryStep), uHash );
	uHash = sphFNV64 ( &tSettings.m_iOvershortStep, sizeof(tSettings.m_iOvershortStep), uHash );
	uHash = sphFNV64 ( &tSettings.m_iStopwordStep, sizeof(tSettings.m_iStopwordStep), uHash );

	return uHash;
}

void RtIndex_c::GetIndexFiles ( CSphVector<CSphString> & dFiles, const FilenameBuilder_i * pParentBuilder ) const
{
	CSphString & sMeta = dFiles.Add();
	sMeta.SetSprintf ( "%s.meta", m_sPath.cstr() );

	CSphString & sRam = dFiles.Add();
	sRam.SetSprintf ( "%s.ram", m_sPath.cstr() );
	// RT index might be with flushed RAM into disk chunk
	CSphString sTmpError;
	if ( !sphIsReadable ( sRam.cstr(), &sTmpError ) )
		dFiles.Pop();

	CSphScopedPtr<const FilenameBuilder_i> pFilenameBuilder ( nullptr );
	if ( !pParentBuilder && GetIndexFilenameBuilder() )
	{
		pFilenameBuilder = GetIndexFilenameBuilder() ( m_sIndexName.cstr() );
		pParentBuilder = pFilenameBuilder.Ptr();
	}

	GetSettingsFiles ( m_pTokenizer, m_pDict, GetSettings(), pParentBuilder, dFiles );

	if ( m_tMutableSettings.NeedSave() ) // should be file already after post-setup
	{
		CSphString & sMutableSettings = dFiles.Add();
		sMutableSettings.SetSprintf ( "%s%s", m_sPath.cstr(), sphGetExt ( SPH_EXT_SETTINGS ).cstr() );
	}

	RtGuard_t tGuard ( m_tRtChunks.RtData() );
	for ( const auto& pIndex : tGuard.m_dDiskChunks )
		pIndex->Cidx().GetIndexFiles ( dFiles, pParentBuilder );

	dFiles.Uniq(); // might be duplicates of tok \ dict files from disk chunks
}


bool RtIndex_c::CopyExternalFiles ( int /*iPostfix*/, StrVec_t & dCopied )
{
	struct Rename_t
	{
		CSphString m_sSrc;
		CSphString m_sDst;
	};

	CSphVector<Rename_t> dExtFiles;
	if ( m_pTokenizer && !m_pTokenizer->GetSettings().m_sSynonymsFile.IsEmpty() )
	{
		const char * szRenameTo = "exceptions.txt";
		dExtFiles.Add ( { m_pTokenizer->GetSettings().m_sSynonymsFile, szRenameTo } );
		const_cast<CSphTokenizerSettings &>(m_pTokenizer->GetSettings()).m_sSynonymsFile = szRenameTo;
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
				sTmp.SetSprintf ( "stopwords_%d.txt", i );
				dExtFiles.Add ( { dStops[i], sTmp } );

				sNewStopwords << sTmp;
			}

			const_cast<CSphDictSettings &>(m_pDict->GetSettings()).m_sStopwords = sNewStopwords.cstr();
		}

		StrVec_t dNewWordforms;
		ARRAY_FOREACH ( i, m_pDict->GetSettings().m_dWordforms )
		{
			CSphString sTmp;
			sTmp.SetSprintf ( "wordforms_%d.txt", i );
			dExtFiles.Add( { m_pDict->GetSettings().m_dWordforms[i], sTmp } );
			dNewWordforms.Add(sTmp);
		}

		const_cast<CSphDictSettings &>(m_pDict->GetSettings()).m_dWordforms = dNewWordforms;
	}

	CSphString sPathOnly = GetPathOnly(m_sPath);
	for ( const auto & i : dExtFiles )
	{
		CSphString sDest;
		sDest.SetSprintf ( "%s%s", sPathOnly.cstr(), i.m_sDst.cstr() );
		if ( !CopyFile ( i.m_sSrc, sDest, m_sLastError ) )
			return false;

		dCopied.Add(sDest);
	}

	SaveMeta();

	auto pDiskChunks = m_tRtChunks.DiskChunks();
	auto& dDiskChunks = *pDiskChunks;
	ARRAY_FOREACH ( i, dDiskChunks )
		if ( !dDiskChunks[i]->CastIdx().CopyExternalFiles ( i, dCopied ) )
		{
			m_sLastError = dDiskChunks[i]->Cidx().GetLastError();
			return false;
		}

	return true;
}

// todo! Remove this, make using GetIndexFiles instead (this is duplication of existing functionality)
void RtIndex_c::CollectFiles ( StrVec_t & dFiles, StrVec_t & dExt ) const
{
	if ( m_pTokenizer && !m_pTokenizer->GetSettings().m_sSynonymsFile.IsEmpty() )
		dExt.Add ( m_pTokenizer->GetSettings ().m_sSynonymsFile );

	if ( m_pDict )
	{
		const CSphString & sStopwords = m_pDict->GetSettings().m_sStopwords;
		if ( !sStopwords.IsEmpty() )
			sphSplit ( dExt, sStopwords.cstr(), sStopwords.Length(), " \t," );

		m_pDict->GetSettings ().m_dWordforms.Apply ( [&dExt] ( const CSphString & a ) { dExt.Add ( a ); } );
	}

	{
		RtGuard_t tGuard ( m_tRtChunks.RtData() );
		tGuard.m_dDiskChunks.for_each ( [&] ( ConstDiskChunkRefPtr_t& p ) { p->Cidx().CollectFiles ( dFiles, dExt ); } );
	}

	CSphString sPath;
	sPath.SetSprintf ( "%s.meta", m_sPath.cstr () );
	if ( sphIsReadable ( sPath ) )
		dFiles.Add ( sPath );
	sPath.SetSprintf ( "%s.ram", m_sPath.cstr () );
	if ( sphIsReadable ( sPath ) )
		dFiles.Add ( sPath );

	if ( !m_tMutableSettings.NeedSave() )
		return;

	// should be file already after post-setup
	sPath.SetSprintf ( "%s%s", m_sPath.cstr(), sphGetExt ( SPH_EXT_SETTINGS ).cstr() );
	if ( sphIsReadable ( sPath ) )
		dFiles.Add ( sPath );
}

void RtIndex_c::ProhibitSave()
{
	StopOptimize();
	m_eSaving.store ( WriteState_e::DISCARD, std::memory_order_relaxed );
}

void RtIndex_c::EnableSave()
{
	m_eSaving.store ( WriteState_e::ENABLED, std::memory_order_relaxed );
	m_bOptimizeStop = false;
}

// fixme! Review, if it still necessary, as SST locks everything itself.
void RtIndex_c::LockFileState ( CSphVector<CSphString>& dFiles )
{
	StopOptimize();
	ForceRamFlush ( "forced" );
	CSphString sError;
	SaveAttributes ( sError ); // fixme! report error, better discard whole locking
	m_eSaving.store ( WriteState_e::DISABLED, std::memory_order_release );

	GetIndexFiles ( dFiles, nullptr );
}


void RtIndex_c::CreateReader ( int64_t iSessionId ) const
{
	// rt docstore doesn't need buffered readers
	// but disk chunks need them
	RtGuard_t tGuard ( m_tRtChunks.RtData() );

	for ( const auto & i : tGuard.m_dDiskChunks )
	{
		assert(i);
		i->Cidx().CreateReader ( iSessionId );
	}
}


bool RtIndex_c::GetDoc ( DocstoreDoc_t & tDoc, DocID_t tDocID, const VecTraits_T<int> * pFieldIds, int64_t iSessionId, bool bPack ) const
{
	RtGuard_t tGuard ( m_tRtChunks.RtData() );

	for ( const auto & i : tGuard.m_dRamSegs )
	{
		assert ( i && i->m_pDocstore.Ptr() );
		RowID_t tRowID = i->GetRowidByDocid(tDocID);
		if ( tRowID==INVALID_ROWID || i->m_tDeadRowMap.IsSet(tRowID) )
			continue;

		tDoc = i->m_pDocstore->GetDoc ( tRowID, pFieldIds, iSessionId, bPack );
		return true;
	}

	for ( const auto & i : tGuard.m_dDiskChunks )
	{
		assert(i);
		if ( i->Cidx().GetDoc ( tDoc, tDocID, pFieldIds, iSessionId, bPack ) )
			return true;
	}

	return false;
}


int RtIndex_c::GetFieldId ( const CSphString & sName, DocstoreDataType_e eType ) const
{
	return m_pDocstoreFields.Ptr() ? m_pDocstoreFields->GetFieldId ( sName, eType ) : -1;
}

Bson_t RtIndex_c::ExplainQuery ( const CSphString & sQuery ) const
{
	ExplainQueryArgs_t tArgs ( sQuery );
	tArgs.m_pSchema = &GetMatchSchema();

	TokenizerRefPtr_c pQueryTokenizer { m_pTokenizer->Clone ( SPH_CLONE_QUERY ) };
	sphSetupQueryTokenizer ( pQueryTokenizer, IsStarDict ( m_bKeywordDict ), m_tSettings.m_bIndexExactWords, false );

	tArgs.m_pDict = GetStatelessDict ( m_pDict );
	SetupStarDict ( tArgs.m_pDict, pQueryTokenizer );
	SetupExactDict ( tArgs.m_pDict, pQueryTokenizer );
	if ( m_pFieldFilter )
		tArgs.m_pFieldFilter = m_pFieldFilter->Clone();
	tArgs.m_pSettings = &m_tSettings;
	tArgs.m_pWordlist = this;
	tArgs.m_pQueryTokenizer = pQueryTokenizer;
	tArgs.m_iExpandKeywords = m_tMutableSettings.m_iExpandKeywords;
	tArgs.m_iExpansionLimit = m_iExpansionLimit;
	tArgs.m_bExpandPrefix = ( m_pDict->GetSettings().m_bWordDict && IsStarDict ( m_bKeywordDict ) );

	RtGuard_t tGuard ( m_tRtChunks.RtData() );
	tArgs.m_pIndexData = tGuard.m_tSegmentsAndChunks.m_pSegs;

	return Explain ( tArgs );
}

bool RtIndex_c::NeedStoreWordID () const
{
	return ( m_tSettings.m_eHitless==SPH_HITLESS_SOME && m_dHitlessWords.GetLength() );
}

//////////////////////////////////////////////////////////////////////////

RtIndex_i * sphGetCurrentIndexRT()
{
	RtAccum_t * pAcc = g_pTlsAccum;
	if ( pAcc )
		return pAcc->GetIndex();
	return nullptr;
}

RtIndex_i * sphCreateIndexRT ( const CSphSchema & tSchema, const char * sIndexName, int64_t iRamSize, const char * sPath, bool bKeywordDict )
{
	MEMORY ( MEM_INDEX_RT );
	return new RtIndex_c ( tSchema, sIndexName, iRamSize, sPath, bKeywordDict );
}

RtIndex_i* MakeOneshotRt ( const VecTraits_T<CSphIndex*>& dChunks, const CSphString& sName )
{
	assert ( dChunks.GetLength() > 1 );
	auto pIndex = dChunks.First();
	const CSphSchema & tSchema = pIndex->GetMatchSchema();
	bool bKeywordDict = pIndex->GetDictionary()->GetSettings().m_bWordDict;
	auto pRtIndex = new RtIndex_c ( tSchema, sName.cstr(), 0, "", bKeywordDict );

	pRtIndex->InitOneshotIndex ( dChunks );
	return pRtIndex;
}

void UpgradeOneshotToFT ( CSphIndex* pIndex )
{
	auto* pRtIndex = (RtIndex_c*)pIndex;
	pRtIndex->InitDictionaryInOneshot();
}

void sphRTInit ( const CSphConfigSection & hSearchd, bool bTestMode, const CSphConfigSection * pCommon )
{
	Binlog::Init ( hSearchd, bTestMode );
	if ( pCommon )
		g_bProgressiveMerge = pCommon->GetBool ( "progressive_merge", true );
}


static bool g_bTestMode = false;

void sphRTSetTestMode ()
{
	g_bTestMode = true;
}


static void SetColumnarFlag ( CSphColumnInfo & tCol, const CSphIndexSettings & tSettings )
{
	for ( const auto & i : tSettings.m_dColumnarAttrs )
		if ( i==tCol.m_sName )
			tCol.m_uAttrFlags |= CSphColumnInfo::ATTR_COLUMNAR;
}


bool sphRTSchemaConfigure ( const CSphConfigSection & hIndex, CSphSchema & tSchema, const CSphIndexSettings & tSettings, CSphString & sError, bool bSkipValidation, bool bPQ )
{
	// fields
	SmallStringHash_T<BYTE> hFields;
	for ( CSphVariant * v=hIndex("rt_field"); v; v=v->m_pNext )
	{
		CSphString sFieldName = v->cstr();
		sFieldName.ToLower();
		tSchema.AddField ( sFieldName.cstr() );
		hFields.Add ( 1, sFieldName );
	}

	if ( !tSchema.GetFieldsCount() && !bSkipValidation )
	{
		sError.SetSprintf ( "no fields configured (use rt_field directive)" );
		return false;
	}

	if ( tSchema.GetFieldsCount()>SPH_MAX_FIELDS )
	{
		sError.SetSprintf ( "too many fields (fields=%d, max=%d)", tSchema.GetFieldsCount(), SPH_MAX_FIELDS );
		return false;
	}

	// add id column
	CSphColumnInfo tDocIdCol ( sphGetDocidName() );
	tDocIdCol.m_eAttrType = SPH_ATTR_BIGINT;
	if ( !bPQ )
		SetColumnarFlag ( tDocIdCol, tSettings );
	tSchema.AddAttr ( tDocIdCol, false );

	// attrs
	const int iNumTypes = 9;
	const char * sTypes[iNumTypes] = { "rt_attr_uint", "rt_attr_bigint", "rt_attr_timestamp", "rt_attr_bool", "rt_attr_float", "rt_attr_string", "rt_attr_json", "rt_attr_multi", "rt_attr_multi_64" };
	const ESphAttr iTypes[iNumTypes] = { SPH_ATTR_INTEGER, SPH_ATTR_BIGINT, SPH_ATTR_TIMESTAMP, SPH_ATTR_BOOL, SPH_ATTR_FLOAT, SPH_ATTR_STRING, SPH_ATTR_JSON, SPH_ATTR_UINT32SET, SPH_ATTR_INT64SET };

	for ( int iType=0; iType<iNumTypes; ++iType )
	{
		for ( CSphVariant * v = hIndex ( sTypes[iType] ); v; v = v->m_pNext )
		{
			StrVec_t dNameParts;
			sphSplit ( dNameParts, v->cstr(), ":");
			CSphColumnInfo tCol ( dNameParts[0].cstr(), iTypes[iType]);
			tCol.m_sName.ToLower();

			// bitcount
			tCol.m_tLocator = CSphAttrLocator();
			if ( dNameParts.GetLength ()>1 )
			{
				if ( tCol.m_eAttrType==SPH_ATTR_INTEGER )
				{
					auto iBits = strtol ( dNameParts[1].cstr(), nullptr, 10 );
					if ( iBits>0 && iBits<=ROWITEM_BITS )
						tCol.m_tLocator.m_iBitCount = (int)iBits;
					else
						sError.SetSprintf ( "attribute '%s': invalid bitcount=%d (bitcount ignored)", tCol.m_sName.cstr(), (int)iBits );

				} else
					sError.SetSprintf ( "attribute '%s': bitcount is only supported for integer types (bitcount ignored)", tCol.m_sName.cstr() );
			}

			if ( !SchemaConfigureCheckAttribute ( tSchema, tCol, sError ) )
				return false;

			if ( !bPQ )
				SetColumnarFlag ( tCol, tSettings );

			tSchema.AddAttr ( tCol, false );

			if ( tCol.m_eAttrType!=SPH_ATTR_STRING && hFields.Exists ( tCol.m_sName ) && !bSkipValidation )
			{
				sError.SetSprintf ( "can not add attribute that shadows '%s' field", tCol.m_sName.cstr () );
				return false;
			}
		}
	}

	// add blob attr locator
	if ( tSchema.HasBlobAttrs() )
	{
		CSphColumnInfo tBlobLocCol ( sphGetBlobLocatorName() );
		tBlobLocCol.m_eAttrType = SPH_ATTR_BIGINT;

		// should be right after docid
		tSchema.InsertAttr ( 1, tBlobLocCol, false );

		// rebuild locators in the schema
		const char * szTmpColName = "$_tmp";
		CSphColumnInfo tTmpCol ( szTmpColName, SPH_ATTR_BIGINT );
		tSchema.AddAttr ( tTmpCol, false );
		tSchema.RemoveAttr ( szTmpColName, false );
	}

	if ( !tSchema.GetAttrsCount() && !g_bTestMode && !bSkipValidation )
	{
		sError.SetSprintf ( "no attribute configured (use rt_attr directive)" );
		return false;
	}

	return true;
}

void RtAccum_t::LoadRtTrx ( const BYTE * pData, int iLen )
{
	MemoryReader_c tReader ( pData, iLen );
	m_bReplace = !!tReader.GetByte();
	m_uAccumDocs = tReader.GetDword();

	// insert and replace
	m_dAccum.Resize ( tReader.GetDword() );
	for ( CSphWordHit & tHit : m_dAccum )
	{
		// such manual serialization is necessary because CSphWordHit is internally aligned by 8,
		// and it's size is 3*8, however actually we have 4+8+4 bytes in members.
		// Sending raw unitialized bytes is not ok, since it may influent crc checking.
		tReader.GetBytes ( &tHit.m_tRowID, (int) sizeof ( tHit.m_tRowID ) );
		tReader.GetBytes ( &tHit.m_uWordID, (int) sizeof ( tHit.m_uWordID ) );
		tReader.GetBytes ( &tHit.m_uWordPos, (int) sizeof ( tHit.m_uWordPos ) );
	}
	m_dAccumRows.Resize ( tReader.GetDword() );
	tReader.GetBytes ( m_dAccumRows.Begin(), (int) m_dAccumRows.GetLengthBytes() );

	m_dBlobs.Resize ( tReader.GetDword() );
	tReader.GetBytes ( m_dBlobs.Begin(), (int) m_dBlobs.GetLengthBytes() );

	m_dPerDocHitsCount.Resize ( tReader.GetDword() );
	tReader.GetBytes ( m_dPerDocHitsCount.Begin(), (int) m_dPerDocHitsCount.GetLengthBytes() );

	m_dPackedKeywords.Reset ( tReader.GetDword() );
	tReader.GetBytes ( m_dPackedKeywords.Begin(), (int) m_dPackedKeywords.GetLengthBytes() );

	if ( tReader.GetByte() )
	{
		if ( !m_pDocstore.Ptr() )
			m_pDocstore = CreateDocstoreRT();
		assert ( m_pDocstore.Ptr() );
		m_pDocstore->Load(tReader);
	}

	if ( tReader.GetByte() )
		m_pColumnarBuilder = CreateColumnarBuilderRT(tReader);

	// delete
	m_dAccumKlist.Resize ( tReader.GetDword() );
	tReader.GetBytes ( m_dAccumKlist.Begin(), (int) m_dAccumKlist.GetLengthBytes() );
}

void RtAccum_t::SaveRtTrx ( MemoryWriter_c & tWriter ) const
{
	tWriter.PutByte ( m_bReplace ); // this need only for data sort on commit
	tWriter.PutDword ( m_uAccumDocs );

	// insert and replace
	tWriter.PutDword ( m_dAccum.GetLength() );
	for ( const CSphWordHit& tHit : m_dAccum )
	{
		tWriter.PutBytes ( &tHit.m_tRowID, sizeof ( tHit.m_tRowID ) );
		tWriter.PutBytes ( &tHit.m_uWordID, sizeof ( tHit.m_uWordID ) );
		tWriter.PutBytes ( &tHit.m_uWordPos, sizeof ( tHit.m_uWordPos ) );
	}
	tWriter.PutDword ( m_dAccumRows.GetLength() );
	tWriter.PutBytes ( m_dAccumRows.Begin(), (int) m_dAccumRows.GetLengthBytes() );

	tWriter.PutDword ( m_dBlobs.GetLength() );
	tWriter.PutBytes ( m_dBlobs.Begin(), (int) m_dBlobs.GetLengthBytes() );

	tWriter.PutDword ( m_dPerDocHitsCount.GetLength() );
	tWriter.PutBytes ( m_dPerDocHitsCount.Begin(), (int) m_dPerDocHitsCount.GetLengthBytes() );
	// packed keywords default length is 1 no need to pass that
	int iLen = ( m_bKeywordDict && m_pDictRt->GetPackedLen()>1 ? (int) m_pDictRt->GetPackedLen() : 0 );
	tWriter.PutDword ( iLen );
	if ( iLen )
		tWriter.PutBytes ( m_pDictRt->GetPackedKeywords(), iLen );
	tWriter.PutByte ( m_pDocstore.Ptr()!=nullptr );
	if ( m_pDocstore.Ptr() )
		m_pDocstore->Save(tWriter);

	tWriter.PutByte ( m_pColumnarBuilder.Ptr()!=nullptr );
	if ( m_pColumnarBuilder.Ptr() )
		m_pColumnarBuilder->Save(tWriter);

	// delete
	tWriter.PutDword ( m_dAccumKlist.GetLength() );
	tWriter.PutBytes ( m_dAccumKlist.Begin(), (int) m_dAccumKlist.GetLengthBytes() );
}

void RtIndex_c::SetSchema ( const CSphSchema & tSchema )
{
	m_tSchema = tSchema;
	m_iStride = m_tSchema.GetRowSize();
	m_uSchemaHash = SchemaFNV ( m_tSchema );

	if ( m_tSchema.HasStoredFields() )
	{
		m_pDocstoreFields = CreateDocstoreFields();
		SetupDocstoreFields ( *m_pDocstoreFields.Ptr(), m_tSchema );
	}

	ARRAY_FOREACH ( i, m_dFieldLens )
	{
		m_dFieldLens[i] = 0;
		m_dFieldLensRam[i] = 0;
		m_dFieldLensDisk[i] = 0;
	}
}

uint64_t SchemaFNV ( const ISphSchema & tSchema )
{
	uint64_t uHash = SPH_FNV64_SEED;

	// attrs
	int iAttrsCount = tSchema.GetAttrsCount();
	for ( int i=0; i<iAttrsCount; i++ )
	{
		const CSphColumnInfo & tAttr = tSchema.GetAttr ( i );
		uHash = sphFNV64cont ( tAttr.m_sName.cstr(), uHash );
		uHash = sphFNV64 ( &tAttr.m_eAttrType, sizeof( tAttr.m_eAttrType ), uHash );
		uHash = tAttr.m_tLocator.FNV ( uHash );
	}

	// fulltext fields
	int iFieldsCount = tSchema.GetFieldsCount();
	for ( int i=0; i<iFieldsCount; i++ )
	{
		const CSphColumnInfo & tField = tSchema.GetField ( i );
		uHash = sphFNV64cont ( tField.m_sName.cstr(), uHash );
		uHash = sphFNV64 ( &tField.m_uFieldFlags, sizeof( tField.m_uFieldFlags ), uHash );
	}

	return uHash;
}

void RtIndex_c::SetMemLimit ( int64_t iMemLimit )
{
	m_iSoftRamLimit = iMemLimit;
	m_iDoubleBufferLimit = ( m_iSoftRamLimit * SPH_RT_DOUBLE_BUFFER_PERCENT ) / 100;
}
