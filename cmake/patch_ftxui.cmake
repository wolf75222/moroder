# Run as FTXUI's PATCH_COMMAND (cmake -P, from FTXUI's source directory).
#
# Synchronized output: each frame is wrapped in DEC mode 2026, so the terminal
# shows it only once it has all of it. FTXUI rewrites the whole screen every
# frame; a terminal that paints while that arrives shows a half-drawn, partly
# blank screen, which flickers on every refresh. Terminals without the mode
# ignore it (unknown private modes are skipped), so nothing changes for them.
#
# Idempotent: a second run finds the marker and leaves the file alone.

set(file "src/ftxui/component/app.cpp")
file(READ "${file}" src)

string(FIND "${src}" "moroder: synchronized output" done)
if(NOT done EQUAL -1)
    return()
endif()

set(begin_old "  // Hide cursor to prevent flickering during reset.\n  TerminalSend(\"\\033[?25l\");")
set(begin_new "  // moroder: synchronized output, begin (see cmake/patch_ftxui.cmake).\n  TerminalSend(\"\\033[?2026h\");\n${begin_old}")
set(end_old "  public_->ToString(output_buffer);\n  TerminalSend(set_cursor_position_);\n  TerminalFlush();")
set(end_new "  public_->ToString(output_buffer);\n  TerminalSend(set_cursor_position_);\n  TerminalSend(\"\\033[?2026l\");  // moroder: synchronized output, end\n  TerminalFlush();")

foreach(part begin end)
    string(FIND "${src}" "${${part}_old}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "patch_ftxui.cmake: FTXUI's Draw() changed, update the patch (${part})")
    endif()
    string(REPLACE "${${part}_old}" "${${part}_new}" src "${src}")
endforeach()

file(WRITE "${file}" "${src}")
message(STATUS "FTXUI: synchronized output patched in")
