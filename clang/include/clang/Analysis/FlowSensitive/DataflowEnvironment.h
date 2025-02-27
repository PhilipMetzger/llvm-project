//===-- DataflowEnvironment.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines an Environment class that is used by dataflow analyses
//  that run over Control-Flow Graphs (CFGs) to keep track of the state of the
//  program at given program points.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ANALYSIS_FLOWSENSITIVE_DATAFLOWENVIRONMENT_H
#define LLVM_CLANG_ANALYSIS_FLOWSENSITIVE_DATAFLOWENVIRONMENT_H

#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"
#include "clang/Analysis/FlowSensitive/ControlFlowContext.h"
#include "clang/Analysis/FlowSensitive/DataflowAnalysisContext.h"
#include "clang/Analysis/FlowSensitive/DataflowLattice.h"
#include "clang/Analysis/FlowSensitive/Formula.h"
#include "clang/Analysis/FlowSensitive/Logger.h"
#include "clang/Analysis/FlowSensitive/StorageLocation.h"
#include "clang/Analysis/FlowSensitive/Value.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"
#include <memory>
#include <type_traits>
#include <utility>

namespace clang {
namespace dataflow {

/// Indicates the result of a tentative comparison.
enum class ComparisonResult {
  Same,
  Different,
  Unknown,
};

/// Holds the state of the program (store and heap) at a given program point.
///
/// WARNING: Symbolic values that are created by the environment for static
/// local and global variables are not currently invalidated on function calls.
/// This is unsound and should be taken into account when designing dataflow
/// analyses.
class Environment {
public:
  /// Supplements `Environment` with non-standard comparison and join
  /// operations.
  class ValueModel {
  public:
    virtual ~ValueModel() = default;

    /// Returns:
    ///   `Same`: `Val1` is equivalent to `Val2`, according to the model.
    ///   `Different`: `Val1` is distinct from `Val2`, according to the model.
    ///   `Unknown`: The model can't determine a relationship between `Val1` and
    ///    `Val2`.
    ///
    /// Requirements:
    ///
    ///  `Val1` and `Val2` must be distinct.
    ///
    ///  `Val1` and `Val2` must model values of type `Type`.
    ///
    ///  `Val1` and `Val2` must be assigned to the same storage location in
    ///  `Env1` and `Env2` respectively.
    virtual ComparisonResult compare(QualType Type, const Value &Val1,
                                     const Environment &Env1, const Value &Val2,
                                     const Environment &Env2) {
      // FIXME: Consider adding QualType to StructValue and removing the Type
      // argument here.
      return ComparisonResult::Unknown;
    }

    /// Modifies `MergedVal` to approximate both `Val1` and `Val2`. This could
    /// be a strict lattice join or a more general widening operation.
    ///
    /// If this function returns true, `MergedVal` will be assigned to a storage
    /// location of type `Type` in `MergedEnv`.
    ///
    /// `Env1` and `Env2` can be used to query child values and path condition
    /// implications of `Val1` and `Val2` respectively.
    ///
    /// Requirements:
    ///
    ///  `Val1` and `Val2` must be distinct.
    ///
    ///  `Val1`, `Val2`, and `MergedVal` must model values of type `Type`.
    ///
    ///  `Val1` and `Val2` must be assigned to the same storage location in
    ///  `Env1` and `Env2` respectively.
    virtual bool merge(QualType Type, const Value &Val1,
                       const Environment &Env1, const Value &Val2,
                       const Environment &Env2, Value &MergedVal,
                       Environment &MergedEnv) {
      return true;
    }

