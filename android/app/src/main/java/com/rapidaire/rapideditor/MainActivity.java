package com.rapidaire.rapideditor;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;
import android.view.PointerIcon;

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

    /**
     * SDL's SDL_SetCursor cannot change the cursor the emulator / DeX draws
     * (Android has no global system-cursor API); the pointer shape must be
     * set per-view through PointerIcon.  Called from native (cui__sdl_set_cursor)
     * on the SDL thread whenever the UI's hovered widget changes.
     *
     * @param style PointerIcon system style constant (see the mapping below).
     */
    public static void setPointerIconFromNative(final int style) {
        final Activity a = SDLActivity.getContext() instanceof Activity
                ? (Activity) SDLActivity.getContext() : null;
        if (a == null) return;
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                PointerIcon icon = PointerIcon.getSystemIcon(a, style);
                a.getWindow().getDecorView().setPointerIcon(icon);
            }
        });
    }
}
