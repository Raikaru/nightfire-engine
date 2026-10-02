#pragma once

namespace nf {

// nfview's character preview mode (`nfview <gamedir> --char <name|hash> ...`), see char_view.cpp.
// True if `argv` asks for it.
bool wants_character_view(int argc, char** argv);
int run_character_view(int argc, char** argv);

}  // namespace nf
