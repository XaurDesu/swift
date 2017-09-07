//===--- Frontend.cpp - frontend utility methods --------------------------===//
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
// This file contains utility methods for parsing and performing semantic
// on modules.
//
//===----------------------------------------------------------------------===//

#include "swift/Frontend/Frontend.h"
#include "swift/Subsystems.h"
#include "swift/Strings.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/Module.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "swift/Parse/DelayedParsingCallbacks.h"
#include "swift/Parse/Lexer.h"
#include "swift/SIL/SILModule.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using namespace swift;

std::string CompilerInvocation::getPCHHash() const {
  using llvm::hash_code;
  using llvm::hash_value;
  using llvm::hash_combine;

  auto Code = hash_value(LangOpts.getPCHHashComponents());
  Code = hash_combine(Code, FrontendOpts.getPCHHashComponents());
  Code = hash_combine(Code, ClangImporterOpts.getPCHHashComponents());
  Code = hash_combine(Code, SearchPathOpts.getPCHHashComponents());
  Code = hash_combine(Code, DiagnosticOpts.getPCHHashComponents());
  Code = hash_combine(Code, SILOpts.getPCHHashComponents());
  Code = hash_combine(Code, IRGenOpts.getPCHHashComponents());

  return llvm::APInt(64, Code).toString(36, /*Signed=*/false);
}

void CompilerInstance::createSILModule() {
  assert(MainModule && "main module not created yet");
  // Assume WMO, if a -primary-file option was not provided.
  bool WholeModule = !Invocation.getFrontendOptions().PrimaryInput.hasValue();
  TheSILModule = SILModule::createEmptyModule(
    getMainModule(), Invocation.getSILOptions(), WholeModule);
}

void CompilerInstance::setPrimarySourceFile(SourceFile *SF) {
  assert(SF);
  assert(MainModule && "main module not created yet");
  assert(!PrimarySourceFile && "already has a primary source file");
  assert(PrimaryBufferID == NO_SUCH_BUFFER || !SF->getBufferID().hasValue() ||
         SF->getBufferID().getValue() == PrimaryBufferID);
  PrimarySourceFile = SF;
  PrimarySourceFile->setReferencedNameTracker(NameTracker);
}

