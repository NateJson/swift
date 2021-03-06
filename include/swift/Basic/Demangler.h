//===--- Demangler.h - String to Node-Tree Demangling -----------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file is the compiler-private API of the demangler.
// It should only be used within the swift compiler or runtime library, but not
// by external tools which use the demangler library (like lldb).
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_DEMANGLER_H
#define SWIFT_BASIC_DEMANGLER_H

#include "swift/Basic/Demangle.h"
#include <vector>

//#define NODE_FACTORY_DEBUGGING

#ifdef NODE_FACTORY_DEBUGGING
#include <iostream>
#endif


using namespace swift::Demangle;
using llvm::StringRef;

namespace swift {
namespace Demangle {

/// The allocator for demangling nodes and other demangling-internal stuff.
///
/// It implements a simple bump-pointer allocator.
class NodeFactory {

  /// Position in the current slab.
  char *CurPtr = nullptr;

  /// The end of the current slab.
  char *End = nullptr;

  struct Slab {
    // The previously allocated slab.
    Slab *Previous;
    // Tail allocated memory starts here.
  };
  
  /// The head of the single-linked slab list.
  Slab *CurrentSlab = nullptr;

  /// The size of the previously allocated slab.
  ///
  /// The slab size can only grow, even clear() does not reset the slab size.
  /// This initial size is good enough to fit most de-manglings.
  size_t SlabSize = 100 * sizeof(Node);

  static char *align(char *Ptr, size_t Alignment) {
    assert(Alignment > 0);
    return (char*)(((uintptr_t)Ptr + Alignment - 1)
                     & ~((uintptr_t)Alignment - 1));
  }

  static void freeSlabs(Slab *slab);
  
public:

  NodeFactory() {
#ifdef NODE_FACTORY_DEBUGGING
    std::cerr << "## New NodeFactory " << this << "\n";
#endif
  }
  
  ~NodeFactory() {
    freeSlabs(CurrentSlab);
#ifdef NODE_FACTORY_DEBUGGING
    std::cerr << "Delete NodeFactory " << this << "\n";
#endif
  }
  
  void clear();

  /// Allocates an object of type T or an array of objects of type T.
  template<typename T> T *Allocate(size_t NumObjects = 1) {
    static_assert(alignof(T) <= alignof(Slab *), "alignment not supported");
    size_t ObjectSize = NumObjects * sizeof(T);
    CurPtr = align(CurPtr, alignof(T));
#ifdef NODE_FACTORY_DEBUGGING
    std::cerr << "  alloc " << ObjectSize << ", CurPtr = "
              << (void *)CurPtr << "\n";
#endif

    // Do we have enough space in the current slab?
    if (CurPtr + ObjectSize > End) {
      // No. We have to malloc a new slab.
      // We doulbe the slab size for each allocated slab.
      SlabSize = std::max(SlabSize * 2, ObjectSize);
      size_t AllocSize = sizeof(Slab) + SlabSize;
      Slab *newSlab = (Slab *)malloc(AllocSize);

      // Insert the new slab in the single-linked list of slabs.
      newSlab->Previous = CurrentSlab;
      CurrentSlab = newSlab;

      // Initialize the pointers to the new slab.
      CurPtr = (char *)(newSlab + 1);
      assert(align(CurPtr, alignof(T)) == CurPtr);
      End = CurPtr + SlabSize;
#ifdef NODE_FACTORY_DEBUGGING
      std::cerr << "    ** new slab " << newSlab << ", allocsize = "
                << AllocSize << ", CurPtr = " << (void *)CurPtr
                << ", End = " << (void *)End << "\n";
#endif
      assert(End == (char *)newSlab + AllocSize);
    }
    T *AllocatedObj = (T *)CurPtr;
    CurPtr += ObjectSize;
    return AllocatedObj;
  }

