#include "../interface/TTreeFormulaCached.h"

#include "TError.h"
#include "TCutG.h"
#include "TTree.h"
#include "TNamed.h"

ClassImp(TTreeFormulaCached)

TTreeFormulaCached::TTreeFormulaCached(char const* _name, char const* _formula, TTree* _tree, CachePtr const& _cache) :
  TTreeFormula(_name, _formula, _tree),
  fCache(_cache)
{
  if (!fCache)
    fCache.reset(new Cache);

  // replace subformulas with TTreeFormulaCached
  for (Int_t j=0; j<kMAXCODES; j++) {
    if (fLookupType[j]==kDataMember || fLookupType[j]==kTreeMember)
      throw std::runtime_error("Parsing of object branches not supported yet");
    
    for (Int_t k = 0; k<kMAXFORMDIM; k++) {
      if (fVarIndexes[j][k]) {
        auto* tmp{fVarIndexes[j][k]};
        fVarIndexes[j][k] = new TTreeFormulaCached(tmp->GetName(), tmp->GetTitle(), tmp->GetTree());
        delete tmp;
      }
    }

    if (j<fNval && fCodes[j]<0) {
      if (fExternalCuts.At(j)) {
        auto* gcut{static_cast<TCutG*>(fExternalCuts.At(j))};
        if (gcut->GetObjectX()) {
          auto* fx{static_cast<TTreeFormula*>(gcut->GetObjectX())};
          gcut->SetObjectX(new TTreeFormulaCached(fx->GetName(), fx->GetTitle(), fx->GetTree()));
        }
        if (gcut->GetObjectY()) {
          auto* fy{static_cast<TTreeFormula*>(gcut->GetObjectY())};
          gcut->SetObjectY(new TTreeFormulaCached(fy->GetName(), fy->GetTitle(), fy->GetTree()));
        }
      }
    }
  }

  for(Int_t k=0;k<fNoper;k++) {
    if (k >= fAliases.GetEntries())
      break;
    if (fAliases[k]) {
      auto* subform{static_cast<TTreeFormula*>(fAliases.UncheckedAt(k))};
      fAliases[k] = new TTreeFormulaCached(subform->GetName(), subform->GetTitle(), subform->GetTree());
      delete subform;
    }
  }
}

TTreeFormulaCached::TTreeFormulaCached(char const* _name, char const* _formula, TTree* _tree) :
  TTreeFormula(_name, _formula, _tree)
{
}

Int_t
TTreeFormulaCached::GetNdata()
{
  Int_t ndata(TTreeFormula::GetNdata());

  if (fCache && fCache->fValues.empty())
    fCache->fValues.assign(ndata, std::pair<Bool_t, Double_t>(false, 0.));

  return ndata;
}

Double_t
TTreeFormulaCached::EvalInstance(Int_t _i, char const* _stringStack[]/* = nullptr*/)
{
  if (fCache) {
    if (_i >= int(fCache->fValues.size())) {
      if (fCache->fValues.size() == 0)
        return 0.;
      else
        return EvalInstance(fCache->fValues.size() - 1, _stringStack);
    }

    if (!fCache->fValues[_i].first) {
      fCache->fValues[_i].first = true;
      fCache->fValues[_i].second = TTreeFormula::EvalInstance(_i, _stringStack);
    }

    return fCache->fValues[_i].second;
  }
  else
    return TTreeFormula::EvalInstance(_i, _stringStack);
}

