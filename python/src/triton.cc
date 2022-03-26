﻿// #include "triton/codegen/pass.h"
// #include "triton/codegen/target.h"
#include "triton/driver/error.h"
#include "triton/driver/llvm.h"

#include "mlir/IR/Builders.h"
#include "mlir-c/IR.h"
#include "mlir-c/BuiltinTypes.h"
#include "mlir/CAPI/IR.h"
// #include "mlir/IR/BuiltinOps.h"
// #include "mlir/IR/MLIRContext.h"

#include "triton/ir/Dialect.h"
#include "triton/ir/Types.h"

#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"

#include <optional>
#include <pybind11/buffer_info.h>
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl_bind.h>
#include <pybind11/stl.h>
#include "Python.h"
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace py = pybind11;
// namespace ir = triton::ir;
namespace drv = triton::driver;


/*****************************************************************************/
/* Python bindings for triton::driver                                        */
/*****************************************************************************/
// information query
template<CUdevice_attribute attr>
int cuGetInfo(CUdevice device) {
  int res;
  drv::dispatch::cuDeviceGetAttribute(&res, attr, device);
  return res;
}

template<hipDeviceAttribute_t attr>
int hipGetInfo(hipDevice_t device) {
  int res;
  drv::dispatch::hipDeviceGetAttribute(&res, attr, device);
  return res;
}

enum backend_t {
  HOST,
  CUDA,
  ROCM,
};

void cu_enable_peer_access(uint64_t peer_ptr){
  CUcontext context;
  drv::dispatch::cuPointerGetAttribute(&context, CU_POINTER_ATTRIBUTE_CONTEXT, peer_ptr);
  try {
      drv::dispatch::cuCtxEnablePeerAccess(context, 0);
  } catch (drv::exception::cuda::peer_access_already_enabled) {}
}

void host_enqueue(uint64_t stream, uint64_t kernel,
                  uint64_t grid_0, uint64_t grid_1, uint64_t grid_2,
                  uint64_t block_0, uint64_t block_1, uint64_t block_2,
                  void* args_ptr, size_t args_size, int64_t shared_mem){
  throw std::runtime_error("unsupported");
// auto hst = kernel->module()->hst();
// hst_->futures->reserve(hst_->futures->size() + grid[0]*grid[1]*grid[2]);
// char* params = new char[args_size];
// std::memcpy((void*)params, (void*)args, args_size);
// for(size_t i = 0; i < grid[0]; i++)
//   for(size_t j = 0; j < grid[1]; j++)
//     for(size_t k = 0; k < grid[2]; k++)
//       hst_->futures->emplace_back(hst_->pool->enqueue(hst->fn, (char**)params, int32_t(i), int32_t(j), int32_t(k)));
}

void cu_enqueue(uint64_t stream, uint64_t kernel,
                uint64_t grid_0, uint64_t grid_1, uint64_t grid_2,
                uint64_t block_0, uint64_t block_1, uint64_t block_2,
                void* args_ptr, size_t args_size, int64_t shared_mem){
  void *config[] = {
      CU_LAUNCH_PARAM_BUFFER_POINTER, (void*)args_ptr,
      CU_LAUNCH_PARAM_BUFFER_SIZE,    &args_size,
      CU_LAUNCH_PARAM_END
  };
  drv::dispatch::cuLaunchKernel((CUfunction)kernel, grid_0, grid_1, grid_2, 
                                block_0, block_1, block_2, 
                                shared_mem, (CUstream)stream, nullptr, config);
}

void hip_enqueue(uint64_t stream, uint64_t kernel,
                uint64_t grid_0, uint64_t grid_1, uint64_t grid_2,
                uint64_t block_0, uint64_t block_1, uint64_t block_2,
                void* args_ptr, size_t args_size, int64_t shared_mem) {
  void *config[] = {
      HIP_LAUNCH_PARAM_BUFFER_POINTER, (void*)args_ptr,
      HIP_LAUNCH_PARAM_BUFFER_SIZE,    &args_size,
      HIP_LAUNCH_PARAM_END
  };
  drv::dispatch::hipModuleLaunchKernel((hipFunction_t)kernel, grid_0, grid_1, grid_2, 
                                block_0, block_1, block_2, 
                                shared_mem, (hipStream_t)stream, nullptr, config);

}

long pow2_divisor(long N){
    if(N % 16 == 0) return 16;
    if(N % 8 == 0) return 8;
    if(N % 4 == 0) return 4;
    if(N % 2 == 0) return 2;
    return 1;
}

// Returns something like "int16", whether dtype is a torch.dtype or
// triton.language.dtype.
std::string dtype_cache_key_part(const py::object& dtype) {
  if (py::hasattr(dtype, "cache_key_part")) {
    // Presumed to be a triton.language.dtype.
    return std::string(py::str(py::getattr(dtype, "cache_key_part")));
  } else {
    // Remove 'torch.' prefix from repr of torch.dtype.
    py::object repr = py::repr(dtype);
    size_t repr_len = PyUnicode_GET_LENGTH(repr.ptr());
    const char* repr_ptr = (const char*)PyUnicode_1BYTE_DATA(repr.ptr());
    if (repr_len <= 6 || strncmp(repr_ptr, "torch.", 6)) {
      throw std::logic_error("invalid dtype: " + std::string(repr_ptr, repr_len));
    }
    return std::string(repr_ptr + 6, repr_len - 6);
  }
}

size_t get_pointer_range_size(uint64_t addr){
  if(addr == 0)
    return 0;
  size_t size;
  drv::dispatch::cuPointerGetAttribute(&size, CU_POINTER_ATTRIBUTE_RANGE_SIZE, (CUdeviceptr)addr);
  return size;
}

