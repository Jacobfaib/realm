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

#include "lowlevel_gpu.h"

#include <stdio.h>

namespace LegionRuntime {
  namespace LowLevel {
    GASNETT_THREADKEY_DEFINE(gpu_thread_ptr);

    extern Logger::Category log_gpu;
#ifdef EVENT_GRAPH_TRACE
    extern Logger::Category log_event_graph;
#endif

    GPUTask::GPUTask(GPUProcessor *_gpu, Task *_task)
      : GPUJob(_gpu), task(_task)
    {
    }

    GPUTask::~GPUTask(void)
    {
      if (__sync_add_and_fetch(&(task->finish_count),-1) == 0)
        delete task;
    }

    bool GPUTask::event_triggered(void)
    {
      // Should never be called
      assert(false);
      return false;
    }

    void GPUTask::print_info(FILE *f)
    {
      // should never be called
      assert(false);
    }

    void GPUTask::run_or_wait(Event start_event)
    {
      // should never be called
      assert(false);
    }

    void GPUTask::execute(void)
    {
      Processor::TaskFuncPtr fptr = get_runtime()->task_table[task->func_id];
      //char argstr[100];
      //argstr[0] = 0;
      //for(size_t i = 0; (i < arglen) && (i < 40); i++)
      //	sprintf(argstr+2*i, "%02x", ((unsigned char *)args)[i]);
      //if(arglen > 40) strcpy(argstr+80, "...");
      //log_gpu.debug("task start: %d (%p) (%s)", func_id, fptr, argstr);

      // make sure CUDA driver's state is ok before we start
      //assert(cudaGetLastError() == cudaSuccess);
#ifdef EVENT_GRAPH_TRACE
      assert(task->finish_event.exists());
      start_enclosing(task->finish_event); 
      unsigned long long start = TimeStamp::get_current_time_in_micros();
#endif
      if (task->perform_capture()) {
        CHECK_CU( cuStreamAddCallback(local_stream, GPUTask::handle_start,
                                      (void*)this, 0) );
      }
      (*fptr)(task->args, task->arglen, gpu->me);
#ifdef EVENT_GRAPH_TRACE
      unsigned long long stop = TimeStamp::get_current_time_in_micros();
      finish_enclosing();
      log_event_graph.debug("Task Time: (" IDFMT ",%d) %lld",
                            task->finish_event.id, task->finish_event.gen,
                            (stop - start));
#endif
      // Mark that we recor
      // Add a callback for when the event has triggered
      CHECK_CU( cuStreamAddCallback(local_stream, GPUTask::handle_finish, 
                                    (void*)this, 0) );
      // A useful debugging macro
#ifdef FORCE_GPU_STREAM_SYNCHRONIZE
      CHECK_CU( cuStreamSynchronize(local_stream) );
#endif
      // check for any uncaught driver errors after the task finishes
#if 0
      {
	cudaError_t result = cudaGetLastError();
	if (result != cudaSuccess) {
	  log_gpu.error("CUDA: uncaught driver error in task %d: %d (%s)",
			func_id, result, cudaGetErrorString(result));
	}
      }
#endif
      //log_gpu.debug("task end: %d (%p) (%s)", func_id, fptr, argstr);
    }

    void GPUTask::finish_job(void)
    {
      // Clear out all our modules that we created
      for (std::set<void**>::const_iterator it = modules.begin();
            it != modules.end(); it++)
      {
        gpu->internal_unregister_fat_binary(*it);
      }
      // If we have a finish event then trigger it
      if (task->finish_event.exists())
        get_runtime()->get_genevent_impl(task->finish_event)->
          trigger(task->finish_event.gen, gasnet_mynode());
    }

    /*static*/ void GPUTask::handle_start(CUstream stream, CUresult res, void *data)
    {
      GPUTask *task = static_cast<GPUTask*>(data);
      task->task->mark_started();
    }

    /*static*/ void GPUTask::handle_finish(CUstream stream, CUresult res, void *data)
    {
      GPUTask *task = static_cast<GPUTask*>(data);
      if (task->task->perform_capture())
        task->task->mark_completed();
      task->gpu->handle_complete_job(task);
    }

    GPUMemcpy::GPUMemcpy(GPUProcessor *_gpu, Event _finish_event,
                         GPUMemcpyKind _kind)
        : GPUJob(_gpu), kind(_kind), finish_event(_finish_event)
    {
      if (kind == GPU_MEMCPY_HOST_TO_DEVICE)
        local_stream = gpu->host_to_device_stream;
      else if (kind == GPU_MEMCPY_DEVICE_TO_HOST)
        local_stream = gpu->device_to_host_stream;
      else if (kind == GPU_MEMCPY_DEVICE_TO_DEVICE)
        local_stream = gpu->device_to_device_stream;
      else if (kind == GPU_MEMCPY_PEER_TO_PEER)
        local_stream = gpu->peer_to_peer_stream;
      else
        assert(false); // who does host to host here?!?
    } 

    bool GPUMemcpy::event_triggered(void)
    {
      log_gpu.info("gpu job %p now runnable", this);
      gpu->enqueue_copy(this);
      // don't delete
      return false;
    }

    void GPUMemcpy::print_info(FILE *f)
    {
      fprintf(f,"GPU Memcpy: %p after=" IDFMT "/%d\n",
          this, finish_event.id, finish_event.gen);
    }

    void GPUMemcpy::run_or_wait(Event start_event)
    {
      if(start_event.has_triggered()) {
        log_gpu.info("job %p can start right away!?", this);
        gpu->enqueue_copy(this);
      } else {
        log_gpu.info("job %p waiting for " IDFMT "/%d", this, start_event.id, start_event.gen);
        start_event.impl()->add_waiter(start_event.gen, this);
      }
    }

    void GPUMemcpy::post_execute(void)
    {
      // Add a callback to the stream to record when the operation is done
      CHECK_CU( cuStreamAddCallback(local_stream, GPUMemcpy::handle_finish,
                                    (void*)this, 0) );
    }

    void GPUMemcpy::finish_job(void)
    {
      // If we have a finish event then trigger it
      if (finish_event.exists())
        get_runtime()->get_genevent_impl(finish_event)->
          trigger(finish_event.gen, gasnet_mynode());
    }

    /*static*/ void GPUMemcpy::handle_finish(CUstream stream, CUresult res, void *data)
    {
      GPUMemcpy *copy = static_cast<GPUMemcpy*>(data);
      copy->gpu->handle_complete_job(copy);
    }

    void GPUMemcpy1D::do_span(off_t pos, size_t len)
    {
      off_t span_start = pos * elmt_size;
      size_t span_bytes = len * elmt_size;

      switch (kind)
      {
        case GPU_MEMCPY_HOST_TO_DEVICE:
          {
            CHECK_CU( cuMemcpyHtoDAsync((CUdeviceptr)(((char*)dst)+span_start),
                                        (((char*)src)+span_start),
                                        span_bytes,
                                        local_stream) );
            break;
          }
        case GPU_MEMCPY_DEVICE_TO_HOST:
          {
            CHECK_CU( cuMemcpyDtoHAsync((((char*)dst)+span_start),
                                        (CUdeviceptr)(((char*)src)+span_start),
                                        span_bytes,
                                        local_stream) );
            break;
          }
        case GPU_MEMCPY_DEVICE_TO_DEVICE:
          {
            CHECK_CU( cuMemcpyDtoDAsync((CUdeviceptr)(((char*)dst)+span_start),
                                        (CUdeviceptr)(((char*)src)+span_start),
                                        span_bytes,
                                        local_stream) );
            break;
          }
        case GPU_MEMCPY_PEER_TO_PEER:
          {
            CUcontext src_ctx, dst_ctx;
            CHECK_CU( cuPointerGetAttribute(&src_ctx, 
                  CU_POINTER_ATTRIBUTE_CONTEXT, (CUdeviceptr)src) );
            CHECK_CU( cuPointerGetAttribute(&dst_ctx, 
                  CU_POINTER_ATTRIBUTE_CONTEXT, (CUdeviceptr)dst) );
            CHECK_CU( cuMemcpyPeerAsync((CUdeviceptr)(((char*)dst)+span_start), dst_ctx,
                                        (CUdeviceptr)(((char*)src)+span_start), src_ctx,
                                        span_bytes,
                                        local_stream) );
            break;
          }
        default:
          assert(false);
      }
    }

