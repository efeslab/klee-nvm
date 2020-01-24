#include "nvm_function_info.hpp"

namespace utils {
    FunctionInfo::FunctionInfo(ModulePass &mp, const Module &mod)
        : mp_(mp), mod_(mod)
    {
        initNvmDeclarations();
        initManip();
    }

    Value* FunctionInfo::getNvmPtrLoc(const Instruction &i) {
        if (!utils::checkInstrinicInst(i, "annotation", nullptr)) {
            return nullptr;
        }

        Value* annotation = i.getOperand(1);
        Value *ann = annotation->stripPointerCasts();
        auto *ex = ValueAsMetadata::getConstant(ann);

        auto *g = dyn_cast<GlobalVariable>(ex->getValue());
        if (g) {
            auto *r = dyn_cast<ConstantDataSequential>(g->getInitializer());
            if (r) {
                auto name = r->getAsCString();
                if ("nvmptr" == name) {
                    return getPtrLoc(i.getOperand(0));
                }
            }
        }

        return nullptr;
    }

    void FunctionInfo::getNvmPtrLocs(const Function &f, unordered_set<const Value*> &s) {
        for (auto &b : f) {
            for (auto &i : b) {
                Value *v = getNvmPtrLoc(i);
                if (v) s.insert(v);
            }
        }
    }


    void FunctionInfo::getNvmPtrsFromLocs(const Function &f, unordered_set<const Value*> &s) {
        if (nvm_locs[f.getName()].empty()) return;

        for (auto *v : nvm_locs[f.getName()]) {
            for (auto *u : v->users()) {
                // if is assignment
                if (isa<LoadInst>(u)) {
                    s.insert(u);
                }
            }
        }

        utils::getDerivativePtrs(s);
    }

    void FunctionInfo::getNvmModifiers(const Function &f, unordered_set<const Value*> &s) {
        if (nvm_ptrs.find(f.getName()) == nvm_ptrs.end()) return;

        for (auto *v : nvm_ptrs[f.getName()]) {
            utils::getModifiers(v, s);
        }
    }

    vector<unordered_set<const Value*>> FunctionInfo::getArgumentManip(const Function &fn) {
        vector<unordered_set<const Value*>> ret(fn.arg_size());

        for (const Argument &arg : fn.args()) {
            // Now we can simply find all the users of this arg and all derivative
            // users of the argument as well.
            unordered_set<const Value*> ptrs;
            ptrs.insert(&arg);
            utils::getDerivativePtrs(ptrs);

            for (const auto *ptr : ptrs) {
                utils::getModifiers(ptr, ret[arg.getArgNo()]);
            }
        }

        return ret;
    }

    void FunctionInfo::initNvmDeclarations() {
        for (auto &f : mod_) {
            auto s = unordered_set<const Value*>();
            getNvmPtrLocs(f, s);
            if (!s.empty()) {
                nvm_locs[f.getName()] = s;
            }
        }

        // Get all the initial values from the pointer locations
        // Then, find all the derivative pointers, like offsets, etc.
        // --- Basically, just not loads and stores and the like.
        for (auto &f : mod_) {
            getNvmPtrsFromLocs(f, nvm_ptrs[f.getName()]);
            if (nvm_ptrs[f.getName()].empty()) {
                nvm_ptrs.erase(f.getName());
            }
        }

    }

    void FunctionInfo::initManip() {
        // Now find everything which modifies NVM
        for (auto &f : mod_) {
            getNvmModifiers(f, nvm_usrs[f.getName()]);
            if (nvm_usrs[f.getName()].empty()) {
                nvm_usrs.erase(f.getName());
            }
            nvm_arg_manip[f.getName()] = getArgumentManip(f);
        }
    }

