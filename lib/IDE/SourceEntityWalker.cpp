//===--- SourceEntityWalker.cpp - Routines for semantic source info -------===//
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

#include "swift/IDE/SourceEntityWalker.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeRepr.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Parse/Lexer.h"
#include "clang/Basic/Module.h"

using namespace swift;

namespace {

class SemaAnnotator : public ASTWalker {
  SourceEntityWalker &SEWalker;
  SmallVector<ConstructorRefCallExpr *, 2> CtorRefs;
  SmallVector<ExtensionDecl *, 2> ExtDecls;
  llvm::SmallDenseMap<OpaqueValueExpr *, Expr *, 4> OpaqueValueMap;
  llvm::SmallPtrSet<Expr *, 16> ExprsToSkip;
  bool Cancelled = false;
  Optional<AccessKind> OpAccess;

public:
  explicit SemaAnnotator(SourceEntityWalker &SEWalker)
    : SEWalker(SEWalker) { }

  // TODO: Can we bail using Action::Stop instead of setting and checking
  // this flag?
  bool isDone() const { return Cancelled; }

private:

  // FIXME: Remove this
  bool shouldWalkAccessorsTheOldWay() override { return true; }

  bool shouldWalkIntoGenericParams() override {
    return SEWalker.shouldWalkIntoGenericParams();
  }

  bool shouldWalkSerializedTopLevelInternalDecls() override {
    return false;
  }

  PreWalkAction walkToDeclPre(Decl *D) override;
  PreWalkAction walkToDeclPreProper(Decl *D);
  PreWalkResult<Expr *> walkToExprPre(Expr *E) override;
  PreWalkAction walkToTypeReprPre(TypeRepr *T) override;

  PostWalkAction walkToDeclPost(Decl *D) override;
  PostWalkAction walkToDeclPostProper(Decl *D);
  PostWalkResult<Expr *> walkToExprPost(Expr *E) override;
  PostWalkAction walkToTypeReprPost(TypeRepr *T) override;

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override;
  PostWalkResult<Stmt *> walkToStmtPost(Stmt *S) override;

  PreWalkResult<ArgumentList *>
  walkToArgumentListPre(ArgumentList *ArgList) override;

  PreWalkResult<Pattern *> walkToPatternPre(Pattern *P) override;
  PostWalkResult<Pattern *> walkToPatternPost(Pattern *P) override;

  bool handleImports(ImportDecl *Import);
  bool handleCustomAttributes(Decl *D);
  bool passModulePathElements(ImportPath::Module Path,
                              const clang::Module *ClangMod);

  bool passReference(ValueDecl *D, Type Ty, SourceLoc Loc, SourceRange Range,
                     ReferenceMetaData Data);
  bool passReference(ValueDecl *D, Type Ty, DeclNameLoc Loc, ReferenceMetaData Data);
  bool passReference(ModuleEntity Mod, ImportPath::Element IdLoc);

  bool passSubscriptReference(ValueDecl *D, SourceLoc Loc,
                              ReferenceMetaData Data, bool IsOpenBracket);
  bool passCallAsFunctionReference(ValueDecl *D, SourceLoc Loc,
                                   ReferenceMetaData Data);

  bool passCallArgNames(Expr *Fn, ArgumentList *ArgList);

  bool shouldIgnore(Decl *D);

  ValueDecl *extractDecl(Expr *Fn) const {
    Fn = Fn->getSemanticsProvidingExpr();
    if (auto *DRE = dyn_cast<DeclRefExpr>(Fn))
      return DRE->getDecl();
    if (auto ApplyE = dyn_cast<ApplyExpr>(Fn))
      return extractDecl(ApplyE->getFn());
    if (auto *ACE = dyn_cast<AutoClosureExpr>(Fn)) {
      if (auto *Unwrapped = ACE->getUnwrappedCurryThunkExpr())
        return extractDecl(Unwrapped);
    }
    return nullptr;
  }
};

} // end anonymous namespace

ASTWalker::PreWalkAction SemaAnnotator::walkToDeclPre(Decl *D) {
  if (isDone())
    return Action::SkipChildren();

  if (shouldIgnore(D)) {
    // If we return true here, the children will still be visited, but we won't
    // call walkToDeclPre on SEWalker. The corresponding walkToDeclPost call
    // on SEWalker will be prevented by the check for shouldIgnore in
    // walkToDeclPost in SemaAnnotator.
    return Action::VisitChildrenIf(isa<PatternBindingDecl>(D));
  }

  SEWalker.beginBalancedASTOrderDeclVisit(D);
  auto Result = walkToDeclPreProper(D);

  if (Result.Action != PreWalkAction::Continue) {
    // To satisfy the contract of balanced calls to
    // begin/endBalancedASTOrderDeclVisit, we must call
    // endBalancedASTOrderDeclVisit here if walkToDeclPost isn't going to be
    // called.
    SEWalker.endBalancedASTOrderDeclVisit(D);
  }

  return Result;
}