    /// This function may widen the current value -- replace it with an
    /// approximation that can reach a fixed point more quickly than iterated
    /// application of the transfer function alone. The previous value is
    /// provided to inform the choice of widened value. The function must also
    /// serve as a comparison operation, by indicating whether the widened value
    /// is equivalent to the previous value.
    ///
    /// Returns either:
    ///
    ///   `nullptr`, if this value is not of interest to the model, or
    ///
    ///   `&Prev`, if the widened value is equivalent to `Prev`, or
    ///
    ///   A non-null value that approximates `Current`. `Prev` is available to
    ///   inform the chosen approximation.
    ///
    /// `PrevEnv` and `CurrentEnv` can be used to query child values and path
    /// condition implications of `Prev` and `Current`, respectively.
    ///
    /// Requirements:
    ///
    ///  `Prev` and `Current` must model values of type `Type`.
    ///
    ///  `Prev` and `Current` must be assigned to the same storage location in
    ///  `PrevEnv` and `CurrentEnv`, respectively.
    virtual Value *widen(QualType Type, Value &Prev, const Environment &PrevEnv,
                         Value &Current, Environment &CurrentEnv) {
      // The default implementation reduces to just comparison, since comparison
      // is required by the API, even if no widening is performed.
      switch (compare(Type, Prev, PrevEnv, Current, CurrentEnv)) {
        case ComparisonResult::Same:
          return &Prev;
        case ComparisonResult::Different:
          return &Current;
        case ComparisonResult::Unknown:
          return nullptr;
      }
      llvm_unreachable("all cases in switch covered");
    }
  };

  /// Creates an environment that uses `DACtx` to store objects that encompass
  /// the state of a program.
  explicit Environment(DataflowAnalysisContext &DACtx);

  // Copy-constructor is private, Environments should not be copied. See fork().
  Environment &operator=(const Environment &Other) = delete;

  Environment(Environment &&Other) = default;
  Environment &operator=(Environment &&Other) = default;

  /// Creates an environment that uses `DACtx` to store objects that encompass
  /// the state of a program.
  ///
  /// If `DeclCtx` is a function, initializes the environment with symbolic
  /// representations of the function parameters.
  ///
  /// If `DeclCtx` is a non-static member function, initializes the environment
  /// with a symbolic representation of the `this` pointee.
  Environment(DataflowAnalysisContext &DACtx, const DeclContext &DeclCtx);

  /// Returns a new environment that is a copy of this one.
  ///
  /// The state of the program is initially the same, but can be mutated without
  /// affecting the original.
  ///
  /// However the original should not be further mutated, as this may interfere
  /// with the fork. (In practice, values are stored independently, but the
  /// forked flow condition references the original).
  Environment fork() const;

  /// Creates and returns an environment to use for an inline analysis  of the
  /// callee. Uses the storage location from each argument in the `Call` as the
  /// storage location for the corresponding parameter in the callee.
  ///
  /// Requirements:
  ///
  ///  The callee of `Call` must be a `FunctionDecl`.
  ///
  ///  The body of the callee must not reference globals.
  ///
  ///  The arguments of `Call` must map 1:1 to the callee's parameters.
  Environment pushCall(const CallExpr *Call) const;
  Environment pushCall(const CXXConstructExpr *Call) const;

  /// Moves gathered information back into `this` from a `CalleeEnv` created via
  /// `pushCall`.
  void popCall(const CallExpr *Call, const Environment &CalleeEnv);
  void popCall(const CXXConstructExpr *Call, const Environment &CalleeEnv);

  /// Returns true if and only if the environment is equivalent to `Other`, i.e
  /// the two environments:
  ///  - have the same mappings from declarations to storage locations,
  ///  - have the same mappings from expressions to storage locations,
  ///  - have the same or equivalent (according to `Model`) values assigned to
  ///    the same storage locations.
  ///
  /// Requirements:
  ///
  ///  `Other` and `this` must use the same `DataflowAnalysisContext`.
  bool equivalentTo(const Environment &Other,
                    Environment::ValueModel &Model) const;

  /// Joins two environments by taking the intersection of storage locations and
  /// values that are stored in them. Distinct values that are assigned to the
  /// same storage locations in `EnvA` and `EnvB` are merged using `Model`.
  ///
  /// Requirements:
  ///
  ///  `EnvA` and `EnvB` must use the same `DataflowAnalysisContext`.
  static Environment join(const Environment &EnvA, const Environment &EnvB,
                          Environment::ValueModel &Model);

  /// Widens the environment point-wise, using `PrevEnv` as needed to inform the
  /// approximation.
  ///
  /// Requirements:
  ///
  ///  `PrevEnv` must be the immediate previous version of the environment.
  ///  `PrevEnv` and `this` must use the same `DataflowAnalysisContext`.
  LatticeJoinEffect widen(const Environment &PrevEnv,
                          Environment::ValueModel &Model);