    bool FunctionInfo::manipulatesNVM(const Function *fn, unordered_set<int> nvmArgs) {
        const auto &fname = fn->getName();
        tuple<string, unordered_set<int>> key(fname, nvmArgs);

        // Step 0: Check if we've done this calculation before. If so, return the
        // results.
        if (manip.find(key) != manip.end()) {
            return manip[key];
        }

        unordered_set<const Value*> nvmVals;
        for (const int &i : nvmArgs) {
            nvmVals.insert(fn->arg_begin() + i);
        }

        if (nvm_ptrs.find(fname) != nvm_ptrs.end()) {
            nvmVals.insert(nvm_ptrs[fname].begin(), nvm_ptrs[fname].end());
        }

        errs() << "! Checking function " << fn->getName() << "\n";

        bool fn_manip_nvm = false;

        for (const auto &bb : *fn) {
            for (const auto &i : bb) {
                // Step 1: Check if we have an sfence. If so, immediately know that
                // this function manipulates NVM.
                if (utils::isFence(i)) {
                    fn_manip_nvm = true;
                }

                // Step 2: Check if we manipulate any NVM pointers directly in this
                // function.
                if (const User *u = dyn_cast<User>(&i)) {
                    errs() << "user: " << *u << "\n";
                    // Step 2a: Check if it's a direct declaration usage.
                    if (nvm_usrs.find(fname) != nvm_usrs.end()) {
                        if (nvm_usrs[fname].find(u) != nvm_usrs[fname].end()) {
                            fn_manip_nvm = true;
                            errs() << "\tequals  " << *nvm_usrs[fname].find(u) << "\n";
                        } else {
                            for (const auto *u : nvm_usrs[fname]) {
                                errs() << "\t DOES NOT equal " << *u << "\n";
                            }
                        }
                    }
                    // Step 2b: Check if it's a argument usage.
                    if (nvm_arg_manip.find(fname) != nvm_arg_manip.end()) {
                        for (const int &argno : nvmArgs) {
                            const auto &x = nvm_arg_manip[fname][argno];
                            if (x.find(u) != x.end()) {
                                fn_manip_nvm = true;
                                errs() << "\tmanip!\n";
                            }
                        }
                    }
                }

                // Step 3: Check if this is a function call. If it is, set up the
                // arg set and check if it manipulatesNVM.
                const CallInst *ci = dyn_cast<CallInst>(&i);
                if (nullptr != ci && !ci->isInlineAsm() &&
                    !isa<IntrinsicInst>(&i) && !isa<InlineAsm>(&i)) {
                    const Function *cfn = ci->getCalledFunction();
                    if (nullptr != cfn) {
                        vector<int> fn_args;
                        for (const Argument &arg : cfn->args()) {
                            fn_args.push_back(arg.getArgNo());
                        }

                        unordered_set<int> cargs;
                        int n = 0;
                        for (const Use &use : ci->args()) {
                            if (nvmVals.find(use.get()) != nvmVals.end()) {
                                cargs.insert(fn_args[n]);
                            }
                            ++n;
                        }

                        if (manipulatesNVM(ci->getCalledFunction(), cargs)) {
                            fn_manip_nvm = true;
                        }
                    } else {
                        errs() << "Called function is null!\n";
                        errs() << "---> " << *ci << "\n";
                    }
                }
            }
        }

        manip[key] = fn_manip_nvm;

        return fn_manip_nvm;
    }

    bool FunctionInfo::manipulatesNVM(const Function *fn) {
        bool does_manip = false;
        for (const Argument &arg : fn->args()) {
            unordered_set<int> args;
            args.insert(arg.getArgNo());
            does_manip |= manipulatesNVM(fn, args);
        }

        return does_manip;
    }

    void FunctionInfo::dumpManip(const Function *fn) {
        for (const auto &t : manip) {
            const auto &key = get<0>(t);
            const bool m = get<1>(t);
            if (get<0>(key) != fn->getName()) continue;

            const char *msg = m ? " manipulates " : " does not manipulate ";
            errs() << fn->getName() << msg << "for NVM args ";
            for (const int argno : get<1>(key)) {
                errs() << argno << ", ";
            }
            errs() << ".\n";
        }
    }

