# app -- command-line executable tetherkitnext-cli
#
# The CMake target name is tetherkitnext, and the **output file name** is tetherkitnext-cli (the same as
# Contents/MacOS/tetherkitnext-cli in the App bundle and the link under /usr/local/bin).
add_executable(tetherkitnext main.cc)
set_target_properties(tetherkitnext PROPERTIES OUTPUT_NAME tetherkitnext-cli)

# tk_core links all the other modules PUBLICly.
target_link_libraries(tetherkitnext PRIVATE tetherkitnext_base tk_version tk_core)

install(TARGETS tetherkitnext RUNTIME DESTINATION bin)