  // FIXME: Rename `createOrGetStorageLocation` to `getOrCreateStorageLocation`,
  // `getStableStorageLocation`, or something more appropriate.

  /// Creates a storage location appropriate for `Type`. Does not assign a value
  /// to the returned storage location in the environment.
  ///
  /// Requirements:
  ///
  ///  `Type` must not be null.
  StorageLocation &createStorageLocation(QualType Type);

  /// Creates a storage location for `D`. Does not assign the returned storage
  /// location to `D` in the environment. Does not assign a value to the
  /// returned storage location in the environment.
  StorageLocation &createStorageLocation(const VarDecl &D);

  /// Creates a storage location for `E`. Does not assign the returned storage
  /// location to `E` in the environment. Does not assign a value to the
  /// returned storage location in the environment.
  StorageLocation &createStorageLocation(const Expr &E);

  /// Assigns `Loc` as the storage location of `D` in the environment.
  ///
  /// Requirements:
  ///
  ///  `D` must not already have a storage location in the environment.
  ///
  ///  If `D` has reference type, `Loc` must refer directly to the referenced
  ///  object (if any), not to a `ReferenceValue`, and it is not permitted to
  ///  later change `Loc` to refer to a `ReferenceValue.`
  void setStorageLocation(const ValueDecl &D, StorageLocation &Loc);

  /// Returns the storage location assigned to `D` in the environment, or null
  /// if `D` isn't assigned a storage location in the environment.
  ///
  /// Note that if `D` has reference type, the storage location that is returned
  /// refers directly to the referenced object, not a `ReferenceValue`.
  StorageLocation *getStorageLocation(const ValueDecl &D) const;

  /// Assigns `Loc` as the storage location of the glvalue `E` in the
  /// environment.
  ///
  /// Requirements:
  ///
  ///  `E` must not be assigned a storage location in the environment.
  ///  `E` must be a glvalue or a `BuiltinType::BuiltinFn`
  void setStorageLocation(const Expr &E, StorageLocation &Loc);

  /// Deprecated synonym for `setStorageLocation()`.
  void setStorageLocationStrict(const Expr &E, StorageLocation &Loc) {
    setStorageLocation(E, Loc);
  }

  /// Returns the storage location assigned to the glvalue `E` in the
  /// environment, or null if `E` isn't assigned a storage location in the
  /// environment.
  ///
  /// If the storage location for `E` is associated with a
  /// `ReferenceValue RefVal`, returns `RefVal.getReferentLoc()` instead.
  ///
  /// Requirements:
  ///  `E` must be a glvalue or a `BuiltinType::BuiltinFn`
  StorageLocation *getStorageLocation(const Expr &E) const;

  /// Deprecated synonym for `getStorageLocation()`.
  StorageLocation *getStorageLocationStrict(const Expr &E) const {
    return getStorageLocation(E);
  }

  /// Returns the storage location assigned to the `this` pointee in the
  /// environment or null if the `this` pointee has no assigned storage location
  /// in the environment.
  AggregateStorageLocation *getThisPointeeStorageLocation() const;

  /// Returns the location of the result object for a record-type prvalue.
  ///
  /// In C++, prvalues of record type serve only a limited purpose: They can
  /// only be used to initialize a result object (e.g. a variable or a
  /// temporary). This function returns the location of that result object.
  ///
  /// When creating a prvalue of record type, we already need the storage
  /// location of the result object to pass in `this`, even though prvalues are
  /// otherwise not associated with storage locations.
  ///
  /// FIXME: Currently, this simply returns a stable storage location for `E`,
  /// but this doesn't do the right thing in scenarios like the following:
  /// ```
  /// MyClass c = some_condition()? MyClass(foo) : MyClass(bar);
  /// ```
  /// Here, `MyClass(foo)` and `MyClass(bar)` will have two different storage
  /// locations, when in fact their storage locations should be the same.
  /// Eventually, we want to propagate storage locations from result objects
  /// down to the prvalues that initialize them, similar to the way that this is
  /// done in Clang's CodeGen.
  ///
  /// Requirements:
  ///  `E` must be a prvalue of record type.
  AggregateStorageLocation &getResultObjectLocation(const Expr &RecordPRValue);

