if(NOT DEFINED GENERATOR OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "GENERATOR and WORK_DIR are required")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(input_one "${WORK_DIR}/one.txt")
set(input_two "${WORK_DIR}/two.txt")
set(input_list "${WORK_DIR}/inputs.cmake")
set(output_one "${WORK_DIR}/one.inc")
set(output_two "${WORK_DIR}/two.inc")
set(empty_input "${WORK_DIR}/empty-input")
set(empty_list "${WORK_DIR}/empty.cmake")
set(missing_list "${WORK_DIR}/missing.cmake")

file(WRITE "${input_one}" "alpha\n")
file(WRITE "${input_two}" "omega")
file(WRITE "${input_list}"
  "set(VECTIS_BYTE_ARRAY_INPUTS\n  \"${input_one}\"\n  \"${input_two}\"\n)\n")

foreach(output IN ITEMS "${output_one}" "${output_two}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT_LIST_FILE=${input_list}"
            "-DOUTPUT=${output}"
            -P "${GENERATOR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "byte-array generator failed: ${stdout}${stderr}")
  endif()
endforeach()

file(READ "${output_one}" generated_one)
file(READ "${output_two}" generated_two)
set(expected
  "  0x61, 0x6c, 0x70, 0x68, 0x61, 0x0a, 0x6f, 0x6d\n  , 0x65, 0x67, 0x61\n")
if(NOT generated_one STREQUAL expected)
  message(FATAL_ERROR "byte-array output did not preserve exact input bytes")
endif()
if(NOT generated_one STREQUAL generated_two)
  message(FATAL_ERROR "byte-array output is not reproducible")
endif()
if(generated_one MATCHES ",[ \t\r\n]*$")
  message(FATAL_ERROR "byte-array output has a trailing comma")
endif()
string(REPLACE "\n" ";" generated_lines "${generated_one}")
foreach(generated_line IN LISTS generated_lines)
  string(LENGTH "${generated_line}" generated_line_length)
  if(generated_line_length GREATER 72)
    message(FATAL_ERROR "byte-array output exceeds the 72-column limit")
  endif()
endforeach()

file(WRITE "${empty_input}" "")
file(WRITE "${empty_list}"
  "set(VECTIS_BYTE_ARRAY_INPUTS\n  \"${empty_input}\"\n)\n")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
          "-DINPUT_LIST_FILE=${empty_list}"
          "-DOUTPUT=${WORK_DIR}/empty.inc"
          -P "${GENERATOR}"
  RESULT_VARIABLE empty_result
  OUTPUT_VARIABLE empty_stdout
  ERROR_VARIABLE empty_stderr)
if(empty_result EQUAL 0 OR
   NOT empty_stderr MATCHES "raw byte-array input must not be empty")
  message(FATAL_ERROR "byte-array generator accepted a zero-byte input")
endif()

file(WRITE "${output_one}" "published\n")
file(WRITE "${missing_list}"
  "set(VECTIS_BYTE_ARRAY_INPUTS\n  \"${WORK_DIR}/missing.txt\"\n)\n")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
          "-DINPUT_LIST_FILE=${missing_list}"
          "-DOUTPUT=${output_one}"
          -P "${GENERATOR}"
  RESULT_VARIABLE missing_result
  OUTPUT_VARIABLE missing_stdout
  ERROR_VARIABLE missing_stderr)
if(missing_result EQUAL 0 OR
   NOT missing_stderr MATCHES "input does not exist")
  message(FATAL_ERROR "byte-array generator accepted a missing input")
endif()
file(READ "${output_one}" preserved_output)
if(NOT preserved_output STREQUAL "published\n")
  message(FATAL_ERROR "byte-array generator replaced output after failure")
endif()