bool CompilerInstance::setup(const CompilerInvocation &Invok) {
  Invocation = Invok;

  // Honor -Xllvm.
  if (!Invok.getFrontendOptions().LLVMArgs.empty()) {
    llvm::SmallVector<const char *, 4> Args;
    Args.push_back("swift (LLVM option parsing)");
    for (unsigned i = 0, e = Invok.getFrontendOptions().LLVMArgs.size(); i != e;
         ++i)
      Args.push_back(Invok.getFrontendOptions().LLVMArgs[i].c_str());
    Args.push_back(nullptr);
    llvm::cl::ParseCommandLineOptions(Args.size()-1, Args.data());
  }

  if (Invocation.getDiagnosticOptions().ShowDiagnosticsAfterFatalError) {
    Diagnostics.setShowDiagnosticsAfterFatalError();
  }
  if (Invocation.getDiagnosticOptions().SuppressWarnings) {
    Diagnostics.setSuppressWarnings(true);
  }
  if (Invocation.getDiagnosticOptions().WarningsAsErrors) {
    Diagnostics.setWarningsAsErrors(true);
  }

  // If we are asked to emit a module documentation file, configure lexing and
  // parsing to remember comments.
  if (!Invocation.getFrontendOptions().ModuleDocOutputPath.empty())
    Invocation.getLangOptions().AttachCommentsToDecls = true;

  // If we are doing index-while-building, configure lexing and parsing to
  // remember comments.
  if (!Invocation.getFrontendOptions().IndexStorePath.empty()) {
    Invocation.getLangOptions().AttachCommentsToDecls = true;
  }

  Context.reset(new ASTContext(Invocation.getLangOptions(),
                               Invocation.getSearchPathOptions(),
                               SourceMgr, Diagnostics));

  if (Invocation.getFrontendOptions().EnableSourceImport) {
    bool immediate = Invocation.getFrontendOptions().actionIsImmediate();
    bool enableResilience = Invocation.getFrontendOptions().EnableResilience;
    Context->addModuleLoader(SourceLoader::create(*Context,
                                                  !immediate,
                                                  enableResilience,
                                                  DepTracker));
  }
  
  auto SML = SerializedModuleLoader::create(*Context, DepTracker);
  this->SML = SML.get();
  Context->addModuleLoader(std::move(SML));

  // Wire up the Clang importer. If the user has specified an SDK, use it.
  // Otherwise, we just keep it around as our interface to Clang's ABI
  // knowledge.
  auto clangImporter =
    ClangImporter::create(*Context, Invocation.getClangImporterOptions(),
                          Invocation.getPCHHash(),
                          DepTracker);
  if (!clangImporter) {
    Diagnostics.diagnose(SourceLoc(), diag::error_clang_importer_create_fail);
    return true;
  }

  Context->addModuleLoader(std::move(clangImporter), /*isClang*/true);

  assert(Lexer::isIdentifier(Invocation.getModuleName()));

  Optional<unsigned> CodeCompletionBufferID;
  auto CodeCompletePoint = Invocation.getCodeCompletionPoint();
  if (CodeCompletePoint.first) {
    auto MemBuf = CodeCompletePoint.first;
    // CompilerInvocation doesn't own the buffers, copy to a new buffer.
    CodeCompletionBufferID = SourceMgr.addMemBufferCopy(MemBuf);
    BufferIDs.push_back(*CodeCompletionBufferID);
    SourceMgr.setCodeCompletionPoint(*CodeCompletionBufferID,
                                     CodeCompletePoint.second);
  }

  bool MainMode = (Invocation.getInputKind() == InputFileKind::IFK_Swift);
  bool SILMode = (Invocation.getInputKind() == InputFileKind::IFK_SIL);

  if (SILMode)
    Invocation.getLangOptions().EnableAccessControl = false;

  const Optional<SelectedInput> &PrimaryInput =
    Invocation.getFrontendOptions().PrimaryInput;

  // Add the memory buffers first, these will be associated with a filename
  // and they can replace the contents of an input filename.
  for (unsigned i = 0, e = Invocation.getInputBuffers().size(); i != e; ++i) {
    // CompilerInvocation doesn't own the buffers, copy to a new buffer.
    auto *InputBuffer = Invocation.getInputBuffers()[i];
    auto Copy = std::unique_ptr<llvm::MemoryBuffer>(
        llvm::MemoryBuffer::getMemBufferCopy(
            InputBuffer->getBuffer(), InputBuffer->getBufferIdentifier()));
    if (serialization::isSerializedAST(Copy->getBuffer())) {
      PartialModules.push_back({ std::move(Copy), nullptr });
    } else {
      unsigned BufferID = SourceMgr.addNewSourceBuffer(std::move(Copy));
      BufferIDs.push_back(BufferID);

      if (SILMode)
        MainBufferID = BufferID;

      if (PrimaryInput && PrimaryInput->isBuffer() && PrimaryInput->Index == i)
        PrimaryBufferID = BufferID;
    }
  }

  for (unsigned i = 0, e = Invocation.getInputFilenames().size(); i != e; ++i) {
    auto &File = Invocation.getInputFilenames()[i];

    // FIXME: Working with filenames is fragile, maybe use the real path
    // or have some kind of FileManager.
    using namespace llvm::sys::path;
    if (Optional<unsigned> ExistingBufferID =
            SourceMgr.getIDForBufferIdentifier(File)) {
      if (SILMode || (MainMode && filename(File) == "main.swift"))
        MainBufferID = ExistingBufferID.getValue();

      if (PrimaryInput && PrimaryInput->isFilename() &&
          PrimaryInput->Index == i)
        PrimaryBufferID = ExistingBufferID.getValue();

      continue; // replaced by a memory buffer.
    }

    // Open the input file.
    using FileOrError = llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>;
    FileOrError InputFileOrErr = llvm::MemoryBuffer::getFileOrSTDIN(File);
    if (!InputFileOrErr) {
      Diagnostics.diagnose(SourceLoc(), diag::error_open_input_file,
                           File, InputFileOrErr.getError().message());
      return true;
    }

    if (serialization::isSerializedAST(InputFileOrErr.get()->getBuffer())) {
      llvm::SmallString<128> ModuleDocFilePath(File);
      llvm::sys::path::replace_extension(ModuleDocFilePath,
                                         SERIALIZED_MODULE_DOC_EXTENSION);
      FileOrError ModuleDocOrErr =
        llvm::MemoryBuffer::getFileOrSTDIN(ModuleDocFilePath.str());
      if (!ModuleDocOrErr &&
          ModuleDocOrErr.getError() != std::errc::no_such_file_or_directory) {
        Diagnostics.diagnose(SourceLoc(), diag::error_open_input_file,
                             File, ModuleDocOrErr.getError().message());
        return true;
      }
      PartialModules.push_back({ std::move(InputFileOrErr.get()),
                                 ModuleDocOrErr? std::move(ModuleDocOrErr.get())
                                               : nullptr });
      continue;
    }

    // Transfer ownership of the MemoryBuffer to the SourceMgr.
    unsigned BufferID =
      SourceMgr.addNewSourceBuffer(std::move(InputFileOrErr.get()));

    BufferIDs.push_back(BufferID);

    if (SILMode || (MainMode && filename(File) == "main.swift"))
      MainBufferID = BufferID;

    if (PrimaryInput && PrimaryInput->isFilename() && PrimaryInput->Index == i)
      PrimaryBufferID = BufferID;
  }

  // Set the primary file to the code-completion point if one exists.
  if (CodeCompletionBufferID.hasValue())
    PrimaryBufferID = *CodeCompletionBufferID;

  if (MainMode && MainBufferID == NO_SUCH_BUFFER && BufferIDs.size() == 1)
    MainBufferID = BufferIDs.front();

  return false;
}

