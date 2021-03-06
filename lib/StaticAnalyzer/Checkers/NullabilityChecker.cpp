//== Nullabilityhecker.cpp - Nullability checker ----------------*- C++ -*--==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This checker tries to find nullability violations. There are several kinds of
// possible violations:
// * Null pointer is passed to a pointer which has a _Nonnull type.
// * Null pointer is returned from a function which has a _Nonnull return type.
// * Nullable pointer is passed to a pointer which has a _Nonnull type.
// * Nullable pointer is returned from a function which has a _Nonnull return
//   type.
// * Nullable pointer is dereferenced.
//
// This checker propagates the nullability information of the pointers and looks
// for the patterns that are described above. Explicit casts are trusted and are
// considered a way to suppress false positives for this checker. The other way
// to suppress warnings would be to add asserts or guarding if statements to the
// code. In addition to the nullability propagation this checker also uses some
// heuristics to suppress potential false positives.
//
//===----------------------------------------------------------------------===//

#include "ClangSACheckers.h"
#include "llvm/Support/Path.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"

using namespace clang;
using namespace ento;

namespace {
// Do not reorder! The getMostNullable method relies on the order.
// Optimization: Most pointers expected to be unspecified. When a symbol has an
// unspecified or nonnull type non of the rules would indicate any problem for
// that symbol. For this reason only nullable and contradicted nullability are
// stored for a symbol. When a symbol is already contradicted, it can not be
// casted back to nullable.
enum class Nullability : char {
  Contradicted, // Tracked nullability is contradicted by an explicit cast. Do
                // not report any nullability related issue for this symbol.
                // This nullability is propagated agressively to avoid false
                // positive results. See the comment on getMostNullable method.
  Nullable,
  Unspecified,
  Nonnull
};

/// Returns the most nullable nullability. This is used for message expressions
/// like [reciever method], where the nullability of this expression is either
/// the nullability of the receiver or the nullability of the return type of the
/// method, depending on which is more nullable. Contradicted is considered to
/// be the most nullable, to avoid false positive results.
static Nullability getMostNullable(Nullability Lhs, Nullability Rhs) {
  return static_cast<Nullability>(
      std::min(static_cast<char>(Lhs), static_cast<char>(Rhs)));
}

static const char *getNullabilityString(Nullability Nullab) {
  switch (Nullab) {
  case Nullability::Contradicted:
    return "contradicted";
  case Nullability::Nullable:
    return "nullable";
  case Nullability::Unspecified:
    return "unspecified";
  case Nullability::Nonnull:
    return "nonnull";
  }
  assert(false);
  return "";
}

// These enums are used as an index to ErrorMessages array.
enum class ErrorKind : int {
  NilAssignedToNonnull,
  NilPassedToNonnull,
  NilReturnedToNonnull,
  NullableAssignedToNonnull,
  NullableReturnedToNonnull,
  NullableDereferenced,
  NullablePassedToNonnull
};

const char *ErrorMessages[] = {"Null pointer is assigned to a pointer which "
                               "has _Nonnull type",
                               "Null pointer is passed to a parameter which is "
                               "marked as _Nonnull",
                               "Null pointer is returned from a function that "
                               "has _Nonnull return type",
                               "Nullable pointer is assigned to a pointer "
                               "which has _Nonnull type",
                               "Nullable pointer is returned from a function "
                               "that has _Nonnull return type",
                               "Nullable pointer is dereferenced",
                               "Nullable pointer is passed to a parameter "
                               "which is marked as _Nonnull"};

class NullabilityChecker
    : public Checker<check::Bind, check::PreCall, check::PreStmt<ReturnStmt>,
                     check::PostCall, check::PostStmt<ExplicitCastExpr>,
                     check::PostObjCMessage, check::DeadSymbols,
                     check::Event<ImplicitNullDerefEvent>> {
  mutable std::unique_ptr<BugType> BT;

public:
  void checkBind(SVal L, SVal V, const Stmt *S, CheckerContext &C) const;
  void checkPostStmt(const ExplicitCastExpr *CE, CheckerContext &C) const;
  void checkPreStmt(const ReturnStmt *S, CheckerContext &C) const;
  void checkPostObjCMessage(const ObjCMethodCall &M, CheckerContext &C) const;
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  void checkDeadSymbols(SymbolReaper &SR, CheckerContext &C) const;
  void checkEvent(ImplicitNullDerefEvent Event) const;

  void printState(raw_ostream &Out, ProgramStateRef State, const char *NL,
                  const char *Sep) const override;

  struct NullabilityChecksFilter {
    DefaultBool CheckNullPassedToNonnull;
    DefaultBool CheckNullReturnedFromNonnull;
    DefaultBool CheckNullableDereferenced;
    DefaultBool CheckNullablePassedToNonnull;
    DefaultBool CheckNullableReturnedFromNonnull;

    CheckName CheckNameNullPassedToNonnull;
    CheckName CheckNameNullReturnedFromNonnull;
    CheckName CheckNameNullableDereferenced;
    CheckName CheckNameNullablePassedToNonnull;
    CheckName CheckNameNullableReturnedFromNonnull;
  };

  NullabilityChecksFilter Filter;

private:
  class NullabilityBugVisitor
      : public BugReporterVisitorImpl<NullabilityBugVisitor> {
  public:
    NullabilityBugVisitor(const MemRegion *M) : Region(M) {}

    void Profile(llvm::FoldingSetNodeID &ID) const override {
      static int X = 0;
      ID.AddPointer(&X);
      ID.AddPointer(Region);
    }

    PathDiagnosticPiece *VisitNode(const ExplodedNode *N,
                                   const ExplodedNode *PrevN,
                                   BugReporterContext &BRC,
                                   BugReport &BR) override;

  private:
    // The tracked region.
    const MemRegion *Region;
  };

  void reportBug(ErrorKind Error, ExplodedNode *N, const MemRegion *Region,
                 BugReporter &BR, const Stmt *ValueExpr = nullptr) const {
    if (!BT)
      BT.reset(new BugType(this, "Nullability", "Memory error"));
    const char *Msg = ErrorMessages[static_cast<int>(Error)];
    assert(Msg);
    std::unique_ptr<BugReport> R(new BugReport(*BT, Msg, N));
    if (Region) {
      R->markInteresting(Region);
      R->addVisitor(llvm::make_unique<NullabilityBugVisitor>(Region));
    }
    if (ValueExpr) {
      R->addRange(ValueExpr->getSourceRange());
      if (Error == ErrorKind::NilAssignedToNonnull ||
          Error == ErrorKind::NilPassedToNonnull ||
          Error == ErrorKind::NilReturnedToNonnull)
        bugreporter::trackNullOrUndefValue(N, ValueExpr, *R);
    }
    BR.emitReport(std::move(R));
  }
};

class NullabilityState {
public:
  NullabilityState(Nullability Nullab, const Stmt *Source = nullptr)
      : Nullab(Nullab), Source(Source) {}

