/* Copyright 2013 Stanford University
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


#ifndef __LEGION_OPERATIONS_H__
#define __LEGION_OPERATIONS_H__

#include "legion.h"
#include "region_tree.h"

namespace LegionRuntime {
  namespace HighLevel {

    // Special typedef for predicates
    typedef Predicate::Impl PredicateOp;

    /**
     * \class Operation
     * The operation class serves as the root of the tree
     * of all operations that can be performed in a Legion
     * program.
     */
    class Operation {
    public:
      Operation(Runtime *rt);
      virtual ~Operation(void);
    public:
      virtual void activate(void) = 0;
      virtual void deactivate(void) = 0; 
      virtual const char* get_logging_name(void) = 0;
    protected:
      // Base call
      void activate_operation(void);
      void deactivate_operation(void);
    public:
      inline GenerationID get_generation(void) const { return gen; }
      inline Event get_completion_event(void) const { return completion_event; }
      inline SingleTask* get_parent(void) const { return parent_ctx; }
      inline UniqueID get_unique_op_id(void) const { return unique_op_id; } 
    public:
      // Be careful using this call as it is only valid when the operation
      // actually has a parent task.  Right now the only place it is used
      // is in putting the operation in the right dependence queue which
      // we know happens on the home node and therefore the operations is
      // guaranteed to have a parent task.
      unsigned get_operation_depth(void) const; 
    public:
      void initialize_privilege_path(RegionTreePath &path,
                                     const RegionRequirement &req);
      void initialize_mapping_path(RegionTreePath &path,
                                   const RegionRequirement &req,
                                   LogicalRegion start_node);
      void initialize_mapping_path(RegionTreePath &path,
                                   const RegionRequirement &req,
                                   LogicalPartition start_node);
    public:
      // Localize a region requirement to its parent context
      // This means that region == parent and the
      // coherence mode is exclusive
      static void localize_region_requirement(RegionRequirement &req);
    public:
      // Initialize this operation in a new parent context
      // along with the number of regions this task has
      void initialize_operation(SingleTask *ctx, bool track,
                                unsigned num_regions = 0); 
      public:
      // The following two calls may be implemented
      // differently depending on the operation, but we
      // provide base versions of them so that operations
      // only have to overload the stages that they care
      // about modifying.
      // The function to call for depence analysis
      virtual void trigger_dependence_analysis(void);
      // The function to call when the operation is ready to map 
      // In general put this on the ready queue so the runtime
      // can invoke the trigger mapping call.
      virtual void trigger_mapping(void);
      // The function to call for executing an operation
      // Note that this one is not invoked by the Operation class
      // but by the runtime, therefore any operations must be
      // placed on the ready queue in order for the runtime to
      // perform this mapping
      virtual bool trigger_execution(void);
      // The function to trigger once speculation is
      // ready to be resolved
      virtual void trigger_resolution(void);
      // Helper function for deferring complete operations
      // (only used in a limited set of operations and not
      // part of the default pipeline)
      virtual void deferred_complete(void);
      // The function to call once the operation is ready to complete
      virtual void trigger_complete(void);
      // The function to call when commit the operation is
      // ready to commit
      virtual void trigger_commit(void);
    public:
      // The following are sets of calls that we can use to 
      // indicate mapping, execution, resolution, completion, and commit
      //
      // Indicate that we are done mapping this operation
      void complete_mapping(void); 
      // Indicate when this operation has finished executing
      void complete_execution(void);
      // Indicate when we have resolved the speculation for
      // this operation
      void resolve_speculation(void);
      // Indicate that we are completing this operation
      // which will also verify any regions for our producers
      void complete_operation(void);
      // Indicate that we are committing this operation
      void commit_operation(void);
      // Indicate that this operation is hardened against failure
      void harden_operation(void);
      // Quash this task and do what is necessary to the
      // rest of the operations in the graph
      void quash_operation(GenerationID gen, bool restart);
    public:
      // For operations that need to trigger commit early,
      // then they should use this call to avoid races
      // which could result in trigger commit being
      // called twice.  It will return true if the
      // caller is allowed to call trigger commit.
      bool request_early_commit(void);
    public:
      // Everything below here is implementation
      //
      // Call these two functions before and after
      // dependence analysis, they place a temporary
      // dependence on the operation so that it doesn't
      // prematurely trigger before the analysis is
      // complete.  The end call will trigger the
      // operation if it is complete
      void begin_dependence_analysis(void);
      void end_dependence_analysis(void);
      // Operations for registering dependences and
      // then notifying them when being woken up
      // This call will attempt to register a dependence
      // from the operation on which it is called to the target
      // Return true if the operation has committed and can be 
      // pruned out of the list of mapping dependences.
      bool register_dependence(Operation *target, GenerationID target_gen);
      // This is a special case of register dependence that will
      // also mark that we can verify a region produced by an earlier
      // operation so that operation can commit earlier.
      // Return true if the operation has committed and can be pruned
      // out of the list of dependences.
      bool register_region_dependence(Operation *target,
                              GenerationID target_gen, unsigned target_idx);
      // This method is invoked by one of the two above to perform
      // the registration.  Returns true if we have not yet commited
      // and should therefore be notified once the dependent operation
      // has committed or verified its regions.
      bool perform_registration(GenerationID our_gen, 
                                Operation *op, GenerationID op_gen,
                                bool &registered_dependence,
                                unsigned &op_mapping_deps,
                                unsigned &op_speculation_deps);
      // Add and remove mapping references to tell an operation
      // how many places additional dependences can come from.
      // Once the mapping reference count goes to zero, no
      // additional dependences can be registered.
      void add_mapping_reference(GenerationID gen);
      void remove_mapping_reference(GenerationID gen);
    public:
      // Notify when a mapping dependence is met (flows down edges)
      void notify_mapping_dependence(GenerationID gen);
      // Notify when a speculation dependence is met (flows down edges)
      void notify_speculation_dependence(GenerationID gen);
      // Notify when an operation has committed (flows up edges)
      void notify_commit_dependence(GenerationID gen);
      // Notify when a region from a dependent task has 
      // been verified (flows up edges)
      void notify_regions_verified(const std::set<unsigned> &regions,
                                   GenerationID gen);
    public:
      Runtime *const runtime;
    protected:
      Reservation op_lock;
      GenerationID gen;
      UniqueID unique_op_id;
      // Operations on which this operation depends
      std::map<Operation*,GenerationID> incoming;
      // Operations which depend on this operation
      std::map<Operation*,GenerationID> outgoing;
      // Number of outstanding mapping dependences before triggering map
      unsigned outstanding_mapping_deps;
      // Number of outstanding speculation dependences 
      unsigned outstanding_speculation_deps;
      // Number of outstanding commit dependences before triggering commit
      unsigned outstanding_commit_deps;
      // Number of outstanding mapping references, once this goes to 
      // zero then the set of outgoing edges is fixed
      unsigned outstanding_mapping_references;
      // The set of unverified regions
      std::set<unsigned> unverified_regions;
      // For each of our regions, a map of operations to the regions
      // which we can verify for each operation
      std::map<Operation*,std::set<unsigned> > verify_regions;
      // Whether this operation has mapped, once it has mapped then
      // the set of incoming dependences is fixed
      bool mapped;
      // Whether this task has executed or not
      bool executed;
      // Whether speculation for this operation has been resolved
      bool resolved;
      // Whether the physical instances for this region have been
      // hardened by copying them into reslient memories
      bool hardened;
      // Whether this operation has completed, cannot commit until
      // both completed is set, and outstanding mapping references
      // has been gone to zero.
      bool completed;
      // Some operations commit out of order and if they do then
      // commited is set to prevent any additional dependences from
      // begin registered.
      bool committed;
      // Track whether trigger mapped has been invoked
      bool trigger_mapping_invoked;
      // Track whether trigger resolution has been invoked
      bool trigger_resolution_invoked;
      // Track whether trigger complete has been invoked
      bool trigger_complete_invoked;
      // Track whether trigger_commit has already been invoked
      bool trigger_commit_invoked;
      // Indicate whether we are responsible for
      // triggering the completion event for this operation
      bool need_completion_trigger;
      // Are we tracking this operation in the parent's context
      bool track_parent;
      // The enclosing context for this operation
      SingleTask *parent_ctx;
      // The completion event for this operation
      UserEvent completion_event;
    };

    /**
     * \class Predicate::Impl 
     * A predicate operation is an abstract class that
     * contains a method that allows other operations to
     * sample their values and see if they are resolved
     * or whether they are speculated values.
     */
    class Predicate::Impl : public Operation {
    public:
      Impl(Runtime *rt);
    public:
      void add_reference(void);
      void remove_reference(void);
    public:
      virtual bool sample(bool &valid, bool &speculated) = 0;
      // Override the commit stage so we don't deactivate
      // predicates until they no longer need to be used
      virtual void trigger_commit(void);
    };

    /**
     * \class SpeculativeOp
     * A speculative operation is an abstract class
     * that serves as the basis for operation which
     * can be speculated on a predicate value.  They
     * will ask the predicate value for their value and
     * whether they have actually been resolved or not.
     * Based on that infomration the speculative operation
     * will decide how to manage the operation.
     */
    class SpeculativeOp : public Operation {
    public:
      enum SpecState {
        PENDING_MAP_STATE,
        PENDING_PRED_STATE,
        SPECULATE_TRUE_STATE,
        SPECULATE_FALSE_STATE,
        RESOLVE_TRUE_STATE,
        RESOLVE_FALSE_STATE,
      };
    public:
      SpeculativeOp(Runtime *rt);
    public:
      void activate_speculative(void);
      void deactivate_speculative(void);
    public:
      void initialize_speculation(SingleTask *ctx, bool track, 
                                  unsigned regions, const Predicate &p);
      bool is_predicated(void) const;
      // Wait until the predicate is valid and then return
      // its value.  Give it the current processor in case it
      // needs to wait for the value
      bool get_predicate_value(Processor proc);
    public:
      // Override the mapping call so we can decide whether
      // to continue mapping this operation or not 
      // depending on the value of the predicate operation.
      virtual void trigger_mapping(void);
      virtual void trigger_resolution(void);
      virtual void deferred_complete(void);
    public:
      // Call this method for inheriting classes 
      // to indicate when they should map
      virtual void continue_mapping(void) = 0;
    protected:
      SpecState    speculation_state;
      PredicateOp *predicate;
    };

    /**
     * \class MapOp
     * Mapping operations are used for computing inline mapping
     * operations.  Mapping operations will always update a
     * physical region once they have finished mapping.  They
     * then complete and commit immediately, possibly even
     * before the physical region is ready to be used.  This
     * also reflects that mapping operations cannot be rolled
     * back because once they have mapped, then information
     * has the ability to escape back to the application's
     * domain and can no longer be tracked by Legion.  Any
     * attempt to roll back an inline mapping operation
     * will result in the entire enclosing task context
     * being restarted.
     */
    class MapOp : public Inline, public Operation {
    public:
      MapOp(Runtime *rt);
      MapOp(const MapOp &rhs);
      virtual ~MapOp(void);
    public:
      MapOp& operator=(const MapOp &rhs);
    public:
      PhysicalRegion initialize(SingleTask *ctx,
                                const InlineLauncher &launcher,
                                bool check_privileges);
      PhysicalRegion initialize(SingleTask *ctx,
                                const RegionRequirement &req,
                                MapperID id, MappingTagID tag,
                                bool check_privileges);
      void initialize(SingleTask *ctx, const PhysicalRegion &region);
    public:
      virtual void activate(void);
      virtual void deactivate(void);
      virtual const char* get_logging_name(void);
    public:
      virtual void trigger_dependence_analysis(void);
      virtual bool trigger_execution(void);
    public:
      virtual MappableKind get_mappable_kind(void) const;
      virtual Task* as_mappable_task(void) const;
      virtual Copy* as_mappable_copy(void) const;
      virtual Inline* as_mappable_inline(void) const;
      virtual Acquire* as_mappable_acquire(void) const;
      virtual Release* as_mappable_release(void) const;
      virtual UniqueID get_unique_mappable_id(void) const;
    protected:
      void check_privilege(void);
    protected:
      bool remap_region;
      UserEvent termination_event;
      PhysicalRegion region;
      RegionTreePath privilege_path;
      RegionTreePath mapping_path;
    };

    /**
     * \class CopyOp
     * The copy operation provides a mechanism for applications
     * to directly copy data between pairs of fields possibly
     * from different region trees in an efficient way by
     * using the low-level runtime copy facilities. 
     */
    class CopyOp : public Copy, public SpeculativeOp {
    public:
      CopyOp(Runtime *rt);
      CopyOp(const CopyOp &rhs);
      virtual ~CopyOp(void);
    public:
      CopyOp& operator=(const CopyOp &rhs);
    public:
      void initialize(SingleTask *ctx,
                      const CopyLauncher &launcher,
                      bool check_privileges);
    public:
      virtual void activate(void);
      virtual void deactivate(void);
      virtual const char* get_logging_name(void);
    public:
      virtual void trigger_dependence_analysis(void);
      virtual void continue_mapping(void);
      virtual bool trigger_execution(void);
      virtual void deferred_complete(void);
    public:
      virtual MappableKind get_mappable_kind(void) const;
      virtual Task* as_mappable_task(void) const;
      virtual Copy* as_mappable_copy(void) const;
      virtual Inline* as_mappable_inline(void) const;
      virtual Acquire* as_mappable_acquire(void) const;
      virtual Release* as_mappable_release(void) const;
      virtual UniqueID get_unique_mappable_id(void) const;
    protected:
      void check_copy_privilege(const RegionRequirement &req, 
                                unsigned idx, bool src);
    public:
      std::vector<RegionTreePath> src_privilege_paths;
      std::vector<RegionTreePath> dst_privilege_paths;
      std::vector<RegionTreePath> src_mapping_paths; 
      std::vector<RegionTreePath> dst_mapping_paths;
    };

    /**
     * \class FenceOp
     * Fence operations give the application the ability to
     * enforce ordering guarantees between different tasks
     * in the same context which may become important when
     * certain updates to the region tree are desired to be
     * observed before a later operation either maps or 
     * runs.  To support these two kinds of guarantees, we
     * provide both mapping and executing fences.
     */
    class FenceOp : public Operation {
    public:
      FenceOp(Runtime *rt);
      FenceOp(const FenceOp &rhs);
      virtual ~FenceOp(void);
    public:
      FenceOp& operator=(const FenceOp &rhs);
    public:
      void initialize(SingleTask *ctx, bool mapping);
    public:
      virtual void activate(void);
      virtual void deactivate(void);
      virtual const char* get_logging_name(void);
    public:
      virtual void trigger_dependence_analysis(void);
      virtual bool trigger_execution(void);
      virtual void deferred_complete(void);
    protected:
      bool mapping_fence;
    };

    /**
     * \class DeletionOp
     * In keeping with the deferred execution model, deletions
     * must be deferred until all other operations that were
     * issued earlier are done using the regions that are
     * going to be deleted.  Deletion operations defer deletions
     * until they are safe to be committed.
     */
    class DeletionOp : public Operation {
    public:
      enum DeletionKind {
        INDEX_SPACE_DELETION,
        INDEX_PARTITION_DELETION,
        FIELD_SPACE_DELETION,
        FIELD_DELETION,
        LOGICAL_REGION_DELETION,
        LOGICAL_PARTITION_DELETION,
      };
    public:
      DeletionOp(Runtime *rt);
      DeletionOp(const DeletionOp &rhs);
      virtual ~DeletionOp(void);
    public:
      DeletionOp& operator=(const DeletionOp &rhs);
    public:
      void initialize_index_space_deletion(SingleTask *ctx, IndexSpace handle);
      void initialize_index_part_deletion(SingleTask *ctx,
                                          IndexPartition handle);
      void initialize_field_space_deletion(SingleTask *ctx,
                                           FieldSpace handle);
      void initialize_field_deletion(SingleTask *ctx, FieldSpace handle,
                                      FieldID fid);
      void initialize_field_deletions(SingleTask *ctx, FieldSpace handle,
                                      const std::set<FieldID> &to_free);
      void initialize_logical_region_deletion(SingleTask *ctx, 
                                              LogicalRegion handle);
      void initialize_logical_partition_deletion(SingleTask *ctx, 
                                                 LogicalPartition handle);
    public:
      virtual void activate(void);
      virtual void deactivate(void);
      virtual const char* get_logging_name(void);
    public:
      virtual void trigger_dependence_analysis(void);
      virtual void trigger_commit(void);
    protected:
      DeletionKind kind;
      IndexSpace index_space;
      IndexPartition index_part;
      FieldSpace field_space;
      LogicalRegion logical_region;
      LogicalPartition logical_part;
      std::set<FieldID> free_fields;
    }; 

    /**
     * \class CloseOp
     * Close operations are only visible internally inside
     * the runtime and are issued to help close up the 
     * physical region tree states to an existing physical
     * instance that a task context initially mapped.
     */
    class CloseOp : public Operation {
    public:
      CloseOp(Runtime *rt);
      CloseOp(const CloseOp &rhs);
      virtual ~CloseOp(void);
    public:
      CloseOp& operator=(const CloseOp &rhs);
    public:
      void initialize(SingleTask *ctx, unsigned index, 
                      const InstanceRef &reference);
    public:
      virtual void activate(void);
      virtual void deactivate(void);
      virtual const char* get_logging_name(void);
    public:
      virtual void trigger_dependence_analysis(void);
      virtual bool trigger_execution(void);
      virtual void deferred_complete(void);
    protected:
      RegionRequirement requirement;
      InstanceRef reference;
      RegionTreePath privilege_path;
#ifdef DEBUG_HIGH_LEVEL
      unsigned parent_index;
#endif
    };

    /**
     * \class AcquireOp
     * Acquire operations are used for performing
     * user-level software coherence when tasks own
     * regions with simultaneous coherence.
     */
    class AcquireOp : public Acquire, public SpeculativeOp {
    public:
      AcquireOp(Runtime *rt);
      AcquireOp(const AcquireOp &rhs);
      virtual ~AcquireOp(void);
    public:
      AcquireOp& operator=(const AcquireOp &rhs);
    public:
      void initialize(SingleTask *ctx, const AcquireLauncher &launcher,
                      bool check_privileges);
    public:
      virtual void activate(void);
      virtual void deactivate(void);
      virtual const char* get_logging_name(void); 
    public:
      virtual void trigger_dependence_analysis(void);
      virtual bool trigger_execution(void);
      virtual void continue_mapping(void); 
      virtual void deferred_complete(void);
    public:
      virtual MappableKind get_mappable_kind(void) const;
      virtual Task* as_mappable_task(void) const;
      virtual Copy* as_mappable_copy(void) const;
      virtual Inline* as_mappable_inline(void) const;
      virtual Acquire* as_mappable_acquire(void) const;
      virtual Release* as_mappable_release(void) const;
      virtual UniqueID get_unique_mappable_id(void) const;
    public:
      const RegionRequirement& get_requirement(void) const;
    protected:
      void check_acquire_privilege(void);
    protected:
      RegionRequirement requirement;
      RegionTreePath    privilege_path;
#ifdef DEBUG_HIGH_LEVEL
      RegionTreePath    mapping_path;
#endif
    };

    /**
     * \class ReleaseOp
     * Release operations are used for performing
     * user-level software coherence when tasks own
     * regions with simultaneous coherence.
     */
    class ReleaseOp : public Release, public SpeculativeOp {
    public:
      ReleaseOp(Runtime *rt);
      ReleaseOp(const ReleaseOp &rhs);
      virtual ~ReleaseOp(void);
    public:
      ReleaseOp& operator=(const ReleaseOp &rhs);
    public:
      void initialize(SingleTask *ctx, const ReleaseLauncher &launcher,
                      bool check_privileges);
    public:
      virtual void activate(void);
      virtual void deactivate(void);
      virtual const char* get_logging_name(void);
    public:
      virtual void trigger_dependence_analysis(void);
      virtual bool trigger_execution(void);
      virtual void continue_mapping(void); 
      virtual void deferred_complete(void);
    public:
      virtual MappableKind get_mappable_kind(void) const;
      virtual Task* as_mappable_task(void) const;
      virtual Copy* as_mappable_copy(void) const;
      virtual Inline* as_mappable_inline(void) const;
      virtual Acquire* as_mappable_acquire(void) const;
      virtual Release* as_mappable_release(void) const;
      virtual UniqueID get_unique_mappable_id(void) const;
    public:
      const RegionRequirement& get_requirement(void) const;
    protected:
      void check_release_privilege(void);
    protected:
      RegionRequirement requirement;
      RegionTreePath    privilege_path;
#ifdef DEBUG_HIGH_LEVEL
      RegionTreePath    mapping_path;
#endif
    };

    /**
     * \class FuturePredOp
     * A class for making predicates out of futures.
     */
    class FuturePredOp : public Predicate::Impl {
    public:
      FuturePredOp(Runtime *rt);
      FuturePredOp(const FuturePredOp &rhs);
      virtual ~FuturePredOp(void);
    public:
      FuturePredOp& operator=(const FuturePredOp &rhs);
    public:
      void initialize(Future f, Processor proc);
      void speculate(void);
    public:
      virtual void activate(void);
      virtual void deactivate(void);
      const char* get_logging_name(void);
    public:
      virtual bool sample(bool &valid, bool &speculated);
    protected:
      Future future;
      Processor proc;
      bool try_speculated;
      protected: 
      bool pred_valid;
      bool pred_speculated;
      bool pred_value;
    };

    /**
     * \class NotPredOp
     * A class for negating other predicates
     */
    class NotPredOp : public Predicate::Impl {
    public:
      NotPredOp(Runtime *rt);
      NotPredOp(const NotPredOp &rhs);
      virtual ~NotPredOp(void);
    public:
      NotPredOp& operator=(const NotPredOp &rhs);
    public:
      void initialize(const Predicate &p);
    public:
      virtual void activate(void);
      virtual void deactivate(void);
      virtual const char* get_logging_name(void);
    public:
      virtual bool sample(bool &valid, bool &speculated);
    protected:
      PredicateOp *pred_op;
    protected: 
      bool pred_valid;
      bool pred_speculated;
      bool pred_value;
    };

    /**
     * \class AndPredOp
     * A class for and-ing other predicates
     */
    class AndPredOp : public Predicate::Impl {
    public:
      AndPredOp(Runtime *rt);
      AndPredOp(const AndPredOp &rhs);
      virtual ~AndPredOp(void);
    public:
      AndPredOp& operator=(const AndPredOp &rhs);
    public:
      void initialize(const Predicate &p1, const Predicate &p2);
    public:
      virtual void activate(void);
      virtual void deactivate(void);
      virtual const char* get_logging_name(void);
    public:
      virtual bool sample(bool &valid, bool &speculated);
    protected:
      PredicateOp *pred0;
      PredicateOp *pred1;
    protected:
      bool zero_valid;
      bool zero_speculated;
      bool zero_value;
    protected:
      bool one_valid;
      bool one_speculated;
      bool one_value;
    };

    /**
     * \class OrPredOp
     * A class for or-ing other predicates
     */
    class OrPredOp : public Predicate::Impl {
    public:
      OrPredOp(Runtime *rt);
      OrPredOp(const OrPredOp &rhs);
      virtual ~OrPredOp(void);
    public:
      OrPredOp& operator=(const OrPredOp &rhs);
    public:
      void initialize(const Predicate &p1, const Predicate &p2);
    public:
      virtual void activate(void);
      virtual void deactivate(void);
      virtual const char* get_logging_name(void);
    public:
      virtual bool sample(bool &valid, bool &speculated);
    protected:
      PredicateOp *pred0;
      PredicateOp *pred1;
    protected:
      bool zero_valid;
      bool zero_speculated;
      bool zero_value;
    protected:
      bool one_valid;
      bool one_speculated;
      bool one_value;
    };

  }; //namespace HighLevel
}; // namespace LegionRuntime

#endif // __LEGION_OPERATIONS_H__