ASTWalker::PreWalkAction SemaAnnotator::walkToDeclPreProper(Decl *D) {
  if (!handleCustomAttributes(D)) {
    Cancelled = true;
    return Action::SkipChildren();
  }

  SourceLoc Loc = D->getLoc();
  unsigned NameLen = 0;
  bool IsExtension = false;

  if (auto *VD = dyn_cast<ValueDecl>(D)) {
    if (!VD->isImplicit()) {
      SourceManager &SM = VD->getASTContext().SourceMgr;
      if (VD->hasName()) {
        NameLen = VD->getBaseName().userFacingName().size();
        if (Loc.isValid() && SM.extractText({Loc, 1}) == "`")
          NameLen += 2;
      } else if (Loc.isValid() && SM.extractText({Loc, 1}) == "_") {
        NameLen = 1;
      }
    }

    auto ReportParamList = [&](ParameterList *PL) {
      for (auto *PD : *PL) {
        auto Loc = PD->getArgumentNameLoc();
        if (Loc.isInvalid())
          continue;
        if (!SEWalker.visitDeclarationArgumentName(PD->getArgumentName(), Loc,
                                                   VD)) {
          Cancelled = true;
          return false;
        }
      }
      return true;
    };

    if (isa<AbstractFunctionDecl>(VD) || isa<SubscriptDecl>(VD)) {
      auto ParamList = getParameterList(VD);
      if (!ReportParamList(ParamList))
        return Action::SkipChildren();
    }

    if (auto proto = dyn_cast<ProtocolDecl>(VD)) {
      // Report a primary associated type as a references to the associated type
      // declaration.
      for (auto parsedName : proto->getPrimaryAssociatedTypeNames()) {
        Identifier name = parsedName.first;
        SourceLoc loc = parsedName.second;
        if (auto assocTypeDecl = proto->getAssociatedType(name)) {
          passReference(assocTypeDecl,
                        assocTypeDecl->getDeclaredInterfaceType(),
                        DeclNameLoc(loc),
                        ReferenceMetaData(SemaReferenceKind::TypeRef, None));
        }
      }
    }
  } else if (auto *ED = dyn_cast<ExtensionDecl>(D)) {
    SourceRange SR = SourceRange();
    if (auto *repr = ED->getExtendedTypeRepr())
      SR = repr->getSourceRange();
    Loc = SR.Start;
    if (Loc.isValid())
      NameLen = ED->getASTContext().SourceMgr.getByteDistance(SR.Start, SR.End);
    IsExtension = true;
  } else if (auto Import = dyn_cast<ImportDecl>(D)) {
    if (!handleImports(Import))
      return Action::SkipChildren();

  } else if (auto OpD = dyn_cast<OperatorDecl>(D)) {
    Loc = OpD->getLoc();
    if (Loc.isValid())
      NameLen = OpD->getName().getLength();

  } else if (auto PrecD = dyn_cast<PrecedenceGroupDecl>(D)) {
    Loc = PrecD->getLoc();
    if (Loc.isValid())
      NameLen = PrecD->getName().getLength();

  } else if (auto *ICD = dyn_cast<IfConfigDecl>(D)) {
    if (SEWalker.shouldWalkInactiveConfigRegion()) {
      for (auto Clause : ICD->getClauses()) {
        for (auto Member : Clause.Elements) {
          Member.walk(*this);
        }
      }
      return Action::SkipChildren();
    }
  }

  CharSourceRange Range = (Loc.isValid()) ? CharSourceRange(Loc, NameLen)
                                          : CharSourceRange();
  bool ShouldVisitChildren = SEWalker.walkToDeclPre(D, Range);
  // walkToDeclPost is only called when visiting children, so make sure to only
  // push the extension decl in that case (otherwise it won't be popped)
  if (IsExtension && ShouldVisitChildren) {
    ExtDecls.push_back(static_cast<ExtensionDecl*>(D));
  }
  return Action::VisitChildrenIf(ShouldVisitChildren);
}

ASTWalker::PostWalkAction SemaAnnotator::walkToDeclPost(Decl *D) {
  auto Action = walkToDeclPostProper(D);
  SEWalker.endBalancedASTOrderDeclVisit(D);
  return Action;
}

