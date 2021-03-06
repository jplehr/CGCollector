//===- CallGraph.cpp - AST-based Call graph -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines the AST-based CallGraph.
//
//===----------------------------------------------------------------------===//

#include "CallGraph.h"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/DeclObjC.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprObjC.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/StmtVisitor.h>
#include <clang/Basic/IdentifierTable.h>
#include <clang/Basic/LLVM.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Compiler.h>
#include <llvm/Support/DOTGraphTraits.h>
#include <llvm/Support/GraphWriter.h>
#include <llvm/Support/raw_ostream.h>

#include <cassert>
#include <memory>
#include <string>

//#include <iostream>

using namespace clang;

#define DEBUG_TYPE "CallGraph"

STATISTIC(NumObjCCallEdges, "Number of Objective-C method call edges");
STATISTIC(NumBlockCallEdges, "Number of block call edges");

/// A helper class, which walks the AST and locates all the call sites in the
/// given function body.
class CGBuilder : public StmtVisitor<CGBuilder> {
  CallGraph *G;
  CallGraphNode *CallerNode;

 public:
  CGBuilder(CallGraph *g, CallGraphNode *N) : G(g), CallerNode(N) {}

  void VisitStmt(Stmt *S) { VisitChildren(S); }

  Decl *getDeclFromCall(CallExpr *CE) {
    if (FunctionDecl *CalleeDecl = CE->getDirectCallee())
      return CalleeDecl;

    // Simple detection of a call through a block.
    Expr *CEE = CE->getCallee()->IgnoreParenImpCasts();
    if (BlockExpr *Block = dyn_cast<BlockExpr>(CEE)) {
      NumBlockCallEdges++;
      return Block->getBlockDecl();
    }

    return nullptr;
  }

  void handleFunctionPointerInArguments(CallExpr *CE) {
    for (auto argIt = CE->arg_begin(); argIt != CE->arg_end(); ++argIt) {
      // if an adress of operator is used in argument list
      if (UnaryOperator *UO = dyn_cast<UnaryOperator>(*argIt)) {
        if (UO->getOpcode() == UO_AddrOf) {
          // if the adress in the argument list is taken of a function -> function pointer in argument list
          if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(UO->getSubExpr())) {
            if (FunctionDecl *FD = dyn_cast<FunctionDecl>(DRE->getDecl())) {
              // get the called function, which gets the function pointer
              if (FunctionDecl *CalleeDecl = CE->getDirectCallee()) {
                // assumption: callee calls the function which is given in argument list
                addCalledDecl(CalleeDecl, FD);
                // std::cout << "method " << CalleeDecl->getNameAsString() << " calls " << FD->getNameAsString() << " by
                // function pointer" << std::endl;

                // TODO what is done in handle indirects of the old cg? this is a poor implementation we dont want to
                // follow this here if (CalleeDecl && FD) {
                //  indirects.insert({CalleeDecl, FD});
                //}
              }
            }
          }
        }
      }
    }
  }

  // add a called decl to the current function
  void addCalledDecl(Decl *D) {
    if (!G->includeInGraph(D))
      return;

    CallGraphNode *CalleeNode = G->getOrInsertNode(D);
    CallerNode->addCallee(CalleeNode);
  }

  // add a called decl to another function
  void addCalledDecl(Decl *Caller, Decl *Callee) {
    if (!G->includeInGraph(Callee))
      return;

    G->getOrInsertNode(Caller)->addCallee(G->getOrInsertNode(Callee));
  }

  void VisitCallExpr(CallExpr *CE) {
    if (Decl *D = getDeclFromCall(CE))
      addCalledDecl(D);

    handleFunctionPointerInArguments(CE);
    VisitChildren(CE);
  }

  // Adds may-call edges for the ObjC message sends.
  void VisitObjCMessageExpr(ObjCMessageExpr *ME) {
    if (ObjCInterfaceDecl *IDecl = ME->getReceiverInterface()) {
      Selector Sel = ME->getSelector();

      // Find the callee definition within the same translation unit.
      Decl *D = nullptr;
      if (ME->isInstanceMessage())
        D = IDecl->lookupPrivateMethod(Sel);
      else
        D = IDecl->lookupPrivateClassMethod(Sel);
      if (D) {
        addCalledDecl(D);
        NumObjCCallEdges++;
      }
    }
  }

  void VisitChildren(Stmt *S) {
    for (Stmt *SubStmt : S->children())
      if (SubStmt)
        this->Visit(SubStmt);
  }
};

void CallGraph::addNodesForBlocks(DeclContext *D) {
  if (BlockDecl *BD = dyn_cast<BlockDecl>(D))
    addNodeForDecl(BD, true);

  for (auto *I : D->decls())
    if (auto *DC = dyn_cast<DeclContext>(I))
      addNodesForBlocks(DC);
}

