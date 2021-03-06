//===-- NvmHeuristics.h -----------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Author: Ian Neal (iangneal@umich.edu)
//
//===----------------------------------------------------------------------===//

#ifndef __NVM_HEURISTICS_H__
#define __NVM_HEURISTICS_H__

#include <stdint.h>
#include <cassert>
#include <cstdarg>
#include <functional>
#include <vector>
#include <iostream>
#include <iomanip>
#include <map>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <list>
#include <string>
#include <deque>
#include <memory>

#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/GlobalVariable.h"

#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Format.h"

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/PostDominators.h"

#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/Internal/Support/ErrorHandling.h"

#include "NvmAnalysisUtils.h"
#include "Memory.h"

#include "AndersenAA.h"

/**
 * This file 
 */

namespace klee {
  /**
   * Forward declarations cuz that's KLEE's whole thing
   */
  class Executor;
  class ExecutionState;
  class NvmDynamicHeuristic;
  class NvmHeuristicBuilder; // Defined at the end

  typedef std::shared_ptr<AndersenAAWrapperPass> SharedAndersen;

  /* #region NvmValueDesc */
  /**
   * This class represents the state of all values at any point during  
   * symbolic execution. If this ever changes on a fork or on the resolution
   * of a mmap call, we must recompute the overall heuristic information for
   * a given execution state.
   * 
   * It is fine for this to initialize to empty, as global variables will be 
   * added as they are modified.
   * 
   * This also has global variables, why not?
   * 
   * ---
   * This is the runtime state.
   */
  class NvmValueDesc : public std::enable_shared_from_this<NvmValueDesc> {
    public:
      typedef std::shared_ptr<NvmValueDesc> Shared;
      typedef std::unordered_map<const llvm::Value*, 
                                 std::unordered_set<const llvm::Value*> > AndersenCache;
      typedef std::shared_ptr<AndersenCache> SharedAndersenCache;
    private:
      /**
       * The shared state.
       */
      SharedAndersen andersen_;

      /**
       * Querying the same value and constructing the set over and over is 
       * expensive. This cache is helpful.
       */
      mutable SharedAndersenCache anders_cache_;

      /**
       * Here we track nvm allocation locations.
       */
      std::unordered_set<const llvm::Value*> nvm_allocs_;

      /**
       * We conservatively assume that any modification site that points to one
       * of the "mmap" calls is an NVM-modifying instruction
       * 
       * We count do global and local to avoid propagating unnecessary local 
       * variables when we go to the next context.
       */
      std::unordered_set<const llvm::Value*> not_local_nvm_, not_global_nvm_;

      /**
       * Each value has a points-to set given by the Andersen alias analysis.
       * This is a helper method to get that set, as it passes through the 
       * cache first.
       * 
       * We also filter down the set to only values which can return NVM.
       */
      bool getPointsToSet(const llvm::Value *v, 
                          std::unordered_set<const llvm::Value *> &ptsSet) const;

      /**
       * Returns true if A may point to B. A may point to B if their points-to
       * sets overlap.
       */
      bool mayPointTo(const llvm::Value *a, const llvm::Value *b) const;

      /**
       * Returns true if the points-to set of A is equivalent to the points-to
       * set of B.
       */
      bool pointsToIsEq(const llvm::Value *a, const llvm::Value *b) const;

      /**
       * Returns true if posNvm (possible NVM value) matches a value that we 
       * know to be a volatile value.
       */
      bool matchesKnownVolatile(const llvm::Value *posNvm) const;

      /**
       * Vararg functions break the current assumption about passing around
       * NVM pointers, as you can't directly track them. So, instead, we figure
       * out if an NVM value was passed as any of the var args. If so, we mark
       * calls to llvm.va_{start,copy,end} as interesting to resolve all values.
       */ 
      // bool varargs_contain_nvm_;

      NvmValueDesc() {}
      NvmValueDesc(SharedAndersen apa, 
                   SharedAndersenCache cache,
                   std::unordered_set<const llvm::Value*> mmap,
                   std::unordered_set<const llvm::Value*> globals) 
                   : andersen_(apa),
                     anders_cache_(cache),
                     nvm_allocs_(mmap), 
                     not_global_nvm_(globals) {}