  const Stmt *getNullabilitySource() const { return Source; }

  Nullability getValue() const { return Nullab; }

  void Profile(llvm::FoldingSetNodeID &ID) const {
    ID.AddInteger(static_cast<char>(Nullab));
    ID.AddPointer(Source);
  }

  void print(raw_ostream &Out) const {
    Out << getNullabilityString(Nullab) << "\n";
  }

private:
  Nullability Nullab;
  // Source is the expression which determined the nullability. For example in a
  // message like [nullable nonnull_returning] has nullable nullability, because
  // the receiver is nullable. Here the receiver will be the source of the
  // nullability. This is useful information when the diagnostics are generated.
  const Stmt *Source;
};

bool operator==(NullabilityState Lhs, NullabilityState Rhs) {
  return Lhs.getValue() == Rhs.getValue() &&
         Lhs.getNullabilitySource() == Rhs.getNullabilitySource();
}

} // end anonymous namespace

REGISTER_MAP_WITH_PROGRAMSTATE(NullabilityMap, const MemRegion *,
                               NullabilityState)

enum class NullConstraint { IsNull, IsNotNull, Unknown };

static NullConstraint getNullConstraint(DefinedOrUnknownSVal Val,
                                        ProgramStateRef State) {
  ConditionTruthVal Nullness = State->isNull(Val);
  if (Nullness.isConstrainedFalse())
    return NullConstraint::IsNotNull;
  if (Nullness.isConstrainedTrue())
    return NullConstraint::IsNull;
  return NullConstraint::Unknown;
}

