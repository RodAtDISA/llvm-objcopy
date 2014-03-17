set(LLVM_LINK_COMPONENTS
  ${LLVM_TARGETS_TO_BUILD}
  Object
  )

add_llvm_tool(llvm-objcopy
  llvm-objcopy.cpp
  )