  /// Tries to enlarge the \p Capacity of an array of \p Objects.
  ///
  /// If \p Objects is allcoated at the end of the current slab and the slab
  /// has enough free space, the \p Capacity is simpliy enlarged and no new
  /// allocation needs to be done.
  /// Otherwise a new array of objects is allocated and \p Objects is set to the
  /// new memory address.
  /// The \p Capacity is enlarged at least by \p MinGrowth, but can also be
  /// enlarged by a bigger value.
  template<typename T> void Reallocate(T *&Objects, size_t &Capacity,
                                       size_t MinGrowth) {
    size_t OldAllocSize = Capacity * sizeof(T);
    size_t AdditionalAlloc = MinGrowth * sizeof(T);

#ifdef NODE_FACTORY_DEBUGGING
    std::cerr << "  realloc " << Objects << ", num = " << NumObjects
              << " (size = " << OldAllocSize << "), Growth = " << Growth
              << " (size = " << AdditionalAlloc << ")\n";
#endif
    if ((char *)Objects + OldAllocSize == CurPtr
        && CurPtr + AdditionalAlloc <= End) {
      // The existing array is at the end of the current slab and there is
      // enough space. So we are fine.
      CurPtr += AdditionalAlloc;
      Capacity += MinGrowth;
#ifdef NODE_FACTORY_DEBUGGING
      std::cerr << "    ** can grow: CurPtr = " << (void *)CurPtr << "\n";
#endif
      return;
    }
    // We need a new allocation.
    size_t Growth = (MinGrowth >= 4 ? MinGrowth : 4);
    if (Growth < Capacity * 2)
      Growth = Capacity * 2;
    T *NewObjects = Allocate<T>(Capacity + Growth);
    memcpy(NewObjects, Objects, OldAllocSize);
    Objects = NewObjects;
    Capacity += Growth;
  }

  /// Creates a node of kind \p K.
  NodePointer createNode(Node::Kind K);

  /// Creates a node of kind \p K with an \p Index payload.
  NodePointer createNode(Node::Kind K, Node::IndexType Index);

  /// Creates a node of kind \p K with a \p Text payload.
  ///
  /// The \p Text string is copied.
  NodePointer createNode(Node::Kind K, llvm::StringRef Text);

  /// Creates a node of kind \p K with a \p Text payload, which must be a C
  /// string literal.
  ///
  /// The \p Text string is _not_ copied.
  NodePointer createNode(Node::Kind K, const char *Text);
};

/// The demangler.
///
/// It de-mangles a string and it also ownes the returned node-tree. This means
/// The nodes of the tree only live as long as the Demangler itself.
class Demangler : public NodeFactory {
private:
  StringRef Text;
  size_t Pos = 0;

  struct NodeWithPos {
    NodePointer Node;
    size_t Pos;
  };

  std::vector<NodeWithPos> NodeStack;
  std::vector<NodePointer> Substitutions;
  std::vector<unsigned> PendingSubstitutions;

  static const int MaxNumWords = 26;
  StringRef Words[MaxNumWords];
  int NumWords = 0;

  static NodePointer pop_back_val(std::vector<NodePointer> &NodeVector) {
    if (NodeVector.empty())
      return nullptr;
    NodePointer Val = NodeVector.back();
    NodeVector.pop_back();
    return Val;
  }

  bool nextIf(StringRef str) {
    if (!Text.substr(Pos).startswith(str)) return false;
    Pos += str.size();
    return true;
  }

  char peekChar() {
    if (Pos >= Text.size())
      return 0;
    return Text[Pos];
  }

  char nextChar() {
    if (Pos >= Text.size())
      return 0;
    return Text[Pos++];
  }

  bool nextIf(char c) {
    if (peekChar() != c)
      return false;
    Pos++;
    return true;
  }

  void pushBack() {
    assert(Pos > 0);
    Pos--;
  }

  void pushNode(NodePointer Nd) {
    NodeStack.push_back({ Nd, Pos });
  }

  NodePointer popNode() {
    if (!NodeStack.empty()) {
      NodePointer Val = NodeStack.back().Node;
      NodeStack.pop_back();
      return Val;
    }
    return nullptr;
  }

  NodePointer popNode(Node::Kind kind) {
    if (NodeStack.empty())
      return nullptr;

    Node::Kind NdKind = NodeStack.back().Node->getKind();
    if (NdKind != kind)
      return nullptr;

    return popNode();
  }

  template <typename Pred> NodePointer popNode(Pred pred) {
    if (NodeStack.empty())
      return nullptr;

    Node::Kind NdKind = NodeStack.back().Node->getKind();
    if (!pred(NdKind))
      return nullptr;
    
    return popNode();
  }

  void init(StringRef MangledName);
  
  void addSubstitution(NodePointer Nd) {
    if (Nd)
      Substitutions.push_back(Nd);
  }

  NodePointer addChild(NodePointer Parent, NodePointer Child);
  NodePointer createWithChild(Node::Kind kind, NodePointer Child);
  NodePointer createType(NodePointer Child);
  NodePointer createWithChildren(Node::Kind kind, NodePointer Child1,
                                 NodePointer Child2);
  NodePointer createWithChildren(Node::Kind kind, NodePointer Child1,
                                 NodePointer Child2, NodePointer Child3);
  NodePointer createWithPoppedType(Node::Kind kind) {
    return createWithChild(kind, popNode(Node::Kind::Type));
  }

