/* Copyright 2020 Stanford University, NVIDIA Corporation
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

// Realm inter-node networking abstractions

// NOP but helpful for IDEs
#include "realm/network.h"

namespace Realm {

  namespace Network {

    // gets the network for a given node
    inline NetworkModule *get_network(NodeID node)
    {
#ifdef REALM_USE_MULTIPLE_NETWORKS
      if(__builtin_expect(single_network == 0, 0)) {
      } else
#endif
	return single_network;
    }

    inline void barrier(void)
    {
#ifdef REALM_USE_MULTIPLE_NETWORKS
      if(__builtin_expect(single_network == 0, 0)) {
      } else
#endif
	single_network->barrier();
    }

    // collective communication across all nodes (TODO: subcommunicators?)
    template <typename T>
    inline T broadcast(NodeID root, T val)
    {
      T bval;
      broadcast(root, &val, &bval, sizeof(T));
      return bval;
    }

    template <typename T>
    inline void gather(NodeID root, T val, std::vector<T>& result)
    {
      result.resize(max_node_id + 1);
      gather(root, &val, &result[0], sizeof(T));
    }

    template <typename T>
    inline void gather(NodeID root, T val)  // for non-root participants
    {
      gather(root, &val, 0, sizeof(T));
    }
    
    inline void broadcast(NodeID root,
			  const void *val_in, void *val_out, size_t bytes)
    {
#ifdef REALM_USE_MULTIPLE_NETWORKS
      if(__builtin_expect(single_network == 0, 0)) {
      } else
#endif
	single_network->broadcast(root, val_in, val_out, bytes);
    }
    
    inline void gather(NodeID root,
		       const void *val_in, void *vals_out, size_t bytes)
    {
#ifdef REALM_USE_MULTIPLE_NETWORKS
      if(__builtin_expect(single_network == 0, 0)) {
      } else
#endif
	single_network->gather(root, val_in, vals_out, bytes);
    }
    
    inline ActiveMessageImpl *create_active_message_impl(NodeID target,
							 unsigned short msgid,
							 size_t header_size,
							 size_t max_payload_size,
							 void *dest_payload_addr,
							 void *storage_base,
							 size_t storage_size)
    {
#ifdef REALM_USE_MULTIPLE_NETWORKS
      if(__builtin_expect(single_network == 0, 0)) {
      } else
#endif
	return single_network->create_active_message_impl(target,
							  msgid,
							  header_size,
							  max_payload_size,
							  dest_payload_addr,
							  storage_base,
							  storage_size);
    }

    inline ActiveMessageImpl *create_active_message_impl(const NodeSet& targets,
							 unsigned short msgid,
							 size_t header_size,
							 size_t max_payload_size,
							 void *storage_base,
							 size_t storage_size)
    {
#ifdef REALM_USE_MULTIPLE_NETWORKS
      if(__builtin_expect(single_network == 0, 0)) {
      } else
#endif
	return single_network->create_active_message_impl(targets,
							  msgid,
							  header_size,
							  max_payload_size,
							  storage_base,
							  storage_size);
    }
    
  };

  ////////////////////////////////////////////////////////////////////////
  //
  // class NetworkSegment
  //

  inline NetworkSegment::NetworkSegment()
    : base(0), bytes(0), alignment(0)
    , single_network(0), single_network_data(0)
  {}
    
  // normally a request will just be for a particular size
  inline NetworkSegment::NetworkSegment(size_t _bytes, size_t _alignment)
    : base(0), bytes(_bytes), alignment(_alignment)
    , single_network(0), single_network_data(0)
  {}

  // but it can also be for a pre-allocated chunk of memory with a fixed address
  inline NetworkSegment::NetworkSegment(void *_base, size_t _bytes)
    : base(_base), bytes(_bytes), alignment(0)
    , single_network(0), single_network_data(0)
  {}
  
  inline void NetworkSegment::request(size_t _bytes, size_t _alignment)
  {
    bytes = _bytes;
    alignment = _alignment;
  }

  inline void NetworkSegment::assign(void *_base, size_t _bytes)
  {
    base = _base;
    bytes = _bytes;
  }


};
