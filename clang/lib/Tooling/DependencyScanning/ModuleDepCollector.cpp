//===- ModuleDepCollector.cpp - Callbacks to collect deps -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Tooling/DependencyScanning/ModuleDepCollector.h"

#include "clang/Basic/MakeSupport.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningWorker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/BLAKE3.h"
#include <optional>

using namespace clang;
using namespace tooling;
using namespace dependencies;

void ModuleDeps::forEachFileDep(llvm::function_ref<void(StringRef)> Cb) const {
  SmallString<0> PathBuf;
  PathBuf.reserve(256);
  for (StringRef FileDep : FileDeps) {
    auto ResolvedFileDep =
        ASTReader::ResolveImportedPath(PathBuf, FileDep, FileDepsBaseDir);
    Cb(*ResolvedFileDep);
  }
}

const std::vector<std::string> &ModuleDeps::getBuildArguments() const {
  // FIXME: this operation is not thread safe and is expected to be called
  // on a single thread. Otherwise it should be protected with a lock.
  assert(!std::holds_alternative<std::monostate>(BuildInfo) &&
         "Using uninitialized ModuleDeps");
  if (const auto *CI = std::get_if<CowCompilerInvocation>(&BuildInfo))
    BuildInfo = CI->getCC1CommandLine();
  return std::get<std::vector<std::string>>(BuildInfo);
}

void PrebuiltModuleASTAttrs::updateDependentsNotInStableDirs(
    PrebuiltModulesAttrsMap &PrebuiltModulesMap) {
  setInStableDir();
  for (const auto Dep : ModuleFileDependents) {
    if (!PrebuiltModulesMap[Dep].isInStableDir())
      return;
    PrebuiltModulesMap[Dep].updateDependentsNotInStableDirs(PrebuiltModulesMap);
  }
}

static void
optimizeHeaderSearchOpts(HeaderSearchOptions &Opts, ASTReader &Reader,
                         const serialization::ModuleFile &MF,
                         const PrebuiltModulesAttrsMap &PrebuiltModulesASTMap,
                         ScanningOptimizations OptimizeArgs) {
  if (any(OptimizeArgs & ScanningOptimizations::HeaderSearch)) {
    // Only preserve search paths that were used during the dependency scan.
    std::vector<HeaderSearchOptions::Entry> Entries;
    std::swap(Opts.UserEntries, Entries);

    llvm::BitVector SearchPathUsage(Entries.size());
    llvm::DenseSet<const serialization::ModuleFile *> Visited;
    std::function<void(const serialization::ModuleFile *)> VisitMF =
        [&](const serialization::ModuleFile *MF) {
          SearchPathUsage |= MF->SearchPathUsage;
          Visited.insert(MF);
          for (const serialization::ModuleFile *Import : MF->Imports)
            if (!Visited.contains(Import))
              VisitMF(Import);
        };
    VisitMF(&MF);

    if (SearchPathUsage.size() != Entries.size())
      llvm::report_fatal_error(
          "Inconsistent search path options between modules detected");

    for (auto Idx : SearchPathUsage.set_bits())
      Opts.UserEntries.push_back(std::move(Entries[Idx]));
  }
  if (any(OptimizeArgs & ScanningOptimizations::VFS)) {
    std::vector<std::string> VFSOverlayFiles;
    std::swap(Opts.VFSOverlayFiles, VFSOverlayFiles);

    llvm::BitVector VFSUsage(VFSOverlayFiles.size());
    llvm::DenseSet<const serialization::ModuleFile *> Visited;
    std::function<void(const serialization::ModuleFile *)> VisitMF =
        [&](const serialization::ModuleFile *MF) {
          Visited.insert(MF);
          if (MF->Kind == serialization::MK_ImplicitModule) {
            VFSUsage |= MF->VFSUsage;
            // We only need to recurse into implicit modules. Other module types
            // will have the correct set of VFSs for anything they depend on.
            for (const serialization::ModuleFile *Import : MF->Imports)
              if (!Visited.contains(Import))
                VisitMF(Import);
          } else {
            // This is not an implicitly built module, so it may have different
            // VFS options. Fall back to a string comparison instead.
            auto PrebuiltModulePropIt =
                PrebuiltModulesASTMap.find(MF->FileName);
            if (PrebuiltModulePropIt == PrebuiltModulesASTMap.end())
              return;
            for (std::size_t I = 0, E = VFSOverlayFiles.size(); I != E; ++I) {
              if (PrebuiltModulePropIt->second.getVFS().contains(
                      VFSOverlayFiles[I]))
                VFSUsage[I] = true;
            }
          }
        };
    VisitMF(&MF);

    if (VFSUsage.size() != VFSOverlayFiles.size())
      llvm::report_fatal_error(
          "Inconsistent -ivfsoverlay options between modules detected");

    for (auto Idx : VFSUsage.set_bits())
      Opts.VFSOverlayFiles.push_back(std::move(VFSOverlayFiles[Idx]));
  }
}

static void optimizeDiagnosticOpts(DiagnosticOptions &Opts,
                                   bool IsSystemModule) {
  // If this is not a system module or -Wsystem-headers was passed, don't
  // optimize.
  if (!IsSystemModule)
    return;
  bool Wsystem_headers = false;
  for (StringRef Opt : Opts.Warnings) {
    bool isPositive = !Opt.consume_front("no-");
    if (Opt == "system-headers")
      Wsystem_headers = isPositive;
  }
  if (Wsystem_headers)
    return;

  // Remove all warning flags. System modules suppress most, but not all,
  // warnings.
  Opts.Warnings.clear();
  Opts.UndefPrefixes.clear();
  Opts.Remarks.clear();
}

