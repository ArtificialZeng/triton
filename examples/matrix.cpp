#include <cstring>
#include <cstdio>
#include "ast.h"
#include "ir/context.h"
#include "ir/module.h"

typedef struct yy_buffer_state * YY_BUFFER_STATE;
extern int yyparse();
extern YY_BUFFER_STATE yy_scan_string(const char * str);
extern void yy_delete_buffer(YY_BUFFER_STATE buffer);
using tdl::ast::translation_unit;
extern translation_unit *ast_root;

const char src[] =
"\
void test(fp32 *A, fp32 *B, fp32 *C, int32 i){\
  int32 j = 1;\
  int32 k;\
  i = i + j;\
  for(k = 0; k < 10; k = k+1){\
    int32 u = 1;\
    u = u + i;\
    if(k == 0)\
     u = u + 2;\
  }\
}\
";

int main() {
   YY_BUFFER_STATE buffer = yy_scan_string(src);
   yyparse();
   yy_delete_buffer(buffer);
   translation_unit *program = ast_root;
   tdl::ir::context context;
   tdl::ir::module module("matrix", context);
   program->codegen(&module);
//   llvm::PrintModulePass print(llvm::outs());
//   llvm::AnalysisManager<llvm::Module> analysis;
//   print.run(*module.handle(), analysis);
   return 0;
}
