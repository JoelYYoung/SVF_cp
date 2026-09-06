set(LLVM_REQUIRES_EH ON)
add_llvm_executable(ae ae.cpp)

if(BUILD_TESTING)
  add_test(
    NAME ae-explicit-modeling-options
    COMMAND ae
            -ae-sparsity=semi-sparse
            -model-consts=true
            -model-arrays=true
            -pre-field-sensitive=false
            -stat=false
            -extapi=${CMAKE_BINARY_DIR}/lib/extapi.bc
            ${CMAKE_SOURCE_DIR}/svf/lib/AE/Test/BoxLoop.ll
  )
endif()
