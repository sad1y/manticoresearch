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
#include "sphinxsearch.h"
#include "searchnode.h"
#include "sphinxint.h"
#include "sphinxplugin.h"
#include "sphinxqcache.h"
#include "attribute.h"
#include "conversion.h"

#include <math.h>


bool operator < ( const SkiplistEntry_t & a, RowID_t b )	{ return a.m_tBaseRowIDPlus1<b; }
bool operator == ( const SkiplistEntry_t & a, RowID_t b )	{ return a.m_tBaseRowIDPlus1==b; }
bool operator < ( RowID_t a, const SkiplistEntry_t & b )	{ return a<b.m_tBaseRowIDPlus1; }

//////////////////////////////////////////////////////////////////////////

#define SPH_TREE_DUMP			0
#define SPH_BM25_SCALE			1000


RowID_t	ISphQword::AdvanceTo ( RowID_t tRowID )
{
	// this is sub-optimal, faster versions of AdvanceTo should be implemented in descendants
	HintRowID ( tRowID );

	RowID_t tFoundRowID = INVALID_ROWID;
	do
	{
		tFoundRowID = GetNextDoc().m_tRowID;
	}
	while ( tFoundRowID < tRowID );

	return tFoundRowID;
}


void ISphQword::CollectHitMask()
{
	if ( m_bAllFieldsKnown )
		return;
	SeekHitlist ( m_iHitlistPos );
	for ( Hitpos_t uHit = GetNextHit(); uHit!=EMPTY_HIT; uHit = GetNextHit() )
		m_dQwordFields.Set ( HITMAN::GetField ( uHit ) );
	m_bAllFieldsKnown = true;
}


void ISphQword::Reset ()
{
	m_iDocs = 0;
	m_iHits = 0;
	m_dQwordFields.UnsetAll();
	m_bAllFieldsKnown = false;
	m_uMatchHits = 0;
	m_iHitlistPos = 0;
}


int	ISphQword::GetAtomPos() const
{
	return m_iAtomPos;
}


/// per-document zone information (span start/end positions)
struct ZoneInfo_t
{
	RowID_t			m_tRowID;
	ZoneHits_t *	m_pHits;
};

// FindSpan vector operators
static bool operator < ( const ZoneInfo_t & tZone, RowID_t tRowID )
{
	return tZone.m_tRowID<tRowID;
}

static bool operator == ( const ZoneInfo_t & tZone, RowID_t tRowID )
{
	return tZone.m_tRowID==tRowID;
}

static bool operator < ( RowID_t tRowID, const ZoneInfo_t & tZone )
{
	return tRowID<tZone.m_tRowID;
}

//////////////////////////////////////////////////////////////////////////
// RANKER
//////////////////////////////////////////////////////////////////////////
typedef CSphFixedVector < CSphVector < ZoneInfo_t > > ZoneVVector_t;

/// ranker interface
/// ranker folds incoming hitstream into simple match chunks, and computes relevance rank
class ExtRanker_c : public ISphRanker, public ISphZoneCheck
{
public:
								ExtRanker_c ( const XQQuery_t & tXQ, const ISphQwordSetup & tSetup, bool bSkipQCache, bool bCollectHits, bool bUseBM25 );
								~ExtRanker_c() override;
	void						Reset ( const ISphQwordSetup & tSetup ) override;

	CSphMatch *					GetMatchesBuffer () override { return m_dMatches; }
	int							GetQwords ( ExtQwordsHash_t & hQwords )					{ return m_pRoot ? m_pRoot->GetQwords ( hQwords ) : -1; }
	virtual void				SetQwordsIDF ( const ExtQwordsHash_t & hQwords );
	virtual void				SetTermDupes ( const ExtQwordsHash_t & , int ) {}
	virtual bool				InitState ( const CSphQueryContext &, CSphString & )	{ return true; }

	void						FinalizeCache ( const ISphSchema & tSorterSchema ) override;

public:
	// FIXME? hide and friend?
	SphZoneHit_e				IsInZone ( int iZone, const ExtHit_t * pHit, int * pLastSpan ) override;
	virtual const CSphIndex *	GetIndex() { return m_pIndex; }
	const CSphQueryContext * GetCtx() const { return m_pCtx; }

public:
	CSphMatch					m_dMatches[MAX_BLOCK_DOCS];	///< exposed for caller
	DWORD						m_uPayloadMask = 0;					///< exposed for ranker state functors
	int							m_iQwords = 0;						///< exposed for ranker state functors
	int							m_iMaxQpos = 0;						///< max in-query pos among all keywords, including dupes; for ranker state functors

protected:
	ExtNode_i *					m_pRoot = nullptr;
	const ExtDoc_t *			m_pDoclist = nullptr;
	const ExtHit_t *			m_pHitlist = nullptr;
	ExtDoc_t					m_dMyDocs[MAX_BLOCK_DOCS];		///< my local documents pool; for filtering
	CSphMatch					m_dMyMatches[MAX_BLOCK_DOCS];	///< my local matches pool; for filtering
	CSphMatch					m_tTestMatch;
	const CSphIndex *			m_pIndex = nullptr;					///< this is he who'll do my filtering!
	CSphQueryContext *			m_pCtx = nullptr;

	int64_t *					m_pNanoBudget = nullptr;
	QcacheEntry_c *				m_pQcacheEntry = nullptr;			///< data to cache if we decide that the current query is worth caching

	CSphVector<CSphString>		m_dZones;
	CSphVector<ExtNode_i*>		m_dZoneStartTerm;
	CSphVector<ExtNode_i*>		m_dZoneEndTerm;
	CSphVector<const ExtDoc_t*>	m_dZoneStart;
	CSphVector<const ExtDoc_t*>	m_dZoneEnd;
	CSphVector<RowID_t>			m_dZoneMax;				///< last rowid we (tried) to cache
	CSphVector<RowID_t>			m_dZoneMin;				///< first rowid we (tried) to cache
	ZoneVVector_t				m_dZoneInfo {0};
	bool						m_bZSlist;

	void						CleanupZones ( RowID_t tMaxRowID );
	void						UpdateQcache ( int iMatches );
};


template <bool USE_BM25>
class ExtRanker_T : public ExtRanker_c
{
public:
								ExtRanker_T ( const XQQuery_t & tXQ, const ISphQwordSetup & tSetup, bool bSkipQCache, bool bCollectHits );

	virtual const ExtDoc_t *	GetFilteredDocs ();
};



STATIC_ASSERT ( ( 8*8*sizeof(DWORD) )>=SPH_MAX_FIELDS, PAYLOAD_MASK_OVERFLOW );

static const bool WITH_BM25 = true;

template < bool USE_BM25 = false >
class ExtRanker_WeightSum_c : public ExtRanker_T<USE_BM25>
{
protected:
	int				m_iWeights = 0;
	const int *		m_pWeights = nullptr;

public:
	ExtRanker_WeightSum_c ( const XQQuery_t & tXQ, const ISphQwordSetup & tSetup, bool bSkipQCache )
		: ExtRanker_T<USE_BM25> ( tXQ, tSetup, bSkipQCache, false )
	{}

	int		GetMatches () override;

	bool	InitState ( const CSphQueryContext & tCtx, CSphString & ) override
	{
		m_iWeights = tCtx.m_iWeights;
		m_pWeights = tCtx.m_dWeights;
		return true;
	}
};


class ExtRanker_None_c : public ExtRanker_T<false>
{
public:
	ExtRanker_None_c ( const XQQuery_t & tXQ, const ISphQwordSetup & tSetup, bool bSkipQCache )
		: ExtRanker_T<false> ( tXQ, tSetup, bSkipQCache, false )
	{}

	int		GetMatches () override;
};


template < typename STATE, bool USE_BM25 >
class ExtRanker_State_T : public ExtRanker_T<USE_BM25>
{
protected:
	STATE				m_tState;
	const ExtHit_t *	m_pHitBase;
	CSphVector<int>		m_dZonespans; // zonespanlists for my matches

public:
					ExtRanker_State_T ( const XQQuery_t & tXQ, const ISphQwordSetup & tSetup, bool bSkipQCache, bool bCollectHits = true );
	int				GetMatches () override;

	bool InitState ( const CSphQueryContext & tCtx, CSphString & sError ) override
	{
		return m_tState.Init ( tCtx.m_iWeights, &tCtx.m_dWeights[0], this, sError, tCtx.m_uPackedFactorFlags );
	}

private:
	bool ExtraDataImpl ( ExtraData_e eType, void ** ppResult ) override
	{
		switch ( eType )
		{
			case EXTRA_GET_DATA_ZONESPANS:
				assert ( ppResult );
				*ppResult = &m_dZonespans;
				return true;
			default:
				return m_tState.ExtraData ( eType, ppResult );
		}
	}
};

//////////////////////////////////////////////////////////////////////////
namespace { // static

// dump spec for keyword nodes
// fixme! consider make specific implementation if dot/plain need to be different
void RenderAccessSpecs ( StringBuilder_c & tRes, const bson::Bson_c& tBson, bool bWithZones )
{
	using namespace bson;
	{
		ScopedComma_c dFieldsComma ( tRes, ", ", "fields=(", ")" );
		Bson_c ( tBson.ChildByName ( SZ_FIELDS ) ).ForEach ( [&tRes] ( const NodeHandle_t & tNode ) {
			tRes << String ( tNode );
		} );
	}
	int iPos = (int)Int ( tBson.ChildByName ( SZ_MAX_FIELD_POS ) );
	if ( iPos )
		tRes.Sprintf ( "max_field_pos=%d", iPos );

	if ( !bWithZones )
		return;

	auto tZones = tBson.GetFirstOf ( { SZ_ZONES, SZ_ZONESPANS } );
	ScopedComma_c dZoneDelim ( tRes, ", ", ( tZones.first==1 ) ? "zonespans=(" : "zones=(", ")" );
	Bson_c ( tZones.second ).ForEach ( [&tRes] ( const NodeHandle_t & tNode ) {
		tRes << String ( tNode );
	} );
}

bool RenderKeywordNode ( StringBuilder_c & tRes, const bson::Bson_c& tBson )
{
	using namespace bson;
	auto tWord = tBson.ChildByName ( SZ_WORD );
	if ( IsNullNode ( tWord ) )
		return false;

	ScopedComma_c ExplainComma ( tRes, ", ", "KEYWORD(", ")" );
	tRes << String ( tWord );
	tRes.Sprintf ( "querypos=%d", Int ( tBson.ChildByName ( SZ_QUERYPOS ) ) );
	if ( Bool ( tBson.ChildByName ( SZ_EXCLUDED ) ) )
		tRes += "excluded";
	if ( Bool ( tBson.ChildByName ( SZ_EXPANDED ) ) )
		tRes += "expanded";
	if ( Bool ( tBson.ChildByName ( SZ_FIELD_START ) ) )
		tRes += "field_start";
	if ( Bool ( tBson.ChildByName ( SZ_FIELD_END ) ) )
		tRes += "field_end";
	if ( Bool ( tBson.ChildByName ( SZ_MORPHED ) ) )
		tRes += "morphed";
	auto tBoost = tBson.ChildByName ( SZ_BOOST );
	if ( !IsNullNode ( tBoost ) )
	{
		auto fBoost = Double ( tBoost );
		if ( fBoost!=1.0f ) // really comparing floats?
			tRes.Appendf ( "boost=%f", fBoost );
	}
	return true;
}

void RenderPlainBsonPlan ( bson::NodeHandle_t dBson, StringBuilder_c & tRes, bool bWithZones,
		int iIndent, const char * szIndent, const char * szLinebreak )
{
	using namespace bson;
	if ( dBson==nullnode )
		return;

	Bson_c tBson ( dBson );
	if ( RenderKeywordNode ( tRes, tBson ) )
		return;

	ScopedComma_c sEmpty ( tRes, nullptr );

	if ( iIndent )
		tRes += szLinebreak;

	for ( int i = 0; i<iIndent; ++i )
		tRes += szIndent;

	tRes << String ( tBson.ChildByName ( SZ_TYPE ) );

	// enclose the rest in brackets, comma-separated
	ScopedComma_c ExplainComma ( tRes, ", ", "(", ")" );
	Bson_c ( tBson.ChildByName ( SZ_OPTIONS ) ).ForEach ( [&tRes] ( CSphString&& sName, const NodeHandle_t & tNode ) {
		tRes.Sprintf ( "%s=%d", sName.cstr (), (int) Int ( tNode ) );
	} );
	if ( Bool ( tBson.ChildByName ( SZ_VIRTUALLY_PLAIN ) ) )
		tRes += "virtually-plain";

	// dump spec for keyword nodes
	RenderAccessSpecs ( tRes, dBson, bWithZones );

	Bson_c ( tBson.ChildByName ( SZ_CHILDREN ) ).ForEach ( [&] ( const NodeHandle_t & tNode ) {
		RenderPlainBsonPlan ( tNode, tRes, bWithZones, iIndent+1, szIndent, szLinebreak );
	} );
}


bool RenderKeywordNodeDot ( StringBuilder_c & tRes, const bson::Bson_c& tBson )
{
	using namespace bson;
	auto tWord = tBson.ChildByName ( SZ_WORD );
	if ( IsNullNode ( tWord ) )
		return false;

	//[shape=record label="wayyy | {expanded | pos=1}"]
	ScopedComma_c ExplainComma ( tRes, " | ", "[shape=record label=\"", "\"]\n" );
	tRes << String ( tWord );
	ScopedComma_c ParamComma ( tRes, " | ", "{ ", " }" );
	tRes.Sprintf ( "querypos=%d", Int ( tBson.ChildByName ( SZ_QUERYPOS ) ) );
	if ( Bool ( tBson.ChildByName ( SZ_EXCLUDED ) ) )
		tRes += "excluded";
	if ( Bool ( tBson.ChildByName ( SZ_EXPANDED ) ) )
		tRes += "expanded";
	if ( Bool ( tBson.ChildByName ( SZ_FIELD_START ) ) )
		tRes += "field_start";
	if ( Bool ( tBson.ChildByName ( SZ_FIELD_END ) ) )
		tRes += "field_end";
	if ( Bool ( tBson.ChildByName ( SZ_MORPHED ) ) )
		tRes += "morphed";
	auto tBoost = tBson.ChildByName ( SZ_BOOST );
	if ( !IsNullNode ( tBoost ) )
	{
		auto fBoost = Double ( tBoost );
		if ( fBoost!=1.0f ) // really comparing floats?
			tRes.Appendf ( "boost=%f", fBoost );
	}
	return true;
}

void RenderDotBsonNodePlan ( bson::NodeHandle_t dBson, StringBuilder_c & tRes, int& iId )
{
	using namespace bson;
	if ( dBson==nullnode )
		return;

	tRes << "\n" << iId << " "; // node num

	Bson_c tBson ( dBson );
	if ( RenderKeywordNodeDot ( tRes, tBson ) )
		return;

	{
		ScopedComma_c ExplainComma ( tRes, " | ", R"([shape=record,style=filled,bgcolor="lightgrey" label=")", "\"]\n" );
		tRes << String ( tBson.ChildByName ( SZ_TYPE ) );
		ScopedComma_c ParamComma ( tRes, " \\n| ", "{ ", " }" );

		// enclose the rest in brackets, comma-separated
		Bson_c ( tBson.ChildByName ( SZ_OPTIONS ) ).ForEach (
				[&tRes] ( CSphString && sName, const NodeHandle_t & tNode ) {
					tRes.Sprintf ( "%s=%d", sName.cstr (), (int) Int ( tNode ) );
				} );
		if ( Bool ( tBson.ChildByName ( SZ_VIRTUALLY_PLAIN ) ) )
			tRes << "virtually-plain";

		// dump spec for keyword nodes
		RenderAccessSpecs ( tRes, dBson, true );
	}
	int iRoot = iId;

	Bson_c ( tBson.ChildByName ( SZ_CHILDREN ) ).ForEach ( [&] ( const NodeHandle_t & tNode ) {
		++iId;
		tRes << iRoot << " -> " << iId;
		RenderDotBsonNodePlan ( tNode, tRes, iId );
	} );
}

void RenderDotBsonPlan ( bson::NodeHandle_t dBson, StringBuilder_c & tRes )
{
	int iId=0;
	tRes << "digraph \"transformed_tree\"\n{\n";
	RenderDotBsonNodePlan ( dBson, tRes, iId );
	tRes << "}";
}

// parse node to bson
void XQNodeGetExtraBson ( bson::Assoc_c & tNode, const XQNode_t * pNode )
{
	switch ( pNode->GetOp() )
	{
	case SPH_QUERY_PROXIMITY:
	case SPH_QUERY_NEAR:	bson::Obj_c ( tNode.StartObj (SZ_OPTIONS) ).AddInt ( "distance", pNode->m_iOpArg ); break;
	case SPH_QUERY_QUORUM:	bson::Obj_c ( tNode.StartObj (SZ_OPTIONS) ).AddInt ( "count", pNode->m_iOpArg ); break;
	default:					break;
	}
}

void AddAccessSpecsBson ( bson::Assoc_c & tNode, const XQNode_t * pNode, const CSphSchema & tSchema, const StrVec_t & dZones )
{
	assert ( pNode );
	// dump spec for keyword nodes
	// FIXME? double check that spec does *not* affect non keyword nodes
	if ( pNode->m_dSpec.IsEmpty () || pNode->m_dWords.IsEmpty () )
		return;

	const XQLimitSpec_t & s = pNode->m_dSpec;
	if ( s.m_bFieldSpec && !s.m_dFieldMask.TestAll ( true ) )
	{
		StrVec_t dFields;
		for ( int i = 0; i<tSchema.GetFieldsCount (); ++i )
			if ( s.m_dFieldMask.Test ( i ) )
				dFields.Add ( tSchema.GetFieldName ( i ) );
		tNode.AddStringVec( SZ_FIELDS, dFields );
	}
	if ( s.m_iFieldMaxPos )
		tNode.AddInt ( SZ_MAX_FIELD_POS, s.m_iFieldMaxPos );
	if ( s.m_dZones.GetLength () )
		tNode.AddStringVec ( s.m_bZoneSpan ? SZ_ZONESPANS : SZ_ZONES, dZones );
}

void CreateKeywordBson ( bson::Assoc_c& tWord, const XQKeyword_t & tKeyword )
{
	tWord.AddString ( SZ_TYPE, "KEYWORD" );
	tWord.AddString ( SZ_WORD, tKeyword.m_sWord.cstr () );
	tWord.AddInt ( SZ_QUERYPOS, tKeyword.m_iAtomPos );
	if ( tKeyword.m_bExcluded )
		tWord.AddBool ( SZ_EXCLUDED, true );
	if ( tKeyword.m_bExpanded )
		tWord.AddBool ( SZ_EXPANDED, true );
	if ( tKeyword.m_bFieldStart )
		tWord.AddBool ( SZ_FIELD_START, true );
	if ( tKeyword.m_bFieldEnd )
		tWord.AddBool ( SZ_FIELD_END, true );
	if ( tKeyword.m_bMorphed )
		tWord.AddBool ( SZ_MORPHED, true );
	if ( tKeyword.m_fBoost!=1.0f )
		tWord.AddDouble ( SZ_BOOST, tKeyword.m_fBoost );
}

void BuildProfileBson ( bson::Assoc_c& tPlan, const XQNode_t * pNode, const CSphSchema & tSchema, const StrVec_t & dZones )
{
	using namespace bson;
	tPlan.AddString ( SZ_TYPE, sphXQNodeToStr ( pNode ).cstr() );
	XQNodeGetExtraBson ( tPlan, pNode );
	AddAccessSpecsBson ( tPlan, pNode, tSchema, dZones );

	if ( pNode->m_dChildren.GetLength () && pNode->m_dWords.GetLength () )
		tPlan.AddBool ( SZ_VIRTUALLY_PLAIN, true );

	if ( pNode->m_dChildren.IsEmpty () )
	{
		MixedVector_c dChildren ( tPlan.StartMixedVec( SZ_CHILDREN ), pNode->m_dWords.GetLength() );
		for ( const auto & i : pNode->m_dWords )
		{
			Obj_c tWord ( dChildren.StartObj () );
			CreateKeywordBson ( tWord, i );
		}
	} else
	{
		MixedVector_c dChildren ( tPlan.StartMixedVec ( SZ_CHILDREN ), pNode->m_dChildren.GetLength () );
		for ( const auto & i : pNode->m_dChildren )
		{
			Obj_c tChild ( dChildren.StartObj () );
			BuildProfileBson ( tChild, i, tSchema, dZones );
		}
	}
}

} // namespace static

void sph::RenderBsonPlan ( StringBuilder_c& tRes, const bson::NodeHandle_t & dBson, bool bDot )
{
	if ( bDot )
		RenderDotBsonPlan ( dBson, tRes );
	else
		RenderPlainBsonPlan ( dBson, tRes, true, 0, "  ", "\n" );
#if 0
	CSphString sResult1;
	bson::Bson_c ( dBson ).BsonToJson ( sResult1 );
	tRes << "raw: " << sResult1;
#endif
}

CSphString sph::RenderBsonPlanBrief ( const bson::NodeHandle_t& dBson )
{
	StringBuilder_c tRes;
	RenderPlainBsonPlan ( dBson, tRes, false, 0, "", " " );
	CSphString sResult;
	tRes.MoveTo ( sResult );
	return sResult;
}


Bson_t sphExplainQuery ( const XQNode_t * pNode, const CSphSchema & tSchema, const StrVec_t & dZones )
{
	CSphVector<BYTE> dPlan;
	bson::Root_c tPlan ( dPlan );
	::BuildProfileBson ( tPlan, pNode, tSchema, dZones );
	return dPlan;
}


void QueryProfile_c::BuildResult ( XQNode_t * pRoot, const CSphSchema & tSchema, const StrVec_t & dZones )
{
	m_dPlan.Reset();
	bson::Root_c tPlan ( m_dPlan );
	::BuildProfileBson ( tPlan, pRoot, tSchema, dZones );
}