  void parseAndPushNodes();

  NodePointer changeKind(NodePointer Node, Node::Kind NewKind);

  NodePointer demangleOperator();

  int demangleNatural();
  int demangleIndex();
  NodePointer demangleIndexAsNode();
  NodePointer demangleIdentifier();
  NodePointer demangleOperatorIdentifier();

  NodePointer demangleMultiSubstitutions();
  NodePointer createSwiftType(Node::Kind typeKind, StringRef name);
  NodePointer demangleKnownType();
  NodePointer demangleLocalIdentifier();

  NodePointer popModule();
  NodePointer popContext();
  NodePointer popTypeAndGetChild();
  NodePointer popTypeAndGetNominal();
  NodePointer demangleBuiltinType();
  NodePointer demangleNominalType(Node::Kind kind);
  NodePointer demangleTypeAlias();
  NodePointer demangleExtensionContext();
  NodePointer demanglePlainFunction();
  NodePointer popFunctionType(Node::Kind kind);
  NodePointer popFunctionParams(Node::Kind kind);
  NodePointer popTuple();
  NodePointer popTypeList();
  NodePointer popProtocol();
  NodePointer demangleBoundGenericType();
  NodePointer demangleBoundGenericArgs(NodePointer nominalType,
                                    const std::vector<NodePointer> &TypeLists,
                                    size_t TypeListIdx);
  NodePointer demangleInitializer();
  NodePointer demangleImplParamConvention();
  NodePointer demangleImplResultConvention(Node::Kind ConvKind);
  NodePointer demangleImplFunctionType();
  NodePointer demangleMetatype();
  NodePointer createArchetypeRef(int depth, int i);
  NodePointer demangleArchetype();
  NodePointer demangleAssociatedTypeSimple(NodePointer GenericParamIdx);
  NodePointer demangleAssociatedTypeCompound(NodePointer GenericParamIdx);

  NodePointer popAssocTypeName();
  NodePointer getDependentGenericParamType(int depth, int index);
  NodePointer demangleGenericParamIndex();
  NodePointer popProtocolConformance();
  NodePointer demangleThunkOrSpecialization();
  NodePointer demangleGenericSpecialization(Node::Kind SpecKind);
  NodePointer demangleFunctionSpecialization();
  NodePointer demangleFuncSpecParam(Node::IndexType ParamIdx);
  NodePointer addFuncSpecParamNumber(NodePointer Param,
                              FunctionSigSpecializationParamKind Kind);

  NodePointer demangleSpecAttributes(Node::Kind SpecKind,
                                     bool demangleUniqueID = false);

  NodePointer demangleWitness();
  NodePointer demangleSpecialType();
  NodePointer demangleMetatypeRepresentation();
  NodePointer demangleFunctionEntity();
  NodePointer demangleEntity(Node::Kind Kind);
  NodePointer demangleProtocolListType();
  NodePointer demangleGenericSignature(bool hasParamCounts);
  NodePointer demangleGenericRequirement();
  NodePointer demangleGenericType();
  NodePointer demangleValueWitness();

  NodePointer demangleObjCTypeName();

public:
  Demangler() {}

  /// Demangle the given symbol and return the parse tree.
  ///
  /// \param MangledName The mangled symbol string, which start with the
  /// mangling prefix _T0.
  ///
  /// \returns A parse tree for the demangled string - or a null pointer
  /// on failure.
  /// The lifetime of the returned node tree ends with the lifetime of the
  /// Demangler or with a call of clear().
  NodePointer demangleSymbol(StringRef MangledName);

  /// Demangle the given type and return the parse tree.
  ///
  /// \param MangledName The mangled type string, which does _not_ start with
  /// the mangling prefix _T0.
  ///
  /// \returns A parse tree for the demangled string - or a null pointer
  /// on failure.
  /// The lifetime of the returned node tree ends with the lifetime of the
  /// Demangler or with a call of clear().
  NodePointer demangleType(StringRef MangledName);
};
  
NodePointer demangleOldSymbolAsNode(StringRef MangledName,
                                    NodeFactory &Factory);

NodePointer demangleOldTypeAsNode(StringRef MangledName,
                                  NodeFactory &Factory);

} // end namespace NewMangling
} // end namespace swift

#endif // SWIFT_BASIC_DEMANGLER_H