// If an SVal wraps a region that should be tracked, it will return a pointer
// to the wrapped region. Otherwise it will return a nullptr.
static const SymbolicRegion *getTrackRegion(SVal Val,
                                            bool CheckSuperRegion = false) {
  auto RegionSVal = Val.getAs<loc::MemRegionVal>();
  if (!RegionSVal)
    return nullptr;

  const MemRegion *Region = RegionSVal->getRegion();

  if (CheckSuperRegion) {
    if (auto FieldReg = Region->getAs<FieldRegion>())
      return dyn_cast<SymbolicRegion>(FieldReg->getSuperRegion());
    else if (auto ElementReg = Region->getAs<ElementRegion>())
      return dyn_cast<SymbolicRegion>(ElementReg->getSuperRegion());
  }

  return dyn_cast<SymbolicRegion>(Region);
}

PathDiagnosticPiece *NullabilityChecker::NullabilityBugVisitor::VisitNode(
    const ExplodedNode *N, const ExplodedNode *PrevN, BugReporterContext &BRC,
    BugReport &BR) {
  ProgramStateRef state = N->getState();
  ProgramStateRef statePrev = PrevN->getState();

  const NullabilityState *TrackedNullab = state->get<NullabilityMap>(Region);
  const NullabilityState *TrackedNullabPrev =
      statePrev->get<NullabilityMap>(Region);
  if (!TrackedNullab)
    return nullptr;

  if (TrackedNullabPrev &&
      TrackedNullabPrev->getValue() == TrackedNullab->getValue())
    return nullptr;

  // Retrieve the associated statement.
  const Stmt *S = TrackedNullab->getNullabilitySource();
  if (!S) {
    ProgramPoint ProgLoc = N->getLocation();
    if (Optional<StmtPoint> SP = ProgLoc.getAs<StmtPoint>()) {
      S = SP->getStmt();
    }
  }

  if (!S)
    return nullptr;

  std::string InfoText =
      (llvm::Twine("Nullability '") +
       getNullabilityString(TrackedNullab->getValue()) + "' is infered")
          .str();

  // Generate the extra diagnostic.
  PathDiagnosticLocation Pos(S, BRC.getSourceManager(),
                             N->getLocationContext());
  return new PathDiagnosticEventPiece(Pos, InfoText, true, nullptr);
}

static Nullability getNullabilityAnnotation(QualType Type) {
  const auto *AttrType = Type->getAs<AttributedType>();
  if (!AttrType)
    return Nullability::Unspecified;
  if (AttrType->getAttrKind() == AttributedType::attr_nullable)
    return Nullability::Nullable;
  else if (AttrType->getAttrKind() == AttributedType::attr_nonnull)
    return Nullability::Nonnull;
  return Nullability::Unspecified;
}

/// Cleaning up the program state.
void NullabilityChecker::checkDeadSymbols(SymbolReaper &SR,
                                          CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  NullabilityMapTy Nullabilities = State->get<NullabilityMap>();
  for (NullabilityMapTy::iterator I = Nullabilities.begin(),
                                  E = Nullabilities.end();
       I != E; ++I) {
    if (!SR.isLiveRegion(I->first)) {
      State = State->remove<NullabilityMap>(I->first);
    }
  }
}