ModuleDecl *CompilerInstance::getMainModule() {
  if (!MainModule) {
    Identifier ID = Context->getIdentifier(Invocation.getModuleName());
    MainModule = ModuleDecl::create(ID, *Context);
    if (Invocation.getFrontendOptions().EnableTesting)
      MainModule->setTestingEnabled();

    if (Invocation.getFrontendOptions().EnableResilience)
      MainModule->setResilienceStrategy(ResilienceStrategy::Resilient);
    else if (Invocation.getSILOptions().SILSerializeAll)
      MainModule->setResilienceStrategy(ResilienceStrategy::Fragile);
  }
  return MainModule;
}

static void
addAdditionalInitialImportsTo(SourceFile *SF, CompilerInstance::ImplicitImports &implicitImports) {
  if (!implicitImports.objCModuleUnderlyingMixedFramework && !implicitImports.headerModule &&
      implicitImports.modules.empty())
    return;

  using ImportPair =
      std::pair<ModuleDecl::ImportedModule, SourceFile::ImportOptions>;
  SmallVector<ImportPair, 4> additionalImports;

  if (implicitImports.objCModuleUnderlyingMixedFramework)
    additionalImports.push_back(
        {{/*accessPath=*/{}, implicitImports.objCModuleUnderlyingMixedFramework},
         SourceFile::ImportFlags::Exported});
  if (implicitImports.headerModule)
    additionalImports.push_back({{/*accessPath=*/{}, implicitImports.headerModule},
                                 SourceFile::ImportFlags::Exported});
  if (!implicitImports.modules.empty()) {
    for (auto &importModule : implicitImports.modules) {
      additionalImports.push_back({{/*accessPath=*/{}, importModule}, {}});
    }
  }

  SF->addImports(additionalImports);
}

