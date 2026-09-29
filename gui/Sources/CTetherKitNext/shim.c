// SwiftPM requires every C target to have at least one source file, otherwise it refuses to build.
//
// The entire content of this target is really include/tetherkitnext_c.h (a symbolic link to the C++ side's
// header) -- the real implementation is in libtetherkitnext.dylib. This empty translation unit only serves to
// satisfy SwiftPM's formal requirement.
//
// Incidentally it does one useful thing: it wraps the header once, so that "syntax that only C++ understands was written into the header"
// and similar problems are found at compile time, without waiting for the import on the Swift side to fail.
#include "tetherkitnext_c.h"

// A non-exported dummy symbol, to keep some linkers from warning about a completely empty object file.
static const int tetherkitnext_c_shim_anchor = 0;
const int* tetherkitnext_c_shim_unused(void);
const int* tetherkitnext_c_shim_unused(void) { return &tetherkitnext_c_shim_anchor; }
