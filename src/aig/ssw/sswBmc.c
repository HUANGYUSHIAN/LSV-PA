/**CFile****************************************************************

  FileName    [sswBmc.c]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [Inductive prover with constraints.]

  Synopsis    [Bounded model checker for equivalence clases.]

  Author      [Alan Mishchenko]
  
  Affiliation [UC Berkeley]

  Date        [Ver. 1.0. Started - September 1, 2008.]

  Revision    [$Id: sswBmc.c,v 1.00 2008/09/01 00:00:00 alanmi Exp $]

***********************************************************************/

#include "sswInt.h"

////////////////////////////////////////////////////////////////////////
///                        DECLARATIONS                              ///
////////////////////////////////////////////////////////////////////////

typedef struct Ssw_Bmc_t_ Ssw_Bmc_t; // BMC manager

struct Ssw_Bmc_t_
{
    // parameters
    int              nConfMaxStart;  // the starting conflict limit
    int              nConfMax;       // the intermediate conflict limit
    int              nFramesSweep;   // the number of frames to sweep
    int              fVerbose;       // prints output statistics
    // equivalences considered
    Ssw_Man_t *      pMan;           // SAT sweeping manager
    Vec_Ptr_t *      vTargets;       // the nodes that are watched
    // storage for patterns
    int              nPatternsAlloc; // the max number of interesting states
    int              nPatterns;      // the number of patterns
    Vec_Ptr_t *      vPatterns;      // storage for the interesting states
    Vec_Int_t *      vHistory;       // what state and how many steps
};

