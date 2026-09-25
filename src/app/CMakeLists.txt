# app —— 命令行可执行程序 tetherkitnext-cli
#
# CMake 目标名是 tetherkitnext，**产物文件名**是 tetherkitnext-cli（与 App 包里
# Contents/MacOS/tetherkitnext-cli、/usr/local/bin 下的链接同名）。
add_executable(tetherkitnext main.cc)
set_target_properties(tetherkitnext PROPERTIES OUTPUT_NAME tetherkitnext-cli)

# tk_core PUBLIC 链接了其余全部模块。
target_link_libraries(tetherkitnext PRIVATE tetherkitnext_base tk_version tk_core)

install(TARGETS tetherkitnext RUNTIME DESTINATION bin)
