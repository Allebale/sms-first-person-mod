# cmake -S . -B build -A x64 -DSMS_MODS_OUT=C:/path/to/the/game/mods   (Windows 64-bit)
# cmake --build build --config Release
#
# The SDK: the copy in sdk/ when there is one, else GitHub's at SMS_MOD_SDK_TAG.
cmake_minimum_required(VERSION 3.16)
project(first_person CXX)

set(SMS_MOD_SDK_TAG v1.5.0-beta.8 CACHE STRING "The mod SDK's release")
# An SDK shipped next to this file (sdk/) is used as it is, no download.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/include/sms_mod.h AND NOT FETCHCONTENT_SOURCE_DIR_SMS_MOD_SDK)
  set(FETCHCONTENT_SOURCE_DIR_SMS_MOD_SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
endif()
include(FetchContent)
FetchContent_Declare(sms_mod_sdk
  GIT_REPOSITORY https://github.com/chasem-dev/sms-mod-sdk.git
  GIT_TAG ${SMS_MOD_SDK_TAG}
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(sms_mod_sdk)

sms_add_mod(mod src/first_person.cpp)
set_target_properties(${SMS_MOD_TARGET} PROPERTIES CXX_STANDARD 11)
if(MSVC)
  target_compile_options(${SMS_MOD_TARGET} PRIVATE /W3 /D_CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(${SMS_MOD_TARGET} PRIVATE -Wall -Wextra -O2)
endif()
