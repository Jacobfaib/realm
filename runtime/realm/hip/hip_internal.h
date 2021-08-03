/* Copyright 2021 Stanford University, NVIDIA Corporation
 *                Los Alamos National Laboratory
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

#ifndef REALM_HIP_INTERNAL_H
#define REALM_HIP_INTERNAL_H

#include <hip/hip_runtime.h>
#ifdef __HIP_PLATFORM_NVCC__
#define hipDeviceScheduleBlockingSync CU_CTX_SCHED_BLOCKING_SYNC 
typedef CUdeviceptr hipDeviceCharptr_t;
#else
typedef char* hipDeviceCharptr_t;
#endif

#include "realm/realm_config.h"
#include "realm/operation.h"
#include "realm/threads.h"
#include "realm/circ_queue.h"
#include "realm/indexspace.h"
#include "realm/proc_impl.h"
#include "realm/mem_impl.h"
#include "realm/bgwork.h"
#include "realm/transfer/channel.h"

#define CHECK_CUDART(cmd) do { \
  hipError_t ret = (cmd); \
  if(ret != hipSuccess) { \
    fprintf(stderr, "CUDART: %s = %d (%s)\n", #cmd, ret, hipGetErrorString(ret)); \
    assert(0); \
    exit(1); \
  } \
} while(0)
  
#define REPORT_CU_ERROR(cmd, ret) \
  do { \
    const char *name, *str; \
    name = hipGetErrorName(ret); \
    str = hipGetErrorString(ret); \
    fprintf(stderr, "CU: %s = %d (%s): %s\n", cmd, ret, name, str); \
    abort(); \
  } while(0)

#define CHECK_CU(cmd) do {                      \
  hipError_t ret = (cmd); \
  if(ret != hipSuccess) REPORT_CU_ERROR(#cmd, ret); \
} while(0)


namespace Realm {
  
  namespace Hip {

    struct GPUInfo {
      int index;  // index used by HIP runtime
      hipDevice_t device;

      static const size_t MAX_NAME_LEN = 64;
      char name[MAX_NAME_LEN];

      int compute_major, compute_minor;
      size_t total_mem;
      std::set<hipDevice_t> peers;  // other GPUs we can do p2p copies with
    };

    enum GPUMemcpyKind {
      GPU_MEMCPY_HOST_TO_DEVICE,
      GPU_MEMCPY_DEVICE_TO_HOST,
      GPU_MEMCPY_DEVICE_TO_DEVICE,
      GPU_MEMCPY_PEER_TO_PEER,
    };

    // Forard declaration
    class GPUProcessor;
    class GPUWorker;
    class GPUStream;
    class GPUFBMemory;
    class GPUZCMemory;
    class GPU;
    class HipModule;

    // an interface for receiving completion notification for a GPU operation
    //  (right now, just copies)
    class GPUCompletionNotification {
    public:
      virtual ~GPUCompletionNotification(void) {}

      virtual void request_completed(void) = 0;
    };

    class GPUPreemptionWaiter : public GPUCompletionNotification {
    public:
      GPUPreemptionWaiter(GPU *gpu);
      virtual ~GPUPreemptionWaiter(void) {}
    public:
      virtual void request_completed(void);
    public:
      void preempt(void);
    private:
      GPU *const gpu;
      Event wait_event;
    };

    // An abstract base class for all GPU memcpy operations
    class GPUMemcpy { //: public GPUJob {
    public:
      GPUMemcpy(GPU *_gpu, GPUMemcpyKind _kind);
      virtual ~GPUMemcpy(void) { }
    public:
      virtual void execute(GPUStream *stream) = 0;
    public:
      GPU *const gpu;
    protected:
      GPUMemcpyKind kind;
    };

    class GPUWorkFence : public Realm::Operation::AsyncWorkItem {
    public:
      GPUWorkFence(Realm::Operation *op);
      
      virtual void request_cancellation(void);

      void enqueue_on_stream(GPUStream *stream);

      virtual void print(std::ostream& os) const;
      
      IntrusiveListLink<GPUWorkFence> fence_list_link;
      REALM_PMTA_DEFN(GPUWorkFence,IntrusiveListLink<GPUWorkFence>,fence_list_link);
      typedef IntrusiveList<GPUWorkFence, REALM_PMTA_USE(GPUWorkFence,fence_list_link), DummyLock> FenceList;

    protected:
      static void cuda_callback(hipStream_t stream, hipError_t res, void *data);
    };

    class GPUWorkStart : public Realm::Operation::AsyncWorkItem {
    public:
      GPUWorkStart(Realm::Operation *op);

      virtual void request_cancellation(void) { return; };

      void enqueue_on_stream(GPUStream *stream);

      virtual void print(std::ostream& os) const;
      
      void mark_gpu_work_start();

    protected:
      static void cuda_start_callback(hipStream_t stream, hipError_t res, void *data);
    };

    class GPUMemcpyFence : public GPUMemcpy {
    public:
      GPUMemcpyFence(GPU *_gpu, GPUMemcpyKind _kind,
		     GPUWorkFence *_fence);

      virtual void execute(GPUStream *stream);

    protected:
      GPUWorkFence *fence;
    };

    class GPUMemcpy1D : public GPUMemcpy {
    public:
      GPUMemcpy1D(GPU *_gpu,
		  void *_dst, const void *_src, size_t _bytes, GPUMemcpyKind _kind,
		  GPUCompletionNotification *_notification);

      virtual ~GPUMemcpy1D(void);

    public:
      void do_span(off_t pos, size_t len);
      virtual void execute(GPUStream *stream);
    protected:
      void *dst;
      const void *src;
      size_t elmt_size;
      GPUCompletionNotification *notification;
    private:
      GPUStream *local_stream;  // used by do_span
    };

    class GPUMemcpy2D : public GPUMemcpy {
    public:
      GPUMemcpy2D(GPU *_gpu,
                  void *_dst, const void *_src,
                  off_t _dst_stride, off_t _src_stride,
                  size_t _bytes, size_t _lines,
                  GPUMemcpyKind _kind,
		  GPUCompletionNotification *_notification);

      virtual ~GPUMemcpy2D(void);

    public:
      virtual void execute(GPUStream *stream);
    protected:
      void *dst;
      const void *src;
      off_t dst_stride, src_stride;
      size_t bytes, lines;
      GPUCompletionNotification *notification;
    };

    class GPUMemcpy3D : public GPUMemcpy {
    public:
      GPUMemcpy3D(GPU *_gpu,
                  void *_dst, const void *_src,
                  off_t _dst_stride, off_t _src_stride,
                  off_t _dst_pstride, off_t _src_pstride,
                  size_t _bytes, size_t _height, size_t _depth,
                  GPUMemcpyKind _kind,
                  GPUCompletionNotification *_notification);

      virtual ~GPUMemcpy3D(void);

    public:
      virtual void execute(GPUStream *stream);
    protected:
      void *dst;
      const void *src;
      off_t dst_stride, src_stride, dst_pstride, src_pstride;
      size_t bytes, height, depth;
      GPUCompletionNotification *notification;
    };

    class GPUMemset1D : public GPUMemcpy {
    public:
      GPUMemset1D(GPU *_gpu,
		  void *_dst, size_t _bytes,
		  const void *_fill_data, size_t _fill_data_size,
		  GPUCompletionNotification *_notification);

      virtual ~GPUMemset1D(void);

    public:
      virtual void execute(GPUStream *stream);
    protected:
      void *dst;
      size_t bytes;
      static const size_t MAX_DIRECT_SIZE = 8;
      union {
	char direct[8];
	char *indirect;
      } fill_data;
      size_t fill_data_size;
      GPUCompletionNotification *notification;
    };

    class GPUMemset2D : public GPUMemcpy {
    public:
      GPUMemset2D(GPU *_gpu,
		  void *_dst, size_t _dst_stride,
		  size_t _bytes, size_t _lines,
		  const void *_fill_data, size_t _fill_data_size,
		  GPUCompletionNotification *_notification);

      virtual ~GPUMemset2D(void);

    public:
      void do_span(off_t pos, size_t len);
      virtual void execute(GPUStream *stream);
    protected:
      void *dst;
      size_t dst_stride;
      size_t bytes, lines;
      static const size_t MAX_DIRECT_SIZE = 8;
      union {
	char direct[8];
	char *indirect;
      } fill_data;
      size_t fill_data_size;
      GPUCompletionNotification *notification;
    };
    
    class GPUMemset3D : public GPUMemcpy {
     public:
       GPUMemset3D(GPU *_gpu,
 		  void *_dst, size_t _dst_stride, size_t _dst_pstride,
 		  size_t _bytes, size_t _height, size_t _depth,
 		  const void *_fill_data, size_t _fill_data_size,
 		  GPUCompletionNotification *_notification);

       virtual ~GPUMemset3D(void);

     public:
       void do_span(off_t pos, size_t len);
       virtual void execute(GPUStream *stream);
     protected:
       void *dst;
       size_t dst_stride, dst_pstride;
       size_t bytes, height, depth;
       static const size_t MAX_DIRECT_SIZE = 8;
       union {
 	char direct[8];
 	char *indirect;
       } fill_data;
       size_t fill_data_size;
       GPUCompletionNotification *notification;
     };

    // a class that represents a HIP stream and work associated with 
    //  it (e.g. queued copies, events in flight)
    // a stream is also associated with a GPUWorker that it will register
    //  with when async work needs doing
    class GPUStream {
    public:
      GPUStream(GPU *_gpu, GPUWorker *_worker);
      ~GPUStream(void);

      GPU *get_gpu(void) const;
      hipStream_t get_stream(void) const;

      // may be called by anybody to enqueue a copy or an event
      void add_copy(GPUMemcpy *copy);
      void add_fence(GPUWorkFence *fence);
      void add_start_event(GPUWorkStart *start);
      void add_notification(GPUCompletionNotification *notification);
      void wait_on_streams(const std::set<GPUStream*> &other_streams);

      // to be called by a worker (that should already have the GPU context
      //   current) - returns true if any work remains
      bool issue_copies(TimeLimit work_until);
      bool reap_events(TimeLimit work_until);

    protected:
      // may only be tested with lock held
      bool has_work(void) const;
      
      void add_event(hipEvent_t event, GPUWorkFence *fence, 
		     GPUCompletionNotification *notification=NULL, GPUWorkStart *start=NULL);

      GPU *gpu;
      GPUWorker *worker;

      hipStream_t stream;

      Mutex mutex;

#define USE_CQ
#ifdef USE_CQ
      Realm::CircularQueue<GPUMemcpy *> pending_copies;
#else
      std::deque<GPUMemcpy *> pending_copies;
#endif
      bool issuing_copies;

      struct PendingEvent {
	hipEvent_t event;
	GPUWorkFence *fence;
	GPUWorkStart *start;
	GPUCompletionNotification* notification;
      };
#ifdef USE_CQ
      Realm::CircularQueue<PendingEvent> pending_events;
#else
      std::deque<PendingEvent> pending_events;
#endif
    };

    // a GPUWorker is responsible for making progress on one or more GPUStreams -
    //  this may be done directly by a GPUProcessor or in a background thread
    //  spawned for the purpose
    class GPUWorker : public BackgroundWorkItem {
    public:
      GPUWorker(void);
      virtual ~GPUWorker(void);

      // adds a stream that has work to be done
      void add_stream(GPUStream *s);

      // used to start a dedicate thread (mutually exclusive with being
      //  registered with a background work manager)
      void start_background_thread(Realm::CoreReservationSet& crs,
				   size_t stack_size);
      void shutdown_background_thread(void);
      
      bool do_work(TimeLimit work_until);

    public:
      void thread_main(void);

    protected:
      // used by the background thread
      // processes work on streams, optionally sleeping for work to show up
      // returns true if work remains to be done
      bool process_streams(bool sleep_on_empty);
      
      Mutex lock;
      CondVar condvar;
      
      typedef CircularQueue<GPUStream *, 16> ActiveStreamQueue;
      ActiveStreamQueue active_streams;

      // used by the background thread (if any)
      Realm::CoreReservation *core_rsrv;
      Realm::Thread *worker_thread;
      bool thread_sleeping;
      atomic<bool> worker_shutdown_requested;      
    };

    // a little helper class to manage a pool of CUevents that can be reused
    //  to reduce alloc/destroy overheads
    class GPUEventPool {
    public:
      GPUEventPool(int _batch_size = 256);

      // allocating the initial batch of events and cleaning up are done with
      //  these methods instead of constructor/destructor because we don't
      //  manage the GPU context in this helper class
      void init_pool(int init_size = 0 /* default == batch size */);
      void empty_pool(void);

      hipEvent_t get_event(bool external = false);
      void return_event(hipEvent_t e, bool external = false);

    protected:
      Mutex mutex;
      int batch_size, current_size, total_size, external_count;
      std::vector<hipEvent_t> available_events;
    };
    
    // when the runtime hijack is not enabled/active, a cuCtxSynchronize
    //  is required to ensure a task's completion event covers all of its
    //  actions - rather than blocking an important thread, we create a
    //  small thread pool to handle these
    class ContextSynchronizer {
    public:
      ContextSynchronizer(GPU *_gpu, int _device_id,
                  			  CoreReservationSet& crs,
                  			  int _max_threads);
      ~ContextSynchronizer();

      void add_fence(GPUWorkFence *fence);

      void shutdown_threads();

      void thread_main();

    protected:
      GPU *gpu;
      //hipCtx_t context;
      int device_id;
      int max_threads;
      Mutex mutex;
      CondVar condvar;
      bool shutdown_flag;
      GPUWorkFence::FenceList fences;
      int total_threads, sleeping_threads, syncing_threads;
      std::vector<Thread *> worker_threads;
      CoreReservation *core_rsrv;
    };

    struct FatBin;
    struct RegisteredVariable;
    struct RegisteredFunction;

    // a GPU object represents our use of a given HIP-capable GPU - this will
    //  have an associated HIP context, a (possibly shared) worker thread, a 
    //  processor, and an FB memory (the ZC memory is shared across all GPUs)
    class GPU {
    public:
      GPU(HipModule *_module, GPUInfo *_info, GPUWorker *worker,
	        int _device_id,
          int num_streams);
      ~GPU(void);

      void push_context(void);
      void pop_context(void);

#ifdef REALM_USE_HIP_HIJACK
      void register_fat_binary(const FatBin *data);
      void register_variable(const RegisteredVariable *var);
      void register_function(const RegisteredFunction *func);

      hipFunction_t lookup_function(const void *func);
      hipDeviceCharptr_t lookup_variable(const void *var);
#endif

      void create_processor(RuntimeImpl *runtime, size_t stack_size);
      void create_fb_memory(RuntimeImpl *runtime, size_t size);

      void create_dma_channels(Realm::RuntimeImpl *r);

      // copy and operations are asynchronous - use a fence (of the right type)
      //   after all of your copies, or a completion notification for particular copies
      void copy_to_fb(off_t dst_offset, const void *src, size_t bytes,
		      GPUCompletionNotification *notification = 0);

      void copy_from_fb(void *dst, off_t src_offset, size_t bytes,
			GPUCompletionNotification *notification = 0);

      void copy_within_fb(off_t dst_offset, off_t src_offset,
			  size_t bytes,
			  GPUCompletionNotification *notification = 0);

      void copy_to_fb_2d(off_t dst_offset, const void *src,
                         off_t dst_stride, off_t src_stride,
                         size_t bytes, size_t lines,
			 GPUCompletionNotification *notification = 0);

      void copy_to_fb_3d(off_t dst_offset, const void *src,
                         off_t dst_stride, off_t src_stride,
                         off_t dst_height, off_t src_height,
                         size_t bytes, size_t height, size_t depth,
			 GPUCompletionNotification *notification = 0);

      void copy_from_fb_2d(void *dst, off_t src_offset,
                           off_t dst_stride, off_t src_stride,
                           size_t bytes, size_t lines,
			   GPUCompletionNotification *notification = 0);

      void copy_from_fb_3d(void *dst, off_t src_offset,
                           off_t dst_stride, off_t src_stride,
                           off_t dst_height, off_t src_height,
                           size_t bytes, size_t height, size_t depth,
			   GPUCompletionNotification *notification = 0);

      void copy_within_fb_2d(off_t dst_offset, off_t src_offset,
                             off_t dst_stride, off_t src_stride,
                             size_t bytes, size_t lines,
			     GPUCompletionNotification *notification = 0);

      void copy_within_fb_3d(off_t dst_offset, off_t src_offset,
                             off_t dst_stride, off_t src_stride,
                             off_t dst_height, off_t src_height,
                             size_t bytes, size_t height, size_t depth,
			     GPUCompletionNotification *notification = 0);

      void copy_to_peer(GPU *dst, off_t dst_offset, 
                        off_t src_offset, size_t bytes,
			GPUCompletionNotification *notification = 0);

      void copy_to_peer_2d(GPU *dst, off_t dst_offset, off_t src_offset,
                           off_t dst_stride, off_t src_stride,
                           size_t bytes, size_t lines,
			   GPUCompletionNotification *notification = 0);

      void copy_to_peer_3d(GPU *dst, off_t dst_offset, off_t src_offset,
                           off_t dst_stride, off_t src_stride,
                           off_t dst_height, off_t src_height,
                           size_t bytes, size_t height, size_t depth,
			   GPUCompletionNotification *notification = 0);

      // fill operations are also asynchronous - use fence_within_fb at end
      void fill_within_fb(off_t dst_offset,
			                    size_t bytes,
			                    const void *fill_data, size_t fill_data_size,
			                    GPUCompletionNotification *notification = 0);

      void fill_within_fb_2d(off_t dst_offset, off_t dst_stride,
			                    size_t bytes, size_t lines,
			                    const void *fill_data, size_t fill_data_size,
			                    GPUCompletionNotification *notification = 0);
           
      void fill_within_fb_3d(off_t dst_offset, off_t dst_stride,
                             off_t dst_height,
     			                   size_t bytes, size_t height, size_t depth,
     			                   const void *fill_data, size_t fill_data_size,
     			                   GPUCompletionNotification *notification = 0);

      void fence_to_fb(Realm::Operation *op);
      void fence_from_fb(Realm::Operation *op);
      void fence_within_fb(Realm::Operation *op);
      void fence_to_peer(Realm::Operation *op, GPU *dst);

      bool can_access_peer(GPU *peer);

      GPUStream *find_stream(hipStream_t stream) const;
      GPUStream *get_null_task_stream(void) const;
      GPUStream *get_next_task_stream(bool create = false);
    protected:
      hipModule_t load_hip_module(const void *data);

    public:
      HipModule *module;
      GPUInfo *info;
      GPUWorker *worker;
      GPUProcessor *proc;
      GPUFBMemory *fbmem;

      //hipCtx_t context;
      int device_id;
      hipDeviceCharptr_t fbmem_base;

      // which system memories have been registered and can be used for cuMemcpyAsync
      std::set<Memory> pinned_sysmems;

      // which other FBs we have peer access to
      std::set<Memory> peer_fbs;

      // streams for different copy types and a pile for actual tasks
      GPUStream *host_to_device_stream;
      GPUStream *device_to_host_stream;
      GPUStream *device_to_device_stream;
      std::vector<GPUStream *> peer_to_peer_streams; // indexed by target
      std::vector<GPUStream *> task_streams;
      atomic<unsigned> next_stream;

      GPUEventPool event_pool;

#ifdef REALM_USE_HIP_HIJACK
      std::map<const FatBin *, hipModule_t> device_modules;
      std::map<const void *, hipFunction_t> device_functions;
      std::map<const void *, hipDeviceCharptr_t> device_variables;
#endif
    };

    // helper to push/pop a GPU's context by scope
    class AutoGPUContext {
    public:
      AutoGPUContext(GPU& _gpu);
      AutoGPUContext(GPU *_gpu);
      ~AutoGPUContext(void);
    protected:
      GPU *gpu;
    };

    class GPUProcessor : public Realm::LocalTaskProcessor {
    public:
      GPUProcessor(GPU *_gpu, Processor _me, Realm::CoreReservationSet& crs,
                   size_t _stack_size);
      virtual ~GPUProcessor(void);

    public:
      virtual void shutdown(void);

      static GPUProcessor *get_current_gpu_proc(void);

#ifdef REALM_USE_HIP_HIJACK
      // calls that come from the HIP runtime API
      void push_call_configuration(dim3 grid_dim, dim3 block_dim,
                                   size_t shared_size, void *stream);
      void pop_call_configuration(dim3 *grid_dim, dim3 *block_dim,
                                  size_t *shared_size, void *stream);
#endif

      void stream_wait_on_event(hipStream_t stream, hipEvent_t event);
      void stream_synchronize(hipStream_t stream);
      void device_synchronize(void);

#ifdef REALM_USE_HIP_HIJACK
      void event_create(hipEvent_t *event, int flags);
      void event_destroy(hipEvent_t event);
      void event_record(hipEvent_t event, hipStream_t stream);
      void event_synchronize(hipEvent_t event);
      void event_elapsed_time(float *ms, hipEvent_t start, hipEvent_t end);
      
      void configure_call(dim3 grid_dim, dim3 block_dim,
			  size_t shared_memory, hipStream_t stream);
      void setup_argument(const void *arg, size_t size, size_t offset);
      void launch(const void *func);
      void launch_kernel(const void *func, dim3 grid_dim, dim3 block_dim, 
                         void **args, size_t shared_memory, 
                         hipStream_t stream);
#endif

      void gpu_memcpy(void *dst, const void *src, size_t size, hipMemcpyKind kind);
      void gpu_memcpy_async(void *dst, const void *src, size_t size,
			    hipMemcpyKind kind, hipStream_t stream);
#ifdef REALM_USE_HIP_HIJACK
      void gpu_memcpy_to_symbol(const void *dst, const void *src, size_t size,
				size_t offset, hipMemcpyKind kind);
      void gpu_memcpy_to_symbol_async(const void *dst, const void *src, size_t size,
				      size_t offset, hipMemcpyKind kind,
				      hipStream_t stream);
      void gpu_memcpy_from_symbol(void *dst, const void *src, size_t size,
				  size_t offset, hipMemcpyKind kind);
      void gpu_memcpy_from_symbol_async(void *dst, const void *src, size_t size,
					size_t offset, hipMemcpyKind kind,
					hipStream_t stream);
#endif

      void gpu_memset(void *dst, int value, size_t count);
      void gpu_memset_async(void *dst, int value, size_t count, hipStream_t stream);
    public:
      GPU *gpu;

      // data needed for kernel launches
      struct LaunchConfig {
        dim3 grid;
        dim3 block;
        size_t shared;
	LaunchConfig(dim3 _grid, dim3 _block, size_t _shared);
      };
      struct CallConfig : public LaunchConfig {
        hipStream_t stream; 
        CallConfig(dim3 _grid, dim3 _block, size_t _shared, hipStream_t _stream);
      };
      std::vector<CallConfig> launch_configs;
      std::vector<char> kernel_args;
      std::vector<CallConfig> call_configs;
      bool block_on_synchronize;
      ContextSynchronizer ctxsync;
    protected:
      Realm::CoreReservation *core_rsrv;
    };

    class GPUFBMemory : public LocalManagedMemory {
    public:
      GPUFBMemory(Memory _me, GPU *_gpu, hipDeviceCharptr_t _base, size_t _size);

      virtual ~GPUFBMemory(void);

      // these work, but they are SLOW
      virtual void get_bytes(off_t offset, void *dst, size_t size);
      virtual void put_bytes(off_t offset, const void *src, size_t size);

      virtual void *get_direct_ptr(off_t offset, size_t size);

    public:
      GPU *gpu;
      hipDeviceCharptr_t base;
      NetworkSegment local_segment;
    };

    class GPUZCMemory : public LocalManagedMemory {
    public:
      GPUZCMemory(Memory _me, hipDeviceCharptr_t _gpu_base, void *_cpu_base, size_t _size);

      virtual ~GPUZCMemory(void);

      virtual void get_bytes(off_t offset, void *dst, size_t size);

      virtual void put_bytes(off_t offset, const void *src, size_t size);

      virtual void *get_direct_ptr(off_t offset, size_t size);

    public:
      hipDeviceCharptr_t gpu_base;
      char *cpu_base;
      NetworkSegment local_segment;
    };
    
    class GPURequest;

    class GPUCompletionEvent : public GPUCompletionNotification {
    public:
      void request_completed(void);

      GPURequest *req;
    };

    class GPURequest : public Request {
    public:
      const void *src_base;
      void *dst_base;
      //off_t src_gpu_off, dst_gpu_off;
      GPU* dst_gpu;
      GPUCompletionEvent event;
    };
    
    class GPUTransferCompletion : public GPUCompletionNotification {
    public:
      GPUTransferCompletion(XferDes *_xd, int _read_port_idx,
                            size_t _read_offset, size_t _read_size,
                            int _write_port_idx, size_t _write_offset,
                            size_t _write_size);

      virtual void request_completed(void);

    protected:
      XferDes *xd;
      int read_port_idx;
      size_t read_offset, read_size;
      int write_port_idx;
      size_t write_offset, write_size;
    };

    class GPUChannel;

    class GPUXferDes : public XferDes {
    public:
      GPUXferDes(uintptr_t _dma_op, Channel *_channel,
		 NodeID _launch_node, XferDesID _guid,
		 const std::vector<XferDesPortInfo>& inputs_info,
		 const std::vector<XferDesPortInfo>& outputs_info,
		 int _priority);

      ~GPUXferDes()
      {
        while (!available_reqs.empty()) {
          GPURequest* gpu_req = (GPURequest*) available_reqs.front();
          available_reqs.pop();
          delete gpu_req;
        }
      }

      long get_requests(Request** requests, long nr);
      void notify_request_read_done(Request* req);
      void notify_request_write_done(Request* req);
      void flush();

      bool progress_xd(GPUChannel *channel, TimeLimit work_until);

    private:
      //GPURequest* gpu_reqs;
      //char *src_buf_base;
      //char *dst_buf_base;
      GPU *dst_gpu, *src_gpu;
    };

    class GPUChannel : public SingleXDQChannel<GPUChannel, GPUXferDes> {
    public:
      GPUChannel(GPU* _src_gpu, XferDesKind _kind,
		 BackgroundWorkManager *bgwork);
      ~GPUChannel();

      // multi-threading of cuda copies for a given device is disabled by
      //  default (can be re-enabled with -cuda:mtdma 1)
      static const bool is_ordered = true;

      virtual XferDes *create_xfer_des(uintptr_t dma_op,
      				       NodeID launch_node,
      				       XferDesID guid,
      				       const std::vector<XferDesPortInfo>& inputs_info,
      				       const std::vector<XferDesPortInfo>& outputs_info,
      				       int priority,
      				       XferDesRedopInfo redop_info,
      				       const void *fill_data, size_t fill_size);                        

      long submit(Request** requests, long nr);

    private:
      GPU* src_gpu;
      //std::deque<Request*> pending_copies;
    };
    
    class GPUfillChannel;

    class GPUfillXferDes : public XferDes {
    public:
      GPUfillXferDes(uintptr_t _dma_op, Channel *_channel,
		     NodeID _launch_node, XferDesID _guid,
		     const std::vector<XferDesPortInfo>& inputs_info,
		     const std::vector<XferDesPortInfo>& outputs_info,
		     int _priority,
		     const void *_fill_data, size_t _fill_size);

      long get_requests(Request** requests, long nr);

      bool progress_xd(GPUfillChannel *channel, TimeLimit work_until);

    protected:
      size_t reduced_fill_size;
    };

    class GPUfillChannel : public SingleXDQChannel<GPUfillChannel, GPUfillXferDes> {
    public:
      GPUfillChannel(GPU* _gpu, BackgroundWorkManager *bgwork);

      // multiple concurrent cuda fills ok
      static const bool is_ordered = false;

      virtual XferDes *create_xfer_des(uintptr_t dma_op,
				       NodeID launch_node,
				       XferDesID guid,
				       const std::vector<XferDesPortInfo>& inputs_info,
				       const std::vector<XferDesPortInfo>& outputs_info,
				       int priority,
				       XferDesRedopInfo redop_info,
				       const void *fill_data, size_t fill_size);

      long submit(Request** requests, long nr);

    protected:
      friend class GPUfillXferDes;

      GPU* gpu;
    };

  }; // namespace Hip
  
}; // namespace Realm 

#endif