/// This callback triggers when a pointer is dereferenced and the analyzer does
/// not know anything about the value of that pointer. When that pointer is
/// nullable, this code emits a warning.
void NullabilityChecker::checkEvent(ImplicitNullDerefEvent Event) const {
  const MemRegion *Region =
      getTrackRegion(Event.Location, /*CheckSuperregion=*/true);
  if (!Region)
    return;

  ProgramStateRef State = Event.SinkNode->getState();
  const NullabilityState *TrackedNullability =
      State->get<NullabilityMap>(Region);

  if (!TrackedNullability)
    return;

  if (Filter.CheckNullableDereferenced &&
      TrackedNullability->getValue() == Nullability::Nullable) {
    BugReporter &BR = *Event.BR;
    if (Event.IsDirectDereference)
      reportBug(ErrorKind::NullableDereferenced, Event.SinkNode, Region, BR);
    else
      reportBug(ErrorKind::NullablePassedToNonnull, Event.SinkNode, Region, BR);
  }
}

/// This method check when nullable pointer or null value is returned from a
/// function that has nonnull return type.
///
/// TODO: when nullability preconditons are violated, it is ok to violate the
/// nullability postconditons (i.e.: when one of the nonnull parameters are null
/// this check should not report any nullability related issue).
void NullabilityChecker::checkPreStmt(const ReturnStmt *S,
                                      CheckerContext &C) const {
  auto RetExpr = S->getRetValue();
  if (!RetExpr)
    return;

  if (!RetExpr->getType()->isAnyPointerType())
    return;

  ProgramStateRef State = C.getState();
  auto RetSVal =
      State->getSVal(S, C.getLocationContext()).getAs<DefinedOrUnknownSVal>();
  if (!RetSVal)
    return;

  AnalysisDeclContext *DeclCtxt =
      C.getLocationContext()->getAnalysisDeclContext();
  const FunctionType *FuncType = DeclCtxt->getDecl()->getFunctionType();
  if (!FuncType)
    return;

  NullConstraint Nullness = getNullConstraint(*RetSVal, State);

  Nullability StaticNullability =
      getNullabilityAnnotation(FuncType->getReturnType());

  if (Filter.CheckNullReturnedFromNonnull &&
      Nullness == NullConstraint::IsNull &&
      StaticNullability == Nullability::Nonnull) {
    static CheckerProgramPointTag Tag(this, "NullReturnedFromNonnull");
    ExplodedNode *N = C.addTransition(State, C.getPredecessor(), &Tag);
    reportBug(ErrorKind::NilReturnedToNonnull, N, nullptr, C.getBugReporter(),
              S);
    return;
  }

  const MemRegion *Region = getTrackRegion(*RetSVal);
  if (!Region)
    return;

  const NullabilityState *TrackedNullability =
      State->get<NullabilityMap>(Region);
  if (TrackedNullability) {
    Nullability TrackedNullabValue = TrackedNullability->getValue();
    if (Filter.CheckNullableReturnedFromNonnull &&
        Nullness != NullConstraint::IsNotNull &&
        TrackedNullabValue == Nullability::Nullable &&
        StaticNullability == Nullability::Nonnull) {
      static CheckerProgramPointTag Tag(this, "NullableReturnedFromNonnull");
      ExplodedNode *N = C.addTransition(State, C.getPredecessor(), &Tag);
      reportBug(ErrorKind::NullableReturnedToNonnull, N, Region,
                C.getBugReporter());
    }
    return;
  }
  if (StaticNullability == Nullability::Nullable) {
    State = State->set<NullabilityMap>(Region,
                                       NullabilityState(StaticNullability, S));
    C.addTransition(State);
  }
}