ExtRanker_c::ExtRanker_c ( const XQQuery_t & tXQ, const ISphQwordSetup & tSetup, bool bSkipQCache, bool bCollectHits, bool bUseBM25 )
{
	assert ( tSetup.m_pCtx );

	for ( int i=0; i<MAX_BLOCK_DOCS; i++ )
	{
		m_dMatches[i].Reset ( tSetup.m_iDynamicRowitems );
		m_dMyMatches[i].Reset ( tSetup.m_iDynamicRowitems );
	}
	m_tTestMatch.Reset ( tSetup.m_iDynamicRowitems );

	assert ( tXQ.m_pRoot );
	tSetup.m_pZoneChecker = this;
	m_pRoot = ExtNode_i::Create ( tXQ.m_pRoot, tSetup, bUseBM25 );
	if ( m_pRoot && bCollectHits )
		m_pRoot->SetCollectHits();

#if SPH_TREE_DUMP
	if ( m_pRoot )
		m_pRoot->DebugDump(0);
#endif

	// we generally have three (!) trees for each query
	// 1) parsed tree, a raw result of query parsing
	// 2) transformed tree, with star expansions, morphology, and other transfomations
	// 3) evaluation tree, with tiny keywords cache, and other optimizations
	// tXQ.m_pRoot, passed to ranker from the index, is the transformed tree
	// m_pRoot, internal to ranker, is the evaluation tree
	if ( tSetup.m_pCtx->m_pProfile )
		tSetup.m_pCtx->m_pProfile->BuildResult ( tXQ.m_pRoot, tSetup.m_pIndex->GetMatchSchema(), tXQ.m_dZones );

	m_pIndex = tSetup.m_pIndex;
	m_pCtx = tSetup.m_pCtx;
	m_pNanoBudget = tSetup.m_pStats ? tSetup.m_pStats->m_pNanoBudget : nullptr;

	m_dZones = tXQ.m_dZones;
	m_dZoneStart.Resize ( m_dZones.GetLength() );
	m_dZoneEnd.Resize ( m_dZones.GetLength() );
	m_dZoneMax.Resize ( m_dZones.GetLength() );
	m_dZoneMin.Resize ( m_dZones.GetLength() );
	m_dZoneMax.Fill ( 0 );
	m_dZoneMin.Fill	( INVALID_ROWID );
	m_bZSlist = tXQ.m_bNeedSZlist;
	m_dZoneInfo.Reset ( m_dZones.GetLength() );

	DictRefPtr_c pZonesDict;
	// workaround for a particular case with following conditions
	if ( !m_pIndex->GetDictionary()->GetSettings().m_bWordDict && m_dZones.GetLength() )
		pZonesDict = m_pIndex->GetDictionary()->Clone();

	ARRAY_FOREACH ( i, m_dZones )
	{
		XQKeyword_t tDot;

		tDot.m_sWord.SetSprintf ( "%c%s", MAGIC_CODE_ZONE, m_dZones[i].cstr() );
		ExtNode_i * pStartTerm = ExtNode_i::Create ( tDot, tSetup, pZonesDict, false );
		assert ( pStartTerm );
		pStartTerm->SetCollectHits();
		m_dZoneStartTerm.Add ( pStartTerm );
		m_dZoneStart[i] = nullptr;

		tDot.m_sWord.SetSprintf ( "%c/%s", MAGIC_CODE_ZONE, m_dZones[i].cstr() );
		ExtNode_i * pEndTerm = ExtNode_i::Create ( tDot, tSetup, pZonesDict, false );
		assert ( pEndTerm );
		pEndTerm->SetCollectHits();
		m_dZoneEndTerm.Add ( pEndTerm );
		m_dZoneEnd[i] = nullptr;
	}

	if ( QcacheGetStatus().m_iMaxBytes>0 && !bSkipQCache )
	{
		m_pQcacheEntry = new QcacheEntry_c();
		m_pQcacheEntry->m_iIndexId = m_pIndex->GetIndexId();
	}
	memset ( m_dMyDocs, 0, sizeof ( m_dMyDocs ) );
}


ExtRanker_c::~ExtRanker_c ()
{
	SafeRelease ( m_pQcacheEntry );

	SafeDelete ( m_pRoot );
	ARRAY_FOREACH ( i, m_dZones )
	{
		SafeDelete ( m_dZoneStartTerm[i] );
		SafeDelete ( m_dZoneEndTerm[i] );
	}

	ARRAY_FOREACH ( i, m_dZoneInfo )
	{
		ARRAY_FOREACH ( iDoc, m_dZoneInfo[i] )
		{
			SafeDelete ( m_dZoneInfo[i][iDoc].m_pHits );
		}
		m_dZoneInfo[i].Reset();
	}
}


void ExtRanker_c::Reset ( const ISphQwordSetup & tSetup )
{
	if ( m_pRoot )
		m_pRoot->Reset ( tSetup );
	ARRAY_FOREACH ( i, m_dZones )
	{
		m_dZoneStartTerm[i]->Reset ( tSetup );
		m_dZoneEndTerm[i]->Reset ( tSetup );

		m_dZoneStart[i] = nullptr;
		m_dZoneEnd[i] = nullptr;
	}

	m_dZoneMax.Fill ( 0 );
	m_dZoneMin.Fill ( INVALID_ROWID );
	ARRAY_FOREACH ( i, m_dZoneInfo )
	{
		ARRAY_FOREACH ( iDoc, m_dZoneInfo[i] )
			SafeDelete ( m_dZoneInfo[i][iDoc].m_pHits );
		m_dZoneInfo[i].Reset();
	}

	// Ranker::Reset() happens on a switch to next RT segment
	// next segment => new and shiny docids => gotta restart encoding
	if ( m_pQcacheEntry )
		m_pQcacheEntry->RankerReset();
}


void ExtRanker_c::UpdateQcache ( int iMatches )
{
	if ( m_pQcacheEntry )
	{
		CSphScopedProfile tProf ( m_pCtx->m_pProfile, SPH_QSTATE_QCACHE_UP );
		for ( int i=0; i<iMatches; i++ )
			m_pQcacheEntry->Append ( m_dMatches[i].m_tRowID, m_dMatches[i].m_iWeight );
	}
}


void ExtRanker_c::FinalizeCache ( const ISphSchema & tSorterSchema )
{
	if ( m_pQcacheEntry )
	{
		CSphScopedProfile tProf ( m_pCtx->m_pProfile, SPH_QSTATE_QCACHE_FINAL );
		QcacheAdd ( m_pCtx->m_tQuery, m_pQcacheEntry, tSorterSchema );
	}

	SafeRelease ( m_pQcacheEntry );
}


void ExtRanker_c::CleanupZones ( RowID_t tMaxRowID )
{
	if ( tMaxRowID==INVALID_ROWID )
		return;

	ARRAY_FOREACH ( i, m_dZoneMin )
	{
		RowID_t tMinRowID = m_dZoneMin[i];
		if ( tMinRowID==INVALID_ROWID )
			continue;

		CSphVector<ZoneInfo_t> & dZone = m_dZoneInfo[i];
		int iSpan = FindSpan ( dZone, tMaxRowID );
		if ( iSpan==-1 )
			continue;

		if ( iSpan==dZone.GetLength()-1 )
		{
			ARRAY_FOREACH ( iDoc, dZone )
				SafeDelete ( dZone[iDoc].m_pHits );
			dZone.Resize ( 0 );
			m_dZoneMin[i] = tMaxRowID;
			continue;
		}

		for ( int iDoc=0; iDoc<=iSpan; iDoc++ )
			SafeDelete ( dZone[iDoc].m_pHits );

		int iLen = dZone.GetLength() - iSpan - 1;
		memmove ( dZone.Begin(), dZone.Begin()+iSpan+1, sizeof(dZone[0]) * iLen );
		dZone.Resize ( iLen );
		m_dZoneMin[i] = dZone.Begin()->m_tRowID;
	}
}


void ExtRanker_c::SetQwordsIDF ( const ExtQwordsHash_t & hQwords )
{
	m_iQwords = hQwords.GetLength ();
	if ( m_pRoot )
		m_pRoot->SetQwordsIDF ( hQwords );
}


static SphZoneHit_e ZoneCacheFind ( const ZoneVVector_t & dZones, int iZone, const ExtHit_t * pHit, int * pLastSpan )
{
	if ( !dZones[iZone].GetLength() )
		return SPH_ZONE_NO_DOCUMENT;

	ZoneInfo_t * pZone = dZones[iZone].BinarySearch ( bind ( &ZoneInfo_t::m_tRowID ), pHit->m_tRowID );

	if ( pZone )
	{
		// remove end markers that might mess up ordering
		Hitpos_t uPosWithField = HITMAN::GetPosWithField ( pHit->m_uHitpos );
		int iSpan = FindSpan ( pZone->m_pHits->m_dStarts, uPosWithField );
		if ( iSpan<0 || uPosWithField>pZone->m_pHits->m_dEnds[iSpan] )
			return SPH_ZONE_NO_SPAN;
		if ( pLastSpan )
			*pLastSpan = iSpan;
		return SPH_ZONE_FOUND;
	}

	return SPH_ZONE_NO_DOCUMENT;
}


SphZoneHit_e ExtRanker_c::IsInZone ( int iZone, const ExtHit_t * pHit, int * pLastSpan )
{
	// quick route, we have current docid cached
	SphZoneHit_e eRes = ZoneCacheFind ( m_dZoneInfo, iZone, pHit, pLastSpan );
	if ( eRes!=SPH_ZONE_NO_DOCUMENT )
		return eRes;

	// is there any zone info for this document at all?
	if ( pHit->m_tRowID<m_dZoneMax[iZone] )
		return SPH_ZONE_NO_DOCUMENT;

	// long route, read in zone info for all (!) the documents until next requested
	// that's because we might be queried out of order

	// current chunk
	const ExtDoc_t * pStart = m_dZoneStart[iZone];
	const ExtDoc_t * pEnd = m_dZoneEnd[iZone];

	// now keep caching spans until we see current id
	while ( pHit->m_tRowID >= m_dZoneMax[iZone] )
	{
		// get more docs if needed
		if ( ( !pStart && m_dZoneMax[iZone]!=INVALID_ROWID ) || pStart->m_tRowID==INVALID_ROWID )
		{
			pStart = m_dZoneStartTerm[iZone]->GetDocsChunk();
			if ( !pStart )
			{
				m_dZoneMax[iZone] = INVALID_ROWID;
				return SPH_ZONE_NO_DOCUMENT;
			}
		}

		if ( ( !pEnd && m_dZoneMax[iZone]!=INVALID_ROWID ) || pEnd->m_tRowID==INVALID_ROWID )
		{
			pEnd = m_dZoneEndTerm[iZone]->GetDocsChunk();
			if ( !pEnd )
			{
				m_dZoneMax[iZone] = INVALID_ROWID;
				return SPH_ZONE_NO_DOCUMENT;
			}
		}

		assert ( pStart && pEnd );

		// skip zone starts past already cached stuff
		while ( pStart->m_tRowID<m_dZoneMax[iZone] )
			pStart++;
		if ( pStart->m_tRowID==INVALID_ROWID )
			continue;

		// skip zone ends until a match with start
		while ( pEnd->m_tRowID<pStart->m_tRowID )
			pEnd++;
		if ( pEnd->m_tRowID==INVALID_ROWID )
			continue;

		// handle mismatching start/end ids
		// (this must never happen normally, but who knows what data we're fed)
		assert ( pStart->m_tRowID!=INVALID_ROWID );
		assert ( pEnd->m_tRowID!=INVALID_ROWID );
		assert ( pStart->m_tRowID<=pEnd->m_tRowID );

		if ( pStart->m_tRowID!=pEnd->m_tRowID )
		{
			while ( pStart->m_tRowID < pEnd->m_tRowID )
				pStart++;
			if ( pStart->m_tRowID==INVALID_ROWID )
				continue;
		}

		// first matching uncached docid found!
		assert ( pStart->m_tRowID==pEnd->m_tRowID );
		assert ( pStart->m_tRowID >= m_dZoneMax[iZone] );

		// but maybe we don't need docid this big just yet?
		if ( pStart->m_tRowID > pHit->m_tRowID )
		{
			// store current in-chunk positions
			m_dZoneStart[iZone] = pStart;
			m_dZoneEnd[iZone] = pEnd;

			// no zone info for all those precending documents (including requested one)
			m_dZoneMax[iZone] = pStart->m_tRowID;
			return SPH_ZONE_NO_DOCUMENT;
		}

		// cache all matching docs from current chunks below requested docid
		// (there might be more matching docs, but we are lazy and won't cache them upfront)
		ExtDoc_t dCache [ MAX_BLOCK_DOCS ];
		int iCache = 0;

		while ( pStart->m_tRowID<=pHit->m_tRowID )
		{
			// match
			if ( pStart->m_tRowID==pEnd->m_tRowID )
			{
				dCache[iCache++] = *pStart;
				pStart++;
				pEnd++;
				continue;
			}

			// mismatch!
			// this must not really happen, starts/ends must be in sync
			// but let's be graceful anyway, and just skip to next match
			if ( pStart->m_tRowID==INVALID_ROWID || pEnd->m_tRowID==INVALID_ROWID )
				break;

			while ( pStart->m_tRowID < pEnd->m_tRowID )
				pStart++;
			if ( pStart->m_tRowID==INVALID_ROWID )
				break;

			while ( pEnd->m_tRowID < pStart->m_tRowID )
				pEnd++;
			if ( pEnd->m_tRowID==INVALID_ROWID )
				break;
		}

		// should have found at least one id to cache
		assert ( iCache );
		assert ( iCache < MAX_BLOCK_DOCS );
		dCache[iCache].m_tRowID = INVALID_ROWID;

		// do caching
		const ExtHit_t * pStartHits = m_dZoneStartTerm[iZone]->GetHits ( dCache );
		const ExtHit_t * pEndHits = m_dZoneEndTerm[iZone]->GetHits ( dCache );
		int iReserveStart = m_dZoneStartTerm[iZone]->GetHitsCount() / Max ( m_dZoneStartTerm[iZone]->GetDocsCount(), 1 );
		int iReserveEnd = m_dZoneEndTerm[iZone]->GetHitsCount() / Max ( m_dZoneEndTerm[iZone]->GetDocsCount(), 1 );
		int iReserve = Max ( iReserveStart, iReserveEnd );

		// loop documents one by one
		while ( pStartHits->m_tRowID!=INVALID_ROWID && pEndHits->m_tRowID!=INVALID_ROWID )
		{
			// load all hits for current document
			RowID_t tCurRowID = pStartHits->m_tRowID;

			// FIXME!!! replace by iterate then add elements to vector instead of searching each time
			ZoneHits_t * pZone = nullptr;
			CSphVector<ZoneInfo_t> & dZones = m_dZoneInfo[iZone];
			if ( dZones.GetLength() )
			{
				ZoneInfo_t * pInfo = dZones.BinarySearch ( bind ( &ZoneInfo_t::m_tRowID ), tCurRowID );
				if ( pInfo )
					pZone = pInfo->m_pHits;
			}
			if ( !pZone )
			{
				if ( dZones.GetLength() && dZones.Last().m_tRowID>tCurRowID )
				{
					int iInsertPos = FindSpan ( dZones, tCurRowID );
					if ( iInsertPos>=0 )
					{
						dZones.Insert ( iInsertPos, ZoneInfo_t() );
						dZones[iInsertPos].m_tRowID = tCurRowID;
						pZone = dZones[iInsertPos].m_pHits = new ZoneHits_t();
					}
				} else
				{
					ZoneInfo_t & tElem = dZones.Add ();
					tElem.m_tRowID = tCurRowID;
					pZone = tElem.m_pHits = new ZoneHits_t();
				}
				if ( pZone )
				{
					pZone->m_dStarts.Reserve ( iReserve );
					pZone->m_dEnds.Reserve ( iReserve );
				}
			}

			assert ( pEndHits->m_tRowID==tCurRowID );

			// load all the pairs of start and end hits for it
			// do it by with the FSM:
			//
			// state 'begin':
			// - start marker -> set state 'inspan', startspan=pos++
			// - end marker -> pos++
			// - end of doc -> set state 'finish'
			//
			// state 'inspan':
			// - start marker -> startspan = pos++
			// - end marker -> set state 'outspan', endspan=pos++
			// - end of doc -> set state 'finish'
			//
			// state 'outspan':
			// - start marker -> set state 'inspan', commit span, startspan=pos++
			// - end marker -> endspan = pos++
			// - end of doc -> set state 'finish', commit span
			//
			// state 'finish':
			// - we are done.

			int bEofDoc = 0;

			// state 'begin' is here.
			while ( !bEofDoc && pEndHits->m_uHitpos < pStartHits->m_uHitpos )
			{
				++pEndHits;
				bEofDoc |= (pEndHits->m_tRowID!=tCurRowID)?2:0;
			}

			if ( !bEofDoc )
			{
				// state 'inspan' (true) or 'outspan' (false)
				bool bInSpan = true;
				Hitpos_t iSpanBegin = pStartHits->m_uHitpos;
				Hitpos_t iSpanEnd = pEndHits->m_uHitpos;
				while ( bEofDoc!=3 ) /// action end-of-doc
				{
					// action inspan/start-marker
					if ( bInSpan )
					{
						++pStartHits;
						bEofDoc |= (pStartHits->m_tRowID!=tCurRowID)?1:0;
					} else
						// action outspan/end-marker
					{
						++pEndHits;
						bEofDoc |= (pEndHits->m_tRowID!=tCurRowID)?2:0;
					}

					if ( !( bEofDoc & 1 ) && pStartHits->m_uHitpos<pEndHits->m_uHitpos )
					{
						// actions for outspan/start-marker state
						// <b>...<b>..<b>..</b> will ignore all the <b> inside.
						if ( !bInSpan )
						{
							bInSpan = true;
							pZone->m_dStarts.Add ( iSpanBegin );
							pZone->m_dEnds.Add ( iSpanEnd );
							iSpanBegin = pStartHits->m_uHitpos;
						}
					} else if ( !( bEofDoc & 2 ) )
					{
						// actions for inspan/end-marker state
						// so, <b>...</b>..</b>..</b> will ignore all the </b> inside.
						bInSpan = false;
						iSpanEnd = pEndHits->m_uHitpos;
					}
				}
				// action 'commit' for outspan/end-of-doc
				if ( iSpanBegin < iSpanEnd )
				{
					pZone->m_dStarts.Add ( iSpanBegin );
					pZone->m_dEnds.Add ( iSpanEnd );
				}
			}

			// data sanity checks
			assert ( pZone->m_dStarts.GetLength()==pZone->m_dEnds.GetLength() );

			// update cache status
			m_dZoneMax[iZone] = tCurRowID+1;
			m_dZoneMin[iZone] = Min ( m_dZoneMin[iZone], tCurRowID );
		}
	}

	// store current in-chunk positions
	m_dZoneStart[iZone] = pStart;
	m_dZoneEnd[iZone] = pEnd;

	// cached a bunch of spans, try our check again
	return ZoneCacheFind ( m_dZoneInfo, iZone, pHit, pLastSpan );
}

//////////////////////////////////////////////////////////////////////////
template<bool USE_BM25>
ExtRanker_T<USE_BM25>::ExtRanker_T ( const XQQuery_t & tXQ, const ISphQwordSetup & tSetup, bool bSkipQCache, bool bCollectHits )
	: ExtRanker_c ( tXQ, tSetup, bSkipQCache, bCollectHits, USE_BM25 )
{}


template<bool USE_BM25>
const ExtDoc_t * ExtRanker_T<USE_BM25>::GetFilteredDocs ()
{
#if QDEBUG
	printf ( "ranker getfiltereddocs %p\n", this );
#endif

	while (true)
	{
		// get another chunk
		SwitchProfile ( m_pCtx->m_pProfile, SPH_QSTATE_GET_DOCS );
		const ExtDoc_t * pCand = m_pRoot->GetDocsChunk();
		if ( !pCand )
		{
			SwitchProfile ( m_pCtx->m_pProfile, SPH_QSTATE_RANK );
			return nullptr;
		}

		// create matches, and filter them
		SwitchProfile ( m_pCtx->m_pProfile, SPH_QSTATE_FILTER );
		int iDocs = 0;
		RowID_t tMaxRowID = 0;
		while ( pCand->m_tRowID!=INVALID_ROWID )
		{
			m_tTestMatch.m_tRowID = pCand->m_tRowID;
			m_tTestMatch.m_pStatic = nullptr;

			if ( m_pIndex->EarlyReject ( m_pCtx, m_tTestMatch ) )
			{
				pCand++;
				continue;
			}

			tMaxRowID = pCand->m_tRowID;
			m_dMyDocs[iDocs] = *pCand;

			if_const ( USE_BM25 )
				m_tTestMatch.m_iWeight = (int)( (pCand->m_fTFIDF+0.5f)*SPH_BM25_SCALE );

			Swap ( m_tTestMatch, m_dMyMatches[iDocs] );
			iDocs++;
			pCand++;
		}

		SwitchProfile ( m_pCtx->m_pProfile, SPH_QSTATE_RANK );

		// clean up zone hash
		if ( !m_bZSlist )
			CleanupZones ( tMaxRowID );

		if ( iDocs )
		{
			if ( m_pNanoBudget )
				*m_pNanoBudget -= g_iPredictorCostMatch*iDocs;
			m_dMyDocs[iDocs].m_tRowID = INVALID_ROWID;
			return m_dMyDocs;
		}
	}
}


//////////////////////////////////////////////////////////////////////////