  /// Returns the return value of the current function. This can be null if:
  /// - The function has a void return type
  /// - No return value could be determined for the function, for example
  ///   because it calls a function without a body.
  ///
  /// Requirements:
  ///  The current function must have a non-reference return type.
  Value *getReturnValue() const {
    assert(getCurrentFunc() != nullptr &&
           !getCurrentFunc()->getReturnType()->isReferenceType());
    return ReturnVal;
  }

  /// Returns the storage location for the reference returned by the current
  /// function. This can be null if function doesn't return a single consistent
  /// reference.
  ///
  /// Requirements:
  ///  The current function must have a reference return type.
  StorageLocation *getReturnStorageLocation() const {
    assert(getCurrentFunc() != nullptr &&
           getCurrentFunc()->getReturnType()->isReferenceType());
    return ReturnLoc;
  }

  /// Sets the return value of the current function.
  ///
  /// Requirements:
  ///  The current function must have a non-reference return type.
  void setReturnValue(Value *Val) {
    assert(getCurrentFunc() != nullptr &&
           !getCurrentFunc()->getReturnType()->isReferenceType());
    ReturnVal = Val;
  }

  /// Sets the storage location for the reference returned by the current
  /// function.
  ///
  /// Requirements:
  ///  The current function must have a reference return type.
  void setReturnStorageLocation(StorageLocation *Loc) {
    assert(getCurrentFunc() != nullptr &&
           getCurrentFunc()->getReturnType()->isReferenceType());
    ReturnLoc = Loc;
  }

  /// Returns a pointer value that represents a null pointer. Calls with
  /// `PointeeType` that are canonically equivalent will return the same result.
  PointerValue &getOrCreateNullPointerValue(QualType PointeeType);

  /// Creates a value appropriate for `Type`, if `Type` is supported, otherwise
  /// returns null.
  ///
  /// If `Type` is a pointer or reference type, creates all the necessary
  /// storage locations and values for indirections until it finds a
  /// non-pointer/non-reference type.
  ///
  /// If `Type` is a class, struct, or union type, calls `setValue()` to
  /// associate the `StructValue` with its storage location
  /// (`StructValue::getAggregateLoc()`).
  ///
  /// If `Type` is one of the following types, this function will always return
  /// a non-null pointer:
  /// - `bool`
  /// - Any integer type
  /// - Any class, struct, or union type
  ///
  /// Requirements:
  ///
  ///  `Type` must not be null.
  Value *createValue(QualType Type);

  /// Creates an object (i.e. a storage location with an associated value) of
  /// type `Ty`. If `InitExpr` is non-null and has a value associated with it,
  /// initializes the object with this value. Otherwise, initializes the object
  /// with a value created using `createValue()`.
  StorageLocation &createObject(QualType Ty, const Expr *InitExpr = nullptr) {
    return createObjectInternal(nullptr, Ty, InitExpr);
  }

  /// Creates an object for the variable declaration `D`. If `D` has an
  /// initializer and this initializer is associated with a value, initializes
  /// the object with this value.  Otherwise, initializes the object with a
  /// value created using `createValue()`. Uses the storage location returned by
  /// `DataflowAnalysisContext::getStableStorageLocation(D)`.
  StorageLocation &createObject(const VarDecl &D) {
    return createObjectInternal(&D, D.getType(), D.getInit());
  }

  /// Creates an object for the variable declaration `D`. If `InitExpr` is
  /// non-null and has a value associated with it, initializes the object with
  /// this value. Otherwise, initializes the object with a value created using
  /// `createValue()`.  Uses the storage location returned by
  /// `DataflowAnalysisContext::getStableStorageLocation(D)`.
  StorageLocation &createObject(const VarDecl &D, const Expr *InitExpr) {
    return createObjectInternal(&D, D.getType(), InitExpr);
  }

  /// Assigns `Val` as the value of `Loc` in the environment.
  void setValue(const StorageLocation &Loc, Value &Val);

  /// Clears any association between `Loc` and a value in the environment.
  void clearValue(const StorageLocation &Loc) { LocToVal.erase(&Loc); }

