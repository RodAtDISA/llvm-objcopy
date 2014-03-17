//===-- llvm-objcopy.h ----------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_OBJCOPY_H
#define LLVM_OBJCOPY_H

namespace llvm {

class error_code;

// Various helper functions.
bool error(error_code ec);

} // end namespace llvm

#endif