CallGraph::CallGraph() { Root = getOrInsertNode(nullptr); }

CallGraph::~CallGraph() = default;

bool CallGraph::includeInGraph(const Decl *D) {
  assert(D);
  // we want to include functions without body to merge them afterwards
  // if (!D->hasBody())
  //  return false;

  if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
    // We skip function template definitions, as their semantics is
    // only determined when they are instantiated.
    if (FD->isDependentContext())
      return false;

    IdentifierInfo *II = FD->getIdentifier();
    // TODO not sure whether we want to include __inline marked functions
    if (II && II->getName().startswith("__inline"))
      return false;
  }

  return true;
}

void CallGraph::addNodeForDecl(Decl *D, bool IsGlobal) {
  assert(D);

  // Allocate a new node, mark it as root, and process it's calls.
  CallGraphNode *Node = getOrInsertNode(D);

  // Process all the calls by this function as well.
  CGBuilder builder(this, Node);
  if (Stmt *Body = D->getBody())
    builder.Visit(Body);
}

CallGraphNode *CallGraph::getNode(const Decl *F) const {
  FunctionMapTy::const_iterator I = FunctionMap.find(F);
  if (I == FunctionMap.end())
    return nullptr;
  return I->second.get();
}

CallGraphNode *CallGraph::getOrInsertNode(const Decl *F) {
  if (F && !isa<ObjCMethodDecl>(F))
    F = F->getCanonicalDecl();

  std::unique_ptr<CallGraphNode> &Node = FunctionMap[F];
  if (Node)
    return Node.get();

  Node = llvm::make_unique<CallGraphNode>(F);
  // Make Root node a parent of all functions to make sure all are reachable.
  if (F)
    Root->addCallee(Node.get());
  return Node.get();
}

bool CallGraph::VisitCXXMethodDecl(clang::CXXMethodDecl *MD) {
  if (!MD->isVirtual())
    return true;

  // std::cout << "virtual function " << MD->getParent()->getNameAsString() << "::" << MD->getNameAsString() <<
  // std::endl;

  auto node = getOrInsertNode(MD);

  // multiple inheritance causes multiple entries in overriden_methods
  // hierarchical overridden methods does not show up here
  for (auto om : MD->overridden_methods()) {
    // std::cout << "override " << om->getParent()->getNameAsString() << "::" << om->getNameAsString() << std::endl;

    node->addOverriddenMethod(om);
    getOrInsertNode(om)->addOverriddenBy(node->getDecl());
  }

  return true;
}

void CallGraph::print(raw_ostream &OS) const {
  OS << " --- Call graph Dump --- \n";

  // We are going to print the graph in reverse post order, partially, to make
  // sure the output is deterministic.
  llvm::ReversePostOrderTraversal<const CallGraph *> RPOT(this);
  for (llvm::ReversePostOrderTraversal<const CallGraph *>::rpo_iterator I = RPOT.begin(), E = RPOT.end(); I != E; ++I) {
    const CallGraphNode *N = *I;

    OS << "  Function: ";
    if (N == Root)
      OS << "< root >";
    else
      N->print(OS);

    OS << " calls: ";
    for (CallGraphNode::const_iterator CI = N->begin(), CE = N->end(); CI != CE; ++CI) {
      assert(*CI != Root && "No one can call the root node.");
      (*CI)->print(OS);
      OS << " ";
    }
    OS << '\n';
  }
  OS.flush();
}

LLVM_DUMP_METHOD void CallGraph::dump() const { print(llvm::errs()); }

void CallGraph::viewGraph() const { llvm::ViewGraph(this, "CallGraph"); }

void CallGraphNode::print(raw_ostream &os) const {
  if (const NamedDecl *ND = dyn_cast_or_null<NamedDecl>(FD))
    return ND->printQualifiedName(os);
  os << "< >";
}

LLVM_DUMP_METHOD void CallGraphNode::dump() const { print(llvm::errs()); }

namespace llvm {

template <>
struct DOTGraphTraits<const CallGraph *> : public DefaultDOTGraphTraits {
  DOTGraphTraits(bool isSimple = false) : DefaultDOTGraphTraits(isSimple) {}

  static std::string getNodeLabel(const CallGraphNode *Node, const CallGraph *CG) {
    if (CG->getRoot() == Node) {
      return "< root >";
    }
    if (const NamedDecl *ND = dyn_cast_or_null<NamedDecl>(Node->getDecl()))
      return ND->getNameAsString();
    else
      return "< >";
  }
};

}  // namespace llvm