ASTWalker::PostWalkAction SemaAnnotator::walkToDeclPostProper(Decl *D) {
  if (isDone())
    return Action::Stop();

  if (shouldIgnore(D))
    return Action::Continue();

  if (isa<ExtensionDecl>(D)) {
    assert(ExtDecls.back() == D);
    ExtDecls.pop_back();
  }

  bool Continue = SEWalker.walkToDeclPost(D);
  if (!Continue) {
    Cancelled = true;
    return Action::Stop();
  }
  return Action::Continue();
}

ASTWalker::PreWalkResult<Stmt *> SemaAnnotator::walkToStmtPre(Stmt *S) {
  if (isDone())
    return Action::Stop();

  bool TraverseChildren = SEWalker.walkToStmtPre(S);
  if (TraverseChildren) {
    if (auto *DeferS = dyn_cast<DeferStmt>(S)) {
      // Since 'DeferStmt::getTempDecl()' is marked as implicit, we manually
      // walk into the body.
      if (auto *FD = DeferS->getTempDecl()) {
        auto *Body = FD->getBody();
        if (!Body)
          return Action::Stop();

        auto *RetS = Body->walk(*this);
        if (!RetS)
          return Action::Stop();
        assert(RetS == Body);
      }
      bool Continue = SEWalker.walkToStmtPost(DeferS);
      if (!Continue) {
        Cancelled = true;
        return Action::Stop();
      }
      // Already walked children.
      return Action::SkipChildren(DeferS);
    }
  }
  return Action::VisitChildrenIf(TraverseChildren, S);
}

ASTWalker::PostWalkResult<Stmt *> SemaAnnotator::walkToStmtPost(Stmt *S) {
  if (isDone())
    return Action::Stop();

  bool Continue = SEWalker.walkToStmtPost(S);
  if (!Continue) {
    Cancelled = true;
    return Action::Stop();
  }
  return Action::Continue(S);
}

static SemaReferenceKind getReferenceKind(Expr *Parent, Expr *E) {
  if (auto SA = dyn_cast_or_null<SelfApplyExpr>(Parent)) {
    if (SA->getFn() == E)
      return SemaReferenceKind::DeclMemberRef;
  }
  return SemaReferenceKind::DeclRef;
}

ASTWalker::PreWalkResult<ArgumentList *>
SemaAnnotator::walkToArgumentListPre(ArgumentList *ArgList) {
  auto doStopTraversal = [&]() {
    Cancelled = true;
    return Action::Stop();
  };

  // Don't consider the argument labels for an implicit ArgumentList.
  if (ArgList->isImplicit())
    return Action::Continue(ArgList);

  // FIXME: What about SubscriptExpr and KeyPathExpr arg labels? (SR-15063)
  if (auto CallE = dyn_cast_or_null<CallExpr>(Parent.getAsExpr())) {
    if (!passCallArgNames(CallE->getFn(), ArgList))
      return doStopTraversal();
  }
  return Action::Continue(ArgList);
}

