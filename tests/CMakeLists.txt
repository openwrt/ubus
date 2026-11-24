ADD_SUBDIRECTORY(cram)

FIND_LIBRARY(json_script_library NAMES json_script)

MACRO(ADD_UNIT_TEST name)
  ADD_EXECUTABLE(${name} ${name}.c)
  TARGET_LINK_LIBRARIES(${name} ubus ${ubox_library} ${blob_library} ${json_script_library} ${json})
  TARGET_INCLUDE_DIRECTORIES(${name} PRIVATE ${PROJECT_SOURCE_DIR})
ENDMACRO(ADD_UNIT_TEST)

MACRO(ADD_UNIT_TEST_CPP name)
  ADD_EXECUTABLE(${name} ${name}.cpp)
  TARGET_LINK_LIBRARIES(${name} ${ubox_library} ${blob_library} ${json_script_library} ${json})
  TARGET_INCLUDE_DIRECTORIES(${name} PRIVATE ${PROJECT_SOURCE_DIR})
ENDMACRO(ADD_UNIT_TEST_CPP)

FILE(GLOB test_cases "test-*.c")
FOREACH(test_case ${test_cases})
  GET_FILENAME_COMPONENT(test_case ${test_case} NAME_WE)
  ADD_UNIT_TEST(${test_case})
  ADD_UNIT_TEST_SAN(${test_case} ${test_case}-san)
ENDFOREACH(test_case)

ADD_TEST(NAME remove-object-lost COMMAND test-remove-object-lost $<TARGET_FILE:ubusd>)
ADD_TEST(NAME remove-object-lost-san COMMAND test-remove-object-lost-san $<TARGET_FILE:ubusd>)
ADD_TEST(NAME reconnect-events COMMAND test-reconnect-events $<TARGET_FILE:ubusd>)
ADD_TEST(NAME reconnect-events-san COMMAND test-reconnect-events-san $<TARGET_FILE:ubusd>)
SET_TESTS_PROPERTIES(remove-object-lost remove-object-lost-san
		     reconnect-events reconnect-events-san
		     PROPERTIES SKIP_RETURN_CODE 77)

ENABLE_LANGUAGE(CXX)
ADD_COMPILE_OPTIONS($<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>)
ADD_UNIT_TEST_CPP(test-cplusplus)
ADD_TEST(NAME cplusplus COMMAND test-cplusplus)

IF(CMAKE_C_COMPILER_ID STREQUAL "Clang")
  ADD_SUBDIRECTORY(fuzz)
ENDIF()
