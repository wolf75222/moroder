# Run as FTXUI's PATCH_COMMAND (cmake -P, from FTXUI's source directory).
# Two changes to App::Internal::Draw, each applied once (idempotent):
#
# 1. Synchronized output: each frame is wrapped in DEC mode 2026, so the
#    terminal shows it only once it has all of it. Only when
#    MORODER_SYNC_OUTPUT=1, which Moroder sets at startup when the terminal
#    says it supports the mode (detectSyncOutput in src/main.cpp).
#
# 2. Only the lines that changed: FTXUI rewrites the whole screen every frame
#    (~35 KB on a large terminal, ten times a second with sung-along lyrics).
#    In fullscreen (alternate screen) each line ends with a style reset, so
#    lines are independent: unchanged ones are skipped and changed ones are
#    written at their absolute position. A full frame still goes out on the
#    first frame, on resize, and every 20 s in case anything else wrote to the
#    terminal.

set(file "src/ftxui/component/app.cpp")
file(READ "${file}" src)
set(changed FALSE)

# The first version of (1), unconditional: back to FTXUI's own code first.
string(REPLACE "  // moroder: synchronized output, begin (see cmake/patch_ftxui.cmake).\n  TerminalSend(\"\\033[?2026h\");\n" "" src "${src}")
string(REPLACE "  TerminalSend(\"\\033[?2026l\");  // moroder: synchronized output, end\n" "" src "${src}")

# (1) synchronized output
string(FIND "${src}" "moroder: synchronized output v2" has_sync)
if(has_sync EQUAL -1)
    set(begin_old "  // Hide cursor to prevent flickering during reset.\n  TerminalSend(\"\\033[?25l\");")
    set(begin_new "  // moroder: synchronized output v2, begin (see cmake/patch_ftxui.cmake).\n  static const bool moroder_sync = [] {\n    const char* v = std::getenv(\"MORODER_SYNC_OUTPUT\");\n    return v != nullptr && v[0] == '1';\n  }();\n  if (moroder_sync) {\n    TerminalSend(\"\\033[?2026h\");\n  }\n${begin_old}")
    set(end_old "  public_->ToString(output_buffer);\n  TerminalSend(set_cursor_position_);\n  TerminalFlush();")
    set(end_new "  public_->ToString(output_buffer);\n  TerminalSend(set_cursor_position_);\n  if (moroder_sync) {\n    TerminalSend(\"\\033[?2026l\");  // moroder: synchronized output, end\n  }\n  TerminalFlush();")
    foreach(part begin end)
        string(FIND "${src}" "${${part}_old}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "patch_ftxui.cmake: FTXUI's Draw() changed, update the patch (sync ${part})")
        endif()
        string(REPLACE "${${part}_old}" "${${part}_new}" src "${src}")
    endforeach()
    set(changed TRUE)
endif()

# (2) only the lines that changed
string(FIND "${src}" "moroder: line diff" has_diff)
if(has_diff EQUAL -1)
    set(old "  public_->ToString(output_buffer);\n  TerminalSend(set_cursor_position_);")
    string(FIND "${src}" "${old}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "patch_ftxui.cmake: FTXUI's Draw() changed, update the patch (line diff)")
    endif()
    set(new [=[
  // moroder: line diff (see cmake/patch_ftxui.cmake).
  {
    static std::vector<std::string> moroder_previous;
    static auto moroder_full_at = std::chrono::steady_clock::now();
    std::string frame;
    public_->ToString(frame);
    std::vector<std::string> rows;
    rows.reserve(static_cast<size_t>(public_->dimy_));
    for (size_t start = 0;;) {
      const size_t nl = frame.find("\r\n", start);
      if (nl == std::string::npos) {
        rows.push_back(frame.substr(start));
        break;
      }
      rows.push_back(frame.substr(start, nl - start));
      start = nl + 2;
    }
    const auto now = std::chrono::steady_clock::now();
    const bool full = !use_alternative_screen_ || resized || frame_count_ == 0 ||
                      moroder_previous.size() != rows.size() ||
                      now - moroder_full_at > std::chrono::seconds(20);
    if (full) {
      output_buffer += frame;
      moroder_full_at = now;
    } else {
      for (size_t y = 0; y < rows.size(); ++y) {
        if (rows[y] == moroder_previous[y]) {
          continue;
        }
        output_buffer += "\x1B[" + std::to_string(y + 1) + ";1H";
        output_buffer += rows[y];
      }
      // Where a full frame would have left the cursor: the last cell.
      output_buffer += "\x1B[" + std::to_string(rows.size()) + ";" +
                       std::to_string(public_->dimx_) + "H";
    }
    moroder_previous = std::move(rows);
  }
  TerminalSend(set_cursor_position_);]=])
    string(REPLACE "${old}" "${new}" src "${src}")
    set(changed TRUE)
endif()

string(FIND "${src}" "#include <cstdlib>" has_cstdlib)
if(has_cstdlib EQUAL -1)
    string(REPLACE "#include <algorithm>" "#include <algorithm>\n#include <cstdlib>  // moroder: getenv" src "${src}")
    set(changed TRUE)
endif()

if(changed)
    file(WRITE "${file}" "${src}")
    message(STATUS "FTXUI: synchronized output and line diff patched in")
endif()