ASTWalker::PreWalkResult<Expr *> SemaAnnotator::walkToExprPre(Expr *E) {
  assert(E);

  if (isDone())
    return Action::Stop();

  if (ExprsToSkip.count(E) != 0) {
    // We are skipping the expression. Call neither walkToExprPr nor
    // walkToExprPost on it
    return Action::SkipChildren(E);
  }

  auto doStopTraversal = [&]() {
    Cancelled = true;
    return Action::Stop();
  };

  // Skip the synthesized curry thunks and just walk over the unwrapped
  // expression
  if (auto *ACE = dyn_cast<AutoClosureExpr>(E)) {
    if (auto *SubExpr = ACE->getUnwrappedCurryThunkExpr()) {
      if (!SubExpr->walk(*this))
        return doStopTraversal();
      return Action::SkipChildren(E);
    }
  }

  if (!SEWalker.walkToExprPre(E)) {
    return Action::SkipChildren(E);
  }

  auto doSkipChildren = [&]() -> PreWalkResult<Expr *> {
    // If we decide to skip the children after having issued the call to
    // walkToExprPre, we need to simulate a corresponding call to walkToExprPost
    // which will not be issued by the ASTWalker if we return false in the first
    // component.
    // TODO: We should consider changing Action::SkipChildren to still call
    // walkToExprPost, which would eliminate the need for this.
    auto postWalkResult = walkToExprPost(E);
    switch (postWalkResult.Action.Action) {
    case PostWalkAction::Stop:
      return Action::Stop();
    case PostWalkAction::Continue:
      return Action::SkipChildren(*postWalkResult.Value);
    }
    llvm_unreachable("Unhandled case in switch!");
  };

  if (auto *CtorRefE = dyn_cast<ConstructorRefCallExpr>(E))
    CtorRefs.push_back(CtorRefE);

  if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    auto *FD = dyn_cast<FuncDecl>(DRE->getDecl());
    // Handle implicit callAsFunction reference. An explicit reference will be
    // handled by the usual DeclRefExpr case below.
    if (DRE->isImplicit() && FD && FD->isCallAsFunctionMethod()) {
      ReferenceMetaData data(SemaReferenceKind::DeclMemberRef, OpAccess);
      if (!passCallAsFunctionReference(FD, DRE->getLoc(), data))
        return Action::Stop();

      return Action::Continue(E);
    }
  }

  if (!isa<InOutExpr>(E) && !isa<LoadExpr>(E) && !isa<OpenExistentialExpr>(E) &&
      !isa<MakeTemporarilyEscapableExpr>(E) &&
      !isa<CollectionUpcastConversionExpr>(E) && !isa<OpaqueValueExpr>(E) &&
      !isa<SubscriptExpr>(E) && !isa<KeyPathExpr>(E) && !isa<LiteralExpr>(E) &&
      !isa<CollectionExpr>(E) && E->isImplicit())
    return Action::Continue(E);

  if (auto LE = dyn_cast<LiteralExpr>(E)) {
    if (LE->getInitializer() &&
        !passReference(LE->getInitializer().getDecl(), LE->getType(), {},
                       LE->getSourceRange(),
                       ReferenceMetaData(SemaReferenceKind::DeclRef, OpAccess,
                                         /*isImplicit=*/true))) {
      return doStopTraversal();
    }
    return Action::Continue(E);
  } else if (auto CE = dyn_cast<CollectionExpr>(E)) {
    if (CE->getInitializer() &&
        !passReference(CE->getInitializer().getDecl(), CE->getType(), {},
                       CE->getSourceRange(),
                       ReferenceMetaData(SemaReferenceKind::DeclRef, OpAccess,
                                         /*isImplicit=*/true))) {
      return doStopTraversal();
    }
    return Action::Continue(E);
  } else if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    if (auto *module = dyn_cast<ModuleDecl>(DRE->getDecl())) {
      if (!passReference(ModuleEntity(module),
                         {module->getName(), E->getLoc()}))
        return doStopTraversal();
    } else if (!passReference(DRE->getDecl(), DRE->getType(),
                              DRE->getNameLoc(),
                      ReferenceMetaData(getReferenceKind(Parent.getAsExpr(), DRE),
                                        OpAccess))) {
      return doStopTraversal();
    }
  } else if (auto *MRE = dyn_cast<MemberRefExpr>(E)) {
    {
      // This could be made more accurate if the member is nonmutating,
      // or whatever.
      Optional<AccessKind> NewOpAccess;
      if (OpAccess) {
        if (*OpAccess == AccessKind::Write)
          NewOpAccess = AccessKind::ReadWrite;
        else
          NewOpAccess = OpAccess;
      }

      llvm::SaveAndRestore<Optional<AccessKind>>
        C(this->OpAccess, NewOpAccess);

      // Visit in source order.
      if (!MRE->getBase()->walk(*this))
        return doStopTraversal();
    }

    if (!passReference(MRE->getMember().getDecl(), MRE->getType(),
                       MRE->getNameLoc(),
                       ReferenceMetaData(SemaReferenceKind::DeclMemberRef,
                                         OpAccess)))
      return doStopTraversal();

    // We already visited the children.
    return doSkipChildren();

  } else if (auto OtherCtorE = dyn_cast<OtherConstructorDeclRefExpr>(E)) {
    if (!passReference(OtherCtorE->getDecl(), OtherCtorE->getType(),
                       OtherCtorE->getConstructorLoc(),
                       ReferenceMetaData(SemaReferenceKind::DeclConstructorRef,
                                         OpAccess)))
      return doStopTraversal();

  } else if (auto *SE = dyn_cast<SubscriptExpr>(E)) {
    // Visit in source order.
    if (!SE->getBase()->walk(*this))
      return doStopTraversal();

    ValueDecl *SubscrD = nullptr;
    if (SE->hasDecl())
      SubscrD = SE->getDecl().getDecl();

    ReferenceMetaData data(SemaReferenceKind::SubscriptRef, OpAccess,
                           SE->isImplicit());

    if (SubscrD) {
      if (!passSubscriptReference(SubscrD, E->getLoc(), data, true))
        return doStopTraversal();
    }

    if (!SE->getArgs()->walk(*this))
      return doStopTraversal();

    if (SubscrD) {
      if (!passSubscriptReference(SubscrD, E->getEndLoc(), data, false))
        return doStopTraversal();
    }

    // We already visited the children.
    return doSkipChildren();

  } else if (auto *KPE = dyn_cast<KeyPathExpr>(E)) {
    for (auto &component : KPE->getComponents()) {
      switch (component.getKind()) {
      case KeyPathExpr::Component::Kind::Property:
      case KeyPathExpr::Component::Kind::Subscript: {
        auto *decl = component.getDeclRef().getDecl();
        auto loc = component.getLoc();
        SourceRange range(loc, loc);
        passReference(decl, component.getComponentType(), loc, range,
                      ReferenceMetaData(
                        (isa<SubscriptDecl>(decl)
                          ? SemaReferenceKind::SubscriptRef
                          : SemaReferenceKind::DeclMemberRef),
                        OpAccess));
        break;
      }

      case KeyPathExpr::Component::Kind::TupleElement:
      case KeyPathExpr::Component::Kind::Invalid:
      case KeyPathExpr::Component::Kind::UnresolvedProperty:
      case KeyPathExpr::Component::Kind::UnresolvedSubscript:
      case KeyPathExpr::Component::Kind::OptionalChain:
      case KeyPathExpr::Component::Kind::OptionalWrap:
      case KeyPathExpr::Component::Kind::OptionalForce:
      case KeyPathExpr::Component::Kind::Identity:
      case KeyPathExpr::Component::Kind::DictionaryKey:
      case KeyPathExpr::Component::Kind::CodeCompletion:
        break;
      }
    }
  } else if (auto *BinE = dyn_cast<BinaryExpr>(E)) {
    // Visit in source order.
    if (!BinE->getLHS()->walk(*this))
      return doStopTraversal();
    if (!BinE->getFn()->walk(*this))
      return doStopTraversal();
    if (!BinE->getRHS()->walk(*this))
      return doStopTraversal();

    // We already visited the children.
    return doSkipChildren();
  } else if (auto IOE = dyn_cast<InOutExpr>(E)) {
    llvm::SaveAndRestore<Optional<AccessKind>>
      C(this->OpAccess, AccessKind::ReadWrite);

    if (!IOE->getSubExpr()->walk(*this))
      return doStopTraversal();

    // We already visited the children.
    return doSkipChildren();
  } else if (auto LE = dyn_cast<LoadExpr>(E)) {
    llvm::SaveAndRestore<Optional<AccessKind>>
      C(this->OpAccess, AccessKind::Read);

    if (!LE->getSubExpr()->walk(*this))
      return doStopTraversal();

    // We already visited the children.
    return doSkipChildren();
  } else if (auto AE = dyn_cast<AssignExpr>(E)) {
    {
      llvm::SaveAndRestore<Optional<AccessKind>>
        C(this->OpAccess, AccessKind::Write);

      if (AE->getDest() && !AE->getDest()->walk(*this))
        return doStopTraversal();
    }

    if (AE->getSrc() && !AE->getSrc()->walk(*this))
      return doStopTraversal();

    // We already visited the children.
    return doSkipChildren();
  } else if (auto OEE = dyn_cast<OpenExistentialExpr>(E)) {
    // Record opaque value.
    OpaqueValueMap[OEE->getOpaqueValue()] = OEE->getExistentialValue();
    SWIFT_DEFER {
      OpaqueValueMap.erase(OEE->getOpaqueValue());
    };

    if (!OEE->getSubExpr()->walk(*this))
      return doStopTraversal();

    return doSkipChildren();
  } else if (auto MTEE = dyn_cast<MakeTemporarilyEscapableExpr>(E)) {
    // Manually walk to original arguments in order. We don't handle
    // OpaqueValueExpr here.

    // Original non-escaping closure.
    if (!MTEE->getNonescapingClosureValue()->walk(*this))
      return doStopTraversal();

    // Body, which is called by synthesized CallExpr.
    auto *callExpr = cast<CallExpr>(MTEE->getSubExpr());
    if (!callExpr->getFn()->walk(*this))
      return doStopTraversal();

    return doSkipChildren();
  } else if (auto CUCE = dyn_cast<CollectionUpcastConversionExpr>(E)) {
    // Ignore conversion expressions. We don't handle OpaqueValueExpr here
    // because it's only in conversion expressions. Instead, just walk into
    // sub expression.
    if (!CUCE->getSubExpr()->walk(*this))
      return doStopTraversal();

    return doSkipChildren();
  } else if (auto OVE = dyn_cast<OpaqueValueExpr>(E)) {
    // Walk into mapped value.
    auto value = OpaqueValueMap.find(OVE);
    if (value != OpaqueValueMap.end()) {
      if (!value->second->walk(*this))
        return doStopTraversal();

      return doSkipChildren();
    }
  } else if (auto DMRE = dyn_cast<DynamicMemberRefExpr>(E)) {
    // Visit in source order.
    if (!DMRE->getBase()->walk(*this))
        return doStopTraversal();
    if (!passReference(DMRE->getMember().getDecl(), DMRE->getType(),
                       DMRE->getNameLoc(),
                       ReferenceMetaData(SemaReferenceKind::DynamicMemberRef,
                                         OpAccess)))
        return doStopTraversal();
    // We already visited the children.
    return doSkipChildren();
  }

  return Action::Continue(E);
}