bool
TTreeFormulaCached::ReplaceLeaf(TString const& _from, TString const& _to)
{
  bool replaced{false};
  
  Int_t nleaves = fLeafNames.GetEntriesFast();
  ResetBit( kMissingLeaf );
  for (Int_t i=0;i<nleaves;i++) {
    if (!fTree) break;
    if (!fLeafNames[i]) continue;

    if (fLeafNames[i]->GetName() != _from)
      continue;

    replaced = true;

    delete fLeafNames[i];
    // leaf "name" = alias.leaf, leaf "title" = bare leaf name
    fLeafNames[i] = new TNamed(_to, _to(_to.Index(".") + 1, _to.Length()));

    TLeaf *leaf = fTree->GetLeaf(fLeafNames[i]->GetTitle(),fLeafNames[i]->GetName());
    fLeaves[i] = leaf;

    if (fBranches[i] && leaf) {
      fBranches[i] = leaf->GetBranch();
      ((TBranch*)fBranches[i])->ResetReadEntry();
    }
    if (leaf==0) SetBit( kMissingLeaf );
  }

  for (Int_t j=0; j<kMAXCODES; j++) {
    for (Int_t k = 0; k<kMAXFORMDIM; k++) {
      if (fVarIndexes[j][k]) {
        replaced = replaced || static_cast<TTreeFormulaCached*>(fVarIndexes[j][k])->ReplaceLeaf(_from, _to);
      }
    }
    if (j<fNval && fCodes[j]<0) {
      TCutG *gcut = (TCutG*)fExternalCuts.At(j);
      if (gcut) {
        auto* fx = static_cast<TTreeFormulaCached*>(gcut->GetObjectX());
        auto* fy = static_cast<TTreeFormulaCached*>(gcut->GetObjectY());
        if (fx) {
          replaced = replaced || fx->ReplaceLeaf(_from, _to);
        }
        if (fy) {
          replaced = replaced || fy->ReplaceLeaf(_from, _to);
        }
      }
    }
  }
  for(Int_t k=0;k<fNoper;k++) {
    const Int_t oper = GetOper()[k];
    switch(oper >> kTFOperShift) {
    case kAlias:
    case kAliasString:
    case kAlternate:
    case kAlternateString:
    case kMinIf:
    case kMaxIf:
      {
        auto* subform = static_cast<TTreeFormulaCached*>(fAliases.UncheckedAt(k));
        replaced = replaced || subform->ReplaceLeaf(_from, _to);
        break;
      }
    case kDefinedVariable:
      {
        Int_t code = GetActionParam(k);
        if (fCodes[code]==0) switch(fLookupType[code]) {
          case kLengthFunc:
          case kSum:
          case kMin:
          case kMax:
            {
              auto* subform = static_cast<TTreeFormulaCached*>(fAliases.UncheckedAt(k));
              replaced = replaced || subform->ReplaceLeaf(_from, _to);
              break;
            }
          default:
            break;
          }
      }
    default:
      break;
    }
  }

  return replaced;
}

struct ErrorHandlerReport {
  thread_local static Int_t lastErrorLevel;
  static void errorHandler(Int_t _level, Bool_t _abort, char const* _location, char const* _msg) {
    lastErrorLevel = _level;
    DefaultErrorHandler(_level, _abort, _location, _msg);
  }
};

thread_local Int_t ErrorHandlerReport::lastErrorLevel{0};

TTreeFormula*
NewTTreeFormula(char const* _name, char const* _expr, TTree* _tree, bool _silent/* = false*/)
{
  auto* originalErrh(SetErrorHandler(ErrorHandlerReport::errorHandler));
  auto originalIgnoreLevel(gErrorIgnoreLevel);
  if (_silent)
    gErrorIgnoreLevel = kFatal;

  auto* formula(new TTreeFormula(_name, _expr, _tree));

  if (_silent)
    gErrorIgnoreLevel = originalIgnoreLevel;

  SetErrorHandler(originalErrh);

  if (formula->GetTree() == nullptr || ErrorHandlerReport::lastErrorLevel == kError) {
    // compilation failed
    delete formula;
    return nullptr;
  }

  ErrorHandlerReport::lastErrorLevel = 0;

  return formula;
}

//! A wrapper for TTreeFormulaCached creation
TTreeFormulaCached*
NewTTreeFormulaCached(char const* _name, char const* _expr, TTree* _tree, TTreeFormulaCached::CachePtr const& _cache, bool _silent/* = false*/)
{
  auto* originalErrh(SetErrorHandler(ErrorHandlerReport::errorHandler));
  auto originalIgnoreLevel(gErrorIgnoreLevel);
  if (_silent)
    gErrorIgnoreLevel = kFatal;

  auto* formula(new TTreeFormulaCached(_name, _expr, _tree, _cache));

  SetErrorHandler(originalErrh);

  if (_silent)
    gErrorIgnoreLevel = originalIgnoreLevel;

  if (formula->GetTree() == nullptr || ErrorHandlerReport::lastErrorLevel == kError) {
    // compilation failed
    delete formula;
    return nullptr;
  }

  ErrorHandlerReport::lastErrorLevel = 0;

  return formula;
}