template < bool USE_BM25 >
int ExtRanker_WeightSum_c<USE_BM25>::GetMatches ()
{
	if ( !this->m_pRoot )
		return 0;

	SwitchProfile ( this->m_pCtx->m_pProfile, SPH_QSTATE_RANK );
	const ExtDoc_t * pDoc = this->m_pDoclist;
	int iMatches = 0;
	const int iWeights = Min ( m_iWeights, 32 );

	while ( iMatches<MAX_BLOCK_DOCS )
	{
		if ( !pDoc || pDoc->m_tRowID==INVALID_ROWID ) pDoc = this->GetFilteredDocs ();
		if ( !pDoc ) break;

		DWORD uRank = 0;
		DWORD uMask = pDoc->m_uDocFields;
		if ( !uMask )
		{
			// possible if we have more than 32 fields
			// honestly loading all hits etc is cumbersome, so let's just fake it
			uRank = 1;
		} else
		{
			// just sum weights over the lowest 32 fields
			for ( int i=0; i<iWeights; i++ )
				if ( uMask & (1<<i) )
					uRank += m_pWeights[i];
		}

		Swap ( this->m_dMatches[iMatches], this->m_dMyMatches[pDoc-this->m_dMyDocs] ); // OPTIMIZE? can avoid this swap and simply return m_dMyMatches (though in lesser chunks)
		if_const ( USE_BM25 )
			this->m_dMatches[iMatches].m_iWeight = this->m_dMatches[iMatches].m_iWeight + uRank*SPH_BM25_SCALE;
		else
			this->m_dMatches[iMatches].m_iWeight = uRank;

		iMatches++;
		pDoc++;
	}

	this->UpdateQcache ( iMatches );

	this->m_pDoclist = pDoc;
	return iMatches;
}

//////////////////////////////////////////////////////////////////////////

int ExtRanker_None_c::GetMatches ()
{
	if ( !m_pRoot )
		return 0;

	SwitchProfile ( m_pCtx->m_pProfile, SPH_QSTATE_RANK );
	const ExtDoc_t * pDoc = m_pDoclist;
	int iMatches = 0;

	while ( iMatches<MAX_BLOCK_DOCS )
	{
		if ( !pDoc || pDoc->m_tRowID==INVALID_ROWID ) pDoc = GetFilteredDocs ();
		if ( !pDoc ) break;

		Swap ( m_dMatches[iMatches], m_dMyMatches[pDoc-m_dMyDocs] ); // OPTIMIZE? can avoid this swap and simply return m_dMyMatches (though in lesser chunks)
		m_dMatches[iMatches].m_iWeight = 1;
		iMatches++;
		pDoc++;
	}

	UpdateQcache ( iMatches );

	m_pDoclist = pDoc;
	return iMatches;
}

//////////////////////////////////////////////////////////////////////////

template < typename STATE, bool USE_BM25 >
ExtRanker_State_T<STATE,USE_BM25>::ExtRanker_State_T ( const XQQuery_t & tXQ, const ISphQwordSetup & tSetup, bool bSkipQCache, bool bCollectHits )
	: ExtRanker_T<USE_BM25> ( tXQ, tSetup, bSkipQCache, bCollectHits )
{
	// FIXME!!! move out the disable of m_bZSlist in case no zonespan nodes
	if ( this->m_bZSlist )
		m_dZonespans.Reserve ( MAX_BLOCK_DOCS * this->m_dZones.GetLength() );

	m_pHitBase = nullptr;
}


static inline const ExtHit_t * RankerGetHits ( QueryProfile_c * pProfile, ExtNode_i * pRoot, const ExtDoc_t * pDocs )
{
	if ( !pProfile )
		return pRoot->GetHits ( pDocs );

	pProfile->Switch ( SPH_QSTATE_GET_HITS );
	const ExtHit_t * pHlist = pRoot->GetHits ( pDocs );
	pProfile->Switch ( SPH_QSTATE_RANK );
	return pHlist;
}


template < typename STATE, bool USE_BM25 >
int ExtRanker_State_T<STATE,USE_BM25>::GetMatches ()
{
	if ( !this->m_pRoot )
		return 0;

	SwitchProfile ( this->m_pCtx->m_pProfile, SPH_QSTATE_RANK );
	QueryProfile_c * pProfile = this->m_pCtx->m_pProfile;
	int iMatches = 0;
	const ExtHit_t * pHlist = this->m_pHitlist;
	const ExtHit_t * pHitBase = m_pHitBase;
	const ExtDoc_t * pDocs = this->m_pDoclist;
	m_dZonespans.Resize(1);
	int	iLastZoneData = 0;

	CSphVector<int> dSpans;
	if ( this->m_bZSlist )
	{
		dSpans.Resize ( this->m_dZones.GetLength() );
		dSpans.Fill ( -1 );
	}

	// warmup if necessary
	if ( !pDocs )
	{
		pDocs = this->GetFilteredDocs ();
		if ( !pDocs )
		{
			this->UpdateQcache(0);
			return 0;
		}

		pHlist = RankerGetHits ( pProfile, this->m_pRoot, pDocs );
	}

	if ( !pHitBase )
		pHitBase = pHlist;

	// main matching loop
	const ExtDoc_t * pDoc = pDocs;
	for ( RowID_t tCurRowID=INVALID_ROWID; iMatches < MAX_BLOCK_DOCS; )
	{
		// keep ranking
		while ( pHlist->m_tRowID==tCurRowID )
		{
			m_tState.Update ( pHlist );
			if ( this->m_bZSlist )
			{
				ARRAY_FOREACH ( i, this->m_dZones )
				{
					int iSpan;
					if ( this->IsInZone ( i, pHlist, &iSpan )!=SPH_ZONE_FOUND )
						continue;

					if ( iSpan!=dSpans[i] )
					{
						m_dZonespans.Add ( i );
						m_dZonespans.Add ( iSpan );
						dSpans[i] = iSpan;
					}
				}
			}
			++pHlist;
		}

		// flush current doc
		if ( tCurRowID!=INVALID_ROWID )
		{
			assert ( tCurRowID==pDoc->m_tRowID );
			Swap ( this->m_dMatches[iMatches], this->m_dMyMatches[pDoc-this->m_dMyDocs] );
			this->m_dMatches[iMatches].m_iWeight = m_tState.Finalize ( this->m_dMatches[iMatches] );
			if ( this->m_bZSlist )
			{
				m_dZonespans[iLastZoneData] = m_dZonespans.GetLength() - iLastZoneData - 1;
				this->m_dMatches[iMatches].m_iTag = iLastZoneData;

				iLastZoneData = m_dZonespans.GetLength();
				m_dZonespans.Add(0);

				dSpans.Fill ( -1 );
			}
			iMatches++;
		}

		// boundary checks
		if ( pHlist->m_tRowID==INVALID_ROWID )
		{
			if ( this->m_bZSlist && tCurRowID!=INVALID_ROWID )
				this->CleanupZones ( tCurRowID );

			// there are no more hits for current docs block; do we have a next one?
			assert ( pDocs );
			pDoc = pDocs = this->GetFilteredDocs ();

			// we don't, so bail out
			if ( !pDocs )
				break;

			// we do, get some hits with proper profile
			pHlist = RankerGetHits ( pProfile, this->m_pRoot, pDocs );
		}

		// skip until next good doc/hit pair
		assert ( pDoc->m_tRowID<=pHlist->m_tRowID );
		while ( pDoc->m_tRowID<pHlist->m_tRowID ) pDoc++;
		assert ( pDoc->m_tRowID==pHlist->m_tRowID );

		tCurRowID = pHlist->m_tRowID;
	}

	this->m_pDoclist = pDocs;
	this->m_pHitlist = pHlist;
	if ( !m_pHitBase )
		m_pHitBase = pHitBase;

	this->UpdateQcache ( iMatches );

	return iMatches;
}

//////////////////////////////////////////////////////////////////////////

template < bool USE_BM25, bool HANDLE_DUPES >
struct RankerState_Proximity_fn : public ISphExtra
{
	BYTE m_uLCS[SPH_MAX_FIELDS];
	BYTE m_uCurLCS;
	int m_iExpDelta;
	int m_iLastHitPosWithField;
	int m_iFields;
	const int * m_pWeights;

	DWORD m_uLcsTailPos;
	DWORD m_uLcsTailQposMask;
	DWORD m_uCurQposMask;
	DWORD m_uCurPos;

	bool Init ( int iFields, const int * pWeights, ExtRanker_c *, CSphString &, DWORD )
	{
		memset ( m_uLCS, 0, sizeof(m_uLCS) );
		m_uCurLCS = 0;
		m_iExpDelta = -INT_MAX;
		m_iLastHitPosWithField = -INT_MAX;
		m_iFields = iFields;
		m_pWeights = pWeights;

		m_uLcsTailPos = 0;
		m_uLcsTailQposMask = 0;
		m_uCurQposMask = 0;
		m_uCurPos = 0;

		return true;
	}

	void Update ( const ExtHit_t * pHlist )
	{
		if_const ( !HANDLE_DUPES )
		{
			// all query keywords are unique
			// simpler path (just do the delta)
			const int iPosWithField = HITMAN::GetPosWithField ( pHlist->m_uHitpos );
			int iDelta = iPosWithField - pHlist->m_uQuerypos;
			if ( iPosWithField>m_iLastHitPosWithField )
				m_uCurLCS = ( ( iDelta==m_iExpDelta ) ? m_uCurLCS : 0 ) + BYTE(pHlist->m_uWeight);

			DWORD uField = HITMAN::GetField ( pHlist->m_uHitpos );
			if ( m_uCurLCS>m_uLCS[uField] )
				m_uLCS[uField] = m_uCurLCS;

			m_iLastHitPosWithField = iPosWithField;
			m_iExpDelta = iDelta + pHlist->m_uSpanlen - 1; // !COMMIT why spanlen??
		} else
		{
			// keywords are duplicated in the query
			// so there might be multiple qpos entries sharing the same hitpos
			DWORD uPos = HITMAN::GetPosWithField ( pHlist->m_uHitpos );
			DWORD uField = HITMAN::GetField ( pHlist->m_uHitpos );

			// reset accumulated data from previous field
			if ( (DWORD)HITMAN::GetField ( m_uCurPos )!=uField )
				m_uCurQposMask = 0;

			if ( uPos!=m_uCurPos )
			{
				// next new and shiny hitpos in line
				// FIXME!? what do we do with longer spans? keep looking? reset?
				if ( m_uCurLCS<2 )
				{
					m_uLcsTailPos = m_uCurPos;
					m_uLcsTailQposMask = m_uCurQposMask;
					m_uCurLCS = 1; // FIXME!? can this ever be different? ("a b" c) maybe..
				}
				m_uCurQposMask = 0;
				m_uCurPos = uPos;
				if ( m_uLCS[uField] < pHlist->m_uWeight )
					m_uLCS[uField] = BYTE(pHlist->m_uWeight);
			}

			// add that qpos to current qpos mask (for the current hitpos)
			m_uCurQposMask |= ( 1UL << pHlist->m_uQuerypos );

			// and check if that results in a better lcs match now
			int iDelta = m_uCurPos - m_uLcsTailPos;
			if ( iDelta && iDelta<32 && ( m_uCurQposMask >> iDelta ) & m_uLcsTailQposMask )
			{
				// cool, it matched!
				m_uLcsTailQposMask = ( 1UL << pHlist->m_uQuerypos ); // our lcs span now ends with a specific qpos
				m_uLcsTailPos = m_uCurPos; // and in a specific position
				m_uCurLCS = BYTE ( m_uCurLCS + pHlist->m_uWeight ); // and it's longer
				m_uCurQposMask = 0; // and we should avoid matching subsequent hits on the same hitpos

				// update per-field vector
				if ( m_uCurLCS>m_uLCS[uField] )
					m_uLCS[uField] = m_uCurLCS;
			}
		}
	}

	int Finalize ( const CSphMatch & tMatch )
	{
		m_uCurLCS = 0;
		m_iExpDelta = -1;
		m_iLastHitPosWithField = -1;

		if_const ( HANDLE_DUPES )
		{
			m_uLcsTailPos = 0;
			m_uLcsTailQposMask = 0;
			m_uCurQposMask = 0;
			m_uCurPos = 0;
		}

		int iRank = 0;
		for ( int i=0; i<m_iFields; i++ )
		{
			iRank += (int)(m_uLCS[i])*m_pWeights[i];
			m_uLCS[i] = 0;
		}

		return USE_BM25 ? tMatch.m_iWeight + iRank*SPH_BM25_SCALE : iRank;
	}
};

//////////////////////////////////////////////////////////////////////////

// sph04, proximity + exact boost
struct RankerState_ProximityBM25Exact_fn : public ISphExtra
{
	BYTE m_uLCS[SPH_MAX_FIELDS];
	BYTE m_uCurLCS;
	int m_iExpDelta;
	int m_iLastHitPos;
	DWORD m_uMinExpPos;
	int m_iFields;
	const int * m_pWeights;
	DWORD m_uHeadHit;
	DWORD m_uExactHit;
	int m_iMaxQuerypos;

	bool Init ( int iFields, const int * pWeights, ExtRanker_c * pRanker, CSphString &, DWORD )
	{
		memset ( m_uLCS, 0, sizeof(m_uLCS) );
		m_uCurLCS = 0;
		m_iExpDelta = -INT_MAX;
		m_iLastHitPos = -1;
		m_uMinExpPos = 0;
		m_iFields = iFields;
		m_pWeights = pWeights;
		m_uHeadHit = 0;
		m_uExactHit = 0;

		// tricky bit
		// in expr and export rankers, this gets handled by the overridden (!) SetQwordsIDF()
		// but in all the other ones, we need this, because SetQwordsIDF() won't touch the state by default
		// FIXME? this is actually MaxUniqueQpos, queries like [foo|foo|foo] might break
		m_iMaxQuerypos = pRanker->m_iMaxQpos;
		return true;
	}

	void Update ( const ExtHit_t * pHlist )
	{
		// upd LCS
		DWORD uField = HITMAN::GetField ( pHlist->m_uHitpos );
		int iPosWithField = HITMAN::GetPosWithField ( pHlist->m_uHitpos );
		int iDelta = iPosWithField - pHlist->m_uQuerypos;

		if ( iDelta==m_iExpDelta && HITMAN::GetPosWithField ( pHlist->m_uHitpos )>=m_uMinExpPos )
		{
			if ( iPosWithField>m_iLastHitPos )
				m_uCurLCS = (BYTE)( m_uCurLCS + pHlist->m_uWeight );
			if ( HITMAN::IsEnd ( pHlist->m_uHitpos )
				&& (int)pHlist->m_uQuerypos==m_iMaxQuerypos
				&& HITMAN::GetPos ( pHlist->m_uHitpos )==m_iMaxQuerypos )
			{
				m_uExactHit |= ( 1UL << HITMAN::GetField ( pHlist->m_uHitpos ) );
			}
		} else
		{
			if ( iPosWithField>m_iLastHitPos )
				m_uCurLCS = BYTE(pHlist->m_uWeight);
			if ( HITMAN::GetPos ( pHlist->m_uHitpos )==1 )
			{
				m_uHeadHit |= ( 1UL << HITMAN::GetField ( pHlist->m_uHitpos ) );
				if ( HITMAN::IsEnd ( pHlist->m_uHitpos ) && m_iMaxQuerypos==1 )
					m_uExactHit |= ( 1UL << HITMAN::GetField ( pHlist->m_uHitpos ) );
			}
		}

		if ( m_uCurLCS>m_uLCS[uField] )
			m_uLCS[uField] = m_uCurLCS;

		m_iExpDelta = iDelta + pHlist->m_uSpanlen - 1;
		m_iLastHitPos = iPosWithField;
		m_uMinExpPos = HITMAN::GetPosWithField ( pHlist->m_uHitpos ) + 1;
	}

	int Finalize ( const CSphMatch & tMatch )
	{
		m_uCurLCS = 0;
		m_iExpDelta = -1;
		m_iLastHitPos = -1;

		int iRank = 0;
		for ( int i=0; i<m_iFields; i++ )
		{
			iRank += (int)( 4*m_uLCS[i] + 2*((m_uHeadHit>>i)&1) + ((m_uExactHit>>i)&1) )*m_pWeights[i];
			m_uLCS[i] = 0;
		}
		m_uHeadHit = 0;
		m_uExactHit = 0;

		return tMatch.m_iWeight + iRank*SPH_BM25_SCALE;
	}
};


template < bool USE_BM25 >
struct RankerState_ProximityPayload_fn : public RankerState_Proximity_fn<USE_BM25,false>
{
	DWORD m_uPayloadRank;
	DWORD m_uPayloadMask;

	bool Init ( int iFields, const int * pWeights, ExtRanker_c * pRanker, CSphString & sError, DWORD )
	{
		RankerState_Proximity_fn<USE_BM25,false>::Init ( iFields, pWeights, pRanker, sError, false );
		m_uPayloadRank = 0;
		m_uPayloadMask = pRanker->m_uPayloadMask;
		return true;
	}

	void Update ( const ExtHit_t * pHlist )
	{
		DWORD uField = HITMAN::GetField ( pHlist->m_uHitpos );
		if ( ( 1<<uField ) & m_uPayloadMask )
			this->m_uPayloadRank += HITMAN::GetPos ( pHlist->m_uHitpos ) * this->m_pWeights[uField];
		else
			RankerState_Proximity_fn<USE_BM25,false>::Update ( pHlist );
	}

	int Finalize ( const CSphMatch & tMatch )
	{
		// as usual, redundant 'this' is just because gcc is stupid
		this->m_uCurLCS = 0;
		this->m_iExpDelta = -1;
		this->m_iLastHitPosWithField = -1;

		int iRank = (int)m_uPayloadRank;
		for ( int i=0; i<this->m_iFields; i++ )
		{
			// no special care for payload fields as their LCS will be 0 anyway
			iRank += (int)(this->m_uLCS[i])*this->m_pWeights[i];
			this->m_uLCS[i] = 0;
		}

		m_uPayloadRank = 0;
		return USE_BM25 ? tMatch.m_iWeight + iRank*SPH_BM25_SCALE : iRank;
	}
};

//////////////////////////////////////////////////////////////////////////

struct RankerState_MatchAny_fn : public RankerState_Proximity_fn<false,false>
{
	int m_iPhraseK;
	BYTE m_uMatchMask[SPH_MAX_FIELDS];

	bool Init ( int iFields, const int * pWeights, ExtRanker_c * pRanker, CSphString & sError, DWORD )
	{
		RankerState_Proximity_fn<false,false>::Init ( iFields, pWeights, pRanker, sError, false );
		m_iPhraseK = 0;
		for ( int i=0; i<iFields; i++ )
			m_iPhraseK += pWeights[i] * pRanker->m_iQwords;
		memset ( m_uMatchMask, 0, sizeof(m_uMatchMask) );
		return true;
	}

	void Update ( const ExtHit_t * pHlist )
	{
		RankerState_Proximity_fn<false,false>::Update ( pHlist );
		m_uMatchMask [ HITMAN::GetField ( pHlist->m_uHitpos ) ] |= ( 1<<(pHlist->m_uQuerypos-1) );
	}

	int Finalize ( const CSphMatch & )
	{
		m_uCurLCS = 0;
		m_iExpDelta = -1;
		m_iLastHitPosWithField = -1;

		int iRank = 0;
		for ( int i=0; i<m_iFields; ++i )
		{
			if ( m_uMatchMask[i] )
				iRank += (int)( sphBitCount ( m_uMatchMask[i] ) + ( m_uLCS[i]-1 )*m_iPhraseK )*m_pWeights[i];
			m_uMatchMask[i] = 0;
			m_uLCS[i] = 0;
		}

		return iRank;
	}
};

//////////////////////////////////////////////////////////////////////////

struct RankerState_Wordcount_fn : public ISphExtra
{
	int m_iRank;
	const int * m_pWeights;

	bool Init ( int, const int * pWeights, ExtRanker_c *, CSphString &, DWORD )
	{
		m_iRank = 0;
		m_pWeights = pWeights;
		return true;
	}

	void Update ( const ExtHit_t * pHlist )
	{
		m_iRank += m_pWeights [ HITMAN::GetField ( pHlist->m_uHitpos ) ];
	}

	int Finalize ( const CSphMatch & )
	{
		int iRes = m_iRank;
		m_iRank = 0;
		return iRes;
	}
};

//////////////////////////////////////////////////////////////////////////

struct RankerState_Fieldmask_fn : public ISphExtra
{
	DWORD m_uRank;

	bool Init ( int, const int *, ExtRanker_c *, CSphString &, DWORD )
	{
		m_uRank = 0;
		return true;
	}

	void Update ( const ExtHit_t * pHlist )
	{
		m_uRank |= 1UL << HITMAN::GetField ( pHlist->m_uHitpos );
	}

	int Finalize ( const CSphMatch & )
	{
		DWORD uRes = m_uRank;
		m_uRank = 0;
		return uRes;
	}
};


struct RankerState_Plugin_fn final : public ISphExtra
{
	RankerState_Plugin_fn() = default;

	~RankerState_Plugin_fn() final
	{
		assert ( m_pPlugin );
		if ( m_pPlugin->m_fnDeinit )
			m_pPlugin->m_fnDeinit ( m_pData );
		m_pPlugin->Release();
	}

	bool Init ( int iFields, const int * pWeights, ExtRanker_c * pRanker, CSphString & sError, DWORD )
	{
		if ( !m_pPlugin->m_fnInit )
			return true;

		SPH_RANKER_INIT r;
		r.num_field_weights = iFields;
		r.field_weights = const_cast<int*>(pWeights);
		r.options = m_sOptions.cstr();
		r.payload_mask = pRanker->m_uPayloadMask;
		r.num_query_words = pRanker->m_iQwords;
		r.max_qpos = pRanker->m_iMaxQpos;

		char sErrorBuf [ SPH_UDF_ERROR_LEN ];
		if ( m_pPlugin->m_fnInit ( &m_pData, &r, sErrorBuf )==0 )
			return true;
		sError = sErrorBuf;
		return false;
	}