/// Implicitly import the SwiftOnoneSupport module in non-optimized
/// builds. This allows for use of popular specialized functions
/// from the standard library, which makes the non-optimized builds
/// execute much faster.
static bool shouldImplicityImportSwiftOnoneSupportModule(CompilerInvocation &Invocation) {
  if (Invocation.getImplicitModuleImportKind() != SourceFile::ImplicitModuleImportKind::Stdlib)
    return false;
  SILOptions::SILOptMode optimization = Invocation.getSILOptions().Optimization;
  if (optimization <= SILOptions::SILOptMode::None  &&
      Invocation.getFrontendOptions().shouldImportSwiftOnoneModuleIfNoneOrImplicitOptimization()) {
      return true;
  }
  return optimization == SILOptions::SILOptMode::None  &&  Invocation.getFrontendOptions().isCreatingSIL();
}

void CompilerInstance::performSema() {
  SharedTimer timer("performSema");
  Context->LoadedModules[MainModule->getName()] = getMainModule();
  
  if (Invocation.getInputKind() == InputFileKind::IFK_SIL) {
    assert(BufferIDs.size() == 1);
    assert(MainBufferID != NO_SUCH_BUFFER);
    createSILModule();
  }
  
  if (Invocation.getImplicitModuleImportKind() == SourceFile::ImplicitModuleImportKind::Stdlib) {
    if (!loadStdlib())
      return;
  }
  if (shouldImplicityImportSwiftOnoneSupportModule(Invocation)) {
    Invocation.getFrontendOptions().ImplicitImportModuleNames.push_back(SWIFT_ONONE_SUPPORT);
  }
  
  ImplicitImports implicitImports(*this);
  
  if (Invocation.getInputKind() == InputFileKind::IFK_Swift_REPL) {
    createREPLFileWithImports(implicitImports);
    return;
  }

  std::unique_ptr<DelayedParsingCallbacks> DelayedCB(
      computeDelayedParsingCallback());

  PersistentParserState PersistentState;

  // Make sure the main file is the first file in the module, so do this now.
  createMainFileWithImportsAndAddToModule(implicitImports);

  bool hadLoadError = parsePartialModulesAndLibraryFiles(implicitImports, PersistentState, DelayedCB.get());
  if (hadLoadError)
    return;

  // Parse main file last in order to make sure that it can use decls from other
  // files in the module.
  checkTypesWhileParsingMain(PersistentState, DelayedCB.get());
}

CompilerInstance::ImplicitImports::ImplicitImports(CompilerInstance &compiler) {
  kind = compiler.Invocation.getImplicitModuleImportKind();

  
  objCModuleUnderlyingMixedFramework =
  compiler.Invocation.getFrontendOptions().ImportUnderlyingModule
  ? compiler.importUnderlyingModule()
  : nullptr;
  
  compiler.fillInModulesToImportFromImplicitImportModuleNames(modules);
  
  headerModule = compiler.importBridgingHeader();
}

// Return true if should continue, i.e. no error
bool CompilerInstance::loadStdlib() {
  SharedTimer timer("performSema-loadStdlib");
  ModuleDecl *M = Context->getStdlibModule(true);

  if (!M) {
    Diagnostics.diagnose(SourceLoc(), diag::error_stdlib_not_found,
                         Invocation.getTargetTriple());
    return false;
  }

  // If we failed to load, we should have already diagnosed
  if (M->failedToLoad()) {
    assert(Diagnostics.hadAnyError() &&
           "Module failed to load but nothing was diagnosed?");
    return false;
  }
  return true;
}

ModuleDecl *
CompilerInstance::importUnderlyingModule() {
  SharedTimer timer("performSema-importUnderlyingModule");
  ModuleDecl *objCModuleUnderlyingMixedFramework = static_cast<ClangImporter *>(Context->getClangModuleLoader())
    ->loadModule(SourceLoc(), std::make_pair(MainModule->getName(), SourceLoc()));
  if (objCModuleUnderlyingMixedFramework)
    return objCModuleUnderlyingMixedFramework;
  Diagnostics.diagnose(SourceLoc(), diag::error_underlying_module_not_found,
                       MainModule->getName());
  return nullptr;
}

