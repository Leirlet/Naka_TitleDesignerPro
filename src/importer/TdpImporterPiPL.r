// NAKA Title Designer Pro - PiPL (converted by CnvtPiPL at build time)

#ifndef PRWIN_ENV
#include <CoreServices/CoreServices.r>
#define MAC_ENV
#include "PrSDKPiPLVer.h"
#include "PrSDKPiPL.r"
#endif

// Shown in File > New. Must match PIPL_NAME_UTF in tdp_importer.cpp.
#define plugInName		"NAKA Title Designer Pro"

// Internal name, never change.
#define plugInMatchName	"NAKAfx_TitleDesignerPro"

resource 'PiPL' (16000)
{
	{
		Kind {PrImporter},
		Name {plugInName},
		AE_Effect_Match_Name {plugInMatchName}
	}
};

type 'IMPT'
{
	longint;
};

resource 'IMPT' (1000)
{
	0x0000
};
