//===-- SourcePrinter.cpp -  source interleaving utilities ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the LiveElementPrinter and SourcePrinter classes to
// keep track of DWARF info as the current address is updated, and print out the
// source file line and variable or inlined function liveness as needed.
//
//===----------------------------------------------------------------------===//

#include "SourcePrinter.h"
#include "llvm-objdump.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/DebugInfo/DWARF/DWARFExpressionPrinter.h"
#include "llvm/DebugInfo/DWARF/LowLevel/DWARFExpression.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/FormatVariadic.h"

#define DEBUG_TYPE "objdump"

namespace llvm {
namespace objdump {

bool InlinedFunction::liveAtAddress(object::SectionedAddress Addr) const {
  if (!Range.valid())
    return false;

  return Range.LowPC <= Addr.Address && Range.HighPC > Addr.Address;
}

void InlinedFunction::print(raw_ostream &OS, const MCRegisterInfo &MRI) const {
  const char *MangledCallerName = FuncDie.getName(DINameKind::LinkageName);
  if (!MangledCallerName)
    return;

  if (Demangle)
    OS << "inlined into " << demangle(MangledCallerName);
  else
    OS << "inlined into " << MangledCallerName;
}

void InlinedFunction::dump(raw_ostream &OS) const {
  OS << Name << " @ " << Range << ": ";
}

void InlinedFunction::printElementLine(raw_ostream &OS,
                                       object::SectionedAddress Addr,
                                       bool IsEnd) const {
  bool LiveIn = !IsEnd && Range.LowPC == Addr.Address;
  bool LiveOut = IsEnd && Range.HighPC == Addr.Address;
  if (!(LiveIn || LiveOut))
    return;

  uint32_t CallFile, CallLine, CallColumn, CallDiscriminator;
  InlinedFuncDie.getCallerFrame(CallFile, CallLine, CallColumn,
                                CallDiscriminator);
  const DWARFDebugLine::LineTable *LineTable =
      Unit->getContext().getLineTableForUnit(Unit);
  std::string FileName;
  if (!LineTable->hasFileAtIndex(CallFile))
    return;
  if (!LineTable->getFileNameByIndex(
          CallFile, Unit->getCompilationDir(),
          DILineInfoSpecifier::FileLineInfoKind::AbsoluteFilePath, FileName))
    return;

  if (FileName.empty())
    return;

  const char *MangledCallerName = FuncDie.getName(DINameKind::LinkageName);
  if (!MangledCallerName)
    return;

  std::string CallerName = MangledCallerName;
  std::string CalleeName = Name;
  if (Demangle) {
    CallerName = demangle(MangledCallerName);
    CalleeName = demangle(Name);
  }

  OS << "; " << FileName << ":" << CallLine << ":" << CallColumn << ": ";
  if (IsEnd)
    OS << "end of ";
  OS << CalleeName << " inlined into " << CallerName << "\n";
}

bool LiveVariable::liveAtAddress(object::SectionedAddress Addr) const {
  if (LocExpr.Range == std::nullopt)
    return false;
  return LocExpr.Range->SectionIndex == Addr.SectionIndex &&
         LocExpr.Range->LowPC <= Addr.Address &&
         LocExpr.Range->HighPC > Addr.Address;
}

void LiveVariable::print(raw_ostream &OS, const MCRegisterInfo &MRI) const {
  DataExtractor Data({LocExpr.Expr.data(), LocExpr.Expr.size()},
                     Unit->getContext().isLittleEndian(), 0);
  DWARFExpression Expression(Data, Unit->getAddressByteSize());

  auto GetRegName = [&MRI, &OS](uint64_t DwarfRegNum, bool IsEH) -> StringRef {
    if (std::optional<MCRegister> LLVMRegNum =
            MRI.getLLVMRegNum(DwarfRegNum, IsEH))
      if (const char *RegName = MRI.getName(*LLVMRegNum))
        return StringRef(RegName);
    OS << "<unknown register " << DwarfRegNum << ">";
    return {};
  };

  printDwarfExpressionCompact(&Expression, OS, GetRegName);
}

void LiveVariable::dump(raw_ostream &OS) const {
  OS << Name << " @ " << LocExpr.Range << ": ";
}

void LiveElementPrinter::addInlinedFunction(DWARFDie FuncDie,
                                            DWARFDie InlinedFuncDie) {
  uint64_t FuncLowPC, FuncHighPC, SectionIndex;
  if (!InlinedFuncDie.getLowAndHighPC(FuncLowPC, FuncHighPC, SectionIndex))
    return;

  DWARFUnit *U = InlinedFuncDie.getDwarfUnit();
  const char *InlinedFuncName = InlinedFuncDie.getName(DINameKind::LinkageName);
  DWARFAddressRange Range{FuncLowPC, FuncHighPC, SectionIndex};
  LiveElements.emplace_back(std::make_unique<InlinedFunction>(
      InlinedFuncName, U, FuncDie, InlinedFuncDie, Range));
}

void LiveElementPrinter::addVariable(DWARFDie FuncDie, DWARFDie VarDie) {
  uint64_t FuncLowPC, FuncHighPC, SectionIndex;
  FuncDie.getLowAndHighPC(FuncLowPC, FuncHighPC, SectionIndex);
  const char *VarName = VarDie.getName(DINameKind::ShortName);
  DWARFUnit *U = VarDie.getDwarfUnit();

  Expected<DWARFLocationExpressionsVector> Locs =
      VarDie.getLocations(dwarf::DW_AT_location);
  if (!Locs) {
    // If the variable doesn't have any locations, just ignore it. We don't
    // report an error or warning here as that could be noisy on optimised
    // code.
    consumeError(Locs.takeError());
    return;
  }

  for (const DWARFLocationExpression &LocExpr : *Locs) {
    if (LocExpr.Range) {
      LiveElements.emplace_back(
          std::make_unique<LiveVariable>(LocExpr, VarName, U, FuncDie));
    } else {
      // If the LocExpr does not have an associated range, it is valid for
      // the whole of the function.
      // TODO: technically it is not valid for any range covered by another
      // LocExpr, does that happen in reality?
      DWARFLocationExpression WholeFuncExpr{
          DWARFAddressRange(FuncLowPC, FuncHighPC, SectionIndex), LocExpr.Expr};
      LiveElements.emplace_back(
          std::make_unique<LiveVariable>(WholeFuncExpr, VarName, U, FuncDie));
    }
  }
}

void LiveElementPrinter::addFunction(DWARFDie D) {
  for (const DWARFDie &Child : D.children()) {
    if (DbgVariables != DFDisabled &&
        (Child.getTag() == dwarf::DW_TAG_variable ||
         Child.getTag() == dwarf::DW_TAG_formal_parameter)) {
      addVariable(D, Child);
    } else if (DbgInlinedFunctions != DFDisabled &&
               Child.getTag() == dwarf::DW_TAG_inlined_subroutine) {
      addInlinedFunction(D, Child);
      addFunction(Child);
    } else
      addFunction(Child);
  }
}

// Get the column number (in characters) at which the first live element
// line should be printed.
unsigned LiveElementPrinter::getIndentLevel() const {
  return DbgIndent + getInstStartColumn(STI);
}

// Indent to the first live-range column to the right of the currently
// printed line, and return the index of that column.
// TODO: formatted_raw_ostream uses "column" to mean a number of characters
// since the last \n, and we use it to mean the number of slots in which we
// put live element lines. Pick a less overloaded word.
unsigned LiveElementPrinter::moveToFirstVarColumn(formatted_raw_ostream &OS) {
  // Logical column number: column zero is the first column we print in, each
  // logical column is 2 physical columns wide.
  unsigned FirstUnprintedLogicalColumn =
      std::max((int)(OS.getColumn() - getIndentLevel() + 1) / 2, 0);
  // Physical column number: the actual column number in characters, with
  // zero being the left-most side of the screen.
  unsigned FirstUnprintedPhysicalColumn =
      getIndentLevel() + FirstUnprintedLogicalColumn * 2;

  if (FirstUnprintedPhysicalColumn > OS.getColumn())
    OS.PadToColumn(FirstUnprintedPhysicalColumn);

  return FirstUnprintedLogicalColumn;
}

unsigned LiveElementPrinter::findFreeColumn() {
  for (unsigned ColIdx = 0; ColIdx < ActiveCols.size(); ++ColIdx)
    if (!ActiveCols[ColIdx].isActive())
      return ColIdx;

  size_t OldSize = ActiveCols.size();
  ActiveCols.grow(std::max<size_t>(OldSize * 2, 1));
  return OldSize;
}

void LiveElementPrinter::dump() const {
  for (const std::unique_ptr<LiveElement> &LE : LiveElements) {
    LE->dump(dbgs());
    LE->print(dbgs(), MRI);
    dbgs() << "\n";
  }
}

void LiveElementPrinter::addCompileUnit(DWARFDie D) {
  if (D.getTag() == dwarf::DW_TAG_subprogram)
    addFunction(D);
  else
    for (const DWARFDie &Child : D.children())
      addFunction(Child);
}

/// Update to match the state of the instruction between ThisAddr and
/// NextAddr. In the common case, any live range active at ThisAddr is
/// live-in to the instruction, and any live range active at NextAddr is
/// live-out of the instruction. If IncludeDefinedVars is false, then live
/// ranges starting at NextAddr will be ignored.
void LiveElementPrinter::update(object::SectionedAddress ThisAddr,
                                object::SectionedAddress NextAddr,
                                bool IncludeDefinedVars) {
  // Do not create live ranges when debug-inlined-funcs option is provided with
  // line format option.
  if (DbgInlinedFunctions == DFLimitsOnly)
    return;

  // First, check variables which have already been assigned a column, so
  // that we don't change their order.
  SmallSet<unsigned, 8> CheckedElementIdxs;
  for (unsigned ColIdx = 0, End = ActiveCols.size(); ColIdx < End; ++ColIdx) {
    if (!ActiveCols[ColIdx].isActive())
      continue;

    CheckedElementIdxs.insert(ActiveCols[ColIdx].ElementIdx);
    const std::unique_ptr<LiveElement> &LE =
        LiveElements[ActiveCols[ColIdx].ElementIdx];
    ActiveCols[ColIdx].LiveIn = LE->liveAtAddress(ThisAddr);
    ActiveCols[ColIdx].LiveOut = LE->liveAtAddress(NextAddr);
    std::string Name = Demangle ? demangle(LE->getName()) : LE->getName();
    LLVM_DEBUG(dbgs() << "pass 1, " << ThisAddr.Address << "-"
                      << NextAddr.Address << ", " << Name << ", Col " << ColIdx
                      << ": LiveIn=" << ActiveCols[ColIdx].LiveIn
                      << ", LiveOut=" << ActiveCols[ColIdx].LiveOut << "\n");

    if (!ActiveCols[ColIdx].LiveIn && !ActiveCols[ColIdx].LiveOut)
      ActiveCols[ColIdx].ElementIdx = Column::NullElementIdx;
  }

  // Next, look for variables which don't already have a column, but which
  // are now live.
  if (IncludeDefinedVars) {
    for (unsigned ElementIdx = 0, End = LiveElements.size(); ElementIdx < End;
         ++ElementIdx) {
      if (CheckedElementIdxs.count(ElementIdx))
        continue;

      const std::unique_ptr<LiveElement> &LE = LiveElements[ElementIdx];
      bool LiveIn = LE->liveAtAddress(ThisAddr);
      bool LiveOut = LE->liveAtAddress(NextAddr);
      if (!LiveIn && !LiveOut)
        continue;

      unsigned ColIdx = findFreeColumn();
      std::string Name = Demangle ? demangle(LE->getName()) : LE->getName();
      LLVM_DEBUG(dbgs() << "pass 2, " << ThisAddr.Address << "-"
                        << NextAddr.Address << ", " << Name << ", Col "
                        << ColIdx << ": LiveIn=" << LiveIn
                        << ", LiveOut=" << LiveOut << "\n");
      ActiveCols[ColIdx].ElementIdx = ElementIdx;
      ActiveCols[ColIdx].LiveIn = LiveIn;
      ActiveCols[ColIdx].LiveOut = LiveOut;
      ActiveCols[ColIdx].MustDrawLabel = true;
    }
  }
}

enum class LineChar {
  RangeStart,
  RangeMid,
  RangeEnd,
  LabelVert,
  LabelCornerNew,
  LabelCornerActive,
  LabelHoriz,
};
const char *LiveElementPrinter::getLineChar(LineChar C) const {
  bool IsASCII = DbgVariables == DFASCII || DbgInlinedFunctions == DFASCII;
  switch (C) {
  case LineChar::RangeStart:
    return IsASCII ? "^" : (const char *)u8"\u2548";
  case LineChar::RangeMid:
    return IsASCII ? "|" : (const char *)u8"\u2503";
  case LineChar::RangeEnd:
    return IsASCII ? "v" : (const char *)u8"\u253b";
  case LineChar::LabelVert:
    return IsASCII ? "|" : (const char *)u8"\u2502";
  case LineChar::LabelCornerNew:
    return IsASCII ? "/" : (const char *)u8"\u250c";
  case LineChar::LabelCornerActive:
    return IsASCII ? "|" : (const char *)u8"\u2520";
  case LineChar::LabelHoriz:
    return IsASCII ? "-" : (const char *)u8"\u2500";
  }
  llvm_unreachable("Unhandled LineChar enum");
}

/// Print live ranges to the right of an existing line. This assumes the
/// line is not an instruction, so doesn't start or end any live ranges, so
/// we only need to print active ranges or empty columns. If AfterInst is
/// true, this is being printed after the last instruction fed to update(),
/// otherwise this is being printed before it.
void LiveElementPrinter::printAfterOtherLine(formatted_raw_ostream &OS,
                                             bool AfterInst) {
  if (ActiveCols.size()) {
    unsigned FirstUnprintedColumn = moveToFirstVarColumn(OS);
    for (size_t ColIdx = FirstUnprintedColumn, End = ActiveCols.size();
         ColIdx < End; ++ColIdx) {
      if (ActiveCols[ColIdx].isActive()) {
        if ((AfterInst && ActiveCols[ColIdx].LiveOut) ||
            (!AfterInst && ActiveCols[ColIdx].LiveIn))
          OS << getLineChar(LineChar::RangeMid);
        else if (!AfterInst && ActiveCols[ColIdx].LiveOut)
          OS << getLineChar(LineChar::LabelVert);
        else
          OS << " ";
      }
      OS << " ";
    }
  }
  OS << "\n";
}

/// Print any live element range info needed to the right of a
/// non-instruction line of disassembly. This is where we print the element
/// names and expressions, with thin line-drawing characters connecting them
/// to the live range which starts at the next instruction. If MustPrint is
/// true, we have to print at least one line (with the continuation of any
/// already-active live ranges) because something has already been printed
/// earlier on this line.
void LiveElementPrinter::printBetweenInsts(formatted_raw_ostream &OS,
                                           bool MustPrint) {
  bool PrintedSomething = false;
  for (unsigned ColIdx = 0, End = ActiveCols.size(); ColIdx < End; ++ColIdx) {
    if (ActiveCols[ColIdx].isActive() && ActiveCols[ColIdx].MustDrawLabel) {
      // First we need to print the live range markers for any active
      // columns to the left of this one.
      OS.PadToColumn(getIndentLevel());
      for (unsigned ColIdx2 = 0; ColIdx2 < ColIdx; ++ColIdx2) {
        if (ActiveCols[ColIdx2].isActive()) {
          if (ActiveCols[ColIdx2].MustDrawLabel && !ActiveCols[ColIdx2].LiveIn)
            OS << getLineChar(LineChar::LabelVert) << " ";
          else
            OS << getLineChar(LineChar::RangeMid) << " ";
        } else
          OS << "  ";
      }

      const std::unique_ptr<LiveElement> &LE =
          LiveElements[ActiveCols[ColIdx].ElementIdx];
      // Then print the variable name and location of the new live range,
      // with box drawing characters joining it to the live range line.
      OS << getLineChar(ActiveCols[ColIdx].LiveIn ? LineChar::LabelCornerActive
                                                  : LineChar::LabelCornerNew)
         << getLineChar(LineChar::LabelHoriz) << " ";

      std::string Name = Demangle ? demangle(LE->getName()) : LE->getName();
      WithColor(OS, raw_ostream::GREEN) << Name;
      OS << " = ";
      {
        WithColor ExprColor(OS, raw_ostream::CYAN);
        LE->print(OS, MRI);
      }

      // If there are any columns to the right of the expression we just
      // printed, then continue their live range lines.
      unsigned FirstUnprintedColumn = moveToFirstVarColumn(OS);
      for (unsigned ColIdx2 = FirstUnprintedColumn, End = ActiveCols.size();
           ColIdx2 < End; ++ColIdx2) {
        if (ActiveCols[ColIdx2].isActive() && ActiveCols[ColIdx2].LiveIn)
          OS << getLineChar(LineChar::RangeMid) << " ";
        else
          OS << "  ";
      }

      OS << "\n";
      PrintedSomething = true;
    }
  }

  for (unsigned ColIdx = 0, End = ActiveCols.size(); ColIdx < End; ++ColIdx)
    if (ActiveCols[ColIdx].isActive())
      ActiveCols[ColIdx].MustDrawLabel = false;

  // If we must print something (because we printed a line/column number),
  // but don't have any new variables to print, then print a line which
  // just continues any existing live ranges.
  if (MustPrint && !PrintedSomething)
    printAfterOtherLine(OS, false);
}

/// Print the live element ranges to the right of a disassembled instruction.
void LiveElementPrinter::printAfterInst(formatted_raw_ostream &OS) {
  if (!ActiveCols.size())
    return;
  unsigned FirstUnprintedColumn = moveToFirstVarColumn(OS);
  for (unsigned ColIdx = FirstUnprintedColumn, End = ActiveCols.size();
       ColIdx < End; ++ColIdx) {
    if (!ActiveCols[ColIdx].isActive())
      OS << "  ";
    else if (ActiveCols[ColIdx].LiveIn && ActiveCols[ColIdx].LiveOut)
      OS << getLineChar(LineChar::RangeMid) << " ";
    else if (ActiveCols[ColIdx].LiveOut)
      OS << getLineChar(LineChar::RangeStart) << " ";
    else if (ActiveCols[ColIdx].LiveIn)
      OS << getLineChar(LineChar::RangeEnd) << " ";
    else
      llvm_unreachable("var must be live in or out!");
  }
}

void LiveElementPrinter::printStartLine(formatted_raw_ostream &OS,
                                        object::SectionedAddress Addr) {
  // Print a line to idenfity the start of an inlined function if line format
  // is specified.
  if (DbgInlinedFunctions == DFLimitsOnly)
    for (const std::unique_ptr<LiveElement> &LE : LiveElements)
      LE->printElementLine(OS, Addr, false);
}

void LiveElementPrinter::printEndLine(formatted_raw_ostream &OS,
                                      object::SectionedAddress Addr) {
  // Print a line to idenfity the end of an inlined function if line format is
  // specified.
  if (DbgInlinedFunctions == DFLimitsOnly)
    for (const std::unique_ptr<LiveElement> &LE : LiveElements)
      LE->printElementLine(OS, Addr, true);
}

bool SourcePrinter::cacheSource(const DILineInfo &LineInfo) {
  std::unique_ptr<MemoryBuffer> Buffer;
  if (LineInfo.Source) {
    Buffer = MemoryBuffer::getMemBuffer(*LineInfo.Source);
  } else {
    auto BufferOrError =
        MemoryBuffer::getFile(LineInfo.FileName, /*IsText=*/true);
    if (!BufferOrError) {
      if (MissingSources.insert(LineInfo.FileName).second)
        reportWarning("failed to find source " + LineInfo.FileName,
                      Obj->getFileName());
      return false;
    }
    Buffer = std::move(*BufferOrError);
  }
  // Chomp the file to get lines
  const char *BufferStart = Buffer->getBufferStart(),
             *BufferEnd = Buffer->getBufferEnd();
  std::vector<StringRef> &Lines = LineCache[LineInfo.FileName];
  const char *Start = BufferStart;
  for (const char *I = BufferStart; I != BufferEnd; ++I)
    if (*I == '\n') {
      Lines.emplace_back(Start, I - Start - (BufferStart < I && I[-1] == '\r'));
      Start = I + 1;
    }
  if (Start < BufferEnd)
    Lines.emplace_back(Start, BufferEnd - Start);
  SourceCache[LineInfo.FileName] = std::move(Buffer);
  return true;
}

void SourcePrinter::printSourceLine(formatted_raw_ostream &OS,
                                    object::SectionedAddress Address,
                                    StringRef ObjectFilename,
                                    LiveElementPrinter &LEP,
                                    StringRef Delimiter) {
  if (!Symbolizer)
    return;

  DILineInfo LineInfo = DILineInfo();
  Expected<DILineInfo> ExpectedLineInfo =
      Symbolizer->symbolizeCode(*Obj, Address);
  if (ExpectedLineInfo) {
    LineInfo = *ExpectedLineInfo;
  } else if (!WarnedInvalidDebugInfo) {
    WarnedInvalidDebugInfo = true;
    // TODO Untested.
    reportWarning("failed to parse debug information: " +
                      toString(ExpectedLineInfo.takeError()),
                  ObjectFilename);
  }

  if (!objdump::Prefix.empty() &&
      sys::path::is_absolute_gnu(LineInfo.FileName)) {
    // FileName has at least one character since is_absolute_gnu is false for
    // an empty string.
    assert(!LineInfo.FileName.empty());
    if (PrefixStrip > 0) {
      uint32_t Level = 0;
      auto StrippedNameStart = LineInfo.FileName.begin();

      // Path.h iterator skips extra separators. Therefore it cannot be used
      // here to keep compatibility with GNU Objdump.
      for (auto Pos = StrippedNameStart + 1, End = LineInfo.FileName.end();
           Pos != End && Level < PrefixStrip; ++Pos) {
        if (sys::path::is_separator(*Pos)) {
          StrippedNameStart = Pos;
          ++Level;
        }
      }

      LineInfo.FileName =
          std::string(StrippedNameStart, LineInfo.FileName.end());
    }

    SmallString<128> FilePath;
    sys::path::append(FilePath, Prefix, LineInfo.FileName);

    LineInfo.FileName = std::string(FilePath);
  }

  if (PrintLines)
    printLines(OS, Address, LineInfo, Delimiter, LEP);
  if (PrintSource)
    printSources(OS, LineInfo, ObjectFilename, Delimiter, LEP);
  OldLineInfo = LineInfo;
}

void SourcePrinter::printLines(formatted_raw_ostream &OS,
                               object::SectionedAddress Address,
                               const DILineInfo &LineInfo, StringRef Delimiter,
                               LiveElementPrinter &LEP) {
  bool PrintFunctionName = LineInfo.FunctionName != DILineInfo::BadString &&
                           LineInfo.FunctionName != OldLineInfo.FunctionName;
  if (PrintFunctionName) {
    OS << Delimiter << LineInfo.FunctionName;
    // If demangling is successful, FunctionName will end with "()". Print it
    // only if demangling did not run or was unsuccessful.
    if (!StringRef(LineInfo.FunctionName).ends_with("()"))
      OS << "()";
    OS << ":\n";
  }
  if (LineInfo.FileName != DILineInfo::BadString && LineInfo.Line != 0 &&
      (OldLineInfo.Line != LineInfo.Line ||
       OldLineInfo.FileName != LineInfo.FileName || PrintFunctionName)) {
    OS << Delimiter << LineInfo.FileName << ":" << LineInfo.Line;
    LEP.printBetweenInsts(OS, true);
  }
}

// Get the source line text for LineInfo:
// - use LineInfo::LineSource if available;
// - use LineCache if LineInfo::Source otherwise.
StringRef SourcePrinter::getLine(const DILineInfo &LineInfo,
                                 StringRef ObjectFilename) {
  if (LineInfo.LineSource)
    return LineInfo.LineSource.value();

  if (SourceCache.find(LineInfo.FileName) == SourceCache.end())
    if (!cacheSource(LineInfo))
      return {};

  auto LineBuffer = LineCache.find(LineInfo.FileName);
  if (LineBuffer == LineCache.end())
    return {};

  if (LineInfo.Line > LineBuffer->second.size()) {
    reportWarning(
        formatv("debug info line number {0} exceeds the number of lines in {1}",
                LineInfo.Line, LineInfo.FileName),
        ObjectFilename);
    return {};
  }

  // Vector begins at 0, line numbers are non-zero
  return LineBuffer->second[LineInfo.Line - 1];
}

void SourcePrinter::printSources(formatted_raw_ostream &OS,
                                 const DILineInfo &LineInfo,
                                 StringRef ObjectFilename, StringRef Delimiter,
                                 LiveElementPrinter &LEP) {
  if (LineInfo.FileName == DILineInfo::BadString || LineInfo.Line == 0 ||
      (OldLineInfo.Line == LineInfo.Line &&
       OldLineInfo.FileName == LineInfo.FileName))
    return;

  StringRef Line = getLine(LineInfo, ObjectFilename);
  if (!Line.empty()) {
    OS << Delimiter << Line;
    LEP.printBetweenInsts(OS, true);
  }
}

SourcePrinter::SourcePrinter(const object::ObjectFile *Obj,
                             StringRef DefaultArch)
    : Obj(Obj) {
  symbolize::LLVMSymbolizer::Options SymbolizerOpts;
  SymbolizerOpts.PrintFunctions =
      DILineInfoSpecifier::FunctionNameKind::LinkageName;
  SymbolizerOpts.Demangle = Demangle;
  SymbolizerOpts.DefaultArch = std::string(DefaultArch);
  Symbolizer.reset(new symbolize::LLVMSymbolizer(SymbolizerOpts));
}

} // namespace objdump
} // namespace llvm