// Launch
void parse_args(py::list& args, py::list do_not_specialize, const std::string& func_key, py::list& arg_names,
                std::string& cache_key, std::string& params, size_t& params_size, py::dict constants,
                int num_warps, int num_stages) {
    size_t len = PyList_Size(args.ptr());
    params.reserve(8*len); // 8 max bytes by argument
    char* params_ptr = &params[0];
    cache_key = func_key;
    cache_key += "-" + std::to_string(num_warps);
    cache_key += "-" + std::to_string(num_stages);
    cache_key += "-";
    for(int i = 0; i < len; i++){
      cache_key += "_";
      py::int_ py_i = py::int_(i);
      bool specialize = !do_not_specialize.contains(py_i);
      py::object arg = args[i];
      auto arg_ptr = arg.ptr();

      // argument is `long`
      if(PyLong_Check(arg_ptr)){
        int overflow;
        long long value = PyLong_AsLongLongAndOverflow(arg_ptr, &overflow);
        // values equal to 1 are specialized
        if(specialize && (value == 1)){
          cache_key += "1";
          continue;
        }
        // int32, uint32, int64, and uint64 have different kernels
        if (!overflow && -0x8000'0000LL <= value && value <= 0x7FFF'FFFFLL) {
          cache_key += "int32";
          params_ptr = (char*)(((uintptr_t)params_ptr + 3) & (-4));
          std::memcpy(params_ptr, &value, 4);
          params_ptr += 4;
        } else if (!overflow && 0x8000'0000LL <= value && value <= 0xFFFF'FFFFLL) {
          cache_key += "uint32";
          params_ptr = (char*)(((uintptr_t)params_ptr + 3) & (-4));
          std::memcpy(params_ptr, &value, 4);
          params_ptr += 4;
        } else if (!overflow) {
          cache_key += "int64";
          params_ptr = (char*)(((uintptr_t)params_ptr + 7) & (-8));
          std::memcpy(params_ptr, &value, 8);
          params_ptr += 8;
        } else {
          if (PyErr_Occurred()) {
            throw std::logic_error("An error occurred?");
          }
          unsigned long long unsigned_value = PyLong_AsUnsignedLongLong(arg_ptr);
          if (PyErr_Occurred()) {
            throw std::runtime_error("integer overflow in argument: " + std::string(py::str(arg)));
          }
          cache_key += "uint64";
          params_ptr = (char*)(((uintptr_t)params_ptr + 7) & (-8));
          std::memcpy(params_ptr, &unsigned_value, 8);
          params_ptr += 8;
        }
        if(!specialize)
          continue;
        // values divisible by small powers of 2 are specialized
        cache_key += "[multipleof(";
        cache_key += std::to_string(pow2_divisor(value));
        cache_key += ")]";
        continue;
      }
      // argument is `float`
      if(PyFloat_Check(arg_ptr)){
        cache_key += "float32";
        float value = PyFloat_AsDouble(arg_ptr);
        params_ptr = (char*)(((uintptr_t)params_ptr + 3) & (-4));
        std::memcpy(params_ptr, &value, 4);
        params_ptr += 4;
        continue;
      }
      // argument is `bool`
      if(PyBool_Check(arg_ptr)){
        cache_key += "bool";
        bool value =  arg_ptr == Py_True ? true : false;
        std::memcpy(params_ptr, &value, 1);
        params_ptr += 1;
        continue;
      }
      // argument is tensor
      if(py::hasattr(arg, "data_ptr")){
        py::object data_ptr = arg.attr("data_ptr")();
        long value = data_ptr.cast<long>();
        params_ptr = (char*)(((uintptr_t)params_ptr + 7) & (-8));
        // copy param
        std::memcpy(params_ptr, &value, 8);
        params_ptr += 8;
        // udpate cache key
        cache_key += dtype_cache_key_part(arg.attr("dtype"));
        cache_key += "*";
        cache_key += "[multipleof(";
        size_t range_size = get_pointer_range_size(value);
        cache_key += std::to_string(std::min(pow2_divisor(value), pow2_divisor(range_size)));
        cache_key += ")]";
        continue;
      }
      // argument is `constexpr`
      if(py::hasattr(arg, "value")){
        py::object value = arg.attr("value");
        py::object name = arg_names[i];
        constants[name] = value;
        py::object repr = py::repr(value);
        const char* start = (const char*)PyUnicode_1BYTE_DATA(repr.ptr());
        size_t len = PyUnicode_GET_LENGTH(repr.ptr());
        cache_key += std::string(start, len);
        continue;
      }
      std::string ty_str = arg.attr("__class__").attr("__name__").cast<std::string>();
      if(ty_str == "NoneType"){
        cache_key += "None";
        continue;
      }
      std::string err_msg = "Received type '" + ty_str + "' for argument " + std::to_string(i) + "."
                            + " Only int, float, bool, torch.Tensor, and triton.language.constexpr are supported.";
      throw std::runtime_error(err_msg);
    }
  params_size = (std::ptrdiff_t)(params_ptr - &params[0]);
}

//