	void Update ( const ExtHit_t * p )
	{
		if ( !m_pPlugin->m_fnUpdate )
			return;

		SPH_RANKER_HIT h;
		h.doc_id = p->m_tRowID;
		h.hit_pos = p->m_uHitpos;
		h.query_pos = p->m_uQuerypos;
		h.node_pos = p->m_uNodepos;
		h.span_length = p->m_uSpanlen;
		h.match_length = p->m_uMatchlen;
		h.weight = p->m_uWeight;
		h.query_pos_mask = p->m_uQposMask;
		m_pPlugin->m_fnUpdate ( m_pData, &h );
	}

	int Finalize ( const CSphMatch & tMatch )
	{
		// at some point in the future, we might start passing the entire match,
		// with blackjack, hookers, attributes, and their schema; but at this point,
		// the only sort-of useful part of a match that we are able to push down to
		// the ranker plugin is the match weight
		return m_pPlugin->m_fnFinalize ( m_pData, tMatch.m_iWeight );
	}

private:
	void *					m_pData = nullptr;
	const PluginRanker_c *	m_pPlugin = nullptr;
	CSphString				m_sOptions;

	bool ExtraDataImpl ( ExtraData_e eType, void ** ppResult ) final
	{
		switch ( eType )
		{
			case EXTRA_SET_RANKER_PLUGIN:		m_pPlugin = (const PluginRanker_c*)ppResult; break;
			case EXTRA_SET_RANKER_PLUGIN_OPTS:	m_sOptions = (char*)ppResult; break;
			default:							return false;
		}
		return true;
	}
};

//////////////////////////////////////////////////////////////////////////

class FactorPool_c
{
public:
	void			Prealloc ( int iElementSize, int nElements );
	BYTE *			Alloc ();
	void			Free ( BYTE * pPtr );
	int				GetElementSize() const;
	int				GetIntElementSize () const;
	void			AddToHash ( RowID_t tRowID, BYTE * pPacked );
	void			AddRef ( RowID_t tRowID );
	void			Release ( RowID_t tRowID );
	void			Flush ();

	bool			IsInitialized() const;
	SphFactorHash_t * GetHashPtr();

private:
	int				m_iElementSize = 0;

	CSphFixedVector<BYTE>	m_dPool { 0 };
	SphFactorHash_t			m_dHash { 0 };
	CSphFreeList			m_dFree;
	SphFactorHashEntry_t * Find ( RowID_t tRowID ) const;
	inline DWORD	HashFunc ( RowID_t tRowID ) const;
	bool			FlushEntry ( SphFactorHashEntry_t * pEntry );
};


void FactorPool_c::Prealloc ( int iElementSize, int nElements )
{
	m_iElementSize = iElementSize;

	m_dPool.Reset ( nElements*GetIntElementSize() );
	m_dHash.Reset ( nElements );
	m_dFree.Reset ( nElements );

	memset ( m_dHash.Begin(), 0, sizeof(m_dHash[0])*m_dHash.GetLength() );
}


BYTE * FactorPool_c::Alloc ()
{
	int iIndex = m_dFree.Get();
	assert ( iIndex>=0 && iIndex*GetIntElementSize()<m_dPool.GetLength() );
	return m_dPool.Begin() + iIndex * GetIntElementSize();
}


void FactorPool_c::Free ( BYTE * pPtr )
{
	if ( !pPtr )
		return;

	assert ( (pPtr-m_dPool.Begin() ) % GetIntElementSize()==0);
	assert ( pPtr>=m_dPool.Begin() && pPtr<&( m_dPool.Last() ) );

	int iIndex = int ( pPtr-m_dPool.Begin() ) / GetIntElementSize();
	m_dFree.Free ( iIndex );
}


int FactorPool_c::GetIntElementSize () const
{
	return m_iElementSize+sizeof(SphFactorHashEntry_t);
}


int	FactorPool_c::GetElementSize() const
{
	return m_iElementSize;
}


void FactorPool_c::AddToHash ( RowID_t tRowID, BYTE * pPacked )
{
	auto * pNew = (SphFactorHashEntry_t *)(pPacked+m_iElementSize);
	memset ( pNew, 0, sizeof(SphFactorHashEntry_t) );

	DWORD uKey = HashFunc ( tRowID );
	if ( m_dHash[uKey] )
	{
		SphFactorHashEntry_t * pStart = m_dHash[uKey];
		pNew->m_pPrev = nullptr;
		pNew->m_pNext = pStart;
		pStart->m_pPrev = pNew;
	}

	pNew->m_pData = pPacked;
	pNew->m_tRowID = tRowID;
	m_dHash[uKey] = pNew;
}


SphFactorHashEntry_t * FactorPool_c::Find ( RowID_t tRowID ) const
{
	DWORD uKey = HashFunc ( tRowID );
	if ( m_dHash[uKey] )
	{
		SphFactorHashEntry_t * pEntry = m_dHash[uKey];
		while ( pEntry )
		{
			if ( pEntry->m_tRowID==tRowID )
				return pEntry;

			pEntry = pEntry->m_pNext;
		}
	}

	return nullptr;
}


void FactorPool_c::AddRef ( RowID_t tRowID )
{
	if ( tRowID==INVALID_ROWID )
		return;

	SphFactorHashEntry_t * pEntry = Find ( tRowID );
	if ( pEntry )
		pEntry->m_iRefCount++;
}


void FactorPool_c::Release ( RowID_t tRowID )
{
	if ( tRowID==INVALID_ROWID )
		return;

	SphFactorHashEntry_t * pEntry = Find ( tRowID );
	if ( pEntry )
	{
		pEntry->m_iRefCount--;
		bool bHead = !pEntry->m_pPrev;
		SphFactorHashEntry_t * pNext = pEntry->m_pNext;
		if ( FlushEntry ( pEntry ) && bHead )
			m_dHash [ HashFunc ( tRowID ) ] = pNext;
	}
}


bool FactorPool_c::FlushEntry ( SphFactorHashEntry_t * pEntry )
{
	assert ( pEntry );
	assert ( pEntry->m_iRefCount>=0 );
	if ( pEntry->m_iRefCount )
		return false;

	if ( pEntry->m_pPrev )
		pEntry->m_pPrev->m_pNext = pEntry->m_pNext;

	if ( pEntry->m_pNext )
		pEntry->m_pNext->m_pPrev = pEntry->m_pPrev;

	Free ( pEntry->m_pData );

	return true;
}


void FactorPool_c::Flush()
{
	ARRAY_FOREACH ( i, m_dHash )
	{
		SphFactorHashEntry_t * pEntry = m_dHash[i];
		while ( pEntry )
		{
			SphFactorHashEntry_t * pNext = pEntry->m_pNext;
			bool bHead = !pEntry->m_pPrev;
			if ( FlushEntry(pEntry) && bHead )
				m_dHash[i] = pNext;

			pEntry = pNext;
		}
	}
}


inline DWORD FactorPool_c::HashFunc ( RowID_t tRowID ) const
{
	return (DWORD)( tRowID % m_dHash.GetLength() );
}


bool FactorPool_c::IsInitialized() const
{
	return !!m_iElementSize;
}


SphFactorHash_t * FactorPool_c::GetHashPtr ()
{
	return &m_dHash;
}


//////////////////////////////////////////////////////////////////////////
// EXPRESSION RANKER
//////////////////////////////////////////////////////////////////////////

/// lean hit
/// only stores keyword id and hit position
struct LeanHit_t
{
	WORD		m_uQuerypos;
	Hitpos_t	m_uHitpos;

	LeanHit_t & operator = ( const ExtHit_t & rhs )
	{
		m_uQuerypos = rhs.m_uQuerypos;
		m_uHitpos = rhs.m_uHitpos;
		return *this;
	}
};

/// ranker state that computes weight dynamically based on user supplied expression (formula)
template < bool NEED_PACKEDFACTORS = false, bool HANDLE_DUPES = false >
struct RankerState_Expr_fn : public ISphExtra
{
public:
	// per-field and per-document stuff
	BYTE				m_uLCS[SPH_MAX_FIELDS];
	BYTE				m_uCurLCS;
	DWORD				m_uCurPos;
	DWORD				m_uLcsTailPos;
	DWORD				m_uLcsTailQposMask;
	DWORD				m_uCurQposMask;
	int					m_iExpDelta;
	int					m_iLastHitPos;
	int					m_iFields = 0;
	const int *			m_pWeights = nullptr;
	DWORD				m_uDocBM25 = 0;
	CSphBitvec			m_tMatchedFields;
	int					m_iCurrentField = 0;
	DWORD				m_uHitCount[SPH_MAX_FIELDS];
	DWORD				m_uWordCount[SPH_MAX_FIELDS];
	CSphVector<float>	m_dIDF;
	float				m_dTFIDF[SPH_MAX_FIELDS];
	float				m_dMinIDF[SPH_MAX_FIELDS];
	float				m_dMaxIDF[SPH_MAX_FIELDS];
	float				m_dSumIDF[SPH_MAX_FIELDS];
	int					m_iMinHitPos[SPH_MAX_FIELDS];
	int					m_iMinBestSpanPos[SPH_MAX_FIELDS];
	CSphBitvec			m_tExactHit;
	CSphBitvec			m_tExactOrder;
	CSphBitvec			m_tKeywords;
	DWORD				m_uDocWordCount = 0;
	int					m_iMaxWindowHits[SPH_MAX_FIELDS];
	CSphVector<int>		m_dTF;			///< for bm25a
	float				m_fDocBM25A = 0.0f;	///< for bm25a
	CSphVector<int>		m_dFieldTF;		///< for bm25f, per-field layout (ie all field0 tfs, then all field1 tfs, etc)
	int					m_iMinGaps[SPH_MAX_FIELDS];		///< number of gaps in the minimum matching window

	const char *		m_sExpr = nullptr;
	CSphRefcountedPtr<ISphExpr>	m_pExpr;
	ESphAttr			m_eExprType { SPH_ATTR_NONE };
	const CSphSchema *	m_pSchema = nullptr;
	CSphAttrLocator		m_tFieldLensLoc;
	float				m_fAvgDocLen = 0.0f;
	const int64_t *		m_pFieldLens = nullptr;
	int64_t				m_iTotalDocuments = 0;
	float				m_fParamK1 = 1.2f;
	float				m_fParamB = 0.75f;
	int					m_iMaxQpos = 65535;		///< among all words, including dupes
	CSphVector<WORD>	m_dTermDupes;
	CSphVector<Hitpos_t>	m_dTermsHit;
	CSphBitvec			m_tHasMultiQpos;
	int					m_uLastSpanStart = 0;

	FactorPool_c 		m_tFactorPool;
	int					m_iPoolMatchCapacity = 0;

	// per-query stuff
	int					m_iMaxLCS = 0;
	int					m_iQueryWordCount = 0;

public:
	// internal state, and factor settings
	// max_window_hits(n)
	CSphVector<DWORD>	m_dWindow;
	int					m_iWindowSize = 1;

	// min_gaps
	int						m_iHaveMinWindow = 0;		///< whether to compute minimum matching window, and over how many query words
	int						m_iMinWindowWords = 0;		///< how many unique words have we seen so far
	CSphVector<LeanHit_t>	m_dMinWindowHits;			///< current minimum matching window candidate hits
	CSphVector<int>			m_dMinWindowCounts;			///< maps querypos indexes to number of occurrencess in m_dMinWindowHits

	// exact_order
	int					m_iLastField = 0;
	int					m_iLastQuerypos = 0;
	int					m_iExactOrderWords = 0;

	// LCCS and Weighted LCCS
	BYTE				m_dLCCS[SPH_MAX_FIELDS];
	float				m_dWLCCS[SPH_MAX_FIELDS];
	CSphVector<WORD>	m_dNextQueryPos;				///< words positions might have gaps due to stop-words
	WORD				m_iQueryPosLCCS = 0;
	int					m_iHitPosLCCS = 0;
	BYTE				m_iLenLCCS = 0;
	float				m_fWeightLCCS = 0.0f;

	// ATC
#define XRANK_ATC_WINDOW_LEN 10
#define XRANK_ATC_BUFFER_LEN 30
#define XRANK_ATC_DUP_DIV 0.25f
#define XRANK_ATC_EXP 1.75f
	struct AtcHit_t
	{
		int			m_iHitpos = 0;
		WORD		m_uQuerypos = 65535;
	};
	AtcHit_t			m_dAtcHits[XRANK_ATC_BUFFER_LEN];	///< ATC hits ring buffer
	int					m_iAtcHitStart = 0;					///< hits start at ring buffer
	int					m_iAtcHitCount = 0;					///< hits amount in buffer
	CSphVector<float>	m_dAtcTerms;						///< per-word ATC
	CSphBitvec			m_dAtcProcessedTerms;				///< temporary processed mask
	DWORD				m_uAtcField = 0;					///< currently processed field
	float				m_dAtc[SPH_MAX_FIELDS];				///< ATC per-field values
	bool				m_bAtcHeadProcessed = false;		///< flag for hits from buffer start to window start
	bool				m_bHaveAtc = false;					///< calculate ATC?
	bool				m_bWantAtc = false;

	void				UpdateATC ( bool bFlushField );
	float				TermTC ( int iTerm, bool bLeft );

public:

	bool				Init ( int iFields, const int * pWeights, ExtRanker_T<true> * pRanker, CSphString & sError, DWORD uFactorFlags );
	void				Update ( const ExtHit_t * pHlist );
	int					Finalize ( const CSphMatch & tMatch );
	bool				IsTermSkipped ( int iTerm );
    BYTE*               GetTextFeatures( const CSphMatch & tMatch );

public:
	/// setup per-keyword data needed to compute the factors
	/// (namely IDFs, counts, masks etc)
	/// WARNING, CALLED EVEN BEFORE INIT()!
	void SetQwords ( const ExtQwordsHash_t & hQwords )
	{
		m_dIDF.Resize ( m_iMaxQpos+1 ); // [MaxUniqQpos, MaxQpos] range will be all 0, but anyway
		m_dIDF.Fill ( 0.0f );

		m_dTF.Resize ( m_iMaxQpos+1 );
		m_dTF.Fill ( 0 );

		m_dMinWindowCounts.Resize ( m_iMaxQpos+1 );
		m_dMinWindowCounts.Fill ( 0 );

		m_iQueryWordCount = 0;
		m_tKeywords.Init ( m_iMaxQpos+1 ); // will not be tracking dupes
		bool bGotExpanded = false;
		CSphVector<WORD> dQueryPos;
		dQueryPos.Reserve ( m_iMaxQpos+1 );

		hQwords.IterateStart();
		while ( hQwords.IterateNext() )
		{
			// tricky bit
			// for query_word_count, we only want to count keywords that are not (!) excluded by the query
			// that is, in (aa NOT bb) case, we want a value of 1, not 2
			// there might be tail excluded terms these not affected MaxQpos
			ExtQword_t & dCur = hQwords.IterateGet();
			const int iQueryPos = dCur.m_iQueryPos;
			if ( dCur.m_bExcluded )
				continue;

			bool bQposUsed = m_tKeywords.BitGet ( iQueryPos );
			bGotExpanded |= bQposUsed;
			m_iQueryWordCount += ( bQposUsed ? 0 : 1 ); // count only one term at that position
			m_tKeywords.BitSet ( iQueryPos ); // just to assert at early stage!

			m_dIDF [ iQueryPos ] += dCur.m_fIDF;
			m_dTF [ iQueryPos ]++;
			if ( !bQposUsed )
				dQueryPos.Add ( (WORD)iQueryPos );
		}

		// FIXME!!! average IDF for expanded terms (aot morphology or dict=keywords)
		if ( bGotExpanded )
			ARRAY_FOREACH ( i, m_dTF )
			{
				if ( m_dTF[i]>1 )
					m_dIDF[i] /= m_dTF[i];
			}

		m_dTF.Fill ( 0 );

		// set next term position for current term in query (degenerates to +1 in the simplest case)
		dQueryPos.Sort();
		m_dNextQueryPos.Resize ( m_iMaxQpos+1 );
		m_dNextQueryPos.Fill ( (WORD)-1 ); // WORD_MAX filler
		for ( int i=0; i<dQueryPos.GetLength()-1; i++ )
		{
			WORD iCutPos = dQueryPos[i];
			WORD iNextPos = dQueryPos[i+1];
			m_dNextQueryPos[iCutPos] = iNextPos;
		}
	}

	void SetTermDupes ( const ExtQwordsHash_t & hQwords, int iMaxQpos, const ExtNode_i * pRoot )
	{
		if ( !pRoot )
			return;

		m_dTermsHit.Resize ( iMaxQpos + 1 );
		m_dTermsHit.Fill ( EMPTY_HIT );
		m_tHasMultiQpos.Init ( iMaxQpos+1 );
		m_dTermDupes.Resize ( iMaxQpos+1 );
		m_dTermDupes.Fill ( (WORD)-1 );

		CSphVector<TermPos_t> dTerms;
		dTerms.Reserve ( iMaxQpos );
		pRoot->GetTerms ( hQwords, dTerms );

		// reset excluded for all duplicates
		ARRAY_FOREACH ( i, dTerms )
		{
			WORD uAtomPos = dTerms[i].m_uAtomPos;
			WORD uQpos = dTerms[i].m_uQueryPos;
			if ( uAtomPos!=uQpos )
			{
				m_tHasMultiQpos.BitSet ( uAtomPos );
				m_tHasMultiQpos.BitSet ( uQpos );
			}

			m_tKeywords.BitSet ( uAtomPos );
			m_tKeywords.BitSet ( uQpos );
			m_dTermDupes[uAtomPos] = uQpos;

			// fill missed idf for dups
			if ( fabs ( m_dIDF[uAtomPos] )<=1e-6 )
				m_dIDF[uAtomPos] = m_dIDF[uQpos];
		}
	}

	/// finalize per-document factors that, well, need finalization
	void FinalizeDocFactors ( const CSphMatch & tMatch )
	{
		m_uDocBM25 = tMatch.m_iWeight;
		for ( int i=0; i<m_iFields; i++ )
		{
			m_uWordCount[i] = sphBitCount ( m_uWordCount[i] );
			if ( m_dMinIDF[i] > m_dMaxIDF[i] )
				m_dMinIDF[i] = m_dMaxIDF[i] = 0; // must be FLT_MAX vs -FLT_MAX, aka no hits
		}
		m_uDocWordCount = sphBitCount ( m_uDocWordCount );

		// compute real BM25
		// with blackjack, and hookers, and field lengths, and parameters
		//
		// canonical idf = log ( (N-n+0.5) / (n+0.5) )
		// sphinx idf = log ( (N-n+1) / n )
		// and we also downscale our idf by 1/log(N+1) to map it into [-0.5, 0.5] range

		// compute document length
		float dl = 0; // OPTIMIZE? could precompute and store total dl in attrs, but at a storage cost
		CSphAttrLocator tLoc = m_tFieldLensLoc;
		if ( tLoc.m_iBitOffset>=0 )
			for ( int i=0; i<m_iFields; i++ )
		{
			dl += tMatch.GetAttr ( tLoc );
			tLoc.m_iBitOffset += 32;
		}

		// compute BM25A (one value per document)
		m_fDocBM25A = 0.0f;
		for ( int iWord=1; iWord<=m_iMaxQpos; iWord++ )
		{
			if ( IsTermSkipped ( iWord ) )
				continue;

			auto tf = (float)m_dTF[iWord]; // OPTIMIZE? remove this vector, hook into m_uMatchHits somehow?
			float idf = m_dIDF[iWord];
			m_fDocBM25A += tf / (tf + m_fParamK1*(1 - m_fParamB + m_fParamB*dl/m_fAvgDocLen)) * idf;
		}
		m_fDocBM25A += 0.5f; // map to [0..1] range
	}

	/// reset per-document factors, prepare for the next document
	void ResetDocFactors()
	{
		// OPTIMIZE? quick full wipe? (using dwords/sse/whatever)
		m_uCurLCS = 0;
		if_const ( HANDLE_DUPES )
		{
			m_uCurPos = 0;
			m_uLcsTailPos = 0;
			m_uLcsTailQposMask = 0;
			m_uCurQposMask = 0;
			m_uLastSpanStart = 0;
		}
		m_iExpDelta = -1;
		m_iLastHitPos = -1;
		for ( int i=0; i<m_iFields; i++ )
		{
			m_uLCS[i] = 0;
			m_uHitCount[i] = 0;
			m_uWordCount[i] = 0;
			m_dMinIDF[i] = FLT_MAX;
			m_dMaxIDF[i] = -FLT_MAX;
			m_dSumIDF[i] = 0;
			m_dTFIDF[i] = 0;
			m_iMinHitPos[i] = 0;
			m_iMinBestSpanPos[i] = 0;
			m_iMaxWindowHits[i] = 0;
			m_iMinGaps[i] = 0;
			m_dAtc[i] = 0.0f;
		}
		m_dTF.Fill ( 0 );
		m_dFieldTF.Fill ( 0 ); // OPTIMIZE? make conditional?
		m_tMatchedFields.Clear();
		m_tExactHit.Clear();
		m_tExactOrder.Clear();
		m_uDocWordCount = 0;
		m_dWindow.Resize ( 0 );
		m_fDocBM25A = 0;
		m_dMinWindowHits.Resize ( 0 );
		m_dMinWindowCounts.Fill ( 0 );
		m_iMinWindowWords = 0;
		m_iLastField = -1;
		m_iLastQuerypos = 0;
		m_iExactOrderWords = 0;

		m_dAtcTerms.Fill ( 0.0f );
		m_iAtcHitStart = 0;
		m_iAtcHitCount = 0;
		m_uAtcField = 0;

		if_const ( HANDLE_DUPES )
			m_dTermsHit.Fill ( EMPTY_HIT );
	}

