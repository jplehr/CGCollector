#include "helper/common.h"
#include "clang/AST/DeclCXX.h"

#include <memory>

std::string getMangledName(clang::NamedDecl const *const nd) {
  // auto mc = nd->getASTContext().createMangleContext();
  std::unique_ptr<clang::MangleContext> mc(nd->getASTContext().createMangleContext());

  if (!nd || !mc) {
    std::cerr << "NamedDecl was nullptr" << std::endl;
    return "__NO_NAME__";
  }

  if (llvm::isa<clang::CXXConstructorDecl>(nd) || llvm::isa<clang::CXXDestructorDecl>(nd)) {
    //    std::cerr << "NamedDecl was ctor or dtor" << std::endl;
    return "__NO_NAME__";
  }

  if (const clang::FunctionDecl *dc = llvm::dyn_cast<clang::FunctionDecl>(nd)) {
    if (dc->isExternC()) {
      return dc->getNameAsString();
    }
    if (dc->isMain()) {
      return "main";
    }
  }
  std::string functionName;
  llvm::raw_string_ostream llvmName(functionName);
  mc->mangleName(nd, llvmName);
  return llvmName.str();
}
