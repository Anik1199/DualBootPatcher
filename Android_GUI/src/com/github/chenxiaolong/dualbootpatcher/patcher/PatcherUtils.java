/*
 * Copyright (C) 2014  Andrew Gunnerson <andrewgunnerson@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

package com.github.chenxiaolong.dualbootpatcher.patcher;

import android.content.Context;
import android.os.Environment;
import android.util.Log;

import com.github.chenxiaolong.dualbootpatcher.BuildConfig;
import com.github.chenxiaolong.dualbootpatcher.FileUtils;
import com.github.chenxiaolong.dualbootpatcher.R;
import com.github.chenxiaolong.dualbootpatcher.RomUtils;
import com.github.chenxiaolong.dualbootpatcher.nativelib.LibMbp.Device;
import com.github.chenxiaolong.dualbootpatcher.nativelib.LibMbp.PatcherConfig;
import com.github.chenxiaolong.dualbootpatcher.nativelib.LibMiscStuff;

import java.io.File;
import java.util.ArrayList;

public class PatcherUtils {
    public static final String TAG = PatcherUtils.class.getSimpleName();
    private static final String FILENAME = "data-%s.tar.xz";
    private static final String DIRNAME = "data-%s";

    private static final String PREFIX_DATA_SLOT = "data-slot-";
    private static final String PREFIX_EXTSD_SLOT = "extsd-slot-";

    public static PatcherConfig sPC;

    private static String sTargetFile;
    private static String sTargetDir;

    private static InstallLocation[] sInstallLocations;

    static {
        String version = BuildConfig.VERSION_NAME.split("-")[0];
        sTargetFile = String.format(FILENAME, version);
        sTargetDir = String.format(DIRNAME, version);
    }

    private static File getTargetFile(Context context) {
        return new File(context.getCacheDir() + File.separator + sTargetFile);
    }

    public static File getTargetDirectory(Context context) {
        return new File(context.getFilesDir() + File.separator + sTargetDir);
    }

    public synchronized static void initializePatcher(Context context) {
        if (sPC == null) {
            extractPatcher(context);

            sPC = new PatcherConfig();
            sPC.setDataDirectory(getTargetDirectory(context).getAbsolutePath());
            sPC.setTempDirectory(context.getCacheDir().getAbsolutePath());
        }
    }

    public synchronized static Device getCurrentDevice(Context context, PatcherConfig pc) {
        String realCodename = RomUtils.getDeviceCodename(context);

        Device device = null;
        for (Device d : pc.getDevices()) {
            for (String codename : d.getCodenames()) {
                if (realCodename.equals(codename)) {
                    device = d;
                    break;
                }
            }
            if (device != null) {
                break;
            }
        }

        return device;
    }

    public synchronized static void extractPatcher(Context context) {
        for (File d : context.getCacheDir().listFiles()) {
            if (d.getName().startsWith("DualBootPatcherAndroid")
                    || d.getName().startsWith("tmp")
                    || d.getName().startsWith("data-")) {
                org.apache.commons.io.FileUtils.deleteQuietly(d);
            }
        }
        for (File d : context.getFilesDir().listFiles()) {
            if (d.isDirectory()) {
                for (File t : d.listFiles()) {
                    if (t.getName().contains("tmp")) {
                        org.apache.commons.io.FileUtils.deleteQuietly(t);
                    }
                }
            }
        }

        File targetFile = getTargetFile(context);
        File targetDir = getTargetDirectory(context);

        if (!targetDir.exists()) {
            FileUtils.extractAsset(context, sTargetFile, targetFile);

            // Remove all previous files
            for (File d : context.getFilesDir().listFiles()) {
                org.apache.commons.io.FileUtils.deleteQuietly(d);
            }

            LibMiscStuff.INSTANCE.extract_archive(targetFile.getAbsolutePath(),
                    context.getFilesDir().getAbsolutePath());

            // Delete archive
            targetFile.delete();
        }
    }

    public static class InstallLocation {
        public String id;
        public String name;
        public String description;
    }

    public static InstallLocation[] getInstallLocations(Context context) {
        if (sInstallLocations == null) {
            ArrayList<InstallLocation> locations = new ArrayList<>();

            InstallLocation location = new InstallLocation();
            location.id = "primary";
            location.name = context.getString(R.string.install_location_primary_upgrade);
            location.description = context.getString(R.string.install_location_primary_upgrade_desc);

            locations.add(location);

            location = new InstallLocation();
            location.id = "dual";
            location.name = context.getString(R.string.secondary);
            location.description = String.format(context.getString(R.string.install_location_desc),
                    "/system/multiboot/dual");
            locations.add(location);

            for (int i = 1; i <= 3; i++) {
                location = new InstallLocation();
                location.id = "multi-slot-" + i;
                location.name = String.format(context.getString(R.string.multislot), i);
                location.description = String.format(context.getString(R.string.install_location_desc),
                        "/cache/multiboot/multi-slot-" + i);
                locations.add(location);
            }

            sInstallLocations = locations.toArray(new InstallLocation[locations.size()]);
        }

        return sInstallLocations;
    }

    public static InstallLocation[] getNamedInstallLocations(Context context) {
        //ThreadUtils.enforceExecutionOnNonMainThread();

        Log.d(TAG, "Looking for named ROMs");

        File dir = new File(Environment.getExternalStorageDirectory()
                + File.separator + "MultiBoot");

        ArrayList<InstallLocation> locations = new ArrayList<>();

        File[] files = dir.listFiles();
        if (files != null) {
            for (File f : files) {
                String name = f.getName();

                if (name.startsWith("data-slot-") && !name.equals("data-slot-")) {
                    Log.d(TAG, "- Found data-slot: " + name.substring(10));
                    locations.add(getDataSlotInstallLocation(context, name.substring(10)));
                } else if (name.startsWith("extsd-slot-") && !name.equals("extsd-slot-")) {
                    Log.d(TAG, "- Found extsd-slot: " + name.substring(11));
                    locations.add(getExtsdSlotInstallLocation(context, name.substring(11)));
                }
            }
        } else {
            Log.e(TAG, "Failed to list files in: " + dir);
        }

        return locations.toArray(new InstallLocation[locations.size()]);
    }

    public static InstallLocation getDataSlotInstallLocation(Context context, String dataSlotId) {
        InstallLocation location = new InstallLocation();
        location.id = getDataSlotRomId(dataSlotId);
        location.name = String.format(context.getString(R.string.dataslot), dataSlotId);
        location.description = context.getString(R.string.install_location_desc,
                "/data/multiboot/data-slot-" + dataSlotId);
        return location;
    }

    public static InstallLocation getExtsdSlotInstallLocation(Context context, String extsdSlotId) {
        InstallLocation location = new InstallLocation();
        location.id = getExtsdSlotRomId(extsdSlotId);
        location.name = String.format(context.getString(R.string.extsdslot), extsdSlotId);
        location.description = context.getString(R.string.install_location_desc,
                "[External SD]/multiboot/extsd-slot-" + extsdSlotId);
        return location;
    }

    public static String getDataSlotRomId(String dataSlotId) {
        return PREFIX_DATA_SLOT + dataSlotId;
    }

    public static String getExtsdSlotRomId(String extsdSlotId) {
        return PREFIX_EXTSD_SLOT + extsdSlotId;
    }

    public static boolean isDataSlotRomId(String romId) {
        return romId.startsWith(PREFIX_DATA_SLOT);
    }

    public static boolean isExtsdSlotRomId(String romId) {
        return romId.startsWith(PREFIX_EXTSD_SLOT);
    }

    public static String getDataSlotIdFromRomId(String romId) {
        if (isDataSlotRomId(romId)) {
            return romId.substring(PREFIX_DATA_SLOT.length());
        } else {
            return null;
        }
    }

    public static String getExtsdSlotIdFromRomId(String romId) {
        if (isExtsdSlotRomId(romId)) {
            return romId.substring(PREFIX_EXTSD_SLOT.length());
        } else {
            return null;
        }
    }
}