    void GPUMemcpy1D::execute(void)
    {
      DetailedTimer::ScopedPush sp(TIME_COPY);
      log_gpu.info("gpu memcpy: dst=%p src=%p bytes=%zd kind=%d",
                   dst, src, elmt_size, kind);
      if(mask) {
        ElementMask::forall_ranges(*this, *mask);
      } else {
        do_span(0, 1);
      }
      post_execute();  
      log_gpu.info("gpu memcpy complete: dst=%p src=%p bytes=%zd kind=%d",
                   dst, src, elmt_size, kind);
    }

    void GPUMemcpy2D::execute(void)
    {
      log_gpu.info("gpu memcpy 2d: dst=%p src=%p "
                   "dst_off=%ld src_off=%ld bytes=%ld lines=%ld kind=%d",
                   dst, src, (long)dst_stride, (long)src_stride, bytes, lines, kind); 
      CUDA_MEMCPY2D copy_info;
      if (kind == GPU_MEMCPY_PEER_TO_PEER) {
        // If we're doing peer to peer, just let unified memory it deal with it
        copy_info.srcMemoryType = CU_MEMORYTYPE_UNIFIED;
        copy_info.dstMemoryType = CU_MEMORYTYPE_UNIFIED;
      } else {
        // otherwise we know the answers here 
        copy_info.srcMemoryType = (kind == GPU_MEMCPY_HOST_TO_DEVICE) ?
          CU_MEMORYTYPE_HOST : CU_MEMORYTYPE_DEVICE;
        copy_info.dstMemoryType = (kind == GPU_MEMCPY_DEVICE_TO_HOST) ? 
          CU_MEMORYTYPE_HOST : CU_MEMORYTYPE_DEVICE;
      }
      copy_info.srcDevice = (CUdeviceptr)src;
      copy_info.srcHost = src;
      copy_info.srcPitch = src_stride;
      copy_info.srcY = 0;
      copy_info.srcXInBytes = 0;
      copy_info.dstDevice = (CUdeviceptr)dst;
      copy_info.dstHost = dst;
      copy_info.dstPitch = dst_stride;
      copy_info.dstY = 0;
      copy_info.dstXInBytes = 0;
      copy_info.WidthInBytes = bytes;
      copy_info.Height = lines;
      CHECK_CU( cuMemcpy2DAsync(&copy_info, local_stream) );
      post_execute();
      log_gpu.info("gpu memcpy 2d complete: dst=%p src=%p "
                   "dst_off=%ld src_off=%ld bytes=%ld lines=%ld kind=%d",
                   dst, src, (long)dst_stride, (long)src_stride, bytes, lines, kind);
    }

    GPUThread::GPUThread(GPUProcessor *gpu)
      : LocalThread(gpu), gpu_proc(gpu)
    {
    }

    GPUThread::~GPUThread(void)
    {
    }

    void GPUThread::thread_main(void)
    {
      gasnett_threadkey_set(gpu_thread_ptr, gpu_proc);
      // Load the context
      gpu_proc->load_context();
      if (initialize)
        proc->initialize_processor();
      while (true)
      {
        assert(state == RUNNING_STATE);
        bool quit = gpu_proc->execute_gpu(this);
        if (quit) break;
      }
      if (finalize)
        proc->finalize_processor();
    }

    GPUProcessor::GPUProcessor(Processor _me, Processor::Kind _kind, 
                               const char *_name, int _gpu_index, 
                               size_t _zcmem_size, size_t _fbmem_size, 
                               size_t _stack_size, GPUWorker *worker/*can be 0*/,
                               int streams, int _core_id /*= -1*/)
      : LocalProcessor(_me, _kind, _stack_size, _name, _core_id),
        gpu_index(_gpu_index), zcmem_size(_zcmem_size), fbmem_size(_fbmem_size),
        zcmem_reserve(16 << 20), fbmem_reserve(32 << 20), gpu_worker(worker),
        current_stream(0)
    {
      assert(streams > 0);
      task_streams.resize(streams);
      // Make our context and then immediately pop it off
      CHECK_CU( cuDeviceGet(&proc_dev, gpu_index) );

      CHECK_CU( cuCtxCreate(&proc_ctx, CU_CTX_MAP_HOST |
                            CU_CTX_SCHED_BLOCKING_SYNC, proc_dev) );

      // allocate zero-copy memory
      CHECK_CU( cuMemHostAlloc(&zcmem_cpu_base,
                               zcmem_size + zcmem_reserve,
                               (CU_MEMHOSTALLOC_PORTABLE |
                                CU_MEMHOSTALLOC_DEVICEMAP)) );
      CHECK_CU( cuMemHostGetDevicePointer((CUdeviceptr*)&zcmem_gpu_base,
                                          zcmem_cpu_base, 0) );

      // allocate frame buffer memory
      CHECK_CU( cuMemAlloc((CUdeviceptr*)&fbmem_gpu_base, fbmem_size + fbmem_reserve) );

      // allocate pinned buffer for kernel arguments
      kernel_buffer_size = 8192; // default four pages
      kernel_arg_size = 0;
      CHECK_CU( cuMemAllocHost((void**)&kernel_arg_buffer, kernel_buffer_size) );
      
      CHECK_CU( cuCtxPopCurrent(&proc_ctx) );
    }

    GPUProcessor::~GPUProcessor(void)
    {
    }

    void* GPUProcessor::get_zcmem_cpu_base(void) const
    {
      return ((char *)zcmem_cpu_base) + zcmem_reserve;
    }

    void* GPUProcessor::get_fbmem_gpu_base(void) const
    {
      return ((char *)fbmem_gpu_base) + fbmem_reserve;
    }

    size_t GPUProcessor::get_zcmem_size(void) const
    {
      return zcmem_size;
    }

    size_t GPUProcessor::get_fbmem_size(void) const
    {
      return fbmem_size;
    }