static void optimizeCWD(CowCompilerInvocation &BuildInvocation, StringRef CWD) {
  BuildInvocation.getMutFileSystemOpts().WorkingDir.clear();
  if (BuildInvocation.getCodeGenOpts().DwarfVersion) {
    // It is necessary to explicitly set the DebugCompilationDir
    // to a common directory (e.g. root) if IgnoreCWD is true.
    // When IgnoreCWD is true, the module's content should not
    // depend on the current working directory. However, if dwarf
    // information is needed (when CGOpts.DwarfVersion is
    // non-zero), then CGOpts.DebugCompilationDir must be
    // populated, because otherwise the current working directory
    // will be automatically embedded in the dwarf information in
    // the pcm, contradicting the assumption that it is safe to
    // ignore the CWD. Thus in such cases,
    // CGOpts.DebugCompilationDir is explicitly set to a common
    // directory.
    // FIXME: It is still excessive to create a copy of
    // CodeGenOpts for each module. Since we do not modify the
    // CodeGenOpts otherwise per module, the following code
    // ends up generating identical CodeGenOpts for each module
    // with DebugCompilationDir pointing to the root directory.
    // We can optimize this away by creating a _single_ copy of
    // CodeGenOpts whose DebugCompilationDir points to the root
    // directory and reuse it across modules.
    BuildInvocation.getMutCodeGenOpts().DebugCompilationDir =
        llvm::sys::path::root_path(CWD);
  }
}

