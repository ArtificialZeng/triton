#ifndef ISAAC_DRIVER_DEVICE_H
#define ISAAC_DRIVER_DEVICE_H

#include "isaac/defines.h"
#include "isaac/driver/common.h"
#include "isaac/driver/platform.h"
#include "isaac/driver/handle.h"

namespace isaac
{

namespace driver
{

// Device
class ISAACAPI Device
{
private:
  friend class Context;
  friend class CommandQueue;

public:
  enum ISAACAPI VENDOR
  {
      AMD,
      INTEL,
      NVIDIA,
      UNKNOWN
  };

private:
#ifdef ISAAC_WITH_CUDA
  template<CUdevice_attribute attr>
  int cuGetInfo() const;
#endif
public:
#ifdef ISAAC_WITH_CUDA
  Device(int ordinal);
#endif
  Device(cl_device_id const & device, bool take_ownership = true);

  bool operator==(Device const &) const;
  bool operator<(Device const &) const;

  backend_type backend() const;
  size_t clock_rate() const;
  unsigned int address_bits() const;
  driver::Platform platform() const;
  std::string name() const;
  std::string vendor_str() const;
  VENDOR vendor() const;
  std::vector<size_t> max_work_item_sizes() const;
  device_type type() const;
  std::string extensions() const;
  size_t max_work_group_size() const;
  size_t local_mem_size() const;
  size_t warp_wavefront_size() const;
  bool fp64_support() const;
  std::pair<unsigned int, unsigned int> nv_compute_capability() const;

private:
  backend_type backend_;
  HANDLE_TYPE(cl_device_id, CUdevice) h_;
};

}

}

#endif
