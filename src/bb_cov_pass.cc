#include "bb_cov_pass.hpp"

BB_COV_Pass::BB_COV_Pass() : llvm::ModulePass(BB_COV_Pass::ID) {}

char BB_COV_Pass::ID = 0;

#if LLVM_VERSION_MAJOR >= 4
llvm::StringRef
#else
const char *
#endif
BB_COV_Pass::getPassName() const {
  return "instrumenting for bb coverage";
}

bool BB_COV_Pass::runOnModule(llvm::Module &M) {
  Mod = &M;
  llvm::LLVMContext &Ctx = M.getContext();

  llvm::Type *voidTy = llvm::Type::getVoidTy(Ctx);
  llvm::Type *int8Ty = llvm::Type::getInt8Ty(Ctx);
  llvm::Type *int32Ty = llvm::Type::getInt32Ty(Ctx);
  llvm::Type *int64Ty = llvm::Type::getInt64Ty(Ctx);
  llvm::Type *int8PtrTy = llvm::Type::getInt8PtrTy(Ctx);
  llvm::Type *int32PtrTy = llvm::Type::getInt32PtrTy(Ctx);

  record_bb_cov = M.getOrInsertFunction("__record_bb_cov", voidTy, int8PtrTy,
                                        int8PtrTy, int32Ty);

  cov_fini = M.getOrInsertFunction("__cov_fini", voidTy);

  IRB = new llvm::IRBuilder<>(Ctx);

  for (auto &F : M.functions()) {
    const string mangled_func_name = F.getName().str();
    const string func_name = llvm::demangle(mangled_func_name);
    // const string func_name = mangled_func_name;


    if (F.isIntrinsic()) {
      continue;
    }

    if (func_name.find("_GLOBAL__sub_I_") != string::npos ||
        func_name.find("__cxx_global_var_init") != string::npos) {
      continue;
    }

    if (func_name == "main") {
      // Get all return instructions in main function
      set<llvm::ReturnInst *> ret_inst_set;
      for (auto &BB : F) {
        for (auto &I : BB) {
          if (llvm::ReturnInst *ret_inst =
                  llvm::dyn_cast<llvm::ReturnInst>(&I)) {
            ret_inst_set.insert(ret_inst);
          }
        }
      }

      // Insert cov_fini before each return instruction
      for (auto ret_inst : ret_inst_set) {
        IRB->SetInsertPoint(ret_inst);
        IRB->CreateCall(cov_fini, {});
      }
    }
    
    auto subp = F.getSubprogram();
    if (subp == NULL) {
      continue;
    }

    const string dirname = subp->getDirectory().str();
    string filename = subp->getFilename().str();

    if (dirname != "") {
      filename = dirname + "/" + filename;
    }

    if (filename.find("/usr/bin") != string::npos) {
      continue;
    }

    instrument_bb_cov(&F, filename, func_name);
  }

  // Create empty .cov template files (overwrite)
  for (auto [filename, file_coverage_map] : file_bb_map) {
    ofstream cov_file(filename + ".cov");

    for (auto [func, bb_capacity] : file_coverage_map) {
      cov_file << "F " << func << " " << false << "\n";
      for (size_t i = 0; i < bb_capacity; i++) {
        cov_file << "b " << i << " " << false << "\n";
      }
    }

    cov_file.close();
  }

  delete IRB;

  llvm::errs() << "Verifying module...\n";
  string out;
  llvm::raw_string_ostream output(out);
  bool has_error = verifyModule(M, &output);

  if (has_error > 0) {
    llvm::errs() << "IR errors : \n";
    llvm::errs() << out;
    return false;
  }

  llvm::errs() << "Verifying done without errors\n";

  return true;
}

void BB_COV_Pass::instrument_bb_cov(llvm::Function *F, const string &filename,
                                    const string &func_name) {
  if (file_bb_map.find(filename) == file_bb_map.end()) {
    file_bb_map.insert({filename, {}});
  }

  if (file_bb_map[filename].find(func_name) == file_bb_map[filename].end()) {
    file_bb_map[filename].insert({func_name, {}});
  }

  llvm::Constant *filename_const = gen_new_string_constant(filename, IRB, Mod);
  llvm::Constant *func_name_const =
      gen_new_string_constant(func_name, IRB, Mod);

  size_t &bb_capacity = file_bb_map[filename][func_name];

  llvm::errs() << "Instrumenting " << func_name << " in " << filename << "\n";

  // For each basic block, insert record_bb_cov to notify that the basic block
  // is reached
  for (auto &BB : F->getBasicBlockList()) {
    llvm::Instruction *first_inst = BB.getFirstNonPHIOrDbgOrLifetime();
    if (first_inst == NULL) {
      continue;
    }

    if (llvm::isa<llvm::LandingPadInst>(first_inst)) {
      continue;
    }

    // Get unsigned 32bit integer LLVM constant
    llvm::Constant *bb_index_const = llvm::ConstantInt::get(
        llvm::Type::getInt32Ty(Mod->getContext()), bb_capacity++);

    IRB->SetInsertPoint(first_inst);
    IRB->CreateCall(record_bb_cov,
                    {filename_const, func_name_const, bb_index_const});
  }

  // Insert cov fini before each exit call
  for (auto &BB : F->getBasicBlockList()) {
    for (auto &IN : BB) {
      if (!llvm::isa<llvm::CallInst>(IN)) {
        continue;
      }

      llvm::CallInst *call_inst = llvm::dyn_cast<llvm::CallInst>(&IN);
      llvm::Function *called_func = call_inst->getCalledFunction();
      if (called_func == NULL) {
        continue;
      }

      string called_func_name = called_func->getName().str();
      if (called_func_name != "exit") {
        continue;
      }
      IRB->SetInsertPoint(call_inst);
      IRB->CreateCall(cov_fini, {});
    }
  }
}

static llvm::Constant *gen_new_string_constant(string name,
                                               llvm::IRBuilder<> *IRB,
                                               llvm::Module *Mod) {
  auto search = new_string_globals.find(name);

  if (search == new_string_globals.end()) {
    llvm::Constant *new_global = IRB->CreateGlobalStringPtr(name, "", 0, Mod);
    new_string_globals.insert(make_pair(name, new_global));
    return new_global;
  }

  return search->second;
}

static llvm::RegisterPass<BB_COV_Pass> X("bbcov", "BB coverage pass", false,
                                         false);

static void registerPass(const llvm::PassManagerBuilder &,
                         llvm::legacy::PassManagerBase &PM) {
  auto p = new BB_COV_Pass();
  PM.add(p);
}

static llvm::RegisterStandardPasses RegisterPassOpt(
    llvm::PassManagerBuilder::EP_ModuleOptimizerEarly, registerPass);

static llvm::RegisterStandardPasses RegisterPassO0(
    llvm::PassManagerBuilder::EP_EnabledOnOptLevel0, registerPass);