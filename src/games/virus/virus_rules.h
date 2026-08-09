#pragma once
#include "virus_api.h"

// ============================================================================
//  The four viruses you can bring to a match. Edit their bodies in
//  virus_rules.cpp. Any you leave alone keep playing the default behaviour, so
//  you can start by editing just decideA and pitting it against the rest.
// ============================================================================
Decision decideA(const Cell& me, const World& world);
Decision decideB(const Cell& me, const World& world);
Decision decideC(const Cell& me, const World& world);
Decision decideD(const Cell& me, const World& world);

// Lookup table the referee uses: VIRUS_RULES[0]=decideA .. [3]=decideD.
extern RuleFn VIRUS_RULES[4];