static std::vector<std::string> splitString(std::string S, char Separator) {
  SmallVector<StringRef> Segments;
  StringRef(S).split(Segments, Separator, /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  std::vector<std::string> Result;
  Result.reserve(Segments.size());
  for (StringRef Segment : Segments)
    Result.push_back(Segment.str());
  return Result;
}

void ModuleDepCollector::addOutputPaths(CowCompilerInvocation &CI,
                                        ModuleDeps &Deps) {
  CI.getMutFrontendOpts().OutputFile =
      Controller.lookupModuleOutput(Deps, ModuleOutputKind::ModuleFile);
  if (!CI.getDiagnosticOpts().DiagnosticSerializationFile.empty())
    CI.getMutDiagnosticOpts().DiagnosticSerializationFile =
        Controller.lookupModuleOutput(
            Deps, ModuleOutputKind::DiagnosticSerializationFile);
  if (!CI.getDependencyOutputOpts().OutputFile.empty()) {
    CI.getMutDependencyOutputOpts().OutputFile =
        Controller.lookupModuleOutput(Deps, ModuleOutputKind::DependencyFile);
    CI.getMutDependencyOutputOpts().Targets =
        splitString(Controller.lookupModuleOutput(
                        Deps, ModuleOutputKind::DependencyTargets),
                    '\0');
    if (!CI.getDependencyOutputOpts().OutputFile.empty() &&
        CI.getDependencyOutputOpts().Targets.empty()) {
      // Fallback to -o as dependency target, as in the driver.
      SmallString<128> Target;
      quoteMakeTarget(CI.getFrontendOpts().OutputFile, Target);
      CI.getMutDependencyOutputOpts().Targets.push_back(std::string(Target));
    }
  }
}

void dependencies::resetBenignCodeGenOptions(frontend::ActionKind ProgramAction,
                                             const LangOptions &LangOpts,
                                             CodeGenOptions &CGOpts) {
  // TODO: Figure out better way to set options to their default value.
  if (ProgramAction == frontend::GenerateModule) {
    CGOpts.MainFileName.clear();
    CGOpts.DwarfDebugFlags.clear();
  }
  if (ProgramAction == frontend::GeneratePCH ||
      (ProgramAction == frontend::GenerateModule && !LangOpts.ModulesCodegen)) {
    CGOpts.DebugCompilationDir.clear();
    CGOpts.CoverageCompilationDir.clear();
    CGOpts.CoverageDataFile.clear();
    CGOpts.CoverageNotesFile.clear();
    CGOpts.ProfileInstrumentUsePath.clear();
    CGOpts.SampleProfileFile.clear();
    CGOpts.ProfileRemappingFile.clear();
  }
}

bool dependencies::isPathInStableDir(const ArrayRef<StringRef> Directories,
                                     const StringRef Input) {
  using namespace llvm::sys;

  if (!path::is_absolute(Input))
    return false;

  auto PathStartsWith = [](StringRef Prefix, StringRef Path) {
    auto PrefixIt = path::begin(Prefix), PrefixEnd = path::end(Prefix);
    for (auto PathIt = path::begin(Path), PathEnd = path::end(Path);
         PrefixIt != PrefixEnd && PathIt != PathEnd; ++PrefixIt, ++PathIt) {
      if (*PrefixIt != *PathIt)
        return false;
    }
    return PrefixIt == PrefixEnd;
  };

  return any_of(Directories, [&](StringRef Dir) {
    return !Dir.empty() && PathStartsWith(Dir, Input);
  });
}

bool dependencies::areOptionsInStableDir(const ArrayRef<StringRef> Directories,
                                         const HeaderSearchOptions &HSOpts) {
  assert(isPathInStableDir(Directories, HSOpts.Sysroot) &&
         "Sysroots differ between module dependencies and current TU");

  assert(isPathInStableDir(Directories, HSOpts.ResourceDir) &&
         "ResourceDirs differ between module dependencies and current TU");

  for (const auto &Entry : HSOpts.UserEntries) {
    if (!Entry.IgnoreSysRoot)
      continue;
    if (!isPathInStableDir(Directories, Entry.Path))
      return false;
  }

  for (const auto &SysPrefix : HSOpts.SystemHeaderPrefixes) {
    if (!isPathInStableDir(Directories, SysPrefix.Prefix))
      return false;
  }

  return true;
}

static CowCompilerInvocation
makeCommonInvocationForModuleBuild(CompilerInvocation CI) {
  CI.resetNonModularOptions();
  CI.clearImplicitModuleBuildOptions();

  // The scanner takes care to avoid passing non-affecting module maps to the
  // explicit compiles. No need to do extra work just to find out there are no
  // module map files to prune.
  CI.getHeaderSearchOpts().ModulesPruneNonAffectingModuleMaps = false;

  // Remove options incompatible with explicit module build or are likely to
  // differ between identical modules discovered from different translation
  // units.
  CI.getFrontendOpts().Inputs.clear();
  CI.getFrontendOpts().OutputFile.clear();
  // LLVM options are not going to affect the AST
  CI.getFrontendOpts().LLVMArgs.clear();

  resetBenignCodeGenOptions(frontend::GenerateModule, CI.getLangOpts(),
                            CI.getCodeGenOpts());

  // Map output paths that affect behaviour to "-" so their existence is in the
  // context hash. The final path will be computed in addOutputPaths.
  if (!CI.getDiagnosticOpts().DiagnosticSerializationFile.empty())
    CI.getDiagnosticOpts().DiagnosticSerializationFile = "-";
  if (!CI.getDependencyOutputOpts().OutputFile.empty())
    CI.getDependencyOutputOpts().OutputFile = "-";
  CI.getDependencyOutputOpts().Targets.clear();

  CI.getFrontendOpts().ProgramAction = frontend::GenerateModule;
  CI.getLangOpts().ModuleName.clear();

  // Remove any macro definitions that are explicitly ignored.
  if (!CI.getHeaderSearchOpts().ModulesIgnoreMacros.empty()) {
    llvm::erase_if(
        CI.getPreprocessorOpts().Macros,
        [&CI](const std::pair<std::string, bool> &Def) {
          StringRef MacroDef = Def.first;
          return CI.getHeaderSearchOpts().ModulesIgnoreMacros.contains(
              llvm::CachedHashString(MacroDef.split('=').first));
        });
    // Remove the now unused option.
    CI.getHeaderSearchOpts().ModulesIgnoreMacros.clear();
  }

  return CI;
}

CowCompilerInvocation
ModuleDepCollector::getInvocationAdjustedForModuleBuildWithoutOutputs(
    const ModuleDeps &Deps,
    llvm::function_ref<void(CowCompilerInvocation &)> Optimize) const {
  CowCompilerInvocation CI = CommonInvocation;

  CI.getMutLangOpts().ModuleName = Deps.ID.ModuleName;
  CI.getMutFrontendOpts().IsSystemModule = Deps.IsSystem;

  // Inputs
  InputKind ModuleMapInputKind(CI.getFrontendOpts().DashX.getLanguage(),
                               InputKind::Format::ModuleMap);
  CI.getMutFrontendOpts().Inputs.emplace_back(Deps.ClangModuleMapFile,
                                              ModuleMapInputKind);

  auto CurrentModuleMapEntry =
      ScanInstance.getFileManager().getOptionalFileRef(Deps.ClangModuleMapFile);
  assert(CurrentModuleMapEntry && "module map file entry not found");

  // Remove directly passed modulemap files. They will get added back if they
  // were actually used.
  CI.getMutFrontendOpts().ModuleMapFiles.clear();

  auto DepModuleMapFiles = collectModuleMapFiles(Deps.ClangModuleDeps);
  for (StringRef ModuleMapFile : Deps.ModuleMapFileDeps) {
    // TODO: Track these as `FileEntryRef` to simplify the equality check below.
    auto ModuleMapEntry =
        ScanInstance.getFileManager().getOptionalFileRef(ModuleMapFile);
    assert(ModuleMapEntry && "module map file entry not found");

    // Don't report module maps describing eagerly-loaded dependency. This
    // information will be deserialized from the PCM.
    // TODO: Verify this works fine when modulemap for module A is eagerly
    // loaded from A.pcm, and module map passed on the command line contains
    // definition of a submodule: "explicit module A.Private { ... }".
    if (Service.shouldEagerLoadModules() &&
        DepModuleMapFiles.contains(*ModuleMapEntry))
      continue;

    // Don't report module map file of the current module unless it also
    // describes a dependency (for symmetry).
    if (*ModuleMapEntry == *CurrentModuleMapEntry &&
        !DepModuleMapFiles.contains(*ModuleMapEntry))
      continue;

    CI.getMutFrontendOpts().ModuleMapFiles.emplace_back(ModuleMapFile);
  }

  // Report the prebuilt modules this module uses.
  for (const auto &PrebuiltModule : Deps.PrebuiltModuleDeps)
    CI.getMutFrontendOpts().ModuleFiles.push_back(PrebuiltModule.PCMFile);

  // Add module file inputs from dependencies.
  addModuleFiles(CI, Deps.ClangModuleDeps);

  if (!CI.getDiagnosticOpts().SystemHeaderWarningsModules.empty()) {
    // Apply -Wsystem-headers-in-module for the current module.
    if (llvm::is_contained(CI.getDiagnosticOpts().SystemHeaderWarningsModules,
                           Deps.ID.ModuleName))
      CI.getMutDiagnosticOpts().Warnings.push_back("system-headers");
    // Remove the now unused option(s).
    CI.getMutDiagnosticOpts().SystemHeaderWarningsModules.clear();
  }

  Optimize(CI);

  return CI;
}

llvm::DenseSet<const FileEntry *> ModuleDepCollector::collectModuleMapFiles(
    ArrayRef<ModuleID> ClangModuleDeps) const {
  llvm::DenseSet<const FileEntry *> ModuleMapFiles;
  for (const ModuleID &MID : ClangModuleDeps) {
    ModuleDeps *MD = ModuleDepsByID.lookup(MID);
    assert(MD && "Inconsistent dependency info");
    // TODO: Track ClangModuleMapFile as `FileEntryRef`.
    auto FE = ScanInstance.getFileManager().getOptionalFileRef(
        MD->ClangModuleMapFile);
    assert(FE && "Missing module map file that was previously found");
    ModuleMapFiles.insert(*FE);
  }
  return ModuleMapFiles;
}

void ModuleDepCollector::addModuleMapFiles(
    CompilerInvocation &CI, ArrayRef<ModuleID> ClangModuleDeps) const {
  if (Service.shouldEagerLoadModules())
    return; // Only pcm is needed for eager load.

  for (const ModuleID &MID : ClangModuleDeps) {
    ModuleDeps *MD = ModuleDepsByID.lookup(MID);
    assert(MD && "Inconsistent dependency info");
    CI.getFrontendOpts().ModuleMapFiles.push_back(MD->ClangModuleMapFile);
  }
}

void ModuleDepCollector::addModuleFiles(
    CompilerInvocation &CI, ArrayRef<ModuleID> ClangModuleDeps) const {
  for (const ModuleID &MID : ClangModuleDeps) {
    ModuleDeps *MD = ModuleDepsByID.lookup(MID);
    std::string PCMPath =
        Controller.lookupModuleOutput(*MD, ModuleOutputKind::ModuleFile);

    if (Service.shouldEagerLoadModules())
      CI.getFrontendOpts().ModuleFiles.push_back(std::move(PCMPath));
    else
      CI.getHeaderSearchOpts().PrebuiltModuleFiles.insert(
          {MID.ModuleName, std::move(PCMPath)});
  }
}

void ModuleDepCollector::addModuleFiles(
    CowCompilerInvocation &CI, ArrayRef<ModuleID> ClangModuleDeps) const {
  for (const ModuleID &MID : ClangModuleDeps) {
    ModuleDeps *MD = ModuleDepsByID.lookup(MID);
    std::string PCMPath =
        Controller.lookupModuleOutput(*MD, ModuleOutputKind::ModuleFile);

    if (Service.shouldEagerLoadModules())
      CI.getMutFrontendOpts().ModuleFiles.push_back(std::move(PCMPath));
    else
      CI.getMutHeaderSearchOpts().PrebuiltModuleFiles.insert(
          {MID.ModuleName, std::move(PCMPath)});
  }
}

static bool needsModules(FrontendInputFile FIF) {
  switch (FIF.getKind().getLanguage()) {
  case Language::Unknown:
  case Language::Asm:
  case Language::LLVM_IR:
    return false;
  default:
    return true;
  }
}

void ModuleDepCollector::applyDiscoveredDependencies(CompilerInvocation &CI) {
  CI.clearImplicitModuleBuildOptions();
  resetBenignCodeGenOptions(CI.getFrontendOpts().ProgramAction,
                            CI.getLangOpts(), CI.getCodeGenOpts());

  if (llvm::any_of(CI.getFrontendOpts().Inputs, needsModules)) {
    Preprocessor &PP = ScanInstance.getPreprocessor();
    if (Module *CurrentModule = PP.getCurrentModuleImplementation())
      if (OptionalFileEntryRef CurrentModuleMap =
              PP.getHeaderSearchInfo()
                  .getModuleMap()
                  .getModuleMapFileForUniquing(CurrentModule))
        CI.getFrontendOpts().ModuleMapFiles.emplace_back(
            CurrentModuleMap->getNameAsRequested());

    SmallVector<ModuleID> DirectDeps;
    for (const auto &KV : ModularDeps)
      if (DirectModularDeps.contains(KV.first))
        DirectDeps.push_back(KV.second->ID);

    // TODO: Report module maps the same way it's done for modular dependencies.
    addModuleMapFiles(CI, DirectDeps);

    addModuleFiles(CI, DirectDeps);

    for (const auto &KV : DirectPrebuiltModularDeps)
      CI.getFrontendOpts().ModuleFiles.push_back(KV.second.PCMFile);
  }
}

static bool isSafeToIgnoreCWD(const CowCompilerInvocation &CI) {
  // Check if the command line input uses relative paths.
  // It is not safe to ignore the current working directory if any of the
  // command line inputs use relative paths.
#define IF_RELATIVE_RETURN_FALSE(PATH)                                         \
  do {                                                                         \
    if (!PATH.empty() && !llvm::sys::path::is_absolute(PATH))                  \
      return false;                                                            \
  } while (0)

#define IF_ANY_RELATIVE_RETURN_FALSE(PATHS)                                    \
  do {                                                                         \
    if (llvm::any_of(PATHS, [](const auto &P) {                                \
          return !P.empty() && !llvm::sys::path::is_absolute(P);               \
        }))                                                                    \
      return false;                                                            \
  } while (0)

  // Header search paths.
  const auto &HeaderSearchOpts = CI.getHeaderSearchOpts();
  IF_RELATIVE_RETURN_FALSE(HeaderSearchOpts.Sysroot);
  for (auto &Entry : HeaderSearchOpts.UserEntries)
    if (Entry.IgnoreSysRoot)
      IF_RELATIVE_RETURN_FALSE(Entry.Path);
  IF_RELATIVE_RETURN_FALSE(HeaderSearchOpts.ResourceDir);
  IF_RELATIVE_RETURN_FALSE(HeaderSearchOpts.ModuleCachePath);
  IF_RELATIVE_RETURN_FALSE(HeaderSearchOpts.ModuleUserBuildPath);
  for (auto I = HeaderSearchOpts.PrebuiltModuleFiles.begin(),
            E = HeaderSearchOpts.PrebuiltModuleFiles.end();
       I != E;) {
    auto Current = I++;
    IF_RELATIVE_RETURN_FALSE(Current->second);
  }
  IF_ANY_RELATIVE_RETURN_FALSE(HeaderSearchOpts.PrebuiltModulePaths);
  IF_ANY_RELATIVE_RETURN_FALSE(HeaderSearchOpts.VFSOverlayFiles);

  // Preprocessor options.
  const auto &PPOpts = CI.getPreprocessorOpts();
  IF_ANY_RELATIVE_RETURN_FALSE(PPOpts.MacroIncludes);
  IF_ANY_RELATIVE_RETURN_FALSE(PPOpts.Includes);
  IF_RELATIVE_RETURN_FALSE(PPOpts.ImplicitPCHInclude);

  // Frontend options.
  const auto &FrontendOpts = CI.getFrontendOpts();
  for (const FrontendInputFile &Input : FrontendOpts.Inputs) {
    if (Input.isBuffer())
      continue; // FIXME: Can this happen when parsing command-line?

    IF_RELATIVE_RETURN_FALSE(Input.getFile());
  }
  IF_RELATIVE_RETURN_FALSE(FrontendOpts.CodeCompletionAt.FileName);
  IF_ANY_RELATIVE_RETURN_FALSE(FrontendOpts.ModuleMapFiles);
  IF_ANY_RELATIVE_RETURN_FALSE(FrontendOpts.ModuleFiles);
  IF_ANY_RELATIVE_RETURN_FALSE(FrontendOpts.ModulesEmbedFiles);
  IF_ANY_RELATIVE_RETURN_FALSE(FrontendOpts.ASTMergeFiles);
  IF_RELATIVE_RETURN_FALSE(FrontendOpts.OverrideRecordLayoutsFile);
  IF_RELATIVE_RETURN_FALSE(FrontendOpts.StatsFile);

  // Filesystem options.
  const auto &FileSystemOpts = CI.getFileSystemOpts();
  IF_RELATIVE_RETURN_FALSE(FileSystemOpts.WorkingDir);

  // Codegen options.
  const auto &CodeGenOpts = CI.getCodeGenOpts();
  IF_RELATIVE_RETURN_FALSE(CodeGenOpts.DebugCompilationDir);
  IF_RELATIVE_RETURN_FALSE(CodeGenOpts.CoverageCompilationDir);

  // Sanitizer options.
  IF_ANY_RELATIVE_RETURN_FALSE(CI.getLangOpts().NoSanitizeFiles);

  // Coverage mappings.
  IF_RELATIVE_RETURN_FALSE(CodeGenOpts.ProfileInstrumentUsePath);
  IF_RELATIVE_RETURN_FALSE(CodeGenOpts.SampleProfileFile);
  IF_RELATIVE_RETURN_FALSE(CodeGenOpts.ProfileRemappingFile);

  // Dependency output options.
  for (auto &ExtraDep : CI.getDependencyOutputOpts().ExtraDeps)
    IF_RELATIVE_RETURN_FALSE(ExtraDep.first);

  return true;
}

static std::string getModuleContextHash(const ModuleDeps &MD,
                                        const CowCompilerInvocation &CI,
                                        bool EagerLoadModules, bool IgnoreCWD,
                                        llvm::vfs::FileSystem &VFS) {
  llvm::HashBuilder<llvm::TruncatedBLAKE3<16>, llvm::endianness::native>
      HashBuilder;

  // Hash the compiler version and serialization version to ensure the module
  // will be readable.
  HashBuilder.add(getClangFullRepositoryVersion());
  HashBuilder.add(serialization::VERSION_MAJOR, serialization::VERSION_MINOR);
  llvm::ErrorOr<std::string> CWD = VFS.getCurrentWorkingDirectory();
  if (CWD && !IgnoreCWD)
    HashBuilder.add(*CWD);

  // Hash the BuildInvocation without any input files.
  SmallString<0> ArgVec;
  ArgVec.reserve(4096);
  CI.generateCC1CommandLine([&](const Twine &Arg) {
    Arg.toVector(ArgVec);
    ArgVec.push_back('\0');
  });
  HashBuilder.add(ArgVec);

  // Hash the module dependencies. These paths may differ even if the invocation
  // is identical if they depend on the contents of the files in the TU -- for
  // example, case-insensitive paths to modulemap files. Usually such a case
  // would indicate a missed optimization to canonicalize, but it may be
  // difficult to canonicalize all cases when there is a VFS.
  for (const auto &ID : MD.ClangModuleDeps) {
    HashBuilder.add(ID.ModuleName);
    HashBuilder.add(ID.ContextHash);
  }

  HashBuilder.add(EagerLoadModules);

  llvm::BLAKE3Result<16> Hash = HashBuilder.final();
  std::array<uint64_t, 2> Words;
  static_assert(sizeof(Hash) == sizeof(Words), "Hash must match Words");
  std::memcpy(Words.data(), Hash.data(), sizeof(Hash));
  return toString(llvm::APInt(sizeof(Words) * 8, Words), 36, /*Signed=*/false);
}

void ModuleDepCollector::associateWithContextHash(
    const CowCompilerInvocation &CI, bool IgnoreCWD, ModuleDeps &Deps) {
  Deps.ID.ContextHash =
      getModuleContextHash(Deps, CI, Service.shouldEagerLoadModules(),
                           IgnoreCWD, ScanInstance.getVirtualFileSystem());
  bool Inserted = ModuleDepsByID.insert({Deps.ID, &Deps}).second;
  (void)Inserted;
  assert(Inserted && "duplicate module mapping");
}

void ModuleDepCollectorPP::LexedFileChanged(FileID FID,
                                            LexedFileChangeReason Reason,
                                            SrcMgr::CharacteristicKind FileType,
                                            FileID PrevFID,
                                            SourceLocation Loc) {
  if (Reason != LexedFileChangeReason::EnterFile)
    return;

  SourceManager &SM = MDC.ScanInstance.getSourceManager();

  // Dependency generation really does want to go all the way to the
  // file entry for a source location to find out what is depended on.
  // We do not want #line markers to affect dependency generation!
  if (std::optional<StringRef> Filename = SM.getNonBuiltinFilenameForID(FID))
    MDC.addFileDep(llvm::sys::path::remove_leading_dotslash(*Filename));
}

void ModuleDepCollectorPP::InclusionDirective(
    SourceLocation HashLoc, const Token &IncludeTok, StringRef FileName,
    bool IsAngled, CharSourceRange FilenameRange, OptionalFileEntryRef File,
    StringRef SearchPath, StringRef RelativePath, const Module *SuggestedModule,
    bool ModuleImported, SrcMgr::CharacteristicKind FileType) {
  if (!File && !ModuleImported) {
    // This is a non-modular include that HeaderSearch failed to find. Add it
    // here as `FileChanged` will never see it.
    MDC.addFileDep(FileName);
  }
  handleImport(SuggestedModule);
}

void ModuleDepCollectorPP::moduleImport(SourceLocation ImportLoc,
                                        ModuleIdPath Path,
                                        const Module *Imported) {
  if (MDC.ScanInstance.getPreprocessor().isInImportingCXXNamedModules()) {
    P1689ModuleInfo RequiredModule;
    RequiredModule.ModuleName = Path[0].getIdentifierInfo()->getName().str();
    RequiredModule.Type = P1689ModuleInfo::ModuleType::NamedCXXModule;
    MDC.RequiredStdCXXModules.push_back(std::move(RequiredModule));
    return;
  }

  handleImport(Imported);
}

void ModuleDepCollectorPP::handleImport(const Module *Imported) {
  if (!Imported)
    return;

  const Module *TopLevelModule = Imported->getTopLevelModule();

  if (MDC.isPrebuiltModule(TopLevelModule))
    MDC.DirectPrebuiltModularDeps.insert(
        {TopLevelModule, PrebuiltModuleDep{TopLevelModule}});
  else {
    MDC.DirectModularDeps.insert(TopLevelModule);
    MDC.DirectImports.insert(Imported);
  }
}

void ModuleDepCollectorPP::EndOfMainFile() {
  FileID MainFileID = MDC.ScanInstance.getSourceManager().getMainFileID();
  MDC.MainFile = std::string(MDC.ScanInstance.getSourceManager()
                                 .getFileEntryRefForID(MainFileID)
                                 ->getName());

  auto &PP = MDC.ScanInstance.getPreprocessor();
  if (PP.isInNamedModule()) {
    P1689ModuleInfo ProvidedModule;
    ProvidedModule.ModuleName = PP.getNamedModuleName();
    ProvidedModule.Type = P1689ModuleInfo::ModuleType::NamedCXXModule;
    ProvidedModule.IsStdCXXModuleInterface = PP.isInNamedInterfaceUnit();
    // Don't put implementation (non partition) unit as Provide.
    // Put the module as required instead. Since the implementation
    // unit will import the primary module implicitly.
    if (PP.isInImplementationUnit())
      MDC.RequiredStdCXXModules.push_back(ProvidedModule);
    else
      MDC.ProvidedStdCXXModule = ProvidedModule;
  }

  if (!MDC.ScanInstance.getPreprocessorOpts().ImplicitPCHInclude.empty())
    MDC.addFileDep(MDC.ScanInstance.getPreprocessorOpts().ImplicitPCHInclude);

  for (const Module *M :
       MDC.ScanInstance.getPreprocessor().getAffectingClangModules())
    if (!MDC.isPrebuiltModule(M))
      MDC.DirectModularDeps.insert(M);

  MDC.addVisibleModules();

  for (const Module *M : MDC.DirectModularDeps)
    handleTopLevelModule(M);

  MDC.Consumer.handleContextHash(
      MDC.ScanInstance.getInvocation().getModuleHash());

  MDC.Consumer.handleDependencyOutputOpts(*MDC.Opts);

  MDC.Consumer.handleProvidedAndRequiredStdCXXModules(
      MDC.ProvidedStdCXXModule, MDC.RequiredStdCXXModules);

  for (auto &&I : MDC.ModularDeps)
    MDC.Consumer.handleModuleDependency(*I.second);

  for (const Module *M : MDC.DirectModularDeps) {
    auto It = MDC.ModularDeps.find(M);
    // Only report direct dependencies that were successfully handled.
    if (It != MDC.ModularDeps.end())
      MDC.Consumer.handleDirectModuleDependency(It->second->ID);
  }

  for (auto &&I : MDC.VisibleModules)
    MDC.Consumer.handleVisibleModule(std::string(I.getKey()));

  for (auto &&I : MDC.FileDeps)
    MDC.Consumer.handleFileDependency(I);

  for (auto &&I : MDC.DirectPrebuiltModularDeps)
    MDC.Consumer.handlePrebuiltModuleDependency(I.second);
}

std::optional<ModuleID>
ModuleDepCollectorPP::handleTopLevelModule(const Module *M) {
  assert(M == M->getTopLevelModule() && "Expected top level module!");

  // A top-level module might not be actually imported as a module when
  // -fmodule-name is used to compile a translation unit that imports this
  // module. In that case it can be skipped. The appropriate header
  // dependencies will still be reported as expected.
  if (!M->getASTFile())
    return {};

  // If this module has been handled already, just return its ID.
  if (auto ModI = MDC.ModularDeps.find(M); ModI != MDC.ModularDeps.end())
    return ModI->second->ID;

  auto OwnedMD = std::make_unique<ModuleDeps>();
  ModuleDeps &MD = *OwnedMD;

  MD.ID.ModuleName = M->getFullModuleName();
  MD.IsSystem = M->IsSystem;

  // Start off with the assumption that this module is shareable when there
  // are stable directories. As more dependencies are discovered, check if those
  // come from the provided directories.
  MD.IsInStableDirectories = !MDC.StableDirs.empty();

  // For modules which use export_as link name, the linked product that of the
  // corresponding export_as-named module.
  if (!M->UseExportAsModuleLinkName)
    MD.LinkLibraries = M->LinkLibraries;

  ModuleMap &ModMapInfo =
      MDC.ScanInstance.getPreprocessor().getHeaderSearchInfo().getModuleMap();

  if (auto ModuleMap = ModMapInfo.getModuleMapFileForUniquing(M)) {
    SmallString<128> Path = ModuleMap->getNameAsRequested();
    ModMapInfo.canonicalizeModuleMapPath(Path);
    MD.ClangModuleMapFile = std::string(Path);
  }

  serialization::ModuleFile *MF =
      MDC.ScanInstance.getASTReader()->getModuleManager().lookup(
          *M->getASTFile());
  MD.FileDepsBaseDir = MF->BaseDirectory;
  MDC.ScanInstance.getASTReader()->visitInputFileInfos(
      *MF, /*IncludeSystem=*/true,
      [&](const serialization::InputFileInfo &IFI, bool IsSystem) {
        // The __inferred_module.map file is an insignificant implementation
        // detail of implicitly-built modules. The PCM will also report the
        // actual on-disk module map file that allowed inferring the module,
        // which is what we need for building the module explicitly
        // Let's ignore this file.
        if (IFI.UnresolvedImportedFilename.ends_with("__inferred_module.map"))
          return;
        MDC.addFileDep(MD, IFI.UnresolvedImportedFilename);
      });

  llvm::DenseSet<const Module *> SeenDeps;
  addAllSubmodulePrebuiltDeps(M, MD, SeenDeps);
  addAllSubmoduleDeps(M, MD, SeenDeps);
  addAllAffectingClangModules(M, MD, SeenDeps);

  SmallString<0> PathBuf;
  PathBuf.reserve(256);
  MDC.ScanInstance.getASTReader()->visitInputFileInfos(
      *MF, /*IncludeSystem=*/true,
      [&](const serialization::InputFileInfo &IFI, bool IsSystem) {
        if (MD.IsInStableDirectories) {
          auto FullFilePath = ASTReader::ResolveImportedPath(
              PathBuf, IFI.UnresolvedImportedFilename, MF->BaseDirectory);
          MD.IsInStableDirectories =
              isPathInStableDir(MDC.StableDirs, *FullFilePath);
        }
        if (!(IFI.TopLevel && IFI.ModuleMap))
          return;
        if (IFI.UnresolvedImportedFilenameAsRequested.ends_with(
                "__inferred_module.map"))
          return;
        auto ResolvedFilenameAsRequested = ASTReader::ResolveImportedPath(
            PathBuf, IFI.UnresolvedImportedFilenameAsRequested,
            MF->BaseDirectory);
        MD.ModuleMapFileDeps.emplace_back(*ResolvedFilenameAsRequested);
      });

  bool IgnoreCWD = false;
  CowCompilerInvocation CI =
      MDC.getInvocationAdjustedForModuleBuildWithoutOutputs(
          MD, [&](CowCompilerInvocation &BuildInvocation) {
            if (any(MDC.Service.getOptimizeArgs() &
                    (ScanningOptimizations::HeaderSearch |
                     ScanningOptimizations::VFS)))
              optimizeHeaderSearchOpts(BuildInvocation.getMutHeaderSearchOpts(),
                                       *MDC.ScanInstance.getASTReader(), *MF,
                                       MDC.PrebuiltModulesASTMap,
                                       MDC.Service.getOptimizeArgs());

            if (any(MDC.Service.getOptimizeArgs() &
                    ScanningOptimizations::SystemWarnings))
              optimizeDiagnosticOpts(
                  BuildInvocation.getMutDiagnosticOpts(),
                  BuildInvocation.getFrontendOpts().IsSystemModule);

            IgnoreCWD = any(MDC.Service.getOptimizeArgs() &
                            ScanningOptimizations::IgnoreCWD) &&
                        isSafeToIgnoreCWD(BuildInvocation);
            if (IgnoreCWD) {
              llvm::ErrorOr<std::string> CWD =
                  MDC.ScanInstance.getVirtualFileSystem()
                      .getCurrentWorkingDirectory();
              if (CWD)
                optimizeCWD(BuildInvocation, *CWD);
            }
          });

  // Check provided input paths from the invocation for determining
  // IsInStableDirectories.
  if (MD.IsInStableDirectories)
    MD.IsInStableDirectories =
        areOptionsInStableDir(MDC.StableDirs, CI.getHeaderSearchOpts());

  MDC.associateWithContextHash(CI, IgnoreCWD, MD);

  // Finish the compiler invocation. Requires dependencies and the context hash.
  MDC.addOutputPaths(CI, MD);

  MD.BuildInfo = std::move(CI);

  MDC.ModularDeps.insert({M, std::move(OwnedMD)});

  return MD.ID;
}

static void forEachSubmoduleSorted(const Module *M,
                                   llvm::function_ref<void(const Module *)> F) {
  // Submodule order depends on order of header includes for inferred submodules
  // we don't care about the exact order, so sort so that it's consistent across
  // TUs to improve sharing.
  SmallVector<const Module *> Submodules(M->submodules());
  llvm::stable_sort(Submodules, [](const Module *A, const Module *B) {
    return A->Name < B->Name;
  });
  for (const Module *SubM : Submodules)
    F(SubM);
}

void ModuleDepCollectorPP::addAllSubmodulePrebuiltDeps(
    const Module *M, ModuleDeps &MD,
    llvm::DenseSet<const Module *> &SeenSubmodules) {
  addModulePrebuiltDeps(M, MD, SeenSubmodules);

  forEachSubmoduleSorted(M, [&](const Module *SubM) {
    addAllSubmodulePrebuiltDeps(SubM, MD, SeenSubmodules);
  });
}

void ModuleDepCollectorPP::addModulePrebuiltDeps(
    const Module *M, ModuleDeps &MD,
    llvm::DenseSet<const Module *> &SeenSubmodules) {
  for (const Module *Import : M->Imports)
    if (Import->getTopLevelModule() != M->getTopLevelModule())
      if (MDC.isPrebuiltModule(Import->getTopLevelModule()))
        if (SeenSubmodules.insert(Import->getTopLevelModule()).second) {
          MD.PrebuiltModuleDeps.emplace_back(Import->getTopLevelModule());
          if (MD.IsInStableDirectories) {
            auto PrebuiltModulePropIt = MDC.PrebuiltModulesASTMap.find(
                MD.PrebuiltModuleDeps.back().PCMFile);
            MD.IsInStableDirectories =
                (PrebuiltModulePropIt != MDC.PrebuiltModulesASTMap.end()) &&
                PrebuiltModulePropIt->second.isInStableDir();
          }
        }
}

void ModuleDepCollectorPP::addAllSubmoduleDeps(
    const Module *M, ModuleDeps &MD,
    llvm::DenseSet<const Module *> &AddedModules) {
  addModuleDep(M, MD, AddedModules);

  forEachSubmoduleSorted(M, [&](const Module *SubM) {
    addAllSubmoduleDeps(SubM, MD, AddedModules);
  });
}

void ModuleDepCollectorPP::addOneModuleDep(const Module *M, const ModuleID ID,
                                           ModuleDeps &MD) {
  MD.ClangModuleDeps.push_back(std::move(ID));
  if (MD.IsInStableDirectories)
    MD.IsInStableDirectories = MDC.ModularDeps[M]->IsInStableDirectories;
}

void ModuleDepCollectorPP::addModuleDep(
    const Module *M, ModuleDeps &MD,
    llvm::DenseSet<const Module *> &AddedModules) {
  for (const Module *Import : M->Imports) {
    if (Import->getTopLevelModule() != M->getTopLevelModule() &&
        !MDC.isPrebuiltModule(Import)) {
      if (auto ImportID = handleTopLevelModule(Import->getTopLevelModule()))
        if (AddedModules.insert(Import->getTopLevelModule()).second)
          addOneModuleDep(Import->getTopLevelModule(), *ImportID, MD);
    }
  }
}

void ModuleDepCollectorPP::addAllAffectingClangModules(
    const Module *M, ModuleDeps &MD,
    llvm::DenseSet<const Module *> &AddedModules) {
  addAffectingClangModule(M, MD, AddedModules);

  for (const Module *SubM : M->submodules())
    addAllAffectingClangModules(SubM, MD, AddedModules);
}

void ModuleDepCollectorPP::addAffectingClangModule(
    const Module *M, ModuleDeps &MD,
    llvm::DenseSet<const Module *> &AddedModules) {
  for (const Module *Affecting : M->AffectingClangModules) {
    assert(Affecting == Affecting->getTopLevelModule() &&
           "Not quite import not top-level module");
    if (Affecting != M->getTopLevelModule() &&
        !MDC.isPrebuiltModule(Affecting)) {
      if (auto ImportID = handleTopLevelModule(Affecting))
        if (AddedModules.insert(Affecting).second)
          addOneModuleDep(Affecting, *ImportID, MD);
    }
  }
}

ModuleDepCollector::ModuleDepCollector(
    DependencyScanningService &Service,
    std::unique_ptr<DependencyOutputOptions> Opts,
    CompilerInstance &ScanInstance, DependencyConsumer &C,
    DependencyActionController &Controller, CompilerInvocation OriginalCI,
    const PrebuiltModulesAttrsMap PrebuiltModulesASTMap,
    const ArrayRef<StringRef> StableDirs)
    : Service(Service), ScanInstance(ScanInstance), Consumer(C),
      Controller(Controller),
      PrebuiltModulesASTMap(std::move(PrebuiltModulesASTMap)),
      StableDirs(StableDirs), Opts(std::move(Opts)),
      CommonInvocation(
          makeCommonInvocationForModuleBuild(std::move(OriginalCI))) {}

void ModuleDepCollector::attachToPreprocessor(Preprocessor &PP) {
  PP.addPPCallbacks(std::make_unique<ModuleDepCollectorPP>(*this));
}

void ModuleDepCollector::attachToASTReader(ASTReader &R) {}

bool ModuleDepCollector::isPrebuiltModule(const Module *M) {
  std::string Name(M->getTopLevelModuleName());
  const auto &PrebuiltModuleFiles =
      ScanInstance.getHeaderSearchOpts().PrebuiltModuleFiles;
  auto PrebuiltModuleFileIt = PrebuiltModuleFiles.find(Name);
  if (PrebuiltModuleFileIt == PrebuiltModuleFiles.end())
    return false;
  assert("Prebuilt module came from the expected AST file" &&
         PrebuiltModuleFileIt->second == M->getASTFile()->getName());
  return true;
}

void ModuleDepCollector::addVisibleModules() {
  llvm::DenseSet<const Module *> ImportedModules;
  auto InsertVisibleModules = [&](const Module *M) {
    if (ImportedModules.contains(M))
      return;

    VisibleModules.insert(M->getTopLevelModuleName());
    SmallVector<Module *> Stack;
    M->getExportedModules(Stack);
    while (!Stack.empty()) {
      const Module *CurrModule = Stack.pop_back_val();
      if (ImportedModules.contains(CurrModule))
        continue;
      ImportedModules.insert(CurrModule);
      VisibleModules.insert(CurrModule->getTopLevelModuleName());
      CurrModule->getExportedModules(Stack);
    }
  };

  for (const Module *Import : DirectImports)
    InsertVisibleModules(Import);
}

static StringRef makeAbsoluteAndPreferred(CompilerInstance &CI, StringRef Path,
                                          SmallVectorImpl<char> &Storage) {
  if (llvm::sys::path::is_absolute(Path) &&
      !llvm::sys::path::is_style_windows(llvm::sys::path::Style::native))
    return Path;
  Storage.assign(Path.begin(), Path.end());
  CI.getFileManager().makeAbsolutePath(Storage);
  llvm::sys::path::make_preferred(Storage);
  return StringRef(Storage.data(), Storage.size());
}

void ModuleDepCollector::addFileDep(StringRef Path) {
  if (Service.getFormat() == ScanningOutputFormat::P1689) {
    // Within P1689 format, we don't want all the paths to be absolute path
    // since it may violate the traditional make style dependencies info.
    FileDeps.emplace_back(Path);
    return;
  }

  llvm::SmallString<256> Storage;
  Path = makeAbsoluteAndPreferred(ScanInstance, Path, Storage);
  FileDeps.emplace_back(Path);
}

void ModuleDepCollector::addFileDep(ModuleDeps &MD, StringRef Path) {
  MD.FileDeps.emplace_back(Path);
}