    public:

      uint64_t hash(void) const {
        return std::hash<uint64_t>{}((not_local_nvm_.size() << 16) | 
                                     (not_global_nvm_.size() << 8) |
                                      nvm_allocs_.size());
      }

      /**
       * Set up the value state when performing a function call.
       * We just return an instance with the global variables and the 
       * propagated state due to the function call arguments.
       */
      NvmValueDesc::Shared doCall(llvm::CallBase *cb, llvm::Function *f) const;

      /** 
       * Set up the value state when doing a return.
       * This essentially just pops the "stack" and propagates the return val.
       */
      NvmValueDesc::Shared doReturn(NvmValueDesc::Shared callerVals,
                                    llvm::ReturnInst *ret, 
                                    llvm::Instruction *dest) const;

      /**
       * Directly create a new description. This is generally for when we actually
       * execute and want to update our assumptions.
       * 
       * If the state is updated, returns a new shared pointer. Else, returns
       * shared_from_this.
       */
      NvmValueDesc::Shared updateState(llvm::Value *val, bool nvm);

      /**
       * When we do an indirect function call, we can't propagate local nvm variables
       * cuz we don't know the arguments yet. This lets us do that.
       */
      NvmValueDesc::Shared resolveFunctionPointer(llvm::Function *f);

      bool isNvmAllocCall(const llvm::CallBase *cb) const {
        return !!nvm_allocs_.count(cb);
      }

      /**
       * It is possible for a function to have var args, with one of these 
       * arguments being a pointer which points to NVM. Example: snprintf to
       * NVM.
       * 
       * We need to mark certain va_arg instructions as important to resolve.
       * These instructions will be the ones that convert a var arg into a 
       * pointer value---scalars do not matter to us.
       * 
       * Note that va_arg (http://llvm.org/docs/LangRef.html#i-va-arg) is not 
       * supported on many targets, in that case we will look for a getelementptr
       * and subsequent load.
       */
      bool isImportantVAArg(llvm::Instruction *i) const {
        // if (!varargs_contain_nvm_) return false;

        // if (llvm::isa<llvm::VAArgInst>(i) && 
        //     i->getType()->isPtrOrPtrVectorTy()) return true;
        // // TODO: The getelementptr case is more annoying
        // // if (llvm::LoadInstruction *li = llvm::dyn_cast<llvm::LoadInst>(i)) {
        // //   if (llvm::GetElementPtrInst *gi = llvm::dyn_cast<llvm::GetElementPtrInst>(li->getPointerOperand())) {
        // //     if (gi->getPointerOperandType())
        // //   }
        // // }

        // // Conservative answer:
        // // By catching vaend, we will get all va_arg calls.
        // if (llvm::CallInst *ci = llvm::dyn_cast<llvm::CallInst>(i)) {
        //   llvm::Function *f = ci->getCalledFunction();
        //   return (f && f->isDeclaration() &&
        //           (f->getIntrinsicID() == llvm::Intrinsic::vastart ||
        //            f->getIntrinsicID() == llvm::Intrinsic::vacopy ||
        //            f->getIntrinsicID() == llvm::Intrinsic::vaend));
        // }
        
        return false;
      }

      /**
       * The "points-to" set points to allocation sites.
       */
      bool isNvm(const llvm::Value *ptr) const;

      /**
       * If this instructions stores or flushes NVM, return true, else false.
       */
      bool mayModifyNvm(const llvm::Instruction *i) const;

      std::string str(void) const;

      // Populate with all the calls to mmap.
      static NvmValueDesc::Shared staticState(SharedAndersen andersen, 
                                              llvm::Module *m);

      friend bool operator==(const NvmValueDesc &lhs, const NvmValueDesc &rhs);
  };
  /* #endregion */

  /* #region NvmContextDesc */

  class NvmContextDesc : public std::enable_shared_from_this<NvmContextDesc> {
    public: 
      typedef std::shared_ptr<NvmContextDesc> Shared;
    private:
      friend class NvmDynamicHeuristic;
      SharedAndersen andersen;

      /**
       * These are the core pieces that define a context.
       */
      llvm::Function *function;
      NvmValueDesc::Shared valueState;
      bool returnHasWeight; 