    size_t FunctionInfo::totalPathsInFunction(const Function *fn) {
        const auto &fname = fn->getName();
        if (paths_total.find(fname) != paths_total.end()) {
            return paths_total[fname];
        }

        if (fn->isIntrinsic()) {
            paths_total[fname] = 1;
            return paths_total[fname];
        }

        DominatorTree &dom =
            mp_.getAnalysis<DominatorTreeWrapperPass>(
                    *const_cast<Function*>(fn)).getDomTree();

        size_t paths = 0;

        queue<const BasicBlock*> frontier;
        frontier.push(&fn->getEntryBlock());
        unordered_map<const BasicBlock*, unordered_set<const BasicBlock*>> backedges;

        while (!frontier.empty()) {
            const BasicBlock *bb = frontier.front();

            size_t nsucc = 0;

            for (const BasicBlock *succ : successors(bb)) {
                nsucc++;
                if (dom.dominates(succ, bb)) {
                    if (backedges[bb].find(succ) == backedges[bb].end()) {
                        backedges[bb].insert(succ);
                    } else {
                        continue;
                    }
                }

                frontier.push(succ);
            }

            if (!nsucc) {
                ++paths;
            }

            frontier.pop();
        }

        paths_total[fname] = paths;

        return paths;
    }

    size_t FunctionInfo::totalPathsThroughFunction(const Function *fn) {
        const auto &fname = fn->getName();
        if (paths_total_rec.find(fname) != paths_total_rec.end()) {
            return paths_total_rec[fname];
        }

        if (fn->isIntrinsic()) {
            paths_total_rec[fname] = 1;
            return paths_total_rec[fname];
        }

        DominatorTree &dom =
            mp_.getAnalysis<DominatorTreeWrapperPass>(
                    *const_cast<Function*>(fn)).getDomTree();

        size_t paths = 0;

        typedef tuple<const BasicBlock*, size_t> fnode_t;
        queue<fnode_t> frontier;
        frontier.emplace(&fn->getEntryBlock(), 1);
        unordered_map<const BasicBlock*, unordered_set<const BasicBlock*>> backedges;

        while (!frontier.empty()) {
            const BasicBlock *bb = get<0>(frontier.front());
            size_t path_count = get<1>(frontier.front());

            for (const Function *nfn : utils::getNestedFunctionCalls(bb)) {
                path_count *= totalPathsThroughFunction(nfn);
            }

            size_t nsucc = 0;

            for (const BasicBlock *succ : successors(bb)) {
                nsucc++;
                if (dom.dominates(succ, bb)) {
                    if (backedges[bb].find(succ) == backedges[bb].end()) {
                        backedges[bb].insert(succ);
                    } else {
                        continue;
                    }
                }

                frontier.emplace(succ, path_count);
            }

            if (!nsucc) {
                paths += path_count;
            }

            frontier.pop();
        }

        paths_total_rec[fname] = paths;

        return paths;
    }

    void FunctionInfo::dumpPathsThrough() {
        for (const auto &t : paths_total_rec) {
            errs() << "Number of paths through " << get<0>(t)
                << format(" = %lu\n", get<1>(t));
        }
    }

