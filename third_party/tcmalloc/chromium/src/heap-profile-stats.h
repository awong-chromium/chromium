// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file defines structs to accumulate memory allocation and deallocation
// counts.  These structs are commonly used for malloc (in HeapProfileTable)
// and mmap (in MemoryRegionMap).

// A bucket is data structure for heap profiling to store a pair of a stack
// trace and counts of allocation and deallocation.  Buckets are stored in a
// hash table which is declared as HeapProfileBucket**.
//
// A hash value is computed from a stack trace.  Collision in the hash table
// is resolved by separate chaining with linked lists.
//
// For Bucket** bucket_table,
// bucket[0] => NULL
// bucket[1] => HeapProfileBucket() => HeapProfileBucket() => NULL
// ...
// bucket[i] => HeapProfileBucket() => NULL
// ...

#ifndef HEAP_PROFILE_STATS_H_
#define HEAP_PROFILE_STATS_H_

struct HeapProfileStats {
  // Returns true if the two HeapProfileStats are semantically equal.
  bool Equivalent(const HeapProfileStats& other) const {
    return allocs - frees == other.allocs - other.frees &&
        alloc_size - free_size == other.alloc_size - other.free_size;
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