////////////////////////////////////////////////////////////////////////
///                     FUNCTION DEFINITIONS                         ///
////////////////////////////////////////////////////////////////////////

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
Ssw_Bmc_t * Ssw_BmcManStart( Ssw_Man_t * pMan, int nConfMax, int fVerbose )
{
    Ssw_Bmc_t * p;
    Aig_Obj_t * pObj;
    int i;
    // create interpolation manager
    p = ALLOC( Ssw_Bmc_t, 1 ); 
    memset( p, 0, sizeof(Ssw_Bmc_t) );
    p->nConfMaxStart  = nConfMax;
    p->nConfMax       = nConfMax;
    p->nFramesSweep   = AIG_MAX( (1<<21)/Aig_ManNodeNum(pMan->pAig), pMan->nFrames );
    p->fVerbose       = fVerbose;
    // equivalences considered
    p->pMan           = pMan;
    p->vTargets       = Vec_PtrAlloc( Saig_ManPoNum(p->pMan->pAig) );
    Saig_ManForEachPo( p->pMan->pAig, pObj, i )
        Vec_PtrPush( p->vTargets, Aig_ObjFanin0(pObj) );
    // storage for patterns
    p->nPatternsAlloc = 512;
    p->nPatterns      = 1;
    p->vPatterns      = Vec_PtrAllocSimInfo( Aig_ManRegNum(p->pMan->pAig), Aig_BitWordNum(p->nPatternsAlloc) );
    Vec_PtrCleanSimInfo( p->vPatterns, 0, Aig_BitWordNum(p->nPatternsAlloc) );
    p->vHistory       = Vec_IntAlloc( 100 );
    Vec_IntPush( p->vHistory, 0 );
    // update arrays of the manager
    free( p->pMan->pNodeToFrames );
    Vec_IntFree( p->pMan->vSatVars );
    p->pMan->pNodeToFrames = CALLOC( Aig_Obj_t *, Aig_ManObjNumMax(p->pMan->pAig) * p->nFramesSweep );
    p->pMan->vSatVars      = Vec_IntStart( Aig_ManObjNumMax(p->pMan->pAig) * (p->nFramesSweep+1) );
    return p;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Ssw_BmcManStop( Ssw_Bmc_t * p )
{
    Vec_PtrFree( p->vTargets );
    Vec_PtrFree( p->vPatterns );
    Vec_IntFree( p->vHistory );
    free( p );
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Ssw_BmcCheckTargets( Ssw_Bmc_t * p )
{
    Aig_Obj_t * pObj;
    int i;
    Vec_PtrForEachEntry( p->vTargets, pObj, i )
        if ( !Ssw_ObjIsConst1Cand(p->pMan->pAig, pObj) )
            return 1;
    return 0;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Ssw_ManFilterBmcSavePattern( Ssw_Bmc_t * p )
{
    unsigned * pInfo;
    Aig_Obj_t * pObj;
    int i;
    if ( p->nPatterns >= p->nPatternsAlloc )
        return;
    Saig_ManForEachLo( p->pMan->pAig, pObj, i )
    {
        pInfo = Vec_PtrEntry( p->vPatterns, i );
        if ( Aig_InfoHasBit( p->pMan->pPatWords, Saig_ManPiNum(p->pMan->pAig) + i ) )
            Aig_InfoSetBit( pInfo, p->nPatterns );
    }
    p->nPatterns++;
}

/**Function*************************************************************

  Synopsis    [Performs fraiging for the internal nodes.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Ssw_ManFilterBmc( Ssw_Bmc_t * pBmc, int iPat, int fCheckTargets )
{
    Ssw_Man_t * p = pBmc->pMan;
    Aig_Obj_t * pObj, * pObjNew, * pObjLi, * pObjLo;
    unsigned * pInfo;
    int i, f, clk, RetValue, fFirst = 0;
clk = clock();

    // start initialized timeframes
    p->pFrames = Aig_ManStart( Aig_ManObjNumMax(p->pAig) * 3 );
    Saig_ManForEachLo( p->pAig, pObj, i )
    {
        pInfo = Vec_PtrEntry( pBmc->vPatterns, i );
        pObjNew = Aig_NotCond( Aig_ManConst1(p->pFrames), !Aig_InfoHasBit(pInfo, iPat) );
        Ssw_ObjSetFrame( p, pObj, 0, pObjNew );
    }

    // sweep internal nodes
    Ssw_ManStartSolver( p );
    RetValue = pBmc->nFramesSweep;
    for ( f = 0; f < pBmc->nFramesSweep; f++ )
    {
        // map constants and PIs
        Ssw_ObjSetFrame( p, Aig_ManConst1(p->pAig), f, Aig_ManConst1(p->pFrames) );
        Saig_ManForEachPi( p->pAig, pObj, i )
            Ssw_ObjSetFrame( p, pObj, f, Aig_ObjCreatePi(p->pFrames) );
        // sweep internal nodes
        Aig_ManForEachNode( p->pAig, pObj, i )
        {
            pObjNew = Aig_And( p->pFrames, Ssw_ObjChild0Fra(p, pObj, f), Ssw_ObjChild1Fra(p, pObj, f) );
            Ssw_ObjSetFrame( p, pObj, f, pObjNew );
            if ( Ssw_ManSweepNode( p, pObj, f, 1 ) )
            {
                Ssw_ManFilterBmcSavePattern( pBmc );
                if ( fFirst == 0 )
                {
                    fFirst = 1;
                    pBmc->nConfMax *= 10;
                } 
            }
            if ( f > 0 && p->pSat->stats.conflicts >= pBmc->nConfMax )
            {
                RetValue = -1;
                break;
            }
        }
        // quit if this is the last timeframe
        if ( p->pSat->stats.conflicts >= pBmc->nConfMax )
        {
            RetValue += f + 1;
            break;
        }
        if ( fCheckTargets && Ssw_BmcCheckTargets( pBmc ) )
            break;
        // transfer latch input to the latch outputs 
        // build logic cones for register outputs
        Saig_ManForEachLiLo( p->pAig, pObjLi, pObjLo, i )
        {
            pObjNew = Ssw_ObjChild0Fra(p, pObjLi,f);
            Ssw_ObjSetFrame( p, pObjLo, f+1, pObjNew );
            Ssw_CnfNodeAddToSolver( p, Aig_Regular(pObjNew) );
        }
//printf( "Frame %2d : Conflicts = %6d. \n", f, p->pSat->stats.conflicts );
    }
    if ( fFirst )
        pBmc->nConfMax /= 10;

    // cleanup
    Ssw_ClassesCheck( p->ppClasses );
p->timeBmc += clock() - clk;
    return RetValue;
}

/**Function*************************************************************

  Synopsis    [Returns 1 if one of the targets has failed.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Ssw_FilterUsingBmc( Ssw_Man_t * pMan, int fCheckTargets, int nConfMax, int fVerbose )
{
    Ssw_Bmc_t * p;
    int RetValue, Frames, Iter, clk = clock();
    p = Ssw_BmcManStart( pMan, nConfMax, fVerbose );
    if ( fCheckTargets && Ssw_BmcCheckTargets( p ) )
    {
        assert( 0 );
        Ssw_BmcManStop( p );
        return 1;
    }
    if ( fVerbose )
    {
        printf( "AIG : Const = %6d. Cl = %6d. Nodes = %6d.  ConfMax = %6d. FramesMax = %6d.\n", 
            Ssw_ClassesCand1Num(p->pMan->ppClasses), Ssw_ClassesClassNum(p->pMan->ppClasses), 
            Aig_ManNodeNum(p->pMan->pAig), p->nConfMax, p->nFramesSweep );
    } 
    RetValue = 0;
    for ( Iter = 0; Iter < p->nPatterns; Iter++ )
    {
clk = clock();
        Frames = Ssw_ManFilterBmc( p, Iter, fCheckTargets );
        if ( fVerbose )
        {
            printf( "%3d : Const = %6d. Cl = %6d. NR = %6d. F = %3d. C = %5d. P = %3d. %s ", 
                Iter, Ssw_ClassesCand1Num(p->pMan->ppClasses), Ssw_ClassesClassNum(p->pMan->ppClasses), 
                Aig_ManNodeNum(p->pMan->pFrames), Frames, (int)p->pMan->pSat->stats.conflicts, p->nPatterns, 
                p->pMan->nSatFailsReal? "f" : " " );
            PRT( "T", clock() - clk );
        } 
        Ssw_ManCleanup( p->pMan );
        if ( fCheckTargets && Ssw_BmcCheckTargets( p ) )
        {
            printf( "Target is hit!!!\n" );
            RetValue = 1;
        }
        if ( p->nPatterns >= p->nPatternsAlloc )
            break;
    }
    Ssw_BmcManStop( p );

    pMan->nStrangers = 0;
    pMan->nSatCalls = 0;
    pMan->nSatProof = 0;
    pMan->nSatFailsReal = 0; 
    pMan->nSatFailsTotal = 0;
    pMan->nSatCallsUnsat = 0;
    pMan->nSatCallsSat = 0;  
    pMan->timeSimSat = 0;    
    pMan->timeSat = 0;       
    pMan->timeSatSat = 0;    
    pMan->timeSatUnsat = 0;  
    pMan->timeSatUndec = 0;  
    return RetValue;
}

////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////