      /**
       * Many function contexts will be the same. Particularly, when we do 
       * updates, we don't want to recompute the priority every time, as they
       * will likely have common themes. So, we will cache more than just the
       * initial state.
       */
      struct ContextCacheKey {
        llvm::Function *function;
        NvmValueDesc::Shared valueState;

        ContextCacheKey(llvm::Function *f, NvmValueDesc::Shared vals) 
          : function(f), valueState(vals) {}

        bool operator==(const ContextCacheKey &rhs) const {
          return function == rhs.function && *valueState == *rhs.valueState;
        }

        struct Hash {
          uint64_t operator()(const ContextCacheKey &cck) const {
            return std::hash<void*>{}(cck.function) ^ cck.valueState->hash();
          }
        };
      };

      static std::unordered_map<ContextCacheKey, 
                                NvmContextDesc::Shared,
                                ContextCacheKey::Hash> contextCache;

      /**
       * This function has a bunch of instructions. They have weights based
       * on the current context.
       */
      std::unordered_map<llvm::Instruction*, uint64_t> weights;

      bool hasCoreWeight = false;

      /**
       * This functions's instructions also have a bunch of priorities.
       */
      std::unordered_map<llvm::Instruction*, uint64_t> priorities;

      /**
       * CallInsts have succeeding ContextDesc, which is nice to pre-compute
       */
      std::unordered_map<llvm::CallBase*, NvmContextDesc::Shared> contexts;

      /* METHODS */

      /**
       * Generally used for generating contexts for calls.
       */
      NvmContextDesc(SharedAndersen anders,
                     llvm::Function *fn,
                     NvmValueDesc::Shared initialArgs,
                     bool parentHasWeight);

      /**
       * Returns the priority of the subcontext.
       */
      uint64_t constructCalledContext(llvm::CallBase *cb, llvm::Function *f);
      uint64_t constructCalledContext(llvm::CallBase *cb);

      /**
       * Core instructions are instructions that impact NVM
       */
      bool isaCoreInst(llvm::Instruction *i) const;

      /**
       * Auxiliary instructions are instructions that have weight as a 
       * consequency of control flow.
       * 
       * For this version of the heuristic, this will just be call and return
       * instructions.
       */
      bool isaAuxInst(llvm::Instruction *i) const;
      uint64_t computeAuxInstWeight(llvm::Instruction *i);

      /**
       * After this, the context should be fully valid.
       */
      std::list<llvm::Instruction*> setCoreWeights(void);
      void setAuxWeights(std::list<llvm::Instruction*> auxInsts);
      void setPriorities(void);

    public:

      /**
       * Constructs the first context, generally for whatever function KLEE is 
       * using for a main function.
       */
      NvmContextDesc(SharedAndersen anders, llvm::Module *m, llvm::Function *main);

      /**
       * Gets the next context if the given PC is a call or return instruction.
       * Otherwise, returns this.
       */
      NvmContextDesc::Shared tryGetNextContext(KInstruction *pc, 
                                               KInstruction *nextPC);

      /**
       * Gets the resulting context of updating the state. If updating the state
       * does not cause any change in priority, returns this.
       */
      NvmContextDesc::Shared tryUpdateContext(llvm::Value *v, bool isNvm);

      NvmContextDesc::Shared tryResolveFnPtr(llvm::CallBase *cb, 
                                             llvm::Function *f);

      /**
       * Gets the priority at the root of the function, i.e. at the first 
       * instruction.
       */
      uint64_t getRootPriority(void) const {
        llvm::Instruction *i = function->getEntryBlock().getFirstNonPHIOrDbg();
        if (priorities.count(i)) return priorities.at(i);
        
        return hasCoreWeight ? 1lu : 0lu;
      }

      uint64_t getPriority(KInstruction *pc) const {
        return priorities.at(pc->inst);
      }

      NvmContextDesc::Shared dup(void) const {
        return std::make_shared<NvmContextDesc>(*this);
      }

      std::string str() const;
  };

  /* #endregion */


  /* #region NvmHeuristicInfo (super class) */
  /**
   * This is per state.
   */

  class NvmHeuristicInfo {
    friend class NvmHeuristicBuilder;
    protected:

      typedef std::unordered_set<const llvm::Value*> ValueSet;
      typedef std::vector<const llvm::Value*> ValueVector;
      
      virtual void computePriority() = 0;

      virtual bool needsRecomputation() const = 0;

    public:

      typedef std::shared_ptr<NvmHeuristicInfo> Shared;
    
      virtual ~NvmHeuristicInfo() = 0;

      virtual uint64_t getCurrentPriority(void) const = 0;

      /**
       * May change one of the current states, or could not.
       */
      virtual void updateCurrentState(ExecutionState *es, KInstruction *pc, bool isNvm) = 0;

      /**
       * Resolve a function call. Useful for function pointer shenanigans.
       */
      virtual void resolveFunctionCall(KInstruction *pc, llvm::Function *f) {}

      /**
       * Advance the current state, if we can.
       * 
       * It's fine if the current PC was a jump, branch, etc. We already computed
       * the possible successor states for ourself (without symbolic values of course).
       * If we did our job correctly, this should work fine. Otherwise, we error.
       * 
       * The only case we currently don't handle well is interprocedurally 
       * generated function pointers. We will resolve them at runtime.
       * 
       * In stepState, we also want check if we modified any persistent state.
       * 
       * We need the current PC to resolve when we execute one of our possible
       * states. We need the next PC to resolve function pointers.
       */
      virtual void stepState(ExecutionState *es, KInstruction *pc, KInstruction *nextPC) = 0;

      virtual void dump(void) const = 0;
  };
  /* #endregion */

  /* #region NvmStaticHeuristic */
  class NvmStaticHeuristic : public NvmHeuristicInfo {
    friend class NvmHeuristicBuilder;
    protected:
      typedef std::unordered_map<llvm::Instruction*, uint64_t> WeightMap;
      typedef std::shared_ptr<WeightMap> SharedWeightMap;

      Executor *executor_;
      SharedAndersen analysis_; // Andersen's whole program pointer analysis

      SharedWeightMap weights_;
      SharedWeightMap priorities_;

      void resetWeights(void) {
        weights_ = std::make_shared<WeightMap>();
        priorities_ = std::make_shared<WeightMap>();
      }

      llvm::Instruction *curr_;

      llvm::Module *module_;
      ValueSet nvmSites_;

      NvmValueDesc::Shared valueState_;

      NvmStaticHeuristic(Executor *executor, KFunction *mainFn);

      bool isNvmAllocSite(llvm::Instruction *i) const {
        // return nvmSites_.count(i);
        return valueState_->isNvmAllocCall(dyn_cast<llvm::CallBase>(i));
      }

      /**
       * Returns true if the given instruction is important.
       * 
       * Return instructions are important because they force us to leave 
       * functions and eventually terminate the program. It also makes it 
       * easier to calculate the heuristic.
       * 
       * It's okay to do it this way, because the NvmPathSearcher prioritizes
       * changes in priority, rather than just the priority number. So if you
       * have a big long function with no modifications to NVM, the path searcher
       * will just explore one path toward the return statement.
       */
      virtual bool mayHaveWeight(llvm::Instruction *i) const {
        return isa<llvm::StoreInst>(i) || 
               isa<llvm::ReturnInst>(i) ||
               utils::isFlush(i) || 
               utils::isFence(i) || 
               isNvmAllocSite(i);
      }

      virtual const ValueSet &getCurrentNvmSites() const { return nvmSites_; }

      /**
       * Calculate what the weight of this instruction would be.
       */
      virtual uint64_t computeInstWeight(llvm::Instruction *i) const;

      virtual void computePriority() override;

      virtual bool needsRecomputation() const override { return false; }

    public:

      NvmStaticHeuristic(const NvmStaticHeuristic &other) = default;

      virtual ~NvmStaticHeuristic() {}

      virtual uint64_t getCurrentPriority(void) const override {
        uint64_t priority = priorities_->count(curr_) ? priorities_->at(curr_) : 0lu;
        return priority;
      };

      /**
       * May change one of the current states, or could not.
       */
      virtual void updateCurrentState(ExecutionState *es, 
                                      KInstruction *pc, 
                                      bool isNvm) override {}