	void FlushMatches ()
	{
		m_tFactorPool.Flush ();
	}

protected:
	inline void UpdateGap ( int iField, int iWords, int iGap )
	{
		if ( m_iMinWindowWords<iWords || ( m_iMinWindowWords==iWords && m_iMinGaps[iField]>iGap ) )
		{
			m_iMinGaps[iField] = iGap;
			m_iMinWindowWords = iWords;
		}
	}

	void			UpdateMinGaps ( const ExtHit_t * pHlist );
	void			UpdateFreq ( WORD uQpos, DWORD uField );

private:
	bool			ExtraDataImpl ( ExtraData_e eType, void ** ppResult ) override;
	int				GetMaxPackedLength();
	BYTE *			PackFactors();
};

/// extra expression ranker node types
enum ExprRankerNode_e
{
	// field level factors
	XRANK_LCS,
	XRANK_USER_WEIGHT,
	XRANK_HIT_COUNT,
	XRANK_WORD_COUNT,
	XRANK_TF_IDF,
	XRANK_MIN_IDF,
	XRANK_MAX_IDF,
	XRANK_SUM_IDF,
	XRANK_MIN_HIT_POS,
	XRANK_MIN_BEST_SPAN_POS,
	XRANK_EXACT_HIT,
	XRANK_EXACT_ORDER,
	XRANK_MAX_WINDOW_HITS,
	XRANK_MIN_GAPS,
	XRANK_LCCS,
	XRANK_WLCCS,
	XRANK_ATC,

	// document level factors
	XRANK_BM25,
	XRANK_MAX_LCS,
	XRANK_FIELD_MASK,
	XRANK_QUERY_WORD_COUNT,
	XRANK_DOC_WORD_COUNT,
	XRANK_BM25A,
	XRANK_BM25F,

	// field aggregation functions
	XRANK_SUM,
	XRANK_TOP
};


/// generic field factor
template < typename T >
struct Expr_FieldFactor_c : public Expr_NoLocator_c
{
	const int *		m_pIndex;
	const T *		m_pData;

	Expr_FieldFactor_c ( const int * pIndex, const T * pData )
		: m_pIndex ( pIndex )
		, m_pData ( pData )
	{}

	float Eval ( const CSphMatch & ) const final
	{
		return (float) m_pData [ *m_pIndex ];
	}

	int IntEval ( const CSphMatch & ) const final
	{
		return (int) m_pData [ *m_pIndex ];
	}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & ) final
	{
		assert ( 0 && "ranker expressions in filters" );
		return 0;
	}

	ISphExpr* Clone () const final
	{
		assert ( 0 && "cloning ranker expressions is not expected now" );
		return nullptr; //new Expr_FieldFactor_c ( *this );
	}

private:
	Expr_FieldFactor_c ( const Expr_FieldFactor_c& rhs )
		: m_pIndex ( rhs.m_pIndex )
		, m_pData ( rhs.m_pData )
	{}
};


/// bitmask field factor specialization
template<>
struct Expr_FieldFactor_c<bool> : public Expr_NoLocator_c
{
	const int *		m_pIndex;
	const DWORD *	m_pData;

	Expr_FieldFactor_c ( const int * pIndex, const DWORD * pData )
		: m_pIndex ( pIndex )
		, m_pData ( pData )
	{}

	float Eval ( const CSphMatch & ) const final
	{
		return (float)( (*m_pData) >> (*m_pIndex) );
	}

	int IntEval ( const CSphMatch & ) const final
	{
		return (int)( (*m_pData) >> (*m_pIndex) );
	}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & ) final
	{
		assert ( 0 && "ranker expressions in filters" );
		return 0;
	}
};


/// generic per-document int factor
struct Expr_IntPtr_c : public Expr_NoLocator_c
{
	DWORD * m_pVal;

	explicit Expr_IntPtr_c ( DWORD * pVal )
		: m_pVal ( pVal )
	{}

	float Eval ( const CSphMatch & ) const final
	{
		return (float)*m_pVal;
	}

	int IntEval ( const CSphMatch & ) const final
	{
		return (int)*m_pVal;
	}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & ) final
	{
		assert ( 0 && "ranker expressions in filters" );
		return 0;
	}

	ISphExpr * Clone () const final
	{
		assert ( 0 && "cloning ranker expressions is not expected now" );
		return nullptr; //new Expr_IntPtr_c ( *this );
	}

private:
	Expr_IntPtr_c ( const Expr_IntPtr_c& rhs ) : m_pVal ( rhs.m_pVal ) {}
};


/// per-document field mask factor
struct Expr_FieldMask_c : public Expr_NoLocator_c
{
	const CSphBitvec & m_tFieldMask;

	explicit Expr_FieldMask_c ( const CSphBitvec & tFieldMask )
		: m_tFieldMask ( tFieldMask )
	{}

	float Eval ( const CSphMatch & ) const final
	{
		return (float)*m_tFieldMask.Begin();
	}

	int IntEval ( const CSphMatch & ) const final
	{
		return (int)*m_tFieldMask.Begin();
	}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & ) final
	{
		assert ( 0 && "ranker expressions in filters" );
		return 0;
	}

	ISphExpr * Clone () const final
	{
		assert ( 0 && "cloning ranker expressions is not expected now" );
		return nullptr; //new Expr_FieldMask_c ( *this );
	}

private:
	Expr_FieldMask_c ( const Expr_FieldMask_c& rhs ) : m_tFieldMask ( rhs.m_tFieldMask ) {}
};


/// bitvec field factor specialization
template<>
struct Expr_FieldFactor_c<CSphBitvec> : public Expr_NoLocator_c
{
	const int *		m_pIndex;
	const CSphBitvec & m_tField;

	Expr_FieldFactor_c ( const int * pIndex, const CSphBitvec & tField )
		: m_pIndex ( pIndex )
		, m_tField ( tField )
	{}

	float Eval ( const CSphMatch & ) const final
	{
		return (float)( m_tField.BitGet ( *m_pIndex ) );
	}

	int IntEval ( const CSphMatch & ) const final
	{
		return (int)( m_tField.BitGet ( *m_pIndex ) );
	}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & ) final
	{
		assert ( 0 && "ranker expressions in filters" );
		return 0;
	}

	ISphExpr * Clone () const final
	{
		assert ( 0 && "cloning ranker expressions is not expected now" );
		return nullptr; // new Expr_FieldFactor_c<CSphBitvec> ( *this );
	}

private:
	Expr_FieldFactor_c<CSphBitvec> ( const Expr_FieldFactor_c<CSphBitvec>& rhs )
	        : m_pIndex ( rhs.m_pIndex ), m_tField ( rhs.m_tField ) {}
};


/// generic per-document float factor
struct Expr_FloatPtr_c : public Expr_NoLocator_c
{
	float * m_pVal;

	explicit Expr_FloatPtr_c ( float * pVal )
		: m_pVal ( pVal )
	{}

	float Eval ( const CSphMatch & ) const final
	{
		return (float)*m_pVal;
	}

	int IntEval ( const CSphMatch & ) const final
	{
		return (int)*m_pVal;
	}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & ) final
	{
		assert ( 0 && "ranker expressions in filters" );
		return 0;
	}

	ISphExpr * Clone () const final
	{
		assert ( 0 && "cloning ranker expressions is not expected now" );
		return nullptr; // new Expr_FloatPtr_c ( *this );
	}

private:
	Expr_FloatPtr_c ( const Expr_FloatPtr_c& rhs )
		: m_pVal ( rhs.m_pVal )
	{}
};

template < bool NEED_PACKEDFACTORS, bool HANDLE_DUPES >
struct Expr_BM25F_T : public Expr_NoLocator_c
{
	RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES> * m_pState;
	float					m_fK1;
	float					m_fB;
	float					m_fWeightedAvgDocLen;
	CSphVector<int>			m_dWeights;		///< per field weights

	explicit Expr_BM25F_T ( RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES> * pState, float k1, float b, ISphExpr * pFieldWeights )
	{
		// bind k1, b
		m_pState = pState;
		m_fK1 = k1;
		m_fB = b;

		// bind weights
		m_dWeights.Resize ( pState->m_iFields );
		m_dWeights.Fill ( 1 );
		if ( pFieldWeights )
		{
			auto pMapArg = ( Expr_MapArg_c * ) pFieldWeights;

			VecTraits_T<CSphNamedVariant> dOpts ( pMapArg->m_pValues, pMapArg->m_iCount );
			for ( const auto& dOpt : dOpts )
			{
				// FIXME? report errors if field was not found?
				if ( !dOpt.m_sValue.IsEmpty() )
					continue; // weights must be int, not string
				const CSphString & sField = dOpt.m_sKey;
				int iField = pState->m_pSchema->GetFieldIndex ( sField.cstr() );
				if ( iField>=0 )
					m_dWeights[iField] = dOpt.m_iValue;
			}
		}

		// compute weighted avgdl
		m_fWeightedAvgDocLen = 0;
		if ( m_pState->m_pFieldLens )
			ARRAY_FOREACH ( i, m_dWeights )
				m_fWeightedAvgDocLen += m_pState->m_pFieldLens[i] * m_dWeights[i];
		else
			m_fWeightedAvgDocLen = 1.0f;
		m_fWeightedAvgDocLen /= m_pState->m_iTotalDocuments;
	}

	float Eval ( const CSphMatch & tMatch ) const final
	{
		// compute document length
		// OPTIMIZE? could precompute and store total dl in attrs, but at a storage cost
		// OPTIMIZE? could at least share between multiple BM25F instances, if there are many
		float dl = 0;
		CSphAttrLocator tLoc = m_pState->m_tFieldLensLoc;
		if ( tLoc.m_iBitOffset>=0 )
			for ( int i=0; i<m_pState->m_iFields; i++ )
		{
			dl += tMatch.GetAttr ( tLoc ) * m_dWeights[i];
			tLoc.m_iBitOffset += 32;
		}

		// compute (the current instance of) BM25F
		float fRes = 0.0f;
		for ( int iWord=1; iWord<=m_pState->m_iMaxQpos; iWord++ )
		{
			if ( m_pState->IsTermSkipped ( iWord ) )
				continue;

			// compute weighted TF
			float tf = 0.0f;
			for ( int i=0; i<m_pState->m_iFields; i++ )
				tf += m_pState->m_dFieldTF [ iWord + i*(1+m_pState->m_iMaxQpos) ] * m_dWeights[i];
			float idf = m_pState->m_dIDF[iWord]; // FIXME? zeroed out for dupes!
			fRes += tf / (tf + m_fK1*(1.0f - m_fB + m_fB*dl/m_fWeightedAvgDocLen)) * idf;
		}
		return fRes + 0.5f; // map to [0..1] range
	}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & ) final
	{
		assert ( 0 && "ranker expressions in filters" );
		return 0;
	}

	ISphExpr * Clone () const final
	{
		assert ( 0 && "cloning ranker expressions is not expected now" );
		return nullptr; // new Expr_BM25F_T ( *this );
	}

private:
	Expr_BM25F_T ( const Expr_BM25F_T& rhs )
		: m_pState ( rhs.m_pState )
		, m_fK1 ( rhs.m_fK1 )
		, m_fB ( rhs.m_fB )
		, m_fWeightedAvgDocLen ( rhs.m_fWeightedAvgDocLen )
		, m_dWeights ( rhs.m_dWeights )
	{}
};


/// function that sums sub-expressions over matched fields
template < bool NEED_PACKEDFACTORS, bool HANDLE_DUPES >
struct Expr_Sum_T : public ISphExpr
{
	RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES> * m_pState;
	CSphRefcountedPtr<ISphExpr>				m_pArg;

	Expr_Sum_T ( RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES> * pState, ISphExpr * pArg )
		: m_pState ( pState )
		, m_pArg ( pArg )
	{
		SafeAddRef ( pArg );
	}

	float Eval ( const CSphMatch & tMatch ) const final
	{
		m_pState->m_iCurrentField = 0;
		float fRes = 0;
		const CSphBitvec & tFields = m_pState->m_tMatchedFields;
		int iBits = tFields.BitCount();
		while ( iBits )
		{
			if ( tFields.BitGet ( m_pState->m_iCurrentField ) )
			{
				fRes += m_pArg->Eval ( tMatch );
				iBits--;
			}
			m_pState->m_iCurrentField++;
		}
		return fRes;
	}

	int IntEval ( const CSphMatch & tMatch ) const final
	{
		m_pState->m_iCurrentField = 0;
		int iRes = 0;
		const CSphBitvec & tFields = m_pState->m_tMatchedFields;
		int iBits = tFields.BitCount();
		while ( iBits )
		{
			if ( tFields.BitGet ( m_pState->m_iCurrentField ) )
			{
				iRes += m_pArg->IntEval ( tMatch );
				iBits--;
			}
			m_pState->m_iCurrentField++;
		}
		return iRes;
	}

	void FixupLocator ( const ISphSchema * /*pOldSchema*/, const ISphSchema * /*pNewSchema*/ ) final
	{
		assert ( 0 && "ranker expressions in filters" );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		assert ( m_pArg );
		m_pArg->Command ( eCmd, pArg );
	}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & ) final
	{
		assert ( 0 && "ranker expressions in filters" );
		return 0;
	}

	ISphExpr * Clone () const final
	{
		assert ( 0 && "cloning ranker expressions is not expected now" );
		return nullptr; // new Expr_Sum_T ( *this );
	}

private:
	Expr_Sum_T ( const Expr_Sum_T& rhs )
		: m_pState ( rhs.m_pState ) // fixme!
		, m_pArg ( SafeClone (rhs.m_pArg ) ) {}
};


/// aggregate max over matched fields
template < bool NEED_PACKEDFACTORS, bool HANDLE_DUPES >
struct Expr_Top_T : public ISphExpr
{
	RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES> * m_pState;
	CSphRefcountedPtr<ISphExpr>		m_pArg;

	Expr_Top_T ( RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES> * pState, ISphExpr * pArg )
		: m_pState ( pState )
		, m_pArg ( pArg )
	{
		SafeAddRef ( pArg );
	}

	float Eval ( const CSphMatch & tMatch ) const final
	{
		m_pState->m_iCurrentField = 0;
		float fRes = FLT_MIN;
		const CSphBitvec & tFields = m_pState->m_tMatchedFields;
		int iBits = tFields.BitCount();
		while ( iBits )
		{
			if ( tFields.BitGet ( m_pState->m_iCurrentField ) )
			{
				fRes = Max ( fRes, m_pArg->Eval ( tMatch ) );
				iBits--;
			}
			m_pState->m_iCurrentField++;
		}
		return fRes;
	}

	int IntEval ( const CSphMatch & tMatch ) const final
	{
		m_pState->m_iCurrentField = 0;
		int iRes = INT_MIN;
		const CSphBitvec & tFields = m_pState->m_tMatchedFields;
		int iBits = tFields.BitCount();
		while ( iBits )
		{
			if ( tFields.BitGet ( m_pState->m_iCurrentField ) )
			{
				iRes = Max ( iRes, m_pArg->IntEval ( tMatch ) );
				iBits--;
			}
			m_pState->m_iCurrentField++;
		}
		return iRes;
	}

	void FixupLocator ( const ISphSchema * /*pOldSchema*/, const ISphSchema * /*pNewSchema*/ ) final
	{
		assert ( 0 && "ranker expressions in filters" );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		assert ( m_pArg );
		m_pArg->Command ( eCmd, pArg );
	}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & ) final
	{
		assert ( 0 && "ranker expressions in filters" );
		return 0;
	}

	ISphExpr * Clone () const final
	{
		assert ( 0 && "cloning ranker expressions is not expected now" );
		return nullptr; // new Expr_Top_T ( *this );
	}

private:
	Expr_Top_T ( const Expr_Top_T& rhs )
		: m_pState ( rhs.m_pState ) // fixme!
		, m_pArg ( SafeClone ( rhs.m_pArg ) ) {}
};


// FIXME! cut/pasted from sphinxexpr; remove dupe
struct Expr_GetIntConst_Rank_c : public Expr_NoLocator_c
{
	int m_iValue;
	explicit Expr_GetIntConst_Rank_c ( int iValue ) : m_iValue ( iValue ) {}
	float Eval ( const CSphMatch & ) const final { return (float) m_iValue; } // no assert() here cause generic float Eval() needs to work even on int-evaluator tree
	int IntEval ( const CSphMatch & ) const final { return m_iValue; }
	int64_t Int64Eval ( const CSphMatch & ) const final { return m_iValue; }

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & ) final
	{
		assert ( 0 && "ranker expressions in filters" );
		return 0;
	}

	ISphExpr * Clone () const final
	{
		assert ( 0 && "cloning ranker expressions is not expected now" );
		return nullptr; // new Expr_GetIntConst_Rank_c ( *this );
	}

private:
	Expr_GetIntConst_Rank_c ( const Expr_GetIntConst_Rank_c& rhs ) : m_iValue ( rhs.m_iValue ) {}
};


/// hook that exposes field-level factors, document-level factors, and matched field SUM() function to generic expressions
template < bool NEED_PACKEDFACTORS, bool HANDLE_DUPES >
class ExprRankerHook_T : public ISphExprHook
{
public:
	RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES> * m_pState;
	const char *			m_sCheckError = nullptr;
	bool					m_bCheckInFieldAggr = false;

public:
	explicit ExprRankerHook_T ( RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES> * pState )
		: m_pState ( pState )
	{}

	int IsKnownIdent ( const char * sIdent ) const final
	{
		// OPTIMIZE? hash this some nice long winter night?
		if ( !strcasecmp ( sIdent, "lcs" ) )
			return XRANK_LCS;
		if ( !strcasecmp ( sIdent, "user_weight" ) )
			return XRANK_USER_WEIGHT;
		if ( !strcasecmp ( sIdent, "hit_count" ) )
			return XRANK_HIT_COUNT;
		if ( !strcasecmp ( sIdent, "word_count" ) )
			return XRANK_WORD_COUNT;
		if ( !strcasecmp ( sIdent, "tf_idf" ) )
			return XRANK_TF_IDF;
		if ( !strcasecmp ( sIdent, "min_idf" ) )
			return XRANK_MIN_IDF;
		if ( !strcasecmp ( sIdent, "max_idf" ) )
			return XRANK_MAX_IDF;
		if ( !strcasecmp ( sIdent, "sum_idf" ) )
			return XRANK_SUM_IDF;
		if ( !strcasecmp ( sIdent, "min_hit_pos" ) )
			return XRANK_MIN_HIT_POS;
		if ( !strcasecmp ( sIdent, "min_best_span_pos" ) )
			return XRANK_MIN_BEST_SPAN_POS;
		if ( !strcasecmp ( sIdent, "exact_hit" ) )
			return XRANK_EXACT_HIT;
		if ( !strcasecmp ( sIdent, "exact_order" ) )
			return XRANK_EXACT_ORDER;

		if ( !strcasecmp ( sIdent, "bm25" ) )
			return XRANK_BM25;
		if ( !strcasecmp ( sIdent, "max_lcs" ) )
			return XRANK_MAX_LCS;
		if ( !strcasecmp ( sIdent, "field_mask" ) )
			return XRANK_FIELD_MASK;
		if ( !strcasecmp ( sIdent, "query_word_count" ) )
			return XRANK_QUERY_WORD_COUNT;
		if ( !strcasecmp ( sIdent, "doc_word_count" ) )
			return XRANK_DOC_WORD_COUNT;

		if ( !strcasecmp ( sIdent, "min_gaps" ) )
			return XRANK_MIN_GAPS;

		if ( !strcasecmp ( sIdent, "lccs" ) )
			return XRANK_LCCS;
		if ( !strcasecmp ( sIdent, "wlccs" ) )
			return XRANK_WLCCS;
		if ( !strcasecmp ( sIdent, "atc" ) )
			return XRANK_ATC;

		return -1;
	}

	int IsKnownFunc ( const char * sFunc ) const final
	{
		if ( !strcasecmp ( sFunc, "sum" ) )
			return XRANK_SUM;
		if ( !strcasecmp ( sFunc, "top" ) )
			return XRANK_TOP;
		if ( !strcasecmp ( sFunc, "max_window_hits" ) )
			return XRANK_MAX_WINDOW_HITS;
		if ( !strcasecmp ( sFunc, "bm25a" ) )
			return XRANK_BM25A;
		if ( !strcasecmp ( sFunc, "bm25f" ) )
			return XRANK_BM25F;
		return -1;
	}