    void FunctionInfo::findImportantOps(const Function *fn, const unordered_set<int> &args){
        /*
        if (nvm_usrs.find(fn->getName()) == nvm_usrs.end()) {
            errs() << "Need to initialize this!! " << fn->getName() << "\n";
            exit(-1);
        }
        */
        unordered_set<const Value*> local_nvm_ptrs = nvm_ptrs[fn->getName()];
        unordered_set<const Value*> local_nvm_usrs = nvm_usrs[fn->getName()];

        // Now, find all the users of the input arguments.
        for (int argno : args) {
            const Argument *arg = fn->arg_begin() + argno;
            unordered_set<const Value*> argDerivatives({arg});
            utils::getDerivativePtrs(argDerivatives);
            local_nvm_ptrs.insert(argDerivatives.begin(), argDerivatives.end());

            unordered_set<const Value*> argMod;
            for (const Value *val : argDerivatives) {
                utils::getModifiers(val, argMod);
            }

            local_nvm_usrs.insert(argMod.begin(), argMod.end());
        }

        // Now that we have all the modifiers, we check per basic block.
        for (const BasicBlock &bb : *fn) {
            bkey_t key(&bb, args);
            if (imp_factor.find(key) != imp_factor.end()) {
                errs() << fn->getName() << ", recovered BasicBlock: " << bb << "\n";
                errs() << format("\t%lu\n", imp_factor[key]);
                continue;
            }

            size_t nImp = 0;

            for (const Instruction &i : bb) {
                if (local_nvm_usrs.find(&i) != local_nvm_usrs.end() ||
                    utils::isFence(i))
                {
                    nImp++;
                }

                const CallInst *ci = dyn_cast<CallInst>(&i);
                if (ci && !ci->isInlineAsm()) {
                    const Function *cfn = ci->getCalledFunction();
                    if (cfn && !cfn->isIntrinsic()) {
                        unordered_set<int> calledArgs;
                        for (int i = 0; i < ci->getNumArgOperands(); ++i) {
                            const Value *op = ci->getArgOperand(i);
                            if (local_nvm_ptrs.find(op) != local_nvm_ptrs.end()) {
                                calledArgs.insert(i);
                            }
                        }

                        findImportantOps(cfn, calledArgs);
                    }
                }
            }
            errs() << fn->getName() << ", BasicBlock: " << bb << "\n";
            errs() << format("\t%lu\n", nImp);

            imp_factor[key] = nImp;
        }
    }

    void FunctionInfo::accumulateImportanceFactor(const Function *fn, const unordered_set<int> &args) {
        if (acc_factor.find(fn->getName()) != acc_factor.end()) {
            return;
        }
        unordered_set<const Value*> local_nvm_ptrs = nvm_ptrs[fn->getName()];

        // Now, find all the users of the input arguments.
        for (int argno : args) {
            const Argument *arg = fn->arg_begin() + argno;
            unordered_set<const Value*> argDerivatives({arg});
            utils::getDerivativePtrs(argDerivatives);
            local_nvm_ptrs.insert(argDerivatives.begin(), argDerivatives.end());
        }

        size_t factor = 0;

        for (const BasicBlock &bb : *fn) {
            bkey_t key(&bb, args);
            size_t bbFactor = imp_factor[key];
            for (const Instruction &i : bb) {

                const CallInst *ci = dyn_cast<CallInst>(&i);
                if (ci && !ci->isInlineAsm()) {
                    const Function *cfn = ci->getCalledFunction();
                    if (cfn && !cfn->isIntrinsic()) {
                        unordered_set<int> calledArgs;
                        for (int i = 0; i < ci->getNumArgOperands(); ++i) {
                            const Value *op = ci->getArgOperand(i);
                            if (local_nvm_ptrs.find(op) != local_nvm_ptrs.end()) {
                                calledArgs.insert(i);
                            }
                        }

                        accumulateImportanceFactor(cfn, calledArgs);
                        bbFactor += acc_factor[cfn->getName()];
                    }
                }
            }

            factor += bbFactor;
        }

        errs() << fn->getName() << " overall factor: " << factor << "\n";

        acc_factor[fn->getName()] = factor;
    }

