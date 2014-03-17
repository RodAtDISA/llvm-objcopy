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
                           clEnumVal(intel_hex, "Intel Hex format"),
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

class ObjectCopyBase {
public:
    ObjectCopyBase(StringRef InputFilename) 
        : mBinaryOutput(false)
        , mFillGaps(false)
    {
    }
    virtual ~ObjectCopyBase() {}

    void CopyTo(ObjectFile *o, StringRef OutputFilename) const {
        if (o == NULL) {
            return;
        }

        std::string ErrorInfo;

        tool_output_file Out(OutputFilename.data(), ErrorInfo, mBinaryOutput ? sys::fs::F_Binary : sys::fs::F_None);
        if (!ErrorInfo.empty()) {
            errs() << ErrorInfo << '\n';
            return;
        }

        error_code  ec;
        bool        FillNextGap = false;
        uint64_t    LastAddress;
        StringRef   LastSectionName;

        for (section_iterator si = o->begin_sections(), se = o->end_sections(); si != se; si.increment(ec)) {
            if (error(ec)) return;

            StringRef SectionName;
            StringRef SectionContents;
            uint64_t  SectionAddress;
            bool      BSS;
            bool      Required;

            if (error(si->getName(SectionName))) return;
            if (error(si->getContents(SectionContents))) return;
            if (error(si->getAddress(SectionAddress))) return;
            if (error(si->isBSS(BSS))) continue;
            if (error(si->isRequiredForExecution(Required))) continue;

            if (   !Required
                || BSS
                || SectionContents.size() == 0) {
                continue;
            }

            if (FillNextGap) {
                if (SectionAddress < LastAddress) {
                    errs() << "Trying to fill gaps between sections " << LastSectionName << " and " << SectionName << " in invalid order\n";
                    return;
                } else if (SectionAddress == LastAddress) {
                    // No gap, do nothing
                } else if (SectionAddress - LastAddress > (1<<16)) {
                    // Gap size limit reached
                    errs() << "Gap between sections is too large\n";
                    return;
                } else {
                    FillGap(Out, 0x00, SectionAddress - LastAddress);
                }
            }

            PrintSection(Out, SectionName, SectionContents, SectionAddress);

            if (mFillGaps) {
                FillNextGap     = true;
                LastSectionName = SectionName;
                LastAddress     = SectionAddress + SectionContents.size();
            }
        }

        Out.keep();
    }


protected:
    virtual void PrintSection(tool_output_file &Out, const StringRef &SectionName,
                              const StringRef &SectionContents, uint64_t SectionAddress) const = 0;
    virtual void FillGap(tool_output_file &Out, unsigned char Value, uint64_t Size) const { }

    bool                  mBinaryOutput;
    bool                  mFillGaps;
};

class ObjectCopyIntelHex : public ObjectCopyBase {
public:
    ObjectCopyIntelHex(StringRef InputFilename) 
        : ObjectCopyBase(InputFilename) {}
    virtual ~ObjectCopyIntelHex() {}

protected:
    virtual void PrintSection(tool_output_file &Out, const StringRef &SectionName,
                              const StringRef &SectionContents, uint64_t SectionAddress) const
    {
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
};

class ObjectCopyReadMemH : public ObjectCopyBase {
public:
    ObjectCopyReadMemH(StringRef InputFilename) 
        : ObjectCopyBase(InputFilename) {}
    virtual ~ObjectCopyReadMemH() {}

protected:
    virtual void PrintSection(tool_output_file &Out, const StringRef &SectionName,
                              const StringRef &SectionContents, uint64_t SectionAddress) const
    {
        // Dump address
        Out.os() << "@" << format("%" PRIx64, SectionAddress) << "\n";
        uint64_t addr;
        uint64_t end;
        for (addr = 0, end = SectionContents.size(); addr < end; ++addr) {
            // Dump hex value.
            Out.os() << format("%02" PRIx8 "\n", (unsigned char)(SectionContents[addr]));
        }
    }
};

class ObjectCopyBinary : public ObjectCopyBase {
public:
    ObjectCopyBinary(StringRef InputFilename) 
        : ObjectCopyBase(InputFilename)
    {
        mBinaryOutput = true;
        mFillGaps     = true;
    }
    virtual ~ObjectCopyBinary() {}

protected:
    virtual void PrintSection(tool_output_file &Out, const StringRef &SectionName,
                              const StringRef &SectionContents, uint64_t SectionAddress) const
    {
        uint64_t addr;
        uint64_t end;
        for (addr = 0, end = SectionContents.size(); addr < end; ++addr) {
            Out.os() << SectionContents[addr];
        }
    }

    virtual void FillGap(tool_output_file &Out, unsigned char Value, uint64_t Size) const
    {
        uint64_t i;
        for (i = 0; i < Size; ++i) {
            Out.os() << Value;
        }
    }
};

int main(int argc, char **argv) {
    // Print a stack trace if we signal out.
    sys::PrintStackTraceOnErrorSignal();
    PrettyStackTraceProgram X(argc, argv);
    llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.

    cl::ParseCommandLineOptions(argc, argv, "llvm object file copy utility\n");

    ToolName = argv[0];
    OwningPtr<ObjectCopyBase> ObjectCopy;

    switch (OutputTarget) {
    case OutputFormatTy::binary:
        ObjectCopy.reset(new ObjectCopyBinary(InputFilename));
        break;
    case OutputFormatTy::intel_hex:
        ObjectCopy.reset(new ObjectCopyIntelHex(InputFilename));
        break;
    case OutputFormatTy::readmemh:
        ObjectCopy.reset(new ObjectCopyReadMemH(InputFilename));
        break;
    default:
        return 1;
    }

    // If file isn't stdin, check that it exists.
    if (InputFilename != "-" && !sys::fs::exists(InputFilename)) {
        errs() << ToolName << ": '" << InputFilename << "': " << "No such file\n";
        return 1;
    }

    // Attempt to open  binary.
    OwningPtr<Binary> binary;
    if (error_code ec = createBinary(InputFilename, binary)) {
        errs() << ToolName << ": '" << InputFilename << "': " << ec.message() << ".\n";
        return 1;
    }

    ObjectFile *o = dyn_cast<ObjectFile>(binary.get());
    if (o == NULL) {
        errs() << ToolName << ": '" << InputFilename << "': " << "Unrecognized file type.\n";
    }

    ObjectCopy->CopyTo(o, OutputFilename);

    return 0;
}
