//===-- llvm-objcopy.cpp - Object file copying utility for llvm -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under  University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This program is a utility that mimics a tiny subset of binutils "objcopy".
//
//===----------------------------------------------------------------------===//

#include "llvm-objcopy.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/system_error.h"

using namespace llvm;
using namespace object;

static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input object file>"), cl::Required);
static cl::opt<std::string>
OutputFilename(cl::Positional, cl::desc("<output object file>"), cl::Required);

namespace {
    enum OutputFormatTy { binary, intel_hex, readmemh };
    cl::opt<OutputFormatTy>
        OutputTarget("O",
                cl::desc("Specify output target"),
                cl::values(clEnumVal(binary,    "raw binary"),
                    clEnumVal(intel_hex,  "Intel HEX format"),
                    clEnumVal(readmemh,  "Format read by Verilog's $readmemh system task"),
                    clEnumValEnd),
                cl::init(binary));
    cl::alias OutputTarget2("output-target", cl::desc("Alias for -O"),
            cl::aliasopt(OutputTarget));

    static StringRef ToolName;
}

bool llvm::error(error_code ec) {
    if (!ec) return false;

    errs() << ToolName << ": error reading file: " << ec.message() << ".\n";
    return true;
}

static void PrintSectionAsIntelHex(tool_output_file &Out, const StringRef &SectionName,
                                   const StringRef &SectionContents, uint64_t SectionAddress) {
    uint64_t LastBaseAddr = UINT64_MAX;

    Out.os() << "; Contents of section " << SectionName << "(@" << format("%08" PRIx64, SectionAddress) << "):\n";

    // Dump out content as Intel-Hex.
    uint64_t addr;
    uint64_t end;
    for (addr = 0, end = SectionContents.size(); addr < end; addr += 16) {
        uint64_t      LineAddr = SectionAddress + addr;
        uint64_t      Base = LineAddr >> 16;
        uint64_t      Size = (addr + 16 > end) ? end-addr : 16;
        unsigned char Sum;

        if (LastBaseAddr != Base) {
            Sum = 6 + (Base & 0xff) + ((Base >> 8) & 0xff);
            Out.os() << format(":02000004%04" PRIx64 "%02" PRIx8 "\n", Base, (unsigned char)(-Sum));
            LastBaseAddr = Base;
        }

        // Dump line header.
        Sum = Size + (LineAddr & 0xff) + ((LineAddr >> 8) & 0xff);
        Out.os() << format(":%02" PRIx64 "%04" PRIx64 "00", Size, LineAddr & 0xffff);

        // Dump line of hex.
        for (uint64_t i = 0; i < Size; ++i) {
            Sum += SectionContents[addr + i];
            Out.os() << format("%02" PRIx8, (unsigned char)(SectionContents[addr + i]));
        }
        // Dump checksum byte.
        Out.os() << format("%02" PRIx8 "\n", (unsigned char)(-Sum));
    }
}

static void PrintSectionAsReadmemhInput(tool_output_file &Out, const StringRef &SectionName,
                                   const StringRef &SectionContents, uint64_t SectionAddress) {
    // Dump address
    Out.os() << "@" << format("%" PRIx64, SectionAddress) << "\n";
    uint64_t addr;
    uint64_t end;
    for (addr = 0, end = SectionContents.size(); addr < end; ++addr) {
        // Dump hex value.
        Out.os() << format("%02" PRIx8 "\n", (unsigned char)(SectionContents[addr]));
    }
}

static void PrintObject(const ObjectFile *o, StringRef OutputFilename, sys::fs::OpenFlags flags,
                        void (*PrintSectionFct)(tool_output_file&, const StringRef &, const StringRef &, uint64_t)) {
    std::string ErrorInfo;
    tool_output_file Out(OutputFilename.data(), ErrorInfo, flags);
    if (!ErrorInfo.empty()) {
        errs() << ErrorInfo << '\n';
        return;
    }

    error_code  ec;

    for (section_iterator si = o->begin_sections(), se = o->end_sections(); si != se; si.increment(ec)) {
        if (error(ec)) return;

        StringRef SectionName;
        StringRef SectionContents;
        uint64_t  SectionAddress;
        bool      BSS;

        if (error(si->getName(SectionName))) return;
        if (error(si->getContents(SectionContents))) return;
        if (error(si->getAddress(SectionAddress))) return;
        if (error(si->isBSS(BSS))) continue;

        if (   BSS
            || SectionContents.size() == 0) {
            continue;
        }

        PrintSectionFct(Out, SectionName, SectionContents, SectionAddress);
    }

    Out.keep();
}

/// @brief Open file and figure out how to dump it.
static void CopyInput(StringRef InputFilename, StringRef OutputFilename) {
    // If file isn't stdin, check that it exists.
    if (InputFilename != "-" && !sys::fs::exists(InputFilename)) {
        errs() << ToolName << ": '" << InputFilename << "': " << "No such file\n";
        return;
    }

    // Attempt to open  binary.
    OwningPtr<Binary> binary;
    if (error_code ec = createBinary(InputFilename, binary)) {
        errs() << ToolName << ": '" << InputFilename << "': " << ec.message() << ".\n";
        return;
    }

    if (ObjectFile *o = dyn_cast<ObjectFile>(binary.get()))
        switch (OutputTarget) {
            default:
            case OutputFormatTy::binary:
                errs() << ToolName << ": '" << "unsupported output target.\n";
                break;
            case OutputFormatTy::intel_hex:
                PrintObject(o, OutputFilename, sys::fs::F_None, PrintSectionAsIntelHex);
                break;
            case OutputFormatTy::readmemh:
                PrintObject(o, OutputFilename, sys::fs::F_None, PrintSectionAsReadmemhInput);
                break;
        }
    else
        errs() << ToolName << ": '" << InputFilename << "': " << "Unrecognized file type.\n";
}

int main(int argc, char **argv) {
    // Print a stack trace if we signal out.
    sys::PrintStackTraceOnErrorSignal();
    PrettyStackTraceProgram X(argc, argv);
    llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.

    cl::ParseCommandLineOptions(argc, argv, "llvm object file copy utility\n");

    ToolName = argv[0];

    CopyInput(InputFilename, OutputFilename);

    return 0;
}