    void FunctionInfo::propagateToCallsites(const Function *fn,
            const unordered_set<int> &args) {
        if (acc_factor.find(fn->getName()) == acc_factor.end()) {
            return;
        }

        unordered_set<const Value*> local_nvm_ptrs = nvm_ptrs[fn->getName()];

        // Now, find all the users of the input arguments.
        for (int argno : args) {
            const Argument *arg = fn->arg_begin() + argno;
            unordered_set<const Value*> argDerivatives({arg});
            utils::getDerivativePtrs(argDerivatives);
            local_nvm_ptrs.insert(argDerivatives.begin(), argDerivatives.end());
        }

        errs() << "------ " << fn->getName() << "\n";

        for (const BasicBlock &bb : *fn) {
            bkey_t key(&bb, args);
            if (imp_total.find(key) != imp_total.end()) continue;

            size_t bbFactor = imp_factor[key];
            for (const Instruction &i : bb) {

                const CallInst *ci = dyn_cast<CallInst>(&i);
                if (ci && !ci->isInlineAsm()) {
                    const Function *cfn = ci->getCalledFunction();
                    if (cfn && !cfn->isIntrinsic()) {
                        unordered_set<int> calledArgs;
                        for (int i = 0; i < ci->getNumArgOperands(); ++i) {
                            const Value *op = ci->getArgOperand(i);
                            if (local_nvm_ptrs.find(op) != local_nvm_ptrs.end()) {
                                calledArgs.insert(i);
                            }
                        }

                        bbFactor += acc_factor[cfn->getName()];

                        // Also recurse
                        propagateToCallsites(cfn, calledArgs);
                    }
                }
            }

            imp_total[key] = bbFactor;
            errs() << "imp of " << fn->getName() << " " << &bb << "\n";
        }

    }

    void FunctionInfo::calcImportance(const BasicBlock *bb,
                                      const unordered_set<int> &args,
                                      const unordered_set<const BasicBlock*> &be) {
        bkey_t key(bb, args);
        if (imp_succ.find(key) != imp_succ.end()) return;

        size_t max_imp = 0;
        for (const BasicBlock *succ : successors(bb)) {
            DominatorTree &dom =
                mp_.getAnalysis<DominatorTreeWrapperPass>(
                        *const_cast<Function*>(bb->getParent())).getDomTree();
            PostDominatorTree &pdom =
                mp_.getAnalysis<PostDominatorTreeWrapperPass>(
                        *const_cast<Function*>(bb->getParent())).getPostDomTree();

            bool is_succ_loop_body = false;
            for (const BasicBlock *lbb : be) {
                is_succ_loop_body |= pdom.dominates(lbb, succ);
            }
            if (is_succ_loop_body) continue;

            bkey_t skey(succ, args);
            unordered_set<const BasicBlock*> beSucc(be.begin(), be.end());
            if (dom.dominates(succ, bb)) {
                beSucc.insert(bb);
            }

            calcImportance(succ, args, beSucc);
            max_imp = max_imp > imp_succ[skey] ? max_imp : imp_succ[skey];
        }

        imp_succ[key] = imp_total[key] + max_imp;
        errs() << bb->getParent()->getName() << " bb " << bb << "\n";
        errs() << "\t imp = " << imp_succ[key] << "\n";
    }

    void FunctionInfo::doSuccessorCalculation(const Function *fn,
            const unordered_set<int> &args) {

        unordered_set<const Value*> local_nvm_ptrs = nvm_ptrs[fn->getName()];

        // Now, find all the users of the input arguments.
        for (int argno : args) {
            const Argument *arg = fn->arg_begin() + argno;
            unordered_set<const Value*> argDerivatives({arg});
            utils::getDerivativePtrs(argDerivatives);
            local_nvm_ptrs.insert(argDerivatives.begin(), argDerivatives.end());
        }
        // First, do non-recursive calc, then find all the called functions and
        // do this for them
        const unordered_set<const BasicBlock*> visited;
        calcImportance(&fn->getEntryBlock(), args, visited);
        for (const BasicBlock &bb : *fn) {

            // Now, recurse
            for (const Instruction &i : bb) {

                const CallInst *ci = dyn_cast<CallInst>(&i);
                if (ci && !ci->isInlineAsm()) {
                    const Function *cfn = ci->getCalledFunction();
                    if (cfn && !cfn->isIntrinsic()) {
                        unordered_set<int> calledArgs;
                        for (int i = 0; i < ci->getNumArgOperands(); ++i) {
                            const Value *op = ci->getArgOperand(i);
                            if (local_nvm_ptrs.find(op) != local_nvm_ptrs.end()) {
                                calledArgs.insert(i);
                            }
                        }

                        doSuccessorCalculation(cfn, calledArgs);
                    }
                }
            }

        }

    }

