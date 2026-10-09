# ── FYI notes backup — v0.0.929 (2026-10-09) ─────────────────────────────────
# FYI/ (Architecture.md, SessionLog.md, …) is git-ignored on purpose, so it has
# no history and no offsite copy. At every CMake configure (CLion: Reload CMake
# Project, or any configure after a CMakeLists change), copy the whole FYI/
# folder into a timestamped .zip — only when its content changed since the
# last backup — and keep the newest PATCHY_FYI_BACKUP_KEEP backups.
# Default destination: iCloud Drive (offsite), else ~/Documents; any folder
# (e.g. on a NAS) via PATCHY_FYI_BACKUP_DIR — ~/Documents when it's unreachable.
# Planned before release: a commit hook + a private repo for FYI/ (roadmap).

option(PATCHY_FYI_BACKUP "Back up FYI/ at each CMake configure" ON)
set(PATCHY_FYI_BACKUP_KEEP 10 CACHE STRING "How many FYI backups to keep")

set(_fyi_icloud "$ENV{HOME}/Library/Mobile Documents/com~apple~CloudDocs")
if(APPLE AND EXISTS "${_fyi_icloud}")
    set(_fyi_default "${_fyi_icloud}/Patchy-FYI-backups")
elseif(DEFINED ENV{HOME})
    set(_fyi_default "$ENV{HOME}/Documents/Patchy-FYI-backups")
else()
    set(_fyi_default "${CMAKE_SOURCE_DIR}/../Patchy-FYI-backups")
endif()
set(PATCHY_FYI_BACKUP_DIR "${_fyi_default}" CACHE PATH "Where FYI backups go")

# Destination reachable? A NAS that isn't mounted (e.g. away from the studio)
# must not make CMake create a fake folder under /Volumes or fail: the backup
# folder itself may be created, but its parent has to exist already. When it
# doesn't, fall back to ~/Documents/Patchy-FYI-backups (always local). Each
# destination keeps its own "last backup" fingerprint, so the NAS catches up
# at the first configure after it's mounted again.
set(_fyi_dir "${PATCHY_FYI_BACKUP_DIR}")
get_filename_component(_fyi_parent "${_fyi_dir}" DIRECTORY)
if(PATCHY_FYI_BACKUP AND NOT IS_DIRECTORY "${_fyi_parent}")
    set(_fyi_local "$ENV{HOME}/Documents/Patchy-FYI-backups")
    message(STATUS "FYI backup: ${_fyi_parent} isn't reachable (NAS not mounted?) — using ${_fyi_local}")
    set(_fyi_dir "${_fyi_local}")
endif()

if(NOT PATCHY_FYI_BACKUP)
    message(STATUS "FYI backup: off (PATCHY_FYI_BACKUP)")
elseif(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/FYI")
    # Fingerprint of the folder: every file's path + content hash
    file(GLOB_RECURSE _fyi_files RELATIVE "${CMAKE_SOURCE_DIR}/FYI" "${CMAKE_SOURCE_DIR}/FYI/*")
    list(SORT _fyi_files)
    set(_fyi_concat "")
    foreach(_f IN LISTS _fyi_files)
        file(SHA256 "${CMAKE_SOURCE_DIR}/FYI/${_f}" _h)
        string(APPEND _fyi_concat "${_f}:${_h};")
    endforeach()
    string(SHA256 _fyi_hash "${_fyi_concat}")

    set(_fyi_stamp "${_fyi_dir}/.last-backup-hash")
    set(_fyi_last "")
    if(EXISTS "${_fyi_stamp}")
        file(READ "${_fyi_stamp}" _fyi_last)
        string(STRIP "${_fyi_last}" _fyi_last)
    endif()

    if(_fyi_files AND NOT _fyi_last STREQUAL _fyi_hash)
        string(TIMESTAMP _fyi_ts "%Y-%m-%d_%H%M%S")
        # One .zip per backup (v0.0.929, user's choice): notes compress a lot,
        # one file per backup is tidier on a NAS. Paths inside are relative to
        # FYI/ (archive made from inside it). Old folder-style backups are
        # still counted and pruned below.
        set(_fyi_dest "${_fyi_dir}/${_fyi_ts}_v${PROJECT_VERSION}.zip")
        file(MAKE_DIRECTORY "${_fyi_dir}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E tar cf "${_fyi_dest}" --format=zip -- ${_fyi_files}
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/FYI"
            RESULT_VARIABLE _fyi_rc ERROR_VARIABLE _fyi_err)
        if(_fyi_rc EQUAL 0)
            file(WRITE "${_fyi_stamp}" "${_fyi_hash}\n")
            file(SIZE "${_fyi_dest}" _fyi_size)
            if(_fyi_size GREATER 1048576)
                math(EXPR _fyi_mb "${_fyi_size} / 1048576")
                set(_fyi_human "${_fyi_mb} MB")
            else()
                math(EXPR _fyi_kb "(${_fyi_size} + 1023) / 1024")
                set(_fyi_human "${_fyi_kb} KB")
            endif()
            message(STATUS "FYI backup: saved ${_fyi_dest} (${_fyi_human})")
        else()
            file(REMOVE "${_fyi_dest}")
            message(WARNING "FYI backup failed (${_fyi_rc}): ${_fyi_err}")
        endif()

        # Keep the newest N (folder names sort by date)
        file(GLOB _fyi_all LIST_DIRECTORIES true "${_fyi_dir}/20*_v*")
        list(SORT _fyi_all)
        list(LENGTH _fyi_all _fyi_count)
        if(_fyi_count GREATER PATCHY_FYI_BACKUP_KEEP)
            math(EXPR _fyi_drop "${_fyi_count} - ${PATCHY_FYI_BACKUP_KEEP}")
            list(SUBLIST _fyi_all 0 ${_fyi_drop} _fyi_old)
            foreach(_d IN LISTS _fyi_old)
                file(REMOVE_RECURSE "${_d}")
            endforeach()
            message(STATUS "FYI backup: removed ${_fyi_drop} old backup(s), keeping ${PATCHY_FYI_BACKUP_KEEP}")
        endif()
    else()
        message(STATUS "FYI backup: unchanged since the last backup (${_fyi_dir})")
    endif()
endif()
