/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Copyright (c) 2024, Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2024, The Constellation Project
 * All rights reserved.
 *
 * This is free software.  You are permitted to use, redistribute,
 * and modify it as specified in the file "LICENSE".
 */


#include <alaska/alaska.hpp>
#include <alaska/sim/HTLB.hpp>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iomanip>
#include <vector>

using namespace alaska::sim;

static HTLB *the_htlb = nullptr;


#define L1_ENTS 16
#define L1_SETS 4
#define L1_WAYS (L1_ENTS / L1_SETS)

#define L2_WAYS 8
#define L2_SETS 64
#define TOTAL_ENTRIES (L1_WAYS * L1_SETS + L2_WAYS * L2_SETS)

HTLB::HTLB()
    : htlb(L1_SETS, L1_WAYS, L2_SETS, L2_WAYS)
    , tlb(4, 4, 8, 8)
    // , tlb(16, 4, 128, 8)
    // , tlb(1, 1, 1, 1)
    , dcache(32, 8, 256, 16) {
  the_htlb = this;
}

HTLB *HTLB::get() {
  return the_htlb; }




#undef MASK