    unordered_set<list<FunctionInfo::bbid_t>, FunctionInfo::bi_list_hash>
        FunctionInfo::getImportantPaths(const Function *fn,
                                        const unordered_set<int> &args)
    {
        key_t key(fn->getName(), args);
        if (paths_imp_total.find(key) != paths_imp_total.end()) {
            return paths_imp_total[key];
        }

        if (fn->isIntrinsic()) {
            // Initializes to empty.
            return paths_imp_total[key];
        }

        unordered_set<const Value*> local_nvm_ptrs = nvm_ptrs[fn->getName()];
        // Now, find all the users of the input arguments.
        for (int argno : args) {
            const Argument *arg = fn->arg_begin() + argno;
            unordered_set<const Value*> argDerivatives({arg});
            utils::getDerivativePtrs(argDerivatives);
            local_nvm_ptrs.insert(argDerivatives.begin(), argDerivatives.end());
        }

        // Track unique paths as sets of interesting basic blocks.
        /*
         * We will, again, explore the frontier. As we do, we will track a set
         * of important basic blocks visited by that path. When the path ends,
         * we will append the set of basic blocks into another set, thus determining
         * the total number of unique items.
         */
        queue<
            tuple<
                const BasicBlock*,
                list<FunctionInfo::bbid_t>,
                unordered_set<const BasicBlock*>
                >
            > frontier;
        unordered_set<list<FunctionInfo::bbid_t>, FunctionInfo::bi_list_hash> unique_paths;

        const BasicBlock &entry = fn->getEntryBlock();
        frontier.emplace(&entry, // current block
                         list<FunctionInfo::bbid_t>(), // path description
                         unordered_set<const BasicBlock*>()); // backedge detection

        while (!frontier.empty()) {
            const BasicBlock *bb = get<0>(frontier.front());
            auto &nvm_path = get<1>(frontier.front());
            auto &backedge = get<2>(frontier.front());

            bkey_t key(bb, args);
            // This means that the basic block is interesting.
            if (imp_factor.find(key) != imp_factor.end()
                && imp_factor[key] > 0) {
                nvm_path.emplace_back(bb, deque<const Instruction*>()); // we don't know our callsite
            }

            unordered_set<list<FunctionInfo::bbid_t>, FunctionInfo::bi_list_hash> subpaths;
            subpaths.insert(nvm_path);

            for (const Instruction &i : *bb) {

                const CallInst *ci = dyn_cast<CallInst>(&i);
                if (ci && !ci->isInlineAsm()) {
                    const Function *cfn = ci->getCalledFunction();
                    if (cfn && !cfn->isIntrinsic()) {
                        unordered_set<int> calledArgs;
                        for (int i = 0; i < ci->getNumArgOperands(); ++i) {
                            const Value *op = ci->getArgOperand(i);
                            if (local_nvm_ptrs.find(op) != local_nvm_ptrs.end()) {
                                calledArgs.insert(i);
                            }
                        }

                        unordered_set<list<FunctionInfo::bbid_t>,
                            FunctionInfo::bi_list_hash> new_subpaths;
                        // Recurse
                        const auto nested_paths = getImportantPaths(cfn, calledArgs);
                        for (const auto &root : subpaths) {
                            for (const auto &path : nested_paths) {
                                list<FunctionInfo::bbid_t> new_path(root.begin(), root.end());

                                for (FunctionInfo::bbid_t t : path) {
                                    get<1>(t).push_back(ci);
                                    new_path.push_back(t);
                                }

                                new_subpaths.insert(new_path);
                            }
                        }

                        subpaths = new_subpaths;
                    }
                }
            }

            // Now, we can look at successors
            DominatorTree &dom =
                mp_.getAnalysis<DominatorTreeWrapperPass>(
                        *const_cast<Function*>(fn)).getDomTree();

            size_t nsucc = 0;

            for (const BasicBlock *succ : successors(bb)) {
                nsucc++;
                if (dom.dominates(succ, bb)) {
                    if (backedge.find(succ) == backedge.end()) {
                        backedge.insert(succ);
                    } else {
                        continue;
                    }
                }

                for (const auto &path : subpaths) {
                    frontier.emplace(succ, path, backedge);
                }
            }

            if (!nsucc) {
                for (const auto &path : subpaths) {
                    unique_paths.insert(path);
                }
            }

            frontier.pop();
        }


        for (const auto &path : unique_paths) {
            errs() << "\t" << fn->getName() << " has a path of length "
                << path.size() << "\n";
            if (path.size() == 1) {
                for (const auto &t : path) {
                    errs() << *get<0>(t) << "\n";
                }
            }
        }

        paths_imp_total[key] = unique_paths;
        return unique_paths;
    }

