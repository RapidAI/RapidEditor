package com.rapidaire.rapideditor;

import org.libsdl.app.SDLActivity;

/**
 * Hosts the RapidEditor native core. SDL2 drives the app lifecycle: the Java
 * side only provides the window/surface and input plumbing, while
 * libRapidEditor.so exports SDL_main (the C `main` renamed by the build) and
 * runs the editor's software-rendered UI on SDL's native thread.
 *
 * With an external keyboard + mouse (DeX / PC mode) SDL delivers hardware key
 * events and pointer events directly; on touch-only use, SDL emulates the
 * mouse from finger events and CUI's text fields raise the soft keyboard
 * through SDL_StartTextInput.
 */
public class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        // libRapidEditor.so exports SDL_main; SDL2's activity dlopens it and
        // calls SDL_main on the native thread.
        return new String[] {
            "SDL2",
            "RapidEditor"
        };
    }
}