/// This callback warns when a nullable pointer or a null value is passed to a
/// function that expects its argument to be nonnull.
void NullabilityChecker::checkPreCall(const CallEvent &Call,
                                      CheckerContext &C) const {
  if (!Call.getDecl())
    return;

  ProgramStateRef State = C.getState();
  ProgramStateRef OrigState = State;

  unsigned Idx = 0;
  for (const ParmVarDecl *Param : Call.parameters()) {
    if (Param->isParameterPack())
      break;

    const Expr *ArgExpr = nullptr;
    if (Idx < Call.getNumArgs())
      ArgExpr = Call.getArgExpr(Idx);
    auto ArgSVal = Call.getArgSVal(Idx++).getAs<DefinedOrUnknownSVal>();
    if (!ArgSVal)
      continue;

    if (!Param->getType()->isAnyPointerType() &&
        !Param->getType()->isReferenceType())
      continue;

    NullConstraint Nullness = getNullConstraint(*ArgSVal, State);

    Nullability ParamNullability = getNullabilityAnnotation(Param->getType());
    Nullability ArgStaticNullability =
        getNullabilityAnnotation(ArgExpr->getType());

    if (Filter.CheckNullPassedToNonnull && Nullness == NullConstraint::IsNull &&
        ArgStaticNullability != Nullability::Nonnull &&
        ParamNullability == Nullability::Nonnull) {
      static CheckerProgramPointTag Tag(this, "NullPassedToNonnull");
      ExplodedNode *N = C.generateSink(State, C.getPredecessor(), &Tag);
      reportBug(ErrorKind::NilPassedToNonnull, N, nullptr, C.getBugReporter(),
                ArgExpr);
      return;
    }

    const MemRegion *Region = getTrackRegion(*ArgSVal);
    if (!Region)
      continue;

    const NullabilityState *TrackedNullability =
        State->get<NullabilityMap>(Region);

    if (TrackedNullability) {
      if (Nullness == NullConstraint::IsNotNull ||
          TrackedNullability->getValue() != Nullability::Nullable)
        continue;

      if (Filter.CheckNullablePassedToNonnull &&
          ParamNullability == Nullability::Nonnull) {
        static CheckerProgramPointTag Tag(this, "NullablePassedToNonnull");
        ExplodedNode *N = C.generateSink(State, C.getPredecessor(), &Tag);
        reportBug(ErrorKind::NullablePassedToNonnull, N, Region,
                  C.getBugReporter(), ArgExpr);
        return;
      }
      if (Filter.CheckNullableDereferenced &&
          Param->getType()->isReferenceType()) {
        static CheckerProgramPointTag Tag(this, "NullableDereferenced");
        ExplodedNode *N = C.generateSink(State, C.getPredecessor(), &Tag);
        reportBug(ErrorKind::NullableDereferenced, N, Region,
                  C.getBugReporter(), ArgExpr);
        return;
      }
      continue;
    }
    // No tracked nullability yet.
    if (ArgStaticNullability != Nullability::Nullable)
      continue;
    State = State->set<NullabilityMap>(
        Region, NullabilityState(ArgStaticNullability, ArgExpr));
  }
  if (State != OrigState)
    C.addTransition(State);
}

/// Suppress the nullability warnings for some functions.
void NullabilityChecker::checkPostCall(const CallEvent &Call,
                                       CheckerContext &C) const {
  auto Decl = Call.getDecl();
  if (!Decl)
    return;
  // ObjC Messages handles in a different callback.
  if (Call.getKind() == CE_ObjCMessage)
    return;
  const FunctionType *FuncType = Decl->getFunctionType();
  if (!FuncType)
    return;
  QualType ReturnType = FuncType->getReturnType();
  if (!ReturnType->isAnyPointerType())
    return;
  const MemRegion *Region = getTrackRegion(Call.getReturnValue());
  if (!Region)
    return;
  ProgramStateRef State = C.getState();

  // CG headers are misannotated. Do not warn for symbols that are the results
  // of CG calls.
  const SourceManager &SM = C.getSourceManager();
  StringRef FilePath = SM.getFilename(SM.getSpellingLoc(Decl->getLocStart()));
  if (llvm::sys::path::filename(FilePath).startswith("CG")) {
    State = State->set<NullabilityMap>(Region, Nullability::Contradicted);
    C.addTransition(State);
    return;
  }

  const NullabilityState *TrackedNullability =
      State->get<NullabilityMap>(Region);

  if (!TrackedNullability &&
      getNullabilityAnnotation(ReturnType) == Nullability::Nullable) {
    State = State->set<NullabilityMap>(Region, Nullability::Nullable);
    C.addTransition(State);
  }
}

