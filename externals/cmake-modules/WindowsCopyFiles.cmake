# Copyright 2016 Citra Emulator Project
# Licensed under GPLv2 or any later version
# Refer to the license.txt file included.

# This file provides the function windows_copy_files.
# This is only valid on Windows.

# Include guard
if(__windows_copy_files)
	return()
endif()
set(__windows_copy_files YES)

# Any number of files to copy from SOURCE_DIR to DEST_DIR can be specified after DEST_DIR.
# This copying happens post-build.
function(windows_copy_files TARGET SOURCE_DIR DEST_DIR)
    # windows commandline expects the / to be \ so switch them
    string(REPLACE "/" "\\\\" SOURCE_DIR ${SOURCE_DIR})
    string(REPLACE "/" "\\\\" DEST_DIR ${DEST_DIR})

    # /IS /S - Overwrite any existing files
    # /NJH /NJS /NC /NS /NP - Clean up the output
    # cmake adds an extra check for command success which doesn't work too well with robocopy
    # so trick it into thinking the command was successful with the `|| call`
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND (robocopy ${SOURCE_DIR} ${DEST_DIR} ${ARGN} /IS /S /NJH /NJS /NC /NS /NP || call)
    )
endfunction()