ASTWalker::PostWalkResult<Expr *> SemaAnnotator::walkToExprPost(Expr *E) {
  if (isDone())
    return Action::Stop();

  if (isa<ConstructorRefCallExpr>(E)) {
    assert(CtorRefs.back() == E);
    CtorRefs.pop_back();
  }

  bool Continue = SEWalker.walkToExprPost(E);
  if (!Continue) {
    Cancelled = true;
    return Action::Stop();
  }
  return Action::Continue(E);
}

ASTWalker::PreWalkAction SemaAnnotator::walkToTypeReprPre(TypeRepr *T) {
  if (isDone())
    return Action::Stop();

  bool Continue = SEWalker.walkToTypeReprPre(T);
  if (!Continue) {
    Cancelled = true;
    return Action::Stop();
  }

  if (auto IdT = dyn_cast<ComponentIdentTypeRepr>(T)) {
    if (ValueDecl *VD = IdT->getBoundDecl()) {
      if (auto *ModD = dyn_cast<ModuleDecl>(VD)) {
        auto ident = IdT->getNameRef().getBaseIdentifier();
        auto VisitChildren = passReference(ModD, {ident, IdT->getLoc()});
        return Action::VisitChildrenIf(VisitChildren);
      }
      auto VisitChildren =
          passReference(VD, Type(), IdT->getNameLoc(),
                        ReferenceMetaData(SemaReferenceKind::TypeRef, None));
      return Action::VisitChildrenIf(VisitChildren);
    }
  }

  return Action::Continue();
}