  /// Assigns `Val` as the value of the prvalue `E` in the environment.
  ///
  /// If `E` is not yet associated with a storage location, associates it with
  /// a newly created storage location. In any case, associates the storage
  /// location of `E` with `Val`.
  ///
  /// Once the migration to strict handling of value categories is complete
  /// (see https://discourse.llvm.org/t/70086), this function will be renamed to
  /// `setValue()`. At this point, prvalue expressions will be associated
  /// directly with `Value`s, and the legacy behavior of associating prvalue
  /// expressions with storage locations (as described above) will be
  /// eliminated.
  ///
  /// Requirements:
  ///
  ///  `E` must be a prvalue
  ///  `Val` must not be a `ReferenceValue`
  ///  If `Val` is a `StructValue`, its `AggregateStorageLocation` must be the
  ///  same as that of any `StructValue` that has already been associated with
  ///  `E`. This is to guarantee that the result object initialized by a prvalue
  ///  `StructValue` has a durable storage location.
  void setValue(const Expr &E, Value &Val);

  /// Deprecated synonym for `setValue()`.
  void setValueStrict(const Expr &E, Value &Val) { setValue(E, Val); }

  /// Returns the value assigned to `Loc` in the environment or null if `Loc`
  /// isn't assigned a value in the environment.
  Value *getValue(const StorageLocation &Loc) const;

  /// Equivalent to `getValue(getStorageLocation(D))` if `D` is assigned a
  /// storage location in the environment, otherwise returns null.
  Value *getValue(const ValueDecl &D) const;

  /// Equivalent to `getValue(getStorageLocation(E, SP))` if `E` is assigned a
  /// storage location in the environment, otherwise returns null.
  Value *getValue(const Expr &E) const;

  // FIXME: should we deprecate the following & call arena().create() directly?

  /// Creates a `T` (some subclass of `Value`), forwarding `args` to the
  /// constructor, and returns a reference to it.
  ///
  /// The analysis context takes ownership of the created object. The object
  /// will be destroyed when the analysis context is destroyed.
  template <typename T, typename... Args>
  std::enable_if_t<std::is_base_of<Value, T>::value, T &>
  create(Args &&...args) {
    return arena().create<T>(std::forward<Args>(args)...);
  }

  /// Returns a symbolic integer value that models an integer literal equal to
  /// `Value`
  IntegerValue &getIntLiteralValue(llvm::APInt Value) const {
    return arena().makeIntLiteral(Value);
  }

  /// Returns a symbolic boolean value that models a boolean literal equal to
  /// `Value`
  AtomicBoolValue &getBoolLiteralValue(bool Value) const {
    return cast<AtomicBoolValue>(
        arena().makeBoolValue(arena().makeLiteral(Value)));
  }

  /// Returns an atomic boolean value.
  BoolValue &makeAtomicBoolValue() const {
    return arena().makeAtomValue();
  }

  /// Returns a unique instance of boolean Top.
  BoolValue &makeTopBoolValue() const {
    return arena().makeTopValue();
  }

  /// Returns a boolean value that represents the conjunction of `LHS` and
  /// `RHS`. Subsequent calls with the same arguments, regardless of their
  /// order, will return the same result. If the given boolean values represent
  /// the same value, the result will be the value itself.
  BoolValue &makeAnd(BoolValue &LHS, BoolValue &RHS) const {
    return arena().makeBoolValue(
        arena().makeAnd(LHS.formula(), RHS.formula()));
  }

  /// Returns a boolean value that represents the disjunction of `LHS` and
  /// `RHS`. Subsequent calls with the same arguments, regardless of their
  /// order, will return the same result. If the given boolean values represent
  /// the same value, the result will be the value itself.
  BoolValue &makeOr(BoolValue &LHS, BoolValue &RHS) const {
    return arena().makeBoolValue(
        arena().makeOr(LHS.formula(), RHS.formula()));
  }

  /// Returns a boolean value that represents the negation of `Val`. Subsequent
  /// calls with the same argument will return the same result.
  BoolValue &makeNot(BoolValue &Val) const {
    return arena().makeBoolValue(arena().makeNot(Val.formula()));
  }

