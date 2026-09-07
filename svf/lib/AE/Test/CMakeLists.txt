if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.23)
  project(SVFBoxDomainTests LANGUAGES CXX)
  include(CTest)

  get_filename_component(
    SVF_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE
  )
  list(APPEND CMAKE_MODULE_PATH "${SVF_REPOSITORY_ROOT}/cmake/Modules")
  find_package(GMP REQUIRED)
  find_package(MPFR REQUIRED)

  set(ABSTRACT_DOMAIN_SOURCE_ROOT "${SVF_REPOSITORY_ROOT}/svf")
  add_library(
    AbstractDomainCore
    ${ABSTRACT_DOMAIN_SOURCE_ROOT}/lib/AE/Core/AbstractDomain.cpp
    ${ABSTRACT_DOMAIN_SOURCE_ROOT}/lib/AE/Core/AddressDomain.cpp
    ${ABSTRACT_DOMAIN_SOURCE_ROOT}/lib/AE/Core/Expression.cpp
    ${ABSTRACT_DOMAIN_SOURCE_ROOT}/lib/AE/Core/BoxAddressDomain.cpp
    ${ABSTRACT_DOMAIN_SOURCE_ROOT}/lib/AE/Core/NumericalDomain.cpp
  )
  target_compile_features(AbstractDomainCore PUBLIC cxx_std_17)
  target_include_directories(
    AbstractDomainCore PUBLIC ${ABSTRACT_DOMAIN_SOURCE_ROOT}/include
  )
  target_link_libraries(AbstractDomainCore PUBLIC GMP::GMPXX MPFR::MPFR)
endif()

add_executable(svf-core-domain-test CoreDomainTest.cpp)
target_link_libraries(svf-core-domain-test PRIVATE AbstractDomainCore)
add_test(NAME core-domain COMMAND svf-core-domain-test)

add_executable(
  svf-address-domain-storage-benchmark AddressDomainStorageBenchmark.cpp
)
target_link_libraries(
  svf-address-domain-storage-benchmark PRIVATE AbstractDomainCore
)

if(TARGET SvfLLVM)
  add_executable(svf-box-ae-integration-test BoxAEIntegrationTest.cpp)
  target_link_libraries(svf-box-ae-integration-test PRIVATE SvfLLVM)

  function(add_box_ae_test test_name fixture sparsity)
    add_test(
      NAME ${test_name}
      COMMAND svf-box-ae-integration-test
              -ae-sparsity=${sparsity}
              -widen-delay=1
              -stat=false
              -model-consts=true
              -model-arrays=true
              -pre-field-sensitive=false
              -extapi=${CMAKE_BINARY_DIR}/lib/extapi.bc
              ${CMAKE_CURRENT_LIST_DIR}/${fixture}
    )
  endfunction()

  foreach(sparsity IN ITEMS dense semi-sparse sparse)
    add_box_ae_test(
      box-ae-projection-${sparsity} BoxReducedProduct.ll ${sparsity}
    )
    add_box_ae_test(
      box-ae-loop-${sparsity} BoxLoop.ll ${sparsity}
    )
    add_box_ae_test(
      box-ae-memory-refinement-${sparsity} SparseMemoryFlow.ll ${sparsity}
    )
    add_box_ae_test(
      box-ae-conservative-unknown-casts-${sparsity}
      ConservativeUnknownCasts.ll ${sparsity}
    )
    add_box_ae_test(
      box-ae-floating-constant-${sparsity}
      FloatingConstantFlow.ll ${sparsity}
    )
    add_box_ae_test(
      box-ae-pointer-argument-${sparsity}
      PointerArgumentFlow.ll ${sparsity}
    )
    add_box_ae_test(
      box-ae-multi-caller-pointer-${sparsity}
      MultiCallerPointerFlow.ll ${sparsity}
    )
    add_box_ae_test(
      box-ae-unknown-caller-pointer-${sparsity}
      UnknownCallerPointerFlow.ll ${sparsity}
    )
    add_box_ae_test(
      box-ae-pointer-ordering-${sparsity}
      PointerOrderingFlow.ll ${sparsity}
    )
    add_box_ae_test(
      box-ae-negative-gep-${sparsity}
      NegativeGepFlow.ll ${sparsity}
    )
  endforeach()
  add_box_ae_test(
    box-ae-global-scalar-carrier GlobalScalarCarrier.ll dense
  )
  foreach(sparsity IN ITEMS dense semi-sparse sparse)
    add_box_ae_test(
      box-ae-opaque-gep-object-${sparsity}
      OpaqueGepObject.ll ${sparsity}
    )
  endforeach()
  foreach(sparsity IN ITEMS dense semi-sparse sparse)
    add_box_ae_test(
      box-ae-global-gep-initializer-${sparsity}
      GlobalGepInitializer.ll ${sparsity}
    )
    set_tests_properties(
      box-ae-global-gep-initializer-${sparsity}
      PROPERTIES
        ENVIRONMENT
          "SVF_AE_VALIDATE_GLOBAL_GEP_INITIALIZERS=1;SVF_AE_SEMANTIC_CHECKSUM=1"
        PASS_REGULAR_EXPRESSION
          "AE_RESULT_HASH fnv1a64=3002b6c833fd07fd records=62 contract=svf-query-projection-v7"
    )
  endforeach()
  add_box_ae_test(
    box-ae-wide-integer WideIntegerTruncation.ll dense
  )
  add_box_ae_test(
    box-ae-dynamic-object-schema BoxReducedProduct.ll dense
  )
  set_tests_properties(
    box-ae-dynamic-object-schema
    PROPERTIES ENVIRONMENT "SVF_AE_VALIDATE_DYNAMIC_ADAPTER=1"
  )
  set_tests_properties(
    box-ae-projection-semi-sparse
    PROPERTIES
      ENVIRONMENT
        "SVF_AE_VALIDATE_VARIABLE_ID_LAYOUT=1;SVF_AE_SEMANTIC_CHECKSUM=1"
      PASS_REGULAR_EXPRESSION
        "AE_RESULT_HASH fnv1a64=b0c9d6b1f8615842 records=43 contract=svf-query-projection-v7"
  )
endif()

if(APPLE AND TARGET z3::libz3)
  get_target_property(Z3_TEST_LIBRARY z3::libz3 IMPORTED_LOCATION)
  get_filename_component(
    Z3_TEST_LIBRARY_DIRECTORY "${Z3_TEST_LIBRARY}" DIRECTORY
  )
  set(box_test_targets svf-core-domain-test)
  if(TARGET svf-box-ae-integration-test)
    list(APPEND box_test_targets svf-box-ae-integration-test)
  endif()
  foreach(box_test_target IN LISTS box_test_targets)
    set_property(
      TARGET ${box_test_target} APPEND
      PROPERTY BUILD_RPATH "${Z3_TEST_LIBRARY_DIRECTORY}"
    )
    add_custom_command(
      TARGET ${box_test_target} POST_BUILD
      COMMAND install_name_tool -change libz3.dylib @rpath/libz3.dylib
              "$<TARGET_FILE:${box_test_target}>"
      VERBATIM
    )
  endforeach()
endif()