void init_triton_runtime(py::module &&m) {

  // m.def("current_stream", [](uint64_t device){
  //   return (uint64_t)(c10::cuda::getCurrentCUDAStream(device).stream());
  // });

  // wrap backend_t
  py::enum_<backend_t>(m, "backend")
    .value("HOST", HOST)
    .value("CUDA", CUDA)
    .value("ROCM", ROCM)
    .export_values();

  // enable peer-to-peer
  m.def("enable_peer_access", [](backend_t backend, uint64_t peer_ptr) {
      if (backend != CUDA)
        throw std::runtime_error("P2P only supported on CUDA devices!");
      cu_enable_peer_access(peer_ptr);
    }
  );

  // get range size for the given pointer
  m.def("get_pointer_range_size", &get_pointer_range_size);


  // cache key
  m.def("launch", [](py::list args, py::list do_not_specialize, const std::string& func_key, py::list& arg_names, 
                     py::object device, py::int_ stream, py::dict bin_cache, py::int_ num_warps, py::int_ num_stages, 
                     py::function add_to_cache, py::object grid){
    // parse arguments to compute cache key, compile-time constants and packed kernel arguments
    long _num_warps = PyLong_AsLong(num_warps.ptr());
    long _num_stages = PyLong_AsLong(num_stages.ptr());
    std::string cache_key;
    std::string params;
    size_t params_size;
    py::dict constants;
    parse_args(args, do_not_specialize, func_key, arg_names, cache_key, params, params_size, constants, _num_warps, _num_stages);

    // get cached binary
    py::str key(cache_key);
    py::bool_ noop = false;
    if(!bin_cache.contains(key)) {
      noop = add_to_cache(key, args, device, num_warps, num_stages);
    }
    if (noop)
      return (py::object)py::none();
    py::object bin = bin_cache[key];

    // get grid
    py::sequence seq;
    if(!PySequence_Check(grid.ptr()))
      seq = grid(constants);
    else
      seq = grid;
    int size = seq.size();
    int grid_0 = py::cast<int>(seq[0]);
    int grid_1 = size < 2 ? 1 : py::cast<int>(seq[1]);
    int grid_2 = size < 3 ? 1 : py::cast<int>(seq[2]);

    // enqueue
    uint64_t kernel = py::cast<uint64_t>(bin.attr("kernel"));
    uint64_t shared_mem = py::cast<uint64_t>(bin.attr("shared_mem"));

    // actually launch
    void *config[] = {
        CU_LAUNCH_PARAM_BUFFER_POINTER, params.data(),
        CU_LAUNCH_PARAM_BUFFER_SIZE, &params_size,
        CU_LAUNCH_PARAM_END
    };
    uint64_t _stream = PyLong_AsLong(stream.ptr());
    if(grid_0*grid_1*grid_2 > 0) {
      // release the gil in case the enqueue blocks
      // cuda will block if too many ops are enqueued
      py::gil_scoped_release allow_threads;
      drv::dispatch::cuLaunchKernel((CUfunction)kernel, grid_0, grid_1, grid_2, 
                                    _num_warps*32, 1, 1, shared_mem, (CUstream)_stream, 
                                     nullptr, config);
   }
    return bin;
  });

  m.def("cc", [](backend_t backend, uint64_t device) -> int {
    if (backend == CUDA) {
      CUdevice dev = (CUdevice)device;
      int major = cuGetInfo<CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR>(dev);
      int minor = cuGetInfo<CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR>(dev);
      return major*10 + minor;
    }
    return -1;
  });

  // query maximum shared memory
  m.def("max_shared_memory", [](backend_t backend, uint64_t device) {
      if (backend == HOST)
        return 0;
      if(backend == CUDA) 
        return cuGetInfo<CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK_OPTIN>(device);
      if(backend == ROCM)
        return hipGetInfo<hipDeviceAttributeMaxSharedMemoryPerBlock>(device);
      return -1;
  });

  // query DRAM & L2 cache
  m.def("memory_clock_rate", [](backend_t backend, uint64_t device) {
    if (backend == CUDA) return cuGetInfo<CU_DEVICE_ATTRIBUTE_MEMORY_CLOCK_RATE>(device);
    return -1;
  });
  m.def("global_memory_bus_width", [](backend_t backend, uint64_t device) {
    if (backend == CUDA) return cuGetInfo<CU_DEVICE_ATTRIBUTE_GLOBAL_MEMORY_BUS_WIDTH>(device);
    return -1;
  });
  m.def("l2_cache_size", [](backend_t backend, uint64_t device) {
    if (backend == CUDA) return cuGetInfo<CU_DEVICE_ATTRIBUTE_L2_CACHE_SIZE>(device);
    return -1;
  });

  // query clock rate (in kilohertz)
  m.def("clock_rate", [](backend_t backend, uint64_t device) {
    if (backend == CUDA) return cuGetInfo<CU_DEVICE_ATTRIBUTE_CLOCK_RATE>(device);
    return -1;
  });

  m.def("num_sm", [](backend_t backend, uint64_t device) {
    if (backend == CUDA) return cuGetInfo<CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT>(device);
    return -1;
  });

  // enqueue
  m.def("enqueue", [](backend_t backend, uint64_t stream, uint64_t kernel,
                      uint64_t grid_0, uint64_t grid_1, uint64_t grid_2,
                      uint64_t block_0, uint64_t block_1, uint64_t block_2,
                      const std::string &args, int64_t shared_mem){
    void* args_ptr = (void*)args.data();
    size_t args_size = args.size();
    // release the gil in case the enqueue blocks
    // cuda will block if too many ops are enqueued
    py::gil_scoped_release allow_threads;
    if(backend == HOST)
      host_enqueue(stream, kernel, grid_0, grid_1, grid_2, block_0, block_1, block_2, args_ptr, args_size, shared_mem);
    if(backend == CUDA)
      cu_enqueue(stream, kernel, grid_0, grid_1, grid_2, block_0, block_1, block_2, args_ptr, args_size, shared_mem);
    if(backend == ROCM)
      hip_enqueue(stream, kernel, grid_0, grid_1, grid_2, block_0, block_1, block_2, args_ptr, args_size, shared_mem);
  });

  
}

/*****************************************************************************/
/* Python bindings for triton::codegen                                       */
/*****************************************************************************/
typedef std::map<std::string, py::object> asm_map_t;

// --------------------------------------- 
// Load provided assembly code into driver
// --------------------------------------- 

// CUDA
std::tuple<uint64_t, uint64_t> cu_load_binary(const std::string& name, asm_map_t &asm_map, size_t n_shared_bytes, uint64_t dev){
  // load assembly
  std::string assembly;
  if(asm_map.find("cubin") != asm_map.end())
    assembly = py::cast<std::string>(asm_map["cubin"]);
  else
    assembly = py::cast<std::string>(asm_map["ptx"]);
  // create driver handles
  CUfunction fun;
  CUmodule mod;
  drv::dispatch::cuModuleLoadData(&mod, assembly.c_str());
  drv::dispatch::cuModuleGetFunction(&fun, mod, name.c_str());
  // set dynamic shared memory if necessary
  int shared_optin;
  drv::dispatch::cuDeviceGetAttribute(&shared_optin, CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK_OPTIN, dev);
  if(n_shared_bytes > 49152 && shared_optin > 49152){
    drv::dispatch::cuFuncSetCacheConfig(fun, CU_FUNC_CACHE_PREFER_SHARED);
    int shared_total, shared_static;
    int n_spills, n_reg;
    drv::dispatch::cuDeviceGetAttribute(&shared_total, CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_MULTIPROCESSOR, dev);
    drv::dispatch::cuFuncGetAttribute(&shared_static, CU_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES, fun);
    drv::dispatch::cuFuncGetAttribute(&n_spills, CU_FUNC_ATTRIBUTE_LOCAL_SIZE_BYTES,  fun);
    drv::dispatch::cuFuncGetAttribute(&n_reg, CU_FUNC_ATTRIBUTE_NUM_REGS, fun);
    drv::dispatch::cuFuncSetAttribute(fun, CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES, shared_optin - shared_static);
  }
  return std::make_tuple((uint64_t)mod, (uint64_t)fun);
}

