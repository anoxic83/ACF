cmake_minimum_required(VERSION 3.11)

set(CMAKE_INSTALL_PREFIX ${PROJECT_BINARY_DIR})

project(TEMP_PROJ VERSION 0.1.0)

set(CMAKE_C_STANDARD 17)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)


if( CMAKE_SIZEOF_VOID_P EQUAL 8 )
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib/x86_64/$<0:>)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib/x86_64/$<0:>)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/x86_64/$<0:>)
elseif( CMAKE_SIZEOF_VOID_P EQUAL 4 )
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/x86_i686/$<0:>)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib/x86_i686/$<0:>)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib/x86_i686/$<0:>)
endif()

set(ROOTSRC ${PROJECT_SOURCE_DIR}/src)
set(ROOTINC ${PROJECT_SOURCE_DIR}/include)


set(DEPS ${PROJECT_SOURCE_DIR}/deps)


if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(CMAKE_CXX_FLAGS "-Wall -Wextra")
  set(CMAKE_CXX_FLAGS_DEBUG "-g -DDEBUG")
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -s -DNDEBUG")
  set(CMAKE_CXX_FLAGS_MINSIZEREL "-O3 -s -DNDEBUG")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DDEBUG")
  #
  set(CMAKE_C_FLAGS "-Wall -Wextra")
  set(CMAKE_C_FLAGS_DEBUG "-g -DDEBUG")
  set(CMAKE_C_FLAGS_RELEASE "-O3 -s -DNDEBUG")
  set(CMAKE_C_FLAGS_MINSIZEREL "-O3 -s -DNDEBUG")
  set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g -DDEBUG")
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set(CMAKE_CXX_FLAGS_DEBUG "/MDd /Zi /Ob0 /Od /RTC1 /D_CRT_SECURE_NO_WARNINGS")
  set(CMAKE_CXX_FLAGS "/DWIN32 /D_WINDOWS /W3 /GR /EHsc")
  set(CMAKE_CXX_FLAGS_MINSIZEREL "/MD /O1 /Ob1 /DNDEBUG")
  set(CMAKE_CXX_FLAGS_RELEASE "/MD /O2 /Ob2 /DNDEBUG")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/MD /Zi /O2 /Ob1 /DDEBUG")
  #
  set(CMAKE_C_FLAGS_DEBUG "/MDd /Zi /Ob0 /Od /RTC1 /D_CRT_SECURE_NO_WARNINGS")
  set(CMAKE_C_FLAGS "/DWIN32 /D_WINDOWS /W3 /GR /EHsc")
  set(CMAKE_C_FLAGS_MINSIZEREL "/MD /O1 /Ob1 /DNDEBUG")
  set(CMAKE_C_FLAGS_RELEASE "/MD /O2 /Ob2 /DNDEBUG")
  set(CMAKE_C_FLAGS_RELWITHDEBINFO "/MD /Zi /O2 /Ob1 /DDEBUG")
endif()

include_directories(${ROOTSRC})
include_directories(${ROOTINC})

#find_package(PkgConfig)
#pkg_check_modules(CEGUI cegui) 
#link_directories( ${CEGUI_LIBRARY_DIRS} )
#include_directories( ${CEGUI_INCLUDE_DIRS} )


set(ACFLIB_FILES
  ${ROOTSRC}/acf.cc
)
add_library(acf ${ACFLIB_FILES})
target_link_libraries(acf libzstd.a)

set(CMAKE_SHARED_LIBRARY_PREFIX "")

if( CMAKE_SIZEOF_VOID_P EQUAL 8 )
  set(CMAKE_SHARED_LIBRARY_SUFFIX ".wcx64")
elseif( CMAKE_SIZEOF_VOID_P EQUAL 4 )
  set(CMAKE_SHARED_LIBRARY_SUFFIX ".wcx")
endif()

set(ACFWCX_FILES
  ${ROOTSRC}/acfwcx.cc
)
add_library(acfwcx SHARED ${ACFWCX_FILES})
target_link_libraries(acfwcx acf libzstd.a libc++.a)

set(ACFCLI_FILES
  ${ROOTSRC}/acfcli.cc
)

add_executable(acfcli ${ACFCLI_FILES})
target_link_libraries(acfcli acf libc++.a)
#target_link_options(test PUBLIC -Wl,-Bstatic,--whole-archive -lpthread -Wl,--no-whole-archive)
#target_link_options(test PUBLIC -Wl,-allow-multiple-definition)
#target_link_options(test PUBLIC -static-libstdc++ -static-libgcc)