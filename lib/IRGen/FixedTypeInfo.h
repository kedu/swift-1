//===--- FixedTypeInfo.h - Supplement for fixed-layout types ----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines FixedTypeInfo, which supplements the TypeInfo
// interface for classes with (at least locally) fixed-layout type
// implementations.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_FIXEDTYPEINFO_H
#define SWIFT_IRGEN_FIXEDTYPEINFO_H

#include "Address.h"
#include "TypeInfo.h"
#include "llvm/ADT/BitVector.h"

namespace llvm {
  class ConstantInt;
}

namespace swift {
namespace irgen {

/// FixedTypeInfo - An abstract class designed for use when
/// implementing a type that has a statically known layout.
class FixedTypeInfo : public TypeInfo {
private:
  /// The storage size of this type in bytes.  This may be zero even
  /// for well-formed and complete types, such as a trivial enum or
  /// tuple.
  Size StorageSize;
  
  /// The spare bit mask for this type. SpareBits[0] is the LSB of the first
  /// byte. This may be empty if the type has no spare bits.
  llvm::BitVector SpareBits;
  
protected:
  FixedTypeInfo(llvm::Type *type, Size size, llvm::BitVector spareBits,
                Alignment align, IsPOD_t pod,
                SpecialTypeInfoKind stik = STIK_Fixed)
      : TypeInfo(type, align, pod, stik), StorageSize(size),
        SpareBits(std::move(spareBits)) {
    assert(isFixedSize());
  }

public:
  // This is useful for metaprogramming.
  static bool isFixed() { return true; }

  /// Whether this type is known to be empty.
  bool isKnownEmpty() const { return StorageSize.isZero(); }

  ContainedAddress allocateStack(IRGenFunction &IGF,
                                 const llvm::Twine &name) const override;
  void deallocateStack(IRGenFunction &IGF, Address addr) const override;

  OwnedAddress allocateBox(IRGenFunction &IGF,
                           const llvm::Twine &name) const override;

  // We can give these reasonable default implementations.

  void initializeWithTake(IRGenFunction &IGF, Address destAddr,
                          Address srcAddr) const override;

  std::pair<llvm::Value*, llvm::Value*>
  getSizeAndAlignmentMask(IRGenFunction &IGF) const override;
  std::tuple<llvm::Value*,llvm::Value*,llvm::Value*>
  getSizeAndAlignmentMaskAndStride(IRGenFunction &IGF) const override;
  llvm::Value *getSize(IRGenFunction &IGF) const override;
  llvm::Value *getAlignmentMask(IRGenFunction &IGF) const override;
  llvm::Value *getStride(IRGenFunction &IGF) const override;

  llvm::Constant *getStaticSize(IRGenModule &IGM) const override;
  llvm::Constant *getStaticAlignmentMask(IRGenModule &IGM) const override;
  llvm::Constant *getStaticStride(IRGenModule &IGM) const override;

  void completeFixed(Size size, Alignment alignment) {
    StorageSize = size;
    setStorageAlignment(alignment);
  }

  /// Returns the known, fixed size required to store a value of this type.
  Alignment getFixedAlignment() const {
    return getBestKnownAlignment();
  }

  /// Returns the known, fixed alignment of a stored value of this type.
  Size getFixedSize() const {
    return StorageSize;
  }

  /// Returns the (assumed fixed) stride of the storage for this
  /// object.  The stride is the storage size rounded up to the
  /// alignment; its practical use is that, in an array, it is the
  /// offset from the size of one element to the offset of the next.
  Size getFixedStride() const {
    return StorageSize.roundUpToAlignment(getFixedAlignment());
  }
  
  /// Returns the fixed number of "extra inhabitants" (that is, bit
  /// patterns that don't represent valid values of the type) in the type
  /// representation.
  virtual unsigned getFixedExtraInhabitantCount() const {
    return getSpareBitExtraInhabitantCount();
  }

  /// Returns the number of extra inhabitants available by exercising spare
  /// bits.
  unsigned getSpareBitExtraInhabitantCount() const;

  /// We can statically determine the presence of extra inhabitants for fixed
  /// types.
  bool mayHaveExtraInhabitants() const override {
    return getFixedExtraInhabitantCount() > 0;
  }

  /// Create a constant of the given bit width holding one of the extra
  /// inhabitants of the type.
  /// The index must be less than the value returned by
  /// getFixedExtraInhabitantCount().
  virtual llvm::ConstantInt *getFixedExtraInhabitantValue(IRGenModule &IGM,
                                                       unsigned bits,
                                                       unsigned index) const {
    return getSpareBitFixedExtraInhabitantValue(IGM, bits, index);
  }
  
  /// Create an extra inhabitant constant using the spare bits of the type.
  llvm::ConstantInt *getSpareBitFixedExtraInhabitantValue(IRGenModule &IGM,
                                                       unsigned bits,
                                                       unsigned index) const;
  
  /// Map an extra inhabitant representation in memory to a unique 31-bit
  /// identifier, and map a valid representation of the type to -1.
  llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF,
                                       Address src) const override {
    return getSpareBitExtraInhabitantIndex(IGF, src);
  }
  
  /// Map an extra inhabitant representation derived from spare bits to an
  /// index.
  llvm::Value *getSpareBitExtraInhabitantIndex(IRGenFunction &IGF,
                                               Address src) const;
  
  /// Store the extra inhabitant representation indexed by a 31-bit identifier
  /// to memory.
  void storeExtraInhabitant(IRGenFunction &IGF,
                            llvm::Value *index,
                            Address dest) const override {
    storeSpareBitExtraInhabitant(IGF, index, dest);
  }
  
  /// Store the indexed spare-bit-derived extra inhabitant to memory.
  void storeSpareBitExtraInhabitant(IRGenFunction &IGF,
                                    llvm::Value *index,
                                    Address dest) const;
  
  /// Get the spare bit mask for the type.
  const llvm::BitVector &getSpareBits() const { return SpareBits; }
  
  /// True if the type representation has statically "spare" unused bits.
  bool hasFixedSpareBits() const {
    return SpareBits.any();
  }
  
  /// Applies the fixed spare bits mask for this type to the given BitVector,
  /// clearing any bits used by valid representations of the type.
  ///
  /// If the bitvector is empty or smaller than this type, it is grown and
  /// filled with bits direct from the spare bits mask. If the bitvector is
  /// larger than this type, the trailing bits are untouched.
  ///
  /// The intent is that, for all the data types of an enum, you should be able
  /// to do this:
  ///
  ///   llvm::BitVector spareBits;
  ///   for (EnumElementDecl *elt : u->getAllElements())
  ///     getFragileTypeInfo(elt->getArgumentType())
  ///       .applyFixedSpareBitsMask(spareBits, 0);
  ///
  /// and end up with a spare bits mask for the entire enum.
  void applyFixedSpareBitsMask(llvm::BitVector &bits) const;
  
  /// Fixed-size types never need dynamic value witness table instantiation.
  void initializeValueWitnessTable(IRGenFunction &IGF,
                                   llvm::Value *metadata,
                                   llvm::Value *vwtable) const override {}
  
  static bool classof(const FixedTypeInfo *type) { return true; }
  static bool classof(const TypeInfo *type) { return type->isFixedSize(); }
};

}
}

#endif