      /**
       * Advance the current state, if we can.
       * 
       * It's fine if the current PC was a jump, branch, etc. We already computed
       * the possible successor states for ourself (without symbolic values of course).
       * If we did our job correctly, this should work fine. Otherwise, we error.
       * 
       * The only case we currently don't handle well is interprocedurally 
       * generated function pointers. We will resolve them at runtime.
       * 
       * In stepState, we also want check if we modified any persistent state.
       * 
       * We need the current PC to resolve when we execute one of our possible
       * states. We need the next PC to resolve function pointers.
       */
      virtual void stepState(ExecutionState *es, 
                             KInstruction *pc, 
                             KInstruction *nextPC) override {
        curr_ = nextPC->inst;
      }

      virtual void dump(void) const override;
  };

  /* #endregion */

  /* #region NvmDynamicHeuristic */

  /**
   * We keep call stack information with our values, but not flow information.
   * We forgo a flow-sensitive analysis as it is extremely costly.
   */
  class NvmDynamicHeuristic : public NvmHeuristicInfo {
    friend class NvmHeuristicBuilder;
    protected:
      
      std::list<NvmContextDesc::Shared> contextStack;
      std::list<llvm::CallBase*> callInstStack; 
      NvmContextDesc::Shared contextDesc;
      KInstruction *curr;

      /**
       * We can map a function and it's context to a priority value. This makes
       * re-computing priority fairly efficient.
       */

      NvmDynamicHeuristic(Executor *executor, KFunction *mainFn);

      virtual void computePriority() override {
        contextDesc->setAuxWeights(contextDesc->setCoreWeights());
        contextDesc->setPriorities();
      }

      virtual bool needsRecomputation() const override { return false; }

    public:

      NvmDynamicHeuristic(const NvmDynamicHeuristic &other) = default;

      virtual ~NvmDynamicHeuristic() {}

      virtual uint64_t getCurrentPriority(void) const override {
        return contextDesc->getPriority(curr);
      }

      /**
       * May change one of the current states, or could not.
       */
      virtual void updateCurrentState(ExecutionState *es, 
                                      KInstruction *pc, 
                                      bool isNvm) override;

      virtual void stepState(ExecutionState *es, 
                             KInstruction *pc, 
                             KInstruction *nextPC) override;

      virtual void dump(void) const override {
        uint64_t nonZeroWeights = 0, nonZeroPriorities = 0;

        for (const auto &p : contextDesc->weights) 
          nonZeroWeights += (p.second > 0);
        for (const auto &p : contextDesc->priorities) 
          nonZeroPriorities += (p.second > 0);

        double pWeights = 100.0 * ((double)nonZeroWeights / (double)contextDesc->weights.size());
        double pPriorities = 100.0 * ((double)nonZeroPriorities / (double)contextDesc->priorities.size());

        llvm::errs() << "NvmContext: \n" << contextDesc->str() << "\n";
        llvm::errs() << "\tCurrent instruction: " << *curr->inst << "\n"; 
        fprintf(stderr, "\t%% insts with weight: %f%%\n", pWeights);
        fprintf(stderr, "\t%% insts with priority: %f%%\n", pPriorities);
      }
  };
  /* #endregion */

  /* #region NvmHeuristicBuilder */
  class NvmHeuristicBuilder {
    private:
      static const char *typeNames[];

      static const char *typeDesc[];

    public:
      NvmHeuristicBuilder() = delete;

      enum Type {
        None = 0,
        Static,
        Dynamic,
        Invalid
      };

      static const char *stringify(Type t) {
        return typeNames[t];
      }

      static const char *explanation(Type t) {
        return typeDesc[t];
      }

      static Type toType(const char *tStr) {
        for (int t = None; t < Invalid; ++t) {
          if (tStr == typeNames[t]) {
            return (Type)t;
          }
        }
        return Invalid;
      }

      static std::shared_ptr<NvmHeuristicInfo> 
      create(Type t, Executor *executor, KFunction *main);

      static std::shared_ptr<NvmHeuristicInfo> 
      copy(const std::shared_ptr<NvmHeuristicInfo> &info);
  };
  /* #endregion */

}
#endif //__NVM_HEURISTICS_H__
/*
 * vim: set tabstop=2 softtabstop=2 shiftwidth=2:
 * vim: set filetype=cpp:
 */