    size_t FunctionInfo::totalImportantPaths(
            const Function *root, const unordered_set<int> &args)
    {
        const auto &paths = getImportantPaths(root, args);
        size_t nPaths = 0;
        for (const auto &path : paths) {
            nPaths += path.empty() ? 0 : 1;
        }
        return nPaths;
    }

    size_t FunctionInfo::totalImportantPaths(const Function *root) {
        unordered_set<int> empty;
        return totalImportantPaths(root, empty);
    }

    void FunctionInfo::computeImportantSuccessors(const Function *root) {
        // The root is important because it tells us to ignore the input arguments
        // to that particular function. It also helps us limit which function calls
        // are interesting or not.
        //
        // I think we can do this iteratively
        //
        // Part A - Find the importance factor of each block.
        // 1. Find all basic blocks which are important. Count the number of
        // stores, flushes, and fences in that basic block. That is the importance
        // counter of that basic block.
        // 2. For basic blocks with function calls, accumulate the importance count
        // of the underlying function (recursively). This in theory should be a
        // profile-based average of the paths, but we can just do a path-agnostic
        // sum of all possible successors for this.
        //
        // Part B - Find the importance factor of a block's successors
        // 1. For each function, do a (non-recursive) sum per path. Each node is the
        // max of it's descendants.

        // A.1 -- Single iteration importance factor.

        unordered_set<int> empty;
        findImportantOps(root, empty);

        // A.2 -- Accumulate
        accumulateImportanceFactor(root, empty);

        // A.3 -- Propagate importance to function callsites.
        propagateToCallsites(root, empty);

        // B.1 -- Do the successor calculation.
        doSuccessorCalculation(root, empty);
    }

    void FunctionInfo::dumpImportantSuccessors() {
        errs() << "\n-------------------------------------------------------\n";
        for (const auto &t : imp_succ) {
            const auto *bb = get<0>(get<0>(t));
            size_t factor  = get<1>(t);
            errs() << "For function " << bb->getParent()->getName()
                << ", the basic block \n" << *bb <<
                "\n has a successor factor of " << factor << "\n";
        }
        errs() << "\n-------------------------------------------------------\n";
    }

    void FunctionInfo::dumpUnique() {
        errs() << "\n-------------------------------------------------------\n";
        for (const auto &t : paths_imp_total) {
            const auto name = get<0>(get<0>(t));
            const auto &s = get<1>(t);
            errs() << "For function " << name
                << format(" there are %lu unique paths.\n", s.size());
        }
        errs() << "\n-------------------------------------------------------\n";
    }
}
