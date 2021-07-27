/* Copyright 2015-2017 Philippe Tillet
* 
* Permission is hereby granted, free of charge, to any person obtaining 
* a copy of this software and associated documentation files 
* (the "Software"), to deal in the Software without restriction, 
* including without limitation the rights to use, copy, modify, merge, 
* publish, distribute, sublicense, and/or sell copies of the Software, 
* and to permit persons to whom the Software is furnished to do so, 
* subject to the following conditions:
* 
* The above copyright notice and this permission notice shall be 
* included in all copies or substantial portions of the Software.
* 
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <cassert>
#include <unistd.h>
#include <array>
#include "triton/driver/backend.h"
#include "triton/driver/stream.h"
#include "triton/driver/context.h"
#include "triton/driver/device.h"
#include "triton/driver/event.h"
#include "triton/driver/kernel.h"
#include "triton/driver/buffer.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/GenericValue.h"

namespace triton
{

namespace driver
{

/* ------------------------ */
//         Base             //
/* ------------------------ */

stream::stream(driver::context *ctx, CUstream cu, bool has_ownership)
  : polymorphic_resource(cu, has_ownership), ctx_(ctx) {
}


stream::stream(driver::context *ctx, host_stream_t cl, bool has_ownership)
  : polymorphic_resource(cl, has_ownership), ctx_(ctx) {
}

driver::stream* stream::create(driver::context* ctx) {
  switch(ctx->backend()){
    case CUDA: return new cu_stream(ctx);
    case Host: return new host_stream(ctx);
    default: throw std::runtime_error("unknown backend");
  }
}

driver::context* stream::context() const {
  return ctx_;
}

/* ------------------------ */
//          Host            //
/* ------------------------ */

host_stream::host_stream(driver::context *ctx): stream(ctx, host_stream_t(), true) {
  hst_->pool.reset(new ThreadPool(1));
  hst_->futures.reset(new std::vector<std::future<void>>());
}

void host_stream::synchronize() {
  for(auto& x: *hst_->futures)
    x.wait();
  hst_->futures->clear();
  hst_->args.clear();
}

void host_stream::enqueue(driver::kernel* kernel, std::array<size_t, 3> grid, std::array<size_t, 3> block, std::vector<event> const *, event* event, void **args, size_t args_size) {
  auto hst = kernel->module()->hst();
  char* params = new char[args_size];
  std::memcpy((void*)params, (void*)args, args_size);
  for(size_t i = 0; i < grid[0]; i++)
    for(size_t j = 0; j < grid[1]; j++)
      for(size_t k = 0; k < grid[2]; k++)
        hst_->futures->emplace_back(hst_->pool->enqueue(hst->fn, (char**)params, int32_t(i), int32_t(j), int32_t(k)));
}

void host_stream::write(driver::buffer* buffer, bool blocking, std::size_t offset, std::size_t size, void const* ptr) {
  std::memcpy((void*)buffer->hst()->data, ptr, size);
}

void host_stream::read(driver::buffer* buffer, bool blocking, std::size_t offset, std::size_t size, void* ptr) {
  std::memcpy(ptr, (const void*)buffer->hst()->data, size);
}


/* ------------------------ */
//         CUDA             //
/* ------------------------ */

inline CUcontext get_context() {
  CUcontext result;
  dispatch::cuCtxGetCurrent(&result);
  return result;
}

cu_stream::cu_stream(CUstream str, bool take_ownership):
  stream(backend::contexts::import(get_context()), str, take_ownership) {
}

cu_stream::cu_stream(driver::context *context): stream((driver::cu_context*)context, CUstream(), true) {
  cu_context::context_switcher ctx_switch(*ctx_);
  dispatch::cuStreamCreate(&*cu_, 0);
}

void cu_stream::synchronize() {
  cu_context::context_switcher ctx_switch(*ctx_);
  dispatch::cuStreamSynchronize(*cu_);
}

void cu_stream::enqueue(driver::kernel* kernel, std::array<size_t, 3> grid, std::array<size_t, 3> block, std::vector<event> const *, event* event, void** args, size_t args_size) {
  cu_context::context_switcher ctx_switch(*ctx_);
  void *config[] = {
      CU_LAUNCH_PARAM_BUFFER_POINTER, args,
      CU_LAUNCH_PARAM_BUFFER_SIZE,    &args_size,
      CU_LAUNCH_PARAM_END
  };
  if(event)
    dispatch::cuEventRecord(event->cu()->first, *cu_);
  dispatch::cuLaunchKernel(*kernel->cu(), grid[0], grid[1], grid[2], block[0], block[1], block[2], 0, *cu_, nullptr, config);
  if(event)
    dispatch::cuEventRecord(event->cu()->second, *cu_);
}

void cu_stream::write(driver::buffer* buffer, bool blocking, std::size_t offset, std::size_t size, void const* ptr) {
  cu_context::context_switcher ctx_switch(*ctx_);
  if(blocking)
    dispatch::cuMemcpyHtoD(*buffer->cu() + offset, ptr, size);
  else
    dispatch::cuMemcpyHtoDAsync(*buffer->cu() + offset, ptr, size, *cu_);
}

void cu_stream::read(driver::buffer* buffer, bool blocking, std::size_t offset, std::size_t size, void* ptr) {
  cu_context::context_switcher ctx_switch(*ctx_);
  if(blocking)
    dispatch::cuMemcpyDtoH(ptr, *buffer->cu() + offset, size);
  else
    dispatch::cuMemcpyDtoHAsync(ptr, *buffer->cu() + offset, size, *cu_);
}


}

}
