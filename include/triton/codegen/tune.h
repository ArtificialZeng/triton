#ifndef TDL_INCLUDE_IR_CODEGEN_TUNE_H
#define TDL_INCLUDE_IR_CODEGEN_TUNE_H

#include <map>
#include <set>
#include <vector>

namespace triton{

namespace ir{
  class value;
  class module;
  class instruction;
  class function;
  class metaparameter;
}

namespace codegen{

class tune {
  typedef std::pair<ir::value*, unsigned> node_t;
  typedef std::map <node_t, std::set<node_t>> graph_t;

  enum fragment_t{
    STRIDED_SCAN,
    HMMA_FRAGMENT_C
  };

private:
  void add_constraint(node_t x, node_t y);
  void init_c_phi(ir::instruction *i);
  void init_c_graph(ir::instruction *v);
  fragment_t get_fragmentation_type(node_t x, graph_t &graph);
  void connected_components(node_t x, const std::vector<ir::metaparameter *> mps, const std::vector<std::string> prefixes, std::set<node_t> &nodes, graph_t &graph, unsigned group_id);
  void create_grids(std::vector<ir::instruction*> &grids, std::map<ir::metaparameter *, ir::instruction *> &references, ir::function *fn);


public:
  tune();
  std::vector<ir::metaparameter *> get_params(ir::module& mod);
  std::map<std::string, ir::metaparameter *> get_params(ir::instruction* i);
  ir::metaparameter* get_param(ir::value *value, const std::string &key) { return params_[value][key]; }
  unsigned get_param_group(ir::value *value, unsigned ax);
  void copy(ir::value *dst, ir::value *src) { params_[dst] = params_[src]; groups_[dst] = groups_[src]; }
  bool check_constraints(std::map<ir::value *, std::vector<std::string>> &errors);
  void run(ir::module &mod);
  void init(ir::module &mod);
  unsigned get_num_global_range();
  unsigned get_global_range_size(unsigned axis);
  unsigned get_num_threads();

private:
  std::vector<unsigned*> pool_;
  graph_t dependencies_;
  std::set<node_t> nodes_;
  std::map<node_t, fragment_t> fragments_;
  std::map<node_t, unsigned> static_params_;
  std::map<ir::value*, std::map<std::string, ir::metaparameter*>> params_;
  std::map<unsigned, ir::metaparameter*> global_range_sizes_;
  unsigned num_global_ranges_;
  unsigned num_threads_;
  std::vector<ir::instruction*> grids_;
  std::map<ir::value*, std::map<unsigned, unsigned>> groups_;
};


}
}

#endif