ASTWalker::PostWalkAction SemaAnnotator::walkToTypeReprPost(TypeRepr *T) {
  if (isDone())
    return Action::Stop();

  bool Continue = SEWalker.walkToTypeReprPost(T);
  if (!Continue) {
    Cancelled = true;
    return Action::Stop();
  }
  return Action::Continue();
}

ASTWalker::PreWalkResult<Pattern *>
SemaAnnotator::walkToPatternPre(Pattern *P) {
  if (isDone())
    return Action::Stop();

  if (!SEWalker.walkToPatternPre(P))
    return Action::SkipChildren(P);

  if (P->isImplicit())
    return Action::Continue(P);

  if (auto *EP = dyn_cast<EnumElementPattern>(P)) {
    auto *Element = EP->getElementDecl();
    if (!Element)
      return Action::Continue(P);
    Type T = EP->hasType() ? EP->getType() : Type();
    auto Continue = passReference(
        Element, T, DeclNameLoc(EP->getLoc()),
        ReferenceMetaData(SemaReferenceKind::EnumElementRef, None));
    return Action::VisitChildrenIf(Continue, P);
  }

  auto *TP = dyn_cast<TypedPattern>(P);
  if (!TP || !TP->isPropagatedType())
    return Action::Continue(P);

  // If the typed pattern was propagated from somewhere, just walk the
  // subpattern.  The type will be walked as a part of another TypedPattern.
  TP->getSubPattern()->walk(*this);
  return Action::SkipChildren(P);
}

ASTWalker::PostWalkResult<Pattern *>
SemaAnnotator::walkToPatternPost(Pattern *P) {
  if (isDone())
    return Action::Stop();

  bool Continue = SEWalker.walkToPatternPost(P);
  if (!Continue) {
    Cancelled = true;
    return Action::Stop();
  }
  return Action::Continue(P);
}

bool SemaAnnotator::handleCustomAttributes(Decl *D) {
  // CustomAttrs of non-param VarDecls are handled when this method is called
  // on their containing PatternBindingDecls (see below).
  if (isa<VarDecl>(D) && !isa<ParamDecl>(D))
    return true;

  if (auto *PBD = dyn_cast<PatternBindingDecl>(D)) {
    if (auto *SingleVar = PBD->getSingleVar()) {
      D = SingleVar;
    } else {
      return true;
    }
  }
  for (auto *customAttr : D->getAttrs().getAttributes<CustomAttr, true>()) {
    if (auto *Repr = customAttr->getTypeRepr()) {
      if (!Repr->walk(*this))
        return false;
    }
    if (auto *SemaInit = customAttr->getSemanticInit()) {
      if (!SemaInit->isImplicit()) {
        assert(customAttr->hasArgs());
        if (!SemaInit->walk(*this))
          return false;
        // Don't walk this again via the associated PatternBindingDecl's
        // initializer
        ExprsToSkip.insert(SemaInit);
      }
    } else if (auto *Args = customAttr->getArgs()) {
      if (!Args->walk(*this))
        return false;
    }
  }
  return true;
}

