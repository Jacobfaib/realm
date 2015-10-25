/* Copyright 2015 Stanford University, NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// sparsity maps for Realm

// NOTE: SparsityMap's are not intended to be manipulated directly by Realm
//  applications (including higher-level runtimes), but they make heavy use of
//  templating and inlining for performance, so the headers are "reachable" from
//  the external parts of the Realm API

#ifndef REALM_SPARSITY_H
#define REALM_SPARSITY_H

#include "indexspace.h"

namespace Realm {

  template <int N, typename T /*= int*/> struct ZRect;
  template <int N, typename T = int> class HierarchicalBitMap;

  // a SparsityMap is a Realm handle to sparsity data for one or more index spaces - all
  //  SparsityMap's use the same ID namespace (i.e. regardless of N and T), but the
  //  template parameters are kept to avoid losing dimensionality information upon iteration/etc.

  // there are three layers to the SparsityMap onion:
  // a) SparsityMap is the Realm handle that (like all other handles) can be copied/stored
  //     whereever and represents a name for a distributed object with a known "creator" node,
  //     with valid data for the object existing on one or more nodes (which may or may not
  //     include the creator node) - methods on this "object" simply forward to the actual
  //     implementation object, described next
  // b) SparsityMapPublicImpl is the public subset of the storage and functionality of the actual
  //     sparsity map implementation - this should be sufficient for all the needs of user code,
  //     but not for Realm internals (e.g. the part that actually computes new sparsity maps) -
  //     these objects are not allocated directly
  // c) SparsityMapImpl is the actual dynamically allocated object that exists on each "interested"
  //     node for a given SparsityMap - it inherits from SparsityMapPublicImpl and adds the "private"
  //     storage and functionality - this separation is primarily to avoid the installed version of
  //     of Realm having to include all the internal .h files

  template <int N, typename T> class SparsityMapPublicImpl;

  template <int N, typename T>
  class SparsityMap {
  public:
    typedef ::legion_lowlevel_id_t id_t;
    id_t id;
    bool operator<(const SparsityMap<N,T> &rhs) const;
    bool operator==(const SparsityMap<N,T> &rhs) const;
    bool operator!=(const SparsityMap<N,T> &rhs) const;

    //static const SparsityMap<N,T> NO_SPACE;

    bool exists(void) const;

    // looks up the public subset of the implementation object
    SparsityMapPublicImpl<N,T> *impl(void) const;
  };

  template <int N, typename T>
  inline std::ostream& operator<<(std::ostream& os, SparsityMap<N,T> s) { return os << std::hex << s.id << std::dec; }

  template <int N, typename T>
  struct SparsityMapEntry {
    ZRect<N,T> bounds;
    SparsityMap<N,T> sparsity;
    HierarchicalBitMap<N,T> *bitmap;
  };

  template <int N, typename T>
  class SparsityMapPublicImpl {
  protected:
    // cannot be constructed directly
    SparsityMapPublicImpl(void);

  public:
    // application side code should only ever look at "completed" sparsity maps (i.e. ones
    //  that have reached their steady-state immutable value - this is computed in a deferred
    //  fashion and fetched by other nodes on demand, so the application code needs to call
    //  make_valid() before attempting to use the contents and either wait on the event or 
    //  otherwise defer the actual use until the event has triggered
    Event make_valid(void);

    // a sparsity map entry is similar to an IndexSpace - it's a rectangle and optionally a
    //  reference to another SparsityMap OR a pointer to a HierarchicalBitMap, which is a 
    //  dense array of bits describing the validity of each point in the rectangle

    std::vector<SparsityMapEntry<N,T> > entries;
  };

}; // namespace Realm

#include "sparsity.inl"

#endif // ifndef REALM_SPARSITY_H