	ISphExpr * CreateNode ( int iID, ISphExpr * _pLeft, const ISphSchema *, ESphEvalStage *, bool *, CSphString & ) final
	{
		SafeAddRef ( _pLeft );
		CSphRefcountedPtr<ISphExpr> pLeft ( _pLeft );

		int * pCF = &m_pState->m_iCurrentField; // just a shortcut
		switch ( iID )
		{
			case XRANK_LCS:					return new Expr_FieldFactor_c<BYTE> ( pCF, m_pState->m_uLCS );
			case XRANK_USER_WEIGHT:			return new Expr_FieldFactor_c<int> ( pCF, m_pState->m_pWeights );
			case XRANK_HIT_COUNT:			return new Expr_FieldFactor_c<DWORD> ( pCF, m_pState->m_uHitCount );
			case XRANK_WORD_COUNT:			return new Expr_FieldFactor_c<DWORD> ( pCF, m_pState->m_uWordCount );
			case XRANK_TF_IDF:				return new Expr_FieldFactor_c<float> ( pCF, m_pState->m_dTFIDF );
			case XRANK_MIN_IDF:				return new Expr_FieldFactor_c<float> ( pCF, m_pState->m_dMinIDF );
			case XRANK_MAX_IDF:				return new Expr_FieldFactor_c<float> ( pCF, m_pState->m_dMaxIDF );
			case XRANK_SUM_IDF:				return new Expr_FieldFactor_c<float> ( pCF, m_pState->m_dSumIDF );
			case XRANK_MIN_HIT_POS:			return new Expr_FieldFactor_c<int> ( pCF, m_pState->m_iMinHitPos );
			case XRANK_MIN_BEST_SPAN_POS:	return new Expr_FieldFactor_c<int> ( pCF, m_pState->m_iMinBestSpanPos );
			case XRANK_EXACT_HIT:			return new Expr_FieldFactor_c<CSphBitvec> ( pCF, m_pState->m_tExactHit );
			case XRANK_EXACT_ORDER:			return new Expr_FieldFactor_c<CSphBitvec> ( pCF, m_pState->m_tExactOrder );
			case XRANK_MAX_WINDOW_HITS:
				{
					CSphMatch tDummy;
					m_pState->m_iWindowSize = pLeft->IntEval ( tDummy ); // must be constant; checked in GetReturnType()
					return new Expr_FieldFactor_c<int> ( pCF, m_pState->m_iMaxWindowHits );
				}
			case XRANK_MIN_GAPS:			return new Expr_FieldFactor_c<int> ( pCF, m_pState->m_iMinGaps );
			case XRANK_LCCS:				return new Expr_FieldFactor_c<BYTE> ( pCF, m_pState->m_dLCCS );
			case XRANK_WLCCS:				return new Expr_FieldFactor_c<float> ( pCF, m_pState->m_dWLCCS );
			case XRANK_ATC:
				m_pState->m_bWantAtc = true;
				return new Expr_FieldFactor_c<float> ( pCF, m_pState->m_dAtc );

			case XRANK_BM25:				return new Expr_IntPtr_c ( &m_pState->m_uDocBM25 );
			case XRANK_MAX_LCS:				return new Expr_GetIntConst_Rank_c ( m_pState->m_iMaxLCS );
			case XRANK_FIELD_MASK:			return new Expr_FieldMask_c ( m_pState->m_tMatchedFields );
			case XRANK_QUERY_WORD_COUNT:	return new Expr_GetIntConst_Rank_c ( m_pState->m_iQueryWordCount );
			case XRANK_DOC_WORD_COUNT:		return new Expr_IntPtr_c ( &m_pState->m_uDocWordCount );
			case XRANK_BM25A:
				{
					// exprs we'll evaluate here must be constant; that is checked in GetReturnType()
					// so having a dummy match with no data work alright
					assert ( pLeft->IsArglist() );
					CSphMatch tDummy;
					m_pState->m_fParamK1 = pLeft->GetArg(0)->Eval ( tDummy );
					m_pState->m_fParamB = pLeft->GetArg(1)->Eval ( tDummy );
					m_pState->m_fParamK1 = Max ( m_pState->m_fParamK1, 0.001f );
					m_pState->m_fParamB = Min ( Max ( m_pState->m_fParamB, 0.0f ), 1.0f );
					return new Expr_FloatPtr_c ( &m_pState->m_fDocBM25A );
				}
			case XRANK_BM25F:
				{
					assert ( pLeft->IsArglist() );
					CSphMatch tDummy;
					float fK1 = pLeft->GetArg(0)->Eval ( tDummy );
					float fB = pLeft->GetArg(1)->Eval ( tDummy );
					fK1 = Max ( fK1, 0.001f );
					fB = Min ( Max ( fB, 0.0f ), 1.0f );
					return new Expr_BM25F_T<NEED_PACKEDFACTORS, HANDLE_DUPES> ( m_pState, fK1, fB, pLeft->GetArg(2) );
				}

			case XRANK_SUM:					return new Expr_Sum_T<NEED_PACKEDFACTORS, HANDLE_DUPES> ( m_pState, pLeft );
			case XRANK_TOP:					return new Expr_Top_T<NEED_PACKEDFACTORS, HANDLE_DUPES> ( m_pState, pLeft );
			default:						return nullptr;
		}
	}

	ESphAttr GetIdentType ( int iID ) const final
	{
		switch ( iID )
		{
			case XRANK_LCS: // field-level
			case XRANK_USER_WEIGHT:
			case XRANK_HIT_COUNT:
			case XRANK_WORD_COUNT:
			case XRANK_MIN_HIT_POS:
			case XRANK_MIN_BEST_SPAN_POS:
			case XRANK_EXACT_HIT:
			case XRANK_EXACT_ORDER:
			case XRANK_MAX_WINDOW_HITS:
			case XRANK_BM25: // doc-level
			case XRANK_MAX_LCS:
			case XRANK_FIELD_MASK:
			case XRANK_QUERY_WORD_COUNT:
			case XRANK_DOC_WORD_COUNT:
			case XRANK_MIN_GAPS:
			case XRANK_LCCS:
				return SPH_ATTR_INTEGER;
			case XRANK_TF_IDF:
			case XRANK_MIN_IDF:
			case XRANK_MAX_IDF:
			case XRANK_SUM_IDF:
			case XRANK_WLCCS:
			case XRANK_ATC:
				return SPH_ATTR_FLOAT;
			default:
				assert ( 0 );
				return SPH_ATTR_INTEGER;
		}
	}

	/// helper to check argument types by a signature string (passed in sArgs)
	/// every character in the signature describes a type
	/// ? = any type
	/// i = integer
	/// I = integer constant
	/// f = float
	/// s = scalar (int/float)
	/// h = hash
	/// signature can also be preceded by "c:" modifier than means that all arguments must be constant
	bool CheckArgtypes ( const CSphVector<ESphAttr> & dArgs, const char * sFuncname, const char * sArgs, bool bAllConst, CSphString & sError )  const
	{
		if ( sArgs[0]=='c' && sArgs[1]==':' )
		{
			if ( !bAllConst )
			{
				sError.SetSprintf ( "%s() requires constant arguments", sFuncname );
				return false;
			}
			sArgs += 2;
		}

		auto iLen = (int)strlen ( sArgs );
		if ( dArgs.GetLength()!=iLen )
		{
			sError.SetSprintf ( "%s() requires %d argument(s), not %d", sFuncname, iLen, dArgs.GetLength() );
			return false;
		}

		ARRAY_FOREACH ( i, dArgs )
		{
			switch ( *sArgs++ )
			{
				case '?':
					break;
				case 'i':
					if ( dArgs[i]!=SPH_ATTR_INTEGER )
					{
						sError.SetSprintf ( "argument %d to %s() must be integer", i, sFuncname );
						return false;
					}
					break;
				case 's':
					if ( dArgs[i]!=SPH_ATTR_INTEGER && dArgs[i]!=SPH_ATTR_FLOAT )
					{
						sError.SetSprintf ( "argument %d to %s() must be scalar (integer or float)", i, sFuncname );
						return false;
					}
					break;
				case 'h':
					if ( dArgs[i]!=SPH_ATTR_MAPARG )
					{
						sError.SetSprintf ( "argument %d to %s() must be a map of constants", i, sFuncname );
						return false;
					}
					break;
				default:
					assert ( 0 && "unknown signature code" );
					break;
			}
		}

		// this is important!
		// other previous failed checks might have filled sError
		// and if anything else up the stack checks it, we need an empty message now
		sError = "";
		return true;
	}

	ESphAttr GetReturnType ( int iID, const CSphVector<ESphAttr> & dArgs, bool bAllConst, CSphString & sError ) const final
	{
		switch ( iID )
		{
			case XRANK_SUM:
				if ( !CheckArgtypes ( dArgs, "SUM", "?", bAllConst, sError ) )
					return SPH_ATTR_NONE;
				return dArgs[0];

			case XRANK_TOP:
				if ( !CheckArgtypes ( dArgs, "TOP", "?", bAllConst, sError ) )
					return SPH_ATTR_NONE;
				return dArgs[0];

			case XRANK_MAX_WINDOW_HITS:
				if ( !CheckArgtypes ( dArgs, "MAX_WINDOW_HITS", "c:i", bAllConst, sError ) )
					return SPH_ATTR_NONE;
				return SPH_ATTR_INTEGER;

			case XRANK_BM25A:
				if ( !CheckArgtypes ( dArgs, "BM25A", "c:ss", bAllConst, sError ) )
					return SPH_ATTR_NONE;
				return SPH_ATTR_FLOAT;

			case XRANK_BM25F:
				if ( !CheckArgtypes ( dArgs, "BM25F", "c:ss", bAllConst, sError ) )
					if ( !CheckArgtypes ( dArgs, "BM25F", "c:ssh", bAllConst, sError ) )
						return SPH_ATTR_NONE;
				return SPH_ATTR_FLOAT;

			default:
				sError.SetSprintf ( "internal error: unknown hook function (id=%d)", iID );
		}
		return SPH_ATTR_NONE;
	}

	void CheckEnter ( int iID ) final
	{
		if ( !m_sCheckError )
			switch ( iID )
		{
			case XRANK_LCS:
			case XRANK_USER_WEIGHT:
			case XRANK_HIT_COUNT:
			case XRANK_WORD_COUNT:
			case XRANK_TF_IDF:
			case XRANK_MIN_IDF:
			case XRANK_MAX_IDF:
			case XRANK_SUM_IDF:
			case XRANK_MIN_HIT_POS:
			case XRANK_MIN_BEST_SPAN_POS:
			case XRANK_EXACT_HIT:
			case XRANK_MAX_WINDOW_HITS:
			case XRANK_LCCS:
			case XRANK_WLCCS:
				if ( !m_bCheckInFieldAggr )
					m_sCheckError = "field factors must only occur within field aggregation functions in ranking expression";
				break;

			case XRANK_SUM:
			case XRANK_TOP:
				if ( m_bCheckInFieldAggr )
					m_sCheckError = "field aggregates can not be nested in ranking expression";
				else
					m_bCheckInFieldAggr = true;
				break;

			default:
				assert ( iID>=0 );
				return;
		}
	}

	void CheckExit ( int iID ) final
	{
		if ( !m_sCheckError && ( iID==XRANK_SUM || iID==XRANK_TOP ) )
		{
			assert ( m_bCheckInFieldAggr );
			m_bCheckInFieldAggr = false;
		}
	}
};

/// initialize ranker state
template < bool NEED_PACKEDFACTORS, bool HANDLE_DUPES >
bool RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES>::Init ( int iFields, const int * pWeights, ExtRanker_T<true> * pRanker, CSphString & sError,	DWORD uFactorFlags )
{
	m_iFields = iFields;
	m_pWeights = pWeights;
	m_uDocBM25 = 0;
	m_tMatchedFields.Init ( iFields );
	m_tExactHit.Init ( iFields );
	m_tExactOrder.Init ( iFields );
	m_iCurrentField = 0;
	m_iMaxQpos = pRanker->m_iMaxQpos; // already copied in SetQwords, but anyway
	m_iWindowSize = 1;
	m_iHaveMinWindow = 0;
	m_dMinWindowHits.Reserve ( Max ( m_iMaxQpos, 32 ) );
	memset ( m_dLCCS, 0 , sizeof(m_dLCCS) );
	memset ( m_dWLCCS, 0, sizeof(m_dWLCCS) );
	m_iQueryPosLCCS = 0;
	m_iHitPosLCCS = 0;
	m_iLenLCCS = 0;
	m_fWeightLCCS = 0.0f;
	m_dAtcTerms.Resize ( m_iMaxQpos + 1 );
	m_dAtcProcessedTerms.Init ( m_iMaxQpos + 1 );
	m_bAtcHeadProcessed = false;
	ResetDocFactors();

	// compute query level constants
	// max_lcs, aka m_iMaxLCS (for matchany ranker emulation) gets computed here
	// query_word_count, aka m_iQueryWordCount is set elsewhere (in SetQwordsIDF())
	m_iMaxLCS = 0;
	for ( int i=0; i<iFields; i++ )
		m_iMaxLCS += pWeights[i] * pRanker->m_iQwords;

	for ( int i=0; i<m_pSchema->GetAttrsCount(); i++ )
	{
		if ( m_pSchema->GetAttr(i).m_eAttrType!=SPH_ATTR_TOKENCOUNT )
			continue;
		m_tFieldLensLoc = m_pSchema->GetAttr(i).m_tLocator;
		break;
	}

	m_fAvgDocLen = 0.0f;
	m_pFieldLens = pRanker->GetIndex()->GetFieldLens();
	if ( m_pFieldLens )
		for ( int i=0; i<iFields; i++ )
			m_fAvgDocLen += m_pFieldLens[i];
	else
		m_fAvgDocLen = 1.0f;
	m_iTotalDocuments = pRanker->GetCtx()->m_iTotalDocs;
	m_fAvgDocLen /= m_iTotalDocuments;

	m_fParamK1 = 1.2f;
	m_fParamB = 0.75f;

	// not in SetQwords, because we only get iFields here
	m_dFieldTF.Resize ( m_iFields*(m_iMaxQpos+1) );
	m_dFieldTF.Fill ( 0 );

	ExprRankerHook_T<NEED_PACKEDFACTORS, HANDLE_DUPES> tHook ( this );

	// parse expression
	bool bUsesWeight;
	ExprParseArgs_t tExprArgs;
	tExprArgs.m_pAttrType = &m_eExprType;
	tExprArgs.m_pUsesWeight = &bUsesWeight;
	tExprArgs.m_pHook = &tHook;

	m_pExpr = sphExprParse ( m_sExpr, *m_pSchema, sError, tExprArgs ); // FIXME!!! profile UDF here too
	if ( !m_pExpr )
		return false;
	if ( m_eExprType!=SPH_ATTR_INTEGER && m_eExprType!=SPH_ATTR_FLOAT )
	{
		sError = "ranking expression must evaluate to integer or float";
		return false;
	}
	if ( bUsesWeight )
	{
		sError = "ranking expression must not refer to WEIGHT()";
		return false;
	}
	if ( tHook.m_sCheckError )
	{
		sError = tHook.m_sCheckError;
		return false;
	}

	int iUniq = m_iMaxQpos;
	if_const ( HANDLE_DUPES )
	{
		iUniq = 0;
		ARRAY_FOREACH ( i, m_dTermDupes )
			iUniq += ( IsTermSkipped(i) ? 0 : 1 );
	}

	m_iHaveMinWindow = iUniq;

	// we either have an ATC factor in the expression or packedfactors() without no_atc=1
	if ( m_bWantAtc || ( uFactorFlags & SPH_FACTOR_CALC_ATC ) )
		m_bHaveAtc = ( iUniq>1 );

	// all seems ok
	return true;
}


/// process next hit, update factors
template < bool NEED_PACKEDFACTORS, bool HANDLE_DUPES >
void RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES>::Update ( const ExtHit_t * pHlist )
{
	const DWORD uField = HITMAN::GetField ( pHlist->m_uHitpos );
	const int iPos = HITMAN::GetPos ( pHlist->m_uHitpos );
	const DWORD uPosWithField = HITMAN::GetPosWithField ( pHlist->m_uHitpos );

	if_const ( !HANDLE_DUPES )
	{
		// update LCS
		int iDelta = uPosWithField - pHlist->m_uQuerypos;
		if ( iDelta==m_iExpDelta )
		{
			if ( (int)uPosWithField>m_iLastHitPos )
				m_uCurLCS = (BYTE)( m_uCurLCS + pHlist->m_uWeight );
			if ( HITMAN::IsEnd ( pHlist->m_uHitpos ) && (int)pHlist->m_uQuerypos==m_iMaxQpos && iPos==m_iMaxQpos )
				m_tExactHit.BitSet ( uField );
		} else
		{
			if ( (int)uPosWithField>m_iLastHitPos )
				m_uCurLCS = BYTE(pHlist->m_uWeight);
			if ( iPos==1 && HITMAN::IsEnd ( pHlist->m_uHitpos ) && m_iMaxQpos==1 )
				m_tExactHit.BitSet ( uField );
		}

		if ( m_uCurLCS>m_uLCS[uField] )
		{
			m_uLCS[uField] = m_uCurLCS;
			// for the first hit in current field just use current position as min_best_span_pos
			// else adjust for current lcs
			if ( !m_iMinBestSpanPos [ uField ] )
				m_iMinBestSpanPos [ uField ] = iPos;
			else
				m_iMinBestSpanPos [ uField ] = iPos - m_uCurLCS + 1;
		}
		m_iExpDelta = iDelta + pHlist->m_uSpanlen - 1;
		m_iLastHitPos = uPosWithField;
	} else
	{
		// reset accumulated data from previous field
		if ( (DWORD)HITMAN::GetField ( m_uCurPos )!=uField )
		{
			m_uCurPos = 0;
			m_uLcsTailPos = 0;
			m_uCurQposMask = 0;
			m_uCurLCS = 0;
		}

		if ( (DWORD)uPosWithField!=m_uCurPos )
		{
			// next new and shiny hitpos in line
			// FIXME!? what do we do with longer spans? keep looking? reset?
			if ( m_uCurLCS<2 )
			{
				m_uLcsTailPos = m_uCurPos;
				m_uLcsTailQposMask = m_uCurQposMask;
				m_uCurLCS = 1;
			}
			m_uCurQposMask = 0;
			m_uCurPos = uPosWithField;
			if ( m_uLCS [ uField ]<pHlist->m_uWeight )
			{
				m_uLCS [ uField ] = BYTE ( pHlist->m_uWeight );
				m_iMinBestSpanPos [ uField ] = iPos;
				m_uLastSpanStart = iPos;
			}
		}

		// add that qpos to current qpos mask (for the current hitpos)
		m_uCurQposMask |= ( 1UL << pHlist->m_uQuerypos );

		// and check if that results in a better lcs match now
		int iDelta = ( m_uCurPos-m_uLcsTailPos );
		if ( iDelta && iDelta<32 && ( m_uCurQposMask >> iDelta ) & m_uLcsTailQposMask )
		{
			// cool, it matched!
			m_uLcsTailQposMask = ( 1UL << pHlist->m_uQuerypos ); // our lcs span now ends with a specific qpos
			m_uLcsTailPos = m_uCurPos; // and in a specific position
			m_uCurLCS = BYTE ( m_uCurLCS+pHlist->m_uWeight ); // and it's longer
			m_uCurQposMask = 0; // and we should avoid matching subsequent hits on the same hitpos

			// update per-field vector
			if ( m_uCurLCS>m_uLCS[uField] )
			{
				m_uLCS[uField] = m_uCurLCS;
				m_iMinBestSpanPos[uField] = m_uLastSpanStart;
			}
		}

		if ( iDelta==m_iExpDelta )
		{
			if ( HITMAN::IsEnd ( pHlist->m_uHitpos ) && (int)pHlist->m_uQuerypos==m_iMaxQpos && iPos==m_iMaxQpos )
				m_tExactHit.BitSet ( uField );
		} else
		{
			if ( iPos==1 && HITMAN::IsEnd ( pHlist->m_uHitpos ) && m_iMaxQpos==1 )
				m_tExactHit.BitSet ( uField );
		}
		m_iExpDelta = iDelta + pHlist->m_uSpanlen - 1;
	}

	bool bLetsKeepup = false;
	// update LCCS
	if ( m_iQueryPosLCCS==pHlist->m_uQuerypos && m_iHitPosLCCS==iPos )
	{
		m_iLenLCCS++;
		m_fWeightLCCS += m_dIDF [ pHlist->m_uQuerypos ];
	} else
	{
		if_const ( HANDLE_DUPES && m_iHitPosLCCS && iPos<=m_iHitPosLCCS && m_tHasMultiQpos.BitGet ( pHlist->m_uQuerypos ) )
		{
			bLetsKeepup = true;
		} else
		{
			m_iLenLCCS = 1;
			m_fWeightLCCS = m_dIDF[pHlist->m_uQuerypos];
		}
	}
	if ( !bLetsKeepup )
	{
		WORD iNextQPos = m_dNextQueryPos[pHlist->m_uQuerypos];
		m_iQueryPosLCCS = iNextQPos;
		m_iHitPosLCCS = iPos + pHlist->m_uSpanlen + iNextQPos - pHlist->m_uQuerypos - 1;
	}
	if ( m_dLCCS[uField]<=m_iLenLCCS ) // FIXME!!! check weight too or keep length and weight separate
	{
		m_dLCCS[uField] = m_iLenLCCS;
		m_dWLCCS[uField] = m_fWeightLCCS;
	}

	// update ATC
	if ( m_bHaveAtc )
	{
		if ( m_uAtcField!=uField || m_iAtcHitCount==XRANK_ATC_BUFFER_LEN )
		{
			UpdateATC ( m_uAtcField!=uField );
			if ( m_uAtcField!=uField )
			{
				m_uAtcField = uField;
			}
			if ( m_iAtcHitCount==XRANK_ATC_BUFFER_LEN ) // advance ring buffer
			{
				m_iAtcHitStart = ( m_iAtcHitStart + XRANK_ATC_WINDOW_LEN ) % XRANK_ATC_BUFFER_LEN;
				m_iAtcHitCount -= XRANK_ATC_WINDOW_LEN;
			}
		}
		assert ( m_iAtcHitStart<XRANK_ATC_BUFFER_LEN && m_iAtcHitCount<XRANK_ATC_BUFFER_LEN );
		int iRing = ( m_iAtcHitStart + m_iAtcHitCount ) % XRANK_ATC_BUFFER_LEN;
		AtcHit_t & tAtcHit = m_dAtcHits [ iRing ];
		tAtcHit.m_iHitpos = iPos;
		tAtcHit.m_uQuerypos = pHlist->m_uQuerypos;
		m_iAtcHitCount++;
	}

	// update other stuff
	m_tMatchedFields.BitSet ( uField );

	// keywords can be duplicated in the query, so we need this extra check
	WORD uQpos = pHlist->m_uQuerypos;
	bool bUniq = m_tKeywords.BitGet ( pHlist->m_uQuerypos );
	if_const ( HANDLE_DUPES && bUniq )
	{
		uQpos = m_dTermDupes [ uQpos ];
		bUniq = ( m_dTermsHit[uQpos]!=pHlist->m_uHitpos && m_dTermsHit[0]!=pHlist->m_uHitpos );
		m_dTermsHit[uQpos] = pHlist->m_uHitpos;
		m_dTermsHit[0] = pHlist->m_uHitpos;
	}
	if ( bUniq )
	{
		UpdateFreq ( uQpos, uField );
	}
	// handle hit with multiple terms
	if ( pHlist->m_uSpanlen>1 )
	{
		WORD uQposSpanned = pHlist->m_uQuerypos+1;
		DWORD uQposMask = ( pHlist->m_uQposMask>>1 );
		while ( uQposMask!=0 )
		{
			WORD uQposFixed = uQposSpanned;
			if ( ( uQposMask & 1 )==1 )
			{
				bool bUniqSpanned = true;
				if_const ( HANDLE_DUPES )
				{
					uQposFixed = m_dTermDupes[uQposFixed];
					bUniqSpanned = ( m_dTermsHit[uQposFixed]!=pHlist->m_uHitpos );
					m_dTermsHit[uQposFixed] = pHlist->m_uHitpos;
				}
				if ( bUniqSpanned )
					UpdateFreq ( uQposFixed, uField );
			}
			uQposSpanned++;
			uQposMask = ( uQposMask>>1 );
		}
	}

	if ( !m_iMinHitPos[uField] )
		m_iMinHitPos[uField] = iPos;

	// update hit window, max_window_hits factor
	if ( m_iWindowSize>1 )
	{
		if ( m_dWindow.GetLength() )
		{
			// sorted_remove_if ( _1 + winsize <= hitpos ) )
			int i = 0;
			while ( i<m_dWindow.GetLength() && ( m_dWindow[i] + m_iWindowSize )<=pHlist->m_uHitpos )
				i++;
			for ( int j=0; j<m_dWindow.GetLength()-i; j++ )
				m_dWindow[j] = m_dWindow[j+i];
			m_dWindow.Resize ( m_dWindow.GetLength()-i );
		}
		m_dWindow.Add ( pHlist->m_uHitpos );
		m_iMaxWindowHits[uField] = Max ( m_iMaxWindowHits[uField], m_dWindow.GetLength() );
	} else
		m_iMaxWindowHits[uField] = 1;

	// update exact_order factor
	if ( (int)uField!=m_iLastField )
	{
		m_iLastQuerypos = 0;
		m_iExactOrderWords = 0;
		m_iLastField = (int)uField;
	}
	if ( pHlist->m_uQuerypos==m_iLastQuerypos+1 )
	{
		if ( ++m_iExactOrderWords==m_iQueryWordCount )
			m_tExactOrder.BitSet ( uField );
		m_iLastQuerypos++;
	}

	// update min_gaps factor
	if ( bUniq && m_iHaveMinWindow>1 )
	{
		uQpos = pHlist->m_uQuerypos;
		if_const ( HANDLE_DUPES )
			uQpos = m_dTermDupes[uQpos];

		switch ( m_iHaveMinWindow )
		{
		// 2 keywords, special path
		case 2:
			if ( m_dMinWindowHits.GetLength() && HITMAN::GetField ( m_dMinWindowHits[0].m_uHitpos )!=(int)uField )
			{
				m_iMinWindowWords = 0;
				m_dMinWindowHits.Resize ( 0 );
			}

			if ( !m_dMinWindowHits.GetLength() )
			{
				m_dMinWindowHits.Add() = *pHlist; // {} => {A}
				m_dMinWindowHits.Last().m_uQuerypos = uQpos;
				break;
			}

			assert ( m_dMinWindowHits.GetLength()==1 );
			if ( uQpos==m_dMinWindowHits[0].m_uQuerypos )
				m_dMinWindowHits[0].m_uHitpos = pHlist->m_uHitpos;
			else
			{
				UpdateGap ( uField, 2, HITMAN::GetPos ( pHlist->m_uHitpos ) - HITMAN::GetPos ( m_dMinWindowHits[0].m_uHitpos ) - 1 );
				m_dMinWindowHits[0] = *pHlist;
				m_dMinWindowHits[0].m_uQuerypos = uQpos;
			}
			break;

		// 3 keywords, special path
		case 3:
			if ( m_dMinWindowHits.GetLength() && HITMAN::GetField ( m_dMinWindowHits.Last().m_uHitpos )!=(int)uField )
			{
				m_iMinWindowWords = 0;
				m_dMinWindowHits.Resize ( 0 );
			}

			// how many unique words are already there in the current candidate?
			switch ( m_dMinWindowHits.GetLength() )
			{
				case 0:
					m_dMinWindowHits.Add() = *pHlist; // {} => {A}
					m_dMinWindowHits.Last().m_uQuerypos = uQpos;
					break;

				case 1:
					if ( m_dMinWindowHits[0].m_uQuerypos==uQpos )
						m_dMinWindowHits[0] = *pHlist; // {A} + A2 => {A2}
					else
					{
						UpdateGap ( uField, 2, HITMAN::GetPos ( pHlist->m_uHitpos ) - HITMAN::GetPos ( m_dMinWindowHits[0].m_uHitpos ) - 1 );
						m_dMinWindowHits.Add() = *pHlist; // {A} + B => {A,B}
						m_dMinWindowHits.Last().m_uQuerypos = uQpos;
					}
					break;

				case 2:
					if ( m_dMinWindowHits[0].m_uQuerypos==uQpos )
					{
						UpdateGap ( uField, 2, HITMAN::GetPos ( pHlist->m_uHitpos ) - HITMAN::GetPos ( m_dMinWindowHits[1].m_uHitpos ) - 1 );
						m_dMinWindowHits[0] = m_dMinWindowHits[1]; // {A,B} + A2 => {B,A2}
						m_dMinWindowHits[1] = *pHlist;
						m_dMinWindowHits[1].m_uQuerypos = uQpos;
					} else if ( m_dMinWindowHits[1].m_uQuerypos==uQpos )
					{
						m_dMinWindowHits[1] = *pHlist; // {A,B} + B2 => {A,B2}
						m_dMinWindowHits[1].m_uQuerypos = uQpos;
					} else
					{
						// new {A,B,C} window!
						// handle, and then immediately reduce it to {B,C}
						UpdateGap ( uField, 3, HITMAN::GetPos ( pHlist->m_uHitpos ) - HITMAN::GetPos ( m_dMinWindowHits[0].m_uHitpos ) - 2 );
						m_dMinWindowHits[0] = m_dMinWindowHits[1];
						m_dMinWindowHits[1] = *pHlist;
						m_dMinWindowHits[1].m_uQuerypos = uQpos;
					}
					break;

				default:
					assert ( 0 && "min_gaps current window size not in 0..2 range; must not happen" );
			}
			break;

		// slow generic update
		default:
			UpdateMinGaps ( pHlist );
			break;
		}
	}
}