static Nullability getReceiverNullability(const ObjCMethodCall &M,
                                          ProgramStateRef State) {
  Nullability RetNullability = Nullability::Unspecified;
  if (M.isReceiverSelfOrSuper()) {
    // For super and super class receivers we assume that the receiver is
    // nonnull.
    RetNullability = Nullability::Nonnull;
  } else {
    // Otherwise look up nullability in the state.
    SVal Receiver = M.getReceiverSVal();
    auto ValueRegionSVal = Receiver.getAs<loc::MemRegionVal>();
    if (ValueRegionSVal) {
      const MemRegion *SelfRegion = ValueRegionSVal->getRegion();
      assert(SelfRegion);

      const NullabilityState *TrackedSelfNullability =
          State->get<NullabilityMap>(SelfRegion);
      if (TrackedSelfNullability) {
        RetNullability = TrackedSelfNullability->getValue();
      }
    }
    if (auto DefOrUnknown = Receiver.getAs<DefinedOrUnknownSVal>()) {
      // If the receiver is constrained to be nonnull, assume that it is nonnull
      // regardless of its type.
      NullConstraint Nullness = getNullConstraint(*DefOrUnknown, State);
      if (Nullness == NullConstraint::IsNotNull)
        RetNullability = Nullability::Nonnull;
    }
  }
  return RetNullability;
}

/// Calculate the nullability of the result of a message expr based on the
/// nullability of the receiver, the nullability of the return value, and the
/// constraints.
void NullabilityChecker::checkPostObjCMessage(const ObjCMethodCall &M,
                                              CheckerContext &C) const {
  auto Decl = M.getDecl();
  if (!Decl)
    return;
  QualType RetType = Decl->getReturnType();
  if (!RetType->isAnyPointerType())
    return;

  const MemRegion *ReturnRegion = getTrackRegion(M.getReturnValue());
  if (!ReturnRegion)
    return;

  ProgramStateRef State = C.getState();
  auto Interface = Decl->getClassInterface();
  auto Name = Interface ? Interface->getName() : "";
  // In order to reduce the noise in the diagnostics generated by this checker,
  // some framework and programming style based heuristics are used. These
  // heuristics are for Cocoa APIs which have NS prefix.
  if (Name.startswith("NS")) {
    // Developers rely on dynamic invariants such as an item should be available
    // in a collection, or a collection is not empty often. Those invariants can
    // not be inferred by any static analysis tool. To not to bother the users
    // with too many false positives, every item retrieval function should be
    // ignored for collections. The instance methods of dictionaries in Cocoa
    // are either item retrieval related or not interesting nullability wise.
    // Using this fact, to keep the code easier to read just ignore the return
    // value of every instance method of dictionaries.
    if (M.isInstanceMessage() && Name.find("Dictionary") != StringRef::npos) {
      State =
          State->set<NullabilityMap>(ReturnRegion, Nullability::Contradicted);
      C.addTransition(State);
      return;
    }
    // For similar reasons ignore some methods of Cocoa arrays.
    StringRef FirstSelectorSlot = M.getSelector().getNameForSlot(0);
    if (Name.find("Array") != StringRef::npos &&
        (FirstSelectorSlot == "firstObject" ||
         FirstSelectorSlot == "lastObject")) {
      State =
          State->set<NullabilityMap>(ReturnRegion, Nullability::Contradicted);
      C.addTransition(State);
      return;
    }

    // Encoding related methods of string should not fail when lossless
    // encodings are used. Using lossless encodings is so frequent that ignoring
    // this class of methods reduced the emitted diagnostics by about 30% on
    // some projects (and all of that was false positives).
    if (Name.find("String") != StringRef::npos) {
      for (auto Param : M.parameters()) {
        if (Param->getName() == "encoding") {
          State = State->set<NullabilityMap>(ReturnRegion,
                                             Nullability::Contradicted);
          C.addTransition(State);
          return;
        }
      }
    }
  }

  const ObjCMessageExpr *Message = M.getOriginExpr();
  Nullability SelfNullability = getReceiverNullability(M, State);

  const NullabilityState *NullabilityOfReturn =
      State->get<NullabilityMap>(ReturnRegion);

  if (NullabilityOfReturn) {
    // When we have a nullability tracked for the return value, the nullability
    // of the expression will be the most nullable of the receiver and the
    // return value.
    Nullability RetValTracked = NullabilityOfReturn->getValue();
    Nullability ComputedNullab =
        getMostNullable(RetValTracked, SelfNullability);
    if (ComputedNullab != RetValTracked &&
        ComputedNullab != Nullability::Unspecified) {
      const Stmt *NullabilitySource =
          ComputedNullab == RetValTracked
              ? NullabilityOfReturn->getNullabilitySource()
              : Message->getInstanceReceiver();
      State = State->set<NullabilityMap>(
          ReturnRegion, NullabilityState(ComputedNullab, NullabilitySource));
      C.addTransition(State);
    }
    return;
  }

  // No tracked information. Use static type information for return value.
  Nullability RetNullability = getNullabilityAnnotation(RetType);

  // Properties might be computed. For this reason the static analyzer creates a
  // new symbol each time an unknown property  is read. To avoid false pozitives
  // do not treat unknown properties as nullable, even when they explicitly
  // marked nullable.
  if (M.getMessageKind() == OCM_PropertyAccess && !C.wasInlined)
    RetNullability = Nullability::Nonnull;

  Nullability ComputedNullab = getMostNullable(RetNullability, SelfNullability);
  if (ComputedNullab == Nullability::Nullable) {
    const Stmt *NullabilitySource = ComputedNullab == RetNullability
                                        ? Message
                                        : Message->getInstanceReceiver();
    State = State->set<NullabilityMap>(
        ReturnRegion, NullabilityState(ComputedNullab, NullabilitySource));
    C.addTransition(State);
  }
}