ModuleDecl *
CompilerInstance::importBridgingHeader() {
  SharedTimer timer("performSema-importBridgingHeader");
  const StringRef &implicitHeaderPath =
      Invocation.getFrontendOptions().ImplicitObjCHeaderPath;
  auto clangImporter = static_cast<ClangImporter *>(Context->getClangModuleLoader());
  if (implicitHeaderPath.empty() ||
      clangImporter->importBridgingHeader(implicitHeaderPath, MainModule))
    return nullptr;
  ModuleDecl *importedHeaderModule = clangImporter->getImportedHeaderModule();
  assert(importedHeaderModule);
  return importedHeaderModule;
}

void CompilerInstance::fillInModulesToImportFromImplicitImportModuleNames(
    SmallVectorImpl<ModuleDecl *> &importModules) {
  SharedTimer timer("performSema-fillInModulesToImportFromImplicitImportModuleNames");
  for (auto &ImplicitImportModuleName :
       Invocation.getFrontendOptions().ImplicitImportModuleNames) {
    if (Lexer::isIdentifier(ImplicitImportModuleName)) {
      auto moduleID = Context->getIdentifier(ImplicitImportModuleName);
      ModuleDecl *importModule =
          Context->getModule(std::make_pair(moduleID, SourceLoc()));
      if (importModule) {
        importModules.push_back(importModule);
      } else {
        Diagnostics.diagnose(SourceLoc(), diag::sema_no_import,
                             ImplicitImportModuleName);
        if (Invocation.getSearchPathOptions().SDKPath.empty() &&
            llvm::Triple(llvm::sys::getProcessTriple()).isMacOSX()) {
          Diagnostics.diagnose(SourceLoc(), diag::sema_no_import_no_sdk);
          Diagnostics.diagnose(SourceLoc(), diag::sema_no_import_no_sdk_xcrun);
        }
      }
    } else {
      Diagnostics.diagnose(SourceLoc(), diag::error_bad_module_name,
                           ImplicitImportModuleName, false);
    }
  }
}

void CompilerInstance::createREPLFileWithImports( ImplicitImports &implicitImports) {
  SharedTimer timer("performSema-createREPLFileWithImports");
  auto *SingleInputFile =
  new (*Context) SourceFile(*MainModule, Invocation.getSourceFileKind(),
                            None, implicitModuleImportKind, Invocation.getLangOptions().KeepTokensInSourceFile);
  MainModule->addFile(*SingleInputFile);
  addAdditionalInitialImportsTo(SingleInputFile, implicitImports);
}

DelayedParsingCallbacks *CompilerInstance::computeDelayedParsingCallback() {
  SharedTimer timer("performSema-computeDelayedParsingCallback");
  if (Invocation.isCodeCompletion())
    return new CodeCompleteDelayedCallbacks(SourceMgr.getCodeCompletionLoc());
  if (Invocation.isDelayedFunctionBodyParsing()) {
    return new AlwaysDelayedCallbacks;
  }
  return nullptr;
}

// The main file may only be
// a source file, or it may be a SIL file, which requires pumping the parser.

void CompilerInstance::createMainFileWithImportsAndAddToModule(ImplicitImports &implicitImports) {
  SharedTimer timer("performSema-createMainFileWithImportsAndAddToModule");
  if (MainBufferID == NO_SUCH_BUFFER)
    return;

  const InputFileKind Kind = Invocation.getInputKind();
  assert(Kind == InputFileKind::IFK_Swift || Kind == InputFileKind::IFK_SIL);

  if (Kind == InputFileKind::IFK_Swift)
    SourceMgr.setHashbangBufferID(MainBufferID);
  
  auto *MainFile = new (*Context) SourceFile(*MainModule,
                                             Invocation.getSourceFileKind(),
                                             MainBufferID, implicitModuleImportKind, Invocation.getLangOptions().KeepTokensInSourceFile);
  MainModule->addFile(*MainFile);
  addAdditionalInitialImportsTo(MainFile, implicitImports);

  if (MainBufferID == PrimaryBufferID)
    setPrimarySourceFile(MainFile);
}

