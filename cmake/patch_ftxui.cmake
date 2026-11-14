# Run as FTXUI's PATCH_COMMAND (cmake -P, from FTXUI's source directory).
#
# Synchronized output: each frame is wrapped in DEC mode 2026, so the terminal
# shows it only once it has all of it. FTXUI rewrites the whole screen every
# frame; a terminal that paints while that arrives shows a half-drawn, partly
# blank screen, which flickers on every refresh.
#
# Only when MORODER_SYNC_OUTPUT=1: Moroder sets it at startup when the terminal
# answers that it supports the mode (see detectSyncOutput in src/main.cpp), so
# a terminal that doesn't, or handles it badly, gets exactly what it had.
#
# Idempotent: a second run finds the marker and leaves the file alone; a file
# carrying the first version of this patch is brought to this one.

set(file "src/ftxui/component/app.cpp")
file(READ "${file}" src)

string(FIND "${src}" "moroder: synchronized output v2" done)
if(NOT done EQUAL -1)
    return()
endif()

# First version (unconditional): back to FTXUI's own code first.
string(REPLACE "  // moroder: synchronized output, begin (see cmake/patch_ftxui.cmake).\n  TerminalSend(\"\\033[?2026h\");\n" "" src "${src}")
string(REPLACE "  TerminalSend(\"\\033[?2026l\");  // moroder: synchronized output, end\n" "" src "${src}")

set(begin_old "  // Hide cursor to prevent flickering during reset.\n  TerminalSend(\"\\033[?25l\");")
set(begin_new "  // moroder: synchronized output v2, begin (see cmake/patch_ftxui.cmake).\n  static const bool moroder_sync = [] {\n    const char* v = std::getenv(\"MORODER_SYNC_OUTPUT\");\n    return v != nullptr && v[0] == '1';\n  }();\n  if (moroder_sync) {\n    TerminalSend(\"\\033[?2026h\");\n  }\n${begin_old}")
set(end_old "  public_->ToString(output_buffer);\n  TerminalSend(set_cursor_position_);\n  TerminalFlush();")
set(end_new "  public_->ToString(output_buffer);\n  TerminalSend(set_cursor_position_);\n  if (moroder_sync) {\n    TerminalSend(\"\\033[?2026l\");  // moroder: synchronized output, end\n  }\n  TerminalFlush();")

foreach(part begin end)
    string(FIND "${src}" "${${part}_old}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "patch_ftxui.cmake: FTXUI's Draw() changed, update the patch (${part})")
    endif()
    string(REPLACE "${${part}_old}" "${${part}_new}" src "${src}")
endforeach()

string(FIND "${src}" "#include <cstdlib>" has_cstdlib)
if(has_cstdlib EQUAL -1)
    string(REPLACE "#include <algorithm>" "#include <algorithm>\n#include <cstdlib>  // moroder: getenv" src "${src}")
endif()

file(WRITE "${file}" "${src}")
message(STATUS "FTXUI: synchronized output patched in (when MORODER_SYNC_OUTPUT=1)")