template < bool PF, bool HANDLE_DUPES >
void RankerState_Expr_fn<PF, HANDLE_DUPES>::UpdateFreq ( WORD uQpos, DWORD uField )
{
	float fIDF = m_dIDF [ uQpos ];
	DWORD uHitPosMask = 1u<<uQpos;

	if ( !( m_uWordCount[uField] & uHitPosMask ) )
		m_dSumIDF[uField] += fIDF;

	if ( fIDF < m_dMinIDF[uField] )
		m_dMinIDF[uField] = fIDF;

	if ( fIDF > m_dMaxIDF[uField] )
		m_dMaxIDF[uField] = fIDF;

	m_uHitCount[uField]++;
	m_uWordCount[uField] |= uHitPosMask;
	m_uDocWordCount |= uHitPosMask;
	m_dTFIDF[uField] += fIDF;

	// avoid duplicate check for BM25A, BM25F though
	// (that sort of automatically accounts for qtf factor)
	m_dTF [ uQpos ]++;
	m_dFieldTF [ uField*(1+m_iMaxQpos) + uQpos ]++;
}


template < bool PF, bool HANDLE_DUPES >
void RankerState_Expr_fn<PF, HANDLE_DUPES>::UpdateMinGaps ( const ExtHit_t * pHlist )
{
	// update the minimum MW, aka matching window, for min_gaps and ymw factors
	// we keep a window with all the positions of all the matched words
	// we keep it left-minimal at all times, so that leftmost keyword only occurs once
	// thus, when a previously unseen keyword is added, the window is guaranteed to be minimal

	WORD uQpos = pHlist->m_uQuerypos;
	if_const ( HANDLE_DUPES )
		uQpos = m_dTermDupes[uQpos];

	// handle field switch
	const int iField = HITMAN::GetField ( pHlist->m_uHitpos );
	if ( m_dMinWindowHits.GetLength() && HITMAN::GetField ( m_dMinWindowHits.Last().m_uHitpos )!=iField )
	{
		m_dMinWindowHits.Resize ( 0 );
		m_dMinWindowCounts.Fill ( 0 );
		m_iMinWindowWords = 0;
	}

	// assert we are left-minimal
	assert ( m_dMinWindowHits.GetLength()==0 || m_dMinWindowCounts [ m_dMinWindowHits[0].m_uQuerypos ]==1 );

	// another occurrence of the trailing word?
	// just update hitpos, effectively dumping the current occurrence
	if ( m_dMinWindowHits.GetLength() && m_dMinWindowHits.Last().m_uQuerypos==uQpos )
	{
		m_dMinWindowHits.Last().m_uHitpos = pHlist->m_uHitpos;
		return;
	}

	// add that word
	LeanHit_t & t = m_dMinWindowHits.Add();
	t.m_uQuerypos = uQpos;
	t.m_uHitpos = pHlist->m_uHitpos;

	int iWord = uQpos;
	m_dMinWindowCounts[iWord]++;

	// new, previously unseen keyword? just update the window size
	if ( m_dMinWindowCounts[iWord]==1 )
	{
		m_iMinGaps[iField] = HITMAN::GetPos ( pHlist->m_uHitpos ) - HITMAN::GetPos ( m_dMinWindowHits[0].m_uHitpos ) - m_iMinWindowWords;
		m_iMinWindowWords++;
		return;
	}

	// check if we can shrink the left boundary
	if ( iWord!=m_dMinWindowHits[0].m_uQuerypos )
		return;

	// yes, we can!
	// keep removing the leftmost keyword until it's unique (in the window) again
	assert ( m_dMinWindowCounts [ m_dMinWindowHits[0].m_uQuerypos ]==2 );
	int iShrink = 0;
	while ( m_dMinWindowCounts [ m_dMinWindowHits [ iShrink ].m_uQuerypos ]!=1 )
	{
		m_dMinWindowCounts [ m_dMinWindowHits [ iShrink ].m_uQuerypos ]--;
		iShrink++;
	}

	int iNewLen = m_dMinWindowHits.GetLength() - iShrink;
	memmove ( m_dMinWindowHits.Begin(), &m_dMinWindowHits[iShrink], iNewLen*sizeof(LeanHit_t) );
	m_dMinWindowHits.Resize ( iNewLen );

	int iNewGaps = HITMAN::GetPos ( pHlist->m_uHitpos ) - HITMAN::GetPos ( m_dMinWindowHits[0].m_uHitpos ) - m_iMinWindowWords + 1;
	m_iMinGaps[iField] = Min ( m_iMinGaps[iField], iNewGaps );
}


template<bool A1, bool A2>
int RankerState_Expr_fn<A1,A2>::GetMaxPackedLength()
{
	return sizeof(DWORD)*( 8 + m_tExactHit.GetSize() + m_tExactOrder.GetSize() + m_iFields*15 + m_iMaxQpos*4 + m_dFieldTF.GetLength() );
}


template < bool NEED_PACKEDFACTORS, bool HANDLE_DUPES >
BYTE * RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES>::PackFactors()
{
	auto * pPackStart = (DWORD *)m_tFactorPool.Alloc();
	DWORD * pPack = pPackStart;
	assert ( pPackStart );

	// leave space for size
	pPack++;
	assert ( m_tMatchedFields.GetSize()==m_tExactHit.GetSize() && m_tExactHit.GetSize()==m_tExactOrder.GetSize() );

	// document level factors
	*pPack++ = m_uDocBM25;
	*pPack++ = sphF2DW ( m_fDocBM25A );
	*pPack++ = *m_tMatchedFields.Begin();
	*pPack++ = m_uDocWordCount;

	// field level factors
	*pPack++ = (DWORD)m_iFields;
	// v.6 set these depends on number of fields
	for ( int i=0; i<m_tExactHit.GetSize(); i++ )
		*pPack++ = *( m_tExactHit.Begin() + i );
	for ( int i=0; i<m_tExactOrder.GetSize(); i++ )
		*pPack++ = *( m_tExactOrder.Begin() + i );

	for ( int i=0; i<m_iFields; i++ )
	{
		DWORD uHit = m_uHitCount[i];
		*pPack++ = uHit;
		if ( uHit )
		{
			*pPack++ = (DWORD)i;
			*pPack++ = m_uLCS[i];
			*pPack++ = m_uWordCount[i];
			*pPack++ = sphF2DW ( m_dTFIDF[i] );
			*pPack++ = sphF2DW ( m_dMinIDF[i] );
			*pPack++ = sphF2DW ( m_dMaxIDF[i] );
			*pPack++ = sphF2DW ( m_dSumIDF[i] );
			*pPack++ = (DWORD)m_iMinHitPos[i];
			*pPack++ = (DWORD)m_iMinBestSpanPos[i];
			// had exact_hit here before v.4
			*pPack++ = (DWORD)m_iMaxWindowHits[i];
			*pPack++ = (DWORD)m_iMinGaps[i]; // added in v.3
			*pPack++ = sphF2DW ( m_dAtc[i] );			// added in v.4
			*pPack++ = m_dLCCS[i];					// added in v.5
			*pPack++ = sphF2DW ( m_dWLCCS[i] );	// added in v.5
		}
	}

	// word level factors
	*pPack++ = (DWORD)m_iMaxQpos;
	for ( int i=1; i<=m_iMaxQpos; i++ )
	{
		DWORD uKeywordMask = !IsTermSkipped(i); // !COMMIT !m_tExcluded.BitGet(i);
		*pPack++ = uKeywordMask;
		if ( uKeywordMask )
		{
			*pPack++ = (DWORD)i;
			*pPack++ = (DWORD)m_dTF[i];
			*pPack++ = *(DWORD*)&m_dIDF[i];
		}
	}

	// m_dFieldTF = iWord + iField * ( 1 + iWordsCount )
	// FIXME! pack these sparse factors ( however these should fit into fixed-size FactorPool block )
	*pPack++ = m_dFieldTF.GetLength();
	memcpy ( pPack, m_dFieldTF.Begin(), m_dFieldTF.GetLength()*sizeof(m_dFieldTF[0]) );
	pPack += m_dFieldTF.GetLength();

	*pPackStart = (DWORD)((pPack-pPackStart)*sizeof(DWORD));
	assert ( (pPack-pPackStart)*sizeof(DWORD)<=(DWORD)m_tFactorPool.GetElementSize() );
	return (BYTE*)pPackStart;
}


template <bool NEED_PACKEDFACTORS, bool HANDLE_DUPES>
bool RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES>::ExtraDataImpl ( ExtraData_e eType, void ** ppResult )
{
	if_const ( eType!=EXTRA_SET_BLOBPOOL && !NEED_PACKEDFACTORS )
		return false;

	switch ( eType )
	{
		case EXTRA_SET_BLOBPOOL:
			m_pExpr->Command ( SPH_EXPR_SET_BLOB_POOL, *ppResult );
			return true;
		case EXTRA_SET_POOL_CAPACITY:
			m_iPoolMatchCapacity = *(int*)ppResult;
			m_iPoolMatchCapacity += MAX_BLOCK_DOCS;
			return true;
		case EXTRA_SET_MATCHPUSHED:
			m_tFactorPool.AddRef ( *(RowID_t*)ppResult );
			return true;
		case EXTRA_SET_MATCHPOPPED:
			for ( RowID_t iRow : *(CSphTightVector<RowID_t> *) ppResult )
				m_tFactorPool.Release ( iRow );
			return true;
		case EXTRA_GET_DATA_PACKEDFACTORS:
			*ppResult = m_tFactorPool.GetHashPtr();
			return true;
		case EXTRA_GET_DATA_RANKER_STATE:
			{
				auto * pState = (SphExtraDataRankerState_t *)ppResult;
				pState->m_iFields = m_iFields;
				pState->m_pSchema = m_pSchema;
				pState->m_pFieldLens = m_pFieldLens;
				pState->m_iTotalDocuments = m_iTotalDocuments;
				pState->m_tFieldLensLoc = m_tFieldLensLoc;
				pState->m_iMaxQpos = m_iMaxQpos;
			}
			return true;
		default:
			return false;
	}

	return true;
}

template < bool NEED_PACKEDFACTORS, bool HANDLE_DUPES >
BYTE * RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES>::GetTextFeatures ( const CSphMatch & tMatch )
{
    assert(NEED_PACKEDFACTORS) ;
    if (!m_tFactorPool.IsInitialized())
      m_tFactorPool.Prealloc(GetMaxPackedLength(), m_iPoolMatchCapacity);
    return PackFactors();
}

/// finish document processing, compute weight from factors
template < bool NEED_PACKEDFACTORS, bool HANDLE_DUPES >
int RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES>::Finalize ( const CSphMatch & tMatch )
{
#ifndef NDEBUG
	// sanity check
	for ( int i=0; i<m_iFields; ++i )
	{
		assert ( m_iMinHitPos[i]<=m_iMinBestSpanPos[i] );
		if ( m_uLCS[i]==1 )
			assert ( m_iMinHitPos[i]==m_iMinBestSpanPos[i] );
	}
#endif // NDEBUG

	// finishing touches
	FinalizeDocFactors ( tMatch );
	UpdateATC ( true );

	if_const ( NEED_PACKEDFACTORS )
	{
		// pack factors
		if ( !m_tFactorPool.IsInitialized() )
			m_tFactorPool.Prealloc ( GetMaxPackedLength(), m_iPoolMatchCapacity );
		m_tFactorPool.AddToHash ( tMatch.m_tRowID, PackFactors() );
	}

	// compute expression
	int iRes = ( m_eExprType==SPH_ATTR_INTEGER )
		? m_pExpr->IntEval ( tMatch )
		: (int)m_pExpr->Eval ( tMatch );

	if_const ( HANDLE_DUPES )
	{
		m_uCurPos = 0;
		m_uLcsTailPos = 0;
		m_uLcsTailQposMask = 0;
		m_uCurQposMask = 0;
	}

	// cleanup
	ResetDocFactors();
	memset ( m_dLCCS, 0 , sizeof(m_dLCCS) );
	memset ( m_dWLCCS, 0, sizeof(m_dWLCCS) );
	m_iQueryPosLCCS = 0;
	m_iHitPosLCCS = 0;
	m_iLenLCCS = 0;
	m_fWeightLCCS = 0.0f;

	// done
	return iRes;
}


template < bool NEED_PACKEDFACTORS, bool HANDLE_DUPES >
bool RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES>::IsTermSkipped ( int iTerm )
{
	assert ( iTerm>=0 && iTerm<m_iMaxQpos+1 );
	if_const ( HANDLE_DUPES )
		return !m_tKeywords.BitGet ( iTerm ) || m_dTermDupes[iTerm]!=iTerm;
	else
		return !m_tKeywords.BitGet ( iTerm );
}


template < bool NEED_PACKEDFACTORS, bool HANDLE_DUPES >
float RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES>::TermTC ( int iTerm, bool bLeft )
{
	// border case short-cut
	if ( ( bLeft && iTerm==m_iAtcHitStart ) || ( !bLeft && iTerm==m_iAtcHitStart+m_iAtcHitCount-1 ) )
		return 0.0f;

	int iRing = iTerm % XRANK_ATC_BUFFER_LEN;
	int iHitpos = m_dAtcHits[iRing].m_iHitpos;
	WORD uQuerypos = m_dAtcHits[iRing].m_uQuerypos;

	m_dAtcProcessedTerms.Clear();

	float fTC = 0.0f;

	// loop bounds for down \ up climbing
	int iStart, iEnd, iStep;
	if ( bLeft )
	{
		iStart = iTerm - 1;
		iEnd = Max ( iStart - XRANK_ATC_WINDOW_LEN, m_iAtcHitStart-1 );
		iStep = -1;
	} else
	{
		iStart = iTerm + 1;
		iEnd = Min ( iStart + XRANK_ATC_WINDOW_LEN, m_iAtcHitStart + m_iAtcHitCount );
		iStep = 1;
	}

	int iFound = 0;
	for ( int i=iStart; i!=iEnd && iFound!=m_iMaxQpos; i+=iStep )
	{
		iRing = i % XRANK_ATC_BUFFER_LEN;
		const AtcHit_t & tCur = m_dAtcHits[iRing];
		bool bGotDup = ( uQuerypos==tCur.m_uQuerypos );

		if ( m_dAtcProcessedTerms.BitGet ( tCur.m_uQuerypos ) || iHitpos==tCur.m_iHitpos )
			continue;

		auto fWeightedDist = (float)pow ( float ( abs ( iHitpos - tCur.m_iHitpos ) ), XRANK_ATC_EXP );
		float fTermTC = ( m_dIDF[tCur.m_uQuerypos] / fWeightedDist );
		if ( bGotDup )
			fTermTC *= XRANK_ATC_DUP_DIV;
		fTC += fTermTC;

		m_dAtcProcessedTerms.BitSet ( tCur.m_uQuerypos );
		iFound++;
	}

	return fTC;
}