bool SemaAnnotator::handleImports(ImportDecl *Import) {
  auto Mod = Import->getModule();
  if (!Mod)
    return true;

  auto ClangMod = Mod->findUnderlyingClangModule();
  if (ClangMod && ClangMod->isSubModule()) {
    if (!passModulePathElements(Import->getModulePath(), ClangMod))
      return false;
  } else {
    if (!passReference(Mod, Import->getModulePath().front()))
      return false;
  }

  auto Decls = Import->getDecls();
  if (Decls.size() == 1) {
    // FIXME: ImportDecl should store a DeclNameLoc.
    // FIXME: Handle overloaded funcs too by passing a reference for each?
    if (!passReference(Decls.front(), Type(), DeclNameLoc(Import->getEndLoc()),
        ReferenceMetaData(SemaReferenceKind::DeclRef, None)))
      return false;
  }

  return true;
}

bool SemaAnnotator::passModulePathElements(
    ImportPath::Module Path,
    const clang::Module *ClangMod) {

  assert(ClangMod && "can't passModulePathElements of null ClangMod");

  // Visit parent, if any, first.
  if (ClangMod->Parent && Path.hasSubmodule())
    if (!passModulePathElements(Path.getParentPath(), ClangMod->Parent))
      return false;

  return passReference(ClangMod, Path.back());
}

bool SemaAnnotator::passSubscriptReference(ValueDecl *D, SourceLoc Loc,
                                           ReferenceMetaData Data,
                                           bool IsOpenBracket) {
  CharSourceRange Range = Loc.isValid()
                        ? CharSourceRange(Loc, 1)
                        : CharSourceRange();

  bool Continue =
      SEWalker.visitSubscriptReference(D, Range, Data, IsOpenBracket);
  if (!Continue)
    Cancelled = true;
  return Continue;
}

bool SemaAnnotator::passCallAsFunctionReference(ValueDecl *D, SourceLoc Loc,
                                                ReferenceMetaData Data) {
  CharSourceRange Range =
      Loc.isValid() ? CharSourceRange(Loc, 1) : CharSourceRange();

  bool Continue = SEWalker.visitCallAsFunctionReference(D, Range, Data);
  if (!Continue)
    Cancelled = true;
  return Continue;
}

bool SemaAnnotator::
passReference(ValueDecl *D, Type Ty, DeclNameLoc Loc, ReferenceMetaData Data) {
  SourceManager &SM = D->getASTContext().SourceMgr;
  SourceLoc BaseStart = Loc.getBaseNameLoc(), BaseEnd = BaseStart;
  if (BaseStart.isValid() && SM.extractText({BaseStart, 1}) == "`")
    BaseEnd = Lexer::getLocForEndOfToken(SM, BaseStart.getAdvancedLoc(1));
  return passReference(D, Ty, BaseStart, {BaseStart, BaseEnd}, Data);
}

bool SemaAnnotator::
passReference(ValueDecl *D, Type Ty, SourceLoc BaseNameLoc, SourceRange Range,
              ReferenceMetaData Data) {
  TypeDecl *CtorTyRef = nullptr;
  ExtensionDecl *ExtDecl = nullptr;

  if (auto *TD = dyn_cast<TypeDecl>(D)) {
    if (!CtorRefs.empty() && BaseNameLoc.isValid()) {
      Expr *Fn = CtorRefs.back()->getFn();
      if (Fn->getLoc() == BaseNameLoc) {
        D = extractDecl(Fn);
        CtorTyRef = TD;
      }
    }

    if (!ExtDecls.empty() && BaseNameLoc.isValid()) {
      SourceLoc ExtTyLoc = SourceLoc();
      if (auto *repr = ExtDecls.back()->getExtendedTypeRepr())
        ExtTyLoc = repr->getLoc();
      if (ExtTyLoc.isValid() && ExtTyLoc == BaseNameLoc) {
        ExtDecl = ExtDecls.back();
      }
    }
  }

  if (D == nullptr) {
    // FIXME: When does this happen?
    assert(false && "unhandled reference");
    return true;
  }

  CharSourceRange CharRange =
    Lexer::getCharSourceRangeFromSourceRange(D->getASTContext().SourceMgr,
                                             Range);
  bool Continue = SEWalker.visitDeclReference(D, CharRange, CtorTyRef, ExtDecl,
                                              Ty, Data);
  if (!Continue)
    Cancelled = true;
  return Continue;
}