void CompilerInstance::parseALibraryFile(
    unsigned BufferID,
    ImplicitImports &implicitImports,
    PersistentParserState &PersistentState,
    DelayedParsingCallbacks *DelayedParseCB) {
  SharedTimer timer("performSema-parseALibraryFile");

  auto *NextInput = new (*Context) SourceFile(
      *MainModule, SourceFileKind::Library, BufferID, implicitImports.kind, Invocation.getLangOptions().KeepTokensInSourceFile);
  MainModule->addFile(*NextInput);
  addAdditionalInitialImportsTo(NextInput, implicitImports);

  if (BufferID == PrimaryBufferID)
    setPrimarySourceFile(NextInput);

  auto &Diags = NextInput->getASTContext().Diags;
  auto DidSuppressWarnings = Diags.getSuppressWarnings();
  auto IsPrimary =
      generateOutputForTheWholeModule() || BufferID == PrimaryBufferID;
  Diags.setSuppressWarnings(DidSuppressWarnings || !IsPrimary);

  bool Done;
  do {
    // Parser may stop at some erroneous constructions like #else, #endif
    // or '}' in some cases, continue parsing until we are done
    parseIntoSourceFile(*NextInput, BufferID, &Done, nullptr, &PersistentState,
                        DelayedParseCB);
  } while (!Done);

  Diags.setSuppressWarnings(DidSuppressWarnings);

  performNameBinding(*NextInput);
}

OptionSet<TypeCheckingFlags> CompilerInstance::computeTypeCheckingOptions() {
  OptionSet<TypeCheckingFlags> TypeCheckOptions;
  if (generateOutputForTheWholeModule()) {
    TypeCheckOptions |= TypeCheckingFlags::DelayWholeModuleChecking;
  }
  const auto &options = Invocation.getFrontendOptions();
  if (options.DebugTimeFunctionBodies) {
    TypeCheckOptions |= TypeCheckingFlags::DebugTimeFunctionBodies;
  }
  if (options.actionIsImmediate()) {
    TypeCheckOptions |= TypeCheckingFlags::ForImmediateMode;
  }
  if (options.DebugTimeExpressionTypeChecking) {
    TypeCheckOptions |= TypeCheckingFlags::DebugTimeExpressions;
  }
  return TypeCheckOptions;
}

bool CompilerInstance::parsePartialModulesAndLibraryFiles(
    ImplicitImports &implicitImports,
    PersistentParserState &PersistentState,
    DelayedParsingCallbacks *DelayedParseCB) {
  SharedTimer timer("performSema-parsePartialModulesAndLibraryFiles");
  bool hadLoadError = false;
  // Parse all the partial modules first.
  for (auto &PM : PartialModules) {
    assert(PM.ModuleBuffer);
    if (!SML->loadAST(*MainModule, SourceLoc(), std::move(PM.ModuleBuffer),
                      std::move(PM.ModuleDocBuffer)))
      hadLoadError = true;
  }

  // Then parse all the library files.
  for (auto BufferID : BufferIDs) {
    if (BufferID != MainBufferID) {
      parseALibraryFile(BufferID, implicitImports, PersistentState, DelayedParseCB);
    }
  }
  if (Invocation.isCodeCompletion()) {
    // When we are doing code completion, make sure to emit at least one
    // diagnostic, so that ASTContext is marked as erroneous.  In this case
    // various parts of the compiler (for example, AST verifier) have less
    // strict assumptions about the AST.
    Diagnostics.diagnose(SourceLoc(), diag::error_doing_code_completion);
  }
  return hadLoadError;
}

