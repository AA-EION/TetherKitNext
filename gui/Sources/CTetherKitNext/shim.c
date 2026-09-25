// SwiftPM 要求每个 C target 至少有一个源文件，否则拒绝构建。
//
// 本 target 的全部内容其实是 include/tetherkitnext_c.h（一个指向 C++ 侧头文件的
// 符号链接）—— 真正的实现在 libtetherkitnext.dylib 里。这个空的翻译单元只是用来
// 满足 SwiftPM 的形式要求。
//
// 顺带做一件有用的事：把头文件包一遍，编译期就能发现「头文件里写了 C++ 才认的
// 语法」这类问题，而不用等到 Swift 侧导入失败。
#include "tetherkitnext_c.h"

// 一个不导出的哑符号，防止某些链接器对完全空的目标文件发出告警。
static const int tetherkitnext_c_shim_anchor = 0;
const int* tetherkitnext_c_shim_unused(void);
const int* tetherkitnext_c_shim_unused(void) { return &tetherkitnext_c_shim_anchor; }