  /// Returns a boolean value represents `LHS` => `RHS`. Subsequent calls with
  /// the same arguments, will return the same result. If the given boolean
  /// values represent the same value, the result will be a value that
  /// represents the true boolean literal.
  BoolValue &makeImplication(BoolValue &LHS, BoolValue &RHS) const {
    return arena().makeBoolValue(
        arena().makeImplies(LHS.formula(), RHS.formula()));
  }

  /// Returns a boolean value represents `LHS` <=> `RHS`. Subsequent calls with
  /// the same arguments, regardless of their order, will return the same
  /// result. If the given boolean values represent the same value, the result
  /// will be a value that represents the true boolean literal.
  BoolValue &makeIff(BoolValue &LHS, BoolValue &RHS) const {
    return arena().makeBoolValue(
        arena().makeEquals(LHS.formula(), RHS.formula()));
  }

  /// Returns a boolean variable that identifies the flow condition (FC).
  ///
  /// The flow condition is a set of facts that are necessarily true when the
  /// program reaches the current point, expressed as boolean formulas.
  /// The flow condition token is equivalent to the AND of these facts.
  ///
  /// These may e.g. constrain the value of certain variables. A pointer
  /// variable may have a consistent modeled PointerValue throughout, but at a
  /// given point the Environment may tell us that the value must be non-null.
  ///
  /// The FC is necessary but not sufficient for this point to be reachable.
  /// In particular, where the FC token appears in flow conditions of successor
  /// environments, it means "point X may have been reached", not
  /// "point X was reached".
  Atom getFlowConditionToken() const { return FlowConditionToken; }

  /// Record a fact that must be true if this point in the program is reached.
  void addToFlowCondition(const Formula &);

  /// Returns true if the formula is always true when this point is reached.
  /// Returns false if the formula may be false, or if the flow condition isn't
  /// sufficiently precise to prove that it is true.
  bool flowConditionImplies(const Formula &) const;

  /// Returns the `DeclContext` of the block being analysed, if any. Otherwise,
  /// returns null.
  const DeclContext *getDeclCtx() const { return CallStack.back(); }

  /// Returns the function currently being analyzed, or null if the code being
  /// analyzed isn't part of a function.
  const FunctionDecl *getCurrentFunc() const {
    return dyn_cast<FunctionDecl>(getDeclCtx());
  }

  /// Returns whether this `Environment` can be extended to analyze the given
  /// `Callee` (i.e. if `pushCall` can be used), with recursion disallowed and a
  /// given `MaxDepth`.
  bool canDescend(unsigned MaxDepth, const DeclContext *Callee) const;

  /// Returns the `DataflowAnalysisContext` used by the environment.
  DataflowAnalysisContext &getDataflowAnalysisContext() const { return *DACtx; }

  Arena &arena() const { return DACtx->arena(); }

  LLVM_DUMP_METHOD void dump() const;
  LLVM_DUMP_METHOD void dump(raw_ostream &OS) const;

private:
  // The copy-constructor is for use in fork() only.
  Environment(const Environment &) = default;

  /// Internal version of `setStorageLocation()` that doesn't check if the
  /// expression is a prvalue.
  void setStorageLocationInternal(const Expr &E, StorageLocation &Loc);

  /// Internal version of `getStorageLocation()` that doesn't check if the
  /// expression is a prvalue.
  StorageLocation *getStorageLocationInternal(const Expr &E) const;

  /// Creates a value appropriate for `Type`, if `Type` is supported, otherwise
  /// return null.
  ///
  /// Recursively initializes storage locations and values until it sees a
  /// self-referential pointer or reference type. `Visited` is used to track
  /// which types appeared in the reference/pointer chain in order to avoid
  /// creating a cyclic dependency with self-referential pointers/references.
  ///
  /// Requirements:
  ///
  ///  `Type` must not be null.
  Value *createValueUnlessSelfReferential(QualType Type,
                                          llvm::DenseSet<QualType> &Visited,
                                          int Depth, int &CreatedValuesCount);

  /// Creates a storage location for `Ty`. Also creates and associates a value
  /// with the storage location, unless values of this type are not supported or
  /// we hit one of the limits at which we stop producing values (controlled by
  /// `Visited`, `Depth`, and `CreatedValuesCount`).
  StorageLocation &createLocAndMaybeValue(QualType Ty,
                                          llvm::DenseSet<QualType> &Visited,
                                          int Depth, int &CreatedValuesCount);