void CompilerInstance::checkTypesWhileParsingMain(
    PersistentParserState &PersistentState,
    DelayedParsingCallbacks *DelayedParseCB) {
  SharedTimer timer("performSema-checkTypesWhileParsingMain");
  OptionSet<TypeCheckingFlags> TypeCheckOptions = computeTypeCheckingOptions();

  // Parse the main file last.
  if (MainBufferID != NO_SUCH_BUFFER) {
    parseAndTypeCheckTheMainFile(PersistentState, DelayedParseCB,
                                 TypeCheckOptions);
  }

  if (generateOutputForTheWholeModule())
    typeCheckEveryFileInMainModule(PersistentState, TypeCheckOptions);
  else if (PrimarySourceFile != nullptr)
    typeCheckThePrimaryFile(PersistentState, TypeCheckOptions);

  // Even if there were no source files, we should still record known
  // protocols.
  if (auto *stdlib = Context->getStdlibModule())
    Context->recordKnownProtocols(stdlib);

  if (DelayedParseCB) {
    performDelayedParsing(MainModule, PersistentState,
                          Invocation.getCodeCompletionFactory());
  }
  typeCheckMainModule(TypeCheckOptions);
}

void CompilerInstance::parseAndTypeCheckTheMainFile(
    PersistentParserState &PersistentState,
    DelayedParsingCallbacks *DelayedParseCB,
    const OptionSet<TypeCheckingFlags> TypeCheckOptions) {
  SharedTimer timer("performSema-checkTypesWhileParsingMain-parseAndTypeCheckTheMainFile");
  bool mainIsPrimary =
      (generateOutputForTheWholeModule() || MainBufferID == PrimaryBufferID);

  SourceFile &MainFile =
      MainModule->getMainSourceFile(Invocation.getSourceFileKind());

  auto &Diags = MainFile.getASTContext().Diags;
  auto DidSuppressWarnings = Diags.getSuppressWarnings();
  Diags.setSuppressWarnings(DidSuppressWarnings || !mainIsPrimary);

  SILParserState SILContext(TheSILModule.get());
  unsigned CurTUElem = 0;
  bool Done;
  do {
    // Pump the parser multiple times if necessary.  It will return early
    // after parsing any top level code in a main module, or in SIL mode when
    // there are chunks of swift decls (e.g. imports and types) interspersed
    // with 'sil' definitions.
    parseIntoSourceFile(MainFile, MainFile.getBufferID().getValue(), &Done,
                        TheSILModule ? &SILContext : nullptr, &PersistentState,
                        DelayedParseCB);
    if (mainIsPrimary) {
      performTypeChecking(MainFile, PersistentState.getTopLevelContext(),
                          TypeCheckOptions, CurTUElem,
                          options.WarnLongFunctionBodies,
                          options.WarnLongExpressionTypeChecking,
                          options.SolverExpressionTimeThreshold);
    }
    CurTUElem = MainFile.Decls.size();
  } while (!Done);

  Diags.setSuppressWarnings(DidSuppressWarnings);

  if (mainIsPrimary && !Context->hadError() &&
      Invocation.getFrontendOptions().PCMacro) {
    performPCMacro(MainFile, PersistentState.getTopLevelContext());
  }

  // Playground transform knows to look out for PCMacro's changes and not
  // to playground log them.
  if (mainIsPrimary && !Context->hadError() &&
      Invocation.getFrontendOptions().PlaygroundTransform)
    performPlaygroundTransform(
        MainFile, Invocation.getFrontendOptions().PlaygroundHighPerformance);
  if (!mainIsPrimary) {
    performNameBinding(MainFile);
  }
}

void CompilerInstance::typeCheckEveryFileInMainModule(
                                                      PersistentParserState &PersistentState,
                                                      const OptionSet<TypeCheckingFlags> TypeCheckOptions) {
  for (auto File : MainModule->getFiles()) {
    if (auto SF = dyn_cast<SourceFile>(File)) {
      performTypeChecking(*SF, PersistentState.getTopLevelContext(),
                          TypeCheckOptions, /*curElem*/ 0,
                          options.WarnLongFunctionBodies,
                          options.WarnLongExpressionTypeChecking,
                          options.SolverExpressionTimeThreshold);
    }
  }
}
void CompilerInstance::typeCheckThePrimaryFile(
                                               PersistentParserState &PersistentState,
                                               const OptionSet<TypeCheckingFlags> TypeCheckOptions) {
  assert (PrimarySourceFile != nullptr  &&  "No primary file?");
  performTypeChecking(*PrimarySourceFile, PersistentState.getTopLevelContext(),
                      TypeCheckOptions, /*curElem*/ 0,
                      options.WarnLongFunctionBodies,
                      options.WarnLongExpressionTypeChecking,
                      options.SolverExpressionTimeThreshold);
}