// ROCM
std::tuple<uint64_t, uint64_t> hip_load_binary(const std::string& name, asm_map_t &asm_map, size_t n_shared_bytes, uint64_t dev){
  py::bytes _assembly = asm_map["hsaco"];
  std::string assembly = py::cast<std::string>(_assembly);
  // HSA-CO -> hipModule
  hipModule_t mod = drv::amdgpu_to_hipmodule(assembly);
  // Handle to the kernel
  hipFunction_t fun;
  drv::dispatch::hipModuleGetFunction(&fun, mod, name.c_str());
  // record asm
  return std::make_tuple((uint64_t)mod, (uint64_t)fun);
}

// --------------------------------------- 
// Compile Triton-IR to assembly
// --------------------------------------- 

// // CUDA
// std::tuple<std::string, asm_map_t, int> cu_compile_ttir(const std::string& name, ir::module &ir, 
//                                                                uint64_t device, int num_warps, int num_stages,
//                                                                asm_map_t &asm_map){

//   int n_shared_bytes;
//   py::gil_scoped_release allow_threads;
//   llvm::LLVMContext ctx;
//   // device properties
//   CUdevice dev = (CUdevice)device;
//   size_t major = cuGetInfo<CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR>(dev);
//   size_t minor = cuGetInfo<CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR>(dev);
//   size_t cc = major*10 + minor;
//   int version;
//   std::string ptxas_path = drv::path_to_ptxas(version);
//   // Triton-IR -> NVPTX LLVM-IR
//   triton::codegen::nvidia_cu_target target(cc);
//   auto llvm = triton::codegen::add_passes_to_emit_bin(ir, ctx, &target, cc, num_warps, num_stages, n_shared_bytes);
//   std::string tmp;
//   llvm::raw_string_ostream llir(tmp);
//   llir << *llvm;
//   llir.flush();
//   asm_map["llir"] = py::cast(tmp);
//   // LLVM-IR -> PTX
//   std::string ptx = drv::llir_to_ptx(llvm.get(), cc, version);
//   asm_map["ptx"] = py::cast(ptx);
//   // PTX -> Binary
//   std::string cubin = drv::ptx_to_cubin(ptx, ptxas_path, cc);
//   if(!cubin.empty()){
//     py::bytes bytes(cubin);
//     asm_map["cubin"] = bytes;
//   }
//   return std::make_tuple(name, asm_map, n_shared_bytes);
// }

// // HIP
// std::tuple<std::string, asm_map_t, int> hip_compile_ttir(const std::string& name, ir::module &ir, 
//                                                                 uint64_t device, int num_warps, int num_stages, 
//                                                                 asm_map_t &asm_map){
//   llvm::LLVMContext ctx;
//   // Triton-IR -> NVPTX LLVM-IR
//   triton::codegen::amd_cl_target target;
//   int n_shared_bytes;
//   auto llvm = triton::codegen::add_passes_to_emit_bin(ir, ctx, &target, 70, num_warps, num_stages, n_shared_bytes);
//   std::string tmp;
//   llvm::raw_string_ostream llir(tmp);
//   llir << *llvm;
//   llir.flush();
//   asm_map["llir"] = py::cast(tmp);
//   // LLVM-IR -> HSA-CO
//   std::string path = drv::llir_to_amdgpu(llvm.get(), "gfx908");
//   asm_map["hsaco"] = py::cast(path);
//   return std::make_tuple(name, asm_map, n_shared_bytes);
// }

// void init_triton_codegen(py::module &&m) {
//   m.def(
//       "compile_ttir", [](backend_t backend, ir::module &ir, uint64_t device, int num_warps, int num_stages) {
//         std::string name = ir.get_function_list()[0]->get_name();
//         // record asm as we generate
//         asm_map_t asm_map;
//         std::ostringstream ttir;
//         ir.print(ttir);
//         asm_map["ttir"] = py::cast(ttir.str());
//         llvm::LLVMContext ctx;
//         if(backend == CUDA)
//           return cu_compile_ttir(name, ir, device, num_warps, num_stages, asm_map);
//         if(backend == ROCM)
//           return hip_compile_ttir(name, ir, device, num_warps, num_stages, asm_map);
//       }, py::return_value_policy::take_ownership);
//   m.def("load_binary", [](backend_t backend, const std::string& name, asm_map_t &asm_map, size_t n_shared_bytes, uint64_t dev){
// 	py::gil_scoped_release allow_threads;
//         if(backend == CUDA)
//           return cu_load_binary(name, asm_map, n_shared_bytes, dev);
//         if(backend == ROCM)
//           return hip_load_binary(name, asm_map, n_shared_bytes, dev);
//       }, py::return_value_policy::take_ownership);
// }


/*****************************************************************************/
/* Python bindings for triton::ir                                            */
/*****************************************************************************/