/// Explicit casts are trusted. If there is a disagreement in the nullability
/// annotations in the destination and the source or '0' is casted to nonnull
/// track the value as having contraditory nullability. This will allow users to
/// suppress warnings.
void NullabilityChecker::checkPostStmt(const ExplicitCastExpr *CE,
                                       CheckerContext &C) const {
  QualType OriginType = CE->getSubExpr()->getType();
  QualType DestType = CE->getType();
  if (!OriginType->isAnyPointerType())
    return;
  if (!DestType->isAnyPointerType())
    return;

  Nullability DestNullability = getNullabilityAnnotation(DestType);

  // No explicit nullability in the destination type, so this cast does not
  // change the nullability.
  if (DestNullability == Nullability::Unspecified)
    return;

  ProgramStateRef State = C.getState();
  auto RegionSVal =
      State->getSVal(CE, C.getLocationContext()).getAs<DefinedOrUnknownSVal>();
  const MemRegion *Region = getTrackRegion(*RegionSVal);
  if (!Region)
    return;

  // When 0 is converted to nonnull mark it as contradicted.
  if (DestNullability == Nullability::Nonnull) {
    NullConstraint Nullness = getNullConstraint(*RegionSVal, State);
    if (Nullness == NullConstraint::IsNull) {
      State = State->set<NullabilityMap>(Region, Nullability::Contradicted);
      C.addTransition(State);
      return;
    }
  }

  const NullabilityState *TrackedNullability =
      State->get<NullabilityMap>(Region);

  if (!TrackedNullability) {
    if (DestNullability != Nullability::Nullable)
      return;
    State = State->set<NullabilityMap>(Region,
                                       NullabilityState(DestNullability, CE));
    C.addTransition(State);
    return;
  }

  if (TrackedNullability->getValue() != DestNullability &&
      TrackedNullability->getValue() != Nullability::Contradicted) {
    State = State->set<NullabilityMap>(Region, Nullability::Contradicted);
    C.addTransition(State);
  }
}