void CompilerInstance::typeCheckMainModule(
    OptionSet<TypeCheckingFlags> TypeCheckOptions) {
  if (TypeCheckOptions & TypeCheckingFlags::DelayWholeModuleChecking) {
    for (auto File : MainModule->getFiles())
      if (auto SF = dyn_cast<SourceFile>(File))
        performWholeModuleTypeChecking(*SF);

  }
  if (generateOutputForTheWholeModule()) {
    for (auto File : MainModule->getFiles())
      if (auto SF = dyn_cast<SourceFile>(File))
        finishTypeChecking(*SF);
  }
  else {
    finishTypeChecking(*PrimarySourceFile);
  }
}

void CompilerInstance::performParseOnly(bool EvaluateConditionals) {
  const InputFileKind Kind = Invocation.getInputKind();
  ModuleDecl *MainModule = getMainModule();
  Context->LoadedModules[MainModule->getName()] = MainModule;
  bool KeepTokens = Invocation.getLangOptions().KeepTokensInSourceFile;

  assert((Kind == InputFileKind::IFK_Swift ||
          Kind == InputFileKind::IFK_Swift_Library) &&
         "only supports parsing .swift files");
  (void)Kind;

  auto implicitModuleImportKind = SourceFile::ImplicitModuleImportKind::None;

  // Make sure the main file is the first file in the module but parse it last,
  // to match the parsing logic used when performing Sema.
  if (MainBufferID != NO_SUCH_BUFFER) {
    assert(Kind == InputFileKind::IFK_Swift);
    SourceMgr.setHashbangBufferID(MainBufferID);

    auto *MainFile = new (*Context) SourceFile(
        *MainModule, Invocation.getSourceFileKind(), MainBufferID, implicitModuleImportKind, KeepTokens);
    MainModule->addFile(*MainFile);

    if (MainBufferID == PrimaryBufferID)
      setPrimarySourceFile(MainFile);
  }

  PersistentParserState PersistentState;
  PersistentState.PerformConditionEvaluation = EvaluateConditionals;
  // Parse all the library files.
  for (auto BufferID : BufferIDs) {
    if (BufferID == MainBufferID)
      continue;

    auto *NextInput = new (*Context)
        SourceFile(*MainModule, SourceFileKind::Library, BufferID, implicitModuleImportKind, KeepTokens);
    MainModule->addFile(*NextInput);
    if (BufferID == PrimaryBufferID)
      setPrimarySourceFile(NextInput);

    bool Done;
    do {
      // Parser may stop at some erroneous constructions like #else, #endif
      // or '}' in some cases, continue parsing until we are done
      parseIntoSourceFile(*NextInput, BufferID, &Done, nullptr,
                          &PersistentState, nullptr);
    } while (!Done);
  }

  // Now parse the main file.
  if (MainBufferID != NO_SUCH_BUFFER) {
    SourceFile &MainFile =
        MainModule->getMainSourceFile(Invocation.getSourceFileKind());

    bool Done;
    do {
      parseIntoSourceFile(MainFile, MainFile.getBufferID().getValue(), &Done,
                          nullptr, &PersistentState, nullptr);
    } while (!Done);
  }

  assert(Context->LoadedModules.size() == 1 &&
         "Loaded a module during parse-only");
}

void CompilerInstance::freeContextAndSIL() {
  Context.reset();
  TheSILModule.reset();
  MainModule = nullptr;
  SML = nullptr;
  PrimarySourceFile = nullptr;
}