template < bool NEED_PACKEDFACTORS, bool HANDLE_DUPES >
void RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES>::UpdateATC ( bool bFlushField )
{
	if ( !m_iAtcHitCount )
		return;

	int iWindowStart = m_iAtcHitStart + XRANK_ATC_WINDOW_LEN;
	int iWindowEnd = Min ( iWindowStart + XRANK_ATC_WINDOW_LEN, m_iAtcHitStart+m_iAtcHitCount );
	// border cases (hits: +below ATC window collected since start of buffer; +at the end of buffer and less then ATC window)
	if ( !m_bAtcHeadProcessed )
		iWindowStart = m_iAtcHitStart;
	if ( bFlushField )
		iWindowEnd = m_iAtcHitStart+m_iAtcHitCount;

	assert ( iWindowStart<iWindowEnd && iWindowStart>=m_iAtcHitStart && iWindowEnd<=m_iAtcHitStart+m_iAtcHitCount );
	// per term ATC
	// sigma(t' E query) ( idf(t') \ left_deltapos(t, t')^z + idf (t') \ right_deltapos(t,t')^z ) * ( t==t' ? 0.25 : 1 )
	for ( int iWinPos=iWindowStart; iWinPos<iWindowEnd; iWinPos++ )
	{
		float fTC = TermTC ( iWinPos, true ) + TermTC ( iWinPos, false );

		int iRing = iWinPos % XRANK_ATC_BUFFER_LEN;
		m_dAtcTerms [ m_dAtcHits[iRing].m_uQuerypos ] += fTC;
	}

	m_bAtcHeadProcessed = true;
	if ( bFlushField )
	{
		float fWeightedSum = 0.0f;
		ARRAY_FOREACH ( i, m_dAtcTerms )
		{
			fWeightedSum += m_dAtcTerms[i] * m_dIDF[i];
			m_dAtcTerms[i] = 0.0f;
		}

		m_dAtc[m_uAtcField] = (float)log ( 1.0f + fWeightedSum );
		m_iAtcHitStart = 0;
		m_iAtcHitCount = 0;
		m_bAtcHeadProcessed = false;
	}
}


/// expression ranker
template < bool NEED_PACKEDFACTORS, bool HANDLE_DUPES >
class ExtRanker_Expr_T : public ExtRanker_State_T< RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES>, true >
{
public:
	ExtRanker_Expr_T ( const XQQuery_t & tXQ, const ISphQwordSetup & tSetup, const char * sExpr, const CSphSchema & tSchema, bool bSkipQCache )
		: ExtRanker_State_T< RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES>, true > ( tXQ, tSetup, bSkipQCache )
	{
		// tricky bit, we stash the pointer to expr here, but it will be parsed
		// somewhat later during InitState() call, when IDFs etc are computed
		this->m_tState.m_sExpr = sExpr;
		this->m_tState.m_pSchema = &tSchema;
	}

	void SetQwordsIDF ( const ExtQwordsHash_t & hQwords ) final
	{
		ExtRanker_State_T< RankerState_Expr_fn<NEED_PACKEDFACTORS, HANDLE_DUPES>,true >::SetQwordsIDF ( hQwords );
		this->m_tState.m_iMaxQpos = this->m_iMaxQpos;
		this->m_tState.SetQwords ( hQwords );
	}

	int GetMatches () final
	{
		if_const ( NEED_PACKEDFACTORS )
			this->m_tState.FlushMatches ();

		return ExtRanker_State_T<RankerState_Expr_fn <NEED_PACKEDFACTORS, HANDLE_DUPES>,true >::GetMatches ();
	}

	void SetTermDupes ( const ExtQwordsHash_t & hQwords, int iMaxQpos ) final
	{
		this->m_tState.SetTermDupes ( hQwords, iMaxQpos, this->m_pRoot );
	}

    bool CanExportTextFeatures() { return NEED_PACKEDFACTORS; }

    BYTE* GetTextFeatures( const CSphMatch & tMatch ) {
      return ExtRanker_State_T<RankerState_Expr_fn <NEED_PACKEDFACTORS, HANDLE_DUPES>,true >::GetTextFeatures (tMatch);
    }
};

//////////////////////////////////////////////////////////////////////////
// EXPRESSION FACTORS EXPORT RANKER
//////////////////////////////////////////////////////////////////////////

/// ranker state that computes BM25 as weight, but also all known factors for export purposes
struct RankerState_Export_fn : public RankerState_Expr_fn<>
{
public:
	CSphOrderedHash < CSphString, RowID_t, IdentityHash_fn, 256 > m_hFactors;

public:
	int Finalize ( const CSphMatch & tMatch )
	{
		// finalize factor computations
		FinalizeDocFactors ( tMatch );

		// build document level factors
		// FIXME? should we build query level factors too? max_lcs, query_word_count, etc
		const int MAX_STR_LEN = 1024;
		CSphVector<char> dVal;
		dVal.Resize ( MAX_STR_LEN );
		snprintf ( dVal.Begin(), dVal.GetLength(), "bm25=%d, bm25a=%f, field_mask=%d, doc_word_count=%d",
			m_uDocBM25, m_fDocBM25A, *m_tMatchedFields.Begin(), m_uDocWordCount );

		char sTmp[MAX_STR_LEN];

		// build field level factors
		for ( int i=0; i<m_iFields; i++ )
			if ( m_uHitCount[i] )
		{
			snprintf ( sTmp, MAX_STR_LEN, ", field%d="
				"(lcs=%d, hit_count=%d, word_count=%d, "
				"tf_idf=%f, min_idf=%f, max_idf=%f, sum_idf=%f, "
				"min_hit_pos=%d, min_best_span_pos=%d, exact_hit=%d, max_window_hits=%d)",
				i,
				m_uLCS[i], m_uHitCount[i], m_uWordCount[i],
				m_dTFIDF[i], m_dMinIDF[i], m_dMaxIDF[i], m_dSumIDF[i],
				m_iMinHitPos[i], m_iMinBestSpanPos[i], m_tExactHit.BitGet ( i ), m_iMaxWindowHits[i] );

			auto iValLen = (int) strlen ( dVal.Begin() );
			auto iTotalLen = iValLen+(int)strlen(sTmp);
			if ( dVal.GetLength() < iTotalLen+1 )
				dVal.Resize ( iTotalLen+1 );

			strcpy ( &(dVal[iValLen]), sTmp ); //NOLINT
		}

		// build word level factors
		for ( int i=1; i<=m_iMaxQpos; i++ )
			if ( !IsTermSkipped ( i ) )
		{
			snprintf ( sTmp, MAX_STR_LEN, ", word%d=(tf=%d, idf=%f)", i, m_dTF[i], m_dIDF[i] );
			auto iValLen = (int)strlen ( dVal.Begin() );
			auto iTotalLen = iValLen+(int)strlen(sTmp);
			if ( dVal.GetLength() < iTotalLen+1 )
				dVal.Resize ( iTotalLen+1 );

			strcpy ( &(dVal[iValLen]), sTmp ); //NOLINT
		}

		// export factors
		m_hFactors.Add ( dVal.Begin(), tMatch.m_tRowID );

		// compute sorting expression now
		int iRes = ( m_eExprType==SPH_ATTR_INTEGER )
			? m_pExpr->IntEval ( tMatch )
			: (int)m_pExpr->Eval ( tMatch );

		// cleanup and return!
		ResetDocFactors();
		return iRes;
	}

	bool ExtraDataImpl ( ExtraData_e eType, void ** ppResult ) final
	{
		if ( eType==EXTRA_GET_DATA_RANKFACTORS )
			*ppResult = &m_hFactors;
		return true;
	}
};

/// export ranker that emits BM25 as the weight, but computes and export all the factors
/// useful for research purposes, eg. exporting the data for machine learning
class ExtRanker_Export_c : public ExtRanker_State_T<RankerState_Export_fn, true>
{
public:
	ExtRanker_Export_c ( const XQQuery_t & tXQ, const ISphQwordSetup & tSetup, const char * sExpr, const CSphSchema & tSchema, bool bSkipQCache )
		: ExtRanker_State_T<RankerState_Export_fn,true> ( tXQ, tSetup, bSkipQCache )
	{
		m_tState.m_sExpr = sExpr;
		m_tState.m_pSchema = &tSchema;
	}

	void SetQwordsIDF ( const ExtQwordsHash_t & hQwords ) final
	{
		ExtRanker_State_T<RankerState_Export_fn,true>::SetQwordsIDF ( hQwords );
		m_tState.m_iMaxQpos = m_iMaxQpos;
		m_tState.SetQwords ( hQwords );
	}
};

//////////////////////////////////////////////////////////////////////////
// RANKER FACTORY
//////////////////////////////////////////////////////////////////////////

struct ExtQwordOrderbyQueryPos_t
{
	bool IsLess ( const ExtQword_t * pA, const ExtQword_t * pB ) const
	{
		return pA->m_iQueryPos<pB->m_iQueryPos;
	}
};


static bool HasQwordDupes ( XQNode_t * pNode, SmallStringHash_T<int> & hQwords )
{
	ARRAY_FOREACH ( i, pNode->m_dChildren )
		if ( HasQwordDupes ( pNode->m_dChildren[i], hQwords ) )
			return true;
	ARRAY_FOREACH ( i, pNode->m_dWords )
		if ( !hQwords.Add ( 1, pNode->m_dWords[i].m_sWord ) )
			return true;
	return false;
}


static bool HasQwordDupes ( XQNode_t * pNode )
{
	SmallStringHash_T<int> hQwords;
	return HasQwordDupes ( pNode, hQwords );
}


ISphRanker * sphCreateRanker ( const XQQuery_t & tXQ, const CSphQuery & tQuery, CSphQueryResultMeta & tMeta,
	const ISphQwordSetup & tTermSetup, const CSphQueryContext & tCtx, const ISphSchema & tSorterSchema )
{
	// shortcut
	const CSphIndex * pIndex = tTermSetup.m_pIndex;

	// fill payload mask
	DWORD uPayloadMask = 0;
	for ( int i=0; i < pIndex->GetMatchSchema().GetFieldsCount(); i++ )
		uPayloadMask |= pIndex->GetMatchSchema().GetField(i).m_bPayload << i;

	bool bGotDupes = HasQwordDupes ( tXQ.m_pRoot );
	bool bSkipQCache = tCtx.m_bSkipQCache;

	// can we serve this from cache?
	QcacheEntryRefPtr_t pCached;
	if ( !bSkipQCache )
		pCached = QcacheFind ( pIndex->GetIndexId(), tQuery, tSorterSchema );
	if ( pCached )
		return QcacheRanker ( pCached, tTermSetup );

	// setup eval-tree
	ExtRanker_c * pRanker = nullptr;
	switch ( tQuery.m_eRanker )
	{
		case SPH_RANK_PROXIMITY_BM25:
			if ( uPayloadMask )
				pRanker = new ExtRanker_State_T < RankerState_ProximityPayload_fn<true>, true > ( tXQ, tTermSetup, bSkipQCache );
			else if ( tXQ.m_bSingleWord )
				pRanker = new ExtRanker_WeightSum_c<WITH_BM25> ( tXQ, tTermSetup, bSkipQCache );
			else if ( bGotDupes )
				pRanker = new ExtRanker_State_T < RankerState_Proximity_fn<true,true>, true > ( tXQ, tTermSetup, bSkipQCache );
			else
				pRanker = new ExtRanker_State_T < RankerState_Proximity_fn<true,false>, true > ( tXQ, tTermSetup, bSkipQCache );
			break;

		case SPH_RANK_BM25:
			pRanker = new ExtRanker_WeightSum_c<WITH_BM25> ( tXQ, tTermSetup, bSkipQCache );
			break;

		case SPH_RANK_NONE:
			pRanker = new ExtRanker_None_c ( tXQ, tTermSetup, bSkipQCache );
			break;

		case SPH_RANK_WORDCOUNT:
			pRanker = new ExtRanker_State_T < RankerState_Wordcount_fn, false > ( tXQ, tTermSetup, bSkipQCache );
			break;

		case SPH_RANK_PROXIMITY:
			if ( tXQ.m_bSingleWord )
				pRanker = new ExtRanker_WeightSum_c<> ( tXQ, tTermSetup, bSkipQCache );
			else if ( bGotDupes )
				pRanker = new ExtRanker_State_T < RankerState_Proximity_fn<false,true>, false > ( tXQ, tTermSetup, bSkipQCache );
			else
				pRanker = new ExtRanker_State_T < RankerState_Proximity_fn<false,false>, false > ( tXQ, tTermSetup, bSkipQCache );
			break;

		case SPH_RANK_MATCHANY:
			pRanker = new ExtRanker_State_T < RankerState_MatchAny_fn,  false> ( tXQ, tTermSetup, bSkipQCache );
			break;

		case SPH_RANK_FIELDMASK:
			pRanker = new ExtRanker_State_T < RankerState_Fieldmask_fn, false > ( tXQ, tTermSetup, bSkipQCache );
			break;

		case SPH_RANK_SPH04:
			pRanker = new ExtRanker_State_T < RankerState_ProximityBM25Exact_fn, true > ( tXQ, tTermSetup, bSkipQCache );
			break;

		case SPH_RANK_EXPR:
			{
				// we need that mask in case these factors usage:
				// min_idf,max_idf,sum_idf,hit_count,word_count,doc_word_count,tf_idf,tf,field_tf
				// however ranker expression got parsed later at Init stage
				// FIXME!!! move QposMask initialization past Init
				tTermSetup.m_bSetQposMask = true;
				bool bNeedFactors = !!(tCtx.m_uPackedFactorFlags & SPH_FACTOR_ENABLE);
				if ( bNeedFactors && bGotDupes )
					pRanker = new ExtRanker_Expr_T <true, true> ( tXQ, tTermSetup, tQuery.m_sRankerExpr.cstr(), pIndex->GetMatchSchema(), bSkipQCache );
				else if ( bNeedFactors && !bGotDupes )
					pRanker = new ExtRanker_Expr_T <true, false> ( tXQ, tTermSetup, tQuery.m_sRankerExpr.cstr(), pIndex->GetMatchSchema(), bSkipQCache );
				else if ( !bNeedFactors && bGotDupes )
					pRanker = new ExtRanker_Expr_T <false, true> ( tXQ, tTermSetup, tQuery.m_sRankerExpr.cstr(), pIndex->GetMatchSchema(), bSkipQCache );
				else if ( !bNeedFactors && !bGotDupes )
					pRanker = new ExtRanker_Expr_T <false, false> ( tXQ, tTermSetup, tQuery.m_sRankerExpr.cstr(), pIndex->GetMatchSchema(), bSkipQCache );
			}
			break;

		case SPH_RANK_EXPORT:
			// TODO: replace Export ranker to Expression ranker to remove duplicated code
			tTermSetup.m_bSetQposMask = true;
			pRanker = new ExtRanker_Export_c ( tXQ, tTermSetup, tQuery.m_sRankerExpr.cstr(), pIndex->GetMatchSchema(), bSkipQCache );
			break;

		default:
			tMeta.m_sWarning.SetSprintf ( "unknown ranking mode %d; using default", (int) tQuery.m_eRanker );
			if ( bGotDupes )
				pRanker = new ExtRanker_State_T < RankerState_Proximity_fn<true,true>, true > ( tXQ, tTermSetup, bSkipQCache );
			else
				pRanker = new ExtRanker_State_T < RankerState_Proximity_fn<true,false>, false > ( tXQ, tTermSetup, bSkipQCache );
			break;

		case SPH_RANK_PLUGIN:
			{
				const auto * p = (const PluginRanker_c *) sphPluginGet ( PLUGIN_RANKER, tQuery.m_sUDRanker.cstr() );
				// might be a case for query to distributed index
				if ( p )
				{
					pRanker = new ExtRanker_State_T < RankerState_Plugin_fn, true > ( tXQ, tTermSetup, bSkipQCache );
					pRanker->ExtraData ( EXTRA_SET_RANKER_PLUGIN, (void**)p );
					pRanker->ExtraData ( EXTRA_SET_RANKER_PLUGIN_OPTS, (void**) tQuery.m_sUDRankerOpts.cstr() );
				} else
				{
					// create default ranker in case of missed plugin
					tMeta.m_sWarning.SetSprintf ( "unknown ranker plugin '%s'; using default", tQuery.m_sUDRanker.cstr() );
					if ( bGotDupes )
						pRanker = new ExtRanker_State_T < RankerState_Proximity_fn<true,true>, true > ( tXQ, tTermSetup, bSkipQCache );
					else
						pRanker = new ExtRanker_State_T < RankerState_Proximity_fn<true,false>, true > ( tXQ, tTermSetup, bSkipQCache );
				}
			}
			break;
	}
	assert ( pRanker );
	pRanker->m_uPayloadMask = uPayloadMask;

	// setup IDFs
	ExtQwordsHash_t hQwords;
	int iMaxQpos = pRanker->GetQwords ( hQwords );

	const int iQwords = hQwords.GetLength ();
	int64_t iTotalDocuments = tCtx.m_iTotalDocs;

	CSphVector<const ExtQword_t *> dWords;
	dWords.Reserve ( hQwords.GetLength() );

	hQwords.IterateStart ();
	while ( hQwords.IterateNext() )
	{
		ExtQword_t & tWord = hQwords.IterateGet ();

		int64_t iTermDocs = tWord.m_iDocs;
		// shared docs count
		if ( tCtx.m_pLocalDocs )
		{
			int64_t * pDocs = (*tCtx.m_pLocalDocs)( tWord.m_sWord );
			if ( pDocs )
				iTermDocs = *pDocs;
		}

		// build IDF
		float fIDF = 0.0f;
		if ( tQuery.m_bGlobalIDF )
			fIDF = pIndex->GetGlobalIDF ( tWord.m_sWord, iTermDocs, tQuery.m_bPlainIDF );
		else if ( iTermDocs )
		{
			// (word_docs > total_docs) case *is* occasionally possible
			// because of dupes, or delayed purging in RT, etc
			// FIXME? we don't expect over 4G docs per just 1 local index
			const int64_t iTotalClamped = Max ( iTotalDocuments, iTermDocs );

			if ( !tQuery.m_bPlainIDF )
			{
				// bm25 variant, idf = log((N-n+1)/n), as per Robertson et al
				//
				// idf \in [-log(N), log(N)]
				// weight \in [-NumWords*log(N), NumWords*log(N)]
				// we want weight \in [0, 1] range
				// we prescale idfs and get weight \in [-0.5, 0.5] range
				// then add 0.5 as our final step
				//
				// for the record, idf = log((N-n+0.5)/(n+0.5)) in the original paper
				// but our variant is a bit easier to compute, and has a better (symmetric) range
				float fLogTotal = logf ( float ( 1+iTotalClamped ) );
				fIDF = logf ( float ( iTotalClamped-iTermDocs+1 ) / float ( iTermDocs ) )
					/ ( 2*fLogTotal );
			} else
			{
				// plain variant, idf=log(N/n), as per Sparck-Jones
				//
				// idf \in [0, log(N)]
				// weight \in [0, NumWords*log(N)]
				// we prescale idfs and get weight in [0, 0.5] range
				// then add 0.5 as our final step
				float fLogTotal = logf ( float ( 1+iTotalClamped ) );
				fIDF = logf ( float ( iTotalClamped ) / float ( iTermDocs ) )
					/ ( 2*fLogTotal );
			}
		}

		// optionally normalize IDFs so that sum(TF*IDF) fits into [0, 1]
		if ( tQuery.m_bNormalizedTFIDF )
			fIDF /= iQwords;

		tWord.m_fIDF = fIDF * tWord.m_fBoost;
		dWords.Add ( &tWord );
	}

	dWords.Sort ( ExtQwordOrderbyQueryPos_t() );
	ARRAY_FOREACH ( i, dWords )
	{
		const ExtQword_t * pWord = dWords[i];
		if ( !pWord->m_bExpanded )
			tMeta.AddStat ( pWord->m_sDictWord, pWord->m_iDocs, pWord->m_iHits );
	}

	pRanker->m_iMaxQpos = iMaxQpos;
	pRanker->SetQwordsIDF ( hQwords );
	if ( bGotDupes )
		pRanker->SetTermDupes ( hQwords, iMaxQpos );
	if ( !pRanker->InitState ( tCtx, tMeta.m_sError ) )
		SafeDelete ( pRanker );
	return pRanker;
}

//////////////////////////////////////////////////////////////////////////
/// HIT MARKER
//////////////////////////////////////////////////////////////////////////

void CSphHitMarker::Mark ( CSphVector<SphHitMark_t> & dMarked )
{
	if ( !m_pRoot )
		return;

	const ExtHit_t * pHits = nullptr;
	const ExtDoc_t * pDocs = nullptr;

	pDocs = m_pRoot->GetDocsChunk();
	if ( !pDocs )
		return;

	pHits = m_pRoot->GetHits ( pDocs );
	for ( ; pHits->m_tRowID!=INVALID_ROWID; pHits++ )
	{
		SphHitMark_t tMark;
		tMark.m_uPosition = HITMAN::GetPosWithField ( pHits->m_uHitpos );
		tMark.m_uSpan = pHits->m_uMatchlen;

		dMarked.Add ( tMark );
	}
}


CSphHitMarker::~CSphHitMarker ()
{
	SafeDelete ( m_pRoot );
}


CSphHitMarker * CSphHitMarker::Create ( const XQNode_t * pRoot, const ISphQwordSetup & tSetup )
{
	ExtNode_i * pNode = nullptr;
	if ( pRoot )
		pNode = ExtNode_i::Create ( pRoot, tSetup, false );

	if ( !pNode )
		return nullptr;

	CSphHitMarker * pMarker = new CSphHitMarker;
	pMarker->m_pRoot = pNode;
	pMarker->m_pRoot->SetCollectHits();
	return pMarker;
}


CSphString sphXQNodeToStr ( const XQNode_t * pNode )
{
	static const char * szNodeNames[] =
	{
		"AND",
		"OR",
		"MAYBE",
		"NOT",
		"ANDNOT",
		"BEFORE",
		"PHRASE",
		"PROXIMITY",
		"QUORUM",
		"NEAR",
		"NOTNEAR",
		"SENTENCE",
		"PARAGRAPH"
	};

	if ( pNode->GetOp()>=SPH_QUERY_AND && pNode->GetOp()<=SPH_QUERY_PARAGRAPH )
		return szNodeNames [ pNode->GetOp()-SPH_QUERY_AND ];

	CSphString sTmp;
	sTmp.SetSprintf ( "OPERATOR-%d", pNode->GetOp() );
	return sTmp; 
}
