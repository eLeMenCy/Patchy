# PatchJuce.cmake — v0.0.981: a one-line fix to JUCE 8 (8.0.4 … 8.0.12; fixed
# upstream in JUCE 9 — checked 9.0.5, 2026-11-11: this script then does nothing),
# applied to the fetched sources at configure time. Idempotent: the file is
# only rewritten when the original text is still there.
#
# Linux WebBrowserComponent — killChild(): after asking WebKit's helper process
# to quit and waiting 1.5 s, JUCE sends SIGTERM and loops
#     kill (child, SIGTERM); waitpid (child, &status, 0); if (WIFEXITED (status)) break;
# A child killed by a signal is never WIFEXITED, and once reaped waitpid
# fails at once — the loop spins forever. Patchy then hung on quit (and would
# hang a DAW closing the plug-in window) whenever the helper took more than
# 1.5 s to quit (user, Garuda, 2026-11-10/11: the log stopped at
# "Quit 3/6: closing the web view"). Patched: stop once the child is reaped
# (exited OR killed) or is already gone (ECHILD).

set(_patchy_wb "${juce_SOURCE_DIR}/modules/juce_gui_extra/native/juce_WebBrowserComponent_linux.cpp")
if (EXISTS "${_patchy_wb}")
    file(READ "${_patchy_wb}" _patchy_src)
    set(_patchy_old [=[                    kill (childProcess, SIGTERM);
                    waitpid (childProcess, &status, 0);

                    if (WIFEXITED (status))
                        break;]=])
    set(_patchy_new [=[                    kill (childProcess, SIGTERM);

                    // Patchy (cmake/PatchJuce.cmake): a SIGTERM'd child is never
                    // WIFEXITED — stop once it is reaped, or already gone
                    if (waitpid (childProcess, &status, 0) == childProcess || errno == ECHILD)
                        break;]=])
    string(FIND "${_patchy_src}" "${_patchy_old}" _patchy_at)
    if (_patchy_at GREATER -1)
        string(REPLACE "${_patchy_old}" "${_patchy_new}" _patchy_src "${_patchy_src}")
        file(WRITE "${_patchy_wb}" "${_patchy_src}")
        message(STATUS "Patchy: patched JUCE's Linux WebBrowserComponent (quit hang)")
    elseif (_patchy_src MATCHES "WIFEXITED \\(status\\) \\|\\| WIFSIGNALED \\(status\\)")
        # JUCE 9 (checked 9.0.5 + develop, 2026-11-11) fixed it the same way
        message(STATUS "Patchy: JUCE's Linux WebBrowserComponent quit fix is upstream — nothing to patch")
    elseif (_patchy_src MATCHES "Patchy \\(cmake/PatchJuce.cmake\\)")
        message(STATUS "Patchy: JUCE's Linux WebBrowserComponent already patched")
    else()
        message(WARNING "Patchy: JUCE's Linux WebBrowserComponent changed — cmake/PatchJuce.cmake not applied (check whether JUCE fixed the quit hang)")
    endif()
endif()
