// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file defines structs to accumulate memory allocation and deallocation
// counts.  These structs are commonly used for malloc (in HeapProfileTable)
// and mmap (in MemoryRegionMap).

#ifndef HEAP_PROFILE_STATS_H_
#define HEAP_PROFILE_STATS_H_

struct HeapProfileStats {
  // Returns true if the two HeapProfileStats are semantically equal.
  bool Equivalent(const HeapProfileStats& x) const {
    return allocs - frees == x.allocs - x.frees &&
      alloc_size - free_size == x.alloc_size - x.free_size;
  }

  int32 allocs;      // Number of allocation calls.
  int32 frees;       // Number of free calls.
  int64 alloc_size;  // Total size of all allocated objects so far.
  int64 free_size;   // Total size of all freed objects so far.
};

// Allocation and deallocation statistics per each stack trace.
struct HeapProfileBucket : public HeapProfileStats {
  // Longest stack trace we record.
  static const int kMaxStackDepth = 32;

  uintptr_t hash;           // Hash value of the stack trace.
  int depth;                // Depth of stack trace.
  const void** stack;       // Stack trace.
  HeapProfileBucket* next;  // Next entry in hash-table.
};

#endif  // HEAP_PROFILE_STATS_H_
