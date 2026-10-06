package com.rocketret.rocketr;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Comparator;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Exports a bounded list of logs and graphics settings. No ROMs or saves. */
public final class DiagnosticsReport {
    private static final int MAX_LOGS = 3;
    private static final int MAX_LOG_BYTES = 1024 * 1024;
    private DiagnosticsReport() { }

    public static void write(OutputStream destination, File config, String device) throws IOException {
        try (ZipOutputStream zip = new ZipOutputStream(destination)) {
            zip.putNextEntry(new ZipEntry("device.txt"));
            zip.write(device.getBytes(StandardCharsets.UTF_8));
            zip.closeEntry();
            File settings = new File(config, "rocket-r-settings.ini");
            if (settings.isFile()) addFile(zip, settings, "rocket-r-settings.ini", 64 * 1024);
            File directory = new File(config, "logs").getCanonicalFile();
            File[] logs = directory.listFiles(file -> file.isFile() &&
                ((file.getName().startsWith("runtime-") && file.getName().endsWith(".log")) ||
                 (file.getName().startsWith("crash-") && file.getName().endsWith(".txt"))));
            if (logs == null) return;
            Arrays.sort(logs, Comparator.comparingLong(File::lastModified).reversed());
            int count = 0;
            for (File file : logs) {
                if (!directory.equals(file.getCanonicalFile().getParentFile())) continue;
                addFile(zip, file, "logs/" + file.getName(), MAX_LOG_BYTES);
                if (++count == MAX_LOGS) break;
            }
        }
    }

    private static void addFile(ZipOutputStream zip, File file, String name, int limit) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        try (FileInputStream input = new FileInputStream(file)) {
            // Keep the newest part of oversized logs; never load a whole log into RAM.
            input.getChannel().position(Math.max(0, input.getChannel().size() - limit));
            byte[] buffer = new byte[8192];
            int remaining = limit, read;
            while (remaining > 0 && (read = input.read(buffer, 0, Math.min(buffer.length, remaining))) != -1) {
                zip.write(buffer, 0, read);
                remaining -= read;
            }
        }
        zip.closeEntry();
    }
}