void init_triton_ir(py::module &&m) {
  using ret = py::return_value_policy;
  using namespace pybind11::literals;

  py::enum_<mlir::triton::CacheModifier>(m, "CACHE_MODIFIER")
      .value("NONE", mlir::triton::CacheModifier::NONE)
      .value("CA", mlir::triton::CacheModifier::CA)
      .value("CG", mlir::triton::CacheModifier::CG)
      .export_values();
  
  py::enum_<mlir::triton::EvictionPolicy>(m, "EVICTION_POLICY")
      .value("NORMAL", mlir::triton::EvictionPolicy::NORMAL)
      .value("EVICT_FIRST", mlir::triton::EvictionPolicy::EVICT_FIRST)
      .value("EVICT_LAST", mlir::triton::EvictionPolicy::EVICT_LAST)
      .export_values();
  
  py::enum_<mlir::triton::RedOp>(m, "REDUCE_OP")
      .value("ADD", mlir::triton::RedOp::ADD)
      .value("FADD", mlir::triton::RedOp::FADD)
      .value("MIN", mlir::triton::RedOp::MIN)
      .value("MAX", mlir::triton::RedOp::MAX)
      .value("FMIN", mlir::triton::RedOp::FMIN)
      .value("FMAX", mlir::triton::RedOp::FMAX)
      .value("XOR", mlir::triton::RedOp::XOR);
  
  py::enum_<mlir::triton::RMWOp>(m, "ATOMIC_OP")
      .value("ADD", mlir::triton::RMWOp::ADD)
      .value("FADD", mlir::triton::RMWOp::FADD)
      .value("AND", mlir::triton::RMWOp::AND)
      .value("OR", mlir::triton::RMWOp::OR)
      .value("XOR", mlir::triton::RMWOp::XOR)
      // .value("XCHG", mlir::triton::RMWOp::Xchg)
      .value("MAX", mlir::triton::RMWOp::MAX)
      .value("MIN", mlir::triton::RMWOp::MIN)
      .value("UMIN", mlir::triton::RMWOp::UMIN)
      .value("UMAX", mlir::triton::RMWOp::UMAX);

  py::class_<mlir::MLIRContext>(m, "context")
      .def(py::init<>())
      .def("load_triton", [](mlir::MLIRContext &self) {
        self.getOrLoadDialect<mlir::triton::TritonDialect>();
      });
      // .def(py::init([](){
      //   mlir::MLIRContext context;
      //   context.getOrLoadDialect<mlir::triton.TritonDialect>();
      //   // TODO: should we return a (raw/unique) pointer here?
      //   return context;
      // }));

  // py::class_<ir::value>(m, "value")
  //     .def("multiple_of", [](ir::value *self, int val) {
  //       if (auto *instr = dynamic_cast<ir::instruction*>(self)) {
  //         instr->set_metadata(ir::metadata::multiple_of, val);
  //       } else
  //         throw std::runtime_error("multiple_of");
  //     })
  //     .def("max_contiguous", [](ir::value *self, int val) {
  //       if (auto *instr = dynamic_cast<ir::instruction*>(self)) {
  //         instr->set_metadata(ir::metadata::max_contiguous, val);
  //       } else
  //         throw std::runtime_error("max_contiguous");
  //     })
  //     .def("set_fdiv_ieee_rounding", [](ir::value *self, bool val) {
  //       if (auto *instr = dynamic_cast<ir::binary_operator*>(self))
  //         instr->set_fdiv_ieee_rounding(val);
  //       else
  //         throw std::runtime_error("set_fdiv_ieee_rounding");
  //     })
  //     .def("ops", [](ir::value *self) {
  //       if (auto *instr = dynamic_cast<ir::instruction*>(self)) {
  //         return instr->ops();
  //       }
  //       throw std::runtime_error("cannot use ops()");
  //     })
  //     .def("replace_all_uses_with", &ir::value::replace_all_uses_with)
  //     .def("erase_from_parent", [](ir::value *self) {
  //       if (auto *instr = dynamic_cast<ir::instruction*>(self))
  //         return instr->erase_from_parent();
  //       throw std::runtime_error("cannot use erase_from_parent");
  //     })
  //     .def_property("name", &ir::value::get_name, &ir::value::set_name)
  //     .def_property_readonly("type", &ir::value::get_type);

  // // // Do we need under in TritonIR ?
  // // py::class_<ir::undef_value, ir::constant>(m, "undef")
  // //     .def("get", &ir::undef_value::get, ret::reference);

  py::class_<MlirType>(m, "type")
      .def("is_integer", [](MlirType &self) -> bool {
        return mlirTypeIsAInteger(self);
      })
      .def("is_fp16", [](MlirType &self) -> bool {
        return mlirTypeIsABF16(self);
      })
      ;

  py::class_<MlirOperation>(m, "operation")
      .def("add_entry_block", [](MlirOperation &self) -> MlirBlock {
        if (auto info = unwrap(self)->getRegisteredInfo()) {
          if (mlir::TypeID::get<mlir::FuncOp>() == info->getTypeID()) {
            auto FunctionOp = mlir::FuncOp::getFromOpaquePointer(unwrap(self));
            mlir::Block *entry = FunctionOp.addEntryBlock();
            return wrap(entry);
          }
          throw std::runtime_error("Only FuncOp can call add_entry_block");
        } else
          throw std::runtime_error("Unknown error");
      }) // this should be automatic?
      .def("dump", [](MlirOperation &self) -> void {
        unwrap(self)->dump();
      })
      ;

  py::class_<MlirValue>(m, "value")
      ;

  py::class_<MlirBlock>(m, "block")
      .def("arg", [](MlirBlock &self, int index) -> MlirValue {
        return wrap(unwrap(self)->getArgument(index));
      })
      ;

  // py::class_<mlir::triton::Float8Type>(m, "float8_type")
  //     .def_static("get", &mlir::triton::Float8Type::get);
  // py::class_<mlir::triton::BFloat8Type>(m, "bfloat8_type")
  //     .def_static("get", &mlir::triton::BFloat8Type::get);
  // py::class_<mlir::triton::PointerType>(m, "pointer_type")
  //     .def_static("get", &mlir::triton::PointerType::get);
  // py::class_<mlir::FunctionType>(m, "function_type")
  //     .def_static("get", &mlir::FunctionType::get);
  // py::class_<mlir::IntegerType>(m, "integer_type")
  //     .def_static("get", &mlir::IntegerType::get);
  // py::class_<mlir::RankedTensorType>(m, "block_type")
  //     .def_static("get", &mlir::RankedTensorType::get);

  // py::class_<mlir::ModuleOp>(m, "module")
  //     .def(py::init<std::string, ir::builder &>())
  //     .def("set_instr_metadata", [](ir::module *self, const std::string &name, ir::value *value) {
  //       const auto metadatas = self->get_metadatas();
  //       auto it = metadatas.find(name);
  //       if (it != metadatas.end())
  //         if (auto *instr = dynamic_cast<ir::instruction*>(value)) {
  //           instr->set_metadata(it->second.first, it->second.second);
  //         }
  //     })
  //     .def("get_or_insert_function", &ir::module::get_or_insert_function, ret::reference);

  // using eattr = ir::attribute_kind_t;
  // py::enum_<eattr>(m, "attribute_kind")
  //     .value("readonly", eattr::readonly)
  //     .value("writeonly", eattr::writeonly)
  //     .value("noalias", eattr::noalias)
  //     .value("aligned", eattr::aligned)
  //     .value("multiple_of", eattr::multiple_of)
  //     .value("retune", eattr::retune)
  //     .value("not_implemented", eattr::not_implemented);

  // py::class_<mlir::Attribute>(m, "attribute");
  //     // .def(py::init<eattr, int>());

  // py::class_<mlir::FuncOp>(m, "function")
  //     .def_property_readonly("args", &ir::function::args)
  //     .def_property_readonly("attrs", &ir::function::attrs)
  //     .def("add_attr", &ir::function::add_attr);

  // // // We don't need to expose mlir::Block (?)
  // // py::class_<mlir::Block>(m, "basic_block")
  // //     // .def("create", &ir::basic_block::create, ret::reference)
  // //     .def("get_predecessors", &ir::basic_block::get_predecessors, ret::reference)
  // //     .def_property_readonly("parent", &ir::basic_block::get_parent, ret::reference);

  py::class_<mlir::OpBuilder>(m, "builder", py::dynamic_attr())
      .def(py::init<mlir::MLIRContext *>())
      // // getters
      // .def_property_readonly("context", &ir::builder::get_context, ret::reference);
      // // control flow
      // .def("br", &ir::builder::create_br, ret::reference)
      // .def("cond_br", &ir::builder::create_cond_br, ret::reference)
      // .def("ret_void", &ir::builder::create_ret_void, ret::reference)
      // // insertion block/point, insert points are represented as (*bb, *instr)
      .def("set_insertion_point_to_start", [](mlir::OpBuilder &self, MlirBlock &block) -> void{
        self.setInsertionPointToStart(unwrap(block));
      })
      // .def("get_insert_block", &ir::builder::get_insert_block, ret::reference)
      // .def("set_insert_block", (void (ir::builder::*)(ir::basic_block *)) & ir::builder::set_insert_point)
      // .def("get_insert_point", [](ir::builder *self) {
      //   ir::basic_block *bb = self->get_insert_block();
      //   ir::basic_block::iterator it = self->get_insert_point();
      //   ir::instruction *instr = it == bb->end() ? nullptr : *it;
      //   return std::make_pair(bb, instr);
      // }, ret::reference)
      // .def("set_insert_point", [](ir::builder *self, std::pair<ir::basic_block*, ir::instruction*> pt) {
      //   ir::basic_block *bb = pt.first;
      //   ir::instruction *instr = pt.second;
      //   if (instr) {
      //     if (bb != instr->get_parent())
      //       throw std::runtime_error("invalid insertion point, instr not in bb");
      //     self->set_insert_point(instr);
      //   } else {
      //     assert(bb);
      //     self->set_insert_point(bb);
      //   }
      // })
      // Use arith.ConstantOp to create constants
      // // Constants
      // .def("get_int1", &ir::builder::get_int1, ret::reference)
      .def("get_int32", [](mlir::OpBuilder &self, int64_t v) -> MlirValue { 
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::ConstantIntOp>(
          loc, v, self.getI32Type()
        )));
      })
      // .def("get_uint32", &ir::builder::get_int32, ret::reference)
      // .def("get_int64", [](ir::builder *self, int64_t v) { return self->get_int64((uint64_t)v); }, ret::reference)
      // .def("get_uint64", &ir::builder::get_int64, ret::reference)
      // .def("get_float16", &ir::builder::get_float16, ret::reference)
      // .def("get_float32", &ir::builder::get_float32, ret::reference)
      // .def("get_range", &ir::builder::get_range, ret::reference)

      // Types
      .def("get_void_ty", [](mlir::OpBuilder &self) ->MlirType {
        return wrap(self.getNoneType());
      })
      .def("get_int1_ty", [](mlir::OpBuilder &self) -> MlirType {
        return wrap(self.getI1Type());
      }) // or ret::copy?
      .def("get_int8_ty", [](mlir::OpBuilder &self) -> MlirType {
        return wrap(self.getI8Type());
      })
      .def("get_int16_ty", [](mlir::OpBuilder &self) -> MlirType {
        return wrap(self.getType<mlir::IntegerType>(16));
      })
      .def("get_int32_ty", [](mlir::OpBuilder &self) -> MlirType {
        return wrap(self.getI32Type());
      })
      .def("get_int64_ty", [](mlir::OpBuilder &self) -> MlirType {
        return wrap(self.getI64Type());
      })
      .def("get_fp8_ty", [](mlir::OpBuilder &self) -> MlirType {
        return wrap(self.getType<mlir::triton::Float8Type>());
      })
      .def("get_bf8_ty", [](mlir::OpBuilder &self) -> MlirType {
        return wrap(self.getType<mlir::triton::BFloat8Type>());
      })
      .def("get_half_ty", [](mlir::OpBuilder &self) -> MlirType {
        return wrap(self.getF16Type());
      })
      .def("get_bf16_ty", [](mlir::OpBuilder &self) -> MlirType {
        return wrap(self.getBF16Type());
      })
      .def("get_float_ty", [](mlir::OpBuilder &self) -> MlirType {
        return wrap(self.getF32Type());
      })
      .def("get_double_ty", [](mlir::OpBuilder &self) -> MlirType {
        return wrap(self.getF64Type());
      })
      .def("get_ptr_ty", [](mlir::OpBuilder &self, MlirType &type) -> MlirType {
        return wrap(
          mlir::triton::PointerType::get(unwrap(type))
        );
      })
      .def("get_function_ty", [](mlir::OpBuilder &self,
                                 std::vector<MlirType> inTypes,
                                 std::vector<MlirType> outTypes) -> MlirType {
        llvm::SmallVector<mlir::Type, 4> inputsTypeList;
        llvm::SmallVector<mlir::Type, 4> resultsTypeList;
        (void)unwrapList(inTypes.size(), inTypes.data(), inputsTypeList);
        (void)unwrapList(outTypes.size(), outTypes.data(), resultsTypeList);
        return wrap(self.getFunctionType(inputsTypeList, resultsTypeList));
      })

      // Ops
      .def("create_function", [](mlir::OpBuilder &self, std::string name, MlirType funcType) -> MlirOperation {
        // TODO: loc
        auto loc = self.getUnknownLoc();
        if (auto funcTy = unwrap(funcType).dyn_cast<mlir::FunctionType>()) {
          return wrap(self.create<mlir::FuncOp>(loc, name, funcTy));
        }
        throw std::runtime_error("invalid function type");
      })
      // Structured control flow
      .def("create_for", [](mlir::OpBuilder &self, MlirValue &lb, MlirValue &ub,
                            MlirValue &step) {
        auto loc = self.getUnknownLoc();
        return wrap(
          self.create<mlir::scf::ForOp>(loc, unwrap(lb), unwrap(ub), unwrap(step))
        );
      })
      // .def("create_yield")
      // .def("create_if")
      // .def("create_while")

      // miscellious
      .def("create_make_range", [](mlir::OpBuilder &self, int start, int end) -> MlirValue {
        auto loc = self.getUnknownLoc();
        auto retType = mlir::RankedTensorType::get({end-start}, self.getI32Type());
        return wrap(
          mlir::Value(self.create<mlir::triton::MakeRangeOp>(loc, retType, start, end))
        );
      })
      .def("create_get_program_id", [](mlir::OpBuilder &self, int axis) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(
          mlir::Value(self.create<mlir::triton::GetProgramIdOp>(loc, self.getI32Type(), axis))
        );
      })

      // // Cast instructions
      // .def("create_bitcast", &ir::builder::create_bitcast, ret::reference)
      // .def("create_cast", &ir::builder::create_cast, ret::reference)
      // .def("create_ptr_to_int", &ir::builder::create_ptr_to_int, ret::reference)
      // .def("create_si_to_fp", &ir::builder::create_si_to_fp, ret::reference)
      // .def("create_ui_to_fp", &ir::builder::create_ui_to_fp, ret::reference)
      // .def("create_fp_to_si", &ir::builder::create_fp_to_si, ret::reference)
      // .def("create_fp_to_ui", &ir::builder::create_fp_to_ui, ret::reference)
      // .def("create_fp_ext", &ir::builder::create_fp_ext, ret::reference)
      // .def("create_fp_trunc", &ir::builder::create_fp_trunc, ret::reference)
      // .def("create_int_cast", &ir::builder::create_int_cast, ret::reference)
      // .def("create_downcast", &ir::builder::create_downcast, ret::reference)
      // // Binary instructions
      // .def("create_insert_nuwnswb_binop", &ir::builder::create_insert_nuwnswb_binop, ret::reference)
      .def("create_fmul", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(
          self.create<mlir::arith::MulFOp>(loc, unwrap(lhs), unwrap(rhs))
        ));
      })
      .def("create_fdiv", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(
          self.create<mlir::arith::DivFOp>(loc, unwrap(lhs), unwrap(rhs))
        ));
      })
      .def("create_frem", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(
          self.create<mlir::arith::RemFOp>(loc, unwrap(lhs), unwrap(rhs))
        ));
      })
      .def("create_fadd", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(
          self.create<mlir::arith::AddFOp>(loc, unwrap(lhs), unwrap(rhs))
        ));
      })
      .def("create_fsub", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(
          self.create<mlir::arith::SubFOp>(loc, unwrap(lhs), unwrap(rhs))
        ));
      })
      .def("create_mul", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        // Check lhs & rhs have single result (?)
        return wrap(
          mlir::Value(self.create<mlir::arith::MulIOp>(loc, unwrap(lhs), unwrap(rhs)))
        );
      })
      .def("create_sdiv", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(
          mlir::Value(self.create<mlir::arith::DivSIOp>(loc, unwrap(lhs), unwrap(rhs)))
        );
      })
      .def("create_udiv", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(
          mlir::Value(self.create<mlir::arith::DivUIOp>(loc, unwrap(lhs), unwrap(rhs)))
        );
      })
      .def("create_srem", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(
          mlir::Value(self.create<mlir::arith::RemSIOp>(loc, unwrap(lhs), unwrap(rhs)))
        );
      })
      .def("create_urem", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(
          mlir::Value(self.create<mlir::arith::RemUIOp>(loc, unwrap(lhs), unwrap(rhs)))
        );
      })
      .def("create_add", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(
          mlir::Value(self.create<mlir::arith::AddIOp>(loc, unwrap(lhs), unwrap(rhs)))
        );
      })
      .def("create_sub", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(
          mlir::Value(self.create<mlir::arith::SubIOp>(loc, unwrap(lhs), unwrap(rhs)))
        );
      })
      .def("create_shl", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(
          mlir::Value(self.create<mlir::arith::ShLIOp>(loc, unwrap(lhs), unwrap(rhs)))
        );
      })
      .def("create_lshr", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(
          mlir::Value(self.create<mlir::arith::ShRUIOp>(loc, unwrap(lhs), unwrap(rhs)))
        );
      })
      .def("create_ashr", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(
          mlir::Value(self.create<mlir::arith::ShRSIOp>(loc, unwrap(lhs), unwrap(rhs)))
        );
      })
      // GEP
      .def("create_gep", [](mlir::OpBuilder &self, MlirValue &ptr, MlirValue &offset) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(
          mlir::Value(self.create<mlir::triton::GEPOp>(loc, unwrap(ptr).getType(), unwrap(ptr), unwrap(offset)))
        );
      })
      // Comparison (int)
      .def("create_icmpSLE", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpIOp>(
          loc, mlir::arith::CmpIPredicate::sle,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_icmpSLT", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpIOp>(
          loc, mlir::arith::CmpIPredicate::slt,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_icmpSGE", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpIOp>(
          loc, mlir::arith::CmpIPredicate::sge,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_icmpSGT", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpIOp>(
          loc, mlir::arith::CmpIPredicate::sgt,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_icmpULE", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpIOp>(
          loc, mlir::arith::CmpIPredicate::ule,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_icmpULT", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpIOp>(
          loc, mlir::arith::CmpIPredicate::ult,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_icmpUGE", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpIOp>(
          loc, mlir::arith::CmpIPredicate::uge,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_icmpUGT", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpIOp>(
          loc, mlir::arith::CmpIPredicate::ugt,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_icmpEQ", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpIOp>(
          loc, mlir::arith::CmpIPredicate::eq,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_icmpNE", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpIOp>(
          loc, mlir::arith::CmpIPredicate::ne,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      // Comparison (float)
      .def("create_fcmpOLT", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpFOp>(
          loc, mlir::arith::CmpFPredicate::OLT,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_fcmpOGT", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpFOp>(
          loc, mlir::arith::CmpFPredicate::OGT,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_fcmpOLE", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpFOp>(
          loc, mlir::arith::CmpFPredicate::OLE,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_fcmpOGE", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpFOp>(
          loc, mlir::arith::CmpFPredicate::OGE,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_fcmpOEQ", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpFOp>(
          loc, mlir::arith::CmpFPredicate::OEQ,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_fcmpONE", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpFOp>(
          loc, mlir::arith::CmpFPredicate::ONE,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_fcmpULT", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpFOp>(
          loc, mlir::arith::CmpFPredicate::ULT,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_fcmpUGT", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpFOp>(
          loc, mlir::arith::CmpFPredicate::UGT,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_fcmpULE", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpFOp>(
          loc, mlir::arith::CmpFPredicate::ULE,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_fcmpUGE", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpFOp>(
          loc, mlir::arith::CmpFPredicate::UGE,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_fcmpUEQ", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpFOp>(
          loc, mlir::arith::CmpFPredicate::UEQ,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_fcmpUNE", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::CmpFOp>(
          loc, mlir::arith::CmpFPredicate::UNE,
          unwrap(lhs), unwrap(rhs)
        )));
      })
      // // Logical
      .def("create_and", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::AndIOp>(
          loc, unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_xor", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::XOrIOp>(
          loc, unwrap(lhs), unwrap(rhs)
        )));
      })
      .def("create_or", [](mlir::OpBuilder &self, MlirValue &lhs, MlirValue &rhs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(self.create<mlir::arith::OrIOp>(
          loc, unwrap(lhs), unwrap(rhs)
        )));
      })
      // // Input/Output
      .def("create_load", [](mlir::OpBuilder &self, MlirValue &ptrs) -> MlirValue {
        auto loc = self.getUnknownLoc();
        return wrap(mlir::Value(
          self.create<mlir::triton::LoadOp>(loc, unwrap(ptrs))
        ));
      })
      .def("create_store", [](mlir::OpBuilder &self, MlirValue &ptrs, MlirValue &value) -> void {
        auto loc = self.getUnknownLoc();
        self.create<mlir::triton::StoreOp>(loc, unwrap(ptrs), unwrap(value));
      })
      // .def("create_masked_load", &ir::builder::create_masked_load, ret::reference)
      // .def("create_masked_store", &ir::builder::create_masked_store, ret::reference)
      // // Block instruction
      // .def("create_splat", &ir::builder::create_splat, ret::reference)
      // .def("create_reshape", &ir::builder::create_reshape, ret::reference)
      // .def("create_cat", &ir::builder::create_cat, ret::reference)
      .def("create_broadcast", [](mlir::OpBuilder &self, MlirValue &arg, std::vector<int64_t> &shape) -> MlirValue {
        auto loc = self.getUnknownLoc();
        auto argType = unwrap(arg).getType();
        return wrap(mlir::Value(self.create<mlir::triton::BroadcastOp>(
          loc, mlir::RankedTensorType::get(shape, argType), unwrap(arg)
        )));
      })
      // // atomic
      // .def("create_atomic_cas", &ir::builder::create_atomic_cas, ret::reference)
      // .def("create_atomic_rmw", &ir::builder::create_atomic_rmw, ret::reference)

      // // Built-in instruction
      // .def("create_get_program_id", &ir::builder::create_get_program_id, ret::reference)
      // .def("create_get_num_programs", &ir::builder::create_get_num_programs, ret::reference)
      // .def("create_exp", &ir::builder::create_exp, ret::reference)
      // .def("create_cos", &ir::builder::create_cos, ret::reference)
      // .def("create_sin", &ir::builder::create_sin, ret::reference)
      // .def("create_log", &ir::builder::create_log, ret::reference)
      // .def("create_dot", &ir::builder::create_dot, ret::reference)
      // .def("create_trans", &ir::builder::create_trans, ret::reference)
      // .def("create_sqrt", &ir::builder::create_sqrt, ret::reference)
      // .def("create_reduce", &ir::builder::create_reduce, ret::reference)
      // .def("create_select", &ir::builder::create_select, ret::reference)
      // // Intrinsics
      // // These have no place in the IR, and hopefully they can be removed at some point
      // .def("create_umulhi", &ir::builder::create_umulhi, ret::reference)
      // .def("create_barrier", &ir::builder::create_barrier, ret::reference);
      ;
}

void init_triton(py::module &m) {
  py::module subm = m.def_submodule("triton");
  // init_triton_codegen(std::move(subm.def_submodule("code_gen")));
  init_triton_runtime(std::move(subm.def_submodule("runtime")));
  init_triton_ir(std::move(subm.def_submodule("ir")));
}