    void GPUProcessor::copy_to_fb(off_t dst_offset, const void *src, size_t bytes,
				  Event start_event, Event finish_event)
    {
      (new GPUMemcpy1D(this, finish_event,
       ((char *)fbmem_gpu_base) + (fbmem_reserve + dst_offset),
       src, bytes, GPU_MEMCPY_HOST_TO_DEVICE))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_to_fb(off_t dst_offset, const void *src,
				  const ElementMask *mask, size_t elmt_size,
				  Event start_event, Event finish_event)
    {
      (new GPUMemcpy1D(this, finish_event,
       ((char *)fbmem_gpu_base) + (fbmem_reserve + dst_offset),
       src, mask, elmt_size, GPU_MEMCPY_HOST_TO_DEVICE))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_from_fb(void *dst, off_t src_offset, size_t bytes,
				    Event start_event, Event finish_event)
    {
      (new GPUMemcpy1D(this, finish_event,
       dst, ((char *)fbmem_gpu_base) + (fbmem_reserve + src_offset),
       bytes, GPU_MEMCPY_DEVICE_TO_HOST))->run_or_wait(start_event);
    } 

    void GPUProcessor::copy_from_fb(void *dst, off_t src_offset,
				    const ElementMask *mask, size_t elmt_size,
				    Event start_event, Event finish_event)
    {
      (new GPUMemcpy1D(this, finish_event,
       dst, ((char *)fbmem_gpu_base) + (fbmem_reserve + src_offset),
       mask, elmt_size, GPU_MEMCPY_DEVICE_TO_HOST))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_within_fb(off_t dst_offset, off_t src_offset,
				      size_t bytes,
				      Event start_event, Event finish_event)
    {
      (new GPUMemcpy1D(this, finish_event,
       ((char *)fbmem_gpu_base) + (fbmem_reserve + dst_offset),
       ((char *)fbmem_gpu_base) + (fbmem_reserve + src_offset),
       bytes, GPU_MEMCPY_DEVICE_TO_DEVICE))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_within_fb(off_t dst_offset, off_t src_offset,
				      const ElementMask *mask, size_t elmt_size,
				      Event start_event, Event finish_event)
    {
      (new GPUMemcpy1D(this, finish_event,
       ((char *)fbmem_gpu_base) + (fbmem_reserve + dst_offset),
       ((char *)fbmem_gpu_base) + (fbmem_reserve + src_offset),
       mask, elmt_size, GPU_MEMCPY_DEVICE_TO_DEVICE))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_to_fb_2d(off_t dst_offset, const void *src, 
                                     off_t dst_stride, off_t src_stride,
                                     size_t bytes, size_t lines,
                                     Event start_event, Event finish_event)
    {
      (new GPUMemcpy2D(this, finish_event,
                       ((char*)fbmem_gpu_base)+
                        (fbmem_reserve + dst_offset),
                        src, dst_stride, src_stride, bytes, lines,
                        GPU_MEMCPY_HOST_TO_DEVICE))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_from_fb_2d(void *dst, off_t src_offset,
                                       off_t dst_stride, off_t src_stride,
                                       size_t bytes, size_t lines,
                                       Event start_event, Event finish_event)
    {
      (new GPUMemcpy2D(this, finish_event, dst,
                       ((char*)fbmem_gpu_base)+
                        (fbmem_reserve + src_offset),
                        dst_stride, src_stride, bytes, lines,
                        GPU_MEMCPY_DEVICE_TO_HOST))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_within_fb_2d(off_t dst_offset, off_t src_offset,
                                         off_t dst_stride, off_t src_stride,
                                         size_t bytes, size_t lines,
                                         Event start_event, Event finish_event)
    {
      (new GPUMemcpy2D(this, finish_event,
                       ((char*)fbmem_gpu_base) + 
                        (fbmem_reserve + dst_offset),
                       ((char*)fbmem_gpu_base) + 
                        (fbmem_reserve + src_offset),
                        dst_stride, src_stride, bytes, lines,
                        GPU_MEMCPY_DEVICE_TO_DEVICE))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_to_peer(GPUProcessor *dst, off_t dst_offset,
                                    off_t src_offset, size_t bytes,
                                    Event start_event, Event finish_event)
    {
      (new GPUMemcpy1D(this, finish_event,
              ((char*)dst->fbmem_gpu_base) + 
                      (dst->fbmem_reserve + dst_offset),
              ((char*)fbmem_gpu_base) + 
                      (fbmem_reserve + src_offset),
              bytes, GPU_MEMCPY_PEER_TO_PEER))->run_or_wait(start_event);
    }

    void GPUProcessor::copy_to_peer_2d(GPUProcessor *dst,
                                       off_t dst_offset, off_t src_offset,
                                       off_t dst_stride, off_t src_stride,
                                       size_t bytes, size_t lines,
                                       Event start_event, Event finish_event)
    {
      (new GPUMemcpy2D(this, finish_event,
                       ((char*)dst->fbmem_gpu_base) +
                                (dst->fbmem_reserve + dst_offset),
                       ((char*)fbmem_gpu_base) +
                                (fbmem_reserve + src_offset),
                        dst_stride, src_stride, bytes, lines,
                        GPU_MEMCPY_PEER_TO_PEER))->run_or_wait(start_event);
    }

    /*static*/ Processor GPUProcessor::get_processor(void)
    {
      void *tls_val = gasnett_threadkey_get(gpu_thread_ptr);
      // If this happens there is a case we're not handling
      assert(tls_val != NULL);
      GPUProcessor *gpu = (GPUProcessor*)tls_val;
      return gpu->me;
    }

    void GPUProcessor::register_host_memory(void *base, size_t size)
    {
      if (!shutdown)
      {
        CHECK_CU( cuCtxPushCurrent(proc_ctx) );
        CHECK_CU( cuMemHostRegister(base, size, CU_MEMHOSTREGISTER_PORTABLE) ); 
        CHECK_CU( cuCtxPopCurrent(&proc_ctx) );
      }
    }

    void GPUProcessor::enable_peer_access(GPUProcessor *peer)
    {
      peer->handle_peer_access(proc_ctx);
      peer_gpus.insert(peer);
    }

    void GPUProcessor::handle_peer_access(CUcontext peer_ctx)
    {
      CHECK_CU( cuCtxPushCurrent(proc_ctx) );
      CHECK_CU( cuCtxEnablePeerAccess(peer_ctx, 0) );
      CHECK_CU( cuCtxPopCurrent(&proc_ctx) );
    }

    bool GPUProcessor::can_access_peer(GPUProcessor *peer) const
    {
      return (peer_gpus.find(peer) != peer_gpus.end());
    }

    void GPUProcessor::handle_complete_job(GPUJob *job)
    {
      // If we have a GPU DMA worker, see if it is
      // one of the DMA streams and then notify the worker
      if (gpu_worker) {
        gpu_worker->handle_complete_job(this, job);
      } else {
        // Otherwise see if we need to wake up a thread
        LocalThread *to_wake = 0;
        LocalThread *to_start = 0;
        gasnet_hsl_lock(&mutex);
        // Add this to the list of complete jobs
        complete_jobs.push_back(job);
        // Make sure there is a running thread
        if (running_thread == NULL) {
          if (!available_threads.empty()) {
            to_wake = available_threads.back();
            available_threads.pop_back();
            running_thread = to_wake;
          } else {
            to_start = create_new_thread();
            running_thread = to_start;
          }
        }
        gasnet_hsl_unlock(&mutex);
        if (to_wake)
          to_wake->awake();
        if (to_start)
          to_start->start_thread(stack_size, core_id, processor_name);
      }
    }

    CUstream GPUProcessor::get_current_task_stream(void)
    {
      return task_streams[current_stream];
    }

    void GPUProcessor::load_context(void)
    {
      // Push our context onto the stack
      CHECK_CU( cuCtxPushCurrent(proc_ctx) );
    }

    bool GPUProcessor::execute_gpu(GPUThread *thread)
    {
      gasnet_hsl_lock(&mutex);
      // Sanity check, we should be the running thread if we are in here
      assert(thread == running_thread);
      // First check to see if there are any resumable threads
      // If there are then we will switch onto those
      if (!resumable_threads.empty())
      {
        // Move this thread on to the available threads and wake
        // up one of the resumable threads
        thread->prepare_to_sleep();
        available_threads.push_back(thread);
        // Pull the first thread off the resumable threads
        LocalThread *to_resume = resumable_threads.front();
        resumable_threads.pop_front();
        // Make this the running thread
        running_thread = to_resume;
        // Release the lock
        gasnet_hsl_unlock(&mutex);
        // Wake up the resumable thread
        to_resume->resume();
        // Put ourselves to sleep
        thread->sleep();
      }
      else if (task_queue.empty() &&
               (gpu_worker || (copies.empty() && complete_jobs.empty())))
      {
        // If there is nothing to do then we should go to sleep
        thread->prepare_to_sleep();
        available_threads.push_back(thread);
        running_thread = NULL;
        gasnet_hsl_unlock(&mutex);
        thread->sleep();
      }
      else
      {
        std::vector<GPUMemcpy*> ready_copies;
        std::vector<GPUJob*> to_complete;
        if (!gpu_worker)
        {
          if (!ready_copies.empty()) {
            ready_copies.insert(ready_copies.end(),copies.begin(),copies.end()); 
            copies.clear();
          }
          if (!complete_jobs.empty()) {
            to_complete.insert(to_complete.end(),
                               complete_jobs.begin(),complete_jobs.end());
            complete_jobs.clear();
          }
        }
        GPUTask *gpu_task = 0;
        if (!task_queue.empty()) {
          Task *task = task_queue.pop();  
          // If this is the kill pill, then do a 
          // little extra work before releasing the lock
          if (task->func_id == 0) {
            finished();
            // Mark that we received the shutdown trigger
            shutdown_trigger = true;
            gasnett_cond_signal(&condvar);
            gasnet_hsl_unlock(&mutex);
            // Trigger the completion task
            if (__sync_fetch_and_add(&(task->run_count),1) == 0)
              get_runtime()->get_genevent_impl(task->finish_event)->
                            trigger(task->finish_event.gen, gasnet_mynode());
            // Delete the task
            if (__sync_add_and_fetch(&(task->finish_count),-1) == 0)
              delete task;
          } else {
            gasnet_hsl_unlock(&mutex);
            // Figure out if we are going to execute this task, if so we
            // need to add it to our list of pending tasks on the current stream
            // before we release the lock and run the task
            if (__sync_fetch_and_add(&(task->run_count),1) == 0) {
              // Wrap this task up in a GPUTask
              gpu_task = new GPUTask(this, task);
            } else {
              // Remove our delete reference
              if (__sync_add_and_fetch(&(task->finish_count),-1) == 0)
                delete task;
            }
          }
        } else {
          // Still have to release the lock
          gasnet_hsl_unlock(&mutex);
        }
        // Launch any ready copies
        if (!ready_copies.empty())
        {
          // Launch all the copies first since we know that they
          // are going to be asynchronous on streams that won't block tasks
          // These calls well enqueue the copies on the right queue.
          for (std::vector<GPUMemcpy*>::const_iterator it = ready_copies.begin();
                it != ready_copies.end(); it++)
          {
            (*it)->execute();
          }
        }
        if (gpu_task)
        {
          gpu_task->set_local_stream(task_streams[current_stream]);
          //printf("executing job %p\n", job);
          assert(task_modules.empty());
          gpu_task->execute();
          // When we are done, tell the task about all the modules
          // it needs to unload
          gpu_task->record_modules(task_modules);
          task_modules.clear();
          // Update the current stream
          current_stream++;
          if ((size_t)current_stream >= task_streams.size())
            current_stream = 0;
        }
        // Handle any complete jobs that we have
        if (!to_complete.empty())
        {
          for (std::vector<GPUJob*>::const_iterator it = 
                to_complete.begin(); it != to_complete.end(); it++)
          {
            (*it)->finish_job();
            delete (*it);
          }
        }
      }
      return shutdown;
    }

    void GPUProcessor::initialize_processor(void)
    {
      // load any modules, functions, and variables that we deferred
      const std::map<void*,void**> &deferred_modules = get_deferred_modules();
      for (std::map<void*,void**>::const_iterator it = deferred_modules.begin();
            it != deferred_modules.end(); it++)
      {
        ModuleInfo &info = modules[it->second];
        FatBin *fatbin_args = (FatBin*)it->first;
        assert((fatbin_args->data != NULL) ||
               (fatbin_args->filename_or_fatbins != NULL));
        if (fatbin_args->data != NULL)
        {
          load_module(&(info.module), fatbin_args->data);
        } else {
          CHECK_CU( cuModuleLoad(&(info.module), 
                (const char*)fatbin_args->filename_or_fatbins) );
        }
      }
      const std::map<void*,void**> &deferred_cubins = get_deferred_cubins();
      for (std::map<void*,void**>::const_iterator it = deferred_cubins.begin();
            it != deferred_cubins.end(); it++)
      {
        ModuleInfo &info = modules[it->second];
        CHECK_CU( cuModuleLoadData(&(info.module), it->first) );
      }
      const std::deque<DeferredFunction> &deferred_functions = get_deferred_functions();
      for (std::deque<DeferredFunction>::const_iterator it = deferred_functions.begin();
            it != deferred_functions.end(); it++)
      {
        internal_register_function(it->handle, it->host_fun, it->device_fun);
      }
      const std::deque<DeferredVariable> &deferred_variables = get_deferred_variables();
      for (std::deque<DeferredVariable>::const_iterator it = deferred_variables.begin();
            it != deferred_variables.end(); it++)
      {
        internal_register_var(it->handle, it->host_var, it->device_name, 
                              it->external, it->size, it->constant, it->global);
      } 

      // initialize the streams for copy operations
      CHECK_CU( cuStreamCreate(&host_to_device_stream,
                               CU_STREAM_NON_BLOCKING) );
      CHECK_CU( cuStreamCreate(&device_to_host_stream,
                               CU_STREAM_NON_BLOCKING) );
      CHECK_CU( cuStreamCreate(&device_to_device_stream,
                               CU_STREAM_NON_BLOCKING) );
      CHECK_CU( cuStreamCreate(&peer_to_peer_stream,
                               CU_STREAM_NON_BLOCKING) );
      for (unsigned idx = 0; idx < task_streams.size(); idx++)
      {
        CHECK_CU( cuStreamCreate(&task_streams[idx],
                                 CU_STREAM_NON_BLOCKING) );
      }

      log_gpu.info("gpu initialized: zcmem=%p/%p fbmem=%p",
              zcmem_cpu_base, zcmem_gpu_base, fbmem_gpu_base);

      // now do the normal initialization
      LocalProcessor::initialize_processor();
    }

    void GPUProcessor::finalize_processor(void)
    {
      log_gpu.info("shutting down");
      // do the normal finalization 
      LocalProcessor::finalize_processor();
      // Synchronize the device so we can flush any printf buffers
      CHECK_CU( cuCtxSynchronize() );
    }

    LocalThread* GPUProcessor::create_new_thread(void)
    {
      return new GPUThread(this);
    }

    void GPUProcessor::enqueue_copy(GPUMemcpy *copy)
    {
      // Add it to the list of copies and wake up whoever is
      // supposed to be handling the copies
      if (gpu_worker) {
        gpu_worker->enqueue_copy(this, copy);
      } else {
        LocalThread *to_wake = 0;
        LocalThread *to_start = 0;
        gasnet_hsl_lock(&mutex);
        copies.push_back(copy);
        // If there is no worker, we need to make sure a thread
        // is awake and running to handle the copies
        if (running_thread == NULL) {
          if (!available_threads.empty()) {
            to_wake = available_threads.back();
            available_threads.pop_back();
            running_thread = to_wake;
          } else {
            to_start = create_new_thread(); 
            running_thread = to_start;
          }
        }
        gasnet_hsl_unlock(&mutex);
        if (to_wake)
          to_wake->awake();
        if (to_start)
          to_start->start_thread(stack_size, core_id, processor_name);
      }
    }

    void GPUProcessor::issue_copies(const std::deque<GPUMemcpy*> &to_issue)
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(!to_issue.empty());
#endif
      cuCtxPushCurrent(proc_ctx);
      for (std::deque<GPUMemcpy*>::const_iterator it = to_issue.begin();
            it != to_issue.end(); it++)
      {
        (*it)->execute();
      }
      cuCtxPopCurrent(&proc_ctx);
    }

    void GPUProcessor::finish_jobs(const std::deque<GPUJob*> &to_complete)
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(!to_complete.empty());
#endif
      cuCtxPushCurrent(proc_ctx);
      for (std::deque<GPUJob*>::const_iterator it = to_complete.begin();
            it != to_complete.end(); it++)
      {
        (*it)->finish_job();
        delete (*it);
      }
      cuCtxPopCurrent(&proc_ctx);
    }
    
    /*static*/ GPUProcessor** GPUProcessor::node_gpus;
    /*static*/ size_t GPUProcessor::num_node_gpus;
    static std::vector<pthread_t> dma_threads;


    GPUWorker::GPUWorker(void)
      : copies_empty(true), jobs_empty(true), worker_shutdown_requested(false)
    {
      gasnet_hsl_init(&worker_lock);
      gasnett_cond_init(&worker_cond);
    }

    GPUWorker::~GPUWorker(void)
    {
    }

    void GPUWorker::shutdown(void)
    {
      AutoHSLLock a(worker_lock);
      worker_shutdown_requested = true;
      gasnett_cond_signal(&worker_cond);
    }

    void GPUWorker::enqueue_copy(GPUProcessor *proc, GPUMemcpy *copy)
    {
      AutoHSLLock a(worker_lock);
      std::deque<GPUMemcpy*> &proc_copies = copies[proc];
      proc_copies.push_back(copy);
      copies_empty = false;
      gasnett_cond_signal(&worker_cond);
    }

    void GPUWorker::handle_complete_job(GPUProcessor *proc, GPUJob *job)
    {
      AutoHSLLock a(worker_lock);
      std::deque<GPUJob*> &proc_jobs = complete_jobs[proc];
      proc_jobs.push_back(job);
      jobs_empty = false;
      gasnett_cond_signal(&worker_cond);
    }

    Processor GPUWorker::get_processor(void) const
    {
      // should never be called
      assert(false);
      return Processor::NO_PROC;
    }

    void GPUWorker::thread_main(void)
    {
      std::map<GPUProcessor*,std::deque<GPUMemcpy*> > ready_copies;
      std::map<GPUProcessor*,std::deque<GPUJob*> > to_complete;
      while (true) 
      {
        {
          AutoHSLLock a(worker_lock);
          // See if we have any work to do
          if (copies_empty && jobs_empty) {
            if (worker_shutdown_requested)
              break;
            else
              gasnett_cond_wait(&worker_cond, &worker_lock.lock);
          } else {
            for (std::map<GPUProcessor*,std::deque<GPUMemcpy*> >::iterator
                  it = copies.begin(); it != copies.end(); it++)
            {
              if (it->second.empty())
                continue;
              ready_copies[it->first] = it->second;
              it->second.clear();
            }
            copies_empty = true;
            for (std::map<GPUProcessor*,std::deque<GPUJob*> >::iterator
                  it = complete_jobs.begin(); it != complete_jobs.end(); it++)
            {
              if (it->second.empty())
                continue;
              to_complete[it->first] = it->second;
              it->second.clear();
            }
            jobs_empty = true;
          }
        }
        // Now that we've released the lock, handle everything
        for (std::map<GPUProcessor*,std::deque<GPUMemcpy*> >::iterator
              it = ready_copies.begin(); it != ready_copies.end(); it++)
        {
          if (!it->second.empty())
            it->first->issue_copies(it->second);
          it->second.clear();
        }
        for (std::map<GPUProcessor*,std::deque<GPUJob*> >::iterator
              it = to_complete.begin(); it != to_complete.end(); it++)
        {
          if (!it->second.empty())
            it->first->finish_jobs(it->second);
          it->second.clear();
        }
      }
    }

    void GPUWorker::sleep_on_event(Event wait_for)
    {
      // should never be called
      assert(false);
    }

    /*static*/
    GPUWorker* GPUWorker::start_gpu_worker_thread(size_t stack_size)
    {
      GPUWorker *&the_gpu_worker = get_worker();
      the_gpu_worker = new GPUWorker();
      the_gpu_worker->start_thread(stack_size, -1, "GPU worker");
      return the_gpu_worker;
    }

    /*static*/ void GPUWorker::stop_gpu_worker_thread(void)
    {
      GPUWorker *worker = get_worker();
      if (worker != NULL)
        get_worker()->shutdown(); 
    }

    /*static*/
    GPUWorker*& GPUWorker::get_worker(void)
    {
      static GPUWorker *worker = NULL;
      return worker;
    }

    // framebuffer memory

    GPUFBMemory::GPUFBMemory(Memory _me, GPUProcessor *_gpu)
      : Memory::Impl(_me, _gpu->get_fbmem_size(), MKIND_GPUFB, 512, Memory::GPU_FB_MEM),
	gpu(_gpu)
    {
      base = (char *)(gpu->get_fbmem_gpu_base());
      free_blocks[0] = size;
    }

    GPUFBMemory::~GPUFBMemory(void) {}

    // these work, but they are SLOW
    void GPUFBMemory::get_bytes(off_t offset, void *dst, size_t size)
    {
      // create an async copy and then wait for it to finish...
      Event e = GenEventImpl::create_genevent()->current_event();
      gpu->copy_from_fb(dst, offset, size, Event::NO_EVENT, e);
      e.wait();
    }

    void GPUFBMemory::put_bytes(off_t offset, const void *src, size_t size)
    {
      // create an async copy and then wait for it to finish...
      Event e = GenEventImpl::create_genevent()->current_event();
      gpu->copy_to_fb(offset, src, size, Event::NO_EVENT, e);
      e.wait();
    }

    // zerocopy memory

    GPUZCMemory::GPUZCMemory(Memory _me, GPUProcessor *_gpu)
      : Memory::Impl(_me, _gpu->get_zcmem_size(), MKIND_ZEROCOPY, 256, Memory::Z_COPY_MEM),
	gpu(_gpu)
    {
      cpu_base = (char *)(gpu->get_zcmem_cpu_base());
      free_blocks[0] = size;
    }

    GPUZCMemory::~GPUZCMemory(void) {}

#ifdef POINTER_CHECKS
    static unsigned *get_gpu_valid_mask(RegionMetaDataUntyped region)
    {
	const ElementMask &mask = region.get_valid_mask();
	void *valid_mask_base;
	for(size_t p = 0; p < mask.raw_size(); p += 4)
	  log_gpu.info("  raw mask data[%zd] = %08x\n", p,
		       ((unsigned *)(mask.get_raw()))[p>>2]);
        CHECK_CU( cuMemAlloc((cuDevicePtr*)(&valid_mask_base), mask.raw_size()) );
	log_gpu.info("copy of valid mask (%zd bytes) created at %p",
		     mask.raw_size(), valid_mask_base);
        CHECK_CU( cuMemcpyHtoD(vald_mask_base, 
                               mask.get_raw(),
                               mask.raw_size()) );
	return (unsigned *)&(((ElementMaskImpl *)valid_mask_base)->bits);
    }
#endif

    // Helper methods for emulating the cuda runtime
    /*static*/ GPUProcessor* GPUProcessor::find_local_gpu(void)
    {
      void *tls_val = gasnett_threadkey_get(gpu_thread_ptr);
      // This can return NULL during start-up
      if (tls_val == NULL)
        return NULL;
      assert(tls_val != NULL);
      GPUProcessor *local = (GPUProcessor*)tls_val;
      return local;
    }

    /*static*/ std::map<void*,void**>& GPUProcessor::get_deferred_modules(void)
    {
      static std::map<void*,void**> deferred_modules;
      return deferred_modules;
    }

    /*static*/ std::map<void*,void**>& GPUProcessor::get_deferred_cubins(void)
    {
      static std::map<void*,void**> deferred_cubins;
      return deferred_cubins;
    }

    /*static*/ std::deque<GPUProcessor::DeferredFunction>&
                                      GPUProcessor::get_deferred_functions(void)
    {
      static std::deque<DeferredFunction> deferred_functions;
      return deferred_functions;
    }

    /*static*/ std::deque<GPUProcessor::DeferredVariable>&
                                      GPUProcessor::get_deferred_variables(void)
    {
      static std::deque<DeferredVariable> deferred_variables;
      return deferred_variables;
    }

    void** GPUProcessor::internal_register_fat_binary(void *fat_bin)
    {
      void **handle = (void**)malloc(sizeof(void**));
      *handle = fat_bin;
      ModuleInfo &info = modules[handle];
      FatBin *fatbin_args = (FatBin*)fat_bin;
      assert((fatbin_args->data != NULL) ||
             (fatbin_args->filename_or_fatbins != NULL));
      if (fatbin_args->data != NULL)
      {
        load_module(&(info.module), fatbin_args->data);
      } else {
        CHECK_CU( cuModuleLoad(&(info.module), (const char*)fatbin_args->filename_or_fatbins) );
      }
      // add this to the list of local modules to be created during this task
      task_modules.insert(handle);
      return handle;
    }

    void** GPUProcessor::internal_register_cuda_binary(void *cubin)
    {
      void **handle = (void**)malloc(sizeof(void**));
      *handle = cubin;
      ModuleInfo &info = modules[handle];
      CHECK_CU( cuModuleLoadData(&(info.module), cubin) );
      // add this to the list of local modules to be created during this task
      task_modules.insert(handle);
      return handle;
    }

    /*static*/ void** GPUProcessor::defer_module_load(void *fat_bin)
    {
      void **handle = (void**)malloc(sizeof(void**));
      *handle = fat_bin;
      // Assume we don't need a lock here because all this is sequential
      get_deferred_modules()[fat_bin] = handle;
      return handle;
    }

    /*static*/ void** GPUProcessor::defer_cubin_load(void *cubin)
    {
      void **handle = (void**)malloc(sizeof(void**));
      *handle = cubin;
      // Assume we don't need a lock here because all this is sequential
      get_deferred_cubins()[cubin] = handle;
      return handle;
    }

    /*static*/ void** GPUProcessor::register_fat_binary(void *fat_bin)
    {
      GPUProcessor *local = find_local_gpu(); 
      // Ignore anything that goes on during start-up
      if (local == NULL)
        return GPUProcessor::defer_module_load(fat_bin);
      return local->internal_register_fat_binary(fat_bin);
    }

    /*static*/ void** GPUProcessor::register_cuda_binary(void *cubin,
                                                         size_t cubinSize)
    {
      void* cubinCopy = malloc(cubinSize);
      memcpy(cubinCopy, cubin, cubinSize);
      GPUProcessor *local = find_local_gpu();
      // Ignore anything that goes on during start-up
      if (local == NULL)
        return GPUProcessor::defer_cubin_load(cubinCopy);
      return local->internal_register_cuda_binary(cubinCopy);
    }

    void GPUProcessor::internal_unregister_fat_binary(void **fat_bin)
    {
      // It's never safe to unload our modules here
      // We need to wait until we synchronize the current
      // task stream with all the things that we've done
      CHECK_CU( cuCtxSynchronize() );
      std::map<void**,ModuleInfo>::iterator finder = modules.find(fat_bin);
      assert(finder != modules.end());
      CHECK_CU( cuModuleUnload(finder->second.module) );
      for (std::set<const void*>::const_iterator it = finder->second.host_aliases.begin();
            it != finder->second.host_aliases.end(); it++)
      {
        device_functions.erase(*it);
      }
      for (std::set<const void*>::const_iterator it = finder->second.var_aliases.begin();
            it != finder->second.var_aliases.end(); it++)
      {
        device_variables.erase(*it);
      }
      modules.erase(finder);
      free(fat_bin);
    }
    
    /*static*/ void GPUProcessor::unregister_fat_binary(void **fat_bin)
    {
      // Do nothing, our task contexts will clean themselves up after they are done
    }

    void GPUProcessor::internal_register_var(void **fat_bin, char *host_var,
                                             const char *device_name,
                                             bool ext, int size, bool constant, bool global)
    {
      std::map<void**,ModuleInfo>::iterator mod_finder = modules.find(fat_bin);
      assert(mod_finder != modules.end());
      ModuleInfo &info = mod_finder->second;
      // Check to see if we already have this symbol
      std::map<const void*,VarInfo>::const_iterator var_finder = 
        device_variables.find(host_var);
      if (var_finder == device_variables.end())
      {
        VarInfo target;
        CHECK_CU( cuModuleGetGlobal(&(target.ptr), &(target.size), info.module, device_name) );
        target.name = device_name;
        device_variables[host_var] = target;
        info.var_aliases.insert(host_var);
      }
    }

    /*static*/ void GPUProcessor::defer_variable_load(void **fat_bin,
                                                                char *host_var,
                                                                const char *device_name,
                                                                bool ext, int size,
                                                                bool constant, bool global)
    {
      DeferredVariable var;
      var.handle = fat_bin;
      var.host_var = host_var;
      var.device_name = device_name;
      var.external = ext;
      var.size = size;
      var.constant = constant;
      var.global = global;
      get_deferred_variables().push_back(var);
    }

    /*static*/ void GPUProcessor::register_var(void **fat_bin, char *host_var,
                                               char *device_addr, const char *device_name,
                                               int ext, int size, int constant, int global)
    {
      GPUProcessor *local = find_local_gpu();
      if (local == NULL)
      {
        GPUProcessor::defer_variable_load(fat_bin, host_var, device_name,
                                                    (ext == 1), size, 
                                                    (constant == 1), (global == 1));
        return;
      }
      local->internal_register_var(fat_bin, host_var, device_name, 
                                   (ext == 1), size, (constant == 1), (global == 1));
    }

    void GPUProcessor::internal_register_function(void **fat_bin, const char *host_fun,
                                                  const char *device_fun) 
    {
      // Check to see if we already loaded this function
      std::map<const void*,CUfunction>::const_iterator func_finder = 
        device_functions.find(host_fun);
      if (func_finder != device_functions.end())
        return;
      // Otherwise we need to load it
      std::map<void**,ModuleInfo>::iterator mod_finder = modules.find(fat_bin);
      assert(mod_finder != modules.end());
      ModuleInfo &info = mod_finder->second;      
      info.host_aliases.insert(host_fun);
      CUfunction *func = &(device_functions[host_fun]); 
      CHECK_CU( cuModuleGetFunction(func, info.module, device_fun) );
    }

    /*static*/ void GPUProcessor::defer_function_load(void **fat_bin,
                                                                const char *host_fun,
                                                                const char *device_fun)
    {
      DeferredFunction df;
      df.handle = fat_bin;
      df.host_fun = host_fun;
      df.device_fun = device_fun;
      // Assume we don't need a lock here
      get_deferred_functions().push_back(df); 
    }

    /*static*/ void GPUProcessor::register_function(void **fat_bin, const char *host_fun,
                                                    char *device_fun, const char *device_name,
                                                    int thread_limit, uint3 *tid, uint3 *bid,
                                                    dim3 *bDim, dim3 *gDim, int *wSize)
    {
      GPUProcessor *local = find_local_gpu();
      if (local == NULL)
      {
        GPUProcessor::defer_function_load(fat_bin, host_fun, device_fun);
        return;
      }
      local->internal_register_function(fat_bin, host_fun, device_fun);
    }

    char GPUProcessor::internal_init_module(void **fat_bin)
    {
      // We don't really care about managed runtimes
      return 1;
    }

    void GPUProcessor::load_module(CUmodule *module, const void *image)
    {
      const unsigned num_options = 4;
      CUjit_option jit_options[num_options];
      void*        option_vals[num_options];
      const size_t buffer_size = 16384;
      char* log_info_buffer = (char*)malloc(buffer_size);
      char* log_error_buffer = (char*)malloc(buffer_size);
      jit_options[0] = CU_JIT_INFO_LOG_BUFFER;
      jit_options[1] = CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES;
      jit_options[2] = CU_JIT_ERROR_LOG_BUFFER;
      jit_options[3] = CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES;
      option_vals[0] = log_info_buffer;
      option_vals[1] = (void*)buffer_size;
      option_vals[2] = log_error_buffer;
      option_vals[3] = (void*)buffer_size;
      CUresult result = cuModuleLoadDataEx(module, image, num_options, 
                                           jit_options, option_vals); 
      if (result != CUDA_SUCCESS)
      {
#ifdef __MACH__
        if (result == CUDA_ERROR_OPERATING_SYSTEM) {
          log_gpu.error("ERROR: Device side asserts are not supported by the "
                              "CUDA driver for MAC OSX, see NVBugs 1628896.");
        }
#endif
        if (result == CUDA_ERROR_NO_BINARY_FOR_GPU) {
          log_gpu.error("ERROR: The binary was compiled for the wrong GPU "
                              "architecture. Update the 'GPU_ARCH' flag at the top "
                              "of runtime/runtime.mk to match your current GPU "
                              "architecture.");
        }
        log_gpu.error("Failed to load CUDA module! Error log: %s", 
                log_error_buffer);
#if __CUDA_API_VERSION >= 6050
        const char *name, *str;
        CHECK_CU( cuGetErrorName(result, &name) );
        CHECK_CU( cuGetErrorString(result, &str) );
        fprintf(stderr,"CU: cuModuleLoadDataEx = %d (%s): %s\n",
                result, name, str);
#else
        fprintf(stderr,"CU: cuModuleLoadDataEx = %d\n", result);
#endif
        assert(0);
      }
      else
        log_gpu.info("Loaded CUDA Module. JIT Output: %s", log_info_buffer);
      free(log_info_buffer);
      free(log_error_buffer);
    }

    /*static*/ char GPUProcessor::init_module(void **fat_bin)
    {
      GPUProcessor *local = find_local_gpu();
      if (local == NULL)
        return 1;
      return local->internal_init_module(fat_bin);
    }

    // Helper methods for replacing the CUDA runtime API calls
    /*static*/ cudaError_t GPUProcessor::stream_create(cudaStream_t *stream)
    {
      log_gpu.error("Stream creation not permitted in Legion CUDA!");
      assert(false);
      return cudaSuccess;
    }

    /*static*/ cudaError_t GPUProcessor::stream_destroy(cudaStream_t stream)
    {
      log_gpu.error("Stream destruction not permitted in Legion CUDA!");
      assert(false);
      return cudaSuccess;
    }

    cudaError_t GPUProcessor::internal_stream_synchronize(void)
    {
      CUstream current = get_current_task_stream();
      CHECK_CU( cuStreamSynchronize(current) );
      return cudaSuccess;
    }

    /*static*/ cudaError_t GPUProcessor::stream_synchronize(cudaStream_t stream)
    {
      // Just ignore whatever stream they passed in and synchronize on our stream
      GPUProcessor *local = find_local_gpu();
      assert(local != NULL);
      return local->internal_stream_synchronize();
    }

    cudaError_t GPUProcessor::internal_configure_call(dim3 grid_dim,
                                                      dim3 block_dim,
                                                      size_t shared_mem)
    {
      LaunchConfig config;
      config.grid = grid_dim;
      config.block = block_dim;
      config.shared = shared_mem;
      launch_configs.push_back(config);
      return cudaSuccess;
    }

    /*static*/ cudaError_t GPUProcessor::configure_call(dim3 grid_dim, dim3 block_dim,
                                                        size_t shared_memory, 
                                                        cudaStream_t stream)
    {
      // Ignore their stream, it is meaningless
      GPUProcessor *local = find_local_gpu();
      assert(local != NULL);
      return local->internal_configure_call(grid_dim, block_dim, shared_memory);
    }

    cudaError_t GPUProcessor::internal_setup_argument(const void *arg,
                                                       size_t size, size_t offset)
    {
      const size_t required = offset + size;
      // If we need more memory, allocate it now
      if (required > kernel_buffer_size)
      {
        // Just make it twice as big to be safe
        size_t needed = required * 2;
        // Allocate a new buffer
        char *new_buffer; 
        CHECK_CU( cuMemAllocHost((void**)&new_buffer, needed) );
        // Copy over the old data
        memcpy(new_buffer, kernel_arg_buffer, kernel_arg_size);
        // Free the old buffer
        CHECK_CU( cuMemFreeHost(kernel_arg_buffer) );
        // Update our buffer
        kernel_arg_buffer = new_buffer;
        kernel_buffer_size = needed;
      }
      memcpy(kernel_arg_buffer+offset, arg, size);
      if (required > kernel_arg_size)
        kernel_arg_size = required;
      return cudaSuccess;
    }

    /*static*/ cudaError_t GPUProcessor::setup_argument(const void *arg, 
                                                        size_t size, size_t offset)
    {
      GPUProcessor *local = find_local_gpu();  
      assert(local != NULL);
      return local->internal_setup_argument(arg, size, offset);
    }

    cudaError_t GPUProcessor::internal_launch(const void *func)
    {
      // Ready to do the launch 
      assert(!launch_configs.empty());
      LaunchConfig &config = launch_configs.back();
      // Find our function
      CUfunction f;
      std::map<const void*,CUfunction>::const_iterator finder = 
        device_functions.find(func);
      if (finder != device_functions.end()) f = finder->second; else f = (CUfunction)func;
      void *args[] = { 
        CU_LAUNCH_PARAM_BUFFER_POINTER, kernel_arg_buffer,
        CU_LAUNCH_PARAM_BUFFER_SIZE, (void*)&kernel_arg_size,
        CU_LAUNCH_PARAM_END
      };

      // Launch the kernel on our stream dammit!
      CHECK_CU( cuLaunchKernel(f, config.grid.x, config.grid.y, config.grid.z,
                               config.block.x, config.block.y, config.block.z,
                               config.shared, get_current_task_stream(), NULL, args) );
      // Clean everything up from the launch
      launch_configs.pop_back();
      // Reset the kernel arg size
      kernel_arg_size = 0;
      return cudaSuccess;
    }

    /*static*/ cudaError_t GPUProcessor::launch(const void *func)
    {
      GPUProcessor *local = find_local_gpu();
      assert(local != NULL);
      return local->internal_launch(func); 
    }

    /*static*/ cudaError_t GPUProcessor::gpu_malloc(void **ptr, size_t size)
    {
      CHECK_CU( cuMemAlloc((CUdeviceptr*)ptr, size) );
      return cudaSuccess;
    }

    /*static*/ cudaError_t GPUProcessor::gpu_free(void *ptr)
    {
      CHECK_CU( cuMemFree((CUdeviceptr)ptr) );
      return cudaSuccess;
    }

    cudaError_t GPUProcessor::internal_gpu_memcpy(void *dst, const void *src,
                                                   size_t size, bool sync)
    {
      CUstream current = get_current_task_stream();
      CHECK_CU( cuMemcpyAsync((CUdeviceptr)dst, (CUdeviceptr)src, size, current) );
      if (sync)
      {
        CHECK_CU( cuStreamSynchronize(current) );
      }
      return cudaSuccess;
    }

    /*static*/ cudaError_t GPUProcessor::gpu_memcpy(void *dst, const void *src, 
                                                    size_t size, cudaMemcpyKind kind)
    {
      GPUProcessor *local = find_local_gpu();
      assert(local != NULL);
      return local->internal_gpu_memcpy(dst, src, size, true/*sync*/);
    }

    /*static*/ cudaError_t GPUProcessor::gpu_memcpy_async(void *dst, const void *src,
                                                          size_t size, cudaMemcpyKind kind,
                                                          cudaStream_t stream)
    {
      GPUProcessor *local = find_local_gpu();
      assert(local != NULL);
      return local->internal_gpu_memcpy(dst, src, size, false/*sync*/);
    }

    cudaError_t GPUProcessor::internal_gpu_memcpy_to_symbol(void *dst, const void *src,
                                                            size_t size, size_t offset,
                                                            cudaMemcpyKind kind, bool sync)
    {
      std::map<const void*,VarInfo>::const_iterator finder = device_variables.find(dst);
      assert(finder != device_variables.end());
      CUstream current = get_current_task_stream();
      CHECK_CU( cuMemcpyAsync(finder->second.ptr+offset, (CUdeviceptr)src, size, current) );
      if (sync)
      {
        CHECK_CU( cuStreamSynchronize(current) );
      }
      return cudaSuccess;
    }

    /*static*/ cudaError_t GPUProcessor::gpu_memcpy_to_symbol(void *dst, const void *src,
                                                              size_t size, size_t offset,
                                                              cudaMemcpyKind kind, bool sync)
    {
      GPUProcessor *local = find_local_gpu();
      assert(local != NULL);
      return local->internal_gpu_memcpy_to_symbol(dst, src, size, offset, kind, sync);
    }

    cudaError_t GPUProcessor::internal_gpu_memcpy_from_symbol(void *dst, const void *src,
                                                              size_t size, size_t offset,
                                                              cudaMemcpyKind kind, bool sync)
    {
      std::map<const void*,VarInfo>::const_iterator finder = device_variables.find(dst);
      assert(finder != device_variables.end());
      CUstream current = get_current_task_stream();
      CHECK_CU( cuMemcpyAsync((CUdeviceptr)dst, finder->second.ptr+offset, size, current) );
      if (sync)
      {
        CHECK_CU( cuStreamSynchronize(current) );
      }
      return cudaSuccess;
    }

    /*static*/ cudaError_t GPUProcessor::gpu_memcpy_from_symbol(void *dst, const void *src,
                                                                size_t size, size_t offset,
                                                                cudaMemcpyKind kind, bool sync)
    {
      GPUProcessor *local = find_local_gpu();
      assert(local != NULL);
      return local->internal_gpu_memcpy_from_symbol(dst, src, size, offset, kind, sync);
    }

    /*static*/ cudaError_t GPUProcessor::device_synchronize(void)
    {
      // Users are dumb, never let them mess us up
      GPUProcessor *local = find_local_gpu();
      assert(local != NULL);
      return local->internal_stream_synchronize();
    }

    /*static*/ cudaError_t GPUProcessor::set_shared_memory_config(cudaSharedMemConfig config)
    {
      CHECK_CU( cuCtxSetSharedMemConfig(
        (config == cudaSharedMemBankSizeDefault) ? CU_SHARED_MEM_CONFIG_DEFAULT_BANK_SIZE :
        (config == cudaSharedMemBankSizeFourByte) ? CU_SHARED_MEM_CONFIG_FOUR_BYTE_BANK_SIZE :
                                                    CU_SHARED_MEM_CONFIG_EIGHT_BYTE_BANK_SIZE) );
      return cudaSuccess;
    }

  }; // namespace LowLevel
}; // namespace LegionRuntime

// Our implementation of the CUDA runtime API for Legion
// so we can intercept all of these calls

// All these extern C methods are for internal implementations
// of functions of the cuda runtime API that nvcc assumes
// exists and can be used for code generation. They are all
// pretty simple to map to the driver API.

extern "C" void** __cudaRegisterFatBinary(void *fat_bin)
{
  return LegionRuntime::LowLevel::GPUProcessor::register_fat_binary(fat_bin);
}

// this is not really a part of CUDA runtime API but used by the regent compiler
extern "C" void** __cudaRegisterCudaBinary(void *cubin, size_t cubinSize)
{
  return LegionRuntime::LowLevel::GPUProcessor::register_cuda_binary(cubin,
                                                                     cubinSize);
}

extern "C" void __cudaUnregisterFatBinary(void **fat_bin)
{
  LegionRuntime::LowLevel::GPUProcessor::unregister_fat_binary(fat_bin);
}

extern "C" void __cudaRegisterVar(void **fat_bin,
                                  char *host_var,
                                  char *device_addr,
                                  const char *device_name,
                                  int ext, int size, int constant, int global)
{
  LegionRuntime::LowLevel::GPUProcessor::register_var(fat_bin, host_var, device_addr,
                                                      device_name, ext, size, 
                                                      constant, global);
}

extern "C" void __cudaRegisterFunction(void **fat_bin,
                                       const char *host_fun,
                                       char *device_fun,
                                       const char *device_name,
                                       int thread_limit,
                                       uint3 *tid, uint3 *bid,
                                       dim3 *bDim, dim3 *gDim,
                                       int *wSize)
{
  LegionRuntime::LowLevel::GPUProcessor::register_function(fat_bin, host_fun,
                                                           device_fun, device_name,
                                                           thread_limit, tid, bid,
                                                           bDim, gDim, wSize);
}

extern "C" char __cudaInitModule(void **fat_bin)
{
  return LegionRuntime::LowLevel::GPUProcessor::init_module(fat_bin);
}

// All the following methods are cuda runtime API calls that we 
// intercept and then either execute using the driver API or 
// modify in ways that are important to Legion.

extern cudaError_t cudaStreamCreate(cudaStream_t *stream)
{
  return LegionRuntime::LowLevel::GPUProcessor::stream_create(stream);
}

extern cudaError_t cudaStreamDestroy(cudaStream_t stream)
{
  return LegionRuntime::LowLevel::GPUProcessor::stream_destroy(stream);
}

extern cudaError_t cudaStreamSynchronize(cudaStream_t stream)
{
  return LegionRuntime::LowLevel::GPUProcessor::stream_synchronize(stream);
}

extern cudaError_t cudaConfigureCall(dim3 grid_dim,
                                     dim3 block_dim,
                                     size_t shared_memory,
                                     cudaStream_t stream)
{
  return LegionRuntime::LowLevel::GPUProcessor::configure_call(grid_dim, block_dim,
                                                               shared_memory, stream);
}

extern cudaError_t cudaSetupArgument(const void *arg,
                                     size_t size,
                                     size_t offset)
{
  return LegionRuntime::LowLevel::GPUProcessor::setup_argument(arg, size, offset);
}

extern cudaError_t cudaLaunch(const void *func)
{
  return LegionRuntime::LowLevel::GPUProcessor::launch(func);
}

extern cudaError_t cudaMalloc(void **ptr, size_t size)
{
  return LegionRuntime::LowLevel::GPUProcessor::gpu_malloc(ptr, size);
}

extern cudaError_t cudaFree(void *ptr)
{
  return LegionRuntime::LowLevel::GPUProcessor::gpu_free(ptr);
}

extern cudaError_t cudaMemcpy(void *dst, const void *src, 
                              size_t size, cudaMemcpyKind kind)
{
  return LegionRuntime::LowLevel::GPUProcessor::gpu_memcpy(dst, src, size, kind);
}

extern cudaError_t cudaMemcpyAsync(void *dst, const void *src,
                                   size_t size, cudaMemcpyKind kind,
                                   cudaStream_t stream)
{
  return LegionRuntime::LowLevel::GPUProcessor::gpu_memcpy_async(dst, src, size, kind, stream);
}

extern cudaError_t cudaDeviceSynchronize(void)
{
  return LegionRuntime::LowLevel::GPUProcessor::device_synchronize();
}

extern cudaError_t cudaMemcpyToSymbol(void *dst, const void *src,
                                      size_t size, size_t offset,
                                      cudaMemcpyKind kind)
{
  return LegionRuntime::LowLevel::GPUProcessor::gpu_memcpy_to_symbol(dst, src, size, 
                                                      offset, kind, true/*sync*/);
}

extern cudaError_t cudaMemcpyToSymbol(void *dst, const void *src,
                                      size_t size, size_t offset,
                                      cudaMemcpyKind kind, cudaStream_t stream)
{
  return LegionRuntime::LowLevel::GPUProcessor::gpu_memcpy_to_symbol(dst, src, size,
                                                        offset, kind, false/*sync*/);
}

extern cudaError_t cudaMemcpyFromSymbol(void *dst, const void *src,
                                        size_t size, size_t offset,
                                        cudaMemcpyKind kind)
{
  return LegionRuntime::LowLevel::GPUProcessor::gpu_memcpy_from_symbol(dst, src, size,
                                                        offset, kind, true/*sync*/);
}

extern cudaError_t cudaMemcpyFromSymbolAsync(void *dst, const void *src,
                                             size_t size, size_t offset,
                                             cudaMemcpyKind kind, cudaStream_t stream)
{
  return LegionRuntime::LowLevel::GPUProcessor::gpu_memcpy_from_symbol(dst, src, size,
                                                        offset, kind, false/*sync*/);
}

extern cudaError_t cudaDeviceSetSharedMemConfig(cudaSharedMemConfig config)
{
  return LegionRuntime::LowLevel::GPUProcessor::set_shared_memory_config(config);
}

