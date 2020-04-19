function(copy_citra_SDL_deps target_dir)
    include(WindowsCopyFiles)
    windows_copy_files(${target_dir} ${SDL2_DLL_DIR} ${DLL_DEST} SDL2.dll)
endfunction(copy_citra_SDL_deps)
