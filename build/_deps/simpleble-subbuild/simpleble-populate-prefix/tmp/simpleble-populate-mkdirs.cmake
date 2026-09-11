# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/home/kdy/home-assistant-core/build/_deps/simpleble-src")
  file(MAKE_DIRECTORY "/home/kdy/home-assistant-core/build/_deps/simpleble-src")
endif()
file(MAKE_DIRECTORY
  "/home/kdy/home-assistant-core/build/_deps/simpleble-build"
  "/home/kdy/home-assistant-core/build/_deps/simpleble-subbuild/simpleble-populate-prefix"
  "/home/kdy/home-assistant-core/build/_deps/simpleble-subbuild/simpleble-populate-prefix/tmp"
  "/home/kdy/home-assistant-core/build/_deps/simpleble-subbuild/simpleble-populate-prefix/src/simpleble-populate-stamp"
  "/home/kdy/home-assistant-core/build/_deps/simpleble-subbuild/simpleble-populate-prefix/src"
  "/home/kdy/home-assistant-core/build/_deps/simpleble-subbuild/simpleble-populate-prefix/src/simpleble-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/kdy/home-assistant-core/build/_deps/simpleble-subbuild/simpleble-populate-prefix/src/simpleble-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/kdy/home-assistant-core/build/_deps/simpleble-subbuild/simpleble-populate-prefix/src/simpleble-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
