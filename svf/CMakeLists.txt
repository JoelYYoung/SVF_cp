# Build the backend-neutral abstract-domain library as a separate target. None of
# these sources includes SVF IR or LLVM headers.
set(ABSTRACT_DOMAIN_CORE_HEADERS
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/AbstractDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/Variable.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/Expression.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/NumericalDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/AddressDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/BoxAddressDomain.h
)
set(ABSTRACT_DOMAIN_CORE_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/AbstractDomain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/AddressDomain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/BoxAddressDomain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/Expression.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/NumericalDomain.cpp
)
add_library(AbstractDomainCore)
add_library(SVF::AbstractDomainCore ALIAS AbstractDomainCore)
target_compile_features(AbstractDomainCore PUBLIC cxx_std_17)
target_sources(
  AbstractDomainCore
  PUBLIC FILE_SET HEADERS
                  BASE_DIRS ${CMAKE_CURRENT_LIST_DIR}/include
                  FILES ${ABSTRACT_DOMAIN_CORE_HEADERS}
  PRIVATE ${ABSTRACT_DOMAIN_CORE_SOURCES}
)
target_include_directories(
  AbstractDomainCore PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
)
target_link_libraries(AbstractDomainCore PUBLIC GMP::GMPXX MPFR::MPFR)
set_target_properties(
  AbstractDomainCore PROPERTIES
  VERSION ${SVF_VERSION}
  SOVERSION ${SVF_VERSION_MAJOR}
)

install(
  TARGETS AbstractDomainCore
  EXPORT SVFTargets
  RUNTIME DESTINATION ${SVF_INSTALL_BINDIR}
  LIBRARY DESTINATION ${SVF_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${SVF_INSTALL_LIBDIR}
          FILE_SET HEADERS
          DESTINATION ${SVF_INSTALL_INCLUDEDIR}
)

# Define the core SVF library.
add_library(SvfCore)

# Inherit compiler & linker options by publically linking against the interface library
target_link_libraries(SvfCore PUBLIC SvfFlags)

# Gather & set all of the core library's source files by globbing all .h and .cpp files (recursively)
file(GLOB_RECURSE SVF_CORE_HEADERS ${CMAKE_CURRENT_LIST_DIR}/include/*.h)
file(GLOB_RECURSE SVF_CORE_SOURCES ${CMAKE_CURRENT_LIST_DIR}/lib/*.cpp)
list(FILTER SVF_CORE_SOURCES EXCLUDE REGEX "/lib/AE/Test/")
list(REMOVE_ITEM SVF_CORE_HEADERS ${ABSTRACT_DOMAIN_CORE_HEADERS})
list(REMOVE_ITEM SVF_CORE_SOURCES ${ABSTRACT_DOMAIN_CORE_SOURCES})
target_link_libraries(SvfCore PUBLIC AbstractDomainCore)
target_sources(
  SvfCore
  PUBLIC FILE_SET HEADERS
                  BASE_DIRS ${CMAKE_CURRENT_LIST_DIR}/include
                  FILES ${SVF_CORE_HEADERS}
  PRIVATE ${SVF_CORE_SOURCES}
)

# Only expose the headers in the source tree to in-tree users of SVF
target_include_directories(SvfCore PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>)

# Set the library & .so version of the core library
set_target_properties(SvfCore PROPERTIES VERSION ${SVF_VERSION} SOVERSION ${SVF_VERSION_MAJOR})

# Install the core library's files to the regular/configured install tree
install(
  TARGETS SvfCore
  EXPORT SVFTargets
  RUNTIME DESTINATION ${SVF_INSTALL_BINDIR}
  LIBRARY DESTINATION ${SVF_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${SVF_INSTALL_LIBDIR}
          FILE_SET HEADERS
          DESTINATION ${SVF_INSTALL_INCLUDEDIR}
)
