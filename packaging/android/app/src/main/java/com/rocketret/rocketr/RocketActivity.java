package com.rocketret.rocketr;

import org.libsdl.app.SDLActivity;

import java.io.File;

public final class RocketActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {
        File rom = new File(getFilesDir(), "rocket.us.z64");
        File config = new File(getFilesDir(), "config");
        if (!config.exists()) config.mkdirs();
        return new String[] { "--rom", rom.getAbsolutePath(), "--config", config.getAbsolutePath() };
    }
}