bool SemaAnnotator::passReference(ModuleEntity Mod,
                                  ImportPath::Element IdLoc) {
  if (IdLoc.Loc.isInvalid())
    return true;
  unsigned NameLen = IdLoc.Item.getLength();
  CharSourceRange Range{ IdLoc.Loc, NameLen };
  bool Continue = SEWalker.visitModuleReference(Mod, Range);
  if (!Continue)
    Cancelled = true;
  return Continue;
}

bool SemaAnnotator::passCallArgNames(Expr *Fn, ArgumentList *ArgList) {
  ValueDecl *D = extractDecl(Fn);
  if (!D)
    return true; // continue.

  for (auto Arg : *ArgList) {
    Identifier Name = Arg.getLabel();
    if (Name.empty())
      continue;

    SourceLoc Loc = Arg.getLabelLoc();
    if (Loc.isInvalid())
      continue;

    CharSourceRange Range{ Loc, Name.getLength() };
    bool Continue = SEWalker.visitCallArgName(Name, Range, D);
    if (!Continue) {
      Cancelled = true;
      return false;
    }
  }

  return true;
}

bool SemaAnnotator::shouldIgnore(Decl *D) {
  // TODO: There should really be a separate field controlling whether
  //       constructors are visited or not
  return D->isImplicit() && !isa<ConstructorDecl>(D);
}

bool SourceEntityWalker::walk(SourceFile &SrcFile) {
  SemaAnnotator Annotator(*this);
  return performWalk(Annotator, [&]() { return SrcFile.walk(Annotator); });
}

bool SourceEntityWalker::walk(ModuleDecl &Mod) {
  SemaAnnotator Annotator(*this);
  return performWalk(Annotator, [&]() { return Mod.walk(Annotator); });
}

bool SourceEntityWalker::walk(Stmt *S) {
  SemaAnnotator Annotator(*this);
  return performWalk(Annotator, [&]() { return S->walk(Annotator); });
}

bool SourceEntityWalker::walk(Expr *E) {
  SemaAnnotator Annotator(*this);
  return performWalk(Annotator, [&]() { return E->walk(Annotator); });
}

bool SourceEntityWalker::walk(Pattern *P) {
  SemaAnnotator Annotator(*this);
  return performWalk(Annotator, [&]() { return P->walk(Annotator); });
}

bool SourceEntityWalker::walk(Decl *D) {
  SemaAnnotator Annotator(*this);
  return performWalk(Annotator, [&]() { return D->walk(Annotator); });
}

bool SourceEntityWalker::walk(DeclContext *DC) {
  SemaAnnotator Annotator(*this);
  return performWalk(Annotator, [&]() { return DC->walkContext(Annotator); });
}

bool SourceEntityWalker::walk(ASTNode N) {
  if (auto *E = N.dyn_cast<Expr*>())
    return walk(E);
  if (auto *S = N.dyn_cast<Stmt*>())
    return walk(S);
  if (auto *D = N.dyn_cast<Decl*>())
    return walk(D);

  llvm_unreachable("unsupported AST node");
}

bool SourceEntityWalker::visitDeclReference(ValueDecl *D, CharSourceRange Range,
                                            TypeDecl *CtorTyRef,
                                            ExtensionDecl *ExtTyRef, Type T,
                                            ReferenceMetaData Data) {
  return true;
}

bool SourceEntityWalker::visitSubscriptReference(ValueDecl *D,
                                                 CharSourceRange Range,
                                                 ReferenceMetaData Data,
                                                 bool IsOpenBracket) {
  // Most of the clients treat subscript reference the same way as a
  // regular reference when called on the open bracket and
  // ignore the closing one.
  return IsOpenBracket
             ? visitDeclReference(D, Range, nullptr, nullptr, Type(), Data)
             : true;
}

bool SourceEntityWalker::visitCallAsFunctionReference(ValueDecl *D,
                                                      CharSourceRange Range,
                                                      ReferenceMetaData Data) {
  return true;
}

bool SourceEntityWalker::visitCallArgName(Identifier Name,
                                          CharSourceRange Range,
                                          ValueDecl *D) {
  return true;
}

bool SourceEntityWalker::
visitDeclarationArgumentName(Identifier Name, SourceLoc Start, ValueDecl *D) {
  return true;
}

bool SourceEntityWalker::visitModuleReference(ModuleEntity Mod,
                                              CharSourceRange Range) {
  return true;
}

void SourceEntityWalker::anchor() {}