  /// Shared implementation of `createObject()` overloads.
  /// `D` and `InitExpr` may be null.
  StorageLocation &createObjectInternal(const VarDecl *D, QualType Ty,
                                        const Expr *InitExpr);

  /// Shared implementation of `pushCall` overloads. Note that unlike
  /// `pushCall`, this member is invoked on the environment of the callee, not
  /// of the caller.
  void pushCallInternal(const FunctionDecl *FuncDecl,
                        ArrayRef<const Expr *> Args);

  /// Assigns storage locations and values to all global variables, fields
  /// and functions referenced in `FuncDecl`. `FuncDecl` must have a body.
  void initFieldsGlobalsAndFuncs(const FunctionDecl *FuncDecl);

  // `DACtx` is not null and not owned by this object.
  DataflowAnalysisContext *DACtx;

  // FIXME: move the fields `CallStack`, `ReturnVal`, `ReturnLoc` and
  // `ThisPointeeLoc` into a separate call-context object, shared between
  // environments in the same call.
  // https://github.com/llvm/llvm-project/issues/59005

  // `DeclContext` of the block being analysed if provided.
  std::vector<const DeclContext *> CallStack;

  // Value returned by the function (if it has non-reference return type).
  Value *ReturnVal = nullptr;
  // Storage location of the reference returned by the function (if it has
  // reference return type).
  StorageLocation *ReturnLoc = nullptr;
  // The storage location of the `this` pointee. Should only be null if the
  // function being analyzed is only a function and not a method.
  AggregateStorageLocation *ThisPointeeLoc = nullptr;

  // Maps from program declarations and statements to storage locations that are
  // assigned to them. Unlike the maps in `DataflowAnalysisContext`, these
  // include only storage locations that are in scope for a particular basic
  // block.
  llvm::DenseMap<const ValueDecl *, StorageLocation *> DeclToLoc;
  llvm::DenseMap<const Expr *, StorageLocation *> ExprToLoc;
  // We preserve insertion order so that join/widen process values in
  // deterministic sequence. This in turn produces deterministic SAT formulas.
  llvm::MapVector<const StorageLocation *, Value *> LocToVal;

  Atom FlowConditionToken;
};

/// Returns the storage location for the implicit object of a
/// `CXXMemberCallExpr`, or null if none is defined in the environment.
/// Dereferences the pointer if the member call expression was written using
/// `->`.
AggregateStorageLocation *
getImplicitObjectLocation(const CXXMemberCallExpr &MCE, const Environment &Env);

/// Returns the storage location for the base object of a `MemberExpr`, or null
/// if none is defined in the environment. Dereferences the pointer if the
/// member expression was written using `->`.
AggregateStorageLocation *getBaseObjectLocation(const MemberExpr &ME,
                                                const Environment &Env);

/// Returns the fields of `RD` that are initialized by an `InitListExpr`, in the
/// order in which they appear in `InitListExpr::inits()`.
std::vector<FieldDecl *> getFieldsForInitListExpr(const RecordDecl *RD);

/// Associates a new `StructValue` with `Loc` and returns the new value.
/// It is not defined whether the field values remain the same or not.
///
/// This function is primarily intended for use by checks that set custom
/// properties on `StructValue`s to model the state of these values. Such checks
/// should avoid modifying the properties of an existing `StructValue` because
/// these changes would be visible to other `Environment`s that share the same
/// `StructValue`. Instead, call `refreshStructValue()`, then set the properties
/// on the new `StructValue` that it returns. Typical usage:
///
///   refreshStructValue(Loc, Env).setProperty("my_prop", MyPropValue);
StructValue &refreshStructValue(AggregateStorageLocation &Loc,
                                Environment &Env);

/// Associates a new `StructValue` with `Expr` and returns the new value.
/// See also documentation for the overload above.
StructValue &refreshStructValue(const Expr &Expr, Environment &Env);

} // namespace dataflow
} // namespace clang

#endif // LLVM_CLANG_ANALYSIS_FLOWSENSITIVE_DATAFLOWENVIRONMENT_H
