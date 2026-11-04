#pragma once
// `Code/components` is built as a *unity* build upstream (xmake `c++.unity_build`, batch
// size 12), so some translation units resolve names their neighbours happened to pull in.
// `ESLoader.cpp` uses `Set<String>` unqualified, which only works because another file in
// the same batch had already opened the namespace. A per-file build has to say so.
#include <TiltedCore/Stl.hpp>

using namespace TiltedPhoques;