/// Propagate the nullability information through binds and warn when nullable
/// pointer or null symbol is assigned to a pointer with a nonnull type.
void NullabilityChecker::checkBind(SVal L, SVal V, const Stmt *S,
                                   CheckerContext &C) const {
  const TypedValueRegion *TVR =
      dyn_cast_or_null<TypedValueRegion>(L.getAsRegion());
  if (!TVR)
    return;

  QualType LocType = TVR->getValueType();
  if (!LocType->isAnyPointerType())
    return;

  auto ValDefOrUnknown = V.getAs<DefinedOrUnknownSVal>();
  if (!ValDefOrUnknown)
    return;

  ProgramStateRef State = C.getState();
  NullConstraint RhsNullness = getNullConstraint(*ValDefOrUnknown, State);

  Nullability ValNullability = Nullability::Unspecified;
  if (SymbolRef Sym = ValDefOrUnknown->getAsSymbol())
    ValNullability = getNullabilityAnnotation(Sym->getType());

  Nullability LocNullability = getNullabilityAnnotation(LocType);
  if (Filter.CheckNullPassedToNonnull &&
      RhsNullness == NullConstraint::IsNull &&
      ValNullability != Nullability::Nonnull &&
      LocNullability == Nullability::Nonnull) {
    static CheckerProgramPointTag Tag(this, "NullPassedToNonnull");
    ExplodedNode *N = C.addTransition(State, C.getPredecessor(), &Tag);
    reportBug(ErrorKind::NilAssignedToNonnull, N, nullptr, C.getBugReporter(),
              S);
    return;
  }
  // Intentionally missing case: '0' is bound to a reference. It is handled by
  // the DereferenceChecker.

  const MemRegion *ValueRegion = getTrackRegion(*ValDefOrUnknown);
  if (!ValueRegion)
    return;

  const NullabilityState *TrackedNullability =
      State->get<NullabilityMap>(ValueRegion);

  if (TrackedNullability) {
    if (RhsNullness == NullConstraint::IsNotNull ||
        TrackedNullability->getValue() != Nullability::Nullable)
      return;
    if (Filter.CheckNullablePassedToNonnull &&
        LocNullability == Nullability::Nonnull) {
      static CheckerProgramPointTag Tag(this, "NullablePassedToNonnull");
      ExplodedNode *N = C.addTransition(State, C.getPredecessor(), &Tag);
      reportBug(ErrorKind::NullableAssignedToNonnull, N, ValueRegion,
                C.getBugReporter());
    }
    return;
  }

  const auto *BinOp = dyn_cast<BinaryOperator>(S);

  if (ValNullability == Nullability::Nullable) {
    // Trust the static information of the value more than the static
    // information on the location.
    const Stmt *NullabilitySource = BinOp ? BinOp->getRHS() : S;
    State = State->set<NullabilityMap>(
        ValueRegion, NullabilityState(ValNullability, NullabilitySource));
    C.addTransition(State);
    return;
  }

  if (LocNullability == Nullability::Nullable) {
    const Stmt *NullabilitySource = BinOp ? BinOp->getLHS() : S;
    State = State->set<NullabilityMap>(
        ValueRegion, NullabilityState(LocNullability, NullabilitySource));
    C.addTransition(State);
  }
}

void NullabilityChecker::printState(raw_ostream &Out, ProgramStateRef State,
                                    const char *NL, const char *Sep) const {

  NullabilityMapTy B = State->get<NullabilityMap>();

  if (B.isEmpty())
    return;

  Out << Sep << NL;

  for (NullabilityMapTy::iterator I = B.begin(), E = B.end(); I != E; ++I) {
    Out << I->first << " : ";
    I->second.print(Out);
    Out << NL;
  }
}

#define REGISTER_CHECKER(name)                                                 \
  void ento::register##name##Checker(CheckerManager &mgr) {                    \
    NullabilityChecker *checker = mgr.registerChecker<NullabilityChecker>();   \
    checker->Filter.Check##name = true;                                        \
    checker->Filter.CheckName##name = mgr.getCurrentCheckName();               \
  }

REGISTER_CHECKER(NullPassedToNonnull)
REGISTER_CHECKER(NullReturnedFromNonnull)
REGISTER_CHECKER(NullableDereferenced)
REGISTER_CHECKER(NullablePassedToNonnull)
REGISTER_CHECKER(NullableReturnedFromNonnull)
